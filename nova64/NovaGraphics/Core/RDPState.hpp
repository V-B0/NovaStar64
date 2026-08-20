/**
 * @file RDPState.hpp
 * @brief Complete RDP hardware state machine
 *
 * Represents the entire state of the N64 Reality Display Processor—tile descriptors,
 * other modes, combiner settings, blender config, colors, scissor, framebuffer/zbuffer
 * targets, TMEM contents, and derived state (dirty flags, hash for shader caching).
 */

#pragma once

#include "N64Types.hpp"
#include "N64Math.hpp"
#include "../Common/BitUtils.hpp"

#include <cstdint>
#include <cstring>
#include <array>
#include <atomic>
#include <functional>

namespace Nova64::Video {

// ──────────────────────────────────────────────────────────────
//  Single RDP tile descriptor (mirrors hardware state)
// ──────────────────────────────────────────────────────────────
struct TileDescriptor {
    // From SetTile command
    uint8_t  format  = 0;   // 0=RGBA, 1=YUV, 2=CI, 3=IA, 4=I
    uint8_t  size    = 0;   // 0=4bit, 1=8bit, 2=16bit, 3=32bit
    uint16_t line    = 0;   // Stride in 64-bit words
    uint16_t tmemAddr = 0;  // TMEM address (0-511, in 64-bit words)
    uint8_t  palette = 0;   // Palette index (CI4 only, 0-15)

    // Clamp/Mirror/Mask for S and T
    uint8_t  clampS  = 0;   // 0=wrap, 1=mirror, 2=clamp
    uint8_t  maskS   = 0;   // Mask bits (0=no mask)
    uint8_t  shiftS  = 0;   // Shift (0-15)
    uint8_t  clampT  = 0;
    uint8_t  maskT   = 0;
    uint8_t  shiftT  = 0;

    // From SetTileSize command (10.2 fixed point)
    int32_t  sl = 0;  // S low (upper-left)
    int32_t  tl = 0;  // T low (upper-left)
    int32_t  sh = 0;  // S high (lower-right)
    int32_t  th = 0;  // T high (lower-right)

    // Convenience
    float GetSL() const { return sl / 4.0f; }
    float GetTL() const { return tl / 4.0f; }
    float GetSH() const { return sh / 4.0f; }
    float GetTH() const { return th / 4.0f; }

    int GetWidth() const { return ((sh - sl) >> 2) + 1; }
    int GetHeight() const { return ((th - tl) >> 2) + 1; }

    int GetBitsPerPixel() const {
        static const int bpp[] = { 4, 8, 16, 32 };
        return bpp[size & 3];
    }

    int GetBytesPerRow() const { return line * 8; } // line is in 64-bit words
    int GetTMEMSizeBytes() const { return GetBytesPerRow() * GetHeight(); }

    uint16_t GetTMEMWordAddr() const { return tmemAddr; }
    uint32_t GetTMEMByteAddr() const { return static_cast<uint32_t>(tmemAddr) * 8; }

    bool IsCI() const { return format == 2; }
    bool IsIA() const { return format == 3; }
    bool IsI() const { return format == 4; }
    bool IsRGBA() const { return format == 0; }
    bool IsYUV() const { return format == 1; }

    bool IsMirrored_S() const { return (clampS & 1) != 0; }
    bool IsClamped_S() const { return (clampS & 2) != 0; }
    bool IsMirrored_T() const { return (clampT & 1) != 0; }
    bool IsClamped_T() const { return (clampT & 2) != 0; }

    bool operator==(const TileDescriptor& o) const {
        return format == o.format && size == o.size && line == o.line &&
               tmemAddr == o.tmemAddr && palette == o.palette &&
               clampS == o.clampS && maskS == o.maskS && shiftS == o.shiftS &&
               clampT == o.clampT && maskT == o.maskT && shiftT == o.shiftT &&
               sl == o.sl && tl == o.tl && sh == o.sh && th == o.th;
    }
    bool operator!=(const TileDescriptor& o) const { return !(*this == o); }
};

// ──────────────────────────────────────────────────────────────
//  Scissor rectangle (11.2 fixed-point as stored by RDP)
// ──────────────────────────────────────────────────────────────
struct ScissorRect {
    int32_t xh = 0; // Upper-left X (11.2 fixed)
    int32_t yh = 0; // Upper-left Y (11.2 fixed)
    int32_t xl = 0; // Lower-right X (11.2 fixed)
    int32_t yl = 0; // Lower-right Y (11.2 fixed)
    uint8_t mode = 0; // 0 = Even/odd, 1 = BusyBit

    float GetXhF() const { return xh / 4.0f; }
    float GetYhF() const { return yh / 4.0f; }
    float GetXlF() const { return xl / 4.0f; }
    float GetYlF() const { return yl / 4.0f; }

    float GetWidthF() const { return (xl - xh) / 4.0f; }
    float GetHeightF() const { return (yl - yh) / 4.0f; }

    int GetLeft() const   { return xh >> 2; }
    int GetTop() const    { return yh >> 2; }
    int GetRight() const  { return (xl + 3) >> 2; }
    int GetBottom() const { return (yl + 3) >> 2; }
    int GetPixelWidth() const  { return GetRight() - GetLeft(); }
    int GetPixelHeight() const { return GetBottom() - GetTop(); }

    bool IsValid() const { return xl > xh && yl > yh; }
    bool Contains(int x, int y) const {
        return x >= GetLeft() && x < GetRight() && y >= GetTop() && y < GetBottom();
    }
    bool ContainsF(float x, float y) const {
        return x >= GetXhF() && x < GetXlF() && y >= GetYhF() && y < GetYlF();
    }

    bool operator==(const ScissorRect& o) const {
        return xh == o.xh && yh == o.yh && xl == o.xl && yl == o.yl && mode == o.mode;
    }
    bool operator!=(const ScissorRect& o) const { return !(*this == o); }
};

// ═══════════════════════════════════════════════════════════════════════════
//  State Change Flags (for incremental GPU updates)
// ═══════════════════════════════════════════════════════════════════════════

enum class RDPDirtyFlag : uint32_t {
    None            = 0,
    CombineMode     = (1 << 0),
    OtherModes      = (1 << 1),
    BlendMode       = (1 << 2),
    Scissor         = (1 << 3),
    Tiles           = (1 << 4),
    FillColor       = (1 << 5),
    FogColor        = (1 << 6),
    BlendColor      = (1 << 7),
    PrimColor       = (1 << 8),
    EnvColor        = (1 << 9),
    PrimDepth       = (1 << 10),
    ColorImage      = (1 << 11),
    ZBufferImage    = (1 << 12),
    TextureImage    = (1 << 13),
    GeometryMode    = (1 << 14),
    Viewport        = (1 << 15),
    Lights          = (1 << 16),
    Matrix          = (1 << 17),
    TextureState    = (1 << 18),
    VertexBuffer    = (1 << 19),
    All             = 0xFFFFF,
};

inline RDPDirtyFlag operator|(RDPDirtyFlag a, RDPDirtyFlag b) {
    return static_cast<RDPDirtyFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline RDPDirtyFlag operator&(RDPDirtyFlag a, RDPDirtyFlag b) {
    return static_cast<RDPDirtyFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline RDPDirtyFlag& operator|=(RDPDirtyFlag& a, RDPDirtyFlag b) {
    a = a | b; return a;
}
inline bool HasFlag(RDPDirtyFlag flags, RDPDirtyFlag test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Color Image / Z-Buffer Image Descriptors
// ═══════════════════════════════════════════════════════════════════════════

struct ImageDescriptor {
    uint32_t    address;    // RDRAM address (24-bit)
    uint16_t    width;      // Pixels per row
    ImageFormat format;
    ImageSize   size;
    
    bool operator==(const ImageDescriptor& o) const {
        return address == o.address && width == o.width &&
               format == o.format && size == o.size;
    }
    bool operator!=(const ImageDescriptor& o) const { return !(*this == o); }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Combiner Hash Key (for shader caching)
// ═══════════════════════════════════════════════════════════════════════════

struct CombinerKey {
    uint64_t combineMode;       // Raw 64-bit combine word
    uint32_t otherModeH;        // High word of other modes (texture-related bits)
    uint32_t otherModeL;        // Low word (blender, alpha compare, z mode)
    uint32_t geometryMode;      // For fog, lighting
    
    bool operator==(const CombinerKey& o) const {
        return combineMode == o.combineMode && otherModeH == o.otherModeH &&
               otherModeL == o.otherModeL && geometryMode == o.geometryMode;
    }
    
    struct Hash {
        size_t operator()(const CombinerKey& k) const {
            size_t h = std::hash<uint64_t>{}(k.combineMode);
            h ^= std::hash<uint32_t>{}(k.otherModeH) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.otherModeL) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.geometryMode) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
};

// ═══════════════════════════════════════════════════════════════════════════
//  TMEM State
// ═══════════════════════════════════════════════════════════════════════════

struct TMEMState {
    uint8_t data[TMEM_SIZE];    // 4 KB TMEM
    
    // Track what ranges are loaded for cache invalidation
    struct LoadedRegion {
        uint32_t rdramAddr;     // Source address in RDRAM
        uint16_t tmemAddr;      // Destination offset in TMEM
        uint16_t sizeBytes;     // Size loaded
        uint32_t crc;           // CRC of loaded data
        bool     valid;
    };
    
    static constexpr int MAX_REGIONS = 16;
    LoadedRegion regions[MAX_REGIONS];
    int regionCount = 0;
    
    void Reset(bool cold = true) {
        // N64 hardware does NOT clear TMEM between RSP tasks.
        // Only clear TMEM on a true cold reset (emulator init / ROM change).
        if (cold) {
            std::memset(data, 0, TMEM_SIZE);
            regionCount = 0;
        }
    }
    
    /// Record a load into TMEM
    void RecordLoad(uint32_t rdramAddr, uint16_t tmemAddr, uint16_t size, uint32_t crc) {
        // See if this overlaps an existing region
        for (int i = 0; i < regionCount; i++) {
            if (regions[i].tmemAddr == tmemAddr) {
                regions[i] = { rdramAddr, tmemAddr, size, crc, true };
                return;
            }
        }
        if (regionCount < MAX_REGIONS) {
            regions[regionCount++] = { rdramAddr, tmemAddr, size, crc, true };
        }
    }
    
    /// Check if a TMEM region matches a given CRC (for texture cache hits)
    bool IsRegionValid(uint16_t tmemAddr, uint32_t expectedCRC) const {
        for (int i = 0; i < regionCount; i++) {
            if (regions[i].tmemAddr == tmemAddr && regions[i].valid) {
                return regions[i].crc == expectedCRC;
            }
        }
        return false;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Vertex Buffer State (RSP vertex cache)
// ═══════════════════════════════════════════════════════════════════════════

// GLideN64 parity (GraphicsDrawer.h: VERTBUFF_SIZE = 256, VERTBUFF_MASK = 255).
// Power-of-two so vertex indices can be MASKED rather than range-rejected --
// see the note at DrawTriangle's index handling. 80 was both too small for
// ucodes with a larger cache and not maskable.
constexpr int VERTEX_BUFFER_SIZE = 256;
constexpr int VERTEX_BUFFER_MASK = VERTEX_BUFFER_SIZE - 1;

struct VertexBufferState {
    TransformedVertex vertices[VERTEX_BUFFER_SIZE];
    bool loaded[VERTEX_BUFFER_SIZE];
    int  count;
    
    void Reset() {
        count = 0;
        std::memset(loaded, 0, sizeof(loaded));
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  RSP Geometry State (set by GBI commands)
// ═══════════════════════════════════════════════════════════════════════════

struct GeometryState {
    // Matrix stacks
    Matrix4x4 modelView;
    Matrix4x4 projection;
    Matrix4x4 combinedMatrix;      // projection * modelView (alias: combined)
    bool      combinedDirty;
    
    static constexpr int MAX_MATRIX_STACK = 32;
    Matrix4x4 modelViewStack[MAX_MATRIX_STACK];
    int       modelViewStackDepth;
    Matrix4x4 projectionStack[2];
    int       projectionStackDepth;
    
    // Vertex buffer (RSP vertex cache)
    TransformedVertex vertexBuffer[VERTEX_BUFFER_SIZE];
    bool              vertexLoaded[VERTEX_BUFFER_SIZE];
    int               vertexCount;

    // NOVA_F3D_VTRACE diagnostics only: per-slot "which LoadVertices call last
    // wrote this slot" stamp, to detect a triangle mixing vertices from very
    // different points in the same display list (stale cache-slot reuse).
    uint32_t          vertexGeneration[VERTEX_BUFFER_SIZE];
    uint32_t          vertexLoadCounter;

    // NOVA_F3D_VTRACE diagnostics only: counts Reset() calls (~1 per display
    // list / frame). Deliberately NOT cleared by Reset() itself, so it gives
    // an absolute, monotonic frame index usable to scope a triangle dump to a
    // specific point in the run.
    uint64_t          frameSeq = 0;

    // Viewport
    N64Viewport viewport;
    
    // Geometry mode flags
    uint32_t geometryMode;
    
    // Lighting
    N64Light lights[MAX_LIGHTS];
    int      numLights;             // Directional lights (0-7)
    N64Light ambient;
    
    // Texture state
    TextureState texture;
    
    // Fog parameters  
    int16_t fogMultiplier;
    int16_t fogOffset;

    // Guard-band clip ratio (set by gSPClipRatio / MW_CLIP)
    // N64 RSP clips at w*clipRatio instead of w, avoiding unnecessary
    // clipping for triangles near screen edges.
    float clipRatio = 1.0f;

    // Widescreen hack: scale factor for horizontal expansion.
    // 1.0 = native 4:3, 4/3 ≈ 1.333 = 16:9 widescreen.
    // Applied to projection matrix (inverse), viewport, and clip flags.
    float widescreenScale = 1.0f;
    
    // Fog usage mode (set by microcode handler):
    // 0 = no fog, 1 = standard fog (alpha), 2 = F3DAM mode 2 (RGB)
    int fogUsage = 0;
    
    // LookAt vectors for environment map texture generation
    float    lookat[2][3];        // [0]=X (right), [1]=Y (up)
    bool     lookatEnabled;
    
    // Segment table (for address translation)
    uint32_t segments[16];
    
    void Reset() {
        ++frameSeq;
        modelView = Matrix4x4::Identity();
        projection = Matrix4x4::Identity();
        combinedMatrix = Matrix4x4::Identity();
        combinedDirty = true;
        modelViewStackDepth = 0;
        projectionStackDepth = 0;
        modelViewStack[0] = Matrix4x4::Identity();
        projectionStack[0] = Matrix4x4::Identity();
        vertexCount = 0;
        std::memset(vertexLoaded, 0, sizeof(vertexLoaded));
        std::memset(vertexGeneration, 0, sizeof(vertexGeneration));
        vertexLoadCounter = 0;
        geometryMode = 0;
        numLights = 0;
        std::memset(&ambient, 0, sizeof(ambient));
        std::memset(&texture, 0, sizeof(texture));
        fogMultiplier = 0;
        fogOffset = 0;
        fogUsage = 0;
        clipRatio = 1.0f;
        // Default lookat: match GLideN64 (lookat[0]=Y-up, lookat[1]=X-right)
        lookat[0][0] = 0.0f; lookat[0][1] = 1.0f; lookat[0][2] = 0.0f;
        lookat[1][0] = 1.0f; lookat[1][1] = 0.0f; lookat[1][2] = 0.0f;
        lookatEnabled = true;
        std::memset(segments, 0, sizeof(segments));
        // Default viewport: 320x240 centered (N64 standard)
        viewport.scaleX =  160.0f;
        viewport.scaleY = -120.0f;
        viewport.scaleZ =   0.5f;
        viewport.transX =  160.0f;
        viewport.transY =  120.0f;
        viewport.transZ =   0.5f;
    }
    
    /// Resolve segmented address to physical RDRAM address
    uint32_t ResolveAddress(uint32_t segAddr) const {
        uint8_t seg = (segAddr >> 24) & 0x0F;
        return segments[seg] + (segAddr & 0x00FFFFFF);
    }
    
    /// Recompute combined = modelView * projection (row-vector)
    void UpdateCombinedMatrix() {
        if (combinedDirty) {
            // Row-vector convention: v_clip = v * Combined
            // combined = MV * P
            MatrixOps::Multiply(modelViewStack[modelViewStackDepth],
                               projectionStack[projectionStackDepth],
                               combinedMatrix);
            combinedDirty = false;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Complete RDP State
// ═══════════════════════════════════════════════════════════════════════════

struct RDPState {
    // ── Other Modes ──
    uint32_t otherModeH;            // High word (GBI-level, updated by 0xE2/0xE3)
    uint32_t otherModeL;            // Low word  (GBI-level, updated by 0xE2/0xE3)
    uint32_t rdpOtherModeH;         // RDP-level copy (updated only by 0xEF SetOtherModes)
    uint32_t rdpOtherModeL;         // RDP-level copy (updated only by 0xEF SetOtherModes)
    
    // ── Derived from Other Modes ──
    CycleType       cycleType;
    TextureFilter   textureFilter;
    TextureLUT      textureLUT;
    ZMode           zMode;
    CvgDst          coverageDst;
    AlphaCompare    alphaCompare;
    bool            perspCorrect;
    bool            textureDetail;
    bool            textureLOD;
    bool            aaEnable;
    bool            zCompare;
    bool            zUpdate;
    bool            imageRead;
    bool            clearOnCvg;
    bool            cvgXAlpha;
    bool            alphaCvgSel;
    bool            forceBlend;
    
    // ── Combine Mode ──
    CombineMode combineMode;
    uint64_t    combineModeRaw;     // Raw 64-bit word
    
    // ── Blender ──
    BlenderMode blenderMode;
    
    // ── Tiles ──
    std::array<TileDescriptor, MAX_TILES> tiles;
    
    // ── TMEM ──
    TMEMState tmem;
    
    // ── Colors ──
    RDPColor fillColor;
    uint32_t fillColorRaw = 0;   // Raw 32-bit fill color register (decoded at draw-time)
    RDPColor fogColor;
    RDPColor blendColor;
    RDPColor primColor;
    RDPColor envColor;
    float    primLODFrac;           // Primitive LOD fraction (0-1)
    float    primLODMin;
    
    // ── Texture Scrolling ──
    float scrollS[8] = {0};
    float scrollT[8] = {0};
    
    // ── Per-tile RDRAM source address (set by LoadBlock/LoadTile) ──
    // Tracks which textureImage.address was active when each tile was loaded.
    // Used by PopulateTextureInfo so tile0/tile1 get the correct RDRAM source
    // even in 2-cycle mode with two different texture loads.
    uint32_t tileLoadAddress[MAX_TILES] = {0};

    // ── Per-TMEM-slot RDRAM source address (set by LoadBlock/LoadTile) ──
    // Indexed by TMEM word address (0–511 for 4 KB TMEM / 8-byte words).
    // N64 games use a dedicated "loading tile" (typically tile 7) for G_LOADBLOCK
    // which sets tileLoadAddress[7], but the render tile (tile 0) is distinct and
    // tileLoadAddress[0] stays zero.  Both tiles share the same tmemAddr so
    // tmemSourceAddress[tmemAddr] bridges them correctly.  Also handles the case
    // where a G_SETTEXTUREIMAGE for TLUT loading changes textureImage.address
    // between the main texture load and the draw call.
    static constexpr int TMEM_WORDS = 512;
    uint32_t tmemSourceAddress[TMEM_WORDS] = {0};

    // ── Per-TMEM-slot LOAD INFO (mirrors GLideN64's gDP.loadInfo[]) ──
    // Records HOW the data in each TMEM slot got there. GLideN64's
    // _calcTileSizes() (Textures.cpp) has two entirely different size
    // derivations selected by this: a LOADTYPE_TILE branch that sizes the
    // texture from the region actually LOADED (loadWidth/loadHeight/texWidth,
    // rescaled when the load size differs from the tile size), and the
    // mask/clamp branch used otherwise. We previously implemented only the
    // mask/clamp branch, so any LoadTile whose loaded region disagrees with the
    // tile's mask-derived extent produced wrong texture dimensions -- and since
    // width/height drive scaleS/scaleT, wrong dimensions map the texture across
    // the wrong UV range, i.e. visible stretching/squashing.
    enum : uint8_t { LOADTYPE_BLOCK = 0, LOADTYPE_TILE = 1 };
    uint8_t  tmemLoadType[TMEM_WORDS]  = {0};
    uint16_t tmemLoadWidth[TMEM_WORDS] = {0};   // texels actually loaded per row
    uint16_t tmemLoadHeight[TMEM_WORDS]= {0};   // rows actually loaded
    uint16_t tmemLoadTexWidth[TMEM_WORDS] = {0}; // SetTextureImage width
    uint8_t  tmemLoadSize[TMEM_WORDS]  = {0};   // SetTextureImage pixel size
    
    // ── Scissor ──
    ScissorRect scissor;
    
    // ── Prim Depth ──
    uint16_t primDepth;
    uint16_t primDeltaZ;
    
    // ── Image Targets ──
    ImageDescriptor colorImage;     // Framebuffer
    ImageDescriptor zbufferImage;   // Z-buffer
    ImageDescriptor textureImage;   // Source for loads
    
    // ── Convert Coefficients ──
    int16_t convertK[6];            // K0-K5 for YUV→RGB
    
    // ── Geometry State (RSP) ──
    GeometryState geometry;
    
    // ── Key (chroma key) ──
    struct {
        uint16_t widthR, widthG, widthB;    // 12-bit fields
        uint8_t centerR, centerG, centerB;
        uint8_t scaleR, scaleG, scaleB;
    } chromaKey;
    
    // ── Dirty Flags ──
    RDPDirtyFlag dirtyFlags;
    
    // ── Statistics ──
    uint64_t triangleCount;
    uint64_t rectCount;
    uint64_t textureLoadCount;
    uint64_t syncCount;
    
    // ═════════════════════════════════════════════════════════════════
    //  Methods
    // ═════════════════════════════════════════════════════════════════
    
    void Reset(bool cold = true) {
        otherModeH = 0;
        otherModeL = 0;
        rdpOtherModeH = 0;
        rdpOtherModeL = 0;
        cycleType = CycleType::Cycle1;
        textureFilter = TextureFilter::Point;
        textureLUT = TextureLUT::None;
        zMode = ZMode::Opaque;
        coverageDst = CvgDst::Clamp;
        alphaCompare = AlphaCompare::None;
        perspCorrect = false;
        textureDetail = false;
        textureLOD = false;
        aaEnable = false;
        zCompare = false;
        zUpdate = false;
        imageRead = false;
        clearOnCvg = false;
        cvgXAlpha = false;
        alphaCvgSel = false;
        forceBlend = false;
        
        combineModeRaw = 0;
        std::memset(&combineMode, 0, sizeof(combineMode));
        std::memset(&blenderMode, 0, sizeof(blenderMode));
        
        for (auto& tile : tiles)
            std::memset(&tile, 0, sizeof(tile));
        
        tmem.Reset(cold);
        
        // On real N64 hardware, color registers persist across RSP task
        // boundaries (there is no hardware reset between display lists).
        // Only clear them on a cold reset so that inherited env/prim/fog/blend
        // colors work correctly for games that rely on state carrying over.
        if (cold) {
            fillColor = {};
            fillColorRaw = 0;
            fogColor = {};
            blendColor = {};
            primColor = {};
            envColor = {};
            primLODFrac = 0;
            primLODMin = 0;
        }
        
        scissor = { 0, 0, 320, 240, 2 };
        primDepth = 0;
        primDeltaZ = 0;
        
        colorImage = {};
        zbufferImage = {};
        textureImage = {};
        
        std::memset(convertK, 0, sizeof(convertK));
        std::memset(&chromaKey, 0, sizeof(chromaKey));
        // tileLoadAddress and tmemSourceAddress track which RDRAM address fed
        // each TMEM slot/tile. Preserve on warm reset so that textures loaded in
        // a prior DL are still identifiable when drawn in the next DL.
        if (cold) {
            std::memset(tileLoadAddress, 0, sizeof(tileLoadAddress));
            std::memset(tmemSourceAddress, 0, sizeof(tmemSourceAddress));
            std::memset(tmemLoadType, 0, sizeof(tmemLoadType));
            std::memset(tmemLoadWidth, 0, sizeof(tmemLoadWidth));
            std::memset(tmemLoadHeight, 0, sizeof(tmemLoadHeight));
            std::memset(tmemLoadTexWidth, 0, sizeof(tmemLoadTexWidth));
            std::memset(tmemLoadSize, 0, sizeof(tmemLoadSize));
        }
        std::memset(scrollS, 0, sizeof(scrollS));
        std::memset(scrollT, 0, sizeof(scrollT));
        
        dirtyFlags = RDPDirtyFlag::All;
        
        triangleCount = 0;
        rectCount = 0;
        textureLoadCount = 0;
        syncCount = 0;
        
        geometry.Reset();
    }
    
    /// Parse otherModeH/L into human-readable derived fields
    void UpdateDerivedOtherModes() {
        cycleType     = static_cast<CycleType>((otherModeH >> 20) & 0x03);
        textureFilter = static_cast<TextureFilter>((otherModeH >> 12) & 0x03);
        textureLUT    = static_cast<TextureLUT>((otherModeH >> 14) & 0x03);
        perspCorrect  = (otherModeH & OTHERMODE_TEXTPERSP) != 0;
        textureLOD    = (otherModeH & OTHERMODE_TEXTLOD) != 0;
        textureDetail = ((otherModeH & OTHERMODE_TEXTDETAIL_MASK) >> 17) != 0;
        
        alphaCompare  = static_cast<AlphaCompare>(otherModeL & 0x03);
        zMode         = static_cast<ZMode>((otherModeL >> 10) & 0x03);
        coverageDst   = static_cast<CvgDst>((otherModeL >> 8) & 0x03);
        aaEnable      = (otherModeL & OTHERMODE_AA_EN) != 0;
        zCompare      = (otherModeL & OTHERMODE_Z_CMP) != 0;
        zUpdate       = (otherModeL & OTHERMODE_Z_UPD) != 0;
        imageRead     = (otherModeL & OTHERMODE_IM_RD) != 0;
        clearOnCvg    = (otherModeL & OTHERMODE_CLR_ON_CVG) != 0;
        cvgXAlpha     = (otherModeL & OTHERMODE_CVG_X_ALPHA) != 0;
        alphaCvgSel   = (otherModeL & OTHERMODE_ALPHA_CVG_SEL) != 0;
        forceBlend    = (otherModeL & OTHERMODE_FORCE_BL) != 0;
        
        blenderMode.Decode(otherModeL);
    }
    
    /// Get combiner key for shader caching
    CombinerKey GetCombinerKey(uint32_t geoMode) const {
        return { combineModeRaw, otherModeH, otherModeL, geoMode };
    }
    
    /// Mark a state component as dirty
    void MarkDirty(RDPDirtyFlag flag) {
        dirtyFlags |= flag;
    }
    
    /// Check and clear dirty flag
    bool CheckAndClearDirty(RDPDirtyFlag flag) {
        if (HasFlag(dirtyFlags, flag)) {
            dirtyFlags = static_cast<RDPDirtyFlag>(
                static_cast<uint32_t>(dirtyFlags) & ~static_cast<uint32_t>(flag)
            );
            return true;
        }
        return false;
    }
    
    /// Clear all dirty flags (after flushing state to GPU)
    void ClearDirty() {
        dirtyFlags = RDPDirtyFlag::None;
    }
    
    /// Check if any state is dirty
    bool IsDirty() const {
        return dirtyFlags != RDPDirtyFlag::None;
    }
    
    /// Compute a combined hash of all state for debugging
    uint32_t ComputeStateHash() const {
        uint32_t hash = 0;
        hash = CRC32_Fast(reinterpret_cast<const uint8_t*>(&otherModeH), 4, hash);
        hash = CRC32_Fast(reinterpret_cast<const uint8_t*>(&otherModeL), 4, hash);
        hash = CRC32_Fast(reinterpret_cast<const uint8_t*>(&combineModeRaw), 8, hash);
        hash = CRC32_Fast(reinterpret_cast<const uint8_t*>(&fillColor), 4, hash);
        return hash;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Complete RSP + RDP State (the "big" state context)
// ═══════════════════════════════════════════════════════════════════════════

struct VideoState {
    RDPState        rdp;

    // Frame info
    uint32_t frameCount;
    bool     displayListActive;

    void Reset(bool cold = true) {
        rdp.Reset(cold);
        frameCount = 0;
        displayListActive = false;
    }
};

} // namespace Nova64::Video
