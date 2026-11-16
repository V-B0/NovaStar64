/**
 * @file MicrocodeDispatcher.hpp
 * @brief Dynamic microcode dispatch and management
 *
 * The dispatcher is the central hub that routes RSP graphics tasks to the
 * appropriate microcode handler. It manages detection, handler creation,
 * mid-frame microcode switching (G_LOAD_UCODE), and provides statistics.
 */

#pragma once

#include "Microcode.hpp"
#include "MicrocodeRegistry.hpp"
#include "GBIHandler.hpp"
#include <memory>
#include <unordered_map>

namespace Nova64::Video {

class MicrocodeDispatcher {
public:
    MicrocodeDispatcher();
    ~MicrocodeDispatcher();

    /**
     * Initialize the dispatcher with RDRAM access.
     * Must be called before ProcessDisplayList.
     */
    void Initialize(const uint8_t* rdram, uint32_t rdramSize, RDPState* state, RDPProcessor* rdpProcessor);

    /**
     * Shut down and release all handlers.
     */
    void Shutdown();

    /**
     * Process a complete graphics task (display list from RSP task header).
     * Detects microcode type from task data, selects appropriate handler,
     * and executes the display list.
     *
     * @param taskDataAddr  RDRAM address of task data (used for microcode detection)
     * @param ucodeAddr     RDRAM address of microcode text (IMEM)
     * @param ucodeSize     Size of microcode text
     * @param ucodeDataAddr RDRAM address of microcode data (DMEM)
     * @param ucodeDataSize Size of microcode data
     * @param dlAddr        RDRAM address of display list start
     */
    void ProcessGraphicsTask(uint32_t taskDataAddr, uint32_t ucodeAddr, uint32_t ucodeSize, uint32_t ucodeDataAddr,
                             uint32_t ucodeDataSize, uint32_t dlAddr);

    /**
     * Process a display list with explicitly specified microcode type.
     */
    void ProcessDisplayList(MicrocodeType type, uint32_t dlAddr);

    /**
     * Handle mid-frame microcode switch (G_LOAD_UCODE).
     * Re-detects and switches handler, then continues at currentPC.
     */
    void HandleUcodeSwitch(uint32_t textAddr, uint32_t textSize, uint32_t dataAddr, uint32_t dataSize);

    /**
     * Run the display list in shadow mode: extract RT world-space geometry
     * without submitting any RDP commands.  Safe to call concurrently with
     * the LLE RSP interpreter because rdpProcessor is nulled out internally.
     *
     * @param taskDataAddr   RDRAM address of task data (for ucode detection)
     * @param ucodeAddr      RDRAM address of microcode text (IMEM)
     * @param ucodeSize      Size of microcode text
     * @param ucodeDataAddr  RDRAM address of microcode data (DMEM)
     * @param ucodeDataSize  Size of microcode data
     * @param dlAddr         RDRAM address of display list start
     * @param consumer       RT geometry consumer to receive triangles
     * @param rdramRW        Writable RDRAM pointer for widescreen matrix write-back.
     *                       Pass nullptr (default) when write-back is not needed.
     */
    void RunShadow(uint32_t taskDataAddr, uint32_t ucodeAddr, uint32_t ucodeSize, uint32_t ucodeDataAddr,
                   uint32_t ucodeDataSize, uint32_t dlAddr, RT::ISceneGeometryConsumer* consumer,
                   uint8_t* rdramRW = nullptr);

    /**
     * Called at end of frame — flush pending work, update stats.
     */
    void OnFrameEnd();

    // --- Registry Access ---

    MicrocodeRegistry& GetRegistry() { return m_registry; }
    const MicrocodeRegistry& GetRegistry() const { return m_registry; }

    // --- Current State ---

    GBIHandler* GetCurrentHandler() const { return m_activeHandler; }

    /// Set the RT scene geometry consumer on the shared DListContext.
    /// Pass nullptr to detach.
    void SetGeometryConsumer(RT::ISceneGeometryConsumer* consumer) { m_context.geometryConsumer = consumer; }

    // --- Handler Cache ---

    /// Get or create a handler for the given microcode type
    GBIHandler* GetHandler(MicrocodeType type);

    /// Clear handler cache (e.g., on ROM change)
    void ClearHandlerCache();

    // --- Statistics ---

    struct DispatcherStats {
        uint64_t totalDLsProcessed = 0;
        uint64_t totalCommandsProcessed = 0;
        uint64_t totalTrianglesProcessed = 0;
        uint64_t totalVerticesLoaded = 0;
        uint64_t microcodesSwitched = 0;
        uint64_t framesProcessed = 0;

        // Per-microcode counters
        struct MicrocodeUsage {
            MicrocodeType type;
            uint32_t dlCount;
            uint64_t commandCount;
        };
        std::vector<MicrocodeUsage> usageByType;
    };

    const DispatcherStats& GetStats() const { return m_stats; }
    void ResetStats();

private:
    GBIHandler* CreateHandler(MicrocodeType type);
    void ActivateHandler(MicrocodeType type, const MicrocodeInfo& info);

    // Handler registry + factory cache
    MicrocodeRegistry m_registry;
    std::unordered_map<MicrocodeType, std::unique_ptr<GBIHandler>> m_handlerCache;

    // Currently active handler
    GBIHandler* m_activeHandler = nullptr;
    MicrocodeInfo m_activeInfo;

    // Execution context (reused per frame)
    DListContext m_context;

    // Isolated RDPState used during RunShadow() so the shadow HLE run
    // cannot corrupt the live LLE RDPState.
    RDPState m_shadowState;

    DispatcherStats m_stats;
};

} // namespace Nova64::Video
