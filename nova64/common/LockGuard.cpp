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

namespace {
// Portable 32-bit thread identifier.
//   - Windows: GetCurrentThreadId()
//   - Apple:   pthread_t is an opaque pointer, so pthread_self() cannot be
//              cast to an integer; use pthread_threadid_np() instead.
//   - Other POSIX: pthread_t is an integral type.
inline uint32_t CurrentThreadId32() noexcept {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<uint32_t>(tid);
#else
    return static_cast<uint32_t>(pthread_self());
#endif
}
} // namespace

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

void RecursiveMutex::Lock() {
#ifdef _WIN32
    EnterCriticalSection(static_cast<CRITICAL_SECTION*>(m_mutex));
#else
    pthread_mutex_lock(static_cast<pthread_mutex_t*>(m_mutex));
#endif
    m_ownerThreadId.store(CurrentThreadId32(), std::memory_order_relaxed);
    m_recursionCount.fetch_add(1, std::memory_order_relaxed);
}

void RecursiveMutex::Unlock() {
    uint32_t count = m_recursionCount.fetch_sub(1, std::memory_order_relaxed);
    if (count == 1) {
        m_ownerThreadId.store(0, std::memory_order_relaxed);
    }
#ifdef _WIN32
    LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(m_mutex));
#else
    pthread_mutex_unlock(static_cast<pthread_mutex_t*>(m_mutex));
#endif
}
