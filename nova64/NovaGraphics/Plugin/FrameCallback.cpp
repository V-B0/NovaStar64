/**
 * @file FrameCallback.cpp
 * @brief Globals shared by the video plugin and the launcher.
 */
#include "FrameCallback.hpp"

namespace Nova64::Video {

std::vector<uint8_t> g_framePixels;
std::atomic<int> g_frameW{640};
std::atomic<int> g_frameH{480};
std::mutex g_frameMutex;
std::atomic<bool> g_frameReady{false};
std::string g_rendererDescription;

FrameReadyFn g_frameCallback{nullptr};

std::atomic<bool> g_embeddedMode{false};
std::atomic<bool> g_emuPaused{false};

} // namespace Nova64::Video
