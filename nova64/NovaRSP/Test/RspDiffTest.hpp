/**
 * @file RspDiffTest.hpp
 * @brief Randomised differential test for the RSP engines.
 *
 * Generates random but well-formed RSP programs and runs each one on two
 * independent RSP systems, one per engine. After every program the complete
 * architectural state (scalar and vector registers, accumulator, flags, DMEM,
 * IMEM, RDRAM, the SP and DPC registers) must be identical. The interpreter is
 * the reference: it is the code the rest of the emulator already trusts.
 *
 * Run it with `nova64 --rsp-selftest`.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP::Test {

enum class Engine : int {
    Interpreter = 0,
    Recompiler  = 1,
};

struct DiffTestOptions {
    uint64_t seed       = 1;                     ///< Same seed, same programs
    uint32_t programs   = 2000;                  ///< How many random programs to run
    uint32_t only       = 0xFFFFFFFFu;           ///< Run just this program index (to reproduce a failure)
    uint32_t maxLength  = 160;                   ///< Instructions per program, upper bound
    bool     verbose    = false;                 ///< Print every program that fails, with disassembly words
    Engine   reference  = Engine::Interpreter;
    Engine   candidate  = Engine::Recompiler;
};

/// Returns 0 when every program produced identical state on both engines.
int RunDifferentialTest(const DiffTestOptions& options);

} // namespace Nova64::RSP::Test
