/**
 * @file RspProfiling.hpp
 * @brief RSP Profiling.
 */
// SuperNova64 - N64 Emulator
// RSP Profiling - Modernized from Project64's RspProfiling.h
// C++20 timer infrastructure for RSP performance analysis

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
