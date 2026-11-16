/**
 * @file Microcode.hpp
 * @brief N64 RSP microcode identification system
 *
 * The N64 RSP (Reality Signal Processor) runs task-specific microcode.
 * Graphics microcode variants differ in their GBI (Graphics Binary Interface)
 * command encoding, vertex buffer layout, and feature support. This header
 * defines all known microcode types and their detection signatures.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <array>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Known Microcode Types
// ═══════════════════════════════════════════════════════════════════════════

enum class MicrocodeType : uint8_t {
    Unknown = 0,

    // Fast3D family
    F3D = 1,        // Original Fast3D (Super Mario 64, Wave Race)
    F3DEX = 2,      // Extended F3D with matrix ops
    F3DEX2 = 3,     // Major revision — new command IDs (most common)
    F3DPD = 4,      // Perfect Dark custom F3D variant
    F3DCBFD = 5,    // Conker's Bad Fur Day custom F3D
    F3DTEXA = 6,    // Texture-alpha variant (Yoshis Story)
    F3DLX = 7,      // Low-latency F3D variant
    F3DLX2 = 8,     // Low-latency F3DEX2 variant
    F3DZEX = 9,     // Zelda exclusive F3DEX2 variant (OoT, MM)
    F3DFLX2 = 10,   // Star Fox variant
    F3DGOLDEN = 11, // GoldenEye F3D variant

    // Line3D family (wireframe variants)
    L3D = 12,    // Line3D variant of F3D
    L3DEX = 13,  // Line3D variant of F3DEX
    L3DEX2 = 14, // Line3D variant of F3DEX2

    // F3D variants (game-specific)
    F3DSETA = 15,       // F3D SetA variant (RSP SW 2.0D alternate)
    F3DEX2ACCLAIM = 16, // Acclaim F3DEX2 (Turok 2/3, South Park)
    F3DAM = 17,         // Army Men F3DEX2 variant

    // S2DEX family (2D sprite commands)
    S2DEX = 20,
    S2DEX2 = 21,

    // Turbo3D
    Turbo3D = 30,

    // Specialized
    ZSort = 40,     // Z-sorted microcode (World Driver Championship)
    WaveRace = 41,  // Wave Race custom commands
    DiddyKong = 42, // Diddy Kong Racing custom
    StarWars = 43,  // Star Wars: Rogue Squadron
    JetForce = 44,  // Jet Force Gemini variant
    ZSortBOSS = 45, // ZSort BOSS variant (Stunt Racer)

    // Factor 5 custom microcode
    F5Rogue = 50,      // Star Wars: Rogue Squadron (Factor 5 custom)
    F5Indi_Naboo = 51, // Indiana Jones / Battle for Naboo (Factor 5 custom)

    // Additional variants
    F3DBETA = 60,  // Early F3D beta (Star Wars SoE, Wave Race early)
    F3DEX3 = 61,   // Newest F3DEX2 derivative
    F3DEX095 = 62, // F3DEX 0.95 (Mario Kart 64)
    T3DUX = 63,    // Turbo3D UX (Toukon Road)

    Count
};

/// Get human-readable name for a microcode type
const char* MicrocodeTypeName(MicrocodeType type);

// ═══════════════════════════════════════════════════════════════════════════
//  GBI Command Sets (Graphics Binary Interface)
// ═══════════════════════════════════════════════════════════════════════════

/// GBI version determines command byte encoding
enum class GBIVersion : uint8_t {
    GBI1 = 0,       // F3D, F3DEX, Turbo3D — original command encoding
    GBI2 = 1,       // F3DEX2 and derivatives — revised encoding
    GBI_S2DEX = 2,  // S2DEX sprite command set
    GBI_S2DEX2 = 3, // S2DEX2 sprite command set
    GBI_Custom = 4, // Game-specific custom extension
};

// ═══════════════════════════════════════════════════════════════════════════
//  GBI1 Command IDs (F3D / F3DEX / older microcodes)
// ═══════════════════════════════════════════════════════════════════════════

namespace GBI1 {
constexpr uint8_t G_SPNOOP = 0x00;
constexpr uint8_t G_MTX = 0x01;
constexpr uint8_t G_MOVEMEM = 0x03;
constexpr uint8_t G_VTX = 0x04;
constexpr uint8_t G_DL = 0x06;
constexpr uint8_t G_RESERVED0 = 0x07;
constexpr uint8_t G_RESERVED1 = 0x08;
constexpr uint8_t G_SPRITE2D_BASE = 0x09;

constexpr uint8_t G_TRI1 = 0xBF;
constexpr uint8_t G_TRI2 = 0xB1;
constexpr uint8_t G_QUAD = 0xB5;
constexpr uint8_t G_LINE3D = 0xB6; // F3DEX
constexpr uint8_t G_CULLDL = 0xBE;

constexpr uint8_t G_POPMTX = 0xBD;
constexpr uint8_t G_MOVEWORD = 0xBC;
constexpr uint8_t G_TEXTURE = 0xBB;
constexpr uint8_t G_SETOTHERMODE_H = 0xBA;
constexpr uint8_t G_SETOTHERMODE_L = 0xB9;
constexpr uint8_t G_ENDDL = 0xB8;
constexpr uint8_t G_SETGEOMETRYMODE = 0xB7;
constexpr uint8_t G_CLEARGEOMETRYMODE = 0xB6;
constexpr uint8_t G_RDPHALF_1 = 0xB4;
constexpr uint8_t G_RDPHALF_2 = 0xB3;
constexpr uint8_t G_RDPHALF_CONT = 0xB2;
constexpr uint8_t G_LOAD_UCODE = 0xAF;

// F3DEX additional
constexpr uint8_t G_BRANCH_Z = 0xB0;
constexpr uint8_t G_MODIFYVTX = 0xB2;
} // namespace GBI1

// ═══════════════════════════════════════════════════════════════════════════
//  GBI2 Command IDs (F3DEX2 / F3DZEX / most common)
// ═══════════════════════════════════════════════════════════════════════════

namespace GBI2 {
constexpr uint8_t G_NOOP = 0x00;
constexpr uint8_t G_VTX = 0x01;
constexpr uint8_t G_MODIFYVTX = 0x02;
constexpr uint8_t G_CULLDL = 0x03;
constexpr uint8_t G_BRANCH_Z = 0x04;
constexpr uint8_t G_TRI1 = 0x05;
constexpr uint8_t G_TRI2 = 0x06;
constexpr uint8_t G_QUAD = 0x07;
constexpr uint8_t G_LINE3D = 0x08;

// 0x09-0xCF are undefined / reserved

constexpr uint8_t G_SPECIAL_3 = 0xD3;
constexpr uint8_t G_SPECIAL_2 = 0xD4;
constexpr uint8_t G_SPECIAL_1 = 0xD5;
constexpr uint8_t G_DMA_IO = 0xD6;
constexpr uint8_t G_TEXTURE = 0xD7;
constexpr uint8_t G_POPMTX = 0xD8;
constexpr uint8_t G_SETGEOMETRYMODE = 0xD9;
// NOTE: GBI2 has no separate CLEARGEOMETRYMODE — 0xD9 handles both
constexpr uint8_t G_MTX = 0xDA;
constexpr uint8_t G_DL = 0xDE;
constexpr uint8_t G_ENDDL = 0xDF;
constexpr uint8_t G_MOVEWORD = 0xDB;
constexpr uint8_t G_MOVEMEM = 0xDC;
constexpr uint8_t G_LOAD_UCODE = 0xDD;
constexpr uint8_t G_SETOTHERMODE_H = 0xE3; // F3DEX2: 0xE3 = H, 0xE2 = L
constexpr uint8_t G_SETOTHERMODE_L = 0xE2;
constexpr uint8_t G_RDPHALF_1 = 0xE1;
constexpr uint8_t G_RDPHALF_2 = 0xF1;

// All RDP commands 0xC0-0xFF pass through to RDP
} // namespace GBI2

// ═══════════════════════════════════════════════════════════════════════════
//  MoveWord / MoveMem Indices
// ═══════════════════════════════════════════════════════════════════════════

namespace MoveWord {
constexpr uint8_t MW_MATRIX = 0x00;
constexpr uint8_t MW_NUMLIGHT = 0x02;
constexpr uint8_t MW_CLIP = 0x04;
constexpr uint8_t MW_SEGMENT = 0x06;
constexpr uint8_t MW_FOG = 0x08;
constexpr uint8_t MW_LIGHTCOL = 0x0A;
constexpr uint8_t MW_FORCEMTX = 0x0C;
constexpr uint8_t MW_PERSPNORM = 0x0E;
} // namespace MoveWord

namespace MoveMem {
// GBI1
constexpr uint8_t G_MV_VIEWPORT = 0x80;
constexpr uint8_t G_MV_LOOKATY = 0x82;
constexpr uint8_t G_MV_LOOKATX = 0x84;
constexpr uint8_t G_MV_L0 = 0x86;
constexpr uint8_t G_MV_L1 = 0x88;
constexpr uint8_t G_MV_L2 = 0x8A;
constexpr uint8_t G_MV_L3 = 0x8C;
constexpr uint8_t G_MV_L4 = 0x8E;
constexpr uint8_t G_MV_L5 = 0x90;
constexpr uint8_t G_MV_L6 = 0x92;
constexpr uint8_t G_MV_L7 = 0x94;
constexpr uint8_t G_MV_TXTATT = 0x96;
constexpr uint8_t G_MV_MATRIX_1 = 0x9E;

// GBI2 (different addressing: index * 8 = offset)
constexpr uint8_t G_MV2_VIEWPORT = 8;
constexpr uint8_t G_MV2_LIGHT = 10;
constexpr uint8_t G_MV2_POINT = 12;
constexpr uint8_t G_MV2_MATRIX = 14;
} // namespace MoveMem

// ═══════════════════════════════════════════════════════════════════════════
//  Microcode Signature (for detection from IMEM)
// ═══════════════════════════════════════════════════════════════════════════

struct MicrocodeSignature {
    MicrocodeType type;
    uint32_t crc;            // CRC of the microcode IMEM data
    uint32_t textLength;     // Size of microcode text in bytes
    const char* name;        // Human-readable name
    const char* description; // Additional info (e.g., "Zelda OoT v1.2")
};

/// Microcode feature capabilities
struct MicrocodeCapabilities {
    MicrocodeType type;
    GBIVersion gbiVersion;

    int maxVertices;        // Max vertex buffer entries
    int maxTriPerCmd;       // Triangles per single GBI command (1 or 2)
    bool hasModifyVtx;      // Supports G_MODIFYVTX
    bool hasCullDL;         // Supports G_CULLDL
    bool hasBranchZ;        // Supports G_BRANCH_Z
    bool hasNearClip;       // Hardware near-clip support
    bool hasLighting;       // Supports G_LIGHTING mode
    bool hasFog;            // Supports G_FOG mode
    bool hasTextureGen;     // Supports G_TEXTURE_GEN
    bool hasQuadCommand;    // Supports G_QUAD
    bool hasLine3D;         // Supports G_LINE3D
    bool hasSpriteCommands; // 2D sprite GBI (S2DEX variants)
    bool hasZSort;          // Z-sorted rendering
    bool hasDMAIO;          // Direct DMA commands
    bool hasLoadUcode;      // Supports dynamic ucode loading
    bool hasPointLight;     // Point lighting (Zelda)

    /// Get capabilities for a given microcode type
    static MicrocodeCapabilities ForType(MicrocodeType type);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Microcode Info (runtime-detected info for current game)
// ═══════════════════════════════════════════════════════════════════════════

struct MicrocodeInfo {
    MicrocodeType type = MicrocodeType::Unknown;
    GBIVersion gbiVersion = GBIVersion::GBI2;
    MicrocodeCapabilities caps{};
    uint32_t crc = 0;
    uint32_t dataAddress = 0; // RDRAM address of microcode data
    uint32_t textAddress = 0; // RDRAM address of microcode text
    uint32_t dataSize = 0;
    uint32_t textSize = 0;
    std::string name; // Detected name string

    bool Valid() const { return type != MicrocodeType::Unknown; }
};

} // namespace Nova64::Video
