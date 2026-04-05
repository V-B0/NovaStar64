/**
 * @file GBIHandler.hpp
 * @brief Abstract base for microcode-specific GBI command handlers
 *
 * Each microcode type dispatches GBI commands differently. This base class
 * defines the interface that all microcode handlers must implement. The
 * MicrocodeDispatcher routes display list commands to the appropriate handler.
 */

#pragma once

#include "Microcode.hpp"
#include "../Core/RDPState.hpp"
#include "../Core/RDPProcessor.hpp"
#include <cstdint>
#include <string>

namespace Nova64::Video {

// Forward declarations
class MicrocodeDispatcher;

// ═══════════════════════════════════════════════════════════════════════════
//  Display List Execution Context
// ═══════════════════════════════════════════════════════════════════════════

struct DListContext {
    const uint8_t*  rdram = nullptr;        // Main RDRAM (8MB)
    uint32_t        rdramSize = 0;
    RDPState*       state = nullptr;        // Current graphics state
    RDPProcessor*   rdpProcessor = nullptr; // RDP command processor
    MicrocodeDispatcher* dispatcher = nullptr; // For mid-frame microcode switching
    
    // Display list execution state
    uint32_t        pc = 0;                 // Current program counter (RDRAM address)
    bool            halted = false;         // True when EndDL reached
    
    // Display list stack (DL call nesting)
    static constexpr int MAX_DL_STACK = 18;
    struct DLStackEntry {
        uint32_t    returnAddr;
        bool        isBranch;   // true = branch (no return), false = call
    };
    DLStackEntry    dlStack[MAX_DL_STACK];
    int             dlStackDepth = 0;
    
    // Microcode info for current task
    MicrocodeInfo   microcodeInfo;
    
    // ─── Helper Methods ──────────────────────────────────────────────
    
    /// Read a 32-bit word from RDRAM (native endian — emulator stores RDRAM in host byte order)
    uint32_t ReadWord(uint32_t addr) const {
        addr &= 0x00FFFFFF; // Mask to physical
        if (addr + 4 > rdramSize) return 0;
        return *reinterpret_cast<const uint32_t*>(rdram + addr);
    }

    /// Read a 16-bit halfword from RDRAM (word-swapped storage: XOR addr with 2)
    uint16_t ReadHalf(uint32_t addr) const {
        addr &= 0x00FFFFFF;
        if (addr + 2 > rdramSize) return 0;
        return *reinterpret_cast<const uint16_t*>(rdram + (addr ^ 2));
    }

    /// Read a byte from RDRAM (word-swapped storage: XOR addr with 3)
    uint8_t ReadByte(uint32_t addr) const {
        addr &= 0x00FFFFFF;
        if (addr >= rdramSize) return 0;
        return rdram[addr ^ 3];
    }
    
    /// Read a big-endian 64-bit double word from RDRAM
    uint64_t ReadDWord(uint32_t addr) const {
        uint64_t hi = ReadWord(addr);
        uint64_t lo = ReadWord(addr + 4);
        return (hi << 32) | lo;
    }
    
    /// Read N bytes from RDRAM into a buffer
    bool ReadBytes(uint32_t addr, void* dst, uint32_t count) const {
        addr &= 0x00FFFFFF;
        if (addr + count > rdramSize) return false;
        std::memcpy(dst, rdram + addr, count);
        return true;
    }
    
    /// Resolve a segmented address using current segment table
    uint32_t ResolveAddress(uint32_t segAddr) const {
        if (state) return state->geometry.ResolveAddress(segAddr);
        return segAddr & 0x00FFFFFF;
    }
    
    /// Push DL address onto call stack
    bool PushDL(uint32_t returnAddr, bool isBranch) {
        if (dlStackDepth >= MAX_DL_STACK) return false;
        dlStack[dlStackDepth++] = { returnAddr, isBranch };
        return true;
    }
    
    /// Pop DL from call stack (returns false if stack empty)
    bool PopDL() {
        if (dlStackDepth <= 0) return false;
        --dlStackDepth;
        pc = dlStack[dlStackDepth].returnAddr;
        return true;
    }
    
    /// Advance PC by word count
    void AdvancePC(int words = 2) {
        pc += words * 4;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  GBI Command Handler Interface
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Abstract base class for microcode-specific command handlers.
 * 
 * Each microcode variant (F3D, F3DEX, F3DEX2, etc.) inherits from this
 * and implements command dispatch with its own opcode table. The handler
 * translates GBI commands into RDP state changes and draw calls.
 */
class GBIHandler {
public:
    virtual ~GBIHandler() = default;
    
    /// Initialize the handler with its capabilities
    virtual void Initialize(const MicrocodeInfo& info) = 0;
    
    /// Get the microcode type this handler supports
    virtual MicrocodeType GetType() const = 0;
    
    /// Get the GBI version
    virtual GBIVersion GetGBIVersion() const = 0;
    
    /// Get human-readable name
    virtual const char* GetName() const = 0;
    
    /**
     * Process a single GBI command from the display list.
     * @param ctx  Display list execution context
     * @param w0   First word of the command (opcode in high byte)
     * @param w1   Second word of the command
     * @return true to continue processing, false to halt
     */
    virtual bool ProcessCommand(DListContext& ctx, uint32_t w0, uint32_t w1) = 0;
    
    /**
     * Execute a full display list starting at the given address.
     * Default implementation reads w0/w1 pairs and calls ProcessCommand.
     * Handles DL call/branch/end stack.
     */
    virtual void ExecuteDisplayList(DListContext& ctx, uint32_t address);
    
    /**
     * Called before starting a new frame's display list processing.
     * Override to reset per-frame state.
     */
    virtual void OnFrameBegin(DListContext& ctx) { (void)ctx; }
    
    /**
     * Called after finishing a frame's display list processing.
     */
    virtual void OnFrameEnd(DListContext& ctx) { (void)ctx; }
    
    // ─── Common GBI Commands (shared across all microcodes) ──────────
    // These implement RDP commands (0xC0-0xFF) which are identical
    // regardless of microcode variant.
    
    void HandleRDPCommand(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    // ─── Statistics ──────────────────────────────────────────────────
    
    uint64_t GetCommandsProcessed() const { return m_commandsProcessed; }
    uint64_t GetTrianglesProcessed() const { return m_trianglesProcessed; }
    uint64_t GetVerticesLoaded() const { return m_verticesLoaded; }
    void ResetStats();
    
protected:
    // Track statistics
    uint64_t m_commandsProcessed = 0;
    uint64_t m_trianglesProcessed = 0;
    uint64_t m_verticesLoaded = 0;
    
    // ─── Common Helper Methods ───────────────────────────────────────
    
    /// Transform a batch of vertices through the full pipeline (MVP, clip, light, fog, texgen)
    void TransformVertexBatch(GeometryState& geo, int startIndex, int count);
    
    /// Load vertices from RDRAM into the vertex buffer
    void LoadVertices(DListContext& ctx, uint32_t address, int count, int startIndex);
    
    /// Load a 4x4 matrix from RDRAM (N64 format: 2 segments of 32 bytes)
    void LoadMatrix(DListContext& ctx, uint32_t address, bool push, bool load, bool projection);
    
    /// Set geometry mode bits
    void SetGeometryMode(DListContext& ctx, uint32_t clearBits, uint32_t setBits);
    
    /// Process G_TEXTURE command
    void SetTextureState(DListContext& ctx, uint32_t w0, uint32_t w1);
    
    /// Process G_MOVEWORD
    void MoveWord(DListContext& ctx, uint8_t index, uint16_t offset, uint32_t value);
    
    /// Process G_MOVEMEM (GBI1 style)
    void MoveMemGBI1(DListContext& ctx, uint8_t index, uint8_t size, uint32_t address);
    
    /// Process G_MOVEMEM (GBI2 style)
    void MoveMemGBI2(DListContext& ctx, uint8_t index, uint8_t size, uint8_t offset, uint32_t address);
    
    /// Process G_SETOTHERMODE_H
    void SetOtherModeH(DListContext& ctx, uint32_t shift, uint32_t length, uint32_t value);
    
    /// Process G_SETOTHERMODE_L
    void SetOtherModeL(DListContext& ctx, uint32_t shift, uint32_t length, uint32_t value);
    
    /// Draw a single triangle (vertex indices)
    void DrawTriangle(DListContext& ctx, int v0, int v1, int v2);
    
    /// Draw two triangles
    void DrawTriangle2(DListContext& ctx, int v0, int v1, int v2, int v3, int v4, int v5);
    
    /// Process G_DL (call/branch to sub-display list)
    void DisplayList(DListContext& ctx, uint32_t address, bool branch);
    
    /// Process G_ENDDL
    void EndDisplayList(DListContext& ctx);
    
    /// Process G_POPMTX
    void PopMatrix(DListContext& ctx, uint32_t param);
    
    /// Process G_MODIFYVTX
    void ModifyVertex(DListContext& ctx, uint8_t where, uint16_t vtxIndex, uint32_t value);
    
    /// Process G_CULLDL
    void CullDisplayList(DListContext& ctx, uint32_t startVtx, uint32_t endVtx);
    
    /// Process G_BRANCH_Z
    void BranchZ(DListContext& ctx, uint32_t vtxIndex, uint32_t zValue, uint32_t address);
    
    /// Process G_BRANCH_W (F3DZEX2 — branch on clip-space W)
    void BranchW(DListContext& ctx, uint32_t vtxIndex, uint32_t wValue, uint32_t address);
    
    /// Process G_LOAD_UCODE
    void LoadUcode(DListContext& ctx, uint32_t address, uint32_t dataAddress);
    
    /// Process G_RDPHALF_1 / G_RDPHALF_2
    uint32_t m_rdpHalf1 = 0;
    uint32_t m_rdpHalf2 = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  GBI Command Logging/Debug
// ═══════════════════════════════════════════════════════════════════════════

/// Get description of a GBI1 command
std::string DescribeGBI1Command(uint32_t w0, uint32_t w1);

/// Get description of a GBI2 command
std::string DescribeGBI2Command(uint32_t w0, uint32_t w1);

} // namespace Nova64::Video
