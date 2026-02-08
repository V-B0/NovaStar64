#pragma once
#include "Memoryhandler.hpp"

class UnCachedSysMemory :
    public Memoryhandler
{
public:
    UnCachedSysMemory();
    bool Read32(uint32_t Address, uint32_t & Value);
    bool Write32(uint32_t Address, uint32_t Value, uint32_t Mask);

private:
    UnCachedSysMemory(const UnCachedSysMemory &);
    UnCachedSysMemory & operator=(const UnCachedSysMemory &);
};