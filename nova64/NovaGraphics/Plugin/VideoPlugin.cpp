/**
 * @file VideoPlugin.cpp
 * @brief Video Plugin.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Video Plugin Implementation
// ══════════════════════════════════════════════════════════════════════════════

#include <cctype>
#include "VideoPlugin.hpp"
#include "FrameCallback.hpp"
#include "../Common/Logger.hpp"
#include "../Debug/ImGuiDebugUI.hpp"
#include "../Renderer/GL/GLShaderCompiler.hpp"
#include "../Renderer/VK/VKShaderCompiler.hpp"
#include "../Renderer/VK/VKCommon.hpp"
#include "../Core/FramebufferManager.hpp"
#include "../Core/DepthBuffer.hpp"

#include "../../common/log.hpp"
#include "../../NovaRSP/Settings/RspSettings.hpp"

#include <glad/gl.h>
#include <SDL.h>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

namespace Nova64::Video {

static LogLevel ParseVideoLogLevel(const char* value) {
    if (!value || value[0] == '\0') return LogLevel::Info;
    if (std::strcmp(value, "trace") == 0 || std::strcmp(value, "TRACE") == 0) return LogLevel::Trace;
    if (std::strcmp(value, "debug") == 0 || std::strcmp(value, "DEBUG") == 0) return LogLevel::Debug;
    if (std::strcmp(value, "info") == 0 || std::strcmp(value, "INFO") == 0) return LogLevel::Info;
    if (std::strcmp(value, "warning") == 0 || std::strcmp(value, "WARNING") == 0) return LogLevel::Warning;
    if (std::strcmp(value, "error") == 0 || std::strcmp(value, "ERROR") == 0) return LogLevel::Error;
    return LogLevel::Info;
}

const char* PluginStatusName(PluginStatus status) {
    switch (status) {
    case PluginStatus::Uninitialized: return "Uninitialized";
    case PluginStatus::Initialized:   return "Initialized";
    case PluginStatus::Running:       return "Running";
    case PluginStatus::Paused:        return "Paused";
    case PluginStatus::Error:         return "Error";
    case PluginStatus::ShuttingDown:  return "ShuttingDown";
    default:                          return "Unknown";
    }
}

VideoPlugin::VideoPlugin() = default;

VideoPlugin::~VideoPlugin() {
    if (m_status != PluginStatus::Uninitialized) {
        Shutdown();
    }
}

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

void VideoPlugin::SetExternalWindow(WindowManager* window) { m_externalWindow = window; }

WindowManager* VideoPlugin::GetActiveWindow() const { return m_externalWindow ? m_externalWindow : m_window.get(); }

bool VideoPlugin::Initialize(const SystemInfo& sysInfo, const PluginCallbacks& callbacks) {
    if (m_status != PluginStatus::Uninitialized) {
        LOG_WARN("VideoPlugin already initialized (status={})", PluginStatusName(m_status));
        return false;
    }

    // Initialize NovaGraphics logger so LOG_INFO/WARN/ERROR actually produce output
    if (!g_logger.IsEnabled(LogLevel::Info, LogSubsystem::Core)) {
        LoggerConfig logCfg;
        logCfg.consoleOutput = true;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const char* videoLogPath = std::getenv("NOVA64_VIDEO_LOG");
        logCfg.fileOutput = videoLogPath && videoLogPath[0] != '\0';
        if (logCfg.fileOutput) logCfg.logFilePath = videoLogPath;
        logCfg.colorConsole = true;
        logCfg.globalLevel = ParseVideoLogLevel(std::getenv("NOVA64_VIDEO_LOG_LEVEL"));
        if (std::getenv("NOVA64_MM_RDP_TRACE")) {
            logCfg.subsystemLevels[static_cast<size_t>(LogSubsystem::RDP)] = LogLevel::Info;
        }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        g_logger.Init(logCfg);
    }

    Log::info("NovaGraphics initializing...");
    LOG_INFO("NovaGraphics {} initializing...", m_version);
    m_sysInfo = sysInfo;
    m_callbacks = callbacks;

    // Load configuration
    m_config.LoadDefaults();
    // The file is only written here, when it doesn't exist yet, so there is one
    // to edit. Saving at shutdown wrote per-game overrides and command-line
    // flags back into the user's settings.
    if (!std::filesystem::exists("settings/NovaGraphics.ini")) {
        std::error_code ec;
        std::filesystem::create_directories("settings", ec);
        m_config.SaveToFile("settings/NovaGraphics.ini");
    }
    m_config.LoadFromFile("settings/NovaGraphics.ini");

    // ── MLE pivot: propagate renderingMode to RSP settings ───────────
    // HLE  : graphics microcode handlers own rendering (legacy default).
    // LLE  : RSP recompiler executes graphics ucode; RDPProcessor consumes
    //        the raw RDP FIFO produced by the RSP.
    // Auto : audio HLE, graphics LLE.
    //
    // Must run before any RSP task is dispatched. CRSPSettings::SetRomOpen
    // (called later from N64SYS startup) will trigger RefreshSettings which
    // re-reads NovaStar64.ini — if that file exists and defines GraphicsHLE
    // it will override us. That's intentional: explicit per-emu config wins.
    {
        const RenderingMode mode = m_config.renderingMode;
        const bool wantGfxHle = (mode == RenderingMode::HLE);
        Nova64::RSP::CRSPSettings::SetGraphicsHle(wantGfxHle);
        Nova64::RSP::CRSPSettings::SetAudioHle(true); // keep audio HLE always
        if (!wantGfxHle) {
            // LLE: DoRspCycles still tries HLE first, which keeps audio HLE working,
            // and runs the graphics microcode on the RSP engine (recompiler or
            // interpreter, see CRSPSettings::Engine).
            Nova64::RSP::CRSPSettings::SetCPUMethod(Nova64::RSP::RSPCpuMethod::HleWithFallback);
        }
        LOG_INFO("MLE: renderingMode={} → graphicsHle={}", static_cast<int>(mode), wantGfxHle ? "true" : "false");
    }

    // Apply CLI overrides (set by NovaGraphicsGFXPlugin before Initialize)
    if (m_pendingResScale > 0) {
        m_config.internalResScale = m_pendingResScale;
        m_config.windowWidth = m_pendingWindowW;
        m_config.windowHeight = m_pendingWindowH;
        m_pendingResScale = 0;
    }

    // Initialize game compatibility database
    GameDatabase::Instance().Init();
    GameDatabase::Instance().LoadCustomProfiles("NovaGraphics_games.ini");

    // Create window (unless an external window was provided)
    if (m_externalWindow) {
        LOG_INFO("Using external window (skipping internal window creation)");
    } else {
        if (!CreateDisplayWindow()) {
            LOG_ERROR("Failed to create window");
            m_status = PluginStatus::Error;
            return false;
        }
    }

    // Guard against a missing OpenGL context before touching any GL entry point.
    // In embedded mode the emulator runs on a background thread, and macOS/Cocoa
    // refuses to create an NSWindow + GL context off the main thread — so window
    // init fails and there is no current context here.  Calling into the renderer
    // (glGetIntegerv) at that point segfaults and takes the whole process — and
    // the embedding launcher — down.  Fail the plugin cleanly instead.
    const bool usingGL = (m_config.backendType != 1); // 0/default = OpenGL, 1 = Vulkan
    if (usingGL && SDL_GL_GetCurrentContext() == nullptr) {
        LOG_ERROR("VideoPlugin: no current GL context (cannot create a GL window off the "
                  "main thread on macOS in embedded mode) — disabling video instead of crashing.");
        DestroyDisplayWindow();
        m_status = PluginStatus::Error;
        return false;
    }

    // Create renderer
    if (!CreateDisplayRenderer()) {
        LOG_ERROR("Failed to create renderer");
        DestroyDisplayWindow();
        m_status = PluginStatus::Error;
        return false;
    }

    // Initialize the renderer — this sets up GL state (depth test, scissor,
    // clip control) and queries capabilities. GLRenderer::Initialize() does
    // not create a context; the SDL+OpenGL context was already made by the
    // WindowManager, so passing nullptr for the window handle is correct.
    {
        // With no window, Vulkan renders offscreen (a game inside the launcher).
        int32_t winW = 0, winH = 0;
        GetOutputSize(winW, winH);
        void* windowHandle = nullptr;
        if (auto* win = GetActiveWindow()) windowHandle = win->GetSDLWindow();
        if (!m_renderer->Initialize(windowHandle, static_cast<uint32_t>(winW), static_cast<uint32_t>(winH))) {
            // Abort instead of limping on: downstream init (RDPProcessor →
            // CreateTexture) dereferences the renderer's device context and
            // SEGFAULTS on a half-initialized renderer (seen with LLE mode in
            // headless runs where no GL window exists).
            LOG_ERROR("Renderer Initialize() failed — aborting video plugin init");
            m_renderer.reset();
            return false;
        }
    }

    {
        const auto& caps = m_renderer->GetCapabilities();
        std::lock_guard<std::mutex> lock(g_frameMutex);
        g_rendererDescription = std::string(GetBackendName(m_renderer->GetBackendType())) + " on " + caps.deviceName;
    }

    // Create texture cache
    m_textureCache = std::make_unique<Tex::TextureCache>();
    {
        Tex::TextureCacheConfig tcConfig;
        tcConfig.enableMipmaps = m_config.enableMipmaps;
        tcConfig.lodBias = m_config.lodBias;
        m_textureCache->Initialize(m_renderer.get(), tcConfig);
    }

    // Create shader manager — use a Vulkan-aware compiler on the VK path,
    // and the GL compiler on the OpenGL path.
    m_shaderManager = std::make_unique<Shader::ShaderManager>();
    {
        const bool isVulkan = (m_renderer->GetBackendType() == BackendType::Vulkan);
        const Shader::BackendAPI shaderApi = isVulkan ? Shader::BackendAPI::Vulkan : Shader::BackendAPI::OpenGL;
        m_shaderManager->Initialize(shaderApi);

        if (isVulkan) {
            // VKShaderCompiler: compiles GLSL → SPIR-V, writes uniforms directly
            // into the VKBackend's per-frame scratch UBO via GetMappedUBOPtr().
            auto vkCompiler = std::make_unique<VK::VKShaderCompiler>(m_renderer.get());
            vkCompiler->Initialize(shaderApi);
            m_shaderManager->SetCompiler(std::move(vkCompiler));
        } else {
            auto glCompiler = std::make_unique<GL::GLShaderCompiler>();
            glCompiler->Initialize(shaderApi);
            m_shaderManager->SetCompiler(std::move(glCompiler));
        }
        std::filesystem::create_directories("ShaderCache");
        m_shaderManager->LoadCache("ShaderCache/NovaGraphics_shadercache.bin");
    }

    // Initialize RDP processor with real renderer, texture cache, and shader manager
    m_rdpProcessor = std::make_unique<RDPProcessor>();
    m_rdpProcessor->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize, m_renderer.get(), m_textureCache.get(),
                               m_shaderManager.get());
    m_rdpProcessor->SetMLEDirector(&m_mleDirector);

    // Wire SyncFull → DP interrupt. When a display list contains RDPFULLSYNC, the
    // N64 CPU expects the DP interrupt to fire at that point (not after the whole
    // display list). Using the checkInterrupts callback here delivers the interrupt
    // at the architecturally-correct moment, matching real N64 hardware timing.
    if (m_callbacks.checkInterrupts) {
        m_rdpProcessor->SetSyncFullCallback(m_callbacks.checkInterrupts);
    }

    // Create microcode dispatcher for HLE display list processing
    m_dispatcher = std::make_unique<MicrocodeDispatcher>();
    m_dispatcher->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize, &m_rdpProcessor->GetVideoStateMutable().rdp,
                             m_rdpProcessor.get());

    // Initialize framebuffer manager (creates blit/filter GL resources)
    g_fbManager.Init(static_cast<uint32_t>(m_config.internalResScale));

    // Apply widescreen hack if configured
    if (m_config.aspectRatio == AspectRatio::Widescreen16_9) {
        float wsScale = (16.0f / 9.0f) / (4.0f / 3.0f); // ≈ 1.333
        m_rdpProcessor->GetVideoStateMutable().rdp.geometry.widescreenScale = wsScale;
        LOG_INFO("Widescreen 16:9 hack enabled (scale={:.3f})", wsScale);
    }

    // Shape of the picture in the window, the same for both renderers: 4:3
    // like the N64's video output, 16:9 with either widescreen setting.
    const float outputAspect =
        (m_config.aspectRatio == AspectRatio::Widescreen16_9 || m_config.aspectRatio == AspectRatio::Stretch16_9)
            ? 16.0f / 9.0f
            : 4.0f / 3.0f;
    g_fbManager.SetOutputAspect(outputAspect);
    if (m_renderer) m_renderer->SetOutputAspect(outputAspect);

    // Initialize depth buffer manager (N64-accurate depth compare)
    g_depthManager.Init();
    if (g_fbManager.IsGL()) {
        g_depthManager.InitZLUT();
    }

    // Initialize post-processing pipeline (GL-only)
    if (g_fbManager.IsGL()) {
        m_postProcess = std::make_unique<PostProcessingPipeline>();
        int32_t w = 0, h = 0;
        GetOutputSize(w, h);
        m_postProcess->Init(w, h);
        m_postProcess->SetupDefaultChain(); // Gamma + FXAA
        LOG_INFO("Post-processing pipeline initialized ({}x{})", w, h);
    }

    // Create VI blit resources (GL-only: fullscreen triangle + texture blit shader)
    if (g_fbManager.IsGL()) {
        // Compile blit shader
        static const char* kBlitVS = R"(#version 460 core
layout(location = 0) out vec2 v_texCoord;
void main() {
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0)
    );
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    vec2 uv = positions[gl_VertexID] * 0.5 + 0.5;
    v_texCoord = vec2(uv.x, 1.0 - uv.y); // Flip Y: N64 FB is top-to-bottom, GL tex is bottom-to-top
}
)";
        static const char* kBlitFS = R"(#version 460 core
layout(binding = 0) uniform sampler2D u_screenTexture;
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 fragColor;
uniform vec4 uCropRect;  // (uMin, vMin, uMax, vMax) — overscan crop
void main() {
    vec2 uv = mix(uCropRect.xy, uCropRect.zw, v_texCoord);
    fragColor = vec4(texture(u_screenTexture, uv).rgb, 1.0);
}
)";
        // Compile vertex shader
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &kBlitVS, nullptr);
        glCompileShader(vs);
        {
            GLint status = 0;
            glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
            if (!status) {
                char infoLog[512];
                glGetShaderInfoLog(vs, sizeof(infoLog), nullptr, infoLog);
                LOG_ERROR("VI blit vertex shader compile failed: {}", infoLog);
            }
        }

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &kBlitFS, nullptr);
        glCompileShader(fs);
        {
            GLint status = 0;
            glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
            if (!status) {
                char infoLog[512];
                glGetShaderInfoLog(fs, sizeof(infoLog), nullptr, infoLog);
                LOG_ERROR("VI blit fragment shader compile failed: {}", infoLog);
            }
        }

        m_viBlitProgram = glCreateProgram();
        glAttachShader(m_viBlitProgram, vs);
        glAttachShader(m_viBlitProgram, fs);
        glLinkProgram(m_viBlitProgram);
        {
            GLint status = 0;
            glGetProgramiv(m_viBlitProgram, GL_LINK_STATUS, &status);
            if (!status) {
                char infoLog[512];
                glGetProgramInfoLog(m_viBlitProgram, sizeof(infoLog), nullptr, infoLog);
                LOG_ERROR("VI blit program link failed: {}", infoLog);
            }
        }
        glDeleteShader(vs);
        glDeleteShader(fs);

        // Empty VAO for fullscreen triangle
        glCreateVertexArrays(1, &m_viBlitVAO);
    }

    m_isInitializationPhase = true;
    m_framesAfterRomOpen = 0;

    m_status = PluginStatus::Initialized;
    LOG_INFO("NovaGraphics initialized successfully");

    // Initialize ImGui debug UI (GL-only; Vulkan path uses no ImGui for now)
    if (g_fbManager.IsGL()) {
        if (auto* activeWin = GetActiveWindow()) {
            g_imguiDebugUI.Init(activeWin->GetSDLWindow(), activeWin->GetGLContext());
        }
    }

    return true;
}

void VideoPlugin::Shutdown() {
    if (m_status == PluginStatus::Uninitialized) return;

    // Complete any pending deferred swap before shutting down
    if (m_swapPending) {
        if (auto* win = GetActiveWindow()) {
            win->MakeContextCurrent();
            win->SwapBuffers();
        }
        m_swapPending = false;
    }

    LOG_INFO("NovaGraphics shutting down...");
    m_status = PluginStatus::ShuttingDown;

    // The last frame may still be executing. Everything below destroys textures, shaders
    // and buffers it can reference, so let it finish first.
    if (m_renderer) m_renderer->WaitIdle();

    // Shut down ImGui debug UI
    g_imguiDebugUI.Shutdown();

    // Shut down subsystems in reverse order

    if (m_dispatcher) {
        m_dispatcher->Shutdown();
        m_dispatcher.reset();
    }

    if (m_postProcess) {
        m_postProcess->Shutdown();
        m_postProcess.reset();
    }

    if (m_rdpProcessor) {
        m_rdpProcessor->Shutdown();
        m_rdpProcessor.reset();
    }

    if (m_shaderManager) {
        m_shaderManager->SaveCache("ShaderCache/NovaGraphics_shadercache.bin");
        m_shaderManager->Shutdown();
        m_shaderManager.reset();
    }

    if (m_textureCache) {
        m_textureCache->Shutdown();
        m_textureCache.reset();
    }

    // Destroy VI blit resources (GL-only)
    m_viTexture.reset();
    if (g_fbManager.IsGL()) {
        if (m_viBlitProgram) {
            glDeleteProgram(m_viBlitProgram);
            m_viBlitProgram = 0;
        }
        if (m_viBlitVAO) {
            glDeleteVertexArrays(1, &m_viBlitVAO);
            m_viBlitVAO = 0;
        }
    }

    // Shut down depth buffer manager
    g_depthManager.Shutdown();

    // Destroy post-processing intermediate FBO (GL-only)
    if (g_fbManager.IsGL()) {
        if (m_ppFBO) {
            glDeleteFramebuffers(1, &m_ppFBO);
            m_ppFBO = 0;
        }
        if (m_ppTex) {
            glDeleteTextures(1, &m_ppTex);
            m_ppTex = 0;
        }
    }
    m_ppWidth = 0;
    m_ppHeight = 0;

    DestroyDisplayRenderer();
    DestroyDisplayWindow();

    m_activeProfile = nullptr;
    m_romLoaded = false;
    m_frameCount = 0;
    m_dlistIndex = 0;

    m_status = PluginStatus::Uninitialized;
    LOG_INFO("NovaGraphics shutdown complete");
}

void VideoPlugin::Reset() {
    LOG_INFO("NovaGraphics resetting...");
    std::lock_guard<std::mutex> lock(m_glMutex);

    if (m_rdpProcessor) m_rdpProcessor->Reset(false);
    if (m_dispatcher) m_dispatcher->ClearHandlerCache();
    if (m_textureCache) m_textureCache->InvalidateAll();

    m_frameStats = {};
    m_frameCount = 0;
    m_dlistIndex = 0;
    m_hleRenderedThisFrame = false;
    m_hleEverRendered = false;
    m_framesWithoutHLE = 0;
    m_viStatus = 0;
    m_viOrigin = 0;
    m_viWidth = 0;
    m_viXScale = 0;
    m_viYScale = 0;
}

// ═══════════════════════════════════════════════════════════════
//  Per-Frame Operations
// ═══════════════════════════════════════════════════════════════

void VideoPlugin::ProcessDListShadow() {
    // Shadow HLE: run the GBI display list in geometry-only mode so the RT
    // layer can collect world-space triangles while LLE is active.
    // No GL/Vulkan context needed; no RDP commands are submitted.
    if (m_status != PluginStatus::Running && m_status != PluginStatus::Initialized) return;
    if (!m_sysInfo.dmem || !m_dispatcher) return;

    // Skip the entire shadow pass when nothing would consume its output:
    //   - Widescreen write-back only does work when aspectRatio=Widescreen16_9
    //   - RT geometry only matters when the backend actually supports raytracing
    // Running shadow HLE unconditionally doubles the GBI T&L cost for no gain.
    const bool needsWidescreen = (m_config.aspectRatio == AspectRatio::Widescreen16_9);
    const bool needsRT = m_renderer && m_renderer->GetCapabilities().supportsRayTracing;
    if (!needsWidescreen && !needsRT) return;

    auto readDMEM = [&](uint32_t absOff) -> uint32_t {
        return *reinterpret_cast<const uint32_t*>(m_sysInfo.dmem + absOff);
    };

    uint32_t taskDataPtr = readDMEM(0xFF0) & 0x00FFFFFF;
    uint32_t ucodeAddr = readDMEM(0xFD0) & 0x00FFFFFF;
    uint32_t ucodeSize = readDMEM(0xFD4);
    uint32_t ucodeDataAddr = readDMEM(0xFD8) & 0x00FFFFFF;
    uint32_t ucodeDataSize = readDMEM(0xFDC);

    if (taskDataPtr >= m_sysInfo.rdramSize) return;

    // Start a new RT scene on the first shadow task of this VI period.
    // Subsequent tasks in the same frame accumulate into the same scene.
    // ProcessRDPList will not call NewFrame again thanks to m_shadowSceneInitialized.
    if (!m_shadowSceneInitialized) {
        m_rtScene.NewFrame();
        m_shadowSceneInitialized = true;
    }

    m_dispatcher->RunShadow(taskDataPtr, ucodeAddr, ucodeSize, ucodeDataAddr, ucodeDataSize, taskDataPtr, &m_rtScene,
                            (m_config.aspectRatio == AspectRatio::Widescreen16_9) ? m_sysInfo.rdram : nullptr);
}

void VideoPlugin::ProcessDList() {
    if (m_status != PluginStatus::Running && m_status != PluginStatus::Initialized) return;

    auto start = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(m_glMutex);

    // Ensure GL context is current on this thread (may be called from CPU thread)
    if (auto* win = GetActiveWindow()) win->MakeContextCurrent();

    m_status = PluginStatus::Running;
    m_dlistIndex++;
    m_frameStats.dlistCount++;

    // Read DL pointer from DMEM task structure
    // The OSTask header lives at DMEM[0xFC0..0xFFF].
    // DMEM is stored in native endian by the emulator, so use direct uint32_t reads.
    // Field offsets (absolute DMEM addresses, matching NovaRSP TaskOffset):
    //   0xFC0: Type         0xFC4: Flags
    //   0xFC8: UcodeBoot    0xFCC: UcodeBootSize
    //   0xFD0: Ucode        0xFD4: UcodeSize
    //   0xFD8: UcodeData    0xFDC: UcodeDataSize
    //   0xFE0: DramStack    0xFE4: DramStackSize
    //   0xFE8: OutputBuff   0xFEC: OutputBuffSize
    //   0xFF0: DataPtr      0xFF4: DataSize
    //   0xFF8: YieldDataPtr 0xFFC: YieldDataSize
    if (!m_sysInfo.dmem) {
        LOG_WARN("ProcessDList: dmem is null");
        return;
    }

    auto readDMEM_native = [&](uint32_t absOffset) -> uint32_t {
        return *reinterpret_cast<const uint32_t*>(m_sysInfo.dmem + absOffset);
    };

    uint32_t taskDataPtr = readDMEM_native(0xFF0); // DataPtr

    // Mask to physical RDRAM
    taskDataPtr &= 0x00FFFFFF;

    if (taskDataPtr >= m_sysInfo.rdramSize) {
        LOG_WARN("ProcessDList: taskDataPtr=0x{:08X} >= rdramSize=0x{:08X}", taskDataPtr, m_sysInfo.rdramSize);
        return;
    }

    LOG_DEBUG("ProcessDList: taskDataPtr=0x{:08X} rdramSize=0x{:08X}", taskDataPtr, m_sysInfo.rdramSize);

    // Process the display list through the microcode interpreter
    if (m_dispatcher && m_rdpProcessor) {
        uint32_t ucodeAddr = readDMEM_native(0xFD0) & 0x00FFFFFF;     // Ucode
        uint32_t ucodeSize = readDMEM_native(0xFD4);                  // UcodeSize
        uint32_t ucodeDataAddr = readDMEM_native(0xFD8) & 0x00FFFFFF; // UcodeData
        uint32_t ucodeDataSize = readDMEM_native(0xFDC);              // UcodeDataSize
        uint32_t dlAddr = taskDataPtr;

        // Notify the renderer of the window size BEFORE BeginFrame so that any
        // swapchain recreation triggered by OnResize happens before the render
        // pass command buffer is opened.  (Swapchain recreation mid-frame would
        // leave the open command buffer referencing destroyed framebuffers.)
        // Do NOT call glViewport here — OnSetColorImage (emitted by the DL) binds the correct
        // game FBO and sets glViewport to the FBO dimensions (320×240). Forcing 640×480 here
        // would map NDC 0..1 to window coords 320..640, clipping the right half of every draw.
        int32_t winW = 0, winH = 0;
        GetOutputSize(winW, winH);
        m_renderer->OnResize(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));

        // On Vulkan, begin the frame here (before any draw calls).
        // BeginFrame acquires a swapchain image and opens the render pass so
        // that FlushBatch draw calls are recorded into the command buffer.
        // Multiple display lists per VI period are safe: the flag prevents
        // a second BeginFrame call while the frame is already open.
        if (!g_fbManager.IsGL() && !m_vkFrameBegun) {
            m_renderer->BeginFrame();
            m_vkFrameBegun = true;
        }
        if (g_fbManager.IsGL()) {
            glDisable(GL_SCISSOR_TEST);
        }

        // Reset RDP state for this display list. Each display list should start with
        // clean state (fill color, blend mode, depth test flags, etc.), not inherit from the
        // previous frame.
        if (m_rdpProcessor) {
            m_rdpProcessor->Reset(false); // warm reset: fresh RDP state, keep frame stats
        }

        // RT scene extraction — only collect geometry when the backend actually
        // supports ray tracing.  On Intel Iris Xe (and any other non-RT GPU)
        // supportsRayTracing is false, so skipping this saves the per-triangle
        // SubmitTriangles() call overhead on every frame.
        // Compute-RT path: MoltenVK can't do hardware RT, so NOVA64_RT=1 enables
        // the compute ray tracer (CPU BVH + compute traversal) which the backend
        // runs in ExecuteRayTracing. Either path needs the same scene extraction.
        static const bool s_computeRTEnv = []() {
            const char* v = std::getenv("NOVA64_RT");
            return v && v[0] && v[0] != '0';
        }();
        const bool computeRT = s_computeRTEnv || m_config.rayTracing;
        const bool hwRT = m_renderer && m_renderer->GetCapabilities().supportsRayTracing;
        const bool needsRT = m_renderer && (hwRT || computeRT);
        // One-time confirmation so it's obvious whether RT is active.
        {
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                LOG_INFO("Compute ray tracing: {} (env={}, ini={}, hwRT={})", needsRT ? "ENABLED" : "disabled",
                         s_computeRTEnv, m_config.rayTracing, hwRT);
            }
        }
        if (needsRT) {
            m_rtScene.NewFrame();
            m_dispatcher->SetGeometryConsumer(&m_rtScene);
        }

        m_dispatcher->ProcessGraphicsTask(taskDataPtr, ucodeAddr, ucodeSize, ucodeDataAddr, ucodeDataSize, dlAddr);

        // Detach consumer before batch flush (consumer already has all triangles)
        m_dispatcher->SetGeometryConsumer(nullptr);

        // After the display list, flush any remaining RDP batch
        m_rdpProcessor->FlushBatch();

        // Dispatch hardware ray tracing if the backend supports it and the
        // scene accumulated geometry this frame.
        if (needsRT && m_rtScene.GetTriangleCount() > 0) {
            m_renderer->ExecuteRayTracing(&m_rtScene, static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));
        }

        // Log RT scene stats if debug logging is enabled
        if (needsRT) m_rtScene.DebugLogIfEnabled();

        // Signal that HLE content was rendered — UpdateScreen will
        // present it instead of (or composited with) the VI framebuffer.
        m_hleRenderedThisFrame = true;

        // Reset per-frame stats for next display list
        m_rdpProcessor->OnFrameEnd();
    }

    auto end = std::chrono::high_resolution_clock::now();
    m_frameStats.dlistTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
}

void VideoPlugin::ProcessRDPList() {
    if (m_status != PluginStatus::Running && m_status != PluginStatus::Initialized) return;
    if (!m_sysInfo.dpcCurrentReg || !m_sysInfo.dpcEndReg) return;

    uint32_t startAddr = *m_sysInfo.dpcCurrentReg & 0x00FFFFFF;
    uint32_t endAddr = *m_sysInfo.dpcEndReg & 0x00FFFFFF;

    // ── LLE diagnostic: trace first 4 ProcessRDPList invocations ──
    static uint32_t s_traceCount = 0;
    const bool trace = (++s_traceCount <= 4);
    if (trace) {
        Log::infof("VPLLE[%u] ProcessRDPList enter dpcStart=0x%06X dpcEnd=0x%06X status=%d rdram=%p", s_traceCount,
                   startAddr, endAddr, static_cast<int>(m_status.load()), m_sysInfo.rdram);
    }

    if (endAddr <= startAddr) {
        if (trace) Log::infof("VPLLE[%u] skipped: endAddr<=startAddr", s_traceCount);
        return;
    }
    if (endAddr > m_sysInfo.rdramSize) {
        if (trace)
            Log::infof("VPLLE[%u] skipped: endAddr 0x%06X > rdramSize 0x%X", s_traceCount, endAddr,
                       m_sysInfo.rdramSize);
        return;
    }

    std::lock_guard<std::mutex> lock(m_glMutex);
    if (auto* win = GetActiveWindow()) win->MakeContextCurrent();

    // ── LLE frame open (no ProcessDList path) ───────────────────
    // Under LLE, F3D microcode runs on the RSP interpreter and emits raw RDP
    // commands directly to RDRAM. ProcessDList is never invoked, so the
    // Vulkan frame would never be opened and nothing would present.
    // Open the frame here, mirroring ProcessDList's preamble. On Vulkan, the
    // first ProcessRDPList of the VI period opens the swapchain frame; on GL
    // the default FBO is always available.
    if (!m_vkFrameBegun) {
        int32_t winW = 0, winH = 0;
        GetOutputSize(winW, winH);
        m_renderer->OnResize(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));
        if (!g_fbManager.IsGL()) {
            m_renderer->BeginFrame();
            m_vkFrameBegun = true;
        }
        // NOTE: Do NOT call RDPProcessor::Reset(false) here. RDPState::Reset
        // clears otherModeL/H, combiner, tiles, scissor etc. regardless of the
        // `cold` flag, so a per-frame warm reset would wipe persistent RDP
        // state that the N64 hardware keeps between display lists. Under LLE
        // the RSP/RDP state must persist exactly like real hardware --
        // otherwise the first triangles of each frame run with depthtest=0,
        // tile=0, combiner=0 and visibly break depth/textures.
        // Shadow HLE already called NewFrame for the RT scene if it ran first;
        // only call it here if shadow HLE was not active (pure LLE with no RT).
        if (!m_shadowSceneInitialized) {
            m_rtScene.NewFrame();
        }
        m_dispatcher->SetGeometryConsumer(&m_rtScene);
    }

    // Read the RDRAM buffer as native 32-bit words (PJ64-style: RDRAM stores
    // words in host LE order, so a native uint32_t read gives the correct N64 value).
    uint32_t byteCount = endAddr - startAddr;
    int wordCount = static_cast<int>(byteCount / 4);

    m_rdpCmdScratch.resize(wordCount);
    const uint32_t* srcWords = reinterpret_cast<const uint32_t*>(m_sysInfo.rdram + startAddr);
    for (int i = 0; i < wordCount; ++i) m_rdpCmdScratch[i] = srcWords[i];

    m_rdpProcessor->ProcessCommandBuffer(m_rdpCmdScratch.data(), wordCount);
    m_frameStats.rdpCommandCount += static_cast<uint32_t>(wordCount / 2);
    if (trace) {
        Log::infof("VPLLE[%u] ProcessCommandBuffer done: words=%d firstWord=0x%08X rdpCount=%u", s_traceCount,
                   wordCount, wordCount > 0 ? m_rdpCmdScratch[0] : 0u, m_frameStats.rdpCommandCount);
    }

    // Update DPC current register
    *m_sysInfo.dpcCurrentReg = endAddr;

    // RDP commands were processed — mark this frame as HLE-rendered so
    // UpdateScreen will blit the game FBO instead of the VI RDRAM buffer.
    m_hleRenderedThisFrame = true;

    // Note: DP interrupt is signaled by the RDP processor's SyncFull callback,
    // not here. This ensures the interrupt only fires when sync_full is processed.
}

void VideoPlugin::ProcessRDPCommand(uint32_t w0, uint32_t w1) {
    if (!m_renderer) return; // video disabled (no GL context)
    if (!m_rdpProcessor) return;

    // Dispatch to RDP processor
    uint64_t cmd = (static_cast<uint64_t>(w0) << 32) | w1;
    m_rdpProcessor->ProcessRDPCommand(cmd);

    // Track statistics
    uint8_t opcode = (w0 >> 24) & 0x3F;
    switch (opcode) {
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: m_frameStats.triangleCount++; break;
    case 0x24: // Texture rectangle
    case 0x25: // Texture rectangle flip
    case 0x36: // Fill rectangle
        m_frameStats.rectCount++;
        break;
    }
}

void VideoPlugin::UpdateScreen() {
    // Video disabled (no renderer/GL context — e.g. embedded mode on macOS where
    // the window can't be created on the emulator's background thread).  Skip all
    // GL work instead of crashing in glClearColor/etc.
    if (!m_renderer) return;

    // Advance the texture cache's frame counter. This drains its deferred-
    // destroy "zombie" queue (evicted GPU textures held a few frames so any
    // in-flight command buffer referencing them finishes first) and runs its
    // periodic stale-texture sweep. Without this call -- which nothing in the
    // codebase was making -- evicted textures' GPU resources were never
    // actually freed: every cache eviction (routine once the 8192-entry/256MB
    // budget fills, which happens well within a normal play session) leaked
    // its Vulkan/Metal texture allocation for the rest of the process, until
    // MoltenVK's IOGPU residency-set allocation limit was exceeded and the
    // driver called abort() (SIGABRT) out from under a texture upload.
    if (m_textureCache) m_textureCache->BeginFrame();

    auto start = std::chrono::high_resolution_clock::now();
    auto stage = start;
    double tSwap = 0, tCompose = 0, tPost = 0, tImgui = 0, tAlpha = 0, tFlush = 0, tRead = 0;
    auto markStage = [&](double& slot) {
        auto now = std::chrono::high_resolution_clock::now();
        slot += std::chrono::duration<double, std::milli>(now - stage).count();
        stage = now;
    };
    std::lock_guard<std::mutex> lock(m_glMutex);

    // Ensure GL context is current on this thread (may be called from CPU thread)
    auto* win = GetActiveWindow();
    if (win) win->MakeContextCurrent();

    // Deferred swap: complete the previous frame's swap now.
    // The GPU work was already submitted via glFlush() at the end of the
    // previous UpdateScreen, so SwapBuffers should return near-instantly
    // because the GPU has had the entire CPU frame time to finish.
    if (m_swapPending && win) {
        auto swapStart = std::chrono::high_resolution_clock::now();
        win->SwapBuffers();
        auto swapEnd = std::chrono::high_resolution_clock::now();
        m_frameStats.swapTimeMs = std::chrono::duration<double, std::milli>(swapEnd - swapStart).count();
        m_swapPending = false;
    }

    // Capture and reset the Vulkan frame-begun flag.  ProcessDList sets this
    // when it calls BeginFrame before recording draw commands.  UpdateScreen
    // checks it to know whether to call EndFrame+Present (frame in-flight) or
    // BeginFrame+EndFrame+Present (no draw calls this VI period, clear only).
    const bool hadVkFrameBegun = m_vkFrameBegun;
    m_vkFrameBegun = false;
    // Reset shadow HLE scene flag so the next VI period starts fresh.
    const bool hadShadowScene = m_shadowSceneInitialized;
    m_shadowSceneInitialized = false;

    // LLE: when ProcessRDPList opened a frame, it has been recording RDP
    // draws into the batched renderer. Flush any pending primitives and run
    // end-of-frame book-keeping before EndFrame/Present below.
    if (hadVkFrameBegun && m_rdpProcessor) {
        m_rdpProcessor->FlushBatch();
        m_rdpProcessor->OnFrameEnd();
        m_hleRenderedThisFrame = true;
        if (m_dispatcher) m_dispatcher->SetGeometryConsumer(nullptr);
        // RT: dispatch ray tracing if shadow HLE captured geometry this frame.
        const bool canRT = m_renderer && m_renderer->GetCapabilities().supportsRayTracing;
        if (canRT && hadShadowScene && m_rtScene.GetTriangleCount() > 0) {
            int32_t winW = 0, winH = 0;
            GetOutputSize(winW, winH);
            m_renderer->ExecuteRayTracing(&m_rtScene, static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));
        }
    }

    markStage(tSwap);

    m_frameCount++;
    m_frameStats.viInterrupts++;
    // Diagnostics: publish the authoritative per-VI frame index so the headless
    // screenshot capture (VKBackend) can key off it. The Vulkan present counter
    // is decoupled from VI rate in HLE, so VI count is the consistent timeline.
    g_presentIndex.store(static_cast<long>(m_frameStats.viInterrupts), std::memory_order_relaxed);

    // Read VI registers
    UpdateVIRegisters();

    // Propagate VI config to FramebufferManager so GetDisplayFramebuffer() works.
    // Without this, m_viConfig.origin stays 0 and BlitToScreen() always returns early.
    {
        VIConfig viCfg;
        viCfg.origin = m_viOrigin;
        viCfg.width = m_viWidth;
        viCfg.xScale = m_viXScale;
        viCfg.yScale = m_viYScale;
        viCfg.control = m_viStatus;
        // Bits 8-9 = AA mode: 0/1 = AA enabled, 2 = resample only, 3 = replicate
        uint32_t aaMode = (m_viStatus >> 8) & 0x3;
        viCfg.antiAlias = (aaMode <= 1);
        viCfg.dedither = (aaMode == 0);                     // Only AA mode 0 enables dither removal
        viCfg.divot = (m_viStatus & (1u << 4)) != 0;
        viCfg.gammaCorrect = (m_viStatus & (1u << 3)) != 0; // VI_STATUS gamma enable
        viCfg.gammaDither = (m_viStatus & (1u << 2)) != 0;  // VI_STATUS gamma dither
        viCfg.interlaced = (m_viStatus & (1u << 6)) != 0;   // Serrate bit
        viCfg.field = static_cast<uint32_t>(m_frameCount & 1);
        uint32_t yscale = m_viYScale & 0xFFF;
        uint32_t baseH = m_sysInfo.isPAL ? 288u : 240u;
        viCfg.height = (yscale > 0) ? (baseH * yscale) / 1024u : baseH;
        g_fbManager.UpdateVI(viCfg);

        // ── CPU->GPU framebuffer upload (Vulkan) ──
        // UpdateVI's own upload path calls FramebufferManager::CopyToGPU, which
        // is GL-only, so on Vulkan a buffer the CPU composed in RDRAM never
        // reached the GPU and displayed as nothing. Mirror it here for the
        // display buffer whenever the RDP has NOT rendered into it this frame
        // (gpuModified false) -- that is exactly the "content came from the CPU,
        // not from us" case, and it avoids stomping GPU-rendered frames.
        if (m_renderer && !g_fbManager.IsGL() && m_sysInfo.rdram && m_config.fbWriteAccess) {
            if (auto* viFB = g_fbManager.GetDisplayFramebuffer()) {
                // Sticky check, NOT gpuModified: the read-back clears
                // gpuModified, so using it would let a later frame upload an
                // N64-resolution RDRAM copy over a GPU-rendered frame and
                // discard the internal-resolution detail.
                if (!viFB->everGpuRendered) g_fbManager.CopyToGPUVulkan(viFB, m_sysInfo.rdram, m_renderer.get());
            }
        }
    }

    // Determine output dimensions from VI registers
    uint32_t viType = m_viStatus & 0x3;
    bool viActive = (viType == 2 || viType == 3); // 16-bit or 32-bit

    // ── Compositing strategy ─────────────────────────────────────
    // If the HLE path rendered triangles this frame (ProcessDList called
    // FlushBatch), the content is already on the default framebuffer.
    // In that case we skip the VI framebuffer blit so the HLE output is
    // presented directly.  When no HLE content was produced (e.g. the game
    // renders entirely via the CPU, or the N64 boot screen), fall back to
    // the VI RDRAM blit.
    //
    // Once the HLE path has produced content, latch m_hleEverRendered so we
    // never fall back to VI RDRAM blit on intermediate (non-DL) VI frames.
    // Without this, double-buffered SwapBuffers alternates HLE ↔ VI each
    // frame, causing rapid black/white flickering.
    if (m_hleRenderedThisFrame) {
        m_hleEverRendered = true;
        m_framesWithoutHLE = 0;
    } else if (m_hleEverRendered) {
        // No display list this frame while latched — count consecutive misses.
        // GL: unlatch after 3 frames so the VI RDRAM blit path can take over.
        // Vulkan: no VI RDRAM blit is implemented; unlatching falls back to a
        // black clear frame which is worse than persisting the last HLE frame.
        // Keep the latch on Vulkan to avoid the black-screen regression.
        m_framesWithoutHLE++;
        const uint32_t kUnlatchThreshold = g_fbManager.IsGL() ? 3 : 60;
        if (m_framesWithoutHLE >= kUnlatchThreshold) {
            m_hleEverRendered = false;
        }
    }

    bool hasVIContent = viActive && m_viOrigin != 0 && m_viWidth != 0;
    bool useVIBlit = !m_hleRenderedThisFrame && !m_hleEverRendered && hasVIContent;
    bool didRender = false;

    // During the initialization grace period, show the VI RDRAM blit
    // (this is the N64 boot logo / IPL sequence).  Don't black out.
    if (m_isInitializationPhase) {
        m_framesAfterRomOpen++;
        if (m_framesAfterRomOpen >= ROM_INIT_GRACE_PERIOD) {
            m_isInitializationPhase = false;
        }
        // During init, never override — let useVIBlit decide
    }

    if (m_hleRenderedThisFrame || m_hleEverRendered) {
        // HLE content is available — present it directly (no VI RDRAM compositing).
        int32_t winW = 0, winH = 0;
        GetOutputSize(winW, winH);

        // Vulkan per-buffer canvases: present the buffer VI says is display-
        // ready, not the one the display list is still drawing into.
        if (m_renderer && m_viOrigin != 0) m_renderer->SetPresentOrigin(m_viOrigin & 0x00FFFFFF);

        // VI_STATUS bottom 2 bits = colour depth: 0 = blank.
        // When the game intentionally blanks the screen (transitions,
        // fades, white flashes) we must clear to black instead of
        // re-presenting the last HLE frame, otherwise the user sees the
        // previous menu colour persisting through the transition.
        if (viType == 0) {
            if (g_fbManager.IsGL()) {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            } else {
                // Vulkan: VI_TYPE=0 means game is blanking the screen (transition/fade).
                // Only present the clear if this frame actually had draw calls;
                // otherwise, skip the present to avoid a black flash on intermediate
                // VI interrupts (N64 renders at 30 fps, VI fires at 60 fps).
                // Also drop the frame-carry canvas so the blank doesn't resurrect
                // the previous frame's content on the next BeginFrame.
                m_renderer->InvalidateCanvas();
                if (hadVkFrameBegun) {
                    m_renderer->EndFrame();
                    m_renderer->Present();
                }
            }
            didRender = true;
        } else {
            if (g_fbManager.IsGL()) {
                g_fbManager.BlitToScreen(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH), true);
            } else {
                // Vulkan: draw calls were recorded into the command buffer opened
                // in ProcessDList.  End the frame and show the swapchain image.
                // If no ProcessDList was called this VI period (hadVkFrameBegun==false),
                // skip presenting — the previous frame's image stays visible.
                // Presenting a blank clear frame here was causing 30 Hz flickering:
                // every other VI interrupt showed black instead of the rendered scene.
                if (hadVkFrameBegun) {
                    m_renderer->EndFrame();
                    m_renderer->Present();
                }
            }
            didRender = true;
        }
    } else if (useVIBlit && m_viWidth != 0) {
        // Compute output resolution from xscale/yscale
        uint32_t yscale = m_viYScale & 0xFFF;
        uint32_t baseHeight = m_sysInfo.isPAL ? 288u : 240u;
        uint32_t outHeight = (yscale > 0) ? (baseHeight * yscale) / 1024 : baseHeight;

        if (m_sysInfo.rdram && m_viBlitProgram && m_viWidth > 0 && outHeight > 0 && g_fbManager.IsGL()) {
            m_renderer->BeginFrame();

            // Set viewport/scissor AFTER BeginFrame (which may reset them)
            int32_t winW = 0, winH = 0;
            GetOutputSize(winW, winH);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, winW, winH);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            BlitVIFramebuffer(viType, m_viOrigin & 0x00FFFFFF, m_viWidth, outHeight);
            m_renderer->EndFrame();
            didRender = true;
        } else if (!g_fbManager.IsGL() && m_viWidth > 0 && outHeight > 0) {
            // Vulkan: no VI RDRAM blit implemented yet — present a clear frame
            // so the swapchain advances and the game loop is not stalled.
            if (!hadVkFrameBegun) m_renderer->BeginFrame();
            m_renderer->EndFrame();
            m_renderer->Present();
            didRender = true;
        }
    }

    if (!didRender) {
        if (g_fbManager.IsGL()) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            // Vulkan: no HLE frame was rendered and no VI blit path fired.
            // Only present if ProcessDList opened a frame this interrupt;
            // otherwise leave the swapchain image unchanged (no flicker).
            if (hadVkFrameBegun) {
                m_renderer->EndFrame();
                m_renderer->Present();
            }
        }
    }
    markStage(tCompose);

    // Reset HLE flag for next frame
    m_hleRenderedThisFrame = false;

    // ── Post-processing pass ─────────────────────────────────
    // Both render paths (VI blit and HLE BlitToScreen) output to the default
    // framebuffer. If post-processing has enabled effects, capture the default
    // FB into an intermediate texture, then run the effect chain back to FB 0.
    // GL-only: PostProcessingPipeline is not initialized on Vulkan.
    if (didRender && m_postProcess && m_postProcess->HasEnabledEffects() && g_fbManager.IsGL()) {
        int32_t ppW = 0, ppH = 0;
        GetOutputSize(ppW, ppH);

        // (Re)create intermediate FBO if window size changed
        if (m_ppFBO == 0 || m_ppWidth != ppW || m_ppHeight != ppH) {
            if (m_ppFBO) {
                glDeleteFramebuffers(1, &m_ppFBO);
                m_ppFBO = 0;
            }
            if (m_ppTex) {
                glDeleteTextures(1, &m_ppTex);
                m_ppTex = 0;
            }

            glCreateTextures(GL_TEXTURE_2D, 1, &m_ppTex);
            glTextureStorage2D(m_ppTex, 1, GL_RGBA8, ppW, ppH);
            glTextureParameteri(m_ppTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(m_ppTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            glCreateFramebuffers(1, &m_ppFBO);
            glNamedFramebufferTexture(m_ppFBO, GL_COLOR_ATTACHMENT0, m_ppTex, 0);

            m_ppWidth = ppW;
            m_ppHeight = ppH;
            m_postProcess->Resize(ppW, ppH);
        }

        // Capture default FB → intermediate
        glBlitNamedFramebuffer(0, m_ppFBO, 0, 0, ppW, ppH, 0, 0, ppW, ppH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Run post-process chain: intermediate tex → default FB 0
        m_postProcess->Apply(m_ppTex, 0, ppW, ppH);
    }
    markStage(tPost);

    // Render ImGui debug panels (on top of everything, before swap)
    g_imguiDebugUI.Render();
    markStage(tImgui);

    // Force default-framebuffer alpha to 1.0 so the Windows DWM compositor
    // never makes the window see-through (it reads the backbuffer alpha).
    if (g_fbManager.IsGL()) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    markStage(tAlpha);

    // Deferred swap: submit all GPU work now so the GPU can finish while
    // the CPU continues emulating.  The actual SwapBuffers is deferred to
    // the start of the next UpdateScreen call.
    if (g_fbManager.IsGL()) {
        glFlush();
    }
    m_swapPending = true;
    markStage(tFlush);

    // ── Frame for the launcher ──────────────────────────────────
    // A game drawn inside the launcher hands each frame over as RGBA pixels,
    // top row first, in g_framePixels. Vulkan does that itself when it presents
    // offscreen; on OpenGL the frame is read back from the hidden window here.
    // The launcher uploads the pixels to its own texture, since the two GL
    // contexts don't share objects.
    if (g_embeddedMode.load(std::memory_order_relaxed)) {
        if (g_fbManager.IsGL()) {
            int32_t cbW = 0, cbH = 0;
            GetOutputSize(cbW, cbH);
            if (cbW > 0 && cbH > 0) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                std::lock_guard<std::mutex> lk(g_frameMutex);
                const size_t row = static_cast<size_t>(cbW) * 4;
                g_framePixels.resize(row * static_cast<size_t>(cbH));
                glReadPixels(0, 0, cbW, cbH, GL_RGBA, GL_UNSIGNED_BYTE, g_framePixels.data());
                std::vector<uint8_t> rowBuffer(row); // GL rows are bottom-up
                for (int32_t y = 0; y < cbH / 2; ++y) {
                    uint8_t* top = g_framePixels.data() + static_cast<size_t>(y) * row;
                    uint8_t* bottom = g_framePixels.data() + static_cast<size_t>(cbH - 1 - y) * row;
                    std::memcpy(rowBuffer.data(), top, row);
                    std::memcpy(top, bottom, row);
                    std::memcpy(bottom, rowBuffer.data(), row);
                }
                g_frameW.store(cbW, std::memory_order_relaxed);
                g_frameH.store(cbH, std::memory_order_relaxed);
                g_frameReady.store(true, std::memory_order_release);
            }
        }
        if (g_frameCallback) g_frameCallback();
    }

    // ── HLE Framebuffer Writeback (GPU → RDRAM, ASYNC) ───────────
    // Majora's Mask (and other games) sample the previous frame from RDRAM
    // (e.g. for the pause menu blurred background, motion blur, etc.). When
    // we render with HLE the pixels live only in GPU memory, so the game
    // sees zeros and draws a black background. Copy the display framebuffer
    // back into the emulated RDRAM at the VI origin so the CPU sees real
    // image data when it reads the frame.
    //
    // Use the async PBO path: each frame we first try to consume the PBO
    // submitted earlier (typically by RDPProcessor::HandleSyncFull) — this is
    // non-blocking and incurs at most one frame of latency. We only submit a
    // new readback here as a fallback, when nothing is in flight and the
    // display FB still has unsynced GPU writes.
    double tFinish = 0, tBegin = 0;
    if (m_sysInfo.rdram && m_viOrigin && m_hleEverRendered && m_config.fbReadAccess) {
        // Throttle GPU→RDRAM readback. On Intel iGPUs the glMapBufferRange
        // call inside FinishAsyncReadback blocks ~10ms even with a fence,
        // because the driver doesn't actually finish the readback until the
        // map. Doing it every Nth frame cuts steady-state render time by ~75%
        // while still keeping RDRAM fresh enough for most games.
        static uint64_t s_readCounter = 0;
        const int interval = std::max(1, m_config.fbReadbackInterval);
        if ((s_readCounter++ % static_cast<uint64_t>(interval)) == 0) {
            auto t0 = std::chrono::high_resolution_clock::now();
            g_fbManager.FinishAsyncReadback(m_sysInfo.rdram);
            auto t1 = std::chrono::high_resolution_clock::now();
            tFinish = std::chrono::duration<double, std::milli>(t1 - t0).count();

            if (auto* displayFB = g_fbManager.GetDisplayFramebuffer()) {
                if (!g_fbManager.IsReadbackPending() && displayFB->gpuModified) {
                    g_fbManager.BeginAsyncReadback(displayFB);
                }
            }
            auto t2 = std::chrono::high_resolution_clock::now();
            tBegin = std::chrono::duration<double, std::milli>(t2 - t1).count();
        }
    }
    markStage(tRead);

    auto end = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
    m_frameStats.frameTimeMs = totalMs;
    m_frameStats.fps = (totalMs > 0.0) ? 1000.0 / totalMs : 0.0;

    // Diagnostic: log when UpdateScreen is exceptionally slow
    if (totalMs > 100.0) {
        LOG_WARN("UpdateScreen slow: total={:.1f}ms swap={:.1f}ms frame={}", totalMs, m_frameStats.swapTimeMs,
                 m_frameCount);
    }

    // Per-stage averages every 60 frames
    {
        static double aSwap = 0, aCompose = 0, aPost = 0, aImgui = 0, aAlpha = 0, aFlush = 0, aRead = 0, aFin = 0,
                      aBeg = 0, aTotal = 0;
        static int aFrames = 0;
        aSwap += tSwap;
        aCompose += tCompose;
        aPost += tPost;
        aImgui += tImgui;
        aAlpha += tAlpha;
        aFlush += tFlush;
        aRead += tRead;
        aFin += tFinish;
        aBeg += tBegin;
        aTotal += totalMs;
        if (++aFrames >= 60) {
            Log::infof("RENDER[60f avg ms] total=%.2f | swap=%.2f compose=%.2f post=%.2f imgui=%.2f alpha=%.2f "
                       "flush=%.2f read=%.2f (fin=%.2f beg=%.2f)",
                       aTotal / 60, aSwap / 60, aCompose / 60, aPost / 60, aImgui / 60, aAlpha / 60, aFlush / 60,
                       aRead / 60, aFin / 60, aBeg / 60);
            aSwap = aCompose = aPost = aImgui = aAlpha = aFlush = aRead = aFin = aBeg = aTotal = 0;
            aFrames = 0;
        }
    }
}

void VideoPlugin::ShowCFB() {
    if (!m_renderer) return; // video disabled (no GL context) — don't touch GL
    // Color Frame Buffer display — used when the CPU writes directly to the frame buffer
    // without going through the RDP (e.g., for menus, 2D graphics).
    if (!m_sysInfo.rdram || !m_sysInfo.viOriginReg) return;

    UpdateVIRegisters();

    uint32_t viType = m_viStatus & 0x3;
    bool viActive = (viType == 2 || viType == 3);
    if (!viActive || m_viOrigin == 0 || m_viWidth == 0) {
        LOG_DEBUG("ShowCFB: VI not active (type={}, origin=0x{:06X}, width={})", viType, m_viOrigin, m_viWidth);
        return;
    }

    uint32_t xscale = m_viXScale & 0xFFF;
    uint32_t yscale = m_viYScale & 0xFFF;
    uint32_t baseHeight = m_sysInfo.isPAL ? 288u : 240u;
    uint32_t outHeight = (yscale > 0) ? (baseHeight * yscale) / 1024 : baseHeight;
    (void)xscale; // xscale not needed here — fbWidth comes from VI width register

    if (!m_viBlitProgram || outHeight == 0) return;

    m_renderer->BeginFrame();
    BlitVIFramebuffer(viType, m_viOrigin & 0x00FFFFFF, m_viWidth, outHeight);
    m_renderer->EndFrame();

    if (g_fbManager.IsGL()) {
        if (auto* win = GetActiveWindow()) {
            win->SwapBuffers();
        }
    } else {
        m_renderer->Present();
    }

    LOG_DEBUG("ShowCFB: displayed origin=0x{:06X} {}x{}", m_viOrigin, m_viWidth, outHeight);
}

// ─────────────────────────────────────────────────────────────────
//  Shared VI framebuffer decode + blit helper
// ─────────────────────────────────────────────────────────────────

void VideoPlugin::BlitVIFramebuffer(uint32_t viType, uint32_t fbAddr, uint32_t fbWidth, uint32_t fbHeight) {
    if (!m_sysInfo.rdram || !m_viBlitProgram || fbWidth == 0 || fbHeight == 0) return;

    uint32_t bpp = (viType == 3) ? 4 : 2;
    uint32_t fbSize = fbWidth * fbHeight * bpp;

    if (fbAddr + fbSize > m_sysInfo.rdramSize) return;

    const uint8_t* src = m_sysInfo.rdram + fbAddr;

    // Decode N64 framebuffer to RGBA8888
    std::vector<uint32_t> pixels(fbWidth * fbHeight);

    if (viType == 3) {
        // 32-bit RGBA8888: RDRAM stores words as native LE uint32_t (SW has no XOR).
        // N64 pixel word: R[31:24] G[23:16] B[15:8] A[7:0]
        for (uint32_t i = 0; i < fbWidth * fbHeight; ++i) {
            uint32_t word = *reinterpret_cast<const uint32_t*>(src + i * 4);
            uint8_t r = (word >> 24) & 0xFF;
            uint8_t g = (word >> 16) & 0xFF;
            uint8_t b = (word >> 8) & 0xFF;
            uint8_t a = word & 0xFF;
            pixels[i] = (a << 24) | (b << 16) | (g << 8) | r; // ABGR for GL
        }
    } else {
        // 16-bit RGBA5551: RDRAM stores halfwords with XOR 2 (SH_Memory).
        // Read back with matching XOR 2 to recover the original pixel value.
        for (uint32_t i = 0; i < fbWidth * fbHeight; ++i) {
            uint16_t pixel = *reinterpret_cast<const uint16_t*>(src + ((i * 2) ^ 2));
            uint8_t r = static_cast<uint8_t>((pixel >> 11) & 0x1F) << 3;
            uint8_t g = static_cast<uint8_t>((pixel >> 6) & 0x1F) << 3;
            uint8_t b = static_cast<uint8_t>((pixel >> 1) & 0x1F) << 3;
            uint8_t a = (pixel & 1) ? 0xFF : 0x00;
            pixels[i] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }

    // Create or resize the VI texture
    if (m_viTexture && (m_viTexture->GetWidth() != fbWidth || m_viTexture->GetHeight() != fbHeight)) {
        m_viTexture.reset(); // Dimensions changed — recreate
    }
    if (!m_viTexture) {
        TextureDesc desc;
        desc.width = fbWidth;
        desc.height = fbHeight;
        desc.format = TextureFormat::RGBA8;
        desc.usage = BufferUsage::Dynamic;
        desc.mipLevels = 1;
        m_viTexture = m_renderer->CreateTexture(desc);
        // Use nearest-neighbor for crisp pixel-perfect VI output
        if (m_viTexture) {
            SamplerDesc sampler;
            sampler.minFilter = SamplerFilter::Nearest;
            sampler.magFilter = SamplerFilter::Nearest;
            sampler.wrapS = TextureWrap::ClampToEdge;
            sampler.wrapT = TextureWrap::ClampToEdge;
            m_viTexture->SetSampler(sampler);
        }
    }

    if (m_viTexture) {
        m_viTexture->Upload(pixels.data(), fbWidth * fbHeight * 4);
    }

    // Draw fullscreen blit
    // IMPORTANT: Always bind the default framebuffer (FBO 0) before drawing.
    // Without this, the draw targets whatever FBO is currently bound, which
    // is the game's color FBO after ProcessDList — corrupting its content.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(m_viBlitProgram);

    // Crop 4 native overscan pixels from each edge (same as HLE blit path)
    {
        float fw = static_cast<float>(fbWidth);
        float fh = static_cast<float>(fbHeight);
        constexpr float kCrop = 4.0f;
        float cropU0 = (fw > kCrop) ? (kCrop / fw) : 0.0f;
        float cropV0 = (fh > kCrop) ? (kCrop / fh) : 0.0f;
        float cropU1 = (fw > kCrop) ? ((fw - kCrop) / fw) : 1.0f;
        float cropV1 = (fh > kCrop) ? ((fh - kCrop) / fh) : 1.0f;
        GLint cropLoc = glGetUniformLocation(m_viBlitProgram, "uCropRect");
        if (cropLoc >= 0) glUniform4f(cropLoc, cropU0, cropV0, cropU1, cropV1);
    }

    glBindVertexArray(m_viBlitVAO);
    if (m_viTexture) {
        m_renderer->BindTexture(0, m_viTexture.get());
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

// ═══════════════════════════════════════════════════════════════
//  VI Register Changes
// ═══════════════════════════════════════════════════════════════

void VideoPlugin::ViStatusChanged() {
    uint32_t newStatus = ReadVIRegister(m_sysInfo.viStatusReg);
    if (newStatus == m_viStatus) return;

    uint32_t oldType = m_viStatus & 0x3;
    uint32_t newType = newStatus & 0x3;

    m_viStatus = newStatus;

    if (oldType != newType) {
        LOG_DEBUG("VI type changed: {} -> {} ({}bpp)", oldType, newType, newType == 2 ? 16 : newType == 3 ? 32 : 0);
    }
}

void VideoPlugin::ViWidthChanged() {
    uint32_t newWidth = ReadVIRegister(m_sysInfo.viWidthReg);
    if (newWidth == m_viWidth) return;

    LOG_DEBUG("VI width changed: {} -> {}", m_viWidth, newWidth);
    m_viWidth = newWidth;
}

void VideoPlugin::UpdateVIRegisters() {
    m_viStatus = ReadVIRegister(m_sysInfo.viStatusReg);
    m_viOrigin = ReadVIRegister(m_sysInfo.viOriginReg) & 0x00FFFFFF;
    m_viWidth = ReadVIRegister(m_sysInfo.viWidthReg);
    m_viXScale = ReadVIRegister(m_sysInfo.viXScaleReg);
    m_viYScale = ReadVIRegister(m_sysInfo.viYScaleReg);
}

uint32_t VideoPlugin::ReadVIRegister(uint32_t* reg) const { return reg ? *reg : 0; }

bool VideoPlugin::IsFramebufferValid(const uint8_t* fbData, uint32_t width, uint32_t height, uint32_t bpp) const {
    if (!fbData || width == 0 || height == 0) return false;

    // Quick heuristic: Check first 100 pixels to see if it's completely empty.
    // If it's entirely filled with 0s, we consider it uninitialized to prevent
    // garbage display when the grace period ends too early.
    uint32_t zeroCount = 0;
    uint32_t pixelsToCheck = std::min(width * height, 100u);
    for (uint32_t i = 0; i < pixelsToCheck; i++) {
        if (bpp == 2) {
            if (fbData[i * 2] == 0 && fbData[i * 2 + 1] == 0) zeroCount++;
        } else {
            if (fbData[i * 4] == 0 && fbData[i * 4 + 1] == 0 && fbData[i * 4 + 2] == 0 && fbData[i * 4 + 3] == 0)
                zeroCount++;
        }
    }

    // Also a fully zeroed frame is technically just black, delaying it rendering
    // just keeps the screen black. So rejecting it is safe.
    if (zeroCount == pixelsToCheck) return false;

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  ROM Events
// ═══════════════════════════════════════════════════════════════

void VideoPlugin::RomOpen(const SystemInfo& sysInfo) {
    m_sysInfo = sysInfo;
    m_romLoaded = true;

    LOG_INFO("ROM loaded: {} [{}] CRC:{:08X}/{:08X}", sysInfo.romName, sysInfo.romGameId, sysInfo.romCRC1,
             sysInfo.romCRC2);

    // Re-initialize subsystems with new RDRAM pointers
    if (m_rdpProcessor) {
        m_rdpProcessor->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize, m_renderer.get(), m_textureCache.get(),
                                   m_shaderManager.get());
        m_rdpProcessor->SetMLEDirector(&m_mleDirector);
    }
    if (m_dispatcher && m_rdpProcessor) {
        m_dispatcher->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize, &m_rdpProcessor->GetVideoStateMutable().rdp,
                                 m_rdpProcessor.get());
    }

    // Depth-buffer texture rectangles mean different things in different
    // games; enable each reading only for the games GLideN64 enables it for.
    if (m_rdpProcessor) {
        std::string name = sysInfo.romName;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        auto found = [&](const char* part) { return name.find(part) != std::string::npos; };
        auto& hacks = m_rdpProcessor->m_hacks;
        hacks.texRectShadowMap = found("BEETLE") || found("HSV") || found("DUCK DODGERS") || found("DAFFY DUCK");
        hacks.texRectDepthBufferCopy =
            !hacks.texRectShadowMap && (found("PERFECT DARK") || found("TUROK_DINOSAUR_HUNTE"));
    }

    // Look up game profile
    auto& db = GameDatabase::Instance();
    m_activeProfile = db.FindByGameId(sysInfo.romGameId);
    if (!m_activeProfile) {
        m_activeProfile = db.FindByCRC(sysInfo.romCRC1, sysInfo.romCRC2);
    }

    if (m_activeProfile) {
        LOG_INFO("Game profile found: {} [{}]", m_activeProfile->name, CompatRatingName(m_activeProfile->rating));
        ApplyGameSpecificHacks();
    } else {
        LOG_INFO("No game profile found, using defaults");
    }

    SetupTexturePack();

    // Reset frame state
    m_frameStats = {};
    m_frameCount = 0;
    m_dlistIndex = 0;

    // Start initialization grace period
    m_framesAfterRomOpen = 0;
    m_isInitializationPhase = true;

    // Optional: Clear any uninitialized RDRAM at the VI origin or just wait
    // We will handle skipping the render in UpdateScreen and BlitVIFramebuffer
}

// The folder a game's textures live in is named after its internal ROM name, as Rice Video and
// GLideN64 name theirs, so packs made for those work unchanged.
static std::string TexturePackName(const std::string& romName) {
    std::string name;
    for (char c : romName) name += (std::strchr("\\/:*?\"<>|", c) != nullptr) ? '_' : c;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return name;
}

// `<root>/<game>`, matching the folder name without regard to case.
static std::filesystem::path FindGameFolder(const std::filesystem::path& root, const std::string& game) {
    std::error_code ec;
    if (game.empty() || !std::filesystem::is_directory(root, ec)) return {};
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string wanted = lower(game);
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        if (entry.is_directory(ec) && lower(entry.path().filename().string()) == wanted) return entry.path();
    return {};
}

void VideoPlugin::SetupTexturePack() {
    if (!m_textureCache) return;
    const std::string game = TexturePackName(m_sysInfo.romName);

    std::shared_ptr<Tex::HiResPack> pack;
    if (m_config.hiResTexEnabled) {
        const std::filesystem::path folder =
            FindGameFolder(m_config.hiResTexPath.empty() ? "hires_texture" : m_config.hiResTexPath, game);
        if (!folder.empty()) {
            pack = std::make_shared<Tex::HiResPack>();
            if (pack->Load(folder)) {
                LOG_INFO("Texture pack: {} pictures from {}", pack->Count(), folder.string());
            } else {
                LOG_INFO("Texture pack folder {} holds no pictures in the Rice format", folder.string());
                pack.reset();
            }
        }
    }
    m_textureCache->SetHiResPack(pack);

    const char* dumpEnv = std::getenv("NOVA64_DUMP_TEXTURES");
    const bool dump = (m_config.textureDump || (dumpEnv && dumpEnv[0] && dumpEnv[0] != '0')) && !game.empty();
    if (dump) {
        const std::filesystem::path folder =
            std::filesystem::path(m_config.textureDumpPath.empty() ? "texture_dump" : m_config.textureDumpPath) / game;
        LOG_INFO("Dumping textures to {}", folder.string());
        m_textureCache->SetDumpFolder(folder, game);
    } else {
        m_textureCache->SetDumpFolder({}, game);
    }
    if (m_rdpProcessor) m_rdpProcessor->SetHiResHashing(pack != nullptr || dump);
}

void VideoPlugin::RomClosed() {
    LOG_INFO("ROM closed");
    m_romLoaded = false;
    m_activeProfile = nullptr;
    Reset();
}

// ═══════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════

void VideoPlugin::ApplyConfig(const PluginConfig& config) {
    m_config = config;
    m_config.SaveToFile("settings/NovaGraphics.ini");

    // Live-apply settings that can change at runtime
    // Resolution changes require renderer recreation
    // Filtering changes are applied on next frame
    LOG_INFO("Configuration applied");
}

void VideoPlugin::OpenConfigDialog() {
    // Ensure the config file exists with current settings
    const std::string configPath = "settings/NovaGraphics.ini";
    m_config.SaveToFile(configPath);
    LOG_INFO("Opening configuration file: {}", configPath);

#ifdef _WIN32
    // Open the INI file in the user's default text editor
    ShellExecuteA(nullptr, "open", configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    LOG_INFO("Edit {} manually to change settings, then restart the plugin", configPath);
#endif
}

// ═══════════════════════════════════════════════════════════════
//  Debug Features
// ═══════════════════════════════════════════════════════════════

void VideoPlugin::EnableWireframe(bool enable) {
    m_wireframe = enable;
    LOG_INFO("Wireframe mode: {}", enable ? "ON" : "OFF");
}

void VideoPlugin::EnableTextureDebug(bool enable) {
    m_textureDebug = enable;
    LOG_INFO("Texture debug mode: {}", enable ? "ON" : "OFF");
}

void VideoPlugin::TakeScreenshot(const std::string& path) {
    if (!m_renderer) {
        LOG_ERROR("Cannot take screenshot — no renderer");
        return;
    }

    // Read back the current framebuffer as RGBA8
    uint32_t w = m_config.windowWidth;
    uint32_t h = m_config.windowHeight;
    std::vector<uint8_t> pixels(w * h * 4);
    m_renderer->ReadPixels(0, 0, w, h, TextureFormat::RGBA8, pixels.data());

    // Flip vertically (OpenGL produces bottom-up images)
    uint32_t rowBytes = w * 4;
    std::vector<uint8_t> rowBuf(rowBytes);
    for (uint32_t y = 0; y < h / 2; ++y) {
        uint8_t* top = pixels.data() + y * rowBytes;
        uint8_t* bot = pixels.data() + (h - 1 - y) * rowBytes;
        std::memcpy(rowBuf.data(), top, rowBytes);
        std::memcpy(top, bot, rowBytes);
        std::memcpy(bot, rowBuf.data(), rowBytes);
    }

    // Write as TGA (always available, no external deps)
    // TGA header
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    FILE* fp = fopen(path.c_str(), "wb");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!fp) {
        LOG_ERROR("Screenshot failed — cannot open {}", path);
        return;
    }
    uint8_t header[18] = {};
    header[2] = 2; // Uncompressed RGB
    header[12] = w & 0xFF;
    header[13] = (w >> 8) & 0xFF;
    header[14] = h & 0xFF;
    header[15] = (h >> 8) & 0xFF;
    header[16] = 32;   // 32 bpp
    header[17] = 0x20; // Top-left origin
    fwrite(header, 1, 18, fp);

    // Convert RGBA to BGRA for TGA
    for (uint32_t i = 0; i < w * h; ++i) {
        uint8_t bgra[4] = {pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4 + 0], pixels[i * 4 + 3]};
        fwrite(bgra, 1, 4, fp);
    }
    fclose(fp);

    LOG_INFO("Screenshot saved to: {}", path);
}

void VideoPlugin::DumpFrameInfo() {
    LOG_INFO("=== Frame {} Info ===", m_frameCount);
    LOG_INFO("DLists: {} | RDP cmds: {} | Tris: {} | Rects: {}", m_frameStats.dlistCount, m_frameStats.rdpCommandCount,
             m_frameStats.triangleCount, m_frameStats.rectCount);
    LOG_INFO("Frame: {:.2f}ms | DList: {:.2f}ms | Render: {:.2f}ms | Swap: {:.2f}ms", m_frameStats.frameTimeMs,
             m_frameStats.dlistTimeMs, m_frameStats.renderTimeMs, m_frameStats.swapTimeMs);
}

PluginFrameStats VideoPlugin::GetFrameStats() const { return m_frameStats; }

void VideoPlugin::LockRender() { m_glMutex.lock(); }

void VideoPlugin::UnlockRender() { m_glMutex.unlock(); }

// ═══════════════════════════════════════════════════════════════
//  Internal: Renderer & Window
// ═══════════════════════════════════════════════════════════════

bool VideoPlugin::CreateDisplayWindow() {
    WindowConfig wc;
    wc.width = m_config.windowWidth;
    wc.height = m_config.windowHeight;
    wc.title = "NovaStar64 - NovaGraphics";
    wc.resizable = true;

    // A game drawn inside the launcher has no visible window. Vulkan renders
    // offscreen without one; OpenGL still needs a context, so it gets a hidden
    // window the size of the frames the launcher shows.
    if (g_embeddedMode.load(std::memory_order_relaxed)) {
        if (m_config.backendType == 1) return true;
        int32_t w = 0, h = 0;
        GetOutputSize(w, h);
        wc.width = w;
        wc.height = h;
        wc.headless = true;
    }

    m_window = std::make_unique<WindowManager>();
    if (!m_window->Initialize(wc)) {
        LOG_ERROR("WindowManager init failed");
        m_window.reset();
        return false;
    }

    return true;
}

// Frames are rendered at the window's drawable size. With no window (Vulkan
// drawing a game inside the launcher, or before the hidden GL window exists)
// the size is N64 resolution times the internal scale.
void VideoPlugin::GetOutputSize(int32_t& width, int32_t& height) const {
    if (auto* win = GetActiveWindow()) {
        win->GetDrawableSize(width, height);
        return;
    }
    const int scale = std::clamp(m_config.internalResScale, 1, 8);
    height = 240 * scale;
    width = (m_config.aspectRatio == AspectRatio::Widescreen16_9) ? height * 16 / 9 : 320 * scale;
}

bool VideoPlugin::CreateDisplayRenderer() {
    BackendType backend = BackendType::OpenGL46;

    // Use requested backend from configuration if available
    int cfgBackend = m_config.backendType;
    if (cfgBackend == 1) {
        backend = BackendType::Vulkan;
    }

    g_fbManager.SetIsGL(backend == BackendType::OpenGL46);
    m_renderer = Nova64::Video::CreateRenderer(backend);
    if (!m_renderer) {
        LOG_ERROR("Failed to create {} renderer", GetBackendName(backend));
        return false;
    }
    LOG_INFO("Created {} renderer", GetBackendName(backend));
    return true;
}

void VideoPlugin::DestroyDisplayRenderer() {
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    LOG_DEBUG("Renderer destroyed");
}

void VideoPlugin::DestroyDisplayWindow() {
    // Only destroy the window we own — never the external one
    m_externalWindow = nullptr;
    if (m_window) {
        m_window->Shutdown();
        m_window.reset();
    }
}

void VideoPlugin::ApplyGameSpecificHacks() {
    if (!m_activeProfile) return;

    using namespace VideoFlags;
    const auto& prof = *m_activeProfile;

    // ── Framebuffer flags ────────────────────────────────────
    if (prof.flags & CFBReadWrite) {
        LOG_INFO("  Enabling CFB read/write emulation");
        m_config.fbReadAccess = true;
        m_config.fbWriteAccess = true;
    }
    if (prof.flags & NoFBEmulation) {
        LOG_INFO("  Disabling FB emulation");
        m_config.fbEmulation = false;
    }
    if (prof.flags & ForceNativeRes) {
        LOG_INFO("  Forcing native resolution");
        m_config.internalResScale = 1;
    }
    if (prof.flags & HalfResVI) {
        LOG_INFO("  Using half-resolution VI");
        // Reduce effective internal res by half (minimum 1)
        m_config.internalResScale = std::max(1, m_config.internalResScale / 2);
    }
    if (prof.flags & NoDepthCompare) {
        LOG_INFO("  Disabling depth compare");
        m_config.depthCompare = false;
        m_config.n64DepthCompare = false;
    }

    // ── Texture flags → TextureCacheConfig ──────────────────
    if (m_textureCache) {
        Tex::TextureCacheConfig tcCfg = m_textureCache->GetConfig();
        bool texCfgChanged = false;

        if (prof.flags & Force16BitTex) {
            LOG_INFO("  Forcing 16-bit textures");
            tcCfg.force16bpp = true;
            texCfgChanged = true;
        }
        if (prof.flags & NoMipmaps) {
            LOG_INFO("  Disabling mipmaps");
            tcCfg.enableMipmaps = false;
            m_config.enableMipmaps = false;
            texCfgChanged = true;
        }
        if (prof.flags & PointSampleTextures) {
            LOG_INFO("  Forcing point-sample textures");
            tcCfg.enable3PointFilter = false;
            tcCfg.forceLinearFilter = false;
            texCfgChanged = true;
        }
        if (prof.flags & DisableHiResTex) {
            LOG_INFO("  Disabling hi-res texture packs");
            tcCfg.enableHiResTextures = false;
            m_config.hiResTexEnabled = false;
            texCfgChanged = true;
        }
        if (prof.textureFilter >= 0) {
            LOG_INFO("  Overriding texture filter to {}", prof.textureFilter);
            if (prof.textureFilter == 0) {
                tcCfg.enable3PointFilter = false;
                tcCfg.forceLinearFilter = false;
            } else if (prof.textureFilter == 1) {
                tcCfg.enable3PointFilter = false;
                tcCfg.forceLinearFilter = true;
            } else {
                tcCfg.enable3PointFilter = true;
                tcCfg.forceLinearFilter = false;
            }
            texCfgChanged = true;
        }
        if (texCfgChanged) {
            m_textureCache->SetConfig(tcCfg);
        }
    }

    // ── Blending / rendering flags ──────────────────────────
    if (prof.flags & AccurateBlending) {
        LOG_INFO("  Enabling accurate blending");
        m_config.accurateBlending = true;
    }
    if (prof.flags & ForceFogDisable) {
        LOG_INFO("  Disabling fog");
        m_config.fogEnabled = false;
    }
    if (prof.flags & NoAlphaTest) {
        LOG_INFO("  Disabling alpha test");
        m_config.forceNoAlphaTest = true;
    }

    // ── Numerical tweaks ────────────────────────────────────
    if (prof.textureLodBias != 0.0f) {
        LOG_INFO("  Texture LOD bias: {:.2f}", prof.textureLodBias);
        m_config.lodBias = prof.textureLodBias;
        if (m_textureCache) {
            Tex::TextureCacheConfig tcCfg2 = m_textureCache->GetConfig();
            tcCfg2.lodBias = prof.textureLodBias;
            m_textureCache->SetConfig(tcCfg2);
        }
    }
    if (prof.fogAdjust != 1.0f) {
        LOG_INFO("  Fog adjust: {:.2f}", prof.fogAdjust);
        m_config.fogAdjust = prof.fogAdjust;
    }
    if (prof.aspectRatioOverride > 0.0f) {
        LOG_INFO("  Aspect ratio override: {:.3f}", prof.aspectRatioOverride);
        m_config.stretchFactor = prof.aspectRatioOverride;
    }

    // ── Performance flags ───────────────────────────────────
    if (prof.flags & DisableAsync) {
        LOG_INFO("  Disabling async texture loads");
        m_config.asyncTextureLoading = false;
    }

    // ── Game-specific hacks ─────────────────────────────────
    if (prof.zelda64Hack) {
        LOG_INFO("  Applying Zelda 64 hacks (FB copy + depth bias)");
        m_config.fbReadAccess = true;
        m_config.depthCopy = true;
    }
    if (prof.perfectDarkHack) {
        LOG_INFO("  Applying Perfect Dark hacks (CFB + native res FB)");
        m_config.fbReadAccess = true;
        m_config.fbWriteAccess = true;
        m_config.nativeResFB = true;
    }
    if (prof.conkerHack) {
        LOG_INFO("  Applying Conker hacks (FB emulation + accurate blend)");
        m_config.fbEmulation = true;
        m_config.fbReadAccess = true;
        m_config.fbWriteAccess = true;
    }
    if (prof.paperMarioHack) {
        LOG_INFO("  Applying Paper Mario hacks (alpha test fixup)");
        m_config.paperMarioAlphaHack = true;
    }
    if (prof.dkRacingHack) {
        LOG_INFO("  Applying DK Racing hacks (depth bias)");
        m_config.depthBiasHack = prof.depthBias != 0 ? prof.depthBias : -2;
    }

    // ── RDP Processor hack flags ────────────────────────────
    if (m_rdpProcessor) {
        m_rdpProcessor->m_hacks.skipTexRect = prof.winBackSkipTexrect;
        m_rdpProcessor->m_hacks.noDepthFrameBuffers = prof.noDepthFrameBuffers;
        m_rdpProcessor->m_hacks.doNotResetOtherMode = prof.doNotResetOtherMode;
        m_rdpProcessor->m_hacks.forceDepthClear = prof.perfectDarkDepthClear;
        m_rdpProcessor->m_hacks.tonyHawkBlendFix = prof.tonyHawkBlendFix;
        m_rdpProcessor->m_hacks.blastCorpsBlendFix = prof.blastCorpsBlendFix;
        m_rdpProcessor->m_hacks.zeldaMonochrome = prof.zeldaMonochrome;
        m_rdpProcessor->m_hacks.zeldaSubscreen = prof.zeldaSubscreen;
        m_rdpProcessor->m_hacks.paperMarioSubscreen = prof.paperMarioSubscreen;
        m_rdpProcessor->m_hacks.conkerBlurPauseScreen = prof.conkerBlurPauseScreen;
        m_rdpProcessor->m_hacks.conkerFBTextureOffset = prof.conkerHack;
        m_rdpProcessor->m_hacks.conkerDepthCopySkip = prof.conkerHack;
        m_rdpProcessor->m_hacks.starCraftBgSkip = prof.starCraftBgSkip;
        m_rdpProcessor->m_hacks.snapForceCopy = prof.snapForceCopy;
        m_rdpProcessor->m_hacks.pdDepthCopyOnce = prof.perfectDarkHack;
    }

    // ── Framebuffer Manager hack flags ──────────────────────────
    g_fbManager.m_fbHacks.paperMarioBlueBoost = prof.paperMarioSubscreen;
    g_fbManager.m_fbHacks.conkerBlurDetect = prof.conkerHack;
    g_fbManager.m_fbHacks.pdDepthOncePerFrame = prof.perfectDarkHack;
    g_fbManager.m_fbHacks.starCraftBlockFB = prof.starCraftBgSkip;
    g_fbManager.m_fbHacks.snapSkipAuxCopy = prof.snapForceCopy;
}

} // namespace Nova64::Video
