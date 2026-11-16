/**
 * @file x64RegInfo.cpp
 * @brief x64 register information and allocation implementation
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * Implementation of CX64RegInfo for managing MIPS-to-x64 register mappings.
 */

#if defined(__amd64__) || defined(_M_X64)

#include "x64RegInfo.hpp"
#include "x64ops.hpp"
#include "../CodeBlock.hpp"
#include "../../../common/log.hpp"

#include <algorithm>
#include <sstream>
#include <cassert>

namespace Nova64 {
namespace Recompiler {

// --- X64RegisterInfo static lookup ---

X64RegisterInfo X64RegisterInfo::GetInfo(X64RegisterType reg) {
    X64RegisterInfo info;
    info.type = reg;

    // Map type to class, size, and properties
    auto idx = static_cast<uint16_t>(reg);

    // 64-bit GPR: RAX..R15
    if (idx <= static_cast<uint16_t>(X64RegisterType::R15)) {
        info.regClass = X64RegisterClass::GeneralPurpose;
        info.size = 8;
        info.index = static_cast<uint8_t>(idx);
        // Volatile (caller-saved) on Windows x64: RAX, RCX, RDX, R8-R11
        bool isVolatile = (idx == 0 || idx == 1 || idx == 2 || idx == 3 || (idx >= 8 && idx <= 11));
        info.isCallerSaved = isVolatile;
        info.isCalleeSaved = !isVolatile;
        info.isVolatile = isVolatile;
        info.isReserved = (idx == 4); // RSP
        static const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
        info.name = names[idx];
        return info;
    }

    // Default: invalid
    info.regClass = X64RegisterClass::GeneralPurpose;
    info.name = "unknown";
    return info;
}

// --- RegisterMappingInfo ---

std::string RegisterMappingInfo::ToString() const {
    std::ostringstream oss;
    oss << "MIPS r" << m_MipsReg << " -> ";
    auto hostInfo = X64RegisterInfo::GetInfo(m_HostReg);
    oss << hostInfo.name;
    if (m_IsDirty) oss << " [dirty]";
    if (m_IsLocked) oss << " [locked]";
    return oss.str();
}

// --- CX64RegInfo Construction / Destruction ---

CX64RegInfo::CX64RegInfo(CodeBlock& codeBlock, CX64Ops& assembler,
                         std::unique_ptr<IRegisterAllocationStrategy> strategy) :
    CRegBase(), m_CodeBlock(&codeBlock), m_Assembler(&assembler), m_AllocationStrategy(std::move(strategy)) {
    m_HostRegistersUsed.reset();
    m_LockedRegisters.reset();
    m_HostToMips.fill(std::nullopt);

    // Mark RSP as used (always reserved)
    m_HostRegistersUsed.set(static_cast<size_t>(X64RegisterType::RSP));

    Log::debug("CX64RegInfo: Created");
}

CX64RegInfo::~CX64RegInfo() { Log::debug("CX64RegInfo: Destroyed"); }

CX64RegInfo::CX64RegInfo(CX64RegInfo&& other) noexcept :
    CRegBase(std::move(other)), m_CodeBlock(other.m_CodeBlock), m_Assembler(other.m_Assembler),
    m_MipsToHost(std::move(other.m_MipsToHost)), m_HostToMips(std::move(other.m_HostToMips)),
    m_HostRegistersUsed(other.m_HostRegistersUsed), m_LockedRegisters(other.m_LockedRegisters),
    m_AllocationStrategy(std::move(other.m_AllocationStrategy)), m_StateStack(std::move(other.m_StateStack)),
    m_Stats(other.m_Stats) {
    other.m_CodeBlock = nullptr;
    other.m_Assembler = nullptr;
}

CX64RegInfo& CX64RegInfo::operator=(CX64RegInfo&& other) noexcept {
    if (this != &other) {
        CRegBase::operator=(std::move(other));
        m_CodeBlock = other.m_CodeBlock;
        m_Assembler = other.m_Assembler;
        m_MipsToHost = std::move(other.m_MipsToHost);
        m_HostToMips = std::move(other.m_HostToMips);
        m_HostRegistersUsed = other.m_HostRegistersUsed;
        m_LockedRegisters = other.m_LockedRegisters;
        m_AllocationStrategy = std::move(other.m_AllocationStrategy);
        m_StateStack = std::move(other.m_StateStack);
        m_Stats = other.m_Stats;
        other.m_CodeBlock = nullptr;
        other.m_Assembler = nullptr;
    }
    return *this;
}

bool CX64RegInfo::operator==(const CX64RegInfo& other) const noexcept {
    for (size_t i = 0; i < 32; ++i) {
        if (m_MipsToHost[i].has_value() != other.m_MipsToHost[i].has_value()) return false;
        if (m_MipsToHost[i].has_value() && other.m_MipsToHost[i].has_value()) {
            if (m_MipsToHost[i].value() != other.m_MipsToHost[i].value()) return false;
        }
    }
    return true;
}

// --- Register Mapping ---

X64RegisterType CX64RegInfo::MapRegister(uint32_t mipsReg, RegisterWidth width, bool isSigned,
                                         X64RegisterType preferredReg) {
    if (mipsReg >= 32) return X64RegisterType::Invalid;

    // If already mapped, return existing mapping
    if (m_MipsToHost[mipsReg].has_value()) {
        auto& mapping = m_MipsToHost[mipsReg].value();
        mapping.SetWidth(width);
        mapping.SetSigned(isSigned);
        m_Stats.cacheHits++;
        return mapping.GetHostReg();
    }

    m_Stats.cacheMisses++;

    X64RegisterType hostReg = FindFreeRegister(X64RegisterClass::GeneralPurpose, preferredReg);
    if (hostReg == X64RegisterType::Invalid) {
        // Need to evict
        hostReg = EvictRegister(X64RegisterClass::GeneralPurpose);
        if (hostReg == X64RegisterType::Invalid) {
            Log::error("CX64RegInfo: Failed to allocate host register");
            return X64RegisterType::Invalid;
        }
    }

    UpdateMapping(mipsReg, hostReg, width, isSigned);
    LoadRegisterToHost(mipsReg, hostReg);
    m_Stats.totalAllocations++;

    return hostReg;
}

void CX64RegInfo::UnmapRegister(uint32_t mipsReg, bool writeBack, bool force) {
    if (mipsReg >= 32) return;
    if (!m_MipsToHost[mipsReg].has_value()) return;

    auto& mapping = m_MipsToHost[mipsReg].value();
    if (mapping.IsLocked() && !force) {
        Log::warningf("CX64RegInfo: Attempt to unmap locked register r%u", mipsReg);
        return;
    }

    if (writeBack && mapping.IsDirty()) {
        WriteBackRegister(mipsReg);
    }

    auto hostReg = mapping.GetHostReg();
    RemoveMapping(mipsReg);
    m_HostRegistersUsed.reset(static_cast<size_t>(hostReg));
}

bool CX64RegInfo::IsHostRegisterUsed(X64RegisterType hostReg) const noexcept {
    return m_HostRegistersUsed.test(static_cast<size_t>(hostReg));
}

// --- Spill / Reload / Flush ---

void CX64RegInfo::SpillRegister(uint32_t mipsReg) {
    if (mipsReg >= 32 || !m_MipsToHost[mipsReg].has_value()) return;

    auto& mapping = m_MipsToHost[mipsReg].value();
    if (mapping.IsDirty()) {
        WriteBackRegister(mipsReg);
        mapping.SetDirty(false);
        m_Stats.totalSpills++;
    }
}

void CX64RegInfo::ReloadRegister(uint32_t mipsReg) {
    if (mipsReg >= 32 || !m_MipsToHost[mipsReg].has_value()) return;

    auto hostReg = m_MipsToHost[mipsReg]->GetHostReg();
    LoadRegisterToHost(mipsReg, hostReg);
    m_Stats.totalReloads++;
}

void CX64RegInfo::FlushRegisters(bool writeBack) {
    for (uint32_t i = 0; i < 32; ++i) {
        if (m_MipsToHost[i].has_value()) {
            UnmapRegister(i, writeBack, true);
        }
    }
}

// --- Allocation ---

X64RegisterType CX64RegInfo::AllocateHostRegister(X64RegisterClass regClass, X64RegisterType preferredReg) {
    X64RegisterType reg = FindFreeRegister(regClass, preferredReg);
    if (reg == X64RegisterType::Invalid && m_AutoSpill) {
        reg = EvictRegister(regClass);
    }
    if (reg != X64RegisterType::Invalid) {
        m_HostRegistersUsed.set(static_cast<size_t>(reg));
    }
    return reg;
}

void CX64RegInfo::FreeHostRegister(X64RegisterType reg) {
    auto idx = static_cast<size_t>(reg);
    if (idx < m_HostToMips.size() && m_HostToMips[idx].has_value()) {
        RemoveMapping(m_HostToMips[idx].value());
    }
    m_HostRegistersUsed.reset(idx);
}

// --- State Management ---

// --- Query / Statistics ---

size_t CX64RegInfo::GetMappedRegisterCount() const noexcept {
    size_t count = 0;
    for (auto& m : m_MipsToHost) {
        if (m.has_value()) ++count;
    }
    return count;
}

size_t CX64RegInfo::GetDirtyRegisterCount() const noexcept {
    size_t count = 0;
    for (auto& m : m_MipsToHost) {
        if (m.has_value() && m->IsDirty()) ++count;
    }
    return count;
}

// --- Internal Helpers ---

void CX64RegInfo::WriteBackRegister(uint32_t mipsReg) {
    if (!m_MipsToHost[mipsReg].has_value() || !m_Assembler) return;

    auto& mapping = m_MipsToHost[mipsReg].value();
    auto hostReg = mapping.GetHostReg();
    auto hostIdx = static_cast<uint8_t>(hostReg);

    auto& a = m_Assembler->GetAssembler();
    (void)a;
    (void)hostIdx;

    // In the interpreter-callback model, register writes happen through the callback
    // functions and registers are flushed before every callback, so nothing is emitted.
    Log::debugf("CX64RegInfo: WriteBack r%u (spill)", mipsReg);
}

void CX64RegInfo::LoadRegisterToHost(uint32_t mipsReg, X64RegisterType hostReg) {
    if (!m_Assembler) return;

    auto hostIdx = static_cast<uint8_t>(hostReg);
    auto& a = m_Assembler->GetAssembler();
    (void)a;
    (void)hostIdx;

    // In the interpreter-callback model, register loads happen through the callback
    // functions, so nothing is emitted.
    Log::debugf("CX64RegInfo: LoadToHost r%u (reload)", mipsReg);
}

X64RegisterType CX64RegInfo::FindFreeRegister(X64RegisterClass regClass, X64RegisterType preferred) const {
    if (regClass == X64RegisterClass::GeneralPurpose) {
        // Check preferred first
        if (preferred != X64RegisterType::Invalid) {
            auto idx = static_cast<size_t>(preferred);
            if (idx < 16 && !m_HostRegistersUsed.test(idx)) {
                return preferred;
            }
        }

        // Try volatile registers first
        static const X64RegisterType volatileGPRs[] = {X64RegisterType::RAX, X64RegisterType::RCX, X64RegisterType::RDX,
                                                       X64RegisterType::R8,  X64RegisterType::R9,  X64RegisterType::R10,
                                                       X64RegisterType::R11};
        for (auto reg : volatileGPRs) {
            auto idx = static_cast<size_t>(reg);
            if (!m_HostRegistersUsed.test(idx)) return reg;
        }

        // Try non-volatile
        static const X64RegisterType nonVolatileGPRs[] = {
            X64RegisterType::RBX, X64RegisterType::RBP, X64RegisterType::RSI, X64RegisterType::RDI,
            X64RegisterType::R12, X64RegisterType::R13, X64RegisterType::R14, X64RegisterType::R15};
        for (auto reg : nonVolatileGPRs) {
            auto idx = static_cast<size_t>(reg);
            if (idx != static_cast<size_t>(X64RegisterType::RSP) && !m_HostRegistersUsed.test(idx)) {
                return reg;
            }
        }
    }

    return X64RegisterType::Invalid;
}

X64RegisterType CX64RegInfo::EvictRegister(X64RegisterClass regClass) {
    if (regClass != X64RegisterClass::GeneralPurpose) return X64RegisterType::Invalid;

    // Find the least recently used unlocked register to evict
    for (uint32_t i = 0; i < 32; ++i) {
        if (m_MipsToHost[i].has_value() && !m_MipsToHost[i]->IsLocked()) {
            auto hostReg = m_MipsToHost[i]->GetHostReg();
            UnmapRegister(i, true);
            return hostReg;
        }
    }

    return X64RegisterType::Invalid;
}

void CX64RegInfo::UpdateMapping(uint32_t mipsReg, X64RegisterType hostReg, RegisterWidth width, bool isSigned) {
    RegisterMappingInfo mapping(mipsReg, hostReg, width, isSigned);
    m_MipsToHost[mipsReg] = mapping;

    auto hostIdx = static_cast<size_t>(hostReg);
    if (hostIdx < m_HostToMips.size()) {
        m_HostToMips[hostIdx] = mipsReg;
    }
    m_HostRegistersUsed.set(hostIdx);
}

void CX64RegInfo::RemoveMapping(uint32_t mipsReg) {
    if (mipsReg >= 32 || !m_MipsToHost[mipsReg].has_value()) return;

    auto hostReg = m_MipsToHost[mipsReg]->GetHostReg();
    auto hostIdx = static_cast<size_t>(hostReg);
    if (hostIdx < m_HostToMips.size()) {
        m_HostToMips[hostIdx] = std::nullopt;
    }
    m_MipsToHost[mipsReg] = std::nullopt;
}

// --- Allocation Strategies ---

} // namespace Recompiler
} // namespace Nova64

#endif // __amd64__ || _M_X64
