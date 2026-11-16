/**
 * @file GLBackend.hpp
 * @brief OpenGL 4.6 core-profile backend (IRenderer implementation using direct state access).
 */
// SPDX-License-Identifier: MIT

#pragma once

#include "Renderer.hpp"
#include "../Common/Logger.hpp"

#include <glad/gl.h>

#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <map>
#include <tuple>

namespace Nova64 {
namespace Video {
namespace GL {

// --- GL TEXTURE POOL — recycles GL texture handles to avoid driver heap growth ---

class GLTexturePool {
public:
    using Key = std::tuple<uint32_t, uint32_t, TextureFormat, uint32_t>; // w,h,fmt,mips

    // Try to acquire a recycled GL handle matching the given desc.
    // Returns 0 if none available.
    GLuint Acquire(uint32_t w, uint32_t h, TextureFormat fmt, uint32_t mips);

    // Return a GL handle to the pool for recycling.
    // If the pool is full, the handle is deleted immediately.
    void Recycle(GLuint handle, uint32_t w, uint32_t h, TextureFormat fmt, uint32_t mips);

    // Delete all pooled handles (call on shutdown).
    void Shutdown();

    size_t PooledCount() const { return m_totalPooled; }

    static GLTexturePool& Instance();

private:
    static constexpr size_t kMaxPooled = 4096;
    std::map<Key, std::vector<GLuint>> m_pool;
    size_t m_totalPooled = 0;
};

// --- GL FORMAT TRANSLATION UTILITIES ---

struct GLFormatInfo {
    GLenum internalFormat;
    GLenum baseFormat;
    GLenum type;
};

inline GLFormatInfo TranslateFormat(TextureFormat fmt) {
    switch (fmt) {
    case TextureFormat::RGBA8:           return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case TextureFormat::RGB8:            return {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE};
    case TextureFormat::RGBA4:           return {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4};
    case TextureFormat::RGB5A1:          return {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1};
    case TextureFormat::R8:              return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case TextureFormat::RG8:             return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case TextureFormat::RGBA16F:         return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
    case TextureFormat::RGBA32F:         return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
    case TextureFormat::Depth16:         return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT};
    case TextureFormat::Depth24:         return {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT};
    case TextureFormat::Depth32F:        return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
    case TextureFormat::Depth24Stencil8: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
    // N64 formats mapped to standard GL formats
    case TextureFormat::IA16: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case TextureFormat::I8:   return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case TextureFormat::CI4:  return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case TextureFormat::CI8:  return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    default:                  return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    }
}

inline GLenum TranslatePrimitive(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::Triangles:     return GL_TRIANGLES;
    case PrimitiveType::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveType::TriangleFan:   return GL_TRIANGLE_FAN;
    case PrimitiveType::Lines:         return GL_LINES;
    case PrimitiveType::LineStrip:     return GL_LINE_STRIP;
    case PrimitiveType::Points:        return GL_POINTS;
    default:                           return GL_TRIANGLES;
    }
}

inline GLenum TranslateBlendFactor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:                  return GL_ZERO;
    case BlendFactor::One:                   return GL_ONE;
    case BlendFactor::SrcColor:              return GL_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:      return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor:              return GL_DST_COLOR;
    case BlendFactor::OneMinusDstColor:      return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha:              return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:      return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:              return GL_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:      return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor:         return GL_CONSTANT_COLOR;
    case BlendFactor::OneMinusConstantColor: return GL_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::ConstantAlpha:         return GL_CONSTANT_ALPHA;
    case BlendFactor::OneMinusConstantAlpha: return GL_ONE_MINUS_CONSTANT_ALPHA;
    case BlendFactor::SrcAlphaSaturate:      return GL_SRC_ALPHA_SATURATE;
    case BlendFactor::Src1Color:             return GL_SRC1_COLOR;
    case BlendFactor::OneMinusSrc1Color:     return GL_ONE_MINUS_SRC1_COLOR;
    case BlendFactor::Src1Alpha:             return GL_SRC1_ALPHA;
    case BlendFactor::OneMinusSrc1Alpha:     return GL_ONE_MINUS_SRC1_ALPHA;
    default:                                 return GL_ONE;
    }
}

inline GLenum TranslateBlendOp(BlendOp op) {
    switch (op) {
    case BlendOp::Add:             return GL_FUNC_ADD;
    case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
    case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOp::Min:             return GL_MIN;
    case BlendOp::Max:             return GL_MAX;
    default:                       return GL_FUNC_ADD;
    }
}

inline GLenum TranslateCompareFunc(CompareFunc func) {
    switch (func) {
    case CompareFunc::Never:        return GL_NEVER;
    case CompareFunc::Less:         return GL_LESS;
    case CompareFunc::Equal:        return GL_EQUAL;
    case CompareFunc::LessEqual:    return GL_LEQUAL;
    case CompareFunc::Greater:      return GL_GREATER;
    case CompareFunc::NotEqual:     return GL_NOTEQUAL;
    case CompareFunc::GreaterEqual: return GL_GEQUAL;
    case CompareFunc::Always:       return GL_ALWAYS;
    default:                        return GL_ALWAYS;
    }
}

inline GLenum TranslateStencilOp(StencilOp op) {
    switch (op) {
    case StencilOp::Keep:      return GL_KEEP;
    case StencilOp::Zero:      return GL_ZERO;
    case StencilOp::Replace:   return GL_REPLACE;
    case StencilOp::IncrClamp: return GL_INCR;
    case StencilOp::DecrClamp: return GL_DECR;
    case StencilOp::Invert:    return GL_INVERT;
    case StencilOp::IncrWrap:  return GL_INCR_WRAP;
    case StencilOp::DecrWrap:  return GL_DECR_WRAP;
    default:                   return GL_KEEP;
    }
}

inline GLenum TranslateTexFilter(SamplerFilter filter) {
    switch (filter) {
    case SamplerFilter::Nearest:           return GL_NEAREST;
    case SamplerFilter::Linear:            return GL_LINEAR;
    case SamplerFilter::NearestMipNearest: return GL_NEAREST_MIPMAP_NEAREST;
    case SamplerFilter::LinearMipNearest:  return GL_LINEAR_MIPMAP_NEAREST;
    case SamplerFilter::NearestMipLinear:  return GL_NEAREST_MIPMAP_LINEAR;
    case SamplerFilter::LinearMipLinear:   return GL_LINEAR_MIPMAP_LINEAR;
    case SamplerFilter::N64_3Point:        return GL_NEAREST; // Emulated in shader
    default:                               return GL_LINEAR;
    }
}

inline GLenum TranslateTexWrap(TextureWrap wrap) {
    switch (wrap) {
    case TextureWrap::Repeat:            return GL_REPEAT;
    case TextureWrap::MirroredRepeat:    return GL_MIRRORED_REPEAT;
    case TextureWrap::ClampToEdge:       return GL_CLAMP_TO_EDGE;
    case TextureWrap::ClampToBorder:     return GL_CLAMP_TO_BORDER;
    case TextureWrap::MirrorClampToEdge: return GL_MIRROR_CLAMP_TO_EDGE;
    default:                             return GL_REPEAT;
    }
}

inline GLenum TranslateIndexType(IndexType type) {
    return (type == IndexType::UInt32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
}

inline GLenum TranslateVertexAttribBaseType(VertexAttribType type) {
    switch (type) {
    case VertexAttribType::Float1:
    case VertexAttribType::Float2:
    case VertexAttribType::Float3:
    case VertexAttribType::Float4:  return GL_FLOAT;
    case VertexAttribType::UByte4:
    case VertexAttribType::UByte4N: return GL_UNSIGNED_BYTE;
    case VertexAttribType::Short2:
    case VertexAttribType::Short4:
    case VertexAttribType::Short2N:
    case VertexAttribType::Short4N: return GL_SHORT;
    case VertexAttribType::UInt1:   return GL_UNSIGNED_INT;
    case VertexAttribType::Int1:    return GL_INT;
    default:                        return GL_FLOAT;
    }
}

// --- GL VERTEX BUFFER ---

class GLVertexBuffer : public IVertexBuffer {
public:
    GLVertexBuffer(size_t sizeBytes, BufferUsage usage);
    ~GLVertexBuffer() override;

    void Upload(const void* data, size_t sizeBytes) override;
    void Update(const void* data, size_t offset, size_t sizeBytes) override;
    size_t GetSize() const override { return m_size; }
    BufferUsage GetUsage() const override { return m_usage; }
    GLuint GetHandle() const { return m_handle; }

private:
    GLuint m_handle = 0;
    size_t m_size = 0;
    BufferUsage m_usage;
};

// --- GL INDEX BUFFER ---

class GLIndexBuffer : public IIndexBuffer {
public:
    GLIndexBuffer(size_t sizeBytes, BufferUsage usage);
    ~GLIndexBuffer() override;

    void Upload(const void* data, size_t sizeBytes, IndexType type) override;
    void Update(const void* data, size_t offset, size_t sizeBytes) override;
    size_t GetCount() const override { return m_count; }
    IndexType GetIndexType() const override { return m_indexType; }
    GLuint GetHandle() const { return m_handle; }

private:
    GLuint m_handle = 0;
    size_t m_size = 0;
    size_t m_count = 0;
    IndexType m_indexType = IndexType::UInt16;
    BufferUsage m_usage;
};

// --- GL UNIFORM BUFFER ---

class GLUniformBuffer : public IUniformBuffer {
public:
    explicit GLUniformBuffer(size_t sizeBytes);
    ~GLUniformBuffer() override;

    void Upload(const void* data, size_t sizeBytes) override;
    void Update(const void* data, size_t offset, size_t sizeBytes) override;
    void BindSlot(uint32_t slot) override;
    size_t GetSize() const override { return m_size; }
    GLuint GetHandle() const { return m_handle; }

private:
    GLuint m_handle = 0;
    size_t m_size = 0;
};

// --- GL TEXTURE ---

class GLTexture : public ITexture {
public:
    GLTexture(const TextureDesc& desc);
    ~GLTexture() override;

    void Upload(const void* data, size_t sizeBytes, uint32_t mipLevel = 0) override;
    void UploadSubRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data, size_t sizeBytes,
                         uint32_t mipLevel = 0) override;
    void GenerateMipmaps() override;
    void SetSampler(const SamplerDesc& desc) override;

    uint32_t GetWidth() const override { return m_desc.width; }
    uint32_t GetHeight() const override { return m_desc.height; }
    TextureFormat GetFormat() const override { return m_desc.format; }
    uint64_t GetNativeHandle() const override { return static_cast<uint64_t>(m_handle); }
    GLuint GetHandle() const { return m_handle; }

private:
    GLuint m_handle = 0;
    TextureDesc m_desc;
};

// --- GL SHADER PROGRAM ---

class GLShaderProgram : public IShaderProgram {
public:
    GLShaderProgram();
    ~GLShaderProgram() override;

    bool Compile(const std::string& vertexSrc, const std::string& fragmentSrc,
                 const std::string& geometrySrc = "") override;
    bool CompileCompute(const std::string& computeSrc) override;
    void Bind() override;
    void Unbind() override;

    void SetInt(const char* name, int value) override;
    void SetFloat(const char* name, float value) override;
    void SetVec2(const char* name, float x, float y) override;
    void SetVec3(const char* name, float x, float y, float z) override;
    void SetVec4(const char* name, float x, float y, float z, float w) override;
    void SetMat4(const char* name, const float* data) override;

    int GetUniformLocation(const char* name) const override;
    uint64_t GetNativeHandle() const override { return static_cast<uint64_t>(m_program); }
    const std::string& GetInfoLog() const override { return m_infoLog; }

private:
    GLuint CompileShader(GLenum type, const std::string& source);

    GLuint m_program = 0;
    std::string m_infoLog;
    mutable std::unordered_map<std::string, GLint> m_uniformCache;
};

// --- GL RENDER TARGET (Framebuffer Object) ---

class GLRenderTarget : public IRenderTarget {
public:
    GLRenderTarget(const RenderTargetDesc& desc);
    ~GLRenderTarget() override;

    void Bind() override;
    void Unbind() override;
    void Clear(float r, float g, float b, float a, float depth = 1.0f, uint8_t stencil = 0) override;
    void ClearColor(float r, float g, float b, float a) override;
    void ClearDepthStencil(float depth, uint8_t stencil) override;
    void Resize(uint32_t width, uint32_t height) override;

    ITexture* GetColorTexture() override { return m_colorTex.get(); }
    ITexture* GetDepthTexture() override { return m_depthTex.get(); }
    uint32_t GetWidth() const override { return m_desc.width; }
    uint32_t GetHeight() const override { return m_desc.height; }
    uint64_t GetNativeHandle() const override { return static_cast<uint64_t>(m_fbo); }

private:
    void CreateAttachments();
    void DestroyAttachments();

    GLuint m_fbo = 0;
    RenderTargetDesc m_desc;
    std::unique_ptr<GLTexture> m_colorTex;
    std::unique_ptr<GLTexture> m_depthTex;
};

// --- GL PIPELINE (VAO + State combination) ---

class GLPipeline : public IPipeline {
public:
    GLPipeline(const PipelineDesc& desc);
    ~GLPipeline() override;

    void Bind() override;
    uint64_t GetNativeHandle() const override { return static_cast<uint64_t>(m_vao); }

    const PipelineDesc& GetDesc() const { return m_desc; }

private:
    GLuint m_vao = 0;
    PipelineDesc m_desc;
};

// --- GL RENDERER — MAIN BACKEND ---

class GLRenderer : public IRenderer {
public:
    GLRenderer();
    ~GLRenderer() override;

    // --- Lifecycle ---
    bool Initialize(void* windowHandle, uint32_t width, uint32_t height) override;
    void Shutdown() override;
    void BeginFrame() override;
    void EndFrame() override;
    void Present() override;

    // --- Resource Creation ---
    std::unique_ptr<IVertexBuffer> CreateVertexBuffer(size_t sizeBytes, BufferUsage usage) override;
    std::unique_ptr<IIndexBuffer> CreateIndexBuffer(size_t sizeBytes, BufferUsage usage) override;
    std::unique_ptr<IUniformBuffer> CreateUniformBuffer(size_t sizeBytes) override;
    std::unique_ptr<ITexture> CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<IShaderProgram> CreateShaderProgram() override;
    std::unique_ptr<IRenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) override;
    std::unique_ptr<IPipeline> CreatePipeline(const PipelineDesc& desc) override;

    // --- State Management ---
    void SetViewport(const RenderViewport& vp) override;
    void SetScissor(const RenderScissorRect& rect) override;
    void SetBlendState(const BlendState& state) override;
    void SetDepthStencilState(const DepthStencilState& state) override;
    void SetRasterizerState(const RasterizerState& state) override;
    void SetBlendColor(float r, float g, float b, float a) override;

    // --- Binding ---
    void BindVertexBuffer(IVertexBuffer* vb, const VertexLayout& layout) override;
    void BindIndexBuffer(IIndexBuffer* ib) override;
    void BindTexture(uint32_t unit, ITexture* tex) override;
    void BindShader(IShaderProgram* shader) override;
    void BindPipeline(IPipeline* pipeline) override;
    void BindRenderTarget(IRenderTarget* rt) override;
    void BindImageForRW(uint32_t unit, uint64_t handle, ImageBindFormat format) override;
    void UnbindAllImages() override;
    void SetColorWriteMask(bool r, bool g, bool b, bool a) override;
    void DisableRasterizerColorOutput() override;
    void RestoreRasterizerColorOutput() override;
    void ImageMemoryBarrier() override;

    // --- Drawing ---
    void Draw(const DrawCommand& cmd) override;
    void DrawArrays(PrimitiveType prim, uint32_t first, uint32_t count) override;
    void DrawIndexed(PrimitiveType prim, uint32_t indexCount, IndexType type, uint32_t offset) override;

    // --- Compute ---
    void DispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;

    // --- Readback ---
    void ReadPixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h, TextureFormat format, void* outData) override;

    // --- Debug ---
    void PushDebugGroup(const char* name) override;
    void PopDebugGroup() override;
    void InsertDebugMarker(const char* name) override;

    // --- Queries ---
    BackendType GetBackendType() const override { return BackendType::OpenGL46; }
    const RendererCaps& GetCapabilities() const override { return m_caps; }
    const RendererStats& GetStats() const override { return m_stats; }
    void ResetStats() override { m_stats.Reset(); }

    // --- Window ---
    void OnResize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

private:
    void QueryCapabilities();
    static void GLAD_API_PTR DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                           const GLchar* message, const void* userParam);

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    RendererCaps m_caps;
    RendererStats m_stats;

    // Objects currently bound, as tracked by this renderer.
    GLuint m_boundVAO = 0;
    GLuint m_boundProgram = 0;
    GLuint m_boundFBO = 0;
    GLuint m_boundVBO = 0;
    GLuint m_boundEBO = 0;
    IndexType m_boundIBType = IndexType::UInt16;

    bool m_initialized = false;
};

// --- FACTORY FUNCTION ---

std::unique_ptr<IRenderer> CreateOpenGLRenderer();

} // namespace GL
} // namespace Video
} // namespace Nova64
