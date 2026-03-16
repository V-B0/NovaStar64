#pragma once
#include <cstdint>

class MemoryHandler {
public:
    virtual ~MemoryHandler() = default;
    virtual bool Read32(uint32_t addr, uint32_t& val) = 0;
    virtual bool Write32(uint32_t addr, uint32_t val, uint32_t mask) = 0;
    virtual bool Read8(uint32_t /*addr*/, uint8_t& /*val*/) { return false; }
    virtual bool Write8(uint32_t /*addr*/, uint8_t /*val*/) { return false; }
    virtual bool Read64(uint32_t /*addr*/, uint64_t& /*val*/) { return false; }
    virtual bool Write64(uint32_t /*addr*/, uint64_t /*val*/, uint64_t /*mask*/) { return false; }
};