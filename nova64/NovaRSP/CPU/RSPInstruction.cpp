/**
 * @file RSPInstruction.cpp
 * @brief RSP Instruction Analysis Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "RSPInstruction.hpp"
#include "RspMemory.hpp"
#include "RspSystem.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Nova64::RSP {

static inline const char* GPR_Name(uint32_t reg) { return (reg < 32) ? GPR_Names[reg] : "???"; }

// --- Construction / Copy ---

RSPInstruction::RSPInstruction(uint32_t address, uint32_t instruction) :
    m_Address(address), m_Instruction(RSPOpcode(instruction)) {}

RSPInstruction::RSPInstruction(const RSPInstruction& other) :
    m_Address(other.m_Address), m_Instruction(other.m_Instruction) {
    // The lazy analysis state starts cleared; callers must re-analyze.
}

RSPInstruction& RSPInstruction::operator=(const RSPInstruction& other) {
    if (this != &other) {
        m_Address = other.m_Address;
        m_Instruction = other.m_Instruction;

        m_Flag = RSPInstructionFlag::Unknown;
        m_Name[0] = '\0';
        m_Param[0] = '\0';
        m_Analyzed = false;
        m_isBranch = false;
        m_isJump = false;
        m_isGprOp = false;
        m_isVectorOp = false;
        m_isVectorStoreOp = false;
        m_isMfCop2 = false;
        m_isMtCop2 = false;
        m_ReadAccumLow = false;
        m_ReadAccumMid = false;
        m_ReadAccumHigh = false;
        m_SetAccumLow = false;
        m_SetAccumMid = false;
        m_SetAccumHigh = false;
        m_DestReg = UNUSED_OPERAND;
        m_SourceReg0 = UNUSED_OPERAND;
        m_SourceReg1 = UNUSED_OPERAND;
    }
    return *this;
}

// --- Address / Target Queries ---

uint32_t RSPInstruction::ConditionalBranchTarget() const {
    return (m_Address + (static_cast<int16_t>(m_Instruction.offset) << 2) + 4) & 0x1FFC;
}

uint32_t RSPInstruction::JumpTarget() const { return (m_Instruction.target << 2) & 0x1FFC; }

uint32_t RSPInstruction::StaticCallTarget() const { return JumpTarget(); }

// --- Classification Queries ---

bool RSPInstruction::IsJump() const { return m_Instruction.op == RSP_J; }

bool RSPInstruction::IsJumpReturn() const {
    if (m_Instruction.op != RSP_SPECIAL) return false;
    const auto funct = m_Instruction.funct;
    return (funct == RSP_SPECIAL_JR || funct == RSP_SPECIAL_JALR) && m_Instruction.rs == 31;
}

bool RSPInstruction::IsRegisterJump() const {
    if (m_Instruction.op != RSP_SPECIAL) return false;
    const auto funct = m_Instruction.funct;
    return funct == RSP_SPECIAL_JR || funct == RSP_SPECIAL_JALR;
}

bool RSPInstruction::IsStaticCall() const { return m_Instruction.op == RSP_JAL; }

bool RSPInstruction::DelaySlotAffectBranch() const {
    // Read the instruction at PC+4 from IMEM
    const uint8_t* imem = RSPSystem.IMEM();
    if (!imem) return false;

    uint32_t dsValue = 0;
    RSP_LW_IMEM(imem, m_Address + 4, &dsValue);
    RSPInstruction delaySlot(m_Address + 4, dsValue);

    if (!delaySlot.m_Analyzed) {
        delaySlot.AnalyzeInstruction();
    }

    // Check if the delay slot writes to a register the branch reads
    if (delaySlot.m_DestReg == UNUSED_OPERAND) return false;

    switch (m_Instruction.op) {
    case RSP_REGIMM:
        switch (m_Instruction.rt) {
        case RSP_REGIMM_BLTZ:
        case RSP_REGIMM_BGEZ:
        case RSP_REGIMM_BLTZAL:
        case RSP_REGIMM_BGEZAL: return delaySlot.m_DestReg == m_Instruction.rs;
        default:                break;
        }
        break;
    case RSP_BEQ:
    case RSP_BNE:  return delaySlot.m_DestReg == m_Instruction.rs || delaySlot.m_DestReg == m_Instruction.rt;
    case RSP_BLEZ:
    case RSP_BGTZ: return delaySlot.m_DestReg == m_Instruction.rs;
    default:       break;
    }
    return false;
}

bool RSPInstruction::IsBranch() const {
    switch (m_Instruction.op) {
    case RSP_REGIMM:
        switch (m_Instruction.rt) {
        case RSP_REGIMM_BLTZ:
        case RSP_REGIMM_BGEZ:
        case RSP_REGIMM_BLTZAL:
        case RSP_REGIMM_BGEZAL: return true;
        default:                break;
        }
        break;
    case RSP_SPECIAL:
        switch (m_Instruction.funct) {
        case RSP_SPECIAL_JR:
        case RSP_SPECIAL_JALR: return true;
        default:               break;
        }
        break;
    case RSP_J:
    case RSP_JAL:  return true;
    case RSP_BEQ:
    case RSP_BNE:
    case RSP_BLEZ:
    case RSP_BGTZ: return true;
    default:       break;
    }
    return false;
}

bool RSPInstruction::IsConditionalBranch() const {
    switch (m_Instruction.op) {
    case RSP_REGIMM:
        switch (m_Instruction.rt) {
        case RSP_REGIMM_BLTZ:
        case RSP_REGIMM_BGEZ:
        case RSP_REGIMM_BLTZAL:
        case RSP_REGIMM_BGEZAL: return true;
        default:                break;
        }
        break;
    case RSP_BEQ:
    case RSP_BNE:
    case RSP_BLEZ:
    case RSP_BGTZ: return true;
    default:       break;
    }
    return false;
}

bool RSPInstruction::IsNop() const {
    return m_Instruction.op == RSP_SPECIAL && m_Instruction.funct == RSP_SPECIAL_SLL && m_Instruction.rd == 0;
}

// --- Lazy-Analyzed Accessors ---

bool RSPInstruction::IsVectorOp() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_isVectorOp;
}

bool RSPInstruction::IsVectorStoreOp() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_isVectorStoreOp;
}

bool RSPInstruction::IsMfCop2() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_isMfCop2;
}

bool RSPInstruction::IsMtCop2() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_isMtCop2;
}

bool RSPInstruction::ReadAccumLow() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_ReadAccumLow;
}

bool RSPInstruction::ReadAccumMid() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_ReadAccumMid;
}

bool RSPInstruction::ReadAccumHigh() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_ReadAccumHigh;
}

bool RSPInstruction::SetAccumLow() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_SetAccumLow;
}

bool RSPInstruction::SetAccumMid() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_SetAccumMid;
}

bool RSPInstruction::SetAccumHigh() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_SetAccumHigh;
}

RSPInstructionFlag RSPInstruction::Flag() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_Flag;
}

uint32_t RSPInstruction::DestReg() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_DestReg;
}

uint32_t RSPInstruction::SourceReg0() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_SourceReg0;
}

uint32_t RSPInstruction::SourceReg1() const {
    if (!m_Analyzed) AnalyzeInstruction();
    return m_SourceReg1;
}

// --- Disassembly Accessors ---

const char* RSPInstruction::Name() const {
    if (m_Name[0] == '\0') DecodeName();
    return m_Name;
}

const char* RSPInstruction::Param() const {
    if (m_Param[0] == '\0') DecodeName();
    return m_Param;
}

std::string RSPInstruction::NameAndParam() const {
    const char* n = Name();
    const char* p = Param();
    if (p[0] == '\0') return std::string(n);
    return std::string(n) + " " + p;
}

// --- Instruction Analysis (full opcode classification) ---

void RSPInstruction::AnalyzeInstruction() const {
    m_isGprOp = false;
    m_isVectorOp = false;
    m_isVectorStoreOp = false;
    m_isMfCop2 = false;
    m_isMtCop2 = false;
    m_ReadAccumLow = false;
    m_ReadAccumMid = false;
    m_ReadAccumHigh = false;
    m_SetAccumLow = false;
    m_SetAccumMid = false;
    m_SetAccumHigh = false;
    m_isJump = false;
    m_isBranch = false;
    m_DestReg = UNUSED_OPERAND;
    m_SourceReg0 = UNUSED_OPERAND;
    m_SourceReg1 = UNUSED_OPERAND;
    m_Flag = RSPInstructionFlag::Unknown;
    m_Analyzed = true;

    switch (m_Instruction.op) {
    // --- SPECIAL ---
    case RSP_SPECIAL:
        switch (m_Instruction.funct) {
        case RSP_SPECIAL_BREAK: m_Flag = RSPInstructionFlag::Break; break;
        case RSP_SPECIAL_SLL:
        case RSP_SPECIAL_SRL:
        case RSP_SPECIAL_SRA:
            m_DestReg = m_Instruction.rd;
            m_SourceReg0 = m_Instruction.rt;
            m_isGprOp = true;
            m_Flag = RSPInstructionFlag::GPROperation;
            break;
        case RSP_SPECIAL_SLLV:
        case RSP_SPECIAL_SRLV:
        case RSP_SPECIAL_SRAV:
        case RSP_SPECIAL_ADD:
        case RSP_SPECIAL_ADDU:
        case RSP_SPECIAL_SUB:
        case RSP_SPECIAL_SUBU:
        case RSP_SPECIAL_AND:
        case RSP_SPECIAL_OR:
        case RSP_SPECIAL_XOR:
        case RSP_SPECIAL_NOR:
        case RSP_SPECIAL_SLT:
        case RSP_SPECIAL_SLTU:
            m_DestReg = m_Instruction.rd;
            m_SourceReg0 = m_Instruction.rs;
            m_SourceReg1 = m_Instruction.rt;
            m_isGprOp = true;
            m_Flag = RSPInstructionFlag::GPROperation;
            break;
        case RSP_SPECIAL_JR:
        case RSP_SPECIAL_JALR:
            m_Flag = RSPInstructionFlag::JumpRegister;
            m_isJump = true;
            m_SourceReg0 = m_Instruction.rs;
            m_isGprOp = true;
            break;
        default: m_Flag = RSPInstructionFlag::InvalidOpcode; break;
        }
        break;

    // --- REGIMM ---
    case RSP_REGIMM:
        switch (m_Instruction.rt) {
        case RSP_REGIMM_BLTZ:
        case RSP_REGIMM_BGEZ:
        case RSP_REGIMM_BLTZAL:
        case RSP_REGIMM_BGEZAL:
            m_Flag = RSPInstructionFlag::Branch;
            m_isBranch = true;
            m_isGprOp = true;
            m_SourceReg0 = m_Instruction.rs;
            break;
        default: m_Flag = RSPInstructionFlag::InvalidOpcode; break;
        }
        break;

    // --- Unconditional jumps ---
    case RSP_J:
    case RSP_JAL:
        m_Flag = RSPInstructionFlag::Jump;
        m_isJump = true;
        m_isGprOp = true;
        break;

    // --- Conditional branches (two-register) ---
    case RSP_BEQ:
    case RSP_BNE:
        m_Flag = RSPInstructionFlag::Branch;
        m_isBranch = true;
        m_isGprOp = true;
        m_SourceReg0 = m_Instruction.rt;
        m_SourceReg1 = m_Instruction.rs;
        break;

    // --- Conditional branches (one-register) ---
    case RSP_BLEZ:
    case RSP_BGTZ:
        m_Flag = RSPInstructionFlag::Branch;
        m_isBranch = true;
        m_isGprOp = true;
        m_SourceReg0 = m_Instruction.rs;
        m_SourceReg1 = UNUSED_OPERAND;
        break;

    // --- Immediate ALU ---
    case RSP_ADDI:
    case RSP_ADDIU:
    case RSP_SLTI:
    case RSP_SLTIU:
    case RSP_ANDI:
    case RSP_ORI:
    case RSP_XORI:
        m_isGprOp = true;
        m_DestReg = m_Instruction.rt;
        m_SourceReg0 = m_Instruction.rs;
        m_Flag = RSPInstructionFlag::GPROperation;
        break;

    case RSP_LUI:
        m_isGprOp = true;
        m_DestReg = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::GPROperation;
        break;

    // --- COP0 ---
    case RSP_CP0:
        switch (m_Instruction.rs) {
        case RSP_COP0_MF:
            m_isGprOp = true;
            m_DestReg = m_Instruction.rt;
            m_Flag = RSPInstructionFlag::MF;
            break;
        case RSP_COP0_MT:
            m_isGprOp = true;
            m_SourceReg0 = m_Instruction.rt;
            m_Flag = RSPInstructionFlag::MT;
            break;
        default: m_Flag = RSPInstructionFlag::InvalidOpcode; break;
        }
        break;

    // --- COP2 (Vector Unit) ---
    case RSP_CP2:
        if ((m_Instruction.rs & 0x10) != 0) {
            AnalyzeVectorOp();
        } else {
            AnalyzeCop2ScalarOp();
        }
        break;

    // --- Scalar loads ---
    case RSP_LB:
    case RSP_LH:
    case RSP_LW:
    case RSP_LBU:
    case RSP_LHU:
    case RSP_LWU:
        m_isGprOp = true;
        m_DestReg = m_Instruction.rt;
        m_SourceReg0 = m_Instruction.base;
        m_Flag = RSPInstructionFlag::Load;
        break;

    // --- Scalar stores ---
    case RSP_SB:
    case RSP_SH:
    case RSP_SW:
        m_isGprOp = true;
        m_SourceReg0 = m_Instruction.rt;
        m_SourceReg1 = m_Instruction.base;
        m_Flag = RSPInstructionFlag::Store;
        break;

    // --- Vector loads (LC2) ---
    case RSP_LC2:
        switch (m_Instruction.rd) {
        case RSP_LSC2_BV:
        case RSP_LSC2_SV:
        case RSP_LSC2_LV:
        case RSP_LSC2_DV:
        case RSP_LSC2_QV:
        case RSP_LSC2_RV:
        case RSP_LSC2_PV:
        case RSP_LSC2_UV:
            m_isVectorOp = true;
            m_DestReg = m_Instruction.rt;
            m_SourceReg0 = m_Instruction.base;
            m_SourceReg1 = UNUSED_OPERAND;
            m_Flag = RSPInstructionFlag::VectorLoad;
            break;
        case RSP_LSC2_TV:
        default:          m_Flag = RSPInstructionFlag::InvalidOpcode; break;
        }
        break;

    // --- Vector stores (SC2) ---
    case RSP_SC2:
        switch (m_Instruction.rd) {
        case RSP_LSC2_BV:
        case RSP_LSC2_SV:
        case RSP_LSC2_LV:
        case RSP_LSC2_DV:
        case RSP_LSC2_QV:
        case RSP_LSC2_RV:
        case RSP_LSC2_PV:
        case RSP_LSC2_UV:
        case RSP_LSC2_HV:
        case RSP_LSC2_FV:
        case RSP_LSC2_WV:
            m_isVectorStoreOp = true;
            m_SourceReg0 = m_Instruction.rt; // vt (source vector)
            m_SourceReg1 = m_Instruction.base;
            m_Flag = RSPInstructionFlag::VectorStore;
            break;
        case RSP_LSC2_TV:
        default:          m_Flag = RSPInstructionFlag::InvalidOpcode; break;
        }
        break;

    default: m_Flag = RSPInstructionFlag::InvalidOpcode; break;
    }
}

// Vector computational sub-analysis (called from AnalyzeInstruction)
void RSPInstruction::AnalyzeVectorOp() const {
    switch (m_Instruction.funct) {
    case RSP_VECTOR_VNOP: m_Flag = RSPInstructionFlag::Vector; break;

    // Multiply-direct: write all three accumulator levels
    case RSP_VECTOR_VMULF:
    case RSP_VECTOR_VMULU:
    case RSP_VECTOR_VMUDL:
    case RSP_VECTOR_VMUDM:
    case RSP_VECTOR_VMUDN:
    case RSP_VECTOR_VMUDH:
        m_isVectorOp = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rd;
        m_SourceReg1 = m_Instruction.rt;
        m_SetAccumLow = true;
        m_SetAccumMid = true;
        m_SetAccumHigh = true;
        m_Flag = RSPInstructionFlag::VectorSetAccum;
        break;

    // Logical / ABS: write accumulator low only
    case RSP_VECTOR_VABS:
    case RSP_VECTOR_VAND:
    case RSP_VECTOR_VOR:
    case RSP_VECTOR_VXOR:
    case RSP_VECTOR_VNAND:
    case RSP_VECTOR_VNOR:
    case RSP_VECTOR_VNXOR:
        m_isVectorOp = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rd;
        m_SourceReg1 = m_Instruction.rt;
        m_SetAccumLow = true;
        m_Flag = RSPInstructionFlag::VectorSetAccum;
        break;

    // Multiply-accumulate: read & write all three levels
    case RSP_VECTOR_VMACF:
    case RSP_VECTOR_VMACU:
    case RSP_VECTOR_VMADL:
    case RSP_VECTOR_VMADM:
    case RSP_VECTOR_VMADN:
        m_ReadAccumLow = true;
        m_ReadAccumMid = true;
        m_ReadAccumHigh = true;
        m_SetAccumLow = true;
        m_SetAccumMid = true;
        m_SetAccumHigh = true;
        m_isVectorOp = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rd;
        m_SourceReg1 = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::VectorUseAccum;
        break;

    // VMADH: reads all three but only writes mid+high
    case RSP_VECTOR_VMADH:
        m_ReadAccumLow = true;
        m_ReadAccumMid = true;
        m_ReadAccumHigh = true;
        m_SetAccumMid = true;
        m_SetAccumHigh = true;
        m_isVectorOp = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rd;
        m_SourceReg1 = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::VectorUseAccum;
        break;

    // Add/sub/compare/clip: set accumulator low
    case RSP_VECTOR_VADD:
    case RSP_VECTOR_VADDC:
    case RSP_VECTOR_VSUB:
    case RSP_VECTOR_VSUBC:
    case RSP_VECTOR_VCR:
    case RSP_VECTOR_VCH:
    case RSP_VECTOR_VCL:
    case RSP_VECTOR_VLT:
    case RSP_VECTOR_VEQ:
    case RSP_VECTOR_VGE:
    case RSP_VECTOR_VNE:
        m_isVectorOp = true;
        m_SetAccumLow = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rd;
        m_SourceReg1 = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::VectorSetAccum;
        break;

    // Reciprocal / sqrt / move: single-element, set accum low
    case RSP_VECTOR_VMOV:
    case RSP_VECTOR_VRCP:
    case RSP_VECTOR_VRCPL:
    case RSP_VECTOR_VRCPH:
    case RSP_VECTOR_VRSQ:
    case RSP_VECTOR_VRSQL:
    case RSP_VECTOR_VRSQH:
        m_isVectorOp = true;
        m_SetAccumLow = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rt;
        m_SourceReg1 = UNUSED_OPERAND;
        m_Flag = RSPInstructionFlag::VectorSetAccum;
        break;

    // VMRG: merge using VCC flags
    case RSP_VECTOR_VMRG:
        m_isVectorOp = true;
        m_SetAccumLow = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = m_Instruction.rt;
        m_SourceReg1 = m_Instruction.rd;
        m_Flag = RSPInstructionFlag::VectorSetAccum;
        break;

    // VSAW: read all accumulator levels
    case RSP_VECTOR_VSAW:
        m_isVectorOp = true;
        m_ReadAccumLow = true;
        m_ReadAccumMid = true;
        m_ReadAccumHigh = true;
        m_DestReg = m_Instruction.sa;
        m_SourceReg0 = UNUSED_OPERAND;
        m_SourceReg1 = UNUSED_OPERAND;
        m_Flag = RSPInstructionFlag::VectorUseAccum;
        break;

    default: m_Flag = RSPInstructionFlag::InvalidOpcode; break;
    }
}

// --- COP2 scalar sub-analysis (MF/CF/MT/CT) ---
void RSPInstruction::AnalyzeCop2ScalarOp() const {
    switch (m_Instruction.rs) {
    case RSP_COP2_CT:
        m_isGprOp = true;
        m_SourceReg0 = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::CT;
        break;
    case RSP_COP2_CF:
        m_isGprOp = true;
        m_DestReg = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::CF;
        break;
    case RSP_COP2_MT:
        m_isMtCop2 = true;
        m_DestReg = m_Instruction.sa >> 1; // vs field
        m_SourceReg0 = m_Instruction.rt;
        m_Flag = RSPInstructionFlag::MT;
        break;
    case RSP_COP2_MF:
        m_isMfCop2 = true;
        m_DestReg = m_Instruction.rt;
        m_SourceReg0 = m_Instruction.sa >> 1; // vs field
        m_Flag = RSPInstructionFlag::MF;
        break;
    default: m_Flag = RSPInstructionFlag::InvalidOpcode; break;
    }
}

// --- Disassembly ---

void RSPInstruction::DecodeName() const {
    switch (m_Instruction.op) {
    case RSP_SPECIAL: DecodeSpecialName(); break;
    case RSP_REGIMM:  DecodeRegImmName(); break;
    case RSP_J:
        std::strcpy(m_Name, "J");
        std::snprintf(m_Param, sizeof(m_Param), "0x%04X", (m_Instruction.target << 2) & 0x1FFC);
        break;
    case RSP_JAL:
        std::strcpy(m_Name, "JAL");
        std::snprintf(m_Param, sizeof(m_Param), "0x%04X", (m_Instruction.target << 2) & 0x1FFC);
        break;
    case RSP_BEQ:
        if (m_Instruction.rs == 0 && m_Instruction.rt == 0) {
            std::strcpy(m_Name, "B");
            std::snprintf(m_Param, sizeof(m_Param), "0x%04X", ConditionalBranchTarget());
        } else if (m_Instruction.rs == 0 || m_Instruction.rt == 0) {
            std::strcpy(m_Name, "BEQZ");
            std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X",
                          GPR_Name(m_Instruction.rs == 0 ? m_Instruction.rt : m_Instruction.rs),
                          ConditionalBranchTarget());
        } else {
            std::strcpy(m_Name, "BEQ");
            std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rs),
                          GPR_Name(m_Instruction.rt), ConditionalBranchTarget());
        }
        break;
    case RSP_BNE:
        if ((m_Instruction.rs == 0) ^ (m_Instruction.rt == 0)) {
            std::strcpy(m_Name, "BNEZ");
            std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X",
                          GPR_Name(m_Instruction.rs == 0 ? m_Instruction.rt : m_Instruction.rs),
                          ConditionalBranchTarget());
        } else {
            std::strcpy(m_Name, "BNE");
            std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rs),
                          GPR_Name(m_Instruction.rt), ConditionalBranchTarget());
        }
        break;
    case RSP_BLEZ:
        std::strcpy(m_Name, "BLEZ");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rs), ConditionalBranchTarget());
        break;
    case RSP_BGTZ:
        std::strcpy(m_Name, "BGTZ");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rs), ConditionalBranchTarget());
        break;
    case RSP_ADDI:
        std::strcpy(m_Name, "ADDI");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_ADDIU:
        std::strcpy(m_Name, "ADDIU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_SLTI:
        std::strcpy(m_Name, "SLTI");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_SLTIU:
        std::strcpy(m_Name, "SLTIU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_ANDI:
        std::strcpy(m_Name, "ANDI");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_ORI:
        std::strcpy(m_Name, "ORI");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_XORI:
        std::strcpy(m_Name, "XORI");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%04X", GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs), m_Instruction.immediate);
        break;
    case RSP_LUI:
        std::strcpy(m_Name, "LUI");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rt), m_Instruction.immediate);
        break;
    case RSP_CP0: DecodeCop0Name(); break;
    case RSP_CP2: DecodeCop2Name(); break;
    case RSP_LB:
        std::strcpy(m_Name, "LB");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_LH:
        std::strcpy(m_Name, "LH");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_LW:
        std::strcpy(m_Name, "LW");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_LBU:
        std::strcpy(m_Name, "LBU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_LHU:
        std::strcpy(m_Name, "LHU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_LWU:
        std::strcpy(m_Name, "LWU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_SB:
        std::strcpy(m_Name, "SB");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_SH:
        std::strcpy(m_Name, "SH");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_SW:
        std::strcpy(m_Name, "SW");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X(%s)", GPR_Name(m_Instruction.rt), m_Instruction.offset,
                      GPR_Name(m_Instruction.base));
        break;
    case RSP_LC2: DecodeLSC2Name('L'); break;
    case RSP_SC2: DecodeLSC2Name('S'); break;
    default:
        std::strcpy(m_Name, "UNKNOWN");
        std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
        break;
    }
}

// --- SPECIAL function disassembly ---
void RSPInstruction::DecodeSpecialName() const {
    switch (m_Instruction.funct) {
    case RSP_SPECIAL_SLL:
        if (m_Instruction.Value != 0) {
            std::strcpy(m_Name, "SLL");
            std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%X", GPR_Name(m_Instruction.rd),
                          GPR_Name(m_Instruction.rt), m_Instruction.sa);
        } else {
            std::strcpy(m_Name, "NOP");
            m_Param[0] = '\0';
        }
        break;
    case RSP_SPECIAL_SRL:
        std::strcpy(m_Name, "SRL");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%X", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rt),
                      m_Instruction.sa);
        break;
    case RSP_SPECIAL_SRA:
        std::strcpy(m_Name, "SRA");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, 0x%X", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rt),
                      m_Instruction.sa);
        break;
    case RSP_SPECIAL_SLLV:
        std::strcpy(m_Name, "SLLV");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs));
        break;
    case RSP_SPECIAL_SRLV:
        std::strcpy(m_Name, "SRLV");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs));
        break;
    case RSP_SPECIAL_SRAV:
        std::strcpy(m_Name, "SRAV");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rt),
                      GPR_Name(m_Instruction.rs));
        break;
    case RSP_SPECIAL_JR:
        std::strcpy(m_Name, "JR");
        std::snprintf(m_Param, sizeof(m_Param), "%s", GPR_Name(m_Instruction.rs));
        break;
    case RSP_SPECIAL_JALR:
        std::strcpy(m_Name, "JALR");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs));
        break;
    case RSP_SPECIAL_BREAK:
        std::strcpy(m_Name, "BREAK");
        m_Param[0] = '\0';
        break;
    case RSP_SPECIAL_ADD:
        std::strcpy(m_Name, "ADD");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_ADDU:
        std::strcpy(m_Name, "ADDU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_SUB:
        std::strcpy(m_Name, "SUB");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_SUBU:
        std::strcpy(m_Name, "SUBU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_AND:
        std::strcpy(m_Name, "AND");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_OR:
        std::strcpy(m_Name, "OR");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_XOR:
        std::strcpy(m_Name, "XOR");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_NOR:
        std::strcpy(m_Name, "NOR");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_SLT:
        std::strcpy(m_Name, "SLT");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    case RSP_SPECIAL_SLTU:
        std::strcpy(m_Name, "SLTU");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s, %s", GPR_Name(m_Instruction.rd), GPR_Name(m_Instruction.rs),
                      GPR_Name(m_Instruction.rt));
        break;
    default:
        std::strcpy(m_Name, "UNKNOWN");
        std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
        break;
    }
}

// --- REGIMM disassembly ---
void RSPInstruction::DecodeRegImmName() const {
    switch (m_Instruction.rt) {
    case RSP_REGIMM_BLTZ:
        std::strcpy(m_Name, "BLTZ");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rs), ConditionalBranchTarget());
        break;
    case RSP_REGIMM_BGEZ:
        if (m_Instruction.rs == 0) {
            std::strcpy(m_Name, "B");
            std::snprintf(m_Param, sizeof(m_Param), "0x%04X", ConditionalBranchTarget());
        } else {
            std::strcpy(m_Name, "BGEZ");
            std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rs),
                          ConditionalBranchTarget());
        }
        break;
    case RSP_REGIMM_BLTZAL:
        std::strcpy(m_Name, "BLTZAL");
        std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rs), ConditionalBranchTarget());
        break;
    case RSP_REGIMM_BGEZAL:
        if (m_Instruction.rs == 0) {
            std::strcpy(m_Name, "BAL");
            std::snprintf(m_Param, sizeof(m_Param), "0x%04X", ConditionalBranchTarget());
        } else {
            std::strcpy(m_Name, "BGEZAL");
            std::snprintf(m_Param, sizeof(m_Param), "%s, 0x%04X", GPR_Name(m_Instruction.rs),
                          ConditionalBranchTarget());
        }
        break;
    default:
        std::strcpy(m_Name, "UNKNOWN");
        std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
        break;
    }
}

// --- COP0 disassembly ---
void RSPInstruction::DecodeCop0Name() const {
    switch (m_Instruction.rs) {
    case RSP_COP0_MF:
        std::strcpy(m_Name, "MFC0");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s", GPR_Name(m_Instruction.rt), COP0_Name(m_Instruction.rd));
        break;
    case RSP_COP0_MT:
        std::strcpy(m_Name, "MTC0");
        std::snprintf(m_Param, sizeof(m_Param), "%s, %s", GPR_Name(m_Instruction.rt), COP0_Name(m_Instruction.rd));
        break;
    default:
        std::strcpy(m_Name, "UNKNOWN");
        std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
        break;
    }
}

// --- COP2 disassembly (scalar moves + vector ops) ---
void RSPInstruction::DecodeCop2Name() const {
    if ((m_Instruction.rs & 0x10) == 0) {
        // Scalar COP2 moves
        switch (m_Instruction.rs) {
        case RSP_COP2_MF:
            std::strcpy(m_Name, "MFC2");
            std::snprintf(m_Param, sizeof(m_Param), "%s, $v%d[%d]", GPR_Name(m_Instruction.rt), m_Instruction.rd,
                          m_Instruction.sa >> 1);
            break;
        case RSP_COP2_CF:
            std::strcpy(m_Name, "CFC2");
            std::snprintf(m_Param, sizeof(m_Param), "%s, %d", GPR_Name(m_Instruction.rt), m_Instruction.rd % 4);
            break;
        case RSP_COP2_MT:
            std::strcpy(m_Name, "MTC2");
            std::snprintf(m_Param, sizeof(m_Param), "%s, $v%d[%d]", GPR_Name(m_Instruction.rt), m_Instruction.rd,
                          m_Instruction.sa >> 1);
            break;
        case RSP_COP2_CT:
            std::strcpy(m_Name, "CTC2");
            std::snprintf(m_Param, sizeof(m_Param), "%s, %d", GPR_Name(m_Instruction.rt), m_Instruction.rd % 4);
            break;
        default:
            std::strcpy(m_Name, "UNKNOWN");
            std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
            break;
        }
        return;
    }

    // Vector computational opcodes
    const char* elemStr = ElementSpecifier(m_Instruction.e);

    switch (m_Instruction.funct) {
    // --- Multiply ops ---
    case RSP_VECTOR_VMULF:
        std::strcpy(m_Name, "VMULF");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMULU:
        std::strcpy(m_Name, "VMULU");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VRNDP:
        std::strcpy(m_Name, "VRNDP");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs & 1,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMULQ:
        std::strcpy(m_Name, "VMULQ");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMUDL:
        std::strcpy(m_Name, "VMUDL");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMUDM:
        std::strcpy(m_Name, "VMUDM");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMUDN:
        std::strcpy(m_Name, "VMUDN");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMUDH:
        std::strcpy(m_Name, "VMUDH");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Multiply-accumulate ---
    case RSP_VECTOR_VMACF:
        std::strcpy(m_Name, "VMACF");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMACU:
        std::strcpy(m_Name, "VMACU");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VRNDN:
        std::strcpy(m_Name, "VRNDN");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs & 1,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMACQ:
        std::strcpy(m_Name, "VMACQ");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMADL:
        std::strcpy(m_Name, "VMADL");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMADM:
        std::strcpy(m_Name, "VMADM");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMADN:
        std::strcpy(m_Name, "VMADN");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VMADH:
        std::strcpy(m_Name, "VMADH");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Add / subtract ---
    case RSP_VECTOR_VADD:
        std::strcpy(m_Name, "VADD");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUB:
        std::strcpy(m_Name, "VSUB");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUT:
        std::strcpy(m_Name, "Reserved (VSUT)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VABS:
        std::strcpy(m_Name, "VABS");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VADDC:
        std::strcpy(m_Name, "VADDC");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUBC:
        std::strcpy(m_Name, "VSUBC");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Reserved add/sub ---
    case RSP_VECTOR_VADDB:
        std::strcpy(m_Name, "Reserved (VADDB)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUBB:
        std::strcpy(m_Name, "Reserved (VSUBB)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VACCB:
        std::strcpy(m_Name, "Reserved (VACCB)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUCB:
        std::strcpy(m_Name, "Reserved (VSUCB)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSAD:
        std::strcpy(m_Name, "Reserved (VSAD)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSAC:
        std::strcpy(m_Name, "Reserved (VSAC)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUM:
        std::strcpy(m_Name, "Reserved (VSUM)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VACC:
        std::strcpy(m_Name, "Reserved (VACC)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VSUC:
        std::strcpy(m_Name, "Reserved (VSUC)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Accumulator read ---
    case RSP_VECTOR_VSAW:
        std::strcpy(m_Name, "VSAW");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Compare / select ---
    case RSP_VECTOR_VLT:
        std::strcpy(m_Name, "VLT");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VEQ:
        std::strcpy(m_Name, "VEQ");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VNE:
        std::strcpy(m_Name, "VNE");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VGE:
        std::strcpy(m_Name, "VGE");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Clip ---
    case RSP_VECTOR_VCL:
        std::strcpy(m_Name, "VCL");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VCH:
        std::strcpy(m_Name, "VCH");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VCR:
        std::strcpy(m_Name, "VCR");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Merge ---
    case RSP_VECTOR_VMRG:
        std::strcpy(m_Name, "VMRG");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Logical ---
    case RSP_VECTOR_VAND:
        std::strcpy(m_Name, "VAND");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VNAND:
        std::strcpy(m_Name, "VNAND");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VOR:
        std::strcpy(m_Name, "VOR");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VNOR:
        std::strcpy(m_Name, "VNOR");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VXOR:
        std::strcpy(m_Name, "VXOR");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_VNXOR:
        std::strcpy(m_Name, "VNXOR");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_V056:
        std::strcpy(m_Name, "Reserved (V056)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;
    case RSP_VECTOR_V057:
        std::strcpy(m_Name, "Reserved (V057)");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d, $v%d, $v%d%s", m_Instruction.vd, m_Instruction.vs,
                      m_Instruction.vt, elemStr);
        break;

    // --- Reciprocal / square-root / move ---
    case RSP_VECTOR_VRCP:
        std::strcpy(m_Name, "VRCP");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d[%d]", m_Instruction.vd, m_Instruction.rd & 0x7,
                      m_Instruction.vt, m_Instruction.e & 0x7);
        break;
    case RSP_VECTOR_VRCPL:
        std::strcpy(m_Name, "VRCPL");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d[%d]", m_Instruction.vd, m_Instruction.rd & 0x7,
                      m_Instruction.vt, m_Instruction.e & 0x7);
        break;
    case RSP_VECTOR_VRCPH:
        std::strcpy(m_Name, "VRCPH");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d%s[%d]", m_Instruction.vd, m_Instruction.de & 0x7,
                      m_Instruction.vt, elemStr, m_Instruction.de & 0x7);
        break;
    case RSP_VECTOR_VMOV:
        std::strcpy(m_Name, "VMOV");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d[%d]%s", m_Instruction.vd, m_Instruction.de & 0x7,
                      m_Instruction.rt, m_Instruction.de & 0x7, elemStr);
        break;
    case RSP_VECTOR_VRSQ:
        std::strcpy(m_Name, "VRSQ");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d[%d]", m_Instruction.vd, m_Instruction.rd & 0x7,
                      m_Instruction.vt, m_Instruction.e & 0x7);
        break;
    case RSP_VECTOR_VRSQL:
        std::strcpy(m_Name, "VRSQL");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d[%d]", m_Instruction.vd, m_Instruction.rd & 0x7,
                      m_Instruction.vt, m_Instruction.e & 0x7);
        break;
    case RSP_VECTOR_VRSQH:
        std::strcpy(m_Name, "VRSQH");
        std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], $v%d%s[%d]", m_Instruction.vd, m_Instruction.rd & 0x7,
                      m_Instruction.vt, elemStr, m_Instruction.rd & 0x7);
        break;
    case RSP_VECTOR_VNOP:
        std::strcpy(m_Name, "VNOP");
        m_Param[0] = '\0';
        break;

    // --- Reserved pack/extract/insert ---
    case RSP_VECTOR_VEXTT:
        std::strcpy(m_Name, "Reserved (VEXTT)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VEXTQ:
        std::strcpy(m_Name, "Reserved (VEXTQ)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VEXTN:
        std::strcpy(m_Name, "Reserved (VEXTN)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_V073:
        std::strcpy(m_Name, "Reserved (V073)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VINST:
        std::strcpy(m_Name, "Reserved (VINST)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VINSQ:
        std::strcpy(m_Name, "Reserved (VINSQ)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VINSN:
        std::strcpy(m_Name, "Reserved (VINSN)");
        m_Param[0] = '\0';
        break;
    case RSP_VECTOR_VNULL:
        std::strcpy(m_Name, "VNULL");
        m_Param[0] = '\0';
        break;

    default:
        std::strcpy(m_Name, "UNKNOWN");
        std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
        break;
    }
}

// --- Vector Load/Store COP2 disassembly ---
void RSPInstruction::DecodeLSC2Name(char loadStoreIdent) const {
    // Offset shift depends on element size
    static constexpr int shiftTable[] = {0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4};

    const uint32_t rd = m_Instruction.rd;
    if (rd > RSP_LSC2_TV) {
        std::strcpy(m_Name, "UNKNOWN");
        std::snprintf(m_Param, sizeof(m_Param), "0x%08X", m_Instruction.Value);
        return;
    }

    const int shift = shiftTable[rd];
    const int scaledOffset = std::abs(m_Instruction.voffset << shift);
    const char* sign = (m_Instruction.voffset < 0) ? "-" : "";

    static constexpr const char* suffixes[] = {"BV", "SV", "LV", "DV", "QV", "RV", "PV", "UV", "HV", "FV", "WV", "TV"};

    std::snprintf(m_Name, sizeof(m_Name), "%c%s", loadStoreIdent, suffixes[rd]);
    std::snprintf(m_Param, sizeof(m_Param), "$v%d[%d], %s0x%03X(%s)", m_Instruction.rt, m_Instruction.del, sign,
                  scaledOffset, GPR_Name(m_Instruction.base));
}

// --- Element specifier string (for vector disassembly) ---
const char* RSPInstruction::ElementSpecifier(uint32_t element) {
    static constexpr const char* table[] = {
        "",     "",     " [0q]", " [1q]", " [0h]", " [1h]", " [2h]", " [3h]",
        " [0]", " [1]", " [2]",  " [3]",  " [4]",  " [5]",  " [6]",  " [7]",
    };

    return (element < 16) ? table[element] : "Unknown Element";
}

} // namespace Nova64::RSP
