/**
 * @file RSPInfo.cpp
 * @brief RSP Info / Initialization Implementation.
 */
// SuperNova64 - N64 Emulator
// RSP Info / Initialization Implementation
// Module lifecycle: init, plugin load, ROM open/close, free

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

// Module-level recompiler memory (code buffers, jump tables)
static RecompilerMemory s_RecompMem;

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

    // Allocate recompiler code buffers if using recompiler
    if (CRSPSettings::CPUMethod() == RSPCpuMethod::RecompilerTasks)
    {
        AllocateRSPMemory(s_RecompMem);
    }
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

    // Free recompiler buffers
    FreeRSPMemory(s_RecompMem);
}

// ──────────────────────────────────────────────
// Free all RSP resources (module unload)
// ──────────────────────────────────────────────

void FreeRSP()
{
    s_HleContext.reset();
    FreeRSPMemory(s_RecompMem);
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
    // Log::infof("RSPTRACE DoRspCycles enter cycles=0x%08X method=%u task=%u",
    //           cycles, static_cast<uint32_t>(method), taskType);

    // Try HLE dispatch first (fast path for known microcode tasks)
    if (method != RSPCpuMethod::Interpreter)
    {
        // Log::info("RSPTRACE HLE execute enter");
        const bool hleHandled = s_HleContext && s_HleContext->Execute();
        // Log::infof("RSPTRACE HLE execute exit handled=%d", hleHandled ? 1 : 0);
        if (hleHandled)
            return cycles;
    }

    // Recompiler path (task-based JIT)
    if (method == RSPCpuMethod::RecompilerTasks)
    {
        // Log::info("RSPTRACE RSP recompiler enter");
        const bool recompiled = RSPSystem.RunRecompiledTask(s_RecompMem);
        // Log::infof("RSPTRACE RSP recompiler exit handled=%d", recompiled ? 1 : 0);
        if (recompiled)
            return cycles;
    }

    // Interpreter fallback — run until RSP halts (BREAK instruction)
    // Pass -1 as targetPC to disable the PC breakpoint (0 would break immediately at the start)
    // Log::info("RSPTRACE interpreter fallback enter");
    RSPSystem.ExecuteOps(cycles, static_cast<uint32_t>(-1));
    // Log::info("RSPTRACE interpreter fallback exit");
    return 0;
}

} // namespace Nova64::RSP
