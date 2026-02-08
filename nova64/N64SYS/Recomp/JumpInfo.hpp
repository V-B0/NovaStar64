/**
 * @file JumpInfo.hpp
 * @brief Jump/branch information for code sections in the dynamic recompiler
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
 * 
 * This structure holds information about branch and jump targets during
 * dynamic recompilation, including both static (PC-based) and runtime
 * (label-based) linking information.
 */

#pragma once

#include "asmjit.hpp"
#include "ExitInfo.hpp"
#include "../Types.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <string>
#include <optional>
#include <string_view>

namespace Nova64 {
namespace Recompiler {

// Forward declarations
class CCodeBlock;
class CRegInfo;
class CExitInfo;

/**
 * @enum JumpType
 * @brief Classification of jump/branch types for optimization
 */
enum class JumpType : uint8_t {
    Direct,     ///< Direct jump/branch with known target
    Indirect,   ///< Indirect jump via register
    Conditional, ///< Conditional branch
    Call,       ///< Function call (JAL/JALR)
    Return,     ///< Return from function
    Link,       ///< Linked jump (JAL with return address)
    System,     ///< System call/exception
    Unknown     ///< Unknown/unclassified jump
};

/**
 * @class CJumpInfo
 * @brief Contains information about branch/jump targets for code linking
 * 
 * This class holds both compile-time and runtime information needed to
 * link jumps between recompiled code blocks. It supports both direct
 * linking (when target is known) and deferred linking (when target
 * needs to be patched later).
 */
class CJumpInfo {
public:
    /**
     * @brief Construct a jump info for a specific code block
     * @param codeBlock Reference to the containing code block
     */
    explicit CJumpInfo(CCodeBlock& codeBlock);
    
    /**
     * @brief Construct a jump info with specific parameters
     * @param jumpPC Address of the jump instruction
     * @param targetPC Target address (if known)
     * @param linkAddr Link register value for JAL/JALR
     * @param type Classification of the jump
     */
    CJumpInfo(uint32_t jumpPC, uint32_t targetPC, 
              std::optional<uint32_t> linkAddr, JumpType type);
    
    // Allow default copy/move since we're just storing data
    CJumpInfo(const CJumpInfo&) = default;
    CJumpInfo(CJumpInfo&&) = default;
    CJumpInfo& operator=(const CJumpInfo&) = default;
    CJumpInfo& operator=(CJumpInfo&&) = default;
    ~CJumpInfo() = default;

    // Getters
    uint32_t GetJumpPC() const noexcept { return m_JumpPC; }
    uint32_t GetTargetPC() const noexcept { return m_TargetPC; }
    std::optional<uint32_t> GetLinkAddress() const noexcept { return m_LinkAddress; }
    JumpType GetJumpType() const noexcept { return m_Type; }
    ExitReason GetExitReason() const noexcept { return m_ExitReason; }
    bool IsFallThrough() const noexcept { return m_CanFallThrough; }
    bool IsPermanentLoop() const noexcept { return m_IsPermanentLoop; }
    bool IsDelaySlotDone() const noexcept { return m_DelaySlotDone; }
    std::string_view GetBranchLabel() const noexcept { return m_BranchLabel; }
    
    // Get AsmJit labels (const and non-const versions)
    const asmjit::Label& GetLinkLocation() const noexcept { return m_LinkLocation; }
    asmjit::Label& GetLinkLocation() noexcept { return m_LinkLocation; }
    
    const asmjit::Label& GetSecondaryLinkLocation() const noexcept { return m_SecondaryLinkLocation; }
    asmjit::Label& GetSecondaryLinkLocation() noexcept { return m_SecondaryLinkLocation; }
    
    // Setters
    void SetTargetPC(uint32_t targetPC) noexcept { m_TargetPC = targetPC; }
    void SetLinkAddress(std::optional<uint32_t> linkAddr) noexcept { m_LinkAddress = linkAddr; }
    void SetJumpType(JumpType type) noexcept { m_Type = type; }
    void SetExitReason(ExitReason reason) noexcept { m_ExitReason = reason; }
    void SetFallThrough(bool fallThrough) noexcept { m_CanFallThrough = fallThrough; }
    void SetPermanentLoop(bool permLoop) noexcept { m_IsPermanentLoop = permLoop; }
    void SetDelaySlotDone(bool done) noexcept { m_DelaySlotDone = done; }
    void SetBranchLabel(std::string_view label) { m_BranchLabel = label; }
    
    /**
     * @brief Check if this jump has a known target address
     */
    bool HasKnownTarget() const noexcept {
        return m_TargetPC != 0 && m_TargetPC != UINT32_MAX;
    }
    
    /**
     * @brief Check if this jump requires linking at runtime
     */
    bool RequiresLinking() const noexcept {
        return !m_LinkLocation.isValid() || !m_SecondaryLinkLocation.isValid();
    }
    
    /**
     * @brief Check if this is a function call with return address
     */
    bool IsFunctionCall() const noexcept {
        return m_Type == JumpType::Call && m_LinkAddress.has_value();
    }
    
    /**
     * @brief Check if this is a conditional branch
     */
    bool IsConditional() const noexcept {
        return m_Type == JumpType::Conditional;
    }
    
    /**
     * @brief Mark as resolved (both link locations are valid)
     */
    void MarkResolved() noexcept {
        // Both labels should be bound by now
        m_IsResolved = true;
    }
    
    bool IsResolved() const noexcept { return m_IsResolved; }
    
    /**
     * @brief Reset the jump info for reuse
     */
    void Reset() noexcept {
        m_TargetPC = 0;
        m_LinkAddress.reset();
        m_BranchLabel.clear();
        m_LinkLocation = asmjit::Label();
        m_SecondaryLinkLocation = asmjit::Label();
        m_CanFallThrough = false;
        m_IsPermanentLoop = false;
        m_DelaySlotDone = false;
        m_ExitReason = ExitReason::NormalBlockEnd;
        m_Type = JumpType::Unknown;
        m_IsResolved = false;
    }
    
    /**
     * @brief Get a string representation for debugging
     */
    std::string ToString() const;
    
    /**
     * @brief Check if registers need synchronization for this jump
     * @param sourceReg Register state at jump site
     * @param targetReg Register state at jump target
     * @return true if synchronization is needed
     * 
     * Creates dependency on CRegInfo for register state comparison
     */
    bool NeedsRegisterSynchronization(const CRegInfo& sourceReg, const CRegInfo& targetReg) const;
    
    /**
     * @brief Update exit information based on this jump
     * @param exitInfo Exit information to update
     * @param regInfo Current register state
     * 
     * Creates dependencies on both CExitInfo and CRegInfo
     */
    void UpdateExitInfo(CExitInfo& exitInfo, const CRegInfo& regInfo) const;

private:
    // Instruction information
    uint32_t m_JumpPC = 0;                   ///< Address of the jump instruction
    uint32_t m_TargetPC = 0;                 ///< Jump target address (0 = unknown)
    std::optional<uint32_t> m_LinkAddress;   ///< Link register value (JAL/JALR)
    JumpType m_Type = JumpType::Unknown;     ///< Type of jump
    
    // Debug information
    std::string m_BranchLabel;               ///< Debug label name for disassembly
    
    // AsmJit linking information
    asmjit::Label m_LinkLocation;            ///< Primary link point in generated code
    asmjit::Label m_SecondaryLinkLocation;   ///< Secondary link point (for patch-up)
    
    // State flags
    bool m_CanFallThrough = false;           ///< Whether execution can fall through
    bool m_IsPermanentLoop = false;          ///< Permanent loop detected (optimization hint)
    bool m_DelaySlotDone = false;            ///< Delay slot has been compiled
    bool m_IsResolved = false;               ///< Whether jump has been linked
    
    // Exit information
    ExitReason m_ExitReason = ExitReason::NormalBlockEnd;
    
    // Note: RegSet will be added as CRegInfo* when RegInfo.hpp is included
    // CRegInfo* m_RegSet = nullptr;
};

// Helper functions
namespace JumpInfo {
    /**
     * @brief Create a jump info for a direct jump
     */
    inline CJumpInfo CreateDirectJump(uint32_t jumpPC, uint32_t targetPC) {
        return CJumpInfo(jumpPC, targetPC, std::nullopt, JumpType::Direct);
    }
    
    /**
     * @brief Create a jump info for a function call
     */
    inline CJumpInfo CreateCall(uint32_t jumpPC, uint32_t targetPC, uint32_t linkAddr) {
        return CJumpInfo(jumpPC, targetPC, linkAddr, JumpType::Call);
    }
    
    /**
     * @brief Create a jump info for a conditional branch
     */
    inline CJumpInfo CreateConditionalBranch(uint32_t jumpPC, uint32_t targetPC) {
        return CJumpInfo(jumpPC, targetPC, std::nullopt, JumpType::Conditional);
    }
}

} // namespace Recompiler
} // namespace Nova64