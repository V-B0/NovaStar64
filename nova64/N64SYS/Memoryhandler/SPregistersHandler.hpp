#pragma once
#include "MIPSYinterfacehandler.hpp"
#include "Memoryhandler.hpp"
#include <cstdint>
#include <cstring>

class SPRegisters {
  protected:
    SPRegisters(MIPSYInterface* SignalProcessorInterface);
  public:
    uint32_t & SP_MEM_ADDR;
    uint32_t & SP_DRAM_ADDR;
    uint32_t & SP_RD_LEN;
    uint32_t & SP_WR_LEN;
    uint32_t & SP_STATUS;
    uint32_t & SP_DMA_FULL;
    uint32_t & SP_DMA_BUSY;
    uint32_t & SP_SEMAPHORE;
    uint32_t & SP_PC;
    uint32_t & SP_IBIST;
  private:
    SPRegisters( const SPRegisters& );
    SPRegisters&operator= ( const SPRegisters& );
};

class MIPSYInterface;
class CRegisters;

class SRegistersHandler:
  public MemoryHandler,
  private SPRegisters,
  private MIPSYInterface
{
public:
    SRegistersHandler() : SPRegisters(this), MIPSYInterface() {
        std::memset(m_IMEM, 0, sizeof(m_IMEM));
        std::memset(m_DMEM, 0, sizeof(m_DMEM));
    }
    
    bool Read32(uint32_t Address, uint32_t & Value) override {
        Value = 0;
        return true;
    }
    
    bool Write32(uint32_t Address, uint32_t Value, uint32_t Mask) override {
        return true;
    }

    uint8_t * Imem()
    {
        return &m_IMEM[0];
    }

    uint8_t * Dmem()
    {
        return &m_DMEM[0];
    }

private:
    SRegistersHandler(const SRegistersHandler &) = delete;
    SRegistersHandler & operator=(const SRegistersHandler &) = delete;

    static void stSystemReset(SRegistersHandler * _this)
    {
        _this->SystemReset();
    }

    void ClearSPInterrupt(void) {}
    void SetSPInterrupt(void) {}
    void SetHalt(void) {}
    void DmaReadDone(uint32_t End) {}
    void SystemReset(void) {}

    uint8_t m_IMEM[0x1000];
    uint8_t m_DMEM[0x1000];
};
