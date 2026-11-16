/**
 * @file RSPTypes.hpp
 * @brief RSP Core Types.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>
#include <cstring>
#include <array>
#include <bit>

#if defined(_MSC_VER)
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

namespace Nova64::RSP {

// --- Multi-view scalar word (like PJ64 UWORD32) ---
union Word32 {
    int32_t W;
    uint32_t UW;
    int16_t HW[2];
    uint16_t UHW[2];
    int8_t B[4];
    uint8_t UB[4];

    constexpr Word32() : UW(0) {}
    constexpr explicit Word32(uint32_t v) : UW(v) {}
    constexpr explicit Word32(int32_t v) : W(v) {}
};

// --- Multi-view 64-bit doubleword ---
union DWord64 {
    int64_t DW;
    uint64_t UDW;
    int32_t W[2];
    uint32_t UW[2];
    int16_t HW[4];
    uint16_t UHW[4];
    int8_t B[8];
    uint8_t UB[8];

    constexpr DWord64() : UDW(0) {}
    constexpr explicit DWord64(uint64_t v) : UDW(v) {}
    constexpr explicit DWord64(int64_t v) : DW(v) {}
};

/// RSP accumulator: 8 elements of 48 bits each, stored as three 8 x 16-bit lanes (High, Mid, Low).
class alignas(16) RSPAccumulator {
public:
    RSPAccumulator() { Reset(); }

    // Element accessors (0..7), each element is 48-bit (High:Mid:Low)
    [[nodiscard]] int16_t& Low(uint8_t idx) { return reinterpret_cast<int16_t*>(&m_Low)[idx]; }
    [[nodiscard]] int16_t& Mid(uint8_t idx) { return reinterpret_cast<int16_t*>(&m_Mid)[idx]; }
    [[nodiscard]] int16_t& High(uint8_t idx) { return reinterpret_cast<int16_t*>(&m_High)[idx]; }

    [[nodiscard]] uint16_t& ULow(uint8_t idx) { return reinterpret_cast<uint16_t*>(&m_Low)[idx]; }
    [[nodiscard]] uint16_t& UMid(uint8_t idx) { return reinterpret_cast<uint16_t*>(&m_Mid)[idx]; }
    [[nodiscard]] uint16_t& UHigh(uint8_t idx) { return reinterpret_cast<uint16_t*>(&m_High)[idx]; }

    [[nodiscard]] const int16_t& Low(uint8_t idx) const { return reinterpret_cast<const int16_t*>(&m_Low)[idx]; }
    [[nodiscard]] const int16_t& Mid(uint8_t idx) const { return reinterpret_cast<const int16_t*>(&m_Mid)[idx]; }
    [[nodiscard]] const int16_t& High(uint8_t idx) const { return reinterpret_cast<const int16_t*>(&m_High)[idx]; }

    // Get full 48-bit value sign-extended to 64 bits
    [[nodiscard]] int64_t Get(uint8_t idx) const {
        int64_t val = static_cast<int64_t>(High(idx)) << 32 |
                      static_cast<int64_t>(static_cast<uint16_t>(Mid(idx))) << 16 |
                      static_cast<int64_t>(static_cast<uint16_t>(Low(idx)));
        // Sign-extend from 48 bits
        val = (val << 16) >> 16;
        return val;
    }

    // Set full 48-bit value
    void Set(uint8_t idx, int64_t value) {
        Low(idx) = static_cast<int16_t>(value & 0xFFFF);
        Mid(idx) = static_cast<int16_t>((value >> 16) & 0xFFFF);
        High(idx) = static_cast<int16_t>((value >> 32) & 0xFFFF);
    }

    // Saturate element for signed/unsigned clamp
    [[nodiscard]] uint16_t Saturate(uint8_t el, bool highValue) const {
        if (highValue) {
            // Signed saturation: clamp to [0x8000, 0x7FFF]
            if (High(el) < 0) {
                return (High(el) == -1 && Mid(el) < 0) ? static_cast<uint16_t>(Mid(el)) : 0x8000;
            }
            return (High(el) != 0 || Mid(el) < 0) ? 0x7FFF : static_cast<uint16_t>(Mid(el));
        } else {
            // Unsigned saturation: clamp to [0x0000, 0xFFFF]
            // Sign extension check: if High is the sign extension of Mid,
            // the accumulator value fits in 32 bits, so return Low directly
            if (High(el) == 0 && Mid(el) >= 0) return static_cast<uint16_t>(Low(el));
            if (High(el) == -1 && Mid(el) < 0) return static_cast<uint16_t>(Low(el));
            if (High(el) < 0) return 0x0000;
            return 0xFFFF;
        }
    }

    void Reset() { std::memset(this, 0, sizeof(*this)); }

    // Direct SIMD access for recompiler
    [[nodiscard]] void* LowData() { return &m_Low; }
    [[nodiscard]] void* MidData() { return &m_Mid; }
    [[nodiscard]] void* HighData() { return &m_High; }
    [[nodiscard]] const void* LowData() const { return &m_Low; }
    [[nodiscard]] const void* MidData() const { return &m_Mid; }
    [[nodiscard]] const void* HighData() const { return &m_High; }

private:
    alignas(16) uint64_t m_High[2]{}; // Accumulator high (8 × 16-bit)
    alignas(16) uint64_t m_Mid[2]{};  // Accumulator mid  (8 × 16-bit)
    alignas(16) uint64_t m_Low[2]{};  // Accumulator low  (8 × 16-bit)
};

// --- RSP Vector Register - 128 bits = 8 × 16-bit elements ---
class alignas(16) RSPVector {
public:
    RSPVector() { std::memset(m_Data, 0, sizeof(m_Data)); }

    // Element access with broadcast element selection
    // Maps the VT element index based on broadcast specifier 'e':
    //   e=0,1: identity (no broadcast)
    //   e=2,3: pair broadcast
    //   e=4-7: quad broadcast
    //   e=8-15: single-element scalar broadcast
    [[nodiscard]] static uint8_t BroadcastIndex(uint8_t idx, uint8_t e) {
        if (e < 2) return idx;
        if (e < 4) return (idx & 0x6) | (~e & 0x1);
        if (e < 8) return (idx & 0x4) | (~e & 0x3);
        return 7 - (e & 0x7);
    }
    [[nodiscard]] uint16_t& ue(uint8_t idx, uint8_t element) {
        return reinterpret_cast<uint16_t*>(m_Data)[BroadcastIndex(idx, element)];
    }
    [[nodiscard]] int16_t& se(uint8_t idx, uint8_t element) {
        return reinterpret_cast<int16_t*>(m_Data)[BroadcastIndex(idx, element)];
    }

    // Direct element access
    [[nodiscard]] int8_t& s8(uint8_t idx) { return reinterpret_cast<int8_t*>(m_Data)[idx]; }
    [[nodiscard]] uint8_t& u8(uint8_t idx) { return reinterpret_cast<uint8_t*>(m_Data)[idx]; }
    [[nodiscard]] int16_t& s16(uint8_t idx) { return reinterpret_cast<int16_t*>(m_Data)[idx]; }
    [[nodiscard]] uint16_t& u16(uint8_t idx) { return reinterpret_cast<uint16_t*>(m_Data)[idx]; }
    [[nodiscard]] int32_t& s32(uint8_t idx) { return reinterpret_cast<int32_t*>(m_Data)[idx]; }
    [[nodiscard]] uint64_t& u64(uint8_t idx) { return m_Data[idx]; }
    [[nodiscard]] const uint64_t& u64(uint8_t idx) const { return m_Data[idx]; }

    [[nodiscard]] const int16_t& s16(uint8_t idx) const { return reinterpret_cast<const int16_t*>(m_Data)[idx]; }
    [[nodiscard]] const uint16_t& u16(uint8_t idx) const { return reinterpret_cast<const uint16_t*>(m_Data)[idx]; }

    // Raw SIMD-friendly data pointer
    [[nodiscard]] void* Data() { return m_Data; }
    [[nodiscard]] const void* Data() const { return m_Data; }

private:
    alignas(16) uint64_t m_Data[2];
};

/// RSP flag register: 8 per-element boolean flags, each stored as a uint16_t of 0x0000 or 0xFFFF for
/// SIMD use. GetPacked/SetPacked convert to the 8-bit CTC2/CFC2 form (element 0 in bit 7).
class alignas(16) RSPFlag {
public:
    RSPFlag() { Clear(); }

    RSPFlag(const RSPFlag&) = delete;
    RSPFlag& operator=(const RSPFlag&) = delete;

    void Clear() { std::memset(m_Flags, 0, sizeof(m_Flags)); }

    bool Set(uint8_t idx, bool value) {
        m_Flags[idx & 7] = value ? 0xFFFF : 0x0000;
        return value;
    }

    [[nodiscard]] bool Get(uint8_t idx) const { return m_Flags[idx & 7] != 0; }

    // Pack 8 element flags into single byte (for CTC2/CFC2)
    [[nodiscard]] uint8_t GetPacked() const {
        uint8_t result = 0;
        for (int i = 0; i < 8; i++) {
            if (m_Flags[i]) result |= (1 << (7 - i));
        }
        return result;
    }

    void SetPacked(uint8_t value) {
        for (int i = 0; i < 8; i++) {
            m_Flags[i] = (value & (1 << (7 - i))) ? 0xFFFF : 0x0000;
        }
    }

    // Direct SIMD access (8 × uint16_t, each 0x0000 or 0xFFFF)
    [[nodiscard]] uint16_t* Value() { return m_Flags; }
    [[nodiscard]] const uint16_t* Value() const { return m_Flags; }

private:
    alignas(16) uint16_t m_Flags[8]{};
};

} // namespace Nova64::RSP
