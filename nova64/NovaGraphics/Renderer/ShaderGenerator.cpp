/**
 * @file ShaderGenerator.cpp
 * @brief Shader Generator.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Shader Generator Implementation
// Generates GLSL/HLSL/SPIRV shader source code for N64 combiner configurations

#include "ShaderManager.hpp"
#include <sstream>
#include <cassert>

namespace Nova64 { namespace Video { namespace Shader {

// ═══════════════════════════════════════════════════════════════════════════════
// CONSTRUCTION / DESTRUCTION
// ═══════════════════════════════════════════════════════════════════════════════

ShaderGenerator::ShaderGenerator()
    : m_language(ShaderLanguage::GLSL_460)
{}

ShaderGenerator::~ShaderGenerator() = default;

void ShaderGenerator::Initialize(ShaderLanguage language) {
    m_language = language;
}

void ShaderGenerator::Shutdown() {
    // Nothing to clean up currently
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: VERSION DIRECTIVE
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GetVersionDirective() const {
    switch (m_language) {
        case ShaderLanguage::GLSL_460: return "#version 460 core\n";
        case ShaderLanguage::GLSL_450: return "#version 450 core\n";
        case ShaderLanguage::HLSL_SM5: return ""; // HLSL doesn't use a version directive
        case ShaderLanguage::SPIRV:    return "#version 460 core\n"; // GLSL for SPIR-V cross-compile
        default:                       return "#version 460 core\n";
    }
}

std::string ShaderGenerator::GetHLSLCompatPreamble() const {
    // GLSL → HLSL compatibility defines.
    // This lets the combiner/blender code (which uses GLSL syntax) compile
    // unchanged as HLSL. The combiner tables emit vec4/vec3/texture().
    return
        "// GLSL → HLSL compatibility\n"
        "#define vec2 float2\n"
        "#define vec3 float3\n"
        "#define vec4 float4\n"
        "#define mat4 float4x4\n"
        "#define mat3 float3x3\n"
        "#define mix lerp\n"
        "#define fract frac\n"
        "#define mod fmod\n"
        "#define dFdx ddx\n"
        "#define dFdy ddy\n"
        "#define ivec2 int2\n"
        "#define uvec4 uint4\n"
        "float4 texture(Texture2D t, SamplerState s, float2 uv) { return t.Sample(s, uv); }\n"
        "#define texture(sampler, uv) sampler.Sample(sLinear, uv)\n"
        "#define textureLod(sampler, uv, lod) sampler.SampleLevel(sLinear, uv, lod)\n"
        "#define gl_FragCoord input.position\n"
        "#define discard clip(-1)\n"
        "\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: COMMON UNIFORMS
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GetCommonUniforms() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "cbuffer NovaConstants : register(b0) {\n"
            "    float4x4 uMVPMatrix;\n"
            "    float4x4 uModelView;\n"
            "    float4x4 uProjection;\n"
            "    float4 uPrimColor;\n"
            "    float4 uEnvColor;\n"
            "    float4 uFogColor;\n"
            "    float4 uBlendColor;\n"
            "    float4 uFillColor;\n"
            "    float  uPrimLOD;\n"
            "    float  uPrimLODFrac;\n"
            "    float  uK4;\n"
            "    float  uK5;\n"
            "    float4 uConvertK;\n"
            "    float2 uTexScale0;\n"
            "    float2 uTexScale1;\n"
            "    float2 uTexOffset0;\n"
            "    float2 uTexOffset1;\n"
            "    float4 uTexClamp0;\n"
            "    float4 uTexClamp1;\n"
            "    float2 uTexMask0;\n"
            "    float2 uTexMask1;\n"
            "    float2 uTexSize0;\n"
            "    float2 uTexSize1;\n"
            "    float  uAlphaRef;\n"
            "    float  uFogMultiplier;\n"
            "    float  uFogOffset;\n"
            "    float  uDepthScale;\n"
            "    float  uDepthOffset;\n"
            "    float  uPrimDepth;\n"
            "    float2 uScreenSize;\n"
            "    float2 uInvScreenSize;\n"
            "    int    uFrameCount;\n"
            "    float  uPrimDeltaZ;\n"
            "    float2 uResolutionScale;\n"
            "    float3 uCenterColor;\n"
            "    float  _pad1;\n"
            "    float3 uScaleColor;\n"
            "    float  _pad2;\n"
            "    float2 uGPUScale0;\n"
            "    float2 uGPUScale1;\n"
            "    float2 uScreenScale;\n"
            "    float3 uKeyWidth;\n"
            "    float  _pad3;\n"
            "    float  uMinLod;\n"
            "    int    uMaxTile;\n"
            "    int    uTextureDetail;\n"
            "    int    uFogUsage;\n"
            "    int    uScreenSpaceTriangle;\n"
            "    int    uTexturePersp;\n"
            "    int    uFbMonochrome0;\n"
            "    int    uFbMonochrome1;\n"
            "    int    uFbFixedAlpha0;\n"
            "    int    uFbFixedAlpha1;\n"
            "    int    uUseTexCoordBounds;\n"
            "    int    uZMode;\n"
            "    float  _pad3;\n"
            "    float4 uTexCoordBounds0;\n"
            "    float4 uTexCoordBounds1;\n"
            "};\n";
    }
    return
        "uniform mat4  uMVPMatrix;\n"
        "uniform mat4  uModelView;\n"
        "uniform mat4  uProjection;\n"
        "\n"
        "uniform vec4  uPrimColor;\n"
        "uniform vec4  uEnvColor;\n"
        "uniform vec4  uFogColor;\n"
        "uniform vec4  uBlendColor;\n"
        "uniform vec4  uFillColor;\n"
        "uniform vec3  uCenterColor;\n"
        "uniform vec3  uScaleColor;\n"
        "uniform float uPrimLOD;\n"
        "uniform float uPrimLODFrac;\n"
        "uniform float uK4;\n"
        "uniform float uK5;\n"
        "uniform vec4  uConvertK;\n"
        "\n"
        "uniform vec2  uTexScale0;\n"
        "uniform vec2  uTexScale1;\n"
        "uniform vec2  uTexOffset0;\n"
        "uniform vec2  uTexOffset1;\n"
        "uniform vec4  uTexClamp0;\n"
        "uniform vec4  uTexClamp1;\n"
        "uniform vec2  uTexMask0;\n"
        "uniform vec2  uTexMask1;\n"
        "uniform vec2  uTexSize0;\n"
        "uniform vec2  uTexSize1;\n"
        "uniform vec2  uGPUScale0;\n"
        "uniform vec2  uGPUScale1;\n"
        "\n"
        "uniform float uAlphaRef;\n"
        "uniform float uFogMultiplier;\n"
        "uniform float uFogOffset;\n"
        "uniform float uDepthScale;\n"
        "uniform float uDepthOffset;\n"
        "uniform float uPrimDepth;\n"
        "uniform vec2  uScreenSize;\n"
        "uniform vec2  uInvScreenSize;\n"
        "uniform int   uFrameCount;\n"
        "uniform int   uZMode;\n"
        "uniform float uPrimDeltaZ;\n"
        "uniform vec4  uTexCoordBounds0;\n"
        "uniform vec4  uTexCoordBounds1;\n"
        "uniform int   uUseTexCoordBounds;\n"
        "uniform int   uTexturePersp;\n"
        "uniform int   uFbMonochrome0;\n"
        "uniform int   uFbMonochrome1;\n"
        "uniform int   uFbFixedAlpha0;\n"
        "uniform int   uFbFixedAlpha1;\n"
        "uniform vec2  uScreenScale;\n"
        "uniform vec3  uKeyWidth;\n"
        "uniform float uMinLod;\n"
        "uniform int   uMaxTile;\n"
        "uniform int   uTextureDetail;\n"
        "uniform int   uFogUsage;\n"
        "uniform int   uScreenSpaceTriangle;\n"
        "uniform vec2  uResolutionScale;\n"
        "uniform int   uDebugOutput;\n";
}

std::string ShaderGenerator::GetTextureUniforms() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "Texture2D    tTexture0     : register(t0);\n"
            "Texture2D    tTexture1     : register(t1);\n"
            "Texture2D    tTLUT0        : register(t2);\n"
            "Texture2D    tTLUT1        : register(t3);\n"
            "Texture2D    tDepthTexture : register(t4);\n"
            "Texture2D    tNoiseTexture : register(t5);\n"
            "Texture2D    tDitherTexture: register(t6);\n"
            "Texture2D    tFBTexture    : register(t7);\n"
            "SamplerState sLinear       : register(s0);\n"
            "SamplerState sPoint        : register(s1);\n"
            "\n"
            "// GLSL texture() equivalent macros\n"
            "#define texture2D_0(uv)  tTexture0.Sample(sLinear, uv)\n"
            "#define texture2D_1(uv)  tTexture1.Sample(sLinear, uv)\n"
            "#define texture2D_fb(uv) tFBTexture.Sample(sLinear, uv)\n"
            "#define texture2D_depth(uv) tDepthTexture.Sample(sPoint, uv)\n"
            "#define texture2D_noise(uv) tNoiseTexture.Sample(sPoint, uv)\n"
            "#define texture2D_dither(uv) tDitherTexture.Sample(sPoint, uv)\n";
    }
    return
        "layout(binding = 0) uniform sampler2D uTexture0;\n"
        "layout(binding = 1) uniform sampler2D uTexture1;\n"
        "layout(binding = 2) uniform sampler2D uTLUT0;\n"
        "layout(binding = 3) uniform sampler2D uTLUT1;\n"
        "layout(binding = 4) uniform sampler2D uDepthTexture;\n"
        "layout(binding = 5) uniform sampler2D uNoiseTexture;\n"
        "layout(binding = 6) uniform sampler2D uDitherTexture;\n"
        "layout(binding = 7) uniform sampler2D uFBTexture;\n"
        "layout(binding = 8) uniform sampler2D uZImageTex;\n"
        "layout(binding = 9) uniform sampler2D uDeltaZImageTex;\n"
        "layout(binding = 10) uniform sampler2D uZLUT;\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: INPUTS / OUTPUTS
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GetVertexInputs() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "struct VSInput {\n"
            "    float4 aPosition  : POSITION;\n"
            "    float4 aColor     : COLOR;\n"
            "    float2 aTexCoord0 : TEXCOORD0;\n"
            "    float2 aTexCoord1 : TEXCOORD1;\n"
            "    float  aFog       : FOG;\n"
            "    float  aClipW     : TEXCOORD2;\n"
            "    float2 aBaryCoord : TEXCOORD3;\n"
            "};\n";
    }
    return
        "layout(location = 0) in vec4 aPosition;\n"
        "layout(location = 1) in vec4 aColor;\n"
        "layout(location = 2) in vec2 aTexCoord0;\n"
        "layout(location = 3) in vec2 aTexCoord1;\n"
        "layout(location = 4) in float aFog;\n"
        "layout(location = 5) in float aClipW;\n"
        "layout(location = 6) in vec2 aBaryCoord;\n";
}

std::string ShaderGenerator::GetVertexOutputs() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "struct VSOutput {\n"
            "    float4 position  : SV_Position;\n"
            "    float4 vColor    : COLOR;\n"
            "    noperspective float4 vColorNP : COLOR1;\n"
            "    float2 vTexCoord0: TEXCOORD0;\n"
            "    float2 vTexCoord1: TEXCOORD1;\n"
            "    float  vFog      : FOG;\n"
            "    float  vDepth    : DEPTH_INTERP;\n"
            "    float  vClipW    : TEXCOORD2;\n"
            "    float4 vBaryCoord: TEXCOORD3;\n"
            "};\n";
    }
    return
        "out vec4 vColor;\n"
        "noperspective out vec4 vColorNP;\n"
        "out vec2 vTexCoord0;\n"
        "out vec2 vTexCoord1;\n"
        "out float vFog;\n"
        "out float vDepth;\n"
        "out float vClipW;\n"
        "out vec4 vBaryCoord;\n";
}

std::string ShaderGenerator::GetFragmentInputs() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "struct PSInput {\n"
            "    float4 position  : SV_Position;\n"
            "    float4 vColor    : COLOR;\n"
            "    noperspective float4 vColorNP : COLOR1;\n"
            "    float2 vTexCoord0: TEXCOORD0;\n"
            "    float2 vTexCoord1: TEXCOORD1;\n"
            "    float  vFog      : FOG;\n"
            "    float  vDepth    : DEPTH_INTERP;\n"
            "    float  vClipW    : TEXCOORD2;\n"
            "    float4 vBaryCoord: TEXCOORD3;\n"
            "};\n";
    }
    return
        "in vec4 vColor;\n"
        "noperspective in vec4 vColorNP;\n"
        "in vec2 vTexCoord0;\n"
        "in vec2 vTexCoord1;\n"
        "in float vFog;\n"
        "in float vDepth;\n"
        "in float vClipW;\n"
        "in vec4 vBaryCoord;\n";
}

std::string ShaderGenerator::GetFragmentOutputs(uint64_t features) const {
    bool dualSrc = (features & SF_DUAL_SOURCE_BLEND) != 0;
    if (m_language == ShaderLanguage::HLSL_SM5) {
        std::string s = "struct PSOutput {\n"
                        "    float4 fragColor : SV_Target0;\n";
        if (dualSrc)
            s += "    float4 fragColor1 : SV_Target1;\n";
        if (features & (SF_DEPTH_SOURCE_PRIM | SF_DEPTH_RENDER))
            s += "    float  fragDepth : SV_Depth;\n";
        s += "};\n";
        return s;
    }
    std::string s = "layout(location = 0) out vec4 fragColor;\n";
    if (dualSrc)
        s += "layout(location = 0, index = 1) out vec4 fragColor1;\n";
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// N64 COLOR COMBINER + BLENDER CODE GENERATION
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateCombinerCode(uint64_t mux, uint64_t features) const {
    // ── N64 Color Combiner: (A - B) * C + D  (RGB and Alpha) ──
    // The 64-bit mux word encodes input selectors for both cycles.

    // Input expression tables (adapted to ShaderGenerator naming)
    static const char* const kRGBInputA[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "shadeColor.rgb", "uEnvColor.rgb", "vec3(1.0)", "vec3(noise)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"
    };
    static const char* const kRGBInputB[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "shadeColor.rgb", "uEnvColor.rgb", "uCenterColor.rgb", "vec3(uK4)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"
    };
    static const char* const kRGBInputC[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "shadeColor.rgb", "uEnvColor.rgb", "uScaleColor.rgb", "vec3(combined.a)",
        "vec3(texel0.a)", "vec3(texel1.a)", "vec3(uPrimColor.a)", "vec3(shadeColor.a)",
        "vec3(uEnvColor.a)", "vec3(lodFrac)", "vec3(uPrimLODFrac)", "vec3(uK5)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"
    };
    static const char* const kRGBInputD[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "shadeColor.rgb", "uEnvColor.rgb", "vec3(1.0)", "vec3(0.0)"
    };
    static const char* const kAlphaABD[] = {
        "combined.a", "texel0.a", "texel1.a", "uPrimColor.a",
        "shadeColor.a", "uEnvColor.a", "1.0", "0.0"
    };
    static const char* const kAlphaC[] = {
        "lodFrac", "texel0.a", "texel1.a", "uPrimColor.a",
        "shadeColor.a", "uEnvColor.a", "uPrimLODFrac", "0.0"
    };

    uint32_t hi = static_cast<uint32_t>(mux >> 32);
    uint32_t lo = static_cast<uint32_t>(mux & 0xFFFFFFFF);

    std::ostringstream ss;

    // texel1 / noise defaults (texel0 is declared at the top of main())
    if (!(features & SF_TEXTURE_1))
        ss << "    vec4 texel1 = texel0;\n";
    if (!(features & SF_NOISE_ENABLE))
        ss << "    float noise = 0.0;\n";

    // N64 hardware clears the combiner pipeline to 0x80 (=0.5) before
    // cycle 0.  In 2-cycle mode, any COMBINED reference in cycle 0 thus
    // reads 0.5; in 1-cycle mode GLideN64 forces COMBINED to zero.
    if (features & SF_2CYCLE)
        ss << "    vec4 combined = vec4(0.5);\n";
    else
        ss << "    vec4 combined = vec4(0.0);\n";

    auto emitCycle = [&](int cycle) {
        // N64 SET_COMBINE (0xFC) mux encoding per libultra gbi.h:
        // Verified against EncodeCombineMode macro in src/Combiner.h.
        //
        //   hi (upper 32 bits):
        //     [23:20]=saRGB0(A) [19:15]=mRGB0(C) [14:12]=saA0(A) [11:9]=mA0(C)
        //     [8:5]=saRGB1(A)  [4:0]=mRGB1(C)
        //
        //   lo (lower 32 bits):
        //     [31:28]=sbRGB0(B) [27:24]=sbRGB1(B) [23:21]=saA1(A) [20:18]=mA1(C)
        //     [17:15]=dRGB0(D) [14:12]=bA0(B)    [11:9]=dA0(D)
        //     [8:6]=dRGB1(D)   [5:3]=bA1(B)      [2:0]=dA1(D)
        int aRGB, bRGB, cRGB, dRGB, aA, bA, cA, dA;
        if (cycle == 0) {
            aRGB = (hi >> 20) & 0xF;     // saRGB0 [23:20]
            cRGB = (hi >> 15) & 0x1F;    // mRGB0  [19:15]
            aA   = (hi >> 12) & 0x7;     // saA0   [14:12]
            cA   = (hi >>  9) & 0x7;     // mA0    [11:9]
            bRGB = (lo >> 28) & 0xF;     // sbRGB0 [31:28]
            dRGB = (lo >> 15) & 0x7;     // dRGB0  [17:15]
            bA   = (lo >> 12) & 0x7;     // bA0    [14:12]
            dA   = (lo >>  9) & 0x7;     // dA0    [11:9]
        } else {
            aRGB = (hi >>  5) & 0xF;     // saRGB1 [8:5]
            cRGB = (hi >>  0) & 0x1F;    // mRGB1  [4:0]
            bRGB = (lo >> 24) & 0xF;     // sbRGB1 [27:24]
            aA   = (lo >> 21) & 0x7;     // saA1   [23:21]
            cA   = (lo >> 18) & 0x7;     // mA1    [20:18]
            dRGB = (lo >>  6) & 0x7;     // dRGB1  [8:6]
            bA   = (lo >>  3) & 0x7;     // bA1    [5:3]
            dA   = (lo >>  0) & 0x7;     // dA1    [2:0]
        }

        // N64 2-cycle texel pipeline corrections:
        // Cycle 0: When only one texture is active, TEXEL1 hasn't been
        //          fetched yet and reads as TEXEL0.  When two textures
        //          are active (SF_TEXTURE_1), TEXEL1 IS valid tile-1 data
        //          and must NOT be remapped.
        // Cycle 1: pipeline shift swaps TEXEL0↔TEXEL1 (always).
        if (features & SF_2CYCLE) {
            bool hasTex1 = (features & SF_TEXTURE_1) != 0;
            // Swap helper for 4-bit RGB A/B and 3-bit D/alpha indices
            // where index 1=texel0, 2=texel1
            auto swapTex = [cycle, hasTex1](int idx) -> int {
                if (cycle == 0) { if (!hasTex1 && idx == 2) return 1; }
                else { if (idx == 1) return 2; if (idx == 2) return 1; }
                return idx;
            };
            // RGB C table (5-bit): additionally 8=texel0_alpha, 9=texel1_alpha
            auto swapTexC = [cycle, hasTex1](int idx) -> int {
                if (cycle == 0) { if (!hasTex1) { if (idx == 2) return 1; if (idx == 9) return 8; } }
                else { if (idx == 1) return 2; if (idx == 2) return 1;
                       if (idx == 8) return 9; if (idx == 9) return 8; }
                return idx;
            };
            aRGB = swapTex(aRGB);  bRGB = swapTex(bRGB);
            cRGB = swapTexC(cRGB); dRGB = swapTex(dRGB);
            aA = swapTex(aA); bA = swapTex(bA);
            cA = swapTex(cA); dA = swapTex(dA);
        }

        ss << "\n    // Cycle " << cycle << ": (A - B) * C + D\n";
        ss << "    combined.rgb = ("
           << kRGBInputA[aRGB & 0xF] << " - "
           << kRGBInputB[bRGB & 0xF] << ") * "
           << kRGBInputC[cRGB & 0x1F] << " + "
           << kRGBInputD[dRGB & 0x7] << ";\n";
        ss << "    combined.a = ("
           << kAlphaABD[aA & 0x7] << " - "
           << kAlphaABD[bA & 0x7] << ") * "
           << kAlphaC[cA & 0x7] << " + "
           << kAlphaABD[dA & 0x7] << ";\n";
    };

    if (features & SF_FILL_RECT) {
        ss << "    combined = uFillColor;\n";
        ss << "    float c0Alpha = combined.a;\n";
    } else if (features & SF_COPY_MODE) {
        ss << "    combined = texel0;\n";
        ss << "    float c0Alpha = combined.a;\n";
    } else {
        bool is2cycle = (features & SF_2CYCLE) != 0;
        if (is2cycle) {
            emitCycle(0);   // Cycle 0 intermediate (unclamped)

            // N64 sign extension: the combiner intermediate is a signed 9-bit
            // value.  Apply only when cycle 1 references COMBINED (index 0).
            // The C component (multiplier) wraps in [-1,1) and the
            // A/B/D components wrap in [-0.5,1.5).
            // See GLideN64 ShaderSignExtendColorC / ShaderSignExtendColorABD.
            {
                // Extract cycle 1 raw mux indices
                int c1_aRGB = (hi >> 5) & 0xF;
                int c1_bRGB = (lo >> 24) & 0xF;
                int c1_cRGB = hi & 0x1F;
                int c1_dRGB = (lo >> 6) & 0x7;
                int c1_aA   = (lo >> 21) & 0x7;
                int c1_bA   = (lo >> 3) & 0x7;
                int c1_cA   = (lo >> 18) & 0x7;
                int c1_dA   = lo & 0x7;

                // RGB: check if COMBINED (0) is used in C vs A/B/D
                bool rgbUsesC   = (c1_cRGB == 0);
                bool rgbUsesABD = (c1_dRGB == 0) ||
                    ((c1_aRGB == 0 || c1_bRGB == 0) && c1_cRGB != 31);
                if (rgbUsesC)
                    ss << "    combined.rgb = mod(combined.rgb - (-1.01), 2.02) + (-1.01);\n";
                else if (rgbUsesABD)
                    ss << "    combined.rgb = mod(combined.rgb - (-0.51), 2.02) + (-0.51);\n";

                // Alpha: check if COMBINED (0) is used in C vs A/B/D
                bool alphaUsesC   = (c1_cA == 0);
                bool alphaUsesABD = (c1_dA == 0) ||
                    ((c1_aA == 0 || c1_bA == 0) && c1_cA != 7);
                if (alphaUsesC)
                    ss << "    combined.a = mod(combined.a - (-1.01), 2.02) + (-1.01);\n";
                else if (alphaUsesABD)
                    ss << "    combined.a = mod(combined.a - (-0.51), 2.02) + (-0.51);\n";
            }

            // Save cycle 0 alpha for alpha test (GLideN64 tests alpha1 after cycle 0)
            ss << "    float c0Alpha = combined.a;\n";

            emitCycle(1);   // Cycle 1 (unclamped — we WRAP-then-clamp below)

            // CVG×Alpha threshold after cycle 1 (GLideN64: alpha2 < 0.125)
            if (features & SF_ALPHA_CVG)
                ss << "    if (combined.a < 0.125) discard;\n";
        } else {
            // In 1-cycle mode N64 hardware uses cycle 1 fields, not cycle 0.
            // COMBINED inputs read as zero (already handled by combined init).
            emitCycle(1);   // Unclamped — we WRAP-then-clamp below

            // In 1-cycle mode, c0Alpha is just the single-cycle alpha
            ss << "    float c0Alpha = combined.a;\n";

            // CVG×Alpha threshold (GLideN64: alpha1 < 0.125)
            if (features & SF_ALPHA_CVG)
                ss << "    if (combined.a < 0.125) discard;\n";
        }
        // N64 final color wrap-then-clamp: values outside [-0.5, 1.5) wrap
        // around by 2.0 before clamping to [0,1].  This matches the hardware
        // behaviour where e.g. 1.6 wraps to -0.4 then clamps to 0.
        // See GLideN64 ShaderClamp: WRAP(cmbRes, -0.51, 1.51) then clamp.
        ss << "    combined = clamp(mod(combined - (-0.51), 2.02) + (-0.51), 0.0, 1.0);\n";
    }
    return ss.str();
}

std::string ShaderGenerator::GenerateBlenderCode(uint32_t blenderBits, uint32_t otherModeL, uint64_t features) const {
    // N64 blender: out = (P * A + M * B) / (A + B)
    //
    // P/M color source table: 0=combined, 1=memColor(FB), 2=blendReg, 3=fogColor
    // A alpha source table:   0=combined.a, 1=fogColor.a, 2=shadeAlpha, 3=0.0
    // B alpha source table:   0=(1-A), 1=memColor.a, 2=1.0, 3=0.0
    //
    // Cycle 1 selectors: bits [30:31]=P1 [26:27]=A1 [22:23]=M1 [18:19]=B1
    // Cycle 2 selectors: bits [28:29]=P2 [24:25]=A2 [20:21]=M2 [16:17]=B2
    if (blenderBits == 0) return "";

    // Copy mode and fill mode bypass the blender entirely on real N64 hardware.
    if (features & (SF_COPY_MODE | SF_FILL_RECT)) return "";

    // Extract all cycle selectors
    int p1 = (blenderBits >> 30) & 0x3;
    int a1 = (blenderBits >> 26) & 0x3;
    int m1 = (blenderBits >> 22) & 0x3;
    int b1 = (blenderBits >> 18) & 0x3;
    int p2 = (blenderBits >> 28) & 0x3;
    int a2 = (blenderBits >> 24) & 0x3;
    int m2 = (blenderBits >> 20) & 0x3;
    int b2 = (blenderBits >> 16) & 0x3;

    bool is2Cycle = (features & SF_2CYCLE) != 0;
    bool forceBlend = (features & SF_FORCE_BLEND) != 0;
    // Non-MLE path has no framebuffer read (memColor = 0).  Forcing the
    // blend formula when FORCE_BL is actually 0 produces P*A/(A+B) instead
    // of plain P, darkening geometry whose combined alpha < 1.  Let the
    // real FORCE_BL flag decide; MLE already does per-pixel bypass via
    // coverage < 1.0.

    // ══════════════════════════════════════════════════════════════════════
    //  MLE BLENDER: Full shader-based read-modify-write via imageLoad/Store
    //  Uses fragment shader interlock for correct ordering.
    // ══════════════════════════════════════════════════════════════════════
    if (features & SF_MLE_BLENDER) {
        std::ostringstream ss;
        ss << "\n    // ─── MLE Blender: imageLoad/imageStore read-modify-write ───\n";
        ss << "    {\n";
        ss << "        ivec2 blendCoord = ivec2(gl_FragCoord.xy);\n";

        // The depth-compare code opens the interlock and leaves it open for
        // the MLE blender to close ONLY when both SF_DEPTH_COMPARE and
        // SF_N64_DEPTH_COMPARE are set.  Otherwise we must open it ourselves.
        bool depthWillInterlock = (features & SF_N64_DEPTH_COMPARE) &&
                                  (features & SF_DEPTH_COMPARE);
        bool needOwnInterlock = !depthWillInterlock;
        if (needOwnInterlock)
            ss << "        BEGIN_INTERLOCK();\n";

        // When combined with N64 depth compare, the depth code sets
        // n64DepthPassed = false on depth failure.  Skip the blender body
        // for failed pixels but still close the interlock at the top level
        // (Intel Iris Xe rejects endInvocationInterlockARB inside flow control).
        if (depthWillInterlock)
            ss << "        if (n64DepthPassed) {\n";

        ss << "        vec4 memColor = IMG_LOAD_F4(uColorImage, blendCoord);\n";
        ss << "        uint mem_cvg_raw = IMG_LOAD_U(uCoverageImage, blendCoord);\n";
        ss << "        float memCvgF = float(mem_cvg_raw) / 7.0;\n";

        auto pmColor = [](int s, bool isCycle2) -> const char* {
            switch (s) {
            case 0: return isCycle2 ? "blenderOut.rgb" : "combined.rgb";
            case 1: return "memColor.rgb";
            case 2: return "uBlendColor.rgb";
            case 3: return "uFogColor.rgb";
            default: return "vec3(0.0)";
            }
        };
        auto aAlpha = [](int s, bool isCycle2) -> const char* {
            switch (s) {
            case 0: return isCycle2 ? "blenderOut.a" : "combined.a";
            case 1: return "uFogColor.a";
            case 2: return "shadeColor.a";
            case 3: return "0.0";
            default: return "0.0";
            }
        };

        ss << "        // Blender cycle 1\n";
        ss << "        vec3 bP = " << pmColor(p1, false) << ";\n";
        ss << "        float bA = " << aAlpha(a1, false) << ";\n";
        ss << "        vec3 bM = " << pmColor(m1, false) << ";\n";
        switch (b1) {
        case 0:  ss << "        float bB = 1.0 - bA;\n"; break;
        case 1:  ss << "        float bB = memCvgF;\n"; break;
        case 2:  ss << "        float bB = 1.0;\n"; break;
        default: ss << "        float bB = 0.0;\n"; break;
        }

        // Per-pixel blender bypass: on real N64, when FORCE_BL is clear,
        // pixels with full coverage (cvg>=7) skip the blend and write P.
        // Only partially-covered edge pixels actually blend.
        // When AA_EN (SF_COVERAGE_ENABLE) is set, forceBlend is always true
        // at compile time so the runtime check is skipped.
        if (!is2Cycle) {
            if (forceBlend) {
                ss << "        combined.rgb = clamp((bP * bA + bM * bB) / max(bA + bB, 0.001), 0.0, 1.0);\n";
            } else {
                ss << "        if (coverage < 1.0) {\n";
                ss << "            combined.rgb = clamp((bP * bA + bM * bB) / max(bA + bB, 0.001), 0.0, 1.0);\n";
                ss << "        } else {\n";
                ss << "            combined.rgb = clamp(bP, 0.0, 1.0);\n";
                ss << "        }\n";
            }
        } else {
            ss << "        combined.rgb = clamp((bP * bA + bM * bB) / max(bA + bB, 0.001), 0.0, 1.0);\n";
            ss << "        vec4 blenderOut = vec4(combined.rgb, combined.a);\n";

            ss << "        // Blender cycle 2\n";
            ss << "        bP = " << pmColor(p2, true) << ";\n";
            ss << "        bA = " << aAlpha(a2, true) << ";\n";
            ss << "        bM = " << pmColor(m2, true) << ";\n";
            switch (b2) {
            case 0:  ss << "        bB = 1.0 - bA;\n"; break;
            case 1:  ss << "        bB = memCvgF;\n"; break;
            case 2:  ss << "        bB = 1.0;\n"; break;
            default: ss << "        bB = 0.0;\n"; break;
            }
            if (forceBlend) {
                ss << "        combined.rgb = clamp((bP * bA + bM * bB) / max(bA + bB, 0.001), 0.0, 1.0);\n";
            } else {
                ss << "        if (coverage < 1.0) {\n";
                ss << "            combined.rgb = clamp((bP * bA + bM * bB) / max(bA + bB, 0.001), 0.0, 1.0);\n";
                ss << "        } else {\n";
                ss << "            combined.rgb = clamp(bP, 0.0, 1.0);\n";
                ss << "        }\n";
            }
        }

        ss << "        // CVG_DEST logic (4-bit: 0-8)\n";
        ss << "        int cur_cvg = int(round(coverage * 7.0));\n";
        ss << "        int mem_cvg = int(mem_cvg_raw);\n";
        ss << "        int final_cvg = cur_cvg;\n";
        int cvgDst = (otherModeL >> 8) & 0x03;
        ss << "        int cvgDst = " << cvgDst << ";\n";
        ss << "        if (cvgDst == 0) {\n";
        ss << "            final_cvg = min(cur_cvg + mem_cvg, 7);\n";
        ss << "        } else if (cvgDst == 1) {\n";
        ss << "            final_cvg = (cur_cvg + mem_cvg) & 7;\n";
        ss << "        } else if (cvgDst == 2) {\n";
        ss << "            final_cvg = 7;\n";
        ss << "        } else if (cvgDst == 3) {\n";
        ss << "            final_cvg = mem_cvg;\n";
        ss << "        }\n";

        ss << "        IMG_STORE_F4(uColorImage, blendCoord, vec4(combined.rgb, combined.a));\n";
        ss << "        IMG_STORE_U(uCoverageImage, blendCoord, uint(final_cvg));\n";

        // Close the depth-guarded block if active
        if (depthWillInterlock)
            ss << "        } // end if (n64DepthPassed)\n";

        // Always close the interlock region at the top level — either we
        // opened it ourselves or the depth-compare code opened it and left
        // it for us to close.  Keeping END_INTERLOCK out of flow control
        // is required for Intel Iris Xe compatibility.
        ss << "        END_INTERLOCK();\n";

        // Discard depth-failed fragments AFTER ending the interlock
        if (depthWillInterlock)
            ss << "        if (!n64DepthPassed) discard;\n";

        ss << "    }\n";
        return ss.str();
    }

    // ══════════════════════════════════════════════════════════════════════
    //  When memColor is needed but no FB read is available, we still run
    //  the blender with memColor = vec4(0).  Skipping the blender entirely
    //  caused some surfaces (e.g. OoT title logo) to become invisible
    //  because the GL hardware blend mode didn't match the N64 intent.
    //  GLideN64 likewise keeps the blender code and treats memColor as 0.
    // ══════════════════════════════════════════════════════════════════════

    // ══════════════════════════════════════════════════════════════════════
    //  HLE BLENDER: Legacy dual-source / GL hardware blend path
    // ══════════════════════════════════════════════════════════════════════
    bool dualSource = (features & SF_DUAL_SOURCE_BLEND) != 0;

    std::ostringstream preamble;
    preamble << "\n    // Shader blender\n";
    preamble << "    vec4 memColor = vec4(0.0);\n";
    if (features & SF_FRAMEBUFFER_READ) {
        preamble << "    vec2 fbUV = gl_FragCoord.xy * uInvScreenSize;\n";
        preamble << "    memColor = texture(uFBTexture, fbUV);\n";
    }

    // P/M color sources (N64 hardware order)
    // In cycle 2, source 0 is the OUTPUT of cycle 1 (not the combiner output).
    // We use "blenderOut.rgb" for this after cycle 1 runs.
    auto pmColorSel = [](int s, bool isCycle2) -> const char* {
        switch (s) {
        case 0: return isCycle2 ? "blenderOut.rgb" : "combined.rgb";
        case 1: return "memColor.rgb";
        case 2: return "uBlendColor.rgb";
        case 3: return "uFogColor.rgb";
        default: return "vec3(0.0)";
        }
    };

    // For dual-source: memColor is handled by HW blend, so P/M source 1 → vec3(0)
    auto pmColorSelDS = [](int s, bool isCycle2) -> const char* {
        switch (s) {
        case 0: return isCycle2 ? "blenderOut.rgb" : "combined.rgb";
        case 1: return "vec3(0.0)"; // memColor excluded — handled by HW blend
        case 2: return "uBlendColor.rgb";
        case 3: return "uFogColor.rgb";
        default: return "vec3(0.0)";
        }
    };

    // A alpha sources — cycle 2 source 0 uses cycle 1 output alpha
    auto aAlphaSel = [](int s, bool isCycle2) -> const char* {
        switch (s) {
        case 0: return isCycle2 ? "blenderOut.a" : "combined.a";
        case 1: return "uFogColor.a";
        case 2: return "shadeColor.a";
        case 3: return "0.0";
        default: return "0.0";
        }
    };

    // is2Cycle and forceBlend already declared above (shared with MLE path)

    std::ostringstream ss;
    ss << preamble.str();

    // ── Helper lambda: emit the final-cycle dual-source split ────────────
    // Computes srcContrib (non-memColor terms) → combined.rgb,
    //          dstFactor  (memColor weight)     → fragColor1.
    // Hardware blend does: fragColor * 1 + dst * fragColor1.
    auto emitFinalCycleDS = [&](int p, int m, int b, bool isCycle2) {
        bool p_is_mem = (p == 1);
        bool m_is_mem = (m == 1);
        if (forceBlend) {
            ss << "        vec3 srcContrib = vec3(0.0);\n";
            ss << "        float dstFactor = 0.0;\n";
            if (!p_is_mem)
                ss << "        srcContrib += bP * bA;\n";
            else
                ss << "        dstFactor += bA;\n";
            if (!m_is_mem)
                ss << "        srcContrib += bM * bB;\n";
            else
                ss << "        dstFactor += bB;\n";
            ss << "        combined.rgb = clamp(srcContrib, 0.0, 1.0);\n";
            ss << "        fragColor1 = vec4(clamp(dstFactor, 0.0, 1.0));\n";
        } else {
            // !forceBlend: output P directly (full coverage bypass)
            if (!p_is_mem) {
                ss << "        combined.rgb = clamp(bP, 0.0, 1.0);\n";
                ss << "        fragColor1 = vec4(0.0);\n";
            } else {
                // P=memColor → keep destination
                ss << "        combined.rgb = vec3(0.0);\n";
                ss << "        fragColor1 = vec4(1.0);\n";
            }
        }
    };

    // ── Helper lambda: emit the final-cycle original (non-dual-source) ───
    auto emitFinalCycleOrig = [&]() {
        if (forceBlend) {
            ss << "        float denom = max(bA + bB, 0.001);\n";
            ss << "        combined.rgb = clamp((bP * bA + bM * bB) / denom, 0.0, 1.0);\n";
        } else {
            ss << "        combined.rgb = clamp(bP, 0.0, 1.0);\n";
        }
    };

    // ── Cycle 1 blender ──────────────────────────────────────────────────
    ss << "\n    // Blender cycle 1: out = (P * A + M * B) / (A + B)\n";
    ss << "    {\n";
    if (dualSource && !is2Cycle) {
        // 1-cycle mode with dual-source: use DS color selectors for P/M
        ss << "        vec3 bP = " << pmColorSelDS(p1, false) << ";\n";
    } else {
        ss << "        vec3 bP = " << pmColorSel(p1, false) << ";\n";
    }
    ss << "        float bA = " << aAlphaSel(a1, false) << ";\n";
    if (dualSource && !is2Cycle) {
        ss << "        vec3 bM = " << pmColorSelDS(m1, false) << ";\n";
    } else {
        ss << "        vec3 bM = " << pmColorSel(m1, false) << ";\n";
    }

    // B source: 0=(1-A), 1=memColor.a, 2=1.0, 3=0.0
    switch (b1) {
    case 0:  ss << "        float bB = 1.0 - bA;\n"; break;
    case 1:
        // For dual-source final cycle, approximate memColor.a as 1.0
        if (dualSource && !is2Cycle)
            ss << "        float bB = 1.0; // memColor.a approx for dual-source\n";
        else
            ss << "        float bB = memColor.a;\n";
        break;
    case 2:  ss << "        float bB = 1.0;\n"; break;
    default: ss << "        float bB = 0.0;\n"; break;
    }

    if (!is2Cycle) {
        // 1-cycle mode: cycle 1 is the only (and final) cycle.
        if (dualSource)
            emitFinalCycleDS(p1, m1, b1, false);
        else
            emitFinalCycleOrig();
        ss << "    }\n";
    } else {
        // 2-cycle mode: cycle 1 always runs the full formula (intermediate).
        // Intermediate cycle: out = (P * A + M * B) / (A + B)
        ss << "        float denom = max(bA + bB, 0.001);\n";
        ss << "        combined.rgb = clamp((bP * bA + bM * bB) / denom, 0.0, 1.0);\n";
        ss << "    }\n";

        ss << "    vec4 blenderOut = vec4(combined.rgb, combined.a);\n";
        ss << "\n    // Blender cycle 2: out = (P * A + M * B) / (A + B)\n";
        ss << "    {\n";
        if (dualSource) {
            ss << "        vec3 bP = " << pmColorSelDS(p2, true) << ";\n";
        } else {
            ss << "        vec3 bP = " << pmColorSel(p2, true) << ";\n";
        }
        ss << "        float bA = " << aAlphaSel(a2, true) << ";\n";
        if (dualSource) {
            ss << "        vec3 bM = " << pmColorSelDS(m2, true) << ";\n";
        } else {
            ss << "        vec3 bM = " << pmColorSel(m2, true) << ";\n";
        }
        switch (b2) {
        case 0:  ss << "        float bB = 1.0 - bA;\n"; break;
        case 1:
            if (dualSource)
                ss << "        float bB = 1.0; // memColor.a approx for dual-source\n";
            else
                ss << "        float bB = memColor.a;\n";
            break;
        case 2:  ss << "        float bB = 1.0;\n"; break;
        default: ss << "        float bB = 0.0;\n"; break;
        }
        // Cycle 2 is the final cycle.
        if (dualSource)
            emitFinalCycleDS(p2, m2, b2, true);
        else
            emitFinalCycleOrig();
        ss << "    }\n";
    }

    // ── CVG_DEST logic (N64 Coverage Wrap/Clamp) ─────────────────────────
    ss << "\n    // CVG_DEST logic (N64 Coverage Wrap/Clamp)\n";
    ss << "    {\n";
    ss << "        int cur_cvg = int(round(coverage * 7.0));\n";
    ss << "        int mem_cvg = int(round(memColor.a * 7.0));\n";
    ss << "        int final_cvg = cur_cvg;\n";
    ss << "        int cvgDst = " << ((otherModeL >> 8) & 0x03) << ";\n";
    ss << "        if (cvgDst == 0) {\n";
    ss << "            final_cvg = cur_cvg + mem_cvg;\n";
    ss << "            if ((final_cvg & 8) == 0) final_cvg &= 7; else final_cvg = 7;\n";
    ss << "        } else if (cvgDst == 1) {\n";
    ss << "            final_cvg = (cur_cvg + mem_cvg) & 7;\n";
    ss << "        } else if (cvgDst == 2) {\n";
    ss << "            final_cvg = 7;\n";
    ss << "        } else if (cvgDst == 3) {\n";
    ss << "            final_cvg = mem_cvg;\n";
    ss << "        }\n";
    ss << "        combined.a = float(final_cvg) / 7.0;\n";
    ss << "    }\n";

    return ss.str();
}

std::string ShaderGenerator::GenerateAlphaTestCode(uint64_t features) const {
    if (!(features & SF_ALPHA_TEST)) return "";
    // N64 hardware: alpha compare tests cycle 0's alpha output (before cycle 1
    // in 2-cycle mode).  c0Alpha is set by GenerateCombinerCode.
    std::string code = "    {\n";
    // Determine alpha compare threshold
    if (features & SF_ALPHA_NOISE_DITHER) {
        code += "        float alphaTestValue = noise;\n";
    } else {
        code += "        float alphaTestValue = uAlphaRef;\n";
    }
    // Determine alpha source value (GLideN64 ShaderAlphaTest):
    // When alphaCvgSel is set and cvgXAlpha is NOT set, the alpha compare
    // uses a fixed value of 0.125 (average of 1-out-of-8 coverage samples)
    // instead of the actual combiner alpha.
    if ((features & SF_ALPHA_CVG_SEL) && !(features & SF_ALPHA_CVG)) {
        code += "        float alphaValue = 0.125;\n";
    } else {
        code += "        float alphaValue = clamp(c0Alpha, 0.0, 1.0);\n";
    }
    code += "        if (alphaValue < alphaTestValue) discard;\n";
    code += "    }\n";
    return code;
}

std::string ShaderGenerator::GenerateFogCode(uint64_t features) const {
    if (!(features & SF_FOG)) return "";
    // GLideN64 ShaderLegacyBlender: fog is applied by mixing fragColor with
    // uFogColor using the per-vertex fog factor stored in vShadeColor.a
    // (which equals vFog — set in vertex shader when uFogUsage == 1, or
    // pre-set by RSP fog computation in shade alpha).
    //
    // uFogUsage: 0=standard fog, 1=F3DAM fog→shadeAlpha, 2=F3DAM fog→shadeRGB
    // When uFogUsage==2, fog was baked into shade RGB; no post-process needed.
    // For modes 0 and 1, use vShadeColor.a (= vFog = RSP fog factor).
    return
        "    if (uFogUsage == 0 || uFogUsage == 1) {\n"
        "        float fogFactor = clamp(vFog, 0.0, 1.0);\n"
        "        fragColor.rgb = mix(fragColor.rgb, uFogColor.rgb, fogFactor);\n"
        "    }\n";
}

std::string ShaderGenerator::GenerateNoiseCode(uint64_t features) const {
    if (!(features & SF_NOISE_ENABLE)) return "";
    // Hash-based pseudorandom noise (GLideN64 approach).
    // Uses screen coords + per-frame seed — no texture lookup needed.
    return
        "    float noise;\n"
        "    {\n"
        "        vec2 noiseCoord = floor(gl_FragCoord.xy / uScreenScale);\n"
        "        vec3 p3 = fract(vec3(noiseCoord, float(uFrameCount)) * vec3(0.1031, 0.1030, 0.0973));\n"
        "        p3 += dot(p3, p3.yzx + 33.33);\n"
        "        noise = fract((p3.x + p3.y) * p3.z);\n"
        "    }\n";
}

std::string ShaderGenerator::GenerateTextureSampleCode(uint64_t features) const {
    std::string code;
    if (features & SF_TEXTURE_0) {
        // Perspective-correct texture coordinates: divide by vClipW (1/W)
        // to recover screen-space texels.  The vertex setup pre-multiplied
        // texcoords by 1/W when texturePersp is ON, or left them as-is
        // (with perspW=1) when OFF — either way this division is correct.
        code += "    vec2 tc0 = (vClipW > 1e-6) ? vTexCoord0 / vClipW : vTexCoord0;\n";
        code += "    if (uUseTexCoordBounds != 0)\n";
        code += "        tc0 = clamp(tc0, uTexCoordBounds0.xy, uTexCoordBounds0.zw);\n";
        // Compute tile-space texel coordinate, then WRAP to [-1024,1024]
        // range to prevent overflow — matches GLideN64's WRAP() macro.
        code += "    vec2 tileCoord0 = tc0 * uTexScale0 - uTexOffset0;\n";
        code += "    tileCoord0 = tileCoord0 - floor(tileCoord0 / 2048.0) * 2048.0;\n";
        code += "    tileCoord0 = mix(tileCoord0, tileCoord0 - 2048.0, greaterThan(tileCoord0, vec2(1024.0)));\n";
        code += "    vec2 uv0 = tileCoord0 / max(uTexSize0, vec2(1.0));\n";
        // Apply N64 wrap/mirror/clamp using tile settings
        // uTexClamp0 = (clampS, clampT, mirrorS, mirrorT)
        // uTexMask0 = (maskS, maskT)  — number of mask bits (0 = no mask)
        code += "    {\n";
        code += "      vec2 maskBits0 = uTexMask0;\n";
        code += "      vec2 maskSize0 = pow(vec2(2.0), maskBits0);\n";
        code += "      if (maskBits0.x > 0.0) {\n";
        code += "        if (uTexClamp0.z > 0.0) {\n"; // mirrorS
        code += "          float period = maskSize0.x * 2.0;\n";
        code += "          float m = mod(uv0.x * uTexSize0.x, period);\n";
        code += "          uv0.x = (m < maskSize0.x) ? m : (period - m - 1.0);\n";
        code += "          uv0.x /= uTexSize0.x;\n";
        code += "        } else {\n";
        code += "          uv0.x = mod(uv0.x * uTexSize0.x, maskSize0.x) / uTexSize0.x;\n";
        code += "        }\n";
        code += "      }\n";
        code += "      if (maskBits0.y > 0.0) {\n";
        code += "        if (uTexClamp0.w > 0.0) {\n"; // mirrorT
        code += "          float period = maskSize0.y * 2.0;\n";
        code += "          float m = mod(uv0.y * uTexSize0.y, period);\n";
        code += "          uv0.y = (m < maskSize0.y) ? m : (period - m - 1.0);\n";
        code += "          uv0.y /= uTexSize0.y;\n";
        code += "        } else {\n";
        code += "          uv0.y = mod(uv0.y * uTexSize0.y, maskSize0.y) / uTexSize0.y;\n";
        code += "        }\n";
        code += "      }\n";
        code += "      if (uTexClamp0.x > 0.0) uv0.x = clamp(uv0.x, 0.0, 1.0);\n";
        code += "      if (uTexClamp0.y > 0.0) uv0.y = clamp(uv0.y, 0.0, 1.0);\n";
        code += "    }\n";
        if (features & SF_TEXTURE_LOD) {
            // ── Mipmap tile ladder: compute LOD then sample with explicit level ──
            code += "    {\n";
            code += "        vec2 dSDX = dFdx(tc0 * uTexSize0) * uScreenScale;\n";
            code += "        vec2 dSDY = dFdy(tc0 * uTexSize0) * uScreenScale;\n";
            code += "        float lodMag = max(max(abs(dSDX.x), abs(dSDX.y)), max(abs(dSDY.x), abs(dSDY.y)));\n";
            code += "        int max_tile = min(uTextureDetail != 2 ? 7 : 6, uMaxTile);\n";
            code += "        float min_lod = uTextureDetail != 0 ? uMinLod : 1.0;\n";
            code += "        float max_lod = exp2(float(max_tile)) - 1.0 / 32.0;\n";
            code += "        float lod_clamp = min(max(lodMag, min_lod), max_lod);\n";
            code += "        lod_tile = clamp(int(log2(max(lod_clamp, 1.0))), 0, max_tile);\n";
            code += "        lodFrac = lod_clamp / exp2(float(lod_tile));\n";
            code += "        if (uTextureDetail == 1 || lod_clamp >= 1.0)\n";
            code += "            lodFrac = clamp(lodFrac - 1.0, -1.0, 1.0);\n";
            code += "        else if (lod_clamp < 1.0 && uTextureDetail == 0)\n";
            code += "            lodFrac = 0.0;\n";
            code += "    }\n";
            // Detail mode: shift tile indices so texel0=lod_tile+1, texel1=lod_tile+2
            code += "    int sample_tile = (uTextureDetail == 2 && lod_tile >= 0) ? lod_tile + 1 : lod_tile;\n";
            code += "    vec4 texel0 = textureLod(uTexture0, uv0 * uGPUScale0, float(min(sample_tile, uMaxTile)));\n";
        } else if (features & SF_TEXTURE_3POINT)
            code += "    vec4 texel0 = n64Filter3Point(uTexture0, uv0, uTexSize0, uGPUScale0);\n";
        else if (features & SF_TEXTURE_BILINEAR)
            code += "    vec4 texel0 = texture(uTexture0, uv0 * uGPUScale0);\n"; // GL bilinear
        else
            code += "    vec4 texel0 = texture(uTexture0, uv0 * uGPUScale0);\n";
        // FB monochrome/fixedAlpha for tile 0 — when texture comes from a framebuffer
        code += "    if (uFbMonochrome0 == 1) texel0 = vec4(texel0.r);\n";
        code += "    else if (uFbMonochrome0 == 2) texel0.rgb = vec3(dot(vec3(0.2126, 0.7152, 0.0722), texel0.rgb));\n";
        code += "    if (uFbFixedAlpha0 == 1) texel0.a = 0.825;\n";
        // Chroma key alpha: replace texel0.a with key match value
        if (features & SF_CHROMA_KEY) {
            // N64 chroma key: per-channel distance check against center/width/scale.
            // key_ch = clamp(width - |texel - center| * scale, 0, 1)
            // Width defines the acceptance radius; scale sharpens the falloff.
            // texel0.a = min(keyR, keyG, keyB)
            code += "    {\n";
            code += "        float keyR = clamp(uKeyWidth.r - abs(texel0.r - uCenterColor.r) * uScaleColor.r, 0.0, 1.0);\n";
            code += "        float keyG = clamp(uKeyWidth.g - abs(texel0.g - uCenterColor.g) * uScaleColor.g, 0.0, 1.0);\n";
            code += "        float keyB = clamp(uKeyWidth.b - abs(texel0.b - uCenterColor.b) * uScaleColor.b, 0.0, 1.0);\n";
            code += "        texel0.a = min(min(keyR, keyG), keyB);\n";
            code += "    }\n";
        }
    }
    if (features & SF_TEXTURE_1) {
        if (features & SF_TEXTURE_LOD) {
            // ── LOD mode: texel1 from next mip level of same texture ──
            code += "    vec4 texel1 = textureLod(uTexture0, uv0 * uGPUScale0, float(min(sample_tile + 1, uMaxTile)));\n";
        } else {
            // ── Non-LOD: texel1 from separate texture unit 1 ──
            code += "    vec2 tc1 = (vClipW > 1e-6) ? vTexCoord1 / vClipW : vTexCoord1;\n";
            code += "    if (uUseTexCoordBounds != 0)\n";
            code += "        tc1 = clamp(tc1, uTexCoordBounds1.xy, uTexCoordBounds1.zw);\n";
            code += "    vec2 tileCoord1 = tc1 * uTexScale1 - uTexOffset1;\n";
            code += "    tileCoord1 = tileCoord1 - floor(tileCoord1 / 2048.0) * 2048.0;\n";
            code += "    tileCoord1 = mix(tileCoord1, tileCoord1 - 2048.0, greaterThan(tileCoord1, vec2(1024.0)));\n";
            code += "    vec2 uv1 = tileCoord1 / max(uTexSize1, vec2(1.0));\n";
            code += "    {\n";
            code += "      vec2 maskBits1 = uTexMask1;\n";
            code += "      vec2 maskSize1 = pow(vec2(2.0), maskBits1);\n";
            code += "      if (maskBits1.x > 0.0) {\n";
            code += "        if (uTexClamp1.z > 0.0) {\n";
            code += "          float period = maskSize1.x * 2.0;\n";
            code += "          float m = mod(uv1.x * uTexSize1.x, period);\n";
            code += "          uv1.x = (m < maskSize1.x) ? m : (period - m - 1.0);\n";
            code += "          uv1.x /= uTexSize1.x;\n";
            code += "        } else {\n";
            code += "          uv1.x = mod(uv1.x * uTexSize1.x, maskSize1.x) / uTexSize1.x;\n";
            code += "        }\n";
            code += "      }\n";
            code += "      if (maskBits1.y > 0.0) {\n";
            code += "        if (uTexClamp1.w > 0.0) {\n";
            code += "          float period = maskSize1.y * 2.0;\n";
            code += "          float m = mod(uv1.y * uTexSize1.y, period);\n";
            code += "          uv1.y = (m < maskSize1.y) ? m : (period - m - 1.0);\n";
            code += "          uv1.y /= uTexSize1.y;\n";
            code += "        } else {\n";
            code += "          uv1.y = mod(uv1.y * uTexSize1.y, maskSize1.y) / uTexSize1.y;\n";
            code += "        }\n";
            code += "      }\n";
            code += "      if (uTexClamp1.x > 0.0) uv1.x = clamp(uv1.x, 0.0, 1.0);\n";
            code += "      if (uTexClamp1.y > 0.0) uv1.y = clamp(uv1.y, 0.0, 1.0);\n";
            code += "    }\n";
            if (features & SF_TEXTURE_3POINT)
                code += "    vec4 texel1 = n64Filter3Point(uTexture1, uv1, uTexSize1, uGPUScale1);\n";
            else
                code += "    vec4 texel1 = texture(uTexture1, uv1 * uGPUScale1);\n";
            // FB monochrome/fixedAlpha for tile 1
            code += "    if (uFbMonochrome1 == 1) texel1 = vec4(texel1.r);\n";
            code += "    else if (uFbMonochrome1 == 2) texel1.rgb = vec3(dot(vec3(0.2126, 0.7152, 0.0722), texel1.rgb));\n";
            code += "    if (uFbFixedAlpha1 == 1) texel1.a = 0.825;\n";
        }
    }
    return code;
}

std::string ShaderGenerator::GenerateN64TexFilterCode() const {
      // N64 3-point bilinear filter with GPU padding scaling.
      // Includes a color-bleeding variant that pre-multiplies alpha before
      // interpolation to prevent color fringing at transparent edges.
      return R"(
vec4 n64Filter3Point(sampler2D tex, vec2 uv, vec2 texSize, vec2 gpuScale) {
    vec2 texelPos = uv * texSize - 0.5;
    vec2 fracPart = fract(texelPos);
    vec2 baseUV = ((floor(texelPos) + 0.5) / texSize) * gpuScale;
    vec2 step = (1.0 / texSize) * gpuScale;
    vec4 c00 = texture(tex, baseUV);
    vec4 c10 = texture(tex, baseUV + vec2(step.x, 0.0));
    vec4 c01 = texture(tex, baseUV + vec2(0.0, step.y));
    vec4 c11 = texture(tex, baseUV + step);

#ifdef ALPHA_TEST
    // Pre-multiply RGB by alpha to prevent color bleeding at edges
    c00.rgb *= c00.a;
    c10.rgb *= c10.a;
    c01.rgb *= c01.a;
    c11.rgb *= c11.a;
#endif

    vec4 result;
    if (fracPart.x + fracPart.y < 1.0) {
        result = c00 + fracPart.x * (c10 - c00) + fracPart.y * (c01 - c00);
    } else {
        result = c11 + (1.0 - fracPart.x) * (c01 - c11) + (1.0 - fracPart.y) * (c10 - c11);
    }

#ifdef ALPHA_TEST
    // Un-premultiply to recover final color
    if (result.a > 0.001)
        result.rgb /= result.a;
#endif

    return result;
}
)";
}

// ═══════════════════════════════════════════════════════════════════════════════
// DEPTH RENDER: When colorImage == depthImage, the combiner output (fragColor.r)
// is a depth value.  Write it to the Z-image and set gl_FragDepth so the GL
// depth buffer also receives the value.
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateDepthRenderCode(uint64_t features) const {
    if (!(features & SF_DEPTH_RENDER)) return "";

    const bool hasN64Depth = (features & SF_N64_DEPTH_COMPARE) != 0;
    const bool depthCompare = (features & SF_DEPTH_COMPARE) != 0;

    std::string code;
    code += "    // ─── Depth render: write combiner color.r as depth ───\n";
    code += "    float depthRenderZ = clamp(fragColor.r, 0.0, 1.0);\n";

    if (hasN64Depth) {
        // Z-image path: write depth to Z-image via imageStore (interlock required)
        code += "    {\n";
        code += "        ivec2 zCoord = ivec2(gl_FragCoord.xy);\n";
        code += "        BEGIN_INTERLOCK();\n";
        if (depthCompare) {
            // Mode 2: compare against existing Z-image, write if passes.
            // Keep END_INTERLOCK at top level for Intel Iris Xe compatibility.
            code += "        float storedZ = IMG_LOAD_F(uZImageImg, zCoord);\n";
            code += "        bool depthRenderPass = (depthRenderZ < storedZ);\n";
            code += "        if (depthRenderPass) {\n";
            code += "            IMG_STORE_F(uZImageImg, zCoord, depthRenderZ);\n";
            code += "            IMG_STORE_F(uDeltaZImageImg, zCoord, 0.0);\n";
            code += "        }\n";
            code += "        END_INTERLOCK();\n";
            code += "        if (!depthRenderPass) discard;\n";
        } else {
            // No compare — just store unconditionally
            code += "        IMG_STORE_F(uZImageImg, zCoord, depthRenderZ);\n";
            code += "        IMG_STORE_F(uDeltaZImageImg, zCoord, 0.0);\n";
            code += "        END_INTERLOCK();\n";
        }
        code += "    }\n";
    }

    if (m_language != ShaderLanguage::HLSL_SM5) {
        code += "    gl_FragDepth = depthRenderZ;\n";
    }
    return code;
}

std::string ShaderGenerator::GenerateDepthCompareCode(uint64_t features) const {
    if (!(features & SF_DEPTH_COMPARE)) return "";

    // N64-accurate Z-image depth compare matching GLideN64's normalized approach.
    // All depth values are stored and compared as normalized floats [0,1],
    // using gl_FragCoord.z directly.  DeltaZ uses fwidth() for screen-space
    // gradient, matching GLideN64's `4.0*fwidth(curZ)`.  No ZLUT needed.
    if (features & SF_N64_DEPTH_COMPARE) {
        const bool primDepth = (features & SF_DEPTH_SOURCE_PRIM) != 0;
        const bool depthWrite = (features & SF_DEPTH_WRITE) != 0;
        const bool mleBlender = (features & SF_MLE_BLENDER) != 0;

        std::string code;
        code += "    // ─── N64-accurate depth compare (normalized, GLideN64-style) ───\n";
        if (mleBlender)
            code += "    bool n64DepthPassed = true;\n";
        code += "    {\n";
        code += "        ivec2 zCoord = ivec2(gl_FragCoord.xy);\n";
        if (primDepth) {
            code += "        float curZ = clamp(uPrimDepth, 0.0, 1.0);\n";
            code += "        float dz = uPrimDeltaZ;\n";
        } else {
            code += "        float curZ = clamp(gl_FragCoord.z, 0.0, 1.0);\n";
            code += "        float dz = 4.0 * fwidth(curZ);\n";
        }
        code += "\n";
        code += "        BEGIN_INTERLOCK();\n";
        code += "        float bufZ = IMG_LOAD_F(uZImageImg, zCoord);\n";
        code += "        float bufDZ = IMG_LOAD_F(uDeltaZImageImg, zCoord);\n";
        code += "\n";
        if (primDepth) {
            code += "        float dzMin = dz;\n";
        } else {
            code += "        float dzMin = min(dz, bufDZ);\n";
        }
        code += "        bool bMax = (bufZ == 1.0);\n";
        code += "        bool bInfront = (curZ < bufZ);\n";
        code += "        bool bNearer  = ((curZ - dzMin) <= bufZ);\n";
        code += "        bool bFarther = ((curZ + dzMin) >= bufZ);\n";
        code += "\n";
        code += "        bool zPass = false;\n";
        code += "        // uZMode: 0=OPAQUE, 1=INTERPENETRATING, 2=TRANSPARENT, 3=DECAL\n";
        code += "        if (uZMode == 0) {\n";
        code += "            zPass = bMax || bInfront;\n";
        code += "        } else if (uZMode == 1) {\n";
        code += "            zPass = bMax || bNearer;\n";
        code += "        } else if (uZMode == 2) {\n";
        code += "            zPass = bMax || bInfront;\n";
        code += "        } else {\n";
        code += "            zPass = bFarther && bNearer && !bMax;\n";
        code += "        }\n";
        if (depthWrite) {
            code += "\n";
            code += "        if (zPass) {\n";
            code += "            IMG_STORE_F(uZImageImg, zCoord, curZ);\n";
            code += "            IMG_STORE_F(uDeltaZImageImg, zCoord, dz);\n";
            code += "        }\n";
        }
        if (mleBlender) {
            code += "        n64DepthPassed = zPass;\n";
        } else {
            code += "        END_INTERLOCK();\n";
            code += "        if (!zPass) discard;\n";
        }
        code += "    }\n";
        return code;
    }

    // Fallback: simple depth texture compare (used when Z-image textures aren't bound)
    return
        "    float depthRef = texture(uDepthTexture, gl_FragCoord.xy * uInvScreenSize).r;\n"
        "    if (gl_FragCoord.z > depthRef) discard;\n";
}

// When Z_SOURCE_SEL=PRIM (SF_DEPTH_SOURCE_PRIM), all fragments output a fixed
// primitive depth instead of the rasterizer-interpolated depth.  Write
// gl_FragDepth at the very top of main() so every non-discarding execution
// path has it set (GLSL requires this when gl_FragDepth is written at all).
std::string ShaderGenerator::GeneratePrimDepthCode(uint64_t features) const {
    if (!(features & SF_DEPTH_SOURCE_PRIM)) return "";
    if (m_language == ShaderLanguage::HLSL_SM5) return "";  // HLSL needs SV_Depth in PSOutput
    return "    gl_FragDepth = uPrimDepth;\n";
}

std::string ShaderGenerator::GenerateCoverageCode(uint64_t features) const {
    // Fill/copy modes don't use coverage — return full coverage immediately.
    if (features & (SF_COPY_MODE | SF_FILL_RECT)) {
        return "    // Fill/copy: full coverage, no alpha modification\n"
               "    float coverage = 1.0;\n";
    }

    bool hasCvgAlpha = (features & SF_ALPHA_CVG) != 0;
    bool hasAlphaSel = (features & SF_ALPHA_CVG_SEL) != 0;
    bool hasClrOnCvg = (features & SF_COLOR_ON_COVERAGE) != 0;
    bool hasAA       = (features & SF_COVERAGE_ENABLE) != 0;

    std::string code;
    code += "    // N64 coverage emulation (8-sample sub-pixel AA)\n";
    if (hasAA) {
        // GLideN64 ShaderCoverage: 8 sub-pixel sample points using barycentric
        // coordinate derivatives. Each sample tests if the biased barycoord is
        // inside the triangle (all components >= 0). Coverage is the fraction
        // of samples that pass.
        code += "    float cov;\n";
        code += "    {\n";
        code += "        const vec2 bias[8] = vec2[8](\n";
        code += "            vec2(-0.5,-0.5), vec2(0.0,-0.5), vec2(-0.25,-0.25), vec2(0.25,-0.25),\n";
        code += "            vec2(-0.5, 0.0), vec2(0.0, 0.0), vec2(-0.25, 0.25), vec2(0.25, 0.25));\n";
        code += "        vec4 dBCdx = dFdx(vBaryCoord);\n";
        code += "        vec4 dBCdy = dFdy(vBaryCoord);\n";
        code += "        float cvgSum = 0.0;\n";
        code += "        for (int i = 0; i < 8; i++) {\n";
        code += "            vec4 bc = vBaryCoord + dBCdx * bias[i].x + dBCdy * bias[i].y;\n";
        code += "            vec4 inside = step(0.0, bc);\n";
        code += "            cvgSum += 0.125 * inside.x * inside.y * inside.z * inside.w;\n";
        code += "        }\n";
        code += "        cov = cvgSum;\n";
        code += "    }\n";
    } else {
        code += "    float cov = 1.0;\n";
    }
    if (hasCvgAlpha) {
        // CVG_X_ALPHA: multiply coverage by the ORIGINAL combiner alpha.
        // Must happen BEFORE alpha is replaced by ALPHA_CVG_SEL.
        // See GLideN64: uCvgXAlpha → cvg *= clampedColor.a (original alpha)
        code += "    cov *= combined.a;\n";
        code += "    if (cov < 0.125) discard;\n";
    }
    if (hasAlphaSel) {
        // ALPHA_CVG_SEL: replace combiner alpha with (modified) coverage.
        // Happens AFTER CVG_X_ALPHA multiply.
        // See GLideN64: uAlphaCvgSel → clampedColor.a = cvg
        code += "    combined.a = cov;\n";
    } else if (hasCvgAlpha) {
        // When CVG_X_ALPHA is set but ALPHA_CVG_SEL is not, leave
        // combined.a unchanged — the coverage multiply only affects
        // the 'cov' value used for edge AA, not the blender alpha.
    }
    if (hasClrOnCvg) {
        code += "    if (cov < 0.125) { combined = uFillColor; combined.a = 1.0; }\n";
    }
    code += "    float coverage = cov;\n";
    return code;
}

std::string ShaderGenerator::GenerateDitherCode(uint64_t features, uint32_t otherModeH) const {
    if (!(features & (SF_DITHER_ENABLE | SF_ALPHA_DITHER))) return "";
    // N64 hardware dithers the combiner output (8-bit → 5-bit) BEFORE the blender.
    // Operates on 'combined' so it must be emitted before GenerateBlenderCode.
    // There are two 4×4 matrices: MagicSquare and Bayer.
    // Color dither mode (otherModeH bits 6-7): 0=MagicSquare, 1=Bayer, 2=Noise, 3=None
    // Alpha dither mode (otherModeH bits 4-5): 0=pattern, 1=~pattern, 2=Noise, 3=None
    // Alpha mode 0/1 selects same-or-complement pattern based on color mode.
    // Based on GLideN64 ShaderDithering.
    uint32_t colorDitherMode = (otherModeH >> 6) & 0x03;
    uint32_t alphaDitherMode = (otherModeH >> 4) & 0x03;

    std::string code = "    {\n";

    // Emit the two 4×4 matrices if any structured pattern is needed
    bool needMatrices = ((features & SF_DITHER_ENABLE) && colorDitherMode <= 1)
                     || ((features & SF_ALPHA_DITHER)  && alphaDitherMode <= 1);
    if (needMatrices) {
        // Use flat const arrays + scalar index instead of mat4×vec4 dot product.
        // The mat4/vec4 dynamic-index pattern crashes Intel Iris Xe (driver
        // Build 32.0.101.7080) inside glLinkProgram — a known driver bug.
        code += "        const float bayerLUT[16] = float[16](\n"
                "            0.0, 0.5, 0.125, 0.625,\n"
                "            0.5, 0.0, 0.625, 0.125,\n"
                "            0.375, 0.875, 0.25, 0.75,\n"
                "            0.875, 0.375, 0.75, 0.25);\n";
        code += "        const float mSquareLUT[16] = float[16](\n"
                "            0.0, 0.75, 0.125, 0.875,\n"
                "            0.5, 0.25, 0.625, 0.375,\n"
                "            0.375, 0.625, 0.25, 0.5,\n"
                "            0.875, 0.125, 0.75, 0.0);\n";
        code += "        ivec2 ditherPos = ivec2(mod(floor(gl_FragCoord.xy / uScreenScale) - 0.5, 4.0));\n";
        code += "        int ditherIdx = ditherPos.y * 4 + ditherPos.x;\n";
        code += "        float bayerVal  = bayerLUT[ditherIdx];\n";
        code += "        float mSquareVal = mSquareLUT[ditherIdx];\n";
    }

    // --- Color dither ---
    if (features & SF_DITHER_ENABLE) {
        switch (colorDitherMode) {
        case 0: // MagicSquare
            code += "        float colorThreshold = mSquareVal;\n";
            break;
        case 1: // Bayer
            code += "        float colorThreshold = bayerVal;\n";
            break;
        case 2: // Noise — hash33 from https://www.shadertoy.com/view/4djSRW
            code += "        { vec2 ditherCoord = floor(gl_FragCoord.xy / uScreenScale);\n";
            code += "          vec3 p3 = fract(vec3(float(uFrameCount), ditherCoord) * vec3(.1031, .1030, .0973));\n";
            code += "          p3 += dot(p3, p3.yxz + 33.33);\n";
            code += "          vec3 nRGB = fract((p3.xxy + p3.yxx) * p3.zyx);\n";
            code += "          vec3 ditherRGB = vec3(7.0 / 255.0) * (nRGB - 0.5);\n";
            code += "          combined.rgb = clamp(combined.rgb + ditherRGB, 0.0, 1.0);\n";
            code += "          combined.rgb = round(combined.rgb * 31.0) / 31.0; }\n";
            break;
        default: break; // 3=None — shouldn't reach here
        }
        if (colorDitherMode <= 1) {
            // Structured pattern: uniform threshold for all 3 channels
            code += "        vec3 ditherRGB = vec3(7.0 / 255.0) * (vec3(colorThreshold) - 0.5);\n";
            code += "        combined.rgb = clamp(combined.rgb + ditherRGB, 0.0, 1.0);\n";
            code += "        combined.rgb = round(combined.rgb * 31.0) / 31.0;\n";
        }
    }

    // --- Alpha dither ---
    if (features & SF_ALPHA_DITHER) {
        // Alpha dither pattern depends on both alpha and color dither modes.
        // Mode 0: same family as color (MagicSquare if color=0|2, Bayer if color=1|3)
        // Mode 1: complement     (Bayer    if color=0|2, MagicSquare if color=1|3)
        // Mode 2: noise
        // Mode 3: none
        switch (alphaDitherMode) {
        case 0:
            if (colorDitherMode == 0 || colorDitherMode == 2)
                code += "        float alphaThreshold = mSquareVal;\n";
            else
                code += "        float alphaThreshold = bayerVal;\n";
            break;
        case 1:
            if (colorDitherMode == 0 || colorDitherMode == 2)
                code += "        float alphaThreshold = bayerVal;\n";
            else
                code += "        float alphaThreshold = mSquareVal;\n";
            break;
        case 2: { // Noise — hash13
            code += "        { vec2 ditherCoord2 = floor(gl_FragCoord.xy / uScreenScale);\n";
            code += "          vec3 p3 = fract(vec3(float(uFrameCount) + 1.0, ditherCoord2) * .1031);\n";
            code += "          p3 += dot(p3, p3.zyx + 31.32);\n";
            code += "          float alphaThreshold = fract((p3.x + p3.y) * p3.z);\n";
            code += "          float ditherA = 7.0 / 255.0 * (alphaThreshold - 0.5);\n";
            code += "          combined.a = clamp(combined.a + ditherA, 0.0, 1.0);\n";
            code += "          combined.a = round(combined.a * 31.0) / 31.0; }\n";
            break;
        }
        default: break; // 3=None
        }
        if (alphaDitherMode <= 1) {
            code += "        float ditherA = 7.0 / 255.0 * (alphaThreshold - 0.5);\n";
            code += "        combined.a = clamp(combined.a + ditherA, 0.0, 1.0);\n";
            code += "        combined.a = round(combined.a * 31.0) / 31.0;\n";
        }
    }

    code += "    }\n";
    return code;
}

std::string ShaderGenerator::GenerateMonochromeCode(uint64_t features) const {
    if (!(features & SF_MONOCHROME)) return "";
    return
        "    // Monochrome desaturation (game hack — e.g. Zelda OoT cutscenes)\n"
        "    {\n"
        "        float monoLuma = dot(fragColor.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
        "        fragColor.rgb = vec3(monoLuma);\n"
        "    }\n";
}

std::string ShaderGenerator::GenerateGammaCorrectionCode() const {
    return
        "    fragColor.rgb = pow(fragColor.rgb, vec3(1.0 / 2.2));\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// YUV TEXTURE CONVERSION
// N64 games (FMV sequences in e.g. Banjo-Kazooie, Conker, Pokémon Stadium)
// use YUV422 textures. The RDP converts Y'CbCr to RGB using ITU-R BT.601.
// uConvertK = (K0, K1, K2, K3) from hardware registers.
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateYUVConversionCode(uint64_t features) const {
    if (!(features & SF_TEXTURE_YUV)) return "";
    return
        "    // YUV → RGB conversion (N64 BT.601)\n"
        "    {\n"
        "        // texel0.r = Y', texel0.g = Cb (U), texel0.b = Cr (V)\n"
        "        float yy  = texel0.r;\n"
        "        float cb  = texel0.g - 0.5;\n"
        "        float cr  = texel0.b - 0.5;\n"
        "        // RDP uses programmable K coefficients:\n"
        "        // R = Y + K0 * Cr\n"
        "        // G = Y + K1 * Cr + K2 * Cb\n"
        "        // B = Y + K3 * Cb\n"
        "        texel0.r = clamp(yy + uConvertK.x * cr,                      0.0, 1.0);\n"
        "        texel0.g = clamp(yy + uConvertK.y * cr + uConvertK.z * cb,   0.0, 1.0);\n"
        "        texel0.b = clamp(yy + uConvertK.w * cb,                      0.0, 1.0);\n"
        "        texel0.a = 1.0;\n"
        "    }\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// LOD / MIPMAP CALCULATION
// The N64 RDP computes level-of-detail from screen-space texcoord gradients.
// In 2-cycle mode, texel0 = base mip, texel1 = next mip, and LOD fraction
// is used in the combiner — slot 13 (LOD_FRACTION).
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateLODCode(uint64_t features) const {
    // LOD computation is now integrated into GenerateTextureSampleCode()
    // which computes lod_tile and lodFrac before sampling textures so that
    // textureLod() can select the correct mipmap level.
    (void)features;
    return "";
}

// ═══════════════════════════════════════════════════════════════════════════════
// DETAIL / SHARPEN TEXTURE
// Detail: Multiply base texture by a repeating detail texture at higher freq.
// Sharpen: Extrapolate the mipmap blending past 1.0 to sharpen transitions.
// Used heavily in Zelda OoT (terrain), GoldenEye, Star Fox 64.
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateDetailSharpenCode(uint64_t features) const {
    // N64 detail/sharpen modes only modify the LOD fraction and tile selection,
    // both of which are already handled in the LOD computation code above.
    // The combiner naturally blends texel0/texel1 using the adjusted lodFrac.
    // No additional texel modification is needed here.
    (void)features;
    return "";
}


// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: FULL SHADER GENERATION FOR A COMBINER KEY
// ═══════════════════════════════════════════════════════════════════════════════

ShaderTemplate ShaderGenerator::GenerateShader(const CombinerKey& key) const {
    ShaderTemplate tmpl;
    uint64_t f = key.features;

    // ── defines ──────────────────────────────────────────────────────────
    tmpl.AddDefineIf(f & SF_TEXTURE_0,     "HAS_TEXTURE0");
    tmpl.AddDefineIf(f & SF_TEXTURE_1,     "HAS_TEXTURE1");
    tmpl.AddDefineIf(f & SF_ALPHA_TEST,    "ALPHA_TEST");
    tmpl.AddDefineIf(f & SF_FOG,           "USE_FOG");
    tmpl.AddDefineIf(f & SF_NOISE_ENABLE,  "USE_NOISE");
    tmpl.AddDefineIf(f & SF_DEPTH_COMPARE, "DEPTH_COMPARE");
    tmpl.AddDefineIf(f & SF_DITHER_ENABLE, "USE_DITHER");

    if (m_language == ShaderLanguage::HLSL_SM5) {
        // ── HLSL Vertex ──────────────────────────────────────────────────
        tmpl.vertexHeader =
            GetVertexInputs() +
            GetVertexOutputs() +
            GetCommonUniforms();
        tmpl.vertexBody =
            "VSOutput main(VSInput input) {\n"
            "    VSOutput output;\n"
            "    output.position  = mul(uMVPMatrix, input.aPosition);\n"
            "    output.vColor    = input.aColor;\n"
            "    output.vTexCoord0= input.aTexCoord0;\n"
            "    output.vTexCoord1= input.aTexCoord1;\n"
            "    output.vFog      = input.aFog;\n"
            "    output.vClipW    = input.aClipW;\n"
            "    output.vBaryCoord= float4(input.aBaryCoord, 1.0 - input.aBaryCoord.x - input.aBaryCoord.y, 0.5);\n"
            "    output.vDepth    = output.position.z / output.position.w;\n"
            "    if (uFogUsage == 1)\n"
            "        output.vColor.a = output.vFog;\n"
            "    else if (uFogUsage == 2)\n"
            "        output.vColor.rgb = float3(output.vFog, output.vFog, output.vFog);\n"
            "    output.vColorNP  = output.vColor;\n"
            "    return output;\n"
            "}\n";

        // ── HLSL Fragment ────────────────────────────────────────────────
        std::string fragHeader;
        fragHeader += GetHLSLCompatPreamble();
        fragHeader += GetFragmentInputs();
        fragHeader += GetFragmentOutputs(f);
        fragHeader += GetCommonUniforms();
        fragHeader += GetTextureUniforms();
        // ROV image declarations for MLE/depth-compare (D3D11.3)
        if (f & SF_N64_DEPTH_COMPARE) {
            fragHeader += "RasterizerOrderedTexture2D<float> uZImageImg : register(u0);\n"
                          "RasterizerOrderedTexture2D<float> uDeltaZImageImg : register(u1);\n";
        }
        if (f & SF_MLE_BLENDER) {
            fragHeader += "RasterizerOrderedTexture2D<float4> uColorImage : register(u2);\n"
                          "RasterizerOrderedTexture2D<uint> uCoverageImage : register(u3);\n";
        }
        if (f & (SF_N64_DEPTH_COMPARE | SF_MLE_BLENDER)) {
            fragHeader += "#define BEGIN_INTERLOCK()\n"
                          "#define END_INTERLOCK()\n"
                          "#define IMG_LOAD_F4(img, coord) ((img)[(coord)])\n"
                          "#define IMG_LOAD_F(img, coord) ((img)[(coord)])\n"
                          "#define IMG_LOAD_U(img, coord) ((img)[(coord)])\n"
                          "#define IMG_STORE_F4(img, coord, val) (img)[(coord)] = (val)\n"
                          "#define IMG_STORE_F(img, coord, val) (img)[(coord)] = (val)\n"
                          "#define IMG_STORE_U(img, coord, val) (img)[(coord)] = (val)\n";
        }
        if (f & SF_TEXTURE_3POINT)
            fragHeader += GenerateN64TexFilterCode();
        tmpl.fragmentHeader = std::move(fragHeader);

        std::string frag;
        frag += "PSOutput main(PSInput input) {\n";
        frag += "    float4 vColor = input.vColor;\n";
        frag += "    float4 vColorNP = input.vColorNP;\n";
        frag += "    float2 vTexCoord0 = input.vTexCoord0;\n";
        frag += "    float2 vTexCoord1 = input.vTexCoord1;\n";
        frag += "    float vFog = input.vFog;\n";
        frag += "    float vDepth = input.vDepth;\n";
        frag += "    float vClipW = input.vClipW;\n";
        frag += "    float4 vBaryCoord = input.vBaryCoord;\n";
        frag += "    float4 fragColor;\n";
        if (f & SF_DUAL_SOURCE_BLEND)
            frag += "    float4 fragColor1 = float4(0,0,0,0);\n";
        frag += "    int lod_tile = 0;\n";
        frag += "    float lodFrac = 1.0;\n";
        frag += GeneratePrimDepthCode(f);
        frag += GenerateTextureSampleCode(f);
        frag += GenerateYUVConversionCode(f);
        frag += GenerateLODCode(f);
        frag += GenerateDetailSharpenCode(f);
        frag += GenerateNoiseCode(f);
        frag += "    float4 shadeColor = uScreenSpaceTriangle == 0 ? vColor : vColorNP;\n";
        frag += GenerateCombinerCode(key.mux, f);
        frag += GenerateAlphaTestCode(f);                // N64: alpha compare before blender
        frag += GenerateCoverageCode(f);                 // Coverage modifies alpha AFTER test
        frag += GenerateDitherCode(f, key.otherModeH);   // N64: dither combiner output before blender
        std::string blenderCode0 = GenerateBlenderCode(key.blenderBits, key.otherModeL, f);
        bool mleDepthInterlock0 = (f & SF_MLE_BLENDER) &&
                                  (f & SF_N64_DEPTH_COMPARE) &&
                                  (f & SF_DEPTH_COMPARE) &&
                                  !(f & SF_DEPTH_RENDER);
        if (mleDepthInterlock0)
            frag += GenerateDepthCompareCode(f);
        frag += blenderCode0;
        frag += "    fragColor = combined;\n";
        if (f & SF_DEPTH_RENDER) {
            // Depth render: fragColor.r is a depth value — skip fog/dither/mono
            frag += GenerateDepthRenderCode(f);
        } else {
            // Fog fallback: on real N64 fog is applied by the blender.
            // Only add the separate fog pass when the shader blender was skipped.
            if (blenderCode0.empty()) {
                frag += GenerateFogCode(f);
            }
            if (!mleDepthInterlock0)
                frag += GenerateDepthCompareCode(f);
            frag += GenerateMonochromeCode(f);
        }
        frag += "    PSOutput psOut;\n";
        frag += "    psOut.fragColor = fragColor;\n";
        if (f & SF_DUAL_SOURCE_BLEND)
            frag += "    psOut.fragColor1 = fragColor1;\n";
        if (f & SF_DEPTH_RENDER)
            frag += "    psOut.fragDepth = depthRenderZ;\n";
        else if (f & SF_DEPTH_SOURCE_PRIM)
            frag += "    psOut.fragDepth = uPrimDepth;\n";
        frag += "    return psOut;\n";
        frag += "}\n";
        tmpl.fragmentBody = std::move(frag);
        return tmpl;
    }

    // ── GLSL path (unchanged) ────────────────────────────────────────────

    // ── vertex header ────────────────────────────────────────────────────
    tmpl.vertexHeader =
        GetVersionDirective() +
        GetVertexInputs() +
        GetVertexOutputs() +
        GetCommonUniforms();

    // ── vertex body ──────────────────────────────────────────────────────
    tmpl.vertexBody =
        "void main() {\n"
        "    gl_Position = uMVPMatrix * aPosition;\n"
        "    vColor      = aColor;\n"
        "    vTexCoord0  = aTexCoord0;\n"
        "    vTexCoord1  = aTexCoord1;\n"
        "    vFog        = aFog;\n"
        "    vClipW      = aClipW;\n"
        "    vBaryCoord  = vec4(aBaryCoord, 1.0 - aBaryCoord.x - aBaryCoord.y, 0.5);\n"
        "    vDepth      = gl_Position.z / gl_Position.w;\n"
        "    if (uFogUsage == 1)\n"
        "        vColor.a = vFog;\n"
        "    else if (uFogUsage == 2)\n"
        "        vColor.rgb = vec3(vFog);\n"
        "    vColorNP    = vColor;\n"
        "}\n";

    // ── fragment header ──────────────────────────────────────────────────
    tmpl.fragmentHeader = GetVersionDirective();
    // Fragment shader interlock: required for correct N64 depth compare
    // (imageLoad/imageStore on the Z-image) to prevent race conditions
    // between overlapping fragments from the same draw call.
    bool needsInterlock = (f & (SF_N64_DEPTH_COMPARE | SF_MLE_BLENDER)) != 0;
    if (needsInterlock) {
        tmpl.fragmentHeader +=
            "#extension GL_ARB_fragment_shader_interlock : enable\n"
            "layout(pixel_interlock_ordered) in;\n";
    }
    tmpl.fragmentHeader +=
        GetFragmentInputs() +
        GetFragmentOutputs(f) +
        GetCommonUniforms() +
        GetTextureUniforms();
    if (f & SF_N64_DEPTH_COMPARE) {
        tmpl.fragmentHeader +=
            "layout(binding = 0, r32f) uniform restrict image2D uZImageImg;\n"
            "layout(binding = 1, r32f) uniform restrict image2D uDeltaZImageImg;\n";
    }
    if (f & SF_MLE_BLENDER) {
        tmpl.fragmentHeader +=
            "layout(binding = 2, rgba8) uniform restrict image2D uColorImage;\n"
            "layout(binding = 3, r8ui) uniform restrict uimage2D uCoverageImage;\n";
    }
    if (f & (SF_N64_DEPTH_COMPARE | SF_MLE_BLENDER)) {
        if (needsInterlock) {
            tmpl.fragmentHeader +=
                "#define BEGIN_INTERLOCK() beginInvocationInterlockARB()\n"
                "#define END_INTERLOCK() endInvocationInterlockARB()\n";
        } else {
            // No interlock: Intel Iris Xe workaround — just use plain
            // imageLoad/imageStore without synchronization.
            tmpl.fragmentHeader +=
                "#define BEGIN_INTERLOCK()\n"
                "#define END_INTERLOCK()\n";
        }
        tmpl.fragmentHeader +=
            "#define IMG_LOAD_F4(img, coord) imageLoad(img, coord)\n"
            "#define IMG_LOAD_F(img, coord) imageLoad(img, coord).r\n"
            "#define IMG_LOAD_U(img, coord) imageLoad(img, coord).r\n"
            "#define IMG_STORE_F4(img, coord, val) imageStore(img, coord, val)\n"
            "#define IMG_STORE_F(img, coord, val) imageStore(img, coord, vec4(val, 0.0, 0.0, 0.0))\n"
            "#define IMG_STORE_U(img, coord, val) imageStore(img, coord, uvec4(val, 0u, 0u, 0u))\n";
    }
    if (f & SF_TEXTURE_3POINT)
        tmpl.fragmentHeader += GenerateN64TexFilterCode();

    // ── fragment body ────────────────────────────────────────────────────
    std::string frag;
    frag += "void main() {\n";
    // Declare texel0 default before GenerateDetailSharpenCode which may use it
    if (!(f & SF_TEXTURE_0)) frag += "    vec4 texel0 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    if (f & SF_DUAL_SOURCE_BLEND) frag += "    fragColor1 = vec4(0.0);\n";
    frag += "    int lod_tile = 0;\n";
    frag += "    float lodFrac = 1.0;\n";
    frag += GeneratePrimDepthCode(f);
    frag += GenerateTextureSampleCode(f);
    frag += GenerateYUVConversionCode(f);
    frag += GenerateLODCode(f);
    frag += GenerateDetailSharpenCode(f);
    frag += GenerateNoiseCode(f);
    frag += "    vec4 shadeColor = uScreenSpaceTriangle == 0 ? vColor : vColorNP;\n";
    frag += GenerateCombinerCode(key.mux, f);
    frag += GenerateAlphaTestCode(f);                // N64: alpha compare on RAW combiner output
    frag += GenerateDitherCode(f, key.otherModeH);   // N64: dither combiner output before coverage
    frag += GenerateCoverageCode(f);                  // CVG_X_ALPHA uses post-dither alpha
    std::string blenderCode1 = GenerateBlenderCode(key.blenderBits, key.otherModeL, f);

    // When MLE blender + N64 depth compare are both active, the depth
    // compare opens the interlock and sets n64DepthPassed; the blender
    // reads the flag, conditionally blends, and closes the interlock.
    // Depth compare must therefore be emitted BEFORE the blender.
    bool mleDepthInterlock1 = (f & SF_MLE_BLENDER) &&
                              (f & SF_N64_DEPTH_COMPARE) &&
                              (f & SF_DEPTH_COMPARE) &&
                              !(f & SF_DEPTH_RENDER);
    if (mleDepthInterlock1) {
        frag += GenerateDepthCompareCode(f);
    }
    frag += blenderCode1;

    // Debug output override: isolate combiner inputs for visual debugging.
    // uDebugOutput: 0=normal, 1=texel0, 2=shade, 3=primColor, 4=envColor, 5=UVs, 6=depth, 7=alpha
    frag += "    if (uDebugOutput == 1) combined = vec4(texel0.rgb, 1.0);\n";
    frag += "    else if (uDebugOutput == 2) combined = vec4(shadeColor.rgb, 1.0);\n";
    frag += "    else if (uDebugOutput == 3) combined = uPrimColor;\n";
    frag += "    else if (uDebugOutput == 4) combined = uEnvColor;\n";
    frag += "    else if (uDebugOutput == 5) combined = vec4(vTexCoord0, 0.0, 1.0);\n";
    frag += "    else if (uDebugOutput == 6) combined = vec4(vec3(gl_FragCoord.z), 1.0);\n";
    frag += "    else if (uDebugOutput == 7) combined = vec4(vec3(combined.a), 1.0);\n";

    frag += "    fragColor = combined;\n";

    if (f & SF_DEPTH_RENDER) {
        // Depth render: fragColor.r is a depth value — skip fog/dither/mono
        frag += GenerateDepthRenderCode(f);
    } else {
        // Fog fallback: on real N64 fog is applied by the blender.
        // Only add the separate fog pass when the shader blender was skipped.
        if (blenderCode1.empty()) {
            frag += GenerateFogCode(f);
        }
        if (!mleDepthInterlock1)
            frag += GenerateDepthCompareCode(f);
        frag += GenerateMonochromeCode(f);
    }
    frag += "}\n";
    tmpl.fragmentBody = std::move(frag);

    return tmpl;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: VERTEX SHADER
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateVertexShader(uint64_t /*features*/) const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        std::string src;
        src += GetVertexInputs();
        src += GetVertexOutputs();
        src += GetCommonUniforms();
        src +=
            "VSOutput main(VSInput input) {\n"
            "    VSOutput output;\n"
            "    output.position  = mul(uMVPMatrix, input.aPosition);\n"
            "    output.vColor    = input.aColor;\n"
            "    output.vTexCoord0= input.aTexCoord0;\n"
            "    output.vTexCoord1= input.aTexCoord1;\n"
            "    output.vFog      = input.aFog;\n"
            "    output.vClipW    = input.aClipW;\n"
            "    output.vBaryCoord= float4(input.aBaryCoord, 1.0 - input.aBaryCoord.x - input.aBaryCoord.y, 0.5);\n"
            "    output.vDepth    = output.position.z / output.position.w;\n"
            "    if (uFogUsage == 1)\n"
            "        output.vColor.a = output.vFog;\n"
            "    else if (uFogUsage == 2)\n"
            "        output.vColor.rgb = float3(output.vFog, output.vFog, output.vFog);\n"
            "    output.vColorNP  = output.vColor;\n"
            "    return output;\n"
            "}\n";
        return src;
    }
    std::string src;
    src += GetVersionDirective();
    src += GetVertexInputs();
    src += GetVertexOutputs();
    src += GetCommonUniforms();
    src +=
        "void main() {\n"
        "    gl_Position = uMVPMatrix * aPosition;\n"
        "    vColor      = aColor;\n"
        "    vTexCoord0  = aTexCoord0;\n"
        "    vTexCoord1  = aTexCoord1;\n"
        "    vFog        = aFog;\n"
        "    vClipW      = aClipW;\n"
        "    vBaryCoord  = vec4(aBaryCoord, 1.0 - aBaryCoord.x - aBaryCoord.y, 0.5);\n"
        "    vDepth      = gl_Position.z / gl_Position.w;\n"
        "    if (uFogUsage == 1)\n"
        "        vColor.a = vFog;\n"
        "    else if (uFogUsage == 2)\n"
        "        vColor.rgb = vec3(vFog);\n"
        "    vColorNP    = vColor;\n"
        "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: FRAGMENT SHADER
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateFragmentShader(const CombinerKey& key) const {
    // ── Shadow map: specialised depth → TLUT → fog-color overlay ─────
    if (key.features & SF_SHADOW_MAP) {
        return GenerateShadowMapShader();
    }
    if (m_language == ShaderLanguage::HLSL_SM5) {
        std::string src;
        src += GetHLSLCompatPreamble();
        src += GetFragmentInputs();
        src += GetFragmentOutputs(key.features);
        src += GetCommonUniforms();
        src += GetTextureUniforms();
        // ROV image declarations for MLE/depth-compare (D3D11.3 RasterizerOrderedViews)
        if (key.features & SF_N64_DEPTH_COMPARE) {
            src += "RasterizerOrderedTexture2D<float> uZImageImg : register(u0);\n"
                   "RasterizerOrderedTexture2D<float> uDeltaZImageImg : register(u1);\n";
        }
        if (key.features & SF_MLE_BLENDER) {
            src += "RasterizerOrderedTexture2D<float4> uColorImage : register(u2);\n"
                   "RasterizerOrderedTexture2D<uint> uCoverageImage : register(u3);\n";
        }
        if (key.features & (SF_N64_DEPTH_COMPARE | SF_MLE_BLENDER)) {
            src += "#define BEGIN_INTERLOCK()\n"
                   "#define END_INTERLOCK()\n"
                   "#define IMG_LOAD_F4(img, coord) ((img)[(coord)])\n"
                   "#define IMG_LOAD_F(img, coord) ((img)[(coord)])\n"
                   "#define IMG_LOAD_U(img, coord) ((img)[(coord)])\n"
                   "#define IMG_STORE_F4(img, coord, val) (img)[(coord)] = (val)\n"
                   "#define IMG_STORE_F(img, coord, val) (img)[(coord)] = (val)\n"
                   "#define IMG_STORE_U(img, coord, val) (img)[(coord)] = (val)\n";
        }
        if (key.features & SF_TEXTURE_3POINT)
            src += GenerateN64TexFilterCode();
        src += "PSOutput main(PSInput input) {\n";
        src += "    float4 vColor = input.vColor;\n";
        src += "    float4 vColorNP = input.vColorNP;\n";
        src += "    float2 vTexCoord0 = input.vTexCoord0;\n";
        src += "    float2 vTexCoord1 = input.vTexCoord1;\n";
        src += "    float vFog = input.vFog;\n";
        src += "    float vDepth = input.vDepth;\n";
        src += "    float vClipW = input.vClipW;\n";
        src += "    float4 vBaryCoord = input.vBaryCoord;\n";
        src += "    float4 fragColor;\n";
        if (key.features & SF_DUAL_SOURCE_BLEND)
            src += "    float4 fragColor1 = float4(0,0,0,0);\n";
        src += "    int lod_tile = 0;\n";
        src += "    float lodFrac = 1.0;\n";
        src += GeneratePrimDepthCode(key.features);
        src += GenerateTextureSampleCode(key.features);
        src += GenerateYUVConversionCode(key.features);
        src += GenerateLODCode(key.features);
        src += GenerateDetailSharpenCode(key.features);
        src += GenerateNoiseCode(key.features);
        src += "    float4 shadeColor = uScreenSpaceTriangle == 0 ? vColor : vColorNP;\n";
        src += GenerateCombinerCode(key.mux, key.features);
        src += GenerateAlphaTestCode(key.features);
        src += GenerateDitherCode(key.features, key.otherModeH);
        src += GenerateCoverageCode(key.features);
        std::string blenderCode2 = GenerateBlenderCode(key.blenderBits, key.otherModeL, key.features);
        src += blenderCode2;
        src += "    fragColor = combined;\n";
        if (key.features & SF_DEPTH_RENDER) {
            src += GenerateDepthRenderCode(key.features);
        } else {
            // Fog fallback: on real N64 fog is applied by the blender.
            // Only add the separate fog pass when the shader blender was skipped.
            if (blenderCode2.empty()) {
                src += GenerateFogCode(key.features);
            }
            src += GenerateDepthCompareCode(key.features);
            src += GenerateMonochromeCode(key.features);
        }
        src += "    PSOutput psOut;\n";
        src += "    psOut.fragColor = fragColor;\n";
        if (key.features & SF_DUAL_SOURCE_BLEND)
            src += "    psOut.fragColor1 = fragColor1;\n";
        if (key.features & SF_DEPTH_RENDER)
            src += "    psOut.fragDepth = depthRenderZ;\n";
        else if (key.features & SF_DEPTH_SOURCE_PRIM)
            src += "    psOut.fragDepth = uPrimDepth;\n";
        src += "    return psOut;\n";
        src += "}\n";
        return src;
    }
    std::string src;
    src += GetVersionDirective();
    // Intel Iris Xe workaround: skip interlock for depth compare only.
    // Only enable when MLE blender needs it.
    bool needsInterlockFS = (key.features & SF_MLE_BLENDER) != 0;
    if (needsInterlockFS) {
        src += "#extension GL_ARB_fragment_shader_interlock : enable\n"
               "layout(pixel_interlock_ordered) in;\n";
    }
    src += GetFragmentInputs();
    src += GetFragmentOutputs(key.features);
    src += GetCommonUniforms();
    src += GetTextureUniforms();
    if (key.features & SF_N64_DEPTH_COMPARE) {
        src += "layout(binding = 0, r32f) uniform restrict image2D uZImageImg;\n"
               "layout(binding = 1, r32f) uniform restrict image2D uDeltaZImageImg;\n";
    }
    if (key.features & SF_MLE_BLENDER) {
        src += "layout(binding = 2, rgba8) uniform restrict image2D uColorImage;\n"
               "layout(binding = 3, r8ui) uniform restrict uimage2D uCoverageImage;\n";
    }
    if (key.features & (SF_N64_DEPTH_COMPARE | SF_MLE_BLENDER)) {
        if (needsInterlockFS) {
            src += "#define BEGIN_INTERLOCK() beginInvocationInterlockARB()\n"
                   "#define END_INTERLOCK() endInvocationInterlockARB()\n";
        } else {
            src += "#define BEGIN_INTERLOCK()\n"
                   "#define END_INTERLOCK()\n";
        }
        src += "#define IMG_LOAD_F4(img, coord) imageLoad(img, coord)\n"
               "#define IMG_LOAD_F(img, coord) imageLoad(img, coord).r\n"
               "#define IMG_LOAD_U(img, coord) imageLoad(img, coord).r\n"
               "#define IMG_STORE_F4(img, coord, val) imageStore(img, coord, val)\n"
               "#define IMG_STORE_F(img, coord, val) imageStore(img, coord, vec4(val, 0.0, 0.0, 0.0))\n"
               "#define IMG_STORE_U(img, coord, val) imageStore(img, coord, uvec4(val, 0u, 0u, 0u))\n";
    }
    if (key.features & SF_TEXTURE_3POINT)
        src += GenerateN64TexFilterCode();
    src += "void main() {\n";
    // Declare texel0 default before GenerateDetailSharpenCode which may use it
    if (!(key.features & SF_TEXTURE_0)) src += "    vec4 texel0 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    if (key.features & SF_DUAL_SOURCE_BLEND) src += "    fragColor1 = vec4(0.0);\n";
    src += "    int lod_tile = 0;\n";
    src += "    float lodFrac = 1.0;\n";
    src += GeneratePrimDepthCode(key.features);
    src += GenerateTextureSampleCode(key.features);
    src += GenerateYUVConversionCode(key.features);
    src += GenerateLODCode(key.features);
    src += GenerateDetailSharpenCode(key.features);
    src += GenerateNoiseCode(key.features);
    src += "    vec4 shadeColor = uScreenSpaceTriangle == 0 ? vColor : vColorNP;\n";
    src += GenerateCombinerCode(key.mux, key.features);
    src += GenerateCoverageCode(key.features);
    src += GenerateAlphaTestCode(key.features);
    src += GenerateDitherCode(key.features, key.otherModeH);  // N64: dither before blender
    std::string blenderCode3 = GenerateBlenderCode(key.blenderBits, key.otherModeL, key.features);

    // When MLE blender + N64 depth compare are both active, the depth
    // compare opens the interlock and sets n64DepthPassed; the blender
    // reads the flag, conditionally blends, and closes the interlock.
    // Depth compare must therefore be emitted BEFORE the blender.
    bool mleDepthInterlock = (key.features & SF_MLE_BLENDER) &&
                             (key.features & SF_N64_DEPTH_COMPARE) &&
                             (key.features & SF_DEPTH_COMPARE) &&
                             !(key.features & SF_DEPTH_RENDER);
    if (mleDepthInterlock) {
        src += GenerateDepthCompareCode(key.features);
        src += blenderCode3;
    } else {
        src += blenderCode3;
    }
    src += "    fragColor = combined;\n";
    if (key.features & SF_DEPTH_RENDER) {
        src += GenerateDepthRenderCode(key.features);
    } else {
        // Fog fallback: on real N64 fog is applied by the blender.
        // Only add the separate fog pass when the shader blender was skipped.
        if (blenderCode3.empty()) {
            src += GenerateFogCode(key.features);
        }
        if (!mleDepthInterlock)
            src += GenerateDepthCompareCode(key.features);
        src += GenerateMonochromeCode(key.features);
    }
    src += "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SHADOW MAP FRAGMENT SHADER
// Reads the GL depth buffer, converts via ZLUT to N64 Z16, looks up a palette
// (TLUT) from TMEM to produce a fog-coloured shadow overlay.
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateShadowMapShader() const {
    std::string src;
    if (m_language == ShaderLanguage::HLSL_SM5) {
        src += GetHLSLCompatPreamble();
        src += GetFragmentInputs();
        src +=
            "struct PSOutput {\n"
            "    float4 fragColor : SV_Target0;\n"
            "};\n";
        src += GetCommonUniforms();
        src += GetTextureUniforms();
        src +=
            "PSOutput main(PSInput input) {\n"
            "    // Read GL depth at this fragment\n"
            "    int2 coord = int2(input.position.xy);\n"
            "    float bufZ = tDepthTexture.Load(int3(coord, 0)).r;\n"
            "    // Map [0,1] depth to 0..255 TLUT index\n"
            "    int index = min(255, int(bufZ * 255.0));\n"
            "    // Look up shadow alpha from TLUT (palette loaded from TMEM)\n"
            "    float alpha = tTLUT0.Load(int3(index, 0, 0)).r;\n"
            "    PSOutput psOut;\n"
            "    psOut.fragColor = float4(uFogColor.rgb, alpha);\n"
            "    return psOut;\n"
            "}\n";
        return src;
    }
    // GLSL path (OpenGL 4.6)
    src += GetVersionDirective();
    src += GetFragmentInputs();
    src += "out vec4 fragColor;\n";
    src += GetCommonUniforms();
    src += GetTextureUniforms();
    src +=
        "void main() {\n"
        "    // Read GL depth at this fragment\n"
        "    ivec2 coord = ivec2(gl_FragCoord.xy);\n"
        "    float bufZ = texelFetch(uDepthTexture, coord, 0).r;\n"
        "    // Map [0,1] depth to 0..255 TLUT index\n"
        "    int index = min(255, int(bufZ * 255.0));\n"
        "    // Look up shadow alpha from TLUT (palette loaded from TMEM)\n"
        "    float alpha = texelFetch(uTLUT0, ivec2(index, 0), 0).r;\n"
        "    fragColor = vec4(uFogColor.rgb, alpha);\n"
        "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: GEOMETRY SHADER
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateGeometryShader(uint64_t features) const {
    if (!(features & SF_DEBUG_WIREFRAME)) return "";

    std::string src;
    src += GetVersionDirective();
    src +=
        "layout(triangles) in;\n"
        "layout(line_strip, max_vertices = 4) out;\n"
        "\n"
        "void main() {\n"
        "    for (int i = 0; i < 3; ++i) {\n"
        "        gl_Position = gl_in[i].gl_Position;\n"
        "        EmitVertex();\n"
        "    }\n"
        "    gl_Position = gl_in[0].gl_Position;\n"
        "    EmitVertex();\n"
        "    EndPrimitive();\n"
        "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: DEPTH-ONLY SHADERS
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateDepthOnlyVertex() const {
    std::string src;
    src += GetVersionDirective();
    src +=
        "layout(location = 0) in vec4 aPosition;\n"
        "\n"
        "uniform mat4 uMVPMatrix;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = uMVPMatrix * aPosition;\n"
        "}\n";
    return src;
}

std::string ShaderGenerator::GenerateDepthOnlyFragment() const {
    std::string src;
    src += GetVersionDirective();
    src +=
        "void main() {\n"
        "    // Depth is written automatically\n"
        "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: POST-PROCESS SHADERS
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GeneratePostProcessVertex() const {
    std::string src;
    src += GetVersionDirective();
    src +=
        "layout(location = 0) in vec2 aPosition;\n"
        "layout(location = 1) in vec2 aTexCoord;\n"
        "\n"
        "out vec2 vTexCoord;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "    vTexCoord   = aTexCoord;\n"
        "}\n";
    return src;
}

std::string ShaderGenerator::GeneratePostProcessFragment(uint64_t features) const {
    std::string src;
    src += GetVersionDirective();
    src +=
        "in vec2 vTexCoord;\n"
        "layout(location = 0) out vec4 fragColor;\n"
        "\n"
        "uniform sampler2D uFBTexture;\n"
        "uniform vec2 uScreenSize;\n"
        "uniform vec2 uInvScreenSize;\n"
        "\n"
        "void main() {\n"
        "    fragColor = texture(uFBTexture, vTexCoord);\n";

    if (features & SF_GAMMA_CORRECT)
        src += GenerateGammaCorrectionCode();
    if (features & SF_AA_ENABLE)
        src +=
            "    // N64-style anti-aliasing: 3x3 weighted blur on coverage edges\n"
            "    {\n"
            "        vec4 center = fragColor;\n"
            "        vec4 sum = center * 4.0;\n"
            "        sum += texture(uFBTexture, vTexCoord + vec2(-uInvScreenSize.x, 0.0));\n"
            "        sum += texture(uFBTexture, vTexCoord + vec2( uInvScreenSize.x, 0.0));\n"
            "        sum += texture(uFBTexture, vTexCoord + vec2(0.0, -uInvScreenSize.y));\n"
            "        sum += texture(uFBTexture, vTexCoord + vec2(0.0,  uInvScreenSize.y));\n"
            "        // Edge detection: blend only where neighbors differ significantly\n"
            "        vec4 avg = sum / 8.0;\n"
            "        float diff = length(center.rgb - avg.rgb);\n"
            "        float blend = smoothstep(0.02, 0.15, diff);\n"
            "        fragColor = mix(center, avg, blend);\n"
            "    }\n";
    if (features & SF_DIVOT_ENABLE)
        src +=
            "    // Divot filter: 3-pixel horizontal median to remove single-pixel errors\n"
            "    {\n"
            "        vec4 left  = texture(uFBTexture, vTexCoord + vec2(-uInvScreenSize.x, 0.0));\n"
            "        vec4 right = texture(uFBTexture, vTexCoord + vec2( uInvScreenSize.x, 0.0));\n"
            "        // Per-channel median of (left, center, right)\n"
            "        vec4 mn = min(min(left, right), fragColor);\n"
            "        vec4 mx = max(max(left, right), fragColor);\n"
            "        fragColor = left + right + fragColor - mn - mx;\n"
            "    }\n";

    src += "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: UI OVERLAY SHADERS
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateUIVertex() const {
    std::string src;
    src += GetVersionDirective();
    src +=
        "layout(location = 0) in vec2 aPosition;\n"
        "layout(location = 1) in vec2 aTexCoord;\n"
        "layout(location = 2) in vec4 aColor;\n"
        "\n"
        "uniform mat4 uProjection;\n"
        "\n"
        "out vec2 vTexCoord;\n"
        "out vec4 vColor;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);\n"
        "    vTexCoord   = aTexCoord;\n"
        "    vColor      = aColor;\n"
        "}\n";
    return src;
}

std::string ShaderGenerator::GenerateUIFragment() const {
    std::string src;
    src += GetVersionDirective();
    src +=
        "in vec2 vTexCoord;\n"
        "in vec4 vColor;\n"
        "\n"
        "layout(location = 0) out vec4 fragColor;\n"
        "\n"
        "uniform sampler2D uTexture0;\n"
        "\n"
        "void main() {\n"
        "    fragColor = vColor * texture(uTexture0, vTexCoord);\n"
        "}\n";
    return src;
}

}}} // namespace Nova64::Video::Shader


