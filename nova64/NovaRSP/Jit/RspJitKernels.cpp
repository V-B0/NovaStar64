/**
 * @file RspJitKernels.cpp
 * @brief Vector-unit kernels. See RspJitKernels.hpp.
 *
 * Reading guide: each kernel is the interpreter's loop over 8 lanes
 * (RSPInterpreterOps.cpp) written once for all lanes. The accumulator is kept as
 * three 16-bit lanes per element (high, mid, low) exactly as the interpreter
 * keeps it; a 48-bit value is rebuilt in 64-bit lanes when arithmetic needs it.
 */

#include "RspJitKernels.hpp"

#include "../CPU/RSPRegisters.hpp"

#include <cstring>

#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
#define NOVA_RSP_KERNELS 1
#endif

#ifdef NOVA_RSP_KERNELS

namespace Nova64::RSP::Jit {

namespace {

using V8s = int16_t  __attribute__((vector_size(16)));
using V8u = uint16_t __attribute__((vector_size(16)));
using V8w = int32_t  __attribute__((vector_size(32)));
using V8x = uint32_t __attribute__((vector_size(32)));
using V8l = int64_t  __attribute__((vector_size(64)));

template <class To, class From> inline To Cvt(From v) { return __builtin_convertvector(v, To); }   // per-lane conversion
template <class To, class From> inline To Bc(From v) { return (To)v; }                               // same bits, other lane type

inline V8s Ld(const void* p) { V8s v; std::memcpy(&v, p, sizeof(v)); return v; }
inline void St(void* p, V8s v) { std::memcpy(p, &v, sizeof(v)); }
inline V8s Splat(int16_t x) { return V8s{x, x, x, x, x, x, x, x}; }
inline V8w SplatW(int32_t x) { return V8w{x, x, x, x, x, x, x, x}; }

// (mask & a) | (~mask & b), for masks that are all ones or all zeros per lane.
inline V8s Sel(V8s mask, V8s a, V8s b) { return (mask & a) | (~mask & b); }
inline V8w SelW(V8w mask, V8w a, V8w b) { return (mask & a) | (~mask & b); }

// ---- operand fetch -------------------------------------------------------------
struct Op {
    unsigned vd, vs, vt, e;
    explicit Op(uint32_t w)
        : vd((w >> 6) & 31), vs((w >> 11) & 31), vt((w >> 16) & 31), e((w >> 21) & 15) {}
};

// VT with the instruction's element selector applied (RSPVector::BroadcastIndex).
inline V8s Select(V8s v, unsigned e)
{
    switch (e) {
    case 0: case 1: return v;
    case 2:  return __builtin_shufflevector(v, v, 1, 1, 3, 3, 5, 5, 7, 7);
    case 3:  return __builtin_shufflevector(v, v, 0, 0, 2, 2, 4, 4, 6, 6);
    case 4:  return __builtin_shufflevector(v, v, 3, 3, 3, 3, 7, 7, 7, 7);
    case 5:  return __builtin_shufflevector(v, v, 2, 2, 2, 2, 6, 6, 6, 6);
    case 6:  return __builtin_shufflevector(v, v, 1, 1, 1, 1, 5, 5, 5, 5);
    case 7:  return __builtin_shufflevector(v, v, 0, 0, 0, 0, 4, 4, 4, 4);
    case 8:  return Splat(v[7]);
    case 9:  return Splat(v[6]);
    case 10: return Splat(v[5]);
    case 11: return Splat(v[4]);
    case 12: return Splat(v[3]);
    case 13: return Splat(v[2]);
    case 14: return Splat(v[1]);
    default: return Splat(v[0]);
    }
}

inline V8s Vs(CRSPRegisters* r, const Op& o) { return Ld(r->m_Vect[o.vs].Data()); }
inline V8s Vt(CRSPRegisters* r, const Op& o) { return Select(Ld(r->m_Vect[o.vt].Data()), o.e); }
inline void SetVd(CRSPRegisters* r, const Op& o, V8s v) { St(r->m_Vect[o.vd].Data(), v); }

// ---- flags: each is 8 lanes of 0 or 0xFFFF ---------------------------------------
inline V8s FlagMask(const RSPFlag& f) { return Ld(f.Value()) != 0; }   // -1 where set
inline void SetFlag(RSPFlag& f, V8s mask) { St(f.Value(), mask); }
inline void ClearFlag(RSPFlag& f) { St(f.Value(), V8s{}); }

// ---- the accumulator ------------------------------------------------------------
struct Acc { V8s H, M, L; };

inline Acc LoadAcc(CRSPRegisters* r)
{
    return {Ld(r->m_ACCUM.HighData()), Ld(r->m_ACCUM.MidData()), Ld(r->m_ACCUM.LowData())};
}
inline void StoreAcc(CRSPRegisters* r, const Acc& a)
{
    St(r->m_ACCUM.HighData(), a.H);
    St(r->m_ACCUM.MidData(),  a.M);
    St(r->m_ACCUM.LowData(),  a.L);
}
inline V8l AccGet(const Acc& a)   // RSPAccumulator::Get
{
    return (Cvt<V8l>(a.H) << 32) | (Cvt<V8l>(Bc<V8u>(a.M)) << 16) | Cvt<V8l>(Bc<V8u>(a.L));
}
inline void AccSet(Acc& a, V8l v)   // RSPAccumulator::Set
{
    a.L = Cvt<V8s>(v);
    a.M = Cvt<V8s>(v >> 16);
    a.H = Cvt<V8s>(v >> 32);
}

// RSPAccumulator::Saturate(el, true) and (el, false)
inline V8s SatHigh(V8s H, V8s M)
{
    const V8s negH       = H < 0;
    const V8s inRangeNeg = (H == -1) & (M < 0);
    const V8s rNeg       = Sel(inRangeNeg, M, Splat(static_cast<int16_t>(0x8000)));
    const V8s overflow   = (H != 0) | (M < 0);
    const V8s rPos       = Sel(overflow, Splat(0x7FFF), M);
    return Sel(negH, rNeg, rPos);
}
inline V8s SatLow(const Acc& a)
{
    const V8s fits = ((a.H == 0) & (a.M >= 0)) | ((a.H == -1) & (a.M < 0));
    const V8s neg  = a.H < 0;
    return Sel(fits, a.L, Sel(neg, V8s{}, Splat(-1)));
}

inline V8s Clamp16(V8w v)
{
    v = SelW(v < SplatW(-32768), SplatW(-32768), v);
    v = SelW(v > SplatW(32767), SplatW(32767), v);
    return Cvt<V8s>(v);
}

// ---- multiplies -------------------------------------------------------------------
void VMULF(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8l prod = Cvt<V8l>(Cvt<V8w>(S) * Cvt<V8w>(T));
    AccSet(a, prod * 2 + 0x8000);
    StoreAcc(r, a);
    SetVd(r, o, SatHigh(a.H, a.M));
}

void VMUDL(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8x p = (Cvt<V8x>(Bc<V8u>(S)) * Cvt<V8x>(Bc<V8u>(T))) >> 16;
    a.L = Cvt<V8s>(p);
    a.M = V8s{};
    a.H = V8s{};
    StoreAcc(r, a);
    SetVd(r, o, a.L);
}

void VMUDM(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8w p = Bc<V8w>(Bc<V8x>(Cvt<V8w>(S)) * Cvt<V8x>(Bc<V8u>(T)));
    AccSet(a, Cvt<V8l>(p));
    StoreAcc(r, a);
    SetVd(r, o, a.M);
}

void VMUDN(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8w p = Bc<V8w>(Cvt<V8x>(Bc<V8u>(S)) * Bc<V8x>(Cvt<V8w>(T)));
    AccSet(a, Cvt<V8l>(p));
    StoreAcc(r, a);
    SetVd(r, o, a.L);
}

void VMUDH(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8l prod = Cvt<V8l>(Cvt<V8w>(S) * Cvt<V8w>(T));
    AccSet(a, prod << 16);
    StoreAcc(r, a);
    SetVd(r, o, SatHigh(a.H, a.M));
}

void VMACF(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8l prod = Cvt<V8l>(Cvt<V8w>(S) * Cvt<V8w>(T));
    AccSet(a, AccGet(a) + (prod << 1));
    StoreAcc(r, a);
    SetVd(r, o, SatHigh(a.H, a.M));
}

void VMADL(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8x p = (Cvt<V8x>(Bc<V8u>(S)) * Cvt<V8x>(Bc<V8u>(T))) >> 16;
    AccSet(a, AccGet(a) + Cvt<V8l>(p));
    StoreAcc(r, a);
    SetVd(r, o, SatLow(a));
}

void VMADM(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    // signed vs times unsigned vt: the product fits in 32 bits as a signed value
    const V8w p = Bc<V8w>(Bc<V8x>(Cvt<V8w>(S)) * Cvt<V8x>(Bc<V8u>(T)));
    AccSet(a, AccGet(a) + Cvt<V8l>(p));
    StoreAcc(r, a);
    SetVd(r, o, SatHigh(a.H, a.M));
}

void VMADN(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    // unsigned vs times signed vt
    const V8w p = Bc<V8w>(Cvt<V8x>(Bc<V8u>(S)) * Bc<V8x>(Cvt<V8w>(T)));
    AccSet(a, AccGet(a) + Cvt<V8l>(p));
    StoreAcc(r, a);
    SetVd(r, o, SatLow(a));
}

void VMADH(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    // Only the upper 32 bits (high:mid) take part; the low lane is left alone.
    const V8w hm = (Cvt<V8w>(a.H) << 16) | Bc<V8w>(Cvt<V8x>(Bc<V8u>(a.M)));
    const V8w value = Bc<V8w>(Bc<V8x>(hm) + Bc<V8x>(Cvt<V8w>(S) * Cvt<V8w>(T)));   // wraps at 32 bits, as the interpreter's int32 does
    a.H = Cvt<V8s>(value >> 16);
    a.M = Cvt<V8s>(value);
    StoreAcc(r, a);
    SetVd(r, o, SatHigh(a.H, a.M));
}

// ---- add / subtract ---------------------------------------------------------------
void VADD(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    const V8w carry = -Cvt<V8w>(FlagMask(r->m_VCOL));
    const V8w value = Cvt<V8w>(S) + Cvt<V8w>(T) + carry;
    a.L = Cvt<V8s>(value);
    St(r->m_ACCUM.LowData(), a.L);
    SetVd(r, o, Clamp16(value));
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
}

void VSUB(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8w carry = -Cvt<V8w>(FlagMask(r->m_VCOL));
    const V8w value = Cvt<V8w>(S) - Cvt<V8w>(T) - carry;
    St(r->m_ACCUM.LowData(), Cvt<V8s>(value));
    SetVd(r, o, Clamp16(value));
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
}

void VADDC(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8w temp = Cvt<V8w>(Bc<V8u>(S)) + Cvt<V8w>(Bc<V8u>(T));
    const V8s low = Cvt<V8s>(temp);
    St(r->m_ACCUM.LowData(), low);
    SetVd(r, o, low);
    ClearFlag(r->m_VCOH);
    SetFlag(r->m_VCOL, Cvt<V8s>((temp >> 16) != 0));
}

void VSUBC(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8w temp = Cvt<V8w>(Bc<V8u>(S)) - Cvt<V8w>(Bc<V8u>(T));
    const V8s low = Cvt<V8s>(temp);
    St(r->m_ACCUM.LowData(), low);
    SetVd(r, o, low);
    SetFlag(r->m_VCOL, Cvt<V8s>((temp >> 16) != 0));
    SetFlag(r->m_VCOH, Cvt<V8s>(temp != 0));
}

// ---- compare and select -----------------------------------------------------------
void VLT(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s both = FlagMask(r->m_VCOL) & FlagMask(r->m_VCOH);
    const V8s takeS = (S < T) | ((S == T) & both);
    const V8s result = Sel(takeS, S, T);
    SetFlag(r->m_VCCL, takeS);
    St(r->m_ACCUM.LowData(), result);
    SetVd(r, o, result);
    ClearFlag(r->m_VCCH);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
}

void VEQ(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s eq = (S == T) & ~FlagMask(r->m_VCOH);
    const V8s result = Sel(eq, S, T);
    SetFlag(r->m_VCCL, eq);
    St(r->m_ACCUM.LowData(), result);
    SetVd(r, o, result);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
    ClearFlag(r->m_VCCH);
}

void VNE(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s ne = (S != T) | FlagMask(r->m_VCOH);
    const V8s result = Sel(ne, S, T);
    SetFlag(r->m_VCCL, ne);
    St(r->m_ACCUM.LowData(), result);
    SetVd(r, o, result);
    ClearFlag(r->m_VCCH);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
}

void VGE(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s both = FlagMask(r->m_VCOL) & FlagMask(r->m_VCOH);
    const V8s takeS = (S > T) | ((S == T) & ~both);
    const V8s result = Sel(takeS, S, T);
    SetFlag(r->m_VCCL, takeS);
    St(r->m_ACCUM.LowData(), result);
    SetVd(r, o, result);
    ClearFlag(r->m_VCCH);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
}

void VMRG(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s result = Sel(FlagMask(r->m_VCCL), S, T);
    St(r->m_ACCUM.LowData(), result);
    SetVd(r, o, result);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
}

// ---- logic ---------------------------------------------------------------------------
template <int Kind>
void Logic(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    V8s result;
    switch (Kind) {
    case 0:  result = S & T; break;      // VAND
    case 1:  result = ~(S & T); break;   // VNAND
    case 2:  result = S | T; break;      // VOR
    case 3:  result = ~(S | T); break;   // VNOR
    case 4:  result = S ^ T; break;      // VXOR
    default: result = ~(S ^ T); break;   // VNXOR
    }
    St(r->m_ACCUM.LowData(), result);
    SetVd(r, o, result);
}

void VSAW(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    V8s result{};
    switch (o.e) {   // the interpreter reads the selector from the rs field's low four bits
    case 8:  result = Ld(r->m_ACCUM.HighData()); break;
    case 9:  result = Ld(r->m_ACCUM.MidData());  break;
    case 10: result = Ld(r->m_ACCUM.LowData());  break;
    default: break;
    }
    SetVd(r, o, result);
}


// ---- decoding for vector loads and stores ---------------------------------------------------
inline uint32_t BaseValue(CRSPRegisters* r, uint32_t w) { return r->m_GPR[(w >> 21) & 31].UW; }
inline int32_t  VOffset(uint32_t w) { return static_cast<int32_t>(w << 25) >> 25; }   // signed 7 bits
inline unsigned Del(uint32_t w) { return (w >> 7) & 15; }
inline uint8_t* VBytes(CRSPRegisters* r, uint32_t w) { return static_cast<uint8_t*>(r->m_Vect[(w >> 16) & 31].Data()); }

// ---- more vector computation ---------------------------------------------------------------
void VMULU(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    AccSet(a, Cvt<V8l>(Cvt<V8w>(S) * Cvt<V8w>(T)) * 2 + 0x8000);
    StoreAcc(r, a);
    SetVd(r, o, Sel(a.H < 0, V8s{}, Sel((a.H ^ a.M) < 0, Splat(-1), a.M)));
}

void VMACU(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    Acc a = LoadAcc(r);
    AccSet(a, AccGet(a) + (Cvt<V8l>(Cvt<V8w>(S) * Cvt<V8w>(T)) << 1));
    StoreAcc(r, a);
    SetVd(r, o, Sel(a.H < 0, V8s{}, Sel((a.H != 0) | (a.M < 0), Splat(-1), a.M)));
}

void VABS(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s pos = S > 0, neg = S < 0;
    const V8s negT = Bc<V8s>(V8u{} - Bc<V8u>(T));           // -vt, wrapping like the interpreter's store
    const V8s special = neg & (T == Splat(static_cast<int16_t>(0x8000)));
    const V8s result = Sel(pos, T, Sel(neg, Sel(special, Splat(0x7FFF), negT), V8s{}));
    const V8s accLow = Sel(pos, T, Sel(neg, Sel(special, Splat(static_cast<int16_t>(0x8000)), negT), V8s{}));
    St(r->m_ACCUM.LowData(), accLow);
    SetVd(r, o, result);
}

void VCL(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s coL = FlagMask(r->m_VCOL), coH = FlagMask(r->m_VCOH);
    const V8s ccL = FlagMask(r->m_VCCL), ccH = FlagMask(r->m_VCCH), ce = FlagMask(r->m_VCE);
    const V8w Su = Cvt<V8w>(Bc<V8u>(S)), Tu = Cvt<V8w>(Bc<V8u>(T));
    const V8s negT = Bc<V8s>(V8u{} - Bc<V8u>(T));

    const V8s caseA = coL & coH;
    const V8s caseB = coL & ~coH;
    const V8s caseC = ~coL & coH;
    const V8s caseD = ~coL & ~coH;

    const V8s setB = Cvt<V8s>(SelW(Cvt<V8w>(ce), (Su + Tu) <= SplatW(0x10000), (Tu + Su) == SplatW(0)));
    const V8s geD  = Cvt<V8s>((Su - Tu) >= SplatW(0));

    V8s low = Sel(caseA, Sel(ccL, negT, S),
              Sel(caseB, Sel(setB, negT, S),
              Sel(caseC, Sel(ccH, T, S),
                         Sel(geD, T, S))));
    SetFlag(r->m_VCCL, Sel(caseB, setB, ccL));
    SetFlag(r->m_VCCH, Sel(caseD, geD, ccH));
    St(r->m_ACCUM.LowData(), low);
    SetVd(r, o, low);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
    ClearFlag(r->m_VCE);
}

void VCH(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s signs = (S ^ T) < 0;                              // lanes where the signs differ
    const V8s negT  = Bc<V8s>(V8u{} - Bc<V8u>(T));
    const V8s notT  = ~T;
    const V8s sum   = S + T;                                    // int16 wrap, as in the interpreter
    const V8s diff  = S - T;
    const V8s neMask = S != notT;

    // signs differ
    const V8s sumLE0 = sum <= 0;
    const V8s lowN   = Sel(sumLE0, negT, S);
    const V8s cohN   = (sum != 0) & neMask;
    const V8s cclN   = sumLE0;
    const V8s cchN   = T < 0;
    const V8s ceN    = sum == Splat(-1);

    // signs agree
    const V8s diffGE0 = diff >= 0;
    const V8s lowP    = Sel(diffGE0, T, S);
    const V8s cohP    = (diff != 0) & neMask;
    const V8s cclP    = T < 0;
    const V8s cchP    = diffGE0;

    const V8s low = Sel(signs, lowN, lowP);
    SetFlag(r->m_VCOL, signs);
    SetFlag(r->m_VCOH, Sel(signs, cohN, cohP));
    SetFlag(r->m_VCCL, Sel(signs, cclN, cclP));
    SetFlag(r->m_VCCH, Sel(signs, cchN, cchP));
    SetFlag(r->m_VCE,  signs & ceN);
    St(r->m_ACCUM.LowData(), low);
    SetVd(r, o, low);
}

void VCR(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s S = Vs(r, o), T = Vt(r, o);
    const V8s signs = (S ^ T) < 0;
    const V8w Sw = Cvt<V8w>(S), Tw = Cvt<V8w>(T);

    // signs differ
    const V8s cchN = T < 0;
    const V8s cclN = Cvt<V8s>((Sw + Tw + SplatW(1)) <= SplatW(0));
    const V8s lowN = Sel(cclN, ~T, S);
    // signs agree
    const V8s cclP = T < 0;
    const V8s cchP = Cvt<V8s>((Sw - Tw) >= SplatW(0));
    const V8s lowP = Sel(cchP, T, S);

    const V8s low = Sel(signs, lowN, lowP);
    SetFlag(r->m_VCCH, Sel(signs, cchN, cchP));
    SetFlag(r->m_VCCL, Sel(signs, cclN, cclP));
    St(r->m_ACCUM.LowData(), low);
    SetVd(r, o, low);
    ClearFlag(r->m_VCOL);
    ClearFlag(r->m_VCOH);
    ClearFlag(r->m_VCE);
}

// ---- reciprocal, reciprocal square root and element moves (scalar by nature) -------------
inline uint32_t Clz32(uint32_t v) { return v ? static_cast<uint32_t>(__builtin_clz(v)) : 32u; }
inline int16_t* Lanes(CRSPRegisters* r, unsigned reg) { return static_cast<int16_t*>(r->m_Vect[reg].Data()); }

void FinishReciprocal(CRSPRegisters* r, const Op& o, uint32_t w, int32_t result)
{
    r->m_High = false;
    r->m_Result = static_cast<uint16_t>(static_cast<uint32_t>(result) >> 16);
    St(r->m_ACCUM.LowData(), Vt(r, o));
    Lanes(r, o.vd)[7 - ((w >> 11) & 7)] = static_cast<int16_t>(result);
}

int32_t ReciprocalOf(CRSPRegisters* r, int32_t input)
{
    const int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    if (input > -32768) data -= mask;
    if (data == 0) return 0x7FFFFFFF;
    if (input == static_cast<int32_t>(0xFFFF8000)) return static_cast<int32_t>(0xFFFF0000);
    const uint32_t shift = Clz32(static_cast<uint32_t>(data));
    const uint32_t index = static_cast<uint32_t>((static_cast<uint64_t>(data) << shift & 0x7FC00000) >> 22);
    return (((0x10000 | r->m_Reciprocals[index]) << 14) >> (31 - shift)) ^ mask;
}

void VRCP(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const int32_t input = Lanes(r, o.vt)[7 - (o.e & 7)];
    FinishReciprocal(r, o, w, ReciprocalOf(r, input));
}

void VRCPL(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const uint16_t lane = static_cast<uint16_t>(Lanes(r, o.vt)[7 - (o.e & 7)]);
    const int32_t input = r->m_High
        ? static_cast<int32_t>((static_cast<uint32_t>(r->m_In) << 16) | lane)
        : static_cast<int32_t>(static_cast<int16_t>(lane));
    FinishReciprocal(r, o, w, ReciprocalOf(r, input));
}

void VRCPH(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    r->m_High = true;
    r->m_In = static_cast<uint16_t>(Lanes(r, o.vt)[7 - (o.e & 7)]);
    St(r->m_ACCUM.LowData(), Vt(r, o));
    Lanes(r, o.vd)[7 - ((w >> 11) & 7)] = static_cast<int16_t>(r->m_Result);
}

int32_t InverseRootOf(CRSPRegisters* r, uint32_t uinput)
{
    if (uinput == 0) return 0x7FFFFFFF;
    if (uinput == 0xFFFF8000u) return static_cast<int32_t>(0xFFFF0000u);
    if (uinput > 0xFFFF8000u) uinput--;
    const int32_t sinput = static_cast<int32_t>(uinput);
    const int32_t mask = sinput >> 31;
    uinput ^= static_cast<uint32_t>(mask);
    const uint32_t shift = Clz32(uinput) + 1;
    const uint32_t index = ((uinput << (shift & 31)) >> 24) | ((shift & 1) << 8);
    const uint32_t rom = static_cast<uint32_t>(r->m_InverseSquareRoots[index]) << 14;
    const uint32_t rShift = (32 - shift) >> 1;
    return static_cast<int32_t>((0x40000000u | rom) >> rShift) ^ mask;
}

void VRSQ(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const uint32_t uinput = static_cast<uint32_t>(static_cast<int32_t>(Lanes(r, o.vt)[7 - (o.e & 7)]));
    FinishReciprocal(r, o, w, InverseRootOf(r, uinput));
}

void VRSQL(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const uint16_t lane = static_cast<uint16_t>(Lanes(r, o.vt)[7 - (o.e & 7)]);
    const uint32_t uinput = r->m_High
        ? ((static_cast<uint32_t>(r->m_In) << 16) | lane)
        : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(lane)));
    FinishReciprocal(r, o, w, InverseRootOf(r, uinput));
}

void VRSQH(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    VRCPH(r, nullptr, w);   // identical: same lane in, same lane out
}

void VMOV(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const Op o(w);
    const V8s T = Vt(r, o);
    St(r->m_ACCUM.LowData(), T);
    const unsigned index = 7 - ((w >> 11) & 7);
    Lanes(r, o.vd)[index] = T[index];
}

// ---- moves between scalar and vector registers --------------------------------------------
inline uint8_t PackFlag(const RSPFlag& f)
{
    const uint16_t* v = f.Value();
    uint8_t packed = 0;
    for (int i = 0; i < 8; ++i) if (v[i]) packed |= static_cast<uint8_t>(1u << (7 - i));
    return packed;
}
inline void UnpackFlag(RSPFlag& f, uint8_t packed)
{
    uint16_t* v = f.Value();
    for (int i = 0; i < 8; ++i) v[i] = (packed & (1u << (7 - i))) ? 0xFFFF : 0x0000;
}

void MFC2(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const unsigned rt = (w >> 16) & 31;
    if (rt == 0) return;
    const unsigned element = (w >> 7) & 15;                       // the interpreter's `sa >> 1`
    const uint8_t* v = static_cast<const uint8_t*>(r->m_Vect[(w >> 11) & 31].Data());
    const uint16_t hi = v[15 - element], lo = v[15 - ((element + 1) % 16)];
    r->m_GPR[rt].W = static_cast<int16_t>(static_cast<uint16_t>((hi << 8) | lo));
}

void CFC2(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const unsigned rt = (w >> 16) & 31;
    if (rt == 0) return;
    switch ((w >> 11) & 3) {
    case 0:  r->m_GPR[rt].W = static_cast<int16_t>((PackFlag(r->m_VCOH) << 8) | PackFlag(r->m_VCOL)); break;
    case 1:  r->m_GPR[rt].W = static_cast<int16_t>((PackFlag(r->m_VCCH) << 8) | PackFlag(r->m_VCCL)); break;
    default: r->m_GPR[rt].W = PackFlag(r->m_VCE); break;
    }
}

void MTC2(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const unsigned element = 15 - ((w >> 7) & 15);
    uint8_t* v = static_cast<uint8_t*>(r->m_Vect[(w >> 11) & 31].Data());
    const uint32_t value = r->m_GPR[(w >> 16) & 31].UW;
    v[element] = static_cast<uint8_t>(value >> 8);                // GPR.B[1]
    if (element != 0) v[element - 1] = static_cast<uint8_t>(value);   // GPR.B[0]
}

void CTC2(CRSPRegisters* r, uint8_t*, uint32_t w)
{
    const uint32_t value = r->m_GPR[(w >> 16) & 31].UW;
    switch ((w >> 11) & 3) {
    case 0:  UnpackFlag(r->m_VCOL, static_cast<uint8_t>(value)); UnpackFlag(r->m_VCOH, static_cast<uint8_t>(value >> 8)); break;
    case 1:  UnpackFlag(r->m_VCCL, static_cast<uint8_t>(value)); UnpackFlag(r->m_VCCH, static_cast<uint8_t>(value >> 8)); break;
    default: UnpackFlag(r->m_VCE, static_cast<uint8_t>(value)); break;
    }
}

// ---- packed loads ----------------------------------------------------------------------------
void LPV(CRSPRegisters* r, uint8_t* dmem, uint32_t w)
{
    uint32_t addr = BaseValue(r, w) + static_cast<uint32_t>(VOffset(w) * 8);
    const uint32_t off = (addr & 7) - Del(w);
    addr &= ~7u;
    uint16_t* vec = static_cast<uint16_t*>(r->m_Vect[(w >> 16) & 31].Data());
    for (unsigned i = 0; i < 8; ++i)
        vec[7 - i] = static_cast<uint16_t>(dmem[((addr + ((off + i) & 0xF)) ^ 3) & 0xFFF] << 8);
}

void LUV(CRSPRegisters* r, uint8_t* dmem, uint32_t w)
{
    uint32_t addr = BaseValue(r, w) + static_cast<uint32_t>(VOffset(w) * 8);
    const uint32_t off = (addr & 7) - Del(w);
    addr &= ~7u;
    int16_t* vec = static_cast<int16_t*>(r->m_Vect[(w >> 16) & 31].Data());
    for (unsigned i = 0; i < 8; ++i)
        vec[7 - i] = static_cast<int16_t>(static_cast<int16_t>(dmem[((addr + ((off + i) & 0xF)) ^ 3) & 0xFFF]) << 7);
}

// ---- vector loads and stores ------------------------------------------------------------
// The vector register is 16 bytes with byte i of the N64 register at u8(15 - i);
// DMEM is stored with each 4-byte word byte-reversed, hence the `^ 3`.


// A load or store of `size` bytes (1, 2, 4, 8), starting at element `del`.
template <unsigned LogSize>
void LoadSmall(CRSPRegisters* r, uint8_t* dmem, uint32_t w)
{
    constexpr unsigned size = 1u << LogSize;
    uint32_t addr = (BaseValue(r, w) + static_cast<uint32_t>(VOffset(w) * static_cast<int32_t>(size))) & 0xFFF;
    uint8_t* vec = VBytes(r, w);
    const unsigned del = Del(w);
    const unsigned length = size < 16 - del ? size : 16 - del;

    if constexpr (size == 8) {
        // The common case: element 0, word aligned. The two words land in the upper half
        // of the register, in swapped order.
        if (del == 0 && (addr & 3) == 0) {
            uint32_t a, b;
            std::memcpy(&a, dmem + addr, 4);
            std::memcpy(&b, dmem + ((addr + 4) & 0xFFC), 4);
            std::memcpy(vec + 12, &a, 4);
            std::memcpy(vec + 8, &b, 4);
            return;
        }
    }
    for (unsigned i = del, n = length + del; i < n; ++i, ++addr)
        vec[15 - i] = dmem[(addr ^ 3) & 0xFFF];
}

template <unsigned LogSize>
void StoreSmall(CRSPRegisters* r, uint8_t* dmem, uint32_t w)
{
    constexpr unsigned size = 1u << LogSize;
    uint32_t addr = (BaseValue(r, w) + static_cast<uint32_t>(VOffset(w) * static_cast<int32_t>(size))) & 0xFFF;
    const uint8_t* vec = VBytes(r, w);
    const unsigned del = Del(w);

    if constexpr (size == 8) {
        if (del == 0 && (addr & 3) == 0) {
            uint32_t a, b;
            std::memcpy(&a, vec + 12, 4);
            std::memcpy(&b, vec + 8, 4);
            std::memcpy(dmem + addr, &a, 4);
            std::memcpy(dmem + ((addr + 4) & 0xFFC), &b, 4);
            return;
        }
    }
    for (unsigned i = del, n = size + del; i < n; ++i, ++addr)
        dmem[(addr ^ 3) & 0xFFF] = vec[15 - (i & 0xF)];
}

void LQV(CRSPRegisters* r, uint8_t* dmem, uint32_t w)
{
    uint32_t addr = (BaseValue(r, w) + static_cast<uint32_t>(VOffset(w) * 16)) & 0xFFF;
    uint8_t* vec = VBytes(r, w);
    const unsigned del = Del(w);
    if (del == 0 && (addr & 15) == 0) {
        // 16 aligned bytes: the register is the block with its four words in reverse order.
        uint32_t x[4];
        std::memcpy(x, dmem + addr, 16);
        const uint32_t y[4] = {x[3], x[2], x[1], x[0]};
        std::memcpy(vec, y, 16);
        return;
    }
    const unsigned toBoundary = ((addr + 0x10) & ~0xFu) - addr;
    const unsigned length = toBoundary < 16 - del ? toBoundary : 16 - del;
    for (unsigned i = del, n = length + del; i < n; ++i, ++addr)
        vec[15 - i] = dmem[addr ^ 3];
}

void SQV(CRSPRegisters* r, uint8_t* dmem, uint32_t w)
{
    uint32_t addr = (BaseValue(r, w) + static_cast<uint32_t>(VOffset(w) * 16)) & 0xFFF;
    const uint8_t* vec = VBytes(r, w);
    const unsigned del = Del(w);
    if (del == 0 && (addr & 15) == 0) {
        uint32_t x[4];
        std::memcpy(x, vec, 16);
        const uint32_t y[4] = {x[3], x[2], x[1], x[0]};
        std::memcpy(dmem + addr, y, 16);
        return;
    }
    const unsigned length = ((addr + 0x10) & ~0xFu) - addr;
    for (unsigned i = del; i < length + del; ++i, ++addr)
        dmem[(addr ^ 3) & 0xFFF] = vec[15 - (i & 0xF)];
}

// ---- lookup ------------------------------------------------------------------------------
struct Tables {
    KernelFn vector[64] = {};
    KernelFn scalar[32] = {};   // MFC2 CFC2 MTC2 CTC2, indexed by the rs field
    KernelFn load[32]   = {};
    KernelFn store[32]  = {};
    Tables()
    {
        vector[0]  = VMULF;   vector[4]  = VMUDL;   vector[5]  = VMUDM;   vector[6]  = VMUDN;
        vector[7]  = VMUDH;   vector[8]  = VMACF;   vector[12] = VMADL;   vector[13] = VMADM;
        vector[14] = VMADN;   vector[15] = VMADH;   vector[16] = VADD;    vector[17] = VSUB;
        vector[20] = VADDC;   vector[21] = VSUBC;   vector[29] = VSAW;
        vector[32] = VLT;     vector[33] = VEQ;     vector[34] = VNE;     vector[35] = VGE;
        vector[39] = VMRG;
        vector[1]  = VMULU;   vector[9]  = VMACU;   vector[19] = VABS;
        vector[36] = VCL;     vector[37] = VCH;     vector[38] = VCR;
        vector[48] = VRCP;    vector[49] = VRCPL;   vector[50] = VRCPH;   vector[51] = VMOV;
        vector[52] = VRSQ;    vector[53] = VRSQL;   vector[54] = VRSQH;
        scalar[0] = MFC2;     scalar[2] = CFC2;     scalar[4] = MTC2;     scalar[6] = CTC2;
        vector[40] = Logic<0>; vector[41] = Logic<1>; vector[42] = Logic<2>;
        vector[43] = Logic<3>; vector[44] = Logic<4>; vector[45] = Logic<5>;

        load[0] = LoadSmall<0>;   load[1] = LoadSmall<1>;   load[2] = LoadSmall<2>;   load[3] = LoadSmall<3>;
        load[4] = LQV;   load[6] = LPV;   load[7] = LUV;
        store[0] = StoreSmall<0>; store[1] = StoreSmall<1>; store[2] = StoreSmall<2>; store[3] = StoreSmall<3>;
        store[4] = SQV;
    }
};

} // namespace

KernelFn FindKernel(uint32_t word)
{
    static const Tables tables;
    switch (word >> 26) {
    case 0x12:                                         // COP2: vector computation has bit 25 set
        return (word & (1u << 25)) ? tables.vector[word & 63] : tables.scalar[(word >> 21) & 31];
    case 0x32:  return tables.load[(word >> 11) & 31];   // LWC2
    case 0x3A:  return tables.store[(word >> 11) & 31];  // SWC2
    default:    return nullptr;
    }
}

} // namespace Nova64::RSP::Jit

#else   // no vector extensions: the interpreter handles every vector instruction

namespace Nova64::RSP::Jit {
KernelFn FindKernel(uint32_t) { return nullptr; }
}

#endif
