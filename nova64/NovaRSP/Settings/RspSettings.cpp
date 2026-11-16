/**
 * @file RspSettings.cpp
 * @brief RSP Settings Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include <cstdlib>
#include <cstring>
#include "RspSettings.hpp"
#include "GameDB.hpp"
#include "../../common/IniFile.hpp"
#include <cstdio>
#include <filesystem>

namespace Nova64::RSP {

// --- Static member definitions ---

bool CRSPSettings::s_DebuggingEnabled = false;
bool CRSPSettings::s_RomOpen = false;
RSPCpuMethod CRSPSettings::s_CPUMethod = RSPCpuMethod::Interpreter;
bool CRSPSettings::s_GraphicsHle = true;
bool CRSPSettings::s_AudioHle = false;
bool CRSPSettings::s_Profiling = false;
bool CRSPSettings::s_LogAsmCode = false;
bool CRSPSettings::s_SyncCPU = false;
bool CRSPSettings::s_RspMultiThreaded = false;
bool CRSPSettings::s_ConditionalMove = false;
bool CRSPSettings::s_ShowErrors = false;
bool CRSPSettings::s_BreakOnStart = false;
bool CRSPSettings::s_LogRDP = false;
bool CRSPSettings::s_IndividualBlock = false;
bool CRSPSettings::s_UserGraphicsHleSet = false;
bool CRSPSettings::s_UserAudioHleSet = false;
bool CRSPSettings::s_UserCPUMethodSet = false;
RSPEngine CRSPSettings::s_Engine = RSPEngine::Recompiler;
bool CRSPSettings::s_UserEngineSet = false;

// --- Initialize default settings ---

void CRSPSettings::InitializeRspSetting() {
    // Respect sticky user overrides set before this point (e.g. video plugin
    // applied NovaGraphics.ini renderingMode before MainSYS called us).
    if (!s_UserCPUMethodSet) s_CPUMethod = RSPCpuMethod::HighLevelEmulation;
    if (!s_UserGraphicsHleSet) s_GraphicsHle = true;
    if (!s_UserAudioHleSet) s_AudioHle = true;
    if (!s_UserEngineSet) {
        // NOVA64_RSP_ENGINE=recompiler|interpreter, for development and testing.
        const char* v = std::getenv("NOVA64_RSP_ENGINE");
        if (v && std::strcmp(v, "interpreter") == 0) s_Engine = RSPEngine::Interpreter;
        else if (v && (std::strcmp(v, "recompiler") == 0 || std::strcmp(v, "jit") == 0))
            s_Engine = RSPEngine::Recompiler;
    }
    s_Profiling = false;
    s_LogAsmCode = false;
    s_SyncCPU = false;
    s_RspMultiThreaded = false;
    s_ConditionalMove = false;
    s_ShowErrors = false;
    s_BreakOnStart = false;
    s_LogRDP = false;
    s_IndividualBlock = false;
    s_DebuggingEnabled = false;
    s_RomOpen = false;
}

// Refresh settings from config
// Reads from CSettings or IniFile if available

void CRSPSettings::RefreshSettings() {
    // Attempt to read RSP settings from config file
    static const char* kSection = "RSP";

    std::filesystem::path configPath = std::filesystem::current_path() / "NovaStar64.ini";

    if (std::filesystem::exists(configPath)) {
        CIniFile config(configPath, true /* readOnly */, false /* don't create */);
        if (config.IsFileOpen()) {
            // CPU method: 0=Interpreter, 2=HleWithFallback, 3=HLE
            auto cpuMethod = config.GetNumber(kSection, "CPUMethod");
            if (cpuMethod.has_value()) {
                switch (cpuMethod.value()) {
                case 0:  s_CPUMethod = RSPCpuMethod::Interpreter; break;
                case 2:  s_CPUMethod = RSPCpuMethod::HleWithFallback; break;
                case 3:  s_CPUMethod = RSPCpuMethod::HighLevelEmulation; break;
                default: s_CPUMethod = RSPCpuMethod::Interpreter; break;
                }
            }

            auto graphicsHle = config.GetBool(kSection, "GraphicsHLE");
            if (graphicsHle.has_value()) s_GraphicsHle = graphicsHle.value();

            auto audioHle = config.GetBool(kSection, "AudioHLE");
            if (audioHle.has_value()) s_AudioHle = audioHle.value();

            auto profiling = config.GetBool(kSection, "Profiling");
            if (profiling.has_value()) s_Profiling = profiling.value();

            auto logAsm = config.GetBool(kSection, "LogAsmCode");
            if (logAsm.has_value()) s_LogAsmCode = logAsm.value();

            auto syncCpu = config.GetBool(kSection, "SyncCPU");
            if (syncCpu.has_value()) s_SyncCPU = syncCpu.value();

            auto multiThread = config.GetBool(kSection, "MultiThreaded");
            if (multiThread.has_value()) s_RspMultiThreaded = multiThread.value();

            auto showErrors = config.GetBool(kSection, "ShowErrors");
            if (showErrors.has_value()) s_ShowErrors = showErrors.value();

            auto logRDP = config.GetBool(kSection, "LogRDP");
            if (logRDP.has_value()) s_LogRDP = logRDP.value();

            auto indBlock = config.GetBool(kSection, "IndividualBlock");
            if (indBlock.has_value()) s_IndividualBlock = indBlock.value();
        }
    }

    // Validate CPU method regardless of config read
    switch (s_CPUMethod) {
    case RSPCpuMethod::Interpreter:
    case RSPCpuMethod::HleWithFallback:
    case RSPCpuMethod::HighLevelEmulation: break;
    default:                               s_CPUMethod = RSPCpuMethod::Interpreter; break;
    }
}

// --- ROM open/close state ---

void CRSPSettings::SetRomOpen(bool opened) {
    s_RomOpen = opened;
    if (opened) {
        RefreshSettings();
    }
}

// --- Per-game overrides from ROM CRC database ---

void CRSPSettings::ApplyPerGameOverrides(uint32_t crc1, uint32_t crc2) {
    const GameOverrides* overrides = LookupGameOverrides(crc1, crc2);
    uint32_t matchedCrc1 = crc1;
    uint32_t matchedCrc2 = crc2;

    if (overrides == nullptr) {
        const uint32_t swappedCrc1 = ((crc1 & 0x000000FFu) << 24) | ((crc1 & 0x0000FF00u) << 8) |
                                     ((crc1 & 0x00FF0000u) >> 8) | ((crc1 & 0xFF000000u) >> 24);
        const uint32_t swappedCrc2 = ((crc2 & 0x000000FFu) << 24) | ((crc2 & 0x0000FF00u) << 8) |
                                     ((crc2 & 0x00FF0000u) >> 8) | ((crc2 & 0xFF000000u) >> 24);
        overrides = LookupGameOverrides(swappedCrc1, swappedCrc2);
        matchedCrc1 = swappedCrc1;
        matchedCrc2 = swappedCrc2;
    }

    if (overrides == nullptr) return;

    std::fprintf(stderr, "RSP: Per-game overrides for '%s' (CRC %08X/%08X)\n", overrides->name ? overrides->name : "?",
                 matchedCrc1, matchedCrc2);

    if (overrides->graphicsHle.has_value()) {
        if (s_UserGraphicsHleSet) {
            std::fprintf(stderr, "RSP:   GraphicsHLE override IGNORED (user set %s)\n",
                         s_GraphicsHle ? "true" : "false");
        } else {
            s_GraphicsHle = overrides->graphicsHle.value();
            std::fprintf(stderr, "RSP:   GraphicsHLE = %s\n", s_GraphicsHle ? "true" : "false");
        }
    }
    if (overrides->audioHle.has_value()) {
        if (s_UserAudioHleSet) {
            std::fprintf(stderr, "RSP:   AudioHLE override IGNORED (user set %s)\n", s_AudioHle ? "true" : "false");
        } else {
            s_AudioHle = overrides->audioHle.value();
            std::fprintf(stderr, "RSP:   AudioHLE = %s\n", s_AudioHle ? "true" : "false");
        }
    }
    if (overrides->cpuMethod.has_value()) {
        if (s_UserCPUMethodSet) {
            std::fprintf(stderr, "RSP:   CPUMethod override IGNORED (user set %u)\n",
                         static_cast<uint32_t>(s_CPUMethod));
        } else {
            switch (overrides->cpuMethod.value()) {
            case 0:  s_CPUMethod = RSPCpuMethod::Interpreter; break;
            case 2:  s_CPUMethod = RSPCpuMethod::HleWithFallback; break;
            case 3:  s_CPUMethod = RSPCpuMethod::HighLevelEmulation; break;
            default: break;
            }
            std::fprintf(stderr, "RSP:   CPUMethod = %u\n", static_cast<uint32_t>(s_CPUMethod));
        }
    }
}

// --- Debug toggle ---

void CRSPSettings::EnableDebugging(bool enabled) { s_DebuggingEnabled = enabled; }

} // namespace Nova64::RSP
