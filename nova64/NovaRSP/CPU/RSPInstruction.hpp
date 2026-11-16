/**
 * @file RSPInstruction.hpp
 * @brief RSP Instruction Analysis.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "RSPOpcode.hpp"
#include <cstdint>
#include <string>

namespace Nova64::RSP {

// --- Instruction classification flags ---
enum class RSPInstructionFlag : uint8_t {
    Unknown,
    Branch,
    Break,
    GPROperation,
    Jump,
    JumpRegister,
    Load,
    Store,
    MF,             // Move From (COP0/COP2)
    MT,             // Move To (COP0/COP2)
    CF,             // Control From (flags)
    CT,             // Control To (flags)
    Vector,         // Vector computational
    VectorSetAccum, // Vector that writes accumulator
    VectorUseAccum, // Vector that reads accumulator
    VectorLoad,     // Vector memory load
    VectorStore,    // Vector memory store
    InvalidOpcode,
};

// Decoded RSP instruction with analysis metadata
// Used by the recompiler for optimization decisions
class RSPInstruction {
public:
    static constexpr uint32_t UNUSED_OPERAND = ~0u;

    RSPInstruction(uint32_t address, uint32_t instruction);
    RSPInstruction(const RSPInstruction&);
    RSPInstruction& operator=(const RSPInstruction&);

    // Address / target queries
    [[nodiscard]] uint32_t Address() const { return m_Address; }
    [[nodiscard]] uint32_t Value() const { return m_Instruction.Value; }

    [[nodiscard]] uint32_t ConditionalBranchTarget() const;

    // Disassembly
    [[nodiscard]] const char* Name() const;

private:
    RSPInstruction() = delete;

    void DecodeName() const;
    void DecodeSpecialName() const;
    void DecodeRegImmName() const;
    void DecodeCop0Name() const;
    void DecodeCop2Name() const;
    void DecodeLSC2Name(char loadStoreIdent) const;

    static const char* ElementSpecifier(uint32_t element);

    uint32_t m_Address;
    RSPOpcode m_Instruction;

    // Lazy analysis cache (mutable for const analysis)
    mutable RSPInstructionFlag m_Flag{RSPInstructionFlag::Unknown};
    mutable char m_Name[40]{};
    mutable char m_Param[200]{};
    mutable bool m_Analyzed{false};

    // Cached classification bits
    mutable bool m_isBranch{false};
    mutable bool m_isJump{false};
    mutable bool m_isGprOp{false};
    mutable bool m_isVectorOp{false};
    mutable bool m_isVectorStoreOp{false};
    mutable bool m_isMfCop2{false};
    mutable bool m_isMtCop2{false};

    // Accumulator dependency flags
    mutable bool m_ReadAccumLow{false};
    mutable bool m_ReadAccumMid{false};
    mutable bool m_ReadAccumHigh{false};
    mutable bool m_SetAccumLow{false};
    mutable bool m_SetAccumMid{false};
    mutable bool m_SetAccumHigh{false};

    // Register dependencies
    mutable uint32_t m_DestReg{UNUSED_OPERAND};
    mutable uint32_t m_SourceReg0{UNUSED_OPERAND};
    mutable uint32_t m_SourceReg1{UNUSED_OPERAND};
};

} // namespace Nova64::RSP
