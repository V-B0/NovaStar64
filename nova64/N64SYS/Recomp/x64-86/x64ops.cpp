/**
 * @file x64ops.cpp
 * @brief x64 assembler operations implementation
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Implementation of the CX64Ops class which wraps AsmJit for x64 code generation.
 * AsmJit v1.21 uses snake_case API (is_valid, new_label, set_logger, etc.)
 */

#if defined(__amd64__) || defined(_M_X64)

#include "x64ops.hpp"
#include "../CodeBlock.hpp"
#include "../../../common/log.hpp"

#include <sstream>
#include <cassert>

namespace Nova64 {
namespace Recompiler {

// --- CX64Ops::RegisterAllocator ---

CX64Ops::RegisterAllocator::RegisterAllocator(CX64Ops& assembler) : m_Assembler(assembler) {
    m_GpUsed.fill(false);
    m_XmmUsed.fill(false);
    // Reserve RSP (index 4) - stack pointer is not allocatable
    m_GpUsed[4] = true;
}

void CX64Ops::RegisterAllocator::Reset() {
    m_GpUsed.fill(false);
    m_XmmUsed.fill(false);
    m_GpUsed[4] = true; // RSP reserved
}

// --- CX64Ops Construction / Destruction ---

CX64Ops::CX64Ops(CodeBlock& codeBlock, EmitOptions options) :
    asmjit::x86::Assembler(), asmjit::Logger(), m_CodeBlock(codeBlock), m_Options(std::move(options)),
    m_Allocator(*this) {
    Log::debug("CX64Ops: Created");
}

CX64Ops::~CX64Ops() { Log::debug("CX64Ops: Destroyed"); }

// Move constructor - AsmJit Assembler/Logger don't support move, so we
// default-construct the base classes and transfer our own state.
CX64Ops::CX64Ops(CX64Ops&& other) noexcept :
    asmjit::x86::Assembler(), asmjit::Logger(), m_Symbols(std::move(other.m_Symbols)),
    m_LabelSymbols(std::move(other.m_LabelSymbols)), m_CodeBlock(other.m_CodeBlock),
    m_Options(std::move(other.m_Options)), m_EmitMode(other.m_EmitMode), m_LastError(other.m_LastError),
    m_LastErrorContext(std::move(other.m_LastErrorContext)), m_Stats(other.m_Stats), m_Allocator(*this),
    m_LogOutput(other.m_LogOutput), m_LogCallback(std::move(other.m_LogCallback)), m_CodeHolder(other.m_CodeHolder) {
    other.m_LogOutput = nullptr;
    other.m_CodeHolder = nullptr;
}

CX64Ops& CX64Ops::operator=(CX64Ops&& other) noexcept {
    if (this != &other) {
        m_Symbols = std::move(other.m_Symbols);
        m_LabelSymbols = std::move(other.m_LabelSymbols);
        m_Options = std::move(other.m_Options);
        m_EmitMode = other.m_EmitMode;
        m_LastError = other.m_LastError;
        m_LastErrorContext = std::move(other.m_LastErrorContext);
        m_Stats = other.m_Stats;
        m_LogOutput = other.m_LogOutput;
        m_LogCallback = std::move(other.m_LogCallback);
        m_CodeHolder = other.m_CodeHolder;
        other.m_LogOutput = nullptr;
        other.m_CodeHolder = nullptr;
    }
    return *this;
}

// --- Initialization ---

bool CX64Ops::Initialize(asmjit::CodeHolder& codeHolder) {
    m_CodeHolder = &codeHolder;
    asmjit::Error err = codeHolder.attach(static_cast<asmjit::x86::Assembler*>(this));
    if (err != asmjit::kErrorOk) {
        HandleError(err, "Initialize: attach failed");
        return false;
    }

    if (m_Options.enableLogging) {
        codeHolder.set_logger(static_cast<asmjit::Logger*>(this));
    }

    InitializeSymbols();
    return true;
}

void CX64Ops::InitializeSymbols() {
    m_Symbols.clear();
    m_LabelSymbols.clear();
}

// --- Symbol Management ---

// --- Logging ---

void CX64Ops::SetLogCallback(std::function<void(std::string_view)> callback) noexcept {
    m_LogCallback = std::move(callback);
}

asmjit::Error CX64Ops::_log(const char* data, size_t size) noexcept {
    if (size == SIZE_MAX) {
        size = std::strlen(data);
    }
    std::string_view sv(data, size);
    if (m_LogOutput) {
        *m_LogOutput << sv;
    }
    if (m_LogCallback) {
        m_LogCallback(sv);
    }
    return asmjit::kErrorOk;
}

// --- Code Generation Helpers ---

void CX64Ops::EmitCall(void* target, bool isTailCall) {
    using namespace asmjit::x86;

    if (isTailCall) {
        AddComment("  tail call");
        mov(rax, reinterpret_cast<uint64_t>(target));
        jmp(rax);
    } else {
        AddComment("  call");
        mov(rax, reinterpret_cast<uint64_t>(target));
        call(rax);
    }

    m_Stats.callsEmitted++;
    m_Stats.instructionsEmitted += 2;
}

// --- Error Handling ---

void CX64Ops::HandleError(asmjit::Error error, std::string_view context) {
    m_LastError = error;
    m_LastErrorContext = std::string(context);
    m_Stats.errorsEncountered++;
    Log::errorf("CX64Ops error [%s]: %s (code %d)", std::string(context).c_str(),
                asmjit::DebugUtils::error_as_string(error), static_cast<int>(error));
}

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
