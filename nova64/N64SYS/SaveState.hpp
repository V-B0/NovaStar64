/**
 * @file SaveState.hpp
 * @brief Save-state slots and the rewind history for a game running in the launcher.
 *
 * The launcher asks for things (save to slot 2, rewind to that picture) from
 * its own thread. The emulator thread carries them out when it reaches an
 * instruction boundary, the only place where the whole machine state is in
 * memory rather than partly inside a compiled block. That boundary is also
 * where the game waits while the launcher has it paused.
 */
#pragma once

#include "MachineState.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CN64System;

namespace Nova64 {

constexpr int kSaveSlots = 4;

struct SaveSlotInfo {
    bool        exists   = false;   ///< a file is there
    bool        loadable = false;   ///< ...and this version can read it
    std::time_t savedAt  = 0;
    std::shared_ptr<const StateThumbnail> thumbnail;

    [[nodiscard]] std::string SavedAtText() const;
};

struct RewindPoint {
    uint64_t id    = 0;
    uint64_t frame = 0;   ///< video field count when it was taken
    std::shared_ptr<const StateThumbnail> thumbnail;
};

class StateService {
public:
    /// A rewind point is taken every this many seconds of play.
    static constexpr int kRewindIntervalSeconds = 2;

    static StateService& Instance();

    // ---- Launcher thread ----

    /// Starts a game: finds its save slots and empties the rewind history.
    void BeginSession(const std::string& romPath);
    /// Ends it. The rewind history is deleted.
    void EndSession();

    void RequestSave(int slot);
    void RequestLoad(int slot);
    void RequestRewind(uint64_t pointId);
    /// Rewind points are only taken while this is on. Turning it off deletes
    /// the history.
    void SetRewindEnabled(bool enabled);
    [[nodiscard]] bool RewindEnabled() const { return m_rewindEnabled.load(); }

    /// True from a request until the emulator has carried it out.
    [[nodiscard]] bool RequestPending() const { return m_requestPending.load(); }

    [[nodiscard]] SaveSlotInfo Slot(int slot) const;
    /// Oldest first.
    [[nodiscard]] std::vector<RewindPoint> RewindPoints() const;
    /// Changes whenever a slot or the rewind history changes.
    [[nodiscard]] uint64_t Revision() const { return m_revision.load(); }
    /// Video fields per second of the running game: 60, or 50 for PAL.
    [[nodiscard]] int FieldsPerSecond() const { return m_fieldsPerSecond.load(); }
    /// Video field count when the emulator last checked in.
    [[nodiscard]] uint64_t CurrentFrame() const { return m_currentFrame.load(); }
    /// Result of the last request ("Saved to slot 2"); `sequence` changes with
    /// every new message.
    [[nodiscard]] std::string LastMessage(uint64_t& sequence) const;

    // ---- Emulator thread ----

    /// Cheap check made once per video field: does Service() have work?
    [[nodiscard]] bool NeedsService(uint64_t frame) const;
    /// Carries out requests and takes rewind points. Waits here while paused.
    void Service(CN64System& system);

private:
    struct Request {
        enum class Kind { Save, Load, Rewind } kind;
        int      slot    = 0;
        uint64_t pointId = 0;
    };
    struct HistoryEntry {
        RewindPoint point;
        std::shared_ptr<const MachineSnapshot> snapshot;
    };

    StateService() = default;

    void Enqueue(const Request& request);
    void ServiceOnce(CN64System& system);
    void SaveSlot(CN64System& system, int slot);
    void LoadSlot(CN64System& system, int slot);
    void RewindTo(CN64System& system, uint64_t pointId);
    void CaptureRewindPoint(CN64System& system, uint64_t frame);
    void TrimHistoryLocked();
    void SetMessageLocked(std::string text);
    std::string SlotPath(int slot) const;

    mutable std::mutex m_mutex;
    std::string m_slotBase;   ///< "saves/<rom>"; slot N is "<base>_slotN.nss"
    std::array<SaveSlotInfo, kSaveSlots> m_slots;
    std::vector<Request> m_requests;
    std::deque<HistoryEntry> m_history;
    uint64_t m_nextPointId = 1;
    std::string m_message;
    uint64_t m_messageSequence = 0;

    std::atomic<bool>     m_sessionActive{false};
    std::atomic<bool>     m_rewindEnabled{true};
    std::atomic<bool>     m_requestPending{false};
    std::atomic<uint64_t> m_nextCaptureFrame{0};
    std::atomic<uint64_t> m_currentFrame{0};
    std::atomic<uint64_t> m_revision{1};
    std::atomic<int>      m_fieldsPerSecond{60};
};

} // namespace Nova64
