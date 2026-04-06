/**
 * @file PostProcessing.cpp
 * @brief Post-Processing Pipeline.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Post-Processing Pipeline Implementation
// ══════════════════════════════════════════════════════════════════════════════

#include "PostProcessing.hpp"
#include "../Common/Logger.hpp"

#include <glad/gl.h>
#include <cstring>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════
//  Full-screen quad helper
// ═══════════════════════════════════════════════════════════════

static const float kQuadVerts[] = {
    // pos (x,y)   texcoord (u,v)
    -1, -1,  0, 0,
     1, -1,  1, 0,
    -1,  1,  0, 1,
     1,  1,  1, 1,
};

static void CreateFullscreenQuad(uint32_t& vao, uint32_t& vbo) {
    glCreateVertexArrays(1, &vao);
    glCreateBuffers(1, &vbo);
    glNamedBufferStorage(vbo, sizeof(kQuadVerts), kQuadVerts, 0);
    // pos
    glVertexArrayAttribFormat(vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 0);
    // uv
    glVertexArrayAttribFormat(vao, 1, 2, GL_FLOAT, GL_FALSE, 8);
    glVertexArrayAttribBinding(vao, 1, 0);
    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 16);
}

static void DestroyFullscreenQuad(uint32_t& vao, uint32_t& vbo) {
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
}

static void DrawFullscreenQuad(uint32_t vao) {
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static uint32_t CompileShaderProgram(const char* vertSrc, const char* fragSrc) {
    auto compile = [](GLenum type, const char* src) -> uint32_t {
        uint32_t s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        int ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            LOG_ERROR("Shader compile error: {}", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    uint32_t vs = compile(GL_VERTEX_SHADER, vertSrc);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) return 0;

    uint32_t prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    int ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_ERROR("Shader link error: {}", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ═══════════════════════════════════════════════════════════════
//  Vertex shader (shared)
// ═══════════════════════════════════════════════════════════════

static const char* kPostVS = R"(
#version 460 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// ═══════════════════════════════════════════════════════════════
//  Gamma Correction
// ═══════════════════════════════════════════════════════════════

static const char* kGammaFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform float uGamma;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;

void main() {
    vec3 color = texture(uInput, vUV).rgb;
    
    // Brightness
    color *= uBrightness;
    
    // Contrast
    color = (color - 0.5) * uContrast + 0.5;
    
    // Saturation
    float grey = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(grey), color, uSaturation);
    
    // Gamma
    color = pow(max(color, 0.0), vec3(1.0 / uGamma));
    
    fragColor = vec4(color, 1.0);
}
)";

bool GammaEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kGammaFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void GammaEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void GammaEffect::Resize(int, int) { /* No resources to resize */ }

void GammaEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform1f(glGetUniformLocation(m_shader, "uGamma"), params.gamma);
    glUniform1f(glGetUniformLocation(m_shader, "uBrightness"), params.brightness);
    glUniform1f(glGetUniformLocation(m_shader, "uContrast"), params.contrast);
    glUniform1f(glGetUniformLocation(m_shader, "uSaturation"), params.saturation);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  FXAA 
// ═══════════════════════════════════════════════════════════════

static const char* kFXAAFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uInvResolution;
uniform float uSubpixQuality;
uniform float uEdgeThreshold;
uniform float uEdgeThresholdMin;

float FXAALuma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 uv = vUV;
    vec3 rgbM  = texture(uInput, uv).rgb;
    float lumaM = FXAALuma(rgbM);
    
    float lumaN  = FXAALuma(textureOffset(uInput, uv, ivec2(0, 1)).rgb);
    float lumaS  = FXAALuma(textureOffset(uInput, uv, ivec2(0,-1)).rgb);
    float lumaE  = FXAALuma(textureOffset(uInput, uv, ivec2(1, 0)).rgb);
    float lumaW  = FXAALuma(textureOffset(uInput, uv, ivec2(-1,0)).rgb);
    
    float rangeMax = max(max(lumaN, lumaS), max(lumaE, lumaW));
    float rangeMin = min(min(lumaN, lumaS), min(lumaE, lumaW));
    float range = rangeMax - rangeMin;
    
    if (range < max(uEdgeThresholdMin, rangeMax * uEdgeThreshold)) {
        fragColor = vec4(rgbM, 1.0);
        return;
    }
    
    float lumaNW = FXAALuma(textureOffset(uInput, uv, ivec2(-1, 1)).rgb);
    float lumaNE = FXAALuma(textureOffset(uInput, uv, ivec2( 1, 1)).rgb);
    float lumaSW = FXAALuma(textureOffset(uInput, uv, ivec2(-1,-1)).rgb);
    float lumaSE = FXAALuma(textureOffset(uInput, uv, ivec2( 1,-1)).rgb);
    
    float lumaAvg = (lumaN + lumaS + lumaE + lumaW) * 0.25;
    float subpix = abs(lumaAvg - lumaM) / range;
    subpix = clamp(subpix / uSubpixQuality, 0.0, 1.0);
    subpix = smoothstep(0.0, 1.0, subpix) * subpix;
    
    float edgeH = abs(lumaN + lumaS - 2.0 * lumaM);
    float edgeV = abs(lumaE + lumaW - 2.0 * lumaM);
    bool isHorizontal = edgeH >= edgeV;
    
    float stepLen = isHorizontal ? uInvResolution.y : uInvResolution.x;
    float luma1 = isHorizontal ? lumaN : lumaE;
    float luma2 = isHorizontal ? lumaS : lumaW;
    float grad1 = luma1 - lumaM;
    float grad2 = luma2 - lumaM;
    
    bool is1Steepest = abs(grad1) >= abs(grad2);
    float gradScaled = max(abs(grad1), abs(grad2)) * 0.25;
    
    float lumaLocalAvg;
    if (is1Steepest) {
        stepLen = -stepLen;
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }
    
    vec2 currentUV = uv;
    if (isHorizontal) currentUV.y += stepLen * 0.5;
    else              currentUV.x += stepLen * 0.5;
    
    // Edge search
    vec2 offset = isHorizontal ? vec2(uInvResolution.x, 0) : vec2(0, uInvResolution.y);
    vec2 uv1 = currentUV - offset;
    vec2 uv2 = currentUV + offset;
    
    float lumaEnd1 = FXAALuma(texture(uInput, uv1).rgb) - lumaLocalAvg;
    float lumaEnd2 = FXAALuma(texture(uInput, uv2).rgb) - lumaLocalAvg;
    
    bool done1 = abs(lumaEnd1) >= gradScaled;
    bool done2 = abs(lumaEnd2) >= gradScaled;
    
    for (int i = 0; i < 12 && !(done1 && done2); i++) {
        if (!done1) { uv1 -= offset; lumaEnd1 = FXAALuma(texture(uInput, uv1).rgb) - lumaLocalAvg; done1 = abs(lumaEnd1) >= gradScaled; }
        if (!done2) { uv2 += offset; lumaEnd2 = FXAALuma(texture(uInput, uv2).rgb) - lumaLocalAvg; done2 = abs(lumaEnd2) >= gradScaled; }
    }
    
    float dist1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    float distFinal = min(dist1, dist2);
    float edgeLen = dist1 + dist2;
    float pixelOff = -distFinal / edgeLen + 0.5;
    
    bool isLumaCenterSmaller = lumaM < lumaLocalAvg;
    bool correctVar = ((dist1 < dist2) ? lumaEnd1 : lumaEnd2) < 0.0 != isLumaCenterSmaller;
    float finalOff = correctVar ? pixelOff : 0.0;
    finalOff = max(finalOff, subpix);
    
    vec2 finalUV = uv;
    if (isHorizontal) finalUV.y += finalOff * stepLen;
    else              finalUV.x += finalOff * stepLen;
    
    fragColor = vec4(texture(uInput, finalUV).rgb, 1.0);
}
)";

bool FXAAEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kFXAAFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void FXAAEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void FXAAEffect::Resize(int, int) {}

void FXAAEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform2f(glGetUniformLocation(m_shader, "uInvResolution"),
                1.0f / width, 1.0f / height);
    glUniform1f(glGetUniformLocation(m_shader, "uSubpixQuality"), params.qualitySubpix);
    glUniform1f(glGetUniformLocation(m_shader, "uEdgeThreshold"), params.edgeThreshold);
    glUniform1f(glGetUniformLocation(m_shader, "uEdgeThresholdMin"), params.edgeThresholdMin);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Bloom (multi-pass downsample/upsample)
// ═══════════════════════════════════════════════════════════════

static const char* kBloomThresholdFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform float uThreshold;
uniform float uSoftKnee;
void main() {
    vec3 color = texture(uInput, vUV).rgb;
    float br = max(color.r, max(color.g, color.b));
    float knee = uThreshold * uSoftKnee;
    float soft = br - uThreshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contrib = max(soft, br - uThreshold) / max(br, 1e-4);
    fragColor = vec4(color * max(contrib, 0.0), 1.0);
}
)";

static const char* kBloomDownFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uTexelSize;
void main() {
    // 13-tap downsampling filter for better quality
    vec3 a = texture(uInput, vUV + uTexelSize * vec2(-2, 2)).rgb;
    vec3 b = texture(uInput, vUV + uTexelSize * vec2( 0, 2)).rgb;
    vec3 c = texture(uInput, vUV + uTexelSize * vec2( 2, 2)).rgb;
    vec3 d = texture(uInput, vUV + uTexelSize * vec2(-2, 0)).rgb;
    vec3 e = texture(uInput, vUV).rgb;
    vec3 f = texture(uInput, vUV + uTexelSize * vec2( 2, 0)).rgb;
    vec3 g = texture(uInput, vUV + uTexelSize * vec2(-2,-2)).rgb;
    vec3 h = texture(uInput, vUV + uTexelSize * vec2( 0,-2)).rgb;
    vec3 i = texture(uInput, vUV + uTexelSize * vec2( 2,-2)).rgb;
    vec3 j = texture(uInput, vUV + uTexelSize * vec2(-1, 1)).rgb;
    vec3 k = texture(uInput, vUV + uTexelSize * vec2( 1, 1)).rgb;
    vec3 l = texture(uInput, vUV + uTexelSize * vec2(-1,-1)).rgb;
    vec3 m = texture(uInput, vUV + uTexelSize * vec2( 1,-1)).rgb;
    
    vec3 color = e * 0.125;
    color += (a+c+g+i) * 0.03125;
    color += (b+d+f+h) * 0.0625;
    color += (j+k+l+m) * 0.125;
    fragColor = vec4(color, 1.0);
}
)";

static const char* kBloomUpFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uTexelSize;
uniform float uRadius;
void main() {
    vec3 a = texture(uInput, vUV + uTexelSize * vec2(-1, 1) * uRadius).rgb;
    vec3 b = texture(uInput, vUV + uTexelSize * vec2( 0, 1) * uRadius).rgb;
    vec3 c = texture(uInput, vUV + uTexelSize * vec2( 1, 1) * uRadius).rgb;
    vec3 d = texture(uInput, vUV + uTexelSize * vec2(-1, 0) * uRadius).rgb;
    vec3 e = texture(uInput, vUV).rgb;
    vec3 f = texture(uInput, vUV + uTexelSize * vec2( 1, 0) * uRadius).rgb;
    vec3 g = texture(uInput, vUV + uTexelSize * vec2(-1,-1) * uRadius).rgb;
    vec3 h = texture(uInput, vUV + uTexelSize * vec2( 0,-1) * uRadius).rgb;
    vec3 i = texture(uInput, vUV + uTexelSize * vec2( 1,-1) * uRadius).rgb;
    
    vec3 color = e * 4.0 + (b+d+f+h) * 2.0 + (a+c+g+i);
    fragColor = vec4(color / 16.0, 1.0);
}
)";

static const char* kBloomCompositeFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uIntensity;
void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    vec3 bloom = texture(uBloom, vUV).rgb;
    fragColor = vec4(scene + bloom * uIntensity, 1.0);
}
)";

bool BloomEffect::Init(int width, int height) {
    m_thresholdShader  = CompileShaderProgram(kPostVS, kBloomThresholdFS);
    m_downsampleShader = CompileShaderProgram(kPostVS, kBloomDownFS);
    m_upsampleShader   = CompileShaderProgram(kPostVS, kBloomUpFS);
    m_compositeShader  = CompileShaderProgram(kPostVS, kBloomCompositeFS);
    CreateFullscreenQuad(m_vao, m_vbo);

    if (!m_thresholdShader || !m_downsampleShader || !m_upsampleShader || !m_compositeShader)
        return false;

    Resize(width, height);
    return true;
}

void BloomEffect::Shutdown() {
    for (auto& mip : m_mips) {
        if (mip.fbo) glDeleteFramebuffers(1, &mip.fbo);
        if (mip.tex) glDeleteTextures(1, &mip.tex);
    }
    m_mips.clear();
    if (m_thresholdShader)  { glDeleteProgram(m_thresholdShader); m_thresholdShader = 0; }
    if (m_downsampleShader) { glDeleteProgram(m_downsampleShader); m_downsampleShader = 0; }
    if (m_upsampleShader)   { glDeleteProgram(m_upsampleShader); m_upsampleShader = 0; }
    if (m_compositeShader)  { glDeleteProgram(m_compositeShader); m_compositeShader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void BloomEffect::Resize(int width, int height) {
    for (auto& mip : m_mips) {
        if (mip.fbo) glDeleteFramebuffers(1, &mip.fbo);
        if (mip.tex) glDeleteTextures(1, &mip.tex);
    }
    m_mips.clear();

    int w = width / 2;
    int h = height / 2;
    for (int i = 0; i < params.mipLevels && w >= 1 && h >= 1; ++i) {
        MipLevel mip;
        mip.width = w;
        mip.height = h;
        glCreateTextures(GL_TEXTURE_2D, 1, &mip.tex);
        glTextureStorage2D(mip.tex, 1, GL_R11F_G11F_B10F, w, h);
        glTextureParameteri(mip.tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(mip.tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(mip.tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(mip.tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glCreateFramebuffers(1, &mip.fbo);
        glNamedFramebufferTexture(mip.fbo, GL_COLOR_ATTACHMENT0, mip.tex, 0);

        m_mips.push_back(mip);
        w /= 2;
        h /= 2;
    }
}

void BloomEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    if (m_mips.empty()) return;

    // Save GL state we're about to modify
    GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLint prevBlendSrc, prevBlendDst;
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDst);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // 1. Threshold pass → mip[0]
    glBindFramebuffer(GL_FRAMEBUFFER, m_mips[0].fbo);
    glViewport(0, 0, m_mips[0].width, m_mips[0].height);
    glUseProgram(m_thresholdShader);
    glUniform1f(glGetUniformLocation(m_thresholdShader, "uThreshold"), params.threshold);
    glUniform1f(glGetUniformLocation(m_thresholdShader, "uSoftKnee"), params.softKnee);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);

    // 2. Downsample chain
    glUseProgram(m_downsampleShader);
    for (size_t i = 1; i < m_mips.size(); ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_mips[i].fbo);
        glViewport(0, 0, m_mips[i].width, m_mips[i].height);
        glUniform2f(glGetUniformLocation(m_downsampleShader, "uTexelSize"),
                    1.0f / m_mips[i-1].width, 1.0f / m_mips[i-1].height);
        glBindTextureUnit(0, m_mips[i-1].tex);
        DrawFullscreenQuad(m_vao);
    }

    // 3. Upsample chain (additive blend)
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(m_upsampleShader);
    glUniform1f(glGetUniformLocation(m_upsampleShader, "uRadius"), params.radius);
    for (int i = static_cast<int>(m_mips.size()) - 2; i >= 0; --i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_mips[i].fbo);
        glViewport(0, 0, m_mips[i].width, m_mips[i].height);
        glUniform2f(glGetUniformLocation(m_upsampleShader, "uTexelSize"),
                    1.0f / m_mips[i+1].width, 1.0f / m_mips[i+1].height);
        glBindTextureUnit(0, m_mips[i+1].tex);
        DrawFullscreenQuad(m_vao);
    }
    glDisable(GL_BLEND);

    // 4. Composite: scene + bloom → output
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_compositeShader);
    glUniform1f(glGetUniformLocation(m_compositeShader, "uIntensity"), params.intensity);
    glBindTextureUnit(0, inputTex);
    glBindTextureUnit(1, m_mips[0].tex);
    DrawFullscreenQuad(m_vao);

    // Restore previous GL state
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(prevBlendSrc, prevBlendDst);
}

// ═══════════════════════════════════════════════════════════════
//  CRT Filter
// ═══════════════════════════════════════════════════════════════

static const char* kCRTFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uResolution;
uniform float uCurvature;
uniform float uScanlineWeight;
uniform float uMaskDotPitch;
uniform float uVignetteStrength;

vec2 Distort(vec2 uv) {
    uv = uv * 2.0 - 1.0;
    float d = dot(uv, uv);
    uv *= 1.0 + d * uCurvature;
    return uv * 0.5 + 0.5;
}

void main() {
    vec2 uv = Distort(vUV);
    
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0,0,0,1);
        return;
    }
    
    vec3 color = texture(uInput, uv).rgb;
    
    // Scanlines
    float scanline = sin(uv.y * uResolution.y * 3.14159) * 0.5 + 0.5;
    scanline = pow(scanline, uScanlineWeight * 10.0);
    color *= mix(1.0, scanline, uScanlineWeight);
    
    // Shadow mask (simplified RGB stripe)
    float maskPos = mod(gl_FragCoord.x, 3.0);
    vec3 mask = vec3(1.0);
    if (maskPos < 1.0) mask = vec3(1.0, uMaskDotPitch, uMaskDotPitch);
    else if (maskPos < 2.0) mask = vec3(uMaskDotPitch, 1.0, uMaskDotPitch);
    else mask = vec3(uMaskDotPitch, uMaskDotPitch, 1.0);
    color *= mask;
    
    // Vignette
    vec2 vigUV = vUV * (1.0 - vUV);
    float vig = vigUV.x * vigUV.y * 15.0;
    vig = pow(vig, uVignetteStrength);
    color *= vig;
    
    fragColor = vec4(color, 1.0);
}
)";

bool CRTEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kCRTFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void CRTEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void CRTEffect::Resize(int, int) {}

void CRTEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform2f(glGetUniformLocation(m_shader, "uResolution"),
                static_cast<float>(width), static_cast<float>(height));
    glUniform1f(glGetUniformLocation(m_shader, "uCurvature"), params.curvature);
    glUniform1f(glGetUniformLocation(m_shader, "uScanlineWeight"), params.scanlineWeight);
    glUniform1f(glGetUniformLocation(m_shader, "uMaskDotPitch"), params.maskDotPitch);
    glUniform1f(glGetUniformLocation(m_shader, "uVignetteStrength"), params.vignetteStrength);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Sharpen (CAS-style)
// ═══════════════════════════════════════════════════════════════

static const char* kSharpenFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uInvResolution;
uniform float uStrength;
uniform float uClamp;

void main() {
    vec3 c = texture(uInput, vUV).rgb;
    vec3 n = textureOffset(uInput, vUV, ivec2(0, 1)).rgb;
    vec3 s = textureOffset(uInput, vUV, ivec2(0,-1)).rgb;
    vec3 e = textureOffset(uInput, vUV, ivec2(1, 0)).rgb;
    vec3 w = textureOffset(uInput, vUV, ivec2(-1,0)).rgb;
    
    vec3 minN = min(min(n, s), min(e, w));
    vec3 maxN = max(max(n, s), max(e, w));
    
    vec3 d = (c - (n + s + e + w) * 0.25) * uStrength;
    d = clamp(d, -vec3(uClamp), vec3(uClamp));
    
    fragColor = vec4(clamp(c + d, minN, maxN), 1.0);
}
)";

bool SharpenEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kSharpenFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void SharpenEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void SharpenEffect::Resize(int, int) {}

void SharpenEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform2f(glGetUniformLocation(m_shader, "uInvResolution"),
                1.0f / width, 1.0f / height);
    glUniform1f(glGetUniformLocation(m_shader, "uStrength"), params.strength);
    glUniform1f(glGetUniformLocation(m_shader, "uClamp"), params.clamp);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Chromatic Aberration
// ═══════════════════════════════════════════════════════════════

static const char* kChromaFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform float uIntensity;
uniform float uOffsetR;
uniform float uOffsetB;

void main() {
    vec2 dir = vUV - 0.5;
    float dist = length(dir);
    vec2 offset = dir * dist * uIntensity;

    float r = texture(uInput, vUV + offset * uOffsetR).r;
    float g = texture(uInput, vUV).g;
    float b = texture(uInput, vUV + offset * uOffsetB).b;

    fragColor = vec4(r, g, b, 1.0);
}
)";

bool ChromaticAberrationEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kChromaFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void ChromaticAberrationEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void ChromaticAberrationEffect::Resize(int, int) {}

void ChromaticAberrationEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform1f(glGetUniformLocation(m_shader, "uIntensity"), params.intensity);
    glUniform1f(glGetUniformLocation(m_shader, "uOffsetR"), params.offsetR);
    glUniform1f(glGetUniformLocation(m_shader, "uOffsetB"), params.offsetB);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Film Grain
// ═══════════════════════════════════════════════════════════════

static const char* kFilmGrainFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform float uIntensity;
uniform float uResponse;
uniform float uTime;

float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 color = texture(uInput, vUV).rgb;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Grain response: less grain on bright areas
    float response = mix(1.0, 1.0 - luma, uResponse);

    float grain = hash(vUV * 1000.0 + uTime) * 2.0 - 1.0;
    color += grain * uIntensity * response;

    fragColor = vec4(max(color, 0.0), 1.0);
}
)";

bool FilmGrainEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kFilmGrainFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void FilmGrainEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void FilmGrainEffect::Resize(int, int) {}

void FilmGrainEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform1f(glGetUniformLocation(m_shader, "uIntensity"), params.intensity);
    glUniform1f(glGetUniformLocation(m_shader, "uResponse"), params.response);
    glUniform1f(glGetUniformLocation(m_shader, "uTime"), static_cast<float>(m_frameCount) * 0.01f);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
    m_frameCount++;
}

// ═══════════════════════════════════════════════════════════════
//  Dithering (Bayer 8x8 ordered dither)
// ═══════════════════════════════════════════════════════════════

static const char* kDitheringFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uResolution;

// 8x8 Bayer matrix normalized to [0,1]
float bayer8(ivec2 p) {
    const int b[64] = int[64](
         0, 32,  8, 40,  2, 34, 10, 42,
        48, 16, 56, 24, 50, 18, 58, 26,
        12, 44,  4, 36, 14, 46,  6, 38,
        60, 28, 52, 20, 62, 30, 54, 22,
         3, 35, 11, 43,  1, 33,  9, 41,
        51, 19, 59, 27, 49, 17, 57, 25,
        15, 47,  7, 39, 13, 45,  5, 37,
        63, 31, 55, 23, 61, 29, 53, 21
    );
    return float(b[(p.y & 7) * 8 + (p.x & 7)]) / 64.0;
}

void main() {
    vec3 color = texture(uInput, vUV).rgb;
    ivec2 px = ivec2(gl_FragCoord.xy);
    float threshold = bayer8(px);
    // Quantize to N64-like 5-bit per channel (32 levels)
    vec3 quantized = floor(color * 31.0 + threshold) / 31.0;
    fragColor = vec4(quantized, 1.0);
}
)";

bool DitheringEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kDitheringFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void DitheringEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void DitheringEffect::Resize(int, int) {}

void DitheringEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform2f(glGetUniformLocation(m_shader, "uResolution"),
                static_cast<float>(width), static_cast<float>(height));
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Color Grading
// ═══════════════════════════════════════════════════════════════

static const char* kColorGradingFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform float uTemperature;
uniform float uTint;
uniform float uHueShift;
uniform float uShadows;
uniform float uHighlights;
uniform vec3  uLift;
uniform vec3  uGain;

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec3 color = texture(uInput, vUV).rgb;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Temperature: shift blue-orange axis
    color.r += uTemperature * 0.1;
    color.b -= uTemperature * 0.1;
    // Tint: shift green-magenta axis
    color.g += uTint * 0.1;

    // Hue shift
    if (abs(uHueShift) > 0.001) {
        vec3 hsv = rgb2hsv(color);
        hsv.x = fract(hsv.x + uHueShift);
        color = hsv2rgb(hsv);
    }

    // Shadows / Highlights
    float shadowMask = 1.0 - smoothstep(0.0, 0.5, luma);
    float highlightMask = smoothstep(0.5, 1.0, luma);
    color += uShadows * shadowMask * 0.1;
    color += uHighlights * highlightMask * 0.1;

    // Lift / Gain
    color = (color + uLift) * uGain;

    fragColor = vec4(max(color, 0.0), 1.0);
}
)";

bool ColorGradingEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kColorGradingFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void ColorGradingEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void ColorGradingEffect::Resize(int, int) {}

void ColorGradingEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform1f(glGetUniformLocation(m_shader, "uTemperature"), params.temperature);
    glUniform1f(glGetUniformLocation(m_shader, "uTint"), params.tint);
    glUniform1f(glGetUniformLocation(m_shader, "uHueShift"), params.hueShift);
    glUniform1f(glGetUniformLocation(m_shader, "uShadows"), params.shadows);
    glUniform1f(glGetUniformLocation(m_shader, "uHighlights"), params.highlights);
    glUniform3f(glGetUniformLocation(m_shader, "uLift"),
                params.liftR, params.liftG, params.liftB);
    glUniform3f(glGetUniformLocation(m_shader, "uGain"),
                params.gainR, params.gainG, params.gainB);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Depth of Field (disk blur weighted by depth)
// ═══════════════════════════════════════════════════════════════

static const char* kDofFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform sampler2D uDepth;
uniform vec2 uInvResolution;
uniform float uFocalDist;
uniform float uFocalRange;
uniform float uBlurStrength;

void main() {
    float depth = texture(uDepth, vUV).r;
    float coc = abs(depth - uFocalDist) / uFocalRange;
    coc = clamp(coc, 0.0, 1.0) * uBlurStrength;

    if (coc < 0.5) {
        fragColor = vec4(texture(uInput, vUV).rgb, 1.0);
        return;
    }

    // Poisson disk 12-tap blur
    const vec2 poisson[12] = vec2[12](
        vec2(-0.326,-0.406), vec2(-0.840,-0.074), vec2(-0.696, 0.457),
        vec2(-0.203, 0.621), vec2( 0.962,-0.195), vec2( 0.473,-0.480),
        vec2( 0.519, 0.767), vec2( 0.185,-0.893), vec2( 0.507, 0.064),
        vec2( 0.896, 0.412), vec2(-0.322,-0.933), vec2(-0.792,-0.598)
    );

    vec3 result = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        vec2 offset = poisson[i] * coc * uInvResolution;
        result += texture(uInput, vUV + offset).rgb;
    }
    fragColor = vec4(result / 12.0, 1.0);
}
)";

bool DepthOfFieldEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kDofFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void DepthOfFieldEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void DepthOfFieldEffect::Resize(int, int) {}

void DepthOfFieldEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform2f(glGetUniformLocation(m_shader, "uInvResolution"),
                1.0f / width, 1.0f / height);
    glUniform1f(glGetUniformLocation(m_shader, "uFocalDist"), params.focalDistance);
    glUniform1f(glGetUniformLocation(m_shader, "uFocalRange"), params.focalRange);
    glUniform1f(glGetUniformLocation(m_shader, "uBlurStrength"), params.blurStrength);
    glBindTextureUnit(0, inputTex);
    if (m_depthTex) glBindTextureUnit(1, m_depthTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  Motion Blur (frame accumulation)
// ═══════════════════════════════════════════════════════════════

static const char* kMotionBlurFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uCurrent;
uniform sampler2D uPrevious;
uniform float uStrength;
uniform int uSamples;

void main() {
    vec3 current = texture(uCurrent, vUV).rgb;
    vec3 previous = texture(uPrevious, vUV).rgb;

    // Blend current with previous frame based on strength
    vec3 color = mix(current, previous, uStrength * 0.5);

    fragColor = vec4(color, 1.0);
}
)";

bool MotionBlurEffect::Init(int width, int height) {
    m_shader = CompileShaderProgram(kPostVS, kMotionBlurFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    m_width = width;
    m_height = height;

    // Create previous frame texture
    glCreateTextures(GL_TEXTURE_2D, 1, &m_prevTex);
    glTextureStorage2D(m_prevTex, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_prevTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_prevTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    return m_shader != 0;
}

void MotionBlurEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    if (m_prevTex) { glDeleteTextures(1, &m_prevTex); m_prevTex = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void MotionBlurEffect::Resize(int width, int height) {
    if (m_prevTex) { glDeleteTextures(1, &m_prevTex); m_prevTex = 0; }
    m_width = width;
    m_height = height;
    glCreateTextures(GL_TEXTURE_2D, 1, &m_prevTex);
    glTextureStorage2D(m_prevTex, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_prevTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_prevTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

void MotionBlurEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform1f(glGetUniformLocation(m_shader, "uStrength"), params.strength);
    glUniform1i(glGetUniformLocation(m_shader, "uSamples"), params.samples);
    glBindTextureUnit(0, inputTex);
    glBindTextureUnit(1, m_prevTex);
    DrawFullscreenQuad(m_vao);

    // Copy current to previous for next frame
    glCopyImageSubData(inputTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_prevTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       width, height, 1);
}

// ═══════════════════════════════════════════════════════════════
//  Vignette
// ═══════════════════════════════════════════════════════════════

static const char* kVignetteFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform float uIntensity;
uniform float uRoundness;
uniform float uSmoothness;

void main() {
    vec3 color = texture(uInput, vUV).rgb;
    vec2 uv = vUV * (1.0 - vUV);
    float vig = uv.x * uv.y * (16.0 / uRoundness);
    vig = pow(vig, uSmoothness * uIntensity);
    color *= clamp(vig, 0.0, 1.0);
    fragColor = vec4(color, 1.0);
}
)";

bool VignetteEffect::Init(int, int) {
    m_shader = CompileShaderProgram(kPostVS, kVignetteFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    return m_shader != 0;
}

void VignetteEffect::Shutdown() {
    if (m_shader) { glDeleteProgram(m_shader); m_shader = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void VignetteEffect::Resize(int, int) {}

void VignetteEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_shader);
    glUniform1f(glGetUniformLocation(m_shader, "uIntensity"), params.intensity);
    glUniform1f(glGetUniformLocation(m_shader, "uRoundness"), params.roundness);
    glUniform1f(glGetUniformLocation(m_shader, "uSmoothness"), params.smoothness);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  SMAA (edge-detect + neighborhood blend)
// ═══════════════════════════════════════════════════════════════

static const char* kSMAAEdgeFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uInvResolution;

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    float threshold = 0.1;
    float L      = Luma(texture(uInput, vUV).rgb);
    float Lleft  = Luma(texture(uInput, vUV - vec2(uInvResolution.x, 0)).rgb);
    float Ltop   = Luma(texture(uInput, vUV - vec2(0, uInvResolution.y)).rgb);
    float Lright = Luma(texture(uInput, vUV + vec2(uInvResolution.x, 0)).rgb);
    float Lbottom= Luma(texture(uInput, vUV + vec2(0, uInvResolution.y)).rgb);

    vec4 delta = abs(vec4(L) - vec4(Lleft, Ltop, Lright, Lbottom));
    vec2 edges = step(vec2(threshold), delta.xy);

    if (dot(edges, vec2(1.0)) == 0.0) discard;
    fragColor = vec4(edges, 0.0, 1.0);
}
)";

static const char* kSMAABlendFS = R"(
#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform sampler2D uEdges;
uniform vec2 uInvResolution;

void main() {
    vec4 edges = texture(uEdges, vUV);
    if (edges.r + edges.g < 0.01) {
        fragColor = vec4(texture(uInput, vUV).rgb, 1.0);
        return;
    }

    // Blend with neighbors along edge directions
    vec3 color = texture(uInput, vUV).rgb;
    float weight = 0.0;
    vec3 sum = vec3(0.0);

    if (edges.r > 0.5) {
        sum += texture(uInput, vUV - vec2(uInvResolution.x, 0)).rgb;
        sum += texture(uInput, vUV + vec2(uInvResolution.x, 0)).rgb;
        weight += 2.0;
    }
    if (edges.g > 0.5) {
        sum += texture(uInput, vUV - vec2(0, uInvResolution.y)).rgb;
        sum += texture(uInput, vUV + vec2(0, uInvResolution.y)).rgb;
        weight += 2.0;
    }

    if (weight > 0.0) {
        color = mix(color, sum / weight, 0.5);
    }

    fragColor = vec4(color, 1.0);
}
)";

bool SMAAEffect::Init(int width, int height) {
    m_edgeShader  = CompileShaderProgram(kPostVS, kSMAAEdgeFS);
    m_blendShader = CompileShaderProgram(kPostVS, kSMAABlendFS);
    CreateFullscreenQuad(m_vao, m_vbo);
    m_width = width;
    m_height = height;

    if (!m_edgeShader || !m_blendShader) return false;

    Resize(width, height);
    return true;
}

void SMAAEffect::Shutdown() {
    if (m_edgeShader)  { glDeleteProgram(m_edgeShader); m_edgeShader = 0; }
    if (m_blendShader) { glDeleteProgram(m_blendShader); m_blendShader = 0; }
    if (m_edgeTex) { glDeleteTextures(1, &m_edgeTex); m_edgeTex = 0; }
    if (m_edgeFBO) { glDeleteFramebuffers(1, &m_edgeFBO); m_edgeFBO = 0; }
    DestroyFullscreenQuad(m_vao, m_vbo);
}

void SMAAEffect::Resize(int width, int height) {
    if (m_edgeTex) { glDeleteTextures(1, &m_edgeTex); m_edgeTex = 0; }
    if (m_edgeFBO) { glDeleteFramebuffers(1, &m_edgeFBO); m_edgeFBO = 0; }
    m_width = width;
    m_height = height;

    glCreateTextures(GL_TEXTURE_2D, 1, &m_edgeTex);
    glTextureStorage2D(m_edgeTex, 1, GL_RGBA8, width, height);
    glTextureParameteri(m_edgeTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_edgeTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_edgeTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_edgeTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &m_edgeFBO);
    glNamedFramebufferTexture(m_edgeFBO, GL_COLOR_ATTACHMENT0, m_edgeTex, 0);
}

void SMAAEffect::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    // Pass 1: Edge detection → m_edgeFBO
    glBindFramebuffer(GL_FRAMEBUFFER, m_edgeFBO);
    glViewport(0, 0, width, height);
    float clearColor[4] = { 0, 0, 0, 0 };
    glClearNamedFramebufferfv(m_edgeFBO, GL_COLOR, 0, clearColor);
    glUseProgram(m_edgeShader);
    glUniform2f(glGetUniformLocation(m_edgeShader, "uInvResolution"),
                1.0f / width, 1.0f / height);
    glBindTextureUnit(0, inputTex);
    DrawFullscreenQuad(m_vao);

    // Pass 2: Neighborhood blend → output
    glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
    glViewport(0, 0, width, height);
    glUseProgram(m_blendShader);
    glUniform2f(glGetUniformLocation(m_blendShader, "uInvResolution"),
                1.0f / width, 1.0f / height);
    glBindTextureUnit(0, inputTex);
    glBindTextureUnit(1, m_edgeTex);
    DrawFullscreenQuad(m_vao);
}

// ═══════════════════════════════════════════════════════════════
//  PostProcessingPipeline
// ═══════════════════════════════════════════════════════════════

PostProcessingPipeline::PostProcessingPipeline() = default;
PostProcessingPipeline::~PostProcessingPipeline() { Shutdown(); }

bool PostProcessingPipeline::Init(int width, int height) {
    m_width = width;
    m_height = height;
    if (!CreateIntermediateBuffers(width, height)) return false;
    m_initialized = true;
    return true;
}

void PostProcessingPipeline::Shutdown() {
    for (auto& e : m_effects) e->Shutdown();
    m_effects.clear();
    DestroyIntermediateBuffers();
    m_initialized = false;
}

void PostProcessingPipeline::Resize(int width, int height) {
    m_width = width;
    m_height = height;
    DestroyIntermediateBuffers();
    CreateIntermediateBuffers(width, height);
    for (auto& e : m_effects) e->Resize(width, height);
}

bool PostProcessingPipeline::CreateIntermediateBuffers(int width, int height) {
    // Ping
    glCreateTextures(GL_TEXTURE_2D, 1, &m_pingTex);
    glTextureStorage2D(m_pingTex, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_pingTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_pingTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateFramebuffers(1, &m_pingFBO);
    glNamedFramebufferTexture(m_pingFBO, GL_COLOR_ATTACHMENT0, m_pingTex, 0);

    // Pong
    glCreateTextures(GL_TEXTURE_2D, 1, &m_pongTex);
    glTextureStorage2D(m_pongTex, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_pongTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_pongTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateFramebuffers(1, &m_pongFBO);
    glNamedFramebufferTexture(m_pongFBO, GL_COLOR_ATTACHMENT0, m_pongTex, 0);

    return true;
}

void PostProcessingPipeline::DestroyIntermediateBuffers() {
    if (m_pingFBO) { glDeleteFramebuffers(1, &m_pingFBO); m_pingFBO = 0; }
    if (m_pingTex) { glDeleteTextures(1, &m_pingTex); m_pingTex = 0; }
    if (m_pongFBO) { glDeleteFramebuffers(1, &m_pongFBO); m_pongFBO = 0; }
    if (m_pongTex) { glDeleteTextures(1, &m_pongTex); m_pongTex = 0; }
}

void PostProcessingPipeline::Apply(uint32_t inputTex, uint32_t outputFBO,
                                     int width, int height) {
    if (m_effects.empty()) return;

    // Count enabled effects
    int enabledCount = 0;
    for (const auto& e : m_effects) {
        if (e->IsEnabled()) enabledCount++;
    }
    if (enabledCount == 0) return;

    uint32_t currentInput = inputTex;
    bool usePing = true;
    int processed = 0;

    for (const auto& effect : m_effects) {
        if (!effect->IsEnabled()) continue;

        processed++;
        bool isLast = (processed == enabledCount);

        uint32_t targetFBO = isLast ? outputFBO : (usePing ? m_pingFBO : m_pongFBO);
        effect->Apply(currentInput, targetFBO, width, height);

        if (!isLast) {
            currentInput = usePing ? m_pingTex : m_pongTex;
            usePing = !usePing;
        }
    }
}

void PostProcessingPipeline::AddEffect(std::unique_ptr<PostEffect> effect) {
    m_effects.push_back(std::move(effect));
}

void PostProcessingPipeline::RemoveEffect(PostEffectType type) {
    m_effects.erase(
        std::remove_if(m_effects.begin(), m_effects.end(),
                        [type](const auto& e) { return e->GetType() == type; }),
        m_effects.end());
}

PostEffect* PostProcessingPipeline::GetEffect(PostEffectType type) {
    for (auto& e : m_effects) {
        if (e->GetType() == type) return e.get();
    }
    return nullptr;
}

void PostProcessingPipeline::SetEffectEnabled(PostEffectType type, bool enabled) {
    for (auto& e : m_effects) {
        if (e->GetType() == type) e->SetEnabled(enabled);
    }
}

void PostProcessingPipeline::ClearEffects() {
    for (auto& e : m_effects) e->Shutdown();
    m_effects.clear();
}

bool PostProcessingPipeline::HasEnabledEffects() const {
    for (const auto& e : m_effects) {
        if (e->IsEnabled()) return true;
    }
    return false;
}

void PostProcessingPipeline::SetupDefaultChain() {
    ClearEffects();
    // No effects by default — FXAA adds significant per-frame GPU overhead
    // on integrated GPUs (Intel Iris Xe). Enable via config if desired.
}

void PostProcessingPipeline::SetupRetroChain() {
    ClearEffects();
    auto crt = std::make_unique<CRTEffect>();
    crt->Init(m_width, m_height);
    AddEffect(std::move(crt));

    auto gamma = std::make_unique<GammaEffect>();
    gamma->Init(m_width, m_height);
    AddEffect(std::move(gamma));
}

void PostProcessingPipeline::SetupEnhancedChain() {
    ClearEffects();
    auto bloom = std::make_unique<BloomEffect>();
    bloom->Init(m_width, m_height);
    AddEffect(std::move(bloom));

    auto sharpen = std::make_unique<SharpenEffect>();
    sharpen->Init(m_width, m_height);
    AddEffect(std::move(sharpen));

    auto gamma = std::make_unique<GammaEffect>();
    gamma->Init(m_width, m_height);
    AddEffect(std::move(gamma));

    auto fxaa = std::make_unique<FXAAEffect>();
    fxaa->Init(m_width, m_height);
    AddEffect(std::move(fxaa));
}

} // namespace Nova64::Video
