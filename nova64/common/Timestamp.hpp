/**
 * @file Timestamp.hpp
 * @brief High-resolution timestamp declarations.
 */
#pragma once

#include <chrono>
#include <cstdint>

/**
 * @class HighResolutionTimestamp
 * @brief Simple high-resolution timestamp with microsecond precision
 */
class HighResolutionTimestamp {
public:
    HighResolutionTimestamp();

    // Comparison
    bool operator==(const HighResolutionTimestamp& other) const;
    bool operator<(const HighResolutionTimestamp& other) const;

private:
    std::chrono::high_resolution_clock::time_point m_timePoint;

#ifdef _WIN32
    static bool s_initialized;
    static int64_t s_frequency;
#endif
};
