/**
 * @file RspJit.hpp
 * @brief RSP recompiler: block discovery, code cache and dispatch.
 *
 * The RSP is a small MIPS-like core with 1024 instructions of IMEM, so the
 * recompiler is block-at-a-time and lazy: a block is compiled the first time
 * execution reaches its start, and the code cache is a flat table indexed by
 * IMEM address. Execution is checked against the interpreter instruction for
 * instruction by `nova64 --rsp-selftest`.
 *
 * Anything a backend doesn't translate natively is executed by calling the
 * interpreter's implementation of that one instruction, so the recompiler is
 * exact by construction and only gets faster as more is translated.
 */

#pragma once
#include "RspJitBackend.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace Nova64::RSP {
class CRSPSystem;
}

namespace Nova64::RSP::Jit {

class RspJit {
public:
    explicit RspJit(CRSPSystem& system);
    ~RspJit();

    RspJit(const RspJit&) = delete;
    RspJit& operator=(const RspJit&) = delete;

    /// True if this build has a code emitter for the CPU it is running on.
    static bool Available();

    /// Run from the current SP_PC until the RSP halts. Same contract as
    /// CRSPSystem::ExecuteOps with an unlimited cycle budget.
    void Run();

    /// IMEM was written (by an SP DMA). The compiled code may be stale.
    void NotifyImemWritten() { m_ImemDirty = true; }

    /// Drop every compiled block.
    void Flush();

    struct Stats {
        uint64_t blocksCompiled  = 0;
        uint64_t blocksRun       = 0;
        uint64_t flushes         = 0;
        uint64_t setSwitches     = 0;   ///< Times execution moved to another IMEM image's code
        uint64_t interpreterRuns = 0;   ///< Tasks finished by the interpreter after a compile failure
    };
    [[nodiscard]] const Stats& GetStats() const { return m_Stats; }

private:
    // A game switches between a handful of microcodes (and overlays within them) many
    // times a frame. Code is kept per IMEM image, so going back to one already seen
    // costs a lookup instead of a recompile.
    struct CodeSet {
        uint64_t                  hash = 0;
        uint64_t                  lastUse = 0;
        bool                      valid = false;
        std::array<BlockFn, 1024> blocks{};   // indexed by IMEM address / 4
    };
    static constexpr size_t kCodeSets = 16;

    CodeSet* SelectSet(uint64_t imemHash);
    void     ClearSet(CodeSet& set);

    BlockFn Lookup(uint32_t pc);
    bool    Discover(uint32_t startPc, Block& block) const;
    uint32_t Fetch(uint32_t pc) const;
    uint64_t HashImem() const;

    CRSPSystem&                m_System;
    std::unique_ptr<Backend>   m_Backend;
    std::array<CodeSet, kCodeSets> m_Sets{};
    CodeSet*                   m_Active{nullptr};
    uint64_t                   m_Clock{0};
    bool                       m_ImemDirty{false};
    Stats                      m_Stats;
};

} // namespace Nova64::RSP::Jit
