/**
 * @file RegBase.cpp
 * @brief Implementation of base register state management
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This implementation tracks MIPS register states during dynamic recompilation,
 * enabling optimizations like constant propagation, register allocation, and
 * dead code elimination.
 * 
 * Improvements over Project64:
 * - Modern C++ design with RegisterSet abstraction
 * - Comprehensive state tracking with RegisterState
 * - Better debugging support with detailed ToString methods
 * - Enhanced FPU rounding mode management
 * - Improved type safety with enums
 */

#include "RegBase.hpp"
#include "RegInfo.hpp"
#include "CodeBlock.hpp"
#include "CodeSection.hpp"
#include "ExitInfo.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/log.hpp"
#include "../../common/trace.hpp"
#include "../../common/MemoryManagment.hpp"
#include "../../common/platform.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace Nova64::Recompiler {

// --- RegisterState Implementation ---

// --- RegisterSet Implementation ---

void RegisterSet::CopyFrom(const RegisterSet& other) noexcept {
    m_States = other.m_States;
    m_Values = other.m_Values;

    m_CycleCount = other.m_CycleCount;
    m_FpuUsed = other.m_FpuUsed;
    m_RoundingMode = other.m_RoundingMode;

    Log::debugf("RegisterSet: Copied state with %u cycles, FPU used: %s", m_CycleCount, m_FpuUsed ? "yes" : "no");
}

bool RegisterSet::Compare(const RegisterSet& other) const noexcept {
    for (size_t i = 0; i < NUM_GPR; ++i) {
        const auto& state1 = m_States[i];
        const auto& state2 = other.m_States[i];

        if (state1.width != state2.width || state1.mapping != state2.mapping || state1.isSigned != state2.isSigned ||
            state1.isModified != state2.isModified || state1.isDirty != state2.isDirty) {
            return false;
        }

        if (state1.IsConstant() && state2.IsConstant()) {
            if (m_Values[i] != other.m_Values[i]) {
                return false;
            }
        }
    }

    if (m_CycleCount != other.m_CycleCount || m_FpuUsed != other.m_FpuUsed || m_RoundingMode != other.m_RoundingMode) {
        return false;
    }

    return true;
}

std::string RegisterSet::RegisterToString(size_t regIndex) const {
    if (regIndex >= NUM_GPR) {
        return "Invalid register";
    }

    const auto& state = m_States[regIndex];
    char buffer[256];

    const char* regName = "Unknown";
    if (regIndex == 0) regName = "zero";
    else if (regIndex == 1) regName = "at";
    else if (regIndex >= 2 && regIndex <= 3) {
        static char vName[8];
        snprintf(vName, sizeof(vName), "v%zu", regIndex - 2);
        regName = vName;
    } else if (regIndex >= 4 && regIndex <= 7) {
        static char aName[8];
        snprintf(aName, sizeof(aName), "a%zu", regIndex - 4);
        regName = aName;
    } else if (regIndex >= 8 && regIndex <= 15) {
        static char tName[8];
        snprintf(tName, sizeof(tName), "t%zu", regIndex - 8);
        regName = tName;
    } else if (regIndex >= 16 && regIndex <= 23) {
        static char sName[8];
        snprintf(sName, sizeof(sName), "s%zu", regIndex - 16);
        regName = sName;
    } else if (regIndex >= 24 && regIndex <= 25) {
        static char tName[8];
        snprintf(tName, sizeof(tName), "t%zu", regIndex - 16);
        regName = tName;
    } else if (regIndex == 26) regName = "k0";
    else if (regIndex == 27) regName = "k1";
    else if (regIndex == 28) regName = "gp";
    else if (regIndex == 29) regName = "sp";
    else if (regIndex == 30) regName = "fp/s8";
    else if (regIndex == 31) regName = "ra";

    const char* mappingStr = state.IsConstant() ? "const" : state.IsMapped() ? "mapped" : "unmapped";
    const char* widthStr = state.Is32Bit() ? "32" : state.Is64Bit() ? "64" : "??";
    const char* signStr = state.isSigned ? "S" : "U";

    snprintf(buffer, sizeof(buffer), "%-6s %s %s%s = 0x%016llX%s%s", regName, mappingStr, widthStr, signStr,
             static_cast<unsigned long long>(m_Values[regIndex]), state.isModified ? " MOD" : "",
             state.isDirty ? " DIRTY" : "");

    return std::string(buffer);
}

// --- CRegBase Implementation ---

CRegBase::CRegBase() {
    m_Registers.Reset();

    // $zero is always 0 and constant
    m_Registers.SetConstant(0, 0, RegisterWidth::Bits64, false);

    Log::debug("CRegBase: Initialized");
}

CRegBase& CRegBase::operator=(const CRegBase& other) {
    if (this != &other) {
        m_Registers.CopyFrom(other.m_Registers);
        Log::debug("CRegBase: Copied state from another instance");
    }
    return *this;
}

bool CRegBase::operator==(const CRegBase& right) const {
    bool equal = m_Registers.Compare(right.m_Registers);

    if (!equal) {
        Log::debug("CRegBase: Register states differ");
    }

    return equal;
}

bool CRegBase::operator!=(const CRegBase& right) const { return !(*this == right); }

// --- Legacy Interface Implementation ---

void CRegBase::SetMipsRegState(int32_t reg, REG_STATE state) noexcept {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        Log::warningf("CRegBase: Invalid register index %d", reg);
        return;
    }

    auto& regState = m_Registers.GetState(reg);
    regState.Reset();

    if (state & STATE_KNOWN_VALUE) {
        if (state & STATE_X86_MAPPED) {
            regState.mapping = RegisterMapping::MappedToHost;
        } else {
            regState.mapping = RegisterMapping::Constant;
        }
    } else {
        regState.mapping = RegisterMapping::Unknown;
    }

    if (state & STATE_32BIT) {
        regState.width = RegisterWidth::Bits32;
    } else if (state & STATE_KNOWN_VALUE) {
        regState.width = RegisterWidth::Bits64;
    } else {
        regState.width = RegisterWidth::Unknown;
    }

    regState.isSigned = (state & STATE_SIGN) != 0;
    regState.isModified = (state & STATE_MODIFIED) != 0;

    Log::debugf("CRegBase: Set register $%d state to 0x%02X", reg, state);
}

bool CRegBase::IsSigned(int32_t reg) const noexcept {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return false;
    }

    const auto& state = m_Registers.GetState(reg);
    return state.isSigned;
}

const char* CRegBase::RoundingModelName(FpuRoundingMode roundType) const noexcept {
    switch (roundType) {
    case FpuRoundingMode::Default:  return "Default";
    case FpuRoundingMode::Truncate: return "Truncate (ToZero)";
    case FpuRoundingMode::Ceiling:  return "Ceiling (ToPosInf)";
    case FpuRoundingMode::Floor:    return "Floor (ToNegInf)";
    case FpuRoundingMode::Nearest:  return "Nearest";
    case FpuRoundingMode::Unknown:  return "Unknown";
    default:                        return "Invalid";
    }
}

std::string CRegBase::RegisterToString(int32_t reg) const {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return "Invalid register";
    }

    return m_Registers.RegisterToString(static_cast<size_t>(reg));
}

// --- Register state analysis ---

namespace {

/**
 * @brief Analyze register state transitions for optimization
 * 
 * Returns false when both states hold the same constant; true when the
 * transition needs work (unknown to known, constant to mapped, width change).
 */

/**
 * @brief Check if register needs synchronization at section boundary
 */

} // anonymous namespace

// --- Register usage and synchronization analysis ---

// --- Module Initialization ---

} // namespace Nova64::Recompiler
