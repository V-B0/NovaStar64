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
    uint16_t    imageLoad;      // BG load mode
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
    void OnUcodeSwitch(DListContext& ctx) override;
    
protected:
    MicrocodeType m_type = MicrocodeType::S2DEX;
    GBIVersion m_gbiVersion = GBIVersion::GBI_S2DEX;
    const char* m_name = "S2DEX";
    
    // Current 2D state
    ObjMatrix m_objMatrix;
    bool m_objMatrixValid = false;
    uint32_t m_lastObjTextureSource = 0;
    uint16_t m_lastObjTextureTmem = 0;
    uint32_t m_objRendermode = 0;  // gSP.objRendermode equivalent (GLideN64)

    // G_SELECT_DL two-command state (mirrors GLideN64 gSP.selectDL).
    // 0xE1 (G_RDPHALF_0) sets this up; G_SELECT_DL (0xB0 / S2DEX2: 0x04)
    // reads it, assembles the full address, and conditionally jumps.
    struct SelectDLState {
        uint32_t addr = 0;  // low-16 bits of the segmented DL address
        uint8_t  sid  = 0;  // status register index (0-3, direct array index)
        uint32_t flag = 0;  // desired status bit pattern
    };
    SelectDLState m_selectDL;

    // NOTE: S2DEX status registers live in DListContext::s2dexStatusRegs,
    // NOT here — they survive G_LOAD_UCODE microcode switches within a task,
    // and are reset to 0 at the start of each RSP task in
    // MicrocodeDispatcher::ProcessGraphicsTask.

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
    m_lastObjTextureSource = 0;
    m_lastObjTextureTmem = 0;
    m_objRendermode = 0;
    m_selectDL = {};
    // s2dexStatusRegs live in DListContext and are reset here
    // because S2DEX DMEM is overwritten on G_LOAD_UCODE.
    ResetStats();
}

inline void S2DEXHandler::OnFrameBegin(DListContext& ctx) {
    m_objMatrixValid = false;
    m_lastObjTextureSource = 0;
    m_lastObjTextureTmem = 0;
    
    // Status registers live in DMEM. A G_LOAD_UCODE switch to S2DEX
    // reloads DMEM, effectively zeroing these registers. If we don't
    // zero them here, SELECT_DL will erroneously skip needed sub-displaylists
    // (due to stale flags), causing game freezes in OoT/MM.
    ctx.s2dexStatusRegs[0] = 0;
    ctx.s2dexStatusRegs[1] = 0;
    ctx.s2dexStatusRegs[2] = 0;
    ctx.s2dexStatusRegs[3] = 0;
}

inline void S2DEXHandler::OnUcodeSwitch(DListContext& ctx) {
    // Mid-task G_LOAD_UCODE switch into S2DEX.  On real hardware G_LOAD_UCODE
    // reloads DMEM from RDRAM (CPU wrote zeros there before task start), so
    // status registers are effectively zeroed on every switch — same as task
    // start.  Not zeroing them here causes SELECT_DL to skip sub-displaylists
    // it thinks already ran, freezing boot-logo and other animations.
    OnFrameBegin(ctx);
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

    // 0xE1 = G_RDPHALF_1.  In S2DEX1, when the *next* command is G_SELECT_DL
    // (0xB0), this serves as the setup half: it carries the low-16 bits of
    // the DL address, the status-register index, and the comparison flag.
    // Only store the setup state in that case; otherwise forward to the RDP.
    case 0xE1: {
        bool nextIsSelectDL = false;
        if (ctx.pc + 8 <= ctx.rdramSize) {
            nextIsSelectDL = ((ctx.ReadWord(ctx.pc) >> 24) & 0xFF)
                             == S2DEXCmd::G_SELECT_DL; // 0xB0
        }
        if (nextIsSelectDL) {
            m_selectDL.addr = w0 & 0x0000FFFFu;    // low 16 bits of DL addr
            m_selectDL.sid  = (w0 >> 18) & 0xFFu;  // status register index — bits [25:18] per GLideN64
            m_selectDL.flag = w1;                   // desired flag value
        } else {
            HandleRDPCommand(ctx, w0, w1);
        }
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
    case 0xDD: LoadUcode(ctx, w1, m_rdpHalf1, (w0 & 0xFFFF) + 1); return !ctx.halted;
    case 0xDE: {
        bool branch = ((w0 >> 16) & 0xFF) == 1;
        DisplayList(ctx, w1, branch);
        break;
    }
    case 0xDF: EndDisplayList(ctx); return !ctx.halted;
    // 0xE1 = G_RDPHALF_1 in F3DEX2/S2DEX2 — stores the data half for
    // G_LOAD_UCODE (used by 0xDD) and for texrect lower-right coords.
    // NOT the SELECT_DL preamble (that's 0xE4, see below).
    case 0xE1: m_rdpHalf1 = w1; break;
    // 0xE4 = G_RDPHALF_0 in S2DEX2 (GLideN64: S2DEX2_RDPHALF_0 = 0xE4).
    // In F3DEX2 this opcode is G_TEXRECT, but S2DEX2 repurposes it:
    // when followed by G_SELECT_DL (0x04) it is the SELECT_DL preamble
    // (stores low-16 addr bits, sid, flag); when followed by G_RDPHALF_1
    // (0xE1) it starts a texrect sequence.
    case 0xE4: {
        bool nextIsSelectDL = false;
        uint32_t nextWord = 0;
        if (ctx.pc + 8 <= ctx.rdramSize) {
            nextWord = ctx.ReadWord(ctx.pc);
            nextIsSelectDL = ((nextWord >> 24) & 0xFF) == S2DEXCmd::V2::G_SELECT_DL; // 0x04
        }
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
            "S2DEX2 0xE4: w0=0x{:08X} w1=0x{:08X} nextOp=0x{:02X} nextIsSelectDL={}",
            w0, w1, (nextWord >> 24) & 0xFF, nextIsSelectDL);
        if (nextIsSelectDL) {
            m_selectDL.addr = w0 & 0x0000FFFFu;
            m_selectDL.sid  = (w0 >> 18) & 0xFFu;  // bits [25:18] per GLideN64 — sid is a 2-bit index
            m_selectDL.flag = w1;
        } else {
            // Texrect upper-left half (followed by 0xE1 lower-right + 0xF1 tile).
            // Delegate to base RDP handler so texrects still work.
            HandleRDPCommand(ctx, w0, w1);
        }
        break;
    }
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
    uint8_t subType = (w0 >> 16) & 0xFF;
    
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
    // Store the S2DEX object render mode flags (mirrors GLideN64: gSP.objRendermode = w1).
    // Do NOT modify otherModeH — the render mode bits are S2DEX-specific flags
    // (G_OBJRM_BILERP, G_OBJRM_SHRINKSIZE_*, etc.) and are not otherModeH values.
    // RenderBgSprite / RenderObjSprite set cycle type explicitly when needed.
    (void)ctx; (void)w0;
    m_objRendermode = w1;
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
    // Two-command G_SELECT_DL protocol.
    //
    // Command 1 — G_RDPHALF_0 (0xE1/0xE4), handled by ProcessCommand before us:
    //   m_selectDL.addr  = w0[15:0]   low 16 bits of the segmented DL address
    //   m_selectDL.sid   = w0[25:18]  status register index
    //   m_selectDL.flag  = w1         desired bit pattern
    //
    // Command 2 — G_SELECT_DL (this function):
    //   w0[15:0]  = high 16 bits of the segmented DL address
    //   w0[23:16] = 0 → call (G_DL_PUSH), non-zero → branch (G_DL_NOPUSH)
    //   w1        = status mask
    //
    // Status-register guard (intra-task deduplication):
    //   DListContext::s2dexStatusRegs is reset to 0 at the start of every RSP
    //   task (in MicrocodeDispatcher::ProcessGraphicsTask).  Within a task the
    //   check prevents the same SELECT_DL from running more than once, breaking
    //   any DL cycles that the unconditional approach would loop forever on and
    //   flood the GPU with draws (triggering Intel Iris Xe TDR).
    //   Across tasks the status is always fresh, so each RSP task's SELECT_DL
    //   calls execute once — animated DLs update every VI frame.

    m_selectDL.addr |= (w0 & 0x0000FFFFu) << 16;

    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
        "S2DEX SELECT_DL: addr=0x{:08X} sid={} flag=0x{:08X} mask=0x{:08X}",
        m_selectDL.addr, m_selectDL.sid, m_selectDL.flag, w1);

    const uint8_t  sid  = m_selectDL.sid;
    const uint32_t flag = m_selectDL.flag;
    const uint32_t mask = w1;

    // sid here is S2DEX_RDPHalf_0's raw _SHIFTR(w0,18,8) field, which real
    // hardware/GLideN64 (S2DEX.cpp S2DEX_Select_DL) uses DIRECTLY as the
    // gSP.status[] index (validated < 4) -- unlike gSPSetStatus/MW_GENSTAT
    // and the OBJLT_TXTRSPRITE/TXTRTILE sid fields elsewhere in this file,
    // which really are byte offsets (0/4/8/12) needing >>2. This previously
    // used the same >>2 shift here too, which for the only valid real sid
    // values (0-3) collapses ALL FOUR status registers onto
    // s2dexStatusRegs[0] -- so two independent SELECT_DL sequences using
    // different sid values (e.g. selecting between different equipped-item
    // icon variants) would alias and corrupt each other's dedup state,
    // causing a DL to wrongly re-run (visible as stacked/duplicated icon
    // geometry) or wrongly get skipped.
    if (sid >= 4) {
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
            "S2DEX SELECT_DL: invalid sid {} (must be < 4)", sid);
        m_selectDL = {};
        return;
    }
    uint32_t& statusReg = ctx.s2dexStatusRegs[sid];

    if ((statusReg & mask) == flag) {
        // This SELECT_DL has already executed with this flag within the current
        // RSP task — skip to prevent DL cycles and redundant draws.
        m_selectDL = {};
        return;
    }
    // Update status so a second call with the same flag is skipped.
    statusReg = (statusReg & ~mask) | (flag & mask);

    // Validate resolved address before jumping.
    uint32_t resolvedAddr = ctx.ResolveAddress(m_selectDL.addr);
    if (resolvedAddr + 8 > ctx.rdramSize) {
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
            "S2DEX SELECT_DL target out of range — skipping: "
            "segAddr=0x{:08X} resolved=0x{:08X}",
            m_selectDL.addr, resolvedAddr);
        m_selectDL = {};
        return;
    }

    bool branch = ((w0 >> 16) & 0xFF) != 0; // G_DL_NOPUSH = branch
    DisplayList(ctx, m_selectDL.addr, branch);

    m_selectDL = {};
}

// ─── RDRAM Read Helpers ─────────────────────────────────────────────────

inline bool S2DEXHandler::ReadBgSprite(DListContext& ctx, uint32_t address, ObjBgSprite& bg) {
    address = ctx.ResolveAddress(address);
    if (address + 40 > ctx.rdramSize) return false;
    
    // uObjScaleBg_t from the SDK's gs2dex.h (40 bytes), read big-endian.
    // GLideN64 declares the fields of each 32-bit word in the opposite order
    // because mupen64plus keeps RDRAM byte-swapped; copying that order here
    // read every pair backwards (width 0, load mode as format/size), so no
    // background was ever drawn, including Majora's Mask's pause backdrop.
    //   0x00: imageX (u11.5), imageW (u10.2)
    //   0x04: frameX (s10.2), frameW (u10.2)
    //   0x08: imageY (u11.5), imageH (u10.2)
    //   0x0C: frameY (s10.2), frameH (u10.2)
    //   0x10: imagePtr
    //   0x14: imageLoad (u16), 0x16: imageFmt (u8), 0x17: imageSiz (u8)
    //   0x18: imagePal (u16), 0x1A: imageFlip (u16)
    //   0x1C: scaleW (u5.10), 0x1E: scaleH (u5.10)
    //   0x20: imageYorig (s20.11)
    bg.imageX     = ctx.ReadHalf(address + 0x00);
    bg.imageW     = ctx.ReadHalf(address + 0x02);
    bg.frameX     = static_cast<int16_t>(ctx.ReadHalf(address + 0x04));
    bg.frameW     = ctx.ReadHalf(address + 0x06);
    bg.imageY     = ctx.ReadHalf(address + 0x08);
    bg.imageH     = ctx.ReadHalf(address + 0x0A);
    bg.frameY     = static_cast<int16_t>(ctx.ReadHalf(address + 0x0C));
    bg.frameH     = ctx.ReadHalf(address + 0x0E);
    bg.imageAddr  = ctx.ReadWord(address + 0x10);
    bg.imageLoad  = ctx.ReadHalf(address + 0x14);
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
    
    // uObjSprite_t from the SDK's gs2dex.h (24 bytes), read big-endian. See
    // ReadBgSprite for why GLideN64's field order does not apply here.
    //   0x00: objX (s10.2), scaleW (u5.10), imageW (u10.5), paddingX
    //   0x08: objY (s10.2), scaleH (u5.10), imageH (u10.5), paddingY
    //   0x10: imageStride (u16), imageAdrs (u16)
    //   0x14: imageFmt, imageSiz, imagePal, imageFlags (u8 each)
    obj.objX        = static_cast<int16_t>(ctx.ReadHalf(address + 0x00));
    obj.scaleW      = ctx.ReadHalf(address + 0x02);
    obj.imageW      = ctx.ReadHalf(address + 0x04);
    obj.objY        = static_cast<int16_t>(ctx.ReadHalf(address + 0x08));
    obj.scaleH      = ctx.ReadHalf(address + 0x0A);
    obj.imageH      = ctx.ReadHalf(address + 0x0C);
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
    
    // uObjMtx_t from the SDK's gs2dex.h (24 bytes), read big-endian, the same
    // order as the uObjSubMtx_t read in Cmd_OBJ_MOVEMEM. See ReadBgSprite for
    // why GLideN64's field order does not apply here.
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
    // Strips are loaded the way the microcode loads them: the texture image
    // stays at the start of the picture, each strip is a LoadTile of the next
    // rows, and a strip's T coordinate is the row it starts at. The draw path
    // then sees the same commands as a game's own gDPLoadTextureTile strips,
    // which matters when the picture is a framebuffer (Majora's Mask copies
    // the frame into its pause backdrop and back this way).
    ctx.state->textureImage.address = resolvedAddr;
    ctx.state->textureImage.format  = bg.imageFormat;
    ctx.state->textureImage.size    = bg.imageSize;
    ctx.state->textureImage.width   = static_cast<uint16_t>(imageWi);

    float screenY = frameY;
    int row = std::clamp(static_cast<int>(imageY), 0, imageHi);
    
    while (row < imageHi && screenY < frameY + frameH) {
        int stripRows = std::min(rowsPerStrip, imageHi - row);
        int lastRow = row + stripRows - 1;
        
        // Load tile (tile 7): these rows of the picture into TMEM address 0
        auto& loadTile = ctx.state->tiles[7];
        loadTile.format   = static_cast<uint8_t>(bg.imageFormat);
        loadTile.size     = static_cast<uint8_t>(bg.imageSize);
        loadTile.tmemAddr = 0;
        loadTile.line     = static_cast<uint16_t>(lineWords);
        loadTile.palette  = 0;
        loadTile.clampS = 0; loadTile.clampT = 0;
        loadTile.maskS = 0; loadTile.maskT = 0;
        loadTile.shiftS = 0; loadTile.shiftT = 0;
        loadTile.sl = 0;
        loadTile.tl = row * 4;
        loadTile.sh = (imageWi - 1) * 4;
        loadTile.th = lastRow * 4;
        // Same bookkeeping as RDPProcessor::HandleLoadTile.
        ctx.state->tileLoadAddress[7] = resolvedAddr;
        ctx.state->tmemSourceAddress[0] = resolvedAddr;
        ctx.state->tmemLoadType[0] = RDPState::LOADTYPE_BLOCK;
        ctx.rdpProcessor->ExecuteLoadTile(7, 0.0f, static_cast<float>(row),
                                          static_cast<float>(imageWi - 1), static_cast<float>(lastRow));
        
        // Render tile (tile 0) covering the same rows
        auto& renderTile = ctx.state->tiles[0];
        renderTile.format   = static_cast<uint8_t>(bg.imageFormat);
        renderTile.size     = static_cast<uint8_t>(bg.imageSize);
        renderTile.tmemAddr = 0;
        renderTile.palette  = static_cast<uint8_t>(bg.imagePal & 0xF);
        renderTile.line     = static_cast<uint16_t>(lineWords);
        renderTile.sl = 0;
        renderTile.tl = row * 4;
        renderTile.sh = (imageWi - 1) * 4;
        renderTile.th = lastRow * 4;
        renderTile.clampS = 0; renderTile.clampT = 0;
        renderTile.maskS = 0; renderTile.maskT = 0;
        renderTile.shiftS = 0; renderTile.shiftT = 0;
        ctx.state->tileLoadAddress[0] = resolvedAddr;
        
        // Screen extents for this strip
        float stripScreenH = static_cast<float>(stripRows) / scaleH;
        float stripUlY = screenY;
        float stripLrY = std::min(screenY + stripScreenH, frameY + frameH);
        float ulx = frameX;
        float lrx = ulx + std::min(frameW, imageW / scaleW);
        
        // Texture coordinates, in texels per screen pixel. DrawTextureRect
        // takes them in this unit in every cycle mode; only
        // HandleTextureRect converts COPY mode's four-texel steps.
        float ulS = imageX;
        float lrS = ulS + (lrx - ulx) * scaleW;
        
        // Horizontal flip
        float drawUlx = ulx, drawLrx = lrx;
        if (bg.imageFlip) {
            std::swap(drawUlx, drawLrx);
        }
        
        DecodedRectangle rect{};
        rect.x0 = std::min(drawUlx, drawLrx);
        rect.y0 = stripUlY;
        rect.x1 = std::max(drawUlx, drawLrx);
        rect.y1 = stripLrY;
        rect.s  = bg.imageFlip ? lrS : ulS;
        rect.t  = static_cast<float>(row);
        rect.dsDx = bg.imageFlip ? -scaleW : scaleW;
        rect.dtDy = scaleH;
        rect.tileIndex = 0;
        rect.flipped = false;
        ctx.rdpProcessor->DrawTextureRect(rect);
        
        screenY += stripScreenH;
        row += stripRows;
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
        // Prefer the per-TMEM-slot source address set by LoadSpriteTexture;
        // fall back to a delta from the last bulk load only when that's unavailable.
        uint32_t tmemSource = ctx.state->tmemSourceAddress[obj.imageAdrs & 0x1FF];
        if (tmemSource != 0) {
            ctx.state->tileLoadAddress[0] = tmemSource;
        } else if (m_lastObjTextureSource != 0) {
            uint32_t tmemDelta = (obj.imageAdrs >= m_lastObjTextureTmem)
                ? static_cast<uint32_t>(obj.imageAdrs - m_lastObjTextureTmem)
                : 0u;
            ctx.state->tileLoadAddress[0] = (m_lastObjTextureSource + tmemDelta * 8u) & 0x00FFFFFFu;
        }
    }

    // dsDx/dtDy: texels per screen pixel (reciprocal of scale)
    float dsDx = (scaleW > 0.0f) ? (1.0f / scaleW) : 1.0f;
    float dtDy = (scaleH > 0.0f) ? (1.0f / scaleH) : 1.0f;

    bool flipS = (obj.imageFlags & 0x01) != 0;
    bool flipT = (obj.imageFlags & 0x10) != 0;

    DecodedRectangle rect{};
    rect.x0 = x;  rect.y0 = y;
    rect.x1 = x + screenW;  rect.y1 = y + screenH;
    rect.s = flipS ? w : 0.0f;
    rect.t = flipT ? h : 0.0f;
    rect.dsDx = flipS ? -dsDx : dsDx;  
    rect.dtDy = flipT ? -dtDy : dtDy;
    rect.tileIndex = 0;
    rect.flipped = false;

    // DIAG: log first 50 sprite renders + 1-in-200 sampling thereafter, capped
    // at 600 lines. MM HUD likely doesn't go through this path (uses F3DZEX
    // TexRect instead) but we'll capture any S2DEX2 cinematic/title sprites.
    static int s_spriteSeen = 0;
    static int s_spriteLogCount = 0;
    ++s_spriteSeen;
    bool shouldLogSprite = (s_spriteLogCount < 600) &&
                           ((s_spriteLogCount < 50) || ((s_spriteSeen % 200) == 0));
    if (shouldLogSprite) {
        ++s_spriteLogCount;
        NOVA_LOG(LogLevel::Info, LogSubsystem::Core,
            "S2DEX OBJ_SPRITE #{} (call {}): pos=({:.1f},{:.1f}) size=({:.1f}x{:.1f}) imageAdrs=0x{:04X} fmt={} siz={} pal={} stride={}",
            s_spriteLogCount, s_spriteSeen, x, y, screenW, screenH,
            (unsigned)obj.imageAdrs, (int)obj.imageFmt, (int)obj.imageSiz,
            (int)obj.imagePal, (int)obj.imageStride);
    }

    ctx.rdpProcessor->DrawTextureRect(rect);
}

inline void S2DEXHandler::LoadSpriteTexture(DListContext& ctx, uint32_t address) {
    address = ctx.ResolveAddress(address);
    if (address + 24 > ctx.rdramSize) return;
    if (!ctx.state || !ctx.rdpProcessor) return;
    
    // N64 uObjTxtr (union of Block/Tile/TLUT, each 24 bytes), SDK gs2dex.h order:
    //   0x00: type (u32) — identifies which variant
    //   0x04: image (u32) — RDRAM source address (segmented)
    //   0x08: tmem / phead, 0x0A: tsize / twidth / pnum,
    //   0x0C: tline / theight / zero, 0x0E: sid, 0x10: flag, 0x14: mask
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
        // sid is a byte offset; divide by 4 for the array index
        uint16_t sid   = ctx.ReadHalf(address + 0x0E);
        uint32_t flag  = ctx.ReadWord(address + 0x10);
        uint32_t mask  = ctx.ReadWord(address + 0x14);
        
        // SetTextureImage: RGBA 16b, width=1 (N64 SDK: gDPSetTextureImage(RGBA, SIZ_16b, 1, image))
        ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
        ctx.state->textureImage.format  = ImageFormat::RGBA;
        ctx.state->textureImage.size    = ImageSize::Bpp16;
        ctx.state->textureImage.width   = 1;
        ctx.state->tileLoadAddress[loadTile] = ctx.state->textureImage.address;
        m_lastObjTextureSource = ctx.state->textureImage.address;
        m_lastObjTextureTmem = tmem;
        
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
        
        ctx.rdpProcessor->ExecuteLoadBlock(loadTile, 0, 0, tsize, tline);
        // Mirror the tmemSourceAddress update that HandleLoadBlock would perform,
        // since we call ExecuteLoadBlock directly (bypassing HandleLoadBlock).
        ctx.state->tmemSourceAddress[tmem & 0x1FF] = ctx.state->textureImage.address;
        
        // Update S2DEX status register (sid is a byte offset 0/4/8/12; >>2 = array index 0-3).
        { uint8_t idx = (sid >> 2) & 0xFF;
          ctx.s2dexStatusRegs[idx] = (ctx.s2dexStatusRegs[idx] & ~mask) | (flag & mask); }
        break;
    }
    case 0x1034: { // G_OBJLT_TXTRTILE
        uint16_t tmem    = ctx.ReadHalf(address + 0x08);
        uint16_t twidth  = ctx.ReadHalf(address + 0x0A);
        uint16_t theight = ctx.ReadHalf(address + 0x0C);
        uint16_t sid     = ctx.ReadHalf(address + 0x0E);
        uint32_t flag    = ctx.ReadWord(address + 0x10);
        uint32_t mask    = ctx.ReadWord(address + 0x14);
        
        // SetTextureImage: RGBA 16b, width = twidth+1
        ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
        ctx.state->textureImage.format  = ImageFormat::RGBA;
        ctx.state->textureImage.size    = ImageSize::Bpp16;
        ctx.state->textureImage.width   = twidth + 1;
        ctx.state->tileLoadAddress[loadTile] = ctx.state->textureImage.address;
        m_lastObjTextureSource = ctx.state->textureImage.address;
        m_lastObjTextureTmem = tmem;
        
        // SetTile for load tile
        auto& lt = ctx.state->tiles[loadTile];
        lt.format   = 0; // RGBA
        lt.size     = 2; // 16b
        lt.line     = static_cast<uint16_t>((static_cast<uint32_t>(twidth) + 4u) >> 2);
        lt.tmemAddr = tmem;
        lt.palette  = 0;
        lt.clampS = 0; lt.clampT = 0;
        lt.maskS = 0; lt.maskT = 0;
        lt.shiftS = 0; lt.shiftT = 0;
        
        ctx.rdpProcessor->ExecuteLoadTile(loadTile, 0.0f, 0.0f,
            static_cast<float>(twidth), static_cast<float>(theight));
        // Mirror the tmemSourceAddress update that HandleLoadTile would perform.
        ctx.state->tmemSourceAddress[tmem & 0x1FF] = ctx.state->textureImage.address;
        
        { uint8_t idx = (sid >> 2) & 0xFF;
          ctx.s2dexStatusRegs[idx] = (ctx.s2dexStatusRegs[idx] & ~mask) | (flag & mask); }
        break;
    }
    case 0x0030: { // G_OBJLT_TLUT (GLideN64 uses 0x00000030)
        uint16_t phead = ctx.ReadHalf(address + 0x08);
        uint16_t pnum  = ctx.ReadHalf(address + 0x0A);
        uint16_t sid   = ctx.ReadHalf(address + 0x0E);
        uint32_t flag  = ctx.ReadWord(address + 0x10);
        uint32_t mask  = ctx.ReadWord(address + 0x14);
        
        // SetTextureImage: RGBA 16b, width = 1
        ctx.state->textureImage.address = resolvedImage & 0x00FFFFFF;
        ctx.state->textureImage.format  = ImageFormat::RGBA;
        ctx.state->textureImage.size    = ImageSize::Bpp16;
        ctx.state->textureImage.width   = 1;
        
        // SetTile for load: 4b format, tmem = phead + 256.
        // phead is a palette-entry index (0-255), not an absolute TMEM QW address.
        // The palette region starts at QW 256 (byte 2048), so the actual TMEM slot
        // is phead + 256.  GLideN64 reference: gSP.cpp G_OBJLT_TLUT case:
        //   u32 tmem = lt->phead + 256;
        // Without +256, tmemAddr < 256 triggers the early-return guard in
        // ExecuteLoadTLUT and the TLUT is silently never loaded.
        auto& lt = ctx.state->tiles[loadTile];
        lt.format   = 0; // RGBA
        lt.size     = 0; // 4b
        lt.line     = 0;
        lt.tmemAddr = phead + 256;
        lt.palette  = 0;
        lt.clampS = 0; lt.clampT = 0;
        lt.maskS = 0; lt.maskT = 0;
        lt.shiftS = 0; lt.shiftT = 0;
        // Real G_LOADTLUT carries no uls/ult in its own command word (always 0)
        // -- this path calls ExecuteLoadTLUT directly, bypassing HandleLoadTLUT's
        // reset, so sl/tl must be zeroed here too or ExecuteLoadTLUT's address
        // math reads whatever a prior SetTileSize/LoadTile/LoadBlock left on
        // this tile (typically tile 7, reused across every load).
        lt.sl = 0; lt.tl = 0;

        // LoadTLUT: count = pnum + 1 (pnum is count-1 per N64 SDK)
        // N64 SDK: gDPLoadTLUT(G_TX_LOADTILE, pnum) → DecodeLoadTLUT adds +1
        ctx.rdpProcessor->ExecuteLoadTLUT(loadTile, pnum + 1);
        
        { uint8_t idx = (sid >> 2) & 0xFF;
          ctx.s2dexStatusRegs[idx] = (ctx.s2dexStatusRegs[idx] & ~mask) | (flag & mask); }
        break;
    }
    default:
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "S2DEX: Unknown ObjTxtr type 0x{:08X}", type);
        break;
    }
}

} // namespace Nova64::Video
