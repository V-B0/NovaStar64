/**
 * @file VKPipeline.hpp
 * @brief Real Vulkan IPipeline — VkPipelineLayout + VkPipeline (graphics).
 *
 * Stage 3 scope:
 *   * Builds a graphics pipeline matching the engine PipelineDesc:
 *     - vertex layout from PipelineDesc::vertexLayout
 *     - blend / depth-stencil / rasterizer from PipelineDesc
 *     - primitive topology
 *     - dynamic viewport + scissor (set at draw time)
 *     - default render pass = the swapchain's render pass passed at create
 *   * Pipeline layout currently has zero descriptor sets (Stage 4 will add
 *     a UBO + sampled image set). Push constants are not used.
 */
// SPDX-License-Identifier: MIT
#pragma once

#include "VKCommon.hpp"
#include "VKMemory.hpp"
#include "../Renderer.hpp"

namespace Nova64 { namespace Video { namespace VK {

// Engine-wide fixed descriptor set layout (set 0):
//   binding 0  : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER          (vertex+fragment)
//   binding 1-4: VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER  (fragment)
constexpr uint32_t kVKMaxTexturesPerDraw = 11; // UBO=binding0, textures at 1..11

/// Lazily build (per-device) the shared descriptor set layout. Owned by caller
/// (VulkanRenderer); destroyed via vkDestroyDescriptorSetLayout.
VkDescriptorSetLayout CreateStandardSetLayout(VkDevice device);

class VKPipelineImpl final : public IPipeline {
public:
    /// Build the graphics pipeline. `compatibleRenderPass` is the render pass
    /// the pipeline will be used inside (e.g. the swapchain's). `setLayout`
    /// is the standard layout from CreateStandardSetLayout. Returns false on
    /// failure; check Logger for details.
    bool Build(const VKContext& ctx,
               const PipelineDesc& desc,
               VkRenderPass compatibleRenderPass,
               VkDescriptorSetLayout setLayout);

    /// Build a compute pipeline from `cs` (a compiled VkShaderModule with a
    /// single entry point `main`). Uses the same standard descriptor set layout.
    bool BuildCompute(const VKContext& ctx,
                      VkShaderModule cs,
                      VkDescriptorSetLayout setLayout);
    void Destroy(const VKContext& ctx);

    bool IsCompute() const { return m_isCompute; }

    // IPipeline
    void Bind() override {} // bound by VKBackend during draw recording
    uint64_t GetNativeHandle() const override {
        return reinterpret_cast<uint64_t>(m_pipeline);
    }

    VkPipeline       Get()       const { return m_pipeline; }
    VkPipelineLayout GetLayout() const { return m_layout; }

private:
    VkPipeline       m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout   = VK_NULL_HANDLE;
    bool             m_isCompute = false;
};

}}} // namespace Nova64::Video::VK
