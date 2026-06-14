/**
 * @file SaveState.hpp
 * @brief 4-slot save state manager for embedded emulator sessions.
 *
 * Wraps CN64System::SaveState(path) / LoadState(path) via the
 * g_System global pointer defined in MainSYS.cpp.
 */
#pragma once

#include <string>
#include <array>
#include <ctime>

namespace NovaUI {

constexpr int kSaveSlots = 4;

struct SaveSlotInfo {
    bool        hasData   = false;
    std::string filePath;
    std::time_t timestamp = 0;

    std::string TimestampStr() const;
};

class SaveStateManager {
public:
    /// Call once when a ROM starts to set the base filename for slot files.
    void SetRom(const std::string& romPath);

    /// Save to slot [0..3]. Calls g_System->SaveState(path).
    bool Save(int slot);

    /// Load from slot [0..3]. Calls g_System->LoadState(path).
    bool Load(int slot);

    const SaveSlotInfo& GetSlot(int slot) const { return m_slots[slot]; }

    void RefreshSlotInfo();

private:
    std::string m_romBase;
    std::array<SaveSlotInfo, kSaveSlots> m_slots;
};

} // namespace NovaUI
