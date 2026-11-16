/**
 * @file MainSYS.cpp
 * @brief Core N64 system emulation: initialization, ROM loading, CPU dispatch, and frame pacing.
 */
#include "../common/platform.hpp"
#include <cstdio>
#include "../common/trace.hpp"
#include "../common/log.hpp"
#include "../common/file.hpp"
#include "../common/md5.hpp"
#include "../common/MemoryManagment.hpp"
#include "../common/Thread.hpp"

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>
#include <immintrin.h>
#endif

#include "MainSYS.hpp"
#include "Types.hpp"
#include "N64Rom.hpp"
#include "StateTrace.hpp"
#include "SaveState.hpp"
#include "N64DD.hpp"
#include "MIPSY/Systiming.hpp"
#include "MIPSY/Register.hpp"
#include "MIPSY/Vmem.hpp"
#include "MIPSY/TLB.hpp"
#include "MIPSY/Sysevents.hpp"
#include "Interpreter/InterpreterOps.hpp"
#include "Recomp/Recompiler.hpp"
#include "Memoryhandler/PifInterface.hpp"
#include "../NovaRSP/RSPInfo.hpp"
#include "../NovaRSP/CPU/RspSystem.hpp"
#include "../NovaGraphics/Window/WindowManager.hpp"
#include "../NovaGraphics/Plugin/NovaGraphicsGFXPlugin.hpp"
#include "../NovaGraphics/Plugin/FrameCallback.hpp"
#include "../NovaGraphics/Plugin/PluginConfig.hpp"
#include <SDL.h>

#include "../common/IniFile.hpp"

#include <cstring>
#include <cstdlib>
#include <vector>
#include <memory>
#include <ctime>
#include <chrono>
#include <thread>
#include <format>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>

// Using directives for Trace namespace
using Trace::ModuleID;
using Trace::Severity;
using Trace::SourceLocation;

// Using directives for N64 ROM and Disk classes
using N64::Disk::N64Disk;
using N64::Rom::N64Rom;

// Global system instances
CN64System* g_System = nullptr;
CN64System* g_BaseSystem = nullptr;
N64Rom* g_Rom = nullptr;
int CN64System::s_TargetFPS = 60;
std::atomic<bool> CN64System::s_StopRequested{false};
int CN64System::s_WindowWidth = 1280;
int CN64System::s_WindowHeight = 960;
int CN64System::s_InternalResScale = 0;
int CN64System::s_FullscreenMode = 0; // Default: windowed
bool CN64System::s_HeadlessMode = false;
std::string CN64System::s_PipeName = "";

// RSP callback wrappers (static functions matching RSP_INFO signatures)
// These bridge the N64System → RSP interface

static void RSP_CheckInterrupts() {
    if (g_System && g_System->GetRegisters()) {
        g_System->GetRegisters()->CheckInterrupts();
    }
}

static void RSP_ProcessDList() {
    // Called by RSP HLE when a video (GFX) task is detected.
    // Forward to the GFX plugin's ProcessDList.
    // Log::info("RSPTRACE ProcessDList enter");
    if (g_System && g_System->GetPluginManager()) {
        auto* gfx = g_System->GetPluginManager()->GetGraphicsPlugin();
        if (gfx) {
            gfx->ProcessDList(0, 0);
        }
    }
    // Log::info("RSPTRACE ProcessDList exit");

    // Signal DP (Display Processor) completion interrupt.
    // After HLE processes the display list, the RDP is "done".
    // If we're inside ClearHalt's DoRspCycles, defer the interrupt bits
    // so the game code can return and update scheduler state first.
    if (g_System && g_System->GetRegisters()) {
        auto* regs = g_System->GetRegisters();
        if (regs->m_defer_hle_interrupts) {
            // Accumulate DP interrupt for deferred delivery via RspTimer
            regs->m_deferred_mi_intr |= 0x20;  // MI_INTR_DP
        } else {
            regs->m_GfxIntrReg |= 0x20;        // MI_INTR_DP
            regs->m_Mips_Interface[2] |= 0x20; // MI_INTR_REG |= DP bit
            regs->CheckInterrupts();
        }
    }
}

static void RSP_ProcessRdpList() {
    // Called by RSP HLE when raw RDP commands are submitted.
    // Forward to the GFX plugin.
    if (g_System && g_System->GetPluginManager()) {
        auto* gfx = g_System->GetPluginManager()->GetGraphicsPlugin();
        if (gfx) {
            gfx->UpdateRDPStatus();
        }
    }
}

static void RSP_ProcessDListShadow() {
    // Called by shadow HLE when LLE is active.
    // Runs the GBI display list in geometry-only mode (no RDP output)
    // so the RT layer can harvest world-space triangles for ray tracing.
    if (g_System && g_System->GetPluginManager()) {
        auto* gfx = g_System->GetPluginManager()->GetGraphicsPlugin();
        if (gfx) {
            gfx->ProcessDListShadow(0, 0);
        }
    }
}
N64Rom* g_DDRom = nullptr;
N64Disk* g_Disk = nullptr;

using SAVE_CHIP_TYPE = N64Types::SaveChipType;
constexpr SAVE_CHIP_TYPE SAVE_CHIP_UNKNOWN = SAVE_CHIP_TYPE::Auto;

// Helper macro for logging - uses existing TRACE_* macros
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_ERROR
#define LOG_DEBUG(...)   TRACE_DEBUG(Trace::Module::Core, __VA_ARGS__)
#define LOG_INFO(...)    TRACE_INFO(Trace::Module::Core, __VA_ARGS__)
#define LOG_WARNING(...) TRACE_WARN(Trace::Module::Core, __VA_ARGS__)
#define LOG_ERROR(...)   TRACE_ERROR(Trace::Module::Core, __VA_ARGS__)

CN64System::CN64System(uint32_t randomizer_seed, bool SavesReadOnly, bool SyncSystem) :
    m_SaveUsing(SAVE_CHIP_UNKNOWN), m_EndEmulation(false), m_SyncCPU(nullptr), m_InReset(false), m_NextTimer(0),
    m_bCleanFrameBox(true), m_TestTimer(false), m_DeferredCheckInterrupts(false), m_CyclesToSkip(0),
    m_JumpToLocation(0), m_JumpDelayLocation(0), m_TLBLoadAddress(0), m_TLBStoreAddress(0), m_SyncCount(0),
    m_thread(nullptr), m_SyncSystem(SyncSystem), m_Random(randomizer_seed), m_SavesReadOnly(SavesReadOnly),
    m_PipelineStage(N64Types::PipelineStage::Normal), m_SystemTimer(std::make_unique<N64System::SystemTimer>(*this)) {
    Log::debugf("CN64System constructor (seed: %u, ReadOnly: %s, Sync: %s)", randomizer_seed,
                SavesReadOnly ? "true" : "false", SyncSystem ? "true" : "false");

    // Initialize components
    m_SystemEvents = std::make_unique<CSystemEvents>(*this);
    m_TLB = std::make_unique<N64System::CTLB>();
    m_Registers = std::make_unique<CRegisters>(*this, *m_SystemEvents);
    // Wire up TLB with registers first (MipsMemoryVM constructor needs m_TLB.GetRegisters())
    if (m_TLB) m_TLB->Initialize(nullptr, m_Registers.get(), nullptr);

    m_Memory = std::make_unique<Nova64::MipsMemoryVM>(*this, *m_TLB);

    // Re-initialize TLB with memory pointer now that MipsMemoryVM is created
    if (m_TLB) m_TLB->Initialize(GetMemory(), GetRegisters(), nullptr);

    // RDRAM
    m_RDRAMInterfaceHandler = std::make_unique<RDRAMInterfaceHandler>(*m_Registers);
    m_RDRAMRegistersHandler = std::make_unique<RDRAMRegistersHandler>(*m_Registers, RDRAMRegistersHandler::Config{});

    // MIPS Interface
    m_MIPSInterfaceHandler = std::make_unique<MIPSYInterfaceHandler>(*m_Registers);

    // SP Registers
    m_SPRegistersHandler = std::make_unique<SRegistersHandler>(*m_Registers);

    m_DDRegisters.fill(0);
}

CN64System::~CN64System() {
    LOG_DEBUG("CN64System destructor");

#ifdef _WIN32
    if (m_FramePaceInitialized) {
        timeEndPeriod(1);
    }
#endif

    // Shutdown and release plugins before anything else
    if (m_PluginManager) {
        m_PluginManager->ShutdownPlugins();
        m_PluginManager.reset();
    }

    SetActiveSystem(false);

    if (m_SyncCPU) {
        m_SyncCPU->CpuStopped();
        delete m_SyncCPU;
        m_SyncCPU = nullptr;
    }

    if (m_thread != nullptr) {
        LOG_DEBUG("Deleting thread object");
        delete m_thread;
        m_thread = nullptr;
    }
}

bool CN64System::LoadFileImage(const char* FileLoc) {
    LOG_DEBUG("LoadFileImage: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (FileLoc == nullptr || FileLoc[0] == '\0') {
        LOG_ERROR("Invalid file location");
        return false;
    }

    CloseSystem();

    // Mark the ROM as loading
    LOG_DEBUG("Mark ROM as loading");

    // Try to load the passed N64 ROM
    if (g_Rom == nullptr) {
        LOG_DEBUG("Allocating global ROM object");
        g_Rom = new N64Rom();
    } else {
        LOG_DEBUG("Use existing global ROM object");
    }

    Log::debugf("Loading \"%s\"", FileLoc);
    try {
        g_Rom->LoadRom(FileLoc);

        // Save-chip override is applied on the live CN64System instance in RunLoadedImage().

        if (g_Rom->GetImage().IsDDIPL()) {
            // 64DD IPL
            if (g_DDRom == nullptr) {
                g_DDRom = new N64Rom();
            }
            g_DDRom->LoadIPL(FileLoc);
        }

        LOG_DEBUG("ROM loaded successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("LoadN64Image failed (\"{}\")", FileLoc);
        LOG_ERROR("Exception: {}", e.what());
        delete g_Rom;
        g_Rom = nullptr;
        return false;
    }
}

bool CN64System::LoadFileImageIPL(const char* FileLoc) {
    LOG_DEBUG("LoadFileImageIPL: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (FileLoc == nullptr || FileLoc[0] == '\0') {
        LOG_ERROR("Invalid file location");
        return false;
    }

    CloseSystem();

    // Mark the N64DD IPL as loading
    LOG_DEBUG("Mark N64DD IPL as loading");

    // Try to load the passed N64DD IPL
    if (g_DDRom == nullptr) {
        LOG_DEBUG("Allocating global N64DD IPL object");
        g_DDRom = new N64Rom();
    } else {
        LOG_DEBUG("Use existing global N64DD IPL object");
    }

    Log::debugf("Loading \"%s\"", FileLoc);
    try {
        g_DDRom->LoadIPL(FileLoc);
        if (!g_DDRom->GetImage().IsDDIPL()) {
            // If not 64DD IPL then it's wrong
            LOG_ERROR("LoadN64ImageIPL failed - not a 64DD IPL (\"{}\")", FileLoc);
            delete g_DDRom;
            g_DDRom = nullptr;
            return false;
        }

        LOG_DEBUG("64DD IPL loaded successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("LoadN64ImageIPL failed (\"{}\")", FileLoc);
        delete g_DDRom;
        g_DDRom = nullptr;
        return false;
    }
}

bool CN64System::LoadDiskImage(const char* FileLoc, const bool Expansion) {
    Log::debugf("LoadDiskImage: \"%s\" (Expansion: %s)", FileLoc ? FileLoc : "(null)", Expansion ? "true" : "false");

    if (FileLoc == nullptr || FileLoc[0] == '\0') {
        LOG_ERROR("Invalid file location");
        return false;
    }

    CloseSystem();

    // Mark the disk as loading
    LOG_DEBUG("Mark disk as loading");

    // Try to load the passed N64 disk
    if (g_Disk == nullptr) {
        LOG_DEBUG("Allocating global disk object");
        g_Disk = new N64Disk();
    } else {
        LOG_DEBUG("Use existing global disk object");
    }

    Log::debugf("Loading \"%s\"", FileLoc);
    try {
        g_Disk->LoadDisk(FileLoc);
        LOG_DEBUG("Disk loaded successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("LoadDiskImage failed (\"{}\")", FileLoc);
        delete g_Disk;
        g_Disk = nullptr;
        return false;
    }
}

bool CN64System::RunFileImage(const char* FileLoc, uint64_t frameLimit) {
    LOG_DEBUG("RunFileImage with frameLimit");
    Log::infof("RunFileImage: \"%s\" (frameLimit: %llu)", FileLoc ? FileLoc : "(null)", (unsigned long long)frameLimit);

    // NOVA64_STATE_SAVE_AT / NOVA64_STATE_LOAD_AT need the save-state service,
    // which the launcher normally starts; start it here for command-line runs.
    if (FileLoc && (std::getenv("NOVA64_STATE_SAVE_AT") || std::getenv("NOVA64_STATE_LOAD_AT")))
        Nova64::StateService::Instance().BeginSession(FileLoc);

    // Uninitialize g_Disk and g_DDRom to prevent exception when ending emulation
    if (g_Disk != nullptr) {
        g_Disk->UnloadDisk();
        delete g_Disk;
        g_Disk = nullptr;
    }
    if (g_DDRom != nullptr) {
        g_DDRom->UnloadRom();
        delete g_DDRom;
        g_DDRom = nullptr;
    }

    if (!LoadFileImage(FileLoc)) {
        return false;
    }

    RunLoadedImage(frameLimit);
    return true;
}

bool CN64System::RunDiskImage(const char* FileLoc) {
    LOG_DEBUG("RunDiskImage: \"{}\"", FileLoc ? FileLoc : "(null)");

    if (!LoadDiskImage(FileLoc, false)) {
        return false;
    }

    // Select IPL ROM based on disk country code
    // The N64DD requires a matching IPL ROM (Japanese or US retail/dev)
    if (g_Disk != nullptr) {
        // If a DDRom (IPL) isn't already loaded, we need one
        if (g_DDRom == nullptr || !g_DDRom->IsLoaded()) {
            LOG_WARNING("No 64DD IPL ROM loaded. N64DD disk requires an IPL ROM.");
            // In a full implementation, this would search known IPL paths
            // or prompt the user. For now, log and continue - will fail at boot.
        }
    }

    RunLoadedImage();
    return true;
}

bool CN64System::RunDiskComboImage(const char* FileLoc, const char* FileLocDisk) {
    Log::debugf("RunDiskComboImage: ROM=\"%s\" Disk=\"%s\"", FileLoc ? FileLoc : "(null)",
                FileLocDisk ? FileLocDisk : "(null)");

    if (!LoadDiskImage(FileLocDisk, true)) {
        return false;
    }
    if (!LoadFileImage(FileLoc)) {
        return false;
    }

    // IPL ROM is loaded as part of the combo - the ROM file serves as both
    // the game ROM and provides the IPL boot sequence for 64DD

    RunLoadedImage();
    return true;
}

void CN64System::RunLoadedImage(uint64_t frameLimit) {
    LOG_DEBUG("RunLoadedImage with frameLimit");
    Log::infof("RunLoadedImage (frameLimit: %llu)", (unsigned long long)frameLimit);
    if (s_StopRequested.load()) return;

    g_BaseSystem = new CN64System((uint32_t)time(nullptr), false, false);
    if (g_BaseSystem) {
        // Default save chip selection to auto-detect, then apply known-title overrides.
        g_BaseSystem->m_SaveUsing = SAVE_CHIP_UNKNOWN;
        if (g_Rom != nullptr) {
            std::string romName = g_Rom->GetImage().GetRomName();
            std::transform(romName.begin(), romName.end(), romName.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

            // Cart-ID based detection (more reliable than name parsing).
            // Header offset 0x3C is a 4-char ID like "NZSE" (MM US),
            // "NZPE"/"NZSP" (MM PAL), "NZSJ" (MM Japan).
            // Stored byte-swapped (host-word order) so XOR 3 to read.
            char cartId[4] = {0};
            const uint8_t* romData = g_Rom->GetImage().Data();
            size_t romSize = g_Rom->GetImage().Size();
            if (romData != nullptr && romSize >= 0x40) {
                for (int i = 0; i < 4; i++) cartId[i] = static_cast<char>(romData[(0x3C + i) ^ 3]);
            }
            std::string cartIdStr(cartId, 4);
            Log::infof("ROM identification: name='%s' cartId='%s'", romName.c_str(), cartIdStr.c_str());

            // FlashRAM games (Majora's Mask, Pokemon Stadium 2, etc.)
            // MM cart IDs: NZSE (US), NZPE/NZSP (PAL), NZSJ (Japan).
            bool isMM = romName.find("MAJORA") != std::string::npos || cartIdStr.compare(0, 3, "NZS") == 0 ||
                        cartIdStr.compare(0, 3, "NZP") == 0;
            if (isMM) {
                g_BaseSystem->m_SaveUsing = N64Types::SaveChipType::FlashRam;
                Log::info("Save chip override: FlashRAM (Majora's Mask)");
            }
        }

        // Initialize memory (allocates RDRAM, creates memory maps)
        if (!g_BaseSystem->m_Memory->Initialize()) {
            LOG_ERROR("Failed to initialize memory system");
            delete g_BaseSystem;
            g_BaseSystem = nullptr;
            return;
        }
        Log::info("Memory system initialized (8MB RDRAM)");

        // Set frame limit if specified
        if (frameLimit > 0) {
            g_BaseSystem->SetFrameLimit(frameLimit);
            Log::infof("Frame limit applied: %llu frames", (unsigned long long)frameLimit);
        }

        // s_TargetFPS is an NTSC-equivalent speed; SetSpeed converts it to the
        // ROM's field rate.
        g_BaseSystem->SetSpeed(s_TargetFPS);
        Log::infof("Frame rate limiter: %d fields/s%s", g_BaseSystem->GetTargetFPS(),
                   s_TargetFPS == 0 ? " (unlimited)" : "");

        // Reset system state: copies boot code to DMEM, sets post-PIF registers
        g_BaseSystem->Reset(true, false);
        Log::infof("System reset complete. PC = 0x%08X", (uint32_t)g_BaseSystem->m_Registers->m_PROGRAM_COUNTER);

        g_BaseSystem->DumpRegisters();

        // If window was initialized AND we are NOT in embedded mode, run the
        // interactive event loop (polls SDL window events, exits when window closes).
        // In embedded mode the launcher owns the event loop, so we just start the
        // CPU thread and return immediately — the launcher will call CloseSystem().
        if (g_BaseSystem->m_WindowManager && g_BaseSystem->m_WindowManager->IsInitialized() &&
            !Nova64::Video::g_embeddedMode.load(std::memory_order_relaxed)) {
            LOG_DEBUG("Running in interactive mode (window available)");
            g_BaseSystem->RunInteractiveLoop();
        } else {
            LOG_DEBUG("Starting emulation (headless or embedded mode)");
            // Run CPU on background thread; this call returns immediately.
            // The launcher is responsible for calling CloseSystem() when done.
            g_BaseSystem->StartEmulation(true);
            // Block this thread (RunFileImage) until emulation finishes or
            // CloseSystem() is called from the launcher thread.
            while (g_BaseSystem && g_BaseSystem->IsEmulationRunning() && !s_StopRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // Wait for CPU thread to actually finish
            if (g_BaseSystem) {
                g_BaseSystem->CloseCpu();
            }
        }

    } else {
        LOG_ERROR("Failed to create CN64System");
    }
}

void CN64System::RunInteractiveLoop() {
    // Start CPU in background thread
    StartEmulation(true);

    Log::info("Interactive mode: CPU running in background thread, processing window events");

    // Main event loop - process window events and keep running until window closes
    if (m_WindowManager) {
        const int EVENT_POLL_INTERVAL_MS = 16; // ~60 Hz

        while (m_WindowManager->ProcessEvents()) {
            // Window is still open and valid
#ifdef _WIN32
            Sleep(EVENT_POLL_INTERVAL_MS);
#else
            usleep(EVENT_POLL_INTERVAL_MS * 1000);
#endif

            // Check if CPU emulation has ended
            if (m_EndEmulation) {
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

void CN64System::CloseSystem() {
    LOG_DEBUG("CloseSystem");

    if (g_BaseSystem) {
        g_BaseSystem->CloseCpu();
        delete g_BaseSystem;
        g_BaseSystem = nullptr;
    }
}

void CN64System::CloseCpu() {
    LOG_DEBUG("CloseCpu - Start");

    if (m_thread == nullptr) {
        return;
    }

    LOG_DEBUG("Setting end emulation");
    m_EndEmulation = true;

    // If paused, queue resume so the CPU loop can exit
    if (m_SystemEvents) {
        m_SystemEvents->QueueEvent(SysEvent_CloseCPU);
    }

    CThread* hThread = m_thread;
    m_thread = nullptr;

    if (hThread != nullptr) {
        // Wait for thread to finish with a timeout
        // CThread doesn't have a join, so we poll IsRunning with a sleep loop
        constexpr int MAX_WAIT_MS = 5000;
        constexpr int POLL_INTERVAL_MS = 10;
        int waited = 0;
        while (hThread->IsRunning() && waited < MAX_WAIT_MS) {
#ifdef _WIN32
            Sleep(POLL_INTERVAL_MS);
#else
            usleep(POLL_INTERVAL_MS * 1000);
#endif
            waited += POLL_INTERVAL_MS;
        }

        if (hThread->IsRunning()) {
            LOG_WARNING("Thread did not stop in time, terminating");
            hThread->Terminate();
        }

        delete hThread;
    }

    LOG_DEBUG("CloseCpu - Done");
}

void CN64System::StartEmulation(bool NewThread) {
    LOG_DEBUG("StartEmulation (NewThread: {})", NewThread ? "true" : "false");

    StartEmulation2(NewThread);
}

void CN64System::StartEmulation2(bool NewThread) {
    LOG_DEBUG("StartEmulation2 (NewThread: {})", NewThread ? "true" : "false");

    if (NewThread) {
        LOG_DEBUG("Setting system as active");
        bool bSetActive = SetActiveSystem();

        if (bSetActive) {
            LOG_DEBUG("Starting emulation thread");
            StartEmulationThread();
        } else {
            LOG_WARNING("Failed to set system as active");
        }
    } else {
        // Execute CPU directly on current thread
        LOG_DEBUG("Setting system as active (non-threaded)");
        SetActiveSystem();
        LOG_DEBUG("Start executing CPU");
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

void CN64System::StartEmulationThread() {
    LOG_DEBUG("StartEmulationThread");

    if (m_thread != nullptr) {
        LOG_WARNING("Thread already running. Stopping previous execution.");
        CloseCpu();
    }

    // Create and start thread
    m_thread = new CThread(reinterpret_cast<CThread::CTHREAD_START_ROUTINE>(StartThreadWrapper));

    if (m_thread) {
        m_thread->Start(this);
    } else {
        LOG_ERROR("Failed to create emulation thread");
    }
}

void CN64System::EndEmulation(void) {
    Log::info("EndEmulation called");
    m_EndEmulation = true;
}

void CN64System::Pause() {
    Log::info("Pause called - setting m_EndEmulation");
    m_EndEmulation = true;
}

void CN64System::GameReset() {
    LOG_DEBUG("GameReset");
    Reset(true, false);
}

void CN64System::Reset(bool bInitReg, bool ClearMemory) {
    Log::debugf("Reset (InitReg: %s, ClearMemory: %s)", bInitReg ? "true" : "false", ClearMemory ? "true" : "false");

    m_InReset = true;
    m_EndEmulation = false; // Reset stop flag if set

    m_CyclesToSkip = 0;
    m_SyncCount = 0;

    // Reset Memory
    if (m_Memory) {
        m_Memory->Reset(ClearMemory);

        // Copy boot code (PIF replacement)
        if (GetRom() && GetRom()->IsLoaded()) {
            auto bootCode = GetRom()->GetBootCode();
            if (!bootCode.empty()) {
                // Copy boot code to SP DMEM (0xA4000040). Dmem() hands back a
                // direct pointer, so a block copy beats per-byte virtual writes.
                uint8_t* dmem = m_Memory->Dmem();
                if (dmem) {
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

                    // Write IPL3 output values to DMEM[0x00..0x0F].
                    // Real IPL3 stores system info here before jumping to the game.
                    // Libdragon-based ROMs (e.g. n64-systemtest) read these to
                    // determine memory size and ROM layout.
                    uint32_t dmemRdramSize = m_Memory->RdramSize();
                    *reinterpret_cast<uint32_t*>(dmem + 0x00) = dmemRdramSize;
                    *reinterpret_cast<uint32_t*>(dmem + 0x04) = 1; // TV type: NTSC
                    *reinterpret_cast<uint32_t*>(dmem + 0x08) = 0; // Reset type: cold boot
                    // DMEM[0x0C]: upper 16 bits = ROM-to-RAM offset indicator
                    // (0x10 << 8 = 0x1000 = standard ROM program offset)
                    *reinterpret_cast<uint32_t*>(dmem + 0x0C) = 0x00100000;
                    Log::infof("HLE Boot: Wrote RDRAM size 0x%08X to DMEM[0x00]", dmemRdramSize);
                }
            }
        }
    }

    // HLE Boot: Copy ROM code to RDRAM at the entry point physical offset
    // (matching what the boot code's PI DMA would do)
    uint32_t entryPoint = 0x80000400; // default
    bool isLibdragonElf = false;
    if (GetRom() && GetRom()->IsLoaded() && m_Memory) {
        const uint8_t* romData = GetRom()->GetRomAddress();
        uint32_t romSize = static_cast<uint32_t>(GetRom()->GetSize());
        uint8_t* rdram = m_Memory->Rdram();
        uint32_t rdramSize = m_Memory->RdramSize();

        // IPL3 initializes and clears all RDRAM; HLE boot must do the same
        if (rdram && rdramSize > 0) {
            std::memset(rdram, 0, rdramSize);
        }

        // Read entry point from ROM header (offset 0x08)
        // ROM data is in host word order after DetectFormatAndByteSwap.
        uint32_t headerEntryPoint = 0x80000400;
        if (romSize >= 0x0C) {
            headerEntryPoint = *reinterpret_cast<const uint32_t*>(romData + 0x08);
            Log::infof("HLE Boot: ROM header entry point = 0x%08X", headerEntryPoint);
        }
        // IPL3 copies 1MB from ROM[0x1000] to the RDRAM address derived from the
        // header entry point, then jumps there.  HLE boot must replicate this.
        entryPoint = headerEntryPoint;

        // Detect libdragon ROMs: scan from ROM[0x1000] for ELF magic.
        // nust64 embeds the raw ELF file starting around ROM[0x1900].
        // The libdragon IPL3 parses ELF program headers and loads each
        // PT_LOAD segment to its correct virtual address.
        uint32_t elfOffset = 0;
        for (uint32_t scan = 0x1000; scan < romSize - 4 && scan < 0x4000; scan += 0x100) {
            // Check for ELF magic: 7F 45 4C 46 (big-endian, not byte-swapped)
            // ROM data is in host word order after DetectFormatAndByteSwap,
            // so bytes within each 32-bit word are reversed on LE hosts.
            uint32_t word = *reinterpret_cast<const uint32_t*>(romData + scan);
            // ELF magic as uint32 in host order
            if (word == 0x7F454C46 || word == 0x464C457F) // BE or LE host
            {
                elfOffset = scan;
                Log::infof("HLE Boot: Found ELF header at ROM[0x%X]", elfOffset);
                break;
            }
        }

        if (elfOffset != 0 && rdram && romData) {
            // ── Libdragon ELF-in-ROM loading ────────────────────────
            isLibdragonElf = true;
            const uint8_t* elf = romData + elfOffset;
            [[maybe_unused]] uint32_t elfSize = romSize - elfOffset;

            // Helper to read a big-endian 32-bit value from the ELF.
            // The ROM is stored in host word order (each 32-bit word
            // byte-swapped from big-endian).  The ELF sits at an
            // arbitrary offset inside the ROM, so its header fields
            // are NOT necessarily word-aligned relative to the ROM's
            // byte-swap grid.  Read individual bytes and reconstruct
            // big-endian manually.
            [[maybe_unused]] auto readBE16 = [](const uint8_t* p) -> uint16_t {
                // bytes in host-word-order ROM: within each 4-byte
                // group the bytes are reversed on LE hosts.
                // We'll just use the actual byte values.
                uint32_t wordOff = reinterpret_cast<uintptr_t>(p) & 3;
                (void)wordOff;
                return (static_cast<uint16_t>(p[0]) << 8) | p[1];
            };
            [[maybe_unused]] auto readBE32 = [](const uint8_t* p) -> uint32_t {
                return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
            };

            // But wait — the ROM data has been byte-swapped per 32-bit
            // word during ROM loading (DetectFormatAndByteSwap).  So
            // the bytes within each 4-byte group at romData are in LE
            // order on a LE host.  To read big-endian ELF fields we
            // must un-swap: for a field at ROM offset O, the original
            // .z64 byte at O is now at romData[(O & ~3) | (3 - (O & 3))]
            // on a little-endian host.
            // Simpler: just read the uint32 word and extract bytes in
            // big-endian order by shifting.
            auto elfByte = [&](uint32_t off) -> uint8_t {
                // Read the 32-bit host-order word that contains 'off',
                // then pick the big-endian byte within it.
                uint32_t wordIdx = off & ~3u;
                uint32_t w = *reinterpret_cast<const uint32_t*>(elf + wordIdx);
                // 'w' contains the 4 big-endian bytes in host int order.
                // Byte 0 of the BE group is in bits 31-24, etc.
                uint32_t shift = (3 - (off & 3)) * 8;
                return static_cast<uint8_t>((w >> shift) & 0xFF);
            };
            auto elfU16 = [&](uint32_t off) -> uint16_t {
                return (static_cast<uint16_t>(elfByte(off)) << 8) | elfByte(off + 1);
            };
            auto elfU32 = [&](uint32_t off) -> uint32_t {
                return (static_cast<uint32_t>(elfByte(off)) << 24) | (static_cast<uint32_t>(elfByte(off + 1)) << 16) |
                       (static_cast<uint32_t>(elfByte(off + 2)) << 8) | static_cast<uint32_t>(elfByte(off + 3));
            };

            // Parse ELF header (32-bit big-endian)
            uint32_t e_phoff = elfU32(0x1C); // program header offset
            uint16_t e_phentsize = elfU16(0x2A);
            uint16_t e_phnum = elfU16(0x2C);

            Log::infof("HLE Boot: ELF phoff=0x%X phnum=%u phentsize=%u", e_phoff, e_phnum, e_phentsize);

            uint32_t totalLoaded = 0;
            for (uint16_t i = 0; i < e_phnum; ++i) {
                uint32_t phdr = e_phoff + i * e_phentsize;
                uint32_t p_type = elfU32(phdr + 0x00);
                uint32_t p_offset = elfU32(phdr + 0x04);
                uint32_t p_vaddr = elfU32(phdr + 0x08);
                uint32_t p_filesz = elfU32(phdr + 0x10);
                uint32_t p_memsz = elfU32(phdr + 0x14);

                if (p_type != 1) continue; // PT_LOAD = 1

                uint32_t pAddr = p_vaddr & 0x1FFFFFFF;
                if (pAddr + p_filesz > rdramSize) continue; // out of range
                if (elfOffset + p_offset + p_filesz > romSize) continue;

                // Copy segment data from ROM ELF to RDRAM.
                // The source is raw byte data that was already byte-swapped
                // per-word during ROM load.  We need to un-swap it to get
                // the original big-endian bytes, then write to RDRAM (which
                // also stores data in host word order).  Since both source
                // and destination use the same per-word byte order, a plain
                // memcpy is correct as long as we copy whole words.
                // For simplicity, copy word-by-word.
                const uint8_t* src = romData + elfOffset + p_offset;
                uint8_t* dst = rdram + pAddr;
                std::memcpy(dst, src, p_filesz);

                // Zero-fill BSS (memsz > filesz)
                if (p_memsz > p_filesz && pAddr + p_memsz <= rdramSize) {
                    std::memset(dst + p_filesz, 0, p_memsz - p_filesz);
                }

                Log::infof("HLE Boot: ELF PT_LOAD[%u] ROM[0x%X] -> RDRAM[0x%X] (0x%X bytes, vaddr 0x%08X)", i,
                           elfOffset + p_offset, pAddr, p_filesz, p_vaddr);
                totalLoaded += p_filesz;
            }

            Log::infof("HLE Boot: Loaded %u bytes from %u ELF segments", totalLoaded, e_phnum);

            // Update DMEM values for libdragon
            uint8_t* dmem = m_Memory->Dmem();
            if (dmem) {
                *reinterpret_cast<uint32_t*>(dmem + 0x00) = rdramSize;
                *reinterpret_cast<uint32_t*>(dmem + 0x04) = 0;          // entropy
                *reinterpret_cast<uint32_t*>(dmem + 0x08) = 0x00010000; // TV=NTSC(1)<<16
                // DMEM[0x0C]: encodes elf offset so ((val>>16)<<8) = elfOffset
                *reinterpret_cast<uint32_t*>(dmem + 0x0C) = (elfOffset >> 8) << 16;
            }
        } else {
            // ── Standard Nintendo IPL3 boot ─────────────────────────
            // IPL3 copies 1MB from ROM[0x1000] to RDRAM at the physical
            // address derived from the header entry point (offset 0x08).
            uint32_t dramBase = headerEntryPoint & 0x1FFFFFFF; // KSEG0/1 → physical
            uint32_t romOffset = 0x1000;
            uint32_t dmaLen = 0x100000;                        // standard 1MB
            if (romOffset + dmaLen > romSize) dmaLen = romSize - romOffset;
            if (dramBase + dmaLen > rdramSize) dmaLen = rdramSize - dramBase;

            if (rdram && romData && dmaLen > 0) {
                std::memcpy(rdram + dramBase, romData + romOffset, dmaLen);
                Log::infof("HLE Boot: Copied 0x%X bytes from ROM[0x%X] to RDRAM[0x%X]", dmaLen, romOffset, dramBase);
            }
        }

        // Write RDRAM size to CIC-expected location
        // CIC-6105 uses offset 0x03F0; CIC-6102/6101/6103 use 0x0318
        if (rdramSize > 0 && rdram) {
            uint32_t offset = 0x03F0; // CIC-6105 (OoT, Majora's Mask, etc.)
            *reinterpret_cast<uint32_t*>(rdram + offset) = rdramSize;
            // Also write to 6102 offset for compatibility
            *reinterpret_cast<uint32_t*>(rdram + 0x0318) = rdramSize;
            Log::infof("HLE Boot: Wrote RDRAM size 0x%08X to offsets 0x03F0 and 0x0318", rdramSize);
        }

        // Write PIF boot values to RDRAM (matching what the real PIF ROM writes)
        // These values are read by the N64 OS __osInitialize to configure the system.
        if (rdram && rdramSize >= 0x400) {
            // Detect CIC type from ROM
            using N64::Rom::CICChip;
            CICChip cicChip = CICChip::CIC_NUS_6102; // default fallback
            if (GetRom() && GetRom()->IsLoaded()) {
                cicChip = GetRom()->GetImage().GetCICChip();
                if (cicChip == CICChip::Unknown) cicChip = CICChip::CIC_NUS_6102;
            }

            // Map CICChip enum to osCicId seed value and PIF CIC type.
            // N64 OS __osInitialize checks osCicId against the CIC SEED BYTE
            // (not the chip number), e.g. 0x91 for CIC-6105, 0x3F for 6102.
            uint32_t cicSeed = 0x3F;
            PIF::CicType pifCicType = PIF::CicType::NUS_6102;
            switch (cicChip) {
            case CICChip::CIC_NUS_6101:
                cicSeed = 0x3F;
                pifCicType = PIF::CicType::NUS_6101;
                break;
            case CICChip::CIC_NUS_6102:
                cicSeed = 0x3F;
                pifCicType = PIF::CicType::NUS_6102;
                break;
            case CICChip::CIC_NUS_6103:
                cicSeed = 0x78;
                pifCicType = PIF::CicType::NUS_6103;
                break;
            case CICChip::CIC_NUS_6105:
                cicSeed = 0x91;
                pifCicType = PIF::CicType::NUS_6105;
                break;
            default:
                cicSeed = 0x3F;
                pifCicType = PIF::CicType::NUS_6102;
                break;
            }

            // Update PIF handler with the correct CIC type and re-seed
            if (m_Memory->PifRam()) {
                Log::infof("HLE Boot: Updating PIF for CIC type %d (seed=0x%02X)", pifCicType, cicSeed);
                m_Memory->PifRam()->set_cic_type(pifCicType);
                m_Memory->PifRam()->reset();
            } else {
                Log::warning("HLE Boot: PIF RAM not available, skipping PIF reset");
            }

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

            // 0x0310: osCicId — CIC seed byte (0x3F=6102, 0x78=6103, 0x91=6105, 0x85=6106)
            *reinterpret_cast<uint32_t*>(rdram + 0x0310) = cicSeed;

            // 0x0314: osVersion — IPL3 version
            *reinterpret_cast<uint32_t*>(rdram + 0x0314) = 0;

            Log::infof("HLE Boot: Wrote PIF boot values: osTvType=%d osRomBase=0xB0000000 osCicId=0x%02X", tvType,
                       cicSeed);
        }

        // NOTE: CIC-6105 IMEM bootstrap is NOT needed for HLE boot.
        // HLE boot skips IPL3 entirely (PC goes directly to entryPoint=0x80000400).
        // The bootstrap code at IMEM 0xA4001000 is only used by IPL3's CRC jump.
        // Leaving IMEM clean allows non_task_dispatching() to correctly detect
        // CIC x105 RSP tasks via IMEM checksum when the game loads them.
    }

    // Reset Registers
    if (bInitReg && m_Registers && m_Memory) {
        m_Registers->Reset(bInitReg, *m_Memory);

        // Apply CIC specific register values for HLE Boot
        if (GetRom() && GetRom()->IsLoaded()) {
            auto cicChip = GetRom()->GetImage().GetCICChip();
            if (cicChip == N64::Rom::CICChip::CIC_NUS_6102 || cicChip == N64::Rom::CICChip::CIC_NUS_6101) {
                // Mario 64 (CIC-6102) expects t3 (GPR 11) to be 0x00000000
                m_Registers->m_GPR[11] = 0x0000000000000000;
                // a0 (GPR 4) should be 0x1
                m_Registers->m_GPR[4] = 0x0000000000000001;
                Log::infof("HLE Boot: Applied CIC-6102 register overrides (A0=1, T3=0)");
            } else if (cicChip == N64::Rom::CICChip::CIC_NUS_6105 || cicChip == N64::Rom::CICChip::CIC_NUS_6106) {
                // Zelda (CIC-6105) expects t3 (GPR 11) to be 0xFFFFFFFFA4000040
                m_Registers->m_GPR[11] = 0xFFFFFFFFA4000040;
            } else if (cicChip == N64::Rom::CICChip::CIC_NUS_6103) {
                // 6103 (e.g. Diddy Kong Racing)
                m_Registers->m_GPR[11] =
                    0xFFFFFFFFA4000040; // Depending on requirements, default to 6105 behavior for now
            }
        }
    }

    // HLE Boot: Skip boot code and CRC check, jump directly to game entry point.
    // The boot code would set up caches and PI domain timing, but for emulation
    // these are not critical. The important thing is that RDRAM has the game code
    // and registers are set to post-PIF-boot values.
    if (m_Registers) {
        m_Registers->m_PROGRAM_COUNTER = entryPoint;
        Log::infof("HLE Boot: Set PC to entry point 0x%08X (skipping boot code + CRC)", entryPoint);

        // For libdragon ROMs, the IPL3 sets SP to end-of-RDRAM
        if (isLibdragonElf && m_Memory) {
            uint64_t spVal = 0x80000000ULL + m_Memory->RdramSize();
            m_Registers->m_GPR[29] = static_cast<int64_t>(static_cast<int32_t>(spVal));
            Log::infof("HLE Boot: Set SP to 0x%08X (libdragon)", static_cast<uint32_t>(spVal));
        }
    }

    // Reset components
    if (m_SystemTimer) m_SystemTimer->reset();
    if (m_SystemEvents) m_SystemEvents->DeleteAllEvents();
    if (m_TLB) m_TLB->Initialize(m_Memory.get(), m_Registers.get(), nullptr);
    if (m_MIPSInterfaceHandler) m_MIPSInterfaceHandler->Reset();

    // Set up the initial VI timer. The VR4300 runs at 93.75 MHz in both regions,
    // and Count increments once every 2 CPU cycles, so Count ticks per field is
    // (93,750,000 / fieldRate) / 2: NTSC 60 Hz -> 781,250, PAL 50 Hz -> 937,500.
    // SystemTimer's `period`/`cycles_to_fire` are already Count-tick units (see
    // update_compare_timer()'s "distance IS the timer period"), so viIncrement
    // must be passed as-is -- multiplying by CountPerOp() here double-counted
    // the /2 already baked into viIncrement, scheduling VI at half the correct
    // rate in Count-tick terms. Wall-clock pacing in RefreshScreen() still held
    // real VI delivery at 60fps, so the game looked and moved normally, but its
    // own Count-based real-time reads (osGetTime()-style timers -- minigame
    // countdowns, event timers) saw Count advancing 2x too fast.
    if (m_SystemTimer) {
        const bool isPal = GetRom() && GetRom()->IsLoaded() && GetRom()->GetImage().IsPal();
        const uint32_t viIncrement = isPal ? 937500u : 781250u;
        m_SystemTimer->set_timer_periodic(N64System::TimerType::ViTimer, viIncrement);
        Log::infof("VI timer: %s field rate (%u Count ticks/field)", isPal ? "PAL 50Hz" : "NTSC 60Hz", viIncrement);
        // Set up initial Compare timer based on Count/Compare registers
        m_SystemTimer->update_compare_timer();
        Log::debug("VI timer and Compare timer initialized");
    }

    if (m_SyncCPU) {
        m_SyncCPU->Reset(bInitReg, ClearMemory);
    }

    // Initialize RSP subsystem
    // Wire hardware register pointers so the RSP
    // can read/write SP, MI, and DPC registers
    // Use Vmem's SP handler — it's the one that receives actual game writes
    // (DMA, SP_STATUS, etc). MainSYS's m_SPRegistersHandler is a separate
    // instance whose DMEM/IMEM never get game data.
    auto* spHandler = m_Memory ? m_Memory->SPRegisters() : nullptr;
    if (spHandler && m_Registers && m_Memory) {
        // One-time RSP plugin init (sets up settings, debugger)
        Nova64::RSP::RspPluginLoaded();

        // Populate RSP_INFO with pointers to actual hardware registers
        Nova64::RSP::RSP_INFO rspInfo{};

        // ROM header (used for game identification in HLE)
        rspInfo.HEADER = (GetRom() && GetRom()->IsLoaded()) ? const_cast<uint8_t*>(GetRom()->GetRomAddress()) : nullptr;

        // Main memory
        rspInfo.RDRAM = m_Memory->Rdram();
        rspInfo.RdramSize = m_Memory->RdramSize();

        // RSP local memory — from Vmem's handler (receives game DMA)
        rspInfo.DMEM = spHandler->Dmem();
        rspInfo.IMEM = spHandler->Imem();

        // MI interrupt register (index 2 = MI_INTR_REG)
        rspInfo.MI_INTR_REG = &m_Registers->m_Mips_Interface[2];

        // SP registers — from Vmem's handler (game reads/writes these)
        rspInfo.SP_MEM_ADDR_REG = &spHandler->m_SPRegs[0];
        rspInfo.SP_DRAM_ADDR_REG = &spHandler->m_SPRegs[1];
        rspInfo.SP_RD_LEN_REG = &spHandler->m_SPRegs[2];
        rspInfo.SP_WR_LEN_REG = &spHandler->m_SPRegs[3];
        rspInfo.SP_STATUS_REG = &spHandler->m_SPRegs[4];
        rspInfo.SP_DMA_FULL_REG = &spHandler->m_SPRegs[5];
        rspInfo.SP_DMA_BUSY_REG = &spHandler->m_SPRegs[6];
        rspInfo.SP_SEMAPHORE_REG = &spHandler->m_SPRegs[7];
        rspInfo.SP_PC_REG = &spHandler->m_SPRegs[8];

        // DPC registers (Display Processor Command)
        rspInfo.DPC_START_REG = &m_Registers->m_Display_ControlReg[0];
        rspInfo.DPC_END_REG = &m_Registers->m_Display_ControlReg[1];
        rspInfo.DPC_CURRENT_REG = &m_Registers->m_Display_ControlReg[2];
        rspInfo.DPC_STATUS_REG = &m_Registers->m_Display_ControlReg[3];
        rspInfo.DPC_CLOCK_REG = &m_Registers->m_Display_ControlReg[4];
        rspInfo.DPC_BUFBUSY_REG = &m_Registers->m_Display_ControlReg[5];
        rspInfo.DPC_PIPEBUSY_REG = &m_Registers->m_Display_ControlReg[6];
        rspInfo.DPC_TMEM_REG = &m_Registers->m_Display_ControlReg[7];

        // Callback functions
        rspInfo.CheckInterrupts = RSP_CheckInterrupts;
        rspInfo.ProcessDList = RSP_ProcessDList;
        rspInfo.ProcessRdpList = RSP_ProcessRdpList;
        rspInfo.ProcessDListShadow = RSP_ProcessDListShadow;

        // Initialize RSP: bind pointers + build dispatch tables
        Nova64::RSP::InitializeRSP(rspInfo);

        // Per-game RSP setup (allocates recompiler buffers if needed)
        Nova64::RSP::RspRomOpened();

        Log::info("RSP subsystem initialized and wired to hardware registers");
    }

    // Initialize graphics window BEFORE plugins (plugins need it)
    // Only create on first call — warm resets must not destroy the window
    // or plugin, as that would lose all rendering state.
    // A game drawn inside the launcher has no window of its own: the video
    // plugin renders offscreen or into a hidden window. A window made here would
    // also be made off the main thread, which macOS doesn't allow.
    const bool embedded = Nova64::Video::g_embeddedMode.load(std::memory_order_relaxed);
    if (!m_WindowManager && !embedded) {
        m_WindowManager = std::make_unique<Nova64::Video::WindowManager>();
        Nova64::Video::WindowConfig windowConfig;
        windowConfig.title = "NovaStar64 - Emulator";
        windowConfig.width = s_WindowWidth;
        windowConfig.height = s_WindowHeight;
        windowConfig.posX = -1;     // center
        windowConfig.posY = -1;
        windowConfig.vsync = false; // Software frame pacing handles timing
        windowConfig.glMajor = 4;
        windowConfig.glMinor = 6;

        // The window must be made for the renderer the video plugin will pick:
        // SDL can't create a Vulkan surface on a window made for OpenGL. Read the
        // setting the way the plugin does, so a missing file gives the same
        // platform default (Vulkan on macOS) instead of OpenGL.
        Nova64::Video::PluginConfig graphicsConfig;
        graphicsConfig.LoadFromFile("settings/NovaGraphics.ini");
        windowConfig.backendType = graphicsConfig.backendType;

        // Fullscreen mode: borderless fullscreen by default to bypass DWM compositor stalls
        if (s_FullscreenMode == 2) {
            windowConfig.fullscreen = true;
            windowConfig.borderless = false; // Exclusive fullscreen
        } else if (s_FullscreenMode == 1) {
            windowConfig.fullscreen = true;
            windowConfig.borderless = true; // Borderless fullscreen (DWM flip model)
        }
        // else s_FullscreenMode == 0: windowed (default config values)

        windowConfig.headless = s_HeadlessMode;

        if (m_WindowManager->Initialize(windowConfig)) {
            Log::info("Graphics window initialized successfully");
        } else {
            Log::warning("Failed to initialize graphics window (headless mode)");
        }
    }

    bool windowReady = m_WindowManager && m_WindowManager->IsInitialized();

    // Initialize PluginManager and load plugins
    // Only create once — subsequent Reset() calls should reset the
    // existing plugin, not destroy and recreate it.
    if (!m_PluginManager) {
        m_PluginManager = std::make_unique<Plugins::PluginManager>(*this);
        m_PluginManager->LoadPlugin(Plugins::PluginType::Graphics);
        m_PluginManager->LoadPlugin(Plugins::PluginType::Audio);
        m_PluginManager->InitializePlugins();

        // Initialize the graphics plugin display with the window handle
        if (windowReady || embedded) {
            auto* gfxPlugin = m_PluginManager->GetGraphicsPlugin();
            if (gfxPlugin) {
                Plugins::RenderSettings settings;
                settings.WindowWidth = s_WindowWidth;
                settings.WindowHeight = s_WindowHeight;
                settings.AntiAliasing = false;
                settings.Fullscreen = false;

                if (gfxPlugin->InitializeDisplay(windowReady ? m_WindowManager->GetNativeHandle() : nullptr,
                                                 settings)) {
                    Log::info("Graphics plugin display initialized with window handle");
                } else {
                    Log::warning("Graphics plugin display initialization failed");
                }

                // Wire GFX plugin to DPC handler for direct RDP command processing
                if (m_Memory && m_Memory->DPCommandRegisters()) {
                    m_Memory->DPCommandRegisters()->SetGraphicsPlugin(gfxPlugin);
                }
            }

            // Release the GL context from this thread so the CPU thread can use it
            // (UpdateScreen/ProcessDList run on the CPU thread and need the context)
            if (windowReady) m_WindowManager->ReleaseContext();
            else if (SDL_GL_GetCurrentContext()) SDL_GL_MakeCurrent(SDL_GL_GetCurrentWindow(), nullptr);
        }

        Log::info("PluginManager created and plugins initialized");
    } else {
        // Plugin already exists — notify it of the warm reset
        m_PluginManager->ResetPlugins();
    }

    m_InReset = false;
}

bool CN64System::SetActiveSystem(bool bActive) {
    LOG_DEBUG("SetActiveSystem: {}", bActive ? "true" : "false");

    if (bActive) {
        // Set this system as active
        if (g_System != nullptr && g_System != this) {
            LOG_WARNING("Another system is already active");
            return false;
        }
        g_System = this;
    } else {
        // Deactivate this system
        if (g_System == this) {
            g_System = nullptr;
        }
    }

    return true;
}

void CN64System::ExecuteCPU() {
    LOG_DEBUG("ExecuteCPU - Start");

    m_EndEmulation = s_StopRequested.load();

    if (m_EndEmulation) return;

    if (s_UseInterpreter) {
        ExecuteInterpret();
    } else {
        ExecuteRecompiler();
    }

    CpuStopped();

    LOG_DEBUG("ExecuteCPU - Done");
}

void CN64System::ExecuteInterpret() {
    LOG_DEBUG("ExecuteInterpret");
    Nova64::Interpreter::R4300iInterpreter interpreter(*this);
    interpreter.ExecuteCPU();
}

void CN64System::ExecuteSingleOp() {
    Nova64::Interpreter::R4300iInterpreter interpreter(*this);
    interpreter.ExecuteOps(CountPerOp());
}

void CN64System::ExecuteRecompiler() {
    LOG_DEBUG("ExecuteRecompiler");
    Nova64::Recompiler::CRecompiler recompiler(*this, m_EndEmulation);
    recompiler.Run();
}

void CN64System::ExecuteSyncCPU() {
    LOG_DEBUG("ExecuteSyncCPU");
    // Sync CPU mode validates recompiler output against interpreter results.
    // Full lockstep comparison requires single-step hooks in the recompiler;
    // for now fall back to interpreter execution which is always correct.
    LOG_WARNING("SyncCPU: lockstep comparison not yet available, falling back to interpreter");
    ExecuteInterpret();
}

void CN64System::CpuStopped() {
    LOG_DEBUG("CpuStopped");

    if (!m_InReset) {
        LOG_INFO("Emulation ended");
    }

    if (m_SyncCPU) {
        m_SyncCPU->CpuStopped();
    }
}

bool CN64System::IsFrameLimitReached() const {
    if (m_MaxFrames == 0) {
        return false; // No limit
    }

    uint64_t currentFrames = GetCurrentFrameCount();
    if (currentFrames >= m_MaxFrames) {
        return true;
    }

    return false;
}

uint64_t CN64System::GetCurrentFrameCount() const {
    if (m_Memory) {
        auto* vi = m_Memory->VideoInterface();
        return vi->get_statistics().total_frames;
    }
    return 0;
}

void CN64System::DelayedJump(uint64_t target) {
    m_JumpToLocation = target;
    m_PipelineStage = N64Types::PipelineStage::DelaySlot;
}

void CN64System::DelayedRelativeJump(int32_t offset) {
    m_JumpToLocation = m_Registers->m_PROGRAM_COUNTER + offset;

    // Permanent loop detection: a branch whose target equals
    // its own address is an idle/wait loop.  Fast-forward timers instead of
    // spinning millions of iterations burning host CPU.
    if (offset == 0) {
        m_PipelineStage = N64Types::PipelineStage::PermanentLoopDoDelay;
    } else {
        m_PipelineStage = N64Types::PipelineStage::DelaySlot;
    }
}

void CN64System::ExternalEvent(SystemEvent action) {
    Log::debugf("ExternalEvent: %d", static_cast<int>(action));

    switch (action) {
    case SysEvent_CloseCPU:
        Log::info("ExternalEvent: SysEvent_CloseCPU -> ending emulation");
        m_EndEmulation = true;
        break;
    case SysEvent_ResetCPU_Soft: GameReset(); break;
    case SysEvent_ResetCPU_Hard: Reset(true, true); break;
    case SysEvent_PauseCPU:      Pause(); break;
    default:                     Log::warningf("Unknown system event: %d", static_cast<int>(action)); break;
    }
}

void CN64System::RefreshScreen() {
    // Called when the VI timer fires (once per frame, ~60Hz NTSC / ~50Hz PAL)

    // --- PROFILING ---
    static auto profileFrameStart = std::chrono::steady_clock::now();
    auto frameEntryTime = std::chrono::steady_clock::now();
    static double accCpuMs = 0, accRenderMs = 0, accAudioMs = 0, accPaceMs = 0, accSwapMs = 0;
    static int profileFrameCount = 0;

    // Cycle timer: real wall clock between successive RefreshScreen entries
    static auto lastEntry = frameEntryTime;
    static double accCycleMs = 0;
    accCycleMs += std::chrono::duration<double, std::milli>(frameEntryTime - lastEntry).count();
    lastEntry = frameEntryTime;

    // Snapshot recompiler counters at frame entry to attribute CPU spikes
    uint64_t recompCountStart = Nova64::Recompiler::g_CompileCount.load(std::memory_order_relaxed);
    uint64_t recompTimeNsStart = Nova64::Recompiler::g_CompileTimeNs.load(std::memory_order_relaxed);

    double cpuMs = std::chrono::duration<double, std::milli>(frameEntryTime - profileFrameStart).count();
    accCpuMs += cpuMs;
    // --- END PROFILING SETUP ---

    uint64_t frameNum = GetCurrentFrameCount();

    // Divergence tracing. Recorded before any frameskip decision so the trace
    // is identical whether or not this frame ends up being rendered.
    if (Nova64::Trace::IsEnabled()) {
        Nova64::Trace::RecordFrame(*this, frameNum);
    }

    // Read once: the launcher can change the target from another thread.
    const int targetFPS = GetTargetFPS();

    // Auto-frameskip: check if we're behind schedule
    bool shouldRender = true;
    if (targetFPS > 0 && m_FramePaceInitialized) {
        using clock = std::chrono::steady_clock;
        using namespace std::chrono;
        auto targetDuration = duration_cast<nanoseconds>(duration<double>(1.0 / targetFPS));
        auto now = clock::now();
        auto remaining = (m_FramePaceStart + targetDuration) - now;

        // If we're already behind and haven't hit max skip, skip this render
        if (remaining < nanoseconds(0) && m_FrameSkipCount < m_MaxFrameSkip) {
            shouldRender = false;
            m_FrameSkipCount++;
        } else {
            m_FrameSkipCount = 0;
        }
    }

    // Update display via GFX plugin (skip if behind schedule)
    if (shouldRender && m_PluginManager) {
        auto* gfx = m_PluginManager->GetGraphicsPlugin();
        if (gfx) {
            gfx->UpdateScreen();
        }
    }

    auto afterRender = std::chrono::steady_clock::now();
    accRenderMs += std::chrono::duration<double, std::milli>(afterRender - frameEntryTime).count();
    if (shouldRender && m_PluginManager) {
        if (auto* gfx = m_PluginManager->GetGraphicsPlugin()) accSwapMs += gfx->GetLastSwapTimeMs();
    }

    // Audio feeding is handled by the dedicated audio thread in CAudioOutput.

    auto afterAudio = std::chrono::steady_clock::now();
    accAudioMs += std::chrono::duration<double, std::milli>(afterAudio - afterRender).count();

    // Frame rate limiter: pace CPU execution to real time.
    // NOVA64_TURBO=1 disables pacing entirely so headless diagnostic runs reach
    // late game state quickly instead of in real time.
    static const bool s_turbo = []() {
        const char* v = std::getenv("NOVA64_TURBO");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (targetFPS > 0 && !s_turbo) {
        using clock = std::chrono::steady_clock;
        using namespace std::chrono;

        auto targetDuration = duration_cast<nanoseconds>(duration<double>(1.0 / targetFPS));

        if (!m_FramePaceInitialized) {
            m_FramePaceStart = clock::now();
            m_FramePaceInitialized = true;
            // NOTE: timeBeginPeriod(1) was here previously but caused +2-5ms latency
            // in OpenGL driver / scheduler overhead system-wide. With it removed,
            // sleep precision relies on default ~15.6ms quanta, but our spin loop
            // at the end of pace closes the gap to <1us.
        } else {
            m_FramePaceStart += targetDuration;
            auto now = clock::now();
            auto remaining = m_FramePaceStart - now;

            // If we're more than 2 frames behind, snap to current time
            // (don't try to catch up — just accept the dropped frames)
            if (remaining < -targetDuration) {
                m_FramePaceStart = now;
            } else if (remaining > nanoseconds(0)) {
                auto sleepThreshold = microseconds(8000);
                auto preSleep = clock::now();
                double sleptMs = 0;
                if (remaining > sleepThreshold) {
                    std::this_thread::sleep_for(remaining - sleepThreshold);
                    sleptMs = std::chrono::duration<double, std::milli>(clock::now() - preSleep).count();
                }
                auto preSpin = clock::now();
                while (clock::now() < m_FramePaceStart) {
#ifdef _WIN32
                    _mm_pause();
#endif
                }
                (void)preSpin;
                (void)sleptMs;
            }
        }
    }

    auto afterPace = std::chrono::steady_clock::now();
    accPaceMs += std::chrono::duration<double, std::milli>(afterPace - afterAudio).count();
    profileFrameCount++;

    // Per-frame spike detection (only log extreme spikes to avoid I/O overhead)
    double frameTotalMs = cpuMs + std::chrono::duration<double, std::milli>(afterPace - frameEntryTime).count();
    if (frameTotalMs > 100.0) {
        double renderMs = std::chrono::duration<double, std::milli>(afterRender - frameEntryTime).count();
        double audioMs = std::chrono::duration<double, std::milli>(afterAudio - afterRender).count();
        double paceMs = std::chrono::duration<double, std::milli>(afterPace - afterAudio).count();
        uint64_t recompCount = Nova64::Recompiler::g_CompileCount.load(std::memory_order_relaxed) - recompCountStart;
        double recompMs =
            (Nova64::Recompiler::g_CompileTimeNs.load(std::memory_order_relaxed) - recompTimeNsStart) / 1'000'000.0;
        Log::infof("SPIKE frame %llu: CPU=%.1fms Render=%.1fms Audio=%.1fms Pace=%.1fms TOTAL=%.1fms | JIT "
                   "compiles=%llu time=%.1fms",
                   frameNum, cpuMs, renderMs, audioMs, paceMs, frameTotalMs, (unsigned long long)recompCount, recompMs);
    }

    if (profileFrameCount >= 60) {
        double avgCpu = accCpuMs / profileFrameCount;
        double avgRender = accRenderMs / profileFrameCount;
        double avgAudio = accAudioMs / profileFrameCount;
        double avgPace = accPaceMs / profileFrameCount;
        double avgSwap = accSwapMs / profileFrameCount;
        double avgCycle = accCycleMs / profileFrameCount;
        double avgTotal = (accCpuMs + accRenderMs + accAudioMs + accPaceMs) / profileFrameCount;
        double fps = (avgCycle > 0) ? (1000.0 / avgCycle) : 0;
        Log::infof("PERF[%d frames] CPU=%.1fms Render=%.1fms(swap=%.1fms) Audio=%.1fms Pace=%.1fms Sum=%.1fms "
                   "Cycle=%.1fms FPS=%.1f",
                   profileFrameCount, avgCpu, avgRender, avgSwap, avgAudio, avgPace, avgTotal, avgCycle, fps);
        accCpuMs = accRenderMs = accAudioMs = accPaceMs = accSwapMs = accCycleMs = 0;
        profileFrameCount = 0;
    }
    profileFrameStart = std::chrono::steady_clock::now();

    // [SM64-FIX] Workaround for orphaned __osSiAccessQueue at 0x80367158.
    // SM64's game thread (id=5, pri=10) blocks forever during init_controllers
    // waiting on this queue, because an SI/PIF interrupt timing race during
    // boot causes the credit message to never be posted. We detect that exact
    // pattern (idle-thread PC + queue state + waiter signature) and post the
    // missing credit + re-attach the thread to the active queue.
    // See repo memory /memories/repo/sm64-blackscreen-fix-verified.md.
    if (m_Registers && m_Memory && m_Memory->Rdram()) {
        uint32_t curPc = static_cast<uint32_t>(m_Registers->m_PROGRAM_COUNTER);
        if (curPc == 0x80246DD8u) // SM64 idle thread boot loop
        {
            uint8_t* rdram = m_Memory->Rdram();
            const uint32_t rdramSize = m_Memory->RdramSize();
            auto rd32 = [&](uint32_t a) -> uint32_t {
                if (a + 4 > rdramSize) return 0;
                return *reinterpret_cast<const uint32_t*>(rdram + a);
            };
            auto wr32 = [&](uint32_t a, uint32_t v) {
                if (a + 4 > rdramSize) return;
                *reinterpret_cast<uint32_t*>(rdram + a) = v;
            };

            const uint32_t SI_ACCESS_Q = 0x367158;
            const uint32_t ACTIVE_QUEUE = 0x3359A8;

            uint32_t mtq = rd32(SI_ACCESS_Q + 0x00);
            uint32_t validCount = rd32(SI_ACCESS_Q + 0x08);
            uint32_t first = rd32(SI_ACCESS_Q + 0x0C);
            uint32_t msgCount = rd32(SI_ACCESS_Q + 0x10);
            uint32_t msgPtrK = rd32(SI_ACCESS_Q + 0x14);

            if (validCount == 0 && msgCount == 1 && mtq != 0 && (mtq & 0xFF000000u) == 0x80000000u) {
                uint32_t threadAddr = mtq & 0x1FFFFFFFu;
                if (threadAddr + 0x20 <= rdramSize) {
                    uint32_t pri = rd32(threadAddr + 0x04);
                    uint32_t id = rd32(threadAddr + 0x14);
                    uint32_t threadNext = rd32(threadAddr + 0x00);
                    uint32_t sf = rd32(threadAddr + 0x10);
                    if (id == 5 && pri == 10) {
                        uint32_t msgPtrPhys = msgPtrK & 0x1FFFFFFFu;
                        // 1. Post NULL credit into queue ring buffer
                        wr32(msgPtrPhys + first * 4, 0);
                        // 2. Mark queue valid
                        wr32(SI_ACCESS_Q + 0x08, 1);
                        // 3. Detach thread from mtq list
                        wr32(SI_ACCESS_Q + 0x00, threadNext);
                        // 4. Set thread state RUNNABLE (state in high 16 of u32 at +0x10)
                        wr32(threadAddr + 0x10, (sf & 0xFFFFu) | (2u << 16));
                        // 5. Insert thread at head of __osActiveQueue
                        uint32_t prevHead = rd32(ACTIVE_QUEUE);
                        wr32(threadAddr + 0x00, prevHead);
                        wr32(ACTIVE_QUEUE, mtq);

                        static bool sm64FixLogged = false;
                        if (!sm64FixLogged) {
                            sm64FixLogged = true;
                            Log::infof("[SM64-FIX] Released orphaned __osSiAccessQueue (thread id=%u pri=%u)", id, pri);
                        }
                    }
                }
            }
        }
    }

    // Test hooks for unattended runs: NOVA64_STATE_SAVE_AT=<frame>[:slot],... saves
    // a state when the video field count reaches <frame>; NOVA64_STATE_LOAD_AT
    // loads one. Slot defaults to 0. A run that loads a state continues counting
    // from the field count the state was saved at.
    {
        struct Trigger {
            uint64_t frame;
            int slot;
        };
        static const auto parse = [](const char* name) {
            std::vector<Trigger> out;
            const char* value = std::getenv(name);
            for (const char* p = value; p && *p;) {
                char* end = nullptr;
                const uint64_t frame = std::strtoull(p, &end, 10);
                if (end == p) break;
                int slot = 0;
                if (*end == ':') slot = static_cast<int>(std::strtol(end + 1, &end, 10));
                out.push_back({frame, slot});
                p = (*end == ',') ? end + 1 : end;
            }
            return out;
        };
        static const std::vector<Trigger> saves = parse("NOVA64_STATE_SAVE_AT");
        static const std::vector<Trigger> loads = parse("NOVA64_STATE_LOAD_AT");
        for (const Trigger& t : saves)
            if (t.frame == frameNum) Nova64::StateService::Instance().RequestSave(t.slot);
        for (const Trigger& t : loads)
            if (t.frame == frameNum) Nova64::StateService::Instance().RequestLoad(t.slot);
    }

    // Save-state requests, rewind points and the launcher's pause are handled
    // at the next instruction boundary, where the machine state is complete.
    if (m_SystemEvents && Nova64::StateService::Instance().NeedsService(frameNum)) {
        m_SystemEvents->QueueEvent(SysEvent_ServiceStates);
    }

    m_bCleanFrameBox = false;
}

N64System::SystemTimer* CN64System::GetSystemTimer() const { return m_SystemTimer.get(); }

N64Rom* CN64System::GetRom() const { return g_Rom; }

void CN64System::SetSpeed(int ntscEquivalentFps) {
    // Pacing is per VI field, and a PAL console scans 50 fields a second, not 60.
    const bool isPal = g_Rom && g_Rom->IsLoaded() && g_Rom->GetImage().IsPal();
    SetTargetFPS(isPal ? ntscEquivalentFps * 5 / 6 : ntscEquivalentFps);
}

N64Rom* CN64System::GetDDRom() const { return g_DDRom; }

N64Disk* CN64System::GetDisk() const { return g_Disk; }

void CN64System::DumpRegisters() {
    if (!m_Registers) return;
    Log::info("─── CPU Register Dump (Start of Emulation) ───");
    Log::infof("PC:  0x%016llX", m_Registers->m_PROGRAM_COUNTER);
    for (int i = 0; i < 32; i += 2) {
        Log::infof("R%02d (%.3s): 0x%016llX    R%02d (%.3s): 0x%016llX", i, CRegName::GPR[i], m_Registers->m_GPR[i],
                   i + 1, CRegName::GPR[i + 1], m_Registers->m_GPR[i + 1]);
    }
    Log::info("──────────────────────────────────────────────");
}

// Default execution mode.  The dynamic recompiler currently has a native
// backend only for x86-64 (and x86).  On AArch64/other hosts the recompiler
// backend is still being brought up, so default to the always-correct
// interpreter there.  This can be overridden at runtime via SetUseInterpreter().
#if defined(__amd64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
bool CN64System::s_UseInterpreter = false;
#else
bool CN64System::s_UseInterpreter = true;
#endif
