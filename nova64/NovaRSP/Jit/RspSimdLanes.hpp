/**
 * @file RspSimdLanes.hpp
 * @brief A portable stand-in for compiler vector types, for the RSP vector kernels.
 *
 * RspJitKernels.cpp is written against fixed-width vector types with the usual
 * operators. GCC and Clang provide those natively (and turn them into NEON or SSE);
 * MSVC does not. `Lanes<T, N>` supplies the same operators with plain loops over the
 * lanes, which every compiler accepts and most auto-vectorise. It is used wherever the
 * native types are unavailable, and `-DNOVA_RSP_FORCE_LANES` selects it for testing on
 * a machine that has them.
 *
 * Arithmetic goes through the unsigned type of the same width, so overflow wraps the
 * way the native vector types (and the RSP) do instead of being undefined.
 */

#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Nova64::RSP::Jit {

template <class T, int N> struct Lanes {
    using Elem = T;
    using Signed = std::make_signed_t<T>;
    static constexpr int kCount = N;

    T v[N];

    constexpr Lanes() : v{} {}
    static Lanes Splat(T x) {
        Lanes r;
        for (int i = 0; i < N; ++i) r.v[i] = x;
        return r;
    }

    T& operator[](int i) { return v[i]; }
    const T& operator[](int i) const { return v[i]; }
};

// ---- arithmetic and bitwise operators, lane by lane -------------------------------
#define NOVA_LANES_BINARY(OP)                                                                                          \
    template <class T, int N> inline Lanes<T, N> operator OP(const Lanes<T, N>& a, const Lanes<T, N>& b) {             \
        using Un = std::make_unsigned_t<T>;                                                                            \
        Lanes<T, N> r;                                                                                                 \
        for (int i = 0; i < N; ++i) r.v[i] = static_cast<T>(static_cast<Un>(a.v[i]) OP static_cast<Un>(b.v[i]));       \
        return r;                                                                                                      \
    }                                                                                                                  \
    template <class T, int N> inline Lanes<T, N> operator OP(const Lanes<T, N>& a, T b) {                              \
        return a OP Lanes<T, N>::Splat(b);                                                                             \
    }                                                                                                                  \
    template <class T, int N, class S, class = std::enable_if_t<std::is_integral_v<S> && !std::is_same_v<S, T>>>       \
    inline Lanes<T, N> operator OP(const Lanes<T, N>& a, S b) {                                                        \
        return a OP Lanes<T, N>::Splat(static_cast<T>(b));                                                             \
    }

NOVA_LANES_BINARY(+)
NOVA_LANES_BINARY(-)
NOVA_LANES_BINARY(*)
NOVA_LANES_BINARY(&)
NOVA_LANES_BINARY(|)
NOVA_LANES_BINARY(^)
#undef NOVA_LANES_BINARY

template <class T, int N> inline Lanes<T, N> operator~(const Lanes<T, N>& a) {
    Lanes<T, N> r;
    for (int i = 0; i < N; ++i) r.v[i] = static_cast<T>(~a.v[i]);
    return r;
}

template <class T, int N> inline Lanes<T, N> operator-(const Lanes<T, N>& a) {
    using Un = std::make_unsigned_t<T>;
    Lanes<T, N> r;
    for (int i = 0; i < N; ++i) r.v[i] = static_cast<T>(Un(0) - static_cast<Un>(a.v[i]));
    return r;
}

// Shifts by a count that is the same for every lane. Left shifts go through the
// unsigned type; right shifts are arithmetic for signed lanes and logical for unsigned.
template <class T, int N> inline Lanes<T, N> operator<<(const Lanes<T, N>& a, int n) {
    using Un = std::make_unsigned_t<T>;
    Lanes<T, N> r;
    for (int i = 0; i < N; ++i) r.v[i] = static_cast<T>(static_cast<Un>(a.v[i]) << n);
    return r;
}
template <class T, int N> inline Lanes<T, N> operator>>(const Lanes<T, N>& a, int n) {
    Lanes<T, N> r;
    for (int i = 0; i < N; ++i) r.v[i] = static_cast<T>(a.v[i] >> n);
    return r;
}

// ---- comparisons: a lane is all ones where true, zero where false --------------------
#define NOVA_LANES_COMPARE(OP)                                                                                         \
    template <class T, int N>                                                                                          \
    inline Lanes<std::make_signed_t<T>, N> operator OP(const Lanes<T, N>& a, const Lanes<T, N>& b) {                   \
        Lanes<std::make_signed_t<T>, N> r;                                                                             \
        for (int i = 0; i < N; ++i) r.v[i] = (a.v[i] OP b.v[i]) ? static_cast<std::make_signed_t<T>>(-1) : 0;          \
        return r;                                                                                                      \
    }                                                                                                                  \
    template <class T, int N, class S, class = std::enable_if_t<std::is_integral_v<S>>>                                \
    inline Lanes<std::make_signed_t<T>, N> operator OP(const Lanes<T, N>& a, S b) {                                    \
        return a OP Lanes<T, N>::Splat(static_cast<T>(b));                                                             \
    }

NOVA_LANES_COMPARE(==)
NOVA_LANES_COMPARE(!=)
NOVA_LANES_COMPARE(<)
NOVA_LANES_COMPARE(>)
NOVA_LANES_COMPARE(<=)
NOVA_LANES_COMPARE(>=)
#undef NOVA_LANES_COMPARE

// ---- conversions ----------------------------------------------------------------------
/// Element-wise conversion to another lane type with the same lane count.
template <class To, class From> inline To Cvt(const From& a) {
    static_assert(To::kCount == From::kCount, "lane counts must match");
    To r;
    for (int i = 0; i < To::kCount; ++i) r.v[i] = static_cast<typename To::Elem>(a.v[i]);
    return r;
}

/// The same bits viewed as another lane type of the same total size.
template <class To, class From> inline To Bc(const From& a) {
    static_assert(sizeof(To) == sizeof(From), "sizes must match");
    To r;
    std::memcpy(&r, &a, sizeof(r));
    return r;
}

} // namespace Nova64::RSP::Jit
