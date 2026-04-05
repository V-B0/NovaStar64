/**
 * @file RSPInterpreterOps.hpp
 * @brief RSP Interpreter Operations.
 */
// SuperNova64 - N64 Emulator
// RSP Interpreter Operations - Modernized from Project64's RSPInterpreterOps.h
// C++20 with function pointer dispatch tables and clean member references

#pragma once
#include "RSPOpcode.hpp"
#include "RspPipelineStage.hpp"
#include "RspTypes.hpp"
#include <cstdint>
#include <memory>

namespace Nova64::RSP {

class CRSPSystem;
class CRSPRegisters;
class RSPRegisterHandlerPlugin;

namespace Recompiler { class CRSPRecompilerOps; }
namespace HLE { class CHleTask; }

// ──────────────────────────────────────────────
// RSP Interpreter - Executes all RSP opcodes
// Function pointer tables for O(1) opcode dispatch
// ──────────────────────────────────────────────
class RSPOp {
    friend class CRSPSystem;
    friend class Recompiler::CRSPRecompilerOps;
    friend class HLE::CHleTask;

public:
    explicit RSPOp(CRSPSystem& system);
    ~RSPOp();

    RSPOp(const RSPOp&) = delete;
    RSPOp& operator=(const RSPOp&) = delete;

private:
    RSPOp() = delete;

    void BuildInterpreter();

    using Func = void (RSPOp::*)();

    // ── Primary opcode handlers ──
    void SPECIAL();  void REGIMM();  void J();     void JAL();
    void BEQ();      void BNE();     void BLEZ();  void BGTZ();
    void ADDI();     void ADDIU();   void SLTI();  void SLTIU();
    void ANDI();     void ORI();     void XORI();  void LUI();
    void COP0();     void COP2();
    void LB();       void LH();     void LW();
    void LBU();      void LHU();    void LWU();
    void SB();       void SH();     void SW();
    void LC2();      void SC2();

    // ── SPECIAL function handlers ──
    void Special_SLL();   void Special_SRL();  void Special_SRA();
    void Special_SLLV();  void Special_SRLV(); void Special_SRAV();
    void Special_JR();    void Special_JALR(); void Special_BREAK();
    void Special_ADD();   void Special_ADDU(); void Special_SUB();
    void Special_SUBU();  void Special_AND();  void Special_OR();
    void Special_XOR();   void Special_NOR();
    void Special_SLT();   void Special_SLTU();

    // ── RegImm handlers ──
    void BLTZ();    void BGEZ();
    void BLTZAL();  void BGEZAL();

    // ── COP0 handlers ──
    void Cop0_MF(); void Cop0_MT();

    // ── COP2 handlers (scalar ↔ vector moves) ──
    void Cop2_MF(); void Cop2_CF();
    void Cop2_MT(); void Cop2_CT();
    void Cop2_VECTOR();

    // ── Vector computational handlers ──
    void Vector_VMULF();  void Vector_VMULU();  void Vector_VRNDP();
    void Vector_VMULQ();  void Vector_VMUDL();  void Vector_VMUDM();
    void Vector_VMUDN();  void Vector_VMUDH();
    void Vector_VMACF();  void Vector_VMACU();  void Vector_VMACQ();
    void Vector_VRNDN();  void Vector_VMADL();  void Vector_VMADM();
    void Vector_VMADN();  void Vector_VMADH();
    void Vector_VADD();   void Vector_VSUB();   void Vector_VABS();
    void Vector_VADDC();  void Vector_VSUBC();  void Vector_Reserved();
    void Vector_VSAW();
    void Vector_VLT();    void Vector_VEQ();    void Vector_VNE();
    void Vector_VGE();    void Vector_VCL();    void Vector_VCH();
    void Vector_VCR();    void Vector_VMRG();
    void Vector_VAND();   void Vector_VNAND();  void Vector_VOR();
    void Vector_VNOR();   void Vector_VXOR();   void Vector_VNXOR();
    void Vector_VRCP();   void Vector_VRCPL();  void Vector_VRCPH();
    void Vector_VMOV();   void Vector_VRSQ();   void Vector_VRSQL();
    void Vector_VRSQH();  void Vector_VNOOP();

    // ── LC2 (vector load) handlers ──
    void LBV(); void LSV(); void LLV(); void LDV();
    void LQV(); void LRV(); void LPV(); void LUV();
    void LHV(); void LFV(); void LWV(); void LTV();

    // ── SC2 (vector store) handlers ──
    void SBV(); void SSV(); void SLV(); void SDV();
    void SQV(); void SRV(); void SPV(); void SUV();
    void SHV(); void SFV(); void STV(); void SWV();

    // ── Utility ──
    void UnknownOpcode();
    uint32_t BranchIf(bool condition);

    // ── Dispatch tables ──
    Func Jump_Opcode[64]{};
    Func Jump_RegImm[32]{};
    Func Jump_Special[64]{};
    Func Jump_Cop0[32]{};
    Func Jump_Cop2[32]{};
    Func Jump_Vector[64]{};
    Func Jump_Lc2[32]{};
    Func Jump_Sc2[32]{};

    // ── System references ──
    CRSPSystem&                m_System;
    std::unique_ptr<RSPRegisterHandlerPlugin>& m_RSPRegisterHandler;
    RSPOpcode&                 m_OpCode;
    CRSPRegisters&             m_Reg;
    PipelineStage&             m_NextInstruction;
    uint32_t&                  m_JumpTo;

    // Hardware register pointers
    uint32_t*& m_MI_INTR_REG;
    uint32_t*& m_SP_PC_REG;
    uint32_t*& m_SP_STATUS_REG;
    uint32_t*& m_SP_DMA_FULL_REG;
    uint32_t*& m_SP_DMA_BUSY_REG;
    uint32_t*& m_SP_SEMAPHORE_REG;
    uint32_t*& m_DPC_START_REG;
    uint32_t*& m_DPC_END_REG;
    uint32_t*& m_DPC_CURRENT_REG;
    uint32_t*& m_DPC_STATUS_REG;
    uint32_t*& m_DPC_CLOCK_REG;

    // Memory pointers
    uint8_t*& m_DMEM;

    // Register shortcuts (set during BuildInterpreter)
    Word32*          m_GPR{nullptr};
    RSPAccumulator&  m_ACCUM;
    RSPVector*       m_Vect{nullptr};
    RSPFlag&         VCOL;
    RSPFlag&         VCOH;
    RSPFlag&         VCCL;
    RSPFlag&         VCCH;
    RSPFlag&         VCE;

    // Callbacks
    void (*&CheckInterrupts)();
    void (*&ProcessRdpList)();
};

} // namespace Nova64::RSP
