/**
 * @file RspRecompilerCPU.hpp
 * @brief RSP Recompiler CPU.
 */
// SuperNova64 - N64 Emulator
// RSP Recompiler CPU - Modernized from Project64's RspRecompilerCPU-x64.h
// x64 JIT compiler managing code generation and dispatch

#pragma once
#if defined(__amd64__) || defined(_M_X64)

#include "RspCodeBlock.hpp"
#include "RspRecompilerOps.hpp"
#include "RspRegState.hpp"
#include "../CPU/RSPOpcode.hpp"
#include "../CPU/RspPipelineStage.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace Nova64::RSP {
class CRSPSystem;
}

namespace Nova64::RSP::Recompiler {

class RspAssembler;

// ──────────────────────────────────────────────
// CRSPRecompiler - Main JIT compilation engine
// Compiles RSP blocks into native x64 code
// ──────────────────────────────────────────────
class CRSPRecompiler {
    using BranchTargets = std::unordered_map<uint32_t, uint32_t>; // PC → label id

    friend CRSPRecompilerOps;

public:
    explicit CRSPRecompiler(CRSPSystem& system);
    ~CRSPRecompiler();

    CRSPRecompiler(const CRSPRecompiler&) = delete;
    CRSPRecompiler& operator=(const CRSPRecompiler&) = delete;

    // ── Lifecycle ──
    void Reset();

    // ── Assembler management ──
    void EnsureAssembler(uint8_t* codeBuffer, size_t bufferSize);
    [[nodiscard]] bool HasAssembler() const { return m_Assembler != nullptr; }

    // ── Task compilation ──
    void* CompileTaskEnter();
    void* CompileTaskLeave();
    void* CompileHLETask(uint32_t address, RspCodeBlocks& functions, uint32_t dispatchAddress);

    // ── Logging ──
    void Log(const char* text, ...);

    // ── Address-of helper for recompiler ops ──
    static void* GetAddressOf(int32_t value, ...);

private:
    CRSPRecompiler() = delete;

    void ClearBranchJump();
    void AddBranchJump(uint32_t target);
    bool FindBranchJump(uint32_t target, uint32_t& labelId);
    void BuildRecompilerCPU();
    bool CompileSubFunctions(RspCodeBlocks& functions, const RspCodeBlock::Addresses& addresses);
    void CompileCodeBlock(RspCodeBlock& block);
    void CompileOpcode(uint32_t pc);
    void SetupRspAssembler();
    void FinalizeAssembler(void* funcPtr);

    // ── State ──
    CRSPSystem&          m_System;
    CRSPRecompilerOps    m_RecompilerOps;
    PipelineStage        m_NextInstruction{PipelineStage::Normal};
    const RspCodeBlock*  m_CurrentBlock{nullptr};
    std::string          m_CodeLog;
    uint32_t             m_BlockID{0};
    uint32_t             m_CompilePC{0};
    RSPOpcode            m_OpCode;
    RspAssembler*        m_Assembler{nullptr};
    BranchTargets        m_BranchTargets;
    CRspRegState         m_RegState;

    // Save buffer for callee-saved registers
    alignas(16) uint8_t  m_SaveBuffer[176]{};
};

} // namespace Nova64::RSP::Recompiler

#define AddressOf(Addr) Nova64::RSP::Recompiler::CRSPRecompiler::GetAddressOf(5, (Addr))

#endif // x64 only
