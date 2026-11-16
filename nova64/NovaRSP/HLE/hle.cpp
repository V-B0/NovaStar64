/**
 * @file hle.cpp
 * @brief RSP high-level emulation core: task detection, microcode dispatch, and audio output.
 */

#include "hle.hpp"
#include "TaskDispatcher.hpp"
#include "AudioOutput.hpp"
#include "../CPU/RspSystem.hpp"
#include "../CPU/RSPRegisters.hpp"
#include "../Settings/RspSettings.hpp"
#include "mem.hpp"
#include "ucodes.hpp"
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace Nova64::RSP::HLE {

// --- Construction / Destruction ---

CHle::CHle(CRSPSystem& system) :
    m_dram(system.m_RDRAM), m_dram_size(system.m_RdramSize), m_dmem(system.m_DMEM), m_imem(system.m_IMEM),
    m_mi_intr(system.m_MI_INTR_REG), m_sp_mem_addr(system.m_SP_MEM_ADDR_REG), m_sp_dram_addr(system.m_SP_DRAM_ADDR_REG),
    m_sp_rd_length(system.m_SP_RD_LEN_REG), m_sp_wr_length(system.m_SP_WR_LEN_REG), m_sp_status(system.m_SP_STATUS_REG),
    m_sp_dma_full(system.m_SP_DMA_FULL_REG), m_sp_dma_busy(system.m_SP_DMA_BUSY_REG), m_sp_pc(system.m_SP_PC_REG),
    m_sp_semaphore(system.m_SP_SEMAPHORE_REG), m_dpc_start(system.m_DPC_START_REG), m_dpc_end(system.m_DPC_END_REG),
    m_dpc_current(system.m_DPC_CURRENT_REG), m_dpc_status(system.m_DPC_STATUS_REG), m_dpc_clock(system.m_DPC_CLOCK_REG),
    m_dpc_bufbusy(system.m_DPC_BUFBUSY_REG), m_dpc_pipebusy(system.m_DPC_PIPEBUSY_REG),
    m_dpc_tmem(system.m_DPC_TMEM_REG), m_CheckInterrupts(system.CheckInterrupts), m_ProcessDList(system.ProcessDList),
    m_ProcessRdpList(system.ProcessRdpList), m_ProcessDListShadow(system.ProcessDListShadow) {
    m_AudioHle = CRSPSettings::IsAudioHle();
    m_GraphicsHle = CRSPSettings::IsGraphicsHle();

    VerboseMessage("RSP HLE: AudioHle=%s, GraphicsHle=%s, CPUMethod=%u", m_AudioHle ? "ON" : "OFF",
                   m_GraphicsHle ? "ON" : "OFF", static_cast<uint32_t>(CRSPSettings::CPUMethod()));

    if (!m_GraphicsHle) WarnMessage("RSP HLE: Graphics HLE is DISABLED — graphics tasks will forward to plugin only");
    if (!m_AudioHle) WarnMessage("RSP HLE: Audio HLE is DISABLED — audio tasks will not be processed");

    // Audio output is owned by AudioInterfaceHandler — it initializes the device
    // when the game writes a valid DACRATE.  Don't init here (premature default
    // rate is rejected by some output backends).
}

CHle::~CHle() = default;

// --- Logging ---

void CHle::VerboseMessage(const char* message, ...) {
#ifndef NDEBUG
    va_list args;
    va_start(args, message);
    std::vfprintf(stdout, message, args);
    va_end(args);
    std::fputc('\n', stdout);
#else
    (void)message;
#endif
}

void CHle::WarnMessage(const char* message, ...) {
    va_list args;
    va_start(args, message);
    std::fprintf(stderr, "[RSP HLE WARN] ");
    std::vfprintf(stderr, message, args);
    va_end(args);
    std::fputc('\n', stderr);
}

// --- RSP control ---

void CHle::rsp_break(uint32_t setbits) {
    // Feed the audio device before halting the RSP (CAudioOutput::FeedDevice is currently empty).
    FeedAudioDevice();

    if (m_sp_status == nullptr) return;
    *m_sp_status |= setbits;
    *m_sp_status |= SPStatus::Halt | SPStatus::Broke;

    // --- Signal management matching real N64 RSP microcode behavior ---
    // All RSP microcode sets SIG1 before BREAK (task-done indicator).
    *m_sp_status |= SPStatus::Signal1;

    // SIG2 distinguishes task type for the N64 OS interrupt dispatcher:
    //   SIG2 SET   → GFX task completion   (dispatched to video event queue)
    //   SIG2 CLEAR → Audio task completion  (dispatched to audio event queue)
    // GFX microcode (F3DEX2, etc.) sets SIG2 before BREAK; Audio microcode
    // does not.  If we forced SIG2 for audio tasks, audio completions would be
    // misrouted to the video queue, deadlocking the audio thread
    // during scene transitions (e.g., OoT cutscene loop).
    if (m_dmem) {
        const uint32_t taskType = *u32(m_dmem, TaskOffset::Type);
        if (taskType == 1) { // Video/GFX task
            *m_sp_status |= SPStatus::Signal2;
        }
    }

    if ((*m_sp_status & SPStatus::IntrBreak) != 0) {
        // Do not set MI_INTR_SP or call CheckInterrupts here. Both SP and DP interrupt
        // bits are deferred to an RspTimer scheduled by ClearHalt() after DoRspCycles()
        // returns. Setting MI_INTR_SP here would let the CPU's normal interrupt polling see
        // the pending bit and fire the exception immediately, before the game returns from
        // the CLR_HALT write. ClearHalt accumulates the bits in m_Reg.m_deferred_mi_intr
        // and the timer applies them later.
        m_sp_interrupt_pending = true;
    }
}

// --- Task detection and dispatching ---

bool CHle::is_task() {
    if (m_dmem == nullptr) return false;

    // Check for valid OSTask type at DMEM 0xFC0
    // Standard N64 OSTask types: 1=Video, 2=Audio, 4=JPEG/CIC, 7=Boot
    const uint32_t type = *u32(m_dmem, TaskOffset::Type);
    if (type != 1 && type != 2 && type != 4 && type != 7) return false;

    const uint32_t ucode_size = *u32(m_dmem, TaskOffset::UcodeSize);
    const uint32_t ucode_data_size = *u32(m_dmem, TaskOffset::UcodeDataSize);
    const uint32_t ucode_addr = *u32(m_dmem, TaskOffset::Ucode);
    const uint32_t data_addr = *u32(m_dmem, TaskOffset::DataPtr);

    // Validate microcode size bounds
    if (ucode_size > kMaxUcodeSize) {
        WarnMessage("RSP HLE: Suspicious ucode size 0x%X (max 0x%X), type=%u", ucode_size, kMaxUcodeSize, type);
        // Don't reject — some games have unusual sizes, just warn
    }

    if (ucode_data_size > kMaxUcodeDataSize) {
        WarnMessage("RSP HLE: Suspicious ucode data size 0x%X (max 0x%X), type=%u", ucode_data_size, kMaxUcodeDataSize,
                    type);
    }

    // Validate DRAM address ranges (if set, must be within RDRAM)
    if (ucode_addr != 0 && (ucode_addr & kRDRAMMask) != ucode_addr) {
        WarnMessage("RSP HLE: Ucode address 0x%08X outside RDRAM range, type=%u", ucode_addr, type);
    }

    if (data_addr != 0 && (data_addr & kRDRAMMask) != data_addr) {
        WarnMessage("RSP HLE: Data address 0x%08X outside RDRAM range, type=%u", data_addr, type);
    }

    return true;
}

bool CHle::try_fast_audio_dispatching() {
    if (!m_AudioHle) return false;
    if (m_dmem == nullptr) return false;

    const uint32_t type = *u32(m_dmem, TaskOffset::Type);
    if (type != 2) // Audio = 2
        return false;

    // Use CRC-based dispatcher for audio tasks
    if (m_Dispatcher.DispatchTask(this)) return true;

    // Audio task not handled by CRC dispatcher — fall through to normal_task_dispatching()
    WarnMessage("RSP HLE: Fast audio dispatch failed for audio task, falling back to normal dispatch");
    return false;
}

bool CHle::normal_task_dispatching() {
    if (m_dmem == nullptr) return false;

    const uint32_t type = *u32(m_dmem, TaskOffset::Type);

    // Try the full CRC-based dispatcher first (handles both video + audio via CRC)
    if (m_Dispatcher.DispatchTask(this)) return true;

    // Fallback: basic type-based dispatch
    switch (type) {
    case 1: // Video (GFX) task
        if (!m_GraphicsHle) {
            // LLE mode: bail out so DoRspCycles falls through to the RSP
            // recompiler/interpreter which executes the real ucode.
            return false;
        }
        if (m_ProcessDList != nullptr) {
            WarnMessage("RSP HLE: Gfx task reached type-based fallback — forwarding to plugin");
            m_ProcessDList();
        }
        break;
    case 2: // Audio task
        if (m_AudioHle) {
            WarnMessage("RSP HLE: Audio task reached type-based fallback — using generic alist");
            alist_process_audio(this);
        }
        break;
    case 7:          // CIC x105 boot
        cicx105_ucode(this);
        return true; // cicx105 handles rsp_break internally
    default:
        // Unknown task type — let LLE interpreter handle it
        WarnMessage("RSP HLE: Unknown task type %u — falling back to RSP interpreter", type);
        return false;
    }

    rsp_break(0);
    return true;
}

bool CHle::non_task_dispatching() {
    // Non-task microcodes (e.g., CIC-X105 boot code)
    if (m_imem == nullptr) return false;

    // Check for CIC-X105 pattern: compute IMEM checksum
    uint32_t sum = 0;
    const uint32_t* imem32 = reinterpret_cast<const uint32_t*>(m_imem);
    for (int i = 0; i < 44; i++) {
        sum += imem32[i];
    }

    // Known CIC-X105 boot code checksums
    if (sum == 0x9E2 || sum == 0x9E4) {
        cicx105_ucode(this);
        return true;
    }

    // Unknown non-task microcode — let LLE interpreter handle it
    WarnMessage("RSP HLE: Unknown non-task IMEM checksum=0x%X, falling back to RSP interpreter", sum);
    return false;
}

void CHle::QueueAudioOutput(const int16_t* samples, uint32_t count) { m_audioOutput.QueueSamples(samples, count); }

void CHle::FeedAudioDevice() { m_audioOutput.FeedDevice(); }

// --- Main HLE execution entry point ---
bool CHle::Execute() {
    const auto start_time = std::chrono::steady_clock::now();

    // Refresh HLE enable flags from CRSPSettings every dispatch. The plugin
    // can flip these (renderingMode in NovaGraphics.ini) after CHle was
    // constructed in RspRomOpened(), so a cached snapshot would be stale.
    m_AudioHle = CRSPSettings::IsAudioHle();
    m_GraphicsHle = CRSPSettings::IsGraphicsHle();

    [[maybe_unused]] uint32_t taskType = 0;
    if (m_dmem) taskType = *u32(m_dmem, TaskOffset::Type);

    bool handled = false;

    if (is_task()) {
        // Attempt fast audio dispatch first
        if (try_fast_audio_dispatching()) {
            FeedAudioDevice();
            return true;
        }

        // Full task dispatch (video -> plugin, audio -> HLE, etc.)
        handled = normal_task_dispatching();
    } else {
        // Non-task microcode (e.g. CIC-X105 boot code)
        handled = non_task_dispatching();
    }

    // If HLE couldn't handle the task, return false so the RSP interpreter
    // can run the actual microcode (e.g., CIC compare, custom tasks).
    if (!handled) {
        return false;
    }

    FeedAudioDevice();

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    if (elapsed > kTaskTimeoutMs) {
        m_timeout_count++;
        if (m_timeout_count <= kMaxTimeoutsBeforeWarn) {
            WarnMessage("RSP HLE: Task execution took %lld ms (threshold: %lld ms), timeout #%u",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
                        static_cast<long long>(kTaskTimeoutMs.count()), m_timeout_count);
        }
    }

    return true;
}

} // namespace Nova64::RSP::HLE
