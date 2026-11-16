/**
 * @file InterpreterOps.hpp
 * @brief MIPS R4300i Interpreter for the N64 emulator
 *
 * Provides software interpretation of MIPS R4300i instructions.
 * This is the fallback execution path when dynamic recompilation
 * is not available or for debugging purposes.
 *
 * Based on Project64's interpreter implementation.
 */

#pragma once

#include "../MIPSY/R4300Iops.hpp"
#include "../MIPSY/Register.hpp"
#include "../Types.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cfenv>
#include <cfloat>
#include <array>
#include <functional>

class CN64System;
namespace N64System {
class CTLB;
class SystemTimer;
} // namespace N64System
namespace Nova64 {
class MipsMemoryVM;
}
class CSystemEvents;

namespace Nova64 {
namespace Interpreter {

/**
 * @brief R4300i Interpreter class
 *
 * Provides instruction-by-instruction interpretation of MIPS R4300i code.
 * Used as fallback when recompilation isn't possible and for debugging.
 */
class R4300iInterpreter {
public:
    using OpcodeFunc = void (R4300iInterpreter::*)();

    /**
     * @brief Construct the interpreter
     * @param system Reference to the N64 system
     * @param force32bit If true, use 32-bit optimized paths
     */
    R4300iInterpreter(CN64System& system, bool force32bit = false);

    ~R4300iInterpreter();

    R4300iInterpreter(const R4300iInterpreter&) = delete;
    R4300iInterpreter& operator=(const R4300iInterpreter&) = delete;

    /**
     * @brief Execute CPU until stopped
     */
    void ExecuteCPU();

    /**
     * @brief Execute a specific number of cycles
     * @param cycles Number of cycles to execute
     */
    void ExecuteOps(uint32_t cycles);

    /**
     * @brief Handle permanent loop (idle loop optimization)
     */
    void InPermLoop();

    /**
     * @brief Get the current opcode
     * @return Current instruction opcode
     */
    [[nodiscard]] R4300iOps Opcode() const noexcept { return m_Opcode; }

private:
    /**
     * @brief Build the instruction dispatch tables
     * @param force32bit If true, use 32-bit optimized handlers
     */
    void BuildInterpreter(bool force32bit);

    // --- Opcode Dispatch Functions ---

    void SPECIAL();
    void REGIMM();
    void COP0();
    void COP0_CO();
    void COP1();
    void COP2();
    void COP3();
    void COP1_BC();
    void COP1_S();
    void COP1_D();
    void COP1_W();
    void COP1_L();

    // --- Primary Opcode Functions ---

    void J();
    void JAL();
    void BNE();
    void BEQ();
    void BLEZ();
    void BGTZ();
    void ADDI();
    void ADDIU();
    void SLTI();
    void SLTIU();
    void ANDI();
    void ORI();
    void XORI();
    void LUI();
    void BEQL();
    void BNEL();
    void BLEZL();
    void BGTZL();
    void DADDI();
    void DADDIU();

    // --- Load Instructions (64-bit and 32-bit versions) ---

    void LDL();
    void LDR();
    void LB();
    void LB_32();
    void LH();
    void LH_32();
    void LWL();
    void LWL_32();
    void LW();
    void LW_32();
    void LBU();
    void LBU_32();
    void LHU();
    void LHU_32();
    void LWR();
    void LWR_32();
    void LWU();
    void LWU_32();
    void LL();
    void LL_32();
    void LWC1();
    void LWC1_32();
    void LLD();
    void LLD_32();
    void LDC1();
    void LDC1_32();
    void LD();
    void LD_32();

    // --- Store Instructions (64-bit and 32-bit versions) ---

    void SB();
    void SB_32();
    void SH();
    void SH_32();
    void SWL();
    void SWL_32();
    void SW();
    void SW_32();
    void SDL();
    void SDL_32();
    void SDR();
    void SDR_32();
    void SWR();
    void SWR_32();
    void SC();
    void SC_32();
    void SCD();
    void SCD_32();
    void SWC1();
    void SWC1_32();
    void SDC1();
    void SDC1_32();
    void SD();
    void SD_32();

    // --- Cache Operations ---

    void CACHE();

    // --- R4300i SPECIAL Opcodes ---

    void SPECIAL_SLL();
    void SPECIAL_SRL();
    void SPECIAL_SRA();
    void SPECIAL_SLLV();
    void SPECIAL_SRLV();
    void SPECIAL_SRAV();
    void SPECIAL_JR();
    void SPECIAL_JALR();
    void SPECIAL_SYSCALL();
    void SPECIAL_BREAK();
    void SPECIAL_SYNC();
    void SPECIAL_MFHI();
    void SPECIAL_MTHI();
    void SPECIAL_MFLO();
    void SPECIAL_MTLO();
    void SPECIAL_DSLLV();
    void SPECIAL_DSRLV();
    void SPECIAL_DSRAV();
    void SPECIAL_MULT();
    void SPECIAL_MULTU();
    void SPECIAL_DIV();
    void SPECIAL_DIVU();
    void SPECIAL_DMULT();
    void SPECIAL_DMULTU();
    void SPECIAL_DDIV();
    void SPECIAL_DDIVU();
    void SPECIAL_ADD();
    void SPECIAL_ADDU();
    void SPECIAL_SUB();
    void SPECIAL_SUBU();
    void SPECIAL_AND();
    void SPECIAL_OR();
    void SPECIAL_XOR();
    void SPECIAL_NOR();
    void SPECIAL_SLT();
    void SPECIAL_SLTU();
    void SPECIAL_DADD();
    void SPECIAL_DADDU();
    void SPECIAL_DSUB();
    void SPECIAL_DSUBU();
    void SPECIAL_TGE();
    void SPECIAL_TGEU();
    void SPECIAL_TLT();
    void SPECIAL_TLTU();
    void SPECIAL_TEQ();
    void SPECIAL_TNE();
    void SPECIAL_DSLL();
    void SPECIAL_DSRL();
    void SPECIAL_DSRA();
    void SPECIAL_DSLL32();
    void SPECIAL_DSRL32();
    void SPECIAL_DSRA32();

    // --- R4300i REGIMM Opcodes ---

    void REGIMM_BLTZ();
    void REGIMM_BGEZ();
    void REGIMM_BLTZL();
    void REGIMM_BGEZL();
    void REGIMM_BLTZAL();
    void REGIMM_BGEZAL();
    void REGIMM_BLTZALL();
    void REGIMM_BGEZALL();
    void REGIMM_TEQI();
    void REGIMM_TGEI();
    void REGIMM_TGEIU();
    void REGIMM_TLTI();
    void REGIMM_TLTIU();
    void REGIMM_TNEI();

    // --- COP0 Functions ---

    void COP0_MF();
    void COP0_DMF();
    void COP0_MT();
    void COP0_DMT();

    // COP0 CO Functions
    void COP0_CO_TLBR();
    void COP0_CO_TLBWI();
    void COP0_CO_TLBWR();
    void COP0_CO_TLBP();
    void COP0_CO_ERET();
    void COP0_CO_WAIT();

    // --- COP1 Functions ---

    void CPO1_UNIMPLEMENTED_OP();
    void COP1_MF();
    void COP1_DMF();
    void COP1_CF();
    void COP1_MT();
    void COP1_DMT();
    void COP1_CT();

    // COP1 BC Functions
    void COP1_BCF();
    void COP1_BCT();
    void COP1_BCFL();
    void COP1_BCTL();

    // --- COP1 Single Precision Functions ---

    void COP1_S_ADD();
    void COP1_S_SUB();
    void COP1_S_MUL();
    void COP1_S_DIV();
    void COP1_S_SQRT();
    void COP1_S_ABS();
    void COP1_S_MOV();
    void COP1_S_NEG();
    void COP1_S_ROUND_L();
    void COP1_S_TRUNC_L();
    void COP1_S_CEIL_L();
    void COP1_S_FLOOR_L();
    void COP1_S_ROUND_W();
    void COP1_S_TRUNC_W();
    void COP1_S_CEIL_W();
    void COP1_S_FLOOR_W();
    void COP1_S_CVT_D();
    void COP1_S_CVT_W();
    void COP1_S_CVT_L();
    void COP1_S_CMP();

    // --- COP1 Double Precision Functions ---

    void COP1_D_ADD();
    void COP1_D_SUB();
    void COP1_D_MUL();
    void COP1_D_DIV();
    void COP1_D_SQRT();
    void COP1_D_ABS();
    void COP1_D_MOV();
    void COP1_D_NEG();
    void COP1_D_ROUND_L();
    void COP1_D_TRUNC_L();
    void COP1_D_CEIL_L();
    void COP1_D_FLOOR_L();
    void COP1_D_ROUND_W();
    void COP1_D_TRUNC_W();
    void COP1_D_CEIL_W();
    void COP1_D_FLOOR_W();
    void COP1_D_CVT_S();
    void COP1_D_CVT_W();
    void COP1_D_CVT_L();
    void COP1_D_CMP();

    // --- COP1 Word Functions ---

    void COP1_W_CVT_S();
    void COP1_W_CVT_D();

    // --- COP1 Long Functions ---

    void COP1_L_CVT_S();
    void COP1_L_CVT_D();

    // --- COP2 Functions ---

    void CPO2_INVALID_OP();
    void COP2_MF();
    void COP2_DMF();
    void COP2_CF();
    void COP2_MT();
    void COP2_DMT();
    void COP2_CT();

    // --- Error Handling ---

    void ReservedInstruction();
    void UnknownOpcode();

    // --- FPU Helper Functions ---

    bool TestCop1UsableException();
    bool CheckFPUInput32(const uint32_t& value);
    bool CheckFPUInputs32(const uint32_t& value, const uint32_t& value2);
    bool CheckFPUInput32Conv(const float& value);
    bool CheckFPUInput64(const double& value);
    bool CheckFPUInputs64(const double& value, const double& value2);
    bool CheckFPUInput64Conv(const double& value);
    bool CheckFPUResult32(float& result);
    bool CheckFPUResult64(double& result);
    bool CheckFPUInvalidException();
    bool InitFpuOperation(unsigned int roundingModel);
    bool SetFPUException();
    void UpdateInstructionMemory();

    // Opcode field access helpers
    // These adapt the R4300iOps union fields to flat-style access
    // matching Project64's m_Opcode.rs, m_Opcode.rt, etc.

    // Note: Itype and Jtype fields are correct. Rtype/Ftype/Memtype have
    // incorrect field naming (bit positions don't match MIPS standard).
    // These helpers extract the correct bits using verified-correct structs
    // or direct bit extraction from Value.
    inline uint32_t op() const { return m_Opcode.Itype.op; }                                     // bits 31:26
    inline uint32_t rs() const { return m_Opcode.Itype.rs; }                                     // bits 25:21
    inline uint32_t rt() const { return m_Opcode.Itype.rt; }                                     // bits 20:16
    inline uint32_t rd() const { return (m_Opcode.Value >> 11) & 0x1F; }                         // bits 15:11
    inline uint32_t sa() const { return (m_Opcode.Value >> 6) & 0x1F; }                          // bits 10:6
    inline uint32_t funct() const { return m_Opcode.Value & 0x3F; }                              // bits 5:0
    inline uint32_t imm() const { return m_Opcode.Itype.imm; }                                   // bits 15:0
    inline int16_t offset() const { return static_cast<int16_t>(m_Opcode.Value & 0xFFFF); }      // bits 15:0 signed
    inline uint32_t target() const { return m_Opcode.Jtype.target; }                             // bits 25:0
    inline uint16_t immediate() const { return static_cast<uint16_t>(m_Opcode.Value & 0xFFFF); } // bits 15:0
    inline uint32_t base() const { return m_Opcode.Itype.rs; }                                   // bits 25:21 = rs
    inline uint32_t ft() const { return m_Opcode.Itype.rt; }                                     // bits 20:16 = rt
    inline uint32_t fs() const { return (m_Opcode.Value >> 11) & 0x1F; }                         // bits 15:11 = rd
    inline uint32_t fd() const { return (m_Opcode.Value >> 6) & 0x1F; }                          // bits 10:6  = sa
    inline uint32_t fmt() const { return m_Opcode.Itype.rs; }                                    // bits 25:21 = rs

    // System references
    CN64System& m_System;
    CRegisters& m_Reg;
    N64System::CTLB& m_TLB;
    MipsMemoryVM& m_MMU;

    // Current instruction
    R4300iOps m_Opcode;

    // Register references (for fast access)
    uint64_t& m_PROGRAM_COUNTER;
    N64Types::MIPS_DWORD* m_GPR;
    N64Types::MIPS_DWORD* m_FPR;
    uint64_t* m_CP0;
    N64Types::MIPS_DWORD& m_RegHI;
    N64Types::MIPS_DWORD& m_RegLO;

    // FPU register access pointers
    uint32_t** m_FPR_UW;
    uint64_t** m_FPR_UDW;
    float** m_FPR_S;
    float** m_FPR_S_L;
    double** m_FPR_D;
    uint32_t* m_FPCR;
    uint32_t& m_LLBit;

    // Instruction memory cache
    uint64_t m_InstructionRegion;
    uint8_t* m_InstructionMemory;
    uint32_t* m_InstructionPtr;

    // --- Dispatch Tables ---

    OpcodeFunc Jump_Opcode[64];
    OpcodeFunc Jump_Special[64];
    OpcodeFunc Jump_Regimm[32];
    OpcodeFunc Jump_CoP0[32];
    OpcodeFunc Jump_CoP0_Function[64];
    OpcodeFunc Jump_CoP1[32];
    OpcodeFunc Jump_CoP1_BC[32];
    OpcodeFunc Jump_CoP1_S[64];
    OpcodeFunc Jump_CoP1_D[64];
    OpcodeFunc Jump_CoP1_W[64];
    OpcodeFunc Jump_CoP1_L[64];
    OpcodeFunc Jump_CoP2[32];

    // --- Static Data - Shift/mask tables for unaligned memory access ---

    static const uint32_t SWL_MASK[4];
    static const uint32_t SWR_MASK[4];
    static const uint32_t LWL_MASK[4];
    static const uint32_t LWR_MASK[4];
    static const int32_t SWL_SHIFT[4];
    static const int32_t SWR_SHIFT[4];
    static const int32_t LWL_SHIFT[4];
    static const int32_t LWR_SHIFT[4];
};

} // namespace Interpreter
} // namespace Nova64
