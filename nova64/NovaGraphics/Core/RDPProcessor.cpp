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

#include <glad/gl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <random>
#include <unordered_set>

namespace Nova64::Video {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Construction / Initialization
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
    m_state.Reset();
    m_batch.Reset();
    m_stats.Reset();
    
    // ── Create noise texture (64x64 random R8, bound to unit 5) ──
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

    // ── Create N64 dither textures (4x4 R8, bound to unit 6) ──
    // N64 has two ordered dither patterns selectable via otherModeH rgb_dither_sel:
    //   Mode 0: Magic Square (N64 default) — values 0-7 (3 bits)
    //   Mode 1: Standard Bayer             — values 0-7 (3 bits)
    //   Mode 2: Noise dither (uses noise texture)
    //   Mode 3: Disabled
    // We store both patterns in separate textures; the active one is bound at draw time.
    {
        // N64 Magic Square dither matrix (normalized 0-7 → 0-255)
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

        // N64 Bayer dither matrix (normalized 0-7 → 0-255)
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

    m_state.Reset();
    m_commandBuffer.Reset();
    m_initialized = false;
    
    NOVA_LOG(LogLevel::Info, LogSubsystem::Core,
             "RDP Processor shutdown (commands: {}, triangles: {}, rects: {})",
             m_stats.commandsProcessed, m_stats.trianglesDrawn, m_stats.rectsDrawn);
}

void RDPProcessor::Reset(bool coldReset) {
    FlushBatch();
    m_state.Reset();
    m_commandBuffer.Reset();
    m_batch.Reset();
    
    if (coldReset) {
        m_stats.Reset();
    } else {
        m_stats.ResetFrame();
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Display List Processing
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
                
            // â”€â”€ Triangles â”€â”€
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
                int triSize = 8; // Edge coefficients: 4 × 64-bit words = 8 × 32-bit words
                if (shade)   triSize += 16; // Shade: 8 × 64-bit = 16 × 32-bit
                if (texture) triSize += 16; // Texture: 8 × 64-bit = 16 × 32-bit
                if (zbuffer) triSize += 4;  // Z: 2 × 64-bit = 4 × 32-bit
                
                if (remaining >= triSize) {
                    HandleTriangle(current, triSize);
                }
                offset += triSize;
                break;
            }
                
            // â”€â”€ Rectangles â”€â”€
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
                
            // â”€â”€ Syncs â”€â”€
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
                
            // â”€â”€ State Setting â”€â”€
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
                
            // â”€â”€ Texture Loads â”€â”€
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
                
            default:
                // Unknown command — skip 2 words
                NOVA_LOG(LogLevel::Debug, LogSubsystem::RDP,
                         "Unknown RDP command: 0x{:02X}", static_cast<int>(cmdID));
                offset += 2;
                break;
        }
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Command Handlers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
    m_decoder.DecodeSetScissor(words, m_state.rdp.scissor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Scissor);
    m_stats.stateChanges++;
    // Keep FramebufferManager in sync so new FBOs get the right height
    uint32_t scissorW = m_state.rdp.scissor.xl >> 2;
    uint32_t scissorH = m_state.rdp.scissor.yl >> 2;
    g_fbManager.SetScissorHint(scissorW, scissorH);
}

void RDPProcessor::HandleSetColorImage(const uint32_t* words) {
    // Count scene-bearing draws before the flush so we know whether
    // to snapshot the FBO for presentation (avoids the end-of-DL
    // fill-rect clear from destroying the completed frame).
    int sceneDraws = 0;
    for (const auto& c : m_batch.drawCalls) {
        if (c.type == DrawCall::Type::Triangle || c.type == DrawCall::Type::TextureRect)
            ++sceneDraws;
    }

    FlushBatch(); // Changing render target requires flush

    // Snapshot the just-rendered scene so the post-scene fill-rect clear
    // doesn't wipe it before BlitToScreen can present it.
    if (sceneDraws >= 1) {
        g_fbManager.SnapshotForPresentation();
    }

    m_decoder.DecodeSetColorImage(words, m_state.rdp.colorImage);
    m_state.rdp.colorImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.colorImage.address);
    m_state.rdp.MarkDirty(RDPDirtyFlag::ColorImage);
    m_stats.stateChanges++;

    g_fbManager.OnSetColorImage(m_state.rdp.colorImage.address,
                                m_state.rdp.colorImage.width,
                                static_cast<uint32_t>(m_state.rdp.colorImage.format),
                                static_cast<uint32_t>(m_state.rdp.colorImage.size));
    // Evict any stale TextureCache entries at this address — it is now a GPU render target
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
    auto* depthBuf = g_depthManager.GetCurrent();
    if (depthBuf) {
        g_depthManager.InitDepthImageTextures(*depthBuf);
    }
}

void RDPProcessor::HandleSetTextureImage(const uint32_t* words) {
    m_decoder.DecodeSetTextureImage(words, m_state.rdp.textureImage);
    m_state.rdp.textureImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.textureImage.address);
    m_state.rdp.MarkDirty(RDPDirtyFlag::TextureImage);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetFillColor(const uint32_t* words) {
    // Decode fill color based on current color image pixel size.
    // For 16-bit modes (OoT), the 32-bit word packs two RGBA5551 pixels;
    // FromCommand() wrongly treats it as RGBA8888.
    uint32_t raw = words[1];
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Texture Load Handlers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void RDPProcessor::HandleLoadBlock(const uint32_t* words) {
    // Flush pending draws before overwriting TMEM — batched draws need
    // the TMEM content that was present when they were queued.
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile, uls, ult, texels, dxt;
    m_decoder.DecodeLoadBlock(words, tile, uls, ult, texels, dxt);
    ExecuteLoadBlock(tile, uls, ult, texels, dxt);
}

void RDPProcessor::HandleLoadTile(const uint32_t* words) {
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile;
    float sl, tl, sh, th;
    m_decoder.DecodeLoadTile(words, tile, sl, tl, sh, th);
    ExecuteLoadTile(tile, sl, tl, sh, th);
}

void RDPProcessor::HandleLoadTLUT(const uint32_t* words) {
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile, count;
    m_decoder.DecodeLoadTLUT(words, tile, count);
    ExecuteLoadTLUT(tile, count);
}

void RDPProcessor::ExecuteLoadBlock(int tile, int uls, int /*ult*/,
                                     int texels, int dxt) {
    if (!m_rdram) return;
    
    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];
    
    int bpp = GetBitsPerPixel(texImg.size);
    uint32_t rawAddr = texImg.address + (uls * bpp) / 8;
    uint32_t srcAddr = rawAddr & (m_rdramSize - 1);  // Wrap to physical RDRAM (hardware mirrors)
    uint16_t tmemAddr = tileDesc.tmemAddr * 8; // Convert word address to byte address
    
    // Bit-based size calculation — handles 4bpp correctly (e.g. 1024 texels × 4bpp = 512 bytes)
    int sizeBytes = ((texels + 1) * bpp + 7) / 8;
    
    // Clamp to TMEM size
    if (tmemAddr + sizeBytes > TMEM_SIZE) {
        sizeBytes = TMEM_SIZE - tmemAddr;
    }
    
    // Copy from RDRAM to TMEM (word-swapped RDRAM)
    if (srcAddr + sizeBytes <= m_rdramSize && sizeBytes > 0) {
        for (int i = 0; i < sizeBytes; i++) {
            m_state.rdp.tmem.data[tmemAddr + i] = m_rdram[(srcAddr + i) ^ 3];
        }
        
        // NOTE: Real N64 hardware swaps 32-bit halves of 64-bit TMEM words on
        // odd rows during LoadBlock (DXT interleave), then compensates via
        // interleaved bank addressing during texture reads.  Our TMEM is a flat
        // byte array and our decoders read sequentially, so both operations cancel
        // out — we skip the swap entirely.
        
        // Compute CRC for texture cache
        uint32_t crc = CRC32_Fast(m_state.rdp.tmem.data + tmemAddr, sizeBytes);
        m_state.rdp.tmem.RecordLoad(srcAddr, tileDesc.tmemAddr, static_cast<uint16_t>(sizeBytes), crc);
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

void RDPProcessor::ExecuteLoadTile(int tile, float sl, float tl, float sh, float th) {
    if (!m_rdram) return;
    
    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];
    
    int width = static_cast<int>(sh - sl + 1);
    int height = static_cast<int>(th - tl + 1);
    
    if (width <= 0 || height <= 0) return;
    
    int bpp = GetBitsPerPixel(texImg.size);
    int srcLineSize = (texImg.width * bpp + 7) / 8;
    int dstLineSize = tileDesc.line * 8; // TMEM line words to bytes
    
    if (dstLineSize == 0) dstLineSize = width * bpp / 8;
    
    uint32_t srcAddr = texImg.address + static_cast<uint32_t>(tl) * srcLineSize + 
                       static_cast<uint32_t>(sl) * bpp / 8;
    srcAddr &= (m_rdramSize - 1);  // Wrap to physical RDRAM (hardware mirrors)
    uint16_t tmemAddr = tileDesc.tmemAddr * 8;
    
    int totalBytes = 0;
    
    for (int row = 0; row < height; row++) {
        uint32_t rowSrcAddr = srcAddr + row * srcLineSize;
        int tmemOff = tmemAddr + row * dstLineSize;
        int rowBytes = width * bpp / 8;
        
        if (tmemOff + rowBytes > TMEM_SIZE) {
            rowBytes = TMEM_SIZE - tmemOff;
            if (rowBytes <= 0) break;
        }
        
        if (rowSrcAddr + rowBytes <= m_rdramSize) {
            for (int b = 0; b < rowBytes; b++) {
                m_state.rdp.tmem.data[tmemOff + b] = m_rdram[(rowSrcAddr + b) ^ 3];
            }
            totalBytes += rowBytes;
        }
    }
    
    // CRC the loaded data
    if (totalBytes > 0 && tmemAddr + totalBytes <= TMEM_SIZE) {
        uint32_t crc = CRC32_Fast(m_state.rdp.tmem.data + tmemAddr, totalBytes);
        m_state.rdp.tmem.RecordLoad(srcAddr, tileDesc.tmemAddr,
                                     static_cast<uint16_t>(totalBytes), crc);
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

void RDPProcessor::ExecuteLoadTLUT(int tile, int count) {
    if (!m_rdram) return;
    
    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];
    
    uint32_t srcAddr = texImg.address & (m_rdramSize - 1);  // Wrap to physical RDRAM
    
    // TLUT destination is determined by the tile's TMEM address
    uint16_t tmemAddr = tileDesc.tmemAddr * 8;
    int tmemSpaceNeeded = count * 4; // each entry occupies 4 bytes in TMEM (interleaved)
    int rdramBytes = count * 2;       // 16-bit palette entries in RDRAM
    
    if (tmemAddr + tmemSpaceNeeded > TMEM_SIZE) {
        // Clamp count to what fits in TMEM
        count = (TMEM_SIZE - tmemAddr) / 4;
        rdramBytes = count * 2;
    }
    
    if (srcAddr + rdramBytes <= m_rdramSize && rdramBytes > 0) {
        // TLUT entries are loaded every 4 bytes in TMEM (interleaved with padding)
        for (int i = 0; i < count && (tmemAddr + i * 4 + 1) < TMEM_SIZE; i++) {
            uint32_t addr = srcAddr + i * 2;
            if (addr + 1 < m_rdramSize) {
                m_state.rdp.tmem.data[tmemAddr + i * 4]     = m_rdram[addr ^ 3];
                m_state.rdp.tmem.data[tmemAddr + i * 4 + 1] = m_rdram[(addr + 1) ^ 3];
            }
        }
        
        uint32_t crc = CRC32_Fast(m_rdram + srcAddr, count * 2);
        m_state.rdp.tmem.RecordLoad(srcAddr, static_cast<uint16_t>(tmemAddr),
                                     static_cast<uint16_t>(count * 2), crc);
    }
    
    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Sync Handlers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void RDPProcessor::HandleSyncFull() {
    // Signal DP_FULL interrupt immediately — this is what the N64 CPU waits for.
    // The callback fires before FlushBatch so the CPU can overlap with rendering,
    // matching real hardware where the RDP issues the interrupt at sync time.
    if (m_syncFullCallback) m_syncFullCallback();

    // Count scene draws before flushing so we know whether to snapshot.
    int sceneDraws = 0;
    int triDraws = 0, rectDraws = 0, fillDraws = 0;
    for (const auto& c : m_batch.drawCalls) {
        if (c.type == DrawCall::Type::Triangle) { ++sceneDraws; ++triDraws; }
        else if (c.type == DrawCall::Type::TextureRect) { ++sceneDraws; ++rectDraws; }
        else if (c.type == DrawCall::Type::FillRect) { ++fillDraws; }
    }

    FlushBatch();

    // SyncFull is the N64’s explicit “rendering complete” signal.
    // Snapshot the just-rendered content so BlitToScreen can present it
    // even if a subsequent FillRect clear fires before the next VI interrupt.
    if (sceneDraws >= 1)
        g_fbManager.SnapshotForPresentation();

    m_stats.syncFullCount++;
    m_state.rdp.syncCount++;
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Primitive Handlers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
    }

    DrawTextureRect(rect);
}

void RDPProcessor::HandleTextureRectFlip(const uint32_t* words) {
    if (m_hacks.skipTexRect) return;  // WinBack: grey rectangle fix
    DecodedRectangle rect;
    m_decoder.DecodeTextureRectFlip(words, rect);

    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 2) {
        rect.dsDx /= 4.0f;
        rect.x1 += 1.0f;
    }

    DrawTextureRect(rect);
}

void RDPProcessor::HandleFillRect(const uint32_t* words) {
    float x0, y0, x1, y1;
    m_decoder.DecodeFillRect(words, x0, y0, x1, y1);

    // In FILL mode (cycleType=3), the N64 RDP uses inclusive right/bottom edges.
    // OpenGL rasterization uses exclusive right/bottom (top-left fill rule).
    // Add 1 pixel to compensate so the last column/row is filled.
    uint32_t cycleType = (m_state.rdp.otherModeH >> 20) & 0x03;
    if (cycleType == 3) {
        x1 += 1.0f;
        y1 += 1.0f;
    }

    DrawFillRect(x0, y0, x1, y1);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Drawing
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
    
    // Reconstruct 3 triangle vertices from RDP edge coefficients.
    // The RDP defines 3 edges:
    //   H edge: from yh to yl (major edge spanning full height)  start X = xh, slope = dxhDy
    //   M edge: from yh to ym (upper minor edge)                 start X = xm, slope = dxmDy
    //   L edge: from ym to yl (lower minor edge)                 start X = xl, slope = dxlDy
    // The 3 triangle corners are:
    //   Top:    (xh, yh) — where H and M edges both start
    //   Mid:    (xl, ym) — where M edge ends / L edge starts (the "knee")
    //   Bottom: (xh + dxhDy*(yl-yh), yl) — where H and L edges meet
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
    // Position W must remain 1.0 — the ortho matrix assumes pre-divided screen-space inputs.
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
    v0.fogFactor = v1.fogFactor = v2.fogFactor = 0.0f;
    
    // LLE triangles are in screen space — no clip flags needed
    v0.clipFlags = v1.clipFlags = v2.clipFlags = 0;
    
    m_batch.vertices.push_back(v0);
    m_batch.vertices.push_back(v1);
    m_batch.vertices.push_back(v2);
    call.vertexCount = 3;
    
    m_batch.drawCalls.push_back(call);
    
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
        v.x = tv.position[0] * invW * geo.viewport.scaleX + geo.viewport.transX;
        v.y = tv.position[1] * invW * geo.viewport.scaleY + geo.viewport.transY;
        // Normalize screen-space z to [0,1]. The ortho maps this to clip_z = 2*z_norm - 1 ∈ [-1,1]
        // (full NDC range). Near z_norm=0 → window_depth=0, Far z_norm=1 → window_depth=1.0 (clear).
        // Without normalization, z_screen ∈ [0,1024] → clip_z ∈ [-1,3] → far geometry clipped.
        float z_screen = tv.position[2] * invW * geo.viewport.scaleZ + geo.viewport.transZ;
        float z_range  = geo.viewport.transZ + std::abs(geo.viewport.scaleZ);
        v.z = (z_range > 0.0f) ? z_screen / z_range : 0.0f;
        v.z = std::max(0.0f, std::min(1.0f, v.z));  // clamp: extrapolated/clipped verts may stray outside
        v.w = 1.0f;  // ortho maps screen coords -> NDC; clip W already divided out
        v.r = tv.color[0]; v.g = tv.color[1];
        v.b = tv.color[2]; v.a = tv.color[3];
        // Apply gSPTexture scale at draw time so the correct scale is used
        // even if gSPTexture is issued after gSPVertex.
        float scaleS = geo.texture.enabled ? geo.texture.GetScaleSFloat() : 1.0f;
        float scaleT = geo.texture.enabled ? geo.texture.GetScaleTFloat() : 1.0f;
        // Store texcoords pre-divided by clip_W (= texCoord * invW).
        // The fragment shader divides by vClipW (= invW) to recover the screen-space texcoord.
        // This gives perspective-correct interpolation even with an orthographic projection.
        v.s  = tv.texCoord[0]  * scaleS * invW; v.t  = tv.texCoord[1]  * scaleT * invW;
        v.s1 = tv.texCoord1[0] * scaleS * invW; v.t1 = tv.texCoord1[1] * scaleT * invW;
        v.perspW = invW;
        v.fogFactor = tv.fogFactor;
        v.clipFlags = tv.clipFlags;  // Copy clip flags for frustum culling
        return v;
    };

    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = 0;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = geo.geometryMode;
    PopulateTextureInfo(call, 0);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;

    m_batch.vertices.push_back(makeVert(v0));
    m_batch.vertices.push_back(makeVert(v1));
    m_batch.vertices.push_back(makeVert(v2));
    call.vertexCount = 3;

    m_batch.drawCalls.push_back(call);

    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTriangleRaw(const RenderBatch::Vertex& rv0, const RenderBatch::Vertex& rv1,
                                   const RenderBatch::Vertex& rv2, uint32_t geometryMode) {
    if (m_batch.IsFull()) FlushBatch();

    DrawCall call;
    call.type = DrawCall::Type::Triangle;
    call.vertexStart = static_cast<int>(m_batch.vertices.size());
    call.tileIndex = 0;
    call.combineMode = m_state.rdp.combineModeRaw;
    call.otherModeH = m_state.rdp.otherModeH;
    call.otherModeL = m_state.rdp.otherModeL;
    call.geometryMode = geometryMode;
    PopulateTextureInfo(call, 0);
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;

    m_batch.vertices.push_back(rv0);
    m_batch.vertices.push_back(rv1);
    m_batch.vertices.push_back(rv2);
    call.vertexCount = 3;

    m_batch.drawCalls.push_back(call);
    m_stats.trianglesDrawn++;
    m_stats.frameTriangles++;
    m_state.rdp.triangleCount++;
}

void RDPProcessor::DrawTextureRect(const DecodedRectangle& rect) {
    if (m_batch.IsFull()) FlushBatch();

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
    
    // Generate 4 vertices (two triangles) for the rectangle
    float width = rect.x1 - rect.x0;
    float height = rect.y1 - rect.y0;
    
    float s0 = rect.s;
    float t0 = rect.t;
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
    
    // Top-left
    v[0] = { rect.x0, rect.y0, 0, 1, s0, t0, 1, 1, 1, 1, 0, 0, 0, 0 };
    // Top-right
    v[1] = { rect.x1, rect.y0, 0, 1, s1, t0, 1, 1, 1, 1, 0, 0, 0, 0 };
    // Bottom-left
    v[2] = { rect.x0, rect.y1, 0, 1, s0, t1, 1, 1, 1, 1, 0, 0, 0, 0 };
    // Bottom-right
    v[3] = { rect.x1, rect.y1, 0, 1, s1, t1, 1, 1, 1, 1, 0, 0, 0, 0 };
    
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
    
    m_batch.drawCalls.push_back(call);
    
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
    std::memset(&call.tileDesc0, 0, sizeof(call.tileDesc0));
    std::memset(&call.tileDesc1, 0, sizeof(call.tileDesc1));
    call.primColor = m_state.rdp.primColor;
    call.envColor = m_state.rdp.envColor;
    call.fogColor = m_state.rdp.fogColor;
    call.blendColor = m_state.rdp.blendColor;
    call.primLODFrac = m_state.rdp.primLODFrac;

    float fr, fg, fb, fa;
    if (rdpCycleType == 3) {
        // FILL mode: raw fill color, opaque
        call.combineMode = 0;
        fr = static_cast<float>(m_state.rdp.fillColor.r) / 255.0f;
        fg = static_cast<float>(m_state.rdp.fillColor.g) / 255.0f;
        fb = static_cast<float>(m_state.rdp.fillColor.b) / 255.0f;
        fa = 1.0f;
    } else {
        // 1-cycle / 2-cycle mode: use primColor as fill (e.g. OoT fade overlay).
        // The combiner in these modes typically outputs primColor, so we shortcut
        // by passing primColor directly as the fill color including its alpha.
        call.combineMode = 0;
        fr = static_cast<float>(m_state.rdp.primColor.r) / 255.0f;
        fg = static_cast<float>(m_state.rdp.primColor.g) / 255.0f;
        fb = static_cast<float>(m_state.rdp.primColor.b) / 255.0f;
        fa = static_cast<float>(m_state.rdp.primColor.a) / 255.0f;
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
    
    m_batch.drawCalls.push_back(call);
    
    m_stats.fillRectsDrawn++;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Texture Helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void RDPProcessor::PopulateTextureInfo(DrawCall& call, int tileIndex) {
    call.textureImageAddr = m_state.rdp.textureImage.address;

    // Snapshot tile descriptors for the two possible texture tiles
    const auto& tile0 = m_state.rdp.tiles[tileIndex & 7];
    const auto& tile1 = m_state.rdp.tiles[(tileIndex + 1) & 7];
    call.tileDesc0 = tile0;
    call.tileDesc1 = tile1;

    // Compute a quick hash to determine whether a texture is active.
    // A tile is "active" if it has non-zero dimensions and a valid TMEM address
    // or the texture image has been loaded.
    auto computeHash = [&](const TileDescriptor& td) -> uint64_t {
        int w = td.GetWidth();
        int h = td.GetHeight();
        if (w <= 0 || h <= 0) return 0;

        // Hash from tile fields + textureImage address
        uint64_t hash = 0;
        hash ^= static_cast<uint64_t>(call.textureImageAddr) * 0x9e3779b97f4a7c15ULL;
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
    call.textureHash0 = computeHash(tile0);
    call.textureHash1 = (cycleType == 1) ? computeHash(tile1) : 0;
}

Tex::TextureKey RDPProcessor::BuildTextureKey(const TileDescriptor& tile,
                                               uint32_t textureImageAddr) const {
    Tex::TextureKey key{};
    key.address = textureImageAddr;
    key.palette = tile.palette;
    key.width   = static_cast<uint16_t>(std::max(1, tile.GetWidth()));
    key.height  = static_cast<uint16_t>(std::max(1, tile.GetHeight()));
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

    // Compute CRC over the relevant TMEM region for content-change detection.
    // The tile's TMEM byte address tells us where the data lives in TMEM.
    uint32_t tmemByteAddr = tile.GetTMEMByteAddr();
    uint32_t tmemSize     = static_cast<uint32_t>(tile.GetBytesPerRow()) *
                            static_cast<uint32_t>(key.height);
    if (tmemSize > 0 && tmemByteAddr + tmemSize <= 4096) {
        key.crc = CRC32_Fast(m_state.rdp.tmem.data + tmemByteAddr, tmemSize);
    } else {
        key.crc = 0;
    }

    // For CI (Color Indexed) textures, palette lookup makes the decoded pixels
    // depend on the TLUT contents in the upper half of TMEM.  Mix the palette
    // region CRC into the texture CRC so a palette change triggers a cache miss.
    if (key.format == Tex::N64TexFormat::CI) {
        uint32_t palCrc = 0;
        if (key.size == Tex::N64TexSize::Bits4) {
            // CI4: 16 entries starting at TMEM_PALETTE_BASE + palette*0x80, each 4 bytes apart
            uint32_t palBase = TMEM_PALETTE_BASE + static_cast<uint32_t>(tile.palette) * 0x80;
            if (palBase + 16 * 4 <= TMEM_SIZE)
                palCrc = CRC32_Fast(m_state.rdp.tmem.data + palBase, 16 * 4);
        } else {
            // CI8: 256 entries starting at TMEM_PALETTE_BASE, each 4 bytes apart
            if (TMEM_PALETTE_BASE + 256 * 4 <= TMEM_SIZE)
                palCrc = CRC32_Fast(m_state.rdp.tmem.data + TMEM_PALETTE_BASE, 256 * 4);
        }
        key.crc ^= palCrc * 0x9e3779b9;
    }

    return key;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Batch Management
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void RDPProcessor::FlushBatch() {
    if (m_batch.drawCalls.empty()) return;
    if (!m_renderer) {
        m_batch.Reset();
        return;
    }



    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  1. Upload vertex data to a dynamic VBO
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //
    // RenderBatch::Vertex layout (44 bytes):
    //   float x, y, z, w   — position   (offset 0,  4 floats)
    //   float s, t          — texcoord   (offset 16, 2 floats)
    //   float r, g, b, a    — color      (offset 24, 4 floats)
    //   float fogFactor     — fog        (offset 40, 1 float)
    //
    // The N64 VertexLayout from Renderer.hpp expects:
    //   loc 0: float4 position (offset 0)
    //   loc 1: float4 color    (offset 16)
    //   loc 2: float2 texcoord0(offset 32)
    //   loc 3: float2 texcoord1(offset 40)
    //   loc 4: float1 fog      (offset 48)
    //   stride = 52
    //
    // We convert RenderBatch::Vertex â†’ interleaved GPU vertex format.

    struct GPUVertex {
        float x, y, z, w;      // position (loc 0)
        float r, g, b, a;      // color (loc 1)
        float s0, t0;          // texcoord0 (loc 2)
        float s1, t1;          // texcoord1 (loc 3) — same as tex0 for now
        float fog;              // fog (loc 4)
        float perspW;           // 1/clip_W for perspective-correct textures (loc 5)
    };
    static_assert(sizeof(GPUVertex) == 56, "GPUVertex must be 56 bytes for N64 layout");

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
        // Note: clipFlags stored in RenderBatch::Vertex but not passed to GPU
        // (used for CPU-side trivial rejection above)
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

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    //  2. Iterate draw calls, set state, and draw
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    // Compute N64 screen dimensions from scissor for ortho projection
    float n64W = static_cast<float>(m_state.rdp.scissor.xl >> 2);
    float n64H = static_cast<float>(m_state.rdp.scissor.yl >> 2);
    if (n64W < 1.0f) n64W = 320.0f;
    if (n64H < 1.0f) n64H = 240.0f;

    // Intel Iris Xe workaround: FBOs created lazily (after game textures are
    // loaded) can fail texture sampling.  Recreating the FBO here — during the
    // rendering phase — produces a fresh GL object that works correctly.
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
    // Z: clip_z = 2*z_norm - 1, so window_depth = z_norm ∈ [0,1] (full precision).
    // Near z_norm=0 → depth=0, Far z_norm=1 → depth=1.0 (= clear value).
    // GL_LEQUAL: near objects (depth → 0) correctly overwrite far objects (depth → 1).
    // Column-major for OpenGL: ortho(left=0, right=n64W, top=0, bottom=n64H)
    float ortho[16] = {
         2.0f/n64W, 0,          0,   0,
         0,        -2.0f/n64H,  0,   0,
         0,         0,           2,   0,
        -1.0f,      1.0f,       -1,   1
    };



    // FBO dimensions for scissor scaling — use the current color FBO size, not the window.
    // The N64 renders to a 320×240 FBO; using window dims (640×480) would double all scissors
    // (scaleX = 640/320 = 2.0) causing every scissor rect to overflow the FBO entirely.
    GPUFramebuffer* curFBO = g_fbManager.GetCurrentColor();
    float fboW = (curFBO && curFBO->scaledWidth  > 0) ? static_cast<float>(curFBO->scaledWidth)  : n64W;
    float fboH = (curFBO && curFBO->scaledHeight > 0) ? static_cast<float>(curFBO->scaledHeight) : n64H;

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

        // FillRect always uses the fill-color shader path.
        // For FILL mode (cycleType=3), the fill color is the raw N64 fill color.
        // For other modes, DrawFillRect substitutes primColor (with alpha) so that
        // fade overlays and similar effects blend correctly via GL hardware blending.
        if (call.type == DrawCall::Type::FillRect) {
            features |= Shader::SF_FILL_RECT;
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
        if (call.otherModeL & 0x0010) features |= Shader::SF_DEPTH_TEST;    // Z_CMP
        if (call.otherModeL & 0x0020) features |= Shader::SF_DEPTH_WRITE;   // Z_UPD
        if (call.otherModeL & 0x0004) features |= Shader::SF_DEPTH_SOURCE_PRIM; // Z_SOURCE_SEL=PRIM

        // N64-accurate Z-image depth compare: enable when Z_CMP is set
        // and the depth buffer manager has a current Z-image texture pair
        if ((call.otherModeL & 0x0010) && g_depthManager.GetCurrent() &&
            g_depthManager.GetCurrent()->glZImageTex != 0) {
            features |= Shader::SF_N64_DEPTH_COMPARE;
        }

        // Alpha compare: bit 0 = G_AC_THRESHOLD, bit 1 = G_AC_NOISE;
        // both modes need the alpha discard test in the shader.
        uint32_t alphaCmp = call.otherModeL & 0x03;
        if (alphaCmp & 1) features |= Shader::SF_ALPHA_TEST;

        // Fog (from geometry mode)
        if (call.geometryMode & 0x00010000) features |= Shader::SF_FOG;

        // Texture filter
        uint32_t texFilter = (call.otherModeH >> 12) & 0x03;
        if (texFilter == 2) features |= Shader::SF_TEXTURE_BILINEAR;
        else if (texFilter == 0) features |= Shader::SF_TEXTURE_3POINT; // N64 default

        // Monochrome post-process (game-specific hack — e.g. Zelda OoT cutscenes)
        if (m_hacks.zeldaMonochrome) features |= Shader::SF_MONOCHROME;

        // Texture LOD (G_TL_LOD — mipmap fraction via LOD_FRACTION combiner input)
        if (call.otherModeH & 0x00010000) features |= Shader::SF_TEXTURE_LOD;

        // Texture detail/sharpen (otherModeH bits 17-18: 0=clamp, 1=sharpen, 2=detail)
        {
            uint32_t tdMode = (call.otherModeH >> 17) & 0x03;
            if (tdMode == 1) features |= Shader::SF_TEXTURE_SHARPEN;
            if (tdMode == 2) features |= Shader::SF_TEXTURE_DETAIL;
        }

        // Alpha noise/dither (otherModeL bits 0-1: 1=threshold, 3=dither — note: value 2 unused)
        if ((call.otherModeL & 0x03) == 3) features |= Shader::SF_NOISE_ENABLE;

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

        // Framebuffer read: currently disabled for blender-based detection.
        // The shader blender only runs cycle 1 and skips when memColor is needed,
        // so blender selectors never trigger FB reads.  SF_FRAMEBUFFER_READ
        // can be re-enabled later for special screen effects that truly need it.

        // Coverage / AA flags (otherModeL)
        if (call.otherModeL & 0x0008) features |= Shader::SF_COVERAGE_ENABLE;  // AA_EN   bit 3
        if (call.otherModeL & 0x0080) features |= Shader::SF_COLOR_ON_COVERAGE; // CLR_ON_CVG bit 7
        if (call.otherModeL & 0x1000) features |= Shader::SF_ALPHA_CVG;         // CVG_X_ALPHA bit 12
        if (call.otherModeL & 0x2000) features |= Shader::SF_ALPHA_CVG_SEL;     // ALPHA_CVG_SEL bit 13

        shaderKey.features = features;

        // --- Get/compile shader ---
        const Shader::CompiledShader* shader = nullptr;
        if (m_shaderManager) {
            shader = m_shaderManager->GetShader(shaderKey);
        }


        if (!shader) {
            static int nullShaderWarnCount = 0;
            if (nullShaderWarnCount < 10) {
                NOVA_LOG_WARN("FlushBatch: null shader for draw#{} mux=0x{:016X} features=0x{:08X} cycleType={} drawType={}",
                              nullShaderWarnCount, shaderKey.mux, shaderKey.features,
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
                call.primLODFrac);


            // Set fill color for fill rect draws (shader reads uFillColor)
            if (call.type == DrawCall::Type::FillRect) {
                const auto& v0 = m_batch.vertices[call.vertexStart];
                m_shaderManager->SetFillColor(v0.r, v0.g, v0.b, v0.a);
            }

            // Set screen size uniforms (uScreenSize, uInvScreenSize) — use FBO dims
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
                // S/T values go directly to the texture lookup — no tile shift.
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

                int w0 = call.tileDesc0.GetWidth();
                int h0 = call.tileDesc0.GetHeight();
                if (w0 > 0 && h0 > 0) {
                    size0[0] = static_cast<float>(w0);
                    size0[1] = static_cast<float>(h0);
                }
                int w1 = call.tileDesc1.GetWidth();
                int h1 = call.tileDesc1.GetHeight();
                if (w1 > 0 && h1 > 0) {
                    size1[0] = static_cast<float>(w1);
                    size1[1] = static_cast<float>(h1);
                }

                m_shaderManager->SetTextureUniforms(0, 1, scale0, scale1, size0, size1, nullptr, nullptr);
            }

            // Set tile offset, clamp, mirror, mask uniforms
            {
                float offset0[2] = { call.tileDesc0.GetSL(), call.tileDesc0.GetTL() };
                float offset1[2] = { call.tileDesc1.GetSL(), call.tileDesc1.GetTL() };
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

            // Set alpha reference for alpha compare
            {
                uint32_t alphaCompMode = call.otherModeL & 0x03;
                if (alphaCompMode & 1) {
                    // Threshold (1) or dither (3) — set blendColor.a as reference
                    m_shaderManager->SetAlphaRef(call.blendColor.a / 255.0f);
                }
            }

            // Set fog params from geometry state
            {
                float fogMul = static_cast<float>(m_state.rdp.geometry.fogMultiplier) / 256.0f;
                float fogOff = static_cast<float>(m_state.rdp.geometry.fogOffset) / 256.0f;
                m_shaderManager->SetFogParams(fogMul, fogOff);
            }

            // Set depth params
            m_shaderManager->SetDepthParams(1.0f, 0.0f,
                static_cast<float>(m_state.rdp.primDepth) / 65535.0f);
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
            uint32_t cvgXAlpha  = (call.otherModeL >> 12) & 1;
            uint32_t alphaCvgSel = (call.otherModeL >> 13) & 1;
            bool forceBlend = ((call.otherModeL >> 14) & 1) != 0;

            // In 2-cycle mode, the final pixel color comes from cycle 1.
            // In 1-cycle (or copy/fill), use cycle 0.
            uint32_t cycleType = (call.otherModeH >> 20) & 3;
            uint32_t pF = (cycleType == 1) ? p1 : p0;
            uint32_t aF = (cycleType == 1) ? a1 : a0;
            uint32_t mF = (cycleType == 1) ? m1 : m0;
            uint32_t bF = (cycleType == 1) ? b1 : b0;

            // Determine whether GL hardware blending is needed.
            //
            // Two independent compositing paths exist:
            //  (a) Shader blender — runs when GenerateBlenderCode emits code.
            //      This happens when the active cycle does NOT reference memColor,
            //      OR when SF_FRAMEBUFFER_READ is available.
            //  (b) GL_BLEND — hardware alpha-blends the fragment with the real FB.
            //
            // When the shader blender handles the full formula (with FB-read),
            // it writes combined.a=1.0 so GL_BLEND can stay off.
            // When the shader blender is SKIPPED (memColor needed but no FB-read),
            // we must enable GL_BLEND as the fallback compositing path.
            bool needsMemColor = (mF == 1) || (bF == 1);
            bool hasFBRead = (features & Shader::SF_FRAMEBUFFER_READ) != 0;
            bool shaderBlenderSkipped = needsMemColor && !hasFBRead;

            bool isTranslucent = shaderBlenderSkipped && forceBlend;

            // N64 blender: result = (P*A + M*B)/(A+B).
            // When P==M, result = P*(A+B)/(A+B) = P → always opaque, regardless of alpha.
            if (pF == mF) {
                blend.enabled = false;
            } else {
                blend.enabled = isTranslucent;
            }

            // Derive GL blend factors from N64 blender mux fields.
            // N64 blender: result = (P*A + M*B) / (A+B)
            //   P sources: 0=CC, 1=Mem, 2=BlendColor, 3=FogColor
            //   A sources: 0=CCAlpha, 1=FogAlpha, 2=ShadeAlpha, 3=Zero
            //   M sources: 0=CC, 1=Mem, 2=BlendColor, 3=FogColor
            //   B sources: 0=1-A, 1=MemAlpha, 2=One, 3=Zero
            //
            // The GL blend approximation:
            //   - A is the "source factor" (alpha-like multiplier for P)
            //   - B is the "dest factor" (multiplier for M, which maps to Mem/dst)
            //   - We map P→src, M→dst in most cases.

// A factor (source multiplier)
            switch (aF) {
                case 0: // CCAlpha (combiner output alpha = fragment alpha)
                    blend.srcColor = BlendFactor::SrcAlpha;
                    break;
                case 1: // Cycle 1: FogAlpha, Cycle 2: MemAlpha
                    if (cycleType == 1) blend.srcColor = BlendFactor::DstAlpha;
                    else blend.srcColor = BlendFactor::SrcAlpha;
                    break;
                case 2: // ShadeAlpha — vertex color alpha, also in SrcAlpha after combiner
                    blend.srcColor = BlendFactor::SrcAlpha;
                    break;
                case 3: // Zero
                    blend.srcColor = BlendFactor::Zero;
                    // If A=0 and B=0, force opaque (P*0 + M*0)/0 is undefined, treat as opaque
                    if (bF == 3) {
                        blend.srcColor = BlendFactor::One;
                    }
                    break;
            }

            // B factor (destination multiplier)
            switch (bF) {
                case 0: // 1-A
                    if (aF == 1 && cycleType == 1) blend.dstColor = BlendFactor::OneMinusDstAlpha; // 1 - MemAlpha
                    else if (aF == 3) blend.dstColor = BlendFactor::One; // 1 - 0
                    else blend.dstColor = BlendFactor::OneMinusSrcAlpha; // 1 - (CC/Fog/Shade)Alpha
                    break;
                case 1: // MemAlpha (framebuffer alpha)
                    blend.dstColor = BlendFactor::DstAlpha;
                    break;
                case 2: // One (additive)
                    blend.dstColor = BlendFactor::One;
                    break;
                case 3: // Zero
                    blend.dstColor = BlendFactor::Zero;
                    break;
            }

            // Alpha channel: preserve source alpha to framebuffer for
            // subsequent B=MemAlpha reads by other draw calls.
            blend.srcAlpha = BlendFactor::One;
            blend.dstAlpha = BlendFactor::Zero;

            // --- Texture-off sanity check (was blastCorpsBlendFix) ---
            // When textures are disabled but the combiner references TEXEL0/TEXEL1,
            // the combiner outputs undefined (black/zero-alpha on real HW for most
            // titles).  With SrcAlpha blend that means invisible pixels, but if
            // force-blend is set those zero-alpha fragments still get written and
            // corrupt the framebuffer.  Force opaque dst-keep to prevent artifacts.
            //
            // IMPORTANT: The shader already initialises texel0 to vec4(0,0,0,1) when
            // no texture is bound, so alpha=1.0 avoids the zero-alpha corruption.
            // Only check for actual TEXEL0(1) or TEXEL1(2) references in the
            // *active* cycle, not COMBINED(0) which is a valid non-texture source.
            //
            // Skip this check for TextureRect/FillRect: these don't carry a
            // geometry mode, so G_TEXTURE_ENABLE is never set. They inherently
            // use textures (or a fill color) and the shader already handles the
            // no-texture fallback via the texel0 default.
            if (call.type == DrawCall::Type::Triangle) {
                bool textureOff = ((call.geometryMode & 0x40000) == 0); // !G_TEXTURE_ENABLE
                // Check the active cycle's A input (cycle 1 for 1-cycle mode)
                uint32_t activeCycleA;
                if (cycleType == 0) { // 1-cycle: uses cycle 1 fields
                    activeCycleA = (call.combineMode >> 37) & 0xF; // saRGB1 at hi bits 5-8
                } else {
                    activeCycleA = (call.combineMode >> 52) & 0xF; // saRGB0 at hi bits 20-23
                }
                bool combinerUsesTexel = (activeCycleA == 1 || activeCycleA == 2);
                if (textureOff && combinerUsesTexel) {
                    blend.srcColor = BlendFactor::Zero;
                    blend.dstColor = BlendFactor::One;
                }
            }
        }
        m_renderer->SetBlendState(blend);

        // --- Set depth state ---
        DepthStencilState depth;
        depth.depthTestEnabled  = (features & Shader::SF_DEPTH_TEST) != 0;
        depth.depthWriteEnabled = (features & Shader::SF_DEPTH_WRITE) != 0;
        // Derive depth func from N64 zMode (otherModeL bits 10-11)
        uint32_t zMode = (call.otherModeL >> 10) & 0x03;
        switch (zMode) {
            case 0: depth.depthFunc = CompareFunc::LessEqual; break;  // OPAQUE
            case 1: depth.depthFunc = CompareFunc::Less;      break;  // INTERPENETRATING
            case 2: depth.depthFunc = CompareFunc::Less;      break;  // TRANSPARENT
            case 3:                                                     // DECAL
                depth.depthFunc = CompareFunc::LessEqual;
                depth.depthWriteEnabled = false;              // decals don't write depth
                break;
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
        // Polygon offset for ZMODE_DEC (decal) — nudge decal geometry slightly
        // toward the camera to prevent Z-fighting with the base surface.
        if (zMode == 3) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        m_renderer->SetDepthStencilState(depth);

        // ─── N64 Z-image depth compare: bind Z-image textures + set mode ────
        if (features & Shader::SF_N64_DEPTH_COMPARE) {
            auto* depthBuf = g_depthManager.GetCurrent();
            if (depthBuf) {
                g_depthManager.InitDepthImageTextures(*depthBuf);
                g_depthManager.BindDepthImageTextures(*depthBuf, 8, 9);
                g_depthManager.BindZLUT(10);
                m_shaderManager->SetN64DepthMode(
                    static_cast<int>(zMode),
                    static_cast<float>(m_state.rdp.primDeltaZ) / 65535.0f
                );
            }
        }

        // ─── CVG_DST coverage accumulator via stencil ───────────────────────
        // The N64 uses the 3 coverage bits per pixel for anti-aliasing blending.
        // We map CVG_DST to GL stencil operations on the 3 LSBs of the stencil
        // plane (packed into the GL_DEPTH32F_STENCIL8 depth-stencil surface).
        {
            uint32_t cvgDst = (call.otherModeL >> 8) & 0x03;
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 0x07, 0x07);
            glStencilMask(0x07);
            switch (cvgDst) {
            case 0: // CVG_DST_CLAMP — increment, cap at 7
                glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                break;
            case 1: // CVG_DST_WRAP — increment with wrap
                glStencilOp(GL_KEEP, GL_KEEP, GL_INCR_WRAP);
                break;
            case 2: // CVG_DST_FULL/ZAP — force to 7 (full coverage)
                glStencilFunc(GL_ALWAYS, 0x07, 0x07);
                glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
                break;
            case 3: // CVG_DST_SAVE — preserve existing coverage
            default:
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                break;
            }
        }

        // --- Set scissor (scale N64 coords to GL window coords, flip Y) ---
        {
            int n64sx = static_cast<int>(m_state.rdp.scissor.xh >> 2);
            int n64sy = static_cast<int>(m_state.rdp.scissor.yh >> 2);
            int n64sw = static_cast<int>((m_state.rdp.scissor.xl >> 2) - n64sx);
            int n64sh = static_cast<int>((m_state.rdp.scissor.yl >> 2) - n64sy);

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

        // --- Bind textures if available and set wrap modes ---
        float gpuScale0[2] = {1.0f, 1.0f};
        float gpuScale1[2] = {1.0f, 1.0f};

        auto getGLWrapMode = [](const TileDescriptor& td, bool isS) -> GLenum {
            bool clamp  = isS ? td.IsClamped_S()  : td.IsClamped_T();
            bool mirror = isS ? td.IsMirrored_S() : td.IsMirrored_T();
            if (clamp)  return GL_CLAMP_TO_EDGE;
            if (mirror) return GL_MIRRORED_REPEAT;
            return GL_REPEAT;
        };

        if (m_textureCache && call.textureHash0 != 0) {
            Tex::TextureKey key0 = BuildTextureKey(call.tileDesc0, call.textureImageAddr);
            if (key0.width > 0 && key0.height > 0) {
                GPUFramebuffer* rtFB0 = g_fbManager.FindByAddress(key0.address);
                uint32_t fbTex0 = rtFB0 ? g_fbManager.GetSampleableTexture(
                    rtFB0, rtFB0 == g_fbManager.GetCurrentColor()) : 0;
                if (fbTex0 != 0) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, fbTex0);
                } else {
                    Tex::CachedTexture* cached0 = m_textureCache->GetTexture(
                        key0, m_rdram, m_state.rdp.tmem.data);
                    if (cached0 && cached0->gpuTexture) {
                        m_renderer->BindTexture(0, cached0->gpuTexture.get());
                        // Texture is already bound on unit 0 by BindTexture above
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getGLWrapMode(call.tileDesc0, true));
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getGLWrapMode(call.tileDesc0, false));
                        gpuScale0[0] = cached0->scaleS;
                        gpuScale0[1] = cached0->scaleT;
                    }


                }
            }
        }
        if (m_textureCache && call.textureHash1 != 0) {
            Tex::TextureKey key1 = BuildTextureKey(call.tileDesc1, call.textureImageAddr);
            if (key1.width > 0 && key1.height > 0) {
                GPUFramebuffer* rtFB1 = g_fbManager.FindByAddress(key1.address);
                uint32_t fbTex1 = rtFB1 ? g_fbManager.GetSampleableTexture(
                    rtFB1, rtFB1 == g_fbManager.GetCurrentColor()) : 0;
                if (fbTex1 != 0) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, fbTex1);
                } else {
                    Tex::CachedTexture* cached1 = m_textureCache->GetTexture(
                        key1, m_rdram, m_state.rdp.tmem.data);
                    if (cached1 && cached1->gpuTexture) {
                        m_renderer->BindTexture(1, cached1->gpuTexture.get());
                        // Texture is already bound on unit 1 by BindTexture above
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getGLWrapMode(call.tileDesc1, true));
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getGLWrapMode(call.tileDesc1, false));
                        gpuScale1[0] = cached1->scaleS;
                        gpuScale1[1] = cached1->scaleT;
                    }
                }
            }
        }
        m_shaderManager->SetGPUScale(gpuScale0, gpuScale1);

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
            // Shader blender computes the blend formula; disable GL hardware blend
            // to avoid double-blending.
            blend.enabled = false;
            m_renderer->SetBlendState(blend);
        }

        // --- Trivial rejection: cull triangles where all 3 verts are outside same plane ---
        if (call.type == DrawCall::Type::Triangle && call.vertexCount >= 3) {
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
                // Skip this triangle entirely — all vertices are culled
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


        m_renderer->DrawArrays(prim,
                               static_cast<uint32_t>(batchBaseVertex + call.vertexStart),
                               static_cast<uint32_t>(call.vertexCount));

    }

    m_stats.batchesFlushed++;
    m_stats.frameDrawCalls += static_cast<uint32_t>(m_batch.drawCalls.size());
    
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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Frame Buffer Access
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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






