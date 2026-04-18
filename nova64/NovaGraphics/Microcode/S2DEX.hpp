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
#include <algorithm>
#include <cmath>

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

/// Object sprite definition (uObjSprite — matches N64 SDK layout)
struct ObjSprite {
    int16_t     objX;           // Screen X (s10.2)
    int16_t     objY;           // Screen Y (s10.2)
    uint16_t    scaleW;         // H scale (u5.10)
    uint16_t    scaleH;         // V scale (u5.10)
    uint16_t    imageW;         // Texture width (u10.5)
    uint16_t    imageH;         // Texture height (u10.5)
    
    uint16_t    imageAdrs;      // TMEM word address (64-bit words)
    uint16_t    imageStride;    // Stride in 64-bit words
    uint8_t     imageFlags;     // Flip flags
    uint8_t     imagePal;       // Palette index (0-7)
    uint8_t     imageSiz;       // Texel size (G_IM_SIZ_*)
    uint8_t     imageFmt;       // Texture format (G_IM_FMT_*)
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
    // S2DEX1 commands (GBI1 variant — matches GLideN64 S2DEX.cpp)
    constexpr uint8_t G_BG_1CYC         = 0x01;
    constexpr uint8_t G_BG_COPY         = 0x02;
    constexpr uint8_t G_OBJ_RECTANGLE   = 0x03;
    constexpr uint8_t G_OBJ_SPRITE      = 0x04;
    constexpr uint8_t G_OBJ_MOVEMEM     = 0x05;
    constexpr uint8_t G_SELECT_DL       = 0xB0;
    constexpr uint8_t G_OBJ_RENDERMODE  = 0xB1;
    constexpr uint8_t G_OBJ_RECTANGLE_R = 0xB2;
    constexpr uint8_t G_OBJ_LOADTXTR    = 0xC1;
    constexpr uint8_t G_OBJ_LDTX_SPRITE = 0xC2;
    constexpr uint8_t G_OBJ_LDTX_RECT   = 0xC3;
    constexpr uint8_t G_OBJ_LDTX_RECT_R = 0xC4;
    
    // S2DEX2 commands (GBI2 variant — matches GLideN64 S2DEX2.cpp)
    namespace V2 {
        constexpr uint8_t G_OBJ_RECTANGLE   = 0x01;
        constexpr uint8_t G_OBJ_SPRITE      = 0x02;
        constexpr uint8_t G_SELECT_DL       = 0x04;
        constexpr uint8_t G_OBJ_LOADTXTR    = 0x05;
        constexpr uint8_t G_OBJ_LDTX_SPRITE = 0x06;
        constexpr uint8_t G_OBJ_LDTX_RECT   = 0x07;
        constexpr uint8_t G_OBJ_LDTX_RECT_R = 0x08;
        constexpr uint8_t G_BG_1CYC         = 0x09;
        constexpr uint8_t G_BG_COPY         = 0x0A;
        constexpr uint8_t G_OBJ_RENDERMODE  = 0x0B;
        constexpr uint8_t G_OBJ_RECTANGLE_R = 0xDA;
        constexpr uint8_t G_OBJ_MOVEMEM     = 0xDC;
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

private:
    uint32_t m_rdpHalf1 = 0;
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
            case 0xB9: SetOtherModeL(ctx, (w0>>8)&0xFF, (w0&0xFF), w1); break;
            case 0xBA: SetOtherModeH(ctx, (w0>>8)&0xFF, (w0&0xFF), w1); break;
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
    
    // S2DEX2 uses GBI2 state commands in the high range + completely different
    // low-range sprite opcode assignments from S2DEX1. Handle everything here
    // without falling through to S2DEXHandler (which uses S2DEX1 opcodes).
    switch (opcode) {
    // ── S2DEX2 sprite commands (low range: 0x00–0x0B) ──
    case 0x00: break; // NOOP
    case S2DEXCmd::V2::G_OBJ_RECTANGLE:   Cmd_OBJ_RECTANGLE(ctx, w0, w1); break;   // 0x01
    case S2DEXCmd::V2::G_OBJ_SPRITE:      Cmd_OBJ_SPRITE(ctx, w0, w1); break;      // 0x02
    case S2DEXCmd::V2::G_SELECT_DL:       Cmd_SELECT_DL(ctx, w0, w1); break;       // 0x04
    case S2DEXCmd::V2::G_OBJ_LOADTXTR:    Cmd_OBJ_LOADTXTR(ctx, w0, w1); break;    // 0x05
    case S2DEXCmd::V2::G_OBJ_LDTX_SPRITE: Cmd_OBJ_LDTX_SPRITE(ctx, w0, w1); break; // 0x06
    case S2DEXCmd::V2::G_OBJ_LDTX_RECT:   Cmd_OBJ_LDTX_RECT(ctx, w0, w1); break;   // 0x07
    case S2DEXCmd::V2::G_OBJ_LDTX_RECT_R: Cmd_OBJ_LDTX_RECT(ctx, w0, w1); break;   // 0x08
    case S2DEXCmd::V2::G_BG_1CYC:         Cmd_BG_1CYC(ctx, w0, w1); break;         // 0x09
    case S2DEXCmd::V2::G_BG_COPY:         Cmd_BG_COPY(ctx, w0, w1); break;         // 0x0A
    case S2DEXCmd::V2::G_OBJ_RENDERMODE:  Cmd_OBJ_RENDERMODE(ctx, w0, w1); break;  // 0x0B
    
    // ── GBI2 state commands (high range) ──
    case 0xD7: SetTextureState(ctx, w0, w1); break;
    case 0xD9: SetGeometryMode(ctx, ~(w0 & 0x00FFFFFF), w1); break;
    case S2DEXCmd::V2::G_OBJ_RECTANGLE_R: Cmd_OBJ_RECTANGLE_R(ctx, w0, w1); break; // 0xDA
    case 0xDB: MoveWord(ctx, (w0>>16)&0xFF, w0&0xFFFF, w1); break;
    case S2DEXCmd::V2::G_OBJ_MOVEMEM:     Cmd_OBJ_MOVEMEM(ctx, w0, w1); break;     // 0xDC
    case 0xDD: LoadUcode(ctx, w1, m_rdpHalf1); return !ctx.halted;
    case 0xDE: {
        bool branch = ((w0 >> 16) & 0xFF) == 1;
        DisplayList(ctx, w1, branch);
        break;
    }
    case 0xDF: EndDisplayList(ctx); return !ctx.halted;
    case 0xE1: m_rdpHalf1 = w1; break;
    // F3DEX2/S2DEX2 swaps the opcodes: 0xE2=SetOtherMode_L, 0xE3=SetOtherMode_H
    case 0xE2: {
        uint32_t len = (w0 & 0xFF) + 1;
        uint32_t sft = 32 - ((w0 >> 8) & 0xFF) - len;
        SetOtherModeL(ctx, sft, len, w1);
        break;
    }
    case 0xE3: {
        uint32_t len = (w0 & 0xFF) + 1;
        uint32_t sft = 32 - ((w0 >> 8) & 0xFF) - len;
        SetOtherModeH(ctx, sft, len, w1);
        break;
    }
    
    default:
        // RDP passthrough for 0xC0+ (no S2DEX2 texture commands use 0xC1-0xC4)
        if (opcode >= 0xE0 || (opcode >= 0xC0 && opcode <= 0xCF)) {
            HandleRDPCommand(ctx, w0, w1);
        } else {
            NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                    "S2DEX2: Unknown opcode 0x{:02X}", opcode);
        }
        break;
    }
    
    return !ctx.halted;
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
    uint16_t subType = w0 & 0xFFFF;
    
    switch (subType) {
    case 0: { // S2DEX_MV_MATRIX — full 24-byte ObjMtx
        uint32_t addr = ctx.ResolveAddress(w1);
        if (ReadObjMatrix(ctx, addr, m_objMatrix)) {
            m_objMatrixValid = true;
        }
        break;
    }
    case 2: { // S2DEX_MV_SUBMUTRIX — 8-byte sub-matrix (X, Y, BaseScaleX, BaseScaleY only)
        uint32_t addr = ctx.ResolveAddress(w1);
        if (addr + 8 > ctx.rdramSize) break;
        // N64 uObjSubMtx layout (8 bytes):
        //   0x00: X (s16), Y (s16)   [first BE halfword = X, second = Y]
        //   0x04: BaseScaleX (u16), BaseScaleY (u16)
        m_objMatrix.X = static_cast<int16_t>(ctx.ReadHalf(addr + 0x00));
        m_objMatrix.Y = static_cast<int16_t>(ctx.ReadHalf(addr + 0x02));
        m_objMatrix.BaseScaleX = ctx.ReadHalf(addr + 0x04);
        m_objMatrix.BaseScaleY = ctx.ReadHalf(addr + 0x06);
        break;
    }
    case 8: // S2DEX_MV_VIEWPORT
        MoveMemGBI1(ctx, 0x80 /*G_MV_VIEWPORT*/, 16, ctx.ResolveAddress(w1));
        break;
    default:
        break;
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
    
    // N64 uObjScaleBg_t struct layout (40 bytes, big-endian):
    //   0x00: imageX (u16), imageW (u16)
    //   0x04: frameX (s16), frameW (u16)
    //   0x08: imageY (u16), imageH (u16)
    //   0x0C: frameY (s16), frameH (u16)
    //   0x10: imagePtr (u32)
    //   0x14: imageLoad (u16), imageFmt (u8), imageSiz (u8)
    //   0x18: imagePal (u16), imageFlip (u16)
    //   0x1C: scaleW (u16), scaleH (u16)
    //   0x20: imageYorig (s32)
    bg.imageX     = ctx.ReadHalf(address + 0x00);
    bg.imageW     = ctx.ReadHalf(address + 0x02);
    bg.frameX     = static_cast<int16_t>(ctx.ReadHalf(address + 0x04));
    bg.frameW     = ctx.ReadHalf(address + 0x06);
    bg.imageY     = ctx.ReadHalf(address + 0x08);
    bg.imageH     = ctx.ReadHalf(address + 0x0A);
    bg.frameY     = static_cast<int16_t>(ctx.ReadHalf(address + 0x0C));
    bg.frameH     = ctx.ReadHalf(address + 0x0E);
    bg.imageAddr  = ctx.ReadWord(address + 0x10);
    bg.imageFormat= static_cast<ImageFormat>(ctx.ReadByte(address + 0x16));
    bg.imageSize  = static_cast<ImageSize>(ctx.ReadByte(address + 0x17));
    bg.imagePal   = ctx.ReadHalf(address + 0x18);
    bg.imageFlip  = (ctx.ReadHalf(address + 0x1A) & 0x01) != 0;
    bg.scaleW     = ctx.ReadHalf(address + 0x1C);
    bg.scaleH     = ctx.ReadHalf(address + 0x1E);
    bg.imageYorig = static_cast<int32_t>(ctx.ReadWord(address + 0x20));
    
    return true;
}

inline bool S2DEXHandler::ReadObjSprite(DListContext& ctx, uint32_t address, ObjSprite& obj) {
    address = ctx.ResolveAddress(address);
    if (address + 24 > ctx.rdramSize) return false;
    
    // N64 uObjSprite_t layout (24 bytes, big-endian):
    //   0x00: objX (s16), scaleW (u16)
    //   0x04: imageW (u16), paddingX (u16)
    //   0x08: objY (s16), scaleH (u16)
    //   0x0C: imageH (u16), paddingY (u16)
    //   0x10: imageStride (u16), imageAdrs (u16)
    //   0x14: imageFmt (u8), imageSiz (u8), imagePal (u8), imageFlags (u8)
    obj.objX        = static_cast<int16_t>(ctx.ReadHalf(address + 0x00));
    obj.scaleW      = ctx.ReadHalf(address + 0x02);
    obj.imageW      = ctx.ReadHalf(address + 0x04);
    // skip paddingX at 0x06
    obj.objY        = static_cast<int16_t>(ctx.ReadHalf(address + 0x08));
    obj.scaleH      = ctx.ReadHalf(address + 0x0A);
    obj.imageH      = ctx.ReadHalf(address + 0x0C);
    // skip paddingY at 0x0E
    obj.imageStride = ctx.ReadHalf(address + 0x10);
    obj.imageAdrs   = ctx.ReadHalf(address + 0x12);
    obj.imageFmt    = ctx.ReadByte(address + 0x14);
    obj.imageSiz    = ctx.ReadByte(address + 0x15);
    obj.imagePal    = ctx.ReadByte(address + 0x16);
    obj.imageFlags  = ctx.ReadByte(address + 0x17);
    
    return true;
}

inline bool S2DEXHandler::ReadObjMatrix(DListContext& ctx, uint32_t address, ObjMatrix& mtx) {
    if (address + 24 > ctx.rdramSize) return false;
    
    // N64 uObjMtx layout (24 bytes):
    //   0x00: A (s15.16), 0x04: B, 0x08: C, 0x0C: D
    //   0x10: X (s10.2), 0x12: Y (s10.2)
    //   0x14: BaseScaleX (u5.10), 0x16: BaseScaleY (u5.10)
    mtx.A = static_cast<int32_t>(ctx.ReadWord(address + 0x00));
    mtx.B = static_cast<int32_t>(ctx.ReadWord(address + 0x04));
    mtx.C = static_cast<int32_t>(ctx.ReadWord(address + 0x08));
    mtx.D = static_cast<int32_t>(ctx.ReadWord(address + 0x0C));
    mtx.X = static_cast<int16_t>(ctx.ReadHalf(address + 0x10));
    mtx.Y = static_cast<int16_t>(ctx.ReadHalf(address + 0x12));
    mtx.BaseScaleX = ctx.ReadHalf(address + 0x14);
    mtx.BaseScaleY = ctx.ReadHalf(address + 0x16);
    
    return true;
}

// ─── Rendering Helpers ──────────────────────────────────────────────────

inline void S2DEXHandler::RenderBgSprite(DListContext& ctx, const ObjBgSprite& bg, bool copyMode) {
    if (!ctx.rdpProcessor || !ctx.state) return;
    
    float frameX = static_cast<float>(bg.frameX) / 4.0f;
    float frameY = static_cast<float>(bg.frameY) / 4.0f;
    float frameW = static_cast<float>(bg.frameW) / 4.0f;
    float frameH = static_cast<float>(bg.frameH) / 4.0f;
    
    float imageX = static_cast<float>(bg.imageX) / 32.0f;
    float imageY = static_cast<float>(bg.imageY) / 32.0f;
    
    // imageW/imageH are u10.2; round down to even pixel count (GLideN64 convention)
    int imageWi = static_cast<int>((bg.imageW >> 2) & ~1u);
    int imageHi = static_cast<int>((bg.imageH >> 2) & ~1u);
    float imageW = static_cast<float>(imageWi);
    
    float scaleW, scaleH;
    if (copyMode) {
        scaleW = 1.0f;
        scaleH = 1.0f;
    } else {
        scaleW = static_cast<float>(bg.scaleW) / 1024.0f;
        scaleH = static_cast<float>(bg.scaleH) / 1024.0f;
    }
    if (scaleW <= 0.0f) scaleW = 1.0f;
    if (scaleH <= 0.0f) scaleH = 1.0f;
    
    // Force cycle type
    if (copyMode)
        ctx.state->otherModeH = (ctx.state->otherModeH & ~0x00300000u) | (2u << 20); // G_CYC_COPY
    else
        ctx.state->otherModeH = (ctx.state->otherModeH & ~0x00300000u) | (0u << 20); // G_CYC_1CYCLE
    
    uint32_t resolvedAddr = ctx.ResolveAddress(bg.imageAddr);
    resolvedAddr &= 0x00FFFFFF;
    
    // Bits per pixel for this format
    static const int bpp[] = { 4, 8, 16, 32 };
    int bitsPerPixel = bpp[static_cast<uint8_t>(bg.imageSize) & 3];
    
    // TMEM line stride: 64-bit words per row
    int lineWords = (imageWi * bitsPerPixel + 63) / 64;
    int bytesPerRow = lineWords * 8;
    
    // How many rows fit in TMEM? (4096 bytes total)
    int rowsPerStrip = (bytesPerRow > 0) ? (4096 / bytesPerRow) : imageHi;
    if (rowsPerStrip <= 0) rowsPerStrip = 1;
    if (rowsPerStrip > imageHi) rowsPerStrip = imageHi;
    
    // ── Strip loop ──
    // Iterate over horizontal strips of the BG image. Each strip loads
    // rowsPerStrip rows into TMEM via LoadBlock, then draws a TextureRect.
    float screenY = frameY;
    float texRowY = imageY;
    int remainingRows = imageHi;
    
    // RDRAM bytes per row (source stride, NOT TMEM line stride)
    int rdramBytesPerRow = (imageWi * bitsPerPixel + 7) / 8;
    
    while (remainingRows > 0 && screenY < frameY + frameH) {
        int stripRows = std::min(rowsPerStrip, remainingRows);
        int stripBytes = bytesPerRow * stripRows;
        
        // Source RDRAM address for this strip
        int rowOffset = imageHi - remainingRows;
        uint32_t stripAddr = resolvedAddr + static_cast<uint32_t>(rowOffset) * rdramBytesPerRow;
        
        // Point textureImage at this strip's RDRAM source
        ctx.state->textureImage.address = stripAddr;
        ctx.state->textureImage.format  = bg.imageFormat;
        ctx.state->textureImage.size    = bg.imageSize;
        ctx.state->textureImage.width   = static_cast<uint16_t>(imageWi);
        
        // Configure load tile (tile 7) for LoadBlock
        auto& loadTile = ctx.state->tiles[7];
        loadTile.format   = static_cast<uint8_t>(bg.imageFormat);
        loadTile.size     = static_cast<uint8_t>(bg.imageSize);
        loadTile.tmemAddr = 0;
        loadTile.line     = 0;
        loadTile.palette  = 0;
        loadTile.clampS = 0; loadTile.clampT = 0;
        loadTile.maskS = 0; loadTile.maskT = 0;
        loadTile.shiftS = 0; loadTile.shiftT = 0;
        
        // LoadBlock: load strip texels into TMEM starting at address 0
        // texels parameter is (count-1) for LoadBlock; bpp-based conversion to 16b words
        int loadTexels = (stripBytes / 2) - 1; // Convert bytes to 16-bit RGBA words - 1
        if (loadTexels < 0) loadTexels = 0;
        ctx.rdpProcessor->ExecuteLoadBlock(7, 0, 0, loadTexels, 0);
        
        // Configure render tile (tile 0) with this strip's dimensions
        auto& renderTile = ctx.state->tiles[0];
        renderTile.format   = static_cast<uint8_t>(bg.imageFormat);
        renderTile.size     = static_cast<uint8_t>(bg.imageSize);
        renderTile.tmemAddr = 0;
        renderTile.palette  = static_cast<uint8_t>(bg.imagePal & 0xF);
        renderTile.line     = static_cast<uint16_t>(lineWords);
        renderTile.sl = 0; renderTile.tl = 0;
        renderTile.sh = static_cast<int32_t>(imageWi - 1) * 4;
        renderTile.th = static_cast<int32_t>(stripRows - 1) * 4;
        renderTile.clampS = 0; renderTile.clampT = 0;
        renderTile.maskS = 0; renderTile.maskT = 0;
        renderTile.shiftS = 0; renderTile.shiftT = 0;
        
        // Screen extents for this strip
        float stripScreenH = static_cast<float>(stripRows) / scaleH;
        float stripUlY = screenY;
        float stripLrY = std::min(screenY + stripScreenH, frameY + frameH);
        float ulx = frameX;
        float lrx = ulx + std::min(frameW, imageW / scaleW);
        
        // Texture coordinates for this strip
        float ulS = imageX;
        float ulT = 0.0f; // Each strip starts at T=0 in its own TMEM load
        float lrS = ulS + (lrx - ulx) * scaleW;
        float lrT = (stripLrY - stripUlY) * scaleH;
        
        // Horizontal flip
        float drawUlx = ulx, drawLrx = lrx;
        if (bg.imageFlip) {
            std::swap(drawUlx, drawLrx);
        }
        
        float spanX = drawLrx - drawUlx;
        float spanY = stripLrY - stripUlY;
        float dsdx = (std::abs(spanX) > 0.001f) ? (lrS - ulS) / spanX : 1.0f;
        float dtdy = (std::abs(spanY) > 0.001f) ? lrT / spanY : 1.0f;
        if (copyMode) {
            dsdx *= 4.0f;
        }
        
        DecodedRectangle rect{};
        rect.x0 = std::min(drawUlx, drawLrx);
        rect.y0 = stripUlY;
        rect.x1 = std::max(drawUlx, drawLrx);
        rect.y1 = stripLrY;
        rect.s  = bg.imageFlip ? lrS : ulS;
        rect.t  = ulT;
        rect.dsDx = bg.imageFlip ? -std::abs(dsdx) : dsdx;
        rect.dtDy = dtdy;
        rect.tileIndex = 0;
        rect.flipped = false;
        ctx.rdpProcessor->DrawTextureRect(rect);
        
        screenY += stripScreenH;
        texRowY += static_cast<float>(stripRows);
        remainingRows -= stripRows;
    }
}

inline void S2DEXHandler::RenderObjSprite(DListContext& ctx, const ObjSprite& obj) {
    if (!ctx.rdpProcessor || !ctx.state) return;
    
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

    // Set up tile 0 with sprite's own format info (matches GLideN64 gSPSetSpriteTile)
    {
        uint32_t texW = std::max<uint32_t>(obj.imageW >> 5, 1u);
        uint32_t texH = std::max<uint32_t>(obj.imageH >> 5, 1u);
        auto& tile = ctx.state->tiles[0];
        tile.format   = obj.imageFmt;
        tile.size     = obj.imageSiz;
        tile.line     = obj.imageStride;
        tile.tmemAddr = obj.imageAdrs;
        tile.palette  = obj.imagePal;
        tile.clampS   = 2; tile.clampT = 2;  // clamp mode
        tile.maskS    = 0; tile.maskT = 0;
        tile.shiftS   = 0; tile.shiftT = 0;
        tile.sl = 0; tile.tl = 0;
        tile.sh = static_cast<int32_t>(texW - 1) << 2;
        tile.th = static_cast<int32_t>(texH - 1) << 2;
        // textureImage width for the tile (used by texture cache)
        ctx.state->textureImage.width = static_cast<uint16_t>(texW);
        ctx.state->textureImage.format = static_cast<ImageFormat>(obj.imageFmt);
        ctx.state->textureImage.size   = static_cast<ImageSize>(obj.imageSiz);
    }

    // dsDx/dtDy: texels per screen pixel (reciprocal of scale)
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
    if (!ctx.state || !ctx.rdpProcessor) return;
    
    // N64 uObjTxtr (union of Block/Tile/TLUT, each 24 bytes):
    //   0x00: type (u32) — identifies which variant
    //   0x04: image (u32) — RDRAM source address (segmented)
    //   0x08-0x17: type-specific fields
    // GLideN64 constants: TXTRBLOCK=0x00001033, TXTRTILE=0x00fc1034, TLUT=0x00000030
    // Match only the lower 16 bits for robustness (upper bits vary between games)
    uint32_t type  = ctx.ReadWord(address + 0x00);
    uint32_t image = ctx.ReadWord(address + 0x04);
    
    // Resolve the texture source address
    uint32_t resolvedImage = ctx.ResolveAddress(image);
    
    // Load tile index for S2DEX texture loads (tile 7 = G_TX_LOADTILE)
    constexpr int loadTile = 7;
    
    switch (type & 0xFFFF) {
    case 0x1033: { // G_OBJLT_TXTRBLOCK
        uint16_t tmem  = ctx.ReadHalf(address + 0x08);
        uint16_t tsize = ctx.ReadHalf(address + 0x0A);
        uint16_t tline = ctx.ReadHalf(address + 0x0C);
        
        // SetTextureImage: RGBA 16b, width=1 (N64 SDK: gDPSetTextureImage(RGBA, SIZ_16b, 1, image))
        ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
        ctx.state->textureImage.format  = ImageFormat::RGBA;
        ctx.state->textureImage.size    = ImageSize::Bpp16;
        ctx.state->textureImage.width   = 1;
        
        // SetTile for load tile
        auto& lt = ctx.state->tiles[loadTile];
        lt.format   = 0; // RGBA
        lt.size     = 2; // 16b
        lt.line     = 0;
        lt.tmemAddr = tmem;
        lt.palette  = 0;
        lt.clampS = 0; lt.clampT = 0;
        lt.maskS = 0; lt.maskT = 0;
        lt.shiftS = 0; lt.shiftT = 0;
        
        // LoadBlock: sl=0, tl=0, sh=tsize (texels-1), dxt=tline
        // N64 SDK: gDPLoadBlock(G_TX_LOADTILE, 0, 0, tsize, tline)
        ctx.rdpProcessor->ExecuteLoadBlock(loadTile, 0, 0, tsize, tline);
        break;
    }
    case 0x1034: { // G_OBJLT_TXTRTILE
        uint16_t tmem    = ctx.ReadHalf(address + 0x08);
        uint16_t twidth  = ctx.ReadHalf(address + 0x0A);
        uint16_t theight = ctx.ReadHalf(address + 0x0C);
        
        // SetTextureImage: RGBA 16b, width = twidth+1
        ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
        ctx.state->textureImage.format  = ImageFormat::RGBA;
        ctx.state->textureImage.size    = ImageSize::Bpp16;
        ctx.state->textureImage.width   = twidth + 1;
        
        // SetTile for load tile
        auto& lt = ctx.state->tiles[loadTile];
        lt.format   = 0; // RGBA
        lt.size     = 2; // 16b
        lt.line     = (twidth + 1) >> 2;
        lt.tmemAddr = tmem;
        lt.palette  = 0;
        lt.clampS = 0; lt.clampT = 0;
        lt.maskS = 0; lt.maskT = 0;
        lt.shiftS = 0; lt.shiftT = 0;
        
        // LoadTile: 0,0 to twidth, theight (texel coordinates)
        // N64 SDK: gDPLoadTile(G_TX_LOADTILE, 0, 0, twidth, theight) — u10.2 internally
        // ExecuteLoadTile expects float texel coords (decoded from u10.2)
        ctx.rdpProcessor->ExecuteLoadTile(loadTile, 0.0f, 0.0f,
            static_cast<float>(twidth), static_cast<float>(theight));
        break;
    }
    case 0x0030: { // G_OBJLT_TLUT (GLideN64 uses 0x00000030)
        uint16_t phead = ctx.ReadHalf(address + 0x08);
        uint16_t pnum  = ctx.ReadHalf(address + 0x0A);
        
        // SetTextureImage: RGBA 16b, width = 1
        ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
        ctx.state->textureImage.format  = ImageFormat::RGBA;
        ctx.state->textureImage.size    = ImageSize::Bpp16;
        ctx.state->textureImage.width   = 1;
        
        // SetTile for load: 4b format, tmem = phead
        auto& lt = ctx.state->tiles[loadTile];
        lt.format   = 0; // RGBA
        lt.size     = 0; // 4b
        lt.line     = 0;
        lt.tmemAddr = phead;
        lt.palette  = 0;
        lt.clampS = 0; lt.clampT = 0;
        lt.maskS = 0; lt.maskT = 0;
        lt.shiftS = 0; lt.shiftT = 0;
        
        // LoadTLUT: count = pnum + 1 (pnum is count-1 per N64 SDK)
        // N64 SDK: gDPLoadTLUT(G_TX_LOADTILE, pnum) → DecodeLoadTLUT adds +1
        ctx.rdpProcessor->ExecuteLoadTLUT(loadTile, pnum + 1);
        break;
    }
    default:
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "S2DEX: Unknown ObjTxtr type 0x{:08X}", type);
        break;
    }
}

} // namespace Nova64::Video
