// SuperNova64 - N64 Emulator
// RSP Debugger Interface Implementation
// Global debugger instance and null debugger

#include "RSPDebugger.hpp"

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Global debugger instance
// nullptr = debugging disabled
// Set externally by the host application
// ──────────────────────────────────────────────

IRSPDebugger* g_RSPDebugger = nullptr;

} // namespace Nova64::RSP
