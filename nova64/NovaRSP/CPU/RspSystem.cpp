// SuperNova64 - N64 Emulator
// RSP System Implementation
// Central RSP subsystem: lifecycle, execution loop, sync CPU

#include "RspSystem.hpp"
#include "RSPCpu.hpp"
#include "RSPRegisterHandler.hpp"
#include "RspMemory.hpp"
#include "../RSPDebugger.hpp"
#include "../Settings/RspSettings.hpp"
#include "../Recompiler/RspRecompilerCPU.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>

#if defined(_WIN32)
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif

namespace Nova64::RSP {

// Global RSP system instance
CRSPSystem RSPSystem;

// ──────────────────────────────────────────────
// Construction / Destruction
// ──────────────────────────────────────────────

CRSPSystem::CRSPSystem()
    : m_Op(*this)
{
}

CRSPSystem::~CRSPSystem()
{
    // If this is a sync system with cloned memory, free it
    if (m_SyncSystem != nullptr && m_SyncSystem->m_BaseSystem != nullptr)
    {
        delete[] m_SyncSystem->m_IMEM;
        m_SyncSystem->m_IMEM = nullptr;
        delete[] m_SyncSystem->m_DMEM;
        m_SyncSystem->m_DMEM = nullptr;
    }

    delete[] m_SyncReg;
    m_SyncReg = nullptr;

    delete m_SyncSystem;
    m_SyncSystem = nullptr;

    m_RSPRegisterHandler.reset();
}

// ──────────────────────────────────────────────
// Reset: bind hardware register pointers
// ──────────────────────────────────────────────
void CRSPSystem::Reset(RSP_INFO& info)
{
    m_Reg.Reset();

    m_HEADER = info.HEADER;
    m_RDRAM  = info.RDRAM;

    // Only bind registers for the primary (non-sync) system
    if (m_BaseSystem == nullptr)
    {
        m_DMEM = info.DMEM;
        m_IMEM = info.IMEM;
        m_MI_INTR_REG      = info.MI_INTR_REG;
        m_SP_MEM_ADDR_REG  = info.SP_MEM_ADDR_REG;
        m_SP_DRAM_ADDR_REG = info.SP_DRAM_ADDR_REG;
        m_SP_RD_LEN_REG    = info.SP_RD_LEN_REG;
        m_SP_WR_LEN_REG    = info.SP_WR_LEN_REG;
        m_SP_STATUS_REG    = info.SP_STATUS_REG;
        m_SP_DMA_FULL_REG  = info.SP_DMA_FULL_REG;
        m_SP_DMA_BUSY_REG  = info.SP_DMA_BUSY_REG;
        m_SP_PC_REG        = info.SP_PC_REG;
        m_SP_SEMAPHORE_REG = info.SP_SEMAPHORE_REG;
        m_DPC_START_REG    = info.DPC_START_REG;
        m_DPC_END_REG      = info.DPC_END_REG;
        m_DPC_CURRENT_REG  = info.DPC_CURRENT_REG;
        m_DPC_STATUS_REG   = info.DPC_STATUS_REG;
        m_DPC_CLOCK_REG    = info.DPC_CLOCK_REG;
        m_DPC_BUFBUSY_REG  = info.DPC_BUFBUSY_REG;
        m_DPC_PIPEBUSY_REG = info.DPC_PIPEBUSY_REG;
        m_DPC_TMEM_REG     = info.DPC_TMEM_REG;
        CheckInterrupts    = info.CheckInterrupts;
        ProcessDList       = info.ProcessDList;
        ProcessRdpList     = info.ProcessRdpList;

        m_RdramSize = info.RdramSize;
        if (m_RdramSize == 0)
        {
            m_RdramSize = 0x00400000; // Default 4 MB
        }
    }

    // (Re)create register handler
    m_RSPRegisterHandler = std::make_unique<RSPRegisterHandlerPlugin>(*this);

    // Propagate reset to sync system if present
    if (m_SyncSystem != nullptr)
    {
        m_SyncSystem->Reset(info);
    }
}

void CRSPSystem::RomClosed()
{
    // Per-game cleanup
    m_Recompiler.reset();
}

// ──────────────────────────────────────────────
// Recompiler Execution
// ──────────────────────────────────────────────

bool CRSPSystem::RunRecompiledTask(RecompilerMemory& mem)
{
#if defined(__amd64__) || defined(_M_X64)
    if (mem.Code == nullptr || mem.JumpTable == nullptr) return false;
    if (m_SP_PC_REG == nullptr || m_IMEM == nullptr) return false;

    uint32_t pc = (*m_SP_PC_REG) & 0xFFC;

    // Create recompiler on first use
    if (!m_Recompiler)
    {
        m_Recompiler = std::make_unique<Recompiler::CRSPRecompiler>(*this);
    }

    // Initialize assembler with the executable code buffer
    m_Recompiler->EnsureAssembler(mem.Code, 0x100000);

    // Check JumpTable for cached compiled code
    void* compiled = mem.JumpTable[pc / 4];

    if (compiled == nullptr)
    {
        // Compile the RSP block at current PC
        Recompiler::RspCodeBlocks blocks;
        compiled = m_Recompiler->CompileHLETask(pc, blocks, 0);
        if (compiled == nullptr) return false;
        mem.JumpTable[pc / 4] = compiled;
    }

    // Execute the compiled block
    using CompiledFunc = void(*)();
    reinterpret_cast<CompiledFunc>(compiled)();
    return true;
#else
    (void)mem;
    return false;
#endif
}

// ──────────────────────────────────────────────
// Main Execution Loop (Interpreter)
// ──────────────────────────────────────────────
void CRSPSystem::ExecuteOps(uint32_t cycles, uint32_t targetPC)
{
    RSP_Running = 1;

    if (g_RSPDebugger != nullptr)
    {
        g_RSPDebugger->StartingCPU();
    }

    uint32_t& gprR0 = m_Reg.m_GPR[0].UW;
    uint32_t& pc    = *m_SP_PC_REG;

    while (RSP_Running && cycles > 0)
    {
        if (g_RSPDebugger != nullptr)
        {
            g_RSPDebugger->BeforeExecuteOp();
        }

        // Target PC breakpoint (used by sync CPU)
        if (targetPC != static_cast<uint32_t>(-1) && (pc & 0xFFC) == targetPC)
        {
            break;
        }

        // Fetch instruction from IMEM
        m_OpCode.Value = *reinterpret_cast<uint32_t*>(m_IMEM + (pc & 0xFFC));



        // Dispatch through primary opcode table
        (m_Op.*(m_Op.Jump_Opcode[m_OpCode.op]))();

        // MIPS $zero hard-wired to 0
        gprR0 = 0;

        // Decrement cycle counter (unless unlimited)
        if (cycles != static_cast<uint32_t>(-1))
        {
            --cycles;
        }

        // Pipeline state machine
        switch (m_NextInstruction)
        {
        case PipelineStage::Normal:
            pc = (pc + 4) & 0xFFC;
            break;
        case PipelineStage::DoDelaySlot:
            m_NextInstruction = PipelineStage::Jump;
            pc = (pc + 4) & 0xFFC;
            break;
        case PipelineStage::Jump:
            m_NextInstruction = PipelineStage::Normal;
            pc = m_JumpTo;
            break;
        case PipelineStage::SingleStep:
            pc = (pc + 4) & 0xFFC;
            m_NextInstruction = PipelineStage::SingleStepDone;
            break;
        case PipelineStage::SingleStepDone:
            pc = (pc + 4) & 0xFFC;
            *m_SP_STATUS_REG |= SPStatus::Halt;
            RSP_Running = 0;
            break;
        }
    }
}

// ──────────────────────────────────────────────
// Sync CPU (interpreter verification mode)
// ──────────────────────────────────────────────

void CRSPSystem::SetupSyncCPU()
{
    if (m_SyncSystem == nullptr)
    {
        m_SyncSystem = new CRSPSystem();
        m_SyncSystem->m_BaseSystem = this;
        m_SyncSystem->m_HEADER = m_HEADER;
        m_SyncSystem->m_RDRAM  = m_RDRAM;

        // Allocate independent IMEM/DMEM for the sync system
        m_SyncSystem->m_IMEM = new uint8_t[IMEM_SIZE];
        m_SyncSystem->m_DMEM = new uint8_t[DMEM_SIZE];

        // Allocate register shadow buffer (18 registers)
        m_SyncSystem->m_SyncReg = new uint32_t[18]{};
        m_SyncSystem->m_MI_INTR_REG      = &m_SyncSystem->m_SyncReg[0];
        m_SyncSystem->m_SP_MEM_ADDR_REG  = &m_SyncSystem->m_SyncReg[1];
        m_SyncSystem->m_SP_DRAM_ADDR_REG = &m_SyncSystem->m_SyncReg[2];
        m_SyncSystem->m_SP_RD_LEN_REG    = &m_SyncSystem->m_SyncReg[3];
        m_SyncSystem->m_SP_WR_LEN_REG    = &m_SyncSystem->m_SyncReg[4];
        m_SyncSystem->m_SP_STATUS_REG    = &m_SyncSystem->m_SyncReg[5];
        m_SyncSystem->m_SP_DMA_FULL_REG  = &m_SyncSystem->m_SyncReg[6];
        m_SyncSystem->m_SP_DMA_BUSY_REG  = &m_SyncSystem->m_SyncReg[7];
        m_SyncSystem->m_SP_PC_REG        = &m_SyncSystem->m_SyncReg[8];
        m_SyncSystem->m_SP_SEMAPHORE_REG = &m_SyncSystem->m_SyncReg[9];
        m_SyncSystem->m_DPC_START_REG    = &m_SyncSystem->m_SyncReg[10];
        m_SyncSystem->m_DPC_END_REG      = &m_SyncSystem->m_SyncReg[11];
        m_SyncSystem->m_DPC_CURRENT_REG  = &m_SyncSystem->m_SyncReg[12];
        m_SyncSystem->m_DPC_STATUS_REG   = &m_SyncSystem->m_SyncReg[13];
        m_SyncSystem->m_DPC_CLOCK_REG    = &m_SyncSystem->m_SyncReg[14];
        m_SyncSystem->m_DPC_BUFBUSY_REG  = &m_SyncSystem->m_SyncReg[15];
        m_SyncSystem->m_DPC_PIPEBUSY_REG = &m_SyncSystem->m_SyncReg[16];
        m_SyncSystem->m_DPC_TMEM_REG     = &m_SyncSystem->m_SyncReg[17];

        m_SyncSystem->m_Reg.Reset();
        m_SyncSystem->m_RdramSize = m_RdramSize;
        m_SyncSystem->m_RSPRegisterHandler =
            std::make_unique<RSPRegisterHandlerPlugin>(*m_SyncSystem);
    }

    // Copy memory state
    if (m_IMEM != nullptr)
        std::memcpy(m_SyncSystem->m_IMEM, m_IMEM, IMEM_SIZE);
    if (m_DMEM != nullptr)
        std::memcpy(m_SyncSystem->m_DMEM, m_DMEM, DMEM_SIZE);

    // Copy hardware register state
    auto copyReg = [](uint32_t* dst, uint32_t* src) {
        if (dst && src) *dst = *src;
    };

    copyReg(m_SyncSystem->m_MI_INTR_REG,      m_MI_INTR_REG);
    copyReg(m_SyncSystem->m_SP_MEM_ADDR_REG,  m_SP_MEM_ADDR_REG);
    copyReg(m_SyncSystem->m_SP_DRAM_ADDR_REG, m_SP_DRAM_ADDR_REG);
    copyReg(m_SyncSystem->m_SP_RD_LEN_REG,    m_SP_RD_LEN_REG);
    copyReg(m_SyncSystem->m_SP_WR_LEN_REG,    m_SP_WR_LEN_REG);
    copyReg(m_SyncSystem->m_SP_STATUS_REG,    m_SP_STATUS_REG);
    copyReg(m_SyncSystem->m_SP_DMA_FULL_REG,  m_SP_DMA_FULL_REG);
    copyReg(m_SyncSystem->m_SP_DMA_BUSY_REG,  m_SP_DMA_BUSY_REG);
    copyReg(m_SyncSystem->m_SP_PC_REG,        m_SP_PC_REG);
    copyReg(m_SyncSystem->m_SP_SEMAPHORE_REG, m_SP_SEMAPHORE_REG);
    copyReg(m_SyncSystem->m_DPC_START_REG,    m_DPC_START_REG);
    copyReg(m_SyncSystem->m_DPC_END_REG,      m_DPC_END_REG);
    copyReg(m_SyncSystem->m_DPC_CURRENT_REG,  m_DPC_CURRENT_REG);
    copyReg(m_SyncSystem->m_DPC_STATUS_REG,   m_DPC_STATUS_REG);
    copyReg(m_SyncSystem->m_DPC_CLOCK_REG,    m_DPC_CLOCK_REG);
    copyReg(m_SyncSystem->m_DPC_BUFBUSY_REG,  m_DPC_BUFBUSY_REG);
    copyReg(m_SyncSystem->m_DPC_PIPEBUSY_REG, m_DPC_PIPEBUSY_REG);
    copyReg(m_SyncSystem->m_DPC_TMEM_REG,     m_DPC_TMEM_REG);
}

bool CRSPSystem::IsSyncSystem() const
{
    return m_BaseSystem != nullptr;
}

CRSPSystem* CRSPSystem::SyncSystem() const
{
    if (m_SyncSystem == nullptr)
    {
        const_cast<CRSPSystem*>(this)->SetupSyncCPU();
    }
    return m_SyncSystem;
}

void CRSPSystem::BasicSyncCheck()
{
    if (m_SyncSystem == nullptr) return;

    bool syncFailed = false;

    // Compare vector registers
    const RSPVector* vect     = m_Reg.m_Vect;
    const RSPVector* vectSync = m_SyncSystem->m_Reg.m_Vect;
    for (int i = 0; i < 32; i++)
    {
        if (vect[i].u64(0) != vectSync[i].u64(0) ||
            vect[i].u64(1) != vectSync[i].u64(1))
        {
            syncFailed = true;
            break;
        }
    }

    // Compare IMEM / DMEM
    if (std::memcmp(m_IMEM, m_SyncSystem->m_IMEM, IMEM_SIZE) != 0)
    {
        syncFailed = true;
    }
    if (std::memcmp(m_DMEM, m_SyncSystem->m_DMEM, DMEM_SIZE) != 0)
    {
        syncFailed = true;
    }

    // Compare key hardware registers
    auto checkReg = [&](uint32_t* a, uint32_t* b) {
        if (a && b && *a != *b) syncFailed = true;
    };

    checkReg(m_MI_INTR_REG,      m_SyncSystem->m_MI_INTR_REG);
    checkReg(m_SP_MEM_ADDR_REG,  m_SyncSystem->m_SP_MEM_ADDR_REG);
    checkReg(m_SP_DRAM_ADDR_REG, m_SyncSystem->m_SP_DRAM_ADDR_REG);
    checkReg(m_SP_RD_LEN_REG,    m_SyncSystem->m_SP_RD_LEN_REG);
    checkReg(m_SP_WR_LEN_REG,    m_SyncSystem->m_SP_WR_LEN_REG);
    checkReg(m_SP_STATUS_REG,    m_SyncSystem->m_SP_STATUS_REG);
    checkReg(m_SP_DMA_FULL_REG,  m_SyncSystem->m_SP_DMA_FULL_REG);
    checkReg(m_SP_DMA_BUSY_REG,  m_SyncSystem->m_SP_DMA_BUSY_REG);
    checkReg(m_SP_SEMAPHORE_REG, m_SyncSystem->m_SP_SEMAPHORE_REG);

    // PC comparison (masked to 12 bits)
    if (m_SP_PC_REG && m_SyncSystem->m_SP_PC_REG &&
        (*m_SP_PC_REG & 0xFFC) != (*m_SyncSystem->m_SP_PC_REG & 0xFFC))
    {
        syncFailed = true;
    }

    if (syncFailed)
    {
        // Detailed sync failure report
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "RSP Sync FAILED at PC=0x%03X (sync PC=0x%03X)",
            m_SP_PC_REG ? (*m_SP_PC_REG & 0xFFC) : 0u,
            m_SyncSystem->m_SP_PC_REG ? (*m_SyncSystem->m_SP_PC_REG & 0xFFC) : 0u);

        // Log which subsystem diverged
        if (std::memcmp(m_IMEM, m_SyncSystem->m_IMEM, IMEM_SIZE) != 0)
            std::snprintf(msg + std::strlen(msg), sizeof(msg) - std::strlen(msg), " [IMEM mismatch]");
        if (std::memcmp(m_DMEM, m_SyncSystem->m_DMEM, DMEM_SIZE) != 0)
            std::snprintf(msg + std::strlen(msg), sizeof(msg) - std::strlen(msg), " [DMEM mismatch]");
        for (int i = 0; i < 32; i++)
        {
            if (m_Reg.m_Vect[i].u64(0) != m_SyncSystem->m_Reg.m_Vect[i].u64(0) ||
                m_Reg.m_Vect[i].u64(1) != m_SyncSystem->m_Reg.m_Vect[i].u64(1))
            {
                std::snprintf(msg + std::strlen(msg), sizeof(msg) - std::strlen(msg),
                    " [VR%d]", i);
            }
        }

        if (g_RSPDebugger != nullptr)
        {
            g_RSPDebugger->SyncError(msg);
        }
    }
}

// ──────────────────────────────────────────────
// Aligned allocation (16-byte alignment for SIMD)
// ──────────────────────────────────────────────

void* CRSPSystem::operator new(size_t size)
{
#if defined(_WIN32)
    return _aligned_malloc(size, 16);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 16, size) != 0)
        return nullptr;
    return ptr;
#endif
}

void CRSPSystem::operator delete(void* ptr)
{
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

// ──────────────────────────────────────────────
// Null callbacks (used before real ones are set)
// ──────────────────────────────────────────────

void CRSPSystem::NullProcessDList()   {}
void CRSPSystem::NullProcessRdpList() {}
void CRSPSystem::NullCheckInterrupts() {}

// ──────────────────────────────────────────────
// DMA Transfer Helpers
// SP ↔ RDRAM transfers as done by the hardware
// ──────────────────────────────────────────────

void CRSPSystem::SP_DMA_READ()
{
    // DMA from RDRAM → IMEM/DMEM
    if (m_SP_MEM_ADDR_REG == nullptr || m_SP_DRAM_ADDR_REG == nullptr ||
        m_SP_RD_LEN_REG == nullptr || m_RDRAM == nullptr)
        return;

    uint32_t mem_addr  = *m_SP_MEM_ADDR_REG & 0x1FFF;
    uint32_t dram_addr = *m_SP_DRAM_ADDR_REG & 0x7FFFFF;
    uint32_t length    = (*m_SP_RD_LEN_REG & 0xFFF) + 1;
    uint32_t count     = ((*m_SP_RD_LEN_REG >> 12) & 0xFF) + 1;
    uint32_t skip      = (*m_SP_RD_LEN_REG >> 20) & 0xFFF;

    // Determine target: bit 12 of mem_addr selects IMEM vs DMEM
    uint8_t* target = (mem_addr & 0x1000) ? m_IMEM : m_DMEM;
    mem_addr &= 0xFFF;

    if (target == nullptr) return;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t copy_len = std::min<uint32_t>(length, 0x1000 - mem_addr);
        copy_len = std::min<uint32_t>(copy_len, m_RdramSize - dram_addr);

        if (copy_len > 0)
            std::memcpy(target + mem_addr, m_RDRAM + dram_addr, copy_len);

        // Advance for multi-line DMA
        dram_addr += length + skip;
        dram_addr &= 0x7FFFFF;
        mem_addr += length;
        mem_addr &= 0xFFF;
    }

    // Clear DMA flags
    if (m_SP_DMA_BUSY_REG) *m_SP_DMA_BUSY_REG = 0;
    if (m_SP_DMA_FULL_REG) *m_SP_DMA_FULL_REG = 0;
}

void CRSPSystem::SP_DMA_WRITE()
{
    // DMA from IMEM/DMEM → RDRAM
    if (m_SP_MEM_ADDR_REG == nullptr || m_SP_DRAM_ADDR_REG == nullptr ||
        m_SP_WR_LEN_REG == nullptr || m_RDRAM == nullptr)
        return;

    uint32_t mem_addr  = *m_SP_MEM_ADDR_REG & 0x1FFF;
    uint32_t dram_addr = *m_SP_DRAM_ADDR_REG & 0x7FFFFF;
    uint32_t length    = (*m_SP_WR_LEN_REG & 0xFFF) + 1;
    uint32_t count     = ((*m_SP_WR_LEN_REG >> 12) & 0xFF) + 1;
    uint32_t skip      = (*m_SP_WR_LEN_REG >> 20) & 0xFFF;

    const uint8_t* source = (mem_addr & 0x1000) ? m_IMEM : m_DMEM;
    mem_addr &= 0xFFF;

    if (source == nullptr) return;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t copy_len = std::min<uint32_t>(length, 0x1000 - mem_addr);
        copy_len = std::min<uint32_t>(copy_len, m_RdramSize - dram_addr);

        if (copy_len > 0)
            std::memcpy(m_RDRAM + dram_addr, source + mem_addr, copy_len);

        dram_addr += length + skip;
        dram_addr &= 0x7FFFFF;
        mem_addr += length;
        mem_addr &= 0xFFF;
    }

    if (m_SP_DMA_BUSY_REG) *m_SP_DMA_BUSY_REG = 0;
    if (m_SP_DMA_FULL_REG) *m_SP_DMA_FULL_REG = 0;
}

// ──────────────────────────────────────────────
// Debug: dump sync state to buffer
// ──────────────────────────────────────────────

void CRSPSystem::DumpSyncState(char* buf, size_t bufSize) const
{
    if (buf == nullptr || bufSize == 0) return;

    int pos = 0;
    pos += std::snprintf(buf + pos, bufSize - pos,
        "=== RSP Sync State Dump ===\n");

    // PC
    if (m_SP_PC_REG)
        pos += std::snprintf(buf + pos, bufSize - pos,
            "PC: 0x%03X\n", *m_SP_PC_REG & 0xFFC);

    // SP Status
    if (m_SP_STATUS_REG)
        pos += std::snprintf(buf + pos, bufSize - pos,
            "SP_STATUS: 0x%08X\n", *m_SP_STATUS_REG);

    // GPRs
    pos += std::snprintf(buf + pos, bufSize - pos, "GPRs:\n");
    for (int i = 0; i < 32 && pos < static_cast<int>(bufSize) - 20; i++)
    {
        pos += std::snprintf(buf + pos, bufSize - pos,
            "  R%02d: 0x%08X", i, m_Reg.m_GPR[i].UW);
        if ((i & 3) == 3)
            pos += std::snprintf(buf + pos, bufSize - pos, "\n");
    }

    // First 8 vector registers (full contents would be too large)
    pos += std::snprintf(buf + pos, bufSize - pos, "Vector Regs (0-7):\n");
    for (int i = 0; i < 8 && pos < static_cast<int>(bufSize) - 40; i++)
    {
        pos += std::snprintf(buf + pos, bufSize - pos,
            "  V%02d: %016llX %016llX\n", i,
            static_cast<unsigned long long>(m_Reg.m_Vect[i].u64(0)),
            static_cast<unsigned long long>(m_Reg.m_Vect[i].u64(1)));
    }

    // DMA registers
    pos += std::snprintf(buf + pos, bufSize - pos, "DMA:\n");
    if (m_SP_MEM_ADDR_REG)
        pos += std::snprintf(buf + pos, bufSize - pos,
            "  MEM_ADDR: 0x%08X\n", *m_SP_MEM_ADDR_REG);
    if (m_SP_DRAM_ADDR_REG)
        pos += std::snprintf(buf + pos, bufSize - pos,
            "  DRAM_ADDR: 0x%08X\n", *m_SP_DRAM_ADDR_REG);
    if (m_SP_RD_LEN_REG)
        pos += std::snprintf(buf + pos, bufSize - pos,
            "  RD_LEN: 0x%08X\n", *m_SP_RD_LEN_REG);
    if (m_SP_WR_LEN_REG)
        pos += std::snprintf(buf + pos, bufSize - pos,
            "  WR_LEN: 0x%08X\n", *m_SP_WR_LEN_REG);
}

// ──────────────────────────────────────────────
// Execution profiling stats
// ──────────────────────────────────────────────

void CRSPSystem::ResetProfilingStats()
{
    m_ProfileOpsExecuted = 0;
    m_ProfileCyclesUsed  = 0;
    m_ProfileDMATransfers = 0;
    m_ProfileBreaks = 0;
}

void CRSPSystem::GetProfilingStats(uint64_t& opsExecuted, uint64_t& cyclesUsed,
                                    uint32_t& dmaTransfers, uint32_t& breaks) const
{
    opsExecuted  = m_ProfileOpsExecuted;
    cyclesUsed   = m_ProfileCyclesUsed;
    dmaTransfers = m_ProfileDMATransfers;
    breaks       = m_ProfileBreaks;
}

} // namespace Nova64::RSP
