/**
 * @file StateTrace.cpp
 * @brief CPU/memory state tracing for cross-engine divergence testing.
 */

#include "StateTrace.hpp"
#include "MainSYS.hpp"
#include "MIPSY/Register.hpp"
#include "MIPSY/Vmem.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace Nova64::Trace {

namespace {

// FNV-1a. Chosen for being trivially reproducible by the compare script and by
// any future reimplementation that needs to emit a matching trace.
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

inline void HashBytes(uint64_t& h, const void* data, size_t len)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

inline void HashU64(uint64_t& h, uint64_t v)
{
    HashBytes(h, &v, sizeof(v));
}

struct TraceState {
    bool          enabled   = false;
    bool          hashRdram = false;
    bool          breakdownCp0 = false;
    bool          events = false;
    uint64_t      eventSeq = 0;
    uint64_t      maxFrames = 0;
    std::ofstream out;
};

TraceState& State()
{
    static TraceState state;
    static bool initialised = false;

    if (!initialised) {
        initialised = true;

        const char* path = std::getenv("NOVA64_TRACE_STATE");
        if (path && *path) {
            state.out.open(path, std::ios::trunc);
            if (state.out) {
                state.enabled = true;

                const char* rdram = std::getenv("NOVA64_TRACE_RDRAM");
                state.hashRdram = rdram && *rdram && *rdram != '0';

                const char* cp0 = std::getenv("NOVA64_TRACE_CP0");
                state.breakdownCp0 = cp0 && *cp0 && *cp0 != '0';

                const char* evs = std::getenv("NOVA64_TRACE_EVENTS");
                state.events = evs && *evs && *evs != '0';

                if (const char* limit = std::getenv("NOVA64_TRACE_FRAMES")) {
                    state.maxFrames = std::strtoull(limit, nullptr, 10);
                }

                state.out << "# nova64 state trace v2\n"
                          << "# fields: frame count pc gpr hilo fpr cp0 rdram\n"
                          << "# count is CP0 Count at the sample point. It is an ALIGNMENT check,\n"
                          << "#   not a compared field: if two runs disagree on count at the same\n"
                          << "#   frame they were sampled at different emulated instants, and any\n"
                          << "#   other difference that frame is inconclusive rather than a bug.\n"
                          << "# cp0 excludes Count/Random for the same reason.\n"
                          << "# rdram is '-' unless NOVA64_TRACE_RDRAM=1\n";
            } else {
                std::fprintf(stderr,
                    "[trace] could not open NOVA64_TRACE_STATE path '%s'\n", path);
            }
        }
    }
    return state;
}

} // namespace

EventCounters& Counters()
{
    static EventCounters counters;
    return counters;
}

void RecordEvent(const char* kind, uint64_t count)
{
    TraceState& state = State();
    if (!state.enabled || !state.events) return;

    char line[96];
    std::snprintf(line, sizeof(line), "#seq %llu %s %llu\n",
                  static_cast<unsigned long long>(state.eventSeq++),
                  kind,
                  static_cast<unsigned long long>(count));
    state.out << line;
}

bool IsEnabled()
{
    return State().enabled;
}

void RecordFrame(const CN64System& system, uint64_t frameNumber)
{
    TraceState& state = State();
    if (!state.enabled) return;
    if (state.maxFrames && frameNumber >= state.maxFrames) return;

    const CRegisters* regs = system.GetRegisters();
    if (!regs) return;

    uint64_t gpr = kFnvOffset;
    for (uint64_t r : regs->m_GPR) HashU64(gpr, r);

    uint64_t hilo = kFnvOffset;
    HashU64(hilo, regs->m_HI);
    HashU64(hilo, regs->m_LO);

    uint64_t fpr = kFnvOffset;
    for (uint64_t r : regs->m_FPR) HashU64(fpr, r);

    // Deliberately excludes COUNT and RANDOM: both advance on cycle accounting
    // that legitimately differs between execution engines, so including them
    // would report divergence on every frame and hide real mismatches.
    uint64_t cp0 = kFnvOffset;
    HashU64(cp0, regs->INDEX_REGISTER);
    HashU64(cp0, regs->WIRED_REGISTER);
    HashU64(cp0, regs->BAD_VADDR_REGISTER);
    HashU64(cp0, regs->COMPARE_REGISTER);
    HashU64(cp0, regs->EPC_REGISTER);
    HashU64(cp0, regs->CONFIG_REGISTER);
    HashU64(cp0, regs->ERROREPC_REGISTER);
    HashU64(cp0, regs->TAGLO_REGISTER);
    HashU64(cp0, regs->TAGHI_REGISTER);

    std::string rdramField = "-";
    if (state.hashRdram) {
        if (auto* mem = const_cast<CN64System&>(system).GetMemory()) {
            if (uint8_t* ram = mem->Rdram()) {
                uint64_t h = kFnvOffset;
                HashBytes(h, ram, mem->RdramSize());
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%016llx",
                              static_cast<unsigned long long>(h));
                rdramField = buf;
            }
        }
    }

    char line[224];
    std::snprintf(line, sizeof(line),
        "%llu %016llx %016llx %016llx %016llx %016llx %016llx %s\n",
        static_cast<unsigned long long>(frameNumber),
        static_cast<unsigned long long>(regs->COUNT_REGISTER),
        static_cast<unsigned long long>(regs->m_PROGRAM_COUNTER),
        static_cast<unsigned long long>(gpr),
        static_cast<unsigned long long>(hilo),
        static_cast<unsigned long long>(fpr),
        static_cast<unsigned long long>(cp0),
        rdramField.c_str());

    state.out << line;

    // Companion breakdown for pinpointing which CP0 register diverged. Written
    // as a comment so the compare script ignores it; diff these lines by hand
    // once a run has been confirmed aligned.
    if (state.breakdownCp0) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
            "#cp0 %llu index=%llx wired=%llx badv=%llx compare=%llx epc=%llx "
            "config=%llx errorepc=%llx taglo=%llx taghi=%llx\n",
            static_cast<unsigned long long>(frameNumber),
            static_cast<unsigned long long>(regs->INDEX_REGISTER),
            static_cast<unsigned long long>(regs->WIRED_REGISTER),
            static_cast<unsigned long long>(regs->BAD_VADDR_REGISTER),
            static_cast<unsigned long long>(regs->COMPARE_REGISTER),
            static_cast<unsigned long long>(regs->EPC_REGISTER),
            static_cast<unsigned long long>(regs->CONFIG_REGISTER),
            static_cast<unsigned long long>(regs->ERROREPC_REGISTER),
            static_cast<unsigned long long>(regs->TAGLO_REGISTER),
            static_cast<unsigned long long>(regs->TAGHI_REGISTER));
        state.out << detail;

        const EventCounters& c = Counters();
        char ev[160];
        std::snprintf(ev, sizeof(ev),
            "#ev %llu timerDone=%llu idleSkip=%llu timerChecks=%llu\n",
            static_cast<unsigned long long>(frameNumber),
            static_cast<unsigned long long>(c.timerDone),
            static_cast<unsigned long long>(c.idleSkip),
            static_cast<unsigned long long>(c.timerChecks));
        state.out << ev;
    }

    state.out.flush();   // a crash mid-run should still leave a usable trace
}

void Shutdown()
{
    TraceState& state = State();
    if (state.out.is_open()) state.out.close();
    state.enabled = false;
}

} // namespace Nova64::Trace
