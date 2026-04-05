/**
 * @file RspMemory.cpp
 * @brief RSP Memory Implementation.
 */
// SuperNova64 - N64 Emulator
// RSP Memory Implementation
// IMEM/DMEM management and recompiler code buffer allocation

#include "RspMemory.hpp"
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

namespace Nova64::RSP {

// ──────────────────────────────────────────────
// Recompiler memory allocation
// Allocates executable code buffers + jump table
// ──────────────────────────────────────────────

static constexpr size_t CODE_BUF_SIZE      = 0x100000;  // 1 MB primary
static constexpr size_t CODE_BUF_SECONDARY = 0x100000;  // 1 MB secondary
static constexpr size_t JUMP_TABLE_SIZE    = (IMEM_SIZE / 4) * sizeof(void*);

bool AllocateRSPMemory(RecompilerMemory& mem)
{
    FreeRSPMemory(mem);

#if defined(_WIN32)
    mem.Code = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, CODE_BUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    mem.CodeSecondary = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, CODE_BUF_SECONDARY, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#else
    mem.Code = static_cast<uint8_t*>(
        mmap(nullptr, CODE_BUF_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mem.Code == MAP_FAILED) mem.Code = nullptr;

    mem.CodeSecondary = static_cast<uint8_t*>(
        mmap(nullptr, CODE_BUF_SECONDARY, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mem.CodeSecondary == MAP_FAILED) mem.CodeSecondary = nullptr;
#endif

    mem.JumpTable = static_cast<void**>(std::calloc(IMEM_SIZE / 4, sizeof(void*)));
    mem.Pos       = mem.Code;
    mem.Table     = 0;

    if (mem.Code == nullptr || mem.CodeSecondary == nullptr || mem.JumpTable == nullptr)
    {
        FreeRSPMemory(mem);
        return false;
    }

    return true;
}

void FreeRSPMemory(RecompilerMemory& mem)
{
#if defined(_WIN32)
    if (mem.Code != nullptr)
    {
        VirtualFree(mem.Code, 0, MEM_RELEASE);
        mem.Code = nullptr;
    }
    if (mem.CodeSecondary != nullptr)
    {
        VirtualFree(mem.CodeSecondary, 0, MEM_RELEASE);
        mem.CodeSecondary = nullptr;
    }
#else
    if (mem.Code != nullptr)
    {
        munmap(mem.Code, CODE_BUF_SIZE);
        mem.Code = nullptr;
    }
    if (mem.CodeSecondary != nullptr)
    {
        munmap(mem.CodeSecondary, CODE_BUF_SECONDARY);
        mem.CodeSecondary = nullptr;
    }
#endif

    if (mem.JumpTable != nullptr)
    {
        std::free(mem.JumpTable);
        mem.JumpTable = nullptr;
    }

    mem.Pos   = nullptr;
    mem.Table = 0;
}

} // namespace Nova64::RSP
