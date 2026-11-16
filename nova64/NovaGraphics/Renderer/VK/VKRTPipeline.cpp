/**
 * @file VKRTPipeline.cpp
 * @brief VKRTPipeline — build and dispatch the N64 RT pipeline.
 */
// SPDX-License-Identifier: MIT

#include "VKRTPipeline.hpp"

#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace Nova64 {
namespace Video {
namespace VK {

// --- Build ---

bool VKRTPipeline::Build(const VKContext& ctx, const char* rgenSrc, const char* rmissSrc, const char* rchitSrc) {
    if (!ctx.device || !rgenSrc || !rmissSrc || !rchitSrc) return false;

    Destroy();
    m_device = ctx.device;

    GlslangInitProcess();

    // --- 1. Compile shaders ---
    auto rgenSpv = CompileGLSLtoSPIRV(ShaderStage::RayGen, rgenSrc, "n64rt.rgen");
    auto rmissSpv = CompileGLSLtoSPIRV(ShaderStage::RayMiss, rmissSrc, "n64rt.rmiss");
    auto rchitSpv = CompileGLSLtoSPIRV(ShaderStage::RayClosestHit, rchitSrc, "n64rt.rchit");

    if (rgenSpv.empty() || rmissSpv.empty() || rchitSpv.empty()) {
        Logger::Error("VKRTPipeline: shader compilation failed");
        GlslangFinalizeProcess();
        return false;
    }

    // --- 2. Create VkShaderModules ---
    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx.device, &ci, nullptr, &mod) != VK_SUCCESS) {
            Logger::Error("VKRTPipeline: vkCreateShaderModule failed");
            mod = VK_NULL_HANDLE;
        }
        return mod;
    };

    VkShaderModule rgenMod = makeModule(rgenSpv);
    VkShaderModule rmissMod = makeModule(rmissSpv);
    VkShaderModule rchitMod = makeModule(rchitSpv);

    if (!rgenMod || !rmissMod || !rchitMod) {
        if (rgenMod) vkDestroyShaderModule(ctx.device, rgenMod, nullptr);
        if (rmissMod) vkDestroyShaderModule(ctx.device, rmissMod, nullptr);
        if (rchitMod) vkDestroyShaderModule(ctx.device, rchitMod, nullptr);
        GlslangFinalizeProcess();
        return false;
    }

    // --- 3. Create descriptor set layout ---
    // Binding 0: acceleration structure (TLAS)
    // Binding 1: storage image (output)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = static_cast<uint32_t>(bindings.size());
    dsli.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(ctx.device, &dsli, nullptr, &m_dsLayout) != VK_SUCCESS) {
        Logger::Error("VKRTPipeline: vkCreateDescriptorSetLayout failed");
        goto cleanup;
    }

    // --- 4. Pipeline layout ---
    {
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &m_dsLayout;
        if (vkCreatePipelineLayout(ctx.device, &pli, nullptr, &m_layout) != VK_SUCCESS) {
            Logger::Error("VKRTPipeline: vkCreatePipelineLayout failed");
            goto cleanup;
        }
    }

    // --- 5. Shader groups ---
    // Group 0: RAYGEN
    // Group 1: MISS
    // Group 2: HIT (closesthit)
    {
        std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
        stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        stages[0].module = rgenMod;
        stages[0].pName = "main";
        stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        stages[1].module = rmissMod;
        stages[1].pName = "main";
        stages[2] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        stages[2].module = rchitMod;
        stages[2].pName = "main";

        std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{};
        // Group 0: raygen (general)
        groups[0] = VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
        // Group 1: miss (general)
        groups[1] = groups[0];
        groups[1].generalShader = 1;
        // Group 2: closest-hit (triangles)
        groups[2] = VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[2].generalShader = VK_SHADER_UNUSED_KHR;
        groups[2].closestHitShader = 2;
        groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR rci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        rci.stageCount = static_cast<uint32_t>(stages.size());
        rci.pStages = stages.data();
        rci.groupCount = static_cast<uint32_t>(groups.size());
        rci.pGroups = groups.data();
        rci.maxPipelineRayRecursionDepth = 1;
        rci.layout = m_layout;

        VkResult r = vkCreateRayTracingPipelinesKHR(ctx.device, VK_NULL_HANDLE, ctx.pipelineCache, 1, &rci, nullptr,
                                                    &m_pipeline);
        if (r != VK_SUCCESS) {
            Logger::Error("VKRTPipeline: vkCreateRayTracingPipelinesKHR failed: {}", VkResultToString(r));
            goto cleanup;
        }

        // --- 6. Build SBT ---
        if (!BuildSBT(ctx, static_cast<uint32_t>(groups.size()))) {
            Logger::Error("VKRTPipeline: SBT build failed");
            goto cleanup;
        }
    }

    // --- 7. Descriptor pool + set ---
    {
        std::array<VkDescriptorPoolSize, 2> ps{};
        ps[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        ps[0].descriptorCount = 1;
        ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ps[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = static_cast<uint32_t>(ps.size());
        dpi.pPoolSizes = ps.data();
        if (vkCreateDescriptorPool(ctx.device, &dpi, nullptr, &m_descPool) != VK_SUCCESS) {
            Logger::Error("VKRTPipeline: vkCreateDescriptorPool failed");
            goto cleanup;
        }
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = m_descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_dsLayout;
        if (vkAllocateDescriptorSets(ctx.device, &dsai, &m_descSet) != VK_SUCCESS) {
            Logger::Error("VKRTPipeline: vkAllocateDescriptorSets failed");
            goto cleanup;
        }
    }

    // The shader modules are not needed once the pipeline exists.
    vkDestroyShaderModule(ctx.device, rgenMod, nullptr);
    vkDestroyShaderModule(ctx.device, rmissMod, nullptr);
    vkDestroyShaderModule(ctx.device, rchitMod, nullptr);
    GlslangFinalizeProcess();
    Logger::Info("VKRTPipeline: RT pipeline built successfully");
    return true;

cleanup:
    vkDestroyShaderModule(ctx.device, rgenMod, nullptr);
    vkDestroyShaderModule(ctx.device, rmissMod, nullptr);
    vkDestroyShaderModule(ctx.device, rchitMod, nullptr);
    GlslangFinalizeProcess();
    Destroy();
    return false;
}

// --- BuildSBT ---

bool VKRTPipeline::BuildSBT(const VKContext& ctx, uint32_t groupCount) {
    // Query RT pipeline properties for handle alignment.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(ctx.physical, &props2);

    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t handleAlign = rtProps.shaderGroupHandleAlignment;
    // Each record is rounded up to handleAlign.
    const uint32_t stride = (handleSize + handleAlign - 1) & ~(handleAlign - 1);
    // SBT layout: [rgen][miss][hit]  (one record each)
    const uint32_t sbtSize = stride * groupCount;

    // Retrieve handles from the pipeline.
    std::vector<uint8_t> handles(handleSize * groupCount);
    if (vkGetRayTracingShaderGroupHandlesKHR(ctx.device, m_pipeline, 0, groupCount, handles.size(), handles.data()) !=
        VK_SUCCESS) {
        Logger::Error("VKRTPipeline: vkGetRayTracingShaderGroupHandlesKHR failed");
        return false;
    }

    // Pack into SBT buffer with proper alignment.
    std::vector<uint8_t> sbtData(sbtSize, 0);
    for (uint32_t g = 0; g < groupCount; ++g) {
        std::memcpy(sbtData.data() + g * stride, handles.data() + g * handleSize, handleSize);
    }

    // Upload through a host-visible, host-coherent buffer.
    if (!CreateBufferStorage(ctx, sbtSize,
                             VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m_sbt)) {
        Logger::Error("VKRTPipeline: SBT buffer alloc failed");
        return false;
    }
    if (m_sbt.mapped) {
        std::memcpy(m_sbt.mapped, sbtData.data(), sbtSize);
    } else {
        // Map, copy, unmap.
        void* mapped = nullptr;
        vkMapMemory(ctx.device, m_sbt.memory, 0, sbtSize, 0, &mapped);
        std::memcpy(mapped, sbtData.data(), sbtSize);
        vkUnmapMemory(ctx.device, m_sbt.memory);
    }

    // Fill region descriptors.
    VkBufferDeviceAddressInfo bdai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bdai.buffer = m_sbt.buffer;
    const VkDeviceAddress base = vkGetBufferDeviceAddress(ctx.device, &bdai);

    m_rgenRegion.deviceAddress = base + 0 * stride;
    m_rgenRegion.stride = stride;
    m_rgenRegion.size = stride;

    m_missRegion.deviceAddress = base + 1 * stride;
    m_missRegion.stride = stride;
    m_missRegion.size = stride;

    m_hitRegion.deviceAddress = base + 2 * stride;
    m_hitRegion.stride = stride;
    m_hitRegion.size = stride;

    m_callRegion = {}; // no callable shaders

    return true;
}

// --- Dispatch ---

void VKRTPipeline::Dispatch(VkCommandBuffer cmd, void* tlas, void* outputView, uint32_t w, uint32_t h) {
    if (!m_pipeline || !tlas || !outputView || w == 0 || h == 0) return;

    auto tlasHandle = static_cast<VkAccelerationStructureKHR>(tlas);
    auto outputViewVK = static_cast<VkImageView>(outputView);

    // Rewrite the descriptor set only if the inputs changed.
    if (tlasHandle != m_boundTlas || outputViewVK != m_boundView) {
        m_boundTlas = tlasHandle;
        m_boundView = outputViewVK;

        // Binding 0: TLAS
        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        tlasWrite.accelerationStructureCount = 1;
        tlasWrite.pAccelerationStructures = &m_boundTlas;

        VkWriteDescriptorSet wr0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr0.dstSet = m_descSet;
        wr0.dstBinding = 0;
        wr0.descriptorCount = 1;
        wr0.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        wr0.pNext = &tlasWrite;

        // Binding 1: output storage image
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView = m_boundView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet wr1{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr1.dstSet = m_descSet;
        wr1.dstBinding = 1;
        wr1.descriptorCount = 1;
        wr1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr1.pImageInfo = &imgInfo;

        std::array<VkWriteDescriptorSet, 2> writes = {wr0, wr1};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_layout, 0, 1, &m_descSet, 0, nullptr);
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion, w, h, 1);
}

// --- Destroy ---

void VKRTPipeline::Destroy() {
    if (!m_device) return;
    if (m_descPool) {
        vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
        m_descSet = VK_NULL_HANDLE;
    }
    if (m_sbt.buffer) {
        vkDestroyBuffer(m_device, m_sbt.buffer, nullptr);
        m_sbt.buffer = VK_NULL_HANDLE;
    }
    if (m_sbt.memory) {
        vkFreeMemory(m_device, m_sbt.memory, nullptr);
        m_sbt.memory = VK_NULL_HANDLE;
    }
    if (m_pipeline) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout) {
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    if (m_dsLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_dsLayout, nullptr);
        m_dsLayout = VK_NULL_HANDLE;
    }
    m_boundTlas = VK_NULL_HANDLE;
    m_boundView = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

} // namespace VK
} // namespace Video
} // namespace Nova64
