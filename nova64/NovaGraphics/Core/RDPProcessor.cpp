/**
 * @file RDPProcessor.cpp
 * @brief RDP command processor — lifecycle: ctor, Initialize, Shutdown, Reset,
 *        ProcessDList, ProcessRDPCommand, ProcessCommandBuffer.
 *        State handlers  → RDPProcessor_State.cpp
 *        Texture loads   → RDPProcessor_Texture.cpp
 *        Draw primitives → RDPProcessor_Draw.cpp
 *        Batch/flush     → RDPProcessor_Batch.cpp
 */
#include "RDPProcessor_Private.hpp"
#include <cstdlib>

RDPProcessor::RDPProcessor() {
    m_stats.Reset();
    // Headless debug hook: NOVA64_DEBUG_OUTPUT_MODE lets diagnostics select
    // the shader's built-in debug-output modes (1=texel0, 2=shade, 3=prim,
    // 4=env, 5=UV, 6=depth, 7=alpha) without needing the interactive ImGui UI.
    if (const char* v = std::getenv("NOVA64_DEBUG_OUTPUT_MODE")) {
        SetDebugOutputMode(std::atoi(v));
    }
}

RDPProcessor::~RDPProcessor() { Shutdown(); }

bool RDPProcessor::Initialize(uint8_t* rdram, size_t rdramSize, IRenderer* renderer, Tex::TextureCache* textureCache,
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

    // --- Create noise texture (64x64 random R8, bound to unit 5) ---
    {
        constexpr int NOISE_SIZE = 64;
        std::mt19937 rng(42); // fixed seed for reproducibility
        std::uniform_int_distribution<int> dist(0, 255);
        std::vector<uint8_t> noiseData(NOISE_SIZE * NOISE_SIZE);
        for (auto& p : noiseData) p = static_cast<uint8_t>(dist(rng));

        TextureDesc desc;
        desc.width = NOISE_SIZE;
        desc.height = NOISE_SIZE;
        desc.mipLevels = 1;
        desc.format = TextureFormat::R8;
        desc.usage = BufferUsage::Dynamic;
        desc.debugName = "NovaNoise";
        m_noiseTexture = m_renderer->CreateTexture(desc);
        if (m_noiseTexture) {
            m_noiseTexture->Upload(noiseData.data(), noiseData.size());
            SamplerDesc s;
            s.minFilter = SamplerFilter::Nearest;
            s.magFilter = SamplerFilter::Nearest;
            s.wrapS = TextureWrap::Repeat;
            s.wrapT = TextureWrap::Repeat;
            m_noiseTexture->SetSampler(s);
        }
    }

    // --- Create N64 dither textures (4x4 R8, bound to unit 6) ---
    // otherModeH rgb_dither_sel selects the dither:
    //   Mode 0: Magic Square (N64 default), values 0-7 (3 bits)
    //   Mode 1: Standard Bayer, values 0-7 (3 bits)
    //   Mode 2: Noise dither (uses the noise texture)
    //   Mode 3: Disabled
    // Both matrices are stored in separate textures; the active one is bound at draw time.
    {
        // N64 Magic Square dither matrix (0-7 scaled to 0-255)
        static constexpr uint8_t magicSquare4x4[16] = {0,   219, 36,  255, 146, 73,  182, 109,
                                                       219, 0,   255, 36,  73,  146, 109, 182};
        TextureDesc desc;
        desc.width = 4;
        desc.height = 4;
        desc.mipLevels = 1;
        desc.format = TextureFormat::R8;
        desc.usage = BufferUsage::Static;
        desc.debugName = "NovaDitherMagic";
        m_ditherTexture = m_renderer->CreateTexture(desc);
        if (m_ditherTexture) {
            m_ditherTexture->Upload(magicSquare4x4, sizeof(magicSquare4x4));
            SamplerDesc s;
            s.minFilter = SamplerFilter::Nearest;
            s.magFilter = SamplerFilter::Nearest;
            s.wrapS = TextureWrap::Repeat;
            s.wrapT = TextureWrap::Repeat;
            m_ditherTexture->SetSampler(s);
        }

        // N64 Bayer dither matrix (0-7 scaled to 0-255)
        static constexpr uint8_t bayer4x4[16] = {0,   146, 36, 182, 146, 0,   182, 36,
                                                 109, 255, 73, 219, 255, 109, 219, 73};
        TextureDesc desc2;
        desc2.width = 4;
        desc2.height = 4;
        desc2.mipLevels = 1;
        desc2.format = TextureFormat::R8;
        desc2.usage = BufferUsage::Static;
        desc2.debugName = "NovaDitherBayer";
        m_ditherBayerTexture = m_renderer->CreateTexture(desc2);
        if (m_ditherBayerTexture) {
            m_ditherBayerTexture->Upload(bayer4x4, sizeof(bayer4x4));
            SamplerDesc s;
            s.minFilter = SamplerFilter::Nearest;
            s.magFilter = SamplerFilter::Nearest;
            s.wrapS = TextureWrap::Repeat;
            s.wrapT = TextureWrap::Repeat;
            m_ditherBayerTexture->SetSampler(s);
        }
    }

    // --- Create debug white texture (1x1 RGBA8, for diagnostic tests) ---
    {
        uint32_t white = 0xFFFFFFFF;
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.mipLevels = 1;
        desc.format = TextureFormat::RGBA8;
        desc.usage = BufferUsage::Static;
        desc.debugName = "NovaDebugWhite";
        m_debugWhiteTexture = m_renderer->CreateTexture(desc);
        if (m_debugWhiteTexture) {
            m_debugWhiteTexture->Upload(&white, sizeof(white));
            SamplerDesc s;
            s.minFilter = SamplerFilter::Nearest;
            s.magFilter = SamplerFilter::Nearest;
            s.wrapS = TextureWrap::Repeat;
            s.wrapT = TextureWrap::Repeat;
            m_debugWhiteTexture->SetSampler(s);
        }
    }

    m_initialized = true;

    NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "RDP Processor initialized (RDRAM: {} MB)", rdramSize / (1024 * 1024));
    return true;
}

void RDPProcessor::Shutdown() {
    if (!m_initialized) return;

    FlushBatch();

    m_noiseTexture.reset();
    m_ditherTexture.reset();
    m_ditherBayerTexture.reset();
    m_debugWhiteTexture.reset();
    if (m_shadowTLUTTexture && g_fbManager.IsGL()) {
        glDeleteTextures(1, &m_shadowTLUTTexture);
        m_shadowTLUTTexture = 0;
    }

    m_state.Reset();
    m_commandBuffer.Reset();
    m_initialized = false;

    NOVA_LOG(LogLevel::Info, LogSubsystem::Core, "RDP Processor shutdown (commands: {}, triangles: {}, rects: {})",
             m_stats.commandsProcessed, m_stats.trianglesDrawn, m_stats.rectsDrawn);
}

void RDPProcessor::Reset(bool coldReset) {
    FlushBatch();
    uint32_t savedFrameCount = m_state.frameCount;
    float savedWidescreenScale = m_state.rdp.geometry.widescreenScale;
    m_state.Reset(coldReset);
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

// --- Display List Processing ---

void RDPProcessor::ProcessDList(uint32_t startAddress, uint32_t endAddress) {
    if (!m_initialized || !m_rdram) return;

    if (startAddress >= m_rdramSize || endAddress > m_rdramSize || startAddress >= endAddress) {
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core, "Invalid DList range: 0x{:08X} - 0x{:08X}", startAddress,
                 endAddress);
        return;
    }

    m_state.displayListActive = true;
    m_stats.displayListsProcessed++;

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

    static uint32_t s_histo[64] = {};
    static bool s_histoDumped = false;
    static uint32_t s_histoCommands = 0;
    const bool histoActive = !s_histoDumped;

    while (offset < wordCount) {
        const uint32_t* current = commands + offset;
        int remaining = wordCount - offset;

        if (remaining < 2) break; // Need at least 2 words (64 bits)

        RDPCommandID cmdID = m_decoder.DecodeCommand(current, remaining);

        if (histoActive) {
            uint32_t op = (current[0] >> 24) & 0x3F;
            s_histo[op]++;
            if (++s_histoCommands >= 20000) {
                s_histoDumped = true;
                char buf[2048];
                int len = 0;
                len += std::snprintf(buf + len, sizeof(buf) - len,
                                     "[RDP-HISTO] first %u commands by opcode (top hits):", s_histoCommands);
                for (uint32_t op2 = 0; op2 < 64; ++op2) {
                    if (s_histo[op2] == 0) continue;
                    if (len < (int)sizeof(buf) - 32)
                        len += std::snprintf(buf + len, sizeof(buf) - len, " 0x%02X=%u", op2, s_histo[op2]);
                }
                Log::infof("%s", buf);
            }
        }

        if (m_logCommands) {
            m_lastCommandDesc = RDPCommandDecoder::DescribeCommand(cmdID);
        }

        m_stats.commandsProcessed++;

        switch (cmdID) {
        case RDPCommandID::Nop: offset += 2; break;

        // --- Triangles ---
        case RDPCommandID::TriNoShade:
        case RDPCommandID::TriNoShadeZ:
        case RDPCommandID::TriTexNoShade:
        case RDPCommandID::TriTexNoShadeZ:
        case RDPCommandID::TriShade:
        case RDPCommandID::TriShadeZ:
        case RDPCommandID::TriTexShade:
        case RDPCommandID::TriTexShadeZ:   {
            bool shade = (static_cast<int>(cmdID) & 0x04) != 0;
            bool texture = (static_cast<int>(cmdID) & 0x02) != 0;
            bool zbuffer = (static_cast<int>(cmdID) & 0x01) != 0;
            int triSize = 8;            // Edge coefficients: 4 x 64-bit words = 8 x 32-bit words
            if (shade) triSize += 16;   // Shade: 8 x 64-bit = 16 x 32-bit
            if (texture) triSize += 16; // Texture: 8 x 64-bit = 16 x 32-bit
            if (zbuffer) triSize += 4;  // Z: 2 x 64-bit = 4 x 32-bit

            if (remaining >= triSize) {
                HandleTriangle(current, triSize);
            }
            offset += triSize;
            break;
        }

        // --- Rectangles ---
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

        // --- Syncs ---
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

        // --- State Setting ---
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

        // --- Texture Loads ---
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
            // Unknown command: skip 2 words
            NOVA_LOG(LogLevel::Debug, LogSubsystem::RDP, "Unknown RDP command: 0x{:02X}", static_cast<int>(cmdID));
            offset += 2;
            break;
        }
    }
}

} // namespace Nova64::Video
