/**
 * @file VKRTPipeline.hpp
 * @brief Vulkan ray tracing pipeline (KHR) + Shader Binding Table.
 *
 * Encapsulates:
 *   - VkPipeline built via vkCreateRayTracingPipelinesKHR
 *   - SBT (Shader Binding Table) buffer with rgen / miss / hit groups
 *   - Descriptor set layout with TLAS (binding 0) + output image (binding 1)
 *   - Dispatch helper: vkCmdTraceRaysKHR
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "VKBuffer.hpp"
#include "VKShader.hpp"

#include <cstdint>

namespace Nova64 { namespace Video { namespace VK {

class VKRTPipeline {
public:
    VKRTPipeline() = default;
    ~VKRTPipeline() { Destroy(); }

    VKRTPipeline(const VKRTPipeline&)            = delete;
    VKRTPipeline& operator=(const VKRTPipeline&) = delete;

    /**
     * Compile the three RT shader stages and create the pipeline.
     *
     * @param ctx        VKContext (device, physical, cache).
     * @param rgenSrc    Ray generation GLSL source.
     * @param rmissSrc   Miss shader GLSL source.
     * @param rchitSrc   Closest-hit shader GLSL source.
     * @return true on success.
     */
    bool Build(const VKContext& ctx,
               const char* rgenSrc,
               const char* rmissSrc,
               const char* rchitSrc);

    /**
     * Record vkCmdTraceRaysKHR into `cmd`.
     *
     * @param cmd         Open command buffer (outside render pass).
     * @param tlas        VkAccelerationStructureKHR handle (as void*).
     * @param outputView  VkImageView for the storage image output (as void*).
     * @param w, h        Dispatch dimensions in pixels.
     */
    void Dispatch(VkCommandBuffer cmd,
                  void*           tlas,
                  void*           outputView,
                  uint32_t        w,
                  uint32_t        h);

    void Destroy();

    bool IsBuilt() const { return m_pipeline != VK_NULL_HANDLE; }

private:
    bool BuildSBT(const VKContext& ctx, uint32_t groupCount);

    VkDevice               m_device          = VK_NULL_HANDLE;
    VkPipeline             m_pipeline        = VK_NULL_HANDLE;
    VkPipelineLayout       m_layout          = VK_NULL_HANDLE;
    VkDescriptorSetLayout  m_dsLayout        = VK_NULL_HANDLE;
    VkDescriptorPool       m_descPool        = VK_NULL_HANDLE;
    VkDescriptorSet        m_descSet         = VK_NULL_HANDLE;

    VKBufferStorage        m_sbt{};

    // SBT stride/size values (filled from RT pipeline properties).
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};

    // Cached writes for UpdateDescriptors (called each Dispatch).
    VkAccelerationStructureKHR m_boundTlas    = VK_NULL_HANDLE;
    VkImageView                m_boundView    = VK_NULL_HANDLE;
    VkSampler                  m_dummySampler = VK_NULL_HANDLE;
};

}}} // namespace Nova64::Video::VK
