/**
 * @file Thread.hpp
 * @brief Thread declarations and types.
 */
#pragma once

#include <cstdint>

class CThread
{
public:
#ifdef _WIN32
    using CTHREAD_START_ROUTINE = uint32_t (*)(void* lpThreadParameter);
#else
    using CTHREAD_START_ROUTINE = void* (*)(void*);
#endif

    explicit CThread(CTHREAD_START_ROUTINE lpStartAddress);
    ~CThread();

    // Thread management
    bool Start(void* lpThreadParameter);
    bool IsRunning() const;
    void Terminate();
    
    // Thread information
    uint32_t GetThreadID() const { return m_threadID; }
    static uint32_t GetCurrentThreadId();

    // Delete copy/move semantics
    CThread() = delete;
    CThread(const CThread&) = delete;
    CThread(CThread&&) = delete;
    CThread& operator=(const CThread&) = delete;
    CThread& operator=(CThread&&) = delete;

private:
    static void* ThreadWrapper(CThread* pThis);

private:
    CTHREAD_START_ROUTINE m_StartAddress;
    void* m_lpThreadParameter;
    void* m_thread;
    uint32_t m_threadID;
#ifndef _WIN32
    bool m_running;
#endif
};