/**
 * @file hle.hpp
 * @brief RSP high-level emulation interface.
 */

#pragma once
#include "ucodes.hpp"
#include "AudioOutput.hpp"
#include "TaskDispatcher.hpp"
#include <chrono>
#include <cstdint>
#include <memory>

namespace Nova64::RSP {
class CRSPSystem;
}

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// CHle - High-Level Emulation engine
// Dispatches RSP tasks to audio/video HLE handlers
// ──────────────────────────────────────────────
class CHle {
public:
    explicit CHle(CRSPSystem& system);
    ~CHle();

    CHle(const CHle&) = delete;
    CHle& operator=(const CHle&) = delete;

    // ── Memory access ──
    [[nodiscard]] uint8_t* dram()  { return m_dram; }
    [[nodiscard]] uint8_t* dmem()  { return m_dmem; }
    [[nodiscard]] uint8_t* imem()  { return m_imem; }

    // ── Settings queries ──
    [[nodiscard]] bool AudioHle()    const { return m_AudioHle; }
    [[nodiscard]] bool GraphicsHle() const { return m_GraphicsHle; }

    // ── Audio state ──
    [[nodiscard]] AlistAudio&  alist_audio()  { return m_alist_audio; }
    [[nodiscard]] AlistNaudio& alist_naudio() { return m_alist_naudio; }
    [[nodiscard]] AlistNead&   alist_nead()   { return m_alist_nead; }
    [[nodiscard]] uint8_t*     mp3_buffer()   { return m_mp3_buffer; }
    [[nodiscard]] uint8_t*     alist_buffer() { return m_alist_buffer; }

    // ── Logging ──
    void VerboseMessage(const char* message, ...);
    void WarnMessage(const char* message, ...);
    void ErrorMessage(const char* message, ...);

    // ── RSP control ──
    void rsp_break(uint32_t setbits);
    bool try_fast_audio_dispatching();

    // ── Main HLE execution entry point ──
    // Detects task type and dispatches to appropriate HLE handler.
    // Returns true if HLE handled the task, false if LLE fallback is needed.
    bool Execute();

    // ── Audio output ──
    [[nodiscard]] CAudioOutput& audioOutput() { return m_audioOutput; }
    void QueueAudioOutput(const int16_t* samples, uint32_t count);
    void FeedAudioDevice();

    // ── Plugin callbacks ──
    void CallProcessDList()  { if (m_ProcessDList != nullptr) m_ProcessDList(); }
    void CallProcessRdpList() { if (m_ProcessRdpList != nullptr) m_ProcessRdpList(); }

    // ── SP Hardware Register Access (for HLE mem.hpp) ──
    [[nodiscard]] uint32_t*& sp_mem_addr()  { return m_sp_mem_addr; }
    [[nodiscard]] uint32_t*& sp_dram_addr() { return m_sp_dram_addr; }
    [[nodiscard]] uint32_t*& sp_status()    { return m_sp_status; }
    [[nodiscard]] uint32_t*& sp_pc()        { return m_sp_pc; }
    [[nodiscard]] uint32_t*& mi_intr()      { return m_mi_intr; }

    // ── DPC Register Access (for GfxProcessor RDP command buffer) ──
    [[nodiscard]] uint32_t*& dpc_start()    { return m_dpc_start; }
    [[nodiscard]] uint32_t*& dpc_end()      { return m_dpc_end; }
    [[nodiscard]] uint32_t*& dpc_current()  { return m_dpc_current; }
    [[nodiscard]] uint32_t*& dpc_status()   { return m_dpc_status; }
    [[nodiscard]] uint32_t*& dpc_clock()    { return m_dpc_clock; }
    [[nodiscard]] uint32_t*& dpc_bufbusy()  { return m_dpc_bufbusy; }
    [[nodiscard]] uint32_t*& dpc_pipebusy() { return m_dpc_pipebusy; }
    [[nodiscard]] uint32_t*& dpc_tmem()     { return m_dpc_tmem; }

private:
    CHle() = delete;

    bool is_task();
    bool normal_task_dispatching();
    bool non_task_dispatching();

    // Memory pointers
    uint8_t*& m_dram;
    uint8_t*& m_dmem;
    uint8_t*& m_imem;

    // MI register
    uint32_t*& m_mi_intr;

    // SP registers
    uint32_t*& m_sp_mem_addr;
    uint32_t*& m_sp_dram_addr;
    uint32_t*& m_sp_rd_length;
    uint32_t*& m_sp_wr_length;
    uint32_t*& m_sp_status;
    uint32_t*& m_sp_dma_full;
    uint32_t*& m_sp_dma_busy;
    uint32_t*& m_sp_pc;
    uint32_t*& m_sp_semaphore;

    // DPC registers
    uint32_t*& m_dpc_start;
    uint32_t*& m_dpc_end;
    uint32_t*& m_dpc_current;
    uint32_t*& m_dpc_status;
    uint32_t*& m_dpc_clock;
    uint32_t*& m_dpc_bufbusy;
    uint32_t*& m_dpc_pipebusy;
    uint32_t*& m_dpc_tmem;

    // Callbacks
    void (*&m_CheckInterrupts)();
    void (*&m_ProcessDList)();
    void (*&m_ProcessRdpList)();

    // Audio HLE state
    uint8_t     m_alist_buffer[0x1000]{};
    AlistAudio  m_alist_audio{};
    AlistNaudio m_alist_naudio{};
    AlistNead   m_alist_nead{};
    uint8_t     m_mp3_buffer[0x1000]{};

    // Settings
    bool m_AudioHle{false};
    bool m_GraphicsHle{false};
    bool m_ForwardAudio{false};
    bool m_ForwardGFX{false};

    // Task validation limits
    static constexpr uint32_t kMaxUcodeSize     = 0x2000;   // 8 KB max microcode text
    static constexpr uint32_t kMaxUcodeDataSize = 0x100000; // 1 MB max microcode data
    static constexpr uint32_t kMaxDataSize      = 0x400000; // 4 MB max display list / audio data
    static constexpr uint32_t kRDRAMMask        = 0x007FFFFF;

    // Execution timeout
    static constexpr auto kTaskTimeoutMs = std::chrono::milliseconds(200);
    uint32_t m_timeout_count{0};
    static constexpr uint32_t kMaxTimeoutsBeforeWarn = 5;

    // Deferred SP interrupt flag — set by rsp_break(), consumed by ClearHalt()
    bool m_sp_interrupt_pending{false};

    // Audio output device
    CAudioOutput m_audioOutput;

    // Microcode dispatcher (persistent across tasks for caching)
    CTaskDispatcher m_Dispatcher;
};

} // namespace Nova64::RSP::HLE
