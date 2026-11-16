/**
 * @file Sysevents.hpp
 * @brief System event queue declarations.
 */
#pragma once
#include <cstdint>
#include <algorithm>
#include "../MainSYS.hpp"

/**
 * @class CSystemEvents
 * @brief Manages queued system events (interrupts, resets, pause/resume)
 * 
 * Events are queued by the CPU core or external triggers and processed
 * at safe points in the interpreter/recompiler execution loop (branch targets).
 * Based on Project64's CSystemEvents.
 */
class CSystemEvents {
public:
    static constexpr int MAX_EVENTS = 40;

    CSystemEvents(CN64System& system) : m_System(system), m_bDoSomething(false), m_Count(0) {
        m_Events.fill(SysEvent_CloseCPU);
    }

    /**
     * @brief Queue a system event for deferred processing
     * Events are processed at safe points (branch targets) during CPU execution.
     */
    void QueueEvent(SystemEvent action) {
        // Check for duplicates - don't queue the same event twice
        for (int i = 0; i < m_Count; i++) {
            if (m_Events[i] == action) {
                return; // Already queued
            }
        }

        if (m_Count < MAX_EVENTS) {
            m_Events[m_Count] = action;
            m_Count++;
            m_bDoSomething = true;
        }
    }

    /**
     * @brief Execute all pending events
     * Called from the interpreter/recompiler at branch targets when DoSomething() is true.
     */
    void ExecuteEvents() {
        // Copy events to local array in case processing triggers new events
        int count = m_Count;
        std::array<SystemEvent, MAX_EVENTS> events = m_Events;

        // Clear the queue before processing (new events can be queued during processing)
        m_Count = 0;
        m_bDoSomething = false;

        bool serviceStates = false;
        for (int i = 0; i < count; i++) {
            switch (events[i]) {
            case SysEvent_CloseCPU:      m_System.m_EndEmulation = true; break;
            case SysEvent_ResetCPU_Soft: m_System.GameReset(); break;
            case SysEvent_ResetCPU_Hard: m_System.Reset(true, true); break;
            case SysEvent_PauseCPU:      m_System.Pause(); break;
            case SysEvent_ResumeCPU:
                // Resume is handled by clearing the pause state
                break;
            case SysEvent_ExecuteInterrupt:
                if (m_System.m_Registers) {
                    m_System.m_Registers->DoIntrException();
                }
                break;
            case SysEvent_ServiceStates: serviceStates = true; break;
            }
        }
        // Last, so a snapshot sees the machine after every other event.
        if (serviceStates) {
            m_System.ServiceStates();
        }
    }

    /**
     * @brief Check if there are pending events to process
     */
    bool DoSomething() const { return m_bDoSomething; }

    /**
     * @brief Delete all pending events (used during reset)
     */
    void DeleteAllEvents() {
        m_Count = 0;
        m_bDoSomething = false;
    }

private:
    CN64System& m_System;
    bool m_bDoSomething;
    int m_Count;
    std::array<SystemEvent, MAX_EVENTS> m_Events;
};
