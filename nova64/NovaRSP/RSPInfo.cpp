/**
 * @file RSPInfo.cpp
 * @brief RSP Info / Initialization Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "RSPInfo.hpp"
#include "CPU/RspMemory.hpp"
#include "CPU/RspSystem.hpp"
#include "CPU/RSPCpu.hpp"
#include "CPU/RSPInterpreterOps.hpp"
#include "RSPDebugger.hpp"
#include "Settings/RspSettings.hpp"
#include "HLE/hle.hpp"
#include "../common/log.hpp"
#include <cstring>
#include <memory>

namespace Nova64::RSP {

#ifdef NOVA_RSP_RECOMPILER
// Module-level recompiler memory (code buffers, jump tables)
static RecompilerMemory s_RecompMem;
#endif

// Persistent HLE context — survives across DoRspCycles calls for audio continuity
static std::unique_ptr<HLE::CHle> s_HleContext;

// ──────────────────────────────────────────────
// Initialize RSP with hardware register pointers
// Called by the main system after N64 memory is
// allocated and SP interface is set up
// ──────────────────────────────────────────────

void InitializeRSP(RSP_INFO& rspInfo)
{
    // Bind system to hardware registers
    RSPSystem.Reset(rspInfo);

    // Refresh settings from current config
    CRSPSettings::RefreshSettings();

    // Build RSP interpreter dispatch tables
    Build_RSP();
}

// ──────────────────────────────────────────────
// Called when RSP plugin is loaded (one-time)
// Sets up global state and default settings
// ──────────────────────────────────────────────

void RspPluginLoaded()
{
    CRSPSettings::InitializeRspSetting();
    g_RSPDebugger = nullptr;
}

// ──────────────────────────────────────────────
// Called when a ROM is opened
// Allocates recompiler memory and prepares per-game state
// ──────────────────────────────────────────────

void RspRomOpened()
{
    CRSPSettings::SetRomOpen(true);
    CRSPSettings::RefreshSettings();

    // Apply per-game RSP overrides based on ROM header CRC
    // N64 ROM header: CRC1 @ offset 0x10, CRC2 @ offset 0x14 (big-endian)
    const uint8_t* header = RSPSystem.Header();
    if (header != nullptr)
    {
        uint32_t crc1 = (static_cast<uint32_t>(header[0x10]) << 24)
                      | (static_cast<uint32_t>(header[0x11]) << 16)
                      | (static_cast<uint32_t>(header[0x12]) << 8)
                      |  static_cast<uint32_t>(header[0x13]);
        uint32_t crc2 = (static_cast<uint32_t>(header[0x14]) << 24)
                      | (static_cast<uint32_t>(header[0x15]) << 16)
                      | (static_cast<uint32_t>(header[0x16]) << 8)
                      |  static_cast<uint32_t>(header[0x17]);
        CRSPSettings::ApplyPerGameOverrides(crc1, crc2);
    }

    // Create persistent HLE context (initializes audio output device, ucode state)
    s_HleContext = std::make_unique<HLE::CHle>(RSPSystem);

#ifdef NOVA_RSP_RECOMPILER
    // Allocate recompiler code buffers if using recompiler
    if (CRSPSettings::CPUMethod() == RSPCpuMethod::RecompilerTasks)
    {
        AllocateRSPMemory(s_RecompMem);
    }
#endif
}

// ──────────────────────────────────────────────
// Called when a ROM is closed
// Frees per-game RSP resources
// ──────────────────────────────────────────────

void RspRomClosed()
{
    CRSPSettings::SetRomOpen(false);

    // Destroy HLE context (shuts down WASAPI audio, flushes buffers)
    s_HleContext.reset();

#ifdef NOVA_RSP_RECOMPILER
    // Free recompiler buffers
    FreeRSPMemory(s_RecompMem);
#endif
}

// ──────────────────────────────────────────────
// Free all RSP resources (module unload)
// ──────────────────────────────────────────────

void FreeRSP()
{
    s_HleContext.reset();
#ifdef NOVA_RSP_RECOMPILER
    FreeRSPMemory(s_RecompMem);
#endif
    g_RSPDebugger = nullptr;
}

// ──────────────────────────────────────────────
// DoRspCycles: main RSP execution entry point
// Called when the RSP halt bit is cleared.
// Tries HLE dispatch first; falls back to LLE interpreter.
// ──────────────────────────────────────────────

uint32_t DoRspCycles(uint32_t cycles)
{
    const auto method = CRSPSettings::CPUMethod();
    const uint8_t* dmem = RSPSystem.DMEM();
    const uint32_t taskType = dmem ? *reinterpret_cast<const uint32_t*>(dmem + 0xFC0) : 0;
    // Throttled diagnostic: first 10 calls, then 1-in-1024. Quiet by default.
    static uint32_t s_trace = 0;
    ++s_trace;
    const bool trace = (s_trace <= 10) || ((s_trace & 0x3FF) == 0);
    if (trace) {
        Log::infof("RSPTRACE DoRspCycles cycles=0x%X method=%u task=%u graphicsHle=%d",
            cycles, static_cast<uint32_t>(method), taskType,
            CRSPSettings::IsGraphicsHle() ? 1 : 0);
    }

    // Try HLE dispatch first (fast path for known microcode tasks)
    if (method != RSPCpuMethod::Interpreter)
    {
        const bool hleHandled = s_HleContext && s_HleContext->Execute();
        if (hleHandled)
            return cycles;
    }

#ifdef NOVA_RSP_RECOMPILER
    // Recompiler path (task-based JIT)
    // NOTE: under MLE/LLE the recompiler buffer is intentionally not allocated
    // by RspRomOpened (because CPUMethod was still HLE at ROM-open time, and
    // the video plugin flipped it to RecompilerTasks afterwards). That's by
    // design — the RSP recompiler hangs on F3D microcode (Phase 2 work).
    // The call below harmlessly returns false when Code==nullptr, causing the
    // interpreter fallback to handle graphics tasks instead.
    if (method == RSPCpuMethod::RecompilerTasks)
    {
        const bool recompiled = RSPSystem.RunRecompiledTask(s_RecompMem);
        if (recompiled)
            return cycles;
    }
#endif

    // Interpreter fallback — run until RSP halts (BREAK instruction).
    // Under LLE this is the path that executes graphics microcode; it currently
    // does not advance F3D far enough to emit RDP commands (Phase 2 work).
    RSPSystem.ExecuteOps(cycles, static_cast<uint32_t>(-1));
    return 0;
}

} // namespace Nova64::RSP
