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

// --- RSP Module Lifecycle ---

/// Binds the RSP system to the hardware registers. Call after N64 memory is allocated and
/// the SP interface is set up.
void InitializeRSP(RSP_INFO& rspInfo);

/// One-time setup when the RSP plugin is loaded: global state and default settings.
void RspPluginLoaded();

/// Per-game setup when a ROM is opened: applies per-game overrides and creates the persistent HLE context.
void RspRomOpened();

/// Per-game cleanup when the ROM is closed: destroys the HLE context.
void RspRomClosed();

/// Frees all RSP resources (module unload).
void FreeRSP();

/// Entry point called when the RSP halt bit is cleared. Tries HLE dispatch first, otherwise runs the
/// task on the RSP engine until BREAK or halt. Returns `cycles` when HLE handled the task and 0 after
/// the engine ran it.
uint32_t DoRspCycles(uint32_t cycles);

} // namespace Nova64::RSP
