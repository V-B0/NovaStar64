/**
 * @file VKShader.hpp
 * @brief Real Vulkan IShaderProgram — GLSL → SPIR-V via glslang, then VkShaderModule.
 *
 * Stage 3 scope:
 *   * Compile vertex/fragment/compute GLSL strings to SPIR-V at runtime.
 *   * Create VkShaderModule per stage.
 *   * Uniform setters are stored client-side in a small map; they're applied
 *     by the pipeline through a default UBO in Stage 4 (currently no-op).
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "../Renderer.hpp"

#include <unordered_map>
#include <vector>

namespace Nova64 { namespace Video { namespace VK {

/// Initialize / shut down glslang once per process. Reference-counted.
void GlslangInitProcess();
void GlslangFinalizeProcess();

enum class ShaderStage : uint8_t { Vertex, Fragment, Geometry, Compute };

/// Compile a GLSL source string of `stage` to SPIR-V. Returns empty on failure
/// (errors are logged via Logger). `debugName` is used purely for log messages.
std::vector<uint32_t> CompileGLSLtoSPIRV(ShaderStage stage,
                                         const std::string& source,
                                         const char* debugName);

class VKShaderProgramImpl final : public IShaderProgram {
public:
    explicit VKShaderProgramImpl(const VKContext* ctx);
    ~VKShaderProgramImpl() override;

    // IShaderProgram
    bool Compile(const std::string& vertexSrc,
                 const std::string& fragmentSrc,
                 const std::string& geometrySrc = "") override;
    bool CompileCompute(const std::string& computeSrc) override;
    void Bind() override   {}
    void Unbind() override {}

    void SetInt   (const char* name, int v)                 override;
    void SetFloat (const char* name, float v)               override;
    void SetVec2  (const char* name, float x, float y)      override;
    void SetVec3  (const char* name, float x, float y, float z)            override;
    void SetVec4  (const char* name, float x, float y, float z, float w)   override;
    void SetMat4  (const char* name, const float* data)     override;

    int      GetUniformLocation(const char* name) const override;
    uint64_t GetNativeHandle()                    const override { return 0; }
    const std::string& GetInfoLog()               const override { return m_log; }

    // VK accessors
    VkShaderModule VS() const { return m_vs; }
    VkShaderModule FS() const { return m_fs; }
    VkShaderModule GS() const { return m_gs; }
    VkShaderModule CS() const { return m_cs; }
    bool HasGeometry() const { return m_gs != VK_NULL_HANDLE; }
    bool IsCompute()   const { return m_cs != VK_NULL_HANDLE; }

private:
    bool CreateModule(const std::vector<uint32_t>& spirv, VkShaderModule& outMod);
    void DestroyModules();

    const VKContext* m_ctx;
    VkShaderModule   m_vs = VK_NULL_HANDLE;
    VkShaderModule   m_fs = VK_NULL_HANDLE;
    VkShaderModule   m_gs = VK_NULL_HANDLE;
    VkShaderModule   m_cs = VK_NULL_HANDLE;
    std::string      m_log;
    // Cheap CPU-side uniform store (consumed by pipeline UBO in Stage 4).
    std::unordered_map<std::string, std::vector<float>> m_uniforms;
};

}}} // namespace Nova64::Video::VK
