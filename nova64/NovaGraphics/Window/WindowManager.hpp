/**
 * @file WindowManager.hpp
 * @brief Window Manager.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Window Manager
// SDL2 window creation, OpenGL context, input, display management

#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Forward declare SDL types to avoid including SDL in the header
struct SDL_Window;
typedef void* SDL_GLContext;

namespace Nova64 {
namespace Video {

// ═══════════════════════════════════════════════════════════════════════════════
//  WINDOW EVENTS
// ═══════════════════════════════════════════════════════════════════════════════

enum class WindowEvent : uint32_t {
    None = 0,
    Resized,
    Minimized,
    Restored,
    FocusGained,
    FocusLost,
    Moved,
    Closed,
    Exposed,
    Maximized,
};

struct WindowEventData {
    WindowEvent type = WindowEvent::None;
    int32_t param1 = 0; // width for Resized, x for Moved
    int32_t param2 = 0; // height for Resized, y for Moved
};

// ═══════════════════════════════════════════════════════════════════════════════
//  DISPLAY MODE
// ═══════════════════════════════════════════════════════════════════════════════

struct DisplayMode {
    int32_t width;
    int32_t height;
    int32_t refreshRate;
    uint32_t pixelFormat;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  WINDOW CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════════

struct WindowConfig {
    std::string title = "NovaStar64 - NovaGraphics";
    int32_t width = 640;
    int32_t height = 480;
    int32_t posX = -1; // -1 = centered
    int32_t posY = -1;
    bool fullscreen = false;
    bool borderless = false;
    bool resizable = true;
    bool highDPI = false;
    bool vsync = true;
    int32_t displayIndex = 0;
    float aspectRatio = 4.0f / 3.0f; // N64 native aspect

    // OpenGL context settings
    int32_t glMajor = 4;
    int32_t glMinor = 6;
    bool glDebug = false;
    int32_t multiSamples = 0; // 0 = disabled

    // Backend selection
    int32_t backendType = 0; // 0 = OpenGL, 1 = Vulkan
    bool headless = false;   // If true, do not create a visible window
};

// ═══════════════════════════════════════════════════════════════════════════════
//  WINDOW MANAGER
// ═══════════════════════════════════════════════════════════════════════════════

class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    // Prevent copy
    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    /// Initialize SDL and create window with OpenGL context
    bool Initialize(const WindowConfig& config);

    /// Create a WindowManager that WRAPS an already-existing SDL_Window and GL context.
    /// The returned instance does NOT own the window or context — it will not
    /// destroy them on Shutdown(). Used by the launcher to share its window with VideoPlugin.
    static WindowManager* WrapExternal(SDL_Window* sdlWindow, SDL_GLContext glCtx);

    /// Destroy window and clean up
    void Shutdown();

    /// Check if initialized
    bool IsInitialized() const { return m_window != nullptr; }

    // ── Window Properties ─────────────────────────────────────────────────

    /// Get current window size (client area)
    void GetSize(int32_t& width, int32_t& height) const;

    /// Get drawable size (may differ from window size on HiDPI)
    void GetDrawableSize(int32_t& width, int32_t& height) const;

    /// Set window size
    void SetSize(int32_t width, int32_t height);

    /// Get/Set window position
    void GetPosition(int32_t& x, int32_t& y) const;
    void SetPosition(int32_t x, int32_t y);

    /// Set window title
    void SetTitle(const std::string& title);

    /// Get window title
    std::string GetTitle() const;

    // ── Display Modes ────────────────────────────────────────────────────

    /// Toggle fullscreen
    void SetFullscreen(bool fullscreen, bool borderless = false);
    bool IsFullscreen() const { return m_fullscreen; }

    /// Get available display modes
    std::vector<DisplayMode> GetDisplayModes(int32_t displayIndex = 0) const;

    /// Get current display mode
    DisplayMode GetCurrentDisplayMode() const;

    /// Get number of connected displays
    int32_t GetDisplayCount() const;

    // ── OpenGL Context ──────────────────────────────────────────────────

    /// Make the GL context current (needed for multi-threaded rendering)
    void MakeContextCurrent();

    /// Release the GL context from current thread
    void ReleaseContext();

    /// Swap front/back buffers
    void SwapBuffers();

    /// Set VSync mode: 0 = off, 1 = on, -1 = adaptive
    void SetVSync(int32_t interval);

    /// Get the native window handle (HWND on Windows)
    void* GetNativeHandle() const;

    /// Get SDL window pointer
    SDL_Window* GetSDLWindow() const { return m_window; }

    /// Get SDL GL context
    SDL_GLContext GetGLContext() const { return m_glContext; }

    // ── Event Processing ────────────────────────────────────────────────

    /// Process pending SDL events. Returns false if quit requested.
    bool ProcessEvents();

    /// Poll for window events
    bool PollWindowEvent(WindowEventData& event);

    /// Register a callback for window events
    using EventCallback = std::function<void(const WindowEventData&)>;
    void SetEventCallback(EventCallback callback) { m_eventCallback = callback; }

    /// Check if window has focus
    bool HasFocus() const { return m_hasFocus; }

    /// Check if window is minimized
    bool IsMinimized() const { return m_minimized; }

    // ── Screenshots ─────────────────────────────────────────────────────

    /// Capture the current framebuffer to pixels (RGBA8)
    bool CaptureFramebuffer(std::vector<uint32_t>& pixels, int32_t& width, int32_t& height);

    /// Save framebuffer to BMP file
    bool SaveScreenshot(const std::string& path);

    // ── Cursor ──────────────────────────────────────────────────────────

    /// Show/hide cursor
    void ShowCursor(bool show);

    /// Confine cursor to window
    void GrabCursor(bool grab);

    /// Set relative mouse mode (for FPS-style mouse look)
    void SetRelativeMouseMode(bool relative);

private:
    void HandleSDLEvent(const void* sdlEvent); // Actually SDL_Event*

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    WindowConfig m_config;
    std::thread::id m_glContextThread; // Track which thread owns GL context

    bool m_fullscreen = false;
    bool m_hasFocus = true;
    bool m_minimized = false;
    bool m_shouldClose = false;
    bool m_isExternal = false; // if true, Shutdown() does not destroy window/context

    // Pending window events
    std::vector<WindowEventData> m_pendingEvents;
    EventCallback m_eventCallback;

    // Pre-fullscreen window state (for restoring)
    int32_t m_windowedX = 0;
    int32_t m_windowedY = 0;
    int32_t m_windowedW = 0;
    int32_t m_windowedH = 0;
};

} // namespace Video
} // namespace Nova64
