/**
 * @file SysGlobals.cpp
 * @brief Implementation of global utility functions for the N64 emulator
 */

#include "SysGlobals.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace N64Types {

// --- String Conversion Functions ---

std::string to_string(CpuType type) {
    switch (type) {
    case CpuType::Default:     return "Default";
    case CpuType::Interpreter: return "Interpreter";
    case CpuType::Recompiler:  return "Recompiler";
    case CpuType::SyncCores:   return "SyncCores";
    default:                   return "Unknown";
    }
}

std::string to_string(CicChip chip) {
    switch (chip) {
    case CicChip::Unknown:  return "Unknown";
    case CicChip::NUS_6101: return "CIC-NUS-6101";
    case CicChip::NUS_6102: return "CIC-NUS-6102";
    case CicChip::NUS_6103: return "CIC-NUS-6103";
    case CicChip::NUS_6104: return "CIC-NUS-6104";
    case CicChip::NUS_6105: return "CIC-NUS-6105";
    case CicChip::NUS_6106: return "CIC-NUS-6106";
    case CicChip::NUS_5167: return "CIC-NUS-5167";
    case CicChip::NUS_8303: return "CIC-NUS-8303";
    case CicChip::NUS_8501: return "CIC-NUS-8501";
    case CicChip::NUS_8401: return "CIC-NUS-8401";
    case CicChip::NUS_5101: return "CIC-NUS-5101";
    case CicChip::MiniIpl3: return "Mini-IPL3";
    default:                return "Unknown";
    }
}

std::string to_string(CountryCode code) {
    switch (code) {
    case CountryCode::Unknown:        return "Unknown";
    case CountryCode::NTSC_Beta:      return "NTSC (Beta)";
    case CountryCode::Asian_NTSC:     return "Asia (NTSC)";
    case CountryCode::Brazil:         return "Brazil";
    case CountryCode::China:          return "China";
    case CountryCode::Germany:        return "Germany";
    case CountryCode::USA:            return "USA";
    case CountryCode::France:         return "France";
    case CountryCode::Gateway64_NTSC: return "Gateway64 (NTSC)";
    case CountryCode::Netherlands:    return "Netherlands";
    case CountryCode::Italy:          return "Italy";
    case CountryCode::Japan:          return "Japan";
    case CountryCode::Korea:          return "Korea";
    case CountryCode::Gateway64_PAL:  return "Gateway64 (PAL)";
    case CountryCode::Canada:         return "Canada";
    case CountryCode::Europe:         return "Europe";
    case CountryCode::Spain:          return "Spain";
    case CountryCode::Australia:      return "Australia";
    case CountryCode::Scandinavia:    return "Scandinavia";
    case CountryCode::EuropeanX_PAL:  return "Europe (X)";
    case CountryCode::EuropeanY_PAL:  return "Europe (Y)";
    default:                          return "Unknown";
    }
}

std::string to_string(SaveChipType type) {
    switch (type) {
    case SaveChipType::Auto:          return "Auto";
    case SaveChipType::Eeprom4K:      return "EEPROM 4Kbit";
    case SaveChipType::Eeprom16K:     return "EEPROM 16Kbit";
    case SaveChipType::Sram:          return "SRAM";
    case SaveChipType::FlashRam:      return "FlashRAM";
    case SaveChipType::ControllerPak: return "Controller Pak";
    case SaveChipType::None:          return "None";
    default:                          return "Unknown";
    }
}

std::string to_string(PauseType type) {
    switch (type) {
    case PauseType::None:                return "None";
    case PauseType::FromMenu:            return "Menu";
    case PauseType::AppLostActive:       return "App Lost Active";
    case PauseType::AppLostFocus:        return "App Lost Focus";
    case PauseType::SaveGame:            return "Saving Game";
    case PauseType::LoadGame:            return "Loading Game";
    case PauseType::DumpMemory:          return "Dumping Memory";
    case PauseType::SearchMemory:        return "Searching Memory";
    case PauseType::Settings:            return "Settings";
    case PauseType::Cheats:              return "Cheats";
    case PauseType::ChangingBreakpoints: return "Changing Breakpoints";
    case PauseType::Enhancement:         return "Enhancement";
    default:                             return "Unknown";
    }
}

std::string to_string(ExceptionCode code) {
    switch (code) {
    case ExceptionCode::Interrupt:           return "Interrupt";
    case ExceptionCode::TLBModification:     return "TLB Modification";
    case ExceptionCode::TLBLoadMiss:         return "TLB Load Miss";
    case ExceptionCode::TLBStoreMiss:        return "TLB Store Miss";
    case ExceptionCode::AddressErrorLoad:    return "Address Error (Load)";
    case ExceptionCode::AddressErrorStore:   return "Address Error (Store)";
    case ExceptionCode::BusErrorInstruction: return "Bus Error (Instruction)";
    case ExceptionCode::BusErrorData:        return "Bus Error (Data)";
    case ExceptionCode::Syscall:             return "Syscall";
    case ExceptionCode::Breakpoint:          return "Breakpoint";
    case ExceptionCode::ReservedInstruction: return "Reserved Instruction";
    case ExceptionCode::CoprocessorUnusable: return "Coprocessor Unusable";
    case ExceptionCode::Overflow:            return "Overflow";
    case ExceptionCode::Trap:                return "Trap";
    case ExceptionCode::FloatingPoint:       return "Floating Point";
    case ExceptionCode::Watch:               return "Watch";
    case ExceptionCode::XTLB_Refill:         return "XTLB Refill";
    default:                                 return "Unknown";
    }
}

std::string to_string(PipelineStage stage) {
    switch (stage) {
    case PipelineStage::Normal:                 return "Normal";
    case PipelineStage::DoDelaySlot:            return "DoDelaySlot";
    case PipelineStage::DoEndDelaySlot:         return "DoEndDelaySlot";
    case PipelineStage::DelaySlot:              return "DelaySlot";
    case PipelineStage::EndDelaySlot:           return "EndDelaySlot";
    case PipelineStage::LikelyDelaySlot:        return "LikelyDelaySlot";
    case PipelineStage::Jump:                   return "Jump";
    case PipelineStage::JumpDelaySlot:          return "JumpDelaySlot";
    case PipelineStage::DelaySlotDone:          return "DelaySlotDone";
    case PipelineStage::LikelyDelaySlotDone:    return "LikelyDelaySlotDone";
    case PipelineStage::EndBlock:               return "EndBlock";
    case PipelineStage::PermanentLoopDoDelay:   return "PermanentLoopDoDelay";
    case PipelineStage::PermanentLoopDelayDone: return "PermanentLoopDelayDone";
    default:                                    return "Unknown";
    }
}

// --- Formatting Utilities ---

std::string FormatAddress(uint32_t address) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << address;
    return oss.str();
}

std::string FormatAddress64(uint64_t address) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << address;
    return oss.str();
}

std::string FormatHex8(uint8_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(value);
    return oss.str();
}

std::string FormatHex16(uint16_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << value;
    return oss.str();
}

std::string FormatHex32(uint32_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return oss.str();
}

std::string FormatHex64(uint64_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << value;
    return oss.str();
}

std::string FormatSize(size_t bytes) {
    std::ostringstream oss;

    if (bytes >= 1024 * 1024 * 1024) {
        oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    } else if (bytes >= 1024 * 1024) {
        oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024) {
        oss << std::fixed << std::setprecision(2) << (bytes / 1024.0) << " KB";
    } else {
        oss << bytes << " bytes";
    }

    return oss.str();
}

// --- Memory Utilities ---

std::string GetMemoryRegionName(uint32_t address) {
    uint32_t paddr = address & 0x1FFFFFFF;

    if (paddr < N64Constants::RDRAM_SIZE_8MB) {
        return "RDRAM";
    } else if (paddr >= 0x03F00000 && paddr < 0x04000000) {
        return "RDRAM Registers";
    } else if (paddr >= N64Constants::SP_DMEM_BASE && paddr < N64Constants::SP_IMEM_BASE) {
        return "SP DMEM";
    } else if (paddr >= N64Constants::SP_IMEM_BASE && paddr < N64Constants::SP_REG_BASE) {
        return "SP IMEM";
    } else if (paddr >= N64Constants::SP_REG_BASE && paddr < N64Constants::DPC_REG_BASE) {
        return "SP Registers";
    } else if (paddr >= N64Constants::DPC_REG_BASE && paddr < N64Constants::DPS_REG_BASE) {
        return "DPC Registers";
    } else if (paddr >= N64Constants::DPS_REG_BASE && paddr < N64Constants::MI_REG_BASE) {
        return "DPS Registers";
    } else if (paddr >= N64Constants::MI_REG_BASE && paddr < N64Constants::VI_REG_BASE) {
        return "MI Registers";
    } else if (paddr >= N64Constants::VI_REG_BASE && paddr < N64Constants::AI_REG_BASE) {
        return "VI Registers";
    } else if (paddr >= N64Constants::AI_REG_BASE && paddr < N64Constants::PI_REG_BASE) {
        return "AI Registers";
    } else if (paddr >= N64Constants::PI_REG_BASE && paddr < N64Constants::RI_REG_BASE) {
        return "PI Registers";
    } else if (paddr >= N64Constants::RI_REG_BASE && paddr < N64Constants::SI_REG_BASE) {
        return "RI Registers";
    } else if (paddr >= N64Constants::SI_REG_BASE && paddr < N64Constants::CART_DOM2_BASE) {
        return "SI Registers";
    } else if (paddr >= N64Constants::CART_ROM_BASE && paddr < N64Constants::PIF_ROM_BASE) {
        return "Cartridge ROM";
    } else if (paddr >= N64Constants::PIF_ROM_BASE && paddr < N64Constants::PIF_RAM_BASE) {
        return "PIF ROM";
    } else if (paddr >= N64Constants::PIF_RAM_BASE && paddr < N64Constants::PIF_RAM_BASE + N64Constants::PIF_RAM_SIZE) {
        return "PIF RAM";
    }

    return "Unknown";
}

bool IsValidRdramAddress(uint32_t address, uint32_t rdramSize) {
    uint32_t paddr = address & 0x1FFFFFFF;
    return paddr < rdramSize;
}

bool IsCachedAddress(uint32_t address) {
    return (address >= N64Constants::KSEG0_BASE && address < N64Constants::KSEG1_BASE);
}

bool IsUncachedAddress(uint32_t address) {
    return (address >= N64Constants::KSEG1_BASE && address < N64Constants::KSEG2_BASE);
}

bool IsTlbMappedAddress(uint32_t address) {
    return (address < N64Constants::KSEG0_BASE) || (address >= N64Constants::KSEG2_BASE);
}

} // namespace N64Types
