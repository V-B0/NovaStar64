/**
 * @file RspJitKernels.hpp
 * @brief Vector-unit instructions as whole-register kernels.
 *
 * The RSP's vector unit works on 8 lanes of 16 bits at a time, but the
 * interpreter implements each instruction as a loop over the lanes. These
 * kernels do the same arithmetic on all 8 lanes at once (the compiler turns the
 * vector code into NEON or SSE), and compiled blocks call them directly instead
 * of going through the interpreter's decode.
 *
 * Every kernel is written to match the interpreter bit for bit, quirks
 * included; `nova64 --rsp-selftest` holds them to it. The interpreter's version
 * stays in the tree as the reference.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP {
class CRSPRegisters;
}

namespace Nova64::RSP::Jit {

/// `word` is the whole instruction. `dmem` is only used by loads and stores.
using KernelFn = void (*)(CRSPRegisters* regs, uint8_t* dmem, uint32_t word);

/// The kernel for a COP2 vector instruction or a vector load/store, or nullptr
/// when this build has none for it (the interpreter handles it then).
KernelFn FindKernel(uint32_t word);

} // namespace Nova64::RSP::Jit
