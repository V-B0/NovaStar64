/**
 * @file asmjit.hpp
 * @brief AsmJit library wrapper for dynamic code generation
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 * 
 * This file provides a centralized wrapper around the AsmJit library with:
 * - Safe inclusion handling (protects against macro conflicts)
 * - Platform-specific architecture detection
 * - Type aliases for common AsmJit types
 * - Error handling helpers
 * - Modern C++ improvements over Project64's implementation
 * 
 * AsmJit is used for dynamic recompilation (dynarec) of MIPS instructions
 * to native x86/x64 or ARM code for improved emulation performance.
 * 
 * Original Project64 code licensed under GPL-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <optional>
#include <functional>
#include "../../common/log.hpp"

// Check if AsmJit is available (defined by CMake when asmjit package is found)
#ifdef NOVA64_ASMJIT_ENABLED

// AsmJit is available - use real implementation
// #define ASMJIT_STATIC // Defined by CMake

// Protect against 'new' macro conflicts (common in debug builds)
#ifdef new
#pragma push_macro("new")
#undef new
#define ASMJIT_NEW_MACRO_PROTECTED
#include <asmjit/core.h>
#pragma pop_macro("new")
#else
#include <asmjit/core.h>
#endif

// Include platform-specific assembler headers
#if defined(__amd64__) || defined(_M_X64) || defined(__x86_64__)
    #ifndef NOVA64_ARCH_X64
    #define NOVA64_ARCH_X64
    #endif
    #include <asmjit/x86.h>
    namespace Nova64Asm = asmjit::x86;
#elif defined(__i386__) || defined(_M_IX86)
    #ifndef NOVA64_ARCH_X86
    #define NOVA64_ARCH_X86
    #endif
    #include <asmjit/x86.h>
    namespace Nova64Asm = asmjit::x86;
#elif defined(__aarch64__) || defined(_M_ARM64)
    #ifndef NOVA64_ARCH_ARM64
    #define NOVA64_ARCH_ARM64
    #endif
    #include <asmjit/a64.h>
    namespace Nova64Asm = asmjit::a64;
#elif defined(__arm__) || defined(_M_ARM)
    #ifndef NOVA64_ARCH_ARM32
    #define NOVA64_ARCH_ARM32
    #endif
    #include <asmjit/arm.h>
    namespace Nova64Asm = asmjit::arm;
#else
    #error "Unsupported architecture for recompiler"
#endif

// Custom hasher for map keys (Real AsmJit)
struct AsmJitLabelHash {
    std::size_t operator()(const asmjit::Label& k) const noexcept {
        return std::hash<uint32_t>{}(k.id());
    }
};

// Also specialize std::hash just in case
namespace std {
    template<>
    struct hash<asmjit::Label> {
        std::size_t operator()(const asmjit::Label& k) const noexcept {
            return std::hash<uint32_t>{}(k.id());
        }
    };
}

#else
// AsmJit not available - use stub declarations

#define NOVA64_ASMJIT_STUB 1

// Detect target architecture
#if defined(__amd64__) || defined(_M_X64) || defined(__x86_64__)
    #ifndef NOVA64_ARCH_X64
    #define NOVA64_ARCH_X64
    #endif
#elif defined(__i386__) || defined(_M_IX86)
    #ifndef NOVA64_ARCH_X86
    #define NOVA64_ARCH_X86
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #ifndef NOVA64_ARCH_ARM64
    #define NOVA64_ARCH_ARM64
    #endif
#elif defined(__arm__) || defined(_M_ARM)
    #ifndef NOVA64_ARCH_ARM32
    #define NOVA64_ARCH_ARM32
    #endif
#else
    #error "Unsupported architecture for recompiler"
#endif

    class ErrorHandler;
    class BaseEmitter; 

    // Forward declarations for AsmJit types (stubs until library is integrated)
    namespace asmjit {
        enum Error : uint32_t { kErrorOk = 0 };

        class Environment {
        public: Environment() = default; static Environment host() { return Environment(); } };

        class BaseEmitter;
        class ErrorHandler {
        public: virtual ~ErrorHandler() = default; virtual void handleError(Error, const char*, BaseEmitter*) = 0; };

        class Section {
        public:
            size_t bufferSize() const { return 0; }
        };

        class JitRuntime {
        public:
            JitRuntime() = default;
            virtual ~JitRuntime() = default;
            Environment environment() const { return Environment(); }
            void release(void*) {}
            Error add(void** dst, CodeHolder*) { *dst = nullptr; return kErrorOk; }
        };

        class CodeHolder {
        public:
            CodeHolder() = default;
            virtual ~CodeHolder() = default;
            void init(const Environment&) {}
            Error attach(BaseEmitter*) { return kErrorOk; }
            void detach(BaseEmitter*) {}
            size_t codeSize() const { return 0; }
            void setErrorHandler(ErrorHandler*) {}
            Section* textSection() { return &m_textSection; }
        private:
            Section m_textSection;
        };

        class Logger { public: virtual ~Logger() = default; };
        class FormatOptions {};
        class Operand {};
        class Label : public Operand { public: Label() = default; bool isValid() const { return true; } uint32_t id() const { return 0; } };
    class Imm : public Operand { public: Imm(int64_t) {} };
    class BaseEmitter { public: virtual ~BaseEmitter() = default; };

    namespace x86 {
        // Stub register instances
        struct Gp : public Operand {};
        struct Gpd : public Gp {};
        struct Gpq : public Gp {};
        inline Gpq rax{}, rbx{}, rcx{}, rdx{}, rsi{}, rdi{}, rbp{}, rsp{};
        inline Gpq r8{}, r9{}, r10{}, r11{}, r12{}, r13{}, r14{}, r15{};
        inline Gpd eax{}, ebx{}, ecx{}, edx{}, esi{}, edi{}, ebp{}, esp{};

        class Assembler : public BaseEmitter { 
        public: 
            Assembler(CodeHolder*) {} 
            void ret() {}
            void push(const Operand&) {}
            void pop(const Operand&) {}
            void mov(const Operand&, const Operand&) {}
            void add(const Operand&, const Operand&) {}
            void sub(const Operand&, const Operand&) {}
            void xor_(const Operand&, const Operand&) {}
            void and_(const Operand&, const Operand&) {}
            void or_(const Operand&, const Operand&) {}
            void cmp(const Operand&, const Operand&) {}
            void test(const Operand&, const Operand&) {}
            void jmp(const Label&) {}
            void je(const Label&) {}
            void jne(const Label&) {}
            void jz(const Label&) {}
            void jnz(const Label&) {}
            void call(const Operand&) {}
            void nop() {}
            Label newLabel() { return Label(); }
            void bind(const Label&) {}
        };
        class Compiler {}; class Builder {};
        class Xmm : public Operand {};
        class Ymm : public Operand {};
        class Mm : public Operand {};
        class St : public Operand {};
        class Mem : public Operand {};
        enum class Condition : uint32_t {};
    }

    namespace a64 {
        class Assembler : public BaseEmitter { public: Assembler(CodeHolder*) {} };
        class Compiler {}; class Builder {};
        class Gp : public Operand {};
        class Vec : public Operand {};
        class Mem : public Operand {};
        enum class Condition : uint32_t {};
    }

    namespace arm {
        class Assembler : public BaseEmitter { public: Assembler(CodeHolder*) {} };
        class Compiler {}; class Gp : public Operand {};
        class Vec : public Operand {}; class Mem : public Operand {};
    }

    namespace DebugUtils {
        inline const char* errorAsString(Error) { return "AsmJit stub"; }
    }
}

// std::hash specialization for asmjit::Label (needed for unordered_map keys)
namespace std {
    template<>
    struct hash<asmjit::Label> {
        size_t operator()(const asmjit::Label& /*label*/) const noexcept {
            return 0; // Stub implementation
        }
    };
    template<>
    struct equal_to<asmjit::Label> {
        bool operator()(const asmjit::Label& /*a*/, const asmjit::Label& /*b*/) const noexcept {
            return true; // Stub implementation
        }
    };
}

// Namespace alias for platform-specific types
#if defined(NOVA64_ARCH_X64) || defined(NOVA64_ARCH_X86)
    namespace Nova64Asm = asmjit::x86;
#elif defined(NOVA64_ARCH_ARM64)
    namespace Nova64Asm = asmjit::a64;
#elif defined(NOVA64_ARCH_ARM32)
    namespace Nova64Asm = asmjit::arm;
#endif

// Custom hasher for map keys
struct AsmJitLabelHash {
    std::size_t operator()(const asmjit::Label& k) const noexcept {
        return std::hash<uint32_t>{}(k.id());
    }
};

// Also specialize std::hash just in case
namespace std {
    template<>
    struct hash<asmjit::Label> {
        std::size_t operator()(const asmjit::Label& k) const noexcept {
            return std::hash<uint32_t>{}(k.id());
        }
    };
}

#endif // NOVA64_ASMJIT_ENABLED

// =============================================================================
// Type Aliases - Simplify common AsmJit types (work with both real and stub)
// =============================================================================

namespace Nova64 {
namespace Recomp {

// Core AsmJit types
using JitRuntime = asmjit::JitRuntime;
using CodeHolder = asmjit::CodeHolder;
using Environment = asmjit::Environment;
using ErrorHandler = asmjit::ErrorHandler;
using Logger = asmjit::Logger;
using FormatOptions = asmjit::FormatOptions;

// Code generation types
using Label = asmjit::Label;
using Operand = asmjit::Operand;
using Imm = asmjit::Imm;

// Error handling
using Error = asmjit::Error;

// Platform-specific assembler types
#if defined(NOVA64_ARCH_X64) || defined(NOVA64_ARCH_X86)
    using Assembler = asmjit::x86::Assembler;
    using Compiler = asmjit::x86::Compiler;
    using Builder = asmjit::x86::Builder;
    using Gp = asmjit::x86::Gp;       // General purpose register
    using Gpd = asmjit::x86::Gp;      // 32-bit GP register (aliased to Gp)
    using Gpq = asmjit::x86::Gp;      // 64-bit GP register (aliased to Gp)
    using Vec = asmjit::x86::Vec;      // Unified vector register (XMM/YMM/ZMM)
    using Xmm = asmjit::x86::Vec;     // SSE register (alias for Vec)
    using Ymm = asmjit::x86::Vec;     // AVX register (alias for Vec)
    using Mm = asmjit::x86::Mm;       // MMX register
    using St = asmjit::x86::St;       // x87 FPU register
    using Mem = asmjit::x86::Mem;     // Memory operand
    using CondCode = asmjit::x86::CondCode;  // Condition code enum
    using Condition = asmjit::x86::CondCode;  // Legacy alias
#elif defined(NOVA64_ARCH_ARM64)
    using Assembler = asmjit::a64::Assembler;
    using Compiler = asmjit::a64::Compiler;
    using Builder = asmjit::a64::Builder;
    using Gp = asmjit::a64::Gp;       // General purpose register
    using Vec = asmjit::a64::Vec;     // Vector register
    using Mem = asmjit::a64::Mem;     // Memory operand
    using Condition = asmjit::a64::Condition;
#elif defined(NOVA64_ARCH_ARM32)
    using Assembler = asmjit::arm::Assembler;
    using Compiler = asmjit::arm::Compiler;
    using Gp = asmjit::arm::Gp;
    using Vec = asmjit::arm::Vec;
    using Mem = asmjit::arm::Mem;
#endif

// =============================================================================
// Error Handling Helper
// =============================================================================

/**
 * @class AsmJitErrorHandler
 * @brief Custom error handler for AsmJit operations
 * 
 * Captures and logs AsmJit errors with detailed information.
 * Provides better diagnostics than Project64's basic error handling.
 */
class AsmJitErrorHandler {
public:
    AsmJitErrorHandler() = default;
    virtual ~AsmJitErrorHandler() = default;

    // ErrorHandler interface (virtual for future AsmJit integration)
    virtual void handleError(asmjit::Error err, const char* message, asmjit::BaseEmitter* /*origin*/) {
        m_lastError = err;
        m_lastMessage = message ? message : "Unknown AsmJit error";
        
        Log::errorf("AsmJit Error [%u]: %s", 
                   static_cast<uint32_t>(err), 
                   m_lastMessage.c_str());
    }

    // Get last error information
    [[nodiscard]] asmjit::Error getLastError() const noexcept { return m_lastError; }
    [[nodiscard]] const std::string& getLastMessage() const noexcept { return m_lastMessage; }
    
    // Check if an error occurred
    [[nodiscard]] bool hasError() const noexcept { return m_lastError != asmjit::kErrorOk; }
    
    // Clear error state
    void clearError() noexcept {
        m_lastError = asmjit::kErrorOk;
        m_lastMessage.clear();
    }

private:
    asmjit::Error m_lastError = asmjit::kErrorOk;
    std::string m_lastMessage;
};

// =============================================================================
// Code Buffer Manager
// =============================================================================

/**
 * @class CodeBufferInfo
 * @brief Information about generated code buffer
 * 
 * Tracks statistics and metadata for recompiled code blocks.
 */
struct CodeBufferInfo {
    void* baseAddress = nullptr;
    size_t codeSize = 0;
    size_t capacity = 0;
    uint32_t instructionCount = 0;
    
    [[nodiscard]] bool isValid() const noexcept { 
        return baseAddress != nullptr && codeSize > 0; 
    }
    
    [[nodiscard]] void* getEndAddress() const noexcept {
        return static_cast<uint8_t*>(baseAddress) + codeSize;
    }
    
    [[nodiscard]] size_t getUsedPercentage() const noexcept {
        return capacity > 0 ? (codeSize * 100) / capacity : 0;
    }
};

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Get human-readable error message for AsmJit error code
 */
[[nodiscard]] inline const char* GetErrorString(asmjit::Error /*err*/) noexcept {
    return "AsmJit Error (details unavailable)"; 
}

/**
 * @brief Log AsmJit error with context
 */
inline void LogAsmJitError(asmjit::Error err, const char* context = nullptr) {
    if (err != asmjit::kErrorOk) {
        if (context) {
            Log::errorf("AsmJit Error in %s: %s", context, GetErrorString(err));
        } else {
            Log::errorf("AsmJit Error: %s", GetErrorString(err));
        }
    }
}

/**
 * @brief Check if AsmJit error occurred and log it
 * @return true if error occurred, false otherwise
 */
[[nodiscard]] inline bool CheckAsmJitError(asmjit::Error err, const char* context = nullptr) {
    if (err != asmjit::kErrorOk) {
        LogAsmJitError(err, context);
        return true;
    }
    return false;
}

// =============================================================================
// Architecture Information
// =============================================================================

/**
 * @brief Get current target architecture as string
 */
[[nodiscard]] inline constexpr const char* GetTargetArchName() noexcept {
#if defined(NOVA64_ARCH_X64)
    return "x86-64";
#elif defined(NOVA64_ARCH_X86)
    return "x86";
#elif defined(NOVA64_ARCH_ARM64)
    return "ARM64";
#elif defined(NOVA64_ARCH_ARM32)
    return "ARM32";
#else
    return "Unknown";
#endif
}

/**
 * @brief Check if running on 64-bit architecture
 */
[[nodiscard]] inline constexpr bool Is64BitArch() noexcept {
#if defined(NOVA64_ARCH_X64) || defined(NOVA64_ARCH_ARM64)
    return true;
#else
    return false;
#endif
}

/**
 * @brief Get pointer size for current architecture
 */
[[nodiscard]] inline constexpr size_t GetPointerSize() noexcept {
    return Is64BitArch() ? 8 : 4;
}

} // namespace Recomp
} // namespace Nova64

// =============================================================================
// Convenience Macros
// =============================================================================

// Error checking macros
#define ASMJIT_CHECK(expr) \
    do { \
        asmjit::Error _err = (expr); \
        if (_err != asmjit::kErrorOk) { \
            Nova64::Recomp::LogAsmJitError(_err, #expr); \
        } \
    } while (0)

#define ASMJIT_CHECK_RETURN(expr) \
    do { \
        asmjit::Error _err = (expr); \
        if (_err != asmjit::kErrorOk) { \
            Nova64::Recomp::LogAsmJitError(_err, #expr); \
            return false; \
        } \
    } while (0)

#define ASMJIT_CHECK_RETURN_NULL(expr) \
    do { \
        asmjit::Error _err = (expr); \
        if (_err != asmjit::kErrorOk) { \
            Nova64::Recomp::LogAsmJitError(_err, #expr); \
            return nullptr; \
        } \
    } while (0)
