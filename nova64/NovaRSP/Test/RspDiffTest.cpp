/**
 * @file RspDiffTest.cpp
 * @brief Randomised differential test for the RSP engines. See RspDiffTest.hpp.
 */

#include "RspDiffTest.hpp"

#include "../CPU/RSPCpu.hpp"
#include "../CPU/RSPRomTables.hpp"
#include "../CPU/RspMemory.hpp"
#include "../CPU/RspSystem.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace Nova64::RSP::Test {

namespace {

constexpr uint32_t kRdramSize = 0x100000;   // 1 MB is plenty for the DMA the programs do

void NullCallback() {}

// ---------------------------------------------------------------------------
// One complete RSP with its own memory, hardware registers and RDRAM.
// ---------------------------------------------------------------------------
class Rig {
public:
    explicit Rig(Engine engine) : m_Engine(engine), m_Rdram(kRdramSize)
    {
        // SP registers must be contiguous and in this order: the register handler
        // reaches them all through a pointer to the first.
        RSP_INFO info{};
        info.HEADER           = m_Header;
        info.RDRAM            = m_Rdram.data();
        info.DMEM             = m_Dmem;
        info.IMEM             = m_Imem;
        info.MI_INTR_REG      = &m_MiIntr;
        info.SP_MEM_ADDR_REG  = &m_Sp[0];
        info.SP_DRAM_ADDR_REG = &m_Sp[1];
        info.SP_RD_LEN_REG    = &m_Sp[2];
        info.SP_WR_LEN_REG    = &m_Sp[3];
        info.SP_STATUS_REG    = &m_Sp[4];
        info.SP_DMA_FULL_REG  = &m_Sp[5];
        info.SP_DMA_BUSY_REG  = &m_Sp[6];
        info.SP_SEMAPHORE_REG = &m_Sp[7];
        info.SP_PC_REG        = &m_Sp[8];
        info.DPC_START_REG    = &m_Dpc[0];
        info.DPC_END_REG      = &m_Dpc[1];
        info.DPC_CURRENT_REG  = &m_Dpc[2];
        info.DPC_STATUS_REG   = &m_Dpc[3];
        info.DPC_CLOCK_REG    = &m_Dpc[4];
        info.DPC_BUFBUSY_REG  = &m_Dpc[5];
        info.DPC_PIPEBUSY_REG = &m_Dpc[6];
        info.DPC_TMEM_REG     = &m_Dpc[7];
        info.RdramSize        = kRdramSize;
        info.CheckInterrupts  = NullCallback;
        info.ProcessDList     = NullCallback;
        info.ProcessRdpList   = NullCallback;
        info.ProcessDListShadow = NullCallback;

        m_System.reset(new CRSPSystem());
        m_System->Reset(info);
        m_System->SetEngine(engine == Engine::Recompiler ? RSPEngine::Recompiler
                                                         : RSPEngine::Interpreter);
        auto& reg = m_System->Registers();
        std::memcpy(reg.m_Reciprocals, RcpRom, sizeof(RcpRom));
        std::memcpy(reg.m_InverseSquareRoots, RsqRom, sizeof(RsqRom));
    }

    /// Load a program and fill every piece of state from `rng` (so two rigs given
    /// the same seed start out bit-identical).
    void Setup(uint64_t stateSeed, const std::vector<uint32_t>& program, uint32_t entry)
    {
        std::mt19937_64 rng(stateSeed);
        auto r32 = [&] { return static_cast<uint32_t>(rng()); };

        std::memset(m_Imem, 0, sizeof(m_Imem));
        for (size_t i = 0; i < program.size(); ++i) {
            const uint32_t at = (entry + static_cast<uint32_t>(i) * 4) & 0xFFC;
            WriteBE32(m_Imem + at, program[i]);
        }
        for (auto& b : m_Dmem) b = static_cast<uint8_t>(rng());
        for (auto& b : m_Rdram) b = static_cast<uint8_t>(rng());

        m_System->ResetPipeline();
        auto& reg = m_System->Registers();
        for (int i = 1; i < 32; ++i) reg.m_GPR[i].UW = r32();
        reg.m_GPR[0].UW = 0;
        reg.m_GPR[29].UW = 0;   // the loop counter, see Generator
        for (auto& v : reg.m_Vect) { v.u64(0) = rng(); v.u64(1) = rng(); }
        for (int part = 0; part < 3; ++part) {
            void* p = part == 0 ? reg.m_ACCUM.LowData()
                    : part == 1 ? reg.m_ACCUM.MidData() : reg.m_ACCUM.HighData();
            const uint64_t a = rng(), b = rng();
            std::memcpy(p, &a, 8);
            std::memcpy(static_cast<uint8_t*>(p) + 8, &b, 8);
        }
        auto setFlag = [&](RSPFlag& f) { for (int i = 0; i < 8; ++i) f.Set(i, (rng() & 1) != 0); };
        setFlag(reg.m_VCOL); setFlag(reg.m_VCOH);
        setFlag(reg.m_VCCL); setFlag(reg.m_VCCH);
        setFlag(reg.m_VCE);
        reg.m_Result = static_cast<uint16_t>(rng());
        reg.m_In     = static_cast<uint16_t>(rng());
        reg.m_High   = (rng() & 1) != 0;

        m_MiIntr = 0;
        m_Sp[0] = r32() & 0x1FF8;
        m_Sp[1] = (r32() % (kRdramSize - 0x4000)) & ~7u;
        m_Sp[2] = 0xFF8;
        m_Sp[3] = 0xFF8;
        m_Sp[4] = SPStatus::Halt | (r32() & 0x7F80);   // halted, random signal bits
        m_Sp[5] = 0;
        m_Sp[6] = 0;
        m_Sp[7] = r32() & 1;
        m_Sp[8] = entry & 0xFFC;
        for (auto& d : m_Dpc) d = r32() & 0x00FFFFF8;
        m_Dpc[3] = 0;   // DPC status
    }

    /// Run the program. With a `budget` the interpreter gives up after that many
    /// instructions and this returns false (a DMA over the program can turn it into
    /// anything, including an endless loop). The recompiler has no budget and is only
    /// run on programs the interpreter already finished.
    bool Run(uint32_t budget)
    {
        if (m_Engine == Engine::Recompiler) {
            m_System->Run(0xFFFFFFFFu);
            return true;
        }
        m_System->ExecuteOps(budget, 0xFFFFFFFFu);
        return RSP_Running == 0;
    }

    /// Everything a program can change, flattened, with a name per section.
    struct Section { const char* name; size_t offset; size_t size; };
    std::vector<uint8_t> Snapshot(std::vector<Section>& sections) const
    {
        std::vector<uint8_t> out;
        sections.clear();
        auto add = [&](const char* name, const void* data, size_t size) {
            sections.push_back({name, out.size(), size});
            const auto* p = static_cast<const uint8_t*>(data);
            out.insert(out.end(), p, p + size);
        };
        const auto& reg = m_System->Registers();
        uint32_t gpr[32];
        for (int i = 0; i < 32; ++i) gpr[i] = reg.m_GPR[i].UW;
        add("GPR", gpr, sizeof(gpr));
        add("VR", reg.m_Vect, sizeof(reg.m_Vect));
        add("ACC.low",  reg.m_ACCUM.LowData(),  16);
        add("ACC.mid",  reg.m_ACCUM.MidData(),  16);
        add("ACC.high", reg.m_ACCUM.HighData(), 16);
        add("VCOL", reg.m_VCOL.Value(), 16);
        add("VCOH", reg.m_VCOH.Value(), 16);
        add("VCCL", reg.m_VCCL.Value(), 16);
        add("VCCH", reg.m_VCCH.Value(), 16);
        add("VCE",  reg.m_VCE.Value(),  16);
        const uint16_t div[3] = {reg.m_Result, reg.m_In, static_cast<uint16_t>(reg.m_High ? 1 : 0)};
        add("DIV", div, sizeof(div));
        add("SP regs", m_Sp, sizeof(m_Sp));
        add("DPC regs", m_Dpc, sizeof(m_Dpc));
        add("MI_INTR", &m_MiIntr, sizeof(m_MiIntr));
        add("DMEM", m_Dmem, sizeof(m_Dmem));
        add("IMEM", m_Imem, sizeof(m_Imem));
        add("RDRAM", m_Rdram.data(), m_Rdram.size());
        return out;
    }

private:
    static void WriteBE32(uint8_t* p, uint32_t v)
    {
        // IMEM is read as a host-endian word (see CRSPSystem::ExecuteOps), so store it that way.
        std::memcpy(p, &v, 4);
    }

    Engine                      m_Engine;
    std::vector<uint8_t>        m_Rdram;
    alignas(16) uint8_t         m_Imem[IMEM_SIZE]{};
    alignas(16) uint8_t         m_Dmem[DMEM_SIZE]{};
    uint8_t                     m_Header[0x1000]{};
    uint32_t                    m_Sp[9]{};
    uint32_t                    m_Dpc[8]{};
    uint32_t                    m_MiIntr{0};
    std::unique_ptr<CRSPSystem> m_System;
};

// ---------------------------------------------------------------------------
// Random program generator
// ---------------------------------------------------------------------------
class Generator {
public:
    explicit Generator(uint64_t seed) : m_Rng(seed) {}

    struct Program {
        std::vector<uint32_t> words;
        uint32_t              entry = 0;
    };

    Program Make(uint32_t maxLength)
    {
        Program p;
        p.entry = (Pick(0, 0x40) * 4) & 0xFFC;
        const uint32_t target = 8 + Pick(0, maxLength > 8 ? maxLength - 8 : 1);
        while (p.words.size() < target) EmitOne(p.words, target);
        // Every path ends in BREAK, with a few spare so a skipped-over BREAK still lands on one.
        for (int i = 0; i < 12; ++i) p.words.push_back(Break());
        return p;
    }

private:
    // ---- encoders -----------------------------------------------------------
    static uint32_t R(uint32_t funct, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa = 0)
    { return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct; }
    static uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
    { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }
    static uint32_t Break() { return 0x0000000D; }

    uint32_t Pick(uint32_t lo, uint32_t hi)   // inclusive
    { return lo + static_cast<uint32_t>(m_Rng() % (hi - lo + 1)); }
    bool Chance(uint32_t percent) { return Pick(0, 99) < percent; }
    // Destination registers avoid r28..r31 (loop counters, jump targets, link).
    uint32_t Dst() { return Pick(1, 27); }
    uint32_t Src() { return Pick(0, 31); }
    uint32_t Vec() { return Pick(0, 31); }

    // ---- instruction groups -----------------------------------------------
    uint32_t Alu()
    {
        static const uint32_t rtype[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B,
                                         0x04, 0x06, 0x07};   // ADD ADDU SUB SUBU AND OR XOR NOR SLT SLTU SLLV SRLV SRAV
        switch (Pick(0, 4)) {
        case 0: return R(rtype[Pick(0, 12)], Src(), Src(), Dst());
        case 1: return R(Pick(0, 2) == 0 ? 0x00 : Pick(0, 1) ? 0x02 : 0x03, 0, Src(), Dst(), Pick(0, 31)); // SLL SRL SRA
        case 2: {
            static const uint32_t iops[] = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};   // ADDI ADDIU SLTI SLTIU ANDI ORI XORI
            return I(iops[Pick(0, 6)], Src(), Dst(), Pick(0, 0xFFFF));
        }
        case 3: return I(0x0F, 0, Dst(), Pick(0, 0xFFFF));   // LUI
        default: return R(rtype[Pick(0, 12)], Src(), Src(), Dst());
        }
    }

    uint32_t ScalarMem()
    {
        static const uint32_t ld[] = {0x20, 0x21, 0x23, 0x24, 0x25, 0x27};   // LB LH LW LBU LHU LWU
        static const uint32_t st[] = {0x28, 0x29, 0x2B};                     // SB SH SW
        const uint32_t imm = Pick(0, 0xFFF);
        if (Chance(60)) return I(ld[Pick(0, 5)], Src(), Dst(), imm);
        return I(st[Pick(0, 2)], Src(), Src(), imm);
    }

    uint32_t Cop0()
    {
        // SP_STATUS (4) is picked rarely: a random write sets Halt about half the time, which ends the program.
        static const uint32_t regs[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12};
        const uint32_t rd = Chance(6) ? 4 : regs[Pick(0, 11)];
        if (Chance(55)) return (0x10u << 26) | (0u << 21) | (Dst() << 16) | (rd << 11);          // MFC0
        return (0x10u << 26) | (4u << 21) | (Src() << 16) | (rd << 11);                           // MTC0
    }

    uint32_t Cop2Scalar()
    {
        static const uint32_t sub[] = {0, 2, 4, 6};   // MFC2 CFC2 MTC2 CTC2
        const uint32_t s = sub[Pick(0, 3)];
        const bool toScalar = (s == 0 || s == 2);          // MFC2 and CFC2 write the scalar register
        return (0x12u << 26) | (s << 21) | ((toScalar ? Dst() : Src()) << 16)
             | (Vec() << 11) | (Pick(0, 15) << 7);
    }

    uint32_t VectorOp()
    {
        const uint32_t e = Pick(0, 15);
        return (0x12u << 26) | (1u << 25) | (e << 21) | (Vec() << 16) | (Vec() << 11) | (Vec() << 6) | Pick(0, 0x3F);
    }

    uint32_t VectorMem()
    {
        const bool load = Chance(55);
        const uint32_t op  = load ? 0x32 : 0x3A;
        const uint32_t sub = Pick(0, 0xF);   // byte, short, long, double, quad, rest, packed, ... transpose
        return (op << 26) | (Src() << 21) | (Vec() << 16) | (sub << 11) | (Pick(0, 15) << 7) | Pick(0, 0x7F);
    }

    uint32_t Simple()
    {
        const uint32_t k = Pick(0, 99);
        if (k < 30) return Alu();
        if (k < 45) return ScalarMem();
        if (k < 50) return Cop0();
        if (k < 58) return Cop2Scalar();
        if (k < 82) return VectorOp();
        return VectorMem();
    }

    // ---- control flow -------------------------------------------------------
    // All branches go forward, so every program terminates. `end` is the length the
    // program is aiming for; targets stay inside it (there are BREAKs behind it).
    void EmitOne(std::vector<uint32_t>& w, uint32_t end)
    {
        const uint32_t here = static_cast<uint32_t>(w.size());
        const uint32_t k = Pick(0, 99);
        if (k < 70 || here + 6 >= end) { w.push_back(Simple()); return; }

        if (k < 82) {   // conditional branch, forward, with a delay slot
            const uint32_t skip = Pick(1, 6);
            const uint32_t off  = skip;                              // relative to the delay slot
            switch (Pick(0, 5)) {
            case 0: w.push_back(I(0x04, Src(), Src(), off)); break;  // BEQ
            case 1: w.push_back(I(0x05, Src(), Src(), off)); break;  // BNE
            case 2: w.push_back(I(0x06, Src(), 0, off)); break;      // BLEZ
            case 3: w.push_back(I(0x07, Src(), 0, off)); break;      // BGTZ
            case 4: w.push_back(I(0x01, Src(), Chance(50) ? 0 : 1, off)); break;    // BLTZ BGEZ
            default: w.push_back(I(0x01, Src(), Chance(50) ? 0x10 : 0x11, off)); break; // BLTZAL BGEZAL
            }
            DelaySlot(w);
            return;
        }
        if (k < 88) {   // J / JAL
            const uint32_t skip = Pick(1, 6);
            const uint32_t destWord = (w.size() + 1 + skip);
            // The jump target is an absolute IMEM address, so it needs the entry offset;
            // the caller adjusts it (see FixJumps).
            const uint32_t op = Chance(50) ? 0x02u : 0x03u;
            w.push_back((op << 26) | (destWord & 0x3FFFFFF) | kJumpMarker);
            DelaySlot(w);
            return;
        }
        if (k < 94) {   // JR / JALR through a register the generator loads first
            const uint32_t skip = Pick(1, 6);
            const uint32_t destWord = (w.size() + 2 + skip);
            w.push_back(kLoadTargetMarker | (destWord & 0xFFFF));            // becomes ORI r28, r0, entry+dest*4
            if (Chance(50)) w.push_back(R(0x08, 28, 0, 0));                  // JR r28
            else            w.push_back(R(0x09, 28, 0, Dst()));              // JALR rd, r28
            DelaySlot(w);
            return;
        }
        // small counted loop (backward branch): r29 = n; body; r29--; BNE r29, r0, body
        const uint32_t n = Pick(1, 4);
        w.push_back(I(0x09, 0, 29, n));                                       // ADDIU r29, r0, n
        const uint32_t bodyLen = Pick(1, 4);
        for (uint32_t i = 0; i < bodyLen; ++i) w.push_back(Simple());
        w.push_back(I(0x09, 29, 29, 0xFFFF));                                 // ADDIU r29, r29, -1
        const int32_t back = -static_cast<int32_t>(bodyLen + 2);
        // BGTZ rather than BNE: a forward jump into the loop that skips the counter
        // set-up still leaves r29 at 0, so the loop runs once instead of 2^32 times.
        w.push_back(I(0x07, 29, 0, static_cast<uint32_t>(back)));             // BGTZ r29, body
        w.push_back(Simple());                                                // delay slot
    }

    void DelaySlot(std::vector<uint32_t>& w)
    {
        // Occasionally a branch in a delay slot, which the RSP defines.
        if (Chance(3)) {
            w.push_back(I(0x05, Src(), Src(), Pick(1, 3)));
            w.push_back(Simple());
        } else {
            w.push_back(Simple());
        }
    }

public:
    static constexpr uint32_t kJumpMarker       = 0;            // J/JAL words are patched by Finish()
    static constexpr uint32_t kLoadTargetMarker = 0xFC000000u;  // placeholder opcode, patched by Finish()

    /// Turn the position-independent words into real ones for the chosen entry point.
    static void Finish(Program& p)
    {
        for (size_t i = 0; i < p.words.size(); ++i) {
            uint32_t& w = p.words[i];
            const uint32_t op = w >> 26;
            if (op == 0x3F) {                                        // ORI r28, r0, entry + word*4
                const uint32_t dest = (p.entry + (w & 0xFFFF) * 4) & 0xFFC;
                w = (0x0Du << 26) | (0u << 21) | (28u << 16) | dest;
            } else if (op == 0x02 || op == 0x03) {                   // J / JAL
                const uint32_t dest = (p.entry + (w & 0x3FFFFFF) * 4) & 0xFFC;
                w = (op << 26) | (dest >> 2);
            }
        }
    }

private:
    std::mt19937_64 m_Rng;
};

std::string Hex(const std::vector<uint32_t>& words)
{
    std::string s;
    char buf[16];
    for (size_t i = 0; i < words.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%08X%c", words[i], (i % 8 == 7) ? '\n' : ' ');
        s += buf;
    }
    return s;
}

} // namespace

int RunDifferentialTest(const DiffTestOptions& opt)
{
    Build_RSP();   // element-selection tables the vector ops share

    Rig reference(opt.reference);
    Rig candidate(opt.candidate);

    uint32_t failures = 0, ran = 0, skipped = 0;
    for (uint32_t index = 0; index < opt.programs; ++index) {
        if (opt.only != 0xFFFFFFFFu && index != opt.only) continue;
        const uint64_t programSeed = opt.seed * 0x9E3779B97F4A7C15ull + index;

        Generator gen(programSeed);
        auto program = gen.Make(opt.maxLength);
        Generator::Finish(program);

        reference.Setup(programSeed ^ 0xA5A5A5A5ull, program.words, program.entry);
        candidate.Setup(programSeed ^ 0xA5A5A5A5ull, program.words, program.entry);
        if (!reference.Run(500000)) { ++skipped; continue; }
        candidate.Run(0);
        ++ran;

        std::vector<Rig::Section> sections;
        const auto a = reference.Snapshot(sections);
        std::vector<Rig::Section> sectionsB;
        const auto b = candidate.Snapshot(sectionsB);
        if (a == b) continue;

        ++failures;
        for (const auto& s : sections) {
            if (std::memcmp(a.data() + s.offset, b.data() + s.offset, s.size) == 0) continue;
            size_t first = 0;
            while (a[s.offset + first] == b[s.offset + first]) ++first;
            std::printf("MISMATCH program %u (seed %llu): %s byte %zu: reference %02X candidate %02X\n",
                        index, static_cast<unsigned long long>(opt.seed), s.name, first,
                        a[s.offset + first], b[s.offset + first]);
        }
        if (opt.verbose)
            std::printf("entry 0x%03X, %zu words:\n%s", program.entry, program.words.size(), Hex(program.words).c_str());
        if (failures >= 10) { std::printf("stopping after 10 failures\n"); break; }
    }

    std::printf("RSP differential test: %u programs compared, %u differed, %u skipped as endless (seed %llu)\n",
                ran, failures, skipped, static_cast<unsigned long long>(opt.seed));
    return failures == 0 ? 0 : 1;
}

} // namespace Nova64::RSP::Test
