/**
 * @file MachineState.cpp
 * @brief Machine snapshots and save-state files.
 */
#include "MachineState.hpp"
#include "StateArchive.hpp"

#include "MainSYS.hpp"
#include "N64Rom.hpp"
#include "MIPSY/Register.hpp"
#include "MIPSY/Sysevents.hpp"
#include "MIPSY/Systiming.hpp"
#include "MIPSY/TLB.hpp"
#include "MIPSY/Vmem.hpp"
#include "Savetypes/FRam.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

constexpr uint32_t kPageSize = 0x1000;

const std::shared_ptr<const Nova64::RdramPage>& ZeroPage() {
    static const std::shared_ptr<const Nova64::RdramPage> page = std::make_shared<const Nova64::RdramPage>();
    return page;
}

bool AtInstructionBoundary(const CN64System& system) {
    return system.m_PipelineStage == N64Types::PipelineStage::Normal ||
           system.m_PipelineStage == N64Types::PipelineStage::Jump;
}

} // namespace

// ============================================================================
// CN64System
// ============================================================================

void CN64System::SerializeMachineState(Nova64::StateArchive& ar) {
    CRegisters& reg = *m_Registers;

    ar.Section("CPU ");
    // The next instruction to run is PC, or a jump that is already pending (an
    // interrupt raised by the timer that just ran). Both engines apply a
    // pending jump before executing anything, so a load always sets one up.
    uint64_t nextPC = (m_PipelineStage == N64Types::PipelineStage::Jump) ? m_JumpToLocation : reg.m_PROGRAM_COUNTER;
    ar.Value(nextPC);
    if (ar.IsLoading()) {
        reg.m_PROGRAM_COUNTER = nextPC;
        m_JumpToLocation = nextPC;
        m_PipelineStage = N64Types::PipelineStage::Jump;
    }
    ar.Value(reg.m_GPR);
    ar.Value(reg.m_CP0);
    ar.Value(reg.m_CP0Latch);
    ar.Value(reg.m_CP2Latch);
    ar.Value(reg.m_HI);
    ar.Value(reg.m_LO);
    ar.Value(reg.m_LLBit);
    ar.Value(reg.m_FPCR);
    ar.Value(reg.m_FPR);
    ar.Value(reg.m_FPR_Union);

    ar.Section("REGS");
    ar.Value(reg.m_RDRAM_Registers);
    ar.Value(reg.m_SigProcessor_Interface);
    ar.Value(reg.m_Display_ControlReg);
    ar.Value(reg.m_Mips_Interface);
    ar.Value(reg.m_Video_Interface);
    ar.Value(reg.m_Audio_Interface);
    ar.Value(reg.m_Peripheral_Interface);
    ar.Value(reg.m_RDRAM_Interface);
    ar.Value(reg.m_SerialInterface);
    ar.Value(reg.m_DiskInterface);
    ar.Value(reg.m_AudioIntrReg);
    ar.Value(reg.m_GfxIntrReg);
    ar.Value(reg.m_RspIntrReg);
    ar.Value(reg.m_DpFreshFromHLE);
    ar.Value(reg.m_defer_hle_interrupts);
    ar.Value(reg.m_deferred_mi_intr);

    ar.Section("SYS ");
    ar.Value(m_NextTimer);
    ar.Value(m_TestTimer);
    ar.Value(m_DeferredCheckInterrupts);
    ar.Value(m_InterruptDelay);
    ar.Value(m_CyclesToSkip);
    ar.Value(m_TLBLoadAddress);
    ar.Value(m_TLBStoreAddress);
    ar.Value(m_Random);
    ar.Value(m_DDRegisters);

    m_SystemTimer->SerializeState(ar);
    m_TLB->SerializeState(ar);

    Nova64::MipsMemoryVM& memory = *m_Memory;
    memory.VideoInterface()->SerializeState(ar);
    memory.AudioInterface()->SerializeState(ar);
    memory.PeripheralInterface()->SerializeState(ar);
    memory.SerialInterface()->SerializeState(ar);
    memory.RDRAMInterface()->SerializeState(ar);
    memory.DPCommandRegisters()->SerializeState(ar);
    memory.SPRegisters()->SerializeState(ar);
    memory.PifRam()->SerializeState(ar);
    memory.GetFlashRam().SerializeState(ar);
}

void CN64System::FinishStateLoad() {
    // Compiled code and the self-modifying-code map describe the old RDRAM.
    FlushCompiledCode();
    std::memset(m_Memory->GetSmcPageBitmap(), 0, Nova64::MipsMemoryVM::kSmcChunks);
    // Anything still queued, such as a pending interrupt, belonged to the old state.
    m_SystemEvents->DeleteAllEvents();
}

namespace Nova64 {

// ============================================================================
// Snapshots
// ============================================================================

bool CaptureMachineState(CN64System& system, MachineSnapshot& out, const MachineSnapshot* previous) {
    MipsMemoryVM* memory = system.GetMemory();
    const uint8_t* rdram = memory ? memory->Rdram() : nullptr;
    if (!rdram || !AtInstructionBoundary(system)) return false;

    out.frame = system.GetCurrentFrameCount();
    out.core.clear();
    StateArchive ar = StateArchive::Writer(out.core);
    system.SerializeMachineState(ar);

    out.plugins.clear();
    if (auto* plugins = system.GetPluginManager()) plugins->SaveState(out.plugins);

    const uint32_t size = memory->RdramSize();
    const size_t pageCount = size / kPageSize;
    const bool share = previous && previous->rdramSize == size && previous->pages.size() == pageCount;
    const auto& zero = ZeroPage();
    out.rdramSize = size;
    out.pages.assign(pageCount, nullptr);
    for (size_t i = 0; i < pageCount; ++i) {
        const uint8_t* bytes = rdram + i * kPageSize;
        if (share && std::memcmp(previous->pages[i]->data(), bytes, kPageSize) == 0) {
            out.pages[i] = previous->pages[i];
        } else if (std::memcmp(zero->data(), bytes, kPageSize) == 0) {
            out.pages[i] = zero;
        } else {
            auto page = std::make_shared<RdramPage>();
            std::memcpy(page->data(), bytes, kPageSize);
            out.pages[i] = std::move(page);
        }
    }
    return true;
}

bool RestoreMachineState(CN64System& system, const MachineSnapshot& snapshot) {
    MipsMemoryVM* memory = system.GetMemory();
    uint8_t* rdram = memory ? memory->Rdram() : nullptr;
    if (!rdram || snapshot.rdramSize != memory->RdramSize() ||
        snapshot.pages.size() != snapshot.rdramSize / kPageSize) {
        return false;
    }

    // Check the layout against this build before touching the machine, so a
    // state that doesn't fit is refused rather than half applied.
    std::vector<uint8_t> current;
    StateArchive probe = StateArchive::Writer(current);
    system.SerializeMachineState(probe);
    if (!probe.SameLayoutAs(snapshot.core)) return false;

    StateArchive ar = StateArchive::Reader(snapshot.core);
    system.SerializeMachineState(ar);
    for (size_t i = 0; i < snapshot.pages.size(); ++i) {
        const auto& page = snapshot.pages[i];
        if (page) std::memcpy(rdram + i * kPageSize, page->data(), kPageSize);
        else std::memset(rdram + i * kPageSize, 0, kPageSize);
    }
    if (auto* plugins = system.GetPluginManager(); plugins && !snapshot.plugins.empty()) {
        plugins->LoadState(snapshot.plugins);
    }
    system.FinishStateLoad();
    return ar.Ok();
}

size_t SnapshotMemoryUse(const MachineSnapshot& snapshot, const MachineSnapshot* previous) {
    size_t bytes = snapshot.core.size() + snapshot.plugins.size();
    if (snapshot.thumbnail) bytes += snapshot.thumbnail->rgba.size();
    const bool share = previous && previous->pages.size() == snapshot.pages.size();
    const auto& zero = ZeroPage();
    for (size_t i = 0; i < snapshot.pages.size(); ++i) {
        const auto& page = snapshot.pages[i];
        if (page && page != zero && !(share && previous->pages[i] == page)) bytes += kPageSize;
    }
    return bytes;
}

bool GetRomHeader(const CN64System& system, RomHeader& out) {
    auto* rom = system.GetRom();
    if (!rom || !rom->IsLoaded() || rom->GetImage().Size() < out.size()) return false;
    std::memcpy(out.data(), rom->GetImage().Data(), out.size());
    return true;
}

// ============================================================================
// Files
// ============================================================================
//
//   "N64S"  u32 version (2)
//   64-byte ROM header          i64 time saved
//   u32 thumbnail width, height, then its RGBA bytes
//   u64 video field count
//   u32 size + core state       u32 size + plugin state
//   u32 RDRAM size, one byte per page (1 = stored), then the stored pages
//
// Version 1 files (registers and RDRAM only) can't be loaded: they were never
// complete enough to resume from.

namespace {

constexpr char kFileMagic[4] = {'N', '6', '4', 'S'};
constexpr uint32_t kFileVersion = 2;
constexpr uint32_t kMaxBlobSize = 16u * 1024 * 1024;
constexpr uint32_t kMaxThumbnailSide = 1024;
constexpr uint32_t kMaxRdramSize = 0x800000;

// CRC1/CRC2 and the game code identify the game; the rest of the header can
// differ between otherwise identical dumps.
bool SameGame(const RomHeader& a, const RomHeader& b) {
    return std::memcmp(a.data() + 0x10, b.data() + 0x10, 8) == 0 &&
           std::memcmp(a.data() + 0x38, b.data() + 0x38, 8) == 0;
}

template <class T> void Put(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <class T> bool Get(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

void PutBlob(std::ostream& out, const std::vector<uint8_t>& blob) {
    Put(out, static_cast<uint32_t>(blob.size()));
    out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
}

bool GetBlob(std::istream& in, std::vector<uint8_t>& blob) {
    uint32_t size = 0;
    if (!Get(in, size) || size > kMaxBlobSize) return false;
    blob.resize(size);
    in.read(reinterpret_cast<char*>(blob.data()), size);
    return static_cast<bool>(in);
}

enum class HeaderResult { Ok, NotAState, OldVersion, Damaged };

// Everything before the machine state: magic, version, game, time and thumbnail.
HeaderResult ReadHeader(std::istream& in, RomHeader& rom, std::time_t& savedAt,
                        std::shared_ptr<const StateThumbnail>& thumbnail) {
    char magic[4] = {};
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kFileMagic, sizeof(magic)) != 0)
        return HeaderResult::NotAState;
    uint32_t version = 0;
    if (!Get(in, version)) return HeaderResult::Damaged;
    if (version != kFileVersion) return HeaderResult::OldVersion;

    int64_t time = 0;
    uint32_t width = 0, height = 0;
    if (!in.read(reinterpret_cast<char*>(rom.data()), static_cast<std::streamsize>(rom.size())) || !Get(in, time) ||
        !Get(in, width) || !Get(in, height) || width > kMaxThumbnailSide || height > kMaxThumbnailSide) {
        return HeaderResult::Damaged;
    }
    auto thumb = std::make_shared<StateThumbnail>();
    thumb->width = static_cast<int>(width);
    thumb->height = static_cast<int>(height);
    thumb->rgba.resize(static_cast<size_t>(width) * height * 4);
    if (!thumb->rgba.empty() &&
        !in.read(reinterpret_cast<char*>(thumb->rgba.data()), static_cast<std::streamsize>(thumb->rgba.size()))) {
        return HeaderResult::Damaged;
    }
    savedAt = static_cast<std::time_t>(time);
    if (thumb->rgba.empty()) thumbnail.reset();
    else thumbnail = std::move(thumb);
    return HeaderResult::Ok;
}

} // namespace

bool WriteStateFile(const std::string& path, const MachineSnapshot& snapshot, const RomHeader& rom,
                    std::string* error) {
    std::error_code ec;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);

    // Written to a temporary file first, so a failed write can't destroy the
    // state already in the slot.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "Couldn't create " + path;
            return false;
        }
        out.write(kFileMagic, sizeof(kFileMagic));
        Put(out, kFileVersion);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
        Put(out, static_cast<int64_t>(std::time(nullptr)));

        const StateThumbnail* thumb = snapshot.thumbnail.get();
        const bool hasThumb = thumb && thumb->width > 0 && thumb->height > 0 &&
                              thumb->rgba.size() == static_cast<size_t>(thumb->width) * thumb->height * 4;
        Put(out, hasThumb ? static_cast<uint32_t>(thumb->width) : uint32_t{0});
        Put(out, hasThumb ? static_cast<uint32_t>(thumb->height) : uint32_t{0});
        if (hasThumb)
            out.write(reinterpret_cast<const char*>(thumb->rgba.data()),
                      static_cast<std::streamsize>(thumb->rgba.size()));

        Put(out, snapshot.frame);
        PutBlob(out, snapshot.core);
        PutBlob(out, snapshot.plugins);
        Put(out, snapshot.rdramSize);

        // All-zero pages, of which RDRAM has many, are marked rather than stored.
        const auto& zero = ZeroPage();
        std::vector<uint8_t> stored(snapshot.pages.size());
        for (size_t i = 0; i < stored.size(); ++i) stored[i] = (snapshot.pages[i] && snapshot.pages[i] != zero) ? 1 : 0;
        out.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        for (size_t i = 0; i < stored.size(); ++i) {
            if (stored[i]) out.write(reinterpret_cast<const char*>(snapshot.pages[i]->data()), kPageSize);
        }
        if (!out) {
            if (error) *error = "Couldn't write " + path;
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        if (error) *error = "Couldn't replace " + path;
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool ReadStateFile(const std::string& path, const RomHeader& rom, MachineSnapshot& out, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("That save slot is empty.");

    RomHeader fileRom{};
    std::time_t savedAt = 0;
    MachineSnapshot snapshot;
    switch (ReadHeader(in, fileRom, savedAt, snapshot.thumbnail)) {
    case HeaderResult::Ok:         break;
    case HeaderResult::OldVersion: return fail("That save state was made by an older version and can't be loaded.");
    default:                       return fail("That save state file is damaged.");
    }
    if (!SameGame(fileRom, rom)) return fail("That save state is from a different game.");

    uint32_t rdramSize = 0;
    if (!Get(in, snapshot.frame) || !GetBlob(in, snapshot.core) || !GetBlob(in, snapshot.plugins) ||
        !Get(in, rdramSize) || rdramSize > kMaxRdramSize || rdramSize % kPageSize != 0) {
        return fail("That save state file is damaged.");
    }
    snapshot.rdramSize = rdramSize;
    std::vector<uint8_t> stored(rdramSize / kPageSize);
    if (!in.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size())))
        return fail("That save state file is damaged.");

    snapshot.pages.resize(stored.size());
    for (size_t i = 0; i < stored.size(); ++i) {
        if (!stored[i]) {
            snapshot.pages[i] = ZeroPage();
            continue;
        }
        auto page = std::make_shared<RdramPage>();
        if (!in.read(reinterpret_cast<char*>(page->data()), kPageSize)) return fail("That save state file is damaged.");
        snapshot.pages[i] = std::move(page);
    }
    out = std::move(snapshot);
    return true;
}

bool ReadStateFileSummary(const std::string& path, std::time_t& savedAt,
                          std::shared_ptr<const StateThumbnail>& thumbnail) {
    std::ifstream in(path, std::ios::binary);
    RomHeader rom{};
    return in && ReadHeader(in, rom, savedAt, thumbnail) == HeaderResult::Ok;
}

} // namespace Nova64
