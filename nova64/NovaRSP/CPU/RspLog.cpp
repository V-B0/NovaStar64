/**
 * @file RspLog.cpp
 * @brief RSP Logging Implementation.
 */
// SuperNova64 - N64 Emulator
// RSP Logging Implementation
// CPU execution trace and RDP command logging

#include "RspLog.hpp"
#include "RspSystem.hpp"
#include <cstdarg>
#include <cstdio>
#include <memory>

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// CPU trace log implementation
// ──────────────────────────────────────────────

static FILE* s_CpuLogFile = nullptr;

void StartCPULog()
{
    if (s_CpuLogFile != nullptr) return;
    s_CpuLogFile = std::fopen("RSP_Cpu.log", "w");
}

void StopCPULog()
{
    if (s_CpuLogFile != nullptr)
    {
        std::fclose(s_CpuLogFile);
        s_CpuLogFile = nullptr;
    }
}

void CPU_Message(const char* message, ...)
{
    if (s_CpuLogFile == nullptr) return;

    va_list args;
    va_start(args, message);
    std::vfprintf(s_CpuLogFile, message, args);
    va_end(args);

    std::fputc('\n', s_CpuLogFile);
    std::fflush(s_CpuLogFile);
}

// ──────────────────────────────────────────────
// RDP Log - Opaque implementation
// ──────────────────────────────────────────────

class CRDPLog::LogImpl {
public:
    FILE* File{nullptr};

    ~LogImpl()
    {
        if (File != nullptr)
        {
            std::fclose(File);
            File = nullptr;
        }
    }
};

CRDPLog::CRDPLog(CRSPSystem& system)
    : m_System(system)
    , m_Impl(nullptr)
    , m_DPC_START_REG(system.m_DPC_START_REG)
    , m_DPC_END_REG(system.m_DPC_END_REG)
    , m_DPC_CURRENT_REG(system.m_DPC_CURRENT_REG)
    , m_DPC_STATUS_REG(system.m_DPC_STATUS_REG)
    , m_DPC_CLOCK_REG(system.m_DPC_CLOCK_REG)
    , m_RDRAM(system.m_RDRAM)
    , m_DMEM(system.m_DMEM)
{
}

CRDPLog::~CRDPLog()
{
    StopLog();
}

void CRDPLog::StartLog()
{
    if (m_Impl != nullptr) return;
    m_Impl = new LogImpl();
    m_Impl->File = std::fopen("RSP_RDP.log", "w");
}

void CRDPLog::StopLog()
{
    if (m_Impl != nullptr)
    {
        delete m_Impl;
        m_Impl = nullptr;
    }
}

void CRDPLog::Message(const char* message, ...)
{
    if (m_Impl == nullptr || m_Impl->File == nullptr) return;

    va_list args;
    va_start(args, message);
    std::vfprintf(m_Impl->File, message, args);
    va_end(args);

    std::fputc('\n', m_Impl->File);
    std::fflush(m_Impl->File);
}

void CRDPLog::LogDlist()
{
    if (m_Impl == nullptr || m_Impl->File == nullptr) return;
    if (m_DPC_START_REG == nullptr || m_DPC_END_REG == nullptr) return;

    const uint32_t start = *m_DPC_START_REG;
    const uint32_t end   = *m_DPC_END_REG;
    std::fprintf(m_Impl->File, "── DList [0x%08X → 0x%08X] ──\n", start, end);
    std::fflush(m_Impl->File);
}

void CRDPLog::LogMT0(uint32_t pc, int reg, uint32_t value)
{
    if (m_Impl == nullptr || m_Impl->File == nullptr) return;
    std::fprintf(m_Impl->File, "MTC0 @ 0x%03X  COP0[%d] = 0x%08X\n", pc, reg, value);
    std::fflush(m_Impl->File);
}

void CRDPLog::LogMF0(uint32_t pc, int reg)
{
    if (m_Impl == nullptr || m_Impl->File == nullptr) return;
    std::fprintf(m_Impl->File, "MFC0 @ 0x%03X  COP0[%d]\n", pc, reg);
    std::fflush(m_Impl->File);
}

} // namespace Nova64::RSP
