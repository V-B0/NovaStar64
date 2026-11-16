/**
 * @file Thread.cpp
 * @brief Thread implementation.
 */
#include "Thread.hpp"
#include "trace.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#endif

// Constructor: Initializes thread with start routine
// The start routine will be called when Start() is invoked
CThread::CThread(CTHREAD_START_ROUTINE lpStartAddress) :
    m_StartAddress(lpStartAddress), m_lpThreadParameter(nullptr), m_thread(nullptr), m_threadID(0)
#ifndef _WIN32
    ,
    m_running(false)
#endif
{
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "CThread constructor");

    if (lpStartAddress == nullptr) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Thread start address is null");
    }
}

// Destructor: Cleans up thread resources
// Terminates thread if still running (except when called from within the thread itself)
CThread::~CThread() {
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "CThread destructor start");

    // Check if we're destroying the thread from within itself
    // This would cause deadlock, so we log an error
    if (GetCurrentThreadId() == m_threadID) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error,
                   "Attempting to delete CThread from within its own thread context - potential deadlock!");
        // Don't terminate - let the thread finish naturally
    } else if (IsRunning()) {
        // Thread is still running and we're calling from a different thread
        WriteTrace(Trace::Module::Core, Trace::Severity::Warning,
                   "Thread still running during destruction - forcing termination");
        Terminate();
    }

#ifdef _WIN32
    // Close the Windows thread handle
    if (m_thread != nullptr) {
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
#else
    // POSIX: pthread_t is cleaned up in ThreadWrapper after thread exits
    // No explicit cleanup needed here as ThreadWrapper already deletes it
#endif

    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "CThread destructor done");
}

// Start: Begins thread execution with the given parameter
// Returns true on successful thread creation, false otherwise
bool CThread::Start(void* lpThreadParameter) {
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Starting thread");

    // Validate that we're not trying to start an already running thread
    if (IsRunning()) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Warning, "Attempted to start thread that is already running");
        return false;
    }

    m_lpThreadParameter = lpThreadParameter;

#ifdef _WIN32
    // Windows: Use CreateThread to spawn a new thread
    // The thread ID is set by CreateThread
    m_thread = CreateThread(nullptr,                               // Default security attributes
                            0,                                     // Default stack size
                            (LPTHREAD_START_ROUTINE)ThreadWrapper, // Thread function
                            this,                                  // Thread parameter (this CThread object)
                            0,                                     // Start immediately
                            (LPDWORD)&m_threadID                   // Receive thread ID
    );

    if (m_thread == nullptr) {
        DWORD error = GetLastError();
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Failed to create thread, error code: %lu", error);
        return false;
    }

    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread created successfully, ID: %u", m_threadID);
#else
    // POSIX: Use pthread_create to spawn a new thread
    // Allocate pthread_t on heap to avoid lifetime issues
    pthread_t* thread_id = nullptr;

    try {
        thread_id = new pthread_t;
    } catch (const std::bad_alloc&) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Failed to allocate memory for pthread_t");
        return false;
    }

    m_thread = static_cast<void*>(thread_id);

    // Create the thread with default attributes
    int result = pthread_create(thread_id,                       // Thread ID output
                                nullptr,                         // Default attributes
                                (void* (*)(void*))ThreadWrapper, // Thread function
                                this                             // Thread parameter (this CThread object)
    );

    if (result != 0) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Failed to create pthread, error code: %d", result);
        delete thread_id;
        m_thread = nullptr;
        return false;
    }

    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "pthread created successfully");
#endif

    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread start completed");
    return true;
}

// ThreadWrapper: Internal wrapper function that calls the user's thread routine
// Handles platform-specific setup/cleanup and exception handling
void* CThread::ThreadWrapper(CThread* pThis) {
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread wrapper start");

    if (pThis == nullptr) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "ThreadWrapper called with null CThread pointer");
        return nullptr;
    }

    // Set the thread ID now that we're executing in the thread context
    pThis->m_threadID = GetCurrentThreadId();

#ifndef _WIN32
    // POSIX: Set running flag
    pThis->m_running = true;
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread is now running (POSIX), ID: %u", pThis->m_threadID);
#endif

    // Call the user's thread routine with exception handling
    void* result = nullptr;

    try {
        // Execute the actual thread function
        // Need to handle different pointer sizes on different architectures
#if defined(__i386__) || defined(_M_IX86) || defined(__arm__) || defined(_M_ARM)
        // 32-bit architectures: direct cast
        result = reinterpret_cast<void*>(pThis->m_StartAddress(pThis->m_lpThreadParameter));
#else
        // 64-bit architectures: the start routine returns void*; round-trip
        // through uintptr_t (static_cast from void* to an integer is ill-formed).
        result =
            reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pThis->m_StartAddress(pThis->m_lpThreadParameter)));
#endif
    } catch (const std::exception& e) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Unhandled std::exception in thread: %s", e.what());
    } catch (...) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Unhandled unknown exception in thread");
    }

#ifndef _WIN32
    // POSIX: Clear running flag and clean up pthread_t
    pThis->m_running = false;
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread finished running (POSIX)");

    // Clean up the dynamically allocated pthread_t
    pthread_t* thread_id = static_cast<pthread_t*>(pThis->m_thread);
    delete thread_id;
    pThis->m_thread = nullptr;
#else
    // Windows: CloseHandle is called in destructor
    // We don't close it here because the thread is still technically running
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread finished running (Windows)");
#endif

    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread wrapper done");
    return result;
}

// IsRunning: Checks if the thread is currently executing
// Returns true if thread is active, false if finished or never started
bool CThread::IsRunning() const {
    WriteTrace(Trace::Module::Core, Trace::Severity::Verbose, "Checking if thread is running");

    if (m_thread == nullptr) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Verbose, "Thread is not running (handle is null)");
        return false;
    }

#ifdef _WIN32
    // Windows: Query the thread exit code
    // If exit code is STILL_ACTIVE (259), the thread is still running
    DWORD exitCode = 0;
    if (GetExitCodeThread(m_thread, &exitCode)) {
        if (exitCode == STILL_ACTIVE) {
            WriteTrace(Trace::Module::Core, Trace::Severity::Verbose, "Thread is running (Windows)");
            return true;
        }
    } else {
        DWORD error = GetLastError();
        WriteTrace(Trace::Module::Core, Trace::Severity::Warning, "Failed to get thread exit code, error: %lu", error);
    }

    WriteTrace(Trace::Module::Core, Trace::Severity::Verbose, "Thread is not running (Windows)");
    return false;
#else
    // POSIX: Use our m_running flag
    // This is more reliable than trying to query pthread status
    WriteTrace(Trace::Module::Core, Trace::Severity::Verbose, "Thread is %s (POSIX)",
               m_running ? "running" : "not running");
    return m_running;
#endif
}

// Terminate: Forcefully stops thread execution
// WARNING: This is dangerous and should only be used as a last resort
// Prefer using signaling mechanisms to allow threads to exit gracefully
void CThread::Terminate() {
    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Attempting to terminate thread");

    if (!IsRunning()) {
        WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread is not running, no termination needed");
        return;
    }

#ifdef _WIN32
    // Windows: Use TerminateThread (dangerous - can leak resources)
    WriteTrace(Trace::Module::Core, Trace::Severity::Warning,
               "Force terminating thread (Windows) - this may leak resources");

    if (m_thread != nullptr) {
        if (!TerminateThread(m_thread, 0)) {
            DWORD error = GetLastError();
            WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Failed to terminate thread, error: %lu", error);
        } else {
            WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread terminated successfully");
        }
    }
#else
    // POSIX: pthread_cancel is also dangerous, but better than nothing
    // Modern practice would be to use condition variables or signals
    WriteTrace(Trace::Module::Core, Trace::Severity::Warning,
               "Force terminating thread (POSIX) - using pthread_cancel");

    if (m_thread != nullptr) {
        pthread_t* thread_id = static_cast<pthread_t*>(m_thread);
        int result = pthread_cancel(*thread_id);

        if (result == 0) {
            WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Thread cancellation initiated");

            // Wait for thread to finish cancellation
            pthread_join(*thread_id, nullptr);
            m_running = false;
        } else {
            WriteTrace(Trace::Module::Core, Trace::Severity::Error, "Failed to cancel thread, error: %d", result);
        }
    }
#endif

    WriteTrace(Trace::Module::Core, Trace::Severity::Debug, "Terminate completed");
}

// GetCurrentThreadId: Returns the ID of the calling thread
// Static method - can be called without a CThread instance
uint32_t CThread::GetCurrentThreadId() {
#ifdef _WIN32
    // Windows: Use GetCurrentThreadId API
    return ::GetCurrentThreadId();
#elif defined(__APPLE__)
    // macOS: pthread_t is opaque; use pthread_threadid_np.  (Checked before the
    // Linux branch because macOS's <sys/syscall.h> defines SYS_gettid without
    // providing __NR_gettid.)
    uint64_t tid;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<uint32_t>(tid);
#elif defined(__linux__) && defined(SYS_gettid)
    // Linux: Use syscall to get thread ID (GLIBC doesn't always wrap this)
    return static_cast<uint32_t>(syscall(SYS_gettid));
#else
    // Fallback: Use pthread_self (returns pthread_t, not ideal but works)
    // Note: pthread_t might not be an integer type on all platforms
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}
