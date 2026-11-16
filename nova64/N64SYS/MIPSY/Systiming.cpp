/**
 * @file Systiming.cpp
 * @brief N64 system timer implementation: timer update, expiry, and scheduling.
 */
#include "Systiming.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "../StateArchive.hpp"
#include "../../common/log.hpp"
#include "../MainSYS.hpp"
#include "Vmem.hpp"
#include "Register.hpp"
#include <thread>
#include <cstdio>

namespace N64System {

// --- HighResolutionTimer Implementation ---

HighResolutionTimer::HighResolutionTimer() { reset(); }

void HighResolutionTimer::reset() { start_time = Clock::now(); }

HighResolutionTimer::Duration HighResolutionTimer::elapsed() const { return Clock::now() - start_time; }

// --- SystemTimer Implementation ---

SystemTimer::SystemTimer(CN64System& system) : system(system) {}

SystemTimer::~SystemTimer() {}

void SystemTimer::set_timer(TimerType type, uint32_t cycles, bool relative) {
    if (static_cast<size_t>(type) >= static_cast<size_t>(TimerType::MaxTimers)) return;

    // Flush accumulated cycles before modifying the timer array.
    // Without this, update_next_timer() resets last_update_cycles and any
    // cycles elapsed since the last UpdateTimers/TimerDone are lost — COUNT
    // doesn't advance for those cycles and other timers aren't decremented.
    // This matches Project64's ChangeTimer which calls TimerCorrection first.
    UpdateTimers();

    auto& timer = timers[static_cast<size_t>(type)];
    timer.active = true;
    timer.period = 0;
    if (relative) {
        timer.cycles_to_fire = cycles;
    } else {
        timer.cycles_to_fire = cycles;
    }

    update_next_timer();
}

void SystemTimer::set_timer_periodic(TimerType type, uint32_t period, const std::function<void()>& callback) {
    if (static_cast<size_t>(type) >= static_cast<size_t>(TimerType::MaxTimers)) return;

    auto& timer = timers[static_cast<size_t>(type)];
    timer.active = true;
    timer.period = period;
    timer.cycles_to_fire = period;

    if (callback) {
        timer.callback = callback;
    }

    update_next_timer();
}

uint32_t SystemTimer::get_timer(TimerType type) const {
    if (static_cast<size_t>(type) >= static_cast<size_t>(TimerType::MaxTimers)) return 0;
    return static_cast<uint32_t>(timers[static_cast<size_t>(type)].cycles_to_fire);
}

void SystemTimer::update_timers(uint32_t cycles_executed) {

    if (!registers) registers = system.GetRegisters();

    total_cycles += cycles_executed;

    for (size_t i = 0; i < static_cast<size_t>(TimerType::MaxTimers); ++i) {
        auto& timer = timers[i];
        if (!timer.active) continue;

        if (cycles_executed >= timer.cycles_to_fire) {

            if (timer.period > 0) {

                uint64_t overshoot = cycles_executed - timer.cycles_to_fire;
                timer.cycles_to_fire = timer.period > overshoot ? timer.period - overshoot : 0;
            } else {

                timer.active = false;
                timer.cycles_to_fire = 0;
            }

            if (timer.callback) {
                timer.callback();
            } else {
                fire_timer(static_cast<TimerType>(i));
            }

            performance_stats.timer_firings[i]++;
            performance_stats.total_firings++;
        } else {
            timer.cycles_to_fire -= cycles_executed;
        }
    }

    // update_compare_timer() is deliberately not called here: Compare is only rescheduled when
    // CP0 Compare is written (MTC0). Calling it on every update would reschedule Compare at
    // distance 0 and fire it again and again.
}

void SystemTimer::fire_timer(TimerType type) {
    if (!registers) registers = system.GetRegisters();

    switch (type) {
    case TimerType::Compare:
        // CP0 Compare interrupt: Set IP7 (bit 7 of PendingInterrupts = bit 15 of Cause)
        if (registers) {
            registers->CAUSE_REGISTER.PendingInterrupts |= COP0Flags::CAUSE_IP7;
            registers->CheckInterrupts();
        }
        break;
    case TimerType::ViTimer:
        // VI frame timer: the VI handler runs the frame update, then the screen is refreshed.
        {
            static int viCount = 0;
            viCount++;
            auto* vi = system.GetMemory() ? system.GetMemory()->VideoInterface() : nullptr;
            if (vi) {
                vi->on_frame_timer();
            } else {
                if (viCount <= 20) {
                    Log::warning("[TIMER] VI timer fired but no VI handler available");
                }
            }
        }
        system.RefreshScreen();
        break;
    case TimerType::AiTimerInterrupt:
        if (registers) {
            registers->m_Mips_Interface[2] |= MIFlags::INTR_AI; // MI_INTR_REG |= AI bit
            registers->CheckInterrupts();
        }
        break;
    case TimerType::PiTimer:
        // Peripheral interface DMA complete — call CompleteDMA on the actual PI handler
        // so it updates PI_STATUS_REG properly (clears DMA_BUSY, sets INTERRUPT, etc.)
        {
            auto* mem = system.GetMemory();
            if (mem && mem->PeripheralInterface()) {
                mem->PeripheralInterface()->CompleteDMA();
            } else if (registers) {
                // Fallback: set the interrupt directly in the registers.
                registers->m_Mips_Interface[2] |= MIFlags::INTR_PI;
                registers->m_Peripheral_Interface[3] = 0;
                registers->CheckInterrupts();
            }
        }
        break;
    case TimerType::SiTimer:
        // Serial interface DMA complete
        if (registers) {
            registers->m_Mips_Interface[2] |= MIFlags::INTR_SI;          // MI_INTR_REG |= SI bit
            registers->m_SerialInterface[static_cast<int>(3)] |= 0x1000; // SI_STATUS interrupt flag
            registers->CheckInterrupts();
        }
        break;
    case TimerType::RspTimer:
        // Fires at the block boundary after ClearHalt. The game's CLR_DP_INTR cleanup (which
        // clears old DP interrupts) runs in the same block and would wipe the DP bit that
        // ClearHalt set, so the bit is set again here.
        if (registers) {
            if (registers->m_DpFreshFromHLE) {
                registers->m_GfxIntrReg |= MIFlags::INTR_DP;
                registers->m_Mips_Interface[2] |= MIFlags::INTR_DP;
                registers->m_DpFreshFromHLE = false;
            }
            registers->CheckInterrupts();
        }
        break;
    case TimerType::AiTimerDMA:
    case TimerType::AiTimerBusy:
        // Audio DMA completion — call DMAComplete on the audio interface handler
        // so it triggers MI_INTR_AI and processes the audio FIFO.
        {
            auto* mem = system.GetMemory();
            if (mem && mem->AudioInterface()) {
                mem->AudioInterface()->DMAComplete();
            } else if (registers) {
                // Fallback: direct interrupt
                registers->m_Mips_Interface[2] |= MIFlags::INTR_AI;
                registers->CheckInterrupts();
            }
        }
        break;
    default: break;
    }
}

void SystemTimer::update_compare_timer() {
    if (!registers) registers = system.GetRegisters();
    if (!registers) return;

    // The CP0 Count register increments at half CPU speed (once per 2 CPU cycles).
    // In the interpreter, NextTimer counts down by CountPerOp per executed instruction.
    // We need to schedule the Compare timer for when Count will equal Compare.

    uint32_t count = static_cast<uint32_t>(registers->COUNT_REGISTER);
    uint32_t compare = static_cast<uint32_t>(registers->COMPARE_REGISTER);

    // Calculate cycles until Count reaches Compare (accounting for wrap-around)
    uint32_t distance;
    if (compare >= count) {
        distance = compare - count;
    } else {
        // Count needs to wrap around 32-bit space
        distance = (0xFFFFFFFF - count) + compare + 1;
    }

    // Timer ticks are in Count-tick units (like P64), so distance IS the timer period.
    uint32_t cycles = distance;
    if (cycles == 0) cycles = 1; // Prevent immediate re-fire

    set_timer(TimerType::Compare, cycles, false);
}

void SystemTimer::update_next_timer() {
    // Find the active timer with the fewest cycles remaining
    // and set system.m_NextTimer to that value
    int64_t minCycles = std::numeric_limits<int64_t>::max();
    TimerType nextType = TimerType::Unknown;

    for (size_t i = 0; i < static_cast<size_t>(TimerType::MaxTimers); ++i) {
        auto& timer = timers[i];
        if (!timer.active) continue;

        if (static_cast<int64_t>(timer.cycles_to_fire) < minCycles) {
            minCycles = static_cast<int64_t>(timer.cycles_to_fire);
            nextType = static_cast<TimerType>(i);
        }
    }

    current_timer = nextType;
    if (nextType != TimerType::Unknown) {
        system.m_NextTimer = static_cast<int32_t>((minCycles > 0x7FFFFFFF) ? 0x7FFFFFFF : minCycles);
    } else {
        // No active timers - set a large default
        system.m_NextTimer = 0x7FFFFFFF;
    }
    // Sync last_update_cycles so UpdateTimers/TimerDone can compute elapsed correctly
    last_update_cycles = static_cast<uint32_t>(system.m_NextTimer);
}

void SystemTimer::PerformanceStats::reset() {
    for (auto& f : timer_firings) f = 0;
    total_firings = 0;
}

void SystemTimer::UpdateTimers() {
    if (!registers) registers = system.GetRegisters();

    // Calculate elapsed cycles since last update
    // m_NextTimer counts down by CountPerOp per instruction.
    // We compute how many cycles have elapsed and update timers accordingly.
    int32_t nextTimer = system.m_NextTimer;
    int32_t elapsed = static_cast<int32_t>(last_update_cycles) - nextTimer;
    if (elapsed > 0) {
        // Update CP0 Count register: timer ticks are in Count-tick units (like P64),
        // so elapsed IS the Count increment — no division needed.
        if (registers) {
            registers->COUNT_REGISTER += elapsed;
        }
        update_timers(static_cast<uint32_t>(elapsed));
    }
    last_update_cycles = static_cast<uint32_t>(nextTimer);
}

void SystemTimer::TimerDone() {
    if (!registers) registers = system.GetRegisters();

    // m_NextTimer has gone negative: at least one timer has expired.
    // Use int32_t arithmetic (matching UpdateTimers) to correctly handle
    // the case where last_update_cycles and m_NextTimer are both 0xFFFFFFFE/-2
    // after InPermLoop's second UpdateTimers() call.
    int32_t elapsed32 = static_cast<int32_t>(last_update_cycles) - system.m_NextTimer;

    if (elapsed32 > 0) {
        uint32_t elapsed = static_cast<uint32_t>(elapsed32);

        // Update CP0 Count register: timer ticks = Count ticks (like P64)
        if (registers) {
            registers->COUNT_REGISTER += elapsed;
        }

        update_timers(elapsed);
    }

    update_next_timer();
}

void SystemTimer::reset() {

    for (auto& timer : timers) {
        timer.active = false;
        timer.cycles_to_fire = 0;
        timer.period = 0;
        timer.callback = nullptr;
    }
    current_timer = TimerType::Unknown;
    total_cycles = 0;
    last_update_cycles = 0;
    last_real_time_sync = 0;
    real_time_timer.reset();
    performance_stats.reset();
}

void SystemTimer::SerializeState(Nova64::StateArchive& ar) {
    ar.Section("TIME");
    for (auto& timer : timers) {
        ar.Value(timer.active);
        ar.Value(timer.cycles_to_fire);
        ar.Value(timer.period);
    }
    ar.Value(current_timer);
    ar.Value(total_cycles);
    ar.Value(last_update_cycles);
}

} // namespace N64System
