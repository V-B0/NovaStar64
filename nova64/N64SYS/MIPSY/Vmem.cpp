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
#include <cstdlib>
#include <string>
#include <vector>

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
// Memory Write Watch (debug-only, env-var driven)
// ----------------------------------------------------------------------------
// NOVA64_WATCH_ADDR  : hex (KSEG0 / KSEG1 / physical) start address. Required.
// NOVA64_WATCH_LEN   : hex bytes of range, default 1.
// NOVA64_WATCH_FILE  : output path; default stderr.
// NOVA64_WATCH_FIRST : if "1", log only the FIRST write to each 8-byte aligned
//                    address (huge noise reduction; surfaces fresh writes).
// NOVA64_WATCH_VAL   : hex value filter; only log writes whose value matches.
// NOVA64_WATCH_MASK  : hex mask applied before comparing to NOVA64_WATCH_VAL
//                    (default ~0). Useful for "only log writes that look like
//                    pointers" (mask=0xF0000000 val=0x80000000) etc.
// NOVA64_DUMP_RDRAM  : path. If set, the full 8 MB RDRAM is dumped here at
//                    process exit. Take two runs (good vs bad) and `fc /b`.
// ============================================================================
namespace {

struct WatchConfig
{
    bool     enabled        = false;
    bool     first_only     = false;
    bool     val_filter     = false;
    uint32_t pStart         = 0;   // physical start
    uint32_t pEnd           = 0;   // physical end (exclusive)
    uint64_t val_match      = 0;
    uint64_t val_mask       = ~uint64_t(0);
    FILE*    out            = nullptr;
    // Bitmap for first-only mode: one bit per 8-byte slot in the watched range.
    std::vector<uint8_t> seen;
};

const WatchConfig& GetWatchConfig()
{
    static WatchConfig cfg = []() {
        WatchConfig c;
        const char* addrEnv = std::getenv("NOVA64_WATCH_ADDR");
        if (addrEnv == nullptr || *addrEnv == '\0') return c;
        uint64_t addr = std::strtoull(addrEnv, nullptr, 16);
        uint64_t len  = 1;
        const char* lenEnv = std::getenv("NOVA64_WATCH_LEN");
        if (lenEnv != nullptr && *lenEnv != '\0')
            len = std::strtoull(lenEnv, nullptr, 16);
        if (len == 0) return c;
        // Mask to physical (strip KSEG0/KSEG1 segment bits)
        uint32_t pStart = static_cast<uint32_t>(addr) & 0x1FFFFFFF;
        uint32_t pEnd   = pStart + static_cast<uint32_t>(len);
        c.enabled = true;
        c.pStart  = pStart;
        c.pEnd    = pEnd;

        const char* firstEnv = std::getenv("NOVA64_WATCH_FIRST");
        if (firstEnv != nullptr && firstEnv[0] == '1')
        {
            c.first_only = true;
            // One bit per 8-byte slot.
            size_t slots = (static_cast<size_t>(pEnd - pStart) + 7) / 8;
            c.seen.assign((slots + 7) / 8, 0);
        }

        const char* valEnv = std::getenv("NOVA64_WATCH_VAL");
        if (valEnv != nullptr && *valEnv != '\0')
        {
            c.val_filter = true;
            c.val_match  = std::strtoull(valEnv, nullptr, 16);
            const char* maskEnv = std::getenv("NOVA64_WATCH_MASK");
            if (maskEnv != nullptr && *maskEnv != '\0')
                c.val_mask = std::strtoull(maskEnv, nullptr, 16);
        }

        const char* fileEnv = std::getenv("NOVA64_WATCH_FILE");
        if (fileEnv != nullptr && *fileEnv != '\0')
            c.out = std::fopen(fileEnv, "w");
        if (c.out == nullptr) c.out = stderr;
        std::fprintf(c.out,
                     "[NOVA64_WATCH] active: phys [0x%08X .. 0x%08X) first=%d val_filter=%d match=0x%llX mask=0x%llX\n",
                     c.pStart, c.pEnd,
                     c.first_only ? 1 : 0,
                     c.val_filter ? 1 : 0,
                     static_cast<unsigned long long>(c.val_match),
                     static_cast<unsigned long long>(c.val_mask));
        std::fflush(c.out);
        return c;
    }();
    return cfg;
}

// Mutable view of the same singleton (so first_only bookkeeping can update).
WatchConfig& GetMutableWatchConfig()
{
    return const_cast<WatchConfig&>(GetWatchConfig());
}

// Return true if this is the first time the 8-byte slot containing pAddr has
// been written. Always returns true if first_only is disabled.
bool MarkAndCheckFirst(uint32_t pAddr)
{
    WatchConfig& cfg = GetMutableWatchConfig();
    if (!cfg.first_only) return true;
    if (pAddr < cfg.pStart || pAddr >= cfg.pEnd) return true;
    size_t slot = (pAddr - cfg.pStart) / 8;
    size_t byte = slot / 8;
    uint8_t bit = static_cast<uint8_t>(1u << (slot & 7));
    if (byte >= cfg.seen.size()) return true;
    if (cfg.seen[byte] & bit) return false;
    cfg.seen[byte] |= bit;
    return true;
}

// Set from SW_Memory/etc each call (idempotent) so WatchStore can dump the
// instruction word at the writing PC for NOVA64_WATCH_DUMP_INSN diagnostics,
// without threading an extra parameter through every store-instruction path.
uint8_t* g_watchRdramPtr = nullptr;
uint32_t g_watchRdramSize = 0;

inline void WatchStore(uint32_t pAddr, unsigned size, uint64_t value, uint64_t pc)
{
    const WatchConfig& cfg = GetWatchConfig();
    if (!cfg.enabled) return;
    uint32_t end = pAddr + size;
    if (end <= cfg.pStart || pAddr >= cfg.pEnd) return;
    if (cfg.val_filter && (value & cfg.val_mask) != (cfg.val_match & cfg.val_mask))
        return;
    if (cfg.first_only && !MarkAndCheckFirst(pAddr))
        return;
    std::fprintf(cfg.out,
                 "[NOVA64_WATCH] PC=%016llX  S%c phys=0x%08X val=0x%016llX\n",
                 static_cast<unsigned long long>(pc),
                 size == 1 ? 'B' : size == 2 ? 'H' : size == 4 ? 'W' : 'D',
                 pAddr,
                 static_cast<unsigned long long>(value));

    static const bool s_dumpInsn = std::getenv("NOVA64_WATCH_DUMP_INSN") != nullptr;
    if (s_dumpInsn && g_watchRdramPtr) {
        uint32_t pcPhys = static_cast<uint32_t>(pc) & 0x1FFFFFFF;
        if (pcPhys + 24 <= g_watchRdramSize && pcPhys >= 20) {
            std::fprintf(cfg.out, "[NOVA64_WATCH]   insn context @0x%08X:", pcPhys);
            for (int32_t off = -20; off <= 20; off += 4) {
                uint32_t a = pcPhys + static_cast<uint32_t>(off);
                uint32_t word = *reinterpret_cast<const uint32_t*>(g_watchRdramPtr + a);
                std::fprintf(cfg.out, "%s0x%08X", off == 0 ? " [" : " ", word);
                if (off == 0) std::fprintf(cfg.out, "]");
            }
            std::fprintf(cfg.out, "\n");
        }
    }
    std::fflush(cfg.out);
}

} // anonymous namespace

void WatchDMA(uint32_t pAddr, uint32_t length, const char* source)
{
    const WatchConfig& cfg = GetWatchConfig();
    if (!cfg.enabled || length == 0) return;
    uint32_t end = pAddr + length;
    if (end <= cfg.pStart || pAddr >= cfg.pEnd) return;
    uint32_t hitStart = pAddr      > cfg.pStart ? pAddr      : cfg.pStart;
    uint32_t hitEnd   = end        < cfg.pEnd   ? end        : cfg.pEnd;
    std::fprintf(cfg.out,
                 "[NOVA64_WATCH] DMA %-3s phys=0x%08X len=0x%X (hit 0x%08X..0x%08X)\n",
                 source ? source : "?",
                 pAddr, length, hitStart, hitEnd);
    std::fflush(cfg.out);
}

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
    , m_RDRAM(nullptr)
    , m_AllocatedRdramSize(0)
    , m_TLB_ReadMap(nullptr)
    , m_TLB_WriteMap(nullptr)
    , m_MemoryReadMap(nullptr)
    , m_MemoryWriteMap(nullptr)
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

    // Register an atexit handler that dumps RDRAM to NOVA64_DUMP_RDRAM if set.
    // Capturing the pointer + size in static state lets us do it without a
    // reference back to MipsMemoryVM at exit time.
    {
        static uint8_t* s_dumpPtr  = nullptr;
        static uint32_t s_dumpSize = 0;
        s_dumpPtr  = m_RDRAM;
        s_dumpSize = m_AllocatedRdramSize;
        static bool s_registered = false;
        if (!s_registered)
        {
            s_registered = true;
            std::atexit([]() {
                const char* path = std::getenv("NOVA64_DUMP_RDRAM");
                if (path == nullptr || *path == '\0') return;
                if (s_dumpPtr == nullptr || s_dumpSize == 0) return;
                FILE* f = std::fopen(path, "wb");
                if (f == nullptr) return;
                std::fwrite(s_dumpPtr, 1, s_dumpSize, f);
                std::fclose(f);
            });
        }
    }

    // Set RDRAM for SP DMA
    m_SPRegistersHandler->SetRDRAM(m_RDRAM, m_AllocatedRdramSize);

    // Wire DMEM pointer to DPC handler for XBUS mode
    m_DPCommandRegistersHandler->SetDMEM(m_SPRegistersHandler->Dmem());

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
                m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x007FFFFF)) - address);
                m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x007FFFFF)) - address);
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

    // Check for 64-bit address mode: in 32-bit mode, upper 32 bits must be
    // the sign extension of bit 31. Otherwise it's an Address Error.
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_RADE);
        return false;
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
// MI Repeat Mode Support
// ============================================================================

void MipsMemoryVM::SetMIRepeatMode(bool active)
{
    // When MI init mode is set, invalidate KSEG1 (uncached) RDRAM WriteMap pages
    // so the JIT inline store path falls through to the slow path (which handles repeat).
    // Only uncached writes trigger MI repeat on real hardware (cached writes go to L1).
    if (!m_MemoryWriteMap || !m_RDRAM || m_AllocatedRdramSize == 0)
        return;

    uint32_t baseAddress = 0xA0000000; // KSEG1 only
    for (size_t address = baseAddress; address < baseAddress + m_AllocatedRdramSize; address += 0x1000)
    {
        if (active)
            m_MemoryWriteMap[address >> 12] = static_cast<size_t>(-1);
        else
            m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x1FFFFFFF)) - address);
    }
}

bool MipsMemoryVM::MI_RepeatStore(uint32_t pAddr, uint64_t busData, uint32_t patternSize, uint32_t byteOffset)
{
    // MI repeat: the repeat_count+1 total bytes are counted from the 8-byte bus word start.
    // The first (byteOffset) bytes are skipped (before the store address).
    // Writing starts at pAddr and continues using the bus word pattern cyclically.
    // Pattern byte selection is based on (writeAddr & (patternSize-1)).
    // Wraps at 2KiB boundary within the RDRAM region.
    uint32_t miMode = *m_MIPSInterfaceHandler->MI_MODE_REG;
    uint32_t repeatCount = miMode & 0x7F;
    uint32_t totalBytes = repeatCount + 1;
    uint32_t skip = byteOffset;
    if (totalBytes <= skip) return true;
    uint32_t bytesToWrite = totalBytes - skip;

    // Extract big-endian pattern bytes from busData
    uint8_t pattern[8];
    for (uint32_t i = 0; i < patternSize; i++)
        pattern[i] = static_cast<uint8_t>(busData >> ((patternSize - 1 - i) * 8));

    // Base address for 2KiB wrap
    uint32_t wrapBase = pAddr & ~0x7FFu;
    uint32_t offset = pAddr & 0x7FF;

    for (uint32_t i = 0; i < bytesToWrite; i++)
    {
        uint32_t writeAddr = wrapBase | ((offset + i) & 0x7FF);
        if (writeAddr < m_AllocatedRdramSize)
            m_RDRAM[writeAddr ^ 3] = pattern[writeAddr & (patternSize - 1)];
    }

    // Auto-clear INIT mode after the repeat store completes.
    // On real hardware, INIT persists but the test re-asserts it before each store.
    // We clear it here to prevent stale INIT from causing massive fill writes
    // during subsequent non-repeat stores to uncached RDRAM.
    *m_MIPSInterfaceHandler->MI_MODE_REG &= ~0x0080u;
    SetMIRepeatMode(false);

    return true;
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

    // MI repeat mode: only uncached (KSEG1) stores to RDRAM trigger repeat
    if ((vAddr32 & 0xE0000000) == 0xA0000000 && m_MIPSInterfaceHandler->IsInitMode())
    {
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < m_AllocatedRdramSize)
        {
            uint32_t byteOff = vAddr32 & 7;
            // Bus data: 32-bit word with byte positioned in its lane
            uint64_t busData = static_cast<uint64_t>(value << (24 - (vAddr32 & 3) * 8));
            return MI_RepeatStore(pAddr, busData, 4, byteOff);
        }
    }

    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        WatchStore(vAddr32 & 0x1FFFFFFF, 1, value & 0xFF,
                   m_System.GetRegisters()->m_PROGRAM_COUNTER);
        *(memoryPtr + (vAddr32 ^ 3)) = static_cast<uint8_t>(value);
        // SMC detection
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < 0x800000) {
            uint32_t page = pAddr >> kSmcShift;
            if (m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
        }
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

    // MI repeat mode: only uncached (KSEG1) stores to RDRAM trigger repeat
    if ((vAddr32 & 0xE0000000) == 0xA0000000 && m_MIPSInterfaceHandler->IsInitMode())
    {
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < m_AllocatedRdramSize)
        {
            uint32_t byteOff = vAddr32 & 7;
            // Bus data: 32-bit word with halfword positioned in its lane
            uint64_t busData = static_cast<uint64_t>(value << (16 - (vAddr32 & 2) * 8));
            return MI_RepeatStore(pAddr, busData, 4, byteOff);
        }
    }

    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        WatchStore(vAddr32 & 0x1FFFFFFF, 2, value & 0xFFFF,
                   m_System.GetRegisters()->m_PROGRAM_COUNTER);
        *reinterpret_cast<uint16_t*>(memoryPtr + (vAddr32 ^ 2)) = static_cast<uint16_t>(value);
        // SMC detection
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < 0x800000) {
            uint32_t page = pAddr >> kSmcShift;
            if (m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
        }
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

    // Check for 64-bit address mode: in 32-bit mode, upper 32 bits must be
    // the sign extension of bit 31. Otherwise it's an Address Error.
    if (static_cast<uint64_t>(static_cast<int32_t>(vAddr)) != vAddr)
    {
        m_System.GetRegisters()->TriggerAddressException(vAddr, EXC_WADE);
        return false;
    }

    uint32_t vAddr32 = static_cast<uint32_t>(vAddr);

    // MI repeat mode: only uncached (KSEG1) stores to RDRAM trigger repeat
    if ((vAddr32 & 0xE0000000) == 0xA0000000 && m_MIPSInterfaceHandler->IsInitMode())
    {
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < m_AllocatedRdramSize)
        {
            uint32_t byteOff = vAddr32 & 7;
            // Bus data: the full 32-bit value
            uint64_t busData = static_cast<uint64_t>(value);
            return MI_RepeatStore(pAddr, busData, 4, byteOff);
        }
    }

    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        g_watchRdramPtr = m_RDRAM;
        g_watchRdramSize = m_AllocatedRdramSize;
        WatchStore(vAddr32 & 0x1FFFFFFF, 4, value,
                   m_System.GetRegisters()->m_PROGRAM_COUNTER);
        *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32) = value;
        // SMC detection: if this page has compiled code, invalidate it
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < 0x800000) {
            uint32_t page = pAddr >> kSmcShift;
            if (m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
        }
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

    // MI repeat mode: only uncached (KSEG1) stores to RDRAM trigger repeat
    if ((vAddr32 & 0xE0000000) == 0xA0000000 && m_MIPSInterfaceHandler->IsInitMode())
    {
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < m_AllocatedRdramSize)
        {
            // Bus data: the full 64-bit value, pattern size 8
            return MI_RepeatStore(pAddr, value, 8, 0);
        }
    }

    uint8_t* memoryPtr = reinterpret_cast<uint8_t*>(m_MemoryWriteMap[vAddr32 >> 12]);
    if (memoryPtr != reinterpret_cast<uint8_t*>(-1))
    {
        WatchStore(vAddr32 & 0x1FFFFFFF, 8, value,
                   m_System.GetRegisters()->m_PROGRAM_COUNTER);
        *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32 + 0) = *((uint32_t*)(&value) + 1);
        *reinterpret_cast<uint32_t*>(memoryPtr + vAddr32 + 4) = *((uint32_t*)(&value) + 0);
        // SMC detection for SD (8-byte store may span two pages)
        uint32_t pAddr = vAddr32 & 0x1FFFFFFF;
        if (pAddr < 0x800000) {
            uint32_t page = pAddr >> kSmcShift;
            if (m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
            uint32_t page2 = (pAddr + 4) >> kSmcShift;
            if (page2 != page && page2 < kSmcChunks && m_SmcPageBitmap[page2]) {
                m_SmcPageBitmap[page2] = 0;
                m_System.NotifyDMAToRDRAM(page2 << kSmcShift, kSmcChunkSize);
            }
        }
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
        (void)m_PifRamHandler->Read32(readAddress, value32);
        break;

    default:
        if (pAddr < RdramSize())
        {
            value = *(m_RDRAM + (pAddr ^ 3));
            return true;
        }
        else if (pAddr >= 0x08000000 && pAddr < 0x10000000)
        {
            // PI bus is 16-bit: sub-word reads return from upper halfword
            // of the word at adjusted address ((pAddr + 2) & ~3)
            uint32_t adjAddr = (pAddr + 2) & ~3u;
            if (!m_RomMemoryHandler->Read32(adjAddr, value32))
            {
                return false;
            }
            value = static_cast<uint8_t>((value32 >> (24 - (pAddr & 1) * 8)) & 0xFF);
            return true;
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            // PI bus is 16-bit: sub-word reads return from upper halfword
            // of the word at adjusted address ((pAddr + 2) & ~3)
            uint32_t adjAddr = (pAddr + 2) & ~3u;
            if (!m_RomMemoryHandler->Read32(adjAddr, value32))
            {
                return false;
            }
            value = static_cast<uint8_t>((value32 >> (24 - (pAddr & 1) * 8)) & 0xFF);
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
    uint32_t readAddress = pAddr & ~3;
    uint32_t value32;

    switch (pAddr & 0xFFF00000)
    {
    case 0x1FC00000:
        (void)m_PifRamHandler->Read32(readAddress, value32);
        break;

    default:
        if (pAddr < RdramSize())
        {
            value = *reinterpret_cast<uint16_t*>(m_RDRAM + (pAddr ^ 2));
            return true;
        }
        else if (pAddr >= 0x08000000 && pAddr < 0x10000000)
        {
            // PI bus is 16-bit: sub-word reads always return the upper halfword
            // of the word at adjusted address ((pAddr + 2) & ~3)
            uint32_t adjAddr = (pAddr + 2) & ~3u;
            if (!m_RomMemoryHandler->Read32(adjAddr, value32))
            {
                return false;
            }
            value = static_cast<uint16_t>((value32 >> 16) & 0xFFFF);
            return true;
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            // PI bus is 16-bit: sub-word reads always return the upper halfword
            // of the word at adjusted address ((pAddr + 2) & ~3)
            uint32_t adjAddr = (pAddr + 2) & ~3u;
            if (!m_RomMemoryHandler->Read32(adjAddr, value32))
            {
                return false;
            }
            value = static_cast<uint16_t>((value32 >> 16) & 0xFFFF);
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
        (void)m_RDRAMRegistersHandler->Read32(pAddr, value);
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
        (void)m_VideoInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04500000:
        m_AudioInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04600000:
        (void)m_PeripheralInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04700000:
        (void)m_RDRAMInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x04800000:
        (void)m_SerialInterfaceHandler->Read32(pAddr, value);
        break;
    case 0x1FC00000:
        (void)m_PifRamHandler->Read32(pAddr, value);
        break;

    default:
        if (pAddr < RdramSize())
        {
            value = *reinterpret_cast<uint32_t*>(m_RDRAM + pAddr);
        }
        else if (pAddr >= 0x08000000 && pAddr < 0x10000000)
        {
            (void)m_RomMemoryHandler->Read32(pAddr, value);
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            (void)m_RomMemoryHandler->Read32(pAddr, value);
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
            uint32_t page = pAddr >> kSmcShift;
            if (page < kSmcChunks && m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
        }
        break;

    case 0x04000000:
    {
        uint32_t shift = (3 - (pAddr & 3)) * 8;
        m_SPRegistersHandler->Write32(pAddr & ~3, value << shift, 0xFFFFFFFFu);
        break;
    }

    case 0x1FC00000:
    {
        uint32_t shift = (3 - (pAddr & 3)) * 8;
        (void)m_PifRamHandler->Write32(pAddr & ~3, value << shift, 0xFFFFFFFFu);
        break;
    }

    default:
        if ((pAddr & 0xFFFF0000) == IS_VIEWER_BASE)
        {
            ISViewer_WriteByte(pAddr, static_cast<uint8_t>(value));
        }
        else if (pAddr >= 0x08000000 && pAddr < 0x10000000)
        {
            uint32_t shift = (3 - (pAddr & 3)) * 8;
            (void)m_RomMemoryHandler->Write32(pAddr, value << shift, 0xFFu << shift);
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            uint32_t shift = (3 - (pAddr & 3)) * 8;
            (void)m_RomMemoryHandler->Write32(pAddr, value << shift, 0xFFu << shift);
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
            uint32_t page = pAddr >> kSmcShift;
            if (page < kSmcChunks && m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
        }
        break;

    case 0x04000000:
    {
        uint32_t shift = (2 - (pAddr & 2)) * 8;
        m_SPRegistersHandler->Write32(pAddr & ~3, value << shift, 0xFFFFFFFFu);
        break;
    }

    case 0x1FC00000:
    {
        uint32_t shift = (2 - (pAddr & 2)) * 8;
        (void)m_PifRamHandler->Write32(pAddr & ~3, value << shift, 0xFFFFFFFFu);
        break;
    }

    default:
        if (pAddr >= 0x08000000 && pAddr < 0x10000000)
        {
            uint32_t shift = (2 - (pAddr & 2)) * 8;
            (void)m_RomMemoryHandler->Write32(pAddr, value << shift, 0xFFFFu << shift);
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            uint32_t shift = (2 - (pAddr & 2)) * 8;
            (void)m_RomMemoryHandler->Write32(pAddr, value << shift, 0xFFFFu << shift);
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
            // SMC detection: if this page has compiled code, invalidate it
            uint32_t page = pAddr >> kSmcShift;
            if (page < kSmcChunks && m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
        }
        break;

    case 0x03F00000:
        (void)m_RDRAMRegistersHandler->Write32(pAddr, value, 0xFFFFFFFF);
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
        (void)m_VideoInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04500000:
        m_AudioInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04600000:
        (void)m_PeripheralInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04700000:
        (void)m_RDRAMInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x04800000:
        (void)m_SerialInterfaceHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    case 0x1FC00000:
        (void)m_PifRamHandler->Write32(pAddr, value, 0xFFFFFFFF);
        break;

    default:
        if ((pAddr & 0xFFFF0000) == IS_VIEWER_BASE)
        {
            ISViewer_Write32(pAddr, value);
        }
        else if (pAddr >= 0x08000000 && pAddr < 0x10000000)
        {
            (void)m_RomMemoryHandler->Write32(pAddr, value, 0xFFFFFFFF);
        }
        else if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            (void)m_RomMemoryHandler->Write32(pAddr, value, 0xFFFFFFFF);
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
            // SMC detection for SD
            uint32_t page = pAddr >> kSmcShift;
            if (page < kSmcChunks && m_SmcPageBitmap[page]) {
                m_SmcPageBitmap[page] = 0;
                m_System.NotifyDMAToRDRAM(page << kSmcShift, kSmcChunkSize);
            }
            uint32_t page2 = (pAddr + 4) >> kSmcShift;
            if (page2 != page && page2 < kSmcChunks && m_SmcPageBitmap[page2]) {
                m_SmcPageBitmap[page2] = 0;
                m_System.NotifyDMAToRDRAM(page2 << kSmcShift, kSmcChunkSize);
            }
        }
        break;

    case 0x04000000:
        m_SPRegistersHandler->Write32(pAddr, static_cast<uint32_t>(value >> 32), 0xFFFFFFFF);
        break;

    default:
        if (pAddr >= 0x10000000 && pAddr < 0x20000000)
        {
            (void)m_RomMemoryHandler->Write32(pAddr, static_cast<uint32_t>(value >> 32), 0xFFFFFFFF);
        }
        break;
    }
    return true;
}

// ============================================================================
// IS-Viewer Debug Output
// ============================================================================

void MipsMemoryVM::ISViewer_Write32(uint32_t pAddr, uint32_t value)
{
    if (pAddr == IS_VIEWER_LENGTH)
    {
        // Length register written — flush buffer
        uint32_t len = value;
        if (len > sizeof(m_ISViewerBuf)) len = sizeof(m_ISViewerBuf);
        if (len > 0)
        {
            // Accumulate into line buffer and print complete lines
            for (uint32_t i = 0; i < len; ++i)
            {
                char c = static_cast<char>(m_ISViewerBuf[i]);
                if (c == '\n' || m_ISViewerLinePos >= sizeof(m_ISViewerLineBuf) - 1)
                {
                    m_ISViewerLineBuf[m_ISViewerLinePos] = '\0';
                    fprintf(stdout, "[ISViewer] %s\n", m_ISViewerLineBuf);
                    fflush(stdout);
                    m_ISViewerLinePos = 0;
                }
                else if (c != '\r')
                {
                    m_ISViewerLineBuf[m_ISViewerLinePos++] = c;
                }
            }
        }
        m_ISViewerLen = 0;
    }
    else if (pAddr >= IS_VIEWER_BUFFER && pAddr < IS_VIEWER_BUF_END)
    {
        uint32_t off = pAddr - IS_VIEWER_BUFFER;
        if (off + 3 < sizeof(m_ISViewerBuf))
        {
            m_ISViewerBuf[off]     = (value >> 24) & 0xFF;
            m_ISViewerBuf[off + 1] = (value >> 16) & 0xFF;
            m_ISViewerBuf[off + 2] = (value >> 8) & 0xFF;
            m_ISViewerBuf[off + 3] = value & 0xFF;
        }
    }
}

void MipsMemoryVM::ISViewer_WriteByte(uint32_t pAddr, uint8_t value)
{
    if (pAddr >= IS_VIEWER_BUFFER && pAddr < IS_VIEWER_BUF_END)
    {
        uint32_t off = pAddr - IS_VIEWER_BUFFER;
        if (off < sizeof(m_ISViewerBuf))
        {
            m_ISViewerBuf[off] = value;
        }
    }
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
            m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x007FFFFF)) - address);
            m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x007FFFFF)) - address);
            m_TLB_ReadMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
            m_TLB_WriteMap[address >> 12] = static_cast<uint32_t>((address & 0x1FFFFFFF) - address);
        }
        else if (address >= 0xA0000000 && address < 0xA0000000 + m_AllocatedRdramSize)
        {
            m_MemoryReadMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x007FFFFF)) - address);
            m_MemoryWriteMap[address >> 12] = reinterpret_cast<size_t>((m_RDRAM + (address & 0x007FFFFF)) - address);
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
    snprintf(m_strLabelName, sizeof(m_strLabelName), "0x%08X", address);
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
    // Ensure RDRAM is practically a flat 8MB array so pointers above 4MB aren't dropped
    uint32_t newSize = 0x800000;
    
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

