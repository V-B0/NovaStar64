/**
 * @file RspRecompilerOps.hpp
 * @brief RSP Recompiler Operations.
 */
// SuperNova64 - N64 Emulator
// RSP Recompiler Operations - Modernized from Project64's RspRecompilerOps-x64.h
// x64 code generation for all RSP opcodes

#pragma once
#if defined(__amd64__) || defined(_M_X64)

#include "RspRegState.hpp"
#include "../CPU/RSPInterpreterOps.hpp"
#include "../CPU/RspPipelineStage.hpp"
#include <cstdint>

namespace Nova64::RSP {
class CRSPSystem;
class CRSPRegisters;
}

namespace Nova64::RSP::Recompiler {

class CRSPRecompiler;
class RspAssembler;
class RspCodeBlock;

// ──────────────────────────────────────────────
// Accumulator access location for optimization
// ──────────────────────────────────────────────
enum class AccumLocation : uint8_t {
    High,
    Middle,
    Low,
    Entire,
};

// ──────────────────────────────────────────────
// RSP Recompiler Operations
// Emits x64 code for each RSP opcode
// ──────────────────────────────────────────────
class CRSPRecompilerOps {
    friend CRspRegState;

    static constexpr uint32_t FunctionStackSize = 40;

public:
    CRSPRecompilerOps(CRSPSystem& system, CRSPRecompiler& recompiler);
    ~CRSPRecompilerOps() = default;

    CRSPRecompilerOps(const CRSPRecompilerOps&) = delete;
    CRSPRecompilerOps& operator=(const CRSPRecompilerOps&) = delete;

    // ── Primary opcode codegen ──
    void SPECIAL();  void REGIMM();  void J();     void JAL();
    void BEQ();      void BNE();     void BLEZ();  void BGTZ();
    void ADDI();     void ADDIU();   void SLTI();  void SLTIU();
    void ANDI();     void ORI();     void XORI();  void LUI();
    void COP0();     void COP2();
    void LB();       void LH();     void LW();
    void LBU();      void LHU();    void LWU();
    void SB();       void SH();     void SW();
    void LC2();      void SC2();

    // ── SPECIAL codegen ──
    void Special_SLL();   void Special_SRL();  void Special_SRA();
    void Special_SLLV();  void Special_SRLV(); void Special_SRAV();
    void Special_JR();    void Special_JALR(); void Special_BREAK();
    void Special_ADD();   void Special_ADDU(); void Special_SUB();
    void Special_SUBU();  void Special_AND();  void Special_OR();
    void Special_XOR();   void Special_NOR();
    void Special_SLT();   void Special_SLTU();

    // ── RegImm codegen ──
    void RegImm_BLTZ();   void RegImm_BGEZ();
    void RegImm_BLTZAL(); void RegImm_BGEZAL();

    // ── COP0 codegen ──
    void Cop0_MF(); void Cop0_MT();

    // ── COP2 codegen ──
    void Cop2_MF(); void Cop2_CF();
    void Cop2_MT(); void Cop2_CT();
    void COP2_VECTOR();

    // ── Vector codegen ──
    void Vector_VMULF();  void Vector_VMULU();  void Vector_VRNDP();
    void Vector_VRNDN();  void Vector_VMULQ();
    void Vector_VMUDL();  void Vector_VMUDM();
    void Vector_VMUDN();  void Vector_VMUDH();
    void Vector_VMACF();  void Vector_VMACU();  void Vector_VMACQ();
    void Vector_VMADL();  void Vector_VMADM();
    void Vector_VMADN();  void Vector_VMADH();
    void Vector_VADD();   void Vector_VSUB();   void Vector_VABS();
    void Vector_VADDC();  void Vector_VSUBC();
    void Vector_VSAW();
    void Vector_VLT();    void Vector_VEQ();    void Vector_VNE();
    void Vector_VGE();    void Vector_VCL();    void Vector_VCH();
    void Vector_VCR();    void Vector_VMRG();
    void Vector_VAND();   void Vector_VNAND();  void Vector_VOR();
    void Vector_VNOR();   void Vector_VXOR();   void Vector_VNXOR();
    void Vector_VRCP();   void Vector_VRCPL();  void Vector_VRCPH();
    void Vector_VMOV();   void Vector_VRSQ();   void Vector_VRSQL();
    void Vector_VRSQH();  void Vector_VNOOP();  void Vector_Reserved();

    // ── Vector Load/Store codegen ──
    void Opcode_LBV(); void Opcode_LSV(); void Opcode_LLV(); void Opcode_LDV();
    void Opcode_LQV(); void Opcode_LRV(); void Opcode_LPV(); void Opcode_LUV();
    void Opcode_LHV(); void Opcode_LFV(); void Opcode_LWV(); void Opcode_LTV();

    void Opcode_SBV(); void Opcode_SSV(); void Opcode_SLV(); void Opcode_SDV();
    void Opcode_SQV(); void Opcode_SRV(); void Opcode_SPV(); void Opcode_SUV();
    void Opcode_SHV(); void Opcode_SFV(); void Opcode_SWV(); void Opcode_STV();

    // ── Utility ──
    void UnknownOpcode();
    bool RSP_DoSections();

    // ── Block entry/exit ──
    void EnterCodeBlock();
    void ExitCodeBlock();

private:
    void LoadVectorRegister(uint8_t xmmReg, uint8_t vectorReg, uint8_t e);
    void Cheat_r4300iOpcode(void (RSPOp::*funcAddress)(), const char* funcName, bool commentOp = true);
    bool WriteToVectorDest(uint32_t destReg, uint32_t pc);
    bool WriteToAccum(AccumLocation location, uint32_t pc);
    uint32_t GprOffset(uint8_t gpReg) const;
    uint32_t VectorOffset(uint8_t vectorReg) const;
    uint32_t AccumOffset(AccumLocation location) const;
    uint32_t FlagOffset(RspFlags flag) const;

    // ── MAC operation helpers (shared by VMACF/VMADH/VMADM etc.) ──

    // Emit signed saturation of accumulator hi:mid → result in xmmMid.
    // Given acc_hi and acc_mid, produces:
    //   if hi == sign_extend(mid): result = mid (in range)
    //   if hi > 0 (positive overflow): result = 0x7FFF
    //   if hi < 0 (negative overflow): result = 0x8000
    // Allocates ~4 temp XMM registers. xmmMid is modified in-place.
    void EmitSignedSaturation(uint8_t xmmMid, uint8_t xmmAccHi);

    // Emit unsigned carry detection: returns XMM register containing carry mask.
    // carry = (oldVal > newVal) unsigned, using XOR 0x8000 + pcmpgtw trick.
    // xmmOld and xmmNew are consumed (modified). xmmConst8000 must hold 0x8000.
    // Returns the XMM register holding the carry mask (all-ones per lane where carry).
    uint8_t EmitUnsignedCarryDetect(uint8_t xmmOld, uint8_t xmmNew, uint8_t xmmConst8000);

    // System references
    CRSPSystem&         m_System;
    CRSPRecompiler&     m_Recompiler;
    RSPOpcode&          m_OpCode;
    const uint32_t&     m_CompilePC;
    const RspCodeBlock*& m_CurrentBlock;
    PipelineStage&      m_NextInstruction;
    uint8_t*&           m_DMEM;
    CRSPRegisters&      m_Reg;

    // Register shortcuts
    Word32*          m_GPR{nullptr};
    RSPVector*       m_Vect{nullptr};
    RSPAccumulator&  m_ACCUM;
    RSPFlag&         m_VCOL;
    RSPFlag&         m_VCOH;
    RSPFlag&         m_VCCL;
    RSPFlag&         m_VCCH;
    RSPFlag&         m_VCE;

    // Assembler and reg state
    RspAssembler*&   m_Assembler;
    CRspRegState&    m_RegState;
    bool             m_DelayAffectBranch{false};
};

using RecompFunc = void (CRSPRecompilerOps::*)();

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
