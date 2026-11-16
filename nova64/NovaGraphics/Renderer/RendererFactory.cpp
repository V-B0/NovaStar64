/**
 * @file RendererFactory.cpp
 * @brief Backend selection and renderer creation, plus format and N64 blend/depth translation helpers.
 */
// SPDX-License-Identifier: MIT

#include "Renderer.hpp"
#include "../Common/Logger.hpp"

#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Nova64 {
namespace Video {

// --- BACKEND NAME TABLE ---

static const char* const kBackendNames[] = {"None", "OpenGL 4.6", "Software", "Vulkan 1.1"};

const char* GetBackendName(BackendType type) {
    int idx = static_cast<int>(type);
    if (idx < 0 || idx >= static_cast<int>(BackendType::Count)) return "Unknown";
    return kBackendNames[idx];
}

// --- BACKEND DETECTION ---

namespace GL {
bool IsOpenGL46Available();
}
namespace VK {
bool IsVulkanAvailable();
}

BackendType DetectBestBackend() {
    // Prefer Vulkan when available (modern, lower driver overhead).
    if (VK::IsVulkanAvailable()) return BackendType::Vulkan;
    return BackendType::OpenGL46;
}

// --- RENDERER FACTORY ---

namespace GL {
std::unique_ptr<IRenderer> CreateOpenGLRenderer();
}
namespace VK {
std::unique_ptr<IRenderer> CreateVulkanRenderer();
}

std::unique_ptr<IRenderer> CreateRenderer(BackendType type) {
    switch (type) {
    case BackendType::OpenGL46: return GL::CreateOpenGLRenderer();

    case BackendType::Vulkan: return VK::CreateVulkanRenderer();

    case BackendType::Software: return nullptr; // No software renderer exists

    default: return nullptr;
    }
}

// --- VERTEX LAYOUT UTILITIES ---

uint32_t GetVertexAttribSize(VertexAttribType type) {
    switch (type) {
    case VertexAttribType::Float1:  return 4;
    case VertexAttribType::Float2:  return 8;
    case VertexAttribType::Float3:  return 12;
    case VertexAttribType::Float4:  return 16;
    case VertexAttribType::UByte4:  return 4;
    case VertexAttribType::UByte4N: return 4;
    case VertexAttribType::Short2:  return 4;
    case VertexAttribType::Short4:  return 8;
    case VertexAttribType::Short2N: return 4;
    case VertexAttribType::Short4N: return 8;
    case VertexAttribType::UInt1:   return 4;
    case VertexAttribType::Int1:    return 4;
    default:                        return 0;
    }
}

uint32_t GetVertexAttribComponentCount(VertexAttribType type) {
    switch (type) {
    case VertexAttribType::Float1:  return 1;
    case VertexAttribType::Float2:  return 2;
    case VertexAttribType::Float3:  return 3;
    case VertexAttribType::Float4:  return 4;
    case VertexAttribType::UByte4:  return 4;
    case VertexAttribType::UByte4N: return 4;
    case VertexAttribType::Short2:  return 2;
    case VertexAttribType::Short4:  return 4;
    case VertexAttribType::Short2N: return 2;
    case VertexAttribType::Short4N: return 4;
    case VertexAttribType::UInt1:   return 1;
    case VertexAttribType::Int1:    return 1;
    default:                        return 0;
    }
}

// --- TEXTURE FORMAT UTILITIES ---

uint32_t GetTexelSizeBytes(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:           return 4;
    case TextureFormat::RGB8:            return 3;
    case TextureFormat::RGBA4:           return 2;
    case TextureFormat::RGB5A1:          return 2;
    case TextureFormat::R8:              return 1;
    case TextureFormat::RG8:             return 2;
    case TextureFormat::RGBA16F:         return 8;
    case TextureFormat::RGBA32F:         return 16;
    case TextureFormat::Depth16:         return 2;
    case TextureFormat::Depth24:         return 3;
    case TextureFormat::Depth32F:        return 4;
    case TextureFormat::Depth24Stencil8: return 4;
    case TextureFormat::IA16:            return 2;
    case TextureFormat::I8:              return 1;
    case TextureFormat::CI4:             return 1; // 2 pixels per byte
    case TextureFormat::CI8:             return 1;
    default:                             return 0;
    }
}

bool IsDepthFormat(TextureFormat format) {
    return format == TextureFormat::Depth16 || format == TextureFormat::Depth24 || format == TextureFormat::Depth32F ||
           format == TextureFormat::Depth24Stencil8;
}

bool HasStencil(TextureFormat format) { return format == TextureFormat::Depth24Stencil8; }

bool IsN64Format(TextureFormat format) {
    return format == TextureFormat::IA16 || format == TextureFormat::I8 || format == TextureFormat::CI4 ||
           format == TextureFormat::CI8;
}

// --- BLEND STATE HELPERS (for N64 RDP blender modes) ---

BlendState TranslateN64BlendMode(uint32_t blenderBits, uint32_t otherModeBits) {
    BlendState state;

    // N64 otherMode low bits: cycle type and blender settings
    bool forceBlend = (otherModeBits & 0x4000) != 0; // FORCE_BL bit

    if (!forceBlend) {
        state.enabled = false;
        return state;
    }

    state.enabled = true;

    // Decode N64 blender: P * A + M * B
    // Where P,M are color sources and A,B are alpha sources
    uint32_t p1 = (blenderBits >> 30) & 0x3; // P source, cycle 1
    uint32_t a1 = (blenderBits >> 26) & 0x3; // A source, cycle 1
    uint32_t m1 = (blenderBits >> 22) & 0x3; // M source, cycle 1
    uint32_t b1 = (blenderBits >> 18) & 0x3; // B source, cycle 1

    // Common N64 blend modes → OpenGL blend factors
    // P=G_BL_CLR_IN, A=G_BL_A_IN, M=G_BL_CLR_MEM, B=G_BL_1MA (standard alpha blend)
    if (p1 == 0 && a1 == 0 && m1 == 1 && b1 == 1) {
        // Standard: SrcColor * SrcAlpha + DstColor * (1-SrcAlpha)
        state.srcColor = BlendFactor::SrcAlpha;
        state.dstColor = BlendFactor::OneMinusSrcAlpha;
        state.srcAlpha = BlendFactor::One;
        state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
    }
    // P=G_BL_CLR_IN, A=G_BL_A_FOG, M=G_BL_CLR_MEM, B=G_BL_1MA (fog blend)
    else if (p1 == 0 && a1 == 1 && m1 == 1 && b1 == 1) {
        state.srcColor = BlendFactor::SrcAlpha;
        state.dstColor = BlendFactor::OneMinusSrcAlpha;
    }
    // P=G_BL_CLR_FOG, A=G_BL_A_SHADE, M=G_BL_CLR_IN, B=G_BL_1MA (decal mode)
    else if (p1 == 1 && a1 == 2 && m1 == 0 && b1 == 1) {
        state.srcColor = BlendFactor::SrcAlpha;
        state.dstColor = BlendFactor::OneMinusSrcAlpha;
    }
    // P=G_BL_CLR_IN, A=G_BL_A_IN, M=G_BL_CLR_MEM, B=G_BL_A_MEM (multiply)
    else if (p1 == 0 && a1 == 0 && m1 == 1 && b1 == 0) {
        state.srcColor = BlendFactor::SrcAlpha;
        state.dstColor = BlendFactor::DstAlpha;
    }
    // Additive
    else if (p1 == 0 && a1 == 0 && m1 == 1 && b1 == 2) {
        state.srcColor = BlendFactor::SrcAlpha;
        state.dstColor = BlendFactor::One;
    }
    // Any other combination: plain alpha blend
    else {
        state.srcColor = BlendFactor::SrcAlpha;
        state.dstColor = BlendFactor::OneMinusSrcAlpha;
        state.srcAlpha = BlendFactor::One;
        state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
    }

    return state;
}

// --- DEPTH STATE HELPERS ---

DepthStencilState TranslateN64DepthMode(uint32_t otherModeBits) {
    DepthStencilState state;

    bool zCompare = (otherModeBits & 0x10) != 0; // Z_CMP bit
    bool zUpdate = (otherModeBits & 0x20) != 0;  // Z_UPD bit

    state.depthTestEnabled = zCompare;
    state.depthWriteEnabled = zUpdate;

    // N64 uses less-equal depth by default
    state.depthFunc = CompareFunc::LessEqual;

    return state;
}

} // namespace Video
} // namespace Nova64
