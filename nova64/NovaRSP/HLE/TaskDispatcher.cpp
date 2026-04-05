/**
 * @file TaskDispatcher.cpp
 * @brief RSP HLE microcode identification via CRC32 and task routing.
 */

#include "TaskDispatcher.hpp"
#include "hle.hpp"
#include "ucodes.hpp"
#include <algorithm>
#include <cstring>

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
        hle->WarnMessage("RSP HLE: Unknown microcode (data_crc=0x%08X, text_crc=0x%08X, addr=0x%08X, size=0x%X)",
            data_crc, text_crc, task.UcodeData, task.UcodeDataSize);
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

bool CTaskDispatcher::DispatchAudioTask(CHle* hle, const TaskInfo& task, const MicrocodeEntry* ucode)
{
    if (!hle->AudioHle())
        return false;

    if (ucode != nullptr && ucode->handler != nullptr)
    {
        // Identified microcode — use specific handler
        hle->VerboseMessage("RSP HLE: Dispatching audio task to '%.*s'",
            static_cast<int>(ucode->name.size()), ucode->name.data());
        ucode->handler(hle);
        hle->rsp_break(0);
        return true;
    }

    // Unknown audio microcode — try generic audio dispatch
    // Check if the ucode data looks like an alist command table
    if (task.DataPtr != 0 && task.DataSize > 0)
    {
        hle->WarnMessage("RSP HLE: Unidentified audio microcode, attempting generic dispatch");
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

} // namespace Nova64::RSP::HLE
