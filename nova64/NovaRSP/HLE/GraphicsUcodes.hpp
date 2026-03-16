// SuperNova64 - N64 Emulator
// Graphics Microcode Definitions
// F3DEX/F3DEX2 command opcodes, geometry modes, and microcode CRC database
// Based on Project64 RSP HLE + GLideN64 microcode database

#pragma once
#include <cstdint>
#include <string_view>

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// Graphics Microcode Types
// ──────────────────────────────────────────────
enum class GfxUcodeType : uint8_t {
    Unknown = 0,

    // Fast3D family
    F3D,            // Original Fast3D (SM64, MK64 early)
    F3DEX,          // Fast3DEX (extended vertex cache)
    F3DPD,          // Perfect Dark variant
    F3DDKR,         // Diddy Kong Racing variant
    F3DWRB,         // Wave Race 64 variant
    F3DSWSE,        // Shadows of the Empire
    F3DGBI,         // Generic GBI

    // Fast3DEX2 family
    F3DEX2,         // Standard F3DEX2 (Zelda OOT, Banjo, etc.)
    F3DEX2_CBFD,    // Conker's Bad Fur Day
    F3DEX2_PD,      // Perfect Dark (F3DEX2)

    // Texture extensions
    F3DTEXA,        // F3DEX with texture atlas extensions

    // Sprite 2D
    S2DEX,          // Sprite 2D (Yoshi's Story, etc.)
    S2DEX2,         // Sprite 2D v2

    // Special
    L3DEX,          // Line3D extended
    L3DEX2,         // Line3D extended v2
    ZSort,          // Z-Sort pipeline (World Driver Championship)
    Turbo3D,        // Turbo3D (Extreme-G)
};

// ──────────────────────────────────────────────
// F3D / F3DEX Command Opcodes (GBI 1.0)
// ──────────────────────────────────────────────
namespace F3D {
    inline constexpr uint8_t NOOP           = 0x00;
    inline constexpr uint8_t MTX            = 0x01;
    inline constexpr uint8_t RESERVED0      = 0x02;
    inline constexpr uint8_t MOVEMEM        = 0x03;
    inline constexpr uint8_t VTX            = 0x04;
    inline constexpr uint8_t RESERVED1      = 0x05;
    inline constexpr uint8_t DL             = 0x06;
    inline constexpr uint8_t RESERVED2      = 0x07;
    inline constexpr uint8_t RESERVED3      = 0x08;
    inline constexpr uint8_t SPRITE2D_BASE  = 0x09;

    inline constexpr uint8_t TRI1           = 0xBF;
    inline constexpr uint8_t CULLDL         = 0xBE;
    inline constexpr uint8_t POPMTX         = 0xBD;
    inline constexpr uint8_t MOVEWORD       = 0xBC;
    inline constexpr uint8_t TEXTURE        = 0xBB;
    inline constexpr uint8_t SETOTHERMODE_H = 0xBA;
    inline constexpr uint8_t SETOTHERMODE_L = 0xB9;
    inline constexpr uint8_t ENDDL          = 0xB8;
    inline constexpr uint8_t SETGEOMETRYMODE = 0xB7;
    inline constexpr uint8_t CLEARGEOMETRYMODE = 0xB6;
    inline constexpr uint8_t LINE3D         = 0xB5;
    inline constexpr uint8_t RDPHALF_1      = 0xB4;
    inline constexpr uint8_t RDPHALF_2      = 0xB3;

    // RDP commands passed through
    inline constexpr uint8_t SETCIMG        = 0xFF;
    inline constexpr uint8_t SETZIMG        = 0xFE;
    inline constexpr uint8_t SETTIMG        = 0xFD;
    inline constexpr uint8_t SETCOMBINE     = 0xFC;
    inline constexpr uint8_t SETENVCOLOR    = 0xFB;
    inline constexpr uint8_t SETPRIMCOLOR   = 0xFA;
    inline constexpr uint8_t SETBLENDCOLOR  = 0xF9;
    inline constexpr uint8_t SETFOGCOLOR    = 0xF8;
    inline constexpr uint8_t SETFILLCOLOR   = 0xF7;
    inline constexpr uint8_t FILLRECT       = 0xF6;
    inline constexpr uint8_t SETTILE        = 0xF5;
    inline constexpr uint8_t LOADTILE       = 0xF4;
    inline constexpr uint8_t LOADBLOCK      = 0xF3;
    inline constexpr uint8_t SETTILESIZE    = 0xF2;
    inline constexpr uint8_t LOADTLUT       = 0xF0;
    inline constexpr uint8_t RDPSETOTHERMODE = 0xEF;
    inline constexpr uint8_t SETPRIMDEPTH   = 0xEE;
    inline constexpr uint8_t SETSCISSOR     = 0xED;
    inline constexpr uint8_t SETCONVERT     = 0xEC;
    inline constexpr uint8_t SETKEYR        = 0xEB;
    inline constexpr uint8_t SETKEYGB       = 0xEA;
    inline constexpr uint8_t RDPFULLSYNC    = 0xE9;
    inline constexpr uint8_t RDPTILESYNC    = 0xE8;
    inline constexpr uint8_t RDPPIPESYNC    = 0xE7;
    inline constexpr uint8_t RDPLOADSYNC    = 0xE6;
    inline constexpr uint8_t TEXRECT_FLIP   = 0xE5;
    inline constexpr uint8_t TEXRECT        = 0xE4;
}

// ──────────────────────────────────────────────
// F3DEX Command Opcodes (GBI 1.x extended)
// ──────────────────────────────────────────────
namespace F3DEX {
    inline constexpr uint8_t NOOP           = 0x00;
    inline constexpr uint8_t MTX            = 0x01;
    inline constexpr uint8_t RESERVED0      = 0x02;
    inline constexpr uint8_t MOVEMEM        = 0x03;
    inline constexpr uint8_t VTX            = 0x04;
    inline constexpr uint8_t RESERVED1      = 0x05;
    inline constexpr uint8_t DL             = 0x06;
    inline constexpr uint8_t RESERVED2      = 0x07;
    inline constexpr uint8_t RESERVED3      = 0x08;
    inline constexpr uint8_t SPRITE2D_BASE  = 0x09;

    inline constexpr uint8_t MODIFYVTX      = 0xB2;
    inline constexpr uint8_t RDPHALF_2      = 0xB3;
    inline constexpr uint8_t RDPHALF_1      = 0xB4;
    inline constexpr uint8_t LINE3D         = 0xB5;
    inline constexpr uint8_t CLEARGEOMETRYMODE = 0xB6;
    inline constexpr uint8_t SETGEOMETRYMODE = 0xB7;
    inline constexpr uint8_t ENDDL          = 0xB8;
    inline constexpr uint8_t SETOTHERMODE_L = 0xB9;
    inline constexpr uint8_t SETOTHERMODE_H = 0xBA;
    inline constexpr uint8_t TEXTURE        = 0xBB;
    inline constexpr uint8_t MOVEWORD       = 0xBC;
    inline constexpr uint8_t POPMTX         = 0xBD;
    inline constexpr uint8_t CULLDL         = 0xBE;
    inline constexpr uint8_t TRI1           = 0xBF;

    // Extended commands
    inline constexpr uint8_t BRANCH_Z       = 0xB0;
    inline constexpr uint8_t TRI2           = 0xB1;
    inline constexpr uint8_t LOAD_UCODE     = 0xAF;

    // RDP commands - same as F3D
    inline constexpr uint8_t SETCIMG        = F3D::SETCIMG;
    inline constexpr uint8_t SETZIMG        = F3D::SETZIMG;
    inline constexpr uint8_t SETTIMG        = F3D::SETTIMG;
    inline constexpr uint8_t SETCOMBINE     = F3D::SETCOMBINE;
    inline constexpr uint8_t SETENVCOLOR    = F3D::SETENVCOLOR;
    inline constexpr uint8_t SETPRIMCOLOR   = F3D::SETPRIMCOLOR;
    inline constexpr uint8_t SETBLENDCOLOR  = F3D::SETBLENDCOLOR;
    inline constexpr uint8_t SETFOGCOLOR    = F3D::SETFOGCOLOR;
    inline constexpr uint8_t SETFILLCOLOR   = F3D::SETFILLCOLOR;
    inline constexpr uint8_t FILLRECT       = F3D::FILLRECT;
    inline constexpr uint8_t SETTILE        = F3D::SETTILE;
    inline constexpr uint8_t LOADTILE       = F3D::LOADTILE;
    inline constexpr uint8_t LOADBLOCK      = F3D::LOADBLOCK;
    inline constexpr uint8_t SETTILESIZE    = F3D::SETTILESIZE;
    inline constexpr uint8_t LOADTLUT       = F3D::LOADTLUT;
    inline constexpr uint8_t RDPSETOTHERMODE = F3D::RDPSETOTHERMODE;
    inline constexpr uint8_t SETPRIMDEPTH   = F3D::SETPRIMDEPTH;
    inline constexpr uint8_t SETSCISSOR     = F3D::SETSCISSOR;
    inline constexpr uint8_t SETCONVERT     = F3D::SETCONVERT;
    inline constexpr uint8_t SETKEYR        = F3D::SETKEYR;
    inline constexpr uint8_t SETKEYGB       = F3D::SETKEYGB;
    inline constexpr uint8_t RDPFULLSYNC    = F3D::RDPFULLSYNC;
    inline constexpr uint8_t RDPTILESYNC    = F3D::RDPTILESYNC;
    inline constexpr uint8_t RDPPIPESYNC    = F3D::RDPPIPESYNC;
    inline constexpr uint8_t RDPLOADSYNC    = F3D::RDPLOADSYNC;
    inline constexpr uint8_t TEXRECT_FLIP   = F3D::TEXRECT_FLIP;
    inline constexpr uint8_t TEXRECT        = F3D::TEXRECT;
}

// ──────────────────────────────────────────────
// F3DEX2 Command Opcodes (GBI 2.0)
// Major opcode reassignment from F3DEX
// ──────────────────────────────────────────────
namespace F3DEX2 {
    inline constexpr uint8_t NOOP           = 0x00;
    inline constexpr uint8_t VTX            = 0x01;
    inline constexpr uint8_t MODIFYVTX      = 0x02;
    inline constexpr uint8_t CULLDL         = 0x03;
    inline constexpr uint8_t BRANCH_Z       = 0x04;
    inline constexpr uint8_t TRI1           = 0x05;
    inline constexpr uint8_t TRI2           = 0x06;
    inline constexpr uint8_t QUAD           = 0x07;
    inline constexpr uint8_t LINE3D         = 0x08;

    inline constexpr uint8_t DL             = 0xDE;
    inline constexpr uint8_t ENDDL          = 0xDF;
    inline constexpr uint8_t SPNOOP         = 0xE0;
    inline constexpr uint8_t RDPHALF_1      = 0xE1;
    inline constexpr uint8_t SETOTHERMODE_L = 0xE2;
    inline constexpr uint8_t SETOTHERMODE_H = 0xE3;
    inline constexpr uint8_t TEXRECT        = 0xE4;
    inline constexpr uint8_t TEXRECT_FLIP   = 0xE5;
    inline constexpr uint8_t RDPLOADSYNC    = 0xE6;
    inline constexpr uint8_t RDPPIPESYNC    = 0xE7;
    inline constexpr uint8_t RDPTILESYNC    = 0xE8;
    inline constexpr uint8_t RDPFULLSYNC    = 0xE9;
    inline constexpr uint8_t SETKEYGB       = 0xEA;
    inline constexpr uint8_t SETKEYR        = 0xEB;
    inline constexpr uint8_t SETCONVERT     = 0xEC;
    inline constexpr uint8_t SETSCISSOR     = 0xED;
    inline constexpr uint8_t SETPRIMDEPTH   = 0xEE;
    inline constexpr uint8_t RDPSETOTHERMODE = 0xEF;
    inline constexpr uint8_t LOADTLUT       = 0xF0;
    inline constexpr uint8_t RDPHALF_2      = 0xF1;
    inline constexpr uint8_t SETTILESIZE    = 0xF2;
    inline constexpr uint8_t LOADBLOCK      = 0xF3;
    inline constexpr uint8_t LOADTILE       = 0xF4;
    inline constexpr uint8_t SETTILE        = 0xF5;
    inline constexpr uint8_t FILLRECT       = 0xF6;
    inline constexpr uint8_t SETFILLCOLOR   = 0xF7;
    inline constexpr uint8_t SETFOGCOLOR    = 0xF8;
    inline constexpr uint8_t SETBLENDCOLOR  = 0xF9;
    inline constexpr uint8_t SETPRIMCOLOR   = 0xFA;
    inline constexpr uint8_t SETENVCOLOR    = 0xFB;
    inline constexpr uint8_t SETCOMBINE     = 0xFC;
    inline constexpr uint8_t SETTIMG        = 0xFD;
    inline constexpr uint8_t SETZIMG        = 0xFE;
    inline constexpr uint8_t SETCIMG        = 0xFF;

    // F3DEX2 geometry-mode specific
    inline constexpr uint8_t TEXTURE        = 0xD7;
    inline constexpr uint8_t POPMTX         = 0xD8;
    inline constexpr uint8_t GEOMETRYMODE   = 0xD9;
    inline constexpr uint8_t MTX            = 0xDA;
    inline constexpr uint8_t MOVEWORD       = 0xDB;
    inline constexpr uint8_t MOVEMEM        = 0xDC;
    inline constexpr uint8_t LOAD_UCODE     = 0xDD;

    // Special
    inline constexpr uint8_t DMA_IO         = 0xD6;
    inline constexpr uint8_t SPECIAL_1      = 0xD5;
    inline constexpr uint8_t SPECIAL_2      = 0xD4;
    inline constexpr uint8_t SPECIAL_3      = 0xD3;
}

// ──────────────────────────────────────────────
// N64 Geometry Mode Flags
// ──────────────────────────────────────────────
namespace GeometryMode {
    // GBI 1 (F3D/F3DEX)
    inline constexpr uint32_t G_ZBUFFER            = 0x00000001;
    inline constexpr uint32_t G_SHADE              = 0x00000004;
    inline constexpr uint32_t G_CULL_FRONT         = 0x00000200;
    inline constexpr uint32_t G_CULL_BACK          = 0x00000400;
    inline constexpr uint32_t G_CULL_BOTH          = G_CULL_FRONT | G_CULL_BACK;
    inline constexpr uint32_t G_FOG                = 0x00010000;
    inline constexpr uint32_t G_LIGHTING           = 0x00020000;
    inline constexpr uint32_t G_TEXTURE_GEN        = 0x00040000;
    inline constexpr uint32_t G_TEXTURE_GEN_LINEAR = 0x00080000;
    inline constexpr uint32_t G_SHADING_SMOOTH     = 0x00200000;
    inline constexpr uint32_t G_CLIPPING           = 0x00800000;

    // GBI 2 (F3DEX2) - reordered
    inline constexpr uint32_t G2_ZBUFFER            = 0x00000001;
    inline constexpr uint32_t G2_SHADE              = 0x00000200;
    inline constexpr uint32_t G2_CULL_FRONT         = 0x00000400;
    inline constexpr uint32_t G2_CULL_BACK          = 0x00000800;
    inline constexpr uint32_t G2_CULL_BOTH          = G2_CULL_FRONT | G2_CULL_BACK;
    inline constexpr uint32_t G2_FOG                = 0x00010000;
    inline constexpr uint32_t G2_LIGHTING           = 0x00020000;
    inline constexpr uint32_t G2_TEXTURE_GEN        = 0x00040000;
    inline constexpr uint32_t G2_TEXTURE_GEN_LINEAR = 0x00080000;
    inline constexpr uint32_t G2_LOD                = 0x00100000;
    inline constexpr uint32_t G2_SHADING_SMOOTH     = 0x00200000;
    inline constexpr uint32_t G2_CLIPPING           = 0x00800000;
}

// ──────────────────────────────────────────────
// MoveWord indices
// ──────────────────────────────────────────────
namespace MoveWord {
    inline constexpr uint8_t MATRIX     = 0x00;
    inline constexpr uint8_t NUMLIGHT   = 0x02;
    inline constexpr uint8_t CLIP       = 0x04;
    inline constexpr uint8_t SEGMENT    = 0x06;
    inline constexpr uint8_t FOG        = 0x08;
    inline constexpr uint8_t LIGHTCOL   = 0x0A;
    inline constexpr uint8_t FORCEMTX   = 0x0C;
    inline constexpr uint8_t PERSPNORM  = 0x0E;
}

// ──────────────────────────────────────────────
// MoveMem indices
// ──────────────────────────────────────────────
namespace MoveMem {
    // F3D / F3DEX
    inline constexpr uint8_t VIEWPORT  = 0x80;
    inline constexpr uint8_t LOOKATY   = 0x82;
    inline constexpr uint8_t LOOKATX   = 0x84;
    inline constexpr uint8_t LIGHT     = 0x86;

    // F3DEX2
    inline constexpr uint8_t V2_VIEWPORT = 0x08;
    inline constexpr uint8_t V2_LIGHT    = 0x0A;
    inline constexpr uint8_t V2_MATRIX   = 0x0E;
}

// ──────────────────────────────────────────────
// Texture Image format / size
// ──────────────────────────────────────────────
namespace TexFormat {
    inline constexpr uint8_t RGBA = 0;
    inline constexpr uint8_t YUV  = 1;
    inline constexpr uint8_t CI   = 2;
    inline constexpr uint8_t IA   = 3;
    inline constexpr uint8_t I    = 4;
}

namespace TexSize {
    inline constexpr uint8_t Bpp4  = 0;
    inline constexpr uint8_t Bpp8  = 1;
    inline constexpr uint8_t Bpp16 = 2;
    inline constexpr uint8_t Bpp32 = 3;
}

// ──────────────────────────────────────────────
// OtherMode bit positions (H = high, L = low)
// ──────────────────────────────────────────────
namespace OtherModeH {
    inline constexpr uint32_t BLEND_MASK  = 0x00000001;
    inline constexpr uint32_t ALPHA_DITHER = 0x00000030;
    inline constexpr uint32_t RGB_DITHER  = 0x000000C0;
    inline constexpr uint32_t COMBKEY     = 0x00000100;
    inline constexpr uint32_t TEXTCONV    = 0x00000E00;
    inline constexpr uint32_t TEXTFILT    = 0x00003000;
    inline constexpr uint32_t TEXTLUT     = 0x0000C000;
    inline constexpr uint32_t TEXTLOD     = 0x00010000;
    inline constexpr uint32_t TEXTDETAIL  = 0x00060000;
    inline constexpr uint32_t TEXTPERSP   = 0x00080000;
    inline constexpr uint32_t CYCLETYPE   = 0x00300000;
    inline constexpr uint32_t PIPELINE    = 0x00800000;
}

namespace OtherModeL {
    inline constexpr uint32_t ALPHA_COMPARE = 0x00000003;
    inline constexpr uint32_t ZSRC_SEL      = 0x00000004;
    inline constexpr uint32_t RENDERMODE    = 0xFFFFFFF8;
}

// ──────────────────────────────────────────────
// Cycle type from OtherMode
// ──────────────────────────────────────────────
namespace CycleType {
    inline constexpr uint32_t Mode1   = 0;
    inline constexpr uint32_t Mode2   = 1;
    inline constexpr uint32_t Copy    = 2;
    inline constexpr uint32_t Fill    = 3;
}

// ──────────────────────────────────────────────
// Graphics Microcode CRC Database
// CRC values from GLideN64, Mupen64Plus, and Project64
// ──────────────────────────────────────────────
struct GfxMicrocodeEntry {
    uint32_t        ucode_crc;
    uint32_t        ucode_data_crc;  // 0 = don't check
    GfxUcodeType    type;
    std::string_view name;
};

inline constexpr GfxMicrocodeEntry kGfxMicrocodeDatabase[] = {
    // ── F3D (original Fast3D) ──
    { 0xBC45382E, 0, GfxUcodeType::F3D,     "Fast3D (SM64)" },
    { 0x47F74D2B, 0, GfxUcodeType::F3D,     "Fast3D (alternate)" },

    // ── F3DEX (GBI 1.x extended) ──
    { 0x5D1D6F72, 0, GfxUcodeType::F3DEX,   "F3DEX 1.21 (Yoshi)" },
    { 0x1A62DC2F, 0, GfxUcodeType::F3DEX,   "F3DEX 1.21 (standard)" },
    { 0xDA51CCDB, 0, GfxUcodeType::F3DEX,   "F3DEX 1.23 (standard)" },
    { 0x97414797, 0, GfxUcodeType::F3DEX,   "F3DEX 1.23 (Zelda MM)" },
    { 0x25CD0965, 0, GfxUcodeType::F3DEX,   "F3DEX 1.23 (OOT Debug)" },
    { 0xCB8C9B6C, 0, GfxUcodeType::F3DEX,   "F3DEX (Jet Force)" },
    { 0x91A40802, 0, GfxUcodeType::F3DEX,   "F3DEX 1.0" },

    // ── F3DEX2 (GBI 2.0) ── 
    { 0x22099872, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.04H (MK64)" },
    { 0x86B1593E, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.04H (standard)" },
    { 0x2B291027, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.04H (alt)" },
    { 0xB4577B9C, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.07 (Zelda OOT)" },
    { 0x1412F2A6, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.07 (alt)" },
    { 0x2B5A89C2, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.08 (Banjo)" },
    { 0xDE7D67D4, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.08 (Zelda MM)" },
    { 0x3003A5D1, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.08 (DK64)" },
    { 0xD536B388, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.08 (Paper Mario)" },
    { 0x1A1E1920, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.08 (Kirby64)" },
    { 0x0C8E5EC6, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.08 (Animal Forest)" },
    { 0xC46DBD86, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.04 (generic)" },
    { 0xBAD725A4, 0, GfxUcodeType::F3DEX2,  "F3DEX2 2.05 (generic)" },

    // ── F3DEX2 CBFD (Conker) ──
    { 0x5B5D36E3, 0, GfxUcodeType::F3DEX2_CBFD, "F3DEX2 CBFD (Conker)" },

    // ── F3DEX2 PD (Perfect Dark) ──
    { 0x5D3099F1, 0, GfxUcodeType::F3DEX2_PD, "F3DEX2 PD" },

    // ── F3DPD (Perfect Dark F3D) ──
    { 0x05165579, 0, GfxUcodeType::F3DPD,   "F3DPD (Perfect Dark)" },

    // ── F3DDKR (Diddy Kong Racing) ──
    { 0x07C3C4A2, 0, GfxUcodeType::F3DDKR,  "F3DDKR (Diddy Kong)" },

    // ── F3DSWSE (Shadows of the Empire) ──
    { 0x167E028B, 0, GfxUcodeType::F3DSWSE, "F3DSWSE (Shadows)" },

    // ── F3DTEXA ──
    { 0x4165E1FD, 0, GfxUcodeType::F3DTEXA, "F3DTEXA" },

    // ── S2DEX (Sprite 2D) ──
    { 0xEC94C05E, 0, GfxUcodeType::S2DEX,   "S2DEX 1.06 (Yoshi)" },
    { 0x9DF31081, 0, GfxUcodeType::S2DEX,   "S2DEX 1.05" },
    { 0x13CD7658, 0, GfxUcodeType::S2DEX,   "S2DEX 1.03" },

    // ── S2DEX2 ──
    { 0xB4CD7858, 0, GfxUcodeType::S2DEX2,  "S2DEX2 2.05" },
    { 0xA3CDD17E, 0, GfxUcodeType::S2DEX2,  "S2DEX2 2.04" },

    // ── L3DEX ──
    { 0x6A834BFA, 0, GfxUcodeType::L3DEX,   "L3DEX 1.23" },

    // ── L3DEX2 ──
    { 0x994C8AFC, 0, GfxUcodeType::L3DEX2,  "L3DEX2 2.08" },

    // ── ZSort ──
    { 0xF10DB2C5, 0, GfxUcodeType::ZSort,   "ZSort (WDC)" },

    // ── Turbo3D ──
    { 0x9A4178E0, 0, GfxUcodeType::Turbo3D, "Turbo3D (Extreme-G)" },
};

inline constexpr size_t kGfxMicrocodeCount = sizeof(kGfxMicrocodeDatabase) / sizeof(kGfxMicrocodeDatabase[0]);

// ──────────────────────────────────────────────
// Lookup function for graphics microcodes
// ──────────────────────────────────────────────
[[nodiscard]] inline const GfxMicrocodeEntry* FindGfxMicrocode(uint32_t crc) noexcept
{
    for (size_t i = 0; i < kGfxMicrocodeCount; ++i)
    {
        if (kGfxMicrocodeDatabase[i].ucode_crc == crc)
            return &kGfxMicrocodeDatabase[i];
    }
    return nullptr;
}

[[nodiscard]] inline const GfxMicrocodeEntry* FindGfxMicrocode(uint32_t crc, uint32_t data_crc) noexcept
{
    const GfxMicrocodeEntry* fallback = nullptr;
    for (size_t i = 0; i < kGfxMicrocodeCount; ++i)
    {
        if (kGfxMicrocodeDatabase[i].ucode_crc == crc)
        {
            if (kGfxMicrocodeDatabase[i].ucode_data_crc == 0)
                fallback = &kGfxMicrocodeDatabase[i];
            else if (kGfxMicrocodeDatabase[i].ucode_data_crc == data_crc)
                return &kGfxMicrocodeDatabase[i];
        }
    }
    return fallback;
}

// ──────────────────────────────────────────────
// Helper: is this a GBI2 microcode?
// ──────────────────────────────────────────────
[[nodiscard]] inline constexpr bool IsGBI2(GfxUcodeType type) noexcept
{
    switch (type) {
    case GfxUcodeType::F3DEX2:
    case GfxUcodeType::F3DEX2_CBFD:
    case GfxUcodeType::F3DEX2_PD:
    case GfxUcodeType::S2DEX2:
    case GfxUcodeType::L3DEX2:
        return true;
    default:
        return false;
    }
}

} // namespace Nova64::RSP::HLE
