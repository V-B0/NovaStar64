/**
 * @file MIPSYinterfacehandler.hpp
 * @brief MIPS Interface (MI) handler declarations: mode, interrupt status and interrupt mask registers.
 */
#pragma once
#include <cstdint>
#include <memory>
#include "Memoryhandler.hpp"
#include "../../common/log.hpp"

class MIPSYInterface {
public:
    uint32_t* const MI_INIT_MODE_REG;
    uint32_t* const MI_MODE_REG;
    uint32_t* const MI_VERSION_REG;
    uint32_t* const MI_NOOP_REG;
    uint32_t* const MI_INTR_REG;
    uint32_t* const MI_INTR_MASK_REG;

protected:
    explicit MIPSYInterface(uint32_t* reg0, uint32_t* reg1, uint32_t* reg2, uint32_t* reg3, uint32_t* reg4,
                            uint32_t* reg5) :
        MI_INIT_MODE_REG(reg0), MI_MODE_REG(reg1), MI_VERSION_REG(reg2), MI_NOOP_REG(reg3), MI_INTR_REG(reg4),
        MI_INTR_MASK_REG(reg5) {}

public:
    MIPSYInterface(const MIPSYInterface&) = delete;
    MIPSYInterface& operator=(const MIPSYInterface&) = delete;
    MIPSYInterface(MIPSYInterface&&) = default;
    MIPSYInterface& operator=(MIPSYInterface&&) = delete;
    virtual ~MIPSYInterface() = default;

    uint32_t getInitMode() const { return *MI_INIT_MODE_REG; }
    uint32_t getMode() const { return *MI_MODE_REG; }
    uint32_t getVersion() const { return *MI_VERSION_REG; }

    bool isInterruptPending() const { return (*MI_INTR_REG & *MI_INTR_MASK_REG) != 0; }
};

class CRegisters;

class MIPSYInterfaceHandler : public MemoryHandler, public MIPSYInterface {
public:
    enum class LogLevel { Trace, Debug, Info, Warning, Error };

    MIPSYInterfaceHandler(CRegisters& reg);
    MIPSYInterfaceHandler(const MIPSYInterfaceHandler&) = delete;
    MIPSYInterfaceHandler& operator=(const MIPSYInterfaceHandler&) = delete;

    bool Read32(uint32_t addr, uint32_t& val) override;
    bool Write32(uint32_t addr, uint32_t val, uint32_t mask) override;

    void WriteLog(LogLevel level, const char* format, ...);
    void ClearInterrupt(uint32_t mask);
    void SetInterrupt(uint32_t interruptBit);

    bool IsInterruptPending() const;
    uint32_t GetPendingInterrupts() const;

    bool IsInitMode() const;
    bool IsEBusMode() const;
    bool IsRDRAMRegMode() const;

    void Reset();

private:
    void ProcessModeWrite(uint32_t val);
    void ProcessMaskWrite(uint32_t val);

public:
    CRegisters& m_Reg;
    uint64_t& m_PC;
};
