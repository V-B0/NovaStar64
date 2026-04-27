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
#include <cstdio>
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
        
        // Dispatch through the active handler so mid-frame microcode
        // switches (G_LOAD_UCODE) take effect immediately.
        GBIHandler* handler = ctx.dispatcher
            ? ctx.dispatcher->GetCurrentHandler()
            : this;
        
        if (!handler->ProcessCommand(ctx, w0, w1)) {
            break;
        }
        
        ++commandCount;
        // Credit the command to whichever handler actually ran it
        ++handler->m_commandsProcessed;
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
        // SetTextureImage (0x3D), SetDepthImage (0x3E), SetColorImage (0x3F), LoadBlock (0x33)
        // carry RDRAM addresses in w1 that need segment resolution.
        // The real RSP microcode resolves segments before emitting to the RDP.
        if (opcode == 0x3D || opcode == 0x3E || opcode == 0x3F) {
            w1 = ctx.ResolveAddress(w1);
        }
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
        // Only X/Y and W, matching GLideN64 behavior.
        // Z clipping is handled by the GPU (GL_DEPTH_CLAMP for NoN microcodes).
        tv.clipFlags = 0;
        float w = tv.position[3];
        // W-near detection: matches GLideN64 (gSP.cpp gSPClipVertex).  Threshold
        // is 0.01 — the actual clip plane is at w=0 (camera plane), and the
        // post-clip vertex w is clamped to 0.01 in ClipMath.cpp to keep 1/W
        // finite for downstream perspective math.
        if (w < 0.01f) {
            tv.clipFlags |= CLIP_W;
        }
        // X/Y clip flags: computed for ALL vertices regardless of W sign,
        // matching GLideN64's gSPClipVertex().  For negative W the comparisons
        // may set multiple flags (e.g. both POSX and NEGX), which is correct
        // for the AND-rejection test.  Post-clip face culling (below) handles
        // the case where camera-through-wall triangles survive AND-rejection.
        {
            float wg = w * geo.clipRatio;
            float wsInv = (geo.widescreenScale != 1.0f) ? (1.0f / geo.widescreenScale) : 1.0f;
            float clipX = tv.position[0] * wsInv;
            if (clipX > +wg) tv.clipFlags |= CLIP_POSX;
            if (clipX < -wg) tv.clipFlags |= CLIP_NEGX;
            if (tv.position[1] > +wg) tv.clipFlags |= CLIP_POSY;
            if (tv.position[1] < -wg) tv.clipFlags |= CLIP_NEGY;
        }

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
            // N64 RSP alternates between primary and secondary light colors
            // for even/odd vertices (dithered flat shading). GLideN64 ref:
            // gSPLightVertexStandard — useFirstColor = ((v + j) & 1) == 0
            bool useFirstColor = ((startIndex + i) & 1) == 0;
            const float* ambColor = useFirstColor
                ? geo.lights[geo.numLights].color
                : geo.lights[geo.numLights].color2;
            float lr = ambColor[0];
            float lg = ambColor[1];
            float lb = ambColor[2];

            bool pointLighting = (geo.geometryMode & G_POINT_LIGHTING) != 0;

            if (pointLighting) {
                // Compute eye-space vertex position (MV * objPos)
                float eyeX = in[0]*mvMtx.m[0][0] + in[1]*mvMtx.m[1][0] + in[2]*mvMtx.m[2][0] + mvMtx.m[3][0];
                float eyeY = in[0]*mvMtx.m[0][1] + in[1]*mvMtx.m[1][1] + in[2]*mvMtx.m[2][1] + mvMtx.m[3][1];
                float eyeZ = in[0]*mvMtx.m[0][2] + in[1]*mvMtx.m[1][2] + in[2]*mvMtx.m[2][2] + mvMtx.m[3][2];

                for (int l = 0; l < geo.numLights; ++l) {
                    float intensity;
                    if (geo.lights[l].ca != 0.0f) {
                        // Point light (matching GLideN64 processPointLight)
                        constexpr float FIXED2FLOATRECIP16 = 1.0f / 65536.0f;
                        float lvx = geo.lights[l].position[0] - eyeX;
                        float lvy = geo.lights[l].position[1] - eyeY;
                        float lvz = geo.lights[l].position[2] - eyeZ;

                        // N64 RSP microcode weights Z² by 2x in point light distance
                        float K = lvx*lvx + lvy*lvy + lvz*lvz * 2.0f;
                        float KS = std::sqrt(K);

                        // Inverse transform lvec to model space (transpose of upper 3x3 MV)
                        float ix = lvx*mvMtx.m[0][0] + lvy*mvMtx.m[0][1] + lvz*mvMtx.m[0][2];
                        float iy = lvx*mvMtx.m[1][0] + lvy*mvMtx.m[1][1] + lvz*mvMtx.m[1][2];
                        float iz = lvx*mvMtx.m[2][0] + lvy*mvMtx.m[2][1] + lvz*mvMtx.m[2][2];

                        // Normalize with 4/KS factor, clamp to [-1,1]
                        if (KS > 0.0f) {
                            float s = 4.0f / KS;
                            ix = std::clamp(ix * s, -1.0f, 1.0f);
                            iy = std::clamp(iy * s, -1.0f, 1.0f);
                            iz = std::clamp(iz * s, -1.0f, 1.0f);
                        }

                        // Dot with object-space normal
                        float V = std::clamp(ix*tv.normal[0] + iy*tv.normal[1] + iz*tv.normal[2], -1.0f, 1.0f);

                        // Attenuation
                        float KSF = std::floor(KS);
                        float D = (KSF * geo.lights[l].la * 2.0f + KSF * KSF * geo.lights[l].qa / 8.0f) * FIXED2FLOATRECIP16 + 1.0f;
                        intensity = V / D;
                    } else {
                        // Directional light
                        intensity = tnx * geo.lights[l].direction[0] +
                                    tny * geo.lights[l].direction[1] +
                                    tnz * geo.lights[l].direction[2];
                    }
                    if (intensity > 0.0f) {
                        const float* lc = useFirstColor ? geo.lights[l].color : geo.lights[l].color2;
                        lr += lc[0] * intensity;
                        lg += lc[1] * intensity;
                        lb += lc[2] * intensity;
                    }
                }
            } else {
                // Standard directional-only lighting
                for (int l = 0; l < geo.numLights; ++l) {
                    float dot = tnx * geo.lights[l].direction[0] +
                               tny * geo.lights[l].direction[1] +
                               tnz * geo.lights[l].direction[2];
                    if (dot > 0.0f) {
                        const float* lc = useFirstColor ? geo.lights[l].color : geo.lights[l].color2;
                        lr += lc[0] * dot;
                        lg += lc[1] * dot;
                        lb += lc[2] * dot;
                    }
                }
            }

            tv.color[0] = std::min(lr, 1.0f);
            tv.color[1] = std::min(lg, 1.0f);
            tv.color[2] = std::min(lb, 1.0f);
        }

        // --- Fog ---
        // When G_FOG is set the RSP computes fog_alpha from the NDC depth
        // (z/w) and writes it into shade.a, REPLACING the vertex alpha.
        // The blender reads shade.a for the fog cycle (G_RM_FOG_SHADE_A):
        //   out = fogColor * shade.a + combined * (1 - shade.a)
        // Formula (matching GLideN64 / real RSP):
        //   fog = clamp(z/w * fogMul/256 + fogOff/256, 0, 1)
        // Note: z/w is sign-invariant (both z and w flip together when
        // the projection matrix convention changes), so this works even
        // when our clip W is negative.
        if (fogEnabled && tv.position[3] != 0.0f) {
            float zOverW = tv.position[2] / tv.position[3];
            // N64 RSP clamps z/w to -1.0 for vertices behind the near plane.
            // Without this, near-clipped geometry gets wildly wrong fog values
            // that propagate through the clipper. (GLideN64 ref: vertex shader)
            if (tv.position[2] < -tv.position[3])
                zOverW = -1.0f;
            float fogVal = zOverW * (static_cast<float>(geo.fogMultiplier) / 256.0f)
                         + (static_cast<float>(geo.fogOffset) / 256.0f);
            tv.fogFactor = std::clamp(fogVal, 0.0f, 1.0f);
            // Overwrite shade alpha with fog factor — the real RSP does this
            tv.color[3] = tv.fogFactor;
        }
        // fogFactor stays at its LoadVertices default (0.0) when G_FOG is off

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
        bool texGen = (geo.geometryMode & G_TEXTURE_GEN) != 0;
        bool texGenLinear = (geo.geometryMode & G_TEXTURE_GEN_LINEAR) != 0;
        bool needEyeNormal = lighting || texGen || texGenLinear;
        
        if (needEyeNormal) {
            // Store normal for lighting calculation or texture generation
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
        tv.modifiedXY = false;
        tv.modifiedZ = false;
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

            // Widescreen hack: scale projection X to widen horizontal FOV.
            // For perspective (m[3][3] ≈ 0): only divide m[0][0] to widen FOV.
            // For ortho (m[3][3] ≈ 1): divide both m[0][0] and m[3][0]
            //   to scale 2D content (HUD, text) inward to 4:3 proportions.
            if (geo.widescreenScale != 1.0f) {
                auto& P = geo.projectionStack[geo.projectionStackDepth];
                P.m[0][0] /= geo.widescreenScale;
                // Only adjust ortho translation — perspective m[3][0] is
                // usually 0 but dividing a non-zero value would shift the
                // frustum and cause far objects to cull incorrectly.
                if (fabsf(P.m[3][3]) > 0.5f) {
                    P.m[3][0] /= geo.widescreenScale;
                }
            }
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
            // Multiply: current = new * current (left-multiply)
            // N64 SDK documents gSPMatrix(MUL) as "M × G" (new × current).
            // In row-vector convention (v * MV), left-multiplying means the
            // new matrix is applied first (closest to the vertex), which is
            // correct for hierarchical transforms: v * (M_child * M_parent * View).
            Matrix4x4 temp;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    temp.m[i][j] = 0;
                    for (int k = 0; k < 4; ++k) {
                        temp.m[i][j] += mtx.m[i][k] * geo.modelViewStack[geo.modelViewStackDepth].m[k][j];
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
        if (numLights >= MAX_LIGHTS) numLights = MAX_LIGHTS - 1;
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
        if (lightIdx < 9) {
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
        // Guard-band clip ratio — N64 RSP clips at w*ratio instead of w.
        // gSPClipRatio sends 4 moveword commands (RNX/RNY/RPX/RPY) with
        // the same ratio value.  Store the ratio from any of them.
        uint32_t ratio = value & 0xFFFF;
        if (ratio >= 1 && ratio <= 6) {
            geo.clipRatio = static_cast<float>(ratio);
        }
        NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
                "MW_CLIP: offset=0x{:02X} value=0x{:08X} ratio={}",
                offset, value, geo.clipRatio);
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
        // gSPInsertMatrix — modifies individual elements of the combined matrix.
        // The 'offset' selects which matrix and element; 'value' contains two
        // 16-bit halves (integer or fractional parts of two consecutive elements).
        // GLideN64 ref: gSP.cpp gSPInsertMatrix.
        auto& geo = ctx.state->geometry;
        if ((offset & 0x3) != 0) break; // alignment check
        uint16_t addr = (offset + 0x80) & 0xFFFF;
        float* pMtx = nullptr;
        if (addr < 0x40) {
            pMtx = &geo.modelViewStack[geo.modelViewStackDepth].m[0][0];
        } else if (addr < 0x80) {
            pMtx = &geo.projection.m[0][0];
            addr -= 0x40;
        } else if (addr < 0xC0) {
            pMtx = &geo.combinedMatrix.m[0][0];
            addr -= 0x80;
        } else {
            break;
        }
        // Decode two halfwords from 'value'
        uint16_t half0 = static_cast<uint16_t>((value >> 16) & 0xFFFF);
        uint16_t half1 = static_cast<uint16_t>(value & 0xFFFF);
        uint32_t idx = (addr < 0x20) ? (addr >> 1) : ((addr - 0x20) >> 1);
        if (idx + 1 >= 16) break; // bounds check
        for (uint32_t ii = 0; ii < 2; ii++) {
            uint16_t h = (ii == 0) ? half0 : half1;
            // Reconstruct s15.16 fixed-point from current float
            int32_t curFixed = static_cast<int32_t>(pMtx[idx + ii] * 65536.0f);
            if (addr < 0x20) {
                // Replace integer part, keep fractional
                int16_t newInt = static_cast<int16_t>(h);
                uint16_t curFrac = static_cast<uint16_t>(curFixed & 0xFFFF);
                int32_t newFixed = (static_cast<int32_t>(newInt) << 16) | curFrac;
                pMtx[idx + ii] = static_cast<float>(newFixed) / 65536.0f;
            } else {
                // Replace fractional part, keep integer
                int16_t curInt = static_cast<int16_t>((curFixed >> 16) & 0xFFFF);
                int32_t newFixed = (static_cast<int32_t>(curInt) << 16) | h;
                pMtx[idx + ii] = static_cast<float>(newFixed) / 65536.0f;
            }
        }
        geo.combinedDirty = true;
        ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
        break;
    }
    
    case MoveWord::MW_FORCEMTX:
    {
        // GLideN64: w1==0 → mark matrix changed (recompute from MV*P)
        //           w1!=0 → mark matrix NOT changed (keep forced matrix)
        if (value == 0) {
            geo.combinedDirty = true;
            ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
        } else {
            geo.combinedDirty = false;
        }
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
        // Normalize lookat vector
        float llen = std::sqrt(geo.lookat[n][0]*geo.lookat[n][0] +
                               geo.lookat[n][1]*geo.lookat[n][1] +
                               geo.lookat[n][2]*geo.lookat[n][2]);
        if (llen > 0.0001f) {
            float linv = 1.0f / llen;
            geo.lookat[n][0] *= linv;
            geo.lookat[n][1] *= linv;
            geo.lookat[n][2] *= linv;
        }
        // Match GLideN64: loading lookat[0] always enables; lookat[1] enables if x or y non-zero
        if (n == 0) {
            geo.lookatEnabled = true;
        } else {
            geo.lookatEnabled = (geo.lookat[1][0] != 0.0f || geo.lookat[1][1] != 0.0f);
        }
        break;
    }
    
    default:
    {
        // Light data: G_MV_L0..G_MV_L7
        if (index >= MoveMem::G_MV_L0 && index <= MoveMem::G_MV_L7) {
            int lightIdx = (index - MoveMem::G_MV_L0) / 2;
            if (lightIdx >= 0 && lightIdx < MAX_LIGHTS && address + 16 <= ctx.rdramSize) {
                auto& light = geo.lights[lightIdx];
                // Primary color (N64 bytes 0-2)
                light.color[0] = static_cast<float>(ctx.ReadByte(address + 0)) / 255.0f;
                light.color[1] = static_cast<float>(ctx.ReadByte(address + 1)) / 255.0f;
                light.color[2] = static_cast<float>(ctx.ReadByte(address + 2)) / 255.0f;
                // Secondary color (N64 bytes 4-6) — N64 alternates between
                // primary and secondary colors for even/odd vertices.
                light.color2[0] = static_cast<float>(ctx.ReadByte(address + 4)) / 255.0f;
                light.color2[1] = static_cast<float>(ctx.ReadByte(address + 5)) / 255.0f;
                light.color2[2] = static_cast<float>(ctx.ReadByte(address + 6)) / 255.0f;
                
                // Direction (N64 bytes 8-10, signed int8, normalized)
                light.direction[0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                light.direction[1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                light.direction[2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
                // Normalize direction
                float len = std::sqrt(light.direction[0]*light.direction[0] +
                                      light.direction[1]*light.direction[1] +
                                      light.direction[2]*light.direction[2]);
                if (len > 0.0001f) {
                    float inv = 1.0f / len;
                    light.direction[0] *= inv;
                    light.direction[1] *= inv;
                    light.direction[2] *= inv;
                }
                
                // Point light data (F3DEX2 extended format)
                // Position: N64 halfwords at bytes 8-9, 10-11, 12-13
                light.position[0] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(address + 8)));
                light.position[1] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(address + 10)));
                light.position[2] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(address + 12)));
                // Attenuation: N64 bytes 3 (ca), 7 (la), 14 (qa)
                light.ca = static_cast<float>(ctx.ReadByte(address + 3));
                light.la = static_cast<float>(ctx.ReadByte(address + 7));
                light.qa = static_cast<float>(ctx.ReadByte(address + 14));
                
                ctx.state->MarkDirty(RDPDirtyFlag::Lights);
            }
        }
        break;
    }
    
    case MoveMem::G_MV_MATRIX_1:
    {
        // gSPForceMatrix (GBI1) — directly sets the combined matrix (MVP)
        if (address + 64 <= ctx.rdramSize) {
            auto& geo = ctx.state->geometry;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    int idx = i * 4 + j;
                    int16_t intPart = static_cast<int16_t>(ctx.ReadHalf(address + idx * 2));
                    uint16_t fracPart = ctx.ReadHalf(address + 32 + idx * 2);
                    int32_t fixed = (static_cast<int32_t>(intPart) << 16) | fracPart;
                    geo.combinedMatrix.m[i][j] = static_cast<float>(fixed) / 65536.0f;
                }
            }
            geo.combinedDirty = false; // Prevent UpdateCombinedMatrix from overwriting
            
            // Widescreen hack
            if (geo.widescreenScale != 1.0f) {
                geo.combinedMatrix.m[0][0] /= geo.widescreenScale;
            }
            ctx.state->MarkDirty(RDPDirtyFlag::Matrix);
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
        // LookAt X is at offset 0 (index = -2)
        // LookAt Y is at offset 24 (index = -1)
        int lightIdx = (offset / 24) - 2;
        if (lightIdx >= -2 && lightIdx < MAX_LIGHTS && address + 16 <= ctx.rdramSize) {
            if (lightIdx >= 0) {
                auto& light = geo.lights[lightIdx];
                // Primary color (N64 bytes 0-2)
                light.color[0] = static_cast<float>(ctx.ReadByte(address + 0)) / 255.0f;
                light.color[1] = static_cast<float>(ctx.ReadByte(address + 1)) / 255.0f;
                light.color[2] = static_cast<float>(ctx.ReadByte(address + 2)) / 255.0f;
                // Secondary color (N64 bytes 4-6)
                light.color2[0] = static_cast<float>(ctx.ReadByte(address + 4)) / 255.0f;
                light.color2[1] = static_cast<float>(ctx.ReadByte(address + 5)) / 255.0f;
                light.color2[2] = static_cast<float>(ctx.ReadByte(address + 6)) / 255.0f;
                
                // Direction (N64 bytes 8-10, signed int8)
                light.direction[0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                light.direction[1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                light.direction[2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
                // Normalize direction
                float len = std::sqrt(light.direction[0]*light.direction[0] +
                                      light.direction[1]*light.direction[1] +
                                      light.direction[2]*light.direction[2]);
                if (len > 0.0001f) {
                    float inv = 1.0f / len;
                    light.direction[0] *= inv;
                    light.direction[1] *= inv;
                    light.direction[2] *= inv;
                }
                
                // Point light data (F3DEX2 extended format)
                light.position[0] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(address + 8)));
                light.position[1] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(address + 10)));
                light.position[2] = static_cast<float>(static_cast<int16_t>(ctx.ReadHalf(address + 12)));
                light.ca = static_cast<float>(ctx.ReadByte(address + 3));
                light.la = static_cast<float>(ctx.ReadByte(address + 7));
                light.qa = static_cast<float>(ctx.ReadByte(address + 14));
            } else if (lightIdx == -1) {
                // LookAt Y (same format as light direction at offset 8)
                geo.lookat[1][0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                geo.lookat[1][1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                geo.lookat[1][2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
                // Normalize
                float llen = std::sqrt(geo.lookat[1][0]*geo.lookat[1][0] + geo.lookat[1][1]*geo.lookat[1][1] + geo.lookat[1][2]*geo.lookat[1][2]);
                if (llen > 0.0001f) { float li = 1.0f/llen; geo.lookat[1][0]*=li; geo.lookat[1][1]*=li; geo.lookat[1][2]*=li; }
                // Match GLideN64: loading lookat[1] enables if x or y non-zero
                geo.lookatEnabled = (geo.lookat[1][0] != 0.0f || geo.lookat[1][1] != 0.0f);
            } else if (lightIdx == -2) {
                // LookAt X (same format as light direction at offset 8)
                geo.lookat[0][0] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 8))) / 127.0f;
                geo.lookat[0][1] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 9))) / 127.0f;
                geo.lookat[0][2] = static_cast<float>(static_cast<int8_t>(ctx.ReadByte(address + 10))) / 127.0f;
                // Normalize
                float llen = std::sqrt(geo.lookat[0][0]*geo.lookat[0][0] + geo.lookat[0][1]*geo.lookat[0][1] + geo.lookat[0][2]*geo.lookat[0][2]);
                if (llen > 0.0001f) { float li = 1.0f/llen; geo.lookat[0][0]*=li; geo.lookat[0][1]*=li; geo.lookat[0][2]*=li; }
                // Match GLideN64: loading lookat[0] always enables
                geo.lookatEnabled = true;
            }
            
            ctx.state->MarkDirty(RDPDirtyFlag::Lights);
        }
        break;
    }
    
    case MoveMem::G_MV2_MATRIX:
    {
        // gSPForceMatrix — directly sets the combined matrix (MVP), bypassing
        // the separate ModelView/Projection stacks.
        if (address + 64 <= ctx.rdramSize) {
            auto& geo = ctx.state->geometry;
            uint32_t resolved = ctx.ResolveAddress(address);
            if (resolved + 64 <= ctx.rdramSize) {
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 4; ++j) {
                        int idx = i * 4 + j;
                        int16_t intPart = static_cast<int16_t>(ctx.ReadHalf(resolved + idx * 2));
                        uint16_t fracPart = ctx.ReadHalf(resolved + 32 + idx * 2);
                        int32_t fixed = (static_cast<int32_t>(intPart) << 16) | fracPart;
                        geo.combinedMatrix.m[i][j] = static_cast<float>(fixed) / 65536.0f;
                    }
                }
                // Mark NOT dirty so UpdateCombinedMatrix() won't overwrite
                geo.combinedDirty = false;

                // Widescreen hack: scale combined matrix X focal length.
                // Only m[0][0] — don't touch the full column, as ForceMatrix
                // is used for 3D camera matrices where shifting X translation
                // would break the view.
                if (geo.widescreenScale != 1.0f) {
                    geo.combinedMatrix.m[0][0] /= geo.widescreenScale;
                }
            }
        }
        // ForceMatrix uses two consecutive display list commands (16 bytes).
        // The second is G_MOVEWORD(MW_FORCEMTX, 0) which would re-dirty the
        // combined matrix, undoing the force. Skip it (matches GLideN64).
        ctx.AdvancePC(2);
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
    
    uint32_t mask = (length >= 32) ? 0xFFFFFFFFu : (((uint32_t)1 << length) - 1) << shift;
    ctx.state->otherModeH = (ctx.state->otherModeH & ~mask) | (value & mask);
    ctx.state->UpdateDerivedOtherModes();
    ctx.state->MarkDirty(RDPDirtyFlag::OtherModes);
}

void GBIHandler::SetOtherModeL(DListContext& ctx, uint32_t shift, uint32_t length, uint32_t value) {
    if (!ctx.state) return;
    
    uint32_t mask = (length >= 32) ? 0xFFFFFFFFu : (((uint32_t)1 << length) - 1) << shift;
    ctx.state->otherModeL = (ctx.state->otherModeL & ~mask) | (value & mask);
    ctx.state->UpdateDerivedOtherModes();
    ctx.state->MarkDirty(RDPDirtyFlag::OtherModes);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debug flags — disable specific culling/branching mechanisms for diagnosis.
//  Set to true one at a time to narrow down geometry disappearing issues.
// ═══════════════════════════════════════════════════════════════════════════
static constexpr bool DEBUG_DISABLE_CULLDL     = false; // disable CullDisplayList
static constexpr bool DEBUG_DISABLE_BRANCH     = false;  // force-disable BranchZ/BranchW: always render high-detail DL (fixes MM building / distant texture pop-out)
static constexpr bool DEBUG_DISABLE_TRI_REJECT = false; // disable face culling + AND-rejection in DrawTriangle

// ═══════════════════════════════════════════════════════════════════════════
//  Triangle Drawing
// ═══════════════════════════════════════════════════════════════════════════

void GBIHandler::DrawTriangle(DListContext& ctx, int v0, int v1, int v2) {
    if (!ctx.state || !ctx.rdpProcessor) { return; }
    
    {
    auto& geo = ctx.state->geometry;
    int maxVtx = VERTEX_BUFFER_SIZE;
    
    // Validate indices
    if (v0 >= maxVtx || v1 >= maxVtx || v2 >= maxVtx) {
        return;
    }
    
    // Backface culling in screen space (like GLideN64) for numerical stability.
    // NDC-space cross products lose precision for nearly edge-on polygons.
    if (!DEBUG_DISABLE_TRI_REJECT &&
        (geo.geometryMode & (G_CULL_FRONT | G_CULL_BACK)) &&
        !ctx.rdpProcessor->IsDebugDisableCulling()) {
        const auto& tv0 = geo.vertexBuffer[v0];
        const auto& tv1 = geo.vertexBuffer[v1];
        const auto& tv2 = geo.vertexBuffer[v2];
        
        // Skip culling if any vertex is behind the camera (W < 0) or has
        // W too close to zero.  When W < 0 the perspective divide flips the
        // screen-space position, inverting the triangle's apparent winding
        // and causing false backface rejections.  These triangles will be
        // properly clipped by the Sutherland-Hodgman clipper below.
        float w0 = tv0.position[3];
        float w1 = tv1.position[3];
        float w2 = tv2.position[3];
        constexpr float wEps = 1e-5f;
        if (w0 > wEps && w1 > wEps && w2 > wEps) {
            float w0inv = 1.0f / w0;
            float w1inv = 1.0f / w1;
            float w2inv = 1.0f / w2;
            
            // Transform to screen space (viewport transform) for stable cross product
            float sx = geo.viewport.scaleX;
            float sy = geo.viewport.scaleY;
            float tx = geo.viewport.transX;
            float ty = geo.viewport.transY;
            
            float x0 = tv0.position[0] * w0inv * sx + tx;
            float y0 = tv0.position[1] * w0inv * sy + ty;
            float x1 = tv1.position[0] * w1inv * sx + tx;
            float y1 = tv1.position[1] * w1inv * sy + ty;
            float x2 = tv2.position[0] * w2inv * sx + tx;
            float y2 = tv2.position[1] * w2inv * sy + ty;
            
            float cross = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            
            // scaleY is negative (Y-flip), so the cross product sign is
            // flipped compared to NDC.  CULL_BACK rejects CCW screen-space
            // triangles (cross > 0), CULL_FRONT rejects CW (cross < 0).
            if ((geo.geometryMode & G_CULL_BACK) && cross > 0.0f) {
                return;
            }
            if ((geo.geometryMode & G_CULL_FRONT) && cross < 0.0f) {
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
    if (!DEBUG_DISABLE_TRI_REJECT && (tv0c.clipFlags & tv1c.clipFlags & tv2c.clipFlags)) {
        return;
    }
    
    // Skipping X/Y software clipping (only running W-near via the
    // CLIP_W mask) caused texture flicker / "vibration" from certain
    // camera angles: when a triangle straddled the screen edge AND
    // had a vertex very close to the W-near plane, the W-clip produced
    // a new vertex that inherited huge X/Y from the original; the
    // GPU then rasterised across millions of texels worth of
    // screen space, producing aliased garbage in the visible area.
    // Re-enabling the full 5-plane Sutherland-Hodgman clipper fixes
    // this without measurable perf cost (most triangles still trivial-
    // accept via orFlags == 0).
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
        int triCount = ClipTriangle(toClipVert(tv0c), toClipVert(tv1c), toClipVert(tv2c), poly, geo.clipRatio);
        
        if (triCount > 0 && poly.vertexCount >= 3) {
            // Fan-triangulate clipped polygon (vertex 0 is the hub)
            bool clipTextPersp = (ctx.state->otherModeH & 0x00080000u) != 0; // OTHERMODE_TEXTPERSP
            auto toRenderVert = [&geo, clipTextPersp](const ClipVertex& cv) -> RenderBatch::Vertex {
                RenderBatch::Vertex rv{};
                // Perspective divide before viewport transform (same as makeVert in RDPProcessor)
                float invW = (cv.position[3] != 0.0f) ? 1.0f / cv.position[3] : 1.0f;
                rv.x = cv.position[0] * invW * geo.viewport.scaleX + geo.viewport.transX;
                rv.y = cv.position[1] * invW * geo.viewport.scaleY + geo.viewport.transY;
                // Snap intentionally disabled here too — see makeVert in
                // RDPProcessor::DrawTriangle for the rationale.
                {
                    float z_screen = cv.position[2] * invW * geo.viewport.scaleZ + geo.viewport.transZ;
                    float absScaleZ = std::abs(geo.viewport.scaleZ);
                    rv.z = (absScaleZ > 0.0001f)
                        ? (z_screen - geo.viewport.transZ + absScaleZ) / (2.0f * absScaleZ)
                        : 0.0f;
                    rv.z = std::max(0.0f, std::min(1.0f, rv.z));
                    // Match makeVert: bias away from 1.0 so far-plane geometry
                    // passes GL_LESS (which is used for opaque Z mode).
                    if (rv.z >= 1.0f) rv.z = 0.99999f;
                }
                rv.w = 1.0f;
                rv.r = cv.color[0]; rv.g = cv.color[1];
                rv.b = cv.color[2]; rv.a = cv.color[3];
                // Pre-divide texcoords by clip_W for perspective-correct interpolation.
                // Apply gSPTexture scale at draw time (same convention as HLE makeVert).
                float scaleS = geo.texture.enabled ? geo.texture.GetScaleSFloat() : 1.0f;
                float scaleT = geo.texture.enabled ? geo.texture.GetScaleTFloat() : 1.0f;
                bool textPersp = clipTextPersp;
                float tcW = textPersp ? invW : 1.0f;
                float affineHalf = textPersp ? 1.0f : 0.5f;
                rv.s  = cv.texCoord.x  * scaleS * tcW * affineHalf; rv.t  = cv.texCoord.y  * scaleT * tcW * affineHalf;
                rv.s1 = cv.texCoord1.x * scaleS * tcW * affineHalf; rv.t1 = cv.texCoord1.y * scaleT * tcW * affineHalf;
                rv.perspW = tcW;
                rv.fogFactor = cv.fog;
                rv.clipFlags = 0; // Already clipped
                return rv;
            };
            
            // Post-clip face culling: the pre-clip culling was skipped
            // when any vertex had W ≤ epsilon (behind camera).  Now that
            // all clipped vertices have valid W > 0, we can reliably
            // determine screen-space winding on each sub-triangle.
            // Without this, backface geometry (e.g. the inside of walls)
            // passes through unchecked when the camera clips through
            // objects during cutscenes, covering the camera view.
            bool doCull = (geo.geometryMode & (G_CULL_FRONT | G_CULL_BACK)) != 0
                       && !ctx.rdpProcessor->IsDebugDisableCulling();

            for (int i = 1; i + 1 < poly.vertexCount; ++i) {
                RenderBatch::Vertex rv0 = toRenderVert(poly.vertices[0]);
                RenderBatch::Vertex rv1 = toRenderVert(poly.vertices[i]);
                RenderBatch::Vertex rv2 = toRenderVert(poly.vertices[i + 1]);

                if (doCull) {
                    float cross = (rv1.x - rv0.x) * (rv2.y - rv0.y)
                                - (rv2.x - rv0.x) * (rv1.y - rv0.y);
                    // scaleY is negative → screen-space winding is flipped.
                    // CULL_BACK rejects CCW (cross > 0), CULL_FRONT rejects CW (cross < 0).
                    if ((geo.geometryMode & G_CULL_BACK) && cross > 0.0f) continue;
                    if ((geo.geometryMode & G_CULL_FRONT) && cross < 0.0f) continue;
                }

                ctx.rdpProcessor->DrawTriangleRaw(
                    rv0, rv1, rv2,
                    geo.geometryMode,
                    geo.texture.tileIndex);
            }
            m_trianglesProcessed += (poly.vertexCount - 2);
        }
    }
    } // end clip rejection block
    } // end geo scope
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
        // Branch: replace current DL (no push).
        // Detect self-branch (infinite loop) — halt instead.
        if (address == ctx.pc - 8) {
            ctx.halted = true;
            return;
        }
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
        // GLideN64 uses _FIXED2FLOATCOLOR which divides by 256 (not 255)
        v.color[0] = static_cast<float>((value >> 24) & 0xFF) / 256.0f;
        v.color[1] = static_cast<float>((value >> 16) & 0xFF) / 256.0f;
        v.color[2] = static_cast<float>((value >> 8) & 0xFF) / 256.0f;
        v.color[3] = static_cast<float>(value & 0xFF) / 256.0f;
        break;
        
    case 0x14: // G_MWO_POINT_ST
    {
        int16_t s = static_cast<int16_t>((value >> 16) & 0xFFFF);
        int16_t t = static_cast<int16_t>(value & 0xFFFF);
        // GLideN64 reference (gSPModifyVertex G_MWO_POINT_ST):
        //   vtx.s = _FIXED2FLOAT(s, 5) / gSP.texture.scales
        //   vtx.t = _FIXED2FLOAT(t, 5) / gSP.texture.scalet
        // Pre-divide by texture scale because makeVert multiplies by scale
        // at draw time. Without this division, coordinates are double-scaled.
        float scaleS = geo.texture.GetScaleSFloat();
        float scaleT = geo.texture.GetScaleTFloat();
        v.texCoord[0] = (static_cast<float>(s) / 32.0f) / scaleS;
        v.texCoord[1] = (static_cast<float>(t) / 32.0f) / scaleT;
        break;
    }
    
    case 0x18: // G_MWO_POINT_XYSCREEN — sets screen-space X/Y directly
    {
        // These are already in screen-space (10.2 fixed-point).
        // Store them in screenX/Y and flag the vertex so makeVert
        // skips the viewport transform for X/Y while keeping the
        // original clip-space Z/W for depth and perspective texturing.
        int16_t x = static_cast<int16_t>((value >> 16) & 0xFFFF);
        int16_t y = static_cast<int16_t>(value & 0xFFFF);
        v.screenX = static_cast<float>(x) / 4.0f;
        v.screenY = static_cast<float>(y) / 4.0f;

        v.modifiedXY = true;
        v.clipFlags = 0;
        break;
    }
    
    case 0x1C: // G_MWO_POINT_ZSCREEN — sets screen-space Z directly
    {
        // GLideN64: scrZ = float(_val) / 65535.0f / 2048.0f
        // The full 32-bit value is used as a normalized depth.
        float scrZ = static_cast<float>(value) / 65535.0f / 2048.0f;
        v.screenZ = scrZ;
        v.modifiedZ = true;
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
    if constexpr (DEBUG_DISABLE_CULLDL) return;
    if (!ctx.state) return;
    auto& geo = ctx.state->geometry;
    
    // Some games (Aidyn Chronicles) pass parameters in reverse order.
    if (endVtx < startVtx)
        std::swap(startVtx, endVtx);
    
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
    if constexpr (DEBUG_DISABLE_BRANCH) return;
    if (!ctx.state) return;

    auto& geo = ctx.state->geometry;
    if (vtxIndex >= VERTEX_BUFFER_SIZE) return;
    const auto& tv = geo.vertexBuffer[vtxIndex];
    float w = tv.position[3];
    if (w == 0.0f) return; // vertex at infinity — don't branch
    // GLideN64 reference (gSPBranchLessZ):
    //   zTest = u32((v.z / v.w) * 1023.0f)
    //   branch if zTest > 0x03FF || zTest <= zval
    uint32_t zTest = static_cast<uint32_t>((tv.position[2] / w) * 1023.0f);
    if (zTest > 0x03FFu || zTest <= zValue) {
        ctx.pc = ctx.ResolveAddress(address);
    }
}

void GBIHandler::BranchW(DListContext& ctx, uint32_t vtxIndex, uint32_t wValue, uint32_t address) {
    if constexpr (DEBUG_DISABLE_BRANCH) return;
    if (!ctx.state) return;

    auto& geo = ctx.state->geometry;
    if (vtxIndex >= VERTEX_BUFFER_SIZE) return;
    float vertexW = geo.vertexBuffer[vtxIndex].position[3];
    // GLideN64 reference (gSPBranchLessW): branch if v.w < (float)wval
    float wThreshold = static_cast<float>(wValue);
    if (vertexW < wThreshold) {
        ctx.pc = ctx.ResolveAddress(address);
    }
}

void GBIHandler::LoadUcode(DListContext& ctx, uint32_t address, uint32_t dataAddress) {
    // Dynamic microcode loading � switch microcode mid-frame
    // Demoted to Debug: MM/OoT issue G_LOAD_UCODE every frame (HUD<->world
    // ucode swap), which floods INFO-level logs with thousands of lines.
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
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
    case GBI1::G_MOVEWORD:      return "G_MOVEWORD index=" + std::to_string(w0 & 0xFF) + " offset=" + std::to_string((w0 >> 8) & 0xFFFF) + " val=0x" + std::to_string(w1);
    case GBI1::G_SETOTHERMODE_H: return "G_SETOTHERMODE_H";
    case GBI1::G_SETOTHERMODE_L: return "G_SETOTHERMODE_L";
    case GBI1::G_SETGEOMETRYMODE: return "G_SETGEOMETRYMODE";
    case GBI1::G_CLEARGEOMETRYMODE: return "G_CLEARGEOMETRYMODE";
    case GBI1::G_RDPHALF_1:     return "G_RDPHALF_1 val=0x" + std::to_string(w1);
    case GBI1::G_RDPHALF_2:     return "G_RDPHALF_2 val=0x" + std::to_string(w1);
    case GBI1::G_ENDDL:         return "G_ENDDL";
    case 0xAF:                  return "G_LOAD_UCODE";
    case 0xDB:                  return "G_MOVEWORD (GBI2_STYLE)";
    default: {
        if (opcode >= 0xC0) {
            uint8_t rdpOp = (w0 >> 24) & 0x3F;
            switch(rdpOp) {
                case 0x3F: return "G_SETCIMG";
                case 0x3E: return "G_SETZIMG";
                case 0x3D: return "G_SETTIMG";
                case 0x3B: return "G_SETCOMBINE";
                case 0x33: return "G_LOADBLOCK";
                case 0x34: return "G_LOADTILE";
                case 0x2F: return "G_SETCOLOR";
                case 0x24: return "G_TEXRECT";
                case 0x22: return "G_FILLRECT";
                default: return "RDP_CMD_0x" + std::to_string(rdpOp);
            }
        }
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
