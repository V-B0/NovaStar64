/**
 * @file MLEDirector.hpp
 * @brief Mixed-Level Emulation director — heuristic router between HLE/LLE paths.
 *
 * MLEDirector inspects each RDP batch at FlushBatch() time and classifies it
 * into one of three rendering levels:
 *
 *   HLE_Fast     — Simple fill/copy operations, solid-colour rectangles,
 *                  UI overlays.  Rendered entirely by the GL/Vulkan pipeline
 *                  with no extra work.
 *
 *   HLE_Accurate — Geometry with complex combiners, alpha blending
 *                  (ALPHA_CVG_SEL, translucency), subpixel effects.
 *                  Rendered by the GL/Vulkan pipeline with full shader accuracy.
 *
 *   LLE          — Patterns known to need true RDP accuracy (Banjo-Kazooie
 *                  mirror floor, OOT heat haze, etc.).  Reserved for a future
 *                  LLE RDP back-end; currently falls back to HLE_Accurate.
 *
 * Wiring:
 *   VideoPlugin creates one MLEDirector and passes a pointer to RDPProcessor
 *   via SetMLEDirector().  RDPProcessor::FlushBatch() calls
 *   MLEDirector::Evaluate(batch, state) before drawing.
 *
 * The result is advisory — the current implementation only logs and sets a
 * per-batch classification for the RT compose pass (Phase 9) to consume.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "../Core/RDPState.hpp"
#include "../Common/Logger.hpp"
#include <cstdint>
#include <cstdlib>  // getenv

namespace Nova64::Video {

// ─────────────────────────────────────────────────────────────────────────────
//  MLELevel  —  rendering path classification
// ─────────────────────────────────────────────────────────────────────────────
enum class MLELevel : uint8_t {
    HLE_Fast     = 0,   // Rasterise with current GL/VK pipeline, no accuracy pass
    HLE_Accurate = 1,   // Full combiner/blender accuracy via GL/VK shaders
    LLE          = 2,   // True RDP emulation (fallback to HLE_Accurate for now)
};

// ─────────────────────────────────────────────────────────────────────────────
//  RenderBatch metadata (minimal view for the director)
// ─────────────────────────────────────────────────────────────────────────────
struct BatchInfo {
    uint32_t drawCallCount  = 0;
    uint32_t triangleCount  = 0;

    // Aggregate RDP state from the batch draw calls
    uint32_t otherModeH     = 0;
    uint32_t otherModeL     = 0;
    uint64_t combineModeRaw = 0;    // First combine mode word in batch
    uint32_t geometryMode   = 0;

    bool     hasFillOps     = false;    // Any G_FILLRECT / cycle-type Fill
    bool     hasCopyOps     = false;    // Any cycle-type Copy
    bool     hasTexturedTri = false;    // Any triangle with a texture bound
    bool     hasTranslucent = false;    // Any triangle with ALPHA_CVG_SEL/blending
};

// ─────────────────────────────────────────────────────────────────────────────
//  MLEDirector
// ─────────────────────────────────────────────────────────────────────────────
class MLEDirector {
public:
    MLEDirector() = default;
    ~MLEDirector() = default;

    /// Classify a batch and return the recommended rendering path.
    /// Called by RDPProcessor::FlushBatch() before any GPU submission.
    MLELevel Evaluate(const BatchInfo& batch, const RDPState& state) {
        // ── Rule 1: Fill/Copy-only batches → HLE_Fast ─────────────────
        if (batch.hasFillOps && !batch.hasTexturedTri && !batch.hasTranslucent) {
            LogIfEnabled(MLELevel::HLE_Fast, "fill-only batch");
            return MLELevel::HLE_Fast;
        }
        if (batch.hasCopyOps && !batch.hasTexturedTri && !batch.hasTranslucent) {
            LogIfEnabled(MLELevel::HLE_Fast, "copy-only batch");
            return MLELevel::HLE_Fast;
        }

        // ── Rule 2: Translucent / alpha coverage → HLE_Accurate ───────
        //   ALPHA_CVG_SEL bit 6 of otherModeL, or CVG_TIMES_ALPHA bit 12
        const bool alphaCvgSel   = (state.otherModeL & (1u << 6)) != 0;
        const bool cvgTimesAlpha = (state.otherModeL & (1u << 12)) != 0;
        if (alphaCvgSel || cvgTimesAlpha || batch.hasTranslucent) {
            LogIfEnabled(MLELevel::HLE_Accurate, "translucent alpha batch");
            return MLELevel::HLE_Accurate;
        }

        // ── Rule 3: Known LLE patterns (game-specific heuristics) ──────
        if (IsKnownLLEPattern(batch, state)) {
            // Currently we don't have an LLE back-end, fall back.
            LogIfEnabled(MLELevel::LLE, "known LLE pattern (fallback to HLE_Accurate)");
            return MLELevel::LLE;
        }

        // ── Default: HLE_Fast ──────────────────────────────────────────
        LogIfEnabled(MLELevel::HLE_Fast, "default");
        return MLELevel::HLE_Fast;
    }

private:
    // ─── LLE Pattern Database ─────────────────────────────────────────────
    // These identify batches whose visual output is known to be incorrect
    // under pure HLE due to RDP-level accuracy requirements.
    //
    // Patterns are matched by combine mode hash + geometry mode flags.
    // The hashes below are indicative — they must be calibrated against
    // real ROM captures (see dump/ directory probes).

    bool IsKnownLLEPattern(const BatchInfo& batch, const RDPState& /*state*/) const {
        // Banjo-Kazooie mirror floor: textured with LOD + perspective correct +
        // specific combine mode involving COMBINED_ALPHA.  No geometry flags.
        // (Placeholder hash — calibrate from ROM probe.)
        constexpr uint64_t kBKMirrorCombineHash = 0xFC121824FC65E4FFull;
        if (batch.combineModeRaw == kBKMirrorCombineHash &&
            batch.hasTexturedTri && !batch.hasTranslucent) {
            return true;
        }

        // Heat haze / OOT fire: cycle-2 mode with LOD_FRAC in alpha channel.
        constexpr uint64_t kHeatHazeCombineHash  = 0xFC119623FE043020ull;
        if (batch.combineModeRaw == kHeatHazeCombineHash) {
            return true;
        }

        return false;
    }

    // ─── Debug logging ────────────────────────────────────────────────────
    void LogIfEnabled(MLELevel level, const char* reason) const {
        static const bool kEnabled = []() {
            const char* v = std::getenv("MLE_DIRECTOR_DEBUG");
            return v && v[0] != '0';
        }();
        if (!kEnabled) return;
        const char* levelStr = (level == MLELevel::HLE_Fast)     ? "HLE_Fast"
                             : (level == MLELevel::HLE_Accurate) ? "HLE_Accurate"
                             :                                      "LLE";
        Logger::Info("[MLE] level=%s  reason=%s", levelStr, reason);
    }
};

} // namespace Nova64::Video
