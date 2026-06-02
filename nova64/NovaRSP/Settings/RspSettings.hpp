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

    // ── MLE pivot mutators ──
    // Driven by the video plugin from NovaGraphics.ini renderingMode key.
    // Must be called before DoRspCycles processes the first task.
    // These set a *sticky user override* that suppresses the matching
    // field of per-game CRC overrides in ApplyPerGameOverrides.
    static void SetGraphicsHle(bool enabled) {
        s_GraphicsHle = enabled;
        s_UserGraphicsHleSet = true;
    }
    static void SetAudioHle(bool enabled) {
        s_AudioHle = enabled;
        s_UserAudioHleSet = true;
    }
    static void SetCPUMethod(RSPCpuMethod m) {
        s_CPUMethod = m;
        s_UserCPUMethodSet = true;
    }

    // Predicates consumed by ApplyPerGameOverrides.
    [[nodiscard]] static bool UserGraphicsHleSet() { return s_UserGraphicsHleSet; }
    [[nodiscard]] static bool UserAudioHleSet()    { return s_UserAudioHleSet; }
    [[nodiscard]] static bool UserCPUMethodSet()   { return s_UserCPUMethodSet; }

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
    // MLE pivot sticky-user-override flags
    static bool         s_UserGraphicsHleSet;
    static bool         s_UserAudioHleSet;
    static bool         s_UserCPUMethodSet;
};

} // namespace Nova64::RSP
