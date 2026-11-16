/**
 * @file TimeStamp.cpp
 * @brief Time Stamp implementation.
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

HighResolutionTimestamp& HighResolutionTimestamp::SetToNow() {
    m_timePoint = std::chrono::high_resolution_clock::now();
    return *this;
}

uint64_t HighResolutionTimestamp::GetMicroseconds() const {
    auto duration = m_timePoint.time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

void HighResolutionTimestamp::SetMicroseconds(uint64_t microseconds) {
    m_timePoint = std::chrono::high_resolution_clock::time_point(std::chrono::microseconds(microseconds));
}

uint64_t HighResolutionTimestamp::GetNanoseconds() const {
    auto duration = m_timePoint.time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

uint64_t HighResolutionTimestamp::GetMilliseconds() const {
    auto duration = m_timePoint.time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

double HighResolutionTimestamp::GetSeconds() const {
    auto duration = m_timePoint.time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

HighResolutionTimestamp HighResolutionTimestamp::Now() {
    HighResolutionTimestamp ts;
    ts.SetToNow();
    return ts;
}

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
