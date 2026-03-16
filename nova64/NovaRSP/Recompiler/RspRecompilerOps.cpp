// SuperNova64 - N64 Emulator
// RSP Recompiler Operations Implementation
// x64 code generation for all RSP opcodes

#include "RspRecompilerOps.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include "RspAssembler.hpp"
#include "RspCodeBlock.hpp"
#include "RspRecompilerCPU.hpp"
#include "x64/x64RspVectorGen.hpp"
#include "x64/x64RspRegInfo.hpp"
#include "../CPU/RSPInterpreterOps.hpp"
#include "../CPU/RSPRegisters.hpp"
#include "../CPU/RspSystem.hpp"
#include <cstdio>
#include <cstring>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Constructor
// ──────────────────────────────────────────────

CRSPRecompilerOps::CRSPRecompilerOps(CRSPSystem& system, CRSPRecompiler& recompiler)
    : m_System(system)
    , m_Recompiler(recompiler)
    , m_OpCode(recompiler.m_OpCode)
    , m_CompilePC(recompiler.m_CompilePC)
    , m_CurrentBlock(recompiler.m_CurrentBlock)
    , m_NextInstruction(recompiler.m_NextInstruction)
    , m_DMEM(system.m_DMEM)
    , m_Reg(system.m_Reg)
    , m_ACCUM(system.m_Reg.m_ACCUM)
    , m_VCOL(system.m_Reg.m_VCOL)
    , m_VCOH(system.m_Reg.m_VCOH)
    , m_VCCL(system.m_Reg.m_VCCL)
    , m_VCCH(system.m_Reg.m_VCCH)
    , m_VCE(system.m_Reg.m_VCE)
    , m_Assembler(recompiler.m_Assembler)
    , m_RegState(recompiler.m_RegState)
{
    m_GPR = system.m_Reg.m_GPR;
    m_Vect = system.m_Reg.m_Vect;
}

// ──────────────────────────────────────────────
// Utility: fallback to interpreter
// ──────────────────────────────────────────────

void CRSPRecompilerOps::Cheat_r4300iOpcode(void (RSPOp::*funcAddress)(), const char* funcName, bool commentOp)
{
    if (m_Assembler == nullptr) return;

    if (commentOp)
    {
        m_Recompiler.Log("; Fallback: %s\n", funcName);
    }

    // Write back all registers before calling interpreter
    m_RegState.WriteBackRegisters();

    if (funcAddress != nullptr)
    {
        // Extract raw function address from member-function-pointer
        // On MSVC x64 single-inheritance classes, sizeof(mfp) == sizeof(void*)
        void* rawAddr;
        static_assert(sizeof(funcAddress) == sizeof(void*), "member function pointer size mismatch");
        std::memcpy(&rawAddr, &funcAddress, sizeof(void*));

        // Emit: mov rcx, &m_System.m_Op  (this pointer for RSPOp)
        //       call rawAddr
        m_Assembler->CallThis(&m_System.m_Op, rawAddr, funcName);
    }
}

// ──────────────────────────────────────────────
// Unknown opcode handler
// ──────────────────────────────────────────────

void CRSPRecompilerOps::UnknownOpcode()
{
    m_Recompiler.Log("; UnknownOpcode 0x%08X at PC=0x%03X\n", m_OpCode.Value, m_CompilePC);
}

// ──────────────────────────────────────────────
// Block entry/exit
// ──────────────────────────────────────────────

void CRSPRecompilerOps::EnterCodeBlock()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; === EnterCodeBlock ===\n");

    // Save callee-saved GPRs (Windows x64 ABI: RBX, RBP, R12-R15)
    m_Assembler->PushX86reg(3);   // RBX
    m_Assembler->PushX86reg(5);   // RBP
    m_Assembler->PushX86reg(12);  // R12
    m_Assembler->PushX86reg(13);  // R13
    m_Assembler->PushX86reg(14);  // R14
    m_Assembler->PushX86reg(15);  // R15

    // Save callee-saved XMMs (XMM6-XMM15) to m_SaveBuffer
    // m_SaveBuffer is alignas(16), 176 bytes = room for 10 × 16 bytes
    uint8_t* saveArea = m_Recompiler.m_SaveBuffer;
    for (uint8_t i = 6; i <= 15; i++)
    {
        m_Assembler->Movdqa_MemFromXmm(saveArea + (i - 6) * 16, "SaveXMM", i);
    }
}

void CRSPRecompilerOps::ExitCodeBlock()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    m_Recompiler.Log("; === ExitCodeBlock ===\n");

    // Restore callee-saved XMMs (XMM6-XMM15) from m_SaveBuffer
    uint8_t* saveArea = m_Recompiler.m_SaveBuffer;
    for (uint8_t i = 6; i <= 15; i++)
    {
        m_Assembler->Movdqa_XmmFromMem(i, saveArea + (i - 6) * 16, "SaveXMM");
    }

    // Restore callee-saved GPRs (reverse order)
    m_Assembler->PopX86reg(15);   // R15
    m_Assembler->PopX86reg(14);   // R14
    m_Assembler->PopX86reg(13);   // R13
    m_Assembler->PopX86reg(12);   // R12
    m_Assembler->PopX86reg(5);    // RBP
    m_Assembler->PopX86reg(3);    // RBX

    m_Assembler->Ret();
}

// ──────────────────────────────────────────────
// Section analysis helper
// ──────────────────────────────────────────────

bool CRSPRecompilerOps::RSP_DoSections()
{
    return m_CurrentBlock != nullptr;
}

// ──────────────────────────────────────────────
// Helper: offsets into register file
// ──────────────────────────────────────────────

uint32_t CRSPRecompilerOps::GprOffset(uint8_t gpReg) const
{
    return static_cast<uint32_t>(gpReg) * sizeof(Word32);
}

uint32_t CRSPRecompilerOps::VectorOffset(uint8_t vectorReg) const
{
    return static_cast<uint32_t>(vectorReg) * sizeof(RSPVector);
}

uint32_t CRSPRecompilerOps::AccumOffset(AccumLocation location) const
{
    switch (location) {
    case AccumLocation::High:   return 0;
    case AccumLocation::Middle: return 16;
    case AccumLocation::Low:    return 32;
    case AccumLocation::Entire: return 0;
    }
    return 0;
}

uint32_t CRSPRecompilerOps::FlagOffset(RspFlags flag) const
{
    return static_cast<uint32_t>(flag) * sizeof(RSPFlag);
}

// ──────────────────────────────────────────────
// Helper: check if vector dest write can be skipped
// ──────────────────────────────────────────────

bool CRSPRecompilerOps::WriteToVectorDest(uint32_t destReg, uint32_t pc)
{
    // Optimization: if no following instruction reads this register, skip write
    // For now, always write (conservative)
    (void)destReg;
    (void)pc;
    return true;
}

bool CRSPRecompilerOps::WriteToAccum(AccumLocation location, uint32_t pc)
{
    // Optimization: if no following instruction reads the accumulator, skip
    (void)location;
    (void)pc;
    return true;
}

// ──────────────────────────────────────────────
// Helper: load vector register into XMM
// ──────────────────────────────────────────────

void CRSPRecompilerOps::LoadVectorRegister(uint8_t xmmReg, uint8_t vectorReg, uint8_t e)
{
    if (m_Assembler == nullptr) return;

    // Load full 128-bit vector register into XMM
    m_Assembler->Movdqa_XmmFromMem(xmmReg, &m_Vect[vectorReg], "VR[vreg]");

    // Apply element broadcast shuffle if e >= 2
    // e=0..1: identity (all 8 elements), no shuffle needed
    // e=2..15: replicate specific lanes via pshufb with precomputed mask
    if (e >= 2)
    {
        // Use a temporary XMM for the shuffle mask (pick one different from xmmReg)
        uint8_t maskXmm = (xmmReg == 0) ? 1 : 0;
        m_Assembler->Movdqa_XmmFromMem(maskXmm,
            const_cast<uint8_t*>(x64::g_ElementBroadcastMasks[e].mask), "ElemMask");
        m_Assembler->Pshufb_XmmToXmm(xmmReg, maskXmm);
    }
}

// ══════════════════════════════════════════════
// Primary opcodes
// ══════════════════════════════════════════════

void CRSPRecompilerOps::SPECIAL()
{
    switch (m_OpCode.funct) {
    case RSP_SPECIAL_SLL:   Special_SLL();   break;
    case RSP_SPECIAL_SRL:   Special_SRL();   break;
    case RSP_SPECIAL_SRA:   Special_SRA();   break;
    case RSP_SPECIAL_SLLV:  Special_SLLV();  break;
    case RSP_SPECIAL_SRLV:  Special_SRLV();  break;
    case RSP_SPECIAL_SRAV:  Special_SRAV();  break;
    case RSP_SPECIAL_JR:    Special_JR();    break;
    case RSP_SPECIAL_JALR:  Special_JALR();  break;
    case RSP_SPECIAL_BREAK: Special_BREAK(); break;
    case RSP_SPECIAL_ADD:   Special_ADD();   break;
    case RSP_SPECIAL_ADDU:  Special_ADDU();  break;
    case RSP_SPECIAL_SUB:   Special_SUB();   break;
    case RSP_SPECIAL_SUBU:  Special_SUBU();  break;
    case RSP_SPECIAL_AND:   Special_AND();   break;
    case RSP_SPECIAL_OR:    Special_OR();    break;
    case RSP_SPECIAL_XOR:   Special_XOR();   break;
    case RSP_SPECIAL_NOR:   Special_NOR();   break;
    case RSP_SPECIAL_SLT:   Special_SLT();   break;
    case RSP_SPECIAL_SLTU:  Special_SLTU();  break;
    default: UnknownOpcode(); break;
    }
}

void CRSPRecompilerOps::REGIMM()
{
    switch (m_OpCode.rt) {
    case RSP_REGIMM_BLTZ:   RegImm_BLTZ();   break;
    case RSP_REGIMM_BGEZ:   RegImm_BGEZ();   break;
    case RSP_REGIMM_BLTZAL: RegImm_BLTZAL(); break;
    case RSP_REGIMM_BGEZAL: RegImm_BGEZAL(); break;
    default: UnknownOpcode(); break;
    }
}

void CRSPRecompilerOps::COP0()
{
    switch (m_OpCode.rs) {
    case 0: Cop0_MF(); break;
    case 4: Cop0_MT(); break;
    default: UnknownOpcode(); break;
    }
}

void CRSPRecompilerOps::COP2()
{
    switch (m_OpCode.rs) {
    case 0:  Cop2_MF(); break;
    case 2:  Cop2_CF(); break;
    case 4:  Cop2_MT(); break;
    case 6:  Cop2_CT(); break;
    case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23:
    case 24: case 25: case 26: case 27:
    case 28: case 29: case 30: case 31:
        COP2_VECTOR();
        break;
    default: UnknownOpcode(); break;
    }
}

// ══════════════════════════════════════════════
// Branches & jumps — emit compare + conditional jump
// ══════════════════════════════════════════════

void CRSPRecompilerOps::J()
{
    if (m_Assembler == nullptr) return;
    uint32_t target = (m_OpCode.target << 2) & 0xFFC;
    m_Recompiler.Log("; J 0x%03X\n", target);
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
}

void CRSPRecompilerOps::JAL()
{
    if (m_Assembler == nullptr) return;
    uint32_t target = (m_OpCode.target << 2) & 0xFFC;
    m_Recompiler.Log("; JAL 0x%03X\n", target);
    m_NextInstruction = PipelineStage::DoDelaySlot;
    // RA = PC + 8
    uint32_t returnAddr = (m_CompilePC + 8) & 0xFFC;
    m_Assembler->MoveConstToVariable(&m_GPR[31].UW, "GPR[31]", returnAddr);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
}

void CRSPRecompilerOps::BEQ()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BEQ r%u, r%u\n", m_OpCode.rs, m_OpCode.rt);
    m_NextInstruction = PipelineStage::DoDelaySlot;

    uint32_t target = (m_CompilePC + 4 + (static_cast<int16_t>(m_OpCode.offset) << 2)) & 0xFFC;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        if (m_RegState.GetGprConstValue(m_OpCode.rs) == m_RegState.GetGprConstValue(m_OpCode.rt))
            m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
        else
            m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
        return;
    }

    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->CompX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
    AsmLabel branchLabel = m_Assembler->CreateLabel();
    m_Assembler->JeLabel("BranchTaken", branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
    AsmLabel doneLabel = m_Assembler->CreateLabel();
    m_Assembler->JmpLabel("BranchDone", doneLabel);
    m_Assembler->BindLabel(branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    m_Assembler->BindLabel(doneLabel);
}

void CRSPRecompilerOps::BNE()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BNE r%u, r%u\n", m_OpCode.rs, m_OpCode.rt);
    m_NextInstruction = PipelineStage::DoDelaySlot;

    uint32_t target = (m_CompilePC + 4 + (static_cast<int16_t>(m_OpCode.offset) << 2)) & 0xFFC;

    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->CompX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
    AsmLabel branchLabel = m_Assembler->CreateLabel();
    m_Assembler->JneLabel("BranchTaken", branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
    AsmLabel doneLabel = m_Assembler->CreateLabel();
    m_Assembler->JmpLabel("BranchDone", doneLabel);
    m_Assembler->BindLabel(branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    m_Assembler->BindLabel(doneLabel);
}

void CRSPRecompilerOps::BLEZ()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BLEZ r%u\n", m_OpCode.rs);
    m_NextInstruction = PipelineStage::DoDelaySlot;

    uint32_t target = (m_CompilePC + 4 + (static_cast<int16_t>(m_OpCode.offset) << 2)) & 0xFFC;
    m_Assembler->CompConstToVariable(&m_GPR[m_OpCode.rs].UW, "GPR[rs]", 0);
    AsmLabel branchLabel = m_Assembler->CreateLabel();
    m_Assembler->JleLabel("BranchTaken", branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
    AsmLabel doneLabel = m_Assembler->CreateLabel();
    m_Assembler->JmpLabel("BranchDone", doneLabel);
    m_Assembler->BindLabel(branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    m_Assembler->BindLabel(doneLabel);
}

void CRSPRecompilerOps::BGTZ()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BGTZ r%u\n", m_OpCode.rs);
    m_NextInstruction = PipelineStage::DoDelaySlot;

    uint32_t target = (m_CompilePC + 4 + (static_cast<int16_t>(m_OpCode.offset) << 2)) & 0xFFC;
    m_Assembler->CompConstToVariable(&m_GPR[m_OpCode.rs].UW, "GPR[rs]", 0);
    AsmLabel branchLabel = m_Assembler->CreateLabel();
    m_Assembler->JgLabel("BranchTaken", branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
    AsmLabel doneLabel = m_Assembler->CreateLabel();
    m_Assembler->JmpLabel("BranchDone", doneLabel);
    m_Assembler->BindLabel(branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    m_Assembler->BindLabel(doneLabel);
}

// ══════════════════════════════════════════════
// Immediate ALU — const propagation or emit code
// ══════════════════════════════════════════════

void CRSPRecompilerOps::ADDI()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return; // NOP to $zero

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) +
                       static_cast<uint32_t>(static_cast<int16_t>(m_OpCode.immediate));
        m_RegState.SetGprConst(m_OpCode.rt, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rt);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(static_cast<int16_t>(m_OpCode.immediate)));
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::ADDIU()
{
    ADDI();
}

void CRSPRecompilerOps::SLTI()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; SLTI r%u, r%u, %d\n", m_OpCode.rt, m_OpCode.rs, static_cast<int16_t>(m_OpCode.immediate));
    m_RegState.SetGprUnknown(m_OpCode.rt);
    
    // SLTI: rt = (rs < immediate) ? 1 : 0  [signed]
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
    m_Assembler->CompConstToVariable(&m_GPR[m_OpCode.rs].UW, "GPR[rs]", static_cast<uint32_t>(static_cast<int16_t>(m_OpCode.immediate)));
    m_Assembler->SetlVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]");
}

void CRSPRecompilerOps::SLTIU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; SLTIU r%u, r%u, %u\n", m_OpCode.rt, m_OpCode.rs, m_OpCode.immediate);
    m_RegState.SetGprUnknown(m_OpCode.rt);
    
    // SLTIU: rt = (rs < immediate) ? 1 : 0  [unsigned]
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
    m_Assembler->CompConstToVariable(&m_GPR[m_OpCode.rs].UW, "GPR[rs]", static_cast<uint32_t>(static_cast<int16_t>(m_OpCode.immediate)));
    m_Assembler->SetbVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]");
}

void CRSPRecompilerOps::ANDI()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) & m_OpCode.immediate;
        m_RegState.SetGprConst(m_OpCode.rt, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rt);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AndConstToX86reg(0, m_OpCode.immediate);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::ORI()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) | m_OpCode.immediate;
        m_RegState.SetGprConst(m_OpCode.rt, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rt);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->OrConstToX86reg(0, m_OpCode.immediate);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::XORI()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) ^ m_OpCode.immediate;
        m_RegState.SetGprConst(m_OpCode.rt, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", val);
        return;
    }

    // XOR with immediate: load rs into eax, XOR with immediate loaded in ecx
    m_RegState.SetGprUnknown(m_OpCode.rt);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", m_OpCode.immediate);
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->XorX86regToX86reg(0, 1);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::LUI()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    uint32_t val = static_cast<uint32_t>(m_OpCode.immediate) << 16;
    m_RegState.SetGprConst(m_OpCode.rt, val);
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", val);
}

// ══════════════════════════════════════════════
// SPECIAL opcodes (register-register ALU)
// ══════════════════════════════════════════════

void CRSPRecompilerOps::Special_SLL()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return; // NOP

    if (m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rt) << m_OpCode.sa;
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    if (m_OpCode.sa != 0)
        m_Assembler->ShiftLeftX86reg(0, m_OpCode.sa);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SRL()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rt) >> m_OpCode.sa;
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    if (m_OpCode.sa != 0)
        m_Assembler->ShiftRightX86reg(0, m_OpCode.sa);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SRA()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = static_cast<uint32_t>(
            static_cast<int32_t>(m_RegState.GetGprConstValue(m_OpCode.rt)) >> m_OpCode.sa);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    if (m_OpCode.sa != 0)
        m_Assembler->ArithShiftRightX86reg(0, m_OpCode.sa);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SLLV()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    m_Recompiler.Log("; SLLV r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rt, m_OpCode.rs);

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rt) << (m_RegState.GetGprConstValue(m_OpCode.rs) & 0x1F);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rd);
    // Load shift amount into RCX (CL), value into RAX
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rs].UW, "GPR[rs]"); // ECX
    m_Assembler->AndConstToX86reg(1, 0x1F);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]"); // EAX
    m_Assembler->ShiftLeftX86regByCL(0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SRLV()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    m_Recompiler.Log("; SRLV r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rt, m_OpCode.rs);

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rt) >> (m_RegState.GetGprConstValue(m_OpCode.rs) & 0x1F);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AndConstToX86reg(1, 0x1F);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->ShiftRightX86regByCL(0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SRAV()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    m_Recompiler.Log("; SRAV r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rt, m_OpCode.rs);

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = static_cast<uint32_t>(
            static_cast<int32_t>(m_RegState.GetGprConstValue(m_OpCode.rt)) >> (m_RegState.GetGprConstValue(m_OpCode.rs) & 0x1F));
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AndConstToX86reg(1, 0x1F);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->ArithShiftRightX86regByCL(0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_JR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; JR r%u\n", m_OpCode.rs);
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_RegState.WriteBackRegisters();

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        uint32_t target = m_RegState.GetGprConstValue(m_OpCode.rs) & 0xFFC;
        m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    }
    else
    {
        m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
        m_Assembler->MoveX86regToVariable(&m_System.m_JumpTo, "JumpToLocation", 0);
    }
}

void CRSPRecompilerOps::Special_JALR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; JALR r%u, r%u\n", m_OpCode.rd, m_OpCode.rs);
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_RegState.WriteBackRegisters();

    uint32_t returnAddr = (m_CompilePC + 8) & 0xFFC;
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", returnAddr);
    m_RegState.SetGprConst(m_OpCode.rd, returnAddr);

    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveX86regToVariable(&m_System.m_JumpTo, "JumpToLocation", 0);
}

void CRSPRecompilerOps::Special_BREAK()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BREAK\n");
    m_RegState.WriteBackRegisters();
    // Use interpreter to correctly set SP_STATUS (Halt|Broke) and raise MI interrupt
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Special_BREAK), "RSPOp::Special_BREAK");
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", 0xFFFFFFFF);
}

void CRSPRecompilerOps::Special_ADD()  { Special_ADDU(); }
void CRSPRecompilerOps::Special_ADDU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) +
                       m_RegState.GetGprConstValue(m_OpCode.rt);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    // Partial const propagation: if one operand is const, use AddConst
    if (m_RegState.IsGprConst(m_OpCode.rt))
    {
        m_RegState.SetGprUnknown(m_OpCode.rd);
        m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
        m_Assembler->AddConstToX86reg(0, m_RegState.GetGprConstValue(m_OpCode.rt));
        m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
        return;
    }
    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        m_RegState.SetGprUnknown(m_OpCode.rd);
        m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
        m_Assembler->AddConstToX86reg(0, m_RegState.GetGprConstValue(m_OpCode.rs));
        m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
        return;
    }

    // Both operands non-const: native reg-reg add
    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->AddX86regToX86reg(0, 1);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SUB()  { Special_SUBU(); }
void CRSPRecompilerOps::Special_SUBU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) -
                       m_RegState.GetGprConstValue(m_OpCode.rt);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    // Partial const: if rt is const, load rs and subtract the constant
    if (m_RegState.IsGprConst(m_OpCode.rt))
    {
        m_RegState.SetGprUnknown(m_OpCode.rd);
        m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
        m_Assembler->SubConstFromX86reg(0, m_RegState.GetGprConstValue(m_OpCode.rt));
        m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
        return;
    }

    // Both operands non-const: native reg-reg sub
    m_RegState.SetGprUnknown(m_OpCode.rd);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->SubX86regFromX86reg(0, 1);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_AND()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) &
                       m_RegState.GetGprConstValue(m_OpCode.rt);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_Recompiler.Log("; AND r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rs, m_OpCode.rt);
    m_RegState.SetGprUnknown(m_OpCode.rd);
    
    // rd = rs & rt (variable operands)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->AndX86regToX86reg(0, 1);  // AND eax, ecx
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_OR()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) |
                       m_RegState.GetGprConstValue(m_OpCode.rt);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_Recompiler.Log("; OR r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rs, m_OpCode.rt);
    m_RegState.SetGprUnknown(m_OpCode.rd);
    
    // rd = rs | rt (variable operands)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->OrX86regToX86reg(0, 1);  // OR eax, ecx
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_XOR()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = m_RegState.GetGprConstValue(m_OpCode.rs) ^
                       m_RegState.GetGprConstValue(m_OpCode.rt);
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_Recompiler.Log("; XOR r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rs, m_OpCode.rt);
    m_RegState.SetGprUnknown(m_OpCode.rd);
    
    // rd = rs ^ rt (variable operands)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->XorX86regToX86reg(0, 1);  // XOR eax, ecx
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_NOR()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    if (m_RegState.IsGprConst(m_OpCode.rs) && m_RegState.IsGprConst(m_OpCode.rt))
    {
        uint32_t val = ~(m_RegState.GetGprConstValue(m_OpCode.rs) |
                         m_RegState.GetGprConstValue(m_OpCode.rt));
        m_RegState.SetGprConst(m_OpCode.rd, val);
        m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", val);
        return;
    }

    m_Recompiler.Log("; NOR r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rs, m_OpCode.rt);
    m_RegState.SetGprUnknown(m_OpCode.rd);
    
    // rd = ~(rs | rt) (variable operands)
    // OR first, then invert via XOR with 0xFFFFFFFF
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");
    m_Assembler->OrX86regToX86reg(0, 1);  // OR eax, ecx
    m_Assembler->XorConstToX86reg(0, 0xFFFFFFFFU);  // XOR eax, 0xFFFFFFFF (NOT via XOR)
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
}

void CRSPRecompilerOps::Special_SLT()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    m_Recompiler.Log("; SLT r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rs, m_OpCode.rt);
    m_RegState.SetGprUnknown(m_OpCode.rd);
    
    // SLT: rd = (rs < rt) ? 1 : 0  [signed comparison]
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->CompX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
    m_Assembler->SetlVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]");
}

void CRSPRecompilerOps::Special_SLTU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rd == 0) return;

    m_Recompiler.Log("; SLTU r%u, r%u, r%u\n", m_OpCode.rd, m_OpCode.rs, m_OpCode.rt);
    m_RegState.SetGprUnknown(m_OpCode.rd);
    
    // SLTU: rd = (rs < rt) ? 1 : 0  [unsigned comparison]
    m_Assembler->MoveConstToVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]", 0);
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->CompX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
    m_Assembler->SetbVariable(&m_GPR[m_OpCode.rd].UW, "GPR[rd]");
}

// ══════════════════════════════════════════════
// RegImm branches
// ══════════════════════════════════════════════

void CRSPRecompilerOps::RegImm_BLTZ()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BLTZ r%u\n", m_OpCode.rs);
    m_NextInstruction = PipelineStage::DoDelaySlot;

    uint32_t target = (m_CompilePC + 4 + (static_cast<int16_t>(m_OpCode.offset) << 2)) & 0xFFC;

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        int32_t val = static_cast<int32_t>(m_RegState.GetGprConstValue(m_OpCode.rs));
        m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation",
            val < 0 ? target : (m_CompilePC + 8) & 0xFFC);
        return;
    }

    m_Assembler->CompConstToVariable(&m_GPR[m_OpCode.rs].UW, "GPR[rs]", 0);
    AsmLabel branchLabel = m_Assembler->CreateLabel();
    m_Assembler->JlLabel("BranchTaken", branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
    AsmLabel doneLabel = m_Assembler->CreateLabel();
    m_Assembler->JmpLabel("BranchDone", doneLabel);
    m_Assembler->BindLabel(branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    m_Assembler->BindLabel(doneLabel);
}

void CRSPRecompilerOps::RegImm_BGEZ()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; BGEZ r%u\n", m_OpCode.rs);
    m_NextInstruction = PipelineStage::DoDelaySlot;

    uint32_t target = (m_CompilePC + 4 + (static_cast<int16_t>(m_OpCode.offset) << 2)) & 0xFFC;

    if (m_RegState.IsGprConst(m_OpCode.rs))
    {
        int32_t val = static_cast<int32_t>(m_RegState.GetGprConstValue(m_OpCode.rs));
        m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation",
            val >= 0 ? target : (m_CompilePC + 8) & 0xFFC);
        return;
    }

    m_Assembler->CompConstToVariable(&m_GPR[m_OpCode.rs].UW, "GPR[rs]", 0);
    AsmLabel branchLabel = m_Assembler->CreateLabel();
    m_Assembler->JgeLabel("BranchTaken", branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", (m_CompilePC + 8) & 0xFFC);
    AsmLabel doneLabel = m_Assembler->CreateLabel();
    m_Assembler->JmpLabel("BranchDone", doneLabel);
    m_Assembler->BindLabel(branchLabel);
    m_Assembler->MoveConstToVariable(&m_System.m_JumpTo, "JumpToLocation", target);
    m_Assembler->BindLabel(doneLabel);
}

void CRSPRecompilerOps::RegImm_BLTZAL()
{
    if (m_Assembler == nullptr) return;
    uint32_t returnAddr = (m_CompilePC + 8) & 0xFFC;
    m_Assembler->MoveConstToVariable(&m_GPR[31].UW, "GPR[31]", returnAddr);
    m_RegState.SetGprConst(31, returnAddr);
    RegImm_BLTZ();
}

void CRSPRecompilerOps::RegImm_BGEZAL()
{
    if (m_Assembler == nullptr) return;
    uint32_t returnAddr = (m_CompilePC + 8) & 0xFFC;
    m_Assembler->MoveConstToVariable(&m_GPR[31].UW, "GPR[31]", returnAddr);
    m_RegState.SetGprConst(31, returnAddr);
    RegImm_BGEZ();
}

// ══════════════════════════════════════════════
// COP0 (system register access)
// ══════════════════════════════════════════════

void CRSPRecompilerOps::Cop0_MF()
{
    if (m_Assembler == nullptr) return;
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Cop0_MF), "RSPOp::Cop0_MF");
    m_RegState.SetGprUnknown(m_OpCode.rt);
}

void CRSPRecompilerOps::Cop0_MT()
{
    if (m_Assembler == nullptr) return;
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Cop0_MT), "RSPOp::Cop0_MT");
}

// ══════════════════════════════════════════════
// COP2 (vector scalar moves)
// ══════════════════════════════════════════════

void CRSPRecompilerOps::Cop2_MF()
{
    if (m_Assembler == nullptr) return;
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Cop2_MF), "RSPOp::Cop2_MF");
    m_RegState.SetGprUnknown(m_OpCode.rt);
}

void CRSPRecompilerOps::Cop2_CF()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; CFC2 r%u, $%u\n", m_OpCode.rt, m_OpCode.rd);

    // CFC2 needs to pack 8x16-bit flag lanes into an 8-bit value per flag,
    // then combine lo|hi — use interpreter for correctness
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Cop2_CF), "RSPOp::Cop2_CF");
    m_RegState.SetGprUnknown(m_OpCode.rt);
}

void CRSPRecompilerOps::Cop2_MT()
{
    if (m_Assembler == nullptr) return;
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Cop2_MT), "RSPOp::Cop2_MT");
    m_RegState.SetVecRegUnknown(m_OpCode.vs);
}

void CRSPRecompilerOps::Cop2_CT()
{
    if (m_Assembler == nullptr) return;

    m_Recompiler.Log("; CTC2 r%u, $%u\n", m_OpCode.rt, m_OpCode.rd);

    // CTC2 needs to unpack a 16-bit GPR value into separate 8x16-bit flag lanes
    // for both lo and hi flags — use interpreter for correctness
    Cheat_r4300iOpcode(static_cast<void(RSPOp::*)()>(&RSPOp::Cop2_CT), "RSPOp::Cop2_CT");
}

// ══════════════════════════════════════════════
// COP2 Vector dispatch
// ══════════════════════════════════════════════

void CRSPRecompilerOps::COP2_VECTOR()
{
    switch (m_OpCode.funct) {
    case RSP_VECTOR_VMULF:  Vector_VMULF();  break;
    case RSP_VECTOR_VMULU:  Vector_VMULU();  break;
    case RSP_VECTOR_VRNDP:  Vector_VRNDP();  break;
    case RSP_VECTOR_VMULQ:  Vector_VMULQ();  break;
    case RSP_VECTOR_VMUDL:  Vector_VMUDL();  break;
    case RSP_VECTOR_VMUDM:  Vector_VMUDM();  break;
    case RSP_VECTOR_VMUDN:  Vector_VMUDN();  break;
    case RSP_VECTOR_VMUDH:  Vector_VMUDH();  break;
    case RSP_VECTOR_VMACF:  Vector_VMACF();  break;
    case RSP_VECTOR_VMACU:  Vector_VMACU();  break;
    case RSP_VECTOR_VMACQ:  Vector_VMACQ();  break;
    case RSP_VECTOR_VMADL:  Vector_VMADL();  break;
    case RSP_VECTOR_VMADM:  Vector_VMADM();  break;
    case RSP_VECTOR_VMADN:  Vector_VMADN();  break;
    case RSP_VECTOR_VMADH:  Vector_VMADH();  break;
    case RSP_VECTOR_VADD:   Vector_VADD();   break;
    case RSP_VECTOR_VSUB:   Vector_VSUB();   break;
    case RSP_VECTOR_VABS:   Vector_VABS();   break;
    case RSP_VECTOR_VADDC:  Vector_VADDC();  break;
    case RSP_VECTOR_VSUBC:  Vector_VSUBC();  break;
    case RSP_VECTOR_VSAW:   Vector_VSAW();   break;
    case RSP_VECTOR_VLT:    Vector_VLT();    break;
    case RSP_VECTOR_VEQ:    Vector_VEQ();    break;
    case RSP_VECTOR_VNE:    Vector_VNE();    break;
    case RSP_VECTOR_VGE:    Vector_VGE();    break;
    case RSP_VECTOR_VCL:    Vector_VCL();    break;
    case RSP_VECTOR_VCH:    Vector_VCH();    break;
    case RSP_VECTOR_VCR:    Vector_VCR();    break;
    case RSP_VECTOR_VMRG:   Vector_VMRG();   break;
    case RSP_VECTOR_VAND:   Vector_VAND();   break;
    case RSP_VECTOR_VNAND:  Vector_VNAND();  break;
    case RSP_VECTOR_VOR:    Vector_VOR();    break;
    case RSP_VECTOR_VNOR:   Vector_VNOR();   break;
    case RSP_VECTOR_VXOR:   Vector_VXOR();   break;
    case RSP_VECTOR_VNXOR:  Vector_VNXOR();  break;
    case RSP_VECTOR_VRCP:   Vector_VRCP();   break;
    case RSP_VECTOR_VRCPL:  Vector_VRCPL();  break;
    case RSP_VECTOR_VRCPH:  Vector_VRCPH();  break;
    case RSP_VECTOR_VMOV:   Vector_VMOV();   break;
    case RSP_VECTOR_VRSQ:   Vector_VRSQ();   break;
    case RSP_VECTOR_VRSQL:  Vector_VRSQL();  break;
    case RSP_VECTOR_VRSQH:  Vector_VRSQH();  break;
    case RSP_VECTOR_VNOP:  Vector_VNOOP();  break;
    default:                Vector_Reserved(); break;
    }
}

// ══════════════════════════════════════════════
// Vector opcodes — XMM-accelerated code emission
// Core arithmetic/logic ops use SSE2 via CX64RspVectorGen.
// Reciprocal/sqrt and edge-case ops fall back to interpreter.
// ══════════════════════════════════════════════

// Helper: emit vector op preamble — null check only
// WriteBackRegisters is handled by Cheat_r4300iOpcode, no need to double-flush
#define VECTOR_PREAMBLE() \
    if (m_Assembler == nullptr) return

// -- Interpreter fallback for ops not yet natively compiled --
#define VECTOR_FALLBACK(name) \
    void CRSPRecompilerOps::Vector_##name() { \
        VECTOR_PREAMBLE(); \
        Cheat_r4300iOpcode(&RSPOp::Vector_##name, "RSP_Vector_" #name); \
    }

// ── Multiply operations (native SSE2) ──

void CRSPRecompilerOps::Vector_VMULF()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMULF vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMULF: signed fraction multiply
    // acc = vs*vt*2 + 0x8000, vd = clamp_signed(acc >> 16)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    // Compute products: lo = pmullw(vs,vt), hi = pmulhw(vs,vt)
    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Double the 32-bit product: (hi:lo) <<= 1
    // carry = bit 15 of lo (will shift into hi)
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmLo);  // reuse xmmVs as temp
    m_Assembler->Psraw_XmmImm(xmmVs, 15);          // carry from lo bit 15
    m_Assembler->Paddw_XmmToXmm(xmmLo, xmmLo);    // lo <<= 1
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmHi);    // hi <<= 1
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmVs);    // hi += carry (sub -1 = add 1)

    // Add rounding constant 0x8000 to lo, propagate carry to hi
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmLo);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);          // carry from lo + 0x8000
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVt);  // reuse xmmVt for 0x8000
    m_Assembler->Psllw_XmmImm(xmmVt, 15);
    m_Assembler->Paddw_XmmToXmm(xmmLo, xmmVt);    // lo += 0x8000
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmVs);    // hi += carry

    // Store accumulator
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmHi);
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmVs);

    // Clamp: only -32768*-32768 overflows, producing hi=0x8000 instead of 0x7FFF
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmHi);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVs, xmmVt);  // xmmVt = 0x8000, match overflow
    m_Assembler->Pxor_XmmToXmm(xmmHi, xmmVs);      // 0x8000 XOR 0xFFFF = 0x7FFF

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmHi);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmHi);
}

void CRSPRecompilerOps::Vector_VMULU()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMULU vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMULU: signed fraction multiply with unsigned clamp (0..0xFFFF)
    // acc = vs*vt*2 + 0x8000 (same as VMULF)
    // vd = clamp_unsigned(acc >> 16): if negative→0, else acc_mid
    // Overflow case (-32768*-32768): acc_hi=0 but acc_mid=0x8000→positive, result=0x8000.
    // In signed 16-bit 0x8000 is negative but the 32-bit value (0x0000_8000) is positive
    // so VMULU should output 0x8000 (not 0). The only clamp-to-zero case is when acc_hi=0xFFFF.
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    // Products
    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Double (hi:lo) <<= 1
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmLo);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);
    m_Assembler->Paddw_XmmToXmm(xmmLo, xmmLo);
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmHi);
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmVs);

    // Add rounding 0x8000
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmLo);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Psllw_XmmImm(xmmVt, 15);          // xmmVt = 0x8000
    m_Assembler->Paddw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmVs);

    // Store accumulator
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmHi);
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);          // acc_hi = sign-extend of mid
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmVs);

    // Unsigned clamp: if acc_hi == 0xFFFF (negative 32-bit value) → zero it
    // xmmVs = acc_hi = 0 (positive) or 0xFFFF (negative)
    // pandn(neg_mask, acc_mid) → zero where negative, acc_mid elsewhere
    m_Assembler->Pandn_XmmToXmm(xmmVs, xmmHi);    // ~neg_mask & acc_mid

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmHi);
}

void CRSPRecompilerOps::Vector_VMUDL()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMUDL vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMUDL: unsigned multiply low. vd = high16(unsigned_vs * unsigned_vt)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // pmulhuw gives the high 16 bits of unsigned multiply
    m_Assembler->Pmulhuw_XmmToXmm(xmmVs, xmmVt);

    // Store accumulator: hi=0, mid=0, lo=result
    m_Assembler->Pxor_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmVt);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmVt);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

void CRSPRecompilerOps::Vector_VMUDM()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMUDM vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMUDM: signed vs * unsigned vt, vd = accum_mid
    // pmulhw treats both as signed. For signed*unsigned:
    // correction = where vt has bit 15 set, add vs (since vt was treated as negative)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Correction for unsigned vt: add vs where vt bit 15 set
    uint8_t xmmCorr = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmCorr, xmmVt);
    m_Assembler->Psraw_XmmImm(xmmCorr, 15);  // 0xFFFF where vt "negative" (unsigned bit 15)
    m_Assembler->Pand_XmmToXmm(xmmCorr, xmmVs);  // vs where vt unsigned-high
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmCorr);   // correct hi

    // Store accumulator: VMUDM result = signed*unsigned 32-bit product
    // Hi = sign-extend of product[31], Mid = product[16:31], Lo = product[0:15]
    m_Assembler->Movdqa_XmmToXmm(xmmCorr, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmCorr, 15);   // sign-extend
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmCorr);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmHi);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmLo);

    // vd = accum_mid = product[16:31]
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmHi);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmHi);
}

void CRSPRecompilerOps::Vector_VMUDN()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMUDN vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMUDN: unsigned vs * signed vt, vd = accum_lo = pmullw(vs,vt)
    // pmullw gives the same low 16 bits regardless of signedness
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);

    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Correction for unsigned vs: add vt where vs bit 15 set
    uint8_t xmmCorr = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmCorr, xmmVs);
    m_Assembler->Psraw_XmmImm(xmmCorr, 15);
    m_Assembler->Pand_XmmToXmm(xmmCorr, xmmVt);
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmCorr);

    // Store accumulator: VMUDN = unsigned*signed 32-bit product
    // Hi = sign-extend of product[31], Mid = product[16:31], Lo = product[0:15]
    m_Assembler->Movdqa_XmmToXmm(xmmCorr, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmCorr, 15);   // sign-extend
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmCorr);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmHi);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmLo);

    // vd = accum_lo = product[0:15]
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmLo);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmLo);
}

void CRSPRecompilerOps::Vector_VMUDH()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMUDH vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMUDH: signed multiply high, vd = clamp(vs*vt as 32-bit → 16-bit)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    // Compute lo = pmullw(vs,vt), hi = pmulhw(vs,vt)
    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Store accumulator: hi=product_hi, mid=product_lo, lo=0
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmHi);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmLo);
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);

    // vd = clamp(32-bit product → 16-bit signed)
    // Interleave lo/hi words into dwords, then pack with signed saturation
    uint8_t xmmDLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDLo);
    m_Assembler->Movdqa_XmmToXmm(xmmDLo, xmmLo);
    m_Assembler->Punpcklwd_XmmToXmm(xmmDLo, xmmHi);  // dwords lanes 0-3
    m_Assembler->Punpckhwd_XmmToXmm(xmmLo, xmmHi);    // dwords lanes 4-7
    m_Assembler->Packssdw_XmmToXmm(xmmDLo, xmmLo);    // 8 clamped words

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmDLo);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmHi);
    m_RegState.UnprotectXmm(xmmDLo);
}

// ── MAC helper: signed saturation from acc_hi:acc_mid ──
// Shared by VMACF, VMADH, VMADM. Result goes into xmmMid.
void CRSPRecompilerOps::EmitSignedSaturation(uint8_t xmmMid, uint8_t xmmAccHi)
{
    // sign_mid = psraw(mid, 15) → 0x0000 if mid>=0, 0xFFFF if mid<0
    uint8_t xmmSignMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmSignMid, xmmMid);
    m_Assembler->Psraw_XmmImm(xmmSignMid, 15);

    // match = (hi == sign_mid) → lanes where hi is proper sign extension of mid
    uint8_t xmmMatch = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMatch, xmmAccHi);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMatch, xmmSignMid);

    // neg = psraw(hi, 15) → 0xFFFF where hi<0 (underflow indicator)
    uint8_t xmmNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmNeg, xmmAccHi);
    m_Assembler->Psraw_XmmImm(xmmNeg, 15);

    // overflow_val = pandn(neg, allones) → 0xFFFF (0x7FFF after blend) for pos overflow, 0x0000 for neg
    uint8_t xmmOvf = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmOvf, xmmOvf);      // allones
    m_Assembler->Pandn_XmmToXmm(xmmNeg, xmmOvf);         // ~neg & allones

    // Blend: result = (mid & match) | (~match & overflow_val)
    m_Assembler->Pand_XmmToXmm(xmmMid, xmmMatch);        // mid where in range
    m_Assembler->Pandn_XmmToXmm(xmmMatch, xmmNeg);       // overflow_val where clamped
    m_Assembler->Por_XmmToXmm(xmmMid, xmmMatch);         // final saturated result
}

// ── MAC helper: unsigned carry detection ──
// Returns carry mask: all-ones per lane where (old + addend) wrapped unsigned.
// xmmOld and xmmNew are consumed.
uint8_t CRSPRecompilerOps::EmitUnsignedCarryDetect(uint8_t xmmOld, uint8_t xmmNew, uint8_t xmmConst8000)
{
    // Convert unsigned comparison to signed via XOR 0x8000
    m_Assembler->Pxor_XmmToXmm(xmmOld, xmmConst8000);
    m_Assembler->Pxor_XmmToXmm(xmmNew, xmmConst8000);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmOld, xmmNew);  // carry mask in xmmOld
    return xmmOld;
}

// ── Multiply-accumulate (native SSE2) ──

void CRSPRecompilerOps::Vector_VMACF()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMACF vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMACF: acc += vs*vt*2 (doubled signed fraction multiply-accumulate)
    // vd = Saturate(signed) from acc_hi:acc_mid

    // --- Product computation ---
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Double: (hi:lo) <<= 1
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmLo);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);           // carry bit from lo
    m_Assembler->Paddw_XmmToXmm(xmmLo, xmmLo);     // lo <<= 1
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmHi);     // hi <<= 1
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmVs);     // hi += carry

    // Product sign extension (bits 32-47)
    uint8_t xmmSign = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSign);
    m_Assembler->Movdqa_XmmToXmm(xmmSign, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmSign, 15);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);

    // --- Constant 0x8000 for unsigned carry detection ---
    uint8_t xmmConst = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmConst);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmConst, xmmConst);
    m_Assembler->Psllw_XmmImm(xmmConst, 15);  // 0x8000

    // --- Step 1: acc_lo += product_lo, detect carry ---
    uint8_t xmmOld = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOld);
    m_Assembler->Movdqa_XmmFromMem(xmmOld, m_ACCUM.LowData(), "ACCUM_Lo");
    uint8_t xmmNew = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNew);
    m_Assembler->Movdqa_XmmToXmm(xmmNew, xmmOld);
    m_Assembler->Paddw_XmmToXmm(xmmNew, xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmNew);

    // carry_lo = (old > new) unsigned
    EmitUnsignedCarryDetect(xmmOld, xmmNew, xmmConst);  // result in xmmOld
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmNew);

    // --- Step 2: acc_mid += product_hi + carry_lo ---
    uint8_t xmmOldMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOldMid);
    uint8_t xmmMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMid);
    m_Assembler->Movdqa_XmmFromMem(xmmOldMid, m_ACCUM.MidData(), "ACCUM_Mid");
    m_Assembler->Movdqa_XmmToXmm(xmmMid, xmmOldMid);

    // Add product_hi first
    m_Assembler->Paddw_XmmToXmm(xmmMid, xmmHi);

    // Carry1 from adding product_hi: (old_mid > mid_after_hi) unsigned
    uint8_t xmmC1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmC1);
    m_Assembler->Movdqa_XmmToXmm(xmmC1, xmmOldMid);
    uint8_t xmmT = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT, xmmMid);
    EmitUnsignedCarryDetect(xmmC1, xmmT, xmmConst);  // carry1 in xmmC1

    // Carry2 from adding carry_lo: mid==0xFFFF AND carry_lo active
    uint8_t xmmC2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmC2, xmmC2);  // allones = 0xFFFF
    uint8_t xmmMaxCk = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMaxCk, xmmMid);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMaxCk, xmmC2);  // 0xFFFF where mid==max
    m_Assembler->Pand_XmmToXmm(xmmMaxCk, xmmOld);     // AND carry_lo → carry2

    // Add carry_lo to mid
    m_Assembler->Psubw_XmmToXmm(xmmMid, xmmOld);  // mid += carry (sub -1 = +1)
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmMid);

    // Total carry_mid = carry1 | carry2
    m_Assembler->Por_XmmToXmm(xmmC1, xmmMaxCk);  // xmmC1 = carry_mid
    m_RegState.UnprotectXmm(xmmOld);
    m_RegState.UnprotectXmm(xmmOldMid);
    m_RegState.UnprotectXmm(xmmHi);

    // --- Step 3: acc_hi += product_sign + carry_mid ---
    uint8_t xmmAccHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAccHi);
    m_Assembler->Movdqa_XmmFromMem(xmmAccHi, m_ACCUM.HighData(), "ACCUM_Hi");
    m_Assembler->Paddw_XmmToXmm(xmmAccHi, xmmSign);   // + sign extension
    m_Assembler->Psubw_XmmToXmm(xmmAccHi, xmmC1);     // + carry_mid
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmAccHi);
    m_RegState.UnprotectXmm(xmmSign);
    m_RegState.UnprotectXmm(xmmC1);

    // --- Step 4: Signed saturation → vd ---
    EmitSignedSaturation(xmmMid, xmmAccHi);

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMid);

    m_RegState.UnprotectXmm(xmmMid);
    m_RegState.UnprotectXmm(xmmConst);
    m_RegState.UnprotectXmm(xmmAccHi);
}

void CRSPRecompilerOps::Vector_VMACU()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMACU vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMACU: acc += vs*vt*2 (same accumulation as VMACF)
    // vd = Unsigned saturation: hi<0 → 0, hi!=0||mid<0 → 0xFFFF, else → mid

    // --- Product computation (identical to VMACF) ---
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Double: (hi:lo) <<= 1
    m_Assembler->Movdqa_XmmToXmm(xmmVs, xmmLo);
    m_Assembler->Psraw_XmmImm(xmmVs, 15);           // carry bit from lo
    m_Assembler->Paddw_XmmToXmm(xmmLo, xmmLo);     // lo <<= 1
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmHi);     // hi <<= 1
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmVs);     // hi += carry

    // Product sign extension (bits 32-47)
    uint8_t xmmSign = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSign);
    m_Assembler->Movdqa_XmmToXmm(xmmSign, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmSign, 15);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);

    // --- Constant 0x8000 for unsigned carry detection ---
    uint8_t xmmConst = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmConst);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmConst, xmmConst);
    m_Assembler->Psllw_XmmImm(xmmConst, 15);  // 0x8000

    // --- Step 1: acc_lo += product_lo, detect carry ---
    uint8_t xmmOld = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOld);
    m_Assembler->Movdqa_XmmFromMem(xmmOld, m_ACCUM.LowData(), "ACCUM_Lo");
    uint8_t xmmNew = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNew);
    m_Assembler->Movdqa_XmmToXmm(xmmNew, xmmOld);
    m_Assembler->Paddw_XmmToXmm(xmmNew, xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmNew);

    // carry_lo = (old > new) unsigned
    m_Assembler->Pxor_XmmToXmm(xmmOld, xmmConst);
    m_Assembler->Pxor_XmmToXmm(xmmNew, xmmConst);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmOld, xmmNew);  // xmmOld = carry_lo mask
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmNew);

    // --- Step 2: acc_mid += product_hi + carry_lo ---
    uint8_t xmmOldMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOldMid);
    uint8_t xmmMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMid);
    m_Assembler->Movdqa_XmmFromMem(xmmOldMid, m_ACCUM.MidData(), "ACCUM_Mid");
    m_Assembler->Movdqa_XmmToXmm(xmmMid, xmmOldMid);

    // Add product_hi first
    m_Assembler->Paddw_XmmToXmm(xmmMid, xmmHi);

    // Carry1 from adding product_hi
    uint8_t xmmC1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmC1);
    m_Assembler->Movdqa_XmmToXmm(xmmC1, xmmOldMid);
    m_Assembler->Pxor_XmmToXmm(xmmC1, xmmConst);
    uint8_t xmmT = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT, xmmMid);
    m_Assembler->Pxor_XmmToXmm(xmmT, xmmConst);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmC1, xmmT);  // carry1

    // Carry2 from adding carry_lo: mid==0xFFFF AND carry_lo active
    uint8_t xmmC2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmC2, xmmC2);  // allones = 0xFFFF
    uint8_t xmmMaxCk = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMaxCk, xmmMid);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMaxCk, xmmC2);  // 0xFFFF where mid==max
    m_Assembler->Pand_XmmToXmm(xmmMaxCk, xmmOld);     // AND carry_lo → carry2

    // Add carry_lo to mid
    m_Assembler->Psubw_XmmToXmm(xmmMid, xmmOld);  // mid += carry (sub -1 = +1)
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmMid);

    // Total carry_mid = carry1 | carry2
    m_Assembler->Por_XmmToXmm(xmmC1, xmmMaxCk);  // xmmC1 = carry_mid
    m_RegState.UnprotectXmm(xmmOld);
    m_RegState.UnprotectXmm(xmmOldMid);
    m_RegState.UnprotectXmm(xmmHi);

    // --- Step 3: acc_hi += product_sign + carry_mid ---
    uint8_t xmmAccHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAccHi);
    m_Assembler->Movdqa_XmmFromMem(xmmAccHi, m_ACCUM.HighData(), "ACCUM_Hi");
    m_Assembler->Paddw_XmmToXmm(xmmAccHi, xmmSign);   // + sign extension
    m_Assembler->Psubw_XmmToXmm(xmmAccHi, xmmC1);     // + carry_mid
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmAccHi);
    m_RegState.UnprotectXmm(xmmSign);
    m_RegState.UnprotectXmm(xmmC1);

    // --- Step 4: Unsigned saturation → vd ---
    // hi < 0 → 0;  hi != 0 || mid < 0 → 0xFFFF;  else → mid

    // neg_mask = (hi < 0) → all 1s where hi is negative
    uint8_t xmmNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNeg);
    m_Assembler->Movdqa_XmmToXmm(xmmNeg, xmmAccHi);
    m_Assembler->Psraw_XmmImm(xmmNeg, 15);

    // hi_is_zero = pcmpeqw(hi, 0)
    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    uint8_t xmmHiZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmHiZero, xmmAccHi);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmHiZero, xmmZero);

    // mid_neg = (mid < 0) → all 1s where mid bit 15 is set
    uint8_t xmmMidNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMidNeg, xmmMid);
    m_Assembler->Psraw_XmmImm(xmmMidNeg, 15);

    // overflow = (~hi_is_zero) | mid_neg = (hi != 0) | (mid < 0)
    uint8_t xmmOvf = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmOvf, xmmOvf);  // all ones
    m_Assembler->Pxor_XmmToXmm(xmmHiZero, xmmOvf);   // ~hi_is_zero = hi_nonzero
    m_Assembler->Por_XmmToXmm(xmmHiZero, xmmMidNeg);  // overflow mask

    // result = mid | overflow (overflow lanes → 0xFFFF since 0xFFFF | anything = 0xFFFF)
    m_Assembler->Por_XmmToXmm(xmmMid, xmmHiZero);

    // Apply underflow: result = result & ~neg_mask (zero out where hi < 0)
    // PANDN: dst = ~dst & src → xmmNeg = ~neg_mask & result
    m_Assembler->Pandn_XmmToXmm(xmmNeg, xmmMid);

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmNeg);

    m_RegState.UnprotectXmm(xmmMid);
    m_RegState.UnprotectXmm(xmmConst);
    m_RegState.UnprotectXmm(xmmAccHi);
    m_RegState.UnprotectXmm(xmmNeg);
}

void CRSPRecompilerOps::Vector_VMADL()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMADL vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMADL: acc += (unsigned(vs) * unsigned(vt)) >> 16
    // The product is just the high 16 bits of unsigned multiply → added to acc_lo
    // with carry propagating through acc_mid and acc_hi
    // vd = Saturate(unsigned) from acc_hi:acc_mid:acc_lo

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Product = pmulhuw(vs, vt): high 16 bits of unsigned multiply
    m_Assembler->Pmulhuw_XmmToXmm(xmmVs, xmmVt);
    m_RegState.UnprotectXmm(xmmVt);

    // --- Constant 0x8000 for unsigned carry detection ---
    uint8_t xmmConst = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmConst);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmConst, xmmConst);
    m_Assembler->Psllw_XmmImm(xmmConst, 15);  // 0x8000

    // allones for later
    uint8_t xmmAllOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAllOnes);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmAllOnes, xmmAllOnes);

    // --- Step 1: acc_lo += product, detect carry ---
    uint8_t xmmOld = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOld);
    m_Assembler->Movdqa_XmmFromMem(xmmOld, m_ACCUM.LowData(), "ACCUM_Lo");
    uint8_t xmmNew = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNew);
    m_Assembler->Movdqa_XmmToXmm(xmmNew, xmmOld);
    m_Assembler->Paddw_XmmToXmm(xmmNew, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmNew);

    // carry_lo = (old > new) unsigned via XOR 0x8000 trick
    m_Assembler->Pxor_XmmToXmm(xmmOld, xmmConst);
    m_Assembler->Pxor_XmmToXmm(xmmNew, xmmConst);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmOld, xmmNew);  // carry_lo mask (0xFFFF/-1)
    m_RegState.UnprotectXmm(xmmNew);
    // xmmOld = carry_lo

    // --- Step 2: acc_mid += carry_lo ---
    uint8_t xmmMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMid);
    m_Assembler->Movdqa_XmmFromMem(xmmMid, m_ACCUM.MidData(), "ACCUM_Mid");

    // carry_mid = (mid == 0xFFFF) AND carry_lo → mid would wrap
    uint8_t xmmCMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCMid);
    m_Assembler->Movdqa_XmmToXmm(xmmCMid, xmmMid);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmCMid, xmmAllOnes);
    m_Assembler->Pand_XmmToXmm(xmmCMid, xmmOld);  // carry_mid mask

    // mid -= carry_lo (sub -1 = +1 where carry active)
    m_Assembler->Psubw_XmmToXmm(xmmMid, xmmOld);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmMid);
    m_RegState.UnprotectXmm(xmmOld);

    // --- Step 3: acc_hi += carry_mid ---
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);
    m_Assembler->Movdqa_XmmFromMem(xmmHi, m_ACCUM.HighData(), "ACCUM_Hi");
    m_Assembler->Psubw_XmmToXmm(xmmHi, xmmCMid);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmHi);
    m_RegState.UnprotectXmm(xmmCMid);

    // --- Unsigned saturation: vd ---
    // if hi < 0 → 0, if hi > 0 or (hi==0 && mid<0) → 0xFFFF, else → lo
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    m_Assembler->Movdqa_XmmFromMem(xmmLo, m_ACCUM.LowData(), "ACCUM_Lo");

    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);

    // neg_mask = hi < 0
    uint8_t xmmNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNeg);
    m_Assembler->Movdqa_XmmToXmm(xmmNeg, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmNeg, 15);

    // pos_mask = hi > 0 (signed)
    uint8_t xmmPos = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmPos, xmmHi);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmZero, xmmPos);  // wait: pcmpgtw(0, hi) = 0 > hi? no...
    // Redo: pcmpgtw(a,b) = a > b. Want hi > 0
    // xmmZero was zero; pcmpgtw(hi, zero) would give hi > 0
    m_Assembler->Movdqa_XmmToXmm(xmmPos, xmmHi);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmPos, xmmZero);  // pos_strict: 0xFFFF where hi > 0

    // hi_zero AND mid_neg
    m_Assembler->Pcmpeqw_XmmToXmm(xmmZero, xmmHi);  // 0xFFFF where hi == 0
    uint8_t xmmMidNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMidNeg, xmmMid);
    m_Assembler->Psraw_XmmImm(xmmMidNeg, 15);
    m_Assembler->Pand_XmmToXmm(xmmZero, xmmMidNeg);  // hi_zero & mid_neg

    // overflow = pos_strict | (hi_zero & mid_neg)
    m_Assembler->Por_XmmToXmm(xmmPos, xmmZero);  // xmmPos = overflow_mask

    // clamp = neg | overflow
    uint8_t xmmClamp = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmClamp, xmmNeg);
    m_Assembler->Por_XmmToXmm(xmmClamp, xmmPos);

    // normal = NOT clamp
    // result = (lo & normal) | overflow
    m_Assembler->Pandn_XmmToXmm(xmmClamp, xmmLo);  // xmmClamp = lo & NOT clamp (i.e., lo where normal)
    m_Assembler->Por_XmmToXmm(xmmClamp, xmmPos);    // | overflow (0xFFFF where overflow, 0 where underflow/normal already handled)

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmClamp);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmConst);
    m_RegState.UnprotectXmm(xmmAllOnes);
    m_RegState.UnprotectXmm(xmmOld); // already unprotected above, but just in case if changed flow
    m_RegState.UnprotectXmm(xmmMid);
    m_RegState.UnprotectXmm(xmmHi);
    m_RegState.UnprotectXmm(xmmLo);
    m_RegState.UnprotectXmm(xmmNeg);
}

void CRSPRecompilerOps::Vector_VMADM()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMADM vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMADM: acc += signed(vs) * unsigned(vt), vd = Saturate(signed)
    // 32-bit product added to 48-bit accumulator with carry propagation

    // --- Product computation: signed * unsigned ---
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);  // lo = low 16 bits
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);  // hi = signed high 16 bits

    // Correction for unsigned vt: add vs where vt bit 15 set
    uint8_t xmmCorr = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCorr);
    m_Assembler->Movdqa_XmmToXmm(xmmCorr, xmmVt);
    m_Assembler->Psraw_XmmImm(xmmCorr, 15);
    m_Assembler->Pand_XmmToXmm(xmmCorr, xmmVs);
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmCorr);

    // Product sign extension (bits 32-47)
    uint8_t xmmSign = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSign);
    m_Assembler->Movdqa_XmmToXmm(xmmSign, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmSign, 15);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmCorr);

    // --- Constant 0x8000 for unsigned carry detection ---
    uint8_t xmmConst = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmConst);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmConst, xmmConst);
    m_Assembler->Psllw_XmmImm(xmmConst, 15);  // 0x8000

    // --- Step 1: acc_lo += product_lo, detect carry ---
    uint8_t xmmOld = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOld);
    m_Assembler->Movdqa_XmmFromMem(xmmOld, m_ACCUM.LowData(), "ACCUM_Lo");
    uint8_t xmmNew = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNew);
    m_Assembler->Movdqa_XmmToXmm(xmmNew, xmmOld);
    m_Assembler->Paddw_XmmToXmm(xmmNew, xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmNew);

    // carry_lo = (old > new) unsigned
    EmitUnsignedCarryDetect(xmmOld, xmmNew, xmmConst);  // result in xmmOld
    m_RegState.UnprotectXmm(xmmNew);
    m_RegState.UnprotectXmm(xmmLo);
    // xmmOld = carry_lo mask

    // --- Step 2: acc_mid += product_hi + carry_lo ---
    uint8_t xmmOldMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOldMid);
    uint8_t xmmMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMid);
    m_Assembler->Movdqa_XmmFromMem(xmmOldMid, m_ACCUM.MidData(), "ACCUM_Mid");
    m_Assembler->Movdqa_XmmToXmm(xmmMid, xmmOldMid);

    // Add product_hi
    m_Assembler->Paddw_XmmToXmm(xmmMid, xmmHi);

    // Carry1: (old_mid > mid_after) unsigned
    uint8_t xmmC1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmC1);
    m_Assembler->Movdqa_XmmToXmm(xmmC1, xmmOldMid);
    uint8_t xmmT = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT, xmmMid);
    EmitUnsignedCarryDetect(xmmC1, xmmT, xmmConst);  // carry1 in xmmC1

    // Carry2: mid==0xFFFF AND carry_lo
    uint8_t xmmAllOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmAllOnes, xmmAllOnes);
    uint8_t xmmMaxCk = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMaxCk, xmmMid);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMaxCk, xmmAllOnes);
    m_Assembler->Pand_XmmToXmm(xmmMaxCk, xmmOld);

    // Add carry_lo to mid
    m_Assembler->Psubw_XmmToXmm(xmmMid, xmmOld);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmMid);

    // Total carry_mid = carry1 | carry2
    m_Assembler->Por_XmmToXmm(xmmC1, xmmMaxCk);
    m_RegState.UnprotectXmm(xmmOld);
    m_RegState.UnprotectXmm(xmmOldMid);
    m_RegState.UnprotectXmm(xmmHi);

    // --- Step 3: acc_hi += product_sign + carry_mid ---
    uint8_t xmmAccHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAccHi);
    m_Assembler->Movdqa_XmmFromMem(xmmAccHi, m_ACCUM.HighData(), "ACCUM_Hi");
    m_Assembler->Paddw_XmmToXmm(xmmAccHi, xmmSign);
    m_Assembler->Psubw_XmmToXmm(xmmAccHi, xmmC1);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmAccHi);
    m_RegState.UnprotectXmm(xmmSign);
    m_RegState.UnprotectXmm(xmmC1);

    // --- Signed saturation → vd ---
    EmitSignedSaturation(xmmMid, xmmAccHi);

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMid);

    m_RegState.UnprotectXmm(xmmMid);
    m_RegState.UnprotectXmm(xmmConst);
    m_RegState.UnprotectXmm(xmmAccHi);
}

void CRSPRecompilerOps::Vector_VMADN()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMADN vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMADN: acc += unsigned(vs) * signed(vt), vd = Saturate(unsigned)
    // 32-bit product added to 48-bit accumulator with carry propagation

    // --- Product computation: unsigned * signed ---
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);

    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);

    // Correction for unsigned vs: add vt where vs bit 15 set
    uint8_t xmmCorr = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCorr);
    m_Assembler->Movdqa_XmmToXmm(xmmCorr, xmmVs);
    m_Assembler->Psraw_XmmImm(xmmCorr, 15);
    m_Assembler->Pand_XmmToXmm(xmmCorr, xmmVt);
    m_Assembler->Paddw_XmmToXmm(xmmHi, xmmCorr);

    // Product sign extension
    uint8_t xmmSign = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSign);
    m_Assembler->Movdqa_XmmToXmm(xmmSign, xmmHi);
    m_Assembler->Psraw_XmmImm(xmmSign, 15);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmCorr);

    // --- Constant 0x8000 ---
    uint8_t xmmConst = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmConst);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmConst, xmmConst);
    m_Assembler->Psllw_XmmImm(xmmConst, 15);

    // --- Step 1: acc_lo += product_lo, detect carry ---
    uint8_t xmmOld = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOld);
    m_Assembler->Movdqa_XmmFromMem(xmmOld, m_ACCUM.LowData(), "ACCUM_Lo");
    uint8_t xmmNew = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNew);
    m_Assembler->Movdqa_XmmToXmm(xmmNew, xmmOld);
    m_Assembler->Paddw_XmmToXmm(xmmNew, xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmNew);

    m_Assembler->Pxor_XmmToXmm(xmmOld, xmmConst);
    m_Assembler->Pxor_XmmToXmm(xmmNew, xmmConst);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmOld, xmmNew);
    m_RegState.UnprotectXmm(xmmNew);
    m_RegState.UnprotectXmm(xmmLo);
    // xmmOld = carry_lo

    // --- Step 2: acc_mid += product_hi + carry_lo ---
    uint8_t xmmOldMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOldMid);
    uint8_t xmmMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMid);
    m_Assembler->Movdqa_XmmFromMem(xmmOldMid, m_ACCUM.MidData(), "ACCUM_Mid");
    m_Assembler->Movdqa_XmmToXmm(xmmMid, xmmOldMid);

    m_Assembler->Paddw_XmmToXmm(xmmMid, xmmHi);

    // Carry1
    uint8_t xmmC1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmC1);
    m_Assembler->Movdqa_XmmToXmm(xmmC1, xmmOldMid);
    m_Assembler->Pxor_XmmToXmm(xmmC1, xmmConst);
    uint8_t xmmT = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT, xmmMid);
    m_Assembler->Pxor_XmmToXmm(xmmT, xmmConst);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmC1, xmmT);

    // Carry2: mid==0xFFFF AND carry_lo
    uint8_t xmmAllOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmAllOnes, xmmAllOnes);
    uint8_t xmmMaxCk = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMaxCk, xmmMid);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMaxCk, xmmAllOnes);
    m_Assembler->Pand_XmmToXmm(xmmMaxCk, xmmOld);

    // Add carry_lo to mid
    m_Assembler->Psubw_XmmToXmm(xmmMid, xmmOld);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmMid);

    // carry_mid = carry1 | carry2
    m_Assembler->Por_XmmToXmm(xmmC1, xmmMaxCk);
    m_RegState.UnprotectXmm(xmmOld);
    m_RegState.UnprotectXmm(xmmOldMid);
    m_RegState.UnprotectXmm(xmmHi);

    // --- Step 3: acc_hi += product_sign + carry_mid ---
    uint8_t xmmAccHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAccHi);
    m_Assembler->Movdqa_XmmFromMem(xmmAccHi, m_ACCUM.HighData(), "ACCUM_Hi");
    m_Assembler->Paddw_XmmToXmm(xmmAccHi, xmmSign);
    m_Assembler->Psubw_XmmToXmm(xmmAccHi, xmmC1);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmAccHi);
    m_RegState.UnprotectXmm(xmmSign);
    m_RegState.UnprotectXmm(xmmC1);

    // --- Unsigned saturation → vd ---
    // if hi < 0 → 0, if hi > 0 or (hi==0 && mid<0) → 0xFFFF, else → acc_lo
    uint8_t xmmAccLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAccLo);
    m_Assembler->Movdqa_XmmFromMem(xmmAccLo, m_ACCUM.LowData(), "ACCUM_Lo");

    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);

    // neg = hi < 0
    uint8_t xmmNeg2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmNeg2, xmmAccHi);
    m_Assembler->Psraw_XmmImm(xmmNeg2, 15);

    // pos = hi > 0
    uint8_t xmmPos = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmPos, xmmAccHi);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmPos, xmmZero);

    // hi_zero & mid_neg
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmZero, xmmAccHi);  // hi == 0
    uint8_t xmmMidNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmMidNeg, xmmMid);
    m_Assembler->Psraw_XmmImm(xmmMidNeg, 15);
    m_Assembler->Pand_XmmToXmm(xmmZero, xmmMidNeg);

    // overflow = pos | (hi_zero & mid_neg)
    m_Assembler->Por_XmmToXmm(xmmPos, xmmZero);

    // clamp = neg | overflow
    uint8_t xmmClamp = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmClamp, xmmNeg2);
    m_Assembler->Por_XmmToXmm(xmmClamp, xmmPos);

    // result = (acc_lo & NOT clamp) | overflow
    m_Assembler->Pandn_XmmToXmm(xmmClamp, xmmAccLo);
    m_Assembler->Por_XmmToXmm(xmmClamp, xmmPos);

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmClamp);

    m_RegState.UnprotectXmm(xmmMid);
    m_RegState.UnprotectXmm(xmmConst);
    m_RegState.UnprotectXmm(xmmAccHi);
    m_RegState.UnprotectXmm(xmmAccLo);
}

void CRSPRecompilerOps::Vector_VMADH()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMADH vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMADH: signed multiply-accumulate high
    // acc(hi:mid) += vs * vt (32-bit signed), acc_lo unchanged
    // vd = Saturate(signed) from acc_hi:acc_mid
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Products: lo = pmullw(vs,vt), hi = pmulhw(vs,vt)
    uint8_t xmmLo = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmLo);
    uint8_t xmmHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmHi);
    m_Assembler->Movdqa_XmmToXmm(xmmLo, xmmVs);
    m_Assembler->Pmullw_XmmToXmm(xmmLo, xmmVt);
    m_Assembler->Movdqa_XmmToXmm(xmmHi, xmmVs);
    m_Assembler->Pmulhw_XmmToXmm(xmmHi, xmmVt);
    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);

    // --- acc_mid += product_lo, detect carry ---
    uint8_t xmmOldMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOldMid);
    uint8_t xmmMid = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMid);
    m_Assembler->Movdqa_XmmFromMem(xmmOldMid, m_ACCUM.MidData(), "ACCUM_Mid");
    m_Assembler->Movdqa_XmmToXmm(xmmMid, xmmOldMid);
    m_Assembler->Paddw_XmmToXmm(xmmMid, xmmLo);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.MidData(), "ACCUM_Mid", xmmMid);

    // Carry detection: old > new (unsigned) via helper
    uint8_t xmmConst = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmConst);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmConst, xmmConst);
    m_Assembler->Psllw_XmmImm(xmmConst, 15);  // 0x8000
    uint8_t xmmT = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT, xmmMid);
    EmitUnsignedCarryDetect(xmmOldMid, xmmT, xmmConst);  // carry mask in xmmOldMid
    m_RegState.UnprotectXmm(xmmLo);

    // --- acc_hi += product_hi + carry ---
    uint8_t xmmAccHi = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAccHi);
    m_Assembler->Movdqa_XmmFromMem(xmmAccHi, m_ACCUM.HighData(), "ACCUM_Hi");
    m_Assembler->Paddw_XmmToXmm(xmmAccHi, xmmHi);       // + product_hi
    m_Assembler->Psubw_XmmToXmm(xmmAccHi, xmmOldMid);   // + carry (sub -1 = add 1)
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.HighData(), "ACCUM_Hi", xmmAccHi);
    m_RegState.UnprotectXmm(xmmOldMid);
    m_RegState.UnprotectXmm(xmmHi);

    // --- Signed saturation → vd ---
    EmitSignedSaturation(xmmMid, xmmAccHi);

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMid);

    m_RegState.UnprotectXmm(xmmMid);
    m_RegState.UnprotectXmm(xmmConst);
    m_RegState.UnprotectXmm(xmmAccHi);
}

// ── Add / Sub (native SSE2 — simplest vector ops) ──

void CRSPRecompilerOps::Vector_VADD()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VADD vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // Allocate temp XMMs for vs and vt (with element broadcast)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Load VCOL carry (0xFFFF per lane where set, need 0 or 1)
    uint8_t xmmCarry = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCarry);
    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    m_Assembler->Movdqa_XmmFromMem(xmmCarry, &m_VCOL, "VCO_lo");
    m_Assembler->Psubw_XmmToXmm(xmmZero, xmmCarry);  // 0 - 0xFFFF = 1, 0 - 0 = 0

    // Unclamped sum for accumulator low: accum_lo = vs + vt + carry (wrapping)
    uint8_t xmmAccum = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmAccum, xmmVs);
    m_Assembler->Paddw_XmmToXmm(xmmAccum, xmmVt);
    m_Assembler->Paddw_XmmToXmm(xmmAccum, xmmZero);   // add carry (0 or 1)
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmAccum);

    // Clamped (saturating) add for vd: paddsw(paddsw(vs,vt), carry)
    m_Assembler->Paddsw_XmmToXmm(xmmVs, xmmVt);
    m_Assembler->Paddsw_XmmToXmm(xmmVs, xmmZero);     // add carry with saturation
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    // Clear VCO flags (VADD always clears VCO)
    m_Assembler->Pxor_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVt);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVt);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

void CRSPRecompilerOps::Vector_VSUB()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VSUB vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Load VCOL carry (0xFFFF per lane where set, need 0 or 1)
    uint8_t xmmCarry = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCarry);
    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    m_Assembler->Movdqa_XmmFromMem(xmmCarry, &m_VCOL, "VCO_lo");
    m_Assembler->Psubw_XmmToXmm(xmmZero, xmmCarry);  // 0 - 0xFFFF = 1, 0 - 0 = 0

    // Unclamped difference for accumulator low: accum_lo = vs - vt - carry (wrapping)
    uint8_t xmmAccum = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmAccum, xmmVs);
    m_Assembler->Psubw_XmmToXmm(xmmAccum, xmmVt);
    m_Assembler->Psubw_XmmToXmm(xmmAccum, xmmZero);   // subtract carry (0 or 1)
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmAccum);

    // Clamped (saturating) subtract for vd: psubsw(psubsw(vs,vt), carry)
    m_Assembler->Psubsw_XmmToXmm(xmmVs, xmmVt);
    m_Assembler->Psubsw_XmmToXmm(xmmVs, xmmZero);     // subtract carry with saturation
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    // Clear VCO flags
    m_Assembler->Pxor_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVt);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVt);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

void CRSPRecompilerOps::Vector_VABS()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VABS vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VABS: vd[i] = (vs[i]<0) ? saturate(-vt[i]) : (vs[i]>0) ? vt[i] : 0
    // acc_lo[i] differs from vd when vt==-32768: acc_lo=0x8000, vd=0x7FFF
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmSign = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSign);
    uint8_t xmmSatNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSatNeg);
    uint8_t xmmWrapNeg = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmWrapNeg);

    // Zero register for comparisons
    m_Assembler->Pxor_XmmToXmm(xmmSatNeg, xmmSatNeg);
    m_Assembler->Pxor_XmmToXmm(xmmWrapNeg, xmmWrapNeg);

    // Compute vs==0 mask first (need original vs)
    uint8_t xmmEqZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmEqZ, xmmVs);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmEqZ, xmmSatNeg);  // 0xFFFF where vs==0

    // Saturating negate: satNeg = saturate(0 - vt), -32768 -> 0x7FFF
    m_Assembler->Psubsw_XmmToXmm(xmmSatNeg, xmmVt);
    // Wrapping negate: wrapNeg = 0 - vt, -32768 -> 0x8000
    m_Assembler->Psubw_XmmToXmm(xmmWrapNeg, xmmVt);

    // Sign mask: 0xFFFF where vs < 0
    m_Assembler->Movdqa_XmmToXmm(xmmSign, xmmVs);
    m_Assembler->Psraw_XmmImm(xmmSign, 15);

    // Build vd (saturated path): (sign & satNeg) | (~sign & vt)
    m_Assembler->Pand_XmmToXmm(xmmSatNeg, xmmSign);      // satNeg where vs<0
    uint8_t xmmVd = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmVd, xmmSign);
    m_Assembler->Pandn_XmmToXmm(xmmVd, xmmVt);           // vt where vs>=0
    m_Assembler->Por_XmmToXmm(xmmVd, xmmSatNeg);          // blended
    m_Assembler->Pandn_XmmToXmm(xmmEqZ, xmmVd);           // 0 where vs==0

    // Build acc_lo (wrapping path): (sign & wrapNeg) | (~sign & vt)
    m_Assembler->Pand_XmmToXmm(xmmWrapNeg, xmmSign);      // wrapNeg where vs<0
    m_Assembler->Pandn_XmmToXmm(xmmSign, xmmVt);          // vt where vs>=0
    m_Assembler->Por_XmmToXmm(xmmWrapNeg, xmmSign);        // blended

    // For acc_lo, also zero where vs==0. Recompute eqz mask since pandn destroyed it.
    uint8_t xmmEqZ2 = m_RegState.MapXmmTemp(false, 0, 0);
    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    m_Assembler->Movdqa_XmmToXmm(xmmEqZ2, xmmVs);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmEqZ2, xmmZero);     // 0xFFFF where vs==0
    m_Assembler->Pandn_XmmToXmm(xmmEqZ2, xmmWrapNeg);     // 0 where vs==0

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmEqZ2);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmEqZ);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmSign);
    m_RegState.UnprotectXmm(xmmSatNeg);
    m_RegState.UnprotectXmm(xmmWrapNeg);
}

void CRSPRecompilerOps::Vector_VADDC()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VADDC vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VADDC: vd = vs + vt (wrapping), VCO_lo = carry, VCO_hi = 0
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Save copy of vs for carry detection
    uint8_t xmmCarry = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmCarry, xmmVs);

    // Result = vs + vt (wrapping)
    m_Assembler->Paddw_XmmToXmm(xmmVs, xmmVt);

    // Carry detection: unsigned overflow when result < vs
    // Use sign-flip trick: XOR both with 0x8000, then pcmpgtw
    // Generate 0x8000: pcmpeqw(self) then psllw 15
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Psllw_XmmImm(xmmVt, 15);  // xmmVt = 0x8000
    m_Assembler->Pxor_XmmToXmm(xmmCarry, xmmVt);    // vs ^ 0x8000
    uint8_t xmmResFlip = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmResFlip, xmmVs);
    m_Assembler->Pxor_XmmToXmm(xmmResFlip, xmmVt);  // result ^ 0x8000
    m_Assembler->Pcmpgtw_XmmToXmm(xmmCarry, xmmResFlip); // carry where vs > result (unsigned)

    // Store results
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmCarry);
    // Clear VCO_hi
    m_Assembler->Pxor_XmmToXmm(xmmResFlip, xmmResFlip);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmResFlip);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

void CRSPRecompilerOps::Vector_VSUBC()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VSUBC vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VSUBC: vd = vs - vt (wrapping), VCO_lo = borrow, VCO_hi = (result!=0)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Borrow detection: borrow when vs < vt (unsigned)
    // Generate 0x8000 for unsigned comparison
    uint8_t xmmFlip = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmFlip);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmFlip, xmmFlip);
    m_Assembler->Psllw_XmmImm(xmmFlip, 15);  // 0x8000

    uint8_t xmmBorrow = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmBorrow, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmBorrow, xmmFlip);  // vt ^ 0x8000
    m_Assembler->Pxor_XmmToXmm(xmmFlip, xmmVs);      // vs ^ 0x8000 (reuse xmmFlip)
    m_Assembler->Pcmpgtw_XmmToXmm(xmmBorrow, xmmFlip); // vt > vs unsigned → borrow

    // Result = vs - vt (wrapping)
    m_Assembler->Psubw_XmmToXmm(xmmVs, xmmVt);

    // Nonzero flag: result != 0
    m_Assembler->Pxor_XmmToXmm(xmmVt, xmmVt);  // zero
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVs);  // 0xFFFF where result==0
    // Invert: xor with all-ones
    m_Assembler->Pcmpeqw_XmmToXmm(xmmFlip, xmmFlip);  // all ones
    m_Assembler->Pxor_XmmToXmm(xmmVt, xmmFlip);       // nonzero mask

    // Store
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmBorrow);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVt);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmFlip);
}

// ── Logical (native SSE2 — pand/por/pxor + invert) ──

void CRSPRecompilerOps::Vector_VAND()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VAND vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);

    m_Assembler->Pand_XmmToXmm(xmmVs, xmmVt);
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
}

void CRSPRecompilerOps::Vector_VNAND()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VNAND vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    m_Assembler->Pand_XmmToXmm(xmmVs, xmmVt);
    // Invert: xmmVt = all_ones, then xmmVs ^= all_ones
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVt);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

void CRSPRecompilerOps::Vector_VOR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VOR vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);

    m_Assembler->Por_XmmToXmm(xmmVs, xmmVt);
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
}

void CRSPRecompilerOps::Vector_VNOR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VNOR vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    m_Assembler->Por_XmmToXmm(xmmVs, xmmVt);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVt);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

void CRSPRecompilerOps::Vector_VXOR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VXOR vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // Special case: VXOR vd, vr, vr → zero
    if (m_OpCode.vs == m_OpCode.vt && m_OpCode.e <= 1)
    {
        uint8_t xmmZero = m_RegState.MapXmmZero();
        if (WriteToAccum(AccumLocation::Low, m_CompilePC))
            m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmZero);
        if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
            m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmZero);
        return;
    }

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);

    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVt);
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
}

void CRSPRecompilerOps::Vector_VNXOR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VNXOR vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // Special case: VNXOR vr, vr → all ones
    if (m_OpCode.vs == m_OpCode.vt && m_OpCode.e <= 1)
    {
        uint8_t xmmOnes = m_RegState.MapXmmTemp(false, 0, 0);
        m_Assembler->Pcmpeqw_XmmToXmm(xmmOnes, xmmOnes);
        if (WriteToAccum(AccumLocation::Low, m_CompilePC))
            m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmOnes);
        if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
            m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmOnes);
        return;
    }

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVt);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVt);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
}

// ── Comparison / select (SSE2 pcmpgtw/pcmpeqw + blend) ──
// These ops select between vs/vt values based on comparison, update VCC flags,
// write accumulator low, and clear VCO flags.

void CRSPRecompilerOps::Vector_VLT()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VLT vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VLT: if vs < vt (or vs==vt && VCOL&&VCOH): vd = vs, VCCL = true
    //      else: vd = vt, VCCL = false
    //      accum_lo = vd; clear VCOL, VCOH, VCCH

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmMask = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMask);
    uint8_t xmmEq = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmEq);
    uint8_t xmmTie = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmTie);

    // mask = (vt > vs) → 0xFFFF where vs < vt (strict)
    m_Assembler->Movdqa_XmmToXmm(xmmMask, xmmVt);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmMask, xmmVs);

    // eq = (vs == vt)
    m_Assembler->Movdqa_XmmToXmm(xmmEq, xmmVs);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmEq, xmmVt);

    // tie = VCOL & VCOH (both flags set → tie-break selects vs)
    m_Assembler->Movdqa_XmmFromMem(xmmTie, &m_VCOL, "VCO_lo");
    uint8_t xmmTmp = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmTmp);
    m_Assembler->Movdqa_XmmFromMem(xmmTmp, &m_VCOH, "VCO_hi");
    m_Assembler->Pand_XmmToXmm(xmmTie, xmmTmp);

    // tie = eq & tie (only applies where equal)
    m_Assembler->Pand_XmmToXmm(xmmTie, xmmEq);

    // finalMask = mask | tie → select vs where true, vt where false
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTie);

    // Store VCCL = finalMask
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmMask);

    // result = (mask & vs) | (~mask & vt)
    m_Assembler->Movdqa_XmmToXmm(xmmTie, xmmVs);  // reuse xmmTie for blend
    m_Assembler->Pand_XmmToXmm(xmmTie, xmmMask);   // mask & vs
    m_Assembler->Pandn_XmmToXmm(xmmMask, xmmVt);   // ~mask & vt
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTie);     // result

    // Write accum_lo = result
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmMask);

    // Write vd = result
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMask);

    // Clear VCOL, VCOH, VCCH
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmMask);
    m_RegState.UnprotectXmm(xmmEq);
    m_RegState.UnprotectXmm(xmmTie);
    m_RegState.UnprotectXmm(xmmTmp);
}

void CRSPRecompilerOps::Vector_VEQ()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VEQ vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VEQ: if vs == vt and !VCOH: VCCL = true, vd = vs
    //      else: VCCL = false, vd = vt
    //      accum_lo = vd; clear VCOL, VCOH, VCCH

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmMask = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMask);
    uint8_t xmmTmp = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmTmp);

    // eq = (vs == vt)
    m_Assembler->Movdqa_XmmToXmm(xmmMask, xmmVs);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMask, xmmVt);

    // Exclude elements where VCOH is set: mask &= ~VCOH
    m_Assembler->Movdqa_XmmFromMem(xmmTmp, &m_VCOH, "VCO_hi");
    m_Assembler->Pandn_XmmToXmm(xmmTmp, xmmMask);  // ~VCOH & eq
    m_Assembler->Movdqa_XmmToXmm(xmmMask, xmmTmp);

    // Store VCCL = mask
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmMask);

    // result = (mask & vs) | (~mask & vt)
    m_Assembler->Movdqa_XmmToXmm(xmmTmp, xmmVs);
    m_Assembler->Pand_XmmToXmm(xmmTmp, xmmMask);
    m_Assembler->Pandn_XmmToXmm(xmmMask, xmmVt);
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTmp);

    // Write accum_lo = result
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmMask);

    // Write vd = result
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMask);

    // Clear VCOL, VCOH, VCCH
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmMask);
    m_RegState.UnprotectXmm(xmmTmp);
}

void CRSPRecompilerOps::Vector_VNE()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VNE vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VNE: if vs != vt or VCOH: VCCL = true, vd = vs
    //      else: VCCL = false, vd = vt
    //      accum_lo = vd; clear VCOL, VCOH, VCCH

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmMask = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMask);
    uint8_t xmmTmp = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmTmp);
    uint8_t xmmOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOnes);

    // ne = (vs != vt) = ~(vs == vt)
    m_Assembler->Movdqa_XmmToXmm(xmmMask, xmmVs);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmMask, xmmVt);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmOnes, xmmOnes);  // All ones
    m_Assembler->Pxor_XmmToXmm(xmmMask, xmmOnes);      // Invert → not equal

    // Include elements where VCOH is set: mask |= VCOH
    m_Assembler->Movdqa_XmmFromMem(xmmTmp, &m_VCOH, "VCO_hi");
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTmp);

    // Store VCCL = mask
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmMask);

    // result = (mask & vs) | (~mask & vt)
    m_Assembler->Movdqa_XmmToXmm(xmmTmp, xmmVs);
    m_Assembler->Pand_XmmToXmm(xmmTmp, xmmMask);
    m_Assembler->Pandn_XmmToXmm(xmmMask, xmmVt);
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTmp);

    // Write accum_lo = result
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmMask);

    // Write vd = result
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMask);

    // Clear VCOL, VCOH, VCCH
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmMask);
    m_RegState.UnprotectXmm(xmmTmp);
    m_RegState.UnprotectXmm(xmmOnes);
}

void CRSPRecompilerOps::Vector_VGE()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VGE vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VGE: if vs > vt (or vs==vt && (!VCOL || !VCOH)): vd = vs, VCCL = true
    //      else: vd = vt, VCCL = false
    //      accum_lo = vd; clear VCOL, VCOH, VCCH

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmMask = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMask);
    uint8_t xmmEq = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmEq);
    uint8_t xmmTie = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmTie);
    uint8_t xmmTmp = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmTmp);

    // mask = (vs > vt) → pcmpgtw(vs, vt)
    m_Assembler->Movdqa_XmmToXmm(xmmMask, xmmVs);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmMask, xmmVt);

    // eq = (vs == vt)
    m_Assembler->Movdqa_XmmToXmm(xmmEq, xmmVs);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmEq, xmmVt);

    // tie = ~(VCOL & VCOH) = ~VCOL | ~VCOH; but use: ~(VCOL & VCOH)
    m_Assembler->Movdqa_XmmFromMem(xmmTie, &m_VCOL, "VCO_lo");
    m_Assembler->Movdqa_XmmFromMem(xmmTmp, &m_VCOH, "VCO_hi");
    m_Assembler->Pand_XmmToXmm(xmmTie, xmmTmp);       // VCOL & VCOH
    m_Assembler->Pcmpeqw_XmmToXmm(xmmTmp, xmmTmp);    // all ones
    m_Assembler->Pxor_XmmToXmm(xmmTie, xmmTmp);        // ~(VCOL & VCOH)

    // tie = eq & tie (only applies where equal)
    m_Assembler->Pand_XmmToXmm(xmmTie, xmmEq);

    // finalMask = mask | tie
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTie);

    // Store VCCL = finalMask
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmMask);

    // result = (mask & vs) | (~mask & vt)
    m_Assembler->Movdqa_XmmToXmm(xmmTie, xmmVs);
    m_Assembler->Pand_XmmToXmm(xmmTie, xmmMask);
    m_Assembler->Pandn_XmmToXmm(xmmMask, xmmVt);
    m_Assembler->Por_XmmToXmm(xmmMask, xmmTie);

    // Write accum_lo = result
    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmMask);

    // Write vd = result
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmMask);

    // Clear VCOL, VCOH, VCCH
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmMask);
    m_RegState.UnprotectXmm(xmmEq);
    m_RegState.UnprotectXmm(xmmTie);
    m_RegState.UnprotectXmm(xmmTmp);
}

void CRSPRecompilerOps::Vector_VCL()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VCL vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);
    m_RegState.WriteBackRegisters();

    // VCL: compare-and-clip low — uses flags set by VCH
    // 4 paths based on VCOL × VCOH:
    //   VCOL=1, VCOH=1: use existing VCCL, select -vt or vs
    //   VCOL=1, VCOH=0: compute condition (unsigned add check), set VCCL, select -vt or vs
    //   VCOL=0, VCOH=1: use existing VCCH, select vt or vs
    //   VCOL=0, VCOH=0: compute (vs_u >= vt_u), set VCCH, select vt or vs
    // After: accum_lo = vd = result; clear VCOL, VCOH, VCE

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmZero);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    uint8_t xmmOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOnes);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmOnes, xmmOnes);

    // Load existing flags
    uint8_t xmmCOL = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCOL);
    m_Assembler->Movdqa_XmmFromMem(xmmCOL, &m_VCOL, "VCO_lo");
    uint8_t xmmCOH = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCOH);
    m_Assembler->Movdqa_XmmFromMem(xmmCOH, &m_VCOH, "VCO_hi");
    uint8_t xmmVCE = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVCE);
    m_Assembler->Movdqa_XmmFromMem(xmmVCE, &m_VCE, "VCE");
    uint8_t xmmCCL = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCCL);
    m_Assembler->Movdqa_XmmFromMem(xmmCCL, &m_VCCL, "VCC_lo");
    uint8_t xmmCCH = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCCH);
    m_Assembler->Movdqa_XmmFromMem(xmmCCH, &m_VCCH, "VCC_hi");

    // neg_vt = 0 - vt (for VCOL=1 paths)
    uint8_t xmmNV = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNV);
    m_Assembler->Movdqa_XmmToXmm(xmmNV, xmmZero);
    m_Assembler->Psubw_XmmToXmm(xmmNV, xmmVt);

    // --- Path B: VCOL=1, VCOH=0 → update VCCL ---
    // path_b_mask = VCOL & ~VCOH
    uint8_t xmmPB = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmPB);
    m_Assembler->Movdqa_XmmToXmm(xmmPB, xmmCOH);
    m_Assembler->Pxor_XmmToXmm(xmmPB, xmmOnes);  // ~VCOH
    m_Assembler->Pand_XmmToXmm(xmmPB, xmmCOL);   // VCOL & ~VCOH

    // Unsigned add wrapping: sum16 = vs + vt
    uint8_t xmmAddW = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmAddW);
    m_Assembler->Movdqa_XmmToXmm(xmmAddW, xmmVs);
    m_Assembler->Paddw_XmmToXmm(xmmAddW, xmmVt);

    // sum_is_zero = (sum16 == 0)
    uint8_t xmmSZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSZ);
    m_Assembler->Movdqa_XmmToXmm(xmmSZ, xmmAddW);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmSZ, xmmZero);

    // Detect unsigned carry: carry = (sum16 < vs_unsigned)
    // Unsigned compare via XOR 0x8000 trick
    uint8_t xmmC80 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmC80);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmC80, xmmC80);
    m_Assembler->Psllw_XmmImm(xmmC80, 15);  // 0x8000

    uint8_t xmmCarry = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCarry);
    m_Assembler->Movdqa_XmmToXmm(xmmCarry, xmmVs);
    m_Assembler->Pxor_XmmToXmm(xmmCarry, xmmC80);
    uint8_t xmmT1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT1, xmmAddW);
    m_Assembler->Pxor_XmmToXmm(xmmT1, xmmC80);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmCarry, xmmT1);  // carry where vs > sum (unsigned overflow)
    m_RegState.UnprotectXmm(xmmC80);
    m_RegState.UnprotectXmm(xmmAddW);

    // With VCE: cond = ~carry | sum_is_zero (no overflow, or wrapped to zero)
    // Without VCE: cond = sum_is_zero
    // path_b_cond = VCE ? (~carry | sum_is_zero) : sum_is_zero
    uint8_t xmmNCarry = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNCarry);
    m_Assembler->Movdqa_XmmToXmm(xmmNCarry, xmmCarry);
    m_Assembler->Pxor_XmmToXmm(xmmNCarry, xmmOnes);       // ~carry
    m_Assembler->Por_XmmToXmm(xmmNCarry, xmmSZ);           // ~carry | sum_is_zero

    // cond = (VCE & ncarry_or_sz) | (~VCE & sum_is_zero)
    uint8_t xmmCond = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmCond);
    m_Assembler->Movdqa_XmmToXmm(xmmCond, xmmVCE);
    m_Assembler->Pand_XmmToXmm(xmmCond, xmmNCarry);
    uint8_t xmmT2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT2, xmmVCE);
    m_Assembler->Pandn_XmmToXmm(xmmT2, xmmSZ);             // ~VCE & sum_is_zero
    m_Assembler->Por_XmmToXmm(xmmCond, xmmT2);
    m_RegState.UnprotectXmm(xmmSZ);
    m_RegState.UnprotectXmm(xmmCarry);
    m_RegState.UnprotectXmm(xmmNCarry);

    // Update VCCL: new_ccl = (path_b & cond) | (~path_b & old_ccl)
    uint8_t xmmNewCCL = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNewCCL);
    m_Assembler->Movdqa_XmmToXmm(xmmNewCCL, xmmPB);
    m_Assembler->Pand_XmmToXmm(xmmNewCCL, xmmCond);
    uint8_t xmmT3 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT3, xmmPB);
    m_Assembler->Pandn_XmmToXmm(xmmT3, xmmCCL);
    m_Assembler->Por_XmmToXmm(xmmNewCCL, xmmT3);
    m_Assembler->Movdqa_XmmToXmm(xmmCCL, xmmNewCCL);       // CCL now updated
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmCCL);
    m_RegState.UnprotectXmm(xmmNewCCL);
    m_RegState.UnprotectXmm(xmmCond);
    m_RegState.UnprotectXmm(xmmPB);

    // --- Path D: VCOL=0, VCOH=0 → update VCCH ---
    uint8_t xmmPD = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmPD);
    m_Assembler->Movdqa_XmmToXmm(xmmPD, xmmCOL);
    m_Assembler->Por_XmmToXmm(xmmPD, xmmCOH);
    m_Assembler->Pxor_XmmToXmm(xmmPD, xmmOnes);  // ~(VCOL | VCOH) = !VCOL & !VCOH

    // Unsigned compare: vs_u >= vt_u → NOT(vt_u > vs_u)
    uint8_t xmmC8 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmC8);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmC8, xmmC8);
    m_Assembler->Psllw_XmmImm(xmmC8, 15);  // 0x8000

    uint8_t xmmUGE = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmUGE);
    m_Assembler->Movdqa_XmmToXmm(xmmUGE, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmUGE, xmmC8);
    uint8_t xmmT4 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT4, xmmVs);
    m_Assembler->Pxor_XmmToXmm(xmmT4, xmmC8);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmUGE, xmmT4);  // vt > vs unsigned
    m_Assembler->Pxor_XmmToXmm(xmmUGE, xmmOnes);   // invert → vs >= vt unsigned
    m_RegState.UnprotectXmm(xmmC8);

    // Update VCCH: new_cch = (path_d & unsigned_ge) | (~path_d & old_cch)
    uint8_t xmmNewCCH = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNewCCH);
    m_Assembler->Movdqa_XmmToXmm(xmmNewCCH, xmmPD);
    m_Assembler->Pand_XmmToXmm(xmmNewCCH, xmmUGE);
    uint8_t xmmT5 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT5, xmmPD);
    m_Assembler->Pandn_XmmToXmm(xmmT5, xmmCCH);
    m_Assembler->Por_XmmToXmm(xmmNewCCH, xmmT5);
    m_Assembler->Movdqa_XmmToXmm(xmmCCH, xmmNewCCH);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmCCH);
    m_RegState.UnprotectXmm(xmmNewCCH);
    m_RegState.UnprotectXmm(xmmUGE);
    m_RegState.UnprotectXmm(xmmPD);
    m_RegState.UnprotectXmm(xmmVCE);
    m_RegState.UnprotectXmm(xmmCOH);

    // --- Compute result ---
    // VCOL=1: result = CCL ? -vt : vs
    // VCOL=0: result = CCH ? vt : vs

    // result_col1 = (CCL & neg_vt) | (~CCL & vs)
    uint8_t xmmR1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmR1);
    m_Assembler->Movdqa_XmmToXmm(xmmR1, xmmCCL);
    m_Assembler->Pand_XmmToXmm(xmmR1, xmmNV);
    uint8_t xmmT6 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT6, xmmCCL);
    m_Assembler->Pandn_XmmToXmm(xmmT6, xmmVs);
    m_Assembler->Por_XmmToXmm(xmmR1, xmmT6);

    // result_col0 = (CCH & vt) | (~CCH & vs)
    uint8_t xmmR0 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmR0);
    m_Assembler->Movdqa_XmmToXmm(xmmR0, xmmCCH);
    m_Assembler->Pand_XmmToXmm(xmmR0, xmmVt);
    uint8_t xmmT7 = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqa_XmmToXmm(xmmT7, xmmCCH);
    m_Assembler->Pandn_XmmToXmm(xmmT7, xmmVs);
    m_Assembler->Por_XmmToXmm(xmmR0, xmmT7);

    // final = VCOL ? result_col1 : result_col0
    m_Assembler->Pand_XmmToXmm(xmmR1, xmmCOL);
    m_Assembler->Pandn_XmmToXmm(xmmCOL, xmmR0);  // destroys COL
    m_Assembler->Por_XmmToXmm(xmmR1, xmmCOL);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmR1);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmR1);

    // Clear VCOL, VCOH, VCE
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmZero);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmZero);
    m_Assembler->Movdqa_MemFromXmm(&m_VCE, "VCE", xmmZero);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmZero);
    m_RegState.UnprotectXmm(xmmOnes);
    m_RegState.UnprotectXmm(xmmCOL);
    m_RegState.UnprotectXmm(xmmCCL);
    m_RegState.UnprotectXmm(xmmCCH);
    m_RegState.UnprotectXmm(xmmNV);
    m_RegState.UnprotectXmm(xmmR1);
    m_RegState.UnprotectXmm(xmmR0);
}

void CRSPRecompilerOps::Vector_VCH()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VCH vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);
    m_RegState.WriteBackRegisters();

    // VCH: compare-and-clip high — sets all 5 flag registers
    // Per element: if signs differ (vs^vt < 0):
    //   sum = vs + vt
    //   VCOL=1, VCCL=(sum<=0), VCCH=(vt<0), VCE=(sum==-1)
    //   VCOH=(sum!=0 && vs!=~vt), result = (sum<=0) ? -vt : vs
    // Else (same signs):
    //   diff = vs - vt
    //   VCOL=0, VCCL=(vt<0), VCCH=(diff>=0), VCE=0
    //   VCOH=(diff!=0 && vs!=~vt), result = (diff>=0) ? vt : vs

    // Load operands
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    // Constants: zero and allones
    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmZero);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    uint8_t xmmOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOnes);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmOnes, xmmOnes);  // 0xFFFF

    // sign_diff = (vs ^ vt) >> 15 → 0xFFFF where signs differ
    uint8_t xmmSD = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSD);
    m_Assembler->Movdqa_XmmToXmm(xmmSD, xmmVs);
    m_Assembler->Pxor_XmmToXmm(xmmSD, xmmVt);
    m_Assembler->Psraw_XmmImm(xmmSD, 15);
    // VCOL = sign_diff
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmSD);

    // sum = vs + vt; diff = vs - vt
    uint8_t xmmSum = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSum);
    m_Assembler->Movdqa_XmmToXmm(xmmSum, xmmVs);
    m_Assembler->Paddw_XmmToXmm(xmmSum, xmmVt);
    uint8_t xmmDiff = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDiff);
    m_Assembler->Movdqa_XmmToXmm(xmmDiff, xmmVs);
    m_Assembler->Psubw_XmmToXmm(xmmDiff, xmmVt);

    // VCE = sign_diff & (sum == -1)
    uint8_t xmmT1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmT1);
    m_Assembler->Movdqa_XmmToXmm(xmmT1, xmmSum);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmT1, xmmOnes);  // sum == 0xFFFF == -1
    m_Assembler->Pand_XmmToXmm(xmmT1, xmmSD);
    m_Assembler->Movdqa_MemFromXmm(&m_VCE, "VCE", xmmT1);
    m_RegState.UnprotectXmm(xmmT1);

    // sum_le_zero = ~pcmpgtw(sum, 0)
    uint8_t xmmSLZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSLZ);
    m_Assembler->Movdqa_XmmToXmm(xmmSLZ, xmmSum);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmSLZ, xmmZero);
    m_Assembler->Pxor_XmmToXmm(xmmSLZ, xmmOnes);  // invert → sum <= 0

    // vt_neg = vt >> 15 (all 1s where vt < 0)
    uint8_t xmmVN = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVN);
    m_Assembler->Movdqa_XmmToXmm(xmmVN, xmmVt);
    m_Assembler->Psraw_XmmImm(xmmVN, 15);

    // VCCL = sign_diff ? sum_le_zero : vt_neg
    uint8_t xmmVCCL = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVCCL);
    m_Assembler->Movdqa_XmmToXmm(xmmVCCL, xmmSD);
    m_Assembler->Pand_XmmToXmm(xmmVCCL, xmmSLZ);          // sign_diff & sum_le_zero
    uint8_t xmmT2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmT2);
    m_Assembler->Movdqa_XmmToXmm(xmmT2, xmmSD);
    m_Assembler->Pandn_XmmToXmm(xmmT2, xmmVN);            // ~sign_diff & vt_neg
    m_Assembler->Por_XmmToXmm(xmmVCCL, xmmT2);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmVCCL);
    m_RegState.UnprotectXmm(xmmVCCL);
    m_RegState.UnprotectXmm(xmmT2);

    // diff_ge_zero = ~pcmpgtw(0, diff) = ~(0 > diff) = diff >= 0
    uint8_t xmmDGZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDGZ);
    m_Assembler->Movdqa_XmmToXmm(xmmDGZ, xmmZero);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmDGZ, xmmDiff);
    m_Assembler->Pxor_XmmToXmm(xmmDGZ, xmmOnes);  // invert → diff >= 0

    // VCCH = sign_diff ? vt_neg : diff_ge_zero
    uint8_t xmmVCCH = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVCCH);
    m_Assembler->Movdqa_XmmToXmm(xmmVCCH, xmmSD);
    m_Assembler->Pand_XmmToXmm(xmmVCCH, xmmVN);           // sign_diff & vt_neg
    uint8_t xmmT3 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmT3);
    m_Assembler->Movdqa_XmmToXmm(xmmT3, xmmSD);
    m_Assembler->Pandn_XmmToXmm(xmmT3, xmmDGZ);           // ~sign_diff & diff_ge_zero
    m_Assembler->Por_XmmToXmm(xmmVCCH, xmmT3);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmVCCH);
    m_RegState.UnprotectXmm(xmmVCCH);
    m_RegState.UnprotectXmm(xmmT3);
    m_RegState.UnprotectXmm(xmmVN);

    // VCOH: value = sign_diff ? sum : diff; VCOH = (value != 0) & (vs != ~vt)
    uint8_t xmmVal = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVal);
    m_Assembler->Movdqa_XmmToXmm(xmmVal, xmmSD);
    m_Assembler->Pand_XmmToXmm(xmmVal, xmmSum);            // sign_diff & sum
    uint8_t xmmT4 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmT4);
    m_Assembler->Movdqa_XmmToXmm(xmmT4, xmmSD);
    m_Assembler->Pandn_XmmToXmm(xmmT4, xmmDiff);           // ~sign_diff & diff
    m_Assembler->Por_XmmToXmm(xmmVal, xmmT4);
    m_RegState.UnprotectXmm(xmmT4);
    // Already done with sum and diff for flags
    m_RegState.UnprotectXmm(xmmSum);
    m_RegState.UnprotectXmm(xmmDiff);

    // value_nonzero = ~(value == 0)
    uint8_t xmmVNZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVNZ);
    m_Assembler->Movdqa_XmmToXmm(xmmVNZ, xmmVal);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmVNZ, xmmZero);
    m_Assembler->Pxor_XmmToXmm(xmmVNZ, xmmOnes);           // invert
    m_RegState.UnprotectXmm(xmmVal);

    // vs_ne_inv_vt = ~(vs == ~vt)
    uint8_t xmmNIV = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNIV);
    m_Assembler->Movdqa_XmmToXmm(xmmNIV, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmNIV, xmmOnes);           // ~vt
    m_Assembler->Pcmpeqw_XmmToXmm(xmmNIV, xmmVs);          // vs == ~vt
    m_Assembler->Pxor_XmmToXmm(xmmNIV, xmmOnes);           // invert → vs != ~vt

    m_Assembler->Pand_XmmToXmm(xmmVNZ, xmmNIV);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVNZ);
    m_RegState.UnprotectXmm(xmmVNZ);
    m_RegState.UnprotectXmm(xmmNIV);

    // --- Result ---
    // Reload sum/diff since we freed them (register pressure management)
    uint8_t xmmSum2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSum2);
    m_Assembler->Movdqa_XmmToXmm(xmmSum2, xmmVs);
    m_Assembler->Paddw_XmmToXmm(xmmSum2, xmmVt);
    uint8_t xmmDiff2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDiff2);
    m_Assembler->Movdqa_XmmToXmm(xmmDiff2, xmmVs);
    m_Assembler->Psubw_XmmToXmm(xmmDiff2, xmmVt);

    // Recompute sum_le_zero and diff_ge_zero for result blending
    uint8_t xmmSLZ2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSLZ2);
    m_Assembler->Movdqa_XmmToXmm(xmmSLZ2, xmmSum2);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmSLZ2, xmmZero);
    m_Assembler->Pxor_XmmToXmm(xmmSLZ2, xmmOnes);  // sum <= 0

    uint8_t xmmDGZ2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDGZ2);
    m_Assembler->Movdqa_XmmToXmm(xmmDGZ2, xmmZero);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmDGZ2, xmmDiff2);
    m_Assembler->Pxor_XmmToXmm(xmmDGZ2, xmmOnes);  // diff >= 0
    m_RegState.UnprotectXmm(xmmSum2);
    m_RegState.UnprotectXmm(xmmDiff2);

    // neg_vt = 0 - vt
    uint8_t xmmNV = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNV);
    m_Assembler->Movdqa_XmmToXmm(xmmNV, xmmZero);
    m_Assembler->Psubw_XmmToXmm(xmmNV, xmmVt);

    // result_diff = sum_le_zero ? neg_vt : vs
    uint8_t xmmRD = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmRD);
    m_Assembler->Movdqa_XmmToXmm(xmmRD, xmmSLZ2);
    m_Assembler->Pand_XmmToXmm(xmmRD, xmmNV);              // slz & neg_vt
    m_Assembler->Pandn_XmmToXmm(xmmSLZ2, xmmVs);           // ~slz & vs (destroys SLZ2)
    m_Assembler->Por_XmmToXmm(xmmRD, xmmSLZ2);
    m_RegState.UnprotectXmm(xmmSLZ2);
    m_RegState.UnprotectXmm(xmmNV);

    // result_same = diff_ge_zero ? vt : vs
    uint8_t xmmRS = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmRS);
    m_Assembler->Movdqa_XmmToXmm(xmmRS, xmmDGZ2);
    m_Assembler->Pand_XmmToXmm(xmmRS, xmmVt);              // dgz & vt
    m_Assembler->Pandn_XmmToXmm(xmmDGZ2, xmmVs);           // ~dgz & vs (destroys DGZ2)
    m_Assembler->Por_XmmToXmm(xmmRS, xmmDGZ2);
    m_RegState.UnprotectXmm(xmmDGZ2);

    // final = sign_diff ? result_diff : result_same
    m_Assembler->Pand_XmmToXmm(xmmRD, xmmSD);              // sd & result_diff
    m_Assembler->Pandn_XmmToXmm(xmmSD, xmmRS);             // ~sd & result_same (destroys SD)
    m_Assembler->Por_XmmToXmm(xmmRD, xmmSD);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmRD);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmRD);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmZero);
    m_RegState.UnprotectXmm(xmmOnes);
    m_RegState.UnprotectXmm(xmmSD);
    m_RegState.UnprotectXmm(xmmSLZ);
    m_RegState.UnprotectXmm(xmmRD);
    m_RegState.UnprotectXmm(xmmRS);
}

void CRSPRecompilerOps::Vector_VCR()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VCR vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);
    m_RegState.WriteBackRegisters();

    // VCR: compare-and-clip reduced (simplified VCH, no VCE)
    // Per element: if signs differ (vs^vt < 0):
    //   VCCH = (vt < 0), VCCL = (vs + vt + 1 <= 0) = (vs + vt < 0)
    //   result = VCCL ? ~vt : vs
    // Else (same signs):
    //   VCCL = (vt < 0), VCCH = (vs - vt >= 0)
    //   result = VCCH ? vt : vs
    // After: clear VCOL, VCOH, VCE

    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);

    uint8_t xmmZero = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmZero);
    m_Assembler->Pxor_XmmToXmm(xmmZero, xmmZero);
    uint8_t xmmOnes = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmOnes);
    m_Assembler->Pcmpeqw_XmmToXmm(xmmOnes, xmmOnes);

    // sign_diff = (vs ^ vt) >> 15
    uint8_t xmmSD = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSD);
    m_Assembler->Movdqa_XmmToXmm(xmmSD, xmmVs);
    m_Assembler->Pxor_XmmToXmm(xmmSD, xmmVt);
    m_Assembler->Psraw_XmmImm(xmmSD, 15);

    // sum = vs + vt; diff = vs - vt
    uint8_t xmmSum = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSum);
    m_Assembler->Movdqa_XmmToXmm(xmmSum, xmmVs);
    m_Assembler->Paddw_XmmToXmm(xmmSum, xmmVt);
    uint8_t xmmDiff = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDiff);
    m_Assembler->Movdqa_XmmToXmm(xmmDiff, xmmVs);
    m_Assembler->Psubw_XmmToXmm(xmmDiff, xmmVt);

    // vt_neg = vt >> 15 (0xFFFF where vt < 0)
    uint8_t xmmVN = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVN);
    m_Assembler->Movdqa_XmmToXmm(xmmVN, xmmVt);
    m_Assembler->Psraw_XmmImm(xmmVN, 15);

    // sum_lt_zero = (sum < 0) = sum >> 15 [for different signs: VCCL = vs+vt < 0]
    uint8_t xmmSLZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmSLZ);
    m_Assembler->Movdqa_XmmToXmm(xmmSLZ, xmmSum);
    m_Assembler->Psraw_XmmImm(xmmSLZ, 15);

    // diff_ge_zero = ~pcmpgtw(0, diff)
    uint8_t xmmDGZ = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmDGZ);
    m_Assembler->Movdqa_XmmToXmm(xmmDGZ, xmmZero);
    m_Assembler->Pcmpgtw_XmmToXmm(xmmDGZ, xmmDiff);
    m_Assembler->Pxor_XmmToXmm(xmmDGZ, xmmOnes);

    // VCCL = sign_diff ? sum_lt_zero : vt_neg
    uint8_t xmmVCCL = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVCCL);
    m_Assembler->Movdqa_XmmToXmm(xmmVCCL, xmmSD);
    m_Assembler->Pand_XmmToXmm(xmmVCCL, xmmSLZ);
    uint8_t xmmT1 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmT1);
    m_Assembler->Movdqa_XmmToXmm(xmmT1, xmmSD);
    m_Assembler->Pandn_XmmToXmm(xmmT1, xmmVN);
    m_Assembler->Por_XmmToXmm(xmmVCCL, xmmT1);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCL, "VCC_lo", xmmVCCL);
    m_RegState.UnprotectXmm(xmmVCCL);
    m_RegState.UnprotectXmm(xmmT1);

    // VCCH = sign_diff ? vt_neg : diff_ge_zero
    uint8_t xmmVCCH = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmVCCH);
    m_Assembler->Movdqa_XmmToXmm(xmmVCCH, xmmSD);
    m_Assembler->Pand_XmmToXmm(xmmVCCH, xmmVN);
    uint8_t xmmT2 = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmT2);
    m_Assembler->Movdqa_XmmToXmm(xmmT2, xmmSD);
    m_Assembler->Pandn_XmmToXmm(xmmT2, xmmDGZ);
    m_Assembler->Por_XmmToXmm(xmmVCCH, xmmT2);
    m_Assembler->Movdqa_MemFromXmm(&m_VCCH, "VCC_hi", xmmVCCH);
    m_RegState.UnprotectXmm(xmmVCCH);
    m_RegState.UnprotectXmm(xmmT2);
    m_RegState.UnprotectXmm(xmmVN);
    m_RegState.UnprotectXmm(xmmSum);
    m_RegState.UnprotectXmm(xmmDiff);

    // --- Result ---
    // not_vt = ~vt (for different signs path: result = ~vt when VCCL set)
    uint8_t xmmNV = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmNV);
    m_Assembler->Movdqa_XmmToXmm(xmmNV, xmmVt);
    m_Assembler->Pxor_XmmToXmm(xmmNV, xmmOnes);  // ~vt

    // result_diff = sum_lt_zero ? ~vt : vs = (SLZ & ~vt) | (~SLZ & vs)
    uint8_t xmmRD = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmRD);
    m_Assembler->Movdqa_XmmToXmm(xmmRD, xmmSLZ);
    m_Assembler->Pand_XmmToXmm(xmmRD, xmmNV);
    m_Assembler->Pandn_XmmToXmm(xmmSLZ, xmmVs);   // destroys SLZ
    m_Assembler->Por_XmmToXmm(xmmRD, xmmSLZ);
    m_RegState.UnprotectXmm(xmmSLZ);
    m_RegState.UnprotectXmm(xmmNV);

    // result_same = diff_ge_zero ? vt : vs = (DGZ & vt) | (~DGZ & vs)
    uint8_t xmmRS = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmRS);
    m_Assembler->Movdqa_XmmToXmm(xmmRS, xmmDGZ);
    m_Assembler->Pand_XmmToXmm(xmmRS, xmmVt);
    m_Assembler->Pandn_XmmToXmm(xmmDGZ, xmmVs);   // destroys DGZ
    m_Assembler->Por_XmmToXmm(xmmRS, xmmDGZ);
    m_RegState.UnprotectXmm(xmmDGZ);

    // final = sign_diff ? result_diff : result_same
    m_Assembler->Pand_XmmToXmm(xmmRD, xmmSD);
    m_Assembler->Pandn_XmmToXmm(xmmSD, xmmRS);    // destroys SD
    m_Assembler->Por_XmmToXmm(xmmRD, xmmSD);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmRD);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmRD);

    // Clear VCOL, VCOH, VCE
    m_Assembler->Pxor_XmmToXmm(xmmVs, xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVs);
    m_Assembler->Movdqa_MemFromXmm(&m_VCE, "VCE", xmmVs);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmZero);
    m_RegState.UnprotectXmm(xmmOnes);
    m_RegState.UnprotectXmm(xmmSD);
    m_RegState.UnprotectXmm(xmmRD);
    m_RegState.UnprotectXmm(xmmRS);
}

void CRSPRecompilerOps::Vector_VMRG()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMRG vd=%u, vs=%u, vt=%u, e=%u\n",
                     m_OpCode.vd, m_OpCode.vs, m_OpCode.vt, m_OpCode.e);

    // VMRG: vd = VCC_lo ? vs : vt (element-wise blend)
    uint8_t xmmVs = m_RegState.MapXmmTemp(true, m_OpCode.vs, 0);
    m_RegState.ProtectXmm(xmmVs);
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    uint8_t xmmMask = m_RegState.MapXmmTemp(false, 0, 0);
    m_RegState.ProtectXmm(xmmMask);

    // Load VCC_lo as selection mask (0xFFFF or 0x0000 per lane)
    m_Assembler->Movdqa_XmmFromMem(xmmMask, &m_VCCL, "VCC_lo");

    // Blend: result = (mask & vs) | (~mask & vt)
    m_Assembler->Pand_XmmToXmm(xmmVs, xmmMask);
    m_Assembler->Pandn_XmmToXmm(xmmMask, xmmVt);
    m_Assembler->Por_XmmToXmm(xmmVs, xmmMask);

    if (WriteToAccum(AccumLocation::Low, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVs);
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVs);

    // Clear VCO flags
    m_Assembler->Pxor_XmmToXmm(xmmVt, xmmVt);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOL, "VCO_lo", xmmVt);
    m_Assembler->Movdqa_MemFromXmm(&m_VCOH, "VCO_hi", xmmVt);

    m_RegState.UnprotectXmm(xmmVs);
    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmMask);
}

// ── Accumulator read ──

void CRSPRecompilerOps::Vector_VSAW()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VSAW vd=%u, e=%u\n", m_OpCode.vd, m_OpCode.e);

    // VSAW: Read accumulator slice into vd based on element field
    uint8_t xmmResult = m_RegState.MapXmmTemp(false, 0, 0);

    switch (m_OpCode.e) {
    case 8:  // ACCUM High (bits 32-47)
        m_Assembler->Movdqa_XmmFromMem(xmmResult, m_ACCUM.HighData(), "ACCUM_Hi");
        break;
    case 9:  // ACCUM Middle (bits 16-31)
        m_Assembler->Movdqa_XmmFromMem(xmmResult, m_ACCUM.MidData(), "ACCUM_Mid");
        break;
    case 10: // ACCUM Low (bits 0-15)
        m_Assembler->Movdqa_XmmFromMem(xmmResult, m_ACCUM.LowData(), "ACCUM_Lo");
        break;
    default: // Undefined: zero
        m_Assembler->Pxor_XmmToXmm(xmmResult, xmmResult);
        break;
    }

    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmResult);
}

// ── Reciprocal / sqrt — interpreter fallback ──
// These use CLZ + 512-entry lookup tables and are not SSE-friendly

VECTOR_FALLBACK(VRCP)   VECTOR_FALLBACK(VRCPL)  VECTOR_FALLBACK(VRCPH)
VECTOR_FALLBACK(VRSQ)   VECTOR_FALLBACK(VRSQL)  VECTOR_FALLBACK(VRSQH)

// ── VMOV — native SSE2 single-element copy ──

void CRSPRecompilerOps::Vector_VMOV()
{
    if (m_Assembler == nullptr) return;
    m_Recompiler.Log("; VMOV vd=%u, vt=%u, e=%u, de=%u\n",
                     m_OpCode.vd, m_OpCode.vt, m_OpCode.e, m_OpCode.de);

    // Load vt with element broadcast → store to accumulator low
    uint8_t xmmVt = m_RegState.MapXmmTemp(true, m_OpCode.vt, m_OpCode.e);
    m_RegState.ProtectXmm(xmmVt);
    m_Assembler->Movdqa_MemFromXmm(m_ACCUM.LowData(), "ACCUM_Lo", xmmVt);

    // Compute element indices at JIT compile time
    // RSP dest element: index = 7 - (de & 7)
    // Source RSP element via EleSpec[e]: replicate the formula from Build_RSP
    uint8_t e = m_OpCode.e;
    uint8_t dstElemRsp = 7 - (m_OpCode.de & 0x7);
    uint8_t srcElemRsp;
    if (e <= 1)       srcElemRsp = dstElemRsp;                           // identity
    else if (e <= 3)  srcElemRsp = (dstElemRsp & 0x6) | (e & 0x1);      // pair broadcast
    else if (e <= 7)  srcElemRsp = (dstElemRsp & 0x4) | (e & 0x3);      // quad broadcast
    else              srcElemRsp = e & 0x7;                               // scalar broadcast

    // Convert RSP element indices to XMM word positions (reversed)
    uint8_t srcXmmWord = 7 - srcElemRsp;
    uint8_t dstXmmWord = m_OpCode.de & 0x7;  // = 7 - dstElemRsp

    // Extract source element from vt (loaded plain, not broadcast)
    // For broadcast cases, MapXmmTemp loaded plain vt, so use computed srcXmmWord
    // pextrw eax, xmmVt, srcXmmWord
    m_Assembler->Pextrw(0 /*RAX*/, xmmVt, srcXmmWord);

    // Load vd into a temp XMM
    uint8_t xmmVd = m_RegState.MapXmmTemp(true, m_OpCode.vd, 0);
    m_RegState.ProtectXmm(xmmVd);

    // Insert the extracted word into vd at the destination position
    // pinsrw xmmVd, eax, dstXmmWord
    m_Assembler->Pinsrw(xmmVd, 0 /*RAX*/, dstXmmWord);

    // Store result
    if (WriteToVectorDest(m_OpCode.vd, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.vd], "VR[vd]", xmmVd);

    m_RegState.UnprotectXmm(xmmVt);
    m_RegState.UnprotectXmm(xmmVd);
}

// ── Misc / rare ──

VECTOR_FALLBACK(VRNDP)  VECTOR_FALLBACK(VRNDN)  VECTOR_FALLBACK(VMULQ)
VECTOR_FALLBACK(VMACQ)  VECTOR_FALLBACK(VNOOP)  VECTOR_FALLBACK(Reserved)

#undef VECTOR_FALLBACK
#undef VECTOR_PREAMBLE

// ══════════════════════════════════════════════
// Scalar memory (LB/LH/LW/LBU/LHU/LWU/SB/SH/SW)
// DMEM is word-swapped: aligned 32-bit loads need no byte swap,
// byte access uses addr ^ 3, halfword access uses addr ^ 2.
// RBP = DMEM base pointer during recompiled code execution.
// ══════════════════════════════════════════════

void CRSPRecompilerOps::LB()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; LB r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);
    m_RegState.SetGprUnknown(m_OpCode.rt);

    // addr = (GPR[rs] + offset) & 0xFFF; addr ^= 3 (word-swapped byte access)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFF);
    m_Assembler->XorConstToX86reg(0, 3);   // byte swap within word

    // movsx eax, byte [rbp + rax]  (RBP=DMEM base)
    m_Assembler->MovsxByteFromBaseIndex(0, 5, 0);  // dst=RAX, base=RBP, index=RAX
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].W, "GPR[rt]", 0);
}

void CRSPRecompilerOps::LH()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; LH r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);
    m_RegState.SetGprUnknown(m_OpCode.rt);

    // addr = (GPR[rs] + offset) & 0xFFE; addr ^= 2 (halfword swap)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFE);
    m_Assembler->XorConstToX86reg(0, 2);

    // movsx eax, word [rbp + rax] (sign-extended halfword load)
    m_Assembler->MovsxWordFromBaseIndex(0, 5, 0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].W, "GPR[rt]", 0);
}

void CRSPRecompilerOps::LW()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; LW r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);
    m_RegState.SetGprUnknown(m_OpCode.rt);

    // addr = (GPR[rs] + offset) & 0xFFC (word aligned, no swap needed)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFC);

    // mov eax, dword [rbp + rax]
    m_Assembler->MovDwordFromBaseIndex(0, 5, 0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::LBU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; LBU r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);
    m_RegState.SetGprUnknown(m_OpCode.rt);

    // addr = (GPR[rs] + offset) & 0xFFF; addr ^= 3
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFF);
    m_Assembler->XorConstToX86reg(0, 3);

    // movzx eax, byte [rbp + rax]
    m_Assembler->MovzxByteFromBaseIndex(0, 5, 0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::LHU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; LHU r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);
    m_RegState.SetGprUnknown(m_OpCode.rt);

    // addr = (GPR[rs] + offset) & 0xFFE; addr ^= 2
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFE);
    m_Assembler->XorConstToX86reg(0, 2);

    // movzx eax, word [rbp + rax]
    m_Assembler->MovzxWordFromBaseIndex(0, 5, 0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::LWU()
{
    if (m_Assembler == nullptr) return;
    if (m_OpCode.rt == 0) return;

    m_Recompiler.Log("; LWU r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);
    m_RegState.SetGprUnknown(m_OpCode.rt);

    // LWU is same as LW on 32-bit RSP (no sign extension difference)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFC);

    m_Assembler->MovDwordFromBaseIndex(0, 5, 0);
    m_Assembler->MoveX86regToVariable(&m_GPR[m_OpCode.rt].UW, "GPR[rt]", 0);
}

void CRSPRecompilerOps::SB()
{
    if (m_Assembler == nullptr) return;

    m_Recompiler.Log("; SB r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);

    // Load value into ECX (to avoid clobbering RAX which we need for address)
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");

    // addr = (GPR[rs] + offset) & 0xFFF; addr ^= 3
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFF);
    m_Assembler->XorConstToX86reg(0, 3);

    // mov byte [rbp + rax], cl
    m_Assembler->MovByteToBaseIndex(5, 0, 1);  // base=RBP, index=RAX, src=RCX
}

void CRSPRecompilerOps::SH()
{
    if (m_Assembler == nullptr) return;

    m_Recompiler.Log("; SH r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);

    // Load value into ECX
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");

    // addr = (GPR[rs] + offset) & 0xFFE; addr ^= 2 (halfword swap)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFE);
    m_Assembler->XorConstToX86reg(0, 2);

    // mov word [rbp + rax], cx
    m_Assembler->MovWordToBaseIndex(5, 0, 1);
}

void CRSPRecompilerOps::SW()
{
    if (m_Assembler == nullptr) return;

    m_Recompiler.Log("; SW r%u, %d(r%u)\n", m_OpCode.rt, m_OpCode.offset, m_OpCode.rs);

    // Load value into ECX
    m_Assembler->MoveVariableToX86reg(1, &m_GPR[m_OpCode.rt].UW, "GPR[rt]");

    // addr = (GPR[rs] + offset) & 0xFFC (word aligned, no swap needed)
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.rs].UW, "GPR[rs]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.offset));
    m_Assembler->AndConstToX86reg(0, 0xFFC);

    // mov dword [rbp + rax], ecx
    m_Assembler->MovDwordToBaseIndex(5, 0, 1);
}

// ══════════════════════════════════════════════
// Vector Load/Store (LC2/SC2)
// ══════════════════════════════════════════════

void CRSPRecompilerOps::LC2()
{
    switch (m_OpCode.rd) {
    case 0:  Opcode_LBV(); break;
    case 1:  Opcode_LSV(); break;
    case 2:  Opcode_LLV(); break;
    case 3:  Opcode_LDV(); break;
    case 4:  Opcode_LQV(); break;
    case 5:  Opcode_LRV(); break;
    case 6:  Opcode_LPV(); break;
    case 7:  Opcode_LUV(); break;
    case 8:  Opcode_LHV(); break;
    case 9:  Opcode_LFV(); break;
    case 10: Opcode_LWV(); break;
    case 11: Opcode_LTV(); break;
    default: UnknownOpcode(); break;
    }
}

void CRSPRecompilerOps::SC2()
{
    switch (m_OpCode.rd) {
    case 0:  Opcode_SBV(); break;
    case 1:  Opcode_SSV(); break;
    case 2:  Opcode_SLV(); break;
    case 3:  Opcode_SDV(); break;
    case 4:  Opcode_SQV(); break;
    case 5:  Opcode_SRV(); break;
    case 6:  Opcode_SPV(); break;
    case 7:  Opcode_SUV(); break;
    case 8:  Opcode_SHV(); break;
    case 9:  Opcode_SFV(); break;
    case 10: Opcode_SWV(); break;
    case 11: Opcode_STV(); break;
    default: UnknownOpcode(); break;
    }
}

// ══════════════════════════════════════════════════════════════════════
// Vector load/store operations (24 opcodes: LBV-LTV, SBV-STV)
// Addressing: address = (rs + (offset << element_shift)) DMEM offset
// Element-wise: stride and element size varies by opcode (LBV=1B, LSV=2B, etc)
// Element masking: bits in element field select which of 16 vector elements
// ══════════════════════════════════════════════════════════════════════

// Load vector byte (element = 1 byte)
void CRSPRecompilerOps::Opcode_LBV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LBV, "RSP_Opcode_LBV");
}

// Load vector short (element = 2 bytes)
void CRSPRecompilerOps::Opcode_LSV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LSV, "RSP_Opcode_LSV");
}

// Load vector long (element = 4 bytes)
void CRSPRecompilerOps::Opcode_LLV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LLV, "RSP_Opcode_LLV");
}

// Load vector double (element = 8 bytes)
void CRSPRecompilerOps::Opcode_LDV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LDV, "RSP_Opcode_LDV");
}

// Load vector quad (element = 16 bytes / full vector)
void CRSPRecompilerOps::Opcode_LQV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);

    if (m_OpCode.del != 0) {
        // Non-zero element: fallback to interpreter
        m_RegState.WriteBackRegisters();
        Cheat_r4300iOpcode(&RSPOp::LQV, "RSP_Opcode_LQV");
        return;
    }

    m_Recompiler.Log("; LQV v%u[0], %d(r%u)\n",
                     m_OpCode.rt, m_OpCode.voffset, m_OpCode.base);

    // addr = (GPR[base] + (voffset << 4)) & 0xFFF
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.base].UW, "GPR[base]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.voffset << 4));
    m_Assembler->AndConstToX86reg(0, 0xFFF);

    // Load 16 bytes from DMEM[addr] (unaligned OK) and convert byte order
    // DMEM is word-swapped: pshufd 0x1B reverses dword order → correct vector layout
    uint8_t xmmTmp = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Movdqu_XmmFromBaseIndex(xmmTmp, 5, 0);  // movdqu xmm, [rbp+rax]
    m_Assembler->Pshufd_XmmToXmm(xmmTmp, xmmTmp, 0x1B);  // reverse dword order

    if (WriteToVectorDest(m_OpCode.rt, m_CompilePC))
        m_Assembler->Movdqa_MemFromXmm(&m_Vect[m_OpCode.rt], "VR[vt]", xmmTmp);
}

// Load vector rest (remaining bytes after last element)
void CRSPRecompilerOps::Opcode_LRV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LRV, "RSP_Opcode_LRV");
}

// Load vector pack/partial (element = 16 bytes with stride)
void CRSPRecompilerOps::Opcode_LPV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LPV, "RSP_Opcode_LPV");
}

// Load vector unpack (element = 8 bytes with stride)
void CRSPRecompilerOps::Opcode_LUV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LUV, "RSP_Opcode_LUV");
}

// Load vector half-word (element = 2 bytes with stride)
void CRSPRecompilerOps::Opcode_LHV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LHV, "RSP_Opcode_LHV");
}

// Load vector fourth (element = 4 bytes)
void CRSPRecompilerOps::Opcode_LFV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LFV, "RSP_Opcode_LFV");
}

// Load vector word (element = 4 bytes with stride)
void CRSPRecompilerOps::Opcode_LWV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LWV, "RSP_Opcode_LWV");
}

// Load vector transpose (16x16 matrix transpose)
void CRSPRecompilerOps::Opcode_LTV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.SetVecRegUnknown(m_OpCode.rt);
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::LTV, "RSP_Opcode_LTV");
}

// Store vector byte
void CRSPRecompilerOps::Opcode_SBV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SBV, "RSP_Opcode_SBV");
}

// Store vector short
void CRSPRecompilerOps::Opcode_SSV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SSV, "RSP_Opcode_SSV");
}

// Store vector long
void CRSPRecompilerOps::Opcode_SLV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SLV, "RSP_Opcode_SLV");
}

// Store vector double
void CRSPRecompilerOps::Opcode_SDV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SDV, "RSP_Opcode_SDV");
}

// Store vector quad
void CRSPRecompilerOps::Opcode_SQV()
{
    if (m_Assembler == nullptr) return;

    if (m_OpCode.del != 0) {
        m_RegState.WriteBackRegisters();
        Cheat_r4300iOpcode(&RSPOp::SQV, "RSP_Opcode_SQV");
        return;
    }

    m_Recompiler.Log("; SQV v%u[0], %d(r%u)\n",
                     m_OpCode.rt, m_OpCode.voffset, m_OpCode.base);

    // addr = (GPR[base] + (voffset << 4)) & 0xFFF
    m_Assembler->MoveVariableToX86reg(0, &m_GPR[m_OpCode.base].UW, "GPR[base]");
    m_Assembler->AddConstToX86reg(0, static_cast<uint32_t>(m_OpCode.voffset << 4));
    m_Assembler->AndConstToX86reg(0, 0xFFF);

    // Load vector, reverse dword order, store to DMEM
    uint8_t xmmTmp = m_RegState.MapXmmTemp(true, m_OpCode.rt, 0);
    m_RegState.ProtectXmm(xmmTmp);
    uint8_t xmmSwap = m_RegState.MapXmmTemp(false, 0, 0);
    m_Assembler->Pshufd_XmmToXmm(xmmSwap, xmmTmp, 0x1B);
    m_Assembler->Movdqu_BaseIndexFromXmm(5, 0, xmmSwap);  // movdqu [rbp+rax], xmm
    m_RegState.UnprotectXmm(xmmTmp);
}

// Store vector rest
void CRSPRecompilerOps::Opcode_SRV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SRV, "RSP_Opcode_SRV");
}

// Store vector pack
void CRSPRecompilerOps::Opcode_SPV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SPV, "RSP_Opcode_SPV");
}

// Store vector unpack
void CRSPRecompilerOps::Opcode_SUV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SUV, "RSP_Opcode_SUV");
}

// Store vector half-word
void CRSPRecompilerOps::Opcode_SHV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SHV, "RSP_Opcode_SHV");
}

// Store vector fourth
void CRSPRecompilerOps::Opcode_SFV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SFV, "RSP_Opcode_SFV");
}

// Store vector word
void CRSPRecompilerOps::Opcode_SWV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::SWV, "RSP_Opcode_SWV");
}

// Store vector transpose
void CRSPRecompilerOps::Opcode_STV()
{
    if (m_Assembler == nullptr) return;
    m_RegState.WriteBackRegisters();
    Cheat_r4300iOpcode(&RSPOp::STV, "RSP_Opcode_STV");
}

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
