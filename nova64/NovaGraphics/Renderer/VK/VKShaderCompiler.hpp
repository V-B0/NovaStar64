/**
 * @file VKShaderCompiler.hpp
 * @brief Vulkan IShaderCompiler — compiles GLSL→SPIR-V, writes uniforms to
 *        the scratch UBO exposed by VKBackend::GetMappedUBOPtr().
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "../ShaderManager.hpp"
#include "../Renderer.hpp"

#include <unordered_map>
#include <string>
#include <memory>
#include <cstdint>
#include <cstring>

namespace Nova64 {
namespace Video {
namespace VK {

// --- Fixed byte offsets of every uniform inside the N64 UBO ---
// Must match the std140 layout declared in ShaderGenerator::GetCommonUniforms()
// for ShaderLanguage::SPIRV.
//
//  Each 16-byte row:
//    [0]    mat4  uMVPMatrix
//    [64]   mat4  uModelView
//    [128]  mat4  uProjection
//    [192]  vec4  uPrimColor
//    [208]  vec4  uEnvColor
//    [224]  vec4  uFogColor
//    [240]  vec4  uBlendColor
//    [256]  vec4  uFillColor
//    [272]  vec4  uCenterColor  (xyz used)
//    [288]  vec4  uScaleColor   (xyz used)
//    [304]  vec4  uConvertK
//    [320]  vec4  uTexClamp0
//    [336]  vec4  uTexClamp1
//    [352]  vec4  uTexCoordBounds0
//    [368]  vec4  uTexCoordBounds1
//    [384]  vec4  uKeyWidth_MinLod  xyz=uKeyWidth  w=uMinLod
//    [400]  vec4  uTexScale01       xy=scale0 zw=scale1
//    [416]  vec4  uTexOffset01      xy=off0   zw=off1
//    [432]  vec4  uTexMask01        xy=mask0  zw=mask1
//    [448]  vec4  uTexSize01        xy=size0  zw=size1
//    [464]  vec4  uGPUScale01       xy=gpu0   zw=gpu1
//    [480]  vec4  uScreenSizes      xy=screenSz  zw=invScreenSz
//    [496]  vec4  uScreenScaleResScale  xy=screenScale  zw=resScale
//    [512]  vec4  uPrimLODs_K4K5   x=primLOD y=primLODFrac z=K4 w=K5
//    [528]  vec4  uFogParams        x=alphaRef y=fogMult z=fogOff w=primDepth
//    [544]  vec4  uDepthParams      x=depthSc  y=depthOff z=primDeltaZ w=0
//    [560]  ivec4 uIntBits0         x=frameCount y=zMode z=useTexCoordBounds w=texPersp
//    [576]  ivec4 uIntBits1         x=fbMono0 y=fbMono1 z=fbFixAlpha0 w=fbFixAlpha1
//    [592]  ivec4 uIntBits2         x=fogUsage y=sst z=texDetail w=maxTile
//    [608]  ivec4 uIntBits3         x=debugOutput y=texCvgAlpha z=texHiRes w=0
//  Total: 624 bytes

static constexpr size_t kN64UBOSize = 624;

namespace N64UBOOff {
constexpr int uMVPMatrix = 0;
constexpr int uModelView = 64;
constexpr int uProjection = 128;
constexpr int uPrimColor = 192;
constexpr int uEnvColor = 208;
constexpr int uFogColor = 224;
constexpr int uBlendColor = 240;
constexpr int uFillColor = 256;
constexpr int uCenterColor = 272; // vec3: write 12 bytes
constexpr int uScaleColor = 288;  // vec3: write 12 bytes
constexpr int uConvertK = 304;
constexpr int uTexClamp0 = 320;
constexpr int uTexClamp1 = 336;
constexpr int uTexCoordBounds0 = 352;
constexpr int uTexCoordBounds1 = 368;
constexpr int uKeyWidth = 384; // vec3 xyz of uKeyWidth_MinLod
constexpr int uMinLod = 396;   // w of uKeyWidth_MinLod
constexpr int uTexScale0 = 400;
constexpr int uTexScale1 = 408;
constexpr int uTexOffset0 = 416;
constexpr int uTexOffset1 = 424;
constexpr int uTexMask0 = 432;
constexpr int uTexMask1 = 440;
constexpr int uTexSize0 = 448;
constexpr int uTexSize1 = 456;
constexpr int uGPUScale0 = 464;
constexpr int uGPUScale1 = 472;
constexpr int uScreenSize = 480;
constexpr int uInvScreenSize = 488;
constexpr int uScreenScale = 496;
constexpr int uResolutionScale = 504;
constexpr int uPrimLOD = 512;
constexpr int uPrimLODFrac = 516;
constexpr int uK4 = 520;
constexpr int uK5 = 524;
constexpr int uAlphaRef = 528;
constexpr int uFogMultiplier = 532;
constexpr int uFogOffset = 536;
constexpr int uPrimDepth = 540;
constexpr int uDepthScale = 544;
constexpr int uDepthOffset = 548;
constexpr int uPrimDeltaZ = 552;
constexpr int uFrameCount = 560;
constexpr int uZMode = 564;
constexpr int uUseTexCoordBounds = 568;
constexpr int uTexturePersp = 572;
constexpr int uFbMonochrome0 = 576;
constexpr int uFbMonochrome1 = 580;
constexpr int uFbFixedAlpha0 = 584;
constexpr int uFbFixedAlpha1 = 588;
constexpr int uFogUsage = 592;
constexpr int uScreenSpaceTriangle = 596;
constexpr int uTextureDetail = 600;
constexpr int uMaxTile = 604;
constexpr int uDebugOutput = 608;
constexpr int uTexCvgAlpha = 612; // uIntBits3.y: when 1, multiply combined.a by texel0.a
constexpr int uTexHiRes = 616;    // uIntBits3.z: bit 0 = texture 0, bit 1 = texture 1 is a texture-pack picture
} // namespace N64UBOOff

class VKShaderCompiler final : public Shader::IShaderCompiler {
public:
    explicit VKShaderCompiler(IRenderer* renderer);
    ~VKShaderCompiler() override;

    // IShaderCompiler
    bool Initialize(Shader::BackendAPI api) override;
    void Shutdown() override;

    Shader::CompiledShader Compile(const Shader::CombinerKey& key, const std::string& vertSrc,
                                   const std::string& fragSrc) override;

    Shader::CompiledShader CompileWithGeometry(const Shader::CombinerKey& key, const std::string& vertSrc,
                                               const std::string& fragSrc, const std::string& geomSrc) override;

    void DeleteShader(Shader::CompiledShader& shader) override;

    // Binds the stored VKShaderProgramImpl on the renderer.
    void BindShader(const Shader::CompiledShader& shader) override;

    std::string GetLastError() const override { return m_lastError; }

    int GetUniformLocation(Shader::ShaderHandle program, const char* name) override;

    // Uniform setters — write directly to the current frame's scratch UBO.
    void SetUniform1i(int loc, int v) override;
    void SetUniform1f(int loc, float v) override;
    void SetUniform2f(int loc, float x, float y) override;
    void SetUniform3f(int loc, float x, float y, float z) override;
    void SetUniform4f(int loc, float r, float g, float b, float a) override;
    void SetUniformMatrix4fv(int loc, const float* m) override;

    Shader::BackendAPI GetAPI() const override { return Shader::BackendAPI::Vulkan; }
    Shader::ShaderLanguage GetLanguage() const override { return Shader::ShaderLanguage::SPIRV; }

private:
    // Returns a pointer to byte `offset` inside the current frame's UBO,
    // or nullptr if the UBO is not yet mapped.
    uint8_t* UBOPtr(int offset);

    IRenderer* m_renderer = nullptr;
    std::string m_lastError;

    // All compiled VK shader programs, keyed by the handle stored in CompiledShader.
    // Value is IShaderProgram* (VKShaderProgramImpl under the hood).
    std::unordered_map<Shader::ShaderHandle, std::unique_ptr<IShaderProgram>> m_programs;
};

} // namespace VK
} // namespace Video
} // namespace Nova64
