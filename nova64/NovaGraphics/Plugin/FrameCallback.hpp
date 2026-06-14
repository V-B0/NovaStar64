/**
 * @file FrameCallback.hpp
 * @brief VI frame completion callback for embedded launcher rendering.
 *
 * In embedded mode the launcher passes its own WindowManager pointer via
 * g_launcherWindowManager BEFORE starting the emulator thread.
 * VideoPlugin then calls SetExternalWindow() with it, which prevents any
 * second SDL2 window from ever being created.
 *
 * Frame data is delivered as CPU pixel readback (glReadPixels) into
 * g_framePixels so the launcher can upload them to its own ImGui texture.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <atomic>
#include <vector>
#include <mutex>

// Forward declare WindowManager to avoid pulling in SDL headers here
namespace Nova64::Video { class WindowManager; }

namespace Nova64::Video {

// ── Launcher window injection ─────────────────────────────────────────────────
// Set by the launcher to its own WindowManager BEFORE calling LaunchEmbedded.
// VideoPlugin::Initialize reads this and calls SetExternalWindow() so no
// second SDL2 window is ever created or shown.
extern WindowManager* g_launcherWindowManager;

// ── CPU pixel readback buffer (RGBA8) ────────────────────────────────────────
extern std::vector<uint8_t>  g_framePixels;
extern std::atomic<int>      g_frameW;
extern std::atomic<int>      g_frameH;
extern std::mutex            g_frameMutex;
extern std::atomic<bool>     g_frameReady;

// Legacy callback (used for Lua frame tick) — void() signature
using FrameReadyFn = std::function<void()>;
extern FrameReadyFn g_frameCallback;

// ── Emulator control atomics ──────────────────────────────────────────────────
extern std::atomic<bool>  g_emuPaused;
extern std::atomic<float> g_speedMultiplier;
extern std::atomic<bool>  g_rewindActive;
extern std::atomic<bool>  g_embeddedMode;

} // namespace Nova64::Video
