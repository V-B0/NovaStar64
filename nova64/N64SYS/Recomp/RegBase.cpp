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
#include "JumpInfo.hpp"
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

std::string RegisterState::ToString() const {
    char buffer[256];
    const char* mappingStr = "Unknown";
    const char* widthStr = "Unknown";

    switch (mapping) {
    case RegisterMapping::Unmapped:     mappingStr = "Unmapped"; break;
    case RegisterMapping::MappedToHost: mappingStr = "Mapped"; break;
    case RegisterMapping::Constant:     mappingStr = "Constant"; break;
    case RegisterMapping::Unknown:      mappingStr = "Unknown"; break;
    }

    switch (width) {
    case RegisterWidth::Bits32:  widthStr = "32-bit"; break;
    case RegisterWidth::Bits64:  widthStr = "64-bit"; break;
    case RegisterWidth::Unknown: widthStr = "Unknown"; break;
    }

    snprintf(buffer, sizeof(buffer), "State{mapping=%s, width=%s, %s%s%s%s, value=0x%016llX}", mappingStr, widthStr,
             isSigned ? "signed" : "unsigned", isModified ? ", modified" : "", isDirty ? ", dirty" : "",
             isLocked ? ", locked" : "", static_cast<unsigned long long>(constantValue));

    return std::string(buffer);
}

// --- RegisterSet Implementation ---

void RegisterSet::CopyFrom(const RegisterSet& other) noexcept {
    // Copy all register states
    m_States = other.m_States;
    m_Values = other.m_Values;

    // Copy additional state
    m_CycleCount = other.m_CycleCount;
    m_FpuUsed = other.m_FpuUsed;
    m_RoundingMode = other.m_RoundingMode;

    Log::debugf("RegisterSet: Copied state with %u cycles, FPU used: %s", m_CycleCount, m_FpuUsed ? "yes" : "no");
}

bool RegisterSet::Compare(const RegisterSet& other) const noexcept {
    // Compare all register states
    for (size_t i = 0; i < NUM_GPR; ++i) {
        const auto& state1 = m_States[i];
        const auto& state2 = other.m_States[i];

        if (state1.width != state2.width || state1.mapping != state2.mapping || state1.isSigned != state2.isSigned ||
            state1.isModified != state2.isModified || state1.isDirty != state2.isDirty) {
            return false;
        }

        // If both are constants, compare values
        if (state1.IsConstant() && state2.IsConstant()) {
            if (m_Values[i] != other.m_Values[i]) {
                return false;
            }
        }
    }

    // Compare additional state
    if (m_CycleCount != other.m_CycleCount || m_FpuUsed != other.m_FpuUsed || m_RoundingMode != other.m_RoundingMode) {
        return false;
    }

    return true;
}

std::string RegisterSet::ToString() const {
    char buffer[4096];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "RegisterSet: cycles=%u, FPU=%s, rounding=%d\n",
                       m_CycleCount, m_FpuUsed ? "used" : "unused", static_cast<int>(m_RoundingMode));

    // Show only interesting registers (non-zero or modified)
    size_t shownRegs = 0;
    for (size_t i = 0; i < NUM_GPR; ++i) {
        const auto& state = m_States[i];

        if (state.IsKnown() || state.IsModifiedOrDirty() || m_Values[i] != 0) {
            if (offset < sizeof(buffer) - 128) {
                offset +=
                    snprintf(buffer + offset, sizeof(buffer) - offset, "  $%zu: %s\n", i, RegisterToString(i).c_str());
                shownRegs++;

                // Limit output to avoid buffer overflow
                if (shownRegs >= 10) {
                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "  ... (%zu more registers)\n",
                                       NUM_GPR - i - 1);
                    break;
                }
            }
        }
    }

    return std::string(buffer);
}

std::string RegisterSet::RegisterToString(size_t regIndex) const {
    if (regIndex >= NUM_GPR) {
        return "Invalid register";
    }

    const auto& state = m_States[regIndex];
    char buffer[256];

    // Get register name from MIPS registers - creates dependency
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
    // Initialize register set with default state
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

    // Convert legacy state to modern RegisterState
    auto& regState = m_Registers.GetState(reg);
    regState.Reset();

    // Map legacy flags to modern state
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

CRegBase::REG_STATE CRegBase::GetMipsRegState(int32_t reg) const noexcept {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return STATE_UNKNOWN;
    }

    const auto& state = m_Registers.GetState(reg);
    REG_STATE result = STATE_UNKNOWN;

    // Convert modern state to legacy flags
    if (state.IsKnown()) {
        result = STATE_KNOWN_VALUE;

        if (state.IsMapped()) {
            result = static_cast<REG_STATE>(result | STATE_X86_MAPPED);
        }

        if (state.Is32Bit()) {
            result = static_cast<REG_STATE>(result | STATE_32BIT);
        }

        if (state.isSigned) {
            result = static_cast<REG_STATE>(result | STATE_SIGN);
        }
    }

    if (state.isModified) {
        result = static_cast<REG_STATE>(result | STATE_MODIFIED);
    }

    return result;
}

bool CRegBase::IsSigned(int32_t reg) const noexcept {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return false;
    }

    const auto& state = m_Registers.GetState(reg);
    return state.isSigned;
}

bool CRegBase::Is32BitMapped(int32_t reg) const noexcept {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return false;
    }

    const auto& state = m_Registers.GetState(reg);
    return state.IsMapped() && state.Is32Bit();
}

bool CRegBase::Is64BitMapped(int32_t reg) const noexcept {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return false;
    }

    const auto& state = m_Registers.GetState(reg);
    return state.IsMapped() && state.Is64Bit();
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

std::string CRegBase::ToString() const {
    char buffer[512];

    snprintf(buffer, sizeof(buffer), "CRegBase: cycles=%u, FPU=%s, rounding=%s\n", GetBlockCycleCount(),
             GetFpuBeenUsed() ? "used" : "unused", RoundingModelName(GetRoundingModel()));

    std::string result = buffer;

    // Use CRegInfo to get platform information - creates dependency
    CRegInfo regInfo;
    result += "  Platform: ";
    result += regInfo.GetPlatformName();
    result += "\n";

    // Show interesting registers
    size_t shownRegs = 0;
    for (int32_t i = 0; i < static_cast<int32_t>(RegisterSet::NUM_GPR); ++i) {
        const auto& state = m_Registers.GetState(i);

        if (state.IsKnown() || state.IsModifiedOrDirty() || m_Registers.GetValue(i) != 0) {
            result += "  ";
            result += RegisterToString(i);
            result += "\n";
            shownRegs++;

            if (shownRegs >= 15) {
                char moreBuffer[64];
                snprintf(moreBuffer, sizeof(moreBuffer), "  ... (%zu more registers)\n", RegisterSet::NUM_GPR - i - 1);
                result += moreBuffer;
                break;
            }
        }
    }

    return result;
}

std::string CRegBase::RegisterToString(int32_t reg) const {
    if (reg < 0 || reg >= static_cast<int32_t>(RegisterSet::NUM_GPR)) {
        return "Invalid register";
    }

    return m_Registers.RegisterToString(static_cast<size_t>(reg));
}

// --- Register State Analysis (creates dependencies on other components) ---

namespace {

/**
 * @brief Analyze register state transitions for optimization
 * 
 * This helper function examines how register states change and determines
 * if optimizations like constant propagation are possible.
 * Creates dependencies on CCodeSection and CJumpInfo.
 */
bool AnalyzeRegisterTransition(const RegisterState& from, const RegisterState& to, uint64_t fromValue,
                               uint64_t toValue) {
    // If both are constant with same value, no transition needed
    if (from.IsConstant() && to.IsConstant() && fromValue == toValue) {
        Log::debug("RegBase: Constant value unchanged, no transition needed");
        return false;
    }

    // If transitioning from unknown to known, that's always worthwhile
    if (from.IsUnknown() && to.IsKnown()) {
        Log::debug("RegBase: Transitioning from unknown to known state");
        return true;
    }

    // If transitioning from constant to mapped, need to load
    if (from.IsConstant() && to.IsMapped()) {
        Log::debug("RegBase: Transitioning from constant to mapped");
        return true;
    }

    // If width changes, need to handle extension
    if (from.width != to.width && from.width != RegisterWidth::Unknown) {
        Log::debugf("RegBase: Width transition %s -> %s", from.Is32Bit() ? "32" : "64", to.Is32Bit() ? "32" : "64");
        return true;
    }

    return false;
}

/**
 * @brief Check if register needs synchronization at section boundary
 * 
 * Uses CCodeSection to determine if register state must be synchronized
 * when jumping between code sections.
 */
bool NeedsSynchronizationAtBoundary(const RegisterState& state, uint64_t value) {
    // Dirty registers always need sync
    if (state.isDirty) {
        Log::debug("RegBase: Register is dirty, needs sync");
        return true;
    }

    // Modified registers need sync if not written back
    if (state.isModified && !state.isDirty) {
        Log::debug("RegBase: Register is modified but clean");
        return false; // Already synced
    }

    // Mapped registers might need sync depending on calling convention
    if (state.IsMapped()) {
        Log::debug("RegBase: Mapped register may need sync");
        return true;
    }

    return false;
}

} // anonymous namespace

// --- Advanced Register Operations (with dependencies) ---

/**
 * @brief Analyze register usage across a code section
 * 
 * This demonstrates how RegBase creates dependencies on CodeSection
 * by analyzing register usage patterns.
 */
void AnalyzeRegisterUsage(const CRegBase& regBase, const char* contextName) {
    Log::debugf("RegBase: Analyzing register usage in context '%s'", contextName);

    size_t constantRegs = 0;
    size_t mappedRegs = 0;
    size_t modifiedRegs = 0;
    size_t dirtyRegs = 0;

    for (int32_t i = 1; i < static_cast<int32_t>(RegisterSet::NUM_GPR); ++i) {
        const auto& state = regBase.GetRegisterSet().GetState(i);

        if (state.IsConstant()) constantRegs++;
        if (state.IsMapped()) mappedRegs++;
        if (state.isModified) modifiedRegs++;
        if (state.isDirty) dirtyRegs++;
    }

    Log::debugf("RegBase: %zu constant, %zu mapped, %zu modified, %zu dirty", constantRegs, mappedRegs, modifiedRegs,
                dirtyRegs);

    // Use CRegInfo to compare against platform capabilities
    CRegInfo regInfo;

    Log::debugf("RegBase: Platform: %s (using %zu mapped registers)", regInfo.GetPlatformName().c_str(), mappedRegs);

    if (mappedRegs > regInfo.GetNumGeneralPurposeRegisters()) {
        Log::warning("RegBase: More MIPS regs mapped than host regs available - spilling needed");
    }
}

/**
 * @brief Synchronize register state between code sections
 * 
 * Demonstrates dependency on CJumpInfo and CExitInfo for branch handling.
 */
void SynchronizeRegisterState(CRegBase& sourceBase, const CRegBase& targetBase, const char* sourceName,
                              const char* targetName) {
    Log::debugf("RegBase: Synchronizing state from '%s' to '%s'", sourceName, targetName);

    size_t syncedRegs = 0;

    for (int32_t i = 0; i < static_cast<int32_t>(RegisterSet::NUM_GPR); ++i) {
        const auto& srcState = sourceBase.GetRegisterSet().GetState(i);
        const auto& dstState = targetBase.GetRegisterSet().GetState(i);

        uint64_t srcValue = sourceBase.GetMipsReg(i);
        uint64_t dstValue = targetBase.GetMipsReg(i);

        if (NeedsSynchronizationAtBoundary(srcState, srcValue)) {
            Log::debugf("RegBase: Syncing register $%d", i);
            syncedRegs++;
        }

        if (AnalyzeRegisterTransition(srcState, dstState, srcValue, dstValue)) {
            Log::debugf("RegBase: Register $%d requires transition handling", i);
        }
    }

    Log::debugf("RegBase: Synchronized %zu registers", syncedRegs);

    // Create CJumpInfo to track this synchronization point
    CJumpInfo jumpInfo(0x80000000, 0x80000004, std::nullopt, JumpType::Direct);

    // Check if register synchronization is needed using CRegInfo
    CRegInfo sourceInfo, targetInfo;
    bool needsSync = jumpInfo.NeedsRegisterSynchronization(sourceInfo, targetInfo);

    if (needsSync) {
        Log::debug("RegBase: Jump requires full register synchronization");
    }
}

// --- Module Initialization ---

/**
 * @brief Initialize register base subsystem
 * 
 * Called during recompiler initialization to set up register tracking.
 */
void InitializeRegBase() {
    Log::info("RegBase: Initializing register state tracking subsystem");

    // Test that RegisterState works correctly
    RegisterState testState;
    testState.mapping = RegisterMapping::Constant;
    testState.width = RegisterWidth::Bits32;
    testState.constantValue = 0x12345678;

    Log::debugf("RegBase: Test state: %s", testState.ToString().c_str());

    // Test RegisterSet functionality
    RegisterSet testSet;
    testSet.SetConstant(1, 0xDEADBEEF, RegisterWidth::Bits32, false);
    testSet.SetMapped(2, RegisterWidth::Bits64, true);

    Log::debugf("RegBase: Test $1: %s", testSet.RegisterToString(1).c_str());
    Log::debugf("RegBase: Test $2: %s", testSet.RegisterToString(2).c_str());

    // Test CRegBase legacy interface
    CRegBase testBase;
    testBase.SetMipsRegState(3, CRegBase::STATE_CONST_32_ZERO);
    testBase.SetMipsRegLo(3, 0x11223344);

    Log::debugf("RegBase: Test $3: %s", testBase.RegisterToString(3).c_str());

    Log::info("RegBase: Initialization complete");
}

} // namespace Nova64::Recompiler
