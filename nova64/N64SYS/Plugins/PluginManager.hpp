/**
 * @file PluginManager.hpp
 * @brief Plugin Manager - Central hub for loading and managing N64 plugins
 * 
 * Manages the lifecycle of all plugins (Graphics, Audio, Input, RSP).
 * Handles plugin discovery, loading, initialization, and coordination.
 */

#pragma once

#include "PluginBase.hpp"
#include "GFXPlugin.hpp"
#include "AudioPlugin.hpp"
#include "InputPlugin.hpp"
#include "RSPPlugin.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "PluginBase.hpp"

class CRegisters;
namespace Nova64 {
class MipsMemoryVM;
}
class CN64System;

namespace Plugins {

/**
 * @brief Plugin configuration structure
 */
struct PluginConfig {
    PluginType Type;
    std::string Name;
    std::string Path;
    std::string ConfigFile;
    bool Enabled;

    PluginConfig() : Type(PluginType::Graphics), Enabled(true) {}
};

/**
 * @brief Plugin notification callback types
 */
enum class PluginNotification { PluginLoaded, PluginUnloaded, PluginInitialized, PluginShutdown, PluginError };

using PluginNotificationCallback = std::function<void(PluginType, PluginNotification, const std::string&)>;

/**
 * @brief Central plugin management system
 * 
 * Responsibilities:
 * - Load/unload plugins from shared libraries
 * - Initialize plugins with system pointers
 * - Coordinate plugin lifecycle
 * - Manage plugin configurations
 * - Handle plugin communication
 */
class PluginManager {
public:
    /**
     * @brief Construct plugin manager
     * @param system Reference to N64 system
     */
    explicit PluginManager(CN64System& system);

    ~PluginManager();

    // Disable copy/move
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    // --- Plugin Loading ---

    /**
     * @brief Load specific plugin type
     * @param type Plugin type to load
     * @param path Path to plugin file (empty = use config)
     * @return True if loaded successfully
     */
    bool LoadPlugin(PluginType type, const std::string& path = "");

    /**
     * @brief Unload specific plugin type
     * @param type Plugin type to unload
     */
    void UnloadPlugin(PluginType type);

    void UnloadAllPlugins();

    // --- Plugin Lifecycle ---

    /**
     * @brief Initialize all loaded plugins
     * @return True if all plugins initialized successfully
     */
    bool InitializePlugins();

    void ShutdownPlugins();

    void ResetPlugins();

    // --- Plugin Access ---

    /**
     * @brief Get graphics plugin
     * @return Pointer to graphics plugin or nullptr
     */
    GFXPlugin* GetGraphicsPlugin() const { return m_GraphicsPlugin.get(); }

    /**
     * @brief Get audio plugin
     * @return Pointer to audio plugin or nullptr
     */
    AudioPlugin* GetAudioPlugin() const { return m_AudioPlugin.get(); }

    /**
     * @brief Get input plugin
     * @return Pointer to input plugin or nullptr
     */
    InputPlugin* GetInputPlugin() const { return m_InputPlugin.get(); }

    /**
     * @brief Get RSP plugin
     * @return Pointer to RSP plugin or nullptr
     */
    RSPPlugin* GetRSPPlugin() const { return m_RSPPlugin.get(); }

    /**
     * @brief Get plugin by type
     * @param type Plugin type
     * @return Pointer to plugin base or nullptr
     */
    PluginBase* GetPlugin(PluginType type) const;

    // --- Configuration ---

    // --- Plugin Discovery ---

    /**
     * @brief Get plugin information without loading
     * @param path Path to plugin file
     * @return Plugin info or empty if invalid
     */
    PluginInfo GetPluginInfo(const std::string& path) const;

    // --- Notifications ---

    // --- Save States ---

    /**
     * @brief Save all plugin states
     * @param data Output buffer for state data
     * @return True if saved successfully
     */
    bool SaveState(std::vector<uint8_t>& data);

    /**
     * @brief Load all plugin states
     * @param data Input buffer with state data
     * @return True if loaded successfully
     */
    bool LoadState(const std::vector<uint8_t>& data);

    // --- Status ---

private:
    // --- Internal Plugin Creation ---

    void CreateDefaultGraphicsPlugin();

    void CreateDefaultAudioPlugin();

    void CreateDefaultInputPlugin();

    void CreateDefaultRSPPlugin();

    /**
     * @brief Load plugin from shared library
     * @param path Path to plugin file
     * @param type Expected plugin type
     * @return Created plugin or nullptr
     */
    std::unique_ptr<PluginBase> LoadPluginFromFile(const std::string& path, PluginType type);

    void NotifyPluginEvent(PluginType type, PluginNotification notification, const std::string& message = "");

    std::string GetDefaultPluginPath(PluginType type) const;

    CN64System& m_System;
    CRegisters* m_Registers;
    Nova64::MipsMemoryVM* m_Memory;

    // Plugin instances
    std::unique_ptr<GFXPlugin> m_GraphicsPlugin;
    std::unique_ptr<AudioPlugin> m_AudioPlugin;
    std::unique_ptr<InputPlugin> m_InputPlugin;
    std::unique_ptr<RSPPlugin> m_RSPPlugin;

    // Plugin configurations
    PluginConfig m_GraphicsConfig;
    PluginConfig m_AudioConfig;
    PluginConfig m_InputConfig;
    PluginConfig m_RSPConfig;

    // Plugin state
    bool m_PluginsInitialized;
    bool m_PluginsPaused;

    // Notification callback
    PluginNotificationCallback m_NotificationCallback;

    // Configuration file path
    std::string m_ConfigFilePath;
};

} // namespace Plugins
