/**
 * @file RspPipelineStage.hpp
 * @brief RSP Pipeline Stages.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP {

// Pipeline stage tracking for branch delay slots
// and execution control flow
enum class PipelineStage : uint32_t {
    Normal = 0,

    // Branch delay slot states
    DoDelaySlot,               // About to execute delay slot
    DoDelaySlotExit,           // Delay slot then exit execution
    DoDelaySlotTaskExit,       // Delay slot then exit HLE task
    DelaySlot,                 // Currently in delay slot
    DelaySlotDone,             // Delay slot completed
    DelaySlotDoneBranchTarget, // Delay done, jump to branch target
    DelaySlotExit,             // Delay slot with pending exit
    DelaySlotExitDone,         // Delay slot exit completed
    DelaySlotTaskExit,         // Delay slot HLE task exit
    DelaySlotTaskExitDone,     // HLE task exit completed

    // Jump / single-step
    Jump,           // Unconditional jump pending
    SingleStep,     // Single-step mode
    SingleStepDone, // Single-step completed

    // Recompiler block boundaries
    FinishBlock,        // End of compiled block
    FinishSubBlock,     // End of sub-block
    FinishTaskSubBlock, // End of HLE task sub-block
};

// Alias for backward compatibility with legacy code
using RSPPIPELINE_STAGE = PipelineStage;

} // namespace Nova64::RSP
