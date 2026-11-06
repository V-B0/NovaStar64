/**
 * @file RspSystem.hpp
 * @brief RSP System.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "RSPInterpreterOps.hpp"
#include "RspEngine.hpp"
#include "RSPRegisters.hpp"
#include "RspPipelineStage.hpp"
#include "RSPTypes.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>

namespace Nova64::RSP {

// Forward declarations
class RSPRegisterHandlerPlugin;


namespace HLE {
class CHle;
}

// ──────────────────────────────────────────────
// RSP Info structure (passed from main system)
// Mirrors N64's RSP_INFO plugin spec
// ──────────────────────────────────────────────
struct RSP_INFO {
    uint8_t*  HEADER;
    uint8_t*  RDRAM;
    uint8_t*  DMEM;
    uint8_t*  IMEM;
    uint32_t* MI_INTR_REG;
    uint32_t* SP_MEM_ADDR_REG;
    uint32_t* SP_DRAM_ADDR_REG;
    uint32_t* SP_RD_LEN_REG;
    uint32_t* SP_WR_LEN_REG;
    uint32_t* SP_STATUS_REG;
    uint32_t* SP_DMA_FULL_REG;
    uint32_t* SP_DMA_BUSY_REG;
    uint32_t* SP_PC_REG;
    uint32_t* SP_SEMAPHORE_REG;
    uint32_t* DPC_START_REG;
    uint32_t* DPC_END_REG;
    uint32_t* DPC_CURRENT_REG;
    uint32_t* DPC_STATUS_REG;
    uint32_t* DPC_CLOCK_REG;
    uint32_t* DPC_BUFBUSY_REG;
    uint32_t* DPC_PIPEBUSY_REG;
    uint32_t* DPC_TMEM_REG;
    uint32_t  RdramSize;
    void (*CheckInterrupts)();
    void (*ProcessDList)();
    void (*ProcessRdpList)();
    void (*ProcessDListShadow)();  // Optional: shadow HLE for RT geometry under LLE
};

// ──────────────────────────────────────────────
// CRSPSystem - Central RSP subsystem
// Owns interpreter, registers, recompiler, and HLE
// ──────────────────────────────────────────────
namespace Jit { class RspJit; }

class CRSPSystem {
    friend class RSPOp;
    friend class Jit::RspJit;
    friend class CRDPLog;
    friend class RSPRegisterHandler;
    friend class RSPRegisterHandlerPlugin;
    friend class HLE::CHle;

public:
    CRSPSystem();
    ~CRSPSystem();

    CRSPSystem(const CRSPSystem&) = delete;
    CRSPSystem& operator=(const CRSPSystem&) = delete;

    // Lifecycle
    void Reset(RSP_INFO& info);
    void RomClosed();

    // Execution
    void ExecuteOps(uint32_t cycles, uint32_t targetPC);   // the interpreter, always

    /// Run until the RSP halts (BREAK or a halt request) or `cycles` instructions
    /// have run, on whichever engine is selected. The recompiler only handles an
    /// unlimited budget; anything else runs on the interpreter.
    void Run(uint32_t cycles);

    /// Forget a branch that was still waiting for its delay slot (used between unrelated programs).
    void      ResetPipeline() { m_NextInstruction = PipelineStage::Normal; m_JumpTo = 0; }
    /// Runs the instruction `word` on the interpreter, for code that only needs one
    /// instruction's effect (the recompiler uses this for what it doesn't translate).
    /// Control-transfer instructions are not for this: they need the pipeline state.
    void      ExecuteOne(uint32_t word);

    void      SetEngine(RSPEngine engine) { m_Engine = engine; }
    [[nodiscard]] RSPEngine GetEngine() const { return m_Engine; }


    // Sync CPU support (for debugging / verification)
    void SetupSyncCPU();
    [[nodiscard]] bool IsSyncSystem() const;
    [[nodiscard]] CRSPSystem* SyncSystem() const;
    void BasicSyncCheck();

    // DMA transfer helpers
    void SP_DMA_READ();
    void SP_DMA_WRITE();

    // Debug utilities
    void DumpSyncState(char* buf, size_t bufSize) const;

    // Profiling
    void ResetProfilingStats();
    void GetProfilingStats(uint64_t& opsExecuted, uint64_t& cyclesUsed,
                           uint32_t& dmaTransfers, uint32_t& breaks) const;

    // Custom allocator for alignment
    void* operator new(size_t size);
    void  operator delete(void* ptr);

    // Register file access
    [[nodiscard]] CRSPRegisters&       Registers()       { return m_Reg; }
    [[nodiscard]] const CRSPRegisters& Registers() const { return m_Reg; }

    // Memory access (read-only pointers for instruction analysis)
    [[nodiscard]] const uint8_t* IMEM() const { return m_IMEM; }
    [[nodiscard]] const uint8_t* DMEM() const { return m_DMEM; }
    [[nodiscard]] const uint8_t* Header() const { return m_HEADER; }

private:
    static void NullProcessDList();
    static void NullProcessRdpList();
    static void NullCheckInterrupts();

    void RunUntimed(uint32_t cycles);

    RSPEngine m_Engine{RSPEngine::Interpreter};
    std::unique_ptr<Jit::RspJit> m_Jit;

    // Sync CPU
    CRSPSystem* m_SyncSystem{nullptr};
    CRSPSystem* m_BaseSystem{nullptr};

    // Core components
    std::unique_ptr<RSPRegisterHandlerPlugin> m_RSPRegisterHandler;
    CRSPRegisters m_Reg;
    RSPOp         m_Op;
    RSPOpcode     m_OpCode;
    PipelineStage m_NextInstruction{PipelineStage::Normal};
    uint32_t      m_JumpTo{0};

    // Memory / hardware register pointers (set by Reset)
    uint8_t*  m_HEADER{nullptr};
    uint8_t*  m_RDRAM{nullptr};
    uint8_t*  m_DMEM{nullptr};
    uint8_t*  m_IMEM{nullptr};

    // MI / SP / DPC registers
    uint32_t* m_MI_INTR_REG{nullptr};
    uint32_t* m_SP_MEM_ADDR_REG{nullptr};
    uint32_t* m_SP_DRAM_ADDR_REG{nullptr};
    uint32_t* m_SP_RD_LEN_REG{nullptr};
    uint32_t* m_SP_WR_LEN_REG{nullptr};
    uint32_t* m_SP_STATUS_REG{nullptr};
    uint32_t* m_SP_DMA_FULL_REG{nullptr};
    uint32_t* m_SP_DMA_BUSY_REG{nullptr};
    uint32_t* m_SP_PC_REG{nullptr};
    uint32_t* m_SP_SEMAPHORE_REG{nullptr};
    uint32_t* m_DPC_START_REG{nullptr};
    uint32_t* m_DPC_END_REG{nullptr};
    uint32_t* m_DPC_CURRENT_REG{nullptr};
    uint32_t* m_DPC_STATUS_REG{nullptr};
    uint32_t* m_DPC_CLOCK_REG{nullptr};
    uint32_t* m_DPC_BUFBUSY_REG{nullptr};
    uint32_t* m_DPC_PIPEBUSY_REG{nullptr};
    uint32_t* m_DPC_TMEM_REG{nullptr};
    uint32_t  m_RdramSize{0};

    // Sync register buffer
    uint32_t* m_SyncReg{nullptr};

    // Callbacks
    void (*CheckInterrupts)(){nullptr};
    void (*ProcessDList)(){nullptr};
    void (*ProcessRdpList)(){nullptr};
    void (*ProcessDListShadow)(){nullptr};  // Shadow HLE for RT geometry under LLE

    // Profiling counters
    uint64_t m_ProfileOpsExecuted{0};
    uint64_t m_ProfileCyclesUsed{0};
    uint32_t m_ProfileDMATransfers{0};
    uint32_t m_ProfileBreaks{0};
};

// Global RSP system instance
extern CRSPSystem RSPSystem;

} // namespace Nova64::RSP
