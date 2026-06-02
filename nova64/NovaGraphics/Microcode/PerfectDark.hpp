/**
 * @file PerfectDark.hpp
 * @brief Perfect Dark (F3DPD) custom microcode handler
 *
 * Perfect Dark uses a heavily modified F3D variant with:
 * - 80 vertex buffer (vs 16 or 32 in standard microcodes)
 * - Custom vertex loading with packed color/normal data
 * - Extended geometry mode flags for PD-specific effects
 * - Custom matrix operations for multiple player views
 * - GBI1 command encoding base
 */

#pragma once

#include "GBIHandler.hpp"

namespace Nova64::Video {

class F3DPDHandler : public GBIHandler {
public:
    F3DPDHandler() = default;
    ~F3DPDHandler() override = default;
    
    void Initialize(const MicrocodeInfo& info) override;
    MicrocodeType GetType() const override { return MicrocodeType::F3DPD; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    int GetVertexCacheSize() const override { return 80; } // PD handles extended vertex buffer size up to 80
    const char* GetName() const override { return "F3DPD (Perfect Dark)"; }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    void OnFrameBegin(DListContext& ctx) override;
    
private:
    // ─── PD-specific command handlers ────────────────────────────────
    void Cmd_G_VTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_VTX_COLOR_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEMEM_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI1_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI2_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI4_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_DL_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEWORD_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TEXTURE_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_H_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_L_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_ENDDL_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETGEOMETRYMODE_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CLEARGEOMETRYMODE_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_POPMTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CULLDL_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_1_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_2_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_LOAD_UCODE_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_BRANCH_Z_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MODIFYVTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    // PD stores vertex colors separately from positions
    struct PDVertexColor {
        uint8_t r, g, b, a;
    };
    std::array<PDVertexColor, 80> m_vtxColors;
    uint32_t m_vtxColorAddress = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation
// ═══════════════════════════════════════════════════════════════════════════

inline void F3DPDHandler::Initialize(const MicrocodeInfo& info) {
    (void)info;
    m_vtxColors.fill({ 255, 255, 255, 255 });
    m_vtxColorAddress = 0;
    ResetStats();
}

inline void F3DPDHandler::OnFrameBegin(DListContext& ctx) {
    (void)ctx;
    m_vtxColorAddress = 0;
}

inline bool F3DPDHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    if (opcode >= 0xC0) {
        HandleRDPCommand(ctx, w0, w1);
        return true;
    }
    
    switch (opcode) {
    case 0x00: break; // NOOP
    case 0x01: Cmd_G_MTX_PD(ctx, w0, w1); break;
    case 0x03: Cmd_G_MOVEMEM_PD(ctx, w0, w1); break;
    case 0x04: Cmd_G_VTX_PD(ctx, w0, w1); break;
    case 0x06: Cmd_G_DL_PD(ctx, w0, w1); break;
    
    // PD-specific: vertex color load (unique command)
    case 0x07: Cmd_G_VTX_COLOR_PD(ctx, w0, w1); break;
    
    // PD uses a special TRI4 that packs 4 triangles into 3 words
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        Cmd_G_TRI4_PD(ctx, w0, w1);
        break;
    
    case 0xAF: Cmd_G_LOAD_UCODE_PD(ctx, w0, w1); break;
    case 0xB0: Cmd_G_BRANCH_Z_PD(ctx, w0, w1); break;
    case 0xB1: Cmd_G_TRI2_PD(ctx, w0, w1); break;
    case 0xB2: Cmd_G_MODIFYVTX_PD(ctx, w0, w1); break;
    case 0xB3: Cmd_G_RDPHALF_2_PD(ctx, w0, w1); break;
    case 0xB4: Cmd_G_RDPHALF_1_PD(ctx, w0, w1); break;
    case 0xB6: Cmd_G_CLEARGEOMETRYMODE_PD(ctx, w0, w1); break;
    case 0xB7: Cmd_G_SETGEOMETRYMODE_PD(ctx, w0, w1); break;
    case 0xB8: Cmd_G_ENDDL_PD(ctx, w0, w1); break;
    case 0xB9: Cmd_G_SETOTHERMODE_L_PD(ctx, w0, w1); break;
    case 0xBA: Cmd_G_SETOTHERMODE_H_PD(ctx, w0, w1); break;
    case 0xBB: Cmd_G_TEXTURE_PD(ctx, w0, w1); break;
    case 0xBC: Cmd_G_MOVEWORD_PD(ctx, w0, w1); break;
    case 0xBD: Cmd_G_POPMTX_PD(ctx, w0, w1); break;
    case 0xBE: Cmd_G_CULLDL_PD(ctx, w0, w1); break;
    case 0xBF: Cmd_G_TRI1_PD(ctx, w0, w1); break;
    
    default:
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "F3DPD: Unknown opcode 0x{:02X}", opcode);
        break;
    }
    
    return !ctx.halted;
}

// ─── Perfect Dark Vertex Loading ─────────────────────────────────────────

inline void F3DPDHandler::Cmd_G_VTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // PD vertex: count in bits [20:10], start in bits [9:0]
    int count = (w0 >> 10) & 0x3F;
    int startIndex = w0 & 0x3FF;
    
    // PD has 80 vertex buffer
    if (startIndex + count > 80) {
        count = 80 - startIndex;
    }
    
    LoadVertices(ctx, w1, count, startIndex);
    
    // Apply separately stored vertex colors
    if (ctx.state && m_vtxColorAddress != 0) {
        auto& geo = ctx.state->geometry;
        for (int i = 0; i < count; ++i) {
            int vi = startIndex + i;
            if (vi < 80) {
                geo.vertexBuffer[vi].color[0] = static_cast<float>(m_vtxColors[vi].r) / 255.0f;
                geo.vertexBuffer[vi].color[1] = static_cast<float>(m_vtxColors[vi].g) / 255.0f;
                geo.vertexBuffer[vi].color[2] = static_cast<float>(m_vtxColors[vi].b) / 255.0f;
                geo.vertexBuffer[vi].color[3] = static_cast<float>(m_vtxColors[vi].a) / 255.0f;
            }
        }
    }
}

inline void F3DPDHandler::Cmd_G_VTX_COLOR_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // PD stores vertex colors in a separate array from vertex positions
    int start = (w0 >> 16) & 0xFF;
    int count = (w0 >> 10) & 0x3F;
    uint32_t address = ctx.ResolveAddress(w1);
    
    m_vtxColorAddress = address;
    
    for (int i = 0; i < count && (start + i) < 80; ++i) {
        uint32_t colorAddr = address + i * 4;
        if (colorAddr + 4 > ctx.rdramSize) break;
        
        m_vtxColors[start + i].r = ctx.rdram[colorAddr + 0];
        m_vtxColors[start + i].g = ctx.rdram[colorAddr + 1];
        m_vtxColors[start + i].b = ctx.rdram[colorAddr + 2];
        m_vtxColors[start + i].a = ctx.rdram[colorAddr + 3];
    }
}

inline void F3DPDHandler::Cmd_G_MTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t params = w0 & 0x00FFFFFF;
    bool push = (params & 0x01) == 0;
    bool load = (params & 0x02) != 0;
    bool proj = (params & 0x04) != 0;
    LoadMatrix(ctx, w1, push, load, proj);
}

inline void F3DPDHandler::Cmd_G_MOVEMEM_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI1: no size field — payload size determined by index in MoveMemGBI1
    uint8_t index = (w0 >> 16) & 0xFF;
    MoveMemGBI1(ctx, index, 0, w1);
}

inline void F3DPDHandler::Cmd_G_TRI1_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    int v0 = ((w1 >> 16) & 0xFF) / 2;
    int v1 = ((w1 >> 8) & 0xFF) / 2;
    int v2 = (w1 & 0xFF) / 2;
    DrawTriangle(ctx, v0, v1, v2);
}

inline void F3DPDHandler::Cmd_G_TRI2_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    int v3 = ((w1 >> 16) & 0xFF) / 2;
    int v4 = ((w1 >> 8) & 0xFF) / 2;
    int v5 = (w1 & 0xFF) / 2;
    DrawTriangle2(ctx, v0, v1, v2, v3, v4, v5);
}

inline void F3DPDHandler::Cmd_G_TRI4_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // PD's unique TRI4 command can encode up to 4 triangles compactly
    // Opcodes 0x10-0x1F encode the triangle count in low nibble
    // Triangle vertices are packed across w0 and w1
    uint8_t flags = w0 >> 24;
    int numTris = flags & 0x0F;
    if (numTris == 0) numTris = 1;
    
    // First triangle always present
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    DrawTriangle(ctx, v0, v1, v2);
    
    if (numTris >= 2) {
        int v3 = ((w1 >> 16) & 0xFF) / 2;
        int v4 = ((w1 >> 8) & 0xFF) / 2;
        int v5 = (w1 & 0xFF) / 2;
        DrawTriangle(ctx, v3, v4, v5);
    }
    
    // For 3-4 triangles, need to read additional words (1 full GBI command)
    if (numTris >= 3) {
        uint32_t w2 = ctx.ReadWord(ctx.pc);
        uint32_t w3 = ctx.ReadWord(ctx.pc + 4);
        ctx.AdvancePC(2); // Always advance by 8 bytes to maintain GBI alignment
        
        int v6 = ((w2 >> 16) & 0xFF) / 2;
        int v7 = ((w2 >> 8) & 0xFF) / 2;
        int v8 = (w2 & 0xFF) / 2;
        DrawTriangle(ctx, v6, v7, v8);
        
        if (numTris >= 4) {
            int v9  = ((w2 >> 24) & 0xFF) / 2;  // High byte encodes 4th tri start
            int v10 = ((w3 >> 16) & 0xFF) / 2;
            int v11 = ((w3 >> 8) & 0xFF) / 2;
            DrawTriangle(ctx, v9, v10, v11);
        }
    }
}

inline void F3DPDHandler::Cmd_G_DL_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    bool branch = ((w0 >> 16) & 0xFF) == 1;
    DisplayList(ctx, w1, branch);
}

inline void F3DPDHandler::Cmd_G_MOVEWORD_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = (w0 >> 16) & 0xFF;
    uint16_t offset = w0 & 0xFFFF;
    MoveWord(ctx, index, offset, w1);
}

inline void F3DPDHandler::Cmd_G_TEXTURE_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    SetTextureState(ctx, w0, w1);
}

inline void F3DPDHandler::Cmd_G_SETOTHERMODE_H_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t shift = (w0 >> 8) & 0xFF;
    uint32_t length = (w0 & 0xFF) + 1;
    SetOtherModeH(ctx, shift, length, w1);
}

inline void F3DPDHandler::Cmd_G_SETOTHERMODE_L_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t shift = (w0 >> 8) & 0xFF;
    uint32_t length = (w0 & 0xFF) + 1;
    SetOtherModeL(ctx, shift, length, w1);
}

inline void F3DPDHandler::Cmd_G_ENDDL_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0; (void)w1;
    EndDisplayList(ctx);
}

/// Translate GBI1 geometry mode bits to GBI2 (internal) format.
static inline uint32_t TranslateGBI1ToGBI2_PD(uint32_t gbi1) {
    constexpr uint32_t GBI1_SHADING_SMOOTH = 0x00000200;
    constexpr uint32_t GBI1_CULL_FRONT     = 0x00001000;
    constexpr uint32_t GBI1_CULL_BACK      = 0x00002000;
    constexpr uint32_t GBI1_MASK = GBI1_SHADING_SMOOTH | GBI1_CULL_FRONT | GBI1_CULL_BACK;
    uint32_t result = gbi1 & ~GBI1_MASK;
    if (gbi1 & GBI1_SHADING_SMOOTH) result |= 0x00200000;
    if (gbi1 & GBI1_CULL_FRONT)     result |= 0x00000200;
    if (gbi1 & GBI1_CULL_BACK)      result |= 0x00000400;
    return result;
}

inline void F3DPDHandler::Cmd_G_SETGEOMETRYMODE_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w1;
    SetGeometryMode(ctx, 0, TranslateGBI1ToGBI2_PD(w0 & 0x00FFFFFF));
}

inline void F3DPDHandler::Cmd_G_CLEARGEOMETRYMODE_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w1;
    SetGeometryMode(ctx, TranslateGBI1ToGBI2_PD(w0 & 0x00FFFFFF), 0);
}

inline void F3DPDHandler::Cmd_G_POPMTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    PopMatrix(ctx, w1);
}

inline void F3DPDHandler::Cmd_G_CULLDL_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t start = ((w0 & 0xFFF) / 2);
    uint32_t end = ((w1 & 0xFFF) / 2);
    CullDisplayList(ctx, start, end);
}

inline void F3DPDHandler::Cmd_G_RDPHALF_1_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf1 = w1;
}

inline void F3DPDHandler::Cmd_G_RDPHALF_2_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf2 = w1;
}

inline void F3DPDHandler::Cmd_G_LOAD_UCODE_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t textSize = (w0 & 0xFFFF) + 1;
    LoadUcode(ctx, w1, m_rdpHalf1, textSize);
}

inline void F3DPDHandler::Cmd_G_BRANCH_Z_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t vtxIndex = ((w0 & 0xFFF) / 2);
    BranchZ(ctx, vtxIndex, w1, m_rdpHalf1);
}

inline void F3DPDHandler::Cmd_G_MODIFYVTX_PD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t where = (w0 >> 16) & 0xFF;
    uint16_t vtxIndex = ((w0 & 0xFFFF) / 2);
    ModifyVertex(ctx, where, vtxIndex, w1);
}

} // namespace Nova64::Video
