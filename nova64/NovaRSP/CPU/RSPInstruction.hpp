/**
 * @file RSPInstruction.hpp
 * @brief RSP Instruction Analysis.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "RSPOpcode.hpp"
#include <cstdint>
#include <string>

namespace Nova64::RSP {

// --- Instruction classification flags ---
enum class RSPInstructionFlag : uint8_t {
    Unknown,
    Branch,
    Break,
    GPROperation,
    Jump,
    JumpRegister,
    Load,
    Store,
    MF,             // Move From (COP0/COP2)
    MT,             // Move To (COP0/COP2)
    CF,             // Control From (flags)
    CT,             // Control To (flags)
    Vector,         // Vector computational
    VectorSetAccum, // Vector that writes accumulator
    VectorUseAccum, // Vector that reads accumulator
    VectorLoad,     // Vector memory load
    VectorStore,    // Vector memory store
    InvalidOpcode,
};

} // namespace Nova64::RSP
