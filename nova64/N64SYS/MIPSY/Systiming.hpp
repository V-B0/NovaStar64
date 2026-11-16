/**
 * @file Systiming.hpp
 * @brief N64 system timer management: VI, Compare, and event scheduling.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <chrono>
#include <vector>
#include <memory>
#include <string>
#include <limits>
#include <functional>
#include "Register.hpp"
#include "../Types.hpp"

class CN64System;
class CRegisters;
class RomMemoryHandler;
class AudioInterfaceHandler;

namespace Nova64 {
class StateArchive;
}

namespace N64System {

enum class TimerType : uint8_t {
    Unknown = 0,
    Compare,          // CPU compare timer
    SoftReset,        // System reset timer
    ViTimer,          // Video interface timer
    AiTimerInterrupt, // Audio interrupt timer
    AiTimerBusy,      // Audio busy timer
    AiTimerDMA,       // Audio DMA timer
    SiTimer,          // Serial interface timer
    PiTimer,          // Peripheral interface timer
    RspTimer,         // RSP timer
    RSPTimerDlist,    // RSP display list timer
    DDPiTimer,        // Disk drive PI timer
    DDSeekTimer,      // Disk drive seek timer
    DDMotorTimer,     // Disk drive motor timer
    RomWriteDecay,    // ROM write decay timer
    MaxTimers         // Total number of timer types
};

struct TimerDetails {
    bool active;                    // Whether the timer is active
    uint64_t cycles_to_fire;        // Cycles until timer fires
    uint64_t period;                // Timer period in cycles
    std::function<void()> callback; // Callback when timer fires
    std::string name;               // Timer name for debugging

    TimerDetails() : active(false), cycles_to_fire(0), period(0), name("Unknown") {}
};

// High-resolution timer for real-time synchronization
class HighResolutionTimer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    HighResolutionTimer();

    void reset();

    static double cycles_to_seconds(uint64_t cycles, uint32_t clock_frequency);
    static uint64_t seconds_to_cycles(double seconds, uint32_t clock_frequency);

private:
    TimePoint start_time;
};

class SystemTimer {
public:
    SystemTimer(CN64System& system);
    ~SystemTimer();

    SystemTimer(const SystemTimer&) = delete;
    SystemTimer& operator=(const SystemTimer&) = delete;

    SystemTimer(SystemTimer&&) noexcept = default;
    SystemTimer& operator=(SystemTimer&&) = delete;

    // Timer management
    void set_timer(TimerType type, uint32_t cycles, bool relative = true);
    void set_timer_periodic(TimerType type, uint32_t period, const std::function<void()>& callback = nullptr);
    uint32_t get_timer(TimerType type) const;

    // System operations
    void update_timers(uint32_t cycles_executed);
    void reset();

    // Legacy compatibility wrappers (used by interpreter)
    void UpdateTimers();
    void TimerDone();

    // CPU compare timer (special handling)
    void update_compare_timer();

    // Timer information
    TimerType get_current_timer() const { return current_timer; }
    uint64_t get_total_cycles() const { return total_cycles; }
    uint32_t get_next_timer_cycles() const;

    // Frequency constants (N64 runs at 93.75 MHz)
    static constexpr uint32_t CPU_CLOCK_FREQUENCY = 93750000; // 93.75 MHz
    static constexpr uint32_t AI_CLOCK_FREQUENCY = 46875000;  // 46.875 MHz
    static constexpr uint32_t VI_CLOCK_FREQUENCY = 48681812;  // ~48.68 MHz

    /// Save states and rewind (see StateArchive.hpp). Callbacks are wiring, not state.
    void SerializeState(Nova64::StateArchive& ar);

    // Callback registration
    void register_callback(TimerType type, const std::function<void()>& callback);
    void unregister_callback(TimerType type);

    // Performance monitoring
    struct PerformanceStats {
        uint64_t timer_firings[static_cast<size_t>(TimerType::MaxTimers)] = {0};
        uint64_t total_firings = 0;
        double average_period[static_cast<size_t>(TimerType::MaxTimers)] = {0.0};

        void reset();
        std::string to_string() const;
    };

    PerformanceStats get_performance_stats() const { return performance_stats; }

    // Debugging
    std::string timer_status_to_string() const;
    std::string get_timer_name(TimerType type) const;

    /// Return the m_NextTimer snapshot taken at the last UpdateTimers() call.
    uint32_t get_last_update_cycles() const { return last_update_cycles; }

private:
    // Internal helper methods
    void fire_timer(TimerType type);
    void update_next_timer();
    void adjust_all_timers(int32_t adjustment);

    // Timer data
    std::array<TimerDetails, static_cast<size_t>(TimerType::MaxTimers)> timers;
    TimerType current_timer = TimerType::Unknown;
    uint64_t total_cycles = 0;
    uint32_t last_update_cycles = 0;

    // Performance tracking
    PerformanceStats performance_stats;

    // System references
    CN64System& system;
    CRegisters* registers = nullptr;
    RomMemoryHandler* rom_memory = nullptr;
    AudioInterfaceHandler* audio_interface = nullptr;

    // Real-time synchronization
    HighResolutionTimer real_time_timer;
    uint64_t last_real_time_sync = 0;
    static constexpr uint64_t REAL_TIME_SYNC_INTERVAL = 44100; // Sync every audio sample period

    // Timer helper class for individual timer management
    class TimerInstance {
    public:
        TimerInstance(TimerType type, SystemTimer& parent);

        void set(uint32_t cycles, bool relative);
        void set_periodic(uint32_t period, const std::function<void()>& callback);
        void stop();
        void fire();
        void update(uint32_t elapsed_cycles);

        bool is_active() const { return details.active; }
        uint64_t get_cycles_remaining() const { return details.cycles_to_fire; }
        const TimerDetails& get_details() const { return details; }

    private:
        TimerType type;
        TimerDetails details;
        SystemTimer& parent;
    };
};

namespace TimerUtils {
constexpr const char* timer_type_to_string(TimerType type) {
    switch (type) {
    case TimerType::Compare:          return "Compare";
    case TimerType::SoftReset:        return "SoftReset";
    case TimerType::ViTimer:          return "ViTimer";
    case TimerType::AiTimerInterrupt: return "AiTimerInterrupt";
    case TimerType::AiTimerBusy:      return "AiTimerBusy";
    case TimerType::AiTimerDMA:       return "AiTimerDMA";
    case TimerType::SiTimer:          return "SiTimer";
    case TimerType::PiTimer:          return "PiTimer";
    case TimerType::RspTimer:         return "RspTimer";
    case TimerType::RSPTimerDlist:    return "RSPTimerDlist";
    case TimerType::DDPiTimer:        return "DDPiTimer";
    case TimerType::DDSeekTimer:      return "DDSeekTimer";
    case TimerType::DDMotorTimer:     return "DDMotorTimer";
    case TimerType::RomWriteDecay:    return "RomWriteDecay";
    default:                          return "Unknown";
    }
}

constexpr uint32_t get_timer_frequency(TimerType type) {
    switch (type) {
    case TimerType::ViTimer:          return SystemTimer::VI_CLOCK_FREQUENCY;
    case TimerType::AiTimerInterrupt:
    case TimerType::AiTimerBusy:
    case TimerType::AiTimerDMA:       return SystemTimer::AI_CLOCK_FREQUENCY;
    default:                          return SystemTimer::CPU_CLOCK_FREQUENCY;
    }
}

constexpr uint32_t calculate_timer_value(uint32_t frequency, double seconds) {
    return static_cast<uint32_t>(static_cast<double>(frequency) * seconds);
}

constexpr double calculate_seconds(uint32_t cycles, uint32_t frequency) {
    return static_cast<double>(cycles) / static_cast<double>(frequency);
}
} // namespace TimerUtils

} // namespace N64System

namespace GlobalTimers {

N64System::SystemTimer& get_main_timer();

std::unique_ptr<N64System::SystemTimer> create_timer(::CN64System& system);

uint64_t get_global_cycles();
double get_global_time_seconds();
} // namespace GlobalTimers
