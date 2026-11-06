/**
 * @file RspEngine.hpp
 * @brief Which engine executes RSP code.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP {

/// Both engines produce identical results; the recompiler is faster.
/// See NovaRSP/Jit/ and `nova64 --rsp-selftest`.
enum class RSPEngine : uint8_t {
    Interpreter,
    Recompiler,
};

} // namespace Nova64::RSP
