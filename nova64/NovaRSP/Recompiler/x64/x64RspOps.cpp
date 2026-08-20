/**
 * @file x64RspOps.cpp
 * @brief x64 RSP Operations Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "x64RspOps.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Nova64::RSP::Recompiler::x64 {

// ──────────────────────────────────────────────
// Initialization
// ──────────────────────────────────────────────

void CX64RspOps::Initialize(uint8_t* buffer, size_t size)
{
    m_Buffer = buffer;
    m_Pos = buffer;
    m_BufferSize = size;
    m_Stats.Reset();
}

void CX64RspOps::Reset()
{
    m_Pos = m_Buffer;
    m_Stats.Reset();
}

// ──────────────────────────────────────────────
// Raw byte emission
// ──────────────────────────────────────────────

void CX64RspOps::EmitByte(uint8_t b)
{
    if (m_Pos && HasSpace(1))
        *m_Pos++ = b;
}

void CX64RspOps::EmitWord(uint16_t w)
{
    if (m_Pos && HasSpace(2))
    {
        std::memcpy(m_Pos, &w, 2);
        m_Pos += 2;
    }
}

void CX64RspOps::EmitDword(uint32_t d)
{
    if (m_Pos && HasSpace(4))
    {
        std::memcpy(m_Pos, &d, 4);
        m_Pos += 4;
    }
}

void CX64RspOps::EmitQword(uint64_t q)
{
    if (m_Pos && HasSpace(8))
    {
        std::memcpy(m_Pos, &q, 8);
        m_Pos += 8;
    }
}

// ──────────────────────────────────────────────
// Encoding building blocks
// ──────────────────────────────────────────────

void CX64RspOps::EmitRex(bool w, uint8_t reg, uint8_t idx, uint8_t rm)
{
    uint8_t rex = 0x40;
    if (w)        rex |= 0x08;
    if (reg >= 8) rex |= 0x04;
    if (idx >= 8) rex |= 0x02;
    if (rm >= 8)  rex |= 0x01;
    // Only emit REX if needed (W=1 or any high register)
    if (rex != 0x40 || w)
        EmitByte(rex);
}

void CX64RspOps::EmitModRM(uint8_t mod, uint8_t reg, uint8_t rm)
{
    EmitByte(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

void CX64RspOps::EmitSIB(uint8_t scale, uint8_t idx, uint8_t base)
{
    EmitByte(static_cast<uint8_t>((scale << 6) | ((idx & 7) << 3) | (base & 7)));
}

// Emit memory operand via absolute address (RIP-relative not used; we use movabs pattern)
void CX64RspOps::EmitMemOp(uint8_t reg, void* addr)
{
    // Use R11 as scratch to hold address, then access via [R11]
    // mov r11, imm64
    EmitByte(0x49);                  // REX.WB
    EmitByte(0xBB);                  // MOV R11, imm64
    EmitQword(reinterpret_cast<uint64_t>(addr));
    // ModRM: reg, [R11] (R11 = 0b011 in ModRM.rm w/ REX.B)
    // This is handled by caller who adds the opcode + ModRM
}

void CX64RspOps::EmitBaseDispOp(uint8_t reg, GpReg base, int32_t disp)
{
    uint8_t baseIdx = static_cast<uint8_t>(base);
    if (disp == 0 && (baseIdx & 7) != 5) // RBP/R13 always needs disp8
    {
        EmitModRM(0, reg, baseIdx & 7);
        if ((baseIdx & 7) == 4) // RSP/R12 needs SIB
            EmitSIB(0, 4, baseIdx & 7);
    }
    else if (disp >= -128 && disp <= 127)
    {
        EmitModRM(1, reg, baseIdx & 7);
        if ((baseIdx & 7) == 4)
            EmitSIB(0, 4, baseIdx & 7);
        EmitByte(static_cast<uint8_t>(disp));
    }
    else
    {
        EmitModRM(2, reg, baseIdx & 7);
        if ((baseIdx & 7) == 4)
            EmitSIB(0, 4, baseIdx & 7);
        EmitDword(static_cast<uint32_t>(disp));
    }
}

// SSE2 register-register operation: [prefix] 0F opcode ModRM(3, dst, src)
void CX64RspOps::EmitSSE2Op(uint8_t prefix, uint8_t op1, uint8_t op2,
                             XmmReg dst, XmmReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    if (prefix) EmitByte(prefix);
    EmitRex(false, d, 0, s);
    EmitByte(0x0F);
    if (op1) EmitByte(op1);  // Extended opcode byte (for 3-byte opcodes: 0x38/0x3A)
    EmitByte(op2);
    EmitModRM(3, d, s);
}

void CX64RspOps::EmitSSE2MemOp(uint8_t prefix, uint8_t op1, uint8_t op2,
                                XmmReg xmm, void* addr)
{
    uint8_t x = static_cast<uint8_t>(xmm);
    // Load address into R11, then use [R11]
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    if (prefix) EmitByte(prefix);
    // REX for xmm >= 8 and R11 (rm = 3, with REX.B set via 0x41 pattern)
    uint8_t rex = 0x40;
    if (x >= 8) rex |= 0x04;
    rex |= 0x01; // REX.B for R11
    if (rex != 0x40) EmitByte(rex);
    EmitByte(0x0F);
    if (op1) EmitByte(op1);
    EmitByte(op2);
    EmitModRM(0, x & 7, 3); // [R11]
}

void CX64RspOps::EmitSSE2BaseDispOp(uint8_t prefix, uint8_t op1, uint8_t op2,
                                     XmmReg xmm, GpReg base, int32_t disp)
{
    uint8_t x = static_cast<uint8_t>(xmm);
    uint8_t b = static_cast<uint8_t>(base);
    if (prefix) EmitByte(prefix);
    EmitRex(false, x, 0, b);
    EmitByte(0x0F);
    if (op1) EmitByte(op1);
    EmitByte(op2);
    EmitBaseDispOp(x & 7, base, disp);
}

void CX64RspOps::LogInstr(const char* fmt, ...)
{
    if (!m_Log) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    *m_Log += buf;
}

// ══════════════════════════════════════════════
// General-purpose register instructions
// ══════════════════════════════════════════════

void CX64RspOps::MovRegImm32(GpReg dst, uint32_t imm)
{
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0xB8 + (d & 7));
    EmitDword(imm);
    m_Stats.GpInstructions++;
    LogInstr("    mov     %s, 0x%X\n", "r??", imm);
}

void CX64RspOps::MovRegImm64(GpReg dst, uint64_t imm)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(true, 0, 0, d);
    EmitByte(0xB8 + (d & 7));
    EmitQword(imm);
    m_Stats.GpInstructions++;
}

void CX64RspOps::MovRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(true, s, 0, d);
    EmitByte(0x89);
    EmitModRM(3, s, d);
    m_Stats.GpInstructions++;
}

void CX64RspOps::MovRegMem(GpReg dst, void* addr)
{
    uint8_t d = static_cast<uint8_t>(dst);
    // mov r11, addr; mov dst, [r11]
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    EmitRex(false, d, 0, 11);
    EmitByte(0x8B);
    EmitModRM(0, d & 7, 3);
    m_Stats.MemoryInstructions++;
}

void CX64RspOps::MovMemReg(void* addr, GpReg src)
{
    uint8_t s = static_cast<uint8_t>(src);
    // mov r11, addr; mov [r11], src
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    EmitRex(false, s, 0, 11);
    EmitByte(0x89);
    EmitModRM(0, s & 7, 3);
    m_Stats.MemoryInstructions++;
}

void CX64RspOps::MovMemImm32(void* addr, uint32_t imm)
{
    // mov r11, addr; mov dword [r11], imm
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    EmitByte(0x41); EmitByte(0xC7);
    EmitModRM(0, 0, 3);
    EmitDword(imm);
    m_Stats.MemoryInstructions++;
}

void CX64RspOps::AddRegImm32(GpReg dst, int32_t imm)
{
    uint8_t d = static_cast<uint8_t>(dst);
    if (imm >= -128 && imm <= 127)
    {
        EmitRex(false, 0, 0, d);
        EmitByte(0x83);
        EmitModRM(3, 0, d & 7);
        EmitByte(static_cast<uint8_t>(imm));
    }
    else
    {
        if (d == static_cast<uint8_t>(GpReg::RAX))
        {
            EmitByte(0x05);
            EmitDword(static_cast<uint32_t>(imm));
        }
        else
        {
            EmitRex(false, 0, 0, d);
            EmitByte(0x81);
            EmitModRM(3, 0, d & 7);
            EmitDword(static_cast<uint32_t>(imm));
        }
    }
    m_Stats.GpInstructions++;
}

void CX64RspOps::AddRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, s, 0, d);
    EmitByte(0x01);
    EmitModRM(3, s, d);
    m_Stats.GpInstructions++;
}

void CX64RspOps::AddRegMem(GpReg dst, void* addr)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    EmitRex(false, d, 0, 11);
    EmitByte(0x03);
    EmitModRM(0, d & 7, 3);
    m_Stats.GpInstructions++;
}

void CX64RspOps::SubRegImm32(GpReg dst, int32_t imm)
{
    uint8_t d = static_cast<uint8_t>(dst);
    if (imm >= -128 && imm <= 127)
    {
        EmitRex(false, 0, 0, d);
        EmitByte(0x83);
        EmitModRM(3, 5, d & 7);
        EmitByte(static_cast<uint8_t>(imm));
    }
    else
    {
        EmitRex(false, 0, 0, d);
        EmitByte(0x81);
        EmitModRM(3, 5, d & 7);
        EmitDword(static_cast<uint32_t>(imm));
    }
    m_Stats.GpInstructions++;
}

void CX64RspOps::SubRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, s, 0, d);
    EmitByte(0x29);
    EmitModRM(3, s, d);
    m_Stats.GpInstructions++;
}

void CX64RspOps::AndRegImm32(GpReg dst, uint32_t imm)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0x81);
    EmitModRM(3, 4, d & 7);
    EmitDword(imm);
    m_Stats.GpInstructions++;
}

void CX64RspOps::AndRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, s, 0, d);
    EmitByte(0x21);
    EmitModRM(3, s, d);
    m_Stats.GpInstructions++;
}

void CX64RspOps::OrRegImm32(GpReg dst, uint32_t imm)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0x81);
    EmitModRM(3, 1, d & 7);
    EmitDword(imm);
    m_Stats.GpInstructions++;
}

void CX64RspOps::OrRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, s, 0, d);
    EmitByte(0x09);
    EmitModRM(3, s, d);
    m_Stats.GpInstructions++;
}

void CX64RspOps::XorRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, s, 0, d);
    EmitByte(0x31);
    EmitModRM(3, s, d);
    m_Stats.GpInstructions++;
}

void CX64RspOps::NotReg(GpReg reg)
{
    uint8_t r = static_cast<uint8_t>(reg);
    EmitRex(false, 0, 0, r);
    EmitByte(0xF7);
    EmitModRM(3, 2, r & 7);
    m_Stats.GpInstructions++;
}

void CX64RspOps::NegReg(GpReg reg)
{
    uint8_t r = static_cast<uint8_t>(reg);
    EmitRex(false, 0, 0, r);
    EmitByte(0xF7);
    EmitModRM(3, 3, r & 7);
    m_Stats.GpInstructions++;
}

void CX64RspOps::ImulRegReg(GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, d, 0, s);
    EmitByte(0x0F);
    EmitByte(0xAF);
    EmitModRM(3, d, s);
    m_Stats.GpInstructions++;
}

void CX64RspOps::ShlRegImm8(GpReg dst, uint8_t count)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0xC1);
    EmitModRM(3, 4, d & 7);
    EmitByte(count);
    m_Stats.GpInstructions++;
}

void CX64RspOps::ShrRegImm8(GpReg dst, uint8_t count)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0xC1);
    EmitModRM(3, 5, d & 7);
    EmitByte(count);
    m_Stats.GpInstructions++;
}

void CX64RspOps::SarRegImm8(GpReg dst, uint8_t count)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0xC1);
    EmitModRM(3, 7, d & 7);
    EmitByte(count);
    m_Stats.GpInstructions++;
}

void CX64RspOps::ShlRegCl(GpReg dst)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0xD3);
    EmitModRM(3, 4, d & 7);
    m_Stats.GpInstructions++;
}

void CX64RspOps::ShrRegCl(GpReg dst)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0xD3);
    EmitModRM(3, 5, d & 7);
    m_Stats.GpInstructions++;
}

void CX64RspOps::SarRegCl(GpReg dst)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0xD3);
    EmitModRM(3, 7, d & 7);
    m_Stats.GpInstructions++;
}

// ── Compare / Test ──

void CX64RspOps::CmpRegImm32(GpReg reg, uint32_t imm)
{
    uint8_t r = static_cast<uint8_t>(reg);
    if (r == static_cast<uint8_t>(GpReg::RAX))
    {
        EmitByte(0x3D);
        EmitDword(imm);
    }
    else
    {
        EmitRex(false, 0, 0, r);
        EmitByte(0x81);
        EmitModRM(3, 7, r & 7);
        EmitDword(imm);
    }
    m_Stats.GpInstructions++;
}

void CX64RspOps::CmpRegReg(GpReg r1, GpReg r2)
{
    uint8_t a = static_cast<uint8_t>(r1);
    uint8_t b = static_cast<uint8_t>(r2);
    EmitRex(false, b, 0, a);
    EmitByte(0x39);
    EmitModRM(3, b, a);
    m_Stats.GpInstructions++;
}

void CX64RspOps::CmpRegMem(GpReg reg, void* addr)
{
    uint8_t r = static_cast<uint8_t>(reg);
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    EmitRex(false, r, 0, 11);
    EmitByte(0x3B);
    EmitModRM(0, r & 7, 3);
    m_Stats.GpInstructions++;
}

void CX64RspOps::CmpMemImm32(void* addr, uint32_t imm)
{
    EmitByte(0x49); EmitByte(0xBB);
    EmitQword(reinterpret_cast<uint64_t>(addr));
    EmitByte(0x41); EmitByte(0x81);
    EmitModRM(0, 7, 3);
    EmitDword(imm);
    m_Stats.GpInstructions++;
}

void CX64RspOps::TestRegReg(GpReg r1, GpReg r2)
{
    uint8_t a = static_cast<uint8_t>(r1);
    uint8_t b = static_cast<uint8_t>(r2);
    EmitRex(false, b, 0, a);
    EmitByte(0x85);
    EmitModRM(3, b, a);
    m_Stats.GpInstructions++;
}

void CX64RspOps::TestRegImm32(GpReg reg, uint32_t imm)
{
    uint8_t r = static_cast<uint8_t>(reg);
    if (r == static_cast<uint8_t>(GpReg::RAX))
    {
        EmitByte(0xA9);
        EmitDword(imm);
    }
    else
    {
        EmitRex(false, 0, 0, r);
        EmitByte(0xF7);
        EmitModRM(3, 0, r & 7);
        EmitDword(imm);
    }
    m_Stats.GpInstructions++;
}

// ── SETcc ──

void CX64RspOps::Setcc(Condition cc, GpReg dst)
{
    uint8_t d = static_cast<uint8_t>(dst);
    EmitRex(false, 0, 0, d);
    EmitByte(0x0F);
    EmitByte(0x90 + static_cast<uint8_t>(cc));
    EmitModRM(3, 0, d & 7);
    m_Stats.GpInstructions++;
}

// ── CMOVcc ──

void CX64RspOps::Cmovcc(Condition cc, GpReg dst, GpReg src)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t s = static_cast<uint8_t>(src);
    EmitRex(false, d, 0, s);
    EmitByte(0x0F);
    EmitByte(0x40 + static_cast<uint8_t>(cc));
    EmitModRM(3, d, s);
    m_Stats.GpInstructions++;
}

// ── Stack ──

void CX64RspOps::Push(GpReg reg)
{
    uint8_t r = static_cast<uint8_t>(reg);
    if (r >= 8) EmitByte(0x41);
    EmitByte(0x50 + (r & 7));
    m_Stats.GpInstructions++;
}

void CX64RspOps::Pop(GpReg reg)
{
    uint8_t r = static_cast<uint8_t>(reg);
    if (r >= 8) EmitByte(0x41);
    EmitByte(0x58 + (r & 7));
    m_Stats.GpInstructions++;
}

// ── Jumps & Calls ──

void CX64RspOps::Jcc(Condition cc, int32_t rel32)
{
    EmitByte(0x0F);
    EmitByte(0x80 + static_cast<uint8_t>(cc));
    EmitDword(static_cast<uint32_t>(rel32));
    m_Stats.BranchInstructions++;
}

void CX64RspOps::JmpRel32(int32_t rel32)
{
    EmitByte(0xE9);
    EmitDword(static_cast<uint32_t>(rel32));
    m_Stats.BranchInstructions++;
}

void CX64RspOps::JmpReg(GpReg reg)
{
    uint8_t r = static_cast<uint8_t>(reg);
    if (r >= 8) EmitByte(0x41);
    EmitByte(0xFF);
    EmitModRM(3, 4, r & 7);
    m_Stats.BranchInstructions++;
}

void CX64RspOps::CallReg(GpReg reg)
{
    uint8_t r = static_cast<uint8_t>(reg);
    if (r >= 8) EmitByte(0x41);
    EmitByte(0xFF);
    EmitModRM(3, 2, r & 7);
    m_Stats.BranchInstructions++;
}

void CX64RspOps::CallAbs(void* addr)
{
    MovRegImm64(GpReg::RAX, reinterpret_cast<uint64_t>(addr));
    CallReg(GpReg::RAX);
}

void CX64RspOps::Ret()
{
    EmitByte(0xC3);
    m_Stats.BranchInstructions++;
}

// ── LEA ──

void CX64RspOps::LeaRegMemDisp(GpReg dst, GpReg base, int32_t disp)
{
    uint8_t d = static_cast<uint8_t>(dst);
    uint8_t b = static_cast<uint8_t>(base);
    EmitRex(true, d, 0, b);
    EmitByte(0x8D);
    EmitBaseDispOp(d & 7, base, disp);
    m_Stats.GpInstructions++;
}

// ══════════════════════════════════════════════
// SSE2 packed 128-bit instructions
// ══════════════════════════════════════════════

// ── Data movement ──

void CX64RspOps::Movdqa_XmmXmm(XmmReg dst, XmmReg src)
{
    EmitSSE2Op(0x66, 0, 0x6F, dst, src);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movdqa_XmmMem(XmmReg dst, void* addr)
{
    EmitSSE2MemOp(0x66, 0, 0x6F, dst, addr);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movdqa_MemXmm(void* addr, XmmReg src)
{
    EmitSSE2MemOp(0x66, 0, 0x7F, src, addr);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movdqu_XmmMem(XmmReg dst, void* addr)
{
    EmitSSE2MemOp(0xF3, 0, 0x6F, dst, addr);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movdqu_MemXmm(void* addr, XmmReg src)
{
    EmitSSE2MemOp(0xF3, 0, 0x7F, src, addr);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movd_XmmReg(XmmReg dst, GpReg src)
{
    uint8_t x = static_cast<uint8_t>(dst);
    uint8_t g = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(false, x, 0, g);
    EmitByte(0x0F); EmitByte(0x6E);
    EmitModRM(3, x, g);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movd_RegXmm(GpReg dst, XmmReg src)
{
    uint8_t g = static_cast<uint8_t>(dst);
    uint8_t x = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(false, x, 0, g);
    EmitByte(0x0F); EmitByte(0x7E);
    EmitModRM(3, x, g);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movq_XmmReg(XmmReg dst, GpReg src)
{
    uint8_t x = static_cast<uint8_t>(dst);
    uint8_t g = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(true, x, 0, g);
    EmitByte(0x0F); EmitByte(0x6E);
    EmitModRM(3, x, g);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movq_RegXmm(GpReg dst, XmmReg src)
{
    uint8_t g = static_cast<uint8_t>(dst);
    uint8_t x = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(true, x, 0, g);
    EmitByte(0x0F); EmitByte(0x7E);
    EmitModRM(3, x, g);
    m_Stats.SimdInstructions++;
}

// ── XMM zeroing ──

void CX64RspOps::Pxor_XmmXmm(XmmReg dst, XmmReg src)
{
    EmitSSE2Op(0x66, 0, 0xEF, dst, src);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Xorps_XmmXmm(XmmReg dst, XmmReg src)
{
    EmitSSE2Op(0, 0, 0x57, dst, src);
    m_Stats.SimdInstructions++;
}

// ── Packed 16-bit signed arithmetic ──

void CX64RspOps::Paddw_XmmXmm(XmmReg dst, XmmReg src)    { EmitSSE2Op(0x66, 0, 0xFD, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Paddsw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0, 0xED, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Paddusw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0xDD, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Psubw_XmmXmm(XmmReg dst, XmmReg src)    { EmitSSE2Op(0x66, 0, 0xF9, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Psubsw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0, 0xE9, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Psubusw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0xD9, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmullw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0, 0xD5, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmulhw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0, 0xE5, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmulhuw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0xE4, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmaddwd_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0xF5, dst, src); m_Stats.SimdInstructions++; }

// ── Packed min/max ──

void CX64RspOps::Pminsw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0, 0xEA, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmaxsw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0, 0xEE, dst, src); m_Stats.SimdInstructions++; }

// SSE4.1 instructions
void CX64RspOps::Pminuw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0x38, 0x3A, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmaxuw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0x38, 0x3E, dst, src); m_Stats.SimdInstructions++; }

// ── Compare ──

void CX64RspOps::Pcmpeqw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0x75, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pcmpgtw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0x65, dst, src); m_Stats.SimdInstructions++; }

// ── Bitwise ──

void CX64RspOps::Pand_XmmXmm(XmmReg dst, XmmReg src)     { EmitSSE2Op(0x66, 0, 0xDB, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pandn_XmmXmm(XmmReg dst, XmmReg src)    { EmitSSE2Op(0x66, 0, 0xDF, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Por_XmmXmm(XmmReg dst, XmmReg src)      { EmitSSE2Op(0x66, 0, 0xEB, dst, src); m_Stats.SimdInstructions++; }

void CX64RspOps::Pxor_XmmMem(XmmReg dst, void* addr)
{
    EmitSSE2MemOp(0x66, 0, 0xEF, dst, addr);
    m_Stats.SimdInstructions++;
}

// ── Packed shifts (16-bit) ──

void CX64RspOps::Psllw_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x71);
    EmitModRM(3, 6, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Psrlw_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x71);
    EmitModRM(3, 2, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Psraw_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x71);
    EmitModRM(3, 4, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

// ── Packed shifts (32-bit) ──

void CX64RspOps::Pslld_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x72);
    EmitModRM(3, 6, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Psrld_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x72);
    EmitModRM(3, 2, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Psrad_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x72);
    EmitModRM(3, 4, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

// ── Packed shifts (64-bit) ──

void CX64RspOps::Psllq_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x73);
    EmitModRM(3, 6, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Psrlq_XmmImm8(XmmReg dst, uint8_t count)
{
    EmitByte(0x66);
    uint8_t d = static_cast<uint8_t>(dst);
    if (d >= 8) EmitByte(0x41);
    EmitByte(0x0F); EmitByte(0x73);
    EmitModRM(3, 2, d & 7);
    EmitByte(count);
    m_Stats.SimdInstructions++;
}

// ── Shuffle / Interleave / Pack ──

void CX64RspOps::Pshuflw_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t imm)
{
    EmitSSE2Op(0xF2, 0, 0x70, dst, src);
    EmitByte(imm);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Pshufhw_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t imm)
{
    EmitSSE2Op(0xF3, 0, 0x70, dst, src);
    EmitByte(imm);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Pshufd_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t imm)
{
    EmitSSE2Op(0x66, 0, 0x70, dst, src);
    EmitByte(imm);
    m_Stats.SimdInstructions++;
}

// SSSE3: pshufb
void CX64RspOps::Pshufb_XmmXmm(XmmReg dst, XmmReg src)
{
    EmitSSE2Op(0x66, 0x38, 0x00, dst, src);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Punpcklwd_XmmXmm(XmmReg dst, XmmReg src) { EmitSSE2Op(0x66, 0, 0x61, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Punpckhwd_XmmXmm(XmmReg dst, XmmReg src) { EmitSSE2Op(0x66, 0, 0x69, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Punpckldq_XmmXmm(XmmReg dst, XmmReg src) { EmitSSE2Op(0x66, 0, 0x62, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Punpckhdq_XmmXmm(XmmReg dst, XmmReg src) { EmitSSE2Op(0x66, 0, 0x6A, dst, src); m_Stats.SimdInstructions++; }

void CX64RspOps::Packsswb_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0x63, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Packuswb_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0x67, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Packssdw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0, 0x6B, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Packusdw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0x38, 0x2B, dst, src); m_Stats.SimdInstructions++; }

// ── Blend (SSE4.1) ──

void CX64RspOps::Pblendw_XmmXmmImm8(XmmReg dst, XmmReg src, uint8_t mask)
{
    EmitSSE2Op(0x66, 0x3A, 0x0E, dst, src);
    EmitByte(mask);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Blendvps_XmmXmm(XmmReg dst, XmmReg src)
{
    // blendvps uses implicit xmm0 as blend mask
    EmitSSE2Op(0x66, 0x38, 0x14, dst, src);
    m_Stats.SimdInstructions++;
}

// ── Extract / Insert ──

void CX64RspOps::Pextrw_RegXmm(GpReg dst, XmmReg src, uint8_t idx)
{
    uint8_t g = static_cast<uint8_t>(dst);
    uint8_t x = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(false, g, 0, x);
    EmitByte(0x0F); EmitByte(0xC5);
    EmitModRM(3, g, x);
    EmitByte(idx);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Pinsrw_XmmRegImm8(XmmReg dst, GpReg src, uint8_t idx)
{
    uint8_t x = static_cast<uint8_t>(dst);
    uint8_t g = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(false, x, 0, g);
    EmitByte(0x0F); EmitByte(0xC4);
    EmitModRM(3, x, g);
    EmitByte(idx);
    m_Stats.SimdInstructions++;
}

// ── Horizontal operations (SSSE3) ──

void CX64RspOps::Phaddw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0x38, 0x01, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Phsubw_XmmXmm(XmmReg dst, XmmReg src)   { EmitSSE2Op(0x66, 0x38, 0x05, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Phaddsw_XmmXmm(XmmReg dst, XmmReg src)  { EmitSSE2Op(0x66, 0x38, 0x03, dst, src); m_Stats.SimdInstructions++; }

// ── Absolute value (SSSE3) ──

void CX64RspOps::Pabsw_XmmXmm(XmmReg dst, XmmReg src)    { EmitSSE2Op(0x66, 0x38, 0x1D, dst, src); m_Stats.SimdInstructions++; }

// ── Sign extension (SSE4.1) ──

void CX64RspOps::Pmovsxbw_XmmXmm(XmmReg dst, XmmReg src) { EmitSSE2Op(0x66, 0x38, 0x20, dst, src); m_Stats.SimdInstructions++; }
void CX64RspOps::Pmovzxbw_XmmXmm(XmmReg dst, XmmReg src) { EmitSSE2Op(0x66, 0x38, 0x30, dst, src); m_Stats.SimdInstructions++; }

// ── Miscellaneous ──

void CX64RspOps::Pmovmskb_RegXmm(GpReg dst, XmmReg src)
{
    uint8_t g = static_cast<uint8_t>(dst);
    uint8_t x = static_cast<uint8_t>(src);
    EmitByte(0x66);
    EmitRex(false, g, 0, x);
    EmitByte(0x0F); EmitByte(0xD7);
    EmitModRM(3, g, x);
    m_Stats.SimdInstructions++;
}

// ── Base+displacement SIMD ──

void CX64RspOps::Movdqa_XmmBaseDisp(XmmReg dst, GpReg base, int32_t disp)
{
    EmitSSE2BaseDispOp(0x66, 0, 0x6F, dst, base, disp);
    m_Stats.SimdInstructions++;
}

void CX64RspOps::Movdqa_BaseDispXmm(GpReg base, int32_t disp, XmmReg src)
{
    EmitSSE2BaseDispOp(0x66, 0, 0x7F, src, base, disp);
    m_Stats.SimdInstructions++;
}

} // namespace Nova64::RSP::Recompiler::x64

#endif // x64 only
