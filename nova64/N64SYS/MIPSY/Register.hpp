/**
 * @file Register.hpp
 * @brief Register declarations and types.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */
#pragma once

#include "../../common/log.hpp"
#include "../Memoryhandler/CachedRom.hpp"
#include "../Memoryhandler/DisplayCtrlReg.hpp"
#include "../Memoryhandler/MIPSYinterfacehandler.hpp"
#include "../Memoryhandler/PIInterFace.hpp"
#include "../Memoryhandler/RDRAMInterFaceHandler.hpp"
#include "../Memoryhandler/RDRAMRegHandler.hpp"
#include "../Memoryhandler/SPregistersHandler.hpp"
#include "../Memoryhandler/SIInterfaceHandler.hpp"
#include "../Memoryhandler/VideoInterfaceHandler.hpp"
#include "../Types.hpp"
#include <cstdint>
#include <array>

// Avoid conflict with SysGlobals.hpp union definition
using MipsRegisterType = uint64_t;

namespace Nova64 {
    namespace Interpreter {
        class R4300iInterpreter;
    }
}


#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201)
#endif


union COP0EntryLo {
    uint64_t Value;
    
    struct {
        // Lower 32 bits (standard MIPS layout)
        uint32_t GLOBAL : 1;   // Bit 0
        uint32_t V : 1;        // Bit 1 (Valid)
        uint32_t D : 1;        // Bit 2 (Dirty)
        uint32_t C : 3;        // Bits 3-5 (Cache algorithm)
        uint32_t PFN : 20;     // Bits 6-25 (Physical Frame Number)
        uint32_t : 6;          // Bits 26-31 (Reserved)
        
        // Upper 32 bits (64-bit mode only, not used in N64 32-bit mode)
        uint32_t : 32;         // Bits 32-63 (Reserved)
    };
    
    // Constructor for initialization
    COP0EntryLo() : Value(0) {}
};


union COP0PageMask {
    uint64_t Value;
    
    struct {
        uint64_t : 13;         // Bits 0-12: Reserved
        uint64_t Mask : 12;    // Bits 13-24: Page mask (should be 12 bits)
        uint64_t : 39;         // Bits 25-63: Reserved
    };
    
    COP0PageMask() : Value(0) {}
};


union COP0EntryHi {
    uint64_t Value;
    
    // Accessor methods
    uint32_t ASID() const { return static_cast<uint32_t>(Value & 0xFF); }
    uint32_t VPN2() const { return static_cast<uint32_t>((Value >> 13) & 0x7FFFFFF); } // Fixed: 27 bits, not 32
    uint32_t FILL() const { return static_cast<uint32_t>((Value >> 32) & 0x3FFFF); }   // Fixed: 18 bits
    uint32_t R() const { return static_cast<uint32_t>((Value >> 62) & 0x3); }          // Fixed: bits 62-63
    
    void SetFromAddress(uint64_t address) {
        // Preserve ASID (bits [7:0]), set VPN2+R+Fill from faulting address
        // VPN2 occupies bits [39:13] — same position in both address and EntryHi
        Value = (Value & 0xFF) | (address & 0xFFFFFFFFFFFFE000ULL);
    }
    
    struct {
        uint64_t ASID : 8;    // Bits 0-7: Address Space ID
        uint64_t : 5;         // Bits 8-12: Reserved
        uint64_t VPN2 : 27;   // Bits 13-39: Virtual Page Number (27 bits, not 32)
        uint64_t FILL : 18;   // Bits 40-57: Fill bits (implementation dependent)
        uint64_t R : 2;       // Bits 58-59: Region bits (should be bits 62-63)
        uint64_t : 4;         // Bits 60-63: Should be R bits in correct position
    } Raw;
    
    COP0EntryHi() : Value(0) {}
};


enum PrivilegeMode : unsigned int {
    PrivilegeMode_Kernel = 0,
    PrivilegeMode_Supervisor = 1,
    PrivilegeMode_User = 2
};


union COP0Status {
    uint64_t Value;
    
    struct {
        unsigned InterruptEnable : 1;      // IE bit
        unsigned ExceptionLevel : 1;       // EXL bit
        unsigned ErrorLevel : 1;           // ERL bit
        unsigned PrevMode : 2;             // Previous mode (KSU) - uses PrivilegeMode values
        unsigned UserEXAddr : 1;           // UX bit (User Extended Addressing)
        unsigned SuperVisorEXAddr : 1;     // SX bit
        unsigned KernelEXAddr : 1;         // KX bit
        unsigned InterruptMask : 8;        // IM bits 7-0
        unsigned DE : 1;                   // Diagnostic Enable
        unsigned CE : 1;                   // Cache Error
        unsigned CH : 1;                   // Cache Hit
        unsigned NMI : 1;                  // Non-Maskable Interrupt (not NHI)
        unsigned SR : 1;                   // Soft Reset
        unsigned TS : 1;                   // TLB Shutdown
        unsigned BEV : 1;                  // Bootstrap Exception Vector
        unsigned : 1;                      // Reserved
        unsigned : 1;                      // Reserved
        unsigned RE : 1;                   // Reverse Endian
        unsigned FR : 1;                   // Floating Point Register mode
        unsigned RP : 1;                   // Reduced Power
        unsigned CU0 : 1;                  // Coprocessor 0 Usable
        unsigned CU1 : 1;                  // Coprocessor 1 Usable
        unsigned CU2 : 1;                  // Coprocessor 2 Usable
        unsigned CU3 : 1;                  // Coprocessor 3 Usable
    };
    
    COP0Status() : Value(0) {}
};


union COP0Cause {
    uint64_t Value;
    
    struct {
        unsigned : 2;                     // Bits 0-1: Reserved
        unsigned ExceptionCode : 5;       // Bits 2-6: Exception Code
        unsigned : 1;                     // Bit 7: Reserved
        unsigned PendingInterrupts : 8;   // Bits 8-15: IP bits
        unsigned : 12;                    // Bits 16-27: Reserved
        unsigned CoprocessorUnitNumber : 2; // Bits 28-29: CE bits
        unsigned : 1;                     // Bit 30: Reserved
        unsigned BranchDelay : 1;         // Bit 31: BD bit
        uint32_t : 32;                    // Bits 32-63: Upper 32 bits (all 0 in 32-bit mode)
    };
    
    COP0Cause() : Value(0) {}
};


union COP0Context {
    uint64_t Value;
    
    struct {
        uint64_t : 4;                     // Bits 0-3: Reserved
        uint64_t BadVPN2 : 19;            // Bits 4-22: Bad Virtual Page Number
        uint64_t PTEBase : 41;            // Bits 23-63: Page Table Entry Base
    };
    
    COP0Context() : Value(0) {}
};


union COP0XContext {
    uint64_t Value;
    
    struct {
        uint64_t : 4;                     // Bits 0-3: Reserved
        uint64_t BadVPN2 : 27;            // Bits 4-30: Bad Virtual Page Number (extended)
        uint64_t R : 2;                   // Bits 31-32: Region bits
        uint64_t PTEBase : 31;            // Bits 33-63: Page Table Entry Base
    };
    
    COP0XContext() : Value(0) {}
};


enum FPRoundingMode : unsigned {
    FPRoundingMode_RoundToNearest = 0,
    FPRoundingMode_RoundTowardZero = 1,
    FPRoundingMode_RoundTowardPlusInfinity = 2,
    FPRoundingMode_RoundTowardMinusInfinity = 3,
};

union FPStatusReg {
    uint32_t Value;
    
    struct {
        unsigned RoundingMode : 2;         // Uses FPRoundingMode values
        unsigned : 22;
        unsigned FlushSubnormals : 1;
        unsigned : 7;
    };
    
    struct {
        unsigned : 2;
        unsigned Inexact : 1;
        unsigned Underflow : 1;
        unsigned Overflow : 1;
        unsigned DivisionByZero : 1;
        unsigned InvalidOperation : 1;
        unsigned : 25;
    } Flags;
    
    struct {
        unsigned : 7;
        unsigned Inexact : 1;
        unsigned Underflow : 1;
        unsigned Overflow : 1;
        unsigned DivisionByZero : 1;
        unsigned InvalidOperation : 1;
        unsigned : 20;
    } Enable;
    
    struct {
        unsigned : 12;
        unsigned Inexact : 1;
        unsigned Underflow : 1;
        unsigned Overflow : 1;
        unsigned DivisionByZero : 1;
        unsigned InvalidOperation : 1;
        unsigned UnimplementedOperation : 1;
        unsigned : 14;
    } Cause;
    
    FPStatusReg() : Value(0) {}
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif


class CP0registers {
    friend class Nova64::Interpreter::R4300iInterpreter;
    friend class CN64System;
protected:
    std::array<uint64_t, 32> m_CP0;

    CP0registers() :
        m_CP0(),
        INDEX_REGISTER(m_CP0[0]),
        RANDOM_REGISTER(m_CP0[1]),
        ENTRYLO0_REGISTER(*reinterpret_cast<COP0EntryLo*>(&m_CP0[2])),
        ENTRYLO1_REGISTER(*reinterpret_cast<COP0EntryLo*>(&m_CP0[3])),
        CONTEXT_REGISTER(*reinterpret_cast<COP0Context*>(&m_CP0[4])),
        PAGE_MASK_REGISTER(*reinterpret_cast<COP0PageMask*>(&m_CP0[5])),
        WIRED_REGISTER(m_CP0[6]),
        BAD_VADDR_REGISTER(m_CP0[8]),
        COUNT_REGISTER(m_CP0[9]),
        ENTRYHI_REGISTER(*reinterpret_cast<COP0EntryHi*>(&m_CP0[10])),
        COMPARE_REGISTER(m_CP0[11]),
        STATUS_REGISTER(*reinterpret_cast<COP0Status*>(&m_CP0[12])),
        CAUSE_REGISTER(*reinterpret_cast<COP0Cause*>(&m_CP0[13])),
        EPC_REGISTER(m_CP0[14]),
        PREVID_REGISTER(m_CP0[15]),
        CONFIG_REGISTER(m_CP0[16]),
        XCONTEXT_REGISTER(*reinterpret_cast<COP0XContext*>(&m_CP0[20])),
        TAGLO_REGISTER(m_CP0[28]),
        TAGHI_REGISTER(m_CP0[29]),
        ERROREPC_REGISTER(m_CP0[30]) {
    }

public:
    
    uint64_t& INDEX_REGISTER;
    uint64_t& RANDOM_REGISTER;
    COP0EntryLo& ENTRYLO0_REGISTER;
    COP0EntryLo& ENTRYLO1_REGISTER;
    COP0Context& CONTEXT_REGISTER;
    COP0PageMask& PAGE_MASK_REGISTER;
    uint64_t& WIRED_REGISTER;
    uint64_t& BAD_VADDR_REGISTER;
    uint64_t& COUNT_REGISTER;
    COP0EntryHi& ENTRYHI_REGISTER;
    uint64_t& COMPARE_REGISTER;
    COP0Status& STATUS_REGISTER;
    COP0Cause& CAUSE_REGISTER;
    uint64_t& EPC_REGISTER;
    uint64_t& PREVID_REGISTER;
    uint64_t& CONFIG_REGISTER;
    COP0XContext& XCONTEXT_REGISTER;
    uint64_t& TAGLO_REGISTER;
    uint64_t& TAGHI_REGISTER;
    uint64_t& ERROREPC_REGISTER;

private:
    // Disable copying, assignment
    CP0registers(const CP0registers&) = delete;
    CP0registers& operator=(const CP0registers&) = delete;
};


namespace COP0Flags {
    // Status register flags
    constexpr uint32_t STATUS_IE = 0x00000001;
    constexpr uint32_t STATUS_EXL = 0x00000002;
    constexpr uint32_t STATUS_ERL = 0x00000004;
    constexpr uint32_t STATUS_IP0 = 0x00000100;
    constexpr uint32_t STATUS_IP1 = 0x00000200;
    constexpr uint32_t STATUS_IP2 = 0x00000400;
    constexpr uint32_t STATUS_IP3 = 0x00000800;
    constexpr uint32_t STATUS_IP4 = 0x00001000;
    constexpr uint32_t STATUS_IP5 = 0x00002000;
    constexpr uint32_t STATUS_IP6 = 0x00004000;
    constexpr uint32_t STATUS_IP7 = 0x00008000;
    constexpr uint32_t STATUS_BEV = 0x00400000;
    constexpr uint32_t STATUS_FR = 0x04000000;
    constexpr uint32_t STATUS_CU0 = 0x10000000;
    constexpr uint32_t STATUS_CU1 = 0x20000000;
    constexpr uint32_t STATUS_CU2 = 0x40000000;
    constexpr uint32_t STATUS_CU3 = 0x80000000;

    // Cause flags
    constexpr uint32_t CAUSE_IP0 = 0x1;
    constexpr uint32_t CAUSE_IP1 = 0x2;
    constexpr uint32_t CAUSE_IP2 = 0x4;
    constexpr uint32_t CAUSE_IP3 = 0x8;
    constexpr uint32_t CAUSE_IP4 = 0x10;
    constexpr uint32_t CAUSE_IP5 = 0x20;
    constexpr uint32_t CAUSE_IP6 = 0x40;
    constexpr uint32_t CAUSE_IP7 = 0x80;
}


enum class ExceptionCode : uint8_t {
    EXC_INT = 0,     // Interrupt
    EXC_MOD = 1,     // TLB mod
    EXC_RMISS = 2,   // Read TLB miss
    EXC_WMISS = 3,   // Write TLB miss
    EXC_RADE = 4,    // Read address error
    EXC_WADE = 5,    // Write address error
    EXC_IBE = 6,     // Instruction bus error
    EXC_DBE = 7,     // Data bus error
    EXC_SYSCALL = 8, // Syscall
    EXC_BREAK = 9,   // Breakpoint
    EXC_II = 10,     // Illegal instruction
    EXC_CPU = 11,    // Co-processor unusable
    EXC_OV = 12,     // Overflow
    EXC_TRAP = 13,   // Trap exception
    EXC_VCEI = 14,   // Virtual coherency on instruction fetch
    EXC_FPE = 15,    // Floating point exception
    EXC_WATCH = 23,  // Watchpoint reference
    EXC_VCED = 31,   // Virtual coherency on data read
};


namespace FPCSRFlags {
    constexpr uint32_t FS = 0x01000000;      // Flush denormalization to zero
    constexpr uint32_t C = 0x00800000;       // Condition bit
    constexpr uint32_t CE = 0x00020000;      // Cause: unimplemented operation
    constexpr uint32_t CV = 0x00010000;      // Cause: invalid operation
    constexpr uint32_t CZ = 0x00008000;      // Cause: division by zero
    constexpr uint32_t CO = 0x00004000;      // Cause: overflow
    constexpr uint32_t CU = 0x00002000;      // Cause: underflow
    constexpr uint32_t CI = 0x00001000;      // Cause: inexact operation
    constexpr uint32_t EV = 0x00000800;      // Enable: invalid operation
    constexpr uint32_t EZ = 0x00000400;      // Enable: division by zero
    constexpr uint32_t EO = 0x00000200;      // Enable: overflow
    constexpr uint32_t EU = 0x00000100;      // Enable: underflow
    constexpr uint32_t EI = 0x00000080;      // Enable: inexact operation
    constexpr uint32_t FV = 0x00000040;      // Flag: invalid operation
    constexpr uint32_t FZ = 0x00000020;      // Flag: division by zero
    constexpr uint32_t FO = 0x00000010;      // Flag: overflow
    constexpr uint32_t FU = 0x00000008;      // Flag: underflow
    constexpr uint32_t FI = 0x00000004;      // Flag: inexact operation
    constexpr uint32_t RM_MASK = 0x00000003; // Rounding mode mask
    constexpr uint32_t RM_RN = 0x00000000;   // Round to nearest
    constexpr uint32_t RM_RZ = 0x00000001;   // Round to zero
    constexpr uint32_t RM_RP = 0x00000002;   // Round to positive infinity
    constexpr uint32_t RM_RM = 0x00000003;   // Round to negative infinity
}


namespace MIFlags {
    constexpr uint32_t MODE_INIT = 0x0080;
    constexpr uint32_t MODE_EBUS = 0x0100;
    constexpr uint32_t MODE_RDRAM = 0x0200;
    
    constexpr uint32_t CLR_INIT = 0x0080;
    constexpr uint32_t SET_INIT = 0x0100;
    constexpr uint32_t CLR_EBUS = 0x0200;
    constexpr uint32_t SET_EBUS = 0x0400;
    constexpr uint32_t CLR_DP_INTR = 0x0800;
    constexpr uint32_t CLR_RDRAM = 0x1000;
    constexpr uint32_t SET_RDRAM = 0x2000;
    
    // Interrupt mask flags
    constexpr uint32_t INTR_MASK_CLR_SP = 0x0001;
    constexpr uint32_t INTR_MASK_SET_SP = 0x0002;
    constexpr uint32_t INTR_MASK_CLR_SI = 0x0004;
    constexpr uint32_t INTR_MASK_SET_SI = 0x0008;
    constexpr uint32_t INTR_MASK_CLR_AI = 0x0010;
    constexpr uint32_t INTR_MASK_SET_AI = 0x0020;
    constexpr uint32_t INTR_MASK_CLR_VI = 0x0040;
    constexpr uint32_t INTR_MASK_SET_VI = 0x0080;
    constexpr uint32_t INTR_MASK_CLR_PI = 0x0100;
    constexpr uint32_t INTR_MASK_SET_PI = 0x0200;
    constexpr uint32_t INTR_MASK_CLR_DP = 0x0400;
    constexpr uint32_t INTR_MASK_SET_DP = 0x0800;
    
    constexpr uint32_t INTR_MASK_SP = 0x01;
    constexpr uint32_t INTR_MASK_SI = 0x02;
    constexpr uint32_t INTR_MASK_AI = 0x04;
    constexpr uint32_t INTR_MASK_VI = 0x08;
    constexpr uint32_t INTR_MASK_PI = 0x10;
    constexpr uint32_t INTR_MASK_DP = 0x20;
    
    constexpr uint32_t INTR_SP = 0x01;
    constexpr uint32_t INTR_SI = 0x02;
    constexpr uint32_t INTR_AI = 0x04;
    constexpr uint32_t INTR_VI = 0x08;
    constexpr uint32_t INTR_PI = 0x10;
    constexpr uint32_t INTR_DP = 0x20;
}


namespace SPFlags {
    constexpr uint32_t CLR_HALT = 0x00001;
    constexpr uint32_t SET_HALT = 0x00002;
    constexpr uint32_t CLR_BROKE = 0x00004;
    constexpr uint32_t CLR_INTR = 0x00008;
    constexpr uint32_t SET_INTR = 0x00010;
    constexpr uint32_t CLR_SSTEP = 0x00020;
    constexpr uint32_t SET_SSTEP = 0x00040;
    constexpr uint32_t CLR_INTR_BREAK = 0x00080;
    constexpr uint32_t SET_INTR_BREAK = 0x00100;
    
    // Signal operations (0-7)
    constexpr uint32_t CLR_SIGNAL(int n) { return 0x200 << n; }
    constexpr uint32_t SET_SIGNAL(int n) { return 0x400 << n; }
    
    // Status flags
    constexpr uint32_t STATUS_HALT = 0x001;
    constexpr uint32_t STATUS_BROKE = 0x002;
    constexpr uint32_t STATUS_DMA_BUSY = 0x004;
    constexpr uint32_t STATUS_DMA_FULL = 0x008;
    constexpr uint32_t STATUS_IO_FULL = 0x010;
    constexpr uint32_t STATUS_SSTEP = 0x020;
    constexpr uint32_t STATUS_INTR_BREAK = 0x040;
    
    // Signal status flags (0-7)
    constexpr uint32_t STATUS_SIGNAL(int n) { return 0x80 << n; }
}


class CRegName {
public:
    static const char* const GPR[32];
    static const char* const GPR_Hi[32];
    static const char* const GPR_Lo[32];
    static const char* const Cop0[32];
    static const char* const FPR[32];
    static const char* const FPR_Ctrl[32];
};


class CN64System;
class CSystemEvents;
namespace Nova64 { namespace Interpreter { class R4300iInterpreter; } }
namespace N64System { class CTLB; }
using N64System::CTLB;
namespace N64System { class SystemTimer; }
namespace Nova64 { class MipsMemoryVM; }

// Fix 19: Major refactor of CRegisters class - use composition instead of inheritance
class CRegisters : public CP0registers {
public:
    // COP0 register enum
    enum class COP0Reg : uint8_t {
        Index = 0,
        Random = 1,
        EntryLo0 = 2,
        EntryLo1 = 3,
        Context = 4,
        PageMask = 5,
        Wired = 6,
        BadVAddr = 8,
        Count = 9,
        EntryHi = 10,
        Compare = 11,
        Status = 12,
        Cause = 13,
        EPC = 14,
        PRId = 15,
        Config = 16,
        LLAddr = 17,
        WatchLo = 18,
        WatchHi = 19,
        XContext = 20,
        ECC = 26,
        TagLo = 28,
        TagHi = 29,
        ErrorEPC = 30
    };
    
    CRegisters(CN64System& System, CSystemEvents& SystemEvents);
    
    // Public methods
    void Init();
    void CheckInterrupts();
    void DoAddressError(uint64_t BadVaddr, bool FromRead);
    bool DoIntrException();
    void FixFpuLocations();
    void Reset(bool bPostPif, Nova64::MipsMemoryVM& MMU);
    void TriggerAddressException(uint64_t Address, uint32_t ExceptionCode);
    void TriggerException(uint32_t ExceptionCode, uint32_t Coprocessor = 0);
    
    // Timer access for deferred SP interrupt scheduling
    N64System::SystemTimer& GetSystemTimer() { return m_SystemTimer; }
    CN64System& GetSystem() { return m_System; }

    // Allow recompiler branch callbacks to set TestTimer (matching interpreter
    // JR/JALR behavior where every indirect jump triggers a timer check).
    void SetTestTimer();

    // Get the link address for branch-in-delay-slot: if the CPU is in a delay
    // slot (PipelineStage == Jump), returns outer_jump_target + 4.
    // Otherwise returns fallback (typically pc + 8).
    int32_t GetLinkAddress(int32_t fallback) const;

    // Flush compiled code for a physical RDRAM range.
    // Called by SP DMA Write (DMEM/IMEM→RDRAM) and SMC detection paths.
    void InvalidateRecompCode(uint32_t physAddr, uint32_t length);

    // Deferred RSP interrupt delivery: when true, RSP HLE code should NOT set
    // MI_INTR bits or call CheckInterrupts. Instead, accumulate bits in
    // m_deferred_mi_intr for the RspTimer to apply later.
    bool m_defer_hle_interrupts{false};
    uint32_t m_deferred_mi_intr{0};
    
    uint64_t Cop0_MF(COP0Reg Reg);
    void Cop0_MT(COP0Reg Reg, uint64_t Value);
    void Cop1_CT(uint32_t Reg, uint32_t Value);
    uint64_t Cop2_MF(uint32_t Reg);
    void Cop2_MT(uint32_t Reg, uint64_t Value);
    
    // General registers - use std::array for bounds safety
    uint64_t m_PROGRAM_COUNTER;
    std::array<MipsRegisterType, 32> m_GPR;
    // std::array<uint64_t, 32> m_CP0; // Moved to CP0registers
    uint64_t m_CP0Latch;
    uint64_t m_CP2Latch;
    MipsRegisterType m_HI;
    MipsRegisterType m_LO;
    uint32_t m_LLBit;
    
    // Floating point registers
    std::array<uint32_t, 32> m_FPCR;
    std::array<MipsRegisterType, 32> m_FPR;
    
    // Fix 20: Remove pointer arrays - use union for type punning
    union FPReg {
        uint32_t u32;
        uint64_t u64;
        float f32;
        double f64;
        MipsRegisterType dword;
        FPReg() { u64 = 0; }
    };
    std::array<FPReg, 32> m_FPR_Union;  // Single unified FP register array
    
    // Memory-mapped N64 registers
    std::array<uint32_t, 10> m_RDRAM_Registers;
    std::array<uint32_t, 10> m_SigProcessor_Interface;
    std::array<uint32_t, 10> m_Display_ControlReg;
    std::array<uint32_t, 4> m_Mips_Interface;
    std::array<uint32_t, 14> m_Video_Interface;
    std::array<uint32_t, 6> m_Audio_Interface;
    std::array<uint32_t, 13> m_Peripheral_Interface;
    std::array<uint32_t, 8> m_RDRAM_Interface;
    std::array<uint32_t, 4> m_SerialInterface;
    std::array<uint32_t, 22> m_DiskInterface;
    
    uint32_t m_AudioIntrReg;
    uint32_t m_GfxIntrReg;
    uint32_t m_RspIntrReg;
    bool m_DpFreshFromHLE = false;  // Protect DP bit from first CLR_DP_INTR after HLE task
    
private:
    // Private constructor to prevent copying
    CRegisters() = delete;
    CRegisters(const CRegisters&) = delete;
    CRegisters& operator=(const CRegisters&) = delete;
    
    bool m_FirstInterrupt; 
    CN64System& m_System;
    CSystemEvents& m_SystemEvents;
    N64System::SystemTimer& m_SystemTimer;
    CTLB& m_TLB;
    
    // Helper method to access COP0 registers safely
    uint64_t& GetCop0RegRef(COP0Reg reg);
    const uint64_t& GetCop0RegRef(COP0Reg reg) const;
};