/**
 * @file Memoryhandler.hpp
 * @brief Interface for memory-mapped hardware regions.
 */
#pragma once
#include <cstdint>

/// A memory-mapped hardware region. Read32/Write32 are mandatory; the 8- and
/// 64-bit accesses report "unhandled" (false) unless a handler overrides them.
class MemoryHandler {
public:
    virtual ~MemoryHandler() = default;
    virtual bool Read32(uint32_t addr, uint32_t& val) = 0;
    virtual bool Write32(uint32_t addr, uint32_t val, uint32_t mask) = 0;
    virtual bool Read8(uint32_t /*addr*/, uint8_t& /*val*/) { return false; }
    virtual bool Write8(uint32_t /*addr*/, uint8_t /*val*/) { return false; }
};
