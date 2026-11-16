/**
 * @file Vmem.hpp
 * @brief Virtual Memory Management for the N64 emulator
 * 
 * Provides virtual-to-physical address translation and memory access
 * for the MIPS R4300i CPU. Handles TLB mapping, memory handlers,
 * and exception filtering for memory access violations.
 * 
 * Based on Project64's MemoryVirtualMem implementation.
 */

#pragma once

#include "../Memoryhandler/AudioInterfaceHandler.hpp"
#include "../Memoryhandler/CachedRom.hpp"
#include "../Memoryhandler/UnCachedRom.hpp"
#include "../Memoryhandler/CachedSysMemory.hpp"
#include "../Memoryhandler/UnCachedSysMemory.hpp"
#include "../Memoryhandler/DisplayCtrlReg.hpp"
#include "../Memoryhandler/MIPSYinterfacehandler.hpp"
#include "../Memoryhandler/PIInterFace.hpp"
#include "../Memoryhandler/PifInterface.hpp"
#include "../Memoryhandler/RDRAMInterFaceHandler.hpp"
#include "../Memoryhandler/RDRAMRegHandler.hpp"
#include "../Memoryhandler/RomMemHandler.hpp"
#include "../Memoryhandler/SIInterfaceHandler.hpp"
#include "../Memoryhandler/SPregistersHandler.hpp"
#include "../Memoryhandler/VideoInterfaceHandler.hpp"
#include "../Savetypes/SRam.hpp"
#include "../Savetypes/FRam.hpp"
#include "../Savetypes/Eeprom.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <array>

// Forward declarations from global namespace
class CN64System;

// using DPCRegisters::DisplayProcessorHandler; // Global
using PI::PeripheralInterfaceHandler;
// using PIF::PifRamHandler; // Global
// using RDRAM::RDRAMInterfaceHandler; // Global
// using RDRAM::RDRAMRegistersHandler; // Global
// using SI::SerialInterfaceHandler; // Global
// using VI::VideoInterfaceHandler; // Global
// using AI::AudioInterfaceHandler; // Global

// Forward declarations from N64System namespace
namespace N64System {
class CTLB;
}
class CRegisters;

namespace Nova64 {

// ----------------------------------------------------------------------------
// Memory Watch (debug-only).
// Emits a log line whenever a DMA into RDRAM touches the configured range.
// Configured via the same NOVA64_WATCH_ADDR/LEN/FILE environment variables as
// the CPU store watch in Vmem.cpp. `source` is a short tag like "PI", "SP",
// "SI", "DD", etc. for distinguishing the originator in the log output.
// ----------------------------------------------------------------------------
void WatchDMA(uint32_t pAddr, uint32_t length, const char* source);

// Forward declarations
// using N64System = ::CN64System;
// using Registers = ::CRegisters;
// using TLB = ::N64System::CTLB;
class N64Rom;

namespace Recompiler {
namespace x64 {
class X64RecompilerOps;
}
namespace x86 {
class X86RecompilerOps;
}
namespace Arm {
class ArmRecompilerOps;
class ArmRegInfo;
} // namespace Arm
} // namespace Recompiler

namespace Interpreter {
class R4300iInterpreter;
}

/**
 * @brief MIPS Virtual Memory Manager
 * 
 * Manages the N64's memory map including:
 * - RDRAM (main memory)
 * - DMEM/IMEM (RSP memory)
 * - Memory-mapped I/O registers
 * - ROM access
 * - TLB-based address translation
 */
class MipsMemoryVM {
public:
    /**
     * @brief Construct the memory manager
     * @param system Reference to the N64 system
     * @param tlb Reference to TLB
     */
    MipsMemoryVM(::CN64System& system, ::N64System::CTLB& tlb);

    /**
     * @brief Destructor
     */
    ~MipsMemoryVM();

    // Delete copy operations
    MipsMemoryVM(const MipsMemoryVM&) = delete;
    MipsMemoryVM& operator=(const MipsMemoryVM&) = delete;

    /**
     * @brief Initialize memory subsystem
     * @return true if successful
     */
    bool Initialize();

    /**
     * @brief Reset memory to initial state
     * @param eraseMemory If true, clear all memory contents
     */
    void Reset(bool eraseMemory);

    // ========================================================================
    // Memory Region Access
    // ========================================================================

    /**
     * @brief Get pointer to RDRAM
     * @return Reference to RDRAM pointer
     */
    [[nodiscard]] uint8_t*& Rdram() noexcept { return m_RDRAM; }

    /**
     * @brief Get RDRAM size
     * @return Allocated RDRAM size in bytes
     */
    [[nodiscard]] uint32_t RdramSize() const noexcept { return m_AllocatedRdramSize; }

    /**
     * @brief Get reference to owning N64 system
     */
    [[nodiscard]] ::CN64System* System() noexcept { return &m_System; }

    /**
     * @brief Get pointer to DMEM (RSP data memory)
     * @return Pointer to DMEM
     */
    [[nodiscard]] uint8_t* Dmem();

    /**
     * @brief Get pointer to IMEM (RSP instruction memory)
     * @return Pointer to IMEM
     */
    [[nodiscard]] uint8_t* Imem();

    /**
     * @brief Get SRAM interface
     * @return Reference to SRAM handler
     */
    [[nodiscard]] CSram& GetSram();

    /**
     * @brief Get Flash RAM interface
     * @return Reference to Flash RAM handler
     */
    [[nodiscard]] CFlashRam& GetFlashRam();

    // ========================================================================
    // Memory Pointer Access
    // ========================================================================

    /**
     * @brief Get pointer to memory at virtual address
     * @param vAddr Virtual address
     * @param size Size of region needed
     * @param read If true, for reading; if false, for writing
     * @return Pointer to memory, or nullptr if invalid
     */
    [[nodiscard]] uint8_t* MemoryPtr(uint32_t vAddr, uint32_t size, bool read);

    // ========================================================================
    // Memory Read Operations (Debugger/Tools)
    // ========================================================================

    /**
     * @brief Read 8-bit value from virtual address
     * @param vAddr Virtual address
     * @param value Output value
     * @return true if successful
     */
    bool MemoryValue8(uint32_t vAddr, uint8_t& value);

    /**
     * @brief Read 16-bit value from virtual address
     * @param vAddr Virtual address
     * @param value Output value
     * @return true if successful
     */
    bool MemoryValue16(uint32_t vAddr, uint16_t& value);

    /**
     * @brief Read 32-bit value from virtual address
     * @param vAddr Virtual address
     * @param value Output value
     * @return true if successful
     */
    bool MemoryValue32(uint32_t vAddr, uint32_t& value);

    /**
     * @brief Read 64-bit value from virtual address
     * @param vAddr Virtual address
     * @param value Output value
     * @return true if successful
     */
    bool MemoryValue64(uint32_t vAddr, uint64_t& value);

    // ========================================================================
    // Memory Write Operations (Debugger/Tools)
    // ========================================================================

    /**
     * @brief Write 8-bit value to virtual address
     * @param vAddr Virtual address
     * @param value Value to write
     * @return true if successful
     */
    bool UpdateMemoryValue8(uint32_t vAddr, uint8_t value);

    /**
     * @brief Write 16-bit value to virtual address
     * @param vAddr Virtual address
     * @param value Value to write
     * @return true if successful
     */
    bool UpdateMemoryValue16(uint32_t vAddr, uint16_t value);

    /**
     * @brief Write 32-bit value to virtual address
     * @param vAddr Virtual address
     * @param value Value to write
     * @return true if successful
     */
    bool UpdateMemoryValue32(uint32_t vAddr, uint32_t value);

    // ========================================================================
    // CPU Load Operations (64-bit virtual address)
    // ========================================================================

    bool LB_Memory(uint64_t vAddr, uint8_t& value);
    bool LH_Memory(uint64_t vAddr, uint16_t& value);
    bool LW_Memory(uint64_t vAddr, uint32_t& value);
    bool LD_Memory(uint64_t vAddr, uint64_t& value);

    // ========================================================================
    // CPU Store Operations (64-bit virtual address)
    // ========================================================================

    bool SB_Memory(uint64_t vAddr, uint32_t value);
    bool SH_Memory(uint64_t vAddr, uint32_t value);
    bool SW_Memory(uint64_t vAddr, uint32_t value);
    bool SD_Memory(uint64_t vAddr, uint64_t value);

    // MI repeat mode: invalidate/restore RDRAM write maps
    void SetMIRepeatMode(bool active);

    // MI repeat store (handles the byte-fill when MI init mode is active)
    bool MI_RepeatStore(uint32_t pAddr, uint64_t busData, uint32_t patternSize, uint32_t byteOffset);

    // ========================================================================
    // Exception Handling
    // ========================================================================

    /**
     * @brief Memory exception filter for SEH/signal handling
     * @param exptCode Exception code
     * @param exceptionPointer Platform-specific exception info
     * @return Filter result code
     */
    int32_t MemoryFilter(uint32_t exptCode, void* exceptionPointer);

    // ========================================================================
    // Memory Map Management
    // ========================================================================

    /**
     * @brief Clear memory write map for recompiler invalidation
     * @param vAddr Starting virtual address
     * @param length Length in bytes
     */
    void ClearMemoryWriteMap(uint32_t vAddr, uint32_t length);

    // ========================================================================
    // TLB Notification Functions
    // ========================================================================

    /**
     * @brief Notify that a TLB entry has been mapped
     * @param vAddr Virtual address
     * @param len Length of mapping
     * @param pAddr Physical address
     * @param readOnly If true, mapping is read-only
     */
    void TLB_Mapped(uint32_t vAddr, uint32_t len, uint32_t pAddr, bool readOnly);

    /**
     * @brief Notify that a TLB entry has been unmapped
     * @param vAddr Virtual address
     * @param len Length of mapping
     */
    void TLB_Unmaped(uint32_t vAddr, uint32_t len);

    // ========================================================================
    // Address Translation
    // ========================================================================

    /**
     * @brief Check if virtual address is valid
     * @param vAddr Virtual address
     * @return true if valid
     */
    [[nodiscard]] bool ValidVaddr(uint32_t vAddr) const;

    /**
     * @brief Translate virtual address to physical address
     * @param vAddr Virtual address
     * @param pAddr Output physical address
     * @return true if translation successful
     */
    [[nodiscard]] bool VAddrToPAddr(uint32_t vAddr, uint32_t& pAddr) const;

    // ========================================================================
    // Debug Support
    // ========================================================================

    /**
     * @brief Get label name for address (for disassembly)
     * @param address Address to look up
     * @return Label name string
     */
    [[nodiscard]] const char* LabelName(uint32_t address) const;

    // ========================================================================
    // Memory Handler Access
    // ========================================================================

    [[nodiscard]] auto& AudioInterface() noexcept { return m_AudioInterfaceHandler; }
    [[nodiscard]] auto& VideoInterface() noexcept { return m_VideoInterfaceHandler; }
    [[nodiscard]] auto& RomMemory() noexcept { return m_RomMemoryHandler; }
    [[nodiscard]] auto& PifRam() noexcept { return m_PifRamHandler; }
    [[nodiscard]] auto& SPRegisters() noexcept { return m_SPRegistersHandler; }
    [[nodiscard]] auto& DPCommandRegisters() noexcept { return m_DPCommandRegistersHandler; }
    [[nodiscard]] auto& MIPSInterface() noexcept { return m_MIPSInterfaceHandler; }
    [[nodiscard]] auto& PeripheralInterface() noexcept { return m_PeripheralInterfaceHandler; }
    [[nodiscard]] auto& SerialInterface() noexcept { return m_SerialInterfaceHandler; }
    [[nodiscard]] auto& RDRAMInterface() noexcept { return m_RDRAMInterfaceHandler; }
    [[nodiscard]] auto& RDRAMRegisters() noexcept { return m_RDRAMRegistersHandler; }

    // ========================================================================
    // TLB Map Management
    // ========================================================================

    /**
     * @brief Update fast-path memory maps after a TLB entry is written.
     *
     * Called from CTLB::WriteEntry (TLBWI/TLBWR) to propagate TLB mappings
     * into the page-table arrays used by LW_Memory / SW_Memory for direct
     * pointer access.
     *
     * @param entryHi   CP0 EntryHi register value (contains VPN2 and ASID)
     * @param pageMask  CP0 PageMask register value
     * @param entryLo0  CP0 EntryLo0 register value (even page: PFN, V, D, G)
     * @param entryLo1  CP0 EntryLo1 register value (odd page:  PFN, V, D, G)
     */
    void ApplyTLBMapping(uint64_t entryHi, uint64_t pageMask, uint64_t entryLo0, uint64_t entryLo1);

    /**
     * @brief Remove a TLB mapping that was previously applied.
     *
     * @param entryHi   Old EntryHi value
     * @param pageMask  Old PageMask value
     */
    void RemoveTLBMapping(uint64_t entryHi, uint64_t pageMask);

    // Accessors for diagnostics
    const size_t* GetMemoryReadMap() const { return m_MemoryReadMap; }
    const size_t* GetMemoryWriteMap() const { return m_MemoryWriteMap; }
    const uint32_t* GetTLBReadMap() const { return m_TLB_ReadMap; }

private:
    // Friends for internal access
    friend class Interpreter::R4300iInterpreter;

#if defined(__amd64__) || defined(_M_X64)
    friend class Recompiler::x64::X64RecompilerOps;
#elif defined(__i386__) || defined(_M_IX86)
    friend class Recompiler::x86::X86RecompilerOps;
#elif defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) || defined(_M_ARM64)
    friend class Recompiler::Arm::ArmRegInfo;
    friend class Recompiler::Arm::ArmRecompilerOps;
#endif

    // ========================================================================
    // Internal Callbacks
    // ========================================================================

    static void RdramChanged(MipsMemoryVM* self);
    static void ChangeMiIntrMask();

    // ========================================================================
    // Breakpoint Support
    // ========================================================================

    bool MemoryBreakpoint();

    // ========================================================================
    // 32-bit Virtual Address Load Operations
    // ========================================================================

    bool LB_VAddr32(uint32_t vAddr, uint8_t& value);
    bool LH_VAddr32(uint32_t vAddr, uint16_t& value);
    bool LW_VAddr32(uint32_t vAddr, uint32_t& value);
    bool LD_VAddr32(uint32_t vAddr, uint64_t& value);

    // ========================================================================
    // Physical Address Load Operations
    // ========================================================================

    bool LB_PhysicalAddress(uint32_t pAddr, uint8_t& value);
    bool LH_PhysicalAddress(uint32_t pAddr, uint16_t& value);
    bool LW_PhysicalAddress(uint32_t pAddr, uint32_t& value);
    bool LD_PhysicalAddress(uint32_t pAddr, uint64_t& value);

    // ========================================================================
    // 32-bit Virtual Address Store Operations
    // ========================================================================

    bool SB_VAddr32(uint32_t vAddr, uint32_t value);
    bool SH_VAddr32(uint32_t vAddr, uint32_t value);
    bool SW_VAddr32(uint32_t vAddr, uint32_t value);
    bool SD_VAddr32(uint32_t vAddr, uint64_t value);

    // ========================================================================
    // Physical Address Store Operations
    // ========================================================================

    bool SB_PhysicalAddress(uint32_t pAddr, uint32_t value);
    bool SH_PhysicalAddress(uint32_t pAddr, uint32_t value);
    bool SW_PhysicalAddress(uint32_t pAddr, uint32_t value);
    bool SD_PhysicalAddress(uint32_t pAddr, uint64_t value);

    // ========================================================================
    // Platform-Specific Exception Filtering
    // ========================================================================

#if defined(__i386__) || defined(_M_IX86)
    struct X86_CONTEXT {
        uint32_t* Edi;
        uint32_t* Esi;
        uint32_t* Ebx;
        uint32_t* Edx;
        uint32_t* Ecx;
        uint32_t* Eax;
        uint32_t* Eip;
        uint32_t* Esp;
        uint32_t* Ebp;
    };

    static bool FilterX86Exception(uint32_t memAddress, X86_CONTEXT& context);
#endif

#if defined(__amd64__) || defined(_M_X64)
    struct X64_CONTEXT {
        uint64_t* Rdi;
        uint64_t* Rsi;
        uint64_t* Rbx;
        uint64_t* Rdx;
        uint64_t* Rcx;
        uint64_t* Rax;
        uint64_t* R8;
        uint64_t* R9;
        uint64_t* R10;
        uint64_t* R11;
        uint64_t* R12;
        uint64_t* R13;
        uint64_t* R14;
        uint64_t* R15;
        uint64_t* Rip;
        uint64_t* Rsp;
        uint64_t* Rbp;
    };

    static bool FilterX64Exception(uint32_t memAddress, X64_CONTEXT& context);
#endif

#if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) || defined(_M_ARM64)
    static void DumpArmExceptionInfo(uint32_t memAddress, void* context);
    static bool FilterArmException(uint32_t memAddress, void* context);
#endif

    /**
     * @brief Free allocated memory
     */
    void FreeMemory();

    // System references
    ::CN64System& m_System;
    ::N64System::CTLB& m_TLB;

    std::unique_ptr<CEeprom> m_Eeprom;
    std::unique_ptr<CSram> m_Sram;
    std::unique_ptr<CFlashRam> m_FlashRam;
    UncachedRom* m_RomMemoryHandler;

    // Memory handlers (initialized in Initialize())
    DisplayProcessorHandler* m_DPCommandRegistersHandler;
    MIPSYInterfaceHandler* m_MIPSInterfaceHandler;
    PeripheralInterfaceHandler* m_PeripheralInterfaceHandler;
    PifRamHandler* m_PifRamHandler;
    RDRAMInterfaceHandler* m_RDRAMInterfaceHandler;
    RDRAMRegistersHandler* m_RDRAMRegistersHandler;
    SerialInterfaceHandler* m_SerialInterfaceHandler;
    SRegistersHandler* m_SPRegistersHandler;
    VideoInterfaceHandler* m_VideoInterfaceHandler;
    AudioInterfaceHandler* m_AudioInterfaceHandler;

    // Main memory
    uint8_t* m_RDRAM = nullptr;
    uint32_t m_AllocatedRdramSize = 0;

    // Debug label buffer
    mutable char m_strLabelName[100] = {};

    // TLB maps (for fast address translation)
    uint32_t* m_TLB_ReadMap = nullptr;
    uint32_t* m_TLB_WriteMap = nullptr;

    // Memory maps (for recompiler)
    size_t* m_MemoryReadMap = nullptr;
    size_t* m_MemoryWriteMap = nullptr;

public:
    // SMC (self-modifying code) bitmap.
    // One byte per physical RDRAM chunk; non-zero means the chunk has compiled recompiler code.
    // Checked inline by the SW JIT fast path; if set, falls through
    // to slow path which does the write + code invalidation.
    static constexpr uint32_t kRdramPages = 0x800000 / 0x1000; // 2048
    static constexpr uint32_t kSmcShift = 6;                   // 64-byte chunks
    static constexpr uint32_t kSmcChunkSize = 1 << kSmcShift;
    static constexpr uint32_t kSmcChunks = 8388608 / kSmcChunkSize;

    uint8_t m_SmcPageBitmap[kSmcChunks] = {};

    uint8_t* GetSmcPageBitmap() { return m_SmcPageBitmap; }

    // ========================================================================
    // IS-Viewer Debug Output (0x13FF0000)
    // ========================================================================
    static constexpr uint32_t IS_VIEWER_BASE = 0x13FF0000;
    static constexpr uint32_t IS_VIEWER_LENGTH = 0x13FF0014;
    static constexpr uint32_t IS_VIEWER_BUFFER = 0x13FF0020;
    static constexpr uint32_t IS_VIEWER_BUF_END = 0x13FF0220;

    uint8_t m_ISViewerBuf[0x200] = {};
    uint32_t m_ISViewerLen = 0;
    char m_ISViewerLineBuf[1024] = {};
    uint32_t m_ISViewerLinePos = 0;

    void ISViewer_Write32(uint32_t pAddr, uint32_t value);
    void ISViewer_WriteByte(uint32_t pAddr, uint8_t value);

private:
    // Register modification value for exception handling
    static uint32_t RegModValue;
};

// ============================================================================
// N64 Memory Map Constants
// ============================================================================

namespace MemoryMap {

// RDRAM
constexpr uint32_t RDRAM_START = 0x00000000;
constexpr uint32_t RDRAM_END = 0x03FFFFFF;

// RDRAM Registers
constexpr uint32_t RDRAM_REG_START = 0x03F00000;
constexpr uint32_t RDRAM_REG_END = 0x03FFFFFF;

// SP (RSP) Memory
constexpr uint32_t SP_DMEM_START = 0x04000000;
constexpr uint32_t SP_DMEM_END = 0x04000FFF;
constexpr uint32_t SP_IMEM_START = 0x04001000;
constexpr uint32_t SP_IMEM_END = 0x04001FFF;

// SP Registers
constexpr uint32_t SP_REG_START = 0x04040000;
constexpr uint32_t SP_REG_END = 0x040FFFFF;

// DP Command Registers
constexpr uint32_t DPC_REG_START = 0x04100000;
constexpr uint32_t DPC_REG_END = 0x041FFFFF;

// DP Span Registers
constexpr uint32_t DPS_REG_START = 0x04200000;
constexpr uint32_t DPS_REG_END = 0x042FFFFF;

// MIPS Interface
constexpr uint32_t MI_REG_START = 0x04300000;
constexpr uint32_t MI_REG_END = 0x043FFFFF;

// Video Interface
constexpr uint32_t VI_REG_START = 0x04400000;
constexpr uint32_t VI_REG_END = 0x044FFFFF;

// Audio Interface
constexpr uint32_t AI_REG_START = 0x04500000;
constexpr uint32_t AI_REG_END = 0x045FFFFF;

// Peripheral Interface
constexpr uint32_t PI_REG_START = 0x04600000;
constexpr uint32_t PI_REG_END = 0x046FFFFF;

// RDRAM Interface
constexpr uint32_t RI_REG_START = 0x04700000;
constexpr uint32_t RI_REG_END = 0x047FFFFF;

// Serial Interface
constexpr uint32_t SI_REG_START = 0x04800000;
constexpr uint32_t SI_REG_END = 0x048FFFFF;

// Cartridge Domain 2 Address 1 (SRAM/FlashRAM)
constexpr uint32_t CART_DOM2_ADDR1_START = 0x05000000;
constexpr uint32_t CART_DOM2_ADDR1_END = 0x05FFFFFF;

// Cartridge Domain 1 Address 1 (64DD registers)
constexpr uint32_t CART_DOM1_ADDR1_START = 0x06000000;
constexpr uint32_t CART_DOM1_ADDR1_END = 0x07FFFFFF;

// Cartridge Domain 2 Address 2 (SRAM)
constexpr uint32_t CART_DOM2_ADDR2_START = 0x08000000;
constexpr uint32_t CART_DOM2_ADDR2_END = 0x0FFFFFFF;

// Cartridge Domain 1 Address 2 (ROM)
constexpr uint32_t CART_DOM1_ADDR2_START = 0x10000000;
constexpr uint32_t CART_DOM1_ADDR2_END = 0x1FBFFFFF;

// PIF RAM/ROM
constexpr uint32_t PIF_ROM_START = 0x1FC00000;
constexpr uint32_t PIF_ROM_END = 0x1FC007BF;
constexpr uint32_t PIF_RAM_START = 0x1FC007C0;
constexpr uint32_t PIF_RAM_END = 0x1FC007FF;

// Cartridge Domain 1 Address 3
constexpr uint32_t CART_DOM1_ADDR3_START = 0x1FD00000;
constexpr uint32_t CART_DOM1_ADDR3_END = 0x7FFFFFFF;

// Kernel segments
constexpr uint32_t KSEG0_START = 0x80000000; // Cached, unmapped
constexpr uint32_t KSEG0_END = 0x9FFFFFFF;
constexpr uint32_t KSEG1_START = 0xA0000000; // Uncached, unmapped
constexpr uint32_t KSEG1_END = 0xBFFFFFFF;
constexpr uint32_t KSSEG_START = 0xC0000000; // TLB mapped
constexpr uint32_t KSSEG_END = 0xDFFFFFFF;
constexpr uint32_t KSEG3_START = 0xE0000000; // TLB mapped
constexpr uint32_t KSEG3_END = 0xFFFFFFFF;

/**
 * @brief Convert KSEG0/KSEG1 address to physical address
 * @param vAddr Virtual address in KSEG0 or KSEG1
 * @return Physical address
 */
constexpr uint32_t ToPhysical(uint32_t vAddr) noexcept { return vAddr & 0x1FFFFFFF; }

/**
 * @brief Check if address is in KSEG0 (cached unmapped)
 */
constexpr bool IsKSeg0(uint32_t vAddr) noexcept { return vAddr >= KSEG0_START && vAddr <= KSEG0_END; }

/**
 * @brief Check if address is in KSEG1 (uncached unmapped)
 */
constexpr bool IsKSeg1(uint32_t vAddr) noexcept { return vAddr >= KSEG1_START && vAddr <= KSEG1_END; }

/**
 * @brief Check if address requires TLB translation
 */
constexpr bool NeedsTLB(uint32_t vAddr) noexcept { return vAddr < KSEG0_START || vAddr >= KSSEG_START; }

} // namespace MemoryMap

} // namespace Nova64
