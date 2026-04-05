/**
 * @file ExitInfo.cpp
 * @brief Implementation of exit information and control flow management
 * 
 * Nova64 N64 Emulator - Modern C++ reimplementation
 * Based on Project64 recompiler with modern C++ improvements
 */

#include "ExitInfo.hpp"
#include <sstream>
#include <iomanip>
#include "CodeBlock.hpp"
#include "../../common/log.hpp"
#include "CodeSection.hpp"
#include "RegInfo.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/trace.hpp"
#include "../../common/MemoryManagment.hpp"

// If TRACE is used as a macro, we need to ensure its expansion is valid.
#ifndef TRACE
#define TRACE(fmt, ...) Log::debugf(fmt, ##__VA_ARGS__)
#endif

namespace Nova64 {
namespace Recompiler {

// ============================================================================
// ExitRegisterState Implementation
// ============================================================================

bool ExitRegisterState::operator==(const ExitRegisterState& other) const {
    // Compare all register snapshots
    for (size_t i = 0; i < 32; ++i) {
        if (!(gpr[i] == other.gpr[i])) return false;
        if (!(fpr[i] == other.fpr[i])) return false;
    }
    return hi == other.hi && lo == other.lo && pc == other.pc;
}

void ExitRegisterState::merge(const ExitRegisterState& other) {
    // Merge register states - if states differ, mark as not constant
    auto merge_snapshot = [](RegisterSnapshot& a, const RegisterSnapshot& b) {
        if (!b.valid) return;
        
        if (!a.valid) {
            a = b;
        } else {
            // If both valid but different, mark as not constant
            if (a.constant && b.constant && a.value != b.value) {
                a.constant = false;
            }
            a.dirty = a.dirty || b.dirty;
        }
    };
    
    for (size_t i = 0; i < 32; ++i) {
        merge_snapshot(gpr[i], other.gpr[i]);
        merge_snapshot(fpr[i], other.fpr[i]);
    }
    merge_snapshot(hi, other.hi);
    merge_snapshot(lo, other.lo);
    merge_snapshot(pc, other.pc);
}

void ExitRegisterState::reset() {
    for (auto& reg : gpr) reg = RegisterSnapshot{};
    for (auto& reg : fpr) reg = RegisterSnapshot{};
    hi = RegisterSnapshot{};
    lo = RegisterSnapshot{};
    pc = RegisterSnapshot{};
}

std::string ExitRegisterState::to_string() const {
    std::ostringstream oss;
    oss << "ExitRegisterState:\n";
    
    // GPRs
    for (size_t i = 0; i < 32; ++i) {
        if (gpr[i].valid) {
            oss << "  r" << i << ": ";
            if (gpr[i].constant) {
                oss << "const=0x" << std::hex << std::setw(8) << std::setfill('0') 
                    << gpr[i].value;
            } else if (gpr[i].dirty) {
                oss << "dirty";
            } else {
                oss << "valid";
            }
            if (gpr[i].physical_reg != 0xFFFFFFFF) {
                oss << " (phys=" << gpr[i].physical_reg << ")";
            }
            oss << "\n";
        }
    }
    
    // Special registers
    if (hi.valid) oss << "  HI: " << (hi.constant ? "const" : "valid") << "\n";
    if (lo.valid) oss << "  LO: " << (lo.constant ? "const" : "valid") << "\n";
    if (pc.valid) oss << "  PC: 0x" << std::hex << pc.value << "\n";
    
    return oss.str();
}

// ============================================================================
// ExceptionInfo Implementation
// ============================================================================

std::string ExceptionInfo::description() const {
    std::ostringstream oss;
    oss << "Exception at stage " << static_cast<int>(stage);
    oss << ", reason: " << static_cast<int>(reason);
    if (virtual_address != 0) {
        oss << ", vaddr: 0x" << std::hex << virtual_address;
    }
    return oss.str();
}

bool ExceptionInfo::is_recoverable() const {
    // TLB misses are typically recoverable
    return reason == ExitReason::TLBReadMiss || 
           reason == ExitReason::TLBWriteMiss;
}

bool ExceptionInfo::requires_rollback() const {
    // Address errors and bus errors may require rollback
    return reason == ExitReason::AddressErrorRead ||
           reason == ExitReason::AddressErrorWrite ||
           reason == ExitReason::BusErrorInstruction ||
           reason == ExitReason::BusErrorData;
}

// ============================================================================
// BranchPrediction Implementation
// ============================================================================

void BranchPrediction::update(bool actually_taken, uint32_t actual_target) {
    ++training_count;
    
    // Update prediction using a simple adaptive mechanism
    const double learning_rate = 0.1;
    
    if (actually_taken == predicted_taken) {
        // Correct prediction - increase confidence
        confidence = std::min(1.0, confidence + learning_rate);
    } else {
        // Wrong prediction - decrease confidence and flip
        confidence = std::max(0.0, confidence - learning_rate * 2.0);
        if (confidence < 0.5) {
            predicted_taken = actually_taken;
        }
    }
    
    // Update target prediction
    if (actually_taken && actual_target != 0) {
        predicted_target = actual_target;
    }
}

// ============================================================================
// ExitInfo Implementation
// ============================================================================

ExitInfo::ExitInfo(CodeBlock& parent_block, ExitId id, const Config& config)
    : parent_block_(parent_block)
    , id_(id)
    , config_(config)
{
    // Initialize with sensible defaults
    register_state_.reset();
    
    if (config_.enable_profiling) {
        execution_history_.reserve(256);
    }
}

ExitInfo::~ExitInfo() {
    // Cleanup
}

ExitInfo::ExitInfo(ExitInfo&& other) noexcept
    : parent_block_(other.parent_block_)
    , id_(other.id_)
    , target_pc_(other.target_pc_)
    , reason_(other.reason_)
    , category_(other.category_)
    , flags_(other.flags_)
    , pipeline_stage_(other.pipeline_stage_)
    , exception_info_(std::move(other.exception_info_))
    , register_state_(std::move(other.register_state_))
    , prediction_(std::move(other.prediction_))
    , stats_(std::move(other.stats_))
    , jump_label_(other.jump_label_)
    , config_(other.config_)
    , debug_name_(std::move(other.debug_name_))
    , execution_history_(std::move(other.execution_history_))
{
}

ExitInfo& ExitInfo::operator=(ExitInfo&& other) noexcept {
    if (this != &other) {
        // Can't move parent_block_ reference
        id_ = other.id_;
        target_pc_ = other.target_pc_;
        reason_ = other.reason_;
        category_ = other.category_;
        flags_ = other.flags_;
        pipeline_stage_ = other.pipeline_stage_;
        exception_info_ = std::move(other.exception_info_);
        register_state_ = std::move(other.register_state_);
        prediction_ = std::move(other.prediction_);
        stats_ = std::move(other.stats_);
        jump_label_ = other.jump_label_;
        config_ = other.config_;
        debug_name_ = std::move(other.debug_name_);
        execution_history_ = std::move(other.execution_history_);
    }
    return *this;
}

void ExitInfo::set_target(uint32_t target_pc, ExitReason reason, ExitCategory category) {
    target_pc_ = target_pc;
    reason_ = reason;
    category_ = category;
    
    // Update flags based on reason
    update_flags();
}

void ExitInfo::set_exception_info(const ExceptionInfo& exception) {
    exception_info_ = exception;
    category_ = ExitCategory::Exception;
    flags_ = flags_ | ExitFlags::ExceptionPossible;
}

void ExitInfo::capture_register_state(const ::CRegisters& registers) {
    if (!config_.enable_register_state) return;
    
    // Capture current register state
    for (size_t i = 0; i < 32; ++i) {
        auto& snap = register_state_.gpr[i];
        snap.valid = true;
        snap.value = static_cast<uint32_t>(registers.m_GPR[i]); // Explicit cast
        // Would need additional info to determine if constant/dirty
    }
    
    register_state_.pc.valid = true;
    register_state_.pc.value = registers.m_PROGRAM_COUNTER; // Use correct member m_PROGRAM_COUNTER
}

void ExitInfo::restore_register_state(::CRegisters& registers) const {
    if (!config_.enable_register_state) return;
    
    // Restore register state (mainly for debugging/validation)
    for (size_t i = 0; i < 32; ++i) {
        if (register_state_.gpr[i].valid && register_state_.gpr[i].constant) {
            registers.m_GPR[i] = register_state_.gpr[i].value; // Use correct member m_GPR
        }
    }
}

void ExitInfo::predict_branch() {
    if (!config_.enable_branch_prediction) return;
    
    // Use simple static prediction for now
    // Could be enhanced with more sophisticated predictors
    if (stats_.execution_count > 10) {
        prediction_.predicted_taken = stats_.execution_count > 
                                     (stats_.execution_count / 2);
        prediction_.confidence = 0.7;
    }
}

void ExitInfo::update_prediction(bool taken, uint32_t actual_target) {
    if (!config_.enable_branch_prediction) return;
    
    prediction_.update(taken, actual_target);
    
    // Update statistics
    stats_.record_prediction(prediction_.predicted_taken == taken);
    
    // Update flags based on prediction accuracy
    if (prediction_.prediction_accuracy() > 0.8) {
        if (taken) {
            flags_ = flags_ | ExitFlags::Likely;
        } else {
            flags_ = flags_ | ExitFlags::Unlikely;
        }
    }
}

void ExitInfo::record_execution(uint64_t cycles) {
    stats_.record_execution(cycles);
    
    // Update probability
    update_probability();
    
    // Track execution history for profiling
    if (config_.enable_profiling && execution_history_.size() < 1024) {
        execution_history_.push_back(target_pc_);
    }
    
    // Update flags
    if (stats_.is_hot()) {
        flags_ = flags_ | ExitFlags::Hot;
    } else if (stats_.is_cold()) {
        flags_ = flags_ | ExitFlags::Cold;
    }
}

void ExitInfo::optimize() {
    // Apply various optimization strategies
    apply_peephole_optimization();
    
    if (config_.enable_profiling && stats_.execution_count > 100) {
        apply_profile_guided_optimization();
    }
    
    if (should_speculate()) {
        apply_speculative_optimization();
    }
    
    flags_ = flags_ | ExitFlags::Optimized;
}

bool ExitInfo::can_inline() const {
    // Check if this exit is a good candidate for inlining
    if (!stats_.is_hot()) return false;
    if (has_flag(flags_, ExitFlags::ExceptionPossible)) return false;
    if (category_ == ExitCategory::Exception) return false;
    
    return true;
}

bool ExitInfo::should_speculate() const {
    // Check if speculative optimization is worthwhile
    if (!config_.enable_branch_prediction) return false;
    if (category_ == ExitCategory::Exception) return false;
    
    return prediction_.get_confidence() > 0.85 && stats_.is_hot();
}

bool ExitInfo::is_profitable_to_link() const {
    // Check if direct linking would be beneficial
    if (category_ == ExitCategory::Exception) return false;
    if (has_flag(flags_, ExitFlags::SelfModifying)) return false;
    
    return stats_.execution_count > 50 && 
           stats_.prediction_accuracy() > 0.9;
}

std::string ExitInfo::get_description() const {
    std::ostringstream oss;
    oss << "Exit ID: " << static_cast<uint32_t>(id_) << "\n";
    oss << "Target PC: 0x" << std::hex << std::setw(8) << std::setfill('0') 
        << target_pc_ << "\n";
    oss << "Reason: " << static_cast<int>(reason_) << "\n";
    oss << "Category: " << static_cast<int>(category_) << "\n";
    oss << "Executions: " << std::dec << stats_.execution_count << "\n";
    oss << "Hot: " << (is_hot() ? "yes" : "no") << "\n";
    oss << "Likely: " << (is_likely() ? "yes" : "no") << "\n";
    
    if (exception_info_) {
        oss << "Exception: " << exception_info_->description() << "\n";
    }
    
    return oss.str();
}

std::string ExitInfo::get_register_state_string() const {
    return register_state_.to_string();
}

std::string ExitInfo::get_statistics_string() const {
    std::ostringstream oss;
    oss << "Execution count: " << stats_.execution_count << "\n";
    oss << "Average cycles: " << std::fixed << std::setprecision(2) 
        << stats_.average_cycles << "\n";
    oss << "Prediction accuracy: " << (stats_.prediction_accuracy() * 100.0) 
        << "%\n";
    return oss.str();
}

bool ExitInfo::validate() const {
    // Validate exit information consistency
    if (target_pc_ == 0 && reason_ != ExitReason::NormalBlockEnd) {
        return false;
    }
    
    if (category_ == ExitCategory::Exception && !exception_info_) {
        return false;
    }
    
    return true;
}

bool ExitInfo::is_consistent_with(const ExitInfo& other) const {
    // Check if two exits are consistent
    if (target_pc_ != other.target_pc_) return false;
    if (reason_ != other.reason_) return false;
    if (category_ != other.category_) return false;
    
    return true;
}

void ExitInfo::update_flags() {
    // Update flags based on current state
    if (reason_ == ExitReason::ConditionalBranchTaken ||
        reason_ == ExitReason::ConditionalBranchNotTaken) {
        flags_ = flags_ | ExitFlags::DelaySlot;
    }
    
    if (static_cast<uint16_t>(reason_) >= 0x0100 && 
        static_cast<uint16_t>(reason_) < 0x0200) {
        flags_ = flags_ | ExitFlags::ExceptionPossible;
    }
}

void ExitInfo::update_probability() {
    // Calculate exit probability based on execution count
    // This would need access to parent block's total execution count
    if (stats_.execution_count > 0) {
        stats_.probability = static_cast<double>(stats_.execution_count) / 
                           (stats_.execution_count + 1.0);
    }
}

void ExitInfo::log_exit(const std::string& message) {
    // TRACE(TraceLevel::Info, "Exit {}: {}", static_cast<uint32_t>(id_), message);
    Log::debugf("Exit %u: %s", static_cast<uint32_t>(id_), message.c_str());
}

void ExitInfo::apply_peephole_optimization() {
    // Simple peephole optimizations for exit sequences
    // Would involve analyzing the exit code pattern
}

void ExitInfo::apply_profile_guided_optimization() {
    // Use profiling data to optimize
    if (stats_.is_hot()) {
        flags_ = flags_ | ExitFlags::ProfileGuided | ExitFlags::CriticalPath;
    }
    
    // Suggest inlining for very hot exits
    if (stats_.execution_count > 1000 && can_inline()) {
        flags_ = flags_ | ExitFlags::InlineCandidate;
    }
}

void ExitInfo::apply_speculative_optimization() {
    // Apply speculative optimizations
    if (prediction_.confidence > 0.9) {
        flags_ = flags_ | ExitFlags::Speculative | ExitFlags::Predicted;
    }
}

// ============================================================================
// ExitManager Implementation
// ============================================================================

ExitManager::ExitManager(CodeBlock& parent_block, size_t initial_capacity)
    : parent_block_(parent_block)
{
    exits_.reserve(initial_capacity);
}

ExitManager::~ExitManager() {
    clear();
}

ExitInfo& ExitManager::create_exit(uint32_t target_pc, ExitReason reason, 
                                    ExitCategory category) {
    std::unique_lock lock(mutex_);
    
    ExitId id = generate_id();
    auto exit = std::make_unique<ExitInfo>(parent_block_, id);
    exit->set_target(target_pc, reason, category);
    
    ExitInfo* exit_ptr = exit.get();
    exits_[id] = std::move(exit);
    pc_to_exit_id_[target_pc] = id;
    
    return *exit_ptr;
}

std::optional<ExitInfo*> ExitManager::find_exit(uint32_t target_pc) const {
    std::shared_lock lock(mutex_);
    
    auto it = pc_to_exit_id_.find(target_pc);
    if (it == pc_to_exit_id_.end()) {
        return std::nullopt;
    }
    
    auto exit_it = exits_.find(it->second);
    if (exit_it == exits_.end()) {
        return std::nullopt;
    }
    
    return exit_it->second.get();
}

std::optional<ExitInfo*> ExitManager::find_exit(ExitReason reason) const {
    std::shared_lock lock(mutex_);
    
    for (const auto& [id, exit] : exits_) {
        if (exit->reason() == reason) {
            return exit.get();
        }
    }
    
    return std::nullopt;
}

std::optional<ExitInfo*> ExitManager::find_exit(asmjit::Label label) const {
    std::shared_lock lock(mutex_);
    
    auto it = label_to_exit_id_.find(label);
    if (it == label_to_exit_id_.end()) {
        return std::nullopt;
    }
    
    auto exit_it = exits_.find(it->second);
    if (exit_it == exits_.end()) {
        return std::nullopt;
    }
    
    return exit_it->second.get();
}

void ExitManager::remove_exit(ExitId id) {
    std::unique_lock lock(mutex_);
    
    auto it = exits_.find(id);
    if (it != exits_.end()) {
        uint32_t pc = it->second->target_pc();
        pc_to_exit_id_.erase(pc);
        exits_.erase(it);
    }
}

void ExitManager::remove_exit(uint32_t target_pc) {
    std::unique_lock lock(mutex_);
    
    auto it = pc_to_exit_id_.find(target_pc);
    if (it != pc_to_exit_id_.end()) {
        exits_.erase(it->second);
        pc_to_exit_id_.erase(it);
    }
}

void ExitManager::clear() {
    std::unique_lock lock(mutex_);
    exits_.clear();
    pc_to_exit_id_.clear();
    label_to_exit_id_.clear();
}

void ExitManager::optimize_all_exits() {
    std::shared_lock lock(mutex_);
    
    for (auto& [id, exit] : exits_) {
        exit->optimize();
    }
}

void ExitManager::profile_all_exits() {
    std::shared_lock lock(mutex_);
    
    // Collect profiling data from all exits
    for (const auto& [id, exit] : exits_) {
        const auto& stats = exit->statistics();
        // Could aggregate or log this data
    }
}

void ExitManager::reset_all_statistics() {
    std::shared_lock lock(mutex_);
    
    for (auto& [id, exit] : exits_) {
        // Would need to add a reset method to ExitInfo
    }
}

ExitManager::ExitList ExitManager::get_exits_by_category(ExitCategory category) const {
    std::shared_lock lock(mutex_);
    
    ExitList result;
    for (const auto& [id, exit] : exits_) {
        if (exit->category() == category) {
            result.push_back(exit.get());
        }
    }
    return result;
}

ExitManager::ExitList ExitManager::get_exits_by_reason(ExitReason reason) const {
    std::shared_lock lock(mutex_);
    
    ExitList result;
    for (const auto& [id, exit] : exits_) {
        if (exit->reason() == reason) {
            result.push_back(exit.get());
        }
    }
    return result;
}

ExitManager::ExitList ExitManager::get_hot_exits() const {
    std::shared_lock lock(mutex_);
    
    ExitList result;
    for (const auto& [id, exit] : exits_) {
        if (exit->is_hot()) {
            result.push_back(exit.get());
        }
    }
    return result;
}

ExitManager::ExitList ExitManager::get_cold_exits() const {
    std::shared_lock lock(mutex_);
    
    ExitList result;
    for (const auto& [id, exit] : exits_) {
        if (exit->is_cold()) {
            result.push_back(exit.get());
        }
    }
    return result;
}

std::vector<uint32_t> ExitManager::get_all_targets() const {
    std::shared_lock lock(mutex_);
    
    std::vector<uint32_t> targets;
    targets.reserve(exits_.size());
    
    for (const auto& [id, exit] : exits_) {
        targets.push_back(exit->target_pc());
    }
    
    return targets;
}

std::vector<uint32_t> ExitManager::get_likely_targets() const {
    std::shared_lock lock(mutex_);
    
    std::vector<uint32_t> targets;
    for (const auto& [id, exit] : exits_) {
        if (exit->is_likely()) {
            targets.push_back(exit->target_pc());
        }
    }
    
    return targets;
}

std::vector<uint32_t> ExitManager::get_unlikely_targets() const {
    std::shared_lock lock(mutex_);
    
    std::vector<uint32_t> targets;
    for (const auto& [id, exit] : exits_) {
        if (exit->is_unlikely()) {
            targets.push_back(exit->target_pc());
        }
    }
    
    return targets;
}

ExitManager::ManagerStats ExitManager::statistics() const {
    std::shared_lock lock(mutex_);
    
    ManagerStats stats;
    stats.total_exits = exits_.size();
    
    for (const auto& [id, exit] : exits_) {
        if (exit->category() == ExitCategory::Normal) {
            ++stats.normal_exits;
        } else if (exit->category() == ExitCategory::Exception) {
            ++stats.exception_exits;
        }
        
        if (exit->is_hot()) ++stats.hot_exits;
        if (exit->is_cold()) ++stats.cold_exits;
        if (exit->is_likely()) ++stats.linked_exits;
        
        stats.total_executions += exit->statistics().execution_count;
    }
    
    if (stats.total_exits > 0) {
        stats.average_exits_per_block = 1.0; // Would need block count
    }
    
    return stats;
}

std::string ExitManager::ManagerStats::to_string() const {
    std::ostringstream oss;
    oss << "Exit Manager Statistics:\n";
    oss << "  Total exits: " << total_exits << "\n";
    oss << "  Normal: " << normal_exits << ", Exception: " << exception_exits << "\n";
    oss << "  Hot: " << hot_exits << ", Cold: " << cold_exits << "\n";
    oss << "  Linked: " << linked_exits << "\n";
    oss << "  Total executions: " << total_executions << "\n";
    return oss.str();
}

ExitManager::SerializedExitManager ExitManager::serialize() const {
    std::shared_lock lock(mutex_);
    
    SerializedExitManager data;
    data.exits.reserve(exits_.size());
    
    for (const auto& [id, exit] : exits_) {
        SerializedExitManager::SerializedExit se;
        se.target_pc = exit->target_pc();
        se.reason = exit->reason();
        se.category = exit->category();
        se.flags = exit->flags();
        se.stage = exit->pipeline_stage();
        se.stats = exit->statistics();
        se.regs = exit->get_register_state();
        
        data.exits.push_back(se);
    }
    
    return data;
}

bool ExitManager::deserialize(const SerializedExitManager& data) {
    std::unique_lock lock(mutex_);
    
    clear();
    
    for (const auto& se : data.exits) {
        ExitId id = generate_id();
        auto exit = std::make_unique<ExitInfo>(parent_block_, id);
        exit->set_target(se.target_pc, se.reason, se.category);
        exit->set_flags(se.flags);
        exit->set_pipeline_stage(se.stage);
        // Would need to restore statistics and register state
        
        exits_[id] = std::move(exit);
        pc_to_exit_id_[se.target_pc] = id;
    }
    
    return true;
}

ExitId ExitManager::generate_id() {
    static std::atomic<uint32_t> next_id{1};
    return static_cast<ExitId>(next_id.fetch_add(1));
}

void ExitManager::rebuild_indexes() {
    pc_to_exit_id_.clear();
    label_to_exit_id_.clear();
    
    for (const auto& [id, exit] : exits_) {
        pc_to_exit_id_[exit->target_pc()] = id;
        label_to_exit_id_[exit->jump_label()] = id;
    }
}

// ============================================================================
// ExitPredictor Implementation
// ============================================================================

ExitPredictor::ExitPredictor(size_t history_size)
    : max_history_size_(history_size)
{
}

ExitPredictor::PredictionResult ExitPredictor::predict(
    uint32_t current_pc, uint32_t branch_pc, ExitReason last_reason,
    const ExitRegisterState& state) 
{
    std::shared_lock lock(mutex_);
    
    PredictionResult result;
    result.confidence = 0.5;
    result.taken = false;
    result.target = 0;
    
    auto it = branch_histories_.find(branch_pc);
    if (it == branch_histories_.end()) {
        // No history - use static prediction
        return result;
    }
    
    const auto& history = it->second;
    
    // Use history to predict
    result.taken = history.taken_probability() > 0.5;
    result.confidence = std::abs(history.taken_probability() - 0.5) * 2.0;
    
    // Find most common target
    if (!history.target_history.empty()) {
        std::unordered_map<uint32_t, size_t> target_counts;
        for (uint32_t target : history.target_history) {
            ++target_counts[target];
        }
        
        auto max_it = std::max_element(
            target_counts.begin(), target_counts.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; }
        );
        
        if (max_it != target_counts.end()) {
            result.target = max_it->first;
        }
    }
    
    // Suggest flags
    if (result.confidence > 0.7) {
        result.suggested_flags = result.taken ? 
            ExitFlags::Likely : ExitFlags::Unlikely;
    }
    
    return result;
}

void ExitPredictor::train(uint32_t branch_pc, bool taken, uint32_t target, 
                          ExitReason reason) {
    std::unique_lock lock(mutex_);
    
    auto& history = branch_histories_[branch_pc];
    history.address = branch_pc;
    history.record(taken, target, reason);
}

double ExitPredictor::get_branch_probability(uint32_t branch_pc) const {
    std::shared_lock lock(mutex_);
    
    auto it = branch_histories_.find(branch_pc);
    if (it == branch_histories_.end()) {
        return 0.5; // Unknown - assume 50/50
    }
    
    return it->second.taken_probability();
}

std::vector<uint32_t> ExitPredictor::get_likely_targets(uint32_t branch_pc) const {
    std::shared_lock lock(mutex_);
    
    auto it = branch_histories_.find(branch_pc);
    if (it == branch_histories_.end()) {
        return {};
    }
    
    const auto& history = it->second;
    
    // Count target frequencies
    std::unordered_map<uint32_t, size_t> target_counts;
    for (uint32_t target : history.target_history) {
        ++target_counts[target];
    }
    
    // Sort by frequency
    std::vector<std::pair<uint32_t, size_t>> sorted_targets(
        target_counts.begin(), target_counts.end()
    );
    std::sort(sorted_targets.begin(), sorted_targets.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );
    
    // Return top targets
    std::vector<uint32_t> result;
    for (size_t i = 0; i < std::min(size_t(3), sorted_targets.size()); ++i) {
        result.push_back(sorted_targets[i].first);
    }
    
    return result;
}

bool ExitPredictor::is_branch_predictable(uint32_t branch_pc) const {
    std::shared_lock lock(mutex_);
    
    auto it = branch_histories_.find(branch_pc);
    if (it == branch_histories_.end()) {
        return false;
    }
    
    const auto& history = it->second;
    
    // Branch is predictable if probability is strongly biased
    double prob = history.taken_probability();
    return prob < 0.2 || prob > 0.8;
}

ExitFlags ExitPredictor::suggest_flags(uint32_t branch_pc, ExitReason reason) const {
    std::shared_lock lock(mutex_);
    
    ExitFlags flags = ExitFlags::None;
    
    auto it = branch_histories_.find(branch_pc);
    if (it == branch_histories_.end()) {
        return flags;
    }
    
    const auto& history = it->second;
    double prob = history.taken_probability();
    
    if (prob > 0.8) {
        flags = flags | ExitFlags::Likely | ExitFlags::Predicted;
    } else if (prob < 0.2) {
        flags = flags | ExitFlags::Unlikely | ExitFlags::Predicted;
    }
    
    if (history.total_executions > 1000) {
        flags = flags | ExitFlags::Hot | ExitFlags::ProfileGuided;
    }
    
    return flags;
}

bool ExitPredictor::suggest_inlining(uint32_t branch_pc, uint32_t target_pc) const {
    std::shared_lock lock(mutex_);
    
    auto it = branch_histories_.find(branch_pc);
    if (it == branch_histories_.end()) {
        return false;
    }
    
    const auto& history = it->second;
    
    // Suggest inlining for very hot, predictable branches
    return history.total_executions > 500 &&
           history.taken_probability() > 0.9;
}

ExitPredictor::PredictorStats ExitPredictor::statistics() const {
    std::shared_lock lock(mutex_);
    
    PredictorStats stats;
    stats.branches_tracked = branch_histories_.size();
    
    for (const auto& [pc, history] : branch_histories_) {
        // Would need to track prediction accuracy
        stats.branch_accuracies[pc] = history.taken_probability();
    }
    
    return stats;
}

void ExitPredictor::reset_statistics() {
    std::unique_lock lock(mutex_);
    branch_histories_.clear();
}

std::string ExitPredictor::PredictorStats::to_string() const {
    std::ostringstream oss;
    oss << "Exit Predictor Statistics:\n";
    oss << "  Predictions made: " << predictions_made << "\n";
    oss << "  Correct: " << correct_predictions << "\n";
    oss << "  Branches tracked: " << branches_tracked << "\n";
    oss << "  Overall accuracy: " << (overall_accuracy * 100.0) << "%\n";
    return oss.str();
}

bool ExitPredictor::matches_pattern(const BranchHistory& history, 
                                    const std::vector<bool>& pattern) const {
    if (history.taken_history.size() < pattern.size()) {
        return false;
    }
    
    size_t start = history.taken_history.size() - pattern.size();
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (history.taken_history[start + i] != pattern[i]) {
            return false;
        }
    }
    
    return true;
}

} // namespace Nova64::Recompiler

} // namespace Nova64
