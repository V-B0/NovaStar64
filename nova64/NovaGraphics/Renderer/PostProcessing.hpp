/**
 * @file PostProcessing.hpp
 * @brief Post-processing effects (gamma, FXAA, bloom, CRT, sharpen and others) and the pipeline that chains them
 *        on the final resolved framebuffer.
 */

#pragma once

#include "../Common/Types.hpp"
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace Nova64::Video {

// --- Post-processing effect types ---

enum class PostEffectType : uint8_t {
    GammaCorrection,
    FXAA,
    SMAA,
    Bloom,
    CRT,
    Sharpen,
    ChromaticAberration,
    FilmGrain,
    Dithering,
    ColorGrading,
    DepthOfField,
    MotionBlur,
    Vignette,
};

// --- Effect parameters ---

struct GammaParams {
    float gamma = 2.2f;
    float brightness = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
};

struct FXAAParams {
    float qualitySubpix = 0.75f;      // Sub-pixel aliasing removal amount
    float edgeThreshold = 0.166f;     // Minimum amount of local contrast required
    float edgeThresholdMin = 0.0833f; // Trims the algorithm from processing darks
};

struct BloomParams {
    float threshold = 1.0f; // Brightness threshold
    float intensity = 0.5f; // Bloom intensity
    float radius = 0.5f;    // Blur radius
    int mipLevels = 5;      // Downsample levels (5 = 1/32 res)
    float softKnee = 0.5f;  // Soft threshold knee
};

struct CRTParams {
    float curvature = 0.03f;       // Screen curvature amount
    float scanlineWeight = 0.3f;   // Scanline darkness
    float maskDotPitch = 0.65f;    // Shadow mask dot pitch
    float cornerBlur = 0.02f;      // Corner edge blur
    bool interlace = false;        // Interlace effect
    float phosphorPersist = 0.0f;  // Phosphor persistence (0 = off)
    float vignetteStrength = 0.3f; // Vignette at edges
};

struct SharpenParams {
    float strength = 0.5f;
    float clamp = 0.035f; // Limit sharpening on bright pixels
};

struct ChromaticAberrationParams {
    float intensity = 0.003f;
    float offsetR = 1.0f;
    float offsetB = -1.0f;
};

struct FilmGrainParams {
    float intensity = 0.05f;
    float response = 0.5f; // Grain response to luminance
};

struct ColorGradingParams {
    float temperature = 0.0f; // Color temperature shift
    float tint = 0.0f;        // Green-magenta shift
    float hueShift = 0.0f;    // Hue rotation
    float shadows = 0.0f;     // Shadow adjustment
    float highlights = 0.0f;  // Highlight adjustment
    // Lift/gamma/gain
    float liftR = 0, liftG = 0, liftB = 0;
    float gainR = 1, gainG = 1, gainB = 1;
};

struct VignetteParams {
    float intensity = 0.5f;
    float roundness = 0.5f;
    float smoothness = 0.5f;
};

// --- Post-effect base ---

class PostEffect {
public:
    virtual ~PostEffect() = default;
    virtual PostEffectType GetType() const = 0;
    virtual bool Init(int width, int height) = 0;
    virtual void Shutdown() = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) = 0;
    virtual bool IsEnabled() const { return m_enabled; }
    virtual void SetEnabled(bool e) { m_enabled = e; }

protected:
    bool m_enabled = true;
};

// --- Gamma correction effect ---

class GammaEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::GammaCorrection; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    GammaParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- FXAA effect ---

class FXAAEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::FXAA; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    FXAAParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- Bloom effect (multi-pass) ---

class BloomEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::Bloom; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    BloomParams params;

private:
    struct MipLevel {
        uint32_t fbo = 0;
        uint32_t tex = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<MipLevel> m_mips;
    uint32_t m_thresholdShader = 0;
    uint32_t m_downsampleShader = 0;
    uint32_t m_upsampleShader = 0;
    uint32_t m_compositeShader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- CRT filter ---

class CRTEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::CRT; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    CRTParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- Sharpen effect ---

class SharpenEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::Sharpen; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    SharpenParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- Chromatic Aberration effect ---

class ChromaticAberrationEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::ChromaticAberration; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    ChromaticAberrationParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- Film Grain effect ---

class FilmGrainEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::FilmGrain; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    void SetFrameCount(uint32_t frame) { m_frameCount = frame; }
    FilmGrainParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
    uint32_t m_frameCount = 0;
};

// --- Dithering effect ---

class DitheringEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::Dithering; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- Color Grading effect ---

class ColorGradingEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::ColorGrading; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    ColorGradingParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- Depth of Field effect ---

struct DepthOfFieldParams {
    float focalDistance = 0.5f;
    float focalRange = 0.2f;
    float blurStrength = 3.0f;
};

class DepthOfFieldEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::DepthOfField; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    void SetDepthTexture(uint32_t tex) { m_depthTex = tex; }
    DepthOfFieldParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
    uint32_t m_depthTex = 0;
};

// --- Motion Blur effect ---

struct MotionBlurParams {
    float strength = 0.5f;
    int samples = 8;
};

class MotionBlurEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::MotionBlur; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    MotionBlurParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
    uint32_t m_prevTex = 0;
    int m_width = 0;
    int m_height = 0;
};

// --- Vignette effect ---

class VignetteEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::Vignette; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

    VignetteParams params;

private:
    uint32_t m_shader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
};

// --- SMAA effect (edge-detect + blend) ---

class SMAAEffect : public PostEffect {
public:
    PostEffectType GetType() const override { return PostEffectType::SMAA; }
    bool Init(int width, int height) override;
    void Shutdown() override;
    void Resize(int width, int height) override;
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) override;

private:
    uint32_t m_edgeShader = 0;
    uint32_t m_blendShader = 0;
    uint32_t m_vao = 0;
    uint32_t m_vbo = 0;
    uint32_t m_edgeTex = 0;
    uint32_t m_edgeFBO = 0;
    int m_width = 0;
    int m_height = 0;
};

// --- PostProcessingPipeline ---

class PostProcessingPipeline {
public:
    PostProcessingPipeline();
    ~PostProcessingPipeline();

    bool Init(int width, int height);
    void Shutdown();
    void Resize(int width, int height);

    // Applies all enabled effects in order.
    // inputTex: GL texture with the rendered scene.
    // outputFBO: 0 for the default framebuffer, or the target FBO.
    void Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height);

    // --- Effect management ---
    void ClearEffects();

    // --- Quick setup ---
    void SetupDefaultChain(); // Empty chain: no effects
    // CRT + Gamma
    // Bloom + Sharpen + Gamma + FXAA

    int GetEffectCount() const { return static_cast<int>(m_effects.size()); }
    bool HasEnabledEffects() const;

private:
    bool CreateIntermediateBuffers(int width, int height);
    void DestroyIntermediateBuffers();

    std::vector<std::unique_ptr<PostEffect>> m_effects;

    // Ping-pong framebuffers for chaining passes.
    uint32_t m_pingFBO = 0;
    uint32_t m_pingTex = 0;
    uint32_t m_pongFBO = 0;
    uint32_t m_pongTex = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_initialized = false;
};

} // namespace Nova64::Video
