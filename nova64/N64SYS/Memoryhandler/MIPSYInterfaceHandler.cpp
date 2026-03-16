/**
 * @file MIPSYinterfacehandler.cpp
 * @brief MIPS Interface (MI) Register Handler Implementation
 * 
 * Handles the MIPS Interface registers which control:
 * - System initialization mode
 * - Interrupt routing and masking
 * - RDRAM and EBUS configuration
 * 
 * Memory region: 0x04300000-0x0430000F
 * 
 * Register map:
 * 0x04300000 - MI_INIT_MODE_REG / MI_MODE_REG (Read/Write)
 * 0x04300004 - MI_VERSION_REG / MI_NOOP_REG (Read only)
 * 0x04300008 - MI_INTR_REG (Read only - interrupt status)
 * 0x0430000C - MI_INTR_MASK_REG (Write only - interrupt mask control)
 */

#include "MIPSYInterfaceHandler.hpp"
#include "../MIPSY/Register.hpp"
#include "../../common/log.hpp"
#include <cstdarg>
#include <cstdio>

// ============================================================================
// MI Mode Register Bits
// ============================================================================

namespace MIMode {
    constexpr uint32_t INIT          = 0x0080;  // Init mode
    constexpr uint32_t EBUS          = 0x0100;  // EBUS test mode
    constexpr uint32_t RDRAM         = 0x0200;  // RDRAM reg mode
    
    // Control bits (write only)
    constexpr uint32_t CLR_INIT      = 0x0080;  // Clear init mode
    constexpr uint32_t SET_INIT      = 0x0100;  // Set init mode
    constexpr uint32_t CLR_EBUS      = 0x0200;  // Clear EBUS
    constexpr uint32_t SET_EBUS      = 0x0400;  // Set EBUS
    constexpr uint32_t CLR_DP_INTR   = 0x0800;  // Clear DP interrupt
    constexpr uint32_t CLR_RDRAM     = 0x1000;  // Clear RDRAM reg mode
    constexpr uint32_t SET_RDRAM     = 0x2000;  // Set RDRAM reg mode
}

// ============================================================================
// MI Interrupt Register Bits
// ============================================================================

namespace MIIntr {
    constexpr uint32_t SP            = 0x01;    // SP interrupt
    constexpr uint32_t SI            = 0x02;    // SI interrupt
    constexpr uint32_t AI            = 0x04;    // AI interrupt
    constexpr uint32_t VI            = 0x08;    // VI interrupt
    constexpr uint32_t PI            = 0x10;    // PI interrupt
    constexpr uint32_t DP            = 0x20;    // DP interrupt
}

// ============================================================================
// MI Interrupt Mask Register Bits
// ============================================================================

namespace MIMask {
    constexpr uint32_t SP            = 0x01;    // SP interrupt mask
    constexpr uint32_t SI            = 0x02;    // SI interrupt mask
    constexpr uint32_t AI            = 0x04;    // AI interrupt mask
    constexpr uint32_t VI            = 0x08;    // VI interrupt mask
    constexpr uint32_t PI            = 0x10;    // PI interrupt mask
    constexpr uint32_t DP            = 0x20;    // DP interrupt mask
    
    // Control bits (write only)
    constexpr uint32_t CLR_SP        = 0x0001;  // Clear SP mask
    constexpr uint32_t SET_SP        = 0x0002;  // Set SP mask
    constexpr uint32_t CLR_SI        = 0x0004;  // Clear SI mask
    constexpr uint32_t SET_SI        = 0x0008;  // Set SI mask
    constexpr uint32_t CLR_AI        = 0x0010;  // Clear AI mask
    constexpr uint32_t SET_AI        = 0x0020;  // Set AI mask
    constexpr uint32_t CLR_VI        = 0x0040;  // Clear VI mask
    constexpr uint32_t SET_VI        = 0x0080;  // Set VI mask
    constexpr uint32_t CLR_PI        = 0x0100;  // Clear PI mask
    constexpr uint32_t SET_PI        = 0x0200;  // Set PI mask
    constexpr uint32_t CLR_DP        = 0x0400;  // Clear DP mask
    constexpr uint32_t SET_DP        = 0x0800;  // Set DP mask
}

// ============================================================================
// Constructor
// ============================================================================

MIPSYInterfaceHandler::MIPSYInterfaceHandler(CRegisters& reg) :
    MIPSYInterface(
        &reg.m_Mips_Interface[0],  // MI_INIT_MODE_REG / MI_MODE_REG
        &reg.m_Mips_Interface[0],  // MI_MODE_REG (same as above)
        &reg.m_Mips_Interface[1],  // MI_VERSION_REG
        &reg.m_Mips_Interface[1],  // MI_NOOP_REG (same as above)
        &reg.m_Mips_Interface[2],  // MI_INTR_REG
        &reg.m_Mips_Interface[3]   // MI_INTR_MASK_REG
    ),
    m_Reg(reg),
    m_PC(reg.m_PROGRAM_COUNTER)
{
    WriteLog(LogLevel::Info, "MIPSYInterfaceHandler: Initialized");
    
    // Set initial version (0x02020102 is typical)
    *MI_VERSION_REG = 0x02020102;
    
    // Clear interrupt state
    *MI_INTR_REG = 0;
    *MI_INTR_MASK_REG = 0;
    
    // Set default mode
    *MI_MODE_REG = 0;
}

// ============================================================================
// Read Operations
// ============================================================================

bool MIPSYInterfaceHandler::Read32(uint32_t addr, uint32_t& val)
{
    switch (addr & 0x1FFFFFFF) {
        case 0x04300000:  // MI_MODE_REG
            val = *MI_MODE_REG;
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Read MI_MODE_REG = 0x%08X", m_PC, val);
            break;
            
        case 0x04300004:  // MI_VERSION_REG
            val = *MI_VERSION_REG;
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Read MI_VERSION_REG = 0x%08X", m_PC, val);
            break;
            
        case 0x04300008:  // MI_INTR_REG
            val = *MI_INTR_REG;
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Read MI_INTR_REG = 0x%08X", m_PC, val);
            break;
            
        case 0x0430000C:  // MI_INTR_MASK_REG
            val = *MI_INTR_MASK_REG;
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Read MI_INTR_MASK_REG = 0x%08X", m_PC, val);
            break;
            
        default:
            val = 0;
            WriteLog(LogLevel::Warning, "MIPSYInterface[%016llX]: Read from unmapped register 0x%08X", m_PC, addr);
            return false;
    }
    
    return true;
}

// ============================================================================
// Write Operations
// ============================================================================

bool MIPSYInterfaceHandler::Write32(uint32_t addr, uint32_t val, uint32_t mask)
{
    uint32_t maskedVal = val & mask;
    
    switch (addr & 0x1FFFFFFF) {
        case 0x04300000:  // MI_MODE_REG
            WriteLog(LogLevel::Debug, "MIPSYInterface[%016llX]: Write MI_MODE_REG = 0x%08X (mask=0x%08X)", 
                m_PC, val, mask);
            ProcessModeWrite(maskedVal);
            break;
            
        case 0x04300004:  // MI_VERSION_REG / MI_NOOP_REG (read-only, writes ignored)
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Write to MI_VERSION_REG ignored (read-only)", m_PC);
            break;
            
        case 0x04300008:  // MI_INTR_REG (read-only, writes ignored)
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Write to MI_INTR_REG ignored (read-only)", m_PC);
            break;
            
        case 0x0430000C:  // MI_INTR_MASK_REG
            WriteLog(LogLevel::Trace, "MIPSYInterface[%016llX]: Write MI_INTR_MASK_REG = 0x%08X (mask=0x%08X)", 
                m_PC, val, mask);
            ProcessMaskWrite(maskedVal);
            break;
            
        default:
            WriteLog(LogLevel::Warning, "MIPSYInterface[%016llX]: Write to unmapped register 0x%08X = 0x%08X", 
                m_PC, addr, val);
            return false;
    }
    
    return true;
}

// ============================================================================
// Mode Register Processing
// ============================================================================

void MIPSYInterfaceHandler::ProcessModeWrite(uint32_t val)
{
    // Update mode bits directly (lower 7 bits)
    *MI_MODE_REG = (*MI_MODE_REG & ~0x7F) | (val & 0x7F);
    
    // Process control bits
    if (val & MIMode::CLR_INIT) {
        *MI_MODE_REG &= ~MIMode::INIT;
        WriteLog(LogLevel::Debug, "MIPSYInterface: Clear INIT mode");
    }
    
    if (val & MIMode::SET_INIT) {
        *MI_MODE_REG |= MIMode::INIT;
        WriteLog(LogLevel::Debug, "MIPSYInterface: Set INIT mode");
    }
    
    if (val & MIMode::CLR_EBUS) {
        *MI_MODE_REG &= ~MIMode::EBUS;
        WriteLog(LogLevel::Debug, "MIPSYInterface: Clear EBUS test mode");
    }
    
    if (val & MIMode::SET_EBUS) {
        *MI_MODE_REG |= MIMode::EBUS;
        WriteLog(LogLevel::Debug, "MIPSYInterface: Set EBUS test mode");
    }
    
    if (val & MIMode::CLR_DP_INTR) {
        ClearInterrupt(MIIntr::DP);
        WriteLog(LogLevel::Debug, "MIPSYInterface: Clear DP interrupt");
    }
    
    if (val & MIMode::CLR_RDRAM) {
        *MI_MODE_REG &= ~MIMode::RDRAM;
        WriteLog(LogLevel::Debug, "MIPSYInterface: Clear RDRAM reg mode");
    }
    
    if (val & MIMode::SET_RDRAM) {
        *MI_MODE_REG |= MIMode::RDRAM;
        WriteLog(LogLevel::Debug, "MIPSYInterface: Set RDRAM reg mode");
    }
}

// ============================================================================
// Interrupt Mask Register Processing
// ============================================================================

void MIPSYInterfaceHandler::ProcessMaskWrite(uint32_t val)
{
    // Process each interrupt mask control bit
    if (val & MIMask::CLR_SP) {
        *MI_INTR_MASK_REG &= ~MIMask::SP;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Clear SP interrupt mask");
    }
    if (val & MIMask::SET_SP) {
        *MI_INTR_MASK_REG |= MIMask::SP;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Set SP interrupt mask");
    }
    
    if (val & MIMask::CLR_SI) {
        *MI_INTR_MASK_REG &= ~MIMask::SI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Clear SI interrupt mask");
    }
    if (val & MIMask::SET_SI) {
        *MI_INTR_MASK_REG |= MIMask::SI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Set SI interrupt mask");
    }
    
    if (val & MIMask::CLR_AI) {
        *MI_INTR_MASK_REG &= ~MIMask::AI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Clear AI interrupt mask");
    }
    if (val & MIMask::SET_AI) {
        *MI_INTR_MASK_REG |= MIMask::AI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Set AI interrupt mask");
    }
    
    if (val & MIMask::CLR_VI) {
        *MI_INTR_MASK_REG &= ~MIMask::VI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Clear VI interrupt mask");
    }
    if (val & MIMask::SET_VI) {
        *MI_INTR_MASK_REG |= MIMask::VI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Set VI interrupt mask");
    }
    
    if (val & MIMask::CLR_PI) {
        *MI_INTR_MASK_REG &= ~MIMask::PI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Clear PI interrupt mask");
    }
    if (val & MIMask::SET_PI) {
        *MI_INTR_MASK_REG |= MIMask::PI;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Set PI interrupt mask");
    }
    
    if (val & MIMask::CLR_DP) {
        *MI_INTR_MASK_REG &= ~MIMask::DP;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Clear DP interrupt mask");
    }
    if (val & MIMask::SET_DP) {
        *MI_INTR_MASK_REG |= MIMask::DP;
        WriteLog(LogLevel::Trace, "MIPSYInterface: Set DP interrupt mask");
    }
    
    // Check if any interrupts should trigger after mask change
    m_Reg.CheckInterrupts();
}

// ============================================================================
// Interrupt Management
// ============================================================================

void MIPSYInterfaceHandler::SetInterrupt(uint32_t interruptBit)
{
    *MI_INTR_REG |= interruptBit;
    
    WriteLog(LogLevel::Debug, "MIPSYInterface: Set interrupt bit 0x%02X (INTR=0x%02X, MASK=0x%02X)",
        interruptBit, *MI_INTR_REG, *MI_INTR_MASK_REG);
    
    // Trigger interrupt check
    m_Reg.CheckInterrupts();
}

void MIPSYInterfaceHandler::ClearInterrupt(uint32_t interruptBit)
{
    *MI_INTR_REG &= ~interruptBit;
    
    // Also clear from GFX interrupt register if it's a DP interrupt
    if (interruptBit & MIIntr::DP) {
        m_Reg.m_GfxIntrReg &= ~MIIntr::DP;
    }
    
    WriteLog(LogLevel::Debug, "MIPSYInterface: Clear interrupt bit 0x%02X (INTR=0x%02X, MASK=0x%02X)",
        interruptBit, *MI_INTR_REG, *MI_INTR_MASK_REG);
    
    // Recheck interrupts
    m_Reg.CheckInterrupts();
}

bool MIPSYInterfaceHandler::IsInterruptPending() const
{
    return (*MI_INTR_REG & *MI_INTR_MASK_REG) != 0;
}

uint32_t MIPSYInterfaceHandler::GetPendingInterrupts() const
{
    return *MI_INTR_REG & *MI_INTR_MASK_REG;
}

// ============================================================================
// Status Queries
// ============================================================================

bool MIPSYInterfaceHandler::IsInitMode() const
{
    return (*MI_MODE_REG & MIMode::INIT) != 0;
}

bool MIPSYInterfaceHandler::IsEBusMode() const
{
    return (*MI_MODE_REG & MIMode::EBUS) != 0;
}

bool MIPSYInterfaceHandler::IsRDRAMRegMode() const
{
    return (*MI_MODE_REG & MIMode::RDRAM) != 0;
}

// ============================================================================
// Reset
// ============================================================================

void MIPSYInterfaceHandler::Reset()
{
    WriteLog(LogLevel::Info, "MIPSYInterface: Reset");
    
    *MI_MODE_REG = 0;
    *MI_VERSION_REG = 0x02020102;  // Standard version
    *MI_INTR_REG = 0;
    *MI_INTR_MASK_REG = 0;
    
    m_Reg.m_GfxIntrReg = 0;
}

void MIPSYInterfaceHandler::WriteLog(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    switch(level) {
        case LogLevel::Trace: 
            // Suppressed - too verbose for normal debug runs
            break;
        case LogLevel::Debug:
            ::Log::debug(buffer);
            break;
        case LogLevel::Info:
            ::Log::info(buffer);
            break;
        case LogLevel::Warning:
            ::Log::warning(buffer);
            break;
        case LogLevel::Error:
            ::Log::error(buffer);
            break;
    }
}

