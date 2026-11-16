/**
 * @file VKBackend.hpp
 * @brief Vulkan 1.2 backend: IRenderer implementation with swapchain, render pass, per-frame descriptor pools,
 *        dynamic pipeline cache, UBO-backed uniforms, combined image samplers (11 texture units) and SPIR-V shaders.
 */
// SPDX-License-Identifier: MIT

#pragma once

#include "../Renderer.hpp"

#include <memory>

namespace Nova64 {
namespace Video {
namespace VK {

/// Returns true if a usable Vulkan 1.2+ loader and physical device are
/// present on the system. Probes the loader (volkInitialize +
/// vkEnumerateInstanceVersion); see VKBackend.cpp.
bool IsVulkanAvailable();

/// Create a Vulkan IRenderer instance. Even when Vulkan is unavailable
/// this returns a non-null object whose Initialize() will fail cleanly,
/// so the factory can report a useful error.
std::unique_ptr<IRenderer> CreateVulkanRenderer();

} // namespace VK
} // namespace Video
} // namespace Nova64
