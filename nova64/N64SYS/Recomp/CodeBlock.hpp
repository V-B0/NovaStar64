/**
 * @file CodeBlock.hpp
 * @brief Code block management for the dynamic recompiler
 * 
 * Nova64 N64 Emulator - Based on Project64 architecture
 */

#pragma once

#include "../Types.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "RegInfo.hpp"
#include "ExitInfo.hpp"
#include "JumpInfo.hpp"
#include "CodeSection.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <memory>
#include <unordered_map>
#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <atomic>
#include <span>
#include <list>
#include <mutex>
#include <shared_mutex>

// Third-party libraries
#include "asmjit.hpp" // AsmJit wrapper

class CN64System;
class CRegisters;

namespace Nova64 {

class MipsMemoryVM;

namespace Recompiler {

class Recompiler;
class RecompilationMemory;
class RecompilerOperations;

// Block execution flags
enum class BlockFlags : uint8_t {
    None = 0,
    HasLoop = 1 << 0,        // Block contains a loop
    HasConditional = 1 << 1, // Block contains conditional branches
    HasFloatOps = 1 << 2,    // Block contains floating point operations
    HasMemoryOps = 1 << 3,   // Block contains memory operations
    HasExceptions = 1 << 4,  // Block can throw exceptions
    Hot = 1 << 5,            // Block is frequently executed (hot)
    Cold = 1 << 6,           // Block is rarely executed (cold)
    Linked = 1 << 7          // Block has been linked to others
};

inline BlockFlags operator|(BlockFlags a, BlockFlags b) {
    return static_cast<BlockFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline BlockFlags operator&(BlockFlags a, BlockFlags b) {
    return static_cast<BlockFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/**
 * @brief Fast 128-bit hash for code block identification
 */
struct BlockHash {
    uint64_t low;
    uint64_t high;

    constexpr bool operator==(const BlockHash& other) const noexcept { return low == other.low && high == other.high; }

    constexpr bool operator!=(const BlockHash& other) const noexcept { return !(*this == other); }

    // Hash for unordered_map
    struct Hasher {
        size_t operator()(const BlockHash& h) const noexcept { return h.low ^ (h.high << 1); }
    };

    // Combine this hash with another value
    BlockHash combine(uint64_t value) const noexcept {
        // MurmurHash3 64-bit finalizer mix
        uint64_t k = value;
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;

        return BlockHash{low ^ k, high ^ (k >> 32)};
    }
};

// Simple FNV-1a 64-bit hash implementation
constexpr uint64_t fnv1a_hash(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ULL) noexcept {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;

    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

/**
 * @brief Basic block information for control flow analysis
 */
struct BasicBlockInfo {
    uint32_t start_address;
    uint32_t end_address;
    std::vector<uint32_t> predecessors;
    std::vector<uint32_t> successors;
    BlockFlags flags;

    bool contains(uint32_t address) const noexcept { return address >= start_address && address <= end_address; }

    size_t size() const noexcept {
        return (end_address - start_address) / 4 + 1; // MIPS instructions are 4 bytes
    }
};

/**
 * @brief Recompiled code block with JIT compilation
 * 
 * Uses RAII for resource management and modern C++ features for safety.
 */
class CodeBlock : public asmjit::ErrorHandler {
public:
    // Configuration
    struct Config {
        Config() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        bool enable_profiling = true;
        bool enable_logging = false;
        bool enable_optimizations = true;
        size_t max_block_size = 4096;  // Maximum bytes per block
        size_t max_instructions = 256; // Maximum instructions per block
        size_t cache_line_size = 64;   // For alignment
    };

    CodeBlock(N64System& system, uint32_t entry_address, const Config& config = Config(),
              asmjit::JitRuntime* shared_runtime = nullptr);
    ~CodeBlock() noexcept;

    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
    CodeBlock(CodeBlock&&) noexcept;
    CodeBlock& operator=(CodeBlock&&) noexcept;

    // Compilation
    enum class CompileResult {
        Success,
        FailedAnalysis,
        FailedCodeGen,
        OutOfMemory,
        InvalidBlock,
        TooLarge,
        SelfModifyingCode
    };

    struct CompileOptions {
        CompileOptions() noexcept {} // explicit ctor: avoids Clang NSDMI-in-default-arg error
        bool aggressive_optimizations = false;
        bool link_blocks = true;
        bool generate_debug_info = false;
        size_t optimization_level = 2; // 0=none, 1=basic, 2=aggressive
    };

    CompileResult compile(const CompileOptions& options = CompileOptions());
    std::optional<uintptr_t> finalize(RecompilationMemory& memory);

    // Block properties
    uint32_t entry_address() const noexcept { return entry_address_; }
    uint32_t start_address() const noexcept { return start_address_; }
    uint32_t end_address() const noexcept { return end_address_; }
    BlockHash hash() const noexcept { return hash_; }
    BlockFlags flags() const noexcept { return flags_; }
    bool is_valid() const noexcept { return is_valid_; }

    /// True when the block is a spin loop: a branch back to its own address
    /// whose delay slot cannot change the branch condition.
    ///
    /// This is the recompiler's equivalent of the interpreter's permanent-loop
    /// test (`CN64System::DelayedRelativeJump` with offset 0), and the two must
    /// agree -- if one engine fast-forwards the timer through a loop and the
    /// other spins it out instruction by instruction, their Count values drift
    /// apart. BlockFlags::HasLoop is set only by `check_perm_loop`, so it means
    /// exactly this and nothing broader.
    bool is_spin_loop() const noexcept { return (flags_ & BlockFlags::HasLoop) != BlockFlags::None; }

    asmjit::CodeHolder* GetCodeHolder() { return code_holder_.get(); }

    /// Get the system registers (used by platform-specific recompiler register allocators)
    ::CRegisters* GetRegisters() const noexcept { return registers_; }

    N64System* GetSystem() const noexcept { return system_; }

    bool is_compiled() const noexcept { return compiled_function_ != nullptr; }

    // Execution
    using ExecutionFunction = void (*)(CpuRegisters&, MipsMemory&);
    ExecutionFunction compiled_function() const noexcept { return compiled_function_; }

    // Instruction access
    size_t instruction_count() const noexcept { return statistics_.instruction_count; }
    const std::vector<uint32_t>& instructions() const noexcept { return instructions_; }
    std::optional<uint32_t> get_instruction(uint32_t address) const;

    // Statistics
    struct Statistics {
        size_t compilation_time_ns = 0; // Time spent compiling
        size_t execution_count = 0;     // Times executed
        size_t total_cycles = 0;        // Total cycles spent in this block
        size_t instruction_count = 0;   // MIPS instructions in block
        size_t generated_bytes = 0;     // Generated code size
        size_t basic_block_count = 0;   // Number of basic blocks
        double average_cycles = 0.0;    // Average cycles per execution

        void reset() noexcept { *this = Statistics(); }

        void record_execution(size_t cycles) noexcept {
            ++execution_count;
            total_cycles += cycles;
            average_cycles = static_cast<double>(total_cycles) / execution_count;
        }
    };

    const Statistics& statistics() const noexcept { return statistics_; }
    void record_execution(size_t cycles) noexcept { statistics_.record_execution(cycles); }
    void reset_statistics() noexcept { statistics_.reset(); }

    // Memory contents for self-modifying code
    struct MemorySnapshot {
        struct Entry {
            uint64_t value;
            uint32_t address;
            uintptr_t location;

            bool operator==(const Entry& other) const noexcept { return value == other.value; }
        };

        Entry entries[2];
        size_t count = 0;

        MemorySnapshot() = default;

        void add(uint32_t addr, uint64_t val, uintptr_t loc) noexcept {
            if (count < 2) {
                entries[count] = {val, addr, loc};
                ++count;
            }
        }

        bool matches(const MemorySnapshot& other) const noexcept {
            if (count != other.count) return false;
            for (size_t i = 0; i < count; ++i) {
                if (!(entries[i] == other.entries[i])) {
                    return false;
                }
            }
            return true;
        }

        bool empty() const noexcept { return count == 0; }
    };

    const MemorySnapshot& memory_snapshot() const noexcept { return memory_snapshot_; }
    bool verify_memory_snapshot() const;
    void update_memory_snapshot();

    // Debug information
    std::string disassembly() const;
    std::string control_flow_graph() const;
    std::string get_code_log() const { return code_log_; }

    // Hot/cold classification
    bool is_hot() const noexcept { return statistics_.execution_count > 1000; }
    bool is_cold() const noexcept { return statistics_.execution_count < 10; }
    bool should_reoptimize() const noexcept { return is_hot() && statistics_.execution_count % 10000 == 0; }

    // Linking and patching
    bool link_to(CodeBlock& target, uint32_t branch_address);
    void patch_branch(uint32_t from_address, uint32_t to_address);

    // Serialization (for save states)
    struct SerializedBlock {
        BlockHash hash;
        uint32_t entry_address;
        uint32_t start_address;
        uint32_t end_address;
        std::vector<uint32_t> instructions;
        MemorySnapshot snapshot;
        Statistics stats;
        BlockFlags flags;
    };

    SerializedBlock serialize() const;
    bool deserialize(const SerializedBlock& data);

private:
    // Analysis
    struct AnalysisResult {
        struct BranchInfo {
            uint32_t address;
            uint32_t target;
            bool likely;
            bool delay_slot;
            bool unconditional;
        };

        std::vector<BasicBlockInfo> basic_blocks;
        std::vector<BranchInfo> branches;
        std::vector<uint32_t> delay_slots;
        BlockFlags flags;
        bool contains_loop = false;
        bool contains_syscall = false;
        bool contains_breakpoint = false;
        bool contains_wait = false;
        bool contains_tlb_op = false;
        bool contains_cop1_op = false;
        size_t max_stack_depth = 0;

        void add_branch(uint32_t addr, uint32_t tgt, bool lk = false, bool ds = true, bool unc = false) {
            branches.push_back({addr, tgt, lk, ds, unc});
        }
    };

    std::optional<AnalysisResult> analyze_block();
    std::optional<BasicBlockInfo> analyze_basic_block(uint32_t start_address, AnalysisResult& out_result);
    bool analyze_instruction(uint32_t address, uint32_t opcode, AnalysisResult& result, BasicBlockInfo& current_block);

    // Code generation
    class CodeGenerator {
    public:
        CodeGenerator(asmjit::CodeHolder& code, const AnalysisResult& analysis, const Config& config);

        bool generate_prologue();
        bool generate_epilogue();
        bool generate_instruction(uint32_t address, uint32_t opcode);
        bool generate_branch(const AnalysisResult::BranchInfo& branch);
        bool generate_delay_slot(uint32_t address, uint32_t opcode);
        bool generate_linkage();
        bool generate_exception_handler();

        size_t generated_bytes() const noexcept { return generated_bytes_; }
        const std::vector<uint8_t>& generated_code() const noexcept { return generated_code_; }

    private:
        Recomp::Assembler assembler_;
        const AnalysisResult& analysis_;
        const Config& config_;
        std::unordered_map<uint32_t, asmjit::Label> labels_;
        std::vector<std::pair<uint32_t, asmjit::Label>> patch_points_;
        std::vector<uint8_t> generated_code_;
        size_t generated_bytes_ = 0;

        // Register allocation state
        struct RegisterState {
            bool allocated : 1;
            bool dirty : 1;
            uint8_t mapped_mips_reg : 5;
            uint64_t last_use : 56;
        };
        std::array<RegisterState, 16> register_states_;

        Recomp::Gp allocate_register(uint8_t mips_reg);
        void free_register(Recomp::Gp reg);
        void spill_dirty_registers();
    };

    bool generate_code(const AnalysisResult& analysis, const CompileOptions& options);
    bool optimize_code(const AnalysisResult& analysis);

    // Hashing
    BlockHash calculate_hash() const noexcept;
    uint64_t calculate_fast_hash() const noexcept;

    // Error handling
    void handleError(asmjit::Error error, const char* message, asmjit::BaseEmitter* origin);

    // Helper functions
    void log_compilation(const std::string& message);
    void log_error(const std::string& message);
    void update_hash();
    void invalidate() noexcept { is_valid_ = false; }

private:
    // System references
    N64System* system_;
    MipsMemoryVM* memory_;
    ::CRegisters* registers_;

    // Block properties
    uint32_t entry_address_;
    uint32_t start_address_;
    uint32_t end_address_;
    BlockHash hash_;
    BlockFlags flags_;
    bool is_valid_ = true;

    // Instructions in this block
    std::vector<uint32_t> instructions_;

    // Code sections (for complex blocks)
    std::vector<CodeSection> sections_;
    std::unordered_map<uint32_t, size_t> address_to_section_;

    // JIT compilation
    std::unique_ptr<asmjit::JitRuntime> owned_runtime_; // only set when no shared runtime
    asmjit::JitRuntime* runtime_ = nullptr;             // points to shared or owned
    std::unique_ptr<asmjit::CodeHolder> code_holder_;
    ExecutionFunction compiled_function_ = nullptr;

    // Memory tracking for self-modifying code
    MemorySnapshot memory_snapshot_;

    // Statistics and profiling
    Statistics statistics_;

    Config config_;

    // Debug and logging
    std::string code_log_;
    std::vector<uint8_t> generated_code_cache_;

    // Cache line padding for performance
    static constexpr size_t CACHE_LINE_SIZE = 64;
    alignas(CACHE_LINE_SIZE) mutable uint64_t last_access_timestamp_ = 0;
};

/**
 * @brief Manages a collection of code blocks with LRU eviction
 */
class CodeBlockCache {
public:
    struct Config {
        Config() noexcept {}                      // explicit ctor: avoids Clang NSDMI-in-default-arg error
        size_t max_blocks = 1024;
        size_t max_total_size = 64 * 1024 * 1024; // 64MB
        float hot_threshold = 0.1f;               // 10% of cache can be hot
        bool enable_compression = false;
        bool enable_prefetch = true;
    };

    explicit CodeBlockCache(const Config& config = Config());
    ~CodeBlockCache();

    // Block management
    std::shared_ptr<CodeBlock> get_or_compile(uint32_t address);
    std::shared_ptr<CodeBlock> get(uint32_t address) const;
    void insert(std::shared_ptr<CodeBlock> block);
    void remove(uint32_t address);
    void invalidate(uint32_t address);
    void invalidate_range(uint32_t start, uint32_t end);
    void invalidate_all();
    void clear();

    // Batch operations
    void prefetch(const std::vector<uint32_t>& addresses);
    void compress_cold_blocks();

    // Statistics
    struct CacheStats {
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
        size_t compilations = 0;
        size_t recompilations = 0;
        size_t current_blocks = 0;
        size_t current_size = 0;
        size_t total_compilation_time = 0;

        double hit_ratio() const noexcept {
            return (hits + misses) > 0 ? static_cast<double>(hits) / (hits + misses) : 0.0;
        }

        double average_compilation_time() const noexcept {
            return compilations > 0 ? static_cast<double>(total_compilation_time) / compilations : 0.0;
        }

        void reset() noexcept { *this = CacheStats(); }
    };

    CacheStats statistics() const;
    void reset_statistics();

    // Memory management
    size_t memory_usage() const;
    void set_memory_limit(size_t bytes);

private:
    struct CacheEntry {
        std::shared_ptr<CodeBlock> block;
        uint64_t last_access;
        uint64_t access_count;
        bool is_hot;

        CacheEntry(std::shared_ptr<CodeBlock> b, uint64_t timestamp) :
            block(std::move(b)), last_access(timestamp), access_count(1), is_hot(false) {}
    };

    using BlockMap = std::unordered_map<uint32_t, CacheEntry>;
    using LruList = std::list<uint32_t>;
    using LruMap = std::unordered_map<uint32_t, LruList::iterator>;

    BlockMap blocks_;
    LruList lru_list_;
    LruMap lru_map_;
    Config config_;

    // Hot block management (separate LRU for frequently accessed blocks)
    LruList hot_lru_list_;
    LruMap hot_lru_map_;
    size_t hot_block_count_ = 0;

    mutable std::shared_mutex mutex_;

    mutable CacheStats stats_;
    mutable std::mutex stats_mutex_;

    // Helper methods
    void touch(uint32_t address, bool is_hit);
    void evict_oldest();
    void promote_to_hot(uint32_t address);
    void demote_from_hot(uint32_t address);
    bool should_promote(uint32_t address) const;

    // Timestamp generation
    uint64_t get_timestamp() const noexcept {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
};

/**
 * @brief Factory for creating code blocks with different optimization levels
 */
class CodeBlockFactory {
public:
    enum class OptimizationLevel {
        None,         // No optimization, direct translation
        Basic,        // Basic optimizations (dead code elimination, constant folding)
        Aggressive,   // Aggressive optimizations (inlining, loop unrolling)
        ProfileGuided // Profile-guided optimizations
    };

    struct ProfileData {
        std::unordered_map<uint32_t, uint64_t> address_execution_counts;
        std::unordered_map<uint32_t, uint64_t> branch_taken_counts;
        std::vector<uint32_t> hot_paths;
        size_t total_executions = 0;
    };

    static std::unique_ptr<CodeBlock> create_block(N64System& system, uint32_t address,
                                                   OptimizationLevel level = OptimizationLevel::Aggressive,
                                                   const ProfileData* profile = nullptr);

    // Block templates for common patterns
    static std::shared_ptr<CodeBlock> create_nop_block(N64System& system);
    static std::shared_ptr<CodeBlock> create_delay_slot_block(N64System& system);
    static std::shared_ptr<CodeBlock> create_branch_likely_block(N64System& system);
    static std::shared_ptr<CodeBlock> create_exception_handler(N64System& system);
    static std::shared_ptr<CodeBlock> create_tlb_miss_handler(N64System& system);

    // Optimization passes
    static bool apply_dead_code_elimination(CodeBlock& block);
    static bool apply_constant_folding(CodeBlock& block);
    static bool apply_common_subexpression_elimination(CodeBlock& block);
    static bool apply_loop_invariant_code_motion(CodeBlock& block);
    static bool apply_strength_reduction(CodeBlock& block);

private:
    static bool is_nop_sequence(const std::vector<uint32_t>& instructions);
    static bool is_branch_likely_pattern(const std::vector<uint32_t>& instructions);
    static bool is_exception_handler_pattern(const std::vector<uint32_t>& instructions);
};

} // namespace Recompiler
} // namespace Nova64

// Hash specialization for BlockHash
template <> struct std::hash<Nova64::Recompiler::BlockHash> {
    size_t operator()(const Nova64::Recompiler::BlockHash& h) const noexcept {
        // Mix both parts for better distribution
        size_t seed = static_cast<size_t>(h.low);
        seed ^= static_cast<size_t>(h.high) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};
