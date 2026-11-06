/**
 * @file PluginManager.cpp
 * @brief Plugin Manager Implementation
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
    std::unique_ptr<GFXPlugin>   CreateDefaultGFXPlugin();
    std::unique_ptr<GFXPlugin>   CreateGFXPluginFromDLL(const std::string& path);
}

// NovaGraphics factory
namespace Nova64::Video {
    std::unique_ptr<::Plugins::GFXPlugin> CreateNovaGraphicsPlugin();
}

// NovaAudio factory
namespace Nova64::Audio {
    std::unique_ptr<::Plugins::AudioPlugin> CreateNovaAudioPlugin();
}

namespace Plugins {
    std::unique_ptr<AudioPlugin> CreateDefaultAudioPlugin();
    std::unique_ptr<AudioPlugin> CreateAudioPluginFromDLL(const std::string& path);
    std::unique_ptr<InputPlugin> CreateDefaultInputPlugin();
    std::unique_ptr<InputPlugin> CreateInputPluginFromDLL(const std::string& path);
    std::unique_ptr<RSPPlugin>   CreateDefaultRSPPlugin();
    std::unique_ptr<RSPPlugin>   CreateRSPPluginFromDLL(const std::string& path);
}

namespace Plugins {

// ============================================================================
// Constructor / Destructor
// ============================================================================

PluginManager::PluginManager(CN64System& system) :
    m_System(system),
    m_Registers(nullptr),
    m_Memory(nullptr),
    m_PluginsInitialized(false),
    m_PluginsPaused(false)
{
    Log::info("PluginManager: Created");
    
    // Initialize plugin configs with default types
    m_GraphicsConfig.Type = PluginType::Graphics;
    m_GraphicsConfig.Name = "Default Graphics";
    
    m_AudioConfig.Type = PluginType::Audio;
    m_AudioConfig.Name = "Default Audio";
    
    m_InputConfig.Type = PluginType::Input;
    m_InputConfig.Name = "Default Input";
    
    m_RSPConfig.Type = PluginType::RSP;
    m_RSPConfig.Name = "Default RSP";
}

PluginManager::~PluginManager()
{
    Log::info("PluginManager: Destroying");
    UnloadAllPlugins();
}

// ============================================================================
// Plugin Loading
// ============================================================================

bool PluginManager::LoadAllPlugins()
{
    Log::info("PluginManager: Loading all plugins");
    
    bool success = true;
    
    // Load each plugin type
    if (!LoadPlugin(PluginType::Graphics)) {
        Log::error("PluginManager: Failed to load graphics plugin");
        success = false;
    }
    
    if (!LoadPlugin(PluginType::Audio)) {
        Log::error("PluginManager: Failed to load audio plugin");
        success = false;
    }
    
    if (!LoadPlugin(PluginType::Input)) {
        Log::error("PluginManager: Failed to load input plugin");
        success = false;
    }
    
    if (!LoadPlugin(PluginType::RSP)) {
        Log::error("PluginManager: Failed to load RSP plugin");
        success = false;
    }
    
    if (success) {
        Log::info("PluginManager: All plugins loaded successfully");
    } else {
        Log::warning("PluginManager: Some plugins failed to load");
    }
    
    return success;
}

bool PluginManager::LoadPlugin(PluginType type, const std::string& path)
{
    Log::infof("PluginManager: Loading plugin type %d", static_cast<int>(type));
    
    // Unload existing plugin of this type
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
        case PluginType::Graphics:
            CreateDefaultGraphicsPlugin();
            break;
        case PluginType::Audio:
            CreateDefaultAudioPlugin();
            break;
        case PluginType::Input:
            CreateDefaultInputPlugin();
            break;
        case PluginType::RSP:
            CreateDefaultRSPPlugin();
            break;
    }
    
    NotifyPluginEvent(type, PluginNotification::PluginLoaded, "Default");
    return true;
}

void PluginManager::UnloadPlugin(PluginType type)
{
    Log::infof("PluginManager: Unloading plugin type %d", static_cast<int>(type));
    
    PluginBase* plugin = GetPlugin(type);
    if (plugin && m_PluginsInitialized) {
        plugin->Shutdown();
    }
    
    switch (type) {
        case PluginType::Graphics:
            m_GraphicsPlugin.reset();
            break;
        case PluginType::Audio:
            m_AudioPlugin.reset();
            break;
        case PluginType::Input:
            m_InputPlugin.reset();
            break;
        case PluginType::RSP:
            m_RSPPlugin.reset();
            break;
    }
    
    NotifyPluginEvent(type, PluginNotification::PluginUnloaded);
}

void PluginManager::UnloadAllPlugins()
{
    Log::info("PluginManager: Unloading all plugins");
    
    if (m_PluginsInitialized) {
        ShutdownPlugins();
    }
    
    UnloadPlugin(PluginType::RSP);
    UnloadPlugin(PluginType::Input);
    UnloadPlugin(PluginType::Audio);
    UnloadPlugin(PluginType::Graphics);
}

bool PluginManager::IsPluginLoaded(PluginType type) const
{
    return GetPlugin(type) != nullptr;
}

// ============================================================================
// Plugin Lifecycle
// ============================================================================

bool PluginManager::InitializePlugins()
{
    if (m_PluginsInitialized) {
        Log::warning("PluginManager: Plugins already initialized");
        return true;
    }
    
    Log::info("PluginManager: Initializing plugins");
    
    bool success = true;
    
    // Initialize each plugin
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

void PluginManager::ShutdownPlugins()
{
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

void PluginManager::ResetPlugins()
{
    Log::info("PluginManager: Resetting plugins");
    
    if (m_GraphicsPlugin) m_GraphicsPlugin->Reset(true);
    if (m_AudioPlugin) m_AudioPlugin->Reset(true);
    if (m_InputPlugin) m_InputPlugin->Reset(true);
    if (m_RSPPlugin) m_RSPPlugin->Reset(true);
}

void PluginManager::PausePlugins()
{
    if (m_PluginsPaused) {
        return;
    }
    
    Log::info("PluginManager: Pausing plugins");
    
    if (m_GraphicsPlugin) m_GraphicsPlugin->Pause();
    if (m_AudioPlugin) m_AudioPlugin->Pause();
    if (m_InputPlugin) m_InputPlugin->Pause();
    if (m_RSPPlugin) m_RSPPlugin->Pause();
    
    m_PluginsPaused = true;
}

void PluginManager::ResumePlugins()
{
    if (!m_PluginsPaused) {
        return;
    }
    
    Log::info("PluginManager: Resuming plugins");
    
    if (m_GraphicsPlugin) m_GraphicsPlugin->Resume();
    if (m_AudioPlugin) m_AudioPlugin->Resume();
    if (m_InputPlugin) m_InputPlugin->Resume();
    if (m_RSPPlugin) m_RSPPlugin->Resume();
    
    m_PluginsPaused = false;
}

// ============================================================================
// Plugin Access
// ============================================================================

PluginBase* PluginManager::GetPlugin(PluginType type) const
{
    switch (type) {
        case PluginType::Graphics:
            return m_GraphicsPlugin.get();
        case PluginType::Audio:
            return m_AudioPlugin.get();
        case PluginType::Input:
            return m_InputPlugin.get();
        case PluginType::RSP:
            return m_RSPPlugin.get();
        default:
            return nullptr;
    }
}

// ============================================================================
// Configuration
// ============================================================================

bool PluginManager::LoadConfiguration(const std::string& configPath)
{
    Log::infof("PluginManager: Loading configuration from %s", configPath.c_str());
    
    m_ConfigFilePath = configPath;
    
    if (!std::filesystem::exists(configPath)) {
        Log::warning("PluginManager: Config file not found, using defaults");
        return true;  // Not an error - will use defaults
    }
    
    CIniFile ini(configPath);
    
    // Load Graphics plugin config
    m_GraphicsConfig.Path = ini.GetString("Graphics", "Plugin", "");
    m_GraphicsConfig.Name = ini.GetString("Graphics", "Name", "Default Graphics");
    m_GraphicsConfig.ConfigFile = ini.GetString("Graphics", "ConfigFile", "");
    m_GraphicsConfig.Enabled = ini.GetBool("Graphics", "Enabled", true);
    
    // Load Audio plugin config
    m_AudioConfig.Path = ini.GetString("Audio", "Plugin", "");
    m_AudioConfig.Name = ini.GetString("Audio", "Name", "Default Audio");
    m_AudioConfig.ConfigFile = ini.GetString("Audio", "ConfigFile", "");
    m_AudioConfig.Enabled = ini.GetBool("Audio", "Enabled", true);
    
    // Load Input plugin config
    m_InputConfig.Path = ini.GetString("Input", "Plugin", "");
    m_InputConfig.Name = ini.GetString("Input", "Name", "Default Input");
    m_InputConfig.ConfigFile = ini.GetString("Input", "ConfigFile", "");
    m_InputConfig.Enabled = ini.GetBool("Input", "Enabled", true);
    
    // Load RSP plugin config
    m_RSPConfig.Path = ini.GetString("RSP", "Plugin", "");
    m_RSPConfig.Name = ini.GetString("RSP", "Name", "Default RSP");
    m_RSPConfig.ConfigFile = ini.GetString("RSP", "ConfigFile", "");
    m_RSPConfig.Enabled = ini.GetBool("RSP", "Enabled", true);
    
    Log::info("PluginManager: Configuration loaded successfully");
    return true;
}

bool PluginManager::SaveConfiguration(const std::string& configPath) const
{
    Log::infof("PluginManager: Saving configuration to %s", configPath.c_str());
    
    CIniFile ini(configPath);
    
    // Save Graphics plugin config
    ini.SetString("Graphics", "Plugin", m_GraphicsConfig.Path);
    ini.SetString("Graphics", "Name", m_GraphicsConfig.Name);
    ini.SetString("Graphics", "ConfigFile", m_GraphicsConfig.ConfigFile);
    ini.SetBool("Graphics", "Enabled", m_GraphicsConfig.Enabled);
    
    // Save Audio plugin config
    ini.SetString("Audio", "Plugin", m_AudioConfig.Path);
    ini.SetString("Audio", "Name", m_AudioConfig.Name);
    ini.SetString("Audio", "ConfigFile", m_AudioConfig.ConfigFile);
    ini.SetBool("Audio", "Enabled", m_AudioConfig.Enabled);
    
    // Save Input plugin config
    ini.SetString("Input", "Plugin", m_InputConfig.Path);
    ini.SetString("Input", "Name", m_InputConfig.Name);
    ini.SetString("Input", "ConfigFile", m_InputConfig.ConfigFile);
    ini.SetBool("Input", "Enabled", m_InputConfig.Enabled);
    
    // Save RSP plugin config
    ini.SetString("RSP", "Plugin", m_RSPConfig.Path);
    ini.SetString("RSP", "Name", m_RSPConfig.Name);
    ini.SetString("RSP", "ConfigFile", m_RSPConfig.ConfigFile);
    ini.SetBool("RSP", "Enabled", m_RSPConfig.Enabled);
    
    // Write to disk
    ini.Flush();
    
    Log::info("PluginManager: Configuration saved successfully");
    return true;
}

const PluginConfig& PluginManager::GetPluginConfig(PluginType type) const
{
    switch (type) {
        case PluginType::Graphics:
            return m_GraphicsConfig;
        case PluginType::Audio:
            return m_AudioConfig;
        case PluginType::Input:
            return m_InputConfig;
        case PluginType::RSP:
            return m_RSPConfig;
        default:
            return m_GraphicsConfig;  // Fallback
    }
}

void PluginManager::SetPluginConfig(PluginType type, const PluginConfig& config)
{
    switch (type) {
        case PluginType::Graphics:
            m_GraphicsConfig = config;
            break;
        case PluginType::Audio:
            m_AudioConfig = config;
            break;
        case PluginType::Input:
            m_InputConfig = config;
            break;
        case PluginType::RSP:
            m_RSPConfig = config;
            break;
    }
}

void PluginManager::ShowPluginConfig(PluginType type)
{
    PluginBase* plugin = GetPlugin(type);
    if (plugin) {
        plugin->ShowConfigDialog(nullptr);
    } else {
        Log::warningf("PluginManager: Cannot show config for unloaded plugin type %d",
            static_cast<int>(type));
    }
}

// ============================================================================
// Plugin Discovery
// ============================================================================

std::vector<std::string> PluginManager::DiscoverPlugins(const std::string& directory) const
{
    Log::infof("PluginManager: Discovering plugins in %s", directory.c_str());
    
    std::vector<std::string> plugins;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        Log::warningf("PluginManager: Directory does not exist: %s", directory.c_str());
        return plugins;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto ext = entry.path().extension().string();
#ifdef _WIN32
        if (ext == ".dll")
#else
        if (ext == ".so")
#endif
        {
            plugins.push_back(entry.path().string());
        }
    }

    Log::infof("PluginManager: Found %zu plugin(s)", plugins.size());
    return plugins;
}

PluginInfo PluginManager::GetPluginInfo(const std::string& path) const
{
    Log::debugf("PluginManager: Getting plugin info from %s", path.c_str());
    
    PluginInfo info{};
    info.Type = PluginType::Graphics;
    info.Version = 0;
    info.Name = std::filesystem::path(path).stem().string();

    // Infer plugin type from filename convention
    const auto stem = info.Name;
    if (stem.find("RSP") != std::string::npos || stem.find("rsp") != std::string::npos) {
        info.Type = PluginType::RSP;
    } else if (stem.find("Audio") != std::string::npos || stem.find("audio") != std::string::npos) {
        info.Type = PluginType::Audio;
    } else if (stem.find("Input") != std::string::npos || stem.find("input") != std::string::npos) {
        info.Type = PluginType::Input;
    }

    return info;
}

// ============================================================================
// Notifications
// ============================================================================

void PluginManager::RegisterNotificationCallback(PluginNotificationCallback callback)
{
    m_NotificationCallback = callback;
}

void PluginManager::ClearNotificationCallback()
{
    m_NotificationCallback = nullptr;
}

void PluginManager::NotifyPluginEvent(PluginType type, PluginNotification notification, const std::string& message)
{
    if (m_NotificationCallback) {
        m_NotificationCallback(type, notification, message);
    }
}

// ============================================================================
// Save States
// ============================================================================

bool PluginManager::SaveState(std::vector<uint8_t>& data)
{
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

bool PluginManager::LoadState(const std::vector<uint8_t>& data)
{
    Log::debug("PluginManager: Loading plugin states");
    
    PluginBase* plugins[] = {
        m_GraphicsPlugin.get(),
        m_AudioPlugin.get(),
        m_InputPlugin.get(),
        m_RSPPlugin.get()
    };

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

// ============================================================================
// Status
// ============================================================================

bool PluginManager::AreRequiredPluginsLoaded() const
{
    // Graphics and RSP are required, Audio and Input are optional
    return m_GraphicsPlugin != nullptr && m_RSPPlugin != nullptr;
}

std::string PluginManager::GetStatusString() const
{
    std::string status = "Plugins: ";
    
    if (m_GraphicsPlugin) {
        status += "GFX[" + m_GraphicsPlugin->GetPluginInfo().Name + "] ";
    } else {
        status += "GFX[None] ";
    }
    
    if (m_AudioPlugin) {
        status += "Audio[" + m_AudioPlugin->GetPluginInfo().Name + "] ";
    } else {
        status += "Audio[None] ";
    }
    
    if (m_InputPlugin) {
        status += "Input[" + m_InputPlugin->GetPluginInfo().Name + "] ";
    } else {
        status += "Input[None] ";
    }
    
    if (m_RSPPlugin) {
        status += "RSP[" + m_RSPPlugin->GetPluginInfo().Name + "]";
    } else {
        status += "RSP[None]";
    }
    
    return status;
}

// ============================================================================
// Internal Plugin Creation
// ============================================================================

void PluginManager::CreateDefaultGraphicsPlugin()
{
    Log::info("PluginManager: Creating NovaGraphics graphics plugin");
    m_GraphicsPlugin = ::Nova64::Video::CreateNovaGraphicsPlugin();
}

void PluginManager::CreateDefaultAudioPlugin()
{
    Log::info("PluginManager: Creating NovaAudio audio plugin");
    m_AudioPlugin = ::Nova64::Audio::CreateNovaAudioPlugin();
}

void PluginManager::CreateDefaultInputPlugin()
{
    Log::info("PluginManager: Creating default input plugin");
    m_InputPlugin = ::Plugins::CreateDefaultInputPlugin();
}

void PluginManager::CreateDefaultRSPPlugin()
{
    Log::info("PluginManager: Creating default RSP plugin");
    m_RSPPlugin = ::Plugins::CreateDefaultRSPPlugin();
}

std::unique_ptr<PluginBase> PluginManager::LoadPluginFromFile(const std::string& path, PluginType type)
{
    Log::infof("PluginManager: Loading plugin from file: %s", path.c_str());
    
    switch (type) {
        case PluginType::Graphics:
            return ::Plugins::CreateGFXPluginFromDLL(path);
        case PluginType::Audio:
            return ::Plugins::CreateAudioPluginFromDLL(path);
        case PluginType::Input:
            return ::Plugins::CreateInputPluginFromDLL(path);
        case PluginType::RSP:
            return ::Plugins::CreateRSPPluginFromDLL(path);
        default:
            Log::warningf("PluginManager: Unknown plugin type %d", static_cast<int>(type));
            return nullptr;
    }
}

std::string PluginManager::GetDefaultPluginPath(PluginType type) const
{
    return "";
}

} // namespace Plugins
