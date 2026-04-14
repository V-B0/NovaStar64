/**
 * @file N64Types.hpp
 * @brief N64 hardware types, RDP register definitions, and memory map constants
 *
 * This header defines the complete set of N64 video-related hardware constants,
 * register layouts, and fundamental data types used throughout NovaGraphics.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  N64 Memory Map Constants
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint32_t RDRAM_BASE           = 0x00000000;
constexpr uint32_t RDRAM_SIZE           = 0x00800000;   // 8 MB (expanded)
constexpr uint32_t RDRAM_SIZE_4MB       = 0x00400000;   // 4 MB (base)
constexpr uint32_t SP_DMEM_BASE         = 0x04000000;
constexpr uint32_t SP_DMEM_SIZE         = 0x00001000;   // 4 KB
constexpr uint32_t SP_IMEM_BASE         = 0x04001000;
constexpr uint32_t SP_IMEM_SIZE         = 0x00001000;   // 4 KB
constexpr uint32_t DPC_BASE             = 0x04100000;   // RDP command registers
constexpr uint32_t DPC_END              = 0x04100020;
constexpr uint32_t DPS_BASE             = 0x04200000;   // RDP span registers
constexpr uint32_t DPS_END              = 0x04200010;
constexpr uint32_t VI_BASE              = 0x04400000;   // Video interface
constexpr uint32_t VI_END               = 0x04400038;

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Command Processor Registers (DPC)
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint32_t DPC_START_REG        = 0x04100000;
constexpr uint32_t DPC_END_REG          = 0x04100004;
constexpr uint32_t DPC_CURRENT_REG      = 0x04100008;
constexpr uint32_t DPC_STATUS_REG       = 0x0410000C;
constexpr uint32_t DPC_CLOCK_REG        = 0x04100010;
constexpr uint32_t DPC_BUFBUSY_REG      = 0x04100014;
constexpr uint32_t DPC_PIPEBUSY_REG     = 0x04100018;
constexpr uint32_t DPC_TMEM_REG         = 0x0410001C;

// DPC Status bits
constexpr uint32_t DPC_STATUS_XBUS_DMEM_DMA   = 0x0001;
constexpr uint32_t DPC_STATUS_FREEZE           = 0x0002;
constexpr uint32_t DPC_STATUS_FLUSH            = 0x0004;
constexpr uint32_t DPC_STATUS_START_GCLK       = 0x0008;
constexpr uint32_t DPC_STATUS_TMEM_BUSY        = 0x0010;
constexpr uint32_t DPC_STATUS_PIPE_BUSY        = 0x0020;
constexpr uint32_t DPC_STATUS_CMD_BUSY         = 0x0040;
constexpr uint32_t DPC_STATUS_CBUF_READY       = 0x0080;
constexpr uint32_t DPC_STATUS_DMA_BUSY         = 0x0100;
constexpr uint32_t DPC_STATUS_END_VALID        = 0x0200;
constexpr uint32_t DPC_STATUS_START_VALID      = 0x0400;

// DPC Status write bits
constexpr uint32_t DPC_CLR_XBUS_DMEM_DMA      = 0x0001;
constexpr uint32_t DPC_SET_XBUS_DMEM_DMA      = 0x0002;
constexpr uint32_t DPC_CLR_FREEZE             = 0x0004;
constexpr uint32_t DPC_SET_FREEZE             = 0x0008;
constexpr uint32_t DPC_CLR_FLUSH              = 0x0010;
constexpr uint32_t DPC_SET_FLUSH              = 0x0020;
constexpr uint32_t DPC_CLR_TMEM_CTR           = 0x0040;
constexpr uint32_t DPC_CLR_PIPE_CTR           = 0x0080;
constexpr uint32_t DPC_CLR_CMD_CTR            = 0x0100;
constexpr uint32_t DPC_CLR_CLOCK_CTR          = 0x0200;

// ═══════════════════════════════════════════════════════════════════════════
//  Video Interface — register offsets are in VideoInterface.hpp (VIRegister enum)
//  Absolute addresses are in N64Memory.hpp (N64Mem namespace)
// ═══════════════════════════════════════════════════════════════════════════

// VI Status bits
constexpr uint32_t VI_STATUS_TYPE_MASK       = 0x0003;
constexpr uint32_t VI_STATUS_TYPE_BLANK      = 0x0000;
constexpr uint32_t VI_STATUS_TYPE_16BIT      = 0x0002;  // RGBA 5551
constexpr uint32_t VI_STATUS_TYPE_32BIT      = 0x0003;  // RGBA 8888
constexpr uint32_t VI_STATUS_GAMMA_DITHER_EN = 0x0004;
constexpr uint32_t VI_STATUS_GAMMA_EN        = 0x0008;
constexpr uint32_t VI_STATUS_DIVOT_EN        = 0x0010;
constexpr uint32_t VI_STATUS_VBUS_CLK_EN     = 0x0020;
constexpr uint32_t VI_STATUS_SERRATE         = 0x0040;
constexpr uint32_t VI_STATUS_TEST_MODE       = 0x0080;
constexpr uint32_t VI_STATUS_AA_MODE_MASK    = 0x0300;
constexpr uint32_t VI_STATUS_AA_FULL         = 0x0000;
constexpr uint32_t VI_STATUS_AA_OPTIMIZED    = 0x0100;
constexpr uint32_t VI_STATUS_AA_RESAMP_ONLY  = 0x0200;
constexpr uint32_t VI_STATUS_AA_NONE         = 0x0300;
constexpr uint32_t VI_STATUS_DITHER_FILTER   = 0x10000;

// ═══════════════════════════════════════════════════════════════════════════
//  TMEM Layout Constants  
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint32_t TMEM_SIZE            = 4096;          // 4 KB total
constexpr uint32_t TMEM_PALETTE_BASE    = 2048;          // 256 bytes × 8 palettes
constexpr uint32_t TMEM_LINE_SIZE       = 8;             // 8 bytes per TMEM line

// ═══════════════════════════════════════════════════════════════════════════
//  N64 Pixel Format Types
// ═══════════════════════════════════════════════════════════════════════════

enum class ImageFormat : uint8_t {
    RGBA = 0,
    YUV  = 1,
    CI   = 2,   // Color-indexed
    IA   = 3,   // Intensity + Alpha
    I    = 4,   // Intensity only
    Count = 5
};

enum class ImageSize : uint8_t {
    Bpp4  = 0,
    Bpp8  = 1,
    Bpp16 = 2,
    Bpp32 = 3,
    Count = 4
};

/// Get bytes per pixel for a given size (returns fraction for 4bpp)
constexpr float GetBytesPerPixel(ImageFormat fmt, ImageSize size) {
    // Most formats: 4bpp=0.5, 8bpp=1, 16bpp=2, 32bpp=4
    constexpr float kBPP[] = { 0.5f, 1.0f, 2.0f, 4.0f };
    (void)fmt;
    return kBPP[static_cast<int>(size)];
}

/// Get the number of bits per pixel
constexpr int GetBitsPerPixel(ImageSize size) {
    constexpr int kBits[] = { 4, 8, 16, 32 };
    return kBits[static_cast<int>(size)];
}

/// Get format name string
inline const char* GetImageFormatName(ImageFormat fmt) {
    static const char* names[] = { "RGBA", "YUV", "CI", "IA", "I" };
    return names[static_cast<int>(fmt)];
}

inline const char* GetImageSizeName(ImageSize size) {
    static const char* names[] = { "4b", "8b", "16b", "32b" };
    return names[static_cast<int>(size)];
}

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Cycle Types
// ═══════════════════════════════════════════════════════════════════════════

enum class CycleType : uint8_t {
    Cycle1 = 0,
    Cycle2 = 1,
    Copy   = 2,
    Fill   = 3,
};

inline const char* GetCycleTypeName(CycleType ct) {
    static const char* names[] = { "1Cycle", "2Cycle", "Copy", "Fill" };
    return names[static_cast<int>(ct)];
}

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Other Mode Bits (SetOtherModes)
// ═══════════════════════════════════════════════════════════════════════════

// High word bits
constexpr uint32_t OTHERMODE_ALPHA_DITHER_MASK   = 0x00000030;
constexpr uint32_t OTHERMODE_RGB_DITHER_MASK     = 0x000000C0;
constexpr uint32_t OTHERMODE_COMBKEY              = 0x00000100;
constexpr uint32_t OTHERMODE_TEXTCONV_MASK        = 0x00000E00;
constexpr uint32_t OTHERMODE_TEXTFILT_MASK        = 0x00003000;
constexpr uint32_t OTHERMODE_TEXTLUT_MASK         = 0x0000C000;
constexpr uint32_t OTHERMODE_TEXTLOD               = 0x00010000;
constexpr uint32_t OTHERMODE_TEXTDETAIL_MASK      = 0x00060000;
constexpr uint32_t OTHERMODE_TEXTPERSP            = 0x00080000;
constexpr uint32_t OTHERMODE_CYCLETYPE_MASK       = 0x00300000;
constexpr uint32_t OTHERMODE_CYCLETYPE_SHIFT      = 20;
constexpr uint32_t OTHERMODE_COLORDITHER_MASK     = 0x00C00000;
constexpr uint32_t OTHERMODE_PIPELINE_MASK        = 0x00800000;
constexpr uint32_t OTHERMODE_ATOMICPRIM           = 0x80000000;

// Low word bits
constexpr uint32_t OTHERMODE_ALPHACOMPARE_MASK    = 0x00000003;
constexpr uint32_t OTHERMODE_ZSRCSEL_MASK         = 0x00000004;
constexpr uint32_t OTHERMODE_AA_EN                = 0x00000008;
constexpr uint32_t OTHERMODE_Z_CMP                = 0x00000010;
constexpr uint32_t OTHERMODE_Z_UPD                = 0x00000020;
constexpr uint32_t OTHERMODE_IM_RD                = 0x00000040;
constexpr uint32_t OTHERMODE_CLR_ON_CVG           = 0x00000080;
constexpr uint32_t OTHERMODE_CVG_DST_MASK         = 0x00000300;
constexpr uint32_t OTHERMODE_ZMODE_MASK           = 0x00000C00;
constexpr uint32_t OTHERMODE_CVG_X_ALPHA          = 0x00001000;
constexpr uint32_t OTHERMODE_ALPHA_CVG_SEL        = 0x00002000;
constexpr uint32_t OTHERMODE_FORCE_BL             = 0x00004000;
constexpr uint32_t OTHERMODE_TEX_EDGE             = 0x00008000;
constexpr uint32_t OTHERMODE_BLENDER_MASK         = 0xFFFF0000;

// Texture filter modes
enum class TextureFilter : uint8_t {
    Point     = 0,
    Average   = 2,   // N64's hardware bilinear
    Bilerp    = 3,
};

// Texture LUT modes
enum class TextureLUT : uint8_t {
    None    = 0,
    RGBA16  = 2,
    IA16    = 3,
};

// Z mode
enum class ZMode : uint8_t {
    Opaque        = 0,
    Interpenetrate = 1,
    Transparent   = 2,
    Decal         = 3,
};

// Coverage destination
enum class CvgDst : uint8_t {
    Clamp  = 0,
    Wrap   = 1,
    Full   = 2,
    Save   = 3,
};

// Alpha compare mode
enum class AlphaCompare : uint8_t {
    None      = 0,
    Threshold = 1,
    Dither    = 3,
};

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Geometry Mode Bits
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint32_t G_ZBUFFER              = 0x00000001;
constexpr uint32_t G_SHADE                = 0x00000004;
constexpr uint32_t G_CULL_FRONT           = 0x00000200;
constexpr uint32_t G_CULL_BACK            = 0x00000400;
constexpr uint32_t G_CULL_BOTH            = 0x00000600;
constexpr uint32_t G_FOG                  = 0x00010000;
constexpr uint32_t G_LIGHTING             = 0x00020000;
constexpr uint32_t G_TEXTURE_GEN          = 0x00040000;
constexpr uint32_t G_TEXTURE_GEN_LINEAR   = 0x00080000;
constexpr uint32_t G_LOD                  = 0x00100000;
constexpr uint32_t G_SHADING_SMOOTH       = 0x00200000;
constexpr uint32_t G_POINT_LIGHTING       = 0x00400000;
constexpr uint32_t G_CLIPPING             = 0x00800000;

// ═══════════════════════════════════════════════════════════════════════════
//  Fundamental Geometry Types
// ═══════════════════════════════════════════════════════════════════════════

/// N64 vertex as received from display list (16 bytes raw) + transformed data
struct N64Vertex {
    // ── Raw N64 vertex data (16 bytes) ──
    int16_t x, y, z;           // Position (s15 integer)
    uint16_t flag;
    int16_t s, t;              // Texture coordinates (s10.5)
    uint8_t r, g, b, a;       // Color / normal

    // ── Transformed/processed fields ──
    float sx, sy, sz;           // Screen-space position
    float w;                    // W coordinate (clip space)
    float invW;                 // 1/w for perspective correction
    float s0, t0;               // Texture tile 0 coords (float)
    float s1, t1;               // Texture tile 1 coords (float)
    float nx, ny, nz;           // Normal vector (after transform)
    float fog;                  // Fog factor
    uint32_t clipFlags;         // Clipping flags
    bool transformed;           // Has this vertex been transformed?
    bool lit;                   // Has this vertex been lit?
};

/// Transformed vertex (after modelview/projection)
/// Uses anonymous unions to support flat (.x), array (.position[0]), and struct (.position.x) access
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension: nameless struct/union
#endif
struct TransformedVertex {
    union {
        struct { float x, y, z, w; };
        float position[4];
    };
    union {
        struct { float s, t; };
        float texCoord[2];
    };
    union {
        struct { float s1, t1; };
        float texCoord1[2];
    };
    union {
        struct { float r, g, b, a; };
        float color[4];
    };
    union {
        struct { float nx, ny, nz; };
        float normal[3];
    };
    float fogFactor;            // Fog interpolation (0=no fog, 1=full fog)

    // Screen-space (post W-divide, viewport transform)
    float screenX, screenY, screenZ;
    float invW;                 // 1/w for perspective correction

    uint32_t clipFlags;         // Clip test results
    bool     reject;            // Vertex rejected by clipping
    bool     modifiedXY;        // True when gSPModifyVertex XYSCREEN was applied
    bool     modifiedZ;         // True when gSPModifyVertex ZSCREEN was applied

    static constexpr uint32_t CLIP_LEFT   = 0x01;
    static constexpr uint32_t CLIP_RIGHT  = 0x02;
    static constexpr uint32_t CLIP_BOTTOM = 0x04;
    static constexpr uint32_t CLIP_TOP    = 0x08;
    static constexpr uint32_t CLIP_NEAR   = 0x10;
    static constexpr uint32_t CLIP_FAR    = 0x20;
    static constexpr uint32_t CLIP_W      = 0x40;  // w <= 0
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/// Triangle (3 vertex indices + RDP state snapshot)
struct Triangle {
    uint32_t v[3];              // Vertex buffer indices
    uint32_t geometryMode;
    int      tileIndex;         // Index into RDP tile descriptors
    bool     textured;
    bool     zbuffered;
    bool     flatShaded;
};

/// Clipped polygon (up to 10 vertices from Sutherland-Hodgman)
struct ClippedPoly {
    TransformedVertex vertices[10];
    int count;
};

/// Rectangle primitive
struct RDPRect {
    float x0, y0, x1, y1;      // Screen-space corners (in 10.2 fixed-point converted to float)
    float s, t;                 // Texture origin
    float dsdx, dtdy;           // Texture step
    int   tileIndex;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Tile Descriptor — canonical definition in TileManager.hpp
//  (TMEMController.hpp has TMEMTileEntry)
// ═══════════════════════════════════════════════════════════════════════════

constexpr int MAX_TILES = 8;

// ═══════════════════════════════════════════════════════════════════════════
//  Color Combiner Modes
// ═══════════════════════════════════════════════════════════════════════════

/// Color combine mode formula:  (A - B) * C + D
/// Two cycles, RGB + Alpha each
struct CombineMode {
    // Cycle 0 RGB: {a_rgb0, b_rgb0, c_rgb0, d_rgb0}
    // Cycle 0 Alpha: {a_alpha0, b_alpha0, c_alpha0, d_alpha0}
    // Cycle 1 RGB: {a_rgb1, b_rgb1, c_rgb1, d_rgb1}
    // Cycle 1 Alpha: {a_alpha1, b_alpha1, c_alpha1, d_alpha1}
    
    uint8_t a_rgb0, b_rgb0, c_rgb0, d_rgb0;
    uint8_t a_alpha0, b_alpha0, c_alpha0, d_alpha0;
    uint8_t a_rgb1, b_rgb1, c_rgb1, d_rgb1;
    uint8_t a_alpha1, b_alpha1, c_alpha1, d_alpha1;
    
    /// Decode from the 64-bit combine mode word
    void Decode(uint64_t word) {
        // High 32 bits contain cycle 0
        uint32_t hi = static_cast<uint32_t>(word >> 32);
        uint32_t lo = static_cast<uint32_t>(word);
        
        a_rgb0   = (hi >> 20) & 0x0F;
        c_rgb0   = (hi >> 15) & 0x1F;
        a_alpha0 = (hi >> 12) & 0x07;
        c_alpha0 = (hi >> 9)  & 0x07;
        a_rgb1   = (hi >> 5)  & 0x0F;
        c_rgb1   = (hi >> 0)  & 0x1F;
        
        b_rgb0   = (lo >> 28) & 0x0F;
        b_rgb1   = (lo >> 24) & 0x0F;
        a_alpha1 = (lo >> 21) & 0x07;
        c_alpha1 = (lo >> 18) & 0x07;
        // gbi.h layout for lo word [17:0]:
        //   d0  at [17:15], Ab0 at [14:12], Ad0 at [11:9]
        //   d1  at [8:6],   Ab1 at [5:3],   Ad1 at [2:0]
        d_rgb0   = (lo >> 15) & 0x07;  // gbi.h: d0  at [17:15]
        b_alpha0 = (lo >> 12) & 0x07;  // gbi.h: Ab0 at [14:12]
        d_alpha0 = (lo >>  9) & 0x07;  // gbi.h: Ad0 at [11:9]
        d_rgb1   = (lo >>  6) & 0x07;  // gbi.h: d1  at [8:6]
        b_alpha1 = (lo >>  3) & 0x07;  // gbi.h: Ab1 at [5:3]
        d_alpha1 = (lo >>  0) & 0x07;  // gbi.h: Ad1 at [2:0]
    }
    
    /// Encode back to a 64-bit combine mode word
    uint64_t Encode() const {
        uint32_t hi = (static_cast<uint32_t>(a_rgb0)   << 20) |
                      (static_cast<uint32_t>(c_rgb0)   << 15) |
                      (static_cast<uint32_t>(a_alpha0) << 12) |
                      (static_cast<uint32_t>(c_alpha0) << 9)  |
                      (static_cast<uint32_t>(a_rgb1)   << 5)  |
                      (static_cast<uint32_t>(c_rgb1)   << 0);
        uint32_t lo = (static_cast<uint32_t>(b_rgb0)   << 28) |
                      (static_cast<uint32_t>(b_rgb1)   << 24) |
                      (static_cast<uint32_t>(a_alpha1) << 21) |
                      (static_cast<uint32_t>(c_alpha1) << 18) |
                      (static_cast<uint32_t>(d_rgb0)   << 15) |
                      (static_cast<uint32_t>(b_alpha0) << 12) |
                      (static_cast<uint32_t>(d_alpha0) << 9)  |
                      (static_cast<uint32_t>(d_rgb1)   << 6)  |
                      (static_cast<uint32_t>(b_alpha1) << 3)  |
                      (static_cast<uint32_t>(d_alpha1) << 0);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    /// Check if this is a simple pass-through (Texel0, 0, Shade, 0)
    bool IsModulateTexShade() const {
        return a_rgb0 == 1 && b_rgb0 == 0 && c_rgb0 == 4 && d_rgb0 == 0;
    }
    
    /// Check if this uses Texel1 (requires 2-cycle mode)
    bool UsesTexel1() const {
        return a_rgb0 == 2 || b_rgb0 == 2 || c_rgb0 == 2 || d_rgb0 == 2 ||
               a_rgb1 == 2 || b_rgb1 == 2 || c_rgb1 == 2 || d_rgb1 == 2;
    }

    /// Check if a specific cycle's RGB formula is identity (output = 0)
    bool IsCycleRGBZero(int cycle) const {
        if (cycle == 0) {
            // (A-B)*C+D = 0 when: A==B and D is zero, or C is zero and D is zero
            bool subIsZero = (a_rgb0 == b_rgb0) || (a_rgb0 >= 8 && b_rgb0 >= 8);
            bool cIsZero = (c_rgb0 >= 16);
            bool dIsZero = (d_rgb0 >= 7);
            return (subIsZero && dIsZero) || (cIsZero && dIsZero);
        }
        bool subIsZero = (a_rgb1 == b_rgb1) || (a_rgb1 >= 8 && b_rgb1 >= 8);
        bool cIsZero = (c_rgb1 >= 16);
        bool dIsZero = (d_rgb1 >= 7);
        return (subIsZero && dIsZero) || (cIsZero && dIsZero);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Blender Mode
// ═══════════════════════════════════════════════════════════════════════════

struct BlenderMode {
    // Cycle 0: P0, A0, M0, B0
    // Cycle 1: P1, A1, M1, B1
    uint8_t p0, a0, m0, b0;
    uint8_t p1, a1, m1, b1;
    
    void Decode(uint32_t othermodeL) {
        // N64 layout: P0[31:30] P1[29:28] A0[27:26] A1[25:24] M0[23:22] M1[21:20] B0[19:18] B1[17:16]
        uint32_t blBits = (othermodeL >> 16) & 0xFFFF;
        p0 = (blBits >> 14) & 0x03;
        p1 = (blBits >> 12) & 0x03;
        a0 = (blBits >> 10) & 0x03;
        a1 = (blBits >> 8)  & 0x03;
        m0 = (blBits >> 6)  & 0x03;
        m1 = (blBits >> 4)  & 0x03;
        b0 = (blBits >> 2)  & 0x03;
        b1 = (blBits >> 0)  & 0x03;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Light Structure
// ═══════════════════════════════════════════════════════════════════════════

/// Raw N64 light structure (hardware layout, 12 bytes)
struct N64LightRaw {
    uint8_t r, g, b, pad0;
    uint8_t r2, g2, b2, pad1;  // Copy (ambient uses this)
    int8_t  dirX, dirY, dirZ;   // Normalized direction (signed)
    uint8_t pad2;
};
static_assert(sizeof(N64LightRaw) == 12, "N64LightRaw must be 12 bytes");

/// Processed N64 light (float colors/direction for computation)
struct N64Light {
    // Raw bytes (loaded from RDRAM)
    uint8_t r, g, b, pad0;
    uint8_t r2, g2, b2, pad1;
    int8_t  dirX, dirY, dirZ;
    uint8_t pad2;

    // Float arrays for computation (filled by light loading routines)
    float color[3];         // Normalized primary color (0-1) — used for even vertices
    float color2[3];        // Normalized secondary color (0-1) — used for odd vertices (N64 dithered flat shading)
    float direction[3];     // Normalized direction vector

    // Point light data (F3DEX2 extended light format)
    float position[3];      // Light position (int16 from RDRAM)
    float ca;               // Constant attenuation (nonzero = point light)
    float la;               // Linear attenuation
    float qa;               // Quadratic attenuation
};

constexpr int MAX_LIGHTS = 12;  // Up to 11 directional + 1 ambient (matches GLideN64)

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Types (for modelview/projection)
// ═══════════════════════════════════════════════════════════════════════════

/// 4x4 float matrix with ROW-major indexing: m[row][col].
/// NOTE: Math/Matrix4x4.hpp provides a richer Matrix4x4 with column-major
/// indexing (m[col][row]) and SIMD.  The two are memory-transposed, so they
/// are NOT interchangeable.  This struct is kept for the N64 emulation core
/// where row-major access matches the RSP pipeline.  The separation is
/// intentional: RSP commands store matrices row-major, while the rendering
/// backend uses column-major for GPU-friendly SIMD operations.
struct Matrix4x4 {
    float m[4][4];  // m[row][col]
    
    static Matrix4x4 Identity() {
        Matrix4x4 result{};
        result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
        return result;
    }
    
    Matrix4x4 operator*(const Matrix4x4& rhs) const {
        Matrix4x4 r{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = m[i][0]*rhs.m[0][j] + m[i][1]*rhs.m[1][j] +
                             m[i][2]*rhs.m[2][j] + m[i][3]*rhs.m[3][j];
        return r;
    }
    
    /// Transform a 4D vector
    void Transform(const float in[4], float out[4]) const {
        for (int i = 0; i < 4; i++)
            out[i] = m[i][0]*in[0] + m[i][1]*in[1] + m[i][2]*in[2] + m[i][3]*in[3];
    }
    
    /// Load from N64 fixed-point format (s15.16, interleaved integer+fraction)
    void LoadFromN64(const uint8_t* rdram, uint32_t address);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Viewport
// ═══════════════════════════════════════════════════════════════════════════

struct N64Viewport {
    float scaleX, scaleY, scaleZ;
    float transX, transY, transZ;
    
    void LoadFromN64(const uint8_t* rdram, uint32_t address) {
        // VP is 4 scale values + 4 translate values, s15.16 packed as s.10.2
        auto Read = [rdram, address](int offset) -> float {
            int16_t raw = static_cast<int16_t>(
                (rdram[address + offset] << 8) | rdram[address + offset + 1]
            );
            return static_cast<float>(raw) / 4.0f;
        };
        scaleX = Read(0);
        scaleY = Read(2);
        scaleZ = Read(4);
        transX = Read(8);
        transY = Read(10);
        transZ = Read(12);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Scissor Rectangle — canonical definition in ScissorManager.hpp
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
//  Fill / Prim / Env / Fog Colors
// ═══════════════════════════════════════════════════════════════════════════

struct RDPColor {
    uint8_t r, g, b, a;
    
    uint32_t ToRGBA32() const {
        return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
               (static_cast<uint32_t>(b) << 8) | a;
    }

    void FromRGBA32(uint32_t rgba) {
        r = (rgba >> 24) & 0xFF;
        g = (rgba >> 16) & 0xFF;
        b = (rgba >> 8)  & 0xFF;
        a =  rgba        & 0xFF;
    }
    
    void FromCommand(uint32_t lo) {
        r = (lo >> 24) & 0xFF;
        g = (lo >> 16) & 0xFF;
        b = (lo >> 8)  & 0xFF;
        a =  lo        & 0xFF;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Texture State
// ═══════════════════════════════════════════════════════════════════════════

struct TextureState {
    bool     enabled;
    uint16_t scaleS;        // s10.5 fixed point
    uint16_t scaleT;
    uint8_t  level;         // Mipmap level count
    uint8_t  tileIndex;     // gSPTexture tile override
    
    float GetScaleSFloat() const {
        float s = static_cast<float>(scaleS) / 65536.0f;
        return (s == 0.0f) ? 1.0f : s;
    }
    float GetScaleTFloat() const {
        float t = static_cast<float>(scaleT) / 65536.0f;
        return (t == 0.0f) ? 1.0f : t;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Frame Buffer Info (for ReadFrameBuffer / GetFrameBufferInfo)
// ═══════════════════════════════════════════════════════════════════════════

struct FrameBufferInfo {
    uint32_t address;       // RDRAM address
    uint32_t width;
    uint32_t height;
    uint32_t size;          // ImageSize
};

constexpr int MAX_FRAME_BUFFERS = 6;

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Command IDs
// ═══════════════════════════════════════════════════════════════════════════

enum class RDPCommandID : uint8_t {
    Nop                 = 0x00,
    // 0x01-0x07 reserved
    ScrollTex           = 0x04,
    
    // Triangles
    TriNoShade          = 0x08,
    TriNoShadeZ         = 0x09,
    TriTexNoShade       = 0x0A,
    TriTexNoShadeZ      = 0x0B,
    TriShade            = 0x0C,
    TriShadeZ           = 0x0D,
    TriTexShade         = 0x0E,
    TriTexShadeZ        = 0x0F,
    
    // Reserved 0x10-0x23
    
    // Texture/Tile commands
    TextureRectangle    = 0x24,
    TextureRectFlip     = 0x25,
    SyncLoad            = 0x26,
    SyncPipe            = 0x27,
    SyncTile            = 0x28,
    SyncFull            = 0x29,
    SetKeyGB            = 0x2A,
    SetKeyR             = 0x2B,
    SetConvert          = 0x2C,
    SetScissor          = 0x2D,
    SetPrimDepth        = 0x2E,
    SetOtherModes       = 0x2F,
    
    // Load/Set Tile
    LoadTLUT            = 0x30,
    SetTileSize         = 0x32,
    LoadBlock           = 0x33,
    LoadTile            = 0x34,
    SetTile             = 0x35,
    FillRect            = 0x36,
    SetFillColor        = 0x37,
    SetFogColor         = 0x38,
    SetBlendColor       = 0x39,
    SetPrimColor        = 0x3A,
    SetEnvColor         = 0x3B,
    SetCombine          = 0x3C,
    SetTextureImage     = 0x3D,
    SetMaskImage        = 0x3E,   // Z buffer image
    SetColorImage       = 0x3F,   // Framebuffer image
};

/// Get the word count for an RDP command
constexpr int GetRDPCommandWordCount(RDPCommandID cmd) {
    int id = static_cast<int>(cmd);
    if (id >= 0x08 && id <= 0x0F) {
        // Triangles: variable length based on shade/tex/z attributes
        bool shade   = (id & 0x04) != 0;
        bool texture = (id & 0x02) != 0;
        bool zbuffer = (id & 0x01) != 0;
        int words = 4; // Base edge coefficients
        if (shade) words += 8;
        if (texture) words += 8;
        if (zbuffer) words += 4;
        return words * 2; // Each "word" is actually a 32-bit pair → total in 32-bit words
    }
    if (id == 0x24 || id == 0x25) return 4; // TextureRect
    return 2; // All other commands are 64-bit (2 words)
}

/// Calculate triangle command size in bytes
constexpr int GetTriangleSizeBytes(bool shade, bool texture, bool zbuffer) {
    [[maybe_unused]] int coeffs = 2; // edge coefficients (2 64-bit words = 16 bytes) - actually more complex
    // Real sizes: base = 32 bytes, shade +64, tex +64, z +16
    int bytes = 32;
    if (shade) bytes += 64;
    if (texture) bytes += 64;
    if (zbuffer) bytes += 16;
    return bytes;
}

} // namespace Nova64::Video
