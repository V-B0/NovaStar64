/**
 * @file TLB.hpp
 * @brief TLB declarations and types.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include <memory>
#include <string>
#include <optional>
#include <functional>
#include "../../common/Log.hpp"
#include "Register.hpp"

// Global Forward declarations
class CRegisters;
class CRecompiler;
class CGameSettings;
namespace Nova64 { class MipsMemoryVM; }

namespace N64System {

// Forward declarations
using CMipsMemoryVM = ::Nova64::MipsMemoryVM;

// Import global classes
using ::CRegisters;
using ::CRecompiler;
using ::CGameSettings;

// Memory access permissions
enum class MemoryAccessPermission : uint8_t {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    Execute = 1 << 2,
    User = 1 << 3,
    Supervisor = 1 << 4,
    Kernel = 1 << 5
};

inline MemoryAccessPermission operator|(MemoryAccessPermission a, MemoryAccessPermission b) {
    return static_cast<MemoryAccessPermission>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool operator&(MemoryAccessPermission a, MemoryAccessPermission b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

// Memory segments for address translation
enum class MemorySegment : uint8_t {
    Mapped,      // TLB mapped addresses
    Cached,      // Cached addresses (64-bit)
    Cached32,    // Cached addresses (32-bit)
    Direct,      // Direct mapped addresses (64-bit)
    Direct32,    // Direct mapped addresses (32-bit)
    Unused,      // Unused/Invalid addresses
    Kernel,      // Kernel segment (0x80000000-0xBFFFFFFF)
    Supervisor,  // Supervisor segment (0xC0000000-0xDFFFFFFF)
    User         // User segment (0x00000000-0x7FFFFFFF)
};

// MIPS privilege modes
enum class PrivilegeMode : uint8_t {
    Kernel,
    Supervisor,
    User
};

// TLB entry structure for the MIPS R4300
struct TlbEntry {
    bool defined = false;              // Whether this entry is defined
    bool global = false;               // Global bit (G)
    bool valid = false;                // Valid bit (V)
    bool dirty = false;                // Dirty bit (D)
    bool random = false;               // Whether this is a random entry
    bool probed = false;               // Whether this entry was probed
    
    // TLB fields
    struct PageMask {
        uint32_t mask = 0;             // Page mask (4KB - 16MB pages)
        uint32_t size() const { return (mask + 1) << 12; }  // Page size in bytes
    } page_mask;
    
    struct EntryHi {
        uint32_t vpn2 = 0;             // Virtual Page Number / 2
        uint8_t asid = 0;              // Address Space ID
        uint32_t get_vpn() const { return vpn2 << 13; }
    } entry_hi;
    
    struct EntryLo {
        uint32_t pfn = 0;              // Physical Frame Number
        bool global = false;           // Global bit (G)
        bool valid = false;            // Valid bit (V)
        bool dirty = false;            // Dirty bit (D)
        uint8_t cache_algorithm = 0;   // Cache coherency algorithm (C)
        uint8_t uncached = 0;          // Uncached bits (UC)
        
        bool is_valid() const { return valid; }
        uint32_t get_physical_address() const { return pfn << 12; }
    } entry_lo0, entry_lo1;
    
    // Helper methods
    bool is_global() const { return entry_lo0.global && entry_lo1.global; }
    bool is_valid_for_mode(PrivilegeMode mode) const;
    bool matches_virtual_address(uint64_t vaddr, uint8_t asid) const;
    uint32_t get_physical_address(uint64_t vaddr) const;
    
    // Range calculations
    uint64_t virtual_start() const;
    uint64_t virtual_end() const;
    uint64_t physical_start0() const;
    uint64_t physical_end0() const;
    uint64_t physical_start1() const;
    uint64_t physical_end1() const;
    
    // String representation for debugging
    std::string to_string() const;
};

// Fast TLB entry for quick address translation
struct FastTlbEntry {
    uint32_t virtual_start = 0;        // Start of virtual address range
    uint32_t virtual_end = 0;          // End of virtual address range
    uint32_t physical_start = 0;       // Start of physical address range
    uint32_t physical_end = 0;         // End of physical address range
    uint32_t length = 0;               // Length of the mapping
    uint8_t region = 0;                // Memory region
    bool valid = false;                // Whether this entry is valid
    bool dirty = false;                // Dirty flag
    bool global = false;               // Global flag
    bool valid_entry = false;          // Valid TLB entry
    bool random = false;               // Random flag
    bool probed = false;               // Probed flag
    
    // Check if virtual address is in range
    bool contains_virtual_address(uint32_t vaddr) const {
        return vaddr >= virtual_start && vaddr <= virtual_end;
    }
    
    // Translate virtual address to physical
    std::optional<uint32_t> translate(uint32_t vaddr) const {
        if (!contains_virtual_address(vaddr)) {
            return std::nullopt;
        }
        return physical_start + (vaddr - virtual_start);
    }
};

// TLB exception types
enum class TlbException : uint8_t {
    None,
    TlbMiss,           // TLB miss (no matching entry)
    TlbInvalid,        // TLB entry invalid (V=0)
    TlbModified,       // TLB modified (D=0 on write)
    TlbRefill,         // TLB refill
    AddressError,      // Address error (misaligned or out of range)
    BusError           // Bus error
};

// TLB translation result
struct TlbTranslationResult {
    uint32_t physical_address = 0;
    MemorySegment segment = MemorySegment::Unused;
    TlbException exception = TlbException::None;
    bool dirty = false;
    bool cached = false;
    bool valid = false;
    
    explicit operator bool() const { return valid && exception == TlbException::None; }
    std::string exception_string() const;
};

// Main TLB controller class
class TranslationLookasideBuffer {
public:
    static constexpr size_t TLB_ENTRY_COUNT = 32;      // MIPS R4300 has 32 TLB entries
    static constexpr size_t FAST_TLB_ENTRY_COUNT = 64; // Fast TLB cache entries
    
    // Constructor/Destructor
    TranslationLookasideBuffer(Nova64::MipsMemoryVM& mmu, CRegisters& registers, CRecompiler* recompiler);
    TranslationLookasideBuffer(); // Default constructor for MainSYS
    ~TranslationLookasideBuffer();
    
    using CTLB = TranslationLookasideBuffer;

    void Initialize(Nova64::MipsMemoryVM* mmu, CRegisters* registers, CRecompiler* recompiler);

    // No copying
    TranslationLookasideBuffer(const TranslationLookasideBuffer&) = delete;
    TranslationLookasideBuffer& operator=(const TranslationLookasideBuffer&) = delete;
    
    // Allow moving
    TranslationLookasideBuffer(TranslationLookasideBuffer&&) noexcept = default;
    TranslationLookasideBuffer& operator=(TranslationLookasideBuffer&&) noexcept = default;
    
    // Core TLB operations
    void reset(bool invalidate_tlb = true);
    void probe();                                      // TLB probe instruction (TLBP)
    void read_entry();                                 // Read TLB entry (TLBR)
    void write_entry(uint32_t index, bool random);     // Write TLB entry (TLBWI/TLBWR)
    
    // Address translation
    TlbTranslationResult translate_address(uint64_t virtual_address, 
                                          bool is_write = false,
                                          bool is_execute = false);
    
    // TLB query operations
    bool is_address_defined(uint64_t virtual_address, bool& dirty) const;
    const TlbEntry& get_tlb_entry(size_t index) const { return tlb_entries.at(index); }
    TlbEntry& get_tlb_entry(size_t index) { return tlb_entries.at(index); }
    
    // Address conversion
    std::optional<uint32_t> virtual_to_physical(uint64_t virtual_address, bool& memory_unused);
    std::optional<uint32_t> physical_to_virtual(uint32_t physical_address, uint32_t& tlb_index);
    
    // System state changes
    void on_cop0_status_changed();
    void on_privilege_mode_changed(PrivilegeMode new_mode);
    
    // Statistics and debugging
    struct TlbStatistics {
        size_t hits = 0;
        size_t misses = 0;
        size_t exceptions = 0;
        size_t refills = 0;
        size_t modifications = 0;
        
        void reset() { *this = TlbStatistics(); }
        std::string to_string() const;
    };
    
    TlbStatistics get_statistics() const { return statistics; }
    void reset_statistics() { statistics.reset(); }
    
    // Debug information
    std::string dump_tlb_state() const;
    std::string dump_fast_tlb_state() const;
    void validate_tlb_consistency() const;
    
    // Save state support
    struct SaveState {
        std::array<TlbEntry, TLB_ENTRY_COUNT> tlb_entries;
        std::array<FastTlbEntry, FAST_TLB_ENTRY_COUNT> fast_tlb_entries;
        PrivilegeMode privilege_mode;
        bool address_size_32bit;
    };
    
    SaveState save_state() const;
    void load_state(const SaveState& state);
    
    // Performance optimization
    void rebuild_fast_tlb();
    void invalidate_fast_tlb();
    void invalidate_fast_tlb_entry(size_t index);
    
private:
    // Internal helper methods
    void setup_tlb_entry(size_t index, bool random);
    void unmap_tlb_range(uint32_t virtual_address, uint32_t length);
    MemorySegment get_memory_segment(uint64_t virtual_address) const;
    void handle_tlb_exception(TlbException exception, uint64_t virtual_address);
    
    // Fast TLB lookup
    std::optional<FastTlbEntry*> find_fast_tlb_entry(uint64_t virtual_address);
    void add_fast_tlb_entry(const TlbEntry& tlb_entry, size_t tlb_index);
    void remove_fast_tlb_entry(size_t fast_tlb_index);
    
    // TLB arrays
    std::array<TlbEntry, TLB_ENTRY_COUNT> tlb_entries;
    std::array<FastTlbEntry, FAST_TLB_ENTRY_COUNT> fast_tlb_entries;
    
    // System state
    PrivilegeMode privilege_mode = PrivilegeMode::Kernel;
    bool address_size_32bit = true;  // N64 uses 32-bit addressing in user mode
    
    // Statistics
    TlbStatistics statistics;
    
    // System dependencies
    Nova64::MipsMemoryVM* memory_management_unit = nullptr;
    CRegisters* registers = nullptr;
    CRecompiler* recompiler = nullptr;
    
    // Fast TLR hash table for quicker lookups
    static constexpr size_t FAST_TLB_HASH_SIZE = 256;
    std::array<std::vector<size_t>, FAST_TLB_HASH_SIZE> fast_tlb_hash;
    
    size_t hash_virtual_address(uint64_t vaddr) const {
        return (vaddr >> 12) % FAST_TLB_HASH_SIZE;  // Hash by page number
    }
    
    // Random number generator for TLBWR (Write Random)
    class RandomIndexGenerator {
    public:
        RandomIndexGenerator(size_t max_index);
        size_t next();
        void set_wired(size_t wired);
        
    private:
        size_t max_index;
        size_t wired_count;
        size_t counter;
    };
    
    RandomIndexGenerator random_generator{TLB_ENTRY_COUNT};
};

// Legacy CTLB class (for compatibility)
class CTLB {
public:
    struct TLB_ENTRY {
        bool EntryDefined;
        
        // These would be the actual COP0 types from the original code
        // For now, we'll use placeholder types
        struct COP0PageMask { uint64_t Value; } PageMask;
        struct COP0EntryHi { uint64_t Value; } EntryHi;
        // struct COP0EntryLo { uint64_t Value; }; // Use global definition if available, or define local
        struct LocalCOP0EntryLo { uint64_t Value; };
        LocalCOP0EntryLo EntryLo0;
        LocalCOP0EntryLo EntryLo1;
    };

    struct FASTTLB {
        uint32_t VSTART;
        uint32_t VEND;
        uint32_t PHYSSTART;
        uint32_t PHYSEND;
        uint32_t Length;
        uint8_t Region;
        bool VALID;
        bool DIRTY;
        bool GLOBAL;
        bool ValidEntry;
        bool Random;
        bool Probed;
    };

    CTLB(Nova64::MipsMemoryVM& MMU, CRegisters& Reg, CRecompiler*& Recomp);
    CTLB(); // Public default constructor
    ~CTLB();

    void Initialize(Nova64::MipsMemoryVM* MMU, CRegisters* Reg, CRecompiler* Recomp);

    void Reset(bool InvalidateTLB);
    void Probe();
    void ReadEntry();
    void WriteEntry(uint32_t Index, bool Random);
    void COP0StatusChanged();
    
    bool AddressDefined(uint64_t VAddr, bool& Dirty);
    TLB_ENTRY& TlbEntry(int32_t Entry);
    
    bool VAddrToPAddr(uint64_t VAddr, uint32_t& PAddr, bool& MemoryUnused);
    bool PAddrToVAddr(uint32_t PAddr, uint32_t& VAddr, uint32_t& Index);
    
    // Debug methods
    void RecordDifference(class CLog& LogFile, const CTLB& rTLB);

    bool operator==(const CTLB& rTLB) const;
    bool operator!=(const CTLB& rTLB) const;

    CRegisters& GetRegisters() const { return *m_Reg; }

private:
    // CTLB();  // Removed conflicting private default constructor
    CTLB(const CTLB&);
    CTLB& operator=(const CTLB&);

    void SetupTLB_Entry(uint32_t Index, bool Random);
    void TLB_Unmaped(uint32_t VAddr, uint32_t Len);
    MemorySegment VAddrMemorySegment(uint64_t VAddr);

    PrivilegeMode m_PrivilegeMode;
    bool m_AddressSize32bit;
    Nova64::MipsMemoryVM* m_MMU;
    CRegisters* m_Reg;
    CRecompiler* m_Recomp;
    TLB_ENTRY m_tlb[32];
    FASTTLB m_FastTlb[64];
};

// TLB utility functions
namespace TlbUtils {
    // Page size calculations
    constexpr uint32_t get_page_size(uint32_t page_mask) {
        return (page_mask + 1) << 12;  // 4KB * (mask + 1)
    }
    
    constexpr uint32_t get_page_mask_from_size(uint32_t size) {
        return (size >> 12) - 1;
    }
    
    // VPN/PPN extraction
    constexpr uint32_t extract_vpn(uint64_t address) {
        return (address >> 12) & 0xFFFFF;  // 20-bit VPN
    }
    
    constexpr uint32_t extract_vpn2(uint64_t address) {
        return (address >> 13) & 0x7FFFF;  // 19-bit VPN2
    }
    
    constexpr uint32_t extract_offset(uint64_t address) {
        return address & 0xFFF;  // 12-bit offset
    }
    
    constexpr uint32_t extract_asid(uint64_t entry_hi) {
        return entry_hi & 0xFF;  // 8-bit ASID
    }
    
    // Address alignment checks
    constexpr bool is_page_aligned(uint64_t address, uint32_t page_size) {
        return (address & (page_size - 1)) == 0;
    }
    
    // TLB matching logic
    bool tlb_matches(uint64_t entry_hi, uint64_t virtual_address, uint8_t asid);
    
    // Debug formatting
    std::string format_address(uint64_t address);
    std::string format_tlb_entry(const TlbEntry& entry);
}

// Global TLB manager (optional)
class TlbManager {
public:
    static TlbManager& instance();
    
    void register_tlb(TranslationLookasideBuffer* tlb);
    void unregister_tlb(TranslationLookasideBuffer* tlb);
    
    // Global TLB operations
    void invalidate_all_tlbs();
    void update_all_tlbs_for_mode_change(PrivilegeMode new_mode);
    
private:
    TlbManager() = default;
    ~TlbManager() = default;
    
    TlbManager(const TlbManager&) = delete;
    TlbManager& operator=(const TlbManager&) = delete;
    
    std::vector<TranslationLookasideBuffer*> registered_tlbs;
};

} // namespace N64System

// Global TLB access
namespace GlobalTlb {
    // Get the main TLB instance
    N64System::TranslationLookasideBuffer& get_main_tlb();
    
    // Create a new TLB instance
    std::unique_ptr<N64System::TranslationLookasideBuffer> create_tlb(
        Nova64::MipsMemoryVM& mmu,
        N64System::CRegisters& registers,
        N64System::CRecompiler* recompiler);
}