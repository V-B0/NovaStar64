/**
 * @file WindowManager.cpp
 * @brief Window Manager.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Window Manager Implementation
// SDL2 window, OpenGL context, input, display management

#include "WindowManager.hpp"
#include "../Debug/ImGuiDebugUI.hpp"
#include <SDL.h>
#include <SDL_syswm.h>
#include <glad/gl.h>
#include <iostream>
#include <cstring>

namespace Nova64 { namespace Video {

// ═══════════════════════════════════════════════════════════════════════════════
//  GL DEBUG CALLBACK
// ═══════════════════════════════════════════════════════════════════════════════

static void GLAD_API_PTR GLDebugCallback(GLenum source, GLenum type, GLuint id,
                                       GLenum severity, GLsizei length,
                                       const GLchar* message, const void* userParam) {
    (void)source; (void)type; (void)id; (void)length; (void)userParam;

    // Ignore non-significant notifications
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;

    const char* sevStr = "UNKNOWN";
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:   sevStr = "HIGH";   break;
        case GL_DEBUG_SEVERITY_MEDIUM: sevStr = "MEDIUM"; break;
        case GL_DEBUG_SEVERITY_LOW:    sevStr = "LOW";    break;
    }

    const char* typeStr = "UNKNOWN";
    switch (type) {
        case GL_DEBUG_TYPE_ERROR:               typeStr = "ERROR";       break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeStr = "DEPRECATED";  break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  typeStr = "UNDEFINED";   break;
        case GL_DEBUG_TYPE_PORTABILITY:         typeStr = "PORTABILITY"; break;
        case GL_DEBUG_TYPE_PERFORMANCE:         typeStr = "PERFORMANCE"; break;
        case GL_DEBUG_TYPE_OTHER:               typeStr = "OTHER";       break;
    }

    std::cerr << "[GL " << sevStr << "/" << typeStr << "] " << message << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════════

WindowManager::WindowManager() = default;

WindowManager::~WindowManager() {
    Shutdown();
}

bool WindowManager::Initialize(const WindowConfig& config) {
    m_config = config;

    // ── Initialize SDL Video ─────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    // ── Set OpenGL attributes ────────────────────────────────────────────
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, config.glMajor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, config.glMinor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    if (config.glDebug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);

    if (config.multiSamples > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config.multiSamples);
    }

    // ── Create window ────────────────────────────────────────────────────
    uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    // Force HighDPI off until UI layer is fixed for Windows scaling virtualization
    // if (config.highDPI) flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    if (config.fullscreen) {
        flags |= config.borderless ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
    }
    if (config.borderless && !config.fullscreen) {
        flags |= SDL_WINDOW_BORDERLESS;
    }

    int posX = config.posX >= 0 ? config.posX : SDL_WINDOWPOS_CENTERED;
    int posY = config.posY >= 0 ? config.posY : SDL_WINDOWPOS_CENTERED;

    m_window = SDL_CreateWindow(
        config.title.c_str(),
        posX, posY,
        config.width, config.height,
        flags
    );

    if (!m_window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return false;
    }

    // Save windowed position/size
    SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
    m_windowedW = config.width;
    m_windowedH = config.height;

    // ── Create OpenGL context ────────────────────────────────────────────
    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;

        // Try fallback to GL 4.3
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        m_glContext = SDL_GL_CreateContext(m_window);

        if (!m_glContext) {
            std::cerr << "Fallback GL 4.3 context also failed: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            SDL_Quit();
            return false;
        }
    }

    SDL_GL_MakeCurrent(m_window, m_glContext);
    m_glContextThread = std::this_thread::get_id(); // Track initial GL context thread

    // ── Load OpenGL via glad ─────────────────────────────────────────────
    int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!version) {
        std::cerr << "gladLoadGL failed" << std::endl;
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SDL_Quit();
        return false;
    }

    std::cout << "OpenGL " << GLAD_VERSION_MAJOR(version) << "." << GLAD_VERSION_MINOR(version)
              << " loaded" << std::endl;
    std::cout << "  Vendor:   " << glGetString(GL_VENDOR) << std::endl;
    std::cout << "  Renderer: " << glGetString(GL_RENDERER) << std::endl;
    std::cout << "  Version:  " << glGetString(GL_VERSION) << std::endl;
    std::cout << "  GLSL:     " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;

    // ── Enable GL debug output ───────────────────────────────────────────
    if (config.glDebug && GLAD_GL_KHR_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(GLDebugCallback, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    }

    // ── Set VSync ────────────────────────────────────────────────────────
    SetVSync(config.vsync ? 1 : 0);

    // ── Set initial GL state ─────────────────────────────────────────────
    glViewport(0, 0, config.width, config.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    m_fullscreen = config.fullscreen;
    m_hasFocus = true;
    m_minimized = false;

    return true;
}

void WindowManager::Shutdown() {
    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WINDOW PROPERTIES
// ═══════════════════════════════════════════════════════════════════════════════

void WindowManager::GetSize(int32_t& width, int32_t& height) const {
    if (m_window) {
        SDL_GetWindowSize(m_window, &width, &height);
    } else {
        width = m_config.width;
        height = m_config.height;
    }
}

void WindowManager::GetDrawableSize(int32_t& width, int32_t& height) const {
    if (m_window) {
        SDL_GL_GetDrawableSize(m_window, &width, &height);
    } else {
        GetSize(width, height);
    }
}

void WindowManager::SetSize(int32_t width, int32_t height) {
    if (m_window) {
        SDL_SetWindowSize(m_window, width, height);
    }
}

void WindowManager::GetPosition(int32_t& x, int32_t& y) const {
    if (m_window) {
        SDL_GetWindowPosition(m_window, &x, &y);
    }
}

void WindowManager::SetPosition(int32_t x, int32_t y) {
    if (m_window) {
        SDL_SetWindowPosition(m_window, x, y);
    }
}

void WindowManager::SetTitle(const std::string& title) {
    if (m_window) {
        SDL_SetWindowTitle(m_window, title.c_str());
    }
}

std::string WindowManager::GetTitle() const {
    if (m_window) {
        return SDL_GetWindowTitle(m_window);
    }
    return m_config.title;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DISPLAY MODES
// ═══════════════════════════════════════════════════════════════════════════════

void WindowManager::SetFullscreen(bool fullscreen, bool borderless) {
    if (!m_window) return;

    if (fullscreen) {
        // Save windowed state
        if (!m_fullscreen) {
            SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
            SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);
        }

        uint32_t flag = borderless ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
        SDL_SetWindowFullscreen(m_window, flag);
    } else {
        SDL_SetWindowFullscreen(m_window, 0);

        // Restore windowed state
        SDL_SetWindowPosition(m_window, m_windowedX, m_windowedY);
        SDL_SetWindowSize(m_window, m_windowedW, m_windowedH);
    }

    m_fullscreen = fullscreen;
}

std::vector<DisplayMode> WindowManager::GetDisplayModes(int32_t displayIndex) const {
    std::vector<DisplayMode> modes;
    int count = SDL_GetNumDisplayModes(displayIndex);

    for (int i = 0; i < count; i++) {
        SDL_DisplayMode sdlMode;
        if (SDL_GetDisplayMode(displayIndex, i, &sdlMode) == 0) {
            DisplayMode mode;
            mode.width = sdlMode.w;
            mode.height = sdlMode.h;
            mode.refreshRate = sdlMode.refresh_rate;
            mode.pixelFormat = sdlMode.format;
            modes.push_back(mode);
        }
    }

    return modes;
}

DisplayMode WindowManager::GetCurrentDisplayMode() const {
    DisplayMode result{};
    int displayIdx = m_window ? SDL_GetWindowDisplayIndex(m_window) : 0;

    SDL_DisplayMode sdlMode;
    if (SDL_GetCurrentDisplayMode(displayIdx, &sdlMode) == 0) {
        result.width = sdlMode.w;
        result.height = sdlMode.h;
        result.refreshRate = sdlMode.refresh_rate;
        result.pixelFormat = sdlMode.format;
    }

    return result;
}

int32_t WindowManager::GetDisplayCount() const {
    return SDL_GetNumVideoDisplays();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  GL CONTEXT
// ═══════════════════════════════════════════════════════════════════════════════

void WindowManager::MakeContextCurrent() {
    if (m_window && m_glContext) {
        SDL_GL_MakeCurrent(m_window, m_glContext);
    }
}

void WindowManager::ReleaseContext() {
    SDL_GL_MakeCurrent(m_window, nullptr);
}

void WindowManager::SwapBuffers() {
    if (m_window) {
        // Re-assert VSync=OFF before every swap. Intel drivers can
        // reset the swap interval internally; re-setting it each frame
        // prevents unexpected blocking.
        SDL_GL_SetSwapInterval(0);
        SDL_GL_SwapWindow(m_window);
    }
}

void WindowManager::SetVSync(int32_t interval) {
    // -1 = adaptive vsync, 0 = off, 1 = on
    if (SDL_GL_SetSwapInterval(interval) != 0) {
        // Adaptive vsync not supported, fall back to regular vsync
        if (interval == -1) {
            SDL_GL_SetSwapInterval(1);
        }
    }
}

void* WindowManager::GetNativeHandle() const {
#ifdef _WIN32
    if (m_window) {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(m_window, &wmInfo)) {
            return wmInfo.info.win.window;
        }
    }
#endif
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  EVENT PROCESSING
// ═══════════════════════════════════════════════════════════════════════════════

bool WindowManager::ProcessEvents() {
    m_pendingEvents.clear();
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        // Forward events to ImGui debug UI
        g_imguiDebugUI.ProcessEvent(event);

        // F7 toggles the ImGui debug panels
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F7) {
            g_imguiDebugUI.Toggle();
        }
        // F8 cycles combiner debug output mode
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F8) {
            g_imguiDebugUI.CycleDebugOutput();
        }

        HandleSDLEvent(&event);

        if (event.type == SDL_QUIT) {
            m_shouldClose = true;
        }
    }

    return !m_shouldClose;
}

void WindowManager::HandleSDLEvent(const void* sdlEventPtr) {
    const SDL_Event& event = *static_cast<const SDL_Event*>(sdlEventPtr);

    if (event.type == SDL_WINDOWEVENT) {
        WindowEventData data;

        switch (event.window.event) {
            case SDL_WINDOWEVENT_RESIZED:
                data.type = WindowEvent::Resized;
                data.param1 = event.window.data1;
                data.param2 = event.window.data2;
                // Note: glViewport is deferred to the render thread
                // (GL context may not be current on the event thread)
                break;

            case SDL_WINDOWEVENT_MINIMIZED:
                data.type = WindowEvent::Minimized;
                m_minimized = true;
                break;

            case SDL_WINDOWEVENT_RESTORED:
                data.type = WindowEvent::Restored;
                m_minimized = false;
                break;

            case SDL_WINDOWEVENT_MAXIMIZED:
                data.type = WindowEvent::Maximized;
                break;

            case SDL_WINDOWEVENT_FOCUS_GAINED:
                data.type = WindowEvent::FocusGained;
                m_hasFocus = true;
                break;

            case SDL_WINDOWEVENT_FOCUS_LOST:
                data.type = WindowEvent::FocusLost;
                m_hasFocus = false;
                break;

            case SDL_WINDOWEVENT_MOVED:
                data.type = WindowEvent::Moved;
                data.param1 = event.window.data1;
                data.param2 = event.window.data2;
                break;

            case SDL_WINDOWEVENT_CLOSE:
                data.type = WindowEvent::Closed;
                m_shouldClose = true;
                break;

            case SDL_WINDOWEVENT_EXPOSED:
                data.type = WindowEvent::Exposed;
                break;

            default:
                return; // Unhandled window event
        }

        m_pendingEvents.push_back(data);

        if (m_eventCallback) {
            m_eventCallback(data);
        }
    }

    // Handle keyboard shortcuts
    if (event.type == SDL_KEYDOWN) {
        // Alt+Enter = toggle fullscreen
        if (event.key.keysym.sym == SDLK_RETURN &&
            (event.key.keysym.mod & KMOD_ALT)) {
            SetFullscreen(!m_fullscreen, true);
        }

        // Escape = exit fullscreen
        if (event.key.keysym.sym == SDLK_ESCAPE && m_fullscreen) {
            SetFullscreen(false);
        }
    }
}

bool WindowManager::PollWindowEvent(WindowEventData& event) {
    if (m_pendingEvents.empty()) {
        event.type = WindowEvent::None;
        return false;
    }
    event = m_pendingEvents.front();
    m_pendingEvents.erase(m_pendingEvents.begin());
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SCREENSHOTS
// ═══════════════════════════════════════════════════════════════════════════════

bool WindowManager::CaptureFramebuffer(std::vector<uint32_t>& pixels, int32_t& width, int32_t& height) {
    GetDrawableSize(width, height);
    if (width <= 0 || height <= 0) return false;

    pixels.resize(width * height);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // Flip vertically (OpenGL reads bottom-to-top)
    for (int y = 0; y < height / 2; y++) {
        int topRow = y * width;
        int bottomRow = (height - 1 - y) * width;
        for (int x = 0; x < width; x++) {
            std::swap(pixels[topRow + x], pixels[bottomRow + x]);
        }
    }

    return true;
}

bool WindowManager::SaveScreenshot(const std::string& path) {
    int32_t w, h;
    std::vector<uint32_t> pixels;
    if (!CaptureFramebuffer(pixels, w, h)) return false;

    // Write BMP
    const uint32_t fileHeaderSize = 14;
    const uint32_t infoHeaderSize = 40;
    const uint32_t pixelDataOffset = fileHeaderSize + infoHeaderSize;
    const uint32_t rowSize = ((w * 3 + 3) / 4) * 4; // BMP rows are 4-byte aligned
    const uint32_t imageSize = rowSize * h;
    const uint32_t fileSize = pixelDataOffset + imageSize;

    std::vector<uint8_t> bmp(fileSize);

    // File header
    bmp[0] = 'B'; bmp[1] = 'M';
    memcpy(&bmp[2], &fileSize, 4);
    memcpy(&bmp[10], &pixelDataOffset, 4);

    // Info header
    memcpy(&bmp[14], &infoHeaderSize, 4);
    memcpy(&bmp[18], &w, 4);
    memcpy(&bmp[22], &h, 4);
    uint16_t planes = 1;
    memcpy(&bmp[26], &planes, 2);
    uint16_t bpp = 24;
    memcpy(&bmp[28], &bpp, 2);
    memcpy(&bmp[34], &imageSize, 4);

    // Pixel data (BGR, bottom-to-top — already flipped in CaptureFramebuffer)
    // Actually BMP is bottom-to-top, and we flipped to top-to-bottom, so flip back
    for (int y = 0; y < h; y++) {
        uint8_t* row = &bmp[pixelDataOffset + (h - 1 - y) * rowSize];
        for (int x = 0; x < w; x++) {
            uint32_t pixel = pixels[y * w + x];
            row[x * 3 + 0] = (pixel >> 16) & 0xFF; // B
            row[x * 3 + 1] = (pixel >> 8) & 0xFF;  // G
            row[x * 3 + 2] = pixel & 0xFF;          // R
        }
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CURSOR
// ═══════════════════════════════════════════════════════════════════════════════

void WindowManager::ShowCursor(bool show) {
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

void WindowManager::GrabCursor(bool grab) {
    if (m_window) {
        SDL_SetWindowGrab(m_window, grab ? SDL_TRUE : SDL_FALSE);
    }
}

void WindowManager::SetRelativeMouseMode(bool relative) {
    SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE);
}

}} // namespace Nova64::Video
