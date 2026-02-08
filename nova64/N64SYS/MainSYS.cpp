#include "../common/platform.hpp"
#include "../common/trace.hpp"
#include "../common/log.hpp"
#include "../common/path.hpp"
#include "../common/file.hpp"
#include "../common/md5.hpp"
#include "../common/MemoryManagment.hpp"
#include "../common/Thread.hpp"

#include "MainSYS.hpp"
#include "Types.hpp"
#include "N64Rom.hpp"
#include "N64DD.hpp"

#include <cstring>
#include <memory>
#include <ctime>
#include <format>

// Using directives for Trace namespace
using Trace::ModuleID;
using Trace::Severity;
using Trace::SourceLocation;

// Using directives for N64 ROM and Disk classes
using N64::Rom::N64Rom;
using N64::Disk::N64Disk;

// Global system instances
CN64System* g_System = nullptr;
CN64System* g_BaseSystem = nullptr;
N64Rom* g_Rom = nullptr;
N64Rom* g_DDRom = nullptr;
N64Disk* g_Disk = nullptr;

// Save chip type alias
using SAVE_CHIP_TYPE = N64Types::SaveChipType;
constexpr SAVE_CHIP_TYPE SAVE_CHIP_UNKNOWN = SAVE_CHIP_TYPE::Auto;

// Helper macro for logging - uses existing TRACE_* macros
#define LOG_DEBUG(...)   TRACE_DEBUG(Trace::Module::Core, __VA_ARGS__)
#define LOG_INFO(...)    TRACE_INFO(Trace::Module::Core, __VA_ARGS__)
#define LOG_WARNING(...) TRACE_WARN(Trace::Module::Core, __VA_ARGS__)
#define LOG_ERROR(...)   TRACE_ERROR(Trace::Module::Core, __VA_ARGS__)

// CN64System constructor
CN64System::CN64System(uint32_t randomizer_seed, bool SavesReadOnly, bool SyncSystem) :
    m_EndEmulation(false),
    m_SaveUsing(SAVE_CHIP_UNKNOWN),
    m_SyncCPU(nullptr),
    m_InReset(false),
    m_NextTimer(0),
    m_bCleanFrameBox(true),
    m_TestTimer(false),
    m_CyclesToSkip(0),
    m_JumpToLocation(0),
    m_JumpDelayLocation(0),
    m_TLBLoadAddress(0),
    m_TLBStoreAddress(0),
    m_SyncCount(0),
    m_thread(nullptr),
    m_SyncSystem(SyncSystem),
    m_Random(randomizer_seed),
    m_SavesReadOnly(SavesReadOnly)
{
    Log::debugf("CN64System constructor (seed: %u, ReadOnly: %s, Sync: %s)",
                randomizer_seed, SavesReadOnly ? "true" : "false", SyncSystem ? "true" : "false");
}

// CN64System destructor
CN64System::~CN64System()
{
    LOG_DEBUG( "CN64System destructor");

    SetActiveSystem(false);

    if (m_SyncCPU)
    {
        m_SyncCPU->CpuStopped();
        delete m_SyncCPU;
        m_SyncCPU = nullptr;
    }

    if (m_thread != nullptr)
    {
        LOG_DEBUG( "Deleting thread object");
        delete m_thread;
        m_thread = nullptr;
    }
}

// Load ROM file image
bool CN64System::LoadFileImage(const char* FileLoc)
{
    LOG_DEBUG(
              "LoadFileImage: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (FileLoc == nullptr || FileLoc[0] == '\0')
    {
        LOG_ERROR( "Invalid file location");
        return false;
    }

    CloseSystem();

    // Mark the ROM as loading
    LOG_DEBUG( "Mark ROM as loading");

    // Try to load the passed N64 ROM
    if (g_Rom == nullptr)
    {
        LOG_DEBUG( "Allocating global ROM object");
        g_Rom = new N64Rom();
    }
    else
    {
        LOG_DEBUG( "Use existing global ROM object");
    }

    LOG_DEBUG( "Loading \"{}\"", FileLoc);
    try
    {
        g_Rom->LoadRom(FileLoc);
        if (g_Rom->GetImage().IsDDIPL())
        {
            // 64DD IPL
            if (g_DDRom == nullptr)
            {
                g_DDRom = new N64Rom();
            }
            g_DDRom->LoadIPL(FileLoc);
        }

        LOG_DEBUG( "ROM loaded successfully");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(
                  "LoadN64Image failed (\"{}\")", FileLoc);
        delete g_Rom;
        g_Rom = nullptr;
        return false;
    }
}

// Load 64DD IPL image
bool CN64System::LoadFileImageIPL(const char* FileLoc)
{
    LOG_DEBUG(
              "LoadFileImageIPL: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (FileLoc == nullptr || FileLoc[0] == '\0')
    {
        LOG_ERROR( "Invalid file location");
        return false;
    }

    CloseSystem();

    // Mark the N64DD IPL as loading
    LOG_DEBUG( "Mark N64DD IPL as loading");

    // Try to load the passed N64DD IPL
    if (g_DDRom == nullptr)
    {
        LOG_DEBUG( "Allocating global N64DD IPL object");
        g_DDRom = new N64Rom();
    }
    else
    {
        LOG_DEBUG( "Use existing global N64DD IPL object");
    }

    LOG_DEBUG( "Loading \"{}\"", FileLoc);
    try
    {
        g_DDRom->LoadIPL(FileLoc);
        if (!g_DDRom->GetImage().IsDDIPL())
        {
            // If not 64DD IPL then it's wrong
            LOG_ERROR(
                      "LoadN64ImageIPL failed - not a 64DD IPL (\"{}\")", FileLoc);
            delete g_DDRom;
            g_DDRom = nullptr;
            return false;
        }

        LOG_DEBUG( "64DD IPL loaded successfully");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(
                  "LoadN64ImageIPL failed (\"{}\")", FileLoc);
        delete g_DDRom;
        g_DDRom = nullptr;
        return false;
    }
}

// Load disk image
bool CN64System::LoadDiskImage(const char* FileLoc, const bool Expansion)
{
    Log::debugf("LoadDiskImage: \"%s\" (Expansion: %s)",
                FileLoc ? FileLoc : "(null)", Expansion ? "true" : "false");

    if (FileLoc == nullptr || FileLoc[0] == '\0')
    {
        LOG_ERROR( "Invalid file location");
        return false;
    }

    CloseSystem();

    // Mark the disk as loading
    LOG_DEBUG( "Mark disk as loading");

    // Try to load the passed N64 disk
    if (g_Disk == nullptr)
    {
        LOG_DEBUG( "Allocating global disk object");
        g_Disk = new N64Disk();
    }
    else
    {
        LOG_DEBUG( "Use existing global disk object");
    }

    LOG_DEBUG( "Loading \"{}\"", FileLoc);
    try
    {
        g_Disk->LoadDisk(FileLoc);
        LOG_DEBUG( "Disk loaded successfully");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(
                  "LoadDiskImage failed (\"{}\")", FileLoc);
        delete g_Disk;
        g_Disk = nullptr;
        return false;
    }
}

// Run ROM file image
bool CN64System::RunFileImage(const char* FileLoc)
{
    LOG_DEBUG(
              "RunFileImage: \"{}\"", FileLoc ? FileLoc : "(null)");

    // Uninitialize g_Disk and g_DDRom to prevent exception when ending emulation
    if (g_Disk != nullptr)
    {
        g_Disk->UnloadDisk();
        delete g_Disk;
        g_Disk = nullptr;
    }
    if (g_DDRom != nullptr)
    {
        g_DDRom->UnloadRom();
        delete g_DDRom;
        g_DDRom = nullptr;
    }

    if (!LoadFileImage(FileLoc))
    {
        return false;
    }

    RunLoadedImage();
    return true;
}

// Run disk image
bool CN64System::RunDiskImage(const char* FileLoc)
{
    LOG_DEBUG(
              "RunDiskImage: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (!LoadDiskImage(FileLoc, false))
    {
        return false;
    }

    // TODO: Select and load IPL ROM depending on disk country code

    RunLoadedImage();
    return true;
}

// Run disk combo image (ROM + Disk)
bool CN64System::RunDiskComboImage(const char* FileLoc, const char* FileLocDisk)
{
    Log::debugf("RunDiskComboImage: ROM=\"%s\" Disk=\"%s\"",
                FileLoc ? FileLoc : "(null)", FileLocDisk ? FileLocDisk : "(null)");

    if (!LoadDiskImage(FileLocDisk, true))
    {
        return false;
    }
    if (!LoadFileImage(FileLoc))
    {
        return false;
    }

    // TODO: Select and load IPL ROM depending on disk country code

    RunLoadedImage();
    return true;
}

// Run loaded image
void CN64System::RunLoadedImage(void)
{
    LOG_DEBUG( "RunLoadedImage");

    g_BaseSystem = new CN64System((uint32_t)time(nullptr), false, false);
    if (g_BaseSystem)
    {
        LOG_DEBUG( "Starting emulation");
        g_BaseSystem->StartEmulation(true);
    }
    else
    {
        LOG_ERROR( "Failed to create CN64System");
    }
}

// Close system
void CN64System::CloseSystem()
{
    LOG_DEBUG( "CloseSystem");

    if (g_BaseSystem)
    {
        g_BaseSystem->CloseCpu();
        delete g_BaseSystem;
        g_BaseSystem = nullptr;
    }
}

// Close CPU
void CN64System::CloseCpu()
{
    LOG_DEBUG( "CloseCpu - Start");

    if (m_thread == nullptr)
    {
        return;
    }

    LOG_DEBUG( "Setting end emulation");
    m_EndEmulation = true;

    // TODO: If paused, trigger pause event

    // TODO: Wait for thread to finish or timeout
    CThread* hThread = m_thread;
    m_thread = nullptr;

    if (hThread != nullptr)
    {
        // Wait for thread completion
        // TODO: Implement proper thread join/wait
        delete hThread;
    }

    LOG_DEBUG( "CloseCpu - Done");
}

// Start emulation
void CN64System::StartEmulation(bool NewThread)
{
    LOG_DEBUG(
              "StartEmulation (NewThread: {})", NewThread ? "true" : "false");

    StartEmulation2(NewThread);
}

// Start emulation (internal)
void CN64System::StartEmulation2(bool NewThread)
{
    LOG_DEBUG(
              "StartEmulation2 (NewThread: {})", NewThread ? "true" : "false");

    if (NewThread)
    {
        LOG_DEBUG( "Setting system as active");
        bool bSetActive = SetActiveSystem();

        if (bSetActive)
        {
            LOG_DEBUG( "Starting emulation thread");
            StartEmulationThread();
        }
        else
        {
            LOG_WARNING(
                      "Failed to set system as active");
        }
    }
    else
    {
        // Execute CPU directly
        LOG_DEBUG( "Start executing CPU");
        ExecuteCPU();
    }
}

// Start emulation thread
void CN64System::StartEmulationThread()
{
    LOG_DEBUG( "StartEmulationThread");

    // TODO: Create and start emulation thread
    // CThread* thread = new CThread((CThread::CTHREAD_START_ROUTINE)StartEmulationThreadStub);
    // thread->Start(thread);
}

// End emulation
void CN64System::EndEmulation(void)
{
    LOG_DEBUG( "EndEmulation");
    m_EndEmulation = true;
}

// Pause emulation
void CN64System::Pause()
{
    LOG_DEBUG( "Pause");
    // TODO: Implement pause functionality
}

// Game reset
void CN64System::GameReset()
{
    LOG_DEBUG( "GameReset");
    Reset(true, false);
}

// System reset
void CN64System::Reset(bool bInitReg, bool ClearMemory)
{
    Log::debugf("Reset (InitReg: %s, ClearMemory: %s)",
                bInitReg ? "true" : "false", ClearMemory ? "true" : "false");

    // TODO: Implement full reset logic
    m_CyclesToSkip = 0;
    m_SyncCount = 0;

    if (bInitReg)
    {
        // TODO: Initialize registers
    }

    // TODO: Reset system timer
    // TODO: Reset recompiler if exists
    // TODO: Reset plugins

    if (m_SyncCPU)
    {
        m_SyncCPU->Reset(bInitReg, ClearMemory);
    }
}

// Set active system
bool CN64System::SetActiveSystem(bool bActive)
{
    LOG_DEBUG(
              "SetActiveSystem: {}", bActive ? "true" : "false");

    if (bActive)
    {
        // Set this system as active
        if (g_System != nullptr && g_System != this)
        {
            LOG_WARNING(
                      "Another system is already active");
            return false;
        }
        g_System = this;
    }
    else
    {
        // Deactivate this system
        if (g_System == this)
        {
            g_System = nullptr;
        }
    }

    return true;
}

// Execute CPU
void CN64System::ExecuteCPU()
{
    LOG_DEBUG( "ExecuteCPU - Start");

    m_EndEmulation = false;

    // TODO: Determine CPU type (interpreter, recompiler, sync)
    // For now, stub execution
    LOG_INFO(
              "CPU execution stubbed - no emulation core yet");

    // Stub: Just wait and exit
    while (!m_EndEmulation)
    {
        // TODO: Execute instructions
        // For now, just break immediately
        break;
    }

    CpuStopped();

    LOG_DEBUG( "ExecuteCPU - Done");
}

// Execute interpreter
void CN64System::ExecuteInterpret()
{
    LOG_DEBUG( "ExecuteInterpret");
    // TODO: Implement interpreter execution
}

// Execute recompiler
void CN64System::ExecuteRecompiler()
{
    LOG_DEBUG( "ExecuteRecompiler");
    // TODO: Implement recompiler execution
}

// Execute sync CPU
void CN64System::ExecuteSyncCPU()
{
    LOG_DEBUG( "ExecuteSyncCPU");
    // TODO: Implement sync CPU execution
}

// CPU stopped
void CN64System::CpuStopped()
{
    LOG_DEBUG( "CpuStopped");

    if (!m_InReset)
    {
        LOG_INFO( "Emulation ended");
    }

    if (m_SyncCPU)
    {
        m_SyncCPU->CpuStopped();
    }
}

// External event
void CN64System::ExternalEvent(SystemEvent action)
{
    Log::debugf("ExternalEvent: %d", static_cast<int>(action));

    switch (action)
    {
    case SysEvent_CloseCPU:
        m_EndEmulation = true;
        break;
    case SysEvent_ResetCPU_Soft:
        GameReset();
        break;
    case SysEvent_ResetCPU_Hard:
        Reset(true, true);
        break;
    case SysEvent_PauseCPU:
        Pause();
        break;
    default:
        Log::warningf("Unknown system event: %d", static_cast<int>(action));
        break;
    }
}

// Refresh screen
void CN64System::RefreshScreen()
{
    // TODO: Implement screen refresh
}

// Load state
bool CN64System::LoadState()
{
    LOG_DEBUG( "LoadState (default slot)");
    // TODO: Implement save state loading
    return false;
}

// Load state from file
bool CN64System::LoadState(const char* FileName)
{
    LOG_DEBUG(
              "LoadState: \"{}\"", FileName ? FileName : "(null)");
    // TODO: Implement save state loading from file
    return false;
}

// Save state
bool CN64System::SaveState()
{
    LOG_DEBUG( "SaveState (default slot)");
    // TODO: Implement save state saving
    return false;
}

// Save state to file
bool CN64System::SaveState(const char* FileName)
{
    LOG_DEBUG(
              "SaveState: \"{}\"", FileName ? FileName : "(null)");
    // TODO: Implement save state saving to file
    return false;
}

// Stub functions for zip.h functionality
// These would normally use the zip library, but we're stubbing them for now

namespace ZipStub
{
    // Stub for opening zip files
    void* OpenZipFile(const char* filename)
    {
        LOG_DEBUG(
                  "ZipStub::OpenZipFile: \"{}\" (STUBBED)", filename);
        // Return nullptr to indicate zip files are not supported yet
        return nullptr;
    }

    // Stub for closing zip files
    void CloseZipFile(void* zipHandle)
    {
        LOG_DEBUG(
                  "ZipStub::CloseZipFile (STUBBED)");
        // Nothing to do
    }

    // Stub for getting file count in zip
    int GetZipFileCount(void* zipHandle)
    {
        LOG_DEBUG(
                  "ZipStub::GetZipFileCount (STUBBED)");
        return 0;
    }

    // Stub for getting file from zip
    bool GetZipFile(void* zipHandle, int fileIndex, void* buffer, size_t bufferSize)
    {
        LOG_DEBUG(
                  "ZipStub::GetZipFile (STUBBED)");
        return false;
    }

    // Stub for getting file info from zip
    bool GetZipFileInfo(void* zipHandle, int fileIndex, char* filename, size_t* fileSize)
    {
        LOG_DEBUG(
                  "ZipStub::GetZipFileInfo (STUBBED)");
        return false;
    }
}
