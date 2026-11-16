/**
 * @file SceneTypes.hpp
 * @brief World-space scene geometry types for the hardware raytracing pipeline.
 *
 * The N64 RSP flattens 3D geometry to 2D screen-space before any data reaches
 * the RDP.  For RT we intercept vertices from GBIHandler BEFORE the MVP
 * projection is applied, yielding true 3D world-space positions that can be
 * uploaded to Vulkan acceleration structures.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Nova64::Video::RT {

// ─────────────────────────────────────────────────────────────────────────────
//  SceneVertex  —  one world-space vertex captured from the RSP
// ─────────────────────────────────────────────────────────────────────────────

struct SceneVertex {
    float worldPos[3]; // Object-space position × ModelView matrix
    float normal[3];   // Eye-space normal (normalised)
    float uv[2];       // Texture coordinates (raw S10.5 converted to float)
    float color[4];    // Vertex color / shade (RGBA 0-1)
};

// ─────────────────────────────────────────────────────────────────────────────
//  SceneLighting  —  the lights the game had set when a triangle was drawn
// ─────────────────────────────────────────────────────────────────────────────

/// One light as the game set it, in EYE space (the space the RT scene lives in).
struct SceneLight {
    bool valid = false;
    bool point = false;
    float vec[3] = {0, 0, 0};   ///< directional: unit vector TOWARD the light; point: position
    float color[3] = {1, 1, 1}; ///< 0..1
    float la = 0.0f, qa = 0.0f; ///< point light: the RSP's linear/quadratic attenuation bytes
};

/// What lights a triangle: the game's main directional light and, when it has
/// switched on positional lighting, its strongest point light (a torch, a fire,
/// a fairy). Both are stamped on every triangle so each object is shadowed by
/// the light that actually lit it.
struct SceneLighting {
    SceneLight sun;
    SceneLight local;
};

// ─────────────────────────────────────────────────────────────────────────────
//  SceneTriangle  —  one renderable triangle with material identity
// ─────────────────────────────────────────────────────────────────────────────

struct SceneTriangle {
    SceneVertex v[3];

    uint64_t materialHash; // Hash of RDP combine mode + tile CRCs (from combineMode)
    uint32_t geometryMode; // N64 geometry mode flags at submission time
    uint32_t otherModeH;   // RDP otherMode high word (for RT material decisions)
    uint32_t otherModeL;   // RDP otherMode low word
    bool isTranslucent;    // Blended translucent surface (water/glass)
    bool isCutout;         // Texture-alpha cutout (billboards/sprites)
    // True when the RDP color combiner's FINAL cycle multiplicatively
    // references SHADE (the per-vertex interpolated color — from dynamic
    // RSP lighting or raw vertex colors, whichever the DL supplied) in
    // producing this triangle's rasterized color. Only such triangles can
    // have their baked shading validly divided back out for RT's de-light/
    // re-light step; for anything else (pure PRIMITIVE/ENVIRONMENT color,
    // texture-only combiners, etc.) shade has zero influence on the raster
    // output, so dividing by it would apply a meaningless correction.
    // Computed from materialHash (the raw combine mode) + otherModeH's
    // cycle-type bits — see GBIHandler::ComputeUsesShadeInFinalColor.
    bool usesShadeInFinalColor;

    // Indices into the frame's light table (RTSceneManager::Lights()); 0xFF = none.
    // Stamped by the scene manager from the lighting current at submission time.
    uint8_t sunLight = 0xFF;
    uint8_t localLight = 0xFF;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ISceneGeometryConsumer  —  sink for world-space triangles
// ─────────────────────────────────────────────────────────────────────────────

class ISceneGeometryConsumer {
public:
    virtual ~ISceneGeometryConsumer() = default;

    /// Called once at the start of each display-list frame.
    virtual void NewFrame() = 0;

    /// Submit a batch of world-space triangles.  May be called many times
    /// per frame (once per tri1/tri2/quad command dispatch).
    virtual void SubmitTriangles(const SceneTriangle* tris, uint32_t count) = 0;

    void SubmitTriangle(const SceneTriangle& tri) { SubmitTriangles(&tri, 1); }

    /// Optional: report the active EYE->CLIP projection matrix (row-major,
    /// N64 row-vector convention) when a perspective triangle is submitted.
    /// The compute ray tracer uses its inverse to generate primary rays.
    /// Default no-op so non-RT consumers are unaffected.
    virtual void SetCamera(const float proj[16]) { (void)proj; }

    /// Optional: report the active N64 viewport {scaleX, transX, scaleY, transY}
    /// (N64 framebuffer units, scaleY negative = y-flip). Title screens and
    /// cutscenes render with non-fullscreen/letterboxed viewports; the ray
    /// tracer must map window pixels through the SAME viewport or its terms
    /// land offset/scaled relative to the raster. Default no-op.
    virtual void SetViewport(const float vp[4]) { (void)vp; }

    /// Optional: report the lights that apply to the triangles submitted from
    /// now on (until the next call). Default no-op.
    virtual void SetLighting(const SceneLighting& lighting) { (void)lighting; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  NullGeometryConsumer  —  safe no-op default (used before RT backend inits)
// ─────────────────────────────────────────────────────────────────────────────

class NullGeometryConsumer final : public ISceneGeometryConsumer {
public:
    void NewFrame() override {}
    void SubmitTriangles(const SceneTriangle*, uint32_t) override {}
};

// ─────────────────────────────────────────────────────────────────────────────
//  SceneFrameData  —  accumulated triangles for one frame (used by RTSceneManager)
// ─────────────────────────────────────────────────────────────────────────────

struct SceneFrameData {
    std::vector<SceneTriangle> triangles;

    void Reset() { triangles.clear(); }
    uint32_t TriangleCount() const { return static_cast<uint32_t>(triangles.size()); }
};

} // namespace Nova64::Video::RT
