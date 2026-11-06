/**
 * @file RegInfo.hpp
 * @brief Platform-specific register info type definitions and registry
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This header provides a registry system for platform-specific register information
 * and allocation strategies. It avoids conditional compilation in user code by
 * using a factory pattern and compile-time registration.
 */

#pragma once

#include "RegBase.hpp"
#include <memory>
#include <type_traits>
#include <string>
#include <cstdint>
#include <functional>
#include <stdexcept>

namespace Nova64 {
namespace Recompiler {

// Forward declarations
class IRegInfo;
class IRegisterAllocator;
struct InstructionContext;

/**
 * @enum PlatformType
 * @brief Supported recompiler platforms
 */
enum class PlatformType : uint8_t {
    x86,
    x64,
    ARM32,
    ARM64,
    RISCV32,
    RISCV64,
    Unsupported
};

/**
 * @struct PlatformInfo
 * @brief Information about a compilation platform
 */
struct PlatformInfo {
    PlatformType type;
    std::string name;
    std::string description;
    bool is64Bit;
    bool isSupported;
    
    PlatformInfo(PlatformType t, const char* n, const char* d, bool b64, bool s)
        : type(t), name(n), description(d), is64Bit(b64), isSupported(s) {}
};

/**
 * @class IRegInfo
 * @brief Interface for platform-specific register information
 */
class IRegInfo {
public:
    virtual ~IRegInfo() = default;
    
    // Platform identification
    virtual PlatformType GetPlatformType() const = 0;
    virtual std::string GetPlatformName() const = 0;
    
    // Register information
    virtual size_t GetNumGeneralPurposeRegisters() const = 0;
    virtual size_t GetNumFloatRegisters() const = 0;
    virtual size_t GetNumVectorRegisters() const = 0;
    virtual size_t GetHostRegisterSize() const = 0;  // In bits
    
    // Register allocation
    virtual bool CanAllocateRegister(uint32_t regClass) const = 0;
    virtual uint32_t GetAvailableRegisterMask() const = 0;
    
    // ABI information
    virtual uint32_t GetCallerSavedMask() const = 0;
    virtual uint32_t GetCalleeSavedMask() const = 0;
    virtual uint32_t GetArgumentRegistersMask() const = 0;
    virtual uint32_t GetReturnValueRegistersMask() const = 0;
    
    // Debug information
    virtual std::string RegisterToString(uint32_t regIndex, bool isFloat = false) const = 0;
    virtual std::string GetABIName() const = 0;
    
    // Feature detection
    virtual bool SupportsFeature(std::string_view feature) const = 0;
};

/**
 * @class IRegisterAllocator
 * @brief Interface for platform-specific register allocation strategies
 */
class IRegisterAllocator {
public:
    virtual ~IRegisterAllocator() = default;
    
    virtual void Initialize(RegisterSet& regSet) = 0;
    virtual void AllocateRegisters(RegisterSet& regSet, const InstructionContext& ctx) = 0;
    virtual void SpillRegisters(RegisterSet& regSet) = 0;
    virtual void ReloadRegisters(RegisterSet& regSet) = 0;
    
    virtual size_t GetSpilledRegisterCount() const = 0;
    virtual bool CanSpillRegister(uint32_t regIndex) const = 0;
};

/**
 * @class CRegInfo
 * @brief Main register information and allocation manager
 * 
 * Combines platform-specific register information with allocation strategies
 * and provides a unified interface for the recompiler.
 */
class CRegInfo final : public IRegInfo {
public:
    /**
     * @brief Create register info for current platform
     * @param allocator Optional custom allocator (nullptr for default)
     */
    explicit CRegInfo(std::unique_ptr<IRegisterAllocator> allocator = nullptr);
    ~CRegInfo() override;
    
    // Non-copyable, movable
    CRegInfo(const CRegInfo&) = delete;
    CRegInfo& operator=(const CRegInfo&) = delete;
    CRegInfo(CRegInfo&&) = default;
    CRegInfo& operator=(CRegInfo&&) = default;
    
    // IRegInfo interface
    PlatformType GetPlatformType() const override;
    std::string GetPlatformName() const override;
    size_t GetNumGeneralPurposeRegisters() const override;
    size_t GetNumFloatRegisters() const override;
    size_t GetNumVectorRegisters() const override;
    size_t GetHostRegisterSize() const override;
    bool CanAllocateRegister(uint32_t regClass) const override;
    uint32_t GetAvailableRegisterMask() const override;
    uint32_t GetCallerSavedMask() const override;
    uint32_t GetCalleeSavedMask() const override;
    uint32_t GetArgumentRegistersMask() const override;
    uint32_t GetReturnValueRegistersMask() const override;
    std::string RegisterToString(uint32_t regIndex, bool isFloat = false) const override;
    std::string GetABIName() const override;
    bool SupportsFeature(std::string_view feature) const override;
    
    // Register allocation management
    void SetRegisterAllocator(std::unique_ptr<IRegisterAllocator> allocator);
    IRegisterAllocator* GetAllocator() noexcept;
    const IRegisterAllocator* GetAllocator() const noexcept;
    
    // Register state operations
    void ResetRegisterState(RegisterSet& regSet);
    void SaveRegisterState(RegisterSet& regSet);
    void RestoreRegisterState(RegisterSet& regSet);
    
    // Debug and statistics
    struct Statistics {
        size_t allocations = 0;
        size_t spills = 0;
        size_t reloads = 0;
        size_t conflicts = 0;
        
        void Reset() {
            allocations = spills = reloads = conflicts = 0;
        }
    };
    
    const Statistics& GetStatistics() const noexcept { return m_Stats; }
    void ResetStatistics() noexcept { m_Stats.Reset(); }
    
    // Platform feature queries
    static PlatformType DetectPlatform() noexcept;
    static bool IsPlatformSupported(PlatformType platform) noexcept;
    static std::string GetPlatformDescription(PlatformType platform);
    
    // Factory methods
    static std::unique_ptr<CRegInfo> CreateForCurrentPlatform();
    static std::unique_ptr<CRegInfo> CreateForPlatform(PlatformType platform);
    
private:
    // Platform-specific implementation
    class PlatformImpl;
    std::unique_ptr<PlatformImpl> m_Impl;
    
    // Register allocator
    std::unique_ptr<IRegisterAllocator> m_Allocator;
    
    // Statistics
    Statistics m_Stats;
};

/**
 * @brief Get information about the current platform
 */
const PlatformInfo& GetCurrentPlatformInfo() noexcept;

/**
 * @brief Check if a platform feature is available
 */
bool CheckPlatformFeature(std::string_view feature) noexcept;

/**
 * @brief Exception thrown when platform is unsupported
 */
class UnsupportedPlatformException : public std::runtime_error {
public:
    explicit UnsupportedPlatformException(PlatformType platform);
    PlatformType GetPlatform() const noexcept { return m_Platform; }
    
private:
    PlatformType m_Platform;
};

/**
 * @brief Register a custom platform implementation (for testing/experimental platforms)
 */
void RegisterCustomPlatform(PlatformType type, 
                           std::function<std::unique_ptr<IRegInfo>()> factory);

} // namespace Recompiler
} // namespace Nova64

// Platform detection macro
#if defined(__clang__) || defined(__GNUC__)
    #define NOVA64_COMPILER_CLANG 1
#elif defined(_MSC_VER)
    #define NOVA64_COMPILER_MSVC 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
    #ifndef NOVA64_ARCH_X64
        #define NOVA64_ARCH_X64 1
    #endif
#elif defined(__i386__) || defined(_M_IX86)
    #define NOVA64_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #ifndef NOVA64_ARCH_ARM64
        #define NOVA64_ARCH_ARM64 1
    #endif
#elif defined(__arm__) || defined(_M_ARM)
    #define NOVA64_ARCH_ARM32 1
#elif defined(__riscv) && (__riscv_xlen == 64)
    #define NOVA64_ARCH_RISCV64 1
#elif defined(__riscv) && (__riscv_xlen == 32)
    #define NOVA64_ARCH_RISCV32 1
#endif

// No platform-specific includes in the main header
// They are included in the implementation