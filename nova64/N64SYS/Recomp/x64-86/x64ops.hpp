/**
 * @file x64ops.hpp
 * @brief x64 assembler operations wrapper and code generation utilities
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class provides a high-level interface for generating x64 machine code
 * using AsmJit, with support for logging, symbol management, and debugging.
 */

#pragma once

#if defined(__amd64__) || defined(_M_X64)

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

class CodeBlock;

/**
 * @enum EmitMode
 * @brief Different modes for code emission
 */
enum class EmitMode : uint8_t {
    Fast,     ///< Fast code generation (minimal checks)
    Safe,     ///< Safe code generation (with validation)
    Debug,    ///< Debug code generation (with assertions and logging)
    Profile,  ///< Profiling code generation (with instrumentation)
    Optimized ///< Optimized code generation (with peephole optimizations)
};

/**
 * @struct EmitOptions
 * @brief Options controlling code generation
 */
struct EmitOptions {
    bool enableLogging = false;
    bool enableValidation = true;
    bool enableOptimizations = true;
    bool emitDebugComments = false;
    bool emitAddressComments = false;
    bool useRelativeJumps = true;
    uint32_t maxInstructionSize = 15; // Maximum x64 instruction size

    // Optimization flags
    bool optimizeSmallJumps = true;
    bool optimizeRedundantMoves = true;
    bool useShortForms = true;
    bool alignLoops = false;
    uint32_t loopAlignment = 16;

    // Debugging
    bool addTrapOnError = false;
    bool generateStackMaps = false;
    bool preserveRegistersOnCall = false;
};

/**
 * @class CX64Ops
 * @brief x64 assembler wrapper using AsmJit with enhanced features
 * 
 * Provides a comprehensive interface for x64 assembly code generation with:
 * - Symbol management and relocation
 * - Debug logging and disassembly
 * - Code validation and optimization
 * - Error handling and recovery
 * - Performance profiling hooks
 */
class CX64Ops final : private asmjit::x86::Assembler, private asmjit::Logger {
public:
    /**
     * @brief Construct the x64 assembler
     * @param codeBlock Reference to the containing code block
     * @param options Code generation options
     */
    explicit CX64Ops(CodeBlock& codeBlock, EmitOptions options = {});
    ~CX64Ops();

    CX64Ops(const CX64Ops&) = delete;
    CX64Ops& operator=(const CX64Ops&) = delete;
    CX64Ops(CX64Ops&&) noexcept;
    CX64Ops& operator=(CX64Ops&&) noexcept;

    /**
     * @brief Initialize the assembler with a code holder
     * @param codeHolder AsmJit code holder
     * @return true if successful
     */
    bool Initialize(asmjit::CodeHolder& codeHolder);

    void SetEmitMode(EmitMode mode) noexcept { m_EmitMode = mode; }
    EmitMode GetEmitMode() const noexcept { return m_EmitMode; }

    void SetEmitOptions(const EmitOptions& options) noexcept { m_Options = options; }
    const EmitOptions& GetEmitOptions() const noexcept { return m_Options; }

    // Symbol management
    struct SymbolInfo {
        std::string name;
        asmjit::Label label;
        uint64_t address = 0;
        bool isExported = false;
        bool isImported = false;
        std::string section;

        bool IsValid() const noexcept { return label.is_valid(); }
    };

    std::optional<SymbolInfo> GetSymbol(std::string_view name) const;

    std::optional<SymbolInfo> GetSymbol(asmjit::Label label) const;

    // Logging and debugging
    void EnableLogging(bool enable) noexcept { m_Options.enableLogging = enable; }
    bool IsLoggingEnabled() const noexcept { return m_Options.enableLogging; }

    void AddComment(std::string_view comment);

    /**
     * @brief Emit call to native function
     * @param target Function pointer
     * @param isTailCall Whether this is a tail call
     */
    void EmitCall(void* target, bool isTailCall = false);

    // Register allocation helpers
    class RegisterAllocator {
    public:
        explicit RegisterAllocator(CX64Ops& assembler);

        // 64-bit GP
        // 32-bit GP

        void Reset();

    private:
        [[maybe_unused]] CX64Ops& m_Assembler;
        std::array<bool, 16> m_GpUsed;  // General purpose registers
        std::array<bool, 16> m_XmmUsed; // XMM registers
    };

    RegisterAllocator& GetAllocator() noexcept { return m_Allocator; }

    // Error handling
    class AssemblerError : public std::runtime_error {
    public:
        AssemblerError(std::string_view message, asmjit::Error code) :
            std::runtime_error(std::string(message)), m_ErrorCode(code) {}

        asmjit::Error GetErrorCode() const noexcept { return m_ErrorCode; }

    private:
        asmjit::Error m_ErrorCode;
    };

    bool HasError() const noexcept { return m_LastError != asmjit::kErrorOk; }

    asmjit::Error GetLastError() const noexcept { return m_LastError; }

    void ClearError() noexcept { m_LastError = asmjit::kErrorOk; }

    // Performance and statistics
    struct Statistics {
        size_t instructionsEmitted = 0;
        size_t bytesEmitted = 0;
        size_t labelsCreated = 0;
        size_t jumpsEmitted = 0;
        size_t callsEmitted = 0;
        size_t memoryAccesses = 0;
        size_t errorsEncountered = 0;
        size_t optimizationsApplied = 0;

        void Reset() noexcept {
            instructionsEmitted = 0;
            bytesEmitted = 0;
            labelsCreated = 0;
            jumpsEmitted = 0;
            callsEmitted = 0;
            memoryAccesses = 0;
            errorsEncountered = 0;
            optimizationsApplied = 0;
        }
    };

    const Statistics& GetStatistics() const noexcept { return m_Stats; }
    void ResetStatistics() noexcept { m_Stats.Reset(); }

    // Direct access to AsmJit assembler (for advanced use)
    asmjit::x86::Assembler& GetAssembler() noexcept { return *this; }
    const asmjit::x86::Assembler& GetAssembler() const noexcept { return *this; }

private:
    // Logger interface
    asmjit::Error _log(const char* data, size_t size) noexcept override;

    // Internal helpers
    void InitializeSymbols();

    // Error handling
    void HandleError(asmjit::Error error, std::string_view context = "");

    // Symbol management
    using SymbolMap = std::unordered_map<std::string, SymbolInfo>;
    using LabelMap = std::unordered_map<uint32_t, SymbolInfo>; // Label id to symbol

    SymbolMap m_Symbols;
    LabelMap m_LabelSymbols;

    // State
    CodeBlock& m_CodeBlock;
    EmitOptions m_Options;
    EmitMode m_EmitMode = EmitMode::Fast;

    // Error state
    asmjit::Error m_LastError = asmjit::kErrorOk;
    std::string m_LastErrorContext;

    Statistics m_Stats;

    RegisterAllocator m_Allocator;

    // Logging
    std::ostream* m_LogOutput = nullptr;
    std::function<void(std::string_view)> m_LogCallback;

    asmjit::CodeHolder* m_CodeHolder = nullptr;

    // Friends for internal access
    friend class RegisterAllocator;
};

inline void CX64Ops::AddComment(std::string_view comment) {
    if (m_Options.emitDebugComments) {
        asmjit::x86::Assembler::comment(comment.data(), comment.size());
    }
}

inline std::optional<CX64Ops::SymbolInfo> CX64Ops::GetSymbol(std::string_view name) const {
    auto it = m_Symbols.find(std::string(name));
    if (it != m_Symbols.end()) {
        return it->second;
    }
    return std::nullopt;
}

inline std::optional<CX64Ops::SymbolInfo> CX64Ops::GetSymbol(asmjit::Label label) const {
    auto it = m_LabelSymbols.find(label.id());
    if (it != m_LabelSymbols.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
