#pragma once
#include "Memoryhandler.hpp"

class Cregisters;
class CN64Rom;

class CachedSysMemory :
  public Memoryhandler
{
  CachedSysMemory(Cregisters & Reg, CN64Rom & DDRom);

  bool Read32(uint32_t addr, uint32_t & val);
  bool Write32(uint32_t addr, uint32_t val, uint32_t mask);

  private:
    Cregisters & m_Reg;
    uint32_t m_DDRomSize;
    uint8_t & m_DDRom;


};