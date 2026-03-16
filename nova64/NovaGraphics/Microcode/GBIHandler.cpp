/**
 * @file GBIHandler.cpp
 * @brief Base GBI command handler implementation
 *
 * Implements shared command handling logic used by all microcode variants.
 * Individual microcode handlers override ProcessCommand to provide their
 * own opcode dispatch, but delegate shared operations here.
 */

#include "GBIHandler.hpp"
#include "MicrocodeDispatcher.hpp"
#include "../Core/RDPUtils.hpp"
#include "../Core/N64Types.hpp"
#include "../Common/Logger.hpp"
#include "../Common/ClipMath.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace Nova64::Video {

using namespace Clip; // CLIP_POSX, CLIP_NEGX, etc.

// ═══════════════════════════════════════════════════════════════════════════
//  Display List Execution
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::ExecuteDisplayList(DListContext& ctx, uint32_t address) {
    ctx.pc = address & 0x00FFFFFF;
    ctx.halted = false;
    ctx.dlStackDepth = 0;
    
    static constexpr int MAX_COMMANDS_PER_FRAME = 1000000;
    int commandCount = 0;
    
    while (!ctx.halted && commandCount < MAX_COMMANDS_PER_FRAME) {
        // Validate PC
        if (ctx.pc + 8 > ctx.rdramSize) {
            NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
                    "Display list PC out of range: 0x{:08X}", ctx.pc);
            break;
        }
        
        uint32_t w0 = ctx.ReadWord(ctx.pc);
        uint32_t w1 = ctx.ReadWord(ctx.pc + 4);
        
        ctx.AdvancePC(2);
        
        if (!ProcessCommand(ctx, w0, w1)) {
            break;
        }
        
        ++commandCount;
        ++m_commandsProcessed;
    }
    
    if (commandCount >= MAX_COMMANDS_PER_FRAME) {
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
                "Display list exceeded max command count ({})", MAX_COMMANDS_PER_FRAME);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Command Passthrough (0xC0-0xFF shared by all microcodes)
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::HandleRDPCommand(DListContext& ctx, uint32_t w0, uint32_t w1) {
    if (!ctx.rdpProcessor) return;

    uint8_t opcode = (w0 >> 24) & 0x3F; // RDP command ID is bits 29:24
    // TextureRectangle (0x24) and TextureRectFlip (0x25) need S, T, dSdX,
    // and dTdY from continuation words that follow in the display list.
    //
    // Fast3D:  ONE continuation entry — w0 = S|T, w1 = dSdX|dTdY.
    // F3DEX2:  TWO continuation entries —
    //            entry 1  G_RDPHALF_1 (0xE1/0xB3) → w1 = S|T
    //            entry 2  G_RDPHALF_2 (0xF1/0xB4) → w1 = dSdX|dTdY
    //
    // Detect the format by checking for a known RDPHALF opcode byte at
    // bits [31:24] of the first continuation w0.
    if (opcode == 0x24 || opcode == 0x25) {
        uint32_t cont1_w0 = ctx.ReadWord(ctx.pc);
        uint32_t cont1_w1 = ctx.ReadWord(ctx.pc + 4);
        ctx.AdvancePC(2);

        uint32_t st_word, dsdx_word;
        uint8_t topByte = (cont1_w0 >> 24) & 0xFF;
        if (topByte == 0xE1 || topByte == 0xF1 ||
            topByte == 0xB3 || topByte == 0xB4) {
            // F3DEX2 format: S|T is in cont1_w1, read second entry for dSdX|dTdY
            st_word = cont1_w1;

            uint32_t cont2_w1 = ctx.ReadWord(ctx.pc + 4);
            ctx.AdvancePC(2);

            dsdx_word = cont2_w1;
        } else {
            // Fast3D format: single continuation, S|T in w0, dSdX|dTdY in w1
            st_word   = cont1_w0;
            dsdx_word = cont1_w1;
        }

        uint32_t words[4] = { w0, w1, st_word, dsdx_word };
        ctx.rdpProcessor->ProcessCommandBuffer(words, 4);
    } else {
        uint64_t command = (static_cast<uint64_t>(w0) << 32) | static_cast<uint64_t>(w1);
        ctx.rdpProcessor->ProcessRDPCommand(command);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Vertex Transform (consolidated pipeline)
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::TransformVertexBatch(GeometryState& geo, int startIndex, int count) {
    const auto& mvMtx = geo.modelViewStack[geo.modelViewStackDepth];
    bool lighting = (geo.geometryMode & G_LIGHTING) != 0;
    bool fogEnabled = (geo.geometryMode & G_FOG) != 0;
    bool texGen = (geo.geometryMode & G_TEXTURE_GEN) != 0;
    bool texGenLinear = (geo.geometryMode & G_TEXTURE_GEN_LINEAR) != 0;
    bool needEyeNormal = lighting || texGen;

    for (int i = 0; i < count; ++i) {
        TransformedVertex& tv = geo.vertexBuffer[startIndex + i];

        // --- MVP transform (row-vector: v_clip = v * Combined) ---
        float in[4] = { tv.position[0], tv.position[1], tv.position[2], 1.0f };
        float out[4];
        for (int c = 0; c < 4; ++c) {
            out[c] = in[0] * geo.combinedMatrix.m[0][c] +
                     in[1] * geo.combinedMatrix.m[1][c] +
                     in[2] * geo.combinedMatrix.m[2][c] +
                     in[3] * geo.combinedMatrix.m[3][c];
        }
        tv.position[0] = out[0];
        tv.position[1] = out[1];
        tv.position[2] = out[2];
        tv.position[3] = out[3];

        // --- Clip flags ---
        tv.clipFlags = 0;
        float w = tv.position[3];
        if (tv.position[0] > +w) tv.clipFlags |= CLIP_POSX;
        if (tv.position[0] < -w) tv.clipFlags |= CLIP_NEGX;
        if (tv.position[1] > +w) tv.clipFlags |= CLIP_POSY;
        if (tv.position[1] < -w) tv.clipFlags |= CLIP_NEGY;
        if (tv.position[2] > +w) tv.clipFlags |= CLIP_POSZ;
        if (tv.position[2] < -w) tv.clipFlags |= CLIP_NEGZ;

        // --- Eye-space normal (shared by lighting and texGen) ---
        // The N64 RSP transforms the normal by the modelview matrix and
        // normalizes the result. Both lighting and texture generation (env map)
        // use this same normalized eye-space normal.
        float tnx = 0, tny = 0, tnz = 0;
        if (needEyeNormal) {
            float nx = tv.normal[0], ny = tv.normal[1], nz = tv.normal[2];
            tnx = nx*mvMtx.m[0][0] + ny*mvMtx.m[1][0] + nz*mvMtx.m[2][0];
            tny = nx*mvMtx.m[0][1] + ny*mvMtx.m[1][1] + nz*mvMtx.m[2][1];
            tnz = nx*mvMtx.m[0][2] + ny*mvMtx.m[1][2] + nz*mvMtx.m[2][2];

            float len = std::sqrt(tnx*tnx + tny*tny + tnz*tnz);
            if (len > 0.0001f) {
                float invLen = 1.0f / len;
                tnx *= invLen; tny *= invLen; tnz *= invLen;
            }
        }

        // --- Lighting ---
        if (lighting) {
            float r = geo.lights[geo.numLights].color[0];
            float g = geo.lights[geo.numLights].color[1];
            float b = geo.lights[geo.numLights].color[2];

            for (int l = 0; l < geo.numLights; ++l) {
                float dot = tnx * geo.lights[l].direction[0] +
                           tny * geo.lights[l].direction[1] +
                           tnz * geo.lights[l].direction[2];
                if (dot > 0.0f) {
                    r += geo.lights[l].color[0] * dot;
                    g += geo.lights[l].color[1] * dot;
                    b += geo.lights[l].color[2] * dot;
                }
            }

            tv.color[0] = std::min(r, 1.0f);
            tv.color[1] = std::min(g, 1.0f);
            tv.color[2] = std::min(b, 1.0f);
        }

        // --- Fog ---
        if (fogEnabled && tv.position[3] != 0.0f) {
            float z = tv.position[2] / tv.position[3];
            // N64 RSP: fog_alpha = (z/w * mul + off) >> 8, clamped [0,255]
            // Compute in float, divide by 256 to convert from fixed-point to [0,1]
            float fogFactor = (z * geo.fogMultiplier + geo.fogOffset) / 256.0f;
            tv.fogFactor = std::clamp(fogFactor, 0.0f, 1.0f);
        }

        // --- Texture generation (environment mapping) ---
        // Uses the normalized eye-space normal computed above.
        if (texGen) {
            // Project eye-space normal through lookat vectors
            float lx, ly;
            if (geo.lookatEnabled) {
                lx = tnx * geo.lookat[0][0] + tny * geo.lookat[0][1] + tnz * geo.lookat[0][2];
                ly = tnx * geo.lookat[1][0] + tny * geo.lookat[1][1] + tnz * geo.lookat[1][2];
            } else {
                lx = tnx;
                ly = tny;
            }

            if (texGenLinear) {
                // G_TEXTURE_GEN_LINEAR: acos mapping for spherical env map
                float cx = std::clamp(-lx, -1.0f, 1.0f);
                float cy = std::clamp(-ly, -1.0f, 1.0f);
                tv.texCoord[0] = std::acos(cx) * (1.0f / 3.14159265f) * 1024.0f;
                tv.texCoord[1] = std::acos(cy) * (1.0f / 3.14159265f) * 1024.0f;
            } else {
                // G_TEXTURE_GEN: linear mapping for env map reflection
                tv.texCoord[0] = (lx + 1.0f) * 512.0f;
                tv.texCoord[1] = (ly + 1.0f) * 512.0f;
            }
            tv.texCoord1[0] = tv.texCoord[0];
            tv.texCoord1[1] = tv.texCoord[1];
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Vertex Loading
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::LoadVertices(DListContext& ctx, uint32_t address, int count, int startIndex) {
    if (!ctx.state) return;
    
    address = ctx.ResolveAddress(address);
    auto& geo = ctx.state->geometry;
    
    if (startIndex + count > VERTEX_BUFFER_SIZE) {
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
                "Vertex load overflow: start={}, count={}, max={}",
                startIndex, count, VERTEX_BUFFER_SIZE);
        count = VERTEX_BUFFER_SIZE - startIndex;
        if (count <= 0) return;
    }
    
    for (int i = 0; i < count; ++i) {
        uint32_t vtxAddr = address + i * 16; // N64Vertex is 16 bytes
        if (vtxAddr + 16 > ctx.rdramSize) break;
        
        // Read N64 vertex using word-swapped RDRAM addressing
        int16_t x = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 0));
        int16_t y = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 2));
        int16_t z = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 4));
        // uint16_t flag = ctx.ReadHalf(vtxAddr + 6); // unused typically
        int16_t s = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 8));
        int16_t t = static_cast<int16_t>(ctx.ReadHalf(vtxAddr + 10));
        
        // Color/Normal bytes
        uint8_t cn0 = ctx.ReadByte(vtxAddr + 12); // R or NX
        uint8_t cn1 = ctx.ReadByte(vtxAddr + 13); // G or NY
        uint8_t cn2 = ctx.ReadByte(vtxAddr + 14); // B or NZ
        uint8_t cn3 = ctx.ReadByte(vtxAddr + 15); // A
        
        // Fill transformed vertex
        TransformedVertex& tv = geo.vertexBuffer[startIndex + i];
        tv.position[0] = static_cast<float>(x);
        tv.position[1] = static_cast<float>(y);
        tv.position[2] = static_cast<float>(z);
        tv.position[3] = 1.0f;
        
        tv.texCoord[0] = static_cast<float>(s) * (1.0f / 32.0f); // s10.5 to float
        tv.texCoord[1] = static_cast<float>(t) * (1.0f / 32.0f);
        // NOTE: gSPTexture scale is applied at draw time (in makeVert) so that the
        // correct scale is used even if gSPTexture is called after gSPVertex.

        tv.texCoord1[0] = tv.texCoord[0]; // tile 1 starts with same base coords
        tv.texCoord1[1] = tv.texCoord[1];
        
        bool lighting = (geo.geometryMode & G_LIGHTING) != 0;
        
        if (lighting) {
            // Store normal for lighting calculation
            tv.normal[0] = static_cast<float>(static_cast<int8_t>(cn0)) / 127.0f;
            tv.normal[1] = static_cast<float>(static_cast<int8_t>(cn1)) / 127.0f;
            tv.normal[2] = static_cast<float>(static_cast<int8_t>(cn2)) / 127.0f;
            tv.color[3] = static_cast<float>(cn3) / 255.0f;
        } else {
            tv.color[0] = static_cast<float>(cn0) / 255.0f;
            tv.color[1] = static_cast<float>(cn1) / 255.0f;
            tv.color[2] = static_cast<float>(cn2) / 255.0f;
            tv.color[3] = static_cast<float>(cn3) / 255.0f;
        }
        
        tv.clipFlags = 0;
        tv.fogFactor = 0.0f;
    }
    
    // Transform vertices through model-view-projection
    // Uses GeometryState's combined matrix (projection * modelView)
    geo.UpdateCombinedMatrix();
    TransformVertexBatch(geo, startIndex, count);
    
    m_verticesLoaded += count;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Operations
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::LoadMatrix(DListContext& ctx, uint32_t address, bool push, bool load, bool projection) {
    if (!ctx.state) return;
    
    address = ctx.ResolveAddress(address);
    auto& geo = ctx.state->geometry;
    
    // N64 matrices are stored as two 32-byte segments (integer + fractional parts)
    // Total 64 bytes per 4x4 matrix in fixed-point s15.16 format
    if (address + 64 > ctx.rdramSize) return;
    
    Matrix4x4 mtx;
    
    // First 32 bytes: integer parts (16-bit)
    // Next 32 bytes: fractional parts (16-bit)
    // RDRAM uses 32-bit word-swapped storage — use ReadHalf for 16-bit reads
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            int idx = i * 4 + j;
            int16_t intPart = static_cast<int16_t>(ctx.ReadHalf(address + idx * 2));
            uint16_t fracPart = ctx.ReadHalf(address + 32 + idx * 2);
            
            // Combine into s15.16 then convert to float
            int32_t fixed = (static_cast<int32_t>(intPart) << 16) | fracPart;
            mtx.m[i][j] = static_cast<float>(fixed) / 65536.0f;
        }
    }
    
    if (projection) {
        if (load) {
            geo.projectionStack[geo.projectionStackDepth] = mtx;
        } else {
            // Multiply: current = new * current (left-multiply)
            // For row-vector convention (v * MV * P), left-multiplying the projection
            // means the new transform is applied earlier in the pipeline (closer to the
            // vertex), which matches the N64 SDK's gSPMatrix(PROJ|MUL) semantics where
            // the newly-pushed matrix (e.g. view transform) operates before the existing
            // projection.
            Matrix4x4 temp;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    temp.m[i][j] = 0;
                    for (int k = 0; k < 4; ++k) {
                        temp.m[i][j] += mtx.m[i][k] * geo.projectionStack[geo.projectionStackDepth].m[k][j];
                    }
                }
            }
            geo.projectionStack[geo.projectionStackDepth] = temp;
        }
    } else {
        // Model-view
        if (push) {
            if (geo.modelViewStackDepth + 1 < GeometryState::MAX_MATRIX_STACK) {
                geo.modelViewStack[geo.modelViewStackDepth + 1] = geo.modelViewStack[geo.modelViewStackDepth];
                ++geo.modelViewStackDepth;
            }
        }
        
        if (load) {
            geo.modelViewStack[geo.modelViewStackDepth] = mtx;
        } else {
            // Multiply: current = current * new (right-multiply)
            Matrix4x4 temp;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    temp.m[i][j] = 0;
                    for (int k = 0; k < 4; ++k) {
                        temp.m[i][j] += geo.modelViewStack[geo.modelViewStackDepth].m[i][k] * mtx.m[k][j];
                    }
                }
            }
            geo.modelViewStack[geo.modelViewStackDepth] = temp;
        }
    }
    
    geo.combinedDirty = true;
    ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Geometry Mode
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::SetGeometryMode(DListContext& ctx, uint32_t clearBits, uint32_t setBits) {
    if (!ctx.state) return;
    
    auto& geo = ctx.state->geometry;
    geo.geometryMode = (geo.geometryMode & ~clearBits) | setBits;
    
    ctx.state->MarkDirty(RDPDirtyFlag::GeometryMode);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Texture State
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::SetTextureState(DListContext& ctx, uint32_t w0, uint32_t w1) {
    if (!ctx.state) return;
    
    auto& geo = ctx.state->geometry;
    int level = (w0 >> 11) & 0x7;
    int tileIndex = (w0 >> 8) & 0x7;
    bool on = (w0 & 0xFF) != 0;
    
    geo.texture.scaleS = static_cast<uint16_t>((w1 >> 16) & 0xFFFF);
    geo.texture.scaleT = static_cast<uint16_t>(w1 & 0xFFFF);
    geo.texture.level = static_cast<uint8_t>(level);
    geo.texture.tileIndex = static_cast<uint8_t>(tileIndex);
    geo.texture.enabled = on;
    
    ctx.state->MarkDirty(RDPDirtyFlag::TextureState);
}

// ═══════════════════════════════════════════════════════════════════════════
//  MoveWord
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::MoveWord(DListContext& ctx, uint8_t index, uint16_t offset, uint32_t value) {
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    switch (index) {
    case MoveWord::MW_SEGMENT:
    {
        int seg = (offset >> 2) & 0xF;
        geo.segments[seg] = value & 0x00FFFFFF;
        break;
    }
    
    case MoveWord::MW_NUMLIGHT:
    {
        // value = (numLights-1) * 24 for GBI1, or (numLights) * 24 for GBI2
        // We store the actual light count
        int numLights = static_cast<int>(value / 24);
        if (numLights > 8) numLights = 8;
        geo.numLights = numLights;
        ctx.state->MarkDirty(RDPDirtyFlag::Lights);
        break;
    }
    
    case MoveWord::MW_FOG:
    {
        int16_t multiplier = static_cast<int16_t>((value >> 16) & 0xFFFF);
        int16_t fogOffset = static_cast<int16_t>(value & 0xFFFF);
        geo.fogMultiplier = multiplier;
        geo.fogOffset = fogOffset;
        break;
    }
    
    case MoveWord::MW_LIGHTCOL:
    {
        // Light color: offset indicates which light and which field
        int lightIdx = offset / 0x18;
        int field = (offset % 0x18) / 4;
        if (lightIdx < 8) {
            if (field == 0) {
                // RGBA packed in value
                geo.lights[lightIdx].color[0] = static_cast<float>((value >> 24) & 0xFF) / 255.0f;
                geo.lights[lightIdx].color[1] = static_cast<float>((value >> 16) & 0xFF) / 255.0f;
                geo.lights[lightIdx].color[2] = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
            }
        }
        ctx.state->MarkDirty(RDPDirtyFlag::Lights);
        break;
    }
    
    case MoveWord::MW_CLIP:
    {
        // Clipping parameters — sets guard-band clip ratio.
        // In HLE this mainly affects frustum-rejection thresholds.
        // Store for potential future use; current clipping is adequate.
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "MW_CLIP: offset=0x{:02X} value=0x{:08X}", offset, value);
        break;
    }
    
    case MoveWord::MW_PERSPNORM:
    {
        // Perspective normalization factor — on real RSP this scales W to
        // maximise fixed-point precision.  In HLE (float pipeline) it is a
        // no-op, but log for tracing.
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "MW_PERSPNORM: value=0x{:08X}", value);
        break;
    }
    
    case MoveWord::MW_MATRIX:
    {
        // Direct matrix element write — force MVP recomputation so the
        // next vertex batch picks up the change.
        geo.combinedDirty = true;
        ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "MW_MATRIX: offset=0x{:02X} value=0x{:08X}", offset, value);
        break;
    }
    
    case MoveWord::MW_FORCEMTX:
    {
        // Force matrix reload
        geo.combinedDirty = true;
        ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
        break;
    }
    
    default:
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "Unknown MoveWord index: 0x{:02X}", index);
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  MoveMem
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::MoveMemGBI1(DListContext& ctx, uint8_t index, [[maybe_unused]] uint8_t size, uint32_t address) {
    if (!ctx.state) return;
    address = ctx.ResolveAddress(address);
    auto& geo = ctx.state->geometry;
    
    switch (index) {
    case MoveMem::G_MV_VIEWPORT:
    {
        if (address + 16 > ctx.rdramSize) break;
        
        // Viewport struct: scale (4 * int16), translate (4 * int16) — 16 bytes
        // RDRAM uses 32-bit word-swapped storage — use ReadHalf for 16-bit reads
        int16_t scaleX = static_cast<int16_t>(ctx.ReadHalf(address + 0));
        int16_t scaleY = static_cast<int16_t>(ctx.ReadHalf(address + 2));
        int16_t scaleZ = static_cast<int16_t>(ctx.ReadHalf(address + 4));
        // offset 6..7 = padding
        int16_t transX = static_cast<int16_t>(ctx.ReadHalf(address + 8));
        int16_t transY = static_cast<int16_t>(ctx.ReadHalf(address + 10));
        int16_t transZ = static_cast<int16_t>(ctx.ReadHalf(address + 12));
        
        geo.viewport.scaleX = static_cast<float>(scaleX) / 4.0f;
        // Negate viewport scaleY: the N64 screen has Y=0 at the top (Y-down),
        // but clip-space Y is positive-up.  Negating scaleY maps positive
        // clip Y to small screen Y (top), matching reference emulators
        // (Project64-video, GLideN64).
        geo.viewport.scaleY = -static_cast<float>(scaleY) / 4.0f;
        geo.viewport.scaleZ = static_cast<float>(scaleZ) / 1024.0f;
        geo.viewport.transX = static_cast<float>(transX) / 4.0f;
        geo.viewport.transY = static_cast<float>(transY) / 4.0f;
        geo.viewport.transZ = static_cast<float>(transZ) / 1024.0f;
        
        ctx.state->MarkDirty(RDPDirtyFlag::Viewport);
        break;
    }
    
    case MoveMem::G_MV_LOOKATY:
    case MoveMem::G_MV_LOOKATX:
    {
        // LookAt vectors for texture generation
        if (address + 16 > ctx.rdramSize) break;
        int n = (index == MoveMem::G_MV_LOOKATX) ? 0 : 1;
        geo.lookat[n][0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
        geo.lookat[n][1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
        geo.lookat[n][2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
        // Enable lookat when the second vector (Y) has non-zero X or Y
        if (n == 1) {
            geo.lookatEnabled = (geo.lookat[1][0] != 0.0f || geo.lookat[1][1] != 0.0f);
        }
        break;
    }
    
    default:
    {
        // Light data: G_MV_L0..G_MV_L7
        if (index >= MoveMem::G_MV_L0 && index <= MoveMem::G_MV_L7) {
            int lightIdx = (index - MoveMem::G_MV_L0) / 2;
            if (lightIdx >= 0 && lightIdx < 8 && address + 16 <= ctx.rdramSize) {
                // Light struct: color (4 bytes), pad (4 bytes), color_copy (4 bytes), direction (3 bytes + pad)
                // Use ReadByte for word-swapped RDRAM byte access
                geo.lights[lightIdx].color[0] = static_cast<float>(ctx.ReadByte(address + 0)) / 255.0f;
                geo.lights[lightIdx].color[1] = static_cast<float>(ctx.ReadByte(address + 1)) / 255.0f;
                geo.lights[lightIdx].color[2] = static_cast<float>(ctx.ReadByte(address + 2)) / 255.0f;
                
                geo.lights[lightIdx].direction[0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                geo.lights[lightIdx].direction[1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                geo.lights[lightIdx].direction[2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
                
                ctx.state->MarkDirty(RDPDirtyFlag::Lights);
            }
        }
        break;
    }
    }
}

void GBIHandler::MoveMemGBI2(DListContext& ctx, uint8_t index, uint8_t size, uint8_t offset, uint32_t address) {
    if (!ctx.state) return;
    address = ctx.ResolveAddress(address);
    auto& geo = ctx.state->geometry;
    
    switch (index) {
    case MoveMem::G_MV2_VIEWPORT:
    {
        MoveMemGBI1(ctx, MoveMem::G_MV_VIEWPORT, size, address);
        break;
    }
    
    case MoveMem::G_MV2_LIGHT:
    {
        // GBI2 light addressing: gSPLight offset = (n + 1) * 24, 0-based index = offset/24 - 2
        int lightIdx = (offset / 24) - 2;
        if (lightIdx >= 0 && lightIdx < 10 && address + 16 <= ctx.rdramSize) {
            if (lightIdx < 8) {
                // Use ReadByte for word-swapped RDRAM byte access
                geo.lights[lightIdx].color[0] = static_cast<float>(ctx.ReadByte(address + 0)) / 255.0f;
                geo.lights[lightIdx].color[1] = static_cast<float>(ctx.ReadByte(address + 1)) / 255.0f;
                geo.lights[lightIdx].color[2] = static_cast<float>(ctx.ReadByte(address + 2)) / 255.0f;
                
                geo.lights[lightIdx].direction[0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                geo.lights[lightIdx].direction[1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                geo.lights[lightIdx].direction[2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
            } else if (lightIdx == 8) {
                // LookAt Y (same format as light direction at offset 8)
                geo.lookat[1][0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                geo.lookat[1][1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                geo.lookat[1][2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
                geo.lookatEnabled = (geo.lookat[1][0] != 0.0f || geo.lookat[1][1] != 0.0f);
            } else if (lightIdx == 9) {
                // LookAt X (same format as light direction at offset 8)
                geo.lookat[0][0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                geo.lookat[0][1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                geo.lookat[0][2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
            }
            
            ctx.state->MarkDirty(RDPDirtyFlag::Lights);
        }
        break;
    }
    
    case MoveMem::G_MV2_MATRIX:
    {
        // Force matrix — used by some microcodes to insert a combined matrix
        if (address + 64 <= ctx.rdramSize) {
            // Read directly as combined matrix
            LoadMatrix(ctx, address, false, true, false);
        }
        break;
    }
    
    default:
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "Unknown GBI2 MoveMem index: {} offset: {}", index, offset);
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  OtherMode
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::SetOtherModeH(DListContext& ctx, uint32_t shift, uint32_t length, uint32_t value) {
    if (!ctx.state) return;
    
    uint32_t mask = (((uint32_t)1 << length) - 1) << shift;
    ctx.state->otherModeH = (ctx.state->otherModeH & ~mask) | (value & mask);
    ctx.state->UpdateDerivedOtherModes();
    ctx.state->MarkDirty(RDPDirtyFlag::OtherModes);
}

void GBIHandler::SetOtherModeL(DListContext& ctx, uint32_t shift, uint32_t length, uint32_t value) {
    if (!ctx.state) return;
    
    uint32_t mask = (((uint32_t)1 << length) - 1) << shift;
    ctx.state->otherModeL = (ctx.state->otherModeL & ~mask) | (value & mask);
    ctx.state->UpdateDerivedOtherModes();
    ctx.state->MarkDirty(RDPDirtyFlag::OtherModes);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Triangle Drawing
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::DrawTriangle(DListContext& ctx, int v0, int v1, int v2) {
    if (!ctx.state || !ctx.rdpProcessor) return;
    
    auto& geo = ctx.state->geometry;
    int maxVtx = VERTEX_BUFFER_SIZE;
    
    // Validate indices
    if (v0 >= maxVtx || v1 >= maxVtx || v2 >= maxVtx) {
        return;
    }
    
    // Backface culling (only if all vertices have valid w for perspective divide)
    if (geo.geometryMode & (G_CULL_FRONT | G_CULL_BACK)) {
        const auto& tv0 = geo.vertexBuffer[v0];
        const auto& tv1 = geo.vertexBuffer[v1];
        const auto& tv2 = geo.vertexBuffer[v2];
        
        float aw0 = std::abs(tv0.position[3]);
        float aw1 = std::abs(tv1.position[3]);
        float aw2 = std::abs(tv2.position[3]);
        
        // Skip culling if any vertex has w too close to zero (degenerate)
        constexpr float wEps = 1e-5f;
        if (aw0 > wEps && aw1 > wEps && aw2 > wEps) {
            float w0inv = 1.0f / tv0.position[3];
            float w1inv = 1.0f / tv1.position[3];
            float w2inv = 1.0f / tv2.position[3];
            
            float x0 = tv0.position[0] * w0inv, y0 = tv0.position[1] * w0inv;
            float x1 = tv1.position[0] * w1inv, y1 = tv1.position[1] * w1inv;
            float x2 = tv2.position[0] * w2inv, y2 = tv2.position[1] * w2inv;
            
            float cross = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            
            if ((geo.geometryMode & G_CULL_BACK) && cross < 0.0f) {
                return;
            }
            if ((geo.geometryMode & G_CULL_FRONT) && cross > 0.0f) {
                return;
            }
        }
    }
    
    // Clip rejection / clipping
    {
    const auto& tv0c = geo.vertexBuffer[v0];
    const auto& tv1c = geo.vertexBuffer[v1];
    const auto& tv2c = geo.vertexBuffer[v2];
    
    uint32_t orFlags = tv0c.clipFlags | tv1c.clipFlags | tv2c.clipFlags;
    
    // Trivial rejection: all verts on the same side of a plane
    if (tv0c.clipFlags & tv1c.clipFlags & tv2c.clipFlags) {
        return;
    }
    
    if (orFlags == 0) {
        // Trivial accept - fully inside frustum, send directly
        ctx.rdpProcessor->DrawTriangle(v0, v1, v2, geo);
        ++m_trianglesProcessed;
    } else {
        // Needs clipping - convert to ClipVertex, run Sutherland-Hodgman
        auto toClipVert = [](const TransformedVertex& tv) -> ClipVertex {
            ClipVertex cv;
            cv.position  = { tv.position[0], tv.position[1], tv.position[2], tv.position[3] };
            cv.color     = { tv.color[0], tv.color[1], tv.color[2], tv.color[3] };
            cv.texCoord  = { tv.texCoord[0],  tv.texCoord[1] };
            cv.texCoord1 = { tv.texCoord1[0], tv.texCoord1[1] };
            cv.fog       = tv.fogFactor;
            cv.clipFlags = tv.clipFlags;
            return cv;
        };
        
        ClippedPolygon poly;
        int triCount = ClipTriangle(toClipVert(tv0c), toClipVert(tv1c), toClipVert(tv2c), poly);
        
        if (triCount > 0 && poly.vertexCount >= 3) {
            // Fan-triangulate clipped polygon (vertex 0 is the hub)
            auto toRenderVert = [&geo](const ClipVertex& cv) -> RenderBatch::Vertex {
                RenderBatch::Vertex rv{};
                // Perspective divide before viewport transform (same as makeVert in RDPProcessor)
                float invW = (cv.position[3] != 0.0f) ? 1.0f / cv.position[3] : 1.0f;
                rv.x = cv.position[0] * invW * geo.viewport.scaleX + geo.viewport.transX;
                rv.y = cv.position[1] * invW * geo.viewport.scaleY + geo.viewport.transY;
                {
                    float z_screen = cv.position[2] * invW * geo.viewport.scaleZ + geo.viewport.transZ;
                    float z_range  = geo.viewport.transZ + std::abs(geo.viewport.scaleZ);
                    rv.z = (z_range > 0.0f) ? z_screen / z_range : 0.0f;
                    rv.z = std::max(0.0f, std::min(1.0f, rv.z));
                }
                rv.w = 1.0f;
                rv.r = cv.color[0]; rv.g = cv.color[1];
                rv.b = cv.color[2]; rv.a = cv.color[3];
                // Pre-divide texcoords by clip_W for perspective-correct interpolation.
                // Apply gSPTexture scale at draw time (same convention as HLE makeVert).
                float scaleS = geo.texture.enabled ? geo.texture.GetScaleSFloat() : 1.0f;
                float scaleT = geo.texture.enabled ? geo.texture.GetScaleTFloat() : 1.0f;
                rv.s  = cv.texCoord.x  * scaleS * invW; rv.t  = cv.texCoord.y  * scaleT * invW;
                rv.s1 = cv.texCoord1.x * scaleS * invW; rv.t1 = cv.texCoord1.y * scaleT * invW;
                rv.perspW = invW;
                rv.fogFactor = cv.fog;
                rv.clipFlags = 0; // Already clipped
                return rv;
            };
            
            for (int i = 1; i + 1 < poly.vertexCount; ++i) {
                ctx.rdpProcessor->DrawTriangleRaw(
                    toRenderVert(poly.vertices[0]),
                    toRenderVert(poly.vertices[i]),
                    toRenderVert(poly.vertices[i + 1]),
                    geo.geometryMode);
            }
            m_trianglesProcessed += (poly.vertexCount - 2);
        }
    }
    } // end clip rejection block
}

void GBIHandler::DrawTriangle2(DListContext& ctx, int v0, int v1, int v2, int v3, int v4, int v5) {
    DrawTriangle(ctx, v0, v1, v2);
    DrawTriangle(ctx, v3, v4, v5);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Display List Control
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::DisplayList(DListContext& ctx, uint32_t address, bool branch) {
    address = ctx.ResolveAddress(address);
    
    if (branch) {
        // Branch: replace current DL (no push)
        ctx.pc = address;
    } else {
        // Call: push return address first
        if (!ctx.PushDL(ctx.pc, false)) {
            NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
                    "DL call stack overflow at depth {}", ctx.dlStackDepth);
            return;
        }
        ctx.pc = address;
    }
}

void GBIHandler::EndDisplayList(DListContext& ctx) {
    if (ctx.dlStackDepth > 0) {
        ctx.PopDL();
    } else {
        ctx.halted = true;
    }
}

void GBIHandler::PopMatrix(DListContext& ctx, uint32_t param) {
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    // param interpretation varies by GBI version
    // GBI1: param = number of matrices to pop (in 64-byte units)
    // GBI2: param = 0x40 for modelview
    int count = (param >> 6);
    if (count < 1) count = 1;
    
    for (int i = 0; i < count; ++i) {
        if (geo.modelViewStackDepth > 0) {
            --geo.modelViewStackDepth;
        }
    }
    
    geo.combinedDirty = true;
    ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Vertex Modification
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::ModifyVertex(DListContext& ctx, uint8_t where, uint16_t vtxIndex, uint32_t value) {
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    if (vtxIndex >= VERTEX_BUFFER_SIZE) return;
    auto& v = geo.vertexBuffer[vtxIndex];
    
    switch (where) {
    case 0x10: // G_MWO_POINT_RGBA
        v.color[0] = static_cast<float>((value >> 24) & 0xFF) / 255.0f;
        v.color[1] = static_cast<float>((value >> 16) & 0xFF) / 255.0f;
        v.color[2] = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
        v.color[3] = static_cast<float>(value & 0xFF) / 255.0f;
        break;
        
    case 0x14: // G_MWO_POINT_ST
    {
        int16_t s = static_cast<int16_t>((value >> 16) & 0xFFFF);
        int16_t t = static_cast<int16_t>(value & 0xFFFF);
        v.texCoord[0] = static_cast<float>(s) / 32.0f;
        v.texCoord[1] = static_cast<float>(t) / 32.0f;
        break;
    }
    
    case 0x18: // G_MWO_POINT_XYSCREEN — sets screen-space X/Y directly
    {
        // These are already in screen-space (10.2 fixed-point), matching the
        // post-viewport-transform coordinate system used by the vertex buffer.
        int16_t x = static_cast<int16_t>((value >> 16) & 0xFFFF);
        int16_t y = static_cast<int16_t>(value & 0xFFFF);
        v.position[0] = static_cast<float>(x) / 4.0f;
        v.position[1] = static_cast<float>(y) / 4.0f;
        // Mark W=1 so ortho projection works correctly for the overridden position
        v.position[3] = 1.0f;
        v.clipFlags = 0;
        break;
    }
    
    case 0x1C: // G_MWO_POINT_ZSCREEN — sets screen-space Z directly
    {
        // Z is in the same viewport-transformed range as standard vertices.
        int16_t z = static_cast<int16_t>((value >> 16) & 0xFFFF);
        v.position[2] = static_cast<float>(z);
        v.clipFlags = 0;
        break;
    }
    
    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Culling & Branching
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::CullDisplayList(DListContext& ctx, uint32_t startVtx, uint32_t endVtx) {
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    // If all vertices in range are clipped by the same plane, end this DL
    uint32_t andFlags = 0xFFFFFFFF;
    uint32_t maxVtx = VERTEX_BUFFER_SIZE;
    
    for (uint32_t i = startVtx; i <= endVtx && i < maxVtx; ++i) {
        andFlags &= geo.vertexBuffer[i].clipFlags;
    }
    
    if (andFlags != 0) {
        // All vertices clipped by at least one common plane — skip this DL
        EndDisplayList(ctx);
    }
}

void GBIHandler::BranchZ(DListContext& ctx, uint32_t vtxIndex, uint32_t zValue, uint32_t address) {
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    if (vtxIndex >= VERTEX_BUFFER_SIZE) return;
    
    // The N64 z-buffer comparison uses the screen-space Z derived from W.
    // G_BRANCH_Z compares the vertex's projected W-derived depth against a
    // fixed-point threshold. The zValue from the command is in the same space
    // as the z-buffer (0x0000..0xFFFF range, normalized from near to far).
    // We convert the vertex's W to the equivalent z-buffer value for comparison.
    float w = geo.vertexBuffer[vtxIndex].position[3]; // clip W
    
    // N64 depth = (zValue / 0xFFFF) mapped to near..far
    // Vertex depth = 1.0/W normalized. For the comparison we convert both
    // to the same normalized [0,1] range based on the viewport z scale.
    float vtxDepth = 0.0f;
    if (w > 0.0001f) {
        // Viewport Z is now in the correct 2.10 fixed-point scale (divided by
        // 1024 on load), so z_screen is in the same [0,1] range as the BranchZ
        // threshold.  Compare directly — no z_range normalization needed.
        float zScale = geo.viewport.scaleZ;
        float zTrans = geo.viewport.transZ;
        float z = geo.vertexBuffer[vtxIndex].position[2];
        vtxDepth = z * zScale / w + zTrans;
    }
    
    // The N64 BranchZ threshold is a 32-bit fixed-point value (16.16).
    // With viewport Z divided by 1024, vtxDepth \u2248 [0,1].  Threshold is
    // zValue / 65536 which is also in viewport-Z space after the /1024 convention.
    float threshold = static_cast<float>(zValue) / 65536.0f;
    
    if (vtxDepth <= threshold) {
        ctx.pc = ctx.ResolveAddress(address);
    }
}

void GBIHandler::LoadUcode(DListContext& ctx, uint32_t address, uint32_t dataAddress) {
    // Dynamic microcode loading — switch microcode mid-frame
    NOVA_LOG(LogLevel::Info, LogSubsystem::Core,
            "G_LOAD_UCODE: text=0x{:08X}, data=0x{:08X}", address, dataAddress);
    
    // Signal the dispatcher to re-detect and switch handler
    if (ctx.dispatcher) {
        // N64 IMEM is 4KB, DMEM is 4KB
        constexpr uint32_t IMEM_SIZE = 4096;
        constexpr uint32_t DMEM_SIZE = 4096;
        uint32_t textAddr = ctx.ResolveAddress(address);
        uint32_t dataAddr = ctx.ResolveAddress(dataAddress);
        ctx.dispatcher->HandleUcodeSwitch(textAddr, IMEM_SIZE, dataAddr, DMEM_SIZE);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Statistics
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::ResetStats() {
    m_commandsProcessed = 0;
    m_trianglesProcessed = 0;
    m_verticesLoaded = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command Description (Debug)
// ═══════════════════════════════════════════════════════════════════════════

std::string DescribeGBI1Command(uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    switch (opcode) {
    case GBI1::G_SPNOOP:        return "G_SPNOOP";
    case GBI1::G_MTX:           return "G_MTX addr=0x" + std::to_string(w1);
    case GBI1::G_MOVEMEM:       return "G_MOVEMEM";
    case GBI1::G_VTX:           return "G_VTX count=" + std::to_string((w0 >> 10) & 0x3F);
    case GBI1::G_DL:            return "G_DL addr=0x" + std::to_string(w1);
    case GBI1::G_TRI1:          return "G_TRI1";
    case GBI1::G_CULLDL:        return "G_CULLDL";
    case GBI1::G_POPMTX:        return "G_POPMTX";
    case GBI1::G_TEXTURE:       return "G_TEXTURE";
    case GBI1::G_ENDDL:         return "G_ENDDL";
    default: {
        if (opcode >= 0xC0)     return "RDP_CMD_0x" + std::to_string(opcode);
        return "GBI1_UNKNOWN_0x" + std::to_string(opcode);
    }
    }
}

std::string DescribeGBI2Command(uint32_t w0, uint32_t w1) {
    uint8_t opcode = (w0 >> 24) & 0xFF;
    
    switch (opcode) {
    case GBI2::G_NOOP:          return "G_NOOP";
    case GBI2::G_VTX:           return "G_VTX count=" + std::to_string((w0 >> 12) & 0xFF);
    case GBI2::G_MODIFYVTX:     return "G_MODIFYVTX";
    case GBI2::G_CULLDL:        return "G_CULLDL";
    case GBI2::G_BRANCH_Z:      return "G_BRANCH_Z";
    case GBI2::G_TRI1:          return "G_TRI1";
    case GBI2::G_TRI2:          return "G_TRI2";
    case GBI2::G_QUAD:          return "G_QUAD";
    case GBI2::G_TEXTURE:       return "G_TEXTURE";
    case GBI2::G_POPMTX:        return "G_POPMTX";
    case GBI2::G_DL:            return "G_DL addr=0x" + std::to_string(w1);
    case GBI2::G_ENDDL:         return "G_ENDDL";
    case GBI2::G_MTX:           return "G_MTX addr=0x" + std::to_string(w1);
    case GBI2::G_MOVEWORD:      return "G_MOVEWORD";
    case GBI2::G_MOVEMEM:       return "G_MOVEMEM";
    default: {
        if (opcode >= 0xC0)     return "RDP_CMD_0x" + std::to_string(opcode);
        return "GBI2_UNKNOWN_0x" + std::to_string(opcode);
    }
    }
}

} // namespace Nova64::Video
