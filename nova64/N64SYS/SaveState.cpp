/**
 * @file SaveState.cpp
 * @brief Save-state slots and rewind history.
 */
#include "SaveState.hpp"

#include "MainSYS.hpp"
#include "N64Rom.hpp"
#include "../NovaGraphics/Plugin/FrameCallback.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace Nova64 {

namespace {

// The oldest rewind points are dropped past either limit. Neighbouring points
// share the RDRAM pages that didn't change, so the history holds far more than
// the budget divided by the 8 MB of RDRAM.
constexpr size_t kMaxRewindPoints    = 150;                 // five minutes
constexpr size_t kRewindMemoryBudget = 256u * 1024 * 1024;
constexpr int    kThumbnailWidth     = 256;

// A thumbnail of the last frame the video plugin handed to the launcher.
std::shared_ptr<const StateThumbnail> ThumbnailOfLastFrame()
{
    std::lock_guard<std::mutex> lock(Video::g_frameMutex);
    const int srcW = Video::g_frameW.load(std::memory_order_relaxed);
    const int srcH = Video::g_frameH.load(std::memory_order_relaxed);
    if (srcW <= 0 || srcH <= 0 ||
        Video::g_framePixels.size() < static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4) {
        return nullptr;
    }

    auto thumb = std::make_shared<StateThumbnail>();
    thumb->width  = std::min(kThumbnailWidth, srcW);
    thumb->height = std::max(1, thumb->width * srcH / srcW);
    thumb->rgba.resize(static_cast<size_t>(thumb->width) * static_cast<size_t>(thumb->height) * 4);

    // Box filter: each thumbnail pixel is the average of the pixels it covers.
    const uint8_t* src = Video::g_framePixels.data();
    uint8_t* dst = thumb->rgba.data();
    for (int y = 0; y < thumb->height; ++y) {
        const int y0 = y * srcH / thumb->height;
        const int y1 = std::max(y0 + 1, (y + 1) * srcH / thumb->height);
        for (int x = 0; x < thumb->width; ++x) {
            const int x0 = x * srcW / thumb->width;
            const int x1 = std::max(x0 + 1, (x + 1) * srcW / thumb->width);
            uint32_t sum[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src + static_cast<size_t>(sy) * static_cast<size_t>(srcW) * 4;
                for (int sx = x0; sx < x1; ++sx) {
                    sum[0] += row[sx * 4];
                    sum[1] += row[sx * 4 + 1];
                    sum[2] += row[sx * 4 + 2];
                }
            }
            const uint32_t count = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            *dst++ = static_cast<uint8_t>(sum[0] / count);
            *dst++ = static_cast<uint8_t>(sum[1] / count);
            *dst++ = static_cast<uint8_t>(sum[2] / count);
            *dst++ = 255;
        }
    }
    return thumb;
}

} // namespace

std::string SaveSlotInfo::SavedAtText() const
{
    if (!exists) return "Empty";
    if (!loadable) return "Made by an older version";
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &savedAt);
#else
    localtime_r(&savedAt, &local);
#endif
    char text[64] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

StateService& StateService::Instance()
{
    static StateService instance;
    return instance;
}

// ============================================================================
// Launcher thread
// ============================================================================

void StateService::BeginSession(const std::string& romPath)
{
    std::string stem = fs::path(romPath).stem().string();
    std::replace(stem.begin(), stem.end(), ' ', '_');
    std::error_code ec;
    fs::create_directories("saves", ec);
    const std::string base = "saves/" + stem;

    std::array<SaveSlotInfo, kSaveSlots> slots;
    for (int i = 0; i < kSaveSlots; ++i) {
        const std::string path = base + "_slot" + std::to_string(i) + ".nss";
        slots[i].exists = fs::exists(path, ec);
        if (slots[i].exists) slots[i].loadable = ReadStateFileSummary(path, slots[i].savedAt, slots[i].thumbnail);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_slotBase = base;
    m_slots = std::move(slots);
    m_requests.clear();
    m_history.clear();
    m_message.clear();
    m_requestPending.store(false);
    m_nextCaptureFrame.store(0);
    m_currentFrame.store(0);
    m_sessionActive.store(true);
    m_revision.fetch_add(1);
}

void StateService::EndSession()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessionActive.store(false);
    m_requests.clear();
    m_requestPending.store(false);
    m_history.clear();
    m_revision.fetch_add(1);
}

void StateService::SetRewindEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rewindEnabled.store(enabled);
    if (!enabled) m_history.clear();
    m_revision.fetch_add(1);
}

void StateService::Enqueue(const Request& request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sessionActive.load()) return;
    m_requests.push_back(request);
    m_requestPending.store(true);
}

void StateService::RequestSave(int slot)
{
    if (slot >= 0 && slot < kSaveSlots) Enqueue({Request::Kind::Save, slot, 0});
}

void StateService::RequestLoad(int slot)
{
    if (slot >= 0 && slot < kSaveSlots) Enqueue({Request::Kind::Load, slot, 0});
}

void StateService::RequestRewind(uint64_t pointId)
{
    Enqueue({Request::Kind::Rewind, 0, pointId});
}

SaveSlotInfo StateService::Slot(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (slot >= 0 && slot < kSaveSlots) ? m_slots[slot] : SaveSlotInfo{};
}

std::vector<RewindPoint> StateService::RewindPoints() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<RewindPoint> points;
    points.reserve(m_history.size());
    for (const auto& entry : m_history) points.push_back(entry.point);
    return points;
}

std::string StateService::LastMessage(uint64_t& sequence) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    sequence = m_messageSequence;
    return m_message;
}

// ============================================================================
// Emulator thread
// ============================================================================

bool StateService::NeedsService(uint64_t frame) const
{
    if (!m_sessionActive.load(std::memory_order_relaxed)) return false;
    return Video::g_emuPaused.load(std::memory_order_relaxed) ||
           m_requestPending.load(std::memory_order_relaxed) ||
           (m_rewindEnabled.load(std::memory_order_relaxed) &&
            frame >= m_nextCaptureFrame.load(std::memory_order_relaxed));
}

void StateService::Service(CN64System& system)
{
    ServiceOnce(system);
    // The launcher's pause holds the CPU here, where requests can still be served.
    while (Video::g_emuPaused.load(std::memory_order_relaxed) &&
           !system.m_EndEmulation.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        ServiceOnce(system);
    }
}

void StateService::ServiceOnce(CN64System& system)
{
    if (!m_sessionActive.load()) return;
    if (auto* rom = system.GetRom(); rom && rom->IsLoaded()) {
        m_fieldsPerSecond.store(rom->GetImage().IsPal() ? 50 : 60);
    }

    std::vector<Request> requests;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        requests.swap(m_requests);
    }
    for (const Request& request : requests) {
        switch (request.kind) {
            case Request::Kind::Save:   SaveSlot(system, request.slot);    break;
            case Request::Kind::Load:   LoadSlot(system, request.slot);    break;
            case Request::Kind::Rewind: RewindTo(system, request.pointId); break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requestPending.store(!m_requests.empty());
    }

    const uint64_t frame = system.GetCurrentFrameCount();
    m_currentFrame.store(frame);
    if (m_rewindEnabled.load() && frame >= m_nextCaptureFrame.load()) CaptureRewindPoint(system, frame);
}

std::string StateService::SlotPath(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slotBase + "_slot" + std::to_string(slot) + ".nss";
}

void StateService::SaveSlot(CN64System& system, int slot)
{
    MachineSnapshot snapshot;
    RomHeader rom{};
    std::string error;
    const bool captured = GetRomHeader(system, rom) && CaptureMachineState(system, snapshot, nullptr);
    snapshot.thumbnail = ThumbnailOfLastFrame();
    const bool saved = captured && WriteStateFile(SlotPath(slot), snapshot, rom, &error);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (saved) {
        SaveSlotInfo& info = m_slots[slot];
        info.exists    = true;
        info.loadable  = true;
        info.savedAt   = std::time(nullptr);
        info.thumbnail = snapshot.thumbnail;
        SetMessageLocked("Saved to slot " + std::to_string(slot + 1));
    } else {
        SetMessageLocked(error.empty() ? "Couldn't save just now. Try again." : error);
    }
    m_revision.fetch_add(1);
}

void StateService::LoadSlot(CN64System& system, int slot)
{
    RomHeader rom{};
    MachineSnapshot snapshot;
    std::string error;
    bool loaded = GetRomHeader(system, rom) && ReadStateFile(SlotPath(slot), rom, snapshot, &error);
    if (loaded && !RestoreMachineState(system, snapshot)) {
        loaded = false;
        error = "That save state doesn't match this version of NovaStar64.";
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (loaded) {
        // The history belongs to the timeline the load just left. The next
        // point is taken once a frame of the loaded state has been drawn.
        m_history.clear();
        m_nextCaptureFrame.store(snapshot.frame + 1);
        SetMessageLocked("Loaded slot " + std::to_string(slot + 1));
    } else {
        SetMessageLocked(error.empty() ? "Couldn't load that slot." : error);
    }
    m_revision.fetch_add(1);
}

void StateService::RewindTo(CN64System& system, uint64_t pointId)
{
    std::shared_ptr<const MachineSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_history.begin(), m_history.end(),
                                     [pointId](const HistoryEntry& e) { return e.point.id == pointId; });
        if (it != m_history.end()) snapshot = it->snapshot;
    }
    const bool restored = snapshot && RestoreMachineState(system, *snapshot);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (restored) {
        // The points after it show a future that no longer happens.
        while (!m_history.empty() && m_history.back().point.id != pointId) m_history.pop_back();
        const uint64_t interval = static_cast<uint64_t>(m_fieldsPerSecond.load()) * kRewindIntervalSeconds;
        m_nextCaptureFrame.store(snapshot->frame + interval);
    } else {
        SetMessageLocked("Couldn't rewind to that point.");
    }
    m_revision.fetch_add(1);
}

void StateService::CaptureRewindPoint(CN64System& system, uint64_t frame)
{
    std::shared_ptr<const MachineSnapshot> previous;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_history.empty()) previous = m_history.back().snapshot;
    }
    auto snapshot = std::make_shared<MachineSnapshot>();
    // Not at a boundary this time: try again on the next field.
    if (!CaptureMachineState(system, *snapshot, previous.get())) return;
    snapshot->thumbnail = ThumbnailOfLastFrame();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sessionActive.load()) return;
    HistoryEntry entry;
    entry.point.id        = m_nextPointId++;
    entry.point.frame     = frame;
    entry.point.thumbnail = snapshot->thumbnail;
    entry.snapshot        = std::move(snapshot);
    m_history.push_back(std::move(entry));
    TrimHistoryLocked();

    const uint64_t interval = static_cast<uint64_t>(m_fieldsPerSecond.load()) * kRewindIntervalSeconds;
    m_nextCaptureFrame.store(frame + interval);
    m_revision.fetch_add(1);
}

void StateService::TrimHistoryLocked()
{
    const auto memoryUse = [this]() {
        size_t bytes = 0;
        const MachineSnapshot* previous = nullptr;
        for (const auto& entry : m_history) {
            bytes += SnapshotMemoryUse(*entry.snapshot, previous);
            previous = entry.snapshot.get();
        }
        return bytes;
    };
    while (m_history.size() > kMaxRewindPoints) m_history.pop_front();
    while (m_history.size() > 1 && memoryUse() > kRewindMemoryBudget) m_history.pop_front();
}

void StateService::SetMessageLocked(std::string text)
{
    m_message = std::move(text);
    ++m_messageSequence;
}

} // namespace Nova64

void CN64System::ServiceStates()
{
    Nova64::StateService::Instance().Service(*this);
}
