/**
 * @file RSPPlugin.hpp
 * @brief RSP plugin interface for Reality Signal Processor emulation
 * 
 * Handles RSP (Reality Signal Processor) execution:
 * - RSP microcode execution
 * - Task scheduling
 * - DMEM/IMEM access
 */

#pragma once

#include "PluginBase.hpp"
#include <cstdint>

namespace Plugins {

/**
 * @struct RSPInfo
 * @brief RSP interface information
 */
struct RSPInfo {
    uint32_t* RDRAM;                // Pointer to RDRAM
    uint32_t* DMEM;                 // Pointer to RSP DMEM (4KB)
    uint32_t* IMEM;                 // Pointer to RSP IMEM (4KB)
    uint32_t* MI_INTR_REG;          // MI interrupt register
    
    // SP registers
    uint32_t* SP_MEM_ADDR_REG;
    uint32_t* SP_DRAM_ADDR_REG;
    uint32_t* SP_RD_LEN_REG;
    uint32_t* SP_WR_LEN_REG;
    uint32_t* SP_STATUS_REG;
    uint32_t* SP_DMA_FULL_REG;
    uint32_t* SP_DMA_BUSY_REG;
    uint32_t* SP_SEMAPHORE_REG;
    
    // DPC registers
    uint32_t* DPC_START_REG;
    uint32_t* DPC_END_REG;
    uint32_t* DPC_CURRENT_REG;
    uint32_t* DPC_STATUS_REG;
    uint32_t* DPC_CLOCK_REG;
    uint32_t* DPC_BUFBUSY_REG;
    uint32_t* DPC_PIPEBUSY_REG;
    uint32_t* DPC_TMEM_REG;
    
    void (*CheckInterrupts)();      // Interrupt check callback
};

/**
 * @enum RSPTaskType
 * @brief RSP task types
 */
enum class RSPTaskType : uint32_t {
    Graphics = 1,
    Audio = 2,
    JPEG = 3,
    Custom = 4
};

/**
 * @class RSPPlugin
 * @brief RSP plugin interface
 */
class RSPPlugin : public PluginBase {
public:
    virtual ~RSPPlugin() = default;
    
    // ========================================================================
    // RSP Initialization
    // ========================================================================
    
    /**
     * @brief Initialize RSP emulation
     * @param info RSP interface info
     * @return true on success
     */
    virtual bool InitializeRSP(const RSPInfo& info) = 0;
    
    /**
     * @brief Set CPU type for RSP emulation
     * @param useLLE true for low-level emulation, false for HLE
     */
    virtual void SetCPUType(bool useLLE) = 0;
    
    // ========================================================================
    // RSP Execution
    // ========================================================================
    
    /**
     * @brief Execute RSP task
     * @param taskType Type of task
     * @return Cycles executed
     */
    virtual uint32_t DoRSPCycles(RSPTaskType taskType) = 0;
    
    /**
     * @brief Single step RSP execution
     * @return true if instruction executed
     */
    virtual bool DoRSPStep() = 0;
    
    /**
     * @brief Break RSP execution
     */
    virtual void BreakRSP() = 0;
    
    // ========================================================================
    // DMA Operations
    // ========================================================================
    
    /**
     * @brief Process SP DMA read (RDRAM -> DMEM/IMEM)
     */
    virtual void ProcessSPDMARead() = 0;
    
    /**
     * @brief Process SP DMA write (DMEM/IMEM -> RDRAM)
     */
    virtual void ProcessSPDMAWrite() = 0;
    
    // ========================================================================
    // Status and Control
    // ========================================================================
    
    /**
     * @brief Get RSP program counter
     * @return PC value
     */
    virtual uint32_t GetPC() const = 0;
    
    /**
     * @brief Set RSP program counter
     * @param pc New PC value
     */
    virtual void SetPC(uint32_t pc) = 0;
    
    /**
     * @brief Check if RSP is running
     * @return true if RSP is executing
     */
    virtual bool IsRSPRunning() const = 0;
    
    /**
     * @brief Get RSP status flags
     * @return Status register value
     */
    virtual uint32_t GetStatus() const = 0;
};

} // namespace Plugins
