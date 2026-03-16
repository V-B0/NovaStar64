/**
 * @file MicrocodeRegistry.hpp
 * @brief Microcode detection and registry
 *
 * Detects the microcode type from IMEM/DMEM data using CRC signatures
 * and string pattern matching. Maintains a registry of all known microcode
 * variants with their capabilities and custom handling requirements.
 */

#pragma once

#include "Microcode.hpp"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Microcode CRC Database Entry
// ═══════════════════════════════════════════════════════════════════════════

struct MicrocodeCRCEntry {
    uint32_t      crc;
    MicrocodeType type;
    const char*   name;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Detection Result
// ═══════════════════════════════════════════════════════════════════════════

enum class DetectionMethod : uint8_t {
    CRC_Match       = 0,    // Exact CRC match in database
    StringMatch     = 1,    // Matched by text string in DMEM
    Heuristic       = 2,    // Heuristic analysis of command patterns
    ForcedByGame    = 3,    // Overridden by game compatibility entry
    UserOverride    = 4,    // User manually specified
    Default         = 5,    // Fell through — using default (F3DEX2)
};

struct DetectionResult {
    MicrocodeType   type = MicrocodeType::Unknown;
    DetectionMethod method = DetectionMethod::Default;
    uint32_t        crc = 0;
    float           confidence = 0.0f;  // 0.0 = guess, 1.0 = exact match
    std::string     matchDetails;       // Human-readable match info
};

// ═══════════════════════════════════════════════════════════════════════════
//  Microcode Registry
// ═══════════════════════════════════════════════════════════════════════════

class MicrocodeRegistry {
public:
    MicrocodeRegistry();
    ~MicrocodeRegistry() = default;
    
    // ─── Detection ───────────────────────────────────────────────────────
    
    /**
     * Detect microcode type from IMEM text data.
     * Tries CRC first, then string matching, then heuristic.
     * @param imemData Pointer to microcode IMEM (up to 4KB)
     * @param imemSize Size of IMEM data in bytes
     * @param dmemData Optional DMEM data for string-based detection
     * @param dmemSize Size of DMEM data
     * @return Detection result with type, method, and confidence
     */
    DetectionResult Detect(const uint8_t* imemData, uint32_t imemSize,
                          const uint8_t* dmemData = nullptr, uint32_t dmemSize = 0) const;
    
    /**
     * Detect from CRC alone (fast path for cached microcode).
     */
    DetectionResult DetectByCRC(uint32_t crc) const;
    
    /**
     * Detect by examining text strings in DMEM data.
     * Looks for known identifiers like "RSP Gfx ucode F3DEX2"
     */
    DetectionResult DetectByString(const uint8_t* dmemData, uint32_t dmemSize) const;
    
    /**
     * Heuristic detection by analyzing command patterns.
     * Examines instruction opcodes to differentiate GBI1 vs GBI2.
     */
    DetectionResult DetectByHeuristic(const uint8_t* imemData, uint32_t imemSize) const;
    
    // ─── Registry Management ─────────────────────────────────────────────
    
    /**
     * Get full MicrocodeInfo for a detected type.
     * Fills in capabilities, GBI version, etc.
     */
    MicrocodeInfo GetInfo(MicrocodeType type) const;
    
    /**
     * Get info with specific addresses and sizes from the RSP task header.
     */
    MicrocodeInfo GetInfo(MicrocodeType type, uint32_t textAddr, uint32_t textSize,
                         uint32_t dataAddr, uint32_t dataSize) const;
    
    /**
     * Register a user-provided CRC mapping.
     * Useful for unknown rom hacks or homebrew.
     */
    void RegisterCustomCRC(uint32_t crc, MicrocodeType type, const char* name);
    
    /**
     * Force a specific microcode type (game compat override).
     */
    void SetForced(MicrocodeType type);
    void ClearForced();
    bool IsForced() const { return m_forcedType != MicrocodeType::Unknown; }
    
    // ─── Cache ───────────────────────────────────────────────────────────
    
    /**
     * Check if we've seen this CRC before and return cached result.
     */
    bool HasCachedResult(uint32_t crc) const;
    
    /**
     * Cache a detection result for future lookups.
     */
    void CacheResult(uint32_t crc, const DetectionResult& result) const;
    
    /**
     * Clear the detection cache (e.g., on ROM change).
     */
    void ClearCache();
    
    // ─── Statistics ──────────────────────────────────────────────────────
    
    uint32_t GetTotalDetections() const { return m_totalDetections; }
    uint32_t GetCRCHits() const { return m_crcHits; }
    uint32_t GetStringHits() const { return m_stringHits; }
    uint32_t GetHeuristicHits() const { return m_heuristicHits; }
    uint32_t GetCacheHits() const { return m_cacheHits; }
    
private:
    void InitializeDatabase();
    void InitializeStringPatterns();
    
    uint32_t CalculateCRC(const uint8_t* data, uint32_t size) const;
    
    // Known CRC database
    std::vector<MicrocodeCRCEntry> m_crcDatabase;
    
    // String patterns for fallback detection
    struct StringPattern {
        const char*   pattern;
        MicrocodeType type;
        float         confidence;
    };
    std::vector<StringPattern> m_stringPatterns;
    
    // User-registered custom CRCs
    std::vector<MicrocodeCRCEntry> m_customCRCs;
    
    // Detection cache: CRC -> result
    mutable std::unordered_map<uint32_t, DetectionResult> m_cache;
    mutable std::mutex m_cacheMutex;
    
    // Forced microcode override
    MicrocodeType m_forcedType = MicrocodeType::Unknown;
    
    // Statistics
    mutable uint32_t m_totalDetections = 0;
    mutable uint32_t m_crcHits = 0;
    mutable uint32_t m_stringHits = 0;
    mutable uint32_t m_heuristicHits = 0;
    mutable uint32_t m_cacheHits = 0;
};

} // namespace Nova64::Video
