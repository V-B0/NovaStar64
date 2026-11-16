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

// --- ExitRegisterState Implementation ---

bool ExitRegisterState::operator==(const ExitRegisterState& other) const {
    for (size_t i = 0; i < 32; ++i) {
        if (!(gpr[i] == other.gpr[i])) return false;
        if (!(fpr[i] == other.fpr[i])) return false;
    }
    return hi == other.hi && lo == other.lo && pc == other.pc;
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

    for (size_t i = 0; i < 32; ++i) {
        if (gpr[i].valid) {
            oss << "  r" << i << ": ";
            if (gpr[i].constant) {
                oss << "const=0x" << std::hex << std::setw(8) << std::setfill('0') << gpr[i].value;
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

    if (hi.valid) oss << "  HI: " << (hi.constant ? "const" : "valid") << "\n";
    if (lo.valid) oss << "  LO: " << (lo.constant ? "const" : "valid") << "\n";
    if (pc.valid) oss << "  PC: 0x" << std::hex << pc.value << "\n";

    return oss.str();
}

// --- ExceptionInfo Implementation ---

std::string ExceptionInfo::description() const {
    std::ostringstream oss;
    oss << "Exception at stage " << static_cast<int>(stage);
    oss << ", reason: " << static_cast<int>(reason);
    if (virtual_address != 0) {
        oss << ", vaddr: 0x" << std::hex << virtual_address;
    }
    return oss.str();
}

// --- BranchPrediction Implementation ---

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

    if (actually_taken && actual_target != 0) {
        predicted_target = actual_target;
    }
}

// --- ExitInfo Implementation ---

ExitInfo::ExitInfo(CodeBlock& parent_block, ExitId id, const Config& config) :
    parent_block_(parent_block), id_(id), config_(config) {
    register_state_.reset();

    if (config_.enable_profiling) {
        execution_history_.reserve(256);
    }
}

ExitInfo::~ExitInfo() {}

ExitInfo::ExitInfo(ExitInfo&& other) noexcept :
    parent_block_(other.parent_block_), id_(other.id_), target_pc_(other.target_pc_), reason_(other.reason_),
    category_(other.category_), flags_(other.flags_), pipeline_stage_(other.pipeline_stage_),
    exception_info_(std::move(other.exception_info_)), register_state_(std::move(other.register_state_)),
    prediction_(std::move(other.prediction_)), stats_(std::move(other.stats_)), jump_label_(other.jump_label_),
    config_(other.config_), debug_name_(std::move(other.debug_name_)),
    execution_history_(std::move(other.execution_history_)) {}

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

void ExitInfo::record_execution(uint64_t cycles) {
    stats_.record_execution(cycles);

    update_probability();

    // Track execution history for profiling
    if (config_.enable_profiling && execution_history_.size() < 1024) {
        execution_history_.push_back(target_pc_);
    }

    if (stats_.is_hot()) {
        flags_ = flags_ | ExitFlags::Hot;
    } else if (stats_.is_cold()) {
        flags_ = flags_ | ExitFlags::Cold;
    }
}

bool ExitInfo::can_inline() const {
    if (!stats_.is_hot()) return false;
    if (has_flag(flags_, ExitFlags::ExceptionPossible)) return false;
    if (category_ == ExitCategory::Exception) return false;

    return true;
}

bool ExitInfo::validate() const {
    if (target_pc_ == 0 && reason_ != ExitReason::NormalBlockEnd) {
        return false;
    }

    if (category_ == ExitCategory::Exception && !exception_info_) {
        return false;
    }

    return true;
}

void ExitInfo::update_flags() {
    if (reason_ == ExitReason::ConditionalBranchTaken || reason_ == ExitReason::ConditionalBranchNotTaken) {
        flags_ = flags_ | ExitFlags::DelaySlot;
    }

    if (static_cast<uint16_t>(reason_) >= 0x0100 && static_cast<uint16_t>(reason_) < 0x0200) {
        flags_ = flags_ | ExitFlags::ExceptionPossible;
    }
}

void ExitInfo::update_probability() {
    // Approximation: the parent block's total execution count is not available.
    if (stats_.execution_count > 0) {
        stats_.probability = static_cast<double>(stats_.execution_count) / (stats_.execution_count + 1.0);
    }
}

// --- ExitManager Implementation ---

ExitManager::ExitManager(CodeBlock& parent_block, size_t initial_capacity) : parent_block_(parent_block) {
    exits_.reserve(initial_capacity);
}

ExitManager::~ExitManager() { clear(); }

void ExitManager::clear() {
    std::unique_lock lock(mutex_);
    exits_.clear();
    pc_to_exit_id_.clear();
    label_to_exit_id_.clear();
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
        stats.average_exits_per_block = 1.0; // Block count is not available here
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

ExitId ExitManager::generate_id() {
    static std::atomic<uint32_t> next_id{1};
    return static_cast<ExitId>(next_id.fetch_add(1));
}

// --- ExitPredictor Implementation ---

ExitPredictor::ExitPredictor(size_t history_size) : max_history_size_(history_size) {}

ExitPredictor::PredictorStats ExitPredictor::statistics() const {
    std::shared_lock lock(mutex_);

    PredictorStats stats;
    stats.branches_tracked = branch_histories_.size();

    for (const auto& [pc, history] : branch_histories_) {
        // Taken probability stands in for accuracy, which is not tracked.
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

} // namespace Recompiler

} // namespace Nova64
