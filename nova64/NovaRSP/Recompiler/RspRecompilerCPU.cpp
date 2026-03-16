// SuperNova64 - N64 Emulator
// RSP Recompiler CPU Implementation
// x64 JIT compiler managing code generation and dispatch

#include "RspRecompilerCPU.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include "RspAssembler.hpp"
#include "RspCodeBlock.hpp"
#include "RspProfiling.hpp"
#include "../CPU/RSPInterpreterOps.hpp"
#include "../CPU/RspSystem.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Recompiler dispatch function tables
// ──────────────────────────────────────────────

using OpFunc = void (CRSPRecompilerOps::*)();

static const OpFunc PrimaryTable[64] = {
    &CRSPRecompilerOps::SPECIAL,  &CRSPRecompilerOps::REGIMM,
    &CRSPRecompilerOps::J,        &CRSPRecompilerOps::JAL,
    &CRSPRecompilerOps::BEQ,      &CRSPRecompilerOps::BNE,
    &CRSPRecompilerOps::BLEZ,     &CRSPRecompilerOps::BGTZ,
    &CRSPRecompilerOps::ADDI,     &CRSPRecompilerOps::ADDIU,
    &CRSPRecompilerOps::SLTI,     &CRSPRecompilerOps::SLTIU,
    &CRSPRecompilerOps::ANDI,     &CRSPRecompilerOps::ORI,
    &CRSPRecompilerOps::XORI,     &CRSPRecompilerOps::LUI,
    &CRSPRecompilerOps::COP0,     &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::COP2,     &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::LB,       &CRSPRecompilerOps::LH,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::LW,
    &CRSPRecompilerOps::LBU,      &CRSPRecompilerOps::LHU,
    &CRSPRecompilerOps::LWU,      &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::SB,       &CRSPRecompilerOps::SH,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::SW,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::LC2,      &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::SC2,      &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
    &CRSPRecompilerOps::UnknownOpcode, &CRSPRecompilerOps::UnknownOpcode,
};

// ──────────────────────────────────────────────
// Constructor / destructor
// ──────────────────────────────────────────────

CRSPRecompiler::CRSPRecompiler(CRSPSystem& system)
    : m_System(system)
    , m_RecompilerOps(system, *this)
    , m_RegState(m_RecompilerOps)
{
    BuildRecompilerCPU();
}

CRSPRecompiler::~CRSPRecompiler()
{
    delete m_Assembler;
    m_Assembler = nullptr;
}

// ──────────────────────────────────────────────
// Reset
// ──────────────────────────────────────────────

void CRSPRecompiler::Reset()
{
    m_BranchTargets.clear();
    m_BlockID = 0;
    m_CompilePC = 0;
    m_NextInstruction = PipelineStage::Normal;
    m_CurrentBlock = nullptr;
    m_CodeLog.clear();

    if (m_Assembler)
        m_Assembler->Reset();

    ResetTimerList();
}

// ──────────────────────────────────────────────
// Logging
// ──────────────────────────────────────────────

void CRSPRecompiler::Log(const char* text, ...)
{
    char buf[512];
    va_list args;
    va_start(args, text);
    std::vsnprintf(buf, sizeof(buf), text, args);
    va_end(args);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Build recompiler tables
// ──────────────────────────────────────────────

void CRSPRecompiler::BuildRecompilerCPU()
{
    SetupRspAssembler();
}

void CRSPRecompiler::SetupRspAssembler()
{
    // Assembler is created lazily via EnsureAssembler()
    // when the recompiler is first invoked with a code buffer
}

void CRSPRecompiler::EnsureAssembler(uint8_t* codeBuffer, size_t bufferSize)
{
    if (m_Assembler != nullptr) return;
    m_Assembler = new RspAssembler(codeBuffer, bufferSize, m_CodeLog);
}

// ──────────────────────────────────────────────
// Address-of helper (variadic trick for offsetof)
// ──────────────────────────────────────────────

void* CRSPRecompiler::GetAddressOf(int32_t /*value*/, ...)
{
    va_list args;
    va_start(args, 0);
    void* addr = va_arg(args, void*);
    va_end(args);
    return addr;
}

// ──────────────────────────────────────────────
// Branch jump management
// ──────────────────────────────────────────────

void CRSPRecompiler::ClearBranchJump()
{
    m_BranchTargets.clear();
}

void CRSPRecompiler::AddBranchJump(uint32_t target)
{
    if (m_BranchTargets.find(target) == m_BranchTargets.end())
    {
        m_BranchTargets[target] = m_BlockID++;
    }
}

bool CRSPRecompiler::FindBranchJump(uint32_t target, uint32_t& labelId)
{
    auto it = m_BranchTargets.find(target);
    if (it != m_BranchTargets.end())
    {
        labelId = it->second;
        return true;
    }
    return false;
}

// ──────────────────────────────────────────────
// Task compilation entry points
// ──────────────────────────────────────────────

void* CRSPRecompiler::CompileTaskEnter()
{
    if (m_Assembler == nullptr) return nullptr;
    m_RecompilerOps.EnterCodeBlock();
    return m_Assembler->CurrentPos();
}

void* CRSPRecompiler::CompileTaskLeave()
{
    if (m_Assembler == nullptr) return nullptr;
    m_RecompilerOps.ExitCodeBlock();
    return m_Assembler->CurrentPos();
}

// ──────────────────────────────────────────────
// HLE task compilation
// ──────────────────────────────────────────────

void* CRSPRecompiler::CompileHLETask(
    uint32_t address,
    RspCodeBlocks& functions,
    uint32_t dispatchAddress)
{
    auto block = std::make_unique<RspCodeBlock>(
        m_System, address, RspCodeType::Task, dispatchAddress, functions);

    if (!block->IsValid())
    {
        Log("CompileHLETask: Block at 0x%03X is invalid\n", address);
        return nullptr;
    }

    // Compile all sub-functions first
    CompileSubFunctions(functions, block->GetFunctionCalls());

    // Compile the main block
    CompileCodeBlock(*block);

    void* compiled = m_Assembler ? m_Assembler->CurrentPos() : nullptr;
    block->SetCompiledLocation(compiled);
    functions[address & 0xFFC] = std::move(block);

    return compiled;
}

// ──────────────────────────────────────────────
// Sub-function compilation
// ──────────────────────────────────────────────

bool CRSPRecompiler::CompileSubFunctions(
    RspCodeBlocks& functions,
    const RspCodeBlock::Addresses& addresses)
{
    for (uint32_t addr : addresses)
    {
        if (functions.find(addr) != functions.end())
            continue; // Already compiled

        auto subBlock = std::make_unique<RspCodeBlock>(
            m_System, addr, RspCodeType::Subroutine, addr, functions);

        if (!subBlock->IsValid())
        {
            Log("WARNING: Sub-function at 0x%03X is invalid\n", addr);
            continue;
        }

        // Recursively compile nested function calls
        CompileSubFunctions(functions, subBlock->GetFunctionCalls());
        CompileCodeBlock(*subBlock);

        void* compiled = m_Assembler ? m_Assembler->CurrentPos() : nullptr;
        subBlock->SetCompiledLocation(compiled);
        functions[addr & 0xFFC] = std::move(subBlock);
    }

    return true;
}

// ──────────────────────────────────────────────
// Block compilation
// ──────────────────────────────────────────────

void CRSPRecompiler::CompileCodeBlock(RspCodeBlock& block)
{
    m_CurrentBlock = &block;
    ClearBranchJump();

    const auto& instructions = block.GetInstructions();
    const auto& branchTargets = block.GetBranchTargets();

    // Register branch targets
    for (uint32_t target : branchTargets)
    {
        AddBranchJump(target);
    }

    // Compile each instruction
    uint32_t pc = block.GetStartAddress();
    for (size_t i = 0; i < instructions.size(); i++)
    {
        m_CompilePC = pc & 0xFFC;
        m_OpCode = RSPOpcode(instructions[i].Value());
        m_NextInstruction = PipelineStage::Normal;

        CompileOpcode(m_CompilePC);

        pc = (pc + 4) & 0xFFC;
    }

    m_CurrentBlock = nullptr;
}

// ──────────────────────────────────────────────
// Single opcode compilation
// ──────────────────────────────────────────────

void CRSPRecompiler::CompileOpcode(uint32_t pc)
{
    Log("; PC=0x%03X  OP=0x%08X\n", pc, m_OpCode.Value);

    // Reset register protection for this instruction
    m_RegState.ResetRegProtection();

    // Dispatch to the appropriate code generator
    uint32_t primary = m_OpCode.op;
    if (primary < 64 && PrimaryTable[primary] != nullptr)
    {
        (m_RecompilerOps.*(PrimaryTable[primary]))();
    }
    else
    {
        m_RecompilerOps.UnknownOpcode();
    }
}

void CRSPRecompiler::FinalizeAssembler(void* /*funcPtr*/)
{
    // Flush any pending code log output
    if (!m_CodeLog.empty())
    {
        Log("; --- Block finalized ---\n\n");
    }
}

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
