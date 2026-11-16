/**
 * @file ExitInfo.hpp
 * @brief Exit information and control flow management for recompiled code
 * 
 * Nova64 N64 Emulator - Modern C++ reimplementation
 * Features: Type safety, performance, comprehensive exit tracking, and optimization
 */

#pragma once

#include "asmjit.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <variant>
#include <functional>
#include <atomic>
#include <span>
#include <mutex>
#include <shared_mutex>

class CRegisters;
class CN64System;

namespace Nova64 {

class RegisterState;
class MipsMemory;
using ::CN64System;

namespace Recompiler {

class CodeBlock;
class RecompilerOperations;

// Strongly typed identifiers
enum class ExitId : uint32_t {};
// BlockId is defined in CodeSection.hpp.
enum class BlockId : uint32_t;

// Pipeline stages for precise exception handling
enum class PipelineStage : uint8_t { Fetch, Decode, Execute, Memory, WriteBack, Exception, Complete };

// Exit categories for optimization
enum class ExitCategory : uint8_t {
    Normal,        // Normal block exit (jump/branch)
    Exception,     // Exception occurred
    Syscall,       // System call
    Interrupt,     // Interrupt handling
    SelfModifying, // Self-modifying code detected
    Profile,       // Profile-guided optimization trigger
    Optimization,  // Optimization boundary
    Debug,         // Debugger breakpoint
    Resource       // Resource limit (cache, memory, etc.)
};

// Exit reason with detailed classification
enum class ExitReason : uint16_t {
    // Normal exits
    NormalBlockEnd = 0x0001,
    ConditionalBranchTaken = 0x0002,
    ConditionalBranchNotTaken = 0x0003,
    UnconditionalJump = 0x0004,
    JumpRegister = 0x0005,
    JumpAndLink = 0x0006,

    // Exception exits
    TLBReadMiss = 0x0101,
    TLBWriteMiss = 0x0102,
    AddressErrorRead = 0x0103,
    AddressErrorWrite = 0x0104,
    BusErrorInstruction = 0x0105,
    BusErrorData = 0x0106,
    Syscall = 0x0107,
    Breakpoint = 0x0108,
    IllegalInstruction = 0x0109,
    CoprocessorUnusable = 0x010A,
    ArithmeticOverflow = 0x010B,
    TrapException = 0x010C,
    FloatingPointException = 0x010D,
    Watchpoint = 0x010E,

    // System exits
    DoCPUAction = 0x0201,
    ResetRecompCode = 0x0202,
    InterruptPending = 0x0203,
    CacheOperation = 0x0204,

    // Optimization exits
    ProfileThreshold = 0x0301,
    HotPathDetected = 0x0302,
    ColdPathDetected = 0x0303,
    InliningOpportunity = 0x0304,

    // Debug exits
    BreakpointHit = 0x0401,
    WatchpointHit = 0x0402,
    SingleStep = 0x0403,
    MemoryAccessLog = 0x0404,

    // Resource exits
    CacheFull = 0x0501,
    MemoryLimit = 0x0502,
    Timeout = 0x0503,
    StackOverflow = 0x0504
};

// Exit flags for optimization and analysis
enum class ExitFlags : uint32_t {
    None = 0,
    Likely = 1 << 0,            // Branch is likely taken
    Unlikely = 1 << 1,          // Branch is unlikely taken
    DelaySlot = 1 << 2,         // Exit has delay slot
    ExceptionPossible = 1 << 3, // Exception can occur here
    ProfileGuided = 1 << 4,     // Exit guided by profiling
    Optimized = 1 << 5,         // Exit has been optimized
    InlineCandidate = 1 << 6,   // Good candidate for inlining
    CriticalPath = 1 << 7,      // On critical execution path
    Hot = 1 << 8,               // Frequently taken exit
    Cold = 1 << 9,              // Rarely taken exit
    Predicted = 1 << 10,        // Branch prediction applied
    Mispredicted = 1 << 11,     // Branch was mispredicted
    Linked = 1 << 12,           // Exit links directly to target
    Patched = 1 << 13,          // Exit has been patched
    ProfilingEnabled = 1 << 14, // Profiling enabled for this exit
    DebugEnabled = 1 << 15,     // Debugging enabled for this exit
    SelfModifying = 1 << 16,    // Self-modifying code exit
    Atomic = 1 << 17,           // Atomic operation exit
    Vectorized = 1 << 18,       // Vector operation exit
    Parallel = 1 << 19,         // Parallel execution exit
    Speculative = 1 << 20,      // Speculative execution exit
    RollbackSafe = 1 << 21,     // Safe to rollback from this exit
    Checkpoint = 1 << 22        // Creates a checkpoint
};

} // namespace Recompiler
} // namespace Nova64

// Hash specializations
template <> struct std::hash<Nova64::Recompiler::ExitId> {
    size_t operator()(Nova64::Recompiler::ExitId id) const noexcept { return static_cast<size_t>(id); }
};

template <> struct std::hash<Nova64::Recompiler::ExitReason> {
    size_t operator()(Nova64::Recompiler::ExitReason reason) const noexcept { return static_cast<size_t>(reason); }
};

namespace Nova64 {
namespace Recompiler {

inline ExitFlags operator|(ExitFlags a, ExitFlags b) {
    return static_cast<ExitFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ExitFlags operator&(ExitFlags a, ExitFlags b) {
    return static_cast<ExitFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool has_flag(ExitFlags value, ExitFlags flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// Exit statistics for profiling and optimization
struct ExitStatistics {
    uint64_t execution_count = 0; // Times this exit was taken
    uint64_t cycles_to_exit = 0;  // Total cycles spent before exit
    uint64_t last_execution = 0;  // Timestamp of last execution
    double average_cycles = 0.0;  // Average cycles before exit
    double probability = 0.0;     // Probability of taking this exit
    uint64_t mispredictions = 0;  // Branch mispredictions
    uint64_t prediction_hits = 0; // Correct predictions

    void record_execution(uint64_t cycles) {
        ++execution_count;
        cycles_to_exit += cycles;
        average_cycles = static_cast<double>(cycles_to_exit) / execution_count;
        last_execution = get_timestamp();
    }

    void record_prediction(bool correct) {
        if (correct) {
            ++prediction_hits;
        } else {
            ++mispredictions;
        }
    }

    double prediction_accuracy() const {
        uint64_t total = prediction_hits + mispredictions;
        return total > 0 ? static_cast<double>(prediction_hits) / total : 1.0;
    }

    bool is_hot() const { return execution_count > 1000; }
    bool is_cold() const { return execution_count < 10; }

private:
    static uint64_t get_timestamp() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
};

// Register state at exit point
struct ExitRegisterState {
    struct RegisterSnapshot {
        bool valid = false;
        bool dirty = false;
        bool constant = false;
        uint32_t value = 0;
        uint32_t physical_reg = 0xFFFFFFFF;

        bool operator==(const RegisterSnapshot& other) const {
            return valid == other.valid && constant == other.constant && (!constant || value == other.value);
        }
    };

    std::array<RegisterSnapshot, 32> gpr; // General Purpose Registers
    std::array<RegisterSnapshot, 32> fpr; // Floating Point Registers
    RegisterSnapshot hi;                  // HI register
    RegisterSnapshot lo;                  // LO register
    RegisterSnapshot pc;                  // Program Counter

    // Comparison and merging
    bool operator==(const ExitRegisterState& other) const;
    void reset();

    // String representation for debugging
    std::string to_string() const;
};

// Exception information for precise exception handling
struct ExceptionInfo {
    ExitReason reason;
    uint32_t virtual_address = 0;
    uint32_t extra_info = 0;
    PipelineStage stage;
    uint64_t timestamp = 0;
};

// Branch prediction information
struct BranchPrediction {
    bool predicted_taken = false;
    double confidence = 0.0;
    uint32_t predicted_target = 0;
    uint64_t training_count = 0;

    double get_confidence() const { return confidence; }
    bool should_predict() const { return confidence > 0.7; }
    double prediction_accuracy() const { return confidence; }
};

/**
 * @brief Comprehensive exit information for recompiled code blocks
 * 
 * Tracks all information needed to handle exits from recompiled code,
 * including control flow, exceptions, profiling, and optimization data.
 */
class ExitInfo {
public:
    // Configuration for exit tracking
    struct Config {
        Config() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        bool enable_profiling = true;
        bool enable_branch_prediction = true;
        bool enable_exception_tracking = true;
        bool enable_register_state = true;
        size_t max_exits_per_block = 32;
        size_t prediction_history_size = 1024;
    };

    ExitInfo(CodeBlock& parent_block, ExitId id, const Config& config = Config());
    ~ExitInfo();

    ExitInfo(const ExitInfo&) = delete;
    ExitInfo& operator=(const ExitInfo&) = delete;
    ExitInfo(ExitInfo&&) noexcept;
    ExitInfo& operator=(ExitInfo&&) noexcept;

    void set_label(asmjit::Label label) { jump_label_ = label; }
    void set_flags(ExitFlags flags) { flags_ = flags; }

    void set_pipeline_stage(PipelineStage stage) { pipeline_stage_ = stage; }

    const ExitRegisterState& get_register_state() const { return register_state_; }

    BranchPrediction get_prediction() const { return prediction_; }

    const ExitStatistics& statistics() const { return stats_; }

    ExitId id() const { return id_; }
    uint32_t target_pc() const { return target_pc_; }
    ExitReason reason() const { return reason_; }
    ExitCategory category() const { return category_; }
    ExitFlags flags() const { return flags_; }
    PipelineStage pipeline_stage() const { return pipeline_stage_; }
    asmjit::Label jump_label() const { return jump_label_; }
    CodeBlock& parent_block() { return parent_block_; }
    const CodeBlock& parent_block() const { return parent_block_; }

    bool is_likely() const { return has_flag(flags_, ExitFlags::Likely); }
    bool is_unlikely() const { return has_flag(flags_, ExitFlags::Unlikely); }
    bool has_delay_slot() const { return has_flag(flags_, ExitFlags::DelaySlot); }
    bool is_hot() const { return stats_.is_hot(); }
    bool is_cold() const { return stats_.is_cold(); }
    bool is_exception() const { return static_cast<uint16_t>(reason_) >= 0x0100; }

private:
    void update_probability();

private:
    // Parent relationship
    CodeBlock& parent_block_;
    ExitId id_;

    // Exit properties
    uint32_t target_pc_ = 0;
    ExitReason reason_ = ExitReason::NormalBlockEnd;
    ExitCategory category_ = ExitCategory::Normal;
    ExitFlags flags_ = ExitFlags::None;
    PipelineStage pipeline_stage_ = PipelineStage::Complete;

    // Exception information (if applicable)
    std::optional<ExceptionInfo> exception_info_;

    // Register state at exit
    ExitRegisterState register_state_;

    // Branch prediction
    BranchPrediction prediction_;

    ExitStatistics stats_;

    // AsmJit label for code generation
    asmjit::Label jump_label_;

    Config config_;

    // Cache for frequently accessed data
    static constexpr size_t CACHE_LINE_SIZE = 64;
    alignas(CACHE_LINE_SIZE) mutable uint64_t last_access_ = 0;

    // Debug information
    std::string debug_name_;
    std::vector<uint32_t> execution_history_;
};

/**
 * @brief Manages all exits for a code block
 */
class ExitManager {
public:
    using ExitMap = std::unordered_map<ExitId, std::unique_ptr<ExitInfo>>;
    using ExitList = std::vector<ExitInfo*>;

    explicit ExitManager(CodeBlock& parent_block, size_t initial_capacity = 16);
    ~ExitManager();

    void clear();

    // Statistics
    struct ManagerStats {
        size_t total_exits = 0;
        size_t normal_exits = 0;
        size_t exception_exits = 0;
        size_t hot_exits = 0;
        size_t cold_exits = 0;
        size_t linked_exits = 0;
        uint64_t total_executions = 0;
        double average_exits_per_block = 0.0;

        void reset() { *this = ManagerStats(); }
        std::string to_string() const;
    };

    // Serialization for save states
    struct SerializedExitManager {
        struct SerializedExit {
            uint32_t target_pc;
            ExitReason reason;
            ExitCategory category;
            ExitFlags flags;
            PipelineStage stage;
            ExitStatistics stats;
            ExitRegisterState regs;
        };

        std::vector<SerializedExit> exits;
    };

    size_t count() const { return exits_.size(); }
    bool empty() const { return exits_.empty(); }

    // Iteration support
    auto begin() { return exits_.begin(); }
    auto end() { return exits_.end(); }
    auto begin() const { return exits_.begin(); }
    auto end() const { return exits_.end(); }

private:
    CodeBlock& parent_block_;
    ExitMap exits_;
    std::unordered_map<uint32_t, ExitId> pc_to_exit_id_;
    std::unordered_map<asmjit::Label, ExitId, AsmJitLabelHash> label_to_exit_id_;

    mutable std::shared_mutex mutex_;
};

/**
 * @brief Predictor for exit behavior and optimization
 */
class ExitPredictor {
public:
    struct PredictionResult {
        bool taken;
        uint32_t target;
        double confidence;
        ExitFlags suggested_flags;
        std::vector<ExitReason> possible_reasons;
    };

    ExitPredictor(size_t history_size = 4096);

    // Statistics
    struct PredictorStats {
        size_t predictions_made = 0;
        size_t correct_predictions = 0;
        size_t branches_tracked = 0;
        double overall_accuracy = 0.0;
        std::unordered_map<uint32_t, double> branch_accuracies;

        void reset() { *this = PredictorStats(); }
        std::string to_string() const;
    };

private:
    struct BranchHistory {
        uint32_t address;
        std::vector<bool> taken_history;
        std::vector<uint32_t> target_history;
        std::vector<ExitReason> reason_history;
        uint64_t total_executions = 0;
        uint64_t taken_count = 0;

        double taken_probability() const {
            return total_executions > 0 ? static_cast<double>(taken_count) / total_executions : 0.5;
        }

        static constexpr size_t max_history = 64;
    };

    std::unordered_map<uint32_t, BranchHistory> branch_histories_;
    size_t max_history_size_;

    mutable std::shared_mutex mutex_;
};

} // namespace Recompiler
} // namespace Nova64

// Formatter for ExitReason (enable if using fmt library)
#if defined(FMT_VERSION)
namespace fmt {
template <> struct formatter<Nova64::Recompiler::ExitReason> : formatter<int> {
    template <typename FormatContext> auto format(Nova64::Recompiler::ExitReason reason, FormatContext& ctx) {
        return formatter<int>::format(static_cast<int>(reason), ctx);
    }
};
} // namespace fmt
#endif
