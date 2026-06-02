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
    }

    void SubmitTriangles(const SceneTriangle* tris, uint32_t count) override {
        m_frame.triangles.insert(m_frame.triangles.end(), tris, tris + count);
    }

    // ─── Accessors ───────────────────────────────────────────────────

    /// Returns the number of world-space triangles accumulated this frame.
    uint32_t GetTriangleCount() const { return m_frame.TriangleCount(); }

    /// Direct access to the accumulated frame data (for CPU-side debug/export).
    const SceneFrameData& GetFrameData() const { return m_frame; }

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
