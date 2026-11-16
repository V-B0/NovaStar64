/**
 * @file TaskDispatcher.hpp
 * @brief RSP HLE task dispatcher: microcode detection and routing.
 */

#pragma once
#include "HleTypes.hpp"
#include "MicrocodeRegistry.hpp"
#include "GraphicsUcodes.hpp"
#include "Crc32.hpp"
#include "mem.hpp"
#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace Nova64::RSP::HLE {

class CHle;

// ──────────────────────────────────────────────
// Cached microcode identification
// ──────────────────────────────────────────────
struct MicrocodeCache {
    uint32_t ucode_crc{0};
    uint32_t data_crc{0};
    const MicrocodeEntry* entry{nullptr};
    bool valid{false};
};

// ──────────────────────────────────────────────
// CTaskDispatcher
// Identifies RSP microcodes and routes tasks to HLE handlers
// ──────────────────────────────────────────────
class CTaskDispatcher {
public:
    CTaskDispatcher() = default;
    ~CTaskDispatcher() = default;

    CTaskDispatcher(const CTaskDispatcher&) = delete;
    CTaskDispatcher& operator=(const CTaskDispatcher&) = delete;

    // ── Main dispatch entry point ──
    // Returns true if task was handled by HLE
    bool DispatchTask(CHle* hle);

    // ── Task info parsing ──
    [[nodiscard]] static TaskInfo ParseTask(const uint8_t* dmem);

    // ── Microcode identification ──
    [[nodiscard]] const MicrocodeEntry* IdentifyMicrocode(CHle* hle, const TaskInfo& task);

    // ── Cache management ──
    void InvalidateCache() {
        m_Cache.valid = false;
        m_UcodeCache.clear();
        m_GfxHleConsecutiveFailures = 0;
        m_GfxHleAutoDisabled = false;
    }

    // ── Graphics HLE state ──
    [[nodiscard]] bool GfxHleAutoDisabled() const { return m_GfxHleAutoDisabled; }
    void ResetGfxHle() {
        m_GfxHleConsecutiveFailures = 0;
        m_GfxHleAutoDisabled = false;
    }

    // ── Statistics ──
    [[nodiscard]] uint32_t TasksDispatched() const { return m_TasksDispatched; }
    [[nodiscard]] uint32_t TasksFallback() const { return m_TasksFallback; }
    [[nodiscard]] uint32_t CacheHits() const { return m_CacheHits; }

private:
    // Compute CRC32 of microcode data from DRAM
    [[nodiscard]] uint32_t ComputeUcodeCrc(CHle* hle, uint32_t addr, uint32_t size);
    [[nodiscard]] uint32_t ComputeUcodeDataCrc(CHle* hle, uint32_t addr, uint32_t size);

    // Dispatch by task type
    bool DispatchVideoTask(CHle* hle, const TaskInfo& task);
    bool DispatchAudioTask(CHle* hle, const TaskInfo& task, const MicrocodeEntry* ucode);
    bool DispatchJpegTask(CHle* hle, const TaskInfo& task, const MicrocodeEntry* ucode);

    // sum_bytes fallback when CRC lookup fails
    [[nodiscard]] const MicrocodeEntry* TrySumBytesFallback(CHle* hle, const TaskInfo& task);

    // Per-address microcode cache (avoids recomputing CRC for same ucode address)
    std::unordered_map<uint32_t, MicrocodeCache> m_UcodeCache;

    // Last-used cache for fast path
    MicrocodeCache m_Cache;

    // Stats
    uint32_t m_TasksDispatched{0};
    uint32_t m_TasksFallback{0};
    uint32_t m_CacheHits{0};

    // Graphics HLE failure tracking: auto-disable after repeated failures
    uint32_t m_GfxHleConsecutiveFailures{0};
    static constexpr uint32_t kMaxGfxHleFailures = 3; // Disable after N consecutive failures
    bool m_GfxHleAutoDisabled{false};
};

} // namespace Nova64::RSP::HLE
