/**
 * @file FrameCallback.hpp
 * @brief Hand-off between the video plugin and the launcher for a game drawn
 *        inside the launcher window.
 *
 * The video plugin renders without a visible window (Vulkan offscreen, or a
 * hidden OpenGL window) and copies each finished frame into g_framePixels,
 * which the launcher uploads to its own texture.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Nova64::Video {

// Frame pixels: RGBA8, top row first. Guarded by g_frameMutex.
extern std::vector<uint8_t> g_framePixels;
extern std::atomic<int> g_frameW;
extern std::atomic<int> g_frameH;
extern std::mutex g_frameMutex;
extern std::atomic<bool> g_frameReady;

/// The renderer the most recent game used, e.g. "Vulkan 1.1 on Apple M4 Pro".
/// Guarded by g_frameMutex.
extern std::string g_rendererDescription;

/// Called on the emulator thread after each frame (drives Lua scripts).
using FrameReadyFn = std::function<void()>;
extern FrameReadyFn g_frameCallback;

/// Set by the launcher before a game starts: the graphics settings file to read instead of
/// settings/NovaGraphics.ini, when a game has its own setup. Empty means the default file.
extern std::string g_configPath;

/// Set by the launcher: the game is drawn inside it, not in a window of its own.
extern std::atomic<bool> g_embeddedMode;
/// Set by the launcher: the game is paused. The CPU waits at an instruction
/// boundary (StateService::Service), where save states can still be made.
extern std::atomic<bool> g_emuPaused;

} // namespace Nova64::Video
