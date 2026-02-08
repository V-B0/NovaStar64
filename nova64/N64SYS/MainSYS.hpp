/**
 * @file MainSYS.hpp
 * @brief Main N64 System emulation core
 */

#pragma once

#include <cstdint>
#include <string>
#include "Types.hpp"

// Forward declarations
class CN64Rom;
class CN64Disk;
class CThread;

// System event enumeration
enum SystemEvent
{
    SysEvent_CloseCPU,
    SysEvent_ResetCPU_Soft,
    SysEvent_ResetCPU_Hard,
    SysEvent_PauseCPU,
    SysEvent_ResumeCPU
};

// Main N64 System class
class CN64System
{
public:
    // Constructor and destructor
    CN64System(uint32_t randomizer_seed, bool SavesReadOnly, bool SyncSystem);
    virtual ~CN64System();

    // Save chip type
    SAVE_CHIP_TYPE m_SaveUsing;

    // Static methods for ROM/Disk loading
    static bool LoadFileImage(const char* FileLoc);
    static bool LoadFileImageIPL(const char* FileLoc);
    static bool LoadDiskImage(const char* FileLoc, const bool Expansion);
    
    // Static methods for running emulation
    static bool RunFileImage(const char* FileLoc);
    static bool RunDiskImage(const char* FileLoc);
    static bool RunDiskComboImage(const char* FileLoc, const char* FileLocDisk);
    static void RunLoadedImage(void);
    static void CloseSystem(void);

    // Instance methods
    void CloseCpu();
    void ExternalEvent(SystemEvent action);
    void StartEmulation(bool NewThread);
    void EndEmulation();
    void Pause();
    void GameReset();
    void Reset(bool bInitReg, bool ClearMemory);
    bool SetActiveSystem(bool bActive = true);
    
    void ExecuteCPU();
    void RefreshScreen();
    
    // Save state management
    bool LoadState();
    bool LoadState(const char* FileName);
    bool SaveState();
    bool SaveState(const char* FileName);

    // Delete copy/move semantics
    CN64System(const CN64System&) = delete;
    CN64System(CN64System&&) = delete;
    CN64System& operator=(const CN64System&) = delete;
    CN64System& operator=(CN64System&&) = delete;

private:
    // Internal methods
    void StartEmulation2(bool NewThread);
    void StartEmulationThread();
    void ExecuteInterpret();
    void ExecuteRecompiler();
    void ExecuteSyncCPU();
    void CpuStopped();

private:
    // System state
    bool m_EndEmulation;
    CN64System* m_SyncCPU;
    bool m_InReset;
    int32_t m_NextTimer;
    bool m_bCleanFrameBox;
    bool m_TestTimer;
    uint32_t m_CyclesToSkip;
    uint64_t m_JumpToLocation;
    uint64_t m_JumpDelayLocation;
    uint32_t m_TLBLoadAddress;
    uint32_t m_TLBStoreAddress;
    uint32_t m_SyncCount;
    CThread* m_thread;
    bool m_SyncSystem;
    uint32_t m_Random;
    bool m_SavesReadOnly;
};
