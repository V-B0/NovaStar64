/**
 * @file MicrocodeDispatcher.cpp
 * @brief Microcode dispatcher implementation
 */

#include "MicrocodeDispatcher.hpp"
#include <cstdio>
#include "F3D.hpp"
#include "F3DEX.hpp"
#include "F3DEX2.hpp"
#include "S2DEX.hpp"
#include "PerfectDark.hpp"
#include "ExtendedGBIHandlers.hpp"
#include "SpecializedMicrocodes.hpp"
#include "../Common/Logger.hpp"
#include <cassert>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════════

MicrocodeDispatcher::MicrocodeDispatcher() = default;
MicrocodeDispatcher::~MicrocodeDispatcher() { Shutdown(); }

// ═══════════════════════════════════════════════════════════════════════════
//  Initialization
// ═══════════════════════════════════════════════════════════════════════════

void MicrocodeDispatcher::Initialize(
    const uint8_t* rdram, uint32_t rdramSize,
    RDPState* state, RDPProcessor* rdpProcessor)
{
    m_context.rdram = rdram;
    m_context.rdramSize = rdramSize;
    m_context.state = state;
    m_context.rdpProcessor = rdpProcessor;
    m_context.dispatcher = this;
    m_context.halted = false;
    m_context.pc = 0;
    m_context.dlStackDepth = 0;
    
    m_activeHandler = nullptr;
    m_activeInfo = {};
    
    ResetStats();
    
    NOVA_LOG(LogLevel::Info, LogSubsystem::Core,
            "MicrocodeDispatcher initialized (RDRAM: {} MB)", rdramSize / (1024 * 1024));
}

void MicrocodeDispatcher::Shutdown() {
    m_activeHandler = nullptr;
    m_handlerCache.clear();
    
    NOVA_LOG(LogLevel::Info, LogSubsystem::Core,
            "MicrocodeDispatcher shut down (DLs processed: {})", m_stats.totalDLsProcessed);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Graphics Task Processing
// ═══════════════════════════════════════════════════════════════════════════

void MicrocodeDispatcher::ProcessGraphicsTask(
    uint32_t taskDataAddr,
    uint32_t ucodeAddr, uint32_t ucodeSize,
    uint32_t ucodeDataAddr, uint32_t ucodeDataSize,
    uint32_t dlAddr)
{
    (void)taskDataAddr;
    
    // Detect microcode from IMEM text
    ucodeAddr &= 0x00FFFFFF;
    ucodeDataAddr &= 0x00FFFFFF;
    
    const uint8_t* imemData = nullptr;
    const uint8_t* dmemData = nullptr;
    
    if (ucodeAddr + ucodeSize <= m_context.rdramSize) {
        imemData = m_context.rdram + ucodeAddr;
    }
    if (ucodeDataAddr + ucodeDataSize <= m_context.rdramSize) {
        dmemData = m_context.rdram + ucodeDataAddr;
    }
    
    if (!imemData) {
        NOVA_LOG(LogLevel::Error, LogSubsystem::Core,
                "Invalid microcode address: 0x{:08X}", ucodeAddr);
        return;
    }
    
    // Detect type
    DetectionResult detection = m_registry.Detect(
        imemData, ucodeSize, dmemData, ucodeDataSize);
    
    // Get full info
    MicrocodeInfo info = m_registry.GetInfo(
        detection.type, ucodeAddr, ucodeSize, ucodeDataAddr, ucodeDataSize);
    info.crc = detection.crc;
    info.name = detection.matchDetails;
    
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
            "Graphics task: ucode={} crc=0x{:08X} conf={:.0f}% dl=0x{:08X}",
            MicrocodeTypeName(detection.type), detection.crc,
            detection.confidence * 100.0f, dlAddr);
    
    // Activate handler
    ActivateHandler(detection.type, info);
    
    if (!m_activeHandler) {
        NOVA_LOG(LogLevel::Error, LogSubsystem::Core,
                "Failed to create handler for microcode type: {}",
                MicrocodeTypeName(detection.type));
        return;
    }
    
    // Set up context
    m_context.microcodeInfo = info;
    m_context.halted = false;
    m_context.dlStackDepth = 0;
    // Reset S2DEX status registers at the start of each RSP task.
    // On real N64 the CPU writes SP_STATUS signal bits before each DL
    // submission; our HLE approximates this by zeroing the four status
    // words so SELECT_DL deduplication is scoped to one task at a time.
    // This allows animated DLs to re-execute every VI frame (inter-task)
    // while still preventing intra-task DL cycles that would loop forever
    // and flood the GPU with draws (triggering Intel Iris Xe TDR).
    m_context.s2dexStatusRegs[0] = 0;
    m_context.s2dexStatusRegs[1] = 0;
    m_context.s2dexStatusRegs[2] = 0;
    m_context.s2dexStatusRegs[3] = 0;
    m_context.dlBranchVisited.clear();
    
    // Execute
    m_activeHandler->OnFrameBegin(m_context);
    m_activeHandler->ExecuteDisplayList(m_context, dlAddr);
    m_activeHandler->OnFrameEnd(m_context);
    
    // Update stats
    ++m_stats.totalDLsProcessed;
    m_stats.totalCommandsProcessed += m_activeHandler->GetCommandsProcessed();
    m_stats.totalTrianglesProcessed += m_activeHandler->GetTrianglesProcessed();
    m_stats.totalVerticesLoaded += m_activeHandler->GetVerticesLoaded();
    
    // Track per-type usage
    bool found = false;
    for (auto& usage : m_stats.usageByType) {
        if (usage.type == detection.type) {
            ++usage.dlCount;
            usage.commandCount += m_activeHandler->GetCommandsProcessed();
            found = true;
            break;
        }
    }
    if (!found) {
        m_stats.usageByType.push_back({
            detection.type, 1, m_activeHandler->GetCommandsProcessed()
        });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shadow HLE (RT geometry extraction, no RDP output)
// ═══════════════════════════════════════════════════════════════════════════

void MicrocodeDispatcher::RunShadow(
    uint32_t taskDataAddr,
    uint32_t ucodeAddr, uint32_t ucodeSize,
    uint32_t ucodeDataAddr, uint32_t ucodeDataSize,
    uint32_t dlAddr,
    RT::ISceneGeometryConsumer* consumer,
    uint8_t* rdramRW)
{
    if (!consumer) return;

    // Stash live context fields that shadow mode must override
    RDPProcessor* savedRdp      = m_context.rdpProcessor;
    RDPState*     savedState    = m_context.state;
    auto*         savedConsumer = m_context.geometryConsumer;
    uint8_t*      savedRdramRW  = m_context.rdramRW;

    // Reset isolated shadow state so each frame starts clean
    m_shadowState.Reset();
    // Propagate widescreen scale so shadow-mode vertices transform with the
    // same projection as the HLE fast path (needed for correct RT geometry).
    m_shadowState.geometry.widescreenScale =
        savedState ? savedState->geometry.widescreenScale : 1.0f;

    // Clear per-task dedup set so the same matrix address can be patched each frame
    m_context.shadowPatchedAddresses.clear();

    // Override: suppress RDP draw calls, redirect consumer, use shadow state
    m_context.rdpProcessor    = nullptr;
    m_context.state           = &m_shadowState;
    m_context.geometryConsumer = consumer;
    m_context.rdramRW         = rdramRW;

    ProcessGraphicsTask(taskDataAddr,
                        ucodeAddr, ucodeSize,
                        ucodeDataAddr, ucodeDataSize,
                        dlAddr);

    // Restore
    m_context.rdpProcessor    = savedRdp;
    m_context.state           = savedState;
    m_context.geometryConsumer = savedConsumer;
    m_context.rdramRW         = savedRdramRW;
}

void MicrocodeDispatcher::ProcessDisplayList(MicrocodeType type, uint32_t dlAddr) {
    MicrocodeInfo info = m_registry.GetInfo(type);
    ActivateHandler(type, info);
    
    if (!m_activeHandler) return;
    
    m_context.microcodeInfo = info;
    m_context.halted = false;
    m_context.dlStackDepth = 0;
    
    m_activeHandler->OnFrameBegin(m_context);
    m_activeHandler->ExecuteDisplayList(m_context, dlAddr);
    m_activeHandler->OnFrameEnd(m_context);
    
    ++m_stats.totalDLsProcessed;
    m_stats.totalCommandsProcessed += m_activeHandler->GetCommandsProcessed();
    m_stats.totalTrianglesProcessed += m_activeHandler->GetTrianglesProcessed();
    m_stats.totalVerticesLoaded += m_activeHandler->GetVerticesLoaded();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Mid-Frame Microcode Switch
// ═══════════════════════════════════════════════════════════════════════════

void MicrocodeDispatcher::HandleUcodeSwitch(
    uint32_t textAddr, uint32_t textSize,
    uint32_t dataAddr, uint32_t dataSize)
{
    textAddr &= 0x00FFFFFF;
    dataAddr &= 0x00FFFFFF;
    
    const uint8_t* imemData = nullptr;
    const uint8_t* dmemData = nullptr;
    
    if (textAddr + textSize <= m_context.rdramSize) {
        imemData = m_context.rdram + textAddr;
    }
    if (dataAddr + dataSize <= m_context.rdramSize) {
        dmemData = m_context.rdram + dataAddr;
    }
    
    if (!imemData) return;
    
    DetectionResult detection = m_registry.Detect(
        imemData, textSize, dmemData, dataSize);
    
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
            "HandleUcodeSwitch: textAddr=0x{:08X} textSize={} CRC=0x{:08X} detected={} method={} conf={:.2f}",
            textAddr, textSize, detection.crc,
            MicrocodeTypeName(detection.type),
            static_cast<int>(detection.method),
            detection.confidence);

    if (detection.type == m_activeInfo.type) {
        // Same microcode — no switch needed
        return;
    }
    
    NOVA_LOG(LogLevel::Debug, LogSubsystem::Core,
            "Mid-frame microcode switch: {} -> {} (CRC=0x{:08X}, method={}, conf={:.2f})",
            MicrocodeTypeName(m_activeInfo.type),
            MicrocodeTypeName(detection.type),
            detection.crc,
            static_cast<int>(detection.method),
            detection.confidence);
    
    MicrocodeInfo newInfo = m_registry.GetInfo(
        detection.type, textAddr, textSize, dataAddr, dataSize);
    newInfo.crc = detection.crc;
    
    // Flush current handler
    if (m_activeHandler) {
        m_activeHandler->OnFrameEnd(m_context);
    }
    
    // Switch to new handler
    ActivateHandler(detection.type, newInfo);
    m_activeInfo = newInfo;
    m_context.microcodeInfo = newInfo;
    
    if (m_activeHandler) {
        m_activeHandler->OnUcodeSwitch(m_context);
    }

    ++m_stats.microcodesSwitched;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Frame End
// ═══════════════════════════════════════════════════════════════════════════

void MicrocodeDispatcher::OnFrameEnd() {
    ++m_stats.framesProcessed;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Handler Management
// ═══════════════════════════════════════════════════════════════════════════

MicrocodeType MicrocodeDispatcher::GetCurrentType() const {
    return m_activeInfo.type;
}

const MicrocodeInfo& MicrocodeDispatcher::GetCurrentInfo() const {
    return m_activeInfo;
}

GBIHandler* MicrocodeDispatcher::GetHandler(MicrocodeType type) {
    auto it = m_handlerCache.find(type);
    if (it != m_handlerCache.end()) {
        return it->second.get();
    }
    
    GBIHandler* handler = CreateHandler(type);
    if (handler) {
        m_handlerCache[type] = std::unique_ptr<GBIHandler>(handler);
    }
    return handler;
}

GBIHandler* MicrocodeDispatcher::CreateHandler(MicrocodeType type) {
    switch (type) {
    case MicrocodeType::F3D:
        return new F3DHandler();
        
    case MicrocodeType::F3DEX:
    case MicrocodeType::F3DLX:
        return new F3DEXHandler();

    case MicrocodeType::F3DTEXA:
        return new F3DTEXAHandler();

    case MicrocodeType::F3DGOLDEN:
        return new F3DGOLDENHandler();
        
    case MicrocodeType::F3DEX2:
    case MicrocodeType::F3DLX2:
    case MicrocodeType::F3DFLX2:
        return new F3DEX2Handler();
        
    case MicrocodeType::F3DZEX:
        return new F3DZEXHandler();
        
    case MicrocodeType::F3DCBFD:
        return new F3DCBFDHandler();
        
    case MicrocodeType::F3DPD:
        return new F3DPDHandler();
        
    case MicrocodeType::S2DEX:
        return new S2DEXHandler();
        
    case MicrocodeType::S2DEX2:
        return new S2DEX2Handler();
        
    case MicrocodeType::Turbo3D:
        return new Turbo3DGBIHandler();
        
    case MicrocodeType::ZSort:
        return new ZSortGBIHandler();
        
    case MicrocodeType::DiddyKong:
        return new DKRGBIHandler();
        
    case MicrocodeType::WaveRace:
        return new WaveraceGBIHandler();
        
    case MicrocodeType::StarWars:
        return new StarWarsGBIHandler();

    case MicrocodeType::JetForce:
        return new JetForceHandler();

    case MicrocodeType::L3D:
        return new L3DGBIHandler();

    case MicrocodeType::L3DEX:
        return new L3DEXGBIHandler();

    case MicrocodeType::L3DEX2:
        return new L3DEX2GBIHandler();

    case MicrocodeType::F3DSETA:
        return new F3DSETAGBIHandler();

    case MicrocodeType::F3DEX2ACCLAIM:
        return new F3DEX2AcclaimGBIHandler();

    case MicrocodeType::F3DAM:
        return new F3DAMGBIHandler();

    case MicrocodeType::ZSortBOSS:
        return new ZSortBOSSGBIHandler();

    case MicrocodeType::F3DBETA:
        return new F3DBETAHandler();

    case MicrocodeType::F3DEX3:
        return new F3DEX3Handler();

    case MicrocodeType::F3DEX095:
        return new F3DEX095Handler();

    case MicrocodeType::F5Rogue:
        return new F5RogueHandler();

    case MicrocodeType::F5Indi_Naboo:
        return new F5IndiNabooHandler();

    case MicrocodeType::T3DUX:
        return new T3DUXHandler();
        
    default:
        // Default to F3DEX2 as most compatible fallback
        NOVA_LOG(LogLevel::Warning, LogSubsystem::Core,
                "No handler for microcode type {}, falling back to F3DEX2",
                MicrocodeTypeName(type));
        return new F3DEX2Handler();
    }
}

void MicrocodeDispatcher::ActivateHandler(MicrocodeType type, const MicrocodeInfo& info) {
    GBIHandler* handler = GetHandler(type);
    if (handler && handler != m_activeHandler) {
        handler->Initialize(info);
        m_activeHandler = handler;
        m_activeInfo = info;
    }
}

void MicrocodeDispatcher::ClearHandlerCache() {
    m_activeHandler = nullptr;
    m_handlerCache.clear();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Statistics
// ═══════════════════════════════════════════════════════════════════════════

void MicrocodeDispatcher::ResetStats() {
    m_stats = {};
}

} // namespace Nova64::Video
