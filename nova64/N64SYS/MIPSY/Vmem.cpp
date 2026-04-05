/**
 * @file Vmem.cpp
 * @brief Virtual Memory Management Implementation
 * 
 * Implementation of MipsMemoryVM class providing virtual-to-physical 
 * address translation and memory access for the MIPS R4300i CPU.
 * 
 */

#include "Vmem.hpp"
#include "DDisk.hpp"
#include "Register.hpp"
#include "TLB.hpp"
#include "../N64Rom.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../../common/MemoryManagment.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

// ============================================================================
// Exception Code Constants (used by TriggerAddressException)
// ============================================================================

constexpr uint32_t EXC_RADE = 4;   // Read Address Error
constexpr uint32_t EXC_WADE = 5;   // Write Address Error
constexpr uint32_t EXC_RMISS = 2;  // Read TLB Miss
constexpr uint32_t EXC_WMISS = 3;  // Write TLB Miss

// External global ROM instance
extern N64Rom* g_Rom;
extern CN64System* g_System;

namespace Nova64 {

// Static member initialization
uint32_t MipsMemoryVM::RegModValue = 0;

// ============================================================================
// Constructor / Destructor
// ============================================================================

MipsMemoryVM::MipsMemoryVM(::CN64System& system, ::N64System::CTLB& tlb)
    : m_System(system)
    , m_TLB(tlb)
    , m_Eeprom(std::make_unique<CEeprom>())
    , m_Sram(std::make_unique<CSram>(false))
    , m_FlashRam(std::make_unique<CFlashRam>(false))
    , m_RomMemoryHandler(new UncachedRom(m_System, m_TLB.GetRegisters(), *this))
    , m_DPCommandRegistersHandler(new DisplayProcessorHandler(m_TLB.GetRegisters()))
    , m_MIPSInterfaceHandler(new MIPSYInterfaceHandler(m_TLB.GetRegisters()))
    , m_PeripheralInterfaceHandler(new PeripheralInterfaceHandler(m_System, *this, m_TLB.GetRegisters(), *m_Eeprom, *m_RomMemoryHandler))
    , m_PifRamHandler(new PifRamHandler(m_System, *this, m_TLB.GetRegisters(), *m_Eeprom))
    , m_RDRAMInterfaceHandler(new RDRAMInterfaceHandler(m_TLB.GetRegisters()))
    , m_RDRAMRegistersHandler(new RDRAMRegistersHandler(m_TLB.GetRegisters(), {}))
    , m_SerialInterfaceHandler(new SerialInterfaceHandler(*this, m_TLB.GetRegisters(), *m_PifRamHandler))
    , m_SPRegistersHandler(new SRegistersHandler(m_TLB.GetRegisters()))
    , m_VideoInterfaceHandler(new VideoInterfaceHandler(m_System, *this, m_TLB.GetRegisters()))
    , m_AudioInterfaceHandler(new AudioInterfaceHandler(m_System, m_TLB.GetRegisters()))
    , m_MemoryReadMap(nullptr)
    , m_MemoryWriteMap(nullptr)
    , m_TLB_ReadMap(nullptr)
    , m_TLB_WriteMap(nullptr)
    , m_RDRAM(nullptr)
    , m_AllocatedRdramSize(0)
{
}

MipsMemoryVM::~MipsMemoryVM()
{
    delete m_AudioInterfaceHandler;
    delete m_VideoInterfaceHandler;
    delete m_SPRegistersHandler;
    delete m_SerialInterfaceHandler;
    delete m_RDRAMRegistersHandler;
    delete m_RDRAMInterfaceHandler;
    delete m_RomMemoryHandler;
    delete m_PifRamHandler;
    delete m_PeripheralInterfaceHandler;
    delete m_MIPSInterfaceHandler;
    delete m_DPCommandRegistersHandler;
    
    FreeMemory();
}

// ============================================================================
// Initialization
// ============================================================================

bool MipsMemoryVM::Initialize()
{
    if (m_RDRAM != nullptr)
    {
        return true;
    }

    // Allocate RDRAM address space (32MB max for memory mapping)
    m_RDRAM = static_cast<uint8_t*>(AllocateAddressSpace(0x02000000, nullptr));
    if (m_RDRAM == nullptr)
    {
        Logging::log(Logging::Level::Error, "Failed to reserve RDRAM address space");
        FreeMemory();
        return false;
    }

    // Default to 8MB RDRAM (0x800000)
    m_AllocatedRdramSize = 0x800000;

    // Commit the RDRAM memory
    if (CommitMemory(m_RDRAM, m_AllocatedRdramSize, MEM_READWRITE) == nullptr)
    {
        Logging::log(Logging::Level::Error, "Failed to commit RDRAM memory");
        FreeMemory();
        return false;
    }

    // Set RDRAM for SP DMA
    m_SPRegistersHandler->SetRDRAM(m_RDRAM, m_AllocatedRdramSize);

    // Allocate memory maps (1MB entries for 4KB pages in 4GB address space)
    m_MemoryReadMap = new size_t[0x100000];
    if (m_MemoryReadMap == nullptr)
    {
        Logging::log(Logging::Level::Error, "Failed to allocate MemoryReadMap");
        FreeMemory();
        return false;
    }

    m_MemoryWriteMap = new size_t[0x100000];
    if (m_MemoryWriteMap == nullptr)
    {
        Logging::log(Logging::Level::Error, "Failed to allocate MemoryWriteMap");
        FreeMemory();
        return false;
    }

    m_TLB_ReadMap = new uint32_t[0x100000];
    if (m_TLB_ReadMap == nullptr)
    {
        Logging::log(Logging::Level::Error, "Failed to allocate TLB_ReadMap");
        FreeMemory();
        return false;
    }

    m_TLB_WriteMap = new uint32_t[0x100000];
    if (m_TLB_WriteMap == nullptr)
    {
        Logging::log(Logging::Level::Error, "Failed to allocate TLB_WriteMap");
        FreeMemory();
        return false;
    }

    Reset(false);
    return true;
}

void MipsMemoryVM::Reset(bool eraseMemory)
{
    if (eraseMemory && m_RDRAM != nullptr)
    {
        memset(m_RDRAM, 0, m_AllocatedRdramSize);
    }

    if (m_MemoryReadMap != nullptr && m_MemoryWriteMap != nullptr)
    {
        // Initialize all entries to invalid (-1)
        memset(m_MemoryReadMap, 0xFF, 0x100000 * sizeof(m_MemoryReadMap[0]));
        memset(m_MemoryWriteMap, 0xFF, 0x100000 * sizeof(m_MemoryWriteMap[0]));

        // Map KSEG0 (0x80000000) and KSEG1 (0xA0000000) to physical RDRAM
        for (uint32_t i = 0; i < 2; i++)
        {
            uint32_t baseAddress = (i == 0) ? 0x80000000 : 0xA0000000;

            // Map RDRAM
            for (size_t address = baseAddress; address < baseAddress + m_AllocatedRdramSize; address += 0x1000)
            {
                m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
                m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
            }

            // Map DMEM (0x04000000 - 0x04001000)
            for (size_t address = baseAddress + 0x04000000; address < baseAddress + 0x04001000; address += 0x1000)
            {
                m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>(Dmem() - address);
            }

            // Map IMEM (0x04001000 - 0x04002000)
            for (size_t address = baseAddress + 0x04001000; address < baseAddress + 0x04002000; address += 0x1000)
            {
                m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>(Imem() - address);
            }
        }
    }

    if (m_TLB_ReadMap != nullptr && m_TLB_WriteMap != nullptr)
    {
        // Initialize all entries to invalid (-1)
        memset(m_TLB_ReadMap, 0xFF, 0x100000 * sizeof(m_TLB_ReadMap[0]));
        memset(m_TLB_WriteMap, 0xFF, 0x100000 * sizeof(m_TLB_WriteMap[0]));

        // Map KSEG0 and KSEG1 (0x80000000 - 0xBFFFFFFF) to physical addresses
        for (uint32_t address = 0x80000000; address < 0xC0000000; address += 0x1000)
        {
            m_TLB_ReadMap[address >> 12] = (address & 0x1FFFFFFF) - address;
            m_TLB_WriteMap[address >> 12] = (address & 0x1FFFFFFF) - address;
        }
    }
}

void MipsMemoryVM::FreeMemory()
{
    if (m_RDRAM != nullptr)
    {
        DecommitMemory(m_RDRAM, 0x02000000);
        FreeAddressSpace(m_RDRAM, 0x02000000);
        m_RDRAM = nullptr;
    }

    if (m_TLB_ReadMap != nullptr)
    {
        delete[] m_TLB_ReadMap;
        m_TLB_ReadMap = nullptr;
    }

    if (m_TLB_WriteMap != nullptr)
    {
        delete[] m_TLB_WriteMap;
        m_TLB_WriteMap = nullptr;
    }

    if (m_MemoryReadMap != nullptr)
    {
        delete[] m_MemoryReadMap;
        m_MemoryReadMap = nullptr;
    }

    if (m_MemoryWriteMap != nullptr)
    {
        delete[] m_MemoryWriteMap;
        m_MemoryWriteMap = nullptr;
    }
}

// ============================================================================
// TLB Map Management
// ============================================================================

void MipsMemoryVM::RemoveTLBMapping(uint64_t entryHi, uint64_t pageMask)
{
    if (!m_TLB_ReadMap || !m_MemoryReadMap) return;

    uint32_t mask = static_cast<uint32_t>((pageMask >> 13) & 0xFFF);
    uint32_t hi_vpn2 = static_cast<uint32_t>((entryHi >> 13) & 0x7FFFF);
    uint32_t halfPageSize = (mask + 1) << 12;  // bytes per half-page

    // Even page virtual base
    uint32_t vBase = (hi_vpn2 & ~mask) << 13;

    // Unmap both halves (even + odd)
    for (uint32_t half = 0; half < 2; ++half)
    {
        uint32_t base = vBase + half * halfPageSize;
        for (uint32_t off = 0; off < halfPageSize; off += 0x1000)
        {
            uint32_t vPage = (base + off) >> 12;
            if (vPage >= 0x100000) continue;
            // Only unmap if it's outside KSEG0/KSEG1 (those are hard-wired)
            if (vPage >= 0x80000 && vPage < 0xC0000) continue;
            m_TLB_ReadMap[vPage]    = static_cast<uint32_t>(-1);
            m_TLB_WriteMap[vPage]   = static_cast<uint32_t>(-1);
            m_MemoryReadMap[vPage]  = static_cast<size_t>(-1);
            m_MemoryWriteMap[vPage] = static_cast<size_t>(-1);
        }
    }
}

void MipsMemoryVM::ApplyTLBMapping(uint64_t entryHi, uint64_t pageMask,
                                   uint64_t entryLo0, uint64_t entryLo1)
{
    if (!m_TLB_ReadMap || !m_MemoryReadMap) return;

    uint32_t mask = static_cast<uint32_t>((pageMask >> 13) & 0xFFF);
    uint32_t hi_vpn2 = static_cast<uint32_t>((entryHi >> 13) & 0x7FFFF);
    uint32_t halfPageSize = (mask + 1) << 12;

    uint32_t vBase = (hi_vpn2 & ~mask) << 13;

    // Process each half: 0 = even (Lo0), 1 = odd (Lo1)
    for (uint32_t half = 0; half < 2; ++half)
    {
        uint64_t lo = (half == 0) ? entryLo0 : entryLo1;
        bool valid   = (lo & 2) != 0;
        bool dirty   = (lo & 4) != 0;
        uint32_t pfn = static_cast<uint32_t>((lo >> 6) & 0xFFFFF);

        uint32_t vHalfBase = vBase + half * halfPageSize;
        uint32_t pHalfBase = (pfn & ~mask) << 12;

        if (!valid) continue;

        for (uint32_t off = 0; off < halfPageSize; off += 0x1000)
        {
            uint32_t vAddr = vHalfBase + off;
            uint32_t vPage = vAddr >> 12;
            if (vPage >= 0x100000) continue;
            // Don't overwrite KSEG0/KSEG1 hard-wired mappings
            if (vPage >= 0x80000 && vPage < 0xC0000) continue;

            uint32_t pAddr = pHalfBase + off;

            // TLB fast maps  (value = pAddr - vAddr, so pAddr = map[vPage] + vAddr)
            m_TLB_ReadMap[vPage] = pAddr - vAddr;
            if (dirty)
                m_TLB_WriteMap[vPage] = pAddr - vAddr;

            // Direct pointer maps  (only for RDRAM-backed physical addresses)
            if (pAddr < m_AllocatedRdramSize)
            {
                m_MemoryReadMap[vPage] = reinterpret_cast<size_t>(
                    (m_RDRAM + pAddr) - vAddr);
                if (dirty)
                    m_MemoryWriteMap[vPage] = reinterpret_cast<size_t>(
                        (m_RDRAM + pAddr) - vAddr);
            }
        }
    }
}

// ============================================================================
// Memory Region Access
// ============================================================================

uint8_t* MipsMemoryVM::Dmem()
{
    return m_SPRegistersHandler->Dmem();
}

uint8_t* MipsMemoryVM::Imem()
{
    return m_SPRegistersHandler->Imem();
}

CSram& MipsMemoryVM::GetSram()
{
    return *m_Sram;
}

CFlashRam& MipsMemoryVM::GetFlashRam()
{
    return *m_FlashRam;
}

// ============================================================================
// Memory Pointer Access
// ============================================================================

uint8_t* MipsMemoryVM::MemoryPtr(uint32_t vAddr, uint32_t size, bool read)
{
    if (m_TLB_ReadMap[vAddr >> 12] == static_cast<uint32_t>(-1))
    {
        return nullptr;
    }

    uint32_t pAddr = m_TLB_ReadMap[vAddr >> 12] + vAddr;

    // RDRAM access
    if ((pAddr + size) < m_AllocatedRdramSize)
    {
        return m_RDRAM + pAddr;
    }

    // DMEM access (0x04000000 - 0x04001000)
    if (pAddr >= 0x04000000 && (pAddr + size) <= 0x04001000)
    {
        return Dmem() + (pAddr - 0x04000000);
    }

    // IMEM access (0x04001000 - 0x04002000)
    if (pAddr >= 0x04001000 && (pAddr + size) <= 0x04002000)
    {
        return Imem() + (pAddr - 0x04001000);
    }

    // ROM access (read only)
    if (read && pAddr >= 0x10000000 && (pAddr + size) < (0x10000000 + g_Rom->GetSize()))
    {
        return const_cast<uint8_t*>(g_Rom->GetRomAddress()) + (pAddr - 0x10000000);
    }

    return nullptr;
}

// ============================================================================
// Memory Read Operations (Debugger/Tools)
// ============================================================================

bool MipsMemoryVM::MemoryValue8(uint32_t vAddr, uint8_t& value)
{
    uint8_t* ptr = MemoryPtr(vAddr ^ 3, 1, true);
    if (ptr == nullptr)
    {
        return false;
    }
    value = *ptr;
    return true;
}

bool MipsMemoryVM::MemoryValue16(uint32_t vAddr, uint16_t& value)
{
    uint8_t* ptr = MemoryPtr(vAddr ^ 2, 2, true);
    if (ptr == nullptr)
    {
        return false;
    }
    value = *reinterpret_cast<uint16_t*>(ptr);
    return true;
}

bool MipsMemoryVM::MemoryValue32(uint32_t vAddr, uint32_t& value)
{
    uint8_t* ptr = MemoryPtr(vAddr, 4, true);
    if (ptr == nullptr)
    {
        return false;
    }
    value = *reinterpret_cast<uint32_t*>(ptr);
    return true;
}

bool MipsMemoryVM::MemoryValue64(uint32_t vAddr, uint64_t& value)
{
    uint8_t* ptr = MemoryPtr(vAddr, 8, true);
    if (ptr == nullptr)
    {
        return false;
    }
    // Handle endianness - N64 is big-endian
    *((uint32_t*)(&value) + 1) = *reinterpret_cast<uint32_t*>(ptr);
    *((uint32_t*)(&value) + 0) = *reinterpret_cast<uint32_t*>(ptr + 4);
    return true;
}

// ============================================================================
// Memory Write Operations (Debugger/Tools)
// ============================================================================

bool MipsMemoryVM::UpdateMemoryValue8(uint32_t vAddr, uint8_t value)
{
    uint8_t* ptr = MemoryPtr(vAddr ^ 3, 1, false);
    if (ptr == nullptr)
    {
        return false;
    }
    *ptr = value;
    return true;
}

bool MipsMemoryVM::UpdateMemoryValue16(uint32_t vAddr, uint16_t value)
{
    uint8_t* ptr = MemoryPtr(vAddr ^ 2, 2, false);
    if (ptr == nullptr)
    {
        return false;
    }
    *reinterpret_cast<uint16_t*>(ptr) = value;
    return true;
}

bool MipsMemoryVM::UpdateMemoryValue32(uint32_t vAddr, uint32_t value)
{
    uint8_t* ptr = MemoryPtr(vAddr, 4, false);
    if (ptr == nullptr)
    {
        return false;
    }
    *reinterpret_cast<uint32_t*>(ptr) = value;
    return true;
}

// ============================================================================
// CPU Load Operations (64-bit virtual address)
// ============================================================================

bool MipsMemoryVM::LB_Memory(uint64_t vAddr, uint8_t& value)
{
    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_RADE : EXC_RMISS);
            return false;
        }
        return LB_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryReadMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        value = *(memoryPtr + (vAddr32 ^ 3));
        return true;
    }
    return LB_VAddr32(vAddr32, value);
}

bool MipsMemoryVM::LH_Memory(uint64_t vAddr, uint16_t& value)
{
    // Check alignment
    if ((vAddr & 1) != 0)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_RADE);
        return false;
    }

    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_RADE : EXC_RMISS);
            return false;
        }
        return LH_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryReadMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        value = *reinterpret_cast<uint16_t*>(memoryPtr + (vAddr32 ^ 2));
        return true;
    }
    return LH_VAddr32(vAddr32, value);
}

bool MipsMemoryVM::LW_Memory(uint64_t vAddr, uint32_t& value)
{
    // Check alignment
    if ((vAddr & 3) != 0)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_RADE);
        return false;
    }

    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_RADE : EXC_RMISS);
            return false;
        }
        return LW_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryReadMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        value = *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32);
        return true;
    }
    return LW_VAddr32(vAddr32, value);
}

bool MipsMemoryVM::LD_Memory(uint64_t vAddr, uint64_t& value)
{
    // Check alignment
    if ((vAddr & 7) != 0)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_RADE);
        return false;
    }

    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_RADE : EXC_RMISS);
            return false;
        }
        return LD_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryReadMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        // Handle big-endian 64-bit load
        *((uint32_t*)(&value) + 1) = *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32);
        *((uint32_t*)(&value) + 0) = *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32 + 4);
        return true;
    }
    return LD_VAddr32(vAddr32, value);
}

// ============================================================================
// CPU Store Operations (64-bit virtual address)
// ============================================================================

bool MipsMemoryVM::SB_Memory(uint64_t vAddr, uint32_t value)
{
    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_WADE : EXC_WMISS);
            return false;
        }
        return SB_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        *(memoryPtr + (vAddr32 ^ 3)) = static_cast<uint8_t>(value);
        return true;
    }
    return SB_VAddr32(vAddr32, value);
}

bool MipsMemoryVM::SH_Memory(uint64_t vAddr, uint32_t value)
{
    // Check alignment
    if ((vAddr & 1) != 0)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_WADE);
        return false;
    }

    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_WADE : EXC_WMISS);
            return false;
        }
        return SH_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        *reinterpret_cast<uint16_t*>(memoryPtr + (vAddr32 ^ 2)) = static_cast<uint16_t>(value);
        return true;
    }
    return SH_VAddr32(vAddr32, value);
}

bool MipsMemoryVM::SW_Memory(uint64_t vAddr, uint32_t value)
{
    // Check alignment
    if ((vAddr & 3) != 0)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_WADE);
        return false;
    }

    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_WADE : EXC_WMISS);
            return false;
        }
        return SW_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32) = value;
        return true;
    }
    return SW_VAddr32(vAddr32, value);
}

bool MipsMemoryVM::SD_Memory(uint64_t vAddr, uint64_t value)
{
    // Check alignment
    if ((vAddr & 7) != 0)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_WADE);
        return false;
    }

    // Check for 64-bit address mode
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        uint32_t pAddr;
        bool memoryUnused;
        if (!m_TLB.VAddrToPAddr(vAddr, pAddr, memoryUnused))
        {
            m_System.GetRegisters()->TriggerAddressException(vAddr, memoryUnused ? EXC_WADE : EXC_WMISS);
            return false;
        }
        return SD_PhysicalAddress(pAddr, value);
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);
    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32 + 0) = *((uint32_t*)(&value) + 1);
        *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32 + 4) = *((uint32_t*)(&value) + 0);
        return true;
    }
    return SD_VAddr32(vAddr32, value);
}

// ============================================================================
// 32-bit Virtual Address Load Operations
// ============================================================================

bool MipsMemoryVM::LB_VAddr32(uint32_t vAddr, uint8_t& value)
{
    uint32_t baseAddress = m_TLB_ReadMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_RMISS);
        return false;
    }
    return LB_PhysicalAddress(baseAddress + vAddr, value);
}

bool MipsMemoryVM::LH_VAddr32(uint32_t vAddr, uint16_t& value)
{
    uint32_t baseAddress = m_TLB_ReadMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_RMISS);
        return false;
    }
    return LH_PhysicalAddress(baseAddress + vAddr, value);
}

bool MipsMemoryVM::LW_VAddr32(uint32_t vAddr, uint32_t& value)
{
    uint32_t baseAddress = m_TLB_ReadMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_RMISS);
        return false;
    }
    return LW_PhysicalAddress(baseAddress + vAddr, value);
}

bool MipsMemoryVM::LD_VAddr32(uint32_t vAddr, uint64_t& value)
{
    uint32_t baseAddress = m_TLB_ReadMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_RMISS);
        return false;
    }
    return LD_PhysicalAddress(baseAddress + vAddr, value);
}

// ============================================================================
// Physical Address Load Operations
// ============================================================================

bool MipsMemoryVM::LB_PhysicalAddress(uint32_t pAddr, uint8_t& value)
{
    uint32_t readAddress = pAddr & ~3;
    uint32_t value32;

    switch (pAddr & 0xFFF00000)
    {
    case 0x1FC00000:
        m_PifRamHandler->Read32(readAddress, value32);
        break;

    default:
        if (pAddr < RdramSize())
        {
            value = *(m_RDRAM + (pAddr ^ 3));
            return true;
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            if (!m_RomMemoryHandler->Read32(pAddr & ~3, value32))
            {
                return false;
            }
            value = static_cast<uint8_t>((value32 >> (((pAddr & 3) ^ 3) << 3)) & 0xFF);
            return true;
        }
        else
        {
            value32 = 0;
        }
    }

    value = static_cast<uint8_t>((value32 >> (((pAddr & 3) ^ 3) << 3)) & 0xFF);
    return true;
}

bool MipsMemoryVM::LH_PhysicalAddress(uint32_t pAddr, uint16_t& value)
{
    uint32_t readAddress = pAddr & ~1;
    uint32_t value32;

    switch (pAddr & 0xFFF00000)
    {
    case 0x1FC00000:
        m_PifRamHandler->Read32(readAddress, value32);
        break;

    default:
        if (pAddr < RdramSize())
        {
            value = *reinterpret_cast<uint16_t*>(m_RDRAM + (pAddr ^ 2));
            return true;
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            if (!m_RomMemoryHandler->Read32(pAddr & ~3, value32))
            {
                return false;
            }
            value = static_cast<uint16_t>((value32 >> ((((pAddr ^ 2) & 2) << 3))) & 0xFFFF);
            return true;
        }
        else
        {
            value32 = 0;
        }
    }

    value = static_cast<uint16_t>((value32 >> (((pAddr ^ 2) & 2) << 3)) & 0xFFFF);
    return true;
}

bool MipsMemoryVM::LW_PhysicalAddress(uint32_t pAddr, uint32_t& value)
{
    switch (pAddr & 0xFFF00000)
    {
    case 0x03F00000:
        m_RDRAMRegistersHandler->Read32(pAddr, value);
        break;
    case 0x04000000:
        m_SPRegistersHandler->Read32(pAddr, value);
        break;
    case 0x04100000:
        m_DPCommandRegistersHandler->Read32(pAddr, value);
        break;
    case 0x04300000:
        m_MIPSInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04400000:
        m_VideoInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04500000:
        m_AudioInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04600000:
        m_PeripheralInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04700000:
        m_RDRAMInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04800000:
        m_SerialInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x1FC00000:
        m_PifRamHandler->Read32(pAddr, value);
        break;

    default:
        if (pAddr < RdramSize())
        {
            value = *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr);
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            m_RomMemoryHandler->Read32(pAddr, value);
        }
        else
        {
            // Return address as value for unhandled memory
            value = ((pAddr & 0xFFFF) << 16) | (pAddr & 0xFFFF);
        }
        break;
    }
    return true;
}

bool MipsMemoryVM::LD_PhysicalAddress(uint32_t pAddr, uint64_t& value)
{
    if (pAddr < RdramSize())
    {
        *((uint32_t*)(&value) + 1) = *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr);
        *((uint32_t*)(&value) + 0) = *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr + 4);
        return true;
    }
    else if (pAddr < 0x800000)
    {
        value = 0;
        return true;
    }

    value = 0;
    return false;
}

// ============================================================================
// 32-bit Virtual Address Store Operations
// ============================================================================

bool MipsMemoryVM::SB_VAddr32(uint32_t vAddr, uint32_t value)
{
    uint32_t baseAddress = m_TLB_WriteMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_WMISS);
        return false;
    }
    return SB_PhysicalAddress(baseAddress + vAddr, value);
}

bool MipsMemoryVM::SH_VAddr32(uint32_t vAddr, uint32_t value)
{
    uint32_t baseAddress = m_TLB_WriteMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_WMISS);
        return false;
    }
    return SH_PhysicalAddress(baseAddress + vAddr, value);
}

bool MipsMemoryVM::SW_VAddr32(uint32_t vAddr, uint32_t value)
{
    uint32_t baseAddress = m_TLB_WriteMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_WMISS);
        return false;
    }
    return SW_PhysicalAddress(baseAddress + vAddr, value);
}

bool MipsMemoryVM::SD_VAddr32(uint32_t vAddr, uint64_t value)
{
    uint32_t baseAddress = m_TLB_WriteMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        m_System.GetRegisters()->TriggerAddressException(static_cast<int32_t>(vAddr), EXC_WMISS);
        return false;
    }
    return SD_PhysicalAddress(baseAddress + vAddr, value);
}

// ============================================================================
// Physical Address Store Operations
// ============================================================================

bool MipsMemoryVM::SB_PhysicalAddress(uint32_t pAddr, uint32_t value)
{
    switch (pAddr & 0xFFF00000)
    {
    case 0x00000000:
    case 0x00100000:
    case 0x00200000:
    case 0x00300000:
    case 0x00400000:
    case 0x00500000:
    case 0x00600000:
    case 0x00700000:
        if (pAddr < RdramSize())
        {
            *(m_RDRAM + (pAddr ^ 3)) = static_cast<uint8_t>(value);
        }
        break;

    case 0x04000000:
        m_SPRegistersHandler->Write32(pAddr & ~3, value << ((3 - (pAddr & 3)) * 8), 0xFFFFFFFF);
        break;

    case 0x1FC00000:
        m_PifRamHandler->Write32(pAddr & ~3, value << ((3 - (pAddr & 3)) * 8), 0xFFFFFFFF);
        break;

    default:
        if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            m_RomMemoryHandler->Write32(pAddr, value << ((3 - (pAddr & 3)) * 8), 0xFFFFFFFF);
        }
        break;
    }
    return true;
}

bool MipsMemoryVM::SH_PhysicalAddress(uint32_t pAddr, uint32_t value)
{
    switch (pAddr & 0xFFF00000)
    {
    case 0x00000000:
    case 0x00100000:
    case 0x00200000:
    case 0x00300000:
    case 0x00400000:
    case 0x00500000:
    case 0x00600000:
    case 0x00700000:
        if (pAddr < RdramSize())
        {
            *reinterpret_cast<uint16_t*>(m_RDRAM + (pAddr ^ 2)) = static_cast<uint16_t>(value);
        }
        break;

    case 0x04000000:
        m_SPRegistersHandler->Write32(pAddr & ~3, value << ((2 - (pAddr & 2)) * 8), 0xFFFFFFFF);
        break;

    case 0x1FC00000:
        m_PifRamHandler->Write32(pAddr & ~3, value << ((2 - (pAddr & 2)) * 8), 0xFFFFFFFF);
        break;

    default:
        if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            m_RomMemoryHandler->Write32(pAddr, value << ((2 - (pAddr & 2)) * 8), 0xFFFFFFFF);
        }
        break;
    }
    return true;
}

bool MipsMemoryVM::SW_PhysicalAddress(uint32_t pAddr, uint32_t value)
{
    switch (pAddr & 0xFFF00000)
    {
    case 0x00000000:
    case 0x00100000:
    case 0x00200000:
    case 0x00300000:
    case 0x00400000:
    case 0x00500000:
    case 0x00600000:
    case 0x00700000:
    case 0x00800000:
        if (pAddr < RdramSize())
        {
            *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr) = value;
        }
        break;

    case 0x03F00000:
        m_RDRAMRegistersHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04000000:
        m_SPRegistersHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04100000:
        m_DPCommandRegistersHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04300000:
        m_MIPSInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04400000:
        m_VideoInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04500000:
        m_AudioInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04600000:
        m_PeripheralInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04700000:
        m_RDRAMInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04800000:
        m_SerialInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x1FC00000:
        m_PifRamHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    default:
        if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            m_RomMemoryHandler->Write32(pAddr, value, 0xFFFFFFFF);
        }
        break;
    }
    return true;
}

bool MipsMemoryVM::SD_PhysicalAddress(uint32_t pAddr, uint64_t value)
{
    switch (pAddr & 0xFFF00000)
    {
    case 0x00000000:
    case 0x00100000:
    case 0x00200000:
    case 0x00300000:
    case 0x00400000:
    case 0x00500000:
    case 0x00600000:
    case 0x00700000:
        if (pAddr < RdramSize())
        {
            *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr) = *((uint32_t*)(&value) + 1);
            *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr + 4) = *((uint32_t*)(&value) + 0);
        }
        break;

    case 0x04000000:
        m_SPRegistersHandler->Write32(pAddr, static_cast<uint32_t>(value >> 32), 0xFFFFFFFF);
        break;

    default:
        if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            m_RomMemoryHandler->Write32(pAddr, static_cast<uint32_t>(value >> 32), 0xFFFFFFFF);
        }
        break;
    }
    return true;
}

// ============================================================================
// Address Translation
// ============================================================================

bool MipsMemoryVM::ValidVaddr(uint32_t vAddr) const
{
    return m_TLB_ReadMap[vAddr >> 12] != static_cast<uint32_t>(-1);
}

bool MipsMemoryVM::VAddrToPAddr(uint32_t vAddr, uint32_t& pAddr) const
{
    if (m_TLB_ReadMap[vAddr >> 12] == static_cast<uint32_t>(-1))
    {
        return false;
    }
    pAddr = m_TLB_ReadMap[vAddr >> 12] + vAddr;
    return true;
}

// ============================================================================
// TLB Notification Functions
// ============================================================================

void MipsMemoryVM::TLB_Mapped(uint32_t vAddr, uint32_t len, uint32_t pAddr, bool readOnly)
{
    uint64_t vEnd = static_cast<uint64_t>(vAddr) + len;

    for (uint64_t address = vAddr; address < vEnd; address += 0x1000)
    {
        size_t index = static_cast<size_t>(address >> 12);
        uint32_t targetPAddr = static_cast<uint32_t>(address - vAddr + pAddr);

        // Map for reading
        if (targetPAddr < m_AllocatedRdramSize)
        {
            m_MemoryReadMap[index] = reinterpret_cast<size_t>((m_RDRAM + targetPAddr) - address);
        }
        m_TLB_ReadMap[index] = static_cast<uint32_t>(targetPAddr - address);

        // Map for writing (if not read-only)
        if (!readOnly)
        {
            if (targetPAddr < m_AllocatedRdramSize)
            {
                m_MemoryWriteMap[index] = reinterpret_cast<size_t>((m_RDRAM + targetPAddr) - address);
            }
            m_TLB_WriteMap[index] = static_cast<uint32_t>(targetPAddr - address);
        }
    }
}

void MipsMemoryVM::TLB_Unmaped(uint32_t vAddr, uint32_t len)
{
    uint64_t end = static_cast<uint64_t>(vAddr) + len;

    for (uint64_t address = vAddr; address < end && address >= vAddr; address += 0x1000)
    {
        size_t index = static_cast<size_t>(address >> 12);

        // Check if address is in KSEG0/KSEG1 (should remain mapped)
        if (address >= 0x80000000 && address < 0x80000000 + m_AllocatedRdramSize)
        {
            m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
            m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
            m_TLB_ReadMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
            m_TLB_WriteMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
        }
        else if (address >= 0xA0000000 && address < 0xA0000000 + m_AllocatedRdramSize)
        {
            m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
            m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
            m_TLB_ReadMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
            m_TLB_WriteMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
        }
        else if (address >= 0x80000000 && address < 0xC0000000)
        {
            // KSEG0/KSEG1 unmapped region
            m_MemoryReadMap[index] = static_cast<size_t>(-1);
            m_MemoryWriteMap[index] = static_cast<size_t>(-1);
            m_TLB_ReadMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
            m_TLB_WriteMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
        }
        else
        {
            // Fully unmap
            m_MemoryReadMap[index] = static_cast<size_t>(-1);
            m_MemoryWriteMap[index] = static_cast<size_t>(-1);
            m_TLB_ReadMap[index] = static_cast<uint32_t>(-1);
            m_TLB_WriteMap[index] = static_cast<uint32_t>(-1);
        }
    }
}

// ============================================================================
// Memory Map Management
// ============================================================================

void MipsMemoryVM::ClearMemoryWriteMap(uint32_t vAddr, uint32_t length)
{
    uint32_t baseAddress = m_TLB_WriteMap[vAddr >> 12];
    if (baseAddress == static_cast<uint32_t>(-1))
    {
        return;
    }

    uint32_t pAddr = m_TLB_WriteMap[vAddr >> 12] + vAddr;
    for (uint32_t i = pAddr, n = (pAddr + (length & ~0xFFF)) + 0x1000; i < n; i += 0x1000)
    {
        m_MemoryWriteMap[(i + 0x80000000) >> 12] = static_cast<size_t>(-1);
        m_MemoryWriteMap[(i + 0xA0000000) >> 12] = static_cast<size_t>(-1);
    }
}

// ============================================================================
// Exception Filter
// ============================================================================

int32_t MipsMemoryVM::MemoryFilter(uint32_t exptCode, void* exceptionPointer)
{
#if defined(_WIN32)
    if (exptCode == EXCEPTION_ACCESS_VIOLATION)
    {
        PEXCEPTION_POINTERS ep = static_cast<PEXCEPTION_POINTERS>(exceptionPointer);
        // Address that caused the fault is in ExceptionInformation[1]
        uintptr_t accessedAddress = ep->ExceptionRecord->ExceptionInformation[1];
        
#if defined(_M_X64)
        X64_CONTEXT context;
        context.Rax = &ep->ContextRecord->Rax;
        context.Rcx = &ep->ContextRecord->Rcx;
        context.Rdx = &ep->ContextRecord->Rdx;
        context.Rbx = &ep->ContextRecord->Rbx;
        context.Rsp = &ep->ContextRecord->Rsp;
        context.Rbp = &ep->ContextRecord->Rbp;
        context.Rsi = &ep->ContextRecord->Rsi;
        context.Rdi = &ep->ContextRecord->Rdi;
        context.R8  = &ep->ContextRecord->R8;
        context.R9  = &ep->ContextRecord->R9;
        context.R10 = &ep->ContextRecord->R10;
        context.R11 = &ep->ContextRecord->R11;
        context.R12 = &ep->ContextRecord->R12;
        context.R13 = &ep->ContextRecord->R13;
        context.R14 = &ep->ContextRecord->R14;
        context.R15 = &ep->ContextRecord->R15;
        context.Rip = &ep->ContextRecord->Rip;

        if (FilterX64Exception(static_cast<uint32_t>(accessedAddress), context))
        {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
#elif defined(_M_IX86)
        X86_CONTEXT context;
        context.Eax = &ep->ContextRecord->Eax;
        context.Ecx = &ep->ContextRecord->Ecx;
        context.Edx = &ep->ContextRecord->Edx;
        context.Ebx = &ep->ContextRecord->Ebx;
        context.Esp = &ep->ContextRecord->Esp;
        context.Ebp = &ep->ContextRecord->Ebp;
        context.Esi = &ep->ContextRecord->Esi;
        context.Edi = &ep->ContextRecord->Edi;
        context.Eip = &ep->ContextRecord->Eip;

        if (FilterX86Exception(static_cast<uint32_t>(accessedAddress), context))
        {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
#endif
    }
#endif
    // Platform-specific exception handling would go here
    // For now, return 0 to indicate exception was not handled
    (void)exptCode;
    (void)exceptionPointer;
    return 0;
}

bool MipsMemoryVM::MemoryBreakpoint()
{
    // Breakpoint handling for debugger
    return false;
}

// ============================================================================
// Debug Support
// ============================================================================

const char* MipsMemoryVM::LabelName(uint32_t address) const
{
    sprintf(m_strLabelName, "0x%08X", address);
    return m_strLabelName;
}

// ============================================================================
// Static Callbacks
// ============================================================================

void MipsMemoryVM::RdramChanged(MipsMemoryVM* self)
{
    if (self == nullptr || self->m_RDRAMRegistersHandler == nullptr)
    {
        return;
    }

    // Retrieve actual RDRAM size from registers handler
    // If Expansion Pak is enabled, we use 8MB, otherwise 4MB
    uint32_t newSize = self->m_RDRAMRegistersHandler->is_expansion_pak_enabled() ? 0x800000 : 0x400000;
    
    if (newSize != self->m_AllocatedRdramSize)
    {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "RdramChanged: Size changing from 0x%X to 0x%X", 
                     self->m_AllocatedRdramSize, newSize);
        Logging::log(Logging::Level::Info, buffer);
        
        // Handle memory commitment changes
        if (newSize > self->m_AllocatedRdramSize)
        {
            // Expansion: Commit new memory
            if (CommitMemory(self->m_RDRAM + self->m_AllocatedRdramSize, 
                             newSize - self->m_AllocatedRdramSize, MEM_READWRITE) == nullptr)
            {
                Logging::log(Logging::Level::Error, "RdramChanged: Failed to commit memory for expansion");
                return;
            }
        }
        else
        {
            // Reduction: Decommit extra memory
            DecommitMemory(self->m_RDRAM + newSize, self->m_AllocatedRdramSize - newSize);
        }

        // Update size
        self->m_AllocatedRdramSize = newSize;
        
        // Update SP RDRAM pointers
        if (self->m_SPRegistersHandler)
        {
            self->m_SPRegistersHandler->SetRDRAM(self->m_RDRAM, self->m_AllocatedRdramSize);
        }
        
        // Remap virtual memory
        self->Reset(false);
    }
}

void MipsMemoryVM::ChangeMiIntrMask()
{
    if (g_System)
    {
        MipsMemoryVM* vm = g_System->GetMemory();
        if (vm && vm->m_MIPSInterfaceHandler)
        {
            // Apply interrupt mask changes (Set/Clear bits are in RegModValue)
            // MI_INTR_MASK_REG is at offset 0xC from MI base (0x04300000)
            vm->m_MIPSInterfaceHandler->Write32(0x0430000C, RegModValue, 0xFFFFFFFF);
        }
    }
    
    // Reset the modification value
    RegModValue = 0;
}


// ============================================================================
// Platform-Specific Exception Handlers
// ============================================================================

#if defined(__amd64__) || defined(_M_X64)
bool MipsMemoryVM::FilterX64Exception(uint32_t memAddress, X64_CONTEXT& context)
{
    // Exception-based lazy TLB mapping for the x64 recompiler
    // When the recompiler generates direct memory accesses, an access violation
    // triggers this filter which should:
    // 1. Check if the faulting address is within the RDRAM virtual mapping
    // 2. Look up the TLB entry for the faulting virtual address
    // 3. Map the physical page and update the page table
    // 4. Resume execution at the faulting instruction
    
    // Currently returns false (unhandled) since the recompiler is not yet implemented.
    // A full implementation would use VirtualAlloc/mmap to commit pages on demand
    // and patch the recompiler's memory mapping table.
    
    Log::debugf("FilterX64Exception: Access violation at 0x%08X, RIP=0x%016llX",
                memAddress, context.Rip ? *context.Rip : 0);
    return false;
}
#endif

#if defined(__i386__) || defined(_M_IX86)
bool MipsMemoryVM::FilterX86Exception(uint32_t memAddress, X86_CONTEXT& context)
{
    // Exception-based lazy TLB mapping for the x86 recompiler
    // Same concept as x64 version but for 32-bit targets
    
    Log::debugf("FilterX86Exception: Access violation at 0x%08X, EIP=0x%08X",
                memAddress, context.Eip ? *context.Eip : 0);
    return false;
}
#endif

} // namespace Nova64

