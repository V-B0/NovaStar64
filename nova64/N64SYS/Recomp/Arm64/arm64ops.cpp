/**
 * @file arm64ops.cpp
 * @brief AArch64 assembler operations wrapper - implementation
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "arm64ops.hpp"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "../../../common/log.hpp"
#include <cstring>
#include <sstream>

namespace Nova64 {
namespace Recompiler {

// --- Construction / Destruction ---

CArm64Ops::CArm64Ops(CodeBlock& codeBlock, Arm64EmitOptions options) :
    asmjit::a64::Assembler(), asmjit::Logger(), m_CodeBlock(codeBlock), m_Options(std::move(options)) {
    Log::debug("CArm64Ops: Created");
}

CArm64Ops::~CArm64Ops() { Log::debug("CArm64Ops: Destroyed"); }

// --- Initialization ---

bool CArm64Ops::Initialize(asmjit::CodeHolder& codeHolder) {
    m_CodeHolder = &codeHolder;
    asmjit::Error err = codeHolder.attach(static_cast<asmjit::a64::Assembler*>(this));
    if (err != asmjit::kErrorOk) {
        HandleError(err, "Initialize: attach failed");
        return false;
    }

    if (m_Options.enableLogging) {
        codeHolder.set_logger(static_cast<asmjit::Logger*>(this));
    }

    m_Symbols.clear();
    return true;
}

// --- Symbol Management ---

asmjit::Label CArm64Ops::DefineSymbol(std::string_view name, bool isExported) {
    auto label = asmjit::a64::Assembler::new_label();
    if (!label.is_valid()) {
        HandleError(asmjit::Error::kOutOfMemory, "DefineSymbol");
        return label;
    }

    SymbolInfo info;
    info.name = std::string(name);
    info.label = label;
    info.isExported = isExported;

    m_Symbols[info.name] = info;
    return label;
}

// --- Logging ---

asmjit::Error CArm64Ops::_log(const char* data, size_t size) noexcept {
    if (size == SIZE_MAX) {
        size = std::strlen(data);
    }
    std::string_view sv(data, size);
    if (m_LogCallback) {
        m_LogCallback(sv);
    }
    return asmjit::kErrorOk;
}

// --- Code Generation Helpers ---

void CArm64Ops::MovImm64(const asmjit::a64::Gp& dst, uint64_t value) {
    // AsmJit's a64 Assembler synthesises the optimal MOVZ/MOVK (or MOV-bitmask)
    // sequence for an arbitrary 64-bit immediate.
    mov(dst, asmjit::Imm(value));
}

void CArm64Ops::EmitCall(void* target, bool isTailCall) {
    AddComment(isTailCall ? "  tail call" : "  call");

    // x16 (IP0) is a caller-saved intra-procedure-call scratch register; the
    // AAPCS64 ABI explicitly allows it to be clobbered across calls, so it is
    // the canonical place to stage an indirect branch target.
    const asmjit::a64::Gp x16 = asmjit::a64::x16;
    MovImm64(x16, reinterpret_cast<uint64_t>(target));

    if (isTailCall) {
        br(x16);
    } else {
        blr(x16);
    }
}

// --- Error Handling ---

void CArm64Ops::HandleError(asmjit::Error error, std::string_view context) {
    m_LastError = error;
    m_LastErrorContext = std::string(context);
    Log::errorf("CArm64Ops error %u: %.*s", static_cast<unsigned>(error), static_cast<int>(context.size()),
                context.data());
}

} // namespace Recompiler
} // namespace Nova64

#endif // __aarch64__ || _M_ARM64
