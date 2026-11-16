/**
 * @file PostProcessing.cpp
 * @brief Post-processing effect implementations (GLSL passes) and the pipeline that chains them.
 */

#include "PostProcessing.hpp"
#include "../Common/Logger.hpp"

#include <glad/gl.h>
#include <cstring>

namespace Nova64::Video {

// --- Full-screen quad helper ---

// --- Vertex shader (shared) ---

// --- Gamma Correction ---

// --- FXAA ---

// --- Bloom (multi-pass downsample/upsample) ---

// --- CRT Filter ---

// --- Sharpen (CAS-style) ---

// --- Chromatic Aberration ---

// --- Film Grain ---

// --- Dithering (Bayer 8x8 ordered dither) ---

// --- Color Grading ---

// --- Depth of Field (disk blur weighted by depth) ---

// --- Motion Blur (frame accumulation) ---

// --- Vignette ---

// --- SMAA (edge-detect + neighborhood blend) ---

// --- PostProcessingPipeline ---

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
    glCreateTextures(GL_TEXTURE_2D, 1, &m_pingTex);
    glTextureStorage2D(m_pingTex, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_pingTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_pingTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateFramebuffers(1, &m_pingFBO);
    glNamedFramebufferTexture(m_pingFBO, GL_COLOR_ATTACHMENT0, m_pingTex, 0);

    glCreateTextures(GL_TEXTURE_2D, 1, &m_pongTex);
    glTextureStorage2D(m_pongTex, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_pongTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_pongTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateFramebuffers(1, &m_pongFBO);
    glNamedFramebufferTexture(m_pongFBO, GL_COLOR_ATTACHMENT0, m_pongTex, 0);

    return true;
}

void PostProcessingPipeline::DestroyIntermediateBuffers() {
    if (m_pingFBO) {
        glDeleteFramebuffers(1, &m_pingFBO);
        m_pingFBO = 0;
    }
    if (m_pingTex) {
        glDeleteTextures(1, &m_pingTex);
        m_pingTex = 0;
    }
    if (m_pongFBO) {
        glDeleteFramebuffers(1, &m_pongFBO);
        m_pongFBO = 0;
    }
    if (m_pongTex) {
        glDeleteTextures(1, &m_pongTex);
        m_pongTex = 0;
    }
}

void PostProcessingPipeline::Apply(uint32_t inputTex, uint32_t outputFBO, int width, int height) {
    if (m_effects.empty()) return;

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
    // No effects by default: FXAA adds significant per-frame GPU overhead on
    // integrated GPUs (Intel Iris Xe).
}

} // namespace Nova64::Video
