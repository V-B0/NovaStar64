#pragma once

#include <atomic>
#include <mutex>

/**
 * @class RecursiveMutex
 * @brief A recursive mutex with optional timeout support
 */
class RecursiveMutex
{
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
    
    /**
     * @brief Attempts to lock the mutex without blocking
     * @return true if lock was acquired, false otherwise
     */
    bool TryLock();
    
    /**
     * @brief Attempts to lock the mutex with timeout
     * @param timeoutMs Timeout in milliseconds (0 = no wait, UINT32_MAX = infinite)
     * @return true if lock was acquired, false if timed out
     */
    bool TryLock(uint32_t timeoutMs);
    
    /**
     * @brief Unlocks the mutex
     */
    void Unlock();
    
    /**
     * @brief Gets the recursion count for the current thread
     * @return Number of times current thread has locked this mutex
     */
    uint32_t GetRecursionCount() const;

private:
    void* m_mutex;  ///< Platform-specific mutex
    void* m_condition;  ///< Platform-specific condition variable (if needed)
    std::atomic<uint32_t> m_ownerThreadId{0};  ///< Current owning thread
    std::atomic<uint32_t> m_recursionCount{0};  ///< Lock recursion count
};

/**
 * @class ScopedLock
 * @brief Generic RAII lock for any lockable object
 */
template<typename Lockable>
class ScopedLock
{
public:
    explicit ScopedLock(Lockable& lockable) : m_lockable(lockable), m_locked(true)
    {
        m_lockable.Lock();
    }
    
    ScopedLock(Lockable& lockable, std::adopt_lock_t) : 
        m_lockable(lockable), m_locked(true) 
    {
        
    }
    
    ~ScopedLock()
    {
        if (m_locked)
        {
            m_lockable.Unlock();
        }
    }
    
    
    ScopedLock(ScopedLock&& other) noexcept : 
        m_lockable(other.m_lockable), m_locked(other.m_locked)
    {
        other.m_locked = false;
    }
    
    ScopedLock& operator=(ScopedLock&& other) noexcept
    {
        if (this != &other)
        {
            if (m_locked)
            {
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
    void Unlock()
    {
        if (m_locked)
        {
            m_lockable.Unlock();
            m_locked = false;
        }
    }
    
    /**
     * @brief Re-lock the mutex
     */
    void Relock()
    {
        if (!m_locked)
        {
            m_lockable.Lock();
            m_locked = true;
        }
    }
    
    /**
     * @brief Check if the lock is currently held
     */
    [[nodiscard]] bool IsLocked() const { return m_locked; }

private:
    Lockable& m_lockable;
    bool m_locked;
};


using CriticalSection = RecursiveMutex;
using LockGuard = ScopedLock<RecursiveMutex>;