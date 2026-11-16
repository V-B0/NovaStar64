/**
 * @file Thread.hpp
 * @brief CThread, a thin wrapper that runs one start routine on its own thread.
 */
#pragma once

#include <cstdint>

/// Runs a start routine on its own thread. There is no join: poll IsRunning() to
/// wait for the thread to finish.
class CThread {
public:
#ifdef _WIN32
    using CTHREAD_START_ROUTINE = uint32_t (*)(void* lpThreadParameter);
#else
    using CTHREAD_START_ROUTINE = void* (*)(void*);
#endif

    explicit CThread(CTHREAD_START_ROUTINE lpStartAddress);
    ~CThread();

    // Thread management
    /// Starts the thread; false if it is already running or could not be created.
    bool Start(void* lpThreadParameter);
    bool IsRunning() const;
    /// Forcibly stops the thread (TerminateThread / pthread_cancel). A last resort
    /// that may leak resources the thread holds; prefer signalling it to exit.
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
