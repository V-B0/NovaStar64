/**
 * @file RSPCpu.hpp
 * @brief RSP CPU Interface.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "RSPOpcode.hpp"
#include "RSPRegisterHandler.hpp"
#include "RSPTypes.hpp"
#include <cstdint>
#include <memory>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Element broadcast / index tables
// Used by vector ops: VT element selection
// EleSpec[e][i] gives the element index to read from VT
// ──────────────────────────────────────────────
extern DWord64 EleSpec[16];
extern DWord64 Indx[16];

// ──────────────────────────────────────────────
// Runtime state
// ──────────────────────────────────────────────
extern uint32_t RSP_Running;

// ──────────────────────────────────────────────
// Build / initialize RSP CPU subsystem
// Populates element selection tables
// ──────────────────────────────────────────────
void Build_RSP();

// ──────────────────────────────────────────────
// Performance counters
// ──────────────────────────────────────────────
extern uint32_t Mfc0Count;
extern uint32_t SemaphoreExit;

} // namespace Nova64::RSP
