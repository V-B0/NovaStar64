/**
 * @file RspJitX64.cpp
 * @brief x86-64 code emitter for the RSP recompiler.
 *
 * Register use inside a compiled block (all callee-saved on both the System V and
 * the Windows x64 ABI, so calls out don't disturb them):
 *   rbx  scalar register file (uint32_t[32])
 *   r12  DMEM
 *   r13  the CRSPSystem, for calls into the interpreter
 *   r14d where a pending branch goes (set before its delay slot runs)
 *   r15  the CRSPRegisters, for the vector kernels
 * eax, ecx, edx and r8d..r11d are scratch. (rsi and rdi are callee-saved on
 * Windows, so they are not used.)
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "RspJitBackend.hpp"
#include "RspJitDecode.hpp"
#include "RspJitKernels.hpp"

#include <asmjit/core.h>
#include <asmjit/x86.h>

namespace Nova64::RSP::Jit {

namespace x = asmjit::x86;

namespace {

#if defined(_WIN32)
const x::Gp kArg0 = x::rcx, kArg1 = x::rdx, kArg2 = x::r8;
#else
const x::Gp kArg0 = x::rdi, kArg1 = x::rsi, kArg2 = x::rdx;
#endif

const x::Gp kGpr  = x::rbx;
const x::Gp kDmem = x::r12;
const x::Gp kSys  = x::r13;
const x::Gp kJump = x::r14d;
const x::Gp kRegs = x::r15;

x::Mem GprMem(uint32_t r) { return x::dword_ptr(kGpr, static_cast<int32_t>(r * 4)); }

class Emitter {
public:
    Emitter(x::Assembler& as, const Block& block) : m_a(as), m_block(block) {}

    void Emit()
    {
        Prologue();
        for (const Insn& insn : m_block.body)
            Lower(insn);
        Terminate();
    }

private:
    x::Assembler& m_a;
    const Block&  m_block;

    // ---- block frame --------------------------------------------------------
    void Prologue()
    {
        m_a.push(x::rbx);
        m_a.push(x::r12);
        m_a.push(x::r13);
        m_a.push(x::r14);
        m_a.push(x::r15);
        m_a.sub(x::rsp, 32);           // call frame (the Windows ABI's shadow space); keeps rsp 16-aligned
        m_a.mov(x::rax, kArg0);        // the JitContext
        m_a.mov(kGpr,  x::qword_ptr(x::rax, 0));
        m_a.mov(kDmem, x::qword_ptr(x::rax, 8));
        m_a.mov(kSys,  x::qword_ptr(x::rax, 16));
        m_a.mov(kRegs, x::qword_ptr(x::rax, 24));
    }

    // Leaves the PC in eax and returns.
    void Epilogue()
    {
        m_a.add(x::rsp, 32);
        m_a.pop(x::r15);
        m_a.pop(x::r14);
        m_a.pop(x::r13);
        m_a.pop(x::r12);
        m_a.pop(x::rbx);
        m_a.ret();
    }

    // ---- small helpers ------------------------------------------------------
    void LoadGpr(const x::Gp& dst, uint32_t r) { m_a.mov(dst, GprMem(r)); }
    void StoreGpr(uint32_t r, const x::Gp& src) { if (r != 0) m_a.mov(GprMem(r), src); }
    void WriteLink(uint32_t reg, uint32_t pc)
    {
        if (reg == 0) return;
        m_a.mov(GprMem(reg), asmjit::Imm((pc + 8) & 0xFFC));
    }

    void CallAbsolute(const void* fn)
    {
        m_a.mov(x::rax, asmjit::Imm(reinterpret_cast<uint64_t>(fn)));
        m_a.call(x::rax);
    }

    // Leave it to the interpreter.
    void CallInterpreter(uint32_t word)
    {
        m_a.mov(kArg0, kSys);
        m_a.mov(kArg1.r32(), asmjit::Imm(word));
        CallAbsolute(reinterpret_cast<const void*>(InterpreterHelperFor(word)));
    }

    // eax = (gpr[base] + offset) & 0xFFF, the DMEM address a load or store uses.
    void DmemAddress(uint32_t base, int32_t simm)
    {
        LoadGpr(x::eax, base);
        const uint32_t low = static_cast<uint32_t>(simm) & 0xFFF;
        if (low != 0) m_a.add(x::eax, asmjit::Imm(low));
        m_a.and_(x::eax, asmjit::Imm(0xFFF));
    }

    // ---- instruction lowering -----------------------------------------------
    void Lower(const Insn& insn)
    {
        switch (Classify(insn.word)) {
        case Flow::Branch:
        case Flow::Jump:
        case Flow::JumpReg:
            LinkOnly(insn);      // a branch overruled by a branch in its delay slot
            return;
        case Flow::EndsBlock:
            CallInterpreter(insn.word);
            return;
        case Flow::None:
            break;
        }
        if (Native(insn)) return;
        if (KernelFn kernel = FindKernel(insn.word)) {
            m_a.mov(kArg0, kRegs);
            m_a.mov(kArg1, kDmem);
            m_a.mov(kArg2.r32(), asmjit::Imm(insn.word));
            CallAbsolute(reinterpret_cast<const void*>(kernel));
            return;
        }
        CallInterpreter(insn.word);
    }

    void LinkOnly(const Insn& insn)
    {
        const Fields f = Decode(insn.word);
        if (f.op == 0x03) WriteLink(31, insn.pc);                                     // JAL
        else if (f.op == 0x00 && f.funct == 0x09) WriteLink(f.rd, insn.pc);           // JALR
        else if (f.op == 0x01 && (f.rt == 0x10 || f.rt == 0x11)) WriteLink(31, insn.pc); // BLTZAL BGEZAL
    }

    bool Native(const Insn& insn)
    {
        const Fields f = Decode(insn.word);
        switch (f.op) {
        case 0x00: return NativeSpecial(f);
        case 0x08: case 0x09:                                     // ADDI ADDIU
            if (f.rt == 0) return true;
            LoadGpr(x::eax, f.rs);
            m_a.add(x::eax, asmjit::Imm(static_cast<uint32_t>(f.simm)));
            StoreGpr(f.rt, x::eax);
            return true;
        case 0x0A: case 0x0B:                                     // SLTI SLTIU
            if (f.rt == 0) return true;
            LoadGpr(x::ecx, f.rs);
            m_a.xor_(x::eax, x::eax);
            m_a.cmp(x::ecx, asmjit::Imm(static_cast<uint32_t>(f.simm)));
            if (f.op == 0x0A) m_a.setl(x::al); else m_a.setb(x::al);
            StoreGpr(f.rt, x::eax);
            return true;
        case 0x0C: case 0x0D: case 0x0E:                          // ANDI ORI XORI
            if (f.rt == 0) return true;
            LoadGpr(x::eax, f.rs);
            if (f.op == 0x0C)      m_a.and_(x::eax, asmjit::Imm(f.imm));
            else if (f.op == 0x0D) m_a.or_(x::eax, asmjit::Imm(f.imm));
            else                   m_a.xor_(x::eax, asmjit::Imm(f.imm));
            StoreGpr(f.rt, x::eax);
            return true;
        case 0x0F:                                                // LUI
            if (f.rt == 0) return true;
            m_a.mov(GprMem(f.rt), asmjit::Imm(f.imm << 16));
            return true;
        case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27:
            return NativeLoad(insn, f);
        case 0x28: case 0x29: case 0x2B:
            return NativeStore(insn, f);
        default:
            return false;
        }
    }

    bool NativeSpecial(const Fields& f)
    {
        switch (f.funct) {
        case 0x00: case 0x02: case 0x03:                          // SLL SRL SRA
            if (f.rd == 0) return true;
            LoadGpr(x::eax, f.rt);
            if (f.sa != 0) {
                if (f.funct == 0x00)      m_a.shl(x::eax, asmjit::Imm(f.sa));
                else if (f.funct == 0x02) m_a.shr(x::eax, asmjit::Imm(f.sa));
                else                      m_a.sar(x::eax, asmjit::Imm(f.sa));
            }
            StoreGpr(f.rd, x::eax);
            return true;
        case 0x04: case 0x06: case 0x07:                          // SLLV SRLV SRAV
            if (f.rd == 0) return true;
            LoadGpr(x::eax, f.rt);
            LoadGpr(x::ecx, f.rs);                                // the CPU masks the count to 5 bits
            if (f.funct == 0x04)      m_a.shl(x::eax, x::cl);
            else if (f.funct == 0x06) m_a.shr(x::eax, x::cl);
            else                      m_a.sar(x::eax, x::cl);
            StoreGpr(f.rd, x::eax);
            return true;
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:               // ADD ADDU SUB SUBU AND OR XOR NOR
            if (f.rd == 0) return true;
            LoadGpr(x::eax, f.rs);
            switch (f.funct) {
            case 0x20: case 0x21: m_a.add(x::eax, GprMem(f.rt)); break;
            case 0x22: case 0x23: m_a.sub(x::eax, GprMem(f.rt)); break;
            case 0x24: m_a.and_(x::eax, GprMem(f.rt)); break;
            case 0x25: m_a.or_(x::eax, GprMem(f.rt)); break;
            case 0x26: m_a.xor_(x::eax, GprMem(f.rt)); break;
            default:   m_a.or_(x::eax, GprMem(f.rt)); m_a.not_(x::eax); break;
            }
            StoreGpr(f.rd, x::eax);
            return true;
        case 0x2A: case 0x2B:                                     // SLT SLTU
            if (f.rd == 0) return true;
            LoadGpr(x::ecx, f.rs);
            m_a.xor_(x::eax, x::eax);
            m_a.cmp(x::ecx, GprMem(f.rt));
            if (f.funct == 0x2A) m_a.setl(x::al); else m_a.setb(x::al);
            StoreGpr(f.rd, x::eax);
            return true;
        default:
            return false;   // JR JALR BREAK are handled as block terminators; the rest go to the interpreter
        }
    }

    bool NativeLoad(const Insn& insn, const Fields& f)
    {
        if (f.rt == 0) return true;    // no side effects to keep
        const bool word = (f.op == 0x23 || f.op == 0x27);
        const bool half = (f.op == 0x21 || f.op == 0x25);

        asmjit::Label slow, done;
        if (word || half) { slow = m_a.new_label(); done = m_a.new_label(); }
        DmemAddress(f.rs, f.simm);
        if (word) { m_a.test(x::eax, asmjit::Imm(3)); m_a.jnz(slow); }
        if (half) { m_a.test(x::eax, asmjit::Imm(1)); m_a.jnz(slow); }

        if (word) {
            m_a.mov(x::ecx, x::dword_ptr(kDmem, x::rax));
        } else if (half) {
            m_a.mov(x::edx, x::eax);
            m_a.xor_(x::edx, asmjit::Imm(2));
            if (f.op == 0x21) m_a.movsx(x::ecx, x::word_ptr(kDmem, x::rdx));
            else              m_a.movzx(x::ecx, x::word_ptr(kDmem, x::rdx));
        } else {
            m_a.mov(x::edx, x::eax);
            m_a.xor_(x::edx, asmjit::Imm(3));
            if (f.op == 0x20) m_a.movsx(x::ecx, x::byte_ptr(kDmem, x::rdx));
            else              m_a.movzx(x::ecx, x::byte_ptr(kDmem, x::rdx));
        }
        StoreGpr(f.rt, x::ecx);

        if (word || half) {
            m_a.jmp(done);
            m_a.bind(slow);
            CallInterpreter(insn.word);   // unaligned: the interpreter assembles it byte by byte
            m_a.bind(done);
        }
        return true;
    }

    bool NativeStore(const Insn& insn, const Fields& f)
    {
        const bool word = (f.op == 0x2B);
        const bool half = (f.op == 0x29);

        asmjit::Label slow, done;
        if (word || half) { slow = m_a.new_label(); done = m_a.new_label(); }
        DmemAddress(f.rs, f.simm);
        if (word) { m_a.test(x::eax, asmjit::Imm(3)); m_a.jnz(slow); }
        if (half) { m_a.test(x::eax, asmjit::Imm(1)); m_a.jnz(slow); }

        LoadGpr(x::ecx, f.rt);
        if (word) {
            m_a.mov(x::dword_ptr(kDmem, x::rax), x::ecx);
        } else if (half) {
            m_a.mov(x::edx, x::eax);
            m_a.xor_(x::edx, asmjit::Imm(2));
            m_a.mov(x::word_ptr(kDmem, x::rdx), x::cx);
        } else {
            m_a.mov(x::edx, x::eax);
            m_a.xor_(x::edx, asmjit::Imm(3));
            m_a.mov(x::byte_ptr(kDmem, x::rdx), x::cl);
        }

        if (word || half) {
            m_a.jmp(done);
            m_a.bind(slow);
            CallInterpreter(insn.word);
            m_a.bind(done);
        }
        return true;
    }

    // ---- how the block ends -------------------------------------------------
    void Terminate()
    {
        if (m_block.end == Block::End::Fallthrough) {
            m_a.mov(x::eax, asmjit::Imm(m_block.fallPc));
            Epilogue();
            return;
        }

        const Fields f = Decode(m_block.terminator.word);
        const uint32_t pc = m_block.terminator.pc;

        // The branch is decided now, before its delay slot gets to change a register.
        switch (m_block.end) {
        case Block::End::Branch: {
            const uint32_t taken = (pc + 4 + (static_cast<uint32_t>(f.simm) << 2)) & 0xFFC;
            const uint32_t fall  = (pc + 8) & 0xFFC;
            LoadGpr(x::eax, f.rs);
            enum { kE, kNE, kLE, kG, kL, kGE } cc = kE;
            switch (f.op) {
            case 0x04: case 0x05:                                       // BEQ BNE
                m_a.cmp(x::eax, GprMem(f.rt));
                cc = (f.op == 0x04) ? kE : kNE;
                break;
            case 0x06: m_a.cmp(x::eax, asmjit::Imm(0)); cc = kLE; break;  // BLEZ
            case 0x07: m_a.cmp(x::eax, asmjit::Imm(0)); cc = kG; break;   // BGTZ
            default:                                                    // REGIMM
                m_a.cmp(x::eax, asmjit::Imm(0));
                cc = (f.rt == 0 || f.rt == 0x10) ? kL : kGE;
                break;
            }
            // mov does not touch the flags, so the compare above is still what cmov sees
            m_a.mov(kJump, asmjit::Imm(fall));
            m_a.mov(x::ecx, asmjit::Imm(taken));
            switch (cc) {
            case kE:  m_a.cmove(kJump, x::ecx);  break;
            case kNE: m_a.cmovne(kJump, x::ecx); break;
            case kLE: m_a.cmovle(kJump, x::ecx); break;
            case kG:  m_a.cmovg(kJump, x::ecx);  break;
            case kL:  m_a.cmovl(kJump, x::ecx);  break;
            default:  m_a.cmovge(kJump, x::ecx); break;
            }
            if (f.op == 0x01 && (f.rt == 0x10 || f.rt == 0x11)) WriteLink(31, pc);
            break;
        }
        case Block::End::Jump:
            m_a.mov(kJump, asmjit::Imm((f.target << 2) & 0xFFC));
            if (f.op == 0x03) WriteLink(31, pc);
            break;
        default:   // JR / JALR
            LoadGpr(kJump, f.rs);
            m_a.and_(kJump, asmjit::Imm(0xFFC));
            if (f.funct == 0x09) WriteLink(f.rd, pc);
            break;
        }

        Lower(m_block.delay);
        m_a.mov(x::eax, kJump);
        Epilogue();
    }
};

class X64Backend final : public Backend {
public:
    BlockFn Compile(const Block& block) override
    {
        asmjit::CodeHolder code;
        if (code.init(m_runtime.environment()) != asmjit::kErrorOk)
            return nullptr;
        x::Assembler assembler(&code);
        Emitter(assembler, block).Emit();

        BlockFn fn = nullptr;
        if (m_runtime.add(&fn, &code) != asmjit::kErrorOk)
            return nullptr;
        return fn;
    }

    void Release(BlockFn fn) override { m_runtime.release(reinterpret_cast<void*>(fn)); }
    const char* Name() const override { return "x86-64"; }

private:
    asmjit::JitRuntime m_runtime;
};

} // namespace

std::unique_ptr<Backend> CreateBackend()
{
    return std::make_unique<X64Backend>();
}

} // namespace Nova64::RSP::Jit

#endif // x86-64
