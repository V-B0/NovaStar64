/**
 * @file RDPProcessor.cpp
 * @brief RDP command processor implementation
 */

#include "RDPProcessor.hpp"
#include "RDPUtils.hpp"
#include "../../common/log.hpp"
#include "FramebufferManager.hpp"
#include "DepthBuffer.hpp"
#include "../Common/BitUtils.hpp"
#include "../Common/RDRAMAccess.hpp"
#include "../Common/Logger.hpp"
#include "../Texture/TextureCache.hpp"
#include "../Renderer/ShaderManager.hpp"
#include "../Renderer/Renderer.hpp"

#include <glad/gl.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <random>
#include <vector>
#include <unordered_set>

// === DEBUG: Set to true to force all fragments to bright red ===============
static constexpr bool FORCE_RED_DEBUG = false;
// === DEBUG: Set to true to skip ALL depth-tested triangle draws ============
static constexpr bool DEBUG_SKIP_DEPTH_TRIS = false;
// === MLE blender toggle: true = legacy GL blend, false = imageLoad/imageStore ==
static constexpr bool DISABLE_MLE_BLENDER = false;
// === DEBUG: Disable N64 image-based depth compare (use GL depth test only) ===
static constexpr bool DISABLE_N64_DEPTH_COMPARE = false;

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Construction / Initialization
// ═══════════════════════════════════════════════════════════════════════════

RDPProcessor::RDPProcessor() {
    m_stats.Reset();
}

RDPProcessor::~RDPProcessor() {
    Shutdown();
}

bool RDPProcessor::Initialize(uint8_t* rdram, size_t rdramSize,
                               IRenderer* renderer,
                               Tex::TextureCache* textureCache,
                               Shader::ShaderManager* shaderManager) {
    m_rdram = rdram;
    m_rdramSize = rdramSize;
    m_renderer = renderer;
    m_textureCache = textureCache;
    m_shaderManager = shaderManager;
    
    m_decoder.SetRDRAM(rdram, rdramSize);
    g_fbManager.SetRDRAM(rdram);
    m_state.Reset();
    m_batch.Reset();
    m_stats.Reset();
    
    // �� Create noise texture (64x64 random R8, bound to unit 5) ��
    {
        constexpr int NOISE_SIZE = 64;
        std::mt19937 rng(42); // fixed seed for reproducibility
        std::uniform_int_distribution<int> dist(0, 255);
        std::vector<uint8_t> noiseData(NOISE_SIZE * NOISE_SIZE);
        for (auto& p : noiseData) p = static_cast<uint8_t>(dist(rng));

        glCreateTextures(GL_TEXTURE_2D, 1, &m_noiseTexture);
        glTextureStorage2D(m_noiseTexture, 1, GL_R8, NOISE_SIZE, NOISE_SIZE);
        glTextureSubImage2D(m_noiseTexture, 0, 0, 0, NOISE_SIZE, NOISE_SIZE,
                            GL_RED, GL_UNSIGNED_BYTE, noiseData.data());
        glTextureParameteri(m_noiseTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_noiseTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(m_noiseTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_noiseTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // �� Create N64 dither textures (4x4 R8, bound to unit 6) ��
    // N64 has two ordered dither patterns selectable via otherModeH rgb_dither_sel:
    //   Mode 0: Magic Square (N64 default) � values 0-7 (3 bits)
    //   Mode 1: Standard Bayer             � values 0-7 (3 bits)
    //   Mode 2: Noise dither (uses noise texture)
    //   Mode 3: Disabled
    // We store both patterns in separate textures; the active one is bound at draw time.
    {
        // N64 Magic Square dither matrix (normalized 0-7 � 0-255)
        static constexpr uint8_t magicSquare4x4[16] = {
              0, 219,  36, 255,
            146,  73, 182, 109,
            219,   0, 255,  36,
             73, 146, 109, 182
        };
        glCreateTextures(GL_TEXTURE_2D, 1, &m_ditherTexture);
        glTextureStorage2D(m_ditherTexture, 1, GL_R8, 4, 4);
        glTextureSubImage2D(m_ditherTexture, 0, 0, 0, 4, 4,
                            GL_RED, GL_UNSIGNED_BYTE, magicSquare4x4);
        glTextureParameteri(m_ditherTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_ditherTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(m_ditherTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_ditherTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // N64 Bayer dither matrix (normalized 0-7 � 0-255)
        static constexpr uint8_t bayer4x4[16] = {
              0, 146,  36, 182,
            146,   0, 182,  36,
            109, 255,  73, 219,
            255, 109, 219,  73
        };
        glCreateTextures(GL_TEXTURE_2D, 1, &m_ditherBayerTexture);
        glTextureStorage2D(m_ditherBayerTexture, 1, GL_R8, 4, 4);
        glTextureSubImage2D(m_ditherBayerTexture, 0, 0, 0, 4, 4,
                            GL_RED, GL_UNSIGNED_BYTE, bayer4x4);
        glTextureParameteri(m_ditherBayerTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_ditherBayerTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(m_ditherBayerTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_ditherBayerTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // �� Create debug white texture (1x1 RGBA8, for diagnostic tests) ��
    {
        uint32_t white = 0xFFFFFFFF;
        glCreateTextures(GL_TEXTURE_2D, 1, &m_debugWhiteTexture);
        glTextureStorage2D(m_debugWhiteTexture, 1, GL_RGBA8, 1, 1);
        glTextureSubImage2D(m_debugWhiteTexture, 0, 0, 0, 1, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, &white);
        glTextureParameteri(m_debugWhiteTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_debugWhiteTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(m_debugWhiteTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_debugWhiteTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    m_initialized = true;
    
    NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "RDP Processor initialized (RDRAM: {} MB)",
             rdramSize / (1024 * 1024));
    return true;
}

void RDPProcessor::Shutdown() {
    if (!m_initialized) return;
    
    FlushBatch();

    if (m_noiseTexture)       { glDeleteTextures(1, &m_noiseTexture);       m_noiseTexture = 0; }
    if (m_ditherTexture)      { glDeleteTextures(1, &m_ditherTexture);      m_ditherTexture = 0; }
    if (m_ditherBayerTexture) { glDeleteTextures(1, &m_ditherBayerTexture); m_ditherBayerTexture = 0; }
    if (m_shadowTLUTTexture)  { glDeleteTextures(1, &m_shadowTLUTTexture);  m_shadowTLUTTexture = 0; }

    m_state.Reset();
    m_commandBuffer.Reset();
    m_initialized = false;
    
    NOVA_LOG(LogLevel::Info, LogSubsystem::Core,
             "RDP Processor shutdown (commands: {}, triangles: {}, rects: {})",
             m_stats.commandsProcessed, m_stats.trianglesDrawn, m_stats.rectsDrawn);
}

void RDPProcessor::Reset(bool coldReset) {
    FlushBatch();
    uint32_t savedFrameCount = m_state.frameCount;
    float savedWidescreenScale = m_state.rdp.geometry.widescreenScale;
    m_state.Reset();
    if (!coldReset) {
        // Warm reset: preserve frame counter and widescreen hack across display lists
        m_state.frameCount = savedFrameCount;
        m_state.rdp.geometry.widescreenScale = savedWidescreenScale;
    }
    m_commandBuffer.Reset();
    m_batch.Reset();
    
    if (coldReset) {
        m_stats.Reset();
    } else {
        m_stats.ResetFrame();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Display List Processing
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::ProcessDList(uint32_t startAddress, uint32_t endAddress) {
    if (!m_initialized || !m_rdram) return;
    
    // Validate addresses
    if (startAddress >= m_rdramSize || endAddress > m_rdramSize ||
        startAddress >= endAddress) {
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
                 "Invalid DList range: 0x{:08X} - 0x{:08X}", startAddress, endAddress);
        return;
    }
    
    m_state.displayListActive = true;
    m_stats.displayListsProcessed++;
    
    // Read commands from RDRAM into command buffer
    uint32_t numWords = (endAddress - startAddress) / 4;
    
    // RDRAM is stored in native 32-bit word order (PJ64-style)
    std::vector<uint32_t> words(numWords);
    for (uint32_t i = 0; i < numWords; i++) {
        uint32_t addr = startAddress + i * 4;
        words[i] = *reinterpret_cast<const uint32_t*>(m_rdram + addr);
    }
    
    ProcessCommandBuffer(words.data(), static_cast<int>(numWords));
    
    m_state.displayListActive = false;
}

void RDPProcessor::ProcessRDPCommand(uint64_t command) {
    uint32_t words[2];
    words[0] = static_cast<uint32_t>(command >> 32);
    words[1] = static_cast<uint32_t>(command);
    ProcessCommandBuffer(words, 2);
}

void RDPProcessor::ProcessCommandBuffer(const uint32_t* commands, int wordCount) {
    int offset = 0;
    
    while (offset < wordCount) {
        const uint32_t* current = commands + offset;
        int remaining = wordCount - offset;
        
        if (remaining < 2) break; // Need at least 2 words (64 bits)
        
        RDPCommandID cmdID = m_decoder.DecodeCommand(current, remaining);
        
        if (m_logCommands) {
            m_lastCommandDesc = RDPCommandDecoder::DescribeCommand(cmdID);
        }
        
        m_stats.commandsProcessed++;
        
        switch (cmdID) {
            case RDPCommandID::Nop:
                offset += 2;
                break;
                
            // ── Triangles ──
            case RDPCommandID::TriNoShade:
            case RDPCommandID::TriNoShadeZ:
            case RDPCommandID::TriTexNoShade:
            case RDPCommandID::TriTexNoShadeZ:
            case RDPCommandID::TriShade:
            case RDPCommandID::TriShadeZ:
            case RDPCommandID::TriTexShade:
            case RDPCommandID::TriTexShadeZ: {
                bool shade   = (static_cast<int>(cmdID) & 0x04) != 0;
                bool texture = (static_cast<int>(cmdID) & 0x02) != 0;
                bool zbuffer = (static_cast<int>(cmdID) & 0x01) != 0;
                int triSize = 8; // Edge coefficients: 4 � 64-bit words = 8 � 32-bit words
                if (shade)   triSize += 16; // Shade: 8 � 64-bit = 16 � 32-bit
                if (texture) triSize += 16; // Texture: 8 � 64-bit = 16 � 32-bit
                if (zbuffer) triSize += 4;  // Z: 2 � 64-bit = 4 � 32-bit
                
                if (remaining >= triSize) {
                    HandleTriangle(current, triSize);
                }
                offset += triSize;
                break;
            }
                
            // ── Rectangles ──
            case RDPCommandID::TextureRectangle:
                if (remaining >= 4) HandleTextureRect(current);
                offset += 4;
                break;
                
            case RDPCommandID::TextureRectFlip:
                if (remaining >= 4) HandleTextureRectFlip(current);
                offset += 4;
                break;
                
            case RDPCommandID::FillRect:
                HandleFillRect(current);
                offset += 2;
                break;
                
            // ── Syncs ──
            case RDPCommandID::SyncFull:
                HandleSyncFull();
                offset += 2;
                break;
                
            case RDPCommandID::SyncPipe:
                HandleSyncPipe();
                offset += 2;
                break;
                
            case RDPCommandID::SyncTile:
                HandleSyncTile();
                offset += 2;
                break;
                
            case RDPCommandID::SyncLoad:
                HandleSyncLoad();
                offset += 2;
                break;
                
            // ── State Setting ──
            case RDPCommandID::SetOtherModes:
                HandleSetOtherModes(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetCombine:
                HandleSetCombine(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetTile:
                HandleSetTile(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetTileSize:
                HandleSetTileSize(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetScissor:
                HandleSetScissor(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetColorImage:
                HandleSetColorImage(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetMaskImage:
                HandleSetMaskImage(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetTextureImage:
                HandleSetTextureImage(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetFillColor:
                HandleSetFillColor(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetFogColor:
                HandleSetFogColor(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetBlendColor:
                HandleSetBlendColor(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetPrimColor:
                HandleSetPrimColor(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetEnvColor:
                HandleSetEnvColor(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetPrimDepth:
                HandleSetPrimDepth(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetConvert:
                HandleSetConvert(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetKeyGB:
                HandleSetKeyGB(current);
                offset += 2;
                break;
                
            case RDPCommandID::SetKeyR:
                HandleSetKeyR(current);
                offset += 2;
                break;
                
            // ── Texture Loads ──
            case RDPCommandID::LoadBlock:
                HandleLoadBlock(current);
                offset += 2;
                break;
                
            case RDPCommandID::LoadTile:
                HandleLoadTile(current);
                offset += 2;
                break;
                
            case RDPCommandID::LoadTLUT:
                HandleLoadTLUT(current);
                offset += 2;
                break;
                
            case RDPCommandID::ScrollTex:
                HandleScrollTex(current);
                offset += 2;
                break;
                
            default:
                // Unknown command � skip 2 words
                NOVA_LOG(LogLevel::Debug, LogSubsystem::RDP,
                         "Unknown RDP command: 0x{:02X}", static_cast<int>(cmdID));
                offset += 2;
                break;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command Handlers
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::HandleSetOtherModes(const uint32_t* words) {
    m_decoder.DecodeSetOtherModes(words, m_state.rdp);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetCombine(const uint32_t* words) {
    m_decoder.DecodeSetCombine(words, m_state.rdp);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetTile(const uint32_t* words) {
    int tileIdx = (words[1] >> 24) & 0x07;
    m_decoder.DecodeSetTile(words, m_state.rdp.tiles[tileIdx]);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetTileSize(const uint32_t* words) {
    int tileIdx = (words[1] >> 24) & 0x07;
    m_decoder.DecodeSetTileSize(words, m_state.rdp.tiles[tileIdx]);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetScissor(const uint32_t* words) {
    // Flush existing batch before changing scissor � N64 scissor applies per-triangle
    // and we use a single global scissor in FlushBatch, so a mid-batch change would
    // apply the new scissor to all previously-batched triangles incorrectly.
    if (!m_batch.drawCalls.empty()) FlushBatch();
    m_decoder.DecodeSetScissor(words, m_state.rdp.scissor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Scissor);
    m_stats.stateChanges++;
    // Keep FramebufferManager in sync so new FBOs get the right height
    uint32_t scissorX = m_state.rdp.scissor.xh >> 2;
    uint32_t scissorY = m_state.rdp.scissor.yh >> 2;
    uint32_t scissorW = m_state.rdp.scissor.xl >> 2;
    uint32_t scissorH = m_state.rdp.scissor.yl >> 2;
    g_fbManager.SetScissorHint(scissorX, scissorY, scissorW, scissorH);
}

void RDPProcessor::HandleSetColorImage(const uint32_t* words) {
    // Remember the previous color image address for mid-frame readback
    uint32_t prevAddr = m_state.rdp.colorImage.address;

    FlushBatch(); // Changing render target requires flush (snapshot handled inside)

    m_decoder.DecodeSetColorImage(words, m_state.rdp.colorImage);
    m_state.rdp.colorImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.colorImage.address);

    // NOTE: We intentionally do NOT do a proactive CopyFromGPU here.
    // The reactive copies in LoadBlock/LoadTile already handle reading
    // from GPU framebuffers when needed.  A proactive copy here would
    // overwrite unrelated RDRAM data (e.g. palettes) that overlaps
    // with the framebuffer address range.

    m_state.rdp.MarkDirty(RDPDirtyFlag::ColorImage);
    m_stats.stateChanges++;

    g_fbManager.OnSetColorImage(m_state.rdp.colorImage.address,
                                m_state.rdp.colorImage.width,
                                static_cast<uint32_t>(m_state.rdp.colorImage.format),
                                static_cast<uint32_t>(m_state.rdp.colorImage.size));
    // Evict any stale TextureCache entries at this address � it is now a GPU render target
    if (m_textureCache) {
        uint32_t fbSizeBytes = m_state.rdp.colorImage.width * 240 * 4; // conservative upper bound
        m_textureCache->InvalidateRange(m_state.rdp.colorImage.address, fbSizeBytes);
    }
}

void RDPProcessor::HandleSetMaskImage(const uint32_t* words) {
    FlushBatch();
    m_decoder.DecodeSetMaskImage(words, m_state.rdp.zbufferImage);
    m_state.rdp.zbufferImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.zbufferImage.address);
    m_state.rdp.MarkDirty(RDPDirtyFlag::ZBufferImage);
    m_stats.stateChanges++;

    g_fbManager.OnSetDepthImage(m_state.rdp.zbufferImage.address);

    // Wire into the N64-accurate depth buffer manager
    g_depthManager.OnSetDepthImage(m_state.rdp.zbufferImage.address,
                                   m_state.rdp.colorImage.width);

    // Proactively initialize Z-image textures so SF_N64_DEPTH_COMPARE can be
    // set on the very first draw call after SetMaskImage.  Without this, the
    // first frame has glZImageTex == 0 and falls back to simple GL depth.
    // The Z-image is cleared to max depth (1.0) only when first created;
    // afterwards the game clears it via FillRect to the depth buffer address.
    auto* depthBuf = g_depthManager.GetCurrent();
    if (depthBuf) {
        bool isNew = (depthBuf->glZImageTex == 0);
        g_depthManager.InitDepthImageTextures(*depthBuf, g_fbManager.GetResolutionScale());
        if (isNew) {
            g_depthManager.ClearDepthImageTextures(*depthBuf);
        }
    }
}

void RDPProcessor::HandleSetTextureImage(const uint32_t* words) {
    m_decoder.DecodeSetTextureImage(words, m_state.rdp.textureImage);
    m_state.rdp.textureImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.textureImage.address);
    m_state.rdp.MarkDirty(RDPDirtyFlag::TextureImage);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetFillColor(const uint32_t* words) {
    // Store the raw 32-bit fill color register so we can re-decode at draw time
    // (in case the color image size changes between SetFillColor and FillRect).
    uint32_t raw = words[1];
    m_state.rdp.fillColorRaw = raw;
    DecodeFillColor(raw, m_state.rdp.colorImage.size,
                    m_state.rdp.fillColor.r, m_state.rdp.fillColor.g,
                    m_state.rdp.fillColor.b, m_state.rdp.fillColor.a);
    m_state.rdp.MarkDirty(RDPDirtyFlag::FillColor);
}

void RDPProcessor::HandleSetFogColor(const uint32_t* words) {
    m_decoder.DecodeSetFogColor(words, m_state.rdp.fogColor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::FogColor);
}

void RDPProcessor::HandleSetBlendColor(const uint32_t* words) {
    m_decoder.DecodeSetBlendColor(words, m_state.rdp.blendColor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::BlendColor);
}

void RDPProcessor::HandleSetPrimColor(const uint32_t* words) {
    m_decoder.DecodeSetPrimColor(words, m_state.rdp);
    m_state.rdp.MarkDirty(RDPDirtyFlag::PrimColor);
}

void RDPProcessor::HandleSetEnvColor(const uint32_t* words) {
    m_decoder.DecodeSetEnvColor(words, m_state.rdp.envColor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::EnvColor);
}

void RDPProcessor::HandleSetPrimDepth(const uint32_t* words) {
    FlushBatch();
    m_decoder.DecodeSetPrimDepth(words, m_state.rdp.primDepth, m_state.rdp.primDeltaZ);
    m_state.rdp.MarkDirty(RDPDirtyFlag::PrimDepth);
}

void RDPProcessor::HandleSetConvert(const uint32_t* words) {
    m_decoder.DecodeSetConvert(words, m_state.rdp.convertK);
}

void RDPProcessor::HandleSetKeyGB(const uint32_t* words) {
    m_decoder.DecodeSetKeyGB(words, m_state.rdp);
}

void RDPProcessor::HandleSetKeyR(const uint32_t* words) {
    m_decoder.DecodeSetKeyR(words, m_state.rdp);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Load Handlers
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::HandleLoadBlock(const uint32_t* words) {
    // Flush pending draws before overwriting TMEM � batched draws need
    // the TMEM content that was present when they were queued.
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile, uls, ult, texels, dxt;
    m_decoder.DecodeLoadBlock(words, tile, uls, ult, texels, dxt);
    // GLideN64: gDPSetTileSize(tile, uls, ult, lrs, dxt) inside gDPLoadBlock
    auto& tileRef = m_state.rdp.tiles[tile];
    tileRef.sl = uls; tileRef.tl = ult; tileRef.sh = texels; tileRef.th = dxt;
    // Track which RDRAM source fed this tile (for correct 2-cycle texture keys)
    m_state.rdp.tileLoadAddress[tile & 7] = m_state.rdp.textureImage.address;
    ExecuteLoadBlock(tile, uls, ult, texels, dxt);
}

void RDPProcessor::HandleLoadTile(const uint32_t* words) {
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile;
    float sl, tl, sh, th;
    m_decoder.DecodeLoadTile(words, tile, sl, tl, sh, th);
    // GLideN64: gDPSetTileSize(tile, uls, ult, lrs, lrt) inside gDPLoadTile
    auto& tileRef = m_state.rdp.tiles[tile];
    tileRef.sl = static_cast<int32_t>(sl * 4.0f);
    tileRef.tl = static_cast<int32_t>(tl * 4.0f);
    tileRef.sh = static_cast<int32_t>(sh * 4.0f);
    tileRef.th = static_cast<int32_t>(th * 4.0f);
    // Track which RDRAM source fed this tile (for correct 2-cycle texture keys)
    m_state.rdp.tileLoadAddress[tile & 7] = m_state.rdp.textureImage.address;
    ExecuteLoadTile(tile, sl, tl, sh, th);
}

void RDPProcessor::HandleLoadTLUT(const uint32_t* words) {
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile, count;
    m_decoder.DecodeLoadTLUT(words, tile, count);
    ExecuteLoadTLUT(tile, count);
}

void RDPProcessor::HandleScrollTex(const uint32_t* words) {
    int tile = (words[1] >> 8) & 0x7;
    int s = static_cast<int16_t>(words[1] & 0xFFFF);
    int t = static_cast<int16_t>(words[0] >> 16);
    
    // Convert 10.2 fixed point to texel units
    m_state.rdp.scrollS[tile] = static_cast<float>(s) / 1024.0f;
    m_state.rdp.scrollT[tile] = static_cast<float>(t) / 1024.0f;
}

// �� GLideN64-matching TMEM copy helpers ����������������������������������

// Byte-swap copy from big-endian N64 RDRAM into little-endian TMEM byte array.
// Mirrors GLideN64's UnswapCopyWrap: reverses bytes within each 32-bit word
// while copying, wrapping the destination at destMask (0xFFF for TMEM).
static void UnswapCopyWrap(const uint8_t* src, uint32_t srcIdx,
                           uint8_t* dest, uint32_t destIdx,
                           uint32_t destMask, uint32_t numBytes)
{
    // Copy leading bytes to reach 4-byte alignment in source
    uint32_t leadingBytes = srcIdx & 3;
    if (leadingBytes != 0) {
        leadingBytes = 4 - leadingBytes;
        if (leadingBytes > numBytes)
            leadingBytes = numBytes;
        numBytes -= leadingBytes;
        srcIdx ^= 3;
        for (uint32_t i = 0; i < leadingBytes; i++) {
            dest[destIdx & destMask] = src[srcIdx];
            ++destIdx;
            --srcIdx;
        }
        srcIdx += 5;
    }
    // Copy aligned 32-bit words (byte-swap within each word)
    uint32_t numDWords = numBytes >> 2;
    while (numDWords--) {
        dest[(destIdx + 3) & destMask] = src[srcIdx++];
        dest[(destIdx + 2) & destMask] = src[srcIdx++];
        dest[(destIdx + 1) & destMask] = src[srcIdx++];
        dest[(destIdx + 0) & destMask] = src[srcIdx++];
        destIdx += 4;
    }
    // Copy trailing bytes
    uint32_t trailingBytes = numBytes & 3;
    if (trailingBytes) {
        srcIdx ^= 3;
        for (uint32_t i = 0; i < trailingBytes; i++) {
            dest[destIdx & destMask] = src[srcIdx];
            ++destIdx;
            --srcIdx;
        }
    }
}

// Swap 32-bit halves of each 64-bit word on odd TMEM rows.
// Mirrors GLideN64's DWordInterleaveWrap: works on uint32_t view of TMEM.
// srcIdx and srcMask are in uint32_t units (not bytes).
static void DWordInterleaveWrap(uint32_t* src, uint32_t srcIdx,
                                uint32_t srcMask, uint32_t numQWords)
{
    while (numQWords--) {
        uint32_t idx0 = srcIdx++ & srcMask;
        uint32_t idx1 = srcIdx++ & srcMask;
        uint32_t p0 = src[idx0];
        src[idx0] = src[idx1];
        src[idx1] = p0;
    }
}

void RDPProcessor::ExecuteLoadBlock(int tile, int uls, int ult,
                                     int texels, int dxt) {
    if (!m_rdram) return;
    FlushBatch();

    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];

    // If there's a GPU framebuffer at the texture source address, sync it.
    // But only if the GPU has actually rendered to it — if not, the RDRAM
    // data (possibly written by the CPU, e.g. logos) is still current.
    // Reject non-exact matches: FindByAddress's second-pass range matching
    // can return a framebuffer whose memory range merely contains this
    // address.  CopyFromGPU would then overwrite the entire FB region in
    // RDRAM — destroying CPU-written data (e.g. JPEG decoded backgrounds
    // in OOT) with stale GPU scene data.
    GPUFramebuffer* srcFB = g_fbManager.FindByAddress(texImg.address);
    if (srcFB && srcFB->desc.address != (texImg.address & 0x7FFFFF))
        srcFB = nullptr;
    if (srcFB && srcFB->gpuModified) {
        g_fbManager.CopyFromGPU(srcFB, m_rdram);
    }

    // Byte count from tile size (matches GLideN64)
    uint32_t width = (texels - uls + 1) & 0x0FFF;
    uint32_t bytes = width << tileDesc.size >> 1;
    if ((bytes & 7) != 0)
        bytes = (bytes & ~7u) + 8;

    uint32_t address = texImg.address + ult * texImg.width * GetBitsPerPixel(texImg.size) / 8
                     + (uls << static_cast<int>(texImg.size) >> 1);
    address &= (m_rdramSize - 1);

    if (bytes == 0 || (address + bytes) > m_rdramSize) return;

    uint32_t tmemWordAddr = tileDesc.tmemAddr; // in 64-bit word units

    // RGBA32: split-bank storage (matching GLideN64's gDPLoadBlock32)
    if (tileDesc.size == static_cast<uint8_t>(ImageSize::Bpp32)) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(m_rdram);
        uint16_t* tmem16 = reinterpret_cast<uint16_t*>(m_state.rdp.tmem.data);
        const uint32_t tb = tileDesc.tmemAddr << 2;  // in u16 units
        const uint32_t line32 = tileDesc.line << 2;  // in u16 units
        uint32_t srcAddr = m_state.rdp.textureImage.address >> 2;
        uint32_t w32 = (static_cast<uint32_t>(texels) - uls + 1) << 2;
        if (w32 == 4) w32 = 1;
        else if (w32 & 7) w32 = (w32 & ~7u) + 8;
        uint32_t maxIdx = m_rdramSize >> 2;

        if (dxt != 0) {
            uint32_t j = 0, t = 0, oldt = 0;
            for (uint32_t i = 0; i < w32; i += 2) {
                oldt = t;
                t = ((j >> 11) & 1) ? 3 : 1;
                if (t != oldt) i += line32;
                if (i >= w32) break;
                if (srcAddr + i + 1 < maxIdx) {
                    uint32_t ptr = ((tb + i) ^ t) & 0x3ff;
                    uint32_t c = src[srcAddr + i];
                    tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                    tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
                    ptr = ((tb + i + 1) ^ t) & 0x3ff;
                    c = src[srcAddr + i + 1];
                    tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                    tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
                }
                j += dxt;
            }
        } else {
            for (uint32_t i = 0; i < w32 && (srcAddr + i) < maxIdx; i++) {
                uint32_t ptr = ((tb + i) ^ 1) & 0x3ff;
                uint32_t c = src[srcAddr + i];
                tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
            }
        }
    } else {
    // Non-RGBA32: UnswapCopy + DXT interleave (matching GLideN64)
    UnswapCopyWrap(m_rdram, address,
                   m_state.rdp.tmem.data, tmemWordAddr << 3,
                   0xFFF, bytes);
    if (dxt != 0) {
        uint32_t dxtCounter = 0;
        uint32_t qwords = (bytes >> 3);
        uint32_t line = 0;
        uint32_t tmAddr = tmemWordAddr;
        while (true) {
            do {
                ++tmAddr; --qwords;
                if (qwords == 0) goto end_dxt;
                dxtCounter += dxt;
            } while ((dxtCounter & 0x800) == 0);
            do {
                ++line; --qwords;
                if (qwords == 0) goto end_dxt;
                dxtCounter += dxt;
            } while ((dxtCounter & 0x800) != 0);
            DWordInterleaveWrap(reinterpret_cast<uint32_t*>(m_state.rdp.tmem.data),
                                tmAddr << 1, 0x3FF, line);
            tmAddr += line;
            line = 0;
        }
        end_dxt:
        DWordInterleaveWrap(reinterpret_cast<uint32_t*>(m_state.rdp.tmem.data),
                            tmAddr << 1, 0x3FF, line);
    }
    } // end non-RGBA32

    // Compute CRC for texture cache
    uint32_t tmemByteAddr = tmemWordAddr << 3;
    int crcLen = std::min(static_cast<int>(bytes), static_cast<int>(TMEM_SIZE - tmemByteAddr));
    if (crcLen > 0) {
        uint32_t crc = CRC32_Fast(m_state.rdp.tmem.data + tmemByteAddr, crcLen);
        m_state.rdp.tmem.RecordLoad(address, tileDesc.tmemAddr,
                                     static_cast<uint16_t>(crcLen), crc);
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

void RDPProcessor::ExecuteLoadTile(int tile, float sl, float tl, float sh, float th) {
    if (!m_rdram) return;
    FlushBatch();

    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];

    // Same check as LoadBlock — only sync GPU→RDRAM if GPU has rendered to this FB.
    // Reject non-exact matches (see ExecuteLoadBlock comment).
    GPUFramebuffer* srcFB = g_fbManager.FindByAddress(texImg.address);
    if (srcFB && srcFB->desc.address != (texImg.address & 0x7FFFFF))
        srcFB = nullptr;
    if (srcFB && srcFB->gpuModified) {
        g_fbManager.CopyFromGPU(srcFB, m_rdram);
    }

    int iuls = static_cast<int>(sl) & 0x3FF;
    int iult = static_cast<int>(tl) & 0x3FF;
    int ilrs = static_cast<int>(sh) & 0x3FF;
    int ilrt = static_cast<int>(th) & 0x3FF;
    int width  = (ilrs - iuls + 1) & 0x3FF;
    int height = (ilrt - iult + 1) & 0x3FF;

    if (width <= 0 || height <= 0) return;

    // GLideN64: return early when tile line=0 (degenerate config)
    if (tileDesc.line == 0) return;

    int srcBpp = GetBitsPerPixel(texImg.size);
    int srcLineBytes = texImg.width * srcBpp / 8;
    int dstLineBytes = tileDesc.line * 8; // TMEM line in bytes

    // Width-mask alignment (matches GLideN64)
    int wmask = 0;
    switch (texImg.size) {
    case ImageSize::Bpp8:  wmask = 7;  break;
    case ImageSize::Bpp16: wmask = 3;  break;
    case ImageSize::Bpp32: wmask = 1;  break;
    default: break;
    }
    int alignedWidth = width;
    if (wmask != 0 && (width & wmask) != 0)
        alignedWidth = (width & ~wmask) + wmask + 1;

    int tileBpp = GetBitsPerPixel(static_cast<ImageSize>(tileDesc.size));
    int rowBytes = alignedWidth * tileBpp / 8;
    if (dstLineBytes == 0) dstLineBytes = rowBytes;

    uint32_t address = texImg.address
        + static_cast<uint32_t>(iult) * srcLineBytes
        + (static_cast<uint32_t>(iuls) << static_cast<int>(texImg.size) >> 1);
    address &= (m_rdramSize - 1);

    uint32_t tmemWordAddr = tileDesc.tmemAddr; // 64-bit word address
    int totalBytes = 0;

    // RGBA32: split-bank storage per pixel (matching GLideN64's gDPLoadTile32b)
    if (tileDesc.size == static_cast<uint8_t>(ImageSize::Bpp32)) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(m_rdram);
        uint16_t* tmem16 = reinterpret_cast<uint16_t*>(m_state.rdp.tmem.data);
        uint32_t line32 = tileDesc.line << 2;  // tile line in u16 units
        uint32_t tbase = tileDesc.tmemAddr << 2;
        uint32_t rdAddr32 = texImg.address >> 2;
        uint32_t maxIdx = m_rdramSize >> 2;

        for (int j = 0; j < height; ++j) {
            uint32_t tline = tbase + line32 * static_cast<uint32_t>(j);
            uint32_t s = (static_cast<uint32_t>(j + iult) * texImg.width) + static_cast<uint32_t>(iuls);
            uint32_t xorval = (j & 1) ? 3u : 1u;
            for (int i = 0; i < width; ++i) {
                uint32_t srcIdx = rdAddr32 + s + static_cast<uint32_t>(i);
                if (srcIdx >= maxIdx) continue;
                uint32_t c = src[srcIdx];
                uint32_t ptr = ((tline + static_cast<uint32_t>(i)) ^ xorval) & 0x3ff;
                tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
            }
        }
        totalBytes = width * height * 4;
    } else {
    // Non-RGBA32: UnswapCopyWrap per row + DWordInterleave on odd rows
    uint32_t qwpr = rowBytes >> 3; // qwords per row
    uint32_t tmAddr = tmemWordAddr; // in 64-bit word units
    uint32_t srcLineBpl = texImg.width * srcBpp / 8; // source bytes per line
    uint32_t curAddr = address;

    for (int y = 0; y < height && curAddr < m_rdramSize; y++) {
        uint32_t copyLen = rowBytes;
        if (curAddr + copyLen > m_rdramSize)
            copyLen = m_rdramSize - curAddr;

        UnswapCopyWrap(m_rdram, curAddr,
                       m_state.rdp.tmem.data, tmAddr << 3,
                       0xFFF, copyLen);
        totalBytes += copyLen;

        if (y & 1) {
            DWordInterleaveWrap(
                reinterpret_cast<uint32_t*>(m_state.rdp.tmem.data),
                tmAddr << 1, 0x3FF, qwpr);
        }

        curAddr += srcLineBpl;
        tmAddr += tileDesc.line; // advance by tile line (64-bit word units)
    }
    } // end non-RGBA32

    // CRC the loaded data
    uint32_t tmemByteAddr = tmemWordAddr << 3;
    if (totalBytes > 0) {
        int crcLen = std::min(totalBytes, static_cast<int>(TMEM_SIZE - tmemByteAddr));
        if (crcLen > 0) {
            uint32_t crc = CRC32_Fast(m_state.rdp.tmem.data + tmemByteAddr, crcLen);
            m_state.rdp.tmem.RecordLoad(address, tileDesc.tmemAddr,
                                         static_cast<uint16_t>(crcLen), crc);
        }
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

void RDPProcessor::ExecuteLoadTLUT(int tile, int count) {
    if (!m_rdram) return;
    FlushBatch();

    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];

    // GLideN64: tmem < 256 is wrong for TLUT (in 64-bit-word units, 256 = byte 2048)
    if (tileDesc.tmemAddr < 256) return;

    uint32_t address = texImg.address
        + (tileDesc.tl >> 2) * (texImg.width * GetBitsPerPixel(texImg.size) / 8)
        + ((tileDesc.sl >> 2) << static_cast<int>(texImg.size) >> 1);

    uint16_t* tmem16 = reinterpret_cast<uint16_t*>(m_state.rdp.tmem.data);
    uint32_t destIdx = tileDesc.tmemAddr << 2; // in u16 units

    // Byte-swap a 16-bit value (matches GLideN64's swapword)
    auto swapword = [](uint16_t v) -> uint16_t {
        return static_cast<uint16_t>((v << 8) | (v >> 8));
    };

    for (int i = 0; i < count; i++) {
        uint32_t rdAddr = address & (m_rdramSize - 1);
        if (rdAddr + 1 < m_rdramSize) {
            uint16_t val = *reinterpret_cast<const uint16_t*>(m_rdram + (rdAddr ^ 2));
            tmem16[(destIdx | 0x0400) & 0x07FF] = swapword(val);
        }
        address += 2;
        destIdx += 4;
    }

    // CRC from RDRAM source
    uint32_t srcAddr = texImg.address & (m_rdramSize - 1);
    uint32_t crcBytes = count * 2;
    if (srcAddr + crcBytes <= m_rdramSize && crcBytes > 0) {
        uint32_t crc = CRC32_Fast(m_rdram + srcAddr, crcBytes);
        m_state.rdp.tmem.RecordLoad(srcAddr, static_cast<uint16_t>(tileDesc.tmemAddr),
                                     static_cast<uint16_t>(crcBytes), crc);
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sync Handlers
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::HandleSyncFull() {
    // Signal DP_FULL interrupt immediately � this is what the N64 CPU waits for.
    // The callback fires before FlushBatch so the CPU can overlap with rendering,
    // matching real hardware where the RDP issues the interrupt at sync time.
    if (m_syncFullCallback) m_syncFullCallback();

    // Count scene draws for readback decision (snapshot is handled by FlushBatch).
    int sceneDraws = 0;
    for (const auto& c : m_batch.drawCalls) {
        if (c.type == DrawCall::Type::Triangle || c.type == DrawCall::Type::TextureRect)
            ++sceneDraws;
    }

    FlushBatch();


    // �� GPU � RDRAM readback ��
    // After the frame is complete, copy rendered pixels back to RDRAM so the
    // CPU sees up-to-date data if it reads the framebuffer memory directly.
    if (sceneDraws >= 1 && m_rdram) {
        // Complete any previous async readback first
        if (g_fbManager.IsReadbackPending())
            g_fbManager.FinishAsyncReadback(m_rdram);

        GPUFramebuffer* colorFB = g_fbManager.GetCurrentColor();
        if (colorFB && colorFB->desc.address != 0)
            g_fbManager.BeginAsyncReadback(colorFB);

        // Depth readback is synchronous and expensive; only do it for
        // games that actually read depth from RDRAM (e.g., Perfect Dark).
        if (g_fbManager.m_fbHacks.pdDepthOncePerFrame) {
            if (m_state.rdp.zbufferImage.address != 0) {
                GPUFramebuffer* depthFB = g_fbManager.FindByAddress(
                    m_state.rdp.zbufferImage.address);
                if (depthFB && depthFB->depthTex)
                    g_fbManager.CopyDepthFromGPU(depthFB, m_rdram);
            }
        }
    }

    m_stats.syncFullCount++;
    m_state.rdp.syncCount++;

    // ��� Clear coverage image for next frame �����������������������������
    // On real N64, a FILL-mode FillRect clears both the color framebuffer
    // AND the per-pixel coverage.  Our FillRect draws via normal GL (not MLE
    // imageStore), so the coverage image retains stale values from the
    // previous frame's geometry.  With b1=1 (B = mem_coverage), stale
    // coverage causes edge pixels to blend with the old frame's color,
    // producing visible wireframe outlines on every triangle.
    // Clear the coverage image at frame boundaries so the next frame starts
    // with cvg=0 everywhere (matching N64's post-fill state).
    {
        GPUFramebuffer* fb = g_fbManager.GetCurrentColor();
        if (fb && fb->coverageTex) {
            const GLuint zero = 0;
            glClearTexImage(fb->coverageTex, 0, GL_RED_INTEGER,
                            GL_UNSIGNED_BYTE, &zero);
        }
    }

    // ��� Defensive depth clear ������������������������������������������
    // Games normally clear depth via FillRect targeting the Z-buffer address,
    // which we intercept in HandleFillRect.  However, if a game skips that
    // clear (or if our detection missed it), stale depth from the previous
    // frame can cause 2D HUD elements to be hidden behind 3D geometry.
    // Clear the GL depth buffer at frame boundaries as a safety net.
    {
        GPUFramebuffer* fb = g_fbManager.GetCurrentColor();
        if (fb && fb->fbo) {
            glClearNamedFramebufferfi(fb->fbo, GL_DEPTH_STENCIL, 0, 1.0f, 0);
        }
    }
}

void RDPProcessor::HandleSyncPipe() {
    // SyncPipe ensures all primitives have been written before state change
    // In HLE, this is handled by our batch system
    m_stats.syncPipeCount++;
}

void RDPProcessor::HandleSyncTile() {
    m_stats.syncTileCount++;
}

void RDPProcessor::HandleSyncLoad() {
    m_stats.syncLoadCount++;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Primitive Handlers
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::HandleTriangle(const uint32_t* words, int wordCount) {
    DecodedTriangle tri;
    m_decoder.DecodeTriangle(words, wordCount, tri);
    DrawTriangle(tri);
}

void RDPProcessor::HandleTextureRect(const uint32_t* words) {
    if (m_hacks.skipTexRect) return;  // WinBack: grey rectangle fix
    DecodedRectangle rect;
    m_decoder.DecodeTextureRect(words, rect);

    // COPY mode: the RDP processes 4 texels per clock cycle.
    // dSdX is in "per cycle" units; divide by 4 for our per-pixel renderer.
    // XL also needs +1 because the hardware uses an exclusive right edge in COPY.
    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 2) {
        rect.dsDx /= 4.0f;
        rect.x1 += 1.0f;
        rect.y1 += 1.0f;  // COPY mode uses exclusive bottom edge
    } else if (rect.y1 - rect.y0 < 1.0f) {
        rect.y1 = std::ceil(rect.y1);  // Sub-pixel height fixup
    }

    // Widescreen: skip letterbox bars and scale non-fullscreen rects inward.
    float wsScale = m_state.rdp.geometry.widescreenScale;
    if (wsScale != 1.0f) {
        float ciWidth = static_cast<float>(m_state.rdp.colorImage.width);
        if (ciWidth < 1.0f) ciWidth = 320.0f;
        float screenH = static_cast<float>(m_state.rdp.scissor.yl >> 2);
        if (screenH < 1.0f) screenH = 240.0f;
        float rectW = rect.x1 - rect.x0;
        float rectH = rect.y1 - rect.y0;
        bool isFullWidth = (rect.x0 <= 1.0f && rect.x1 >= ciWidth - 1.0f);

        // Skip letterbox bars: full-width, thin (<25% height), touching top/bottom edge.
        if (isFullWidth && rectH < screenH * 0.25f &&
            (rect.y0 <= 1.0f || rect.y1 >= screenH - 1.0f)) {
            return;
        }

        // Scale non-fullscreen rects inward to maintain 4:3 proportions.
        // Fullscreen = covers full width AND >75% height (fades, transitions).
        bool isFullscreen = isFullWidth && rectH > screenH * 0.75f;
        if (!isFullscreen) {
            float n64ScreenW = static_cast<float>(m_state.rdp.scissor.xl >> 2);
            if (n64ScreenW < 1.0f) n64ScreenW = 320.0f;
            float invScale = 1.0f / wsScale;
            float halfW = n64ScreenW * 0.5f;
            float offsetX = halfW * (1.0f - invScale);
            rect.x0 = rect.x0 * invScale + offsetX;
            rect.x1 = rect.x1 * invScale + offsetX;
            // Compensate texture coordinates: the rect is narrower but
            // must display the same texture content, so increase dsDx.
            rect.dsDx *= wsScale;
        } else {
            // Fullscreen transition/fade: expand Y to cover the full
            // framebuffer height so it fills the expanded 3D viewport.
            rect.y0 = 0.0f;
            rect.y1 = 240.0f;
        }
    }

    DrawTextureRect(rect);
}

void RDPProcessor::HandleTextureRectFlip(const uint32_t* words) {
    if (m_hacks.skipTexRect) return;  // WinBack: grey rectangle fix
    DecodedRectangle rect;
    m_decoder.DecodeTextureRectFlip(words, rect);

    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 2) {
        // After flip swap: dsDx = orig dtDy, dtDy = orig dsDx.
        // COPY mode divides the original dsDx by 4 � which is now in dtDy.
        rect.dtDy /= 4.0f;
        rect.x1 += 1.0f;
        rect.y1 += 1.0f;  // COPY mode uses exclusive bottom edge
    } else if (rect.y1 - rect.y0 < 1.0f) {
        rect.y1 = std::ceil(rect.y1);  // Sub-pixel height fixup
    }

    // Widescreen: skip letterbox bars and scale non-fullscreen rects inward.
    float wsScaleFlip = m_state.rdp.geometry.widescreenScale;
    if (wsScaleFlip != 1.0f) {
        float ciWidth = static_cast<float>(m_state.rdp.colorImage.width);
        if (ciWidth < 1.0f) ciWidth = 320.0f;
        float screenH = static_cast<float>(m_state.rdp.scissor.yl >> 2);
        if (screenH < 1.0f) screenH = 240.0f;
        float rectW = rect.x1 - rect.x0;
        float rectH = rect.y1 - rect.y0;
        bool isFullWidth = (rect.x0 <= 1.0f && rect.x1 >= ciWidth - 1.0f);

        if (isFullWidth && rectH < screenH * 0.25f &&
            (rect.y0 <= 1.0f || rect.y1 >= screenH - 1.0f)) {
            return;
        }

        bool isFullscreen = isFullWidth && rectH > screenH * 0.75f;
        if (!isFullscreen) {
            float n64ScreenW = static_cast<float>(m_state.rdp.scissor.xl >> 2);
            if (n64ScreenW < 1.0f) n64ScreenW = 320.0f;
            float invScale = 1.0f / wsScaleFlip;
            float halfW = n64ScreenW * 0.5f;
            float offsetX = halfW * (1.0f - invScale);
            rect.x0 = rect.x0 * invScale + offsetX;
            rect.x1 = rect.x1 * invScale + offsetX;
            // Compensate texture coordinates for narrower rect.
            rect.dsDx *= wsScaleFlip;
        } else {
            rect.y0 = 0.0f;
            rect.y1 = 240.0f;
        }
    }

    DrawTextureRect(rect);
}

void RDPProcessor::HandleFillRect(const uint32_t* words) {
    float x0, y0, x1, y1;
    m_decoder.DecodeFillRect(words, x0, y0, x1, y1);

    // �� Detect depth buffer clear ��
    // The N64 clears the depth buffer by setting the color image to the depth
    // buffer address, then issuing FillRect with the depth clear fill color.
    // Detect this pattern and perform a GL depth clear instead of drawing.
    if (m_state.rdp.colorImage.address == m_state.rdp.zbufferImage.address) {
        // The standard N64 depth clear color is 0xFFFCFFFC (Z=max, dZ=0).
        // Accept any fill to the depth address as a depth clear.
        FlushBatch();
        if (g_fbManager.GetCurrentColor()) {
            GLuint fbo = g_fbManager.GetCurrentColor()->fbo;
            glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, 1.0f, 0);
        }
        // Also clear the N64 Z-image so imageLoad-based depth compare
        // sees max depth (1.0) for newly cleared pixels.
        auto* depthBuf = g_depthManager.GetCurrent();
        if (depthBuf) {
            g_depthManager.ClearDepthImageTextures(*depthBuf);
        }
        return;
    }

    // Re-decode fill color at draw time using the CURRENT color image size
    // (in case it changed since SetFillColor was called).
    DecodeFillColor(m_state.rdp.fillColorRaw, m_state.rdp.colorImage.size,
                    m_state.rdp.fillColor.r, m_state.rdp.fillColor.g,
                    m_state.rdp.fillColor.b, m_state.rdp.fillColor.a);

    // In FILL mode (cycleType=3), the N64 RDP uses inclusive right/bottom edges.
    // OpenGL rasterization uses exclusive right/bottom (top-left fill rule).
    // Add 1 pixel to compensate so the last column/row is filled.
    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 3) {
        x1 += 1.0f;
        y1 += 1.0f;
    } else if (y1 == y0) {
        y1 += 1.0f;
    }

    // Widescreen: skip game-drawn letterbox bars.
    // Detect by position: full-width FillRects touching the top or bottom edge
    // that are thin (< 25% of screen height) are cinematic bars.
    float wsScaleFill = m_state.rdp.geometry.widescreenScale;
    if (wsScaleFill != 1.0f) {
        float ciWidth = static_cast<float>(m_state.rdp.colorImage.width);
        if (ciWidth < 1.0f) ciWidth = 320.0f;
        float screenH = static_cast<float>(m_state.rdp.scissor.yl >> 2);
        if (screenH < 1.0f) screenH = 240.0f;
        bool isFullWidth = (x0 <= 1.0f && x1 >= ciWidth - 1.0f);
        float rectH = y1 - y0;
        bool isThin = (rectH < screenH * 0.25f);
        bool touchesTop = (y0 <= 1.0f);
        bool touchesBottom = (y1 >= screenH - 1.0f);

        if (isFullWidth && isThin && (touchesTop || touchesBottom)) {
            return;  // Skip letterbox bar
        }

        // Expand fullscreen FillRects to cover the full Y range
        // so transitions/color fills cover the expanded 3D viewport.
        if (isFullWidth && rectH > screenH * 0.75f) {
            y0 = 0.0f;
            y1 = 240.0f;
        }
    }

    DrawFillRect(x0, y0, x1, y1);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Drawing
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::DrawTriangle(const DecodedTriangle& tri) {
    if (m_batch.IsFull()) FlushBatch();
    
    // Create draw call
    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = tri.tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = m_state.rdp.geometry.geometryMode;
    PopulateTextureInfo(call, tri.tileIndex);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;
    
    // Reconstruct 3 triangle vertices from RDP edge coefficients.
    // The RDP defines 3 edges:
    //   H edge: from yh to yl (major edge spanning full height)  start X = xh, slope = dxhDy
    //   M edge: from yh to ym (upper minor edge)                 start X = xm, slope = dxmDy
    //   L edge: from ym to yl (lower minor edge)                 start X = xl, slope = dxlDy
    // The 3 triangle corners are:
    //   Top:    (xh, yh) � where H and M edges both start
    //   Mid:    (xl, ym) � where M edge ends / L edge starts (the "knee")
    //   Bottom: (xh + dxhDy*(yl-yh), yl) � where H and L edges meet
    float yTop = tri.edge.yh;
    float yMid = tri.edge.ym;
    float yBot = tri.edge.yl;
    
    RenderBatch::Vertex v0, v1, v2;
    
    // Top vertex: start of major (H) edge
    v0.x = tri.edge.xh;
    v0.y = yTop;
    
    // Mid vertex: start of L edge = end of M edge (the "knee" on the non-major side)
    v1.x = tri.edge.xl;
    v1.y = yMid;
    
    // Bottom vertex: end of major (H) edge
    v2.x = tri.edge.xh + tri.edge.dxhDy * (yBot - yTop);
    v2.y = yBot;
    
    // Attribute interpolation using edge (dDe) and X (dDx) derivatives.
    // At (x, y), attr = attr_start + dDe * (y - yh) + dDx * (x - xH_at_y)
    // where xH_at_y = xh + dxhDy * (y - yh) is the major edge X at scanline y.
    auto interpAttr = [&](float base, float dDe, float dDx, float x, float y) -> float {
        float dy = y - yTop;
        float xMajor = tri.edge.xh + tri.edge.dxhDy * dy; // major edge X at this Y
        return base + dDe * dy + dDx * (x - xMajor);
    };
    
    // Depth
    if (tri.hasDepth) {
        v0.z = tri.depth.z;
        v1.z = interpAttr(tri.depth.z, tri.depth.dzDe, tri.depth.dzDx, v1.x, v1.y);
        v2.z = tri.depth.z + tri.depth.dzDe * (yBot - yTop); // bot is on major edge, dDx term = 0
        // LLE depth coefficients are S15.16 normalised to [0,1]; clamp for safety.
        v0.z = std::max(0.0f, std::min(1.0f, v0.z));
        v1.z = std::max(0.0f, std::min(1.0f, v1.z));
        v2.z = std::max(0.0f, std::min(1.0f, v2.z));
    } else {
        v0.z = v1.z = v2.z = 0.0f;
    }
    
    // Perspective W from RDP texture coefficients.
    // tri.texture.w = 1/clip_W (s15.16 format, same scale as s,t coefficients).
    // Store as perspW so the fragment shader can recover perspective-correct texcoords:
    //   tc = vTexCoord / vClipW  (= (s/W * scale) / (1/W * scale) = s).
    // Position W must remain 1.0 � the ortho matrix assumes pre-divided screen-space inputs.
    if (tri.hasTexture && tri.texture.w != 0.0f) {
        v0.perspW = tri.texture.w;
        float w1 = interpAttr(tri.texture.w, tri.texture.dwDe, tri.texture.dwDx, v1.x, v1.y);
        float w2 = tri.texture.w + tri.texture.dwDe * (yBot - yTop);
        v1.perspW = w1;
        v2.perspW = w2;
    } else {
        v0.perspW = v1.perspW = v2.perspW = 1.0f;
    }
    v0.w = v1.w = v2.w = 1.0f;  // Position W always 1.0 for ortho projection
    
    // Shade (color)
    if (tri.hasShade) {
        v0.r = tri.shade.r / 255.0f; v0.g = tri.shade.g / 255.0f;
        v0.b = tri.shade.b / 255.0f; v0.a = tri.shade.a / 255.0f;
        
        v1.r = interpAttr(tri.shade.r, tri.shade.drDe, tri.shade.drDx, v1.x, v1.y) / 255.0f;
        v1.g = interpAttr(tri.shade.g, tri.shade.dgDe, tri.shade.dgDx, v1.x, v1.y) / 255.0f;
        v1.b = interpAttr(tri.shade.b, tri.shade.dbDe, tri.shade.dbDx, v1.x, v1.y) / 255.0f;
        v1.a = interpAttr(tri.shade.a, tri.shade.daDe, tri.shade.daDx, v1.x, v1.y) / 255.0f;
        
        float dy2 = yBot - yTop;
        v2.r = (tri.shade.r + tri.shade.drDe * dy2) / 255.0f;
        v2.g = (tri.shade.g + tri.shade.dgDe * dy2) / 255.0f;
        v2.b = (tri.shade.b + tri.shade.dbDe * dy2) / 255.0f;
        v2.a = (tri.shade.a + tri.shade.daDe * dy2) / 255.0f;
    } else {
        v0.r = v0.g = v0.b = v0.a = 1.0f;
        v1.r = v1.g = v1.b = v1.a = 1.0f;
        v2.r = v2.g = v2.b = v2.a = 1.0f;
    }
    
    // Texture coordinates (in TMEM texel space)
    if (tri.hasTexture) {
        v0.s = tri.texture.s; v0.t = tri.texture.t;
        v1.s = interpAttr(tri.texture.s, tri.texture.dsDe, tri.texture.dsDx, v1.x, v1.y);
        v1.t = interpAttr(tri.texture.t, tri.texture.dtDe, tri.texture.dtDx, v1.x, v1.y);
        float dy2 = yBot - yTop;
        v2.s = tri.texture.s + tri.texture.dsDe * dy2;
        v2.t = tri.texture.t + tri.texture.dtDe * dy2;
    } else {
        v0.s = v0.t = 0; v1.s = v1.t = 0; v2.s = v2.t = 0;
    }
    
    v0.s1 = v0.t1 = 0; v1.s1 = v1.t1 = 0; v2.s1 = v2.t1 = 0;
    // LLE triangles: the RSP bakes fog factor into shade alpha when G_FOG is set.
    // Copy shade alpha to the fog attribute so the shader's fog path uses the
    // correct per-vertex fog instead of the depth-based fallback.
    if (tri.hasShade) {
        v0.fogFactor = v0.a;
        v1.fogFactor = v1.a;
        v2.fogFactor = v2.a;
    } else {
        v0.fogFactor = v1.fogFactor = v2.fogFactor = 0.0f;
    }
    
    // LLE triangles are in screen space � no clip flags needed
    v0.clipFlags = v1.clipFlags = v2.clipFlags = 0;
    
    m_batch.vertices.push_back(v0);
    m_batch.vertices.push_back(v1);
    m_batch.vertices.push_back(v2);
    call.vertexCount = 3;
    
    m_batch.AppendDrawCall(call);
    
    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTriangle(int v0, int v1, int v2, const GeometryState& geo) {
    if (m_batch.IsFull()) FlushBatch();

    auto makeVert = [&](int idx) -> RenderBatch::Vertex {
        const auto& tv = geo.vertexBuffer[idx];
        RenderBatch::Vertex v{};
        // Perspective divide: NDC first, then apply N64 viewport scale/translate
        float invW = (tv.position[3] != 0.0f) ? 1.0f / tv.position[3] : 1.0f;
        if (tv.modifiedXY) {
            // gSPModifyVertex XYSCREEN: screenX/Y are already in pixel coords
            v.x = tv.screenX;
            v.y = tv.screenY;
        } else {
            v.x = tv.position[0] * invW * geo.viewport.scaleX + geo.viewport.transX;
            v.y = tv.position[1] * invW * geo.viewport.scaleY + geo.viewport.transY;
            // Sub-pixel snap to 1/4 pixel (N64 RSP uses 10.2 fixed-point screen coords)
            v.x = std::floor(v.x * 4.0f) * 0.25f;
            v.y = std::floor(v.y * 4.0f) * 0.25f;
        }
        // Normalize screen-space z to [0,1]. The ortho maps this to clip_z = 2*z_norm - 1 ? [-1,1]
        // (full NDC range). Near z_norm=0 � window_depth=0, Far z_norm=1 � window_depth=1.0 (clear).
        // Without normalization, z_screen ? [0,1024] � clip_z ? [-1,3] � far geometry clipped.
        if (tv.modifiedZ) {
            // gSPModifyVertex ZSCREEN: screenZ is already the final normalized depth
            v.z = tv.screenZ;
        } else {
            float z_screen = tv.position[2] * invW * geo.viewport.scaleZ + geo.viewport.transZ;
            float absScaleZ = std::abs(geo.viewport.scaleZ);
            v.z = (absScaleZ > 0.0001f)
                ? (z_screen - geo.viewport.transZ + absScaleZ) / (2.0f * absScaleZ)
                : 0.0f;
        }
        v.z = std::max(0.0f, std::min(1.0f, v.z));  // clamp: extrapolated/clipped verts may stray outside
        // Bias away from exactly 1.0 so far-plane geometry passes GL_LESS.
        // Without this, vertices at the maximum viewport Z normalize to
        // exactly 1.0, which equals the depth-clear value and fails the
        // strict less-than test, making far-plane surfaces invisible.
        if (v.z >= 1.0f) v.z = 0.99999f;
        v.w = 1.0f;  // ortho maps screen coords -> NDC; clip W already divided out
        v.r = tv.color[0]; v.g = tv.color[1];
        v.b = tv.color[2]; v.a = tv.color[3];
        // Apply gSPTexture scale at draw time so the correct scale is used
        // even if gSPTexture is issued after gSPVertex.
        float scaleS = geo.texture.enabled ? geo.texture.GetScaleSFloat() : 1.0f;
        float scaleT = geo.texture.enabled ? geo.texture.GetScaleTFloat() : 1.0f;
        // When texturePersp is ON (the common case), store texcoords
        // pre-multiplied by 1/W so the fragment shader can recover
        // perspective-correct texels via (vTexCoord / vClipW).
        // When texturePersp is OFF, the N64 does affine (linear) texturing,
        // so store raw texcoords and set perspW=1 � the shader division
        // by vClipW is then an identity op.
        // N64 RSP outputs ST at 2x scale in non-perspective mode; compensate
        // with *0.5 (matches GLideN64 vertex shader: texCoord *= 0.5).
        bool textPersp = (m_state.rdp.otherModeH & OTHERMODE_TEXTPERSP) != 0;
        float tcW = textPersp ? invW : 1.0f;
        float affineHalf = textPersp ? 1.0f : 0.5f;
        v.s  = tv.texCoord[0]  * scaleS * tcW * affineHalf; v.t  = tv.texCoord[1]  * scaleT * tcW * affineHalf;
        v.s1 = tv.texCoord1[0] * scaleS * tcW * affineHalf; v.t1 = tv.texCoord1[1] * scaleT * tcW * affineHalf;
        v.perspW = tcW;
        v.fogFactor = tv.fogFactor;
        v.clipFlags = tv.clipFlags;  // Copy clip flags for frustum culling
        return v;
    };

    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = geo.texture.tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = geo.geometryMode;
    PopulateTextureInfo(call, geo.texture.tileIndex);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;

    m_batch.vertices.push_back(makeVert(v0));
    m_batch.vertices.push_back(makeVert(v1));
    m_batch.vertices.push_back(makeVert(v2));

    // Prim shading: when G_LIGHTING is off AND G_SHADE is off, the shade
    // input should be primColor (not vertex color). GLideN64 ref: GraphicsDrawer.cpp.
    if (!(geo.geometryMode & G_LIGHTING) && !(geo.geometryMode & G_SHADE)) {
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        float pr = m_state.rdp.primColor.r / 255.0f;
        float pg = m_state.rdp.primColor.g / 255.0f;
        float pb = m_state.rdp.primColor.b / 255.0f;
        float pa = m_state.rdp.primColor.a / 255.0f;
        for (int i = 0; i < 3; i++) {
            verts[base + i].r = pr; verts[base + i].g = pg;
            verts[base + i].b = pb; verts[base + i].a = pa;
        }
    }
    // Flat shading: when G_SHADING_SMOOTH is not set, use provoking vertex
    // (v0) color for all three vertices.
    else if (!(geo.geometryMode & 0x00200000)) { // G_SHADING_SMOOTH
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        verts[base + 1].r = verts[base].r; verts[base + 1].g = verts[base].g;
        verts[base + 1].b = verts[base].b; verts[base + 1].a = verts[base].a;
        verts[base + 2].r = verts[base].r; verts[base + 2].g = verts[base].g;
        verts[base + 2].b = verts[base].b; verts[base + 2].a = verts[base].a;
    }

    call.vertexCount = 3;
    m_batch.AppendDrawCall(call);

    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTriangleRaw(const RenderBatch::Vertex& rv0, const RenderBatch::Vertex& rv1,
                                   const RenderBatch::Vertex& rv2, uint32_t geometryMode,
                                   uint8_t tileIndex) {
    if (m_batch.IsFull()) FlushBatch();

    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = geometryMode;
    PopulateTextureInfo(call, tileIndex);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;

    m_batch.vertices.push_back(rv0);
    m_batch.vertices.push_back(rv1);
    m_batch.vertices.push_back(rv2);

    // Prim shading for clipped triangles
    if (!(geometryMode & G_LIGHTING) && !(geometryMode & G_SHADE)) {
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        float pr = m_state.rdp.primColor.r / 255.0f;
        float pg = m_state.rdp.primColor.g / 255.0f;
        float pb = m_state.rdp.primColor.b / 255.0f;
        float pa = m_state.rdp.primColor.a / 255.0f;
        for (int i = 0; i < 3; i++) {
            verts[base + i].r = pr; verts[base + i].g = pg;
            verts[base + i].b = pb; verts[base + i].a = pa;
        }
    }
    // Flat shading for clipped triangles: provoking vertex color is rv0
    else if (!(geometryMode & 0x00200000)) { // G_SHADING_SMOOTH
        auto& verts = m_batch.vertices;
        size_t base = verts.size() - 3;
        verts[base + 1].r = verts[base].r; verts[base + 1].g = verts[base].g;
        verts[base + 1].b = verts[base].b; verts[base + 1].a = verts[base].a;
        verts[base + 2].r = verts[base].r; verts[base + 2].g = verts[base].g;
        verts[base + 2].b = verts[base].b; verts[base + 2].a = verts[base].a;
    }

    call.vertexCount = 3;

    m_batch.AppendDrawCall(call);
    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTextureRect(const DecodedRectangle& rect) {
    if (m_batch.IsFull()) FlushBatch();

    // �� Special depth-buffer texture rect detection ����������������������
    // Some N64 games (Zelda OoT, Perfect Dark, �) read the depth buffer via
    // textured rectangles for shadows (texturedRectShadowMap) or depth
    // readback (texturedRectDepthBufferCopy).
    bool shadowMapRect = false;
    {
        uint32_t texAddr  = m_state.rdp.textureImage.address & 0x7FFFFF;
        uint32_t zbAddr   = m_state.rdp.zbufferImage.address & 0x7FFFFF;
        uint8_t  texSize  = static_cast<uint8_t>(m_state.rdp.textureImage.size);
        uint32_t fbWidth  = m_state.rdp.colorImage.width;

        // Depth buffer range: conservative upper bound (width � width � 1.5).
        uint32_t zbRange  = fbWidth * fbWidth * 3 / 2;

        if (texSize == 2 && zbAddr != 0 &&
            texAddr >= zbAddr && texAddr < zbAddr + zbRange) {

            // texturedRectDepthBufferCopy: 1-pixel-high rect that copies a
            // scanline of depth data from TMEM back to RDRAM.  The GPU has
            // nothing to render � skip the draw entirely.
            if (rect.y0 == 0.0f && rect.y1 <= 1.0f) {
                return;
            }

            // texturedRectShadowMap: depth buffer read for shadow overlay.
            // Mark as shadow map so FlushBatch uses the depth-fog shader.
            shadowMapRect = true;
        }
    }

    DrawCall call;
    call.type = DrawCall::Type::TextureRect;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = rect.tileIndex;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = 0;
    PopulateTextureInfo(call, rect.tileIndex);

    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;
    call.isShadowMap = shadowMapRect;

    // Snapshot TLUT from TMEM palette region for shadow map draws.
    // TMEM layout: palette at byte offset 0x800 (2048).  Each of the 256
    // entries occupies an 8-byte TMEM word; the first two bytes are the
    // 16-bit palette value (big-endian IA16).  Upper byte = intensity/alpha.
    if (shadowMapRect) {
        const uint8_t* tmem = m_state.rdp.tmem.data;
        for (int i = 0; i < 256; i++) {
            call.shadowTLUT[i] = tmem[0x800 + i * 8];
        }
    }
    
    // Generate 4 vertices (two triangles) for the rectangle
    float width = rect.x1 - rect.x0;
    float height = rect.y1 - rect.y0;
    
    float s0 = rect.s;
    float t0 = rect.t;

    // HACK: Some games set s=512.0 (raw 0x4000) for narrow textures.
    // When the tile ULS + colorImage width < 512, the offset wraps
    // incorrectly. Force s to 0. (GLideN64 ref: gDP.cpp gDPTextureRectangle)
    if (s0 == 512.0f) {
        auto& tile = m_state.rdp.tiles[rect.tileIndex];
        if (m_state.rdp.colorImage.width + (tile.sl >> 2) < 512)
            s0 = 0.0f;
    }
    float s1, t1;
    
    if (rect.flipped) {
        s1 = s0 + rect.dtDy * height;  // Flipped: S changes with Y
        t1 = t0 + rect.dsDx * width;   // T changes with X
    } else {
        s1 = s0 + rect.dsDx * width;
        t1 = t0 + rect.dtDy * height;
    }
    
    // Normalize to screen space [-1, 1]
    // The renderer will handle the actual viewport transformation
    RenderBatch::Vertex v[4];
    
    // Top-left (shade color = 0 for TextureRect, matching GLideN64 uRectColor)
    v[0] = { rect.x0, rect.y0, 0, 1, s0, t0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // Top-right
    v[1] = { rect.x1, rect.y0, 0, 1, s1, t0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // Bottom-left
    v[2] = { rect.x0, rect.y1, 0, 1, s0, t1, 0, 0, 0, 0, 0, 0, 0, 0 };
    // Bottom-right
    v[3] = { rect.x1, rect.y1, 0, 1, s1, t1, 0, 0, 0, 0, 0, 0, 0, 0 };
    
    if (rect.flipped) {
        v[1].s = s0;         v[1].t = t1;
        v[2].s = s1;         v[2].t = t0;
    }

    // N64 TextureRect: both tiles use the same S/T coordinates.
    // Copy tile 0 UVs into tile 1 slots for 2-cycle mode multi-tile rendering.
    for (int i = 0; i < 4; i++) {
        v[i].s1 = v[i].s;
        v[i].t1 = v[i].t;
    }

    // Compute texture coordinate bounds to prevent bilinear bleed.
    // Use pixel-center snapping: RDP samples at integer pixel centers,
    // so the first sampled pixel is at ceil(x0), last at ceil(x1)-1.
    {
        float uls, lrs, ult, lrt;
        if (rect.flipped) {
            // Flipped: after decoder swap, dtDy = original dSdX, dsDx = original dTdY
            // S varies with Y (rate = dtDy), T varies with X (rate = dsDx)
            uls = s0 + (std::ceil(rect.y0) - rect.y0) * rect.dtDy;
            lrs = s0 + (std::ceil(rect.y1) - rect.y0 - 1.0f) * rect.dtDy;
            ult = t0 + (std::ceil(rect.x0) - rect.x0) * rect.dsDx;
            lrt = t0 + (std::ceil(rect.x1) - rect.x0 - 1.0f) * rect.dsDx;
        } else {
            // Normal: S varies with X (rate = dsDx), T varies with Y (rate = dtDy)
            uls = s0 + (std::ceil(rect.x0) - rect.x0) * rect.dsDx;
            lrs = s0 + (std::ceil(rect.x1) - rect.x0 - 1.0f) * rect.dsDx;
            ult = t0 + (std::ceil(rect.y0) - rect.y0) * rect.dtDy;
            lrt = t0 + (std::ceil(rect.y1) - rect.y0 - 1.0f) * rect.dtDy;
        }
        float minS = std::min(uls, lrs);
        float minT = std::min(ult, lrt);
        float maxS = std::max(uls, lrs);
        float maxT = std::max(ult, lrt);
        call.texCoordBounds0[0] = minS;
        call.texCoordBounds0[1] = minT;
        call.texCoordBounds0[2] = maxS;
        call.texCoordBounds0[3] = maxT;
        call.texCoordBounds1[0] = minS;
        call.texCoordBounds1[1] = minT;
        call.texCoordBounds1[2] = maxS;
        call.texCoordBounds1[3] = maxT;
        call.useTexCoordBounds = true;
    }
    
    // Compute clip flags (texture rects in screen space: w=1, so no clipping expected)
    for (int i = 0; i < 4; i++) {
        v[i].clipFlags = 0;  // Screen-space rects: always inside frustum
    }
    
    // Two triangles: (0, 1, 2), (1, 3, 2)
    m_batch.vertices.push_back(v[0]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[2]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[3]);
    m_batch.vertices.push_back(v[2]);
    call.vertexCount = 6;
    
    m_batch.AppendDrawCall(call);
    
    m_stats.rectsDrawn++;
    m_stats.frameRects++;
    m_state.rdp.rectCount++;
}

void RDPProcessor::DrawFillRect(float x0, float y0, float x1, float y1) {
    if (m_batch.IsFull()) FlushBatch();

    uint32_t rdpOtherModeH = m_state.rdp.otherModeH;
    uint32_t rdpOtherModeL = m_state.rdp.otherModeL;
    uint32_t rdpCycleType = (rdpOtherModeH >> 20) & 0x03;

    DrawCall call;
    call.type = DrawCall::Type::FillRect;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = 0;
    call.otherModeH = rdpOtherModeH;
    call.otherModeL = rdpOtherModeL;
    call.geometryMode = 0;
    call.textureHash0 = 0;   // Fill rects never use textures
    call.textureHash1 = 0;
    call.textureImageAddr = 0;
    call.textureImageAddr1 = 0;
    std::memset(&call.tileDesc0, 0, sizeof(call.tileDesc0));
    std::memset(&call.tileDesc1, 0, sizeof(call.tileDesc1));
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;
    call.primLODMin  = m_state.rdp.primLODMin;
    call.primDepth   = m_state.rdp.primDepth;
    call.primDeltaZ  = m_state.rdp.primDeltaZ;
    call.fogUsage    = m_state.rdp.geometry.fogUsage;

    float fr, fg, fb, fa;
    if (rdpCycleType == 3) {
        // FILL mode: raw fill color written directly to framebuffer.
        // Alpha = 0: on real N64 FILL mode does not set sub-pixel coverage,
        // so mem_cvg for cleared areas is 0.  Writing alpha=0 emulates that,
        // preventing the MLE blender from incorrectly averaging the first
        // 3-D draw with the background color.
        call.combineMode = 0;
        fr = static_cast<float>(m_state.rdp.fillColor.r) / 255.0f;
        fg = static_cast<float>(m_state.rdp.fillColor.g) / 255.0f;
        fb = static_cast<float>(m_state.rdp.fillColor.b) / 255.0f;
        fa = 0.0f;
    } else {
        // 1-cycle / 2-cycle mode: route through the actual combiner.
        // Shade color input = (0,0,0,0) since FillRect has no vertex shading.
        call.combineMode = m_state.rdp.combineModeRaw;
        fr = 0.0f;
        fg = 0.0f;
        fb = 0.0f;
        fa = 0.0f;
    }
    
    // Two triangles for the fill rectangle
    RenderBatch::Vertex v[4] = {
        { x0, y0, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
        { x1, y0, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
        { x0, y1, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
        { x1, y1, 0, 1, 0, 0, fr, fg, fb, fa, 0, 0, 0, 0 },
    };
    
    // Compute clip flags (fill rects in screen space: w=1, so no clipping expected)
    for (int i = 0; i < 4; i++) {
        v[i].clipFlags = 0;  // Screen-space rects: always inside frustum
    }
    
    m_batch.vertices.push_back(v[0]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[2]);
    m_batch.vertices.push_back(v[1]);
    m_batch.vertices.push_back(v[3]);
    m_batch.vertices.push_back(v[2]);
    call.vertexCount = 6;
    
    m_batch.AppendDrawCall(call);
    
    m_stats.fillRectsDrawn++;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Helpers
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::PopulateTextureInfo(DrawCall& call, int tileIndex) {
    // Use per-tile RDRAM source addresses recorded at LoadBlock/LoadTile time.
    // In 2-cycle mode tile0 and tile1 may come from different SetTextureImage calls.
    uint32_t addr0 = m_state.rdp.tileLoadAddress[tileIndex & 7];
    uint32_t addr1 = m_state.rdp.tileLoadAddress[(tileIndex + 1) & 7];
    // Fallback to the global textureImage address if no load was recorded yet
    if (addr0 == 0) addr0 = m_state.rdp.textureImage.address;
    if (addr1 == 0) addr1 = m_state.rdp.textureImage.address;
    call.textureImageAddr  = addr0;
    call.textureImageAddr1 = addr1;

    // Snapshot tile descriptors for the two possible texture tiles
    const auto& tile0 = m_state.rdp.tiles[tileIndex & 7];
    const auto& tile1 = m_state.rdp.tiles[(tileIndex + 1) & 7];
    call.tileDesc0 = tile0;
    call.tileDesc1 = tile1;

    // RSP tile-clone for HLE triangles:
    // When texture LOD is enabled in the RDP mode word AND the RSP has
    // texture.level == 0 (base level only), the microcode clones
    // tile[tileIndex] into tile[tileIndex+1] so that texel1 reads valid
    // TMEM data.  Without this clone, tile1 may point to stale TMEM
    // (e.g. the palette area), causing noise artifacts.
    //
    // When LOD is NOT enabled, tile0 and tile1 are independent textures
    // used by the 2-cycle combiner (e.g. the NINTENDO64 metallic text
    // effect which blends two tiles via PRIM_LOD_FRAC).  In that case
    // we must NOT clone, otherwise the second texture is lost.
    int level = static_cast<int>(m_state.rdp.geometry.texture.level);
    bool textureLOD = ((call.otherModeH >> 16) & 1) != 0;
    if (level == 0 && call.type == DrawCall::Type::Triangle && textureLOD) {
        call.tileDesc1 = call.tileDesc0;
    }

    // Snapshot mipmap tile chain when LOD mipmapping is active.
    // The RSP sets texture.level to the number of mip levels beyond the base.
    // Tiles are laid out as: base=tiles[tileIndex], mip1=tiles[tileIndex+1], etc.
    call.mipLevelCount = 0;
    if (level > 0 && m_state.rdp.geometry.texture.enabled) {
        int count = std::min(level, 7);
        for (int i = 0; i < count; i++) {
            call.mipTiles[i] = m_state.rdp.tiles[(tileIndex + i + 1) & 7];
        }
        call.mipLevelCount = count;
    }

    // Compute a quick hash to determine whether a texture is active.
    // A tile is "active" if it has non-zero dimensions and a valid TMEM address
    // or the texture image has been loaded.
    auto computeHash = [&](const TileDescriptor& td, uint32_t rdramAddr) -> uint64_t {
        int w = td.GetWidth();
        int h = td.GetHeight();
        // N64 games can set inverted tile rects (sh < sl).  Use absolute
        // dimensions for the hash so the texture is still treated as active.
        w = std::abs(w);
        h = std::abs(h);
        if (w == 0 || h == 0) return 0;

        // Hash from tile fields + textureImage address
        uint64_t hash = 0;
        hash ^= static_cast<uint64_t>(rdramAddr) * 0x9e3779b97f4a7c15ULL;
        hash ^= static_cast<uint64_t>(td.tmemAddr) << 16;
        hash ^= static_cast<uint64_t>(td.format) << 48;
        hash ^= static_cast<uint64_t>(td.size) << 52;
        hash ^= static_cast<uint64_t>(w) << 8;
        hash ^= static_cast<uint64_t>(h) << 24;
        hash ^= static_cast<uint64_t>(td.palette) << 40;
        hash ^= static_cast<uint64_t>(td.line);
        if (hash == 0) hash = 1; // Ensure non-zero for active textures
        return hash;
    };

    // Check cycle type — in 2-cycle mode both tiles can be active
    uint32_t cycleType = (call.otherModeH >> 20) & 0x03;
    call.textureHash0 = computeHash(call.tileDesc0, call.textureImageAddr);
    // When tile1 was cloned from tile0 (LOD active, level==0, triangle),
    // there is no independent second texture.  Clear textureHash1 so
    // SF_TEXTURE_1 is not set; the shader defaults texel1 = texel0.
    if (level == 0 && call.type == DrawCall::Type::Triangle && textureLOD)
        call.textureHash1 = 0;
    else
        call.textureHash1 = (cycleType == 1) ? computeHash(call.tileDesc1, call.textureImageAddr1) : 0;

    // Copy scrolling � when tile1 was cloned from tile0, use tile0's scroll values
    call.scrollS0 = m_state.rdp.scrollS[tileIndex & 7];
    call.scrollT0 = m_state.rdp.scrollT[tileIndex & 7];
    if (level == 0 && call.type == DrawCall::Type::Triangle && textureLOD) {
        call.scrollS1 = call.scrollS0;
        call.scrollT1 = call.scrollT0;
    } else {
        call.scrollS1 = m_state.rdp.scrollS[(tileIndex + 1) & 7];
        call.scrollT1 = m_state.rdp.scrollT[(tileIndex + 1) & 7];
    }
}

// Compute the effective texture dimensions for a tile, accounting for
// mask/clamp settings — equivalent to GLideN64's _calcTileSizes for
// the LOADTYPE_BLOCK case (most common for OoT character textures etc.)
static void CalcTileSize(const TileDescriptor& tile, uint8_t tlutType, int& outW, int& outH) {
    int tileW = std::max(1, tile.GetWidth());
    int tileH = std::max(1, tile.GetHeight());

    // maxTexels = TMEM capacity in texels at this BPP
    // With TLUT, upper 2KB of TMEM is palette → halved for 4/8-bit
    bool hasTLUT = (tlutType != 0);
    static const uint32_t noTLUT[]   = { 8192, 4096, 2048, 1024 };
    static const uint32_t withTLUT[] = { 4096, 2048, 2048, 1024 };
    uint32_t maxTexels = hasTLUT ? withTLUT[tile.size & 3] : noTLUT[tile.size & 3];

    int maskW = (tile.maskS == 0) ? tileW : (1 << tile.maskS);
    int maskH = (tile.maskT == 0) ? tileH : (1 << tile.maskT);

    if (tile.IsClamped_S())
        outW = std::min(maskW, tileW);
    else if (static_cast<uint32_t>(maskW * maskH) <= maxTexels)
        outW = maskW;
    else
        outW = tileW;

    if (tile.IsClamped_T())
        outH = std::min(maskH, tileH);
    else if (static_cast<uint32_t>(maskW * maskH) <= maxTexels)
        outH = maskH;
    else
        outH = tileH;
}

Tex::TextureKey RDPProcessor::BuildTextureKey(const TileDescriptor& tile,
                                               uint32_t textureImageAddr,
                                               uint8_t tlutType,
                                               const TileDescriptor* mipTiles,
                                               int mipCount) const {
    Tex::TextureKey key{};
    key.address = textureImageAddr;
    key.palette = tile.palette;

    // Use mask/clamp-aware tile size instead of raw tile rectangle
    int calcW, calcH;
    CalcTileSize(tile, tlutType, calcW, calcH);
    key.width   = static_cast<uint16_t>(calcW);
    key.height  = static_cast<uint16_t>(calcH);
    key.format  = static_cast<Tex::N64TexFormat>(tile.format);
    key.size    = static_cast<Tex::N64TexSize>(tile.size);
    key.cms     = tile.clampS;
    key.cmt     = tile.clampT;
    key.maskS   = tile.maskS;
    key.maskT   = tile.maskT;
    key.shiftS  = tile.shiftS;
    key.shiftT  = tile.shiftT;
    key.line    = tile.line;
    key.tmemOffset = tile.tmemAddr;  // TMEM base word address for this tile's data
    key.tlutType = tlutType;

    // Compute CRC over the relevant TMEM region for content-change detection.
    // The tile's TMEM byte address tells us where the data lives in TMEM.
    uint32_t tmemByteAddr = tile.GetTMEMByteAddr();
    uint32_t tmemSize     = static_cast<uint32_t>(tile.GetBytesPerRow()) *
                            static_cast<uint32_t>(key.height);
    // RGBA32: data is stored in two 2KB TMEM banks (RG in bank 0, BA in bank 1).
    // CRC each bank separately and combine, rather than doubling the range
    // (which can overshoot TMEM and produce crc=0 for all RGBA32 textures).
    bool isRGBA32 = (key.format == Tex::N64TexFormat::RGBA && key.size == Tex::N64TexSize::Bits32);
    // CI textures: index data lives in the lower 2KB of TMEM (upper 2KB is palette).
    // Clamp the CRC range so it doesn't bleed into palette data.
    uint32_t tmemLimit = (key.format == Tex::N64TexFormat::CI) ? TMEM_PALETTE_BASE : (isRGBA32 ? 2048u : TMEM_SIZE);
    if (tmemByteAddr + tmemSize > tmemLimit)
        tmemSize = (tmemByteAddr < tmemLimit) ? (tmemLimit - tmemByteAddr) : 0;
    if (tmemSize > 0 && tmemByteAddr + tmemSize <= TMEM_SIZE) {
        key.crc = CRC32_Fast(m_state.rdp.tmem.data + tmemByteAddr, tmemSize);
        if (isRGBA32) {
            // CRC bank 1 (offset +2048) and mix into the hash
            uint32_t bank1Addr = tmemByteAddr + 2048;
            if (bank1Addr + tmemSize <= TMEM_SIZE) {
                uint32_t bank1Crc = CRC32_Fast(m_state.rdp.tmem.data + bank1Addr, tmemSize);
                key.crc ^= bank1Crc * 0x9e3779b9u;
            }
        }
    } else {
        key.crc = 0;
    }

    // For CI (Color Indexed) textures or when TLUT mode is active, palette
    // lookup makes the decoded pixels depend on the TLUT contents in the upper
    // half of TMEM.  Mix the palette region CRC into the texture CRC so a
    // palette change triggers a cache miss.
    if (key.format == Tex::N64TexFormat::CI || tlutType != 0) {
        uint32_t palCrc = 0;
        if (key.size == Tex::N64TexSize::Bits4) {
            // CI4: 16 entries starting at TMEM_PALETTE_BASE + palette*0x80, each 8 bytes apart
            uint32_t palBase = TMEM_PALETTE_BASE + static_cast<uint32_t>(tile.palette) * 0x80;
            if (palBase + 16 * 8 <= TMEM_SIZE)
                palCrc = CRC32_Fast(m_state.rdp.tmem.data + palBase, 16 * 8);
        } else {
            // CI8: 256 entries starting at TMEM_PALETTE_BASE, each 8 bytes apart (full upper TMEM)
            if (TMEM_PALETTE_BASE + 256 * 8 <= TMEM_SIZE)
                palCrc = CRC32_Fast(m_state.rdp.tmem.data + TMEM_PALETTE_BASE, 256 * 8);
        }
        key.crc ^= palCrc * 0x9e3779b9;
    }

    // Incorporate mip tile chain TMEM data into the CRC for mipmapped textures
    key.mipLevels = 0;
    if (mipTiles && mipCount > 0) {
        key.mipLevels = static_cast<uint8_t>(mipCount);
        for (int i = 0; i < mipCount; i++) {
            uint32_t mipByteAddr = static_cast<uint32_t>(mipTiles[i].tmemAddr) * 8;
            uint32_t mipSize = static_cast<uint32_t>(mipTiles[i].GetBytesPerRow()) *
                               static_cast<uint32_t>(std::max(1, mipTiles[i].GetHeight()));
            if (mipSize > 0 && mipByteAddr + mipSize <= 4096) {
                uint32_t mipCrc = CRC32_Fast(m_state.rdp.tmem.data + mipByteAddr, mipSize);
                key.crc ^= mipCrc * (0x9e3779b9 + static_cast<uint32_t>(i + 1));
            }
        }
    }

    return key;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Batch Management
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::FlushBatch() {
    if (m_batch.drawCalls.empty()) return;
    if (!m_renderer) {
        m_batch.Reset();
        return;
    }

    // Mark the current render-target framebuffer as GPU-modified so that
    // subsequent LoadBlock/LoadTile calls know to sync GPU→RDRAM.
    if (auto* curFB = g_fbManager.GetCurrentColor())
        curFB->gpuModified = true;

    // ═══════════════════════════════════════════════════════════════════
    //  1. Upload vertex data to a dynamic VBO
    // ═══════════════════════════════════════════════════════════════════
    //
    // RenderBatch::Vertex layout (44 bytes):
    //   float x, y, z, w   � position   (offset 0,  4 floats)
    //   float s, t          � texcoord   (offset 16, 2 floats)
    //   float r, g, b, a    � color      (offset 24, 4 floats)
    //   float fogFactor     � fog        (offset 40, 1 float)
    //
    // The N64 VertexLayout from Renderer.hpp expects:
    //   loc 0: float4 position (offset 0)
    //   loc 1: float4 color    (offset 16)
    //   loc 2: float2 texcoord0(offset 32)
    //   loc 3: float2 texcoord1(offset 40)
    //   loc 4: float1 fog      (offset 48)
    //   stride = 52
    //
    // We convert RenderBatch::Vertex → interleaved GPU vertex format.

    struct GPUVertex {
        float x, y, z, w;      // position (loc 0)
        float r, g, b, a;      // color (loc 1)
        float s0, t0;          // texcoord0 (loc 2)
        float s1, t1;          // texcoord1 (loc 3) � same as tex0 for now
        float fog;              // fog (loc 4)
        float perspW;           // 1/clip_W for perspective-correct textures (loc 5)
        float baryS, baryT;    // barycentric coords for sub-pixel coverage AA (loc 6)
    };
    static_assert(sizeof(GPUVertex) == 64, "GPUVertex must be 64 bytes for N64 layout");

    const size_t vertexCount = m_batch.vertices.size();
    const size_t vbSize = vertexCount * sizeof(GPUVertex);

    // Convert batch vertices to GPU format (reuse persistent scratch buffer)
    m_gpuVertScratch.resize(vbSize);
    GPUVertex* gpuVerts = reinterpret_cast<GPUVertex*>(m_gpuVertScratch.data());
    for (size_t i = 0; i < vertexCount; ++i) {
        const auto& src = m_batch.vertices[i];
        auto& dst = gpuVerts[i];
        dst.x  = src.x;  dst.y  = src.y;  dst.z  = src.z; dst.w = src.w;
        dst.r  = src.r;  dst.g  = src.g;  dst.b  = src.b; dst.a = src.a;
        dst.s0 = src.s;  dst.t0 = src.t;
        dst.s1 = src.s1; dst.t1 = src.t1;
        dst.fog = src.fogFactor;
        dst.perspW = src.perspW;
        dst.baryS = 0.333333f; dst.baryT = 0.333333f; // default: center (full coverage)
    }

    // Assign barycentric coordinates per draw call for sub-pixel coverage AA.
    // Triangles get (1,0), (0,1), (0,0) so the fragment shader can compute
    // 8-sample edge coverage. Non-triangle draws (rects) keep the center
    // value (1/3,1/3) which guarantees full coverage (no edge AA on rects).
    static const float kTriBary[3][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}};
    for (const auto& dc : m_batch.drawCalls) {
        if (dc.type == DrawCall::Type::Triangle) {
            for (int j = 0; j < dc.vertexCount; j += 3) {
                int base = dc.vertexStart + j;
                if (base + 2 < static_cast<int>(vertexCount)) {
                    gpuVerts[base + 0].baryS = kTriBary[0][0]; gpuVerts[base + 0].baryT = kTriBary[0][1];
                    gpuVerts[base + 1].baryS = kTriBary[1][0]; gpuVerts[base + 1].baryT = kTriBary[1][1];
                    gpuVerts[base + 2].baryS = kTriBary[2][0]; gpuVerts[base + 2].baryT = kTriBary[2][1];
                }
            }
        }
    }

    // --- Ring-buffer VBO: allocate once, sub-upload each flush ---
    if (!m_ringVBO) {
        m_ringVBO = m_renderer->CreateVertexBuffer(RING_VBO_SIZE, BufferUsage::Stream);
        m_ringOffset = 0;
        if (!m_ringVBO) {
            m_batch.Reset();
            return;
        }
    }

    // If the current batch doesn't fit in the remaining ring space, wrap to 0
    // (orphan the old data by re-uploading from offset 0).
    if (m_ringOffset + vbSize > RING_VBO_SIZE) {
        m_ringOffset = 0;
    }

    const size_t batchBaseVertex = m_ringOffset / sizeof(GPUVertex);
    m_ringVBO->Update(gpuVerts, m_ringOffset, vbSize);
    m_ringOffset += vbSize;

    VertexLayout layout = VertexLayout::CreateN64();

    // Lazily create the N64 pipeline (owns the VAO with the correct vertex format).
    // BindPipeline sets m_boundVAO, which BindVertexBuffer requires.
    if (!m_n64Pipeline) {
        PipelineDesc pipeDesc{};
        pipeDesc.vertexLayout = layout;
        pipeDesc.primitiveType = PrimitiveType::Triangles;
        pipeDesc.debugName = "N64_RDP_Default";
        // Disable back-face culling: the N64 RSP already handles culling, and the
        // ortho Y-flip reverses triangle winding (CW in NDC), which would wrongly
        // discard all geometry with the default CullMode::Back setting.
        pipeDesc.rasterizer.cullMode = CullMode::None;
        m_n64Pipeline = m_renderer->CreatePipeline(pipeDesc);
    }
    if (m_n64Pipeline) {
        m_renderer->BindPipeline(m_n64Pipeline.get());
    }

    m_renderer->BindVertexBuffer(m_ringVBO.get(), layout);

    // Set the viewport to match the FBO dimensions
    {
        GPUFramebuffer* curFBO_vp = g_fbManager.GetCurrentColor();
        float vpW = (curFBO_vp && curFBO_vp->scaledWidth  > 0) ? static_cast<float>(curFBO_vp->scaledWidth)  : 320.0f;
        float vpH = (curFBO_vp && curFBO_vp->scaledHeight > 0) ? static_cast<float>(curFBO_vp->scaledHeight) : 240.0f;
        RenderViewport vp;
        vp.x = 0; vp.y = 0; vp.width = vpW; vp.height = vpH;
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        m_renderer->SetViewport(vp);
    }

    // =======================================================================
    //  2. Iterate draw calls, set state, and draw
    // =======================================================================

    // Compute N64 screen dimensions from scissor for ortho projection
    float n64W = static_cast<float>(m_state.rdp.scissor.xl >> 2);
    float n64H = static_cast<float>(m_state.rdp.scissor.yl >> 2);
    if (n64W < 1.0f) n64W = 320.0f;
    if (n64H < 1.0f) n64H = 240.0f;

    // Intel Iris Xe workaround: FBOs created lazily (after game textures are
    // loaded) can fail texture sampling.  Recreating the FBO here � during the
    // rendering phase � produces a fresh GL object that works correctly.
    {
        static std::unordered_set<uint32_t> recreatedFBOs;
        GPUFramebuffer* curFB = g_fbManager.GetCurrentColor();
        if (curFB && curFB->fbo != 0 && recreatedFBOs.find(curFB->fbo) == recreatedFBOs.end()) {
            g_fbManager.RecreateGPUFramebuffer(*curFB);
            glBindFramebuffer(GL_FRAMEBUFFER, curFB->fbo);
            glViewport(0, 0, curFB->scaledWidth, curFB->scaledHeight);
            recreatedFBOs.insert(curFB->fbo);
        }
    }

    // Ortho matrix: maps N64 screen coords (0..n64W, 0..n64H, z_norm 0..1) to NDC.
    // Z: clip_z = 2*z_norm - 1, so window_depth = z_norm ? [0,1] (full precision).
    // Near z_norm=0 � depth=0, Far z_norm=1 � depth=1.0 (= clear value).
    // GL_LEQUAL: near objects (depth � 0) correctly overwrite far objects (depth � 1).
    // Column-major for OpenGL: ortho(left=0, right=n64W, top=0, bottom=n64H)
    float ortho[16] = {
         2.0f/n64W, 0,          0,   0,
         0,        -2.0f/n64H,  0,   0,
         0,         0,           2,   0,
        -1.0f,      1.0f,       -1,   1
    };



    // FBO dimensions for scissor scaling � use the current color FBO size, not the window.
    // The N64 renders to a 320�240 FBO; using window dims (640�480) would double all scissors
    // (scaleX = 640/320 = 2.0) causing every scissor rect to overflow the FBO entirely.
    GPUFramebuffer* curFBO = g_fbManager.GetCurrentColor();
    float fboW = (curFBO && curFBO->scaledWidth  > 0) ? static_cast<float>(curFBO->scaledWidth)  : n64W;
    float fboH = (curFBO && curFBO->scaledHeight > 0) ? static_cast<float>(curFBO->scaledHeight) : n64H;

    bool batchUsedN64Depth = false;

    for (const auto& call : m_batch.drawCalls) {

        // --- Build CombinerKey for shader lookup ---
        Shader::CombinerKey shaderKey{};
        shaderKey.mux = call.combineMode;
        shaderKey.blenderBits = call.otherModeL & 0xFFFF0000; // blender bits in upper portion
        shaderKey.otherModeH = static_cast<uint16_t>((call.otherModeH >> 8) & 0xFFFF);
        shaderKey.otherModeL = static_cast<uint16_t>(call.otherModeL & 0xFFFF);

        // Derive feature flags from other modes
        uint64_t features = Shader::SF_NONE;
        uint32_t cycleType = (call.otherModeH >> 20) & 0x03;

        // FillRect in FILL mode (cycleType=3) uses the fill-color shader path directly.
        // FillRect in 1-cycle/2-cycle modes should evaluate the actual combiner
        // (shade color = 0 since there's no vertex shading for rectangles).
        if (call.type == DrawCall::Type::FillRect && cycleType == 3) {
            features |= Shader::SF_FILL_RECT;
        } else if (call.type == DrawCall::Type::FillRect) {
            // Non-FILL FillRect: use the regular cycle mode so the combiner runs
            switch (cycleType) {
                case 0: features |= Shader::SF_1CYCLE; break;
                case 1: features |= Shader::SF_2CYCLE; break;
                case 2: features |= Shader::SF_COPY_MODE; break;
                default: features |= Shader::SF_FILL_RECT; break;
            }
        } else {
            switch (cycleType) {
                case 0: features |= Shader::SF_1CYCLE; break;
                case 1: features |= Shader::SF_2CYCLE; break;
                case 2: features |= Shader::SF_COPY_MODE; break;
                case 3: features |= Shader::SF_FILL_RECT; break;
            }
        }

        // Texture enable
        if (call.textureHash0 != 0) features |= Shader::SF_TEXTURE_0;
        if (call.textureHash1 != 0) features |= Shader::SF_TEXTURE_1;

        // Depth
        // N64 depth: Z_CMP (bit 4) enables depth comparison, Z_UPD (bit 5)
        // enables depth writes. G_ZBUFFER is an RSP flag that tells the RSP to
        // compute per-vertex Z � it does NOT enable the RDP's depth test.
        // When Z_UPD is set without Z_CMP, the RDP writes depth unconditionally
        // (depth init mode) � we use depthFunc=ALWAYS for that case.
        {
            bool zCmp = (call.otherModeL & 0x0010) != 0;
            bool zUpd = (call.otherModeL & 0x0020) != 0;
            bool zPrim = (call.otherModeL & 0x0004) != 0;
            uint32_t cycType = (call.otherModeH >> 20) & 0x03;
            bool depthEnabled = (zCmp || zUpd || zPrim) && (cycType <= 1);
            if (depthEnabled) features |= Shader::SF_DEPTH_TEST;
            if (zUpd) features |= Shader::SF_DEPTH_WRITE;
            if (zPrim) features |= Shader::SF_DEPTH_SOURCE_PRIM;
        }

        // N64-accurate Z-image depth compare: enable when Z_CMP is set,
        // the depth buffer manager has a current Z-image texture pair,
        // and the GPU supports fragment shader interlock for imageLoad/imageStore.
        //
        // INTEL WORKAROUND: When the MLE blender is also active (SF_MLE_BLENDER),
        // the shader would need to access 4 image bindings (Z-image, delta-Z,
        // color, coverage) within a single interlock region.  Intel Iris Xe
        // drivers silently fail when the interlock region spans too many
        // image operations, causing all depth-tested MLE draws to become
        // invisible.  Fall back to GL depth test for these draws — the
        // MLE blender still handles color/coverage blending correctly.
        //
        // INTEL WORKAROUND #2: Also skip N64 depth compare when dual-source
        // blend is active.  Intel Iris Xe silently fails when image bindings
        // (Z-image imageLoad/imageStore) are combined with dual-source
        // fragment outputs (fragColor + fragColor1) in the same shader.
        // Fall back to GL depth test (with z-bias 0.99999 for far-plane).
        if (!DISABLE_N64_DEPTH_COMPARE &&
            (call.otherModeL & 0x0010) && g_depthManager.GetCurrent() &&
            g_depthManager.GetCurrent()->glZImageTex != 0 &&
            m_renderer->GetCapabilities().supportsFragmentShaderInterlock &&
            !(features & Shader::SF_MLE_BLENDER) &&
            !(m_renderer->GetCapabilities().isIntelGPU && (features & Shader::SF_DUAL_SOURCE_BLEND))) {
            features |= Shader::SF_DEPTH_COMPARE;
            features |= Shader::SF_N64_DEPTH_COMPARE;
        }

        // Depth render: when the color image IS the depth image, the
        // combiner output (fragColor.r) is a depth value that needs to be
        // written into the Z-image and gl_FragDepth.  Games that rely on
        // this include Zelda: Majora's Mask (depth buffer copy effects).
        if (m_state.rdp.colorImage.address == m_state.rdp.zbufferImage.address &&
            m_state.rdp.colorImage.address != 0) {
            features |= Shader::SF_DEPTH_RENDER;
        }

        // Alpha compare: bit 0 = G_AC_THRESHOLD, bit 1 = G_AC_NOISE;
        // both modes need the alpha discard test in the shader.
        // FILL mode: alpha test is always disabled (hardware ignores it).
        // COPY mode: N64 hardware ALWAYS tests texel alpha against
        // blendColor.alpha regardless of alpha compare bits � force it on.
        uint32_t alphaCmp = call.otherModeL & 0x03;
        if (cycleType == 2) {
            // COPY mode: unconditional alpha test per N64 hardware
            features |= Shader::SF_ALPHA_TEST;
        } else if ((alphaCmp & 1) && cycleType != 3) {
            features |= Shader::SF_ALPHA_TEST;
        }

        // Fog (from geometry mode)
        if (call.geometryMode & 0x00010000) features |= Shader::SF_FOG;

        // Texture filter (otherModeH bits 12-13)
        //   0 = G_TF_POINT   � nearest-neighbor (no flag)
        //   2 = G_TF_BILERP  � N64 3-sample interpolation
        //   3 = G_TF_AVERAGE � true bilinear (for LOD blending)
        // COPY mode always uses point sampling on real N64 hardware.
        uint32_t texFilter = (cycleType == 2) ? 0 : ((call.otherModeH >> 12) & 0x03);
        if (texFilter == 2) features |= Shader::SF_TEXTURE_3POINT;
        else if (texFilter == 3) features |= Shader::SF_TEXTURE_BILINEAR;

        // Monochrome post-process (game-specific hack — e.g. Zelda OoT cutscenes).
        // Only activate when the tile is CI8 over what was originally an RGBA16
        // framebuffer — that is the specific N64 trick Zelda uses for grayscale
        // flashbacks.  Applying this unconditionally would desaturate everything.
        if (m_hacks.zeldaMonochrome &&
            call.tileDesc0.format == 2 /* G_IM_FMT_CI */ &&
            call.tileDesc0.size   == 1 /* G_IM_SIZ_8b */ &&
            g_fbManager.FindByAddress(call.textureImageAddr) != nullptr)
            features |= Shader::SF_MONOCHROME;

        // Texture LOD (G_TL_LOD � mipmap fraction via LOD_FRACTION combiner input)
        if (call.otherModeH & 0x00010000) features |= Shader::SF_TEXTURE_LOD;

        // Texture detail/sharpen (otherModeH bits 17-18: 0=clamp, 1=sharpen, 2=detail)
        {
            uint32_t tdMode = (call.otherModeH >> 17) & 0x03;
            if (tdMode == 1) features |= Shader::SF_TEXTURE_SHARPEN;
            if (tdMode == 2) features |= Shader::SF_TEXTURE_DETAIL;
        }

        // YUV texture format (tile format == 1) � enable shader YUV�RGB conversion
        if (call.tileDesc0.format == 1) features |= Shader::SF_TEXTURE_YUV;

        // Chroma key enable (otherModeH bit 8)
        if (call.otherModeH & 0x00000100) features |= Shader::SF_CHROMA_KEY;

        // Alpha noise/dither (otherModeL bits 0-1: 1=threshold, 3=dither � note: value 2 unused)
        if ((call.otherModeL & 0x03) == 3) {
            features |= Shader::SF_NOISE_ENABLE;
            features |= Shader::SF_ALPHA_NOISE_DITHER;
        }

        // Also enable noise when the combiner mux references NOISE
        // (G_CCMUX_NOISE = 7 in the RGB SubA/A position, 4-bit field).
        {
            uint32_t muxHi = static_cast<uint32_t>(call.combineMode >> 32);
            int saRGB0 = (muxHi >> 20) & 0xF;  // cycle 0 SubA RGB
            int saRGB1 = (muxHi >>  5) & 0xF;  // cycle 1 SubA RGB
            if (saRGB0 == 7 || saRGB1 == 7)
                features |= Shader::SF_NOISE_ENABLE;
        }

        // RGB dither (otherModeH bits 6-7: 0=magic square, 1=bayer, 2=noise, 3=none)
        {
            uint32_t rgbDither = (call.otherModeH >> 6) & 0x03;
            if (rgbDither != 3) features |= Shader::SF_DITHER_ENABLE;
        }

        // Alpha dither (otherModeH bits 4-5: 0=pattern, 1=~pattern, 2=noise, 3=none)
        {
            uint32_t alphaDither = (call.otherModeH >> 4) & 0x03;
            if (alphaDither != 3) features |= Shader::SF_ALPHA_DITHER;
        }

        // Framebuffer read & blender memColor strategy.
        //
        // MLE BLENDER: When the blender references memColor or needs FB
        // coverage, we use the full shader-based read-modify-write path
        // (imageLoad/imageStore) with fragment shader interlock.  This
        // replaces the old SF_FRAMEBUFFER_READ and SF_DUAL_SOURCE_BLEND
        // approximations and gives pixel-perfect N64 blender behavior.
        {
            uint32_t blenderBits = call.otherModeL & 0xFFFF0000;

            // IM_RD (otherModeL bit 6): when clear, RDP doesn't read FB.
            bool imageRead = (call.otherModeL & 0x40) != 0;

            if (imageRead && blenderBits != 0) {
                // Performance gate: only enable the expensive MLE (interlock)
                // path when the blender actually needs to READ framebuffer.
                // Skip MLE when no blender input references memColor/memCvg
                // AND coverage destination is FULL (always writes 7).
                int p1 = (blenderBits >> 30) & 0x3;
                int m1 = (blenderBits >> 22) & 0x3;
                int b1 = (blenderBits >> 18) & 0x3;
                bool needsMem = (p1 == 1 || m1 == 1 || b1 == 1);
                if (cycleType == 1) { // 2-cycle
                    int p2 = (blenderBits >> 28) & 0x3;
                    int m2 = (blenderBits >> 20) & 0x3;
                    int b2 = (blenderBits >> 16) & 0x3;
                    needsMem = needsMem || (p2 == 1 || m2 == 1 || b2 == 1);
                }
                int cvgDst = (call.otherModeL >> 8) & 0x03;
                if (cvgDst != 2) needsMem = true; // CLAMP/WRAP/SAVE read mem_cvg

                // INTEL WORKAROUND: Intel Iris Xe fragment shader interlock
                // with imageLoad/imageStore silently fails for certain MLE
                // blender configurations (e.g. OoT title screen logo draws
                // with 2-cycle fog blender).  Fall back to dual-source blend
                // on Intel GPUs for reliable blender emulation.
                if (needsMem && !DISABLE_MLE_BLENDER &&
                    m_renderer->GetCapabilities().supportsFragmentShaderInterlock &&
                    !m_renderer->GetCapabilities().isIntelGPU)
                    features |= Shader::SF_MLE_BLENDER;
                else if (needsMem)
                    features |= Shader::SF_DUAL_SOURCE_BLEND;
            }
        }

        // FORCE_BL (otherModeL bit 14) � when set, the blender always applies
        // the full (P*A+M*B)/(A+B) formula.  When clear, fully-covered pixels
        // bypass the blend and output P directly.
        if (call.otherModeL & 0x4000) features |= Shader::SF_FORCE_BLEND;

        // Coverage / AA flags (otherModeL)
        if (call.otherModeL & 0x0008) features |= Shader::SF_COVERAGE_ENABLE;  // AA_EN   bit 3
        if (call.otherModeL & 0x0080) features |= Shader::SF_COLOR_ON_COVERAGE; // CLR_ON_CVG bit 7
        if (call.otherModeL & 0x1000) features |= Shader::SF_ALPHA_CVG;         // CVG_X_ALPHA bit 12
        if (call.otherModeL & 0x2000) features |= Shader::SF_ALPHA_CVG_SEL;     // ALPHA_CVG_SEL bit 13

        // COPY/FILL modes: clear coverage-alpha flags (hardware ignores them)
        if (cycleType >= 2) {
            features &= ~(uint64_t)(Shader::SF_ALPHA_CVG_SEL);
        }

        shaderKey.features = features;

        // �� Shadow map override ����������������������������������������
        // When this draw call is a shadow map rect, replace the entire
        // combiner/blender pipeline with a specialised shader that reads
        // the depth buffer and converts it via a palette (TLUT) into a
        // fog-coloured shadow overlay.
        if (call.isShadowMap) {
            shaderKey.features = Shader::SF_SHADOW_MAP;
            shaderKey.mux = 0;
            shaderKey.blenderBits = 0;
            shaderKey.otherModeH = 0;
            shaderKey.otherModeL = 0;
        }

        // --- Get/compile shader ---
        const Shader::CompiledShader* shader = nullptr;
        if (m_shaderManager) {
            shader = m_shaderManager->GetShader(shaderKey);
        }


        if (!shader) {
            static int nullShaderWarnCount = 0;
            if (nullShaderWarnCount < 10) {
                NOVA_LOG_WARN("FlushBatch: null shader for draw#{} mux=0x{:016X} features=0x{:012X} cycleType={} drawType={}",
                              nullShaderWarnCount, shaderKey.mux, (unsigned long long)shaderKey.features,
                              (shaderKey.features & 0xF), static_cast<int>(call.type));
                ++nullShaderWarnCount;
            }
        }

        if (shader) {
            m_shaderManager->BindShader(shader);

            // Set MVP matrix: ortho for both 2D and 3D.
            // 3D vertices are mapped to N64 screen space coordinates by applying
            // N64 viewport scaling in CPU before being sent here.
            m_shaderManager->SetMatrixUniforms(ortho, nullptr);

            // Set color uniforms from draw call snapshot (not live state)
            float primColor[4] = {
                call.primColor.r / 255.0f,
                call.primColor.g / 255.0f,
                call.primColor.b / 255.0f,
                call.primColor.a / 255.0f
            };
            float envColor[4] = {
                call.envColor.r / 255.0f,
                call.envColor.g / 255.0f,
                call.envColor.b / 255.0f,
                call.envColor.a / 255.0f
            };
            float fogColor[4] = {
                call.fogColor.r / 255.0f,
                call.fogColor.g / 255.0f,
                call.fogColor.b / 255.0f,
                call.fogColor.a / 255.0f
            };
            float blendColor[4] = {
                call.blendColor.r / 255.0f,
                call.blendColor.g / 255.0f,
                call.blendColor.b / 255.0f,
                call.blendColor.a / 255.0f
            };
            m_shaderManager->SetColorUniforms(
                primColor, envColor, fogColor, blendColor,
                call.primLODFrac, call.primLODMin,
                m_state.rdp.chromaKey.centerR / 255.0f,
                m_state.rdp.chromaKey.centerG / 255.0f,
                m_state.rdp.chromaKey.centerB / 255.0f,
                m_state.rdp.chromaKey.scaleR / 255.0f,
                m_state.rdp.chromaKey.scaleG / 255.0f,
                m_state.rdp.chromaKey.scaleB / 255.0f);

            // Upload chroma key width
            m_shaderManager->SetKeyWidth(
                m_state.rdp.chromaKey.widthR / 255.0f,
                m_state.rdp.chromaKey.widthG / 255.0f,
                m_state.rdp.chromaKey.widthB / 255.0f);

            // Set fill color for fill rect draws (shader reads uFillColor)
            if (call.type == DrawCall::Type::FillRect) {
                const auto& v0 = m_batch.vertices[call.vertexStart];
                m_shaderManager->SetFillColor(v0.r, v0.g, v0.b, v0.a);
            }

            // Set screen size uniforms (uScreenSize, uInvScreenSize) � use FBO dims
            m_shaderManager->SetScreenSize(fboW, fboH);

            // Set frame counter for noise/dither animation
            m_shaderManager->SetFrameCount(m_state.frameCount);

            // Set texture sampler uniforms (uTexture0 -> unit 0, uTexture1 -> unit 1)
            // and texture scale/size for the N64 3-point filter
            {
                auto getShiftScale = [](uint32_t shift) -> float {
                    if (shift == 0) return 1.0f;
                    if (shift <= 10) return 1.0f / (1 << shift);
                    return static_cast<float>(1 << (16 - shift));
                };

                // N64 hardware: the tile's shiftS/shiftT only applies to triangle
                // texture coordinates (from the RSP vertex pipeline).  TextureRect
                // S/T values go directly to the texture lookup � no tile shift.
                bool isRect = (call.type == DrawCall::Type::TextureRect ||
                               call.type == DrawCall::Type::FillRect);
                float scale0[2], scale1[2];
                if (isRect) {
                    scale0[0] = scale0[1] = 1.0f;
                    scale1[0] = scale1[1] = 1.0f;
                } else {
                    scale0[0] = getShiftScale(call.tileDesc0.shiftS);
                    scale0[1] = getShiftScale(call.tileDesc0.shiftT);
                    scale1[0] = getShiftScale(call.tileDesc1.shiftS);
                    scale1[1] = getShiftScale(call.tileDesc1.shiftT);
                }
                float size0[2]  = { 1.0f, 1.0f };
                float size1[2]  = { 1.0f, 1.0f };

                // Use mask/clamp-aware tile sizes (must match BuildTextureKey)
                uint8_t tlut = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
                int w0, h0;
                CalcTileSize(call.tileDesc0, tlut, w0, h0);
                if (w0 > 0 && h0 > 0) {
                    size0[0] = static_cast<float>(w0);
                    size0[1] = static_cast<float>(h0);
                }
                int w1, h1;
                CalcTileSize(call.tileDesc1, tlut, w1, h1);
                if (w1 > 0 && h1 > 0) {
                    size1[0] = static_cast<float>(w1);
                    size1[1] = static_cast<float>(h1);
                }

                m_shaderManager->SetTextureUniforms(0, 1, scale0, scale1, size0, size1, nullptr, nullptr);
            }

            // Set tile offset, clamp, mirror, mask uniforms
            {
                float offset0[2] = { call.tileDesc0.GetSL() + call.scrollS0, call.tileDesc0.GetTL() + call.scrollT0 };
                float offset1[2] = { call.tileDesc1.GetSL() + call.scrollS1, call.tileDesc1.GetTL() + call.scrollT1 };
                // clamp vec4 = (clampS, clampT, mirrorS, mirrorT)
                float clamp0[4] = {
                    call.tileDesc0.IsClamped_S() ? 1.0f : 0.0f,
                    call.tileDesc0.IsClamped_T() ? 1.0f : 0.0f,
                    call.tileDesc0.IsMirrored_S() ? 1.0f : 0.0f,
                    call.tileDesc0.IsMirrored_T() ? 1.0f : 0.0f
                };
                float clamp1[4] = {
                    call.tileDesc1.IsClamped_S() ? 1.0f : 0.0f,
                    call.tileDesc1.IsClamped_T() ? 1.0f : 0.0f,
                    call.tileDesc1.IsMirrored_S() ? 1.0f : 0.0f,
                    call.tileDesc1.IsMirrored_T() ? 1.0f : 0.0f
                };
                float mask0[2] = { static_cast<float>(call.tileDesc0.maskS), static_cast<float>(call.tileDesc0.maskT) };
                float mask1[2] = { static_cast<float>(call.tileDesc1.maskS), static_cast<float>(call.tileDesc1.maskT) };
                m_shaderManager->SetTextureTileParams(offset0, offset1, clamp0, clamp1, mask0, mask1);
            }

            // Set texture coordinate bounds for TextureRect bilinear bleed prevention
            m_shaderManager->SetTexCoordBounds(call.useTexCoordBounds,
                                                call.texCoordBounds0, call.texCoordBounds1);

            // Texture perspective is now handled in vertex setup (DrawTriangle sets
            // perspW = invW when TEXTPERSP is on, perspW = 1.0 when off), so
            // the shader always divides by vClipW and we no longer need the
            // uTexturePersp uniform toggle here.

            // Screen scale for resolution-independent LOD computation
            // Screen scale = scaledSize / nativeSize; at native res this is (1,1).
            // Shaders divide gl_FragCoord by this to get native coords, and multiply
            // LOD derivatives by this to compensate for higher pixel density.
            {
                float scaleX = (curFBO ? static_cast<float>(curFBO->scaledWidth)  / std::max(1.0f, static_cast<float>(curFBO->width))  : 1.0f);
                float scaleY = (curFBO ? static_cast<float>(curFBO->scaledHeight) / std::max(1.0f, static_cast<float>(curFBO->height)) : 1.0f);
                m_shaderManager->SetScreenScale(scaleX, scaleY);
            }

            // LOD mipmap parameters
            {
                int tdMode = (call.otherModeH >> 17) & 0x3; // textureDetail: 0=clamp, 1=sharpen, 2=detail
                int maxTile = static_cast<int>(m_state.rdp.geometry.texture.level);
                m_shaderManager->SetLODParams(call.primLODMin, maxTile, tdMode);
            }

            // F3DAM fog usage mode
            m_shaderManager->SetFogUsage(call.fogUsage);

            // Screen-space triangle (noperspective shade) for rects
            m_shaderManager->SetScreenSpaceTriangle(
                (call.type == DrawCall::Type::TextureRect || call.type == DrawCall::Type::FillRect) ? 1 : 0);

            // Combiner debug output (0 = normal rendering)
            {
                int debugMode = m_debugOutputMode;
                if (m_debugMagenta)     debugMode = 9;  // override: magenta highlight
                else if (m_debugForceOpaque) debugMode = 8;  // override: force alpha=1
                m_shaderManager->SetDebugOutput(debugMode);
            }

            // Set alpha reference for alpha compare
            {
                uint32_t alphaCompMode = call.otherModeL & 0x03;
                if (cycleType == 2) {
                    // COPY mode: N64 hardware tests texel alpha > blendColor.alpha.
                    // Add half-LSB to convert strict ">" to shader's "<" test.
                    m_shaderManager->SetAlphaRef(call.blendColor.a / 255.0f + 0.5f / 255.0f);
                } else if (alphaCompMode & 1) {
                    // Threshold (1) or dither (3) � set blendColor.a as reference
                    m_shaderManager->SetAlphaRef(call.blendColor.a / 255.0f);
                }
            }

            // Set fog params from geometry state
            {
                float fogMul = static_cast<float>(m_state.rdp.geometry.fogMultiplier) / 256.0f;
                float fogOff = static_cast<float>(m_state.rdp.geometry.fogOffset) / 256.0f;
                m_shaderManager->SetFogParams(fogMul, fogOff);
            }

            // Set depth params (15-bit Z: mask off MSB, divide by 32768)
            m_shaderManager->SetDepthParams(1.0f, 0.0f,
                static_cast<float>(call.primDepth & 0x7FFF) / 32768.0f);

            // Upload YUV conversion coefficients (K0-K5)
            m_shaderManager->SetConvertK(m_state.rdp.convertK);
        }

        // --- Set blend state from N64 blender mux ---
        BlendState blend;
        {
            // N64 blender formula: (P * A + M * B) / (A + B)
            // otherModeL layout (bits 16-31):
            //   bits 30-31: P0  cycle 0   (0=CC,1=Mem,2=Blend,3=Fog)
            //   bits 28-29: P1  cycle 1
            //   bits 26-27: A0  cycle 0   (0=CCAlpha,1=FogAlpha,2=ShadeAlpha,3=Zero)
            //   bits 24-25: A1  cycle 1
            //   bits 22-23: M0  cycle 0   (0=CC,1=Mem,2=Blend,3=Fog)
            //   bits 20-21: M1  cycle 1
            //   bits 18-19: B0  cycle 0   (0=1-A,1=MemAlpha,2=One,3=Zero)
            //   bits 16-17: B1  cycle 1
            uint32_t p0  = (call.otherModeL >> 30) & 3;
            uint32_t a0  = (call.otherModeL >> 26) & 3;
            uint32_t m0  = (call.otherModeL >> 22) & 3;
            uint32_t b0  = (call.otherModeL >> 18) & 3;
            uint32_t p1  = (call.otherModeL >> 28) & 3;
            uint32_t a1  = (call.otherModeL >> 24) & 3;
            uint32_t m1  = (call.otherModeL >> 20) & 3;
            uint32_t b1  = (call.otherModeL >> 16) & 3;
            bool forceBlend = ((call.otherModeL >> 14) & 1) != 0;

            // In 2-cycle mode, the final pixel color comes from cycle 1.
            // In 1-cycle (or copy/fill), use cycle 0.
            uint32_t cycleType = (call.otherModeH >> 20) & 3;
            uint32_t pF = (cycleType == 1) ? p1 : p0;
            uint32_t aF = (cycleType == 1) ? a1 : a0;
            uint32_t mF = (cycleType == 1) ? m1 : m0;
            uint32_t bF = (cycleType == 1) ? b1 : b0;

            bool hasDualSource = (features & Shader::SF_DUAL_SOURCE_BLEND) != 0;
            bool hasMLE = (features & Shader::SF_MLE_BLENDER) != 0;

            // COPY and FILL modes bypass the blender entirely on real N64 hardware.
            // Disable GL blending unconditionally for these cycle types, matching
            // GLideN64's guard: cycleType < G_CYC_COPY.
            if (cycleType >= 2) {
                blend.enabled = false;
            } else if (hasMLE) {
                // �� MLE blender: shader does full read-modify-write �������
                // No GL blend needed; imageStore handles everything.
                blend.enabled = false;
            } else if (hasDualSource) {
                // �� Dual-source blending path �����������������������������
                // The shader outputs:
                //   fragColor  = source contribution (non-memColor terms)
                //   fragColor1 = destination factor  (memColor weight)
                // Hardware blend: fragColor * ONE + dst * SRC1_COLOR
                blend.enabled = true;
                blend.srcColor = BlendFactor::One;
                blend.dstColor = BlendFactor::Src1Color;
                blend.srcAlpha = BlendFactor::One;
                blend.dstAlpha = BlendFactor::Src1Alpha;

                // Special case from GLideN64: when B (not M) references
                // memColor.a, the shader approximates it as 1.0 but the
                // real factor should multiply by dst alpha.  Adjust the
                // GL blend accordingly.
                if (forceBlend) {
                    if (mF != 1 && bF == 1) {
                        // M is NOT memColor, B IS memColor.a � src needs DST_ALPHA
                        blend.srcColor = BlendFactor::DstAlpha;
                        blend.srcAlpha = BlendFactor::DstAlpha;
                    }
                    if (mF == 1 && bF == 1) {
                        // Both M=memColor and B=memColor.a � dst also gets DST_ALPHA
                        blend.dstColor = BlendFactor::DstAlpha;
                        blend.dstAlpha = BlendFactor::DstAlpha;
                    }
                }

                // P==M � result is always P regardless of blend factors
                if (pF == mF) {
                    blend.enabled = false;
                }
            } else {
                // �� Legacy GL blend approximation �������������������������
                // Match the shader-side skip: only when P or M is memColor.
                bool needsMemColor = (pF == 1) || (mF == 1);
                bool hasFBRead = (features & Shader::SF_FRAMEBUFFER_READ) != 0;
                bool shaderBlenderSkipped = needsMemColor && !hasFBRead;
                // On real N64, ALPHA_CVG_SEL without CVG_X_ALPHA means the
                // blender alpha comes from coverage.  For fully-covered
                // interior pixels coverage is 1.0, so the blend formula
                // degenerates to P (source only) � the surface is opaque.
                // Enabling GL blend for these modes produces additive
                // artifacts on overlapping geometry.
                bool alphaCvgSel = (features & Shader::SF_ALPHA_CVG_SEL) != 0;
                bool cvgXAlpha   = (features & Shader::SF_ALPHA_CVG) != 0;
                bool isOpaqueMode = alphaCvgSel && !cvgXAlpha;
                bool isTranslucent = shaderBlenderSkipped && forceBlend && !isOpaqueMode;

                if (!forceBlend && pF == 1 && cycleType < 2) {
                    // P = memColor without FORCE_BL: fully-covered pixels output P directly.
                    // Since P = framebuffer, this is a no-op draw. Use blend (ZERO, ONE)
                    // to preserve the existing framebuffer contents.
                    blend.enabled = true;
                    blend.srcColor = BlendFactor::Zero;
                    blend.dstColor = BlendFactor::One;
                    blend.srcAlpha = BlendFactor::Zero;
                    blend.dstAlpha = BlendFactor::One;
                } else {
                    // �� Blend enable & factor ������������������������������
                    // N64 formula: out = (P*A + M*B) / (A+B)
                    // Since the shader blender is skipped, fragColor = combined
                    // (raw combiner output, NOT pre-multiplied by A).
                    // Map A � srcFactor (applied to P=combined by GL)
                    //     B � dstFactor (applied to M=memColor=dst by GL)

                    // Helper: map blender A selector to GL blend factor
                    auto mapAToSrc = [&]() -> BlendFactor {
                        switch (aF) {
                        case 0: return BlendFactor::SrcAlpha;       // combined.a
                        case 1:                                     // fogColor.a
                            blend.blendColor[0] = call.fogColor.r / 255.0f;
                            blend.blendColor[1] = call.fogColor.g / 255.0f;
                            blend.blendColor[2] = call.fogColor.b / 255.0f;
                            blend.blendColor[3] = call.fogColor.a / 255.0f;
                            return BlendFactor::ConstantAlpha;
                        case 2: return BlendFactor::SrcAlpha;       // shadeAlpha ? srcAlpha
                        default: return BlendFactor::Zero;          // 0.0
                        }
                    };
                    auto mapOneMinusA = [&]() -> BlendFactor {
                        switch (aF) {
                        case 0: return BlendFactor::OneMinusSrcAlpha;
                        case 1:
                            blend.blendColor[0] = call.fogColor.r / 255.0f;
                            blend.blendColor[1] = call.fogColor.g / 255.0f;
                            blend.blendColor[2] = call.fogColor.b / 255.0f;
                            blend.blendColor[3] = call.fogColor.a / 255.0f;
                            return BlendFactor::OneMinusConstantAlpha;
                        case 2: return BlendFactor::OneMinusSrcAlpha;
                        default: return BlendFactor::One;           // 1 - 0
                        }
                    };
                    auto mapBToDst = [&]() -> BlendFactor {
                        switch (bF) {
                        case 0: return mapOneMinusA();   // 1 - A
                        case 1: return BlendFactor::DstAlpha;  // memColor.a
                        case 2: return BlendFactor::One;       // 1.0
                        default: return BlendFactor::Zero;     // 0.0
                        }
                    };

                    if (pF == 1 && mF == 1) {
                        // Both P and M are memColor � preserve framebuffer
                        blend.enabled = true;
                        blend.srcColor = BlendFactor::Zero;
                        blend.dstColor = BlendFactor::One;
                        blend.srcAlpha = BlendFactor::Zero;
                        blend.dstAlpha = BlendFactor::One;
                    } else if (pF == mF) {
                        // P == M � result is always P regardless of factors
                        blend.enabled = false;
                    } else {
                        blend.enabled = isTranslucent;

                        if (pF == 1) {
                            // P=memColor(dst), M=non-mem(src=combined)
                            // N64: out = mem*A + combined*B
                            // GL:  out = combined*srcFactor + dst*dstFactor
                            blend.srcColor = mapBToDst(); // combined gets B factor
                            blend.dstColor = mapAToSrc(); // dst(mem) gets A factor
                        } else if (mF == 1) {
                            // P=non-mem(src=combined), M=memColor(dst)
                            // N64: out = combined*A + mem*B
                            // GL:  out = combined*srcFactor + dst*dstFactor
                            blend.srcColor = mapAToSrc(); // combined gets A factor
                            blend.dstColor = mapBToDst(); // dst(mem) gets B factor
                        } else {
                            // Neither is memColor � shouldn't reach here
                            blend.srcColor = BlendFactor::One;
                            blend.dstColor = BlendFactor::Zero;
                        }

                        blend.srcAlpha = blend.srcColor;
                        blend.dstAlpha = blend.dstColor;
                    }
                } // end else (!pF==1 bypass)
            }

            // NOTE: The old "Texture-off sanity check" (blastCorpsBlendFix) was
            // removed.  It tested bit 0x40000 of geometryMode, which is
            // G_TEXTURE_GEN in F3DEX2 � NOT G_TEXTURE_ENABLE.  In F3DEX2,
            // texture enable is controlled by gSPTexture, not geometryMode.
            // The check incorrectly made all textured triangles without
            // environment mapping invisible (blend src=0, dst=1).
        }
        m_renderer->SetBlendState(blend);

        // --- Set depth state ---
        DepthStencilState depth;
        depth.depthTestEnabled  = (features & Shader::SF_DEPTH_TEST) != 0;
        depth.depthWriteEnabled = (features & Shader::SF_DEPTH_WRITE) != 0;

        // When Z_CMP is off but depth is still enabled (via G_ZBUFFER or prim Z),
        // set depthFunc = ALWAYS so all pixels pass while depth writes still work.
        // This matches GLideN64's behavior for write-only depth initialization.
        bool zCmpOn = (call.otherModeL & 0x0010) != 0;

        // Derive depth func from N64 zMode (otherModeL bits 10-11)
        uint32_t zMode = (call.otherModeL >> 10) & 0x03;
        if (features & Shader::SF_N64_DEPTH_COMPARE) {
            // When the N64 Z-image compare is active, the fragment shader
            // handles ALL depth testing via imageLoad/imageStore on the
            // Z-image.  Disable GL depth test and GL depth writes entirely
            // to prevent the GL depth buffer from being polluted with
            // rasterizer-interpolated linear Z values that would corrupt
            // subsequent non-N64-depth draws.  Matches GLideN64.
            depth.depthTestEnabled  = false;
            depth.depthWriteEnabled = false;
        } else if (!zCmpOn) {
            depth.depthFunc = CompareFunc::Always;
        } else {
            // Without N64 Z-image compare, GL depth func is our only
            // depth test.  Match GLideN64: OPA/XLU use LESS unless
            // depth source is PRIM with z=1.0 (Max mode → LEQUAL),
            // INTER/DEC always use LEQUAL.
            bool depthSourcePrim = (call.otherModeL & 0x0004) != 0;
            bool primDepthMax = depthSourcePrim && (call.primDepth >= 0.999f);
            switch (zMode) {
                case 0: // OPAQUE
                case 2: // TRANSPARENT
                    depth.depthFunc = primDepthMax ? CompareFunc::LessEqual : CompareFunc::Less;
                    break;
                case 1: // INTERPENETRATING
                    depth.depthFunc = CompareFunc::LessEqual;
                    break;
                case 3: // DECAL
                default:
                    depth.depthFunc = CompareFunc::LessEqual;
                    break;
            }
        }
        // Coverage destination bits (otherModeL [9:8]) refine depth write behaviour:
        //   CVG_DST_SAVE (3): preserve existing depth regardless of Z_UPD flag.
        //   ZMODE_TRANSPARENT (2) without FORCE_BL: game controls draw order via sorting;
        //   depth buffer writes would produce incorrect overdraw for alpha surfaces.
        {
            uint32_t cvgDst = (call.otherModeL >> 8) & 0x03;
            if (cvgDst == 3) {
                depth.depthWriteEnabled = false;
            }
            bool forceBlend = ((call.otherModeL >> 14) & 1) != 0;
            if (zMode == 2 && !forceBlend) {
                depth.depthWriteEnabled = false;
            }
        }

        // --- 1.2 Override Depth for Rectangles ---
        // N64 rectangles have no vertex Z; they only participate in depth
        // when Z_SOURCE_SEL=PRIM (otherModeL bit 2) explicitly routes
        // primDepth to the shader's gl_FragDepth.  Without that, rects
        // at z=0 interact incorrectly with the depth buffer.
        // COPY/FILL modes never do depth (already handled by feature flags
        // via cycType <= 1 check), but guard here too for safety.
        if (call.type == DrawCall::Type::TextureRect || call.type == DrawCall::Type::FillRect) {
            bool zPrim = (call.otherModeL & 0x0004) != 0;
            uint32_t cycType = (call.otherModeH >> 20) & 0x03;
            if (!zPrim || cycType >= 2) {
                depth.depthTestEnabled = false;
                depth.depthWriteEnabled = false;
            } else {
                // zPrim is set and cycle type is 1- or 2-cycle:
                // enable depth so the rect participates in the depth buffer.
                depth.depthTestEnabled = true;
                depth.depthWriteEnabled = (call.otherModeL & 0x0020) != 0; // Z_UPD
            }
        }

        // --- 1.3 Depth render override ---
        // When the shader outputs color.r as depth, enable depth test (ALWAYS)
        // and depth write so gl_FragDepth actually hits the GL depth buffer.
        if (features & Shader::SF_DEPTH_RENDER) {
            depth.depthTestEnabled  = true;
            depth.depthWriteEnabled = true;
            depth.depthFunc = CompareFunc::Always;
            // If the game also has Z_CMP set (mode 2), the shader handles the
            // compare via Z-image imageLoad; GL depth func stays ALWAYS.
        }

        // Polygon offset: match GLideN64 — only DECAL mode uses polygon
        // offset to lift decals above the base surface.  OPA/INTER/XLU
        // should NOT use polygon offset (it causes z-fighting and ground
        // plane instability).
        switch (zMode) {
            case 3: // DEC
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-3.0f, -3.0f);
                break;
            default: // OPA, INTER, XLU
                glDisable(GL_POLYGON_OFFSET_FILL);
                break;
        }
        // Debug: force-disable depth test/write
        if (m_debugDisableDepth) {
            depth.depthTestEnabled  = false;
            depth.depthWriteEnabled = false;
        }
        m_renderer->SetDepthStencilState(depth);

        // ��� N64 Z-image depth compare: bind Z-image textures + set mode ����
        if (features & Shader::SF_N64_DEPTH_COMPARE) {
            auto* depthBuf = g_depthManager.GetCurrent();
            if (depthBuf) {
                g_depthManager.InitDepthImageTextures(*depthBuf, g_fbManager.GetResolutionScale());
                g_depthManager.BindDepthImageTextures(*depthBuf, 8, 9);
                // Normalized approach: primDeltaZ converted to [0,1] range
                m_shaderManager->SetN64DepthMode(
                    static_cast<int>(zMode),
                    static_cast<float>(call.primDeltaZ & 0x7FFF) / 32768.0f
                );
            }
        }

        // ��� MLE blender: bind color/coverage images and disable color writes ����
        if (features & Shader::SF_MLE_BLENDER) {
            // Ensure prior FBO writes (e.g. fill rects) are visible to imageLoad.
            m_renderer->ImageMemoryBarrier();
            GPUFramebuffer* fb = g_fbManager.GetCurrentColor();
            if (fb && fb->coverageTex) {
                m_renderer->BindImageForRW(2, fb->colorImageHandle, ImageBindFormat::RGBA8);
                m_renderer->BindImageForRW(3, fb->coverageImageHandle, ImageBindFormat::R8UI);
                // Disable fragment output writes � only imageStore updates
                // the color buffer.  This avoids undefined aliasing between
                // the FBO attachment and the image binding.
                m_renderer->SetColorWriteMask(false, false, false, false);
            }
        }

        // ��� CVG_DST coverage accumulator �����������������������
        // The N64 uses the 3 coverage bits per pixel for anti-aliasing blending.
        // We now accurately compute CVG_DEST logic (WRAP, CLAMP, SAVE, ZAP) in the
        // pixel shader (GenerateBlenderCode) to perfectly emulate N64 coverage wrap,
        // rather than relying on flawed OpenGL fixed-function stencil operations.
        // For WRAP, it uses SF_FRAMEBUFFER_READ to precisely evaluate (cvg+memCvg)&7.

        // --- Set scissor (scale N64 coords to GL window coords, flip Y) ---
        {
            int n64sx = static_cast<int>(m_state.rdp.scissor.xh >> 2);
            int n64sy = static_cast<int>(m_state.rdp.scissor.yh >> 2);
            int n64sw = static_cast<int>((m_state.rdp.scissor.xl >> 2) - n64sx);
            int n64sh = static_cast<int>((m_state.rdp.scissor.yl >> 2) - n64sy);

            // Widescreen: expand scissor Y to full height for all draws.
            // This lets both 3D content and fullscreen effects (fades,
            // transitions) fill the expanded viewport area. Letterbox bar
            // skipping and inward scaling are handled in Handle*Rect().
            float wsScale = m_state.rdp.geometry.widescreenScale;
            if (wsScale != 1.0f) {
                n64sy = 0;
                n64sh = static_cast<int>(n64H);
            }

            // Scissor interlace mode: mode 2 = even lines only, mode 3 = odd lines only.
            // On real N64, this restricts rendering to alternating scanlines for
            // interlaced displays.  We halve the scissor height and shift Y by field.
            uint8_t scissorMode = m_state.rdp.scissor.mode;
            if (scissorMode >= 2) {
                // Even (mode 2) or odd (mode 3) field
                int field = (scissorMode == 3) ? 1 : 0;
                n64sy = n64sy / 2 + field;
                n64sh = (n64sh + 1) / 2;
            }

            float scaleX = fboW / n64W;
            float scaleY = fboH / n64H;

            RenderScissorRect scissor;
            scissor.x      = static_cast<int32_t>(n64sx * scaleX);
            scissor.y      = static_cast<int32_t>(fboH - (n64sy + n64sh) * scaleY); // GL Y=0 at bottom
            scissor.width  = static_cast<int32_t>(n64sw * scaleX);
            scissor.height = static_cast<int32_t>(n64sh * scaleY);

            // Clamp scissor to FBO bounds to prevent writes past the
            // framebuffer attachment edge (avoids garbage strip artifacts).
            int32_t fboWi = static_cast<int32_t>(fboW);
            int32_t fboHi = static_cast<int32_t>(fboH);
            if (scissor.x < 0) { scissor.width += scissor.x; scissor.x = 0; }
            if (scissor.y < 0) { scissor.height += scissor.y; scissor.y = 0; }
            if (scissor.x + scissor.width  > fboWi) scissor.width  = fboWi - scissor.x;
            if (scissor.y + scissor.height > fboHi) scissor.height = fboHi - scissor.y;

            if (scissor.width > 0 && scissor.height > 0) {
                glEnable(GL_SCISSOR_TEST);
                m_renderer->SetScissor(scissor);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
        }

        // --- Shadow map: bind depth texture & TLUT, override blend ----
        if (call.isShadowMap) {
            // Bind the current FBO's depth texture on unit 4 (uDepthTexture)
            GPUFramebuffer* curFB_shadow = g_fbManager.GetCurrentColor();
            if (curFB_shadow && curFB_shadow->depthTex) {
                glActiveTexture(GL_TEXTURE4);
                glBindTexture(GL_TEXTURE_2D, curFB_shadow->depthTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            }
            // Create or update the 256�1 R8 shadow TLUT texture on unit 2 (uTLUT0)
            if (!m_shadowTLUTTexture) {
                glCreateTextures(GL_TEXTURE_2D, 1, &m_shadowTLUTTexture);
                glTextureStorage2D(m_shadowTLUTTexture, 1, GL_R8, 256, 1);
                glTextureParameteri(m_shadowTLUTTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(m_shadowTLUTTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTextureParameteri(m_shadowTLUTTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            }
            glTextureSubImage2D(m_shadowTLUTTexture, 0, 0, 0, 256, 1,
                                GL_RED, GL_UNSIGNED_BYTE, call.shadowTLUT);
            glBindTextureUnit(2, m_shadowTLUTTexture);

            // Override blend for shadow: SRC_ALPHA / ONE_MINUS_SRC_ALPHA
            blend.enabled  = true;
            blend.srcColor = BlendFactor::SrcAlpha;
            blend.dstColor = BlendFactor::OneMinusSrcAlpha;
            blend.srcAlpha = BlendFactor::SrcAlpha;
            blend.dstAlpha = BlendFactor::OneMinusSrcAlpha;
            m_renderer->SetBlendState(blend);

            // Disable depth write/test for shadow overlay
            {
                DepthStencilState shadowDepth;
                shadowDepth.depthTestEnabled  = false;
                shadowDepth.depthWriteEnabled = false;
                m_renderer->SetDepthStencilState(shadowDepth);
            }

            // Draw the shadow rect
            m_renderer->DrawArrays(PrimitiveType::Triangles,
                                   static_cast<uint32_t>(batchBaseVertex + call.vertexStart),
                                   static_cast<uint32_t>(call.vertexCount));
            continue; // Skip normal texture/draw path
        }

        // --- Bind textures if available and set wrap modes ---
        float gpuScale0[2] = {1.0f, 1.0f};
        float gpuScale1[2] = {1.0f, 1.0f};
        int fbMono0 = 0, fbMono1 = 0, fbAlpha0 = 0, fbAlpha1 = 0;

        auto getGLWrapMode = [](const TileDescriptor& td, bool isS) -> GLenum {
            bool clamp  = isS ? td.IsClamped_S()  : td.IsClamped_T();
            bool mirror = isS ? td.IsMirrored_S() : td.IsMirrored_T();
            uint8_t mask = isS ? td.maskS : td.maskT;
            // When mask bits > 0 the shader handles all wrap/mirror/clamp
            // in texel space.  Use CLAMP_TO_EDGE in GL to avoid the
            // sampler re-applying mirror/repeat on the already-wrapped UVs.
            if (mask > 0) return GL_CLAMP_TO_EDGE;
            if (clamp)  return GL_CLAMP_TO_EDGE;
            if (mirror) return GL_MIRRORED_REPEAT;
            return GL_REPEAT;
        };

        if (m_textureCache && call.textureHash0 != 0) {
            uint8_t tlutType0 = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
            Tex::TextureKey key0 = BuildTextureKey(call.tileDesc0, call.textureImageAddr, tlutType0,
                                                    call.mipLevelCount > 0 ? call.mipTiles : nullptr,
                                                    call.mipLevelCount);
            if (key0.width > 0 && key0.height > 0) {
                GPUFramebuffer* rtFB0 = g_fbManager.FindByAddress(key0.address);

                // Reject tile-offset matches: only accept exact base address.
                // FindByAddress's second-pass range matching causes false
                // positives when regular textures are allocated near FB
                // addresses, binding the live framebuffer (wrong UV scale,
                // changing every frame → flickering textures).
                if (rtFB0 && rtFB0->desc.address != (key0.address & 0x7FFFFF))
                    rtFB0 = nullptr;

                // ── CfB copy-back detection ──────────────────────────────
                // Some N64 games (OoT, MM) copy an AA-processed framebuffer
                // (CfB) back to the display via RGBA32 TextureRect strips.
                // The CfB was never used as an RDP color image, so no GPU
                // framebuffer exists for it.  The TMEM data is really
                // RGBA5551 pixels loaded as RGBA32 → decode produces garbage.
                //
                // Only skip when the texture address is within the RDRAM
                // range of a known framebuffer (CfB is typically allocated
                // adjacent to the main framebuffers).  Textures from other
                // RDRAM regions (e.g. ROM-loaded artwork) are legitimate.
                //
                // IMPORTANT: Only skip TextureRect draws.  Triangle draws
                // with RGBA32 textures (e.g. Zelda logo shield/sword) are
                // legitimate game geometry and must NOT be skipped.
                if (!rtFB0 &&
                    call.type == DrawCall::Type::TextureRect &&
                    key0.format == Tex::N64TexFormat::RGBA &&
                    key0.size == Tex::N64TexSize::Bits32)
                {
                    uint32_t texAddr = key0.address & 0x7FFFFF;
                    bool nearFB = false;
                    for (uint32_t fi = 0; fi < g_fbManager.GetFBCount(); ++fi) {
                        auto* fb = g_fbManager.GetFB(fi);
                        if (!fb || fb->desc.type != FBType::Color) continue;
                        uint32_t fbStart = fb->desc.address;
                        uint32_t fbSize  = fb->desc.ByteSize();
                        // CfB is typically within 2x framebuffer size of a
                        // known FB address (one extra buffer worth of room).
                        if (texAddr >= fbStart && texAddr < fbStart + fbSize * 3) {
                            nearFB = true;
                            break;
                        }
                    }
                    if (nearFB) continue;
                }

                uint32_t fbTex0 = rtFB0 ? g_fbManager.GetSampleableTexture(
                    rtFB0, rtFB0 == g_fbManager.GetCurrentColor()) : 0;
                if (fbTex0 != 0) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, fbTex0);
                    // Set UV scale based on FB dimensions so texcoords map correctly
                    gpuScale0[0] = 1.0f / static_cast<float>(rtFB0->width);
                    gpuScale0[1] = 1.0f / static_cast<float>(rtFB0->height);
                    // FB-as-texture monochrome detection (matches GLideN64 UFrameBufferInfo):
                    //  size==1 (8-bit): mono mode 1 (red channel), fixedAlpha if IM_RD off
                    //  tile IA16: mono mode 2 (weighted luminance)
                    if (rtFB0->desc.size == 1) {
                        fbMono0 = 1;
                        if ((call.otherModeL & 0x40) == 0) fbAlpha0 = 1; // IM_RD off
                    } else if (call.tileDesc0.size == 2 && call.tileDesc0.format == 3) {
                        fbMono0 = 2; // IA16 luminance
                    }
                } else {
                    // Build mip tile info for tile-chain mipmapping
                    Tex::TextureCache::MipTileInfo mipInfo[7];
                    int mipCount = 0;
                    if (call.mipLevelCount > 0) {
                        mipCount = call.mipLevelCount;
                        for (int mi = 0; mi < mipCount; mi++) {
                            const auto& mt = call.mipTiles[mi];
                            mipInfo[mi].tmemAddr = mt.tmemAddr;
                            mipInfo[mi].line     = mt.line;
                            mipInfo[mi].width    = static_cast<uint16_t>(std::max(1, mt.GetWidth()));
                            mipInfo[mi].height   = static_cast<uint16_t>(std::max(1, mt.GetHeight()));
                            mipInfo[mi].format   = mt.format;
                            mipInfo[mi].size     = mt.size;
                            mipInfo[mi].palette  = mt.palette;
                        }
                    }
                    Tex::CachedTexture* cached0 = (mipCount > 0)
                        ? m_textureCache->GetTexture(key0, m_rdram, m_state.rdp.tmem.data, mipInfo, mipCount)
                        : m_textureCache->GetTexture(key0, m_rdram, m_state.rdp.tmem.data);
                    if (cached0 && cached0->gpuTexture) {
                        m_renderer->BindTexture(0, cached0->gpuTexture.get());
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getGLWrapMode(call.tileDesc0, true));
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getGLWrapMode(call.tileDesc0, false));
                        // Force correct per-draw filter mode from otherModeH
                        // texFilter 0/1=point, 2=3-point (shader samples texels manually), 3=bilinear
                        GLenum glFilter = (texFilter == 3) ? GL_LINEAR : GL_NEAREST;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter);
                        gpuScale0[0] = cached0->scaleS;
                        gpuScale0[1] = cached0->scaleT;
                    } else if (m_debugWhiteTexture) {
                        // Fallback: bind 1×1 white texture so the draw isn't invisible
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, m_debugWhiteTexture);
                        gpuScale0[0] = 1.0f;
                        gpuScale0[1] = 1.0f;
                    }

                }
            }
        }
        if (m_textureCache && call.textureHash1 != 0) {
            uint8_t tlutType1 = static_cast<uint8_t>((call.otherModeH >> 14) & 0x3);
            Tex::TextureKey key1 = BuildTextureKey(call.tileDesc1, call.textureImageAddr1, tlutType1);
            if (key1.width > 0 && key1.height > 0) {
                GPUFramebuffer* rtFB1 = g_fbManager.FindByAddress(key1.address);
                // Reject tile-offset matches (same as tex0 — prevent flickering)
                if (rtFB1 && rtFB1->desc.address != (key1.address & 0x7FFFFF))
                    rtFB1 = nullptr;
                uint32_t fbTex1 = rtFB1 ? g_fbManager.GetSampleableTexture(
                    rtFB1, rtFB1 == g_fbManager.GetCurrentColor()) : 0;
                if (fbTex1 != 0) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, fbTex1);
                    if (rtFB1->desc.size == 1) {
                        fbMono1 = 1;
                        if ((call.otherModeL & 0x40) == 0) fbAlpha1 = 1;
                    } else if (call.tileDesc1.size == 2 && call.tileDesc1.format == 3) {
                        fbMono1 = 2;
                    }
                } else {
                    Tex::CachedTexture* cached1 = m_textureCache->GetTexture(
                        key1, m_rdram, m_state.rdp.tmem.data);
                    if (cached1 && cached1->gpuTexture) {
                        m_renderer->BindTexture(1, cached1->gpuTexture.get());
                        // Texture is already bound on unit 1 by BindTexture above
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getGLWrapMode(call.tileDesc1, true));
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getGLWrapMode(call.tileDesc1, false));
                        GLenum glFilter1 = (texFilter == 3) ? GL_LINEAR : GL_NEAREST;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter1);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter1);
                        gpuScale1[0] = cached1->scaleS;
                        gpuScale1[1] = cached1->scaleT;
                    } else if (m_debugWhiteTexture) {
                        // Fallback: bind 1×1 white texture for stale texture unit
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, m_debugWhiteTexture);
                        gpuScale1[0] = 1.0f;
                        gpuScale1[1] = 1.0f;
                    }
                }
            }
        }
        m_shaderManager->SetGPUScale(gpuScale0, gpuScale1);
        m_shaderManager->SetFBTextureParams(fbMono0, fbMono1, fbAlpha0, fbAlpha1);

        // --- Bind noise (unit 5) and dither (unit 6) textures ---
        if (m_noiseTexture) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, m_noiseTexture);
        }
        {
            // Select dither texture based on N64 rgb_dither_sel (otherModeH bits 6-7)
            // 0 = Magic Square (default), 1 = Bayer, 2 = noise (uses noise tex), 3 = none
            uint32_t rgbDitherMode = (call.otherModeH >> 6) & 0x03;
            uint32_t ditherTex = (rgbDitherMode == 1) ? m_ditherBayerTexture : m_ditherTexture;
            if (rgbDitherMode == 2) ditherTex = m_noiseTexture; // noise dither reuses noise
            if (ditherTex) {
                glActiveTexture(GL_TEXTURE6);
                glBindTexture(GL_TEXTURE_2D, ditherTex);
            }
        }

        // --- Framebuffer read: snapshot current FB and bind to unit 7 ---
        if (features & Shader::SF_FRAMEBUFFER_READ) {
            GPUFramebuffer* fbForRead = g_fbManager.GetCurrentColor();
            if (fbForRead) {
                uint32_t fbTex = g_fbManager.GetSampleableTexture(fbForRead, true);
                if (fbTex != 0) {
                    glActiveTexture(GL_TEXTURE7);
                    glBindTexture(GL_TEXTURE_2D, fbTex);
                }
            }
            // When dual-source is NOT active, the shader blender computes
            // the full blend formula (including memColor from FB texture),
            // so disable GL hardware blend to avoid double-blending.
            // When dual-source IS active, we need GL blend for compositing.
            if (!(features & Shader::SF_DUAL_SOURCE_BLEND)) {
                blend.enabled = false;
                m_renderer->SetBlendState(blend);
            }
        }

        // --- Trivial rejection bypass for testing ---
        if (false && call.type == DrawCall::Type::Triangle && call.vertexCount >= 3) {
            bool allOutsideSamePlane = false;
            
            // Check if all 3 vertices share a common "outside" clip flag
            if (call.vertexCount == 3) {
                const auto& v0 = m_batch.vertices[call.vertexStart + 0];
                const auto& v1 = m_batch.vertices[call.vertexStart + 1];
                const auto& v2 = m_batch.vertices[call.vertexStart + 2];
                
                // If all 3 verts are outside the same plane, reject the triangle
                // (Their common AND flags indicate shared outside status)
                uint32_t commonClipFlags = v0.clipFlags & v1.clipFlags & v2.clipFlags;
                allOutsideSamePlane = (commonClipFlags != 0);
            }
            
            if (allOutsideSamePlane) {
                // Skip this triangle entirely � all vertices are culled
                m_stats.trianglesDrawn++;  // Count as drawn (for statistics)
                continue;  // Move to next draw call
            }
        }

        // --- Draw ---
        PrimitiveType prim = PrimitiveType::Triangles;
        if (call.type == DrawCall::Type::TextureRect ||
            call.type == DrawCall::Type::FillRect) {
            prim = PrimitiveType::Triangles; // Rects decomposed into 2 triangles
        }

        // === DEBUG: Skip depth-tested triangles (model draws) =============
        if constexpr (DEBUG_SKIP_DEPTH_TRIS) {
            if (call.type == DrawCall::Type::Triangle && (features & Shader::SF_DEPTH_TEST)) {
                continue;
            }
        }


        m_renderer->DrawArrays(prim,
                               static_cast<uint32_t>(batchBaseVertex + call.vertexStart),
                               static_cast<uint32_t>(call.vertexCount));

        // Restore color writes after MLE draw (imageStore already updated
        // the color buffer; the mask was set to avoid aliasing UB).
        if (features & Shader::SF_MLE_BLENDER) {
            m_renderer->SetColorWriteMask(true, true, true, true);
            m_renderer->ImageMemoryBarrier();
        } else if (features & Shader::SF_N64_DEPTH_COMPARE) {
            // Track that this batch wrote to the Z-image; the barrier is
            // deferred to the end of the batch so that overlapping terrain
            // draws within the same batch all see the pre-batch Z-image
            // state (typically the cleared value).
            batchUsedN64Depth = true;
        }

    }

    // Flush any pending Z-image writes so subsequent batches (e.g. sprites)
    // can see the terrain depth via imageLoad.
    if (batchUsedN64Depth) {
        m_renderer->ImageMemoryBarrier();
    }

    m_stats.batchesFlushed++;
    m_stats.frameDrawCalls += static_cast<uint32_t>(m_batch.drawCalls.size());

    // Snapshot the FBO for presentation whenever scene content was rendered.
    // This must happen here (not in HandleSyncFull/HandleSetColorImage) because
    // other commands like SetScissor can trigger FlushBatch before those handlers
    // run, leaving them with an empty batch and no snapshot.
    {
        bool hasSceneDraws = false;
        for (const auto& c : m_batch.drawCalls) {
            if (c.type == DrawCall::Type::Triangle || c.type == DrawCall::Type::TextureRect) {
                hasSceneDraws = true;
                break;
            }
            // Include non-FILL-mode FillRects (e.g., blended fade overlays drawn in
            // 1-cycle or 2-cycle mode).  Pure FILL mode (cycleType=3) rects are
            // simple screen clears and don't need a snapshot.
            if (c.type == DrawCall::Type::FillRect) {
                uint32_t cycType = (c.otherModeH >> 20) & 0x03;
                if (cycType != 3) {
                    hasSceneDraws = true;
                    break;
                }
            }
        }
        if (hasSceneDraws)
            g_fbManager.SnapshotForPresentation();
    }


    m_batch.Reset();
}

bool RDPProcessor::NeedsBatchFlush(const DrawCall& newCall) const {
    if (m_batch.drawCalls.empty()) return false;
    
    const auto& lastCall = m_batch.drawCalls.back();
    
    // Different render mode requires flush
    if (lastCall.combineMode != newCall.combineMode) return true;
    if (lastCall.otherModeH != newCall.otherModeH) return true;
    if (lastCall.otherModeL != newCall.otherModeL) return true;
    if (lastCall.type != newCall.type) return true;
    
    return false;
}

void RDPProcessor::OnFrameEnd() {
    FlushBatch();

    m_state.frameCount++;
    m_stats.ResetFrame();

    // Re-randomize noise texture each frame (N64 generates new random per pixel per frame)
    if (m_noiseTexture) {
        constexpr int NOISE_SIZE = 64;
        std::vector<uint8_t> noiseData(NOISE_SIZE * NOISE_SIZE);
        // Use frame count as seed for distinct per-frame noise
        std::mt19937 rng(static_cast<uint32_t>(m_state.frameCount) * 2654435761u);
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& p : noiseData) p = static_cast<uint8_t>(dist(rng));
        glTextureSubImage2D(m_noiseTexture, 0, 0, 0, NOISE_SIZE, NOISE_SIZE,
                            GL_RED, GL_UNSIGNED_BYTE, noiseData.data());
    }

    // Advance depth buffer manager frame counter / eviction
    g_depthManager.OnFrameEnd();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Frame Buffer Access
// ═══════════════════════════════════════════════════════════════════════════

void RDPProcessor::GetFrameBufferInfo(FrameBufferInfo* info, int maxBuffers) const {
    if (maxBuffers < 1 || !info) return;
    
    info[0].address = m_state.rdp.colorImage.address;
    info[0].width   = m_state.rdp.colorImage.width;
    info[0].height  = static_cast<uint32_t>(m_state.rdp.scissor.yl >> 2);
    info[0].size    = static_cast<uint32_t>(m_state.rdp.colorImage.size);
}

bool RDPProcessor::ReadFrameBuffer(uint32_t address, uint32_t size, void* data) {
    if (!m_rdram || !data) return false;
    if (address + size > m_rdramSize) return false;
    
    std::memcpy(data, m_rdram + address, size);
    return true;
}

bool RDPProcessor::WriteFrameBuffer(uint32_t address, uint32_t size, const void* data) {
    if (!m_rdram || !data) return false;
    if (address + size > m_rdramSize) return false;
    
    std::memcpy(m_rdram + address, data, size);
    return true;
}

} // namespace Nova64::Video






