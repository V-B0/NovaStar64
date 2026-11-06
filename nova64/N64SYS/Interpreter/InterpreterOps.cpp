/**
 * @file InterpreterOps.cpp
 * @brief MIPS R4300i Interpreter implementation
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "InterpreterOps.hpp"
#include "../StateTrace.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/TLB.hpp"
#include "../MIPSY/Systiming.hpp"
#include "../MIPSY/Sysevents.hpp"
#include "../MIPSY/Register.hpp"
#include "../../common/log.hpp"
#include <fenv.h>
#include <float.h>
#include <array>
#include <string>
#include <math.h>
#include <cmath>   // std::isnan (replaces MSVC _isnan)
#include <cstdlib>

// Softfloat library for IEEE 754 compliant FPU emulation
extern "C"
{
#include "softfloat.h"
}

// Convenience aliases
using R4300iInterpreter = Nova64::Interpreter::R4300iInterpreter;
using MIPS_DWORD = N64Types::MIPS_DWORD;
using PipelineStage = N64Types::PipelineStage;

// Exception code shortcuts (ExceptionCode is enum class, but TriggerException takes uint32_t)
namespace {
    constexpr uint32_t EXC_RMISS   = static_cast<uint32_t>(ExceptionCode::EXC_RMISS);
    constexpr uint32_t EXC_WMISS   = static_cast<uint32_t>(ExceptionCode::EXC_WMISS);
    constexpr uint32_t EXC_RADE    = static_cast<uint32_t>(ExceptionCode::EXC_RADE);
    constexpr uint32_t EXC_SYSCALL = static_cast<uint32_t>(ExceptionCode::EXC_SYSCALL);
    constexpr uint32_t EXC_BREAK   = static_cast<uint32_t>(ExceptionCode::EXC_BREAK);
    constexpr uint32_t EXC_II      = static_cast<uint32_t>(ExceptionCode::EXC_II);
    constexpr uint32_t EXC_CPU     = static_cast<uint32_t>(ExceptionCode::EXC_CPU);
    constexpr uint32_t EXC_OV      = static_cast<uint32_t>(ExceptionCode::EXC_OV);
    constexpr uint32_t EXC_TRAP    = static_cast<uint32_t>(ExceptionCode::EXC_TRAP);
    constexpr uint32_t EXC_FPE     = static_cast<uint32_t>(ExceptionCode::EXC_FPE);

    
    constexpr auto PIPELINE_STAGE_NORMAL              = PipelineStage::Normal;
    constexpr auto PIPELINE_STAGE_DELAY_SLOT          = PipelineStage::DelaySlot;
    constexpr auto PIPELINE_STAGE_JUMP                = PipelineStage::Jump;
    constexpr auto PIPELINE_STAGE_JUMP_DELAY_SLOT     = PipelineStage::JumpDelaySlot;
    constexpr auto PIPELINE_STAGE_PERMLOOP_DO_DELAY   = PipelineStage::PermanentLoopDoDelay;
    constexpr auto PIPELINE_STAGE_PERMLOOP_DELAY_DONE = PipelineStage::PermanentLoopDelayDone;

    // FPCSR condition bit
    constexpr uint32_t FPCSR_C = FPCSRFlags::C;
}

// ============================================================================
// Static Data
// ============================================================================

const uint32_t R4300iInterpreter::SWL_MASK[4] = {0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00};
const uint32_t R4300iInterpreter::SWR_MASK[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000};
const uint32_t R4300iInterpreter::LWL_MASK[4] = {0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF};
const uint32_t R4300iInterpreter::LWR_MASK[4] = {0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000};

const int32_t R4300iInterpreter::SWL_SHIFT[4] = {0, 8, 16, 24};
const int32_t R4300iInterpreter::SWR_SHIFT[4] = {24, 16, 8, 0};
const int32_t R4300iInterpreter::LWL_SHIFT[4] = {0, 8, 16, 24};
const int32_t R4300iInterpreter::LWR_SHIFT[4] = {24, 16, 8, 0};

// ============================================================================
// Constructor / Destructor
// ============================================================================

R4300iInterpreter::R4300iInterpreter(CN64System& system, bool force32bit) :
    m_System(system),
    m_Reg(*system.m_Registers),
    m_TLB(*system.m_TLB),
    m_MMU(*system.m_Memory),
    m_PROGRAM_COUNTER(system.m_Registers->m_PROGRAM_COUNTER),
    m_GPR(reinterpret_cast<MIPS_DWORD*>(system.m_Registers->m_GPR.data())),
    m_FPR(reinterpret_cast<MIPS_DWORD*>(system.m_Registers->m_FPR.data())),
    m_CP0(system.m_Registers->m_CP0.data()),
    m_RegHI(reinterpret_cast<MIPS_DWORD&>(system.m_Registers->m_HI)),
    m_RegLO(reinterpret_cast<MIPS_DWORD&>(system.m_Registers->m_LO)),
    m_FPR_UW(nullptr),
    m_FPR_UDW(nullptr),
    m_FPR_S(nullptr),
    m_FPR_S_L(nullptr),
    m_FPR_D(nullptr),
    m_FPCR(system.m_Registers->m_FPCR.data()),
    m_LLBit(system.m_Registers->m_LLBit),
    m_InstructionRegion(0),
    m_InstructionMemory(nullptr),
    m_InstructionPtr(nullptr)
{
    m_Opcode.Value = 0;

    // Setup FPR access pointers into the m_FPR backing array (uint64_t[32]).
    // In FR=0 (32-bit FPU mode, the default for N64 games):
    //   - Each pair of FPRs (F0:F1, F2:F3, ..., F30:F31) shares one 64-bit slot
    //   - Single-precision F[i] occupies one 32-bit half of m_FPR[i & ~1]
    //   - Double-precision F[i] (even only) occupies all 64 bits of m_FPR[i]
    //   - On little-endian host: F[even] = low 32 bits, F[odd] = high 32 bits
    // In FR=1 (64-bit FPU mode):
    //   - Each FPR is independent 64-bit
    // We set up pointers assuming FR=0 (most common) and call FixFpuLocations 
    // when FR changes.
    static uint32_t* s_FPR_UW[32];
    static uint64_t* s_FPR_UDW[32];
    static float*    s_FPR_S[32];
    static float*    s_FPR_S_L[32];
    static double*   s_FPR_D[32];

    // Use m_FPR (uint64_t[32]) as the backing store for all FP operations
    for (int i = 0; i < 32; i++)
    {
        // For double and uint64: always point to the even register's 64-bit slot
        s_FPR_UDW[i] = (uint64_t*)&system.m_Registers->m_FPR[i & ~1];
        s_FPR_D[i]   = (double*)&system.m_Registers->m_FPR[i & ~1];

        // For single/uint32: point to the correct 32-bit half within the pair
        // On little-endian x86: even registers = low 32 bits (offset 0),
        //                       odd registers = high 32 bits (offset 4)
        uint8_t* pairBase = (uint8_t*)&system.m_Registers->m_FPR[i & ~1];
        int halfOffset = (i & 1) ? 4 : 0;
        s_FPR_UW[i]  = (uint32_t*)(pairBase + halfOffset);
        s_FPR_S[i]   = (float*)(pairBase + halfOffset);
        s_FPR_S_L[i] = (float*)(pairBase + halfOffset);
    }
    m_FPR_UW  = s_FPR_UW;
    m_FPR_UDW = s_FPR_UDW;
    m_FPR_S   = s_FPR_S;
    m_FPR_S_L = s_FPR_S_L;
    m_FPR_D   = s_FPR_D;

    BuildInterpreter(force32bit);
}

R4300iInterpreter::~R4300iInterpreter()
{
}

// ============================================================================
// Execution
// ============================================================================

void R4300iInterpreter::ExecuteCPU()
{
    Log::info("=== R4300i Interpreter starting CPU execution ===");
    Log::infof("  PC = 0x%08X", (uint32_t)m_PROGRAM_COUNTER);
    ExecuteOps((uint32_t)-1);
    Log::info("=== R4300i Interpreter execution ended ===");
}

void R4300iInterpreter::InPermLoop()
{
    
    if ((m_Reg.STATUS_REGISTER.InterruptEnable) == 0 ||
        (m_Reg.STATUS_REGISTER.ExceptionLevel) != 0 ||
        (m_Reg.STATUS_REGISTER.ErrorLevel) != 0)
    {
        Log::infof("[PERM] Permanent loop detected at PC=0x%08X STATUS=0x%08X CAUSE=0x%08X MI_INTR=0x%08X MI_MASK=0x%08X IE=%u EXL=%u ERL=%u IM=0x%02X",
                   (uint32_t)m_PROGRAM_COUNTER, (uint32_t)m_Reg.STATUS_REGISTER.Value,
                   (uint32_t)m_Reg.CAUSE_REGISTER.Value,
                   m_Reg.m_Mips_Interface[2], m_Reg.m_Mips_Interface[3],
                   m_Reg.STATUS_REGISTER.InterruptEnable,
                   m_Reg.STATUS_REGISTER.ExceptionLevel,
                   m_Reg.STATUS_REGISTER.ErrorLevel,
                   m_Reg.STATUS_REGISTER.InterruptMask);
        m_System.m_EndEmulation = true;
    }
    else if (m_System.m_NextTimer > 0)
    {
        ++Nova64::Trace::Counters().idleSkip;
        Nova64::Trace::RecordEvent("idle", m_Reg.COUNT_REGISTER);
        // Fast-forward to next timer event (matches Project64 InPermLoop):
        // 1. Sync timer array with current m_NextTimer position
        m_System.m_SystemTimer->UpdateTimers();
        // 2. Force timer expiration by setting NextTimer negative.
        m_System.m_NextTimer = 0 - m_System.CountPerOp();
        // 3. Second UpdateTimers() flushes the final elapsed cycles into Count
        //    and the timer array before TimerDone() runs in PERMLOOP_DELAY_DONE.
        m_System.m_SystemTimer->UpdateTimers();
    }
}

void R4300iInterpreter::ExecuteOps(uint32_t Cycles)
{
    std::atomic<bool>& Done = m_System.m_EndEmulation;
    PipelineStage & CurrentPipelineStage = m_System.m_PipelineStage;
    uint64_t & JumpToLocation = m_System.m_JumpToLocation;
    uint64_t & JumpDelayLocation = m_System.m_JumpDelayLocation;
    bool & TestTimer = m_System.m_TestTimer;
    CSystemEvents & SystemEvents = *m_System.m_SystemEvents;
    uint32_t CountPerOp = m_System.CountPerOp();
    int32_t & NextTimer = m_System.m_NextTimer;
    bool CheckTimer = false;
    bool updateInstructionMemory = true;
    m_InstructionRegion = (uint64_t)-1;

    while (!Done && Cycles > 0)
    {
        if (updateInstructionMemory)
        {
            UpdateInstructionMemory();
            updateInstructionMemory = false;
        }
        m_Opcode.Value = *m_InstructionPtr;

        // HLE osViSwapBuffer intercept removed: the hardcoded addresses
        // (0x8000176C/0x80001770) only applied to one ROM build and corrupted
        // game state for other ROMs (including OoT v1.2).  The native MIPS
        // osViSwapBuffer implementation works correctly through the interpreter.

        (this->*Jump_Opcode[op()])();
        m_GPR[0].DW = 0; // MIPS $zero hard-wired to 0

        NextTimer -= CountPerOp;
        if (Cycles != (uint32_t)-1)
        {
            Cycles -= CountPerOp;
        }

        if (m_System.IsFrameLimitReached())
        {
            Done = true;
            break;
        }

        m_PROGRAM_COUNTER += 4;
        if ((((uint32_t)m_PROGRAM_COUNTER) & 0xFFFUL) == 0)
        {
            updateInstructionMemory = true;
        }
        m_InstructionPtr++;

        switch (CurrentPipelineStage)
        {
        case PIPELINE_STAGE_NORMAL:
            // Service the timer at the instruction boundary it actually expires
            // on, rather than deferring to the next backward branch. The
            // recompiler exits its block at this same boundary, so both engines
            // enter the ISR having executed exactly the same instructions --
            // which is what keeps their CP0 Compare values in step.
            //
            // Only in the NORMAL stage: a delay slot must not be separated from
            // its branch. DoIntrException additionally refuses to deliver while
            // ExceptionLevel is set, so the MTC0 Status -> ERET sequence stays
            // protected.
            if (NextTimer <= 0)
            {
                ++Nova64::Trace::Counters().timerChecks;
                if (NextTimer < 0)
                {
                    ++Nova64::Trace::Counters().timerDone;
                    Nova64::Trace::RecordEvent("done", m_Reg.COUNT_REGISTER);
                    m_System.m_SystemTimer->TimerDone();
                }
                if (SystemEvents.DoSomething())
                {
                    SystemEvents.ExecuteEvents();
                }
                if (CurrentPipelineStage == PIPELINE_STAGE_JUMP)
                {
                    m_PROGRAM_COUNTER = JumpToLocation;
                    CurrentPipelineStage = PIPELINE_STAGE_NORMAL;
                    updateInstructionMemory = true;
                }
            }
            break;
        case PIPELINE_STAGE_DELAY_SLOT:
            CurrentPipelineStage = PIPELINE_STAGE_JUMP;
            break;
        case PIPELINE_STAGE_PERMLOOP_DO_DELAY:
            CurrentPipelineStage = PIPELINE_STAGE_PERMLOOP_DELAY_DONE;
            break;
        case PIPELINE_STAGE_JUMP:
            // The NORMAL stage above already services the timer at the exact
            // instruction it expires on, so this only needs to cover control
            // transfers (branch/delay-slot pairs), where that check is skipped.
            CheckTimer = true;
            m_PROGRAM_COUNTER = JumpToLocation;
            CurrentPipelineStage = PIPELINE_STAGE_NORMAL;
            if ((m_PROGRAM_COUNTER & 0x3) != 0)
            {
                m_Reg.DoAddressError((int32_t)JumpToLocation, true);
                m_PROGRAM_COUNTER = JumpToLocation;
                CurrentPipelineStage = PIPELINE_STAGE_NORMAL;
            }
            else if (CheckTimer)
            {
                TestTimer = false;
                ++Nova64::Trace::Counters().timerChecks;
                if (NextTimer < 0)
                {
                    ++Nova64::Trace::Counters().timerDone;
                    Nova64::Trace::RecordEvent("done", m_Reg.COUNT_REGISTER);
                    m_System.m_SystemTimer->TimerDone();
                }
                if (SystemEvents.DoSomething())
                {
                    SystemEvents.ExecuteEvents();
                }
                // If an exception (e.g. interrupt) changed the pipeline to JUMP,
                // apply the exception vector jump immediately without executing
                // another instruction at the current PC (which would overwrite it).
                if (CurrentPipelineStage == PIPELINE_STAGE_JUMP)
                {
                    m_PROGRAM_COUNTER = JumpToLocation;
                    CurrentPipelineStage = PIPELINE_STAGE_NORMAL;
                }
            }
            updateInstructionMemory = true;
            break;
        case PIPELINE_STAGE_JUMP_DELAY_SLOT:
            CurrentPipelineStage = PIPELINE_STAGE_JUMP;
            m_PROGRAM_COUNTER = JumpToLocation;
            JumpToLocation = JumpDelayLocation;
            updateInstructionMemory = true;
            break;
        case PIPELINE_STAGE_PERMLOOP_DELAY_DONE:
            m_PROGRAM_COUNTER = JumpToLocation;
            CurrentPipelineStage = PIPELINE_STAGE_NORMAL;
            InPermLoop();
            ++Nova64::Trace::Counters().timerDone;
            Nova64::Trace::RecordEvent("perm", m_Reg.COUNT_REGISTER);
            m_System.m_SystemTimer->TimerDone();
            if (SystemEvents.DoSomething())
            {
                SystemEvents.ExecuteEvents();
            }
            // If an exception redirected to the exception vector, apply immediately.
            if (CurrentPipelineStage == PIPELINE_STAGE_JUMP)
            {
                m_PROGRAM_COUNTER = JumpToLocation;
                CurrentPipelineStage = PIPELINE_STAGE_NORMAL;
            }
            updateInstructionMemory = true;
            break;
        default:
            break;
        }
    }
    m_System.m_SystemTimer->UpdateTimers();
}

// ============================================================================
// Dispatch Functions
// ============================================================================

void R4300iInterpreter::SPECIAL()
{
    (this->*Jump_Special[funct()])();
}

void R4300iInterpreter::REGIMM()
{
    (this->*Jump_Regimm[rt()])();
}

void R4300iInterpreter::COP0()
{
    (this->*Jump_CoP0[rs()])();
}

void R4300iInterpreter::COP0_CO()
{
    (this->*Jump_CoP0_Function[funct()])();
}

void R4300iInterpreter::COP1()
{
    (this->*Jump_CoP1[fmt()])();
}

void R4300iInterpreter::COP2()
{
    (this->*Jump_CoP2[fmt()])();
}

void R4300iInterpreter::COP3()
{
    m_Reg.TriggerException(EXC_II);
}

void R4300iInterpreter::COP1_BC()
{
    (this->*Jump_CoP1_BC[ft()])();
}

void R4300iInterpreter::COP1_S()
{
    (this->*Jump_CoP1_S[funct()])();
}

void R4300iInterpreter::COP1_D()
{
    (this->*Jump_CoP1_D[funct()])();
}

void R4300iInterpreter::COP1_W()
{
    (this->*Jump_CoP1_W[funct()])();
}

void R4300iInterpreter::COP1_L()
{
    (this->*Jump_CoP1_L[funct()])();
}

// ============================================================================
// Build Interpreter (Dispatch Tables)
// ============================================================================

void R4300iInterpreter::BuildInterpreter(bool Force32bit)
{
    Jump_Opcode[0] = &R4300iInterpreter::SPECIAL;
    Jump_Opcode[1] = &R4300iInterpreter::REGIMM;
    Jump_Opcode[2] = &R4300iInterpreter::J;
    Jump_Opcode[3] = &R4300iInterpreter::JAL;
    Jump_Opcode[4] = &R4300iInterpreter::BEQ;
    Jump_Opcode[5] = &R4300iInterpreter::BNE;
    Jump_Opcode[6] = &R4300iInterpreter::BLEZ;
    Jump_Opcode[7] = &R4300iInterpreter::BGTZ;
    Jump_Opcode[8] = &R4300iInterpreter::ADDI;
    Jump_Opcode[9] = &R4300iInterpreter::ADDIU;
    Jump_Opcode[10] = &R4300iInterpreter::SLTI;
    Jump_Opcode[11] = &R4300iInterpreter::SLTIU;
    Jump_Opcode[12] = &R4300iInterpreter::ANDI;
    Jump_Opcode[13] = &R4300iInterpreter::ORI;
    Jump_Opcode[14] = &R4300iInterpreter::XORI;
    Jump_Opcode[15] = &R4300iInterpreter::LUI;
    Jump_Opcode[16] = &R4300iInterpreter::COP0;
    Jump_Opcode[17] = &R4300iInterpreter::COP1;
    Jump_Opcode[18] = &R4300iInterpreter::COP2;
    Jump_Opcode[19] = &R4300iInterpreter::COP3;
    Jump_Opcode[20] = &R4300iInterpreter::BEQL;
    Jump_Opcode[21] = &R4300iInterpreter::BNEL;
    Jump_Opcode[22] = &R4300iInterpreter::BLEZL;
    Jump_Opcode[23] = &R4300iInterpreter::BGTZL;
    Jump_Opcode[24] = &R4300iInterpreter::DADDI;
    Jump_Opcode[25] = &R4300iInterpreter::DADDIU;
    Jump_Opcode[26] = &R4300iInterpreter::LDL;
    Jump_Opcode[27] = &R4300iInterpreter::LDR;
    Jump_Opcode[28] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[29] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[30] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[31] = &R4300iInterpreter::ReservedInstruction;
    Jump_Opcode[32] = Force32bit ? &R4300iInterpreter::LB_32 : &R4300iInterpreter::LB;
    Jump_Opcode[33] = Force32bit ? &R4300iInterpreter::LH_32 : &R4300iInterpreter::LH;
    Jump_Opcode[34] = Force32bit ? &R4300iInterpreter::LWL_32 : &R4300iInterpreter::LWL;
    Jump_Opcode[35] = Force32bit ? &R4300iInterpreter::LW_32 : &R4300iInterpreter::LW;
    Jump_Opcode[36] = Force32bit ? &R4300iInterpreter::LBU_32 : &R4300iInterpreter::LBU;
    Jump_Opcode[37] = Force32bit ? &R4300iInterpreter::LHU_32 : &R4300iInterpreter::LHU;
    Jump_Opcode[38] = Force32bit ? &R4300iInterpreter::LWR_32 : &R4300iInterpreter::LWR;
    Jump_Opcode[39] = Force32bit ? &R4300iInterpreter::LWU_32 : &R4300iInterpreter::LWU;
    Jump_Opcode[40] = Force32bit ? &R4300iInterpreter::SB_32 : &R4300iInterpreter::SB;
    Jump_Opcode[41] = Force32bit ? &R4300iInterpreter::SH_32 : &R4300iInterpreter::SH;
    Jump_Opcode[42] = Force32bit ? &R4300iInterpreter::SWL_32 : &R4300iInterpreter::SWL;
    Jump_Opcode[43] = Force32bit ? &R4300iInterpreter::SW_32 : &R4300iInterpreter::SW;
    Jump_Opcode[44] = Force32bit ? &R4300iInterpreter::SDL_32 : &R4300iInterpreter::SDL;
    Jump_Opcode[45] = Force32bit ? &R4300iInterpreter::SDR_32 : &R4300iInterpreter::SDR;
    Jump_Opcode[46] = Force32bit ? &R4300iInterpreter::SWR_32 : &R4300iInterpreter::SWR;
    Jump_Opcode[47] = &R4300iInterpreter::CACHE;
    Jump_Opcode[48] = Force32bit ? &R4300iInterpreter::LL_32 : &R4300iInterpreter::LL;
    Jump_Opcode[49] = Force32bit ? &R4300iInterpreter::LWC1_32 : &R4300iInterpreter::LWC1;
    Jump_Opcode[50] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[51] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[52] = Force32bit ? &R4300iInterpreter::LLD_32 : &R4300iInterpreter::LLD;
    Jump_Opcode[53] = Force32bit ? &R4300iInterpreter::LDC1_32 : &R4300iInterpreter::LDC1;
    Jump_Opcode[54] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[55] = Force32bit ? &R4300iInterpreter::LD_32 : &R4300iInterpreter::LD;
    Jump_Opcode[56] = Force32bit ? &R4300iInterpreter::SC_32 : &R4300iInterpreter::SC;
    Jump_Opcode[57] = Force32bit ? &R4300iInterpreter::SWC1_32 : &R4300iInterpreter::SWC1;
    Jump_Opcode[58] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[59] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[60] = Force32bit ? &R4300iInterpreter::SCD_32 : &R4300iInterpreter::SCD;
    Jump_Opcode[61] = Force32bit ? &R4300iInterpreter::SDC1_32 : &R4300iInterpreter::SDC1;
    Jump_Opcode[62] = &R4300iInterpreter::UnknownOpcode;
    Jump_Opcode[63] = Force32bit ? &R4300iInterpreter::SD_32 : &R4300iInterpreter::SD;

    Jump_Special[0] = &R4300iInterpreter::SPECIAL_SLL;
    Jump_Special[1] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[2] = &R4300iInterpreter::SPECIAL_SRL;
    Jump_Special[3] = &R4300iInterpreter::SPECIAL_SRA;
    Jump_Special[4] = &R4300iInterpreter::SPECIAL_SLLV;
    Jump_Special[5] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[6] = &R4300iInterpreter::SPECIAL_SRLV;
    Jump_Special[7] = &R4300iInterpreter::SPECIAL_SRAV;
    Jump_Special[8] = &R4300iInterpreter::SPECIAL_JR;
    Jump_Special[9] = &R4300iInterpreter::SPECIAL_JALR;
    Jump_Special[10] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[11] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[12] = &R4300iInterpreter::SPECIAL_SYSCALL;
    Jump_Special[13] = &R4300iInterpreter::SPECIAL_BREAK;
    Jump_Special[14] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[15] = &R4300iInterpreter::SPECIAL_SYNC;
    Jump_Special[16] = &R4300iInterpreter::SPECIAL_MFHI;
    Jump_Special[17] = &R4300iInterpreter::SPECIAL_MTHI;
    Jump_Special[18] = &R4300iInterpreter::SPECIAL_MFLO;
    Jump_Special[19] = &R4300iInterpreter::SPECIAL_MTLO;
    Jump_Special[20] = &R4300iInterpreter::SPECIAL_DSLLV;
    Jump_Special[21] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[22] = &R4300iInterpreter::SPECIAL_DSRLV;
    Jump_Special[23] = &R4300iInterpreter::SPECIAL_DSRAV;
    Jump_Special[24] = &R4300iInterpreter::SPECIAL_MULT;
    Jump_Special[25] = &R4300iInterpreter::SPECIAL_MULTU;
    Jump_Special[26] = &R4300iInterpreter::SPECIAL_DIV;
    Jump_Special[27] = &R4300iInterpreter::SPECIAL_DIVU;
    Jump_Special[28] = &R4300iInterpreter::SPECIAL_DMULT;
    Jump_Special[29] = &R4300iInterpreter::SPECIAL_DMULTU;
    Jump_Special[30] = &R4300iInterpreter::SPECIAL_DDIV;
    Jump_Special[31] = &R4300iInterpreter::SPECIAL_DDIVU;
    Jump_Special[32] = &R4300iInterpreter::SPECIAL_ADD;
    Jump_Special[33] = &R4300iInterpreter::SPECIAL_ADDU;
    Jump_Special[34] = &R4300iInterpreter::SPECIAL_SUB;
    Jump_Special[35] = &R4300iInterpreter::SPECIAL_SUBU;
    Jump_Special[36] = &R4300iInterpreter::SPECIAL_AND;
    Jump_Special[37] = &R4300iInterpreter::SPECIAL_OR;
    Jump_Special[38] = &R4300iInterpreter::SPECIAL_XOR;
    Jump_Special[39] = &R4300iInterpreter::SPECIAL_NOR;
    Jump_Special[40] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[41] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[42] = &R4300iInterpreter::SPECIAL_SLT;
    Jump_Special[43] = &R4300iInterpreter::SPECIAL_SLTU;
    Jump_Special[44] = &R4300iInterpreter::SPECIAL_DADD;
    Jump_Special[45] = &R4300iInterpreter::SPECIAL_DADDU;
    Jump_Special[46] = &R4300iInterpreter::SPECIAL_DSUB;
    Jump_Special[47] = &R4300iInterpreter::SPECIAL_DSUBU;
    Jump_Special[48] = &R4300iInterpreter::SPECIAL_TGE;
    Jump_Special[49] = &R4300iInterpreter::SPECIAL_TGEU;
    Jump_Special[50] = &R4300iInterpreter::SPECIAL_TLT;
    Jump_Special[51] = &R4300iInterpreter::SPECIAL_TLTU;
    Jump_Special[52] = &R4300iInterpreter::SPECIAL_TEQ;
    Jump_Special[53] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[54] = &R4300iInterpreter::SPECIAL_TNE;
    Jump_Special[55] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[56] = &R4300iInterpreter::SPECIAL_DSLL;
    Jump_Special[57] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[58] = &R4300iInterpreter::SPECIAL_DSRL;
    Jump_Special[59] = &R4300iInterpreter::SPECIAL_DSRA;
    Jump_Special[60] = &R4300iInterpreter::SPECIAL_DSLL32;
    Jump_Special[61] = &R4300iInterpreter::UnknownOpcode;
    Jump_Special[62] = &R4300iInterpreter::SPECIAL_DSRL32;
    Jump_Special[63] = &R4300iInterpreter::SPECIAL_DSRA32;

    Jump_Regimm[0] = &R4300iInterpreter::REGIMM_BLTZ;
    Jump_Regimm[1] = &R4300iInterpreter::REGIMM_BGEZ;
    Jump_Regimm[2] = &R4300iInterpreter::REGIMM_BLTZL;
    Jump_Regimm[3] = &R4300iInterpreter::REGIMM_BGEZL;
    Jump_Regimm[4] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[5] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[6] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[7] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[8] = &R4300iInterpreter::REGIMM_TGEI;
    Jump_Regimm[9] = &R4300iInterpreter::REGIMM_TGEIU;
    Jump_Regimm[10] = &R4300iInterpreter::REGIMM_TLTI;
    Jump_Regimm[11] = &R4300iInterpreter::REGIMM_TLTIU;
    Jump_Regimm[12] = &R4300iInterpreter::REGIMM_TEQI;
    Jump_Regimm[13] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[14] = &R4300iInterpreter::REGIMM_TNEI;
    Jump_Regimm[15] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[16] = &R4300iInterpreter::REGIMM_BLTZAL;
    Jump_Regimm[17] = &R4300iInterpreter::REGIMM_BGEZAL;
    Jump_Regimm[18] = &R4300iInterpreter::REGIMM_BLTZALL;
    Jump_Regimm[19] = &R4300iInterpreter::REGIMM_BGEZALL;
    Jump_Regimm[20] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[21] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[22] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[23] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[24] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[25] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[26] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[27] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[28] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[29] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[30] = &R4300iInterpreter::UnknownOpcode;
    Jump_Regimm[31] = &R4300iInterpreter::UnknownOpcode;

    Jump_CoP0[0] = &R4300iInterpreter::COP0_MF;
    Jump_CoP0[1] = &R4300iInterpreter::COP0_DMF;
    Jump_CoP0[2] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[3] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[4] = &R4300iInterpreter::COP0_MT;
    Jump_CoP0[5] = &R4300iInterpreter::COP0_DMT;
    Jump_CoP0[6] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[7] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[8] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[9] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[10] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[11] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[12] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[13] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[14] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[15] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0[16] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[17] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[18] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[19] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[20] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[21] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[22] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[23] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[24] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[25] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[26] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[27] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[28] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[29] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[30] = &R4300iInterpreter::COP0_CO;
    Jump_CoP0[31] = &R4300iInterpreter::COP0_CO;

    Jump_CoP0_Function[0] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[1] = &R4300iInterpreter::COP0_CO_TLBR;
    Jump_CoP0_Function[2] = &R4300iInterpreter::COP0_CO_TLBWI;
    Jump_CoP0_Function[3] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[4] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[5] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[6] = &R4300iInterpreter::COP0_CO_TLBWR;
    Jump_CoP0_Function[7] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[8] = &R4300iInterpreter::COP0_CO_TLBP;
    for (int i = 9; i < 24; i++) Jump_CoP0_Function[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[24] = &R4300iInterpreter::COP0_CO_ERET;
    for (int i = 25; i < 32; i++) Jump_CoP0_Function[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP0_Function[32] = &R4300iInterpreter::COP0_CO_WAIT;
    for (int i = 33; i < 64; i++) Jump_CoP0_Function[i] = &R4300iInterpreter::UnknownOpcode;

    Jump_CoP1[0] = &R4300iInterpreter::COP1_MF;
    Jump_CoP1[1] = &R4300iInterpreter::COP1_DMF;
    Jump_CoP1[2] = &R4300iInterpreter::COP1_CF;
    Jump_CoP1[3] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    Jump_CoP1[4] = &R4300iInterpreter::COP1_MT;
    Jump_CoP1[5] = &R4300iInterpreter::COP1_DMT;
    Jump_CoP1[6] = &R4300iInterpreter::COP1_CT;
    Jump_CoP1[7] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    Jump_CoP1[8] = &R4300iInterpreter::COP1_BC;
    for (int i = 9; i < 16; i++) Jump_CoP1[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1[16] = &R4300iInterpreter::COP1_S;
    Jump_CoP1[17] = &R4300iInterpreter::COP1_D;
    Jump_CoP1[18] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1[19] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1[20] = &R4300iInterpreter::COP1_W;
    Jump_CoP1[21] = &R4300iInterpreter::COP1_L;
    for (int i = 22; i < 32; i++) Jump_CoP1[i] = &R4300iInterpreter::UnknownOpcode;

    Jump_CoP1_BC[0] = &R4300iInterpreter::COP1_BCF;
    Jump_CoP1_BC[1] = &R4300iInterpreter::COP1_BCT;
    Jump_CoP1_BC[2] = &R4300iInterpreter::COP1_BCFL;
    Jump_CoP1_BC[3] = &R4300iInterpreter::COP1_BCTL;
    for (int i = 4; i < 32; i++) Jump_CoP1_BC[i] = &R4300iInterpreter::UnknownOpcode;

    Jump_CoP1_S[0] = &R4300iInterpreter::COP1_S_ADD;
    Jump_CoP1_S[1] = &R4300iInterpreter::COP1_S_SUB;
    Jump_CoP1_S[2] = &R4300iInterpreter::COP1_S_MUL;
    Jump_CoP1_S[3] = &R4300iInterpreter::COP1_S_DIV;
    Jump_CoP1_S[4] = &R4300iInterpreter::COP1_S_SQRT;
    Jump_CoP1_S[5] = &R4300iInterpreter::COP1_S_ABS;
    Jump_CoP1_S[6] = &R4300iInterpreter::COP1_S_MOV;
    Jump_CoP1_S[7] = &R4300iInterpreter::COP1_S_NEG;
    Jump_CoP1_S[8] = &R4300iInterpreter::COP1_S_ROUND_L;
    Jump_CoP1_S[9] = &R4300iInterpreter::COP1_S_TRUNC_L;
    Jump_CoP1_S[10] = &R4300iInterpreter::COP1_S_CEIL_L;
    Jump_CoP1_S[11] = &R4300iInterpreter::COP1_S_FLOOR_L;
    Jump_CoP1_S[12] = &R4300iInterpreter::COP1_S_ROUND_W;
    Jump_CoP1_S[13] = &R4300iInterpreter::COP1_S_TRUNC_W;
    Jump_CoP1_S[14] = &R4300iInterpreter::COP1_S_CEIL_W;
    Jump_CoP1_S[15] = &R4300iInterpreter::COP1_S_FLOOR_W;
    for (int i = 16; i < 32; i++) Jump_CoP1_S[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_S[32] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    Jump_CoP1_S[33] = &R4300iInterpreter::COP1_S_CVT_D;
    Jump_CoP1_S[34] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_S[35] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_S[36] = &R4300iInterpreter::COP1_S_CVT_W;
    Jump_CoP1_S[37] = &R4300iInterpreter::COP1_S_CVT_L;
    for (int i = 38; i < 48; i++) Jump_CoP1_S[i] = &R4300iInterpreter::UnknownOpcode;
    for (int i = 48; i < 64; i++) Jump_CoP1_S[i] = &R4300iInterpreter::COP1_S_CMP;

    Jump_CoP1_D[0] = &R4300iInterpreter::COP1_D_ADD;
    Jump_CoP1_D[1] = &R4300iInterpreter::COP1_D_SUB;
    Jump_CoP1_D[2] = &R4300iInterpreter::COP1_D_MUL;
    Jump_CoP1_D[3] = &R4300iInterpreter::COP1_D_DIV;
    Jump_CoP1_D[4] = &R4300iInterpreter::COP1_D_SQRT;
    Jump_CoP1_D[5] = &R4300iInterpreter::COP1_D_ABS;
    Jump_CoP1_D[6] = &R4300iInterpreter::COP1_D_MOV;
    Jump_CoP1_D[7] = &R4300iInterpreter::COP1_D_NEG;
    Jump_CoP1_D[8] = &R4300iInterpreter::COP1_D_ROUND_L;
    Jump_CoP1_D[9] = &R4300iInterpreter::COP1_D_TRUNC_L;
    Jump_CoP1_D[10] = &R4300iInterpreter::COP1_D_CEIL_L;
    Jump_CoP1_D[11] = &R4300iInterpreter::COP1_D_FLOOR_L;
    Jump_CoP1_D[12] = &R4300iInterpreter::COP1_D_ROUND_W;
    Jump_CoP1_D[13] = &R4300iInterpreter::COP1_D_TRUNC_W;
    Jump_CoP1_D[14] = &R4300iInterpreter::COP1_D_CEIL_W;
    Jump_CoP1_D[15] = &R4300iInterpreter::COP1_D_FLOOR_W;
    for (int i = 16; i < 32; i++) Jump_CoP1_D[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_D[32] = &R4300iInterpreter::COP1_D_CVT_S;
    Jump_CoP1_D[33] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    Jump_CoP1_D[34] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_D[35] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_D[36] = &R4300iInterpreter::COP1_D_CVT_W;
    Jump_CoP1_D[37] = &R4300iInterpreter::COP1_D_CVT_L;
    for (int i = 38; i < 48; i++) Jump_CoP1_D[i] = &R4300iInterpreter::UnknownOpcode;
    for (int i = 48; i < 64; i++) Jump_CoP1_D[i] = &R4300iInterpreter::COP1_D_CMP;

    for (int i = 0; i < 8; i++) Jump_CoP1_W[i] = &R4300iInterpreter::UnknownOpcode;
    for (int i = 8; i < 16; i++) Jump_CoP1_W[i] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    for (int i = 16; i < 32; i++) Jump_CoP1_W[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_W[32] = &R4300iInterpreter::COP1_W_CVT_S;
    Jump_CoP1_W[33] = &R4300iInterpreter::COP1_W_CVT_D;
    Jump_CoP1_W[34] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_W[35] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_W[36] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    Jump_CoP1_W[37] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    for (int i = 38; i < 64; i++) Jump_CoP1_W[i] = &R4300iInterpreter::UnknownOpcode;

    for (int i = 0; i < 8; i++) Jump_CoP1_L[i] = &R4300iInterpreter::UnknownOpcode;
    for (int i = 8; i < 16; i++) Jump_CoP1_L[i] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    for (int i = 16; i < 32; i++) Jump_CoP1_L[i] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_L[32] = &R4300iInterpreter::COP1_L_CVT_S;
    Jump_CoP1_L[33] = &R4300iInterpreter::COP1_L_CVT_D;
    Jump_CoP1_L[34] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_L[35] = &R4300iInterpreter::UnknownOpcode;
    Jump_CoP1_L[36] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    Jump_CoP1_L[37] = &R4300iInterpreter::CPO1_UNIMPLEMENTED_OP;
    for (int i = 38; i < 64; i++) Jump_CoP1_L[i] = &R4300iInterpreter::UnknownOpcode;

    Jump_CoP2[0] = &R4300iInterpreter::COP2_MF;
    Jump_CoP2[1] = &R4300iInterpreter::COP2_DMF;
    Jump_CoP2[2] = &R4300iInterpreter::COP2_CF;
    Jump_CoP2[3] = &R4300iInterpreter::CPO2_INVALID_OP;
    Jump_CoP2[4] = &R4300iInterpreter::COP2_MT;
    Jump_CoP2[5] = &R4300iInterpreter::COP2_DMT;
    Jump_CoP2[6] = &R4300iInterpreter::COP2_CT;
    for (int i = 7; i < 32; i++) Jump_CoP2[i] = &R4300iInterpreter::CPO2_INVALID_OP;
}

// ============================================================================
// Primary Opcode Functions
// ============================================================================

void R4300iInterpreter::J()
{
    m_System.DelayedJump((m_PROGRAM_COUNTER & 0xFFFFFFFFF0000000ULL) + (target() << 2));
}

void R4300iInterpreter::JAL()
{
    m_System.DelayedJump((m_PROGRAM_COUNTER & 0xFFFFFFFFF0000000ULL) + (target() << 2));
    m_GPR[31].DW = (int32_t)(m_System.m_PipelineStage == PIPELINE_STAGE_JUMP_DELAY_SLOT ? m_System.m_JumpToLocation + 4 : m_PROGRAM_COUNTER + 8);
}

void R4300iInterpreter::BEQ()
{
    if (m_GPR[rs()].DW == m_GPR[rt()].DW)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
}

void R4300iInterpreter::BNE()
{
    if (m_GPR[rs()].DW != m_GPR[rt()].DW)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
}

void R4300iInterpreter::BLEZ()
{
    if (m_GPR[rs()].DW <= 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
}

void R4300iInterpreter::BGTZ()
{
    if (m_GPR[rs()].DW > 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
}

void R4300iInterpreter::ADDI()
{
    int32_t rsVal = m_GPR[rs()].W[0];
    int32_t immVal = (int16_t)immediate();
    int32_t sum = rsVal + immVal;
    if ((~(rsVal ^ immVal) & (rsVal ^ sum)) & 0x80000000)
    {
        m_Reg.TriggerException(EXC_OV);
        return;
    }
    m_GPR[rt()].DW = sum;
}

void R4300iInterpreter::ADDIU()
{
    m_GPR[rt()].DW = (m_GPR[rs()].W[0] + ((int16_t)immediate()));
}

void R4300iInterpreter::SLTI()
{
    if (m_GPR[rs()].DW < (int64_t)((int16_t)immediate()))
    {
        m_GPR[rt()].DW = 1;
    }
    else
    {
        m_GPR[rt()].DW = 0;
    }
}

void R4300iInterpreter::SLTIU()
{
    int32_t imm32 = (int16_t)immediate();
    int64_t imm64 = imm32;
    m_GPR[rt()].DW = m_GPR[rs()].UDW < (uint64_t)imm64 ? 1 : 0;
}

void R4300iInterpreter::ANDI()
{
    m_GPR[rt()].DW = m_GPR[rs()].DW & immediate();
}

void R4300iInterpreter::ORI()
{
    m_GPR[rt()].DW = m_GPR[rs()].DW | immediate();
}

void R4300iInterpreter::XORI()
{
    m_GPR[rt()].DW = m_GPR[rs()].DW ^ immediate();
}

void R4300iInterpreter::LUI()
{
    m_GPR[rt()].DW = (int32_t)((int16_t)offset() << 16);
}

void R4300iInterpreter::BEQL()
{
    if (m_GPR[rs()].DW == m_GPR[rt()].DW)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::BNEL()
{
    if (m_GPR[rs()].DW != m_GPR[rt()].DW)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::BLEZL()
{
    if (m_GPR[rs()].DW <= 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::BGTZL()
{
    if (m_GPR[rs()].DW > 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::DADDI()
{
    int64_t rsVal = m_GPR[rs()].DW;
    int64_t immVal = (int64_t)((int16_t)immediate());
    int64_t sum = rsVal + immVal;
    if ((~(rsVal ^ immVal) & (rsVal ^ sum)) & 0x8000000000000000LL)
    {
        m_Reg.TriggerException(EXC_OV);
        return;
    }
    m_GPR[rt()].DW = sum;
}

void R4300iInterpreter::DADDIU()
{
    m_GPR[rt()].DW = m_GPR[rs()].DW + (int64_t)((int16_t)immediate());
}

// ============================================================================
// Load Instructions
// ============================================================================

static uint64_t LDL_MASK[8] = {0, 0xFF, 0xFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFFFFFFULL, 0xFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFULL};
static int32_t LDL_SHIFT[8] = {0, 8, 16, 24, 32, 40, 48, 56};

void R4300iInterpreter::LDL()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint64_t MemoryValue;
    if (m_MMU.LD_Memory((Address & ~7), MemoryValue))
    {
        uint32_t Offset = Address & 7;
        m_GPR[rt()].DW = m_GPR[rt()].DW & LDL_MASK[Offset];
        m_GPR[rt()].DW += MemoryValue << LDL_SHIFT[Offset];
    }
}

static uint64_t LDR_MASK[8] = {0xFFFFFFFFFFFFFF00ULL, 0xFFFFFFFFFFFF0000ULL,
                                0xFFFFFFFFFF000000ULL, 0xFFFFFFFF00000000ULL,
                                0xFFFFFF0000000000ULL, 0xFFFF000000000000ULL,
                                0xFF00000000000000ULL, 0};
static int32_t LDR_SHIFT[8] = {56, 48, 40, 32, 24, 16, 8, 0};

void R4300iInterpreter::LDR()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint64_t MemoryValue;
    if (m_MMU.LD_Memory((Address & ~7), MemoryValue))
    {
        uint32_t Offset = Address & 7;
        m_GPR[rt()].DW = m_GPR[rt()].DW & LDR_MASK[Offset];
        m_GPR[rt()].DW += MemoryValue >> LDR_SHIFT[Offset];
    }
}

void R4300iInterpreter::LB()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint8_t MemoryValue;
    if (m_MMU.LB_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int8_t)MemoryValue;
    }
}

void R4300iInterpreter::LB_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint8_t MemoryValue;
    if (m_MMU.LB_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int8_t)MemoryValue;
    }
}

void R4300iInterpreter::LH()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint16_t MemoryValue;
    if (m_MMU.LH_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int16_t)MemoryValue;
    }
}

void R4300iInterpreter::LH_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint16_t MemoryValue;
    if (m_MMU.LH_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int16_t)MemoryValue;
    }
}

void R4300iInterpreter::LWL()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory((Address & ~3), MemoryValue))
    {
        uint32_t Offset = Address & 3;
        m_GPR[rt()].DW = (int32_t)(m_GPR[rt()].W[0] & LWL_MASK[Offset]);
        m_GPR[rt()].DW += (int32_t)(MemoryValue << LWL_SHIFT[Offset]);
    }
}

void R4300iInterpreter::LWL_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory((Address & ~3), MemoryValue))
    {
        uint32_t Offset = Address & 3;
        m_GPR[rt()].DW = (int32_t)(m_GPR[rt()].W[0] & LWL_MASK[Offset]);
        m_GPR[rt()].DW += (int32_t)(MemoryValue << LWL_SHIFT[Offset]);
    }
}

void R4300iInterpreter::LW()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int32_t)MemoryValue;
    }
}

void R4300iInterpreter::LW_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int32_t)MemoryValue;
    }
}

void R4300iInterpreter::LBU()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint8_t MemoryValue;
    if (m_MMU.LB_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].UDW = MemoryValue;
    }
}

void R4300iInterpreter::LBU_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint8_t MemoryValue;
    if (m_MMU.LB_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].UDW = MemoryValue;
    }
}

void R4300iInterpreter::LHU()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint16_t MemoryValue;
    if (m_MMU.LH_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].UDW = MemoryValue;
    }
}

void R4300iInterpreter::LHU_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint16_t MemoryValue;
    if (m_MMU.LH_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].UDW = MemoryValue;
    }
}

void R4300iInterpreter::LWR()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory((Address & ~3), MemoryValue))
    {
        uint32_t Offset = Address & 3;
        m_GPR[rt()].DW = (int32_t)(m_GPR[rt()].W[0] & LWR_MASK[Offset]);
        m_GPR[rt()].DW += (int32_t)(MemoryValue >> LWR_SHIFT[Offset]);
    }
}

void R4300iInterpreter::LWR_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory((Address & ~3), MemoryValue))
    {
        uint32_t Offset = Address & 3;
        m_GPR[rt()].DW = (int32_t)(m_GPR[rt()].W[0] & LWR_MASK[Offset]);
        m_GPR[rt()].DW += (int32_t)(MemoryValue >> LWR_SHIFT[Offset]);
    }
}

void R4300iInterpreter::LWU()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].UDW = MemoryValue;
    }
}

void R4300iInterpreter::LWU_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].UDW = MemoryValue;
    }
}

void R4300iInterpreter::LL()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int32_t)MemoryValue;
        m_LLBit = 1;
        uint32_t PhysicalAddr;
        bool MemoryUsed;
        m_TLB.VAddrToPAddr(Address, PhysicalAddr, MemoryUsed);
        m_CP0[17] = PhysicalAddr >> 4;
    }
    else
    {
        m_LLBit = 0;
    }
}

void R4300iInterpreter::LL_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.LW_Memory(Address, MemoryValue))
    {
        m_GPR[rt()].DW = (int32_t)MemoryValue;
        m_LLBit = 1;
        uint32_t PhysicalAddr;
        bool MemoryUsed;
        m_TLB.VAddrToPAddr(Address, PhysicalAddr, MemoryUsed);
        m_CP0[17] = PhysicalAddr >> 4;
    }
    else
    {
        m_LLBit = 0;
    }
}

void R4300iInterpreter::LWC1()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.LW_Memory(Address, *(uint32_t *)m_FPR_S[ft()]);
}

void R4300iInterpreter::LWC1_32()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.LW_Memory(Address, *(uint32_t *)m_FPR_S[ft()]);
}

void R4300iInterpreter::LLD()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    if (m_MMU.LD_Memory(Address, m_GPR[rt()].UDW))
    {
        m_LLBit = 1;
        uint32_t PhysicalAddr;
        bool MemoryUsed;
        m_TLB.VAddrToPAddr(Address, PhysicalAddr, MemoryUsed);
        m_CP0[17] = PhysicalAddr >> 4;
    }
    else
    {
        m_LLBit = 0;
    }
}

void R4300iInterpreter::LLD_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    if (m_MMU.LD_Memory(Address, m_GPR[rt()].UDW))
    {
        m_LLBit = 1;
        uint32_t PhysicalAddr;
        bool MemoryUsed;
        m_TLB.VAddrToPAddr(Address, PhysicalAddr, MemoryUsed);
        m_CP0[17] = PhysicalAddr >> 4;
    }
    else
    {
        m_LLBit = 0;
    }
}

void R4300iInterpreter::LDC1()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.LD_Memory(Address, *(uint64_t *)m_FPR_D[ft()]);
}

void R4300iInterpreter::LDC1_32()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.LD_Memory(Address, *(uint64_t *)m_FPR_D[ft()]);
}

void R4300iInterpreter::LD()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.LD_Memory(Address, m_GPR[rt()].UDW);
}

void R4300iInterpreter::LD_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.LD_Memory(Address, m_GPR[rt()].UDW);
}

// ============================================================================
// Store Instructions
// ============================================================================

void R4300iInterpreter::SB()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.SB_Memory(Address, m_GPR[rt()].UW[0]);
}

void R4300iInterpreter::SB_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.SB_Memory(Address, m_GPR[rt()].UW[0]);
}

void R4300iInterpreter::SH()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.SH_Memory(Address, m_GPR[rt()].UW[0]);
}

void R4300iInterpreter::SH_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.SH_Memory(Address, m_GPR[rt()].UW[0]);
}

void R4300iInterpreter::SWL()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.MemoryValue32(Address & ~3, MemoryValue))
    {
        uint32_t Offset = Address & 3;
        MemoryValue &= SWL_MASK[Offset];
        MemoryValue += m_GPR[rt()].UW[0] >> SWL_SHIFT[Offset];
        m_MMU.SW_Memory(Address & ~3, MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::SWL_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.MemoryValue32(Address & ~3, MemoryValue))
    {
        uint32_t Offset = Address & 3;
        MemoryValue &= SWL_MASK[Offset];
        MemoryValue += m_GPR[rt()].UW[0] >> SWL_SHIFT[Offset];
        m_MMU.SW_Memory(Address & ~3, MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::SW()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.SW_Memory(Address, m_GPR[rt()].UW[0]);
}

void R4300iInterpreter::SW_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.SW_Memory(Address, m_GPR[rt()].UW[0]);
}

static uint64_t SDL_MASK[8] = {0, 0xFF00000000000000ULL, 0xFFFF000000000000ULL, 0xFFFFFF0000000000ULL,
                                0xFFFFFFFF00000000ULL, 0xFFFFFFFFFF000000ULL, 0xFFFFFFFFFFFF0000ULL, 0xFFFFFFFFFFFFFF00ULL};
static int32_t SDL_SHIFT[8] = {0, 8, 16, 24, 32, 40, 48, 56};

void R4300iInterpreter::SDL()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint64_t MemoryValue;
    if (m_MMU.MemoryValue64((Address & ~7), MemoryValue))
    {
        uint32_t Offset = Address & 7;
        MemoryValue &= SDL_MASK[Offset];
        MemoryValue += m_GPR[rt()].UDW >> SDL_SHIFT[Offset];
        m_MMU.SD_Memory((Address & ~7), MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::SDL_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint64_t MemoryValue;
    if (m_MMU.MemoryValue64((Address & ~7), MemoryValue))
    {
        uint32_t Offset = Address & 7;
        MemoryValue &= SDL_MASK[Offset];
        MemoryValue += m_GPR[rt()].UDW >> SDL_SHIFT[Offset];
        m_MMU.SD_Memory((Address & ~7), MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

static uint64_t SDR_MASK[8] = {0x00FFFFFFFFFFFFFFULL, 0x0000FFFFFFFFFFFFULL,
                                0x000000FFFFFFFFFFULL, 0x00000000FFFFFFFFULL,
                                0x0000000000FFFFFFULL, 0x000000000000FFFFULL,
                                0x00000000000000FFULL, 0x0000000000000000ULL};
static int32_t SDR_SHIFT[8] = {56, 48, 40, 32, 24, 16, 8, 0};

void R4300iInterpreter::SDR()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint64_t MemoryValue;
    if (m_MMU.MemoryValue64((Address & ~7), MemoryValue))
    {
        uint32_t Offset = Address & 7;
        MemoryValue &= SDR_MASK[Offset];
        MemoryValue += m_GPR[rt()].UDW << SDR_SHIFT[Offset];
        m_MMU.SD_Memory((Address & ~7), MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::SDR_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint64_t MemoryValue;
    if (m_MMU.MemoryValue64((Address & ~7), MemoryValue))
    {
        uint32_t Offset = Address & 7;
        MemoryValue &= SDR_MASK[Offset];
        MemoryValue += m_GPR[rt()].UDW << SDR_SHIFT[Offset];
        m_MMU.SD_Memory((Address & ~7), MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::SWR()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.MemoryValue32((Address & ~3), MemoryValue))
    {
        uint32_t Offset = Address & 3;
        MemoryValue &= SWR_MASK[Offset];
        MemoryValue += m_GPR[rt()].UW[0] << SWR_SHIFT[Offset];
        m_MMU.SW_Memory((Address & ~0x03), MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::SWR_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    uint32_t MemoryValue;
    if (m_MMU.MemoryValue32((Address & ~3), MemoryValue))
    {
        uint32_t Offset = Address & 3;
        MemoryValue &= SWR_MASK[Offset];
        MemoryValue += m_GPR[rt()].UW[0] << SWR_SHIFT[Offset];
        m_MMU.SW_Memory((Address & ~0x03), MemoryValue);
    }
    else
    {
        m_Reg.TriggerAddressException(Address, EXC_WMISS);
    }
}

void R4300iInterpreter::CACHE()
{
    // Cache instruction: invalidate recompiler blocks for the target address.
    // The game uses CACHE to signal code changes (e.g., overlay loads).
    // DMA already invalidates via NotifyDMAToRDRAM, but CACHE Hit Invalidate
    // (op=0x10) after CPU stores needs explicit handling.
    uint32_t cacheOp = rt();  // bits 20:16 encode the cache operation
    if (cacheOp == 0x10 || cacheOp == 0x00 || cacheOp == 0x14) {
        // I-Cache Hit Invalidate (0x10), I-Cache Index Invalidate (0x00), I-Cache Fill (0x14)
        uint64_t vAddr = m_GPR[base()].DW + (int16_t)offset();
        uint32_t pAddr = static_cast<uint32_t>(vAddr) & 0x1FFFFFFF;
        if (pAddr < 0x800000) {
            m_System.NotifyDMAToRDRAM(pAddr & ~0x1F, 32);  // cache line = 32 bytes
        }
    }
}

void R4300iInterpreter::SC()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    bool success = false;
    if (m_LLBit == 1)
    {
        success = m_MMU.SW_Memory(Address, m_GPR[rt()].UW[0]);
    }
    m_GPR[rt()].UDW = success ? 1 : 0;
    m_LLBit = 0;
}

void R4300iInterpreter::SC_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    bool success = false;
    if (m_LLBit == 1)
    {
        success = m_MMU.SW_Memory(Address, m_GPR[rt()].UW[0]);
    }
    m_GPR[rt()].UDW = success ? 1 : 0;
    m_LLBit = 0;
}

void R4300iInterpreter::SCD()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    bool success = false;
    if (m_LLBit == 1)
    {
        success = m_MMU.SD_Memory(Address, m_GPR[rt()].UDW);
    }
    m_GPR[rt()].UDW = success ? 1 : 0;
    m_LLBit = 0;
}

void R4300iInterpreter::SCD_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    bool success = false;
    if (m_LLBit == 1)
    {
        success = m_MMU.SD_Memory(Address, m_GPR[rt()].UDW);
    }
    m_GPR[rt()].UDW = success ? 1 : 0;
    m_LLBit = 0;
}

void R4300iInterpreter::SWC1()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.SW_Memory(Address, *(uint32_t *)m_FPR_S[ft()]);
}

void R4300iInterpreter::SWC1_32()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.SW_Memory(Address, *(uint32_t *)m_FPR_S[ft()]);
}

void R4300iInterpreter::SDC1()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.SD_Memory(Address, *((uint64_t *)m_FPR_D[ft()]));
}

void R4300iInterpreter::SDC1_32()
{
    if (TestCop1UsableException())
    {
        return;
    }
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.SD_Memory(Address, *((uint64_t *)m_FPR_D[ft()]));
}

void R4300iInterpreter::SD()
{
    uint64_t Address = m_GPR[base()].DW + (int16_t)offset();
    m_MMU.SD_Memory(Address, m_GPR[rt()].UDW);
}

void R4300iInterpreter::SD_32()
{
    uint64_t Address = m_GPR[base()].W[0] + (int16_t)offset();
    m_MMU.SD_Memory(Address, m_GPR[rt()].UDW);
}

// ============================================================================
// R4300i SPECIAL Opcodes
// ============================================================================

void R4300iInterpreter::SPECIAL_SLL()
{
    m_GPR[rd()].DW = (m_GPR[rt()].W[0] << sa());
}

void R4300iInterpreter::SPECIAL_SRL()
{
    m_GPR[rd()].DW = (int32_t)(m_GPR[rt()].UW[0] >> sa());
}

void R4300iInterpreter::SPECIAL_SRA()
{
    m_GPR[rd()].DW = (int32_t)(m_GPR[rt()].DW >> sa());
}

void R4300iInterpreter::SPECIAL_SLLV()
{
    m_GPR[rd()].DW = (m_GPR[rt()].W[0] << (m_GPR[rs()].UW[0] & 0x1F));
}

void R4300iInterpreter::SPECIAL_SRLV()
{
    m_GPR[rd()].DW = (int32_t)(m_GPR[rt()].UW[0] >> (m_GPR[rs()].UW[0] & 0x1F));
}

void R4300iInterpreter::SPECIAL_SRAV()
{
    m_GPR[rd()].DW = (int32_t)(m_GPR[rt()].DW >> (m_GPR[rs()].UW[0] & 0x1F));
}

void R4300iInterpreter::SPECIAL_JR()
{
    m_System.DelayedJump(m_GPR[rs()].DW);
    m_System.m_TestTimer = true;
}

void R4300iInterpreter::SPECIAL_JALR()
{
    m_System.DelayedJump(m_GPR[rs()].DW);
    m_GPR[rd()].DW = (int32_t)(m_System.m_PipelineStage == PIPELINE_STAGE_JUMP_DELAY_SLOT ? m_System.m_JumpToLocation + 4 : m_PROGRAM_COUNTER + 8);
    m_System.m_TestTimer = true;
}

void R4300iInterpreter::SPECIAL_SYSCALL()
{
    m_Reg.TriggerException(EXC_SYSCALL);
}

void R4300iInterpreter::SPECIAL_BREAK()
{
    m_Reg.TriggerException(EXC_BREAK);
}

void R4300iInterpreter::SPECIAL_SYNC()
{
}

void R4300iInterpreter::SPECIAL_MFHI()
{
    m_GPR[rd()].DW = m_RegHI.DW;
}

void R4300iInterpreter::SPECIAL_MTHI()
{
    m_RegHI.DW = m_GPR[rs()].DW;
}

void R4300iInterpreter::SPECIAL_MFLO()
{
    m_GPR[rd()].DW = m_RegLO.DW;
}

void R4300iInterpreter::SPECIAL_MTLO()
{
    m_RegLO.DW = m_GPR[rs()].DW;
}

void R4300iInterpreter::SPECIAL_DSLLV()
{
    m_GPR[rd()].DW = m_GPR[rt()].DW << (m_GPR[rs()].UW[0] & 0x3F);
}

void R4300iInterpreter::SPECIAL_DSRLV()
{
    m_GPR[rd()].UDW = m_GPR[rt()].UDW >> (m_GPR[rs()].UW[0] & 0x3F);
}

void R4300iInterpreter::SPECIAL_DSRAV()
{
    m_GPR[rd()].DW = m_GPR[rt()].DW >> (m_GPR[rs()].UW[0] & 0x3F);
}

void R4300iInterpreter::SPECIAL_MULT()
{
    m_RegHI.DW = (int64_t)(m_GPR[rs()].W[0]) * (int64_t)(m_GPR[rt()].W[0]);
    m_RegLO.DW = m_RegHI.W[0];
    m_RegHI.DW = m_RegHI.W[1];
}

void R4300iInterpreter::SPECIAL_MULTU()
{
    m_RegHI.DW = (uint64_t)(m_GPR[rs()].UW[0]) * (uint64_t)(m_GPR[rt()].UW[0]);
    m_RegLO.DW = m_RegHI.W[0];
    m_RegHI.DW = m_RegHI.W[1];
}

void R4300iInterpreter::SPECIAL_DIV()
{
    if (m_GPR[rt()].W[0] != 0)
    {
        if (m_GPR[rs()].W[0] != (int32_t)0x80000000 || m_GPR[rt()].W[0] != -1)
        {
            m_RegLO.DW = m_GPR[rs()].W[0] / m_GPR[rt()].W[0];
            m_RegHI.DW = m_GPR[rs()].W[0] % m_GPR[rt()].W[0];
        }
        else
        {
            m_RegLO.DW = (int64_t)0xFFFFFFFF80000000LL;
            m_RegHI.DW = 0x0000000000000000LL;
        }
    }
    else
    {
        m_RegLO.DW = m_GPR[rs()].W[0] < 0 ? 0x0000000000000001LL : (int64_t)0xFFFFFFFFFFFFFFFFLL;
        m_RegHI.DW = m_GPR[rs()].W[0];
    }
}

void R4300iInterpreter::SPECIAL_DIVU()
{
    if (m_GPR[rt()].UW[0] != 0)
    {
        m_RegLO.DW = (int32_t)(m_GPR[rs()].UW[0] / m_GPR[rt()].UW[0]);
        m_RegHI.DW = (int32_t)(m_GPR[rs()].UW[0] % m_GPR[rt()].UW[0]);
    }
    else
    {
        m_RegLO.DW = (int64_t)0xFFFFFFFFFFFFFFFFLL;
        m_RegHI.DW = m_GPR[rs()].W[0];
    }
}

void R4300iInterpreter::SPECIAL_DMULT()
{
    MIPS_DWORD Tmp[3];
    m_RegLO.UDW = (uint64_t)m_GPR[rs()].UW[0] * (uint64_t)m_GPR[rt()].UW[0];
    Tmp[0].UDW = (int64_t)m_GPR[rs()].W[1] * (int64_t)(uint64_t)m_GPR[rt()].UW[0];
    Tmp[1].UDW = (int64_t)(uint64_t)m_GPR[rs()].UW[0] * (int64_t)m_GPR[rt()].W[1];
    m_RegHI.UDW = (int64_t)m_GPR[rs()].W[1] * (int64_t)m_GPR[rt()].W[1];

    Tmp[2].UDW = (uint64_t)m_RegLO.UW[1] + (uint64_t)Tmp[0].UW[0] + (uint64_t)Tmp[1].UW[0];
    m_RegLO.UDW += ((uint64_t)Tmp[0].UW[0] + (uint64_t)Tmp[1].UW[0]) << 32;
    m_RegHI.UDW += (uint64_t)Tmp[0].W[1] + (uint64_t)Tmp[1].W[1] + Tmp[2].UW[1];
}

void R4300iInterpreter::SPECIAL_DMULTU()
{
    MIPS_DWORD Tmp[3];
    m_RegLO.UDW = (uint64_t)m_GPR[rs()].UW[0] * (uint64_t)m_GPR[rt()].UW[0];
    Tmp[0].UDW = (uint64_t)m_GPR[rs()].UW[1] * (uint64_t)m_GPR[rt()].UW[0];
    Tmp[1].UDW = (uint64_t)m_GPR[rs()].UW[0] * (uint64_t)m_GPR[rt()].UW[1];
    m_RegHI.UDW = (uint64_t)m_GPR[rs()].UW[1] * (uint64_t)m_GPR[rt()].UW[1];

    Tmp[2].UDW = (uint64_t)m_RegLO.UW[1] + (uint64_t)Tmp[0].UW[0] + (uint64_t)Tmp[1].UW[0];
    m_RegLO.UDW += ((uint64_t)Tmp[0].UW[0] + (uint64_t)Tmp[1].UW[0]) << 32;
    m_RegHI.UDW += (uint64_t)Tmp[0].UW[1] + (uint64_t)Tmp[1].UW[1] + Tmp[2].UW[1];
}

void R4300iInterpreter::SPECIAL_DDIV()
{
    if (m_GPR[rt()].UDW != 0)
    {
        if (m_GPR[rs()].DW != (int64_t)0x8000000000000000LL)
        {
            m_RegLO.DW = m_GPR[rs()].DW / m_GPR[rt()].DW;
            m_RegHI.DW = m_GPR[rs()].DW % m_GPR[rt()].DW;
        }
        else
        {
            m_RegLO.DW = m_GPR[rs()].DW;
            m_RegHI.DW = 0;
        }
    }
    else
    {
        m_RegLO.DW = m_GPR[rs()].DW < 0 ? 0x0000000000000001LL : (int64_t)0xFFFFFFFFFFFFFFFFLL;
        m_RegHI.DW = m_GPR[rs()].DW;
    }
}

void R4300iInterpreter::SPECIAL_DDIVU()
{
    if (m_GPR[rt()].UDW != 0)
    {
        m_RegLO.UDW = m_GPR[rs()].UDW / m_GPR[rt()].UDW;
        m_RegHI.UDW = m_GPR[rs()].UDW % m_GPR[rt()].UDW;
    }
    else
    {
        m_RegLO.UDW = 0xFFFFFFFFFFFFFFFFULL;
        m_RegHI.UDW = m_GPR[rs()].UDW;
    }
}

void R4300iInterpreter::SPECIAL_ADD()
{
    int32_t rsVal = m_GPR[rs()].W[0];
    int32_t rtVal = m_GPR[rt()].W[0];
    int32_t sum = rsVal + rtVal;
    if ((~(rsVal ^ rtVal) & (rsVal ^ sum)) & 0x80000000)
    {
        m_Reg.TriggerException(EXC_OV);
        return;
    }
    m_GPR[rd()].DW = sum;
}

void R4300iInterpreter::SPECIAL_ADDU()
{
    m_GPR[rd()].DW = (int32_t)(m_GPR[rs()].UW[0] + m_GPR[rt()].UW[0]);
}

void R4300iInterpreter::SPECIAL_SUB()
{
    int32_t rsVal = m_GPR[rs()].W[0];
    int32_t rtVal = m_GPR[rt()].W[0];
    int32_t sub = rsVal - rtVal;
    if (((rsVal ^ rtVal) & (rsVal ^ sub)) & 0x80000000)
    {
        m_Reg.TriggerException(EXC_OV);
        return;
    }
    m_GPR[rd()].DW = sub;
}

void R4300iInterpreter::SPECIAL_SUBU()
{
    m_GPR[rd()].DW = m_GPR[rs()].W[0] - m_GPR[rt()].W[0];
}

void R4300iInterpreter::SPECIAL_AND()
{
    m_GPR[rd()].DW = m_GPR[rs()].DW & m_GPR[rt()].DW;
}

void R4300iInterpreter::SPECIAL_OR()
{
    m_GPR[rd()].DW = m_GPR[rs()].DW | m_GPR[rt()].DW;
}

void R4300iInterpreter::SPECIAL_XOR()
{
    m_GPR[rd()].DW = m_GPR[rs()].DW ^ m_GPR[rt()].DW;
}

void R4300iInterpreter::SPECIAL_NOR()
{
    m_GPR[rd()].DW = ~(m_GPR[rs()].DW | m_GPR[rt()].DW);
}

void R4300iInterpreter::SPECIAL_SLT()
{
    m_GPR[rd()].DW = m_GPR[rs()].DW < m_GPR[rt()].DW ? 1 : 0;
}

void R4300iInterpreter::SPECIAL_SLTU()
{
    m_GPR[rd()].DW = m_GPR[rs()].UDW < m_GPR[rt()].UDW ? 1 : 0;
}

void R4300iInterpreter::SPECIAL_DADD()
{
    int64_t rsVal = m_GPR[rs()].DW;
    int64_t rtVal = m_GPR[rt()].DW;
    int64_t sum = rsVal + rtVal;
    if ((~(rsVal ^ rtVal) & (rsVal ^ sum)) & 0x8000000000000000LL)
    {
        m_Reg.TriggerException(EXC_OV);
        return;
    }
    m_GPR[rd()].DW = sum;
}

void R4300iInterpreter::SPECIAL_DADDU()
{
    m_GPR[rd()].DW = m_GPR[rs()].DW + m_GPR[rt()].DW;
}

void R4300iInterpreter::SPECIAL_DSUB()
{
    int64_t rsVal = m_GPR[rs()].DW;
    int64_t rtVal = m_GPR[rt()].DW;
    int64_t sub = rsVal - rtVal;
    if (((rsVal ^ rtVal) & (rsVal ^ sub)) & 0x8000000000000000LL)
    {
        m_Reg.TriggerException(EXC_OV);
        return;
    }
    m_GPR[rd()].DW = sub;
}

void R4300iInterpreter::SPECIAL_DSUBU()
{
    m_GPR[rd()].DW = m_GPR[rs()].DW - m_GPR[rt()].DW;
}

void R4300iInterpreter::SPECIAL_TEQ()
{
    if (m_GPR[rs()].DW == m_GPR[rt()].DW)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::SPECIAL_TGE()
{
    if (m_GPR[rs()].DW >= m_GPR[rt()].DW)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::SPECIAL_TGEU()
{
    if (m_GPR[rs()].UDW >= m_GPR[rt()].UDW)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::SPECIAL_TLT()
{
    if (m_GPR[rs()].DW < m_GPR[rt()].DW)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::SPECIAL_TLTU()
{
    if (m_GPR[rs()].UDW < m_GPR[rt()].UDW)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::SPECIAL_TNE()
{
    if (m_GPR[rs()].DW != m_GPR[rt()].DW)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::SPECIAL_DSLL()
{
    m_GPR[rd()].DW = (m_GPR[rt()].DW << sa());
}

void R4300iInterpreter::SPECIAL_DSRL()
{
    m_GPR[rd()].UDW = (m_GPR[rt()].UDW >> sa());
}

void R4300iInterpreter::SPECIAL_DSRA()
{
    m_GPR[rd()].DW = (m_GPR[rt()].DW >> sa());
}

void R4300iInterpreter::SPECIAL_DSLL32()
{
    m_GPR[rd()].DW = (m_GPR[rt()].DW << (sa() + 32));
}

void R4300iInterpreter::SPECIAL_DSRL32()
{
    m_GPR[rd()].UDW = (m_GPR[rt()].UDW >> (sa() + 32));
}

void R4300iInterpreter::SPECIAL_DSRA32()
{
    m_GPR[rd()].DW = (m_GPR[rt()].DW >> (sa() + 32));
}

// ============================================================================
// R4300i REGIMM Opcodes
// ============================================================================

void R4300iInterpreter::REGIMM_BLTZ()
{
    if (m_GPR[rs()].DW < 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
}

void R4300iInterpreter::REGIMM_BGEZ()
{
    if (m_GPR[rs()].DW >= 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
}

void R4300iInterpreter::REGIMM_BLTZL()
{
    if (m_GPR[rs()].DW < 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::REGIMM_BGEZL()
{
    if (m_GPR[rs()].DW >= 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::REGIMM_BLTZAL()
{
    if (m_GPR[rs()].DW < 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
    m_GPR[31].DW = (int32_t)(m_System.m_PipelineStage == PIPELINE_STAGE_JUMP_DELAY_SLOT ? m_System.m_JumpToLocation + 4 : m_PROGRAM_COUNTER + 8);
}

void R4300iInterpreter::REGIMM_BGEZAL()
{
    if (m_GPR[rs()].DW >= 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.DelayedJump(m_PROGRAM_COUNTER + 8);
    }
    m_GPR[31].DW = (int32_t)(m_System.m_PipelineStage == PIPELINE_STAGE_JUMP_DELAY_SLOT ? m_System.m_JumpToLocation + 4 : m_PROGRAM_COUNTER + 8);
}

void R4300iInterpreter::REGIMM_BLTZALL()
{
    if (m_GPR[rs()].DW < 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
    m_GPR[31].DW = (int32_t)(m_System.m_PipelineStage == PIPELINE_STAGE_JUMP_DELAY_SLOT ? m_System.m_JumpToLocation + 4 : m_PROGRAM_COUNTER + 8);
}

void R4300iInterpreter::REGIMM_BGEZALL()
{
    if (m_GPR[rs()].DW >= 0)
    {
        m_System.DelayedRelativeJump((offset() << 2) + 4);
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
    m_GPR[31].DW = (int32_t)(m_System.m_PipelineStage == PIPELINE_STAGE_JUMP_DELAY_SLOT ? m_System.m_JumpToLocation + 4 : m_PROGRAM_COUNTER + 8);
}

void R4300iInterpreter::REGIMM_TEQI()
{
    if (m_GPR[rs()].DW == (int64_t)((int16_t)immediate()))
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::REGIMM_TGEI()
{
    if (m_GPR[rs()].DW >= (int64_t)((int16_t)immediate()))
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::REGIMM_TGEIU()
{
    int32_t imm32 = (int16_t)immediate();
    int64_t imm64 = imm32;
    if (m_GPR[rs()].UDW >= (uint64_t)imm64)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::REGIMM_TLTI()
{
    if (m_GPR[rs()].DW < (int64_t)((int16_t)immediate()))
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::REGIMM_TLTIU()
{
    int32_t imm32 = (int16_t)immediate();
    int64_t imm64 = imm32;
    if (m_GPR[rs()].UDW < (uint64_t)imm64)
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

void R4300iInterpreter::REGIMM_TNEI()
{
    if (m_GPR[rs()].DW != (int64_t)((int16_t)immediate()))
    {
        m_Reg.TriggerException(EXC_TRAP);
    }
}

// ============================================================================
// COP0 Functions
// ============================================================================

void R4300iInterpreter::COP0_MF()
{
    m_GPR[rt()].DW = (int32_t)m_Reg.Cop0_MF((CRegisters::COP0Reg)rd());
}

void R4300iInterpreter::COP0_DMF()
{
    m_GPR[rt()].DW = m_Reg.Cop0_MF((CRegisters::COP0Reg)rd());
}

void R4300iInterpreter::COP0_MT()
{
    m_Reg.Cop0_MT((CRegisters::COP0Reg)rd(), (int64_t)m_GPR[rt()].W[0]);
}

void R4300iInterpreter::COP0_DMT()
{
    m_Reg.Cop0_MT((CRegisters::COP0Reg)rd(), m_GPR[rt()].UDW);
}

// COP0 CO Functions

void R4300iInterpreter::COP0_CO_TLBR()
{
    m_TLB.ReadEntry();
}

void R4300iInterpreter::COP0_CO_TLBWI()
{
    m_TLB.WriteEntry(m_Reg.INDEX_REGISTER & 0x1F, false);
}

void R4300iInterpreter::COP0_CO_TLBWR()
{
    // TLBWR writes the entry selected by the Random register. Read it rather
    // than recomputing so both paths agree on the same deterministic value.
    const uint32_t index = static_cast<uint32_t>(m_Reg.Cop0_MF(CRegisters::COP0Reg::Random)) & 0x1F;
    m_TLB.WriteEntry(index, true);
}

void R4300iInterpreter::COP0_CO_TLBP()
{
    m_TLB.Probe();
}

void R4300iInterpreter::COP0_CO_ERET()
{
    m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
    if ((m_Reg.STATUS_REGISTER.ErrorLevel) != 0)
    {
        m_System.m_JumpToLocation = m_Reg.ERROREPC_REGISTER;
        m_Reg.STATUS_REGISTER.ErrorLevel = 0;
    }
    else
    {
        m_System.m_JumpToLocation = m_Reg.EPC_REGISTER;
        m_Reg.STATUS_REGISTER.ExceptionLevel = 0;
    }
    m_LLBit = 0;
    m_Reg.CheckInterrupts();
    m_System.m_TestTimer = true;
}

void R4300iInterpreter::COP0_CO_WAIT()
{
    // The WAIT instruction pauses execution until an interrupt occurs.
    // In our emulator, we can treat it as a NOOP. The fast-forward features
    // in the recompiler and interpreter loop handle fast-forwarding the timer.
}

// ============================================================================
// COP1 Functions
// ============================================================================

void R4300iInterpreter::CPO1_UNIMPLEMENTED_OP()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode))
    {
        return;
    }
    FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
    StatusReg.Cause.UnimplementedOperation = 1;
    m_Reg.TriggerException(EXC_FPE);
}

void R4300iInterpreter::COP1_MF()
{
    if (TestCop1UsableException())
    {
        return;
    }
    m_GPR[rt()].DW = *(int32_t *)m_FPR_S[fs()];
}

void R4300iInterpreter::COP1_DMF()
{
    if (TestCop1UsableException())
    {
        return;
    }
    m_GPR[rt()].DW = *(int64_t *)m_FPR_D[fs()];
}

void R4300iInterpreter::COP1_CF()
{
    if (TestCop1UsableException())
    {
        return;
    }
    m_GPR[rt()].DW = (int32_t)m_FPCR[fs()];
}

void R4300iInterpreter::COP1_MT()
{
    if (TestCop1UsableException())
    {
        return;
    }
    *(int32_t *)m_FPR_S[fs()] = m_GPR[rt()].W[0];
}

void R4300iInterpreter::COP1_DMT()
{
    if (TestCop1UsableException())
    {
        return;
    }
    *(int64_t *)m_FPR_D[fs()] = m_GPR[rt()].DW;
}

void R4300iInterpreter::COP1_CT()
{
    if (TestCop1UsableException())
    {
        return;
    }
    m_Reg.Cop1_CT(fs(), m_GPR[rt()].W[0]);
}

// COP1: BC1 Functions

void R4300iInterpreter::COP1_BCF()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode))
    {
        return;
    }
    m_System.m_PipelineStage = PIPELINE_STAGE_DELAY_SLOT;
    if ((m_FPCR[31] & FPCSR_C) == 0)
    {
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + ((int16_t)offset() << 2) + 4;
    }
    else
    {
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::COP1_BCT()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode))
    {
        return;
    }
    m_System.m_PipelineStage = PIPELINE_STAGE_DELAY_SLOT;
    if ((m_FPCR[31] & FPCSR_C) != 0)
    {
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + ((int16_t)offset() << 2) + 4;
    }
    else
    {
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::COP1_BCFL()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode))
    {
        return;
    }
    if ((m_FPCR[31] & FPCSR_C) == 0)
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_DELAY_SLOT;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + ((int16_t)offset() << 2) + 4;
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

void R4300iInterpreter::COP1_BCTL()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode))
    {
        return;
    }
    if ((m_FPCR[31] & FPCSR_C) != 0)
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_DELAY_SLOT;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + ((int16_t)offset() << 2) + 4;
    }
    else
    {
        m_System.m_PipelineStage = PIPELINE_STAGE_JUMP;
        m_System.m_JumpToLocation = m_PROGRAM_COUNTER + 8;
    }
}

// ============================================================================
// COP1: Single Precision Functions
// ============================================================================

void R4300iInterpreter::COP1_S_ADD()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs32(*(uint32_t *)m_FPR_S_L[fs()], *(uint32_t *)m_FPR_UW[ft()])) { return; }
    float32_t Result = f32_add(*(float32_t *)m_FPR_S_L[fs()], *(float32_t *)m_FPR_UW[ft()]);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_SUB()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs32(*(uint32_t *)m_FPR_S_L[fs()], *(uint32_t *)m_FPR_UW[ft()])) { return; }
    float32_t Result = f32_sub(*(float32_t *)m_FPR_S_L[fs()], *(float32_t *)m_FPR_UW[ft()]);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_MUL()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs32(*(uint32_t *)m_FPR_S_L[fs()], *(uint32_t *)m_FPR_UW[ft()])) { return; }
    float32_t Result = f32_mul(*(float32_t *)m_FPR_S_L[fs()], *(float32_t *)m_FPR_UW[ft()]);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_DIV()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs32(*(uint32_t *)m_FPR_S_L[fs()], *(uint32_t *)m_FPR_UW[ft()])) { return; }
    float32_t Result = f32_div(*(float32_t *)m_FPR_S_L[fs()], *(float32_t *)m_FPR_UW[ft()]);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_SQRT()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput32(*(uint32_t *)m_FPR_S_L[fs()])) { return; }
    float32_t Result = f32_sqrt(*(float32_t *)(m_FPR_S_L[fs()]));
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_ABS()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput32(*(uint32_t *)m_FPR_S_L[fs()])) { return; }
    float32_t Result;
    Result.v = ((float32_t *)(m_FPR_S_L[fs()]))->v & 0x7FFFFFFF;
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_MOV()
{
    if (TestCop1UsableException()) { return; }
    *m_FPR_UW[fd()] = *m_FPR_UW[fs()];
}

void R4300iInterpreter::COP1_S_NEG()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput32(*(uint32_t *)m_FPR_S_L[fs()])) { return; }
    float Result = (*(float *)m_FPR_S_L[fs()] * -1.0f);
    if (CheckFPUResult32(Result)) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_S_ROUND_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundToNearest)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int64_t Result = f32_to_i64(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_S_TRUNC_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardZero)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int64_t Result = f32_to_i64(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_S_CEIL_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardPlusInfinity)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int64_t Result = f32_to_i64(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_S_FLOOR_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardMinusInfinity)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int64_t Result = f32_to_i64(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_S_ROUND_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundToNearest)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int32_t Result = f32_to_i32(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_S_TRUNC_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardZero)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int32_t Result = f32_to_i32(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_S_CEIL_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardPlusInfinity)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int32_t Result = f32_to_i32(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_S_FLOOR_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardMinusInfinity)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int32_t Result = f32_to_i32(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_S_CVT_D()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput32(*(uint32_t *)m_FPR_S_L[fs()])) { return; }
    float64_t Result = f32_to_f64(*(float32_t *)m_FPR_S_L[fs()]);
    if (CheckFPUResult64(*(double *)&Result)) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_S_CVT_W()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int32_t Result = f32_to_i32(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_S_CVT_L()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput32Conv(*(float *)m_FPR_S_L[fs()])) { return; }
    int64_t Result = f32_to_i64(*(float32_t *)m_FPR_S_L[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_S_CMP()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    float Temp0 = *(float *)m_FPR_S_L[fs()];
    float Temp1 = *(float *)m_FPR_UW[ft()];

    bool less, equal, unorded;
    if (std::isnan(Temp0) || std::isnan(Temp1))
    {
        less = false;
        equal = false;
        unorded = true;

        bool QuietNan = false;
        if ((*(uint32_t *)m_FPR_S_L[fs()] >= 0x7FC00000 && *(uint32_t *)m_FPR_S_L[fs()] <= 0x7FFFFFFF) ||
            (*(uint32_t *)m_FPR_S_L[fs()] >= 0xFFC00000 && *(uint32_t *)m_FPR_S_L[fs()] <= 0xFFFFFFFF))
        {
            QuietNan = true;
        }
        else if ((*(uint32_t *)m_FPR_UW[ft()] >= 0x7FC00000 && *(uint32_t *)m_FPR_UW[ft()] <= 0x7FFFFFFF) ||
                 (*(uint32_t *)m_FPR_UW[ft()] >= 0xFFC00000 && *(uint32_t *)m_FPR_UW[ft()] <= 0xFFFFFFFF))
        {
            QuietNan = true;
        }
        if ((funct() & 8) != 0 || QuietNan)
        {
            FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
            StatusReg.Cause.InvalidOperation = 1;
            if (StatusReg.Enable.InvalidOperation)
            {
                m_Reg.TriggerException(EXC_FPE);
                return;
            }
            else
            {
                StatusReg.Flags.InvalidOperation = 1;
            }
        }
    }
    else
    {
        less = Temp0 < Temp1;
        equal = Temp0 == Temp1;
        unorded = false;
    }

    int32_t condition = ((funct() & 4) && less) || ((funct() & 2) && equal) || ((funct() & 1) && unorded);
    if (condition != 0)
    {
        m_FPCR[31] |= FPCSR_C;
    }
    else
    {
        m_FPCR[31] &= ~FPCSR_C;
    }
}

// ============================================================================
// COP1: Double Precision Functions
// ============================================================================

void R4300iInterpreter::COP1_D_ADD()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs64(*(double *)m_FPR_D[fs()], *(double *)m_FPR_UDW[ft()])) { return; }
    float64_t Result = f64_add(*(float64_t *)m_FPR_D[fs()], *(float64_t *)m_FPR_UDW[ft()]);
    if (CheckFPUResult64(*((double *)&Result))) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_D_SUB()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs64(*(double *)m_FPR_D[fs()], *(double *)m_FPR_UDW[ft()])) { return; }
    float64_t Result = f64_sub(*(float64_t *)m_FPR_D[fs()], *(float64_t *)m_FPR_UDW[ft()]);
    if (CheckFPUResult64(*((double *)&Result))) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_D_MUL()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInputs64(*(double *)m_FPR_D[fs()], *(double *)m_FPR_UDW[ft()])) { return; }
    float64_t Result = f64_mul(*(float64_t *)m_FPR_D[fs()], *(float64_t *)m_FPR_UDW[ft()]);
    if (CheckFPUResult64(*((double *)&Result))) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_D_DIV()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    // Debug: log inputs and FPCSR before CheckFPUInputs64
    {
        static bool loggedDivD = false;
        if (!loggedDivD) {
            [[maybe_unused]] double fsVal = *(double *)m_FPR_D[fs()];
            double ftVal = *(double *)m_FPR_D[ft()];
            uint64_t fsRaw = *(uint64_t *)m_FPR_D[fs()];
            uint64_t ftRaw = *(uint64_t *)m_FPR_UDW[ft()];
            bool fsSubn = ((fsRaw & 0x7FF0000000000000ULL) == 0 && (fsRaw & 0x000FFFFFFFFFFFFFULL) != 0);
            bool ftSubn = ((ftRaw & 0x7FF0000000000000ULL) == 0 && (ftRaw & 0x000FFFFFFFFFFFFFULL) != 0);
            if (fsSubn || ftSubn || ftVal == 0.0) {
                loggedDivD = true;
                Log::infof("[DIV_D_DEBUG] PC=0x%08X fs=%d ft=%d fd=%d FPCSR=0x%08X",
                           (uint32_t)m_PROGRAM_COUNTER, fs(), ft(), fd(), m_FPCR[31]);
                Log::infof("[DIV_D_DEBUG] fsRaw=0x%016llX ftRaw=0x%016llX fsSubn=%d ftSubn=%d ftIsZero=%d",
                           (unsigned long long)fsRaw, (unsigned long long)ftRaw,
                           fsSubn, ftSubn, (ftVal == 0.0) ? 1 : 0);
            }
        }
    }
    if (CheckFPUInputs64(*(double *)m_FPR_D[fs()], *(double *)m_FPR_UDW[ft()])) { return; }
    float64_t Result = f64_div(*(float64_t *)m_FPR_D[fs()], *(float64_t *)m_FPR_UDW[ft()]);
    if (CheckFPUResult64(*((double *)&Result))) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_D_SQRT()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput64(*(double *)m_FPR_D[fs()])) { return; }
    float64_t Result = f64_sqrt(*(float64_t *)m_FPR_D[fs()]);
    if (CheckFPUResult64(*((double *)&Result))) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_D_ABS()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput64(*(double *)m_FPR_D[fs()])) { return; }
    double Result = fabs(*(double *)m_FPR_D[fs()]);
    if (CheckFPUResult64(Result)) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_D_MOV()
{
    if (TestCop1UsableException()) { return; }
    *m_FPR_UDW[fd()] = *(int64_t *)m_FPR_D[fs()];
}

void R4300iInterpreter::COP1_D_NEG()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput64(*(double *)m_FPR_D[fs()])) { return; }
    double Result = (*(double *)m_FPR_D[fs()] * -1.0);
    if (CheckFPUResult64(Result)) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

void R4300iInterpreter::COP1_D_ROUND_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundToNearest)) { return; }
    const double & fsVal = *(double *)m_FPR_D[fs()];
    if (CheckFPUInput64Conv(fsVal)) { return; }
    int64_t Result = f64_to_i64(*(float64_t *)&fsVal, softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = (uint64_t)Result;
}

void R4300iInterpreter::COP1_D_TRUNC_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardZero)) { return; }
    const double & fsVal = *(double *)m_FPR_D[fs()];
    if (CheckFPUInput64Conv(fsVal)) { return; }
    int64_t Result = f64_to_i64(*(float64_t *)&fsVal, softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = (uint64_t)Result;
}

void R4300iInterpreter::COP1_D_CEIL_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardPlusInfinity)) { return; }
    const double & fsVal = *(double *)m_FPR_D[fs()];
    if (CheckFPUInput64Conv(fsVal)) { return; }
    int64_t Result = f64_to_i64(*(float64_t *)&fsVal, softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = (uint64_t)Result;
}

void R4300iInterpreter::COP1_D_FLOOR_L()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardMinusInfinity)) { return; }
    const double & fsVal = *(double *)m_FPR_D[fs()];
    if (CheckFPUInput64Conv(fsVal)) { return; }
    int64_t Result = f64_to_i64(*(float64_t *)&fsVal, softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = (uint64_t)Result;
}

void R4300iInterpreter::COP1_D_ROUND_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundToNearest)) { return; }
    if (CheckFPUInput64Conv(*(double *)m_FPR_D[fs()])) { return; }
    int32_t Result = f64_to_i32(*(float64_t *)m_FPR_D[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_D_TRUNC_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardZero)) { return; }
    if (CheckFPUInput64Conv(*(double *)m_FPR_D[fs()])) { return; }
    int32_t Result = f64_to_i32(*(float64_t *)m_FPR_D[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_D_CEIL_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardPlusInfinity)) { return; }
    if (CheckFPUInput64Conv(*(double *)m_FPR_D[fs()])) { return; }
    int32_t Result = f64_to_i32(*(float64_t *)m_FPR_D[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_D_FLOOR_W()
{
    if (InitFpuOperation(FPRoundingMode_RoundTowardMinusInfinity)) { return; }
    if (CheckFPUInput64Conv(*(double *)m_FPR_D[fs()])) { return; }
    int32_t Result = f64_to_i32(*(float64_t *)m_FPR_D[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_D_CVT_S()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput64(*(double *)m_FPR_D[fs()])) { return; }
    float32_t Result = f64_to_f32(*(float64_t *)m_FPR_D[fs()]);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_D_CVT_W()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    if (CheckFPUInput64Conv(*(double *)m_FPR_D[fs()])) { return; }
    int32_t Result = f64_to_i32(*(float64_t *)m_FPR_D[fs()], softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UW[fd()] = *(uint32_t *)&Result;
}

void R4300iInterpreter::COP1_D_CVT_L()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    const double & fsVal = *(double *)m_FPR_D[fs()];
    if (CheckFPUInput64Conv(fsVal)) { return; }
    int64_t Result = f64_to_i64(*((float64_t *)&fsVal), softfloat_roundingMode, true);
    if (CheckFPUInvalidException()) { return; }
    *m_FPR_UDW[fd()] = (uint64_t)Result;
}

void R4300iInterpreter::COP1_D_CMP()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }

    MIPS_DWORD Temp0, Temp1;
    Temp0.DW = *(int64_t *)m_FPR_D[fs()];
    Temp1.DW = *(int64_t *)m_FPR_UDW[ft()];

    bool less, equal, unorded;
    if (std::isnan(Temp0.D) || std::isnan(Temp1.D))
    {
        less = false;
        equal = false;
        unorded = true;

        bool QuietNan = false;
        if ((*(uint64_t *)m_FPR_D[fs()] >= 0x7FF8000000000000ULL && *(uint64_t *)m_FPR_D[fs()] <= 0x7FFFFFFFFFFFFFFFULL) ||
            (*(uint64_t *)m_FPR_D[fs()] >= 0xFFF8000000000000ULL && *(uint64_t *)m_FPR_D[fs()] <= 0xFFFFFFFFFFFFFFFFULL))
        {
            QuietNan = true;
        }
        else if ((*(uint64_t *)m_FPR_UDW[ft()] >= 0x7FF8000000000000ULL && *(uint64_t *)m_FPR_UDW[ft()] <= 0x7FFFFFFFFFFFFFFFULL) ||
                 (*(uint64_t *)m_FPR_UDW[ft()] >= 0xFFF8000000000000ULL && *(uint64_t *)m_FPR_UDW[ft()] <= 0xFFFFFFFFFFFFFFFFULL))
        {
            QuietNan = true;
        }

        if ((funct() & 8) != 0 || QuietNan)
        {
            FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
            StatusReg.Cause.InvalidOperation = 1;
            if (StatusReg.Enable.InvalidOperation)
            {
                m_Reg.TriggerException(EXC_FPE);
                return;
            }
            else
            {
                StatusReg.Flags.InvalidOperation = 1;
            }
        }
    }
    else
    {
        less = Temp0.D < Temp1.D;
        equal = Temp0.D == Temp1.D;
        unorded = false;
    }

    int32_t condition = ((funct() & 4) && less) || ((funct() & 2) && equal) || ((funct() & 1) && unorded);
    if (condition != 0)
    {
        m_FPCR[31] |= FPCSR_C;
    }
    else
    {
        m_FPCR[31] &= ~FPCSR_C;
    }
}

// ============================================================================
// COP1: Word Functions
// ============================================================================

void R4300iInterpreter::COP1_W_CVT_S()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    float32_t Result = i32_to_f32(*(int32_t *)m_FPR_S_L[fs()]);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_W_CVT_D()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    double Result = (double)*(int32_t *)m_FPR_S_L[fs()];
    if (CheckFPUResult64(Result)) { return; }
    *m_FPR_UDW[fd()] = *(uint64_t *)&Result;
}

// ============================================================================
// COP1: Long Functions
// ============================================================================

void R4300iInterpreter::COP1_L_CVT_S()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    int64_t fsVal = *(int64_t *)m_FPR_D[fs()];
    if (fsVal >= (int64_t)0x0080000000000000ULL || fsVal < (int64_t)0xff80000000000000ULL)
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        StatusReg.Cause.UnimplementedOperation = 1;
        m_Reg.TriggerException(EXC_FPE);
        return;
    }
    float32_t Result = i64_to_f32(fsVal);
    if (CheckFPUResult32(*(float *)&Result)) { return; }
    *m_FPR_UW[fd()] = Result.v;
}

void R4300iInterpreter::COP1_L_CVT_D()
{
    if (InitFpuOperation(((FPStatusReg &)m_FPCR[31]).RoundingMode)) { return; }
    int64_t fsVal = *(int64_t *)m_FPR_D[fs()];
    if (fsVal >= (int64_t)0x0080000000000000ULL || fsVal < (int64_t)0xff80000000000000ULL)
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        StatusReg.Cause.UnimplementedOperation = 1;
        m_Reg.TriggerException(EXC_FPE);
        return;
    }
    float64_t Result = i64_to_f64(fsVal);
    if (CheckFPUResult64(*(double *)&Result)) { return; }
    *m_FPR_UDW[fd()] = Result.v;
}

// ============================================================================
// COP2 Functions
// ============================================================================

void R4300iInterpreter::CPO2_INVALID_OP()
{
    m_Reg.TriggerException(m_Reg.STATUS_REGISTER.CU2 == 0 ? EXC_CPU : EXC_II, 2);
}

void R4300iInterpreter::COP2_MF()
{
    if (m_Reg.STATUS_REGISTER.CU2 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 2);
    }
    else
    {
        m_GPR[rt()].DW = (int32_t)m_Reg.Cop2_MF(rd());
    }
}

void R4300iInterpreter::COP2_DMF()
{
    if (m_Reg.STATUS_REGISTER.CU2 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 2);
    }
    else
    {
        m_GPR[rt()].DW = m_Reg.Cop2_MF(rd());
    }
}

void R4300iInterpreter::COP2_CF()
{
    if (m_Reg.STATUS_REGISTER.CU2 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 2);
    }
    else
    {
        m_GPR[rt()].DW = (int32_t)m_Reg.Cop2_MF(rd());
    }
}

void R4300iInterpreter::COP2_MT()
{
    if (m_Reg.STATUS_REGISTER.CU2 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 2);
    }
    else
    {
        m_Reg.Cop2_MT(rd(), m_GPR[rt()].DW);
    }
}

void R4300iInterpreter::COP2_DMT()
{
    if (m_Reg.STATUS_REGISTER.CU2 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 2);
    }
    else
    {
        m_Reg.Cop2_MT(rd(), m_GPR[rt()].DW);
    }
}

void R4300iInterpreter::COP2_CT()
{
    if (m_Reg.STATUS_REGISTER.CU2 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 2);
    }
    else
    {
        m_Reg.Cop2_MT(rd(), m_GPR[rt()].DW);
    }
}

// ============================================================================
// Error Handling
// ============================================================================

void R4300iInterpreter::ReservedInstruction()
{
    m_Reg.TriggerException(EXC_II);
}

void R4300iInterpreter::UnknownOpcode()
{
    // Treat any unrecognized opcode as a Reserved Instruction exception,
    // exactly like ReservedInstruction(). Setting m_EndEmulation here is
    // wrong: it silently kills emulation whenever the recompiler falls back
    // to InterpretSingleInstruction() for an instruction the dispatch table
    // doesn't recognise.
    m_Reg.TriggerException(EXC_II);
}

// ============================================================================
// FPU Helper Functions
// ============================================================================

bool R4300iInterpreter::TestCop1UsableException()
{
    if (m_Reg.STATUS_REGISTER.CU1 == 0)
    {
        m_Reg.TriggerException(EXC_CPU, 1);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInput32(const uint32_t & Value)
{
    bool Exception = false;
    if ((Value & 0x7F800000) == 0x00000000 && (Value & 0x007FFFFF) != 0x00000000) // Sub Normal
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (StatusReg.FlushSubnormals)
        {
            // FS=1: Flush subnormal inputs to zero silently
        }
        else
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
    }
    else if ((Value & 0x7F800000) == 0x7F800000 && (Value & 0x007FFFFF) != 0x00000000) // Nan
    {
        uint32_t Value32 = *(uint32_t *)&Value;
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if ((Value32 >= 0x7F800001 && Value32 < 0x7FC00000) ||
            (Value32 >= 0xFF800001 && Value32 < 0xFFC00000))
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
        else
        {
            StatusReg.Cause.InvalidOperation = 1;
            if (StatusReg.Enable.InvalidOperation)
            {
                Exception = true;
            }
            else
            {
                StatusReg.Flags.InvalidOperation = 1;
            }
        }
    }
    if (Exception)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInputs32(const uint32_t & Value, const uint32_t & Value2)
{
    bool Exception = false;
    bool isNan[2] = {
        ((Value & 0x7F800000) == 0x7F800000 && (Value & 0x007FFFFF) != 0x00000000),
        ((Value2 & 0x7F800000) == 0x7F800000 && (Value2 & 0x007FFFFF) != 0x00000000),
    };
    bool isQNan[2] = {
        ((Value >= 0x7F800001 && Value < 0x7FC00000) || (Value >= 0xFF800001 && Value < 0xFFC00000)),
        ((Value2 >= 0x7F800001 && Value2 < 0x7FC00000) || (Value2 >= 0xFF800001 && Value2 < 0xFFC00000)),
    };
    bool isSubNormal[2] = {
        ((Value & 0x7F800000) == 0x00000000 && (Value & 0x007FFFFF) != 0x00000000),
        ((Value2 & 0x7F800000) == 0x00000000 && (Value2 & 0x007FFFFF) != 0x00000000),
    };

    if (isSubNormal[0] || isSubNormal[1])
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (StatusReg.FlushSubnormals)
        {
            // FS=1: Flush subnormal inputs to zero silently
        }
        else
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
    }
    else if (isNan[0] || isNan[1])
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (isQNan[0] || isQNan[1])
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
        else
        {
            StatusReg.Cause.InvalidOperation = 1;
            if (StatusReg.Enable.InvalidOperation)
            {
                Exception = true;
            }
            else
            {
                StatusReg.Flags.InvalidOperation = 1;
            }
        }
    }
    if (Exception)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInput32Conv(const float & Value)
{
    uint32_t InvalidValueMax = 0x5a000000, InvalidMinValue = 0xda000000;
    int Type = fpclassify(Value);
    if (Type == FP_SUBNORMAL || Type == FP_INFINITE || Type == FP_NAN ||
        Value >= *(float *)&InvalidValueMax || Value <= *(float *)&InvalidMinValue)
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        StatusReg.Cause.UnimplementedOperation = 1;
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInput64(const double & Value)
{
    bool Exception = false;
    if ((*((uint64_t *)&Value) & 0x7FF0000000000000ULL) == 0x0000000000000000ULL && (*((uint64_t *)&Value) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL)
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (StatusReg.FlushSubnormals)
        {
            // FS=1: Flush subnormal inputs to zero silently
        }
        else
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
    }
    else if ((*((uint64_t *)&Value) & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (*((uint64_t *)&Value) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL)
    {
        uint64_t Value64 = *(uint64_t *)&Value;
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if ((Value64 >= 0x7FF0000000000001ULL && Value64 <= 0x7FF7FFFFFFFFFFFFULL) ||
            (Value64 >= 0xFFF0000000000001ULL && Value64 <= 0xFFF7FFFFFFFFFFFFULL))
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
        else
        {
            StatusReg.Cause.InvalidOperation = 1;
            if (StatusReg.Enable.InvalidOperation)
            {
                Exception = true;
            }
            else
            {
                StatusReg.Flags.InvalidOperation = 1;
            }
        }
    }
    if (Exception)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInputs64(const double & Value, const double & Value2)
{
    bool isNan[2] = {
        ((*((uint64_t *)&Value) & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (*((uint64_t *)&Value) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL),
        ((*((uint64_t *)&Value2) & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (*((uint64_t *)&Value2) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL),
    };
    bool isQNan[2] = {
        ((*(uint64_t *)&Value >= 0x7FF0000000000001ULL && *(uint64_t *)&Value <= 0x7FF7FFFFFFFFFFFFULL) || (*(uint64_t *)&Value >= 0xFFF0000000000001ULL && *(uint64_t *)&Value <= 0xFFF7FFFFFFFFFFFFULL)),
        ((*(uint64_t *)&Value2 >= 0x7FF0000000000001ULL && *(uint64_t *)&Value2 <= 0x7FF7FFFFFFFFFFFFULL) || (*(uint64_t *)&Value2 >= 0xFFF0000000000001ULL && *(uint64_t *)&Value2 <= 0xFFF7FFFFFFFFFFFFULL)),
    };
    bool isSubNormal[2] = {
        ((*((uint64_t *)&Value) & 0x7FF0000000000000ULL) == 0x0000000000000000ULL && (*((uint64_t *)&Value) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL),
        ((*((uint64_t *)&Value2) & 0x7FF0000000000000ULL) == 0x0000000000000000ULL && (*((uint64_t *)&Value2) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL),
    };

    bool Exception = false;
    if (isSubNormal[0] || isSubNormal[1])
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (StatusReg.FlushSubnormals)
        {
            // FS=1: Flush subnormal inputs to zero instead of raising exception.
            // The MIPS R4300i with FS=1 should not trigger UnimplementedOperation
            // for subnormal inputs — they are silently flushed.
            // (Note: the caller still passes the original pointers, so we can't
            // modify the inputs here. But the softfloat library will handle the
            // flushed zero values correctly if we just continue.)
        }
        else
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
    }
    else if (isNan[0] || isNan[1])
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (isQNan[0] || isQNan[1])
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            Exception = true;
        }
        else
        {
            StatusReg.Cause.InvalidOperation = 1;
            if (StatusReg.Enable.InvalidOperation)
            {
                Exception = true;
            }
            else
            {
                StatusReg.Flags.InvalidOperation = 1;
            }
        }
    }
    if (Exception)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInput64Conv(const double & Value)
{
    uint64_t InvalidValueMax = 0x4340000000000000ULL, InvalidMinValue = 0xc340000000000000ULL;
    int Type = fpclassify(Value);
    if (Type == FP_SUBNORMAL || Type == FP_INFINITE || Type == FP_NAN ||
        Value >= *(double *)&InvalidValueMax || Value <= *(double *)&InvalidMinValue)
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        StatusReg.Cause.UnimplementedOperation = 1;
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUResult32(float & Result)
{
    bool DoException = false;

    if ((*((uint32_t *)&Result) & 0x7F800000) == 0x7F800000 && (*((uint32_t *)&Result) & 0x007FFFFF) != 0x00000000) // Nan
    {
        if (softfloat_exceptionFlags == 0 || !SetFPUException())
        {
            *((uint32_t *)&Result) = 0x7fbfffff;
        }
        else
        {
            DoException = true;
        }
    }
    else if ((*((uint32_t *)&Result) & 0x7F800000) == 0x00000000 && (*((uint32_t *)&Result) & 0x007FFFFF) != 0x00000000) // Sub Normal
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (!StatusReg.FlushSubnormals || StatusReg.Enable.Underflow || StatusReg.Enable.Inexact)
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            DoException = true;
        }
        else if (softfloat_exceptionFlags == 0 || !SetFPUException())
        {
            StatusReg.Cause.Underflow = 1;
            StatusReg.Flags.Underflow = 1;
            StatusReg.Cause.Inexact = 1;
            StatusReg.Flags.Inexact = 1;

            switch (StatusReg.RoundingMode)
            {
            case FPRoundingMode_RoundToNearest:
            case FPRoundingMode_RoundTowardZero:
                Result = Result >= 0.0f ? 0.0f : -0.0f;
                break;
            case FPRoundingMode_RoundTowardPlusInfinity:
                Result = Result >= 0.0f ? 1.175494351e-38F : -0.0f;
                break;
            case FPRoundingMode_RoundTowardMinusInfinity:
                Result = Result >= 0.0f ? 0.0f : -1.175494351e-38F;
                break;
            }
        }
        else
        {
            DoException = true;
        }
    }
    else if (softfloat_exceptionFlags != 0 && SetFPUException())
    {
        DoException = true;
    }
    if (DoException)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUResult64(double & Result)
{
    bool DoException = false;
    if ((*((uint64_t *)&Result) & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (*((uint64_t *)&Result) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL) // NaN
    {
        if (softfloat_exceptionFlags == 0 || !SetFPUException())
        {
            *((uint64_t *)&Result) = 0x7FF7FFFFFFFFFFFFULL;
        }
        else
        {
            DoException = true;
        }
    }
    else if ((*((uint64_t *)&Result) & 0x7FF0000000000000ULL) == 0x0000000000000000ULL && (*((uint64_t *)&Result) & 0x000FFFFFFFFFFFFFULL) != 0x0000000000000000ULL)
    {
        FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
        if (!StatusReg.FlushSubnormals || StatusReg.Enable.Underflow || StatusReg.Enable.Inexact)
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            DoException = true;
        }
        else if (softfloat_exceptionFlags == 0 || !SetFPUException())
        {
            StatusReg.Cause.Underflow = 1;
            StatusReg.Flags.Underflow = 1;
            StatusReg.Cause.Inexact = 1;
            StatusReg.Flags.Inexact = 1;

            switch (StatusReg.RoundingMode)
            {
            case FPRoundingMode_RoundToNearest:
            case FPRoundingMode_RoundTowardZero:
                Result = Result >= 0.0 ? 0.0 : -0.0;
                break;
            case FPRoundingMode_RoundTowardPlusInfinity:
                Result = Result >= 0.0 ? 2.2250738585072014e-308 : -0.0;
                break;
            case FPRoundingMode_RoundTowardMinusInfinity:
                Result = Result >= 0.0 ? 0.0 : -2.2250738585072014e-308;
                break;
            }
        }
        else
        {
            DoException = true;
        }
    }
    else if (softfloat_exceptionFlags != 0 && SetFPUException())
    {
        DoException = true;
    }
    if (DoException)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::CheckFPUInvalidException()
{
    if (softfloat_exceptionFlags == 0)
    {
        return false;
    }

    FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
    bool Res = false;
    if ((softfloat_exceptionFlags & softfloat_flag_invalid) != 0)
    {
        StatusReg.Cause.UnimplementedOperation = 1;
        Res = true;
    }
    else if ((softfloat_exceptionFlags & softfloat_flag_inexact) != 0)
    {
        StatusReg.Cause.Inexact = 1;
        if (StatusReg.Enable.Inexact)
        {
            Res = true;
        }
        else
        {
            StatusReg.Flags.Inexact = 1;
        }
    }

    if (Res)
    {
        m_Reg.TriggerException(EXC_FPE);
        return true;
    }
    return false;
}

bool R4300iInterpreter::InitFpuOperation(unsigned int RoundingModel)
{
    if (TestCop1UsableException())
    {
        return true;
    }
    m_FPCR[31] &= ~0x0003F000;
    switch (RoundingModel)
    {
    case FPRoundingMode_RoundToNearest: softfloat_roundingMode = softfloat_round_near_even; break;
    case FPRoundingMode_RoundTowardZero: softfloat_roundingMode = softfloat_round_minMag; break;
    case FPRoundingMode_RoundTowardPlusInfinity: softfloat_roundingMode = softfloat_round_max; break;
    case FPRoundingMode_RoundTowardMinusInfinity: softfloat_roundingMode = softfloat_round_min; break;
    }
    softfloat_exceptionFlags = 0;
    return false;
}

bool R4300iInterpreter::SetFPUException()
{
    FPStatusReg & StatusReg = (FPStatusReg &)m_FPCR[31];
    if ((softfloat_exceptionFlags & softfloat_flag_underflow) != 0)
    {
        if (StatusReg.FlushSubnormals == 0 || StatusReg.Enable.Underflow || StatusReg.Enable.Inexact)
        {
            StatusReg.Cause.UnimplementedOperation = 1;
            return true;
        }
    }

    bool Res = false;
    if ((softfloat_exceptionFlags & softfloat_flag_inexact) != 0)
    {
        StatusReg.Cause.Inexact = 1;
        if (StatusReg.Enable.Inexact) { Res = true; }
        else { StatusReg.Flags.Inexact = 1; }
    }
    if ((softfloat_exceptionFlags & softfloat_flag_underflow) != 0)
    {
        StatusReg.Cause.Underflow = 1;
        if (StatusReg.Enable.Underflow) { Res = true; }
        else { StatusReg.Flags.Underflow = 1; }
    }
    if ((softfloat_exceptionFlags & softfloat_flag_overflow) != 0)
    {
        StatusReg.Cause.Overflow = 1;
        if (StatusReg.Enable.Overflow) { Res = true; }
        else { StatusReg.Flags.Overflow = 1; }
    }
    if ((softfloat_exceptionFlags & softfloat_flag_infinite) != 0)
    {
        StatusReg.Cause.DivisionByZero = 1;
        if (StatusReg.Enable.DivisionByZero) { Res = true; }
        else { StatusReg.Flags.DivisionByZero = 1; }
    }
    if ((softfloat_exceptionFlags & softfloat_flag_invalid) != 0)
    {
        StatusReg.Cause.InvalidOperation = 1;
        if (StatusReg.Enable.InvalidOperation) { Res = true; }
        else { StatusReg.Flags.InvalidOperation = 1; }
    }
    return Res;
}

// ============================================================================
// Instruction Memory Management
// ============================================================================

void R4300iInterpreter::UpdateInstructionMemory()
{
    if (m_InstructionRegion != (m_PROGRAM_COUNTER & ~0xFFFULL))
    {
        m_InstructionRegion = m_PROGRAM_COUNTER & ~0xFFFULL;
        if ((uint64_t)((int32_t)m_PROGRAM_COUNTER) != m_PROGRAM_COUNTER)
        {
            uint32_t PAddr;
            bool MemoryUnused;
            if (!m_TLB.VAddrToPAddr(m_InstructionRegion, PAddr, MemoryUnused))
            {
                m_Reg.TriggerAddressException(m_PROGRAM_COUNTER, MemoryUnused ? EXC_RADE : EXC_RMISS);
                m_PROGRAM_COUNTER = m_System.m_JumpToLocation;
                m_System.m_PipelineStage = PIPELINE_STAGE_NORMAL;
                UpdateInstructionMemory();
                return;
            }
            if (PAddr >= m_MMU.RdramSize())
            {
                return;
            }
            m_InstructionMemory = &m_MMU.Rdram()[PAddr];
        }
        else
        {
            m_InstructionMemory = m_MMU.MemoryPtr((uint32_t)m_InstructionRegion, 4, true);
            if (m_InstructionMemory == nullptr)
            {
                m_Reg.TriggerAddressException((int32_t)m_PROGRAM_COUNTER, EXC_RMISS);
                m_PROGRAM_COUNTER = m_System.m_JumpToLocation;
                m_System.m_PipelineStage = PIPELINE_STAGE_NORMAL;
                UpdateInstructionMemory();
                return;
            }
        }
    }
    m_InstructionPtr = (uint32_t *)(((uint8_t *)m_InstructionMemory) + (m_PROGRAM_COUNTER & 0xFFFULL));
}
