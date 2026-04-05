/**
 * @file x64RspOps.hpp
 * @brief x64 RSP Operations - Platform-specific x64/SSE2 instruction emission.
 */
// SuperNova64 - N64 Emulator
// x64 RSP Operations - Platform-specific x64/SSE2 instruction emission
// Provides typed wrappers around raw x64 encoding for RSP JIT compilation

#pragma once
#if defined(__amd64__) || defined(_M_X64)

#include <cstdint>
#include <cstddef>
#include <string>

namespace Nova64::RSP::Recompiler::x64 {

// ──────────────────────────────────────────────
// x64 general-purpose register identifiers
// ──────────────────────────────────────────────
enum class GpReg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

// ──────────────────────────────────────────────
// XMM register identifiers (128-bit SIMD)
// ──────────────────────────────────────────────
enum class XmmReg : uint8_t {
    XMM0  = 0,  XMM1  = 1,  XMM2  = 2,  XMM3  = 3,
    XMM4  = 4,  XMM5  = 5,  XMM6  = 6,  XMM7  = 7,
    XMM8  = 8,  XMM9  = 9,  XMM10 = 10, XMM11 = 11,
    XMM12 = 12, XMM13 = 13, XMM14 = 14, XMM15 = 15,
};

// ──────────────────────────────────────────────
// Operand size for memory operations
// ──────────────────────────────────────────────
enum class OpSize : uint8_t {
    Byte  = 1,
    Word  = 2,
    Dword = 4,
    Qword = 8,
};

// ──────────────────────────────────────────────
// Condition codes for Jcc/SETcc/CMOVcc
// ──────────────────────────────────────────────
enum class Condition : uint8_t {
    Overflow    = 0x0,  // OF=1
    NoOverflow  = 0x1,  // OF=0
    Below       = 0x2,  // CF=1
    AboveEqual  = 0x3,  // CF=0
    Equal       = 0x4,  // ZF=1
    NotEqual    = 0x5,  // ZF=0
    BelowEqual  = 0x6,  // CF=1 || ZF=1
    Above       = 0x7,  // CF=0 && ZF=0
    Sign        = 0x8,  // SF=1
    NotSign     = 0x9,  // SF=0
    Parity      = 0xA,  // PF=1
    NoParity    = 0xB,  // PF=0
    Less        = 0xC,  // SF!=OF
    GreaterEq   = 0xD,  // SF==OF
    LessEqual   = 0xE,  // ZF=1 || SF!=OF
    Greater     = 0xF,  // ZF=0 && SF==OF
};

// ──────────────────────────────────────────────
// x64 Instruction Emitter for RSP JIT
// Low-level raw code emission into a buffer
// ──────────────────────────────────────────────
class CX64RspOps {
public:
    CX64RspOps() = default;

    // Initialize with a code buffer to emit into
    void Initialize(uint8_t* buffer, size_t size);
    void Reset();

    // ── Buffer management ──
    [[nodiscard]] uint8_t* CodePos() const    { return m_Pos; }
    [[nodiscard]] size_t   CodeSize() const   { return static_cast<size_t>(m_Pos - m_Buffer); }
    [[nodiscard]] size_t   Remaining() const  { return m_BufferSize - CodeSize(); }
    [[nodiscard]] bool     HasSpace(size_t n) const { return Remaining() >= n; }

    // ══════════════════════════════════════════
    // General-purpose register instructions
    // ══════════════════════════════════════════

    // ── MOV ──
    void MovRegImm32(GpReg dst, uint32_t imm);
    void MovRegImm64(GpReg dst, uint64_t imm);
    void MovRegReg(GpReg dst, GpReg src);
    void MovRegMem(GpReg dst, void* addr);
    void MovMemReg(void* addr, GpReg src);
    void MovMemImm32(void* addr, uint32_t imm);

    // ── Arithmetic ──
    void AddRegImm32(GpReg dst, int32_t imm);
    void AddRegReg(GpReg dst, GpReg src);
    void AddRegMem(GpReg dst, void* addr);
    void SubRegImm32(GpReg dst, int32_t imm);
    void SubRegReg(GpReg dst, GpReg src);
    void AndRegImm32(GpReg dst, uint32_t imm);
    void AndRegReg(GpReg dst, GpReg src);
    void OrRegImm32(GpReg dst, uint32_t imm);
    void OrRegReg(GpReg dst, GpReg src);
    void XorRegReg(GpReg dst, GpReg src);
    void NotReg(GpReg reg);
    void NegReg(GpReg reg);
    void ImulRegReg(GpReg dst, GpReg src);
    void ShlRegImm8(GpReg dst, uint8_t count);
    void ShrRegImm8(GpReg dst, uint8_t count);
    void SarRegImm8(GpReg dst, uint8_t count);
    void ShlRegCl(GpReg dst);
    void ShrRegCl(GpReg dst);
    void SarRegCl(GpReg dst);

    // ── Compare / Test ──
    void CmpRegImm32(GpReg reg, uint32_t imm);
    void CmpRegReg(GpReg r1, GpReg r2);
    void CmpRegMem(GpReg reg, void* addr);
    void CmpMemImm32(void* addr, uint32_t imm);
    void TestRegReg(GpReg r1, GpReg r2);
    void TestRegImm32(GpReg reg, uint32_t imm);

    // ── SETcc ──
    void Setcc(Condition cc, GpReg dst);

    // ── CMOVcc ──
    void Cmovcc(Condition cc, GpReg dst, GpReg src);

    // ── Stack ──
    void Push(GpReg reg);
    void Pop(GpReg reg);

    // ── Jumps & Calls ──
    void Jcc(Condition cc, int32_t rel32);
    void JmpRel32(int32_t rel32);
    void JmpReg(GpReg reg);
    void CallReg(GpReg reg);
    void CallAbs(void* addr);
    void Ret();

    // ── LEA helpers ──
    void LeaRegMemDisp(GpReg dst, GpReg base, int32_t disp);

    // ══════════════════════════════════════════
    // SSE2 / SIMD instructions (128-bit packed)
    // ══════════════════════════════════════════

    // ── Data movement ──
    void Movdqa_XmmXmm(XmmReg dst, XmmReg src);        // movdqa xmm, xmm
    void Movdqa_XmmMem(XmmReg dst, void* addr);         // movdqa xmm, [mem]
    void Movdqa_MemXmm(void* addr, XmmReg src);         // movdqa [mem], xmm
    void Movdqu_XmmMem(XmmReg dst, void* addr);         // movdqu xmm, [mem] (unaligned)
    void Movdqu_MemXmm(void* addr, XmmReg src);         // movdqu [mem], xmm (unaligned)
    void Movd_XmmReg(XmmReg dst, GpReg src);            // movd xmm, reg32
    void Movd_RegXmm(GpReg dst, XmmReg src);            // movd reg32, xmm
    void Movq_XmmReg(XmmReg dst, GpReg src);            // movq xmm, reg64
    void Movq_RegXmm(GpReg dst, XmmReg src);            // movq reg64, xmm

    // ── XMM zeroing ──
    void Pxor_XmmXmm(XmmReg dst, XmmReg src);           // pxor xmm, xmm
    void Xorps_XmmXmm(XmmReg dst, XmmReg src);          // xorps xmm, xmm

    // ── Packed 16-bit signed arithmetic ──
    void Paddw_XmmXmm(XmmReg dst, XmmReg src);          // paddw  (wrap add)
    void Paddsw_XmmXmm(XmmReg dst, XmmReg src);         // paddsw (saturated signed add)
    void Paddusw_XmmXmm(XmmReg dst, XmmReg src);        // paddusw (saturated unsigned add)
    void Psubw_XmmXmm(XmmReg dst, XmmReg src);          // psubw  (wrap sub)
    void Psubsw_XmmXmm(XmmReg dst, XmmReg src);         // psubsw (saturated signed sub)
    void Psubusw_XmmXmm(XmmReg dst, XmmReg src);        // psubusw (saturated unsigned sub)
    void Pmullw_XmmXmm(XmmReg dst, XmmReg src);         // pmullw (multiply low)
    void Pmulhw_XmmXmm(XmmReg dst, XmmReg src);         // pmulhw (multiply high signed)
    void Pmulhuw_XmmXmm(XmmReg dst, XmmReg src);        // pmulhuw (multiply high unsigned)
    void Pmaddwd_XmmXmm(XmmReg dst, XmmReg src);        // pmaddwd (multiply-add)

    // ── Packed 16-bit min/max ──
    void Pminsw_XmmXmm(XmmReg dst, XmmReg src);         // pminsw (signed min)
    void Pmaxsw_XmmXmm(XmmReg dst, XmmReg src);         // pmaxsw (signed max)
    void Pminuw_XmmXmm(XmmReg dst, XmmReg src);         // SSE4.1: pminuw (unsigned min)
    void Pmaxuw_XmmXmm(XmmReg dst, XmmReg src);         // SSE4.1: pmaxuw (unsigned max)

    // ── Packed 16-bit compare ──
    void Pcmpeqw_XmmXmm(XmmReg dst, XmmReg src);        // pcmpeqw
    void Pcmpgtw_XmmXmm(XmmReg dst, XmmReg src);        // pcmpgtw

    // ── Bitwise ──
    void Pand_XmmXmm(XmmReg dst, XmmReg src);           // pand
    void Pandn_XmmXmm(XmmReg dst, XmmReg src);          // pandn
    void Por_XmmXmm(XmmReg dst, XmmReg src);            // por
    void Pxor_XmmMem(XmmReg dst, void* addr);           // pxor xmm, [mem]

    // ── Shifts (packed 16-bit) ──
    void Psllw_XmmImm8(XmmReg dst, uint8_t count);      // psllw xmm, imm
    void Psrlw_XmmImm8(XmmReg dst, uint8_t count);      // psrlw xmm, imm
    void Psraw_XmmImm8(XmmReg dst, uint8_t count);      // psraw xmm, imm

    // ── Shifts (packed 32-bit) ──
    void Pslld_XmmImm8(XmmReg dst, uint8_t count);      // pslld xmm, imm
    void Psrld_XmmImm8(XmmReg dst, uint8_t count);      // psrld xmm, imm
    void Psrad_XmmImm8(XmmReg dst, uint8_t count);      // psrad xmm, imm

    // ── Shifts (packed 64-bit) ──
    void Psllq_XmmImm8(XmmReg dst, uint8_t count);      // psllq xmm, imm
    void Psrlq_XmmImm8(XmmReg dst, uint8_t count);      // psrlq xmm, imm

    // ── Shuffle / Interleave / Pack ──
    void Pshuflw_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t imm);   // pshuflw
    void Pshufhw_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t imm);   // pshufhw
    void Pshufd_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t imm);    // pshufd
    void Pshufb_XmmXmm(XmmReg dst, XmmReg src);                      // SSSE3: pshufb
    void Punpcklwd_XmmXmm(XmmReg dst, XmmReg src);      // punpcklwd
    void Punpckhwd_XmmXmm(XmmReg dst, XmmReg src);      // punpckhwd
    void Punpckldq_XmmXmm(XmmReg dst, XmmReg src);      // punpckldq
    void Punpckhdq_XmmXmm(XmmReg dst, XmmReg src);      // punpckhdq
    void Packsswb_XmmXmm(XmmReg dst, XmmReg src);       // packsswb
    void Packuswb_XmmXmm(XmmReg dst, XmmReg src);       // packuswb
    void Packssdw_XmmXmm(XmmReg dst, XmmReg src);       // packssdw
    void Packusdw_XmmXmm(XmmReg dst, XmmReg src);       // SSE4.1: packusdw

    // ── Blend (SSE4.1) ──
    void Pblendw_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t mask);  // pblendw
    void Blendvps_XmmXmm(XmmReg dst, XmmReg src);                    // blendvps (xmm0 = mask)

    // ── Extract / Insert ──
    void Pextrw_RegXmm(GpReg dst, XmmReg src, uint8_t idx);         // pextrw
    void Pinsrw_XmmRegImm8(XmmReg dst, GpReg src, uint8_t idx);     // pinsrw

    // ── Horizontal operations ──
    void Phaddw_XmmXmm(XmmReg dst, XmmReg src);         // SSSE3: phaddw
    void Phsubw_XmmXmm(XmmReg dst, XmmReg src);         // SSSE3: phsubw
    void Phaddsw_XmmXmm(XmmReg dst, XmmReg src);        // SSSE3: phaddsw

    // ── Absolute value (SSSE3) ──
    void Pabsw_XmmXmm(XmmReg dst, XmmReg src);          // pabsw

    // ── Conversion ──
    void Pmovsxbw_XmmXmm(XmmReg dst, XmmReg src);       // SSE4.1: sign-extend bytes to words
    void Pmovzxbw_XmmXmm(XmmReg dst, XmmReg src);       // SSE4.1: zero-extend bytes to words

    // ── Miscellaneous ──
    void Pmovmskb_RegXmm(GpReg dst, XmmReg src);        // pmovmskb (move byte mask)

    // ══════════════════════════════════════════
    // Helper: emit with base+displacement addressing
    // ══════════════════════════════════════════

    void Movdqa_XmmBaseDisp(XmmReg dst, GpReg base, int32_t disp);
    void Movdqa_BaseDispXmm(GpReg base, int32_t disp, XmmReg src);

    // ── Logging ──
    void SetLog(std::string* log) { m_Log = log; }

    // ── Statistics ──
    struct EmitStats {
        uint32_t GpInstructions{0};
        uint32_t SimdInstructions{0};
        uint32_t BranchInstructions{0};
        uint32_t MemoryInstructions{0};
        void Reset() { GpInstructions = SimdInstructions = BranchInstructions = MemoryInstructions = 0; }
    };

    [[nodiscard]] const EmitStats& Stats() const { return m_Stats; }

private:
    // Raw emission helpers
    void EmitByte(uint8_t b);
    void EmitWord(uint16_t w);
    void EmitDword(uint32_t d);
    void EmitQword(uint64_t q);
    void EmitRex(bool w, uint8_t reg, uint8_t idx, uint8_t rm);
    void EmitModRM(uint8_t mod, uint8_t reg, uint8_t rm);
    void EmitSIB(uint8_t scale, uint8_t idx, uint8_t base);
    void EmitMemOp(uint8_t reg, void* addr);
    void EmitBaseDispOp(uint8_t reg, GpReg base, int32_t disp);

    // SSE prefix + opcode emission
    void EmitSSE2Op(uint8_t prefix, uint8_t op1, uint8_t op2, XmmReg dst, XmmReg src);
    void EmitSSE2MemOp(uint8_t prefix, uint8_t op1, uint8_t op2, XmmReg xmm, void* addr);
    void EmitSSE2BaseDispOp(uint8_t prefix, uint8_t op1, uint8_t op2,
                            XmmReg xmm, GpReg base, int32_t disp);

    void LogInstr(const char* fmt, ...);

    uint8_t*    m_Buffer{nullptr};
    uint8_t*    m_Pos{nullptr};
    size_t      m_BufferSize{0};
    std::string* m_Log{nullptr};
    EmitStats   m_Stats;
};

} // namespace Nova64::RSP::Recompiler::x64

#endif // x64 only
