/**
 * @file RSPInterpreterOps.cpp
 * @brief RSP Interpreter Operations - Complete opcode implementations.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include <chrono>
#include <cstdlib>
#include "RSPInterpreterOps.hpp"
#include "RSPCpu.hpp"
#include "RSPRegisters.hpp"
#include "RSPRegisterHandler.hpp"
#include "RspClamp.hpp"
#include "RspLog.hpp"
#include "RspSystem.hpp"
#include "../RSPDebugger.hpp"
#include "../Settings/RspSettings.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace Nova64::RSP {

// NOVA64_RSP_TIME=1 also splits out the time spent drawing the RDP command list the RSP hands over.
uint64_t g_rspRdpNanos = 0;
const bool g_rspTimeRdp = std::getenv("NOVA64_RSP_TIME") != nullptr;

// --- Count leading zeros (portable C++20) ---
static inline uint32_t clz32(uint32_t val) {
#if __cpp_lib_bitops >= 201907L
    return val ? static_cast<uint32_t>(std::countl_zero(val)) : 32;
#elif defined(_MSC_VER)
    unsigned long idx;
    return _BitScanReverse(&idx, val) ? (31u - idx) : 32u;
#elif defined(__GNUC__) || defined(__clang__)
    return val ? static_cast<uint32_t>(__builtin_clz(val)) : 32;
#else
    uint32_t cnt = 0;
    if ((val & 0xFFFF0000u) == 0) {
        cnt += 16;
        val <<= 16;
    }
    if ((val & 0xFF000000u) == 0) {
        cnt += 8;
        val <<= 8;
    }
    if ((val & 0xF0000000u) == 0) {
        cnt += 4;
        val <<= 4;
    }
    if ((val & 0xC0000000u) == 0) {
        cnt += 2;
        val <<= 2;
    }
    if ((val & 0x80000000u) == 0) {
        cnt += 1;
        val <<= 1;
    }
    if ((val & 0x80000000u) == 0) {
        cnt += 1;
    }
    return cnt;
#endif
}

// --- Constructor / Destructor ---

RSPOp::RSPOp(CRSPSystem& system) :
    m_System(system), m_RSPRegisterHandler(system.m_RSPRegisterHandler), m_OpCode(system.m_OpCode), m_Reg(system.m_Reg),
    m_NextInstruction(system.m_NextInstruction), m_JumpTo(system.m_JumpTo), m_MI_INTR_REG(system.m_MI_INTR_REG),
    m_SP_PC_REG(system.m_SP_PC_REG), m_SP_STATUS_REG(system.m_SP_STATUS_REG),
    m_SP_DMA_FULL_REG(system.m_SP_DMA_FULL_REG), m_SP_DMA_BUSY_REG(system.m_SP_DMA_BUSY_REG),
    m_SP_SEMAPHORE_REG(system.m_SP_SEMAPHORE_REG), m_DPC_START_REG(system.m_DPC_START_REG),
    m_DPC_END_REG(system.m_DPC_END_REG), m_DPC_CURRENT_REG(system.m_DPC_CURRENT_REG),
    m_DPC_STATUS_REG(system.m_DPC_STATUS_REG), m_DPC_CLOCK_REG(system.m_DPC_CLOCK_REG), m_DMEM(system.m_DMEM),
    m_GPR(system.m_Reg.m_GPR), m_ACCUM(system.m_Reg.m_ACCUM), m_Vect(system.m_Reg.m_Vect), VCOL(system.m_Reg.m_VCOL),
    VCOH(system.m_Reg.m_VCOH), VCCL(system.m_Reg.m_VCCL), VCCH(system.m_Reg.m_VCCH), VCE(system.m_Reg.m_VCE),
    CheckInterrupts(system.CheckInterrupts), ProcessRdpList(system.ProcessRdpList) {
    BuildInterpreter();
}

RSPOp::~RSPOp() = default;

// --- Dispatch Table Initialization ---

void RSPOp::BuildInterpreter() {
    // --- Primary Opcode Table [64] ---
    std::fill_n(Jump_Opcode, 64, &RSPOp::UnknownOpcode);
    Jump_Opcode[RSP_SPECIAL] = &RSPOp::SPECIAL;
    Jump_Opcode[RSP_REGIMM] = &RSPOp::REGIMM;
    Jump_Opcode[RSP_J] = &RSPOp::J;
    Jump_Opcode[RSP_JAL] = &RSPOp::JAL;
    Jump_Opcode[RSP_BEQ] = &RSPOp::BEQ;
    Jump_Opcode[RSP_BNE] = &RSPOp::BNE;
    Jump_Opcode[RSP_BLEZ] = &RSPOp::BLEZ;
    Jump_Opcode[RSP_BGTZ] = &RSPOp::BGTZ;
    Jump_Opcode[RSP_ADDI] = &RSPOp::ADDI;
    Jump_Opcode[RSP_ADDIU] = &RSPOp::ADDIU;
    Jump_Opcode[RSP_SLTI] = &RSPOp::SLTI;
    Jump_Opcode[RSP_SLTIU] = &RSPOp::SLTIU;
    Jump_Opcode[RSP_ANDI] = &RSPOp::ANDI;
    Jump_Opcode[RSP_ORI] = &RSPOp::ORI;
    Jump_Opcode[RSP_XORI] = &RSPOp::XORI;
    Jump_Opcode[RSP_LUI] = &RSPOp::LUI;
    Jump_Opcode[RSP_CP0] = &RSPOp::COP0;
    Jump_Opcode[RSP_CP2] = &RSPOp::COP2;
    Jump_Opcode[RSP_LB] = &RSPOp::LB;
    Jump_Opcode[RSP_LH] = &RSPOp::LH;
    Jump_Opcode[RSP_LW] = &RSPOp::LW;
    Jump_Opcode[RSP_LBU] = &RSPOp::LBU;
    Jump_Opcode[RSP_LHU] = &RSPOp::LHU;
    Jump_Opcode[RSP_LWU] = &RSPOp::LWU;
    Jump_Opcode[RSP_SB] = &RSPOp::SB;
    Jump_Opcode[RSP_SH] = &RSPOp::SH;
    Jump_Opcode[RSP_SW] = &RSPOp::SW;
    Jump_Opcode[RSP_LC2] = &RSPOp::LC2;
    Jump_Opcode[RSP_SC2] = &RSPOp::SC2;

    // --- SPECIAL Function Table [64] ---
    std::fill_n(Jump_Special, 64, &RSPOp::UnknownOpcode);
    Jump_Special[RSP_SPECIAL_SLL] = &RSPOp::Special_SLL;
    Jump_Special[RSP_SPECIAL_SRL] = &RSPOp::Special_SRL;
    Jump_Special[RSP_SPECIAL_SRA] = &RSPOp::Special_SRA;
    Jump_Special[RSP_SPECIAL_SLLV] = &RSPOp::Special_SLLV;
    Jump_Special[RSP_SPECIAL_SRLV] = &RSPOp::Special_SRLV;
    Jump_Special[RSP_SPECIAL_SRAV] = &RSPOp::Special_SRAV;
    Jump_Special[RSP_SPECIAL_JR] = &RSPOp::Special_JR;
    Jump_Special[RSP_SPECIAL_JALR] = &RSPOp::Special_JALR;
    Jump_Special[RSP_SPECIAL_BREAK] = &RSPOp::Special_BREAK;
    Jump_Special[RSP_SPECIAL_ADD] = &RSPOp::Special_ADD;
    Jump_Special[RSP_SPECIAL_ADDU] = &RSPOp::Special_ADDU;
    Jump_Special[RSP_SPECIAL_SUB] = &RSPOp::Special_SUB;
    Jump_Special[RSP_SPECIAL_SUBU] = &RSPOp::Special_SUBU;
    Jump_Special[RSP_SPECIAL_AND] = &RSPOp::Special_AND;
    Jump_Special[RSP_SPECIAL_OR] = &RSPOp::Special_OR;
    Jump_Special[RSP_SPECIAL_XOR] = &RSPOp::Special_XOR;
    Jump_Special[RSP_SPECIAL_NOR] = &RSPOp::Special_NOR;
    Jump_Special[RSP_SPECIAL_SLT] = &RSPOp::Special_SLT;
    Jump_Special[RSP_SPECIAL_SLTU] = &RSPOp::Special_SLTU;

    // --- REGIMM Table [32] ---
    std::fill_n(Jump_RegImm, 32, &RSPOp::UnknownOpcode);
    Jump_RegImm[RSP_REGIMM_BLTZ] = &RSPOp::BLTZ;
    Jump_RegImm[RSP_REGIMM_BGEZ] = &RSPOp::BGEZ;
    Jump_RegImm[RSP_REGIMM_BLTZAL] = &RSPOp::BLTZAL;
    Jump_RegImm[RSP_REGIMM_BGEZAL] = &RSPOp::BGEZAL;

    // --- COP0 Table [32] ---
    std::fill_n(Jump_Cop0, 32, &RSPOp::UnknownOpcode);
    Jump_Cop0[RSP_COP0_MF] = &RSPOp::Cop0_MF;
    Jump_Cop0[RSP_COP0_MT] = &RSPOp::Cop0_MT;

    // --- COP2 Table [32] ---
    // Indices 0-15: scalar/control moves; 16-31: vector compute dispatch
    std::fill_n(Jump_Cop2, 32, &RSPOp::UnknownOpcode);
    Jump_Cop2[RSP_COP2_MF] = &RSPOp::Cop2_MF;
    Jump_Cop2[RSP_COP2_CF] = &RSPOp::Cop2_CF;
    Jump_Cop2[RSP_COP2_MT] = &RSPOp::Cop2_MT;
    Jump_Cop2[RSP_COP2_CT] = &RSPOp::Cop2_CT;
    for (int i = 16; i < 32; i++) Jump_Cop2[i] = &RSPOp::Cop2_VECTOR;

    // --- Vector Compute Table [64] ---
    std::fill_n(Jump_Vector, 64, &RSPOp::Vector_Reserved);
    Jump_Vector[RSP_VECTOR_VMULF] = &RSPOp::Vector_VMULF;
    Jump_Vector[RSP_VECTOR_VMULU] = &RSPOp::Vector_VMULU;
    Jump_Vector[RSP_VECTOR_VRNDP] = &RSPOp::Vector_VRNDP;
    Jump_Vector[RSP_VECTOR_VMULQ] = &RSPOp::Vector_VMULQ;
    Jump_Vector[RSP_VECTOR_VMUDL] = &RSPOp::Vector_VMUDL;
    Jump_Vector[RSP_VECTOR_VMUDM] = &RSPOp::Vector_VMUDM;
    Jump_Vector[RSP_VECTOR_VMUDN] = &RSPOp::Vector_VMUDN;
    Jump_Vector[RSP_VECTOR_VMUDH] = &RSPOp::Vector_VMUDH;
    Jump_Vector[RSP_VECTOR_VMACF] = &RSPOp::Vector_VMACF;
    Jump_Vector[RSP_VECTOR_VMACU] = &RSPOp::Vector_VMACU;
    Jump_Vector[RSP_VECTOR_VRNDN] = &RSPOp::Vector_VRNDN;
    Jump_Vector[RSP_VECTOR_VMACQ] = &RSPOp::Vector_VMACQ;
    Jump_Vector[RSP_VECTOR_VMADL] = &RSPOp::Vector_VMADL;
    Jump_Vector[RSP_VECTOR_VMADM] = &RSPOp::Vector_VMADM;
    Jump_Vector[RSP_VECTOR_VMADN] = &RSPOp::Vector_VMADN;
    Jump_Vector[RSP_VECTOR_VMADH] = &RSPOp::Vector_VMADH;
    Jump_Vector[RSP_VECTOR_VADD] = &RSPOp::Vector_VADD;
    Jump_Vector[RSP_VECTOR_VSUB] = &RSPOp::Vector_VSUB;
    Jump_Vector[RSP_VECTOR_VABS] = &RSPOp::Vector_VABS;
    Jump_Vector[RSP_VECTOR_VADDC] = &RSPOp::Vector_VADDC;
    Jump_Vector[RSP_VECTOR_VSUBC] = &RSPOp::Vector_VSUBC;
    Jump_Vector[RSP_VECTOR_VSAW] = &RSPOp::Vector_VSAW;
    Jump_Vector[RSP_VECTOR_VLT] = &RSPOp::Vector_VLT;
    Jump_Vector[RSP_VECTOR_VEQ] = &RSPOp::Vector_VEQ;
    Jump_Vector[RSP_VECTOR_VNE] = &RSPOp::Vector_VNE;
    Jump_Vector[RSP_VECTOR_VGE] = &RSPOp::Vector_VGE;
    Jump_Vector[RSP_VECTOR_VCL] = &RSPOp::Vector_VCL;
    Jump_Vector[RSP_VECTOR_VCH] = &RSPOp::Vector_VCH;
    Jump_Vector[RSP_VECTOR_VCR] = &RSPOp::Vector_VCR;
    Jump_Vector[RSP_VECTOR_VMRG] = &RSPOp::Vector_VMRG;
    Jump_Vector[RSP_VECTOR_VAND] = &RSPOp::Vector_VAND;
    Jump_Vector[RSP_VECTOR_VNAND] = &RSPOp::Vector_VNAND;
    Jump_Vector[RSP_VECTOR_VOR] = &RSPOp::Vector_VOR;
    Jump_Vector[RSP_VECTOR_VNOR] = &RSPOp::Vector_VNOR;
    Jump_Vector[RSP_VECTOR_VXOR] = &RSPOp::Vector_VXOR;
    Jump_Vector[RSP_VECTOR_VNXOR] = &RSPOp::Vector_VNXOR;
    Jump_Vector[RSP_VECTOR_VRCP] = &RSPOp::Vector_VRCP;
    Jump_Vector[RSP_VECTOR_VRCPL] = &RSPOp::Vector_VRCPL;
    Jump_Vector[RSP_VECTOR_VRCPH] = &RSPOp::Vector_VRCPH;
    Jump_Vector[RSP_VECTOR_VMOV] = &RSPOp::Vector_VMOV;
    Jump_Vector[RSP_VECTOR_VRSQ] = &RSPOp::Vector_VRSQ;
    Jump_Vector[RSP_VECTOR_VRSQL] = &RSPOp::Vector_VRSQL;
    Jump_Vector[RSP_VECTOR_VRSQH] = &RSPOp::Vector_VRSQH;
    Jump_Vector[RSP_VECTOR_VNOP] = &RSPOp::Vector_VNOOP;
    // slot 63 = VNULL → noop
    Jump_Vector[RSP_VECTOR_VNULL] = &RSPOp::Vector_VNOOP;

    // --- LC2 Vector Load Table [32] ---
    std::fill_n(Jump_Lc2, 32, &RSPOp::UnknownOpcode);
    Jump_Lc2[RSP_LSC2_BV] = &RSPOp::LBV;
    Jump_Lc2[RSP_LSC2_SV] = &RSPOp::LSV;
    Jump_Lc2[RSP_LSC2_LV] = &RSPOp::LLV;
    Jump_Lc2[RSP_LSC2_DV] = &RSPOp::LDV;
    Jump_Lc2[RSP_LSC2_QV] = &RSPOp::LQV;
    Jump_Lc2[RSP_LSC2_RV] = &RSPOp::LRV;
    Jump_Lc2[RSP_LSC2_PV] = &RSPOp::LPV;
    Jump_Lc2[RSP_LSC2_UV] = &RSPOp::LUV;
    Jump_Lc2[RSP_LSC2_HV] = &RSPOp::LHV;
    Jump_Lc2[RSP_LSC2_FV] = &RSPOp::LFV;
    Jump_Lc2[RSP_LSC2_WV] = &RSPOp::LWV;
    Jump_Lc2[RSP_LSC2_TV] = &RSPOp::LTV;

    // --- SC2 Vector Store Table [32] ---
    std::fill_n(Jump_Sc2, 32, &RSPOp::UnknownOpcode);
    Jump_Sc2[RSP_LSC2_BV] = &RSPOp::SBV;
    Jump_Sc2[RSP_LSC2_SV] = &RSPOp::SSV;
    Jump_Sc2[RSP_LSC2_LV] = &RSPOp::SLV;
    Jump_Sc2[RSP_LSC2_DV] = &RSPOp::SDV;
    Jump_Sc2[RSP_LSC2_QV] = &RSPOp::SQV;
    Jump_Sc2[RSP_LSC2_RV] = &RSPOp::SRV;
    Jump_Sc2[RSP_LSC2_PV] = &RSPOp::SPV;
    Jump_Sc2[RSP_LSC2_UV] = &RSPOp::SUV;
    Jump_Sc2[RSP_LSC2_HV] = &RSPOp::SHV;
    Jump_Sc2[RSP_LSC2_FV] = &RSPOp::SFV;
    Jump_Sc2[RSP_LSC2_WV] = &RSPOp::SWV;
    Jump_Sc2[RSP_LSC2_TV] = &RSPOp::STV;
}

// --- Primary Opcode Dispatch ---

void RSPOp::SPECIAL() { (this->*Jump_Special[m_OpCode.funct])(); }
void RSPOp::REGIMM() { (this->*Jump_RegImm[m_OpCode.rt])(); }
void RSPOp::COP0() { (this->*Jump_Cop0[m_OpCode.rs])(); }
void RSPOp::COP2() { (this->*Jump_Cop2[m_OpCode.rs])(); }
void RSPOp::LC2() { (this->*Jump_Lc2[m_OpCode.rd])(); }
void RSPOp::SC2() { (this->*Jump_Sc2[m_OpCode.rd])(); }

// --- Branch & Jump Instructions ---

void RSPOp::J() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = (m_OpCode.target << 2) & 0xFFC;
}

void RSPOp::JAL() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_GPR[31].UW = (*m_SP_PC_REG + 8) & 0xFFC;
    m_JumpTo = (m_OpCode.target << 2) & 0xFFC;
}

void RSPOp::BEQ() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W == m_GPR[m_OpCode.rt].W);
}

void RSPOp::BNE() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W != m_GPR[m_OpCode.rt].W);
}

void RSPOp::BLEZ() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W <= 0);
}

void RSPOp::BGTZ() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W > 0);
}

// --- Immediate Arithmetic & Logic ---

void RSPOp::ADDI() { m_GPR[m_OpCode.rt].W = m_GPR[m_OpCode.rs].W + static_cast<int16_t>(m_OpCode.immediate); }

void RSPOp::ADDIU() {
    m_GPR[m_OpCode.rt].UW = m_GPR[m_OpCode.rs].UW + static_cast<uint32_t>(static_cast<int16_t>(m_OpCode.immediate));
}

void RSPOp::SLTI() { m_GPR[m_OpCode.rt].W = (m_GPR[m_OpCode.rs].W < static_cast<int16_t>(m_OpCode.immediate)) ? 1 : 0; }

void RSPOp::SLTIU() {
    m_GPR[m_OpCode.rt].W =
        (m_GPR[m_OpCode.rs].UW < static_cast<uint32_t>(static_cast<int16_t>(m_OpCode.immediate))) ? 1 : 0;
}

void RSPOp::ANDI() { m_GPR[m_OpCode.rt].W = m_GPR[m_OpCode.rs].W & m_OpCode.immediate; }

void RSPOp::ORI() { m_GPR[m_OpCode.rt].W = m_GPR[m_OpCode.rs].W | m_OpCode.immediate; }

void RSPOp::XORI() { m_GPR[m_OpCode.rt].W = m_GPR[m_OpCode.rs].W ^ m_OpCode.immediate; }

void RSPOp::LUI() { m_GPR[m_OpCode.rt].W = m_OpCode.immediate << 16; }

// --- Scalar Memory Access (DMEM, big-endian) ---

void RSPOp::LB() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    m_GPR[m_OpCode.rt].W = *reinterpret_cast<int8_t*>(m_DMEM + ((addr ^ 3) & 0xFFF));
}

void RSPOp::LH() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    if ((addr & 0x1) != 0) {
        m_GPR[m_OpCode.rt].UHW[0] = static_cast<uint16_t>(*(m_DMEM + (((addr + 0) & 0xFFF) ^ 3))) << 8;
        m_GPR[m_OpCode.rt].UHW[0] += *(m_DMEM + (((addr + 1) & 0xFFF) ^ 3));
    } else {
        m_GPR[m_OpCode.rt].UHW[0] = *reinterpret_cast<uint16_t*>(m_DMEM + ((addr ^ 2) & 0xFFF));
    }
    m_GPR[m_OpCode.rt].W = m_GPR[m_OpCode.rt].HW[0];
}

void RSPOp::LW() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    if ((addr & 0x3) != 0) {
        m_GPR[m_OpCode.rt].UW = static_cast<uint32_t>(*(m_DMEM + (((addr + 0) & 0xFFF) ^ 3))) << 24;
        m_GPR[m_OpCode.rt].UW += static_cast<uint32_t>(*(m_DMEM + (((addr + 1) & 0xFFF) ^ 3))) << 16;
        m_GPR[m_OpCode.rt].UW += static_cast<uint32_t>(*(m_DMEM + (((addr + 2) & 0xFFF) ^ 3))) << 8;
        m_GPR[m_OpCode.rt].UW += *(m_DMEM + (((addr + 3) & 0xFFF) ^ 3));
    } else {
        m_GPR[m_OpCode.rt].UW = *reinterpret_cast<uint32_t*>(m_DMEM + (addr & 0xFFF));
    }
}

void RSPOp::LBU() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    m_GPR[m_OpCode.rt].UW = *(m_DMEM + ((addr ^ 3) & 0xFFF));
}

void RSPOp::LHU() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    if ((addr & 0x1) != 0) {
        m_GPR[m_OpCode.rt].UHW[0] = static_cast<uint16_t>(*(m_DMEM + (((addr + 0) & 0xFFF) ^ 3))) << 8;
        m_GPR[m_OpCode.rt].UHW[0] += *(m_DMEM + (((addr + 1) & 0xFFF) ^ 3));
    } else {
        m_GPR[m_OpCode.rt].UHW[0] = *reinterpret_cast<uint16_t*>(m_DMEM + ((addr ^ 2) & 0xFFF));
    }
    m_GPR[m_OpCode.rt].UW = m_GPR[m_OpCode.rt].UHW[0];
}

void RSPOp::LWU() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    if ((addr & 0x3) != 0) {
        m_GPR[m_OpCode.rt].UW = static_cast<uint32_t>(*(m_DMEM + (((addr + 0) & 0xFFF) ^ 3))) << 24;
        m_GPR[m_OpCode.rt].UW += static_cast<uint32_t>(*(m_DMEM + (((addr + 1) & 0xFFF) ^ 3))) << 16;
        m_GPR[m_OpCode.rt].UW += static_cast<uint32_t>(*(m_DMEM + (((addr + 2) & 0xFFF) ^ 3))) << 8;
        m_GPR[m_OpCode.rt].UW += *(m_DMEM + (((addr + 3) & 0xFFF) ^ 3));
    } else {
        m_GPR[m_OpCode.rt].UW = *reinterpret_cast<uint32_t*>(m_DMEM + (addr & 0xFFF));
    }
}

void RSPOp::SB() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_GPR[m_OpCode.rt].UB[0];
}

void RSPOp::SH() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    if ((addr & 0x1) != 0) {
        *(m_DMEM + ((addr ^ 3) & 0xFFF)) = static_cast<uint8_t>(m_GPR[m_OpCode.rt].UHW[0] >> 8);
        *(m_DMEM + (((addr + 1) ^ 3) & 0xFFF)) = static_cast<uint8_t>(m_GPR[m_OpCode.rt].UHW[0] & 0xFF);
    } else {
        *reinterpret_cast<uint16_t*>(m_DMEM + ((addr ^ 2) & 0xFFF)) = m_GPR[m_OpCode.rt].UHW[0];
    }
}

void RSPOp::SW() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + static_cast<int16_t>(m_OpCode.offset)) & 0xFFF;
    if ((addr & 0x3) != 0) {
        *(m_DMEM + (((addr + 0) ^ 3) & 0xFFF)) = static_cast<uint8_t>((m_GPR[m_OpCode.rt].UW >> 24) & 0xFF);
        *(m_DMEM + (((addr + 1) ^ 3) & 0xFFF)) = static_cast<uint8_t>((m_GPR[m_OpCode.rt].UW >> 16) & 0xFF);
        *(m_DMEM + (((addr + 2) ^ 3) & 0xFFF)) = static_cast<uint8_t>((m_GPR[m_OpCode.rt].UW >> 8) & 0xFF);
        *(m_DMEM + (((addr + 3) ^ 3) & 0xFFF)) = static_cast<uint8_t>(m_GPR[m_OpCode.rt].UW & 0xFF);
    } else {
        *reinterpret_cast<uint32_t*>(m_DMEM + (addr & 0xFFF)) = m_GPR[m_OpCode.rt].UW;
    }
}

// --- SPECIAL Function Instructions ---

void RSPOp::Special_SLL() { m_GPR[m_OpCode.rd].W = m_GPR[m_OpCode.rt].W << m_OpCode.sa; }
void RSPOp::Special_SRL() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rt].UW >> m_OpCode.sa; }
void RSPOp::Special_SRA() { m_GPR[m_OpCode.rd].W = m_GPR[m_OpCode.rt].W >> m_OpCode.sa; }
void RSPOp::Special_SLLV() { m_GPR[m_OpCode.rd].W = m_GPR[m_OpCode.rt].W << (m_GPR[m_OpCode.rs].W & 0x1F); }
void RSPOp::Special_SRLV() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rt].UW >> (m_GPR[m_OpCode.rs].W & 0x1F); }
void RSPOp::Special_SRAV() { m_GPR[m_OpCode.rd].W = m_GPR[m_OpCode.rt].W >> (m_GPR[m_OpCode.rs].W & 0x1F); }

void RSPOp::Special_JR() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = m_GPR[m_OpCode.rs].W & 0xFFC;
}

void RSPOp::Special_JALR() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = m_GPR[m_OpCode.rs].W & 0xFFC;
    m_GPR[m_OpCode.rd].W = (*m_SP_PC_REG + 8) & 0xFFC;
}

void RSPOp::Special_BREAK() {
    RSP_Running = false;
    *m_SP_STATUS_REG |= (SPStatus::Halt | SPStatus::Broke);
    // Do not set MI_INTR_SP or call CheckInterrupts() here. The SP interrupt is delivered
    // via a deferred RspTimer scheduled by ClearHalt() after DoRspCycles() returns;
    // ClearHalt accumulates the SP bit in m_deferred_mi_intr. Setting MI_INTR_SP here
    // would make the CPU's interrupt polling fire the exception immediately.
}

void RSPOp::Special_ADD() { m_GPR[m_OpCode.rd].W = m_GPR[m_OpCode.rs].W + m_GPR[m_OpCode.rt].W; }
void RSPOp::Special_ADDU() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rs].UW + m_GPR[m_OpCode.rt].UW; }
void RSPOp::Special_SUB() { m_GPR[m_OpCode.rd].W = m_GPR[m_OpCode.rs].W - m_GPR[m_OpCode.rt].W; }
void RSPOp::Special_SUBU() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rs].UW - m_GPR[m_OpCode.rt].UW; }
void RSPOp::Special_AND() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rs].UW & m_GPR[m_OpCode.rt].UW; }
void RSPOp::Special_OR() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rs].UW | m_GPR[m_OpCode.rt].UW; }
void RSPOp::Special_XOR() { m_GPR[m_OpCode.rd].UW = m_GPR[m_OpCode.rs].UW ^ m_GPR[m_OpCode.rt].UW; }
void RSPOp::Special_NOR() { m_GPR[m_OpCode.rd].UW = ~(m_GPR[m_OpCode.rs].UW | m_GPR[m_OpCode.rt].UW); }
void RSPOp::Special_SLT() { m_GPR[m_OpCode.rd].UW = (m_GPR[m_OpCode.rs].W < m_GPR[m_OpCode.rt].W) ? 1u : 0u; }
void RSPOp::Special_SLTU() { m_GPR[m_OpCode.rd].UW = (m_GPR[m_OpCode.rs].UW < m_GPR[m_OpCode.rt].UW) ? 1u : 0u; }

// --- RegImm Branch Instructions ---

void RSPOp::BLTZ() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W < 0);
}

void RSPOp::BGEZ() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W >= 0);
}

void RSPOp::BLTZAL() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W < 0);
    m_GPR[31].UW = (*m_SP_PC_REG + 8) & 0xFFC;
}

void RSPOp::BGEZAL() {
    m_NextInstruction = PipelineStage::DoDelaySlot;
    m_JumpTo = BranchIf(m_GPR[m_OpCode.rs].W >= 0);
    m_GPR[31].UW = (*m_SP_PC_REG + 8) & 0xFFC;
}

// --- COP0 - System Control (SP/DPC registers) ---

void RSPOp::Cop0_MF() {
    if (g_RSPDebugger != nullptr) {
        g_RSPDebugger->RDP_LogMF0(*m_SP_PC_REG, m_OpCode.rd);
    }

    switch (m_OpCode.rd) {
    case 0: m_GPR[m_OpCode.rt].UW = m_RSPRegisterHandler->ReadReg(RSPRegister::MemAddr); break;
    case 1: m_GPR[m_OpCode.rt].UW = m_RSPRegisterHandler->ReadReg(RSPRegister::DramAddr); break;
    case 2: m_GPR[m_OpCode.rt].UW = m_RSPRegisterHandler->ReadReg(RSPRegister::RdLen); break;
    case 3: m_GPR[m_OpCode.rt].UW = m_RSPRegisterHandler->ReadReg(RSPRegister::WrLen); break;
    case 4: m_GPR[m_OpCode.rt].UW = m_RSPRegisterHandler->ReadReg(RSPRegister::Status); break;
    case 5: m_GPR[m_OpCode.rt].UW = *m_SP_DMA_FULL_REG; break;
    case 6: m_GPR[m_OpCode.rt].UW = *m_SP_DMA_BUSY_REG; break;
    case 7:
        // Semaphore: test-and-set — read current value, then set to 1
        m_GPR[m_OpCode.rt].W = *m_SP_SEMAPHORE_REG;
        *m_SP_SEMAPHORE_REG = 1;
        break;
    case 8:  m_GPR[m_OpCode.rt].UW = *m_DPC_START_REG; break;
    case 9:  m_GPR[m_OpCode.rt].UW = *m_DPC_END_REG; break;
    case 10: m_GPR[m_OpCode.rt].UW = *m_DPC_CURRENT_REG; break;
    case 11: m_GPR[m_OpCode.rt].W = *m_DPC_STATUS_REG; break;
    case 12: m_GPR[m_OpCode.rt].W = *m_DPC_CLOCK_REG; break;
    default: break; // Unimplemented COP0 register
    }
}

void RSPOp::Cop0_MT() {
    switch (m_OpCode.rd) {
    case 0: m_RSPRegisterHandler->WriteReg(RSPRegister::MemAddr, m_GPR[m_OpCode.rt].UW); break;
    case 1: m_RSPRegisterHandler->WriteReg(RSPRegister::DramAddr, m_GPR[m_OpCode.rt].UW); break;
    case 2: m_RSPRegisterHandler->WriteReg(RSPRegister::RdLen, m_GPR[m_OpCode.rt].UW); break;
    case 3: m_RSPRegisterHandler->WriteReg(RSPRegister::WrLen, m_GPR[m_OpCode.rt].UW); break;
    case 4: m_RSPRegisterHandler->WriteReg(RSPRegister::Status, m_GPR[m_OpCode.rt].UW); break;
    case 7: *m_SP_SEMAPHORE_REG = 0; break;
    case 8: {
        uint32_t masked = m_GPR[m_OpCode.rt].UW & 0x00FFFFF8;
        // On hardware, writes to START are ignored while START_VALID is set
        if (!(*m_DPC_STATUS_REG & DPCStatus::StartValid)) {
            *m_DPC_START_REG = masked;
            *m_DPC_STATUS_REG |= DPCStatus::StartValid;
        }
        break;
    }
    case 9: {
        *m_DPC_END_REG = m_GPR[m_OpCode.rt].UW & 0x00FFFFF8;
        // When END is written and START_VALID is set, commit pending start
        if (*m_DPC_STATUS_REG & DPCStatus::StartValid) {
            *m_DPC_CURRENT_REG = *m_DPC_START_REG;
            *m_DPC_STATUS_REG &= ~DPCStatus::StartValid;
        }
        if (ProcessRdpList != nullptr) {
            if (g_rspTimeRdp) {
                const auto start = std::chrono::steady_clock::now();
                ProcessRdpList();
                g_rspRdpNanos += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                        .count());
            } else {
                ProcessRdpList();
            }
        }
        break;
    }
    case 10: *m_DPC_CURRENT_REG = m_GPR[m_OpCode.rt].UW; break;
    case 11: {
        uint32_t val = m_GPR[m_OpCode.rt].UW;
        if (val & DPCWrite::ClrXbusDmemDma) *m_DPC_STATUS_REG &= ~DPCStatus::XbusDmemDma;
        if (val & DPCWrite::SetXbusDmemDma) *m_DPC_STATUS_REG |= DPCStatus::XbusDmemDma;
        if (val & DPCWrite::ClrFreeze) *m_DPC_STATUS_REG &= ~DPCStatus::Freeze;
        if (val & DPCWrite::SetFreeze) *m_DPC_STATUS_REG |= DPCStatus::Freeze;
        if (val & DPCWrite::ClrFlush) *m_DPC_STATUS_REG &= ~DPCStatus::Flush;
        if (val & DPCWrite::SetFlush) *m_DPC_STATUS_REG |= DPCStatus::Flush;
        // ClrTmemCtr, ClrPipeCtr, ClrCmdCtr, ClrClockCtr: acknowledged but no-op
        break;
    }
    default: break;
    }
}

// --- COP2 - Scalar ↔ Vector Register Moves ---

void RSPOp::Cop2_MF() {
    uint8_t element = static_cast<uint8_t>(m_OpCode.sa >> 1);
    m_GPR[m_OpCode.rt].B[1] = m_Vect[m_OpCode.vs].s8(15 - element);
    m_GPR[m_OpCode.rt].B[0] = m_Vect[m_OpCode.vs].s8(15 - ((element + 1) % 16));
    m_GPR[m_OpCode.rt].W = m_GPR[m_OpCode.rt].HW[0];
}

void RSPOp::Cop2_CF() {
    switch (m_OpCode.rd & 0x03) {
    case 0: m_GPR[m_OpCode.rt].W = static_cast<int16_t>((VCOH.GetPacked() << 8) | VCOL.GetPacked()); break;
    case 1: m_GPR[m_OpCode.rt].W = static_cast<int16_t>((VCCH.GetPacked() << 8) | VCCL.GetPacked()); break;
    case 2: // fall-through
    case 3: m_GPR[m_OpCode.rt].W = static_cast<uint8_t>(VCE.GetPacked()); break;
    }
}

void RSPOp::Cop2_MT() {
    uint8_t element = static_cast<uint8_t>(15 - (m_OpCode.sa >> 1));
    m_Vect[m_OpCode.vs].s8(element) = m_GPR[m_OpCode.rt].B[1];
    if (element != 0) {
        m_Vect[m_OpCode.vs].s8(element - 1) = m_GPR[m_OpCode.rt].B[0];
    }
}

void RSPOp::Cop2_CT() {
    switch (m_OpCode.rd & 0x03) {
    case 0:
        VCOL.SetPacked(m_GPR[m_OpCode.rt].UB[0]);
        VCOH.SetPacked(m_GPR[m_OpCode.rt].UB[1]);
        break;
    case 1:
        VCCL.SetPacked(m_GPR[m_OpCode.rt].UB[0]);
        VCCH.SetPacked(m_GPR[m_OpCode.rt].UB[1]);
        break;
    case 2:
    case 3: VCE.SetPacked(m_GPR[m_OpCode.rt].UB[0]); break;
    }
}

void RSPOp::Cop2_VECTOR() { (this->*Jump_Vector[m_OpCode.funct])(); }

// --- Vector Multiply Instructions ---

void RSPOp::Vector_VMULF() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, (static_cast<int64_t>(m_Vect[m_OpCode.vs].s16(el)) *
                         static_cast<int64_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e)) * 2) +
                            0x8000);
        Result.s16(el) = m_ACCUM.Saturate(el, true);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMULU() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, (static_cast<int64_t>(m_Vect[m_OpCode.vs].s16(el)) *
                         static_cast<int64_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e)) * 2) +
                            0x8000);
        if (m_ACCUM.High(el) < 0) Result.s16(el) = 0;
        else if ((m_ACCUM.High(el) ^ m_ACCUM.Mid(el)) < 0) Result.s16(el) = -1;
        else Result.s16(el) = m_ACCUM.Mid(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VRNDP() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t value = m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        if (m_OpCode.vs & 1) value <<= 16;
        int64_t accum = m_ACCUM.Get(el);
        if (accum >= 0) accum = Clip48(accum + value);
        m_ACCUM.Set(el, accum);
        Result.s16(el) = Clamp16(static_cast<int32_t>(accum >> 16));
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMULQ() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t temp = m_Vect[m_OpCode.vs].s16(el) * m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        if (temp < 0) temp += 31;
        m_ACCUM.High(el) = static_cast<int16_t>(temp >> 16);
        m_ACCUM.Mid(el) = static_cast<int16_t>(temp);
        m_ACCUM.Low(el) = 0;
        Result.s16(el) = Clamp16(temp >> 1) & ~15;
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMUDL() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, static_cast<uint16_t>(static_cast<uint32_t>(m_Vect[m_OpCode.vs].u16(el)) *
                                                  static_cast<uint32_t>(m_Vect[m_OpCode.vt].ue(el, m_OpCode.e)) >>
                                              16));
        Result.s16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMUDM() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, static_cast<int32_t>(static_cast<int32_t>(m_Vect[m_OpCode.vs].s16(el)) *
                                             static_cast<uint32_t>(m_Vect[m_OpCode.vt].ue(el, m_OpCode.e))));
        Result.s16(el) = m_ACCUM.Mid(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMUDN() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, static_cast<int32_t>(
                            static_cast<uint32_t>(m_Vect[m_OpCode.vs].u16(el)) *
                            static_cast<uint32_t>(static_cast<int32_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e)))));
        Result.s16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMUDH() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, (static_cast<int64_t>(static_cast<int32_t>(m_Vect[m_OpCode.vs].s16(el)) *
                                              static_cast<int32_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e))))
                            << 16);
        Result.u16(el) = m_ACCUM.Saturate(el, true);
    }
    m_Vect[m_OpCode.vd] = Result;
}

// --- Vector Multiply-Accumulate Instructions ---

void RSPOp::Vector_VMACF() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, m_ACCUM.Get(el) + (static_cast<int64_t>(m_Vect[m_OpCode.vs].s16(el)) *
                                               static_cast<int64_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e))
                                           << 1));
        Result.u16(el) = m_ACCUM.Saturate(el, true);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMACU() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, m_ACCUM.Get(el) + (static_cast<int64_t>(m_Vect[m_OpCode.vs].s16(el)) *
                                               static_cast<int64_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e))
                                           << 1));
        if (m_ACCUM.High(el) < 0) Result.s16(el) = 0;
        else if (m_ACCUM.UHigh(el) != 0 || m_ACCUM.Mid(el) < 0) Result.u16(el) = 0xFFFF;
        else Result.s16(el) = m_ACCUM.Mid(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMACQ() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t accum = (m_ACCUM.UHigh(el) << 16) | m_ACCUM.UMid(el);
        if (accum < -0x20 && ((accum & 0x20) == 0)) accum += 0x20;
        else if (accum > 0x20 && (accum & 0x20) == 0) accum -= 0x20;
        Result.u16(el) = Clamp16(accum >> 1) & 0xFFF0;
        m_ACCUM.UHigh(el) = static_cast<uint16_t>(accum >> 16);
        m_ACCUM.UMid(el) = static_cast<uint16_t>(accum);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VRNDN() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t value = m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        if (m_OpCode.vs & 1) value <<= 16;
        int64_t accum = m_ACCUM.Get(el);
        if (accum < 0) accum = Clip48(accum + value);
        m_ACCUM.Set(el, accum);
        Result.s16(el) = Clamp16(static_cast<int32_t>(accum >> 16));
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMADL() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, m_ACCUM.Get(el) + (static_cast<uint32_t>(m_Vect[m_OpCode.vs].u16(el)) *
                                               static_cast<uint32_t>(m_Vect[m_OpCode.vt].ue(el, m_OpCode.e)) >>
                                           16));
        Result.u16(el) = m_ACCUM.Saturate(el, false);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMADM() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, m_ACCUM.Get(el) +
                            static_cast<int64_t>(m_Vect[m_OpCode.vs].s16(el)) * m_Vect[m_OpCode.vt].ue(el, m_OpCode.e));
        Result.u16(el) = m_ACCUM.Saturate(el, true);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMADN() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Set(el, m_ACCUM.Get(el) +
                            static_cast<int64_t>(m_Vect[m_OpCode.vs].u16(el)) * m_Vect[m_OpCode.vt].se(el, m_OpCode.e));
        Result.u16(el) = m_ACCUM.Saturate(el, false);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VMADH() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t value = static_cast<int32_t>((m_ACCUM.Get(el) >> 16) +
                                             static_cast<int32_t>(m_Vect[m_OpCode.vs].s16(el)) *
                                                 static_cast<int32_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e)));
        m_ACCUM.High(el) = static_cast<int16_t>(value >> 16);
        m_ACCUM.Mid(el) = static_cast<int16_t>(value);
        Result.u16(el) = m_ACCUM.Saturate(el, true);
    }
    m_Vect[m_OpCode.vd] = Result;
}

// --- Vector Add / Subtract / Absolute ---

void RSPOp::Vector_VADD() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t value = static_cast<int32_t>(m_Vect[m_OpCode.vs].s16(el)) +
                        static_cast<int32_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e)) + VCOL.Get(el);
        m_ACCUM.Low(el) = static_cast<int16_t>(value);
        Result.u16(el) = Clamp16(value);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCOL.Clear();
    VCOH.Clear();
}

void RSPOp::Vector_VSUB() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        int32_t value = static_cast<int32_t>(m_Vect[m_OpCode.vs].s16(el)) -
                        static_cast<int32_t>(m_Vect[m_OpCode.vt].se(el, m_OpCode.e)) - VCOL.Get(el);
        m_ACCUM.Low(el) = static_cast<int16_t>(value);
        Result.u16(el) = Clamp16(value);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCOL.Clear();
    VCOH.Clear();
}

void RSPOp::Vector_VABS() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        if (m_Vect[m_OpCode.vs].s16(el) > 0) {
            Result.s16(el) = m_Vect[m_OpCode.vt].ue(el, m_OpCode.e);
            m_ACCUM.ULow(el) = Result.u16(el);
        } else if (m_Vect[m_OpCode.vs].s16(el) < 0) {
            if (m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) == 0x8000) {
                Result.u16(el) = 0x7FFF;
                m_ACCUM.ULow(el) = 0x8000;
            } else {
                Result.u16(el) = m_Vect[m_OpCode.vt].se(el, m_OpCode.e) * -1;
                m_ACCUM.ULow(el) = Result.u16(el);
            }
        } else {
            Result.u16(el) = 0;
            m_ACCUM.ULow(el) = 0;
        }
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VADDC() {
    RSPVector Result;
    VCOH.Clear();
    for (uint8_t el = 0; el < 8; el++) {
        int32_t temp = static_cast<int32_t>(m_Vect[m_OpCode.vs].u16(el)) +
                       static_cast<int32_t>(m_Vect[m_OpCode.vt].ue(el, m_OpCode.e));
        m_ACCUM.Low(el) = static_cast<int16_t>(temp);
        Result.u16(el) = m_ACCUM.Low(el);
        VCOL.Set(el, (temp >> 16) != 0);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VSUBC() {
    RSPVector Result;
    VCOH.Clear();
    for (uint8_t el = 0; el < 8; el++) {
        int32_t temp = static_cast<int32_t>(m_Vect[m_OpCode.vs].u16(el)) -
                       static_cast<int32_t>(m_Vect[m_OpCode.vt].ue(el, m_OpCode.e));
        m_ACCUM.Low(el) = static_cast<int16_t>(temp);
        Result.u16(el) = m_ACCUM.Low(el);
        VCOL.Set(el, (temp >> 16) != 0);
        VCOH.Set(el, temp != 0);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_Reserved() {
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Low(el) = m_Vect[m_OpCode.vs].s16(el) + m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
    }
    m_Vect[m_OpCode.vd] = RSPVector{};
}

void RSPOp::Vector_VSAW() {
    RSPVector Result;
    switch (m_OpCode.rs & 0xF) {
    case 8:
        for (uint8_t el = 0; el < 8; el++) Result.s16(el) = m_ACCUM.High(el);
        break;
    case 9:
        for (uint8_t el = 0; el < 8; el++) Result.s16(el) = m_ACCUM.Mid(el);
        break;
    case 10:
        for (uint8_t el = 0; el < 8; el++) Result.s16(el) = m_ACCUM.Low(el);
        break;
    default:
        Result.u64(0) = 0;
        Result.u64(1) = 0;
        break;
    }
    m_Vect[m_OpCode.vd] = Result;
}

// --- Vector Compare / Select Instructions ---

void RSPOp::Vector_VLT() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        if (m_Vect[m_OpCode.vs].s16(el) < m_Vect[m_OpCode.vt].se(el, m_OpCode.e) ||
            (m_Vect[m_OpCode.vs].s16(el) == m_Vect[m_OpCode.vt].se(el, m_OpCode.e) && VCOL.Get(el) && VCOH.Get(el))) {
            Result.u16(el) = m_Vect[m_OpCode.vs].u16(el);
            VCCL.Set(el, true);
        } else {
            Result.u16(el) = m_Vect[m_OpCode.vt].ue(el, m_OpCode.e);
            VCCL.Set(el, false);
        }
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCCH.Clear();
    VCOL.Clear();
    VCOH.Clear();
}

void RSPOp::Vector_VEQ() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Low(el) =
            VCCL.Set(el, m_Vect[m_OpCode.vs].u16(el) == m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) && !VCOH.Get(el))
                ? m_Vect[m_OpCode.vs].u16(el)
                : m_Vect[m_OpCode.vt].ue(el, m_OpCode.e);
        Result.u16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCOL.Clear();
    VCOH.Clear();
    VCCH.Clear();
}

void RSPOp::Vector_VNE() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Low(el) =
            VCCL.Set(el, m_Vect[m_OpCode.vs].u16(el) != m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) || VCOH.Get(el))
                ? m_Vect[m_OpCode.vs].u16(el)
                : m_Vect[m_OpCode.vt].ue(el, m_OpCode.e);
        Result.u16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCCH.Clear();
    VCOL.Clear();
    VCOH.Clear();
}

void RSPOp::Vector_VGE() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        if (m_Vect[m_OpCode.vs].s16(el) > m_Vect[m_OpCode.vt].se(el, m_OpCode.e) ||
            (m_Vect[m_OpCode.vs].s16(el) == m_Vect[m_OpCode.vt].se(el, m_OpCode.e) &&
             (!VCOL.Get(el) || !VCOH.Get(el)))) {
            m_ACCUM.ULow(el) = m_Vect[m_OpCode.vs].u16(el);
            VCCL.Set(el, true);
        } else {
            m_ACCUM.ULow(el) = m_Vect[m_OpCode.vt].ue(el, m_OpCode.e);
            VCCL.Set(el, false);
        }
        Result.u16(el) = m_ACCUM.ULow(el);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCCH.Clear();
    VCOL.Clear();
    VCOH.Clear();
}

// --- Vector Clip Instructions ---

void RSPOp::Vector_VCL() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        if (VCOL.Get(el)) {
            if (VCOH.Get(el)) {
                m_ACCUM.Low(el) = VCCL.Get(el) ? -m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) : m_Vect[m_OpCode.vs].s16(el);
            } else {
                bool set = VCE.Get(el)
                               ? (m_Vect[m_OpCode.vs].u16(el) + m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) <= 0x10000)
                               : (m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) + m_Vect[m_OpCode.vs].u16(el) == 0);
                m_ACCUM.Low(el) = set ? -m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) : m_Vect[m_OpCode.vs].s16(el);
                VCCL.Set(el, set);
            }
        } else {
            if (VCOH.Get(el)) {
                m_ACCUM.ULow(el) = VCCH.Get(el) ? m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) : m_Vect[m_OpCode.vs].s16(el);
            } else {
                m_ACCUM.Low(el) =
                    VCCH.Set(el, m_Vect[m_OpCode.vs].u16(el) - m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) >= 0)
                        ? m_Vect[m_OpCode.vt].ue(el, m_OpCode.e)
                        : m_Vect[m_OpCode.vs].s16(el);
            }
        }
        Result.s16(el) = m_ACCUM.Low(el);
    }
    VCOL.Clear();
    VCOH.Clear();
    VCE.Clear();
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VCH() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        if (VCOL.Set(el, (m_Vect[m_OpCode.vs].s16(el) ^ m_Vect[m_OpCode.vt].se(el, m_OpCode.e)) < 0)) {
            int16_t value = m_Vect[m_OpCode.vs].s16(el) + m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
            m_ACCUM.Low(el) = value <= 0 ? -m_Vect[m_OpCode.vt].se(el, m_OpCode.e) : m_Vect[m_OpCode.vs].s16(el);
            VCOH.Set(el, value != 0 && m_Vect[m_OpCode.vs].s16(el) != ~m_Vect[m_OpCode.vt].se(el, m_OpCode.e));
            VCCL.Set(el, value <= 0);
            VCCH.Set(el, m_Vect[m_OpCode.vt].se(el, m_OpCode.e) < 0);
            VCE.Set(el, value == -1);
        } else {
            int16_t value = m_Vect[m_OpCode.vs].s16(el) - m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
            m_ACCUM.Low(el) = value >= 0 ? m_Vect[m_OpCode.vt].ue(el, m_OpCode.e) : m_Vect[m_OpCode.vs].s16(el);
            VCOH.Set(el, value != 0 && m_Vect[m_OpCode.vs].s16(el) != ~m_Vect[m_OpCode.vt].se(el, m_OpCode.e));
            VCCL.Set(el, m_Vect[m_OpCode.vt].se(el, m_OpCode.e) < 0);
            VCCH.Set(el, value >= 0);
            VCE.Set(el, false);
        }
        Result.s16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VCR() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        if ((m_Vect[m_OpCode.vs].s16(el) ^ m_Vect[m_OpCode.vt].se(el, m_OpCode.e)) < 0) {
            VCCH.Set(el, m_Vect[m_OpCode.vt].se(el, m_OpCode.e) < 0);
            m_ACCUM.Low(el) =
                VCCL.Set(el, m_Vect[m_OpCode.vs].s16(el) + m_Vect[m_OpCode.vt].se(el, m_OpCode.e) + 1 <= 0)
                    ? ~m_Vect[m_OpCode.vt].ue(el, m_OpCode.e)
                    : m_Vect[m_OpCode.vs].u16(el);
        } else {
            VCCL.Set(el, m_Vect[m_OpCode.vt].se(el, m_OpCode.e) < 0);
            m_ACCUM.Low(el) = VCCH.Set(el, m_Vect[m_OpCode.vs].s16(el) - m_Vect[m_OpCode.vt].se(el, m_OpCode.e) >= 0)
                                  ? m_Vect[m_OpCode.vt].ue(el, m_OpCode.e)
                                  : m_Vect[m_OpCode.vs].u16(el);
        }
        Result.s16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCOL.Clear();
    VCOH.Clear();
    VCE.Clear();
}

// --- Vector Merge & Logical Instructions ---

void RSPOp::Vector_VMRG() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        m_ACCUM.Low(el) = VCCL.Get(el) ? m_Vect[m_OpCode.vs].s16(el) : m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        Result.s16(el) = m_ACCUM.Low(el);
    }
    m_Vect[m_OpCode.vd] = Result;
    VCOL.Clear();
    VCOH.Clear();
}

void RSPOp::Vector_VAND() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        Result.s16(el) = m_Vect[m_OpCode.vs].s16(el) & m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VNAND() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        Result.s16(el) = ~(m_Vect[m_OpCode.vs].s16(el) & m_Vect[m_OpCode.vt].se(el, m_OpCode.e));
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VOR() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        Result.s16(el) = m_Vect[m_OpCode.vs].s16(el) | m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VNOR() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        Result.s16(el) = ~(m_Vect[m_OpCode.vs].s16(el) | m_Vect[m_OpCode.vt].se(el, m_OpCode.e));
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VXOR() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        Result.s16(el) = m_Vect[m_OpCode.vs].s16(el) ^ m_Vect[m_OpCode.vt].se(el, m_OpCode.e);
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

void RSPOp::Vector_VNXOR() {
    RSPVector Result;
    for (uint8_t el = 0; el < 8; el++) {
        Result.s16(el) = ~(m_Vect[m_OpCode.vs].s16(el) ^ m_Vect[m_OpCode.vt].se(el, m_OpCode.e));
        m_ACCUM.Low(el) = Result.s16(el);
    }
    m_Vect[m_OpCode.vd] = Result;
}

// --- Vector Reciprocal / Inverse Square Root ---

void RSPOp::Vector_VRCP() {
    int32_t input = m_Vect[m_OpCode.vt].s16(7 - (m_OpCode.e & 0x7));
    int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    if (input > -32768) data -= mask;

    int32_t result;
    if (data == 0) result = 0x7FFFFFFF;
    else if (input == static_cast<int32_t>(0xFFFF8000)) result = static_cast<int32_t>(0xFFFF0000);
    else {
        uint32_t shift = clz32(data);
        uint32_t index = (static_cast<uint64_t>(data) << shift & 0x7FC00000) >> 22;
        result = (((0x10000 | m_Reg.m_Reciprocals[index]) << 14) >> (31 - shift)) ^ mask;
    }

    m_Reg.m_High = false;
    m_Reg.m_Result = static_cast<uint16_t>(result >> 16);
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].u16(EleSpec[m_OpCode.e].UB[i]);
    m_Vect[m_OpCode.vd].s16(7 - (m_OpCode.rd & 0x7)) = static_cast<int16_t>(result);
}

void RSPOp::Vector_VRCPL() {
    int32_t input = m_Reg.m_High ? (m_Reg.m_In << 16 | m_Vect[m_OpCode.vt].u16(7 - (m_OpCode.e & 0x7)))
                                 : m_Vect[m_OpCode.vt].s16(7 - (m_OpCode.e & 0x7));
    int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    if (input > -32768) data -= mask;

    int32_t result;
    if (data == 0) result = 0x7FFFFFFF;
    else if (input == static_cast<int32_t>(0xFFFF8000)) result = static_cast<int32_t>(0xFFFF0000);
    else {
        uint32_t shift = clz32(data);
        uint32_t index = (static_cast<uint64_t>(data) << shift & 0x7FC00000) >> 22;
        result = (((0x10000 | m_Reg.m_Reciprocals[index]) << 14) >> (31 - shift)) ^ mask;
    }

    m_Reg.m_High = false;
    m_Reg.m_Result = static_cast<uint16_t>(result >> 16);
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].u16(EleSpec[m_OpCode.e].UB[i]);
    m_Vect[m_OpCode.vd].s16(7 - (m_OpCode.rd & 0x7)) = static_cast<int16_t>(result);
}

void RSPOp::Vector_VRCPH() {
    m_Reg.m_High = true;
    m_Reg.m_In = m_Vect[m_OpCode.vt].u16(7 - (m_OpCode.e & 0x7));
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].u16(EleSpec[m_OpCode.e].UB[i]);
    m_Vect[m_OpCode.vd].u16(7 - (m_OpCode.de & 0x7)) = m_Reg.m_Result;
}

void RSPOp::Vector_VMOV() {
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].ue(i, m_OpCode.e);
    uint8_t index = 7 - (m_OpCode.de & 0x7);
    m_Vect[m_OpCode.vd].u16(index) = m_Vect[m_OpCode.vt].se(index, m_OpCode.e);
}

void RSPOp::Vector_VRSQ() {
    int32_t input = m_Vect[m_OpCode.vt].s16(7 - (m_OpCode.e & 0x7));

    uint32_t uinput = static_cast<uint32_t>(input);
    int32_t result;

    if (uinput == 0) result = 0x7FFFFFFF;
    else if (uinput == 0xFFFF8000u) result = static_cast<int32_t>(0xFFFF0000u);
    else {
        if (uinput > 0xFFFF8000u) uinput--;
        int32_t sinput = static_cast<int32_t>(uinput);
        int32_t mask = sinput >> 31;
        uinput ^= static_cast<uint32_t>(mask);

        uint32_t shift = clz32(uinput) + 1;
        uint32_t index = ((uinput << (shift & 31)) >> 24) | ((shift & 1) << 8);
        uint32_t rom = static_cast<uint32_t>(m_Reg.m_InverseSquareRoots[index]) << 14;
        uint32_t r_shift = (32 - shift) >> 1;
        result = static_cast<int32_t>((0x40000000u | rom) >> r_shift) ^ mask;
    }

    m_Reg.m_High = false;
    m_Reg.m_Result = static_cast<uint16_t>(static_cast<uint32_t>(result) >> 16);
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].ue(i, m_OpCode.e);
    m_Vect[m_OpCode.vd].s16(7 - (m_OpCode.rd & 0x7)) = static_cast<int16_t>(result);
}

void RSPOp::Vector_VRSQL() {
    uint32_t uinput;
    if (m_Reg.m_High)
        uinput = (static_cast<uint32_t>(m_Reg.m_In) << 16) | m_Vect[m_OpCode.vt].u16(7 - (m_OpCode.e & 0x7));
    else uinput = static_cast<uint32_t>(m_Vect[m_OpCode.vt].s16(7 - (m_OpCode.e & 0x7)));

    int32_t result;

    if (uinput == 0) result = 0x7FFFFFFF;
    else if (uinput == 0xFFFF8000u) result = static_cast<int32_t>(0xFFFF0000u);
    else {
        if (uinput > 0xFFFF8000u) uinput--;
        int32_t sinput = static_cast<int32_t>(uinput);
        int32_t mask = sinput >> 31;
        uinput ^= static_cast<uint32_t>(mask);

        uint32_t shift = clz32(uinput) + 1;
        uint32_t index = ((uinput << (shift & 31)) >> 24) | ((shift & 1) << 8);
        uint32_t rom = static_cast<uint32_t>(m_Reg.m_InverseSquareRoots[index]) << 14;
        uint32_t r_shift = (32 - shift) >> 1;
        result = static_cast<int32_t>((0x40000000u | rom) >> r_shift) ^ mask;
    }

    m_Reg.m_High = false;
    m_Reg.m_Result = static_cast<uint16_t>(static_cast<uint32_t>(result) >> 16);
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].u16(EleSpec[m_OpCode.e].UB[i]);
    m_Vect[m_OpCode.vd].s16(7 - (m_OpCode.rd & 0x7)) = static_cast<int16_t>(result);
}

void RSPOp::Vector_VRSQH() {
    m_Reg.m_High = true;
    m_Reg.m_In = m_Vect[m_OpCode.vt].u16(7 - (m_OpCode.e & 0x7));
    for (uint8_t i = 0; i < 8; i++) m_ACCUM.Low(i) = m_Vect[m_OpCode.vt].u16(EleSpec[m_OpCode.e].UB[i]);
    m_Vect[m_OpCode.vd].u16(7 - (m_OpCode.rd & 0x7)) = m_Reg.m_Result;
}

void RSPOp::Vector_VNOOP() {
    // No operation
}

// --- LC2 - Vector Memory Loads ---

void RSPOp::LBV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 0)) & 0xFFF;
    m_Vect[m_OpCode.vt].u8(static_cast<uint8_t>(15 - m_OpCode.del)) = *(m_DMEM + (addr ^ 3));
}

void RSPOp::LSV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 1)) & 0xFFF;
    uint8_t length = std::min<uint8_t>(2, 16 - m_OpCode.del);
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(length + m_OpCode.del); i < n; i++, addr++) {
        m_Vect[m_OpCode.vt].u8(15 - i) = *(m_DMEM + ((addr ^ 3) & 0xFFF));
    }
}

void RSPOp::LLV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 2)) & 0xFFF;
    uint8_t length = std::min<uint8_t>(4, 16 - m_OpCode.del);
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(length + m_OpCode.del); i < n; i++, addr++) {
        m_Vect[m_OpCode.vt].u8(15 - i) = *(m_DMEM + ((addr ^ 3) & 0xFFF));
    }
}

void RSPOp::LDV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 3)) & 0xFFF;
    uint8_t length = std::min<uint8_t>(8, 16 - m_OpCode.del);
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(length + m_OpCode.del); i < n; i++, addr++) {
        m_Vect[m_OpCode.vt].u8(15 - i) = *(m_DMEM + ((addr ^ 3) & 0xFFF));
    }
}

void RSPOp::LQV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4)) & 0xFFF;
    uint8_t length =
        std::min<uint8_t>(static_cast<uint8_t>(((addr + 0x10) & ~0xF) - addr), static_cast<uint8_t>(16 - m_OpCode.del));
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(length + m_OpCode.del); i < n; i++, addr++) {
        m_Vect[m_OpCode.vt].u8(15 - i) = *(m_DMEM + (addr ^ 3));
    }
}

void RSPOp::LRV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4)) & 0xFFF;
    uint8_t offset = static_cast<uint8_t>((0x10 - (addr & 0xF)) + m_OpCode.del);
    addr &= 0xFF0;
    for (uint8_t i = offset; i < 16; i++, addr++) {
        m_Vect[m_OpCode.vt].u8(15 - i) = *(m_DMEM + ((addr ^ 3) & 0xFFF));
    }
}

void RSPOp::LPV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 3));
    uint32_t off = (addr & 7) - m_OpCode.del;
    addr &= ~7u;
    for (uint8_t i = 0; i < 8; i++) {
        m_Vect[m_OpCode.vt].u16(7 - i) = static_cast<uint16_t>(*(m_DMEM + ((addr + ((off + i) & 0xF) ^ 3) & 0xFFF)))
                                         << 8;
    }
}

void RSPOp::LUV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 3));
    uint32_t off = (addr & 7) - m_OpCode.del;
    addr &= ~7u;
    for (uint8_t i = 0; i < 8; i++) {
        m_Vect[m_OpCode.vt].s16(7 - i) = static_cast<int16_t>(*(m_DMEM + ((addr + ((off + i) & 0xF) ^ 3) & 0xFFF)))
                                         << 7;
    }
}

void RSPOp::LHV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4));
    uint32_t off = (addr & 7) - m_OpCode.del;
    addr &= ~7u;
    for (uint8_t i = 0; i < 8; i++) {
        m_Vect[m_OpCode.vt].s16(7 - i) =
            static_cast<int16_t>(*(m_DMEM + ((addr + ((off + (i << 1)) & 0xF) ^ 3) & 0xFFF))) << 7;
    }
}

void RSPOp::LFV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4));
    uint8_t length = std::min<uint8_t>(8 + m_OpCode.del, 16);
    uint32_t off = (addr & 7) - m_OpCode.del;
    addr &= ~7u;

    RSPVector temp;
    for (uint8_t i = 0; i < 4; i++) {
        temp.s16(i) = static_cast<int16_t>(*(m_DMEM + ((addr + ((off + (i << 2)) & 0xF) ^ 3) & 0xFFF))) << 7;
        temp.s16(i + 4) = static_cast<int16_t>(*(m_DMEM + ((addr + ((off + (i << 2) + 8) & 0xF) ^ 3) & 0xFFF))) << 7;
    }
    for (uint8_t i = m_OpCode.del; i < length; i++) {
        m_Vect[m_OpCode.vt].s8(15 - i) = temp.s8(i ^ 1);
    }
}

void RSPOp::LWV() {
    // Not implemented on real hardware
}

void RSPOp::LTV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4));
    uint32_t start = addr & ~7u;
    uint32_t end = start + 0x10;
    addr = start + ((m_OpCode.del + (addr & 8)) & 0xF);
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t del = (((m_OpCode.del >> 1) + i) & 7) + (m_OpCode.rt & ~7);
        m_Vect[del].s8(15 - (i * 2 + 0)) = *(m_DMEM + ((addr++ ^ 3) & 0xFFF));
        if (addr == end) addr = start;
        m_Vect[del].s8(15 - (i * 2 + 1)) = *(m_DMEM + ((addr++ ^ 3) & 0xFFF));
        if (addr == end) addr = start;
    }
}

// --- SC2 - Vector Memory Stores ---

void RSPOp::SBV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 0)) & 0xFFF;
    *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(static_cast<uint8_t>(15 - m_OpCode.del));
}

void RSPOp::SSV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 1)) & 0xFFF;
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(2 + m_OpCode.del); i < n; i++, addr++) {
        *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - (i & 0xF));
    }
}

void RSPOp::SLV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 2)) & 0xFFF;
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(4 + m_OpCode.del); i < n; i++, addr++) {
        *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - (i & 0xF));
    }
}

void RSPOp::SDV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 3)) & 0xFFF;
    for (uint8_t i = m_OpCode.del; i < (8 + m_OpCode.del); i++, addr++) {
        *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - (i & 0xF));
    }
}

void RSPOp::SQV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4)) & 0xFFF;
    uint8_t length = static_cast<uint8_t>(((addr + 0x10) & ~0xF) - addr);
    for (uint8_t i = m_OpCode.del; i < (length + m_OpCode.del); i++, addr++) {
        *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - (i & 0xF));
    }
}

void RSPOp::SRV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4)) & 0xFFF;
    uint8_t length = addr & 0xF;
    uint8_t offset = (0x10 - length) & 0xF;
    addr &= 0xFF0;
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(length + m_OpCode.del); i < n; i++, addr++) {
        *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - ((i + offset) & 0xF));
    }
}

void RSPOp::SPV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 3)) & 0xFFF;
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(8 + m_OpCode.del); i < n; i++, addr++) {
        if ((i & 0xF) < 8) *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - ((i & 0xF) << 1));
        else
            *(m_DMEM + ((addr ^ 3) & 0xFFF)) =
                static_cast<uint8_t>((m_Vect[m_OpCode.vt].u8(15 - ((i & 0x7) << 1)) << 1) +
                                     (m_Vect[m_OpCode.vt].u8(14 - ((i & 0x7) << 1)) >> 7));
    }
}

void RSPOp::SUV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 3)) & 0xFFF;
    for (uint8_t count = m_OpCode.del; count < (8 + m_OpCode.del); count++, addr++) {
        if ((count & 0xF) < 8)
            *(m_DMEM + ((addr ^ 3) & 0xFFF)) =
                static_cast<uint8_t>((m_Vect[m_OpCode.vt].u8(15 - ((count & 0x7) << 1)) << 1) +
                                     (m_Vect[m_OpCode.vt].u8(14 - ((count & 0x7) << 1)) >> 7));
        else *(m_DMEM + ((addr ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].u8(15 - ((count & 0x7) << 1));
    }
}

void RSPOp::SHV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4));
    uint8_t offset = addr & 7;
    addr &= ~7u;
    for (uint32_t i = 0; i < 16; i += 2) {
        uint8_t value = (m_Vect[m_OpCode.vt].u8(15 - ((m_OpCode.del + i) & 15)) << 1) |
                        (m_Vect[m_OpCode.vt].u8(15 - ((m_OpCode.del + i + 1) & 15)) >> 7);
        *(m_DMEM + (((addr + ((offset + i) & 15)) ^ 3) & 0xFFF)) = value;
    }
}

void RSPOp::SFV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4)) & 0xFFF;
    uint8_t offset = addr & 7;
    addr &= 0xFF8;

    auto write4 = [&](uint8_t e0, uint8_t e1, uint8_t e2, uint8_t e3) {
        *(m_DMEM + (((addr + offset) ^ 3) & 0xFFF)) = (m_Vect[m_OpCode.vt].u16(e0) >> 7) & 0xFF;
        *(m_DMEM + (((addr + ((offset + 4) & 0xF)) ^ 3) & 0xFFF)) = (m_Vect[m_OpCode.vt].u16(e1) >> 7) & 0xFF;
        *(m_DMEM + (((addr + ((offset + 8) & 0xF)) ^ 3) & 0xFFF)) = (m_Vect[m_OpCode.vt].u16(e2) >> 7) & 0xFF;
        *(m_DMEM + (((addr + ((offset + 12) & 0xF)) ^ 3) & 0xFFF)) = (m_Vect[m_OpCode.vt].u16(e3) >> 7) & 0xFF;
    };

    switch (m_OpCode.del) {
    case 0:
    case 15: write4(7, 6, 5, 4); break;
    case 1:  write4(1, 0, 3, 2); break;
    case 4:  write4(6, 5, 4, 7); break;
    case 5:  write4(0, 3, 2, 1); break;
    case 8:  write4(3, 2, 1, 0); break;
    case 11: write4(4, 7, 6, 5); break;
    case 12: write4(2, 1, 0, 3); break;
    default:
        *(m_DMEM + (((addr + offset) ^ 3) & 0xFFF)) = 0;
        *(m_DMEM + (((addr + ((offset + 4) & 0xF)) ^ 3) & 0xFFF)) = 0;
        *(m_DMEM + (((addr + ((offset + 8) & 0xF)) ^ 3) & 0xFFF)) = 0;
        *(m_DMEM + (((addr + ((offset + 12) & 0xF)) ^ 3) & 0xFFF)) = 0;
        break;
    }
}

void RSPOp::STV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4));
    uint8_t element = 16 - (m_OpCode.del & ~1);
    uint8_t offset = static_cast<uint8_t>((addr & 7) - (m_OpCode.del & ~1));
    addr &= ~7u;
    for (uint32_t i = 0; i < 16; i += 2) {
        uint8_t del = static_cast<uint8_t>((m_OpCode.vt & ~7) + (i >> 1));
        *(m_DMEM + (((addr + ((offset + i) & 15)) ^ 3) & 0xFFF)) = m_Vect[del].s8(15 - ((element + i) & 15));
        *(m_DMEM + (((addr + ((offset + i + 1) & 15)) ^ 3) & 0xFFF)) = m_Vect[del].s8(15 - ((element + i + 1) & 15));
    }
}

void RSPOp::SWV() {
    uint32_t addr = static_cast<uint32_t>(m_GPR[m_OpCode.base].W + (m_OpCode.voffset << 4)) & 0xFFF;
    uint8_t offset = addr & 7;
    addr &= 0xFF8;
    for (uint8_t i = m_OpCode.del, n = static_cast<uint8_t>(16 + m_OpCode.del); i < n; i++, offset++) {
        *(m_DMEM + (((addr + (offset & 0xF)) ^ 3) & 0xFFF)) = m_Vect[m_OpCode.vt].s8(15 - (i & 0xF));
    }
}

// --- Utility Functions ---

void RSPOp::UnknownOpcode() {
    if (g_RSPDebugger != nullptr) {
        g_RSPDebugger->UnknownOpcode();
    }
}

uint32_t RSPOp::BranchIf(bool condition) {
    return (*m_SP_PC_REG + 4 + (condition ? (static_cast<int16_t>(m_OpCode.offset) << 2) : 4)) & 0xFFC;
}

} // namespace Nova64::RSP
