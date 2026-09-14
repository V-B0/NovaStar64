/**
 * @file RTSceneManager.hpp
 * @brief World-space scene accumulator and Vulkan acceleration structure builder.
 *
 * RTSceneManager implements ISceneGeometryConsumer (the sink that receives
 * per-triangle world-space data from GBIHandler::DrawTriangle) and acts as
 * the bridge into the Vulkan hardware raytracing pipeline.
 *
 * Lifecycle (per frame):
 *   1. VideoPlugin calls NewFrame() at the start of ProcessGraphicsTask.
 *   2. GBIHandler::DrawTriangle calls SubmitTriangles() for every visible tri.
 *   3. After MicrocodeDispatcher::OnFrameEnd(), VideoPlugin calls BuildAS()
 *      which uploads geometry and records BLAS + TLAS build commands.
 *   4. VulkanRenderer::DispatchRayTracing() consumes the TLAS handle.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "SceneTypes.hpp"
#include "../Common/Logger.hpp"
#include "../Renderer/VK/VKAccelStruct.hpp"
#include "../Renderer/VK/VKMemory.hpp"
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace Nova64::Video::RT {

// ─────────────────────────────────────────────────────────────────────────────
//  RTSceneManager
// ─────────────────────────────────────────────────────────────────────────────

class RTSceneManager final : public ISceneGeometryConsumer {
public:
    RTSceneManager() = default;
    ~RTSceneManager() override = default;

    // ─── ISceneGeometryConsumer ──────────────────────────────────────
    void NewFrame() override {
        m_frame.Reset();
        m_hasCamera = false;
        m_lightFinalized = false;
        m_dirAccum[0] = m_dirAccum[1] = m_dirAccum[2] = 0.0f;
        m_colAccum[0] = m_colAccum[1] = m_colAccum[2] = 0.0f;
        m_lightSamples = 0;
    }

    void SubmitTriangles(const SceneTriangle* tris, uint32_t count) override {
        m_frame.triangles.insert(m_frame.triangles.end(), tris, tris + count);
    }

    /// Capture the perspective projection used for the 3D scene (last one wins,
    /// which is the main world camera). Stored row-major (N64 row-vector form).
    void SetCamera(const float proj[16]) override {
        for (int i = 0; i < 16; ++i) m_proj[i] = proj[i];
        m_hasCamera = true;
    }

    /// Capture the matching N64 viewport {scaleX, transX, scaleY, transY}.
    void SetViewport(const float vp[4]) override {
        for (int i = 0; i < 4; ++i) m_viewport[i] = vp[i];
    }

    /// Accumulate the primary directional light (eye-space dir + rgb color).
    ///
    /// Called once per submitted triangle. N64 games (SM64 especially) give
    /// EVERY OBJECT its own light — direction AND color (object material colors
    /// are baked into light colors). Taking the last call verbatim made the RT
    /// sun flicker red/yellow/green and point wherever the last-drawn object's
    /// light pointed (whole characters wrongly self-shadowed). Instead we
    /// average over all calls — the level terrain dominates by triangle count —
    /// and temporally smooth the result when it's queried.
    void SetLight(const float dir[3], const float color[3]) override {
        m_dirAccum[0] += dir[0]; m_dirAccum[1] += dir[1]; m_dirAccum[2] += dir[2];
        m_colAccum[0] += color[0]; m_colAccum[1] += color[1]; m_colAccum[2] += color[2];
        ++m_lightSamples;
        m_hasLight = true;
    }

    // ─── Accessors ───────────────────────────────────────────────────

    /// Returns the number of world-space triangles accumulated this frame.
    uint32_t GetTriangleCount() const { return m_frame.TriangleCount(); }

    /// Direct access to the accumulated frame data (for CPU-side debug/export).
    const SceneFrameData& GetFrameData() const { return m_frame; }

    /// Camera projection (row-major) captured this frame; false if none seen.
    bool        HasCamera()      const { return m_hasCamera; }
    const float* Projection()    const { return m_proj; }
    /// N64 viewport {scaleX, transX, scaleY, transY} (fullscreen default).
    const float* Viewport()      const { return m_viewport; }

    /// Primary directional light for this frame (eye-space): the accumulated
    /// average of all per-object lights, temporally smoothed across frames.
    bool         HasLight()    const { return m_hasLight; }
    const float* LightDir()    const { FinalizeLight(); return m_lightDir; }
    const float* LightColor()  const { FinalizeLight(); return m_lightColor; }

    // ─── Acceleration Structure Build ────────────────────────────────

    /**
     * Upload geometry and build BLAS+TLAS for this frame's triangles.
     *
     * Must be called after all SubmitTriangle calls for the frame and before
     * VulkanRenderer::DispatchRayTracing().
     *
     * @param ctx  VKContext for device/pool/queue.
     * @param cmd  Open command buffer to record build commands into.
     *             The caller must submit cmd and wait for completion before
     *             using the resulting TLAS.
     * @return true on success (even when no triangles → produces empty TLAS).
     */
    bool BuildAS(const Nova64::Video::VK::VKContext& ctx, VkCommandBuffer cmd);

    /**
     * Returns the TLAS handle (VkAccelerationStructureKHR cast to void*) for
     * use by DispatchRayTracing. Returns nullptr when RT is not ready.
     */
    void* GetTLAS() const {
        return m_tlas && m_tlas->IsBuilt()
            ? reinterpret_cast<void*>(m_tlas->Get())
            : nullptr;
    }

    // ─── Debug ───────────────────────────────────────────────────────

    /// Log triangle count (activated by RT_SCENE_DEBUG=1 env var).
    void DebugLogIfEnabled() const {
        static const bool kEnabled = []() {
#pragma warning(push)
#pragma warning(disable: 4996)
            const char* v = std::getenv("RT_SCENE_DEBUG");
#pragma warning(pop)
            return v && v[0] != '0';
        }();
        if (kEnabled) {
            Logger::Info("[RTScene] frame tris=%zu opaque=%u translucent=%u",
                         m_frame.triangles.size(),
                         CountByType(false),
                         CountByType(true));
        }
    }

private:
    SceneFrameData m_frame;

    // Compute-RT camera capture (row-major eye->clip projection).
    float m_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool  m_hasCamera = false;
    // N64 viewport (fullscreen 320×240 default; scaleY negative = y-flip).
    float m_viewport[4] = {160.0f, 160.0f, -120.0f, 120.0f};

    // Compute-RT light: per-frame accumulators + smoothed output (eye-space).
    float m_dirAccum[3] = {0.0f, 0.0f, 0.0f};
    float m_colAccum[3] = {0.0f, 0.0f, 0.0f};
    uint32_t m_lightSamples = 0;
    bool  m_hasLight = false;
    mutable bool  m_lightFinalized = false;
    mutable bool  m_lightEverSet   = false;
    mutable float m_lightDir[3]   = {0.0f, 1.0f, 0.0f};
    mutable float m_lightColor[3] = {1.0f, 1.0f, 1.0f};

    /// Turn this frame's accumulated per-object lights into one stable sun:
    /// average (terrain dominates by triangle count), bias the color halfway
    /// to white (object-material tints average out but never fully), and lerp
    /// against last frame's value so residual draw-order variation can't
    /// flicker. Idempotent per frame.
    void FinalizeLight() const {
        if (m_lightFinalized || m_lightSamples == 0) return;
        m_lightFinalized = true;

        float inv = 1.0f / static_cast<float>(m_lightSamples);
        float d[3] = {m_dirAccum[0]*inv, m_dirAccum[1]*inv, m_dirAccum[2]*inv};
        float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        // Opposing per-object lights can cancel out; keep last frame's sun then.
        if (len < 0.05f) return;
        d[0] /= len; d[1] /= len; d[2] /= len;

        float c[3] = {m_colAccum[0]*inv, m_colAccum[1]*inv, m_colAccum[2]*inv};
        for (int i = 0; i < 3; ++i) c[i] = 0.5f * c[i] + 0.5f;   // bias to white

        if (!m_lightEverSet) {
            m_lightEverSet = true;
            for (int i = 0; i < 3; ++i) { m_lightDir[i] = d[i]; m_lightColor[i] = c[i]; }
            return;
        }
        constexpr float kSmooth = 0.25f;   // per-frame blend toward the new value
        float sm[3];
        for (int i = 0; i < 3; ++i)
            sm[i] = m_lightDir[i] + (d[i] - m_lightDir[i]) * kSmooth;
        float smLen = std::sqrt(sm[0]*sm[0] + sm[1]*sm[1] + sm[2]*sm[2]);
        if (smLen > 1e-4f)
            for (int i = 0; i < 3; ++i) m_lightDir[i] = sm[i] / smLen;
        for (int i = 0; i < 3; ++i)
            m_lightColor[i] += (c[i] - m_lightColor[i]) * kSmooth;
    }

    // Acceleration structures rebuilt each frame.
    // BLASes are grouped by materialHash to minimize AS count.
    std::vector<std::unique_ptr<Nova64::Video::VK::VKBlas>> m_blases;
    std::unique_ptr<Nova64::Video::VK::VKTlas>               m_tlas;

    uint32_t CountByType(bool translucent) const {
        uint32_t n = 0;
        for (const auto& t : m_frame.triangles)
            if (t.isTranslucent == translucent) ++n;
        return n;
    }
};

} // namespace Nova64::Video::RT

// ─────────────────────────────────────────────────────────────────────────────
//  RTSceneManager::BuildAS  —  inline implementation
// ─────────────────────────────────────────────────────────────────────────────

#include <cstring>

inline bool Nova64::Video::RT::RTSceneManager::BuildAS(
        const Nova64::Video::VK::VKContext& ctx,
        VkCommandBuffer cmd) {
    using namespace Nova64::Video::VK;

    m_blases.clear();
    m_tlas.reset();

    const auto& tris = m_frame.triangles;
    if (tris.empty()) return true; // nothing to build

    // ── Group triangles by materialHash ─────────────────────────────────
    std::unordered_map<uint64_t, std::vector<const SceneTriangle*>> groups;
    for (const auto& t : tris)
        groups[t.materialHash].push_back(&t);

    // ── Build one BLAS per material group ─────────────────────────────────
    std::vector<TlasInstance> tlasInstances;
    tlasInstances.reserve(groups.size());

    uint32_t customIndex = 0;
    for (auto& [hash, triPtrs] : groups) {
        // Flatten SceneTriangle world positions into a float3 array.
        std::vector<float> verts;
        verts.reserve(triPtrs.size() * 9);
        for (const SceneTriangle* t : triPtrs) {
            for (int v = 0; v < 3; ++v) {
                verts.push_back(t->v[v].worldPos[0]);
                verts.push_back(t->v[v].worldPos[1]);
                verts.push_back(t->v[v].worldPos[2]);
            }
        }

        auto blas = std::make_unique<VKBlas>();
        const uint32_t vertCount = static_cast<uint32_t>(verts.size() / 3);
        if (!blas->Build(ctx, cmd, verts.data(), vertCount, nullptr, 0)) {
            Logger::Warn("[RTScene] BLAS build failed for materialHash=0x{:016x}", hash);
            ++customIndex;
            continue;
        }

        // Identity transform: row-major 3×4
        TlasInstance inst{};
        inst.blasDeviceAddress  = blas->GetDeviceAddress();
        inst.instanceCustomIndex = customIndex & 0xFFFFFF;
        inst.mask               = 0xFF;
        inst.hitGroupIndex      = 0;
        // Row 0: [1 0 0 0]
        inst.transform[0] = 1.0f; inst.transform[1]  = 0.0f;
        inst.transform[2] = 0.0f; inst.transform[3]  = 0.0f;
        // Row 1: [0 1 0 0]
        inst.transform[4] = 0.0f; inst.transform[5]  = 1.0f;
        inst.transform[6] = 0.0f; inst.transform[7]  = 0.0f;
        // Row 2: [0 0 1 0]
        inst.transform[8] = 0.0f; inst.transform[9]  = 0.0f;
        inst.transform[10] = 1.0f; inst.transform[11] = 0.0f;
        tlasInstances.push_back(inst);

        m_blases.push_back(std::move(blas));
        ++customIndex;
    }

    if (tlasInstances.empty()) return true;

    // ── Build TLAS ─────────────────────────────────────────────────────────
    m_tlas = std::make_unique<VKTlas>();
    if (!m_tlas->Build(ctx, cmd, tlasInstances)) {
        Logger::Error("[RTScene] TLAS build failed");
        m_tlas.reset();
        return false;
    }
    return true;
}
