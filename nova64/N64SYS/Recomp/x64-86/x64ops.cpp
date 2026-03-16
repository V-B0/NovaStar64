/**
 * @file x64ops.cpp
 * @brief x64 assembler operations implementation
 *
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Original Project64 code licensed under GPL-2.0
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

// =============================================================================
// CX64Ops::RegisterAllocator
// =============================================================================

CX64Ops::RegisterAllocator::RegisterAllocator(CX64Ops& assembler)
    : m_Assembler(assembler)
{
    m_GpUsed.fill(false);
    m_XmmUsed.fill(false);
    // Reserve RSP (index 4) - stack pointer is not allocatable
    m_GpUsed[4] = true;
}

asmjit::x86::Gp CX64Ops::RegisterAllocator::AllocateGp() {
    return AllocateGp64();
}

asmjit::x86::Gp CX64Ops::RegisterAllocator::AllocateGp64() {
    // Try volatile registers first (caller-saved)
    static const uint8_t volatileRegs[] = { 0/*rax*/, 1/*rcx*/, 2/*rdx*/, 8, 9, 10, 11 };
    for (uint8_t idx : volatileRegs) {
        if (!m_GpUsed[idx]) {
            m_GpUsed[idx] = true;
            return asmjit::x86::gpq(idx);
        }
    }
    // Try non-volatile registers
    static const uint8_t nonVolatileRegs[] = { 3/*rbx*/, 5/*rbp*/, 6/*rsi*/, 7/*rdi*/, 12, 13, 14, 15 };
    for (uint8_t idx : nonVolatileRegs) {
        if (!m_GpUsed[idx]) {
            m_GpUsed[idx] = true;
            return asmjit::x86::gpq(idx);
        }
    }
    return asmjit::x86::Gp(); // Invalid
}

asmjit::x86::Gp CX64Ops::RegisterAllocator::AllocateGp32() {
    auto gp64 = AllocateGp64();
    if (gp64.is_valid()) {
        return asmjit::x86::gpd(gp64.id());
    }
    return asmjit::x86::Gp();
}

asmjit::x86::Vec CX64Ops::RegisterAllocator::AllocateXmm() {
    for (uint8_t i = 0; i < 16; ++i) {
        if (!m_XmmUsed[i]) {
            m_XmmUsed[i] = true;
            return asmjit::x86::xmm(i);
        }
    }
    return asmjit::x86::Vec();
}

void CX64Ops::RegisterAllocator::Free(asmjit::x86::Gp reg) {
    if (reg.is_valid() && reg.id() < 16) {
        m_GpUsed[reg.id()] = false;
    }
}

void CX64Ops::RegisterAllocator::Free(asmjit::x86::Vec reg) {
    if (reg.is_valid() && reg.id() < 16) {
        m_XmmUsed[reg.id()] = false;
    }
}

bool CX64Ops::RegisterAllocator::IsAllocated(asmjit::x86::Gp reg) const {
    return reg.is_valid() && reg.id() < 16 && m_GpUsed[reg.id()];
}

bool CX64Ops::RegisterAllocator::IsAllocated(asmjit::x86::Vec reg) const {
    return reg.is_valid() && reg.id() < 16 && m_XmmUsed[reg.id()];
}

void CX64Ops::RegisterAllocator::Reset() {
    m_GpUsed.fill(false);
    m_XmmUsed.fill(false);
    m_GpUsed[4] = true; // RSP reserved
}

// =============================================================================
// CX64Ops Construction / Destruction
// =============================================================================

CX64Ops::CX64Ops(CodeBlock& codeBlock, EmitOptions options)
    : asmjit::x86::Assembler()
    , asmjit::Logger()
    , m_CodeBlock(codeBlock)
    , m_Options(std::move(options))
    , m_Allocator(*this)
{
    Log::debug("CX64Ops: Created");
}

CX64Ops::~CX64Ops()
{
    Log::debug("CX64Ops: Destroyed");
}

// Move constructor - AsmJit Assembler/Logger don't support move, so we
// default-construct the base classes and transfer our own state.
CX64Ops::CX64Ops(CX64Ops&& other) noexcept
    : asmjit::x86::Assembler()
    , asmjit::Logger()
    , m_Symbols(std::move(other.m_Symbols))
    , m_LabelSymbols(std::move(other.m_LabelSymbols))
    , m_CodeBlock(other.m_CodeBlock)
    , m_Options(std::move(other.m_Options))
    , m_EmitMode(other.m_EmitMode)
    , m_LastError(other.m_LastError)
    , m_LastErrorContext(std::move(other.m_LastErrorContext))
    , m_Stats(other.m_Stats)
    , m_Allocator(*this)
    , m_LogOutput(other.m_LogOutput)
    , m_LogCallback(std::move(other.m_LogCallback))
    , m_CodeHolder(other.m_CodeHolder)
{
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

// =============================================================================
// Initialization
// =============================================================================

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

// =============================================================================
// Symbol Management
// =============================================================================

asmjit::Label CX64Ops::DefineSymbol(std::string_view name, bool isExported) {
    auto label = asmjit::x86::Assembler::new_label();
    if (!label.is_valid()) {
        HandleError(asmjit::Error::kOutOfMemory, "DefineSymbol");
        return label;
    }

    SymbolInfo info;
    info.name = std::string(name);
    info.label = label;
    info.isExported = isExported;

    m_Symbols[info.name] = info;
    m_LabelSymbols[label.id()] = info;
    m_Stats.labelsCreated++;

    return label;
}

bool CX64Ops::ResolveSymbol(std::string_view name, uint64_t address) {
    auto it = m_Symbols.find(std::string(name));
    if (it == m_Symbols.end()) return false;
    it->second.address = address;
    m_LabelSymbols[it->second.label.id()].address = address;
    return true;
}

// =============================================================================
// Logging
// =============================================================================

void CX64Ops::SetLogOutput(std::ostream& output) noexcept {
    m_LogOutput = &output;
}

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

std::string CX64Ops::GenerateDisassembly() const {
    // Return a summary of generated code statistics
    std::ostringstream oss;
    oss << "; x64 code block - " << m_Stats.instructionsEmitted << " instructions, "
        << m_Stats.bytesEmitted << " bytes\n";
    oss << "; Labels: " << m_Stats.labelsCreated
        << " Jumps: " << m_Stats.jumpsEmitted
        << " Calls: " << m_Stats.callsEmitted << "\n";
    return oss.str();
}

// =============================================================================
// Code Generation Helpers
// =============================================================================

void CX64Ops::EmitPrologue(uint32_t stackSize, uint32_t preserveRegs) {
    using namespace asmjit::x86;

    AddComment("--- prologue ---");
    push(rbp);
    mov(rbp, rsp);

    // Preserve callee-saved registers
    static const Gp calleeSaved[] = { rbx, r12, r13, r14, r15, rsi, rdi };
    for (size_t i = 0; i < 7; ++i) {
        if (preserveRegs & (1u << i)) {
            push(calleeSaved[i]);
        }
    }

    // Allocate stack frame (aligned to 16 bytes)
    if (stackSize > 0) {
        uint32_t aligned = (stackSize + 15u) & ~15u;
        sub(rsp, aligned);
    }

    m_Stats.instructionsEmitted += 4;
}

void CX64Ops::EmitEpilogue(uint32_t stackSize, uint32_t restoreRegs) {
    using namespace asmjit::x86;

    AddComment("--- epilogue ---");

    if (stackSize > 0) {
        uint32_t aligned = (stackSize + 15u) & ~15u;
        add(rsp, aligned);
    }

    // Restore callee-saved registers (reverse order)
    static const Gp calleeSaved[] = { rdi, rsi, r15, r14, r13, r12, rbx };
    for (size_t i = 0; i < 7; ++i) {
        if (restoreRegs & (1u << (6 - i))) {
            pop(calleeSaved[i]);
        }
    }

    pop(rbp);
    ret();

    m_Stats.instructionsEmitted += 4;
}

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

void CX64Ops::EmitConditionalJump(asmjit::x86::CondCode condition,
                                  asmjit::Label target,
                                  asmjit::Label fallback) {
    using namespace asmjit::x86;

    // Map AsmJit CondCode to actual jcc instructions
    // Note: kEqual==kZero and kNotEqual==kNotZero are aliases, only list unique values
    switch (condition) {
    case CondCode::kEqual:        je(target);  break;
    case CondCode::kNotEqual:     jne(target); break;
    case CondCode::kSign:         js(target);  break;
    case CondCode::kNotSign:      jns(target); break;
    case CondCode::kSignedLT:     jl(target);  break;
    case CondCode::kSignedLE:     jle(target); break;
    case CondCode::kSignedGT:     jg(target);  break;
    case CondCode::kSignedGE:     jge(target); break;
    case CondCode::kUnsignedLT:   jb(target);  break;
    case CondCode::kUnsignedLE:   jbe(target); break;
    case CondCode::kUnsignedGT:   ja(target);  break;
    case CondCode::kUnsignedGE:   jae(target); break;
    case CondCode::kOverflow:     jo(target);  break;
    case CondCode::kNotOverflow:  jno(target); break;
    default:
        // For unsupported condition codes, do unconditional jump
        jmp(target);
        break;
    }

    // If fallback is valid, bind it here (for branch-likely paths)
    if (fallback.is_valid()) {
        bind(fallback);
    }

    m_Stats.jumpsEmitted++;
    m_Stats.instructionsEmitted++;
}

void CX64Ops::EmitSafeLoad(asmjit::x86::Gp dest, const asmjit::x86::Mem& src,
                            uint32_t size, bool isSigned) {
    using namespace asmjit::x86;

    AddComment("  safe load");

    // Use the Mem operand directly - it already has the correct size info
    // We need to clone it and set the proper size for the instruction
    Mem memOp = src;

    switch (size) {
    case 1:
        memOp.set_size(1);
        if (isSigned) {
            movsx(dest.r64(), memOp);
        } else {
            movzx(dest.r32(), memOp);
        }
        break;
    case 2:
        memOp.set_size(2);
        if (isSigned) {
            movsx(dest.r64(), memOp);
        } else {
            movzx(dest.r32(), memOp);
        }
        break;
    case 4:
        memOp.set_size(4);
        if (isSigned) {
            movsxd(dest.r64(), memOp);
        } else {
            mov(dest.r32(), memOp);
        }
        break;
    case 8:
        memOp.set_size(8);
        mov(dest.r64(), memOp);
        break;
    default:
        HandleError(asmjit::Error::kInvalidArgument, "EmitSafeLoad: invalid size");
        return;
    }

    m_Stats.memoryAccesses++;
    m_Stats.instructionsEmitted++;
}

void CX64Ops::EmitSafeStore(const asmjit::x86::Mem& dest, asmjit::x86::Gp src,
                             uint32_t size) {
    using namespace asmjit::x86;

    AddComment("  safe store");

    Mem memOp = dest;

    switch (size) {
    case 1:
        memOp.set_size(1);
        mov(memOp, src.r8());
        break;
    case 2:
        memOp.set_size(2);
        mov(memOp, src.r16());
        break;
    case 4:
        memOp.set_size(4);
        mov(memOp, src.r32());
        break;
    case 8:
        memOp.set_size(8);
        mov(memOp, src.r64());
        break;
    default:
        HandleError(asmjit::Error::kInvalidArgument, "EmitSafeStore: invalid size");
        return;
    }

    m_Stats.memoryAccesses++;
    m_Stats.instructionsEmitted++;
}

// =============================================================================
// Error Handling
// =============================================================================

void CX64Ops::HandleError(asmjit::Error error, std::string_view context) {
    m_LastError = error;
    m_LastErrorContext = std::string(context);
    m_Stats.errorsEncountered++;
    Log::errorf("CX64Ops error [%s]: %s (code %d)",
                std::string(context).c_str(),
                asmjit::DebugUtils::error_as_string(error),
                static_cast<int>(error));
}

std::string CX64Ops::GetLastErrorMessage() const {
    std::ostringstream oss;
    if (m_LastError != asmjit::kErrorOk) {
        oss << asmjit::DebugUtils::error_as_string(m_LastError);
        if (!m_LastErrorContext.empty()) {
            oss << " (" << m_LastErrorContext << ")";
        }
    }
    return oss.str();
}

bool CX64Ops::ValidateInstruction(const asmjit::BaseInst& /*inst*/) const {
    return m_LastError == asmjit::kErrorOk;
}

void CX64Ops::ApplyOptimizations() {
    // Placeholder for peephole optimizations
}

void CX64Ops::EmitDebugTrap() {
    asmjit::x86::Assembler::int3();
    m_Stats.instructionsEmitted++;
}

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
