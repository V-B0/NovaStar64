/**
 * @file GLBackend.cpp
 * @brief OpenGL 4.6 backend resources: buffers, textures, shader programs, render targets and pipelines.
 */
// SPDX-License-Identifier: MIT

#include "GLBackend.hpp"
#include "../Common/Logger.hpp"

#include <glad/gl.h>
#include <cstring>
#include <cassert>
#include <algorithm>

namespace Nova64 {
namespace Video {
// Defined in RendererFactory.cpp.
uint32_t GetVertexAttribComponentCount(VertexAttribType type);
} // namespace Video
} // namespace Nova64

namespace Nova64 {
namespace Video {
namespace GL {

// --- GL TEXTURE POOL IMPLEMENTATION ---

GLTexturePool& GLTexturePool::Instance() {
    static GLTexturePool s_instance;
    return s_instance;
}

GLuint GLTexturePool::Acquire(uint32_t w, uint32_t h, TextureFormat fmt, uint32_t mips) {
    Key key{w, h, fmt, mips};
    auto it = m_pool.find(key);
    if (it == m_pool.end() || it->second.empty()) return 0;
    GLuint handle = it->second.back();
    it->second.pop_back();
    m_totalPooled--;
    if (it->second.empty()) m_pool.erase(it);
    return handle;
}

void GLTexturePool::Recycle(GLuint handle, uint32_t w, uint32_t h, TextureFormat fmt, uint32_t mips) {
    if (m_totalPooled >= kMaxPooled) {
        glDeleteTextures(1, &handle);
        return;
    }
    Key key{w, h, fmt, mips};
    m_pool[key].push_back(handle);
    m_totalPooled++;
}

void GLTexturePool::Shutdown() {
    for (auto& [key, handles] : m_pool) {
        for (GLuint h : handles) glDeleteTextures(1, &h);
    }
    m_pool.clear();
    m_totalPooled = 0;
}

// --- GL USAGE TRANSLATION ---

[[maybe_unused]] static GLenum TranslateBufferUsage(BufferUsage usage) {
    switch (usage) {
    case BufferUsage::Static:  return GL_STATIC_DRAW;
    case BufferUsage::Dynamic: return GL_DYNAMIC_DRAW;
    case BufferUsage::Stream:  return GL_STREAM_DRAW;
    default:                   return GL_STATIC_DRAW;
    }
}

// --- GL VERTEX BUFFER IMPLEMENTATION ---

GLVertexBuffer::GLVertexBuffer(size_t sizeBytes, BufferUsage usage) : m_size(sizeBytes), m_usage(usage) {
    glCreateBuffers(1, &m_handle);
    if (sizeBytes > 0) {
        // Every buffer can be updated: Upload()/Update() use glNamedBufferSubData,
        // which immutable storage refuses without GL_DYNAMIC_STORAGE_BIT.
        GLbitfield flags = GL_DYNAMIC_STORAGE_BIT;
        if (usage == BufferUsage::Dynamic || usage == BufferUsage::Stream) flags |= GL_MAP_WRITE_BIT;
        glNamedBufferStorage(m_handle, static_cast<GLsizeiptr>(sizeBytes), nullptr, flags);
    }
}

GLVertexBuffer::~GLVertexBuffer() {
    if (m_handle) glDeleteBuffers(1, &m_handle);
}

void GLVertexBuffer::Upload(const void* data, size_t sizeBytes) {
    if (!data || sizeBytes == 0) return;
    if (sizeBytes > m_size) {
        // Immutable storage cannot be resized, so recreate the buffer.
        glDeleteBuffers(1, &m_handle);
        m_size = sizeBytes;
        glCreateBuffers(1, &m_handle);
        GLbitfield flags = GL_DYNAMIC_STORAGE_BIT;
        if (m_usage == BufferUsage::Dynamic || m_usage == BufferUsage::Stream) flags |= GL_MAP_WRITE_BIT;
        glNamedBufferStorage(m_handle, static_cast<GLsizeiptr>(sizeBytes), data, flags);
    } else {
        glNamedBufferSubData(m_handle, 0, static_cast<GLsizeiptr>(sizeBytes), data);
    }
}

void GLVertexBuffer::Update(const void* data, size_t offset, size_t sizeBytes) {
    if (!data || sizeBytes == 0) return;
    assert(offset + sizeBytes <= m_size);
    glNamedBufferSubData(m_handle, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(sizeBytes), data);
}

// --- GL INDEX BUFFER IMPLEMENTATION ---

GLIndexBuffer::GLIndexBuffer(size_t sizeBytes, BufferUsage usage) : m_size(sizeBytes), m_usage(usage) {
    glCreateBuffers(1, &m_handle);
    if (sizeBytes > 0) {
        // Every buffer can be updated: Upload()/Update() use glNamedBufferSubData,
        // which immutable storage refuses without GL_DYNAMIC_STORAGE_BIT.
        GLbitfield flags = GL_DYNAMIC_STORAGE_BIT;
        if (usage == BufferUsage::Dynamic || usage == BufferUsage::Stream) flags |= GL_MAP_WRITE_BIT;
        glNamedBufferStorage(m_handle, static_cast<GLsizeiptr>(sizeBytes), nullptr, flags);
    }
}

GLIndexBuffer::~GLIndexBuffer() {
    if (m_handle) glDeleteBuffers(1, &m_handle);
}

void GLIndexBuffer::Upload(const void* data, size_t sizeBytes, IndexType type) {
    m_indexType = type;
    size_t indexSize = (type == IndexType::UInt32) ? 4 : 2;
    m_count = sizeBytes / indexSize;

    if (!data || sizeBytes == 0) return;
    if (sizeBytes > m_size) {
        // Immutable storage cannot be resized, so recreate the buffer.
        glDeleteBuffers(1, &m_handle);
        m_size = sizeBytes;
        glCreateBuffers(1, &m_handle);
        GLbitfield flags = GL_DYNAMIC_STORAGE_BIT;
        if (m_usage == BufferUsage::Dynamic || m_usage == BufferUsage::Stream) flags |= GL_MAP_WRITE_BIT;
        glNamedBufferStorage(m_handle, static_cast<GLsizeiptr>(sizeBytes), data, flags);
    } else {
        glNamedBufferSubData(m_handle, 0, static_cast<GLsizeiptr>(sizeBytes), data);
    }
}

void GLIndexBuffer::Update(const void* data, size_t offset, size_t sizeBytes) {
    if (!data || sizeBytes == 0) return;
    glNamedBufferSubData(m_handle, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(sizeBytes), data);
}

// --- GL UNIFORM BUFFER IMPLEMENTATION ---

GLUniformBuffer::GLUniformBuffer(size_t sizeBytes) : m_size(sizeBytes) {
    glCreateBuffers(1, &m_handle);
    GLbitfield flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT;
    glNamedBufferStorage(m_handle, static_cast<GLsizeiptr>(sizeBytes), nullptr, flags);
}

GLUniformBuffer::~GLUniformBuffer() {
    if (m_handle) glDeleteBuffers(1, &m_handle);
}

void GLUniformBuffer::Upload(const void* data, size_t sizeBytes) {
    if (!data || sizeBytes == 0) return;
    glNamedBufferSubData(m_handle, 0, static_cast<GLsizeiptr>(std::min(sizeBytes, m_size)), data);
}

void GLUniformBuffer::Update(const void* data, size_t offset, size_t sizeBytes) {
    if (!data || sizeBytes == 0) return;
    glNamedBufferSubData(m_handle, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(sizeBytes), data);
}

void GLUniformBuffer::BindSlot(uint32_t slot) { glBindBufferBase(GL_UNIFORM_BUFFER, slot, m_handle); }

// --- GL TEXTURE IMPLEMENTATION ---

GLTexture::GLTexture(const TextureDesc& desc) : m_desc(desc) {
    // Direct state access throughout: creating, uploading to or configuring a
    // texture must not change what is bound to the active texture unit.
    // glBindTexture would, so a texture created or uploaded while a draw was
    // being set up (a cache miss on texture 1) would replace texture 0.
    m_desc.mipLevels = std::max(1u, desc.mipLevels);
    m_handle = GLTexturePool::Instance().Acquire(m_desc.width, m_desc.height, m_desc.format, m_desc.mipLevels);
    if (!m_handle) {
        const GLFormatInfo fmt = TranslateFormat(m_desc.format);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_handle);
        glTextureStorage2D(m_handle, static_cast<GLsizei>(m_desc.mipLevels), fmt.internalFormat,
                           static_cast<GLsizei>(std::max(1u, m_desc.width)),
                           static_cast<GLsizei>(std::max(1u, m_desc.height)));
    }
    // A pooled handle keeps the previous owner's parameters; reset all of them.
    glTextureParameteri(m_handle, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_handle, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_handle, GL_TEXTURE_BASE_LEVEL, 0);
    glTextureParameteri(m_handle, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(m_desc.mipLevels - 1));
    glTextureParameterf(m_handle, GL_TEXTURE_LOD_BIAS, 0.0f);
    if (GLAD_GL_VERSION_4_6 || GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic)
        glTextureParameterf(m_handle, GL_TEXTURE_MAX_ANISOTROPY, 1.0f);

    if (desc.debugName && desc.debugName[0]) {
        glObjectLabel(GL_TEXTURE, m_handle, -1, desc.debugName);
    }
}

GLTexture::~GLTexture() {
    if (m_handle) {
        GLTexturePool::Instance().Recycle(m_handle, m_desc.width, m_desc.height, m_desc.format, m_desc.mipLevels);
        m_handle = 0;
    }
}

void GLTexture::Upload(const void* data, size_t sizeBytes, uint32_t mipLevel) {
    (void)sizeBytes;
    if (!data || mipLevel >= m_desc.mipLevels) return;
    const GLFormatInfo fmt = TranslateFormat(m_desc.format);
    const uint32_t w = std::max(1u, m_desc.width >> mipLevel);
    const uint32_t h = std::max(1u, m_desc.height >> mipLevel);
    glTextureSubImage2D(m_handle, static_cast<GLint>(mipLevel), 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                        fmt.baseFormat, fmt.type, data);
}

void GLTexture::UploadSubRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data, size_t sizeBytes,
                                uint32_t mipLevel) {
    (void)sizeBytes;
    if (!data || mipLevel >= m_desc.mipLevels) return;
    const GLFormatInfo fmt = TranslateFormat(m_desc.format);
    glTextureSubImage2D(m_handle, static_cast<GLint>(mipLevel), static_cast<GLint>(x), static_cast<GLint>(y),
                        static_cast<GLsizei>(w), static_cast<GLsizei>(h), fmt.baseFormat, fmt.type, data);
}

void GLTexture::GenerateMipmaps() {
    if (m_desc.mipLevels > 1) glGenerateTextureMipmap(m_handle);
}

void GLTexture::SetSampler(const SamplerDesc& desc) {
    glTextureParameteri(m_handle, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(TranslateTexFilter(desc.minFilter)));
    glTextureParameteri(m_handle, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(TranslateTexFilter(desc.magFilter)));
    glTextureParameteri(m_handle, GL_TEXTURE_WRAP_S, static_cast<GLint>(TranslateTexWrap(desc.wrapS)));
    glTextureParameteri(m_handle, GL_TEXTURE_WRAP_T, static_cast<GLint>(TranslateTexWrap(desc.wrapT)));
    // Always set, so a value from an earlier sampler (or a pooled handle's
    // previous owner) doesn't linger when the new one uses the default.
    if (GLAD_GL_VERSION_4_6 || GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic)
        glTextureParameterf(m_handle, GL_TEXTURE_MAX_ANISOTROPY, std::max(1.0f, desc.maxAnisotropy));
    glTextureParameterf(m_handle, GL_TEXTURE_LOD_BIAS, desc.lodBias);
    if (desc.wrapS == TextureWrap::ClampToBorder || desc.wrapT == TextureWrap::ClampToBorder) {
        glTextureParameterfv(m_handle, GL_TEXTURE_BORDER_COLOR, desc.borderColor);
    }
}

// --- GL SHADER PROGRAM IMPLEMENTATION ---

GLShaderProgram::GLShaderProgram() { m_program = glCreateProgram(); }

GLShaderProgram::~GLShaderProgram() {
    if (m_program) glDeleteProgram(m_program);
}

GLuint GLShaderProgram::CompileShader(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            std::string log(logLen, '\0');
            glGetShaderInfoLog(shader, logLen, nullptr, &log[0]);
            m_infoLog += log;
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool GLShaderProgram::Compile(const std::string& vertexSrc, const std::string& fragmentSrc,
                              const std::string& geometrySrc) {
    m_infoLog.clear();
    m_uniformCache.clear();

    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    if (!vs) return false;

    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }

    GLuint gs = 0;
    if (!geometrySrc.empty()) {
        gs = CompileShader(GL_GEOMETRY_SHADER, geometrySrc);
        if (!gs) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return false;
        }
    }

    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    if (gs) glAttachShader(m_program, gs);

    glLinkProgram(m_program);

    GLint status = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            std::string log(logLen, '\0');
            glGetProgramInfoLog(m_program, logLen, nullptr, &log[0]);
            m_infoLog += log;
        }
    }

    glDetachShader(m_program, vs);
    glDetachShader(m_program, fs);
    if (gs) glDetachShader(m_program, gs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (gs) glDeleteShader(gs);

    return status != 0;
}

bool GLShaderProgram::CompileCompute(const std::string& computeSrc) {
    m_infoLog.clear();
    m_uniformCache.clear();

    GLuint cs = CompileShader(GL_COMPUTE_SHADER, computeSrc);
    if (!cs) return false;

    glAttachShader(m_program, cs);
    glLinkProgram(m_program);

    GLint status = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            std::string log(logLen, '\0');
            glGetProgramInfoLog(m_program, logLen, nullptr, &log[0]);
            m_infoLog += log;
        }
    }

    glDetachShader(m_program, cs);
    glDeleteShader(cs);

    return status != 0;
}

void GLShaderProgram::Bind() { glUseProgram(m_program); }

void GLShaderProgram::Unbind() { glUseProgram(0); }

int GLShaderProgram::GetUniformLocation(const char* name) const {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) return it->second;
    GLint loc = glGetUniformLocation(m_program, name);
    m_uniformCache[name] = loc;
    return loc;
}

void GLShaderProgram::SetInt(const char* name, int value) {
    glProgramUniform1i(m_program, GetUniformLocation(name), value);
}

void GLShaderProgram::SetFloat(const char* name, float value) {
    glProgramUniform1f(m_program, GetUniformLocation(name), value);
}

void GLShaderProgram::SetVec2(const char* name, float x, float y) {
    glProgramUniform2f(m_program, GetUniformLocation(name), x, y);
}

void GLShaderProgram::SetVec3(const char* name, float x, float y, float z) {
    glProgramUniform3f(m_program, GetUniformLocation(name), x, y, z);
}

void GLShaderProgram::SetVec4(const char* name, float x, float y, float z, float w) {
    glProgramUniform4f(m_program, GetUniformLocation(name), x, y, z, w);
}

void GLShaderProgram::SetMat4(const char* name, const float* data) {
    glProgramUniformMatrix4fv(m_program, GetUniformLocation(name), 1, GL_FALSE, data);
}

// --- GL RENDER TARGET IMPLEMENTATION ---

GLRenderTarget::GLRenderTarget(const RenderTargetDesc& desc) : m_desc(desc) {
    glCreateFramebuffers(1, &m_fbo);
    CreateAttachments();

    if (desc.debugName && desc.debugName[0]) {
        glObjectLabel(GL_FRAMEBUFFER, m_fbo, -1, desc.debugName);
    }
}

GLRenderTarget::~GLRenderTarget() {
    DestroyAttachments();
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
}

void GLRenderTarget::CreateAttachments() {
    TextureDesc colorDesc;
    colorDesc.width = m_desc.width;
    colorDesc.height = m_desc.height;
    colorDesc.format = m_desc.colorFormat;
    colorDesc.isRenderTarget = true;
    m_colorTex = std::make_unique<GLTexture>(colorDesc);

    glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0, m_colorTex->GetHandle(), 0);

    if (m_desc.hasDepth) {
        TextureDesc depthDesc;
        depthDesc.width = m_desc.width;
        depthDesc.height = m_desc.height;
        depthDesc.format = m_desc.depthFormat;
        depthDesc.isDepthStencil = true;
        m_depthTex = std::make_unique<GLTexture>(depthDesc);

        GLenum attachment =
            (m_desc.depthFormat == TextureFormat::Depth24Stencil8) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        glNamedFramebufferTexture(m_fbo, attachment, m_depthTex->GetHandle(), 0);
    }

    GLenum status = glCheckNamedFramebufferStatus(m_fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // Incompleteness is not reported.
    }
}

void GLRenderTarget::DestroyAttachments() {
    m_colorTex.reset();
    m_depthTex.reset();
}

void GLRenderTarget::Bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_desc.width, m_desc.height);
}

void GLRenderTarget::Unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

void GLRenderTarget::Clear(float r, float g, float b, float a, float depth, uint8_t stencil) {
    float color[4] = {r, g, b, a};
    glClearNamedFramebufferfv(m_fbo, GL_COLOR, 0, color);
    if (m_desc.hasDepth) {
        if (m_desc.depthFormat == TextureFormat::Depth24Stencil8) {
            glClearNamedFramebufferfi(m_fbo, GL_DEPTH_STENCIL, 0, depth, stencil);
        } else {
            glClearNamedFramebufferfv(m_fbo, GL_DEPTH, 0, &depth);
        }
    }
}

void GLRenderTarget::ClearColor(float r, float g, float b, float a) {
    float color[4] = {r, g, b, a};
    glClearNamedFramebufferfv(m_fbo, GL_COLOR, 0, color);
}

void GLRenderTarget::ClearDepthStencil(float depth, uint8_t stencil) {
    if (m_desc.depthFormat == TextureFormat::Depth24Stencil8) {
        glClearNamedFramebufferfi(m_fbo, GL_DEPTH_STENCIL, 0, depth, stencil);
    } else {
        glClearNamedFramebufferfv(m_fbo, GL_DEPTH, 0, &depth);
    }
}

void GLRenderTarget::Resize(uint32_t width, uint32_t height) {
    if (width == m_desc.width && height == m_desc.height) return;
    m_desc.width = width;
    m_desc.height = height;
    DestroyAttachments();
    CreateAttachments();
}

// --- GL PIPELINE IMPLEMENTATION ---

GLPipeline::GLPipeline(const PipelineDesc& desc) : m_desc(desc) {
    glCreateVertexArrays(1, &m_vao);

    for (const auto& attrib : desc.vertexLayout.attribs) {
        glEnableVertexArrayAttrib(m_vao, attrib.location);
        GLenum baseType = TranslateVertexAttribBaseType(attrib.type);
        uint32_t components = Nova64::Video::GetVertexAttribComponentCount(attrib.type);
        bool isNormalized = attrib.normalized || attrib.type == VertexAttribType::UByte4N ||
                            attrib.type == VertexAttribType::Short2N || attrib.type == VertexAttribType::Short4N;

        if (baseType == GL_FLOAT || isNormalized) {
            glVertexArrayAttribFormat(m_vao, attrib.location, components, baseType, isNormalized ? GL_TRUE : GL_FALSE,
                                      attrib.offset);
        } else {
            glVertexArrayAttribIFormat(m_vao, attrib.location, components, baseType, attrib.offset);
        }
        glVertexArrayAttribBinding(m_vao, attrib.location, 0);
    }
}

GLPipeline::~GLPipeline() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

void GLPipeline::Bind() { glBindVertexArray(m_vao); }

} // namespace GL
} // namespace Video
} // namespace Nova64
