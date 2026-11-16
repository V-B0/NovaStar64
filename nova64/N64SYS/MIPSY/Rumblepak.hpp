/**
 * @file Rumblepak.hpp
 * @brief Rumblepak declarations and types.
 */
#pragma once
#include <cstdint>

class Rumblepak {
public:
    static void ReadFrom(uint32_t addr, uint8_t& data);
    static void WriteTo(uint32_t addr, uint8_t data);
};
