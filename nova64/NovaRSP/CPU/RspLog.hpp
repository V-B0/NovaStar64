// SuperNova64 - N64 Emulator
// RSP Logging - Modernized from Project64's RspLog.h
// C++20 with format strings and structured RDP logging

#pragma once
#include <cstdint>
#include <cstdarg>

namespace Nova64::RSP {

class CRSPSystem;

// ──────────────────────────────────────────────
// CPU execution trace logging
// ──────────────────────────────────────────────
void StartCPULog();
void StopCPULog();
void CPU_Message(const char* message, ...);

// ──────────────────────────────────────────────
// RDP Command Logger
// Logs display list commands, COP0 register access
// ──────────────────────────────────────────────
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
    uint8_t*&  m_RDRAM;
    uint8_t*&  m_DMEM;
};

} // namespace Nova64::RSP
