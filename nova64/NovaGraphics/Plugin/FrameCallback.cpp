/**
 * @file FrameCallback.cpp
 * @brief FrameCallback globals — CPU pixel buffer for embedded rendering.
 */
#include "FrameCallback.hpp"
#include "../Window/WindowManager.hpp"

namespace Nova64::Video {

WindowManager*        g_launcherWindowManager { nullptr };

std::vector<uint8_t>  g_framePixels;
std::atomic<int>      g_frameW   { 640 };
std::atomic<int>      g_frameH   { 480 };
std::mutex            g_frameMutex;
std::atomic<bool>     g_frameReady  { false };

FrameReadyFn          g_frameCallback { nullptr };

std::atomic<bool>     g_emuPaused       { false };
std::atomic<float>    g_speedMultiplier { 1.0f  };
std::atomic<bool>     g_rewindActive    { false };
std::atomic<bool>     g_embeddedMode    { false };

} // namespace Nova64::Video
