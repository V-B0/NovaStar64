/**
 * @file LockGuard.hpp
 * @brief A recursive mutex with timed locking, and a scoped lock for it.
 */
#pragma once

#include <atomic>
#include <mutex>

/**
 * @class RecursiveMutex
 * @brief A recursive mutex with optional timeout support
 */
class RecursiveMutex {
public:
    RecursiveMutex();
    ~RecursiveMutex();

    // Non-copyable, non-movable
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex(RecursiveMutex&&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(RecursiveMutex&&) = delete;

    /**
     * @brief Locks the mutex (blocks indefinitely)
     */
    void Lock();

    void Unlock();

private:
    void* m_mutex;                             ///< Platform-specific mutex
    [[maybe_unused]] void* m_condition;        ///< Platform-specific condition variable (if needed)
    std::atomic<uint32_t> m_ownerThreadId{0};  ///< Current owning thread
    std::atomic<uint32_t> m_recursionCount{0}; ///< Lock recursion count
};

/**
 * @class ScopedLock
 * @brief Generic RAII lock for any lockable object
 */
template <typename Lockable> class ScopedLock {
public:
    explicit ScopedLock(Lockable& lockable) : m_lockable(lockable), m_locked(true) { m_lockable.Lock(); }

    ScopedLock(Lockable& lockable, std::adopt_lock_t) : m_lockable(lockable), m_locked(true) {}

    ~ScopedLock() {
        if (m_locked) {
            m_lockable.Unlock();
        }
    }

    ScopedLock(ScopedLock&& other) noexcept : m_lockable(other.m_lockable), m_locked(other.m_locked) {
        other.m_locked = false;
    }

    ScopedLock& operator=(ScopedLock&& other) noexcept {
        if (this != &other) {
            if (m_locked) {
                m_lockable.Unlock();
            }
            m_lockable = other.m_lockable;
            m_locked = other.m_locked;
            other.m_locked = false;
        }
        return *this;
    }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

    /**
     * @brief Early unlock of the mutex
     */
    void Unlock() {
        if (m_locked) {
            m_lockable.Unlock();
            m_locked = false;
        }
    }

    void Relock() {
        if (!m_locked) {
            m_lockable.Lock();
            m_locked = true;
        }
    }

    [[nodiscard]] bool IsLocked() const { return m_locked; }

private:
    Lockable& m_lockable;
    bool m_locked;
};

using CriticalSection = RecursiveMutex;
using LockGuard = ScopedLock<RecursiveMutex>;
