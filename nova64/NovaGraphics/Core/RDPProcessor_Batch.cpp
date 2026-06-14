#include "RDPProcessor_Private.hpp"
void RDPProcessor::FlushBatch() {
    if (m_batch.drawCalls.empty()) return;
    if (!m_renderer) {
        m_batch.Reset();
        return;
    }

    // ═══════════════════════════════════════════════════════════════════
    //  MLE Director — classify batch before GPU submission
    // ═══════════════════════════════════════════════════════════════════
    bool batchNeedsMleBlender = false;  // true → force FSI blender for this batch
    if (m_mleDirector) {
        BatchInfo bi{};
        bi.drawCallCount = static_cast<uint32_t>(m_batch.drawCalls.size());
        for (const DrawCall& dc : m_batch.drawCalls) {
            bi.triangleCount  += static_cast<uint32_t>(dc.type == DrawCall::Type::Triangle
                                     ? dc.vertexCount / 3 : 0);
            bi.hasFillOps     |= (dc.type == DrawCall::Type::FillRect);
            bi.hasCopyOps     |= (dc.type == DrawCall::Type::TextureRect && dc.textureHash0 != 0);
            bi.hasTexturedTri |= (dc.type == DrawCall::Type::Triangle    && dc.textureHash0 != 0);
            bi.hasTranslucent |= ((dc.otherModeL & (1u << 6)) != 0);  // ALPHA_CVG_SEL
            if (bi.combineModeRaw == 0) bi.combineModeRaw = dc.combineMode;
            bi.otherModeH  |= dc.otherModeH;
            bi.otherModeL  |= dc.otherModeL;
            bi.geometryMode |= dc.geometryMode;
        }
        MLELevel level = m_mleDirector->Evaluate(bi, m_state.rdp);
        // HLE_Accurate or LLE: force the FSI imageLoad/imageStore blender path
        // for this entire batch so blending is pixel-accurate regardless of
        // whether per-draw needsMem analysis would have selected it.
        batchNeedsMleBlender = (level >= MLELevel::HLE_Accurate);
    }

    // Mark the current render-target framebuffer as GPU-modified so that
    // subsequent LoadBlock/LoadTile calls know to sync GPU→RDRAM.
    if (auto* curFB = g_fbManager.GetCurrentColor())
        curFB->gpuModified = true;

    // ═══════════════════════════════════════════════════════════════════
    //  1. Upload vertex data to a dynamic VBO
    // ═══════════════════════════════════════════════════════════════════
    //
    // RenderBatch::Vertex layout (44 bytes):
    //   float x, y, z, w   � position   (offset 0,  4 floats)
    //   float s, t          � texcoord   (offset 16, 2 floats)
    //   float r, g, b, a    � color      (offset 24, 4 floats)
    //   float fogFactor     � fog        (offset 40, 1 float)
    //
    // The N64 VertexLayout from Renderer.hpp expects:
    //   loc 0: float4 position (offset 0)
    //   loc 1: float4 color    (offset 16)
    //   loc 2: float2 texcoord0(offset 32)
    //   loc 3: float2 texcoord1(offset 40)
    //   loc 4: float1 fog      (offset 48)
    //   stride = 52
    //
    // We convert RenderBatch::Vertex → interleaved GPU vertex format.

    struct GPUVertex {
        float x, y, z, w;      // position (loc 0)
        float r, g, b, a;      // color (loc 1)
        float s0, t0;          // texcoord0 (loc 2)
        float s1, t1;          // texcoord1 (loc 3) � same as tex0 for now
        float fog;              // fog (loc 4)
        float perspW;           // 1/clip_W for perspective-correct textures (loc 5)
        float baryS, baryT;    // barycentric coords for sub-pixel coverage AA (loc 6)
    };
    static_assert(sizeof(GPUVertex) == 64, "GPUVertex must be 64 bytes for N64 layout");

    const size_t vertexCount = m_batch.vertices.size();
    const size_t vbSize = vertexCount * sizeof(GPUVertex);

    // Convert batch vertices to GPU format (reuse persistent scratch buffer)
    m_gpuVertScratch.resize(vbSize);
    GPUVertex* gpuVerts = reinterpret_cast<GPUVertex*>(m_gpuVertScratch.data());
    for (size_t i = 0; i < vertexCount; ++i) {
        const auto& src = m_batch.vertices[i];
        auto& dst = gpuVerts[i];
        dst.x  = src.x;  dst.y  = src.y;  dst.z  = src.z; dst.w = src.w;
        dst.r  = src.r;  dst.g  = src.g;  dst.b  = src.b; dst.a = src.a;
        dst.s0 = src.s;  dst.t0 = src.t;
        dst.s1 = src.s1; dst.t1 = src.t1;
        dst.fog = src.fogFactor;
        dst.perspW = src.perspW;
        dst.baryS = 0.333333f; dst.baryT = 0.333333f; // default: center (full coverage)
    }

    // Diagnostic: log vertex position/color range from early flushes with triangles.
    {
        static uint32_t s_triFlushIdx = 0;
        static const uint32_t kLogFlushes[] = {1, 10, 50, 200};
        if (vertexCount >= 3) {
            bool hasTri = false;
            for (const auto& dc : m_batch.drawCalls)
                if (dc.type == DrawCall::Type::Triangle) { hasTri = true; break; }
            if (hasTri) {
                ++s_triFlushIdx;
                bool shouldLog = false;
                for (uint32_t f : kLogFlushes) if (s_triFlushIdx == f) { shouldLog = true; break; }
                if (shouldLog) {
                    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
                    float minZ = 1e9f, maxZ = -1e9f;
                    float minS = 1e9f, maxS = -1e9f;  // texture S coord
                    bool hasExtreme = false;
                    for (size_t vi = 0; vi < vertexCount; ++vi) {
                        minX = std::min(minX, gpuVerts[vi].x); maxX = std::max(maxX, gpuVerts[vi].x);
                        minY = std::min(minY, gpuVerts[vi].y); maxY = std::max(maxY, gpuVerts[vi].y);
                        minZ = std::min(minZ, gpuVerts[vi].z); maxZ = std::max(maxZ, gpuVerts[vi].z);
                        minS = std::min(minS, gpuVerts[vi].s0); maxS = std::max(maxS, gpuVerts[vi].s0);
                        if (gpuVerts[vi].x < -1000.f || gpuVerts[vi].x > 1000.f ||
                            gpuVerts[vi].y < -1000.f || gpuVerts[vi].y > 1000.f)
                            hasExtreme = true;
                    }
                    Log::infof("[GPURANGE#%u] verts=%zu x=[%.1f,%.1f] y=[%.1f,%.1f] z=[%.3f,%.3f] s=[%.2f,%.2f] extreme=%d",
                               s_triFlushIdx, vertexCount, minX, maxX, minY, maxY, minZ, maxZ, minS, maxS, (int)hasExtreme);
                    // Log first 3 vertices
                    for (size_t vi = 0; vi < std::min((size_t)3, vertexCount); ++vi) {
                        Log::infof("[GPUV#%u.%zu] x=%.2f y=%.2f z=%.3f s=%.4f t=%.4f pw=%.4f r=%.4f g=%.4f b=%.4f",
                                   s_triFlushIdx, vi,
                                   gpuVerts[vi].x, gpuVerts[vi].y, gpuVerts[vi].z,
                                   gpuVerts[vi].s0, gpuVerts[vi].t0, gpuVerts[vi].perspW,
                                   gpuVerts[vi].r, gpuVerts[vi].g, gpuVerts[vi].b);
                    }
                }
            }
        }
    }

    // Assign barycentric coordinates per draw call for sub-pixel coverage AA.
    // Triangles get (1,0), (0,1), (0,0) so the fragment shader can compute
    // 8-sample edge coverage. Non-triangle draws (rects) keep the center
    // value (1/3,1/3) which guarantees full coverage (no edge AA on rects).
    static const float kTriBary[3][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}};
    for (const auto& dc : m_batch.drawCalls) {
        if (dc.type == DrawCall::Type::Triangle) {
            for (int j = 0; j < dc.vertexCount; j += 3) {
                int base = dc.vertexStart + j;
                if (base + 2 < static_cast<int>(vertexCount)) {
                    gpuVerts[base + 0].baryS = kTriBary[0][0]; gpuVerts[base + 0].baryT = kTriBary[0][1];
                    gpuVerts[base + 1].baryS = kTriBary[1][0]; gpuVerts[base + 1].baryT = kTriBary[1][1];
                    gpuVerts[base + 2].baryS = kTriBary[2][0]; gpuVerts[base + 2].baryT = kTriBary[2][1];
                }
            }
        }
    }

    // --- Ring-buffer VBO: allocate once, sub-upload each flush ---
    if (!m_ringVBO) {
        m_ringVBO = m_renderer->CreateVertexBuffer(RING_VBO_SIZE, BufferUsage::Stream);
        m_ringOffset = 0;
        if (!m_ringVBO) {
            m_batch.Reset();
            return;
        }
    }

    // If the current batch doesn't fit in the remaining ring space, wrap to 0
    // (orphan the old data by re-uploading from offset 0).
    if (m_ringOffset + vbSize > RING_VBO_SIZE) {
        m_ringOffset = 0;
    }

    const size_t batchBaseVertex = m_ringOffset / sizeof(GPUVertex);
    m_ringVBO->Update(gpuVerts, m_ringOffset, vbSize);
    m_ringOffset += vbSize;

    VertexLayout layout = VertexLayout::CreateN64();

    // Lazily create the N64 pipeline (owns the VAO with the correct vertex format).
    // BindPipeline sets m_boundVAO, which BindVertexBuffer requires.
    if (!m_n64Pipeline) {
        PipelineDesc pipeDesc{};
        pipeDesc.vertexLayout = layout;
        pipeDesc.primitiveType = PrimitiveType::Triangles;
        pipeDesc.debugName = "N64_RDP_Default";
        // Disable back-face culling: the N64 RSP already handles culling, and the
        // ortho Y-flip reverses triangle winding (CW in NDC), which would wrongly
        // discard all geometry with the default CullMode::Back setting.
        pipeDesc.rasterizer.cullMode = CullMode::None;
        m_n64Pipeline = m_renderer->CreatePipeline(pipeDesc);
    }
    if (m_n64Pipeline) {
        m_renderer->BindPipeline(m_n64Pipeline.get());
    }

    m_renderer->BindVertexBuffer(m_ringVBO.get(), layout);

    // Set the viewport to match the FBO dimensions
    {
        GPUFramebuffer* curFBO_vp = g_fbManager.GetCurrentColor();
        float vpW = (curFBO_vp && curFBO_vp->scaledWidth  > 0) ? static_cast<float>(curFBO_vp->scaledWidth)  : 320.0f;
        float vpH = (curFBO_vp && curFBO_vp->scaledHeight > 0) ? static_cast<float>(curFBO_vp->scaledHeight) : 240.0f;
        RenderViewport vp;
        vp.x = 0; vp.y = 0; vp.width = vpW; vp.height = vpH;
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        m_renderer->SetViewport(vp);
    }

    // =======================================================================
    //  2. Iterate draw calls, set state, and draw
    // =======================================================================

    // Compute N64 screen dimensions for the ortho projection.
    // CRITICAL: must use the active color framebuffer dimensions, NOT the
    // scissor.  The scissor is for fragment clipping and changes frequently
    // (per draw call, per frame) — using it as the projection range causes
    // every vertex we've already snapped to N64 screen coords to be remapped
    // to NDC with a different scale each time, producing visible per-frame
    // wobble / "vibrating textures".  The FBO size is the canonical N64
    // render target and stays constant for the duration of a scene.
    GPUFramebuffer* projFB = g_fbManager.GetCurrentColor();
    float n64W = projFB ? static_cast<float>(projFB->width)  : 0.0f;
    float n64H = projFB ? static_cast<float>(projFB->height) : 0.0f;
    if (n64W < 1.0f) n64W = static_cast<float>(m_state.rdp.scissor.xl >> 2);
    if (n64H < 1.0f) n64H = static_cast<float>(m_state.rdp.scissor.yl >> 2);
    if (n64W < 1.0f) n64W = 320.0f;
    if (n64H < 1.0f) n64H = 240.0f;

    // Intel Iris Xe workaround: FBOs created lazily (after game textures are
    // loaded) can fail texture sampling. GL-only: Recreating the FBO here � during the
    // rendering phase � produces a fresh GL object that works correctly.
    {
        static std::unordered_set<uint32_t> recreatedFBOs;
        GPUFramebuffer* curFB = g_fbManager.GetCurrentColor();
        if (g_fbManager.IsGL() && curFB && curFB->fbo != 0 && recreatedFBOs.find(curFB->fbo) == recreatedFBOs.end()) {
            g_fbManager.RecreateGPUFramebuffer(*curFB);
            glBindFramebuffer(GL_FRAMEBUFFER, curFB->fbo);
            glViewport(0, 0, curFB->scaledWidth, curFB->scaledHeight);
            recreatedFBOs.insert(curFB->fbo);
        }
    }

    // Ortho matrix: maps N64 screen coords (0..n64W, 0..n64H, z_norm 0..1) to NDC.
    // Z: clip_z = 2*z_norm - 1, so window_depth = z_norm ? [0,1] (full precision).
    // Near z_norm=0 � depth=0, Far z_norm=1 � depth=1.0 (= clear value).
    // GL_LEQUAL: near objects (depth � 0) correctly overwrite far objects (depth � 1).
    // Column-major for OpenGL/Vulkan: ortho(left=0, right=n64W, top=0, bottom=n64H)
    float ortho[16] = {
         2.0f/n64W, 0,          0,   0,
         0,        -2.0f/n64H,  0,   0,
         0,         0,           1,   0,
        -1.0f,      1.0f,        0,   1
    };

    // Diagnostic: one-shot log of ortho matrix and n64 dims.
    {
        static bool s_orthoLogged = false;
        if (!s_orthoLogged) {
            s_orthoLogged = true;
            Log::infof("[ORTHO] n64W=%.0f n64H=%.0f m=[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
                       n64W, n64H,
                       ortho[0],ortho[1],ortho[2],ortho[3],
                       ortho[4],ortho[5],ortho[6],ortho[7],
                       ortho[8],ortho[9],ortho[10],ortho[11],
                       ortho[12],ortho[13],ortho[14],ortho[15]);
        }
    }
    // FBO dimensions for scissor scaling - use the current color FBO size, not the window.
    // The N64 renders to a 320x240 FBO; using window dims (640x480) would double all scissors
    // The N64 renders to a 320�240 FBO; using window dims (640�480) would double all scissors
    // (scaleX = 640/320 = 2.0) causing every scissor rect to overflow the FBO entirely.
    GPUFramebuffer* curFBO = g_fbManager.GetCurrentColor();
    float fboW = (curFBO && curFBO->scaledWidth  > 0) ? static_cast<float>(curFBO->scaledWidth)  : n64W;
    float fboH = (curFBO && curFBO->scaledHeight > 0) ? static_cast<float>(curFBO->scaledHeight) : n64H;

    bool batchUsedN64Depth = false;

    // ── LLE diagnostic: track unique (combine, tex0, scissor) tuples and triangle counts ──
    struct DrawStats {
        uint32_t triCount = 0;
        uint32_t fillCount = 0;
        uint32_t lastScissorXl = 0, lastScissorYl = 0;
        uint32_t lastColorAddr = 0;
        std::unordered_map<uint64_t, uint32_t> comboCount;
    };
    static DrawStats s_ds;
    static uint32_t s_dsFrames = 0;
    static bool s_dsDumped = false;
    s_ds.lastScissorXl = m_state.rdp.scissor.xl;
    s_ds.lastScissorYl = m_state.rdp.scissor.yl;
    s_ds.lastColorAddr = m_state.rdp.colorImage.address;
    for (const auto& call : m_batch.drawCalls) {
        if (!s_dsDumped) {
            if (call.type == DrawCall::Type::Triangle) s_ds.triCount++;
            else if (call.type == DrawCall::Type::FillRect) s_ds.fillCount++;
            uint64_t key = call.combineMode ^ ((uint64_t)call.textureHash0 << 1);
            s_ds.comboCount[key]++;
        }

        // --- Build CombinerKey for shader lookup ---
        Shader::CombinerKey shaderKey{};
        shaderKey.mux = call.combineMode;
        shaderKey.blenderBits = call.otherModeL & 0xFFFF0000; // blender bits in upper portion
        shaderKey.otherModeH = static_cast<uint16_t>((call.otherModeH >> 8) & 0xFFFF);
        shaderKey.otherModeL = static_cast<uint16_t>(call.otherModeL & 0xFFFF);

        // Derive feature flags from other modes
        uint64_t features = Shader::SF_NONE;
        uint32_t cycleType = (call.otherModeH >> 20) & 0x03;

        // FillRect in FILL mode (cycleType=3) uses the fill-color shader path directly.
        // FillRect in 1-cycle/2-cycle modes should evaluate the actual combiner
        // (shade color = 0 since there's no vertex shading for rectangles).
        if (call.type == DrawCall::Type::FillRect && cycleType == 3) {
            features |= Shader::SF_FILL_RECT;
        } else if (call.type == DrawCall::Type::FillRect) {
            // Non-FILL FillRect: use the regular cycle mode so the combiner runs
            switch (cycleType) {
                case 0: features |= Shader::SF_1CYCLE; break;
                case 1: features |= Shader::SF_2CYCLE; break;
                case 2: features |= Shader::SF_COPY_MODE; break;
                default: features |= Shader::SF_FILL_RECT; break;
            }
        } else {
            switch (cycleType) {
                case 0: features |= Shader::SF_1CYCLE; break;
                case 1: features |= Shader::SF_2CYCLE; break;
                case 2: features |= Shader::SF_COPY_MODE; break;
                case 3: features |= Shader::SF_FILL_RECT; break;
            }
        }

        // Texture enable
        if (call.textureHash0 != 0) features |= Shader::SF_TEXTURE_0;
        if (call.textureHash1 != 0) features |= Shader::SF_TEXTURE_1;

        // Depth
        // N64 depth: Z_CMP (bit 4) enables depth comparison, Z_UPD (bit 5)
        // enables depth writes. G_ZBUFFER is an RSP flag that tells the RSP to
        // compute per-vertex Z � it does NOT enable the RDP's depth test.
        // When Z_UPD is set without Z_CMP, the RDP writes depth unconditionally
        // (depth init mode) � we use depthFunc=ALWAYS for that case.
        {
            bool zCmp = (call.otherModeL & 0x0010) != 0;
            bool zUpd = (call.otherModeL & 0x0020) != 0;
            bool zPrim = (call.otherModeL & 0x0004) != 0;
            uint32_t cycType = (call.otherModeH >> 20) & 0x03;
            bool depthEnabled = (zCmp || zUpd || zPrim) && (cycType <= 1);
            if (depthEnabled) features |= Shader::SF_DEPTH_TEST;
            if (zUpd) features |= Shader::SF_DEPTH_WRITE;
            if (zPrim) features |= Shader::SF_DEPTH_SOURCE_PRIM;
        }

        // N64-accurate Z-image depth compare: enable when Z_CMP is set,
        // the depth buffer manager has a current Z-image texture pair,
        // and the GPU supports fragment shader interlock for imageLoad/imageStore.
        //
        // INTEL WORKAROUND: When the MLE blender is also active (SF_MLE_BLENDER),
        // the shader would need to access 4 image bindings (Z-image, delta-Z,
        // color, coverage) within a single interlock region.  Intel Iris Xe
        // drivers silently fail when the interlock region spans too many
        // image operations, causing all depth-tested MLE draws to become
        // invisible.  Fall back to GL depth test for these draws — the
        // MLE blender still handles color/coverage blending correctly.
        //
        // INTEL WORKAROUND #2: Also skip N64 depth compare when dual-source
        // blend is active.  Intel Iris Xe silently fails when image bindings
        // (Z-image imageLoad/imageStore) are combined with dual-source
        // fragment outputs (fragColor + fragColor1) in the same shader.
        // Fall back to GL depth test (with z-bias 0.99999 for far-plane).
        if (!DISABLE_N64_DEPTH_COMPARE &&
            (call.otherModeL & 0x0010) && g_depthManager.GetCurrent() &&
            g_depthManager.GetCurrent()->glZImageTex != 0 &&
            m_renderer->GetCapabilities().supportsFragmentShaderInterlock &&
            !(features & Shader::SF_MLE_BLENDER) &&
            !(m_renderer->GetCapabilities().isIntelGPU && (features & Shader::SF_DUAL_SOURCE_BLEND))) {
            features |= Shader::SF_DEPTH_COMPARE;
            features |= Shader::SF_N64_DEPTH_COMPARE;
        }

        // Depth render: when triangles explicitly render to the depth image,
        // the combiner output (fragColor.r) is a depth value that needs to be
        // written into the Z-image and gl_FragDepth. Screen-space rectangles
        // can also target the same address as an auxiliary color buffer
        // (Majora's Mask pause/backdrop effects), so do not force them down
        // the depth-render path.
        if (m_state.rdp.colorImage.address == m_state.rdp.zbufferImage.address &&
            m_state.rdp.colorImage.address != 0 &&
            call.type == DrawCall::Type::Triangle) {
            features |= Shader::SF_DEPTH_RENDER;
        }

        // Alpha compare: bit 0 = G_AC_THRESHOLD, bit 1 = G_AC_NOISE;
        // both modes need the alpha discard test in the shader.
        // FILL mode: alpha test is always disabled (hardware ignores it).
        // COPY mode: N64 hardware ALWAYS tests texel alpha against
        // blendColor.alpha regardless of alpha compare bits � force it on.
        uint32_t alphaCmp = call.otherModeL & 0x03;
        if (cycleType == 2) {
            // COPY mode: unconditional alpha test per N64 hardware
            features |= Shader::SF_ALPHA_TEST;
        } else if ((alphaCmp & 1) && cycleType != 3) {
            features |= Shader::SF_ALPHA_TEST;
        }

        // Fog (from geometry mode)
        // Disabled on Intel Iris Xe: SF_FOG combined with SF_DUAL_SOURCE_BLEND +
        // SF_COVERAGE_ENABLE + SF_2CYCLE produces a shader whose GPU ISA compilation
        // exceeds 2 s on Intel (non-preemptable), triggering Windows TDR.
        // Without fog, the shader maps to a known-safe variant (ISA < 2 s).
        if ((call.geometryMode & 0x00010000) && !m_renderer->GetCapabilities().isIntelGPU)
            features |= Shader::SF_FOG;

        // Texture filter (otherModeH bits 12-13)
        //   0 = G_TF_POINT   � nearest-neighbor (no flag)
        //   2 = G_TF_BILERP  � N64 3-sample interpolation
        //   3 = G_TF_AVERAGE � true bilinear (for LOD blending)
        // COPY mode always uses point sampling on real N64 hardware.
        uint32_t texFilter = (cycleType == 2) ? 0 : ((call.otherModeH >> 12) & 0x03);
        // SF_TEXTURE_3POINT disabled: the N64 3-point GLSL function causes >1s glslang
        // compilation and likely >2s GPU ISA compile on Intel Iris Xe → Windows TDR.
        // Use SF_TEXTURE_BILINEAR (VK LINEAR sampler) as a quality-equivalent substitute.
        if (texFilter == 2) features |= Shader::SF_TEXTURE_BILINEAR;
        else if (texFilter == 3) features |= Shader::SF_TEXTURE_BILINEAR;

        // Monochrome post-process (game-specific hack — e.g. Zelda OoT cutscenes).
        // Only activate when the tile is CI8 over what was originally an RGBA16
        // framebuffer — that is the specific N64 trick Zelda uses for grayscale
        // flashbacks.  Applying this unconditionally would desaturate everything.
        if (m_hacks.zeldaMonochrome &&
            call.tileDesc0.format == 2 /* G_IM_FMT_CI */ &&
            call.tileDesc0.size   == 1 /* G_IM_SIZ_8b */ &&
            g_fbManager.FindByAddress(call.textureImageAddr) != nullptr)
            features |= Shader::SF_MONOCHROME;

        // Texture LOD (G_TL_LOD � mipmap fraction via LOD_FRACTION combiner input)
        if (call.otherModeH & 0x00010000) features |= Shader::SF_TEXTURE_LOD;

        // Texture detail/sharpen (otherModeH bits 17-18: 0=clamp, 1=sharpen, 2=detail)
        {
            uint32_t tdMode = (call.otherModeH >> 17) & 0x03;
            if (tdMode == 1) features |= Shader::SF_TEXTURE_SHARPEN;
            if (tdMode == 2) features |= Shader::SF_TEXTURE_DETAIL;
        }

        // YUV texture format (tile format == 1) � enable shader YUV�RGB conversion
        if (call.tileDesc0.format == 1) features |= Shader::SF_TEXTURE_YUV;

        // Chroma key enable (otherModeH bit 8)
        if (call.otherModeH & 0x00000100) features |= Shader::SF_CHROMA_KEY;

        // Alpha noise/dither (otherModeL bits 0-1: 1=threshold, 3=dither � note: value 2 unused)
        if ((call.otherModeL & 0x03) == 3) {
            features |= Shader::SF_NOISE_ENABLE;
            features |= Shader::SF_ALPHA_NOISE_DITHER;
        }

        // Also enable noise when the combiner mux references NOISE
        // (G_CCMUX_NOISE = 7 in the RGB SubA/A position, 4-bit field).
        {
            uint32_t muxHi = static_cast<uint32_t>(call.combineMode >> 32);
            int saRGB0 = (muxHi >> 20) & 0xF;  // cycle 0 SubA RGB
            int saRGB1 = (muxHi >>  5) & 0xF;  // cycle 1 SubA RGB
            if (saRGB0 == 7 || saRGB1 == 7)
                features |= Shader::SF_NOISE_ENABLE;
        }

        // RGB dither (otherModeH bits 6-7: 0=magic square, 1=bayer, 2=noise, 3=none)
        // Disabled on Intel Iris Xe: the dither code (4×4 matrix lookup + RGB
        // perturbation) adds significant shader complexity that causes >2 s GPU ISA
        // compilation and Windows TDR.  Visual impact is negligible on 24-bit displays.
        {
            uint32_t rgbDither = (call.otherModeH >> 6) & 0x03;
            if (rgbDither != 3 && !m_renderer->GetCapabilities().isIntelGPU)
                features |= Shader::SF_DITHER_ENABLE;
        }

        // Alpha dither (otherModeH bits 4-5: 0=pattern, 1=~pattern, 2=noise, 3=none)
        // Disabled on Intel Iris Xe for the same reason as SF_DITHER_ENABLE above.
        {
            uint32_t alphaDither = (call.otherModeH >> 4) & 0x03;
            if (alphaDither != 3 && !m_renderer->GetCapabilities().isIntelGPU)
                features |= Shader::SF_ALPHA_DITHER;
        }

        // Framebuffer read & blender memColor strategy.
        //
        // MLE BLENDER: When the blender references memColor or needs FB
        // coverage, we use the full shader-based read-modify-write path
        // (imageLoad/imageStore) with fragment shader interlock.  This
        // replaces the old SF_FRAMEBUFFER_READ and SF_DUAL_SOURCE_BLEND
        // approximations and gives pixel-perfect N64 blender behavior.
        {
            uint32_t blenderBits = call.otherModeL & 0xFFFF0000;

            // IM_RD (otherModeL bit 6): when clear, RDP doesn't read FB.
            bool imageRead = (call.otherModeL & 0x40) != 0;

            if (imageRead && blenderBits != 0) {
                // Performance gate: only enable the expensive MLE (interlock)
                // path when the blender actually needs to READ framebuffer.
                // Skip MLE when no blender input references memColor/memCvg
                // AND coverage destination is FULL (always writes 7).
                int p1 = (blenderBits >> 30) & 0x3;
                int m1 = (blenderBits >> 22) & 0x3;
                int b1 = (blenderBits >> 18) & 0x3;
                bool needsMem = (p1 == 1 || m1 == 1 || b1 == 1);
                if (cycleType == 1) { // 2-cycle
                    int p2 = (blenderBits >> 28) & 0x3;
                    int m2 = (blenderBits >> 20) & 0x3;
                    int b2 = (blenderBits >> 16) & 0x3;
                    needsMem = needsMem || (p2 == 1 || m2 == 1 || b2 == 1);
                }
                int cvgDst = (call.otherModeL >> 8) & 0x03;
                if (cvgDst != 2) needsMem = true; // CLAMP/WRAP/SAVE read mem_cvg

                // INTEL WORKAROUND: Intel Iris Xe fragment shader interlock
                // with imageLoad/imageStore silently fails for certain MLE
                // blender configurations (e.g. OoT title screen logo draws
                // with 2-cycle fog blender).  Fall back to dual-source blend
                // on Intel GPUs for reliable blender emulation.
                if ((needsMem || batchNeedsMleBlender) && !DISABLE_MLE_BLENDER &&
                    m_renderer->GetCapabilities().supportsFragmentShaderInterlock &&
                    !m_renderer->GetCapabilities().isIntelGPU)
                    features |= Shader::SF_MLE_BLENDER;
                else if (needsMem)
                    features |= Shader::SF_DUAL_SOURCE_BLEND;
            }
        }

        // FORCE_BL (otherModeL bit 14) � when set, the blender always applies
        // the full (P*A+M*B)/(A+B) formula.  When clear, fully-covered pixels
        // bypass the blend and output P directly.
        if (call.otherModeL & 0x4000) features |= Shader::SF_FORCE_BLEND;

        // Coverage / AA flags (otherModeL)
        // SF_COVERAGE_ENABLE (AA_EN) disabled on Intel Iris Xe: the 8-sample barycentric
        // coverage loop (dFdx/dFdy × 8 iterations) adds enough fragment shader complexity
        // that GPU ISA compilation can exceed 2 s on Intel (non-preemptable), triggering TDR.
        // When disabled, cov defaults to 1.0 (full coverage) — correct for non-edge geometry.
        if ((call.otherModeL & 0x0008) && !m_renderer->GetCapabilities().isIntelGPU)
            features |= Shader::SF_COVERAGE_ENABLE;  // AA_EN bit 3
        if (call.otherModeL & 0x0080) features |= Shader::SF_COLOR_ON_COVERAGE; // CLR_ON_CVG bit 7
        if (call.otherModeL & 0x1000) features |= Shader::SF_ALPHA_CVG;         // CVG_X_ALPHA bit 12
        if (call.otherModeL & 0x2000) features |= Shader::SF_ALPHA_CVG_SEL;     // ALPHA_CVG_SEL bit 13

        // COPY/FILL modes: clear coverage-alpha flags (hardware ignores them)
        if (cycleType >= 2) {
            features &= ~(uint64_t)(Shader::SF_ALPHA_CVG_SEL);
        }

        shaderKey.features = features;

        // �� Shadow map override ����������������������������������������
        // When this draw call is a shadow map rect, replace the entire
        // combiner/blender pipeline with a specialised shader that reads
        // the depth buffer and converts it via a palette (TLUT) into a
        // fog-coloured shadow overlay.
        if (call.isShadowMap) {
            shaderKey.features = Shader::SF_SHADOW_MAP;
            shaderKey.mux = 0;
            shaderKey.blenderBits = 0;
            shaderKey.otherModeH = 0;
            shaderKey.otherModeL = 0;
        }

        // --- Get/compile shader ---
        const Shader::CompiledShader* shader = nullptr;
        if (m_shaderManager) {
            shader = m_shaderManager->GetShader(shaderKey);
        }


        if (!shader) {
            static int nullShaderWarnCount = 0;
            if (nullShaderWarnCount < 10) {
                NOVA_LOG_WARN("FlushBatch: null shader for draw#{} mux=0x{:016X} features=0x{:012X} cycleType={} drawType={}",
                              nullShaderWarnCount, shaderKey.mux, (unsigned long long)shaderKey.features,
                              (shaderKey.features & 0xF), static_cast<int>(call.type));
                ++nullShaderWarnCount;
            }
        }

        if (shader) {
            m_shaderManager->BindShader(shader);

            // Set MVP matrix: ortho for both 2D and 3D.
            // 3D vertices are mapped to N64 screen space coordinates by applying
            // N64 viewport scaling in CPU before being sent here.
            m_shaderManager->SetMatrixUniforms(ortho, nullptr);

            // Set color uniforms from draw call snapshot (not live state)
            float primColor[4] = {
                call.primColor.r / 255.0f,
                call.primColor.g / 255.0f,
                call.primColor.b / 255.0f,
                call.primColor.a / 255.0f
            };
            float envColor[4] = {
                call.envColor.r / 255.0f,
                call.envColor.g / 255.0f,
                call.envColor.b / 255.0f,
                call.envColor.a / 255.0f
            };
            float fogColor[4] = {
                call.fogColor.r / 255.0f,
                call.fogColor.g / 255.0f,
                call.fogColor.b / 255.0f,
                call.fogColor.a / 255.0f
            };
            float blendColor[4] = {
                call.blendColor.r / 255.0f,
                call.blendColor.g / 255.0f,
                call.blendColor.b / 255.0f,
                call.blendColor.a / 255.0f
            };
            m_shaderManager->SetColorUniforms(
                primColor, envColor, fogColor, blendColor,
                call.primLODFrac, call.primLODMin,
                m_state.rdp.chromaKey.centerR / 255.0f,
                m_state.rdp.chromaKey.centerG / 255.0f,
                m_state.rdp.chromaKey.centerB / 255.0f,
                m_state.rdp.chromaKey.scaleR / 255.0f,
                m_state.rdp.chromaKey.scaleG / 255.0f,
                m_state.rdp.chromaKey.scaleB / 255.0f);

            // Upload chroma key width
            m_shaderManager->SetKeyWidth(
                m_state.rdp.chromaKey.widthR / 255.0f,
                m_state.rdp.chromaKey.widthG / 255.0f,
                m_state.rdp.chromaKey.widthB / 255.0f);

            // Set fill color for fill rect draws (shader reads uFillColor)
            if (call.type == DrawCall::Type::FillRect) {
                const auto& v0 = m_batch.vertices[call.vertexStart];
                m_shaderManager->SetFillColor(v0.r, v0.g, v0.b, v0.a);
            }

            // Set screen size uniforms (uScreenSize, uInvScreenSize) � use FBO dims
            m_shaderManager->SetScreenSize(fboW, fboH);

            // Set frame counter for noise/dither animation
            m_shaderManager->SetFrameCount(m_state.frameCount);

            // Set texture sampler uniforms (uTexture0 -> unit 0, uTexture1 -> unit 1)
            // and texture scale/size for the N64 3-point filter
            {
                auto getShiftScale = [](uint32_t shift) -> float {
                    if (shift == 0) return 1.0f;
                    if (shift <= 10) return 1.0f / (1 << shift);
                    return static_cast<float>(1 << (16 - shift));
                };

                // N64 hardware: the tile's shiftS/shiftT only applies to triangle
                // texture coordinates (from the RSP vertex pipeline).  TextureRect
                // S/T values go directly to the texture lookup � no tile shift.
                bool isRect = (call.type == DrawCall::Type::TextureRect ||
                               call.type == DrawCall::Type::FillRect);
                float scale0[2], scale1[2];
                if (isRect) {
                    scale0[0] = scale0[1] = 1.0f;
                    scale1[0] = scale1[1] = 1.0f;
                } else {
                    scale0[0] = getShiftScale(call.tileDesc0.shiftS);
                    scale0[1] = getShiftScale(call.tileDesc0.shiftT);
                    scale1[0] = getShiftScale(call.tileDesc1.shiftS);
                    scale1[1] = getShiftScale(call.tileDesc1.shiftT);
                }
                float size0[2]  = { 1.0f, 1.0f };
                float size1[2]  = { 1.0f, 1.0f };

                // Use mask/clamp-aware tile sizes (must match BuildTextureKey)
                uint8_t tlut = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
                int w0, h0;
                CalcTileSize(call.tileDesc0, tlut, w0, h0);
                if (w0 > 0 && h0 > 0) {
                    size0[0] = static_cast<float>(w0);
                    size0[1] = static_cast<float>(h0);
                }
                int w1, h1;
                CalcTileSize(call.tileDesc1, tlut, w1, h1);
                if (w1 > 0 && h1 > 0) {
                    size1[0] = static_cast<float>(w1);
                    size1[1] = static_cast<float>(h1);
                }

                m_shaderManager->SetTextureUniforms(0, 1, scale0, scale1, size0, size1, nullptr, nullptr);

                // Tile diagnostic: log first 15 triangle draw calls with texture,
                // and also log any draw call with vertices in the copyright region (x=[265,286], y=[42,62])
                {
                    static uint32_t s_tileLog = 0;
                    bool inCopyrightRegion = false;
                    if (!isRect && call.vertexCount > 0) {
                        const auto& v0 = m_batch.vertices[call.vertexStart];
                        inCopyrightRegion = (v0.x >= 100.f && v0.x <= 320.f && v0.y >= 40.f && v0.y <= 240.f);
                    }
                    if (!isRect && (s_tileLog < 15 || inCopyrightRegion)) {
                        if (s_tileLog < 15) ++s_tileLog;
                        float sl0 = call.tileDesc0.GetSL();
                        float tl0 = call.tileDesc0.GetTL();
                        bool tp = (call.otherModeH & OTHERMODE_TEXTPERSP) != 0;
                        Log::infof("[TILE#%u] hash=0x%08X shiftS=%u shiftT=%u scale=%.2f,%.2f size=%.0f,%.0f sl=%.2f tl=%.2f mask=%u,%u tp=%d%s",
                                   s_tileLog, (unsigned)call.textureHash0,
                                   (unsigned)call.tileDesc0.shiftS, (unsigned)call.tileDesc0.shiftT,
                                   scale0[0], scale0[1], size0[0], size0[1],
                                   sl0, tl0,
                                   (unsigned)call.tileDesc0.maskS, (unsigned)call.tileDesc0.maskT,
                                   (int)tp,
                                   inCopyrightRegion ? " [COPY]" : "");
                        if (inCopyrightRegion) {
                            const auto& v0c = m_batch.vertices[call.vertexStart];
                            Log::infof("[COPY-MODE] x=%.1f y=%.1f hash=0x%08X combineMode=0x%016llX oMH=0x%08X oML=0x%08X fmt=%u siz=%u envA=%.4f",
                                       v0c.x, v0c.y,
                                       (unsigned)call.textureHash0,
                                       (unsigned long long)call.combineMode,
                                       call.otherModeH, call.otherModeL,
                                       (unsigned)call.tileDesc0.format,
                                       (unsigned)call.tileDesc0.size,
                                       call.envColor.a / 255.0f);
                        }
                    }
                }
            }

            // Set tile offset, clamp, mirror, mask uniforms
            {
                float offset0[2] = { call.tileDesc0.GetSL() + call.scrollS0, call.tileDesc0.GetTL() + call.scrollT0 };
                float offset1[2] = { call.tileDesc1.GetSL() + call.scrollS1, call.tileDesc1.GetTL() + call.scrollT1 };
                // clamp vec4 = (clampS, clampT, mirrorS, mirrorT)
                float clamp0[4] = {
                    call.tileDesc0.IsClamped_S() ? 1.0f : 0.0f,
                    call.tileDesc0.IsClamped_T() ? 1.0f : 0.0f,
                    call.tileDesc0.IsMirrored_S() ? 1.0f : 0.0f,
                    call.tileDesc0.IsMirrored_T() ? 1.0f : 0.0f
                };
                float clamp1[4] = {
                    call.tileDesc1.IsClamped_S() ? 1.0f : 0.0f,
                    call.tileDesc1.IsClamped_T() ? 1.0f : 0.0f,
                    call.tileDesc1.IsMirrored_S() ? 1.0f : 0.0f,
                    call.tileDesc1.IsMirrored_T() ? 1.0f : 0.0f
                };
                float mask0[2] = { static_cast<float>(call.tileDesc0.maskS), static_cast<float>(call.tileDesc0.maskT) };
                float mask1[2] = { static_cast<float>(call.tileDesc1.maskS), static_cast<float>(call.tileDesc1.maskT) };
                m_shaderManager->SetTextureTileParams(offset0, offset1, clamp0, clamp1, mask0, mask1);
            }

            // Set texture coordinate bounds for TextureRect bilinear bleed prevention
            m_shaderManager->SetTexCoordBounds(call.useTexCoordBounds,
                                                call.texCoordBounds0, call.texCoordBounds1);

            // Texture perspective is now handled in vertex setup (DrawTriangle sets
            // perspW = invW when TEXTPERSP is on, perspW = 1.0 when off), so
            // the shader always divides by vClipW and we no longer need the
            // uTexturePersp uniform toggle here.

            // Screen scale for resolution-independent LOD computation
            // Screen scale = scaledSize / nativeSize; at native res this is (1,1).
            // Shaders divide gl_FragCoord by this to get native coords, and multiply
            // LOD derivatives by this to compensate for higher pixel density.
            {
                float scaleX = (curFBO ? static_cast<float>(curFBO->scaledWidth)  / std::max(1.0f, static_cast<float>(curFBO->width))  : 1.0f);
                float scaleY = (curFBO ? static_cast<float>(curFBO->scaledHeight) / std::max(1.0f, static_cast<float>(curFBO->height)) : 1.0f);
                m_shaderManager->SetScreenScale(scaleX, scaleY);
            }

            // LOD mipmap parameters
            {
                int tdMode = (call.otherModeH >> 17) & 0x3; // textureDetail: 0=clamp, 1=sharpen, 2=detail
                int maxTile = static_cast<int>(m_state.rdp.geometry.texture.level);
                m_shaderManager->SetLODParams(call.primLODMin, maxTile, tdMode);
            }

            // F3DAM fog usage mode
            m_shaderManager->SetFogUsage(call.fogUsage);

            // Screen-space triangle (noperspective shade) for rects
            m_shaderManager->SetScreenSpaceTriangle(
                (call.type == DrawCall::Type::TextureRect || call.type == DrawCall::Type::FillRect) ? 1 : 0);

            // Combiner debug output (0 = normal rendering)
            {
                int debugMode = m_debugOutputMode;
                if (m_debugMagenta)     debugMode = 9;  // override: magenta highlight
                else if (m_debugForceOpaque) debugMode = 8;  // override: force alpha=1
                m_shaderManager->SetDebugOutput(debugMode);
            }

            // Texture-alpha coverage: for RGBA16 sprites drawn with the copyright/
            // trademark combine mode (0x00FFFFFFFFFCFA7D), the texture A bit is the
            // only per-pixel coverage mask.  Discard A=0 pixels so background texels
            // don't overdraw the framebuffer.
            //
            // Using the EXACT combine mode avoids the "dA==5" over-match: many other
            // RGBA16 textures (Mario face, trees, sky) also use ENV_ALPHA as dA but
            // leave the A bit = 0 everywhere — discarding those would erase them.
            {
                static constexpr uint64_t kCvgAlphaMux = 0x00FFFFFFFFFCFA7DULL;
                bool needsCvgAlpha = (call.tileDesc0.format == 0
                                   && call.tileDesc0.size >= 2
                                   && call.combineMode == kCvgAlphaMux);
                m_shaderManager->SetTexCvgAlpha(needsCvgAlpha ? 1 : 0);
            }

            // Set alpha reference for alpha compare
            {
                uint32_t alphaCompMode = call.otherModeL & 0x03;
                if (cycleType == 2) {
                    // COPY mode: N64 hardware tests texel alpha > blendColor.alpha.
                    // Add half-LSB to convert strict ">" to shader's "<" test.
                    m_shaderManager->SetAlphaRef(call.blendColor.a / 255.0f + 0.5f / 255.0f);
                } else if (alphaCompMode & 1) {
                    // Threshold (1) or dither (3) � set blendColor.a as reference
                    m_shaderManager->SetAlphaRef(call.blendColor.a / 255.0f);
                }
            }

            // Set fog params from geometry state
            {
                float fogMul = static_cast<float>(m_state.rdp.geometry.fogMultiplier) / 256.0f;
                float fogOff = static_cast<float>(m_state.rdp.geometry.fogOffset) / 256.0f;
                m_shaderManager->SetFogParams(fogMul, fogOff);
            }

            // Set depth params (15-bit Z: mask off MSB, divide by 32768)
            m_shaderManager->SetDepthParams(1.0f, 0.0f,
                static_cast<float>(call.primDepth & 0x7FFF) / 32768.0f);

            // Upload YUV conversion coefficients (K0-K5)
            m_shaderManager->SetConvertK(m_state.rdp.convertK);
        }

        // --- Set blend state from N64 blender mux ---
        BlendState blend;
        {
            // N64 blender formula: (P * A + M * B) / (A + B)
            // otherModeL layout (bits 16-31):
            //   bits 30-31: P0  cycle 0   (0=CC,1=Mem,2=Blend,3=Fog)
            //   bits 28-29: P1  cycle 1
            //   bits 26-27: A0  cycle 0   (0=CCAlpha,1=FogAlpha,2=ShadeAlpha,3=Zero)
            //   bits 24-25: A1  cycle 1
            //   bits 22-23: M0  cycle 0   (0=CC,1=Mem,2=Blend,3=Fog)
            //   bits 20-21: M1  cycle 1
            //   bits 18-19: B0  cycle 0   (0=1-A,1=MemAlpha,2=One,3=Zero)
            //   bits 16-17: B1  cycle 1
            uint32_t p0  = (call.otherModeL >> 30) & 3;
            uint32_t a0  = (call.otherModeL >> 26) & 3;
            uint32_t m0  = (call.otherModeL >> 22) & 3;
            uint32_t b0  = (call.otherModeL >> 18) & 3;
            uint32_t p1  = (call.otherModeL >> 28) & 3;
            uint32_t a1  = (call.otherModeL >> 24) & 3;
            uint32_t m1  = (call.otherModeL >> 20) & 3;
            uint32_t b1  = (call.otherModeL >> 16) & 3;
            bool forceBlend = ((call.otherModeL >> 14) & 1) != 0;

            // In 2-cycle mode, the final pixel color comes from cycle 1.
            // In 1-cycle (or copy/fill), use cycle 0.
            uint32_t cycleType = (call.otherModeH >> 20) & 3;
            uint32_t pF = (cycleType == 1) ? p1 : p0;
            uint32_t aF = (cycleType == 1) ? a1 : a0;
            uint32_t mF = (cycleType == 1) ? m1 : m0;
            uint32_t bF = (cycleType == 1) ? b1 : b0;

            bool hasDualSource = (features & Shader::SF_DUAL_SOURCE_BLEND) != 0;
            bool hasMLE = (features & Shader::SF_MLE_BLENDER) != 0;

            // COPY and FILL modes bypass the blender entirely on real N64 hardware.
            // Disable GL blending unconditionally for these cycle types, matching
            // GLideN64's guard: cycleType < G_CYC_COPY.
            if (cycleType >= 2) {
                blend.enabled = false;
            } else if (hasMLE) {
                // �� MLE blender: shader does full read-modify-write �������
                // No GL blend needed; imageStore handles everything.
                blend.enabled = false;
            } else if (hasDualSource) {
                // �� Dual-source blending path �����������������������������
                // The shader outputs:
                //   fragColor  = source contribution (non-memColor terms)
                //   fragColor1 = destination factor  (memColor weight)
                // Hardware blend: fragColor * ONE + dst * SRC1_COLOR
                blend.enabled = true;
                blend.srcColor = BlendFactor::One;
                blend.dstColor = BlendFactor::Src1Color;
                blend.srcAlpha = BlendFactor::One;
                blend.dstAlpha = BlendFactor::Src1Alpha;

                // Special case from GLideN64: when B (not M) references
                // memColor.a, the shader approximates it as 1.0 but the
                // real factor should multiply by dst alpha.  Adjust the
                // GL blend accordingly.
                if (forceBlend) {
                    if (mF != 1 && bF == 1) {
                        // M is NOT memColor, B IS memColor.a � src needs DST_ALPHA
                        blend.srcColor = BlendFactor::DstAlpha;
                        blend.srcAlpha = BlendFactor::DstAlpha;
                    }
                    if (mF == 1 && bF == 1) {
                        // Both M=memColor and B=memColor.a � dst also gets DST_ALPHA
                        blend.dstColor = BlendFactor::DstAlpha;
                        blend.dstAlpha = BlendFactor::DstAlpha;
                    }
                }

                // P==M � result is always P regardless of blend factors
                if (pF == mF) {
                    blend.enabled = false;
                }
            } else {
                // �� Legacy GL blend approximation �������������������������
                // Match the shader-side skip: only when P or M is memColor.
                bool needsMemColor = (pF == 1) || (mF == 1);
                bool hasFBRead = (features & Shader::SF_FRAMEBUFFER_READ) != 0;
                bool shaderBlenderSkipped = needsMemColor && !hasFBRead;
                // On real N64, ALPHA_CVG_SEL without CVG_X_ALPHA means the
                // blender alpha comes from coverage.  For fully-covered
                // interior pixels coverage is 1.0, so the blend formula
                // degenerates to P (source only) � the surface is opaque.
                // Enabling GL blend for these modes produces additive
                // artifacts on overlapping geometry.
                bool alphaCvgSel = (features & Shader::SF_ALPHA_CVG_SEL) != 0;
                bool cvgXAlpha   = (features & Shader::SF_ALPHA_CVG) != 0;
                bool isOpaqueMode = alphaCvgSel && !cvgXAlpha;
                bool isTranslucent = shaderBlenderSkipped && forceBlend && !isOpaqueMode;

                if (!forceBlend && pF == 1 && cycleType < 2) {
                    // P = memColor without FORCE_BL: fully-covered pixels output P directly.
                    // Since P = framebuffer, this is a no-op draw. Use blend (ZERO, ONE)
                    // to preserve the existing framebuffer contents.
                    blend.enabled = true;
                    blend.srcColor = BlendFactor::Zero;
                    blend.dstColor = BlendFactor::One;
                    blend.srcAlpha = BlendFactor::Zero;
                    blend.dstAlpha = BlendFactor::One;
                } else {
                    // �� Blend enable & factor ������������������������������
                    // N64 formula: out = (P*A + M*B) / (A+B)
                    // Since the shader blender is skipped, fragColor = combined
                    // (raw combiner output, NOT pre-multiplied by A).
                    // Map A � srcFactor (applied to P=combined by GL)
                    //     B � dstFactor (applied to M=memColor=dst by GL)

                    // Helper: map blender A selector to GL blend factor
                    auto mapAToSrc = [&]() -> BlendFactor {
                        switch (aF) {
                        case 0: return BlendFactor::SrcAlpha;       // combined.a
                        case 1:                                     // fogColor.a
                            blend.blendColor[0] = call.fogColor.r / 255.0f;
                            blend.blendColor[1] = call.fogColor.g / 255.0f;
                            blend.blendColor[2] = call.fogColor.b / 255.0f;
                            blend.blendColor[3] = call.fogColor.a / 255.0f;
                            return BlendFactor::ConstantAlpha;
                        case 2: return BlendFactor::SrcAlpha;       // shadeAlpha ? srcAlpha
                        default: return BlendFactor::Zero;          // 0.0
                        }
                    };
                    auto mapOneMinusA = [&]() -> BlendFactor {
                        switch (aF) {
                        case 0: return BlendFactor::OneMinusSrcAlpha;
                        case 1:
                            blend.blendColor[0] = call.fogColor.r / 255.0f;
                            blend.blendColor[1] = call.fogColor.g / 255.0f;
                            blend.blendColor[2] = call.fogColor.b / 255.0f;
                            blend.blendColor[3] = call.fogColor.a / 255.0f;
                            return BlendFactor::OneMinusConstantAlpha;
                        case 2: return BlendFactor::OneMinusSrcAlpha;
                        default: return BlendFactor::One;           // 1 - 0
                        }
                    };
                    auto mapBToDst = [&]() -> BlendFactor {
                        switch (bF) {
                        case 0: return mapOneMinusA();   // 1 - A
                        case 1: return BlendFactor::DstAlpha;  // memColor.a
                        case 2: return BlendFactor::One;       // 1.0
                        default: return BlendFactor::Zero;     // 0.0
                        }
                    };

                    if (pF == 1 && mF == 1) {
                        // Both P and M are memColor � preserve framebuffer
                        blend.enabled = true;
                        blend.srcColor = BlendFactor::Zero;
                        blend.dstColor = BlendFactor::One;
                        blend.srcAlpha = BlendFactor::Zero;
                        blend.dstAlpha = BlendFactor::One;
                    } else if (pF == mF) {
                        // P == M � result is always P regardless of factors
                        blend.enabled = false;
                    } else {
                        blend.enabled = isTranslucent;

                        if (pF == 1) {
                            // P=memColor(dst), M=non-mem(src=combined)
                            // N64: out = mem*A + combined*B
                            // GL:  out = combined*srcFactor + dst*dstFactor
                            blend.srcColor = mapBToDst(); // combined gets B factor
                            blend.dstColor = mapAToSrc(); // dst(mem) gets A factor
                        } else if (mF == 1) {
                            // P=non-mem(src=combined), M=memColor(dst)
                            // N64: out = combined*A + mem*B
                            // GL:  out = combined*srcFactor + dst*dstFactor
                            blend.srcColor = mapAToSrc(); // combined gets A factor
                            blend.dstColor = mapBToDst(); // dst(mem) gets B factor
                        } else {
                            // Neither is memColor � shouldn't reach here
                            blend.srcColor = BlendFactor::One;
                            blend.dstColor = BlendFactor::Zero;
                        }

                        blend.srcAlpha = blend.srcColor;
                        blend.dstAlpha = blend.dstColor;
                    }
                } // end else (!pF==1 bypass)
            }

            // NOTE: The old "Texture-off sanity check" (blastCorpsBlendFix) was
            // removed.  It tested bit 0x40000 of geometryMode, which is
            // G_TEXTURE_GEN in F3DEX2 � NOT G_TEXTURE_ENABLE.  In F3DEX2,
            // texture enable is controlled by gSPTexture, not geometryMode.
            // The check incorrectly made all textured triangles without
            // environment mapping invisible (blend src=0, dst=1).
        }
        m_renderer->SetBlendState(blend);

        // --- Set depth state ---
        DepthStencilState depth;
        depth.depthTestEnabled  = (features & Shader::SF_DEPTH_TEST) != 0;
        depth.depthWriteEnabled = (features & Shader::SF_DEPTH_WRITE) != 0;

        // When Z_CMP is off but depth is still enabled (via G_ZBUFFER or prim Z),
        // set depthFunc = ALWAYS so all pixels pass while depth writes still work.
        // This matches GLideN64's behavior for write-only depth initialization.
        bool zCmpOn = (call.otherModeL & 0x0010) != 0;

        // Derive depth func from N64 zMode (otherModeL bits 10-11)
        uint32_t zMode = (call.otherModeL >> 10) & 0x03;
        if (features & Shader::SF_N64_DEPTH_COMPARE) {
            // When the N64 Z-image compare is active, the fragment shader
            // handles ALL depth testing via imageLoad/imageStore on the
            // Z-image.  Disable GL depth test and GL depth writes entirely
            // to prevent the GL depth buffer from being polluted with
            // rasterizer-interpolated linear Z values that would corrupt
            // subsequent non-N64-depth draws.  Matches GLideN64.
            depth.depthTestEnabled  = false;
            depth.depthWriteEnabled = false;
        } else if (!zCmpOn) {
            depth.depthFunc = CompareFunc::Always;
        } else {
            // Without N64 Z-image compare, GL depth func is our only
            // depth test.  Match GLideN64: OPA/XLU use LESS unless
            // depth source is PRIM with z=1.0 (Max mode → LEQUAL),
            // INTER/DEC always use LEQUAL.
            bool depthSourcePrim = (call.otherModeL & 0x0004) != 0;
            bool primDepthMax = depthSourcePrim && (call.primDepth >= 0.999f);
            switch (zMode) {
                case 0: // OPAQUE
                case 2: // TRANSPARENT
                    depth.depthFunc = primDepthMax ? CompareFunc::LessEqual : CompareFunc::Less;
                    break;
                case 1: // INTERPENETRATING
                    depth.depthFunc = CompareFunc::LessEqual;
                    break;
                case 3: // DECAL
                default:
                    depth.depthFunc = CompareFunc::LessEqual;
                    break;
            }
        }
        // Coverage destination bits (otherModeL [9:8]) refine depth write behaviour:
        //   CVG_DST_SAVE (3): preserve existing depth regardless of Z_UPD flag.
        //   ZMODE_TRANSPARENT (2) without FORCE_BL: game controls draw order via sorting;
        //   depth buffer writes would produce incorrect overdraw for alpha surfaces.
        {
            uint32_t cvgDst = (call.otherModeL >> 8) & 0x03;
            if (cvgDst == 3) {
                depth.depthWriteEnabled = false;
            }
            bool forceBlend = ((call.otherModeL >> 14) & 1) != 0;
            if (zMode == 2 && !forceBlend) {
                depth.depthWriteEnabled = false;
            }
        }

        // --- 1.2 Override Depth for Rectangles ---
        // N64 rectangles have no vertex Z; they only participate in depth
        // when Z_SOURCE_SEL=PRIM (otherModeL bit 2) explicitly routes
        // primDepth to the shader's gl_FragDepth.  Without that, rects
        // at z=0 interact incorrectly with the depth buffer.
        // COPY/FILL modes never do depth (already handled by feature flags
        // via cycType <= 1 check), but guard here too for safety.
        if (call.type == DrawCall::Type::TextureRect || call.type == DrawCall::Type::FillRect) {
            bool zPrim = (call.otherModeL & 0x0004) != 0;
            uint32_t cycType = (call.otherModeH >> 20) & 0x03;
            if (!zPrim || cycType >= 2) {
                depth.depthTestEnabled = false;
                depth.depthWriteEnabled = false;
            } else {
                // zPrim is set and cycle type is 1- or 2-cycle:
                // enable depth so the rect participates in the depth buffer.
                depth.depthTestEnabled = true;
                depth.depthWriteEnabled = (call.otherModeL & 0x0020) != 0; // Z_UPD
            }
        }

        // --- 1.3 Depth render override ---
        // When the shader outputs color.r as depth, enable depth test (ALWAYS)
        // and depth write so gl_FragDepth actually hits the GL depth buffer.
        if (features & Shader::SF_DEPTH_RENDER) {
            depth.depthTestEnabled  = true;
            depth.depthWriteEnabled = true;
            depth.depthFunc = CompareFunc::Always;
            // If the game also has Z_CMP set (mode 2), the shader handles the
            // compare via Z-image imageLoad; GL depth func stays ALWAYS.
        }

        // Polygon offset: match GLideN64 — only DECAL mode uses polygon
        // offset to lift decals above the base surface.  OPA/INTER/XLU
        // should NOT use polygon offset (it causes z-fighting and ground
        // plane instability).
        {
            RasterizerState rast{};
            rast.cullMode = CullMode::None;
            if (zMode == 3) {  // DECAL
                rast.depthBias = -3.0f;
                rast.depthBiasSlope = -3.0f;
            }
            m_renderer->SetRasterizerState(rast);
        }
        // Debug: force-disable depth test/write
        if (m_debugDisableDepth) {
            depth.depthTestEnabled  = false;
            depth.depthWriteEnabled = false;
        }
        m_renderer->SetDepthStencilState(depth);

        // ��� N64 Z-image depth compare: bind Z-image textures + set mode ����
        if (features & Shader::SF_N64_DEPTH_COMPARE) {
            auto* depthBuf = g_depthManager.GetCurrent();
            if (depthBuf) {
                g_depthManager.InitDepthImageTextures(*depthBuf, g_fbManager.GetResolutionScale());
                g_depthManager.BindDepthImageTextures(*depthBuf, 8, 9);
                // Normalized approach: primDeltaZ converted to [0,1] range
                m_shaderManager->SetN64DepthMode(
                    static_cast<int>(zMode),
                    static_cast<float>(call.primDeltaZ & 0x7FFF) / 32768.0f
                );
            }
        }

        // ��� MLE blender: bind color/coverage images and disable color writes ����
        if (features & Shader::SF_MLE_BLENDER) {
            // Ensure prior FBO writes (e.g. fill rects) are visible to imageLoad.
            m_renderer->ImageMemoryBarrier();
            GPUFramebuffer* fb = g_fbManager.GetCurrentColor();
            if (fb && fb->coverageTex) {
                m_renderer->BindImageForRW(2, fb->colorImageHandle, ImageBindFormat::RGBA8);
                m_renderer->BindImageForRW(3, fb->coverageImageHandle, ImageBindFormat::R8UI);
                // Disable fragment output writes � only imageStore updates
                // the color buffer.  This avoids undefined aliasing between
                // the FBO attachment and the image binding.
                m_renderer->SetColorWriteMask(false, false, false, false);
            }
        }

        // ��� CVG_DST coverage accumulator �����������������������
        // The N64 uses the 3 coverage bits per pixel for anti-aliasing blending.
        // We now accurately compute CVG_DEST logic (WRAP, CLAMP, SAVE, ZAP) in the
        // pixel shader (GenerateBlenderCode) to perfectly emulate N64 coverage wrap,
        // rather than relying on flawed OpenGL fixed-function stencil operations.
        // For WRAP, it uses SF_FRAMEBUFFER_READ to precisely evaluate (cvg+memCvg)&7.

        // --- Set scissor (scale N64 coords to GL window coords, flip Y) ---
        {
            int n64sx = static_cast<int>(m_state.rdp.scissor.xh >> 2);
            int n64sy = static_cast<int>(m_state.rdp.scissor.yh >> 2);
            int n64sw = static_cast<int>((m_state.rdp.scissor.xl >> 2) - n64sx);
            int n64sh = static_cast<int>((m_state.rdp.scissor.yl >> 2) - n64sy);

            // Widescreen: expand scissor Y to full height for all draws.
            // This lets both 3D content and fullscreen effects (fades,
            // transitions) fill the expanded viewport area. Letterbox bar
            // skipping and inward scaling are handled in Handle*Rect().
            float wsScale = m_state.rdp.geometry.widescreenScale;
            if (wsScale != 1.0f) {
                n64sy = 0;
                n64sh = static_cast<int>(n64H);
            }

            // Scissor interlace mode: mode 2 = even lines only, mode 3 = odd lines only.
            // On real N64, this restricts rendering to alternating scanlines for
            // interlaced displays.  We halve the scissor height and shift Y by field.
            uint8_t scissorMode = m_state.rdp.scissor.mode;
            if (scissorMode >= 2) {
                // Even (mode 2) or odd (mode 3) field
                int field = (scissorMode == 3) ? 1 : 0;
                n64sy = n64sy / 2 + field;
                n64sh = (n64sh + 1) / 2;
            }

            float scaleX = fboW / n64W;
            float scaleY = fboH / n64H;

            RenderScissorRect scissor;
            scissor.x      = static_cast<int32_t>(n64sx * scaleX);
            scissor.y      = static_cast<int32_t>(fboH - (n64sy + n64sh) * scaleY); // GL Y=0 at bottom
            scissor.width  = static_cast<int32_t>(n64sw * scaleX);
            scissor.height = static_cast<int32_t>(n64sh * scaleY);

            // Clamp scissor to FBO bounds to prevent writes past the
            // framebuffer attachment edge (avoids garbage strip artifacts).
            int32_t fboWi = static_cast<int32_t>(fboW);
            int32_t fboHi = static_cast<int32_t>(fboH);
            if (scissor.x < 0) { scissor.width += scissor.x; scissor.x = 0; }
            if (scissor.y < 0) { scissor.height += scissor.y; scissor.y = 0; }
            if (scissor.x + scissor.width  > fboWi) scissor.width  = fboWi - scissor.x;
            if (scissor.y + scissor.height > fboHi) scissor.height = fboHi - scissor.y;

            if (scissor.width > 0 && scissor.height > 0) {
                if (g_fbManager.IsGL()) glEnable(GL_SCISSOR_TEST);
                m_renderer->SetScissor(scissor);
            } else {
                if (g_fbManager.IsGL()) glDisable(GL_SCISSOR_TEST);
            }
        }

        // --- Shadow map: bind depth texture & TLUT, override blend ----
        if (call.isShadowMap) {
            if (g_fbManager.IsGL()) {
            // Bind the current FBO's depth texture on unit 4 (uDepthTexture)
            GPUFramebuffer* curFB_shadow = g_fbManager.GetCurrentColor();
            if (curFB_shadow && curFB_shadow->depthTex) {
                glActiveTexture(GL_TEXTURE4);
                glBindTexture(GL_TEXTURE_2D, curFB_shadow->depthTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            }
            // Create or update the 256x1 R8 shadow TLUT texture on unit 2 (uTLUT0)
            if (!m_shadowTLUTTexture) {
                glCreateTextures(GL_TEXTURE_2D, 1, &m_shadowTLUTTexture);
                glTextureStorage2D(m_shadowTLUTTexture, 1, GL_R8, 256, 1);
                glTextureParameteri(m_shadowTLUTTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(m_shadowTLUTTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTextureParameteri(m_shadowTLUTTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            }
            glTextureSubImage2D(m_shadowTLUTTexture, 0, 0, 0, 256, 1,
                                GL_RED, GL_UNSIGNED_BYTE, call.shadowTLUT);
            glBindTextureUnit(2, m_shadowTLUTTexture);
            } // end if (g_fbManager.IsGL()) shadow map textures

            // Override blend for shadow: SRC_ALPHA / ONE_MINUS_SRC_ALPHA
            blend.enabled  = true;
            blend.srcColor = BlendFactor::SrcAlpha;
            blend.dstColor = BlendFactor::OneMinusSrcAlpha;
            blend.srcAlpha = BlendFactor::SrcAlpha;
            blend.dstAlpha = BlendFactor::OneMinusSrcAlpha;
            m_renderer->SetBlendState(blend);

            // Disable depth write/test for shadow overlay
            {
                DepthStencilState shadowDepth;
                shadowDepth.depthTestEnabled  = false;
                shadowDepth.depthWriteEnabled = false;
                m_renderer->SetDepthStencilState(shadowDepth);
            }

            // Draw the shadow rect
            m_renderer->DrawArrays(PrimitiveType::Triangles,
                                   static_cast<uint32_t>(batchBaseVertex + call.vertexStart),
                                   static_cast<uint32_t>(call.vertexCount));
            continue; // Skip normal texture/draw path
        }

        // --- Bind textures if available and set wrap modes ---
        float gpuScale0[2] = {1.0f, 1.0f};
        float gpuScale1[2] = {1.0f, 1.0f};
        int fbMono0 = 0, fbMono1 = 0, fbAlpha0 = 0, fbAlpha1 = 0;

        auto getGLWrapMode = [](const TileDescriptor& td, bool isS) -> GLenum {
            bool clamp  = isS ? td.IsClamped_S()  : td.IsClamped_T();
            bool mirror = isS ? td.IsMirrored_S() : td.IsMirrored_T();
            uint8_t mask = isS ? td.maskS : td.maskT;
            // When mask bits > 0 the shader handles all wrap/mirror/clamp
            // in texel space.  Use CLAMP_TO_EDGE in GL to avoid the
            // sampler re-applying mirror/repeat on the already-wrapped UVs.
            if (mask > 0) return GL_CLAMP_TO_EDGE;
            if (clamp)  return GL_CLAMP_TO_EDGE;
            if (mirror) return GL_MIRRORED_REPEAT;
            return GL_REPEAT;
        };

        if (m_textureCache && call.textureHash0 != 0) {
            uint8_t tlutType0 = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
            Tex::TextureKey key0 = BuildTextureKey(call.tileDesc0, call.textureImageAddr, tlutType0,
                                                    call.mipLevelCount > 0 ? call.mipTiles : nullptr,
                                                    call.mipLevelCount,
                                                    call.snapshotCrc0);
            if (key0.width > 0 && key0.height > 0) {
                GPUFramebuffer* rawRTFB0 = g_fbManager.FindByAddress(key0.address);
                bool aliasRTFB0 = CanAliasFramebuffer(rawRTFB0, key0.address, IsRGBAFramebufferFormat(key0));
                GPUFramebuffer* rtFB0 = aliasRTFB0 ? rawRTFB0 : nullptr;

                MM_RDP_TRACE("BindTex0 drawType={} addr=0x{:06X} fmt={} size={} w={} h={} line={} pal={} tlut={} tmem={} crc=0x{:016X} fbFound={} fbAlias={} fbAddr=0x{:06X} color=0x{:06X} z=0x{:06X}",
                             static_cast<int>(call.type),
                             key0.address & 0x7FFFFF,
                             static_cast<int>(key0.format),
                             static_cast<int>(key0.size),
                             key0.width,
                             key0.height,
                             key0.line,
                             key0.palette,
                             key0.tlutType,
                             key0.tmemOffset,
                             key0.crc,
                             rawRTFB0 != nullptr,
                             aliasRTFB0,
                             rawRTFB0 ? rawRTFB0->desc.address : 0u,
                             m_state.rdp.colorImage.address & 0x7FFFFF,
                             m_state.rdp.zbufferImage.address & 0x7FFFFF);

                // ── CfB copy-back detection ──────────────────────────────
                // Some N64 games (OoT, MM) copy an AA-processed framebuffer
                // (CfB) back to the display via RGBA32 TextureRect strips.
                // The CfB was never used as an RDP color image, so no GPU
                // framebuffer exists for it.  The TMEM data is really
                // RGBA5551 pixels loaded as RGBA32 → decode produces garbage.
                //
                // Only skip when the texture address is within the RDRAM
                // range of a known framebuffer (CfB is typically allocated
                // adjacent to the main framebuffers).  Textures from other
                // RDRAM regions (e.g. ROM-loaded artwork) are legitimate.
                //
                // IMPORTANT: Only skip TextureRect draws.  Triangle draws
                // with RGBA32 textures (e.g. Zelda logo shield/sword) are
                // legitimate game geometry and must NOT be skipped.
                if (!rtFB0 &&
                    call.type == DrawCall::Type::TextureRect &&
                    key0.format == Tex::N64TexFormat::RGBA &&
                    key0.size == Tex::N64TexSize::Bits32)
                {
                    uint32_t texAddr = key0.address & 0x7FFFFF;
                    bool nearFB = false;
                    for (uint32_t fi = 0; fi < g_fbManager.GetFBCount(); ++fi) {
                        auto* fb = g_fbManager.GetFB(fi);
                        if (!fb || fb->desc.type != FBType::Color) continue;
                        uint32_t fbStart = fb->desc.address;
                        uint32_t fbSize  = fb->desc.ByteSize();
                        // CfB is typically within 2x framebuffer size of a
                        // known FB address (one extra buffer worth of room).
                        if (texAddr >= fbStart && texAddr < fbStart + fbSize * 3) {
                            nearFB = true;
                            break;
                        }
                    }
                    if (nearFB) {
                        MM_RDP_TRACE("SkipTex0CfBCopy addr=0x{:06X} drawType={} fmt={} size={}",
                                     key0.address & 0x7FFFFF,
                                     static_cast<int>(call.type),
                                     static_cast<int>(key0.format),
                                     static_cast<int>(key0.size));
                        continue;
                    }
                }

                uint32_t fbTex0 = rtFB0 ? g_fbManager.GetSampleableTexture(
                    rtFB0, rtFB0 == g_fbManager.GetCurrentColor()) : 0;

                // Vulkan: GetSampleableTexture returns 0 (no per-N64-FB GL texture).
                // If we have a swapchain snapshot (taken when SetColorImage switched away
                // from the source FB), bind it here.
                bool useVKSnapshot0 = false;
                ITexture* vkSnapshotTex0 = nullptr;
                if (fbTex0 == 0 && !g_fbManager.IsGL()) {
                    // Ping-pong: read whichever slot was most recently written.
                    const uint32_t snapIdx = m_vkFBSnapshotIdx;
                    ITexture* snapTex = m_vkFBSnapshots[snapIdx].get();
                    const uint32_t snapAddr = m_vkFBSnapshotAddrs[snapIdx];
                    if (snapTex) {
                        // Compare against the resolved FB's BASE address, not key0.address.
                        // ExecuteLoadTile overrides tmemSourceAddress with an offset
                        // (base + tl*stride + uls*bpp/2) so range-based FindByAddress works,
                        // but that makes key0.address != snapAddr for any LoadTile
                        // with tl>0 (the typical OoT/MM pause-menu strip pattern).  rtFB0 was
                        // produced by FindByAddress(key0.address) and so its desc.address is
                        // the correct base.
                        if (rtFB0 && snapAddr == (rtFB0->desc.address & 0x7FFFFF)) {
                            // Standard path: RGBA FB whose range contains key0.address.
                            fbTex0 = 1;
                            useVKSnapshot0 = true;
                            vkSnapshotTex0 = snapTex;
                        } else if (!rtFB0 && rawRTFB0 &&
                                   snapAddr == (rawRTFB0->desc.address & 0x7FFFFF)) {
                            // Extended path: non-RGBA tile (e.g. IA8) loaded from within a GPU
                            // FB range.  CanAliasFramebuffer returned false so rtFB0==null, but
                            // rawRTFB0 still identifies which FB the data came from.
                            fbTex0 = 1;
                            useVKSnapshot0 = true;
                            vkSnapshotTex0 = snapTex;
                            rtFB0 = rawRTFB0;  // needed so gpuScale0 uses FB dimensions below
                        }
                    }
                }

                if (fbTex0 != 0) {
                    if (g_fbManager.IsGL()) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, fbTex0);
                    } else if (useVKSnapshot0 && vkSnapshotTex0) {
                        m_renderer->BindTexture(0, vkSnapshotTex0);
                    }
                    // Set UV scale based on FB dimensions so texcoords map correctly
                    gpuScale0[0] = 1.0f / static_cast<float>(rtFB0->desc.width);
                    gpuScale0[1] = 1.0f / static_cast<float>(rtFB0->desc.height);
                    // FB-as-texture monochrome detection (matches GLideN64 UFrameBufferInfo):
                    //  size==1 (8-bit): mono mode 1 (red channel), fixedAlpha if IM_RD off
                    //  tile IA16: mono mode 2 (weighted luminance)
                    if (rtFB0->desc.size == 1) {
                        fbMono0 = 1;
                        if ((call.otherModeL & 0x40) == 0) fbAlpha0 = 1; // IM_RD off
                    } else if (call.tileDesc0.size == 2 && call.tileDesc0.format == 3) {
                        fbMono0 = 2; // IA16 luminance
                    }
                    MM_RDP_TRACE("UseFBTex0 addr=0x{:06X} fbTex={} fbAddr=0x{:06X} fbSize={}x{} gpuScale={:.4f},{:.4f} mono={} alpha={}",
                                 key0.address & 0x7FFFFF,
                                 fbTex0,
                                 rtFB0->desc.address,
                                 rtFB0->desc.width,
                                 rtFB0->desc.height,
                                 gpuScale0[0],
                                 gpuScale0[1],
                                 fbMono0,
                                 fbAlpha0);
                } else {
                    // Build mip tile info for tile-chain mipmapping
                    Tex::TextureCache::MipTileInfo mipInfo[7];
                    int mipCount = 0;
                    if (call.mipLevelCount > 0) {
                        mipCount = call.mipLevelCount;
                        for (int mi = 0; mi < mipCount; mi++) {
                            const auto& mt = call.mipTiles[mi];
                            mipInfo[mi].tmemAddr = mt.tmemAddr;
                            mipInfo[mi].line     = mt.line;
                            mipInfo[mi].width    = static_cast<uint16_t>(std::max(1, mt.GetWidth()));
                            mipInfo[mi].height   = static_cast<uint16_t>(std::max(1, mt.GetHeight()));
                            mipInfo[mi].format   = mt.format;
                            mipInfo[mi].size     = mt.size;
                            mipInfo[mi].palette  = mt.palette;
                        }
                    }
                    Tex::CachedTexture* cached0 = (mipCount > 0)
                        ? m_textureCache->GetTexture(key0, m_rdram, m_state.rdp.tmem.data, mipInfo, mipCount)
                        : m_textureCache->GetTexture(key0, m_rdram, m_state.rdp.tmem.data);
                    if (cached0 && cached0->gpuTexture) {
                        m_renderer->BindTexture(0, cached0->gpuTexture.get());
                        if (g_fbManager.IsGL()) {
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getGLWrapMode(call.tileDesc0, true));
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getGLWrapMode(call.tileDesc0, false));
                            // Force correct per-draw filter mode from otherModeH
                            // texFilter 0/1=point, 2=3-point (shader samples texels manually), 3=bilinear
                            GLenum glFilter = (texFilter == 3) ? GL_LINEAR : GL_NEAREST;
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter);
                        }
                        gpuScale0[0] = cached0->scaleS;
                        gpuScale0[1] = cached0->scaleT;
                        MM_RDP_TRACE("UseCachedTex0 addr=0x{:06X} scale={:.4f},{:.4f} mipCount={} texFilter={}",
                                     key0.address & 0x7FFFFF,
                                     gpuScale0[0],
                                     gpuScale0[1],
                                     mipCount,
                                     texFilter);
                    } else if (m_debugWhiteTexture) {
                        // Fallback: bind 1×1 white texture so the draw isn't invisible
                        m_renderer->BindTexture(0, m_debugWhiteTexture.get());
                        gpuScale0[0] = 1.0f;
                        gpuScale0[1] = 1.0f;
                        MM_RDP_TRACE("FallbackWhiteTex0 addr=0x{:06X} fmt={} size={} w={} h={} crc=0x{:016X}",
                                     key0.address & 0x7FFFFF,
                                     static_cast<int>(key0.format),
                                     static_cast<int>(key0.size),
                                     key0.width,
                                     key0.height,
                                     key0.crc);
                    }

                }
            }
        }
        if (m_textureCache && call.textureHash1 != 0) {
            uint8_t tlutType1 = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
            Tex::TextureKey key1 = BuildTextureKey(call.tileDesc1, call.textureImageAddr1, tlutType1,
                                                    nullptr, 0, call.snapshotCrc1);
            if (key1.width > 0 && key1.height > 0) {
                GPUFramebuffer* rawRTFB1 = g_fbManager.FindByAddress(key1.address);
                bool aliasRTFB1 = CanAliasFramebuffer(rawRTFB1, key1.address, IsRGBAFramebufferFormat(key1));
                GPUFramebuffer* rtFB1 = aliasRTFB1 ? rawRTFB1 : nullptr;
                MM_RDP_TRACE("BindTex1 drawType={} addr=0x{:06X} fmt={} size={} w={} h={} pal={} tlut={} tmem={} crc=0x{:016X} fbFound={} fbAlias={} fbAddr=0x{:06X} color=0x{:06X} z=0x{:06X}",
                             static_cast<int>(call.type),
                             key1.address & 0x7FFFFF,
                             static_cast<int>(key1.format),
                             static_cast<int>(key1.size),
                             key1.width,
                             key1.height,
                             key1.palette,
                             key1.tlutType,
                             key1.tmemOffset,
                             key1.crc,
                             rawRTFB1 != nullptr,
                             aliasRTFB1,
                             rawRTFB1 ? rawRTFB1->desc.address : 0u,
                             m_state.rdp.colorImage.address & 0x7FFFFF,
                             m_state.rdp.zbufferImage.address & 0x7FFFFF);
                uint32_t fbTex1 = rtFB1 ? g_fbManager.GetSampleableTexture(
                    rtFB1, rtFB1 == g_fbManager.GetCurrentColor()) : 0;
                if (fbTex1 != 0) {
                    if (g_fbManager.IsGL()) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, fbTex1);
                    }
                    // Set UV scale based on FB dimensions so texcoords map correctly
                    gpuScale1[0] = 1.0f / static_cast<float>(rtFB1->desc.width);
                    gpuScale1[1] = 1.0f / static_cast<float>(rtFB1->desc.height);
                    if (rtFB1->desc.size == 1) {
                        fbMono1 = 1;
                        if ((call.otherModeL & 0x40) == 0) fbAlpha1 = 1;
                    } else if (call.tileDesc1.size == 2 && call.tileDesc1.format == 3) {
                        fbMono1 = 2;
                    }
                    MM_RDP_TRACE("UseFBTex1 addr=0x{:06X} fbTex={} fbAddr=0x{:06X} fbSize={}x{} gpuScale={:.4f},{:.4f} mono={} alpha={}",
                                 key1.address & 0x7FFFFF,
                                 fbTex1,
                                 rtFB1->desc.address,
                                 rtFB1->desc.width,
                                 rtFB1->desc.height,
                                 gpuScale1[0],
                                 gpuScale1[1],
                                 fbMono1,
                                 fbAlpha1);
                } else {
                    Tex::CachedTexture* cached1 = m_textureCache->GetTexture(
                        key1, m_rdram, m_state.rdp.tmem.data);
                    if (cached1 && cached1->gpuTexture) {
                        m_renderer->BindTexture(1, cached1->gpuTexture.get());
                        if (g_fbManager.IsGL()) {
                            // Texture is already bound on unit 1 by BindTexture above
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getGLWrapMode(call.tileDesc1, true));
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getGLWrapMode(call.tileDesc1, false));
                            GLenum glFilter1 = (texFilter == 3) ? GL_LINEAR : GL_NEAREST;
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter1);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter1);
                        }
                        gpuScale1[0] = cached1->scaleS;
                        gpuScale1[1] = cached1->scaleT;
                        MM_RDP_TRACE("UseCachedTex1 addr=0x{:06X} scale={:.4f},{:.4f} texFilter={}",
                                     key1.address & 0x7FFFFF,
                                     gpuScale1[0],
                                     gpuScale1[1],
                                     texFilter);
                    } else if (m_debugWhiteTexture) {
                        // Fallback: bind 1×1 white texture for stale texture unit
                        m_renderer->BindTexture(1, m_debugWhiteTexture.get());
                        gpuScale1[0] = 1.0f;
                        gpuScale1[1] = 1.0f;
                        MM_RDP_TRACE("FallbackWhiteTex1 addr=0x{:06X} fmt={} size={} w={} h={} crc=0x{:016X}",
                                     key1.address & 0x7FFFFF,
                                     static_cast<int>(key1.format),
                                     static_cast<int>(key1.size),
                                     key1.width,
                                     key1.height,
                                     key1.crc);
                    }
                }
            }
        }
        m_shaderManager->SetGPUScale(gpuScale0, gpuScale1);
        m_shaderManager->SetFBTextureParams(fbMono0, fbMono1, fbAlpha0, fbAlpha1);

        // --- Bind noise (unit 5) and dither (unit 6) textures ---
        if (m_noiseTexture) {
            m_renderer->BindTexture(5, m_noiseTexture.get());
        }
        {
            // Select dither texture based on N64 rgb_dither_sel (otherModeH bits 6-7)
            // 0 = Magic Square (default), 1 = Bayer, 2 = noise (uses noise tex), 3 = none
            uint32_t rgbDitherMode = (call.otherModeH >> 6) & 0x03;
            ITexture* ditherTex = (rgbDitherMode == 1) ? m_ditherBayerTexture.get() : m_ditherTexture.get();
            if (rgbDitherMode == 2) ditherTex = m_noiseTexture.get(); // noise dither reuses noise
            if (ditherTex) {
                m_renderer->BindTexture(6, ditherTex);
            }
        }

        // --- Framebuffer read: snapshot current FB and bind to unit 7 ---
        if (features & Shader::SF_FRAMEBUFFER_READ) {
            GPUFramebuffer* fbForRead = g_fbManager.GetCurrentColor();
            if (fbForRead) {
                uint32_t fbTex = g_fbManager.GetSampleableTexture(fbForRead, true);
                if (fbTex != 0 && g_fbManager.IsGL()) {
                    glActiveTexture(GL_TEXTURE7);
                    glBindTexture(GL_TEXTURE_2D, fbTex);
                }
            }
            // When dual-source is NOT active, the shader blender computes
            // the full blend formula (including memColor from FB texture),
            // so disable GL hardware blend to avoid double-blending.
            // When dual-source IS active, we need GL blend for compositing.
            if (!(features & Shader::SF_DUAL_SOURCE_BLEND)) {
                blend.enabled = false;
                m_renderer->SetBlendState(blend);
            }
        }

        // --- Trivial rejection bypass for testing ---
        if (false && call.type == DrawCall::Type::Triangle && call.vertexCount >= 3) {
            bool allOutsideSamePlane = false;
            
            // Check if all 3 vertices share a common "outside" clip flag
            if (call.vertexCount == 3) {
                const auto& v0 = m_batch.vertices[call.vertexStart + 0];
                const auto& v1 = m_batch.vertices[call.vertexStart + 1];
                const auto& v2 = m_batch.vertices[call.vertexStart + 2];
                
                // If all 3 verts are outside the same plane, reject the triangle
                // (Their common AND flags indicate shared outside status)
                uint32_t commonClipFlags = v0.clipFlags & v1.clipFlags & v2.clipFlags;
                allOutsideSamePlane = (commonClipFlags != 0);
            }
            
            if (allOutsideSamePlane) {
                // Skip this triangle entirely � all vertices are culled
                m_stats.trianglesDrawn++;  // Count as drawn (for statistics)
                continue;  // Move to next draw call
            }
        }

        // --- Draw ---
        PrimitiveType prim = PrimitiveType::Triangles;
        if (call.type == DrawCall::Type::TextureRect ||
            call.type == DrawCall::Type::FillRect) {
            prim = PrimitiveType::Triangles; // Rects decomposed into 2 triangles
        }

        // === DEBUG: Skip depth-tested triangles (model draws) =============
        if constexpr (DEBUG_SKIP_DEPTH_TRIS) {
            if (call.type == DrawCall::Type::Triangle && (features & Shader::SF_DEPTH_TEST)) {
                continue;
            }
        }


        m_renderer->DrawArrays(prim,
                               static_cast<uint32_t>(batchBaseVertex + call.vertexStart),
                               static_cast<uint32_t>(call.vertexCount));

        // Restore color writes after MLE draw (imageStore already updated
        // the color buffer; the mask was set to avoid aliasing UB).
        if (features & Shader::SF_MLE_BLENDER) {
            m_renderer->SetColorWriteMask(true, true, true, true);
            m_renderer->ImageMemoryBarrier();
        } else if (features & Shader::SF_N64_DEPTH_COMPARE) {
            // Track that this batch wrote to the Z-image; the barrier is
            // deferred to the end of the batch so that overlapping terrain
            // draws within the same batch all see the pre-batch Z-image
            // state (typically the cleared value).
            batchUsedN64Depth = true;
        }

    }

    // Flush any pending Z-image writes so subsequent batches (e.g. sprites)
    // can see the terrain depth via imageLoad.
    if (batchUsedN64Depth) {
        m_renderer->ImageMemoryBarrier();
    }

    m_stats.batchesFlushed++;
    m_stats.frameDrawCalls += static_cast<uint32_t>(m_batch.drawCalls.size());

    // Snapshot the FBO for presentation whenever scene content was rendered.
    // This must happen here (not in HandleSyncFull/HandleSetColorImage) because
    // other commands like SetScissor can trigger FlushBatch before those handlers
    // run, leaving them with an empty batch and no snapshot.
    {
        bool hasSceneDraws = false;
        for (const auto& c : m_batch.drawCalls) {
            if (c.type == DrawCall::Type::Triangle || c.type == DrawCall::Type::TextureRect) {
                hasSceneDraws = true;
                break;
            }
            // Include non-FILL-mode FillRects (e.g., blended fade overlays drawn in
            // 1-cycle or 2-cycle mode).  Pure FILL mode (cycleType=3) rects are
            // simple screen clears and don't need a snapshot.
            if (c.type == DrawCall::Type::FillRect) {
                uint32_t cycType = (c.otherModeH >> 20) & 0x03;
                if (cycType != 3) {
                    hasSceneDraws = true;
                    break;
                }
            }
        }
        if (hasSceneDraws)
            g_fbManager.SnapshotForPresentation();
    }

    // ── LLE diagnostic: dump per-flush draw breakdown once after ~200 flushes ──
    {
        static uint32_t s_flushNum = 0;
        s_flushNum++;
        if (s_flushNum == 200 && !s_dsDumped) {
            s_dsDumped = true;
            Log::infof("[DRAW-STATS] flush#%u tris=%u fills=%u uniqueCombos=%zu scissorXl=%u Yl=%u colorAddr=0x%06X",
                       s_flushNum, s_ds.triCount, s_ds.fillCount,
                       s_ds.comboCount.size(),
                       s_ds.lastScissorXl >> 2, s_ds.lastScissorYl >> 2,
                       s_ds.lastColorAddr & 0x7FFFFF);
            // Sort & dump top combos
            std::vector<std::pair<uint64_t, uint32_t>> top(s_ds.comboCount.begin(), s_ds.comboCount.end());
            std::sort(top.begin(), top.end(),
                      [](const auto& a, const auto& b){ return a.second > b.second; });
            for (size_t i = 0; i < top.size() && i < 12; ++i) {
                Log::infof("[DRAW-STATS]   combo[%zu] key=0x%016llX count=%u",
                           i, (unsigned long long)top[i].first, top[i].second);
            }
        }
    }

    m_batch.Reset();
}

bool RDPProcessor::NeedsBatchFlush(const DrawCall& newCall) const {
    if (m_batch.drawCalls.empty()) return false;
    
    const auto& lastCall = m_batch.drawCalls.back();
    
    // Different render mode requires flush
    if (lastCall.combineMode != newCall.combineMode) return true;
    if (lastCall.otherModeH != newCall.otherModeH) return true;
    if (lastCall.otherModeL != newCall.otherModeL) return true;
    if (lastCall.type != newCall.type) return true;
    
    return false;
}

void RDPProcessor::OnFrameEnd() {
    FlushBatch();

    m_state.frameCount++;
    m_stats.ResetFrame();

    // Re-randomize noise texture each frame (N64 generates new random per pixel per frame)
    if (m_noiseTexture) {
        constexpr int NOISE_SIZE = 64;
        std::vector<uint8_t> noiseData(NOISE_SIZE * NOISE_SIZE);
        // Use frame count as seed for distinct per-frame noise
        std::mt19937 rng(static_cast<uint32_t>(m_state.frameCount) * 2654435761u);
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& p : noiseData) p = static_cast<uint8_t>(dist(rng));
        m_noiseTexture->UploadSubRegion(0, 0, NOISE_SIZE, NOISE_SIZE,
                                         noiseData.data(), noiseData.size(), 0);
    }

    // Advance depth buffer manager frame counter / eviction
    g_depthManager.OnFrameEnd();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Frame Buffer Access
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::GetFrameBufferInfo(FrameBufferInfo* info, int maxBuffers) const {
    if (maxBuffers < 1 || !info) return;
    
    info[0].address = m_state.rdp.colorImage.address;
    info[0].width   = m_state.rdp.colorImage.width;
    info[0].height  = static_cast<uint32_t>(m_state.rdp.scissor.yl >> 2);
    info[0].size    = static_cast<uint32_t>(m_state.rdp.colorImage.size);
}

bool RDPProcessor::ReadFrameBuffer(uint32_t address, uint32_t size, void* data) {
    if (!m_rdram || !data) return false;
    if (address + size > m_rdramSize) return false;
    
    std::memcpy(data, m_rdram + address, size);
    return true;
}

bool RDPProcessor::WriteFrameBuffer(uint32_t address, uint32_t size, const void* data) {
    if (!m_rdram || !data) return false;
    if (address + size > m_rdramSize) return false;
    
    std::memcpy(m_rdram + address, data, size);
    return true;
}

} // namespace Nova64::Video