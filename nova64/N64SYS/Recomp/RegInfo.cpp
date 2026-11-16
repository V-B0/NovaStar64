/**
 * @file RegInfo.cpp
 * @brief Implementation of platform-specific register information
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "RegInfo.hpp"
#include <sstream>

namespace Nova64 {
namespace Recompiler {

// --- PlatformImpl - PImpl for platform-specific data ---

class CRegInfo::PlatformImpl {
public:
    PlatformType detectedPlatform = PlatformType::Unsupported;

    PlatformImpl() : detectedPlatform(CRegInfo::DetectPlatform()) {}
};

// --- CRegInfo Implementation ---

CRegInfo::CRegInfo(std::unique_ptr<IRegisterAllocator> allocator) : m_Allocator(std::move(allocator)) {}

CRegInfo::~CRegInfo() = default;

PlatformType CRegInfo::GetPlatformType() const {
#if defined(NOVA64_ARCH_X64) || defined(_M_X64) || defined(__x86_64__)
    return PlatformType::x64;
#elif defined(NOVA64_ARCH_X86) || defined(_M_IX86) || defined(__i386__)
    return PlatformType::x86;
#elif defined(NOVA64_ARCH_ARM64) || defined(_M_ARM64) || defined(__aarch64__)
    return PlatformType::ARM64;
#elif defined(NOVA64_ARCH_ARM32) || defined(_M_ARM) || defined(__arm__)
    return PlatformType::ARM32;
#elif defined(NOVA64_ARCH_RISCV64)
    return PlatformType::RISCV64;
#elif defined(NOVA64_ARCH_RISCV32)
    return PlatformType::RISCV32;
#else
    return PlatformType::Unsupported;
#endif
}

std::string CRegInfo::GetPlatformName() const {
    switch (GetPlatformType()) {
    case PlatformType::x86:     return "x86";
    case PlatformType::x64:     return "x64";
    case PlatformType::ARM32:   return "ARM32";
    case PlatformType::ARM64:   return "ARM64";
    case PlatformType::RISCV32: return "RISC-V 32";
    case PlatformType::RISCV64: return "RISC-V 64";
    default:                    return "Unsupported";
    }
}

size_t CRegInfo::GetNumGeneralPurposeRegisters() const {
    switch (GetPlatformType()) {
    case PlatformType::x86:     return 8;
    case PlatformType::x64:     return 16;
    case PlatformType::ARM32:   return 16;
    case PlatformType::ARM64:   return 31;
    case PlatformType::RISCV32: return 32;
    case PlatformType::RISCV64: return 32;
    default:                    return 0;
    }
}

size_t CRegInfo::GetNumFloatRegisters() const {
    switch (GetPlatformType()) {
    case PlatformType::x86:     return 8;
    case PlatformType::x64:     return 16;
    case PlatformType::ARM32:   return 32;
    case PlatformType::ARM64:   return 32;
    case PlatformType::RISCV32: return 32;
    case PlatformType::RISCV64: return 32;
    default:                    return 0;
    }
}

size_t CRegInfo::GetNumVectorRegisters() const {
    switch (GetPlatformType()) {
    case PlatformType::x86:   return 8;  // XMM0-7
    case PlatformType::x64:   return 16; // XMM0-15
    case PlatformType::ARM64: return 32; // V0-31
    default:                  return 0;
    }
}

size_t CRegInfo::GetHostRegisterSize() const {
    switch (GetPlatformType()) {
    case PlatformType::x86:     return 32;
    case PlatformType::x64:     return 64;
    case PlatformType::ARM32:   return 32;
    case PlatformType::ARM64:   return 64;
    case PlatformType::RISCV32: return 32;
    case PlatformType::RISCV64: return 64;
    default:                    return 0;
    }
}

bool CRegInfo::CanAllocateRegister(uint32_t /*regClass*/) const {
    return GetPlatformType() != PlatformType::Unsupported;
}

uint32_t CRegInfo::GetAvailableRegisterMask() const {
    switch (GetPlatformType()) {
    case PlatformType::x64:   return 0xFFFF;     // 16 registers
    case PlatformType::x86:   return 0xFF;       // 8 registers
    case PlatformType::ARM64: return 0x7FFFFFFF; // 31 registers
    default:                  return 0;
    }
}

uint32_t CRegInfo::GetCallerSavedMask() const {
    switch (GetPlatformType()) {
    case PlatformType::x64:
        // Windows x64: RAX, RCX, RDX, R8-R11
        return 0x0F07;
    case PlatformType::x86:
        // EAX, ECX, EDX
        return 0x07;
    default: return 0;
    }
}

uint32_t CRegInfo::GetCalleeSavedMask() const {
    switch (GetPlatformType()) {
    case PlatformType::x64:
        // Windows x64: RBX, RBP, RDI, RSI, R12-R15
        return 0xF0F8;
    case PlatformType::x86:
        // EBX, EBP, ESI, EDI
        return 0xF8;
    default: return 0;
    }
}

uint32_t CRegInfo::GetArgumentRegistersMask() const {
    switch (GetPlatformType()) {
    case PlatformType::x64:
        // Windows x64: RCX, RDX, R8, R9
        return 0x0306;
    default: return 0;
    }
}

uint32_t CRegInfo::GetReturnValueRegistersMask() const {
    switch (GetPlatformType()) {
    case PlatformType::x64:
    case PlatformType::x86: return 0x01; // RAX/EAX
    default:                return 0;
    }
}

std::string CRegInfo::RegisterToString(uint32_t regIndex, bool isFloat) const {
    if (isFloat) {
        return "FPR" + std::to_string(regIndex);
    }

    if (GetPlatformType() == PlatformType::x64) {
        static const char* names[] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                      "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
        if (regIndex < 16) return names[regIndex];
    } else if (GetPlatformType() == PlatformType::x86) {
        static const char* names[] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
        if (regIndex < 8) return names[regIndex];
    }

    return "REG" + std::to_string(regIndex);
}

std::string CRegInfo::GetABIName() const {
    switch (GetPlatformType()) {
    case PlatformType::x64:
#ifdef _WIN32
        return "Windows x64";
#else
        return "System V AMD64";
#endif
    case PlatformType::x86:   return "cdecl";
    case PlatformType::ARM64: return "AAPCS64";
    case PlatformType::ARM32: return "AAPCS";
    default:                  return "Unknown";
    }
}

bool CRegInfo::SupportsFeature(std::string_view /*feature*/) const {
    // No optional host features are probed.
    return false;
}

void CRegInfo::SetRegisterAllocator(std::unique_ptr<IRegisterAllocator> allocator) {
    m_Allocator = std::move(allocator);
}

IRegisterAllocator* CRegInfo::GetAllocator() noexcept { return m_Allocator.get(); }

const IRegisterAllocator* CRegInfo::GetAllocator() const noexcept { return m_Allocator.get(); }

void CRegInfo::ResetRegisterState(RegisterSet& /*regSet*/) { m_Stats.Reset(); }

void CRegInfo::SaveRegisterState(RegisterSet& /*regSet*/) {
    // No-op.
}

void CRegInfo::RestoreRegisterState(RegisterSet& /*regSet*/) {
    // No-op.
}

// --- Static methods ---

PlatformType CRegInfo::DetectPlatform() noexcept {
#if defined(NOVA64_ARCH_X64) || defined(_M_X64) || defined(__x86_64__)
    return PlatformType::x64;
#elif defined(NOVA64_ARCH_X86) || defined(_M_IX86) || defined(__i386__)
    return PlatformType::x86;
#elif defined(NOVA64_ARCH_ARM64) || defined(_M_ARM64) || defined(__aarch64__)
    return PlatformType::ARM64;
#elif defined(NOVA64_ARCH_ARM32) || defined(_M_ARM) || defined(__arm__)
    return PlatformType::ARM32;
#else
    return PlatformType::Unsupported;
#endif
}

bool CRegInfo::IsPlatformSupported(PlatformType platform) noexcept {
    switch (platform) {
    case PlatformType::x64:
    case PlatformType::x86:
    case PlatformType::ARM64: return true;
    default:                  return false;
    }
}

std::string CRegInfo::GetPlatformDescription(PlatformType platform) {
    switch (platform) {
    case PlatformType::x86:     return "Intel/AMD x86 (32-bit)";
    case PlatformType::x64:     return "Intel/AMD x86-64 (64-bit)";
    case PlatformType::ARM32:   return "ARM (32-bit)";
    case PlatformType::ARM64:   return "ARM (64-bit / AArch64)";
    case PlatformType::RISCV32: return "RISC-V (32-bit)";
    case PlatformType::RISCV64: return "RISC-V (64-bit)";
    default:                    return "Unsupported platform";
    }
}

std::unique_ptr<CRegInfo> CRegInfo::CreateForCurrentPlatform() { return std::make_unique<CRegInfo>(); }

std::unique_ptr<CRegInfo> CRegInfo::CreateForPlatform(PlatformType /*platform*/) {
    // All platforms share one CRegInfo; the platform is detected at compile time.
    return std::make_unique<CRegInfo>();
}

// --- Free functions ---

static PlatformInfo s_CurrentPlatformInfo(CRegInfo::DetectPlatform(),
#if defined(_M_X64) || defined(__x86_64__)
                                          "x64", "Intel/AMD x86-64 (64-bit)", true, true
#elif defined(_M_IX86) || defined(__i386__)
                                          "x86", "Intel/AMD x86 (32-bit)", false, true
#elif defined(_M_ARM64) || defined(__aarch64__)
                                          "ARM64", "ARM AArch64 (64-bit)", true, true
#else
                                          "Unknown", "Unsupported platform", false, false
#endif
);

const PlatformInfo& GetCurrentPlatformInfo() noexcept { return s_CurrentPlatformInfo; }

bool CheckPlatformFeature(std::string_view /*feature*/) noexcept { return false; }

// --- UnsupportedPlatformException ---

UnsupportedPlatformException::UnsupportedPlatformException(PlatformType platform) :
    std::runtime_error("Unsupported platform: " + CRegInfo::GetPlatformDescription(platform)), m_Platform(platform) {}

// --- RegisterCustomPlatform ---

void RegisterCustomPlatform(PlatformType /*type*/, std::function<std::unique_ptr<IRegInfo>()> /*factory*/) {
    // No-op: custom platforms are not supported.
}

} // namespace Recompiler
} // namespace Nova64
