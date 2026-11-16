/**
 * @file RegBase.hpp
 * @brief Base register state management for the recompiler
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class tracks the state of MIPS registers during dynamic recompilation,
 * enabling optimizations like constant propagation and register mapping.
 */

#pragma once

#include "../Types.hpp"
#include <cstdint>
#include <array>
#include <optional>
#include <cassert>
#include <string>
#include <string_view>

namespace Nova64 {
namespace Recompiler {

class CRegInfo;

/**
 * @enum RegisterWidth
 * @brief Width of register value
 */
enum class RegisterWidth : uint8_t {
    Bits32, ///< 32-bit value
    Bits64, ///< 64-bit value
    Unknown ///< Width not determined
};

/**
 * @enum RegisterMapping
 * @brief How register is mapped to host
 */
enum class RegisterMapping : uint8_t {
    Unmapped,     ///< Not mapped to any host register
    MappedToHost, ///< Mapped to a host register
    Constant,     ///< Known constant value
    Unknown       ///< Mapping not determined
};

/**
 * @enum FpuRoundingMode
 * @brief FPU rounding modes
 */
enum class FpuRoundingMode : int8_t {
    Unknown = -1, ///< Unknown/invalid rounding mode
    Default = 0,  ///< Round to nearest, ties to even
    Truncate = 1, ///< Round toward zero
    Ceiling = 2,  ///< Round toward +infinity
    Floor = 3,    ///< Round toward -infinity
    Nearest = 4,  ///< Round to nearest, ties away from zero

    // Aliases for common modes
    ToZero = Truncate,
    ToPosInf = Ceiling,
    ToNegInf = Floor,
    RoundNearest = Nearest
};

/**
 * @struct RegisterState
 * @brief Complete state information for a single MIPS register
 */
struct RegisterState {
    RegisterWidth width = RegisterWidth::Unknown;
    RegisterMapping mapping = RegisterMapping::Unknown;
    bool isSigned = false;      ///< Value is signed (affects extension)
    bool isModified = false;    ///< Register has been written
    bool isDirty = false;       ///< Modified but not yet written back
    bool isLocked = false;      ///< Cannot be evicted from host register
    uint64_t constantValue = 0; ///< Known constant value if applicable

    // Query methods
    bool IsKnown() const noexcept { return mapping != RegisterMapping::Unknown; }
    bool IsUnknown() const noexcept { return mapping == RegisterMapping::Unknown; }
    bool IsMapped() const noexcept { return mapping == RegisterMapping::MappedToHost; }
    bool IsConstant() const noexcept { return mapping == RegisterMapping::Constant; }
    bool Is32Bit() const noexcept { return width == RegisterWidth::Bits32; }
    bool Is64Bit() const noexcept { return width == RegisterWidth::Bits64; }
    bool IsModifiedOrDirty() const noexcept { return isModified || isDirty; }

    void Reset() noexcept {
        width = RegisterWidth::Unknown;
        mapping = RegisterMapping::Unknown;
        isSigned = false;
        isModified = false;
        isDirty = false;
        isLocked = false;
        constantValue = 0;
    }

    std::string ToString() const;
};

/**
 * @class RegisterSet
 * @brief Manages the complete state of all MIPS registers
 * 
 * This class provides efficient access and manipulation of register states
 * with bounds checking and comprehensive query capabilities.
 */
class RegisterSet {
public:
    static constexpr size_t NUM_GPR = 32; ///< Number of general-purpose registers
    static constexpr size_t NUM_FPR = 32; ///< Number of floating-point registers
    static constexpr size_t NUM_HILO = 2; ///< HI and LO registers

    RegisterSet() = default;
    ~RegisterSet() = default;

    RegisterSet(const RegisterSet&) = delete;
    RegisterSet& operator=(const RegisterSet&) = delete;
    RegisterSet(RegisterSet&&) = default;
    RegisterSet& operator=(RegisterSet&&) = default;

    // Register state accessors
    RegisterState& GetState(size_t regIndex) noexcept {
        assert(regIndex < NUM_GPR);
        return m_States[regIndex];
    }

    const RegisterState& GetState(size_t regIndex) const noexcept {
        assert(regIndex < NUM_GPR);
        return m_States[regIndex];
    }

    // Register value accessors
    uint64_t GetValue(size_t regIndex) const noexcept {
        assert(regIndex < NUM_GPR);
        return m_Values[regIndex];
    }

    void SetValue(size_t regIndex, uint64_t value) noexcept {
        assert(regIndex < NUM_GPR);
        m_Values[regIndex] = value;
    }

    // 32-bit accessors
    uint32_t GetValue32(size_t regIndex) const noexcept {
        assert(regIndex < NUM_GPR);
        return static_cast<uint32_t>(m_Values[regIndex] & 0xFFFFFFFF);
    }

    void SetValue32(size_t regIndex, uint32_t value) noexcept {
        assert(regIndex < NUM_GPR);
        m_Values[regIndex] = static_cast<uint64_t>(value);
    }

    int64_t GetValueSigned(size_t regIndex) const noexcept { return static_cast<int64_t>(GetValue(regIndex)); }

    int32_t GetValueSigned32(size_t regIndex) const noexcept { return static_cast<int32_t>(GetValue32(regIndex)); }

    // State query methods
    bool IsKnown(size_t regIndex) const noexcept { return GetState(regIndex).IsKnown(); }

    bool IsMapped(size_t regIndex) const noexcept { return GetState(regIndex).IsMapped(); }

    bool IsConstant(size_t regIndex) const noexcept { return GetState(regIndex).IsConstant(); }

    bool IsModified(size_t regIndex) const noexcept { return GetState(regIndex).isModified; }

    bool IsDirty(size_t regIndex) const noexcept { return GetState(regIndex).isDirty; }

    bool Is32Bit(size_t regIndex) const noexcept { return GetState(regIndex).Is32Bit(); }

    bool Is64Bit(size_t regIndex) const noexcept { return GetState(regIndex).Is64Bit(); }

    void SetConstant(size_t regIndex, uint64_t value, RegisterWidth width, bool isSigned = false) noexcept {
        assert(regIndex < NUM_GPR);
        auto& state = m_States[regIndex];
        state.Reset();
        state.mapping = RegisterMapping::Constant;
        state.width = width;
        state.isSigned = isSigned;
        m_Values[regIndex] = value;
    }

    void SetMapped(size_t regIndex, RegisterWidth width, bool isSigned = false) noexcept {
        assert(regIndex < NUM_GPR);
        auto& state = m_States[regIndex];
        state.Reset();
        state.mapping = RegisterMapping::MappedToHost;
        state.width = width;
        state.isSigned = isSigned;
    }

    void MarkModified(size_t regIndex, bool dirty = true) noexcept {
        assert(regIndex < NUM_GPR);
        auto& state = m_States[regIndex];
        state.isModified = true;
        state.isDirty = dirty;
    }

    void ClearDirty(size_t regIndex) noexcept {
        assert(regIndex < NUM_GPR);
        m_States[regIndex].isDirty = false;
    }

    void Lock(size_t regIndex) noexcept {
        assert(regIndex < NUM_GPR);
        m_States[regIndex].isLocked = true;
    }

    void Unlock(size_t regIndex) noexcept {
        assert(regIndex < NUM_GPR);
        m_States[regIndex].isLocked = false;
    }

    bool IsLocked(size_t regIndex) const noexcept { return GetState(regIndex).isLocked; }

    void Reset() noexcept {
        for (auto& state : m_States) {
            state.Reset();
        }
        m_Values.fill(0);
        m_CycleCount = 0;
        m_FpuUsed = false;
        m_RoundingMode = FpuRoundingMode::Default;
    }

    // Cycle count accessors
    void SetCycleCount(uint32_t count) noexcept { m_CycleCount = count; }
    uint32_t GetCycleCount() const noexcept { return m_CycleCount; }

    // FPU state accessors
    void SetFpuUsed(bool used) noexcept { m_FpuUsed = used; }
    bool IsFpuUsed() const noexcept { return m_FpuUsed; }

    // Rounding mode accessors
    void SetRoundingMode(FpuRoundingMode mode) noexcept { m_RoundingMode = mode; }
    FpuRoundingMode GetRoundingMode() const noexcept { return m_RoundingMode; }

    void ResetRegister(size_t regIndex) noexcept {
        assert(regIndex < NUM_GPR);
        m_States[regIndex].Reset();
        m_Values[regIndex] = 0;
    }

    // Bulk operations
    void CopyFrom(const RegisterSet& other) noexcept;
    bool Compare(const RegisterSet& other) const noexcept;

    // Debug information
    std::string ToString() const;
    std::string RegisterToString(size_t regIndex) const;

private:
    friend class CRegBase; // For legacy compatibility

    std::array<RegisterState, NUM_GPR> m_States;
    std::array<uint64_t, NUM_GPR> m_Values;

    uint32_t m_CycleCount = 0; ///< Cycle count for current block
    bool m_FpuUsed = false;    ///< Whether FPU has been used
    FpuRoundingMode m_RoundingMode = FpuRoundingMode::Default;
};

/**
 * @class CRegBase
 * @brief Legacy-compatible wrapper for RegisterSet
 * 
 * Provides backward compatibility with existing code while using
 * the modern RegisterSet internally.
 */
class CRegBase {
public:
    CRegBase();
    virtual ~CRegBase() = default;

    CRegBase(CRegBase&&) = default;
    CRegBase& operator=(CRegBase&&) = default;

    // Legacy enum for backward compatibility
    enum REG_STATE : uint8_t {
        STATE_UNKNOWN = 0x00,
        STATE_KNOWN_VALUE = 0x01,
        STATE_X86_MAPPED = 0x02,
        STATE_SIGN = 0x04,
        STATE_32BIT = 0x08,
        STATE_MODIFIED = 0x10,

        STATE_MAPPED_64 = (STATE_KNOWN_VALUE | STATE_X86_MAPPED),
        STATE_MAPPED_32_ZERO = (STATE_KNOWN_VALUE | STATE_X86_MAPPED | STATE_32BIT),
        STATE_MAPPED_32_SIGN = (STATE_KNOWN_VALUE | STATE_X86_MAPPED | STATE_32BIT | STATE_SIGN),
        STATE_CONST_32_ZERO = (STATE_KNOWN_VALUE | STATE_32BIT),
        STATE_CONST_32_SIGN = (STATE_KNOWN_VALUE | STATE_32BIT | STATE_SIGN),
        STATE_CONST_64 = (STATE_KNOWN_VALUE),
    };

    // Legacy interface for backward compatibility
    void SetMipsRegState(int32_t reg, REG_STATE state) noexcept;

    bool IsKnown(int32_t reg) const noexcept { return m_Registers.IsKnown(reg); }
    bool IsUnknown(int32_t reg) const noexcept { return !IsKnown(reg); }
    bool IsModified(int32_t reg) const noexcept { return m_Registers.IsModified(reg); }
    bool IsMapped(int32_t reg) const noexcept { return m_Registers.IsMapped(reg); }
    bool IsConst(int32_t reg) const noexcept { return m_Registers.IsConstant(reg); }
    bool IsSigned(int32_t reg) const noexcept;
    bool IsUnsigned(int32_t reg) const noexcept { return !IsSigned(reg); }
    bool Is32Bit(int32_t reg) const noexcept { return m_Registers.Is32Bit(reg); }
    bool Is64Bit(int32_t reg) const noexcept { return m_Registers.Is64Bit(reg); }

    // Value accessors
    uint64_t GetMipsReg(int32_t reg) const noexcept { return m_Registers.GetValue(reg); }
    int64_t GetMipsReg_S(int32_t reg) const noexcept { return m_Registers.GetValueSigned(reg); }
    uint32_t GetMipsRegLo(int32_t reg) const noexcept { return m_Registers.GetValue32(reg); }
    int32_t GetMipsRegLo_S(int32_t reg) const noexcept { return m_Registers.GetValueSigned32(reg); }
    uint32_t GetMipsRegHi(int32_t reg) const noexcept;
    int32_t GetMipsRegHi_S(int32_t reg) const noexcept;

    void SetMipsRegLo(int32_t reg, uint32_t value) noexcept { m_Registers.SetValue32(reg, value); }
    void SetMipsRegHi(int32_t reg, uint32_t value) noexcept;
    void SetMipsReg(int32_t reg, uint64_t value) noexcept { m_Registers.SetValue(reg, value); }
    void SetMipsReg_S(int32_t reg, int64_t value) noexcept { m_Registers.SetValue(reg, static_cast<uint64_t>(value)); }

    // Cycle count management
    void SetBlockCycleCount(uint32_t cycleCount) noexcept { m_Registers.SetCycleCount(cycleCount); }
    uint32_t GetBlockCycleCount() const noexcept { return m_Registers.GetCycleCount(); }

    // FPU state
    void SetFpuBeenUsed(bool fpuUsed) noexcept { m_Registers.SetFpuUsed(fpuUsed); }
    bool GetFpuBeenUsed() const noexcept { return m_Registers.IsFpuUsed(); }

    FpuRoundingMode GetRoundingModel() const noexcept { return m_Registers.GetRoundingMode(); }
    void SetRoundingModel(FpuRoundingMode roundingModel) noexcept { m_Registers.SetRoundingMode(roundingModel); }

    CRegBase& operator=(const CRegBase& other);
    bool operator==(const CRegBase& right) const;
    bool operator!=(const CRegBase& right) const;

    // Modern interface
    RegisterSet& GetRegisterSet() noexcept { return m_Registers; }
    const RegisterSet& GetRegisterSet() const noexcept { return m_Registers; }

    // Debugging
    std::string ToString() const;
    std::string RegisterToString(int32_t reg) const;

protected:
    const char* RoundingModelName(FpuRoundingMode roundType) const noexcept;

private:
    RegisterSet m_Registers;
};

inline void CRegBase::SetMipsRegHi(int32_t reg, uint32_t value) noexcept {
    // Replaces bits 63..32 and keeps the low word; no sign extension is applied.
    uint64_t current = m_Registers.GetValue(reg);
    current = (current & 0xFFFFFFFF) | (static_cast<uint64_t>(value) << 32);
    m_Registers.SetValue(reg, current);
}

inline uint32_t CRegBase::GetMipsRegHi(int32_t reg) const noexcept {
    return static_cast<uint32_t>(m_Registers.GetValue(reg) >> 32);
}

inline int32_t CRegBase::GetMipsRegHi_S(int32_t reg) const noexcept {
    return static_cast<int32_t>(m_Registers.GetValue(reg) >> 32);
}

} // namespace Recompiler
} // namespace Nova64
