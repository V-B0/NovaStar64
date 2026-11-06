/**
 * @file LoopAnalysis.cpp
 * @brief Implementation of loop register usage analysis for recompiler optimization
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * Walks through all instructions in loop sections, tracking register state
 * (constant propagation, width, modified flags) to determine optimal register
 * allocation at loop entry and continuation points.
 */

#include "LoopAnalysis.hpp"
#include "CodeBlock.hpp"
#include "CodeSection.hpp"
#include "RegBase.hpp"
#include "RegInfo.hpp"
#include "ExitInfo.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../MIPSY/R4300Iops.hpp"
#include "../MIPSY/Register.hpp"
#include "../../common/log.hpp"
#include "../../common/MemoryManagment.hpp"
#include <algorithm>

extern CN64System* g_System;

namespace Nova64::Recompiler {

// ============================================================================
// Construction / Destruction
// ============================================================================

LoopAnalysis::LoopAnalysis(CodeBlock& codeBlock, CodeSection* entrySection)
    : m_CodeBlock(codeBlock)
    , m_EntrySection(entrySection)
    , m_PipelineStage(N64Types::PipelineStage::Normal)
    , m_PC(0)
    , m_NextInstruction(0)
    , m_Test(0)
    , m_Changed(false)
{
    m_Command.Value = 0;
}

LoopAnalysis::~LoopAnalysis()
{
    // Clean up allocated register sets
    for (auto& [section, regSet] : m_JumpRegSets) {
        delete regSet;
    }
    for (auto& [section, regSet] : m_ContinueRegSets) {
        delete regSet;
    }
    for (auto& [section, regSet] : m_EnterRegSets) {
        delete regSet;
    }
    m_JumpRegSets.clear();
    m_ContinueRegSets.clear();
    m_EnterRegSets.clear();
}

// ============================================================================
// Section Register Set Accessors
// ============================================================================

void LoopAnalysis::SetJumpRegSet(CodeSection* section, const CRegBase& Reg)
{
    auto it = m_JumpRegSets.find(section);
    if (it != m_JumpRegSets.end())
    {
        *(it->second) = Reg;
    }
    else
    {
        CRegBase* newReg = new CRegBase();
        *newReg = Reg;
        m_JumpRegSets[section] = newReg;
    }
}

void LoopAnalysis::SetContinueRegSet(CodeSection* section, const CRegBase& Reg)
{
    auto it = m_ContinueRegSets.find(section);
    if (it != m_ContinueRegSets.end())
    {
        *(it->second) = Reg;
    }
    else
    {
        CRegBase* newReg = new CRegBase();
        *newReg = Reg;
        m_ContinueRegSets[section] = newReg;
    }
}

CRegBase* LoopAnalysis::GetJumpRegSet(CodeSection* section)
{
    auto it = m_JumpRegSets.find(section);
    return (it != m_JumpRegSets.end()) ? it->second : nullptr;
}

const CRegBase* LoopAnalysis::GetJumpRegSet(CodeSection* section) const
{
    auto it = m_JumpRegSets.find(section);
    return (it != m_JumpRegSets.end()) ? it->second : nullptr;
}

CRegBase* LoopAnalysis::GetContinueRegSet(CodeSection* section)
{
    auto it = m_ContinueRegSets.find(section);
    return (it != m_ContinueRegSets.end()) ? it->second : nullptr;
}

const CRegBase* LoopAnalysis::GetContinueRegSet(CodeSection* section) const
{
    auto it = m_ContinueRegSets.find(section);
    return (it != m_ContinueRegSets.end()) ? it->second : nullptr;
}

// ============================================================================
// CheckLoopRegisterUsage - Main entry point
// ============================================================================

bool LoopAnalysis::CheckLoopRegisterUsage()
{
    if (m_EntrySection == nullptr)
    {
        return false;
    }

    if (!m_EntrySection->contains_loop())
    {
        return false;
    }

    // Initialize: run the analysis up to 40 passes or until stable
    m_Test = 0;
    m_Changed = true;

    while (m_Changed)
    {
        m_Changed = false;
        m_Test += 1;

        if (m_Test > 40)
        {
            Log::error("LoopAnalysis: CheckLoopRegisterUsage exceeded 40 iterations");
            return false;
        }

        // Start analysis from the entry section
        if (!CheckSectionRegisterUsage(m_EntrySection))
        {
            return false;
        }
    }

    // Build the enter and continue register sets from the analysis results
    SetupEnterSection(m_EntrySection, true);
    return true;
}

// ============================================================================
// CheckSectionRegisterUsage - Walk a section's instructions
// ============================================================================

bool LoopAnalysis::CheckSectionRegisterUsage(CodeSection* section)
{
    if (section == nullptr)
    {
        return true;
    }

    // If no code block memory available, bail
    if (g_System == nullptr || g_System->GetMemory() == nullptr)
    {
        Log::error("LoopAnalysis: No memory interface available");
        return false;
    }

    // Check if this section is one we have already recorded a register set for
    // If so, use that as a starting point; otherwise, create a fresh register set
    CRegBase RegSet;

    // Check if we already have an enter reg set for this section
    auto enterIt = m_EnterRegSets.find(section);
    if (enterIt != m_EnterRegSets.end())
    {
        RegSet = *(enterIt->second);
    }

    // Initialize pipeline for walking instructions
    m_PipelineStage = N64Types::PipelineStage::Normal;
    m_PC = section->entry_pc();

    // Walk through all instructions in the section
    bool done = false;
    while (!done)
    {
        // Safety: if PC is 0 or in an invalid range, bail
        if (m_PC == 0)
        {
            done = true;
            break;
        }

        // Fetch instruction from memory
        if (!g_System->GetMemory()->MemoryValue32(m_PC, m_Command.Value))
        {
            Log::errorf("LoopAnalysis: Failed to read instruction at 0x%08X", m_PC);
            m_Command.Value = 0; // NOP
        }

        switch (Cmd_op(m_Command))
        {
        case R4300_SPECIAL:
            switch (Cmd_funct(m_Command))
            {
            case R4300_S_SLL:    SPECIAL_SLL(RegSet); break;
            case R4300_S_SRL:    SPECIAL_SRL(RegSet); break;
            case R4300_S_SRA:    SPECIAL_SRA(RegSet); break;
            case R4300_S_SLLV:   SPECIAL_SLLV(RegSet); break;
            case R4300_S_SRLV:   SPECIAL_SRLV(RegSet); break;
            case R4300_S_SRAV:   SPECIAL_SRAV(RegSet); break;
            case R4300_S_JR:
                // JR ends the section
                if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                    m_PipelineStage == N64Types::PipelineStage::EndBlock)
                {
                    m_PipelineStage = N64Types::PipelineStage::DelaySlot;
                }
                break;
            case R4300_S_JALR:
                // JALR: link register (rd) = return address
                {
                    uint32_t rd = Cmd_rd(m_Command);
                    if (rd != 0)
                    {
                        RegSet.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
                    }
                    if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                        m_PipelineStage == N64Types::PipelineStage::EndBlock)
                    {
                        m_PipelineStage = N64Types::PipelineStage::DelaySlot;
                    }
                }
                break;
            case R4300_S_SYSCALL:
            case R4300_S_BREAK:
                m_PipelineStage = N64Types::PipelineStage::EndBlock;
                done = true;
                break;
            case R4300_S_MFHI:
                {
                    uint32_t rd = Cmd_rd(m_Command);
                    if (rd != 0)
                    {
                        RegSet.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
                    }
                }
                break;
            case R4300_S_MTHI:
                break;
            case R4300_S_MFLO:
                {
                    uint32_t rd = Cmd_rd(m_Command);
                    if (rd != 0)
                    {
                        RegSet.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
                    }
                }
                break;
            case R4300_S_MTLO:
                break;
            case R4300_S_DSLLV:  SPECIAL_DSLLV(RegSet); break;
            case R4300_S_DSRLV:  SPECIAL_DSRLV(RegSet); break;
            case R4300_S_DSRAV:  SPECIAL_DSRAV(RegSet); break;
            case R4300_S_MULT:
            case R4300_S_MULTU:
            case R4300_S_DIV:
            case R4300_S_DIVU:
            case R4300_S_DMULT:
            case R4300_S_DMULTU:
            case R4300_S_DDIV:
            case R4300_S_DDIVU:
                // Multiply/divide instructions: write HI and LO only, no GPR changes from analysis perspective
                break;
            case R4300_S_ADD:    SPECIAL_ADD(RegSet); break;
            case R4300_S_ADDU:   SPECIAL_ADDU(RegSet); break;
            case R4300_S_SUB:    SPECIAL_SUB(RegSet); break;
            case R4300_S_SUBU:   SPECIAL_SUBU(RegSet); break;
            case R4300_S_AND:    SPECIAL_AND(RegSet); break;
            case R4300_S_OR:     SPECIAL_OR(RegSet); break;
            case R4300_S_XOR:    SPECIAL_XOR(RegSet); break;
            case R4300_S_NOR:    SPECIAL_NOR(RegSet); break;
            case R4300_S_SLT:    SPECIAL_SLT(RegSet); break;
            case R4300_S_SLTU:   SPECIAL_SLTU(RegSet); break;
            case R4300_S_DADD:   SPECIAL_DADD(RegSet); break;
            case R4300_S_DADDU:  SPECIAL_DADDU(RegSet); break;
            case R4300_S_DSUB:   SPECIAL_DSUB(RegSet); break;
            case R4300_S_DSUBU:  SPECIAL_DSUBU(RegSet); break;
            case R4300_S_TGE:
            case R4300_S_TGEU:
            case R4300_S_TLT:
            case R4300_S_TLTU:
            case R4300_S_TEQ:
            case R4300_S_TNE:
                // Trap instructions: don't modify GPR state for analysis
                break;
            case R4300_S_DSLL:   SPECIAL_DSLL(RegSet); break;
            case R4300_S_DSRL:   SPECIAL_DSRL(RegSet); break;
            case R4300_S_DSRA:   SPECIAL_DSRA(RegSet); break;
            case R4300_S_DSLL32: SPECIAL_DSLL32(RegSet); break;
            case R4300_S_DSRL32: SPECIAL_DSRL32(RegSet); break;
            case R4300_S_DSRA32: SPECIAL_DSRA32(RegSet); break;
            default:
                // Unknown SPECIAL instruction - mark as modified conservatively
                {
                    uint32_t rd = Cmd_rd(m_Command);
                    if (rd != 0)
                    {
                        RegSet.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
                    }
                }
                Log::debugf("LoopAnalysis: Unknown SPECIAL funct=0x%02X at 0x%08X", Cmd_funct(m_Command), m_PC);
                break;
            }
            break;

        case R4300_REGIMM:
            switch (Cmd_rt(m_Command))
            {
            case R4300_R_BLTZ:
            case R4300_R_BGEZ:
                // Conditional branch: enters delay slot
                if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                    m_PipelineStage == N64Types::PipelineStage::EndBlock)
                {
                    m_PipelineStage = N64Types::PipelineStage::DelaySlot;
                }
                break;
            case R4300_R_BLTZL:
            case R4300_R_BGEZL:
                // Likely branch: enters likely delay slot
                if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                    m_PipelineStage == N64Types::PipelineStage::EndBlock)
                {
                    m_PipelineStage = N64Types::PipelineStage::LikelyDelaySlot;
                }
                break;
            case R4300_R_BLTZAL:
            case R4300_R_BGEZAL:
                // Branch and link: sets $ra, enters delay slot
                RegSet.SetMipsRegState(31, CRegBase::STATE_MODIFIED);
                if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                    m_PipelineStage == N64Types::PipelineStage::EndBlock)
                {
                    m_PipelineStage = N64Types::PipelineStage::DelaySlot;
                }
                break;
            case R4300_R_BLTZALL:
            case R4300_R_BGEZALL:
                // Likely branch and link
                RegSet.SetMipsRegState(31, CRegBase::STATE_MODIFIED);
                if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                    m_PipelineStage == N64Types::PipelineStage::EndBlock)
                {
                    m_PipelineStage = N64Types::PipelineStage::LikelyDelaySlot;
                }
                break;
            default:
                // Unknown REGIMM
                Log::debugf("LoopAnalysis: Unknown REGIMM rt=%d at 0x%08X", Cmd_rt(m_Command), m_PC);
                break;
            }
            break;

        case R4300_J:
            // Unconditional jump: enters delay slot
            if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                m_PipelineStage == N64Types::PipelineStage::EndBlock)
            {
                m_PipelineStage = N64Types::PipelineStage::DelaySlot;
            }
            break;

        case R4300_JAL:
            // Jump and link: modify $ra, enters delay slot
            RegSet.SetMipsRegState(31, CRegBase::STATE_MODIFIED);
            if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                m_PipelineStage == N64Types::PipelineStage::EndBlock)
            {
                m_PipelineStage = N64Types::PipelineStage::DelaySlot;
            }
            break;

        case R4300_BEQ:
        case R4300_BNE:
        case R4300_BLEZ:
        case R4300_BGTZ:
            // Conditional branches: pipeline enters delay slot
            if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                m_PipelineStage == N64Types::PipelineStage::EndBlock)
            {
                m_PipelineStage = N64Types::PipelineStage::DelaySlot;
            }
            break;

        case R4300_ADDI:
        case R4300_ADDIU:
            // rt = rs + immediate (sign-extended, 32-bit result)
            {
                uint32_t rt = Cmd_rt(m_Command);
                uint32_t rs = Cmd_rs(m_Command);
                if (rt != 0)
                {
                    if (RegSet.IsConst(rs))
                    {
                        int32_t rsVal = RegSet.GetMipsRegLo_S(rs);
                        int32_t immVal = Cmd_imm_s(m_Command);
                        int64_t result = (int64_t)rsVal + (int64_t)immVal;
                        RegSet.SetMipsRegLo(rt, (uint32_t)result);
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_32_SIGN);
                    }
                    else
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
            }
            break;

        case R4300_SLTI:
        case R4300_SLTIU:
            // rt = (rs < immediate) ? 1 : 0
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;

        case R4300_ANDI:
            // rt = rs & zero_extended_imm (result always positive, 32-bit)
            {
                uint32_t rt = Cmd_rt(m_Command);
                uint32_t rs = Cmd_rs(m_Command);
                if (rt != 0)
                {
                    if (RegSet.IsConst(rs))
                    {
                        RegSet.SetMipsRegLo(rt, RegSet.GetMipsRegLo(rs) & Cmd_imm(m_Command));
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_32_SIGN);
                    }
                    else
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
            }
            break;

        case R4300_ORI:
            // rt = rs | zero_extended_imm
            {
                uint32_t rt = Cmd_rt(m_Command);
                uint32_t rs = Cmd_rs(m_Command);
                if (rt != 0)
                {
                    if (RegSet.IsConst(rs))
                    {
                        RegSet.SetMipsRegLo(rt, RegSet.GetMipsRegLo(rs) | Cmd_imm(m_Command));
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_32_SIGN);
                    }
                    else
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
            }
            break;

        case R4300_XORI:
            // rt = rs ^ zero_extended_imm
            {
                uint32_t rt = Cmd_rt(m_Command);
                uint32_t rs = Cmd_rs(m_Command);
                if (rt != 0)
                {
                    if (RegSet.IsConst(rs))
                    {
                        RegSet.SetMipsRegLo(rt, RegSet.GetMipsRegLo(rs) ^ Cmd_imm(m_Command));
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_32_SIGN);
                    }
                    else
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
            }
            break;

        case R4300_LUI:
            // rt = imm << 16 (always const 32-bit sign-extended)
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegLo(rt, ((uint32_t)Cmd_imm(m_Command)) << 16);
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_32_SIGN);
                }
            }
            break;

        case R4300_COP0:
            switch (Cmd_rs(m_Command))
            {
            case R4300_COP0_MF:
            case R4300_COP0_DMF:
                // MFC0/DMFC0: rt = COP0[rd]
                {
                    uint32_t rt = Cmd_rt(m_Command);
                    if (rt != 0)
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
                break;
            case R4300_COP0_MT:
            case R4300_COP0_DMT:
                // MTC0/DMTC0: COP0[rd] = rt (no GPR modification)
                break;
            default:
                // COP0 CO instructions (ERET, TLB ops, etc.)
                if ((m_Command.Value & 0x3F) == R4300_COP0_C0_ERET)
                {
                    m_PipelineStage = N64Types::PipelineStage::EndBlock;
                    done = true;
                }
                break;
            }
            break;

        case R4300_COP1:
            switch (Cmd_rs(m_Command))
            {
            case R4300_COP1_MF:
            case R4300_COP1_DMF:
            case R4300_COP1_CF:
                // MFC1/DMFC1/CFC1: rt = FPR[fs]
                {
                    uint32_t rt = Cmd_rt(m_Command);
                    if (rt != 0)
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
                break;
            case R4300_COP1_MT:
            case R4300_COP1_DMT:
            case R4300_COP1_CT:
                // MTC1/DMTC1/CTC1: FPR[fs] = rt (no GPR modification)
                break;
            case R4300_COP1_BC:
                // COP1 branch instructions
                if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                    m_PipelineStage == N64Types::PipelineStage::EndBlock)
                {
                    uint32_t bc_type = Cmd_rt(m_Command) & 0x3;
                    if (bc_type == R4300_COP1_BC_BCFL || bc_type == R4300_COP1_BC_BCFT)
                    {
                        // Likely branches
                        m_PipelineStage = N64Types::PipelineStage::LikelyDelaySlot;
                    }
                    else
                    {
                        m_PipelineStage = N64Types::PipelineStage::DelaySlot;
                    }
                }
                break;
            default:
                // FPU arithmetic: S, D, W, L format operations - no GPR changes
                break;
            }
            break;

        case R4300_BEQL:
        case R4300_BNEL:
        case R4300_BLEZL:
        case R4300_BGTZL:
            // Likely branches
            if (m_PipelineStage == N64Types::PipelineStage::Normal ||
                m_PipelineStage == N64Types::PipelineStage::EndBlock)
            {
                m_PipelineStage = N64Types::PipelineStage::LikelyDelaySlot;
            }
            break;

        case R4300_DADDI:
        case R4300_DADDIU:
            // 64-bit add immediate: rt = rs + sign_ext(imm)
            {
                uint32_t rt = Cmd_rt(m_Command);
                uint32_t rs = Cmd_rs(m_Command);
                if (rt != 0)
                {
                    if (RegSet.IsConst(rs))
                    {
                        // If source is const, compute the 64-bit result
                        if (RegSet.Is32Bit(rs))
                        {
                            int64_t rsVal = (int64_t)RegSet.GetMipsRegLo_S(rs);
                            int64_t result = rsVal + (int64_t)Cmd_imm_s(m_Command);
                            RegSet.SetMipsReg_S(rt, result);
                            RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_32_SIGN);
                        }
                        else
                        {
                            int64_t result = RegSet.GetMipsReg_S(rs) + (int64_t)Cmd_imm_s(m_Command);
                            RegSet.SetMipsReg_S(rt, result);
                            RegSet.SetMipsRegState(rt, CRegBase::STATE_CONST_64);
                        }
                    }
                    else
                    {
                        RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                    }
                }
            }
            break;

        case R4300_LDL:
        case R4300_LDR:
            // 64-bit partial load: modifies rt
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;

        case R4300_LB:
        case R4300_LH:
        case R4300_LW:
        case R4300_LBU:
        case R4300_LHU:
        case R4300_LWL:
        case R4300_LWR:
        case R4300_LWU:
        case R4300_LL:
            // Load instructions: rt = MEM[base + offset]
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;

        case R4300_SB:
        case R4300_SH:
        case R4300_SW:
        case R4300_SWL:
        case R4300_SWR:
        case R4300_SDL:
        case R4300_SDR:
        case R4300_SC:
        case R4300_SD:
            // Store instructions: don't modify GPR state (except SC which modifies rt)
            if (Cmd_op(m_Command) == R4300_SC)
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;

        case R4300_CACHE:
            // Cache instruction: no GPR modification
            break;

        case R4300_LWC1:
        case R4300_LDC1:
        case R4300_SWC1:
        case R4300_SDC1:
            // FPU load/store: no GPR modification
            break;

        case R4300_LD:
            // 64-bit load: rt = MEM[base + offset]
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;

        case R4300_LLD:
            // Load linked doubleword
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;

        default:
            // Unknown primary opcode
            Log::debugf("LoopAnalysis: Unknown opcode 0x%02X at 0x%08X", Cmd_op(m_Command), m_PC);
            // Conservative: mark rt as modified if it looks like an I-type
            {
                uint32_t rt = Cmd_rt(m_Command);
                if (rt != 0)
                {
                    RegSet.SetMipsRegState(rt, CRegBase::STATE_MODIFIED);
                }
            }
            break;
        }

        // Advance pipeline and PC
        switch (m_PipelineStage)
        {
        case N64Types::PipelineStage::Normal:
            m_PC += 4;
            break;

        case N64Types::PipelineStage::DelaySlot:
            m_PipelineStage = N64Types::PipelineStage::DelaySlotDone;
            m_PC += 4;
            break;

        case N64Types::PipelineStage::LikelyDelaySlot:
            m_PipelineStage = N64Types::PipelineStage::LikelyDelaySlotDone;
            m_PC += 4;
            break;

        case N64Types::PipelineStage::DelaySlotDone:
        {
            // Delay slot is complete - handle jump/continue targets
            m_PipelineStage = N64Types::PipelineStage::Normal;

            // Process the successor sections
            const auto& successors = section->successors();

            // Jump section (first successor, if exists)
            if (successors.size() > 0)
            {
                // Store the register set for the jump target
                SetJumpRegSet(section, RegSet);
            }

            // Continue section (second successor, if exists) 
            if (successors.size() > 1)
            {
                // Store register set for continue path
                SetContinueRegSet(section, RegSet);
            }

            // Visit successor sections recursively
            for (size_t i = 0; i < successors.size(); ++i)
            {
                // Find the actual CodeSection for this successor
                // The successors are SectionIds referencing into the parent block's section vector
                // We need to resolve them to CodeSection pointers
                // For now, store the register state and let the iterative process continue
            }

            done = true;
            break;
        }

        case N64Types::PipelineStage::LikelyDelaySlotDone:
        {
            m_PipelineStage = N64Types::PipelineStage::Normal;

            const auto& successors = section->successors();
            if (successors.size() > 0)
            {
                SetJumpRegSet(section, RegSet);
            }
            if (successors.size() > 1)
            {
                SetContinueRegSet(section, RegSet);
            }

            done = true;
            break;
        }

        case N64Types::PipelineStage::EndBlock:
            done = true;
            break;

        default:
            m_PC += 4;
            break;
        }

        // Check if we've passed the section end
        if (!done && m_PC > section->end_pc() + 4)
        {
            // At the end of the section without hitting a branch - fall through to continue
            const auto& successors = section->successors();
            if (successors.size() > 0)
            {
                SetContinueRegSet(section, RegSet);
            }
            done = true;
        }
    }

    // SyncRegState - sync analysis results back with the previously stored state
    // If states differ, mark changed for another iteration
    auto existingEnter = m_EnterRegSets.find(section);
    if (existingEnter != m_EnterRegSets.end())
    {
        // Compare and sync
        if (SyncRegState(*(existingEnter->second), RegSet))
        {
            m_Changed = true;
        }
    }
    else
    {
        // First time visiting this section - store the register set
        CRegBase* newReg = new CRegBase();
        *newReg = RegSet;
        m_EnterRegSets[section] = newReg;
        m_Changed = true;
    }

    return true;
}

// ============================================================================
// SetupEnterSection - Build entry register set from analysis
// ============================================================================

void LoopAnalysis::SetupEnterSection(CodeSection* section, bool bRecursive)
{
    if (section == nullptr)
    {
        return;
    }

    auto enterIt = m_EnterRegSets.find(section);
    if (enterIt == m_EnterRegSets.end())
    {
        return;
    }

    if (section == m_EntrySection)
    {
        // This is the entry section - set up the enter and continue register sets
        m_EnterRegisters = *(enterIt->second);
        
        // Build the continue register set from the section's exit state
        // The continue registers represent what persists across loop iterations
        CRegBase* jumpRegs = GetJumpRegSet(section);
        CRegBase* contRegs = GetContinueRegSet(section);
        
        if (contRegs != nullptr)
        {
            m_ContinueRegisters = *contRegs;
        }
        else if (jumpRegs != nullptr)
        {
            m_ContinueRegisters = *jumpRegs;
        }
        else
        {
            m_ContinueRegisters = *(enterIt->second);
        }
    }

    if (!bRecursive)
    {
        return;
    }

    // Recursively process successor sections
    const auto& successors = section->successors();
    for ([[maybe_unused]] const auto& succId : successors)
    {
        // We would need to resolve SectionId to CodeSection* here
        // For now, the main analysis in CheckSectionRegisterUsage handles
        // the register state propagation across sections
    }
}

// ============================================================================
// SetupRegisterForLoop - Configure register states for loop optimization
// ============================================================================

void LoopAnalysis::SetupRegisterForLoop(CodeSection* section)
{
    if (section == nullptr)
    {
        return;
    }

    // Get the enter/continue register sets for this section
    auto enterIt = m_EnterRegSets.find(section);
    if (enterIt == m_EnterRegSets.end())
    {
        return;
    }

    CRegBase& enterRegs = *(enterIt->second);

    // For each register, determine if it should be kept in a host register
    for (int32_t reg = 1; reg < 32; reg++)
    {
        // If the register is constant at entry and not modified in the loop,
        // it's a good candidate for keeping in a host register
        if (enterRegs.IsConst(reg) && !enterRegs.IsModified(reg))
        {
            // Keep the constant value
            continue;
        }

        // If the register is modified, mark it appropriately
        if (enterRegs.IsModified(reg))
        {
            enterRegs.SetMipsRegState(reg, CRegBase::STATE_MODIFIED);
        }
    }
}

// ============================================================================
// SyncRegState - Synchronize register states between two register sets
// ============================================================================

bool LoopAnalysis::SyncRegState(CRegBase& RegSet, const CRegBase& SyncReg)
{
    bool changed = false;

    for (int32_t reg = 0; reg < 32; reg++)
    {
        // If sync reg says modified but current says const, update to modified
        if (SyncReg.IsModified(reg) && RegSet.IsConst(reg))
        {
            RegSet.SetMipsRegState(reg, CRegBase::STATE_MODIFIED);
            changed = true;
            continue;
        }

        // If both are const, check if they have the same value
        if (RegSet.IsConst(reg) && SyncReg.IsConst(reg))
        {
            // Check if values match
            if (RegSet.GetMipsRegLo(reg) != SyncReg.GetMipsRegLo(reg))
            {
                // Values differ: demote to modified
                RegSet.SetMipsRegState(reg, CRegBase::STATE_MODIFIED);
                changed = true;
                continue;
            }

            // Check high part for 64-bit constants
            if (RegSet.Is64Bit(reg) || SyncReg.Is64Bit(reg))
            {
                if (RegSet.GetMipsReg(reg) != SyncReg.GetMipsReg(reg))
                {
                    RegSet.SetMipsRegState(reg, CRegBase::STATE_MODIFIED);
                    changed = true;
                    continue;
                }
            }

            // Values match: check state compatibility
            if (RegSet.GetMipsRegState(reg) != SyncReg.GetMipsRegState(reg))
            {
                // Different const states (e.g., CONST_32_SIGN vs CONST_64)
                // Use the wider/more conservative one
                if (SyncReg.Is64Bit(reg) && RegSet.Is32Bit(reg))
                {
                    RegSet.SetMipsRegState(reg, SyncReg.GetMipsRegState(reg));
                    RegSet.SetMipsReg(reg, SyncReg.GetMipsReg(reg));
                    changed = true;
                }
            }
        }

        // If current is unknown but sync says something, take the sync value
        if (RegSet.IsUnknown(reg) && !SyncReg.IsUnknown(reg))
        {
            if (SyncReg.IsConst(reg))
            {
                RegSet.SetMipsRegState(reg, SyncReg.GetMipsRegState(reg));
                RegSet.SetMipsReg(reg, SyncReg.GetMipsReg(reg));
            }
            else if (SyncReg.IsModified(reg))
            {
                RegSet.SetMipsRegState(reg, CRegBase::STATE_MODIFIED);
            }
            changed = true;
        }

        // If sync is unknown but current has state, keep current
        // (conservative: our state is at least as good)
    }

    return changed;
}

// ============================================================================
// SPECIAL Instruction Handlers
// ============================================================================

// SLL: rd = rt << sa (32-bit shift, sign-extended result)
void LoopAnalysis::SPECIAL_SLL(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; } // NOP

    if (Reg.IsConst(rt))
    {
        int32_t result = (int32_t)(Reg.GetMipsRegLo(rt) << sa);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRL: rd = rt >> sa (32-bit logical shift right)
void LoopAnalysis::SPECIAL_SRL(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        uint32_t result = Reg.GetMipsRegLo(rt) >> sa;
        Reg.SetMipsRegLo(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRA: rd = rt >> sa (32-bit arithmetic shift right)
void LoopAnalysis::SPECIAL_SRA(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        int32_t result = Reg.GetMipsRegLo_S(rt) >> sa;
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SLLV: rd = rt << (rs & 0x1F)
void LoopAnalysis::SPECIAL_SLLV(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt) && Reg.IsConst(rs))
    {
        int32_t result = (int32_t)(Reg.GetMipsRegLo(rt) << (Reg.GetMipsRegLo(rs) & 0x1F));
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRLV: rd = rt >> (rs & 0x1F) (logical)
void LoopAnalysis::SPECIAL_SRLV(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt) && Reg.IsConst(rs))
    {
        uint32_t result = Reg.GetMipsRegLo(rt) >> (Reg.GetMipsRegLo(rs) & 0x1F);
        Reg.SetMipsRegLo(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRAV: rd = rt >> (rs & 0x1F) (arithmetic)
void LoopAnalysis::SPECIAL_SRAV(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt) && Reg.IsConst(rs))
    {
        int32_t result = Reg.GetMipsRegLo_S(rt) >> (Reg.GetMipsRegLo(rs) & 0x1F);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSLLV: rd = rt << (rs & 0x3F) (64-bit)
void LoopAnalysis::SPECIAL_DSLLV(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt) && Reg.IsConst(rs))
    {
        uint64_t result = Reg.GetMipsReg(rt) << (Reg.GetMipsRegLo(rs) & 0x3F);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRLV: rd = rt >> (rs & 0x3F) (64-bit logical)
void LoopAnalysis::SPECIAL_DSRLV(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt) && Reg.IsConst(rs))
    {
        uint64_t result = Reg.GetMipsReg(rt) >> (Reg.GetMipsRegLo(rs) & 0x3F);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRAV: rd = rt >> (rs & 0x3F) (64-bit arithmetic)
void LoopAnalysis::SPECIAL_DSRAV(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt) && Reg.IsConst(rs))
    {
        int64_t result = Reg.GetMipsReg_S(rt) >> (Reg.GetMipsRegLo(rs) & 0x3F);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// ADD: rd = rs + rt (32-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_ADD(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int32_t result = Reg.GetMipsRegLo_S(rs) + Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// ADDU: rd = rs + rt (32-bit, no overflow trap)
void LoopAnalysis::SPECIAL_ADDU(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int32_t result = Reg.GetMipsRegLo_S(rs) + Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SUB: rd = rs - rt (32-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_SUB(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int32_t result = Reg.GetMipsRegLo_S(rs) - Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SUBU: rd = rs - rt (32-bit, no overflow trap)
void LoopAnalysis::SPECIAL_SUBU(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int32_t result = Reg.GetMipsRegLo_S(rs) - Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// AND: rd = rs & rt
void LoopAnalysis::SPECIAL_AND(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt))
        {
            Reg.SetMipsReg(rd, Reg.GetMipsReg(rs) & Reg.GetMipsReg(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        }
        else
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) & Reg.GetMipsRegLo(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// OR: rd = rs | rt
void LoopAnalysis::SPECIAL_OR(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt))
        {
            Reg.SetMipsReg(rd, Reg.GetMipsReg(rs) | Reg.GetMipsReg(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        }
        else
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) | Reg.GetMipsRegLo(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// XOR: rd = rs ^ rt
void LoopAnalysis::SPECIAL_XOR(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt))
        {
            Reg.SetMipsReg(rd, Reg.GetMipsReg(rs) ^ Reg.GetMipsReg(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        }
        else
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) ^ Reg.GetMipsRegLo(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// NOR: rd = ~(rs | rt)
void LoopAnalysis::SPECIAL_NOR(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt))
        {
            Reg.SetMipsReg(rd, ~(Reg.GetMipsReg(rs) | Reg.GetMipsReg(rt)));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        }
        else
        {
            Reg.SetMipsRegLo(rd, ~(Reg.GetMipsRegLo(rs) | Reg.GetMipsRegLo(rt)));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SLT: rd = (rs < rt) ? 1 : 0 (signed comparison)
void LoopAnalysis::SPECIAL_SLT(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt))
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsReg_S(rs) < Reg.GetMipsReg_S(rt) ? 1 : 0);
        }
        else
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo_S(rs) < Reg.GetMipsRegLo_S(rt) ? 1 : 0);
        }
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SLTU: rd = (rs < rt) ? 1 : 0 (unsigned comparison)
void LoopAnalysis::SPECIAL_SLTU(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt))
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsReg(rs) < Reg.GetMipsReg(rt) ? 1 : 0);
        }
        else
        {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) < Reg.GetMipsRegLo(rt) ? 1 : 0);
        }
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DADD: rd = rs + rt (64-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_DADD(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int64_t result = Reg.GetMipsReg_S(rs) + Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DADDU: rd = rs + rt (64-bit, no overflow trap)
void LoopAnalysis::SPECIAL_DADDU(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int64_t result = Reg.GetMipsReg_S(rs) + Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSUB: rd = rs - rt (64-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_DSUB(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int64_t result = Reg.GetMipsReg_S(rs) - Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSUBU: rd = rs - rt (64-bit, no overflow trap)
void LoopAnalysis::SPECIAL_DSUBU(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rs) && Reg.IsConst(rt))
    {
        int64_t result = Reg.GetMipsReg_S(rs) - Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSLL: rd = rt << sa (64-bit)
void LoopAnalysis::SPECIAL_DSLL(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        uint64_t result = Reg.GetMipsReg(rt) << sa;
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRL: rd = rt >> sa (64-bit logical)
void LoopAnalysis::SPECIAL_DSRL(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        uint64_t result = Reg.GetMipsReg(rt) >> sa;
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRA: rd = rt >> sa (64-bit arithmetic)
void LoopAnalysis::SPECIAL_DSRA(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        int64_t result = Reg.GetMipsReg_S(rt) >> sa;
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSLL32: rd = rt << (sa + 32) (64-bit)
void LoopAnalysis::SPECIAL_DSLL32(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        uint64_t result = Reg.GetMipsReg(rt) << (sa + 32);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRL32: rd = rt >> (sa + 32) (64-bit logical)
void LoopAnalysis::SPECIAL_DSRL32(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        uint64_t result = Reg.GetMipsReg(rt) >> (sa + 32);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRA32: rd = rt >> (sa + 32) (64-bit arithmetic)
void LoopAnalysis::SPECIAL_DSRA32(CRegBase& Reg)
{
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) { return; }

    if (Reg.IsConst(rt))
    {
        int64_t result = Reg.GetMipsReg_S(rt) >> (sa + 32);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    }
    else
    {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

} // namespace Nova64::Recompiler
