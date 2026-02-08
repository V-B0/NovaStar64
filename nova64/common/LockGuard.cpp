// LockGuard.cpp - RecursiveMutex implementation
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
#endif

// ============================================================
// RecursiveMutex
// ============================================================

RecursiveMutex::RecursiveMutex()
    : m_mutex(nullptr), m_condition(nullptr)
{
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

RecursiveMutex::~RecursiveMutex()
{
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

void RecursiveMutex::Lock()
{
#ifdef _WIN32
    EnterCriticalSection(static_cast<CRITICAL_SECTION*>(m_mutex));
#else
    pthread_mutex_lock(static_cast<pthread_mutex_t*>(m_mutex));
#endif
    m_ownerThreadId.store(
#ifdef _WIN32
        GetCurrentThreadId(),
#else
        static_cast<uint32_t>(pthread_self()),
#endif
        std::memory_order_relaxed);
    m_recursionCount.fetch_add(1, std::memory_order_relaxed);
}

bool RecursiveMutex::TryLock()
{
    bool acquired;
#ifdef _WIN32
    acquired = TryEnterCriticalSection(static_cast<CRITICAL_SECTION*>(m_mutex)) != 0;
#else
    acquired = pthread_mutex_trylock(static_cast<pthread_mutex_t*>(m_mutex)) == 0;
#endif
    if (acquired) {
        m_ownerThreadId.store(
#ifdef _WIN32
            GetCurrentThreadId(),
#else
            static_cast<uint32_t>(pthread_self()),
#endif
            std::memory_order_relaxed);
        m_recursionCount.fetch_add(1, std::memory_order_relaxed);
    }
    return acquired;
}

bool RecursiveMutex::TryLock(uint32_t timeoutMs)
{
    if (timeoutMs == 0) {
        return TryLock();
    }
    if (timeoutMs == UINT32_MAX) {
        Lock();
        return true;
    }

#ifdef _WIN32
    // Windows CRITICAL_SECTION doesn't support timed waits natively,
    // so we spin-poll with short sleeps.
    auto start = GetTickCount64();
    while ((GetTickCount64() - start) < timeoutMs) {
        if (TryLock()) {
            return true;
        }
        Sleep(1);
    }
    return TryLock(); // one last attempt
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeoutMs / 1000;
    ts.tv_nsec += (timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_mutex_timedlock(static_cast<pthread_mutex_t*>(m_mutex), &ts);
    if (rc == 0) {
        m_ownerThreadId.store(static_cast<uint32_t>(pthread_self()),
                              std::memory_order_relaxed);
        m_recursionCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
#endif
}

void RecursiveMutex::Unlock()
{
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

uint32_t RecursiveMutex::GetRecursionCount() const
{
    return m_recursionCount.load(std::memory_order_relaxed);
}