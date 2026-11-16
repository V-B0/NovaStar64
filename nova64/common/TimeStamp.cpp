/**
 * @file TimeStamp.cpp
 * @brief HighResolutionTimestamp implementation.
 */
#include "Timestamp.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#ifdef _WIN32
bool HighResolutionTimestamp::s_initialized = false;
int64_t HighResolutionTimestamp::s_frequency = 0;

#endif

HighResolutionTimestamp::HighResolutionTimestamp() : m_timePoint(std::chrono::high_resolution_clock::now()) {}

uint64_t HighResolutionTimestamp::GetFrequency() {
#ifdef _WIN32

    if (!s_initialized) {
        LARGE_INTEGER freq{};
        if (QueryPerformanceFrequency(&freq)) {
            s_frequency = freq.QuadPart;
        } else {

            s_frequency = 10'000'000;
        }
        s_initialized = true;
    }

    return static_cast<uint64_t>(s_frequency);
#else

    using period = std::chrono::high_resolution_clock::period;
    constexpr uint64_t frequency = static_cast<uint64_t>(period::den / period::num);
    return frequency;
#endif
}

bool HighResolutionTimestamp::operator==(const HighResolutionTimestamp& other) const {
    return m_timePoint == other.m_timePoint;
}

bool HighResolutionTimestamp::operator<(const HighResolutionTimestamp& other) const {
    return m_timePoint < other.m_timePoint;
}
