/**
 * @file FunctionInfo.hpp
 * @brief Compiled function information and management system
 * 
 * Nova64 N64 Emulator - Modern C++ reimplementation
 * Features: Type safety, performance, advanced optimization, and comprehensive tracking
 */

#pragma once

#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <functional>
#include <atomic>
#include <span>
#include <string>
#include <array>
#include <mutex>
#include <shared_mutex>

// Forward declarations
namespace Nova64 {
    namespace Recompiler {
        class CodeBlock;
        class Recompiler;
    }
}

namespace Nova64 {
    
// Forward declarations
class CpuRegisters;
class MipsMemory;
class N64System;

namespace Recompiler {

// Forward declarations
class CodeBlockCache;
class ProfileData;

// Strongly typed identifiers
enum class FunctionId : uint32_t {};
enum class BlockId : uint32_t {};

// Function compilation status
enum class CompilationStatus : uint8_t {
    NotCompiled,
    Compiling,
    Compiled,
    Optimized,
    Invalidated,
    SelfModified,
    Exception,
    Inlined,
    Specialized
};

// Function optimization levels
enum class OptimizationLevel : uint8_t {
    None,           // No optimization, direct translation
    Basic,          // Basic optimizations (dead code elimination, constant folding)
    Aggressive,     // Aggressive optimizations (inlining, loop unrolling)
    ProfileGuided,  // Profile-guided optimization
    Max             // Maximum optimization (all techniques)
};

// Function execution flags
enum class FunctionFlags : uint32_t {
    None                = 0,
    Hot                 = 1 << 0,   // Frequently executed function
    Cold                = 1 << 1,   // Rarely executed function
    HasLoops            = 1 << 2,   // Contains loops
    HasExceptions       = 1 << 3,   // Can throw exceptions
    HasFloatOps         = 1 << 4,   // Contains floating point operations
    HasMemoryOps        = 1 << 5,   // Contains memory operations
    HasSelfModifying    = 1 << 6,   // Contains self-modifying code
    InlineCandidate     = 1 << 7,   // Good candidate for inlining
    Vectorizable        = 1 << 8,   // Contains vectorizable operations
    Parallelizable      = 1 << 9,   // Can be parallelized
    Speculative         = 1 << 10,  // Uses speculative execution
    ProfileGuided       = 1 << 11,  // Optimized with profile data
    DebugEnabled        = 1 << 12,  // Debugging enabled
    Checkpointed        = 1 << 13,  // Has checkpoints for rollback
    Atomic              = 1 << 14,  // Contains atomic operations
    Critical            = 1 << 15,  // Critical execution path
    Linked              = 1 << 16,  // Has been linked to other functions
    Specialized         = 1 << 17   // Has been specialized for specific inputs
};

inline FunctionFlags operator|(FunctionFlags a, FunctionFlags b) {
    return static_cast<FunctionFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline FunctionFlags operator&(FunctionFlags a, FunctionFlags b) {
    return static_cast<FunctionFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool has_flag(FunctionFlags value, FunctionFlags flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// Function signature for compiled code
using CompiledFunction = void(*)(CpuRegisters&, MipsMemory&, void*);

// Function metadata for optimization and analysis
struct FunctionMetadata {
    uint32_t entry_pc;
    uint32_t min_pc;
    uint32_t max_pc;
    uint64_t hash;  // Hash of function code for identification
    size_t instruction_count = 0;
    size_t basic_block_count = 0;
    size_t native_code_size = 0;
    OptimizationLevel optimization_level = OptimizationLevel::None;
    std::vector<uint32_t> call_sites;  // PCs that call this function
    std::vector<uint32_t> called_functions;  // Functions this calls
    
    bool contains(uint32_t pc) const noexcept {
        return pc >= min_pc && pc <= max_pc;
    }
    
    bool overlaps(const FunctionMetadata& other) const noexcept {
        return (min_pc <= other.max_pc) && (max_pc >= other.min_pc);
    }
    
    size_t size() const noexcept {
        return (max_pc - min_pc) / 4;  // MIPS instructions are 4 bytes
    }
};

// Function statistics for profiling
struct FunctionStatistics {
    uint64_t execution_count = 0;
    uint64_t total_cycles = 0;
    uint64_t total_instructions = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t recompilation_count = 0;
    uint64_t exception_count = 0;
    double average_cycles = 0.0;
    double average_instructions = 0.0;
    uint64_t last_execution = 0;
    
    void record_execution(uint64_t cycles, uint64_t instructions) {
        ++execution_count;
        total_cycles += cycles;
        total_instructions += instructions;
        average_cycles = static_cast<double>(total_cycles) / execution_count;
        average_instructions = static_cast<double>(total_instructions) / execution_count;
        last_execution = get_timestamp();
    }
    
    void record_cache_hit(bool hit) {
        if (hit) ++cache_hits;
        else ++cache_misses;
    }
    
    void record_exception() {
        ++exception_count;
    }
    
    void record_recompilation() {
        ++recompilation_count;
    }
    
    double cache_hit_ratio() const {
        uint64_t total = cache_hits + cache_misses;
        return total > 0 ? static_cast<double>(cache_hits) / total : 1.0;
    }
    
    bool is_hot() const { return execution_count > 1000; }
    bool is_cold() const { return execution_count < 10; }
    
private:
    static uint64_t get_timestamp() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
};

// Memory snapshot for self-modifying code detection
struct MemorySnapshot {
    struct MemoryEntry {
        uint64_t value;
        uint32_t address;
        uintptr_t location;
        bool valid = false;
        
        bool operator==(const MemoryEntry& other) const {
            return valid == other.valid && 
                   (!valid || value == other.value);
        }
    };
    
    std::array<MemoryEntry, 4> entries;  // Track up to 4 memory locations
    size_t count = 0;
    
    MemorySnapshot() = default;
    
    void add(uint32_t addr, uint64_t val, uintptr_t loc) {
        if (count < entries.size()) {
            entries[count] = {val, addr, loc, true};
            ++count;
        }
    }
    
    bool matches(const MemorySnapshot& other) const {
        if (count != other.count) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!(entries[i] == other.entries[i])) {
                return false;
            }
        }
        return true;
    }
    
    bool empty() const { return count == 0; }
};

// Inlining information
struct InliningInfo {
    struct InlinedFunction {
        FunctionId function_id;
        uint32_t call_site;
        uint32_t inline_start;
        uint32_t inline_end;
        size_t savings;  // Estimated cycles saved by inlining
    };
    
    std::vector<InlinedFunction> inlined_functions;
    size_t total_inlined_size = 0;
    size_t total_savings = 0;
    
    void add_inlined(FunctionId id, uint32_t site, uint32_t start, uint32_t end, size_t savings) {
        inlined_functions.push_back({id, site, start, end, savings});
        total_inlined_size += (end - start);
        total_savings += savings;
    }
    
    bool contains(uint32_t pc) const {
        for (const auto& inline_info : inlined_functions) {
            if (pc >= inline_info.inline_start && pc <= inline_info.inline_end) {
                return true;
            }
        }
        return false;
    }
};

// Specialization context (for function specialization based on inputs)
struct SpecializationContext {
    struct RegisterValue {
        uint32_t reg;
        uint32_t value;
        bool constant;
    };
    
    std::vector<RegisterValue> register_values;
    uint32_t memory_pattern_hash = 0;
    uint32_t branch_history_hash = 0;
    
    bool operator==(const SpecializationContext& other) const {
        return register_values == other.register_values &&
               memory_pattern_hash == other.memory_pattern_hash &&
               branch_history_hash == other.branch_history_hash;
    }
    
    struct Hasher {
        size_t operator()(const SpecializationContext& ctx) const {
            size_t hash = 0;
            for (const auto& rv : ctx.register_values) {
                hash ^= std::hash<uint32_t>{}(rv.reg);
                hash ^= std::hash<uint32_t>{}(rv.value);
                hash ^= std::hash<bool>{}(rv.constant);
            }
            hash ^= std::hash<uint32_t>{}(ctx.memory_pattern_hash);
            hash ^= std::hash<uint32_t>{}(ctx.branch_history_hash);
            return hash;
        }
    };
};

/**
 * @brief Represents a compiled function with comprehensive metadata
 * 
 * Manages all information about a function that has been dynamically
 * recompiled from MIPS to native code, including optimization data,
 * profiling information, and self-modifying code detection.
 */
class CompiledFunction {
public:
    // Configuration
    struct Config {
        bool enable_profiling = true;
        bool enable_inlining = true;
        bool enable_specialization = true;
        bool enable_checkpointing = false;
        size_t max_inline_size = 1024;  // Max bytes for inlining
        size_t max_specializations = 8;  // Max specialized versions
    };
    
    // Construction/Destruction
    CompiledFunction(Recompiler& recompiler, 
                    uint32_t entry_pc,
                    const Config& config = {});
    
    ~CompiledFunction();
    
    // Non-copyable, movable
    CompiledFunction(const CompiledFunction&) = delete;
    CompiledFunction& operator=(const CompiledFunction&) = delete;
    CompiledFunction(CompiledFunction&&) noexcept;
    CompiledFunction& operator=(CompiledFunction&&) noexcept;
    
    // Compilation
    enum class CompileResult {
        Success,
        FailedAnalysis,
        FailedCodeGen,
        OutOfMemory,
        InvalidFunction,
        TooLarge,
        SelfModifying
    };
    
    struct CompileOptions {
        OptimizationLevel optimization = OptimizationLevel::Aggressive;
        bool inline_functions = true;
        bool specialize = false;
        SpecializationContext specialization_context;
        bool generate_debug_info = false;
    };
    
    CompileResult compile(const CompileOptions& options = {});
    bool recompile(const CompileOptions& options = {});
    
    // Execution
    void execute(CpuRegisters& registers, MipsMemory& memory, void* context = nullptr);
    bool execute_safe(CpuRegisters& registers, MipsMemory& memory, void* context = nullptr);
    
    // Optimization
    void optimize();
    bool inline_function(FunctionId function_id, uint32_t call_site);
    bool specialize(const SpecializationContext& context);
    bool apply_profile_guided_optimization(const ProfileData& profile);
    
    // Validation and verification
    bool verify() const;
    bool verify_memory_snapshot() const;
    bool is_valid() const { return status_ == CompilationStatus::Compiled || 
                                  status_ == CompilationStatus::Optimized; }
    
    // Memory management for self-modifying code
    void update_memory_snapshot();
    void invalidate_if_modified();
    
    // Statistics and profiling
    void record_execution(uint64_t cycles, uint64_t instructions);
    const FunctionStatistics& statistics() const { return stats_; }
    void reset_statistics() { stats_ = FunctionStatistics(); }
    
    // Metadata access
    const FunctionMetadata& metadata() const { return metadata_; }
    FunctionId id() const { return id_; }
    uint32_t entry_pc() const { return metadata_.entry_pc; }
    uint32_t min_pc() const { return metadata_.min_pc; }
    uint32_t max_pc() const { return metadata_.max_pc; }
    FunctionFlags flags() const { return flags_; }
    CompilationStatus status() const { return status_; }
    CompiledFunction* next() const { return next_.get(); }
    
    // Function pointer access
    CompiledFunction* function_pointer() const { return function_ptr_; }
    void* code_pointer() const { return code_ptr_; }
    size_t code_size() const { return code_size_; }
    
    // Debug information
    std::string get_disassembly() const;
    std::string get_metadata_string() const;
    std::string get_statistics_string() const;
    std::string get_inlining_info_string() const;
    
    // Linking (for function chaining)
    void link_to(CompiledFunction* target, uint32_t branch_pc);
    void unlink_from(uint32_t branch_pc);
    
    // Serialization for save states
    struct SerializedFunction {
        FunctionId id;
        FunctionMetadata metadata;
        FunctionStatistics stats;
        MemorySnapshot snapshot;
        FunctionFlags flags;
        CompilationStatus status;
        std::vector<uint8_t> native_code;
        std::vector<uint32_t> linked_functions;
    };
    
    SerializedFunction serialize() const;
    bool deserialize(const SerializedFunction& data);
    
private:
    // Internal compilation helpers
    struct CompilationContext {
        const CompileOptions& options;
        std::vector<std::unique_ptr<CodeBlock>> blocks;
        std::vector<uint32_t> basic_blocks;
        std::unordered_set<uint32_t> visited_addresses;
        size_t total_instructions = 0;
        
        explicit CompilationContext(const CompileOptions& opts) : options(opts) {}
    };
    
    std::optional<CompilationContext> analyze_function();
    bool generate_native_code(const CompilationContext& context);
    bool optimize_native_code(const CompilationContext& context);
    
    // Inlining helpers
    bool can_inline_function(FunctionId function_id, uint32_t call_site) const;
    size_t estimate_inline_savings(FunctionId function_id) const;
    
    // Specialization helpers
    bool should_specialize(const SpecializationContext& context) const;
    SpecializationContext extract_context() const;
    
    // Memory snapshot helpers
    void capture_memory_snapshot();
    
    // Helper methods
    void update_flags();
    void update_hash();
    void log_compilation(const std::string& message);
    void set_status(CompilationStatus status);
    
private:
    // Core identification
    Recompiler& recompiler_;
    FunctionId id_;
    FunctionMetadata metadata_;
    uint64_t hash_;  // Hash of original MIPS code
    
    // Compilation state
    CompilationStatus status_ = CompilationStatus::NotCompiled;
    FunctionFlags flags_ = FunctionFlags::None;
    OptimizationLevel optimization_level_ = OptimizationLevel::None;
    
    // Native code
    CompiledFunction* function_ptr_ = nullptr;
    void* code_ptr_ = nullptr;
    size_t code_size_ = 0;
    
    // Memory management
    MemorySnapshot memory_snapshot_;
    
    // Optimization data
    InliningInfo inlining_info_;
    std::unordered_map<SpecializationContext, std::unique_ptr<CompiledFunction>, 
                      SpecializationContext::Hasher> specializations_;
    
    // Statistics
    FunctionStatistics stats_;
    
    // Configuration
    Config config_;
    
    // Linked list for hash table (optional)
    std::unique_ptr<CompiledFunction> next_;
    
    // Thread safety
    mutable std::shared_mutex mutex_;
    
    // Debug information
    std::string debug_log_;
    std::vector<uint8_t> native_code_cache_;
    
    // Cache line padding for performance
    static constexpr size_t CACHE_LINE_SIZE = 64;
    alignas(CACHE_LINE_SIZE) mutable uint64_t last_access_ = 0;
};

/**
 * @brief Manages compiled functions with efficient lookup and caching
 */
class FunctionManager {
public:
    struct Config {
        size_t max_functions = 4096;
        size_t max_total_size = 128 * 1024 * 1024;  // 128MB
        float hot_function_threshold = 0.1f;  // 10% can be hot
        bool enable_function_fusion = true;
        bool enable_cross_module_optimization = true;
        bool enable_lazy_compilation = true;
    };
    
    FunctionManager(Recompiler& recompiler, const Config& config = {});
    ~FunctionManager();
    
    // Function management
    CompiledFunction* get_or_compile(uint32_t entry_pc, 
                                    const CompiledFunction::CompileOptions& options = {});
    CompiledFunction* get(uint32_t entry_pc) const;
    CompiledFunction* get_by_id(FunctionId id) const;
    
    void insert(std::unique_ptr<CompiledFunction> function);
    void remove(FunctionId id);
    void remove(uint32_t entry_pc);
    void invalidate(uint32_t address);
    void invalidate_range(uint32_t start, uint32_t end);
    void clear();
    
    // Bulk operations
    void optimize_all();
    void profile_all();
    void invalidate_modified_functions();
    
    // Function analysis
    std::vector<FunctionId> find_functions_containing(uint32_t pc) const;
    std::vector<FunctionId> find_caller_functions(FunctionId callee) const;
    std::vector<FunctionId> find_callee_functions(FunctionId caller) const;
    
    // Hot/cold function management
    void promote_to_hot(FunctionId id);
    void demote_from_hot(FunctionId id);
    std::vector<FunctionId> get_hot_functions() const;
    std::vector<FunctionId> get_cold_functions() const;
    
    // Function fusion (combining small functions)
    bool fuse_functions(FunctionId f1, FunctionId f2);
    
    // Statistics
    struct ManagerStats {
        size_t total_functions = 0;
        size_t hot_functions = 0;
        size_t cold_functions = 0;
        size_t compiled_functions = 0;
        size_t optimized_functions = 0;
        size_t specialized_functions = 0;
        size_t inlined_functions = 0;
        size_t fused_functions = 0;
        size_t total_compilation_time = 0;
        size_t total_native_code_size = 0;
        
        double average_compilation_time() const {
            return compiled_functions > 0 ? 
                   static_cast<double>(total_compilation_time) / compiled_functions : 0.0;
        }
        
        void reset() { *this = ManagerStats(); }
        std::string to_string() const;
    };
    
    ManagerStats statistics() const;
    void reset_statistics();
    
    // Serialization
    struct SerializedManager {
        struct SerializedFunctionEntry {
            CompiledFunction::SerializedFunction function;
            bool is_hot;
        };
        
        std::vector<SerializedFunctionEntry> functions;
        ManagerStats stats;
        Config config;
    };
    
    SerializedManager serialize() const;
    bool deserialize(const SerializedManager& data);
    
    // Getters
    size_t count() const { return functions_.size(); }
    size_t native_code_size() const;
    
    // Iteration support
    auto begin() { return functions_.begin(); }
    auto end() { return functions_.end(); }
    auto begin() const { return functions_.begin(); }
    auto end() const { return functions_.end(); }
    
private:
    struct FunctionEntry {
        std::unique_ptr<CompiledFunction> function;
        uint64_t last_access;
        uint64_t access_count;
        bool is_hot;
        
        FunctionEntry(std::unique_ptr<CompiledFunction> func, uint64_t timestamp)
            : function(std::move(func)), last_access(timestamp), 
              access_count(1), is_hot(false) {}
    };
    
    using FunctionMap = std::unordered_map<FunctionId, FunctionEntry>;
    using PcToIdMap = std::unordered_map<uint32_t, FunctionId>;
    
    Recompiler& recompiler_;
    FunctionMap functions_;
    PcToIdMap pc_to_function_id_;
    
    // Hot function management
    std::unordered_set<FunctionId> hot_functions_;
    size_t hot_function_count_ = 0;
    
    // Statistics
    mutable ManagerStats stats_;
    mutable std::mutex stats_mutex_;
    
    // Configuration
    Config config_;
    
    // Thread safety
    mutable std::shared_mutex mutex_;
    
    // Helper methods
    FunctionId generate_id();
    void touch(FunctionId id);
    void evict_cold_functions();
    bool should_promote_to_hot(FunctionId id) const;
    
    // Function fusion helpers
    bool functions_can_fuse(FunctionId f1, FunctionId f2) const;
    std::unique_ptr<CompiledFunction> fuse_functions_impl(FunctionId f1, FunctionId f2);
};

/**
 * @brief Analyzes function behavior for optimization
 */
class FunctionAnalyzer {
public:
    struct AnalysisResult {
        bool can_inline = false;
        bool can_specialize = false;
        bool can_vectorize = false;
        bool can_parallelize = false;
        bool has_side_effects = false;
        bool is_pure = false;
        size_t estimated_cycles = 0;
        size_t estimated_instructions = 0;
        std::vector<uint32_t> critical_path;
        std::vector<uint32_t> loop_nests;
        
        std::string to_string() const;
    };
    
    FunctionAnalyzer();
    
    AnalysisResult analyze(const CompiledFunction& function) const;
    AnalysisResult analyze(uint32_t entry_pc, const MipsMemory& memory) const;
    
    // Pattern matching for common optimizations
    bool is_memory_copy_pattern(uint32_t start_pc, const MipsMemory& memory) const;
    bool is_memory_set_pattern(uint32_t start_pc, const MipsMemory& memory) const;
    bool is_math_function_pattern(uint32_t start_pc, const MipsMemory& memory) const;
    bool is_string_function_pattern(uint32_t start_pc, const MipsMemory& memory) const;
    
    // Cost estimation
    size_t estimate_inline_cost(const CompiledFunction& function) const;
    size_t estimate_specialization_benefit(const CompiledFunction& function, 
                                          const SpecializationContext& context) const;
    
    // Call graph analysis
    struct CallGraph {
        struct Node {
            FunctionId id;
            std::vector<FunctionId> calls;
            std::vector<FunctionId> called_by;
            size_t depth = 0;
            bool recursive = false;
        };
        
        std::unordered_map<FunctionId, Node> nodes;
        
        void add_function(FunctionId id);
        void add_call(FunctionId caller, FunctionId callee);
        void analyze();
        
        std::vector<FunctionId> get_recursive_functions() const;
        std::vector<std::vector<FunctionId>> get_call_chains(FunctionId start, FunctionId end) const;
        size_t get_max_depth() const;
    };
    
    CallGraph build_call_graph(const FunctionManager& manager) const;
    
private:
    // Analysis helpers
    bool has_side_effects(uint32_t pc, const MipsMemory& memory, 
                         std::unordered_set<uint32_t>& visited) const;
    bool is_pure_function(uint32_t pc, const MipsMemory& memory,
                         std::unordered_set<uint32_t>& visited) const;
    
    // Pattern recognition
    bool matches_memory_copy(uint32_t pc, const MipsMemory& memory) const;
    bool matches_memory_set(uint32_t pc, const MipsMemory& memory) const;
    bool matches_simple_math(uint32_t pc, const MipsMemory& memory) const;
    
    // Thread safety
    mutable std::shared_mutex mutex_;
};

} // namespace Recompiler
} // namespace Nova64

// Hash specializations
template<>
struct std::hash<Nova64::Recompiler::FunctionId> {
    size_t operator()(Nova64::Recompiler::FunctionId id) const noexcept {
        return static_cast<size_t>(id);
    }
};

template<>
struct std::hash<Nova64::Recompiler::CompiledFunction*> {
    size_t operator()(Nova64::Recompiler::CompiledFunction* func) const noexcept {
        return reinterpret_cast<size_t>(func);
    }
};

template<>
struct std::equal_to<Nova64::Recompiler::CompiledFunction*> {
    bool operator()(Nova64::Recompiler::CompiledFunction* a, 
                   Nova64::Recompiler::CompiledFunction* b) const noexcept {
        return a == b;
    }
};