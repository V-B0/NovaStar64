/**
 * @file RspJit.cpp
 * @brief RSP recompiler: block discovery, code cache and dispatch. See RspJit.hpp.
 */

#include "RspJit.hpp"
#include "RspJitDecode.hpp"

#include "../CPU/RSPCpu.hpp"
#include "../CPU/RspMemory.hpp"
#include "../CPU/RspSystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Nova64::RSP::Jit {

namespace {
constexpr size_t kMaxBlockInstructions = 96; // longer straight-line runs are split
constexpr size_t kMaxBranchChain = 32;       // consecutive branches before we give up on a block
} // namespace

namespace {
// NOVA64_RSP_JIT_STATS=1 also lists which instructions still run on the interpreter,
// to show what is worth translating next. Key: primary opcode, then funct (SPECIAL,
// vector ops) or the load/store/move sub-opcode.
const bool g_countFallbacks = std::getenv("NOVA64_RSP_JIT_STATS") != nullptr;
uint64_t g_fallbackCount[64 * 64] = {};

unsigned FallbackKey(uint32_t word) {
    const uint32_t op = word >> 26;
    uint32_t sub = 0;
    switch (op) {
    case 0x00: sub = word & 63; break; // SPECIAL
    case 0x12: sub = (word >> 25) & 1 ? (word & 63) : ((word >> 21) & 31); break;
    case 0x32:
    case 0x3A: sub = (word >> 11) & 31; break; // vector loads and stores
    case 0x10: sub = (word >> 21) & 31; break;
    default:   break;
    }
    return op * 64 + sub;
}

void PrintFallbacks() {
    for (int row = 0; row < 12; ++row) {
        unsigned best = 0;
        for (unsigned k = 1; k < 64 * 64; ++k)
            if (g_fallbackCount[k] > g_fallbackCount[best]) best = k;
        if (g_fallbackCount[best] == 0) break;
        std::fprintf(stderr, "[RSP JIT]   interpreter: op 0x%02X sub %2u: %llu\n", best / 64, best % 64,
                     static_cast<unsigned long long>(g_fallbackCount[best]));
        g_fallbackCount[best] = 0;
    }
}
} // namespace

static void ExecuteOneOnInterpreter(CRSPSystem* sys, uint32_t word) {
    if (g_countFallbacks) ++g_fallbackCount[FallbackKey(word)];
    sys->ExecuteOne(word);
}

static void ExecuteMfc0OnInterpreter(CRSPSystem* sys, uint32_t word) {
    if (g_countFallbacks) ++g_fallbackCount[FallbackKey(word)];
    sys->ExecuteMfc0(word);
}

static void ExecuteMtc0OnInterpreter(CRSPSystem* sys, uint32_t word) {
    if (g_countFallbacks) ++g_fallbackCount[FallbackKey(word)];
    sys->ExecuteMtc0(word);
}

InterpreterHelper InterpreterHelperFor(uint32_t word) {
    if ((word >> 26) == 0x10) { // COP0
        const uint32_t rs = (word >> 21) & 31;
        if (rs == 0) return &ExecuteMfc0OnInterpreter;
        if (rs == 4) return &ExecuteMtc0OnInterpreter;
    }
    return &ExecuteOneOnInterpreter;
}

bool RspJit::Available() {
    static const bool available = CreateBackend() != nullptr;
    return available;
}

RspJit::RspJit(CRSPSystem& system) : m_System(system), m_Backend(CreateBackend()) {}

RspJit::~RspJit() {
    Flush();
    if (std::getenv("NOVA64_RSP_JIT_STATS")) {
        std::fprintf(stderr,
                     "[RSP JIT] %s: %llu blocks compiled, %llu run, %llu code sets recycled, %llu switches between "
                     "IMEM images, %llu tasks finished in the interpreter\n",
                     m_Backend ? m_Backend->Name() : "none", static_cast<unsigned long long>(m_Stats.blocksCompiled),
                     static_cast<unsigned long long>(m_Stats.blocksRun),
                     static_cast<unsigned long long>(m_Stats.flushes),
                     static_cast<unsigned long long>(m_Stats.setSwitches),
                     static_cast<unsigned long long>(m_Stats.interpreterRuns));
        PrintFallbacks();
    }
}

void RspJit::ClearSet(CodeSet& set) {
    for (auto& fn : set.blocks) {
        if (fn && m_Backend) m_Backend->Release(fn);
        fn = nullptr;
    }
    set.valid = false;
}

void RspJit::Flush() {
    for (auto& set : m_Sets) ClearSet(set);
    m_Active = nullptr;
    ++m_Stats.flushes;
}

RspJit::CodeSet* RspJit::SelectSet(uint64_t imemHash) {
    CodeSet* victim = &m_Sets[0];
    for (auto& set : m_Sets) {
        if (set.valid && set.hash == imemHash) {
            set.lastUse = ++m_Clock;
            return &set;
        }
        if (!set.valid) victim = &set; // prefer an empty one
        else if (victim->valid && set.lastUse < victim->lastUse) victim = &set;
    }
    // Recycle the least recently used set.
    if (victim->valid) {
        ClearSet(*victim);
        ++m_Stats.flushes;
    }
    victim->hash = imemHash;
    victim->valid = true;
    victim->lastUse = ++m_Clock;
    return victim;
}

uint32_t RspJit::Fetch(uint32_t pc) const {
    uint32_t word;
    std::memcpy(&word, m_System.m_IMEM + (pc & 0xFFC), sizeof(word));
    return word;
}

uint64_t RspJit::HashImem() const {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t* p = m_System.m_IMEM;
    for (uint32_t i = 0; i < IMEM_SIZE; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, sizeof(v));
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

// Cut one block starting at `startPc`. Follows the RSP's pipeline exactly as the
// interpreter implements it:
//   * a branch or jump runs the instruction after it (its delay slot) and then
//     transfers control;
//   * if that delay slot is itself a branch or jump, the first one is overruled:
//     only its link-register write survives, and the second runs as an ordinary
//     branch with its own delay slot.
bool RspJit::Discover(uint32_t startPc, Block& block) const {
    block = Block{};
    block.startPc = startPc;

    uint32_t pc = startPc;
    for (size_t count = 0; count < kMaxBlockInstructions; ++count) {
        const uint32_t word = Fetch(pc);
        const Flow flow = Classify(word);

        if (flow == Flow::None) {
            block.body.push_back({pc, word});
            pc = (pc + 4) & 0xFFC;
            continue;
        }
        if (flow == Flow::EndsBlock) {
            block.body.push_back({pc, word});
            block.end = Block::End::Fallthrough;
            block.fallPc = (pc + 4) & 0xFFC;
            return true;
        }

        // A branch or jump.
        Insn term{pc, word};
        uint32_t delayPc = (pc + 4) & 0xFFC;
        uint32_t delayWord = Fetch(delayPc);
        size_t chain = 0;
        while (IsControlTransfer(delayWord)) {
            if (++chain > kMaxBranchChain) return false;
            block.body.push_back(term); // lowered as "link register only"
            term = {delayPc, delayWord};
            delayPc = (delayPc + 4) & 0xFFC;
            delayWord = Fetch(delayPc);
        }
        block.terminator = term;
        block.delay = {delayPc, delayWord};
        switch (Classify(term.word)) {
        case Flow::Branch: block.end = Block::End::Branch; break;
        case Flow::Jump:   block.end = Block::End::Jump; break;
        default:           block.end = Block::End::JumpReg; break;
        }
        return true;
    }

    block.end = Block::End::Fallthrough;
    block.fallPc = pc;
    return true;
}

BlockFn RspJit::Lookup(uint32_t pc) {
    BlockFn& slot = m_Active->blocks[(pc & 0xFFC) >> 2];
    if (slot) return slot;
    if (!m_Backend) return nullptr;

    Block block;
    if (!Discover(pc & 0xFFC, block)) return nullptr;
    slot = m_Backend->Compile(block);
    if (slot) ++m_Stats.blocksCompiled;
    return slot;
}

void RspJit::Run() {
    RSP_Running = 1;

    uint32_t& spPc = *m_System.m_SP_PC_REG;

    // The compiled code is only good for the IMEM it was made from.
    m_Active = SelectSet(HashImem());
    m_ImemDirty = false;

    auto& reg = m_System.m_Reg;
    JitContext ctx{};
    ctx.gpr = &reg.m_GPR[0].UW;
    ctx.dmem = m_System.m_DMEM;
    ctx.sys = &m_System;
    ctx.regs = &reg;

    uint32_t pc = spPc & 0xFFC;
    BlockFn* table = m_Active->blocks.data();
    uint64_t ran = 0;
    while (RSP_Running) {
        BlockFn fn = table[pc >> 2];
        if (!fn) {
            fn = Lookup(pc); // compiles it
            if (!fn) {
                // Something this recompiler can't handle. The interpreter picks up
                // exactly where we are and finishes the task.
                spPc = pc;
                ++m_Stats.interpreterRuns;
                m_Stats.blocksRun += ran;
                m_System.ExecuteOps(0xFFFFFFFFu, 0xFFFFFFFFu);
                return;
            }
        }
        pc = fn(&ctx) & 0xFFC;
        ++ran;

        if (m_ImemDirty) {
            // An SP DMA rewrote IMEM while the block ran: carry on with the code
            // that belongs to the new contents.
            CodeSet* next = SelectSet(HashImem());
            if (next != m_Active) ++m_Stats.setSwitches;
            m_Active = next;
            table = m_Active->blocks.data();
            m_ImemDirty = false;
        }
    }
    m_Stats.blocksRun += ran;
    spPc = pc;
}

} // namespace Nova64::RSP::Jit
