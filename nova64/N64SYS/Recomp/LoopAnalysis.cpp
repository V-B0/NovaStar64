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

// --- Construction / Destruction ---

LoopAnalysis::LoopAnalysis(CodeBlock& codeBlock, CodeSection* /*entrySection*/) :
    m_CodeBlock(codeBlock), m_NextInstruction(0) {
    m_Command.Value = 0;
}

LoopAnalysis::~LoopAnalysis() {
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

// --- Section Register Set Accessors ---

// --- CheckLoopRegisterUsage - Main entry point ---

// --- CheckSectionRegisterUsage - Walk a section's instructions ---

// --- SetupEnterSection - Build entry register set from analysis ---

// --- SetupRegisterForLoop - Configure register states for loop optimization ---

// --- SyncRegState - Synchronize register states between two register sets ---

// --- SPECIAL Instruction Handlers ---

// SLL: rd = rt << sa (32-bit shift, sign-extended result)
void LoopAnalysis::SPECIAL_SLL(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    } // NOP

    if (Reg.IsConst(rt)) {
        int32_t result = (int32_t)(Reg.GetMipsRegLo(rt) << sa);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRL: rd = rt >> sa (32-bit logical shift right)
void LoopAnalysis::SPECIAL_SRL(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        uint32_t result = Reg.GetMipsRegLo(rt) >> sa;
        Reg.SetMipsRegLo(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRA: rd = rt >> sa (32-bit arithmetic shift right)
void LoopAnalysis::SPECIAL_SRA(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        int32_t result = Reg.GetMipsRegLo_S(rt) >> sa;
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SLLV: rd = rt << (rs & 0x1F)
void LoopAnalysis::SPECIAL_SLLV(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt) && Reg.IsConst(rs)) {
        int32_t result = (int32_t)(Reg.GetMipsRegLo(rt) << (Reg.GetMipsRegLo(rs) & 0x1F));
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRLV: rd = rt >> (rs & 0x1F) (logical)
void LoopAnalysis::SPECIAL_SRLV(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt) && Reg.IsConst(rs)) {
        uint32_t result = Reg.GetMipsRegLo(rt) >> (Reg.GetMipsRegLo(rs) & 0x1F);
        Reg.SetMipsRegLo(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SRAV: rd = rt >> (rs & 0x1F) (arithmetic)
void LoopAnalysis::SPECIAL_SRAV(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt) && Reg.IsConst(rs)) {
        int32_t result = Reg.GetMipsRegLo_S(rt) >> (Reg.GetMipsRegLo(rs) & 0x1F);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSLLV: rd = rt << (rs & 0x3F) (64-bit)
void LoopAnalysis::SPECIAL_DSLLV(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt) && Reg.IsConst(rs)) {
        uint64_t result = Reg.GetMipsReg(rt) << (Reg.GetMipsRegLo(rs) & 0x3F);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRLV: rd = rt >> (rs & 0x3F) (64-bit logical)
void LoopAnalysis::SPECIAL_DSRLV(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt) && Reg.IsConst(rs)) {
        uint64_t result = Reg.GetMipsReg(rt) >> (Reg.GetMipsRegLo(rs) & 0x3F);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRAV: rd = rt >> (rs & 0x3F) (64-bit arithmetic)
void LoopAnalysis::SPECIAL_DSRAV(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t rs = Cmd_rs(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt) && Reg.IsConst(rs)) {
        int64_t result = Reg.GetMipsReg_S(rt) >> (Reg.GetMipsRegLo(rs) & 0x3F);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// ADD: rd = rs + rt (32-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_ADD(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int32_t result = Reg.GetMipsRegLo_S(rs) + Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// ADDU: rd = rs + rt (32-bit, no overflow trap)
void LoopAnalysis::SPECIAL_ADDU(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int32_t result = Reg.GetMipsRegLo_S(rs) + Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SUB: rd = rs - rt (32-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_SUB(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int32_t result = Reg.GetMipsRegLo_S(rs) - Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SUBU: rd = rs - rt (32-bit, no overflow trap)
void LoopAnalysis::SPECIAL_SUBU(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int32_t result = Reg.GetMipsRegLo_S(rs) - Reg.GetMipsRegLo_S(rt);
        Reg.SetMipsRegLo(rd, (uint32_t)result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// AND: rd = rs & rt
void LoopAnalysis::SPECIAL_AND(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt)) {
            Reg.SetMipsReg(rd, Reg.GetMipsReg(rs) & Reg.GetMipsReg(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        } else {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) & Reg.GetMipsRegLo(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// OR: rd = rs | rt
void LoopAnalysis::SPECIAL_OR(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt)) {
            Reg.SetMipsReg(rd, Reg.GetMipsReg(rs) | Reg.GetMipsReg(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        } else {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) | Reg.GetMipsRegLo(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// XOR: rd = rs ^ rt
void LoopAnalysis::SPECIAL_XOR(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt)) {
            Reg.SetMipsReg(rd, Reg.GetMipsReg(rs) ^ Reg.GetMipsReg(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        } else {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) ^ Reg.GetMipsRegLo(rt));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// NOR: rd = ~(rs | rt)
void LoopAnalysis::SPECIAL_NOR(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt)) {
            Reg.SetMipsReg(rd, ~(Reg.GetMipsReg(rs) | Reg.GetMipsReg(rt)));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
        } else {
            Reg.SetMipsRegLo(rd, ~(Reg.GetMipsRegLo(rs) | Reg.GetMipsRegLo(rt)));
            Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
        }
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SLT: rd = (rs < rt) ? 1 : 0 (signed comparison)
void LoopAnalysis::SPECIAL_SLT(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt)) {
            Reg.SetMipsRegLo(rd, Reg.GetMipsReg_S(rs) < Reg.GetMipsReg_S(rt) ? 1 : 0);
        } else {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo_S(rs) < Reg.GetMipsRegLo_S(rt) ? 1 : 0);
        }
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// SLTU: rd = (rs < rt) ? 1 : 0 (unsigned comparison)
void LoopAnalysis::SPECIAL_SLTU(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        if (Reg.Is64Bit(rs) || Reg.Is64Bit(rt)) {
            Reg.SetMipsRegLo(rd, Reg.GetMipsReg(rs) < Reg.GetMipsReg(rt) ? 1 : 0);
        } else {
            Reg.SetMipsRegLo(rd, Reg.GetMipsRegLo(rs) < Reg.GetMipsRegLo(rt) ? 1 : 0);
        }
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_32_SIGN);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DADD: rd = rs + rt (64-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_DADD(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int64_t result = Reg.GetMipsReg_S(rs) + Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DADDU: rd = rs + rt (64-bit, no overflow trap)
void LoopAnalysis::SPECIAL_DADDU(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int64_t result = Reg.GetMipsReg_S(rs) + Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSUB: rd = rs - rt (64-bit, signed overflow trap)
void LoopAnalysis::SPECIAL_DSUB(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int64_t result = Reg.GetMipsReg_S(rs) - Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSUBU: rd = rs - rt (64-bit, no overflow trap)
void LoopAnalysis::SPECIAL_DSUBU(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rs = Cmd_rs(m_Command);
    uint32_t rt = Cmd_rt(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rs) && Reg.IsConst(rt)) {
        int64_t result = Reg.GetMipsReg_S(rs) - Reg.GetMipsReg_S(rt);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSLL: rd = rt << sa (64-bit)
void LoopAnalysis::SPECIAL_DSLL(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        uint64_t result = Reg.GetMipsReg(rt) << sa;
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRL: rd = rt >> sa (64-bit logical)
void LoopAnalysis::SPECIAL_DSRL(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        uint64_t result = Reg.GetMipsReg(rt) >> sa;
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRA: rd = rt >> sa (64-bit arithmetic)
void LoopAnalysis::SPECIAL_DSRA(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        int64_t result = Reg.GetMipsReg_S(rt) >> sa;
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSLL32: rd = rt << (sa + 32) (64-bit)
void LoopAnalysis::SPECIAL_DSLL32(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        uint64_t result = Reg.GetMipsReg(rt) << (sa + 32);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRL32: rd = rt >> (sa + 32) (64-bit logical)
void LoopAnalysis::SPECIAL_DSRL32(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        uint64_t result = Reg.GetMipsReg(rt) >> (sa + 32);
        Reg.SetMipsReg(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

// DSRA32: rd = rt >> (sa + 32) (64-bit arithmetic)
void LoopAnalysis::SPECIAL_DSRA32(CRegBase& Reg) {
    uint32_t rd = Cmd_rd(m_Command);
    uint32_t rt = Cmd_rt(m_Command);
    uint32_t sa = Cmd_sa(m_Command);

    if (rd == 0) {
        return;
    }

    if (Reg.IsConst(rt)) {
        int64_t result = Reg.GetMipsReg_S(rt) >> (sa + 32);
        Reg.SetMipsReg_S(rd, result);
        Reg.SetMipsRegState(rd, CRegBase::STATE_CONST_64);
    } else {
        Reg.SetMipsRegState(rd, CRegBase::STATE_MODIFIED);
    }
}

} // namespace Nova64::Recompiler
