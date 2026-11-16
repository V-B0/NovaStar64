/**
 * @file DepthBuffer.hpp
 * @brief N64 RDP depth buffer emulation: 18-bit Z + dZ, compare modes, Z-mode decode, primitive depth,
 *        delta-Z calculation, and GPU depth buffer management.
 */

#pragma once
#include "N64Types.hpp"
#include <cstdint>
#include <array>
#include <vector>
#include <functional>

namespace Nova64::Video {

// --- N64 Depth Format ---

// N64 Z-buffer: 18 bits per pixel, a 14-bit Z in the upper part and 4 bits of dZ (delta-Z,
// precision bits) below it. RDRAM holds 16 bits per pixel: z[15:2] is the 14-bit Z and z[1:0]
// the low 2 bits of dZ; the remaining 2 dZ bits are a separate 2-bit extension.

enum class DepthCompareMode : uint8_t {
    Never = 0,     // Never passes — used for "depth off" semantics
    Less = 1,      // Pass if incoming < stored
    Equal = 2,     // Pass if incoming == stored
    LessEqual = 3, // Pass if incoming <= stored
    Greater = 4,   // Pass if incoming > stored
    NotEqual = 5,  // Pass if incoming != stored
    GreaterEq = 6, // Pass if incoming >= stored
    Always = 7     // Always passes
};

enum class DepthSource : uint8_t {
    Primitive = 0, // Use per-primitive Z (set via SetPrimDepth)
    Pixel = 1      // Use per-pixel interpolated Z (default triangle rendering)
};

// ZMode imported from N64Types.hpp (Opaque, Interpenetrate, Transparent, Decal)

// --- Depth Conversion Tables ---

class DepthConvert {
public:
    // N64 uses a custom non-linear Z encoding. The 14-bit integer portion
    // is derived from a piecewise-linear approximation of the floating-point
    // depth value. These tables convert between the N64 Z encoding and
    // linear/float depth values.

    static void Init();

    // Convert a float depth [0, 1] to N64 14-bit Z value
    static uint16_t FloatToN64Z(float depth);

    // Convert N64 14-bit Z value back to float [0, 1]
    static float N64ZToFloat(uint16_t z14);

    // Get the dZ value for a given 14-bit Z (precision band)
    static uint8_t GetDeltaZ(uint16_t z14);

    // Encode/decode the 16-bit RDRAM representation: z14 in bits 15:2, low 2 bits of dZ in bits 1:0
    static uint16_t EncodeRDRAM16(uint16_t z14, uint8_t dz4);
    static void DecodeRDRAM16(uint16_t rdram, uint16_t& z14, uint8_t& dz4Lower);

private:
    static bool s_initialized;

    // Piecewise-linear Z encoding tables
    // The N64 splits the Z range into 8 segments with different slopes
    static std::array<uint32_t, 8> s_zShifts;    // Bit shifts per segment
    static std::array<uint32_t, 8> s_zBases;     // Base values per segment
    static std::array<float, 0x4000> s_zToFloat; // Z14 → float LUT
    static std::array<uint8_t, 0x4000> s_zToDz;  // Z14 → dZ LUT
};

// --- Depth Buffer State ---

struct DepthBufferConfig {
    bool enabled = false;
    bool writeEnabled = false;
    DepthCompareMode compareMode = DepthCompareMode::Less;
    DepthSource source = DepthSource::Pixel;
    ZMode zMode = ZMode::Opaque;

    // Primitive depth (used when source == Primitive)
    float primZ = 0.0f;
    float primDeltaZ = 0.0f;

    // Anti-aliasing depth bias
    float depthBias = 0.0f;

    // Decal mode tolerance
    float decalOffset = 0.001f;
};

// GPU-side depth buffer representation
struct GPUDepthBuffer {
    uint32_t address = 0; // RDRAM address
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t glDepthTexture = 0; // GL depth texture (DEPTH32F)
    uint32_t glDepthFBO = 0;     // FBO with only depth attachment
    bool dirty = false;          // Needs re-upload from RDRAM
    bool cleared = false;        // Was cleared this frame
    float clearValue = 1.0f;     // Depth clear value
    uint32_t lastUsedFrame = 0;  // Frame counter for eviction

    // Z-image textures for N64-accurate depth compare in fragment shader
    // (GLideN64 DepthBuffer::m_pDepthImageZTexture / m_pDepthImageDeltaZTexture)
    uint32_t glZImageTex = 0;       // R32F — stores N64 14-bit Z as float
    uint32_t glDeltaZImageTex = 0;  // R32F — stores delta-Z as float
    uint32_t glZImageFBO = 0;       // FBO for clearing Z-image textures
    uint64_t zImageHandle = 0;      // Backend-native handle for imageLoad/imageStore
    uint64_t deltaZImageHandle = 0; // Backend-native handle for imageLoad/imageStore
    uint32_t zImageWidth = 0;       // Actual Z-image texture width (may be scaled)
    uint32_t zImageHeight = 0;      // Actual Z-image texture height (may be scaled)

    // MSAA resolve target (single-sample copy for shader reads)
    uint32_t glResolvedDepthTex = 0; // DEPTH32F single-sample
    bool resolved = false;           // Has been resolved this frame
};

// --- Depth Compare Logic ---

class DepthCompare {
public:
    // Perform full N64-accurate depth test with dZ consideration
    // The N64 uses dZ values to implement "fuzzy" depth comparison
    // for anti-aliasing and decal rendering
    static bool CompareWithDeltaZ(uint16_t inZ14, uint8_t inDZ, uint16_t storedZ14, uint8_t storedDZ, ZMode zMode);

    // Compute the per-pixel dZ from triangle gradients
    static uint8_t ComputePixelDeltaZ(float dZdx, float dZdy);

private:
    // Delta-Z decoding: 4-bit dZ → actual delta value
    static const std::array<uint16_t, 16> s_dzDecode;
};

// --- Depth Buffer Manager ---

class GPUDepthBufferManager {
public:
    void Init();
    void Shutdown();
    void Reset();

    const DepthBufferConfig& GetConfig() const { return m_config; }

    // Buffer management
    void OnSetDepthImage(uint32_t address, uint32_t width);
    GPUDepthBuffer* FindOrCreate(uint32_t address, uint32_t width, uint32_t height);

    // Depth operations
    void ClearDepthBuffer(float clearValue = 1.0f);

    // Z-image textures for shader-based N64 depth compare
    void InitDepthImageTextures(GPUDepthBuffer& buf, uint32_t resScale = 1);
    void ClearDepthImageTextures(GPUDepthBuffer& buf);
    void BindDepthImageTextures(const GPUDepthBuffer& buf, uint32_t zUnit, uint32_t dzUnit);

    // Z lookup table texture for GPU-side depth decode
    void InitZLUT();
    uint32_t GetZLUTTexture() const { return m_zLutTex; }

    // Get current GPU depth buffer
    GPUDepthBuffer* GetCurrent() { return m_current; }

    // Frame management
    void OnFrameEnd();

    // Stats
    struct Stats {
        uint64_t depthTestsPassed = 0;
        uint64_t depthTestsFailed = 0;
        uint64_t depthWrites = 0;
        uint64_t bufferUploads = 0;
        uint64_t bufferDownloads = 0;
        uint32_t activeBuffers = 0;
    };
    Stats GetStats() const { return m_stats; }

private:
    static constexpr int MAX_DEPTH_BUFFERS = 8;

    DepthBufferConfig m_config;
    GPUDepthBuffer m_buffers[MAX_DEPTH_BUFFERS];
    int m_bufferCount = 0;
    GPUDepthBuffer* m_current = nullptr;
    uint32_t m_frameCounter = 0;
    Stats m_stats;

    // Software depth buffer for pixel-accurate mode (dynamically sized)
    std::vector<uint16_t> m_softwareZ;
    std::vector<uint8_t> m_softwareDZ;
    uint32_t m_swWidth = 0;
    uint32_t m_swHeight = 0;

    void CreateGPUResources(GPUDepthBuffer& buf);
    void DestroyGPUResources(GPUDepthBuffer& buf);
    void EvictOldBuffers();

    // Z LUT: 16384-entry float texture mapping N64 Z14 → linear float
    uint32_t m_zLutTex = 0;
    bool m_zLutInitialized = false;
};

extern GPUDepthBufferManager g_depthManager;

} // namespace Nova64::Video
