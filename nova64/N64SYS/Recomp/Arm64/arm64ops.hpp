/**
 * @file arm64ops.hpp
 * @brief AArch64 (ARM64) assembler operations wrapper and code generation utilities
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * This class is the AArch64 counterpart of CX64Ops.  It provides a high level
 * interface for generating ARM64 machine code using AsmJit's a64 backend, with
 * support for logging, symbol management and debugging.
 *
 * Apple Silicon / AArch64 notes
 * -----------------------------
 *  - The platform ABI is always AAPCS64 (no Win64/SysV split like x86), so the
 *    argument registers are x0..x7 unconditionally.
 *  - Integer args:        x0, x1, x2, x3, ...
 *  - Integer return:      x0 (w0 for 32-bit)
 *  - Callee-saved GPRs:   x19..x28  (we pin emulator state pointers here)
 *  - Caller-saved scratch: x9..x15, x16 (IP0), x17 (IP1)
 *  - Frame/link:          x29 (FP), x30 (LR), sp must stay 16-byte aligned.
 *
 * Pinned register convention (set up in CArm64RecompilerOps::EnterCodeBlock):
 *      x19 = &CRegisters
 *      x20 = &MipsMemoryVM
 *      x21 = &m_PipelineStage
 *      x22 = &m_NextTimer
 *      x23 = &MemoryReadMap[0]
 * These mirror the x64 backend's RBX/R12/R13/R14/R15.
 */

#pragma once

#if defined(__aarch64__) || defined(_M_ARM64)

#include "../asmjit.hpp"
#include "../CodeBlock.hpp"
#include <unordered_map>
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <functional>
#include <cstdint>
#include <optional>
#include <array>
#include <stdexcept>

namespace Nova64 {
namespace Recompiler {

// Forward declarations
class CodeBlock;

/**
 * @struct Arm64EmitOptions
 * @brief Options controlling AArch64 code generation
 */
struct Arm64EmitOptions {
    bool enableLogging = false;
    bool enableValidation = true;
    bool emitDebugComments = false;
    bool emitAddressComments = false;
};

/**
 * @class CArm64Ops
 * @brief AArch64 assembler wrapper using AsmJit (a64) backend.
 *
 * Mirrors the public surface of CX64Ops that the recompiler relies on:
 *  - Attach to a CodeHolder (Initialize)
 *  - Emit calls to native C++ callbacks (EmitCall)
 *  - Debug comments / logging
 *  - Direct access to the underlying assembler for advanced emission
 */
class CArm64Ops final : private asmjit::a64::Assembler, private asmjit::Logger {
public:
    explicit CArm64Ops(CodeBlock& codeBlock, Arm64EmitOptions options = Arm64EmitOptions());
    ~CArm64Ops();

    // Non-copyable
    CArm64Ops(const CArm64Ops&) = delete;
    CArm64Ops& operator=(const CArm64Ops&) = delete;

    /**
     * @brief Attach the assembler to an AsmJit code holder so emitted bytes
     *        land in the recompiler's buffer.
     */
    bool Initialize(asmjit::CodeHolder& codeHolder);

    void SetEmitOptions(const Arm64EmitOptions& options) noexcept { m_Options = options; }
    const Arm64EmitOptions& GetEmitOptions() const noexcept { return m_Options; }

    // Symbol management ------------------------------------------------------
    struct SymbolInfo {
        std::string name;
        asmjit::Label label;
        uint64_t address = 0;
        bool isExported = false;
        bool IsValid() const noexcept { return label.is_valid(); }
    };

    asmjit::Label DefineSymbol(std::string_view name, bool isExported = false);
    std::optional<SymbolInfo> GetSymbol(std::string_view name) const;

    // Logging / debugging ----------------------------------------------------
    void EnableLogging(bool enable) noexcept { m_Options.enableLogging = enable; }
    bool IsLoggingEnabled() const noexcept { return m_Options.enableLogging; }
    void SetLogCallback(std::function<void(std::string_view)> callback) noexcept {
        m_LogCallback = std::move(callback);
    }
    void AddComment(std::string_view comment);

    // Code generation helpers ------------------------------------------------
    /**
     * @brief Emit a call to a native function pointer.
     *
     * AArch64 cannot encode an arbitrary 64-bit target in a branch, so the
     * target is materialised into x16 (IP0, an intra-procedure-call scratch
     * register that the ABI permits the linker/JIT to clobber) and reached
     * with BLR.  For tail calls we use BR instead of BLR.
     */
    void EmitCall(void* target, bool isTailCall = false);

    /**
     * @brief Materialise a 64-bit immediate into a register using
     *        MOVZ/MOVK.  AsmJit can usually do this directly via mov(reg, imm)
     *        but we wrap it so call sites stay arch-neutral and readable.
     */
    void MovImm64(const asmjit::a64::Gp& dst, uint64_t value);

    /**
     * @brief Load the address of an absolute host pointer into a register.
     *        Equivalent to x64's `mov rax, imm64`.
     */
    void LoadHostPtr(const asmjit::a64::Gp& dst, const void* ptr) { MovImm64(dst, reinterpret_cast<uint64_t>(ptr)); }

    // Error handling ---------------------------------------------------------
    bool HasError() const noexcept { return m_LastError != asmjit::kErrorOk; }
    asmjit::Error GetLastError() const noexcept { return m_LastError; }
    void ClearError() noexcept { m_LastError = asmjit::kErrorOk; }

    // Direct access to AsmJit assembler (for advanced use) -------------------
    asmjit::a64::Assembler& GetAssembler() noexcept { return *this; }
    const asmjit::a64::Assembler& GetAssembler() const noexcept { return *this; }

private:
    // Logger interface
    asmjit::Error _log(const char* data, size_t size) noexcept override;

    void HandleError(asmjit::Error error, std::string_view context = "");

    using SymbolMap = std::unordered_map<std::string, SymbolInfo>;
    SymbolMap m_Symbols;

    [[maybe_unused]] CodeBlock& m_CodeBlock;
    Arm64EmitOptions m_Options;

    asmjit::Error m_LastError = asmjit::kErrorOk;
    std::string m_LastErrorContext;

    std::function<void(std::string_view)> m_LogCallback;
    asmjit::CodeHolder* m_CodeHolder = nullptr;
};

inline void CArm64Ops::AddComment(std::string_view comment) {
    if (m_Options.emitDebugComments) {
        asmjit::a64::Assembler::comment(comment.data(), comment.size());
    }
}

inline std::optional<CArm64Ops::SymbolInfo> CArm64Ops::GetSymbol(std::string_view name) const {
    auto it = m_Symbols.find(std::string(name));
    if (it != m_Symbols.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace Recompiler
} // namespace Nova64

#endif // __aarch64__ || _M_ARM64
