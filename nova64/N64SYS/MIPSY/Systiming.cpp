/**
 * @file Systiming.cpp
 * @brief N64 system timer implementation: timer update, expiry, and scheduling.
 */
#include "Systiming.hpp"
#include "../../common/Log.hpp"
#include "../MainSYS.hpp"
#include "Vmem.hpp"
#include "Register.hpp"
#include <thread>
#include <cstdio>

namespace N64System {

// --- HighResolutionTimer Implementation ---

HighResolutionTimer::HighResolutionTimer() {
    reset();
}

void HighResolutionTimer::reset() {
    start_time = Clock::now();
}

HighResolutionTimer::Duration HighResolutionTimer::elapsed() const {
    return Clock::now() - start_time;
}

double HighResolutionTimer::elapsed_seconds() const {
    auto duration = elapsed();
    return std::chrono::duration<double>(duration).count();
}

double HighResolutionTimer::elapsed_milliseconds() const {
    auto duration = elapsed();
    return std::chrono::duration<double, std::milli>(duration).count();
}

// --- SystemTimer Implementation ---

SystemTimer::SystemTimer(CN64System& system) 
    : system(system)
{
}

SystemTimer::~SystemTimer() {
}

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

void SystemTimer::stop_timer(TimerType type) {
    if (static_cast<size_t>(type) >= static_cast<size_t>(TimerType::MaxTimers)) return;
    timers[static_cast<size_t>(type)].active = false;
}

bool SystemTimer::is_timer_active(TimerType type) const {
    if (static_cast<size_t>(type) >= static_cast<size_t>(TimerType::MaxTimers)) return false;
    return timers[static_cast<size_t>(type)].active;
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
    
    // NOTE: Do NOT call update_compare_timer() here.
    // Compare timer is only recalculated when CP0 Compare is written (MTC0).
    // Calling it every update_timers() causes an infinite re-scheduling loop
    // where Compare fires at distance=0, re-schedules for 1 cycle, fires again, etc.
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
            // VI interrupt: Use proper VI_INTR/VI_CURRENT comparison.
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
            // Audio interrupt
            if (registers) {
                registers->m_Mips_Interface[2] |= MIFlags::INTR_AI;  // MI_INTR_REG |= AI bit
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
                    // Fallback: direct register manipulation (less correct)
                    registers->m_Mips_Interface[2] |= MIFlags::INTR_PI;
                    registers->m_Peripheral_Interface[3] = 0;
                    registers->CheckInterrupts();
                }
            }
            break;
        case TimerType::SiTimer:
            // Serial interface DMA complete
            if (registers) {
                registers->m_Mips_Interface[2] |= MIFlags::INTR_SI;  // MI_INTR_REG |= SI bit
                registers->m_SerialInterface[static_cast<int>(3)] |= 0x1000;  // SI_STATUS interrupt flag
                registers->CheckInterrupts();
            }
            break;
        case TimerType::RspTimer:
            // Fires at the block boundary after ClearHalt.
            // Re-set the DP bit here because the game's CLR_DP_INTR cleanup
            // (which clears OLD DP interrupts) runs within the same block and
            // wipes out the DP bit that ClearHalt set. By re-setting it here,
            // the DP interrupt survives past the cleanup code.
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
        default:
            break;
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
    if (cycles == 0) cycles = 1;  // Prevent immediate re-fire
    
    // Set (or update) the Compare timer
    set_timer(TimerType::Compare, cycles, false);
}

void SystemTimer::set_compare_timer(uint32_t value) {
    // Called when the Compare register is written via MTC0
    // Clear IP7 pending interrupt and recalculate timer
    if (!registers) registers = system.GetRegisters();
    if (registers) {
        registers->CAUSE_REGISTER.PendingInterrupts &= ~COP0Flags::CAUSE_IP7;
    }
    update_compare_timer();
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
        system.m_NextTimer = static_cast<int32_t>(
            (minCycles > 0x7FFFFFFF) ? 0x7FFFFFFF : minCycles
        );
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

void SystemTimer::reset_performance_stats() {
    performance_stats.reset();
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
        
        // Decrement all timer counters and fire expired ones
        update_timers(elapsed);
    }
    
    // Set m_NextTimer to the next shortest timer and sync last_update_cycles
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

void SystemTimer::sync_to_real_time() {
    
    uint64_t cycles_since_sync = total_cycles - last_real_time_sync;
    double expected_seconds = static_cast<double>(cycles_since_sync) / CPU_CLOCK_FREQUENCY;
    double actual_seconds = real_time_timer.elapsed_seconds();
    
    if (actual_seconds < expected_seconds) {
        
        double sleep_time = expected_seconds - actual_seconds;
        if (sleep_time > 0.0 && sleep_time < 0.1) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<int64_t>(sleep_time * 1000000.0))
            );
        }
    }
    
    
    if (actual_seconds > expected_seconds + 0.5 || cycles_since_sync > REAL_TIME_SYNC_INTERVAL * 100) {
        last_real_time_sync = total_cycles;
        real_time_timer.reset();
    }
}

uint32_t SystemTimer::get_current_count() const {
    // Calculate the current Count register value by adding elapsed cycles since last update.
    // This is read-only — it doesn't modify timer state.
    // Count increments at half the CPU clock rate (once per 2 cycles).
    int32_t nextTimer = system.m_NextTimer;
    int32_t elapsed = static_cast<int32_t>(last_update_cycles) - nextTimer;
    uint32_t count = registers ? static_cast<uint32_t>(registers->COUNT_REGISTER) : 0;
    if (elapsed > 0) {
        count += static_cast<uint32_t>(elapsed);
    }
    return count;
}

} // namespace N64System
