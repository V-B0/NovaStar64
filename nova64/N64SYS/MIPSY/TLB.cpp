/**
 * @file TLB.cpp
 * @brief MIPS R4300i TLB: probe/read/write operations, address translation and save-state I/O, in TranslationLookasideBuffer and the CTLB class.
 */
#include "TLB.hpp"
#include "../StateArchive.hpp"
#include "Vmem.hpp"
#include "../../common/log.hpp"
#include "../MainSYS.hpp"
#include "Register.hpp"

namespace N64System {

// CP0 Register Indices
using Reg = CRegisters::COP0Reg;

TranslationLookasideBuffer::TranslationLookasideBuffer() : random_generator(TLB_ENTRY_COUNT) { reset(); }

TranslationLookasideBuffer::TranslationLookasideBuffer(Nova64::MipsMemoryVM& mmu, CRegisters& registers,
                                                       CRecompiler* recompiler) :
    memory_management_unit(&mmu), registers(&registers), recompiler(recompiler), random_generator(TLB_ENTRY_COUNT) {
    reset();
}

TranslationLookasideBuffer::~TranslationLookasideBuffer() {}

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

TranslationLookasideBuffer::RandomIndexGenerator::RandomIndexGenerator(size_t max) :
    max_index(max), wired_count(0), counter(max - 1) {}

// --- Legacy CTLB Implementation ---

CTLB::CTLB(Nova64::MipsMemoryVM& MMU, CRegisters& Reg, CRecompiler*& Recomp) :
    m_PrivilegeMode(PrivilegeMode::Kernel), m_AddressSize32bit(true), m_MMU(&MMU), m_Reg(&Reg), m_Recomp(Recomp) {
    Reset(true);
}

CTLB::CTLB() :
    m_PrivilegeMode(PrivilegeMode::Kernel), m_AddressSize32bit(true), m_MMU(nullptr), m_Reg(nullptr),
    m_Recomp(nullptr) {
    Reset(true);
}

CTLB::~CTLB() {}

void CTLB::Initialize(Nova64::MipsMemoryVM* MMU, CRegisters* Reg, CRecompiler* Recomp) {
    m_MMU = MMU;
    m_Reg = Reg;
    m_Recomp = Recomp;
    Reset(true);
}

void CTLB::Reset(bool InvalidateTLB) {
    if (InvalidateTLB) {
        for (auto& entry : m_tlb) {
            entry.EntryDefined = false;
        }
        for (auto& entry : m_FastTlb) {
            entry.VALID = false;
        }
    }
}

void CTLB::Probe() {
    if (!m_Reg) return;

    uint64_t entry_hi_val = m_Reg->Cop0_MF(Reg::EntryHi);

    // Extract R, VPN2 and ASID
    uint8_t r_field = static_cast<uint8_t>((entry_hi_val >> 62) & 0x3);
    uint32_t vpn2 = (entry_hi_val >> 13) & 0x7FFFF; // 19 bits
    uint8_t asid = static_cast<uint8_t>(entry_hi_val & 0xFF);

    bool match_found = false;

    for (int i = 0; i < 32; ++i) {
        const auto& entry = m_tlb[i];
        if (!entry.EntryDefined) continue;

        // Compare R field (bits 62-63 of EntryHi)
        uint8_t entry_r = static_cast<uint8_t>((entry.EntryHi.Value >> 62) & 0x3);
        if (r_field != entry_r) continue;

        uint32_t mask = static_cast<uint32_t>((entry.PageMask.Value >> 13) & 0x7FFFF);

        uint32_t masked_vpn2 = vpn2 & ~mask;
        uint32_t entry_vpn2 = (entry.EntryHi.Value >> 13) & 0x7FFFF;
        uint32_t masked_entry_vpn2 = entry_vpn2 & ~mask;

        bool global = (entry.EntryLo0.Value & 1) && (entry.EntryLo1.Value & 1);
        uint8_t entry_asid = static_cast<uint8_t>(entry.EntryHi.Value & 0xFF);

        if (masked_vpn2 == masked_entry_vpn2 && (global || entry_asid == asid)) {
            m_Reg->Cop0_MT(Reg::Index, static_cast<uint32_t>(i));
            match_found = true;
            break;
        }
    }

    if (!match_found) {
        // No match: set P (probe) bit, clear index field
        m_Reg->Cop0_MT(Reg::Index, 0x80000000u);
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

    // TLBR: EntryHi VPN2 bits covered by PageMask are cleared
    // Also restore R field at correct position (bits 62-63)
    {
        uint64_t hi_val = entry.EntryHi.Value;
        uint32_t mask12 = static_cast<uint32_t>((entry.PageMask.Value >> 13) & 0xFFF);
        // Clear VPN2 bits that correspond to mask bits (mask bits 0-11 → VPN2 bits 0-11 → register bits 13-24)
        uint64_t vpn2_clear_mask = static_cast<uint64_t>(mask12) << 13;
        hi_val &= ~vpn2_clear_mask;
        m_Reg->Cop0_MT(Reg::EntryHi, hi_val);
    }

    // TLBR: G bit in both EntryLo registers is the AND of stored G bits
    // PFN is 20 bits (bits 6-25), bits 26-29 are cleared on readback
    bool g_combined = (entry.EntryLo0.Value & 1) && (entry.EntryLo1.Value & 1);
    uint64_t lo0 = entry.EntryLo0.Value & 0x03FFFFFFULL; // Mask to 26 bits (PFN 20 bits + lower 6 bits)
    uint64_t lo1 = entry.EntryLo1.Value & 0x03FFFFFFULL;
    lo0 = g_combined ? (lo0 | 1) : (lo0 & ~1ull);
    lo1 = g_combined ? (lo1 | 1) : (lo1 & ~1ull);
    m_Reg->Cop0_MT(Reg::EntryLo0, lo0);
    m_Reg->Cop0_MT(Reg::EntryLo1, lo1);
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
    // VR4300 normalizes PageMask: within each pair of bits, low bit is forced to match high bit
    {
        uint32_t mask_raw = static_cast<uint32_t>((pagemask_val >> 13) & 0xFFF);
        uint32_t high_bits = mask_raw & 0xAAA;
        uint32_t normalized = high_bits | (high_bits >> 1);
        entry.PageMask.Value = static_cast<uint64_t>(normalized) << 13;
    }
    entry.EntryHi.Value = entryhi_val;
    entry.EntryLo0.Value = entrylo0_val;
    entry.EntryLo1.Value = entrylo1_val;

    // Propagate new mapping into MMU fast-path page tables so that
    // LW_Memory / SW_Memory can resolve USEG (TLB-mapped) addresses
    // without falling through to the slow TLB-miss exception path.
    if (m_MMU) {
        m_MMU->ApplyTLBMapping(entryhi_val, pagemask_val, entrylo0_val, entrylo1_val);
    }

    // Invalidate fast lookup
    for (auto& fast : m_FastTlb) {
        fast.VALID = false;
    }
}

void CTLB::COP0StatusChanged() {
    // Invalidate FastTLB when status changes (e.g. KSU bits)
    for (auto& fast : m_FastTlb) {
        fast.VALID = false;
    }
}

CTLB::TlbLookupResult CTLB::AddressLookup(uint64_t VAddr, bool& Dirty) {
    uint32_t vaddr_32 = static_cast<uint32_t>(VAddr);

    // KSEG0/1 are always defined
    if (vaddr_32 >= 0x80000000 && vaddr_32 <= 0xBFFFFFFF) {
        Dirty = true;
        return TlbLookupResult::MatchValid;
    }

    if (!m_Reg) return TlbLookupResult::NoMatch;

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
            uint32_t offset_mask = (mask << 12) | 0xFFF;
            uint32_t select_mask = (offset_mask + 1);
            bool odd_page = (vaddr_32 & select_mask) != 0;
            uint64_t lo_val = odd_page ? entry.EntryLo1.Value : entry.EntryLo0.Value;

            bool valid = (lo_val & 2) != 0;
            if (!valid) return TlbLookupResult::MatchInvalid;

            Dirty = (lo_val & 4) != 0;
            return TlbLookupResult::MatchValid;
        }
    }

    return TlbLookupResult::NoMatch;
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

bool CTLB::operator!=(const CTLB& rTLB) const { return !(*this == rTLB); }

void CTLB::SerializeState(Nova64::StateArchive& ar) {
    ar.Section("TLB ");
    // The MMU's page tables are built from the entries: unmap the old entries
    // before loading and map the loaded ones afterwards.
    const bool loading = ar.IsLoading();
    if (loading && m_MMU) {
        for (const auto& entry : m_tlb) {
            if (entry.EntryDefined) m_MMU->RemoveTLBMapping(entry.EntryHi.Value, entry.PageMask.Value);
        }
    }
    ar.Value(m_tlb);
    ar.Value(m_FastTlb);
    ar.Value(m_PrivilegeMode);
    ar.Value(m_AddressSize32bit);
    if (loading && m_MMU) {
        for (const auto& entry : m_tlb) {
            if (entry.EntryDefined) {
                m_MMU->ApplyTLBMapping(entry.EntryHi.Value, entry.PageMask.Value, entry.EntryLo0.Value,
                                       entry.EntryLo1.Value);
            }
        }
    }
}

} // namespace N64System
