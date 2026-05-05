/**
 * @file TaskDispatcher.cpp
 * @brief RSP HLE microcode identification via CRC32 and task routing.
 */

#include "TaskDispatcher.hpp"
#include "hle.hpp"
#include "ucodes.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <set>

// Audio task dispatch counter
static uint32_t s_audioTaskCount = 0;

namespace Nova64::RSP::HLE {

// ──────────────────────────────────────────────
// OSTask parsing from DMEM 0xFC0
// ──────────────────────────────────────────────

TaskInfo CTaskDispatcher::ParseTask(const uint8_t* dmem)
{
    TaskInfo info{};
    if (dmem == nullptr) return info;

    info.Type           = *u32(dmem, TaskOffset::Type);
    info.Flags          = *u32(dmem, TaskOffset::Flags);
    info.Boot           = *u32(dmem, TaskOffset::UcodeBoot);
    info.BootSize       = *u32(dmem, TaskOffset::UcodeBootSize);
    info.Ucode          = *u32(dmem, TaskOffset::Ucode);
    info.UcodeSize      = *u32(dmem, TaskOffset::UcodeSize);
    info.UcodeData      = *u32(dmem, TaskOffset::UcodeData);
    info.UcodeDataSize  = *u32(dmem, TaskOffset::UcodeDataSize);
    info.DramStack      = *u32(dmem, TaskOffset::DramStack);
    info.DramStackSize  = *u32(dmem, TaskOffset::DramStackSize);
    info.OutputBuff     = *u32(dmem, TaskOffset::OutputBuff);
    info.OutputBuffSize = *u32(dmem, TaskOffset::OutputBuffSize);
    info.DataPtr        = *u32(dmem, TaskOffset::DataPtr);
    info.DataSize       = *u32(dmem, TaskOffset::DataSize);
    info.YieldDataPtr   = *u32(dmem, TaskOffset::YieldDataPtr);
    info.YieldDataSize  = *u32(dmem, TaskOffset::YieldDataSize);
    return info;
}

// ──────────────────────────────────────────────
// CRC32 computation from DRAM
// ──────────────────────────────────────────────

uint32_t CTaskDispatcher::ComputeUcodeCrc(CHle* hle, uint32_t addr, uint32_t size)
{
    if (hle->dram() == nullptr || addr == 0 || size == 0)
        return 0;

    const uint32_t masked = addr & 0x007FFFFF;
    const uint32_t safe_size = std::min(size, 0x1000u);
    return Crc32(hle->dram() + masked, safe_size);
}

uint32_t CTaskDispatcher::ComputeUcodeDataCrc(CHle* hle, uint32_t addr, uint32_t size)
{
    if (hle->dram() == nullptr || addr == 0 || size == 0)
        return 0;

    const uint32_t masked = addr & 0x007FFFFF;
    const uint32_t safe_size = std::min(size, 0x1000u);
    return Crc32(hle->dram() + masked, safe_size);
}

// ──────────────────────────────────────────────
// Microcode identification
// ──────────────────────────────────────────────

const MicrocodeEntry* CTaskDispatcher::IdentifyMicrocode(CHle* hle, const TaskInfo& task)
{
    // Quick cache check: same ucode address as last time?
    if (m_Cache.valid && task.UcodeData != 0)
    {
        auto it = m_UcodeCache.find(task.UcodeData);
        if (it != m_UcodeCache.end() && it->second.valid)
        {
            m_CacheHits++;
            return it->second.entry;
        }
    }

    // Compute both CRCs upfront
    uint32_t data_crc = ComputeUcodeDataCrc(hle, task.UcodeData, task.UcodeDataSize);
    uint32_t text_crc = ComputeUcodeCrc(hle, task.Ucode, task.UcodeSize);

    hle->VerboseMessage("RSP HLE: IdentifyMicrocode data_crc=0x%08X text_crc=0x%08X type=%u",
        data_crc, text_crc, task.Type);

    // Look up by data CRC first
    const MicrocodeEntry* entry = nullptr;
    if (data_crc != 0)
        entry = FindMicrocode(data_crc);

    // If not found by data CRC, try the ucode text CRC
    if (entry == nullptr && text_crc != 0)
        entry = FindMicrocode(text_crc, data_crc);

    // CRC-based lookup failed — try sum_bytes fallback detection.
    // Matches mupen64plus-rsp-hle try_normal_task_detection():
    // sum first half of microcode text bytes to identify known handlers.
    if (entry == nullptr && task.Ucode != 0 && task.UcodeSize > 0 && hle->dram() != nullptr)
    {
        entry = TrySumBytesFallback(hle, task);
    }

    // Cache the result (keyed on ucode data address for fast re-lookup)
    if (task.UcodeData != 0)
    {
        MicrocodeCache cache;
        cache.ucode_crc = text_crc;
        cache.data_crc = data_crc;
        cache.entry = entry;
        cache.valid = true;
        m_UcodeCache[task.UcodeData] = cache;
        m_Cache = cache;
    }

    if (entry != nullptr)
    {
        hle->VerboseMessage("RSP HLE: Identified microcode '%.*s' (data_crc=0x%08X, text_crc=0x%08X)",
            static_cast<int>(entry->name.size()), entry->name.data(), data_crc, text_crc);
    }
    else
    {
        // Warn only once per unique CRC pair (local dispatchers are recreated each call,
        // so the instance cache can't suppress repeated warnings)
        static std::set<std::pair<uint32_t,uint32_t>> s_warnedCrcs;
        auto key = std::make_pair(data_crc, text_crc);
        if (s_warnedCrcs.insert(key).second)
        {
            hle->WarnMessage("RSP HLE: Unknown microcode (data_crc=0x%08X, text_crc=0x%08X, addr=0x%08X, size=0x%X)",
                data_crc, text_crc, task.UcodeData, task.UcodeDataSize);
        }
    }

    return entry;
}

// ──────────────────────────────────────────────
// Main dispatch entry point
// ──────────────────────────────────────────────

bool CTaskDispatcher::DispatchTask(CHle* hle)
{
    if (hle->dmem() == nullptr) return false;

    // Check if this is a task (DMEM contains valid task structure)
    const uint32_t type = *u32(hle->dmem(), TaskOffset::Type);
    if (type == 0) return false;

    const TaskInfo task = ParseTask(hle->dmem());
    m_TasksDispatched++;

    hle->VerboseMessage("RSP HLE: DispatchTask #%u type=%u ucode=0x%08X data=0x%08X dataptr=0x%08X",
        m_TasksDispatched, type, task.Ucode, task.UcodeData, task.DataPtr);

    switch (type)
    {
    case 1: // Video task
        return DispatchVideoTask(hle, task);

    case 2: // Audio task
    {
        const MicrocodeEntry* ucode = IdentifyMicrocode(hle, task);
        return DispatchAudioTask(hle, task, ucode);
    }

    case 4: // JPEG task (some games use type 4)
    {
        const MicrocodeEntry* ucode = IdentifyMicrocode(hle, task);
        return DispatchJpegTask(hle, task, ucode);
    }

    case 7: // CIC x105 boot task
        cicx105_ucode(hle);
        return true;

    default:
        hle->WarnMessage("RSP HLE: Unknown task type %u", type);
        m_TasksFallback++;
        return false;
    }
}

// ──────────────────────────────────────────────
// Video task dispatch
// ──────────────────────────────────────────────

bool CTaskDispatcher::DispatchVideoTask(CHle* hle, const TaskInfo& task)
{
    if (!hle->GraphicsHle())
    {
        hle->VerboseMessage("RSP HLE: GraphicsHle disabled, skipping DispatchVideoTask");
        return false;
    }

    // Always forward to the GFX plugin's ProcessDList.
    // The VideoPlugin has a complete display list processor (F3DEX2Handler)
    // with OpenGL rendering. The internal GfxProcessor generates RDP commands
    // to RDRAM but ProcessRdpList doesn't render them, so skip it entirely.
    (void)task;
    hle->CallProcessDList();
    hle->rsp_break(0);
    return true;
}

// ──────────────────────────────────────────────
// Audio task dispatch
// ──────────────────────────────────────────────

// Signature-based audio microcode identification (from mupen64plus-rsp-hle).
// Reads specific uint32 values from ucode_data in DRAM to identify the audio ABI.
// This is more reliable than CRC since it matches the original identification method.
static UcodeHandler IdentifyAudioBySignature(CHle* hle, uint32_t ucodeData)
{
    if (hle->dram() == nullptr || ucodeData == 0)
        return nullptr;

    auto rd32 = [&](uint32_t addr) -> uint32_t {
        return *reinterpret_cast<const uint32_t*>(hle->dram() + (addr & 0x7FFFFF));
    };

    if (rd32(ucodeData) == 0x00000001) {
        if (rd32(ucodeData + 0x30) == 0xF0000F00) {
            // ABI1 — Standard Audio ABI
            uint32_t v = rd32(ucodeData + 0x28);
            switch (v) {
            case 0x1E24138C: return alist_process_audio;       // most common
            case 0x1DC8138C: return alist_process_audio_ge;    // GoldenEye
            case 0x1E3C1390: return alist_process_audio_bc;    // BlastCorp, DiddyKongRacing
            default:
                std::fprintf(stderr, "[RSP] ABI1 unknown sig: v=0x%08X\n", v);
            }
        } else {
            // ABI2 — NEAD variants
            uint32_t v = rd32(ucodeData + 0x10);
            switch (v) {
            case 0x11181350: return alist_process_nead_mk;     // MarioKart, WaveRace (E)
            case 0x111812E0: return alist_process_nead_sfj;    // StarFox (J)
            case 0x110412AC: return alist_process_nead_wrjb;   // WaveRace (J RevB)
            case 0x110412CC: return alist_process_nead_sf;     // StarFox/LylatWars
            case 0x1CD01250: return alist_process_nead_fz;     // F-Zero X
            case 0x1F08122C: return alist_process_nead_ys;     // Yoshi's Story
            case 0x1F38122C: return alist_process_nead_1080;   // 1080 Snowboarding
            case 0x1F681230: return alist_process_nead_oot;    // Zelda OoT / MM (J)
            case 0x1F801250: return alist_process_nead_mm;     // Zelda MM
            case 0x109411F8: return alist_process_nead_mmb;    // Zelda MM (E Beta)
            case 0x1EAC11B8: return alist_process_nead_ac;     // Animal Crossing
            case 0x00010010: return musyx_v2_task;             // MusyX v2
            default:
                std::fprintf(stderr, "[RSP] ABI2 unknown sig: v=0x%08X\n", v);
            }
        }
    } else {
        // ABI3 — Naudio / MusyX v1
        uint32_t v = rd32(ucodeData + 0x10);
        switch (v) {
        case 0x00000001: return musyx_v1_task;                 // MusyX v1
        case 0x0000127C: return alist_process_naudio;          // Naudio
        case 0x00001280: return alist_process_naudio_bk;       // BanjoKazooie
        case 0x1C58126C: return alist_process_naudio_dk;       // DonkeyKong
        case 0x1AE8143C: return alist_process_naudio_mp3;      // BanjoTooie, PerfectDark
        case 0x1AB0140C: return alist_process_naudio_cbfd;     // Conker
        default:
            std::fprintf(stderr, "[RSP] ABI3 unknown sig: v=0x%08X\n", v);
        }
    }
    return nullptr;
}

bool CTaskDispatcher::DispatchAudioTask(CHle* hle, const TaskInfo& task, const MicrocodeEntry* ucode)
{
    if (!hle->AudioHle()) {
        // BUG-FIX: when audio HLE is disabled but we have no LLE RSP path
        // wired up to actually run the audio ucode, returning false here
        // and letting the task drop on the floor causes the game's audio
        // thread to block forever waiting for SP_INTR. That backpressure
        // stalls the OS message queue and corrupts dependent thread state
        // (notably MM's gSaveContext updates after item-get cutscenes,
        // producing the "Link instant-dies after magic bar" symptom).
        // Always signal task complete so the audio thread can drain its
        // queue. Cost: silent audio. Benefit: stable game logic.
        hle->rsp_break(0);
        return true;
    }

    ++s_audioTaskCount;

    if (ucode != nullptr && ucode->handler != nullptr)
    {
        ucode->handler(hle);
        hle->rsp_break(0);
        return true;
    }

    // CRC-based lookup failed — try signature-based identification (mupen64plus method)
    UcodeHandler sigHandler = IdentifyAudioBySignature(hle, task.UcodeData);
    if (sigHandler != nullptr)
    {
        sigHandler(hle);
        hle->rsp_break(0);
        return true;
    }

    // Unknown audio microcode — try generic audio dispatch
    if (task.DataPtr != 0 && task.DataSize > 0)
    {
        hle->WarnMessage("RSP HLE: Falling back to generic alist_process_audio for unknown audio UCode");
        alist_process_audio(hle);
        hle->rsp_break(0);
        return true;
    }

    m_TasksFallback++;
    return false;
}

// ──────────────────────────────────────────────
// JPEG task dispatch
// ──────────────────────────────────────────────

bool CTaskDispatcher::DispatchJpegTask(CHle* hle, const TaskInfo& /*task*/, const MicrocodeEntry* ucode)
{
    if (ucode != nullptr && ucode->handler != nullptr)
    {
        hle->VerboseMessage("RSP HLE: Dispatching JPEG task to '%.*s'",
            static_cast<int>(ucode->name.size()), ucode->name.data());
        ucode->handler(hle);
        hle->rsp_break(0);
        return true;
    }

    // Unknown type-4 microcode (could be CIC compare, custom task, etc.)
    // Don't blindly try generic JPEG — return false to let the RSP interpreter handle it.
    hle->WarnMessage("RSP HLE: Unidentified type-4 microcode, deferring to RSP interpreter");
    m_TasksFallback++;
    return false;
}

// ──────────────────────────────────────────────
// sum_bytes fallback detection
// Matches mupen64plus-rsp-hle try_normal_task_detection():
// when CRC lookup fails, sum the first half of microcode text
// bytes to identify known RSP tasks by their byte signature.
// ──────────────────────────────────────────────

static void task_done_noop(CHle*) { /* StoreVe12: nothing to emulate */ }

const MicrocodeEntry* CTaskDispatcher::TrySumBytesFallback(CHle* hle, const TaskInfo& task)
{
    static constexpr MicrocodeEntry kStoreVe12  = { 0, 0, UcodeType::Unknown, task_done_noop,   "StoreVe12 (no-op)" };
    static constexpr MicrocodeEntry kJpegPS0    = { 0, 0, UcodeType::JPEG_PS0, jpeg_decode_PS0, "JPEG PS0 (sum)" };
    static constexpr MicrocodeEntry kJpegPS     = { 0, 0, UcodeType::JPEG_PS,  jpeg_decode_PS,  "JPEG PS (sum)" };
    static constexpr MicrocodeEntry kJpegOB     = { 0, 0, UcodeType::JPEG_OB,  jpeg_decode_OB,  "JPEG OB (sum)" };

    const uint32_t masked_addr = task.Ucode & 0x7FFFFF;
    const uint32_t sum_size = std::min(task.UcodeSize, 0xF80u) >> 1;
    const uint8_t* ucode_bytes = hle->dram() + masked_addr;

    uint32_t sum = 0;
    for (uint32_t i = 0; i < sum_size; ++i)
        sum += ucode_bytes[i];

    switch (sum)
    {
    case 0x278:   return &kStoreVe12;  // OoT/MM misleading type-4 no-op
    case 0x2c85a: return &kJpegPS0;    // JPEG (Pokemon Stadium J)
    case 0x2caa6: return &kJpegPS;     // JPEG (OoT, Pokemon Stadium 1/2)
    case 0x130de: // JPEG (Ogre Battle)
    case 0x278b0: return &kJpegOB;     // JPEG (Bottom of the 9th)
    }

    return nullptr;
}

} // namespace Nova64::RSP::HLE
