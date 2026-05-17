/**
 * @file VKShader.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKShader.hpp"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <atomic>
#include <cstring>

namespace Nova64 { namespace Video { namespace VK {

namespace {

std::atomic<int> g_glslangRefcount{0};

EShLanguage ToEShLang(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex:        return EShLangVertex;
        case ShaderStage::Fragment:      return EShLangFragment;
        case ShaderStage::Geometry:      return EShLangGeometry;
        case ShaderStage::Compute:       return EShLangCompute;
        case ShaderStage::RayGen:        return EShLangRayGen;
        case ShaderStage::RayMiss:       return EShLangMiss;
        case ShaderStage::RayClosestHit: return EShLangClosestHit;
    }
    return EShLangVertex;
}

const char* StageName(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex:        return "vertex";
        case ShaderStage::Fragment:      return "fragment";
        case ShaderStage::Geometry:      return "geometry";
        case ShaderStage::Compute:       return "compute";
        case ShaderStage::RayGen:        return "raygen";
        case ShaderStage::RayMiss:       return "miss";
        case ShaderStage::RayClosestHit: return "closesthit";
    }
    return "?";
}

} // anonymous

void GlslangInitProcess() {
    if (g_glslangRefcount.fetch_add(1) == 0) {
        glslang::InitializeProcess();
    }
}

void GlslangFinalizeProcess() {
    if (g_glslangRefcount.fetch_sub(1) == 1) {
        glslang::FinalizeProcess();
    }
}

std::vector<uint32_t> CompileGLSLtoSPIRV(ShaderStage stage,
                                         const std::string& source,
                                         const char* debugName) {
    if (source.empty()) return {};

    const EShLanguage lang = ToEShLang(stage);
    glslang::TShader shader(lang);

    const char* src = source.c_str();
    shader.setStrings(&src, 1);
    // RT stages require Vulkan 1.2 / SPIR-V 1.4 minimum
    const bool isRT = (stage == ShaderStage::RayGen ||
                       stage == ShaderStage::RayMiss ||
                       stage == ShaderStage::RayClosestHit);
    const auto vkTarget  = isRT ? glslang::EShTargetVulkan_1_2 : glslang::EShTargetVulkan_1_1;
    const auto spvTarget = isRT ? glslang::EShTargetSpv_1_4    : glslang::EShTargetSpv_1_3;
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, vkTarget);
    shader.setEnvTarget(glslang::EShTargetSpv,    spvTarget);

    const TBuiltInResource* res = GetDefaultResources();
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(res, 100, false, messages)) {
        Logger::Error("glslang parse failed [{} {}]:\n{}\n{}",
                      debugName ? debugName : "", StageName(stage),
                      shader.getInfoLog(), shader.getInfoDebugLog());
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        Logger::Error("glslang link failed [{} {}]:\n{}\n{}",
                      debugName ? debugName : "", StageName(stage),
                      program.getInfoLog(), program.getInfoDebugLog());
        return {};
    }

    std::vector<uint32_t> spirv;
    glslang::SpvOptions opts{};
    opts.disableOptimizer = true; // ENABLE_OPT=OFF in CMake — keep consistent
    opts.validate         = false;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &opts);
    if (spirv.empty()) {
        Logger::Error("glslang produced empty SPIR-V [{} {}]",
                      debugName ? debugName : "", StageName(stage));
    }
    return spirv;
}

// ─────────────────────────────────────────────────────────────────────────────

VKShaderProgramImpl::VKShaderProgramImpl(const VKContext* ctx) : m_ctx(ctx) {
    GlslangInitProcess();
}

VKShaderProgramImpl::~VKShaderProgramImpl() {
    DestroyModules();
    GlslangFinalizeProcess();
}

bool VKShaderProgramImpl::CreateModule(const std::vector<uint32_t>& spirv,
                                       VkShaderModule& outMod) {
    if (spirv.empty()) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * sizeof(uint32_t);
    mi.pCode    = spirv.data();
    if (vkCreateShaderModule(m_ctx->device, &mi, nullptr, &outMod) != VK_SUCCESS) {
        Logger::Error("VKShader: vkCreateShaderModule failed");
        return false;
    }
    return true;
}

void VKShaderProgramImpl::DestroyModules() {
    if (!m_ctx || !m_ctx->device) return;
    auto destroy = [&](VkShaderModule& m){
        if (m) { vkDestroyShaderModule(m_ctx->device, m, nullptr); m = VK_NULL_HANDLE; }
    };
    destroy(m_vs); destroy(m_fs); destroy(m_gs); destroy(m_cs);
}

bool VKShaderProgramImpl::Compile(const std::string& vertexSrc,
                                  const std::string& fragmentSrc,
                                  const std::string& geometrySrc) {
    DestroyModules();
    m_log.clear();

    auto vSpv = CompileGLSLtoSPIRV(ShaderStage::Vertex,   vertexSrc,   "VKShader");
    auto fSpv = CompileGLSLtoSPIRV(ShaderStage::Fragment, fragmentSrc, "VKShader");
    if (vSpv.empty() || fSpv.empty()) {
        m_log = "vertex or fragment compilation failed";
        return false;
    }
    if (!CreateModule(vSpv, m_vs)) return false;
    if (!CreateModule(fSpv, m_fs)) return false;
    if (!geometrySrc.empty()) {
        auto gSpv = CompileGLSLtoSPIRV(ShaderStage::Geometry, geometrySrc, "VKShader");
        if (!gSpv.empty() && !CreateModule(gSpv, m_gs)) return false;
    }
    return true;
}

bool VKShaderProgramImpl::CompileCompute(const std::string& computeSrc) {
    DestroyModules();
    m_log.clear();
    auto spv = CompileGLSLtoSPIRV(ShaderStage::Compute, computeSrc, "VKShader");
    if (spv.empty()) { m_log = "compute compilation failed"; return false; }
    return CreateModule(spv, m_cs);
}

void VKShaderProgramImpl::SetInt(const char* name, int v) {
    m_uniforms[name] = { static_cast<float>(v) };
}
void VKShaderProgramImpl::SetFloat(const char* name, float v) {
    m_uniforms[name] = { v };
}
void VKShaderProgramImpl::SetVec2(const char* name, float x, float y) {
    m_uniforms[name] = { x, y };
}
void VKShaderProgramImpl::SetVec3(const char* name, float x, float y, float z) {
    m_uniforms[name] = { x, y, z };
}
void VKShaderProgramImpl::SetVec4(const char* name, float x, float y, float z, float w) {
    m_uniforms[name] = { x, y, z, w };
}
void VKShaderProgramImpl::SetMat4(const char* name, const float* data) {
    if (!data) return;
    auto& v = m_uniforms[name];
    v.assign(data, data + 16);
}
int VKShaderProgramImpl::GetUniformLocation(const char* name) const {
    // Vulkan has no glGet*Location; treat the hash as a stable token.
    return static_cast<int>(std::hash<std::string>{}(name) & 0x7FFFFFFF);
}

}}} // namespace Nova64::Video::VK
