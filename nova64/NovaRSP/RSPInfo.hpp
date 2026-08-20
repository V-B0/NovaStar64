/**
 * @file RSPInfo.hpp
 * @brief RSP Info / Initialization.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include "CPU/RspSystem.hpp"

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// RSP Module Lifecycle
// ──────────────────────────────────────────────

// Initialize RSP system with hardware register pointers
void InitializeRSP(RSP_INFO& rspInfo);

// Called when RSP plugin DLL is loaded (one-time setup)
void RspPluginLoaded();

// Called when ROM is opened (per-game setup)
void RspRomOpened();

// Called when ROM is closed (cleanup per-game state)
void RspRomClosed();

// Free all RSP resources
void FreeRSP();

// Execute RSP until BREAK/halt (main entry point called when halt is cleared)
// Returns the number of cycles consumed (0 = HLE handled instantly)
uint32_t DoRspCycles(uint32_t cycles);

} // namespace Nova64::RSP
