/**
 * @file x64RegInfo.hpp
 * @brief x64 register information, mapping, and allocation strategies
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class manages the mapping between MIPS R4300i registers and x64 host
 * registers, implementing register allocation, spilling, and state management
 * for optimal performance in the dynamic recompiler.
 */

#pragma once

#if defined(__amd64__) || defined(_M_X64)

#include "../RegBase.hpp"
#include "x64ops.hpp"
#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <bitset>
#include <optional>
#include <functional>
#include <unordered_map>

namespace Nova64 {
namespace Recompiler {

// Forward declarations
class CodeBlock;
class CX64Ops;

/**
 * @enum X64RegisterClass
 * @brief Classification of x64 register types
 */
enum class X64RegisterClass : uint8_t {
    GeneralPurpose, ///< General purpose registers (RAX, RBX, etc.)
    FloatingPoint,  ///< Floating-point registers (XMM0-XMM15)
    Vector,         ///< Vector registers (YMM, ZMM)
    Special,        ///< Special registers (RIP, RSP, RBP)
    Segment,        ///< Segment registers (CS, DS, etc.)
    Control,        ///< Control registers (CR0-CR15)
    Debug,          ///< Debug registers (DR0-DR15)

    Count           ///< Number of register classes
};

/**
 * @enum X64RegisterType
 * @brief Specific x64 register identifiers
 */
enum class X64RegisterType : uint16_t {
    // 64-bit general purpose
    RAX,
    RBX,
    RCX,
    RDX,
    RSI,
    RDI,
    RBP,
    RSP,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,

    // 32-bit general purpose (low 32 bits of 64-bit regs)
    EAX,
    EBX,
    ECX,
    EDX,
    ESI,
    EDI,
    EBP,
    ESP,
    R8D,
    R9D,
    R10D,
    R11D,
    R12D,
    R13D,
    R14D,
    R15D,

    // 16-bit general purpose
    AX,
    BX,
    CX,
    DX,
    SI,
    DI,
    BP,
    SP,

    // 8-bit general purpose
    AL,
    BL,
    CL,
    DL,
    SIL,
    DIL,
    BPL,
    SPL,
    AH,
    BH,
    CH,
    DH,

    // Floating-point (XMM registers)
    XMM0,
    XMM1,
    XMM2,
    XMM3,
    XMM4,
    XMM5,
    XMM6,
    XMM7,
    XMM8,
    XMM9,
    XMM10,
    XMM11,
    XMM12,
    XMM13,
    XMM14,
    XMM15,

    // Vector (YMM/ZMM registers)
    YMM0,
    YMM1,
    YMM2,
    YMM3,
    YMM4,
    YMM5,
    YMM6,
    YMM7,
    YMM8,
    YMM9,
    YMM10,
    YMM11,
    YMM12,
    YMM13,
    YMM14,
    YMM15,

    // Special
    RIP,    ///< Instruction pointer
    EFLAGS, ///< Flags register

    Invalid ///< Invalid register
};

/**
 * @struct X64RegisterInfo
 * @brief Information about an x64 register
 */
struct X64RegisterInfo {
    X64RegisterType type = X64RegisterType::Invalid;
    X64RegisterClass regClass = X64RegisterClass::GeneralPurpose;
    uint8_t size = 0;           ///< Size in bytes
    uint8_t index = 0;          ///< Register index (0-15 for GPRs/XMMs)
    bool isCallerSaved = true;  ///< Caller must save/restore
    bool isCalleeSaved = false; ///< Callee must save/restore
    bool isVolatile = true;     ///< Can be modified by function calls
    bool isReserved = false;    ///< Reserved for special purposes
    std::string name;           ///< Register name

    constexpr bool IsGeneralPurpose() const noexcept { return regClass == X64RegisterClass::GeneralPurpose; }

    constexpr bool IsFloatingPoint() const noexcept { return regClass == X64RegisterClass::FloatingPoint; }

    constexpr bool IsVector() const noexcept { return regClass == X64RegisterClass::Vector; }

    constexpr bool IsValid() const noexcept { return type != X64RegisterType::Invalid; }

    static X64RegisterInfo GetInfo(X64RegisterType reg);
};

/**
 * @class RegisterMappingInfo
 * @brief Represents a mapping between MIPS and x64 registers
 */
class RegisterMappingInfo {
public:
    RegisterMappingInfo() = default;

    RegisterMappingInfo(uint32_t mipsReg, X64RegisterType hostReg, RegisterWidth width, bool isSigned) :
        m_MipsReg(mipsReg), m_HostReg(hostReg), m_Width(width), m_IsSigned(isSigned), m_IsDirty(false),
        m_IsLocked(false) {}

    uint32_t GetMipsReg() const noexcept { return m_MipsReg; }
    X64RegisterType GetHostReg() const noexcept { return m_HostReg; }
    RegisterWidth GetWidth() const noexcept { return m_Width; }
    bool IsSigned() const noexcept { return m_IsSigned; }
    bool IsDirty() const noexcept { return m_IsDirty; }
    bool IsLocked() const noexcept { return m_IsLocked; }
    bool IsValid() const noexcept { return m_MipsReg < 32 && m_HostReg != X64RegisterType::Invalid; }

    void SetDirty(bool dirty = true) noexcept { m_IsDirty = dirty; }
    void SetLocked(bool locked = true) noexcept { m_IsLocked = locked; }
    void SetWidth(RegisterWidth width) noexcept { m_Width = width; }
    void SetSigned(bool isSigned) noexcept { m_IsSigned = isSigned; }

    // Comparison
    bool operator==(const RegisterMappingInfo& other) const noexcept {
        return m_MipsReg == other.m_MipsReg && m_HostReg == other.m_HostReg;
    }

    bool operator!=(const RegisterMappingInfo& other) const noexcept { return !(*this == other); }

    std::string ToString() const;

private:
    uint32_t m_MipsReg = 0;
    X64RegisterType m_HostReg = X64RegisterType::Invalid;
    RegisterWidth m_Width = RegisterWidth::Bits64;
    bool m_IsSigned = false;
    bool m_IsDirty = false;
    bool m_IsLocked = false;
};

/**
 * @interface IRegisterAllocationStrategy
 * @brief Strategy pattern for register allocation algorithms
 */
class IRegisterAllocationStrategy {
public:
    virtual ~IRegisterAllocationStrategy() = default;

    virtual X64RegisterType AllocateRegister(const RegisterSet& regSet, X64RegisterClass regClass,
                                             uint32_t preferredReg = UINT32_MAX) = 0;
    virtual void FreeRegister(X64RegisterType reg) = 0;
    virtual bool IsRegisterAvailable(X64RegisterType reg) const = 0;
    virtual void Reset() = 0;
    virtual std::string GetName() const = 0;
};

/**
 * @class CX64RegInfo
 * @brief x64-specific register state management and allocation
 * 
 * Manages the mapping between MIPS registers and x64 host registers,
 * implementing efficient register allocation strategies for the
 * dynamic recompiler.
 */
class CX64RegInfo final : public CRegBase {
public:
    /**
     * @brief Construct register info for a code block
     * @param codeBlock Reference to code block
     * @param assembler Reference to x64 assembler
     * @param strategy Register allocation strategy (nullptr for default)
     */
    explicit CX64RegInfo(CodeBlock& codeBlock, CX64Ops& assembler,
                         std::unique_ptr<IRegisterAllocationStrategy> strategy = nullptr);

    ~CX64RegInfo() override;

    // Non-copyable, movable
    CX64RegInfo(const CX64RegInfo&) = delete;
    CX64RegInfo& operator=(const CX64RegInfo&) = delete;
    CX64RegInfo(CX64RegInfo&&) noexcept;
    CX64RegInfo& operator=(CX64RegInfo&&) noexcept;

    // Comparison operators
    bool operator==(const CX64RegInfo& other) const noexcept;
    bool operator!=(const CX64RegInfo& other) const noexcept { return !(*this == other); }

    // Register mapping management
    /**
     * @brief Map a MIPS register to an x64 register
     * @param mipsReg MIPS register index (0-31)
     * @param width Register width
     * @param isSigned Whether the value is signed
     * @param preferredReg Preferred x64 register (optional)
     * @return The allocated x64 register
     * @throws RegisterAllocationError if allocation fails
     */
    X64RegisterType MapRegister(uint32_t mipsReg, RegisterWidth width, bool isSigned = false,
                                X64RegisterType preferredReg = X64RegisterType::Invalid);

    /**
     * @brief Unmap a MIPS register from its host register
     * @param mipsReg MIPS register index
     * @param writeBack Whether to write value back to memory context
     * @param force If true, unmaps even if locked (use with caution)
     */
    void UnmapRegister(uint32_t mipsReg, bool writeBack = true, bool force = false);

    /**
     * @brief Unmap a specific host register
     * @param hostReg Host register to unmap
     * @param writeBack Whether to write value back
     */
    void UnmapHostRegister(X64RegisterType hostReg, bool writeBack = true);

    /**
     * @brief Get the host register for a MIPS register
     * @param mipsReg MIPS register index
     * @return Host register if mapped, Invalid otherwise
     */
    X64RegisterType GetMappedHostRegister(uint32_t mipsReg) const noexcept;

    /**
     * @brief Get the MIPS register mapped to a host register
     * @param hostReg Host register
     * @return MIPS register index if mapped, UINT32_MAX otherwise
     */
    uint32_t GetMappedMipsRegister(X64RegisterType hostReg) const noexcept;

    /**
     * @brief Check if a MIPS register is currently mapped
     */
    bool IsMapped(uint32_t mipsReg) const noexcept;

    /**
     * @brief Check if a host register is currently in use
     */
    bool IsHostRegisterUsed(X64RegisterType hostReg) const noexcept;

    // Register operations
    /**
     * @brief Spill a register to memory (if dirty)
     * @param mipsReg MIPS register to spill
     */
    void SpillRegister(uint32_t mipsReg);

    /**
     * @brief Reload a register from memory
     * @param mipsReg MIPS register to reload
     */
    void ReloadRegister(uint32_t mipsReg);

    /**
     * @brief Spill all dirty registers
     */
    void SpillAllDirtyRegisters();

    /**
     * @brief Reload all registers from memory
     */
    void ReloadAllRegisters();

    /**
     * @brief Flush all register mappings
     * @param writeBack Whether to write back dirty registers
     */
    void FlushRegisters(bool writeBack = true);

    // Register allocation
    /**
     * @brief Allocate a free host register
     * @param regClass Register class to allocate from
     * @param preferredReg Preferred register (optional)
     * @return Allocated register
     */
    X64RegisterType AllocateHostRegister(X64RegisterClass regClass,
                                         X64RegisterType preferredReg = X64RegisterType::Invalid);

    /**
     * @brief Free a host register
     * @param reg Register to free
     */
    void FreeHostRegister(X64RegisterType reg);

    /**
     * @brief Lock a register to prevent spilling
     * @param mipsReg MIPS register to lock
     */
    void LockRegister(uint32_t mipsReg);

    /**
     * @brief Unlock a register
     * @param mipsReg MIPS register to unlock
     */
    void UnlockRegister(uint32_t mipsReg);

    // State management
    /**
     * @brief Save current register state
     */
    void SaveState();

    /**
     * @brief Restore previously saved register state
     */
    void RestoreState();

    /**
     * @brief Synchronize with another register info
     */
    void SynchronizeWith(const CX64RegInfo& other);

    // Query and statistics
    struct AllocationStats {
        size_t totalAllocations = 0;
        size_t totalSpills = 0;
        size_t totalReloads = 0;
        size_t cacheHits = 0;
        size_t cacheMisses = 0;

        void Reset() noexcept { totalAllocations = totalSpills = totalReloads = cacheHits = cacheMisses = 0; }
    };

    const AllocationStats& GetStats() const noexcept { return m_Stats; }
    void ResetStats() noexcept { m_Stats.Reset(); }

    /**
     * @brief Get number of currently mapped registers
     */
    size_t GetMappedRegisterCount() const noexcept;

    /**
     * @brief Get number of dirty registers
     */
    size_t GetDirtyRegisterCount() const noexcept;

    /**
     * @brief Get register usage mask
     */
    uint32_t GetUsedGeneralPurposeMask() const noexcept;
    uint64_t GetUsedFloatingPointMask() const noexcept;

    // Debug and validation
    std::string ToString() const;
    bool Validate() const noexcept;
    void DumpState() const;

    // Configuration
    void SetAllocationStrategy(std::unique_ptr<IRegisterAllocationStrategy> strategy);
    IRegisterAllocationStrategy* GetAllocationStrategy() noexcept { return m_AllocationStrategy.get(); }

    // Direct access (for advanced use)
    CodeBlock& GetCodeBlock() noexcept { return *m_CodeBlock; }
    const CodeBlock& GetCodeBlock() const noexcept { return *m_CodeBlock; }

    CX64Ops& GetAssembler() noexcept { return *m_Assembler; }
    const CX64Ops& GetAssembler() const noexcept { return *m_Assembler; }

private:
    // Internal helpers
    void WriteBackRegister(uint32_t mipsReg);
    void LoadRegisterToHost(uint32_t mipsReg, X64RegisterType hostReg);

    X64RegisterType FindFreeRegister(X64RegisterClass regClass,
                                     X64RegisterType preferred = X64RegisterType::Invalid) const;
    X64RegisterType EvictRegister(X64RegisterClass regClass);

    void UpdateMapping(uint32_t mipsReg, X64RegisterType hostReg, RegisterWidth width, bool isSigned);
    void RemoveMapping(uint32_t mipsReg);

    // State tracking
    struct SavedState {
        std::vector<RegisterMappingInfo> mappings;
        AllocationStats stats;
    };

    CodeBlock* m_CodeBlock = nullptr;
    CX64Ops* m_Assembler = nullptr;

    // Register mappings
    std::array<std::optional<RegisterMappingInfo>, 32> m_MipsToHost; ///< MIPS → host mapping
    std::array<std::optional<uint32_t>, 256> m_HostToMips;           ///< Host → MIPS mapping

    // Register usage tracking
    std::bitset<256> m_HostRegistersUsed; ///< Which host registers are in use
    std::bitset<32> m_LockedRegisters;    ///< Which MIPS registers are locked

    // Allocation strategy
    std::unique_ptr<IRegisterAllocationStrategy> m_AllocationStrategy;

    // State stack for save/restore
    std::vector<SavedState> m_StateStack;

    // Statistics
    AllocationStats m_Stats;

    // Configuration
    bool m_AutoSpill = true;      ///< Automatically spill registers when needed
    bool m_UseCallerSaved = true; ///< Use caller-saved registers
    bool m_UseCalleeSaved = true; ///< Use callee-saved registers

    // Friends
    friend class RegisterAllocationStrategy;
};

// Common allocation strategies
class LinearScanAllocator : public IRegisterAllocationStrategy {
public:
    explicit LinearScanAllocator(CX64RegInfo& regInfo);

    X64RegisterType AllocateRegister(const RegisterSet& regSet, X64RegisterClass regClass,
                                     uint32_t preferredReg = UINT32_MAX) override;
    void FreeRegister(X64RegisterType reg) override;
    bool IsRegisterAvailable(X64RegisterType reg) const override;
    void Reset() override;
    std::string GetName() const override { return "LinearScan"; }

private:
    CX64RegInfo& m_RegInfo;
    std::array<uint32_t, 256> m_RegisterUsageCount;
};

class RoundRobinAllocator : public IRegisterAllocationStrategy {
public:
    explicit RoundRobinAllocator(CX64RegInfo& regInfo);

    X64RegisterType AllocateRegister(const RegisterSet& regSet, X64RegisterClass regClass,
                                     uint32_t preferredReg = UINT32_MAX) override;
    void FreeRegister(X64RegisterType reg) override;
    bool IsRegisterAvailable(X64RegisterType reg) const override;
    void Reset() override;
    std::string GetName() const override { return "RoundRobin"; }

private:
    CX64RegInfo& m_RegInfo;
    uint32_t m_NextGPR = 0;
    uint32_t m_NextXMM = 0;
};

// Exception for allocation errors
class RegisterAllocationError : public std::runtime_error {
public:
    explicit RegisterAllocationError(const std::string& message) : std::runtime_error(message) {}
};

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
