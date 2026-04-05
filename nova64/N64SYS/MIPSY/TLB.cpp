/**
 * @file TLB.cpp
 * @brief TLB implementation.
 */
#include "TLB.hpp"
#include "Vmem.hpp"
#include "../../common/Log.hpp"
#include "../MainSYS.hpp"
#include "Register.hpp"

namespace N64System {

// CP0 Register Indices
using Reg = CRegisters::COP0Reg;
constexpr auto CP0_INDEX = Reg::Index;
constexpr auto CP0_RANDOM = Reg::Random;
constexpr auto CP0_ENTRYLO0 = Reg::EntryLo0;
constexpr auto CP0_ENTRYLO1 = Reg::EntryLo1;
constexpr auto CP0_PAGEMASK = Reg::PageMask;
constexpr auto CP0_WIRED = Reg::Wired;
constexpr auto CP0_ENTRYHI = Reg::EntryHi;

TranslationLookasideBuffer::TranslationLookasideBuffer()
    : random_generator(TLB_ENTRY_COUNT) 
{
    reset();
}

TranslationLookasideBuffer::TranslationLookasideBuffer(Nova64::MipsMemoryVM& mmu, CRegisters& registers, CRecompiler* recompiler)
    : memory_management_unit(&mmu),
      registers(&registers),
      recompiler(recompiler),
      random_generator(TLB_ENTRY_COUNT) {
    reset();
}

TranslationLookasideBuffer::~TranslationLookasideBuffer() {
}

void TranslationLookasideBuffer::Initialize(Nova64::MipsMemoryVM* mmu, CRegisters* reg, CRecompiler* recomp) {
    memory_management_unit = mmu;
    registers = reg;
    recompiler = recomp;
    reset();
}

void TranslationLookasideBuffer::reset(bool invalidate_tlb) {
    if (invalidate_tlb) {
        for (auto& entry : tlb_entries) {
            entry = TlbEntry();
            entry.defined = false;
        }
        invalidate_fast_tlb();
    }
    reset_statistics();
}

void TranslationLookasideBuffer::invalidate_fast_tlb() {
    for (auto& entry : fast_tlb_entries) {
        entry = FastTlbEntry();
        entry.valid = false;
    }
    // Clear hash table
    for (auto& vec : fast_tlb_hash) {
        vec.clear();
    }
}

void TranslationLookasideBuffer::on_cop0_status_changed() {
}

void TranslationLookasideBuffer::on_privilege_mode_changed(PrivilegeMode new_mode) {
    privilege_mode = new_mode;
    invalidate_fast_tlb();
}

void TranslationLookasideBuffer::probe() {
    if (!registers) return;
    
    // Get EntryHi values
    uint64_t entry_hi_val = registers->Cop0_MF(CP0_ENTRYHI);
    COP0EntryHi entry_hi;
    entry_hi.Value = entry_hi_val;
    
    uint32_t vpn2 = entry_hi.VPN2();
    uint8_t asid = static_cast<uint8_t>(entry_hi.ASID());
    
    bool match_found = false;
    
    for (size_t i = 0; i < tlb_entries.size(); ++i) {
        const auto& entry = tlb_entries[i];
        
        uint32_t page_mask = entry.page_mask.mask;
        uint32_t masked_vpn2 = vpn2 & ~page_mask;
        uint32_t entry_vpn2 = entry.entry_hi.vpn2 & ~page_mask;
        
        if (masked_vpn2 == entry_vpn2 && (entry.global || entry.entry_hi.asid == asid)) {
            // Match found
            registers->Cop0_MT(CP0_INDEX, static_cast<uint32_t>(i));
            match_found = true;
            break;
        }
    }
    
    if (!match_found) {
        // Set bit 31 (P) of Index register if no match
        uint32_t index_val = registers->Cop0_MF(CP0_INDEX);
        registers->Cop0_MT(CP0_INDEX, index_val | 0x80000000);
    }
}

void TranslationLookasideBuffer::read_entry() {
    if (!registers) return;
    
    uint32_t index = registers->Cop0_MF(CP0_INDEX) & 0x3F; 
    
    if (index >= TLB_ENTRY_COUNT) {
        return;
    }
    
    const auto& entry = tlb_entries[index];
    
    // Write PageMask
    COP0PageMask page_mask;
    page_mask.Mask = entry.page_mask.mask;
    registers->Cop0_MT(CP0_PAGEMASK, page_mask.Value);
    
    // Write EntryHi
    COP0EntryHi entry_hi;
    entry_hi.Raw.VPN2 = entry.entry_hi.vpn2;
    entry_hi.Raw.ASID = entry.entry_hi.asid;
    registers->Cop0_MT(CP0_ENTRYHI, entry_hi.Value);
    
    // Write EntryLo0
    COP0EntryLo entry_lo0;
    entry_lo0.PFN = entry.entry_lo0.pfn;
    entry_lo0.C = entry.entry_lo0.cache_algorithm;
    entry_lo0.D = entry.entry_lo0.dirty;
    entry_lo0.V = entry.entry_lo0.valid;
    entry_lo0.GLOBAL = entry.entry_lo0.global;
    registers->Cop0_MT(CP0_ENTRYLO0, entry_lo0.Value);
    
    // Write EntryLo1
    COP0EntryLo entry_lo1;
    entry_lo1.PFN = entry.entry_lo1.pfn;
    entry_lo1.C = entry.entry_lo1.cache_algorithm;
    entry_lo1.D = entry.entry_lo1.dirty;
    entry_lo1.V = entry.entry_lo1.valid;
    entry_lo1.GLOBAL = entry.entry_lo1.global;
    registers->Cop0_MT(CP0_ENTRYLO1, entry_lo1.Value);
}

void TranslationLookasideBuffer::write_entry(uint32_t index, bool random) {
    if (!registers) return;
    if (index >= TLB_ENTRY_COUNT) return;
    
    uint64_t pagemask_val = registers->Cop0_MF(CP0_PAGEMASK);
    uint64_t entryhi_val = registers->Cop0_MF(CP0_ENTRYHI);
    uint64_t entrylo0_val = registers->Cop0_MF(CP0_ENTRYLO0);
    uint64_t entrylo1_val = registers->Cop0_MF(CP0_ENTRYLO1);
    
    COP0PageMask pm; pm.Value = pagemask_val;
    COP0EntryHi eh; eh.Value = entryhi_val;
    COP0EntryLo el0; el0.Value = entrylo0_val;
    COP0EntryLo el1; el1.Value = entrylo1_val;
    
    auto& entry = tlb_entries[index];
    
    entry.defined = true;
    entry.random = random;
    
    entry.page_mask.mask = static_cast<uint32_t>(pm.Mask); 
    
    entry.entry_hi.vpn2 = eh.VPN2();
    entry.entry_hi.asid = static_cast<uint8_t>(eh.ASID());
    
    entry.entry_lo0.pfn = el0.PFN;
    entry.entry_lo0.cache_algorithm = static_cast<uint8_t>(el0.C);
    entry.entry_lo0.dirty = el0.D;
    entry.entry_lo0.valid = el0.V;
    entry.entry_lo0.global = el0.GLOBAL;
    
    entry.entry_lo1.pfn = el1.PFN;
    entry.entry_lo1.cache_algorithm = static_cast<uint8_t>(el1.C);
    entry.entry_lo1.dirty = el1.D;
    entry.entry_lo1.valid = el1.V;
    entry.entry_lo1.global = el1.GLOBAL;
    
    entry.global = entry.entry_lo0.global && entry.entry_lo1.global; 
    
    invalidate_fast_tlb();
}

TlbTranslationResult TranslationLookasideBuffer::translate_address(uint64_t virtual_address, bool is_write, bool is_execute) {
    TlbTranslationResult result;
    result.valid = false;
    
    uint32_t vaddr_32 = static_cast<uint32_t>(virtual_address);
    
    // KSEG0: 0x80000000 - 0x9FFFFFFF
    if (vaddr_32 >= 0x80000000 && vaddr_32 <= 0x9FFFFFFF) {
        result.physical_address = vaddr_32 - 0x80000000;
        result.segment = MemorySegment::Cached; 
        result.cached = true;
        result.valid = true;
        return result;
    }
    
    // KSEG1: 0xA0000000 - 0xBFFFFFFF
    if (vaddr_32 >= 0xA0000000 && vaddr_32 <= 0xBFFFFFFF) {
        result.physical_address = vaddr_32 - 0xA0000000;
        result.segment = MemorySegment::Direct;
        result.cached = false;
        result.valid = true;
        return result;
    }
    
    // KSEG2 / USEG -> TLB Mapped
    if (!registers) return result;

    uint8_t current_asid = static_cast<uint8_t>(registers->Cop0_MF(CP0_ENTRYHI) & 0xFF);
    
    for (const auto& entry : tlb_entries) {
        if (!entry.defined) continue;
        
        uint32_t mask = entry.page_mask.mask; // mask for VPN comparison
        uint32_t vaddr_vpn2 = (vaddr_32 >> 13) & 0x7FFFF;
        uint32_t entry_vpn2 = entry.entry_hi.vpn2;
        
        if (((vaddr_vpn2 & ~mask) == (entry_vpn2 & ~mask)) && 
            (entry.global || entry.entry_hi.asid == current_asid)) {
            
            uint32_t offset_mask = (mask << 12) | 0xFFF;
            uint32_t select_mask = (offset_mask + 1);
            
            const auto& lo = (vaddr_32 & select_mask) ? entry.entry_lo1 : entry.entry_lo0;
            
            if (!lo.valid) {
                 result.exception = TlbException::TlbInvalid;
                 return result;
            }
            
            if (is_write && !lo.dirty) {
                result.exception = TlbException::TlbModified;
                return result;
            }
            
            uint32_t pfn = lo.pfn;
            uint32_t offset = vaddr_32 & offset_mask;
            uint32_t phys_page_base = (pfn & ~mask) << 12; 
            result.physical_address = phys_page_base | offset;
            
            result.segment = MemorySegment::Mapped;
            result.cached = (lo.cache_algorithm != 2); 
            result.valid = true;
            result.dirty = lo.dirty;
            
            statistics.hits++;
            return result;
        }
    }
    
    statistics.misses++;
    result.exception = TlbException::TlbRefill; 
    return result;
}

TranslationLookasideBuffer::RandomIndexGenerator::RandomIndexGenerator(size_t max) 
    : max_index(max), wired_count(0), counter(max - 1) {}

size_t TranslationLookasideBuffer::RandomIndexGenerator::next() {
    if (counter <= wired_count) {
        counter = max_index - 1;
    }
    return counter--;
}

// ============================================================================
// Legacy CTLB Implementation
// ============================================================================

CTLB::CTLB(Nova64::MipsMemoryVM& MMU, CRegisters& Reg, CRecompiler*& Recomp) :
    m_PrivilegeMode(PrivilegeMode::Kernel),
    m_AddressSize32bit(true),
    m_MMU(&MMU),
    m_Reg(&Reg),
    m_Recomp(Recomp)
{
    Reset(true);
}

CTLB::CTLB() : 
    m_PrivilegeMode(PrivilegeMode::Kernel),
    m_AddressSize32bit(true),
    m_MMU(nullptr),
    m_Reg(nullptr),
    m_Recomp(nullptr)
{
    Reset(true);
}

CTLB::~CTLB() {
}

void CTLB::Initialize(Nova64::MipsMemoryVM* MMU, CRegisters* Reg, CRecompiler* Recomp) {
    m_MMU = MMU;
    m_Reg = Reg;
    m_Recomp = Recomp;
    Reset(true);
}

void CTLB::Reset(bool InvalidateTLB) {
    if (InvalidateTLB) {
        for(auto& entry : m_tlb) {
            entry.EntryDefined = false;
        }
        for(auto& entry : m_FastTlb) {
            entry.VALID = false;
        }
    }
}

void CTLB::Probe() { 
    if (!m_Reg) return;
    
    // Get EntryHi values
    uint64_t entry_hi_val = m_Reg->Cop0_MF(Reg::EntryHi);
    
    // Extract VPN2 and ASID
    // VPN2 is bits 39-13 (in 64-bit) or 31-13 (in 32-bit check)
    // EntryHi: [63:62 R] [61:60 00] [59:40 Fill] [39:13 VPN2] [12:8 0] [7:0 ASID]
    // We'll trust the manual extraction similar to TranslationLookasideBuffer
    uint32_t vpn2 = (entry_hi_val >> 13) & 0x7FFFF; // 19 bits
    uint8_t asid = static_cast<uint8_t>(entry_hi_val & 0xFF);
    
    bool match_found = false;
    
    for (int i = 0; i < 32; ++i) {
        const auto& entry = m_tlb[i];
        if (!entry.EntryDefined) continue;
        
        // PageMask determines which bits of VPN2 are compared
        // Mask: 0 = compare, 1 = ignore
        uint32_t mask = static_cast<uint32_t>((entry.PageMask.Value >> 13) & 0x7FFFF); // Extract mask from PageMask
        
        uint32_t masked_vpn2 = vpn2 & ~mask;
        uint32_t entry_vpn2 = (entry.EntryHi.Value >> 13) & 0x7FFFF;
        uint32_t masked_entry_vpn2 = entry_vpn2 & ~mask;
        
        bool global = (entry.EntryLo0.Value & 1) && (entry.EntryLo1.Value & 1); // G bit 0
        uint8_t entry_asid = static_cast<uint8_t>(entry.EntryHi.Value & 0xFF);
        
        if (masked_vpn2 == masked_entry_vpn2 && (global || entry_asid == asid)) {
            // Match found
            m_Reg->Cop0_MT(Reg::Index, static_cast<uint32_t>(i));
            match_found = true;
            break;
        }
    }
    
    if (!match_found) {
        // Set bit 31 (P) of Index register if no match
        uint32_t index_val = static_cast<uint32_t>(m_Reg->Cop0_MF(Reg::Index));
        m_Reg->Cop0_MT(Reg::Index, index_val | 0x80000000);
    }
}

void CTLB::ReadEntry() {
    if (!m_Reg) return;
    
    uint32_t index = static_cast<uint32_t>(m_Reg->Cop0_MF(Reg::Index)) & 0x3F; 
    
    if (index >= 32) {
        return;
    }
    
    const auto& entry = m_tlb[index];
    
    m_Reg->Cop0_MT(Reg::PageMask, entry.PageMask.Value);
    m_Reg->Cop0_MT(Reg::EntryHi, entry.EntryHi.Value);
    m_Reg->Cop0_MT(Reg::EntryLo0, entry.EntryLo0.Value);
    m_Reg->Cop0_MT(Reg::EntryLo1, entry.EntryLo1.Value);
}

void CTLB::WriteEntry(uint32_t Index, bool Random) {
    if (!m_Reg) return;
    
    uint32_t write_index = Index;
    
    if (write_index >= 32) return;
    
    uint64_t pagemask_val = m_Reg->Cop0_MF(Reg::PageMask);
    uint64_t entryhi_val = m_Reg->Cop0_MF(Reg::EntryHi);
    uint64_t entrylo0_val = m_Reg->Cop0_MF(Reg::EntryLo0);
    uint64_t entrylo1_val = m_Reg->Cop0_MF(Reg::EntryLo1);
    
    auto& entry = m_tlb[write_index];
    
    // Remove old mapping from MMU page tables before overwriting
    if (entry.EntryDefined && m_MMU) {
        m_MMU->RemoveTLBMapping(entry.EntryHi.Value, entry.PageMask.Value);
    }
    
    entry.EntryDefined = true;
    entry.PageMask.Value = pagemask_val;
    entry.EntryHi.Value = entryhi_val;
    entry.EntryLo0.Value = entrylo0_val;
    entry.EntryLo1.Value = entrylo1_val;
    
    // Propagate new mapping into MMU fast-path page tables so that
    // LW_Memory / SW_Memory can resolve USEG (TLB-mapped) addresses
    // without falling through to the slow TLB-miss exception path.
    if (m_MMU) {
        m_MMU->ApplyTLBMapping(entryhi_val, pagemask_val,
                               entrylo0_val, entrylo1_val);
    }
    
    // Invalidate fast lookup
    for(auto& fast : m_FastTlb) {
        fast.VALID = false;
    }
}

void CTLB::COP0StatusChanged() {
    // Invalidate FastTLB when status changes (e.g. KSU bits)
    for(auto& fast : m_FastTlb) {
        fast.VALID = false;
    }
}

bool CTLB::AddressDefined(uint64_t VAddr, bool& Dirty) {
    uint32_t vaddr_32 = static_cast<uint32_t>(VAddr);
    
    // KSEG0/1 are always defined
    if (vaddr_32 >= 0x80000000 && vaddr_32 <= 0xBFFFFFFF) {
        Dirty = true; // Always writable (conceptually)
        return true;
    }
    
    if (!m_Reg) return false;
    
    uint8_t current_asid = static_cast<uint8_t>(m_Reg->Cop0_MF(Reg::EntryHi) & 0xFF);
    uint32_t vpn2 = (vaddr_32 >> 13) & 0x7FFFF;
    
    for (const auto& entry : m_tlb) {
        if (!entry.EntryDefined) continue;
        
        uint32_t mask = static_cast<uint32_t>((entry.PageMask.Value >> 13) & 0x7FFFF);
        uint32_t entry_vpn2 = (entry.EntryHi.Value >> 13) & 0x7FFFF;
        
        uint32_t masked_vpn_req = vpn2 & ~mask;
        uint32_t masked_vpn_ent = entry_vpn2 & ~mask;
        
        bool global = (entry.EntryLo0.Value & 1) && (entry.EntryLo1.Value & 1);
        uint8_t entry_asid = static_cast<uint8_t>(entry.EntryHi.Value & 0xFF);
        
        if (masked_vpn_req == masked_vpn_ent && (global || entry_asid == current_asid)) {
            // Match found
            uint32_t offset_mask = (mask << 12) | 0xFFF;
            uint32_t select_mask = (offset_mask + 1);
            
            // Check LO0 vs LO1
            // Bit 12 (or highest unmasked bit below VPN2) selects Lo0/Lo1
            // If select_mask bit is set in vaddr, use Lo1 (Odd), else Lo0 (Even)
            bool odd_page = (vaddr_32 & select_mask) != 0;
            
            uint64_t lo_val = odd_page ? entry.EntryLo1.Value : entry.EntryLo0.Value;
            
            // V bit (Valid) is bit 1
            bool valid = (lo_val & 2) != 0;
            if (!valid) return false;
            
            // D bit (Dirty) is bit 2
            Dirty = (lo_val & 4) != 0;
            return true;
        }
    }
    
    return false;
}

CTLB::TLB_ENTRY& CTLB::TlbEntry(int32_t Entry) {
    if (Entry >= 0 && Entry < 32) return m_tlb[Entry];
    static TLB_ENTRY dummy;
    return dummy;
}

bool CTLB::VAddrToPAddr(uint64_t VAddr, uint32_t& PAddr, bool& MemoryUnused) {
    uint32_t vaddr_32 = static_cast<uint32_t>(VAddr);
    MemoryUnused = false;

    // KSEG0: 0x80000000 - 0x9FFFFFFF
    if (vaddr_32 >= 0x80000000 && vaddr_32 <= 0x9FFFFFFF) {
        PAddr = vaddr_32 - 0x80000000;
        return true;
    }
    
    // KSEG1: 0xA0000000 - 0xBFFFFFFF
    if (vaddr_32 >= 0xA0000000 && vaddr_32 <= 0xBFFFFFFF) {
        PAddr = vaddr_32 - 0xA0000000;
        return true;
    }
    
    if (!m_Reg) {
        MemoryUnused = true;
        return false;
    }

    uint8_t current_asid = static_cast<uint8_t>(m_Reg->Cop0_MF(Reg::EntryHi) & 0xFF);
    uint32_t vpn2 = (vaddr_32 >> 13) & 0x7FFFF;
    
    for (const auto& entry : m_tlb) {
        if (!entry.EntryDefined) continue;
        
        uint32_t mask = static_cast<uint32_t>((entry.PageMask.Value >> 13) & 0x7FFFF);
        uint32_t entry_vpn2 = (entry.EntryHi.Value >> 13) & 0x7FFFF;
        
        if (((vpn2 & ~mask) == (entry_vpn2 & ~mask)) && 
            ((entry.EntryLo0.Value & 1 && entry.EntryLo1.Value & 1) || // Global
             (static_cast<uint8_t>(entry.EntryHi.Value & 0xFF) == current_asid))) {
            
            uint32_t offset_mask = (mask << 12) | 0xFFF;
            uint32_t select_mask = (offset_mask + 1);
            
            bool odd_page = (vaddr_32 & select_mask) != 0;
            uint64_t lo_val = odd_page ? entry.EntryLo1.Value : entry.EntryLo0.Value;
            
            bool valid = (lo_val & 2) != 0;
            if (!valid) {
                 MemoryUnused = true; // Mapped but invalid
                 return false;
            }
            
            uint32_t pfn = static_cast<uint32_t>((lo_val >> 6) & 0xFFFFF);
            uint32_t offset = vaddr_32 & offset_mask;
            uint32_t phys_page_base = (pfn & ~mask) << 12;
            
            PAddr = phys_page_base | offset;
            return true;
        }
    }
    
    MemoryUnused = true;
    return false;
}

bool CTLB::PAddrToVAddr(uint32_t PAddr, uint32_t& VAddr, uint32_t& Index) {
    // Reverse lookup - mostly useful for debugger or analyzing code
    // Check KSEG0
    if (PAddr < 0x20000000) {
        VAddr = 0x80000000 + PAddr;
        return true;
    }
    
    // Check TLB
    // We can't easily reverse check all TLB entries because masking makes it n-to-1 potentially?
    // Actually we can check if PAddr falls into a page.
    
    for (int i = 0; i < 32; ++i) {
        const auto& entry = m_tlb[i];
        if (!entry.EntryDefined) continue;
        
        uint32_t mask = static_cast<uint32_t>((entry.PageMask.Value >> 13) & 0x7FFFF);
        uint32_t offset_mask = (mask << 12) | 0xFFF;
            
        // Check Lo0
        if (entry.EntryLo0.Value & 2) { // Valid
             uint32_t pfn0 = static_cast<uint32_t>((entry.EntryLo0.Value >> 6) & 0xFFFFF);
             uint32_t phys_base0 = (pfn0 & ~mask) << 12;
             // Size of page = offset_mask + 1
             uint32_t size = offset_mask + 1;
             
             if (PAddr >= phys_base0 && PAddr < phys_base0 + size) {
                 // Found in Lo0 (Even)
                 uint32_t offset = PAddr - phys_base0;
                 uint32_t vpn2 = static_cast<uint32_t>((entry.EntryHi.Value >> 13) & 0x7FFFF);
                 uint32_t vbase = (vpn2 & ~mask) << 13; // VPN2 is bit 13
                 
                 // Even page is lower part of VPN2 pair?
                 // Virtual Address Reconstruction:
                 // VAddr = (VPN2 << 13) | Offset?
                 // But VPN2 covers 2 pages.
                 // Even page corresponds to bit (select_mask) == 0.
                 
                 VAddr = vbase | offset; 
                 Index = i;
                 return true;
             }
        }
        
        // Check Lo1
        if (entry.EntryLo1.Value & 2) { // Valid
             uint32_t pfn1 = static_cast<uint32_t>((entry.EntryLo1.Value >> 6) & 0xFFFFF);
             uint32_t phys_base1 = (pfn1 & ~mask) << 12;
             uint32_t size = offset_mask + 1;
             
             if (PAddr >= phys_base1 && PAddr < phys_base1 + size) {
                 // Found in Lo1 (Odd)
                 uint32_t offset = PAddr - phys_base1;
                 uint32_t vpn2 = static_cast<uint32_t>((entry.EntryHi.Value >> 13) & 0x7FFFF);
                 uint32_t vbase = (vpn2 & ~mask) << 13;
                 
                 uint32_t select_mask = offset_mask + 1;
                 VAddr = vbase | select_mask | offset;
                 Index = i;
                 return true;
             }
        }
    }

    return false;
}

void CTLB::RecordDifference(CLog& LogFile, const CTLB& rTLB) {
    // Compare all 32 TLB entries and log differences
    // Used for sync CPU mode debugging
    for (int i = 0; i < 32; ++i) {
        const auto& e1 = m_tlb[i];
        const auto& e2 = rTLB.m_tlb[i];
        
        if (e1.EntryDefined != e2.EntryDefined) {
            Log::debugf("TLB[%d] EntryDefined: %s vs %s", i,
                       e1.EntryDefined ? "true" : "false",
                       e2.EntryDefined ? "true" : "false");
            continue;
        }
        
        if (!e1.EntryDefined) continue;
        
        if (e1.PageMask.Value != e2.PageMask.Value) {
            Log::debugf("TLB[%d] PageMask: 0x%016llX vs 0x%016llX", i,
                       e1.PageMask.Value, e2.PageMask.Value);
        }
        if (e1.EntryHi.Value != e2.EntryHi.Value) {
            Log::debugf("TLB[%d] EntryHi: 0x%016llX vs 0x%016llX", i,
                       e1.EntryHi.Value, e2.EntryHi.Value);
        }
        if (e1.EntryLo0.Value != e2.EntryLo0.Value) {
            Log::debugf("TLB[%d] EntryLo0: 0x%016llX vs 0x%016llX", i,
                       e1.EntryLo0.Value, e2.EntryLo0.Value);
        }
        if (e1.EntryLo1.Value != e2.EntryLo1.Value) {
            Log::debugf("TLB[%d] EntryLo1: 0x%016llX vs 0x%016llX", i,
                       e1.EntryLo1.Value, e2.EntryLo1.Value);
        }
    }
}

bool CTLB::operator==(const CTLB& rTLB) const {
    for (int i = 0; i < 32; ++i) {
        const auto& e1 = m_tlb[i];
        const auto& e2 = rTLB.m_tlb[i];
        
        if (e1.EntryDefined != e2.EntryDefined) return false;
        if (!e1.EntryDefined) continue;
        
        if (e1.PageMask.Value != e2.PageMask.Value) return false;
        if (e1.EntryHi.Value != e2.EntryHi.Value) return false;
        if (e1.EntryLo0.Value != e2.EntryLo0.Value) return false;
        if (e1.EntryLo1.Value != e2.EntryLo1.Value) return false;
    }
    return true;
}

bool CTLB::operator!=(const CTLB& rTLB) const {
    return !(*this == rTLB);
}

} // namespace N64System
