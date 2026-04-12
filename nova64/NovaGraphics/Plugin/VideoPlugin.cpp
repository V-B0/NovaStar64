/**
 * @file VideoPlugin.cpp
 * @brief Video Plugin.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Video Plugin Implementation
// ══════════════════════════════════════════════════════════════════════════════

#include "VideoPlugin.hpp"
#include "../Common/Logger.hpp"
#include "../Debug/ImGuiDebugUI.hpp"
#include "../Renderer/GL/GLShaderCompiler.hpp"
#include "../Core/FramebufferManager.hpp"
#include "../Core/DepthBuffer.hpp"

#include "../../common/log.hpp"

#include <glad/gl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

namespace Nova64::Video {

const char* PluginStatusName(PluginStatus status) {
    switch (status) {
    case PluginStatus::Uninitialized: return "Uninitialized";
    case PluginStatus::Initialized:   return "Initialized";
    case PluginStatus::Running:       return "Running";
    case PluginStatus::Paused:        return "Paused";
    case PluginStatus::Error:         return "Error";
    case PluginStatus::ShuttingDown:  return "ShuttingDown";
    default: return "Unknown";
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

void VideoPlugin::SetExternalWindow(WindowManager* window) {
    m_externalWindow = window;
}

WindowManager* VideoPlugin::GetActiveWindow() const {
    return m_externalWindow ? m_externalWindow : m_window.get();
}

bool VideoPlugin::Initialize(const SystemInfo& sysInfo, const PluginCallbacks& callbacks) {
    if (m_status != PluginStatus::Uninitialized) {
        LOG_WARN("VideoPlugin already initialized (status={})", PluginStatusName(m_status));
        return false;
    }

    // Initialize NovaGraphics logger so LOG_INFO/WARN/ERROR actually produce output
    if (!g_logger.IsEnabled(LogLevel::Info, LogSubsystem::Core)) {
        LoggerConfig logCfg;
        logCfg.consoleOutput = true;
        logCfg.fileOutput    = false;
        logCfg.colorConsole  = true;
        logCfg.globalLevel   = LogLevel::Info;
        g_logger.Init(logCfg);
    }

    Log::info("NovaGraphics initializing...");
    LOG_INFO("NovaGraphics {} initializing...", m_version);
    m_sysInfo   = sysInfo;
    m_callbacks = callbacks;

    // Load configuration
    m_config.LoadDefaults();
    m_config.LoadFromFile("NovaGraphics.ini");

    // Apply CLI overrides (set by NovaGraphicsGFXPlugin before Initialize)
    if (m_pendingResScale > 0) {
        m_config.internalResScale = m_pendingResScale;
        m_config.windowWidth      = m_pendingWindowW;
        m_config.windowHeight     = m_pendingWindowH;
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
        int32_t winW = m_config.windowWidth, winH = m_config.windowHeight;
        if (auto* win = GetActiveWindow()) win->GetDrawableSize(winW, winH);
        if (!m_renderer->Initialize(nullptr,
                                     static_cast<uint32_t>(winW),
                                     static_cast<uint32_t>(winH))) {
            LOG_WARN("Renderer Initialize() failed — continuing with reduced GL state");
        }
    }

    // Create texture cache
    m_textureCache = std::make_unique<Tex::TextureCache>();
    {
        Tex::TextureCacheConfig tcConfig;
        m_textureCache->Initialize(m_renderer.get(), tcConfig);
    }

    // Create shader manager with GL compiler
    m_shaderManager = std::make_unique<Shader::ShaderManager>();
    {
        m_shaderManager->Initialize(Shader::BackendAPI::OpenGL);
        // Inject the OpenGL shader compiler
        auto glCompiler = std::make_unique<GL::GLShaderCompiler>();
        glCompiler->Initialize(Shader::BackendAPI::OpenGL);
        m_shaderManager->SetCompiler(std::move(glCompiler));
        std::filesystem::create_directories("ShaderCache");
        m_shaderManager->LoadCache("ShaderCache/NovaGraphics_shadercache.bin");
    }

    // Initialize RDP processor with real renderer, texture cache, and shader manager
    m_rdpProcessor = std::make_unique<RDPProcessor>();
    m_rdpProcessor->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize,
                               m_renderer.get(),
                               m_textureCache.get(),
                               m_shaderManager.get());

    // Wire SyncFull → DP interrupt. When a display list contains RDPFULLSYNC, the
    // N64 CPU expects the DP interrupt to fire at that point (not after the whole
    // display list). Using the checkInterrupts callback here delivers the interrupt
    // at the architecturally-correct moment, matching real N64 hardware timing.
    if (m_callbacks.checkInterrupts) {
        m_rdpProcessor->SetSyncFullCallback(m_callbacks.checkInterrupts);
    }

    // Create microcode dispatcher for HLE display list processing
    m_dispatcher = std::make_unique<MicrocodeDispatcher>();
    m_dispatcher->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize,
                            &m_rdpProcessor->GetVideoStateMutable().rdp,
                            m_rdpProcessor.get());

    // Initialize framebuffer manager (creates blit/filter GL resources)
    g_fbManager.Init(static_cast<uint32_t>(m_config.internalResScale));

    // Initialize depth buffer manager (N64-accurate depth compare)
    g_depthManager.Init();
    g_depthManager.InitZLUT();

    // Initialize post-processing pipeline
    {
        m_postProcess = std::make_unique<PostProcessingPipeline>();
        int32_t w = 640, h = 480;
        if (auto* win = GetActiveWindow()) win->GetDrawableSize(w, h);
        m_postProcess->Init(w, h);
        m_postProcess->SetupDefaultChain(); // Gamma + FXAA
        LOG_INFO("Post-processing pipeline initialized ({}x{})", w, h);
    }

    // Create VI blit resources (fullscreen triangle + simple texture blit shader)
    {
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
void main() {
    fragColor = vec4(texture(u_screenTexture, v_texCoord).rgb, 1.0);
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

    // Initialize ImGui debug UI with the SDL window and GL context
    if (auto* activeWin = GetActiveWindow()) {
        g_imguiDebugUI.Init(activeWin->GetSDLWindow(), activeWin->GetGLContext());
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

    // Shut down ImGui debug UI
    g_imguiDebugUI.Shutdown();

    // Save config
    m_config.SaveToFile("NovaGraphics.ini");

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

    // Destroy VI blit resources
    m_viTexture.reset();
    if (m_viBlitProgram) { glDeleteProgram(m_viBlitProgram); m_viBlitProgram = 0; }
    if (m_viBlitVAO) { glDeleteVertexArrays(1, &m_viBlitVAO); m_viBlitVAO = 0; }

    // Shut down depth buffer manager
    g_depthManager.Shutdown();

    // Destroy post-processing intermediate FBO
    if (m_ppFBO) { glDeleteFramebuffers(1, &m_ppFBO); m_ppFBO = 0; }
    if (m_ppTex) { glDeleteTextures(1, &m_ppTex); m_ppTex = 0; }
    m_ppWidth = 0; m_ppHeight = 0;

    DestroyDisplayRenderer();
    DestroyDisplayWindow();

    m_activeProfile = nullptr;
    m_romLoaded     = false;
    m_frameCount    = 0;
    m_dlistIndex    = 0;

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
    m_viStatus   = 0;
    m_viOrigin   = 0;
    m_viWidth    = 0;
    m_viXScale   = 0;
    m_viYScale   = 0;
}

// ═══════════════════════════════════════════════════════════════
//  Per-Frame Operations
// ═══════════════════════════════════════════════════════════════

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
        LOG_WARN("ProcessDList: taskDataPtr=0x{:08X} >= rdramSize=0x{:08X}",
                 taskDataPtr, m_sysInfo.rdramSize);
        return;
    }

    LOG_DEBUG("ProcessDList: taskDataPtr=0x{:08X} rdramSize=0x{:08X}",
              taskDataPtr, m_sysInfo.rdramSize);

    // Process the display list through the microcode interpreter
    if (m_dispatcher && m_rdpProcessor) {
        uint32_t ucodeAddr     = readDMEM_native(0xFD0) & 0x00FFFFFF; // Ucode
        uint32_t ucodeSize     = readDMEM_native(0xFD4);              // UcodeSize
        uint32_t ucodeDataAddr = readDMEM_native(0xFD8) & 0x00FFFFFF; // UcodeData
        uint32_t ucodeDataSize = readDMEM_native(0xFDC);              // UcodeDataSize
        uint32_t dlAddr        = taskDataPtr;

        // Notify the renderer of the window size so GetWidth()/GetHeight() are up to date.
        // Do NOT call glViewport here — OnSetColorImage (emitted by the DL) binds the correct
        // game FBO and sets glViewport to the FBO dimensions (320×240). Forcing 640×480 here
        // would map NDC 0..1 to window coords 320..640, clipping the right half of every draw.
        int32_t winW = 640, winH = 480;
        if (auto* w = GetActiveWindow()) w->GetDrawableSize(winW, winH);
        m_renderer->OnResize(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));
        glDisable(GL_SCISSOR_TEST);

        // Reset RDP state for this display list. Each display list should start with
        // clean state (fill color, blend mode, depth test flags, etc.), not inherit from the
        // previous frame.
        if (m_rdpProcessor) {
            m_rdpProcessor->Reset(false);  // warm reset: fresh RDP state, keep frame stats
        }

        m_dispatcher->ProcessGraphicsTask(
            taskDataPtr, ucodeAddr, ucodeSize,
            ucodeDataAddr, ucodeDataSize, dlAddr);

        // After the display list, flush any remaining RDP batch
        m_rdpProcessor->FlushBatch();

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
    if (!m_sysInfo.dpcStartReg || !m_sysInfo.dpcEndReg || !m_sysInfo.dpcCurrentReg) return;

    uint32_t startAddr   = *m_sysInfo.dpcStartReg & 0x00FFFFFF;
    uint32_t endAddr     = *m_sysInfo.dpcEndReg & 0x00FFFFFF;

    if (endAddr <= startAddr) return;
    if (endAddr > m_sysInfo.rdramSize) return;

    std::lock_guard<std::mutex> lock(m_glMutex);
    if (auto* win = GetActiveWindow()) win->MakeContextCurrent();

    // Read the RDRAM buffer as native 32-bit words (PJ64-style: RDRAM stores
    // words in host LE order, so a native uint32_t read gives the correct N64 value).
    uint32_t byteCount = endAddr - startAddr;
    int wordCount = static_cast<int>(byteCount / 4);

    m_rdpCmdScratch.resize(wordCount);
    const uint32_t* srcWords = reinterpret_cast<const uint32_t*>(m_sysInfo.rdram + startAddr);
    for (int i = 0; i < wordCount; ++i)
        m_rdpCmdScratch[i] = srcWords[i];

    m_rdpProcessor->ProcessCommandBuffer(m_rdpCmdScratch.data(), wordCount);
    m_frameStats.rdpCommandCount += static_cast<uint32_t>(wordCount / 2);

    // Update DPC current register
    *m_sysInfo.dpcCurrentReg = endAddr;

    // RDP commands were processed — mark this frame as HLE-rendered so
    // UpdateScreen will blit the game FBO instead of the VI RDRAM buffer.
    m_hleRenderedThisFrame = true;

    // Signal RDP interrupt
    if (m_callbacks.checkInterrupts) {
        m_callbacks.checkInterrupts();
    }
}

void VideoPlugin::ProcessRDPCommand(uint32_t w0, uint32_t w1) {
    if (!m_rdpProcessor) return;

    // Dispatch to RDP processor
    uint64_t cmd = (static_cast<uint64_t>(w0) << 32) | w1;
    m_rdpProcessor->ProcessRDPCommand(cmd);

    // Track statistics
    uint8_t opcode = (w0 >> 24) & 0x3F;
    switch (opcode) {
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        m_frameStats.triangleCount++;
        break;
    case 0x24: // Texture rectangle
    case 0x25: // Texture rectangle flip
    case 0x36: // Fill rectangle
        m_frameStats.rectCount++;
        break;
    }
}

void VideoPlugin::UpdateScreen() {
    auto start = std::chrono::high_resolution_clock::now();
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
        m_frameStats.swapTimeMs = std::chrono::duration<double, std::milli>(
            swapEnd - swapStart).count();
        m_swapPending = false;
    }

    m_frameCount++;
    m_frameStats.viInterrupts++;

    // Read VI registers
    UpdateVIRegisters();

    // Propagate VI config to FramebufferManager so GetDisplayFramebuffer() works.
    // Without this, m_viConfig.origin stays 0 and BlitToScreen() always returns early.
    {
        VIConfig viCfg;
        viCfg.origin  = m_viOrigin;
        viCfg.width   = m_viWidth;
        viCfg.xScale  = m_viXScale;
        viCfg.yScale  = m_viYScale;
        viCfg.control = m_viStatus;
        // Bits 8-9 = AA mode: 0/1 = AA enabled, 2 = resample only, 3 = replicate
        uint32_t aaMode = (m_viStatus >> 8) & 0x3;
        viCfg.antiAlias = (aaMode <= 1);
        viCfg.dedither  = (aaMode == 0);  // Only AA mode 0 enables dither removal
        viCfg.divot     = (m_viStatus & (1u << 4)) != 0;
        viCfg.gammaCorrect = (m_viStatus & (1u << 3)) != 0; // VI_STATUS gamma enable
        viCfg.gammaDither  = (m_viStatus & (1u << 2)) != 0; // VI_STATUS gamma dither
        viCfg.interlaced = (m_viStatus & (1u << 6)) != 0; // Serrate bit
        viCfg.field      = static_cast<uint32_t>(m_frameCount & 1);
        uint32_t yscale = m_viYScale & 0xFFF;
        uint32_t baseH  = m_sysInfo.isPAL ? 288u : 240u;
        viCfg.height    = (yscale > 0) ? (baseH * yscale) / 1024u : baseH;
        g_fbManager.UpdateVI(viCfg);
    }

    // Determine output dimensions from VI registers
    uint32_t viType   = m_viStatus & 0x3;
    bool     viActive = (viType == 2 || viType == 3); // 16-bit or 32-bit



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
        // After several frames without DLs, the game has likely transitioned
        // to a SW-rendered screen or scene reset.  Un-latch so the VI RDRAM
        // blit can take over (e.g. OoT attract-mode loop, file select).
        m_framesWithoutHLE++;
        if (m_framesWithoutHLE >= 3) {
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
        int32_t winW = 640, winH = 480;
        if (win) win->GetDrawableSize(winW, winH);
        g_fbManager.BlitToScreen(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH), true);
        didRender = true;
    } else if (useVIBlit && m_viWidth != 0) {
        // Compute output resolution from xscale/yscale
        uint32_t yscale = m_viYScale & 0xFFF;
        uint32_t baseHeight = m_sysInfo.isPAL ? 288u : 240u;
        uint32_t outHeight = (yscale > 0) ? (baseHeight * yscale) / 1024 : baseHeight;



        if (m_sysInfo.rdram && m_viBlitProgram && m_viWidth > 0 && outHeight > 0) {
            m_renderer->BeginFrame();

            // Set viewport/scissor AFTER BeginFrame (which may reset them)
            int32_t winW = 640, winH = 480;
            if (win) win->GetDrawableSize(winW, winH);
            glViewport(0, 0, winW, winH);
            glScissor(0, 0, winW, winH);
            glDisable(GL_SCISSOR_TEST);

            BlitVIFramebuffer(viType, m_viOrigin & 0x00FFFFFF, m_viWidth, outHeight);
            m_renderer->EndFrame();
            didRender = true;
        }
    }

    if (!didRender) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Reset HLE flag for next frame
    m_hleRenderedThisFrame = false;

    // ── Post-processing pass ─────────────────────────────────
    // Both render paths (VI blit and HLE BlitToScreen) output to the default
    // framebuffer. If post-processing has enabled effects, capture the default
    // FB into an intermediate texture, then run the effect chain back to FB 0.
    if (didRender && m_postProcess && m_postProcess->HasEnabledEffects()) {
        int32_t ppW = 640, ppH = 480;
        if (auto* ppWin = GetActiveWindow()) ppWin->GetDrawableSize(ppW, ppH);

        // (Re)create intermediate FBO if window size changed
        if (m_ppFBO == 0 || m_ppWidth != ppW || m_ppHeight != ppH) {
            if (m_ppFBO) { glDeleteFramebuffers(1, &m_ppFBO); m_ppFBO = 0; }
            if (m_ppTex) { glDeleteTextures(1, &m_ppTex); m_ppTex = 0; }

            glCreateTextures(GL_TEXTURE_2D, 1, &m_ppTex);
            glTextureStorage2D(m_ppTex, 1, GL_RGBA8, ppW, ppH);
            glTextureParameteri(m_ppTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(m_ppTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            glCreateFramebuffers(1, &m_ppFBO);
            glNamedFramebufferTexture(m_ppFBO, GL_COLOR_ATTACHMENT0, m_ppTex, 0);

            m_ppWidth = ppW;
            m_ppHeight = ppH; m_postProcess->Resize(ppW, ppH);
        }

        // Capture default FB → intermediate
        glBlitNamedFramebuffer(0, m_ppFBO,
                               0, 0, ppW, ppH,
                               0, 0, ppW, ppH,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Run post-process chain: intermediate tex → default FB 0
        m_postProcess->Apply(m_ppTex, 0, ppW, ppH);
    }

    // Render ImGui debug panels (on top of everything, before swap)
    g_imguiDebugUI.Render();

    // Force default-framebuffer alpha to 1.0 so the Windows DWM compositor
    // never makes the window see-through (it reads the backbuffer alpha).
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Deferred swap: submit all GPU work now so the GPU can finish while
    // the CPU continues emulating.  The actual SwapBuffers is deferred to
    // the start of the next UpdateScreen call.
    glFlush();
    m_swapPending = true;



    auto end = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
    m_frameStats.frameTimeMs = totalMs;
    m_frameStats.fps = (totalMs > 0.0) ? 1000.0 / totalMs : 0.0;

    // Diagnostic: log when UpdateScreen is exceptionally slow
    if (totalMs > 100.0) {
        LOG_WARN("UpdateScreen slow: total={:.1f}ms swap={:.1f}ms frame={}",
                 totalMs, m_frameStats.swapTimeMs, m_frameCount);
    }
}

void VideoPlugin::ShowCFB() {
    // Color Frame Buffer display — used when the CPU writes directly to the frame buffer
    // without going through the RDP (e.g., for menus, 2D graphics).
    if (!m_sysInfo.rdram || !m_sysInfo.viOriginReg) return;

    UpdateVIRegisters();

    uint32_t viType   = m_viStatus & 0x3;
    bool     viActive = (viType == 2 || viType == 3);
    if (!viActive || m_viOrigin == 0 || m_viWidth == 0) {
        LOG_DEBUG("ShowCFB: VI not active (type={}, origin=0x{:06X}, width={})",
                  viType, m_viOrigin, m_viWidth);
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

    if (auto* win = GetActiveWindow()) {
        win->SwapBuffers();
    }

    LOG_DEBUG("ShowCFB: displayed origin=0x{:06X} {}x{}", m_viOrigin, m_viWidth, outHeight);
}

// ─────────────────────────────────────────────────────────────────
//  Shared VI framebuffer decode + blit helper
// ─────────────────────────────────────────────────────────────────

void VideoPlugin::BlitVIFramebuffer(uint32_t viType, uint32_t fbAddr,
                                     uint32_t fbWidth, uint32_t fbHeight) {
    if (!m_sysInfo.rdram || !m_viBlitProgram || fbWidth == 0 || fbHeight == 0)
        return;

    uint32_t bpp    = (viType == 3) ? 4 : 2;
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
            uint8_t b = (word >>  8) & 0xFF;
            uint8_t a =  word        & 0xFF;
            pixels[i] = (a << 24) | (b << 16) | (g << 8) | r; // ABGR for GL
        }
    } else {
        // 16-bit RGBA5551: RDRAM stores halfwords with XOR 2 (SH_Memory).
        // Read back with matching XOR 2 to recover the original pixel value.
        for (uint32_t i = 0; i < fbWidth * fbHeight; ++i) {
            uint16_t pixel = *reinterpret_cast<const uint16_t*>(src + ((i * 2) ^ 2));
            uint8_t r = static_cast<uint8_t>((pixel >> 11) & 0x1F) << 3;
            uint8_t g = static_cast<uint8_t>((pixel >>  6) & 0x1F) << 3;
            uint8_t b = static_cast<uint8_t>((pixel >>  1) & 0x1F) << 3;
            uint8_t a = (pixel & 1) ? 0xFF : 0x00;
            pixels[i] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }

    // Create or resize the VI texture
    if (m_viTexture &&
        (m_viTexture->GetWidth() != fbWidth || m_viTexture->GetHeight() != fbHeight)) {
        m_viTexture.reset(); // Dimensions changed — recreate
    }
    if (!m_viTexture) {
        TextureDesc desc;
        desc.width  = fbWidth;
        desc.height = fbHeight;
        desc.format = TextureFormat::RGBA8;
        desc.usage  = BufferUsage::Dynamic;
        desc.mipLevels = 1;
        m_viTexture = m_renderer->CreateTexture(desc);
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
        LOG_DEBUG("VI type changed: {} -> {} ({}bpp)",
                  oldType, newType, newType == 2 ? 16 : newType == 3 ? 32 : 0);
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
    m_viWidth  = ReadVIRegister(m_sysInfo.viWidthReg);
    m_viXScale = ReadVIRegister(m_sysInfo.viXScaleReg);
    m_viYScale = ReadVIRegister(m_sysInfo.viYScaleReg);
}

uint32_t VideoPlugin::ReadVIRegister(uint32_t* reg) const {
    return reg ? *reg : 0;
}

bool VideoPlugin::IsFramebufferValid(const uint8_t* fbData, uint32_t width, uint32_t height, uint32_t bpp) const {
    if (!fbData || width == 0 || height == 0) return false;
    
    // Quick heuristic: Check first 100 pixels to see if it's completely empty.
    // If it's entirely filled with 0s, we consider it uninitialized to prevent 
    // garbage display when the grace period ends too early.
    uint32_t zeroCount = 0;
    uint32_t pixelsToCheck = std::min(width * height, 100u);
    for (uint32_t i = 0; i < pixelsToCheck; i++) {
        if (bpp == 2) {
            if (fbData[i*2] == 0 && fbData[i*2+1] == 0) zeroCount++;
        } else {
            if (fbData[i*4] == 0 && fbData[i*4+1] == 0 && fbData[i*4+2] == 0 && fbData[i*4+3] == 0) zeroCount++;
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

    LOG_INFO("ROM loaded: {} [{}] CRC:{:08X}/{:08X}",
             sysInfo.romName, sysInfo.romGameId, sysInfo.romCRC1, sysInfo.romCRC2);

    // Re-initialize subsystems with new RDRAM pointers
    if (m_rdpProcessor) {
        m_rdpProcessor->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize,
                                   m_renderer.get(),
                                   m_textureCache.get(),
                                   m_shaderManager.get());
    }
    if (m_dispatcher && m_rdpProcessor) {
        m_dispatcher->Initialize(m_sysInfo.rdram, m_sysInfo.rdramSize,
                                &m_rdpProcessor->GetVideoStateMutable().rdp,
                                m_rdpProcessor.get());
    }

    // Look up game profile
    auto& db = GameDatabase::Instance();
    m_activeProfile = db.FindByGameId(sysInfo.romGameId);
    if (!m_activeProfile) {
        m_activeProfile = db.FindByCRC(sysInfo.romCRC1, sysInfo.romCRC2);
    }

    if (m_activeProfile) {
        LOG_INFO("Game profile found: {} [{}]", 
                 m_activeProfile->name, CompatRatingName(m_activeProfile->rating));
        ApplyGameSpecificHacks();
    } else {
        LOG_INFO("No game profile found, using defaults");
    }

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
    m_config.SaveToFile("NovaGraphics.ini");

    // Live-apply settings that can change at runtime
    // Resolution changes require renderer recreation
    // Filtering changes are applied on next frame
    LOG_INFO("Configuration applied");
}

void VideoPlugin::OpenConfigDialog() {
    // Ensure the config file exists with current settings
    const std::string configPath = "NovaGraphics.ini";
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
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("Screenshot failed — cannot open {}", path);
        return;
    }
    uint8_t header[18] = {};
    header[2]  = 2;  // Uncompressed RGB
    header[12] = w & 0xFF; header[13] = (w >> 8) & 0xFF;
    header[14] = h & 0xFF; header[15] = (h >> 8) & 0xFF;
    header[16] = 32; // 32 bpp
    header[17] = 0x20; // Top-left origin
    fwrite(header, 1, 18, fp);

    // Convert RGBA to BGRA for TGA
    for (uint32_t i = 0; i < w * h; ++i) {
        uint8_t bgra[4] = { pixels[i*4+2], pixels[i*4+1], pixels[i*4+0], pixels[i*4+3] };
        fwrite(bgra, 1, 4, fp);
    }
    fclose(fp);

    LOG_INFO("Screenshot saved to: {}", path);
}

void VideoPlugin::DumpFrameInfo() {
    LOG_INFO("=== Frame {} Info ===", m_frameCount);
    LOG_INFO("DLists: {} | RDP cmds: {} | Tris: {} | Rects: {}",
             m_frameStats.dlistCount, m_frameStats.rdpCommandCount,
             m_frameStats.triangleCount, m_frameStats.rectCount);
    LOG_INFO("Frame: {:.2f}ms | DList: {:.2f}ms | Render: {:.2f}ms | Swap: {:.2f}ms",
             m_frameStats.frameTimeMs, m_frameStats.dlistTimeMs,
             m_frameStats.renderTimeMs, m_frameStats.swapTimeMs);
}

PluginFrameStats VideoPlugin::GetFrameStats() const {
    return m_frameStats;
}

void VideoPlugin::LockRender() {
    m_glMutex.lock();
}

void VideoPlugin::UnlockRender() {
    m_glMutex.unlock();
}

// ═══════════════════════════════════════════════════════════════
//  Internal: Renderer & Window
// ═══════════════════════════════════════════════════════════════

bool VideoPlugin::CreateDisplayWindow() {
    m_window = std::make_unique<WindowManager>();
    
    WindowConfig wc;
    wc.width     = m_config.windowWidth;
    wc.height    = m_config.windowHeight;
    wc.title     = "SuperNova64 - NovaGraphics";
    wc.resizable = true;

    if (!m_window->Initialize(wc)) {
        LOG_ERROR("WindowManager init failed");
        m_window.reset();
        return false;
    }

    return true;
}

bool VideoPlugin::CreateDisplayRenderer() {
    // The WindowManager creates an SDL+OpenGL context, so we must use the
    // OpenGL backend regardless of what DetectBestBackend() returns (it
    // would pick D3D11 on Windows, but D3D11 has no swap-chain wired to our
    // SDL window and would crash on the first GL call).
    BackendType backend = BackendType::OpenGL46;
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
        m_config.fbReadAccess  = true;
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
        m_config.depthCompare    = false;
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
            tcCfg.forceLinearFilter  = false;
            texCfgChanged = true;
        }
        if (prof.flags & DisableHiResTex) {
            LOG_INFO("  Disabling hi-res texture packs");
            tcCfg.enableHiResTextures = false;
            m_config.hiResTexEnabled  = false;
            texCfgChanged = true;
        }
        if (prof.textureFilter >= 0) {
            LOG_INFO("  Overriding texture filter to {}", prof.textureFilter);
            if (prof.textureFilter == 0) {
                tcCfg.enable3PointFilter = false;
                tcCfg.forceLinearFilter  = false;
            } else if (prof.textureFilter == 1) {
                tcCfg.enable3PointFilter = false;
                tcCfg.forceLinearFilter  = true;
            } else {
                tcCfg.enable3PointFilter = true;
                tcCfg.forceLinearFilter  = false;
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
        m_config.fbReadAccess  = true;
        m_config.depthCopy     = true;
    }
    if (prof.perfectDarkHack) {
        LOG_INFO("  Applying Perfect Dark hacks (CFB + native res FB)");
        m_config.fbReadAccess  = true;
        m_config.fbWriteAccess = true;
        m_config.nativeResFB   = true;
    }
    if (prof.conkerHack) {
        LOG_INFO("  Applying Conker hacks (FB emulation + accurate blend)");
        m_config.fbEmulation   = true;
        m_config.fbReadAccess  = true;
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
        m_rdpProcessor->m_hacks.skipTexRect            = prof.winBackSkipTexrect;
        m_rdpProcessor->m_hacks.noDepthFrameBuffers    = prof.noDepthFrameBuffers;
        m_rdpProcessor->m_hacks.doNotResetOtherMode    = prof.doNotResetOtherMode;
        m_rdpProcessor->m_hacks.forceDepthClear        = prof.perfectDarkDepthClear;
        m_rdpProcessor->m_hacks.tonyHawkBlendFix       = prof.tonyHawkBlendFix;
        m_rdpProcessor->m_hacks.blastCorpsBlendFix     = prof.blastCorpsBlendFix;
        m_rdpProcessor->m_hacks.zeldaMonochrome        = prof.zeldaMonochrome;
        m_rdpProcessor->m_hacks.zeldaSubscreen         = prof.zeldaSubscreen;
        m_rdpProcessor->m_hacks.paperMarioSubscreen    = prof.paperMarioSubscreen;
        m_rdpProcessor->m_hacks.conkerBlurPauseScreen  = prof.conkerBlurPauseScreen;
        m_rdpProcessor->m_hacks.conkerFBTextureOffset  = prof.conkerHack;
        m_rdpProcessor->m_hacks.conkerDepthCopySkip    = prof.conkerHack;
        m_rdpProcessor->m_hacks.starCraftBgSkip        = prof.starCraftBgSkip;
        m_rdpProcessor->m_hacks.snapForceCopy          = prof.snapForceCopy;
        m_rdpProcessor->m_hacks.pdDepthCopyOnce        = prof.perfectDarkHack;
    }

    // ── Framebuffer Manager hack flags ──────────────────────────
    g_fbManager.m_fbHacks.paperMarioBlueBoost  = prof.paperMarioSubscreen;
    g_fbManager.m_fbHacks.conkerBlurDetect     = prof.conkerHack;
    g_fbManager.m_fbHacks.pdDepthOncePerFrame  = prof.perfectDarkHack;
    g_fbManager.m_fbHacks.starCraftBlockFB     = prof.starCraftBgSkip;
    g_fbManager.m_fbHacks.snapSkipAuxCopy      = prof.snapForceCopy;
}

} // namespace Nova64::Video
