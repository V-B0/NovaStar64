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

// --- Classification Queries ---

// --- Lazy-Analyzed Accessors ---

// --- Disassembly Accessors ---

const char* RSPInstruction::Name() const {
    if (m_Name[0] == '\0') DecodeName();
    return m_Name;
}

// --- Instruction Analysis (full opcode classification) ---

// --- COP2 scalar sub-analysis (MF/CF/MT/CT) ---

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
