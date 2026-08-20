/**
 * @file RSPDebugger.cpp
 * @brief RSP Debugger Interface Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "RSPDebugger.hpp"

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Global debugger instance
// nullptr = debugging disabled
// Set externally by the host application
// ──────────────────────────────────────────────

IRSPDebugger* g_RSPDebugger = nullptr;

} // namespace Nova64::RSP
