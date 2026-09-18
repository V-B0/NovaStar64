#include "RDPProcessor_Private.hpp"
#include <cstdlib>
#include <cstdio>
#include <atomic>
void RDPProcessor::HandleTriangle(const uint32_t* words, int wordCount) {
    DecodedTriangle tri;
    m_decoder.DecodeTriangle(words, wordCount, tri);
    DrawTriangle(tri);
}

void RDPProcessor::HandleTextureRect(const uint32_t* words) {
    if (m_hacks.skipTexRect) return;  // WinBack: grey rectangle fix
    DecodedRectangle rect;
    m_decoder.DecodeTextureRect(words, rect);

    // COPY mode: the RDP processes 4 texels per clock cycle.
    // dSdX is in "per cycle" units; divide by 4 for our per-pixel renderer.
    // XL also needs +1 because the hardware uses an exclusive right edge in COPY.
    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;

    // Diagnostic trace for UI/HUD invisibility investigation: dumps the
    // pre-widescreen rect plus the scissor + cycle + combiner + Z_CMP state
    // that influences whether the rect renders.  Gated by NOVA_RDP_TRACE.
    MM_RDP_TRACE("TexRect rect=({:.2f},{:.2f})-({:.2f},{:.2f}) dsDx={:.4f} dtDy={:.4f} "
                 "cycle={} otherL=0x{:08X} otherH=0x{:08X} combine=0x{:016X} "
                 "scissor=({},{})-({},{}) wsScale={:.3f} tile={}",
                 rect.x0, rect.y0, rect.x1, rect.y1, rect.dsDx, rect.dtDy,
                 cycleType, m_state.rdp.otherModeL, m_state.rdp.otherModeH,
                 m_state.rdp.combineModeRaw,
                 m_state.rdp.scissor.xh >> 2, m_state.rdp.scissor.yh >> 2,
                 m_state.rdp.scissor.xl >> 2, m_state.rdp.scissor.yl >> 2,
                 m_state.rdp.geometry.widescreenScale, rect.tileIndex);
    if (cycleType == 2) {
        // COPY mode (GLideN64 model): dsDx is in 4-texels-per-clock units →
        // /4 for per-pixel sampling, and the lower-right edge is INCLUSIVE →
        // +1. NO corner/block rounding: an earlier "round to 4-pixel blocks"
        // interpretation (misread from RT64) drew up to 3 extra pixel rows/
        // columns per rect whose samples lay outside the glyph — the stray
        // fringe pixels around every HUD icon. The exact rect has no such
        // pixels, so no clamping gymnastics are needed either.
        rect.dsDx /= 4.0f;
        rect.x1 += 1.0f;
        rect.y1 += 1.0f;
    } else if (rect.y1 - rect.y0 < 1.0f) {
        rect.y1 = std::ceil(rect.y1);  // Sub-pixel height fixup
    }

    // Widescreen: skip letterbox bars and scale non-fullscreen rects inward.
    float wsScale = m_state.rdp.geometry.widescreenScale;
    if (wsScale != 1.0f) {
        float ciWidth = static_cast<float>(m_state.rdp.colorImage.width);
        if (ciWidth < 1.0f) ciWidth = 320.0f;
        float screenH = static_cast<float>(m_state.rdp.scissor.yl >> 2);
        if (screenH < 1.0f) screenH = 240.0f;
        float rectW = rect.x1 - rect.x0;
        float rectH = rect.y1 - rect.y0;
        bool isFullWidth = (rect.x0 <= 1.0f && rect.x1 >= ciWidth - 1.0f);

        // Skip letterbox bars: full-width, thin (<25% height), touching top/bottom edge.
        if (isFullWidth && rectH < screenH * 0.25f &&
            (rect.y0 <= 1.0f || rect.y1 >= screenH - 1.0f)) {
            return;
        }

        // Scale non-fullscreen rects inward to maintain 4:3 proportions.
        // Fullscreen = covers full width AND >75% height (fades, transitions).
        bool isFullscreen = isFullWidth && rectH > screenH * 0.75f;
        if (!isFullscreen) {
            float n64ScreenW = static_cast<float>(m_state.rdp.scissor.xl >> 2);
            if (n64ScreenW < 1.0f) n64ScreenW = 320.0f;
            float invScale = 1.0f / wsScale;
            float halfW = n64ScreenW * 0.5f;
            float offsetX = halfW * (1.0f - invScale);
            rect.x0 = rect.x0 * invScale + offsetX;
            rect.x1 = rect.x1 * invScale + offsetX;
            // Compensate texture coordinates: the rect is narrower but
            // must display the same texture content, so increase dsDx.
            rect.dsDx *= wsScale;
        } else {
            // Fullscreen transition/fade: expand Y to cover the full
            // framebuffer height so it fills the expanded 3D viewport.
            rect.y0 = 0.0f;
            rect.y1 = 240.0f;
        }
    }

    DrawTextureRect(rect);
}

void RDPProcessor::HandleTextureRectFlip(const uint32_t* words) {
    if (m_hacks.skipTexRect) return;  // WinBack: grey rectangle fix
    DecodedRectangle rect;
    m_decoder.DecodeTextureRectFlip(words, rect);

    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 2) {
        // After flip swap: dsDx = orig dtDy, dtDy = orig dsDx.
        // COPY mode divides the original dsDx by 4 — which is now in dtDy.
        // x0/y0/x1/y1 are screen-space rect corners decoded identically to
        // the non-flip TextureRect (flip only changes the S/T axis mapping,
        // not the rect corners), so use the same fix as the non-flip path:
        // the lower-right edge is inclusive in COPY mode (+1), no 4-pixel
        // block rounding. The old block-rounding here was never updated when
        // it was removed from HandleTextureRect for causing stray fringe
        // pixels around every HUD icon — same bug, just not yet propagated.
        rect.dtDy /= 4.0f;
        rect.x1 += 1.0f;
        rect.y1 += 1.0f;
    } else if (rect.y1 - rect.y0 < 1.0f) {
        rect.y1 = std::ceil(rect.y1);  // Sub-pixel height fixup
    }

    // Widescreen: skip letterbox bars and scale non-fullscreen rects inward.
    float wsScaleFlip = m_state.rdp.geometry.widescreenScale;
    if (wsScaleFlip != 1.0f) {
        float ciWidth = static_cast<float>(m_state.rdp.colorImage.width);
        if (ciWidth < 1.0f) ciWidth = 320.0f;
        float screenH = static_cast<float>(m_state.rdp.scissor.yl >> 2);
        if (screenH < 1.0f) screenH = 240.0f;
        float rectW = rect.x1 - rect.x0;
        float rectH = rect.y1 - rect.y0;
        bool isFullWidth = (rect.x0 <= 1.0f && rect.x1 >= ciWidth - 1.0f);

        if (isFullWidth && rectH < screenH * 0.25f &&
            (rect.y0 <= 1.0f || rect.y1 >= screenH - 1.0f)) {
            return;
        }

        bool isFullscreen = isFullWidth && rectH > screenH * 0.75f;
        if (!isFullscreen) {
            float n64ScreenW = static_cast<float>(m_state.rdp.scissor.xl >> 2);
            if (n64ScreenW < 1.0f) n64ScreenW = 320.0f;
            float invScale = 1.0f / wsScaleFlip;
            float halfW = n64ScreenW * 0.5f;
            float offsetX = halfW * (1.0f - invScale);
            rect.x0 = rect.x0 * invScale + offsetX;
            rect.x1 = rect.x1 * invScale + offsetX;
            // Compensate texture coordinates for narrower rect.
            rect.dsDx *= wsScaleFlip;
        } else {
            rect.y0 = 0.0f;
            rect.y1 = 240.0f;
        }
    }

    DrawTextureRect(rect);
}

void RDPProcessor::HandleFillRect(const uint32_t* words) {
    float x0, y0, x1, y1;
    m_decoder.DecodeFillRect(words, x0, y0, x1, y1);

    bool sameColorDepth = (m_state.rdp.colorImage.address == m_state.rdp.zbufferImage.address &&
                           m_state.rdp.colorImage.address != 0);
    MM_RDP_TRACE("FillRect raw=0x{:08X} rect=({:.2f},{:.2f})-({:.2f},{:.2f}) color=0x{:06X} z=0x{:06X} sameColorDepth={} colorSize={} cycle={}",
                 m_state.rdp.fillColorRaw,
                 x0,
                 y0,
                 x1,
                 y1,
                 m_state.rdp.colorImage.address & 0x7FFFFF,
                 m_state.rdp.zbufferImage.address & 0x7FFFFF,
                 sameColorDepth,
                 static_cast<int>(m_state.rdp.colorImage.size),
                 static_cast<int>((m_state.rdp.otherModeH >> 20) & 0x03));

    // �� Detect depth buffer clear ��
    // The N64 clears the depth buffer by setting the color image to the depth
    // buffer address, then issuing FillRect with the canonical depth-clear
    // value. Some games, including Majora's Mask, also use the depth-image
    // address as an auxiliary color buffer, so only the real clear value should
    // be intercepted here.
    constexpr uint32_t kN64DepthClearFillColor = 0xFFFCFFFCu;
    {
        static const bool s_cvDbg = std::getenv("NOVA64_CANVAS_DBG") != nullptr;
        if (s_cvDbg && sameColorDepth) {
            std::fprintf(stderr, "[ZCLR] fill-to-zbuf addr=0x%06X raw=0x%08X size=%d rect=(%.0f,%.0f)-(%.0f,%.0f) intercept=%d\n",
                         m_state.rdp.colorImage.address & 0x7FFFFF,
                         m_state.rdp.fillColorRaw,
                         static_cast<int>(m_state.rdp.colorImage.size),
                         x0, y0, x1, y1,
                         (m_state.rdp.fillColorRaw == kN64DepthClearFillColor &&
                          m_state.rdp.colorImage.size == ImageSize::Bpp16) ? 1 : 0);
        }
    }
    if (m_state.rdp.colorImage.address == m_state.rdp.zbufferImage.address &&
        m_state.rdp.colorImage.size == ImageSize::Bpp16 &&
        m_state.rdp.fillColorRaw == kN64DepthClearFillColor) {
        MM_RDP_TRACE("DepthClearIntercept raw=0x{:08X} addr=0x{:06X}",
                     m_state.rdp.fillColorRaw,
                     m_state.rdp.colorImage.address & 0x7FFFFF);
        FlushBatch();
        if (g_fbManager.IsGL() && g_fbManager.GetCurrentColor()) {
            GLuint fbo = g_fbManager.GetCurrentColor()->fbo;
            glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, 1.0f, 0);
        } else if (!g_fbManager.IsGL() && m_renderer) {
            // Vulkan: clear the active swapchain depth attachment via
            // vkCmdClearAttachments so HUD geometry drawn after this point
            // starts with a fresh depth slate instead of stale scene depth.
            m_renderer->ClearDepthBuffer(1.0f);
        }
        // Also clear the N64 Z-image so imageLoad-based depth compare
        // sees max depth (1.0) for newly cleared pixels.
        auto* depthBuf = g_depthManager.GetCurrent();
        if (depthBuf) {
            g_depthManager.ClearDepthImageTextures(*depthBuf);
        }
        return;
    }

    // Re-decode fill color at draw time using the CURRENT color image size
    // (in case it changed since SetFillColor was called).
    DecodeFillColor(m_state.rdp.fillColorRaw, m_state.rdp.colorImage.size,
                    m_state.rdp.fillColor.r, m_state.rdp.fillColor.g,
                    m_state.rdp.fillColor.b, m_state.rdp.fillColor.a);

    // In FILL mode (cycleType=3), the N64 RDP uses inclusive right/bottom edges.
    // OpenGL rasterization uses exclusive right/bottom (top-left fill rule).
    // Add 1 pixel to compensate so the last column/row is filled.
    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 3) {
        // GLideN64's gDPFillRectangle in G_CYC_FILL does exactly this and
        // nothing else: "++lrx; ++lry;" -- no 4-pixel block alignment.
        // The previous "RT64-style ulx&=~3 / lrx|=3" rounding here was the
        // same discredited hack already found and removed from the
        // TextureRect COPY-mode path (it shifted fades/depth-clears/HUD
        // backplates by up to 3px), just never propagated to this handler.
        x1 += 1.0f;
        y1 += 1.0f;
    } else if (y1 == y0) {
        y1 += 1.0f;
    }

    // Widescreen: skip game-drawn letterbox bars.
    // Detect by position: full-width FillRects touching the top or bottom edge
    // that are thin (< 25% of screen height) are cinematic bars.
    float wsScaleFill = m_state.rdp.geometry.widescreenScale;
    if (wsScaleFill != 1.0f) {
        float ciWidth = static_cast<float>(m_state.rdp.colorImage.width);
        if (ciWidth < 1.0f) ciWidth = 320.0f;
        float screenH = static_cast<float>(m_state.rdp.scissor.yl >> 2);
        if (screenH < 1.0f) screenH = 240.0f;
        bool isFullWidth = (x0 <= 1.0f && x1 >= ciWidth - 1.0f);
        float rectH = y1 - y0;
        bool isThin = (rectH < screenH * 0.25f);
        bool touchesTop = (y0 <= 1.0f);
        bool touchesBottom = (y1 >= screenH - 1.0f);

        if (isFullWidth && isThin && (touchesTop || touchesBottom)) {
            return;  // Skip letterbox bar
        }

        // Expand fullscreen FillRects to cover the full Y range
        // so transitions/color fills cover the expanded 3D viewport.
        if (isFullWidth && rectH > screenH * 0.75f) {
            y0 = 0.0f;
            y1 = 240.0f;
        }
    }

    DrawFillRect(x0, y0, x1, y1);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Drawing
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::DrawTriangle(const DecodedTriangle& tri) {
    if (m_batch.IsFull()) FlushBatch();

    // Diagnostic: log first 15 RDP triangles to verify screen-space positions.
    {
        static uint32_t s_triLog = 0;
        if (s_triLog < 15) {
            ++s_triLog;
            Log::infof("[RDPTRI#%u] lr=%d yh=%.2f ym=%.2f yl=%.2f xh=%.2f xm=%.2f xl=%.2f dxh=%.3f dxm=%.3f dxl=%.3f shade=%d tex=%d z=%d",
                       s_triLog,
                       (int)tri.edge.leftMajor,
                       tri.edge.yh, tri.edge.ym, tri.edge.yl,
                       tri.edge.xh, tri.edge.xm, tri.edge.xl,
                       tri.edge.dxhDy, tri.edge.dxmDy, tri.edge.dxlDy,
                       (int)tri.hasShade, (int)tri.hasTexture, (int)tri.hasDepth);
        }
    }

    // NOVA64_GLYPH_DUMP: dump small textured triangles (dialog-font-glyph
    // candidates) with their texture T edge coefficients + tile size, to
    // diagnose the vertically-flipped dialog font.
    {
        static const bool s_glyphDump = []() {
            const char* v = std::getenv("NOVA64_GLYPH_DUMP");
            return v && v[0] && v[0] != '0';
        }();
        if (s_glyphDump && tri.hasTexture) {
            const auto& tl = m_state.rdp.tiles[tri.tileIndex & 7];
            int th = (static_cast<int>(tl.th) - static_cast<int>(tl.tl)) >> 2; // texels
            int twd = (static_cast<int>(tl.sh) - static_cast<int>(tl.sl)) >> 2;
            float scrH = static_cast<float>(tri.edge.yl - tri.edge.yh);
            if (th >= 0 && th <= 32 && twd >= 0 && twd <= 32 && scrH < 48.0f) {
                static std::atomic<int> n{0};
                if (n.fetch_add(1, std::memory_order_relaxed) < 400) {
                    bool persp = (m_state.rdp.otherModeH & OTHERMODE_TEXTPERSP) != 0;
                    std::fprintf(stderr,
                        "[GLYPH] tile=%d texWxH=%dx%d scrY=[%.1f..%.1f] persp=%d "
                        "S(base=%.1f dDe=%.3f dDx=%.3f) T(base=%.1f dDe=%.3f dDx=%.3f) "
                        "tl_sl=%d tl_tl=%d fmt=%d siz=%d\n",
                        tri.tileIndex, twd, th, (float)tri.edge.yh, (float)tri.edge.yl, (int)persp,
                        (float)tri.texture.s, (float)tri.texture.dsDe, (float)tri.texture.dsDx,
                        (float)tri.texture.t, (float)tri.texture.dtDe, (float)tri.texture.dtDx,
                        (int)tl.sl, (int)tl.tl, (int)tl.format, (int)tl.size);
                }
            }
        }
    }

    // Create draw call
    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = tri.tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = m_state.rdp.geometry.geometryMode;
    PopulateTextureInfo(call, tri.tileIndex);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;
    
    // N64 RDP triangles are defined by a major edge and two minor edges.
    // They are split at the middle Y coordinate (ym).
    double yh = tri.edge.yh;
    double ym = tri.edge.ym;
    double yl = tri.edge.yl;
    double xh = tri.edge.xh;
    double xm = tri.edge.xm;
    double xl = tri.edge.xl;
    double dxhdy = tri.edge.dxhDy;
    double dxmdy = tri.edge.dxmDy;
    double dxldy = tri.edge.dxlDy;

    // Attribute interpolation helper
    auto interpAttr = [&](double base, double dDe, double dDx, double x, double y) -> float {
        double dy = y - yh;
        double xMajor = xh + dxhdy * dy; // major edge X at this Y
        return static_cast<float>(base + dDe * dy + dDx * (x - xMajor));
    };

    auto fillVertex = [&](RenderBatch::Vertex& v, double x, double y) {
        v.x = static_cast<float>(x);
        v.y = static_cast<float>(y);
        v.w = 1.0f;

        if (tri.hasDepth) {
            float rawZ = interpAttr(tri.depth.z, tri.depth.dzDe, tri.depth.dzDx, x, y);
            // Log first vertex raw z value (before clamp) to understand N64 Z scale
            {
                static bool s_zLogged = false;
                if (!s_zLogged) {
                    s_zLogged = true;
                    Log::infof("[RAWZ] rawZ=%.6f baseZ=%.6f dzDe=%.8f dzDx=%.8f",
                               rawZ, (float)tri.depth.z, (float)tri.depth.dzDe, (float)tri.depth.dzDx);
                }
            }
            v.z = std::clamp(rawZ / 32767.0f, 0.0f, 1.0f);
        } else {
            v.z = 0.0f;
        }

        if (tri.hasTexture && tri.texture.w != 0.0f) {
            // N64 stores S in (s_texel * W_clip * 16) and W in (W_clip * 16384).
            // Dividing W by 1024 gives the correct perspective denominator:
            //   tc = S / (W/1024) = s_texel * W_clip * 16 * 1024 / (W_clip * 16384)
            //      = s_texel  (correct texel coordinate)
            // When TEXTPERSP is OFF, S is s_texel*16 (no W_clip factor) and W_clip≈1
            // for ortho, so the same formula still gives tc = s_texel.
            float wInterp = interpAttr(tri.texture.w, tri.texture.dwDe, tri.texture.dwDx, x, y);
            v.perspW = wInterp / 1024.0f;
            v.s = interpAttr(tri.texture.s, tri.texture.dsDe, tri.texture.dsDx, x, y);
            v.t = interpAttr(tri.texture.t, tri.texture.dtDe, tri.texture.dtDx, x, y);
        } else {
            v.perspW = 1.0f;
            v.s = v.t = 0.0f;
        }

        if (tri.hasShade) {
            v.r = interpAttr(tri.shade.r, tri.shade.drDe, tri.shade.drDx, x, y) / 255.0f;
            v.g = interpAttr(tri.shade.g, tri.shade.dgDe, tri.shade.dgDx, x, y) / 255.0f;
            v.b = interpAttr(tri.shade.b, tri.shade.dbDe, tri.shade.dbDx, x, y) / 255.0f;
            v.a = interpAttr(tri.shade.a, tri.shade.daDe, tri.shade.daDx, x, y) / 255.0f;
            v.fogFactor = v.a;
        } else {
            v.r = v.g = v.b = v.a = 1.0f;
            v.fogFactor = 0.0f;
        }
        v.s1 = v.t1 = 0;
        v.clipFlags = 0;
    };

    // Calculate mid-point on major edge
    double x_major_mid = xh + dxhdy * (ym - yh);
    
    // Triangle 1 (Top half: yh to ym)
    // Vertices: (xh, yh), (xm, yh), (xl, ym), (x_major_mid, ym)
    RenderBatch::Vertex vt0, vt1, vt2, vt3;
    fillVertex(vt0, xh, yh);
    fillVertex(vt1, xm, yh);
    fillVertex(vt2, xl, ym);
    fillVertex(vt3, x_major_mid, ym);

    // Standard N64 triangle starts as a single point: xh == xm
    bool hasTop = std::abs(ym - yh) > 0.001;
    if (hasTop) {
        // T1 = (vt0, vt1, vt2): degenerate when xh == xm (vt0 == vt1).
        // Also requires the correct sub-pixel orientation: for leftMajor=1
        // the major (left) edge starts at xh and the minor (right) edge at
        // xm, so T1 is only valid when xm > xh (minor is right of major).
        // For leftMajor=0 the roles are reversed and T1 is valid when xh > xm.
        // Drawing T1 with the inverted sign covers the wrong pixels and
        // produces visible artifacts (lines) on the rendered model.
        bool t1valid = tri.edge.leftMajor ? (xm - xh > 0.001) : (xh - xm > 0.001);
        // T2 = (vt0, vt2, vt3): degenerate when xl == x_major_mid (vt2 == vt3)
        // Direction-aware: for leftMajor=1, xl (minor/right) must be right of x_major_mid (major/left).
        // Drawing with inverted sign covers wrong pixels and produces line artifacts.
        bool t2valid = tri.edge.leftMajor ? (xl - x_major_mid > 0.001) : (x_major_mid - xl > 0.001);
        if (t1valid) {
            m_batch.vertices.push_back(vt0);
            m_batch.vertices.push_back(vt1);
            m_batch.vertices.push_back(vt2);
            call.vertexCount += 3;
        }
        if (t2valid) {
            m_batch.vertices.push_back(vt0);
            m_batch.vertices.push_back(vt2);
            m_batch.vertices.push_back(vt3);
            call.vertexCount += 3;
        }
    }

    // Triangle 2 (Bottom half: ym to yl)
    // Vertices: (xl, ym), (x_major_mid, ym), (xl + dxldy*(yl-ym), yl), (xh + dxhdy*(yl-yh), yl)
    bool hasBot = std::abs(yl - ym) > 0.001;
    if (hasBot) {
        double x_minor_bot = xl + dxldy * (yl - ym);
        double x_major_bot = xh + dxhdy * (yl - yh);
        
        RenderBatch::Vertex vb0, vb1, vb2, vb3;
        fillVertex(vb0, xl, ym);
        fillVertex(vb1, x_major_mid, ym);
        fillVertex(vb2, x_minor_bot, yl);
        fillVertex(vb3, x_major_bot, yl);

        // T3 = (vb0, vb1, vb2): degenerate when xl == x_major_mid (vb0 == vb1)
        // Direction-aware: same condition as T2 — xl must be on the correct side of x_major_mid.
        bool t3valid = tri.edge.leftMajor ? (xl - x_major_mid > 0.001) : (x_major_mid - xl > 0.001);
        // T4 = (vb1, vb2, vb3): extra bottom-corner triangle.
        // Same orientation rule as T1: only valid when the extra strip at yl
        // has the correct left-right direction for the given leftMajor flag.
        // For leftMajor=1 (major left): valid when x_minor_bot > x_major_bot.
        // For leftMajor=0 (major right): valid when x_major_bot > x_minor_bot.
        bool t4valid = tri.edge.leftMajor ? (x_minor_bot - x_major_bot > 0.001) : (x_major_bot - x_minor_bot > 0.001);
        if (t3valid) {
            m_batch.vertices.push_back(vb0);
            m_batch.vertices.push_back(vb1);
            m_batch.vertices.push_back(vb2);
            call.vertexCount += 3;
        }
        if (t4valid) {
            m_batch.vertices.push_back(vb1);
            m_batch.vertices.push_back(vb2);
            m_batch.vertices.push_back(vb3);
            call.vertexCount += 3;
        }
    }

    if (call.vertexCount > 0) {
        m_batch.AppendDrawCall(call);
        m_stats.trianglesDrawn += (call.vertexCount / 3);
        m_stats.frameTriangles += (call.vertexCount / 3);
        m_state.rdp.triangleCount++;
    }
}

void RDPProcessor::DrawTriangle(int v0, int v1, int v2, const GeometryState& geo) {
    if (v0 == v1 || v1 == v2 || v2 == v0) return;
    if (m_batch.IsFull()) FlushBatch();

    // NOVA64_GLYPH_DUMP: settle which path draws in-game + dump small textured
    // triangles' (screenY, T) so we can see if texture-T is inverted vs screen
    // Y for the dialog font (the vertically-flipped glyphs).
    {
        static const bool s_glyphDump = []() {
            const char* v = std::getenv("NOVA64_GLYPH_DUMP");
            return v && v[0] && v[0] != '0';
        }();
        if (s_glyphDump) {
            long curVI = g_presentIndex.load(std::memory_order_relaxed);
            static std::atomic<long> hb{0};
            long h = hb.fetch_add(1, std::memory_order_relaxed);
            if ((h % 20000) == 0)
                std::fprintf(stderr, "[HLETRI-HB] call=%ld VI=%ld\n", h, curVI);
            // Only dump once we're past the intro (dialog appears ~VI 3500+),
            // and only true glyph-sized tiles (font chars are <=16 px).
            if (curVI >= 3000 && geo.texture.enabled) {
                const auto& tile = m_state.rdp.tiles[geo.texture.tileIndex & 7];
                int th = (static_cast<int>(tile.th) - static_cast<int>(tile.tl)) >> 2;
                int tw = (static_cast<int>(tile.sh) - static_cast<int>(tile.sl)) >> 2;
                if (th >= 4 && th <= 20 && tw >= 2 && tw <= 20) {
                    static std::atomic<int> n{0};
                    if (n.fetch_add(1, std::memory_order_relaxed) < 400) {
                        auto sy = [&](int idx) {
                            const auto& tv = geo.vertexBuffer[idx];
                            if (tv.modifiedXY) return tv.screenY;
                            float invW = (tv.position[3]!=0.0f)?1.0f/tv.position[3]:1.0f;
                            return tv.position[1]*invW*geo.viewport.scaleY+geo.viewport.transY;
                        };
                        const auto& a=geo.vertexBuffer[v0]; const auto& b=geo.vertexBuffer[v1]; const auto& c=geo.vertexBuffer[v2];
                        std::fprintf(stderr,
                            "[GLYPH] VI=%ld tile=%d texWxH=%dx%d scaleT=%.3f "
                            "v0(sy=%.1f T=%.3f) v1(sy=%.1f T=%.3f) v2(sy=%.1f T=%.3f)\n",
                            g_presentIndex.load(std::memory_order_relaxed),
                            geo.texture.tileIndex, tw, th, geo.texture.GetScaleTFloat(),
                            sy(v0), a.texCoord[1], sy(v1), b.texCoord[1], sy(v2), c.texCoord[1]);
                    }
                }
            }
        }
    }

    auto makeVert = [&](int idx) -> RenderBatch::Vertex {
        const auto& tv = geo.vertexBuffer[idx];
        RenderBatch::Vertex v{};
        // Perspective divide: NDC first, then apply N64 viewport scale/translate
        float invW = (tv.position[3] != 0.0f) ? 1.0f / tv.position[3] : 1.0f;
        if (tv.modifiedXY) {
            // gSPModifyVertex XYSCREEN: screenX/Y are already in pixel coords
            v.x = tv.screenX;
            v.y = tv.screenY;
        } else {
            v.x = tv.position[0] * invW * geo.viewport.scaleX + geo.viewport.transX;
            v.y = tv.position[1] * invW * geo.viewport.scaleY + geo.viewport.transY;
            // NOTE: 1/4-pixel snap intentionally NOT applied here. The N64 RSP
            // does snap, but our texcoords are stored as s*invW (pre-divided
            // for perspective interpolation). When the camera is close to a
            // surface invW is large, so quantising the screen position by
            // 0.25 px makes the corresponding texel step visibly each time
            // the camera moves — the "horizontal/vertical back-and-forth"
            // texture march users see on water and walls. The far-distance
            // wobble that motivated snapping is solved by using the FBO size
            // (not the per-draw scissor) for the ortho projection in
            // FlushBatch, which is the actual stable mapping.
        }
        // Normalize screen-space z to [0,1]. The ortho maps this to clip_z = 2*z_norm - 1 ? [-1,1]
        // (full NDC range). Near z_norm=0 � window_depth=0, Far z_norm=1 � window_depth=1.0 (clear).
        // Without normalization, z_screen ? [0,1024] � clip_z ? [-1,3] � far geometry clipped.
        if (tv.modifiedZ) {
            // gSPModifyVertex ZSCREEN: screenZ is already the final normalized depth
            v.z = tv.screenZ;
        } else {
            float z_screen = tv.position[2] * invW * geo.viewport.scaleZ + geo.viewport.transZ;
            float absScaleZ = std::abs(geo.viewport.scaleZ);
            v.z = (absScaleZ > 0.0001f)
                ? (z_screen - geo.viewport.transZ + absScaleZ) / (2.0f * absScaleZ)
                : 0.0f;
        }
        v.z = std::max(0.0f, std::min(1.0f, v.z));  // clamp: extrapolated/clipped verts may stray outside
        // Bias away from exactly 1.0 so far-plane geometry passes GL_LESS.
        // Without this, vertices at the maximum viewport Z normalize to
        // exactly 1.0, which equals the depth-clear value and fails the
        // strict less-than test, making far-plane surfaces invisible.
        if (v.z >= 1.0f) v.z = 0.99999f;
        v.w = 1.0f;  // ortho maps screen coords -> NDC; clip W already divided out
        v.r = tv.color[0]; v.g = tv.color[1];
        v.b = tv.color[2]; v.a = tv.color[3];
        // Apply gSPTexture scale at draw time so the correct scale is used
        // even if gSPTexture is issued after gSPVertex.
        float scaleS = geo.texture.enabled ? geo.texture.GetScaleSFloat() : 1.0f;
        float scaleT = geo.texture.enabled ? geo.texture.GetScaleTFloat() : 1.0f;
        // When texturePersp is ON (the common case), store texcoords
        // pre-multiplied by 1/W so the fragment shader can recover
        // perspective-correct texels via (vTexCoord / vClipW).
        // When texturePersp is OFF, the N64 does affine (linear) texturing,
        // so store raw texcoords and set perspW=1 � the shader division
        // by vClipW is then an identity op.
        // N64 RSP outputs ST at 2x scale in non-perspective mode; compensate
        // with *0.5 (matches GLideN64 vertex shader: texCoord *= 0.5).
        bool textPersp = (m_state.rdp.otherModeH & OTHERMODE_TEXTPERSP) != 0;
        float tcW = textPersp ? invW : 1.0f;
        float affineHalf = textPersp ? 1.0f : 0.5f;
        v.s  = tv.texCoord[0]  * scaleS * tcW * affineHalf; v.t  = tv.texCoord[1]  * scaleT * tcW * affineHalf;
        v.s1 = tv.texCoord1[0] * scaleS * tcW * affineHalf; v.t1 = tv.texCoord1[1] * scaleT * tcW * affineHalf;
        v.perspW = tcW;
        v.fogFactor = tv.fogFactor;
        v.clipFlags = tv.clipFlags;  // Copy clip flags for frustum culling
        return v;
    };

    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = geo.texture.tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = geo.geometryMode;
    PopulateTextureInfo(call, geo.texture.tileIndex);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;

    m_batch.vertices.push_back(makeVert(v0));
    m_batch.vertices.push_back(makeVert(v1));
    m_batch.vertices.push_back(makeVert(v2));

    // Prim shading: when G_LIGHTING is off AND G_SHADE is off, the shade
    // input should be primColor (not vertex color). GLideN64 ref: GraphicsDrawer.cpp.
    if (!(geo.geometryMode & G_LIGHTING) && !(geo.geometryMode & G_SHADE)) {
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        float pr = m_state.rdp.primColor.r / 255.0f;
        float pg = m_state.rdp.primColor.g / 255.0f;
        float pb = m_state.rdp.primColor.b / 255.0f;
        float pa = m_state.rdp.primColor.a / 255.0f;
        for (int i = 0; i < 3; i++) {
            verts[base + i].r = pr; verts[base + i].g = pg;
            verts[base + i].b = pb; verts[base + i].a = pa;
        }
    }
    // Flat shading: when G_SHADING_SMOOTH is not set, use provoking vertex
    // (v0) color for all three vertices.
    else if (!(geo.geometryMode & 0x00200000)) { // G_SHADING_SMOOTH
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        verts[base + 1].r = verts[base].r; verts[base + 1].g = verts[base].g;
        verts[base + 1].b = verts[base].b; verts[base + 1].a = verts[base].a;
        verts[base + 2].r = verts[base].r; verts[base + 2].g = verts[base].g;
        verts[base + 2].b = verts[base].b; verts[base + 2].a = verts[base].a;
    }

    call.vertexCount = 3;
    m_batch.AppendDrawCall(call);

    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTriangleRaw(const RenderBatch::Vertex& rv0, const RenderBatch::Vertex& rv1,
                                   const RenderBatch::Vertex& rv2, uint32_t geometryMode,
                                   uint8_t tileIndex) {
    if (m_batch.IsFull()) FlushBatch();

    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = geometryMode;
    PopulateTextureInfo(call, tileIndex);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;

    m_batch.vertices.push_back(rv0);
    m_batch.vertices.push_back(rv1);
    m_batch.vertices.push_back(rv2);

    // Prim shading for clipped triangles
    if (!(geometryMode & G_LIGHTING) && !(geometryMode & G_SHADE)) {
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        float pr = m_state.rdp.primColor.r / 255.0f;
        float pg = m_state.rdp.primColor.g / 255.0f;
        float pb = m_state.rdp.primColor.b / 255.0f;
        float pa = m_state.rdp.primColor.a / 255.0f;
        for (int i = 0; i < 3; i++) {
            verts[base + i].r = pr; verts[base + i].g = pg;
            verts[base + i].b = pb; verts[base + i].a = pa;
        }
    }
    // Flat shading for clipped triangles: provoking vertex color is rv0
    else if (!(geometryMode & 0x00200000)) { // G_SHADING_SMOOTH
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        verts[base + 1].r = verts[base].r; verts[base + 1].g = verts[base].g;
        verts[base + 1].b = verts[base].b; verts[base + 1].a = verts[base].a;
        verts[base + 2].r = verts[base].r; verts[base + 2].g = verts[base].g;
        verts[base + 2].b = verts[base].b; verts[base + 2].a = verts[base].a;
    }

    call.vertexCount = 3;

    m_batch.AppendDrawCall(call);
    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTextureRect(const DecodedRectangle& rect) {
    if (m_batch.IsFull()) FlushBatch();

    // �� Special depth-buffer texture rect detection ����������������������
    // Some N64 games (Zelda OoT, Perfect Dark, �) read the depth buffer via
    // textured rectangles for shadows (texturedRectShadowMap) or depth
    // readback (texturedRectDepthBufferCopy).
    bool shadowMapRect = false;
    {
        uint32_t texAddr  = m_state.rdp.textureImage.address & 0x7FFFFF;
        uint32_t zbAddr   = m_state.rdp.zbufferImage.address & 0x7FFFFF;
        uint8_t  texSize  = static_cast<uint8_t>(m_state.rdp.textureImage.size);
        uint32_t fbWidth  = m_state.rdp.colorImage.width;

        // Depth buffer range: conservative upper bound (width � width � 1.5).
        uint32_t zbRange  = fbWidth * fbWidth * 3 / 2;

        const bool readsDepthBuffer = texSize == 2 && zbAddr != 0 &&
                                      texAddr >= zbAddr && texAddr < zbAddr + zbRange;
        // Only the games GLideN64 applies these to (see m_hacks). Other games
        // put ordinary pictures there: Majora's Mask stores its pause backdrop
        // at the depth buffer's address, and treating those strips as a
        // shadow map replaced the picture with a depth-fog overlay.
        if (readsDepthBuffer && m_hacks.texRectDepthBufferCopy &&
            rect.y0 == 0.0f && rect.y1 <= 1.0f) {
            // texturedRectDepthBufferCopy: a 1-row copy of depth data from
            // TMEM back to RDRAM. The GPU has nothing to render.
            return;
        }
        // texturedRectShadowMap: depth read drawn as a fog-coloured shadow.
        if (readsDepthBuffer && m_hacks.texRectShadowMap) {
            shadowMapRect = true;
        }
    }

    // N64 hardware quirk (matches GLideN64's _calcTileSizes): a degenerate
    // 1x1 declared tile size in COPY cycle mode is not literal -- the RDP
    // infers the real texture width/height from the rectangle's own
    // screen-space span instead of the tile's SETTILESIZE. Some games rely
    // on this rather than setting a matching tile size for a straight,
    // unscaled COPY-mode blit. Temporarily patch the tile's sh/th so the
    // size calculation downstream (CalcTileSize, in PopulateTextureInfo)
    // sees the corrected dimensions for this one draw, then restore --
    // the tile slot's real SETTILESIZE state must not be permanently
    // clobbered for whatever draw reuses it next.
    auto& tileForRect = m_state.rdp.tiles[rect.tileIndex];
    int32_t savedSh = tileForRect.sh, savedTh = tileForRect.th;
    bool copyRectSizeOverridden = false;
    uint32_t rectCycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (rectCycleType == 2 && tileForRect.GetWidth() == 1 && tileForRect.GetHeight() == 1) {
        int rectW = std::max(1, static_cast<int>(std::lround(rect.x1 - rect.x0)));
        int rectH = std::max(1, static_cast<int>(std::lround(rect.y1 - rect.y0)));
        tileForRect.sh = tileForRect.sl + (rectW - 1) * 4;
        tileForRect.th = tileForRect.tl + (rectH - 1) * 4;
        copyRectSizeOverridden = true;
    }

    DrawCall call;
    call.type = DrawCall::Type::TextureRect;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = rect.tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = 0;
    PopulateTextureInfo(call, rect.tileIndex);

    if (copyRectSizeOverridden) {
        tileForRect.sh = savedSh;
        tileForRect.th = savedTh;
    }

    MM_RDP_TRACE("TextureRect tile={} rect=({:.2f},{:.2f})-({:.2f},{:.2f}) st=({:.2f},{:.2f}) dsdx={:.4f} dtdy={:.4f} tex0=0x{:06X} tex1=0x{:06X} hash0=0x{:016X} hash1=0x{:016X} color=0x{:06X} z=0x{:06X} shadow={}",
                 rect.tileIndex,
                 rect.x0,
                 rect.y0,
                 rect.x1,
                 rect.y1,
                 rect.s,
                 rect.t,
                 rect.dsDx,
                 rect.dtDy,
                 call.textureImageAddr & 0x7FFFFF,
                 call.textureImageAddr1 & 0x7FFFFF,
                 call.textureHash0,
                 call.textureHash1,
                 m_state.rdp.colorImage.address & 0x7FFFFF,
                 m_state.rdp.zbufferImage.address & 0x7FFFFF,
                 shadowMapRect);

    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;
    call.isShadowMap = shadowMapRect;

    // Snapshot TLUT from TMEM palette region for shadow map draws.
    // TMEM layout: palette at byte offset 0x800 (2048).  Each of the 256
    // entries occupies an 8-byte TMEM word; the first two bytes are the
    // 16-bit palette value (big-endian IA16).  Upper byte = intensity/alpha.
    if (shadowMapRect) {
        const uint8_t* tmem = m_state.rdp.tmem.data;
        for (int i = 0; i < 256; i++) {
            call.shadowTLUT[i] = tmem[0x800 + i * 8];
        }
    }
    
    // Generate 4 vertices (two triangles) for the rectangle
    float width = rect.x1 - rect.x0;
    float height = rect.y1 - rect.y0;
    
    float s0 = rect.s;
    float t0 = rect.t;

    // HACK: Some games set s=512.0 (raw 0x4000) for narrow textures.
    // When the tile ULS + colorImage width < 512, the offset wraps
    // incorrectly. Force s to 0. (GLideN64 ref: gDP.cpp gDPTextureRectangle)
    if (s0 == 512.0f) {
        auto& tile = m_state.rdp.tiles[rect.tileIndex];
        if (m_state.rdp.colorImage.width + (tile.sl >> 2) < 512)
            s0 = 0.0f;
    }
    float s1, t1;
    
    if (rect.flipped) {
        s1 = s0 + rect.dtDy * height;  // Flipped: S changes with Y
        t1 = t0 + rect.dsDx * width;   // T changes with X
    } else {
        s1 = s0 + rect.dsDx * width;
        t1 = t0 + rect.dtDy * height;
    }
    
    // Normalize to screen space [-1, 1]
    // The renderer will handle the actual viewport transformation
    RenderBatch::Vertex v[4];
    
    // Top-left (shade color = 0 for TextureRect, matching GLideN64 uRectColor)
    v[0] = { rect.x0, rect.y0, 0, 1, s0, t0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // Top-right
    v[1] = { rect.x1, rect.y0, 0, 1, s1, t0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // Bottom-left
    v[2] = { rect.x0, rect.y1, 0, 1, s0, t1, 0, 0, 0, 0, 0, 0, 0, 0 };
    // Bottom-right
    v[3] = { rect.x1, rect.y1, 0, 1, s1, t1, 0, 0, 0, 0, 0, 0, 0, 0 };
    
    if (rect.flipped) {
        v[1].s = s0;         v[1].t = t1;
        v[2].s = s1;         v[2].t = t0;
    }

    // N64 TextureRect: both tiles use the same S/T coordinates.
    // Copy tile 0 UVs into tile 1 slots for 2-cycle mode multi-tile rendering.
    for (int i = 0; i < 4; i++) {
        v[i].s1 = v[i].s;
        v[i].t1 = v[i].t;
    }

    // Compute texture coordinate bounds to prevent bilinear bleed.
    // Use pixel-center snapping: RDP samples at integer pixel centers,
    // so the first sampled pixel is at ceil(x0), last at ceil(x1)-1.
    // COPY mode: bound at the PRE-block-expansion edge (boundX1/boundY1) —
    // the expanded region must repeat the edge texel, not sample past it.
    {
        // COPY mode block expansion: bounds span the ORIGINAL rect's texels
        // (boundX0..boundX1), evaluated in the current (expanded-corner)
        // vertex mapping — expansion pixels then clamp to the glyph's own
        // edge texels on all four sides, like hardware.
        float bx0 = (rect.boundX0 >= 0.0f) ? rect.boundX0 : rect.x0;
        float by0 = (rect.boundY0 >= 0.0f) ? rect.boundY0 : rect.y0;
        float bx1 = (rect.boundX1 >= 0.0f) ? rect.boundX1 : rect.x1;
        float by1 = (rect.boundY1 >= 0.0f) ? rect.boundY1 : rect.y1;
        float uls, lrs, ult, lrt;
        if (rect.flipped) {
            // Flipped: after decoder swap, dtDy = original dSdX, dsDx = original dTdY
            // S varies with Y (rate = dtDy), T varies with X (rate = dsDx)
            uls = s0 + (std::ceil(by0) - rect.y0) * rect.dtDy;
            lrs = s0 + (std::ceil(by1) - rect.y0 - 1.0f) * rect.dtDy;
            ult = t0 + (std::ceil(bx0) - rect.x0) * rect.dsDx;
            lrt = t0 + (std::ceil(bx1) - rect.x0 - 1.0f) * rect.dsDx;
        } else {
            // Normal: S varies with X (rate = dsDx), T varies with Y (rate = dtDy)
            uls = s0 + (std::ceil(bx0) - rect.x0) * rect.dsDx;
            lrs = s0 + (std::ceil(bx1) - rect.x0 - 1.0f) * rect.dsDx;
            ult = t0 + (std::ceil(by0) - rect.y0) * rect.dtDy;
            lrt = t0 + (std::ceil(by1) - rect.y0 - 1.0f) * rect.dtDy;
        }
        float minS = std::min(uls, lrs);
        float minT = std::min(ult, lrt);
        float maxS = std::max(uls, lrs);
        float maxT = std::max(ult, lrt);
        call.texCoordBounds0[0] = minS;
        call.texCoordBounds0[1] = minT;
        call.texCoordBounds0[2] = maxS;
        call.texCoordBounds0[3] = maxT;
        call.texCoordBounds1[0] = minS;
        call.texCoordBounds1[1] = minT;
        call.texCoordBounds1[2] = maxS;
        call.texCoordBounds1[3] = maxT;
        call.useTexCoordBounds = true;
    }
    
    // Compute clip flags (texture rects in screen space: w=1, so no clipping expected)
    for (int i = 0; i < 4; i++) {
        v[i].clipFlags = 0;  // Screen-space rects: always inside frustum
    }
    
    // Two triangles: (0, 1, 2), (1, 3, 2)
    m_batch.vertices.push_back(v[0]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[2]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[3]);
    m_batch.vertices.push_back(v[2]);
    call.vertexCount = 6;
    
    m_batch.AppendDrawCall(call);
    
    m_stats.rectsDrawn++;
    m_stats.frameRects++;
    m_state.rdp.rectCount++;
}

void RDPProcessor::DrawFillRect(float x0, float y0, float x1, float y1) {
    if (m_batch.IsFull()) FlushBatch();

    uint32_t rdpOtherModeH = m_state.rdp.otherModeH;
    uint32_t rdpOtherModeL = m_state.rdp.otherModeL;
    uint32_t rdpCycleType = (rdpOtherModeH >> 20) & 0x03;

    DrawCall call;
    call.type = DrawCall::Type::FillRect;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = 0;
    call.otherModeH = rdpOtherModeH;
    call.otherModeL = rdpOtherModeL;
    call.geometryMode = 0;
    call.textureHash0 = 0;   // Fill rects never use textures
    call.textureHash1 = 0;
    call.textureImageAddr = 0;
    call.textureImageAddr1 = 0;
    std::memset(&call.tileDesc0, 0, sizeof(call.tileDesc0));
    std::memset(&call.tileDesc1, 0, sizeof(call.tileDesc1));
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;

    float fr, fg, fb, fa;
    if (rdpCycleType == 3) {
        // FILL mode: raw fill color written directly to framebuffer.
        // Alpha = 0: on real N64 FILL mode does not set sub-pixel coverage,
        // so mem_cvg for cleared areas is 0.  Writing alpha=0 emulates that,
        // preventing the MLE blender from incorrectly averaging the first
        // 3-D draw with the background color.
        call.combineMode = 0;
        fr = static_cast<float>(m_state.rdp.fillColor.r) / 255.0f;
        fg = static_cast<float>(m_state.rdp.fillColor.g) / 255.0f;
        fb = static_cast<float>(m_state.rdp.fillColor.b) / 255.0f;
        fa = 0.0f;
    } else {
        // 1-cycle / 2-cycle mode: route through the actual combiner.
        // Shade color input = (0,0,0,0) since FillRect has no vertex shading.
        call.combineMode = m_state.rdp.combineModeRaw;
        fr = 0.0f;
        fg = 0.0f;
        fb = 0.0f;
        fa = 0.0f;
    }
    
    // Two triangles for the fill rectangle
    RenderBatch::Vertex v[4] = {
        { x0, y0, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
        { x1, y0, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
        { x0, y1, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
        { x1, y1, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
    };
    
    // Compute clip flags (fill rects in screen space: w=1, so no clipping expected)
    for (int i = 0; i < 4; i++) {
        v[i].clipFlags = 0;  // Screen-space rects: always inside frustum
    }
    
    m_batch.vertices.push_back(v[0]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[2]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[3]);
    m_batch.vertices.push_back(v[2]);
    call.vertexCount = 6;
    
    m_batch.AppendDrawCall(call);
    
    m_stats.fillRectsDrawn++;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Helpers
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::PopulateTextureInfo(DrawCall& call, int tileIndex) {
    // Snapshot tile descriptors first — we need tmemAddr to look up the source address.
    const auto& tile0 = m_state.rdp.tiles[tileIndex & 7];
    const auto& tile1 = m_state.rdp.tiles[(tileIndex + 1) & 7];
    call.tileDesc0 = tile0;
    call.tileDesc1 = tile1;

    // Resolve the RDRAM source address for each tile via TMEM-slot tracking.
    // N64 games load textures into a "loading tile" (typically tile 7) via G_LOADBLOCK;
    // tmemSourceAddress[tile7.tmemAddr] is set to the source RDRAM address at that time.
    // The render tile (tile 0) shares the same tmemAddr, so the lookup here returns the
    // correct address even when a later G_SETTEXTUREIMAGE (e.g. for TLUT loading) has
    // already changed textureImage.address by the time we reach the draw call.
    uint32_t addr0 = m_state.rdp.tmemSourceAddress[tile0.tmemAddr & 0x1FF];
    uint32_t addr1 = m_state.rdp.tmemSourceAddress[tile1.tmemAddr & 0x1FF];
    // Fall back to per-tile-index tracking (set when LoadBlock targets tile 0 directly)
    if (addr0 == 0) addr0 = m_state.rdp.tileLoadAddress[tileIndex & 7];
    if (addr1 == 0) addr1 = m_state.rdp.tileLoadAddress[(tileIndex + 1) & 7];
    // Last resort: current textureImage address
    if (addr0 == 0) addr0 = m_state.rdp.textureImage.address;
    if (addr1 == 0) addr1 = m_state.rdp.textureImage.address;
    call.textureImageAddr  = addr0;
    call.textureImageAddr1 = addr1;

    // Resolve the FB-snapshot ring slot NOW, at enqueue time, while the ring
    // reflects the state as of this exact draw call (see the field comment
    // on snapshotSlot0/1 in RDPProcessor.hpp for why address-only lookup at
    // flush time is ambiguous). Search most-recently-written first so a
    // match reflects whatever was actually last written to that address.
    call.snapshotSlot0 = -1;
    call.snapshotSlot1 = -1;
    if (!g_fbManager.IsGL()) {
        const uint32_t a0 = addr0 & 0x7FFFFF;
        const uint32_t a1 = addr1 & 0x7FFFFF;
        for (uint32_t k = 0; k < kVkFBSnapshotSlots; ++k) {
            uint32_t s = (m_vkFBSnapshotIdx + kVkFBSnapshotSlots - k) % kVkFBSnapshotSlots;
            if (!m_vkFBSnapshots[s]) continue;
            if (call.snapshotSlot0 < 0 && m_vkFBSnapshotAddrs[s] == a0) call.snapshotSlot0 = static_cast<int32_t>(s);
            if (call.snapshotSlot1 < 0 && m_vkFBSnapshotAddrs[s] == a1) call.snapshotSlot1 = static_cast<int32_t>(s);
            if (call.snapshotSlot0 >= 0 && call.snapshotSlot1 >= 0) break;
        }
    }

    // RSP tile-clone for HLE triangles:
    // When texture LOD is enabled in the RDP mode word AND the RSP has
    // texture.level == 0 (base level only), the microcode clones
    // tile[tileIndex] into tile[tileIndex+1] so that texel1 reads valid
    // TMEM data.  Without this clone, tile1 may point to stale TMEM
    // (e.g. the palette area), causing noise artifacts.
    //
    // When LOD is NOT enabled, tile0 and tile1 are independent textures
    // used by the 2-cycle combiner (e.g. the NINTENDO64 metallic text
    // effect which blends two tiles via PRIM_LOD_FRAC).  In that case
    // we must NOT clone, otherwise the second texture is lost.
    int level = static_cast<int>(m_state.rdp.geometry.texture.level);
    bool textureLOD = ((call.otherModeH >> 16) & 1) != 0;
    if (level == 0 && call.type == DrawCall::Type::Triangle && textureLOD) {
        call.tileDesc1 = call.tileDesc0;
    }

    // Snapshot mipmap tile chain when LOD mipmapping is active.
    // The RSP sets texture.level to the number of mip levels beyond the base.
    // Tiles are laid out as: base=tiles[tileIndex], mip1=tiles[tileIndex+1], etc.
    call.mipLevelCount = 0;
    if (level > 0 && m_state.rdp.geometry.texture.enabled) {
        int count = std::min(level, 7);
        for (int i = 0; i < count; i++) {
            call.mipTiles[i] = m_state.rdp.tiles[(tileIndex + i + 1) & 7];
        }
        call.mipLevelCount = count;
    }

    // Compute a quick hash to determine whether a texture is active.
    // A tile is "active" if it has non-zero dimensions and a valid TMEM address
    // or the texture image has been loaded.
    auto computeHash = [&](const TileDescriptor& td, uint32_t rdramAddr) -> uint64_t {
        int w = td.GetWidth();
        int h = td.GetHeight();
        // N64 games can set inverted tile rects (sh < sl).  Use absolute
        // dimensions for the hash so the texture is still treated as active.
        w = std::abs(w);
        h = std::abs(h);
        if (w == 0 || h == 0) return 0;

        // Hash from tile fields + textureImage address
        uint64_t hash = 0;
        hash ^= static_cast<uint64_t>(rdramAddr) * 0x9e3779b97f4a7c15ULL;
        hash ^= static_cast<uint64_t>(td.tmemAddr) << 16;
        hash ^= static_cast<uint64_t>(td.format) << 48;
        hash ^= static_cast<uint64_t>(td.size) << 52;
        hash ^= static_cast<uint64_t>(w) << 8;
        hash ^= static_cast<uint64_t>(h) << 24;
        hash ^= static_cast<uint64_t>(td.palette) << 40;
        hash ^= static_cast<uint64_t>(td.line);
        if (hash == 0) hash = 1; // Ensure non-zero for active textures
        return hash;
    };

    // Check cycle type — in 2-cycle mode both tiles can be active
    uint32_t cycleType = (call.otherModeH >> 20) & 0x03;
    call.textureHash0 = computeHash(call.tileDesc0, call.textureImageAddr);
    // When tile1 was cloned from tile0 (LOD active, level==0, triangle),
    // there is no independent second texture.  Clear textureHash1 so
    // SF_TEXTURE_1 is not set; the shader defaults texel1 = texel0.
    if (level == 0 && call.type == DrawCall::Type::Triangle && textureLOD)
        call.textureHash1 = 0;
    else
        call.textureHash1 = (cycleType == 1) ? computeHash(call.tileDesc1, call.textureImageAddr1) : 0;

    // Copy scrolling when tile1 was cloned from tile0, use tile0's scroll values
    call.scrollS0 = m_state.rdp.scrollS[tileIndex & 7];
    call.scrollT0 = m_state.rdp.scrollT[tileIndex & 7];
    if (level == 0 && call.type == DrawCall::Type::Triangle && textureLOD) {
        call.scrollS1 = call.scrollS0;
        call.scrollT1 = call.scrollT0;
    } else {
        call.scrollS1 = m_state.rdp.scrollS[(tileIndex + 1) & 7];
        call.scrollT1 = m_state.rdp.scrollT[(tileIndex + 1) & 7];
    }

    // Snapshot the TMEM content CRC at draw-queue time.
    // BuildTextureKey (called later at flush time) reads TMEM when building the
    // texture cache key. If a subsequent LoadBlock overwrites TMEM before the
    // batch is flushed, all batched draws would be keyed against the LAST loaded
    // texture rather than their own.  Capturing the CRC now, while TMEM holds
    // the correct data, avoids this flush-time mismatch.
    //
    // That fix only protects the CACHE KEY though -- the actual pixel DECODE
    // (TextureCache::GetTexture's cache-miss path) still reads live TMEM at
    // FLUSH time via m_state.rdp.tmem.data. A batch of several small texture
    // draws sharing one render state (e.g. many TextureRect strips building up
    // one image row by row -- MM's pause-menu background construction) never
    // triggers an intervening flush (NeedsBatchFlush only compares render
    // state, not texture identity), so every strip's LoadTile call keeps
    // overwriting the SAME TMEM region before FlushBatch ever runs. By the
    // time GetTexture actually decodes each (correctly, uniquely keyed) entry,
    // every one of them decodes from whatever TMEM holds AFTER THE LAST
    // strip's load -- silently caching identical (usually the final strip's)
    // pixel content under every different key. Symptom: a supposedly-varied
    // captured background renders as one uniform block/color.
    //
    // Fix: eagerly call GetTexture here, at enqueue time, while TMEM still
    // holds THIS draw's own correct data. This decodes+caches under the same
    // key FlushBatch will look up later (same overrideCrc), so the later
    // lookup is a cache hit and never re-decodes from by-then-stale TMEM.
    uint8_t snapTlutType = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
    if (call.textureHash0 != 0) {
        Tex::TextureKey key0 = BuildTextureKey(call.tileDesc0, call.textureImageAddr, snapTlutType,
                                                call.mipLevelCount > 0 ? call.mipTiles : nullptr,
                                                call.mipLevelCount);
        call.snapshotCrc0 = key0.crc;
        if (m_textureCache && key0.width > 0 && key0.height > 0) {
            if (call.mipLevelCount > 0) {
                Tex::TextureCache::MipTileInfo mipInfo[7];
                for (int mi = 0; mi < call.mipLevelCount; mi++) {
                    const auto& mt = call.mipTiles[mi];
                    mipInfo[mi].tmemAddr = mt.tmemAddr;
                    mipInfo[mi].line     = mt.line;
                    mipInfo[mi].width    = static_cast<uint16_t>(std::max(1, mt.GetWidth()));
                    mipInfo[mi].height   = static_cast<uint16_t>(std::max(1, mt.GetHeight()));
                    mipInfo[mi].format   = mt.format;
                    mipInfo[mi].size     = mt.size;
                    mipInfo[mi].palette  = mt.palette;
                }
                m_textureCache->GetTexture(key0, m_rdram, m_state.rdp.tmem.data, mipInfo, call.mipLevelCount);
            } else {
                m_textureCache->GetTexture(key0, m_rdram, m_state.rdp.tmem.data);
            }
        }
    }
    if (call.textureHash1 != 0) {
        Tex::TextureKey key1 = BuildTextureKey(call.tileDesc1, call.textureImageAddr1, snapTlutType);
        call.snapshotCrc1 = key1.crc;
        if (m_textureCache && key1.width > 0 && key1.height > 0)
            m_textureCache->GetTexture(key1, m_rdram, m_state.rdp.tmem.data);
    }

    FORENSIC_TRACE("ENQUEUE type={} fmt={} size={} pal={} addr=0x{:06X} crc0=0x{:016X} crc1=0x{:016X}",
                   static_cast<int>(call.type),
                   static_cast<int>(call.tileDesc0.format),
                   static_cast<int>(call.tileDesc0.size),
                   static_cast<int>(call.tileDesc0.palette),
                   call.textureImageAddr,
                   call.snapshotCrc0,
                   call.snapshotCrc1);
}

Tex::TextureKey RDPProcessor::BuildTextureKey(const TileDescriptor& tile,
                                               uint32_t textureImageAddr,
                                               uint8_t tlutType,
                                               const TileDescriptor* mipTiles,
                                               int mipCount,
                                               uint64_t overrideCrc) const {
    Tex::TextureKey key{};
    key.address = textureImageAddr;
    key.palette = tile.palette;

    // Use mask/clamp-aware tile size instead of raw tile rectangle
    int calcW, calcH;
    // Supply this tile's TMEM load record so LoadTile-sourced textures use the
    // GLideN64 LOADTYPE_TILE sizing branch rather than mask/clamp derivation.
    const TileLoadInfo li = MakeTileLoadInfo(m_state.rdp, tile);
    int clampW = 0, clampH = 0;
    const int cycType = static_cast<int>((m_state.rdp.otherModeH >> 20) & 0x03);
    CalcTileSize(tile, tlutType, calcW, calcH, &li, cycType, &clampW, &clampH);
    key.width   = static_cast<uint16_t>(calcW);
    key.height  = static_cast<uint16_t>(calcH);
    // Only meaningful when strictly smaller than the sampled extent; otherwise
    // leave 0 so DecodeTexture skips the edge-replication pass entirely.
    key.clampWidth  = (clampW > 0 && clampW < calcW) ? static_cast<uint16_t>(clampW) : 0;
    key.clampHeight = (clampH > 0 && clampH < calcH) ? static_cast<uint16_t>(clampH) : 0;
    key.format  = static_cast<Tex::N64TexFormat>(tile.format);
    key.size    = static_cast<Tex::N64TexSize>(tile.size);
    key.cms     = tile.clampS;
    key.cmt     = tile.clampT;
    key.maskS   = tile.maskS;
    key.maskT   = tile.maskT;
    key.shiftS  = tile.shiftS;
    key.shiftT  = tile.shiftT;
    key.line    = tile.line;
    key.tmemOffset = tile.tmemAddr;  // TMEM base word address for this tile's data
    key.tlutType = tlutType;

    // Compute CRC over the relevant TMEM region for content-change detection.
    // The tile's TMEM byte address tells us where the data lives in TMEM.
    // When overrideCrc is supplied (non-zero), it was captured at draw-queue
    // time when TMEM had the correct content; skip the TMEM read.
    auto FNV1a_64 = [](const uint8_t* data, size_t length) -> uint64_t {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < length; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    if (overrideCrc != 0) {
        key.crc = overrideCrc;
    } else {

    uint32_t tmemByteAddr = tile.GetTMEMByteAddr();
    // When the render tile has line=0 (common for TextureRect item/sprite draws,
    // where stride is irrelevant to the hardware), GetBytesPerRow() returns 0 and
    // tmemSize would be 0 → crc=0 for all such tiles.  If multiple items share the
    // same SetTextureImage address (loaded via different tl offsets into a texture
    // table), they'd all hash to the same cache key and show the first item's texture.
    // Fix: when line=0, derive the row stride from width and bits-per-pixel so that
    // the CRC covers the actual texel data in TMEM.
    uint32_t bytesPerRow = static_cast<uint32_t>(tile.GetBytesPerRow());
    if (bytesPerRow == 0 && key.width > 0) {
        // bitsPerPixel = 4 << size  (4/8/16/32 for size 0/1/2/3)
        uint32_t bitsPerPixel = 4u << (static_cast<uint32_t>(tile.size) & 3u);
        bytesPerRow = (static_cast<uint32_t>(key.width) * bitsPerPixel + 7u) / 8u;
        bytesPerRow = (bytesPerRow + 7u) & ~7u;  // round up to 8-byte TMEM word
    }
    uint32_t tmemSize = bytesPerRow * static_cast<uint32_t>(key.height);
    // RGBA32: data is stored in two 2KB TMEM banks (RG in bank 0, BA in bank 1).
    // CRC each bank separately and combine, rather than doubling the range
    // (which can overshoot TMEM and produce crc=0 for all RGBA32 textures).
    bool isRGBA32 = (key.format == Tex::N64TexFormat::RGBA && key.size == Tex::N64TexSize::Bits32);
    // CI textures: index data lives in the lower 2KB of TMEM (upper 2KB is palette).
    // Also clamp whenever any TLUT mode is active (CI-as-IA palette mode etc.,
    // RT64 v3+ behavior) so the data CRC never bleeds into palette bytes.
    bool tlutActive = (key.format == Tex::N64TexFormat::CI) || (tlutType != 0);
    uint32_t tmemLimit = tlutActive ? TMEM_PALETTE_BASE : (isRGBA32 ? 2048u : TMEM_SIZE);
    if (tmemByteAddr < tmemLimit && tmemByteAddr + tmemSize > tmemLimit) {
        tmemSize = tmemLimit - tmemByteAddr;
    }
    if (tmemSize > 0 && tmemByteAddr + tmemSize <= TMEM_SIZE) {
        key.crc = FNV1a_64(m_state.rdp.tmem.data + tmemByteAddr, tmemSize);
        if (isRGBA32) {
            // CRC bank 1 (offset +2048) and mix into the hash
            uint32_t bank1Addr = tmemByteAddr + 2048;
            if (bank1Addr + tmemSize <= TMEM_SIZE) {
                uint64_t bank1Crc = FNV1a_64(m_state.rdp.tmem.data + bank1Addr, tmemSize);
                key.crc ^= bank1Crc * 0x9e3779b97f4a7c15ULL;
            }
        }
    } else {
        key.crc = 0;
    }

    // For CI (Color Indexed) textures or when TLUT mode is active, palette
    // lookup makes the decoded pixels depend on the TLUT contents in the upper
    // half of TMEM.  Mix the palette region CRC into the texture CRC so a
    // palette change triggers a cache miss.
    if (overrideCrc == 0 && (key.format == Tex::N64TexFormat::CI || tlutType != 0)) {
        uint64_t palCrc = 0;
        if (key.size == Tex::N64TexSize::Bits4) {
            // CI4: 16 entries starting at TMEM_PALETTE_BASE + palette*0x80, each 8 bytes apart
            uint32_t palBase = TMEM_PALETTE_BASE + static_cast<uint32_t>(tile.palette) * 0x80;
            if (palBase + 16 * 8 <= TMEM_SIZE)
                palCrc = FNV1a_64(m_state.rdp.tmem.data + palBase, 16 * 8);
        } else {
            // CI8: 256 entries starting at TMEM_PALETTE_BASE, each 8 bytes apart (full upper TMEM)
            if (TMEM_PALETTE_BASE + 256 * 8 <= TMEM_SIZE)
                palCrc = FNV1a_64(m_state.rdp.tmem.data + TMEM_PALETTE_BASE, 256 * 8);
        }
        key.crc ^= palCrc * 0x9e3779b97f4a7c15ULL;
    }
    } // end else (overrideCrc == 0)

    // TEMP DIAGNOSTIC (NOVA64_CRC_STALE_DBG=1): a batched draw's texture key uses
    // a CRC snapshotted at enqueue time (overrideCrc) instead of a fresh TMEM read,
    // to survive TMEM being overwritten by later draws before the batch flushes.
    // This recomputes what the CRC would be if read fresh from TMEM right now (at
    // flush time) and logs any mismatch -- proving whether the snapshot is stale
    // for a specific draw, without changing which CRC is actually used.
    if (overrideCrc != 0) {
        static const bool s_crcStaleDbg = std::getenv("NOVA64_CRC_STALE_DBG") != nullptr;
        if (s_crcStaleDbg && (key.format == Tex::N64TexFormat::CI || tlutType != 0)) {
            Tex::TextureKey freshKey = BuildTextureKey(tile, textureImageAddr, tlutType, mipTiles, mipCount, 0);
            if (freshKey.crc != overrideCrc) {
                std::fprintf(stderr,
                    "[STALE-CRC] addr=0x%06X fmt=%d size=%d w=%u h=%u pal=%u tlut=%u snapCrc=0x%016llX freshCrc=0x%016llX\n",
                    key.address & 0x7FFFFF, static_cast<int>(key.format), static_cast<int>(key.size),
                    key.width, key.height, key.palette, tlutType,
                    static_cast<unsigned long long>(overrideCrc),
                    static_cast<unsigned long long>(freshKey.crc));
            }
        }
    }

    if (TraceMMRdpEnabled()) {
        bool interestingTexture = key.format == Tex::N64TexFormat::CI ||
                                  tlutType != 0 ||
                                  g_fbManager.FindByAddress(key.address) != nullptr ||
                                  ((key.address & 0x7FFFFF) == (m_state.rdp.zbufferImage.address & 0x7FFFFF) && key.address != 0);
        if (interestingTexture) {
            MM_RDP_TRACE("BuildTextureKey addr=0x{:06X} fmt={} size={} w={} h={} pal={} tlut={} line={} tmem={} crc=0x{:016X} color=0x{:06X} z=0x{:06X}",
                         key.address & 0x7FFFFF,
                         static_cast<int>(key.format),
                         static_cast<int>(key.size),
                         key.width,
                         key.height,
                         key.palette,
                         key.tlutType,
                         key.line,
                         key.tmemOffset,
                         key.crc,
                         m_state.rdp.colorImage.address & 0x7FFFFF,
                         m_state.rdp.zbufferImage.address & 0x7FFFFF);
        }
    }

    // Incorporate mip tile chain TMEM data into the CRC for mipmapped textures
    key.mipLevels = 0;
    if (mipTiles && mipCount > 0) {
        key.mipLevels = static_cast<uint8_t>(mipCount);
        for (int i = 0; i < mipCount; i++) {
            uint32_t mipByteAddr = static_cast<uint32_t>(mipTiles[i].tmemAddr) * 8;
            uint32_t mipSize = static_cast<uint32_t>(mipTiles[i].GetBytesPerRow()) *
                               static_cast<uint32_t>(std::max(1, mipTiles[i].GetHeight()));
            if (mipSize > 0 && mipByteAddr + mipSize <= 4096) {
                uint64_t mipCrc = FNV1a_64(m_state.rdp.tmem.data + mipByteAddr, mipSize);
                key.crc ^= mipCrc * (0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(i + 1));
            }
        }
    }

    return key;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Batch Management
// ═══════════════════════════════════════════════════════════════════════════

} // namespace Nova64::Video