/**
 * @file SaveState.cpp
 * @brief SaveState manager implementation — uses the g_System global.
 */
#include "SaveState.hpp"
#include "MainSYS.hpp"

#include <filesystem>
#include <ctime>
#include <chrono>

// Forward declare the global system pointer from MainSYS.cpp
extern CN64System* g_System;

namespace NovaUI {

std::string SaveSlotInfo::TimestampStr() const {
    if (!hasData) return "(empty)";
    char buf[64];
    std::tm tm_info = {};
#ifdef _WIN32
    localtime_s(&tm_info, &timestamp);
#else
    localtime_r(&timestamp, &tm_info);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
}

void SaveStateManager::SetRom(const std::string& romPath) {
    namespace fs = std::filesystem;
    std::string stem = fs::path(romPath).stem().string();
    for (char& c : stem) if (c == ' ') c = '_';

    fs::create_directories("saves");
    m_romBase = "saves/" + stem;
    RefreshSlotInfo();
}

void SaveStateManager::RefreshSlotInfo() {
    namespace fs = std::filesystem;
    for (int i = 0; i < kSaveSlots; ++i) {
        std::string path = m_romBase + "_slot" + std::to_string(i) + ".nss";
        m_slots[i].filePath = path;
        if (fs::exists(path)) {
            m_slots[i].hasData = true;
            auto ftime = fs::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            m_slots[i].timestamp = std::chrono::system_clock::to_time_t(sctp);
        } else {
            m_slots[i].hasData   = false;
            m_slots[i].timestamp = 0;
        }
    }
}

bool SaveStateManager::Save(int slot) {
    if (slot < 0 || slot >= kSaveSlots) return false;
    if (m_romBase.empty()) return false;
    if (!g_System) return false;

    std::string path = m_romBase + "_slot" + std::to_string(slot) + ".nss";
    bool ok = g_System->SaveState(path.c_str());
    if (ok) RefreshSlotInfo();
    return ok;
}

bool SaveStateManager::Load(int slot) {
    if (slot < 0 || slot >= kSaveSlots) return false;
    if (!m_slots[slot].hasData) return false;
    if (!g_System) return false;

    return g_System->LoadState(m_slots[slot].filePath.c_str());
}

} // namespace NovaUI
