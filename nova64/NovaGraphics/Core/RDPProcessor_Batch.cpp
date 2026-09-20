#include "RDPProcessor_Private.hpp"
#include <cstdlib>
#include <cstdio>
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
    if (auto* curFB = g_fbManager.GetCurrentColor()) {
        // Which buffer is receiving draws, and how many? gpuModified==0 on the
        // prerender buffer proved no batch ever flushed while it was current.
        {
            static const bool s_dbg = std::getenv("NOVA64_FBDUMP") != nullptr;
            const uint32_t a = curFB->desc.address & 0x7FFFFF;
            if (s_dbg && a >= 0x380000 && a <= 0x3B0000) {
                std::fprintf(stderr, "[FLUSHTO] addr=0x%06X drawCalls=%zu\n",
                             a, m_batch.drawCalls.size());
            }
        }
        if (!curFB->gpuModified)
            g_fbManager.RecordRdramFingerprint(curFB, m_rdram, m_rdramSize);
        curFB->gpuModified = true;
        curFB->needsInventorySnapshot = true;
        curFB->needsRdramResync = true;
        curFB->everGpuRendered = true;   // sticky; see the field's comment
        // DIAGNOSTIC (NOVA64_SNAPSHOT_TRACE=1): tally draw activity per FB so
        // the pause-menu snapshot capture points can log how much was
        // actually rendered before the snapshot fired -- distinguishes "FB
        // was fully rendered but the snapshot mechanism is broken" from "FB
        // was snapshotted before the world/background geometry was drawn".
        curFB->drawCallsSinceSwitch += static_cast<uint32_t>(m_batch.drawCalls.size());
        for (const DrawCall& dc : m_batch.drawCalls) {
            if (dc.type == DrawCall::Type::Triangle)
                curFB->trianglesSinceSwitch += static_cast<uint32_t>(dc.vertexCount / 3);
        }
    }

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

        // Draw-call screen-bbox dump (NOVA64_DRAWBOX=viMin,viMax): correlate a
        // visible artifact's pixel position with the exact draw call + state
        // that painted it. One line per DrawCall at flush time.
        {
            static const long boxMin = []() -> long {
                const char* v = std::getenv("NOVA64_DRAWBOX");
                return v ? std::strtol(v, nullptr, 10) : -1L;
            }();
            static const long boxMax = []() -> long {
                const char* v = std::getenv("NOVA64_DRAWBOX");
                if (!v) return -1L;
                const char* comma = std::strchr(v, ',');
                return comma ? std::strtol(comma + 1, nullptr, 10) : -1L;
            }();
            const long presentNow = g_presentIndex.load(std::memory_order_relaxed);
            if (boxMin >= 0 && presentNow >= boxMin && presentNow <= boxMax &&
                call.vertexCount > 0 &&
                static_cast<size_t>(call.vertexStart + call.vertexCount) <= m_batch.vertices.size()) {
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (int vi = 0; vi < call.vertexCount; ++vi) {
                    const auto& v = m_batch.vertices[call.vertexStart + vi];
                    minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
                    minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
                }
                const auto& v0 = m_batch.vertices[call.vertexStart];
                const auto& vA = m_batch.vertices[call.vertexStart + std::min(1, call.vertexCount - 1)];
                const auto& vB = m_batch.vertices[call.vertexStart + std::min(2, call.vertexCount - 1)];
                auto* curFBDbg = g_fbManager.GetCurrentColor();
                std::fprintf(stderr,
                    "[DRAWBOX] vi=%ld type=%d nVtx=%d bbox=(%.0f,%.0f)-(%.0f,%.0f) cmb=0x%llX omL=0x%X omH=0x%X "
                    "hash0=0x%llX addr0=0x%06X crc0=0x%016llX prim=(%u,%u,%u,%u) env=(%u,%u,%u,%u) "
                    "v0col=(%.2f,%.2f,%.2f,%.2f) tile0fmt=%u tile0sz=%u tile0wh=(%d,%d) "
                    "colorImg=0x%06X zImg=0x%06X curFB=0x%06X "
                    "sltl=(%.2f,%.2f) cm=(%u,%u) mask=(%u,%u) shift=(%u,%u) scroll=(%.2f,%.2f) "
                    "st0=(%.3f,%.3f,w%.4f) st1=(%.3f,%.3f,w%.4f) st2=(%.3f,%.3f,w%.4f) "
                    "p0=(%.1f,%.1f) p1=(%.1f,%.1f) p2=(%.1f,%.1f)\n",
                    presentNow, static_cast<int>(call.type), call.vertexCount,
                    minX, minY, maxX, maxY,
                    static_cast<unsigned long long>(call.combineMode),
                    call.otherModeL, call.otherModeH,
                    static_cast<unsigned long long>(call.textureHash0),
                    call.textureImageAddr & 0x7FFFFF,
                    static_cast<unsigned long long>(call.snapshotCrc0),
                    call.primColor.r, call.primColor.g, call.primColor.b, call.primColor.a,
                    call.envColor.r, call.envColor.g, call.envColor.b, call.envColor.a,
                    v0.r, v0.g, v0.b, v0.a,
                    static_cast<unsigned>(call.tileDesc0.format), static_cast<unsigned>(call.tileDesc0.size),
                    call.tileDesc0.GetWidth(), call.tileDesc0.GetHeight(),
                    m_state.rdp.colorImage.address & 0x7FFFFF,
                    m_state.rdp.zbufferImage.address & 0x7FFFFF,
                    curFBDbg ? (curFBDbg->desc.address & 0x7FFFFF) : 0xFFFFFFu,
                    call.tileDesc0.GetSL(), call.tileDesc0.GetTL(),
                    static_cast<unsigned>(call.tileDesc0.clampS), static_cast<unsigned>(call.tileDesc0.clampT),
                    static_cast<unsigned>(call.tileDesc0.maskS), static_cast<unsigned>(call.tileDesc0.maskT),
                    static_cast<unsigned>(call.tileDesc0.shiftS), static_cast<unsigned>(call.tileDesc0.shiftT),
                    call.scrollS0, call.scrollT0,
                    v0.s, v0.t, v0.perspW,
                    vA.s, vA.t, vA.perspW,
                    vB.s, vB.t, vB.perspW,
                    v0.x, v0.y, vA.x, vA.y, vB.x, vB.y);
            }
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

        // Pre-compute whether the MLE blender will be selected for this draw
        // (mirrors the "Framebuffer read & blender memColor strategy" block
        // further down) so the N64 Z-image depth-compare gate immediately
        // below can correctly avoid combining the two on Intel hardware.
        // This must run first: SF_MLE_BLENDER isn't set into `features`
        // until that later block runs, so checking `features &
        // SF_MLE_BLENDER` at this point was always false -- silently
        // turning the intended Intel-only workaround into unscoped dead
        // code that never actually fired on ANY platform (including
        // Intel, where the real driver bug it describes still lives).
        bool willUseMleBlender = false;
        {
            uint32_t preBlenderBits = call.otherModeL & 0xFFFF0000;
            bool preImageRead = (call.otherModeL & 0x40) != 0;
            if (preImageRead && preBlenderBits != 0) {
                int p1 = (preBlenderBits >> 30) & 0x3;
                int m1 = (preBlenderBits >> 22) & 0x3;
                int b1 = (preBlenderBits >> 18) & 0x3;
                bool preNeedsMem = (p1 == 1 || m1 == 1 || b1 == 1);
                if (cycleType == 1) {
                    int p2 = (preBlenderBits >> 28) & 0x3;
                    int m2 = (preBlenderBits >> 20) & 0x3;
                    int b2 = (preBlenderBits >> 16) & 0x3;
                    preNeedsMem = preNeedsMem || (p2 == 1 || m2 == 1 || b2 == 1);
                }
                int preCvgDst = (call.otherModeL >> 8) & 0x03;
                if (preCvgDst != 2) preNeedsMem = true;
                willUseMleBlender = (preNeedsMem || batchNeedsMleBlender) && !DISABLE_MLE_BLENDER &&
                    m_renderer->GetCapabilities().supportsFragmentShaderInterlock &&
                    !m_renderer->GetCapabilities().isIntelGPU;
            }
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
            !(m_renderer->GetCapabilities().isIntelGPU && willUseMleBlender) &&
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

                // Real RDP hardware runs texture rectangles through the same
                // per-pixel span/texture pipeline as triangles (TEXRECT is just a
                // synthesized rectangular span, not a distinct pixel path) -- the
                // tile's shiftS/shiftT applies uniformly to any texture lookup
                // through that tile, not just RSP-vertex-driven triangle coords.
                // Games rely on this: a small HUD icon is commonly drawn by
                // TEXRECT-sampling a larger shared texture through a tile whose
                // SETTILE set a nonzero shift, letting the RDP pre-compress the
                // coordinate instead of shipping a separate small texture. Treating
                // rects as shift=0 always (the previous behavior) left that
                // compression un-applied, so the rect sampled a texel range far
                // wider than intended -- aliasing into sparse noise-like dots for
                // any icon actually relying on tile shift to minify.
                bool isRect = (call.type == DrawCall::Type::TextureRect ||
                               call.type == DrawCall::Type::FillRect);
                float scale0[2], scale1[2];
                scale0[0] = getShiftScale(call.tileDesc0.shiftS);
                scale0[1] = getShiftScale(call.tileDesc0.shiftT);
                scale1[0] = getShiftScale(call.tileDesc1.shiftS);
                scale1[1] = getShiftScale(call.tileDesc1.shiftT);
                float size0[2]  = { 1.0f, 1.0f };
                float size1[2]  = { 1.0f, 1.0f };

                // Use mask/clamp-aware tile sizes, LoadTile-load-aware just
                // like BuildTextureKey (the actual decode-size authority) —
                // these feed the shader's UV scale, so if this disagrees with
                // what the texture was actually decoded at, the result is a
                // stretched/squashed sample: the shader thinks the texture is
                // one size, the data underneath it is a different size.
                uint8_t tlut = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
                const TileLoadInfo li0 = MakeTileLoadInfo(m_state.rdp, call.tileDesc0);
                const TileLoadInfo li1 = MakeTileLoadInfo(m_state.rdp, call.tileDesc1);
                int w0, h0;
                CalcTileSize(call.tileDesc0, tlut, w0, h0, &li0);
                if (w0 > 0 && h0 > 0) {
                    size0[0] = static_cast<float>(w0);
                    size0[1] = static_cast<float>(h0);
                }
                int w1, h1;
                CalcTileSize(call.tileDesc1, tlut, w1, h1, &li1);
                if (w1 > 0 && h1 > 0) {
                    size1[0] = static_cast<float>(w1);
                    size1[1] = static_cast<float>(h1);
                }

                m_shaderManager->SetTextureUniforms(0, 1, scale0, scale1, size0, size1, nullptr, nullptr);

                // Tile diagnostic: log first 15 triangle draw calls with texture,
                // and also log any draw call with vertices in the copyright region (x=[265,286], y=[42,62])
                {
                    // Gated behind NOVA64_COPY_DEBUG: the copyright-region branch
                    // logs EVERY center-screen triangle, which floods stderr
                    // (100MB+) and throttles headless runs via disk I/O.
                    static const bool s_copyDbg = []() {
                        const char* v = std::getenv("NOVA64_COPY_DEBUG");
                        return v && v[0] != '\0' && v[0] != '0';
                    }();
                    static uint32_t s_tileLog = 0;
                    bool inCopyrightRegion = false;
                    if (s_copyDbg && !isRect && call.vertexCount > 0) {
                        const auto& v0 = m_batch.vertices[call.vertexStart];
                        inCopyrightRegion = (v0.x >= 100.f && v0.x <= 320.f && v0.y >= 40.f && v0.y <= 240.f);
                    }
                    if (s_copyDbg && !isRect && (s_tileLog < 15 || inCopyrightRegion)) {
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

            // Tree-billboard uniform diagnostic (NOVA64_TREE_DBG=1): dumps the
            // exact sampler state for 32×64 TEX_EDGE draws (the SM64 tree
            // signature) — proves whether clamp/size/scale reach the shader.
            {
                static const bool s_treeDbg = []() {
                    const char* v = std::getenv("NOVA64_TREE_DBG");
                    return v && v[0] != '\0' && v[0] != '0';
                }();
                static int s_treeDbgLeft = 40;
                if (s_treeDbg && s_treeDbgLeft > 0 &&
                    call.tileDesc0.GetWidth() == 32 && call.tileDesc0.GetHeight() == 64 &&
                    (call.otherModeL & 0x1000) != 0) {
                    --s_treeDbgLeft;
                    uint8_t dbgTlut = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
                    int dbgW = 0, dbgH = 0;
                    const TileLoadInfo dbgLi = MakeTileLoadInfo(m_state.rdp, call.tileDesc0);
                    CalcTileSize(call.tileDesc0, dbgTlut, dbgW, dbgH, &dbgLi);
                    std::fprintf(stderr,
                        "[TREEDBG] type=%d clampS=%u clampT=%u maskS=%u maskT=%u "
                        "calcSize=(%d,%d) sl=%.1f tl=%.1f hash=0x%llX vtx=%d\n",
                        static_cast<int>(call.type),
                        static_cast<unsigned>(call.tileDesc0.clampS),
                        static_cast<unsigned>(call.tileDesc0.clampT),
                        static_cast<unsigned>(call.tileDesc0.maskS),
                        static_cast<unsigned>(call.tileDesc0.maskT),
                        dbgW, dbgH,
                        call.tileDesc0.GetSL(), call.tileDesc0.GetTL(),
                        static_cast<unsigned long long>(call.textureHash0),
                        call.vertexCount);
                }
            }

            // Set tile offset, clamp, mirror, mask uniforms
            {
                float offset0[2] = { call.tileDesc0.GetSL() + call.scrollS0, call.tileDesc0.GetTL() + call.scrollT0 };
                float offset1[2] = { call.tileDesc1.GetSL() + call.scrollS1, call.tileDesc1.GetTL() + call.scrollT1 };
                // clamp vec4 = (tile width if S clamps else 0, tile height if T
                // clamps else 0, mirrorS, mirrorT): the shader clamps texel
                // coordinates to the tile, like GLideN64's uTexClamp. An axis
                // clamps when its clamp bit is set or it has no mask (angrylion
                // clampens = cs || !mask_s), except in COPY mode.
                const bool copyCycle = ((call.otherModeH >> 20) & 0x03) == 2;
                auto clampLimit = [copyCycle](const TileDescriptor& td, bool s) -> float {
                    if (copyCycle) return 0.0f;
                    const bool clamps = s ? (td.IsClamped_S() || td.maskS == 0)
                                          : (td.IsClamped_T() || td.maskT == 0);
                    return clamps ? static_cast<float>(s ? td.GetWidth() : td.GetHeight()) : 0.0f;
                };
                float clamp0[4] = {
                    clampLimit(call.tileDesc0, true),
                    clampLimit(call.tileDesc0, false),
                    call.tileDesc0.IsMirrored_S() ? 1.0f : 0.0f,
                    call.tileDesc0.IsMirrored_T() ? 1.0f : 0.0f
                };
                float clamp1[4] = {
                    clampLimit(call.tileDesc1, true),
                    clampLimit(call.tileDesc1, false),
                    call.tileDesc1.IsMirrored_S() ? 1.0f : 0.0f,
                    call.tileDesc1.IsMirrored_T() ? 1.0f : 0.0f
                };
                float mask0[2] = { static_cast<float>(call.tileDesc0.maskS), static_cast<float>(call.tileDesc0.maskT) };
                float mask1[2] = { static_cast<float>(call.tileDesc1.maskS), static_cast<float>(call.tileDesc1.maskT) };
                m_shaderManager->SetTextureTileParams(offset0, offset1, clamp0, clamp1, mask0, mask1);

                // Bind-time uniform forensics (NOVA64_BINDDBG=1): dumps the exact
                // per-draw texture uniforms for small CI textures (skybox cells)
                // so a black-vs-correct cell can be diffed at the GPU boundary.
                static const bool s_bindDbg = std::getenv("NOVA64_BINDDBG") != nullptr;
                if (s_bindDbg && call.tileDesc0.format == 2 &&
                    call.tileDesc0.GetWidth() == 32) {
                    int bdW = 0, bdH = 0;
                    uint8_t bdTlut = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
                    const TileLoadInfo bdLi = MakeTileLoadInfo(m_state.rdp, call.tileDesc0);
                    CalcTileSize(call.tileDesc0, bdTlut, bdW, bdH, &bdLi);
                    std::fprintf(stderr,
                        "[BINDDBG] cmb=0x%llX crc=0x%016llX off0=(%.2f,%.2f) clamp0=(%.0f,%.0f,%.0f,%.0f) "
                        "mask0=(%.0f,%.0f) shift0=(%u,%u) calcSize=(%d,%d) tcBounds=%d "
                        "tex1hash=0x%llX off1=(%.2f,%.2f) tile1sltl=(%.2f,%.2f) tile1wh=(%d,%d)\n",
                        static_cast<unsigned long long>(call.combineMode),
                        static_cast<unsigned long long>(call.snapshotCrc0),
                        offset0[0], offset0[1],
                        clamp0[0], clamp0[1], clamp0[2], clamp0[3],
                        mask0[0], mask0[1],
                        static_cast<unsigned>(call.tileDesc0.shiftS),
                        static_cast<unsigned>(call.tileDesc0.shiftT),
                        bdW, bdH,
                        call.useTexCoordBounds ? 1 : 0,
                        static_cast<unsigned long long>(call.textureHash1),
                        offset1[0], offset1[1],
                        call.tileDesc1.GetSL(), call.tileDesc1.GetTL(),
                        call.tileDesc1.GetWidth(), call.tileDesc1.GetHeight());
                }
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
                    // COPY mode: N64 hardware KEEPS texels with alpha >= blend
                    // alpha (not strict ">"). The old "+half-LSB" here turned
                    // the test into ">": SM64's HUD text DLs set blendColor
                    // alpha = 255, so fully-opaque glyph texels (alpha == 1.0)
                    // failed 1.0 < 1.002 and the ENTIRE HUD was discarded.
                    // ref = blend.a exactly gives >= semantics in the shader's
                    // "alpha < ref → discard" test. blend.a == 0 keeps a tiny
                    // epsilon so zero-alpha texels still cut out (hardware
                    // keeps them, but games rely on transparent copy texels).
                    float ra = call.blendColor.a / 255.0f;
                    m_shaderManager->SetAlphaRef(ra > 0.0f ? ra : (0.5f / 255.0f));
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

                // NOTE: bF==1 (B references memColor.a, i.e. N64 "coverage")
                // is already fully resolved INSIDE the shader as a constant
                // 1.0 approximation (see ShaderGenerator.cpp's emitFinalCycleDS
                // — bB is baked into srcContrib/dstFactor, i.e. fragColor.rgb
                // or fragColor1, before GL blending ever runs).  A prior
                // version of this code additionally multiplied the GL blend
                // factor by real GL DstAlpha here, reasoning it needed to
                // "supply the real memColor.a".  That reasoning was wrong:
                // per GLideN64 (glsl_CombinerProgramBuilderCommon.cpp's
                // ShaderCoverage/uCoverageImage), N64 "coverage" is a
                // dedicated per-pixel sub-triangle sample count, tracked in
                // its own buffer (this project's own MLE-blender path does
                // the same via uCoverageImage) — it is NOT the framebuffer's
                // literal stored color alpha.  Once the canvas architecture
                // made framebuffer alpha persist correctly across draws
                // within a frame, that extra multiply started reading real
                // (and often 0, e.g. many sky texels' decoded alpha) values
                // instead of an incidental ~1.0, silently killing/zeroing
                // the blend on exactly the full-screen translucent-overlay
                // draws that triggered it — the residual "sky ghosting/
                // streaking" symptom.  Removed; the shader-side bB=1.0
                // approximation is already complete on its own.

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
        // NOTE: a prior heuristic here forced depth writes off for
        // CVG_DST_SAVE (cvgDst==3) and for ZMODE_XLU without FORCE_BL,
        // reasoning "games sort transparent draws themselves". Checked
        // both real-hardware-authoritative sources this pass:
        //   - GLideN64 GraphicsDrawer.cpp::_updateDepthUpdate():
        //     `gfxContext.enableDepthWrite(gDP.otherMode.depthUpdate != 0)`
        //     — gated ONLY by the Z_UPD bit, no zMode/cvgDst involvement.
        //   - angrylion-rdp-plus rasterizer.c: `if (wen) { fbwrite(...);
        //     if (other_modes.z_update_en) z_store(...); }` — same: only
        //     Z_UPD (ANDed with the pixel actually passing z_compare+blend).
        // Neither reference special-cases zMode or cvg_dst for the WRITE
        // decision (zMode only picks the compare function/bias; cvg_dst
        // only affects how coverage is combined). The heuristic silently
        // broke inter-transparent-surface depth ordering — removed;
        // depth.depthWriteEnabled already reflects Z_UPD via SF_DEPTH_WRITE.

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

        // Polygon offset: match GLideN64's _updateDepthCompare() — the
        // POLYGON_OFFSET_FILL toggle only lives inside GLideN64's non-N64-
        // accurate branch (config.frameBufferEmulation.N64DepthCompare ==
        // dcDisable).  When the accurate Z-image compare is active, GLideN64
        // disables GL depth test/write entirely and never touches polygon
        // offset — decal "closeness" is instead handled purely by the
        // shader's bNearer/bFarther tolerance window (see
        // GenerateDepthCompareCode's zMode==3 branch).  Applying a GL bias
        // here while SF_N64_DEPTH_COMPARE is active would perturb
        // gl_FragCoord.z, which our shader reads back as curZ for that same
        // window test — pushing decals outside [bufZ-dzMin, bufZ+dzMin] and
        // causing them to intermittently fail depth (flicker).  So only
        // apply the GL-side offset on the fallback (non-accurate) path.
        {
            RasterizerState rast{};
            rast.cullMode = CullMode::None;
            if (zMode == 3 && !(features & Shader::SF_N64_DEPTH_COMPARE)) {  // DECAL
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
            //
            // Gated to only apply when the game's OWN scissor already covers
            // most of the screen height. widescreenScale is a single global
            // (set once from the user's aspect-ratio config, applied to every
            // draw call regardless of content -- see VideoPlugin.cpp), so
            // this expansion previously fired unconditionally even for small,
            // narrow scissor rects a game sets to isolate one screen region
            // from another -- e.g. MM/OoT's pause-menu inventory grid, where
            // each item slot's small 3D rotating-icon draw gets its own tight
            // scissor rect. Blowing that open to the full screen height
            // removes the vertical isolation between grid rows: geometry that
            // real hardware's scissor would hard-clip to one slot (e.g. an
            // animating icon model momentarily extending past its intended
            // bounding box) is no longer clipped, letting it bleed into a
            // neighboring row -- a plausible mechanism for the observed
            // "stacked/duplicated icon" artifact. Only expand when the
            // original scissor already covers most of the frame vertically,
            // matching the documented intent (full-screen 3D content and
            // fullscreen fade/transition effects) without touching small,
            // deliberately-isolated regions.
            float wsScale = m_state.rdp.geometry.widescreenScale;
            constexpr float kWidescreenExpandMinCoverage = 0.6f;
            bool scissorAlreadyMostlyFull =
                n64H > 0.0f && (static_cast<float>(n64sh) >= kWidescreenExpandMinCoverage * n64H);
            if (wsScale != 1.0f && scissorAlreadyMostlyFull) {
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
        int hiResBits = 0;   // bit 0: texture 0 is a texture-pack picture, bit 1: texture 1
        int fbMono0 = 0, fbMono1 = 0, fbAlpha0 = 0, fbAlpha1 = 0;

        // A framebuffer bound as a texture holds the whole N64 buffer, not just
        // the tile that was loaded from it, so it is sampled in buffer texels:
        // the shader divides the tile coordinate by uTexSize and multiplies by
        // uGPUScale, so a framebuffer draw gets the buffer's size, a scale of 1,
        // and a tile offset measured from where the tile's texels start inside
        // the buffer (GLideN64 keeps that as the framebuffer texture's
        // offsetS/offsetT). Applied after binding, below.
        bool  fbSampling0 = false, fbSampling1 = false;
        float fbOffset0[2]  = {0.0f, 0.0f}, fbOffset1[2]  = {0.0f, 0.0f};
        float fbTexSize0[2] = {1.0f, 1.0f}, fbTexSize1[2] = {1.0f, 1.0f};
        auto fbTileOrigin = [](const GPUFramebuffer& fb, uint32_t texAddr, float& s, float& t) {
            const uint32_t base   = fb.desc.address & 0x7FFFFF;
            const uint32_t addr   = texAddr & 0x7FFFFF;
            const uint32_t stride = fb.desc.stride ? fb.desc.stride : fb.desc.width * 2;
            const uint32_t bpp    = fb.desc.size ? fb.desc.size : 2;
            const uint32_t delta  = addr > base ? addr - base : 0;
            s = static_cast<float>((delta % stride) / bpp);
            t = static_cast<float>(delta / stride);
        };

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
            key0.hiresTexCrc = call.snapHiRes0.texCrc;
            key0.hiresPalCrc = call.snapHiRes0.palCrc;
            key0.hiresWidth  = call.snapHiRes0.width;
            key0.hiresHeight = call.snapHiRes0.height;

            if (key0.width > 0 && key0.height > 0) {
                GPUFramebuffer* rawRTFB0 = g_fbManager.FindByAddress(key0.address);
                // Mirror ExecuteLoadTile's fallback (RDPProcessor_Texture.cpp):
                // FindByAddress's range pass additionally demands STRIDE
                // ALIGNMENT, so a read landing inside a known buffer at a
                // non-row-aligned offset returns null here even though the
                // buffer plainly contains that address. ExecuteLoadTile
                // compensates with FindContaining (a pure range search, like
                // GLideN64's findBuffer) but this draw-side lookup never did --
                // so the sync path could resolve the framebuffer while the draw
                // that consumes it could not, and the draw silently fell through
                // to decoding raw RDRAM. Measured with NOVA64_CANVASTEX_DBG:
                // rawRTFB0 was null on every single sample, which is what made
                // every framebuffer-dependent fix in this area dead code.
                if (!g_fbManager.IsGL() && !rawRTFB0)
                    rawRTFB0 = g_fbManager.FindContaining(key0.address);
                bool aliasRTFB0 = CanAliasFramebuffer(rawRTFB0, key0.address, IsRGBAFramebufferFormat(key0));
                // A buffer matched only by RANGE whose memory has been reused
                // since the RDP last drew into it is not a framebuffer read.
                // OOT renders Link's equipment-page preview into a 64x112 buffer
                // inside the heap; once the menu closes, gameplay textures are
                // loaded over that memory, and treating them as reads of the
                // stale buffer bound the preview image in place of Link's cap.
                if (rawRTFB0 && !aliasRTFB0 && !g_fbManager.RdramUnchanged(rawRTFB0, m_rdram, m_rdramSize))
                    rawRTFB0 = nullptr;
                GPUFramebuffer* rtFB0 = aliasRTFB0 ? rawRTFB0 : nullptr;

                MM_RDP_TRACE("BindTex0 drawType={} addr=0x{:06X} fmt={} size={} w={} h={} line={} pal={} tlut={} tmem={} crc=0x{:016X} fbFound={} fbAlias={} fbAddr=0x{:06X} color=0x{:06X} z=0x{:06X} shiftS={} shiftT={}",
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
                             m_state.rdp.zbufferImage.address & 0x7FFFFF,
                             static_cast<int>(key0.shiftS),
                             static_cast<int>(key0.shiftT));

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
                        {
                            static const bool s_dbg = std::getenv("NOVA64_FBDUMP") != nullptr;
                            static uint32_t s_skips = 0;
                            if (s_dbg && ++s_skips <= 40) {
                                std::fprintf(stderr, "[CFBSKIP] dropping TextureRect texAddr=0x%06X into curColor=0x%06X\n",
                                             key0.address & 0x7FFFFF,
                                             m_state.rdp.colorImage.address & 0x7FFFFF);
                            }
                        }
                        MM_RDP_TRACE("SkipTex0CfBCopy present={} addr=0x{:06X} drawType={} fmt={} size={}",
                                     g_presentIndex.load(std::memory_order_relaxed),
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
                    // Primary path: use the ring slot resolved at ENQUEUE time
                    // (call.snapshotSlot0, set in PopulateTextureInfo). MM's pause
                    // menu issues several SetColorImage switches to the SAME
                    // physical framebuffer address within a single frame (a full
                    // world redraw, then 1-2 much smaller UI-only overlay passes
                    // onto that same target) -- multiple ring slots end up holding
                    // the SAME address but very different content generations.
                    // Re-resolving by address at FLUSH time is ambiguous: it can't
                    // tell which same-address generation this draw call actually
                    // intended (a panel could bind a near-empty UI-only capture
                    // instead of the full world snapshot it needed -- the
                    // asymmetric black-pane bug). The enqueue-time slot was
                    // captured synchronously in DL-execution order, i.e. from
                    // whatever was ACTUALLY live at that address at the moment
                    // this draw was issued, exactly like real hardware would read.
                    if (call.snapshotSlot0 >= 0) {
                        const uint32_t slot0 = static_cast<uint32_t>(call.snapshotSlot0);
                        ITexture* snapTex = m_vkFBSnapshots[slot0].get();
                        // Re-validate the slot still holds the address it was resolved for
                        // in PopulateTextureInfo. Between this draw call's enqueue and this
                        // flush, other capture sites (SetColorImage switch-away in
                        // HandleSetColorImage, the LoadBlock CopyFromGPU-snapshot in
                        // RDPProcessor_Texture.cpp, or the last-resort canvas fallback
                        // below) can ping-pong m_vkFBSnapshotIdx back onto this SAME slot
                        // number and overwrite it with an unrelated framebuffer's content
                        // (e.g. a later 3D-model render target reusing the ring). Trusting
                        // a stale index here let that overwritten content silently replace
                        // the intended source (the OoT/MM pause-menu background snapshot
                        // getting clobbered by the Link-doll render).
                        if (snapTex && m_vkFBSnapshotAddrs[slot0] != (key0.address & 0x7FFFFF)) {
                            static const bool s_snapTrace = std::getenv("NOVA64_SNAPSHOT_TRACE") != nullptr;
                            if (s_snapTrace) {
                                std::fprintf(stderr,
                                    "[SNAPSHOT-STALE] slot=%u expectedAddr=0x%06X actualAddr=0x%06X -- "
                                    "stale ring slot caught and rejected\n",
                                    slot0, key0.address & 0x7FFFFF, m_vkFBSnapshotAddrs[slot0]);
                            }
                        }
                        if (snapTex && m_vkFBSnapshotAddrs[slot0] == (key0.address & 0x7FFFFF) &&
                            (rtFB0 || rawRTFB0)) {
                            fbTex0 = 1;
                            useVKSnapshot0 = true;
                            vkSnapshotTex0 = snapTex;
                            if (!rtFB0) rtFB0 = rawRTFB0;
                        }
                    }
                    // Fallback: address search (covers cases PopulateTextureInfo
                    // could not resolve, e.g. a snapshot captured mid-batch after
                    // this draw call was already enqueued but before flush).
                    for (uint32_t k = 0; k < kVkFBSnapshotSlots && fbTex0 == 0; ++k) {
                        uint32_t s = (m_vkFBSnapshotIdx + kVkFBSnapshotSlots - k) % kVkFBSnapshotSlots;
                        ITexture* snapTex = m_vkFBSnapshots[s].get();
                        if (!snapTex) continue;
                        const uint32_t snapAddr = m_vkFBSnapshotAddrs[s];
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

                    // Last resort — the fix for the black pause-menu panel.
                    // GLideN64 gives EVERY tracked framebuffer a permanent
                    // sampleable color texture (FrameBuffer::m_pTexture), so a
                    // framebuffer-as-texture read can always bind real rendered
                    // content. Here GetSampleableTexture() is 0 on Vulkan, and
                    // the snapshot ring above only captures at a few discrete
                    // EVENTS (SetColorImage switch-away, LoadBlock) -- so a read
                    // of a buffer that wasn't snapshotted at exactly the right
                    // moment fell through to m_textureCache->GetTexture() below,
                    // decoding raw RDRAM. For a buffer the GPU rendered (and
                    // that RDRAM therefore never received) that decodes stale
                    // uniform garbage: the black panel. The backend already
                    // keeps a persistent canvas image per N64 buffer address for
                    // buffer isolation -- those ARE the per-FB textures we were
                    // missing, so pull this buffer's canvas into a ring slot and
                    // bind it rather than decoding RDRAM.
                    if (fbTex0 == 0 && m_renderer) {
                        GPUFramebuffer* canvasFB = rtFB0 ? rtFB0 : rawRTFB0;
                        const uint32_t snapW = m_renderer->GetWidth();
                        const uint32_t snapH = m_renderer->GetHeight();
                        if (canvasFB && snapW > 0 && snapH > 0) {
                            const uint32_t canvasAddr = canvasFB->desc.address & 0x7FFFFF;
                            // Advance first so an in-flight frame's bound slot is
                            // not overwritten (same discipline as the capture sites).
                            m_vkFBSnapshotIdx = (m_vkFBSnapshotIdx + 1) % kVkFBSnapshotSlots;
                            auto& slot = m_vkFBSnapshots[m_vkFBSnapshotIdx];
                            if (!slot || slot->GetWidth() != snapW || slot->GetHeight() != snapH) {
                                TextureDesc snapDesc{};
                                snapDesc.width     = snapW;
                                snapDesc.height    = snapH;
                                snapDesc.format    = TextureFormat::RGBA8;
                                snapDesc.usage     = BufferUsage::Dynamic;
                                snapDesc.mipLevels = 1;
                                slot = m_renderer->CreateTexture(snapDesc);
                            }
                            const bool blitOK = slot && m_renderer->BlitCanvasToTexture(canvasAddr, slot.get());
                            if (blitOK) {
                                m_vkFBSnapshotAddrs[m_vkFBSnapshotIdx] = canvasAddr;
                                fbTex0 = 1;
                                useVKSnapshot0 = true;
                                vkSnapshotTex0 = slot.get();
                                if (!rtFB0) rtFB0 = rawRTFB0;
                                MM_RDP_TRACE("CanvasAsTex0 addr=0x{:06X} slot={}",
                                             canvasAddr, m_vkFBSnapshotIdx);
                            }
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
                    // Sample in buffer texels (see fbSampling0 above). The old
                    // 1/width scale on top of the shader's tile-size
                    // normalisation squeezed every read into the buffer's
                    // first texels, so each strip of a copied frame repeated
                    // the same few rows.
                    {
                        float originS = 0.0f, originT = 0.0f;
                        fbTileOrigin(*rtFB0, key0.address, originS, originT);
                        fbSampling0   = true;
                        fbOffset0[0]  = call.tileDesc0.GetSL() + call.scrollS0 - originS;
                        fbOffset0[1]  = call.tileDesc0.GetTL() + call.scrollT0 - originT;
                        fbTexSize0[0] = static_cast<float>(rtFB0->desc.width);
                        fbTexSize0[1] = static_cast<float>(rtFB0->desc.height);
                    }
                    gpuScale0[0] = 1.0f;
                    gpuScale0[1] = 1.0f;
                    // FB-as-texture monochrome detection (matches GLideN64 UFrameBufferInfo):
                    //  size==1 (8-bit): mono mode 1 (red channel), fixedAlpha if IM_RD off
                    //  tile IA16: mono mode 2 (weighted luminance)
                    if (rtFB0->desc.size == 1) {
                        fbMono0 = 1;
                        if ((call.otherModeL & 0x40) == 0) fbAlpha0 = 1; // IM_RD off
                    } else if (call.tileDesc0.size == 2 && call.tileDesc0.format == 3) {
                        fbMono0 = 2; // IA16 luminance
                    }
                    MM_RDP_TRACE("UseFBTex0 present={} addr=0x{:06X} fbTex={} fbAddr=0x{:06X} fbSize={}x{} gpuScale={:.4f},{:.4f} mono={} alpha={}",
                                 g_presentIndex.load(std::memory_order_relaxed),
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
                            // A tile chain (G_TL_LOD) is read by explicit level, and GL
                            // ignores the level unless minification uses mipmaps, so the
                            // LOD path only ever saw the base tile.
                            GLenum glMinFilter = glFilter;
                            if (mipCount > 0)
                                glMinFilter = (texFilter == 3) ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glMinFilter);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter);
                        }
                        gpuScale0[0] = cached0->scaleS;
                        gpuScale0[1] = cached0->scaleT;
                        if (cached0->isHiRes) hiResBits |= 1;
                        MM_RDP_TRACE("UseCachedTex0 present={} addr=0x{:06X} scale={:.4f},{:.4f} mipCount={} texFilter={}",
                                     g_presentIndex.load(std::memory_order_relaxed),
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
                        MM_RDP_TRACE("FallbackWhiteTex0 present={} addr=0x{:06X} fmt={} size={} w={} h={} crc=0x{:016X}",
                                     g_presentIndex.load(std::memory_order_relaxed),
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
            key1.hiresTexCrc = call.snapHiRes1.texCrc;
            key1.hiresPalCrc = call.snapHiRes1.palCrc;
            key1.hiresWidth  = call.snapHiRes1.width;
            key1.hiresHeight = call.snapHiRes1.height;
            if (key1.width > 0 && key1.height > 0) {
                GPUFramebuffer* rawRTFB1 = g_fbManager.FindByAddress(key1.address);
                // Same stride-alignment fallback as tex0 above.
                if (!g_fbManager.IsGL() && !rawRTFB1)
                    rawRTFB1 = g_fbManager.FindContaining(key1.address);
                bool aliasRTFB1 = CanAliasFramebuffer(rawRTFB1, key1.address, IsRGBAFramebufferFormat(key1));
                if (rawRTFB1 && !aliasRTFB1 && !g_fbManager.RdramUnchanged(rawRTFB1, m_rdram, m_rdramSize))
                    rawRTFB1 = nullptr;   // memory reused since the last draw; see tex0 above
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

                // Vulkan: mirror the tex0 swapchain-snapshot fallback (see above).
                // Without this, a 2-cycle combiner draw that reads a framebuffer
                // address through texture unit 1 (e.g. UI overlays that blend a
                // world snapshot on tex0 with an icon/mask on tex1, or vice versa)
                // fell straight through to m_textureCache->GetTexture() below, which
                // decodes raw RDRAM/TMEM bytes for an address the GPU renders to but
                // never writes back to RDRAM on the Vulkan path — garbage/noise.
                bool useVKSnapshot1 = false;
                ITexture* vkSnapshotTex1 = nullptr;
                if (fbTex1 == 0 && !g_fbManager.IsGL()) {
                    // Primary path: enqueue-time resolved slot -- see the tex0 note above.
                    if (call.snapshotSlot1 >= 0) {
                        const uint32_t slot1 = static_cast<uint32_t>(call.snapshotSlot1);
                        ITexture* snapTex = m_vkFBSnapshots[slot1].get();
                        // Re-validate against the address it was resolved for -- see the
                        // matching comment on the tex0 primary path above.
                        if (snapTex && m_vkFBSnapshotAddrs[slot1] == (key1.address & 0x7FFFFF) &&
                            (rtFB1 || rawRTFB1)) {
                            fbTex1 = 1;
                            useVKSnapshot1 = true;
                            vkSnapshotTex1 = snapTex;
                            if (!rtFB1) rtFB1 = rawRTFB1;
                        }
                    }
                    // Fallback: address search, most-recently-captured first.
                    for (uint32_t k = 0; k < kVkFBSnapshotSlots && fbTex1 == 0; ++k) {
                        uint32_t s = (m_vkFBSnapshotIdx + kVkFBSnapshotSlots - k) % kVkFBSnapshotSlots;
                        ITexture* snapTex = m_vkFBSnapshots[s].get();
                        if (!snapTex) continue;
                        const uint32_t snapAddr = m_vkFBSnapshotAddrs[s];
                        if (rtFB1 && snapAddr == (rtFB1->desc.address & 0x7FFFFF)) {
                            fbTex1 = 1;
                            useVKSnapshot1 = true;
                            vkSnapshotTex1 = snapTex;
                        } else if (!rtFB1 && rawRTFB1 &&
                                   snapAddr == (rawRTFB1->desc.address & 0x7FFFFF)) {
                            fbTex1 = 1;
                            useVKSnapshot1 = true;
                            vkSnapshotTex1 = snapTex;
                            rtFB1 = rawRTFB1;
                        }
                    }

                    // Last resort — mirrors tex0's canvas-blit fallback (the
                    // fix for the black pause-menu panel) above. Tex1 never
                    // had this: only the two ring-based paths, which depend
                    // on the 4-slot snapshot ring (m_vkFBSnapshots) still
                    // holding this address's entry. MM/OoT's pause-menu
                    // world-snapshot background is read through tex1 (a
                    // 2-cycle combiner blending it with a darkening tint),
                    // and the ring churns fast -- the routine double-buffer
                    // and Z-buffer-reuse-as-color-target captures alone
                    // consume 2+ new ring slots per frame -- so a one-time
                    // background capture falls out of the ring within a
                    // frame or two. Without this fallback, every read after
                    // that point had nowhere left to go but raw, never-
                    // synced RDRAM. Confirmed via a live capture: reads
                    // succeeded via the ring for exactly the first two
                    // frames, then failed on every following frame with no
                    // recovery, even though the framebuffer itself remained
                    // correctly resolvable (rtFB1 non-null) the whole time --
                    // the persistent canvas this reads from survives ring
                    // churn entirely, unlike the ring itself.
                    if (fbTex1 == 0 && m_renderer) {
                        GPUFramebuffer* canvasFB1 = rtFB1 ? rtFB1 : rawRTFB1;
                        const uint32_t snapW = m_renderer->GetWidth();
                        const uint32_t snapH = m_renderer->GetHeight();
                        if (canvasFB1 && snapW > 0 && snapH > 0) {
                            const uint32_t canvasAddr1 = canvasFB1->desc.address & 0x7FFFFF;
                            m_vkFBSnapshotIdx = (m_vkFBSnapshotIdx + 1) % kVkFBSnapshotSlots;
                            auto& slot1 = m_vkFBSnapshots[m_vkFBSnapshotIdx];
                            if (!slot1 || slot1->GetWidth() != snapW || slot1->GetHeight() != snapH) {
                                TextureDesc snapDesc1{};
                                snapDesc1.width     = snapW;
                                snapDesc1.height    = snapH;
                                snapDesc1.format    = TextureFormat::RGBA8;
                                snapDesc1.usage     = BufferUsage::Dynamic;
                                snapDesc1.mipLevels = 1;
                                slot1 = m_renderer->CreateTexture(snapDesc1);
                            }
                            const bool blitOK1 = slot1 && m_renderer->BlitCanvasToTexture(canvasAddr1, slot1.get());
                            if (blitOK1) {
                                m_vkFBSnapshotAddrs[m_vkFBSnapshotIdx] = canvasAddr1;
                                fbTex1 = 1;
                                useVKSnapshot1 = true;
                                vkSnapshotTex1 = slot1.get();
                                if (!rtFB1) rtFB1 = rawRTFB1;
                                MM_RDP_TRACE("CanvasAsTex1 addr=0x{:06X} slot={}",
                                             canvasAddr1, m_vkFBSnapshotIdx);
                            }
                        }
                    }
                }

                if (fbTex1 != 0) {
                    if (g_fbManager.IsGL()) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, fbTex1);
                    } else if (useVKSnapshot1 && vkSnapshotTex1) {
                        m_renderer->BindTexture(1, vkSnapshotTex1);
                    }
                    // Sample in buffer texels -- see fbSampling0 above.
                    {
                        float originS = 0.0f, originT = 0.0f;
                        fbTileOrigin(*rtFB1, key1.address, originS, originT);
                        fbSampling1   = true;
                        fbOffset1[0]  = call.tileDesc1.GetSL() + call.scrollS1 - originS;
                        fbOffset1[1]  = call.tileDesc1.GetTL() + call.scrollT1 - originT;
                        fbTexSize1[0] = static_cast<float>(rtFB1->desc.width);
                        fbTexSize1[1] = static_cast<float>(rtFB1->desc.height);
                    }
                    gpuScale1[0] = 1.0f;
                    gpuScale1[1] = 1.0f;
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
                        if (cached1->isHiRes) hiResBits |= 2;
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
        if (fbSampling0 || fbSampling1) {
            m_shaderManager->SetTextureUniforms(0, 1, nullptr, nullptr,
                                                fbSampling0 ? fbTexSize0 : nullptr,
                                                fbSampling1 ? fbTexSize1 : nullptr,
                                                nullptr, nullptr);
            // A framebuffer is read without the tile's clamp, wrap or mirror,
            // as in GLideN64.
            static const float kNoClamp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            static const float kNoMask[2]  = {0.0f, 0.0f};
            m_shaderManager->SetTextureTileParams(fbSampling0 ? fbOffset0 : nullptr,
                                                  fbSampling1 ? fbOffset1 : nullptr,
                                                  fbSampling0 ? kNoClamp : nullptr,
                                                  fbSampling1 ? kNoClamp : nullptr,
                                                  fbSampling0 ? kNoMask : nullptr,
                                                  fbSampling1 ? kNoMask : nullptr);
        }
        m_shaderManager->SetGPUScale(gpuScale0, gpuScale1);
        m_shaderManager->SetTexHiRes(hiResBits);
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