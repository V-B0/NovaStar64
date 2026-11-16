/**
 * @file DepthBuffer.cpp
 * @brief N64 RDP 18-bit depth: piecewise-linear encoding, dZ fuzzy compare, GPU buffer management with
 *        GL4.6 DSA, and a software fallback depth test.
 */

#include "DepthBuffer.hpp"
#include "FramebufferManager.hpp"
#include "../Common/Logger.hpp"

#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Nova64::Video {

GPUDepthBufferManager g_depthManager;

// --- Depth Conversion Tables — Static Data ---

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
    s_zShifts = {6, 5, 4, 3, 2, 1, 0, 0};

    // Base values for each segment
    s_zBases = {0, 32, 64, 96, 128, 160, 192, 224};

    // Build forward LUT: 14-bit Z → float [0, 1]
    // The N64 Z range is 0x0000 to 0x3FFF in encoded form
    // Decoded range is approximately 0 to 0xFFFC
    for (uint32_t z14 = 0; z14 < 0x4000; ++z14) {
        // Determine segment from upper 3 bits (bits 13:11)
        uint32_t seg = (z14 >> 11) & 0x7;
        uint32_t offset = z14 & 0x7FF;      // Lower 11 bits
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

float DepthConvert::N64ZToFloat(uint16_t z14) { return s_zToFloat[z14 & 0x3FFF]; }

uint16_t DepthConvert::FloatToN64Z(float depth) {
    depth = std::clamp(depth, 0.0f, 1.0f);
    [[maybe_unused]] uint32_t target = static_cast<uint32_t>(depth * 65536.0f);

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

uint8_t DepthConvert::GetDeltaZ(uint16_t z14) { return s_zToDz[z14 & 0x3FFF]; }

uint16_t DepthConvert::EncodeRDRAM16(uint16_t z14, uint8_t dz4) {
    // RDP stores: z[13:2] in bits 15:4, z[1:0] in bits 3:2, dz[1:0] in bits 1:0
    return static_cast<uint16_t>((z14 << 2) | (dz4 & 0x3));
}

void DepthConvert::DecodeRDRAM16(uint16_t rdram, uint16_t& z14, uint8_t& dz4Lower) {
    z14 = (rdram >> 2) & 0x3FFF;
    dz4Lower = static_cast<uint8_t>(rdram & 0x3);
}

// --- Depth Buffer Config ---

// --- GPUDepthBuffer ---

// --- Depth Compare Logic ---

const std::array<uint16_t, 16> DepthCompare::s_dzDecode = {0x0000, 0x0001, 0x0002, 0x0004, 0x0008, 0x0010,
                                                           0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400,
                                                           0x0800, 0x1000, 0x2000, 0x4000};

bool DepthCompare::CompareWithDeltaZ(uint16_t inZ14, uint8_t inDZ, uint16_t storedZ14, uint8_t storedDZ, ZMode zMode) {
    // Get the actual delta values
    uint16_t dzIn = s_dzDecode[inDZ & 0xF];
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
    default: return inZ14 < storedZ14;
    }
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

// --- Depth Buffer Manager — Lifecycle ---

void GPUDepthBufferManager::Init() {
    DepthConvert::Init();
    Reset();
    LOG_INFO("GPUDepthBufferManager initialized");
}

void GPUDepthBufferManager::Shutdown() {
    // Only destroy GL resources if using OpenGL backend.
    // Vulkan backend doesn't have GL textures/FBOs, so skipping GL deletes prevents
    // access violations from glDeleteTextures/glDeleteFramebuffers without GL context.
    if (g_fbManager.IsGL()) {
        for (int i = 0; i < m_bufferCount; ++i) {
            DestroyGPUResources(m_buffers[i]);
        }
        if (m_zLutTex) {
            glDeleteTextures(1, &m_zLutTex);
            m_zLutTex = 0;
            m_zLutInitialized = false;
        }
    }
    m_bufferCount = 0;
    m_current = nullptr;
    LOG_INFO("GPUDepthBufferManager shut down");
}

void GPUDepthBufferManager::Reset() {
    // Only destroy GL resources if using OpenGL backend
    if (g_fbManager.IsGL()) {
        for (int i = 0; i < m_bufferCount; ++i) {
            DestroyGPUResources(m_buffers[i]);
        }
    }
    m_bufferCount = 0;
    m_current = nullptr;
    m_config = {};
    m_stats = {};
    m_frameCounter = 0;
    m_softwareZ.assign(m_swWidth * m_swHeight, 0x3FFF);
    m_softwareDZ.assign(m_swWidth * m_swHeight, 0);
}

// --- Buffer Management ---

void GPUDepthBufferManager::OnSetDepthImage(uint32_t address, uint32_t width) {
    address &= 0x00FFFFFF; // Mask to RDRAM range

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
            m_swWidth = buf->width;
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

void GPUDepthBufferManager::CreateGPUResources(GPUDepthBuffer& buf) {
    if (!g_fbManager.IsGL()) return; // No OpenGL context in Vulkan mode
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
    constexpr uint32_t MAX_AGE = 30; // Evict after 30 unused frames

    for (int i = m_bufferCount - 1; i >= 0; --i) {
        if (&m_buffers[i] == m_current) continue; // Never evict current

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

// --- Depth Operations ---

void GPUDepthBufferManager::ClearDepthBuffer(float clearValue) {
    if (!m_current) return;
    m_current->clearValue = clearValue;
    m_current->cleared = true;
    glClearNamedFramebufferfv(m_current->glDepthFBO, GL_DEPTH, 0, &clearValue);
}

// --- GPU ↔ CPU Copy ---

// --- Software Depth Test ---

// --- Z-Image Textures (for shader-based N64 depth compare) ---

void GPUDepthBufferManager::InitDepthImageTextures(GPUDepthBuffer& buf, uint32_t resScale) {
    if (!g_fbManager.IsGL()) return;  // No OpenGL context in Vulkan mode
    if (buf.glZImageTex != 0) return; // Already initialized
    if (buf.width == 0 || buf.height == 0) return;

    // Create Z-image textures at scaled resolution so each screen pixel
    // gets its own depth texel, avoiding cross-pixel contamination.
    uint32_t imgW = buf.width * std::max(resScale, 1u);
    uint32_t imgH = buf.height * std::max(resScale, 1u);
    buf.zImageWidth = imgW;
    buf.zImageHeight = imgH;

    // Z-image texture: stores N64 14-bit Z as float in R32F
    glCreateTextures(GL_TEXTURE_2D, 1, &buf.glZImageTex);
    glTextureStorage2D(buf.glZImageTex, 1, GL_R32F, imgW, imgH);
    glTextureParameteri(buf.glZImageTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(buf.glZImageTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Delta-Z image texture: stores dZ as float in R32F
    glCreateTextures(GL_TEXTURE_2D, 1, &buf.glDeltaZImageTex);
    glTextureStorage2D(buf.glDeltaZImageTex, 1, GL_R32F, imgW, imgH);
    glTextureParameteri(buf.glDeltaZImageTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(buf.glDeltaZImageTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // FBO for clearing these textures (attach as color0 and color1)
    glCreateFramebuffers(1, &buf.glZImageFBO);
    glNamedFramebufferTexture(buf.glZImageFBO, GL_COLOR_ATTACHMENT0, buf.glZImageTex, 0);
    glNamedFramebufferTexture(buf.glZImageFBO, GL_COLOR_ATTACHMENT1, buf.glDeltaZImageTex, 0);

    GLenum drawBuffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glNamedFramebufferDrawBuffers(buf.glZImageFBO, 2, drawBuffers);

    // Set backend-native image handles (GL: texture name → uint64_t)
    buf.zImageHandle = static_cast<uint64_t>(buf.glZImageTex);
    buf.deltaZImageHandle = static_cast<uint64_t>(buf.glDeltaZImageTex);
}

void GPUDepthBufferManager::ClearDepthImageTextures(GPUDepthBuffer& buf) {
    if (!g_fbManager.IsGL()) return; // No OpenGL context in Vulkan mode
    if (!buf.glZImageTex || !buf.glDeltaZImageTex) return;

    // Clear Z-image to 1.0 (normalized max depth) and delta-Z to 0.
    // Uses glClearTexImage instead of FBO clear so the data is reliably
    // visible to subsequent imageLoad() calls.  Some Intel drivers do not
    // propagate glClearNamedFramebufferfv writes to the image-load path.
    float clearZ = 1.0f;
    float clearDZ = 0.0f;
    glClearTexImage(buf.glZImageTex, 0, GL_RED, GL_FLOAT, &clearZ);
    glClearTexImage(buf.glDeltaZImageTex, 0, GL_RED, GL_FLOAT, &clearDZ);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

void GPUDepthBufferManager::BindDepthImageTextures(const GPUDepthBuffer& buf, uint32_t zUnit, uint32_t dzUnit) {
    if (buf.glZImageTex) {
        glBindTextureUnit(zUnit, buf.glZImageTex);
        glBindImageTexture(0, buf.glZImageTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    }
    if (buf.glDeltaZImageTex) {
        glBindTextureUnit(dzUnit, buf.glDeltaZImageTex);
        glBindImageTexture(1, buf.glDeltaZImageTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
    }
}

// --- Depth Attachment Helper ---

// --- MSAA Depth Resolve ---

// --- Z Lookup Table Texture ---

void GPUDepthBufferManager::InitZLUT() {
    if (m_zLutInitialized) return;

    DepthConvert::Init();

    // Create a 512x512 R32F texture mapping 18-bit Z values → compressed Z14 values.
    // 512*512 = 262144 = 0x40000 entries (full 18-bit Z range).
    // Algorithm matches GLideN64: leading-one exponent scan to produce Z14.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_zLutTex);
    glTextureStorage2D(m_zLutTex, 1, GL_R32F, 512, 512);

    std::vector<float> lutData(0x40000);
    for (uint32_t i = 0; i < 0x40000; ++i) {
        uint32_t exponent = 0;
        uint32_t testbit = 1u << 17;
        while ((i & testbit) && (exponent < 7)) {
            exponent++;
            testbit = 1u << (17 - exponent);
        }
        const uint32_t mantissa = (i >> (6 - exponent)) & 0x7FF;
        uint32_t z14 = ((exponent << 11) | mantissa) << 2;
        lutData[i] = static_cast<float>(z14);
    }

    glTextureSubImage2D(m_zLutTex, 0, 0, 0, 512, 512, GL_RED, GL_FLOAT, lutData.data());
    glTextureParameteri(m_zLutTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_zLutTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_zLutTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_zLutTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_zLutInitialized = true;
    LOG_INFO("Z LUT texture initialized (512x512, 262144 entries)");
}

// --- Frame Management ---

void GPUDepthBufferManager::OnFrameEnd() {
    m_frameCounter++;
    if ((m_frameCounter & 63) == 0) {
        EvictOldBuffers();
    }
}

} // namespace Nova64::Video
