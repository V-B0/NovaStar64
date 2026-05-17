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
    float worldPos[3];   // Object-space position × ModelView matrix
    float normal[3];     // Eye-space normal (normalised)
    float uv[2];         // Texture coordinates (raw S10.5 converted to float)
    float color[4];      // Vertex color / shade (RGBA 0-1)
};

// ─────────────────────────────────────────────────────────────────────────────
//  SceneTriangle  —  one renderable triangle with material identity
// ─────────────────────────────────────────────────────────────────────────────

struct SceneTriangle {
    SceneVertex v[3];

    uint64_t materialHash;      // Hash of RDP combine mode + tile CRCs (from combineMode)
    uint32_t geometryMode;      // N64 geometry mode flags at submission time
    uint32_t otherModeH;        // RDP otherMode high word (for RT material decisions)
    uint32_t otherModeL;        // RDP otherMode low word
    bool     isTranslucent;     // True when CVG_TIMES_ALPHA or ALPHA_CVG_SEL set
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
