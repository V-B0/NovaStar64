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

namespace GL {}
namespace VK {
bool IsVulkanAvailable();
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

bool IsDepthFormat(TextureFormat format) {
    return format == TextureFormat::Depth16 || format == TextureFormat::Depth24 || format == TextureFormat::Depth32F ||
           format == TextureFormat::Depth24Stencil8;
}

bool HasStencil(TextureFormat format) { return format == TextureFormat::Depth24Stencil8; }

// --- BLEND STATE HELPERS (for N64 RDP blender modes) ---

// --- DEPTH STATE HELPERS ---

} // namespace Video
} // namespace Nova64
