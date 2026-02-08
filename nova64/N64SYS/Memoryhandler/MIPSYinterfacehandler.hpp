#include <cstdint>
#include <memory>
#include <span>
#include "Memoryhandler.hpp"



class MIPSYInterface
{
public:
    
    uint32_t* const MI_INIT_MODE_REG;
    uint32_t* const MI_MODE_REG;
    uint32_t* const MI_VERSION_REG;
    uint32_t* const MI_NOOP_REG;
    uint32_t* const MI_INTR_REG;
    uint32_t* const MI_INTR_MASK_REG;

protected:
    
    explicit MIPSYInterface(uint32_t* reg0, uint32_t* reg1, uint32_t* reg2, uint32_t* reg3, uint32_t* reg4, uint32_t* reg5) :
        MI_INIT_MODE_REG(reg0),
        MI_MODE_REG(reg1),
        MI_VERSION_REG(reg2),
        MI_NOOP_REG(reg3),
        MI_INTR_REG(reg4),
        MI_INTR_MASK_REG(reg5)
    {
    }

public:
    
    MIPSYInterface(const MIPSYInterface&) = delete;
    MIPSYInterface& operator=(const MIPSYInterface&) = delete;
    MIPSYInterface(MIPSYInterface&&) = default;
    MIPSYInterface& operator=(MIPSYInterface&&) = default;
    virtual ~MIPSYInterface() = default;
   
    uint32_t getInitMode() const { return *MI_INIT_MODE_REG; }
    uint32_t getMode() const { return *MI_MODE_REG; }
    uint32_t getVersion() const { return *MI_VERSION_REG; }
    
    
    bool isInterruptPending() const { return (*MI_INTR_REG & *MI_INTR_MASK_REG) != 0; }
};

class Cregisters;

class MIPSYInterfaceHandler:
  public Memoryhandler,
  public MIPSYInterface
{
  public: 
    MIPSYInterfaceHandler(const MIPSYInterfaceHandler&);
    MIPSYInterfaceHandler & operator=(const MIPSYInterfaceHandler &);

    Cregisters & m_Reg;
    uint64_t & m_PC;
};
