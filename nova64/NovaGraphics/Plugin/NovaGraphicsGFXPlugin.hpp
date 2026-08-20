/**
 * @file NovaGraphicsGFXPlugin.hpp
 * @brief Adapter bridging NovaStar64's GFXPlugin interface to NovaGraphics's VideoPlugin
 *
 * This class implements the Plugins::GFXPlugin abstract interface required by
 * the emulator core, and delegates all calls to the internal Nova64::Video::VideoPlugin
 * which owns the rendering pipeline (RDPProcessor, MicrocodeDispatcher, GLRenderer, etc.).
 */

#pragma once

#include "../../N64SYS/Plugins/GFXPlugin.hpp"
#include "VideoPlugin.hpp"

#include <memory>
#include <chrono>

namespace Nova64::Video {

class NovaGraphicsGFXPlugin final : public ::Plugins::GFXPlugin {
public:
    NovaGraphicsGFXPlugin();
    ~NovaGraphicsGFXPlugin() override;

    // ════════════════════════════════════════════════════════════
    //  PluginBase interface
    // ════════════════════════════════════════════════════════════

    ::Plugins::PluginInfo GetPluginInfo() const override;
    bool Initialize(CN64System* system) override;
    void Shutdown() override;
    void Reset(bool coldReset) override;
    void Pause() override;
    void Resume() override;
    bool IsInitialized() const override;
    void ShowConfigDialog(void* hwndParent) override;
    size_t SaveState(uint8_t* data, size_t size) override;
    bool LoadState(const uint8_t* data, size_t size) override;

    // ════════════════════════════════════════════════════════════
    //  GFXPlugin interface
    // ════════════════════════════════════════════════════════════

    bool InitializeDisplay(void* hwnd, const ::Plugins::RenderSettings& settings) override;
    void ResizeDisplay(uint32_t width, uint32_t height) override;
    void ProcessDList(uint32_t start, uint32_t end) override;
    void ProcessDListShadow(uint32_t start, uint32_t end) override;
    void ProcessRDPCommand(uint64_t cmd) override;
    void UpdateRDPStatus() override;
    void UpdateScreen() override;
    void SwapBuffers() override;
    ::Plugins::FrameBufferInfo GetFrameBufferInfo() const override;
    void ReadFrameBuffer(uint8_t* dest, uint32_t addr, uint32_t width,
                         uint32_t height, uint32_t size) override;
    void WriteFrameBuffer(const uint8_t* src, uint32_t addr, uint32_t width,
                          uint32_t height, uint32_t size) override;
    bool IsFrameReady() const override;
    void GetStatistics(float& fps, float& vis) const override;
    bool CaptureScreenshot(const std::string& filename) override;
    double GetLastSwapTimeMs() const override;

private:
    VideoPlugin                            m_videoPlugin;
    bool                                   m_initialized = false;

    // Frame timing
    uint64_t                               m_viCount = 0;
    std::chrono::steady_clock::time_point  m_lastStatTime;
    float                                  m_fps = 0.0f;
    float                                  m_vis = 0.0f;
};

/// Factory function — called by PluginManager::CreateDefaultGraphicsPlugin
std::unique_ptr<::Plugins::GFXPlugin> CreateNovaGraphicsPlugin();

} // namespace Nova64::Video
