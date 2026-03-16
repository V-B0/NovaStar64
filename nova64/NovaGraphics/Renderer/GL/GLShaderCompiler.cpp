// NovaGraphics — OpenGL implementation of IShaderCompiler
// Uses GLShader / GLProgram (which wrap glCreateShader / glCreateProgram).

#include "GLShaderCompiler.hpp"

#include <glad/gl.h>
#include <cstring>
#include <cstdio>

namespace Nova64::Video::GL {

GLShaderCompiler::~GLShaderCompiler() { Shutdown(); }

bool GLShaderCompiler::Initialize(Shader::BackendAPI api) {
    if (api != Shader::BackendAPI::OpenGL) {
        m_lastError = "GLShaderCompiler only supports OpenGL backend";
        return false;
    }
    return true;
}

void GLShaderCompiler::Shutdown() {
    // GLProgram destructors will call glDeleteProgram
    m_programs.clear();
    m_boundProgram = Shader::INVALID_SHADER;
}

// ─── Compile vertex + fragment ───────────────────────────────────────────────

Shader::CompiledShader GLShaderCompiler::Compile(
    const Shader::CombinerKey& key,
    const std::string& vertexSource,
    const std::string& fragmentSource)
{
    Shader::CompiledShader result;
    result.key = key;
    result.features = key.features;

    auto prog = std::make_unique<GLProgram>();
    if (!prog->BuildFromSource(vertexSource, fragmentSource)) {
        m_lastError = prog->GetInfoLog();
        result.valid = false;
        // Always log compile failures
        std::fprintf(stderr, "[SHADER-FAIL] feat=0x%llX link-log: %s\n",
            (unsigned long long)key.features, prog->GetInfoLog().c_str());
        return result;
    }

    uint32_t glHandle = prog->Handle();
    result.handle = static_cast<Shader::ShaderHandle>(glHandle);
    result.valid = true;
    result.vertexSource = vertexSource;
    result.fragmentSource = fragmentSource;

    m_programs[glHandle] = std::move(prog);
    return result;
}

Shader::CompiledShader GLShaderCompiler::CompileWithGeometry(
    const Shader::CombinerKey& key,
    const std::string& vertexSource,
    const std::string& fragmentSource,
    const std::string& geometrySource)
{
    Shader::CompiledShader result;
    result.key = key;
    result.features = key.features;

    GLShader vert, geom, frag;
    if (!vert.CompileFromSource(ShaderStage::Vertex, vertexSource)) {
        m_lastError = "Vertex: " + vert.GetInfoLog();
        result.valid = false;
        return result;
    }
    if (!geom.CompileFromSource(ShaderStage::Geometry, geometrySource)) {
        m_lastError = "Geometry: " + geom.GetInfoLog();
        result.valid = false;
        return result;
    }
    if (!frag.CompileFromSource(ShaderStage::Fragment, fragmentSource)) {
        m_lastError = "Fragment: " + frag.GetInfoLog();
        result.valid = false;
        return result;
    }

    auto prog = std::make_unique<GLProgram>();
    if (!prog->Link(vert, geom, frag)) {
        m_lastError = "Link: " + prog->GetInfoLog();
        result.valid = false;
        return result;
    }

    uint32_t glHandle = prog->Handle();
    result.handle = static_cast<Shader::ShaderHandle>(glHandle);
    result.valid = true;
    result.vertexSource = vertexSource;
    result.fragmentSource = fragmentSource;

    m_programs[glHandle] = std::move(prog);
    return result;
}

// ─── Shader lifecycle ────────────────────────────────────────────────────────

void GLShaderCompiler::DeleteShader(Shader::CompiledShader& shader) {
    if (shader.handle == Shader::INVALID_SHADER) return;
    uint32_t glHandle = static_cast<uint32_t>(shader.handle);
    m_programs.erase(glHandle);
    shader.handle = Shader::INVALID_SHADER;
    shader.valid = false;
}

void GLShaderCompiler::BindShader(const Shader::CompiledShader& shader) {
    if (!shader.valid) return;
    uint32_t glHandle = static_cast<uint32_t>(shader.handle);
    // Always call glUseProgram — external code may change the program
    // without updating m_boundProgram.
    glUseProgram(glHandle);
    m_boundProgram = shader.handle;
}

// ─── Uniform queries ─────────────────────────────────────────────────────────

int GLShaderCompiler::GetUniformLocation(Shader::ShaderHandle program, const char* name) {
    return glGetUniformLocation(static_cast<GLuint>(program), name);
}

void GLShaderCompiler::SetUniform1i(int loc, int value) {
    if (loc >= 0) glUniform1i(loc, value);
}

void GLShaderCompiler::SetUniform1f(int loc, float value) {
    if (loc >= 0) glUniform1f(loc, value);
}

void GLShaderCompiler::SetUniform2f(int loc, float x, float y) {
    if (loc >= 0) glUniform2f(loc, x, y);
}

void GLShaderCompiler::SetUniform3f(int loc, float x, float y, float z) {
    if (loc >= 0) glUniform3f(loc, x, y, z);
}

void GLShaderCompiler::SetUniform4f(int loc, float r, float g, float b, float a) {
    if (loc >= 0) glUniform4f(loc, r, g, b, a);
}

void GLShaderCompiler::SetUniformMatrix4fv(int loc, const float* matrix) {
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, matrix);
}

} // namespace Nova64::Video::GL
