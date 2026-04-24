/**
 * @file VKBackend.hpp
 * @brief Vulkan 1.2 Backend (stub).
 */
// SPDX-License-Identifier: MIT
// NovaGraphics — Vulkan Backend (compile-only stub)
//
// This is a scaffolding stub. None of the rendering methods do any work yet;
// they log a one-time warning and return safe defaults so callers do not
// crash. `IsVulkanAvailable()` returns false until a real implementation
// is wired up, so `DetectBestBackend()` continues to pick GL/D3D11.
//
// To begin implementing: pull in the Vulkan SDK (link `Vulkan::Vulkan` from
// `find_package(Vulkan)`), replace the stubs in VKBackend.cpp with real
// vkCreateInstance / vkCreateDevice / swapchain / cmd-buffer code, and
// flip IsVulkanAvailable() to do a real `vkEnumerateInstanceVersion` probe.

#pragma once

#include "../Renderer.hpp"

#include <memory>

namespace Nova64 { namespace Video { namespace VK {

/// Returns true if a usable Vulkan 1.2+ loader and physical device are
/// present on the system. Currently always returns false (stub).
bool IsVulkanAvailable();

/// Create a Vulkan IRenderer instance. Even when Vulkan is unavailable
/// this returns a non-null object whose Initialize() will fail cleanly,
/// so the factory can report a useful error.
std::unique_ptr<IRenderer> CreateVulkanRenderer();

}}} // namespace Nova64::Video::VK
