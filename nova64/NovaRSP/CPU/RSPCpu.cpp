/**
 * @file RSPCpu.cpp
 * @brief RSP element-selection tables and ROM table setup.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "RSPCpu.hpp"
#include "RspSystem.hpp"
#include "RSPInterpreterOps.hpp"
#include "RSPRomTables.hpp"
#include "../RSPDebugger.hpp"
#include <cstring>

namespace Nova64::RSP {

// --- Global tables & state ---
DWord64 EleSpec[16];
DWord64 Indx[16];
uint32_t RSP_Running = 0;
uint32_t Mfc0Count = 0;
uint32_t SemaphoreExit = 0;

// Fills the VT element-selection tables and loads the reciprocal and
// inverse-square-root ROM tables.
//
// EleSpec[e][i] gives the *element index* to read from VT
// when the vector instruction specifies element field 'e'.
//
// For e = 0       : identity (0,1,2,3,4,5,6,7)
// For e = 1       : identity
// For e = 2..3    : broadcast pairs
// For e = 4..7    : broadcast quads
// For e = 8..15   : single-element broadcast
void Build_RSP() {
    // Start both tables as the identity mapping.
    for (int e = 0; e < 16; e++) {
        for (int i = 0; i < 8; i++) {
            Indx[e].UB[i] = static_cast<uint8_t>(i);
            EleSpec[e].UB[i] = static_cast<uint8_t>(i);
        }
    }

    // e = 2,3: broadcast even/odd pairs
    // In reversed internal layout: e=2 selects odd internal indices (even N64 elements)
    for (int e = 2; e < 4; e++) {
        for (int i = 0; i < 8; i++) {
            EleSpec[e].UB[i] = static_cast<uint8_t>((i & 0x6) | (~e & 0x1));
        }
    }

    // e = 4..7: broadcast quads
    // In reversed internal layout: need to invert the sub-index within each quad
    for (int e = 4; e < 8; e++) {
        for (int i = 0; i < 8; i++) {
            EleSpec[e].UB[i] = static_cast<uint8_t>((i & 0x4) | (~e & 0x3));
        }
    }

    // e = 8..15: single-element broadcast (all elements read from one)
    // In reversed internal layout: N64 element (e&7) maps to internal index 7-(e&7)
    for (int e = 8; e < 16; e++) {
        for (int i = 0; i < 8; i++) {
            EleSpec[e].UB[i] = static_cast<uint8_t>(7 - (e & 0x7));
        }
    }

    // Byte-reverse the Indx table for big-endian element ordering.
    // EleSpec is not reversed: it holds u16 element indices directly.
    for (int e = 0; e < 16; e++) {
        for (int i = 0; i < 4; i++) {
            uint8_t tmp = Indx[e].UB[i];
            Indx[e].UB[i] = Indx[e].UB[7 - i];
            Indx[e].UB[7 - i] = tmp;
        }
    }

    // Copy hardware-accurate ROM tables (512 entries each)
    // These are burned into the RSP silicon and verified against n64-systemtest.
    auto& reg = RSPSystem.Registers();
    std::memcpy(reg.m_Reciprocals, RcpRom, sizeof(RcpRom));
    std::memcpy(reg.m_InverseSquareRoots, RsqRom, sizeof(RsqRom));
}

} // namespace Nova64::RSP
