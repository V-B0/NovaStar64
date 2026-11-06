/**
 * @file RspJitA64.cpp
 * @brief AArch64 code emitter for the RSP recompiler.
 *
 * Register use inside a compiled block:
 *   x19  scalar register file (uint32_t[32])
 *   x20  DMEM
 *   x21  the CRSPSystem, for calls into the interpreter
 *   w22  where a pending branch goes (set before its delay slot runs)
 *   w9..w15, x16  scratch
 */

#if defined(__aarch64__) || defined(_M_ARM64)

#include "RspJitBackend.hpp"
#include "RspJitDecode.hpp"
#include "RspJitKernels.hpp"

#include <asmjit/core.h>
#include <asmjit/a64.h>

namespace Nova64::RSP::Jit {

namespace a = asmjit::a64;

namespace {

const a::Gp kGpr  = a::x19;
const a::Gp kDmem = a::x20;
const a::Gp kSys  = a::x21;
const a::Gp kJump = a::w22;
const a::Gp kRegs = a::x23;   // the CRSPRegisters, for the vector kernels

// The addressing offset of scalar register `r` in the register file.
int32_t GprOffset(uint32_t r) { return static_cast<int32_t>(r * 4); }

class Emitter {
public:
    Emitter(a::Assembler& as, const Block& block) : m_a(as), m_block(block) {}

    void Emit()
    {
        Prologue();
        for (const Insn& insn : m_block.body)
            Lower(insn, /*delaySlot*/ false);
        Terminate();
    }

private:
    a::Assembler& m_a;
    const Block&  m_block;

    // ---- block frame --------------------------------------------------------
    void Prologue()
    {
        m_a.stp(a::x29, a::x30, a::ptr_pre(a::sp, -64));
        m_a.stp(a::x19, a::x20, a::ptr(a::sp, 16));
        m_a.stp(a::x21, a::x22, a::ptr(a::sp, 32));
        m_a.str(a::x23, a::ptr(a::sp, 48));
        m_a.ldr(kGpr,  a::ptr(a::x0, 0));     // JitContext::gpr
        m_a.ldr(kDmem, a::ptr(a::x0, 8));     // JitContext::dmem
        m_a.ldr(kSys,  a::ptr(a::x0, 16));    // JitContext::sys
        m_a.ldr(kRegs, a::ptr(a::x0, 24));    // JitContext::regs
    }

    // Returns the PC held in `pcReg` (a w register).
    void Epilogue(const a::Gp& pcReg)
    {
        if (pcReg.id() != a::w0.id()) m_a.mov(a::w0, pcReg);
        m_a.ldr(a::x23, a::ptr(a::sp, 48));
        m_a.ldp(a::x21, a::x22, a::ptr(a::sp, 32));
        m_a.ldp(a::x19, a::x20, a::ptr(a::sp, 16));
        m_a.ldp(a::x29, a::x30, a::ptr_post(a::sp, 64));
        m_a.ret(a::x30);
    }

    void EpilogueConst(uint32_t pc)
    {
        m_a.mov(a::w0, asmjit::Imm(pc));
        Epilogue(a::w0);
    }

    // ---- small helpers ------------------------------------------------------
    void LoadGpr(const a::Gp& dst, uint32_t r) { m_a.ldr(dst, a::ptr(kGpr, GprOffset(r))); }
    void StoreGpr(uint32_t r, const a::Gp& src) { if (r != 0) m_a.str(src, a::ptr(kGpr, GprOffset(r))); }
    void WriteLink(uint32_t reg, uint32_t pc)
    {
        if (reg == 0) return;
        m_a.mov(a::w9, asmjit::Imm((pc + 8) & 0xFFC));
        m_a.str(a::w9, a::ptr(kGpr, GprOffset(reg)));
    }

    // Leave it to the interpreter.
    void CallInterpreter(uint32_t word)
    {
        m_a.mov(a::x0, kSys);
        m_a.mov(a::w1, asmjit::Imm(word));
        m_a.mov(a::x16, asmjit::Imm(reinterpret_cast<uint64_t>(&ExecuteOneOnInterpreter)));
        m_a.blr(a::x16);
    }

    // w9 = (gpr[base] + offset) & 0xFFF, the DMEM address a load or store uses.
    void DmemAddress(uint32_t base, int32_t simm)
    {
        LoadGpr(a::w9, base);
        // Only the low 12 bits matter, so the offset can be folded into 12 bits too.
        const uint32_t low = static_cast<uint32_t>(simm) & 0xFFF;
        if (low != 0) m_a.add(a::w9, a::w9, asmjit::Imm(low));
        m_a.and_(a::w9, a::w9, asmjit::Imm(0xFFF));
    }

    // ---- instruction lowering -----------------------------------------------
    void Lower(const Insn& insn, bool /*delaySlot*/)
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
            m_a.mov(a::x0, kRegs);
            m_a.mov(a::x1, kDmem);
            m_a.mov(a::w2, asmjit::Imm(insn.word));
            m_a.mov(a::x16, asmjit::Imm(reinterpret_cast<uint64_t>(kernel)));
            m_a.blr(a::x16);
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

    // Returns false for instructions that are left to the interpreter.
    bool Native(const Insn& insn)
    {
        const Fields f = Decode(insn.word);
        switch (f.op) {
        case 0x00: return NativeSpecial(f);
        case 0x08: case 0x09: {                                   // ADDI ADDIU
            if (f.rt == 0) return true;
            LoadGpr(a::w9, f.rs);
            if (f.simm >= 0 && f.simm <= 4095)        m_a.add(a::w9, a::w9, asmjit::Imm(static_cast<uint32_t>(f.simm)));
            else if (f.simm < 0 && f.simm >= -4095)   m_a.sub(a::w9, a::w9, asmjit::Imm(static_cast<uint32_t>(-f.simm)));
            else { m_a.mov(a::w10, asmjit::Imm(static_cast<uint32_t>(f.simm))); m_a.add(a::w9, a::w9, a::w10); }
            StoreGpr(f.rt, a::w9);
            return true;
        }
        case 0x0A: case 0x0B: {                                   // SLTI SLTIU
            if (f.rt == 0) return true;
            LoadGpr(a::w9, f.rs);
            m_a.mov(a::w10, asmjit::Imm(static_cast<uint32_t>(f.simm)));
            m_a.cmp(a::w9, a::w10);
            m_a.cset(a::w11, f.op == 0x0A ? a::CondCode::kLT : a::CondCode::kLO);
            StoreGpr(f.rt, a::w11);
            return true;
        }
        case 0x0C: case 0x0D: case 0x0E: {                        // ANDI ORI XORI
            if (f.rt == 0) return true;
            LoadGpr(a::w9, f.rs);
            m_a.mov(a::w10, asmjit::Imm(f.imm));
            if (f.op == 0x0C)      m_a.and_(a::w9, a::w9, a::w10);
            else if (f.op == 0x0D) m_a.orr(a::w9, a::w9, a::w10);
            else                   m_a.eor(a::w9, a::w9, a::w10);
            StoreGpr(f.rt, a::w9);
            return true;
        }
        case 0x0F:                                                // LUI
            if (f.rt == 0) return true;
            m_a.mov(a::w9, asmjit::Imm(f.imm << 16));
            StoreGpr(f.rt, a::w9);
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
        case 0x00: case 0x02: case 0x03: {                        // SLL SRL SRA
            if (f.rd == 0) return true;
            LoadGpr(a::w9, f.rt);
            if (f.sa != 0 || f.funct != 0x00) {
                if (f.funct == 0x00)      m_a.lsl(a::w9, a::w9, asmjit::Imm(f.sa));
                else if (f.funct == 0x02) m_a.lsr(a::w9, a::w9, asmjit::Imm(f.sa));
                else                      m_a.asr(a::w9, a::w9, asmjit::Imm(f.sa));
            }
            StoreGpr(f.rd, a::w9);
            return true;
        }
        case 0x04: case 0x06: case 0x07: {                        // SLLV SRLV SRAV
            if (f.rd == 0) return true;
            LoadGpr(a::w9, f.rt);
            LoadGpr(a::w10, f.rs);
            m_a.and_(a::w10, a::w10, asmjit::Imm(31));
            if (f.funct == 0x04)      m_a.lslv(a::w9, a::w9, a::w10);
            else if (f.funct == 0x06) m_a.lsrv(a::w9, a::w9, a::w10);
            else                      m_a.asrv(a::w9, a::w9, a::w10);
            StoreGpr(f.rd, a::w9);
            return true;
        }
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27: {             // ADD ADDU SUB SUBU AND OR XOR NOR
            if (f.rd == 0) return true;
            LoadGpr(a::w9, f.rs);
            LoadGpr(a::w10, f.rt);
            switch (f.funct) {
            case 0x20: case 0x21: m_a.add(a::w9, a::w9, a::w10); break;
            case 0x22: case 0x23: m_a.sub(a::w9, a::w9, a::w10); break;
            case 0x24: m_a.and_(a::w9, a::w9, a::w10); break;
            case 0x25: m_a.orr(a::w9, a::w9, a::w10); break;
            case 0x26: m_a.eor(a::w9, a::w9, a::w10); break;
            default:   m_a.orr(a::w9, a::w9, a::w10); m_a.mvn(a::w9, a::w9); break;
            }
            StoreGpr(f.rd, a::w9);
            return true;
        }
        case 0x2A: case 0x2B: {                                   // SLT SLTU
            if (f.rd == 0) return true;
            LoadGpr(a::w9, f.rs);
            LoadGpr(a::w10, f.rt);
            m_a.cmp(a::w9, a::w10);
            m_a.cset(a::w11, f.funct == 0x2A ? a::CondCode::kLT : a::CondCode::kLO);
            StoreGpr(f.rd, a::w11);
            return true;
        }
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
        if (word) { m_a.tst(a::w9, asmjit::Imm(3)); m_a.b_ne(slow); }
        if (half) { m_a.tst(a::w9, asmjit::Imm(1)); m_a.b_ne(slow); }

        if (word) {
            m_a.ldr(a::w10, a::ptr(kDmem, a::x9));
        } else if (half) {
            m_a.eor(a::w11, a::w9, asmjit::Imm(2));
            if (f.op == 0x21) m_a.ldrsh(a::w10, a::ptr(kDmem, a::x11));
            else              m_a.ldrh(a::w10, a::ptr(kDmem, a::x11));
        } else {
            m_a.eor(a::w11, a::w9, asmjit::Imm(3));
            if (f.op == 0x20) m_a.ldrsb(a::w10, a::ptr(kDmem, a::x11));
            else              m_a.ldrb(a::w10, a::ptr(kDmem, a::x11));
        }
        StoreGpr(f.rt, a::w10);

        if (word || half) {
            m_a.b(done);
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
        if (word) { m_a.tst(a::w9, asmjit::Imm(3)); m_a.b_ne(slow); }
        if (half) { m_a.tst(a::w9, asmjit::Imm(1)); m_a.b_ne(slow); }

        LoadGpr(a::w10, f.rt);
        if (word) {
            m_a.str(a::w10, a::ptr(kDmem, a::x9));
        } else if (half) {
            m_a.eor(a::w11, a::w9, asmjit::Imm(2));
            m_a.strh(a::w10, a::ptr(kDmem, a::x11));
        } else {
            m_a.eor(a::w11, a::w9, asmjit::Imm(3));
            m_a.strb(a::w10, a::ptr(kDmem, a::x11));
        }

        if (word || half) {
            m_a.b(done);
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
            EpilogueConst(m_block.fallPc);
            return;
        }

        const Fields f = Decode(m_block.terminator.word);
        const uint32_t pc = m_block.terminator.pc;

        // The branch is decided now, before its delay slot gets to change a register.
        switch (m_block.end) {
        case Block::End::Branch: {
            const uint32_t taken = (pc + 4 + (static_cast<uint32_t>(f.simm) << 2)) & 0xFFC;
            const uint32_t fall  = (pc + 8) & 0xFFC;
            a::CondCode cc = a::CondCode::kEQ;
            LoadGpr(a::w9, f.rs);
            switch (f.op) {
            case 0x04: case 0x05:                                       // BEQ BNE
                LoadGpr(a::w10, f.rt);
                m_a.cmp(a::w9, a::w10);
                cc = (f.op == 0x04) ? a::CondCode::kEQ : a::CondCode::kNE;
                break;
            case 0x06: m_a.cmp(a::w9, asmjit::Imm(0)); cc = a::CondCode::kLE; break;   // BLEZ
            case 0x07: m_a.cmp(a::w9, asmjit::Imm(0)); cc = a::CondCode::kGT; break;   // BGTZ
            default:                                                    // REGIMM
                m_a.cmp(a::w9, asmjit::Imm(0));
                cc = (f.rt == 0 || f.rt == 0x10) ? a::CondCode::kLT : a::CondCode::kGE;
                break;
            }
            m_a.mov(a::w11, asmjit::Imm(taken));
            m_a.mov(kJump, asmjit::Imm(fall));
            m_a.csel(kJump, a::w11, kJump, cc);
            if (f.op == 0x01 && (f.rt == 0x10 || f.rt == 0x11)) WriteLink(31, pc);
            break;
        }
        case Block::End::Jump:
            m_a.mov(kJump, asmjit::Imm((f.target << 2) & 0xFFC));
            if (f.op == 0x03) WriteLink(31, pc);
            break;
        default: {   // JR / JALR
            LoadGpr(kJump, f.rs);
            m_a.and_(kJump, kJump, asmjit::Imm(0xFFC));
            if (f.funct == 0x09) WriteLink(f.rd, pc);
            break;
        }
        }

        Lower(m_block.delay, /*delaySlot*/ true);
        Epilogue(kJump);
    }
};

class A64Backend final : public Backend {
public:
    BlockFn Compile(const Block& block) override
    {
        asmjit::CodeHolder code;
        if (code.init(m_runtime.environment()) != asmjit::kErrorOk)
            return nullptr;
        a::Assembler assembler(&code);
        Emitter(assembler, block).Emit();

        BlockFn fn = nullptr;
        if (m_runtime.add(&fn, &code) != asmjit::kErrorOk)
            return nullptr;
        return fn;
    }

    void Release(BlockFn fn) override { m_runtime.release(reinterpret_cast<void*>(fn)); }
    const char* Name() const override { return "arm64"; }

private:
    asmjit::JitRuntime m_runtime;
};

} // namespace

std::unique_ptr<Backend> CreateBackend()
{
    return std::make_unique<A64Backend>();
}

} // namespace Nova64::RSP::Jit

#endif // AArch64
