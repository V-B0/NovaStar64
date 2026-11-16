/**
 * @file RspLog.hpp
 * @brief RSP Logging.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>
#include <cstdarg>

namespace Nova64::RSP {

class CRSPSystem;

// --- CPU execution trace logging ---
void StartCPULog();
void StopCPULog();
void CPU_Message(const char* message, ...);

/// Writes display list commands and COP0 register accesses to RSP_RDP.log.
class CRDPLog {
public:
    explicit CRDPLog(CRSPSystem& system);
    ~CRDPLog();

    CRDPLog(const CRDPLog&) = delete;
    CRDPLog& operator=(const CRDPLog&) = delete;

    void StartLog();
    void StopLog();
    void Message(const char* message, ...);

    // Structured logging helpers
    void LogDlist();
    void LogMT0(uint32_t pc, int reg, uint32_t value);
    void LogMF0(uint32_t pc, int reg);

private:
    CRSPSystem& m_System;

    // Opaque log implementation
    class LogImpl;
    LogImpl* m_Impl{nullptr};

    // Register pointers for DPC state logging
    uint32_t*& m_DPC_START_REG;
    uint32_t*& m_DPC_END_REG;
    uint32_t*& m_DPC_CURRENT_REG;
    uint32_t*& m_DPC_STATUS_REG;
    uint32_t*& m_DPC_CLOCK_REG;
    uint8_t*& m_RDRAM;
    uint8_t*& m_DMEM;
};

} // namespace Nova64::RSP
