/**
 * @file StateTrace.hpp
 * @brief CPU/memory state tracing for cross-engine divergence testing.
 *
 * Writes a digest of architectural state once per VI frame so two runs of the
 * same ROM can be diffed against each other. The intended use is validating one
 * execution engine against another (interpreter vs recompiler, or a replacement
 * implementation vs the current one): run the same ROM twice, then find the
 * first frame whose digests disagree.
 *
 * Enabled only when NOVA64_TRACE_STATE is set, and writes nothing otherwise, so
 * it is safe to leave compiled in.
 *
 *   NOVA64_TRACE_STATE=<path>   write the trace to <path>
 *   NOVA64_TRACE_RDRAM=1        also hash RDRAM each frame (slow; off by default)
 *   NOVA64_TRACE_FRAMES=<n>     stop tracing after n frames (0 = unlimited)
 *   NOVA64_TRACE_CP0=1          also emit a per-register CP0 breakdown (as
 *                               comment lines) to pinpoint which one diverged
 */

#pragma once

#include <cstdint>

class CN64System;

namespace Nova64::Trace {

/// Counters for the events that decide *when* emulated time advances. The two
/// execution engines service timers at different points (the interpreter on
/// backward branches, the recompiler at block exits), so comparing these
/// between a `--interpreter` and a `--recompiler` run localises which mechanism
/// is responsible for a COUNT divergence.
struct EventCounters {
    uint64_t timerDone   = 0;   ///< SystemTimer::TimerDone() dispatches
    uint64_t idleSkip    = 0;   ///< idle/permanent-loop fast-forwards
    uint64_t timerChecks = 0;   ///< points where the timer was eligible to fire
};

EventCounters& Counters();

/// Appends one high-resolution event record (NOVA64_TRACE_EVENTS=1 only).
/// Used to localise a COUNT divergence to the interval between two timer
/// events, which per-frame sampling is too coarse to resolve.
void RecordEvent(const char* kind, uint64_t count);

/// True when NOVA64_TRACE_STATE is set. Cheap enough to call per frame.
bool IsEnabled();

/// Appends one record for the current frame. No-op unless tracing is enabled.
/// Call once per VI frame, after the frame's work is done.
void RecordFrame(const CN64System& system, uint64_t frameNumber);

/// Flushes and closes the trace file. Safe to call when tracing is disabled.
void Shutdown();

} // namespace Nova64::Trace
