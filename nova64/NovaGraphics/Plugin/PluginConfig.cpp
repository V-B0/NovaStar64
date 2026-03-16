// ══════════════════════════════════════════════════════════════════════════════
// NovaGraphics — Plugin Configuration Implementation
// ══════════════════════════════════════════════════════════════════════════════

#include "PluginConfig.hpp"
#include "../Common/Logger.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>

namespace Nova64::Video {

void PluginConfig::LoadDefaults() {
    *this = PluginConfig{};
}

void PluginConfig::Clamp() {
    windowWidth        = std::max(320, std::min(windowWidth, 7680));
    windowHeight       = std::max(240, std::min(windowHeight, 4320));
    internalResScale   = std::max(1, std::min(internalResScale, 8));
    backendType        = std::max(0, std::min(backendType, 2));
    textureCacheSizeMB = std::max(16, std::min(textureCacheSizeMB, 2048));
    gamma              = std::max(0.5f, std::min(gamma, 3.0f));
    bloomIntensity     = std::max(0.0f, std::min(bloomIntensity, 2.0f));
    sharpenStrength    = std::max(0.0f, std::min(sharpenStrength, 1.0f));
    lodBias            = std::max(-4.0f, std::min(lodBias, 4.0f));
    stretchFactor      = std::max(0.5f, std::min(stretchFactor, 2.0f));
    osdScale           = std::max(0.5f, std::min(osdScale, 3.0f));
    targetFPS          = std::max(0, std::min(targetFPS, 240));
    threadCount        = std::max(0, std::min(threadCount, 16));
}

bool PluginConfig::Validate() const {
    if (windowWidth < 320 || windowHeight < 240) return false;
    if (internalResScale < 1 || internalResScale > 8) return false;
    if (backendType < 0 || backendType > 2) return false;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  INI Serialization
// ═══════════════════════════════════════════════════════════════

bool PluginConfig::SetValue(const std::string& key, const std::string& val) {
    try {
        // Resolution
        if (key == "windowWidth")        { windowWidth = std::stoi(val); return true; }
        if (key == "windowHeight")       { windowHeight = std::stoi(val); return true; }
        if (key == "internalResScale")   { internalResScale = std::stoi(val); return true; }
        if (key == "fullscreen")         { fullscreen = (val == "1" || val == "true"); return true; }
        if (key == "fullscreenMonitor")  { fullscreenMonitor = std::stoi(val); return true; }
        if (key == "vsync")              { vsync = (val == "1" || val == "true"); return true; }
        if (key == "targetFPS")          { targetFPS = std::stoi(val); return true; }

        // Backend
        if (key == "backendType")        { backendType = std::stoi(val); return true; }

        // AA
        if (key == "antiAliasing")       { antiAliasing = static_cast<AAMode>(std::stoi(val)); return true; }

        // Filtering
        if (key == "texFilter")          { texFilter = static_cast<TextureFilterMode>(std::stoi(val)); return true; }
        if (key == "enableMipmaps")      { enableMipmaps = (val == "1" || val == "true"); return true; }
        if (key == "lodBias")            { lodBias = std::stof(val); return true; }

        // HiRes
        if (key == "hiResTexEnabled")    { hiResTexEnabled = (val == "1" || val == "true"); return true; }
        if (key == "hiResFormat")        { hiResFormat = static_cast<HiResFormat>(std::stoi(val)); return true; }
        if (key == "hiResTexPath")       { hiResTexPath = val; return true; }

        // Texture cache
        if (key == "textureCacheSizeMB") { textureCacheSizeMB = std::stoi(val); return true; }
        if (key == "textureDump")        { textureDump = (val == "1" || val == "true"); return true; }
        if (key == "textureDumpPath")    { textureDumpPath = val; return true; }

        // Framebuffer
        if (key == "fbEmulation")        { fbEmulation = (val == "1" || val == "true"); return true; }
        if (key == "fbReadAccess")       { fbReadAccess = (val == "1" || val == "true"); return true; }
        if (key == "fbWriteAccess")      { fbWriteAccess = (val == "1" || val == "true"); return true; }
        if (key == "depthCopy")          { depthCopy = (val == "1" || val == "true"); return true; }
        if (key == "nativeResFB")        { nativeResFB = (val == "1" || val == "true"); return true; }

        // Depth
        if (key == "depthCompare")       { depthCompare = (val == "1" || val == "true"); return true; }
        if (key == "n64DepthCompare")    { n64DepthCompare = (val == "1" || val == "true"); return true; }

        // Aspect
        if (key == "aspectRatio")        { aspectRatio = static_cast<AspectRatio>(std::stoi(val)); return true; }
        if (key == "stretchFactor")      { stretchFactor = std::stof(val); return true; }

        // Post
        if (key == "postFilter")         { postFilter = static_cast<PostFilter>(std::stoi(val)); return true; }
        if (key == "gamma")              { gamma = std::stof(val); return true; }
        if (key == "bloomIntensity")     { bloomIntensity = std::stof(val); return true; }
        if (key == "sharpenStrength")    { sharpenStrength = std::stof(val); return true; }

        // Dithering
        if (key == "ditheringEnabled")   { ditheringEnabled = (val == "1" || val == "true"); return true; }
        if (key == "ditheringPattern")   { ditheringPattern = std::stoi(val); return true; }

        // Fog
        if (key == "fogEnabled")         { fogEnabled = (val == "1" || val == "true"); return true; }
        if (key == "fogMode")            { fogMode = std::stoi(val); return true; }

        // Performance
        if (key == "multiThreading")     { multiThreading = (val == "1" || val == "true"); return true; }
        if (key == "asyncTextureLoading"){ asyncTextureLoading = (val == "1" || val == "true"); return true; }
        if (key == "threadCount")        { threadCount = std::stoi(val); return true; }
        if (key == "batchDrawCalls")     { batchDrawCalls = (val == "1" || val == "true"); return true; }
        if (key == "skipUnchangedFrames"){ skipUnchangedFrames = (val == "1" || val == "true"); return true; }

        // Overlay
        if (key == "showFPS")            { showFPS = (val == "1" || val == "true"); return true; }
        if (key == "showDebugInfo")      { showDebugInfo = (val == "1" || val == "true"); return true; }
        if (key == "showVRAM")           { showVRAM = (val == "1" || val == "true"); return true; }
        if (key == "wireframe")          { wireframe = (val == "1" || val == "true"); return true; }
        if (key == "debugOverlayPanels") { debugOverlayPanels = std::stoi(val); return true; }

        // OSD
        if (key == "osdColor")           { osdColor = static_cast<uint32_t>(std::stoul(val, nullptr, 16)); return true; }
        if (key == "osdScale")           { osdScale = std::stof(val); return true; }
        if (key == "osdPosition")        { osdPosition = std::stoi(val); return true; }

        // Screenshot
        if (key == "screenshotPath")     { screenshotPath = val; return true; }
        if (key == "screenshotFormat")   { screenshotFormat = std::stoi(val); return true; }

    } catch (...) {
        return false;
    }
    return false;
}

std::string PluginConfig::GetValue(const std::string& key) const {
    if (key == "windowWidth")        return std::to_string(windowWidth);
    if (key == "windowHeight")       return std::to_string(windowHeight);
    if (key == "internalResScale")   return std::to_string(internalResScale);
    if (key == "fullscreen")         return fullscreen ? "1" : "0";
    if (key == "vsync")              return vsync ? "1" : "0";
    if (key == "backendType")        return std::to_string(backendType);
    if (key == "antiAliasing")       return std::to_string(static_cast<int>(antiAliasing));
    if (key == "texFilter")          return std::to_string(static_cast<int>(texFilter));
    if (key == "gamma")              return std::to_string(gamma);
    if (key == "fbEmulation")        return fbEmulation ? "1" : "0";
    if (key == "showFPS")            return showFPS ? "1" : "0";
    // ... other getters follow same pattern
    return "";
}

bool PluginConfig::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    std::string currentSection;

    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments
        if (line[0] == '#' || line[0] == ';') continue;

        // Section header
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                currentSection = line.substr(1, end - 1);
            }
            continue;
        }

        // Key = value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));

        // Strip inline comment
        size_t comment = val.find('#');
        if (comment != std::string::npos) {
            val = val.substr(0, comment);
            val.erase(val.find_last_not_of(" \t") + 1);
        }

        SetValue(key, val);
    }

    Clamp();
    LOG_INFO("Config loaded from {}", path);
    return true;
}

bool PluginConfig::SaveToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file) return false;

    file << "# NovaGraphics Configuration\n";
    file << "# Auto-generated — edit with caution\n\n";

    file << "[Resolution]\n";
    file << "windowWidth = " << windowWidth << "\n";
    file << "windowHeight = " << windowHeight << "\n";
    file << "internalResScale = " << internalResScale << "\n";
    file << "fullscreen = " << (fullscreen ? "1" : "0") << "\n";
    file << "fullscreenMonitor = " << fullscreenMonitor << "\n";
    file << "vsync = " << (vsync ? "1" : "0") << "\n";
    file << "targetFPS = " << targetFPS << "\n\n";

    file << "[Backend]\n";
    file << "backendType = " << backendType << "  # 0=GL, 1=VK, 2=D3D\n\n";

    file << "[AntiAliasing]\n";
    file << "antiAliasing = " << static_cast<int>(antiAliasing) << "\n\n";

    file << "[Textures]\n";
    file << "texFilter = " << static_cast<int>(texFilter) << "\n";
    file << "enableMipmaps = " << (enableMipmaps ? "1" : "0") << "\n";
    file << "lodBias = " << lodBias << "\n";
    file << "hiResTexEnabled = " << (hiResTexEnabled ? "1" : "0") << "\n";
    file << "hiResFormat = " << static_cast<int>(hiResFormat) << "\n";
    file << "hiResTexPath = " << hiResTexPath << "\n";
    file << "textureCacheSizeMB = " << textureCacheSizeMB << "\n";
    file << "textureDump = " << (textureDump ? "1" : "0") << "\n";
    file << "textureDumpPath = " << textureDumpPath << "\n\n";

    file << "[Framebuffer]\n";
    file << "fbEmulation = " << (fbEmulation ? "1" : "0") << "\n";
    file << "fbReadAccess = " << (fbReadAccess ? "1" : "0") << "\n";
    file << "fbWriteAccess = " << (fbWriteAccess ? "1" : "0") << "\n";
    file << "depthCopy = " << (depthCopy ? "1" : "0") << "\n";
    file << "nativeResFB = " << (nativeResFB ? "1" : "0") << "\n\n";

    file << "[Depth]\n";
    file << "depthCompare = " << (depthCompare ? "1" : "0") << "\n";
    file << "n64DepthCompare = " << (n64DepthCompare ? "1" : "0") << "\n\n";

    file << "[Display]\n";
    file << "aspectRatio = " << static_cast<int>(aspectRatio) << "\n";
    file << "stretchFactor = " << stretchFactor << "\n\n";

    file << "[PostProcessing]\n";
    file << "postFilter = " << static_cast<int>(postFilter) << "\n";
    file << "gamma = " << gamma << "\n";
    file << "bloomIntensity = " << bloomIntensity << "\n";
    file << "sharpenStrength = " << sharpenStrength << "\n\n";

    file << "[Rendering]\n";
    file << "ditheringEnabled = " << (ditheringEnabled ? "1" : "0") << "\n";
    file << "ditheringPattern = " << ditheringPattern << "\n";
    file << "fogEnabled = " << (fogEnabled ? "1" : "0") << "\n";
    file << "fogMode = " << fogMode << "\n\n";

    file << "[Performance]\n";
    file << "multiThreading = " << (multiThreading ? "1" : "0") << "\n";
    file << "asyncTextureLoading = " << (asyncTextureLoading ? "1" : "0") << "\n";
    file << "threadCount = " << threadCount << "\n";
    file << "batchDrawCalls = " << (batchDrawCalls ? "1" : "0") << "\n";
    file << "skipUnchangedFrames = " << (skipUnchangedFrames ? "1" : "0") << "\n\n";

    file << "[Debug]\n";
    file << "showFPS = " << (showFPS ? "1" : "0") << "\n";
    file << "showDebugInfo = " << (showDebugInfo ? "1" : "0") << "\n";
    file << "showVRAM = " << (showVRAM ? "1" : "0") << "\n";
    file << "wireframe = " << (wireframe ? "1" : "0") << "\n";
    file << "debugOverlayPanels = " << debugOverlayPanels << "\n\n";

    file << "[OSD]\n";
    file << "osdColor = " << std::hex << osdColor << "\n";
    file << "osdScale = " << std::dec << osdScale << "\n";
    file << "osdPosition = " << osdPosition << "\n\n";

    file << "[Screenshot]\n";
    file << "screenshotPath = " << screenshotPath << "\n";
    file << "screenshotFormat = " << screenshotFormat << "\n";

    return true;
}

} // namespace Nova64::Video
