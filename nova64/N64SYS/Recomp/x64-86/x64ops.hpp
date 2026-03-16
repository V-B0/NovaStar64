/**
 * @file x64ops.hpp
 * @brief x64 assembler operations wrapper and code generation utilities
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
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

// Forward declarations
class CodeBlock;

/**
 * @enum EmitMode
 * @brief Different modes for code emission
 */
enum class EmitMode : uint8_t {
    Fast,           ///< Fast code generation (minimal checks)
    Safe,           ///< Safe code generation (with validation)
    Debug,          ///< Debug code generation (with assertions and logging)
    Profile,        ///< Profiling code generation (with instrumentation)
    Optimized       ///< Optimized code generation (with peephole optimizations)
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
    uint32_t maxInstructionSize = 15;  // Maximum x64 instruction size
    
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
 * @struct GeneratedCodeInfo
 * @brief Information about generated code block
 */
struct GeneratedCodeInfo {
    uint8_t* code = nullptr;
    size_t size = 0;
    size_t maxSize = 0;
    uint32_t instructionCount = 0;
    uint32_t labelCount = 0;
    uint32_t jumpCount = 0;
    std::string errorMessage;
    
    bool IsValid() const noexcept { return code != nullptr && size > 0; }
    void Reset() noexcept {
        code = nullptr;
        size = 0;
        maxSize = 0;
        instructionCount = 0;
        labelCount = 0;
        jumpCount = 0;
        errorMessage.clear();
    }
};

/**
 * @class IX64CodeGenerator
 * @brief Interface for x64 code generation strategies
 */
class IX64CodeGenerator {
public:
    virtual ~IX64CodeGenerator() = default;
    
    virtual bool Initialize(asmjit::CodeHolder& codeHolder) = 0;
    virtual bool GenerateCode(const EmitOptions& options) = 0;
    virtual GeneratedCodeInfo Finalize() = 0;
    virtual void Reset() = 0;
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
    
    // Non-copyable, movable
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
    
    /**
     * @brief Set code generation mode
     */
    void SetEmitMode(EmitMode mode) noexcept { m_EmitMode = mode; }
    EmitMode GetEmitMode() const noexcept { return m_EmitMode; }
    
    /**
     * @brief Update emit options
     */
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
    
    /**
     * @brief Define a new symbol
     * @param name Symbol name
     * @param isExported Whether symbol is exported
     * @return Label for the symbol
     */
    asmjit::Label DefineSymbol(std::string_view name, bool isExported = false);
    
    /**
     * @brief Get symbol information by name
     */
    std::optional<SymbolInfo> GetSymbol(std::string_view name) const;
    
    /**
     * @brief Get symbol information by label
     */
    std::optional<SymbolInfo> GetSymbol(asmjit::Label label) const;
    
    /**
     * @brief Resolve symbol address (for linking)
     */
    bool ResolveSymbol(std::string_view name, uint64_t address);
    
    // Logging and debugging
    void EnableLogging(bool enable) noexcept { m_Options.enableLogging = enable; }
    bool IsLoggingEnabled() const noexcept { return m_Options.enableLogging; }
    
    void SetLogOutput(std::ostream& output) noexcept;
    void SetLogCallback(std::function<void(std::string_view)> callback) noexcept;
    
    /**
     * @brief Generate disassembly of current code
     */
    std::string GenerateDisassembly() const;
    
    /**
     * @brief Add a comment to the generated code
     */
    void AddComment(std::string_view comment);
    
    // Code generation helpers
    /**
     * @brief Emit prologue code
     * @param stackSize Stack frame size
     * @param preserveRegs Mask of registers to preserve
     */
    void EmitPrologue(uint32_t stackSize, uint32_t preserveRegs = 0);
    
    /**
     * @brief Emit epilogue code
     * @param stackSize Stack frame size
     * @param preserveRegs Mask of registers to restore
     */
    void EmitEpilogue(uint32_t stackSize, uint32_t restoreRegs = 0);
    
    /**
     * @brief Emit call to native function
     * @param target Function pointer
     * @param isTailCall Whether this is a tail call
     */
    void EmitCall(void* target, bool isTailCall = false);
    
    /**
     * @brief Emit conditional jump with fallback
     * @param condition AsmJit condition code
     * @param target Jump target label
     * @param fallback Fallback label (for likely branches)
     */
    void EmitConditionalJump(asmjit::x86::CondCode condition, 
                            asmjit::Label target, 
                            asmjit::Label fallback = asmjit::Label());
    
    /**
     * @brief Emit memory load with bounds checking
     * @param dest Destination register
     * @param src Memory operand
     * @param size Size of load (1, 2, 4, 8)
     * @param isSigned Whether load is signed
     */
    void EmitSafeLoad(asmjit::x86::Gp dest, const asmjit::x86::Mem& src, 
                     uint32_t size, bool isSigned);
    
    /**
     * @brief Emit memory store with bounds checking
     * @param dest Memory operand
     * @param src Source register
     * @param size Size of store (1, 2, 4, 8)
     */
    void EmitSafeStore(const asmjit::x86::Mem& dest, asmjit::x86::Gp src, 
                      uint32_t size);
    
    // Register allocation helpers
    class RegisterAllocator {
    public:
        explicit RegisterAllocator(CX64Ops& assembler);
        
        asmjit::x86::Gp AllocateGp();
        asmjit::x86::Vec AllocateXmm();
        asmjit::x86::Gp AllocateGp64();  // 64-bit GP
        asmjit::x86::Gp AllocateGp32();  // 32-bit GP
        
        void Free(asmjit::x86::Gp reg);
        void Free(asmjit::x86::Vec reg);
        
        bool IsAllocated(asmjit::x86::Gp reg) const;
        bool IsAllocated(asmjit::x86::Vec reg) const;
        
        void Reset();
        
    private:
        CX64Ops& m_Assembler;
        std::array<bool, 16> m_GpUsed;    // General purpose registers
        std::array<bool, 16> m_XmmUsed;   // XMM registers
    };
    
    RegisterAllocator& GetAllocator() noexcept { return m_Allocator; }
    
    // Error handling
    class AssemblerError : public std::runtime_error {
    public:
        AssemblerError(std::string_view message, asmjit::Error code)
            : std::runtime_error(std::string(message))
            , m_ErrorCode(code) {}
            
        asmjit::Error GetErrorCode() const noexcept { return m_ErrorCode; }
        
    private:
        asmjit::Error m_ErrorCode;
    };
    
    /**
     * @brief Check if assembler is in error state
     */
    bool HasError() const noexcept { return m_LastError != asmjit::kErrorOk; }
    
    /**
     * @brief Get last error
     */
    asmjit::Error GetLastError() const noexcept { return m_LastError; }
    
    /**
     * @brief Get last error message
     */
    std::string GetLastErrorMessage() const;
    
    /**
     * @brief Clear error state
     */
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
    bool ValidateInstruction(const asmjit::BaseInst& inst) const;
    void ApplyOptimizations();
    void EmitDebugTrap();
    
    // Error handling
    void HandleError(asmjit::Error error, std::string_view context = "");
    
    // Symbol management
    using SymbolMap = std::unordered_map<std::string, SymbolInfo>;
    using LabelMap = std::unordered_map<uint32_t, SymbolInfo>;  // Label id to symbol
    
    SymbolMap m_Symbols;
    LabelMap m_LabelSymbols;
    
    // State
    CodeBlock& m_CodeBlock;
    EmitOptions m_Options;
    EmitMode m_EmitMode = EmitMode::Fast;
    
    // Error state
    asmjit::Error m_LastError = asmjit::kErrorOk;
    std::string m_LastErrorContext;
    
    // Statistics
    Statistics m_Stats;
    
    // Register allocator
    RegisterAllocator m_Allocator;
    
    // Logging
    std::ostream* m_LogOutput = nullptr;
    std::function<void(std::string_view)> m_LogCallback;
    
    // Code holder reference
    asmjit::CodeHolder* m_CodeHolder = nullptr;
    
    // Friends for internal access
    friend class RegisterAllocator;
};

// Inline implementations for performance-critical methods

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