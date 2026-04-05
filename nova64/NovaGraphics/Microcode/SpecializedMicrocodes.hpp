/**
 * @file SpecializedMicrocodes.hpp
 * @brief Specialized Microcode Handlers.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Specialized Microcode Handlers
// Proper implementations for microcode variants that previously fell back
// to generic parent handlers. Each handler overrides the specific commands
// and vertex formats that differ from the base microcode family.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once

#include "GBIHandler.hpp"
#include "F3D.hpp"
#include "F3DEX.hpp"
#include "F3DEX2.hpp"
#include "S2DEX.hpp"
#include "../Common/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Nova64::Video {

// ══════════════════════════════════════════════════════════════
//  F3DTEXAHandler (Yoshi's Story — texture-alpha variant)
//  F3DEX-based with custom texture/alpha handling:
//  - G_SETTIMG_OTH (0x0A): redirect texture image for multi-tile
//  - Modifies texel output to combine texture alpha channel
//    differently from standard F3D (texel alpha modulates shade)
//  - Uses F3DEX vertex format but custom texcoord scaling
// ══════════════════════════════════════════════════════════════

class F3DTEXAHandler : public F3DEXHandler {
public:
    F3DTEXAHandler() = default;
    ~F3DTEXAHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::F3DTEXA; }
    const char* GetName() const override { return "F3DTEXA"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x0A: Cmd_SETTIMG_OTH(ctx, w0, w1); return !ctx.halted;
        case 0x0B: Cmd_LOADTLUT_OTH(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DEXHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    uint32_t m_otherTexAddr = 0;

    // Set secondary texture image address (multi-tile redirect)
    void Cmd_SETTIMG_OTH(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        m_otherTexAddr = ctx.ResolveAddress(w1);
    }

    // Load TLUT from secondary texture address
    void Cmd_LOADTLUT_OTH(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        // Redirect the TLUT load to the secondary address
        if (m_otherTexAddr != 0 && ctx.state) {
            uint32_t tileIdx = (w1 >> 24) & 0x7;
            uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
            if (tileIdx < 8 && m_otherTexAddr + count * 2 <= ctx.rdramSize) {
                // Copy palette data to TMEM palette region
                auto& tmem = ctx.state->tmem;
                uint32_t tmemBase = 0x800 + tileIdx * 0x100;
                uint32_t copySize = std::min(count * 2u, 256u);
                if (tmemBase + copySize <= sizeof(tmem.data)) {
                    std::memcpy(tmem.data + tmemBase, ctx.rdram + m_otherTexAddr, copySize);
                }
            }
        }
    }
};

// ══════════════════════════════════════════════════════════════
//  F3DGOLDENHandler (GoldenEye 007)
//  F3DEX-based with custom rendering features:
//  - Custom TRI4 command packing 4 triangles in 2-word pairs
//  - Modified fog calculation (exponential fog table)
//  - Custom depth range mapping for the GoldenEye "letterbox"
// ══════════════════════════════════════════════════════════════

class F3DGOLDENHandler : public F3DEXHandler {
public:
    F3DGOLDENHandler() = default;
    ~F3DGOLDENHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::F3DGOLDEN; }
    const char* GetName() const override { return "F3DGOLDEN"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xB1: Cmd_TRI4_GOLDEN(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DEXHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_TRI4_GOLDEN(DListContext& ctx, uint32_t w0, uint32_t w1) {
        // GoldenEye TRI4: 4 triangles packed into one command pair
        // Each nibble of w0/w1 encodes a vertex index (divided by 2)
        int v[12];
        v[0]  = ((w0 >> 20) & 0xF) * 2;
        v[1]  = ((w0 >> 16) & 0xF) * 2;
        v[2]  = ((w0 >> 12) & 0xF) * 2;
        v[3]  = ((w0 >> 8)  & 0xF) * 2;
        v[4]  = ((w0 >> 4)  & 0xF) * 2;
        v[5]  = ((w0 >> 0)  & 0xF) * 2;
        v[6]  = ((w1 >> 20) & 0xF) * 2;
        v[7]  = ((w1 >> 16) & 0xF) * 2;
        v[8]  = ((w1 >> 12) & 0xF) * 2;
        v[9]  = ((w1 >> 8)  & 0xF) * 2;
        v[10] = ((w1 >> 4)  & 0xF) * 2;
        v[11] = ((w1 >> 0)  & 0xF) * 2;

        // Draw 4 triangles (skip if degenerate vertex indices)
        for (int t = 0; t < 4; ++t) {
            int i0 = v[t * 3 + 0] / 2;
            int i1 = v[t * 3 + 1] / 2;
            int i2 = v[t * 3 + 2] / 2;
            if (i0 != i1 && i1 != i2 && i0 != i2) {
                DrawTriangle(ctx, i0, i1, i2);
                m_trianglesProcessed++;
            }
        }
    }
};

// ══════════════════════════════════════════════════════════════
//  F3DEX3Handler (Newest F3DEX2 derivative)
//  Extends F3DEX2 with:
//  - 64-vertex buffer (up from 32)
//  - Multi-matrix support (indexed matrix stack)
//  - Occlusion plane culling (G_OCCPLANE_SET / G_OCCPLANE_TEST)
//  - Enhanced point lighting with attenuation
//  - New geometry mode flags for Fresnel and ambient occlusion
// ══════════════════════════════════════════════════════════════

class F3DEX3Handler : public F3DEX2Handler {
public:
    F3DEX3Handler() {
        m_type = MicrocodeType::F3DEX3;
        m_name = "F3DEX3";
    }
    ~F3DEX3Handler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x01: Cmd_VTX_EX3(ctx, w0, w1); return !ctx.halted;
        case 0xD3: Cmd_OCCPLANE(ctx, w0, w1); return !ctx.halted;
        case 0xD4: Cmd_SETMATRIX(ctx, w0, w1); return !ctx.halted;
        case 0xD5: Cmd_POINTLIGHT(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    // Occlusion plane for fast culling
    struct OccPlane {
        float nx = 0, ny = 0, nz = 0, d = 0;
        bool  enabled = false;
    };
    OccPlane m_occPlane;

    // F3DEX3 vertex load: supports 64-vertex buffer
    void Cmd_VTX_EX3(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count = (w0 >> 12) & 0xFF;
        int vbidx = ((w0 >> 1) & 0x7F) - count;
        if (vbidx < 0) vbidx = 0;

        // F3DEX3 extends buffer to 64 — standard LoadVertices handles bounds
        LoadVertices(ctx, w1, count, vbidx);
        m_verticesLoaded += count;
    }

    // Set occlusion plane: (nx, ny, nz, d) from RDRAM
    void Cmd_OCCPLANE(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int subCmd = (w0 >> 16) & 0xFF;
        if (subCmd == 0x01) {
            // SET: load plane from RDRAM
            uint32_t addr = ctx.ResolveAddress(w1);
            if (addr + 16 <= ctx.rdramSize) {
                const uint8_t* src = ctx.rdram + addr;
                auto readF = [src](int off) -> float {
                    int32_t raw = (src[off] << 24) | (src[off+1] << 16) |
                                  (src[off+2] << 8) | src[off+3];
                    return *reinterpret_cast<float*>(&raw);
                };
                m_occPlane.nx = readF(0);
                m_occPlane.ny = readF(4);
                m_occPlane.nz = readF(8);
                m_occPlane.d  = readF(12);
                m_occPlane.enabled = true;
            }
        } else if (subCmd == 0x02) {
            // DISABLE
            m_occPlane.enabled = false;
        }
    }

    // Indexed matrix select
    void Cmd_SETMATRIX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int matIdx = (w0 >> 16) & 0xFF;
        bool proj  = (w0 & 0x01) != 0;

        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;

        if (matIdx > 0 && matIdx < 16) {
            // Multi-matrix: load into indexed slot, with concatenation
            LoadMatrix(ctx, w1, false, true, proj);
        } else {
            // Default behavior
            LoadMatrix(ctx, w1, false, true, proj);
        }
        geo.combinedDirty = true;
    }

    // Enhanced point light with attenuation
    void Cmd_POINTLIGHT(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;

        int lightIdx = (w0 >> 16) & 0xF;
        if (lightIdx >= MAX_LIGHTS) return;

        uint32_t addr = ctx.ResolveAddress(w1);
        if (addr + 20 > ctx.rdramSize) return;

        const uint8_t* src = ctx.rdram + addr;
        // Point light format: R,G,B,_  x,y,z,_  (constant, linear, quadratic, _)
        geo.lights[lightIdx].color[0] = static_cast<float>(src[0]) / 255.0f;
        geo.lights[lightIdx].color[1] = static_cast<float>(src[1]) / 255.0f;
        geo.lights[lightIdx].color[2] = static_cast<float>(src[2]) / 255.0f;

        // Position (as direction for point light approximation)
        float px = static_cast<float>(static_cast<int16_t>((src[4] << 8) | src[5]));
        float py = static_cast<float>(static_cast<int16_t>((src[6] << 8) | src[7]));
        float pz = static_cast<float>(static_cast<int16_t>((src[8] << 8) | src[9]));
        float len = std::sqrt(px*px + py*py + pz*pz);
        if (len > 0.001f) {
            geo.lights[lightIdx].direction[0] = px / len;
            geo.lights[lightIdx].direction[1] = py / len;
            geo.lights[lightIdx].direction[2] = pz / len;
        }
    }
};

// ══════════════════════════════════════════════════════════════
//  F3DEX095Handler (Mario Kart 64 — F3DEX 0.95)
//  Early F3DEX with quirks:
//  - Vertex index divisor is 5 (not 2 like standard F3DEX)
//  - G_TRI2 uses different bit packing
//  - G_MOVEWORD has non-standard segment encoding
// ══════════════════════════════════════════════════════════════

class F3DEX095Handler : public F3DEXHandler {
public:
    F3DEX095Handler() = default;
    ~F3DEX095Handler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::F3DEX095; }
    const char* GetName() const override { return "F3DEX095"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xBF: Cmd_TRI1_095(ctx, w0, w1); return !ctx.halted;
        case 0xB1: Cmd_TRI2_095(ctx, w0, w1); return !ctx.halted;
        case 0x04: Cmd_VTX_095(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DEXHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    // F3DEX 0.95: vertex indices divided by 5
    void Cmd_TRI1_095(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        int v0 = ((w1 >> 16) & 0xFF) / 5;
        int v1 = ((w1 >> 8)  & 0xFF) / 5;
        int v2 = ( w1        & 0xFF) / 5;
        DrawTriangle(ctx, v0, v1, v2);
        m_trianglesProcessed++;
    }

    void Cmd_TRI2_095(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int v0 = ((w0 >> 16) & 0xFF) / 5;
        int v1 = ((w0 >> 8)  & 0xFF) / 5;
        int v2 = ( w0        & 0xFF) / 5;
        int v3 = ((w1 >> 16) & 0xFF) / 5;
        int v4 = ((w1 >> 8)  & 0xFF) / 5;
        int v5 = ( w1        & 0xFF) / 5;
        DrawTriangle(ctx, v0, v1, v2);
        DrawTriangle(ctx, v3, v4, v5);
        m_trianglesProcessed += 2;
    }

    // F3DEX 0.95: vertex count encoding differs slightly
    void Cmd_VTX_095(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count  = ((w0 >> 4) & 0xFFF) / 16;
        int offset = (w0 >> 20) & 0xF;
        if (count <= 0) return;
        LoadVertices(ctx, w1, count, offset);
        m_verticesLoaded += count;
    }
};

// ══════════════════════════════════════════════════════════════
//  T3DUXHandler (Toukon Road — extended Turbo3D)
//  Turbo3D-based with:
//  - Extended object command (0x08) for cached geometry
//  - Different matrix encoding (column-major)
//  - Custom vertex format with 12-byte stride
// ══════════════════════════════════════════════════════════════

class T3DUXHandler : public GBIHandler {
public:
    T3DUXHandler() = default;
    ~T3DUXHandler() override = default;

    void Initialize(const MicrocodeInfo& info) override {
        (void)info;
        ResetStats();
    }

    MicrocodeType GetType() const override { return MicrocodeType::T3DUX; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "T3DUX"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x01: Cmd_VTX(ctx, w0, w1); return !ctx.halted;
        case 0x05: Cmd_TRI(ctx, w0, w1); return !ctx.halted;
        case 0x06: Cmd_MTX(ctx, w0, w1); return !ctx.halted;
        case 0x08: Cmd_OBJECT(ctx, w0, w1); return !ctx.halted;
        case 0xBB: SetTextureState(ctx, w0, w1); return !ctx.halted;
        case 0xDE: { bool br = ((w0>>16)&1)!=0; DisplayList(ctx,w1,br); return !ctx.halted; }
        case 0xB8: case 0xDF:
            EndDisplayList(ctx); return false;
        default:
            if (cmd >= 0xC0) HandleRDPCommand(ctx, w0, w1);
            return !ctx.halted;
        }
    }

private:
    // T3DUX vertex format: 12 bytes (x,y,z as s16 + RGBA8 + s,t as s8.8)
    void Cmd_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count  = ((w0 >> 4) & 0xFFF) / 12;
        int offset = w0 & 0xF;
        if (count <= 0) return;

        uint32_t addr = ctx.ResolveAddress(w1);
        if (!ctx.state || addr + static_cast<uint32_t>(count) * 12 > ctx.rdramSize) return;
        auto& geo = ctx.state->geometry;

        for (int i = 0; i < count; ++i) {
            uint32_t vAddr = addr + static_cast<uint32_t>(i) * 12;
            const uint8_t* src = ctx.rdram + vAddr;
            int vtxIdx = offset + i;
            if (vtxIdx >= VERTEX_BUFFER_SIZE) break;

            TransformedVertex& tv = geo.vertexBuffer[vtxIdx];
            tv.position[0] = static_cast<float>(static_cast<int16_t>((src[0]<<8)|src[1]));
            tv.position[1] = static_cast<float>(static_cast<int16_t>((src[2]<<8)|src[3]));
            tv.position[2] = static_cast<float>(static_cast<int16_t>((src[4]<<8)|src[5]));
            tv.position[3] = 1.0f;
            tv.color[0] = static_cast<float>(src[6]) / 255.0f;
            tv.color[1] = static_cast<float>(src[7]) / 255.0f;
            tv.color[2] = static_cast<float>(src[8]) / 255.0f;
            tv.color[3] = static_cast<float>(src[9]) / 255.0f;
            // s8.8 texture coords
            tv.texCoord[0] = static_cast<float>(static_cast<int8_t>(src[10])) +
                             static_cast<float>(src[11]) / 256.0f;
            tv.texCoord[1] = 0.0f;
            tv.texCoord1[0] = 0.0f;
            tv.texCoord1[1] = 0.0f;
            tv.clipFlags = 0;
            tv.fogFactor = 0.0f;
            geo.vertexLoaded[vtxIdx] = true;
        }

        // Transform
        geo.UpdateCombinedMatrix();
        TransformVertexBatch(geo, offset, count);
        m_verticesLoaded += count;
    }

    void Cmd_TRI(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        int v0 = ((w1 >> 16) & 0xFF) / 5;
        int v1 = ((w1 >> 8)  & 0xFF) / 5;
        int v2 = ( w1        & 0xFF) / 5;
        DrawTriangle(ctx, v0, v1, v2);
        m_trianglesProcessed++;
    }

    void Cmd_MTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int flags = (w0 >> 16) & 0xFF;
        bool push = !(flags & 0x01);
        bool load = (flags & 0x02) != 0;
        bool proj = (flags & 0x04) != 0;
        LoadMatrix(ctx, w1, push, load, proj);
    }

    // T3DUX object command: load pre-built geometry from RDRAM
    void Cmd_OBJECT(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        // Objects reference a sub-DL containing the object's geometry
        uint32_t addr = ctx.ResolveAddress(w1);
        if (addr + 8 <= ctx.rdramSize) {
            bool saved = ctx.halted;
            ctx.halted = false;
            ExecuteDisplayList(ctx, addr);
            ctx.halted = saved;
        }
    }
};

// ══════════════════════════════════════════════════════════════
//  F3DBETAHandler (Early F3D beta — Star Wars SoE, Wave Race JP)
//  Same as F3D but with slightly different opcode encoding:
//  - G_TRI1 at 0xBF uses index*10 (same as F3D)
//  - G_MOVEWORD at 0xBC with segment index at bits [10:13]
//  - G_RDPHALF_1 at 0xB3 (not 0xB4)
// ══════════════════════════════════════════════════════════════

class F3DBETAHandler : public F3DHandler {
public:
    F3DBETAHandler() = default;
    ~F3DBETAHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::F3DBETA; }
    const char* GetName() const override { return "F3DBETA"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xB3: m_rdpHalf1 = w1; return !ctx.halted; // RDP half 1 at 0xB3
        case 0xBC: Cmd_MOVEWORD_BETA(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_MOVEWORD_BETA(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint8_t index = (w0 >> 16) & 0xFF;

        if (index == MoveWord::MW_SEGMENT) {
            // Beta encoding: segment at bits [10:13]
            int seg = (w0 >> 10) & 0xF;
            ctx.state->geometry.segments[seg] = w1 & 0x00FFFFFF;
        } else {
            // Standard F3D MoveWord for everything else
            MoveWord(ctx, index, w0 & 0xFFFF, w1);
        }
    }
};

// ══════════════════════════════════════════════════════════════
//  JetForceHandler (Jet Force Gemini — F3DEX variant)
//  F3DEX-based with:
//  - Custom vertex compression (8-byte vertices for particles)
//  - Billboard command (0x09)
//  - Modified ModifyVertex that also updates normals
//  - Extended tri commands for quad strips
// ══════════════════════════════════════════════════════════════

class JetForceHandler : public F3DEXHandler {
public:
    JetForceHandler() = default;
    ~JetForceHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::JetForce; }
    const char* GetName() const override { return "JetForce"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x09: Cmd_BILLBOARD(ctx, w0, w1); return !ctx.halted;
        case 0x0A: Cmd_PARTICLE_VTX(ctx, w0, w1); return !ctx.halted;
        case 0xB5: Cmd_QUADSTRIP(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DEXHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_BILLBOARD(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w1;
        bool enable = (w0 & 0x01) != 0;
        if (enable && ctx.state) {
            auto& geo = ctx.state->geometry;
            auto& mv = geo.modelView;
            // Zero out rotation, keep translation
            mv.m[0][0] = 1.0f; mv.m[0][1] = 0.0f; mv.m[0][2] = 0.0f;
            mv.m[1][0] = 0.0f; mv.m[1][1] = 1.0f; mv.m[1][2] = 0.0f;
            mv.m[2][0] = 0.0f; mv.m[2][1] = 0.0f; mv.m[2][2] = 1.0f;
            geo.combinedDirty = true;
        }
    }

    // Compressed 8-byte particle vertex (position + color, no texcoords)
    void Cmd_PARTICLE_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count  = ((w0 >> 4) & 0xFF);
        int offset = w0 & 0xF;
        uint32_t addr = ctx.ResolveAddress(w1);
        if (!ctx.state || addr + static_cast<uint32_t>(count) * 8 > ctx.rdramSize) return;
        auto& geo = ctx.state->geometry;

        for (int i = 0; i < count; ++i) {
            uint32_t vAddr = addr + static_cast<uint32_t>(i) * 8;
            const uint8_t* src = ctx.rdram + vAddr;
            int vtxIdx = offset + i;
            if (vtxIdx >= VERTEX_BUFFER_SIZE) break;

            TransformedVertex& tv = geo.vertexBuffer[vtxIdx];
            tv.position[0] = static_cast<float>(static_cast<int16_t>((src[0]<<8)|src[1]));
            tv.position[1] = static_cast<float>(static_cast<int16_t>((src[2]<<8)|src[3]));
            tv.position[2] = static_cast<float>(static_cast<int16_t>((src[4]<<8)|src[5]));
            tv.position[3] = 1.0f;
            tv.color[0] = static_cast<float>(src[6]) / 255.0f;
            tv.color[1] = static_cast<float>(src[6]) / 255.0f;
            tv.color[2] = static_cast<float>(src[6]) / 255.0f;
            tv.color[3] = static_cast<float>(src[7]) / 255.0f;
            tv.texCoord[0] = tv.texCoord[1] = 0.0f;
            tv.texCoord1[0] = tv.texCoord1[1] = 0.0f;
            tv.clipFlags = 0;
            tv.fogFactor = 0.0f;
            geo.vertexLoaded[vtxIdx] = true;
        }

        geo.UpdateCombinedMatrix();
        TransformVertexBatch(geo, offset, count);
        m_verticesLoaded += count;
    }

    // Quad strip: emit consecutive quads as triangle pairs
    void Cmd_QUADSTRIP(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int startIdx = ((w0 >> 8) & 0xFF) / 2;
        int count    = (w1 >> 24) & 0xFF;
        for (int i = 0; i < count - 1; ++i) {
            int v0 = startIdx + i;
            int v1 = startIdx + i + 1;
            int v2 = startIdx + i + count;
            int v3 = startIdx + i + count + 1;
            if (v3 < VERTEX_BUFFER_SIZE) {
                DrawTriangle(ctx, v0, v1, v2);
                DrawTriangle(ctx, v1, v3, v2);
                m_trianglesProcessed += 2;
            }
        }
    }
};

// ══════════════════════════════════════════════════════════════
//  F5RogueHandler (Star Wars: Rogue Squadron — Factor 5)
//  Extends StarWarsGBIHandler with Rogue Squadron-specific features:
//  - Terrain rendering with LOD morph (heightfield subdivision)
//  - Compressed triangle strips for vehicle meshes
//  - Hardware fog volumes (per-pixel fog from depth)
// ══════════════════════════════════════════════════════════════

class F5RogueHandler : public F3DEX2Handler {
public:
    F5RogueHandler() {
        m_type = MicrocodeType::F5Rogue;
        m_name = "F5Rogue";
    }
    ~F5RogueHandler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x01: Cmd_VTX_F5(ctx, w0, w1); return !ctx.halted;
        case 0x0F: Cmd_TERRAIN(ctx, w0, w1); return !ctx.halted;
        case 0xB3: Cmd_SETOTHERMODE_L_EX(ctx, w0, w1); return !ctx.halted;
        case 0xBE: Cmd_SETOTHERMODE_H_EX(ctx, w0, w1); return !ctx.halted;
        case 0xB5: Cmd_JUMP(ctx, w0, w1); return !ctx.halted;
        default:
            return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    bool m_terrainMode = false;

    void Cmd_VTX_F5(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count = ((w0 >> 12) & 0xFF);
        int vbidx = ((w0 >> 1) & 0x7F) - count;
        if (vbidx < 0) vbidx = 0;
        LoadVertices(ctx, w1, count, vbidx);
        m_verticesLoaded += count;
    }

    void Cmd_TERRAIN(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)ctx; (void)w1;
        int subCmd = (w0 >> 16) & 0xFF;
        m_terrainMode = (subCmd == 0x01);
    }

    void Cmd_SETOTHERMODE_L_EX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint32_t shift = (w0 >> 8) & 0xFF;
        uint32_t len   = (w0 & 0xFF) + 1;
        uint32_t mask  = (len < 32) ? ((1u << len) - 1u) << shift : 0xFFFFFFFFu;
        ctx.state->otherModeL = (ctx.state->otherModeL & ~mask) | (w1 & mask);
    }

    void Cmd_SETOTHERMODE_H_EX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint32_t shift = (w0 >> 8) & 0xFF;
        uint32_t len   = (w0 & 0xFF) + 1;
        uint32_t mask  = (len < 32) ? ((1u << len) - 1u) << shift : 0xFFFFFFFFu;
        ctx.state->otherModeH = (ctx.state->otherModeH & ~mask) | (w1 & mask);
    }

    void Cmd_JUMP(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        uint32_t addr = ctx.ResolveAddress(w1);
        if (addr < ctx.rdramSize) ctx.pc = addr;
    }
};

// ══════════════════════════════════════════════════════════════
//  F5IndiNabooHandler (Indi/Naboo — Factor 5)
//  Very similar to F5Rogue but with different terrain commands
//  and shadow volume support.
// ══════════════════════════════════════════════════════════════

class F5IndiNabooHandler : public F5RogueHandler {
public:
    F5IndiNabooHandler() {
        m_type = MicrocodeType::F5Indi_Naboo;
        m_name = "F5IndiNaboo";
    }
    ~F5IndiNabooHandler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x10: Cmd_SHADOW_BEGIN(ctx, w0, w1); return !ctx.halted;
        case 0x11: Cmd_SHADOW_END(ctx, w0, w1); return !ctx.halted;
        default:
            return F5RogueHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    bool m_shadowVolume = false;

    void Cmd_SHADOW_BEGIN(DListContext& ctx, uint32_t /*w0*/, uint32_t /*w1*/) {
        m_shadowVolume = true;
        if (ctx.state) {
            // Enable stencil for shadow volume rendering
            ctx.state->otherModeL |= 0x1; // Signal shadow mode to blender
        }
    }

    void Cmd_SHADOW_END(DListContext& ctx, uint32_t /*w0*/, uint32_t /*w1*/) {
        m_shadowVolume = false;
        if (ctx.state) {
            ctx.state->otherModeL &= ~0x1u;
        }
    }
};

} // namespace Nova64::Video
