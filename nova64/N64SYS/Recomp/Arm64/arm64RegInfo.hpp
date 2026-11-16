/**
 * @file arm64RegInfo.hpp
 * @brief AArch64 register information / state management for the recompiler
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 *
 * AArch64 counterpart of CX64RegInfo, declared as Nova64::Recompiler::Arm::ArmRegInfo
 * to match the forward declarations and friend declarations the rest of the
 * engine already provides (see MIPSY/Vmem.hpp and SysGlobals.hpp).
 *
 * Design note
 * -----------
 * Nova64's dynarec is *callback-threaded*: each MIPS instruction is translated
 * into a stub that calls an interpreter routine which reads and writes the
 * architectural MIPS registers directly in the CRegisters block (reached
 * through the pinned base register x19).  The guest GPRs are therefore never
 * resident in host registers across instruction boundaries, so the "register
 * allocator" reduces to lightweight bookkeeping with no host-register code to
 * emit on flush.  These methods mirror the x64 surface so the shared
 * opcode-dispatch logic ports across unchanged.
 */

#pragma once

#if defined(__aarch64__) || defined(_M_ARM64)

#include "../RegBase.hpp"
#include "arm64ops.hpp"
#include <cstdint>
#include <array>
#include <optional>

namespace Nova64 {
namespace Recompiler {

class CodeBlock;
class CArm64Ops;

namespace Arm {

/// Register-mapping bookkeeping for the AArch64 backend; guest GPRs stay in memory, so no host code is emitted.
class ArmRegInfo final : public CRegBase {
public:
    ArmRegInfo(CodeBlock& codeBlock, CArm64Ops& assembler) : m_CodeBlock(&codeBlock), m_Assembler(&assembler) {
        m_MipsToHost.fill(std::nullopt);
    }
    ~ArmRegInfo() override = default;

    // --- Mapping bookkeeping (no host code emitted in the callback model) ---
    void MapRegister(uint32_t mipsReg, RegisterWidth /*width*/, bool /*isSigned*/ = false) {
        if (mipsReg < 32) m_MipsToHost[mipsReg] = mipsReg; // identity placeholder
    }
    void UnmapRegister(uint32_t mipsReg, bool /*writeBack*/ = true, bool /*force*/ = false) {
        if (mipsReg < 32) m_MipsToHost[mipsReg] = std::nullopt;
    }
    bool IsMapped(uint32_t mipsReg) const noexcept { return mipsReg < 32 && m_MipsToHost[mipsReg].has_value(); }

    // Flush all mappings.  Guest GPRs already live in memory, so there is no
    // write-back code to emit — we only clear the tracking state.
    void FlushRegisters(bool /*writeBack*/ = true) {
        for (uint32_t i = 0; i < 32; ++i) {
            m_MipsToHost[i] = std::nullopt;
        }
    }

    void SpillRegister(uint32_t /*mipsReg*/) {}
    void ReloadRegister(uint32_t /*mipsReg*/) {}
    void SaveState() {}
    void RestoreState() {}
    void SynchronizeWith(const ArmRegInfo& /*other*/) {}

    CodeBlock& GetCodeBlock() noexcept { return *m_CodeBlock; }
    CArm64Ops& GetAssembler() noexcept { return *m_Assembler; }

private:
    CodeBlock* m_CodeBlock = nullptr;
    CArm64Ops* m_Assembler = nullptr;
    std::array<std::optional<uint32_t>, 32> m_MipsToHost;
};

} // namespace Arm
} // namespace Recompiler
} // namespace Nova64

#endif // __aarch64__ || _M_ARM64
