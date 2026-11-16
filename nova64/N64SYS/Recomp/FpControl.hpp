/**
 * @file FpControl.hpp
 * @brief Portable floating-point control/status register shim.
 *
 * Nova64 N64 Emulator
 *
 * The interpreter callbacks shared by the recompiler backends were written
 * against the x86 MXCSR register (via _mm_getcsr/_mm_setcsr) to control the
 * host rounding mode and to read/clear IEEE exception flags while emulating the
 * VR4300 FPU.  This header exposes that same interface as nova_mm_getcsr() /
 * nova_mm_setcsr() so the shared callback code compiles unchanged on every host:
 *
 *   - x86/x64: thin inline wrappers over the real _mm_getcsr/_mm_setcsr.
 *   - AArch64: the MXCSR bit-layout is emulated on top of FPCR (rounding/flush)
 *              and FPSR (cumulative exception flags).  Only the bits the
 *              callbacks actually touch are translated.
 *
 * MXCSR bit layout (the contract the callbacks rely on):
 *   bit 0  IE  Invalid       bit 3  OE  Overflow
 *   bit 1  DE  Denormal       bit 4  UE  Underflow
 *   bit 2  ZE  Divide-by-zero bit 5  PE  Inexact (precision)
 *   bit 6  DAZ
 *   bits 7..12  exception masks (no traps used here -> ignored on AArch64)
 *   bits 13..14 RC rounding control: 0=Near 1=-Inf 2=+Inf 3=Zero
 *   bit 15 FZ  flush-to-zero
 */

#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)

#include <immintrin.h>
static inline uint32_t nova_mm_getcsr() { return _mm_getcsr(); }
static inline void nova_mm_setcsr(uint32_t v) { _mm_setcsr(v); }

#elif defined(__aarch64__) || defined(_M_ARM64)

namespace nova_fp_detail {
static inline uint64_t rd_fpcr() {
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}
static inline void wr_fpcr(uint64_t v) { __asm__ __volatile__("msr fpcr, %0" ::"r"(v)); }
static inline uint64_t rd_fpsr() {
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(v));
    return v;
}
static inline void wr_fpsr(uint64_t v) { __asm__ __volatile__("msr fpsr, %0" ::"r"(v)); }
} // namespace nova_fp_detail

// Return the current FP state expressed in MXCSR bit layout.
static inline uint32_t nova_mm_getcsr() {
    const uint64_t fpcr = nova_fp_detail::rd_fpcr();
    const uint64_t fpsr = nova_fp_detail::rd_fpsr();
    uint32_t mxcsr = 0;

    // FPSR cumulative flags -> MXCSR flags.
    if (fpsr & (1u << 0)) mxcsr |= (1u << 0); // IOC -> Invalid
    if (fpsr & (1u << 7)) mxcsr |= (1u << 1); // IDC -> Denormal (input denormal)
    if (fpsr & (1u << 1)) mxcsr |= (1u << 2); // DZC -> Divide-by-zero
    if (fpsr & (1u << 2)) mxcsr |= (1u << 3); // OFC -> Overflow
    if (fpsr & (1u << 3)) mxcsr |= (1u << 4); // UFC -> Underflow
    if (fpsr & (1u << 4)) mxcsr |= (1u << 5); // IXC -> Inexact

    // FPCR.RMode (bits 23:22) -> MXCSR RC (bits 14:13).  ARM and x86 disagree
    // on the +Inf/-Inf encodings, so 01<->10 are swapped.
    const uint32_t rmode = static_cast<uint32_t>((fpcr >> 22) & 0x3);
    uint32_t rc;
    switch (rmode) {
    case 0:  rc = 0; break; // Nearest
    case 1:  rc = 2; break; // FPCR +Inf -> MXCSR +Inf (10)
    case 2:  rc = 1; break; // FPCR -Inf -> MXCSR -Inf (01)
    default: rc = 3; break; // Zero
    }
    mxcsr |= rc << 13;

    if (fpcr & (1ull << 24)) mxcsr |= (1u << 15); // FZ -> flush-to-zero
    return mxcsr;
}

// Apply an MXCSR-layout value to the host FPCR/FPSR.
static inline void nova_mm_setcsr(uint32_t mxcsr) {
    // Rounding control -> FPCR.RMode.
    const uint32_t rc = (mxcsr >> 13) & 0x3;
    uint32_t rmode;
    switch (rc) {
    case 0:  rmode = 0; break;
    case 1:  rmode = 2; break; // MXCSR -Inf (01) -> FPCR -Inf (10)
    case 2:  rmode = 1; break; // MXCSR +Inf (10) -> FPCR +Inf (01)
    default: rmode = 3; break; // Zero
    }
    uint64_t fpcr = nova_fp_detail::rd_fpcr();
    fpcr = (fpcr & ~(0x3ull << 22)) | (static_cast<uint64_t>(rmode) << 22);
    if (mxcsr & (1u << 15)) fpcr |= (1ull << 24);
    else fpcr &= ~(1ull << 24); // FZ
    nova_fp_detail::wr_fpcr(fpcr);

    // Exception flags -> FPSR (clear the translated bits, then set per MXCSR).
    uint64_t fpsr = nova_fp_detail::rd_fpsr();
    fpsr &= ~((1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 7));
    if (mxcsr & (1u << 0)) fpsr |= (1ull << 0); // Invalid       -> IOC
    if (mxcsr & (1u << 1)) fpsr |= (1ull << 7); // Denormal      -> IDC
    if (mxcsr & (1u << 2)) fpsr |= (1ull << 1); // Divide-by-zero-> DZC
    if (mxcsr & (1u << 3)) fpsr |= (1ull << 2); // Overflow      -> OFC
    if (mxcsr & (1u << 4)) fpsr |= (1ull << 3); // Underflow     -> UFC
    if (mxcsr & (1u << 5)) fpsr |= (1ull << 4); // Inexact       -> IXC
    nova_fp_detail::wr_fpsr(fpsr);
}

#else
#error "FpControl.hpp: unsupported architecture"
#endif
