/**
 * @file RspAssembler.hpp
 * @brief RSP Assembler.
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */

#pragma once
#include <cstdint>
#include <string>
#include <map>
#include <vector>

#if defined(__amd64__) || defined(_M_X64)

namespace Nova64::RSP::Recompiler {

// ──────────────────────────────────────────────
// Label / symbol tracking
// ──────────────────────────────────────────────
struct NumberSymbol {
    std::string Symbol;
    uint32_t    Count{0};
};

using LabelSymbolMap  = std::map<uint64_t, std::string>;
using NumberSymbolMap = std::map<uint64_t, NumberSymbol>;

// ──────────────────────────────────────────────
// Label handle (opaque index)
// ──────────────────────────────────────────────
struct AsmLabel {
    uint32_t id{0};
    bool valid{false};
};

// ──────────────────────────────────────────────
// RspAssembler - x64 code emission with logging
// Wraps asmjit or custom code buffer for RSP JIT
// ──────────────────────────────────────────────
class RspAssembler {
public:
    RspAssembler(uint8_t* codeBuffer, size_t bufferSize, std::string& codeLog);
    ~RspAssembler();

    RspAssembler(const RspAssembler&) = delete;
    RspAssembler& operator=(const RspAssembler&) = delete;

    void Reset();
    void SetPrimarySection();
    void SetSecondarySection();

    // ── High-level emission helpers ──
    void CallFunc(void* funcPtr, const char* funcName);
    void CallThis(void* thisPtr, void* funcPtr, const char* funcName);

    void CompConstToVariable(void* variable, const char* varName, uint32_t constant);
    void CompX86regToVariable(void* variable, const char* varName, uint8_t reg);

    void JeLabel(const char* labelName, AsmLabel& jumpLabel);
    void JgLabel(const char* labelName, AsmLabel& jumpLabel);
    void JleLabel(const char* labelName, AsmLabel& jumpLabel);
    void JmpLabel(const char* labelName, AsmLabel& jumpLabel);
    void JneLabel(const char* labelName, AsmLabel& jumpLabel);
    void JFunc(void* funcPtr, const char* funcName);

    void MoveConstToVariable(void* variable, const char* varName, uint32_t constant);
    void MoveVariableToX86reg(uint8_t reg, void* variable, const char* varName);
    void MoveX86regToVariable(void* variable, const char* varName, uint8_t reg);

    // ── Additional conditional jumps ──
    void JlLabel(const char* labelName, AsmLabel& jumpLabel);
    void JgeLabel(const char* labelName, AsmLabel& jumpLabel);
    void JbLabel(const char* labelName, AsmLabel& jumpLabel);
    void JaeLabel(const char* labelName, AsmLabel& jumpLabel);
    void JaLabel(const char* labelName, AsmLabel& jumpLabel);
    void JbeLabel(const char* labelName, AsmLabel& jumpLabel);

    // ── Register arithmetic ──
    void AddConstToX86reg(uint8_t reg, uint32_t constant);
    void SubConstFromX86reg(uint8_t reg, uint32_t constant);
    void AndConstToX86reg(uint8_t reg, uint32_t constant);
    void OrConstToX86reg(uint8_t reg, uint32_t constant);
    void XorConstToX86reg(uint8_t reg, uint32_t constant);
    void XorX86regToX86reg(uint8_t dst, uint8_t src);
    void AndX86regToX86reg(uint8_t dst, uint8_t src);
    void OrX86regToX86reg(uint8_t dst, uint8_t src);
    void AddX86regToX86reg(uint8_t dst, uint8_t src);
    void SubX86regFromX86reg(uint8_t dst, uint8_t src);
    void TestX86regToX86reg(uint8_t reg1, uint8_t reg2);
    void MoveX86regToX86reg(uint8_t dst, uint8_t src);
    void ShiftLeftX86reg(uint8_t reg, uint8_t amount);
    void ShiftRightX86reg(uint8_t reg, uint8_t amount);
    void ArithShiftRightX86reg(uint8_t reg, uint8_t amount);
    void ShiftLeftX86regByCL(uint8_t reg);
    void ShiftRightX86regByCL(uint8_t reg);
    void ArithShiftRightX86regByCL(uint8_t reg);

    // ── Memory access via base+index (for DMEM access where RBP=base) ──
    void MovzxByteFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg);
    void MovsxByteFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg);
    void MovzxWordFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg);
    void MovsxWordFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg);
    void MovDwordFromBaseIndex(uint8_t dstReg, uint8_t baseReg, uint8_t indexReg);
    void MovByteToBaseIndex(uint8_t baseReg, uint8_t indexReg, uint8_t srcReg);
    void MovWordToBaseIndex(uint8_t baseReg, uint8_t indexReg, uint8_t srcReg);
    void MovDwordToBaseIndex(uint8_t baseReg, uint8_t indexReg, uint8_t srcReg);
    void BswapX86reg(uint8_t reg);
    void ShiftRightX86reg16(uint8_t reg, uint8_t amount);  // shr reg16, imm (for halfword)
    void Movdqu_XmmFromBaseIndex(uint8_t xmm, uint8_t baseReg, uint8_t indexReg);
    void Movdqu_BaseIndexFromXmm(uint8_t baseReg, uint8_t indexReg, uint8_t xmm);

    // ── Set-byte operations ──
    void SetgVariable(void* variable, const char* varName);
    void SetlVariable(void* variable, const char* varName);
    void SetbVariable(void* variable, const char* varName);
    void SetnzVariable(void* variable, const char* varName);
    void SetzVariable(void* variable, const char* varName);

    // ── Push / Pop / Ret ──
    void PushX86reg(uint8_t reg);
    void PopX86reg(uint8_t reg);
    void Ret();

    // ── SSE2 / XMM instructions ──
    void Movdqa_XmmToXmm(uint8_t dst, uint8_t src);
    void Movdqa_XmmFromMem(uint8_t xmm, void* memAddr, const char* addrName);
    void Movdqa_MemFromXmm(void* memAddr, const char* addrName, uint8_t xmm);
    void Movdqu_XmmFromMem(uint8_t xmm, void* memAddr, const char* addrName);
    void Movdqu_MemFromXmm(void* memAddr, const char* addrName, uint8_t xmm);
    void Pxor_XmmToXmm(uint8_t dst, uint8_t src);
    void Paddw_XmmToXmm(uint8_t dst, uint8_t src);
    void Paddsw_XmmToXmm(uint8_t dst, uint8_t src);
    void Psubw_XmmToXmm(uint8_t dst, uint8_t src);
    void Psubsw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pmullw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pmulhw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pmulhuw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pcmpeqw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pcmpgtw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pand_XmmToXmm(uint8_t dst, uint8_t src);
    void Pandn_XmmToXmm(uint8_t dst, uint8_t src);
    void Por_XmmToXmm(uint8_t dst, uint8_t src);
    void Psllw_XmmImm(uint8_t xmm, uint8_t amount);
    void Psrlw_XmmImm(uint8_t xmm, uint8_t amount);
    void Psraw_XmmImm(uint8_t xmm, uint8_t amount);
    void Pshufb_XmmToXmm(uint8_t dst, uint8_t src);
    void Pshufd_XmmToXmm(uint8_t dst, uint8_t src, uint8_t order);
    void Pminsw_XmmToXmm(uint8_t dst, uint8_t src);
    void Pmaxsw_XmmToXmm(uint8_t dst, uint8_t src);

    // ── SSE2 word extract/insert (for scalar element operations) ──
    void Pextrw(uint8_t gprDst, uint8_t xmmSrc, uint8_t wordIndex);
    void Pinsrw(uint8_t xmmDst, uint8_t gprSrc, uint8_t wordIndex);

    // ── Additional SSE2 for vector multiply/accumulate ──
    void Punpcklwd_XmmToXmm(uint8_t dst, uint8_t src);
    void Punpckhwd_XmmToXmm(uint8_t dst, uint8_t src);
    void Paddd_XmmToXmm(uint8_t dst, uint8_t src);
    void Pslld_XmmImm(uint8_t xmm, uint8_t amount);
    void Psrld_XmmImm(uint8_t xmm, uint8_t amount);
    void Psrad_XmmImm(uint8_t xmm, uint8_t amount);
    void Packssdw_XmmToXmm(uint8_t dst, uint8_t src);
    void Movdqa_XmmFromMem_Const(uint8_t xmm, void* memAddr, const char* addrName);

    [[nodiscard]] size_t GetCodeSize() const;

    void AddLabelSymbol(const AsmLabel& label, const char* symbol);

    // ── Code buffer access ──
    [[nodiscard]] uint8_t* CurrentPos() const { return m_Pos; }
    [[nodiscard]] size_t   CodeSize() const   { return m_Pos - m_Buffer; }

    // ── Label management ──
    AsmLabel CreateLabel();
    void BindLabel(AsmLabel& label);

private:
    void AddNumberSymbol(uint64_t value, const char* symbol);

    uint8_t*        m_Buffer;
    uint8_t*        m_Pos;
    size_t          m_BufferSize;
    std::string&    m_CodeLog;
    LabelSymbolMap  m_LabelSymbols;
    NumberSymbolMap  m_NumberSymbols;
    uint32_t        m_NextLabelId{1};
    uint8_t*        m_PrimarySection{nullptr};
    uint8_t*        m_SecondarySection{nullptr};
};

} // namespace Nova64::RSP::Recompiler

#endif // x64 only
