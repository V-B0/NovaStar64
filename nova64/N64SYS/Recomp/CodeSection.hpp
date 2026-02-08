/**
 * @file CodeSection.hpp
 * @brief Code section management for the dynamic recompiler
 * 
 * Nova64 N64 Emulator - Modern C++ reimplementation
 * Features: RAII, type safety, performance optimizations, and thread safety
 */

#pragma once

#include "asmjit.hpp"
#include "RegInfo.hpp"
#include "ExitInfo.hpp"
#include "JumpInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <optional>
#include <functional>
#include <atomic>
#include <span>
#include <string>
#include <mutex>
#include <array>

namespace Nova64 {
    
// Forward declarations
class CpuRegisters;
class MipsMemory;
class N64System;

namespace Recompiler {

// Forward declarations
class CodeBlock;
class RegisterInfo;
class RecompilerOperations;
class JumpInfo;

// Strongly typed identifiers
enum class SectionId : uint32_t {};
enum class BlockId : uint32_t {};

// Section types
enum class SectionType : uint8_t {
    Normal,
    DelaySlot,          // Contains a delay slot instruction
    Loop,               // Forms a loop
    ExceptionHandler,   // Exception handling code
    BranchTarget,       // Target of a branch/jump
    LinkBlock,          // Block that can be linked to others
    HotPath,            // Frequently executed section
    ColdPath            // Rarely executed section
};

// Section flags (using uint32_t to support more flags)
enum class SectionFlags : uint32_t {
    None                = 0,
    Linked              = 1 << 0,   // Section is linked to other sections
    Compiled            = 1 << 1,   // Native code has been generated
    HasDelaySlot        = 1 << 2,   // Section contains a delay slot
    HasBranch           = 1 << 3,   // Section ends with a branch
    HasJump             = 1 << 4,   // Section ends with a jump
    HasLoop             = 1 << 5,   // Section contains a loop
    ExceptionPossible   = 1 << 6,   // Section can throw exceptions
    SelfModifying       = 1 << 7,   // Section contains self-modifying code
    ProfileGuided       = 1 << 8,   // Optimized with profile data
    Optimized           = 1 << 9,   // Has been optimized
    InlineCandidate     = 1 << 10,  // Good candidate for inlining
    CriticalPath        = 1 << 11,  // On the critical execution path
    CacheAligned        = 1 << 12,  // Memory accesses are cache-aligned
    Vectorizable        = 1 << 13,  // Contains vectorizable operations
    Atomic              = 1 << 14,  // Contains atomic operations
    BranchPredicted     = 1 << 15,  // Branch has been predicted
    HotPath             = 1 << 16,  // Frequently executed section
    ColdPath            = 1 << 17   // Rarely executed section
};

inline SectionFlags operator|(SectionFlags a, SectionFlags b) {
    return static_cast<SectionFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SectionFlags operator&(SectionFlags a, SectionFlags b) {
    return static_cast<SectionFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool has_flag(SectionFlags value, SectionFlags flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

/**
 * @brief Register state at section entry
 */
struct RegisterEntryState {
    struct RegisterInfo {
        bool allocated = false;
        bool dirty = false;
        bool constant = false;
        bool locked = false;
        uint32_t value = 0;
        uint32_t last_use = 0;
    };
    
    std::array<RegisterInfo, 32> gpr;      // General Purpose Registers
    std::array<RegisterInfo, 32> fpr;      // Floating Point Registers
    RegisterInfo hi;                       // HI register
    RegisterInfo lo;                       // LO register
    
    // Compare two states for equality
    bool operator==(const RegisterEntryState& other) const;
    
    // Merge two states (used for control flow merging)
    void merge(const RegisterEntryState& other);
    
    // Reset to default state
    void reset();
    
    // String representation for debugging
    std::string to_string() const;
};

/**
 * @brief Control flow edge between sections
 */
struct ControlFlowEdge {
    SectionId source;
    SectionId target;
    uint32_t branch_address;
    bool is_conditional;
    bool is_likely;
    bool has_delay_slot;
    uint64_t execution_count = 0;
    
    // Weight for profile-guided optimization
    double weight() const {
        return static_cast<double>(execution_count);
    }
    
    bool operator==(const ControlFlowEdge& other) const {
        return source == other.source && target == other.target;
    }
    
    struct Hasher {
        size_t operator()(const ControlFlowEdge& edge) const {
            return std::hash<size_t>{}(static_cast<size_t>(edge.source) ^ 
                                      (static_cast<size_t>(edge.target) << 16));
        }
    };
};

/**
 * @brief Section metadata for profiling and optimization
 */
struct SectionMetadata {
    uint32_t entry_pc;
    uint32_t end_pc;
    uint32_t instruction_count = 0;
    uint64_t execution_count = 0;
    uint64_t total_cycles = 0;
    double average_cycles = 0.0;
    uint64_t last_execution = 0;
    std::vector<uint32_t> hot_paths;
    
    void record_execution(uint64_t cycles) {
        ++execution_count;
        total_cycles += cycles;
        average_cycles = static_cast<double>(total_cycles) / execution_count;
        last_execution = get_timestamp();
    }
    
    bool is_hot() const { return execution_count > 1000; }
    bool is_cold() const { return execution_count < 10; }
    
private:
    static uint64_t get_timestamp() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
};

/**
 * @brief Represents a section of recompiled code
 * 
 * A code section is a contiguous sequence of MIPS instructions that
 * ends with a control flow instruction (branch, jump, or exception).
 * Sections are linked together to form complete recompiled blocks.
 * 
 * NOTE: Currently using AsmJit stubs - full integration pending
 */
class CodeSection {
public:
    // Configuration
    struct Config {
        bool enable_profiling = true;
        bool enable_optimizations = true;
        bool enable_inlining = false;
        size_t max_instructions = 256;
        size_t max_successors = 8;
        size_t cache_line_size = 64;
    };
    
    // Construction/Destruction
    CodeSection(CodeBlock& parent_block, 
                uint32_t entry_pc, 
                SectionId id,
                bool allow_linking = true,
                const Config& config = {});
    
    ~CodeSection() noexcept;
    
    // Non-copyable, movable
    CodeSection(const CodeSection&) = delete;
    CodeSection& operator=(const CodeSection&) = delete;
    CodeSection(CodeSection&&) noexcept;
    CodeSection& operator=(CodeSection&&) noexcept;
    
    // Setup and configuration
    void set_delay_slot();
    void set_jump_target(uint32_t branch_pc, uint32_t target_pc, bool permanent_loop);
    void set_continue_target(uint32_t branch_pc, uint32_t target_pc);
    void set_type(SectionType type) { type_ = type; }
    
    // Code generation
    enum class GenerationResult {
        Success,
        FailedAnalysis,
        FailedCodeGen,
        OutOfMemory,
        InvalidSection
    };
    
    GenerationResult generate_native_code();
    bool generate_linkage();
    bool inline_section(const CodeSection& source);
    
    // Control flow analysis
    void analyze_control_flow();
    void analyze_data_dependencies();
    void determine_loops();
    void compute_dominance_tree();
    
    // Section query and navigation
    std::optional<CodeSection*> find_existing_section(uint32_t address);
    bool is_accessible(SectionId section_id) const;
    bool is_dominated_by(SectionId dominator) const;
    bool post_dominates(SectionId target) const;
    
    // Parent/child management
    void add_parent(CodeSection* parent);
    void remove_parent(CodeSection* parent);
    void switch_parent(CodeSection* old_parent, CodeSection* new_parent);
    void add_child(CodeSection* child);
    
    // Register state management
    void capture_entry_state();
    void propagate_constants();
    void update_register_state(const RegisterEntryState& new_state);
    const RegisterEntryState& get_entry_state() const { return entry_state_; }
    
    // Optimization
    void apply_optimizations();
    bool optimize_control_flow();
    bool eliminate_dead_code();
    bool fold_constants();
    bool hoist_invariants();
    bool schedule_instructions();
    
    // Debug and information
    std::string get_disassembly() const;
    std::string get_control_flow_graph() const;
    std::string get_register_state_string() const;
    
    // Statistics
    struct Statistics {
        size_t generation_time_ns = 0;
        size_t optimization_time_ns = 0;
        size_t native_code_size = 0;
        size_t mips_instruction_count = 0;
        size_t basic_block_count = 1;
        size_t data_dependencies = 0;
        size_t control_dependencies = 0;
        
        void reset() { *this = Statistics(); }
    };
    
    const Statistics& statistics() const { return stats_; }
    const SectionMetadata& metadata() const { return metadata_; }
    
    // Getters
    SectionId id() const { return id_; }
    uint32_t entry_pc() const { return metadata_.entry_pc; }
    uint32_t end_pc() const { return metadata_.end_pc; }
    SectionType type() const { return type_; }
    SectionFlags flags() const { return flags_; }
    CodeBlock& parent_block() { return parent_block_; }
    const CodeBlock& parent_block() const { return parent_block_; }
    
    bool is_linked() const { return has_flag(flags_, SectionFlags::Linked); }
    bool is_compiled() const { return has_flag(flags_, SectionFlags::Compiled); }
    bool has_delay_slot() const { return has_flag(flags_, SectionFlags::HasDelaySlot); }
    bool contains_loop() const { return has_flag(flags_, SectionFlags::HasLoop); }
    
    // Successors and predecessors
    const std::vector<SectionId>& successors() const { return successors_; }
    const std::vector<SectionId>& predecessors() const { return predecessors_; }
    const std::vector<ControlFlowEdge>& outgoing_edges() const { return outgoing_edges_; }
    const std::vector<ControlFlowEdge>& incoming_edges() const { return incoming_edges_; }
    
    // Dominance information
    std::optional<SectionId> immediate_dominator() const { return immediate_dominator_; }
    const std::vector<SectionId>& dominated_sections() const { return dominated_; }
    
    // Execution
    void record_execution(uint64_t cycles = 1) {
        metadata_.record_execution(cycles);
        if (metadata_.is_hot()) {
            flags_ = flags_ | SectionFlags::HotPath;
        }
    }
    
    // Patch management for self-modifying code
    struct PatchPoint {
        uint32_t mips_address;
        asmjit::Label label;
        size_t patch_offset;
        bool resolved = false;
        
        bool operator==(const PatchPoint& other) const {
            return mips_address == other.mips_address;
        }
    };
    
    void add_patch_point(uint32_t mips_address, asmjit::Label label);
    bool resolve_patch_points();
    
private:
    // AsmJit error handler (for future integration)
    void handleError(asmjit::Error error, const char* message,
                    asmjit::BaseEmitter* origin);
    
    // Internal analysis methods
    struct AnalysisResult {
        std::vector<uint32_t> basic_blocks;
        std::vector<ControlFlowEdge> edges;
        std::vector<uint32_t> dominators;
        std::vector<uint32_t> post_dominators;
        bool has_loop = false;
        bool has_exception = false;
        bool has_atomic = false;
        size_t max_register_pressure = 0;
    };
    
    std::optional<AnalysisResult> analyze_section();
    void build_control_flow_graph(const AnalysisResult& analysis);
    void compute_dominance(const AnalysisResult& analysis);
    
    // Code generation helpers
    class CodeGenerator {
    public:
        CodeGenerator(asmjit::CodeHolder& code, const AnalysisResult& analysis,
                     const RegisterEntryState& entry_state);
        
        bool generate_prologue();
        bool generate_epilogue();
        bool generate_instruction(uint32_t address, uint32_t opcode);
        bool generate_branch(const ControlFlowEdge& edge);
        bool generate_delay_slot(uint32_t address, uint32_t opcode);
        
        size_t generated_bytes() const { return generated_bytes_; }
        
    private:
        asmjit::x86::Assembler* assembler_;
        const AnalysisResult& analysis_;
        const RegisterEntryState& entry_state_;
        std::unordered_map<uint32_t, asmjit::Label> labels_;
        size_t generated_bytes_ = 0;
    };
    
    bool generate_code(const AnalysisResult& analysis);
    
    // Optimization helpers
    void apply_peephole_optimizations();
    void apply_local_optimizations();
    void apply_global_optimizations();
    
    // Helper methods
    void log_generation(const std::string& message);
    void update_flags();
    void invalidate() { flags_ = SectionFlags::None; }
    
private:
    // Parent relationship
    CodeBlock& parent_block_;
    SectionId id_;
    
    // Section properties
    SectionType type_ = SectionType::Normal;
    SectionFlags flags_ = SectionFlags::None;
    Config config_;
    
    // Control flow graph
    std::vector<SectionId> successors_;
    std::vector<SectionId> predecessors_;
    std::vector<ControlFlowEdge> outgoing_edges_;
    std::vector<ControlFlowEdge> incoming_edges_;
    
    // Dominance tree
    std::optional<SectionId> immediate_dominator_;
    std::vector<SectionId> dominated_;
    std::vector<SectionId> post_dominated_by_;
    
    // Register state
    RegisterEntryState entry_state_;
    RegisterEntryState exit_state_;
    
    // Metadata and statistics
    SectionMetadata metadata_;
    Statistics stats_;
    
    // Code generation
    std::unique_ptr<asmjit::CodeHolder> code_holder_;
    asmjit::Label entry_label_;
    std::vector<PatchPoint> patch_points_;
    
    // Debug information
    std::string debug_log_;
    std::vector<uint32_t> instructions_;
    
    // Cache for frequently accessed data
    static constexpr size_t CACHE_LINE_SIZE = 64;
    alignas(CACHE_LINE_SIZE) mutable uint64_t last_access_ = 0;
    
    // For thread safety in statistics
    mutable std::mutex stats_mutex_;
};

/**
 * @brief Manages relationships between code sections
 */
class SectionGraph {
public:
    using NodeId = SectionId;
    using EdgeId = size_t;
    
    struct Node {
        CodeSection* section = nullptr;
        std::unordered_set<EdgeId> outgoing_edges;
        std::unordered_set<EdgeId> incoming_edges;
        std::unordered_set<NodeId> dominators;
        std::unordered_set<NodeId> post_dominators;
        uint32_t dfs_number = 0;
        uint32_t low_link = 0;
        bool on_stack = false;
    };
    
    struct Edge {
        NodeId source;
        NodeId target;
        ControlFlowEdge data;
    };
    
    SectionGraph();
    ~SectionGraph();
    
    // Graph operations
    NodeId add_node(CodeSection* section);
    EdgeId add_edge(NodeId source, NodeId target, const ControlFlowEdge& data);
    void remove_node(NodeId node);
    void remove_edge(EdgeId edge);
    
    // Query operations
    std::optional<NodeId> find_node(uint32_t entry_pc) const;
    std::optional<EdgeId> find_edge(NodeId source, NodeId target) const;
    std::vector<NodeId> find_path(NodeId start, NodeId end) const;
    
    // Analysis
    void compute_dominance_tree();
    void compute_post_dominance_tree();
    void find_loops();
    void find_reducible_loops();
    void compute_scc();  // Strongly Connected Components
    
    // Optimization
    void inline_sections(NodeId target, const std::vector<NodeId>& sources);
    void merge_equivalent_sections();
    void eliminate_dead_sections();
    
    // Serialization
    struct SerializedGraph {
        struct SerializedNode {
            uint32_t entry_pc;
            SectionType type;
            SectionFlags flags;
            std::vector<uint32_t> instructions;
        };
        
        struct SerializedEdge {
            uint32_t source_pc;
            uint32_t target_pc;
            ControlFlowEdge data;
        };
        
        std::vector<SerializedNode> nodes;
        std::vector<SerializedEdge> edges;
    };
    
    SerializedGraph serialize() const;
    bool deserialize(const SerializedGraph& graph, CodeBlock& parent);
    
    // Statistics
    struct GraphStats {
        size_t node_count = 0;
        size_t edge_count = 0;
        size_t loop_count = 0;
        size_t scc_count = 0;
        size_t max_depth = 0;
        double average_branching_factor = 0.0;
        
        void reset() { *this = GraphStats(); }
    };
    
    GraphStats statistics() const;
    
private:
    std::unordered_map<NodeId, Node> nodes_;
    std::unordered_map<EdgeId, Edge> edges_;
    std::unordered_map<uint32_t, NodeId> pc_to_node_;
    NodeId next_node_id_ = SectionId{0};
    EdgeId next_edge_id_ = 0;
    
    // Helper methods
    void tarjan_scc(NodeId node, std::vector<NodeId>& stack,
                   std::vector<std::vector<NodeId>>& components);
    void compute_dominators();
    void compute_post_dominators();
};

} // namespace Recompiler
} // namespace Nova64

// Hash specializations
template<>
struct std::hash<Nova64::Recompiler::SectionId> {
    size_t operator()(Nova64::Recompiler::SectionId id) const noexcept {
        return static_cast<size_t>(id);
    }
};

template<>
struct std::hash<Nova64::Recompiler::ControlFlowEdge> {
    size_t operator()(const Nova64::Recompiler::ControlFlowEdge& edge) const noexcept {
        size_t h1 = std::hash<size_t>{}(static_cast<size_t>(edge.source));
        size_t h2 = std::hash<size_t>{}(static_cast<size_t>(edge.target));
        return h1 ^ (h2 << 1);
    }
};