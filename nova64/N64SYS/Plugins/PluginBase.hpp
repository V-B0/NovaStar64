/**
 * @file PluginBase.hpp
 * @brief Base plugin interface for N64 emulator plugins
 * 
 * Defines the common interface that all plugins (Graphics, Audio, Input, RSP) must implement.
 * Based on the N64 plugin specification but modernized for C++17/20.
 * 
 * Plugin Types:
 * - Graphics (GFX): Handles RDP commands and video output
 * - Audio: Handles audio playback and DSP processing
 * - Input: Handles controller input and accessories
 * - RSP: Handles RSP microcode execution
 */

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <functional>

// Forward declarations
class CN64System;
class CRegisters;

namespace Plugins {

/**
 * @enum PluginType
 * @brief Types of N64 plugins
 */
enum class PluginType : uint8_t { Graphics = 1, Audio = 2, Input = 3, RSP = 4 };

/**
 * @struct PluginInfo
 * @brief Plugin metadata and capabilities
 */
struct PluginInfo {
    PluginType Type;
    uint16_t Version;        // Plugin API version (0x0104 = 1.4)
    std::string Name;        // Plugin name
    std::string Author;      // Plugin author
    std::string Description; // Plugin description

    // Capabilities
    bool SupportsThreading;    // Can run in separate thread
    bool SupportsStateSync;    // Supports save state synchronization
    bool RequiresAudioSync;    // Needs audio synchronization
    bool SupportsConfigDialog; // Has configuration UI
};

/**
 * @class PluginBase
 * @brief Abstract base class for all N64 plugins
 */
class PluginBase {
public:
    virtual ~PluginBase() = default;

    // ========================================================================
    // Plugin Information
    // ========================================================================

    /**
     * @brief Get plugin information
     * @return Plugin metadata
     */
    virtual PluginInfo GetPluginInfo() const = 0;

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    /**
     * @brief Initialize plugin with system reference
     * @param system N64 system instance
     * @return true on success
     */
    virtual bool Initialize(CN64System* system) = 0;

    /**
     * @brief Shutdown plugin and release resources
     */
    virtual void Shutdown() = 0;

    /**
     * @brief Reset plugin to initial state
     * @param coldReset true for cold reset, false for soft reset
     */
    virtual void Reset(bool coldReset) = 0;

    /**
     * @brief Pause plugin processing
     */
    virtual void Pause() = 0;

    /**
     * @brief Resume plugin processing
     */
    virtual void Resume() = 0;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Check if plugin is properly initialized
     * @return true if initialized
     */
    virtual bool IsInitialized() const = 0;

    /**
     * @brief Open configuration dialog
     * @param hwndParent Parent window handle
     */
    virtual void ShowConfigDialog(void* hwndParent) = 0;

    // ========================================================================
    // Save States
    // ========================================================================

    /**
     * @brief Save plugin state
     * @param data Output buffer for state data
     * @param size Size of output buffer
     * @return Number of bytes written
     */
    virtual size_t SaveState(uint8_t* data, size_t size) = 0;

    /**
     * @brief Load plugin state
     * @param data State data to load
     * @param size Size of state data
     * @return true on success
     */
    virtual bool LoadState(const uint8_t* data, size_t size) = 0;

protected:
    CN64System* m_System = nullptr;
};

/**
 * @brief Plugin initialization callback
 */
using PluginInitCallback = std::function<bool()>;

/**
 * @brief Plugin error callback
 */
using PluginErrorCallback = std::function<void(const std::string& error)>;

} // namespace Plugins
