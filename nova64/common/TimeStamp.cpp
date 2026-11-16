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

bool HighResolutionTimestamp::operator==(const HighResolutionTimestamp& other) const {
    return m_timePoint == other.m_timePoint;
}

bool HighResolutionTimestamp::operator<(const HighResolutionTimestamp& other) const {
    return m_timePoint < other.m_timePoint;
}
