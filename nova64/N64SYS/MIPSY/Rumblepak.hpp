/**
 * @file Rumblepak.hpp
 * @brief Rumble Pak interface: reads and writes of the pak's motor register.
 */
#pragma once
#include <cstdint>

class Rumblepak {
public:
    static void ReadFrom(uint32_t addr, uint8_t& data);
    static void WriteTo(uint32_t addr, uint8_t data);
};
