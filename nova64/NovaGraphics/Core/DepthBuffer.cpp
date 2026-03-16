// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Depth Buffer System Implementation
// N64 RDP 18-bit depth: piecewise-linear encoding, dZ fuzzy compare,
// GPU buffer management with GL4.6 DSA, software fallback depth test.
// ══════════════════════════════════════════════════════════════════════════════

#include "DepthBuffer.hpp"
#include "../Common/Logger.hpp"

#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Nova64::Video {

GPUDepthBufferManager g_depthManager;

// ═══════════════════════════════════════════════════════════════
//  Depth Conversion Tables — Static Data
// ═══════════════════════════════════════════════════════════════

bool DepthConvert::s_initialized = false;
std::array<uint32_t, 8> DepthConvert::s_zShifts;
std::array<uint32_t, 8> DepthConvert::s_zBases;
std::array<float, 0x4000> DepthConvert::s_zToFloat;
std::array<uint8_t, 0x4000> DepthConvert::s_zToDz;

// N64 depth encoding uses a piecewise-linear representation:
// The 14-bit Z space is divided into 8 segments (512 values each in the decoded
// domain). Each segment has a different step size, giving more precision near
// the camera and less far away — similar to a non-linear Z buffer.
//
// Segment | Z range (decoded) | Step
//    0    |     0 -   511     |    4
//    1    |   512 -  1023     |    8
//    2    |  1024 -  2047     |   16
//    3    |  2048 -  4095     |   32
//    4    |  4096 -  8191     |   64
//    5    |  8192 - 16383     |  128
//    6    | 16384 - 32767     |  256
//    7    | 32768 - 65535     |  512

void DepthConvert::Init() {
    if (s_initialized) return;

    // Segment shift values (step = 1 << (shift + 2))
    s_zShifts = { 6, 5, 4, 3, 2, 1, 0, 0 };

    // Base values for each segment
    s_zBases = { 0, 32, 64, 96, 128, 160, 192, 224 };

    // Build forward LUT: 14-bit Z → float [0, 1]
    // The N64 Z range is 0x0000 to 0x3FFF in encoded form
    // Decoded range is approximately 0 to 0xFFFC
    for (uint32_t z14 = 0; z14 < 0x4000; ++z14) {
        // Determine segment from upper 3 bits (bits 13:11)
        uint32_t seg = (z14 >> 11) & 0x7;
        uint32_t offset = z14 & 0x7FF;     // Lower 11 bits
        uint32_t step = 1u << (s_zShifts[seg] + 2);
        uint32_t base = s_zBases[seg] << 8; // Segment base in decoded space

        uint32_t decoded = base + (offset * step);
        s_zToFloat[z14] = static_cast<float>(decoded) / 65536.0f;

        // dZ: the precision band for this Z value
        // Higher segments have larger dZ (less precision)
        s_zToDz[z14] = static_cast<uint8_t>(std::min(seg + 1u, 15u));
    }

    s_initialized = true;
}

float DepthConvert::N64ZToFloat(uint16_t z14) {
    return s_zToFloat[z14 & 0x3FFF];
}

uint16_t DepthConvert::FloatToN64Z(float depth) {
    depth = std::clamp(depth, 0.0f, 1.0f);
    uint32_t target = static_cast<uint32_t>(depth * 65536.0f);

    // Binary search through the float table
    uint16_t lo = 0, hi = 0x3FFF;
    while (lo < hi) {
        uint16_t mid = (lo + hi + 1) / 2;
        float midVal = s_zToFloat[mid];
        if (midVal <= depth) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

uint8_t DepthConvert::GetDeltaZ(uint16_t z14) {
    return s_zToDz[z14 & 0x3FFF];
}

uint32_t DepthConvert::Pack18Bit(uint16_t z14, uint8_t dz4) {
    return (static_cast<uint32_t>(z14 & 0x3FFF) << 4) | (dz4 & 0xF);
}

void DepthConvert::Unpack18Bit(uint32_t packed, uint16_t& z14, uint8_t& dz4) {
    z14 = static_cast<uint16_t>((packed >> 4) & 0x3FFF);
    dz4 = static_cast<uint8_t>(packed & 0xF);
}

uint32_t DepthConvert::FloatToN64Packed(float depth) {
    uint16_t z14 = FloatToN64Z(depth);
    uint8_t dz4 = GetDeltaZ(z14);
    return Pack18Bit(z14, dz4);
}

float DepthConvert::N64PackedToFloat(uint32_t packed) {
    uint16_t z14;
    uint8_t dz4;
    Unpack18Bit(packed, z14, dz4);
    return N64ZToFloat(z14);
}

uint16_t DepthConvert::EncodeRDRAM16(uint16_t z14, uint8_t dz4) {
    // RDP stores: z[13:2] in bits 15:4, z[1:0] in bits 3:2, dz[1:0] in bits 1:0
    return static_cast<uint16_t>((z14 << 2) | (dz4 & 0x3));
}

void DepthConvert::DecodeRDRAM16(uint16_t rdram, uint16_t& z14, uint8_t& dz4Lower) {
    z14 = (rdram >> 2) & 0x3FFF;
    dz4Lower = static_cast<uint8_t>(rdram & 0x3);
}

// ═══════════════════════════════════════════════════════════════
//  Depth Buffer Config
// ═══════════════════════════════════════════════════════════════

void DepthBufferConfig::DecodeOtherMode(uint32_t otherModeH, uint32_t otherModeL) {
    // Z compare: bit 4 of otherModeL
    // Z update:  bit 5 of otherModeL
    enabled      = (otherModeL & (1 << 4)) != 0;  // z_cmp
    writeEnabled = (otherModeL & (1 << 5)) != 0;  // z_upd

    // Z source: bit 2 of otherModeL
    source = ((otherModeL & (1 << 2)) != 0) ? DepthSource::Primitive : DepthSource::Pixel;

    // Z mode: bits 10-11 of otherModeL
    uint32_t zm = (otherModeL >> 10) & 0x3;
    zMode = static_cast<ZMode>(zm);

    // Default compare mode based on z-mode
    switch (zMode) {
    case ZMode::Opaque:
    case ZMode::Transparent:
        compareMode = DepthCompareMode::Less;
        break;
    case ZMode::Decal:
        compareMode = DepthCompareMode::LessEqual;
        break;
    case ZMode::Interpenetrate:
        compareMode = DepthCompareMode::Less;
        break;
    }
}

void DepthBufferConfig::SetPrimitiveDepth(uint16_t z, uint16_t dz) {
    primZ = DepthConvert::N64ZToFloat(z);
    primDeltaZ = static_cast<float>(dz) / 65536.0f;
}

// ═══════════════════════════════════════════════════════════════
//  GPUDepthBuffer
// ═══════════════════════════════════════════════════════════════

bool GPUDepthBuffer::OverlapsAddress(uint32_t addr, uint32_t w, uint32_t h) const {
    if (address == 0) return false;
    uint32_t mySize = width * height * 2;   // 16-bit Z per pixel
    uint32_t otherSize = w * h * 2;
    uint32_t myEnd = address + mySize;
    uint32_t otherEnd = addr + otherSize;
    return (address < otherEnd) && (addr < myEnd);
}

// ═══════════════════════════════════════════════════════════════
//  Depth Compare Logic
// ═══════════════════════════════════════════════════════════════

const std::array<uint16_t, 16> DepthCompare::s_dzDecode = {
    0x0000, 0x0001, 0x0002, 0x0004,
    0x0008, 0x0010, 0x0020, 0x0040,
    0x0080, 0x0100, 0x0200, 0x0400,
    0x0800, 0x1000, 0x2000, 0x4000
};

bool DepthCompare::Compare(float incomingZ, float storedZ,
                            DepthCompareMode mode, ZMode zMode,
                            float decalTolerance) {
    // Decal mode: pass if approximately equal
    if (zMode == ZMode::Decal) {
        float diff = std::abs(incomingZ - storedZ);
        return diff <= decalTolerance;
    }

    switch (mode) {
    case DepthCompareMode::Never:      return false;
    case DepthCompareMode::Less:       return incomingZ < storedZ;
    case DepthCompareMode::Equal:      return std::abs(incomingZ - storedZ) < 1e-6f;
    case DepthCompareMode::LessEqual:  return incomingZ <= storedZ;
    case DepthCompareMode::Greater:    return incomingZ > storedZ;
    case DepthCompareMode::NotEqual:   return std::abs(incomingZ - storedZ) >= 1e-6f;
    case DepthCompareMode::GreaterEq:  return incomingZ >= storedZ;
    case DepthCompareMode::Always:     return true;
    default:                           return true;
    }
}

bool DepthCompare::CompareWithDeltaZ(uint16_t inZ14, uint8_t inDZ,
                                      uint16_t storedZ14, uint8_t storedDZ,
                                      ZMode zMode) {
    // Get the actual delta values
    uint16_t dzIn     = s_dzDecode[inDZ & 0xF];
    uint16_t dzStored = s_dzDecode[storedDZ & 0xF];

    // For interpenetrating mode, use max delta as tolerance
    uint16_t maxDz = std::max(dzIn, dzStored);

    switch (zMode) {
    case ZMode::Opaque: {
        // Standard: incoming must be strictly closer
        return inZ14 < storedZ14;
    }
    case ZMode::Decal: {
        // Decal: pass if within dZ tolerance (fuzzy equal)
        int32_t diff = static_cast<int32_t>(inZ14) - static_cast<int32_t>(storedZ14);
        return std::abs(diff) <= static_cast<int32_t>(maxDz);
    }
    case ZMode::Transparent: {
        // Transparent: pass if closer, considering dZ
        return inZ14 < (storedZ14 + dzStored);
    }
    case ZMode::Interpenetrate: {
        // Interpenetrating: pass if within combined dZ range
        // This handles intersecting semi-transparent surfaces
        int32_t diff = static_cast<int32_t>(inZ14) - static_cast<int32_t>(storedZ14);
        return (diff < static_cast<int32_t>(maxDz)) || (inZ14 < storedZ14);
    }
    default:
        return inZ14 < storedZ14;
    }
}

uint8_t DepthCompare::MaxDeltaZ(uint8_t dz1, uint8_t dz2) {
    return std::max(dz1, dz2);
}

uint8_t DepthCompare::ComputePixelDeltaZ(float dZdx, float dZdy) {
    // Convert the maximum gradient to a dZ code
    float maxGrad = std::max(std::abs(dZdx), std::abs(dZdy));
    uint16_t maxGradInt = static_cast<uint16_t>(maxGrad * 16384.0f);

    // Find the smallest dZ code that covers this gradient
    for (uint8_t i = 0; i < 16; ++i) {
        if (s_dzDecode[i] >= maxGradInt) return i;
    }
    return 15;
}

bool DepthCompare::ShouldSkipTest(const DepthBufferConfig& config) {
    return !config.enabled && !config.writeEnabled;
}

// ═══════════════════════════════════════════════════════════════
//  Depth Buffer Manager — Lifecycle
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::Init() {
    DepthConvert::Init();
    Reset();
    LOG_INFO("GPUDepthBufferManager initialized");
}

void GPUDepthBufferManager::Shutdown() {
    for (int i = 0; i < m_bufferCount; ++i) {
        DestroyGPUResources(m_buffers[i]);
    }
    m_bufferCount = 0;
    m_current = nullptr;
    if (m_zLutTex) {
        glDeleteTextures(1, &m_zLutTex);
        m_zLutTex = 0;
        m_zLutInitialized = false;
    }
    LOG_INFO("GPUDepthBufferManager shut down");
}

void GPUDepthBufferManager::Reset() {
    for (int i = 0; i < m_bufferCount; ++i) {
        DestroyGPUResources(m_buffers[i]);
    }
    m_bufferCount = 0;
    m_current = nullptr;
    m_config = {};
    m_stats = {};
    m_frameCounter = 0;
    m_softwareZ.assign(m_swWidth * m_swHeight, 0x3FFF);
    m_softwareDZ.assign(m_swWidth * m_swHeight, 0);
}

void GPUDepthBufferManager::SetConfig(const DepthBufferConfig& config) {
    m_config = config;
}

// ═══════════════════════════════════════════════════════════════
//  Buffer Management
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::OnSetDepthImage(uint32_t address, uint32_t width) {
    address &= 0x00FFFFFF;  // Mask to RDRAM range

    // Find existing or create new
    GPUDepthBuffer* buf = nullptr;
    for (int i = 0; i < m_bufferCount; ++i) {
        if (m_buffers[i].address == address && m_buffers[i].width == width) {
            buf = &m_buffers[i];
            break;
        }
    }

    if (!buf) {
        // Estimate height from width (common N64 resolutions)
        uint32_t height = 240;
        if (width == 640) height = 480;
        else if (width == 320) height = 240;
        else if (width == 256) height = 224;

        buf = FindOrCreate(address, width, height);
    }

    if (buf) {
        buf->lastUsedFrame = m_frameCounter;
        m_current = buf;

        // Resize software depth buffer to match current depth image
        if (m_swWidth != buf->width || m_swHeight != buf->height) {
            m_swWidth  = buf->width;
            m_swHeight = buf->height;
            m_softwareZ.assign(m_swWidth * m_swHeight, 0x3FFF);
            m_softwareDZ.assign(m_swWidth * m_swHeight, 0);
        }
    }
}

GPUDepthBuffer* GPUDepthBufferManager::FindOrCreate(uint32_t address, uint32_t width, uint32_t height) {
    // Search existing
    for (int i = 0; i < m_bufferCount; ++i) {
        if (m_buffers[i].address == address) {
            if (m_buffers[i].width != width || m_buffers[i].height != height) {
                // Size changed — recreate
                DestroyGPUResources(m_buffers[i]);
                m_buffers[i].width = width;
                m_buffers[i].height = height;
                CreateGPUResources(m_buffers[i]);
            }
            m_buffers[i].lastUsedFrame = m_frameCounter;
            return &m_buffers[i];
        }
    }

    // Create new
    if (m_bufferCount >= MAX_DEPTH_BUFFERS) {
        EvictOldBuffers();
        if (m_bufferCount >= MAX_DEPTH_BUFFERS) {
            LOG_WARN("Depth buffer pool full, evicting oldest");
            DestroyGPUResources(m_buffers[0]);
            for (int i = 1; i < m_bufferCount; ++i) {
                m_buffers[i - 1] = m_buffers[i];
            }
            m_bufferCount--;
        }
    }

    GPUDepthBuffer& buf = m_buffers[m_bufferCount++];
    buf = {};
    buf.address = address;
    buf.width = width;
    buf.height = height;
    buf.lastUsedFrame = m_frameCounter;
    CreateGPUResources(buf);

    m_stats.activeBuffers = m_bufferCount;
    return &buf;
}

GPUDepthBuffer* GPUDepthBufferManager::FindByAddress(uint32_t address) const {
    address &= 0x00FFFFFF;
    for (int i = 0; i < m_bufferCount; ++i) {
        if (m_buffers[i].address == address) {
            return const_cast<GPUDepthBuffer*>(&m_buffers[i]);
        }
    }
    return nullptr;
}

void GPUDepthBufferManager::CreateGPUResources(GPUDepthBuffer& buf) {
    if (buf.width == 0 || buf.height == 0) return;

    // Create depth texture using DSA
    glCreateTextures(GL_TEXTURE_2D, 1, &buf.glDepthTexture);
    glTextureStorage2D(buf.glDepthTexture, 1, GL_DEPTH_COMPONENT32F, buf.width, buf.height);

    glTextureParameteri(buf.glDepthTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(buf.glDepthTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(buf.glDepthTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(buf.glDepthTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create FBO with depth attachment only
    glCreateFramebuffers(1, &buf.glDepthFBO);
    glNamedFramebufferTexture(buf.glDepthFBO, GL_DEPTH_ATTACHMENT, buf.glDepthTexture, 0);
    glNamedFramebufferDrawBuffer(buf.glDepthFBO, GL_NONE);
    glNamedFramebufferReadBuffer(buf.glDepthFBO, GL_NONE);

    GLenum status = glCheckNamedFramebufferStatus(buf.glDepthFBO, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Depth FBO incomplete: 0x{:X}", status);
    }
}

void GPUDepthBufferManager::DestroyGPUResources(GPUDepthBuffer& buf) {
    if (buf.glDepthFBO) {
        glDeleteFramebuffers(1, &buf.glDepthFBO);
        buf.glDepthFBO = 0;
    }
    if (buf.glDepthTexture) {
        glDeleteTextures(1, &buf.glDepthTexture);
        buf.glDepthTexture = 0;
    }
    if (buf.glZImageFBO) {
        glDeleteFramebuffers(1, &buf.glZImageFBO);
        buf.glZImageFBO = 0;
    }
    if (buf.glZImageTex) {
        glDeleteTextures(1, &buf.glZImageTex);
        buf.glZImageTex = 0;
    }
    if (buf.glDeltaZImageTex) {
        glDeleteTextures(1, &buf.glDeltaZImageTex);
        buf.glDeltaZImageTex = 0;
    }
    if (buf.glResolvedDepthTex) {
        glDeleteTextures(1, &buf.glResolvedDepthTex);
        buf.glResolvedDepthTex = 0;
    }
    buf.resolved = false;
}

void GPUDepthBufferManager::EvictOldBuffers() {
    constexpr uint32_t MAX_AGE = 30;  // Evict after 30 unused frames

    for (int i = m_bufferCount - 1; i >= 0; --i) {
        if (&m_buffers[i] == m_current) continue;  // Never evict current

        uint32_t age = m_frameCounter - m_buffers[i].lastUsedFrame;
        if (age >= MAX_AGE) {
            DestroyGPUResources(m_buffers[i]);
            // Shift remaining
            for (int j = i + 1; j < m_bufferCount; ++j) {
                m_buffers[j - 1] = m_buffers[j];
            }
            m_bufferCount--;
            if (m_current > &m_buffers[i]) m_current--;
        }
    }
    m_stats.activeBuffers = m_bufferCount;
}

// ═══════════════════════════════════════════════════════════════
//  Depth Operations
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::ClearDepthBuffer(float clearValue) {
    if (!m_current) return;
    m_current->clearValue = clearValue;
    m_current->cleared = true;
    glClearNamedFramebufferfv(m_current->glDepthFBO, GL_DEPTH, 0, &clearValue);
}

void GPUDepthBufferManager::ClearDepthBufferN64(uint16_t clearZ, uint16_t clearDZ) {
    float clearValue = DepthConvert::N64ZToFloat(clearZ);
    ClearDepthBuffer(clearValue);
}

// ═══════════════════════════════════════════════════════════════
//  GPU ↔ CPU Copy
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::CopyToGPU(GPUDepthBuffer& buf, const uint8_t* rdram, uint32_t rdramSize) {
    if (buf.address + buf.width * buf.height * 2 > rdramSize) return;
    if (!buf.glDepthTexture) return;

    m_stats.bufferUploads++;

    // Convert N64 16-bit Z values to float depth
    std::vector<float> depthData(buf.width * buf.height);
    const uint16_t* src = reinterpret_cast<const uint16_t*>(rdram + buf.address);

    for (uint32_t y = 0; y < buf.height; ++y) {
        for (uint32_t x = 0; x < buf.width; ++x) {
            uint32_t idx = y * buf.width + x;
            // N64 is big-endian
            uint16_t raw = (src[idx] >> 8) | (src[idx] << 8);
            uint16_t z14;
            uint8_t dz;
            DepthConvert::DecodeRDRAM16(raw, z14, dz);
            depthData[idx] = DepthConvert::N64ZToFloat(z14);
        }
    }

    glTextureSubImage2D(buf.glDepthTexture, 0, 0, 0, buf.width, buf.height,
                        GL_DEPTH_COMPONENT, GL_FLOAT, depthData.data());
    buf.dirty = false;
}

void GPUDepthBufferManager::CopyFromGPU(const GPUDepthBuffer& buf, uint8_t* rdram, uint32_t rdramSize) {
    if (buf.address + buf.width * buf.height * 2 > rdramSize) return;
    if (!buf.glDepthTexture) return;

    m_stats.bufferDownloads++;

    // Read float depth from GPU
    std::vector<float> depthData(buf.width * buf.height);
    glGetTextureImage(buf.glDepthTexture, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                      static_cast<GLsizei>(depthData.size() * sizeof(float)), depthData.data());

    // Convert back to N64 16-bit format
    uint16_t* dst = reinterpret_cast<uint16_t*>(rdram + buf.address);
    for (uint32_t y = 0; y < buf.height; ++y) {
        for (uint32_t x = 0; x < buf.width; ++x) {
            uint32_t idx = y * buf.width + x;
            uint16_t z14 = DepthConvert::FloatToN64Z(depthData[idx]);
            uint8_t dz4 = DepthConvert::GetDeltaZ(z14);
            uint16_t encoded = DepthConvert::EncodeRDRAM16(z14, dz4);
            // Write big-endian
            dst[idx] = (encoded >> 8) | (encoded << 8);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  Software Depth Test
// ═══════════════════════════════════════════════════════════════

bool GPUDepthBufferManager::TestAndWrite(int32_t x, int32_t y, float depth, float deltaZ) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(m_swWidth) || y >= static_cast<int32_t>(m_swHeight)) return false;
    if (m_softwareZ.empty()) return true;

    uint32_t idx = static_cast<uint32_t>(y) * m_swWidth + static_cast<uint32_t>(x);

    uint16_t inZ14 = DepthConvert::FloatToN64Z(depth);
    uint8_t inDZ = DepthCompare::ComputePixelDeltaZ(deltaZ, deltaZ);

    uint16_t storedZ14 = m_softwareZ[idx];
    uint8_t storedDZ = m_softwareDZ[idx];

    // Perform depth comparison
    bool pass = false;
    if (m_config.enabled) {
        pass = DepthCompare::CompareWithDeltaZ(inZ14, inDZ, storedZ14, storedDZ, m_config.zMode);
    } else {
        pass = true;
    }

    if (pass) {
        m_stats.depthTestsPassed++;
        if (m_config.writeEnabled) {
            m_softwareZ[idx] = inZ14;
            m_softwareDZ[idx] = inDZ;
            m_stats.depthWrites++;
        }
    } else {
        m_stats.depthTestsFailed++;
    }

    return pass;
}

// ═══════════════════════════════════════════════════════════════
//  Z-Image Textures (for shader-based N64 depth compare)
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::InitDepthImageTextures(GPUDepthBuffer& buf) {
    if (buf.glZImageTex != 0) return;  // Already initialized
    if (buf.width == 0 || buf.height == 0) return;

    // Z-image texture: stores N64 14-bit Z as float in R32F
    glCreateTextures(GL_TEXTURE_2D, 1, &buf.glZImageTex);
    glTextureStorage2D(buf.glZImageTex, 1, GL_R32F, buf.width, buf.height);
    glTextureParameteri(buf.glZImageTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(buf.glZImageTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Delta-Z image texture: stores dZ as float in R32F
    glCreateTextures(GL_TEXTURE_2D, 1, &buf.glDeltaZImageTex);
    glTextureStorage2D(buf.glDeltaZImageTex, 1, GL_R32F, buf.width, buf.height);
    glTextureParameteri(buf.glDeltaZImageTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(buf.glDeltaZImageTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // FBO for clearing these textures (attach as color0 and color1)
    glCreateFramebuffers(1, &buf.glZImageFBO);
    glNamedFramebufferTexture(buf.glZImageFBO, GL_COLOR_ATTACHMENT0, buf.glZImageTex, 0);
    glNamedFramebufferTexture(buf.glZImageFBO, GL_COLOR_ATTACHMENT1, buf.glDeltaZImageTex, 0);

    GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glNamedFramebufferDrawBuffers(buf.glZImageFBO, 2, drawBuffers);
}

void GPUDepthBufferManager::ClearDepthImageTextures(GPUDepthBuffer& buf) {
    if (!buf.glZImageFBO) return;

    // Clear Z-image to max depth (1.0) and delta-Z to 0
    float clearZ[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    float clearDZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glClearNamedFramebufferfv(buf.glZImageFBO, GL_COLOR, 0, clearZ);
    glClearNamedFramebufferfv(buf.glZImageFBO, GL_COLOR, 1, clearDZ);
}

void GPUDepthBufferManager::BindDepthImageTextures(const GPUDepthBuffer& buf,
                                                    uint32_t zUnit, uint32_t dzUnit) {
    if (buf.glZImageTex)
        glBindTextureUnit(zUnit, buf.glZImageTex);
    if (buf.glDeltaZImageTex)
        glBindTextureUnit(dzUnit, buf.glDeltaZImageTex);
}

// ═══════════════════════════════════════════════════════════════
//  Depth Attachment Helper
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::SetDepthAttachment(const GPUDepthBuffer& buf, uint32_t fbo) {
    if (!buf.glDepthTexture || !fbo) return;
    glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, buf.glDepthTexture, 0);
}

// ═══════════════════════════════════════════════════════════════
//  MSAA Depth Resolve
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::ResolveDepthBuffer(GPUDepthBuffer& buf, uint32_t msaaFBO) {
    if (!msaaFBO || !buf.glDepthTexture) return;

    // Lazily create single-sample resolve texture
    if (!buf.glResolvedDepthTex) {
        glCreateTextures(GL_TEXTURE_2D, 1, &buf.glResolvedDepthTex);
        glTextureStorage2D(buf.glResolvedDepthTex, 1, GL_DEPTH_COMPONENT32F,
                           buf.width, buf.height);
        glTextureParameteri(buf.glResolvedDepthTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(buf.glResolvedDepthTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // Use a temporary FBO for the resolve target
    uint32_t resolveFBO = 0;
    glCreateFramebuffers(1, &resolveFBO);
    glNamedFramebufferTexture(resolveFBO, GL_DEPTH_ATTACHMENT, buf.glResolvedDepthTex, 0);
    glNamedFramebufferDrawBuffer(resolveFBO, GL_NONE);
    glNamedFramebufferReadBuffer(resolveFBO, GL_NONE);

    // Blit MSAA depth → single-sample
    glBlitNamedFramebuffer(msaaFBO, resolveFBO,
                           0, 0, buf.width, buf.height,
                           0, 0, buf.width, buf.height,
                           GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glDeleteFramebuffers(1, &resolveFBO);
    buf.resolved = true;
}

// ═══════════════════════════════════════════════════════════════
//  Z Lookup Table Texture
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::InitZLUT() {
    if (m_zLutInitialized) return;

    DepthConvert::Init();

    // Create a 1D-style texture (16384 x 1) mapping N64 Z14 → float depth
    glCreateTextures(GL_TEXTURE_2D, 1, &m_zLutTex);
    glTextureStorage2D(m_zLutTex, 1, GL_R32F, 0x4000, 1);

    // Fill the LUT
    std::array<float, 0x4000> lutData;
    for (uint32_t i = 0; i < 0x4000; ++i) {
        lutData[i] = DepthConvert::N64ZToFloat(static_cast<uint16_t>(i));
    }

    glTextureSubImage2D(m_zLutTex, 0, 0, 0, 0x4000, 1, GL_RED, GL_FLOAT, lutData.data());
    glTextureParameteri(m_zLutTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_zLutTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_zLutTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

    m_zLutInitialized = true;
    LOG_INFO("Z LUT texture initialized (16384 entries)");
}

void GPUDepthBufferManager::BindZLUT(uint32_t textureUnit) {
    if (m_zLutTex)
        glBindTextureUnit(textureUnit, m_zLutTex);
}

// ═══════════════════════════════════════════════════════════════
//  Frame Management
// ═══════════════════════════════════════════════════════════════

void GPUDepthBufferManager::OnFrameEnd() {
    m_frameCounter++;
    if ((m_frameCounter & 63) == 0) {
        EvictOldBuffers();
    }
}

} // namespace Nova64::Video
