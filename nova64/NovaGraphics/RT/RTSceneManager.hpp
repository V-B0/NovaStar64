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
#include <cstring>

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
        m_lightCount = 0;
        for (uint32_t& c : m_sunUse) c = 0;
        m_curSun = m_curLocal = kNoLight;
        m_haveLighting = false;
    }

    void SubmitTriangles(const SceneTriangle* tris, uint32_t count) override {
        const size_t first = m_frame.triangles.size();
        m_frame.triangles.insert(m_frame.triangles.end(), tris, tris + count);
        for (size_t i = first; i < m_frame.triangles.size(); ++i) {
            SceneTriangle& t = m_frame.triangles[i];
            t.sunLight = m_curSun;
            t.localLight = m_curLocal;
            if (m_curSun != kNoLight) ++m_sunUse[m_curSun];
        }
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

    /// The lights in force for the triangles that follow. N64 games light every
    /// object with its own light (SM64 bakes material colours into them, Zelda
    /// rewrites its sun every frame for the time of day, and positional lights
    /// come and go with torches and fires), so each triangle is stamped with the
    /// light that lit it and the tracer shadows it toward that light -- there is
    /// no scene-wide average and nothing is smoothed across frames, so a moving
    /// light moves its shadows on the very frame it moves.
    void SetLighting(const SceneLighting& lighting) override {
        if (m_haveLighting && SameLight(lighting.sun, m_lastSun) && SameLight(lighting.local, m_lastLocal)) return;
        m_lastSun = lighting.sun;
        m_lastLocal = lighting.local;
        m_haveLighting = true;
        m_curSun = lighting.sun.valid ? Intern(lighting.sun, false) : kNoLight;
        m_curLocal = lighting.local.valid ? Intern(lighting.local, true) : kNoLight;
    }

    // ─── Accessors ───────────────────────────────────────────────────

    /// Returns the number of world-space triangles accumulated this frame.
    uint32_t GetTriangleCount() const { return m_frame.TriangleCount(); }

    /// Direct access to the accumulated frame data (for CPU-side debug/export).
    const SceneFrameData& GetFrameData() const { return m_frame; }

    /// Camera projection (row-major) captured this frame; false if none seen.
    bool HasCamera() const { return m_hasCamera; }
    const float* Projection() const { return m_proj; }
    /// N64 viewport {scaleX, transX, scaleY, transY} (fullscreen default).
    const float* Viewport() const { return m_viewport; }

    /// Index 15 (kNoLight) means "no light" in a triangle's light fields, so a
    /// table holds at most 15 entries.
    static constexpr uint32_t kMaxLights = 15;
    static constexpr uint8_t kNoLight = 0xFF;

    /// One light-table row, laid out the way the compute shaders read it (two
    /// vec4s): a = {direction toward the light | position, la}, b = {rgb, qa}.
    struct LightEntry {
        float a[4];
        float b[4];
    };
    uint32_t LightCount() const { return m_lightCount; }
    const LightEntry* Lights() const { return m_lights; }

    /// The directional light that most of this frame's triangles used. Triangles
    /// the game drew unlit (vertex colours, sky, effects) have no light of their
    /// own; they take this one so shadows still land on them.
    bool HasSun() const { return FallbackSun() != kNoLight; }
    const float* SunDir() const { return m_lights[FallbackSun() == kNoLight ? 0 : FallbackSun()].a; }

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
    void* GetTLAS() const { return m_tlas && m_tlas->IsBuilt() ? reinterpret_cast<void*>(m_tlas->Get()) : nullptr; }

    // ─── Debug ───────────────────────────────────────────────────────

    /// Log triangle count (activated by NOVA64_RT_SCENE_DEBUG=1 env var).
    void DebugLogIfEnabled() const {
        static const bool kEnabled = []() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
            const char* v = std::getenv("NOVA64_RT_SCENE_DEBUG");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
            return v && v[0] != '0';
        }();
        if (kEnabled) {
            Logger::Info("[RTScene] frame tris=%zu opaque=%u translucent=%u", m_frame.triangles.size(),
                         CountByType(false), CountByType(true));
        }
    }

private:
    SceneFrameData m_frame;

    // Compute-RT camera capture (row-major eye->clip projection).
    float m_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool m_hasCamera = false;
    // N64 viewport (fullscreen 320×240 default; scaleY negative = y-flip).
    float m_viewport[4] = {160.0f, 160.0f, -120.0f, 120.0f};

    // Compute-RT lights: this frame's table plus the entries currently stamped
    // on submitted triangles.
    LightEntry m_lights[kMaxLights] = {};
    bool m_lightIsPoint[kMaxLights] = {};
    uint32_t m_lightCount = 0;
    uint32_t m_sunUse[kMaxLights] = {}; // triangles per directional entry
    uint8_t m_curSun = kNoLight, m_curLocal = kNoLight;
    bool m_haveLighting = false;
    SceneLight m_lastSun{}, m_lastLocal{};
    mutable float m_keptSun[3] = {0.0f, 1.0f, 0.0f}; // last fallback direction (hysteresis)
    mutable bool m_keptSunValid = false;

    static bool SameLight(const SceneLight& a, const SceneLight& b) {
        if (a.valid != b.valid) return false;
        if (!a.valid) return true;
        return a.point == b.point && a.la == b.la && a.qa == b.qa && std::memcmp(a.vec, b.vec, sizeof(a.vec)) == 0 &&
               std::memcmp(a.color, b.color, sizeof(a.color)) == 0;
    }

    /// Find or add the table row for `l`. Near-identical lights share a row
    /// (an object's own light is re-sent with every draw); when the table is
    /// full the nearest existing light of the same kind stands in.
    uint8_t Intern(const SceneLight& l, bool point) {
        uint8_t best = kNoLight;
        float bestScore = -1e30f;
        for (uint32_t i = 0; i < m_lightCount; ++i) {
            if (m_lightIsPoint[i] != point) continue;
            const LightEntry& e = m_lights[i];
            float score, tol;
            if (point) {
                const float dx = e.a[0] - l.vec[0], dy = e.a[1] - l.vec[1], dz = e.a[2] - l.vec[2];
                score = -(dx * dx + dy * dy + dz * dz);
                tol = -0.25f; // within half a unit and the same colour/attenuation
                if (score >= tol && e.a[3] == l.la && e.b[3] == l.qa &&
                    std::fabs(e.b[0] - l.color[0]) + std::fabs(e.b[1] - l.color[1]) + std::fabs(e.b[2] - l.color[2]) <
                        0.02f)
                    return static_cast<uint8_t>(i);
            } else {
                score = e.a[0] * l.vec[0] + e.a[1] * l.vec[1] + e.a[2] * l.vec[2];
                if (score > 0.9998f) return static_cast<uint8_t>(i); // ~1 degree
            }
            if (score > bestScore) {
                bestScore = score;
                best = static_cast<uint8_t>(i);
            }
        }
        if (m_lightCount < kMaxLights) {
            const uint32_t i = m_lightCount++;
            LightEntry& e = m_lights[i];
            e.a[0] = l.vec[0];
            e.a[1] = l.vec[1];
            e.a[2] = l.vec[2];
            e.a[3] = l.la;
            e.b[0] = l.color[0];
            e.b[1] = l.color[1];
            e.b[2] = l.color[2];
            e.b[3] = l.qa;
            m_lightIsPoint[i] = point;
            return static_cast<uint8_t>(i);
        }
        return best;
    }

    /// The directional entry most triangles used. A near-tie keeps last frame's
    /// choice so unlit surfaces don't flip between two lights.
    uint8_t FallbackSun() const {
        uint8_t best = kNoLight;
        uint32_t bestUse = 0;
        for (uint32_t i = 0; i < m_lightCount; ++i)
            if (!m_lightIsPoint[i] && m_sunUse[i] > bestUse) {
                bestUse = m_sunUse[i];
                best = static_cast<uint8_t>(i);
            }
        if (best == kNoLight) return kNoLight;
        if (m_keptSunValid) {
            for (uint32_t i = 0; i < m_lightCount; ++i) {
                if (m_lightIsPoint[i]) continue;
                const float d =
                    m_lights[i].a[0] * m_keptSun[0] + m_lights[i].a[1] * m_keptSun[1] + m_lights[i].a[2] * m_keptSun[2];
                if (d > 0.9998f && m_sunUse[i] * 10 >= bestUse * 6) {
                    best = static_cast<uint8_t>(i);
                    break;
                }
            }
        }
        for (int k = 0; k < 3; ++k) m_keptSun[k] = m_lights[best].a[k];
        m_keptSunValid = true;
        return best;
    }

    // Acceleration structures rebuilt each frame.
    // BLASes are grouped by materialHash to minimize AS count.
    std::vector<std::unique_ptr<Nova64::Video::VK::VKBlas>> m_blases;
    std::unique_ptr<Nova64::Video::VK::VKTlas> m_tlas;

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

inline bool Nova64::Video::RT::RTSceneManager::BuildAS(const Nova64::Video::VK::VKContext& ctx, VkCommandBuffer cmd) {
    using namespace Nova64::Video::VK;

    m_blases.clear();
    m_tlas.reset();

    const auto& tris = m_frame.triangles;
    if (tris.empty()) return true; // nothing to build

    // ── Group triangles by materialHash ─────────────────────────────────
    std::unordered_map<uint64_t, std::vector<const SceneTriangle*>> groups;
    for (const auto& t : tris) groups[t.materialHash].push_back(&t);

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
        inst.blasDeviceAddress = blas->GetDeviceAddress();
        inst.instanceCustomIndex = customIndex & 0xFFFFFF;
        inst.mask = 0xFF;
        inst.hitGroupIndex = 0;
        // Row 0: [1 0 0 0]
        inst.transform[0] = 1.0f;
        inst.transform[1] = 0.0f;
        inst.transform[2] = 0.0f;
        inst.transform[3] = 0.0f;
        // Row 1: [0 1 0 0]
        inst.transform[4] = 0.0f;
        inst.transform[5] = 1.0f;
        inst.transform[6] = 0.0f;
        inst.transform[7] = 0.0f;
        // Row 2: [0 0 1 0]
        inst.transform[8] = 0.0f;
        inst.transform[9] = 0.0f;
        inst.transform[10] = 1.0f;
        inst.transform[11] = 0.0f;
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
