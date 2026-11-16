/**
 * @file HleTypes.hpp
 * @brief RSP HLE shared type definitions.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>

namespace Nova64::RSP::HLE {

// --- HLE Boot microcode identification ---
enum class HLETaskBooter : uint32_t {
    Unknown,
    Boot_B4C62BFC,
    Boot_CAB15710,
};

/// Task info read from DMEM at 0xFC0; matches the N64 OSTask structure layout.
struct TaskInfo {
    uint32_t Type;
    uint32_t Flags;
    uint32_t Boot;
    uint32_t BootSize;
    uint32_t Ucode;
    uint32_t UcodeSize;
    uint32_t UcodeData;
    uint32_t UcodeDataSize;
    uint32_t DramStack;
    uint32_t DramStackSize;
    uint32_t OutputBuff;
    uint32_t OutputBuffSize;
    uint32_t DataPtr;
    uint32_t DataSize;
    uint32_t YieldDataPtr;
    uint32_t YieldDataSize;
};

// --- HLE Task Types ---
enum class HLETaskType : uint32_t {
    Video = 1,
    Audio = 2,
    JPEG = 4,
    CICx105 = 7,
};

} // namespace Nova64::RSP::HLE
