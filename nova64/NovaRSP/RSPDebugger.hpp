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

/// Hooks into RSP execution events; implement it and assign the instance to g_RSPDebugger.
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

// Global debugger instance, set by the host application (nullptr when debugging is disabled)
extern IRSPDebugger* g_RSPDebugger;

} // namespace Nova64::RSP
