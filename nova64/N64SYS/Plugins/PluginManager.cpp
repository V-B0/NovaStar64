/**
 * @file PluginManager.cpp
 * @brief Plugin manager: loading, lifecycle, configuration, discovery and save states of the graphics, audio, input and RSP plugins.
 */

#include "PluginManager.hpp"
#include "../MainSYS.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/log.hpp"
#include "../../common/file.hpp"
#include "../../common/IniFile.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>

// Factory function declarations from plugin .cpp files
namespace Plugins {
std::unique_ptr<GFXPlugin> CreateGFXPluginFromDLL(const std::string& path);
} // namespace Plugins

// NovaGraphics factory
namespace Nova64::Video {
std::unique_ptr<::Plugins::GFXPlugin> CreateNovaGraphicsPlugin();
}

// NovaAudio factory
namespace Nova64::Audio {
std::unique_ptr<::Plugins::AudioPlugin> CreateNovaAudioPlugin();
}

namespace Plugins {
std::unique_ptr<AudioPlugin> CreateAudioPluginFromDLL(const std::string& path);
std::unique_ptr<InputPlugin> CreateDefaultInputPlugin();
std::unique_ptr<InputPlugin> CreateInputPluginFromDLL(const std::string& path);
std::unique_ptr<RSPPlugin> CreateDefaultRSPPlugin();
std::unique_ptr<RSPPlugin> CreateRSPPluginFromDLL(const std::string& path);
} // namespace Plugins

namespace Plugins {

// --- Constructor / Destructor ---

PluginManager::PluginManager(CN64System& system) :
    m_System(system), m_Registers(nullptr), m_Memory(nullptr), m_PluginsInitialized(false), m_PluginsPaused(false) {
    Log::info("PluginManager: Created");

    m_GraphicsConfig.Type = PluginType::Graphics;
    m_GraphicsConfig.Name = "Default Graphics";

    m_AudioConfig.Type = PluginType::Audio;
    m_AudioConfig.Name = "Default Audio";

    m_InputConfig.Type = PluginType::Input;
    m_InputConfig.Name = "Default Input";

    m_RSPConfig.Type = PluginType::RSP;
    m_RSPConfig.Name = "Default RSP";
}

PluginManager::~PluginManager() {
    Log::info("PluginManager: Destroying");
    UnloadAllPlugins();
}

// --- Plugin Loading ---

bool PluginManager::LoadPlugin(PluginType type, const std::string& path) {
    Log::infof("PluginManager: Loading plugin type %d", static_cast<int>(type));

    UnloadPlugin(type);

    std::string pluginPath = path.empty() ? GetDefaultPluginPath(type) : path;

    // Try to load from file if path specified
    if (!pluginPath.empty() && std::filesystem::exists(pluginPath)) {
        auto plugin = LoadPluginFromFile(pluginPath, type);
        if (plugin) {
            switch (type) {
            case PluginType::Graphics:
                m_GraphicsPlugin.reset(dynamic_cast<GFXPlugin*>(plugin.release()));
                m_GraphicsConfig.Path = pluginPath;
                break;
            case PluginType::Audio:
                m_AudioPlugin.reset(dynamic_cast<AudioPlugin*>(plugin.release()));
                m_AudioConfig.Path = pluginPath;
                break;
            case PluginType::Input:
                m_InputPlugin.reset(dynamic_cast<InputPlugin*>(plugin.release()));
                m_InputConfig.Path = pluginPath;
                break;
            case PluginType::RSP:
                m_RSPPlugin.reset(dynamic_cast<RSPPlugin*>(plugin.release()));
                m_RSPConfig.Path = pluginPath;
                break;
            }

            NotifyPluginEvent(type, PluginNotification::PluginLoaded, pluginPath);
            return true;
        }
    }

    // Fall back to default plugin
    Log::infof("PluginManager: Creating default plugin for type %d", static_cast<int>(type));

    switch (type) {
    case PluginType::Graphics: CreateDefaultGraphicsPlugin(); break;
    case PluginType::Audio:    CreateDefaultAudioPlugin(); break;
    case PluginType::Input:    CreateDefaultInputPlugin(); break;
    case PluginType::RSP:      CreateDefaultRSPPlugin(); break;
    }

    NotifyPluginEvent(type, PluginNotification::PluginLoaded, "Default");
    return true;
}

void PluginManager::UnloadPlugin(PluginType type) {
    Log::infof("PluginManager: Unloading plugin type %d", static_cast<int>(type));

    PluginBase* plugin = GetPlugin(type);
    if (plugin && m_PluginsInitialized) {
        plugin->Shutdown();
    }

    switch (type) {
    case PluginType::Graphics: m_GraphicsPlugin.reset(); break;
    case PluginType::Audio:    m_AudioPlugin.reset(); break;
    case PluginType::Input:    m_InputPlugin.reset(); break;
    case PluginType::RSP:      m_RSPPlugin.reset(); break;
    }

    NotifyPluginEvent(type, PluginNotification::PluginUnloaded);
}

void PluginManager::UnloadAllPlugins() {
    Log::info("PluginManager: Unloading all plugins");

    if (m_PluginsInitialized) {
        ShutdownPlugins();
    }

    UnloadPlugin(PluginType::RSP);
    UnloadPlugin(PluginType::Input);
    UnloadPlugin(PluginType::Audio);
    UnloadPlugin(PluginType::Graphics);
}

// --- Plugin Lifecycle ---

bool PluginManager::InitializePlugins() {
    if (m_PluginsInitialized) {
        Log::warning("PluginManager: Plugins already initialized");
        return true;
    }

    Log::info("PluginManager: Initializing plugins");

    bool success = true;

    if (m_GraphicsPlugin) {
        if (!m_GraphicsPlugin->Initialize(&m_System)) {
            Log::error("PluginManager: Graphics plugin initialization failed");
            success = false;
        } else {
            NotifyPluginEvent(PluginType::Graphics, PluginNotification::PluginInitialized);
        }
    }

    if (m_AudioPlugin) {
        if (!m_AudioPlugin->Initialize(&m_System)) {
            Log::error("PluginManager: Audio plugin initialization failed");
            success = false;
        } else {
            NotifyPluginEvent(PluginType::Audio, PluginNotification::PluginInitialized);
        }
    }

    if (m_InputPlugin) {
        if (!m_InputPlugin->Initialize(&m_System)) {
            Log::error("PluginManager: Input plugin initialization failed");
            success = false;
        } else {
            NotifyPluginEvent(PluginType::Input, PluginNotification::PluginInitialized);
        }
    }

    if (m_RSPPlugin) {
        if (!m_RSPPlugin->Initialize(&m_System)) {
            Log::error("PluginManager: RSP plugin initialization failed");
            success = false;
        } else {
            NotifyPluginEvent(PluginType::RSP, PluginNotification::PluginInitialized);
        }
    }

    if (success) {
        m_PluginsInitialized = true;
        Log::info("PluginManager: All plugins initialized successfully");
    }

    return success;
}

void PluginManager::ShutdownPlugins() {
    if (!m_PluginsInitialized) {
        return;
    }

    Log::info("PluginManager: Shutting down plugins");

    // Shutdown in reverse order
    if (m_RSPPlugin) {
        m_RSPPlugin->Shutdown();
        NotifyPluginEvent(PluginType::RSP, PluginNotification::PluginShutdown);
    }

    if (m_InputPlugin) {
        m_InputPlugin->Shutdown();
        NotifyPluginEvent(PluginType::Input, PluginNotification::PluginShutdown);
    }

    if (m_AudioPlugin) {
        m_AudioPlugin->Shutdown();
        NotifyPluginEvent(PluginType::Audio, PluginNotification::PluginShutdown);
    }

    if (m_GraphicsPlugin) {
        m_GraphicsPlugin->Shutdown();
        NotifyPluginEvent(PluginType::Graphics, PluginNotification::PluginShutdown);
    }

    m_PluginsInitialized = false;
    m_PluginsPaused = false;
}

void PluginManager::ResetPlugins() {
    Log::info("PluginManager: Resetting plugins");

    if (m_GraphicsPlugin) m_GraphicsPlugin->Reset(true);
    if (m_AudioPlugin) m_AudioPlugin->Reset(true);
    if (m_InputPlugin) m_InputPlugin->Reset(true);
    if (m_RSPPlugin) m_RSPPlugin->Reset(true);
}

// --- Plugin Access ---

PluginBase* PluginManager::GetPlugin(PluginType type) const {
    switch (type) {
    case PluginType::Graphics: return m_GraphicsPlugin.get();
    case PluginType::Audio:    return m_AudioPlugin.get();
    case PluginType::Input:    return m_InputPlugin.get();
    case PluginType::RSP:      return m_RSPPlugin.get();
    default:                   return nullptr;
    }
}

// --- Configuration ---

// --- Plugin Discovery ---

// --- Notifications ---

void PluginManager::NotifyPluginEvent(PluginType type, PluginNotification notification, const std::string& message) {
    if (m_NotificationCallback) {
        m_NotificationCallback(type, notification, message);
    }
}

// --- Save States ---

bool PluginManager::SaveState(std::vector<uint8_t>& data) {
    Log::debug("PluginManager: Saving plugin states");

    data.clear();

    bool success = true;

    // Save each plugin's state as [4-byte LE length][payload].
    auto savePlugin = [&](PluginBase* plugin) {
        if (!plugin) {
            // Write a zero-length entry to keep the slot order
            uint32_t zero = 0;
            const auto* p = reinterpret_cast<const uint8_t*>(&zero);
            data.insert(data.end(), p, p + 4);
            return;
        }
        constexpr size_t kMaxStateSize = 64 * 1024;
        std::vector<uint8_t> buf(kMaxStateSize, 0);
        size_t written = plugin->SaveState(buf.data(), buf.size());
        uint32_t len = static_cast<uint32_t>(written);
        const auto* p = reinterpret_cast<const uint8_t*>(&len);
        data.insert(data.end(), p, p + 4);
        if (written > 0) {
            data.insert(data.end(), buf.begin(), buf.begin() + written);
        }
    };

    savePlugin(m_GraphicsPlugin.get());
    savePlugin(m_AudioPlugin.get());
    savePlugin(m_InputPlugin.get());
    savePlugin(m_RSPPlugin.get());

    return success;
}

bool PluginManager::LoadState(const std::vector<uint8_t>& data) {
    Log::debug("PluginManager: Loading plugin states");

    PluginBase* plugins[] = {m_GraphicsPlugin.get(), m_AudioPlugin.get(), m_InputPlugin.get(), m_RSPPlugin.get()};

    size_t offset = 0;
    for (auto* plugin : plugins) {
        if (offset + 4 > data.size()) {
            Log::error("PluginManager: Truncated state data");
            return false;
        }
        uint32_t len = 0;
        std::memcpy(&len, data.data() + offset, 4);
        offset += 4;
        if (offset + len > data.size()) {
            Log::error("PluginManager: State data overflow");
            return false;
        }
        if (plugin && len > 0) {
            plugin->LoadState(data.data() + offset, len);
        }
        offset += len;
    }

    return true;
}

// --- Status ---

// --- Internal Plugin Creation ---

void PluginManager::CreateDefaultGraphicsPlugin() {
    Log::info("PluginManager: Creating NovaGraphics graphics plugin");
    m_GraphicsPlugin = ::Nova64::Video::CreateNovaGraphicsPlugin();
}

void PluginManager::CreateDefaultAudioPlugin() {
    Log::info("PluginManager: Creating NovaAudio audio plugin");
    m_AudioPlugin = ::Nova64::Audio::CreateNovaAudioPlugin();
}

void PluginManager::CreateDefaultInputPlugin() {
    Log::info("PluginManager: Creating default input plugin");
    m_InputPlugin = ::Plugins::CreateDefaultInputPlugin();
}

void PluginManager::CreateDefaultRSPPlugin() {
    Log::info("PluginManager: Creating default RSP plugin");
    m_RSPPlugin = ::Plugins::CreateDefaultRSPPlugin();
}

std::unique_ptr<PluginBase> PluginManager::LoadPluginFromFile(const std::string& path, PluginType type) {
    Log::infof("PluginManager: Loading plugin from file: %s", path.c_str());

    switch (type) {
    case PluginType::Graphics: return ::Plugins::CreateGFXPluginFromDLL(path);
    case PluginType::Audio:    return ::Plugins::CreateAudioPluginFromDLL(path);
    case PluginType::Input:    return ::Plugins::CreateInputPluginFromDLL(path);
    case PluginType::RSP:      return ::Plugins::CreateRSPPluginFromDLL(path);
    default:                   Log::warningf("PluginManager: Unknown plugin type %d", static_cast<int>(type)); return nullptr;
    }
}

std::string PluginManager::GetDefaultPluginPath(PluginType type) const { return ""; }

} // namespace Plugins
