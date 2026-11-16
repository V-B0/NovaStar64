/**
 * @file SysGlobals.hpp
 * @brief Central forward declarations and global type definitions for the N64 emulator
 * 
 * This header provides forward declarations for all major classes and commonly
 * used type definitions. Include this header to avoid circular dependencies
 * and reduce compilation times.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <array>
#include <vector>
#include "Types.hpp"

using N64Types::CountryCode;
using N64Types::SystemType;
// Add others if needed by IsPalCountry implementation

// ============================================================================
// Namespace Forward Declarations
// ============================================================================

namespace N64 {
// ROM handling (N64Rom.hpp)
namespace Rom {
enum class RomFormat : uint8_t;
enum class CountryCode : uint8_t;
enum class CICChip : uint8_t;
struct DataView;
struct RomHeader;
class RomImage;
class RomError;
class N64Rom;
} // namespace Rom

// Disk Drive handling (N64DD.hpp)
namespace Disk {
enum class DiskFormat : uint8_t;
enum class DiskType : uint8_t;
enum class CountryCode : uint8_t;
struct DataView;
struct DiskGeometry;
struct DiskAddress;
class DiskImage;
class DiskError;
class N64Disk;
} // namespace Disk
} // namespace N64

class CN64System;

namespace Nova64 {
// Core system classes
using N64System = ::CN64System;
class MipsMemoryVM;

// CPU components
class Registers;
class TLB;

// Recompiler subsystem
namespace Recompiler {
class CRecompiler;
class CodeBlock;
class CodeSection;
class FunctionInfo;
class FunctionMap;
class RecompilerOps;
class RegInfo;

namespace x64 {
class X64RecompilerOps;
class X64RegInfo;
} // namespace x64

namespace x86 {
class X86RecompilerOps;
class X86RegInfo;
} // namespace x86

namespace Arm {
class ArmRecompilerOps;
class ArmRegInfo;
} // namespace Arm
} // namespace Recompiler

// Interpreter subsystem
namespace Interpreter {
class R4300iInterpreter;
class InterpreterOps;
} // namespace Interpreter
} // namespace Nova64

namespace N64System {
// TLB and memory access
enum class MemoryAccessPermission : uint8_t;
enum class MemorySegment : uint8_t;
enum class PrivilegeMode : uint8_t;
struct TlbEntry;
class CTLB;
} // namespace N64System

// ============================================================================
// Memory Handler Forward Declarations
// ============================================================================

class CRomMemHandler;
class CCachedRom;
class CUnCachedRom;
class CCachedSysMemory;
class CUnCachedSysMemory;
class CDisplayCtrlReg;
class CMIPSYInterfaceHandler;
class CPIInterFace;
class CPifInterface;
class CRDRAMInterfaceHandler;
class CRDRAMRegHandler;
class CSIInterfaceHandler;
class CSPRegistersHandler;
class CVideoInterfaceHandler;
class CDebugConsoleOut;

// ============================================================================
// Save Type Forward Declarations
// ============================================================================

class CSRAM;
class CFlashRAM;
class CEeprom;

// ============================================================================
// Controller/Accessory Forward Declarations
// ============================================================================

class CDDisk;
class CGBCart;
class CMemPak;
class CRumblePak;
class CTransferPak;

// ============================================================================
// Enhancement Forward Declarations
// ============================================================================

class CEnhancement;
class CEnhancementFile;
class CEnhancementList;

// ============================================================================
// Instruction Handling Forward Declarations
// ============================================================================

class R4300iInstruction;

// ============================================================================
// Common Enumerations (from Types.hpp)
// ============================================================================

namespace N64Types {

/**
 * @enum ExceptionCode
 * @brief MIPS exception codes
 */
enum class ExceptionCode : uint8_t {
    Interrupt = 0,
    TLBModification = 1,
    TLBLoadMiss = 2,
    TLBStoreMiss = 3,
    AddressErrorLoad = 4,
    AddressErrorStore = 5,
    BusErrorInstruction = 6,
    BusErrorData = 7,
    Syscall = 8,
    Breakpoint = 9,
    ReservedInstruction = 10,
    CoprocessorUnusable = 11,
    Overflow = 12,
    Trap = 13,
    FloatingPoint = 15,
    Watch = 23,
    XTLB_Refill = 31
};

// ============================================================================
// Helper Functions
// ============================================================================

// Helper functions removed to avoid redefinition (present in Types.hpp)

// ============================================================================
// String Conversion Functions (implemented in SysGlobals.cpp)
// ============================================================================

std::string to_string(CpuType type);
std::string to_string(CicChip chip);
std::string to_string(CountryCode code);
std::string to_string(SaveChipType type);
std::string to_string(PauseType type);
std::string to_string(ExceptionCode code);
std::string to_string(PipelineStage stage);

// ============================================================================
// Formatting Utilities (implemented in SysGlobals.cpp)
// ============================================================================

/**
 * @brief Format a 32-bit address as hex string
 */
std::string FormatAddress(uint32_t address);

/**
 * @brief Format a 64-bit address as hex string
 */
std::string FormatAddress64(uint64_t address);

/**
 * @brief Format an 8-bit value as hex string
 */
std::string FormatHex8(uint8_t value);

/**
 * @brief Format a 16-bit value as hex string
 */
std::string FormatHex16(uint16_t value);

/**
 * @brief Format a 32-bit value as hex string
 */
std::string FormatHex32(uint32_t value);

/**
 * @brief Format a 64-bit value as hex string
 */
std::string FormatHex64(uint64_t value);

/**
 * @brief Format a byte size as human-readable string
 */
std::string FormatSize(size_t bytes);

// ============================================================================
// Memory Utilities (implemented in SysGlobals.cpp)
// ============================================================================

/**
 * @brief Get the name of the memory region for an address
 */
std::string GetMemoryRegionName(uint32_t address);

/**
 * @brief Check if address is valid RDRAM
 */
bool IsValidRdramAddress(uint32_t address, uint32_t rdramSize);

/**
 * @brief Check if address is in cached segment (KSEG0)
 */
bool IsCachedAddress(uint32_t address);

/**
 * @brief Check if address is in uncached segment (KSEG1)
 */
bool IsUncachedAddress(uint32_t address);

/**
 * @brief Check if address requires TLB mapping
 */
bool IsTlbMappedAddress(uint32_t address);

} // namespace N64Types

// ============================================================================
// Common Data Types (from Types.hpp)
// ============================================================================

/**
 * @union MIPS_WORD
 * @brief 32-bit MIPS word with various access types
 */
union MIPS_WORD {
    int32_t W;
    uint32_t UW;
    int16_t HW[2];
    uint16_t UHW[2];
    int8_t B[4];
    uint8_t UB[4];
    float F;

    MIPS_WORD() : UW(0) {}
    explicit MIPS_WORD(uint32_t val) : UW(val) {}
    explicit MIPS_WORD(int32_t val) : W(val) {}
};

/**
 * @union MIPS_DWORD
 * @brief 64-bit MIPS doubleword with various access types
 */
union MIPS_DWORD {
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

    MIPS_DWORD() : UDW(0) {}
    explicit MIPS_DWORD(uint64_t val) : UDW(val) {}
    explicit MIPS_DWORD(int64_t val) : DW(val) {}
};

// ============================================================================
// Common Type Aliases
// ============================================================================

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

using f32 = float;
using f64 = double;

// Virtual and physical address types
using VAddr = uint64_t;
using PAddr = uint32_t;

// ============================================================================
// Common Constants
// ============================================================================

namespace N64Constants {

// Memory sizes
constexpr uint32_t RDRAM_SIZE_4MB = 0x400000;
constexpr uint32_t RDRAM_SIZE_8MB = 0x800000;
constexpr uint32_t DMEM_SIZE = 0x1000;
constexpr uint32_t IMEM_SIZE = 0x1000;
constexpr uint32_t PIF_RAM_SIZE = 0x40;
constexpr uint32_t PIF_ROM_SIZE = 0x7C0;

// Memory base addresses
constexpr uint32_t RDRAM_BASE = 0x00000000;
constexpr uint32_t RDRAM_REG_BASE = 0x03F00000;
constexpr uint32_t SP_DMEM_BASE = 0x04000000;
constexpr uint32_t SP_IMEM_BASE = 0x04001000;
constexpr uint32_t SP_REG_BASE = 0x04040000;
constexpr uint32_t DPC_REG_BASE = 0x04100000;
constexpr uint32_t DPS_REG_BASE = 0x04200000;
constexpr uint32_t MI_REG_BASE = 0x04300000;
constexpr uint32_t VI_REG_BASE = 0x04400000;
constexpr uint32_t AI_REG_BASE = 0x04500000;
constexpr uint32_t PI_REG_BASE = 0x04600000;
constexpr uint32_t RI_REG_BASE = 0x04700000;
constexpr uint32_t SI_REG_BASE = 0x04800000;
constexpr uint32_t CART_DOM2_BASE = 0x05000000;
constexpr uint32_t CART_DOM1_BASE = 0x06000000;
constexpr uint32_t CART_ROM_BASE = 0x10000000;
constexpr uint32_t PIF_ROM_BASE = 0x1FC00000;
constexpr uint32_t PIF_RAM_BASE = 0x1FC007C0;

// Cached/uncached address mappings
constexpr uint32_t KSEG0_BASE = 0x80000000; // Cached
constexpr uint32_t KSEG1_BASE = 0xA0000000; // Uncached
constexpr uint32_t KSEG2_BASE = 0xC0000000; // TLB mapped

// ROM limits
constexpr uint32_t MAX_ROM_SIZE = 0x4000000; // 64MB

// TLB
constexpr uint32_t TLB_ENTRY_COUNT = 32;

// CPU timing
constexpr uint32_t CPU_FREQUENCY_NTSC = 93750000;
constexpr uint32_t CPU_FREQUENCY_PAL = 93750000;
constexpr uint32_t VI_REFRESH_RATE_NTSC = 60;
constexpr uint32_t VI_REFRESH_RATE_PAL = 50;

} // namespace N64Constants

// ============================================================================
// Utility Macros
// ============================================================================

// Compile-time array size
template <typename T, size_t N> constexpr size_t ArraySize(T (&)[N]) noexcept { return N; }

// Bit manipulation helpers
#ifdef BIT
#undef BIT
#endif
constexpr uint32_t BIT(uint32_t n) { return 1u << n; }
constexpr uint64_t BIT64(uint32_t n) { return 1ull << n; }

// Address conversion macros
constexpr uint32_t VAddrToPAddr(uint32_t vaddr) { return vaddr & 0x1FFFFFFF; }

constexpr uint32_t PAddrToKSEG0(uint32_t paddr) { return paddr | N64Constants::KSEG0_BASE; }

constexpr uint32_t PAddrToKSEG1(uint32_t paddr) { return paddr | N64Constants::KSEG1_BASE; }
