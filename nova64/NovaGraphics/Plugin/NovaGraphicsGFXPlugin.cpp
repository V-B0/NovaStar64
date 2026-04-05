/**
 * @file NovaGraphicsGFXPlugin.cpp
 * @brief Adapter bridging GFXPlugin → VideoPlugin + MicrocodeDispatcher + IRenderer
 */

#include "NovaGraphicsGFXPlugin.hpp"
#include <cstdio>
#include "../Common/Logger.hpp"
#include "../Renderer/Renderer.hpp"

// System-level logging that goes to the main log (not NovaGraphics logger)
#include "../../common/log.hpp"

// Emulator core headers needed for SystemInfo wiring
#include "../../N64SYS/MainSYS.hpp"
#include "../../N64SYS/MIPSY/Register.hpp"
#include "../../N64SYS/MIPSY/Vmem.hpp"
#include "../../N64SYS/N64Rom.hpp"
#include "../../N64SYS/Memoryhandler/VideoInterfaceHandler.hpp"
#include "../../N64SYS/Memoryhandler/DisplayCtrlReg.hpp"
#include "../../N64SYS/Memoryhandler/MIPSYInterfaceHandler.hpp"
#include "../Debug/ImGuiDebugUI.hpp"

#include <cstring>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════════

NovaGraphicsGFXPlugin::NovaGraphicsGFXPlugin()
    : m_lastStatTime(std::chrono::steady_clock::now())
{
}

NovaGraphicsGFXPlugin::~NovaGraphicsGFXPlugin()
{
    if (m_initialized)
        Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════
//  PluginBase interface
// ═══════════════════════════════════════════════════════════════════════

::Plugins::PluginInfo NovaGraphicsGFXPlugin::GetPluginInfo() const
{
    ::Plugins::PluginInfo info{};
    info.Type                = ::Plugins::PluginType::Graphics;
    info.Version             = 0x0104; // 1.4
    info.Name                = "NovaGraphics";
    info.Author              = "SuperNova64 Team";
    info.Description         = "OpenGL 4.6 N64 video plugin";
    info.SupportsThreading   = false;
    info.SupportsStateSync   = false;
    info.RequiresAudioSync   = false;
    info.SupportsConfigDialog = false;
    return info;
}

bool NovaGraphicsGFXPlugin::Initialize(CN64System* system)
{
    m_System = system; // PluginBase member

    SystemInfo si{};

    if (system) {
        // ── Memory pointers ──────────────────────────────────────
        if (auto* mem = system->GetMemory()) {
            si.rdram    = mem->Rdram();
            si.rdramSize= mem->RdramSize();
            si.dmem     = mem->Dmem();
            si.imem     = mem->Imem();

            // ── VI register pointers ─────────────────────────────
            if (auto* vi = mem->VideoInterface()) {
                si.viStatusReg  = vi->GetRegisterPtr(VI::Register::STATUS);
                si.viOriginReg  = vi->GetRegisterPtr(VI::Register::ORIGIN);
                si.viWidthReg   = vi->GetRegisterPtr(VI::Register::WIDTH);
                si.viIntrReg    = vi->GetRegisterPtr(VI::Register::INTR);
                si.viVCurrentReg= vi->GetRegisterPtr(VI::Register::CURRENT);
                si.viTimingReg  = vi->GetRegisterPtr(VI::Register::BURST);
                si.viVSyncReg   = vi->GetRegisterPtr(VI::Register::V_SYNC);
                si.viHSyncReg   = vi->GetRegisterPtr(VI::Register::H_SYNC);
                si.viXScaleReg  = vi->GetRegisterPtr(VI::Register::X_SCALE);
                si.viYScaleReg  = vi->GetRegisterPtr(VI::Register::Y_SCALE);
            }

            // ── DPC register pointers ────────────────────────────
            if (auto* dpc = mem->DPCommandRegisters()) {
                si.dpcStartReg  = dpc->GetRegisterPtr(DPCRegisters::DPC_START);
                si.dpcEndReg    = dpc->GetRegisterPtr(DPCRegisters::DPC_END);
                si.dpcCurrentReg= dpc->GetRegisterPtr(DPCRegisters::DPC_CURRENT);
                si.dpcStatusReg = dpc->GetRegisterPtr(DPCRegisters::DPC_STATUS);
            }

            // ── MI interrupt register ────────────────────────────
            if (auto* mi = mem->MIPSInterface()) {
                si.miIntrReg = mi->MI_INTR_REG;
            }
        }

        // ── ROM information ──────────────────────────────────────
        if (auto* rom = system->GetRom(); rom && rom->IsLoaded()) {
            const auto& img = rom->GetImage();
            si.romName    = img.GetRomName();
            si.romCountry = static_cast<uint8_t>(img.GetCountry());
            si.isPAL      = img.IsPal();

            // Read CRC1/CRC2 from ROM header (big-endian)
            if (img.Size() >= N64::Rom::RomHeader::OFFSET_CRC2 + 4) {
                const auto* hdr = img.Data();
                auto read32be = [](const uint8_t* p) -> uint32_t {
                    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
                };
                si.romCRC1 = read32be(hdr + N64::Rom::RomHeader::OFFSET_CRC1);
                si.romCRC2 = read32be(hdr + N64::Rom::RomHeader::OFFSET_CRC2);
            }

            // 2-byte cart ID at offset 0x3C
            if (img.Size() >= N64::Rom::RomHeader::OFFSET_CART_ID + 2) {
                const auto* hdr = img.Data();
                si.romGameId.assign(
                    reinterpret_cast<const char*>(hdr + N64::Rom::RomHeader::OFFSET_CART_ID), 2);
            }
        }
    }

    PluginCallbacks cb{};

    // Wire the SyncFull → DP interrupt callback.  When the RDP encounters a
    // SyncFull command, this fires to set MI_INTR_DP — the architecturally
    // correct point where real N64 hardware raises the DP interrupt.  If we
    // are inside ClearHalt's DoRspCycles (RSP interpreter path), the bit is
    // deferred so ClearHalt can deliver it together with MI_INTR_SP at the
    // next instruction boundary.
    if (system && system->GetRegisters()) {
        auto* regs = system->GetRegisters();
        cb.checkInterrupts = [regs]() {
            if (regs->m_defer_hle_interrupts) {
                regs->m_deferred_mi_intr |= 0x20;  // MI_INTR_DP
            } else {
                regs->m_GfxIntrReg |= 0x20;
                regs->m_Mips_Interface[2] |= 0x20;
                regs->CheckInterrupts();
            }
        };
    }

    // Use the external window from the system instead of creating a second one
    if (system && system->GetWindowManager()) {
        m_videoPlugin.SetExternalWindow(system->GetWindowManager());
    }

    // Apply resolution settings from CLI/system (applied after config load in Initialize)
    m_videoPlugin.SetPendingResolution(
        CN64System::s_InternalResScale,
        CN64System::s_WindowWidth,
        CN64System::s_WindowHeight
    );

    if (!m_videoPlugin.Initialize(si, cb)) {
        LOG_ERROR("NovaGraphicsGFXPlugin: VideoPlugin init failed");
        return false;
    }

    // Give the debug UI access to the N64 system for live register/memory display
    g_imguiDebugUI.SetSystem(system);
    g_imguiDebugUI.SetVideoPlugin(&m_videoPlugin);

    m_initialized = true;
    return true;
}

void NovaGraphicsGFXPlugin::Shutdown()
{
    m_videoPlugin.Shutdown();
    m_initialized = false;
}

void NovaGraphicsGFXPlugin::Reset(bool coldReset)
{
    m_videoPlugin.Reset();
    (void)coldReset;
}

void NovaGraphicsGFXPlugin::Pause()  { LOG_INFO("NovaGraphics paused"); }
void NovaGraphicsGFXPlugin::Resume() { LOG_INFO("NovaGraphics resumed"); }

bool NovaGraphicsGFXPlugin::IsInitialized() const { return m_initialized; }

void NovaGraphicsGFXPlugin::ShowConfigDialog(void* /*hwndParent*/)
{
    m_videoPlugin.OpenConfigDialog();
}

size_t NovaGraphicsGFXPlugin::SaveState(uint8_t* data, size_t size) {
    // Video plugin state: VI registers + frame counter.
    // TMEM and RDRAM are saved by the system-level save state.
    struct VideoState {
        uint32_t magic;       // 'NVGS'
        uint32_t version;
        uint64_t frameCount;
        uint32_t viStatus;
        uint32_t viOrigin;
        uint32_t viWidth;
        uint32_t viXScale;
        uint32_t viYScale;
    };

    constexpr size_t stateSize = sizeof(VideoState);
    if (!data) return stateSize;  // Query size
    if (size < stateSize) return 0;

    VideoState state{};
    state.magic      = 0x4E564753; // "NVGS"
    state.version    = 1;
    state.frameCount = m_videoPlugin.GetFrameStats().viInterrupts;
    // VI registers are maintained by VideoPlugin
    // Store zeros here — the emulator core saves VI register state separately
    std::memcpy(data, &state, stateSize);
    return stateSize;
}

bool NovaGraphicsGFXPlugin::LoadState(const uint8_t* data, size_t size) {
    struct VideoState {
        uint32_t magic;
        uint32_t version;
        uint64_t frameCount;
        uint32_t viStatus;
        uint32_t viOrigin;
        uint32_t viWidth;
        uint32_t viXScale;
        uint32_t viYScale;
    };

    constexpr size_t stateSize = sizeof(VideoState);
    if (!data || size < stateSize) return false;

    VideoState state{};
    std::memcpy(&state, data, stateSize);

    if (state.magic != 0x4E564753 || state.version != 1)
        return false;

    // Invalidate texture cache since RDRAM contents changed
    // (system-level restore overwrites RDRAM, so cached GPU textures are stale)
    m_videoPlugin.GetRDPProcessor().reset();
    LOG_INFO("Video plugin state loaded (frame {})", state.frameCount);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  GFXPlugin interface
// ═══════════════════════════════════════════════════════════════════════

bool NovaGraphicsGFXPlugin::InitializeDisplay(void* hwnd,
                                           const ::Plugins::RenderSettings& settings)
{
    (void)settings;
    (void)hwnd;
    // The window handle is already managed via SetExternalWindow().
    // Do NOT call RomOpen here — it would overwrite the SystemInfo
    // (VI register pointers, RDRAM, etc.) that was set during Initialize().
    return true;
}

void NovaGraphicsGFXPlugin::ResizeDisplay(uint32_t width, uint32_t height)
{
    // VideoPlugin handles resize through its own WindowManager
    (void)width;
    (void)height;
}

void NovaGraphicsGFXPlugin::ProcessDList(uint32_t /*start*/, uint32_t /*end*/)
{
    if (!m_initialized) return;

    m_videoPlugin.ProcessDList();
}

void NovaGraphicsGFXPlugin::ProcessRDPCommand(uint64_t cmd)
{
    if (!m_initialized) return;

    // Forward single RDP command to VideoPlugin's RDP processor
    uint32_t w0 = static_cast<uint32_t>(cmd >> 32);
    uint32_t w1 = static_cast<uint32_t>(cmd);
    m_videoPlugin.ProcessRDPCommand(w0, w1);
}

void NovaGraphicsGFXPlugin::UpdateRDPStatus()
{
    // The RSP HLE has written RDP commands to DRAM and updated DPC registers.
    // Forward them to the VideoPlugin RDP processor.
    m_videoPlugin.ProcessRDPList();
}

void NovaGraphicsGFXPlugin::UpdateScreen()
{
    m_videoPlugin.UpdateScreen();
    m_viCount++;

    // Compute stats every second
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_lastStatTime).count();
    if (elapsed >= 1.0) {
        m_fps = static_cast<float>(m_viCount / elapsed);
        m_vis = m_fps;
        m_viCount = 0;
        m_lastStatTime = now;
    }
}

void NovaGraphicsGFXPlugin::SwapBuffers()
{
    // Swap is done inside UpdateScreen via WindowManager
}

::Plugins::FrameBufferInfo NovaGraphicsGFXPlugin::GetFrameBufferInfo() const
{
    ::Plugins::FrameBufferInfo fb{};
    fb.Width  = 320;
    fb.Height = 240;
    return fb;
}

void NovaGraphicsGFXPlugin::ReadFrameBuffer(uint8_t* dest, uint32_t addr,
                                          uint32_t width, uint32_t height,
                                          uint32_t size)
{
    if (!dest) return;

    // Read back from the GPU-rendered framebuffer into RDRAM.
    // The RDPProcessor owns the RDRAM pointer and can read from it,
    // but for GPU-rendered content we need to do a proper GPU readback.
    auto& rdp = m_videoPlugin.GetRDPProcessor();
    if (!rdp) return;

    uint32_t bytesPerPixel = (size == 2) ? 2 : 4;
    uint32_t byteCount = width * height * bytesPerPixel;
    rdp->ReadFrameBuffer(addr, byteCount, dest);
}

void NovaGraphicsGFXPlugin::WriteFrameBuffer(const uint8_t* src, uint32_t addr,
                                           uint32_t width, uint32_t height,
                                           uint32_t size)
{
    if (!src) return;

    // Upload CPU-side pixel data to the GPU framebuffer (RDRAM write-back).
    auto& rdp = m_videoPlugin.GetRDPProcessor();
    if (!rdp) return;

    uint32_t bytesPerPixel = (size == 2) ? 2 : 4;
    uint32_t byteCount = width * height * bytesPerPixel;
    rdp->WriteFrameBuffer(addr, byteCount, src);
}

bool NovaGraphicsGFXPlugin::IsFrameReady() const { return true; }

void NovaGraphicsGFXPlugin::GetStatistics(float& fps, float& vis) const
{
    fps = m_fps;
    vis = m_vis;
}

bool NovaGraphicsGFXPlugin::CaptureScreenshot(const std::string& filename)
{
    m_videoPlugin.TakeScreenshot(filename);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Factory
// ═══════════════════════════════════════════════════════════════════════

std::unique_ptr<::Plugins::GFXPlugin> CreateNovaGraphicsPlugin()
{
    return std::make_unique<NovaGraphicsGFXPlugin>();
}

} // namespace Nova64::Video
