/**
 * @file MainSYS.hpp
 * @brief Main N64 System emulation core
 */

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include "Types.hpp"

// Memory Handler Includes
#include "Memoryhandler/AudioInterfaceHandler.hpp"
#include "Memoryhandler/VideoInterfaceHandler.hpp"
#include "Memoryhandler/RDRAMInterFaceHandler.hpp"
#include "Memoryhandler/RDRAMRegHandler.hpp"
#include "Memoryhandler/MIPSYInterfaceHandler.hpp"
#include "Memoryhandler/PIInterFace.hpp"
#include "Memoryhandler/SIInterfaceHandler.hpp"
#include "Memoryhandler/SPregistersHandler.hpp"
#include "Memoryhandler/RomMemHandler.hpp"
#include "Memoryhandler/PifInterface.hpp"
#include "Memoryhandler/CachedRom.hpp"
#include "Memoryhandler/UnCachedRom.hpp"
#include "Memoryhandler/CachedSysMemory.hpp"
#include "Memoryhandler/UnCachedSysMemory.hpp"
#include "Savetypes/Eeprom.hpp"
#include "Plugins/PluginManager.hpp"

// Video plugin forward declaration
namespace Nova64 { namespace Video { class WindowManager; } }

// Forward declarations
namespace N64 {
    namespace Rom { class N64Rom; }
    namespace Disk { class N64Disk; }
}
using N64::Rom::N64Rom;
using N64::Disk::N64Disk;

class CThread;
class CRegisters;
class CSystemEvents;
namespace N64System { class SystemTimer; class CTLB; }
namespace Nova64 { class MipsMemoryVM; }

// System event enumeration
enum SystemEvent
{
    SysEvent_CloseCPU,
    SysEvent_ResetCPU_Soft,
    SysEvent_ResetCPU_Hard,
    SysEvent_PauseCPU,
    SysEvent_ResumeCPU,
    SysEvent_ExecuteInterrupt
};

// Main N64 System class
class CN64System
{
public:
    // Constructor and destructor
    CN64System(uint32_t randomizer_seed, bool SavesReadOnly, bool SyncSystem);
    virtual ~CN64System();

    // Save chip type
    N64Types::SaveChipType m_SaveUsing;

    // Static methods for ROM/Disk loading
    static bool LoadFileImage(const char* FileLoc);
    static bool LoadFileImageIPL(const char* FileLoc);
    static bool LoadDiskImage(const char* FileLoc, const bool Expansion);
    
    // Static methods for running emulation
    static bool RunFileImage(const char* FileLoc, uint64_t frameLimit = 0);
    static bool RunDiskImage(const char* FileLoc);
    static bool RunDiskComboImage(const char* FileLoc, const char* FileLocDisk);
    static void RunLoadedImage(uint64_t frameLimit = 0);
    static void CloseSystem(void);

    // Instance method for interactive loop (called from RunLoadedImage)
    void RunInteractiveLoop();

    // Accessors
    N64System::SystemTimer* GetSystemTimer() const;
    N64Rom* GetRom() const;
    N64Rom* GetDDRom() const;
    N64Disk* GetDisk() const;

    // Control Ref
    std::atomic<bool>& GetEndEmulationRef() { return m_EndEmulation; }

    // CPU timing - cycles per opcode (configurable for speed hacks)
    uint32_t CountPerOp() const { return m_CountPerOp; }
    void SetCountPerOp(uint32_t count) { m_CountPerOp = (count > 0) ? count : 1; }

    // Frame limiting for CLI testing
    void SetFrameLimit(uint64_t maxFrames) { m_MaxFrames = maxFrames; }
    [[nodiscard]] bool IsFrameLimitReached() const;
    [[nodiscard]] uint64_t GetCurrentFrameCount() const;

    // Frame rate target (0=unlimited, 30, 60)
    void SetTargetFPS(int fps) { m_TargetFPS = fps; }
    [[nodiscard]] int GetTargetFPS() const { return m_TargetFPS; }
    
    // Static target FPS to pass through to system before creation
    static int s_TargetFPS;

    // Static window resolution to pass through before creation
    static int s_WindowWidth;
    static int s_WindowHeight;
    static int s_InternalResScale;
    static int s_FullscreenMode; // 0=windowed, 1=borderless fullscreen, 2=exclusive fullscreen

    // Components
    CRegisters* GetRegisters() const { return m_Registers.get(); }
    Nova64::MipsMemoryVM* GetMemory() const { return m_Memory.get(); }
    N64System::CTLB* GetTLB() const { return m_TLB.get(); }
    CSystemEvents* GetSystemEvents() const { return m_SystemEvents.get(); }
    Plugins::PluginManager* GetPluginManager() const { return m_PluginManager.get(); }
    Nova64::Video::WindowManager* GetWindowManager() const { return m_WindowManager.get(); }

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

    // Jump helpers for the interpreter
    void DelayedJump(uint64_t target);
    void DelayedRelativeJump(int32_t offset);

    [[nodiscard]] bool IsEmulationRunning() const { return !m_EndEmulation; }
    
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
    void ExecuteRecompiler();
    void ExecuteSyncCPU();
    void CpuStopped();

public:
    // Interpreter can be called from recompiler as fallback
    void ExecuteInterpret();
    
    // Interpret a single instruction at current PC (used by recompiler fallback)
    void ExecuteSingleOp();

public:
    // System state
    std::atomic<bool> m_EndEmulation;
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
    uint32_t m_CountPerOp = 2; // Default: 2 cycles per op (N64 standard)
    uint64_t m_MaxFrames = 0; // 0 = unlimited, >0 = frame limit for CLI testing
    
    // Frame rate limiter
    int m_TargetFPS = 60;  // 0=unlimited, 30=half speed, 60=full speed (NTSC)
    std::chrono::steady_clock::time_point m_FramePaceStart;
    bool m_FramePaceInitialized = false;
    int m_FrameSkipCount = 0;    // Consecutive frames skipped
    int m_MaxFrameSkip = 2;      // Max consecutive skipped renders
    
    // Pipeline stage
    N64Types::PipelineStage m_PipelineStage;
    uint32_t m_InterruptDelay = 0; // Delay interrupt firing after MTC0 Status (MTC0→ERET hazard)
    
    std::unique_ptr<N64System::SystemTimer> m_SystemTimer;
    std::unique_ptr<CSystemEvents> m_SystemEvents;
    std::unique_ptr<N64System::CTLB> m_TLB;
    std::unique_ptr<CRegisters> m_Registers;
    std::unique_ptr<Nova64::MipsMemoryVM> m_Memory;

    // Graphics window (optional for headless mode)
    std::unique_ptr<Nova64::Video::WindowManager> m_WindowManager;

    // Memory Handlers
    std::unique_ptr<Plugins::PluginManager> m_PluginManager;
    std::unique_ptr<CEeprom> m_Eeprom;
    std::unique_ptr<AudioInterfaceHandler> m_AudioInterfaceHandler;
    std::unique_ptr<RDRAMInterfaceHandler> m_RDRAMInterfaceHandler;
    std::unique_ptr<RDRAMRegistersHandler> m_RDRAMRegistersHandler;
    std::unique_ptr<MIPSYInterfaceHandler> m_MIPSInterfaceHandler;
    std::unique_ptr<PI::PeripheralInterfaceHandler> m_PeripheralInterfaceHandler;
    std::unique_ptr<SerialInterfaceHandler> m_SerialInterfaceHandler;
    std::unique_ptr<SRegistersHandler> m_SPRegistersHandler;
    std::unique_ptr<UncachedRom> m_UncachedRomHandler;
    std::unique_ptr<RomMemoryHandler> m_RomMemoryHandler;
    std::unique_ptr<PifRamHandler> m_PifRamHandler;
    std::unique_ptr<DD64::CachedRom> m_CachedRom;
    std::unique_ptr<CachedSysMemory> m_CachedSysMemory;
    std::unique_ptr<UnCachedSysMemory> m_UnCachedSysMemory;

    // Helper for DD regs
    std::array<uint32_t, 32> m_DDRegisters; // Initialized to 0

    // DMA block invalidation callback for recompiler
    // When set, called after PI/SP DMA copies data to RDRAM so the
    // recompiler can discard stale compiled blocks in the target range.
    using DMAInvalidateFunc = void(*)(void* ctx, uint32_t physAddr, uint32_t length);
    DMAInvalidateFunc m_DMAInvalidateFn = nullptr;
    void*             m_DMAInvalidateCtx = nullptr;

    void NotifyDMAToRDRAM(uint32_t physAddr, uint32_t length) {
        if (m_DMAInvalidateFn)
            m_DMAInvalidateFn(m_DMAInvalidateCtx, physAddr, length);
    }
};
