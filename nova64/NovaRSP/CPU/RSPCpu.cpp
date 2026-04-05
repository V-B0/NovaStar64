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
#include "../RSPDebugger.hpp"
#include <cstring>
#include <cmath>

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

    // e = 2,3: broadcast even/odd pairs (0,0,2,2,4,4,6,6) or (1,1,3,3,5,5,7,7)
    for (int e = 2; e < 4; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            EleSpec[e].UB[i] = static_cast<uint8_t>((i & 0x6) | (e & 0x1));
        }
    }

    // e = 4..7: broadcast quads (0,0,0,0,4,4,4,4) etc.
    for (int e = 4; e < 8; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            EleSpec[e].UB[i] = static_cast<uint8_t>((i & 0x4) | (e & 0x3));
        }
    }

    // e = 8..15: single-element broadcast (all elements read from one)
    for (int e = 8; e < 16; e++)
    {
        for (int i = 0; i < 8; i++)
        {
            EleSpec[e].UB[i] = static_cast<uint8_t>(e & 0x7);
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
    // Build reciprocal ROM table (512 entries)
    // Hardware-accurate: entry[i] = floor(2^34 / (i + 512)) - 2^16
    // ──────────────────────────────────────────────
    auto& reg = RSPSystem.Registers();
    for (int i = 0; i < 512; i++)
    {
        uint32_t b = static_cast<uint32_t>((uint64_t(1) << 34) / (i + 512));
        reg.m_Reciprocals[i] = static_cast<uint16_t>(b & 0xFFFF);
    }

    // ──────────────────────────────────────────────
    // Build inverse square root ROM table (512 entries)
    // Even entries: floor(2^17 * sqrt(2^31 / (i/2 + 256))) - 2^16
    // Odd entries:  floor(2^17 * sqrt(2^32 / ((i-1)/2 + 256))) - 2^16
    // ──────────────────────────────────────────────
    for (int i = 0; i < 512; i++)
    {
        uint32_t a = (i >> 1) + 256;
        if ((i & 1) == 0)
        {
            // Even index: lookup for even shifts
            double val = 1.0 / sqrt(static_cast<double>(a) / 8192.0);
            val = floor(val * 65536.0);
            reg.m_InverseSquareRoots[i] = static_cast<uint16_t>(static_cast<uint32_t>(val) & 0xFFFF);
        }
        else
        {
            // Odd index: lookup for odd shifts
            double val = 1.0 / sqrt(static_cast<double>(a) / 4096.0);
            val = floor(val * 65536.0);
            reg.m_InverseSquareRoots[i] = static_cast<uint16_t>(static_cast<uint32_t>(val) & 0xFFFF);
        }
    }
}

} // namespace Nova64::RSP
