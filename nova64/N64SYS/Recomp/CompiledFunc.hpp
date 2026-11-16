/**
 * @file CompiledFunc.hpp
 * @brief Compiled function wrapper for the dynamic recompiler
 *
 * Nova64 N64 Emulator - Based on Project64 architecture
 *
 * CCompiledFunc wraps a native code pointer produced by CodeBlock compilation
 * together with metadata needed for the execution loop (address range, hash,
 * memory snapshot for self-modifying-code detection).
 */

#pragma once

#include "CodeBlock.hpp"
#include "../../common/log.hpp"
#include <cstdint>
#include <memory>
#include <chrono>

namespace Nova64 {
namespace Recompiler {

/**
 * @brief Native function signature for compiled blocks.
 *
 * The recompiler generates a tiny thunk function that, when called,
 * runs the compiled instruction stream.  The function:
 *   - Takes no arguments (register context is baked in at JIT time via
 *     absolute addresses embedded in the code).
 *   - Returns void.
 *
 * On block exit the compiled code writes the updated PC back to
 * CRegisters::m_PROGRAM_COUNTER so the execution loop can dispatch
 * the next block.
 */
using BlockFunction = void (*)();

/**
 * @class CCompiledFunc
 * @brief Represents a single compiled MIPS code block
 *
 * Instances are created by CRecompiler::CompileCode() and stored in
 * CFunctionMap for O(1) lookup by virtual address.
 */
class CCompiledFunc {
public:
    CCompiledFunc() = default;

    CCompiledFunc(uint32_t enterPC, uint32_t minPC, uint32_t maxPC, BlockFunction func,
                  std::unique_ptr<CodeBlock> block) :
        m_EnterPC(enterPC), m_MinPC(minPC), m_MaxPC(maxPC), m_Function(func), m_CodeBlock(std::move(block)) {}

    ~CCompiledFunc() = default;

    // Move-only (CodeBlock owns AsmJit runtime data)
    CCompiledFunc(CCompiledFunc&&) noexcept = default;
    CCompiledFunc& operator=(CCompiledFunc&&) noexcept = default;
    CCompiledFunc(const CCompiledFunc&) = delete;
    CCompiledFunc& operator=(const CCompiledFunc&) = delete;

    // --- Execution ---

    void Execute() const {
        if (m_Function) {
            m_Function();
        } else {
            Log::warningf("CCompiledFunc::Execute: null function pointer at PC 0x%08X", m_EnterPC);
        }
    }

    /** Get the raw function pointer (for profiling / debugging). */
    BlockFunction Function() const noexcept { return m_Function; }

    // --- Address info ---

    uint32_t EnterPC() const noexcept { return m_EnterPC; }
    uint32_t MinPC() const noexcept { return m_MinPC; }
    uint32_t MaxPC() const noexcept { return m_MaxPC; }

    // --- Self-modifying-code detection ---

    /** @return true if the original MIPS memory still matches. */
    bool MemoryValid() const { return m_CodeBlock ? m_CodeBlock->verify_memory_snapshot() : true; }

    // --- Linked list for FunctionMap hash chains ---

    CCompiledFunc* Next() const noexcept { return m_Next; }
    void SetNext(CCompiledFunc* next) noexcept { m_Next = next; }

    // --- Underlying CodeBlock (for debugging / stats) ---

    CodeBlock* GetCodeBlock() const noexcept { return m_CodeBlock.get(); }

private:
    uint32_t m_EnterPC = 0;
    uint32_t m_MinPC = 0;
    uint32_t m_MaxPC = 0;
    BlockFunction m_Function = nullptr;
    std::unique_ptr<CodeBlock> m_CodeBlock;
    CCompiledFunc* m_Next = nullptr; // intrusive list for hash chains
};

} // namespace Recompiler
} // namespace Nova64
