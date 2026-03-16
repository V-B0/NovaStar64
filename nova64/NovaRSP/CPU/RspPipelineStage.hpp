// SuperNova64 - N64 Emulator
// RSP Pipeline Stages - Modernized from Project64's RspPipelineStage.h
// C++20 enum class with descriptive naming

#pragma once
#include <cstdint>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Pipeline stage tracking for branch delay slots
// and execution control flow
// ──────────────────────────────────────────────
enum class PipelineStage : uint32_t {
    Normal = 0,

    // Branch delay slot states
    DoDelaySlot,                     // About to execute delay slot
    DoDelaySlotExit,                 // Delay slot then exit execution
    DoDelaySlotTaskExit,             // Delay slot then exit HLE task
    DelaySlot,                       // Currently in delay slot
    DelaySlotDone,                   // Delay slot completed
    DelaySlotDoneBranchTarget,       // Delay done, jump to branch target
    DelaySlotExit,                   // Delay slot with pending exit
    DelaySlotExitDone,               // Delay slot exit completed
    DelaySlotTaskExit,               // Delay slot HLE task exit
    DelaySlotTaskExitDone,           // HLE task exit completed

    // Jump / single-step
    Jump,                            // Unconditional jump pending
    SingleStep,                      // Single-step mode
    SingleStepDone,                  // Single-step completed

    // Recompiler block boundaries
    FinishBlock,                     // End of compiled block
    FinishSubBlock,                  // End of sub-block
    FinishTaskSubBlock,              // End of HLE task sub-block
};

// Alias for backward compatibility with legacy code
using RSPPIPELINE_STAGE = PipelineStage;

} // namespace Nova64::RSP
