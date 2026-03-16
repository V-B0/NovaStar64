/**
 * @file DisplayCtrlReg.cpp
 * @brief RDP (Reality Display Processor) Control Register Handler
 * 
 * Implements the DPC (Display Processor Command) registers that control the RDP.
 * Memory region: 0x04100000-0x041FFFFF
 * 
 * The RDP processes display lists containing graphics commands and outputs pixels.
 */

#include "DisplayCtrlReg.hpp"
#include "../MIPSY/Register.hpp"
#include "../Plugins/GFXPlugin.hpp"
#include "../../common/log.hpp"
#include <cstring>

// ============================================================================
// Constructor
// ============================================================================

DisplayProcessorHandler::DisplayProcessorHandler(CRegisters& reg, Plugins::GFXPlugin* gfxPlugin) :
    m_Reg(reg),
    m_GfxPlugin(gfxPlugin),
    m_Registers(reg.m_Display_ControlReg.data()),
    m_ProcessingDList(false),
    m_DListStart(0),
    m_DListEnd(0)
{
    Log::info("DisplayProcessorHandler: Initialized");
}

// ============================================================================
// Read Operations
// ============================================================================

bool DisplayProcessorHandler::Read32(uint32_t addr, uint32_t& val)
{
    using namespace DPCRegisters;
    
    switch (addr & 0x000FFFFF) {
        case 0x00000:  // DPC_START_REG
            val = m_Registers[DPC_START];
            break;
            
        case 0x00004:  // DPC_END_REG
            val = m_Registers[DPC_END];
            break;
            
        case 0x00008:  // DPC_CURRENT_REG
            val = m_Registers[DPC_CURRENT];
            break;
            
        case 0x0000C:  // DPC_STATUS_REG
            val = m_Registers[DPC_STATUS];
            break;
            
        case 0x00010:  // DPC_CLOCK_REG
            val = m_Registers[DPC_CLOCK];
            break;
            
        case 0x00014:  // DPC_BUFBUSY_REG
            val = m_Registers[DPC_BUFBUSY];
            break;
            
        case 0x00018:  // DPC_PIPEBUSY_REG
            val = m_Registers[DPC_PIPEBUSY];
            break;
            
        case 0x0001C:  // DPC_TMEM_REG
            val = m_Registers[DPC_TMEM];
            break;
            
        default:
            val = 0;
            Log::warningf(
                "DisplayProcessorHandler: Read from unhandled register at 0x%08X", addr);
            return false;
    }
    
    Log::debugf("DisplayProcessorHandler: Read register[0x%08X] = 0x%08X", addr, val);
    return true;
}

// ============================================================================
// Write Operations
// ============================================================================

bool DisplayProcessorHandler::Write32(uint32_t addr, uint32_t val, uint32_t mask)
{
    using namespace DPCRegisters;
    
    val &= mask;
    
    switch (addr & 0x000FFFFF) {
        case 0x00000:  // DPC_START_REG
            Log::debugf("DisplayProcessorHandler: DPC_START = 0x%08X", val);
            m_Registers[DPC_START] = val & 0x00FFFFF8;  // 24-bit, 8-byte aligned
            m_Registers[DPC_CURRENT] = m_Registers[DPC_START];
            m_DListStart = m_Registers[DPC_START];
            break;
            
        case 0x00004:  // DPC_END_REG
            Log::debugf("DisplayProcessorHandler: DPC_END = 0x%08X", val);
            m_Registers[DPC_END] = val & 0x00FFFFF8;  // 24-bit, 8-byte aligned
            m_DListEnd = m_Registers[DPC_END];
            
            // Trigger display list processing
            if (m_Registers[DPC_END] != m_Registers[DPC_START]) {
                ProcessDisplayList();
            }
            break;
            
        case 0x0000C:  // DPC_STATUS_REG (write-only, command register)
            Log::debugf("DisplayProcessorHandler: DPC_STATUS = 0x%08X", val);
            ProcessStatusCommand(val);
            break;
            
        default:
            Log::warningf(
                "DisplayProcessorHandler: Write to unhandled/read-only register at 0x%08X = 0x%08X", addr, val);
            return false;
    }
    
    return true;
}

// ============================================================================
// Status Command Processing
// ============================================================================

void DisplayProcessorHandler::ProcessStatusCommand(uint32_t cmd)
{
    using namespace DPCRegisters;
    
    uint32_t& status = m_Registers[DPC_STATUS];
    
    // Clear XBUS DMEM DMA
    if (isCommandSet(cmd, DPCCommand::CLR_XBUS_DMEM_DMA)) {
        status &= ~static_cast<uint32_t>(DPCStatus::XBUS_DMEM_DMA);
        Log::debugf("DisplayProcessorHandler: Clear XBUS DMEM DMA");
    }
    
    // Set XBUS DMEM DMA
    if (isCommandSet(cmd, DPCCommand::SET_XBUS_DMEM_DMA)) {
        status |= static_cast<uint32_t>(DPCStatus::XBUS_DMEM_DMA);
        Log::debugf("DisplayProcessorHandler: Set XBUS DMEM DMA");
    }
    
    // Clear Freeze
    if (isCommandSet(cmd, DPCCommand::CLR_FREEZE)) {
        status &= ~static_cast<uint32_t>(DPCStatus::FREEZE);
        Log::debugf("DisplayProcessorHandler: Clear Freeze");
    }
    
    // Set Freeze
    if (isCommandSet(cmd, DPCCommand::SET_FREEZE)) {
        status |= static_cast<uint32_t>(DPCStatus::FREEZE);
        Log::debugf("DisplayProcessorHandler: Set Freeze");
    }
    
    // Clear Flush
    if (isCommandSet(cmd, DPCCommand::CLR_FLUSH)) {
        status &= ~static_cast<uint32_t>(DPCStatus::FLUSH);
        Log::debugf("DisplayProcessorHandler: Clear Flush");
    }
    
    // Set Flush
    if (isCommandSet(cmd, DPCCommand::SET_FLUSH)) {
        status |= static_cast<uint32_t>(DPCStatus::FLUSH);
        Log::debugf("DisplayProcessorHandler: Set Flush");
        
        // Flush triggers immediate processing
        if (m_GfxPlugin) {
            m_GfxPlugin->UpdateRDPStatus();
        }
    }
    
    // Clear TMEM counter
    if (isCommandSet(cmd, DPCCommand::CLR_TMEM_CTR)) {
        m_Registers[DPC_TMEM] = 0;
        Log::debugf("DisplayProcessorHandler: Clear TMEM counter");
    }
    
    // Clear Pipe counter
    if (isCommandSet(cmd, DPCCommand::CLR_PIPE_CTR)) {
        m_Registers[DPC_PIPEBUSY] = 0;
        Log::debugf("DisplayProcessorHandler: Clear Pipe counter");
    }
    
    // Clear CMD counter
    if (isCommandSet(cmd, DPCCommand::CLR_CMD_CTR)) {
        m_Registers[DPC_BUFBUSY] = 0;
        Log::debugf("DisplayProcessorHandler: Clear CMD counter");
    }
    
    // Clear Clock counter
    if (isCommandSet(cmd, DPCCommand::CLR_CLOCK_CTR)) {
        m_Registers[DPC_CLOCK] = 0;
        Log::debugf("DisplayProcessorHandler: Clear Clock counter");
    }
}

// ============================================================================
// Display List Processing
// ============================================================================

void DisplayProcessorHandler::ProcessDisplayList()
{
    using namespace DPCRegisters;
    
    if (m_ProcessingDList) {
        Log::warningf("DisplayProcessorHandler: Already processing display list");
        return;
    }
    
    uint32_t start = m_Registers[DPC_START];
    uint32_t end = m_Registers[DPC_END];
    
    if (start >= end) {
        Log::warningf("DisplayProcessorHandler: Invalid display list range: 0x%08X - 0x%08X", start, end);
        return;
    }
    
    uint32_t length = end - start;
    
    Log::debugf("DisplayProcessorHandler: Processing display list: 0x%08X - 0x%08X (length=0x%X)",
        start, end, length);
    
    // Set busy flags
    m_Registers[DPC_STATUS] |= static_cast<uint32_t>(DPCStatus::PIPE_BUSY);
    m_Registers[DPC_STATUS] |= static_cast<uint32_t>(DPCStatus::CMD_BUSY);
    m_Registers[DPC_STATUS] |= static_cast<uint32_t>(DPCStatus::DMA_BUSY);
    m_Registers[DPC_STATUS] |= static_cast<uint32_t>(DPCStatus::START_VALID);
    m_ProcessingDList = true;
    
    // Process with graphics plugin
    if (m_GfxPlugin) {
        m_GfxPlugin->ProcessDList(start, end);
    } else {
        Log::warning("DisplayProcessorHandler: No graphics plugin available");
    }
    
    // Update current register to end
    m_Registers[DPC_CURRENT] = end;
    
    // Clear busy flags
    m_Registers[DPC_STATUS] &= ~static_cast<uint32_t>(DPCStatus::PIPE_BUSY);
    m_Registers[DPC_STATUS] &= ~static_cast<uint32_t>(DPCStatus::CMD_BUSY);
    m_Registers[DPC_STATUS] &= ~static_cast<uint32_t>(DPCStatus::DMA_BUSY);
    m_Registers[DPC_STATUS] |= static_cast<uint32_t>(DPCStatus::CBUF_READY);
    m_Registers[DPC_STATUS] |= static_cast<uint32_t>(DPCStatus::END_VALID);
    
    m_ProcessingDList = false;
    
    // Increment counters
    m_Registers[DPC_CLOCK]++;
    m_Registers[DPC_BUFBUSY]++;
    
    // Signal DP interrupt through MI
    m_Reg.m_GfxIntrReg |= 0x20;  // MI_INTR_DP
    m_Reg.m_Mips_Interface[2] |= 0x20;
    m_Reg.CheckInterrupts();
    
    Log::debug("DisplayProcessorHandler: Display list processing complete");
}

// ============================================================================
// Plugin Management
// ============================================================================

void DisplayProcessorHandler::SetGraphicsPlugin(Plugins::GFXPlugin* plugin)
{
    m_GfxPlugin = plugin;
    
    if (plugin) {
        Log::infof("DisplayProcessorHandler: Graphics plugin connected: %s",
            plugin->GetPluginInfo().Name.c_str());
    } else {
        Log::warning("DisplayProcessorHandler: Graphics plugin disconnected");
    }
}

Plugins::GFXPlugin* DisplayProcessorHandler::GetGraphicsPlugin() const
{
    return m_GfxPlugin;
}

// ============================================================================
// Status Queries
// ============================================================================

bool DisplayProcessorHandler::IsProcessing() const
{
    return m_ProcessingDList;
}

bool DisplayProcessorHandler::IsFrozen() const
{
    using namespace DPCRegisters;
    return isStatusSet(m_Registers[DPC_STATUS], DPCStatus::FREEZE);
}

bool DisplayProcessorHandler::IsFlushed() const
{
    using namespace DPCRegisters;
    return isStatusSet(m_Registers[DPC_STATUS], DPCStatus::FLUSH);
}

uint32_t DisplayProcessorHandler::GetStatus() const
{
    using namespace DPCRegisters;
    return m_Registers[DPC_STATUS];
}

// ============================================================================
// Reset
// ============================================================================

void DisplayProcessorHandler::Reset()
{
    using namespace DPCRegisters;
    
    Log::info("DisplayProcessorHandler: Reset");
    
    // Clear all registers
    std::memset(m_Registers, 0, sizeof(uint32_t) * REGISTER_COUNT);
    
    // Set initial status (ready, not busy)
    m_Registers[DPC_STATUS] = static_cast<uint32_t>(DPCStatus::CBUF_READY);
    
    m_ProcessingDList = false;
    m_DListStart = 0;
    m_DListEnd = 0;
}
