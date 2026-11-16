/**
 * @file FramebufferManager.cpp
 * @brief GPU/CPU framebuffer synchronization, VI output, and resolution scaling.
 */

#include "FramebufferManager.hpp"
#include <cstdio>
#include "../Common/Logger.hpp"
#include "../Renderer/Renderer.hpp"
#include "../../common/log.hpp"

#include <glad/gl.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

namespace Nova64::Video {

FramebufferManager g_fbManager;

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

FramebufferManager::~FramebufferManager() { Shutdown(); }

bool FramebufferManager::Init(uint32_t resolutionScale) {
    if (m_initialized) return true;

    m_resScale = std::max(1u, resolutionScale);
    Reset();

    if (m_isGL) {
        // Create fullscreen blit resources
        // Fullscreen triangle (covers NDC [-1,1])
        static const float blitVerts[] = {
            // pos (x,y)    texcoord (u,v)
            -1.0f, -1.0f, 0.0f, 0.0f, 3.0f, -1.0f, 2.0f, 0.0f, -1.0f, 3.0f, 0.0f, 2.0f,
        };

        glCreateVertexArrays(1, &m_blitVAO);
        glCreateBuffers(1, &m_blitVBO);
        glNamedBufferStorage(m_blitVBO, sizeof(blitVerts), blitVerts, 0);

        glVertexArrayVertexBuffer(m_blitVAO, 0, m_blitVBO, 0, 4 * sizeof(float));
        glEnableVertexArrayAttrib(m_blitVAO, 0);
        glEnableVertexArrayAttrib(m_blitVAO, 1);
        glVertexArrayAttribFormat(m_blitVAO, 0, 2, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribFormat(m_blitVAO, 1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float));
        glVertexArrayAttribBinding(m_blitVAO, 0, 0);
        glVertexArrayAttribBinding(m_blitVAO, 1, 0);

        // Blit shader program
        const char* blitVS = R"(
        #version 460 core
        layout(location=0) in vec2 aPos;
        layout(location=1) in vec2 aTexCoord;
        out vec2 vTexCoord;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            vTexCoord = aTexCoord;
        }
    )";
        const char* blitFS = R"(
        #version 460 core
        in vec2 vTexCoord;
        out vec4 FragColor;
        uniform sampler2D uTexture;
        void main() {
            FragColor = vec4(texture(uTexture, vTexCoord).rgb, 1.0);
        }
    )";

        auto compileShader = [](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetShaderInfoLog(s, 512, nullptr, log);
                NOVA_LOG_ERROR("Blit shader compile: {}", log);
            }
            return s;
        };

        GLuint vs = compileShader(GL_VERTEX_SHADER, blitVS);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, blitFS);
        m_blitProgram = glCreateProgram();
        glAttachShader(m_blitProgram, vs);
        glAttachShader(m_blitProgram, fs);
        glLinkProgram(m_blitProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);

        // VI filter shader (AA + divot + dither removal + interlace)
        const char* viFilterFS = R"(
        #version 460 core
        in vec2 vTexCoord;
        out vec4 FragColor;
        uniform sampler2D uTexture;
        uniform vec2 uPixelSize;
        uniform int uEnableAA;
        uniform int uEnableDivot;
        uniform int uEnableDedither;
        uniform int uInterlaced;
        uniform int uField;

        // Dither removal: 2×2 box filter cancels the 4×4 ordered dithering pattern.
        // The N64 VI applies this before AA; it averages a 2×2 block of N64 pixels
        // so the alternating +/- dither offsets within each block cancel out.
        vec4 sampleDedither(vec2 uv) {
            if (uEnableDedither == 0) return texture(uTexture, uv);
            vec4 tl = texture(uTexture, uv + vec2(-0.5, -0.5) * uPixelSize);
            vec4 tr = texture(uTexture, uv + vec2( 0.5, -0.5) * uPixelSize);
            vec4 bl = texture(uTexture, uv + vec2(-0.5,  0.5) * uPixelSize);
            vec4 br = texture(uTexture, uv + vec2( 0.5,  0.5) * uPixelSize);
            return (tl + tr + bl + br) * 0.25;
        }

        vec4 sampleAA(vec2 uv) {
            vec4 c = sampleDedither(uv);
            if (uEnableAA == 0) return c;

            // N64 VI AA: 5-tap diamond (plus) pattern, no diagonals.
            // Matches the hardware's coverage-weighted filter shape.
            // Diamond weights: center=2, cardinals=1 each, total=6.
            vec4 up    = sampleDedither(uv + vec2(0.0, -uPixelSize.y));
            vec4 down  = sampleDedither(uv + vec2(0.0,  uPixelSize.y));
            vec4 left  = sampleDedither(uv + vec2(-uPixelSize.x, 0.0));
            vec4 right = sampleDedither(uv + vec2( uPixelSize.x, 0.0));
            return (c * 2.0 + up + down + left + right) / 6.0;
        }

        void main() {
            vec4 color = sampleAA(vTexCoord);

            // Divot filter: median of 3 horizontal samples
            if (uEnableDivot != 0) {
                vec4 left  = sampleAA(vTexCoord + vec2(-uPixelSize.x, 0.0));
                vec4 right = sampleAA(vTexCoord + vec2( uPixelSize.x, 0.0));
                // Per-channel median
                color.r = clamp(color.r, min(left.r, right.r), max(left.r, right.r));
                color.g = clamp(color.g, min(left.g, right.g), max(left.g, right.g));
                color.b = clamp(color.b, min(left.b, right.b), max(left.b, right.b));
            }

            // Interlace simulation: dim scanlines belonging to the "off" field.
            if (uInterlaced != 0) {
                int scanline = int(gl_FragCoord.y);
                if ((scanline & 1) != uField) {
                    vec4 neighbor = texture(uTexture, vTexCoord + vec2(0.0, -uPixelSize.y));
                    color = mix(color, neighbor, 0.5);
                }
            }

            FragColor = vec4(color.rgb, 1.0);
        }
    )";

        GLuint viFS = compileShader(GL_FRAGMENT_SHADER, viFilterFS);
        m_viFilterProgram = glCreateProgram();
        vs = compileShader(GL_VERTEX_SHADER, blitVS);
        glAttachShader(m_viFilterProgram, vs);
        glAttachShader(m_viFilterProgram, viFS);
        glLinkProgram(m_viFilterProgram);
        glDeleteShader(vs);
        glDeleteShader(viFS);

        // Gamma correction shader
        const char* gammaFS = R"(
        #version 460 core
        in vec2 vTexCoord;
        out vec4 FragColor;
        uniform sampler2D uTexture;
        uniform float uGamma;
        void main() {
            vec4 c = texture(uTexture, vTexCoord);
            float invGamma = 1.0 / uGamma;
            FragColor = vec4(pow(c.rgb, vec3(invGamma)), 1.0);
        }
    )";

        vs = compileShader(GL_VERTEX_SHADER, blitVS);
        GLuint gFS = compileShader(GL_FRAGMENT_SHADER, gammaFS);
        m_gammaProgram = glCreateProgram();
        glAttachShader(m_gammaProgram, vs);
        glAttachShader(m_gammaProgram, gFS);
        glLinkProgram(m_gammaProgram);
        glDeleteShader(vs);
        glDeleteShader(gFS);
    }

    m_initialized = true;
    NOVA_LOG_INFO("FramebufferManager initialized (scale={}x)", m_resScale);
    return true;
}

void FramebufferManager::Shutdown() {
    if (!m_initialized) return;

    for (uint32_t i = 0; i < m_fbCount; ++i) {
        DestroyGPUFramebuffer(m_framebuffers[i]);
    }
    m_fbCount = 0;

    if (m_isGL) {
        if (m_pboFence) {
            glDeleteSync(static_cast<GLsync>(m_pboFence));
            m_pboFence = nullptr;
        }
        if (m_pbo) {
            glDeleteBuffers(1, &m_pbo);
            m_pbo = 0;
        }
        if (m_readbackFBO) {
            glDeleteFramebuffers(1, &m_readbackFBO);
            m_readbackFBO = 0;
        }
        if (m_readbackTex) {
            glDeleteTextures(1, &m_readbackTex);
            m_readbackTex = 0;
        }
        m_readbackW = 0;
        m_readbackH = 0;
        if (m_blitProgram) {
            glDeleteProgram(m_blitProgram);
            m_blitProgram = 0;
        }
        if (m_viFilterProgram) {
            glDeleteProgram(m_viFilterProgram);
            m_viFilterProgram = 0;
        }
        if (m_gammaProgram) {
            glDeleteProgram(m_gammaProgram);
            m_gammaProgram = 0;
        }
        if (m_tmpFBO) {
            glDeleteFramebuffers(1, &m_tmpFBO);
            m_tmpFBO = 0;
        }
        if (m_tmpTex) {
            glDeleteTextures(1, &m_tmpTex);
            m_tmpTex = 0;
        }
        m_tmpWidth = 0;
        m_tmpHeight = 0;
        if (m_copyColorFBO) {
            glDeleteFramebuffers(1, &m_copyColorFBO);
            m_copyColorFBO = 0;
        }
        if (m_copyColorTex) {
            glDeleteTextures(1, &m_copyColorTex);
            m_copyColorTex = 0;
        }
        m_copyColorW = 0;
        m_copyColorH = 0;
        if (m_copyDepthFBO) {
            glDeleteFramebuffers(1, &m_copyDepthFBO);
            m_copyDepthFBO = 0;
        }
        if (m_copyDepthTex) {
            glDeleteTextures(1, &m_copyDepthTex);
            m_copyDepthTex = 0;
        }
        m_copyDepthW = 0;
        m_copyDepthH = 0;
        m_pboSize = 0;
        if (m_blitVAO) {
            glDeleteVertexArrays(1, &m_blitVAO);
            m_blitVAO = 0;
        }
        if (m_blitVBO) {
            glDeleteBuffers(1, &m_blitVBO);
            m_blitVBO = 0;
        }
    }

    m_currentColor = nullptr;
    m_currentDepth = nullptr;
    m_initialized = false;
}

void FramebufferManager::Reset() {
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        DestroyGPUFramebuffer(m_framebuffers[i]);
    }
    m_fbCount = 0;
    m_currentColor = nullptr;
    m_currentDepth = nullptr;
    m_viConfig = {};
    m_frameCounter = 0;
    m_dirtyPages.fill(false);
    m_stats = {};
}

// ═══════════════════════════════════════════════════════════════
//  GPU Framebuffer Create / Destroy
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::CreateGPUFramebuffer(GPUFramebuffer& fb) {
    fb.scaledWidth = fb.width * m_resScale;
    fb.scaledHeight = fb.height * m_resScale;
    fb.msaaSamples = m_msaaSamples;
    if (!m_isGL) return;

    {
        static int gpuIsIntel = -1;
        if (gpuIsIntel < 0) {
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            gpuIsIntel = (renderer && strstr(renderer, "Intel")) ? 1 : 0;
        }
        if (gpuIsIntel) glFlush();
    }

    for (int i = 0; i < 8; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);

    glCreateFramebuffers(1, &fb.fbo);

    if (m_msaaSamples > 1) {
        // MSAA path: create multisample textures
        GLsizei samples = static_cast<GLsizei>(m_msaaSamples);

        glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &fb.colorTex);
        glTextureStorage2DMultisample(fb.colorTex, samples, GL_RGBA8, fb.scaledWidth, fb.scaledHeight, GL_TRUE);
        glNamedFramebufferTexture(fb.fbo, GL_COLOR_ATTACHMENT0, fb.colorTex, 0);

        glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &fb.depthTex);
        glTextureStorage2DMultisample(fb.depthTex, samples, GL_DEPTH32F_STENCIL8, fb.scaledWidth, fb.scaledHeight,
                                      GL_TRUE);
        glNamedFramebufferTexture(fb.fbo, GL_DEPTH_STENCIL_ATTACHMENT, fb.depthTex, 0);

        // Create a non-MSAA resolved texture + FBO for FB-as-texture sampling
        glCreateTextures(GL_TEXTURE_2D, 1, &fb.resolvedTex);
        glTextureStorage2D(fb.resolvedTex, 1, GL_RGBA8, fb.scaledWidth, fb.scaledHeight);
        glTextureParameteri(fb.resolvedTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(fb.resolvedTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(fb.resolvedTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(fb.resolvedTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &fb.resolvedFbo);
        glNamedFramebufferTexture(fb.resolvedFbo, GL_COLOR_ATTACHMENT0, fb.resolvedTex, 0);
    } else {
        // Standard non-MSAA path
        glCreateTextures(GL_TEXTURE_2D, 1, &fb.colorTex);
        glTextureStorage2D(fb.colorTex, 1, GL_RGBA8, fb.scaledWidth, fb.scaledHeight);
        glTextureParameteri(fb.colorTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(fb.colorTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(fb.colorTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(fb.colorTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glNamedFramebufferTexture(fb.fbo, GL_COLOR_ATTACHMENT0, fb.colorTex, 0);

        glCreateTextures(GL_TEXTURE_2D, 1, &fb.depthTex);
        glTextureStorage2D(fb.depthTex, 1, GL_DEPTH32F_STENCIL8, fb.scaledWidth, fb.scaledHeight);
        glTextureParameteri(fb.depthTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(fb.depthTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glNamedFramebufferTexture(fb.fbo, GL_DEPTH_STENCIL_ATTACHMENT, fb.depthTex, 0);
    }

    glCreateTextures(GL_TEXTURE_2D, 1, &fb.coverageTex);
    glTextureStorage2D(fb.coverageTex, 1, GL_R8UI, fb.scaledWidth, fb.scaledHeight);
    glTextureParameteri(fb.coverageTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(fb.coverageTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Clear coverage to 0 (no coverage before any geometry)
    const GLuint zeroCvg = 0;
    glClearTexImage(fb.coverageTex, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, &zeroCvg);

    fb.colorImageHandle = static_cast<uint64_t>(fb.colorTex);
    fb.coverageImageHandle = static_cast<uint64_t>(fb.coverageTex);

    GLenum status = glCheckNamedFramebufferStatus(fb.fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        NOVA_LOG_ERROR("FBO incomplete (status=0x{:X}) for addr 0x{:08X} {}x{}", status, fb.desc.address,
                       fb.scaledWidth, fb.scaledHeight);
    } else {

        const GLfloat depthOne = 1.0f;
        const GLint stencilZero = 0;
        glClearNamedFramebufferfi(fb.fbo, GL_DEPTH_STENCIL, 0, depthOne, stencilZero);

        const GLfloat blackZeroAlpha[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glClearNamedFramebufferfv(fb.fbo, GL_COLOR, 0, blackZeroAlpha);
    }

    m_stats.framebuffersCreated++;
}

void FramebufferManager::DestroyGPUFramebuffer(GPUFramebuffer& fb) {

    if (m_pboFB == &fb) m_pboFB = nullptr;
    if (!m_isGL) return;
    if (fb.resolvedFbo) {
        glDeleteFramebuffers(1, &fb.resolvedFbo);
        fb.resolvedFbo = 0;
    }
    if (fb.resolvedTex) {
        glDeleteTextures(1, &fb.resolvedTex);
        fb.resolvedTex = 0;
    }
    if (fb.snapshotTex) {
        glDeleteTextures(1, &fb.snapshotTex);
        fb.snapshotTex = 0;
    }
    if (fb.snapshotCvgTex) {
        glDeleteTextures(1, &fb.snapshotCvgTex);
        fb.snapshotCvgTex = 0;
    }
    if (fb.presentationTex) {
        glDeleteTextures(1, &fb.presentationTex);
        fb.presentationTex = 0;
    }
    if (fb.coverageTex) {
        glDeleteTextures(1, &fb.coverageTex);
        fb.coverageTex = 0;
    }
    if (fb.fbo) {
        glDeleteFramebuffers(1, &fb.fbo);
        fb.fbo = 0;
    }
    if (fb.colorTex) {
        glDeleteTextures(1, &fb.colorTex);
        fb.colorTex = 0;
    }
    if (fb.depthTex) {
        glDeleteTextures(1, &fb.depthTex);
        fb.depthTex = 0;
    }
}

// ═══════════════════════════════════════════════════════════════
//  Sampleable Texture Resolution
// ═══════════════════════════════════════════════════════════════

uint32_t FramebufferManager::GetSampleableTexture(GPUFramebuffer* fb, bool isSelfTexture) {
    if (!m_isGL || !fb || fb->colorTex == 0) return 0;

    if (fb->msaaSamples > 1) {
        if (fb->resolvedFbo != 0) {
            glBlitNamedFramebuffer(fb->fbo, fb->resolvedFbo, 0, 0, (GLint)fb->scaledWidth, (GLint)fb->scaledHeight, 0,
                                   0, (GLint)fb->scaledWidth, (GLint)fb->scaledHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            return fb->resolvedTex;
        }
        return 0;
    }

    if (isSelfTexture) {

        if (fb->snapshotTex == 0) {
            glCreateTextures(GL_TEXTURE_2D, 1, &fb->snapshotTex);
            glTextureStorage2D(fb->snapshotTex, 1, GL_RGBA8, fb->scaledWidth, fb->scaledHeight);
            glTextureParameteri(fb->snapshotTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(fb->snapshotTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTextureParameteri(fb->snapshotTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(fb->snapshotTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glCopyImageSubData(fb->colorTex, GL_TEXTURE_2D, 0, 0, 0, 0, fb->snapshotTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           (GLsizei)fb->scaledWidth, (GLsizei)fb->scaledHeight, 1);

        // Also snapshot coverage for MLE blender reads
        if (fb->coverageTex != 0) {
            if (fb->snapshotCvgTex == 0) {
                glCreateTextures(GL_TEXTURE_2D, 1, &fb->snapshotCvgTex);
                glTextureStorage2D(fb->snapshotCvgTex, 1, GL_R8UI, fb->scaledWidth, fb->scaledHeight);
                glTextureParameteri(fb->snapshotCvgTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(fb->snapshotCvgTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
            glCopyImageSubData(fb->coverageTex, GL_TEXTURE_2D, 0, 0, 0, 0, fb->snapshotCvgTex, GL_TEXTURE_2D, 0, 0, 0,
                               0, (GLsizei)fb->scaledWidth, (GLsizei)fb->scaledHeight, 1);
        }
        return fb->snapshotTex;
    }

    return fb->colorTex;
}

void FramebufferManager::SnapshotForPresentation() {
    if (!m_isGL) return;
    if (!m_currentColor || m_currentColor->fbo == 0 || m_currentColor->colorTex == 0) return;

    uint32_t w = m_currentColor->scaledWidth;
    uint32_t h = m_currentColor->scaledHeight;
    if (w == 0 || h == 0) return;

    // (Re)create the per-FBO presentation texture when dimensions change
    if (m_currentColor->presentationTex == 0) {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_currentColor->presentationTex);
        glTextureStorage2D(m_currentColor->presentationTex, 1, GL_RGBA8, w, h);
        glTextureParameteri(m_currentColor->presentationTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_currentColor->presentationTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_currentColor->presentationTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_currentColor->presentationTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // GPU-side copy — no CPU round-trip, no pipeline stall
    glCopyImageSubData(m_currentColor->colorTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_currentColor->presentationTex,
                       GL_TEXTURE_2D, 0, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1);
}

// ═══════════════════════════════════════════════════════════════
//  Framebuffer Tracking
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::OnSetColorImage(uint32_t address, uint32_t width, [[maybe_unused]] uint32_t format,
                                         uint32_t size) {
    FramebufferDesc desc;
    desc.address = address & 0x7FFFFF; // Mask to RDRAM range
    desc.width = width;

    uint32_t height = 240;
    if (m_scissorHeight > 0) {
        height = m_scissorHeight;
    } else {
        // Look up prior height for this address
        for (uint32_t i = 0; i < m_fbCount; ++i) {
            if (m_framebuffers[i].desc.address == desc.address && m_framebuffers[i].desc.type == FBType::Color) {
                height = m_framebuffers[i].desc.height;
                break;
            }
        }
    }
    desc.height = height;
    // `size` is the RAW RDP size ENCODING (0=4bpp,1=8bpp,2=16bpp,3=32bpp,
    // see ImageSize in N64Types.hpp), not bytes-per-pixel -- the caller
    // (RDPProcessor_State.cpp's HandleSetColorImage) passes
    // static_cast<uint32_t>(colorImage.size) straight through. Comparing it
    // to 4 to detect 32-bit color images was always false (the real 32-bit
    // encoding is 3), so every color image was treated as RGBA16 regardless
    // of its actual format, and `stride = width * size` used the encoding
    // (3) instead of real bytes-per-pixel (4) for genuine 32-bit
    // framebuffers -- understriding them by 25% (wrong ByteSize/EndAddress,
    // wrong overlap detection, wrong readback bounds). Convert to real
    // bytes-per-pixel first. RGBA16 games (the overwhelming majority) were
    // unaffected since encoding 2 happens to equal 2 bytes.
    uint32_t bytesPerPixel = static_cast<uint32_t>(GetBitsPerPixel(static_cast<ImageSize>(size & 3))) / 8;
    if (bytesPerPixel == 0) bytesPerPixel = 2; // 4bpp isn't a valid color-image size; default safely
    desc.format = (bytesPerPixel == 4) ? FBFormat::RGBA32 : FBFormat::RGBA16;
    desc.size = bytesPerPixel;
    desc.stride = width * bytesPerPixel;
    desc.type = FBType::Color;

    GPUFramebuffer* fb = FindOrCreate(desc);
    if (fb) {
        m_currentColor = fb;
        fb->lastUsedFrame = m_frameCounter;
        if (m_isGL) {
            glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
            glViewport(0, 0, fb->scaledWidth, fb->scaledHeight);
            glScissor(0, 0, fb->scaledWidth, fb->scaledHeight);
        }

        const uint32_t viOrigin = m_viConfig.origin & 0x7FFFFF;
        if (fb->lastColorClearedFrame < m_frameCounter && !fb->gpuModified && fb->fbo != 0 &&
            fb->desc.address != viOrigin) {
            if (m_isGL) {
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_SCISSOR_TEST);
            }
            fb->lastColorClearedFrame = m_frameCounter;
        }

        if (m_depthClearPending) {
            if (m_isGL && fb->fbo != 0) glClearNamedFramebufferfi(fb->fbo, GL_DEPTH_STENCIL, 0, 1.0f, 0);
            m_depthClearPending = false;
        }

        if (m_isGL && fb->coverageTex) {
            const GLuint zero = 0;
            glClearTexImage(fb->coverageTex, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, &zero);
        }
    }
}

void FramebufferManager::OnSetDepthImage(uint32_t address) {
    FramebufferDesc desc;
    desc.address = address & 0x7FFFFF;
    desc.width = m_currentColor ? m_currentColor->width : 320;
    desc.height = m_currentColor ? m_currentColor->height : 240;
    desc.format = FBFormat::RGBA16; // N64 depth is 16-bit
    desc.size = 2;
    desc.stride = desc.width * 2;
    desc.type = FBType::Depth;

    GPUFramebuffer* fb = FindOrCreate(desc);
    if (fb) {
        m_currentDepth = fb;
        fb->lastUsedFrame = m_frameCounter;
    }

    m_depthClearPending = true;

    if (m_isGL && m_currentColor && m_currentColor->fbo != 0) {
        glClearNamedFramebufferfi(m_currentColor->fbo, GL_DEPTH_STENCIL, 0, 1.0f, 0);
    }
}

GPUFramebuffer* FramebufferManager::FindOrCreate(const FramebufferDesc& desc) {
    // Search existing — exact match first
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        if (m_framebuffers[i].desc == desc) {
            m_stats.framebuffersReused++;
            return &m_framebuffers[i];
        }
    }

    for (uint32_t i = 0; i < m_fbCount; ++i) {
        auto& fb = m_framebuffers[i];
        if (fb.desc.address == desc.address && fb.desc.type == desc.type && fb.desc.width == desc.width &&
            fb.desc.format == desc.format && fb.desc.height >= desc.height) {
            m_stats.framebuffersReused++;
            return &fb;
        }
    }

    // Search by overlapping address (reuse slot with updated desc)
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        if (m_framebuffers[i].desc.address == desc.address && m_framebuffers[i].desc.type == desc.type) {
            // Same address, different format/size — recreate.
            // Migrate the presentation snapshot so it survives the rebuild.
            // Migrate the presentation and snapshot textures so they survive the rebuild.
            GLuint savedPresentation = m_framebuffers[i].presentationTex;
            GLuint savedSnapshot = m_framebuffers[i].snapshotTex;
            m_framebuffers[i].presentationTex = 0; // prevent DestroyGPU from deleting it
            m_framebuffers[i].snapshotTex = 0;
            uint32_t oldScaledW = m_framebuffers[i].scaledWidth;
            uint32_t oldScaledH = m_framebuffers[i].scaledHeight;
            DestroyGPUFramebuffer(m_framebuffers[i]);
            m_framebuffers[i].desc = desc;
            m_framebuffers[i].width = desc.width;
            m_framebuffers[i].height = desc.height;
            CreateGPUFramebuffer(m_framebuffers[i]);
            // Only reuse saved textures if dimensions haven't changed;
            // otherwise delete the stale ones to avoid size mismatch in
            // glCopyImageSubData during SnapshotForPresentation.
            if (m_framebuffers[i].scaledWidth == oldScaledW && m_framebuffers[i].scaledHeight == oldScaledH) {
                m_framebuffers[i].presentationTex = savedPresentation;
                m_framebuffers[i].snapshotTex = savedSnapshot;
            } else {
                if (savedPresentation) glDeleteTextures(1, &savedPresentation);
                if (savedSnapshot) glDeleteTextures(1, &savedSnapshot);
            }
            return &m_framebuffers[i];
        }
    }

    // Create new
    if (m_fbCount >= kMaxFramebuffers) {
        EvictOldFramebuffers(m_frameCounter);
        if (m_fbCount >= kMaxFramebuffers) {
            NOVA_LOG_ERROR("Max framebuffers reached ({})", kMaxFramebuffers);
            return nullptr;
        }
    }

    auto& fb = m_framebuffers[m_fbCount++];
    fb.desc = desc;
    fb.width = desc.width;
    fb.height = desc.height;
    fb.lastUsedFrame = m_frameCounter;
    CreateGPUFramebuffer(fb);

    NOVA_LOG_DEBUG("Created FB addr=0x{:08X} {}x{} fmt={}", desc.address, desc.width, desc.height,
                   static_cast<int>(desc.format));
    return &fb;
}

GPUFramebuffer* FramebufferManager::FindByAddress(uint32_t address, FBType preferType) {
    address &= 0x7FFFFF;
    GPUFramebuffer* firstHit = nullptr;
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        if (m_framebuffers[i].desc.address == address) {
            if (m_framebuffers[i].desc.type == preferType) return &m_framebuffers[i];
            if (!firstHit) firstHit = &m_framebuffers[i];
        }
    }
    if (firstHit) return firstHit;
    GPUFramebuffer* bestOfType = nullptr;
    GPUFramebuffer* bestAny = nullptr;
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        auto& fb = m_framebuffers[i];
        if (address > fb.desc.address && address < fb.desc.EndAddress()) {
            uint32_t offset = address - fb.desc.address;
            if (fb.desc.stride > 0 && (offset % fb.desc.stride) == 0) {
                if (!bestAny || fb.lastUsedFrame > bestAny->lastUsedFrame) bestAny = &fb;
                if (fb.desc.type == preferType && (!bestOfType || fb.lastUsedFrame > bestOfType->lastUsedFrame))
                    bestOfType = &fb;
            }
        }
    }
    return bestOfType ? bestOfType : bestAny;
}

// Sampled FNV-1a over the buffer's RDRAM bytes: at most kMaxSamples words spread
// evenly across the footprint, so a 150 KB frame costs about as much as a 4 KB
// texture. Reused memory (a DMA'd texture, a decompressed object) changes most
// words, so a sparse sample is enough to notice it.
static uint64_t FootprintFingerprint(const FramebufferDesc& d, const uint8_t* rdram, size_t rdramSize) {
    constexpr uint32_t kMaxSamples = 1024;
    const uint64_t begin = d.address & ~3u;
    const uint64_t end = std::min<uint64_t>(d.EndAddress(), rdramSize);
    if (end <= begin + 3) return 0;
    const uint64_t words = (end - begin) / 4;
    const uint64_t step = std::max<uint64_t>(1, words / kMaxSamples);
    uint64_t hash = 1469598103934665603ull;
    for (uint64_t w = 0; w < words; w += step) {
        uint32_t v;
        std::memcpy(&v, rdram + begin + w * 4, sizeof(v));
        hash = (hash ^ v) * 1099511628211ull;
    }
    return hash;
}

void FramebufferManager::RecordRdramFingerprint(GPUFramebuffer* fb, const uint8_t* rdram, size_t rdramSize) {
    if (!fb || !rdram) return;
    fb->rdramFingerprint = FootprintFingerprint(fb->desc, rdram, rdramSize);
    fb->hasFingerprint = true;
}

bool FramebufferManager::RdramUnchanged(const GPUFramebuffer* fb, const uint8_t* rdram, size_t rdramSize) const {
    if (!fb || !rdram || !fb->hasFingerprint || fb == m_currentColor) return true;
    return FootprintFingerprint(fb->desc, rdram, rdramSize) == fb->rdramFingerprint;
}

GPUFramebuffer* FramebufferManager::FindContaining(uint32_t address) {
    address &= 0x7FFFFF;
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        auto& fb = m_framebuffers[i];
        if (address >= fb.desc.address && address < fb.desc.EndAddress()) {
            return &m_framebuffers[i];
        }
    }
    return nullptr;
}

void FramebufferManager::EvictOldFramebuffers(uint64_t currentFrame) {
    static constexpr uint64_t kStaleFrameThreshold = 600;
    auto isStale = [&](const GPUFramebuffer& fb) { return currentFrame - fb.lastUsedFrame >= kStaleFrameThreshold; };
    auto hasFreshSibling = [&](const GPUFramebuffer& fb) {
        for (uint32_t j = 0; j < m_fbCount; ++j) {
            const auto& other = m_framebuffers[j];
            if (&other != &fb && other.desc.address == fb.desc.address && !isStale(other)) return true;
        }
        return false;
    };
    uint32_t writeIdx = 0;
    for (uint32_t i = 0; i < m_fbCount; ++i) {
        if (!isStale(m_framebuffers[i]) || hasFreshSibling(m_framebuffers[i])) {
            if (writeIdx != i) {
                m_framebuffers[writeIdx] = m_framebuffers[i];
            }
            writeIdx++;
        } else {
            DestroyGPUFramebuffer(m_framebuffers[i]);
        }
    }
    m_fbCount = writeIdx;
}

// ═══════════════════════════════════════════════════════════════
//  GPU ↔ CPU Sync
// ═══════════════════════════════════════════════════════════════

bool FramebufferManager::CopyToGPUVulkan(GPUFramebuffer* fb, const uint8_t* rdram, IRenderer* renderer) {
    if (!fb || !rdram || !renderer || m_isGL) return false;

    const uint32_t addr = fb->desc.address;
    const uint32_t w = fb->width;
    const uint32_t h = fb->height;
    if (w == 0 || h == 0) return false;
    const uint32_t pixelCount = w * h;

    // Guard the RDRAM read: desc dimensions come from game state, so a bogus
    // width/height must not walk off the end of the 8 MB map.
    const uint32_t bytesPerPixel = (fb->desc.format == FBFormat::RGBA32) ? 4u : 2u;
    if (static_cast<uint64_t>(addr) + static_cast<uint64_t>(pixelCount) * bytesPerPixel > 0x800000ull) return false;

    std::vector<uint32_t> rgba8(pixelCount);
    if (fb->desc.format == FBFormat::RGBA16) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(rdram + addr);
        ConvertRGBA16ToRGBA8(src, reinterpret_cast<uint8_t*>(rgba8.data()), pixelCount);
    } else if (fb->desc.format == FBFormat::RGBA32) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(rdram + addr);
        ConvertRGBA32ToRGBA8(src, reinterpret_cast<uint8_t*>(rgba8.data()), pixelCount);
    } else {
        return false;
    }

    return renderer->UploadPixelsToCanvas(addr, w, h, rgba8.data());
}

void FramebufferManager::CopyToGPU(GPUFramebuffer* fb, const uint8_t* rdram) {
    if (!fb || !rdram || !m_isGL) return;

    const uint32_t addr = fb->desc.address;
    const uint32_t w = fb->width;
    const uint32_t h = fb->height;
    const uint32_t pixelCount = w * h;

    std::vector<uint8_t> rgba8(pixelCount * 4);

    if (fb->desc.format == FBFormat::RGBA16) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(rdram + addr);
        ConvertRGBA16ToRGBA8(src, rgba8.data(), pixelCount);
    } else if (fb->desc.format == FBFormat::RGBA32) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(rdram + addr);
        ConvertRGBA32ToRGBA8(src, rgba8.data(), pixelCount);
    }

    // Upload to GPU texture (possibly at higher resolution — tile if needed)
    if (m_resScale == 1) {
        glTextureSubImage2D(fb->colorTex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba8.data());
    } else {
        // Reuse cached temp texture/FBO at native res (avoid per-frame GPU alloc on Intel)
        if (m_copyColorW != w || m_copyColorH != h) {
            if (m_copyColorFBO) {
                glDeleteFramebuffers(1, &m_copyColorFBO);
                m_copyColorFBO = 0;
            }
            if (m_copyColorTex) {
                glDeleteTextures(1, &m_copyColorTex);
                m_copyColorTex = 0;
            }
        }
        if (!m_copyColorFBO) {
            glCreateTextures(GL_TEXTURE_2D, 1, &m_copyColorTex);
            glTextureStorage2D(m_copyColorTex, 1, GL_RGBA8, w, h);
            glCreateFramebuffers(1, &m_copyColorFBO);
            glNamedFramebufferTexture(m_copyColorFBO, GL_COLOR_ATTACHMENT0, m_copyColorTex, 0);
            m_copyColorW = w;
            m_copyColorH = h;
        }

        glTextureSubImage2D(m_copyColorTex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba8.data());
        glBlitNamedFramebuffer(m_copyColorFBO, fb->fbo, 0, 0, w, h, 0, 0, fb->scaledWidth, fb->scaledHeight,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    ClearRDRAMDirty(addr, fb->desc.ByteSize());
    m_stats.rdramToGpuCopies++;
    m_stats.bytesTransferred += pixelCount * 4;
}

void FramebufferManager::CopyFromGPU(GPUFramebuffer* fb, uint8_t* rdram) {
    if (!fb || !rdram || !m_isGL) return;

    const uint32_t w = fb->width;
    const uint32_t h = fb->height;
    const uint32_t pixelCount = w * h;

    std::vector<uint8_t> rgba8(pixelCount * 4);

    if (m_resScale == 1 && fb->msaaSamples <= 1) {
        // Non-MSAA, native resolution: read directly (no VI filter —
        // VI filter is display-only and must not be baked into RDRAM data).
        glGetTextureImage(fb->colorTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(rgba8.size()), rgba8.data());
    } else {
        // MSAA resolve + optional downscale via blit — reuse cached temp FBO/tex
        if (m_copyColorW != w || m_copyColorH != h) {
            if (m_copyColorFBO) {
                glDeleteFramebuffers(1, &m_copyColorFBO);
                m_copyColorFBO = 0;
            }
            if (m_copyColorTex) {
                glDeleteTextures(1, &m_copyColorTex);
                m_copyColorTex = 0;
            }
        }
        if (!m_copyColorFBO) {
            glCreateTextures(GL_TEXTURE_2D, 1, &m_copyColorTex);
            glTextureStorage2D(m_copyColorTex, 1, GL_RGBA8, w, h);
            glCreateFramebuffers(1, &m_copyColorFBO);
            glNamedFramebufferTexture(m_copyColorFBO, GL_COLOR_ATTACHMENT0, m_copyColorTex, 0);
            m_copyColorW = w;
            m_copyColorH = h;
        }

        glBlitNamedFramebuffer(fb->fbo, m_copyColorFBO, 0, 0, fb->scaledWidth, fb->scaledHeight, 0, 0, w, h,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
        // Read resolved pixels directly — no VI filter (display-only effect).
        glGetTextureImage(m_copyColorTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(rgba8.size()),
                          rgba8.data());
    }

    // Convert back to N64 format
    uint32_t addr = fb->desc.address;
    if (fb->desc.format == FBFormat::RGBA16) {
        // Paper Mario subscreen hack: boost blue channel by 4 before conversion
        if (m_fbHacks.paperMarioBlueBoost) {
            for (uint32_t i = 0; i < pixelCount; ++i) {
                uint8_t& b = rgba8[i * 4 + 2];
                if (b >= 0x01 && b <= 0xFB) b += 4;
            }
        }
        uint16_t* dst = reinterpret_cast<uint16_t*>(rdram + addr);
        ConvertRGBA8ToRGBA16(rgba8.data(), dst, pixelCount);
    } else {
        uint32_t* dst = reinterpret_cast<uint32_t*>(rdram + addr);
        ConvertRGBA8ToRGBA32(rgba8.data(), dst, pixelCount);
    }

    MarkRDRAMDirty(addr, fb->desc.ByteSize());
    fb->gpuModified = false;
    m_stats.gpuToRdramCopies++;
    m_stats.bytesTransferred += pixelCount * 4;
}

// ═══════════════════════════════════════════════════════════════
//  Depth Readback (GPU → RDRAM)
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::CopyDepthFromGPU(GPUFramebuffer* fb, uint8_t* rdram) {
    if (!fb || !rdram || !fb->depthTex || !m_isGL) return;

    // Perfect Dark hack: only copy depth once per frame
    if (m_fbHacks.pdDepthOncePerFrame) {
        if (m_depthCopiedThisFrame) return;
        m_depthCopiedThisFrame = true;
    }

    const uint32_t w = fb->width;
    const uint32_t h = fb->height;
    const uint32_t pixelCount = w * h;

    std::vector<float> depthFloat(pixelCount);

    if (m_resScale == 1) {
        glGetTextureImage(fb->depthTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                          static_cast<GLsizei>(pixelCount * sizeof(float)), depthFloat.data());
    } else {
        // Downsample depth to native resolution — reuse cached temp FBO/tex
        if (m_copyDepthW != w || m_copyDepthH != h) {
            if (m_copyDepthFBO) {
                glDeleteFramebuffers(1, &m_copyDepthFBO);
                m_copyDepthFBO = 0;
            }
            if (m_copyDepthTex) {
                glDeleteTextures(1, &m_copyDepthTex);
                m_copyDepthTex = 0;
            }
        }
        if (!m_copyDepthFBO) {
            glCreateTextures(GL_TEXTURE_2D, 1, &m_copyDepthTex);
            glTextureStorage2D(m_copyDepthTex, 1, GL_DEPTH32F_STENCIL8, w, h);
            glCreateFramebuffers(1, &m_copyDepthFBO);
            glNamedFramebufferTexture(m_copyDepthFBO, GL_DEPTH_STENCIL_ATTACHMENT, m_copyDepthTex, 0);
            m_copyDepthW = w;
            m_copyDepthH = h;
        }

        glBlitNamedFramebuffer(fb->fbo, m_copyDepthFBO, 0, 0, fb->scaledWidth, fb->scaledHeight, 0, 0, w, h,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glGetTextureImage(m_copyDepthTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                          static_cast<GLsizei>(pixelCount * sizeof(float)), depthFloat.data());
    }

    // Convert float depth to N64 compressed Z format and write to RDRAM
    uint32_t depthAddr = fb->desc.address;
    uint16_t* dst = reinterpret_cast<uint16_t*>(rdram + depthAddr);
    ConvertDepthFloatToN64(depthFloat.data(), dst, pixelCount, w);

    // Trace depth readback
    {
        uint32_t depthEnd = depthAddr + pixelCount * 2;
        if (depthAddr <= 0x35CCFF && depthEnd > 0x35CC00) {
            Log::infof("[DEPTHTRACE] CopyDepthFromGPU addr=%06X size=%u w=%u h=%u covers 0x35CC00", depthAddr,
                       pixelCount * 2, w, h);
        }
        if (depthAddr <= 0x1CAACF && depthEnd > 0x1CAA00) {
            Log::infof("[DEPTHTRACE] CopyDepthFromGPU addr=%06X size=%u w=%u h=%u covers 0x1CAA00", depthAddr,
                       pixelCount * 2, w, h);
        }
    }

    MarkRDRAMDirty(depthAddr, pixelCount * 2);
    m_stats.gpuToRdramCopies++;
    m_stats.bytesTransferred += pixelCount * sizeof(float);
}

// ═══════════════════════════════════════════════════════════════
//  Async PBO Readback
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::BeginAsyncReadback(GPUFramebuffer* fb) {
    if (!fb || !m_isGL) return;

    // If a readback is already pending, we must finish it first
    if (m_pboFence) {
        NOVA_LOG_WARN("BeginAsyncReadback: previous readback still pending, forcing sync");
        // Delete old fence without completing — data is lost
        glDeleteSync(static_cast<GLsync>(m_pboFence));
        m_pboFence = nullptr;
        m_pboFB = nullptr;
    }

    const uint32_t w = fb->width;
    const uint32_t h = fb->height;
    const uint32_t byteSize = w * h * 4;

    // Create PBO if needed (or resize)
    if (!m_pbo) {
        glCreateBuffers(1, &m_pbo);
    }
    // Only reallocate if size changed (avoid per-frame buffer orphaning on Intel)
    if (m_pboSize != byteSize) {
        glNamedBufferData(m_pbo, byteSize, nullptr, GL_STREAM_READ);
        m_pboSize = byteSize;
    }

    // Bind PBO as pack target and read from framebuffer
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);

    if (m_resScale == 1 && fb->msaaSamples <= 1) {
        glGetTextureImage(fb->colorTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(byteSize), nullptr);
    } else {
        // Downsample to native res — reuse cached readback FBO
        if (m_readbackW != w || m_readbackH != h) {
            if (m_readbackFBO) {
                glDeleteFramebuffers(1, &m_readbackFBO);
                m_readbackFBO = 0;
            }
            if (m_readbackTex) {
                glDeleteTextures(1, &m_readbackTex);
                m_readbackTex = 0;
            }
        }
        if (!m_readbackFBO) {
            glCreateTextures(GL_TEXTURE_2D, 1, &m_readbackTex);
            glTextureStorage2D(m_readbackTex, 1, GL_RGBA8, w, h);
            glCreateFramebuffers(1, &m_readbackFBO);
            glNamedFramebufferTexture(m_readbackFBO, GL_COLOR_ATTACHMENT0, m_readbackTex, 0);
            m_readbackW = w;
            m_readbackH = h;
        }
        glBlitNamedFramebuffer(fb->fbo, m_readbackFBO, 0, 0, fb->scaledWidth, fb->scaledHeight, 0, 0, w, h,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glGetTextureImage(m_readbackTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(byteSize), nullptr);
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // Insert fence sync
    m_pboFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_pboAddress = fb->desc.address;
    m_pboWidth = w;
    m_pboHeight = h;
    m_pboFormat = fb->desc.format;
    m_pboFB = fb;
}

bool FramebufferManager::FinishAsyncReadback(uint8_t* rdram) {
    if (!m_pboFence || !rdram || !m_isGL) return false;

    GLsync fence = static_cast<GLsync>(m_pboFence);

    // Check if the readback is done (non-blocking)
    GLenum result = glClientWaitSync(fence, 0, 0);
    if (result == GL_TIMEOUT_EXPIRED) {
        return false; // Not ready yet
    }

    // Readback is complete — map the PBO and copy to RDRAM
    glDeleteSync(fence);
    m_pboFence = nullptr;

    const uint32_t pixelCount = m_pboWidth * m_pboHeight;
    const uint32_t byteSize = pixelCount * 4;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
    const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, byteSize, GL_MAP_READ_BIT);
    if (mapped) {
        const auto* rgba8 = static_cast<const uint8_t*>(mapped);
        uint32_t addr = m_pboAddress;

        if (m_pboFormat == FBFormat::RGBA16) {
            uint16_t* dst = reinterpret_cast<uint16_t*>(rdram + addr);
            ConvertRGBA8ToRGBA16(rgba8, dst, pixelCount);
        } else {
            uint32_t* dst = reinterpret_cast<uint32_t*>(rdram + addr);
            ConvertRGBA8ToRGBA32(rgba8, dst, pixelCount);
        }

        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        MarkRDRAMDirty(addr, pixelCount * ((m_pboFormat == FBFormat::RGBA16) ? 2u : 4u));
        m_stats.gpuToRdramCopies++;
        m_stats.bytesTransferred += byteSize;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // The GPU pixels just landed in RDRAM, so the FB no longer has
    // unsynced GPU-side modifications. Without this, the next frame's
    // mid-frame texture loads (LoadBlock/LoadTile aliased to this FB)
    // would re-trigger a synchronous glGetTextureImage stall.
    if (m_pboFB) {
        m_pboFB->gpuModified = false;
        ClearRDRAMDirty(m_pboAddress, m_pboFB->desc.ByteSize());
        m_pboFB = nullptr;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Tile Copy (RDP COPY_MODE region transfer)
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::TileCopy(GPUFramebuffer* src, GPUFramebuffer* dst, int32_t srcX, int32_t srcY, int32_t dstX,
                                  int32_t dstY, int32_t w, int32_t h) {
    if (!src || !dst || w <= 0 || h <= 0 || !m_isGL) return;

    // Scale coordinates
    int32_t sx = srcX * static_cast<int32_t>(m_resScale);
    int32_t sy = srcY * static_cast<int32_t>(m_resScale);
    int32_t dx = dstX * static_cast<int32_t>(m_resScale);
    int32_t dy = dstY * static_cast<int32_t>(m_resScale);
    int32_t sw = w * static_cast<int32_t>(m_resScale);
    int32_t sh = h * static_cast<int32_t>(m_resScale);

    glBlitNamedFramebuffer(src->fbo, dst->fbo, sx, sy, sx + sw, sy + sh, dx, dy, dx + sw, dy + sh, GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);
}

// ═══════════════════════════════════════════════════════════════
//  RDRAM Dirty Tracking
// ═══════════════════════════════════════════════════════════════

bool FramebufferManager::IsRDRAMDirty(uint32_t address, uint32_t size) const {
    uint32_t startPage = address / kPageSize;
    uint32_t endPage = (address + size + kPageSize - 1) / kPageSize;
    endPage = std::min(endPage, kPageCount);

    for (uint32_t p = startPage; p < endPage; ++p) {
        if (m_dirtyPages[p]) return true;
    }
    return false;
}

void FramebufferManager::MarkRDRAMDirty(uint32_t address, uint32_t size) {
    uint32_t startPage = address / kPageSize;
    uint32_t endPage = (address + size + kPageSize - 1) / kPageSize;
    endPage = std::min(endPage, kPageCount);

    for (uint32_t p = startPage; p < endPage; ++p) {
        m_dirtyPages[p] = true;
    }
}

void FramebufferManager::ClearRDRAMDirty(uint32_t address, uint32_t size) {
    uint32_t startPage = address / kPageSize;
    uint32_t endPage = (address + size + kPageSize - 1) / kPageSize;
    endPage = std::min(endPage, kPageCount);

    for (uint32_t p = startPage; p < endPage; ++p) {
        m_dirtyPages[p] = false;
    }
}

// ═══════════════════════════════════════════════════════════════
//  VI Output
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::UpdateVI(const VIConfig& config) {
    // Detect VI geometry change — if width or height changed, existing
    // framebuffers at the display address may need to be recreated.
    if (m_viConfig.width != 0 && (m_viConfig.width != config.width || m_viConfig.height != config.height)) {

        GPUFramebuffer* displayFB = FindByAddress(config.origin);
        if (displayFB && (displayFB->width != config.width || displayFB->height != config.height)) {
            // Destroy the stale FBO so FindOrCreate rebuilds it at the new size.
            // Migrate presentation snapshot so it survives the rebuild.
            GLuint savedPresentation = displayFB->presentationTex;
            displayFB->presentationTex = 0;
            DestroyGPUFramebuffer(*displayFB);
            *displayFB = {};
            displayFB->presentationTex = savedPresentation;
            NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "VI geometry changed {}x{} → {}x{}, invalidated display FBO",
                     m_viConfig.width, m_viConfig.height, config.width, config.height);
        }
    }

    m_viConfig = config;
    m_frameCounter++;
    m_depthCopiedThisFrame = false; // Reset PD depth-once-per-frame flag
    m_stats.viUpdates++;

    // Register the VI display buffer as a known framebuffer so that
    // FindByAddress can match texture reads from the displayed image
    // (e.g., OoT's cfb RGBA32 anti-aliasing filter reads the VI buffer
    //  at offsets, not the RDP render target).
    if (config.origin != 0 && config.width > 0) {
        GPUFramebuffer* viFB = FindByAddress(config.origin);
        if (!viFB) {
            FramebufferDesc desc;
            desc.address = config.origin & 0x7FFFFF;
            desc.width = config.width;
            desc.height = config.height > 0 ? config.height : 240;
            desc.format = FBFormat::RGBA16;
            desc.size = 2;
            desc.stride = config.width * 2;
            desc.type = FBType::Color;
            viFB = FindOrCreate(desc);
            if (viFB) {
                viFB->lastUsedFrame = m_frameCounter;
                // Upload RDRAM content to populate the GPU texture
                if (m_rdram) CopyToGPU(viFB, m_rdram);
            }
        }
    }
}

GPUFramebuffer* FramebufferManager::GetDisplayFramebuffer() { return FindByAddress(m_viConfig.origin); }

void FramebufferManager::BlitToScreen(uint32_t screenWidth, uint32_t screenHeight, bool skipVIFilter) {
    if (!m_isGL) return;
    GPUFramebuffer* displayFB = GetDisplayFramebuffer();

    // If VI_ORIGIN doesn't match any FBO (e.g., early boot frames before VI is set),
    // fall back to the last FBO that was actively rendered to.
    if (!displayFB) displayFB = m_currentColor;

    if (!displayFB) {
        // Still clear the back buffer so we don't present an undefined frame after SwapBuffers.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    m_stats.blitsToScreen++;

    if (m_viConfig.gammaCorrect && m_gammaProgram) ApplyGammaCorrection(displayFB, 2.0f);

    // Apply VI divot/AA/dedither filter if enabled (N64 VI_STATUS bits)
    // Skip for HLE-rendered content — the 3D pipeline already produces smooth output;
    // the VI hardware filter only makes sense for raw software-rendered framebuffers.
    if (!skipVIFilter && (m_viConfig.divot || m_viConfig.antiAlias || m_viConfig.dedither) && m_viFilterProgram)
        ApplyVIFilter(displayFB);

    // Bind default framebuffer (screen)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenWidth, screenHeight);
    glDisable(GL_SCISSOR_TEST); // FlushBatch leaves scissor enabled with a game scissor rect
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Determine aspect ratio
    int32_t vpX = 0, vpY = 0;
    uint32_t vpW = screenWidth, vpH = screenHeight;

    if (m_outputAspect <= 0.0f || screenHeight == 0) {
        vpX = 0;
        vpY = 0;
        vpW = screenWidth;
        vpH = screenHeight;
    } else {
        // Box to the output shape, not the framebuffer's pixel ratio: the N64
        // shows every framebuffer at 4:3 (PAL games draw 320x288, and a
        // widescreen hack still wants 16:9 rather than the window's shape).
        float srcAspect = m_outputAspect;
        float dstAspect = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);

        if (srcAspect > dstAspect) {
            // Letterbox (black bars top/bottom)
            vpH = static_cast<uint32_t>(screenWidth / srcAspect);
            vpY = (screenHeight - vpH) / 2;
        } else {
            // Pillarbox (black bars left/right)
            vpW = static_cast<uint32_t>(screenHeight * srcAspect);
            vpX = (screenWidth - vpW) / 2;
        }
    }

    glViewport(vpX, vpY, vpW, vpH);

    // Use simple blit (no VI filter) for crisp output
    glUseProgram(m_blitProgram);
    // Overscan crop is hardcoded in the blit fragment shader (5 native pixels per edge)

    GLuint blitTex = (displayFB->presentationTex != 0) ? displayFB->presentationTex : displayFB->colorTex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blitTex);
    // Bilinear filtering for smooth presentation scaling.
    // The internal FBO resolution rarely matches the display exactly
    // (e.g. 1600x1200 FBO → 1920x1080 window), so bilinear gives a
    // clean image instead of the uneven pixel sizes from GL_NEAREST.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindVertexArray(m_blitVAO);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);

    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void FramebufferManager::ApplyVIFilter(GPUFramebuffer* fb) {
    if (!m_isGL || !fb || !m_viFilterProgram) return;

    // Reuse cached temp FBO/texture (avoid per-frame GPU alloc on Intel)
    if (m_tmpWidth != fb->scaledWidth || m_tmpHeight != fb->scaledHeight) {
        if (m_tmpFBO) {
            glDeleteFramebuffers(1, &m_tmpFBO);
            m_tmpFBO = 0;
        }
        if (m_tmpTex) {
            glDeleteTextures(1, &m_tmpTex);
            m_tmpTex = 0;
        }
    }
    if (!m_tmpFBO) {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_tmpTex);
        glTextureStorage2D(m_tmpTex, 1, GL_RGBA8, fb->scaledWidth, fb->scaledHeight);
        glCreateFramebuffers(1, &m_tmpFBO);
        glNamedFramebufferTexture(m_tmpFBO, GL_COLOR_ATTACHMENT0, m_tmpTex, 0);
        m_tmpWidth = fb->scaledWidth;
        m_tmpHeight = fb->scaledHeight;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_tmpFBO);
    glViewport(0, 0, fb->scaledWidth, fb->scaledHeight);

    glUseProgram(m_viFilterProgram);
    glActiveTexture(GL_TEXTURE0);
    // Apply to the texture that will actually be displayed (same logic as gamma).
    GLuint targetTex = (fb->presentationTex != 0) ? fb->presentationTex : fb->colorTex;
    glBindTexture(GL_TEXTURE_2D, targetTex);
    glUniform2f(glGetUniformLocation(m_viFilterProgram, "uPixelSize"), 1.0f / fb->scaledWidth, 1.0f / fb->scaledHeight);
    glUniform1i(glGetUniformLocation(m_viFilterProgram, "uEnableAA"), 1);
    glUniform1i(glGetUniformLocation(m_viFilterProgram, "uEnableDivot"), m_viConfig.divot ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_viFilterProgram, "uEnableDedither"), m_viConfig.dedither ? 1 : 0);
    // Interlace simulation: tell the shader which field is active
    glUniform1i(glGetUniformLocation(m_viFilterProgram, "uInterlaced"), m_viConfig.interlaced ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_viFilterProgram, "uField"), static_cast<int>(m_viConfig.field));

    glBindVertexArray(m_blitVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Copy result back
    glCopyImageSubData(m_tmpTex, GL_TEXTURE_2D, 0, 0, 0, 0, targetTex, GL_TEXTURE_2D, 0, 0, 0, 0, fb->scaledWidth,
                       fb->scaledHeight, 1);

    // Restore the current N64 FBO so subsequent rendering goes to the right target.
    GPUFramebuffer* curFB = GetCurrentColor();
    if (curFB && curFB->fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, curFB->fbo);
        glViewport(0, 0, curFB->scaledWidth, curFB->scaledHeight);
        glScissor(0, 0, curFB->scaledWidth, curFB->scaledHeight);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void FramebufferManager::ApplyGammaCorrection(GPUFramebuffer* fb, float gamma) {
    if (!m_isGL || !fb || !m_gammaProgram || std::abs(gamma - 1.0f) < 0.001f) return;

    // Apply to the texture that will actually be displayed.
    // BlitToScreen reads presentationTex when available, so gamma must
    // be applied there — not to colorTex (which is the render target).
    GLuint targetTex = (fb->presentationTex != 0) ? fb->presentationTex : fb->colorTex;

    // Reuse cached temp FBO/texture (avoid per-frame GPU alloc on Intel)
    if (m_tmpWidth != fb->scaledWidth || m_tmpHeight != fb->scaledHeight) {
        if (m_tmpFBO) {
            glDeleteFramebuffers(1, &m_tmpFBO);
            m_tmpFBO = 0;
        }
        if (m_tmpTex) {
            glDeleteTextures(1, &m_tmpTex);
            m_tmpTex = 0;
        }
    }
    if (!m_tmpFBO) {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_tmpTex);
        glTextureStorage2D(m_tmpTex, 1, GL_RGBA8, fb->scaledWidth, fb->scaledHeight);
        glCreateFramebuffers(1, &m_tmpFBO);
        glNamedFramebufferTexture(m_tmpFBO, GL_COLOR_ATTACHMENT0, m_tmpTex, 0);
        m_tmpWidth = fb->scaledWidth;
        m_tmpHeight = fb->scaledHeight;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_tmpFBO);
    glViewport(0, 0, fb->scaledWidth, fb->scaledHeight);

    glUseProgram(m_gammaProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targetTex);
    glUniform1f(glGetUniformLocation(m_gammaProgram, "uGamma"), gamma);

    glBindVertexArray(m_blitVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glCopyImageSubData(m_tmpTex, GL_TEXTURE_2D, 0, 0, 0, 0, targetTex, GL_TEXTURE_2D, 0, 0, 0, 0, fb->scaledWidth,
                       fb->scaledHeight, 1);

    // Restore the current N64 FBO so subsequent rendering goes to the right target.
    GPUFramebuffer* curFB2 = GetCurrentColor();
    if (curFB2 && curFB2->fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, curFB2->fbo);
        glViewport(0, 0, curFB2->scaledWidth, curFB2->scaledHeight);
        glScissor(0, 0, curFB2->scaledWidth, curFB2->scaledHeight);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

// ═══════════════════════════════════════════════════════════════
//  Format Conversion
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::ConvertRGBA16ToRGBA8(const uint16_t* src, uint8_t* dst, uint32_t pixelCount) {
    m_stats.formatConversions++;
    for (uint32_t i = 0; i < pixelCount; ++i) {
        // N64 RGBA16: RRRRR GGGGG BBBBB A (5-5-5-1)
        // RDRAM halfwords are swapped within each 32-bit word (XOR 2 byte addressing
        // = XOR 1 halfword indexing). No byte swap needed — LE uint16_t read of an
        // SH/PI-stored halfword already yields the correct numeric pixel value.
        uint16_t pixel = src[i ^ 1];
        uint8_t r5 = (pixel >> 11) & 0x1F;
        uint8_t g5 = (pixel >> 6) & 0x1F;
        uint8_t b5 = (pixel >> 1) & 0x1F;
        uint8_t a1 = pixel & 1;

        dst[i * 4 + 0] = (r5 << 3) | (r5 >> 2); // 5→8 bit expansion
        dst[i * 4 + 1] = (g5 << 3) | (g5 >> 2);
        dst[i * 4 + 2] = (b5 << 3) | (b5 >> 2);
        dst[i * 4 + 3] = a1 ? 0xFF : 0x00;
    }
}

void FramebufferManager::ConvertRGBA32ToRGBA8(const uint32_t* src, uint8_t* dst, uint32_t pixelCount) {
    m_stats.formatConversions++;
    for (uint32_t i = 0; i < pixelCount; ++i) {
        // N64 RGBA32 is big-endian RRGGBBAA
        uint32_t pixel = src[i];
        dst[i * 4 + 0] = (pixel >> 24) & 0xFF; // R
        dst[i * 4 + 1] = (pixel >> 16) & 0xFF; // G
        dst[i * 4 + 2] = (pixel >> 8) & 0xFF;  // B
        dst[i * 4 + 3] = pixel & 0xFF;         // A
    }
}

void FramebufferManager::ConvertRGBA8ToRGBA16(const uint8_t* src, uint16_t* dst, uint32_t pixelCount) {
    m_stats.formatConversions++;
    for (uint32_t i = 0; i < pixelCount; ++i) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        uint8_t a = src[i * 4 + 3];

        uint16_t pixel = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        dst[i ^ 1] = pixel; // XOR 1 for correct halfword position within 32-bit word
    }
}

void FramebufferManager::ConvertRGBA8ToRGBA32(const uint8_t* src, uint32_t* dst, uint32_t pixelCount) {
    m_stats.formatConversions++;
    for (uint32_t i = 0; i < pixelCount; ++i) {
        dst[i] = (static_cast<uint32_t>(src[i * 4 + 0]) << 24) | (static_cast<uint32_t>(src[i * 4 + 1]) << 16) |
                 (static_cast<uint32_t>(src[i * 4 + 2]) << 8) | static_cast<uint32_t>(src[i * 4 + 3]);
    }
}

void FramebufferManager::ConvertDepthN64ToFloat(const uint16_t* src, float* dst, uint32_t pixelCount) {
    m_stats.formatConversions++;
    // N64 depth: 14-bit integer (bits 15:2) + 2-bit dz (bits 1:0)
    // Range: 0x0000 = near, 0x3FFF = far
    constexpr float kInvMaxDepth = 1.0f / 16383.0f; // 14-bit max

    for (uint32_t i = 0; i < pixelCount; ++i) {
        uint16_t raw = src[i ^ 1];    // XOR 1 for correct halfword within 32-bit word
        uint16_t depthInt = raw >> 2; // Extract 14-bit depth
        dst[i] = static_cast<float>(depthInt) * kInvMaxDepth;
    }
}

void FramebufferManager::ConvertDepthFloatToN64(const float* src, uint16_t* dst, uint32_t pixelCount, uint32_t width) {
    m_stats.formatConversions++;
    // N64 dz field encodes the local depth derivative magnitude for coverage AA.
    // We compute it from the 4-connected neighbors (horizontal + vertical diff).
    // dz bins match the hardware lookup: 0→<1/16384, 1→<4/16384, 2→<64/16384, 3→rest
    for (uint32_t i = 0; i < pixelCount; ++i) {
        float depth = std::clamp(src[i], 0.0f, 1.0f);
        uint16_t depthInt = static_cast<uint16_t>(depth * 16383.0f);

        // Compute depth derivative from neighbours (finite differences)
        float dzf = 0.0f;
        if (i > 0) dzf = std::abs(src[i] - src[i - 1]);
        if (i + 1 < pixelCount) dzf = std::max(dzf, std::abs(src[i + 1] - src[i]));
        if (width > 0) {
            if (i >= width) dzf = std::max(dzf, std::abs(src[i] - src[i - width]));
            if (i + width < pixelCount) dzf = std::max(dzf, std::abs(src[i + width] - src[i]));
        }

        uint8_t dz;
        if (dzf < (1.0f / 16384.0f)) dz = 0;
        else if (dzf < (4.0f / 16384.0f)) dz = 1;
        else if (dzf < (64.0f / 16384.0f)) dz = 2;
        else dz = 3;

        uint16_t raw = (depthInt << 2) | dz;
        dst[i ^ 1] = raw; // XOR 1 for correct halfword within 32-bit word
    }
}

// ═══════════════════════════════════════════════════════════════
//  Framebuffer Effects
// ═══════════════════════════════════════════════════════════════

void FramebufferManager::CopyFramebuffer(GPUFramebuffer* src, GPUFramebuffer* dst) {
    if (!src || !dst) return;

    glBlitNamedFramebuffer(src->fbo, dst->fbo, 0, 0, src->scaledWidth, src->scaledHeight, 0, 0, dst->scaledWidth,
                           dst->scaledHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void FramebufferManager::FillRegion(GPUFramebuffer* fb, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!fb) return;

    float clearColor[4] = {((color >> 24) & 0xFF) / 255.0f, ((color >> 16) & 0xFF) / 255.0f,
                           ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f};

    // Scale coordinates
    x *= m_resScale;
    y *= m_resScale;
    w *= m_resScale;
    h *= m_resScale;

    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearNamedFramebufferfv(fb->fbo, GL_COLOR, 0, clearColor);
    glDisable(GL_SCISSOR_TEST);
}

void FramebufferManager::ResolveMSAA(GPUFramebuffer* msaaFB, GPUFramebuffer* resolved) {
    if (!msaaFB || !resolved) return;

    // Resolve color
    glBlitNamedFramebuffer(msaaFB->fbo, resolved->fbo, 0, 0, msaaFB->scaledWidth, msaaFB->scaledHeight, 0, 0,
                           resolved->scaledWidth, resolved->scaledHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Resolve depth (must be separate blit with GL_NEAREST)
    if (msaaFB->depthTex && resolved->depthTex) {
        glBlitNamedFramebuffer(msaaFB->fbo, resolved->fbo, 0, 0, msaaFB->scaledWidth, msaaFB->scaledHeight, 0, 0,
                               resolved->scaledWidth, resolved->scaledHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
}

} // namespace Nova64::Video
