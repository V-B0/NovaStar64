/**
 * @file RspAssembler.cpp
 * @brief RSP Assembler Implementation.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#include "RspAssembler.hpp"

#if defined(__amd64__) || defined(_M_X64)

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// x64 register indices
// ──────────────────────────────────────────────
namespace x64 {
    constexpr uint8_t RAX = 0;
    constexpr uint8_t RCX = 1;
    constexpr uint8_t RDX = 2;
    constexpr uint8_t RBX = 3;
    constexpr uint8_t RSP = 4;
    constexpr uint8_t RBP = 5;
    constexpr uint8_t RSI = 6;
    constexpr uint8_t RDI = 7;
    constexpr uint8_t R8  = 8;
    constexpr uint8_t R9  = 9;
    constexpr uint8_t R10 = 10;
    constexpr uint8_t R11 = 11;
    constexpr uint8_t R12 = 12;
    constexpr uint8_t R13 = 13;
    constexpr uint8_t R14 = 14;
    constexpr uint8_t R15 = 15;
}

// REX prefix byte builder
static uint8_t Rex(bool w, bool r, bool x, bool b)
{
    return static_cast<uint8_t>(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
}

// ModR/M byte builder
static uint8_t ModRM(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

// ──────────────────────────────────────────────
// Constructor / destructor
// ──────────────────────────────────────────────

RspAssembler::RspAssembler(uint8_t* codeBuffer, size_t bufferSize, std::string& codeLog)
    : m_Buffer(codeBuffer)
    , m_Pos(codeBuffer)
    , m_BufferSize(bufferSize)
    , m_CodeLog(codeLog)
{
    m_PrimarySection = codeBuffer;
}

RspAssembler::~RspAssembler() = default;

// ──────────────────────────────────────────────
// Reset / section management
// ──────────────────────────────────────────────

void RspAssembler::Reset()
{
    m_Pos = m_Buffer;
    m_LabelSymbols.clear();
    m_NumberSymbols.clear();
    m_NextLabelId = 1;
    m_CodeLog.clear();
}

void RspAssembler::SetPrimarySection()
{
    m_SecondarySection = m_Pos;
    m_Pos = m_PrimarySection;
}

void RspAssembler::SetSecondarySection()
{
    m_PrimarySection = m_Pos;
    m_Pos = m_SecondarySection;
}

// ──────────────────────────────────────────────
// Helper: emit bytes
// ──────────────────────────────────────────────

static void Emit8(uint8_t*& pos, uint8_t val)
{
    *pos++ = val;
}

static void Emit32(uint8_t*& pos, uint32_t val)
{
    std::memcpy(pos, &val, 4);
    pos += 4;
}

static void Emit64(uint8_t*& pos, uint64_t val)
{
    std::memcpy(pos, &val, 8);
    pos += 8;
}

// ──────────────────────────────────────────────
// Label management
// ──────────────────────────────────────────────

AsmLabel RspAssembler::CreateLabel()
{
    AsmLabel label;
    label.id = m_NextLabelId++;
    label.valid = false;
    return label;
}

void RspAssembler::BindLabel(AsmLabel& label)
{
    label.valid = true;
    // Store current position as label target
    auto key = static_cast<uint64_t>(label.id);
    m_LabelSymbols[key] = "L" + std::to_string(label.id);
}

void RspAssembler::AddLabelSymbol(const AsmLabel& label, const char* symbol)
{
    auto key = static_cast<uint64_t>(label.id);
    m_LabelSymbols[key] = symbol;
}

void RspAssembler::AddNumberSymbol(uint64_t value, const char* symbol)
{
    auto& entry = m_NumberSymbols[value];
    entry.Symbol = symbol;
    entry.Count++;
}

// ──────────────────────────────────────────────
// Call emission
// ──────────────────────────────────────────────

void RspAssembler::CallFunc(void* funcPtr, const char* funcName)
{
    // mov rax, funcPtr
    Emit8(m_Pos, Rex(true, false, false, false));  // REX.W
    Emit8(m_Pos, 0xB8 + (x64::RAX & 7));           // MOV RAX, imm64
    Emit64(m_Pos, reinterpret_cast<uint64_t>(funcPtr));
    // call rax
    Emit8(m_Pos, 0xFF);
    Emit8(m_Pos, ModRM(3, 2, x64::RAX));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    call    %s\n", funcName);
    m_CodeLog += buf;
}

void RspAssembler::CallThis(void* thisPtr, void* funcPtr, const char* funcName)
{
    // Move 'this' into RCX (MSVC x64 calling convention)
    // mov rcx, thisPtr
    Emit8(m_Pos, Rex(true, false, false, false));
    Emit8(m_Pos, 0xB8 + (x64::RCX & 7));
    Emit64(m_Pos, reinterpret_cast<uint64_t>(thisPtr));

    // call funcPtr
    CallFunc(funcPtr, funcName);
}

// ──────────────────────────────────────────────
// Compare
// ──────────────────────────────────────────────

void RspAssembler::CompConstToVariable(void* variable, const char* varName, uint32_t constant)
{
    // mov rax, [variable]
    Emit8(m_Pos, Rex(true, false, false, false));
    Emit8(m_Pos, 0xA1);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    // cmp eax, constant
    Emit8(m_Pos, 0x3D);
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    cmp     dword [%s], 0x%X\n", varName, constant);
    m_CodeLog += buf;
}

void RspAssembler::CompX86regToVariable(void* variable, const char* varName, uint8_t reg)
{
    // cmp reg, [variable]  (via movabs + cmp)
    Emit8(m_Pos, Rex(true, false, false, reg >= 8));
    Emit8(m_Pos, 0xB8 + (x64::R11 & 7));
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, Rex(false, reg >= 8, false, false));
    Emit8(m_Pos, 0x3B);
    Emit8(m_Pos, ModRM(0, reg & 7, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    cmp     reg%u, [%s]\n", reg, varName);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Conditional jumps (forward patching via label)
// ──────────────────────────────────────────────

static void EmitJcc(uint8_t*& pos, uint8_t cc, const char* mnemonic,
                    const char* labelName, AsmLabel& label, std::string& log)
{
    Emit8(pos, 0x0F);
    Emit8(pos, cc);
    Emit32(pos, 0);  // Placeholder for rel32 (patched at BindLabel)

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    %-7s %s\n", mnemonic, labelName);
    log += buf;

    (void)label;  // Label patching handled externally
}

void RspAssembler::JeLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x84, "je", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JgLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x8F, "jg", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JleLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x8E, "jle", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JmpLabel(const char* labelName, AsmLabel& jumpLabel)
{
    Emit8(m_Pos, 0xE9);
    Emit32(m_Pos, 0);  // rel32 placeholder

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    jmp     %s\n", labelName);
    m_CodeLog += buf;

    (void)jumpLabel;
}

void RspAssembler::JneLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x85, "jne", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JFunc(void* funcPtr, const char* funcName)
{
    // mov rax, funcPtr; jmp rax
    Emit8(m_Pos, Rex(true, false, false, false));
    Emit8(m_Pos, 0xB8 + (x64::RAX & 7));
    Emit64(m_Pos, reinterpret_cast<uint64_t>(funcPtr));
    Emit8(m_Pos, 0xFF);
    Emit8(m_Pos, ModRM(3, 4, x64::RAX));  // jmp rax

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    jmp     %s\n", funcName);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Move operations
// ──────────────────────────────────────────────

void RspAssembler::MoveConstToVariable(void* variable, const char* varName, uint32_t constant)
{
    // mov r11, variable; mov dword [r11], constant
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, 0x41);
    Emit8(m_Pos, 0xC7);
    Emit8(m_Pos, ModRM(0, 0, x64::R11 & 7));
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     dword [%s], 0x%X\n", varName, constant);
    m_CodeLog += buf;
}

void RspAssembler::MoveVariableToX86reg(uint8_t reg, void* variable, const char* varName)
{
    // mov r11, variable; mov reg, [r11]
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, Rex(false, reg >= 8, false, true));
    Emit8(m_Pos, 0x8B);
    Emit8(m_Pos, ModRM(0, reg & 7, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     reg%u, dword [%s]\n", reg, varName);
    m_CodeLog += buf;
}

void RspAssembler::MoveX86regToVariable(void* variable, const char* varName, uint8_t reg)
{
    // mov r11, variable; mov [r11], reg
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, Rex(false, reg >= 8, false, true));
    Emit8(m_Pos, 0x89);
    Emit8(m_Pos, ModRM(0, reg & 7, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     dword [%s], reg%u\n", varName, reg);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Set-byte operations
// ──────────────────────────────────────────────

void RspAssembler::SetgVariable(void* variable, const char* varName)
{
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x9F);
    Emit8(m_Pos, ModRM(0, 0, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    setg    byte [%s]\n", varName);
    m_CodeLog += buf;
}

void RspAssembler::SetnzVariable(void* variable, const char* varName)
{
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x95);
    Emit8(m_Pos, ModRM(0, 0, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    setnz   byte [%s]\n", varName);
    m_CodeLog += buf;
}

void RspAssembler::SetzVariable(void* variable, const char* varName)
{
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x94);
    Emit8(m_Pos, ModRM(0, 0, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    setz    byte [%s]\n", varName);
    m_CodeLog += buf;
}

void RspAssembler::SetbVariable(void* variable, const char* varName)
{
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, 0x41);  // REX.B for r11
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x92);
    Emit8(m_Pos, ModRM(0, 0, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    setb    byte [%s]\n", varName);
    m_CodeLog += buf;
}

void RspAssembler::SetlVariable(void* variable, const char* varName)
{
    Emit8(m_Pos, Rex(true, false, false, true));
    Emit8(m_Pos, 0xBB);
    Emit64(m_Pos, reinterpret_cast<uint64_t>(variable));
    Emit8(m_Pos, 0x41);  // REX.B for r11
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x9C);
    Emit8(m_Pos, ModRM(0, 0, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    setl    byte [%s]\n", varName);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Additional conditional jumps
// ──────────────────────────────────────────────

void RspAssembler::JlLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x8C, "jl", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JgeLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x8D, "jge", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JbLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x82, "jb", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JaeLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x83, "jae", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JaLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x87, "ja", labelName, jumpLabel, m_CodeLog);
}

void RspAssembler::JbeLabel(const char* labelName, AsmLabel& jumpLabel)
{
    EmitJcc(m_Pos, 0x86, "jbe", labelName, jumpLabel, m_CodeLog);
}

// ──────────────────────────────────────────────
// Arithmetic on registers
// ──────────────────────────────────────────────

void RspAssembler::AddConstToX86reg(uint8_t reg, uint32_t constant)
{
    if (constant == 0) return;

    // add reg, imm32
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0x81);
    Emit8(m_Pos, ModRM(3, 0, reg & 7));
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    add     reg%u, 0x%X\n", reg, constant);
    m_CodeLog += buf;
}

void RspAssembler::SubConstFromX86reg(uint8_t reg, uint32_t constant)
{
    if (constant == 0) return;

    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0x81);
    Emit8(m_Pos, ModRM(3, 5, reg & 7));
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    sub     reg%u, 0x%X\n", reg, constant);
    m_CodeLog += buf;
}

void RspAssembler::AndConstToX86reg(uint8_t reg, uint32_t constant)
{
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0x81);
    Emit8(m_Pos, ModRM(3, 4, reg & 7));
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    and     reg%u, 0x%X\n", reg, constant);
    m_CodeLog += buf;
}

void RspAssembler::OrConstToX86reg(uint8_t reg, uint32_t constant)
{
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0x81);
    Emit8(m_Pos, ModRM(3, 1, reg & 7));
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    or      reg%u, 0x%X\n", reg, constant);
    m_CodeLog += buf;
}

void RspAssembler::XorX86regToX86reg(uint8_t dst, uint8_t src)
{
    Emit8(m_Pos, Rex(false, src >= 8, false, dst >= 8));
    Emit8(m_Pos, 0x31);
    Emit8(m_Pos, ModRM(3, src & 7, dst & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    xor     reg%u, reg%u\n", dst, src);
    m_CodeLog += buf;
}

void RspAssembler::AndX86regToX86reg(uint8_t dst, uint8_t src)
{
    Emit8(m_Pos, Rex(false, src >= 8, false, dst >= 8));
    Emit8(m_Pos, 0x21);
    Emit8(m_Pos, ModRM(3, src & 7, dst & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    and     reg%u, reg%u\n", dst, src);
    m_CodeLog += buf;
}

void RspAssembler::OrX86regToX86reg(uint8_t dst, uint8_t src)
{
    Emit8(m_Pos, Rex(false, src >= 8, false, dst >= 8));
    Emit8(m_Pos, 0x09);
    Emit8(m_Pos, ModRM(3, src & 7, dst & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    or      reg%u, reg%u\n", dst, src);
    m_CodeLog += buf;
}

void RspAssembler::AddX86regToX86reg(uint8_t dst, uint8_t src)
{
    Emit8(m_Pos, Rex(false, src >= 8, false, dst >= 8));
    Emit8(m_Pos, 0x01);
    Emit8(m_Pos, ModRM(3, src & 7, dst & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    add     reg%u, reg%u\n", dst, src);
    m_CodeLog += buf;
}

void RspAssembler::SubX86regFromX86reg(uint8_t dst, uint8_t src)
{
    Emit8(m_Pos, Rex(false, src >= 8, false, dst >= 8));
    Emit8(m_Pos, 0x29);
    Emit8(m_Pos, ModRM(3, src & 7, dst & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    sub     reg%u, reg%u\n", dst, src);
    m_CodeLog += buf;
}

void RspAssembler::XorConstToX86reg(uint8_t reg, uint32_t constant)
{
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0x81);
    Emit8(m_Pos, ModRM(3, 6, reg & 7));
    Emit32(m_Pos, constant);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    xor     reg%u, 0x%X\n", reg, constant);
    m_CodeLog += buf;
}

void RspAssembler::MoveX86regToX86reg(uint8_t dst, uint8_t src)
{
    Emit8(m_Pos, Rex(false, src >= 8, false, dst >= 8));
    Emit8(m_Pos, 0x89);
    Emit8(m_Pos, ModRM(3, src & 7, dst & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     reg%u, reg%u\n", dst, src);
    m_CodeLog += buf;
}

void RspAssembler::TestX86regToX86reg(uint8_t reg1, uint8_t reg2)
{
    Emit8(m_Pos, Rex(false, reg2 >= 8, false, reg1 >= 8));
    Emit8(m_Pos, 0x85);
    Emit8(m_Pos, ModRM(3, reg2 & 7, reg1 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    test    reg%u, reg%u\n", reg1, reg2);
    m_CodeLog += buf;
}

void RspAssembler::ShiftLeftX86reg(uint8_t reg, uint8_t amount)
{
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0xC1);
    Emit8(m_Pos, ModRM(3, 4, reg & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    shl     reg%u, %u\n", reg, amount);
    m_CodeLog += buf;
}

void RspAssembler::ShiftRightX86reg(uint8_t reg, uint8_t amount)
{
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0xC1);
    Emit8(m_Pos, ModRM(3, 5, reg & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    shr     reg%u, %u\n", reg, amount);
    m_CodeLog += buf;
}

void RspAssembler::ArithShiftRightX86reg(uint8_t reg, uint8_t amount)
{
    bool rexB = reg >= 8;
    Emit8(m_Pos, Rex(false, false, false, rexB));
    Emit8(m_Pos, 0xC1);
    Emit8(m_Pos, ModRM(3, 7, reg & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    sar     reg%u, %u\n", reg, amount);
    m_CodeLog += buf;
}

// CL-based variable shifts (shift amount in CL register)
void RspAssembler::ShiftLeftX86regByCL(uint8_t reg)
{
    bool rexB = reg >= 8;
    if (rexB)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0xD3);
    Emit8(m_Pos, ModRM(3, 4, reg & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    shl     reg%u, cl\n", reg);
    m_CodeLog += buf;
}

void RspAssembler::ShiftRightX86regByCL(uint8_t reg)
{
    bool rexB = reg >= 8;
    if (rexB)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0xD3);
    Emit8(m_Pos, ModRM(3, 5, reg & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    shr     reg%u, cl\n", reg);
    m_CodeLog += buf;
}

void RspAssembler::ArithShiftRightX86regByCL(uint8_t reg)
{
    bool rexB = reg >= 8;
    if (rexB)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0xD3);
    Emit8(m_Pos, ModRM(3, 7, reg & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    sar     reg%u, cl\n", reg);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Memory access via base+index (SIB addressing)
// Used for DMEM access where RBP = DMEM base pointer
// ──────────────────────────────────────────────

// Helper: emit SIB byte for [base + index] addressing
// ModRM mod=00, rm=4 means SIB follows. SIB: scale=0, index, base.
// Special: if base==RBP(5), ModRM mod must be 01 with disp8=0
// since [RBP] in mod=00 means [disp32] (no base).
static void EmitBaseIndexAddr(uint8_t*& pos, uint8_t regField, uint8_t baseReg, uint8_t indexReg)
{
    uint8_t b = baseReg & 7;
    uint8_t i = indexReg & 7;
    uint8_t r = regField & 7;

    if (b == 5) {
        // RBP/R13: need mod=01 with disp8=0
        Emit8(pos, ModRM(1, r, 4));      // mod=01, reg=r, rm=SIB
        Emit8(pos, (0 << 6) | (i << 3) | b); // scale=1, index, base
        Emit8(pos, 0);                    // disp8 = 0
    } else {
        Emit8(pos, ModRM(0, r, 4));      // mod=00, reg=r, rm=SIB
        Emit8(pos, (0 << 6) | (i << 3) | b); // scale=1, index, base
    }
}

// REX for base+index: W=w, R=(regField>=8), X=(index>=8), B=(base>=8)
static uint8_t RexBIR(bool w, uint8_t regField, uint8_t indexReg, uint8_t baseReg)
{
    return Rex(w, regField >= 8, indexReg >= 8, baseReg >= 8);
}

// movzx dst, byte [base + index]
void RspAssembler::MovzxByteFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg)
{
    uint8_t rex = RexBIR(false, dstReg, indexReg, baseReg);
    if (rex != 0x40 || dstReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xB6); // movzx r32, r/m8
    EmitBaseIndexAddr(m_Pos, dstReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    movzx   reg%u, byte [reg%u + reg%u]\n", dstReg, baseReg, indexReg);
    m_CodeLog += buf;
}

// movsx dst, byte [base + index]
void RspAssembler::MovsxByteFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg)
{
    uint8_t rex = RexBIR(false, dstReg, indexReg, baseReg);
    if (rex != 0x40 || dstReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xBE); // movsx r32, r/m8
    EmitBaseIndexAddr(m_Pos, dstReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    movsx   reg%u, byte [reg%u + reg%u]\n", dstReg, baseReg, indexReg);
    m_CodeLog += buf;
}

// movzx dst, word [base + index]
void RspAssembler::MovzxWordFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg)
{
    uint8_t rex = RexBIR(false, dstReg, indexReg, baseReg);
    if (rex != 0x40 || dstReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xB7); // movzx r32, r/m16
    EmitBaseIndexAddr(m_Pos, dstReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    movzx   reg%u, word [reg%u + reg%u]\n", dstReg, baseReg, indexReg);
    m_CodeLog += buf;
}

// movsx dst, word [base + index]
void RspAssembler::MovsxWordFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg)
{
    uint8_t rex = RexBIR(false, dstReg, indexReg, baseReg);
    if (rex != 0x40 || dstReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xBF); // movsx r32, r/m16
    EmitBaseIndexAddr(m_Pos, dstReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    movsx   reg%u, word [reg%u + reg%u]\n", dstReg, baseReg, indexReg);
    m_CodeLog += buf;
}

// mov dst, dword [base + index]
void RspAssembler::MovDwordFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg)
{
    uint8_t rex = RexBIR(false, dstReg, indexReg, baseReg);
    if (rex != 0x40 || dstReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x8B); // mov r32, r/m32
    EmitBaseIndexAddr(m_Pos, dstReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     reg%u, dword [reg%u + reg%u]\n", dstReg, baseReg, indexReg);
    m_CodeLog += buf;
}

// mov byte [base + index], src (lowest byte of src)
void RspAssembler::MovByteToBaseIndex(uint8_t baseReg, uint8_t indexReg, uint8_t srcReg)
{
    // Need REX if srcReg >= 4 (to access SIL/DIL/etc) or any reg >= 8
    uint8_t rex = RexBIR(false, srcReg, indexReg, baseReg);
    if (rex != 0x40 || srcReg >= 4 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x88); // mov r/m8, r8
    EmitBaseIndexAddr(m_Pos, srcReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     byte [reg%u + reg%u], reg%u\n", baseReg, indexReg, srcReg);
    m_CodeLog += buf;
}

// mov word [base + index], src (lowest word of src)
void RspAssembler::MovWordToBaseIndex(uint8_t baseReg, uint8_t indexReg, uint8_t srcReg)
{
    Emit8(m_Pos, 0x66); // operand size prefix for 16-bit
    uint8_t rex = RexBIR(false, srcReg, indexReg, baseReg);
    if (rex != 0x40 || srcReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x89); // mov r/m16, r16
    EmitBaseIndexAddr(m_Pos, srcReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     word [reg%u + reg%u], reg%u\n", baseReg, indexReg, srcReg);
    m_CodeLog += buf;
}

// mov dword [base + index], src
void RspAssembler::MovDwordToBaseIndex(uint8_t baseReg, uint8_t indexReg, uint8_t srcReg)
{
    uint8_t rex = RexBIR(false, srcReg, indexReg, baseReg);
    if (rex != 0x40 || srcReg >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x89); // mov r/m32, r32
    EmitBaseIndexAddr(m_Pos, srcReg, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    mov     dword [reg%u + reg%u], reg%u\n", baseReg, indexReg, srcReg);
    m_CodeLog += buf;
}

// bswap reg32
void RspAssembler::BswapX86reg(uint8_t reg)
{
    if (reg >= 8)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xC8 + (reg & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    bswap   reg%u\n", reg);
    m_CodeLog += buf;
}

// shr reg16, imm (16-bit shift right)
void RspAssembler::ShiftRightX86reg16(uint8_t reg, uint8_t amount)
{
    Emit8(m_Pos, 0x66); // operand size prefix
    if (reg >= 8)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0xC1);
    Emit8(m_Pos, ModRM(3, 5, reg & 7)); // /5 = SHR
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    shr     reg%u_w, %u\n", reg, amount);
    m_CodeLog += buf;
}

// movdqu xmm, [base + index]
void RspAssembler::Movdqu_XmmFromBaseIndex(uint8_t xmm, uint8_t baseReg, uint8_t indexReg)
{
    Emit8(m_Pos, 0xF3);
    uint8_t rex = RexBIR(false, xmm, indexReg, baseReg);
    if (rex != 0x40 || xmm >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x6F); // movdqu xmm, m128
    EmitBaseIndexAddr(m_Pos, xmm, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    movdqu  xmm%u, [reg%u + reg%u]\n", xmm, baseReg, indexReg);
    m_CodeLog += buf;
}

// movdqu [base + index], xmm
void RspAssembler::Movdqu_BaseIndexFromXmm(uint8_t baseReg, uint8_t indexReg, uint8_t xmm)
{
    Emit8(m_Pos, 0xF3);
    uint8_t rex = RexBIR(false, xmm, indexReg, baseReg);
    if (rex != 0x40 || xmm >= 8 || indexReg >= 8 || baseReg >= 8)
        Emit8(m_Pos, rex);
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x7F); // movdqu m128, xmm
    EmitBaseIndexAddr(m_Pos, xmm, baseReg, indexReg);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    movdqu  [reg%u + reg%u], xmm%u\n", baseReg, indexReg, xmm);
    m_CodeLog += buf;
}

// ──────────────────────────────────────────────
// Push / Pop (for prologue/epilogue)
// ──────────────────────────────────────────────

void RspAssembler::PushX86reg(uint8_t reg)
{
    if (reg >= 8)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x50 + (reg & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    push    reg%u\n", reg);
    m_CodeLog += buf;
}

void RspAssembler::PopX86reg(uint8_t reg)
{
    if (reg >= 8)
        Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x58 + (reg & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    pop     reg%u\n", reg);
    m_CodeLog += buf;
}

void RspAssembler::Ret()
{
    Emit8(m_Pos, 0xC3);
    m_CodeLog += "    ret\n";
}

// ──────────────────────────────────────────────
// XMM / SSE2 instructions for vector operations
// ──────────────────────────────────────────────

static void EmitSSE2(uint8_t*& pos, uint8_t prefix, uint8_t op1, uint8_t op2,
                     uint8_t dstXmm, uint8_t srcXmm,
                     const char* mnemonic, std::string& log)
{
    // SSE2 reg-reg: prefix 0F op modrm
    if (prefix) Emit8(pos, prefix);  // e.g., 0x66
    bool rexR = dstXmm >= 8;
    bool rexB = srcXmm >= 8;
    if (rexR || rexB)
        Emit8(pos, Rex(false, rexR, false, rexB));
    Emit8(pos, 0x0F);
    if (op1 == 0x38 || op1 == 0x3A)
    {
        // SSSE3/SSE4 three-byte opcode: 0F 38 xx or 0F 3A xx
        Emit8(pos, op1);
        Emit8(pos, op2);
    }
    else
    {
        Emit8(pos, op1);
    }
    Emit8(pos, ModRM(3, dstXmm & 7, srcXmm & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    %-12s xmm%u, xmm%u\n", mnemonic, dstXmm, srcXmm);
    log += buf;
}

static void EmitSSE2_Mem(uint8_t*& pos, uint8_t prefix, uint8_t op,
                         uint8_t xmmReg, void* memAddr,
                         const char* mnemonic, const char* addrName,
                         std::string& log)
{
    // Load/store from absolute address via r11 scratch
    // mov r11, memAddr
    Emit8(pos, Rex(true, false, false, true));
    Emit8(pos, 0xBB);
    Emit64(pos, reinterpret_cast<uint64_t>(memAddr));
    // prefix 0F op modrm(xmm, [r11])
    if (prefix) Emit8(pos, prefix);
    bool rexR = xmmReg >= 8;
    Emit8(pos, Rex(false, rexR, false, true));
    Emit8(pos, 0x0F);
    Emit8(pos, op);
    Emit8(pos, ModRM(0, xmmReg & 7, x64::R11 & 7));

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    %-12s xmm%u, [%s]\n", mnemonic, xmmReg, addrName);
    log += buf;
}

// Aligned move: movdqa xmm, xmm
void RspAssembler::Movdqa_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x6F, 0, dst, src, "movdqa", m_CodeLog);
}

// Aligned load: movdqa xmm, [mem]
void RspAssembler::Movdqa_XmmFromMem(uint8_t xmm, void* memAddr, const char* addrName)
{
    EmitSSE2_Mem(m_Pos, 0x66, 0x6F, xmm, memAddr, "movdqa", addrName, m_CodeLog);
}

// Aligned store: movdqa [mem], xmm
void RspAssembler::Movdqa_MemFromXmm(void* memAddr, const char* addrName, uint8_t xmm)
{
    EmitSSE2_Mem(m_Pos, 0x66, 0x7F, xmm, memAddr, "movdqa", addrName, m_CodeLog);
}

// Unaligned load: movdqu xmm, [mem]
void RspAssembler::Movdqu_XmmFromMem(uint8_t xmm, void* memAddr, const char* addrName)
{
    EmitSSE2_Mem(m_Pos, 0xF3, 0x6F, xmm, memAddr, "movdqu", addrName, m_CodeLog);
}

// Unaligned store: movdqu [mem], xmm
void RspAssembler::Movdqu_MemFromXmm(void* memAddr, const char* addrName, uint8_t xmm)
{
    EmitSSE2_Mem(m_Pos, 0xF3, 0x7F, xmm, memAddr, "movdqu", addrName, m_CodeLog);
}

// XOR: pxor xmm, xmm (zero a register)
void RspAssembler::Pxor_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xEF, 0, dst, src, "pxor", m_CodeLog);
}

// Add packed words: paddw xmm, xmm
void RspAssembler::Paddw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xFD, 0, dst, src, "paddw", m_CodeLog);
}

// Saturate add signed: paddsw xmm, xmm
void RspAssembler::Paddsw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xED, 0, dst, src, "paddsw", m_CodeLog);
}

// Sub packed words: psubw xmm, xmm
void RspAssembler::Psubw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xF9, 0, dst, src, "psubw", m_CodeLog);
}

// Saturate sub signed: psubsw xmm, xmm
void RspAssembler::Psubsw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xE9, 0, dst, src, "psubsw", m_CodeLog);
}

// Multiply low: pmullw xmm, xmm
void RspAssembler::Pmullw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xD5, 0, dst, src, "pmullw", m_CodeLog);
}

// Multiply high signed: pmulhw xmm, xmm
void RspAssembler::Pmulhw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xE5, 0, dst, src, "pmulhw", m_CodeLog);
}

// Multiply high unsigned: pmulhuw xmm, xmm
void RspAssembler::Pmulhuw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xE4, 0, dst, src, "pmulhuw", m_CodeLog);
}

// Compare equal words: pcmpeqw xmm, xmm
void RspAssembler::Pcmpeqw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x75, 0, dst, src, "pcmpeqw", m_CodeLog);
}

// Compare greater than words: pcmpgtw xmm, xmm
void RspAssembler::Pcmpgtw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x65, 0, dst, src, "pcmpgtw", m_CodeLog);
}

// AND: pand xmm, xmm
void RspAssembler::Pand_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xDB, 0, dst, src, "pand", m_CodeLog);
}

// AND-NOT: pandn xmm, xmm (dst = ~dst & src)
void RspAssembler::Pandn_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xDF, 0, dst, src, "pandn", m_CodeLog);
}

// OR: por xmm, xmm
void RspAssembler::Por_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xEB, 0, dst, src, "por", m_CodeLog);
}

// Shift left words: psllw xmm, imm8
void RspAssembler::Psllw_XmmImm(uint8_t xmm, uint8_t amount)
{
    if (0x66) Emit8(m_Pos, 0x66);
    bool rexB = xmm >= 8;
    if (rexB) Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x71);
    Emit8(m_Pos, ModRM(3, 6, xmm & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    psllw   xmm%u, %u\n", xmm, amount);
    m_CodeLog += buf;
}

// Shift right logical words: psrlw xmm, imm8
void RspAssembler::Psrlw_XmmImm(uint8_t xmm, uint8_t amount)
{
    Emit8(m_Pos, 0x66);
    bool rexB = xmm >= 8;
    if (rexB) Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x71);
    Emit8(m_Pos, ModRM(3, 2, xmm & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    psrlw   xmm%u, %u\n", xmm, amount);
    m_CodeLog += buf;
}

// Shift right arithmetic words: psraw xmm, imm8
void RspAssembler::Psraw_XmmImm(uint8_t xmm, uint8_t amount)
{
    Emit8(m_Pos, 0x66);
    bool rexB = xmm >= 8;
    if (rexB) Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x71);
    Emit8(m_Pos, ModRM(3, 4, xmm & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    psraw   xmm%u, %u\n", xmm, amount);
    m_CodeLog += buf;
}

// SSSE3 pshufb: pshufb xmm, xmm
void RspAssembler::Pshufb_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x38, 0x00, dst, src, "pshufb", m_CodeLog);
}

// Shuffle dwords: pshufd xmm, xmm, imm8
void RspAssembler::Pshufd_XmmToXmm(uint8_t dst, uint8_t src, uint8_t order)
{
    Emit8(m_Pos, 0x66);
    bool rexR = dst >= 8;
    bool rexB = src >= 8;
    if (rexR || rexB) Emit8(m_Pos, Rex(false, rexR, false, rexB));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x70);
    Emit8(m_Pos, ModRM(3, dst & 7, src & 7));
    Emit8(m_Pos, order);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    pshufd  xmm%u, xmm%u, 0x%02X\n", dst, src, order);
    m_CodeLog += buf;
}

// Min signed words: pminsw xmm, xmm
void RspAssembler::Pminsw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xEA, 0, dst, src, "pminsw", m_CodeLog);
}

// Max signed words: pmaxsw xmm, xmm
void RspAssembler::Pmaxsw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xEE, 0, dst, src, "pmaxsw", m_CodeLog);
}

// ── SSE2 word extract/insert ──

// PEXTRW r32, xmm, imm8: extract word from XMM into GPR
void RspAssembler::Pextrw(uint8_t gprDst, uint8_t xmmSrc, uint8_t wordIndex)
{
    // Encoding: 66 [REX] 0F C5 ModRM(3, gpr, xmm) imm8
    Emit8(m_Pos, 0x66);
    bool rexR = gprDst >= 8;
    bool rexB = xmmSrc >= 8;
    if (rexR || rexB) Emit8(m_Pos, Rex(false, rexR, false, rexB));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xC5);
    Emit8(m_Pos, ModRM(3, gprDst & 7, xmmSrc & 7));
    Emit8(m_Pos, wordIndex);

    static const char* gprNames[] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
    };
    char buf[256];
    std::snprintf(buf, sizeof(buf), "    pextrw  %s, xmm%u, %u\n",
                  gprNames[gprDst & 15], xmmSrc, wordIndex);
    m_CodeLog += buf;
}

// PINSRW xmm, r32, imm8: insert word from GPR into XMM
void RspAssembler::Pinsrw(uint8_t xmmDst, uint8_t gprSrc, uint8_t wordIndex)
{
    // Encoding: 66 [REX] 0F C4 ModRM(3, xmm, gpr) imm8
    Emit8(m_Pos, 0x66);
    bool rexR = xmmDst >= 8;
    bool rexB = gprSrc >= 8;
    if (rexR || rexB) Emit8(m_Pos, Rex(false, rexR, false, rexB));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0xC4);
    Emit8(m_Pos, ModRM(3, xmmDst & 7, gprSrc & 7));
    Emit8(m_Pos, wordIndex);

    static const char* gprNames[] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
    };
    char buf[256];
    std::snprintf(buf, sizeof(buf), "    pinsrw  xmm%u, %s, %u\n",
                  xmmDst, gprNames[gprSrc & 15], wordIndex);
    m_CodeLog += buf;
}

// ── Additional SSE2 for vector multiply/accumulate ──

// Unpack interleave low words: punpcklwd xmm, xmm
void RspAssembler::Punpcklwd_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x61, 0, dst, src, "punpcklwd", m_CodeLog);
}

// Unpack interleave high words: punpckhwd xmm, xmm
void RspAssembler::Punpckhwd_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x69, 0, dst, src, "punpckhwd", m_CodeLog);
}

// Add packed dwords: paddd xmm, xmm
void RspAssembler::Paddd_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0xFE, 0, dst, src, "paddd", m_CodeLog);
}

// Shift left dwords by imm: pslld xmm, imm8
void RspAssembler::Pslld_XmmImm(uint8_t xmm, uint8_t amount)
{
    Emit8(m_Pos, 0x66);
    bool rexB = xmm >= 8;
    if (rexB) Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x72);
    Emit8(m_Pos, ModRM(3, 6, xmm & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    pslld   xmm%u, %u\n", xmm, amount);
    m_CodeLog += buf;
}

// Shift right logical dwords by imm: psrld xmm, imm8
void RspAssembler::Psrld_XmmImm(uint8_t xmm, uint8_t amount)
{
    Emit8(m_Pos, 0x66);
    bool rexB = xmm >= 8;
    if (rexB) Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x72);
    Emit8(m_Pos, ModRM(3, 2, xmm & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    psrld   xmm%u, %u\n", xmm, amount);
    m_CodeLog += buf;
}

// Shift right arithmetic dwords by imm: psrad xmm, imm8
void RspAssembler::Psrad_XmmImm(uint8_t xmm, uint8_t amount)
{
    Emit8(m_Pos, 0x66);
    bool rexB = xmm >= 8;
    if (rexB) Emit8(m_Pos, Rex(false, false, false, true));
    Emit8(m_Pos, 0x0F);
    Emit8(m_Pos, 0x72);
    Emit8(m_Pos, ModRM(3, 4, xmm & 7));
    Emit8(m_Pos, amount);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "    psrad   xmm%u, %u\n", xmm, amount);
    m_CodeLog += buf;
}

// Pack signed dwords to signed words with saturation: packssdw xmm, xmm
void RspAssembler::Packssdw_XmmToXmm(uint8_t dst, uint8_t src)
{
    EmitSSE2(m_Pos, 0x66, 0x6B, 0, dst, src, "packssdw", m_CodeLog);
}

// Alias for Movdqa_XmmFromMem (for read-only constant loads)
void RspAssembler::Movdqa_XmmFromMem_Const(uint8_t xmm, void* memAddr, const char* addrName)
{
    Movdqa_XmmFromMem(xmm, memAddr, addrName);
}

// ──────────────────────────────────────────────
// Code size query
// ──────────────────────────────────────────────

size_t RspAssembler::GetCodeSize() const
{
    return static_cast<size_t>(m_Pos - m_Buffer);
}

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
