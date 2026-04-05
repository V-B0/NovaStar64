/**
 * @file MainSYS.cpp
 * @brief Core N64 system emulation: initialization, ROM loading, CPU dispatch, and frame pacing.
 */
#include "../common/platform.hpp"
#include <cstdio>
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
#include "MIPSY/Systiming.hpp"
#include "MIPSY/Register.hpp"
#include "MIPSY/Vmem.hpp"
#include "MIPSY/TLB.hpp"
#include "MIPSY/Sysevents.hpp"
#include "Interpreter/InterpreterOps.hpp"
#include "Recomp/Recompiler.hpp"
#include "../NovaRSP/RSPInfo.hpp"
#include "../NovaRSP/CPU/RspSystem.hpp"
#include "../NovaGraphics/Window/WindowManager.hpp"
#include "../NovaGraphics/Plugin/NovaGraphicsGFXPlugin.hpp"

#include <cstring>
#include <memory>
#include <ctime>
#include <chrono>
#include <thread>
#include <format>
#include <fstream>
#include <filesystem>

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
int CN64System::s_TargetFPS = 60;
int CN64System::s_WindowWidth = 1280;
int CN64System::s_WindowHeight = 960;
int CN64System::s_InternalResScale = 4;

// ──────────────────────────────────────────────
// RSP callback wrappers (static functions matching RSP_INFO signatures)
// These bridge the N64System → RSP interface
// ──────────────────────────────────────────────

static void RSP_CheckInterrupts()
{
    if (g_System && g_System->GetRegisters())
    {
        g_System->GetRegisters()->CheckInterrupts();
    }
}

static void RSP_ProcessDList()
{
    // Called by RSP HLE when a video (GFX) task is detected.
    // Forward to the GFX plugin's ProcessDList.
    if (g_System && g_System->GetPluginManager())
    {
        auto* gfx = g_System->GetPluginManager()->GetGraphicsPlugin();
        if (gfx) { gfx->ProcessDList(0, 0); }
    }

    // Signal DP (Display Processor) completion interrupt.
    // After HLE processes the display list, the RDP is "done".
    // If we're inside ClearHalt's DoRspCycles, defer the interrupt bits
    // so the game code can return and update scheduler state first.
    if (g_System && g_System->GetRegisters())
    {
        auto* regs = g_System->GetRegisters();
        if (regs->m_defer_hle_interrupts) {
            // Accumulate DP interrupt for deferred delivery via RspTimer
            regs->m_deferred_mi_intr |= 0x20;  // MI_INTR_DP
        } else {
            regs->m_GfxIntrReg |= 0x20;          // MI_INTR_DP
            regs->m_Mips_Interface[2] |= 0x20;    // MI_INTR_REG |= DP bit
            regs->CheckInterrupts();
        }
    }
}

static void RSP_ProcessRdpList()
{
    // Called by RSP HLE when raw RDP commands are submitted.
    // Forward to the GFX plugin.
    if (g_System && g_System->GetPluginManager())
    {
        auto* gfx = g_System->GetPluginManager()->GetGraphicsPlugin();
        if (gfx) { gfx->UpdateRDPStatus(); }
    }
}
N64Rom* g_DDRom = nullptr;
N64Disk* g_Disk = nullptr;

using SAVE_CHIP_TYPE = N64Types::SaveChipType;
constexpr SAVE_CHIP_TYPE SAVE_CHIP_UNKNOWN = SAVE_CHIP_TYPE::Auto;

// Helper macro for logging - uses existing TRACE_* macros
#define LOG_DEBUG(...)   TRACE_DEBUG(Trace::Module::Core, __VA_ARGS__)
#define LOG_INFO(...)    TRACE_INFO(Trace::Module::Core, __VA_ARGS__)
#define LOG_WARNING(...) TRACE_WARN(Trace::Module::Core, __VA_ARGS__)
#define LOG_ERROR(...)   TRACE_ERROR(Trace::Module::Core, __VA_ARGS__)

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
    m_SavesReadOnly(SavesReadOnly),
    m_PipelineStage(N64Types::PipelineStage::Normal),
    m_SystemTimer(std::make_unique<N64System::SystemTimer>(*this))
{
    Log::debugf("CN64System constructor (seed: %u, ReadOnly: %s, Sync: %s)",
                randomizer_seed, SavesReadOnly ? "true" : "false", SyncSystem ? "true" : "false");

    // Initialize components
    m_SystemEvents = std::make_unique<CSystemEvents>(*this);
    m_TLB = std::make_unique<N64System::CTLB>();
    m_Registers = std::make_unique<CRegisters>(*this, *m_SystemEvents);
    // Wire up TLB with registers first (MipsMemoryVM constructor needs m_TLB.GetRegisters())
    if(m_TLB) m_TLB->Initialize(nullptr, m_Registers.get(), nullptr);

    m_Memory = std::make_unique<Nova64::MipsMemoryVM>(*this, *m_TLB);
    
    // Re-initialize TLB with memory pointer now that MipsMemoryVM is created
    if(m_TLB) m_TLB->Initialize(GetMemory(), GetRegisters(), nullptr);

    // Initialize Memory Handlers
    m_Eeprom = std::make_unique<CEeprom>(false);

    // RDRAM
    m_RDRAMInterfaceHandler = std::make_unique<RDRAMInterfaceHandler>(*m_Registers);
    m_RDRAMRegistersHandler = std::make_unique<RDRAMRegistersHandler>(*m_Registers, RDRAMRegistersHandler::Config{});

    // MIPS Interface
    m_MIPSInterfaceHandler = std::make_unique<MIPSYInterfaceHandler>(*m_Registers);

    // SP Registers
    m_SPRegistersHandler = std::make_unique<SRegistersHandler>(*m_Registers);

    // Audio & Video
    m_AudioInterfaceHandler = std::make_unique<AudioInterfaceHandler>(*this, *m_Registers);

    m_DDRegisters.fill(0);
}

CN64System::~CN64System()
{
    LOG_DEBUG( "CN64System destructor");

    // Shutdown and release plugins before anything else
    if (m_PluginManager)
    {
        m_PluginManager->ShutdownPlugins();
        m_PluginManager.reset();
    }

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

    Log::debugf("Loading \"%s\"", FileLoc);
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
        LOG_ERROR(
                  "Exception: {}", e.what());
        delete g_Rom;
        g_Rom = nullptr;
        return false;
    }
}

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

    Log::debugf("Loading \"%s\"", FileLoc);
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

    Log::debugf("Loading \"%s\"", FileLoc);
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

bool CN64System::RunFileImage(const char* FileLoc, uint64_t frameLimit)
{
    LOG_DEBUG("RunFileImage with frameLimit");
    Log::infof("RunFileImage: \"%s\" (frameLimit: %llu)", 
               FileLoc ? FileLoc : "(null)", (unsigned long long)frameLimit);

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

    RunLoadedImage(frameLimit);
    return true;
}

bool CN64System::RunDiskImage(const char* FileLoc)
{
    LOG_DEBUG(
              "RunDiskImage: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (!LoadDiskImage(FileLoc, false))
    {
        return false;
    }

    // Select IPL ROM based on disk country code
    // The N64DD requires a matching IPL ROM (Japanese or US retail/dev)
    if (g_Disk != nullptr)
    {
        // If a DDRom (IPL) isn't already loaded, we need one
        if (g_DDRom == nullptr || !g_DDRom->IsLoaded())
        {
            LOG_WARNING( "No 64DD IPL ROM loaded. N64DD disk requires an IPL ROM.");
            // In a full implementation, this would search known IPL paths
            // or prompt the user. For now, log and continue - will fail at boot.
        }
    }

    RunLoadedImage();
    return true;
}

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

    // IPL ROM is loaded as part of the combo - the ROM file serves as both
    // the game ROM and provides the IPL boot sequence for 64DD

    RunLoadedImage();
    return true;
}

void CN64System::RunLoadedImage(uint64_t frameLimit)
{
    LOG_DEBUG("RunLoadedImage with frameLimit");
    Log::infof("RunLoadedImage (frameLimit: %llu)", (unsigned long long)frameLimit);

    g_BaseSystem = new CN64System((uint32_t)time(nullptr), false, false);
    if (g_BaseSystem)
    {
        // Initialize memory (allocates RDRAM, creates memory maps)
        if (!g_BaseSystem->m_Memory->Initialize())
        {
            LOG_ERROR("Failed to initialize memory system");
            delete g_BaseSystem;
            g_BaseSystem = nullptr;
            return;
        }
        Log::info("Memory system initialized (8MB RDRAM)");

        // Set frame limit if specified
        if (frameLimit > 0)
        {
            g_BaseSystem->SetFrameLimit(frameLimit);
            Log::infof("Frame limit applied: %llu frames", (unsigned long long)frameLimit);
        }
        
        // Apply target FPS
        g_BaseSystem->SetTargetFPS(s_TargetFPS);
        Log::infof("Frame rate limiter: %d FPS%s", s_TargetFPS, 
                   s_TargetFPS == 0 ? " (unlimited)" : "");

        // Reset system state: copies boot code to DMEM, sets post-PIF registers
        g_BaseSystem->Reset(true, false);
        Log::infof("System reset complete. PC = 0x%08X", 
                   (uint32_t)g_BaseSystem->m_Registers->m_PROGRAM_COUNTER);

        // If window was initialized, run in interactive mode; otherwise headless
        if (g_BaseSystem->m_WindowManager && g_BaseSystem->m_WindowManager->IsInitialized())
        {
            LOG_DEBUG("Running in interactive mode (window available)");
            g_BaseSystem->RunInteractiveLoop();
        }
        else
        {
            LOG_DEBUG("Starting emulation in headless mode (blocking on current thread)");
            // Run CPU on current thread (blocks until emulation ends or frame limit reached)
            g_BaseSystem->StartEmulation(false);
        }
    }
    else
    {
        LOG_ERROR("Failed to create CN64System");
    }
}

void CN64System::RunInteractiveLoop()
{
    // Start CPU in background thread
    StartEmulation(true);
    
    Log::info("Interactive mode: CPU running in background thread, processing window events");
    
    // Main event loop - process window events and keep running until window closes
    if (m_WindowManager)
    {
        const int EVENT_POLL_INTERVAL_MS = 16;  // ~60 Hz
        
        while (m_WindowManager->ProcessEvents())
        {
            // Window is still open and valid
#ifdef _WIN32
            Sleep(EVENT_POLL_INTERVAL_MS);
#else
            usleep(EVENT_POLL_INTERVAL_MS * 1000);
#endif
            
            // Check if CPU emulation has ended
            if (m_EndEmulation)
            {
                Log::info("CPU emulation has ended");
                break;
            }
        }
        
        Log::info("Window closed or quit requested, stopping emulation");
        
        // Stop the CPU thread if still running
        EndEmulation();
        CloseCpu();
    }
}

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

void CN64System::CloseCpu()
{
    LOG_DEBUG( "CloseCpu - Start");

    if (m_thread == nullptr)
    {
        return;
    }

    LOG_DEBUG( "Setting end emulation");
    m_EndEmulation = true;

    // If paused, queue resume so the CPU loop can exit
    if (m_SystemEvents)
    {
        m_SystemEvents->QueueEvent(SysEvent_CloseCPU);
    }

    CThread* hThread = m_thread;
    m_thread = nullptr;

    if (hThread != nullptr)
    {
        // Wait for thread to finish with a timeout
        // CThread doesn't have a join, so we poll IsRunning with a sleep loop
        constexpr int MAX_WAIT_MS = 5000;
        constexpr int POLL_INTERVAL_MS = 10;
        int waited = 0;
        while (hThread->IsRunning() && waited < MAX_WAIT_MS)
        {
#ifdef _WIN32
            Sleep(POLL_INTERVAL_MS);
#else
            usleep(POLL_INTERVAL_MS * 1000);
#endif
            waited += POLL_INTERVAL_MS;
        }

        if (hThread->IsRunning())
        {
            LOG_WARNING( "Thread did not stop in time, terminating");
            hThread->Terminate();
        }

        delete hThread;
    }

    LOG_DEBUG( "CloseCpu - Done");
}

void CN64System::StartEmulation(bool NewThread)
{
    LOG_DEBUG(
              "StartEmulation (NewThread: {})", NewThread ? "true" : "false");

    StartEmulation2(NewThread);
}

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
        // Execute CPU directly on current thread
        LOG_DEBUG( "Setting system as active (non-threaded)");
        SetActiveSystem();
        LOG_DEBUG( "Start executing CPU");
        ExecuteCPU();
    }
}

#ifdef _WIN32
static uint32_t StartThreadWrapper(void* param)
#else
static void* StartThreadWrapper(void* param)
#endif
{
    if (!param) return 0;
    CN64System* pSystem = static_cast<CN64System*>(param);
    pSystem->ExecuteCPU();
    return 0;
}

void CN64System::StartEmulationThread()
{
    LOG_DEBUG( "StartEmulationThread");

    if (m_thread != nullptr)
    {
        LOG_WARNING("Thread already running. Stopping previous execution.");
        CloseCpu();
    }

    // Create and start thread
    m_thread = new CThread(reinterpret_cast<CThread::CTHREAD_START_ROUTINE>(StartThreadWrapper));
    
    if (m_thread)
    {
        m_thread->Start(this);
    }
    else
    {
        LOG_ERROR("Failed to create emulation thread");
    }
}

void CN64System::EndEmulation(void)
{
    Log::info("EndEmulation called");
    m_EndEmulation = true;
}

void CN64System::Pause()
{
    Log::info("Pause called - setting m_EndEmulation");
    m_EndEmulation = true; 
}

void CN64System::GameReset()
{
    LOG_DEBUG( "GameReset");
    Reset(true, false);
}

void CN64System::Reset(bool bInitReg, bool ClearMemory)
{
    Log::debugf("Reset (InitReg: %s, ClearMemory: %s)",
                bInitReg ? "true" : "false", ClearMemory ? "true" : "false");

    m_InReset = true;
    m_EndEmulation = false; // Reset stop flag if set
    
    m_CyclesToSkip = 0;
    m_SyncCount = 0;
    
    // Reset Memory
    if (m_Memory)
    {
        m_Memory->Reset(ClearMemory);
        
        // Copy boot code (PIF replacement)
        if (GetRom() && GetRom()->IsLoaded())
        {
            auto bootCode = GetRom()->GetBootCode();
            if (!bootCode.empty())
            {
                // Copy boot code to SP DMEM (0xA4000040)
                uint32_t destAddr = 0xA4000040;
                // Since MipsMemoryVM::Dmem() returns a pointer, we don't need UpdateMemoryValue8 
                // IF we trust the mapping.
                // However, UpdateMemoryValue8 is "virtual" write.
                // But Dmem() is direct pointer access. 
                // Let's use Dmem() pointer arithmetic as it's cleaner for block copy.
                uint8_t* dmem = m_Memory->Dmem();
                if (dmem)
                {
                   // The PIF executes bootcode which copies 0x40-0x1000 from ROM to SP DMEM 0x40.
                   // dmem points to 0xA4000000.
                   // ROM data is in big-endian (.z64) format. Memory is in host byte order
                   // (each 32-bit word byte-swapped on little-endian hosts).
                   // We must byte-swap each 32-bit word during the copy.
                   size_t copySize = (bootCode.size <= 0xFC0) ? bootCode.size : 0xFC0;
                   const uint8_t* src = bootCode.begin();
                   uint8_t* dst = dmem + 0x40;
                   // ROM data is already in host (LE) word order after DetectFormatAndByteSwap.
                   // DMEM also uses host word order for *(uint32_t*) access.
                   std::memcpy(dst, src, copySize);
                   // Clear the OSTask area (DMEM 0xFC0-0xFFF) to prevent bootcode data
                   // from being misinterpreted as a valid RSP task by HLE dispatch.
                   // The bootcode fills DMEM[0x40..0xFFF] but we never run IPL3 in HLE boot,
                   // and stale data at 0xFC0 (OSTask.type) causes false task detection.
                   std::memset(dmem + 0xFC0, 0, 0x40);
                   Log::infof("Copied %zu bytes of boot code to SP DMEM (cleared OSTask area)", copySize);
                }
            }
        }
    }
    
    // HLE Boot: Copy ROM code to RDRAM at the entry point physical offset
    // (matching what the boot code's PI DMA would do)
    uint32_t entryPoint = 0x80000400; // default
    if (GetRom() && GetRom()->IsLoaded() && m_Memory)
    {
        const uint8_t* romData = GetRom()->GetRomAddress();
        uint32_t romSize = static_cast<uint32_t>(GetRom()->GetSize());
        uint8_t* rdram = m_Memory->Rdram();
        uint32_t rdramSize = m_Memory->RdramSize();

        // Read entry point from ROM header (offset 0x08)
        if (romSize >= 0x0C)
        {
            entryPoint = *reinterpret_cast<const uint32_t*>(romData + 0x08);
            Log::infof("HLE Boot: ROM entry point = 0x%08X", entryPoint);
        }

        // The boot code DMAs from ROM offset 0x1000 to RDRAM at entry_point_physical
        uint32_t dramBase = entryPoint & 0x1FFFFFFF; // physical address of entry point
        uint32_t romOffset = 0x1000;
        uint32_t dmaLen = 0x100000; // 1MB
        if (romOffset + dmaLen > romSize)
            dmaLen = romSize - romOffset;
        if (dramBase + dmaLen > rdramSize)
            dmaLen = rdramSize - dramBase;

        if (rdram && romData && dmaLen > 0)
        {
            std::memcpy(rdram + dramBase, romData + romOffset, dmaLen);
            Log::infof("HLE Boot: Copied 0x%X bytes from ROM[0x%X] to RDRAM[0x%X]",
                       dmaLen, romOffset, dramBase);
        }

        // Write RDRAM size to CIC-expected location
        // CIC-6105 uses offset 0x03F0; CIC-6102/6101/6103 use 0x0318
        if (rdramSize > 0 && rdram)
        {
            uint32_t offset = 0x03F0; // CIC-6105 (OoT, Majora's Mask, etc.)
            *reinterpret_cast<uint32_t*>(rdram + offset) = rdramSize;
            // Also write to 6102 offset for compatibility
            *reinterpret_cast<uint32_t*>(rdram + 0x0318) = rdramSize;
            Log::infof("HLE Boot: Wrote RDRAM size 0x%08X to offsets 0x03F0 and 0x0318", rdramSize);
        }

        // Write PIF boot values to RDRAM (matching what the real PIF ROM writes)
        // These values are read by the N64 OS __osInitialize to configure the system.
        if (rdram && rdramSize >= 0x400)
        {
            // 0x0300: osTvType — 0=PAL, 1=NTSC, 2=MPAL
            uint32_t tvType = 1; // Default NTSC
            if (GetRom() && GetRom()->IsLoaded()) {
                if (GetRom()->GetImage().IsPal()) {
                    tvType = 0; // PAL
                } else {
                    tvType = 1; // NTSC
                }
            }
            *reinterpret_cast<uint32_t*>(rdram + 0x0300) = tvType;

            // 0x0304: osRomType — 0=cartridge, 1=64DD
            *reinterpret_cast<uint32_t*>(rdram + 0x0304) = 0; // cartridge

            // 0x0308: osRomBase — physical base address of ROM
            *reinterpret_cast<uint32_t*>(rdram + 0x0308) = 0xB0000000; // ROM base (KSEG1 + 0x10000000)

            // 0x030C: osResetType — 0=cold reset, 1=warm reset (NMI)
            *reinterpret_cast<uint32_t*>(rdram + 0x030C) = 0; // cold reset

            // 0x0310: osCicId — CIC chip identifier
            *reinterpret_cast<uint32_t*>(rdram + 0x0310) = 6105; // CIC-6105

            // 0x0314: osVersion — IPL3 version
            *reinterpret_cast<uint32_t*>(rdram + 0x0314) = 0;

            Log::infof("HLE Boot: Wrote PIF boot values: osTvType=%d osRomBase=0xB0000000 osCicId=6105",
                       tvType);
        }

        // NOTE: CIC-6105 IMEM bootstrap is NOT needed for HLE boot.
        // HLE boot skips IPL3 entirely (PC goes directly to entryPoint=0x80000400).
        // The bootstrap code at IMEM 0xA4001000 is only used by IPL3's CRC jump.
        // Leaving IMEM clean allows non_task_dispatching() to correctly detect
        // CIC x105 RSP tasks via IMEM checksum when the game loads them.
    }

    // Reset Registers
    if (bInitReg && m_Registers && m_Memory)
    {
        m_Registers->Reset(bInitReg, *m_Memory);
    }

    // HLE Boot: Skip boot code and CRC check, jump directly to game entry point.
    // The boot code would set up caches and PI domain timing, but for emulation
    // these are not critical. The important thing is that RDRAM has the game code
    // and registers are set to post-PIF-boot values.
    if (m_Registers)
    {
        m_Registers->m_PROGRAM_COUNTER = entryPoint;
        Log::infof("HLE Boot: Set PC to entry point 0x%08X (skipping boot code + CRC)", entryPoint);
    }

    // Reset components
    if (m_SystemTimer) m_SystemTimer->reset();
    if (m_SystemEvents) m_SystemEvents->DeleteAllEvents();
    if (m_TLB) m_TLB->Initialize(m_Memory.get(), m_Registers.get(), nullptr);
    if (m_MIPSInterfaceHandler) m_MIPSInterfaceHandler->Reset();

    // Set up initial VI timer (NTSC ~60Hz: 93,750,000 / 60 â‰ 1,562,500 cycles per frame)
    if (m_SystemTimer)
    {
        // P64 NTSC: ViTimerPeriod = ViTimerIncrement * CountPerOp = 781,250 * 2 = 1,562,500.
        // NextTimer decrements by CountPerOp per instruction, so this fires every
        // 1,562,500 / CountPerOp = 781,250 instructions per frame.
        // COUNT_REGISTER += elapsed gives 1,562,500 Count ticks per frame (same as P64).
        m_SystemTimer->set_timer_periodic(N64System::TimerType::ViTimer, 781250 * CountPerOp());
        // Set up initial Compare timer based on Count/Compare registers
        m_SystemTimer->update_compare_timer();
        Log::debug("VI timer and Compare timer initialized");
    }

    if (m_SyncCPU)
    {
        m_SyncCPU->Reset(bInitReg, ClearMemory);
    }

    // ──────────────────────────────────────────
    // Initialize RSP subsystem
    // Wire hardware register pointers so the RSP
    // can read/write SP, MI, and DPC registers
    // ──────────────────────────────────────────
    // Use Vmem's SP handler — it's the one that receives actual game writes
    // (DMA, SP_STATUS, etc). MainSYS's m_SPRegistersHandler is a separate
    // instance whose DMEM/IMEM never get game data.
    auto* spHandler = m_Memory ? m_Memory->SPRegisters() : nullptr;
    if (spHandler && m_Registers && m_Memory)
    {
        // One-time RSP plugin init (sets up settings, debugger)
        Nova64::RSP::RspPluginLoaded();

        // Populate RSP_INFO with pointers to actual hardware registers
        Nova64::RSP::RSP_INFO rspInfo{};

        // ROM header (used for game identification in HLE)
        rspInfo.HEADER = (GetRom() && GetRom()->IsLoaded())
                       ? const_cast<uint8_t*>(GetRom()->GetRomAddress())
                       : nullptr;

        // Main memory
        rspInfo.RDRAM    = m_Memory->Rdram();
        rspInfo.RdramSize = m_Memory->RdramSize();

        // RSP local memory — from Vmem's handler (receives game DMA)
        rspInfo.DMEM = spHandler->Dmem();
        rspInfo.IMEM = spHandler->Imem();

        // MI interrupt register (index 2 = MI_INTR_REG)
        rspInfo.MI_INTR_REG = &m_Registers->m_Mips_Interface[2];

        // SP registers — from Vmem's handler (game reads/writes these)
        rspInfo.SP_MEM_ADDR_REG  = &spHandler->m_SPRegs[0];
        rspInfo.SP_DRAM_ADDR_REG = &spHandler->m_SPRegs[1];
        rspInfo.SP_RD_LEN_REG    = &spHandler->m_SPRegs[2];
        rspInfo.SP_WR_LEN_REG    = &spHandler->m_SPRegs[3];
        rspInfo.SP_STATUS_REG    = &spHandler->m_SPRegs[4];
        rspInfo.SP_DMA_FULL_REG  = &spHandler->m_SPRegs[5];
        rspInfo.SP_DMA_BUSY_REG  = &spHandler->m_SPRegs[6];
        rspInfo.SP_SEMAPHORE_REG = &spHandler->m_SPRegs[7];
        rspInfo.SP_PC_REG        = &spHandler->m_SPRegs[8];

        // DPC registers (Display Processor Command)
        rspInfo.DPC_START_REG    = &m_Registers->m_Display_ControlReg[0];
        rspInfo.DPC_END_REG      = &m_Registers->m_Display_ControlReg[1];
        rspInfo.DPC_CURRENT_REG  = &m_Registers->m_Display_ControlReg[2];
        rspInfo.DPC_STATUS_REG   = &m_Registers->m_Display_ControlReg[3];
        rspInfo.DPC_CLOCK_REG    = &m_Registers->m_Display_ControlReg[4];
        rspInfo.DPC_BUFBUSY_REG  = &m_Registers->m_Display_ControlReg[5];
        rspInfo.DPC_PIPEBUSY_REG = &m_Registers->m_Display_ControlReg[6];
        rspInfo.DPC_TMEM_REG     = &m_Registers->m_Display_ControlReg[7];

        // Callback functions
        rspInfo.CheckInterrupts = RSP_CheckInterrupts;
        rspInfo.ProcessDList    = RSP_ProcessDList;
        rspInfo.ProcessRdpList  = RSP_ProcessRdpList;

        // Initialize RSP: bind pointers + build dispatch tables
        Nova64::RSP::InitializeRSP(rspInfo);

        // Per-game RSP setup (allocates recompiler buffers if needed)
        Nova64::RSP::RspRomOpened();

        Log::info("RSP subsystem initialized and wired to hardware registers");
    }

    // ──────────────────────────────────────────────────────────────────
    // Initialize graphics window BEFORE plugins (plugins need it)
    // Only create on first call — warm resets must not destroy the window
    // or plugin, as that would lose all rendering state.
    // ──────────────────────────────────────────────────────────────────
    if (!m_WindowManager) {
        m_WindowManager = std::make_unique<Nova64::Video::WindowManager>();
        Nova64::Video::WindowConfig windowConfig;
        windowConfig.title = "SuperNova64 - Emulator";
        windowConfig.width = s_WindowWidth;
        windowConfig.height = s_WindowHeight;
        windowConfig.posX = -1;  // center
        windowConfig.posY = -1;
        windowConfig.vsync = false;  // Software frame pacing handles timing
        windowConfig.glMajor = 4;
        windowConfig.glMinor = 6;
        
        if (m_WindowManager->Initialize(windowConfig)) {
            Log::info("Graphics window initialized successfully");
        } else {
            Log::warning("Failed to initialize graphics window (headless mode)");
        }
    }

    bool windowReady = m_WindowManager && m_WindowManager->IsInitialized();

    // ──────────────────────────────────────────
    // Initialize PluginManager and load plugins
    // Only create once — subsequent Reset() calls should reset the
    // existing plugin, not destroy and recreate it.
    // ──────────────────────────────────────────
    if (!m_PluginManager) {
        m_PluginManager = std::make_unique<Plugins::PluginManager>(*this);
        m_PluginManager->LoadPlugin(Plugins::PluginType::Graphics);
        m_PluginManager->LoadPlugin(Plugins::PluginType::Audio);
        m_PluginManager->InitializePlugins();
    
        // Initialize the graphics plugin display with the window handle
        if (windowReady) {
            auto* gfxPlugin = m_PluginManager->GetGraphicsPlugin();
            if (gfxPlugin) {
                Plugins::RenderSettings settings;
                settings.WindowWidth = s_WindowWidth;
                settings.WindowHeight = s_WindowHeight;
                settings.AntiAliasing = false;
                settings.Fullscreen = false;
                
                if (gfxPlugin->InitializeDisplay(m_WindowManager->GetNativeHandle(), settings)) {
                    Log::info("Graphics plugin display initialized with window handle");
                } else {
                    Log::warning("Graphics plugin display initialization failed");
                }
            }

            // Release GL context from the main thread so the CPU thread can use it
            // (UpdateScreen/ProcessDList run on the CPU thread and need the context)
            m_WindowManager->ReleaseContext();
        }
    
        Log::info("PluginManager created and plugins initialized");
    } else {
        // Plugin already exists — notify it of the warm reset
        m_PluginManager->ResetPlugins();
    }

    m_InReset = false;
}

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

void CN64System::ExecuteCPU()
{
    LOG_DEBUG( "ExecuteCPU - Start");

    m_EndEmulation = false;

    // Determine CPU type from settings
    N64Types::CpuType cpuType = N64Types::CpuType::Recompiler;

    switch (cpuType)
    {
    case N64Types::CpuType::Recompiler:
        ExecuteRecompiler();
        break;
    case N64Types::CpuType::SyncCores:
        ExecuteSyncCPU();
        break;
    case N64Types::CpuType::Interpreter:
    default:
        ExecuteInterpret();
        break;
    }

    CpuStopped();

    LOG_DEBUG( "ExecuteCPU - Done");
}

void CN64System::ExecuteInterpret()
{
    LOG_DEBUG( "ExecuteInterpret");
    Nova64::Interpreter::R4300iInterpreter interpreter(*this);
    interpreter.ExecuteCPU();
}

void CN64System::ExecuteSingleOp()
{
    Nova64::Interpreter::R4300iInterpreter interpreter(*this);
    interpreter.ExecuteOps(CountPerOp());
}

void CN64System::ExecuteRecompiler()
{
    LOG_DEBUG( "ExecuteRecompiler");
    Nova64::Recompiler::CRecompiler recompiler(*this, m_EndEmulation);
    recompiler.Run();
}

void CN64System::ExecuteSyncCPU()
{
    LOG_DEBUG( "ExecuteSyncCPU");
    // Sync CPU mode validates recompiler output against interpreter results.
    // Full lockstep comparison requires single-step hooks in the recompiler;
    // for now fall back to interpreter execution which is always correct.
    LOG_WARNING("SyncCPU: lockstep comparison not yet available, falling back to interpreter");
    ExecuteInterpret();
}

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

bool CN64System::IsFrameLimitReached() const
{
    if (m_MaxFrames == 0)
    {
        return false; // No limit
    }

    uint64_t currentFrames = GetCurrentFrameCount();
    if (currentFrames >= m_MaxFrames)
    {
        return true;
    }

    return false;
}

uint64_t CN64System::GetCurrentFrameCount() const
{
    if (m_Memory)
    {
        auto* vi = m_Memory->VideoInterface();
        return vi->get_statistics().total_frames;
    }
    return 0;
}

void CN64System::DelayedJump(uint64_t target)
{
    m_JumpToLocation = target;
    m_PipelineStage = N64Types::PipelineStage::DelaySlot;
}

void CN64System::DelayedRelativeJump(int32_t offset)
{
    m_JumpToLocation = m_Registers->m_PROGRAM_COUNTER + offset;

    // Permanent loop detection: a branch whose target equals
    // its own address is an idle/wait loop.  Fast-forward timers instead of
    // spinning millions of iterations burning host CPU.
    if (offset == 0)
    {
        m_PipelineStage = N64Types::PipelineStage::PermanentLoopDoDelay;
    }
    else
    {
        m_PipelineStage = N64Types::PipelineStage::DelaySlot;
    }
}

void CN64System::ExternalEvent(SystemEvent action)
{
    Log::debugf("ExternalEvent: %d", static_cast<int>(action));

    switch (action)
    {
    case SysEvent_CloseCPU:
        Log::info("ExternalEvent: SysEvent_CloseCPU -> ending emulation");
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

void CN64System::RefreshScreen()
{
    // Called when the VI timer fires (once per frame, ~60Hz NTSC / ~50Hz PAL)
    
    uint64_t frameNum = GetCurrentFrameCount();
    
    // Auto-frameskip: check if we're behind schedule
    bool shouldRender = true;
    if (m_TargetFPS > 0 && m_FramePaceInitialized)
    {
        using clock = std::chrono::steady_clock;
        using namespace std::chrono;
        auto targetDuration = duration_cast<nanoseconds>(
            duration<double>(1.0 / m_TargetFPS));
        auto now = clock::now();
        auto remaining = (m_FramePaceStart + targetDuration) - now;
        
        // If we're already behind and haven't hit max skip, skip this render
        if (remaining < nanoseconds(0) && m_FrameSkipCount < m_MaxFrameSkip)
        {
            shouldRender = false;
            m_FrameSkipCount++;
        }
        else
        {
            m_FrameSkipCount = 0;
        }
    }
    
    // Update display via GFX plugin (skip if behind schedule)
    if (shouldRender && m_PluginManager)
    {
        auto* gfx = m_PluginManager->GetGraphicsPlugin();
        if (gfx) { gfx->UpdateScreen(); }
    }
    

    
    // Feed audio device to keep playback smooth (always, even on skipped frames)
    if (m_AudioInterfaceHandler)
    {
        m_AudioInterfaceHandler->AudioOutput().FeedDevice();
    }

    // Frame rate limiter: pace CPU execution to real time
    if (m_TargetFPS > 0)
    {
        using clock = std::chrono::steady_clock;
        using namespace std::chrono;

        auto targetDuration = duration_cast<nanoseconds>(
            duration<double>(1.0 / m_TargetFPS));

        if (!m_FramePaceInitialized)
        {
            m_FramePaceStart = clock::now();
            m_FramePaceInitialized = true;
        }
        else
        {
            // Advance target by one frame interval
            m_FramePaceStart += targetDuration;
            auto now = clock::now();
            auto remaining = m_FramePaceStart - now;

            // If we're more than 2 frames behind, reset to avoid spiral
            if (remaining < -targetDuration)
            {
                m_FramePaceStart = now;
            }
            else if (remaining > nanoseconds(0))
            {
                // Sleep for most of the remaining time, then spin-wait
                if (remaining > microseconds(2000))
                {
                    std::this_thread::sleep_for(remaining - microseconds(1500));
                }
                // Spin-wait for the last portion for accuracy
                while (clock::now() < m_FramePaceStart)
                {
                    // busy-wait
                }
            }
            // else: we're behind but within tolerance, skip sleep to catch up
        }
    }

    m_bCleanFrameBox = false;
}

bool CN64System::LoadState()
{
    LOG_DEBUG( "LoadState (default slot)");
    
    // Build default save state path from ROM name
    if (!g_Rom || !g_Rom->IsLoaded())
    {
        LOG_ERROR( "LoadState: No ROM loaded");
        return false;
    }
    
    std::string savePath = "Saves";
    std::string fileName = g_Rom->GetImage().GetRomName() + ".pj.zip";
    // Remove invalid characters from filename
    for (char& c : fileName)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    
    CPath fullPath(savePath);
    fullPath.AppendDirectory(fileName);
    return LoadState(fullPath.GetString().c_str());
}

bool CN64System::LoadState(const char* FileName)
{
    Log::debugf("LoadState: \"%s\"", FileName ? FileName : "(null)");
    
    if (FileName == nullptr || FileName[0] == '\0')
    {
        LOG_ERROR( "LoadState: Invalid filename");
        return false;
    }
    
    // Open save state file
    std::ifstream file(FileName, std::ios::binary);
    if (!file.is_open())
    {
        Log::errorf("LoadState: Could not open file \"%s\"", FileName);
        return false;
    }
    
    // Read and validate header
    char magic[4];
    file.read(magic, 4);
    if (magic[0] != 'N' || magic[1] != '6' || magic[2] != '4' || magic[3] != 'S')
    {
        LOG_ERROR( "LoadState: Invalid save state header");
        return false;
    }
    
    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1)
    {
        Log::errorf("LoadState: Unsupported save state version %u", version);
        return false;
    }
    
    // Read system state
    file.read(reinterpret_cast<char*>(&m_NextTimer), sizeof(m_NextTimer));
    
    // Read registers
    if (m_Registers)
    {
        // GPR (32 x 64-bit)
        file.read(reinterpret_cast<char*>(m_Registers->m_GPR.data()), sizeof(uint64_t) * 32);
        // FPR (32 x 64-bit)
        file.read(reinterpret_cast<char*>(m_Registers->m_FPR.data()), sizeof(uint64_t) * 32);
        // CP0 (32 x 64-bit)
        file.read(reinterpret_cast<char*>(m_Registers->m_CP0.data()), sizeof(uint64_t) * 32);
        // Special registers (HI, LO, PC)
        file.read(reinterpret_cast<char*>(&m_Registers->m_HI), sizeof(uint64_t));
        file.read(reinterpret_cast<char*>(&m_Registers->m_LO), sizeof(uint64_t));
        file.read(reinterpret_cast<char*>(&m_Registers->m_PROGRAM_COUNTER), sizeof(uint64_t));
    }
    
    // Read RDRAM (4MB or 8MB)
    if (m_Memory)
    {
        uint32_t rdramSize;
        file.read(reinterpret_cast<char*>(&rdramSize), sizeof(rdramSize));
        uint8_t* rdram = m_Memory->Rdram();
        if (rdram && rdramSize <= 0x800000)
        {
            file.read(reinterpret_cast<char*>(rdram), rdramSize);
        }
    }
    
    if (!file.good())
    {
        LOG_ERROR( "LoadState: Error reading save state");
        return false;
    }
    
    Log::infof("State loaded from \"%s\"", FileName);
    return true;
}

bool CN64System::SaveState()
{
    LOG_DEBUG( "SaveState (default slot)");
    
    if (!g_Rom || !g_Rom->IsLoaded())
    {
        LOG_ERROR( "SaveState: No ROM loaded");
        return false;
    }
    
    std::string savePath = "Saves";
    std::string fileName = g_Rom->GetImage().GetRomName() + ".pj.zip";
    for (char& c : fileName)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    
    CPath fullPath(savePath);
    fullPath.AppendDirectory(fileName);
    return SaveState(fullPath.GetString().c_str());
}

bool CN64System::SaveState(const char* FileName)
{
    LOG_DEBUG(
              "SaveState: \"{}\"", FileName ? FileName : "(null)");
    
    if (FileName == nullptr || FileName[0] == '\0')
    {
        LOG_ERROR( "SaveState: Invalid filename");
        return false;
    }
    
    // Create save directory if needed
    {
        std::filesystem::path filePath(FileName);
        auto parentDir = filePath.parent_path();
        if (!parentDir.empty())
        {
            std::filesystem::create_directories(parentDir);
        }
    }
    
    // Open save state file
    std::ofstream file(FileName, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Log::errorf("SaveState: Could not create file \"%s\"", FileName);
        return false;
    }
    
    // Write header
    const char magic[4] = {'N', '6', '4', 'S'};
    file.write(magic, 4);
    uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // Write system state
    file.write(reinterpret_cast<const char*>(&m_NextTimer), sizeof(m_NextTimer));
    
    // Write registers
    if (m_Registers)
    {
        file.write(reinterpret_cast<const char*>(m_Registers->m_GPR.data()), sizeof(uint64_t) * 32);
        file.write(reinterpret_cast<const char*>(m_Registers->m_FPR.data()), sizeof(uint64_t) * 32);
        file.write(reinterpret_cast<const char*>(m_Registers->m_CP0.data()), sizeof(uint64_t) * 32);
        file.write(reinterpret_cast<const char*>(&m_Registers->m_HI), sizeof(uint64_t));
        file.write(reinterpret_cast<const char*>(&m_Registers->m_LO), sizeof(uint64_t));
        file.write(reinterpret_cast<const char*>(&m_Registers->m_PROGRAM_COUNTER), sizeof(uint64_t));
    }
    
    // Write RDRAM
    if (m_Memory)
    {
        uint32_t rdramSize = m_Memory->RdramSize();
        file.write(reinterpret_cast<const char*>(&rdramSize), sizeof(rdramSize));
        uint8_t* rdram = m_Memory->Rdram();
        if (rdram)
        {
            file.write(reinterpret_cast<const char*>(rdram), rdramSize);
        }
    }
    
    if (!file.good())
    {
        LOG_ERROR( "SaveState: Error writing save state");
        return false;
    }
    
    Log::infof("State saved to \"%s\"", FileName);
    return true;
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

N64System::SystemTimer* CN64System::GetSystemTimer() const
{
    return m_SystemTimer.get();
}

N64Rom* CN64System::GetRom() const
{
    return g_Rom;
}

N64Rom* CN64System::GetDDRom() const
{
    return g_DDRom;
}

N64Disk* CN64System::GetDisk() const
{
    return g_Disk;
}

