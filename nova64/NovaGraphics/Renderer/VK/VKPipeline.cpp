/**
 * @file VKPipeline.cpp
 */
// SPDX-License-Identifier: MIT
#include "VKPipeline.hpp"
#include "VKShader.hpp"

#include <array>
#include <cstring>
#include <vector>

namespace Nova64 { namespace Video { namespace VK {

namespace {

VkFormat AttribToVkFormat(VertexAttribType t) {
    switch (t) {
        case VertexAttribType::Float1:  return VK_FORMAT_R32_SFLOAT;
        case VertexAttribType::Float2:  return VK_FORMAT_R32G32_SFLOAT;
        case VertexAttribType::Float3:  return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexAttribType::Float4:  return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexAttribType::UByte4:  return VK_FORMAT_R8G8B8A8_UINT;
        case VertexAttribType::UByte4N: return VK_FORMAT_R8G8B8A8_UNORM;
        case VertexAttribType::Short2:  return VK_FORMAT_R16G16_SINT;
        case VertexAttribType::Short4:  return VK_FORMAT_R16G16B16A16_SINT;
        case VertexAttribType::Short2N: return VK_FORMAT_R16G16_SNORM;
        case VertexAttribType::Short4N: return VK_FORMAT_R16G16B16A16_SNORM;
        case VertexAttribType::UInt1:   return VK_FORMAT_R32_UINT;
        case VertexAttribType::Int1:    return VK_FORMAT_R32_SINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkPrimitiveTopology ToVkTopology(PrimitiveType p) {
    switch (p) {
        case PrimitiveType::Triangles:     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveType::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveType::TriangleFan:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case PrimitiveType::Lines:         return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveType::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveType::Points:        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkBlendFactor ToVkBlendFactor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:                  return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:                   return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:              return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:              return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor:      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:              return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:              return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColor:         return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::ConstantAlpha:         return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BlendFactor::OneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case BlendFactor::SrcAlphaSaturate:      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case BlendFactor::Src1Color:             return VK_BLEND_FACTOR_SRC1_COLOR;
        case BlendFactor::OneMinusSrc1Color:     return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case BlendFactor::Src1Alpha:             return VK_BLEND_FACTOR_SRC1_ALPHA;
        case BlendFactor::OneMinusSrc1Alpha:     return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToVkBlendOp(BlendOp o) {
    switch (o) {
        case BlendOp::Add:             return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:             return VK_BLEND_OP_MIN;
        case BlendOp::Max:             return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkCompareOp ToVkCompareOp(CompareFunc f) {
    switch (f) {
        case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
        case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
        case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareFunc::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkStencilOp ToVkStencilOp(StencilOp o) {
    switch (o) {
        case StencilOp::Keep:      return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero:      return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace:   return VK_STENCIL_OP_REPLACE;
        case StencilOp::IncrClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOp::DecrClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOp::Invert:    return VK_STENCIL_OP_INVERT;
        case StencilOp::IncrWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOp::DecrWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkCullModeFlags ToVkCullMode(CullMode c) {
    switch (c) {
        case CullMode::None:         return VK_CULL_MODE_NONE;
        case CullMode::Front:        return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:         return VK_CULL_MODE_BACK_BIT;
        case CullMode::FrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
    }
    return VK_CULL_MODE_BACK_BIT;
}

VkPolygonMode ToVkPolygonMode(PolygonMode p) {
    switch (p) {
        case PolygonMode::Fill:      return VK_POLYGON_MODE_FILL;
        case PolygonMode::Wireframe: return VK_POLYGON_MODE_LINE;
        case PolygonMode::Points:    return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

} // anonymous

VkDescriptorSetLayout CreateStandardSetLayout(VkDevice device) {
    std::array<VkDescriptorSetLayoutBinding, 1 + kVKMaxTexturesPerDraw> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 0; i < kVKMaxTexturesPerDraw; ++i) {
        auto& b = bindings[1 + i];
        b.binding         = 1 + i;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &layout) != VK_SUCCESS) {
        Logger::Error("VKPipeline: vkCreateDescriptorSetLayout failed");
        return VK_NULL_HANDLE;
    }
    return layout;
}

bool VKPipelineImpl::Build(const VKContext& ctx,
                           const PipelineDesc& desc,
                           VkRenderPass compatibleRenderPass,
                           VkDescriptorSetLayout setLayout) {
    Destroy(ctx);

    auto* shader = static_cast<VKShaderProgramImpl*>(desc.shader);
    if (!shader || !shader->VS() || !shader->FS()) {
        Logger::Error("VKPipeline: shader missing or not compiled");
        return false;
    }

    // ── Shader stages ───────────────────────────────────────
    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    uint32_t stageCount = 0;
    {
        auto& s = stages[stageCount++];
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        s.module = shader->VS();
        s.pName  = "main";
    }
    {
        auto& s = stages[stageCount++];
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        s.module = shader->FS();
        s.pName  = "main";
    }
    if (shader->HasGeometry()) {
        auto& s = stages[stageCount++];
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = VK_SHADER_STAGE_GEOMETRY_BIT;
        s.module = shader->GS();
        s.pName  = "main";
    }

    // ── Vertex input ────────────────────────────────────────
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = desc.vertexLayout.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs;
    attrs.reserve(desc.vertexLayout.attribs.size());
    for (const auto& a : desc.vertexLayout.attribs) {
        VkVertexInputAttributeDescription va{};
        va.location = a.location;
        va.binding  = 0;
        va.format   = AttribToVkFormat(a.type);
        va.offset   = a.offset;
        attrs.push_back(va);
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = (binding.stride > 0) ? 1 : 0;
    vi.pVertexBindingDescriptions      = (binding.stride > 0) ? &binding : nullptr;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions    = attrs.data();

    // ── Input assembly ──────────────────────────────────────
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = ToVkTopology(desc.primitiveType);

    // ── Viewport / scissor (dynamic) ────────────────────────
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    // ── Rasterizer ──────────────────────────────────────────
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = ToVkPolygonMode(desc.rasterizer.polygonMode);
    rs.cullMode    = ToVkCullMode(desc.rasterizer.cullMode);
    rs.frontFace   = (desc.rasterizer.frontFace == FrontFace::CounterClockwise)
                   ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    rs.depthClampEnable        = desc.rasterizer.depthClipEnabled ? VK_FALSE : VK_TRUE;
    rs.depthBiasEnable         = desc.rasterizer.depthBias != 0.0f ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = desc.rasterizer.depthBias;
    rs.depthBiasSlopeFactor    = desc.rasterizer.depthBiasSlope;

    // ── Multisample ─────────────────────────────────────────
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ── Depth/stencil ───────────────────────────────────────
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable  = desc.depthStencil.depthTestEnabled  ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depthStencil.depthWriteEnabled ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp   = ToVkCompareOp(desc.depthStencil.depthFunc);
    ds.stencilTestEnable = desc.depthStencil.stencilEnabled ? VK_TRUE : VK_FALSE;
    ds.front.failOp      = ToVkStencilOp(desc.depthStencil.stencilFail);
    ds.front.depthFailOp = ToVkStencilOp(desc.depthStencil.depthFail);
    ds.front.passOp      = ToVkStencilOp(desc.depthStencil.stencilPass);
    ds.front.compareOp   = ToVkCompareOp(desc.depthStencil.stencilFunc);
    ds.front.compareMask = desc.depthStencil.stencilReadMask;
    ds.front.writeMask   = desc.depthStencil.stencilWriteMask;
    ds.front.reference   = desc.depthStencil.stencilRef;
    ds.back              = ds.front;

    // ── Color blend ─────────────────────────────────────────
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable         = desc.blend.enabled ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = ToVkBlendFactor(desc.blend.srcColor);
    cba.dstColorBlendFactor = ToVkBlendFactor(desc.blend.dstColor);
    cba.colorBlendOp        = ToVkBlendOp   (desc.blend.colorOp);
    cba.srcAlphaBlendFactor = ToVkBlendFactor(desc.blend.srcAlpha);
    cba.dstAlphaBlendFactor = ToVkBlendFactor(desc.blend.dstAlpha);
    cba.alphaBlendOp        = ToVkBlendOp   (desc.blend.alphaOp);
    cba.colorWriteMask      = static_cast<VkColorComponentFlags>(desc.blend.colorWriteMask & 0x0F);

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;
    std::memcpy(cb.blendConstants, desc.blend.blendColor, sizeof(float) * 4);

    // ── Dynamic state ───────────────────────────────────────
    std::array<VkDynamicState, 3> dyn = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dy.pDynamicStates    = dyn.data();

    // ── Pipeline layout (1 descriptor set: UBO + 4 sampled images) ──
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = setLayout ? 1u : 0u;
    li.pSetLayouts    = setLayout ? &setLayout : nullptr;
    if (vkCreatePipelineLayout(ctx.device, &li, nullptr, &m_layout) != VK_SUCCESS) {
        Logger::Error("VKPipeline: vkCreatePipelineLayout failed");
        return false;
    }

    // ── Graphics pipeline ───────────────────────────────────
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount          = stageCount;
    gpi.pStages             = stages.data();
    gpi.pVertexInputState   = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &dy;
    gpi.layout              = m_layout;
    gpi.renderPass          = compatibleRenderPass;
    gpi.subpass             = 0;

    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &gpi,
                                  nullptr, &m_pipeline) != VK_SUCCESS) {
        Logger::Error("VKPipeline: vkCreateGraphicsPipelines failed");
        vkDestroyPipelineLayout(ctx.device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VKPipelineImpl::Destroy(const VKContext& ctx) {
    if (m_pipeline) {
        vkDestroyPipeline(ctx.device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout) {
        vkDestroyPipelineLayout(ctx.device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

bool VKPipelineImpl::BuildCompute(const VKContext& ctx,
                                  VkShaderModule cs,
                                  VkDescriptorSetLayout setLayout) {
    if (!cs) {
        Logger::Error("VKPipeline: BuildCompute called with null shader module");
        return false;
    }
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = setLayout ? 1u : 0u;
    li.pSetLayouts    = setLayout ? &setLayout : nullptr;
    if (vkCreatePipelineLayout(ctx.device, &li, nullptr, &m_layout) != VK_SUCCESS) {
        Logger::Error("VKPipeline: vkCreatePipelineLayout (compute) failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo ss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ss.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ss.module = cs;
    ss.pName  = "main";

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage  = ss;
    cpi.layout = m_layout;
    if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &cpi, nullptr,
                                 &m_pipeline) != VK_SUCCESS) {
        Logger::Error("VKPipeline: vkCreateComputePipelines failed");
        vkDestroyPipelineLayout(ctx.device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
        return false;
    }
    m_isCompute = true;
    return true;
}

}}} // namespace Nova64::Video::VK
