/**
 * @file VideoPlugin.hpp
 * @brief Video Plugin Interface.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Video Plugin Interface
// Main entry point implementing NovaStar64's GFXPlugin abstract base.
// Handles plugin lifecycle: Initialize → ProcessDList → UpdateScreen → Shutdown.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once

#include "../Common/Types.hpp"
#include "../Core/RDPState.hpp"
#include "../Core/RDPProcessor.hpp"
#include "../Renderer/Renderer.hpp"
#include "../Renderer/ShaderManager.hpp"
#include "../Renderer/PostProcessing.hpp"
#include "../MLE/MLEDirector.hpp"
#include "../RT/RTSceneManager.hpp"
#include "../Texture/TextureCache.hpp"
#include "../Microcode/MicrocodeDispatcher.hpp"
#include "../Window/WindowManager.hpp"
#include "../Compat/GameDatabase.hpp"
#include "PluginConfig.hpp"

#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

namespace Nova64::Video {

// ──────────────────────────────────────────────────────────────────
//  Plugin status
// ──────────────────────────────────────────────────────────────────
enum class PluginStatus { Uninitialized, Initialized, Running, Paused, Error, ShuttingDown };

const char* PluginStatusName(PluginStatus status);

// ──────────────────────────────────────────────────────────────────
//  System info passed from emulator core
// ──────────────────────────────────────────────────────────────────
struct SystemInfo {
    uint8_t* rdram = nullptr;
    uint32_t rdramSize = 0; // 4MB or 8MB
    uint8_t* dmem = nullptr;
    uint8_t* imem = nullptr;
    uint32_t* miIntrReg = nullptr;
    uint32_t* dpcStatusReg = nullptr;
    uint32_t* dpcStartReg = nullptr;
    uint32_t* dpcEndReg = nullptr;
    uint32_t* dpcCurrentReg = nullptr;
    uint32_t* viStatusReg = nullptr;
    uint32_t* viOriginReg = nullptr;
    uint32_t* viWidthReg = nullptr;
    uint32_t* viIntrReg = nullptr;
    uint32_t* viVCurrentReg = nullptr;
    uint32_t* viTimingReg = nullptr;
    uint32_t* viVSyncReg = nullptr;
    uint32_t* viHSyncReg = nullptr;
    uint32_t* viXScaleReg = nullptr;
    uint32_t* viYScaleReg = nullptr;
    void* windowHandle = nullptr;
    std::string romName;
    std::string romGameId; // 4-char ID
    uint32_t romCRC1 = 0;
    uint32_t romCRC2 = 0;
    uint8_t romCountry = 0;
    bool isPAL = false;
};

// ──────────────────────────────────────────────────────────────────
//  Callback function types
// ──────────────────────────────────────────────────────────────────
using CheckInterruptsFunc = std::function<void()>;
using ProcessDListFunc = std::function<void()>;
using ProcessRDPListFunc = std::function<void()>;
using ShowCFBFunc = std::function<void()>;
using ViStatusChangedFunc = std::function<void()>;
using ViWidthChangedFunc = std::function<void()>;

// ──────────────────────────────────────────────────────────────────
//  Plugin callbacks
// ──────────────────────────────────────────────────────────────────
struct PluginCallbacks {
    CheckInterruptsFunc checkInterrupts;
    ProcessDListFunc processDList;
    ProcessRDPListFunc processRDPList;
    ShowCFBFunc showCFB;
    ViStatusChangedFunc viStatusChanged;
    ViWidthChangedFunc viWidthChanged;
};

// ──────────────────────────────────────────────────────────────────
//  Frame statistics from plugin
// ──────────────────────────────────────────────────────────────────
struct PluginFrameStats {
    uint32_t dlistCount = 0;
    uint32_t rdpCommandCount = 0;
    uint32_t triangleCount = 0;
    uint32_t rectCount = 0;
    uint32_t textureSwitches = 0;
    uint32_t drawCalls = 0;
    double frameTimeMs = 0.0;
    double dlistTimeMs = 0.0;
    double renderTimeMs = 0.0;
    double swapTimeMs = 0.0;
    double fps = 0.0;
    uint32_t viInterrupts = 0;
    uint64_t rdramBytesRead = 0;
    uint64_t rdramBytesWritten = 0;
};

// ══════════════════════════════════════════════════════════════════
//  VideoPlugin — Main plugin class
// ══════════════════════════════════════════════════════════════════

class VideoPlugin {
public:
    VideoPlugin();
    ~VideoPlugin();

    // ── Lifecycle ────────────────────────────────────────────────
    bool Initialize(const SystemInfo& sysInfo, const PluginCallbacks& callbacks);
    void Shutdown();
    void Reset();

    /// Set an external window to use instead of creating an internal one.
    /// Must be called BEFORE Initialize().
    void SetExternalWindow(WindowManager* window);

    // ── Per-frame operations ─────────────────────────────────────
    void ProcessDList();
    void ProcessDListShadow(); ///< Shadow HLE: geometry-only run for RT under LLE
    void ProcessRDPList();
    void UpdateScreen();
    void ShowCFB();

    // ── VI register changes ──────────────────────────────────────
    void ViStatusChanged();
    void ViWidthChanged();

    // ── ROM events ───────────────────────────────────────────────
    void RomOpen(const SystemInfo& sysInfo);
    void RomClosed();

    // ── Configuration ────────────────────────────────────────────
    PluginConfig& GetConfig() { return m_config; }
    const PluginConfig& GetConfig() const { return m_config; }
    void ApplyConfig(const PluginConfig& config);
    void OpenConfigDialog();

    /// Override resolution after config-file load (set BEFORE Initialize).
    void SetPendingResolution(int scale, int w, int h) {
        m_pendingResScale = scale;
        m_pendingWindowW = w;
        m_pendingWindowH = h;
    }

    // ── Status & Info ────────────────────────────────────────────
    PluginStatus GetStatus() const { return m_status.load(); }
    const std::string& GetName() const { return m_name; }
    const std::string& GetVersion() const { return m_version; }
    PluginFrameStats GetFrameStats() const;
    double GetLastSwapTimeMs() const { return m_frameStats.swapTimeMs; }
    const GameProfile* GetActiveGameProfile() const { return m_activeProfile; }

    // ── Debug ────────────────────────────────────────────────────
    void EnableWireframe(bool enable);
    void EnableTextureDebug(bool enable);
    void TakeScreenshot(const std::string& path);
    void DumpFrameInfo();

    // ── Thread safety ────────────────────────────────────────────
    void LockRender();
    void UnlockRender();

    // ── RDP command forwarding (called from adapter) ─────────────
    void ProcessRDPCommand(uint32_t w0, uint32_t w1);

    // Subsystem access
    std::unique_ptr<RDPProcessor>& GetRDPProcessor() { return m_rdpProcessor; }

private:
    // Internal methods
    bool CreateDisplayRenderer();
    bool CreateDisplayWindow();
    /// Size frames are rendered at: the window's drawable size, or without a window
    /// N64 resolution times the internal scale.
    void GetOutputSize(int32_t& width, int32_t& height) const;
    void DestroyDisplayRenderer();
    void DestroyDisplayWindow();
    void ApplyGameSpecificHacks();
    /// Loads the texture pack for the running game (if any) and turns texture dumping on or off.
    void SetupTexturePack();
    void UpdateVIRegisters();
    uint32_t ReadVIRegister(uint32_t* reg) const;

    /// Decode N64 RDRAM framebuffer and blit to screen via the VI blit shader.
    /// @param viType VI color type (2=16-bit, 3=32-bit)
    /// @param fbAddr Physical RDRAM address of the framebuffer
    /// @param fbWidth Framebuffer width in pixels
    /// @param fbHeight Framebuffer height in pixels
    void BlitVIFramebuffer(uint32_t viType, uint32_t fbAddr, uint32_t fbWidth, uint32_t fbHeight);

    /// Return whichever window is active (external or owned).
    WindowManager* GetActiveWindow() const;

    // ── State ────────────────────────────────────────────────────
    std::string m_name = "NovaGraphics";
    std::string m_version = "1.0.0";
    std::atomic<PluginStatus> m_status{PluginStatus::Uninitialized};

    SystemInfo m_sysInfo;
    PluginCallbacks m_callbacks;
    PluginConfig m_config;
    PluginFrameStats m_frameStats;

    // Subsystems
    std::unique_ptr<RDPProcessor> m_rdpProcessor;
    MLEDirector m_mleDirector;
    RT::RTSceneManager m_rtScene;
    std::unique_ptr<WindowManager> m_window;   // owned window (null when external)
    WindowManager* m_externalWindow = nullptr; // non-owning; set before Init
    std::unique_ptr<IRenderer> m_renderer;
    std::unique_ptr<Tex::TextureCache> m_textureCache;
    std::unique_ptr<Shader::ShaderManager> m_shaderManager;
    std::unique_ptr<MicrocodeDispatcher> m_dispatcher;
    std::unique_ptr<PostProcessingPipeline> m_postProcess;

    // Post-processing intermediate capture (blit default FB → texture → Apply → default FB)
    uint32_t m_ppFBO = 0;
    uint32_t m_ppTex = 0;
    int m_ppWidth = 0;
    int m_ppHeight = 0;

    // VI framebuffer blit resources
    std::unique_ptr<ITexture> m_viTexture; // Texture for VI framebuffer
    uint32_t m_viBlitProgram = 0;          // GL program handle for blit
    uint32_t m_viBlitVAO = 0;              // Empty VAO for fullscreen tri

    const GameProfile* m_activeProfile = nullptr;

    // Synchronization — single mutex protects all GL-touching paths
    std::mutex m_glMutex;

    // Frame counter
    uint64_t m_frameCount = 0;
    uint64_t m_dlistIndex = 0;

    // VI state cache
    uint32_t m_viStatus = 0;
    uint32_t m_viOrigin = 0;
    uint32_t m_viWidth = 0;
    uint32_t m_viXScale = 0;
    uint32_t m_viYScale = 0;

    // Flags
    bool m_wireframe = false;
    bool m_textureDebug = false;
    bool m_romLoaded = false;
    bool m_hleRenderedThisFrame = false;   // Set by ProcessDList
    bool m_hleEverRendered = false;        // Latched once HLE renders
    uint32_t m_framesWithoutHLE = 0;       // Consecutive VI frames without DL
    bool m_swapPending = false;            // Deferred swap: glFlush done, swap deferred to next frame
    bool m_vkFrameBegun = false;           // Vulkan: BeginFrame called during ProcessDList this VI period
    bool m_shadowSceneInitialized = false; // Shadow HLE: m_rtScene.NewFrame() already called this VI period

    // Scratch buffer for ProcessRDPList big-endian → native conversion
    std::vector<uint32_t> m_rdpCmdScratch;

    // Initialization Tracking
    bool m_isInitializationPhase = true;
    uint64_t m_framesAfterRomOpen = 0;
    static constexpr uint64_t ROM_INIT_GRACE_PERIOD = 2;

    // CLI resolution overrides (set before Initialize, applied after config load)
    int m_pendingResScale = 0;
    int m_pendingWindowW = 0;
    int m_pendingWindowH = 0;

    bool IsFramebufferValid(const uint8_t* fbData, uint32_t width, uint32_t height, uint32_t bpp) const;
};

} // namespace Nova64::Video
