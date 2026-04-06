/**
 * @file FramebufferManager.hpp
 * @brief N64 framebuffer management: read-back, copy-back, VI scaling, and GPU/CPU sync.
 */
#pragma once

#include "N64Types.hpp"
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <functional>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════
//  Framebuffer Format
// ═══════════════════════════════════════════════════════════════

enum class FBFormat : uint8_t {
    RGBA16  = 0,    // 5-5-5-1 (most N64 games)
    RGBA32  = 1,    // 8-8-8-8
    CI8     = 2,    // Color-indexed 8-bit
    I8      = 3,    // Intensity 8-bit
};

enum class FBType : uint8_t {
    Color   = 0,
    Depth   = 1,
    Aux     = 2,
};

// ═══════════════════════════════════════════════════════════════
//  Framebuffer Descriptor
// ═══════════════════════════════════════════════════════════════

struct FramebufferDesc {
    uint32_t address    = 0;        // RDRAM address
    uint32_t width      = 320;      // Width in pixels
    uint32_t height     = 240;      // Height in pixels (inferred)
    uint32_t stride     = 640;      // Bytes per line
    FBFormat format     = FBFormat::RGBA16;
    FBType   type       = FBType::Color;
    uint32_t size       = 2;        // Pixel size in bytes (2=16bit, 4=32bit)

    uint32_t ByteSize() const { return stride * height; }
    uint32_t EndAddress() const { return address + ByteSize(); }

    bool Overlaps(const FramebufferDesc& other) const {
        return address < other.EndAddress() && other.address < EndAddress();
    }

    bool operator==(const FramebufferDesc& other) const {
        return address == other.address && width == other.width &&
               height == other.height && format == other.format;
    }
};

// ═══════════════════════════════════════════════════════════════
//  GPU Framebuffer Object
// ═══════════════════════════════════════════════════════════════

struct GPUFramebuffer {
    uint32_t fbo         = 0;   // GL framebuffer
    uint32_t colorTex    = 0;   // GL color texture attachment
    uint32_t depthTex    = 0;   // GL depth texture attachment
    uint32_t width       = 0;
    uint32_t height      = 0;
    uint32_t scaledWidth = 0;   // Width after resolution scale
    uint32_t scaledHeight= 0;
    uint32_t msaaSamples = 0;   // MSAA sample count used at creation (0/1 = no MSAA)
    uint32_t resolvedTex = 0;   // Non-MSAA resolved color tex (MSAA FBs only, lazy-init)
    uint32_t resolvedFbo = 0;   // FBO wrapping resolvedTex (MSAA only, lazy-init)
    uint32_t coverageTex     = 0;   // R8UI coverage image for MLE blender (4-bit CVG per pixel)
    uint64_t colorImageHandle    = 0; // Backend-native handle for imageLoad/imageStore
    uint64_t coverageImageHandle = 0; // Backend-native handle for imageLoad/imageStore
    uint32_t snapshotTex     = 0;   // One-frame snapshot for self-texture reads (lazy-init)
    uint32_t snapshotCvgTex  = 0;   // Coverage snapshot for self-texture reads (lazy-init)
    uint32_t presentationTex = 0;   // Snapshot of last completed scene for display
    bool     needsRead   = false;   // Needs GPU→CPU readback
    bool     needsCopy   = false;   // Needs CPU→GPU copy-back
    bool     isAux       = false;
    bool     gpuModified = false;   // GPU has rendered to this FB (set on draw, cleared on CopyFromGPU)
    uint64_t lastUsedFrame = 0;
    FramebufferDesc desc;
};

// ═══════════════════════════════════════════════════════════════
//  VI (Video Interface) Configuration
// ═══════════════════════════════════════════════════════════════

struct VIConfig {
    uint32_t origin     = 0;        // VI_ORIGIN_REG
    uint32_t width      = 320;      // VI_WIDTH_REG
    uint32_t height     = 240;      // Computed from V regs
    uint32_t xScale     = 0x200;    // VI_X_SCALE_REG (2.10 fixed)
    uint32_t yScale     = 0x400;    // VI_Y_SCALE_REG (2.10 fixed)
    uint32_t hStart     = 0;        // VI_H_START_REG
    uint32_t vStart     = 0;        // VI_V_START_REG
    uint32_t control    = 0;        // VI_CONTROL_REG
    bool     antiAlias  = false;    // AA enabled in control reg
    bool     divot      = false;    // Divot filter enabled
    bool     dedither    = false;    // VI dither removal (AA mode 0)
    bool     gammaCorrect = false;  // Gamma correction enabled
    bool     gammaDither  = false;  // Gamma dither enabled
    bool     interlaced   = false;  // Interlaced mode
    uint32_t field        = 0;      // Current interlace field (0 or 1)

    FBFormat GetFormat() const {
        switch (control & 0x3) {
        case 2:  return FBFormat::RGBA16;
        case 3:  return FBFormat::RGBA32;
        default: return FBFormat::RGBA16;
        }
    }

    uint32_t GetPixelSize() const {
        return (control & 0x3) == 3 ? 4 : 2;
    }

    float GetScaleX() const {
        return static_cast<float>(xScale & 0xFFF) / 1024.0f;
    }

    float GetScaleY() const {
        return static_cast<float>(yScale & 0xFFF) / 1024.0f;
    }
};

// ═══════════════════════════════════════════════════════════════
//  Framebuffer Manager
// ═══════════════════════════════════════════════════════════════

class FramebufferManager {
public:
    static constexpr uint32_t kMaxFramebuffers = 32;
    static constexpr uint32_t kMaxHistory = 8;

    FramebufferManager() = default;
    ~FramebufferManager();

    // ── Lifecycle ──
    bool Init(uint32_t resolutionScale = 1);
    void Shutdown();
    void Reset();

    // ── Framebuffer Tracking ──
    /// Called when RDP sets color/depth image — tracks the FB
    void OnSetColorImage(uint32_t address, uint32_t width, uint32_t format, uint32_t size);
    void OnSetDepthImage(uint32_t address);

    /// Find or create GPU framebuffer for the given N64 address
    GPUFramebuffer* FindOrCreate(const FramebufferDesc& desc);
    GPUFramebuffer* FindByAddress(uint32_t address);
    GPUFramebuffer* GetCurrentColor() { return m_currentColor; }
    GPUFramebuffer* GetCurrentDepth() { return m_currentDepth; }
    uint32_t GetFBCount() const { return m_fbCount; }
    GPUFramebuffer* GetFB(uint32_t idx) { return idx < m_fbCount ? &m_framebuffers[idx] : nullptr; }
    uint64_t GetFrameCounter() const { return m_frameCounter; }

    // ── GPU ↔ CPU Sync ──
    /// Copy N64 RDRAM data into GPU color buffer
    void CopyToGPU(GPUFramebuffer* fb, const uint8_t* rdram);

    /// Read GPU color buffer back to N64 RDRAM (synchronous — stalls GPU)
    void CopyFromGPU(GPUFramebuffer* fb, uint8_t* rdram);

    /// Read GPU depth buffer back to N64 RDRAM (synchronous)
    void CopyDepthFromGPU(GPUFramebuffer* fb, uint8_t* rdram);

    /// Initiate async GPU→CPU readback via PBO (non-blocking)
    void BeginAsyncReadback(GPUFramebuffer* fb);

    /// Check if async readback is complete; if so, copy data to RDRAM
    bool FinishAsyncReadback(uint8_t* rdram);

    /// Is an async readback currently pending?
    bool IsReadbackPending() const { return m_pboFence != nullptr; }

    /// Copy a rectangular region between two framebuffer addresses (RDP tile copy)
    void TileCopy(GPUFramebuffer* src, GPUFramebuffer* dst,
                  int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                  int32_t w, int32_t h);

    /// Check if RDRAM region was modified since last GPU upload
    bool IsRDRAMDirty(uint32_t address, uint32_t size) const;

    /// Mark RDRAM region as modified
    void MarkRDRAMDirty(uint32_t address, uint32_t size);

    /// Mark RDRAM region as clean (synced with GPU)
    void ClearRDRAMDirty(uint32_t address, uint32_t size);

    // ── Display (VI Output) ──
    /// Update VI configuration from N64 registers
    void UpdateVI(const VIConfig& config);

    /// Get the framebuffer to display on screen
    GPUFramebuffer* GetDisplayFramebuffer();

    /// Blit display framebuffer to screen with VI scaling/filtering
    void BlitToScreen(uint32_t screenWidth, uint32_t screenHeight, bool skipVIFilter = false);

    /// Apply VI anti-aliasing filter
    void ApplyVIFilter(GPUFramebuffer* fb);

    /// Apply gamma correction
    void ApplyGammaCorrection(GPUFramebuffer* fb, float gamma);

    // ── Format Conversion ──
    /// Convert RGBA16 RDRAM data to RGBA8 for GPU upload
    void ConvertRGBA16ToRGBA8(const uint16_t* src, uint8_t* dst, uint32_t pixelCount);
    void ConvertRGBA32ToRGBA8(const uint32_t* src, uint8_t* dst, uint32_t pixelCount);
    void ConvertRGBA8ToRGBA16(const uint8_t* src, uint16_t* dst, uint32_t pixelCount);
    void ConvertRGBA8ToRGBA32(const uint8_t* src, uint32_t* dst, uint32_t pixelCount);

    /// Convert depth buffer format (N64 uses 18-bit: 14.2 fixed + 2 dz)
    void ConvertDepthN64ToFloat(const uint16_t* src, float* dst, uint32_t pixelCount);
    void ConvertDepthFloatToN64(const float* src, uint16_t* dst, uint32_t pixelCount,
                                    uint32_t width = 0);

    // ── Framebuffer Effects ──
    /// Copy between framebuffers (used by some games for effects)
    void CopyFramebuffer(GPUFramebuffer* src, GPUFramebuffer* dst);

    /// Fill framebuffer region with color
    void FillRegion(GPUFramebuffer* fb, int32_t x, int32_t y, int32_t w, int32_t h,
                    uint32_t color);

    /// Get a GL_TEXTURE_2D handle safe to bind as a shader sampler2D.
    /// Handles MSAA resolve (returns resolvedTex) and self-texture
    /// (returns snapshotTex copy). Returns 0 if the FB has no valid color texture.
    uint32_t GetSampleableTexture(GPUFramebuffer* fb, bool isSelfTexture);

    /// Snapshot the current color FBO for presentation.
    /// Called after a scene batch is flushed so that a subsequent fill-rect
    /// clear (common in N64 DLs) does not destroy the completed frame.
    void SnapshotForPresentation();

    /// Resolve MSAA framebuffer
    void ResolveMSAA(GPUFramebuffer* msaaFB, GPUFramebuffer* resolved);

    // ── Statistics ──
    struct Stats {
        uint64_t gpuToRdramCopies = 0;
        uint64_t rdramToGpuCopies = 0;
        uint64_t framebuffersCreated = 0;
        uint64_t framebuffersReused = 0;
        uint64_t viUpdates = 0;
        uint64_t blitsToScreen = 0;
        uint64_t formatConversions = 0;
        uint64_t bytesTransferred = 0;
    };
    const Stats& GetStats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }

    // ── Configuration ──
    void SetResolutionScale(uint32_t scale) { m_resScale = scale; }
    uint32_t GetResolutionScale() const { return m_resScale; }

    /// Set MSAA sample count (1 = disabled, 2/4/8 = multisample)
    void SetMSAASamples(uint32_t samples) { m_msaaSamples = samples; }
    uint32_t GetMSAASamples() const { return m_msaaSamples; }

    // ── Game-Specific Hack Flags ──
    struct FBHacks {
        bool paperMarioBlueBoost = false;   // Add 4 to blue channel on FB→RDRAM copy
        bool conkerBlurDetect = false;       // Detect colorImage==depthImage pause blur
        bool pdDepthOncePerFrame = false;    // Only copy depth to RDRAM once per frame
        bool starCraftBlockFB = false;       // Block FB ops when LUT is RGBA16
        bool snapSkipAuxCopy = false;        // Skip automatic auxiliary buffer copies
    } m_fbHacks;

    /// Hint the expected framebuffer height (derived from the current scissor box).
    /// Called whenever SetScissor fires so OnSetColorImage uses the right height.
    void SetScissorHint(uint32_t width, uint32_t height) {
        if (height > 0) m_scissorHeight = height;
        if (width  > 0) m_scissorWidth  = width;
    }

    const VIConfig& GetVIConfig() const { return m_viConfig; }

    /// Store the RDRAM base pointer for RDRAM→GPU uploads in UpdateVI
    void SetRDRAM(const uint8_t* rdram) { m_rdram = rdram; }

    // Intel Iris Xe workaround: FBOs created lazily (after game textures)
    // can have broken texture sampling. Recreating gives a fresh GL object.
    void RecreateGPUFramebuffer(GPUFramebuffer& fb) {
        // Preserve presentation snapshot across recreation
        uint32_t savedPresentation = fb.presentationTex;
        fb.presentationTex = 0;
        DestroyGPUFramebuffer(fb);
        CreateGPUFramebuffer(fb);
        fb.presentationTex = savedPresentation;
    }

private:
    void CreateGPUFramebuffer(GPUFramebuffer& fb);
    void DestroyGPUFramebuffer(GPUFramebuffer& fb);
    void EvictOldFramebuffers(uint64_t currentFrame);

    std::array<GPUFramebuffer, kMaxFramebuffers> m_framebuffers{};
    uint32_t m_fbCount = 0;

    GPUFramebuffer* m_currentColor = nullptr;
    GPUFramebuffer* m_currentDepth = nullptr;

    VIConfig m_viConfig;
    uint32_t m_resScale = 1;
    uint32_t m_msaaSamples = 1;
    uint32_t m_scissorWidth  = 320;
    uint32_t m_scissorHeight = 240;

    // RDRAM dirty tracking (1 bit per 4KB page)
    static constexpr uint32_t kRDRAMSize = 8 * 1024 * 1024; // 8MB
    static constexpr uint32_t kPageSize  = 4096;
    static constexpr uint32_t kPageCount = kRDRAMSize / kPageSize;
    std::array<bool, kPageCount> m_dirtyPages{};

    uint64_t m_frameCounter = 0;
    bool     m_depthClearPending = false;     // Set by OnSetDepthImage, consumed by OnSetColorImage
    bool     m_depthCopiedThisFrame = false;  // PD hack: track depth copy per frame
    const uint8_t* m_rdram = nullptr;        // Stored RDRAM pointer for CopyToGPU in UpdateVI
    Stats m_stats{};
    bool m_initialized = false;

    // Blit shader resources
    uint32_t m_blitVAO = 0;
    uint32_t m_blitVBO = 0;
    uint32_t m_blitProgram = 0;

    // VI filter resources
    uint32_t m_viFilterProgram = 0;
    uint32_t m_gammaProgram = 0;

    // Cached temp FBO/texture for gamma and VI filter (avoid per-frame alloc)
    uint32_t m_tmpFBO = 0;
    uint32_t m_tmpTex = 0;
    uint32_t m_tmpWidth = 0;
    uint32_t m_tmpHeight = 0;

    // Async PBO readback state
    uint32_t m_pbo = 0;                     // GL pixel buffer object for async readback
    void*    m_pboFence = nullptr;          // GL sync fence (GLsync, stored as void* to avoid GL header in .hpp)
    uint32_t m_pboAddress = 0;              // RDRAM target address for pending readback
    uint32_t m_pboWidth = 0;                // Width of pending readback
    uint32_t m_pboHeight = 0;              // Height of pending readback
    FBFormat m_pboFormat = FBFormat::RGBA16; // Format for conversion on completion

    // Cached downscale FBO for PBO readback (avoid per-frame alloc)
    uint32_t m_readbackFBO = 0;
    uint32_t m_readbackTex = 0;
    uint32_t m_readbackW = 0;
    uint32_t m_readbackH = 0;

};

extern FramebufferManager g_fbManager;

} // namespace Nova64::Video
