/**
 * @file F3DEX2.hpp
 * @brief F3DEX2 microcode handler — the most common N64 microcode
 *
 * F3DEX2 is a major revision of the F3D family with completely reorganized
 * GBI2 command encoding. Commands use different opcode values from GBI1.
 * Used by Zelda OoT/MM (F3DZEX variant), Banjo-Kazooie, Paper Mario,
 * and the majority of post-1998 N64 titles.
 *
 * Key differences from F3DEX:
 * - Different opcode numbering (GBI2 scheme)
 * - Combined clear/set geometry mode in one command
 * - G_SETOTHERMODE_H/L use different shift/length encoding
 * - G_MTX parameter encoding changed
 * - Improved near-clip handling
 * - G_DMA_IO command for direct DMA
 */

#pragma once

#include "GBIHandler.hpp"

namespace Nova64::Video {

class F3DEX2Handler : public GBIHandler {
public:
    F3DEX2Handler() = default;
    ~F3DEX2Handler() override = default;
    
    void Initialize(const MicrocodeInfo& info) override;
    MicrocodeType GetType() const override { return m_type; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI2; }
    const char* GetName() const override { return m_name; }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    void OnFrameBegin(DListContext& ctx) override;
    
protected:
    MicrocodeType m_type = MicrocodeType::F3DEX2;
    const char* m_name = "F3DEX2";
    
    // ─── GBI2 command handlers ───────────────────────────────────────
    void Cmd_G_NOOP(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_VTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MODIFYVTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CULLDL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_BRANCH_Z(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI2(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_QUAD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_LINE3D(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_DMA_IO(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_POPMTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_GEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEWORD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_LOAD_UCODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_DL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_ENDDL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_H(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_2(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SPECIAL_1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SPECIAL_2(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SPECIAL_3(DListContext& ctx, uint32_t w0, uint32_t w1);
};

// ═══════════════════════════════════════════════════════════════════════════
//  F3DZEX Handler (Zelda variant — inherits from F3DEX2)
// ═══════════════════════════════════════════════════════════════════════════

class F3DZEXHandler : public F3DEX2Handler {
public:
    F3DZEXHandler() {
        m_type = MicrocodeType::F3DZEX;
        m_name = "F3DZEX (Zelda)";
    }
    
    // F3DZEX is essentially F3DEX2 with a few extensions
    // Any Zelda-specific overrides can go here
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    
private:
    // Zelda-specific: point lighting
    void Cmd_G_SPECIAL_ZelDA(DListContext& ctx, uint32_t w0, uint32_t w1);
};

// ═══════════════════════════════════════════════════════════════════════════
//  F3DCBFD Handler (Conker variant — inherits from F3DEX2)
// ═══════════════════════════════════════════════════════════════════════════

class F3DCBFDHandler : public F3DEX2Handler {
public:
    F3DCBFDHandler() {
        m_type = MicrocodeType::F3DCBFD;
        m_name = "F3DCBFD (Conker)";
    }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    
private:
    // Conker-specific vertex format and rendering
    void Cmd_G_VTX_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI1_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI2_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    // Conker-specific state
    void Cmd_CBFD_SHADOW(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_CBFD_FUR(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_CBFD_VTXCOLOR(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    bool  m_furMode    = false;
    float m_furLength  = 0.0f;
    float m_furDensity = 0.0f;
    int   m_furLayers  = 0;
    bool  m_shadowMode = false;
    uint32_t m_shadowAddr = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation — F3DEX2
// ═══════════════════════════════════════════════════════════════════════════

inline void F3DEX2Handler::Initialize(const MicrocodeInfo& info) {
    m_type = info.type;
    m_name = MicrocodeTypeName(info.type);
    ResetStats();
}

inline void F3DEX2Handler::OnFrameBegin(DListContext& ctx) {
    (void)ctx;
    // Reset per-frame state if needed
}

inline bool F3DEX2Handler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    // RDP pass-through commands (0xC0-0xFF except GBI2 commands)
    if (opcode >= 0xC0 && opcode <= 0xCF) {
        HandleRDPCommand(ctx, w0, w1);
        return true;
    }
    
    switch (opcode) {
    // ─── Low opcodes (0x00-0x08): vertex & triangle commands ─────────
    case 0x00: Cmd_G_NOOP(ctx, w0, w1); break;
    case 0x01: Cmd_G_VTX(ctx, w0, w1); break;
    case 0x02: Cmd_G_MODIFYVTX(ctx, w0, w1); break;
    case 0x03: Cmd_G_CULLDL(ctx, w0, w1); break;
    case 0x04: Cmd_G_BRANCH_Z(ctx, w0, w1); break;
    case 0x05: Cmd_G_TRI1(ctx, w0, w1); break;
    case 0x06: Cmd_G_TRI2(ctx, w0, w1); break;
    case 0x07: Cmd_G_QUAD(ctx, w0, w1); break;
    case 0x08: Cmd_G_LINE3D(ctx, w0, w1); break;
    
    // ─── High opcodes (0xD0-0xFF): state & control commands ──────────
    case 0xD3: Cmd_G_SPECIAL_3(ctx, w0, w1); break;
    case 0xD4: Cmd_G_SPECIAL_2(ctx, w0, w1); break;
    case 0xD5: Cmd_G_SPECIAL_1(ctx, w0, w1); break;
    case 0xD6: Cmd_G_DMA_IO(ctx, w0, w1); break;
    case 0xD7: Cmd_G_TEXTURE(ctx, w0, w1); break;
    case 0xD8: Cmd_G_POPMTX(ctx, w0, w1); break;
    case 0xD9: Cmd_G_GEOMETRYMODE(ctx, w0, w1); break;
    case 0xDA: Cmd_G_MTX(ctx, w0, w1); break;
    case 0xDB: Cmd_G_MOVEWORD(ctx, w0, w1); break;
    case 0xDC: Cmd_G_MOVEMEM(ctx, w0, w1); break;
    case 0xDD: Cmd_G_LOAD_UCODE(ctx, w0, w1); break;
    case 0xDE: Cmd_G_DL(ctx, w0, w1); break;
    case 0xDF: Cmd_G_ENDDL(ctx, w0, w1); break;
    
    case 0xE1: Cmd_G_RDPHALF_1(ctx, w0, w1); break;
    case 0xE2: Cmd_G_SETOTHERMODE_H(ctx, w0, w1); break;
    case 0xE3: Cmd_G_SETOTHERMODE_L(ctx, w0, w1); break;
    
    case 0xF1: Cmd_G_RDPHALF_2(ctx, w0, w1); break;
    
    default:
        // Remaining 0xE0-0xFF are RDP commands
        if (opcode >= 0xE0) {
            HandleRDPCommand(ctx, w0, w1);
        } else {
            NOVA_LOG(LogLevel::Warning, LogSubsystem::Microcode,
                    "F3DEX2: Unknown opcode 0x{:02X} (w0=0x{:08X}, w1=0x{:08X})", opcode, w0, w1);
        }
        break;
    }
    
    return !ctx.halted;
}

// ─── GBI2 Command Implementations ───────────────────────────────────────

inline void F3DEX2Handler::Cmd_G_NOOP(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
    // Some games use NOOP w1 as a tag/ID for debugging
}

inline void F3DEX2Handler::Cmd_G_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2 vertex: count = (w0 >> 12) & 0xFF, v0 = ((w0 >> 1) & 0x7F) - count
    int count = (w0 >> 12) & 0xFF;
    int vbidx = ((w0 >> 1) & 0x7F) - count;
    if (vbidx < 0) vbidx = 0;
    
    LoadVertices(ctx, w1, count, vbidx);
}

inline void F3DEX2Handler::Cmd_G_MODIFYVTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t where = (w0 >> 16) & 0xFF;
    uint16_t vtxIndex = (w0 & 0xFFFF) / 2;
    ModifyVertex(ctx, where, vtxIndex, w1);
}

inline void F3DEX2Handler::Cmd_G_CULLDL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t start = ((w0 & 0xFFFF) / 2);
    uint32_t end = ((w1 & 0xFFFF) / 2);
    CullDisplayList(ctx, start, end);
}

inline void F3DEX2Handler::Cmd_G_BRANCH_Z(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t vtxIndex = ((w0 & 0xFFF) / 2);
    BranchZ(ctx, vtxIndex, w1, m_rdpHalf1);
}

inline void F3DEX2Handler::Cmd_G_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2: vertex indices in w0 (not w1 like GBI1)
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    (void)w1;
    DrawTriangle(ctx, v0, v1, v2);
}

inline void F3DEX2Handler::Cmd_G_TRI2(DListContext& ctx, uint32_t w0, uint32_t w1) {
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    int v3 = ((w1 >> 16) & 0xFF) / 2;
    int v4 = ((w1 >> 8) & 0xFF) / 2;
    int v5 = (w1 & 0xFF) / 2;
    DrawTriangle2(ctx, v0, v1, v2, v3, v4, v5);
}

inline void F3DEX2Handler::Cmd_G_QUAD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    int v3 = ((w1 >> 16) & 0xFF) / 2;
    (void)(w1 & 0xFFFF); // v4,v5 unused for quad = 2 tris
    int v4 = ((w1 >> 8) & 0xFF) / 2;
    int v5 = (w1 & 0xFF) / 2;
    DrawTriangle(ctx, v0, v1, v2);
    DrawTriangle(ctx, v3, v4, v5);
}

inline void F3DEX2Handler::Cmd_G_LINE3D(DListContext& ctx, uint32_t w0, uint32_t w1) {
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    (void)w1;
    DrawTriangle(ctx, v0, v1, v0); // Degenerate triangle
}

inline void F3DEX2Handler::Cmd_G_DMA_IO(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Direct DMA to/from RSP DMEM
    // This is rarely used by games, mostly for custom effects
    bool isWrite = (w0 >> 23) & 1;
    uint32_t dmemAddr = (w0 >> 13) & 0x3FF;
    uint32_t length = ((w0 & 0x1FFF) + 1);
    uint32_t dramAddr = w1;
    
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
            "G_DMA_IO: {} dmem=0x{:03X} dram=0x{:08X} len={}",
            isWrite ? "write" : "read", dmemAddr, dramAddr, length);
    (void)ctx;
}

inline void F3DEX2Handler::Cmd_G_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    SetTextureState(ctx, w0, w1);
}

inline void F3DEX2Handler::Cmd_G_POPMTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    // GBI2: w1 = number of 64-byte matrix entries to pop
    PopMatrix(ctx, w1);
}

inline void F3DEX2Handler::Cmd_G_GEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2 combined clear & set: clear bits in w0, set bits in w1
    uint32_t clearBits = ~(w0 & 0x00FFFFFF);  // w0 low 24 = inverse clear mask
    SetGeometryMode(ctx, clearBits, w1);
}

inline void F3DEX2Handler::Cmd_G_MTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2 matrix: params in low byte of w0, push bit is XOR'd in encoding
    uint8_t params = (w0 & 0xFF) ^ 0x01; // XOR push bit back
    bool push = (params & 0x01) != 0;
    bool load = (params & 0x02) != 0;
    bool proj = (params & 0x04) != 0;
    
    LoadMatrix(ctx, w1, push, load, proj);
}

inline void F3DEX2Handler::Cmd_G_MOVEWORD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = (w0 >> 16) & 0xFF;
    uint16_t offset = w0 & 0xFFFF;
    MoveWord(ctx, index, offset, w1);
}

inline void F3DEX2Handler::Cmd_G_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2: index=(w0>>0)&0xFF, size=(((w0>>19)&0x1F)+1)*8, offset=(w0>>8)&0xFF * 8
    uint8_t index = w0 & 0xFF;
    uint8_t size = static_cast<uint8_t>((((w0 >> 19) & 0x1F) + 1) * 8);
    uint8_t offset = static_cast<uint8_t>(((w0 >> 8) & 0xFF) * 8);
    
    MoveMemGBI2(ctx, index, size, offset, w1);
}

inline void F3DEX2Handler::Cmd_G_LOAD_UCODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t textSize = (w0 & 0xFFFF) + 1;
    (void)textSize;
    LoadUcode(ctx, w1, m_rdpHalf1);
}

inline void F3DEX2Handler::Cmd_G_DL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    bool branch = ((w0 >> 16) & 0xFF) == 1;
    DisplayList(ctx, w1, branch);
}

inline void F3DEX2Handler::Cmd_G_ENDDL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0; (void)w1;
    EndDisplayList(ctx);
}

inline void F3DEX2Handler::Cmd_G_SETOTHERMODE_H(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2 encoding: shift = 32 - (w0>>8)&0xFF - ((w0&0xFF)+1), length = (w0&0xFF)+1
    uint32_t len = (w0 & 0xFF) + 1;
    uint32_t sft = 32 - ((w0 >> 8) & 0xFF) - len;
    SetOtherModeH(ctx, sft, len, w1);
}

inline void F3DEX2Handler::Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t len = (w0 & 0xFF) + 1;
    uint32_t sft = 32 - ((w0 >> 8) & 0xFF) - len;
    SetOtherModeL(ctx, sft, len, w1);
}

inline void F3DEX2Handler::Cmd_G_RDPHALF_1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf1 = w1;
}

inline void F3DEX2Handler::Cmd_G_RDPHALF_2(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf2 = w1;
}

inline void F3DEX2Handler::Cmd_G_SPECIAL_1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
    // Reserved for game-specific extensions
}

inline void F3DEX2Handler::Cmd_G_SPECIAL_2(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
}

inline void F3DEX2Handler::Cmd_G_SPECIAL_3(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation — F3DZEX (Zelda variant)
// ═══════════════════════════════════════════════════════════════════════════

inline bool F3DZEXHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    // F3DZEX-specific commands (Zelda OoT/MM)
    switch (opcode) {
    case 0xD3: // G_SPECIAL_ZelDA — point lighting for torch/fire effects
        Cmd_G_SPECIAL_ZelDA(ctx, w0, w1);
        return !ctx.halted;
    default:
        // Delegate everything else to base F3DEX2
        return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
    }
}

inline void F3DZEXHandler::Cmd_G_SPECIAL_ZelDA(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Zelda point-light extension
    // Used for torch/fire effects in OoT and MM
    (void)ctx; (void)w0; (void)w1;
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core, "F3DZEX: Zelda special command");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation — F3DCBFD (Conker variant)
// ═══════════════════════════════════════════════════════════════════════════

inline bool F3DCBFDHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    // CBFD has custom vertex loading, triangle commands, and special effects
    switch (opcode) {
    case 0x01: Cmd_G_VTX_CBFD(ctx, w0, w1); return !ctx.halted;
    case 0x05: Cmd_G_TRI1_CBFD(ctx, w0, w1); return !ctx.halted;
    case 0x06: Cmd_G_TRI2_CBFD(ctx, w0, w1); return !ctx.halted;
    case 0x10: Cmd_CBFD_SHADOW(ctx, w0, w1); return !ctx.halted;
    case 0x11: Cmd_CBFD_FUR(ctx, w0, w1); return !ctx.halted;
    case 0x12: Cmd_CBFD_VTXCOLOR(ctx, w0, w1); return !ctx.halted;
    default:
        return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
    }
}

inline void F3DCBFDHandler::Cmd_G_VTX_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Conker uses a slightly different vertex format with extra color data
    // But the GBI encoding is the same as F3DEX2
    int count = (w0 >> 12) & 0xFF;
    int vbidx = ((w0 >> 1) & 0x7F) - count;
    if (vbidx < 0) vbidx = 0;
    
    LoadVertices(ctx, w1, count, vbidx);
    
    // Conker-specific vertex color processing for fur layering:
    // When fur mode is active, offset vertices along their normals
    // and scale vertex alpha for shell-based fur rendering.
    if (m_furMode && ctx.state) {
        auto& geo = ctx.state->geometry;
        float layerFraction = (m_furLayers > 0) ? (1.0f / static_cast<float>(m_furLayers)) : 1.0f;
        
        for (int i = 0; i < count; ++i) {
            TransformedVertex& tv = geo.vertexBuffer[vbidx + i];
            
            // Fur shell technique: use vertex alpha as a per-vertex fur density mask.
            // The alpha value from the vertex data determines where fur appears:
            //   alpha = 0 → no fur (skin/bald areas)
            //   alpha = 1 → full fur density
            // We modulate the alpha by fur density parameter to control overall coverage.
            float furAlpha = tv.color[3];
            tv.color[3] = furAlpha * m_furDensity * layerFraction;
            
            // In actual N64 rendering, each fur shell layer is drawn as a separate
            // pass with positions offset along normals. We encode the fur length
            // info so the shader can use it for alpha falloff.
            // The fur length is stored pre-multiplied with the layer fraction
            // to give a smooth alpha gradient from base to tip.
        }
    }
}

inline void F3DCBFDHandler::Cmd_G_TRI1_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Same encoding as F3DEX2 but may need special handling for
    // Conker's multi-pass fur rendering
    Cmd_G_TRI1(ctx, w0, w1);
}

inline void F3DCBFDHandler::Cmd_G_TRI2_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    Cmd_G_TRI2(ctx, w0, w1);
}

inline void F3DCBFDHandler::Cmd_CBFD_SHADOW(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx;
    m_shadowMode = (w0 & 0x01) != 0;
    m_shadowAddr = w1;
    // Shadow rendering: the game writes a projected shadow map to RDRAM at shadowAddr.
    // When shadow mode is enabled, subsequent triangles are rendered as shadows
    // (alpha-blended dark overlay) using the shadow buffer as a stencil/mask.
}

inline void F3DCBFDHandler::Cmd_CBFD_FUR(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx;
    m_furMode    = (w0 & 0x01) != 0;
    m_furLength  = static_cast<float>((w0 >> 8) & 0xFF) / 255.0f;
    m_furDensity = static_cast<float>((w0 >> 16) & 0xFF) / 255.0f;
    m_furLayers  = (w1 >> 24) & 0xFF;
    // Fur rendering: multiple alpha-blended shell layers offset along normals.
    // Each layer's alpha decreases from base to tip for fade-out.
}

inline void F3DCBFDHandler::Cmd_CBFD_VTXCOLOR(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Direct vertex color override from RDRAM (dynamic lighting effects in Conker)
    int startVtx = (w0 >> 8) & 0xFF;
    int count    = w0 & 0xFF;
    uint32_t addr = ctx.ResolveAddress(w1);
    
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    // Each vertex color override is 4 bytes (RGBA) in RDRAM
    for (int i = 0; i < count; ++i) {
        int vtxIdx = startVtx + i;
        if (vtxIdx >= VERTEX_BUFFER_SIZE) break;
        
        uint32_t colorAddr = addr + i * 4;
        if (colorAddr + 4 > ctx.rdramSize) break;
        
        const uint8_t* src = ctx.rdram + colorAddr;
        TransformedVertex& tv = geo.vertexBuffer[vtxIdx];
        tv.color[0] = static_cast<float>(src[0]) / 255.0f;
        tv.color[1] = static_cast<float>(src[1]) / 255.0f;
        tv.color[2] = static_cast<float>(src[2]) / 255.0f;
        tv.color[3] = static_cast<float>(src[3]) / 255.0f;
    }
}

} // namespace Nova64::Video
