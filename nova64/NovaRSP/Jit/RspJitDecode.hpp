/**
 * @file RspJitDecode.hpp
 * @brief Instruction decoding shared by the RSP recompiler's front end and emitters.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP::Jit {

struct Fields {
    uint32_t op, rs, rt, rd, sa, funct;
    uint32_t imm;      ///< The 16-bit immediate, zero-extended
    int32_t  simm;     ///< The same immediate, sign-extended
    uint32_t target;   ///< The 26-bit jump target field
};

inline Fields Decode(uint32_t w)
{
    Fields f;
    f.op     = w >> 26;
    f.rs     = (w >> 21) & 31;
    f.rt     = (w >> 16) & 31;
    f.rd     = (w >> 11) & 31;
    f.sa     = (w >> 6) & 31;
    f.funct  = w & 63;
    f.imm    = w & 0xFFFF;
    f.simm   = static_cast<int16_t>(w & 0xFFFF);
    f.target = w & 0x03FFFFFF;
    return f;
}

/// How an instruction affects the flow of the block it is in.
enum class Flow : uint8_t {
    None,         ///< An ordinary instruction
    Branch,       ///< Conditional branch with a delay slot
    Jump,         ///< J / JAL
    JumpReg,      ///< JR / JALR
    EndsBlock,    ///< BREAK and MTC0: they can halt the RSP or rewrite IMEM, so the block stops after them
};

inline Flow Classify(uint32_t word)
{
    const Fields f = Decode(word);
    switch (f.op) {
    case 0x00:
        if (f.funct == 0x08 || f.funct == 0x09) return Flow::JumpReg;   // JR JALR
        if (f.funct == 0x0D) return Flow::EndsBlock;                    // BREAK
        return Flow::None;
    case 0x01:
        // BLTZ BGEZ BLTZAL BGEZAL. Other REGIMM encodings are not branches.
        return (f.rt == 0 || f.rt == 1 || f.rt == 0x10 || f.rt == 0x11) ? Flow::Branch : Flow::None;
    case 0x02: case 0x03: return Flow::Jump;
    case 0x04: case 0x05: case 0x06: case 0x07: return Flow::Branch;
    case 0x10: return f.rs == 0 ? Flow::None : Flow::EndsBlock;       // MFC0 only reads; MTC0 can halt the RSP or start a DMA into IMEM
    default:   return Flow::None;
    }
}

inline bool IsControlTransfer(uint32_t word)
{
    const Flow f = Classify(word);
    return f == Flow::Branch || f == Flow::Jump || f == Flow::JumpReg;
}

} // namespace Nova64::RSP::Jit
