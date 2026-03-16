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
        "float4 texture(Texture2D t, SamplerState s, float2 uv) { return t.Sample(s, uv); }\n"
        "#define texture(sampler, uv) sampler.Sample(sLinear, uv)\n"
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
            "    float4x4 uTexMatrix0;\n"
            "    float4x4 uTexMatrix1;\n"
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
            "    float  _pad1;\n"
            "    float2 uResolutionScale;\n"
            "};\n";
    }
    return
        "uniform mat4  uMVPMatrix;\n"
        "uniform mat4  uModelView;\n"
        "uniform mat4  uProjection;\n"
        "uniform mat4  uTexMatrix0;\n"
        "uniform mat4  uTexMatrix1;\n"
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
        "uniform vec2  uResolutionScale;\n";
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
            "};\n";
    }
    return
        "layout(location = 0) in vec4 aPosition;\n"
        "layout(location = 1) in vec4 aColor;\n"
        "layout(location = 2) in vec2 aTexCoord0;\n"
        "layout(location = 3) in vec2 aTexCoord1;\n"
        "layout(location = 4) in float aFog;\n"
        "layout(location = 5) in float aClipW;\n";
}

std::string ShaderGenerator::GetVertexOutputs() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "struct VSOutput {\n"
            "    float4 position  : SV_Position;\n"
            "    float4 vColor    : COLOR;\n"
            "    float2 vTexCoord0: TEXCOORD0;\n"
            "    float2 vTexCoord1: TEXCOORD1;\n"
            "    float  vFog      : FOG;\n"
            "    float  vDepth    : DEPTH_INTERP;\n"
            "    float  vClipW    : TEXCOORD2;\n"
            "};\n";
    }
    return
        "out vec4 vColor;\n"
        "out vec2 vTexCoord0;\n"
        "out vec2 vTexCoord1;\n"
        "out float vFog;\n"
        "out float vDepth;\n"
        "out float vClipW;\n";
}

std::string ShaderGenerator::GetFragmentInputs() const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        return
            "struct PSInput {\n"
            "    float4 position  : SV_Position;\n"
            "    float4 vColor    : COLOR;\n"
            "    float2 vTexCoord0: TEXCOORD0;\n"
            "    float2 vTexCoord1: TEXCOORD1;\n"
            "    float  vFog      : FOG;\n"
            "    float  vDepth    : DEPTH_INTERP;\n"
            "    float  vClipW    : TEXCOORD2;\n"
            "};\n";
    }
    return
        "in vec4 vColor;\n"
        "in vec2 vTexCoord0;\n"
        "in vec2 vTexCoord1;\n"
        "in float vFog;\n"
        "in float vDepth;\n"
        "in float vClipW;\n";
}

std::string ShaderGenerator::GetFragmentOutputs(uint64_t features) const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        if (features & SF_DEPTH_SOURCE_PRIM) {
            return
                "struct PSOutput {\n"
                "    float4 fragColor : SV_Target0;\n"
                "    float  fragDepth : SV_Depth;\n"
                "};\n";
        }
        return
            "struct PSOutput {\n"
            "    float4 fragColor : SV_Target0;\n"
            "};\n";
    }
    return "layout(location = 0) out vec4 fragColor;\n";
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
        "vColor.rgb", "uEnvColor.rgb", "vec3(1.0)", "vec3(noise)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"
    };
    static const char* const kRGBInputB[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "vColor.rgb", "uEnvColor.rgb", "uCenterColor.rgb", "vec3(uK4)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"
    };
    static const char* const kRGBInputC[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "vColor.rgb", "uEnvColor.rgb", "uScaleColor.rgb", "vec3(combined.a)",
        "vec3(texel0.a)", "vec3(texel1.a)", "vec3(uPrimColor.a)", "vec3(vColor.a)",
        "vec3(uEnvColor.a)", "vec3(uPrimLOD)", "vec3(uPrimLODFrac)", "vec3(uK5)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)",
        "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"
    };
    static const char* const kRGBInputD[] = {
        "combined.rgb", "texel0.rgb", "texel1.rgb", "uPrimColor.rgb",
        "vColor.rgb", "uEnvColor.rgb", "vec3(1.0)", "vec3(0.0)"
    };
    static const char* const kAlphaABD[] = {
        "combined.a", "texel0.a", "texel1.a", "uPrimColor.a",
        "vColor.a", "uEnvColor.a", "1.0", "0.0"
    };
    static const char* const kAlphaC[] = {
        "uPrimLOD", "texel0.a", "texel1.a", "uPrimColor.a",
        "vColor.a", "uEnvColor.a", "uPrimLODFrac", "0.0"
    };

    uint32_t hi = static_cast<uint32_t>(mux >> 32);
    uint32_t lo = static_cast<uint32_t>(mux & 0xFFFFFFFF);

    std::ostringstream ss;

    // texel1 / noise defaults (texel0 is declared at the top of main())
    if (!(features & SF_TEXTURE_1))
        ss << "    vec4 texel1 = texel0;\n";
    if (!(features & SF_NOISE_ENABLE))
        ss << "    float noise = 0.0;\n";

    ss << "    vec4 combined = vec4(0.0);\n";

    auto emitCycle = [&](int cycle, bool isFinal) {
        // N64 SET_COMBINE (0xFC) mux encoding per libultra gbi.h:
        // Verified against EncodeCombineMode macro in src/Combiner.h.
        //
        //   hi (upper 32 bits):
        //     [23:20]=saRGB0(A) [19:15]=mRGB0(C) [14:12]=saA0(A) [11:9]=mA0(C)
        //     [8:5]=saRGB1(A)  [4:0]=mRGB1(C)
        //
        //   lo (lower 32 bits):
        //     [31:28]=sbRGB0(B) [27:24]=sbRGB1(B) [23:21]=saA1(A) [20:18]=mA1(C)
        //     [17:15]=aRGB0(D) [14:12]=sbA0(B)   [11:9]=aA0(D)
        //     [8:6]=aRGB1(D)   [5:3]=sbA1(B)     [2:0]=aA1(D)
        int aRGB, bRGB, cRGB, dRGB, aA, bA, cA, dA;
        if (cycle == 0) {
            aRGB = (hi >> 20) & 0xF;     // saRGB0 [23:20]
            cRGB = (hi >> 15) & 0x1F;    // mRGB0  [19:15]
            aA   = (hi >> 12) & 0x7;     // saA0   [14:12]
            cA   = (hi >>  9) & 0x7;     // mA0    [11:9]
            bRGB = (lo >> 28) & 0xF;     // sbRGB0 [31:28]
            dRGB = (lo >> 15) & 0x7;     // aRGB0  [17:15]
            bA   = (lo >> 12) & 0x7;     // sbA0   [14:12]
            dA   = (lo >>  9) & 0x7;     // aA0    [11:9]
        } else {
            aRGB = (hi >>  5) & 0xF;     // saRGB1 [8:5]
            cRGB = (hi >>  0) & 0x1F;    // mRGB1  [4:0]
            bRGB = (lo >> 24) & 0xF;     // sbRGB1 [27:24]
            aA   = (lo >> 21) & 0x7;     // saA1   [23:21]
            cA   = (lo >> 18) & 0x7;     // mA1    [20:18]
            dRGB = (lo >>  6) & 0x7;     // aRGB1  [8:6]
            bA   = (lo >>  3) & 0x7;     // sbA1   [5:3]
            dA   = (lo >>  0) & 0x7;     // aA1    [2:0]
        }
        ss << "\n    // Cycle " << cycle << ": (A - B) * C + D\n";
        // On real N64 hardware the intermediate value between cycle 0 and cycle 1
        // is NOT clamped — it is a signed 9-bit value.  Only the final output is
        // clamped to [0,1].  Using clamp() after cycle 0 in 2-cycle mode breaks
        // combiners that rely on negative intermediate results (e.g. the standard
        // OoT alpha-modulated texture formula).
        if (isFinal) {
            ss << "    combined.rgb = clamp(("
               << kRGBInputA[aRGB & 0xF] << " - "
               << kRGBInputB[bRGB & 0xF] << ") * "
               << kRGBInputC[cRGB & 0x1F] << " + "
               << kRGBInputD[dRGB & 0x7] << ", 0.0, 1.0);\n";
            ss << "    combined.a = clamp(("
               << kAlphaABD[aA & 0x7] << " - "
               << kAlphaABD[bA & 0x7] << ") * "
               << kAlphaC[cA & 0x7] << " + "
               << kAlphaABD[dA & 0x7] << ", 0.0, 1.0);\n";
        } else {
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
        }
    };

    if (features & SF_FILL_RECT) {
        ss << "    combined = uFillColor;\n";
    } else if (features & SF_COPY_MODE) {
        ss << "    combined = texel0;\n";
    } else {
        bool is2cycle = (features & SF_2CYCLE) != 0;
        if (is2cycle) {
            emitCycle(0, false);   // Cycle 0 intermediate (unclamped)
            emitCycle(1, true);    // Cycle 1 final (clamped)
        } else {
            // In 1-cycle mode N64 hardware uses cycle 1 fields, not cycle 0.
            // COMBINED inputs read as zero (already handled by combined init).
            emitCycle(1, true);
        }
    }
    return ss.str();
}

std::string ShaderGenerator::GenerateBlenderCode(uint32_t blenderBits, uint64_t features) const {
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

    // In 2-cycle mode the shader only executes cycle 1 (the internal blend,
    // e.g. fog).  Cycle 2 composites with the framebuffer — that's what GL
    // hardware blending handles.
    //
    // If cycle 1 references memColor, skip the shader blender entirely and
    // let GL hardware blend handle everything.  Attempting to read the
    // framebuffer into the shader via a texture snapshot produces stale/white
    // artefacts and disables GL blend, breaking normal compositing.
    bool needsMemColor = (p1 == 1) || (m1 == 1) || (b1 == 1);
    if (needsMemColor) return "";

    // Fog blender degenerate case: when the blender uses the standard fog
    // pattern (P=FOG_COLOR, A=SHADE_ALPHA) but G_FOG is NOT set in geometric
    // mode, the RSP doesn't compute fog factors so shade_alpha stays at the
    // original vertex alpha.  For fully opaque geometry (alpha=1.0) the formula
    // degenerates to fogColor*1 + CC*0 = fogColor, wiping out the combiner.
    // Skip the blender only when B=1-A (so B=0 when A=1), making it a true
    // passthrough.  When B=One or B=MemAlpha, the formula still contributes
    // M (the combined color), so we must NOT skip.
    bool fogActive = (features & SF_FOG) != 0;
    if (!fogActive && p1 == 3 && a1 == 2 && b1 == 0) {
        // FOG_COLOR * SHADE_ALPHA + M * (1-SHADE_ALPHA) with no fog → passthrough
        // (only safe when B=1-A, because with alpha=1.0 the M term vanishes)
        return "";
    }

    // No memColor needed — emit the blender formula in the shader
    // (e.g. fog blending: P=fogColor, A=shadeAlpha, M=CC, B=1-A)
    std::ostringstream preamble;
    preamble << "\n    // Shader blender (no framebuffer read needed)\n";
    preamble << "    vec4 memColor = vec4(0.0);\n";

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

    // A alpha sources — cycle 2 source 0 uses cycle 1 output alpha
    // On real N64: when G_FOG is enabled the RSP overwrites shade_alpha with
    // the computed fog factor.  In this emulator vColor.a keeps the original
    // vertex alpha while vFog carries the fog factor, so we route A source 2
    // to vFog when fog is active.
    auto aAlphaSel = [fogActive](int s, bool isCycle2) -> const char* {
        switch (s) {
        case 0: return isCycle2 ? "blenderOut.a" : "combined.a";
        case 1: return "uFogColor.a";
        case 2: return fogActive ? "vFog" : "vColor.a";  // shade alpha (fog-overwritten on N64)
        case 3: return "0.0";
        default: return "0.0";
        }
    };

    // Emit a single blender cycle
    std::ostringstream ss;
    ss << preamble.str();

    // Emit cycle 1 blender: out = (P * A + M * B) / (A + B)
    // In both 1-cycle and 2-cycle modes we only run cycle 1 in the shader.
    // Cycle 2 (framebuffer compositing) is handled by GL blend hardware.
    ss << "\n    // Blender cycle 1: out = (P * A + M * B) / (A + B)\n";
    ss << "    {\n";
    ss << "        vec3 bP = " << pmColorSel(p1, false) << ";\n";
    ss << "        float bA = " << aAlphaSel(a1, false) << ";\n";
    ss << "        vec3 bM = " << pmColorSel(m1, false) << ";\n";

    // B source: 0=(1-A), 1=memColor.a, 2=1.0, 3=0.0
    switch (b1) {
    case 0:  ss << "        float bB = 1.0 - bA;\n"; break;
    case 1:  ss << "        float bB = memColor.a;\n"; break;
    case 2:  ss << "        float bB = 1.0;\n"; break;
    default: ss << "        float bB = 0.0;\n"; break;
    }

    ss << "        float denom = max(bA + bB, 0.001);\n";
    ss << "        combined.rgb = clamp((bP * bA + bM * bB) / denom, 0.0, 1.0);\n";
    ss << "    }\n";

    return ss.str();
}

std::string ShaderGenerator::GenerateAlphaTestCode(uint64_t features) const {
    if (!(features & SF_ALPHA_TEST)) return "";
    // N64 hardware: alpha compare tests the combiner output (combined.a) BEFORE
    // the blender runs.  This must be called before GenerateBlenderCode.
    if (features & SF_NOISE_ENABLE) {
        // Alpha dither: compare against random noise instead of fixed threshold
        return "    if (combined.a < noise) discard;\n";
    }
    return "    if (combined.a < uAlphaRef) discard;\n";
}

std::string ShaderGenerator::GenerateFogCode(uint64_t features) const {
    if (!(features & SF_FOG)) return "";
    // vFog is the RSP-computed fog factor: 0 = no fog, 1 = full fog.
    // Use it directly — the RSP already applied fogMultiplier and fogOffset.
    return
        "    fragColor.rgb = mix(fragColor.rgb, uFogColor.rgb, vFog);\n";
}

std::string ShaderGenerator::GenerateNoiseCode(uint64_t features) const {
    if (!(features & SF_NOISE_ENABLE)) return "";
    return
        "    float noise = texture(uNoiseTexture, (gl_FragCoord.xy\n"
        "        + vec2(float(uFrameCount) * 37.0, float(uFrameCount) * 17.0)) / 64.0).r;\n";
}

std::string ShaderGenerator::GenerateTextureSampleCode(uint64_t features) const {
    std::string code;
    if (features & SF_TEXTURE_0) {
        // Perspective-correct texture coordinates: divide by 1/clip_W (vClipW)
        // to recover screen-space texels. For 2D rects, vClipW == 1.0 so this is a no-op.
        code += "    vec2 tc0 = (vClipW > 1e-6) ? vTexCoord0 / vClipW : vTexCoord0;\n";
        code += "    vec2 uv0 = (tc0 * uTexScale0 - uTexOffset0) / max(uTexSize0, vec2(1.0));\n";
        // Apply N64 wrap/mirror/clamp using tile settings
        // uTexClamp0 = (clampS, clampT, mirrorS, mirrorT)
        // uTexMask0 = (maskS, maskT)  � number of mask bits (0 = no mask)
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
        if (features & SF_TEXTURE_3POINT)
            code += "    vec4 texel0 = n64Filter3Point(uTexture0, uv0, uTexSize0, uGPUScale0);\n";
        else
            code += "    vec4 texel0 = texture(uTexture0, uv0 * uGPUScale0);\n";
    }
    if (features & SF_TEXTURE_1) {
        code += "    vec2 tc1 = (vClipW > 1e-6) ? vTexCoord1 / vClipW : vTexCoord1;\n";
        code += "    vec2 uv1 = (tc1 * uTexScale1 - uTexOffset1) / max(uTexSize1, vec2(1.0));\n";
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
    }
    return code;
}

std::string ShaderGenerator::GenerateN64TexFilterCode() const {
      // N64 3-point bilinear filter with GPU padding scaling
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

    if (fracPart.x + fracPart.y < 1.0) {
        return c00 + fracPart.x * (c10 - c00) + fracPart.y * (c01 - c00);
    } else {
        return c11 + (1.0 - fracPart.x) * (c01 - c11) + (1.0 - fracPart.y) * (c10 - c11);
    }
}
)";
}

std::string ShaderGenerator::GenerateDepthCompareCode(uint64_t features) const {
    if (!(features & SF_DEPTH_COMPARE)) return "";

    // When the full N64 Z-image compare is active we do the compare entirely
    // in the fragment shader using the Z-image / deltaZ-image textures and
    // the ZLUT.  The GL depth test is still active as a coarse cull, but
    // the final accept/reject and depth write is handled here to match the
    // N64's non-linear 18-bit depth and per-pixel deltaZ tolerance.
    if (features & SF_N64_DEPTH_COMPARE) {
        // When SF_DEPTH_SOURCE_PRIM is set, use the primitive depth uniform
        // instead of gl_FragCoord.z (which is rasterizer-interpolated and
        // independent of gl_FragDepth).
        const bool primDepth = (features & SF_DEPTH_SOURCE_PRIM) != 0;
        std::string fragZExpr = primDepth
            ? "clamp(uPrimDepth, 0.0, 1.0)"
            : "clamp(gl_FragCoord.z, 0.0, 1.0)";
        std::string srcDZExpr = primDepth
            ? "uPrimDeltaZ"
            : "max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z)))";

        return
            "    // ─── N64-accurate depth compare ───\n"
            "    {\n"
            "        vec2 zCoord = gl_FragCoord.xy * uInvScreenSize;\n"
            "        // Read stored N64 Z (float-encoded Z14) and deltaZ from Z-image\n"
            "        float storedZ  = texture(uZImageTex, zCoord).r;\n"
            "        float storedDZ = texture(uDeltaZImageTex, zCoord).r;\n"
            "\n"
            "        // Convert fragment depth to N64 Z14 via ZLUT\n"
            "        float fragZ = " + fragZExpr + ";\n"
            "        float srcZ  = texture(uZLUT, vec2(fragZ, 0.5)).r;\n"
            "\n"
            "        // Source deltaZ\n"
            "        float srcDZ = " + srcDZExpr + ";\n"
            "        // For prim-depth sources, use the primitive's deltaZ\n"
            "        srcDZ = max(srcDZ, uPrimDeltaZ);\n"
            "\n"
            "        // N64 compare tolerance: max(srcDZ, storedDZ)\n"
            "        float maxDZ = max(srcDZ, storedDZ);\n"
            "\n"
            "        bool zPass = false;\n"
            "        // uZMode: 0=OPAQUE, 1=INTERPENETRATING, 2=TRANSPARENT, 3=DECAL\n"
            "        if (uZMode == 0) {\n"
            "            // OPAQUE: new fragment must be in front of or equal to stored\n"
            "            zPass = (srcZ <= storedZ);\n"
            "        } else if (uZMode == 1) {\n"
            "            // INTERPENETRATING: pass if within deltaZ tolerance\n"
            "            zPass = (srcZ <= (storedZ + maxDZ));\n"
            "        } else if (uZMode == 2) {\n"
            "            // TRANSPARENT: strict less-than (same Z = fail, avoids flicker)\n"
            "            zPass = (srcZ < storedZ);\n"
            "        } else {\n"
            "            // DECAL: pass if within tolerance, never write depth\n"
            "            zPass = (abs(srcZ - storedZ) <= maxDZ);\n"
            "        }\n"
            "\n"
            "        if (!zPass) discard;\n"
            "    }\n";
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
    // On real N64 hardware, CVG_X_ALPHA and ALPHA_CVG_SEL operate independently
    // of AA_EN.  Without AA_EN, pixel coverage is always full (1.0).
    // ALPHA_CVG_SEL replaces the combiner alpha with coverage before the blender,
    // so when AA is off it forces alpha to 1.0 (opaque).  Games like OoT use this
    // for I8 text textures: the combiner alpha carries the texel intensity, but
    // ALPHA_CVG_SEL overrides it so the blender draws at full opacity.
    bool hasAA       = (features & SF_COVERAGE_ENABLE) != 0;
    bool hasCvgAlpha = (features & SF_ALPHA_CVG) != 0;
    bool hasAlphaSel = (features & SF_ALPHA_CVG_SEL) != 0;
    bool hasClrOnCvg = (features & SF_COLOR_ON_COVERAGE) != 0;
    bool hasHiRes    = (features & SF_HIRES) != 0;

    if (!hasAA && !hasCvgAlpha && !hasAlphaSel) return "";

    std::string code;
    code += "    // N64 coverage emulation\n";
    code += "    {\n";

    if (hasAA) {
        if (hasHiRes) {
            // Hi-res / MSAA path: use gl_SampleMaskIn to count sub-pixel coverage.
            // gl_SampleMaskIn[0] contains a bitmask of which MSAA samples this
            // fragment covers.  bitCount gives us true sub-pixel coverage.
            // The N64 encodes coverage as 0-7 (3 bits), we map to 0/8 .. 8/8.
            code += "        #ifdef GL_ARB_sample_shading\n";
            code += "        int coveredSamples = bitCount(gl_SampleMaskIn[0]);\n";
            code += "        int totalSamples = max(gl_NumSamples, 1);\n";
            code += "        float cvg = float(coveredSamples) / float(totalSamples);\n";
            code += "        #else\n";
            code += "        float cvg = 1.0;\n";  // No MSAA fallback: assume full coverage
            code += "        #endif\n";
        } else {
            // Standard path (no MSAA): true sub-pixel coverage is unavailable.
            // The previous fwidth(vColor) heuristic produced false partial-coverage
            // on Gouraud-shaded surfaces (e.g. N64 boot logo), making opaque
            // geometry translucent.  Without MSAA we cannot detect triangle edges
            // in the fragment shader, so default to full coverage.  Edge AA quality
            // requires the MSAA / hi-res path above.
            code += "        float cvg = 1.0;\n";
        }
    } else {
        // No AA: coverage is always full
        code += "        float cvg = 1.0;\n";
    }

    if (hasCvgAlpha) {
        // cvg_x_alpha: multiply coverage by combiner alpha
        code += "        cvg *= combined.a;\n";
    }

    if (hasAlphaSel) {
        // alpha_cvg_sel: replace combiner alpha with coverage (before blender/alpha test)
        code += "        combined.a = cvg;\n";
    } else if (hasAA) {
        // Only modulate alpha by coverage when AA is actually computing sub-pixel values
        code += "        combined.a *= cvg;\n";
    }

    if (hasClrOnCvg) {
        // clr_on_cvg: where coverage is zero, output fill color instead of
        // the combiner result.  On real N64 hardware this happens regardless
        // of AA_EN (though AA_EN affects how coverage is computed).
        code += "        if (cvg < 0.125) { combined = uFillColor; }\n";
    }

    code += "    }\n";
    return code;
}

std::string ShaderGenerator::GenerateDitherCode(uint64_t features) const {
    if (!(features & (SF_DITHER_ENABLE | SF_ALPHA_DITHER))) return "";
    // N64 hardware dithers during 8-bit → 5-bit color conversion:
    //   output_5bit = min((color_8bit + dither_3bit) >> 3, 31)
    // The dither texture stores N64 3-bit values (0-7) normalized to [0,1].
    // RGB and alpha each have independent dither select modes.
    std::string code = "    {\n";
    code += "        float dither = texture(uDitherTexture, gl_FragCoord.xy / 4.0).r * 7.0;\n";
    if (features & SF_DITHER_ENABLE) {
        code += "        vec3 color8 = fragColor.rgb * 255.0;\n";
        code += "        fragColor.rgb = min(floor((color8 + dither) / 8.0), 31.0) / 31.0;\n";
    }
    if (features & SF_ALPHA_DITHER) {
        // Alpha dither: same 8→5-bit quantization applied to the alpha channel.
        // The N64 alpha_dither_sel uses the same or inverted pattern, or noise.
        // The correct pattern is already bound to the dither texture unit by the CPU.
        code += "        float alpha8 = fragColor.a * 255.0;\n";
        code += "        fragColor.a = min(floor((alpha8 + dither) / 8.0), 31.0) / 31.0;\n";
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
// is used in the combiner (uPrimLOD becomes the LOD fraction).
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateLODCode(uint64_t features) const {
    if (!(features & SF_TEXTURE_LOD)) return "";
    return
        "    // LOD: compute mip-level fraction from screen-space texcoord gradients.\n"
        "    // Tile 0 = base mip, tile 1 = next mip level.  The N64 combiner blends\n"
        "    // between them using the LOD_FRACTION input (mapped to uPrimLOD here).\n"
        "    // Declare a local 'uPrimLOD' that shadows the uniform with the GPU-computed\n"
        "    // gradient-based value so the combiner performs real mipmap blending.\n"
        "    float uPrimLOD;\n"
        "    {\n"
        "        vec2 dSDX = dFdx(tc0 * uTexSize0);\n"
        "        vec2 dSDY = dFdy(tc0 * uTexSize0);\n"
        "        float lodMag = max(length(dSDX), length(dSDY));\n"
        "        float lod = log2(max(lodMag, 1.0));\n"
        "        uPrimLOD = clamp(fract(lod), 0.0, 1.0);\n"
        "    }\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// DETAIL / SHARPEN TEXTURE
// Detail: Multiply base texture by a repeating detail texture at higher freq.
// Sharpen: Extrapolate the mipmap blending past 1.0 to sharpen transitions.
// Used heavily in Zelda OoT (terrain), GoldenEye, Star Fox 64.
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateDetailSharpenCode(uint64_t features) const {
    if (!(features & (SF_TEXTURE_DETAIL | SF_TEXTURE_SHARPEN))) return "";

    std::string code;
    if (features & SF_TEXTURE_DETAIL) {
        code +=
            "    // Detail texture — high-frequency overlay\n"
            "    {\n"
            "        // Detail texture uses texel1 at a higher frequency (4x)\n"
            "        vec2 detailUV = vTexCoord1 * uTexScale1 * 4.0;\n"
            "        vec4 detailTex = texture(uTexture1, detailUV);\n"
            "        // Blend detail in based on LOD: more detail at close range\n"
            "        float detailBlend = clamp(1.0 - uPrimLOD * 2.0, 0.0, 1.0);\n"
            "        texel0.rgb = mix(texel0.rgb, texel0.rgb * detailTex.rgb * 2.0, detailBlend);\n"
            "    }\n";
    }
    if (features & SF_TEXTURE_SHARPEN) {
        code +=
            "    // Sharpened mipmap — extrapolate past base level\n"
            "    {\n"
            "        // Sharpen factor: extend LOD fraction beyond 1.0 for emphasis\n"
            "        float sharpenFac = clamp(1.0 - uPrimLOD, 0.0, 1.0);\n"
            "        texel0.rgb = mix(texel0.rgb, texel0.rgb * (2.0 - sharpenFac), sharpenFac);\n"
            "    }\n";
    }
    return code;
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
            "    output.vDepth    = output.position.z / output.position.w;\n"
            "    return output;\n"
            "}\n";

        // ── HLSL Fragment ────────────────────────────────────────────────
        std::string fragHeader;
        fragHeader += GetHLSLCompatPreamble();
        fragHeader += GetFragmentInputs();
        fragHeader += GetFragmentOutputs(f);
        fragHeader += GetCommonUniforms();
        fragHeader += GetTextureUniforms();
        if (f & SF_TEXTURE_3POINT)
            fragHeader += GenerateN64TexFilterCode();
        tmpl.fragmentHeader = std::move(fragHeader);

        std::string frag;
        frag += "PSOutput main(PSInput input) {\n";
        frag += "    float4 vColor = input.vColor;\n";
        frag += "    float2 vTexCoord0 = input.vTexCoord0;\n";
        frag += "    float2 vTexCoord1 = input.vTexCoord1;\n";
        frag += "    float vFog = input.vFog;\n";
        frag += "    float vDepth = input.vDepth;\n";
        frag += "    float vClipW = input.vClipW;\n";
        frag += "    float4 fragColor;\n";
        frag += GeneratePrimDepthCode(f);
        frag += GenerateTextureSampleCode(f);
        frag += GenerateYUVConversionCode(f);
        frag += GenerateLODCode(f);
        frag += GenerateDetailSharpenCode(f);
        frag += GenerateNoiseCode(f);
        frag += GenerateCombinerCode(key.mux, f);
        frag += GenerateCoverageCode(f);
        frag += GenerateAlphaTestCode(f);                // N64: alpha compare before blender
        std::string blenderCode0 = GenerateBlenderCode(key.blenderBits, f);
        frag += blenderCode0;
        frag += "    fragColor = combined;\n";
        // Fog fallback: on real N64 fog is applied by the blender.
        // Only add the separate fog pass when the shader blender was skipped.
        if (blenderCode0.empty()) {
            if (!(f & (SF_COPY_MODE | SF_FILL_RECT))) {
                uint32_t aF = (f & SF_2CYCLE) ? ((key.blenderBits >> 24) & 3) : ((key.blenderBits >> 26) & 3);
                if (aF == 1) frag += "    fragColor.a = uFogColor.a;\n";
                else if (aF == 2) {
                    if (f & SF_FOG) frag += "    fragColor.a = vFog;\n";
                    else frag += "    fragColor.a = vColor.a;\n";
                }
                else if (aF == 3) frag += "    fragColor.a = 0.0;\n";
            }
            frag += GenerateFogCode(f);
        }
        frag += GenerateDepthCompareCode(f);
        frag += GenerateDitherCode(f);
        frag += GenerateMonochromeCode(f);
        frag += "    PSOutput psOut;\n";
        frag += "    psOut.fragColor = fragColor;\n";
        if (f & SF_DEPTH_SOURCE_PRIM)
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
        "    vDepth      = gl_Position.z / gl_Position.w;\n"
        "}\n";

    // ── fragment header ──────────────────────────────────────────────────
    tmpl.fragmentHeader =
        GetVersionDirective() +
        GetFragmentInputs() +
        GetFragmentOutputs(f) +
        GetCommonUniforms() +
        GetTextureUniforms();
    if (f & SF_TEXTURE_3POINT)
        tmpl.fragmentHeader += GenerateN64TexFilterCode();

    // ── fragment body ────────────────────────────────────────────────────
    std::string frag;
    frag += "void main() {\n";
    // Declare texel0 default before GenerateDetailSharpenCode which may use it
    if (!(f & SF_TEXTURE_0)) frag += "    vec4 texel0 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    frag += GeneratePrimDepthCode(f);
    frag += GenerateTextureSampleCode(f);
    frag += GenerateYUVConversionCode(f);
    frag += GenerateLODCode(f);
    frag += GenerateDetailSharpenCode(f);
    frag += GenerateNoiseCode(f);
    frag += GenerateCombinerCode(key.mux, f);
    frag += GenerateCoverageCode(f);
    frag += GenerateAlphaTestCode(f);                // N64: alpha compare before blender
    std::string blenderCode1 = GenerateBlenderCode(key.blenderBits, f);
    frag += blenderCode1;
    frag += "    fragColor = combined;\n";
    // Fog fallback: on real N64 fog is applied by the blender.
    // Only add the separate fog pass when the shader blender was skipped.
    if (blenderCode1.empty()) {
        if (!(f & (SF_COPY_MODE | SF_FILL_RECT))) {
            uint32_t aF = (f & SF_2CYCLE) ? ((key.blenderBits >> 24) & 3) : ((key.blenderBits >> 26) & 3);
            if (aF == 1) frag += "    fragColor.a = uFogColor.a;\n";
            else if (aF == 2) {
                if (f & SF_FOG) frag += "    fragColor.a = vFog;\n";
                else frag += "    fragColor.a = vColor.a;\n";
            }
            else if (aF == 3) frag += "    fragColor.a = 0.0;\n";
        }
        frag += GenerateFogCode(f);
    }
    frag += GenerateDepthCompareCode(f);
    frag += GenerateDitherCode(f);
    frag += GenerateMonochromeCode(f);
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
            "    output.vDepth    = output.position.z / output.position.w;\n"
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
        "    vDepth      = gl_Position.z / gl_Position.w;\n"
        "}\n";
    return src;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PUBLIC: FRAGMENT SHADER
// ═══════════════════════════════════════════════════════════════════════════════

std::string ShaderGenerator::GenerateFragmentShader(const CombinerKey& key) const {
    if (m_language == ShaderLanguage::HLSL_SM5) {
        std::string src;
        src += GetHLSLCompatPreamble();
        src += GetFragmentInputs();
        src += GetFragmentOutputs(key.features);
        src += GetCommonUniforms();
        src += GetTextureUniforms();
        if (key.features & SF_TEXTURE_3POINT)
            src += GenerateN64TexFilterCode();
        src += "PSOutput main(PSInput input) {\n";
        src += "    float4 vColor = input.vColor;\n";
        src += "    float2 vTexCoord0 = input.vTexCoord0;\n";
        src += "    float2 vTexCoord1 = input.vTexCoord1;\n";
        src += "    float vFog = input.vFog;\n";
        src += "    float vDepth = input.vDepth;\n";
        src += "    float vClipW = input.vClipW;\n";
        src += "    float4 fragColor;\n";
        src += GeneratePrimDepthCode(key.features);
        src += GenerateTextureSampleCode(key.features);
        src += GenerateYUVConversionCode(key.features);
        src += GenerateLODCode(key.features);
        src += GenerateDetailSharpenCode(key.features);
        src += GenerateNoiseCode(key.features);
        src += GenerateCombinerCode(key.mux, key.features);
        src += GenerateCoverageCode(key.features);
        src += GenerateAlphaTestCode(key.features);
        std::string blenderCode2 = GenerateBlenderCode(key.blenderBits, key.features);
        src += blenderCode2;
        src += "    fragColor = combined;\n";
        // Fog fallback: on real N64 fog is applied by the blender.
        // Only add the separate fog pass when the shader blender was skipped.
        if (blenderCode2.empty()) {
            if (!(key.features & (SF_COPY_MODE | SF_FILL_RECT))) {
                uint32_t aF = (key.features & SF_2CYCLE) ? ((key.blenderBits >> 24) & 3) : ((key.blenderBits >> 26) & 3);
                if (aF == 1) src += "    fragColor.a = uFogColor.a;\n";
                else if (aF == 2) {
                    if (key.features & SF_FOG) src += "    fragColor.a = vFog;\n";
                    else src += "    fragColor.a = vColor.a;\n";
                }
                else if (aF == 3) src += "    fragColor.a = 0.0;\n";
            }
            src += GenerateFogCode(key.features);
        }
        src += GenerateDepthCompareCode(key.features);
        src += GenerateDitherCode(key.features);
        src += GenerateMonochromeCode(key.features);
        src += "    PSOutput psOut;\n";
        src += "    psOut.fragColor = fragColor;\n";
        if (key.features & SF_DEPTH_SOURCE_PRIM)
            src += "    psOut.fragDepth = uPrimDepth;\n";
        src += "    return psOut;\n";
        src += "}\n";
        return src;
    }
    std::string src;
    src += GetVersionDirective();
    src += GetFragmentInputs();
    src += GetFragmentOutputs(key.features);
    src += GetCommonUniforms();
    src += GetTextureUniforms();
    if (key.features & SF_TEXTURE_3POINT)
        src += GenerateN64TexFilterCode();
    src += "void main() {\n";
    // Declare texel0 default before GenerateDetailSharpenCode which may use it
    if (!(key.features & SF_TEXTURE_0)) src += "    vec4 texel0 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    src += GeneratePrimDepthCode(key.features);
    src += GenerateTextureSampleCode(key.features);
    src += GenerateYUVConversionCode(key.features);
    src += GenerateLODCode(key.features);
    src += GenerateDetailSharpenCode(key.features);
    src += GenerateNoiseCode(key.features);
    src += GenerateCombinerCode(key.mux, key.features);
    src += GenerateCoverageCode(key.features);
    src += GenerateAlphaTestCode(key.features);
    std::string blenderCode3 = GenerateBlenderCode(key.blenderBits, key.features);
    src += blenderCode3;
    src += "    fragColor = combined;\n";
    // Fog fallback: on real N64 fog is applied by the blender.
    // Only add the separate fog pass when the shader blender was skipped.
    if (blenderCode3.empty()) {
        if (!(key.features & (SF_COPY_MODE | SF_FILL_RECT))) {
            uint32_t aF = (key.features & SF_2CYCLE) ? ((key.blenderBits >> 24) & 3) : ((key.blenderBits >> 26) & 3);
            if (aF == 1) src += "    fragColor.a = uFogColor.a;\n";
            else if (aF == 2) {
                if (key.features & SF_FOG) src += "    fragColor.a = vFog;\n";
                else src += "    fragColor.a = vColor.a;\n";
            }
            else if (aF == 3) src += "    fragColor.a = 0.0;\n";
        }
        src += GenerateFogCode(key.features);
    }
    src += GenerateDepthCompareCode(key.features);
    src += GenerateDitherCode(key.features);
    src += GenerateMonochromeCode(key.features);
    src += "}\n";
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


