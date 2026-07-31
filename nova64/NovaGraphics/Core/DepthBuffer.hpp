/**
 * @file DepthBuffer.hpp
 * @brief Depth Buffer System.
 */
// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Depth Buffer System
// N64 RDP depth buffer emulation: 18-bit Z + dZ, compare modes, Z-mode decode,
// primitive depth, delta-Z calculation, and GPU depth buffer management.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once
#include "N64Types.hpp"
#include <cstdint>
#include <array>
#include <vector>
#include <functional>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════
//  N64 Depth Format
// ═══════════════════════════════════════════════════════════════

// N64 Z-buffer: 18 bits per pixel
//   - 14-bit integer part (upper)
//   - 4-bit dZ (delta-Z / precision bits)
// Stored in RDRAM as 16-bit values with a separate 2-bit dZ extension
// Combined: z[15:0] = 14-bit Z value in bits 15:2, 2 bits of dZ in 1:0
// Full 18-bit: upper 14 = Z, lower 4 = dZ

enum class DepthCompareMode : uint8_t {
    Never      = 0,   // Never passes — used for "depth off" semantics
    Less       = 1,   // Pass if incoming < stored
    Equal      = 2,   // Pass if incoming == stored
    LessEqual  = 3,   // Pass if incoming <= stored
    Greater    = 4,   // Pass if incoming > stored
    NotEqual   = 5,   // Pass if incoming != stored
    GreaterEq  = 6,   // Pass if incoming >= stored
    Always     = 7    // Always passes
};

enum class DepthSource : uint8_t {
    Primitive  = 0,   // Use per-primitive Z (set via SetPrimDepth)
    Pixel      = 1    // Use per-pixel interpolated Z (default triangle rendering)
};

// ZMode imported from N64Types.hpp (Opaque, Interpenetrate, Transparent, Decal)

// ═══════════════════════════════════════════════════════════════
//  Depth Conversion Tables
// ═══════════════════════════════════════════════════════════════

class DepthConvert {
public:
    // N64 uses a custom non-linear Z encoding. The 14-bit integer portion
    // is derived from a piecewise-linear approximation of the floating-point
    // depth value. These tables convert between the N64 Z encoding and
    // linear/float depth values.

    // Initialize conversion tables
    static void Init();

    // Convert a float depth [0, 1] to N64 14-bit Z value
    static uint16_t FloatToN64Z(float depth);

    // Convert N64 14-bit Z value back to float [0, 1]
    static float N64ZToFloat(uint16_t z14);

    // Get the dZ value for a given 14-bit Z (precision band)
    static uint8_t GetDeltaZ(uint16_t z14);

    // Convert N64 18-bit value (14 Z + 4 dZ) to a single 32-bit integer
    static uint32_t Pack18Bit(uint16_t z14, uint8_t dz4);

    // Unpack 18-bit Z into z14 and dz4
    static void Unpack18Bit(uint32_t packed, uint16_t& z14, uint8_t& dz4);

    // Convert from full 32-bit float depth to 18-bit packed N64 format
    static uint32_t FloatToN64Packed(float depth);

    // Convert from 18-bit packed N64 format to float
    static float N64PackedToFloat(uint32_t packed);

    // Encode/decode 16-bit RDRAM representation
    // The N64 stores Z as: [z14_bits13:0] in 16 bits, with dZ in a separate 4-bit array
    static uint16_t EncodeRDRAM16(uint16_t z14, uint8_t dz4);
    static void DecodeRDRAM16(uint16_t rdram, uint16_t& z14, uint8_t& dz4Lower);

private:
    static bool s_initialized;

    // Piecewise-linear Z encoding tables
    // The N64 splits the Z range into 8 segments with different slopes
    static std::array<uint32_t, 8> s_zShifts;     // Bit shifts per segment
    static std::array<uint32_t, 8> s_zBases;      // Base values per segment
    static std::array<float, 0x4000> s_zToFloat;   // Z14 → float LUT
    static std::array<uint8_t, 0x4000> s_zToDz;    // Z14 → dZ LUT
};

// ═══════════════════════════════════════════════════════════════
//  Depth Buffer State
// ═══════════════════════════════════════════════════════════════

struct DepthBufferConfig {
    bool             enabled       = false;
    bool             writeEnabled  = false;
    DepthCompareMode compareMode   = DepthCompareMode::Less;
    DepthSource      source        = DepthSource::Pixel;
    ZMode            zMode         = ZMode::Opaque;

    // Primitive depth (used when source == Primitive)
    float            primZ         = 0.0f;
    float            primDeltaZ    = 0.0f;

    // Anti-aliasing depth bias
    float            depthBias     = 0.0f;

    // Decal mode tolerance
    float            decalOffset   = 0.001f;

    // From OtherMode
    void DecodeOtherMode(uint32_t otherModeH, uint32_t otherModeL);
    void SetPrimitiveDepth(uint16_t z, uint16_t dz);
};

// GPU-side depth buffer representation
struct GPUDepthBuffer {
    uint32_t  address = 0;           // RDRAM address
    uint32_t  width   = 0;
    uint32_t  height  = 0;
    uint32_t  glDepthTexture = 0;    // GL depth texture (DEPTH32F)
    uint32_t  glDepthFBO     = 0;    // FBO with only depth attachment
    bool      dirty   = false;       // Needs re-upload from RDRAM
    bool      cleared = false;       // Was cleared this frame
    float     clearValue = 1.0f;     // Depth clear value
    uint32_t  lastUsedFrame = 0;     // Frame counter for eviction

    // Z-image textures for N64-accurate depth compare in fragment shader
    // (GlideN64 DepthBuffer::m_pDepthImageZTexture / m_pDepthImageDeltaZTexture)
    uint32_t  glZImageTex     = 0;   // R32F — stores N64 14-bit Z as float
    uint32_t  glDeltaZImageTex = 0;  // R32F — stores delta-Z as float
    uint32_t  glZImageFBO     = 0;   // FBO for clearing Z-image textures
    uint64_t  zImageHandle    = 0;   // Backend-native handle for imageLoad/imageStore
    uint64_t  deltaZImageHandle = 0; // Backend-native handle for imageLoad/imageStore
    uint32_t  zImageWidth     = 0;   // Actual Z-image texture width (may be scaled)
    uint32_t  zImageHeight    = 0;   // Actual Z-image texture height (may be scaled)

    // MSAA resolve target (single-sample copy for shader reads)
    uint32_t  glResolvedDepthTex = 0;  // DEPTH32F single-sample
    bool      resolved = false;         // Has been resolved this frame

    // N64 can share depth buffer address between different color buffer configs
    bool OverlapsAddress(uint32_t addr, uint32_t w, uint32_t h) const;
};

// ═══════════════════════════════════════════════════════════════
//  Depth Compare Logic
// ═══════════════════════════════════════════════════════════════

class DepthCompare {
public:
    // Perform depth comparison based on N64 z-mode
    // Returns true if the pixel passes the depth test
    static bool Compare(float incomingZ, float storedZ,
                        DepthCompareMode mode, ZMode zMode,
                        float decalTolerance = 0.001f);

    // Perform full N64-accurate depth test with dZ consideration
    // The N64 uses dZ values to implement "fuzzy" depth comparison
    // for anti-aliasing and decal rendering
    static bool CompareWithDeltaZ(uint16_t inZ14, uint8_t inDZ,
                                   uint16_t storedZ14, uint8_t storedDZ,
                                   ZMode zMode);

    // Get the maximum dZ from two dZ values (used in interpenetrating mode)
    static uint8_t MaxDeltaZ(uint8_t dz1, uint8_t dz2);

    // Compute the per-pixel dZ from triangle gradients
    static uint8_t ComputePixelDeltaZ(float dZdx, float dZdy);

    // Check if depth test should be skipped entirely
    static bool ShouldSkipTest(const DepthBufferConfig& config);

private:
    // Delta-Z decoding: 4-bit dZ → actual delta value
    static const std::array<uint16_t, 16> s_dzDecode;
};

// ═══════════════════════════════════════════════════════════════
//  Depth Buffer Manager
// ═══════════════════════════════════════════════════════════════

class GPUDepthBufferManager {
public:
    void Init();
    void Shutdown();
    void Reset();

    // Configuration
    void SetConfig(const DepthBufferConfig& config);
    const DepthBufferConfig& GetConfig() const { return m_config; }

    // Buffer management
    void OnSetDepthImage(uint32_t address, uint32_t width);
    GPUDepthBuffer* FindOrCreate(uint32_t address, uint32_t width, uint32_t height);
    GPUDepthBuffer* FindByAddress(uint32_t address) const;

    // Depth operations
    void ClearDepthBuffer(float clearValue = 1.0f);
    void ClearDepthBufferN64(uint16_t clearZ, uint16_t clearDZ);

    // GPU ↔ CPU synchronization
    void CopyToGPU(GPUDepthBuffer& buf, const uint8_t* rdram, uint32_t rdramSize);
    void CopyFromGPU(const GPUDepthBuffer& buf, uint8_t* rdram, uint32_t rdramSize);

    // Z-image textures for shader-based N64 depth compare
    void InitDepthImageTextures(GPUDepthBuffer& buf, uint32_t resScale = 1);
    void ClearDepthImageTextures(GPUDepthBuffer& buf);
    void BindDepthImageTextures(const GPUDepthBuffer& buf, uint32_t zUnit, uint32_t dzUnit);
    void UploadRDRAMToZImage(GPUDepthBuffer& buf, const uint8_t* rdram, uint32_t rdramSize);

    // Attach depth buffer to an FBO
    void SetDepthAttachment(const GPUDepthBuffer& buf, uint32_t fbo);

    // MSAA depth resolve
    void ResolveDepthBuffer(GPUDepthBuffer& buf, uint32_t msaaFBO);

    // Z lookup table texture for GPU-side depth decode
    void InitZLUT();
    void BindZLUT(uint32_t textureUnit);
    uint32_t GetZLUTTexture() const { return m_zLutTex; }

    // Software depth test for pixel-accurate rendering
    bool TestAndWrite(int32_t x, int32_t y, float depth, float deltaZ);

    // Get current GPU depth buffer
    GPUDepthBuffer* GetCurrent() { return m_current; }

    // Frame management
    void OnFrameEnd();

    // Stats
    struct Stats {
        uint64_t depthTestsPassed  = 0;
        uint64_t depthTestsFailed  = 0;
        uint64_t depthWrites       = 0;
        uint64_t bufferUploads     = 0;
        uint64_t bufferDownloads   = 0;
        uint32_t activeBuffers     = 0;
    };
    Stats GetStats() const { return m_stats; }

private:
    static constexpr int MAX_DEPTH_BUFFERS = 8;

    DepthBufferConfig m_config;
    GPUDepthBuffer    m_buffers[MAX_DEPTH_BUFFERS];
    int               m_bufferCount = 0;
    GPUDepthBuffer*   m_current = nullptr;
    uint32_t          m_frameCounter = 0;
    Stats             m_stats;

    // Software depth buffer for pixel-accurate mode (dynamically sized)
    std::vector<uint16_t> m_softwareZ;
    std::vector<uint8_t>  m_softwareDZ;
    uint32_t m_swWidth  = 0;
    uint32_t m_swHeight = 0;

    void CreateGPUResources(GPUDepthBuffer& buf);
    void DestroyGPUResources(GPUDepthBuffer& buf);
    void EvictOldBuffers();

    // Z LUT: 16384-entry float texture mapping N64 Z14 → linear float
    uint32_t m_zLutTex = 0;
    bool     m_zLutInitialized = false;
};

extern GPUDepthBufferManager g_depthManager;

} // namespace Nova64::Video
