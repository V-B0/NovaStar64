/**
 * @file GLShaderProgram.cpp
 * @brief OpenGL Shader Program.
 */

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

// --- Stage Helpers ---

uint32_t ShaderStageToGL(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:      return GL_VERTEX_SHADER;
    case ShaderStage::Fragment:    return GL_FRAGMENT_SHADER;
    case ShaderStage::Geometry:    return GL_GEOMETRY_SHADER;
    case ShaderStage::TessControl: return GL_TESS_CONTROL_SHADER;
    case ShaderStage::TessEval:    return GL_TESS_EVALUATION_SHADER;
    case ShaderStage::Compute:     return GL_COMPUTE_SHADER;
    }
    return GL_VERTEX_SHADER;
}

// --- GLShader ---

GLShader::~GLShader() { Destroy(); }

GLShader::GLShader(GLShader&& other) noexcept :
    m_handle(other.m_handle), m_stage(other.m_stage), m_infoLog(std::move(other.m_infoLog)),
    m_source(std::move(other.m_source)) {
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
    const GLint length = static_cast<GLint>(source.size());
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

void GLShader::Destroy() {
    if (m_handle) {
        glDeleteShader(m_handle);
        m_handle = 0;
    }
    m_infoLog.clear();
}

// --- GLProgram ---

GLProgram::~GLProgram() { Destroy(); }

GLProgram::GLProgram(GLProgram&& other) noexcept :
    m_handle(other.m_handle), m_infoLog(std::move(other.m_infoLog)), m_reflection(std::move(other.m_reflection)),
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

// --- Reflection ---

// --- Uniform Queries ---

int32_t GLProgram::GetUniformLocation(const std::string& name) const {
    auto it = m_uniformCache.find(name);
    if (it != m_uniformCache.end()) return it->second;
    if (!m_handle) return -1;

    int32_t loc = glGetUniformLocation(m_handle, name.c_str());
    m_uniformCache[name] = loc;
    return loc;
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

// --- Uniform Setters (by location) ---

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

void GLProgram::SetInt(int32_t loc, int32_t v) const {
    if (loc >= 0) glProgramUniform1i(m_handle, loc, v);
}

void GLProgram::SetBool(int32_t loc, bool v) const {
    if (loc >= 0) glProgramUniform1i(m_handle, loc, v ? 1 : 0);
}

void GLProgram::SetMat4(int32_t loc, const float* m, bool transpose) const {
    if (loc >= 0 && m) glProgramUniformMatrix4fv(m_handle, loc, 1, transpose ? GL_TRUE : GL_FALSE, m);
}

// --- Uniform Setters (by name — convenience wrappers) ---

// --- Block Binding ---

void GLProgram::BindUniformBlock(uint32_t blockIndex, uint32_t bindingPoint) const {
    if (m_handle) glUniformBlockBinding(m_handle, blockIndex, bindingPoint);
}

void GLProgram::BindSSBO(uint32_t index, uint32_t bindingPoint) const {
    if (m_handle) glShaderStorageBlockBinding(m_handle, index, bindingPoint);
}

// --- Compute Dispatch ---

void GLProgram::Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const {
    Use();
    glDispatchCompute(groupsX, groupsY, groupsZ);
}

// --- Binary Cache ---

// --- Label ---

// --- Debug Dump ---

// --- GLProgramPipeline ---

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

// --- ShaderIncludeManager ---

// --- ShaderPreprocessor ---

void ShaderPreprocessor::AddDefine(const std::string& name, const std::string& value) {
    for (auto& [n, v] : m_defines) {
        if (n == name) {
            v = value;
            return;
        }
    }
    m_defines.emplace_back(name, value);
}

void ShaderPreprocessor::RemoveDefine(const std::string& name) {
    m_defines.erase(std::remove_if(m_defines.begin(), m_defines.end(), [&](auto& p) { return p.first == name; }),
                    m_defines.end());
}

// --- ShaderHotReloader ---

} // namespace Nova64::Video::GL
