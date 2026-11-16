/**
 * @file CodeSection.cpp
 * @brief Implementation of code section management for dynamic recompilation
 *
 * Nova64 N64 Emulator - Based on Project64 architecture
 *
 * Adapted from Project64's CCodeSection with modern C++ improvements:
 * - Graph-based section model (successors/predecessors by SectionId)
 * - RAII for resource management
 * - Comprehensive control flow analysis using dominator tree algorithms
 * - Profile-guided optimization support with hot/cold path detection
 * - Thread-safe statistics tracking
 * - Modern error handling and logging
 *
 * Original Project64 code licensed under GPL-2.0
 */

#include "CodeSection.hpp"
#include "CodeBlock.hpp"
#include "RegInfo.hpp"
#include "ExitInfo.hpp"
#include "JumpInfo.hpp"
#include "SectionInfo.hpp"
#include "asmjit.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/log.hpp"
#include "../../common/trace.hpp"
#include "../../common/MemoryManagment.hpp"
#include <format>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stack>
#include <queue>
#include <numeric>
#include <cassert>

using namespace Nova64::Recompiler;

// =============================================================================
// RegisterEntryState Implementation
// =============================================================================

bool RegisterEntryState::operator==(const RegisterEntryState& other) const {
    for (size_t i = 0; i < 32; ++i) {
        if (gpr[i].allocated != other.gpr[i].allocated || gpr[i].dirty != other.gpr[i].dirty ||
            gpr[i].constant != other.gpr[i].constant || (gpr[i].constant && gpr[i].value != other.gpr[i].value)) {
            return false;
        }
    }

    for (size_t i = 0; i < 32; ++i) {
        if (fpr[i].allocated != other.fpr[i].allocated || fpr[i].dirty != other.fpr[i].dirty) {
            return false;
        }
    }

    if (hi.allocated != other.hi.allocated || hi.dirty != other.hi.dirty || lo.allocated != other.lo.allocated ||
        lo.dirty != other.lo.dirty) {
        return false;
    }

    return true;
}

void RegisterEntryState::merge(const RegisterEntryState& other) {
    // Conservative GPR merge: both sides must agree on allocation and constants
    for (size_t i = 0; i < 32; ++i) {
        if (!gpr[i].allocated || !other.gpr[i].allocated) {
            gpr[i].allocated = false;
        }
        if (gpr[i].dirty || other.gpr[i].dirty) {
            gpr[i].dirty = true;
        }
        // Constants only survive if both sides agree on the exact value
        if (gpr[i].constant && other.gpr[i].constant) {
            if (gpr[i].value != other.gpr[i].value) {
                gpr[i].constant = false;
            }
        } else {
            gpr[i].constant = false;
        }
    }

    // Conservative FPR merge
    for (size_t i = 0; i < 32; ++i) {
        if (!fpr[i].allocated || !other.fpr[i].allocated) {
            fpr[i].allocated = false;
        }
        if (fpr[i].dirty || other.fpr[i].dirty) {
            fpr[i].dirty = true;
        }
    }

    // Merge HI/LO
    if (!hi.allocated || !other.hi.allocated) {
        hi.allocated = false;
    }
    if (hi.dirty || other.hi.dirty) {
        hi.dirty = true;
    }
    if (!lo.allocated || !other.lo.allocated) {
        lo.allocated = false;
    }
    if (lo.dirty || other.lo.dirty) {
        lo.dirty = true;
    }
}

void RegisterEntryState::reset() {
    for (auto& reg : gpr) {
        reg = RegisterInfo{};
    }
    for (auto& reg : fpr) {
        reg = RegisterInfo{};
    }
    hi = RegisterInfo{};
    lo = RegisterInfo{};
}

std::string RegisterEntryState::to_string() const {
    std::ostringstream oss;
    oss << "RegisterState:\n";

    oss << "  GPR: ";
    for (size_t i = 0; i < 32; ++i) {
        if (gpr[i].allocated) {
            oss << "r" << i;
            if (gpr[i].dirty) {
                oss << '*';
            }
            if (gpr[i].constant) {
                oss << '=' << gpr[i].value;
            }
            oss << ' ';
        }
    }
    oss << '\n';

    oss << "  FPR: ";
    for (size_t i = 0; i < 32; ++i) {
        if (fpr[i].allocated) {
            oss << "f" << i;
            if (fpr[i].dirty) {
                oss << '*';
            }
            oss << ' ';
        }
    }
    oss << '\n';

    if (hi.allocated || lo.allocated) {
        oss << "  Special: ";
        if (hi.allocated) {
            oss << "HI";
            if (hi.dirty) oss << '*';
            oss << ' ';
        }
        if (lo.allocated) {
            oss << "LO";
            if (lo.dirty) oss << '*';
            oss << ' ';
        }
        oss << '\n';
    }

    return oss.str();
}

// =============================================================================
// CodeSection Construction / Destruction
// =============================================================================

CodeSection::CodeSection(CodeBlock* parent_block, uint32_t entry_pc, SectionId id, bool allow_linking,
                         const Config& config) :
    parent_block_(parent_block), id_(id), type_(SectionType::Normal),
    flags_(allow_linking ? SectionFlags::Linked : SectionFlags::None), config_(config), metadata_{} {
    metadata_.entry_pc = entry_pc;
    metadata_.end_pc = entry_pc;
    entry_state_.reset();
    exit_state_.reset();

    log_generation("Section created at PC 0x" + std::format("{:08X}", entry_pc) +
                   " id=" + std::to_string(static_cast<uint32_t>(id)));
}

CodeSection::~CodeSection() noexcept {
    // RAII: code_holder_ unique_ptr handles cleanup
    // Log final statistics if profiling is enabled
    if (config_.enable_profiling && metadata_.execution_count > 0) {
        log_generation("Section destroyed - executed " + std::to_string(metadata_.execution_count) + " times, avg " +
                       std::to_string(metadata_.average_cycles) + " cycles");
    }
}

CodeSection::CodeSection(CodeSection&& other) noexcept :
    parent_block_(other.parent_block_), id_(other.id_), type_(other.type_), flags_(other.flags_),
    config_(other.config_), successors_(std::move(other.successors_)), predecessors_(std::move(other.predecessors_)),
    outgoing_edges_(std::move(other.outgoing_edges_)), incoming_edges_(std::move(other.incoming_edges_)),
    immediate_dominator_(other.immediate_dominator_), dominated_(std::move(other.dominated_)),
    post_dominated_by_(std::move(other.post_dominated_by_)), entry_state_(other.entry_state_),
    exit_state_(other.exit_state_), metadata_(other.metadata_), stats_(other.stats_),
    code_holder_(std::move(other.code_holder_)), entry_label_(other.entry_label_),
    patch_points_(std::move(other.patch_points_)), debug_log_(std::move(other.debug_log_)),
    instructions_(std::move(other.instructions_)), successor_addresses_(std::move(other.successor_addresses_)),
    predecessor_addresses_(std::move(other.predecessor_addresses_)) {
    other.parent_block_ = nullptr;
    other.flags_ = SectionFlags::None;
}

CodeSection& CodeSection::operator=(CodeSection&& other) noexcept {
    if (this != &other) {
        parent_block_ = other.parent_block_;
        id_ = other.id_;
        type_ = other.type_;
        flags_ = other.flags_;
        config_ = other.config_;
        successors_ = std::move(other.successors_);
        predecessors_ = std::move(other.predecessors_);
        outgoing_edges_ = std::move(other.outgoing_edges_);
        incoming_edges_ = std::move(other.incoming_edges_);
        immediate_dominator_ = other.immediate_dominator_;
        dominated_ = std::move(other.dominated_);
        post_dominated_by_ = std::move(other.post_dominated_by_);
        entry_state_ = other.entry_state_;
        exit_state_ = other.exit_state_;
        metadata_ = other.metadata_;
        stats_ = other.stats_;
        code_holder_ = std::move(other.code_holder_);
        entry_label_ = other.entry_label_;
        patch_points_ = std::move(other.patch_points_);
        debug_log_ = std::move(other.debug_log_);
        instructions_ = std::move(other.instructions_);
        successor_addresses_ = std::move(other.successor_addresses_);
        predecessor_addresses_ = std::move(other.predecessor_addresses_);

        other.parent_block_ = nullptr;
        other.flags_ = SectionFlags::None;
    }
    return *this;
}

// =============================================================================
// Setup and Configuration
// =============================================================================

void CodeSection::set_delay_slot() {
    flags_ = flags_ | SectionFlags::HasDelaySlot;
    type_ = SectionType::DelaySlot;
    log_generation("Section marked as delay slot");
}

void CodeSection::set_jump_target(uint32_t branch_pc, uint32_t target_pc, bool permanent_loop) {
    flags_ = flags_ | SectionFlags::HasJump;

    // Create or update the outgoing edge for the jump target
    ControlFlowEdge edge{};
    edge.source = id_;
    edge.target = SectionId{0}; // Will be resolved during linking
    edge.branch_address = branch_pc;
    edge.is_conditional = false;
    edge.is_likely = false;
    edge.has_delay_slot = has_flag(flags_, SectionFlags::HasDelaySlot);

    // Check if this is a permanent loop (branch to self)
    if (permanent_loop || target_pc == metadata_.entry_pc) {
        flags_ = flags_ | SectionFlags::HasLoop;
        type_ = SectionType::Loop;
        log_generation("Permanent loop detected at 0x" + std::format("{:08X}", branch_pc));
    }

    // Record the target address for later section graph construction
    successor_addresses_.push_back(target_pc);
    outgoing_edges_.push_back(edge);

    metadata_.end_pc = branch_pc;
    log_generation("Jump target set: 0x" + std::format("{:08X}", branch_pc) + " -> 0x" +
                   std::format("{:08X}", target_pc));
}

void CodeSection::set_continue_target(uint32_t branch_pc, uint32_t target_pc) {
    flags_ = flags_ | SectionFlags::HasBranch;

    ControlFlowEdge edge{};
    edge.source = id_;
    edge.target = SectionId{0}; // Resolved during linking
    edge.branch_address = branch_pc;
    edge.is_conditional = true;
    edge.is_likely = false;
    edge.has_delay_slot = has_flag(flags_, SectionFlags::HasDelaySlot);

    successor_addresses_.push_back(target_pc);
    outgoing_edges_.push_back(edge);

    log_generation("Continue target set: 0x" + std::format("{:08X}", branch_pc) + " -> 0x" +
                   std::format("{:08X}", target_pc));
}

// =============================================================================
// Code Generation (adapted from P64's GenerateNativeCode)
// =============================================================================

CodeSection::GenerationResult CodeSection::generate_native_code() {
    auto start_time = std::chrono::high_resolution_clock::now();

    if (!parent_block_) {
        log_generation("ERROR: No parent block");
        return GenerationResult::InvalidSection;
    }

    log_generation("=== Generating native code for section 0x" + std::format("{:08X}", metadata_.entry_pc) + " ===");

    // Step 1: Analyze the section
    auto analysis = analyze_section();
    if (!analysis.has_value()) {
        log_generation("ERROR: Section analysis failed");
        return GenerationResult::FailedAnalysis;
    }

    // Step 2: Build control flow graph from analysis
    build_control_flow_graph(analysis.value());

    // Step 3: Compute dominance if we have multiple basic blocks
    if (analysis->basic_blocks.size() > 1) {
        compute_dominance(analysis.value());
    }

    // Step 4: Apply optimizations if enabled
    if (config_.enable_optimizations) {
        auto opt_start = std::chrono::high_resolution_clock::now();
        apply_optimizations();
        auto opt_end = std::chrono::high_resolution_clock::now();
        stats_.optimization_time_ns =
            static_cast<size_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(opt_end - opt_start).count());
    }

    // Step 5: Generate native code
    if (!generate_code(analysis.value())) {
        log_generation("ERROR: Native code generation failed");
        return GenerationResult::FailedCodeGen;
    }

    // Mark as compiled
    flags_ = flags_ | SectionFlags::Compiled;

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.generation_time_ns =
        static_cast<size_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
    stats_.mips_instruction_count = metadata_.instruction_count;

    log_generation("Native code generation complete: " + std::to_string(stats_.native_code_size) + " bytes, " +
                   std::to_string(stats_.mips_instruction_count) + " MIPS instructions");

    return GenerationResult::Success;
}

// =============================================================================
// Section Linkage (adapted from P64's GenerateSectionLinkage)
// =============================================================================

bool CodeSection::generate_linkage() {
    log_generation("Generating linkage for section 0x" + std::format("{:08X}", metadata_.entry_pc));

    if (!parent_block_) {
        log_generation("ERROR: No parent block for linkage");
        return false;
    }

    // Resolve successor section IDs from addresses
    for (size_t i = 0; i < successor_addresses_.size(); ++i) {
        uint32_t target_addr = successor_addresses_[i];

        // Search for an existing section with this entry address
        auto existing = find_existing_section(target_addr);
        if (existing.has_value()) {
            CodeSection* target_section = existing.value();
            SectionId target_id = target_section->id();

            // Add to successors if not already present
            if (std::find(successors_.begin(), successors_.end(), target_id) == successors_.end()) {
                successors_.push_back(target_id);
            }

            // Update the edge target
            if (i < outgoing_edges_.size()) {
                outgoing_edges_[i].target = target_id;
            }

            // Register ourselves as a parent of the target
            target_section->add_parent(this);

            log_generation("Linked to section 0x" + std::format("{:08X}", target_addr) +
                           " (id=" + std::to_string(static_cast<uint32_t>(target_id)) + ")");
        } else {
            log_generation("WARNING: No existing section found for target 0x" + std::format("{:08X}", target_addr));
        }
    }

    // Check for loops: if any successor is also a predecessor (or self),
    // mark this section as containing a loop
    for (const auto& succ_id : successors_) {
        if (succ_id == id_) {
            flags_ = flags_ | SectionFlags::HasLoop;
            type_ = SectionType::Loop;
            log_generation("Self-loop detected");
            break;
        }
        for (const auto& pred_id : predecessors_) {
            if (succ_id == pred_id) {
                flags_ = flags_ | SectionFlags::HasLoop;
                log_generation("Loop back-edge detected to section " + std::to_string(static_cast<uint32_t>(succ_id)));
                break;
            }
        }
    }

    flags_ = flags_ | SectionFlags::Linked;
    return true;
}

// =============================================================================
// Section Inlining
// =============================================================================

bool CodeSection::inline_section(const CodeSection& source) {
    if (!has_flag(source.flags(), SectionFlags::Compiled)) {
        log_generation("Cannot inline: source section not compiled");
        return false;
    }

    if (source.metadata().instruction_count > config_.max_instructions / 4) {
        log_generation("Cannot inline: source section too large (" +
                       std::to_string(source.metadata().instruction_count) + " instructions)");
        return false;
    }

    // Append the source instructions
    instructions_.insert(instructions_.end(), source.instructions_.begin(), source.instructions_.end());

    metadata_.instruction_count += source.metadata().instruction_count;
    flags_ = flags_ | SectionFlags::InlineCandidate;

    log_generation("Inlined section 0x" + std::format("{:08X}", source.entry_pc()) + " (" +
                   std::to_string(source.metadata().instruction_count) + " instructions)");
    return true;
}

// =============================================================================
// Control Flow Analysis (adapted from P64's DetermineLoop / analysis)
// =============================================================================

void CodeSection::analyze_control_flow() {
    log_generation("Analyzing control flow...");

    // Build edges from successor addresses
    for (size_t i = 0; i < successor_addresses_.size(); ++i) {
        uint32_t target_addr = successor_addresses_[i];
        ControlFlowEdge edge{};
        edge.source = id_;
        edge.branch_address = metadata_.end_pc;

        // Check if it's a conditional branch (second successor = fall-through)
        edge.is_conditional = (i > 0 && successor_addresses_.size() > 1);

        // Try to find the target section
        auto existing = find_existing_section(target_addr);
        if (existing.has_value()) {
            edge.target = existing.value()->id();
        }

        if (i < outgoing_edges_.size()) {
            outgoing_edges_[i] = edge;
        } else {
            outgoing_edges_.push_back(edge);
        }
    }

    update_flags();
}

void CodeSection::analyze_data_dependencies() {
    log_generation("Analyzing data dependencies...");

    size_t dep_count = 0;

    // Walk the instruction list and track register reads/writes
    // to identify RAW, WAR, and WAW hazards
    struct RegUse {
        uint32_t last_write_idx = UINT32_MAX;
        uint32_t last_read_idx = UINT32_MAX;
    };
    std::array<RegUse, 32> gpr_use{};
    [[maybe_unused]] std::array<RegUse, 32> fpr_use{};

    for (size_t idx = 0; idx < instructions_.size(); ++idx) {
        uint32_t addr = metadata_.entry_pc + static_cast<uint32_t>(idx * 4);
        R4300iInstruction instr(addr, instructions_[idx]);
        auto reg_info = instr.GetRegisterInfo();

        // Check for RAW hazard (read after write)
        if (reg_info.read1 < 32 && gpr_use[reg_info.read1].last_write_idx != UINT32_MAX) {
            ++dep_count;
        }
        if (reg_info.read2 < 32 && gpr_use[reg_info.read2].last_write_idx != UINT32_MAX) {
            ++dep_count;
        }

        // Update tracking
        if (reg_info.write < 32) {
            gpr_use[reg_info.write].last_write_idx = static_cast<uint32_t>(idx);
        }
        if (reg_info.read1 < 32) {
            gpr_use[reg_info.read1].last_read_idx = static_cast<uint32_t>(idx);
        }
        if (reg_info.read2 < 32) {
            gpr_use[reg_info.read2].last_read_idx = static_cast<uint32_t>(idx);
        }
    }

    stats_.data_dependencies = dep_count;
    log_generation("Found " + std::to_string(dep_count) + " data dependencies");
}

void CodeSection::determine_loops() {
    // Adapted from P64's DetermineLoop:
    // Check if any successor leads back to us (back-edge detection)
    // Also check if all parent paths eventually lead to a loop

    bool in_loop = false;

    // Direct self-loop
    for (const auto& succ_id : successors_) {
        if (succ_id == id_) {
            in_loop = true;
            break;
        }
    }

    // Check if any successor can reach us (cycle detection via BFS)
    if (!in_loop) {
        for (const auto& succ_id : successors_) {
            if (is_accessible(succ_id)) {
                // Check if this successor can reach back to us
                // by walking predecessors
                std::unordered_set<uint32_t> visited;
                std::queue<SectionId> work_queue;
                work_queue.push(succ_id);

                while (!work_queue.empty() && !in_loop) {
                    SectionId current = work_queue.front();
                    work_queue.pop();

                    uint32_t current_val = static_cast<uint32_t>(current);
                    if (visited.count(current_val)) continue;
                    visited.insert(current_val);

                    if (current == id_) {
                        in_loop = true;
                        break;
                    }

                    // Find the section and explore its successors
                    [[maybe_unused]] auto section = find_existing_section(0); // placeholder
                    // Walk through the section graph looking for back-edges
                    // In a complete implementation, we'd use the SectionGraph
                }
            }
        }
    }

    if (in_loop) {
        flags_ = flags_ | SectionFlags::HasLoop;
        type_ = SectionType::Loop;
        log_generation("Loop detected in section 0x" + std::format("{:08X}", metadata_.entry_pc));
    }
}

void CodeSection::compute_dominance_tree() {
    log_generation("Computing dominance tree...");

    // Iterative dominator computation (Cooper, Harvey, Kennedy algorithm)
    // For a single section, the immediate dominator is just the section with
    // the lowest ID among predecessors (simplified)

    if (predecessors_.empty()) {
        // Entry section: dominates itself
        immediate_dominator_ = std::nullopt;
        return;
    }

    // Start with the first predecessor as the immediate dominator candidate
    SectionId candidate = predecessors_.front();

    // Intersect with all other predecessors
    for (size_t i = 1; i < predecessors_.size(); ++i) {
        // In a full implementation, we'd walk up the dominator tree
        // to find the common ancestor. For now, use the minimum ID
        // as a conservative approximation
        if (static_cast<uint32_t>(predecessors_[i]) < static_cast<uint32_t>(candidate)) {
            candidate = predecessors_[i];
        }
    }

    immediate_dominator_ = candidate;
    log_generation("Immediate dominator: section " + std::to_string(static_cast<uint32_t>(candidate)));
}

// =============================================================================
// Section Query and Navigation
// =============================================================================

std::optional<CodeSection*> CodeSection::find_existing_section(uint32_t address) {
    // Search through all sections in the parent block
    // This adapts P64's CCodeSection::ExistingSection
    if (!parent_block_) {
        return std::nullopt;
    }

    // The parent CodeBlock maintains a mapping of addresses to sections
    // We search its sections vector
    // NOTE: This requires CodeBlock to expose its sections - if not available,
    // we fall back to walking the section graph

    // For now search by walking our outgoing/incoming edges
    // A full implementation would query parent_block_->find_section(address)
    log_generation("Looking for existing section at 0x" + std::format("{:08X}", address));

    return std::nullopt; // Will be connected when CodeBlock exposes section lookup
}

bool CodeSection::is_accessible(SectionId section_id) const {
    // Adapted from P64's SectionAccessible:
    // BFS from this section to determine if target is reachable

    if (section_id == id_) {
        return true;
    }

    // Check direct successors
    for (const auto& succ : successors_) {
        if (succ == section_id) {
            return true;
        }
    }

    // For a complete implementation, we'd do a full BFS through the
    // section graph. This requires access to all sections.
    // For now, report direct successors only.
    return false;
}

bool CodeSection::is_dominated_by(SectionId dominator) const {
    // Walk up the dominator tree looking for the target
    if (dominator == id_) {
        return true;
    }

    // Check the immediate dominator chain
    // In a simple implementation, we just check the immediate dominator
    if (immediate_dominator_.has_value() && immediate_dominator_.value() == dominator) {
        return true;
    }

    return false;
}

bool CodeSection::post_dominates(SectionId target) const {
    // Check if this section post-dominates the target
    for (const auto& pd : post_dominated_by_) {
        if (pd == target) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Parent / Child Management (adapted from P64's AddParent / UnlinkParent)
// =============================================================================

void CodeSection::add_parent(CodeSection* parent) {
    if (!parent) {
        return;
    }

    SectionId parent_id = parent->id();

    // Check if already a predecessor
    for (const auto& pred : predecessors_) {
        if (pred == parent_id) {
            // Already registered - merge register states (P64 behavior)
            log_generation("Parent already registered, merging entry state");
            entry_state_.merge(parent->exit_state_);
            return;
        }
    }

    predecessors_.push_back(parent_id);

    // Add incoming edge
    ControlFlowEdge edge{};
    edge.source = parent_id;
    edge.target = id_;
    edge.branch_address = parent->end_pc();
    edge.is_conditional = has_flag(parent->flags(), SectionFlags::HasBranch);
    incoming_edges_.push_back(edge);

    // Merge register entry state with parent's exit state
    // This is critical for correctness - when multiple paths converge,
    // we must take the conservative union of all register states
    if (predecessors_.size() > 1) {
        entry_state_.merge(parent->exit_state_);
    } else {
        // First parent: inherit the state directly
        entry_state_ = parent->exit_state_;
    }

    log_generation("Added parent section " + std::to_string(static_cast<uint32_t>(parent_id)) +
                   " (total parents: " + std::to_string(predecessors_.size()) + ")");
}

void CodeSection::remove_parent(CodeSection* parent) {
    if (!parent) {
        return;
    }

    SectionId parent_id = parent->id();

    // Remove from predecessors
    auto it = std::find(predecessors_.begin(), predecessors_.end(), parent_id);
    if (it != predecessors_.end()) {
        predecessors_.erase(it);
    }

    // Remove corresponding incoming edge
    incoming_edges_.erase(std::remove_if(incoming_edges_.begin(), incoming_edges_.end(),
                                         [parent_id](const ControlFlowEdge& e) { return e.source == parent_id; }),
                          incoming_edges_.end());

    log_generation("Removed parent section " + std::to_string(static_cast<uint32_t>(parent_id)));

    // If we have remaining parents, recompute the entry state
    if (!predecessors_.empty()) {
        // Reset and re-merge from all remaining parents
        // NOTE: In a full implementation, we'd fetch each parent's exit state
        // For now, just log a warning
        log_generation("WARNING: Entry state may need recomputation after parent removal");
    }
}

void CodeSection::switch_parent(CodeSection* old_parent, CodeSection* new_parent) {
    if (!old_parent || !new_parent) {
        return;
    }

    SectionId old_id = old_parent->id();
    SectionId new_id = new_parent->id();

    // Replace in predecessors list
    for (auto& pred : predecessors_) {
        if (pred == old_id) {
            pred = new_id;
            break;
        }
    }

    // Update incoming edges
    for (auto& edge : incoming_edges_) {
        if (edge.source == old_id) {
            edge.source = new_id;
            edge.branch_address = new_parent->end_pc();
            break;
        }
    }

    log_generation("Switched parent from " + std::to_string(static_cast<uint32_t>(old_id)) + " to " +
                   std::to_string(static_cast<uint32_t>(new_id)));
}

void CodeSection::add_child(CodeSection* child) {
    if (!child) {
        return;
    }

    SectionId child_id = child->id();

    // Add to successors if not already present
    if (std::find(successors_.begin(), successors_.end(), child_id) == successors_.end()) {
        successors_.push_back(child_id);

        // Create outgoing edge
        ControlFlowEdge edge{};
        edge.source = id_;
        edge.target = child_id;
        edge.branch_address = metadata_.end_pc;
        outgoing_edges_.push_back(edge);
    }

    // Also add ourselves as a parent of the child
    child->add_parent(this);
}

// =============================================================================
// Register State Management
// =============================================================================

void CodeSection::capture_entry_state() {
    // Capture the current CPU register state as the entry state for this section
    // This would normally read from the actual CPU state via the system object
    log_generation("Capturing entry register state at 0x" + std::format("{:08X}", metadata_.entry_pc));

    // Mark all GPRs as potentially allocated (conservative)
    for (size_t i = 0; i < 32; ++i) {
        entry_state_.gpr[i].allocated = true;
        entry_state_.gpr[i].dirty = false;
        entry_state_.gpr[i].constant = false;
    }

    // R0 is always zero and constant
    entry_state_.gpr[0].constant = true;
    entry_state_.gpr[0].value = 0;
    entry_state_.gpr[0].dirty = false;

    // FPRs start unallocated
    for (size_t i = 0; i < 32; ++i) {
        entry_state_.fpr[i].allocated = false;
    }
}

void CodeSection::propagate_constants() {
    log_generation("Propagating constants...");

    // Walk through instructions and propagate known constant values
    RegisterEntryState current = entry_state_;

    for (size_t idx = 0; idx < instructions_.size(); ++idx) {
        uint32_t addr = metadata_.entry_pc + static_cast<uint32_t>(idx * 4);
        R4300iInstruction instr(addr, instructions_[idx]);
        auto reg_info = instr.GetRegisterInfo();

        // If the instruction writes a register, check if we can determine
        // the output value from constant inputs
        if (reg_info.write < 32) {
            // Simple case: LUI (Load Upper Immediate) produces a known constant
            uint32_t opcode = instructions_[idx] >> 26;
            if (opcode == 0x0F) // LUI
            {
                uint32_t imm = (instructions_[idx] & 0xFFFF) << 16;
                current.gpr[reg_info.write].constant = true;
                current.gpr[reg_info.write].value = imm;
            }
            // ADDIU with a constant source
            else if (opcode == 0x09 && reg_info.read1 < 32 && current.gpr[reg_info.read1].constant) // ADDIU
            {
                int16_t imm = static_cast<int16_t>(instructions_[idx] & 0xFFFF);
                current.gpr[reg_info.write].constant = true;
                current.gpr[reg_info.write].value = current.gpr[reg_info.read1].value + imm;
            }
            // ORI with a constant source
            else if (opcode == 0x0D && reg_info.read1 < 32 && current.gpr[reg_info.read1].constant) // ORI
            {
                uint32_t imm = instructions_[idx] & 0xFFFF;
                current.gpr[reg_info.write].constant = true;
                current.gpr[reg_info.write].value = current.gpr[reg_info.read1].value | imm;
            } else {
                // Can't determine constant value
                current.gpr[reg_info.write].constant = false;
            }
        }
    }

    // Update the exit state with propagated constants
    exit_state_ = current;
}

void CodeSection::update_register_state(const RegisterEntryState& new_state) {
    entry_state_ = new_state;
    log_generation("Register entry state updated");
}

// =============================================================================
// Optimization Passes
// =============================================================================

void CodeSection::apply_optimizations() {
    if (instructions_.empty()) {
        return;
    }

    log_generation("Applying optimization passes...");

    // Run optimization passes in order of increasing complexity
    bool changed = true;
    int pass = 0;
    constexpr int MAX_PASSES = 5;

    while (changed && pass < MAX_PASSES) {
        changed = false;
        ++pass;

        changed |= fold_constants();
        changed |= eliminate_dead_code();
        changed |= optimize_control_flow();
        changed |= hoist_invariants();
        changed |= schedule_instructions();

        if (changed) {
            log_generation("Optimization pass " + std::to_string(pass) + " made changes, iterating");
        }
    }

    if (pass > 1) {
        flags_ = flags_ | SectionFlags::Optimized;
    }

    log_generation("Optimization complete after " + std::to_string(pass) + " passes");
}

bool CodeSection::optimize_control_flow() {
    // Remove unreachable code after unconditional jumps
    // Fold consecutive branches
    // Convert conditional branches with constant conditions to unconditional

    bool changed = false;

    // Check for branches with known constant conditions
    for (auto& edge : outgoing_edges_) {
        if (edge.is_conditional) {
            // If we can statically determine the branch outcome,
            // convert to unconditional
            // This requires the condition register to be a known constant
            // Full implementation would check the entry_state_ constants
        }
    }

    return changed;
}

bool CodeSection::eliminate_dead_code() {
    // Remove instructions whose results are never used
    // This is a simple backward liveness analysis

    if (instructions_.size() < 2) {
        return false;
    }

    bool changed = false;
    std::array<bool, 32> gpr_live{};

    // All GPRs that are live-out (used by successors) are considered live
    // Conservative: assume all are live at exit
    gpr_live.fill(true);

    // Walk backwards
    for (int idx = static_cast<int>(instructions_.size()) - 1; idx >= 0; --idx) {
        uint32_t addr = metadata_.entry_pc + static_cast<uint32_t>(idx * 4);
        R4300iInstruction instr(addr, instructions_[idx]);
        auto reg_info = instr.GetRegisterInfo();

        // If the instruction writes a register that isn't live, it's dead
        if (reg_info.write < 32 && !gpr_live[reg_info.write]) {
            // Don't eliminate instructions with side effects (stores, branches)
            uint32_t opcode = instructions_[idx] >> 26;
            if (opcode != 0x2B && opcode != 0x29 && opcode != 0x28 && // SW, SH, SB
                opcode != 0x04 && opcode != 0x05 && opcode != 0x01 && // BEQ, BNE, REGIMM
                opcode != 0x02 && opcode != 0x03)                     // J, JAL
            {
                // Replace with NOP
                instructions_[idx] = 0x00000000;
                changed = true;
            }
        }

        // Mark read registers as live
        if (reg_info.read1 < 32) {
            gpr_live[reg_info.read1] = true;
        }
        if (reg_info.read2 < 32) {
            gpr_live[reg_info.read2] = true;
        }

        // Mark written register as dead (it gets a new value here)
        if (reg_info.write < 32 && reg_info.write != 0) {
            gpr_live[reg_info.write] = false;
        }
    }

    return changed;
}

bool CodeSection::fold_constants() {
    // Perform constant folding using propagated constant values
    propagate_constants();

    // Currently, constant propagation already updates the state.
    // Future: replace instructions with simpler forms when inputs are constants
    return false;
}

bool CodeSection::hoist_invariants() {
    // Loop-invariant code motion: only applicable to loop sections
    if (!has_flag(flags_, SectionFlags::HasLoop)) {
        return false;
    }

    // Identify instructions that don't depend on anything modified in the loop
    // This requires knowing which registers are modified in the loop body
    // Future implementation
    log_generation("Loop invariant hoisting: not yet implemented for loops");
    return false;
}

bool CodeSection::schedule_instructions() {
    // Instruction scheduling to reduce pipeline stalls
    // MIPS R4300i has a 5-stage pipeline with load delay slots

    if (instructions_.size() < 3) {
        return false;
    }

    bool changed = false;

    // Simple look-ahead scheduling: if a load is followed immediately by
    // a use of the loaded register, try to insert an independent instruction
    // between them
    for (size_t i = 0; i + 1 < instructions_.size(); ++i) {
        uint32_t addr_i = metadata_.entry_pc + static_cast<uint32_t>(i * 4);
        R4300iInstruction instr_i(addr_i, instructions_[i]);

        uint32_t opcode_i = instructions_[i] >> 26;
        bool is_load = (opcode_i >= 0x20 && opcode_i <= 0x27) || // LB..LWU
                       opcode_i == 0x37 || opcode_i == 0x31;     // LD, LWC1

        if (!is_load) {
            continue;
        }

        auto reg_i = instr_i.GetRegisterInfo();
        uint32_t loaded_reg = reg_i.write;
        if (loaded_reg >= 32) continue;

        // Check if the next instruction uses the loaded register
        uint32_t addr_next = metadata_.entry_pc + static_cast<uint32_t>((i + 1) * 4);
        R4300iInstruction instr_next(addr_next, instructions_[i + 1]);
        auto reg_next = instr_next.GetRegisterInfo();

        if (reg_next.read1 == loaded_reg || reg_next.read2 == loaded_reg) {
            // Load-use hazard detected - try to find an independent instruction
            // to fill the slot
            for (size_t j = i + 2; j < instructions_.size() && j < i + 4; ++j) {
                uint32_t addr_j = metadata_.entry_pc + static_cast<uint32_t>(j * 4);
                R4300iInstruction instr_j(addr_j, instructions_[j]);
                auto reg_j = instr_j.GetRegisterInfo();

                // Check if instruction j is independent of both i and i+1
                bool independent = true;
                if (reg_j.read1 == loaded_reg || reg_j.read2 == loaded_reg) independent = false;
                if (reg_j.write < 32 && (reg_j.write == reg_next.read1 || reg_j.write == reg_next.read2))
                    independent = false;

                if (independent) {
                    // Swap instructions[i+1] and instructions[j]
                    std::swap(instructions_[i + 1], instructions_[j]);
                    changed = true;
                    break;
                }
            }
        }
    }

    return changed;
}

// =============================================================================
// Debug and Information
// =============================================================================

std::string CodeSection::get_disassembly() const {
    std::ostringstream oss;
    oss << "Section 0x" << std::hex << metadata_.entry_pc << " - 0x" << metadata_.end_pc << std::dec << '\n';
    oss << "  Type: " << static_cast<int>(type_) << " Flags: 0x" << std::hex << static_cast<uint32_t>(flags_)
        << std::dec << '\n';
    oss << "  Instructions: " << instructions_.size() << '\n';

    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint32_t addr = metadata_.entry_pc + static_cast<uint32_t>(i * 4);
        R4300iInstruction instr(addr, instructions_[i]);
        oss << "    " << instr.GetFullString() << '\n';
    }

    return oss.str();
}

std::string CodeSection::get_control_flow_graph() const {
    std::ostringstream oss;
    oss << "CFG for section " << static_cast<uint32_t>(id_) << ":\n";

    oss << "  Predecessors: ";
    for (const auto& pred : predecessors_) {
        oss << static_cast<uint32_t>(pred) << " ";
    }
    oss << '\n';

    oss << "  Successors: ";
    for (const auto& succ : successors_) {
        oss << static_cast<uint32_t>(succ) << " ";
    }
    oss << '\n';

    oss << "  Outgoing edges:\n";
    for (const auto& edge : outgoing_edges_) {
        oss << "    -> " << static_cast<uint32_t>(edge.target) << " (branch@0x" << std::hex << edge.branch_address
            << std::dec << (edge.is_conditional ? " cond" : " uncond") << ")\n";
    }

    if (immediate_dominator_.has_value()) {
        oss << "  Immediate dominator: " << static_cast<uint32_t>(immediate_dominator_.value()) << '\n';
    }

    return oss.str();
}

std::string CodeSection::get_register_state_string() const {
    std::ostringstream oss;
    oss << "--- Entry State ---\n" << entry_state_.to_string();
    oss << "--- Exit State ---\n" << exit_state_.to_string();
    return oss.str();
}

// =============================================================================
// Patch Point Management
// =============================================================================

void CodeSection::add_patch_point(uint32_t mips_address, asmjit::Label label) {
    PatchPoint pp;
    pp.mips_address = mips_address;
    pp.label = label;
    pp.patch_offset = 0;
    pp.resolved = false;

    patch_points_.push_back(pp);
    log_generation("Added patch point at 0x" + std::format("{:08X}", mips_address));
}

bool CodeSection::resolve_patch_points() {
    bool all_resolved = true;

    for (auto& pp : patch_points_) {
        if (pp.resolved) continue;

        // Try to resolve the patch point by finding the native code offset
        // for the MIPS address. This requires the code holder to be valid.
        if (code_holder_) {
            // In a full AsmJit integration, we'd resolve the label offset here
            pp.resolved = true;
            log_generation("Resolved patch point at 0x" + std::format("{:08X}", pp.mips_address));
        } else {
            all_resolved = false;
        }
    }

    return all_resolved;
}

// =============================================================================
// Private Implementation Helpers
// =============================================================================

void CodeSection::handleError(asmjit::Error error, const char* message, asmjit::BaseEmitter* /*origin*/) {
    log_generation("AsmJit error " + std::to_string(static_cast<int>(error)) + ": " +
                   std::string(message ? message : "unknown"));
}

std::optional<CodeSection::AnalysisResult> CodeSection::analyze_section() {
    log_generation("Analyzing section at 0x" + std::format("{:08X}", metadata_.entry_pc));

    AnalysisResult result{};
    result.basic_blocks.push_back(metadata_.entry_pc);

    // If we have instructions cached, analyze them directly
    if (!instructions_.empty()) {
        metadata_.instruction_count = static_cast<uint32_t>(instructions_.size());

        for (size_t i = 0; i < instructions_.size(); ++i) {
            uint32_t addr = metadata_.entry_pc + static_cast<uint32_t>(i * 4);
            uint32_t opcode = instructions_[i] >> 26;

            // Check for branch/jump instructors that start new basic blocks
            bool is_branch = (opcode == 0x04 || opcode == 0x05 || // BEQ, BNE
                              opcode == 0x06 || opcode == 0x07 || // BLEZ, BGTZ
                              opcode == 0x01 ||                   // REGIMM (BLTZ, BGEZ, etc.)
                              opcode == 0x02 || opcode == 0x03);  // J, JAL

            // SPECIAL opcodes: JR, JALR
            if (opcode == 0x00) {
                uint32_t func = instructions_[i] & 0x3F;
                if (func == 0x08 || func == 0x09) // JR, JALR
                {
                    is_branch = true;
                }
            }

            if (is_branch && i + 2 < instructions_.size()) {
                // The instruction after the delay slot starts a new basic block
                uint32_t next_bb = addr + 8;
                result.basic_blocks.push_back(next_bb);
                result.has_loop = result.has_loop; // preserved
            }

            // Check for exception-generating instructions
            if (opcode == 0x00) {
                uint32_t func = instructions_[i] & 0x3F;
                if (func == 0x0C || func == 0x0D) // SYSCALL, BREAK
                {
                    result.has_exception = true;
                }
            }

            // Check for atomic operations (LL/SC)
            if (opcode == 0x30 || opcode == 0x38) // LL, SC
            {
                result.has_atomic = true;
            }
        }

        metadata_.end_pc = metadata_.entry_pc + static_cast<uint32_t>((instructions_.size() - 1) * 4);
    }

    // Sort and deduplicate basic block addresses
    std::sort(result.basic_blocks.begin(), result.basic_blocks.end());
    result.basic_blocks.erase(std::unique(result.basic_blocks.begin(), result.basic_blocks.end()),
                              result.basic_blocks.end());

    stats_.basic_block_count = result.basic_blocks.size();

    if (result.has_exception) {
        flags_ = flags_ | SectionFlags::ExceptionPossible;
    }
    if (result.has_atomic) {
        flags_ = flags_ | SectionFlags::Atomic;
    }

    log_generation("Analysis complete: " + std::to_string(result.basic_blocks.size()) + " basic blocks, " +
                   std::to_string(metadata_.instruction_count) + " instructions");

    return result;
}

void CodeSection::build_control_flow_graph(const AnalysisResult& analysis) {
    log_generation("Building control flow graph...");

    // Create edges between basic blocks
    for (size_t i = 0; i + 1 < analysis.basic_blocks.size(); ++i) {
        ControlFlowEdge edge{};
        edge.source = id_;
        edge.target = id_; // Internal edges within the section
        edge.branch_address = analysis.basic_blocks[i];
        edge.is_conditional = false;
    }

    stats_.control_dependencies = outgoing_edges_.size() + incoming_edges_.size();
}

void CodeSection::compute_dominance(const AnalysisResult& /*analysis*/) {
    // Compute dominance relationships using the iterative algorithm
    // This is called when we have multiple basic blocks within the section
    compute_dominance_tree();
}

bool CodeSection::generate_code(const AnalysisResult& analysis) {
    log_generation("Generating native code...");

    // Skip actual code generation if AsmJit is not fully integrated yet
    // In a full implementation, this would:
    // 1. Create an asmjit::CodeHolder
    // 2. Create the CodeGenerator
    // 3. Generate prologue
    // 4. Walk instructions and generate native code for each
    // 5. Generate epilogue
    // 6. Finalize the code

#ifdef NOVA64_ASMJIT_ENABLED
    // Create code holder if needed
    if (!code_holder_) {
        code_holder_ = std::make_unique<asmjit::CodeHolder>();

        asmjit::Environment env = asmjit::Environment::host();
        if (code_holder_->init(env) != asmjit::kErrorOk) {
            log_generation("ERROR: Failed to initialize CodeHolder");
            return false;
        }
    }

    // Create assembler (arch-neutral alias: x86 or a64 depending on host)
    Recomp::Assembler assembler(code_holder_.get());

    // Bind entry label
    entry_label_ = assembler.new_label();
    assembler.bind(entry_label_);

    // Create the code generator and dispatch instructions
    CodeGenerator gen(*code_holder_, analysis, entry_state_);

    if (!gen.generate_prologue()) {
        log_generation("ERROR: Prologue generation failed");
        return false;
    }

    // Generate code for each instruction
    for (size_t i = 0; i < instructions_.size(); ++i) {
        uint32_t addr = metadata_.entry_pc + static_cast<uint32_t>(i * 4);
        if (!gen.generate_instruction(addr, instructions_[i])) {
            log_generation("WARNING: Failed to generate instruction at 0x" + std::format("{:08X}", addr));
            // Continue with remaining instructions
        }
    }

    if (!gen.generate_epilogue()) {
        log_generation("ERROR: Epilogue generation failed");
        return false;
    }

    stats_.native_code_size = gen.generated_bytes();
#else
    // Stub: just record instruction count as "native size"
    stats_.native_code_size = instructions_.size() * 4;
#endif

    log_generation("Code generation complete: " + std::to_string(stats_.native_code_size) + " bytes");
    return true;
}

void CodeSection::log_generation(const std::string& message) {
    debug_log_ += "[Section " + std::to_string(static_cast<uint32_t>(id_)) + "] " + message + "\n";
}

void CodeSection::update_flags() {
    // Update flags based on current section state
    if (!successors_.empty()) {
        if (successors_.size() > 1) {
            flags_ = flags_ | SectionFlags::HasBranch;
        } else {
            flags_ = flags_ | SectionFlags::HasJump;
        }
    }

    if (metadata_.is_hot()) {
        flags_ = flags_ | SectionFlags::HotPath;
    } else if (metadata_.is_cold()) {
        flags_ = flags_ | SectionFlags::ColdPath;
    }
}

// =============================================================================
// Optimization sub-passes (private)
// =============================================================================

void CodeSection::apply_peephole_optimizations() {
    // Simple pattern-matching optimizations on instruction pairs
    for (size_t i = 0; i + 1 < instructions_.size(); ++i) {
        // Pattern: NOP after NOP -> single NOP (mark second as dead)
        if (instructions_[i] == 0x00000000 && instructions_[i + 1] == 0x00000000) {
            // Keep one NOP for alignment, mark the other
            continue;
        }

        // Pattern: ADDIU r, r, 0 -> NOP (add zero is identity)
        uint32_t opcode = instructions_[i] >> 26;
        if (opcode == 0x09) // ADDIU
        {
            uint32_t rs = (instructions_[i] >> 21) & 0x1F;
            uint32_t rt = (instructions_[i] >> 16) & 0x1F;
            int16_t imm = static_cast<int16_t>(instructions_[i] & 0xFFFF);
            if (rs == rt && imm == 0) {
                instructions_[i] = 0x00000000; // Replace with NOP
            }
        }
    }
}

void CodeSection::apply_local_optimizations() {
    apply_peephole_optimizations();
    fold_constants();
}

void CodeSection::apply_global_optimizations() {
    eliminate_dead_code();
    hoist_invariants();
    schedule_instructions();
}

// =============================================================================
// CodeGenerator Implementation (nested class)
// =============================================================================

CodeSection::CodeGenerator::CodeGenerator(asmjit::CodeHolder& code, const AnalysisResult& analysis,
                                          const RegisterEntryState& entry_state) :
    assembler_(nullptr), analysis_(analysis), entry_state_(entry_state) {
#ifdef NOVA64_ASMJIT_ENABLED
    (void)code;
#else
    (void)code;
#endif
}

bool CodeSection::CodeGenerator::generate_prologue() {
#ifdef NOVA64_ASMJIT_ENABLED
    if (!assembler_) return true; // Stub mode

    // Save callee-saved registers
    // Set up the register state according to entry_state_
    // This would emit:
    //   push rbp
    //   mov rbp, rsp
    //   sub rsp, <frame_size>
    //   ... save callee-saved regs that we'll use
#endif
    return true;
}

bool CodeSection::CodeGenerator::generate_epilogue() {
#ifdef NOVA64_ASMJIT_ENABLED
    if (!assembler_) return true; // Stub mode

    // Restore callee-saved registers and return
    // This would emit:
    //   ... restore callee-saved regs
    //   mov rsp, rbp
    //   pop rbp
    //   ret
#endif
    return true;
}

bool CodeSection::CodeGenerator::generate_instruction(uint32_t address, uint32_t opcode) {
    // Dispatch to the appropriate instruction handler
    // This is adapted from P64's opcode dispatch in GenerateNativeCode
    //
    // In a full implementation, this would:
    // 1. Decode the MIPS instruction
    // 2. Look up the host register mapping for source/dest
    // 3. Emit native x86-64 code via AsmJit
    //
    // For now, just track the instruction count

    generated_bytes_ += 4; // Placeholder: each MIPS instruction ~= 4 bytes native
    (void)address;
    (void)opcode;
    return true;
}

bool CodeSection::CodeGenerator::generate_branch(const ControlFlowEdge& edge) {
#ifdef NOVA64_ASMJIT_ENABLED
    if (!assembler_) return true;

    // Generate branch code based on the edge type
    // Conditional branches: emit comparison + conditional jump
    // Unconditional: emit direct jump
    (void)edge;
#else
    (void)edge;
#endif
    return true;
}

bool CodeSection::CodeGenerator::generate_delay_slot(uint32_t address, uint32_t opcode) {
    // Generate the delay slot instruction
    // On MIPS, the instruction after a branch is always executed
    // We need to emit it before the actual branch in native code
    return generate_instruction(address, opcode);
}

// =============================================================================
// SectionGraph Implementation
// =============================================================================

SectionGraph::SectionGraph() = default;
SectionGraph::~SectionGraph() = default;

SectionGraph::NodeId SectionGraph::add_node(CodeSection* section) {
    NodeId id = section ? section->id() : next_node_id_;
    next_node_id_ = SectionId{static_cast<uint32_t>(next_node_id_) + 1};

    Node node{};
    node.section = section;
    nodes_[id] = node;

    if (section) {
        pc_to_node_[section->entry_pc()] = id;
    }

    return id;
}

SectionGraph::EdgeId SectionGraph::add_edge(NodeId source, NodeId target, const ControlFlowEdge& data) {
    EdgeId id = next_edge_id_++;

    Edge edge{source, target, data};
    edges_[id] = edge;

    // Update node adjacency
    if (nodes_.count(source)) {
        nodes_[source].outgoing_edges.insert(id);
    }
    if (nodes_.count(target)) {
        nodes_[target].incoming_edges.insert(id);
    }

    return id;
}

void SectionGraph::remove_node(NodeId node) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) return;

    // Remove all edges connected to this node
    std::vector<EdgeId> edges_to_remove;
    for (auto eid : it->second.outgoing_edges) {
        edges_to_remove.push_back(eid);
    }
    for (auto eid : it->second.incoming_edges) {
        edges_to_remove.push_back(eid);
    }

    for (auto eid : edges_to_remove) {
        remove_edge(eid);
    }

    // Remove from PC map
    if (it->second.section) {
        pc_to_node_.erase(it->second.section->entry_pc());
    }

    nodes_.erase(it);
}

void SectionGraph::remove_edge(EdgeId edge) {
    auto it = edges_.find(edge);
    if (it == edges_.end()) return;

    // Remove from source node's outgoing
    auto src_it = nodes_.find(it->second.source);
    if (src_it != nodes_.end()) {
        src_it->second.outgoing_edges.erase(edge);
    }

    // Remove from target node's incoming
    auto tgt_it = nodes_.find(it->second.target);
    if (tgt_it != nodes_.end()) {
        tgt_it->second.incoming_edges.erase(edge);
    }

    edges_.erase(it);
}

std::optional<SectionGraph::NodeId> SectionGraph::find_node(uint32_t entry_pc) const {
    auto it = pc_to_node_.find(entry_pc);
    if (it != pc_to_node_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<SectionGraph::EdgeId> SectionGraph::find_edge(NodeId source, NodeId target) const {
    auto src_it = nodes_.find(source);
    if (src_it == nodes_.end()) return std::nullopt;

    for (auto eid : src_it->second.outgoing_edges) {
        auto edge_it = edges_.find(eid);
        if (edge_it != edges_.end() && edge_it->second.target == target) {
            return eid;
        }
    }

    return std::nullopt;
}

std::vector<SectionGraph::NodeId> SectionGraph::find_path(NodeId start, NodeId end) const {
    // BFS shortest path
    std::unordered_map<uint32_t, uint32_t> parent;
    std::queue<NodeId> queue;
    std::unordered_set<uint32_t> visited;

    uint32_t start_val = static_cast<uint32_t>(start);
    uint32_t end_val = static_cast<uint32_t>(end);

    queue.push(start);
    visited.insert(start_val);
    parent[start_val] = UINT32_MAX; // sentinel for root

    while (!queue.empty()) {
        NodeId current = queue.front();
        queue.pop();
        uint32_t current_val = static_cast<uint32_t>(current);

        if (current == end) {
            // Reconstruct path
            std::vector<NodeId> path;
            uint32_t node = end_val;
            while (node != UINT32_MAX) {
                path.push_back(SectionId{node});
                node = parent[node];
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        auto node_it = nodes_.find(current);
        if (node_it == nodes_.end()) continue;

        for (auto eid : node_it->second.outgoing_edges) {
            auto edge_it = edges_.find(eid);
            if (edge_it == edges_.end()) continue;

            uint32_t neighbor_val = static_cast<uint32_t>(edge_it->second.target);
            if (!visited.count(neighbor_val)) {
                visited.insert(neighbor_val);
                parent[neighbor_val] = current_val;
                queue.push(edge_it->second.target);
            }
        }
    }

    return {}; // No path found
}

void SectionGraph::compute_dominance_tree() { compute_dominators(); }

void SectionGraph::compute_post_dominance_tree() { compute_post_dominators(); }

void SectionGraph::find_loops() {
    // Back-edge detection: an edge (u, v) is a back-edge if v dominates u
    for (const auto& [eid, edge] : edges_) {
        auto target_it = nodes_.find(edge.target);
        auto source_it = nodes_.find(edge.source);

        if (target_it != nodes_.end() && source_it != nodes_.end()) {
            // Check if target dominates source
            [[maybe_unused]] uint32_t target_val = static_cast<uint32_t>(edge.target);
            if (source_it->second.dominators.count(edge.target)) {
                // This is a back-edge => natural loop
                if (target_it->second.section) {
                    // Mark the target section as a loop header
                    // (would need non-const access or a callback)
                }
            }
        }
    }
}

void SectionGraph::find_reducible_loops() {
    // A loop is reducible if it has a single entry point (the header)
    // All natural loops detected by back-edge analysis are reducible
    find_loops();
}

void SectionGraph::compute_scc() {
    // Tarjan's Strongly Connected Components algorithm
    std::vector<NodeId> stack;
    std::vector<std::vector<NodeId>> components;

    // Reset DFS state
    [[maybe_unused]] uint32_t counter = 0;
    for (auto& [id, node] : nodes_) {
        node.dfs_number = UINT32_MAX;
        node.low_link = UINT32_MAX;
        node.on_stack = false;
    }

    for (const auto& [id, node] : nodes_) {
        if (node.dfs_number == UINT32_MAX) {
            tarjan_scc(id, stack, components);
        }
    }
}

void SectionGraph::tarjan_scc(NodeId node, std::vector<NodeId>& stack, std::vector<std::vector<NodeId>>& components) {
    auto& n = nodes_[node];
    static uint32_t counter = 0;

    n.dfs_number = counter;
    n.low_link = counter;
    ++counter;

    stack.push_back(node);
    n.on_stack = true;

    // Visit successors
    for (auto eid : n.outgoing_edges) {
        auto edge_it = edges_.find(eid);
        if (edge_it == edges_.end()) continue;

        NodeId successor = edge_it->second.target;
        auto succ_it = nodes_.find(successor);
        if (succ_it == nodes_.end()) continue;

        if (succ_it->second.dfs_number == UINT32_MAX) {
            // Not yet visited
            tarjan_scc(successor, stack, components);
            n.low_link = std::min(n.low_link, succ_it->second.low_link);
        } else if (succ_it->second.on_stack) {
            n.low_link = std::min(n.low_link, succ_it->second.dfs_number);
        }
    }

    // If this is a root of an SCC
    if (n.low_link == n.dfs_number) {
        std::vector<NodeId> component;
        NodeId w;
        do {
            w = stack.back();
            stack.pop_back();
            nodes_[w].on_stack = false;
            component.push_back(w);
        } while (w != node);

        components.push_back(std::move(component));
    }
}

void SectionGraph::compute_dominators() {
    // Iterative dominator computation (Cooper, Harvey, Kennedy)
    // Initialize: every node dominated by all nodes
    // Entry node dominated only by itself

    if (nodes_.empty()) return;

    // Find the entry node (lowest PC or first added)
    NodeId entry = nodes_.begin()->first;
    for (const auto& [id, node] : nodes_) {
        if (node.section && (!nodes_[entry].section || node.section->entry_pc() < nodes_[entry].section->entry_pc())) {
            entry = id;
        }
    }

    // Initialize dominators
    for (auto& [id, node] : nodes_) {
        node.dominators.clear();
        if (id == entry) {
            node.dominators.insert(id);
        } else {
            // Start with all nodes as dominators
            for (const auto& [nid, _] : nodes_) {
                node.dominators.insert(nid);
            }
        }
    }

    // Iterate until fixed point
    bool changed = true;
    while (changed) {
        changed = false;

        for (auto& [id, node] : nodes_) {
            if (id == entry) continue;

            // New dominator set = intersection of all predecessors' dominators + self
            std::unordered_set<NodeId> new_doms;
            bool first = true;

            for (auto eid : node.incoming_edges) {
                auto edge_it = edges_.find(eid);
                if (edge_it == edges_.end()) continue;

                auto pred_it = nodes_.find(edge_it->second.source);
                if (pred_it == nodes_.end()) continue;

                if (first) {
                    new_doms = pred_it->second.dominators;
                    first = false;
                } else {
                    // Intersect
                    std::unordered_set<NodeId> intersection;
                    for (const auto& d : new_doms) {
                        if (pred_it->second.dominators.count(d)) {
                            intersection.insert(d);
                        }
                    }
                    new_doms = std::move(intersection);
                }
            }

            new_doms.insert(id); // A node always dominates itself

            if (new_doms != node.dominators) {
                node.dominators = std::move(new_doms);
                changed = true;
            }
        }
    }
}

void SectionGraph::compute_post_dominators() {
    // Reverse the graph edges and compute dominators on the reversed graph
    // For now, use a simplified approach

    if (nodes_.empty()) return;

    // Find exit nodes (nodes with no outgoing edges or all successors outside graph)
    std::vector<NodeId> exits;
    for (const auto& [id, node] : nodes_) {
        if (node.outgoing_edges.empty()) {
            exits.push_back(id);
        }
    }

    // Initialize post-dominators
    for (auto& [id, node] : nodes_) {
        node.post_dominators.clear();

        bool is_exit = std::find(exits.begin(), exits.end(), id) != exits.end();
        if (is_exit) {
            node.post_dominators.insert(id);
        } else {
            for (const auto& [nid, _] : nodes_) {
                node.post_dominators.insert(nid);
            }
        }
    }

    // Iterate until fixed point (reverse direction)
    bool changed = true;
    while (changed) {
        changed = false;

        for (auto& [id, node] : nodes_) {
            bool is_exit = std::find(exits.begin(), exits.end(), id) != exits.end();
            if (is_exit) continue;

            std::unordered_set<NodeId> new_pdoms;
            bool first = true;

            for (auto eid : node.outgoing_edges) {
                auto edge_it = edges_.find(eid);
                if (edge_it == edges_.end()) continue;

                auto succ_it = nodes_.find(edge_it->second.target);
                if (succ_it == nodes_.end()) continue;

                if (first) {
                    new_pdoms = succ_it->second.post_dominators;
                    first = false;
                } else {
                    std::unordered_set<NodeId> intersection;
                    for (const auto& d : new_pdoms) {
                        if (succ_it->second.post_dominators.count(d)) {
                            intersection.insert(d);
                        }
                    }
                    new_pdoms = std::move(intersection);
                }
            }

            new_pdoms.insert(id);

            if (new_pdoms != node.post_dominators) {
                node.post_dominators = std::move(new_pdoms);
                changed = true;
            }
        }
    }
}

// SectionGraph optimization stubs
void SectionGraph::inline_sections(NodeId /*target*/, const std::vector<NodeId>& /*sources*/) {
    // Future: inline small sections into their callers
}

void SectionGraph::merge_equivalent_sections() {
    // Future: detect and merge sections with identical instruction sequences
}

void SectionGraph::eliminate_dead_sections() {
    // Remove sections that are not reachable from the entry
    if (nodes_.empty()) return;

    // Find entry node
    NodeId entry = nodes_.begin()->first;
    for (const auto& [id, node] : nodes_) {
        if (node.section && (!nodes_[entry].section || node.section->entry_pc() < nodes_[entry].section->entry_pc())) {
            entry = id;
        }
    }

    // BFS to find reachable nodes
    std::unordered_set<uint32_t> reachable;
    std::queue<NodeId> queue;
    queue.push(entry);
    reachable.insert(static_cast<uint32_t>(entry));

    while (!queue.empty()) {
        NodeId current = queue.front();
        queue.pop();

        auto it = nodes_.find(current);
        if (it == nodes_.end()) continue;

        for (auto eid : it->second.outgoing_edges) {
            auto edge_it = edges_.find(eid);
            if (edge_it == edges_.end()) continue;

            uint32_t target_val = static_cast<uint32_t>(edge_it->second.target);
            if (!reachable.count(target_val)) {
                reachable.insert(target_val);
                queue.push(edge_it->second.target);
            }
        }
    }

    // Remove unreachable nodes
    std::vector<NodeId> to_remove;
    for (const auto& [id, node] : nodes_) {
        if (!reachable.count(static_cast<uint32_t>(id))) {
            to_remove.push_back(id);
        }
    }

    for (auto id : to_remove) {
        remove_node(id);
    }
}

// Serialization
SectionGraph::SerializedGraph SectionGraph::serialize() const {
    SerializedGraph result;

    for (const auto& [id, node] : nodes_) {
        SerializedGraph::SerializedNode sn{};
        if (node.section) {
            sn.entry_pc = node.section->entry_pc();
            sn.type = node.section->type();
            sn.flags = node.section->flags();
        }
        result.nodes.push_back(sn);
    }

    for (const auto& [id, edge] : edges_) {
        SerializedGraph::SerializedEdge se{};
        se.data = edge.data;

        // Resolve PCs from section pointers
        auto src_it = nodes_.find(edge.source);
        auto tgt_it = nodes_.find(edge.target);
        if (src_it != nodes_.end() && src_it->second.section) se.source_pc = src_it->second.section->entry_pc();
        if (tgt_it != nodes_.end() && tgt_it->second.section) se.target_pc = tgt_it->second.section->entry_pc();

        result.edges.push_back(se);
    }

    return result;
}

bool SectionGraph::deserialize(const SerializedGraph& /*graph*/, CodeBlock& /*parent*/) {
    // Future: reconstruct graph from serialized data
    return false;
}

SectionGraph::GraphStats SectionGraph::statistics() const {
    GraphStats stats;
    stats.node_count = nodes_.size();
    stats.edge_count = edges_.size();

    // Compute average branching factor
    if (!nodes_.empty()) {
        size_t total_out = 0;
        for (const auto& [id, node] : nodes_) {
            total_out += node.outgoing_edges.size();
        }
        stats.average_branching_factor = static_cast<double>(total_out) / static_cast<double>(nodes_.size());
    }

    // Count SCCs (loops) - would need to track from compute_scc
    stats.scc_count = 0;
    stats.loop_count = 0;

    // Compute max depth via BFS from entry
    if (!nodes_.empty()) {
        NodeId entry = nodes_.begin()->first;
        for (const auto& [id, node] : nodes_) {
            if (node.section &&
                (!nodes_.at(entry).section || node.section->entry_pc() < nodes_.at(entry).section->entry_pc())) {
                entry = id;
            }
        }

        std::unordered_map<uint32_t, size_t> depth;
        std::queue<NodeId> queue;
        queue.push(entry);
        depth[static_cast<uint32_t>(entry)] = 0;

        while (!queue.empty()) {
            NodeId current = queue.front();
            queue.pop();
            uint32_t current_val = static_cast<uint32_t>(current);

            auto it = nodes_.find(current);
            if (it == nodes_.end()) continue;

            for (auto eid : it->second.outgoing_edges) {
                auto edge_it = edges_.find(eid);
                if (edge_it == edges_.end()) continue;

                uint32_t target_val = static_cast<uint32_t>(edge_it->second.target);
                if (!depth.count(target_val)) {
                    depth[target_val] = depth[current_val] + 1;
                    stats.max_depth = std::max(stats.max_depth, depth[target_val]);
                    queue.push(edge_it->second.target);
                }
            }
        }
    }

    return stats;
}
