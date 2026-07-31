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
#include <atomic>

namespace Nova64::Video {

class F3DEX2Handler : public GBIHandler {
public:
    F3DEX2Handler() = default;
    ~F3DEX2Handler() override = default;
    
    void Initialize(const MicrocodeInfo& info) override;
    MicrocodeType GetType() const override { return m_type; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI2; }
    int GetVertexCacheSize() const override { return 32; } // F3DEX2 standard vertex cache is 32
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
    // CBFD-specific commands
    void Cmd_G_VTX_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_TRI4_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEWORD_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_MOVEMEM_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    void Cmd_G_LOAD_UCODE_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    // CBFD CoordMod: 16-element vertex position modifier for CBFD lighting
    void ProcessCoordMod(uint32_t w0, uint32_t w1);
    
    // CBFD state
    uint32_t m_vertexNormalBase = 0;  // RDRAM address of vertex normals table
    bool     m_advancedLighting = false;
    float    m_coordMod[16] = {};
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
    // F3DEX2 swaps the opcodes vs F3D: 0xE2=SetOtherMode_L, 0xE3=SetOtherMode_H
    case 0xE2: Cmd_G_SETOTHERMODE_L(ctx, w0, w1); break;
    case 0xE3: Cmd_G_SETOTHERMODE_H(ctx, w0, w1); break;
    
    case 0xF1: Cmd_G_RDPHALF_2(ctx, w0, w1); break;
    
    default:
        // Remaining 0xE0-0xFF are RDP commands
        if (opcode >= 0xE0) {
            HandleRDPCommand(ctx, w0, w1);
        } else {
            // Warn once per unique opcode without a mutex on the hot path.
            // A 256-bit bitset (one bit per opcode) is lock-free: test-and-set
            // with a relaxed CAS is cheaper than std::mutex + std::set insertion.
            static std::atomic<uint32_t> warnedLo{0}, warnedHi{0},
                                          warnedLo2{0}, warnedHi2{0};
            const uint8_t slot = opcode >> 5;   // 0-7
            const uint32_t bit  = 1u << (opcode & 31u);
            std::atomic<uint32_t>* buckets[8] = {
                &warnedLo, &warnedHi, &warnedLo2, &warnedHi2,
                &warnedLo, &warnedHi, &warnedLo2, &warnedHi2 // opcode <=255, 8 buckets
            };
            // Use the correct 32-bit bucket for this opcode
            std::atomic<uint32_t>* bkt;
            switch (slot) {
            case 0: bkt = &warnedLo;  break;
            case 1: bkt = &warnedHi;  break;
            case 2: bkt = &warnedLo2; break;
            default: bkt = &warnedHi2; break;
            }
            const uint32_t prev = bkt->fetch_or(bit, std::memory_order_relaxed);
            if (!(prev & bit)) {
                NOVA_LOG(LogLevel::Warning, LogSubsystem::Microcode,
                        "F3DEX2: Unknown opcode 0x{:02X} (w0=0x{:08X}, w1=0x{:08X})", opcode, w0, w1);
            }
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
    // GLideN64 reference (F3DEX_Branch_Z):
    //   vtx = _SHIFTR(w0, 1, 11), zval = _SHIFTR(w1, 16, 16)
    // Only the upper 16 bits of w1 contain the Z threshold.
    uint32_t zValue = (w1 >> 16) & 0xFFFF;
    BranchZ(ctx, vtxIndex, zValue, m_rdpHalf1);
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
    {
        static const bool s_tri2Dbg = std::getenv("NOVA64_REJECT_DBG") != nullptr;
        static int s_tri2Left = 60;
        if (s_tri2Dbg && s_tri2Left > 0 && (v3 == v5 || v3 == v4 || v4 == v5)) {
            --s_tri2Left;
            std::fprintf(stderr, "[TRI2RAW] w0=0x%08X w1=0x%08X -> t1=(%d,%d,%d) t2=(%d,%d,%d)\n",
                         w0, w1, v0, v1, v2, v3, v4, v5);
        }
    }
    DrawTriangle2(ctx, v0, v1, v2, v3, v4, v5);
}

inline void F3DEX2Handler::Cmd_G_QUAD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // GBI2 G_QUAD carries TWO full triangles, exactly like G_TRI2:
    //   w0 = [07][v00*2][v01*2][v02*2]      first  triangle
    //   w1 = [00][v10*2][v11*2][v12*2]      second triangle
    // gSP1Quadrangle(v0,v1,v2,v3) emits w1 = (v0*2)<<16|(v2*2)<<8|(v3*2):
    // the QUAD'S FOURTH VERTEX IS w1's LOW BYTE. The previous decode took
    // w1[23:16] as "v3" — but that byte holds the DUPLICATED v0 of the
    // shared edge — and dismissed the real low byte as padding. Every
    // quad's second half decoded as the degenerate (v0, v2, v0) and was
    // silently dropped, deleting half of every gSP1Quadrangle mesh: the
    // OoT/MM skyboxes and pre-rendered background meshes rendered as
    // alternating drawn/missing right triangles — the "shattered glass"
    // pattern. (GLideN64's F3DEX2_Quad likewise feeds both words through
    // the same two-triangle path as F3DEX2_Tri2.)
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int v1 = ((w0 >> 8) & 0xFF) / 2;
    int v2 = (w0 & 0xFF) / 2;
    int v3 = ((w1 >> 16) & 0xFF) / 2;
    int v4 = ((w1 >> 8) & 0xFF) / 2;
    int v5 = (w1 & 0xFF) / 2;
    DrawTriangle2(ctx, v0, v1, v2, v3, v4, v5);
}

inline void F3DEX2Handler::Cmd_G_LINE3D(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // G_LINE3D is not a renderable primitive in HLE — the N64 line rasterizer
    // uses a proprietary microcode path with no triangle equivalent.
    // Prior code emitted DrawTriangle(v0,v1,v0) which is silently culled by
    // the v0==v2 degenerate guard; making it an explicit no-op is cleaner.
    (void)ctx; (void)w0; (void)w1;
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
    LoadUcode(ctx, w1, m_rdpHalf1, textSize);
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
    int32_t sft = 32 - static_cast<int32_t>((w0 >> 8) & 0xFF) - static_cast<int32_t>(len);
    if (sft < 0) sft = 0;
    SetOtherModeH(ctx, static_cast<uint32_t>(sft), len, w1);
}

inline void F3DEX2Handler::Cmd_G_SETOTHERMODE_L(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint32_t len = (w0 & 0xFF) + 1;
    int32_t sft = 32 - static_cast<int32_t>((w0 >> 8) & 0xFF) - static_cast<int32_t>(len);
    if (sft < 0) sft = 0;
    SetOtherModeL(ctx, static_cast<uint32_t>(sft), len, w1);
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
    // Reserved for game-specific extensions.  GLideN64's real F3DEX2_Special_1
    // is NOT a no-op: it's gSPCombineMatrices (explicit projection*modelview
    // recombine) when the detected ucode string has combineMatrices=true, or
    // gSPDlistCount (jump to a sub-DL, auto-return after exactly N commands
    // via a per-command counter checked in the main RSP loop) when false --
    // which is the case for F3DZEX (Zelda OoT/MM).  Confirmed via a live
    // 12000-frame MM capture (title through early gameplay, ~15M RDP commands)
    // that this ROM never issues opcode 0xD5 at all in that content, so this
    // stays a no-op rather than adding an unconfirmed, architecturally
    // nontrivial (auto-return-after-N-commands) feature no observed content
    // exercises. Revisit if a specific missing-content symptom ever points here.
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
    case 0x04: // G_BRANCH_W — F3DZEX2 uses W-based LOD culling (not Z like F3DEX2)
    {
        // GLideN64 reference (F3DZEX2_Branch_W):
          //   vtx = _SHIFTR(w0, 1, 11)
          //   wval = w1 (full 32-bit integer)
          uint32_t vtxIndex = (w0 & 0xFFF) / 2;
        BranchW(ctx, vtxIndex, w1, m_rdpHalf1);
        return !ctx.halted;
    }
    case 0xD3: // G_SPECIAL_ZelDA (G_SPECIAL_3 slot)
        Cmd_G_SPECIAL_ZelDA(ctx, w0, w1);
        return !ctx.halted;
    default:
        // Delegate everything else to base F3DEX2
        return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
    }
}

inline void F3DZEXHandler::Cmd_G_SPECIAL_ZelDA(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Opcode 0xD3 = G_SPECIAL_3 slot. Verified against GLideN64's real
    // F3DEX2_Special_3: it's a genuine no-op there too (not a torch/fire
    // point-light extension as an earlier version of this comment claimed --
    // no such mechanism exists in GLideN64's F3DZEX/F3DEX2 handling of this
    // opcode). Kept as a no-op to match.
    (void)ctx; (void)w0; (void)w1;
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core, "F3DZEX: Zelda special command");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Implementation — F3DCBFD (Conker variant)
// ═══════════════════════════════════════════════════════════════════════════

inline bool F3DCBFDHandler::ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    // CBFD overrides: VTX, MoveWord, MoveMem, LoadUcode, and opcodes 0x10-0x1F are Tri4
    switch (opcode) {
    case 0x01: Cmd_G_VTX_CBFD(ctx, w0, w1); return !ctx.halted;
    case 0xDB: Cmd_G_MOVEWORD_CBFD(ctx, w0, w1); return !ctx.halted;
    case 0xDC: Cmd_G_MOVEMEM_CBFD(ctx, w0, w1); return !ctx.halted;
    case 0xAF: Cmd_G_LOAD_UCODE_CBFD(ctx, w0, w1); return !ctx.halted;
    default:
        // Opcodes 0x10-0x1F are all Tri4 in CBFD
        if (opcode >= 0x10 && opcode <= 0x1F) {
            Cmd_G_TRI4_CBFD(ctx, w0, w1);
            return !ctx.halted;
        }
        return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
    }
}

inline void F3DCBFDHandler::Cmd_G_VTX_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Same GBI encoding as F3DEX2 for count/vbidx
    int count = (w0 >> 12) & 0xFF;
    int vbidx = ((w0 >> 1) & 0x7F) - count;
    if (vbidx < 0) vbidx = 0;
    
    // Load standard vertex data (position, texcoords, color/normal bytes)
    LoadVertices(ctx, w1, count, vbidx);
    
    // CBFD-specific fixups:
    // 1) Bytes 12-15 are ALWAYS colors in CBFD (LoadVertices treats them as normals
    //    when lighting is on — we must fix that)
    // 2) When lighting is enabled, normals come from a separate RDRAM table at
    //    vertexNormalBase, NZ is in the vertex flag field (offset 6, low byte)
    if (ctx.state && ctx.rdram) {
        auto& geo = ctx.state->geometry;
        uint32_t resolvedAddr = ctx.ResolveAddress(w1);
        bool lighting = (geo.geometryMode & G_LIGHTING) != 0;
        
        for (int i = 0; i < count; ++i) {
            int vi = vbidx + i;
            if (vi >= VERTEX_BUFFER_SIZE) break;
            uint32_t vtxAddr = resolvedAddr + i * 16;
            if (vtxAddr + 16 > ctx.rdramSize) break;
            
            if (lighting) {
                // Colors: always read from vertex bytes 12-15 (LoadVertices
                // stored them as normals, overwrite with correct color values)
                uint8_t r = ctx.ReadByte(vtxAddr + 12);
                uint8_t g = ctx.ReadByte(vtxAddr + 13);
                uint8_t b = ctx.ReadByte(vtxAddr + 14);
                uint8_t a = ctx.ReadByte(vtxAddr + 15);
                geo.vertexBuffer[vi].color[0] = static_cast<float>(r) / 255.0f;
                geo.vertexBuffer[vi].color[1] = static_cast<float>(g) / 255.0f;
                geo.vertexBuffer[vi].color[2] = static_cast<float>(b) / 255.0f;
                geo.vertexBuffer[vi].color[3] = static_cast<float>(a) / 255.0f;
                
                // Normals: from separate RDRAM table
                if (m_vertexNormalBase != 0) {
                    uint32_t normAddr = m_vertexNormalBase + static_cast<uint32_t>(vi << 1);
                    if (normAddr + 2 <= ctx.rdramSize) {
                        int8_t nx = static_cast<int8_t>(ctx.rdram[normAddr ^ 3]);
                        int8_t ny = static_cast<int8_t>(ctx.rdram[(normAddr + 1) ^ 3]);
                        // NZ from vertex flag field (offset 6, low byte)
                        int8_t nz = static_cast<int8_t>(ctx.ReadHalf(vtxAddr + 6) & 0xFF);
                        
                        geo.vertexBuffer[vi].normal[0] = static_cast<float>(nx) / 127.0f;
                        geo.vertexBuffer[vi].normal[1] = static_cast<float>(ny) / 127.0f;
                        geo.vertexBuffer[vi].normal[2] = static_cast<float>(nz) / 127.0f;
                    }
                }
            }
        }
        
        // ---- CBFD Lighting ----
        // Standard lighting was already applied by TransformVertexBatch but used
        // wrong normals (bytes 12-15 are colors in CBFD, not normals).  The fixup
        // above overwrote the lit colors with the raw vertex RGBA and loaded the
        // correct normals from the external table.  Now apply CBFD-specific
        // position-based lighting, multiplying onto the raw vertex colors.
        if (lighting) {
            constexpr float FIXED2FLOATRECIP16 = 1.0f / 65536.0f;
            const int numLights = geo.numLights;
            
            for (int i = 0; i < count; ++i) {
                int vi = vbidx + i;
                if (vi >= VERTEX_BUFFER_SIZE) break;
                auto& tv = geo.vertexBuffer[vi];
                uint32_t vtxAddr = resolvedAddr + i * 16;
                if (vtxAddr + 16 > ctx.rdramSize) break;
                
                // Skip vertices with negative flag (matches GLideN64)
                int16_t flag = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 6));
                if (flag < 0) continue;
                
                // Re-read raw object-space position from RDRAM
                // (TransformVertexBatch overwrote position[] with clip coords)
                float objX = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 0)));
                float objY = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 2)));
                float objZ = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 4)));
                
                // CBFD coordinate-modified position
                float vPosX = (objX + m_coordMod[8])  * m_coordMod[12];
                float vPosY = (objY + m_coordMod[9])  * m_coordMod[13];
                float vPosZ = (objZ + m_coordMod[10]) * m_coordMod[14];
                
                // Start with ambient light
                float r = geo.lights[numLights].color[0];
                float g = geo.lights[numLights].color[1];
                float b = geo.lights[numLights].color[2];
                
                if (m_advancedLighting) {
                    // Top light (numLights-1) is directional: dot(normal, lightDir)
                    int topLight = numLights - 1;
                    if (topLight >= 0) {
                        float intensity = std::min(1.0f,
                            tv.normal[0] * geo.lights[topLight].direction[0] +
                            tv.normal[1] * geo.lights[topLight].direction[1] +
                            tv.normal[2] * geo.lights[topLight].direction[2]);
                        if (intensity > 0.0f) {
                            r += geo.lights[topLight].color[0] * intensity;
                            g += geo.lights[topLight].color[1] * intensity;
                            b += geo.lights[topLight].color[2] * intensity;
                        }
                    }
                    // Remaining lights (0..numLights-2) are distance-attenuated
                    for (int l = numLights - 2; l >= 0; --l) {
                        float vx = vPosX - geo.lights[l].position[0];
                        float vy = vPosY - geo.lights[l].position[1];
                        float vz = vPosZ - geo.lights[l].position[2];
                        float len = 2.0f * (vx*vx + vy*vy + vz*vz) * FIXED2FLOATRECIP16;
                        float intensity = std::min(1.0f, geo.lights[l].ca / len);
                        if ((geo.geometryMode & G_POINT_LIGHTING) != 0) {
                            intensity *= std::min(1.0f,
                                tv.normal[0] * geo.lights[l].direction[0] +
                                tv.normal[1] * geo.lights[l].direction[1] +
                                tv.normal[2] * geo.lights[l].direction[2]);
                        }
                        if (intensity > 0.0f) {
                            r += geo.lights[l].color[0] * intensity;
                            g += geo.lights[l].color[1] * intensity;
                            b += geo.lights[l].color[2] * intensity;
                        }
                    }
                } else {
                    // Basic: all non-ambient lights (0..numLights-2) distance-attenuated
                    for (int l = numLights - 2; l >= 0; --l) {
                        float vx = vPosX - geo.lights[l].position[0];
                        float vy = vPosY - geo.lights[l].position[1];
                        float vz = vPosZ - geo.lights[l].position[2];
                        float len = 2.0f * (vx*vx + vy*vy + vz*vz) * FIXED2FLOATRECIP16;
                        float intensity = std::min(1.0f, geo.lights[l].ca / len);
                        if (intensity > 0.0f) {
                            r += geo.lights[l].color[0] * intensity;
                            g += geo.lights[l].color[1] * intensity;
                            b += geo.lights[l].color[2] * intensity;
                        }
                    }
                }
                
                r = std::min(1.0f, r);
                g = std::min(1.0f, g);
                b = std::min(1.0f, b);
                
                tv.color[0] *= r;
                tv.color[1] *= g;
                tv.color[2] *= b;
            }
        }
    }
}

inline void F3DCBFDHandler::Cmd_G_TRI4_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    // Tri4: draws 4 triangles packed into two 32-bit words
    // Vertex indices are 5-bit, packed across w0 and w1
    int v0  = (w0 >> 23) & 0x1F;
    int v1  = (w0 >> 18) & 0x1F;
    int v2a = (w0 >> 15) & 0x07;  // upper 3 bits of v2
    int v2b = (w1 >> 30) & 0x03;  // lower 2 bits of v2
    int v2  = (v2a << 2) | v2b;
    
    int v3  = (w0 >> 10) & 0x1F;
    int v4  = (w0 >>  5) & 0x1F;
    int v5  = (w0 >>  0) & 0x1F;
    
    int v6  = (w1 >> 25) & 0x1F;
    int v7  = (w1 >> 20) & 0x1F;
    int v8  = (w1 >> 15) & 0x1F;
    
    int v9  = (w1 >> 10) & 0x1F;
    int v10 = (w1 >>  5) & 0x1F;
    int v11 = (w1 >>  0) & 0x1F;
    
    DrawTriangle(ctx, v0, v1, v2);
    DrawTriangle(ctx, v3, v4, v5);
    DrawTriangle(ctx, v6, v7, v8);
    DrawTriangle(ctx, v9, v10, v11);
}

inline void F3DCBFDHandler::Cmd_G_MOVEWORD_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = (w0 >> 16) & 0xFF;
    
    // CBFD-specific MoveWord indices
    constexpr uint8_t G_MW_COORD_MOD = 0x10;  // CBFD coordinate modifier
    
    if (index == MoveWord::MW_NUMLIGHT) {
        // CBFD uses 48 bytes per light instead of 32
        if (!ctx.state) return;
        int numLights = static_cast<int>(w1 / 48);
        if (numLights >= MAX_LIGHTS) numLights = MAX_LIGHTS - 1;
        ctx.state->geometry.numLights = numLights;
        ctx.state->MarkDirty(RDPDirtyFlag::Lights);
    } else if (index == G_MW_COORD_MOD) {
        ProcessCoordMod(w0, w1);
    } else {
        // Fall through to standard F3DEX2 MoveWord handling
        uint16_t offset = w0 & 0xFFFF;
        MoveWord(ctx, index, offset, w1);
    }
}

inline void F3DCBFDHandler::Cmd_G_MOVEMEM_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    uint8_t index = w0 & 0xFF;
    
    constexpr uint8_t G_MV_NORMALES = 0x0E;  // CBFD normals table (14)
    constexpr uint8_t G_MV_LIGHT = 0x0A;    // Light (10)
    
    if (index == G_MV_NORMALES) {
        // Set the RDRAM address of the vertex normals table
        m_vertexNormalBase = ctx.ResolveAddress(w1);
    } else if (index == G_MV_LIGHT) {
        uint32_t offset = (w0 >> 5) & 0x3FFF;
        uint32_t n = offset / 48;  // CBFD: 48 bytes per light
        if (n < 2) {
            // LookAt vectors (same as F3DEX2)
            MoveMemGBI2(ctx, index, 0, static_cast<uint8_t>(offset), w1);
        } else {
            // CBFD light: 48 bytes (color, direction, position, attenuation)
            uint32_t lightIndex = n - 2;
            if (!ctx.state || !ctx.rdram) return;
            auto& geo = ctx.state->geometry;
            uint32_t addr = ctx.ResolveAddress(w1);
            if (addr + 48 > ctx.rdramSize) return;
            
            if (lightIndex < MAX_LIGHTS) {
                const uint8_t* src = ctx.rdram + addr;
                auto& light = geo.lights[lightIndex];
                
                // Color (bytes 0-2)
                light.r = src[0 ^ 3];
                light.g = src[1 ^ 3];
                light.b = src[2 ^ 3];
                light.color[0] = static_cast<float>(light.r) / 255.0f;
                light.color[1] = static_cast<float>(light.g) / 255.0f;
                light.color[2] = static_cast<float>(light.b) / 255.0f;
                
                // Direction (bytes 8-10), then normalize
                float dx = static_cast<float>(static_cast<int8_t>(src[8 ^ 3]));
                float dy = static_cast<float>(static_cast<int8_t>(src[9 ^ 3]));
                float dz = static_cast<float>(static_cast<int8_t>(src[10 ^ 3]));
                float len = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (len > 0.0001f) {
                    float inv = 1.0f / len;
                    dx *= inv; dy *= inv; dz *= inv;
                }
                light.direction[0] = dx;
                light.direction[1] = dy;
                light.direction[2] = dz;
                
                // Ca attenuation (byte 12)
                light.ca = static_cast<float>(src[12 ^ 3]) / 16.0f;
                
                // Position (bytes 32-37 as int16_t, using halfword swap)
                light.position[0] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(addr + 32)));
                light.position[1] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(addr + 34)));
                light.position[2] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(addr + 36)));
                
                ctx.state->MarkDirty(RDPDirtyFlag::Lights);
            }
        }
    } else {
        // Fall through to standard F3DEX2 MoveMem
        uint8_t size = (((w0 >> 19) & 0x1F) + 1) * 8;
        uint8_t offset_val = (w0 >> 8) & 0xFF;
        MoveMemGBI2(ctx, index, size, offset_val, w1);
    }
}

inline void F3DCBFDHandler::Cmd_G_LOAD_UCODE_CBFD(DListContext& ctx, uint32_t w0, uint32_t w1) {
    (void)ctx; (void)w0; (void)w1;
    // In CBFD, G_LOAD_UCODE is repurposed to switch to advanced lighting mode
    m_advancedLighting = true;
}

inline void F3DCBFDHandler::ProcessCoordMod(uint32_t w0, uint32_t w1) {
    // CBFD coordinate modifier: 16-element position modifier array for CBFD lighting
    if ((w0 & 8) != 0) return;  // Bit 3 set = skip
    
    uint32_t idx = (w0 >> 1) & 0x03;
    uint32_t pos = w0 & 0x30;
    
    if (pos == 0x00) {
        m_coordMod[0 + idx] = static_cast<float>(static_cast<int16_t>((w1 >> 16) & 0xFFFF));
        m_coordMod[1 + idx] = static_cast<float>(static_cast<int16_t>(w1 & 0xFFFF));
    } else if (pos == 0x10) {
        float fracHi = static_cast<float>(static_cast<int16_t>((w1 >> 16) & 0xFFFF)) / 65536.0f;
        float fracLo = static_cast<float>(static_cast<int16_t>(w1 & 0xFFFF)) / 65536.0f;
        m_coordMod[4 + idx] = fracHi;
        m_coordMod[5 + idx] = fracLo;
        m_coordMod[12 + idx] = m_coordMod[0 + idx] + m_coordMod[4 + idx];
        m_coordMod[13 + idx] = m_coordMod[1 + idx] + m_coordMod[5 + idx];
    } else if (pos == 0x20) {
        m_coordMod[8 + idx] = static_cast<float>(static_cast<int16_t>((w1 >> 16) & 0xFFFF));
        m_coordMod[9 + idx] = static_cast<float>(static_cast<int16_t>(w1 & 0xFFFF));
    }
}

} // namespace Nova64::Video
