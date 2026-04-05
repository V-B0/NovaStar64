/**
 * @file RspProfiling.cpp
 * @brief RSP Profiling Implementation.
 */
// SuperNova64 - N64 Emulator
// RSP Profiling Implementation
// Timer infrastructure for RSP performance analysis

#include "RspProfiling.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Internal timer state
// ──────────────────────────────────────────────

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

struct TimerEntry {
    uint32_t  address{0};
    uint64_t  totalTicks{0};
    uint32_t  hitCount{0};
    TimePoint startTime;
    bool      running{false};
};

// Active timer stack
static struct {
    std::unordered_map<uint32_t, TimerEntry> entries;
    uint32_t currentTimer{0};
    bool     active{false};
} g_TimerState;

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void ResetTimerList()
{
    g_TimerState.entries.clear();
    g_TimerState.currentTimer = 0;
    g_TimerState.active = false;
}

uint32_t StartTimer(uint32_t address)
{
    // Stop current timer if one is running
    if (g_TimerState.active)
    {
        StopTimer();
    }

    auto& entry = g_TimerState.entries[address];
    entry.address = address;
    entry.startTime = Clock::now();
    entry.running = true;
    entry.hitCount++;

    g_TimerState.currentTimer = address;
    g_TimerState.active = true;

    return address;
}

void StopTimer()
{
    if (!g_TimerState.active) return;

    auto it = g_TimerState.entries.find(g_TimerState.currentTimer);
    if (it != g_TimerState.entries.end() && it->second.running)
    {
        auto elapsed = Clock::now() - it->second.startTime;
        it->second.totalTicks += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        it->second.running = false;
    }

    g_TimerState.active = false;
}

void GenerateTimerResults()
{
    if (g_TimerState.entries.empty()) return;

    // Sort by total time descending
    struct ResultEntry {
        uint32_t address;
        uint64_t totalNs;
        uint32_t hits;
    };

    std::vector<ResultEntry> results;
    results.reserve(g_TimerState.entries.size());

    uint64_t totalAllNs = 0;
    for (const auto& [addr, entry] : g_TimerState.entries)
    {
        results.push_back({addr, entry.totalTicks, entry.hitCount});
        totalAllNs += entry.totalTicks;
    }

    std::sort(results.begin(), results.end(),
              [](const ResultEntry& a, const ResultEntry& b) {
                  return a.totalNs > b.totalNs;
              });

    // Output to log file
    FILE* fp = fopen("rsp_profile.txt", "w");
    if (fp == nullptr) return;

    std::fprintf(fp, "RSP Profiling Results\n");
    std::fprintf(fp, "═══════════════════════════════════════════\n");
    std::fprintf(fp, "%-12s %-14s %-10s %-8s\n", "Address", "Time (us)", "Hits", "%%");
    std::fprintf(fp, "───────────────────────────────────────────\n");

    for (const auto& r : results)
    {
        double pct = totalAllNs > 0 ? (100.0 * r.totalNs / totalAllNs) : 0.0;
        double timeUs = r.totalNs / 1000.0;

        const char* name = "";
        if (static_cast<int32_t>(r.address) == Timer_Compiling)
            name = " [Compiling]";
        else if (static_cast<int32_t>(r.address) == Timer_RSP_Running)
            name = " [RSP Running]";
        else if (static_cast<int32_t>(r.address) == Timer_RDP_Running)
            name = " [RDP Running]";

        std::fprintf(fp, "0x%08X   %-14.1f %-10u %5.1f%%%s\n",
                     r.address, timeUs, r.hits, pct, name);
    }

    std::fprintf(fp, "───────────────────────────────────────────\n");
    std::fprintf(fp, "Total: %.1f us\n", totalAllNs / 1000.0);
    std::fclose(fp);
}

} // namespace Nova64::RSP::Recompiler
