/**
 * @file ExtendedGBIHandlers.hpp
 * @brief GBIHandler subclasses for extended microcodes (Turbo3D, ZSort, DKR, Waverace, Star Wars,
 *        Line3D and the F3D/F3DEX2 variants), each with its own vertex loading, matrix transforms
 *        and triangle drawing via DListContext.
 */

#pragma once

#include "GBIHandler.hpp"
#include "F3D.hpp"
#include "F3DEX.hpp"
#include "F3DEX2.hpp"
#include "../Common/ClipMath.hpp"
#include "../Common/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Nova64::Video {

using namespace Clip; // CLIP_POSX, CLIP_NEGX, etc.

/// Turbo3D (Indiana Jones, Stunt Race FX): simplified microcode. Inherits from F3DHandler for
/// DL/ENDDL/MOVEMEM/RDP passthrough and overrides VTX/TRI/MTX with Turbo3D-specific encodings.

class Turbo3DGBIHandler : public F3DHandler {
public:
    Turbo3DGBIHandler() = default;
    ~Turbo3DGBIHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::Turbo3D; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "Turbo3D"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x01: Cmd_VTX(ctx, w0, w1); return !ctx.halted;
        case 0x05: Cmd_TRI(ctx, w0, w1); return !ctx.halted;
        case 0x06: Cmd_MTX(ctx, w0, w1); return !ctx.halted;
        default:
            // Delegate F3D-standard commands (MOVEMEM, DL, ENDDL, RDP passthrough)
            return F3DHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        // Turbo3D vertex count: encoded differently than F3D
        // w0 bits [15:4] = data size / 10 = vertex count
        int count = ((w0 >> 4) & 0xFFF) / 10;
        int offset = w0 & 0xF;
        if (count <= 0) return;

        // Turbo3D uses same 16-byte vertex format as F3D
        LoadVertices(ctx, w1, count, offset);
        m_verticesLoaded += count;
    }

    void Cmd_TRI(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        // Turbo3D triangle: v indices divided by 5 (not 10 like F3DEX)
        int v0 = ((w1 >> 16) & 0xFF) / 5;
        int v1 = ((w1 >> 8) & 0xFF) / 5;
        int v2 = (w1 & 0xFF) / 5;

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
};

/// ZSort (World Driver Championship): Z-sorted rendering at the microcode level. Accumulates
/// objects, sorts by Z, then processes their DLs.

class ZSortGBIHandler : public GBIHandler {
public:
    ZSortGBIHandler() = default;
    ~ZSortGBIHandler() override = default;

    void Initialize(const MicrocodeInfo& info) override {
        (void)info;
        ResetStats();
    }

    MicrocodeType GetType() const override { return MicrocodeType::ZSort; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "ZSort"; }

    void OnFrameBegin(DListContext& ctx) override {
        (void)ctx;
        m_zObjects.clear();
        m_signalMask = 0;
    }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x00: Cmd_RDPCMD(ctx, w0, w1); return !ctx.halted;
        case 0x01: Cmd_VTX(ctx, w0, w1); return !ctx.halted;
        case 0x02: Cmd_TRI(ctx, w0, w1); return !ctx.halted;
        case 0x03: Cmd_MULTMATRIX(ctx, w0, w1); return !ctx.halted;
        case 0x04: Cmd_LIGHTING(ctx, w0, w1); return !ctx.halted;
        case 0x05: Cmd_XFMLIGHT(ctx, w0, w1); return !ctx.halted;
        case 0x06: Cmd_SETMASK(ctx, w0, w1); return !ctx.halted;
        case 0x07: Cmd_ZOBJ(ctx, w0, w1); return !ctx.halted;
        case 0x08: /* WAITSIGNAL - no-op in HLE */ return !ctx.halted;
        case 0x09: Cmd_SENDSIGNAL(ctx, w0, w1); return !ctx.halted;
        case 0xB8:
        case 0xDF: EndDisplayList(ctx); return false;
        default:
            // RDP passthrough for 0xC0-0xFF range
            if (cmd >= 0xC0) {
                HandleRDPCommand(ctx, w0, w1);
            } else {
                NOVA_LOG(LogLevel::Warning, LogSubsystem::Microcode,
                         "ZSort: Unknown opcode 0x{:02X} (w0=0x{:08X}, w1=0x{:08X})", cmd, w0, w1);
            }
            return !ctx.halted;
        }
    }

private:
    struct ZSortObject {
        uint32_t rdramAddr;
        float zValue;
        uint32_t flags;
    };

    std::vector<ZSortObject> m_zObjects;
    uint32_t m_signalMask = 0;

    void Cmd_RDPCMD(DListContext& ctx, uint32_t w0, uint32_t w1) { HandleRDPCommand(ctx, w0, w1); }

    void Cmd_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count = (w0 >> 12) & 0xFF;
        int vtxBase = w0 & 0xFFF;
        if (count <= 0) return;

        LoadVertices(ctx, w1, count, vtxBase / 2);
        m_verticesLoaded += count;
    }

    void Cmd_TRI(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        int v0 = ((w1 >> 16) & 0xFF) / 2;
        int v1 = ((w1 >> 8) & 0xFF) / 2;
        int v2 = (w1 & 0xFF) / 2;

        DrawTriangle(ctx, v0, v1, v2);
        m_trianglesProcessed++;
    }

    void Cmd_MULTMATRIX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        bool proj = ((w0 >> 8) & 0xFF) != 0;
        LoadMatrix(ctx, w1, false, true, proj);
    }

    void Cmd_LIGHTING(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        uint32_t addr = ctx.ResolveAddress(w1);
        if (!ctx.state || addr + 16 > ctx.rdramSize) return;

        auto& geo = ctx.state->geometry;
        const uint8_t* src = ctx.rdram + addr;

        float lx = static_cast<float>(static_cast<int8_t>(src[0])) / 127.0f;
        float ly = static_cast<float>(static_cast<int8_t>(src[1])) / 127.0f;
        float lz = static_cast<float>(static_cast<int8_t>(src[2])) / 127.0f;
        float len = std::sqrt(lx * lx + ly * ly + lz * lz);
        if (len > 0.0001f) {
            lx /= len;
            ly /= len;
            lz /= len;
        }

        if (geo.numLights < MAX_LIGHTS) {
            geo.lights[0].direction[0] = lx;
            geo.lights[0].direction[1] = ly;
            geo.lights[0].direction[2] = lz;
            geo.lights[0].color[0] = static_cast<float>(src[4]) / 255.0f;
            geo.lights[0].color[1] = static_cast<float>(src[5]) / 255.0f;
            geo.lights[0].color[2] = static_cast<float>(src[6]) / 255.0f;
        }
    }

    void Cmd_XFMLIGHT(DListContext& ctx, uint32_t w0, uint32_t w1) { Cmd_LIGHTING(ctx, w0, w1); }

    void Cmd_SETMASK(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)ctx;
        (void)w0;
        m_signalMask = w1;
    }

    void Cmd_ZOBJ(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)ctx;
        ZSortObject obj;
        obj.rdramAddr = w1;
        obj.zValue = static_cast<float>(static_cast<int16_t>((w0 >> 8) & 0xFFFF)) / 256.0f;
        obj.flags = w0 & 0xFF;
        m_zObjects.push_back(obj);
    }

    void Cmd_SENDSIGNAL(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        (void)w1;

        // Sort accumulated Z-objects back-to-front for correct transparency
        std::sort(m_zObjects.begin(), m_zObjects.end(),
                  [](const ZSortObject& a, const ZSortObject& b) { return a.zValue > b.zValue; });

        // Process each object's sub-display-list
        for (const auto& obj : m_zObjects) {
            uint32_t addr = ctx.ResolveAddress(obj.rdramAddr);
            if (addr + 8 <= ctx.rdramSize) {
                uint32_t savedPc = ctx.pc;
                int savedDepth = ctx.dlStackDepth;
                bool savedHalted = ctx.halted;

                // Hide the outer stack so ENDDL cleanly halts inner execution
                ctx.dlStackDepth = 0;
                ctx.halted = false;

                ExecuteDisplayList(ctx, addr);

                // Restore outer context state
                ctx.pc = savedPc;
                ctx.dlStackDepth = savedDepth;
                ctx.halted = savedHalted;
            }
        }
        m_zObjects.clear();
    }
};

/// DKR (Diddy Kong Racing, Mickey's Speedway, JFG): custom 10-byte vertex format, indexed
/// matrices and billboards.

class DKRGBIHandler : public GBIHandler {
public:
    DKRGBIHandler() = default;
    ~DKRGBIHandler() override = default;

    void Initialize(const MicrocodeInfo& info) override {
        (void)info;
        ResetStats();
    }

    MicrocodeType GetType() const override { return MicrocodeType::DiddyKong; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "DKR"; }

    void OnFrameBegin(DListContext& ctx) override {
        (void)ctx;
        m_billboardEnabled = false;
        m_vtxBaseAddr = 0;
        m_textureIndex = 0;
    }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x01: Cmd_VTX(ctx, w0, w1); return !ctx.halted;
        case 0x05: Cmd_TRI1(ctx, w0, w1); return !ctx.halted;
        case 0x06: Cmd_TRI2(ctx, w0, w1); return !ctx.halted;
        case 0x07: Cmd_MTX(ctx, w0, w1); return !ctx.halted;
        case 0x03: Cmd_MOVEMEM(ctx, w0, w1); return !ctx.halted;
        case 0xBB: Cmd_TEXTURE(ctx, w0, w1); return !ctx.halted;
        case 0xDE: Cmd_DL(ctx, w0, w1); return !ctx.halted;
        case 0x09: Cmd_BILLBOARD(ctx, w0, w1); return !ctx.halted;
        case 0xB8:
        case 0xDF: EndDisplayList(ctx); return false;
        default:
            if (cmd >= 0xC0) {
                HandleRDPCommand(ctx, w0, w1);
            } else {
                NOVA_LOG(LogLevel::Warning, LogSubsystem::Microcode,
                         "DKR: Unknown opcode 0x{:02X} (w0=0x{:08X}, w1=0x{:08X})", cmd, w0, w1);
            }
            return !ctx.halted;
        }
    }

private:
    bool m_billboardEnabled = false;
    uint32_t m_vtxBaseAddr = 0;
    int m_textureIndex = 0;

    void Cmd_VTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        // DKR vertex format: 10 bytes per vertex
        int count = ((w0 >> 4) & 0x1F) + 1;
        int offset = (w0 >> 9) & 0x1F;
        uint32_t addr = ctx.ResolveAddress(w1);

        if (!ctx.state || addr + static_cast<uint32_t>(count) * 10 > ctx.rdramSize) return;
        auto& geo = ctx.state->geometry;

        for (int i = 0; i < count; ++i) {
            uint32_t vAddr = addr + static_cast<uint32_t>(i) * 10;
            const uint8_t* src = ctx.rdram + vAddr;

            int vtxIdx = offset + i;
            if (vtxIdx >= VERTEX_BUFFER_SIZE) break;

            TransformedVertex& tv = geo.vertexBuffer[vtxIdx];
            tv.position[0] = static_cast<float>(static_cast<int16_t>((src[0] << 8) | src[1]));
            tv.position[1] = static_cast<float>(static_cast<int16_t>((src[2] << 8) | src[3]));
            tv.position[2] = static_cast<float>(static_cast<int16_t>((src[4] << 8) | src[5]));
            tv.position[3] = 1.0f;

            tv.color[0] = static_cast<float>(src[6]) / 255.0f;
            tv.color[1] = static_cast<float>(src[7]) / 255.0f;
            tv.color[2] = static_cast<float>(src[8]) / 255.0f;
            tv.color[3] = static_cast<float>(src[9]) / 255.0f;

            tv.texCoord[0] = 0.0f;
            tv.texCoord[1] = 0.0f;
            tv.texCoord1[0] = 0.0f;
            tv.texCoord1[1] = 0.0f;
            tv.clipFlags = 0;
            tv.fogFactor = 0.0f;
        }

        // Transform through MVP
        TransformVertices(ctx, offset, count);
        m_verticesLoaded += count;
    }

    void Cmd_TRI1(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        int v0 = ((w1 >> 16) & 0xFF) / 10;
        int v1 = ((w1 >> 8) & 0xFF) / 10;
        int v2 = (w1 & 0xFF) / 10;

        DrawTriangle(ctx, v0, v1, v2);
        m_trianglesProcessed++;
    }

    void Cmd_TRI2(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int v0 = ((w0 >> 16) & 0xFF) / 10;
        int v1 = ((w0 >> 8) & 0xFF) / 10;
        int v2 = (w0 & 0xFF) / 10;
        int v3 = ((w1 >> 16) & 0xFF) / 10;
        int v4 = ((w1 >> 8) & 0xFF) / 10;
        int v5 = (w1 & 0xFF) / 10;

        DrawTriangle(ctx, v0, v1, v2);
        DrawTriangle(ctx, v3, v4, v5);
        m_trianglesProcessed += 2;
    }

    void Cmd_MTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        LoadMatrix(ctx, w1, false, true, false);
    }

    void Cmd_MOVEMEM(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int type = (w0 >> 16) & 0xFF;

        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;

        switch (type) {
        case 0x80: {
            // Viewport — delegate to GBI1 MOVEMEM which handles viewport
            uint8_t size = static_cast<uint8_t>(((w0 >> 19) & 0x1F) * 8);
            MoveMemGBI1(ctx, static_cast<uint8_t>(type), size, w1);
            break;
        }
        case 0x08: {
            int seg = (w0 >> 8) & 0xF;
            geo.segments[seg] = w1 & 0x00FFFFFF;
            break;
        }
        case 0x86: {
            uint32_t addr = ctx.ResolveAddress(w1);
            if (addr + 16 <= ctx.rdramSize) {
                const uint8_t* src = ctx.rdram + addr;
                if (geo.numLights < MAX_LIGHTS) {
                    geo.lights[0].color[0] = static_cast<float>(src[0]) / 255.0f;
                    geo.lights[0].color[1] = static_cast<float>(src[1]) / 255.0f;
                    geo.lights[0].color[2] = static_cast<float>(src[2]) / 255.0f;
                    geo.lights[0].direction[0] = static_cast<float>(static_cast<int8_t>(src[8])) / 127.0f;
                    geo.lights[0].direction[1] = static_cast<float>(static_cast<int8_t>(src[9])) / 127.0f;
                    geo.lights[0].direction[2] = static_cast<float>(static_cast<int8_t>(src[10])) / 127.0f;
                }
            }
            break;
        }
        default: break;
        }
    }

    void Cmd_DL(DListContext& ctx, uint32_t w0, uint32_t w1) {
        bool branch = ((w0 >> 16) & 0x01) != 0;
        DisplayList(ctx, w1, branch);
    }

    void Cmd_BILLBOARD(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w1;
        m_billboardEnabled = (w0 & 0x01) != 0;
        if (m_billboardEnabled && ctx.state) {
            auto& geo = ctx.state->geometry;
            auto& mv = geo.modelView;
            // Preserve translation (column 3), set rotation to identity
            mv.m[0][0] = 1.0f;
            mv.m[0][1] = 0.0f;
            mv.m[0][2] = 0.0f;
            mv.m[1][0] = 0.0f;
            mv.m[1][1] = 1.0f;
            mv.m[1][2] = 0.0f;
            mv.m[2][0] = 0.0f;
            mv.m[2][1] = 0.0f;
            mv.m[2][2] = 1.0f;
            geo.combinedDirty = true;
        }
    }

    void Cmd_TEXTURE(DListContext& ctx, uint32_t w0, uint32_t w1) {
        // Delegate to the standard G_TEXTURE handler
        SetTextureState(ctx, w0, w1);
    }

    void TransformVertices(DListContext& ctx, int startIndex, int count) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;
        geo.UpdateCombinedMatrix();

        for (int i = 0; i < count; ++i) {
            int idx = startIndex + i;
            if (idx >= VERTEX_BUFFER_SIZE) break;
            TransformedVertex& tv = geo.vertexBuffer[idx];
            float in[4] = {tv.position[0], tv.position[1], tv.position[2], 1.0f};
            float out[4];
            // Row-vector convention (v_clip = v * combinedMatrix), matching every other transform in this
            // codebase (GBIHandler::TransformVertexBatch, RDPState's matrix multiplies) and how
            // combinedMatrix is populated by the shared LoadMatrix() that this handler's own Cmd_MTX
            // delegates to. Indexing as out[r] = M[r][c]*in[c] (column-vector, M*v) would transpose the
            // transform and give wrong vertex positions for any DKR-microcode content.
            for (int c = 0; c < 4; ++c) {
                out[c] = in[0] * geo.combinedMatrix.m[0][c] + in[1] * geo.combinedMatrix.m[1][c] +
                         in[2] * geo.combinedMatrix.m[2][c] + in[3] * geo.combinedMatrix.m[3][c];
            }
            tv.position[0] = out[0];
            tv.position[1] = out[1];
            tv.position[2] = out[2];
            tv.position[3] = out[3];

            tv.clipFlags = 0;
            float w = tv.position[3];
            if (tv.position[0] > +w) tv.clipFlags |= CLIP_POSX;
            if (tv.position[0] < -w) tv.clipFlags |= CLIP_NEGX;
            if (tv.position[1] > +w) tv.clipFlags |= CLIP_POSY;
            if (tv.position[1] < -w) tv.clipFlags |= CLIP_NEGY;
            if (tv.position[2] > +w) tv.clipFlags |= CLIP_POSZ;
            if (tv.position[2] < -w) tv.clipFlags |= CLIP_NEGZ;
        }
    }
};

/// Wave Race 64: F3DEX-based with custom water surface rendering. Overrides the water/heightmap
/// commands and delegates everything else.

class WaveraceGBIHandler : public F3DEXHandler {
public:
    WaveraceGBIHandler() = default;
    ~WaveraceGBIHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::WaveRace; }
    const char* GetName() const override { return "Waverace"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x0A: Cmd_WATER(ctx, w0, w1); return !ctx.halted;
        case 0x0B: Cmd_HEIGHTMAP(ctx, w0, w1); return !ctx.halted;
        case 0x0C: Cmd_AMBIENT(ctx, w0, w1); return !ctx.halted;
        default:   return F3DEXHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    struct WaterState {
        float amplitude = 0.0f;
        float frequency = 0.0f;
        float phase = 0.0f;
        uint32_t heightmapAddr = 0;
        int gridWidth = 0;
        int gridHeight = 0;
    };
    WaterState m_water;

    void Cmd_WATER(DListContext& ctx, uint32_t w0, uint32_t w1) {
        m_water.amplitude = static_cast<float>(static_cast<int16_t>((w0 >> 8) & 0xFFFF)) / 256.0f;
        m_water.frequency = static_cast<float>(static_cast<int16_t>(w1 >> 16)) / 256.0f;
        m_water.phase = static_cast<float>(static_cast<int16_t>(w1 & 0xFFFF)) / 256.0f;

        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;

        for (int i = 0; i < VERTEX_BUFFER_SIZE; ++i) {
            auto& tv = geo.vertexBuffer[i];
            if (geo.vertexLoaded[i] && tv.position[3] != 0.0f) {
                float waveOffset = m_water.amplitude * std::sin(m_water.frequency * tv.position[0] + m_water.phase);
                tv.position[1] += waveOffset;
            }
        }
    }

    void Cmd_HEIGHTMAP(DListContext& ctx, uint32_t w0, uint32_t w1) {
        m_water.heightmapAddr = ctx.ResolveAddress(w1);
        m_water.gridWidth = (w0 >> 8) & 0xFF;
        m_water.gridHeight = w0 & 0xFF;

        if (!ctx.state || m_water.gridWidth == 0 || m_water.gridHeight == 0) return;

        uint32_t mapSize = static_cast<uint32_t>(m_water.gridWidth * m_water.gridHeight) * 2;
        if (m_water.heightmapAddr + mapSize > ctx.rdramSize) return;

        auto& geo = ctx.state->geometry;
        const uint8_t* mapData = ctx.rdram + m_water.heightmapAddr;

        int cellIdx = 0;
        for (int z = 0; z < m_water.gridHeight && cellIdx < VERTEX_BUFFER_SIZE; z++) {
            for (int x = 0; x < m_water.gridWidth && cellIdx < VERTEX_BUFFER_SIZE; x++) {
                int mapOffset = (z * m_water.gridWidth + x) * 2;
                int16_t height = static_cast<int16_t>((mapData[mapOffset] << 8) | mapData[mapOffset + 1]);
                geo.vertexBuffer[cellIdx].position[1] += static_cast<float>(height) / 256.0f;
                cellIdx++;
            }
        }
    }

    void Cmd_AMBIENT(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)w0;
        uint32_t addr = ctx.ResolveAddress(w1);
        if (!ctx.state || addr + 4 > ctx.rdramSize) return;

        auto& geo = ctx.state->geometry;
        const uint8_t* src = ctx.rdram + addr;

        // Set ambient light color
        geo.ambient.color[0] = static_cast<float>(src[0]) / 255.0f;
        geo.ambient.color[1] = static_cast<float>(src[1]) / 255.0f;
        geo.ambient.color[2] = static_cast<float>(src[2]) / 255.0f;
    }
};

/// Star Wars (Rogue Squadron, Episode I Racer, BFN): F3DEX2-based Factor 5 custom microcode with
/// compressed vertex support, quads and terrain LOD.

class StarWarsGBIHandler : public F3DEX2Handler {
public:
    StarWarsGBIHandler() {
        m_type = MicrocodeType::StarWars;
        m_name = "StarWars";
    }
    ~StarWarsGBIHandler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x01: Cmd_VTX_SW(ctx, w0, w1); return !ctx.halted;
        case 0xB1: Cmd_QUAD(ctx, w0, w1); return !ctx.halted;
        case 0x0F: Cmd_SPECIAL(ctx, w0, w1); return !ctx.halted;
        // F5-specific extended othermode commands (Rogue Squadron)
        case 0xB3: Cmd_SETOTHERMODE_L_EX(ctx, w0, w1); return !ctx.halted;
        case 0xBE: Cmd_SETOTHERMODE_H_EX(ctx, w0, w1); return !ctx.halted;
        // F5 display list jump (JUMPSWDL)
        case 0xB5: Cmd_JUMPSWDL(ctx, w0, w1); return !ctx.halted;
        default:
            // F3DEX2 handles TRI, QUAD, MTX, DL, TEXTURE, CULLDL, ENDDL, RDP, etc.
            return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    bool m_compressedVtx = false;

    void Cmd_VTX_SW(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int count = ((w0 >> 12) & 0xFF);
        int vbidx = ((w0 >> 1) & 0x7F) - count;
        if (vbidx < 0) vbidx = 0;

        if (m_compressedVtx) {
            LoadCompressedVertices(ctx, w1, count, vbidx);
        } else {
            LoadVertices(ctx, w1, count, vbidx);
        }
        m_verticesLoaded += count;
    }

    void LoadCompressedVertices(DListContext& ctx, uint32_t address, int count, int startIndex) {
        address = ctx.ResolveAddress(address);
        if (!ctx.state || address + static_cast<uint32_t>(count) * 8 > ctx.rdramSize) return;

        auto& geo = ctx.state->geometry;
        geo.UpdateCombinedMatrix();

        for (int i = 0; i < count; ++i) {
            uint32_t vAddr = address + static_cast<uint32_t>(i) * 8;
            const uint8_t* src = ctx.rdram + vAddr;

            int vtxIdx = startIndex + i;
            if (vtxIdx >= VERTEX_BUFFER_SIZE) break;

            TransformedVertex& tv = geo.vertexBuffer[vtxIdx];

            float x = static_cast<float>(static_cast<int16_t>((src[0] << 8) | src[1]));
            float y = static_cast<float>(static_cast<int16_t>((src[2] << 8) | src[3]));
            float z = static_cast<float>(static_cast<int16_t>((src[4] << 8) | src[5]));

            // Color packed as RGBA5551
            uint16_t rgba = static_cast<uint16_t>((src[6] << 8) | src[7]);
            tv.color[0] = static_cast<float>((rgba >> 11) & 0x1F) / 31.0f;
            tv.color[1] = static_cast<float>((rgba >> 6) & 0x1F) / 31.0f;
            tv.color[2] = static_cast<float>((rgba >> 1) & 0x1F) / 31.0f;
            tv.color[3] = (rgba & 1) ? 1.0f : 0.0f;

            tv.texCoord[0] = 0.0f;
            tv.texCoord[1] = 0.0f;
            tv.texCoord1[0] = 0.0f;
            tv.texCoord1[1] = 0.0f;

            // Transform through combined MVP. Row-vector convention (v * combinedMatrix), same rationale as
            // DKRGBIHandler::TransformVertices above: out[r]=M[r][c]*in[c] indexing would be the transposed
            // (column-vector) convention, inconsistent with how combinedMatrix is populated everywhere else
            // in this codebase.
            float in[4] = {x, y, z, 1.0f};
            float out[4];
            for (int c = 0; c < 4; ++c) {
                out[c] = in[0] * geo.combinedMatrix.m[0][c] + in[1] * geo.combinedMatrix.m[1][c] +
                         in[2] * geo.combinedMatrix.m[2][c] + in[3] * geo.combinedMatrix.m[3][c];
            }
            tv.position[0] = out[0];
            tv.position[1] = out[1];
            tv.position[2] = out[2];
            tv.position[3] = out[3];

            tv.clipFlags = 0;
            float w = out[3];
            if (out[0] > +w) tv.clipFlags |= CLIP_POSX;
            if (out[0] < -w) tv.clipFlags |= CLIP_NEGX;
            if (out[1] > +w) tv.clipFlags |= CLIP_POSY;
            if (out[1] < -w) tv.clipFlags |= CLIP_NEGY;
            if (out[2] > +w) tv.clipFlags |= CLIP_POSZ;
            if (out[2] < -w) tv.clipFlags |= CLIP_NEGZ;

            tv.fogFactor = 0.0f;
            geo.vertexLoaded[vtxIdx] = true;
        }
    }

    void Cmd_QUAD(DListContext& ctx, uint32_t w0, uint32_t w1) {
        // gSP1Quadrangle encoding (F5/StarWars is F3DEX2-derived): w0 holds the first triangle
        // (v0,v1,v2); w1 holds the entire second triangle (v0,v2,v3), not just v3 in isolation. Matches
        // F3DEX2Handler::Cmd_G_QUAD; this handler decodes QUAD itself instead of delegating, so it needs
        // the same two-triangle decode (the missing half of every quad was the root cause of the
        // "shattered glass" skies/backgrounds).
        int v0 = ((w0 >> 16) & 0xFF) / 2;
        int v1 = ((w0 >> 8) & 0xFF) / 2;
        int v2 = (w0 & 0xFF) / 2;
        int v3 = ((w1 >> 16) & 0xFF) / 2;
        int v4 = ((w1 >> 8) & 0xFF) / 2;
        int v5 = (w1 & 0xFF) / 2;

        DrawTriangle2(ctx, v0, v1, v2, v3, v4, v5);
        m_trianglesProcessed += 2;
    }

    void Cmd_SPECIAL(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)ctx;
        (void)w1;
        int subCmd = (w0 >> 16) & 0xFF;

        switch (subCmd) {
        case 0x01:
            // Terrain LOD: toggle compressed vertex format
            m_compressedVtx = (w0 & 0x01) != 0;
            break;
        case 0x02:
            // Fog volume parameters (handled by standard fog in shader)
            break;
        case 0x03:
            // Particle system (alpha-blended point sprites)
            break;
        }
    }

    /// F5 extended SetOtherMode_L: applies a masked update to the low 32 bits
    void Cmd_SETOTHERMODE_L_EX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint32_t shift = (w0 >> 8) & 0xFF;
        uint32_t len = (w0) & 0xFF;
        uint32_t mask = (len < 32) ? ((1u << len) - 1u) << shift : 0xFFFFFFFFu;
        ctx.state->otherModeL = (ctx.state->otherModeL & ~mask) | (w1 & mask);
    }

    /// F5 extended SetOtherMode_H: applies a masked update to the high 32 bits
    void Cmd_SETOTHERMODE_H_EX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint32_t shift = (w0 >> 8) & 0xFF;
        uint32_t len = (w0) & 0xFF;
        uint32_t mask = (len < 32) ? ((1u << len) - 1u) << shift : 0xFFFFFFFFu;
        ctx.state->otherModeH = (ctx.state->otherModeH & ~mask) | (w1 & mask);
    }

    /// F5 display list jump: switch to a new DL within the same segment
    void Cmd_JUMPSWDL(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        uint32_t addr = ctx.ResolveAddress(w1);
        if (addr < ctx.rdramSize) {
            // Replace current DL pointer (jump, not call — doesn't push return)
            ctx.pc = addr;
        }
    }
};

/// L3D (Line3D, original F3D wireframe variant): renders triangles as three line segments
/// (wireframe mode). G_TRI1 decomposes to 3 lines; G_LINE3D draws a line. Vertex indices are
/// divided by 10 (like F3D).

class L3DGBIHandler : public F3DHandler {
public:
    L3DGBIHandler() = default;
    ~L3DGBIHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::L3D; }
    const char* GetName() const override { return "L3D"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xB5: Cmd_LINE3D(ctx, w0, w1); return !ctx.halted;
        case 0xBF: Cmd_TRI1_AsLines(ctx, w0, w1); return !ctx.halted;
        default:   return F3DHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_LINE3D(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        int v0 = ((w1 >> 16) & 0xFF) / 10;
        int v1 = ((w1 >> 8) & 0xFF) / 10;
        // Render line as degenerate triangle
        DrawTriangle(ctx, v0, v1, v0);
        m_trianglesProcessed++;
    }

    void Cmd_TRI1_AsLines(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        int v0 = ((w1 >> 16) & 0xFF) / 10;
        int v1 = ((w1 >> 8) & 0xFF) / 10;
        int v2 = (w1 & 0xFF) / 10;
        // Decompose triangle into 3 lines
        DrawTriangle(ctx, v0, v1, v0);
        DrawTriangle(ctx, v1, v2, v1);
        DrawTriangle(ctx, v2, v0, v2);
        m_trianglesProcessed += 3;
    }
};

/// L3DEX (Line3D EX, F3DEX wireframe variant): same concept as L3D but with F3DEX vertex
/// encoding (/2).

class L3DEXGBIHandler : public F3DEXHandler {
public:
    L3DEXGBIHandler() = default;
    ~L3DEXGBIHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::L3DEX; }
    const char* GetName() const override { return "L3DEX"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xB1: Cmd_TRI2_AsLines(ctx, w0, w1); return !ctx.halted;
        case 0xB5: Cmd_LINE3D(ctx, w0, w1); return !ctx.halted;
        case 0xBF: Cmd_TRI1_AsLines(ctx, w0, w1); return !ctx.halted;
        default:   return F3DEXHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_LINE3D(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        int v0 = ((w1 >> 16) & 0xFF) / 2;
        int v1 = ((w1 >> 8) & 0xFF) / 2;
        DrawTriangle(ctx, v0, v1, v0);
        m_trianglesProcessed++;
    }

    void Cmd_TRI1_AsLines(DListContext& ctx, uint32_t /*w0*/, uint32_t w1) {
        int v0 = ((w1 >> 16) & 0xFF) / 2;
        int v1 = ((w1 >> 8) & 0xFF) / 2;
        int v2 = (w1 & 0xFF) / 2;
        DrawTriangle(ctx, v0, v1, v0);
        DrawTriangle(ctx, v1, v2, v1);
        DrawTriangle(ctx, v2, v0, v2);
        m_trianglesProcessed += 3;
    }

    void Cmd_TRI2_AsLines(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int v0 = ((w0 >> 16) & 0xFF) / 2;
        int v1 = ((w0 >> 8) & 0xFF) / 2;
        int v2 = (w0 & 0xFF) / 2;
        int v3 = ((w1 >> 16) & 0xFF) / 2;
        int v4 = ((w1 >> 8) & 0xFF) / 2;
        int v5 = (w1 & 0xFF) / 2;
        DrawTriangle(ctx, v0, v1, v0);
        DrawTriangle(ctx, v1, v2, v1);
        DrawTriangle(ctx, v2, v0, v2);
        DrawTriangle(ctx, v3, v4, v3);
        DrawTriangle(ctx, v4, v5, v4);
        DrawTriangle(ctx, v5, v3, v5);
        m_trianglesProcessed += 6;
    }
};

/// L3DEX2 (Line3D EX2, F3DEX2 wireframe variant): GBI2 encoding with vertex indices in w0, /2.

class L3DEX2GBIHandler : public F3DEX2Handler {
public:
    L3DEX2GBIHandler() {
        m_type = MicrocodeType::L3DEX2;
        m_name = "L3DEX2";
    }
    ~L3DEX2GBIHandler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0x05: Cmd_TRI1_AsLines(ctx, w0, w1); return !ctx.halted;
        case 0x06: Cmd_TRI2_AsLines(ctx, w0, w1); return !ctx.halted;
        case 0x08: Cmd_LINE3D(ctx, w0, w1); return !ctx.halted;
        default:   return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_LINE3D(DListContext& ctx, uint32_t w0, uint32_t /*w1*/) {
        int v0 = ((w0 >> 16) & 0xFF) / 2;
        int v1 = ((w0 >> 8) & 0xFF) / 2;
        DrawTriangle(ctx, v0, v1, v0);
        m_trianglesProcessed++;
    }

    void Cmd_TRI1_AsLines(DListContext& ctx, uint32_t w0, uint32_t /*w1*/) {
        int v0 = ((w0 >> 16) & 0xFF) / 2;
        int v1 = ((w0 >> 8) & 0xFF) / 2;
        int v2 = (w0 & 0xFF) / 2;
        DrawTriangle(ctx, v0, v1, v0);
        DrawTriangle(ctx, v1, v2, v1);
        DrawTriangle(ctx, v2, v0, v2);
        m_trianglesProcessed += 3;
    }

    void Cmd_TRI2_AsLines(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int v0 = ((w0 >> 16) & 0xFF) / 2;
        int v1 = ((w0 >> 8) & 0xFF) / 2;
        int v2 = (w0 & 0xFF) / 2;
        int v3 = ((w1 >> 16) & 0xFF) / 2;
        int v4 = ((w1 >> 8) & 0xFF) / 2;
        int v5 = (w1 & 0xFF) / 2;
        DrawTriangle(ctx, v0, v1, v0);
        DrawTriangle(ctx, v1, v2, v1);
        DrawTriangle(ctx, v2, v0, v2);
        DrawTriangle(ctx, v3, v4, v3);
        DrawTriangle(ctx, v4, v5, v4);
        DrawTriangle(ctx, v5, v3, v5);
        m_trianglesProcessed += 6;
    }
};

/// F3DSETA (F3D SetA variant): F3D-based but with a different MoveWord index encoding.
/// MW_NUMLIGHT formula: ((w1 - 0x80000000) >> 5) - 1. MW_SEGMENT uses a 10-bit shift. The opcode
/// slots for PERSPNORM/RDPHALF are swapped.

class F3DSETAGBIHandler : public F3DHandler {
public:
    F3DSETAGBIHandler() = default;
    ~F3DSETAGBIHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::F3DSETA; }
    const char* GetName() const override { return "F3DSETA"; }

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xB1: /* G_RDPHALF_CONT in SetA */ return !ctx.halted;
        case 0xB2: /* G_RDPHALF_2 in SetA */ m_rdpHalf2 = w1; return !ctx.halted;
        case 0xB3: /* G_RDPHALF_1 in SetA */ m_rdpHalf1 = w1; return !ctx.halted;
        case 0xB4: /* G_PERSPNORM in SetA (no-op in HLE) */ return !ctx.halted;
        case 0xBC: Cmd_MOVEWORD_SETA(ctx, w0, w1); return !ctx.halted;
        default:   return F3DHandler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_MOVEWORD_SETA(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;

        uint8_t index = (w0 >> 16) & 0xFF;
        switch (index) {
        case 0x00: {
            // MW_NUMLIGHT — custom formula
            int numLights = static_cast<int>(((w1 - 0x80000000u) >> 5) - 1);
            if (numLights < 0) numLights = 0;
            if (numLights >= MAX_LIGHTS) numLights = MAX_LIGHTS - 1;
            geo.numLights = numLights;
            break;
        }
        case 0x02: {
            // MW_CLIP (standard)
            uint16_t offset = w0 & 0xFFFF;
            MoveWord(ctx, 0x04, offset, w1);
            break;
        }
        case 0x04: {
            // MW_SEGMENT — 10-bit shift
            int seg = (w0 >> 10) & 0xF;
            geo.segments[seg] = w1 & 0x00FFFFFF;
            break;
        }
        case 0x06: {
            // MW_FOG (standard encoding)
            MoveWord(ctx, 0x08, 0, w1);
            break;
        }
        case 0x08: {
            // MW_LIGHTCOL (standard)
            uint16_t offset = w0 & 0xFFFF;
            MoveWord(ctx, 0x0A, offset, w1);
            break;
        }
        default:
            // Fall through to standard MoveWord
            MoveWord(ctx, index, w0 & 0xFFFF, w1);
            break;
        }
    }
};

/// F3DEX2 Acclaim (Turok 2/3, South Park): identical to F3DEX2 except that G_MOVEMEM (0xDC)
/// handles lights differently at high offsets, using 16-byte light structs beyond offset 24*4.

class F3DEX2AcclaimGBIHandler : public F3DEX2Handler {
public:
    F3DEX2AcclaimGBIHandler() {
        m_type = MicrocodeType::F3DEX2ACCLAIM;
        m_name = "F3DEX2ACCLAIM";
    }
    ~F3DEX2AcclaimGBIHandler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        switch (cmd) {
        case 0xDC: Cmd_MOVEMEM_Acclaim(ctx, w0, w1); return !ctx.halted;
        default:   return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
        }
    }

private:
    void Cmd_MOVEMEM_Acclaim(DListContext& ctx, uint32_t w0, uint32_t w1) {
        uint8_t index = w0 & 0xFF;
        uint8_t offset = static_cast<uint8_t>(((w0 >> 8) & 0xFF) * 8);

        // For G_MV_LIGHT with high offsets, use Acclaim 16-byte light format
        if (index == 10 && offset > 24 * 3) {
            // Acclaim compact light: 16 bytes instead of standard 24
            uint32_t addr = ctx.ResolveAddress(w1);
            if (!ctx.state || addr + 16 > ctx.rdramSize) return;

            auto& geo = ctx.state->geometry;
            int lightIdx = (offset / 24) - 2;
            if (lightIdx < 0 || lightIdx >= MAX_LIGHTS) return;

            const uint8_t* src = ctx.rdram + addr;
            // Acclaim format: R,G,B,_ (4 bytes color), R2,G2,B2,_ (4 bytes copy),
            //                 nx,ny,nz,_ (4 bytes normal), pad (4 bytes)
            geo.lights[lightIdx].color[0] = static_cast<float>(src[0]) / 255.0f;
            geo.lights[lightIdx].color[1] = static_cast<float>(src[1]) / 255.0f;
            geo.lights[lightIdx].color[2] = static_cast<float>(src[2]) / 255.0f;

            float nx = static_cast<float>(static_cast<int8_t>(src[8])) / 127.0f;
            float ny = static_cast<float>(static_cast<int8_t>(src[9])) / 127.0f;
            float nz = static_cast<float>(static_cast<int8_t>(src[10])) / 127.0f;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 0.0001f) {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            geo.lights[lightIdx].direction[0] = nx;
            geo.lights[lightIdx].direction[1] = ny;
            geo.lights[lightIdx].direction[2] = nz;
        } else {
            // Standard GBI2 MoveMem for everything else
            uint8_t size = static_cast<uint8_t>((((w0 >> 19) & 0x1F) + 1) * 8);
            MoveMemGBI2(ctx, index, size, offset, w1);
        }
    }
};

/// F3DAM (Army Men, F3DEX2 variant): modified vertex loading and MoveWord; uses standard F3DEX2
/// for most commands.

class F3DAMGBIHandler : public F3DEX2Handler {
public:
    F3DAMGBIHandler() {
        m_type = MicrocodeType::F3DAM;
        m_name = "F3DAM";
    }
    ~F3DAMGBIHandler() override = default;

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t opcode = (w0 >> 24) & 0xFF;
        switch (opcode) {
        case 0x01: Cmd_F3DAM_Vtx(ctx, w0, w1); break;
        case 0xD7: Cmd_F3DAM_Texture(ctx, w0, w1); break;
        case 0xDB: Cmd_F3DAM_MoveWord(ctx, w0, w1); break;
        case 0xD9:
            // Geometry mode change — delegate to base then update F3DAM fog mode
            F3DEX2Handler::ProcessCommand(ctx, w0, w1);
            UpdateF3DAMFogUsage(ctx);
            return !ctx.halted;
        default: return F3DEX2Handler::ProcessCommand(ctx, w0, w1);
        }
        return !ctx.halted;
    }

private:
    // F3DAM-specific texture coordinate scaling registers
    uint32_t m_textureCoordScaleOrg = 0;
    uint32_t m_textureCoordScale[2] = {};

    // --- F3DAM fog usage: compute from geometryMode bits 13 and 16 ---
    // GLideN64: fogMode = ((geomMode >> 13) & 9) + 0xFFF8
    //   fogMode == 0  → standard fog (alpha)
    //   fogMode > 0   → F3DAM mode 2 (fog → shade RGB)
    //   fogMode < 0   → no fog (but G_FOG bit may still be set)
    void UpdateF3DAMFogUsage(DListContext& ctx) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;
        int16_t fogMode = static_cast<int16_t>(((geo.geometryMode >> 13) & 9) + 0xFFF8);
        if (fogMode == 0) geo.fogUsage = 1;                          // standard fog
        else if (fogMode > 0) geo.fogUsage = 2;                      // F3DAM mode 2: fog replaces shade RGB
        else geo.fogUsage = (geo.geometryMode & 0x00010000) ? 1 : 0; // G_FOG flag
    }

    // --- F3DAM MoveWord: extends MW_FOG with texcoord scale registers ---
    void Cmd_F3DAM_MoveWord(DListContext& ctx, uint32_t w0, uint32_t w1) {
        uint8_t index = (w0 >> 16) & 0xFF;
        uint16_t offset = w0 & 0xFFFF;

        if (index == MoveWord::MW_FOG) {
            uint8_t subOffset = w0 & 0xFF;
            switch (subOffset) {
            case 0x00: // Standard fog factor
                MoveWord(ctx, index, offset, w1);
                return;
            case 0x0C: m_textureCoordScale[0] = w1; return;
            case 0x10: m_textureCoordScale[1] = w1; return;
            }
        }
        // Fallthrough for all other MoveWord types
        MoveWord(ctx, index, offset, w1);
    }

    // --- F3DAM Texture: stores org, sets scaleS/T = 1.0 (0xFFFF) ---
    void Cmd_F3DAM_Texture(DListContext& ctx, uint32_t w0, uint32_t w1) {
        m_textureCoordScaleOrg = w1;

        // Build modified w1 with scaleS/T = 0xFFFF (1.0 in 0.16 fixed-point)
        // Keep level, tile, on bits from w0 intact
        uint32_t modW1 = 0xFFFF0000u | 0x0000FFFFu;
        SetTextureState(ctx, w0, modW1);
    }

    // --- F3DAM Vertex: custom texcoord computation ---
    void Cmd_F3DAM_Vtx(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;

        int count = (w0 >> 12) & 0xFF;
        int vbidx = ((w0 >> 1) & 0x7F) - count;
        if (vbidx < 0) vbidx = 0;

        uint32_t address = ctx.ResolveAddress(w1);

        if (vbidx + count > VERTEX_BUFFER_SIZE) {
            count = VERTEX_BUFFER_SIZE - vbidx;
            if (count <= 0) return;
        }

        bool lighting = (geo.geometryMode & G_LIGHTING) != 0;

        for (int i = 0; i < count; ++i) {
            uint32_t vtxAddr = address + i * 16;
            if (vtxAddr + 16 > ctx.rdramSize) break;

            int16_t x = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 0));
            int16_t y = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 2));
            int16_t z = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 4));
            uint16_t rawS = ctx.ReadHalf(vtxAddr + 8);
            uint16_t rawT = ctx.ReadHalf(vtxAddr + 10);
            uint8_t cn0 = ctx.ReadByte(vtxAddr + 12);
            uint8_t cn1 = ctx.ReadByte(vtxAddr + 13);
            uint8_t cn2 = ctx.ReadByte(vtxAddr + 14);
            uint8_t cn3 = ctx.ReadByte(vtxAddr + 15);

            TransformedVertex& tv = geo.vertexBuffer[vbidx + i];
            tv.x = static_cast<float>(x);
            tv.y = static_cast<float>(y);
            tv.z = static_cast<float>(z);
            tv.w = 1.0f;

            // F3DAM texture coordinate scaling
            CalcF3DAMTexCoords(rawS, rawT, tv);

            if (lighting) {
                tv.nx = static_cast<float>(static_cast<int8_t>(cn0)) / 127.0f;
                tv.ny = static_cast<float>(static_cast<int8_t>(cn1)) / 127.0f;
                tv.nz = static_cast<float>(static_cast<int8_t>(cn2)) / 127.0f;
                tv.a = static_cast<float>(cn3) / 255.0f;
            } else {
                tv.r = static_cast<float>(cn0) / 255.0f;
                tv.g = static_cast<float>(cn1) / 255.0f;
                tv.b = static_cast<float>(cn2) / 255.0f;
                tv.a = static_cast<float>(cn3) / 255.0f;
            }

            tv.texCoord1[0] = tv.s;
            tv.texCoord1[1] = tv.t;
            tv.clipFlags = 0;
            tv.fogFactor = 0.0f;
            geo.vertexLoaded[vbidx + i] = true;
        }

        geo.UpdateCombinedMatrix();
        TransformVertexBatch(geo, vbidx, count);
        m_verticesLoaded += count;
    }

    // --- F3DAM texcoord calculation (matches GLideN64 calcF3DAMTexCoords) ---
    void CalcF3DAMTexCoords(uint16_t rawS, uint16_t rawT, TransformedVertex& tv) const {
        uint32_t s0 = rawS;
        uint32_t t0 = rawT;

        uint32_t acum_0 = (((m_textureCoordScaleOrg & 0xFFFF) * t0) << 1) + 0x8000;
        uint32_t acum_1 = (((m_textureCoordScale[1] & 0xFFFF) * t0) << 1) + 0x8000;
        uint32_t sres = ((((m_textureCoordScaleOrg >> 16) & 0xFFFF) * s0) << 1) + acum_0;
        uint32_t tres = ((((m_textureCoordScale[1] >> 16) & 0xFFFF) * s0) << 1) + acum_1;

        int16_t finalS =
            static_cast<int16_t>((sres >> 16) & 0xFFFF) + static_cast<int16_t>((m_textureCoordScale[0] >> 16) & 0xFFFF);
        int16_t finalT =
            static_cast<int16_t>((tres >> 16) & 0xFFFF) + static_cast<int16_t>(m_textureCoordScale[0] & 0xFFFF);

        tv.s = static_cast<float>(finalS) * (1.0f / 32.0f);
        tv.t = static_cast<float>(finalT) * (1.0f / 32.0f);
    }
};

/// ZSortBOSS (Boss Game Studios ZSort variant), used by World Driver Championship and Stunt
/// Racer 64. Features: dual display list (main + sub) with signal-based synchronisation,
/// DMEM-based vertex transform, custom lighting, z-sorted object rendering, matrix
/// concatenation, and audio DSP.

class ZSortBOSSGBIHandler : public GBIHandler {
public:
    ZSortBOSSGBIHandler() = default;
    ~ZSortBOSSGBIHandler() override = default;

    MicrocodeType GetType() const override { return MicrocodeType::ZSortBOSS; }
    GBIVersion GetGBIVersion() const override { return GBIVersion::GBI1; }
    const char* GetName() const override { return "ZSortBOSS"; }

    void Initialize(const MicrocodeInfo& info) override {
        (void)info;
        ResetStats();
        ResetState();
    }

    void OnFrameBegin(DListContext& ctx) override {
        (void)ctx;
        ResetState();
    }

    // ZSortBOSS opcode table (values from GLideN64):
    //  0x00: SPNOOP           0x02: EndMainDL       0x04: MoveMem
    //  0x06: MoveWord         0x08: TransposeMTX    0x0A: MTXCAT
    //  0x0C: MultMPMTX        0x0E: RDPCMD          0x10: Obj
    //  0x12: WaitSignal       0x14: Lighting        0x18: TransformLights
    //  0x1A: EndSubDL         0x1C: Audio2          0x1E: ClearBuffer
    //  0x22: Audio3           0x24: Audio4          0x26: Audio1
    //  0xDD: UpdateMask       0xDE: TriangleCommand 0xDF: FlushRDPCmdBuffer
    //  0xE1: RDPHALF_1        0xE2: SetOtherMode_L  0xE3: SetOtherMode_H
    //  0xEF: SetOtherMode     0xF1: RDPHALF_2
    //  0xC0+: RDP passthrough

    bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) override {
        uint8_t cmd = (w0 >> 24) & 0xFF;
        m_commandsProcessed++;

        switch (cmd) {
        case 0x00: /* SPNOOP */ return !ctx.halted;
        case 0x02: Cmd_EndMainDL(ctx, w0, w1); return !ctx.halted;
        case 0x04: Cmd_MoveMem(ctx, w0, w1); return !ctx.halted;
        case 0x06: Cmd_MoveWord(ctx, w0, w1); return !ctx.halted;
        case 0x08: Cmd_TransposeMTX(ctx, w0, w1); return !ctx.halted;
        case 0x0A: Cmd_MTXCAT(ctx, w0, w1); return !ctx.halted;
        case 0x0C: Cmd_MultMPMTX(ctx, w0, w1); return !ctx.halted;
        case 0x0E: HandleRDPCommand(ctx, w0, w1); return !ctx.halted;
        case 0x10: Cmd_Obj(ctx, w0, w1); return !ctx.halted;
        case 0x12: /* WaitSignal — no-op in HLE */ return !ctx.halted;
        case 0x14: Cmd_Lighting(ctx, w0, w1); return !ctx.halted;
        case 0x18: Cmd_TransformLights(ctx, w0, w1); return !ctx.halted;
        case 0x1A: Cmd_EndSubDL(ctx, w0, w1); return !ctx.halted;
        case 0x1C: /* Audio2 — audio DSP, ignored */ return !ctx.halted;
        case 0x1E: Cmd_ClearBuffer(ctx, w0, w1); return !ctx.halted;
        case 0x22: /* Audio3 — audio DSP, ignored */ return !ctx.halted;
        case 0x24: /* Audio4 — audio DSP, ignored */ return !ctx.halted;
        case 0x26: /* Audio1 — audio DSP, ignored */ return !ctx.halted;
        case 0xDD: Cmd_UpdateMask(ctx, w0, w1); return !ctx.halted;
        case 0xDE: Cmd_TriangleCommand(ctx, w0, w1); return !ctx.halted;
        case 0xDF: /* FlushRDPCmdBuffer — no-op */ return !ctx.halted;
        case 0xE1: m_rdpHalf1 = w1; return !ctx.halted;
        case 0xE2: Cmd_SetOtherMode_L(ctx, w0, w1); return !ctx.halted;
        case 0xE3: Cmd_SetOtherMode_H(ctx, w0, w1); return !ctx.halted;
        case 0xEF: Cmd_SetOtherMode(ctx, w0, w1); return !ctx.halted;
        case 0xF1: m_rdpHalf2 = w1; return !ctx.halted;
        default:
            if (cmd >= 0xC0) {
                HandleRDPCommand(ctx, w0, w1);
            } else {
                NOVA_LOG(LogLevel::Debug, LogSubsystem::Microcode,
                         "ZSortBOSS: Unknown opcode 0x{:02X} (w0=0x{:08X}, w1=0x{:08X})", cmd, w0, w1);
            }
            return !ctx.halted;
        }
    }

private:
    // --- Internal state (emulates part of DMEM) ---
    enum DLState : uint8_t { DEFAULT_DL = 0, PROCESSED_DL = 1 };

    struct BOSSState {
        DLState mainDL = DEFAULT_DL;
        DLState subDL = DEFAULT_DL;
        uint32_t updateMask[2] = {};
        float viewScale[2] = {};
        float viewTrans[2] = {};
        uint32_t rdpCmds[3] = {};
        float invwFactor = 10.0f;
        uint8_t fogTable[256] = {};
        int16_t table[8][8] = {};

        // DMEM buffer for vertex data, normals, etc.
        static constexpr uint32_t DMEM_SIZE = 4096;
        uint8_t dmem[DMEM_SIZE] = {};
    };
    BOSSState m_boss;

    // Lookat vectors for texgen
    float m_lookat[2][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};

    void ResetState() {
        m_boss = BOSSState{};
        m_boss.invwFactor = 10.0f;
        m_lookat[0][0] = 1.0f;
        m_lookat[0][1] = 0.0f;
        m_lookat[0][2] = 0.0f;
        m_lookat[1][0] = 0.0f;
        m_lookat[1][1] = 1.0f;
        m_lookat[1][2] = 0.0f;
    }

    // --- Helper: read s16 from DMEM with XOR swap ---
    int16_t DmemReadS16(uint32_t addr) const {
        addr &= (BOSSState::DMEM_SIZE - 1);
        uint32_t swapped = addr ^ 2;
        return static_cast<int16_t>((static_cast<uint16_t>(m_boss.dmem[swapped]) << 8) |
                                    static_cast<uint16_t>(m_boss.dmem[swapped + 1]));
    }

    void DmemWriteS16(uint32_t addr, int16_t val) {
        addr &= (BOSSState::DMEM_SIZE - 1);
        uint32_t swapped = addr ^ 2;
        m_boss.dmem[swapped] = static_cast<uint8_t>((val >> 8) & 0xFF);
        m_boss.dmem[swapped + 1] = static_cast<uint8_t>(val & 0xFF);
    }

    int8_t DmemReadS8(uint32_t addr) const {
        return static_cast<int8_t>(m_boss.dmem[(addr ^ 3) & (BOSSState::DMEM_SIZE - 1)]);
    }

    uint32_t DmemReadU32(uint32_t addr) const {
        addr &= (BOSSState::DMEM_SIZE - 1);
        return (static_cast<uint32_t>(m_boss.dmem[addr]) << 24) | (static_cast<uint32_t>(m_boss.dmem[addr + 1]) << 16) |
               (static_cast<uint32_t>(m_boss.dmem[addr + 2]) << 8) | static_cast<uint32_t>(m_boss.dmem[addr + 3]);
    }

    // --- Resolve matrix pointer from DMEM offset ---
    // 0x830 = model matrix, 0x870 = projection, 0x8b0 = combined
    Matrix4x4* ResolveMtx(DListContext& ctx, uint32_t dmemAddr) {
        if (!ctx.state) return nullptr;
        auto& geo = ctx.state->geometry;
        switch (dmemAddr & 0xFFF) {
        case 0x830: return &geo.modelViewStack[geo.modelViewStackDepth];
        case 0x870: return &geo.projectionStack[geo.projectionStackDepth];
        case 0x8b0: return &geo.combinedMatrix;
        default:    return nullptr;
        }
    }

    // --- Helper: fixed-point to float ---
    static float Fixed2Float(int16_t val, int fracBits) {
        return static_cast<float>(val) / static_cast<float>(1 << fracBits);
    }
    static float FixedColor(uint8_t val) { return static_cast<float>(val) / 255.0f; }

    // --- Helper: Calc_invw (from GLideN64) ---
    static float CalcInvW(int _w) {
        if (_w == 0) return 1.0f;
        // The N64 RSP uses a reciprocal LUT; approximate with 1.0/w
        return 1.0f / static_cast<float>(_w);
    }

    // --- Helper: Store matrix to RDRAM in N64 format ---
    static void StoreMatrixToRDRAM(uint8_t* rdram, uint32_t addr, const Matrix4x4& mtx) {
        // N64 matrix format: 32 bytes of integer parts, 32 bytes of fraction parts
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                float val = mtx.m[i][j];
                int32_t fixedVal = static_cast<int32_t>(val * 65536.0f);
                int16_t intPart = static_cast<int16_t>(fixedVal >> 16);
                uint16_t fracPart = static_cast<uint16_t>(fixedVal & 0xFFFF);
                uint32_t intOff = addr + (i * 8) + ((j ^ 1) * 2);
                uint32_t fracOff = addr + 32 + (i * 8) + ((j ^ 1) * 2);
                rdram[intOff] = static_cast<uint8_t>((intPart >> 8) & 0xFF);
                rdram[intOff + 1] = static_cast<uint8_t>(intPart & 0xFF);
                rdram[fracOff] = static_cast<uint8_t>((fracPart >> 8) & 0xFF);
                rdram[fracOff + 1] = static_cast<uint8_t>(fracPart & 0xFF);
            }
        }
    }

    // --- Helper: transform & normalize vector by 3x3 ---
    static void TransformVectorNormalize(float vec[3], const Matrix4x4& mtx) {
        float vr[3];
        vr[0] = mtx.m[0][0] * vec[0] + mtx.m[1][0] * vec[1] + mtx.m[2][0] * vec[2];
        vr[1] = mtx.m[0][1] * vec[0] + mtx.m[1][1] * vec[1] + mtx.m[2][1] * vec[2];
        vr[2] = mtx.m[0][2] * vec[0] + mtx.m[1][2] * vec[1] + mtx.m[2][2] * vec[2];
        float len = vr[0] * vr[0] + vr[1] * vr[1] + vr[2] * vr[2];
        float recip = (len > 0.0f) ? (1.0f / std::sqrt(len)) : 256.0f;
        if (recip > 256.0f) recip = 256.0f;
        vec[0] = vr[0] * recip;
        vec[1] = vr[1] * recip;
        vec[2] = vr[2] * recip;
    }

    // --- Dot product ---
    static float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // --- Command implementations ---

    void Cmd_EndMainDL(DListContext& ctx, uint32_t, uint32_t) {
        // In the real microcode, this signals between main/sub DLs
        // via SP_STATUS. In HLE we simply end the DL.
        if (m_boss.subDL == PROCESSED_DL) {
            ctx.halted = true;
            m_boss.mainDL = DEFAULT_DL;
            m_boss.subDL = DEFAULT_DL;
        } else {
            m_boss.mainDL = PROCESSED_DL;
            // In HLE, we don't have dual DL support — just halt
            ctx.halted = true;
        }
    }

    void Cmd_EndSubDL(DListContext& ctx, uint32_t, uint32_t) {
        if (m_boss.mainDL == PROCESSED_DL) {
            ctx.halted = true;
            m_boss.mainDL = DEFAULT_DL;
            m_boss.subDL = DEFAULT_DL;
        } else {
            m_boss.subDL = PROCESSED_DL;
            ctx.halted = true;
        }
    }

    void Cmd_MoveWord(DListContext& ctx, uint32_t w0, uint32_t w1) {
        (void)ctx;
        uint32_t dmemOff = w0 & 0xFFF;
        if (dmemOff == 0x10 && (/* next cmd check not needed in HLE */ true)) {
            m_boss.invwFactor = static_cast<float>(w1);
        }
        // Write to DMEM emulation
        if (dmemOff + 4 <= BOSSState::DMEM_SIZE) {
            m_boss.dmem[dmemOff] = static_cast<uint8_t>((w1 >> 24) & 0xFF);
            m_boss.dmem[dmemOff + 1] = static_cast<uint8_t>((w1 >> 16) & 0xFF);
            m_boss.dmem[dmemOff + 2] = static_cast<uint8_t>((w1 >> 8) & 0xFF);
            m_boss.dmem[dmemOff + 3] = static_cast<uint8_t>(w1 & 0xFF);
        }
    }

    void Cmd_ClearBuffer(DListContext& ctx, uint32_t, uint32_t) {
        (void)ctx;
        // Clear DMEM 0xC20..0xE20 (512 bytes)
        uint32_t start = 0xC20;
        uint32_t end = 0xE20;
        if (end <= BOSSState::DMEM_SIZE) {
            std::memset(m_boss.dmem + start, 0, end - start);
        }
    }

    void Cmd_MoveMem(DListContext& ctx, uint32_t w0, uint32_t w1) {
        int flag = (w0 >> 23) & 0x01;
        int len = 1 + ((w0 >> 12) & 0x7FF);
        uint32_t addr = ctx.ResolveAddress(w1);
        uint32_t dmemOff = w0 & 0xFFF;

        // Model matrix
        if (dmemOff == 0x830 && ctx.state) {
            auto& geo = ctx.state->geometry;
            LoadMatrix(ctx, w1, false, true, false);
            geo.combinedDirty = true;
            return;
        }
        // Projection matrix
        if (dmemOff == 0x870 && ctx.state) {
            auto& geo = ctx.state->geometry;
            LoadMatrix(ctx, w1, false, true, true);
            geo.combinedDirty = true;
            return;
        }
        // Combined matrix
        if (dmemOff == 0x8b0 && ctx.state) {
            auto& geo = ctx.state->geometry;
            if (flag == 0) {
                // Load from RDRAM
                geo.combinedMatrix.LoadFromN64(ctx.rdram, addr);
                geo.combinedDirty = false;
            } else {
                // Store to RDRAM
                if (addr + 64 <= ctx.rdramSize) {
                    StoreMatrixToRDRAM(const_cast<uint8_t*>(ctx.rdram), addr, geo.combinedMatrix);
                }
            }
            return;
        }
        // Viewport
        if (dmemOff == 0x0 && ctx.state) {
            auto& geo = ctx.state->geometry;
            if (addr + 16 > ctx.rdramSize) return;
            const uint8_t* src = ctx.rdram + addr;

            // Read viewport as big-endian s16 pairs
            auto ReadS16 = [src](uint32_t off) -> int16_t {
                return static_cast<int16_t>((src[off] << 8) | src[off + 1]);
            };

            float scaleX = Fixed2Float(ReadS16(0), 2);
            float scaleY = Fixed2Float(ReadS16(2), 2);
            float scaleZ = Fixed2Float(ReadS16(4), 10);
            int16_t fogMul = ReadS16(6);
            float transX = Fixed2Float(ReadS16(8), 2);
            float transY = Fixed2Float(ReadS16(10), 2);
            float transZ = Fixed2Float(ReadS16(12), 10);
            int16_t fogOff = ReadS16(14);

            geo.fogMultiplier = fogMul;
            geo.fogOffset = fogOff;

            geo.viewport.scaleX = scaleX;
            geo.viewport.scaleY = -scaleY; // negate: N64 Y-down → GL Y-up
            geo.viewport.scaleZ = scaleZ;
            geo.viewport.transX = transX;
            geo.viewport.transY = transY;
            geo.viewport.transZ = transZ;

            m_boss.viewScale[0] = scaleX * 4.0f;
            m_boss.viewScale[1] = scaleY * 4.0f;
            m_boss.viewTrans[0] = transX * 4.0f;
            m_boss.viewTrans[1] = transY * 4.0f;
            return;
        }
        // Fog table
        if (dmemOff == 0x730 && len == 256) {
            if (addr + 256 <= ctx.rdramSize) {
                std::memcpy(m_boss.fogTable, ctx.rdram + addr, 256);
            }
            return;
        }

        // Generic RDRAM ↔ DMEM copy
        if (flag == 0) {
            // RDRAM → DMEM
            if (addr + static_cast<uint32_t>(len) <= ctx.rdramSize &&
                dmemOff + static_cast<uint32_t>(len) <= BOSSState::DMEM_SIZE) {
                std::memcpy(m_boss.dmem + dmemOff, ctx.rdram + addr, len);
            }
        } else {
            // DMEM → RDRAM
            if (dmemOff + static_cast<uint32_t>(len) <= BOSSState::DMEM_SIZE &&
                addr + static_cast<uint32_t>(len) <= ctx.rdramSize) {
                std::memcpy(const_cast<uint8_t*>(ctx.rdram) + addr, m_boss.dmem + dmemOff, len);
            }
        }
    }

    void Cmd_MTXCAT(DListContext& ctx, uint32_t w0, uint32_t w1) {
        uint32_t S = (w1 >> 16) & 0xFFF;
        uint32_t T = w0 & 0xFFF;
        uint32_t D = w1 & 0xFFF;

        Matrix4x4* srcS = ResolveMtx(ctx, S);
        Matrix4x4* srcT = ResolveMtx(ctx, T);
        Matrix4x4* dst = ResolveMtx(ctx, D);
        if (!srcS || !srcT || !dst) return;

        *dst = (*srcS) * (*srcT);

        if (ctx.state) ctx.state->geometry.combinedDirty = true;
    }

    void Cmd_TransposeMTX(DListContext& ctx, uint32_t, uint32_t w1) {
        Matrix4x4* mtx = ResolveMtx(ctx, w1 & 0xFFF);
        if (!mtx) return;

        Matrix4x4 tmp = *mtx;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) mtx->m[j][i] = tmp.m[i][j];
    }

    void Cmd_MultMPMTX(DListContext& ctx, uint32_t w0, uint32_t w1) {
        // Transform vertices by combined matrix, store results in DMEM
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;
        geo.UpdateCombinedMatrix();

        int num = 1 + ((w1 >> 24) & 0xFF);
        uint32_t srcDmem = (w1 >> 12) & 0xFFF;
        uint32_t dstDmem = w1 & 0xFFF;
        (void)w0;

        struct ZSortVDest {
            int16_t xi, yi, wi;
            int16_t pad;
            int16_t sx, sy;
            uint16_t invw;
            uint8_t fog, cc;
        };

        for (int i = 0; i < num; i++) {
            // Read 3 x s16 from DMEM
            float sx = static_cast<float>(DmemReadS16(srcDmem));
            float sy = static_cast<float>(DmemReadS16(srcDmem + 2));
            float sz = static_cast<float>(DmemReadS16(srcDmem + 4));
            srcDmem += 6;

            const auto& cm = geo.combinedMatrix;
            float x = sx * cm.m[0][0] + sy * cm.m[1][0] + sz * cm.m[2][0] + cm.m[3][0];
            float y = sx * cm.m[0][1] + sy * cm.m[1][1] + sz * cm.m[2][1] + cm.m[3][1];
            float z = sx * cm.m[0][2] + sy * cm.m[1][2] + sz * cm.m[2][2] + cm.m[3][2];
            float w = sx * cm.m[0][3] + sy * cm.m[1][3] + sz * cm.m[2][3] + cm.m[3][3];

            uint16_t invw_val = static_cast<uint16_t>(CalcInvW(static_cast<int>(w * m_boss.invwFactor)) * 65536.0f);
            (void)invw_val;

            float invw = (w <= 0.0f) ? m_boss.invwFactor : (1.0f / w);
            float xClamp = std::max(-m_boss.invwFactor, std::min(x * invw, m_boss.invwFactor));
            float yClamp = std::max(-m_boss.invwFactor, std::min(y * invw, m_boss.invwFactor));

            int16_t screenX = static_cast<int16_t>(m_boss.viewTrans[0] + xClamp * m_boss.viewScale[0]);
            int16_t screenY = static_cast<int16_t>(m_boss.viewTrans[1] + yClamp * m_boss.viewScale[1]);

            int fog = static_cast<int>(w * Fixed2Float(geo.fogMultiplier, 16) + geo.fogOffset);
            fog = std::max(-128, std::min(127, fog));
            // The fog value is looked up but not used
            uint8_t fogVal = m_boss.fogTable[fog + 128];
            (void)fogVal;

            [[maybe_unused]] uint8_t cc = 0;
            if (x >= w) cc |= 0x10;
            if (y >= w) cc |= 0x20;
            if (z >= w) cc |= 0x40;
            if (x <= -w) cc |= 0x01;
            if (y <= -w) cc |= 0x02;
            if (z <= -w) cc |= 0x04;

            // Write ZSortVDest (12 bytes) to DMEM at dstDmem
            uint32_t d = dstDmem + i * 12;
            DmemWriteS16(d + 0, static_cast<int16_t>(x));
            DmemWriteS16(d + 2, static_cast<int16_t>(y));
            DmemWriteS16(d + 4, static_cast<int16_t>(w));
            DmemWriteS16(d + 6, 0);
            DmemWriteS16(d + 8, screenX);
            // Pack sy, invw, fog, cc into remaining DMEM
            if (d + 12 <= BOSSState::DMEM_SIZE) {
                DmemWriteS16(d + 10, screenY);
            }
        }
    }

    // Object drawing types
    static constexpr uint32_t ZH_NULL = 0;
    static constexpr uint32_t ZH_TXTRI = 4;
    static constexpr uint32_t ZH_TXQUAD = 8;

    void DrawZSortObject(DListContext& ctx, const uint8_t* addr, uint32_t type) {
        if (!ctx.state || !ctx.rdpProcessor) return;

        uint32_t vnum = 0, vsize = 16;
        bool textured = true;
        switch (type) {
        case ZH_NULL:   return;
        case ZH_TXTRI:  vnum = 3; break;
        case ZH_TXQUAD: vnum = 4; break;
        default:        return;
        }

        // Build screen-space rectangles as DecodedRectangle or use DrawTriangleRaw
        // ZSortBOSS draws pre-transformed screen-space vertices
        auto& geo = ctx.state->geometry;
        for (uint32_t i = 0; i < vnum; i++) {
            auto& vtx = geo.vertexBuffer[i];
            vtx.x = Fixed2Float(static_cast<int16_t>((addr[0] << 8) | addr[1]), 2);
            vtx.y = Fixed2Float(static_cast<int16_t>((addr[2] << 8) | addr[3]), 2);
            vtx.z = 0.0f;
            vtx.w = 1.0f;
            vtx.r = FixedColor(addr[4 ^ 3]);
            vtx.g = FixedColor(addr[5 ^ 3]);
            vtx.b = FixedColor(addr[6 ^ 3]);
            vtx.a = FixedColor(addr[7 ^ 3]);
            vtx.clipFlags = 0;

            if (textured) {
                vtx.s = Fixed2Float(static_cast<int16_t>((addr[8] << 8) | addr[9]), 5);
                vtx.t = Fixed2Float(static_cast<int16_t>((addr[10] << 8) | addr[11]), 5);

                int32_t invwRaw =
                    static_cast<int32_t>((addr[12] << 24) | (addr[13] << 16) | (addr[14] << 8) | addr[15]);
                int32_t rgbaRaw = static_cast<int32_t>((addr[4] << 24) | (addr[5] << 16) | (addr[6] << 8) | addr[7]);

                if (invwRaw == rgbaRaw || invwRaw < 0) vtx.w = 1.0f;
                else vtx.w = CalcInvW(invwRaw) / m_boss.invwFactor;
            }

            vtx.screenX = vtx.x;
            vtx.screenY = vtx.y;
            vtx.screenZ = vtx.z;
            vtx.invW = 1.0f / vtx.w;
            vtx.reject = false;
            geo.vertexLoaded[i] = true;

            addr += vsize;
        }

        // Draw as screen-space triangles
        DrawTriangle(ctx, 0, 1, 2);
        m_trianglesProcessed++;
        if (vnum == 4) {
            DrawTriangle(ctx, 0, 2, 3);
            m_trianglesProcessed++;
        }
    }

    uint32_t LoadObject(DListContext& ctx, uint32_t zHeader) {
        uint32_t type = (zHeader & 7) << 1;
        uint32_t objAddr = zHeader & 0xFFFFFFF8;
        objAddr &= 0x00FFFFFF;
        if (objAddr + 16 > ctx.rdramSize) return 0;

        // First 4 words: next pointer, then 3 RDP command words
        uint32_t w1 = ctx.ReadWord(objAddr + 4);
        if (w1 != m_boss.rdpCmds[0]) {
            m_boss.rdpCmds[0] = w1;
            HandleRDPCommand(ctx, 0, w1);
        }
        w1 = ctx.ReadWord(objAddr + 8);
        if (w1 != m_boss.rdpCmds[1]) {
            m_boss.rdpCmds[1] = w1;
            HandleRDPCommand(ctx, 0, w1);
        }
        w1 = ctx.ReadWord(objAddr + 12);
        if (w1 != m_boss.rdpCmds[2]) {
            m_boss.rdpCmds[2] = w1;
            HandleRDPCommand(ctx, 0, w1);
        }

        if (type != ZH_NULL) {
            uint32_t vtxDataOff = objAddr + 16;
            uint32_t numBytes = (type == ZH_TXTRI) ? 48 : 64; // 3 or 4 verts × 16 bytes
            if (vtxDataOff + numBytes <= ctx.rdramSize) {
                DrawZSortObject(ctx, ctx.rdram + vtxDataOff, type);
            }
        }

        // Return next object (pointer in first word, resolved)
        uint32_t next = ctx.ReadWord(objAddr);
        return (next != 0) ? ctx.ResolveAddress(next) : 0;
    }

    void Cmd_Obj(DListContext& ctx, uint32_t w0, uint32_t w1) {
        uint32_t zH0 = ctx.ResolveAddress(w0 & 0x00FFFFFF);
        while (zH0 != 0 && zH0 < ctx.rdramSize) zH0 = LoadObject(ctx, zH0);

        uint32_t zH1 = ctx.ResolveAddress(w1);
        while (zH1 != 0 && zH1 < ctx.rdramSize) zH1 = LoadObject(ctx, zH1);
    }

    void Cmd_Lighting(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;

        uint32_t num = 1 + ((w1 >> 24) & 0xFF);
        uint32_t nsrs = w0 & 0xFFF;
        uint32_t tdest = (w1 & 0xFFF) >> 1;

        for (uint32_t i = 0; i < num; i++) {
            float nx = Fixed2Float(DmemReadS8(nsrs++), 8);
            float ny = Fixed2Float(DmemReadS8(nsrs++), 8);
            float nz = Fixed2Float(DmemReadS8(nsrs++), 8);

            float fDir[3] = {nx, ny, nz};
            float s = (Dot3(m_lookat[0], fDir) + 0.5f) * 1024.0f;
            float t = (Dot3(m_lookat[1], fDir) + 0.5f) * 1024.0f;

            DmemWriteS16(tdest * 2, static_cast<int16_t>(s));
            tdest++;
            DmemWriteS16(tdest * 2, static_cast<int16_t>(t));
            tdest++;
        }
    }

    void Cmd_TransformLights(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;
        Matrix4x4* mtx = ResolveMtx(ctx, w0 & 0xFFF);
        if (!mtx) return;

        uint32_t addr = w1 & 0xFFF;
        int numLights = 1 - static_cast<int>((w1 >> 12) & 0xFFF);
        if (numLights < 0) numLights = 0;

        // Process directional lights
        for (int i = 0; i < numLights && i < MAX_LIGHTS; i++) {
            if (addr + 24 > BOSSState::DMEM_SIZE) break;
            if (i < MAX_LIGHTS) {
                geo.lights[i].color[0] = FixedColor(m_boss.dmem[(addr + 8) ^ 3]);
                geo.lights[i].color[1] = FixedColor(m_boss.dmem[(addr + 9) ^ 3]);
                geo.lights[i].color[2] = FixedColor(m_boss.dmem[(addr + 10) ^ 3]);
                float dir[3];
                dir[0] = Fixed2Float(DmemReadS8(addr + 16), 8);
                dir[1] = Fixed2Float(DmemReadS8(addr + 17), 8);
                dir[2] = Fixed2Float(DmemReadS8(addr + 18), 8);
                TransformVectorNormalize(dir, *mtx);
                geo.lights[i].direction[0] = dir[0];
                geo.lights[i].direction[1] = dir[1];
                geo.lights[i].direction[2] = dir[2];
            }
            addr += 24;
        }

        // Process lookat vectors (2)
        for (int i = 0; i < 2; i++) {
            if (addr + 24 > BOSSState::DMEM_SIZE) break;
            m_lookat[i][0] = Fixed2Float(DmemReadS8(addr + 16), 8);
            m_lookat[i][1] = Fixed2Float(DmemReadS8(addr + 17), 8);
            m_lookat[i][2] = Fixed2Float(DmemReadS8(addr + 18), 8);
            TransformVectorNormalize(m_lookat[i], *mtx);
            addr += 24;
        }

        geo.numLights = numLights;
    }

    void Cmd_UpdateMask(DListContext&, uint32_t w0, uint32_t w1) {
        m_boss.updateMask[0] = w0 | 0xFF000000;
        m_boss.updateMask[1] = w1;
    }

    void Cmd_TriangleCommand(DListContext& ctx, uint32_t, uint32_t w1) {
        if (!ctx.state) return;
        auto& geo = ctx.state->geometry;
        // Set texture state for triangle rendering
        geo.texture.scaleS = 0xFFFF;
        geo.texture.scaleT = 0xFFFF;
        geo.texture.level = (w1 >> 3) & 0x7;
        geo.texture.enabled = true;
        geo.texture.tileIndex = w1 & 0x7;
        // Enable smooth shading
        geo.geometryMode |= 0x00200000; // G_SHADING_SMOOTH | G_SHADE
    }

    void Cmd_SetOtherMode_L(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint32_t shift = w0 & 0x1F;
        uint32_t shr = (w0 >> 8) & 0x1F;
        uint32_t mask =
            (shift == 0) ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(0x80000000) / (1 << shift));
        mask >>= shr;
        ctx.state->otherModeL = (ctx.state->otherModeL & ~mask) | w1;
    }

    void Cmd_SetOtherMode_H(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        uint32_t shift = w0 & 0x1F;
        uint32_t shr = (w0 >> 8) & 0x1F;
        uint32_t mask =
            (shift == 0) ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(0x80000000) / (1 << shift));
        mask >>= shr;
        ctx.state->otherModeH = (ctx.state->otherModeH & ~mask) | w1;
    }

    void Cmd_SetOtherMode(DListContext& ctx, uint32_t w0, uint32_t w1) {
        if (!ctx.state) return;
        ctx.state->otherModeH = (w0 & m_boss.updateMask[0]) | (ctx.state->otherModeH & ~m_boss.updateMask[0]);
        ctx.state->otherModeL = (w1 & m_boss.updateMask[1]) | (ctx.state->otherModeL & ~m_boss.updateMask[1]);
    }
};

} // namespace Nova64::Video
