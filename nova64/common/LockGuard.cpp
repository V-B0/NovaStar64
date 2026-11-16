/**
 * @file LockGuard.cpp
 * @brief RecursiveMutex implementation (CRITICAL_SECTION on Windows, a recursive pthread mutex elsewhere).
 */
#include "LockGuard.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <pthread.h>
#include <cerrno>
#include <ctime>
#include <cstdint>
#endif

namespace {} // namespace

// --- RecursiveMutex ---

RecursiveMutex::RecursiveMutex() : m_mutex(nullptr), m_condition(nullptr) {
#ifdef _WIN32
    auto* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);
    m_mutex = cs;
#else
    auto* mtx = new pthread_mutex_t;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    m_mutex = mtx;
#endif
}

RecursiveMutex::~RecursiveMutex() {
#ifdef _WIN32
    auto* cs = static_cast<CRITICAL_SECTION*>(m_mutex);
    DeleteCriticalSection(cs);
    delete cs;
#else
    auto* mtx = static_cast<pthread_mutex_t*>(m_mutex);
    pthread_mutex_destroy(mtx);
    delete mtx;
#endif
}
