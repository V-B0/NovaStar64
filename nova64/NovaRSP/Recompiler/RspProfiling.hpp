/**
 * @file RspProfiling.hpp
 * @brief RSP Profiling.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Special timer identifiers (negative = system timers)
// ──────────────────────────────────────────────
enum SpecialTimers : int32_t {
    Timer_None         = 0,
    Timer_Compiling    = -1,
    Timer_RSP_Running  = -2,
    Timer_RDP_Running  = -3,
};

// ──────────────────────────────────────────────
// Profiling interface
// ──────────────────────────────────────────────
void     ResetTimerList();
uint32_t StartTimer(uint32_t address);
void     StopTimer();
void     GenerateTimerResults();

} // namespace Nova64::RSP::Recompiler
