/**
 * @file F3DEX.hpp
 * @brief F3DEX microcode handler
 *
 * F3DEX extended F3D with a larger 32-vertex buffer, 2-triangle commands
 * (G_TRI2), vertex modification (G_MODIFYVTX), branch-on-Z (G_BRANCH_Z),
 * cull optimization (G_CULLDL), and dynamic microcode loading.
 * Still uses GBI1 command encoding.
 */

#pragma once

#include "GBIHandler.hpp"

namespace Nova64::Video {

class F3DEXHandler : public GBIHandler {
public:
    F3DEXHandler() = default;
    ~F3DEXHandler() override = default;
    
    void Initialize(const MicrocodeInfo& info) override;
    MicrocodeType GetType() const override { return MicrocodeType::F3DEX; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "F3DEX"; }
    
    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override;
    
protected:
    // ─── F3DEX command handlers (extending F3D) ──────────────────────
    void Cmd_G_VTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_DL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI2(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_QUAD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_LINE3D(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MODIFYVTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_BRANCH_Z(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CULLDL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_LOAD_UCODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEWORD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_H(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_ENDDL(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_SETGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_CLEARGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_POPMTX(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_1(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_RDPHALF_2(DListContext& ctx, uint32_t w0, uint32_t w1);
};

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation
// ═══════════════════════════════════════════════════════════════════════════

inline void F3DEXHandler::Initialize(const MicrocodeInfo& info) {
    (void)info;
    ResetStats();
}

inline bool F3DEXHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    if (opcode >= 0xC0) {
        HandleRDPCommand(ctx, w0, w1);
        return true;
    }
    
    switch (opcode) {
    case 0x00: break; // G_SPNOOP
    case 0x01: Cmd_G_MTX(ctx, w0, w1); break;
    case 0x03: Cmd_G_MOVEMEM(ctx, w0, w1); break;
    case 0x04: Cmd_G_VTX(ctx, w0, w1); break;
    case 0x06: Cmd_G_DL(ctx, w0, w1); break;
    
    case 0xAF: Cmd_G_LOAD_UCODE(ctx, w0, w1); break;
    case 0xB0: Cmd_G_BRANCH_Z(ctx, w0, w1); break;
    case 0xB1: Cmd_G_TRI2(ctx, w0, w1); break;
    case 0xB2: Cmd_G_MODIFYVTX(ctx, w0, w1); break;
    case 0xB3: Cmd_G_RDPHALF_2(ctx, w0, w1); break;
    case 0xB4: Cmd_G_RDPHALF_1(ctx, w0, w1); break;
    case 0xB5: Cmd_G_QUAD(ctx, w0, w1); break;
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
                "F3DEX: Unknown opcode 0x{:02X}", opcode);
        break;
    }
    
    return !ctx.halted;
}

inline void F3DEXHandler::Cmd_G_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // F3DEX: count = ((w0 >> 10) & 0x3F), v0 = ((w0 >> 17) & 0x7F)
    int count = (w0 >> 10) & 0x3F;
    int startIndex = ((w0 >> 17) & 0x7F);
    
    // The start index can exceed the count by the buffer offset
    // F3DEX stores: v0 = (last_vtx + 1 - count)
    startIndex = startIndex + 1 - count;
    if (startIndex < 0) startIndex = 0;
    
    LoadVertices(ctx, w1, count, startIndex);
}

inline void F3DEXHandler::Cmd_G_MTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t params = w0 & 0x00FFFFFF;
    bool push = (params & 0x01) == 0;
    bool load = (params & 0x02) != 0;
    bool proj = (params & 0x04) != 0;
    LoadMatrix(ctx, w1, push, load, proj);
}

inline void F3DEXHandler::Cmd_G_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = (w0 >> 16) & 0xFF;
    uint8_t size = ((w0 >> 19) & 0x1F) * 8;
    MoveMemGBI1(ctx, index, size, w1);
}

inline void F3DEXHandler::Cmd_G_DL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    bool branch = ((w0 >> 16) & 0xFF) == 1;
    DisplayList(ctx, w1, branch);
}

inline void F3DEXHandler::Cmd_G_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    int v0 = ((w1 >> 16) & 0xFF) / 2;
    int v1 = ((w1 >> 8) & 0xFF) / 2;
    int v2 = (w1 & 0xFF) / 2;
    DrawTriangle(ctx, v0, v1, v2);
}

inline void F3DEXHandler::Cmd_G_TRI2(DListContext& ctx, uint32_t w0, uint32_t w1) {
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    int v3 = ((w1 >> 16) & 0xFF) / 2;
    int v4 = ((w1 >> 8) & 0xFF) / 2;
    int v5 = (w1 & 0xFF) / 2;
    DrawTriangle2(ctx, v0, v1, v2, v3, v4, v5);
}

inline void F3DEXHandler::Cmd_G_QUAD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Quad = 2 triangles sharing an edge
    int v0 = ((w1 >> 24) & 0xFF) / 2;
    int v1 = ((w1 >> 16) & 0xFF) / 2;
    int v2 = ((w1 >> 8) & 0xFF) / 2;
    int v3 = (w1 & 0xFF) / 2;
    (void)w0;
    DrawTriangle(ctx, v0, v1, v2);
    DrawTriangle(ctx, v0, v2, v3);
}

inline void F3DEXHandler::Cmd_G_LINE3D(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Line rendering — draw as degenerate triangle for now
    (void)w0;
    int v0 = ((w1 >> 16) & 0xFF) / 2;
    int v1 = ((w1 >> 8) & 0xFF) / 2;
    // For line rendering, we'd need a special draw path
    // Approximate with a very thin triangle
    DrawTriangle(ctx, v0, v1, v0);
}

inline void F3DEXHandler::Cmd_G_MODIFYVTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t where = (w0 >> 16) & 0xFF;
    uint16_t vtxIndex = ((w0 & 0xFFFF) / 2);
    ModifyVertex(ctx, where, vtxIndex, w1);
}

inline void F3DEXHandler::Cmd_G_BRANCH_Z(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t vtxIndex = ((w0 & 0xFFF) / 2);
    BranchZ(ctx, vtxIndex, w1, m_rdpHalf1);
}

inline void F3DEXHandler::Cmd_G_CULLDL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t start = ((w0 & 0xFFF) / 2);
    uint32_t end = ((w1 & 0xFFF) / 2);
    CullDisplayList(ctx, start, end);
}

inline void F3DEXHandler::Cmd_G_LOAD_UCODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t textSize = (w0 & 0xFFFF) + 1;
    (void)textSize;
    LoadUcode(ctx, w1, m_rdpHalf1);
}

inline void F3DEXHandler::Cmd_G_MOVEWORD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = (w0 >> 16) & 0xFF;
    uint16_t offset = w0 & 0xFFFF;
    MoveWord(ctx, index, offset, w1);
}

inline void F3DEXHandler::Cmd_G_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    SetTextureState(ctx, w0, w1);
}

inline void F3DEXHandler::Cmd_G_SETOTHERMODE_H(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t shift = (w0 >> 8) & 0xFF;
    uint32_t length = (w0 & 0xFF);  // GBI1: raw byte IS the length (no -1 encoding)
    SetOtherModeH(ctx, shift, length, w1);
}

inline void F3DEXHandler::Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t shift = (w0 >> 8) & 0xFF;
    uint32_t length = (w0 & 0xFF);  // GBI1: raw byte IS the length (no -1 encoding)
    SetOtherModeL(ctx, shift, length, w1);
}

inline void F3DEXHandler::Cmd_G_ENDDL(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0; (void)w1;
    EndDisplayList(ctx);
}

/// Translate GBI1 geometry mode bits to GBI2 (internal) format.
static inline uint32_t TranslateGBI1ToGBI2_F3DEX(uint32_t gbi1) {
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

inline void F3DEXHandler::Cmd_G_SETGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w1;
    SetGeometryMode(ctx, 0, TranslateGBI1ToGBI2_F3DEX(w0 & 0x00FFFFFF));
}

inline void F3DEXHandler::Cmd_G_CLEARGEOMETRYMODE(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w1;
    SetGeometryMode(ctx, TranslateGBI1ToGBI2_F3DEX(w0 & 0x00FFFFFF), 0);
}

inline void F3DEXHandler::Cmd_G_POPMTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)w0;
    PopMatrix(ctx, w1);
}

inline void F3DEXHandler::Cmd_G_RDPHALF_1(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf1 = w1;
}

inline void F3DEXHandler::Cmd_G_RDPHALF_2(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0;
    m_rdpHalf2 = w1;
}

} // namespace Nova64::Video
