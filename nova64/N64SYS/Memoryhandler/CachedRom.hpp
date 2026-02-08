#pragma once
#include "Memoryhandler.hpp"
#include <cstdint>
#include <span>
#include <array>
#include <bitset>
#include <optional>

namespace DD64 {

  
   
    enum class DDStatus : uint32_t {
        DATA_REQUEST    = 0x40000000,
        C2_TRANSFER     = 0x10000000,
        BUFFER_ERROR    = 0x08000000,
        BUFFER_INTERRUPT = 0x04000000,
        MECHANICAL_INT  = 0x02000000,
        DISK_PRESENT    = 0x01000000,
        BUSY_STATE      = 0x00800000,
        RESET_STATE     = 0x00400000,
        MOTOR_SPINNING  = 0x00100000,
        HEAD_RETRACTED  = 0x00080000,
        WRITE_PROTECT   = 0x00040000,
        MECHANICAL_ERROR = 0x00020000,
        DISK_CHANGED    = 0x00010000
    };

    enum class DDBMStatus : uint32_t {
        RUNNING         = 0x80000000,
        ERROR           = 0x04000000,
        MICRO_MODE      = 0x02000000,
        BLOCK_MODE      = 0x01000000
    };

    enum class DDBMControl : uint32_t {
        START           = 0x80000000,
        MANAGER_MODE    = 0x40000000,
        INT_MASK        = 0x20000000,
        RESET           = 0x10000000,
        BLOCK_TRANSFER  = 0x02000000,
        MECHANICAL_RESET = 0x01000000
    };

    
    constexpr bool isStatusSet(uint32_t reg, DDStatus flag) {
        return (reg & static_cast<uint32_t>(flag)) != 0;
    }
    
    constexpr bool isBMStatusSet(uint32_t reg, DDBMStatus flag) {
        return (reg & static_cast<uint32_t>(flag)) != 0;
    }
    
    constexpr uint32_t setBMControl(uint32_t reg, DDBMControl flag) {
        return reg | static_cast<uint32_t>(flag);
    }
    
    constexpr uint32_t clearBMControl(uint32_t reg, DDBMControl flag) {
        return reg & ~static_cast<uint32_t>(flag);
    }

    
    class DiskInterfaceReg
    {
    protected:
        DiskInterfaceReg(uint32_t * DiskInterface);

    public:
        uint32_t & ASIC_DATA;
        uint32_t & ASIC_MISC_REG;
        uint32_t & ASIC_STATUS;
        uint32_t & ASIC_CMD;
        uint32_t & ASIC_CUR_TK;
        uint32_t & ASIC_BM_STATUS;
        uint32_t & ASIC_BM_CTL;
        uint32_t & ASIC_ERR_SECTOR;
        uint32_t & ASIC_SEQ_STATUS;
        uint32_t & ASIC_SEQ_CTL;
        uint32_t & ASIC_CUR_SECTOR;
        uint32_t & ASIC_HARD_RESET;
        uint32_t & ASIC_C1_S0;
        uint32_t & ASIC_HOST_SECBYTE;
        uint32_t & ASIC_C1_S2;
        uint32_t & ASIC_SEC_BYTE;
        uint32_t & ASIC_C1_S4;
        uint32_t & ASIC_C1_S6;
        uint32_t & ASIC_CUR_ADDR;
        uint32_t & ASIC_ID_REG;
        uint32_t & ASIC_TEST_REG;
        uint32_t & ASIC_TEST_PIN_SEL;
    };
    class CachedRom :
    public Memoryhandler,
    private DiskInterfaceReg
    {
    public:
        CachedRom(Cregisters & Reg, uint32_t* diskRegs)
            : DiskInterfaceReg(diskRegs), m_Reg(Reg) 
        {
        }
        
        ~CachedRom()= default;
        
        bool Read32(uint32_t Address, uint32_t & Value) override;
        bool Write32(uint32_t Address, uint32_t Value, uint32_t Mask) override;

    private:
        Cregisters & m_Reg;
        std::array<uint32_t, 32> m_DiskRegStorage; // If we need to own it
        
        // Delete with = delete
        CachedRom() = delete;
        CachedRom(const CachedRom &) = delete;
        CachedRom & operator=(const CachedRom &) = delete;
    };

}