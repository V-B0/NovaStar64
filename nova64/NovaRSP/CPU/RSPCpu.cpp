/**
 * @file RSPCpu.cpp
 * @brief RSP CPU - Build tables, DoRspCycles dispatch.
 */
// SuperNova64 - N64 Emulator
// RSP CPU - Build tables, DoRspCycles dispatch
// Based on Project64's RSPCpu.cpp, modernized for C++20

#include "RSPCpu.hpp"
#include "RspSystem.hpp"
#include "RSPInterpreterOps.hpp"
#include "RSPRomTables.hpp"
#include "../RSPDebugger.hpp"
#include <cstring>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Global tables & state
// ──────────────────────────────────────────────
DWord64  EleSpec[16];
DWord64  Indx[16];
uint32_t RSP_Running   = 0;
uint32_t Mfc0Count     = 0;
uint32_t SemaphoreExit = 0;

// ──────────────────────────────────────────────
// Build_RSP - Populate element selection tables
//
// EleSpec[e][i] gives the *element index* to read from VT
// when the vector instruction specifies element field 'e'.
//
// For e = 0       : identity (0,1,2,3,4,5,6,7)
// For e = 1       : identity
// For e = 2..3    : broadcast pairs
// For e = 4..7    : broadcast quads
// For e = 8..15   : single-element broadcast
// ──────────────────────────────────────────────
void Build_RSP()
{
    // Identity mapping for Indx
    for (int e = 0; e < 16; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            Indx[e].UB[i] = static_cast<uint8_t>(i);
            EleSpec[e].UB[i] = static_cast<uint8_t>(i);
        }
    }

    // e = 0,1: scalar / no broadcast (identity already set)

    // e = 2,3: broadcast even/odd pairs
    // In reversed internal layout: e=2 selects odd internal indices (even N64 elements)
    for (int e = 2; e < 4; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            EleSpec[e].UB[i] = static_cast<uint8_t>((i & 0x6) | (~e & 0x1));
        }
    }

    // e = 4..7: broadcast quads
    // In reversed internal layout: need to invert the sub-index within each quad
    for (int e = 4; e < 8; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            EleSpec[e].UB[i] = static_cast<uint8_t>((i & 0x4) | (~e & 0x3));
        }
    }

    // e = 8..15: single-element broadcast (all elements read from one)
    // In reversed internal layout: N64 element (e&7) maps to internal index 7-(e&7)
    for (int e = 8; e < 16; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            EleSpec[e].UB[i] = static_cast<uint8_t>(7 - (e & 0x7));
        }
    }

    // Byte-reverse the Indx table for big-endian element ordering
    // NOTE: EleSpec is NOT reversed — it uses u16 indices directly
    for (int e = 0; e < 16; e++)
    {
        for (int i = 0; i < 4; i++)
        {
            uint8_t tmp = Indx[e].UB[i];
            Indx[e].UB[i] = Indx[e].UB[7 - i];
            Indx[e].UB[7 - i] = tmp;
        }
    }

    // ──────────────────────────────────────────────
    // Copy hardware-accurate ROM tables (512 entries each)
    // These are burned into the RSP silicon and verified against n64-systemtest.
    // ──────────────────────────────────────────────
    auto& reg = RSPSystem.Registers();
    std::memcpy(reg.m_Reciprocals, RcpRom, sizeof(RcpRom));
    std::memcpy(reg.m_InverseSquareRoots, RsqRom, sizeof(RsqRom));
}

} // namespace Nova64::RSP
