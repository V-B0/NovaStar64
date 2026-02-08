#pragma
#include "MIPSYinterfacehandler.hpp"
#include <cstdint>

class MemoryHandler {
public:
    virtual ~MemoryHandler() = default;
    virtual bool Read32(uint32_t addr, uint32_t& val) = 0;
    virtual bool Write32(uint32_t addr, uint32_t val, uint32_t mask) = 0;
};