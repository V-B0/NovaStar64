/**
 * @file RSPDebugger.hpp
 * @brief RSP Debugger Interface.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// RSP Debugger Abstract Interface
// Implement this to hook into RSP execution events
// ──────────────────────────────────────────────
class IRSPDebugger {
public:
    virtual ~IRSPDebugger() = default;

    virtual void ResetTimerList() = 0;
    virtual void StartingCPU() = 0;
    virtual void RspCyclesStart() = 0;
    virtual void RspCyclesStop() = 0;
    virtual void BeforeExecuteOp() = 0;
    virtual void UnknownOpcode() = 0;
    virtual void RDP_LogMF0(uint32_t pc, uint32_t reg) = 0;
    virtual void SyncError(const char* msg) = 0;
};

// Global debugger instance (nullptr when debugging disabled)
extern IRSPDebugger* g_RSPDebugger;

} // namespace Nova64::RSP
