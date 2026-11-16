/**
 * @file R4300Instruction.cpp
 * @brief MIPS R4300 instruction analysis and decoding implementation
 */

#include "R4300Instruction.hpp"
#include "R4300Iops.hpp"
#include "Register.hpp"
#include <cassert>
#include <sstream>
#include <iomanip>
#include <algorithm>

#define OPCODE(x)     ((x >> 26) & 0x3F)
#define RS(x)         ((x >> 21) & 0x1F)
#define RT(x)         ((x >> 16) & 0x1F)
#define RD(x)         ((x >> 11) & 0x1F)
#define SA(x)         ((x >> 6) & 0x1F)
#define FUNCT(x)      (x & 0x3F)
#define IMM16(x)      (x & 0xFFFF)
#define TARGET(x)     (x & 0x3FFFFFF)
#define COP0_SEL(x)   ((x >> 0) & 0x7)
#define COP1_FMT(x)   ((x >> 21) & 0x1F)
#define COP1_FT(x)    ((x >> 16) & 0x1F)
#define COP1_FS(x)    ((x >> 11) & 0x1F)
#define COP1_FD(x)    ((x >> 6) & 0x1F)
#define COP1_FUNCT(x) (x & 0x3F)

R4300iInstruction::R4300iInstruction(uint64_t address, uint32_t instruction) :
    m_Address(address), m_Instruction(instruction) {}

static std::string FormatGPR(uint32_t reg) {
    static const char* GPR_NAMES[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                        "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                        "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

    if (reg < 32) {
        return std::string("$") + GPR_NAMES[reg];
    }
    return std::string("$") + std::to_string(reg);
}

static std::string FormatFPR(uint32_t reg) { return std::string("$f") + std::to_string(reg); }

static int32_t SignExtend16(uint16_t imm) { return static_cast<int16_t>(imm); }

static uint32_t GetJumpTarget(uint32_t instruction, uint64_t currentAddr) {
    uint32_t target = TARGET(instruction);
    return (static_cast<uint32_t>(currentAddr) & 0xF0000000) | (target << 2);
}

static uint32_t GetBranchTarget(uint32_t instruction, uint64_t currentAddr) {
    int16_t offset = static_cast<int16_t>(IMM16(instruction));
    return static_cast<uint32_t>(currentAddr) + (offset << 2) + 4;
}

void R4300iInstruction::DecodeName() const {
    uint8_t opcode = OPCODE(m_Instruction);

    switch (opcode) {
    case R4300_SPECIAL: {
        uint8_t funct = FUNCT(m_Instruction);
        switch (funct) {
        case R4300_S_SLL:     m_Name = (m_Instruction == 0) ? "NOP" : "SLL"; break;
        case R4300_S_SRL:     m_Name = "SRL"; break;
        case R4300_S_SRA:     m_Name = "SRA"; break;
        case R4300_S_SLLV:    m_Name = "SLLV"; break;
        case R4300_S_SRLV:    m_Name = "SRLV"; break;
        case R4300_S_SRAV:    m_Name = "SRAV"; break;
        case R4300_S_JR:      m_Name = "JR"; break;
        case R4300_S_JALR:    m_Name = "JALR"; break;
        case R4300_S_SYSCALL: m_Name = "SYSCALL"; break;
        case R4300_S_BREAK:   m_Name = "BREAK"; break;
        case R4300_S_SYNC:    m_Name = "SYNC"; break;
        case R4300_S_MFHI:    m_Name = "MFHI"; break;
        case R4300_S_MTHI:    m_Name = "MTHI"; break;
        case R4300_S_MFLO:    m_Name = "MFLO"; break;
        case R4300_S_MTLO:    m_Name = "MTLO"; break;
        case R4300_S_ADD:     m_Name = "ADD"; break;
        case R4300_S_ADDU:    m_Name = "ADDU"; break;
        case R4300_S_SUB:     m_Name = "SUB"; break;
        case R4300_S_SUBU:    m_Name = "SUBU"; break;
        case R4300_S_AND:     m_Name = "AND"; break;
        case R4300_S_OR:      m_Name = "OR"; break;
        case R4300_S_XOR:     m_Name = "XOR"; break;
        case R4300_S_NOR:     m_Name = "NOR"; break;
        case R4300_S_SLT:     m_Name = "SLT"; break;
        case R4300_S_SLTU:    m_Name = "SLTU"; break;
        case R4300_S_DADD:    m_Name = "DADD"; break;
        case R4300_S_DADDU:   m_Name = "DADDU"; break;
        case R4300_S_DSUB:    m_Name = "DSUB"; break;
        case R4300_S_DSUBU:   m_Name = "DSUBU"; break;
        case R4300_S_MULT:    m_Name = "MULT"; break;
        case R4300_S_MULTU:   m_Name = "MULTU"; break;
        case R4300_S_DIV:     m_Name = "DIV"; break;
        case R4300_S_DIVU:    m_Name = "DIVU"; break;
        default:              m_Name = "UNKNOWN"; break;
        }
        break;
    }
    case R4300_REGIMM: {
        uint8_t rt = RT(m_Instruction);
        switch (rt) {
        case R4300_R_BLTZ:   m_Name = "BLTZ"; break;
        case R4300_R_BGEZ:   m_Name = "BGEZ"; break;
        case R4300_R_BLTZAL: m_Name = "BLTZAL"; break;
        case R4300_R_BGEZAL: m_Name = "BGEZAL"; break;
        default:             m_Name = "UNKNOWN"; break;
        }
        break;
    }
    case R4300_J:     m_Name = "J"; break;
    case R4300_JAL:   m_Name = "JAL"; break;
    case R4300_BEQ:   m_Name = "BEQ"; break;
    case R4300_BNE:   m_Name = "BNE"; break;
    case R4300_BLEZ:  m_Name = "BLEZ"; break;
    case R4300_BGTZ:  m_Name = "BGTZ"; break;
    case R4300_ADDI:  m_Name = "ADDI"; break;
    case R4300_ADDIU: m_Name = "ADDIU"; break;
    case R4300_SLTI:  m_Name = "SLTI"; break;
    case R4300_SLTIU: m_Name = "SLTIU"; break;
    case R4300_ANDI:  m_Name = "ANDI"; break;
    case R4300_ORI:   m_Name = "ORI"; break;
    case R4300_XORI:  m_Name = "XORI"; break;
    case R4300_LUI:   m_Name = "LUI"; break;
    case R4300_LB:    m_Name = "LB"; break;
    case R4300_LH:    m_Name = "LH"; break;
    case R4300_LW:    m_Name = "LW"; break;
    case R4300_LBU:   m_Name = "LBU"; break;
    case R4300_LHU:   m_Name = "LHU"; break;
    case R4300_SB:    m_Name = "SB"; break;
    case R4300_SH:    m_Name = "SH"; break;
    case R4300_SW:    m_Name = "SW"; break;
    case R4300_LWC1:  m_Name = "LWC1"; break;
    case R4300_SWC1:  m_Name = "SWC1"; break;
    case R4300_LDC1:  m_Name = "LDC1"; break;
    case R4300_SDC1:  m_Name = "SDC1"; break;
    case R4300_LD:    m_Name = "LD"; break;
    case R4300_SD:    m_Name = "SD"; break;
    default:          m_Name = "UNKNOWN"; break;
    }
}

void R4300iInstruction::DecodeParam() const {
    uint8_t opcode = OPCODE(m_Instruction);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    switch (opcode) {
    case R4300_SPECIAL: {
        uint8_t funct = FUNCT(m_Instruction);
        uint8_t rs = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);
        uint8_t rd = RD(m_Instruction);
        uint8_t sa = SA(m_Instruction);

        switch (funct) {
        case R4300_S_SLL:
            if (m_Instruction != 0) {
                oss << FormatGPR(rd) << ", " << FormatGPR(rt) << ", " << static_cast<int>(sa);
            }
            break;
        case R4300_S_SRL:
        case R4300_S_SRA:     oss << FormatGPR(rd) << ", " << FormatGPR(rt) << ", " << static_cast<int>(sa); break;
        case R4300_S_SLLV:
        case R4300_S_SRLV:
        case R4300_S_SRAV:    oss << FormatGPR(rd) << ", " << FormatGPR(rt) << ", " << FormatGPR(rs); break;
        case R4300_S_JR:      oss << FormatGPR(rs); break;
        case R4300_S_JALR:    oss << FormatGPR(rd) << ", " << FormatGPR(rs); break;
        case R4300_S_SYSCALL:
        case R4300_S_BREAK:   oss << "0x" << std::setw(5) << (m_Instruction >> 6 & 0xFFFFF); break;
        case R4300_S_ADD:
        case R4300_S_ADDU:
        case R4300_S_SUB:
        case R4300_S_SUBU:
        case R4300_S_AND:
        case R4300_S_OR:
        case R4300_S_XOR:
        case R4300_S_NOR:
        case R4300_S_SLT:
        case R4300_S_SLTU:    oss << FormatGPR(rd) << ", " << FormatGPR(rs) << ", " << FormatGPR(rt); break;
        case R4300_S_MFHI:
        case R4300_S_MFLO:    oss << FormatGPR(rd); break;
        case R4300_S_MTHI:
        case R4300_S_MTLO:    oss << FormatGPR(rs); break;
        case R4300_S_MULT:
        case R4300_S_MULTU:
        case R4300_S_DIV:
        case R4300_S_DIVU:    oss << FormatGPR(rs) << ", " << FormatGPR(rt); break;
        default:              oss << "0x" << std::setw(8) << m_Instruction; break;
        }
        break;
    }
    case R4300_REGIMM: {
        uint8_t rs = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);
        [[maybe_unused]] int32_t offset = SignExtend16(IMM16(m_Instruction));
        uint32_t target = GetBranchTarget(m_Instruction, m_Address);

        switch (rt) {
        case R4300_R_BLTZ:
        case R4300_R_BGEZ:
        case R4300_R_BLTZAL:
        case R4300_R_BGEZAL: oss << FormatGPR(rs) << ", 0x" << std::setw(8) << target; break;
        default:             oss << "0x" << std::setw(8) << m_Instruction; break;
        }
        break;
    }
    case R4300_J:
    case R4300_JAL: {
        uint32_t target = GetJumpTarget(m_Instruction, m_Address);
        oss << "0x" << std::setw(8) << target;
        break;
    }
    case R4300_BEQ:
    case R4300_BNE: {
        uint8_t rs = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);
        [[maybe_unused]] int32_t offset = SignExtend16(IMM16(m_Instruction));
        uint32_t target = GetBranchTarget(m_Instruction, m_Address);

        if (rs == 0 && rt == 0) {
            oss << "0x" << std::setw(8) << target;
        } else if (rs == 0 || rt == 0) {
            oss << FormatGPR(rs == 0 ? rt : rs) << ", 0x" << std::setw(8) << target;
        } else {
            oss << FormatGPR(rs) << ", " << FormatGPR(rt) << ", 0x" << std::setw(8) << target;
        }
        break;
    }
    case R4300_BLEZ:
    case R4300_BGTZ: {
        uint8_t rs = RS(m_Instruction);
        uint32_t target = GetBranchTarget(m_Instruction, m_Address);
        oss << FormatGPR(rs) << ", 0x" << std::setw(8) << target;
        break;
    }
    case R4300_ADDI:
    case R4300_ADDIU:
    case R4300_SLTI:
    case R4300_SLTIU:
    case R4300_ANDI:
    case R4300_ORI:
    case R4300_XORI:  {
        uint8_t rs = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);
        uint16_t imm = IMM16(m_Instruction);
        oss << FormatGPR(rt) << ", " << FormatGPR(rs) << ", 0x" << std::setw(4) << imm;
        break;
    }
    case R4300_LUI: {
        uint8_t rt = RT(m_Instruction);
        uint16_t imm = IMM16(m_Instruction);
        oss << FormatGPR(rt) << ", 0x" << std::setw(4) << imm;
        break;
    }
    case R4300_LB:
    case R4300_LH:
    case R4300_LW:
    case R4300_LBU:
    case R4300_LHU:
    case R4300_SB:
    case R4300_SH:
    case R4300_SW:
    case R4300_LWC1:
    case R4300_SWC1:
    case R4300_LDC1:
    case R4300_SDC1:
    case R4300_LD:
    case R4300_SD:   {
        uint8_t base = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);
        int16_t offset = static_cast<int16_t>(IMM16(m_Instruction));

        // For floating point loads/stores
        if (opcode == R4300_LWC1 || opcode == R4300_SWC1 || opcode == R4300_LDC1 || opcode == R4300_SDC1) {
            oss << FormatFPR(rt) << ", " << offset << "(" << FormatGPR(base) << ")";
        } else {
            oss << FormatGPR(rt) << ", " << offset << "(" << FormatGPR(base) << ")";
        }
        break;
    }
    default: oss << "0x" << std::setw(8) << m_Instruction; break;
    }

    m_Param = oss.str();
}

R4300iInstruction::RegisterInfo R4300iInstruction::GetRegisterInfo() const {
    RegisterInfo info = {};
    uint8_t opcode = OPCODE(m_Instruction);

    switch (opcode) {
    case R4300_SPECIAL: {
        uint8_t funct = FUNCT(m_Instruction);
        uint8_t rs = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);
        uint8_t rd = RD(m_Instruction);

        switch (funct) {
        case R4300_S_SLL:
        case R4300_S_SRL:
        case R4300_S_SRA:
        case R4300_S_DSLL:
        case R4300_S_DSRL:
        case R4300_S_DSRA:
            info.read1 = rt;
            info.write = rd;
            break;

        case R4300_S_SLLV:
        case R4300_S_SRLV:
        case R4300_S_SRAV:
        case R4300_S_DSLLV:
        case R4300_S_DSRLV:
        case R4300_S_DSRAV:
            info.read1 = rs;
            info.read2 = rt;
            info.write = rd;
            break;

        case R4300_S_ADD:
        case R4300_S_ADDU:
        case R4300_S_SUB:
        case R4300_S_SUBU:
        case R4300_S_AND:
        case R4300_S_OR:
        case R4300_S_XOR:
        case R4300_S_NOR:
        case R4300_S_SLT:
        case R4300_S_SLTU:
        case R4300_S_DADD:
        case R4300_S_DADDU:
        case R4300_S_DSUB:
        case R4300_S_DSUBU:
            info.read1 = rs;
            info.read2 = rt;
            info.write = rd;
            break;

        case R4300_S_JR: info.read1 = rs; break;

        case R4300_S_JALR:
            info.read1 = rs;
            info.write = rd;
            break;

        case R4300_S_MFHI:
            info.readsHI = true;
            info.write = rd;
            break;

        case R4300_S_MTHI:
            info.read1 = rs;
            info.writesHI = true;
            break;

        case R4300_S_MFLO:
            info.readsLO = true;
            info.write = rd;
            break;

        case R4300_S_MTLO:
            info.read1 = rs;
            info.writesLO = true;
            break;

        case R4300_S_MULT:
        case R4300_S_MULTU:
        case R4300_S_DMULT:
        case R4300_S_DMULTU:
            info.read1 = rs;
            info.read2 = rt;
            info.writesHI = true;
            info.writesLO = true;
            break;

        case R4300_S_DIV:
        case R4300_S_DIVU:
        case R4300_S_DDIV:
        case R4300_S_DDIVU:
            info.read1 = rs;
            info.read2 = rt;
            info.writesHI = true;
            info.writesLO = true;
            break;
        }
        break;
    }

    case R4300_REGIMM: {
        uint8_t rs = RS(m_Instruction);
        uint8_t rt = RT(m_Instruction);

        switch (rt) {
        case R4300_R_BLTZ:
        case R4300_R_BGEZ:
        case R4300_R_BLTZL:
        case R4300_R_BGEZL: info.read1 = rs; break;

        case R4300_R_BLTZAL:
        case R4300_R_BGEZAL:
        case R4300_R_BLTZALL:
        case R4300_R_BGEZALL:
            info.read1 = rs;
            info.write = 31;
            break;

        case R4300_R_TGEI:
        case R4300_R_TGEIU:
        case R4300_R_TLTI:
        case R4300_R_TLTIU:
        case R4300_R_TEQI:
        case R4300_R_TNEI:  info.read1 = rs; break;
        }
        break;
    }

    case R4300_J:
        // No register reads/writes
        break;

    case R4300_JAL:
        info.write = 31; // $ra
        break;

    case R4300_BEQ:
    case R4300_BNE:
    case R4300_BEQL:
    case R4300_BNEL:
        info.read1 = RS(m_Instruction);
        info.read2 = RT(m_Instruction);
        break;

    case R4300_BLEZ:
    case R4300_BGTZ:
    case R4300_BLEZL:
    case R4300_BGTZL: info.read1 = RS(m_Instruction); break;

    case R4300_ADDI:
    case R4300_ADDIU:
    case R4300_SLTI:
    case R4300_SLTIU:
    case R4300_ANDI:
    case R4300_ORI:
    case R4300_XORI:
    case R4300_DADDI:
    case R4300_DADDIU:
        info.read1 = RS(m_Instruction);
        info.write = RT(m_Instruction);
        break;

    case R4300_LUI: info.write = RT(m_Instruction); break;

    case R4300_LB:
    case R4300_LH:
    case R4300_LW:
    case R4300_LBU:
    case R4300_LHU:
    case R4300_LWU:
    case R4300_LD:
    case R4300_LWL:
    case R4300_LWR:
    case R4300_LDL:
    case R4300_LDR:
        info.read1 = RS(m_Instruction);
        info.write = RT(m_Instruction);
        break;

    case R4300_SB:
    case R4300_SH:
    case R4300_SW:
    case R4300_SD:
    case R4300_SWL:
    case R4300_SWR:
    case R4300_SDL:
    case R4300_SDR:
        info.read1 = RS(m_Instruction);
        info.read2 = RT(m_Instruction);
        break;

    case R4300_LWC1:
    case R4300_LDC1:
        info.read1 = RS(m_Instruction);
        // Note: Floating point register writes are tracked separately
        info.write = 0xFFFFFFFF; // Special value indicating FP register
        break;

    case R4300_SWC1:
    case R4300_SDC1:
        info.read1 = RS(m_Instruction);
        // Note: Floating point register reads are tracked separately
        info.read2 = 0xFFFFFFFF; // Special value indicating FP register
        break;

    case R4300_COP0: {
        uint8_t rs = (m_Instruction >> 21) & 0x1F;
        uint8_t rt = RT(m_Instruction);
        [[maybe_unused]] uint8_t rd = RD(m_Instruction);

        // MFC0, DMFC0
        if (rs == 0x00 || rs == 0x01) {
            info.write = rt;
        }
        // MTC0, DMTC0
        else if (rs == 0x04 || rs == 0x05) {
            info.read1 = rt;
        }
        break;
    }

    case R4300_COP1: {
        uint8_t fmt = COP1_FMT(m_Instruction);
        uint8_t ft = COP1_FT(m_Instruction);
        [[maybe_unused]] uint8_t fs = COP1_FS(m_Instruction);
        [[maybe_unused]] uint8_t fd = COP1_FD(m_Instruction);
        uint8_t funct = COP1_FUNCT(m_Instruction);

        // MFC1, DMFC1, CFC1
        if (fmt == 0x00 || fmt == 0x01 || fmt == 0x02) {
            info.write = ft;
        }
        // MTC1, DMTC1, CTC1
        else if (fmt == 0x04 || fmt == 0x05 || fmt == 0x06) {
            info.read1 = ft;
        }
        // Arithmetic operations
        else if (fmt == 0x10 || fmt == 0x11) {    // S or D format
            if (funct >= 0x00 && funct <= 0x03) { // ADD, SUB, MUL, DIV
                // Two source operands
            } else if (funct == 0x05 || funct == 0x07 ||   // ABS, NEG
                       (funct >= 0x08 && funct <= 0x0F) || // Convert operations
                       (funct >= 0x20 && funct <= 0x25)) { // More converts
                // One source operand
            }
        }
        break;
    }

    case R4300_LL:
    case R4300_SC:
        info.read1 = RS(m_Instruction);
        info.write = RT(m_Instruction);
        break;

    case R4300_CACHE: info.read1 = RS(m_Instruction); break;
    }

    return info;
}
