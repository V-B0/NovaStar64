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

// Persistent HLE context; survives across DoRspCycles calls for audio continuity.
static std::unique_ptr<HLE::CHle> s_HleContext;

void InitializeRSP(RSP_INFO& rspInfo) {
    RSPSystem.Reset(rspInfo);
    CRSPSettings::RefreshSettings();
    Build_RSP();
}

void RspPluginLoaded() {
    CRSPSettings::InitializeRspSetting();
    g_RSPDebugger = nullptr;
}

void RspRomOpened() {
    CRSPSettings::SetRomOpen(true);
    CRSPSettings::RefreshSettings();

    // Apply per-game RSP overrides based on ROM header CRC
    // N64 ROM header: CRC1 @ offset 0x10, CRC2 @ offset 0x14 (big-endian)
    const uint8_t* header = RSPSystem.Header();
    if (header != nullptr) {
        uint32_t crc1 = (static_cast<uint32_t>(header[0x10]) << 24) | (static_cast<uint32_t>(header[0x11]) << 16) |
                        (static_cast<uint32_t>(header[0x12]) << 8) | static_cast<uint32_t>(header[0x13]);
        uint32_t crc2 = (static_cast<uint32_t>(header[0x14]) << 24) | (static_cast<uint32_t>(header[0x15]) << 16) |
                        (static_cast<uint32_t>(header[0x16]) << 8) | static_cast<uint32_t>(header[0x17]);
        CRSPSettings::ApplyPerGameOverrides(crc1, crc2);
    }

    // Create persistent HLE context (initializes audio output device, ucode state)
    s_HleContext = std::make_unique<HLE::CHle>(RSPSystem);
}

void RspRomClosed() {
    CRSPSettings::SetRomOpen(false);

    // Destroy HLE context (shuts down audio output, flushes buffers)
    s_HleContext.reset();
}

void FreeRSP() {
    s_HleContext.reset();
    g_RSPDebugger = nullptr;
}

uint32_t DoRspCycles(uint32_t cycles) {
    const auto method = CRSPSettings::CPUMethod();
    const uint8_t* dmem = RSPSystem.DMEM();
    const uint32_t taskType = dmem ? *reinterpret_cast<const uint32_t*>(dmem + 0xFC0) : 0;
    // Throttled diagnostic: logs the first 10 calls, then 1 in 1024.
    static uint32_t s_trace = 0;
    ++s_trace;
    const bool trace = (s_trace <= 10) || ((s_trace & 0x3FF) == 0);
    if (trace) {
        Log::infof("RSPTRACE DoRspCycles cycles=0x%X method=%u task=%u graphicsHle=%d", cycles,
                   static_cast<uint32_t>(method), taskType, CRSPSettings::IsGraphicsHle() ? 1 : 0);
    }

    // Try HLE dispatch first (fast path for known microcode tasks)
    if (method != RSPCpuMethod::Interpreter) {
        const bool hleHandled = s_HleContext && s_HleContext->Execute();
        if (hleHandled) return cycles;
    }

    // Run the task on the RSP engine until the RSP halts (BREAK). Under LLE this
    // is what executes the graphics microcode.
    RSPSystem.SetEngine(CRSPSettings::Engine());
    RSPSystem.Run(cycles);
    return 0;
}

} // namespace Nova64::RSP
