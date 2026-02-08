#pragma once
#include <stdint.h>

#if defined(__i386) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace N64Types {enum class PauseType {
    None,
    FromMenu,
    AppLostActive,
    AppLostFocus,
    SaveGame,
    LoadGame,
    DumpMemory,
    SearchMemory,
    Settings,
    Cheats,
    ChangingBreakpoints,
    Enhancement
};

enum class CpuType : int8_t {  
    Default = -1,
    Interpreter = 1,
    Recompiler = 2,
    SyncCores = 3
};

enum class FramerateType : uint8_t {
    VIs,
    DLs,
    Percent,
    VIsAndDLs
};

enum class SaveChipType : int8_t {
    Auto = -1,
    Eeprom4K,
    Eeprom16K,
    Sram,
    FlashRam,
    ControllerPak,  
    None
};

enum class SaveDiskType : uint8_t {
    ShadowFile,
    RamFile
};

enum class DiskSeekType : uint8_t {
    Turbo,
    Slow
};

enum class FuncLookupMethod : int8_t {
    Default = -1,
    PhysicalLookup = 1,
    VirtualLookup = 2,
    ChangeMemory = 3
};

enum class SystemType : uint8_t {
    NTSC = 0,
    PAL = 1,
    MPAL = 2
};


enum class CicChip : int8_t {
    Unknown = -1,
    NUS_6101 = 1,
    NUS_6102,
    NUS_6103,
    NUS_6104,
    NUS_6105,
    NUS_6106,
    NUS_5167,
    NUS_8303,
    NUS_8501,
    NUS_8401,
    NUS_5101,
    MiniIpl3
};

// Country codes with ISO 3166-1 alpha-2 where applicable
enum class CountryCode : uint8_t {
    Unknown = 0,
    NTSC_Beta = 0x37,
    Asian_NTSC = 0x41,  // A
    Brazil = 0x42,      // B
    China = 0x43,       // C
    Germany = 0x44,     // D
    USA = 0x45,         // E
    France = 0x46,      // F
    Gateway64_NTSC = 0x47,  // G
    Netherlands = 0x48, // H
    Italy = 0x49,       // I
    Japan = 0x4A,       // J
    Korea = 0x4B,       // K
    Gateway64_PAL = 0x4C,  // L
    Canada = 0x4E,      // N
    Europe = 0x50,      // P
    Spain = 0x53,       // S
    Australia = 0x55,   // U
    Scandinavia = 0x57, // W
    EuropeanX_PAL = 0x58,  // X
    EuropeanY_PAL = 0x59   // Y
};

enum class ProfileTimer : uint8_t {
    None = 0,
    R4300,
    RSP_Dlist,
    RSP_Alist,
    RSP_Unknown,
    RefreshScreen,
    UpdateScreen,
    UpdateFPS,
    Idle,
    Count  
};

enum class PipelineStage : uint8_t {
    Normal,
    DoDelaySlot,
    DoEndDelaySlot,
    DelaySlot,
    EndDelaySlot,
    LikelyDelaySlot,
    Jump,
    JumpDelaySlot,
    DelaySlotDone,
    LikelyDelaySlotDone,
    EndBlock,
    PermanentLoopDoDelay,
    PermanentLoopDelayDone
};

// Helper functions for enums
constexpr const char* to_string(SystemType type) {
    switch (type) {
        case SystemType::NTSC: return "NTSC";
        case SystemType::PAL: return "PAL";
        case SystemType::MPAL: return "MPAL";
        default: return "Unknown";
    }
}

union MIPS_WORD
{
    int32_t W;
    uint32_t UW;
    int16_t HW[2];
    uint16_t UHW[2];
    int8_t B[4];
    uint8_t UB[4];

    float F;
};

union MIPS_DWORD
{
    int64_t DW;
    uint64_t UDW;
    int32_t W[2];
    uint32_t UW[2];
    int16_t HW[4];
    uint16_t UHW[4];
    int8_t B[8];
    uint8_t UB[8];

    double D;
    float F[2];
};
};


   

