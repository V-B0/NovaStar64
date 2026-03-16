// NovaGraphics — OpenGL Shader Program Implementation
// Compilation, linking, reflection, uniform management, hot-reload.

#include "GLShaderProgram.hpp"

#include <glad/gl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cassert>
#include <filesystem>

namespace Nova64::Video::GL {

// ═══════════════════════════════════════════════════════════════
//  Stage Helpers
// ═══════════════════════════════════════════════════════════════

uint32_t ShaderStageToGL(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:       return GL_VERTEX_SHADER;
    case ShaderStage::Fragment:     return GL_FRAGMENT_SHADER;
    case ShaderStage::Geometry:     return GL_GEOMETRY_SHADER;
    case ShaderStage::TessControl:  return GL_TESS_CONTROL_SHADER;
    case ShaderStage::TessEval:     return GL_TESS_EVALUATION_SHADER;
    case ShaderStage::Compute:      return GL_COMPUTE_SHADER;
    }
    return GL_VERTEX_SHADER;
}

const char* ShaderStageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:       return "Vertex";
    case ShaderStage::Fragment:     return "Fragment";
    case ShaderStage::Geometry:     return "Geometry";
    case ShaderStage::TessControl:  return "TessControl";
    case ShaderStage::TessEval:     return "TessEval";
    case ShaderStage::Compute:      return "Compute";
    }
    return "Unknown";
}

// ═══════════════════════════════════════════════════════════════
//  GLShader
// ═══════════════════════════════════════════════════════════════

GLShader::~GLShader() { Destroy(); }

GLShader::GLShader(GLShader&& other) noexcept
    : m_handle(other.m_handle), m_stage(other.m_stage),
      m_infoLog(std::move(other.m_infoLog)), m_source(std::move(other.m_source)) {
    other.m_handle = 0;
}

GLShader& GLShader::operator=(GLShader&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_handle = other.m_handle;
        m_stage = other.m_stage;
        m_infoLog = std::move(other.m_infoLog);
        m_source = std::move(other.m_source);
        other.m_handle = 0;
    }
    return *this;
}

bool GLShader::CompileFromSource(ShaderStage stage, const std::string& source) {
    Destroy();
    m_stage = stage;
    m_source = source;

    m_handle = glCreateShader(ShaderStageToGL(stage));
    if (!m_handle) {
        m_infoLog = "Failed to create shader object";
        return false;
    }

    const char* src = source.c_str();
    GLint length = static_cast<GLint>(source.size());
    glShaderSource(m_handle, 1, &src, &length);
    glCompileShader(m_handle);

    GLint status = 0;
    glGetShaderiv(m_handle, GL_COMPILE_STATUS, &status);

    GLint logLen = 0;
    glGetShaderiv(m_handle, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 1) {
        m_infoLog.resize(logLen);
        glGetShaderInfoLog(m_handle, logLen, nullptr, m_infoLog.data());
    }

    if (!status) {
        glDeleteShader(m_handle);
        m_handle = 0;
        return false;
    }

    return true;
}

bool GLShader::CompileFromSPIRV(ShaderStage stage, const std::vector<uint32_t>& spirv, const char* entry) {
    Destroy();
    m_stage = stage;

    m_handle = glCreateShader(ShaderStageToGL(stage));
    if (!m_handle) {
        m_infoLog = "Failed to create shader object for SPIR-V";
        return false;
    }

    glShaderBinary(1, &m_handle, GL_SHADER_BINARY_FORMAT_SPIR_V,
                    spirv.data(), static_cast<GLsizei>(spirv.size() * sizeof(uint32_t)));
    glSpecializeShader(m_handle, entry, 0, nullptr, nullptr);

    GLint status = 0;
    glGetShaderiv(m_handle, GL_COMPILE_STATUS, &status);

    GLint logLen = 0;
    glGetShaderiv(m_handle, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 1) {
        m_infoLog.resize(logLen);
        glGetShaderInfoLog(m_handle, logLen, nullptr, m_infoLog.data());
    }

    if (!status) {
        glDeleteShader(m_handle);
        m_handle = 0;
        return false;
    }

    return true;
}

void GLShader::Destroy() {
    if (m_handle) {
        glDeleteShader(m_handle);
        m_handle = 0;
    }
    m_infoLog.clear();
}

void GLShader::SetLabel(const std::string& label) {
    if (m_handle) {
        glObjectLabel(GL_SHADER, m_handle, static_cast<GLsizei>(label.size()), label.c_str());
    }
}

// ═══════════════════════════════════════════════════════════════
//  GLProgram
// ═══════════════════════════════════════════════════════════════

GLProgram::~GLProgram() { Destroy(); }

GLProgram::GLProgram(GLProgram&& other) noexcept
    : m_handle(other.m_handle), m_infoLog(std::move(other.m_infoLog)),
      m_reflection(std::move(other.m_reflection)),
      m_uniformCache(std::move(other.m_uniformCache)) {
    other.m_handle = 0;
}

GLProgram& GLProgram::operator=(GLProgram&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_handle = other.m_handle;
        m_infoLog = std::move(other.m_infoLog);
        m_reflection = std::move(other.m_reflection);
        m_uniformCache = std::move(other.m_uniformCache);
        other.m_handle = 0;
    }
    return *this;
}

bool GLProgram::Link(const GLShader& vertex, const GLShader& fragment) {
    Destroy();
    m_handle = glCreateProgram();
    if (!m_handle) return false;

    glAttachShader(m_handle, vertex.Handle());
    glAttachShader(m_handle, fragment.Handle());
    glLinkProgram(m_handle);

    if (!CheckLinkStatus()) return false;
    BuildUniformCache();
    return true;
}

bool GLProgram::Link(const GLShader& vertex, const GLShader& geometry, const GLShader& fragment) {
    Destroy();
    m_handle = glCreateProgram();
    if (!m_handle) return false;

    glAttachShader(m_handle, vertex.Handle());
    glAttachShader(m_handle, geometry.Handle());
    glAttachShader(m_handle, fragment.Handle());
    glLinkProgram(m_handle);

    if (!CheckLinkStatus()) return false;
    BuildUniformCache();
    return true;
}

bool GLProgram::LinkCompute(const GLShader& compute) {
    Destroy();
    m_handle = glCreateProgram();
    if (!m_handle) return false;

    glAttachShader(m_handle, compute.Handle());
    glLinkProgram(m_handle);

    if (!CheckLinkStatus()) return false;
    BuildUniformCache();
    return true;
}

bool GLProgram::LinkFromStages(const std::vector<const GLShader*>& shaders) {
    Destroy();
    m_handle = glCreateProgram();
    if (!m_handle) return false;

    for (auto* shader : shaders) {
        if (shader && shader->IsValid()) {
            glAttachShader(m_handle, shader->Handle());
        }
    }
    glLinkProgram(m_handle);

    if (!CheckLinkStatus()) return false;
    BuildUniformCache();
    return true;
}

bool GLProgram::BuildFromSource(const std::string& vertSrc, const std::string& fragSrc) {
    GLShader vert, frag;
    if (!vert.CompileFromSource(ShaderStage::Vertex, vertSrc)) {
        m_infoLog = "Vertex: " + vert.GetInfoLog();
        return false;
    }
    if (!frag.CompileFromSource(ShaderStage::Fragment, fragSrc)) {
        m_infoLog = "Fragment: " + frag.GetInfoLog();
        return false;
    }
    return Link(vert, frag);
}

bool GLProgram::BuildComputeFromSource(const std::string& src) {
    GLShader cs;
    if (!cs.CompileFromSource(ShaderStage::Compute, src)) {
        m_infoLog = "Compute: " + cs.GetInfoLog();
        return false;
    }
    return LinkCompute(cs);
}

bool GLProgram::BuildFromSourceWithDefines(
    const std::string& vertSrc, const std::string& fragSrc,
    const std::vector<std::pair<std::string, std::string>>& defines) {

    ShaderPreprocessor preprocessor;
    for (auto& [name, val] : defines) {
        preprocessor.AddDefine(name, val);
    }

    std::string processedVert = preprocessor.Process(vertSrc);
    std::string processedFrag = preprocessor.Process(fragSrc);

    return BuildFromSource(processedVert, processedFrag);
}

void GLProgram::Destroy() {
    if (m_handle) {
        glDeleteProgram(m_handle);
        m_handle = 0;
    }
    m_infoLog.clear();
    m_uniformCache.clear();
    m_reflection = ProgramReflection{};
}

void GLProgram::Use() const {
    if (m_handle) glUseProgram(m_handle);
}

void GLProgram::UseNone() {
    glUseProgram(0);
}

bool GLProgram::CheckLinkStatus() {
    GLint status = 0;
    glGetProgramiv(m_handle, GL_LINK_STATUS, &status);

    GLint logLen = 0;
    glGetProgramiv(m_handle, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 1) {
        m_infoLog.resize(logLen);
        glGetProgramInfoLog(m_handle, logLen, nullptr, m_infoLog.data());
    }

    if (!status) {
        glDeleteProgram(m_handle);
        m_handle = 0;
        return false;
    }
    return true;
}

void GLProgram::BuildUniformCache() {
    m_uniformCache.clear();
    if (!m_handle) return;

    GLint count = 0;
    glGetProgramiv(m_handle, GL_ACTIVE_UNIFORMS, &count);

    char nameBuffer[256];
    for (int i = 0; i < count; ++i) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(m_handle, i, sizeof(nameBuffer) - 1, &length, &size, &type, nameBuffer);
        nameBuffer[length] = '\0';

        int32_t loc = glGetUniformLocation(m_handle, nameBuffer);
        if (loc >= 0) {
            m_uniformCache[nameBuffer] = loc;
        }

        // Handle arrays: uniform[0] -> uniform
        std::string name(nameBuffer);
        auto bracket = name.find('[');
        if (bracket != std::string::npos) {
            std::string baseName = name.substr(0, bracket);
            if (m_uniformCache.find(baseName) == m_uniformCache.end()) {
                m_uniformCache[baseName] = loc;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  Reflection
// ═══════════════════════════════════════════════════════════════

void GLProgram::Reflect() {
    m_reflection = ProgramReflection{};
    if (!m_handle) return;

    // Uniforms
    GLint uniCount = 0;
    glGetProgramiv(m_handle, GL_ACTIVE_UNIFORMS, &uniCount);
    char nameBuffer[256];

    for (int i = 0; i < uniCount; ++i) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(m_handle, i, sizeof(nameBuffer) - 1, &length, &size, &type, nameBuffer);
        nameBuffer[length] = '\0';

        UniformInfo info;
        info.name = nameBuffer;
        info.location = glGetUniformLocation(m_handle, nameBuffer);
        info.arraySize = size;

        // Map GL type to our enum
        switch (type) {
        case GL_FLOAT:          info.type = UniformType::Float; break;
        case GL_FLOAT_VEC2:     info.type = UniformType::Vec2; break;
        case GL_FLOAT_VEC3:     info.type = UniformType::Vec3; break;
        case GL_FLOAT_VEC4:     info.type = UniformType::Vec4; break;
        case GL_INT:            info.type = UniformType::Int; break;
        case GL_INT_VEC2:       info.type = UniformType::IVec2; break;
        case GL_INT_VEC3:       info.type = UniformType::IVec3; break;
        case GL_INT_VEC4:       info.type = UniformType::IVec4; break;
        case GL_UNSIGNED_INT:   info.type = UniformType::UInt; break;
        case GL_BOOL:           info.type = UniformType::Bool; break;
        case GL_FLOAT_MAT2:     info.type = UniformType::Mat2; break;
        case GL_FLOAT_MAT3:     info.type = UniformType::Mat3; break;
        case GL_FLOAT_MAT4:     info.type = UniformType::Mat4; break;
        case GL_SAMPLER_2D:     info.type = UniformType::Sampler2D; break;
        case GL_SAMPLER_3D:     info.type = UniformType::Sampler3D; break;
        case GL_SAMPLER_CUBE:   info.type = UniformType::SamplerCube; break;
        case GL_IMAGE_2D:       info.type = UniformType::Image2D; break;
        default:                info.type = UniformType::Float; break;
        }

        m_reflection.uniforms.push_back(info);
    }

    // Uniform Blocks
    GLint blockCount = 0;
    glGetProgramiv(m_handle, GL_ACTIVE_UNIFORM_BLOCKS, &blockCount);

    for (int i = 0; i < blockCount; ++i) {
        UniformBlockInfo block;
        block.index = static_cast<uint32_t>(i);

        GLsizei nameLen = 0;
        glGetActiveUniformBlockName(m_handle, i, sizeof(nameBuffer) - 1, &nameLen, nameBuffer);
        nameBuffer[nameLen] = '\0';
        block.name = nameBuffer;

        GLint bSize = 0;
        glGetActiveUniformBlockiv(m_handle, i, GL_UNIFORM_BLOCK_DATA_SIZE, &bSize);
        block.size = static_cast<uint32_t>(bSize);

        GLint binding = 0;
        glGetActiveUniformBlockiv(m_handle, i, GL_UNIFORM_BLOCK_BINDING, &binding);
        block.binding = static_cast<uint32_t>(binding);

        // Get member uniforms
        GLint memberCount = 0;
        glGetActiveUniformBlockiv(m_handle, i, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &memberCount);
        std::vector<GLint> memberIndices(memberCount);
        glGetActiveUniformBlockiv(m_handle, i, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, memberIndices.data());

        for (int j = 0; j < memberCount; ++j) {
            GLint idx = memberIndices[j];
            GLsizei mLen = 0;
            GLint mSize = 0;
            GLenum mType = 0;
            glGetActiveUniform(m_handle, idx, sizeof(nameBuffer) - 1, &mLen, &mSize, &mType, nameBuffer);

            UniformInfo member;
            member.name = nameBuffer;
            member.arraySize = mSize;

            GLint offset = 0;
            glGetActiveUniformsiv(m_handle, 1, reinterpret_cast<GLuint*>(&idx), GL_UNIFORM_OFFSET, &offset);
            member.offset = offset;
            member.blockIndex = i;
            member.location = -1;

            block.members.push_back(member);
        }

        m_reflection.uniformBlocks.push_back(block);
    }

    // Attributes
    GLint attrCount = 0;
    glGetProgramiv(m_handle, GL_ACTIVE_ATTRIBUTES, &attrCount);

    for (int i = 0; i < attrCount; ++i) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib(m_handle, i, sizeof(nameBuffer) - 1, &length, &size, &type, nameBuffer);
        nameBuffer[length] = '\0';

        AttributeInfo attr;
        attr.name = nameBuffer;
        attr.location = glGetAttribLocation(m_handle, nameBuffer);
        attr.type = type;
        attr.size = size;
        m_reflection.attributes.push_back(attr);
    }

    // SSBOs (GL 4.3+)
    GLint ssboCount = 0;
    glGetProgramInterfaceiv(m_handle, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &ssboCount);

    for (int i = 0; i < ssboCount; ++i) {
        SSBOInfo ssbo;
        ssbo.index = static_cast<uint32_t>(i);

        GLsizei nameLen = 0;
        glGetProgramResourceName(m_handle, GL_SHADER_STORAGE_BLOCK, i, sizeof(nameBuffer) - 1, &nameLen, nameBuffer);
        nameBuffer[nameLen] = '\0';
        ssbo.name = nameBuffer;

        GLenum props[] = { GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE };
        GLint values[2] = {0, 0};
        glGetProgramResourceiv(m_handle, GL_SHADER_STORAGE_BLOCK, i, 2, props, 2, nullptr, values);
        ssbo.binding = static_cast<uint32_t>(values[0]);
        ssbo.size = static_cast<uint32_t>(values[1]);

        m_reflection.ssbos.push_back(ssbo);
    }

    // Compute work group size
    glGetProgramiv(m_handle, GL_COMPUTE_WORK_GROUP_SIZE, reinterpret_cast<GLint*>(m_reflection.computeWorkGroupSize));
}

// ═══════════════════════════════════════════════════════════════
//  Uniform Queries
// ═══════════════════════════════════════════════════════════════

int32_t GLProgram::GetUniformLocation(const std::string& name) const {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) return it->second;
    if (!m_handle) return -1;

    int32_t loc = glGetUniformLocation(m_handle, name.c_str());
    m_uniformCache[name] = loc;
    return loc;
}

int32_t GLProgram::GetUniformLocation(const char* name) const {
    return GetUniformLocation(std::string(name));
}

int32_t GLProgram::GetUniformBlockIndex(const std::string& name) const {
    if (!m_handle) return -1;
    GLuint idx = glGetUniformBlockIndex(m_handle, name.c_str());
    return (idx == GL_INVALID_INDEX) ? -1 : static_cast<int32_t>(idx);
}

int32_t GLProgram::GetSSBOIndex(const std::string& name) const {
    if (!m_handle) return -1;
    GLuint idx = glGetProgramResourceIndex(m_handle, GL_SHADER_STORAGE_BLOCK, name.c_str());
    return (idx == GL_INVALID_INDEX) ? -1 : static_cast<int32_t>(idx);
}

int32_t GLProgram::GetAttribLocation(const std::string& name) const {
    if (!m_handle) return -1;
    return glGetAttribLocation(m_handle, name.c_str());
}

// ═══════════════════════════════════════════════════════════════
//  Uniform Setters (by location)
// ═══════════════════════════════════════════════════════════════

void GLProgram::SetFloat(int32_t loc, float v) const {
    if (loc >= 0) glProgramUniform1f(m_handle, loc, v);
}

void GLProgram::SetVec2(int32_t loc, float x, float y) const {
    if (loc >= 0) glProgramUniform2f(m_handle, loc, x, y);
}

void GLProgram::SetVec3(int32_t loc, float x, float y, float z) const {
    if (loc >= 0) glProgramUniform3f(m_handle, loc, x, y, z);
}

void GLProgram::SetVec4(int32_t loc, float x, float y, float z, float w) const {
    if (loc >= 0) glProgramUniform4f(m_handle, loc, x, y, z, w);
}

void GLProgram::SetVec4(int32_t loc, const float* v) const {
    if (loc >= 0 && v) glProgramUniform4fv(m_handle, loc, 1, v);
}

void GLProgram::SetInt(int32_t loc, int32_t v) const {
    if (loc >= 0) glProgramUniform1i(m_handle, loc, v);
}

void GLProgram::SetUInt(int32_t loc, uint32_t v) const {
    if (loc >= 0) glProgramUniform1ui(m_handle, loc, v);
}

void GLProgram::SetIVec2(int32_t loc, int32_t x, int32_t y) const {
    if (loc >= 0) glProgramUniform2i(m_handle, loc, x, y);
}

void GLProgram::SetIVec3(int32_t loc, int32_t x, int32_t y, int32_t z) const {
    if (loc >= 0) glProgramUniform3i(m_handle, loc, x, y, z);
}

void GLProgram::SetIVec4(int32_t loc, int32_t x, int32_t y, int32_t z, int32_t w) const {
    if (loc >= 0) glProgramUniform4i(m_handle, loc, x, y, z, w);
}

void GLProgram::SetBool(int32_t loc, bool v) const {
    if (loc >= 0) glProgramUniform1i(m_handle, loc, v ? 1 : 0);
}

void GLProgram::SetMat3(int32_t loc, const float* m, bool transpose) const {
    if (loc >= 0 && m) glProgramUniformMatrix3fv(m_handle, loc, 1, transpose ? GL_TRUE : GL_FALSE, m);
}

void GLProgram::SetMat4(int32_t loc, const float* m, bool transpose) const {
    if (loc >= 0 && m) glProgramUniformMatrix4fv(m_handle, loc, 1, transpose ? GL_TRUE : GL_FALSE, m);
}

void GLProgram::SetMat4Array(int32_t loc, const float* m, int count, bool transpose) const {
    if (loc >= 0 && m) glProgramUniformMatrix4fv(m_handle, loc, count, transpose ? GL_TRUE : GL_FALSE, m);
}

void GLProgram::SetFloatArray(int32_t loc, const float* v, int count) const {
    if (loc >= 0 && v) glProgramUniform1fv(m_handle, loc, count, v);
}

void GLProgram::SetIntArray(int32_t loc, const int32_t* v, int count) const {
    if (loc >= 0 && v) glProgramUniform1iv(m_handle, loc, count, v);
}

// ═══════════════════════════════════════════════════════════════
//  Uniform Setters (by name — convenience wrappers)
// ═══════════════════════════════════════════════════════════════

void GLProgram::SetFloat(const std::string& name, float v) const {
    SetFloat(GetUniformLocation(name), v);
}

void GLProgram::SetVec2(const std::string& name, float x, float y) const {
    SetVec2(GetUniformLocation(name), x, y);
}

void GLProgram::SetVec3(const std::string& name, float x, float y, float z) const {
    SetVec3(GetUniformLocation(name), x, y, z);
}

void GLProgram::SetVec4(const std::string& name, float x, float y, float z, float w) const {
    SetVec4(GetUniformLocation(name), x, y, z, w);
}

void GLProgram::SetInt(const std::string& name, int32_t v) const {
    SetInt(GetUniformLocation(name), v);
}

void GLProgram::SetBool(const std::string& name, bool v) const {
    SetBool(GetUniformLocation(name), v);
}

void GLProgram::SetMat4(const std::string& name, const float* m, bool transpose) const {
    SetMat4(GetUniformLocation(name), m, transpose);
}

// ═══════════════════════════════════════════════════════════════
//  Block Binding
// ═══════════════════════════════════════════════════════════════

void GLProgram::BindUniformBlock(uint32_t blockIndex, uint32_t bindingPoint) const {
    if (m_handle) glUniformBlockBinding(m_handle, blockIndex, bindingPoint);
}

void GLProgram::BindUniformBlock(const std::string& name, uint32_t bindingPoint) const {
    int32_t idx = GetUniformBlockIndex(name);
    if (idx >= 0) BindUniformBlock(static_cast<uint32_t>(idx), bindingPoint);
}

void GLProgram::BindSSBO(uint32_t index, uint32_t bindingPoint) const {
    if (m_handle) glShaderStorageBlockBinding(m_handle, index, bindingPoint);
}

void GLProgram::BindSSBO(const std::string& name, uint32_t bindingPoint) const {
    int32_t idx = GetSSBOIndex(name);
    if (idx >= 0) BindSSBO(static_cast<uint32_t>(idx), bindingPoint);
}

// ═══════════════════════════════════════════════════════════════
//  Compute Dispatch
// ═══════════════════════════════════════════════════════════════

void GLProgram::Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const {
    Use();
    glDispatchCompute(groupsX, groupsY, groupsZ);
}

void GLProgram::DispatchForSize(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) const {
    if (!m_handle) return;
    uint32_t wgSize[3];
    std::memcpy(wgSize, m_reflection.computeWorkGroupSize, sizeof(wgSize));
    if (!wgSize[0]) wgSize[0] = 1;
    if (!wgSize[1]) wgSize[1] = 1;
    if (!wgSize[2]) wgSize[2] = 1;

    uint32_t gx = (sizeX + wgSize[0] - 1) / wgSize[0];
    uint32_t gy = (sizeY + wgSize[1] - 1) / wgSize[1];
    uint32_t gz = (sizeZ + wgSize[2] - 1) / wgSize[2];
    Dispatch(gx, gy, gz);
}

void GLProgram::MemoryBarrier(uint32_t barriers) const {
    glMemoryBarrier(barriers);
}

// ═══════════════════════════════════════════════════════════════
//  Binary Cache
// ═══════════════════════════════════════════════════════════════

bool GLProgram::GetBinary(std::vector<uint8_t>& outData, uint32_t& outFormat) const {
    if (!m_handle) return false;

    GLint binLength = 0;
    glGetProgramiv(m_handle, GL_PROGRAM_BINARY_LENGTH, &binLength);
    if (binLength <= 0) return false;

    outData.resize(binLength);
    GLenum format = 0;
    glGetProgramBinary(m_handle, binLength, nullptr, &format, outData.data());
    outFormat = static_cast<uint32_t>(format);
    return true;
}

bool GLProgram::LoadBinary(const std::vector<uint8_t>& data, uint32_t format) {
    Destroy();
    m_handle = glCreateProgram();
    if (!m_handle) return false;

    glProgramBinary(m_handle, static_cast<GLenum>(format), data.data(), static_cast<GLsizei>(data.size()));

    GLint status = 0;
    glGetProgramiv(m_handle, GL_LINK_STATUS, &status);
    if (!status) {
        glDeleteProgram(m_handle);
        m_handle = 0;
        return false;
    }

    BuildUniformCache();
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Label
// ═══════════════════════════════════════════════════════════════

void GLProgram::SetLabel(const std::string& label) {
    if (m_handle) {
        glObjectLabel(GL_PROGRAM, m_handle, static_cast<GLsizei>(label.size()), label.c_str());
    }
}

// ═══════════════════════════════════════════════════════════════
//  Debug Dump
// ═══════════════════════════════════════════════════════════════

std::string GLProgram::DumpUniforms() const {
    std::ostringstream ss;
    ss << "Uniforms (" << m_reflection.uniforms.size() << "):\n";
    for (auto& u : m_reflection.uniforms) {
        ss << "  [" << u.location << "] " << u.name;
        if (u.arraySize > 1) ss << "[" << u.arraySize << "]";
        ss << "\n";
    }
    return ss.str();
}

std::string GLProgram::DumpAttributes() const {
    std::ostringstream ss;
    ss << "Attributes (" << m_reflection.attributes.size() << "):\n";
    for (auto& a : m_reflection.attributes) {
        ss << "  [" << a.location << "] " << a.name << "\n";
    }
    return ss.str();
}

std::string GLProgram::DumpBlocks() const {
    std::ostringstream ss;
    ss << "Uniform Blocks (" << m_reflection.uniformBlocks.size() << "):\n";
    for (auto& b : m_reflection.uniformBlocks) {
        ss << "  Block \"" << b.name << "\" binding=" << b.binding << " size=" << b.size << "\n";
        for (auto& m : b.members) {
            ss << "    " << m.name << " offset=" << m.offset << "\n";
        }
    }
    ss << "SSBOs (" << m_reflection.ssbos.size() << "):\n";
    for (auto& s : m_reflection.ssbos) {
        ss << "  SSBO \"" << s.name << "\" binding=" << s.binding << " size=" << s.size << "\n";
    }
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
//  GLProgramPipeline
// ═══════════════════════════════════════════════════════════════

GLProgramPipeline::~GLProgramPipeline() { Destroy(); }

GLProgramPipeline::GLProgramPipeline(GLProgramPipeline&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = 0;
}

GLProgramPipeline& GLProgramPipeline::operator=(GLProgramPipeline&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_handle = other.m_handle;
        other.m_handle = 0;
    }
    return *this;
}

bool GLProgramPipeline::Create() {
    Destroy();
    glCreateProgramPipelines(1, &m_handle);
    return m_handle != 0;
}

void GLProgramPipeline::Destroy() {
    if (m_handle) {
        glDeleteProgramPipelines(1, &m_handle);
        m_handle = 0;
    }
}

void GLProgramPipeline::SetVertexStage(uint32_t program) {
    if (m_handle) glUseProgramStages(m_handle, GL_VERTEX_SHADER_BIT, program);
}

void GLProgramPipeline::SetFragmentStage(uint32_t program) {
    if (m_handle) glUseProgramStages(m_handle, GL_FRAGMENT_SHADER_BIT, program);
}

void GLProgramPipeline::SetGeometryStage(uint32_t program) {
    if (m_handle) glUseProgramStages(m_handle, GL_GEOMETRY_SHADER_BIT, program);
}

void GLProgramPipeline::SetComputeStage(uint32_t program) {
    if (m_handle) glUseProgramStages(m_handle, GL_COMPUTE_SHADER_BIT, program);
}

void GLProgramPipeline::Bind() const {
    if (m_handle) glBindProgramPipeline(m_handle);
}

void GLProgramPipeline::Unbind() {
    glBindProgramPipeline(0);
}

bool GLProgramPipeline::Validate() const {
    if (!m_handle) return false;
    glValidateProgramPipeline(m_handle);
    GLint status = 0;
    glGetProgramPipelineiv(m_handle, GL_VALIDATE_STATUS, &status);
    return status != 0;
}

void GLProgramPipeline::SetLabel(const std::string& label) {
    if (m_handle) {
        glObjectLabel(GL_PROGRAM_PIPELINE, m_handle, static_cast<GLsizei>(label.size()), label.c_str());
    }
}

// ═══════════════════════════════════════════════════════════════
//  ShaderIncludeManager
// ═══════════════════════════════════════════════════════════════

void ShaderIncludeManager::RegisterInclude(const std::string& path, const std::string& source) {
    m_includes[path] = source;
}

void ShaderIncludeManager::UnregisterInclude(const std::string& path) {
    m_includes.erase(path);
}

void ShaderIncludeManager::ClearAll() {
    m_includes.clear();
}

bool ShaderIncludeManager::HasInclude(const std::string& path) const {
    return m_includes.count(path) > 0;
}

const std::string& ShaderIncludeManager::GetInclude(const std::string& path) const {
    auto it = m_includes.find(path);
    if (it != m_includes.end()) return it->second;
    return m_empty;
}

std::string ShaderIncludeManager::PreprocessIncludes(const std::string& source, int maxDepth) const {
    return ResolveIncludes(source, 0, maxDepth);
}

std::string ShaderIncludeManager::ResolveIncludes(const std::string& src, int depth, int maxDepth) const {
    if (depth >= maxDepth) return src;

    std::istringstream stream(src);
    std::ostringstream result;
    std::string line;

    while (std::getline(stream, line)) {
        // Check for #include "path" or #include <path>
        size_t pos = line.find("#include");
        if (pos != std::string::npos) {
            size_t start = line.find_first_of("\"<", pos + 8);
            size_t end = std::string::npos;
            if (start != std::string::npos) {
                char closeChar = (line[start] == '"') ? '"' : '>';
                end = line.find(closeChar, start + 1);
            }

            if (start != std::string::npos && end != std::string::npos) {
                std::string includePath = line.substr(start + 1, end - start - 1);
                auto it = m_includes.find(includePath);
                if (it != m_includes.end()) {
                    result << "// [include begin: " << includePath << "]\n";
                    result << ResolveIncludes(it->second, depth + 1, maxDepth);
                    result << "\n// [include end: " << includePath << "]\n";
                    continue;
                }
                result << "// [include not found: " << includePath << "]\n";
                continue;
            }
        }
        result << line << "\n";
    }
    return result.str();
}

// ═══════════════════════════════════════════════════════════════
//  ShaderPreprocessor
// ═══════════════════════════════════════════════════════════════

void ShaderPreprocessor::SetVersion(int version, const std::string& profile) {
    m_version = version;
    m_profile = profile;
}

void ShaderPreprocessor::AddDefine(const std::string& name, const std::string& value) {
    for (auto& [n, v] : m_defines) {
        if (n == name) { v = value; return; }
    }
    m_defines.emplace_back(name, value);
}

void ShaderPreprocessor::AddDefine(const std::string& name, int value) {
    AddDefine(name, std::to_string(value));
}

void ShaderPreprocessor::AddDefine(const std::string& name, float value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", value);
    AddDefine(name, std::string(buf));
}

void ShaderPreprocessor::RemoveDefine(const std::string& name) {
    m_defines.erase(
        std::remove_if(m_defines.begin(), m_defines.end(),
                        [&](auto& p) { return p.first == name; }),
        m_defines.end());
}

void ShaderPreprocessor::ClearDefines() {
    m_defines.clear();
}

std::string ShaderPreprocessor::Process(const std::string& source) const {
    std::ostringstream result;

    // Find existing #version directive
    std::istringstream input(source);
    std::string line;
    bool hasVersion = false;

    // First pass: check if #version exists
    while (std::getline(input, line)) {
        if (line.find("#version") != std::string::npos) {
            hasVersion = true;
            break;
        }
    }

    // Reset stream
    input.clear();
    input.str(source);

    if (!hasVersion) {
        result << "#version " << m_version << " " << m_profile << "\n";
        // Add defines right after
        for (auto& [name, val] : m_defines) {
            result << "#define " << name;
            if (!val.empty()) result << " " << val;
            result << "\n";
        }
    }

    bool insertedDefines = hasVersion ? false : true;

    while (std::getline(input, line)) {
        result << line << "\n";
        if (!insertedDefines && line.find("#version") != std::string::npos) {
            for (auto& [name, val] : m_defines) {
                result << "#define " << name;
                if (!val.empty()) result << " " << val;
                result << "\n";
            }
            insertedDefines = true;
        }
    }

    std::string processed = result.str();

    // Resolve includes if manager is set
    if (m_includes) {
        processed = m_includes->PreprocessIncludes(processed);
    }

    return processed;
}

void ShaderPreprocessor::SetN64Defines(bool twoPoint, bool fog, bool noise, bool lod) {
    if (twoPoint) AddDefine("N64_TWO_CYCLE", "1");
    else RemoveDefine("N64_TWO_CYCLE");

    if (fog) AddDefine("N64_FOG", "1");
    else RemoveDefine("N64_FOG");

    if (noise) AddDefine("N64_NOISE", "1");
    else RemoveDefine("N64_NOISE");

    if (lod) AddDefine("N64_LOD", "1");
    else RemoveDefine("N64_LOD");
}

void ShaderPreprocessor::SetBackendDefines(bool bindless, bool dsa, bool multiDraw) {
    if (bindless) AddDefine("USE_BINDLESS_TEXTURES", "1");
    else RemoveDefine("USE_BINDLESS_TEXTURES");

    if (dsa) AddDefine("USE_DSA", "1");
    else RemoveDefine("USE_DSA");

    if (multiDraw) AddDefine("USE_MULTI_DRAW_INDIRECT", "1");
    else RemoveDefine("USE_MULTI_DRAW_INDIRECT");
}

// ═══════════════════════════════════════════════════════════════
//  ShaderHotReloader
// ═══════════════════════════════════════════════════════════════

void ShaderHotReloader::AddWatch(const std::string& path, ShaderStage stage) {
    ShaderFileEntry entry;
    entry.path = path;
    entry.stage = stage;
    entry.lastWriteTime = 0;

    try {
        auto ftime = std::filesystem::last_write_time(path);
        entry.lastWriteTime = static_cast<uint64_t>(ftime.time_since_epoch().count());
    } catch (...) {}

    m_watches.push_back(entry);
}

void ShaderHotReloader::RemoveWatch(const std::string& path) {
    m_watches.erase(
        std::remove_if(m_watches.begin(), m_watches.end(),
                        [&](auto& e) { return e.path == path; }),
        m_watches.end());
}

void ShaderHotReloader::ClearAll() {
    m_watches.clear();
}

std::vector<std::string> ShaderHotReloader::CheckForChanges() {
    std::vector<std::string> changed;
    if (!m_enabled) return changed;

    for (auto& entry : m_watches) {
        try {
            auto ftime = std::filesystem::last_write_time(entry.path);
            uint64_t newTime = static_cast<uint64_t>(ftime.time_since_epoch().count());
            if (newTime != entry.lastWriteTime) {
                entry.lastWriteTime = newTime;
                changed.push_back(entry.path);
            }
        } catch (...) {}
    }
    return changed;
}

bool ShaderHotReloader::ReloadShader(GLShader& shader, const std::string& path) {
    // Find the entry for the path to get the shader stage
    ShaderStage stage = ShaderStage::Vertex;
    for (auto& e : m_watches) {
        if (e.path == path) { stage = e.stage; break; }
    }

    // Read file
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    std::string source((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    return shader.CompileFromSource(stage, source);
}

} // namespace Nova64::Video::GL
