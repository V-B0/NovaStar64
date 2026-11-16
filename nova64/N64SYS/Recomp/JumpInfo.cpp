/**
 * @file JumpInfo.cpp
 * @brief Implementation of jump/branch information tracking
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This implementation manages jump and branch target information during
 * dynamic recompilation, supporting both direct linking and deferred
 * patching for unknown targets at compile time.
 */

#include "JumpInfo.hpp"
#include "CodeBlock.hpp"
#include "ExitInfo.hpp"
#include "RegInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../../common/log.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdio>
#include <string>

namespace Nova64::Recompiler {

// Helper for string conversion
static const char* GetJumpTypeName(JumpType type) {
    switch (type) {
    case JumpType::Direct:      return "Direct";
    case JumpType::Indirect:    return "Indirect";
    case JumpType::Conditional: return "Conditional";
    case JumpType::Call:        return "Call";
    case JumpType::Return:      return "Return";
    case JumpType::Link:        return "Link";
    case JumpType::System:      return "System";
    case JumpType::Unknown:     return "Unknown";
    default:                    return "Invalid";
    }
}

static std::string GetExitReasonName(ExitReason reason) { return std::to_string(static_cast<uint16_t>(reason)); }

// Note: Some CCodeBlock methods are used but require the full definition
// These will be moved to inline implementations or use simpler accessors

// --- CJumpInfo Implementation ---

CJumpInfo::CJumpInfo(CodeBlock& codeBlock) :
    m_JumpPC(0), m_TargetPC(0), m_LinkAddress(std::nullopt), m_Type(JumpType::Unknown), m_CanFallThrough(false),
    m_IsPermanentLoop(false), m_DelaySlotDone(false), m_IsResolved(false), m_ExitReason(ExitReason::NormalBlockEnd) {
    // Extract jump information from the code block
    // Note: Since CCodeBlock is forward declared only, we use simpler access
    // Full implementation would extract detailed state once header is complete

    // For now, just log that we're tracking this code block
    // Real implementation would call: m_JumpPC = codeBlock.GetCurrentPC();
    Log::debug("CJumpInfo: Created jump info from code block");

    // This creates a dependency on CodeBlock - once fully defined,
    // we would inspect its state, detect loops, check existing jumps, etc.
}

CJumpInfo::CJumpInfo(uint32_t jumpPC, uint32_t targetPC, std::optional<uint32_t> linkAddr, JumpType type) :
    m_JumpPC(jumpPC), m_TargetPC(targetPC), m_LinkAddress(linkAddr), m_Type(type), m_CanFallThrough(false),
    m_IsPermanentLoop(false), m_DelaySlotDone(false), m_IsResolved(false), m_ExitReason(ExitReason::NormalBlockEnd) {
    // Validate parameters
    if (jumpPC == 0) {
        Log::warning("CJumpInfo: Jump PC is zero");
    }

    // For direct jumps and calls, target should be known
    if ((type == JumpType::Direct || type == JumpType::Call) && targetPC == 0) {
        Log::warningf("CJumpInfo: Direct jump/call at 0x%08X has unknown target", jumpPC);
    }

    // For calls, link address should be provided
    if (type == JumpType::Call && !linkAddr.has_value()) {
        Log::warningf("CJumpInfo: Call at 0x%08X has no link address", jumpPC);
    }

    // Detect self-loops (permanent loops)
    if (targetPC == jumpPC && type != JumpType::Indirect) {
        m_IsPermanentLoop = true;
        Log::debugf("CJumpInfo: Permanent loop detected at 0x%08X", jumpPC);
    }

    // Conditional branches can fall through
    if (type == JumpType::Conditional) {
        m_CanFallThrough = true;
    }

    // Set appropriate exit reasons based on jump type
    switch (type) {
    case JumpType::System:   m_ExitReason = ExitReason::Syscall; break;
    case JumpType::Call:
    case JumpType::Return:   m_ExitReason = ExitReason::NormalBlockEnd; break;
    case JumpType::Indirect: m_ExitReason = ExitReason::JumpRegister; break;
    default:                 m_ExitReason = ExitReason::NormalBlockEnd; break;
    }

    // Generate debug label
    char label[64];
    std::snprintf(label, sizeof(label), "jump_0x%08X_to_0x%08X", jumpPC, targetPC);
    m_BranchLabel = label;

    Log::debugf("CJumpInfo: Created %s at 0x%08X -> 0x%08X", GetJumpTypeName(type), jumpPC, targetPC);
}

std::string CJumpInfo::ToString() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "JumpInfo[PC=0x%08X, Target=0x%08X, Type=%s", m_JumpPC, m_TargetPC,
                  GetJumpTypeName(m_Type));
    std::string result = buffer;

    if (m_LinkAddress.has_value()) {
        std::snprintf(buffer, sizeof(buffer), ", Link=0x%08X", *m_LinkAddress);
        result += buffer;
    }

    if (m_CanFallThrough) {
        result += ", FallThrough";
    }

    if (m_IsPermanentLoop) {
        result += ", PermanentLoop";
    }

    if (m_DelaySlotDone) {
        result += ", DelaySlotDone";
    }

    if (m_IsResolved) {
        result += ", Resolved";
    }

    if (!m_BranchLabel.empty()) {
        result += ", Label=\"" + m_BranchLabel + "\"";
    }

    result += ", ExitReason=";
    result += GetExitReasonName(m_ExitReason);

    result += "]";
    return result;
}

bool CJumpInfo::NeedsRegisterSynchronization(const CRegInfo& sourceReg, const CRegInfo& targetReg) const {
    // Check if registers need to be synchronized when taking this jump
    // This creates dependencies on CRegInfo's state inspection

    // For indirect jumps, always synchronize (target unknown at compile time)
    if (m_Type == JumpType::Indirect) {
        Log::debug("CJumpInfo: Indirect jump requires full register sync");
        return true;
    }

    // For system calls, registers are modified - need sync
    if (m_Type == JumpType::System) {
        Log::debug("CJumpInfo: System call requires register sync");
        return true;
    }

    // For permanent loops, registers must be consistent
    if (m_IsPermanentLoop) {
        Log::debug("CJumpInfo: Permanent loop requires register sync");
        return true;
    }

    // Compare register states - if they differ, sync is needed
    // This uses CRegInfo's state inspection methods
    const auto& sourceStats = sourceReg.GetStatistics();
    const auto& targetStats = targetReg.GetStatistics();

    if (sourceStats.allocations != targetStats.allocations) {
        Log::debugf("CJumpInfo: Register allocation mismatch (%zu vs %zu)", sourceStats.allocations,
                    targetStats.allocations);
        return true;
    }

    return false;
}

void CJumpInfo::UpdateExitInfo(CExitInfo& exitInfo, const CRegInfo& regInfo) const {
    // Update exit information based on this jump
    // Creates dependency on both CExitInfo and CRegInfo
    // Note: Demonstrates relationship - actual ExitInfo class may have different interface

    // Copy register allocation statistics from CRegInfo
    const auto& stats = regInfo.GetStatistics();
    Log::debugf("CJumpInfo: Exit with %zu register allocations, %zu spills", stats.allocations, stats.spills);

    // Log exit details showing the data that would be passed to ExitInfo
    Log::debugf("CJumpInfo: Exit reason %d, target PC 0x%08X, can fall-through: %s", static_cast<int>(m_ExitReason),
                m_TargetPC, m_CanFallThrough ? "yes" : "no");

    if (m_LinkAddress.has_value()) {
        Log::debugf("CJumpInfo: Link address 0x%08X", m_LinkAddress.value());
    }

    // This demonstrates the inter-dependency:
    // - JumpInfo provides jump/branch metadata
    // - RegInfo provides register state
    // - ExitInfo would combine these for code generation
}

} // namespace Nova64::Recompiler
