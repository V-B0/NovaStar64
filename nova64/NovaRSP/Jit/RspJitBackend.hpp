/**
 * @file RspJitBackend.hpp
 * @brief What the RSP recompiler's front end hands to a CPU-specific code emitter.
 *
 * The front end (RspJit.cpp) finds basic blocks, applies the RSP's branch and
 * delay-slot rules, caches the compiled code and runs it. A backend only has to
 * turn one block into machine code for its CPU: RspJitX64.cpp for x86-64 and
 * RspJitA64.cpp for AArch64. Nothing else about the RSP lives in a backend, so
 * a fix to control flow or caching is made once.
 */

#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace Nova64::RSP {
class CRSPSystem;
class CRSPRegisters;
}

namespace Nova64::RSP::Jit {

/// What compiled code is given. The layout is shared with the emitters, which
/// load these fields in the block prologue.
struct JitContext {
    uint32_t*   gpr;    ///< The 32 scalar registers, 4 bytes each
    uint8_t*    dmem;   ///< 4 KB data memory
    CRSPSystem* sys;    ///< For calls back into the interpreter
    CRSPRegisters* regs;   ///< The whole register file, for the vector kernels
};

/// A compiled block. Takes the context, returns the PC to continue at.
using BlockFn = uint32_t (*)(JitContext*);

struct Insn {
    uint32_t pc   = 0;   ///< Address in IMEM, 0..0xFFC
    uint32_t word = 0;   ///< The raw instruction
};

/// One basic block, already shaped by the RSP's rules.
struct Block {
    enum class End : uint8_t {
        Fallthrough,   ///< Runs into `fallPc`; no branch
        Branch,        ///< Conditional branch (BEQ BNE BLEZ BGTZ BLTZ BGEZ BLTZAL BGEZAL)
        Jump,          ///< J or JAL
        JumpReg,       ///< JR or JALR
    };

    uint32_t          startPc = 0;
    std::vector<Insn> body;            ///< Straight-line instructions, in order
    End               end = End::Fallthrough;
    Insn              terminator;      ///< The branch/jump (unless Fallthrough)
    Insn              delay;           ///< Its delay-slot instruction (unless Fallthrough)
    uint32_t          fallPc = 0;      ///< Where Fallthrough continues
};

/// Generates machine code for blocks and owns the memory it lives in.
class Backend {
public:
    virtual ~Backend() = default;

    /// Returns nullptr when the block can't be compiled; the front end then
    /// finishes the task in the interpreter.
    virtual BlockFn Compile(const Block& block) = 0;
    virtual void    Release(BlockFn fn) = 0;
    virtual const char* Name() const = 0;
};

/// The backend for this machine, or nullptr if there is none.
std::unique_ptr<Backend> CreateBackend();

/// Runs one instruction on the interpreter. Compiled code calls this for
/// everything it doesn't translate itself.
void ExecuteOneOnInterpreter(CRSPSystem* sys, uint32_t word);

} // namespace Nova64::RSP::Jit
