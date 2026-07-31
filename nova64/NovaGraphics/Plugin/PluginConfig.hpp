/**
 * @file PluginConfig.hpp
 * @brief Plugin Configuration.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Plugin Configuration
// All user-facing settings for the video plugin with serialization.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>

namespace Nova64::Video {

// ──────────────────────────────────────────────────────────────
//  Anti-aliasing modes
// ──────────────────────────────────────────────────────────────
enum class AAMode : uint8_t {
    None = 0,
    MSAA2x,
    MSAA4x,
    MSAA8x,
    FXAA,
    SMAA,
};

// ──────────────────────────────────────────────────────────────
//  Texture filtering
// ──────────────────────────────────────────────────────────────
enum class TextureFilterMode : uint8_t {
    Point = 0,   // Nearest neighbor (N64 accurate)
    Bilinear,
    Trilinear,
    Aniso2x,
    Aniso4x,
    Aniso8x,
    Aniso16x,
};

// ──────────────────────────────────────────────────────────────
//  HiRes texture pack format
// ──────────────────────────────────────────────────────────────
enum class HiResFormat : uint8_t {
    None = 0,
    RiceFormat,      // Legacy Rice-format packs
    HTC,             // HTC (GLideN64 format)
    DDS,             // DirectDraw Surface (compressed)
};

// ──────────────────────────────────────────────────────────────
//  Aspect ratio option
// ──────────────────────────────────────────────────────────────
enum class AspectRatio : uint8_t {
    Native4_3 = 0,
    Stretch16_9,
    Widescreen16_9, // Widescreen hack
    Auto,
};

// ──────────────────────────────────────────────────────────────
//  Post-processing filter
// ──────────────────────────────────────────────────────────────
enum class PostFilter : uint8_t {
    None = 0,
    GammaCorrection,
    CRTFilter,
    Bloom,
    Sharpen,
};

// ──────────────────────────────────────────────────────────────
//  Rendering mode (MLE pivot)
//
//  HLE  — Graphics microcode runs in NovaGraphics/Microcode handlers.
//         Display lists are intercepted at GBI level. Default for now.
//  LLE  — RSP recompiler executes the actual graphics microcode and emits
//         RDP commands consumed by RDPProcessor::ProcessCommandBuffer.
//         HLE handlers remain as shadow taps for widescreen / texture-pack /
//         RT world-space recovery (see MLEDirector).
//  Auto — Audio HLE + Graphics LLE (use LLE for visuals, keep HLE for audio).
// ──────────────────────────────────────────────────────────────
enum class RenderingMode : uint8_t {
    HLE  = 0,
    LLE  = 1,
    Auto = 2,
};

// ══════════════════════════════════════════════════════════════
//  PluginConfig
// ══════════════════════════════════════════════════════════════

struct PluginConfig {
    // ── Resolution ────────────────────────────────────────────
    int      windowWidth         = 1280;
    int      windowHeight        = 960;
    int      internalResScale    = 4;      // 1 = native, 2 = 2x, 4 = 4x, etc.
    bool     fullscreen          = false;
    int      fullscreenMonitor   = 0;      // Monitor index
    bool     vsync               = true;
    int      targetFPS           = 0;       // 0 = VI rate (60/50)

    // ── Rendering backend ─────────────────────────────────────
#ifdef __APPLE__
    int      backendType         = 1;      // macOS: Vulkan via MoltenVK (GL caps at 4.1; renderer needs 4.6)
#else
    int      backendType         = 0;      // 0=GL, 1=VK, 2=D3D11
#endif

    // ── Anti-aliasing ─────────────────────────────────────────
    AAMode   antiAliasing        = AAMode::None;

    // ── Texture filtering ─────────────────────────────────────
    TextureFilterMode texFilter  = TextureFilterMode::Bilinear;

    // ── Mipmapping ────────────────────────────────────────────
    bool     enableMipmaps       = true;
    float    lodBias             = 0.0f;

    // ── HiRes textures ────────────────────────────────────────
    bool     hiResTexEnabled     = false;
    HiResFormat hiResFormat      = HiResFormat::None;
    std::string hiResTexPath;

    // ── Texture cache ─────────────────────────────────────────
    int      textureCacheSizeMB  = 128;
    bool     textureDump         = false;
    std::string textureDumpPath;

    // ── Frame buffer emulation ────────────────────────────────
    bool     fbEmulation         = true;
    bool     fbReadAccess        = true;    // Copy from GPU → RDRAM
    bool     fbWriteAccess       = true;    // Copy from RDRAM → GPU
    bool     depthCopy           = true;
    bool     nativeResFB         = false;   // Always use native res for FB ops
    // Perform GPU→RDRAM framebuffer readback once every N VI frames.
    // 1 = every frame (most accurate, costs ~10ms/frame on Intel iGPU due to
    // glMapBufferRange stalls + a 30-50ms spike every Nth frame on idle iGPUs).
    // 60 = once per second (default, sustains a true 60 FPS; sufficient for
    // pause-menu blur and save-state thumbnails). Lower this if a game relies
    // on continuous CPU reads of the framebuffer (rare).
    int      fbReadbackInterval  = 60;

    // ── Depth buffer ──────────────────────────────────────────
    bool     depthCompare        = true;
    bool     n64DepthCompare     = false;   // Software N64-accurate depth compare

    // ── Emulation level (MLE pivot) ──────────────────────────
    RenderingMode renderingMode  = RenderingMode::LLE;

    // ── Compute ray tracing (MoltenVK hybrid shadows/lighting/reflections) ──
    bool rayTracing              = false;

    // ── Aspect ratio ──────────────────────────────────────────
    AspectRatio aspectRatio      = AspectRatio::Native4_3;
    float    stretchFactor       = 1.0f;

    // ── Post-processing ───────────────────────────────────────
    PostFilter postFilter        = PostFilter::None;
    float    gamma               = 1.0f;
    float    bloomIntensity      = 0.5f;
    float    sharpenStrength     = 0.3f;

    // ── Color dithering ───────────────────────────────────────
    bool     ditheringEnabled    = true;
    int      ditheringPattern    = 0;       // 0=N64, 1=Bayer, 2=noise

    // ── Fog ───────────────────────────────────────────────────
    bool     fogEnabled          = true;
    int      fogMode             = 0;       // 0=N64, 1=smooth, 2=enhanced
    float    fogAdjust           = 1.0f;    // Fog multiplier adjustment (per-game hack)

    // ── Game-specific rendering overrides ───────────────────────
    bool     accurateBlending    = false;   // Use more precise blender (slower)
    bool     forceNoAlphaTest    = false;   // Disable alpha test entirely
    bool     paperMarioAlphaHack = false;   // Star spirits alpha test fixup
    int32_t  depthBiasHack       = 0;       // Polygon offset for depth fighting (DK Racing)
    // ── Performance ───────────────────────────────────────────
    bool     multiThreading      = true;
    bool     asyncTextureLoading = true;
    int      threadCount         = 0;       // 0 = auto
    bool     batchDrawCalls      = true;
    bool     skipUnchangedFrames = false;

    // ── Overlay / Debug ───────────────────────────────────────
    bool     showFPS             = false;
    bool     showDebugInfo       = false;
    bool     showVRAM            = false;
    bool     wireframe           = false;
    int      debugOverlayPanels  = 0;       // Bitmask of OverlayPanel

    // ── OSD  ──────────────────────────────────────────────────
    uint32_t osdColor            = 0xFFFFFFFF;
    float    osdScale            = 1.0f;
    int      osdPosition         = 0;       // 0=top-left, 1=top-right, 2=bot-left, 3=bot-right

    // ── Screenshot ────────────────────────────────────────────
    std::string screenshotPath   = "screenshots";
    int      screenshotFormat    = 0;       // 0=PNG, 1=BMP, 2=TGA

    // ═══════════════════════════════════════════════════════════
    //  Methods
    // ═══════════════════════════════════════════════════════════

    void LoadDefaults();
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

    // Get/Set by name (for INI serialization)
    bool SetValue(const std::string& key, const std::string& value);
    std::string GetValue(const std::string& key) const;

    // Validation
    void Clamp();              // Clamp all values to valid ranges
    bool Validate() const;     // Check for invalid combinations
};

} // namespace Nova64::Video
