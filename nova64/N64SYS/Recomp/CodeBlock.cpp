/**
 * @file CodeBlock.cpp
 * @brief Code block management for the dynamic recompiler
 *
 * Nova64 N64 Emulator - Based on Project64 architecture
 * Ported from Project64's CCodeBlock implementation, adapted to
 * SuperNova64's class hierarchy and naming conventions.
 */

#include "CodeBlock.hpp"
#include "CodeSection.hpp"
#include "RecompilerMemory.hpp"
#include "../../common/MemoryManagment.hpp"
#include "../../common/log.hpp"
#include "../../common/md5.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../MIPSY/R4300Iops.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/Register.hpp"
#include "ExitInfo.hpp"

#if defined(__amd64__) || defined(_M_X64)
#include "x64-86/x64RecompilerOps.hpp"
#elif defined(__i386__) || defined(_M_IX86)
#include "x86/x86RecompilerOps.hpp"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "Arm/ArmRecompilerOps.hpp"
#endif

#include <cstring>
#include <cstdarg>
#include <chrono>
#include <algorithm>

namespace Nova64 {
namespace Recompiler {

// ============================================================================
// Inline helpers for safe MIPS instruction field extraction
// (Avoids reliance on bitfield struct ordering which can be compiler-dependent)
// ============================================================================
namespace {

inline uint32_t mips_op(uint32_t instr)     { return (instr >> 26) & 0x3F; }
inline uint32_t mips_rs(uint32_t instr)     { return (instr >> 21) & 0x1F; }
inline uint32_t mips_rt(uint32_t instr)     { return (instr >> 16) & 0x1F; }
inline uint32_t mips_rd(uint32_t instr)     { return (instr >> 11) & 0x1F; }
inline uint32_t mips_sa(uint32_t instr)     { return (instr >>  6) & 0x1F; }
inline uint32_t mips_funct(uint32_t instr)  { return  instr        & 0x3F; }
inline int16_t  mips_offset(uint32_t instr) { return static_cast<int16_t>(instr & 0xFFFF); }
inline uint32_t mips_target(uint32_t instr) { return  instr & 0x03FFFFFF; }
inline uint32_t mips_fmt(uint32_t instr)    { return mips_rs(instr); }  // COP1 fmt = rs field
inline uint32_t mips_ft(uint32_t instr)     { return mips_rt(instr); }  // COP1 ft = rt field

} // anonymous namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

CodeBlock::CodeBlock(N64System& system, uint32_t entry_address, const Config& config,
                     asmjit::JitRuntime* shared_runtime)
    : system_(&system)
    , memory_(system.GetMemory())
    , registers_(system.GetRegisters())
    , entry_address_(entry_address)
    , start_address_(entry_address)
    , end_address_(entry_address)
    , hash_{0, 0}
    , flags_(BlockFlags::None)
    , config_(config)
{
    // Use shared runtime if provided, otherwise create our own
    if (shared_runtime) {
        runtime_ = shared_runtime;
    } else {
        owned_runtime_ = std::make_unique<asmjit::JitRuntime>();
        runtime_ = owned_runtime_.get();
    }

    code_holder_ = std::make_unique<asmjit::CodeHolder>();
    code_holder_->init(runtime_->environment());
    code_holder_->set_error_handler(this);

    // Take a memory snapshot for self-modifying code detection
    if (memory_) {
        update_memory_snapshot();
    }
}

CodeBlock::~CodeBlock() noexcept {
    // Release compiled code from the JIT runtime (shared or owned).
    // For shared runtimes this is essential to free executable memory.
    if (compiled_function_ && runtime_) {
        runtime_->release(reinterpret_cast<void*>(compiled_function_));
        compiled_function_ = nullptr;
    }
}

CodeBlock::CodeBlock(CodeBlock&& other) noexcept
    : asmjit::ErrorHandler()
    , system_(other.system_)
    , memory_(other.memory_)
    , registers_(other.registers_)
    , entry_address_(other.entry_address_)
    , start_address_(other.start_address_)
    , end_address_(other.end_address_)
    , hash_(other.hash_)
    , flags_(other.flags_)
    , config_(other.config_)
    , instructions_(std::move(other.instructions_))
    , sections_(std::move(other.sections_))
    , address_to_section_(std::move(other.address_to_section_))
    , owned_runtime_(std::move(other.owned_runtime_))
    , runtime_(other.runtime_)
    , code_holder_(std::move(other.code_holder_))
    , compiled_function_(other.compiled_function_)
    , memory_snapshot_(std::move(other.memory_snapshot_))
    , statistics_(other.statistics_)
    , code_log_(std::move(other.code_log_))
    , generated_code_cache_(std::move(other.generated_code_cache_))
{
    // Prevent source from double-releasing the compiled code
    other.runtime_ = nullptr;
    other.compiled_function_ = nullptr;
}
// Move assignment must clear source's compiled_function_ to prevent double-release
CodeBlock& CodeBlock::operator=(CodeBlock&& other) noexcept {
    if (this != &other) {
        // Release our own code first
        if (compiled_function_ && runtime_) {
            runtime_->release(reinterpret_cast<void*>(compiled_function_));
        }
        // Move all members
        system_ = other.system_;
        memory_ = other.memory_;
        registers_ = other.registers_;
        entry_address_ = other.entry_address_;
        start_address_ = other.start_address_;
        end_address_ = other.end_address_;
        hash_ = other.hash_;
        flags_ = other.flags_;
        config_ = other.config_;
        instructions_ = std::move(other.instructions_);
        sections_ = std::move(other.sections_);
        address_to_section_ = std::move(other.address_to_section_);
        owned_runtime_ = std::move(other.owned_runtime_);
        runtime_ = other.runtime_;
        code_holder_ = std::move(other.code_holder_);
        compiled_function_ = other.compiled_function_;
        memory_snapshot_ = std::move(other.memory_snapshot_);
        statistics_ = other.statistics_;
        code_log_ = std::move(other.code_log_);
        generated_code_cache_ = std::move(other.generated_code_cache_);
        // Clear source to prevent double-release
        other.runtime_ = nullptr;
        other.compiled_function_ = nullptr;
    }
    return *this;
}

// ============================================================================
// Compilation
// ============================================================================

CodeBlock::CompileResult CodeBlock::compile(const CompileOptions& options)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 1: Analyze the block
    auto analysis = analyze_block();
    if (!analysis.has_value()) {
        return CompileResult::FailedAnalysis;
    }

    // Step 2: Generate code
    if (!generate_code(analysis.value(), options)) {
        return CompileResult::FailedCodeGen;
    }

    // Step 3: Update statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    statistics_.compilation_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    statistics_.instruction_count = instructions_.size();

    // Step 4: Update hash for self-modifying code detection
    update_hash();

    return CompileResult::Success;
}

std::optional<uintptr_t> CodeBlock::finalize(RecompilationMemory& /*recomp_mem*/)
{
    if (!code_holder_) {
        return std::nullopt;
    }

    // Get the code size from asmjit
    // Assuming section 0 is the text section
    asmjit::Section* textSection = code_holder_->text_section();
    size_t code_size = textSection ? textSection->buffer_size() : 0;
    if (code_size == 0) {
        return std::nullopt;
    }

    // Use the runtime to add the code
    void* fn_ptr = nullptr;
    asmjit::Error err = runtime_->add(&fn_ptr, code_holder_.get());
    compiled_function_ = reinterpret_cast<ExecutionFunction>(fn_ptr);
    if (err != asmjit::kErrorOk) {
        // Simple error reporting since asmjit::DebugUtils is gone
        log_error("AsmJit finalize failed: Error " + std::to_string(static_cast<uint32_t>(err)));
        return std::nullopt;
    }

    statistics_.generated_bytes = code_size;

    if (config_.enable_logging) {
        log_compilation("Block finalized: entry=0x" +
                       std::to_string(entry_address_) +
                       " size=" + std::to_string(code_size) + " bytes");
    }

    return reinterpret_cast<uintptr_t>(compiled_function_);
}

// ============================================================================
// Analysis - Block Level
// ============================================================================

std::optional<CodeBlock::AnalysisResult> CodeBlock::analyze_block()
{
    if (!memory_) {
        return std::nullopt;
    }

    AnalysisResult result;
    result.flags = BlockFlags::None;

    instructions_.clear();

    // Analyze starting from entry address, building basic blocks
    auto bb = analyze_basic_block(entry_address_, result);
    if (!bb.has_value()) {
        return std::nullopt;
    }

    result.basic_blocks.push_back(bb.value());
    start_address_ = bb->start_address;
    end_address_ = bb->end_address;
    result.flags = bb->flags;

    statistics_.basic_block_count = result.basic_blocks.size();
    return result;
}

std::optional<BasicBlockInfo> CodeBlock::analyze_basic_block(uint32_t start_addr, AnalysisResult& temp_result)
{
    BasicBlockInfo bb;
    bb.start_address = start_addr;
    bb.end_address = start_addr;
    bb.flags = BlockFlags::None;
    const uint32_t page_end = ((start_addr + 0x1000) & 0xFFFFF000);

    for (uint32_t pc = start_addr; pc < page_end; pc += 4) {
        // Read instruction from memory
        uint32_t raw_instr = 0;
        if (!memory_->MemoryValue32(pc, raw_instr)) {
            log_error("Failed to read instruction at 0x" + std::to_string(pc));
            break;
        }

        instructions_.push_back(raw_instr);
        bb.end_address = pc;

        // Analyze the instruction for control flow
        if (!analyze_instruction(pc, raw_instr, temp_result, bb)) {
            log_error("Failed to analyze instruction at 0x" + std::to_string(pc));
            return std::nullopt;
        }

        // Check if any branch info was added (means we hit a control flow change)
        if (!temp_result.branches.empty()) {
            auto& last_branch = temp_result.branches.back();

            // Include delay slot instruction if needed.
            // The delay slot MUST be included even when it crosses a page
            // boundary — the MIPS architecture always fetches and executes
            // the delay slot regardless of page alignment.  The previous
            // check (pc + 4 < page_end) incorrectly excluded delay slots
            // at the exact page boundary, causing blocks to undercount
            // instructions and skip the delay slot entirely.
            if (last_branch.delay_slot) {
                uint32_t delay_instr = 0;
                if (memory_->MemoryValue32(pc + 4, delay_instr)) {
                    instructions_.push_back(delay_instr);
                    bb.end_address = pc + 4;
                    temp_result.delay_slots.push_back(pc + 4);
                }
            }

            bb.flags = bb.flags | BlockFlags::HasConditional;

            // Add successor addresses
            if (last_branch.target != static_cast<uint32_t>(-1)) {
                bb.successors.push_back(last_branch.target);
            }

            break;
        }

        // Check for end block markers in temp_result
        if (temp_result.contains_syscall || temp_result.contains_breakpoint) {
            break;
        }

        // Safety: limit block size
        if (instructions_.size() >= config_.max_instructions) {
            break;
        }
    }

    bb.flags = bb.flags | temp_result.flags;
    return bb;
}

// ============================================================================
// Analysis - Instruction Level
// Ported from Project64's CCodeBlock::AnalyzeInstruction
// ============================================================================

bool CodeBlock::analyze_instruction(uint32_t pc, uint32_t opcode,
                                    AnalysisResult& result,
                                    BasicBlockInfo& /*current_block*/)
{
    const uint32_t op    = mips_op(opcode);
    const uint32_t rs    = mips_rs(opcode);
    const uint32_t rt    = mips_rt(opcode);
    const uint32_t funct = mips_funct(opcode);
    const int16_t  offset = mips_offset(opcode);
    const uint32_t target_field = mips_target(opcode);

    // Helper lambda: compute branch target from PC-relative offset
    auto branch_target = [&]() -> uint32_t {
        return pc + (static_cast<int32_t>(offset) << 2) + 4;
    };

    // Helper lambda: check if branch target forms a permanent loop
    auto check_perm_loop = [&](uint32_t target_pc) -> bool {
        if (target_pc != pc) return false;
        // Read delay slot and check if it affects the branch comparison
        uint32_t delay_instr = 0;
        if (!memory_->MemoryValue32(pc + 4, delay_instr)) {
            return false;
        }
        R4300iInstruction instr(pc, opcode);
        return !instr.AffectedByDelaySlot(delay_instr);
    };

    switch (op) {
    // =================================================================
    // SPECIAL (opcode 0x00) - R-type instructions
    // =================================================================
    case R4300_SPECIAL:
        switch (funct) {
        // Arithmetic / Logic / Shift - no control flow effect
        case R4300_S_SLL:
        case R4300_S_SRL:
        case R4300_S_SRA:
        case R4300_S_SLLV:
        case R4300_S_SRLV:
        case R4300_S_SRAV:
        case R4300_S_MFHI:
        case R4300_S_MTHI:
        case R4300_S_MFLO:
        case R4300_S_MTLO:
        case R4300_S_DSLLV:
        case R4300_S_DSRLV:
        case R4300_S_DSRAV:
        case R4300_S_ADD:
        case R4300_S_ADDU:
        case R4300_S_SUB:
        case R4300_S_SUBU:
        case R4300_S_AND:
        case R4300_S_OR:
        case R4300_S_XOR:
        case R4300_S_NOR:
        case R4300_S_SLT:
        case R4300_S_SLTU:
        case R4300_S_DADD:
        case R4300_S_DADDU:
        case R4300_S_DSUB:
        case R4300_S_DSUBU:
        case R4300_S_DSLL:
        case R4300_S_DSRL:
        case R4300_S_DSRA:
        case R4300_S_DSLL32:
        case R4300_S_DSRL32:
        case R4300_S_DSRA32:
        case R4300_S_MULT:
        case R4300_S_MULTU:
        case R4300_S_DIV:
        case R4300_S_DIVU:
        case R4300_S_DMULT:
        case R4300_S_DMULTU:
        case R4300_S_DDIV:
        case R4300_S_DDIVU:
        case R4300_S_SYNC:
        // Trap R-type - may cause exception but no branch
        case R4300_S_TEQ:
        case R4300_S_TNE:
        case R4300_S_TGE:
        case R4300_S_TGEU:
        case R4300_S_TLT:
        case R4300_S_TLTU:
            break;

        // JR / JALR - indirect jump, ends block, includes delay slot
        case R4300_S_JR:
        case R4300_S_JALR:
            result.add_branch(pc, static_cast<uint32_t>(-1), false, true, true);
            break;

        // SYSCALL / BREAK - end block, no delay slot
        case R4300_S_SYSCALL:
            result.contains_syscall = true;
            break;
        case R4300_S_BREAK:
            result.contains_breakpoint = true;
            break;

        default:
            log_error("Unknown SPECIAL funct=0x" + std::to_string(funct) +
                      " at PC=0x" + std::to_string(pc));
            // Treat as non-control-flow; codegen handles via interpreter callback
            break;
        }
        break;

    // =================================================================
    // REGIMM (opcode 0x01) - Branch on register value
    // =================================================================
    case R4300_REGIMM:
        switch (rt) {
        case R4300_R_BLTZ:
        case R4300_R_BLTZAL: {
            uint32_t tgt = branch_target();
            if (tgt == pc + 8) {
                break; // NOP branch (target is next instruction after delay slot)
            }
            bool perm = check_perm_loop(tgt);
            result.add_branch(pc, tgt, false, true, false);
            if (perm) {
                result.contains_loop = true;
                result.flags = result.flags | BlockFlags::HasLoop;
            }
            break;
        }
        case R4300_R_BGEZ:
        case R4300_R_BGEZAL: {
            uint32_t tgt = branch_target();
            if (tgt == pc + 8) {
                break;
            }
            if (tgt == pc && rs == 0) {
                // BGEZ $zero is always taken => unconditional self-loop => end block
                result.add_branch(pc, static_cast<uint32_t>(-1), false, true, true);
                break;
            }
            bool perm = check_perm_loop(tgt);
            result.add_branch(pc, tgt, false, true, false);
            if (perm) {
                result.contains_loop = true;
                result.flags = result.flags | BlockFlags::HasLoop;
            }
            break;
        }
        // Likely branch variants
        case R4300_R_BLTZL:
        case R4300_R_BGEZL:
        case R4300_R_BGEZALL: {
            uint32_t tgt = branch_target();
            bool perm = check_perm_loop(tgt);
            result.add_branch(pc, tgt, true, true, false);
            if (perm) {
                result.contains_loop = true;
                result.flags = result.flags | BlockFlags::HasLoop;
            }
            break;
        }
        // Trap immediate instructions - no control flow change
        case R4300_R_TEQI:
        case R4300_R_TNEI:
        case R4300_R_TGEI:
        case R4300_R_TGEIU:
        case R4300_R_TLTI:
        case R4300_R_TLTIU:
            break;

        default:
            // Known invalid instruction pattern that ends block
            if (opcode == 0x0407000D) {
                result.add_branch(pc, static_cast<uint32_t>(-1), false, false, true);
                break;
            }
            log_error("Unknown REGIMM rt=0x" + std::to_string(rt) +
                      " at PC=0x" + std::to_string(pc));
            // Treat as non-control-flow; codegen handles via interpreter callback
            break;
        }
        break;

    // =================================================================
    // J (opcode 0x02) - Unconditional Jump
    // =================================================================
    case R4300_J: {
        uint32_t tgt = (pc & 0xF0000000) | (target_field << 2);
        bool perm = (tgt == pc);
        result.add_branch(pc, tgt, false, true, true);
        if (perm) {
            result.contains_loop = true;
            result.flags = result.flags | BlockFlags::HasLoop;
        }
        break;
    }

    // =================================================================
    // JAL (opcode 0x03) - Jump And Link (function call ends block)
    // =================================================================
    case R4300_JAL:
        result.add_branch(pc, static_cast<uint32_t>(-1), false, true, true);
        break;

    // =================================================================
    // BEQ (opcode 0x04)
    // =================================================================
    case R4300_BEQ: {
        uint32_t tgt = branch_target();
        if (tgt == pc + 8) {
            break; // NOP branch
        }
        bool perm = check_perm_loop(tgt);
        // BEQ $zero,$zero is unconditional
        result.add_branch(pc, tgt, false, true, (rs == 0 && rt == 0));
        if (perm) {
            result.contains_loop = true;
            result.flags = result.flags | BlockFlags::HasLoop;
        }
        break;
    }

    // =================================================================
    // BNE / BLEZ / BGTZ (opcodes 0x05-0x07) - conditional branches
    // =================================================================
    case R4300_BNE:
    case R4300_BLEZ:
    case R4300_BGTZ: {
        uint32_t tgt = branch_target();
        if (tgt == pc + 8) {
            break;
        }
        bool perm = check_perm_loop(tgt);
        result.add_branch(pc, tgt, false, true, false);
        if (perm) {
            result.contains_loop = true;
            result.flags = result.flags | BlockFlags::HasLoop;
        }
        break;
    }

    // =================================================================
    // Arithmetic Immediate - no control flow
    // =================================================================
    case R4300_ADDI:
    case R4300_ADDIU:
    case R4300_SLTI:
    case R4300_SLTIU:
    case R4300_ANDI:
    case R4300_ORI:
    case R4300_XORI:
    case R4300_LUI:
    case R4300_DADDI:
    case R4300_DADDIU:
    case R4300_LDL:
    case R4300_LDR:
        break;

    // =================================================================
    // Load / Store - no control flow
    // =================================================================
    case R4300_LB:
    case R4300_LH:
    case R4300_LWL:
    case R4300_LW:
    case R4300_LBU:
    case R4300_LHU:
    case R4300_LWR:
    case R4300_LWU:
    case R4300_SB:
    case R4300_SH:
    case R4300_SWL:
    case R4300_SW:
    case R4300_SDL:
    case R4300_SDR:
    case R4300_SWR:
    case R4300_CACHE:
    case R4300_LL:
    case R4300_LWC1:
    case R4300_LDC1:
    case R4300_LD:
    case R4300_SC:
    case R4300_SWC1:
    case R4300_SDC1:
    case R4300_SD:
        result.flags = result.flags | BlockFlags::HasMemoryOps;
        break;

    // =================================================================
    // COP0 (opcode 0x10)
    // =================================================================
    case R4300_COP0:
        switch (rs) {
        case R4300_COP0_MT:
        case R4300_COP0_DMT:
        case R4300_COP0_MF:
        case R4300_COP0_DMF:
            break;
        default:
            if ((rs & 0x10) != 0) {
                // CO (Coprocessor Operation) sub-instructions
                switch (funct) {
                case R4300_COP0_C0_TLBR:
                case R4300_COP0_C0_TLBWI:
                case R4300_COP0_C0_TLBWR:
                case R4300_COP0_C0_TLBP:
                    result.contains_tlb_op = true;
                    break;
                case R4300_COP0_C0_ERET:
                    // ERET - return from exception, ends block
                    result.add_branch(pc, static_cast<uint32_t>(-1), false, false, true);
                    break;
                default:
                    log_error("Unknown COP0 CO funct=0x" + std::to_string(funct) +
                              " at PC=0x" + std::to_string(pc));
                    break;
                }
            } else {
                log_error("Unknown COP0 rs=0x" + std::to_string(rs) +
                          " at PC=0x" + std::to_string(pc));
            }
            break;
        }
        break;

    // =================================================================
    // COP1 (opcode 0x11) - Floating Point
    // =================================================================
    case R4300_COP1: {
        uint32_t fmt_field = mips_fmt(opcode);
        switch (fmt_field) {
        case R4300_COP1_MF:
        case R4300_COP1_DMF:
        case R4300_COP1_CF:
        case R4300_COP1_MT:
        case R4300_COP1_DMT:
        case R4300_COP1_CT:
        case R4300_COP1_S:
        case R4300_COP1_D:
        case R4300_COP1_W:
        case R4300_COP1_L:
            result.contains_cop1_op = true;
            result.flags = result.flags | BlockFlags::HasFloatOps;
            break;
        case R4300_COP1_BC: {
            // COP1 branch instructions
            uint32_t ft_field = mips_ft(opcode);
            switch (ft_field) {
            case R4300_COP1_BC_BC:   // BC1F
            case R4300_COP1_BC_BCT:  // BC1T
            {
                uint32_t tgt = branch_target();
                if (tgt == pc + 8) {
                    break;
                }
                result.add_branch(pc, tgt, false, true, false);
                result.contains_cop1_op = true;
                result.flags = result.flags | BlockFlags::HasFloatOps;
                break;
            }
            case R4300_COP1_BC_BCFL:  // BC1FL
            case R4300_COP1_BC_BCFT:  // BC1TL
            {
                uint32_t tgt = branch_target();
                result.add_branch(pc, tgt, true, true, false);
                result.contains_cop1_op = true;
                result.flags = result.flags | BlockFlags::HasFloatOps;
                break;
            }
            default:
                log_error("Unknown COP1 BC ft=0x" + std::to_string(ft_field) +
                          " at PC=0x" + std::to_string(pc));
                break;
            }
            break;
        }
        default:
            log_error("Unknown COP1 fmt=0x" + std::to_string(fmt_field) +
                      " at PC=0x" + std::to_string(pc));
            break;
        }
        break;
    }

    // =================================================================
    // Likely Branch Variants
    // =================================================================
    case R4300_BEQL: {
        uint32_t tgt = branch_target();
        bool perm = check_perm_loop(tgt);
        result.add_branch(pc, tgt, true, true, (rs == 0 && rt == 0));
        if (perm) {
            result.contains_loop = true;
            result.flags = result.flags | BlockFlags::HasLoop;
        }
        break;
    }

    case R4300_BNEL:
    case R4300_BLEZL:
    case R4300_BGTZL: {
        uint32_t tgt = branch_target();
        bool perm = check_perm_loop(tgt);
        result.add_branch(pc, tgt, true, true, false);
        if (perm) {
            result.contains_loop = true;
            result.flags = result.flags | BlockFlags::HasLoop;
        }
        break;
    }

    // =================================================================
    // Default: Unknown Opcode
    // =================================================================
    default:
        // Known invalid/special instruction patterns that end block
        if (opcode == 0x7C1C97C0 || opcode == 0xF1F3F5F7) {
            result.add_branch(pc, static_cast<uint32_t>(-1), false, false, true);
            break;
        }
        log_error("Unknown opcode op=0x" + std::to_string(op) +
                  " (raw=0x" + std::to_string(opcode) +
                  ") at PC=0x" + std::to_string(pc));
        // Treat as non-control-flow; codegen handles via interpreter callback
        break;
    }

    return true;
}

// ============================================================================
// Code Generation
// ============================================================================

bool CodeBlock::generate_code(const AnalysisResult& analysis,
                              const CompileOptions& /*options*/)
{
    if (!code_holder_ || !system_ || !registers_) {
        log_error("generate_code: missing code_holder, system, or registers");
        return false;
    }

#if defined(__amd64__) || defined(_M_X64)
    // ----------------------------------------------------------------
    // x64 path – use CX64RecompilerOps callback-based compilation.
    // Each MIPS instruction is translated to a small piece of x64 code
    // that calls an interpreter callback (Interp_XXX) at runtime.
    // ----------------------------------------------------------------

    // Build a RecompilerContext from our system pointers
    auto* mem = system_->GetMemory();
    auto* tlb = system_->GetTLB();
    if (!mem || !tlb) {
        log_error("generate_code: system has no memory or TLB");
        return false;
    }

    // end_emulation flag – dummy static for compilation (the real flag
    // is checked in the Execute_* loop, not inside compiled blocks).
    static std::atomic<bool> s_dummyEnd{false};

    RecompilerContext ctx(*system_, *mem, *registers_, *tlb, s_dummyEnd);
    CX64RecompilerOps ops(ctx, *this);

    // Attach the assembler to our code_holder_ so emitted bytes land
    // in the right buffer (this is what CX64Ops::Initialize does).
    if (!ops.GetAssembler().Initialize(*code_holder_)) {
        log_error("generate_code: failed to attach assembler to code holder");
        return false;
    }

    // Emit x64 function prologue (push callee-saved regs, set up frame)
    ops.EnterCodeBlock();

    // Compile every instruction in the block
    // Determine which instruction is the branch (if any) so we can skip
    // PipelineStageCheck for it. On MIPS, the delay slot must execute
    // AFTER the branch sets JumpToLocation but BEFORE the block exits.
    // If we check PipelineStage after the branch, we'd exit before the
    // delay slot runs, causing it to be skipped entirely.
    size_t branchInstrIdx = static_cast<size_t>(-1);
    if (!analysis.branches.empty() && !analysis.delay_slots.empty() &&
        instructions_.size() >= 2) {
        // The branch is at second-to-last position, delay slot at last
        branchInstrIdx = instructions_.size() - 2;
    }

    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint32_t addr = start_address_ + static_cast<uint32_t>(i * 4);
        R4300iInstruction mipsInstr(addr, instructions_[i]);
        InstructionContext instrCtx(mipsInstr, addr, addr & 0x1FFFFFFF,
                                   PipelineStage::Execute, nullptr);

        // Before the delay slot instruction, set up correct CPU state so that
        // TriggerException computes the right BranchDelay bit and EPC if the
        // delay slot faults.  The branch callback already set m_PROGRAM_COUNTER
        // to the branch target; EmitDelaySlotSetup saves that target in
        // m_JumpToLocation, restores m_PROGRAM_COUNTER to the delay slot
        // address, and sets PipelineStage = Jump.
        if (branchInstrIdx != static_cast<size_t>(-1) &&
            i == branchInstrIdx + 1) {
            ops.EmitDelaySlotSetup(addr);
        }

        if (!ops.CompileInstruction(instrCtx)) {
            log_error("x64 CompileInstruction failed at 0x" + std::to_string(addr));
            // emit a NOP fallback so the block is still valid
        }

        // Decrement m_NextTimer by CountPerOp after each instruction.
        // This matches the interpreter's per-instruction decrement so that
        // COUNT reads within blocks are accurate, and early exits (exceptions,
        // syscalls) don't over-count cycles for unexecuted instructions.
        // NOTE: For branch-likely instructions, EmitBranchLikelyCallback
        // already emitted the timer decrement BEFORE the conditional skip,
        // so we must not emit a second one here.
        if (!ops.HasPendingBranchLikelySkip() || i != branchInstrIdx) {
            ops.EmitTimerDecrement();
        }

        // After each instruction, check if an exception (TLB miss, syscall,
        // etc.) set PipelineStage to Jump.  If so, exit the block immediately
        // so the dispatcher can redirect to the exception vector.
        // EXCEPT: skip for the branch instruction itself — the branch sets
        // PipelineStage=Jump for normal control flow, and we must let the
        // delay slot execute before exiting.
        if (i != branchInstrIdx) {
            ops.EmitPipelineStageCheck();
        }
    }

    // If the last instruction pair was a branch-likely + delay slot,
    // bind the skip label HERE, AFTER the delay slot code, so that
    // when the branch is NOT taken the delay slot is jumped over.
    ops.BindPendingBranchLikelyLabel();

    // After running all instructions, update the program counter to
    // point past the last instruction in the block so the dispatcher
    // knows where to continue.
    {
        // Emit: m_PROGRAM_COUNTER = end_address_ + 4
        auto& a = ops.GetAssembler().GetAssembler();
        uint32_t nextPC = end_address_ + 4;

        // Handle branches: if the analysis found a branch at the end,
        // the branch callback already updated PC, so we only patch PC
        // for non-branch blocks.
        if (analysis.branches.empty()) {
            // mov qword ptr [&PROGRAM_COUNTER], nextPC
            using namespace asmjit::x86;
            uint64_t pcAddr = reinterpret_cast<uint64_t>(&registers_->m_PROGRAM_COUNTER);
            a.mov(rax, pcAddr);
            a.mov(asmjit::x86::qword_ptr(rax), static_cast<int32_t>(nextPC));
        }
    }

    // Emit x64 function epilogue (restore callee-saved regs, ret)
    ops.ExitCodeBlock(ExitReason::NormalBlockEnd);

    statistics_.generated_bytes = 0; // will be updated by finalize()
    return true;

#elif defined(__i386__) || defined(_M_IX86)
    // ----------------------------------------------------------------
    // x86 32-bit path – use X86RecompilerOps (PJ64-style native codegen).
    // ----------------------------------------------------------------
    using namespace Nova64::Recompiler::x86;

    auto* mem = system_->GetMemory();
    auto* tlb = system_->GetTLB();
    if (!mem || !tlb) {
        log_error("generate_code: system has no memory or TLB");
        return false;
    }

    static std::atomic<bool> s_dummyEnd{false};
    RecompilerContext ctx(*system_, *mem, *registers_, *tlb, s_dummyEnd);
    X86RecompilerOps ops(ctx, *this);

    // X86Ops constructor does not attach to CodeHolder; do it explicitly.
    ops.InitializeAssembler(*code_holder_);

    ops.EnterCodeBlock();

    // Determine branch instruction index (same logic as x64 path)
    size_t branchInstrIdx = static_cast<size_t>(-1);
    if (!analysis.branches.empty() && !analysis.delay_slots.empty() &&
        instructions_.size() >= 2) {
        branchInstrIdx = instructions_.size() - 2;
    }

    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint32_t addr = start_address_ + static_cast<uint32_t>(i * 4);
        R4300iInstruction mipsInstr(addr, instructions_[i]);
        InstructionContext instrCtx(mipsInstr, addr, addr & 0x1FFFFFFF,
                                   PipelineStage::Execute, nullptr);

        if (!ops.CompileInstruction(instrCtx)) {
            log_error("x86 CompileInstruction failed at 0x" + std::to_string(addr));
        }

        if (!ops.HasPendingBranchLikelySkip() || i != branchInstrIdx) {
            ops.EmitTimerDecrement();
        }

        if (i != branchInstrIdx) {
            ops.EmitPipelineStageCheck();
        }
    }

    ops.BindPendingBranchLikelyLabel();

    // For non-branch blocks, update PC past the block
    if (analysis.branches.empty()) {
        ops.EmitPCUpdate();
    }

    ops.ExitCodeBlock();

    statistics_.generated_bytes = 0;
    return true;

#elif defined(__aarch64__) || defined(_M_ARM64)
    // ----------------------------------------------------------------
    // ARM64 path – use ArmRecompilerOps (partial native codegen).
    // Integer ALU / shifts / mul are native; loads/stores/FPU use fallback.
    // ----------------------------------------------------------------
    using namespace Nova64::Recompiler::Arm;

    auto* mem = system_->GetMemory();
    auto* tlb = system_->GetTLB();
    if (!mem || !tlb) {
        log_error("generate_code: system has no memory or TLB");
        return false;
    }

    static std::atomic<bool> s_dummyEnd{false};
    RecompilerContext ctx(*system_, *mem, *registers_, *tlb, s_dummyEnd);
    ArmRecompilerOps ops(ctx, *this);

    // ARM assembler auto-attaches to CodeHolder in its constructor.

    ops.EnterCodeBlock();

    size_t branchInstrIdx = static_cast<size_t>(-1);
    if (!analysis.branches.empty() && !analysis.delay_slots.empty() &&
        instructions_.size() >= 2) {
        branchInstrIdx = instructions_.size() - 2;
    }

    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint32_t addr = start_address_ + static_cast<uint32_t>(i * 4);
        R4300iInstruction mipsInstr(addr, instructions_[i]);
        InstructionContext instrCtx(mipsInstr, addr, addr & 0x1FFFFFFF,
                                   PipelineStage::Execute, nullptr);

        if (!ops.CompileInstruction(instrCtx)) {
            log_error("ARM64 CompileInstruction failed at 0x" + std::to_string(addr));
        }

        if (!ops.HasPendingBranchLikelySkip() || i != branchInstrIdx) {
            ops.EmitTimerDecrement();
        }

        if (i != branchInstrIdx) {
            ops.EmitPipelineStageCheck();
        }
    }

    ops.BindPendingBranchLikelyLabel();

    if (analysis.branches.empty()) {
        ops.EmitPCUpdate();
    }

    ops.ExitCodeBlock();

    statistics_.generated_bytes = 0;
    return true;

#else
    log_error("generate_code: unsupported architecture");
    return false;
#endif
}

bool CodeBlock::optimize_code(const AnalysisResult& /*analysis*/)
{
    // Optimization passes - stub for now
    // Future: dead code elimination, constant folding, etc.
    return true;
}

// ============================================================================
// CodeGenerator Implementation
// ============================================================================

CodeBlock::CodeGenerator::CodeGenerator(asmjit::CodeHolder& code,
                                        const AnalysisResult& analysis,
                                        const Config& config)
    : assembler_(&code)
    , analysis_(analysis)
    , config_(config)
{
    register_states_.fill({});
}

bool CodeBlock::CodeGenerator::generate_prologue()
{
    // Standard x64 function prologue
    assembler_.push(asmjit::x86::rbp);
    assembler_.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    return true;
}

bool CodeBlock::CodeGenerator::generate_epilogue()
{
    // Standard x64 function epilogue
    assembler_.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    assembler_.pop(asmjit::x86::rbp);
    assembler_.ret();
    return true;
}

bool CodeBlock::CodeGenerator::generate_instruction(uint32_t /*address*/, uint32_t /*opcode*/)
{
    // Individual instruction code generation is delegated to
    // CX64RecompilerOps via the recompiler pipeline.
    // Stub until the full recompiler ops are connected.
    generated_bytes_ += 4;
    return true;
}

bool CodeBlock::CodeGenerator::generate_branch(const AnalysisResult::BranchInfo& /*branch*/)
{
    // Branch linkage code generation - stub
    return true;
}

bool CodeBlock::CodeGenerator::generate_delay_slot(uint32_t /*address*/, uint32_t /*opcode*/)
{
    return true;
}

bool CodeBlock::CodeGenerator::generate_linkage()
{
    return true;
}

bool CodeBlock::CodeGenerator::generate_exception_handler()
{
    return true;
}

asmjit::x86::Gp CodeBlock::CodeGenerator::allocate_register(uint8_t /*mips_reg*/)
{
    return asmjit::x86::rax; // Placeholder
}

void CodeBlock::CodeGenerator::free_register(asmjit::x86::Gp /*reg*/)
{
}

void CodeBlock::CodeGenerator::spill_dirty_registers()
{
}

// ============================================================================
// Instruction Access
// ============================================================================

std::optional<uint32_t> CodeBlock::get_instruction(uint32_t address) const
{
    if (address < start_address_ || address > end_address_) {
        return std::nullopt;
    }
    if ((address - start_address_) % 4 != 0) {
        return std::nullopt;
    }
    size_t index = (address - start_address_) / 4;
    if (index >= instructions_.size()) {
        return std::nullopt;
    }
    return instructions_[index];
}

// ============================================================================
// Memory Snapshot (Self-Modifying Code Detection)
// ============================================================================

bool CodeBlock::verify_memory_snapshot() const
{
    if (!memory_ || instructions_.empty()) {
        return true; // Nothing to verify
    }

    // Full-block verification: compare ALL stored instructions against
    // current RDRAM content. The old approach only checked the first 16
    // bytes, missing modifications beyond the 4th instruction.
    uint32_t addr = entry_address_;
    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint8_t* ptr = memory_->MemoryPtr(addr, 4, true);
        if (!ptr) return true; // Can't verify unmapped memory — assume valid
        uint32_t current = *reinterpret_cast<uint32_t*>(ptr);
        if (current != instructions_[i]) {
            return false; // Code has been modified
        }
        addr += 4;
    }
    return true;
}

void CodeBlock::update_memory_snapshot()
{
    memory_snapshot_ = MemorySnapshot{};
    if (!memory_) return;

    uint8_t* ptr = memory_->MemoryPtr(entry_address_, 16, true);
    if (ptr) {
        auto* loc = reinterpret_cast<uint64_t*>(ptr);
        memory_snapshot_.add(entry_address_, *loc, reinterpret_cast<uintptr_t>(loc));
        memory_snapshot_.add(entry_address_ + 8, *(loc + 1),
                           reinterpret_cast<uintptr_t>(loc + 1));
    }
}

// ============================================================================
// Debug Information
// ============================================================================

std::string CodeBlock::disassembly() const
{
    std::string result;
    result.reserve(instructions_.size() * 40);

    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint32_t addr = start_address_ + static_cast<uint32_t>(i * 4);
        R4300iInstruction instr(addr, instructions_[i]);
        result += instr.GetFullString();
        result += "\n";
    }

    return result;
}

std::string CodeBlock::control_flow_graph() const
{
    std::string result;
    result += "CodeBlock CFG: entry=0x" + std::to_string(entry_address_) + "\n";
    result += "  Range: 0x" + std::to_string(start_address_) +
              " - 0x" + std::to_string(end_address_) + "\n";
    result += "  Instructions: " + std::to_string(instructions_.size()) + "\n";
    return result;
}

// ============================================================================
// Linking and Patching
// ============================================================================

bool CodeBlock::link_to(CodeBlock& /*target*/, uint32_t /*branch_address*/)
{
    // Block linking - stub until section linking is operational
    return true;
}

void CodeBlock::patch_branch(uint32_t /*from_address*/, uint32_t /*to_address*/)
{
    // Patch a previously compiled branch to point to a new address
}

// ============================================================================
// Serialization
// ============================================================================

CodeBlock::SerializedBlock CodeBlock::serialize() const
{
    SerializedBlock data;
    data.hash = hash_;
    data.entry_address = entry_address_;
    data.start_address = start_address_;
    data.end_address = end_address_;
    data.instructions = instructions_;
    data.snapshot = memory_snapshot_;
    data.stats = statistics_;
    data.flags = flags_;
    return data;
}

bool CodeBlock::deserialize(const SerializedBlock& data)
{
    hash_ = data.hash;
    entry_address_ = data.entry_address;
    start_address_ = data.start_address;
    end_address_ = data.end_address;
    instructions_ = data.instructions;
    memory_snapshot_ = data.snapshot;
    statistics_ = data.stats;
    flags_ = data.flags;
    is_valid_ = true;
    return true;
}

// ============================================================================
// Hashing
// ============================================================================

BlockHash CodeBlock::calculate_hash() const noexcept
{
    if (instructions_.empty()) {
        return {0, 0};
    }

    const void* data = instructions_.data();
    size_t size = instructions_.size() * sizeof(uint32_t);

    uint64_t low = fnv1a_hash(data, size);
    uint64_t high = fnv1a_hash(data, size, 0x6c62272e07bb0142ULL);

    return {low, high};
}

uint64_t CodeBlock::calculate_fast_hash() const noexcept
{
    return calculate_hash().low;
}

void CodeBlock::update_hash()
{
    hash_ = calculate_hash();
}

// ============================================================================
// Error Handling (asmjit::ErrorHandler override)
// ============================================================================

void CodeBlock::handleError(asmjit::Error err, const char* message,
                           asmjit::BaseEmitter* /*origin*/)
{
    log_error(std::string("AsmJit error: ") +
              (message ? message : "unknown") +
              " (" + std::to_string(static_cast<uint32_t>(err)) + ")");
}

// ============================================================================
// Logging Helpers
// ============================================================================

void CodeBlock::log_compilation(const std::string& message)
{
    if (config_.enable_logging) {
        code_log_ += "[COMPILE] " + message + "\n";
    }
}

void CodeBlock::log_error(const std::string& message)
{
    code_log_ += "[ERROR] " + message + "\n";
}

// ============================================================================
// CodeBlockCache Implementation
// ============================================================================

CodeBlockCache::CodeBlockCache(const Config& config)
    : config_(config)
{
}

CodeBlockCache::~CodeBlockCache() = default;

std::shared_ptr<CodeBlock> CodeBlockCache::get_or_compile(uint32_t address)
{
    auto existing = get(address);
    if (existing) {
        return existing;
    }
    return nullptr;
}

std::shared_ptr<CodeBlock> CodeBlockCache::get(uint32_t address) const
{
    std::shared_lock lock(mutex_);
    auto it = blocks_.find(address);
    if (it != blocks_.end()) {
        const_cast<CodeBlockCache*>(this)->touch(address, true);
        return it->second.block;
    }
    return nullptr;
}

void CodeBlockCache::insert(std::shared_ptr<CodeBlock> block)
{
    if (!block) return;

    std::unique_lock lock(mutex_);

    uint32_t address = block->entry_address();

    // Evict if at capacity
    while (blocks_.size() >= config_.max_blocks) {
        evict_oldest();
    }

    auto timestamp = get_timestamp();
    blocks_.emplace(address, CacheEntry(std::move(block), timestamp));
    lru_list_.push_front(address);
    lru_map_[address] = lru_list_.begin();

    std::lock_guard stats_lock(stats_mutex_);
    stats_.current_blocks = blocks_.size();
}

void CodeBlockCache::remove(uint32_t address)
{
    std::unique_lock lock(mutex_);
    auto it = blocks_.find(address);
    if (it != blocks_.end()) {
        auto lru_it = lru_map_.find(address);
        if (lru_it != lru_map_.end()) {
            lru_list_.erase(lru_it->second);
            lru_map_.erase(lru_it);
        }
        blocks_.erase(it);
    }
}

void CodeBlockCache::invalidate(uint32_t address)
{
    remove(address);
}

void CodeBlockCache::invalidate_range(uint32_t start, uint32_t end)
{
    std::unique_lock lock(mutex_);
    for (auto it = blocks_.begin(); it != blocks_.end(); ) {
        if (it->first >= start && it->first <= end) {
            auto lru_it = lru_map_.find(it->first);
            if (lru_it != lru_map_.end()) {
                lru_list_.erase(lru_it->second);
                lru_map_.erase(lru_it);
            }
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void CodeBlockCache::invalidate_all()
{
    clear();
}

void CodeBlockCache::clear()
{
    std::unique_lock lock(mutex_);
    blocks_.clear();
    lru_list_.clear();
    lru_map_.clear();
    hot_lru_list_.clear();
    hot_lru_map_.clear();
    hot_block_count_ = 0;
}

void CodeBlockCache::prefetch(const std::vector<uint32_t>& /*addresses*/)
{
    // Prefetch hint - could pre-compile blocks we expect to need
}

void CodeBlockCache::compress_cold_blocks()
{
    // Could release generated code cache from cold blocks
}

CodeBlockCache::CacheStats CodeBlockCache::statistics() const
{
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void CodeBlockCache::reset_statistics()
{
    std::lock_guard lock(stats_mutex_);
    stats_.reset();
}

size_t CodeBlockCache::memory_usage() const
{
    std::shared_lock lock(mutex_);
    size_t total = 0;
    for (const auto& [addr, entry] : blocks_) {
        total += entry.block->statistics().generated_bytes;
        total += entry.block->instruction_count() * sizeof(uint32_t);
    }
    return total;
}

void CodeBlockCache::set_memory_limit(size_t bytes)
{
    config_.max_total_size = bytes;
}

void CodeBlockCache::touch(uint32_t address, bool is_hit)
{
    auto lru_it = lru_map_.find(address);
    if (lru_it != lru_map_.end()) {
        lru_list_.erase(lru_it->second);
        lru_list_.push_front(address);
        lru_map_[address] = lru_list_.begin();
    }

    auto it = blocks_.find(address);
    if (it != blocks_.end()) {
        it->second.last_access = get_timestamp();
        it->second.access_count++;
    }

    std::lock_guard lock(stats_mutex_);
    if (is_hit) {
        stats_.hits++;
    } else {
        stats_.misses++;
    }
}

void CodeBlockCache::evict_oldest()
{
    if (lru_list_.empty()) return;

    uint32_t oldest = lru_list_.back();
    lru_list_.pop_back();
    lru_map_.erase(oldest);
    blocks_.erase(oldest);

    std::lock_guard lock(stats_mutex_);
    stats_.evictions++;
    stats_.current_blocks = blocks_.size();
}

void CodeBlockCache::promote_to_hot(uint32_t address)
{
    auto it = blocks_.find(address);
    if (it != blocks_.end() && !it->second.is_hot) {
        it->second.is_hot = true;
        hot_block_count_++;
    }
}

void CodeBlockCache::demote_from_hot(uint32_t address)
{
    auto it = blocks_.find(address);
    if (it != blocks_.end() && it->second.is_hot) {
        it->second.is_hot = false;
        if (hot_block_count_ > 0) hot_block_count_--;
    }
}

bool CodeBlockCache::should_promote(uint32_t address) const
{
    auto it = blocks_.find(address);
    if (it == blocks_.end()) return false;
    return it->second.access_count > 100 && !it->second.is_hot;
}

// ============================================================================
// CodeBlockFactory Implementation
// ============================================================================

std::unique_ptr<CodeBlock> CodeBlockFactory::create_block(
    N64System& system,
    uint32_t address,
    OptimizationLevel level,
    const ProfileData* /*profile*/)
{
    CodeBlock::Config config;
    switch (level) {
    case OptimizationLevel::None:
        config.enable_optimizations = false;
        break;
    case OptimizationLevel::Basic:
    case OptimizationLevel::Aggressive:
        config.enable_optimizations = true;
        break;
    case OptimizationLevel::ProfileGuided:
        config.enable_optimizations = true;
        config.enable_profiling = true;
        break;
    }

    return std::make_unique<CodeBlock>(system, address, config);
}

std::shared_ptr<CodeBlock> CodeBlockFactory::create_nop_block(N64System& system)
{
    return std::make_shared<CodeBlock>(system, 0);
}

std::shared_ptr<CodeBlock> CodeBlockFactory::create_delay_slot_block(N64System& system)
{
    return std::make_shared<CodeBlock>(system, 0);
}

std::shared_ptr<CodeBlock> CodeBlockFactory::create_branch_likely_block(N64System& system)
{
    return std::make_shared<CodeBlock>(system, 0);
}

std::shared_ptr<CodeBlock> CodeBlockFactory::create_exception_handler(N64System& system)
{
    return std::make_shared<CodeBlock>(system, 0);
}

std::shared_ptr<CodeBlock> CodeBlockFactory::create_tlb_miss_handler(N64System& system)
{
    return std::make_shared<CodeBlock>(system, 0);
}

bool CodeBlockFactory::apply_dead_code_elimination(CodeBlock& /*block*/)     { return true; }
bool CodeBlockFactory::apply_constant_folding(CodeBlock& /*block*/)          { return true; }
bool CodeBlockFactory::apply_common_subexpression_elimination(CodeBlock& /*block*/) { return true; }
bool CodeBlockFactory::apply_loop_invariant_code_motion(CodeBlock& /*block*/) { return true; }
bool CodeBlockFactory::apply_strength_reduction(CodeBlock& /*block*/)        { return true; }

bool CodeBlockFactory::is_nop_sequence(const std::vector<uint32_t>& instructions)
{
    return std::all_of(instructions.begin(), instructions.end(),
                       [](uint32_t i) { return i == 0; });
}

bool CodeBlockFactory::is_branch_likely_pattern(const std::vector<uint32_t>& instructions)
{
    if (instructions.empty()) return false;
    uint32_t op = mips_op(instructions[0]);
    return op == R4300_BEQL || op == R4300_BNEL ||
           op == R4300_BLEZL || op == R4300_BGTZL;
}

bool CodeBlockFactory::is_exception_handler_pattern(const std::vector<uint32_t>& /*instructions*/)
{
    return false;
}

// Functions moved to CodeSection.cpp
} // namespace Recompiler
} // namespace Nova64
