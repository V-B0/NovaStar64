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

// --- FXAA effect ---

// --- Bloom effect (multi-pass) ---

// --- CRT filter ---

// --- Sharpen effect ---

// --- Chromatic Aberration effect ---

// --- Film Grain effect ---

// --- Dithering effect ---

// --- Color Grading effect ---

// --- Depth of Field effect ---

struct DepthOfFieldParams {
    float focalDistance = 0.5f;
    float focalRange = 0.2f;
    float blurStrength = 3.0f;
};

// --- Motion Blur effect ---

struct MotionBlurParams {
    float strength = 0.5f;
    int samples = 8;
};

// --- Vignette effect ---

// --- SMAA effect (edge-detect + blend) ---

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
