/**
 * @file x64RspVectorGen.hpp
 * @brief x64 RSP Vector Generator - SSE2/SSSE3 code generation for RSP vector operations.
 */
// SuperNova64 - N64 Emulator
// x64 RSP Vector Generator - SSE2/SSSE3 code generation for RSP vector operations
// Emits optimized SIMD instruction sequences for each RSP vector opcode

#pragma once
#if defined(__amd64__) || defined(_M_X64)

#include "x64RspOps.hpp"
#include <cstdint>

namespace Nova64::RSP {
class CRSPRegisters;
}

namespace Nova64::RSP::Recompiler {
class CRspRegState;
class RspAssembler;
}

namespace Nova64::RSP::Recompiler::x64 {

// ──────────────────────────────────────────────
// RSP element broadcast patterns
// Maps RSP element field (0-15) to a pshufb mask
// that selects the correct element broadcast
// ──────────────────────────────────────────────
struct ElementPattern {
    uint8_t mask[16];  // pshufb shuffle mask for this element selection
};

// ──────────────────────────────────────────────
// Accumulator layout in memory
// The RSP accumulator is 3 x 128-bit vectors:
//   AccumH (high), AccumM (middle), AccumL (low)
// Each holds 8 x 16-bit elements
// ──────────────────────────────────────────────
struct AccumRegs {
    XmmReg high;
    XmmReg mid;
    XmmReg low;
};

// ──────────────────────────────────────────────
// Translation statistics for vector code generation
// ──────────────────────────────────────────────
struct VectorGenStats {
    uint32_t NativeVectorOps{0};     // Vector ops with full SSE codegen
    uint32_t FallbackVectorOps{0};   // Vector ops using interpreter callback
    uint32_t AccumWriteOptimized{0}; // Accum writes skipped via dead-code analysis
    uint32_t ElementBroadcasts{0};   // pshufb element selection uses

    void Reset()
    {
        NativeVectorOps = FallbackVectorOps = 0;
        AccumWriteOptimized = ElementBroadcasts = 0;
    }
};

// ──────────────────────────────────────────────
// CX64RspVectorGen - SSE2 code generation for RSP
// vector operations (VMULF, VADD, VAND, etc.)
//
// Design: Each Emit*() method generates a sequence
// of SSE2/SSSE3 instructions that implement the
// exact behavior of the corresponding RSP vector
// opcode, operating on XMM registers mapped to
// RSP vector registers by the register allocator.
// ──────────────────────────────────────────────
class CX64RspVectorGen {
public:
    CX64RspVectorGen() = default;

    void Initialize(CX64RspOps* ops, CRSPRegisters* regs);
    void Reset();

    // ══════════════════════════════════════════
    // Element selection / broadcast
    // ══════════════════════════════════════════

    // Load a vector register into an XMM with element broadcast
    // applied according to the RSP 'e' field
    void EmitElementBroadcast(XmmReg dst, XmmReg src, uint8_t element);

    // Load the pshufb mask for a given element into XMM
    void EmitLoadElementMask(XmmReg dst, uint8_t element);

    // ══════════════════════════════════════════
    // Multiply operations
    // ══════════════════════════════════════════

    // VMULF: Signed fraction multiply — vd[i] = clamp((vs[i] * vt[e] * 2 + 0x8000) >> 16)
    void EmitVMULF(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMULU: Unsigned clamped fraction multiply
    void EmitVMULU(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMUDL: Unsigned multiply (low * low → low accumulator)
    void EmitVMUDL(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1);

    // VMUDM: Signed * unsigned multiply → middle accumulator
    void EmitVMUDM(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMUDN: Unsigned * signed multiply → middle accumulator
    void EmitVMUDN(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMUDH: Signed * signed multiply → high accumulator
    void EmitVMUDH(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1);

    // VMACF: Signed fraction multiply-accumulate
    void EmitVMACF(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMACU: Unsigned clamped fraction multiply-accumulate
    void EmitVMACU(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMADL: Unsigned multiply-accumulate low
    void EmitVMADL(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1);

    // VMADM: Signed*unsigned multiply-accumulate middle
    void EmitVMADM(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMADN: Unsigned*signed multiply-accumulate middle
    void EmitVMADN(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1, XmmReg scratch2);

    // VMADH: Signed multiply-accumulate high
    void EmitVMADH(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumH, XmmReg accumM, XmmReg accumL,
                   XmmReg scratch1);

    // ══════════════════════════════════════════
    // Add / Subtract operations
    // ══════════════════════════════════════════

    // VADD: Vector add with carry-in from VCO
    void EmitVADD(XmmReg vd, XmmReg vs, XmmReg vt,
                  XmmReg accumL, XmmReg vcoLo,
                  XmmReg scratch1, XmmReg scratch2);

    // VSUB: Vector subtract with borrow-in from VCO
    void EmitVSUB(XmmReg vd, XmmReg vs, XmmReg vt,
                  XmmReg accumL, XmmReg vcoLo,
                  XmmReg scratch1, XmmReg scratch2);

    // VABS: Vector absolute value
    void EmitVABS(XmmReg vd, XmmReg vs, XmmReg vt,
                  XmmReg accumL, XmmReg scratch1);

    // VADDC: Vector add with carry-out to VCO
    void EmitVADDC(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumL, XmmReg vcoLo,
                   XmmReg scratch1);

    // VSUBC: Vector subtract with carry-out/borrow-out
    void EmitVSUBC(XmmReg vd, XmmReg vs, XmmReg vt,
                   XmmReg accumL, XmmReg vcoLo, XmmReg vcoHi,
                   XmmReg scratch1, XmmReg scratch2);

    // ══════════════════════════════════════════
    // Logical operations
    // ══════════════════════════════════════════

    void EmitVAND(XmmReg vd, XmmReg vs, XmmReg vt, XmmReg accumL);
    void EmitVNAND(XmmReg vd, XmmReg vs, XmmReg vt, XmmReg accumL, XmmReg scratch);
    void EmitVOR(XmmReg vd, XmmReg vs, XmmReg vt, XmmReg accumL);
    void EmitVNOR(XmmReg vd, XmmReg vs, XmmReg vt, XmmReg accumL, XmmReg scratch);
    void EmitVXOR(XmmReg vd, XmmReg vs, XmmReg vt, XmmReg accumL);
    void EmitVNXOR(XmmReg vd, XmmReg vs, XmmReg vt, XmmReg accumL, XmmReg scratch);

    // ══════════════════════════════════════════
    // Comparison / select operations
    // ══════════════════════════════════════════

    void EmitVLT(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL, XmmReg vccLo, XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg scratch1, XmmReg scratch2);

    void EmitVEQ(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL, XmmReg vccLo, XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg scratch1);

    void EmitVNE(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL, XmmReg vccLo, XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg scratch1);

    void EmitVGE(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL, XmmReg vccLo, XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg scratch1, XmmReg scratch2);

    void EmitVCL(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL,
                 XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg vccLo, XmmReg vccHi,
                 XmmReg vce,
                 XmmReg scratch1, XmmReg scratch2, XmmReg scratch3);

    void EmitVCH(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL,
                 XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg vccLo, XmmReg vccHi,
                 XmmReg vce,
                 XmmReg scratch1, XmmReg scratch2, XmmReg scratch3);

    void EmitVCR(XmmReg vd, XmmReg vs, XmmReg vt,
                 XmmReg accumL,
                 XmmReg vcoLo, XmmReg vcoHi,
                 XmmReg vccLo, XmmReg vccHi,
                 XmmReg scratch1, XmmReg scratch2);

    void EmitVMRG(XmmReg vd, XmmReg vs, XmmReg vt,
                  XmmReg accumL, XmmReg vccLo,
                  XmmReg scratch1);

    // ══════════════════════════════════════════
    // Accumulator read
    // ══════════════════════════════════════════

    // VSAW: Read accumulator slices
    void EmitVSAW(XmmReg vd, uint8_t element,
                  XmmReg accumH, XmmReg accumM, XmmReg accumL);

    // ══════════════════════════════════════════
    // Reciprocal / Square root
    // ══════════════════════════════════════════

    // These use interpreter callback (lookup table approach is more accurate)
    void EmitVRCP_Callback(void* interpFunc, const char* name);
    void EmitVRCPL_Callback(void* interpFunc, const char* name);
    void EmitVRCPH_Callback(void* interpFunc, const char* name);
    void EmitVRSQ_Callback(void* interpFunc, const char* name);
    void EmitVRSQL_Callback(void* interpFunc, const char* name);
    void EmitVRSQH_Callback(void* interpFunc, const char* name);
    void EmitVMOV_Callback(void* interpFunc, const char* name);

    // ── Statistics ──
    [[nodiscard]] const VectorGenStats& Stats() const { return m_Stats; }

private:
    // Helper: create 16-bit all-ones mask in XMM
    void EmitAllOnesMask(XmmReg dst);

    // Helper: compute carry from unsigned 16-bit add
    void EmitCarryFromAdd16(XmmReg carry, XmmReg a, XmmReg b, XmmReg scratch);

    // Helper: compute borrow from unsigned 16-bit sub
    void EmitBorrowFromSub16(XmmReg borrow, XmmReg a, XmmReg b, XmmReg scratch);

    // Helper: clamp accumulator to signed/unsigned result
    void EmitClampSigned(XmmReg dst, XmmReg accumH, XmmReg accumM, XmmReg scratch);
    void EmitClampUnsigned(XmmReg dst, XmmReg accumH, XmmReg accumM, XmmReg scratch);

    CX64RspOps*     m_Ops{nullptr};
    CRSPRegisters*  m_Regs{nullptr};
    VectorGenStats  m_Stats;
};

// ──────────────────────────────────────────────
// Pre-computed element broadcast masks
// Index by RSP element field (0-15)
// Used with pshufb to implement element selection
// ──────────────────────────────────────────────
extern const ElementPattern g_ElementBroadcastMasks[16];

} // namespace Nova64::RSP::Recompiler::x64

#endif // x64 only
