/**
 * @file InputPlugin.hpp
 * @brief Input plugin interface for N64 controller emulation
 * 
 * Handles all input-related functionality:
 * - Controller input
 * - Pak accessories (Memory, Rumble, Transfer)
 * - Raw controller data
 */

#pragma once

#include "PluginBase.hpp"
#include <cstdint>
#include <array>

namespace Plugins {

/**
 * @struct ControllerState
 * @brief Controller button and analog state
 */
struct ControllerState {
    // Digital buttons
    bool ButtonA;
    bool ButtonB;
    bool ButtonStart;
    bool DPadUp;
    bool DPadDown;
    bool DPadLeft;
    bool DPadRight;
    bool TriggerZ;
    bool TriggerL;
    bool TriggerR;
    bool CButtonUp;
    bool CButtonDown;
    bool CButtonLeft;
    bool CButtonRight;

    // Analog stick (-128 to +127)
    int8_t AnalogX;
    int8_t AnalogY;

    // Controller presence
    bool Connected;

    // Pak type
    enum class PakType : uint8_t { None = 0, MemPak = 1, RumblePak = 2, TransferPak = 3 } Pak;
};

/**
 * @struct InputInfo
 * @brief Input interface information
 */
struct InputInfo {
    void* hwnd;                 // Window handle
    uint32_t* RDRAM;            // Pointer to RDRAM
    uint32_t* MI_INTR_REG;      // MI interrupt register

    uint32_t* SI_DRAM_ADDR_REG; // SI registers
    uint32_t* SI_PIF_ADDR_RD64B_REG;
    uint32_t* SI_PIF_ADDR_WR64B_REG;
    uint32_t* SI_STATUS_REG;

    uint8_t* PIF_RAM;          // PIF RAM pointer

    void (*CheckInterrupts)(); // Interrupt check callback
};

/**
 * @class InputPlugin
 * @brief Input plugin interface
 */
class InputPlugin : public PluginBase {
public:
    virtual ~InputPlugin() = default;

    // ========================================================================
    // Input Initialization
    // ========================================================================

    /**
     * @brief Initialize input system
     * @param info Input interface info
     * @return true on success
     */
    virtual bool InitializeInput(const InputInfo& info) = 0;

    /**
     * @brief Set input focus
     * @param focus true if window has focus
     */
    virtual void SetFocus(bool focus) = 0;

    // ========================================================================
    // Controller Management
    // ========================================================================

    /**
     * @brief Get number of connected controllers
     * @return Controller count (1-4)
     */
    virtual uint32_t GetControllerCount() const = 0;

    /**
     * @brief Check if controller is present
     * @param controller Controller index (0-3)
     * @return true if connected
     */
    virtual bool IsControllerPresent(uint32_t controller) const = 0;

    /**
     * @brief Get controller state
     * @param controller Controller index (0-3)
     * @return Controller state
     */
    virtual ControllerState GetControllerState(uint32_t controller) = 0;

    /**
     * @brief Update controller states
     * Called once per frame
     */
    virtual void UpdateControllers() = 0;

    // ========================================================================
    // PIF/SI Processing
    // ========================================================================

    /**
     * @brief Process PIF RAM command
     * Called when SI writes to PIF RAM
     */
    virtual void ProcessPIFCommand() = 0;

    /**
     * @brief Read controller data
     * @param controller Controller index (0-3)
     * @param command Command buffer
     * @return true on success
     */
    virtual bool ReadController(uint32_t controller, uint8_t* command) = 0;

    /**
     * @brief Get raw controller data (for movie recording)
     * @param controller Controller index (0-3)
     * @param data Output buffer (4 bytes)
     */
    virtual void GetRawControllerData(uint32_t controller, uint8_t* data) = 0;

    /**
     * @brief Set controller data (for movie playback)
     * @param controller Controller index (0-3)
     * @param data Input buffer (4 bytes)
     */
    virtual void SetControllerData(uint32_t controller, const uint8_t* data) = 0;

    // ========================================================================
    // Pak Accessories
    // ========================================================================

    /**
     * @brief Read from controller pak
     * @param controller Controller index (0-3)
     * @param address Pak address
     * @param data Output buffer
     * @param length Data length
     * @return true on success
     */
    virtual bool ReadPak(uint32_t controller, uint16_t address, uint8_t* data, uint32_t length) = 0;

    /**
     * @brief Write to controller pak
     * @param controller Controller index (0-3)
     * @param address Pak address
     * @param data Input buffer
     * @param length Data length
     * @return true on success
     */
    virtual bool WritePak(uint32_t controller, uint16_t address, const uint8_t* data, uint32_t length) = 0;

    /**
     * @brief Start rumble effect
     * @param controller Controller index (0-3)
     * @param strength Rumble strength (0-255)
     */
    virtual void SetRumble(uint32_t controller, uint8_t strength) = 0;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Open controller configuration dialog
     * @param controller Controller index (0-3)
     * @param hwndParent Parent window handle
     */
    virtual void ConfigureController(uint32_t controller, void* hwndParent) = 0;
};

} // namespace Plugins
