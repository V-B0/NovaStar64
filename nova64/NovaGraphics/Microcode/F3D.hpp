/**
 * @file F3D.hpp
 * @brief Fast3D (original) microcode handler
 *
 * The original Fast3D microcode used by early N64 titles like Super Mario 64
 * and Wave Race 64. Uses GBI1 command encoding with 16 vertex buffer.
 */

#pragma once

#include "GBIHandler.hpp"

namespace Nova64::Video {

class F3DHandler : public GBIHandler {
public:
    F3DHandler() = default;
    ~F3DHandler() override = default;
    
    void Initialize(const MicrocodeInfo& info) override;
    MicrocodeType GetType() const override { return MicrocodeType::F3D; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "Fast3D"; }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    
private:
    // ─── F3D-specific command handlers ───────────────────────────────
    void Cmd_G_SPNOOP(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_VTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_DL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CULLDL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_POPMTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEWORD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_H(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_ENDDL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CLEARGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_2(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_CONT(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SPRITE2D_BASE(DListContext& ctx, uint32_t w0, uint32_t w1);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation
// ═══════════════════════════════════════════════════════════════════════════

inline void F3DHandler::Initialize(const MicrocodeInfo& info) {
    (void)info;
    ResetStats();
}

inline bool F3DHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    if (opcode >= 0xC0) {
        // RDP command passthrough
        HandleRDPCommand(ctx, w0, w1);
        return true;
    }
    
    switch (opcode) {
    case 0x00: Cmd_G_SPNOOP(ctx, w0, w1); break;
    case 0x01: Cmd_G_MTX(ctx, w0, w1); break;
    case 0x03: Cmd_G_MOVEMEM(ctx, w0, w1); break;
    case 0x04: Cmd_G_VTX(ctx, w0, w1); break;
    case 0x06: Cmd_G_DL(ctx, w0, w1); break;
    case 0x09: Cmd_G_SPRITE2D_BASE(ctx, w0, w1); break;
    
    case 0xB2: Cmd_G_RDPHALF_CONT(ctx, w0, w1); break;
    case 0xB3: Cmd_G_RDPHALF_2(ctx, w0, w1); break;
    case 0xB4: Cmd_G_RDPHALF_1(ctx, w0, w1); break;
    case 0xB6: Cmd_G_CLEARGEOMETRYMODE(ctx, w0, w1); break;
    case 0xB7: Cmd_G_SETGEOMETRYMODE(ctx, w0, w1); break;
    case 0xB8: Cmd_G_ENDDL(ctx, w0, w1); break;
    case 0xB9: Cmd_G_SETOTHERMODE_L(ctx, w0, w1); break;
    case 0xBA: Cmd_G_SETOTHERMODE_H(ctx, w0, w1); break;
    case 0xBB: Cmd_G_TEXTURE(ctx, w0, w1); break;
    case 0xBC: Cmd_G_MOVEWORD(ctx, w0, w1); break;
    case 0xBD: Cmd_G_POPMTX(ctx, w0, w1); break;
    case 0xBE: Cmd_G_CULLDL(ctx, w0, w1); break;
    case 0xBF: Cmd_G_TRI1(ctx, w0, w1); break;
    
    default:
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "F3D: Unknown opcode 0x{:02X} (w0=0x{:08X}, w1=0x{:08X})", opcode, w0, w1);
        break;
    }
    
    return !ctx.halted;
}

// ─── Individual command implementations ──────────────────────────────────

inline void F3DHandler::Cmd_G_SPNOOP(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
}

inline void F3DHandler::Cmd_G_MTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t params = w0 & 0x00FFFFFF;
    bool push = (params & 0x01) == 0; // bit 0: 0=push, 1=nopush
    bool load = (params & 0x02) != 0; // bit 1: 0=mul, 1=load
    bool proj = (params & 0x04) != 0; // bit 2: 0=modelview, 1=projection
    
    LoadMatrix(ctx, w1, push, load, proj);
}

inline void F3DHandler::Cmd_G_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = (w0 >> 16) & 0xFF;
    uint8_t size = ((w0 >> 19) & 0x1F) * 8;
    uint32_t address = w1;
    
    MoveMemGBI1(ctx, index, size, address);
}

inline void F3DHandler::Cmd_G_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // F3D vertex format: count-1 in bits [23:20], v0 in bits [19:16]
    int count = ((w0 >> 20) & 0xF) + 1;
    int startIndex = ((w0 >> 16) & 0xF);
    
    LoadVertices(ctx, w1, count, startIndex);
}

inline void F3DHandler::Cmd_G_DL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    bool branch = ((w0 >> 16) & 0xFF) == 1;
    DisplayList(ctx, w1, branch);
}

inline void F3DHandler::Cmd_G_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    // F3D: vertex indices in w1 bytes
    int v0 = ((w1 >> 16) & 0xFF) / 10; // F3D uses vtx*10 encoding
    int v1 = ((w1 >> 8) & 0xFF) / 10;
    int v2 = (w1 & 0xFF) / 10;
    
    DrawTriangle(ctx, v0, v1, v2);
}

inline void F3DHandler::Cmd_G_CULLDL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t start = ((w0 & 0x00FFFFFF) / 40);
    uint32_t end = (w1 / 40) - 1;  // F3D encodes exclusive end
    CullDisplayList(ctx, start, end);
}

inline void F3DHandler::Cmd_G_POPMTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    PopMatrix(ctx, w1);
}

inline void F3DHandler::Cmd_G_MOVEWORD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI1 encoding: index at bits [7:0], offset at bits [23:8]
    uint8_t index = w0 & 0xFF;
    uint16_t offset = static_cast<uint16_t>((w0 >> 8) & 0xFFFF);
    
    // GBI1 numLights formula: w1 = 0x80000000 + (numLights+1)*32
    if (index == MoveWord::MW_NUMLIGHT) {
        if (!ctx.state) return;
        int numLights = static_cast<int>(((w1 - 0x80000000u) >> 5) - 1);
        if (numLights < 0) numLights = 0;
        if (numLights >= MAX_LIGHTS) numLights = MAX_LIGHTS - 1;
        ctx.state->geometry.numLights = numLights;
        ctx.state->MarkDirty(RDPDirtyFlag::Lights);
        return;
    }
    
    // GBI1 light colors: 32-byte stride (offsets 0x00,0x20,0x40,...)
    // Base handler uses 24-byte stride (GBI2), so intercept here
    if (index == MoveWord::MW_LIGHTCOL) {
        if (!ctx.state) return;
        // Only handle "a" offsets (primary color at offset % 0x20 == 0)
        // "b" offsets (0x04,0x24,...) are secondary copies, ignored by GLideN64
        if ((offset & 0x1F) == 0) {
            int lightIdx = offset / 0x20;
            if (lightIdx >= 0 && lightIdx < MAX_LIGHTS) {
                auto& light = ctx.state->geometry.lights[lightIdx];
                light.color[0] = static_cast<float>((w1 >> 24) & 0xFF) / 255.0f;
                light.color[1] = static_cast<float>((w1 >> 16) & 0xFF) / 255.0f;
                light.color[2] = static_cast<float>((w1 >> 8)  & 0xFF) / 255.0f;
                ctx.state->MarkDirty(RDPDirtyFlag::Lights);
            }
        }
        return;
    }
    
    MoveWord(ctx, index, offset, w1);
}

inline void F3DHandler::Cmd_G_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    SetTextureState(ctx, w0, w1);
}

inline void F3DHandler::Cmd_G_SETOTHERMODE_H(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t shift = (w0 >> 8) & 0xFF;
    uint32_t length = (w0 & 0xFF);  // GBI1: raw byte IS the length (no -1 encoding)
    SetOtherModeH(ctx, shift, length, w1);
}

inline void F3DHandler::Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t shift = (w0 >> 8) & 0xFF;
    uint32_t length = (w0 & 0xFF);  // GBI1: raw byte IS the length (no -1 encoding)
    SetOtherModeL(ctx, shift, length, w1);
}

inline void F3DHandler::Cmd_G_ENDDL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0; (void)w1;
    EndDisplayList(ctx);
}

/// Translate GBI1 geometry mode bits to GBI2 (internal) format.
/// GBI1 and GBI2 share most bit positions, but SHADING_SMOOTH, CULL_FRONT,
/// and CULL_BACK differ:
///   GBI1: SHADING_SMOOTH=0x200, CULL_FRONT=0x1000, CULL_BACK=0x2000
///   GBI2: SHADING_SMOOTH=0x200000, CULL_FRONT=0x200, CULL_BACK=0x400
static inline uint32_t TranslateGBI1ToGBI2(uint32_t gbi1) {
    constexpr uint32_t GBI1_SHADING_SMOOTH = 0x00000200;
    constexpr uint32_t GBI1_CULL_FRONT     = 0x00001000;
    constexpr uint32_t GBI1_CULL_BACK      = 0x00002000;
    constexpr uint32_t GBI1_MASK = GBI1_SHADING_SMOOTH | GBI1_CULL_FRONT | GBI1_CULL_BACK;
    uint32_t result = gbi1 & ~GBI1_MASK;
    if (gbi1 & GBI1_SHADING_SMOOTH) result |= 0x00200000; // G_SHADING_SMOOTH
    if (gbi1 & GBI1_CULL_FRONT)     result |= 0x00000200; // G_CULL_FRONT
    if (gbi1 & GBI1_CULL_BACK)      result |= 0x00000400; // G_CULL_BACK
    return result;
}

inline void F3DHandler::Cmd_G_SETGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w1;
    SetGeometryMode(ctx, 0, TranslateGBI1ToGBI2(w0 & 0x00FFFFFF));
}

inline void F3DHandler::Cmd_G_CLEARGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w1;
    SetGeometryMode(ctx, TranslateGBI1ToGBI2(w0 & 0x00FFFFFF), 0);
}

inline void F3DHandler::Cmd_G_RDPHALF_1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf1 = w1;
}

inline void F3DHandler::Cmd_G_RDPHALF_2(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf2 = w1;
}

inline void F3DHandler::Cmd_G_RDPHALF_CONT(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
    // Used in conjunction with RDPHALF_1 for 128-bit RDP triangle commands
    // The actual triangle data is assembled from the stored halves
}

inline void F3DHandler::Cmd_G_SPRITE2D_BASE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    if (!ctx.state) return;

    // Read uSprite struct (24 bytes) from RDRAM
    uint32_t address = ctx.ResolveAddress(w1);
    if (address + 24 > ctx.rdramSize) return;

    uint32_t imagePtr = ctx.ReadWord(address + 0);
    uint32_t tlutPtr  = ctx.ReadWord(address + 4);
    int16_t  imageW   = static_cast<int16_t>(ctx.ReadHalf(address + 8));
    int16_t  stride   = static_cast<int16_t>(ctx.ReadHalf(address + 10));
    int8_t   imageSiz = static_cast<int8_t>(ctx.ReadByte(address + 12));
    int8_t   imageFmt = static_cast<int8_t>(ctx.ReadByte(address + 13));
    int16_t  imageH   = static_cast<int16_t>(ctx.ReadHalf(address + 14));
    int16_t  imageY   = static_cast<int16_t>(ctx.ReadHalf(address + 16));
    int16_t  imageX   = static_cast<int16_t>(ctx.ReadHalf(address + 18));

    uint32_t resolvedImagePtr = ctx.ResolveAddress(imagePtr);

    // Load TLUT if present
    if (tlutPtr != 0) {
        uint32_t resolvedTlut = ctx.ResolveAddress(tlutPtr);
        // SetTextureImage(RGBA, 16b, 1, tlutPtr)
        uint32_t stw0 = (0x3Du << 24) | (0u << 21) | (2u << 19) | 0; // RGBA, 16b, width=1
        HandleRDPCommand(ctx, stw0, resolvedTlut);
        // SetTile(RGBA, 16b, 0, 256, tile=7, ...)
        uint32_t tilew0 = (0x35u << 24) | (0u << 21) | (2u << 19) | (0u << 9) | 256u;
        uint32_t tilew1 = (7u << 24); // tile 7, all other params 0
        HandleRDPCommand(ctx, tilew0, tilew1);
        // LoadTLUT(tile=7, sl=0, tl=0, sh=1020, th=0)
        uint32_t ltw0 = (0x30u << 24) | 0; // sl=0, tl=0
        uint32_t ltw1 = (7u << 24) | (1020u << 12); // sh=1020, th=0
        HandleRDPCommand(ctx, ltw0, ltw1);

        // Set texture LUT mode in otherModeH
        // G_TT_RGBA16 for non-RGBA formats, G_TT_NONE for RGBA
        if (imageFmt != 0) { // not RGBA
            // Set textureLUT bits (bits 15:14) to 2 (RGBA16)
            ctx.state->otherModeH = (ctx.state->otherModeH & ~(3u << 14)) | (2u << 14);
        } else {
            ctx.state->otherModeH = (ctx.state->otherModeH & ~(3u << 14));
        }
    } else {
        // Clear texture LUT
        ctx.state->otherModeH = (ctx.state->otherModeH & ~(3u << 14));
    }

    // SetTextureImage for the sprite bitmap
    {
        uint32_t stw0 = (0x3Du << 24) |
                        (static_cast<uint32_t>(imageFmt & 7) << 21) |
                        (static_cast<uint32_t>(imageSiz & 3) << 19) |
                        (static_cast<uint32_t>(stride > 0 ? stride - 1 : 0) & 0x3FF);
        HandleRDPCommand(ctx, stw0, resolvedImagePtr);
    }

    // Set texture state: scaleS=1.0, scaleT=1.0, on
    auto& geo = ctx.state->geometry;
    geo.texture.scaleS = 0xFFFF;
    geo.texture.scaleT = 0xFFFF;
    geo.texture.level = 0;
    geo.texture.tileIndex = 0;
    geo.texture.enabled = true;

    // Enable perspective texture
    ctx.state->otherModeH |= OTHERMODE_TEXTPERSP; // texturePersp (bit 19)

    float z = 0.0f;
    float ww = 1.0f;

    float scaleX = 1.0f, scaleY = 1.0f;
    uint8_t flipX = 0, flipY = 0;

    // Read subsequent Sprite2D commands: 0xBE (ScaleFlip), 0xBD (Draw)
    for (;;) {
        if (ctx.pc + 8 > ctx.rdramSize) break;

        uint32_t nextW0 = ctx.ReadWord(ctx.pc);
        uint8_t nextCmd = (nextW0 >> 24) & 0xFF;

        if (nextCmd != 0xBE && nextCmd != 0xBD) break;

        uint32_t nextW1 = ctx.ReadWord(ctx.pc + 4);
        ctx.pc += 8; // consume the command

        if (nextCmd == 0xBE) {
            // Sprite2DScaleFlip
            scaleX = static_cast<float>((nextW1 >> 16) & 0xFFFF) / 1024.0f;
            scaleY = static_cast<float>(nextW1 & 0xFFFF) / 1024.0f;
            flipX = (nextW0 >> 8) & 0xFF;
            flipY = nextW0 & 0xFF;
            continue;
        }

        // 0xBD: Sprite2DDraw
        float frameX = static_cast<float>(static_cast<int16_t>((nextW1 >> 16) & 0xFFFF)) / 4.0f;
        float frameY = static_cast<float>(static_cast<int16_t>(nextW1 & 0xFFFF)) / 4.0f;
        float frameW = (scaleX > 0.0f) ? static_cast<float>(imageW) / scaleX : 0.0f;
        float frameH = (scaleY > 0.0f) ? static_cast<float>(imageH) / scaleY : 0.0f;

        float ulx, uly, lrx, lry;
        if (flipX) { ulx = frameX + frameW; lrx = frameX; }
        else       { ulx = frameX;          lrx = frameX + frameW; }
        if (flipY) { uly = frameY + frameH; lry = frameY; }
        else       { uly = frameY;          lry = frameY + frameH; }

        float uls = static_cast<float>(imageX);
        float ult = static_cast<float>(imageY);
        float lrs = uls + static_cast<float>(imageW);
        float lrt = ult + static_cast<float>(imageH);
        if (scaleY != 1.0f) { lrs -= 1.0f; lrt -= 1.0f; }

        // Build screen-space quad (4 vertices)
        auto& vb = geo.vertexBuffer;
        auto SetVtx = [&](int idx, float vx, float vy, float vs, float vt) {
            auto& v = vb[idx];
            v.x = vx; v.y = vy; v.z = z; v.w = ww;
            v.s = vs; v.t = vt;
            v.r = 1.0f; v.g = 1.0f; v.b = 1.0f; v.a = 1.0f;
            v.screenX = vx; v.screenY = vy; v.screenZ = z;
            v.invW = 1.0f; v.clipFlags = 0; v.reject = false;
            geo.vertexLoaded[idx] = true;
        };
        SetVtx(0, ulx, uly, uls, ult);
        SetVtx(1, lrx, uly, lrs, ult);
        SetVtx(2, ulx, lry, uls, lrt);
        SetVtx(3, lrx, lry, lrs, lrt);

        if (stride > 0) {
            DrawTriangle(ctx, 0, 1, 2);
            DrawTriangle(ctx, 1, 3, 2);
            m_trianglesProcessed += 2;
        }
    }
}

} // namespace Nova64::Video
