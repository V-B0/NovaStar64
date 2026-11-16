/**
 * @file GLShaderCompiler.hpp
 * @brief OpenGL implementation of IShaderCompiler.
 */
// NovaGraphics — OpenGL implementation of IShaderCompiler
// Delegates to GLShader / GLProgram for actual GL compilation.

#pragma once

#include "../ShaderManager.hpp"
#include "GLShaderProgram.hpp"

#include <string>
#include <unordered_map>

namespace Nova64::Video::GL {

class GLShaderCompiler final : public Shader::IShaderCompiler {
public:
    GLShaderCompiler() = default;
    ~GLShaderCompiler() override;

    bool Initialize(Shader::BackendAPI api) override;
    void Shutdown() override;

    Shader::CompiledShader Compile(const Shader::CombinerKey& key, const std::string& vertexSource,
                                   const std::string& fragmentSource) override;

    Shader::CompiledShader CompileWithGeometry(const Shader::CombinerKey& key, const std::string& vertexSource,
                                               const std::string& fragmentSource,
                                               const std::string& geometrySource) override;

    void DeleteShader(Shader::CompiledShader& shader) override;
    void BindShader(const Shader::CompiledShader& shader) override;

    std::string GetLastError() const override { return m_lastError; }

    int GetUniformLocation(Shader::ShaderHandle program, const char* name) override;

    void SetUniform1i(int location, int value) override;
    void SetUniform1f(int location, float value) override;
    void SetUniform2f(int location, float x, float y) override;
    void SetUniform3f(int location, float x, float y, float z) override;
    void SetUniform4f(int location, float r, float g, float b, float a) override;
    void SetUniformMatrix4fv(int location, const float* matrix) override;

    Shader::BackendAPI GetAPI() const override { return Shader::BackendAPI::OpenGL; }
    Shader::ShaderLanguage GetLanguage() const override { return Shader::ShaderLanguage::GLSL_460; }

private:
    std::string m_lastError;

    // Store GLProgram objects keyed by their GL handle so we can manage lifetimes
    std::unordered_map<uint32_t, std::unique_ptr<GLProgram>> m_programs;

    [[maybe_unused]] Shader::ShaderHandle m_nextHandle = 1;
    Shader::ShaderHandle m_boundProgram = Shader::INVALID_SHADER;
};

} // namespace Nova64::Video::GL
