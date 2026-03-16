/**
 * @file RDPProcessor.hpp
 * @brief RDP display list processor — the central command dispatch
 *
 * This class processes raw RDP commands from the command buffer, updates the
 * RDP state machine, performs texture loads into TMEM, and dispatches rendering
 * operations (triangles, rectangles, fills) to the renderer backend.
 */

#pragma once

#include "N64Types.hpp"
#include "RDPState.hpp"
#include "RDPCommand.hpp"
#include "../Common/Logger.hpp"
#include "../Renderer/Renderer.hpp"
#include "../Texture/TextureCache.hpp"

#include <cstdint>
#include <functional>
#include <vector>
#include <array>
#include <string>

namespace Nova64::Video {

// Forward declarations
class MicrocodeHandler;

namespace Shader { class ShaderManager; }

// ═══════════════════════════════════════════════════════════════════════════
//  Render Batch — accumulated draw calls to send to GPU
// ═══════════════════════════════════════════════════════════════════════════

struct DrawCall {
    enum class Type : uint8_t {
        Triangle,
        TextureRect,
        FillRect,
    };
    
    Type type;
    int  vertexStart;       // Index into batch vertex buffer
    int  vertexCount;
    int  tileIndex;
    
    // State snapshot at time of draw call
    uint64_t combineMode;
    uint32_t otherModeH;
    uint32_t otherModeL;
    uint32_t geometryMode;
    
    // Texture hash for this draw call (non-zero → texture is active on that unit)
    uint64_t textureHash0;
    uint64_t textureHash1;

    // Tile descriptor snapshots for texture binding at flush time
    TileDescriptor tileDesc0;
    TileDescriptor tileDesc1;
    uint32_t textureImageAddr;   // RDRAM address of textureImage at draw time

    // Color state snapshot — needed for correct per-draw-call uniforms
    RDPColor primColor;
    RDPColor envColor;
    RDPColor fogColor;
    RDPColor blendColor;
    float    primLODFrac;
};

struct RenderBatch {
    static constexpr int MAX_DRAW_CALLS = 4096;
    static constexpr int MAX_VERTICES   = 65536;
    
    struct Vertex {
        float x, y, z, w;
        float s, t;             // tile 0 texture coordinates (stored as texCoord * 1/clipW)
        float r, g, b, a;
        float fogFactor;
        float s1, t1;           // tile 1 texture coordinates (stored as texCoord * 1/clipW)
        uint32_t clipFlags;     // Frustum clip flags (PosX, NegX, PosY, NegY, Near, Far)
        float perspW = 1.0f;    // 1/clip_W; fragment shader divides s,t by this to recover screen-space texcoords
    };
    
    std::vector<Vertex>   vertices;
    std::vector<DrawCall> drawCalls;
    
    void Reset() {
        vertices.clear();
        drawCalls.clear();
    }
    
    bool IsFull() const {
        return drawCalls.size() >= MAX_DRAW_CALLS || vertices.size() >= MAX_VERTICES;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Processor Statistics
// ═══════════════════════════════════════════════════════════════════════════

struct RDPProcessorStats {
    uint64_t commandsProcessed;
    uint64_t trianglesDrawn;
    uint64_t rectsDrawn;
    uint64_t fillRectsDrawn;
    uint64_t textureLoads;
    uint64_t syncFullCount;
    uint64_t syncPipeCount;
    uint64_t syncTileCount;
    uint64_t syncLoadCount;
    uint64_t displayListsProcessed;
    uint64_t batchesFlushed;
    uint64_t stateChanges;
    
    // Per-frame counters (reset each frame)
    uint32_t frameTriangles;
    uint32_t frameRects;
    uint32_t frameLoads;
    uint32_t frameDrawCalls;
    
    void Reset() {
        std::memset(this, 0, sizeof(*this));
    }
    
    void ResetFrame() {
        frameTriangles = 0;
        frameRects = 0;
        frameLoads = 0;
        frameDrawCalls = 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Display List Info
// ═══════════════════════════════════════════════════════════════════════════

struct DisplayListInfo {
    uint32_t startAddress;
    uint32_t endAddress;
    uint32_t commandCount;
    bool     fromDMEM;          // True if from SP DMEM, false if RDRAM
};

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Processor
// ═══════════════════════════════════════════════════════════════════════════

class RDPProcessor {
public:
    RDPProcessor();
    ~RDPProcessor();
    
    // ─── Initialization ───
    
    /// Initialize with RDRAM pointer and renderer backend
    bool Initialize(uint8_t* rdram, size_t rdramSize,
                    IRenderer* renderer,
                    Tex::TextureCache* textureCache,
                    Shader::ShaderManager* shaderManager = nullptr);
    
    /// Shutdown and release resources
    void Shutdown();
    
    /// Reset state (e.g., on game reset)
    void Reset(bool coldReset);
    
    // ─── Display List Processing ───
    
    /// Process a display list from RDRAM
    void ProcessDList(uint32_t startAddress, uint32_t endAddress);
    
    /// Process native RDP commands (from DPC registers)
    void ProcessRDPCommand(uint64_t command);
    
    /// Process a full RDP command buffer
    void ProcessCommandBuffer(const uint32_t* commands, int wordCount);
    
    /// Notify that a frame is being presented (flush + render)
    void OnFrameEnd();

    /// Set a callback invoked immediately when SyncFull is processed, before the
    /// batch flush. Used to signal the DP_FULL (MI_INTR_DP) interrupt to the CPU
    /// at the correct N64-hardware-accurate point within a display list rather
    /// than only at the very end of ProcessDList.
    using SyncFullCallback = std::function<void()>;
    void SetSyncFullCallback(SyncFullCallback cb) { m_syncFullCallback = std::move(cb); }
    
    // ─── Texture Loading ───
    
    /// Execute a LoadBlock into TMEM
    void ExecuteLoadBlock(int tile, int uls, int ult, int texels, int dxt);
    
    /// Execute a LoadTile into TMEM
    void ExecuteLoadTile(int tile, float sl, float tl, float sh, float th);
    
    /// Execute a LoadTLUT into TMEM (palette load)
    void ExecuteLoadTLUT(int tile, int count);
    
    // ─── Drawing Primitives ───
    
    /// Draw a decoded triangle
    void DrawTriangle(const DecodedTriangle& tri);
    
    /// Draw a triangle from vertex buffer indices (HLE path)
    void DrawTriangle(int v0, int v1, int v2, const GeometryState& geo);
    
    /// Draw a triangle from raw vertex data (used for clipped triangles)
    void DrawTriangleRaw(const RenderBatch::Vertex& rv0, const RenderBatch::Vertex& rv1,
                         const RenderBatch::Vertex& rv2, uint32_t geometryMode);
    
    /// Draw a texture rectangle
    void DrawTextureRect(const DecodedRectangle& rect);
    
    /// Draw a fill rectangle
    void DrawFillRect(float x0, float y0, float x1, float y1);
    
    // ─── Batch Management ───
    
    /// Flush the current render batch to the GPU
    void FlushBatch();
    
    /// Check if state change requires a batch flush
    bool NeedsBatchFlush(const DrawCall& newCall) const;
    
    // ─── State Access ───
    
    const RDPState& GetRDPState() const { return m_state.rdp; }
    const GeometryState& GetGeometryState() const { return m_state.rdp.geometry; }
    const VideoState& GetVideoState() const { return m_state; }
    VideoState& GetVideoStateMutable() { return m_state; }
    const RDPProcessorStats& GetStats() const { return m_stats; }
    
    /// Get current framebuffer info
    void GetFrameBufferInfo(FrameBufferInfo* info, int maxBuffers) const;
    
    /// Read framebuffer pixels
    bool ReadFrameBuffer(uint32_t address, uint32_t size, void* data);
    
    /// Write to framebuffer
    bool WriteFrameBuffer(uint32_t address, uint32_t size, const void* data);
    
    // ─── Debug ───
    
    /// Enable/disable command logging
    void SetCommandLogging(bool enable) { m_logCommands = enable; }
    
    /// Get description of last processed command
    const std::string& GetLastCommandDesc() const { return m_lastCommandDesc; }

private:
    // ─── Command Handlers ───
    
    void HandleSetOtherModes(const uint32_t* words);
    void HandleSetCombine(const uint32_t* words);
    void HandleSetTile(const uint32_t* words);
    void HandleSetTileSize(const uint32_t* words);
    void HandleSetScissor(const uint32_t* words);
    void HandleSetColorImage(const uint32_t* words);
    void HandleSetMaskImage(const uint32_t* words);
    void HandleSetTextureImage(const uint32_t* words);
    void HandleSetFillColor(const uint32_t* words);
    void HandleSetFogColor(const uint32_t* words);
    void HandleSetBlendColor(const uint32_t* words);
    void HandleSetPrimColor(const uint32_t* words);
    void HandleSetEnvColor(const uint32_t* words);
    void HandleSetPrimDepth(const uint32_t* words);
    void HandleSetConvert(const uint32_t* words);
    void HandleSetKeyGB(const uint32_t* words);
    void HandleSetKeyR(const uint32_t* words);
    void HandleLoadBlock(const uint32_t* words);
    void HandleLoadTile(const uint32_t* words);
    void HandleLoadTLUT(const uint32_t* words);
    void HandleSyncFull();
    void HandleSyncPipe();
    void HandleSyncTile();
    void HandleSyncLoad();
    void HandleTriangle(const uint32_t* words, int wordCount);
    void HandleTextureRect(const uint32_t* words);
    void HandleTextureRectFlip(const uint32_t* words);
    void HandleFillRect(const uint32_t* words);
    
    // ─── Internal State ───
    
    VideoState         m_state;
    RDPCommandDecoder  m_decoder;
    RDPCommandBuffer   m_commandBuffer;
    RenderBatch        m_batch;
    RDPProcessorStats  m_stats;
    
    // Persistent ring VBO for batched rendering (Task 2)
    std::unique_ptr<IVertexBuffer> m_ringVBO;
    size_t m_ringOffset = 0;
    static constexpr size_t RING_VBO_SIZE = RenderBatch::MAX_VERTICES * 56; // 56 = sizeof(FlushBatch local GPUVertex)

    // Scratch buffer for GPU vertex conversion (avoids per-flush heap allocation)
    std::vector<uint8_t> m_gpuVertScratch;

    // Default N64 pipeline (owns the VAO for vertex format binding)
    std::unique_ptr<IPipeline> m_n64Pipeline;
    
    // Backend references (not owned)
    uint8_t*           m_rdram = nullptr;
    size_t             m_rdramSize = 0;
    IRenderer*         m_renderer = nullptr;
    Tex::TextureCache* m_textureCache = nullptr;
    Shader::ShaderManager* m_shaderManager = nullptr;

    // GPU noise & dither textures (bound to units 5 and 6)
    uint32_t m_noiseTexture = 0;
    uint32_t m_ditherTexture = 0;      // N64 Magic Square (default)
    uint32_t m_ditherBayerTexture = 0; // N64 Bayer (mode 1)
    
    // Debug
    bool        m_logCommands = false;
    std::string m_lastCommandDesc;
    
    bool m_initialized = false;

    // Callback fired on each SyncFull, before FlushBatch.
    // Used to deliver the DP interrupt to the CPU at the correct time.
    SyncFullCallback m_syncFullCallback;

public:
    // ─── Game Hack Flags ───
    // Set by VideoPlugin based on GameProfile. Checked in hot paths.
    struct GameHacks {
        bool skipTexRect = false;           // WinBack: skip TEXRECT entirely
        bool noDepthFrameBuffers = false;   // Don't use depth FBs as textures
        bool doNotResetOtherMode = false;   // Keep otherMode across DLists
        bool forceDepthClear = false;       // GoldenEye/PD: force depth-only clear
        bool tonyHawkBlendFix = false;      // Force SRC_ALPHA/1-SRC_ALPHA for blend 0x0150
        bool blastCorpsBlendFix = false;    // Force ZERO/ONE when tex off but combiner uses tex
        bool zeldaMonochrome = false;       // CI8-over-RGBA16 monochrome framebuffer effect
        bool zeldaSubscreen = false;        // Subscreen framebuffer timing delay fix
        bool paperMarioSubscreen = false;   // Blue channel +4 on FB copy to RDRAM
        bool conkerBlurPauseScreen = false; // FB→depth address pause-screen blur
        bool conkerFBTextureOffset = false; // Subtract 1 from FB texture coords
        bool conkerDepthCopySkip = false;   // Skip rect-based depth buffer copies
        bool starCraftBgSkip = false;       // Block FB ops when LUT is RGBA16
        bool snapForceCopy = false;         // Skip automatic aux buffer copies
        bool pdDepthCopyOnce = false;       // Only copy depth buffer once per frame
    } m_hacks;

private:
    // ─── Helpers ───
    
    /// Snapshot current tile descriptors and compute texture hashes for a draw call.
    /// Uses the RDP state at the time of call to capture tile0/tile1 info.
    void PopulateTextureInfo(DrawCall& call, int tileIndex);
    
    /// Build a Tex::TextureKey from a snapshotted TileDescriptor.
    Tex::TextureKey BuildTextureKey(const TileDescriptor& tile, uint32_t textureImageAddr) const;
};

} // namespace Nova64::Video
