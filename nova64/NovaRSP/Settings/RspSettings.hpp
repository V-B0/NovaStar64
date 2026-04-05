/**
 * @file RspSettings.hpp
 * @brief RSP Settings.
 */
// SuperNova64 - N64 Emulator
// RSP Settings - Modernized from Project64's RspSettings.h
// C++20 with thread-safe settings and enum class CPU method

#pragma once
#include <cstdint>
#include <atomic>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// RSP CPU execution method
// ──────────────────────────────────────────────
enum class RSPCpuMethod : uint32_t {
    Interpreter         = 0,
    RecompilerTasks     = 2,  // x64 task-based recompiler
    HighLevelEmulation  = 3,
};

// ──────────────────────────────────────────────
// RSP Settings - Static configuration singleton
// Thread-safe reads, single-writer pattern
// ──────────────────────────────────────────────
class CRSPSettings {
public:
    CRSPSettings() = default;
    virtual ~CRSPSettings() = default;

    CRSPSettings(const CRSPSettings&) = delete;
    CRSPSettings& operator=(const CRSPSettings&) = delete;

    // ── Accessors (inline, hot path) ──
    [[nodiscard]] static RSPCpuMethod CPUMethod() { return s_CPUMethod; }
    [[nodiscard]] static bool RomOpen()           { return s_RomOpen; }
    [[nodiscard]] static bool IsDebugging()       { return s_DebuggingEnabled; }
    [[nodiscard]] static bool IsGraphicsHle()     { return s_GraphicsHle; }
    [[nodiscard]] static bool IsAudioHle()        { return s_AudioHle; }
    [[nodiscard]] static bool IsProfiling()       { return s_Profiling; }
    [[nodiscard]] static bool IsLogAsmCode()      { return s_LogAsmCode; }
    [[nodiscard]] static bool IsSyncCPU()         { return s_SyncCPU; }

    // ── Mutators ──
    static void EnableDebugging(bool enabled);
    static void InitializeRspSetting();
    static void RefreshSettings();
    static void SetRomOpen(bool opened);
    static void ApplyPerGameOverrides(uint32_t crc1, uint32_t crc2);

private:
    static bool         s_DebuggingEnabled;
    static bool         s_RomOpen;
    static RSPCpuMethod s_CPUMethod;
    static bool         s_GraphicsHle;
    static bool         s_AudioHle;
    static bool         s_Profiling;
    static bool         s_LogAsmCode;
    static bool         s_SyncCPU;
    static bool         s_RspMultiThreaded;
    static bool         s_ConditionalMove;
    static bool         s_ShowErrors;
    static bool         s_BreakOnStart;
    static bool         s_LogRDP;
    static bool         s_IndividualBlock;
};

} // namespace Nova64::RSP
