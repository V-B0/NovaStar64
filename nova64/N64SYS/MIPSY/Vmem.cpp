/**
 * @file Vmem.cpp
 * @brief Virtual Memory Management Implementation
 * 
 * Implementation of MipsMemoryVM class providing virtual-to-physical 
 * address translation and memory access for the MIPS R4300i CPU.
 * 
 * Based on Project64's MemoryVirtualMem implementation.
 */

#include "Vmem.hpp"
#include "DDisk.hpp"
#include "Register.hpp"
#include "TLB.hpp"
#include "../N64Rom.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Memoryhandler/StubHandlers.hpp"
#include "../../common/MemoryManagment.hpp"
#include "../../common/log.hpp"

#include <cstring>
#include <cstdio>

// ============================================================================
// Exception Code Constants (used by TriggerAddressException)
// ============================================================================

constexpr uint32_t EXC_RADE = 4;   // Read Address Error
constexpr uint32_t EXC_WADE = 5;   // Write Address Error
constexpr uint32_t EXC_RMISS = 2;  // Read TLB Miss
constexpr uint32_t EXC_WMISS = 3;  // Write TLB Miss

// External global ROM instance
extern N64Rom* g_Rom;

namespace Nova64 {

// Static member initialization
uint32_t MipsMemoryVM::RegModValue = 0;

// ============================================================================
// Constructor / Destructor
// ============================================================================

MipsMemoryVM::MipsMemoryVM(::CN64System& system, ::N64System::CTLB& tlb)
    : m_System(system)
    , m_TLB(tlb)
    , m_DPCommandRegistersHandler(new DisplayProcessorHandler())
    , m_MIPSInterfaceHandler(new MIPSYInterfaceHandler())
    , m_PeripheralInterfaceHandler(new StubPeripheralInterfaceHandler())
    , m_PifRamHandler(new StubPifRamHandler())
    , m_RomMemoryHandler(new StubRomMemoryHandler())
    , m_RDRAMInterfaceHandler(new StubRDRAMInterfaceHandler())
    , m_RDRAMRegistersHandler(new StubRDRAMRegistersHandler())
    , m_SerialInterfaceHandler(new StubSerialInterfaceHandler())
    , m_SPRegistersHandler(new StubSPRegistersHandler())
    , m_VideoInterfaceHandler(new StubVideoInterfaceHandler())
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
    static CSram sram(false);  // TODO: Use system save directory
    return sram;
}

CFlashRam& MipsMemoryVM::GetFlashRam()
{
    static CFlashRam flashRam(false);  // TODO: Use system save directory
    return flashRam;
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
    if (read && pAddr >= 0x10000000 && (pAddr + size) < (0x10000000 + g_Rom->GetRomSize()))
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
        // Handle big-endian 64-bit store
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
        // TODO: Audio Interface
        value = 0;
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
        // TODO: Audio Interface
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
    if (self == nullptr)
    {
        return;
    }

    // TODO: Implement RDRAM size change notification
    // This would be called when settings change the RDRAM size
    // For now, we just log that this was called
    Logging::log(Logging::Level::Debug, "RdramChanged callback invoked");
}

void MipsMemoryVM::ChangeMiIntrMask()
{
    // TODO: Implement MI interrupt mask change handling
    // This is called from exception filter code to modify the MI_INTR_MASK register
    // The RegModValue static member contains the modification flags
    //
    // MI_INTR_MASK bits:
    //   CLR_SP / SET_SP - Clear/Set SP interrupt mask
    //   CLR_SI / SET_SI - Clear/Set SI interrupt mask  
    //   CLR_AI / SET_AI - Clear/Set AI interrupt mask
    //   CLR_VI / SET_VI - Clear/Set VI interrupt mask
    //   CLR_PI / SET_PI - Clear/Set PI interrupt mask
    //   CLR_DP / SET_DP - Clear/Set DP interrupt mask
}

} // namespace Nova64

