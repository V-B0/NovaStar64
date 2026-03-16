/**
 * @file S2DEX.hpp
 * @brief S2DEX/S2DEX2 sprite microcode handler
 *
 * The S2DEX family provides 2D sprite rendering capabilities for the N64 RSP.
 * Used by games that need 2D overlays, HUD elements, or full 2D rendering
 * (e.g., Yoshi's Story menus, Paper Mario 2D sections, various sports titles).
 *
 * S2DEX commands handle:
 * - Background (BG) sprites: full-screen/large scrolling backgrounds
 * - Object (OBJ) sprites: positioned/scaled/rotated 2D objects
 * - Sprite selection and texture assignment
 * - 2D matrix operations (translate, scale, rotate)
 */

#pragma once

#include "GBIHandler.hpp"
#include "../Core/N64Types.hpp"

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  S2DEX Sprite Structures
// ═══════════════════════════════════════════════════════════════════════════

/// Background sprite definition (uObjBg)
struct ObjBgSprite {
    uint16_t    imageX;         // Texture X position (s10.5)
    uint16_t    imageY;         // Texture Y position (s10.5)
    uint16_t    imageW;         // Texture width (10.2)
    uint16_t    imageH;         // Texture height (10.2)
    
    int16_t     frameX;         // Screen X position (s10.2)
    int16_t     frameY;         // Screen Y position (s10.2)
    uint16_t    frameW;         // Screen width (10.2)
    uint16_t    frameH;         // Screen height (10.2)
    
    uint32_t    imageAddr;      // RDRAM address of texture data
    ImageFormat imageFormat;    // Texture format (RGBA, CI, etc.)
    ImageSize   imageSize;      // Pixel depth
    uint16_t    imagePal;       // Palette index for CI textures
    bool        imageFlip;      // Horizontal flip
    
    uint16_t    scaleW;         // Horizontal scale (u5.10)
    uint16_t    scaleH;         // Vertical scale (u5.10)
    
    int32_t     imageYorig;     // Y origin for scroll (s20.5)
};

/// Object sprite definition (uObjSprite)
struct ObjSprite {
    int16_t     objX;           // Screen X (s10.2)
    int16_t     objY;           // Screen Y (s10.2)
    uint16_t    scaleW;         // H scale (u5.10)
    uint16_t    scaleH;         // V scale (u5.10)
    uint16_t    imageW;         // Texture width (10.5)
    uint16_t    imageH;         // Texture height (10.5)
    
    uint16_t    imageSiz;       // Pixel size
    uint16_t    imageFmt;       // Texture format
    uint16_t    imageStride;    // Stride in 64-bit words
    uint16_t    imageAdrs;      // TMEM address
    
    uint32_t    imageAddr;      // RDRAM address
    uint8_t     imagePal;       // Palette index
    uint8_t     imageFlags;     // Flip flags
};

/// 2D Matrix (uObjMtx — 2D affine transform)
struct ObjMatrix {
    int32_t     A;              // s15.16
    int32_t     B;              // s15.16
    int32_t     C;              // s15.16
    int32_t     D;              // s15.16
    int16_t     X;              // s10.2 translation
    int16_t     Y;              // s10.2 translation
    uint16_t    BaseScaleX;     // u5.10
    uint16_t    BaseScaleY;     // u5.10
};

// ═══════════════════════════════════════════════════════════════════════════
//  S2DEX Command IDs
// ═══════════════════════════════════════════════════════════════════════════

namespace S2DEXCmd {
    // S2DEX1 commands
    constexpr uint8_t G_OBJ_SPRITE      = 0x04;
    constexpr uint8_t G_OBJ_RECTANGLE   = 0x09;
    constexpr uint8_t G_OBJ_MOVEMEM     = 0x01;
    constexpr uint8_t G_SELECT_DL       = 0xB0;
    constexpr uint8_t G_OBJ_RENDERMODE  = 0xB1;
    constexpr uint8_t G_OBJ_LOADTXTR    = 0xC1;
    constexpr uint8_t G_OBJ_LDTX_SPRITE = 0xC2;
    constexpr uint8_t G_OBJ_LDTX_RECT   = 0xC3;
    constexpr uint8_t G_OBJ_LDTX_RECT_R = 0xC4;
    constexpr uint8_t G_BG_1CYC         = 0x02;
    constexpr uint8_t G_BG_COPY         = 0x03;
    constexpr uint8_t G_OBJ_RECTANGLE_R = 0xDA;
    
    // S2DEX2 commands (different encoding)
    namespace V2 {
        constexpr uint8_t G_OBJ_SPRITE      = 0x04;
        constexpr uint8_t G_OBJ_RECTANGLE   = 0x09;
        constexpr uint8_t G_OBJ_MOVEMEM     = 0xDC;
        constexpr uint8_t G_SELECT_DL       = 0x04;
        constexpr uint8_t G_OBJ_RENDERMODE  = 0xDA;
        constexpr uint8_t G_OBJ_LOADTXTR    = 0xC1;
        constexpr uint8_t G_OBJ_LDTX_SPRITE = 0xC2;
        constexpr uint8_t G_OBJ_LDTX_RECT   = 0xC3;
        constexpr uint8_t G_OBJ_LDTX_RECT_R = 0xC4;
        constexpr uint8_t G_BG_1CYC         = 0x01;
        constexpr uint8_t G_BG_COPY         = 0x02;
        constexpr uint8_t G_OBJ_RECTANGLE_R = 0x03;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  S2DEX Handler
// ═══════════════════════════════════════════════════════════════════════════

class S2DEXHandler : public GBIHandler {
public:
    S2DEXHandler() = default;
    ~S2DEXHandler() override = default;
    
    void Initialize(const MicrocodeInfo& info) override;
    MicrocodeType GetType() const override { return m_type; }
    GBIVersion GetGBIVersion() const override { return m_gbiVersion; }
    const char* GetName() const override { return m_name; }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    void OnFrameBegin(DListContext& ctx) override;
    
protected:
    MicrocodeType m_type = MicrocodeType::S2DEX;
    GBIVersion m_gbiVersion = GBIVersion::GBI_S2DEX;
    const char* m_name = "S2DEX";
    
    // Current 2D state
    ObjMatrix m_objMatrix;
    bool m_objMatrixValid = false;
    
    // ─── Command Handlers ────────────────────────────────────────────
    void Cmd_BG_1CYC(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_BG_COPY(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_SPRITE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_RECTANGLE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_RECTANGLE_R(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_RENDERMODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_LOADTXTR(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_LDTX_SPRITE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_OBJ_LDTX_RECT(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_SELECT_DL(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    // ─── Sprite Rendering Helpers ────────────────────────────────────
    
    /**
     * Read a background sprite structure from RDRAM.
     */
    bool ReadBgSprite(DListContext& ctx, uint32_t address, ObjBgSprite& bg);
    
    /**
     * Read an object sprite structure from RDRAM.
     */
    bool ReadObjSprite(DListContext& ctx, uint32_t address, ObjSprite& obj);
    
    /**
     * Read a 2D matrix from RDRAM.
     */
    bool ReadObjMatrix(DListContext& ctx, uint32_t address, ObjMatrix& mtx);
    
    /**
     * Render a background sprite as a series of texture rectangles.
     */
    void RenderBgSprite(DListContext& ctx, const ObjBgSprite& bg, bool copyMode);
    
    /**
     * Render an object sprite.
     */
    void RenderObjSprite(DListContext& ctx, const ObjSprite& obj);
    
    /**
     * Load a sprite's texture data into TMEM.
     */
    void LoadSpriteTexture(DListContext& ctx, uint32_t address);
};

// ═══════════════════════════════════════════════════════════════════════════
//  S2DEX2 Handler (GBI2 variant)
// ═══════════════════════════════════════════════════════════════════════════

class S2DEX2Handler : public S2DEXHandler {
public:
    S2DEX2Handler() {
        m_type = MicrocodeType::S2DEX2;
        m_gbiVersion = GBIVersion::GBI_S2DEX2;
        m_name = "S2DEX2";
    }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation
// ═══════════════════════════════════════════════════════════════════════════

inline void S2DEXHandler::Initialize(const MicrocodeInfo& info) {
    m_type = info.type;
    m_name = MicrocodeTypeName(info.type);
    m_gbiVersion = info.gbiVersion;
    m_objMatrixValid = false;
    std::memset(&m_objMatrix, 0, sizeof(m_objMatrix));
    m_objMatrix.A = 0x10000; // Identity scale
    m_objMatrix.D = 0x10000;
    m_objMatrix.BaseScaleX = 0x0400; // 1.0 in u5.10
    m_objMatrix.BaseScaleY = 0x0400;
    ResetStats();
}

inline void S2DEXHandler::OnFrameBegin(DListContext& ctx) {
    (void)ctx;
    m_objMatrixValid = false;
}

inline bool S2DEXHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    // RDP passthrough — but exclude S2DEX texture commands (0xC1-0xC4)
    if (opcode >= 0xC0 && opcode <= 0xCF &&
        opcode != S2DEXCmd::G_OBJ_LOADTXTR &&
        opcode != S2DEXCmd::G_OBJ_LDTX_SPRITE &&
        opcode != S2DEXCmd::G_OBJ_LDTX_RECT &&
        opcode != S2DEXCmd::G_OBJ_LDTX_RECT_R) {
        HandleRDPCommand(ctx, w0, w1);
        return true;
    }
    
    switch (opcode) {
    case 0x00: break; // NOOP
    case S2DEXCmd::G_OBJ_MOVEMEM:    Cmd_OBJ_MOVEMEM(ctx, w0, w1); break;
    case S2DEXCmd::G_BG_1CYC:        Cmd_BG_1CYC(ctx, w0, w1); break;
    case S2DEXCmd::G_BG_COPY:        Cmd_BG_COPY(ctx, w0, w1); break;
    case S2DEXCmd::G_OBJ_SPRITE:     Cmd_OBJ_SPRITE(ctx, w0, w1); break;
    case S2DEXCmd::G_OBJ_RECTANGLE:  Cmd_OBJ_RECTANGLE(ctx, w0, w1); break;
    
    case S2DEXCmd::G_SELECT_DL:      Cmd_SELECT_DL(ctx, w0, w1); break;
    case S2DEXCmd::G_OBJ_RENDERMODE: Cmd_OBJ_RENDERMODE(ctx, w0, w1); break;
    
    case S2DEXCmd::G_OBJ_LOADTXTR:    Cmd_OBJ_LOADTXTR(ctx, w0, w1); break;
    case S2DEXCmd::G_OBJ_LDTX_SPRITE: Cmd_OBJ_LDTX_SPRITE(ctx, w0, w1); break;
    case S2DEXCmd::G_OBJ_LDTX_RECT:   Cmd_OBJ_LDTX_RECT(ctx, w0, w1); break;
    case S2DEXCmd::G_OBJ_LDTX_RECT_R: Cmd_OBJ_LDTX_RECT(ctx, w0, w1); break;
    
    case 0xB8: EndDisplayList(ctx); break; // G_ENDDL
    case 0xDE: { // G_DL
        bool branch = ((w0 >> 16) & 0xFF) == 1;
        DisplayList(ctx, w1, branch);
        break;
    }
    
    case S2DEXCmd::G_OBJ_RECTANGLE_R: Cmd_OBJ_RECTANGLE_R(ctx, w0, w1); break;
    
    default:
        // Try as RDP or GBI1 shared commands
        if (opcode >= 0xE0) {
            HandleRDPCommand(ctx, w0, w1);
        } else if (opcode == 0xB9 || opcode == 0xBA || opcode == 0xBB ||
                   opcode == 0xBC || opcode == 0xBD) {
            // Shared GBI1 state commands
            switch (opcode) {
            case 0xB9: SetOtherModeL(ctx, (w0>>8)&0xFF, (w0&0xFF)+1, w1); break;
            case 0xBA: SetOtherModeH(ctx, (w0>>8)&0xFF, (w0&0xFF)+1, w1); break;
            case 0xBB: SetTextureState(ctx, w0, w1); break;
            case 0xBC: MoveWord(ctx, (w0>>16)&0xFF, w0&0xFFFF, w1); break;
            default: break;
            }
        } else {
            NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                    "S2DEX: Unknown opcode 0x{:02X}", opcode);
        }
        break;
    }
    
    return !ctx.halted;
}

inline bool S2DEX2Handler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    // S2DEX2 uses a mix of GBI2 state commands + S2DEX sprite commands
    // Handle GBI2 state commands
    switch (opcode) {
    case 0xD7: SetTextureState(ctx, w0, w1); return true;
    case 0xD9: SetGeometryMode(ctx, ~(w0 & 0x00FFFFFF), w1); return true;
    case 0xDB: MoveWord(ctx, (w0>>16)&0xFF, w0&0xFFFF, w1); return true;
    case 0xDC: Cmd_OBJ_MOVEMEM(ctx, w0, w1); return true;
    case 0xDE: {
        bool branch = ((w0 >> 16) & 0xFF) == 1;
        DisplayList(ctx, w1, branch);
        return true;
    }
    case 0xDF: EndDisplayList(ctx); return !ctx.halted;
    case 0xE2: {
        uint32_t len = (w0 & 0xFF) + 1;
        uint32_t sft = 32 - ((w0 >> 8) & 0xFF) - len;
        SetOtherModeH(ctx, sft, len, w1);
        return true;
    }
    case 0xE3: {
        uint32_t len = (w0 & 0xFF) + 1;
        uint32_t sft = 32 - ((w0 >> 8) & 0xFF) - len;
        SetOtherModeL(ctx, sft, len, w1);
        return true;
    }
    default:
        break;
    }
    
    // Fall back to base S2DEX handling for sprite commands
    return S2DEXHandler::ProcessCommand(ctx, w0, w1);
}

// ─── BG Sprite Commands ─────────────────────────────────────────────────

inline void S2DEXHandler::Cmd_BG_1CYC(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    ObjBgSprite bg;
    if (ReadBgSprite(ctx, w1, bg)) {
        RenderBgSprite(ctx, bg, false);
    }
}

inline void S2DEXHandler::Cmd_BG_COPY(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    ObjBgSprite bg;
    if (ReadBgSprite(ctx, w1, bg)) {
        RenderBgSprite(ctx, bg, true);
    }
}

// ─── Object Sprite Commands ─────────────────────────────────────────────

inline void S2DEXHandler::Cmd_OBJ_SPRITE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    ObjSprite obj;
    if (ReadObjSprite(ctx, w1, obj)) {
        RenderObjSprite(ctx, obj);
    }
}

inline void S2DEXHandler::Cmd_OBJ_RECTANGLE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Object rectangle — similar to sprite but axis-aligned
    Cmd_OBJ_SPRITE(ctx, w0, w1);
}

inline void S2DEXHandler::Cmd_OBJ_RECTANGLE_R(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Object rectangle with rotation from current matrix
    Cmd_OBJ_SPRITE(ctx, w0, w1);
}

inline void S2DEXHandler::Cmd_OBJ_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    uint32_t addr = ctx.ResolveAddress(w1);
    if (ReadObjMatrix(ctx, addr, m_objMatrix)) {
        m_objMatrixValid = true;
    }
}

inline void S2DEXHandler::Cmd_OBJ_RENDERMODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // OBJ_RENDERMODE sets the RDP cycle type for sprite rendering.
    // w1 contains otherModeH-like flags; bits 52-53 (shifted) select cycle type.
    if (!ctx.state) return;
    (void)w0;
    // The S2DEX render mode maps directly to otherModeH cycle type bits
    // Mask in only the cycle type field (bits 20-21 of otherModeH)
    uint32_t cycleTypeBits = w1 & 0x00300000u;
    ctx.state->otherModeH = (ctx.state->otherModeH & ~0x00300000u) | cycleTypeBits;
}

inline void S2DEXHandler::Cmd_OBJ_LOADTXTR(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    LoadSpriteTexture(ctx, w1);
}

inline void S2DEXHandler::Cmd_OBJ_LDTX_SPRITE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    // Combined load texture + draw sprite
    LoadSpriteTexture(ctx, w1);
    // Sprite data follows texture in RDRAM
    ObjSprite obj;
    if (ReadObjSprite(ctx, w1 + 24, obj)) { // Texture header is 24 bytes
        RenderObjSprite(ctx, obj);
    }
}

inline void S2DEXHandler::Cmd_OBJ_LDTX_RECT(DListContext& ctx, uint32_t w0, uint32_t w1) {
    Cmd_OBJ_LDTX_SPRITE(ctx, w0, w1);
}

inline void S2DEXHandler::Cmd_SELECT_DL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Select DL based on a flag — used for LOD selection
    (void)w0;
    DisplayList(ctx, w1, false);
}

// ─── RDRAM Read Helpers ─────────────────────────────────────────────────

inline bool S2DEXHandler::ReadBgSprite(DListContext& ctx, uint32_t address, ObjBgSprite& bg) {
    address = ctx.ResolveAddress(address);
    if (address + 40 > ctx.rdramSize) return false;
    
    const uint8_t* src = ctx.rdram + address;
    
    bg.imageW     = (src[0] << 8) | src[1];
    bg.imageH     = (src[2] << 8) | src[3];
    bg.frameX     = static_cast<int16_t>((src[4] << 8) | src[5]);
    bg.frameY     = static_cast<int16_t>((src[6] << 8) | src[7]);
    bg.frameW     = (src[8] << 8) | src[9];
    bg.frameH     = (src[10] << 8) | src[11];
    bg.imageAddr  = ((src[12] << 24) | (src[13] << 16) | (src[14] << 8) | src[15]);
    
    uint16_t fmtSiz = (src[16] << 8) | src[17];
    bg.imageFormat = static_cast<ImageFormat>((fmtSiz >> 5) & 0x7);
    bg.imageSize   = static_cast<ImageSize>(fmtSiz & 0x3);
    bg.imagePal    = (src[18] << 8) | src[19];
    bg.imageFlip   = (src[20] & 0x01) != 0;
    
    bg.scaleW     = (src[24] << 8) | src[25];
    bg.scaleH     = (src[26] << 8) | src[27];
    bg.imageX     = (src[28] << 8) | src[29];
    bg.imageY     = (src[30] << 8) | src[31];
    bg.imageYorig = static_cast<int32_t>((src[32] << 24) | (src[33] << 16) | (src[34] << 8) | src[35]);
    
    return true;
}

inline bool S2DEXHandler::ReadObjSprite(DListContext& ctx, uint32_t address, ObjSprite& obj) {
    address = ctx.ResolveAddress(address);
    if (address + 24 > ctx.rdramSize) return false;
    
    const uint8_t* src = ctx.rdram + address;
    
    obj.objX        = static_cast<int16_t>((src[0] << 8) | src[1]);
    obj.scaleW      = (src[2] << 8) | src[3];
    obj.imageW      = (src[4] << 8) | src[5];
    obj.imagePal    = src[6];
    obj.imageFlags  = src[7];
    
    obj.objY        = static_cast<int16_t>((src[8] << 8) | src[9]);
    obj.scaleH      = (src[10] << 8) | src[11];
    obj.imageH      = (src[12] << 8) | src[13];
    obj.imageStride = (src[14] << 8) | src[15];
    
    obj.imageAddr   = ((src[16] << 24) | (src[17] << 16) | (src[18] << 8) | src[19]);
    obj.imageFmt    = (src[20] << 8) | src[21];
    obj.imageSiz    = (src[22] << 8) | src[23];
    
    return true;
}

inline bool S2DEXHandler::ReadObjMatrix(DListContext& ctx, uint32_t address, ObjMatrix& mtx) {
    if (address + 24 > ctx.rdramSize) return false;
    
    const uint8_t* src = ctx.rdram + address;
    
    mtx.A = static_cast<int32_t>((src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3]);
    mtx.B = static_cast<int32_t>((src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7]);
    mtx.C = static_cast<int32_t>((src[8] << 24) | (src[9] << 16) | (src[10] << 8) | src[11]);
    mtx.D = static_cast<int32_t>((src[12] << 24) | (src[13] << 16) | (src[14] << 8) | src[15]);
    mtx.X = static_cast<int16_t>((src[16] << 8) | src[17]);
    mtx.Y = static_cast<int16_t>((src[18] << 8) | src[19]);
    mtx.BaseScaleX = (src[20] << 8) | src[21];
    mtx.BaseScaleY = (src[22] << 8) | src[23];
    
    return true;
}

// ─── Rendering Helpers ──────────────────────────────────────────────────

inline void S2DEXHandler::RenderBgSprite(DListContext& ctx, const ObjBgSprite& bg, bool copyMode) {
    if (!ctx.rdpProcessor) return;
    
    // Background sprites are rendered as large texture rectangles
    // They tile/scroll across the screen
    float frameX = static_cast<float>(bg.frameX) / 4.0f;
    float frameY = static_cast<float>(bg.frameY) / 4.0f;
    float frameW = static_cast<float>(bg.frameW) / 4.0f;
    float frameH = static_cast<float>(bg.frameH) / 4.0f;
    
    float scaleW = static_cast<float>(bg.scaleW) / 1024.0f;
    float scaleH = static_cast<float>(bg.scaleH) / 1024.0f;
    
    // Set up RDP for background rendering
    // In copy mode, uses cycle type COPY for bit-exact reproduction
    // In 1-cycle mode, allows filtering/blending
    
    // Point RDP state at the background image so TextureCache picks it up
    if (ctx.state) {
        uint32_t resolvedAddr = ctx.ResolveAddress(bg.imageAddr);
        ctx.state->textureImage.address = resolvedAddr & 0x00FFFFFF;
        ctx.state->textureImage.format  = static_cast<ImageFormat>(static_cast<uint8_t>(bg.imageFormat));
        ctx.state->textureImage.size    = static_cast<ImageSize>(static_cast<uint8_t>(bg.imageSize));
        ctx.state->textureImage.width   = static_cast<uint16_t>(bg.imageW >> 2); // imageW is 10.2
    }

    // Texture UV start (scrolled into the image)
    float ulS = static_cast<float>(bg.imageX) / 32.0f;
    float ulT = static_cast<float>(bg.imageY) / 32.0f;
    // dsDx: texels advanced per screen pixel. scaleW=1 -> 1 texel/pixel; scaleW=2 -> stretch 2x -> 0.5 texels/pixel
    float dsdx = (scaleW > 0.0f) ? (1.0f / scaleW) : 1.0f;
    float dtdy = (scaleH > 0.0f) ? (1.0f / scaleH) : 1.0f;
    if (copyMode) {
        dsdx *= 4.0f; // Copy mode S increment is in quarter-pixel units
    }

    DecodedRectangle rect{};
    rect.x0 = frameX;  rect.y0 = frameY;
    rect.x1 = frameX + frameW;  rect.y1 = frameY + frameH;
    rect.s = ulS;  rect.t = ulT;
    rect.dsDx = dsdx;  rect.dtDy = dtdy;
    rect.tileIndex = 0;
    rect.flipped = bg.imageFlip;
    ctx.rdpProcessor->DrawTextureRect(rect);
}

inline void S2DEXHandler::RenderObjSprite(DListContext& ctx, const ObjSprite& obj) {
    if (!ctx.rdpProcessor) return;
    
    float x = static_cast<float>(obj.objX) / 4.0f;
    float y = static_cast<float>(obj.objY) / 4.0f;
    float scaleW = static_cast<float>(obj.scaleW) / 1024.0f;
    float scaleH = static_cast<float>(obj.scaleH) / 1024.0f;
    float w = static_cast<float>(obj.imageW) / 32.0f;
    float h = static_cast<float>(obj.imageH) / 32.0f;
    
    // Apply 2D matrix if valid
    if (m_objMatrixValid) {
        float mtxA = static_cast<float>(m_objMatrix.A) / 65536.0f;
        float mtxB = static_cast<float>(m_objMatrix.B) / 65536.0f;
        float mtxC = static_cast<float>(m_objMatrix.C) / 65536.0f;
        float mtxD = static_cast<float>(m_objMatrix.D) / 65536.0f;
        float mtxX = static_cast<float>(m_objMatrix.X) / 4.0f;
        float mtxY = static_cast<float>(m_objMatrix.Y) / 4.0f;
        
        float tx = x * mtxA + y * mtxB + mtxX;
        float ty = x * mtxC + y * mtxD + mtxY;
        x = tx; y = ty;
        
        float mtxScaleX = static_cast<float>(m_objMatrix.BaseScaleX) / 1024.0f;
        float mtxScaleY = static_cast<float>(m_objMatrix.BaseScaleY) / 1024.0f;
        scaleW *= mtxScaleX;
        scaleH *= mtxScaleY;
    }
    
    float screenW = w * scaleW;
    float screenH = h * scaleH;

    // Point RDP state at the sprite image so TextureCache picks it up
    if (ctx.state) {
        uint32_t resolvedAddr = ctx.ResolveAddress(obj.imageAddr);
        ctx.state->textureImage.address = resolvedAddr & 0x00FFFFFF;
        uint8_t fmt = (obj.imageFmt >> 5) & 0x07;
        uint8_t siz = (obj.imageFmt >> 3) & 0x03;
        ctx.state->textureImage.format  = static_cast<ImageFormat>(fmt);
        ctx.state->textureImage.size    = static_cast<ImageSize>(siz);
        ctx.state->textureImage.width   = static_cast<uint16_t>(obj.imageW >> 5); // imageW is 10.5
    }

    // dsDx/dtDy: texels per screen pixel (reciprocal of scale). scaleW=1.0 -> 1 texel/pixel
    float dsDx = (scaleW > 0.0f) ? (1.0f / scaleW) : 1.0f;
    float dtDy = (scaleH > 0.0f) ? (1.0f / scaleH) : 1.0f;

    DecodedRectangle rect{};
    rect.x0 = x;  rect.y0 = y;
    rect.x1 = x + screenW;  rect.y1 = y + screenH;
    rect.s = 0.0f;  rect.t = 0.0f;
    rect.dsDx = dsDx;  rect.dtDy = dtDy;
    rect.tileIndex = 0;
    rect.flipped = (obj.imageFlags & 0x01) != 0;
    ctx.rdpProcessor->DrawTextureRect(rect);
}

inline void S2DEXHandler::LoadSpriteTexture(DListContext& ctx, uint32_t address) {
    address = ctx.ResolveAddress(address);
    if (address + 24 > ctx.rdramSize) return;
    
    // Read texture loading parameters from RDRAM
    // S2DEX texture loading structure (big-endian):
    //   0x00: type (u16) — 0 = LoadBlock, 1 = LoadTile
    //   0x02: padding (u16)
    //   0x04: image addr (u32) — RDRAM source (segmented)
    //   0x08: tmem (u16) — TMEM destination (word address)
    //   0x0A: tsize (u16) — format | size
    //   0x0C: twidth (u16) — texture width for line calculation
    //   0x0E: theight (u16) — texture height
    const uint8_t* src = ctx.rdram + address;
    
    uint16_t type    = (src[0] << 8) | src[1];
    uint32_t image   = ((src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7]);
    uint16_t tmem    = (src[8] << 8) | src[9];
    uint16_t tsize   = (src[10] << 8) | src[11];
    uint16_t twidth  = (src[12] << 8) | src[13];
    uint16_t theight = (src[14] << 8) | src[15];
    
    if (!ctx.state || !ctx.rdpProcessor) return;
    
    // Parse format/size from tsize field
    uint8_t fmt  = (tsize >> 5) & 0x07; // Image format (0=RGBA, 1=YUV, 2=CI, 3=IA, 4=I)
    uint8_t siz  = (tsize >> 3) & 0x03; // Size (0=4b, 1=8b, 2=16b, 3=32b)
    
    // Set the source texture image in RDP state
    uint32_t resolvedImage = ctx.ResolveAddress(image);
    ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
    ctx.state->textureImage.width   = twidth;
    ctx.state->textureImage.format  = static_cast<ImageFormat>(fmt);
    ctx.state->textureImage.size    = static_cast<ImageSize>(siz);
    
    // Configure tile 0 for this load
    auto& tile = ctx.state->tiles[0];
    tile.format   = fmt;
    tile.size     = siz;
    tile.tmemAddr = tmem;
    
    // Calculate line stride: bytes per row / 8 (64-bit words)
    static const int bpp[] = { 4, 8, 16, 32 };
    int bitsPerPixel = bpp[siz & 3];
    uint16_t lineWords = static_cast<uint16_t>((twidth * bitsPerPixel + 63) / 64);
    tile.line = lineWords;
    
    if (type == 0) {
        // LoadBlock: load all texels in one DMA transfer
        int totalTexels = twidth * theight;
        // dxt = (1 << 11) / line — inverse of line for LoadBlock stride
        int dxt = (lineWords > 0) ? ((1 << 11) / lineWords) : 0;
        ctx.rdpProcessor->ExecuteLoadBlock(0, 0, 0, totalTexels - 1, dxt);
    } else {
        // LoadTile: load a rectangular region
        tile.sl = 0;
        tile.tl = 0;
        tile.sh = static_cast<int32_t>(twidth - 1) << 2;
        tile.th = static_cast<int32_t>(theight - 1) << 2;
        ctx.rdpProcessor->ExecuteLoadTile(0, 0.0f, 0.0f,
            static_cast<float>(twidth - 1), static_cast<float>(theight - 1));
    }
    
    // Set tile size for rendering
    tile.sl = 0;
    tile.tl = 0;
    tile.sh = static_cast<int32_t>(twidth - 1) << 2;
    tile.th = static_cast<int32_t>(theight - 1) << 2;
}

} // namespace Nova64::Video
