/**
 * @file GLRenderer.cpp
 * @brief OpenGL 4.6 Backend.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - OpenGL 4.6 Backend Implementation
// Part 2: GLRenderer main class implementation

#include "GLBackend.hpp"
#include "../Common/Logger.hpp"
#include "../../common/log.hpp"

#include <glad/gl.h>
#include <cstring>
#include <cassert>

namespace Nova64 { namespace Video { namespace GL {

// ═══════════════════════════════════════════════════════════════════════════════
//  GL RENDERER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════════

GLRenderer::GLRenderer() = default;

GLRenderer::~GLRenderer() {
    if (m_initialized)
        Shutdown();
}

bool GLRenderer::Initialize(void* windowHandle, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    // glad2 doesn't have GLVersion struct — use glGetIntegerv
    int major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    if (major < 4 || (major == 4 && minor < 6)) {
        // Fall back — try to use 4.5 if DSA is available
        if (major < 4 || (major == 4 && minor < 5)) {
            return false;
        }
    }

    // Enable debug output in debug builds
#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(DebugCallback, this);
    // Disable notification-level messages
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION,
                           0, nullptr, GL_FALSE);
#endif

    // Set up initial GL state
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_SCISSOR_TEST);

    // Use the [0,1] NDC depth convention to match Vulkan and N64 natively.
    // Our ortho matrix computes clip_z = z_norm where z_norm ∈ [0,1] (normalized screen z).
    // That maps clip_z to [0,1] which is fully inside the [0,1] visible range.
    if (GLAD_GL_ARB_clip_control || major > 4 || (major == 4 && minor >= 5)) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    }

    // Disable backface culling initially (N64 manages its own culling)
    glDisable(GL_CULL_FACE);

    // Enable depth clamping — prevents the GPU from clipping geometry at the
    // near/far planes.  N64 F3DZEX uses NoN (No Near-plane) mode, and our HLE
    // vertex pipeline already handles W-plane clipping in software.  Without
    // depth clamping, the GPU rejects geometry near z=0 and z=w that should
    // be visible.
    glEnable(GL_DEPTH_CLAMP);

    // Set viewport
    glViewport(0, 0, width, height);
    glScissor(0, 0, width, height);

    QueryCapabilities();

    m_initialized = true;
    return true;
}

void GLRenderer::Shutdown() {
    if (!m_initialized) return;

    GLTexturePool::Instance().Shutdown();

    m_boundVAO = 0;
    m_boundProgram = 0;
    m_boundFBO = 0;
    m_boundVBO = 0;
    m_boundEBO = 0;

    m_initialized = false;
}

void GLRenderer::BeginFrame() {
    m_stats.Reset();
    glViewport(0, 0, m_width, m_height);
    glScissor(0, 0, m_width, m_height);
}

void GLRenderer::EndFrame() {
    // No explicit glFlush needed — SDL_GL_SwapWindow performs an implicit flush.
    // Removing this avoids a redundant CPU-GPU sync point that stalls Intel drivers.
}

void GLRenderer::Present() {
    // Presentation is handled by SDL_GL_SwapWindow in the window layer
    // This is a no-op for the GL backend
}

// ─── Resource Creation ─────────────────────────────────────────────────────

std::unique_ptr<IVertexBuffer> GLRenderer::CreateVertexBuffer(size_t sizeBytes, BufferUsage usage) {
    return std::make_unique<GLVertexBuffer>(sizeBytes, usage);
}

std::unique_ptr<IIndexBuffer> GLRenderer::CreateIndexBuffer(size_t sizeBytes, BufferUsage usage) {
    return std::make_unique<GLIndexBuffer>(sizeBytes, usage);
}

std::unique_ptr<IUniformBuffer> GLRenderer::CreateUniformBuffer(size_t sizeBytes) {
    return std::make_unique<GLUniformBuffer>(sizeBytes);
}

std::unique_ptr<ITexture> GLRenderer::CreateTexture(const TextureDesc& desc) {
    return std::make_unique<GLTexture>(desc);
}

std::unique_ptr<IShaderProgram> GLRenderer::CreateShaderProgram() {
    return std::make_unique<GLShaderProgram>();
}

std::unique_ptr<IRenderTarget> GLRenderer::CreateRenderTarget(const RenderTargetDesc& desc) {
    return std::make_unique<GLRenderTarget>(desc);
}

std::unique_ptr<IPipeline> GLRenderer::CreatePipeline(const PipelineDesc& desc) {
    return std::make_unique<GLPipeline>(desc);
}

// ─── State Management ──────────────────────────────────────────────────────

void GLRenderer::SetViewport(const RenderViewport& vp) {
    glViewportIndexedf(0, vp.x, vp.y, vp.width, vp.height);
    glDepthRangeIndexed(0, vp.minDepth, vp.maxDepth);
}

void GLRenderer::SetScissor(const RenderScissorRect& rect) {
    glScissor(rect.x, rect.y, rect.width, rect.height);
}

void GLRenderer::SetBlendState(const BlendState& state) {
    if (state.enabled) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(
            TranslateBlendFactor(state.srcColor),
            TranslateBlendFactor(state.dstColor),
            TranslateBlendFactor(state.srcAlpha),
            TranslateBlendFactor(state.dstAlpha)
        );
        glBlendEquationSeparate(
            TranslateBlendOp(state.colorOp),
            TranslateBlendOp(state.alphaOp)
        );
        glBlendColor(state.blendColor[0], state.blendColor[1],
                      state.blendColor[2], state.blendColor[3]);
    } else {
        glDisable(GL_BLEND);
    }
    // The write mask applies whether or not blending is on (Vulkan's pipeline
    // uses it either way); it used to be forced to all channels without blending.
    glColorMask(
        (state.colorWriteMask & 0x01) ? GL_TRUE : GL_FALSE,
        (state.colorWriteMask & 0x02) ? GL_TRUE : GL_FALSE,
        (state.colorWriteMask & 0x04) ? GL_TRUE : GL_FALSE,
        (state.colorWriteMask & 0x08) ? GL_TRUE : GL_FALSE
    );
    m_stats.stateChanges++;
}

void GLRenderer::SetDepthStencilState(const DepthStencilState& state) {
    if (state.depthTestEnabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(TranslateCompareFunc(state.depthFunc));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(state.depthWriteEnabled ? GL_TRUE : GL_FALSE);

    if (state.stencilEnabled) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(TranslateCompareFunc(state.stencilFunc), state.stencilRef, state.stencilReadMask);
        glStencilOp(TranslateStencilOp(state.stencilFail),
                     TranslateStencilOp(state.depthFail),
                     TranslateStencilOp(state.stencilPass));
        glStencilMask(state.stencilWriteMask);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    m_stats.stateChanges++;
}

void GLRenderer::SetRasterizerState(const RasterizerState& state) {
    // Cull mode
    if (state.cullMode == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        switch (state.cullMode) {
            case CullMode::Front: glCullFace(GL_FRONT); break;
            case CullMode::Back: glCullFace(GL_BACK); break;
            case CullMode::FrontAndBack: glCullFace(GL_FRONT_AND_BACK); break;
            default: break;
        }
    }

    // Front face
    glFrontFace(state.frontFace == FrontFace::Clockwise ? GL_CW : GL_CCW);

    // Polygon mode
    switch (state.polygonMode) {
        case PolygonMode::Fill:      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); break;
        case PolygonMode::Wireframe: glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); break;
        case PolygonMode::Points:    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT); break;
    }

    // Scissor
    if (state.scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    // Depth bias
    if (state.depthBias != 0.0f || state.depthBiasSlope != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(state.depthBiasSlope, state.depthBias);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Multisample
    if (state.multisampleEnabled)
        glEnable(GL_MULTISAMPLE);
    else
        glDisable(GL_MULTISAMPLE);

    m_stats.stateChanges++;
}

void GLRenderer::SetBlendColor(float r, float g, float b, float a) {
    glBlendColor(r, g, b, a);
}

// ─── Binding ───────────────────────────────────────────────────────────────

void GLRenderer::BindVertexBuffer(IVertexBuffer* vb, const VertexLayout& layout) {
    if (!vb) return;
    auto* glVB = static_cast<GLVertexBuffer*>(vb);
    m_boundVBO = glVB->GetHandle();

    // Bind to the current VAO's binding point 0
    if (m_boundVAO) {
        glVertexArrayVertexBuffer(m_boundVAO, 0, m_boundVBO, 0, layout.stride);
    }
}

void GLRenderer::BindIndexBuffer(IIndexBuffer* ib) {
    if (!ib) return;
    auto* glIB = static_cast<GLIndexBuffer*>(ib);
    m_boundEBO = glIB->GetHandle();
    m_boundIBType = glIB->GetIndexType();

    if (m_boundVAO) {
        glVertexArrayElementBuffer(m_boundVAO, m_boundEBO);
    }
}

void GLRenderer::BindTexture(uint32_t unit, ITexture* tex) {
    if (!tex) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindSampler(unit, 0);
        return;
    }
    auto* glTex = static_cast<GLTexture*>(tex);
    // Use legacy binding instead of DSA glBindTextureUnit — Intel Iris Xe
    // has a driver bug where DSA binding becomes stale after FBO switches.
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, glTex->GetHandle());
    glBindSampler(unit, 0);
    m_stats.stateChanges++;
}

void GLRenderer::BindShader(IShaderProgram* shader) {
    if (!shader) {
        glUseProgram(0);
        m_boundProgram = 0;
        return;
    }
    auto* glShader = static_cast<GLShaderProgram*>(shader);
    GLuint handle = static_cast<GLuint>(glShader->GetNativeHandle());
    if (handle != m_boundProgram) {
        glUseProgram(handle);
        m_boundProgram = handle;
        m_stats.shaderSwaps++;
    }
}

void GLRenderer::BindPipeline(IPipeline* pipeline) {
    if (!pipeline) return;
    auto* glPipeline = static_cast<GLPipeline*>(pipeline);
    GLuint vao = static_cast<GLuint>(glPipeline->GetNativeHandle());
    // Always rebind VAO — external code (e.g. BlitToScreen) may change it
    // without updating m_boundVAO.
    glBindVertexArray(vao);
    m_boundVAO = vao;

    // Apply pipeline state
    const auto& desc = glPipeline->GetDesc();
    SetBlendState(desc.blend);
    SetDepthStencilState(desc.depthStencil);
    SetRasterizerState(desc.rasterizer);

    if (desc.shader) {
        BindShader(desc.shader);
    }

    m_stats.pipelineBinds++;
}

void GLRenderer::BindRenderTarget(IRenderTarget* rt) {
    if (!rt) {
        // Bind default framebuffer
        if (m_boundFBO != 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            m_boundFBO = 0;
            glViewport(0, 0, m_width, m_height);
        }
        return;
    }
    auto* glRT = static_cast<GLRenderTarget*>(rt);
    GLuint fbo = static_cast<GLuint>(glRT->GetNativeHandle());
    if (fbo != m_boundFBO) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        m_boundFBO = fbo;
        glViewport(0, 0, glRT->GetWidth(), glRT->GetHeight());
    }
}

// ─── Image (MLE) Binding ───────────────────────────────────────────────────

static GLenum TranslateImageFormat(ImageBindFormat fmt) {
    switch (fmt) {
        case ImageBindFormat::R32F:  return GL_R32F;
        case ImageBindFormat::RGBA8: return GL_RGBA8;
        case ImageBindFormat::R8UI:  return GL_R8UI;
        default:                 return GL_RGBA8;
    }
}

void GLRenderer::BindImageForRW(uint32_t unit, uint64_t handle, ImageBindFormat format) {
    glBindImageTexture(unit, static_cast<GLuint>(handle), 0, GL_FALSE, 0,
                       GL_READ_WRITE, TranslateImageFormat(format));
}

void GLRenderer::UnbindAllImages() {
    for (uint32_t i = 0; i < 4; ++i)
        glBindImageTexture(i, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
}

void GLRenderer::SetColorWriteMask(bool r, bool g, bool b, bool a) {
    glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE,
                b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void GLRenderer::DisableRasterizerColorOutput() {
    glDrawBuffer(GL_NONE);
}

void GLRenderer::RestoreRasterizerColorOutput() {
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
}

void GLRenderer::ImageMemoryBarrier() {
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
}

// ─── Drawing ───────────────────────────────────────────────────────────────

void GLRenderer::Draw(const DrawCommand& cmd) {
    GLenum mode = TranslatePrimitive(cmd.primitive);

    if (cmd.indexed) {
        GLenum indexType = TranslateIndexType(m_boundIBType);
        glDrawElementsInstanced(mode, cmd.indexCount, indexType,
                                 reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd.indexOffset)),
                                 cmd.instanceCount);
        m_stats.triangles += cmd.indexCount / 3;
    } else {
        glDrawArraysInstanced(mode, cmd.vertexOffset, cmd.vertexCount, cmd.instanceCount);
        m_stats.triangles += cmd.vertexCount / 3;
    }

    m_stats.drawCalls++;
}

void GLRenderer::DrawArrays(PrimitiveType prim, uint32_t first, uint32_t count) {
    glDrawArrays(TranslatePrimitive(prim), first, count);
    m_stats.drawCalls++;
    m_stats.triangles += count / 3;
}

void GLRenderer::DrawIndexed(PrimitiveType prim, uint32_t indexCount, IndexType type, uint32_t offset) {
    glDrawElements(TranslatePrimitive(prim), indexCount, TranslateIndexType(type),
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(offset)));
    m_stats.drawCalls++;
    m_stats.triangles += indexCount / 3;
}

// ─── Compute ───────────────────────────────────────────────────────────────

void GLRenderer::DispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) {
    glDispatchCompute(groupsX, groupsY, groupsZ);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
}

// ─── Readback ──────────────────────────────────────────────────────────────

void GLRenderer::ReadPixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             TextureFormat format, void* outData) {
    GLFormatInfo fmt = TranslateFormat(format);
    glReadPixels(x, y, w, h, fmt.baseFormat, fmt.type, outData);
}

// ─── Debug ─────────────────────────────────────────────────────────────────

void GLRenderer::PushDebugGroup(const char* name) {
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
}

void GLRenderer::PopDebugGroup() {
    glPopDebugGroup();
}

void GLRenderer::InsertDebugMarker(const char* name) {
    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0,
                          GL_DEBUG_SEVERITY_NOTIFICATION, -1, name);
}

// ─── Window ────────────────────────────────────────────────────────────────

void GLRenderer::OnResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
    glScissor(0, 0, width, height);
}

// ─── Capability Query ──────────────────────────────────────────────────────

void GLRenderer::QueryCapabilities() {
    m_caps.backend = BackendType::OpenGL46;
    m_caps.deviceName = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    m_caps.driverVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    m_caps.apiVersion = "OpenGL 4.6";

    // Detect Intel GPU for driver-specific workarounds
    const char* vendorStr = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    m_caps.isIntelGPU = vendorStr && (strstr(vendorStr, "Intel") != nullptr);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, reinterpret_cast<GLint*>(&m_caps.maxTextureSize));
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, reinterpret_cast<GLint*>(&m_caps.maxTextureLayers));
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, reinterpret_cast<GLint*>(&m_caps.maxTextureUnits));
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, reinterpret_cast<GLint*>(&m_caps.maxUniformBlockSize));
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, reinterpret_cast<GLint*>(&m_caps.maxVertexAttribs));
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, reinterpret_cast<GLint*>(&m_caps.maxColorAttachments));

    GLint maxComputeWorkGroupCount[3], maxComputeWorkGroupSize[3];
    for (int i = 0; i < 3; i++) {
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &maxComputeWorkGroupCount[i]);
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, i, &maxComputeWorkGroupSize[i]);
        m_caps.maxComputeWorkGroupCount[i] = static_cast<uint32_t>(maxComputeWorkGroupCount[i]);
        m_caps.maxComputeWorkGroupSize[i] = static_cast<uint32_t>(maxComputeWorkGroupSize[i]);
    }

    m_caps.supportsCompute = true; // Required by GL 4.6
    m_caps.supportsGeometryShaders = true;
    m_caps.supportsTessellation = true;
    m_caps.supportsSSBO = true;
    m_caps.supportsDSA = true;
    m_caps.supportsClipControl = true;
    m_caps.supportsTimestampQueries = true;
    m_caps.supportsDebugMarkers = true;
    m_caps.supportsIndirectDraw = true;

    // Anisotropic filtering
    if (GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic) {
        m_caps.supportsAnisotropicFiltering = true;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m_caps.maxAnisotropy);
    }

    // Multisample
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    m_caps.maxSamples = static_cast<uint32_t>(maxSamples);
    m_caps.supportsMultisample = maxSamples > 1;

    // Bindless textures (optional extension)
    m_caps.supportsBindlessTextures = GLAD_GL_ARB_bindless_texture != 0;

    // Fragment shader interlock (needed for MLE blender and N64 depth compare)
    // Intel Iris Xe advertises the extension but the GLSL compiler rejects
    // endInvocationInterlockARB inside flow control (if/for/while).
    // Our generated shaders keep begin/end at the top level of main() so
    // a simple probe (without flow control) is sufficient.
    m_caps.supportsFragmentShaderInterlock = false;
    if (GLAD_GL_ARB_fragment_shader_interlock || GLAD_GL_NV_fragment_shader_interlock) {
        const char* testSrc =
            "#version 460\n"
            "#extension GL_ARB_fragment_shader_interlock : enable\n"
            "layout(pixel_interlock_ordered) in;\n"
            "layout(binding = 0, r32f) uniform restrict image2D uImg;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "    beginInvocationInterlockARB();\n"
            "    float v = imageLoad(uImg, ivec2(0)).r;\n"
            "    imageStore(uImg, ivec2(0), vec4(v + 0.5, 0.0, 0.0, 0.0));\n"
            "    endInvocationInterlockARB();\n"
            "    fragColor = vec4(v);\n"
            "}\n";
        GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(sh, 1, &testSrc, nullptr);
        // Suppress debug output during probe — expected to fail on some drivers.
        // Restore what was there: release builds never turn debug output on,
        // and enabling it without a callback only slows the driver down.
        const GLboolean debugOutputWasOn = glIsEnabled(GL_DEBUG_OUTPUT);
        glDisable(GL_DEBUG_OUTPUT);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        glDeleteShader(sh);
        if (debugOutputWasOn) glEnable(GL_DEBUG_OUTPUT);
        m_caps.supportsFragmentShaderInterlock = (ok == GL_TRUE);
    }
}

// ─── Debug Callback ────────────────────────────────────────────────────────

void GLAD_API_PTR GLRenderer::DebugCallback(GLenum source, GLenum type, GLuint id,
                                          GLenum severity, GLsizei length,
                                          const GLchar* message, const void* userParam) {
    (void)source; (void)id; (void)length; (void)userParam;

    // Filter out known non-issues
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204)
        return;

    const char* typeStr = "Unknown";
    switch (type) {
        case GL_DEBUG_TYPE_ERROR:               typeStr = "ERROR"; break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:  typeStr = "DEPRECATED"; break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:   typeStr = "UNDEFINED"; break;
        case GL_DEBUG_TYPE_PORTABILITY:          typeStr = "PORTABILITY"; break;
        case GL_DEBUG_TYPE_PERFORMANCE:          typeStr = "PERFORMANCE"; break;
        case GL_DEBUG_TYPE_MARKER:              typeStr = "MARKER"; break;
        case GL_DEBUG_TYPE_OTHER:               typeStr = "OTHER"; break;
    }

    // Log based on severity
    if (severity == GL_DEBUG_SEVERITY_HIGH) {
        Log::infof("[GL_ERROR] %s", message);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  FACTORY FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════════

std::unique_ptr<IRenderer> CreateOpenGLRenderer() {
    return std::make_unique<GLRenderer>();
}

bool IsOpenGL46Available() {
    // This would be called before full GL initialization
    // to check if the system supports GL 4.6
    // For now, assume it's available if we got this far
    return true;
}

}}} // namespace Nova64::Video::GL
