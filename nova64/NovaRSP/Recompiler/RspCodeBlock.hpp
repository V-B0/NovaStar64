// SuperNova64 - N64 Emulator
// RSP Code Block - Modernized from Project64's RspCodeBlock.h
// C++20 with proper code analysis and block management

#pragma once
#include "../CPU/RSPInstruction.hpp"
#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace Nova64::RSP {
class CRSPSystem;
}

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Block type classification
// ──────────────────────────────────────────────
enum class RspCodeType : uint8_t {
    Task,        // Main task entry point
    Subroutine,  // Called subroutine
};

// ──────────────────────────────────────────────
// Type aliases
// ──────────────────────────────────────────────
class RspCodeBlock;
using RspCodeBlockPtr = std::unique_ptr<RspCodeBlock>;
using RspCodeBlocks   = std::unordered_map<uint32_t, RspCodeBlockPtr>;
using RSPInstructions = std::vector<RSPInstruction>;

// ──────────────────────────────────────────────
// Compiled RSP code block
// Represents a contiguous sequence of RSP instructions
// with analysis metadata for the recompiler
// ──────────────────────────────────────────────
class RspCodeBlock {
    using InstructionIndexMap = std::unordered_map<uint32_t, size_t>;

public:
    using Addresses = std::set<uint32_t>;

    RspCodeBlock(
        CRSPSystem& system,
        uint32_t startAddress,
        RspCodeType type,
        uint32_t dispatchAddress,
        RspCodeBlocks& functions);

    ~RspCodeBlock() = default;

    RspCodeBlock(const RspCodeBlock&) = delete;
    RspCodeBlock& operator=(const RspCodeBlock&) = delete;

    // Query interface
    [[nodiscard]] const Addresses& GetBranchTargets() const  { return m_BranchTargets; }
    [[nodiscard]] void* GetCompiledLocation() const          { return m_CompiledLocation; }
    [[nodiscard]] uint32_t GetDispatchAddress() const        { return m_DispatchAddress; }
    [[nodiscard]] const Addresses& GetFunctionCalls() const  { return m_FunctionCalls; }
    [[nodiscard]] const RSPInstructions& GetInstructions() const { return m_Instructions; }
    [[nodiscard]] const RspCodeBlock* GetFunctionBlock(uint32_t address) const;
    [[nodiscard]] uint32_t GetStartAddress() const           { return m_StartAddress; }
    [[nodiscard]] size_t InstructionIndex(uint32_t pc) const;
    [[nodiscard]] RspCodeType CodeType() const               { return m_CodeType; }
    [[nodiscard]] bool IsEnd(uint32_t address) const;
    [[nodiscard]] bool IsValid() const                       { return m_Valid; }

    void SetCompiledLocation(void* location) { m_CompiledLocation = location; }

private:
    RspCodeBlock() = delete;

    void Analyze();
    bool IsAddressInInstructions(uint32_t address) const;
    void BuildInstructionIndex();

    RspCodeBlocks&     m_Functions;
    const uint32_t     m_DispatchAddress;
    RSPInstructions    m_Instructions;
    InstructionIndexMap m_InstructionIndex;
    uint32_t           m_StartAddress;
    RspCodeType        m_CodeType;
    CRSPSystem&        m_System;
    Addresses          m_End;
    Addresses          m_BranchTargets;
    Addresses          m_FunctionCalls;
    void*              m_CompiledLocation{nullptr};
    bool               m_Valid{false};
};

} // namespace Nova64::RSP::Recompiler
