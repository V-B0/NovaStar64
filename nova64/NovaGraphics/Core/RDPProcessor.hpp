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
#include "../MLE/MLEDirector.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
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

    // Default constructor: zero-init everything, then set non-zero defaults.
    // All initialization is in the body (no default member initializers) to
    // prevent MSVC from emitting post-body initialization code that can
    // overwrite a preceding memcpy in user-defined copy/move constructors.
    DrawCall() {
        std::memset(this, 0, sizeof(DrawCall));
        texCoordBounds0[2] = 1.0f; texCoordBounds0[3] = 1.0f;
        texCoordBounds1[2] = 1.0f; texCoordBounds1[3] = 1.0f;
    }
    DrawCall(const DrawCall& o)            { std::memcpy(this, &o, sizeof(DrawCall)); }
    DrawCall& operator=(const DrawCall& o) { if (this != &o) std::memcpy(this, &o, sizeof(DrawCall)); return *this; }
    DrawCall(DrawCall&& o)                 noexcept { std::memcpy(this, &o, sizeof(DrawCall)); }
    DrawCall& operator=(DrawCall&& o)      noexcept { if (this != &o) std::memcpy(this, &o, sizeof(DrawCall)); return *this; }
    
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

    // TMEM content CRC snapshotted at draw-queue time (not flush time).
    // This ensures each draw uses its own TMEM state even when multiple
    // draws share the same batch and subsequent loads overwrite TMEM.
    uint64_t snapshotCrc0;
    uint64_t snapshotCrc1;

    // Tile descriptor snapshots for texture binding at flush time
    TileDescriptor tileDesc0;
    TileDescriptor tileDesc1;
    uint32_t textureImageAddr;   // RDRAM address for tile0 at draw time
    uint32_t textureImageAddr1;  // RDRAM address for tile1 (2-cycle mode)

    // Vulkan FB-snapshot ring slot resolved AT ENQUEUE TIME (not flush time).
    // Multiple ring slots can share the same RDRAM address (e.g. MM's pause
    // menu issues several SetColorImage switches to the SAME physical FB
    // within one frame -- a full world redraw, then smaller UI-only overlay
    // passes onto that same target), so an address-only lookup at flush time
    // is ambiguous: it can't tell which of several same-address generations
    // this specific draw call actually intended to sample. Resolving here,
    // synchronously in DL-execution order, captures whichever slot was the
    // CURRENT snapshot for that address at the moment this draw was issued --
    // exactly what the RDP would have read on real hardware. -1 = unresolved
    // (flush time falls back to the address search for non-FB textures).
    int32_t snapshotSlot0;
    int32_t snapshotSlot1;

    // Color state snapshot — needed for correct per-draw-call uniforms
    RDPColor primColor;
    RDPColor envColor;
    RDPColor fogColor;
    RDPColor blendColor;
    float    primLODFrac;
    float    primLODMin;
    int      fogUsage;         // 0=none, 1=standard, 2=F3DAM RGB
    
    // Primitive depth (snapshot — SetPrimDepth can change between draws)
    uint16_t primDepth;
    uint16_t primDeltaZ;
    
    // Texture scrolling
    float scrollS0;
    float scrollT0;
    float scrollS1;
    float scrollT1;

    // Mipmap tile chain (for LOD textures — tiles baseTile+1 through baseTile+level)
    TileDescriptor mipTiles[7];   // Up to 7 additional mip levels
    int mipLevelCount;            // Number of mip levels beyond base (0 = no mipmapping)

    // Texture coordinate bounds (for TextureRect bilinear bleed prevention)
    float texCoordBounds0[4]; // minS, minT, maxS, maxT — defaults: 0,0,1,1
    float texCoordBounds1[4];
    bool  useTexCoordBounds;

    // Shadow map flag: texture read from the depth buffer for shadow overlay
    bool  isShadowMap;
    // Shadow TLUT snapshot (256 alpha values extracted from TMEM palette region)
    uint8_t shadowTLUT[256];
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
    
    // Append a draw call using resize+memcpy instead of push_back.
    // Works around MSVC codegen issue where push_back zeroes the
    // newly constructed element despite the copy constructor body
    // executing correctly.
    void AppendDrawCall(const DrawCall& call) {
        size_t idx = drawCalls.size();
        drawCalls.resize(idx + 1);
        std::memcpy(&drawCalls[idx], &call, sizeof(DrawCall));
    }

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
                         const RenderBatch::Vertex& rv2, uint32_t geometryMode,
                         uint8_t tileIndex = 0);
    
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

    /// Attach an MLEDirector to classify each batch before flushing.
    /// Pass nullptr to detach.
    void SetMLEDirector(MLEDirector* d) { m_mleDirector = d; }

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
    void HandleScrollTex(const uint32_t* words);

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
    static constexpr size_t RING_VBO_SIZE = RenderBatch::MAX_VERTICES * 64; // 64 = sizeof(FlushBatch local GPUVertex)

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
    std::unique_ptr<ITexture> m_noiseTexture;
    std::unique_ptr<ITexture> m_ditherTexture;      // N64 Magic Square (default)
    std::unique_ptr<ITexture> m_ditherBayerTexture; // N64 Bayer (mode 1)
    std::unique_ptr<ITexture> m_debugWhiteTexture;  // 1x1 white texture for diagnostics
    uint32_t m_shadowTLUTTexture = 0;  // 256×1 R8 — shadow map palette from TMEM

    // Vulkan framebuffer-as-texture snapshot.
    // When the inventory (or any draw) reads the previously-rendered N64 framebuffer
    // as a source texture, the GL path can directly sample the GL FBO color texture.
    // In the Vulkan path there is no per-N64-FB texture — all rendering goes to the
    // swapchain — so we snapshot the swapchain via ReadPixels and keep it here.
    //
    // Ring buffer, not just a double-buffer: two independent concerns share it.
    // (1) With kFramesInFlight=2 the GPU may still be sampling the previously-
    // bound snapshot when the CPU records a new capture, so writing into the
    // same VkImage would race -- each new capture flips the index FIRST then
    // writes, leaving the previously-bound slot untouched until its frame's
    // fence is signalled. (2) Multi-panel UI (MM's pause menu) can need SEVERAL
    // distinct FB addresses (world background + more than one peek panel)
    // alive at once, each looked up later by address match (see
    // RDPProcessor_Batch.cpp). Two slots is exactly enough for (1) but not (2):
    // a third concurrently-needed address gets evicted by round-robin before
    // its panel ever samples it, going black. Sized to 4 to give (2) headroom
    // without giving up the race safety from (1).
    static constexpr uint32_t kVkFBSnapshotSlots = 4;
    std::array<std::unique_ptr<ITexture>, kVkFBSnapshotSlots> m_vkFBSnapshots;
    std::array<uint32_t, kVkFBSnapshotSlots> m_vkFBSnapshotAddrs{};
    uint32_t m_vkFBSnapshotIdx = 0;
    
    // MLE heuristic router (optional — set via SetMLEDirector)
    MLEDirector* m_mleDirector = nullptr;

    // Debug
    bool        m_logCommands = false;
    std::string m_lastCommandDesc;
    int         m_debugOutputMode = 0; // 0=normal, 1=tex0, 2=shade, 3=prim, 4=env, 5=UV, 6=depth, 7=alpha
    static constexpr int kDebugModeCount = 8;
    
    // Diagnostic debug toggles (runtime, toggled via hotkeys)
    bool m_debugForceOpaque    = false; // Force alpha=1 on all draws
    bool m_debugDisableDepth   = false; // Disable depth test/write
    bool m_debugDisableCulling = false; // Disable backface culling in RSP
    bool m_debugMagenta        = false; // Render all geometry as magenta
    
    bool m_initialized = false;

    // Callback fired on each SyncFull, before FlushBatch.
    // Used to deliver the DP interrupt to the CPU at the correct time.
    SyncFullCallback m_syncFullCallback;

public:
    // ─── Debug output mode ───
    int  GetDebugOutputMode() const { return m_debugOutputMode; }
    void SetDebugOutputMode(int mode) { m_debugOutputMode = (mode >= 0 && mode < kDebugModeCount) ? mode : 0; }
    void CycleDebugOutputMode() { m_debugOutputMode = (m_debugOutputMode + 1) % kDebugModeCount; }

    // ─── Diagnostic debug toggles ───
    void ToggleDebugForceOpaque()    { m_debugForceOpaque    = !m_debugForceOpaque; }
    void ToggleDebugDisableDepth()   { m_debugDisableDepth   = !m_debugDisableDepth; }
    void ToggleDebugDisableCulling() { m_debugDisableCulling = !m_debugDisableCulling; }
    void ToggleDebugMagenta()        { m_debugMagenta        = !m_debugMagenta; }
    bool IsDebugForceOpaque()    const { return m_debugForceOpaque; }
    bool IsDebugDisableDepth()   const { return m_debugDisableDepth; }
    bool IsDebugDisableCulling() const { return m_debugDisableCulling; }
    bool IsDebugMagenta()        const { return m_debugMagenta; }

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
        // Texture rectangles that sample the depth buffer's address, chosen per
        // game from the ROM name as GLideN64's _setSpecialTexrect does.
        bool texRectShadowMap = false;        // Beetle/HSV Adventure Racing, Duck Dodgers
        bool texRectDepthBufferCopy = false;  // Perfect Dark, Turok: 1-row depth copies
    } m_hacks;

private:
    // ─── Helpers ───
    
    /// Snapshot current tile descriptors and compute texture hashes for a draw call.
    /// Uses the RDP state at the time of call to capture tile0/tile1 info.
    void PopulateTextureInfo(DrawCall& call, int tileIndex);
    
    /// Build a Tex::TextureKey from a snapshotted TileDescriptor.
    /// Pass overrideCrc != 0 to skip the TMEM read and use the supplied CRC directly
    /// (used when a CRC was captured at draw-queue time rather than flush time).
    Tex::TextureKey BuildTextureKey(const TileDescriptor& tile, uint32_t textureImageAddr,
                                    uint8_t tlutType = 0,
                                    const TileDescriptor* mipTiles = nullptr,
                                    int mipCount = 0,
                                    uint64_t overrideCrc = 0) const;
};

} // namespace Nova64::Video
