/**
 * @file FunctionInfo.cpp
 * @brief Implementation of compiled function management system
 * 
 * Nova64 N64 Emulator - Modern C++ reimplementation
 * Based on Project64 recompiler with modern C++ improvements
 */

#include "FunctionInfo.hpp"
#include "CodeBlock.hpp"
#include "CodeSection.hpp"
#include "Recompiler.hpp"
#include "RegInfo.hpp"
#include "ExitInfo.hpp"
#include "../MainSYS.hpp"
#include "../SysGlobals.hpp"
#include "../Types.hpp"
#include "../MIPSY/Register.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../../common/log.hpp"
#include "../../common/trace.hpp"
#include "../../common/md5.hpp"
#include "../../common/MemoryManagment.hpp"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace Nova64::Recompiler {

// ============================================================================
// CompiledFunction Implementation
// ============================================================================

CompiledFunction::CompiledFunction(Recompiler& recompiler, uint32_t entry_pc, 
                                   const Config& config)
    : recompiler_(recompiler)
    , id_(static_cast<FunctionId>(0))  // Will be set by manager
    , config_(config)
{
    metadata_.entry_pc = entry_pc;
    metadata_.min_pc = entry_pc;
    metadata_.max_pc = entry_pc;
    
    // Calculate initial hash
    update_hash();
}

CompiledFunction::~CompiledFunction() {
    // Cleanup native code if allocated
    if (code_ptr_) {
        // Would free executable memory here
        code_ptr_ = nullptr;
    }
}

CompiledFunction::CompiledFunction(CompiledFunction&& other) noexcept
    : recompiler_(other.recompiler_)
    , id_(other.id_)
    , metadata_(std::move(other.metadata_))
    , hash_(other.hash_)
    , status_(other.status_)
    , flags_(other.flags_)
    , optimization_level_(other.optimization_level_)
    , function_ptr_(other.function_ptr_)
    , code_ptr_(other.code_ptr_)
    , code_size_(other.code_size_)
    , memory_snapshot_(std::move(other.memory_snapshot_))
    , inlining_info_(std::move(other.inlining_info_))
    , specializations_(std::move(other.specializations_))
    , stats_(std::move(other.stats_))
    , config_(other.config_)
    , next_(std::move(other.next_))
    , debug_log_(std::move(other.debug_log_))
    , native_code_cache_(std::move(other.native_code_cache_))
{
    other.function_ptr_ = nullptr;
    other.code_ptr_ = nullptr;
    other.code_size_ = 0;
}

CompiledFunction& CompiledFunction::operator=(CompiledFunction&& other) noexcept {
    if (this != &other) {
        // Note: recompiler_ is a reference and can't be moved
        id_ = other.id_;
        metadata_ = std::move(other.metadata_);
        hash_ = other.hash_;
        status_ = other.status_;
        flags_ = other.flags_;
        optimization_level_ = other.optimization_level_;
        function_ptr_ = other.function_ptr_;
        code_ptr_ = other.code_ptr_;
        code_size_ = other.code_size_;
        memory_snapshot_ = std::move(other.memory_snapshot_);
        inlining_info_ = std::move(other.inlining_info_);
        specializations_ = std::move(other.specializations_);
        stats_ = std::move(other.stats_);
        config_ = other.config_;
        next_ = std::move(other.next_);
        debug_log_ = std::move(other.debug_log_);
        native_code_cache_ = std::move(other.native_code_cache_);
        
        other.function_ptr_ = nullptr;
        other.code_ptr_ = nullptr;
        other.code_size_ = 0;
    }
    return *this;
}

CompiledFunction::CompileResult CompiledFunction::compile(const CompileOptions& options) {
    std::unique_lock lock(mutex_);
    
    set_status(CompilationStatus::Compiling);
    log_compilation("Starting compilation");
    
    // Step 1: Analyze function structure
    auto analysis = analyze_function();
    if (!analysis) {
        set_status(CompilationStatus::Exception);
        return CompileResult::FailedAnalysis;
    }
    
    // Update metadata from analysis
    metadata_.instruction_count = analysis->total_instructions;
    metadata_.basic_block_count = analysis->basic_blocks.size();
    metadata_.optimization_level = options.optimization;
    
    // Step 2: Generate native code
    if (!generate_native_code(*analysis)) {
        set_status(CompilationStatus::Exception);
        return CompileResult::FailedCodeGen;
    }
    
    // Step 3: Optimize if requested
    if (options.optimization != OptimizationLevel::None) {
        if (!optimize_native_code(*analysis)) {
            // Continue even if optimization fails
            log_compilation("Optimization failed, using unoptimized code");
        }
    }
    
    // Step 4: Capture memory snapshot for self-modifying code detection
    capture_memory_snapshot();
    
    // Update flags and status
    update_flags();
    set_status(CompilationStatus::Compiled);
    optimization_level_ = options.optimization;
    
    stats_.record_recompilation();
    
    log_compilation("Compilation complete");
    return CompileResult::Success;
}

bool CompiledFunction::recompile(const CompileOptions& options) {
    std::unique_lock lock(mutex_);
    
    log_compilation("Recompiling function");
    
    // Invalidate current compilation
    set_status(CompilationStatus::NotCompiled);
    
    // Clear old code
    if (code_ptr_) {
        // Would free old code here
        code_ptr_ = nullptr;
        code_size_ = 0;
    }
    
    // Recompile
    lock.unlock();
    return compile(options) == CompileResult::Success;
}

void CompiledFunction::execute(CpuRegisters& registers, MipsMemory& memory, void* context) {
    std::shared_lock lock(mutex_);
    
    if (!is_valid() || !function_ptr_) {
        // Function not compiled, interpret or recompile
        return;
    }
    
    // Record execution for profiling
    uint64_t start_cycles = 0;  // Would get from system
    
    uint64_t end_cycles = 0;  // Would get from system
    uint64_t cycles = end_cycles - start_cycles;
    
    // Update statistics
    stats_.record_execution(cycles, metadata_.instruction_count);
    stats_.record_cache_hit(true);
}

bool CompiledFunction::execute_safe(CpuRegisters& registers, MipsMemory& memory, 
                                    void* context) {
    try {
        // Verify memory snapshot before execution
        if (!verify_memory_snapshot()) {
            log_compilation("Memory snapshot mismatch - recompiling");
            return false;
        }
        
        execute(registers, memory, context);
        return true;
    } catch (const std::exception& e) {
        log_compilation(std::string("Exception during execution: ") + e.what());
        stats_.record_exception();
        set_status(CompilationStatus::Exception);
        return false;
    }
}

void CompiledFunction::optimize() {
    std::unique_lock lock(mutex_);
    
    if (status_ == CompilationStatus::Optimized) {
        return;  // Already optimized
    }
    
    log_compilation("Optimizing function");
    
    // Apply various optimizations based on profiling data
    if (stats_.is_hot()) {
        flags_ = flags_ | FunctionFlags::Hot;
        
        // Hot functions get more aggressive optimization
        if (config_.enable_inlining) {
            // Would apply inlining here
        }
    }
    
    set_status(CompilationStatus::Optimized);
    flags_ = flags_ | FunctionFlags::ProfileGuided;
}

bool CompiledFunction::inline_function(FunctionId function_id, uint32_t call_site) {
    std::unique_lock lock(mutex_);
    
    if (!config_.enable_inlining) {
        return false;
    }
    
    if (!can_inline_function(function_id, call_site)) {
        return false;
    }
    
    // Calculate estimated savings
    size_t savings = estimate_inline_savings(function_id);
    
    // Perform inlining (simplified)
    uint32_t inline_start = call_site;
    uint32_t inline_end = call_site + 4;  // Simplified
    
    inlining_info_.add_inlined(function_id, call_site, inline_start, inline_end, savings);
    
    flags_ = flags_ | FunctionFlags::Inlined;
    log_compilation("Inlined function at call site");
    
    return true;
}

bool CompiledFunction::specialize(const SpecializationContext& context) {
    std::unique_lock lock(mutex_);
    
    if (!config_.enable_specialization) {
        return false;
    }
    
    if (!should_specialize(context)) {
        return false;
    }
    
    // Check if already specialized for this context
    if (specializations_.find(context) != specializations_.end()) {
        return true;  // Already exists
    }
    
    // Create specialized version
    auto specialized = std::make_unique<CompiledFunction>(recompiler_, entry_pc(), config_);
    
    CompileOptions opts;
    opts.specialize = true;
    opts.specialization_context = context;
    opts.optimization = OptimizationLevel::Aggressive;
    
    if (specialized->compile(opts) == CompileResult::Success) {
        specialized->flags_ = specialized->flags_ | FunctionFlags::Specialized;
        specializations_[context] = std::move(specialized);
        log_compilation("Created specialized version");
        return true;
    }
    
    return false;
}

bool CompiledFunction::apply_profile_guided_optimization(const ProfileData& profile) {
    std::unique_lock lock(mutex_);
    
    // Apply optimizations based on profiling data
    // This is a simplified version - real implementation would be more complex
    
    if (stats_.is_hot()) {
        flags_ = flags_ | FunctionFlags::Hot | FunctionFlags::ProfileGuided;
        optimization_level_ = OptimizationLevel::ProfileGuided;
        
        log_compilation("Applied profile-guided optimization");
        return true;
    }
    
    return false;
}

bool CompiledFunction::verify() const {
    std::shared_lock lock(mutex_);
    
    // Verify function consistency
    if (metadata_.min_pc > metadata_.max_pc) {
        return false;
    }
    
    if (metadata_.entry_pc < metadata_.min_pc || 
        metadata_.entry_pc > metadata_.max_pc) {
        return false;
    }
    
    if (status_ == CompilationStatus::Compiled || 
        status_ == CompilationStatus::Optimized) {
        if (!code_ptr_ || code_size_ == 0) {
            return false;
        }
    }
    
    return true;
}

bool CompiledFunction::verify_memory_snapshot() const {
    std::shared_lock lock(mutex_);
    
    if (memory_snapshot_.empty()) {
        return true;  // No snapshot to verify
    }
    
    // Would verify memory hasn't changed
    // For now, simplified version
    return true;
}

void CompiledFunction::update_memory_snapshot() {
    std::unique_lock lock(mutex_);
    capture_memory_snapshot();
}

void CompiledFunction::invalidate_if_modified() {
    std::unique_lock lock(mutex_);
    
    if (!verify_memory_snapshot()) {
        log_compilation("Memory modified - invalidating");
        set_status(CompilationStatus::Invalidated);
        flags_ = flags_ | FunctionFlags::HasSelfModifying;
    }
}

void CompiledFunction::record_execution(uint64_t cycles, uint64_t instructions) {
    // Lock-free statistics update for hot path
    stats_.record_execution(cycles, instructions);
    
    // Update access time
    last_access_ = stats_.last_execution;
}

std::string CompiledFunction::get_disassembly() const {
    std::shared_lock lock(mutex_);
    
    std::ostringstream oss;
    oss << "Function " << static_cast<uint32_t>(id_) << " disassembly:\n";
    oss << "Entry PC: 0x" << std::hex << std::setw(8) << std::setfill('0') 
        << entry_pc() << "\n";
    oss << "Size: " << std::dec << metadata_.size() << " instructions\n";
    oss << "Native code size: " << code_size_ << " bytes\n";
    
    // Would disassemble native code here
    
    return oss.str();
}

std::string CompiledFunction::get_metadata_string() const {
    std::shared_lock lock(mutex_);
    
    std::ostringstream oss;
    oss << "Function Metadata:\n";
    oss << "  ID: " << static_cast<uint32_t>(id_) << "\n";
    oss << "  Entry PC: 0x" << std::hex << entry_pc() << "\n";
    oss << "  Range: 0x" << min_pc() << " - 0x" << max_pc() << "\n";
    oss << "  Instructions: " << std::dec << metadata_.instruction_count << "\n";
    oss << "  Basic blocks: " << metadata_.basic_block_count << "\n";
    oss << "  Native size: " << code_size_ << " bytes\n";
    oss << "  Status: " << static_cast<int>(status_) << "\n";
    oss << "  Optimization: " << static_cast<int>(optimization_level_) << "\n";
    
    return oss.str();
}

std::string CompiledFunction::get_statistics_string() const {
    std::shared_lock lock(mutex_);
    
    std::ostringstream oss;
    oss << "Function Statistics:\n";
    oss << "  Executions: " << stats_.execution_count << "\n";
    oss << "  Total cycles: " << stats_.total_cycles << "\n";
    oss << "  Average cycles: " << std::fixed << std::setprecision(2) 
        << stats_.average_cycles << "\n";
    oss << "  Cache hit ratio: " << (stats_.cache_hit_ratio() * 100.0) << "%\n";
    oss << "  Recompilations: " << stats_.recompilation_count << "\n";
    oss << "  Exceptions: " << stats_.exception_count << "\n";
    oss << "  Hot: " << (stats_.is_hot() ? "yes" : "no") << "\n";
    
    return oss.str();
}

std::string CompiledFunction::get_inlining_info_string() const {
    std::shared_lock lock(mutex_);
    
    std::ostringstream oss;
    oss << "Inlining Information:\n";
    oss << "  Inlined functions: " << inlining_info_.inlined_functions.size() << "\n";
    oss << "  Total inlined size: " << inlining_info_.total_inlined_size << " bytes\n";
    oss << "  Total savings: " << inlining_info_.total_savings << " cycles\n";
    
    for (const auto& inline_info : inlining_info_.inlined_functions) {
        oss << "    Function " << static_cast<uint32_t>(inline_info.function_id) 
            << " at 0x" << std::hex << inline_info.call_site << "\n";
    }
    
    return oss.str();
}

void CompiledFunction::link_to(CompiledFunction* target, uint32_t branch_pc) {
    std::unique_lock lock(mutex_);
    
    if (!target) return;
    
    // Linking functionality would patch branches to directly jump to target
    flags_ = flags_ | FunctionFlags::Linked;
    log_compilation("Linked to target function");
}

void CompiledFunction::unlink_from(uint32_t branch_pc) {
    std::unique_lock lock(mutex_);
    
    // Would remove direct link at branch_pc
    log_compilation("Unlinked from branch");
}

CompiledFunction::SerializedFunction CompiledFunction::serialize() const {
    std::shared_lock lock(mutex_);
    
    SerializedFunction data;
    data.id = id_;
    data.metadata = metadata_;
    data.stats = stats_;
    data.snapshot = memory_snapshot_;
    data.flags = flags_;
    data.status = status_;
    
    // Copy native code
    if (code_ptr_ && code_size_ > 0) {
        data.native_code.resize(code_size_);
        std::memcpy(data.native_code.data(), code_ptr_, code_size_);
    }
    
    return data;
}

bool CompiledFunction::deserialize(const SerializedFunction& data) {
    std::unique_lock lock(mutex_);
    
    id_ = data.id;
    metadata_ = data.metadata;
    stats_ = data.stats;
    memory_snapshot_ = data.snapshot;
    flags_ = data.flags;
    status_ = data.status;
    
    // Restore native code
    if (!data.native_code.empty()) {
        code_size_ = data.native_code.size();
        // Would allocate executable memory and copy code
    }
    
    return true;
}

std::optional<CompiledFunction::CompilationContext> CompiledFunction::analyze_function() {
    CompilationContext context(CompileOptions{});
    
    // Simple function analysis - trace from entry point
    uint32_t current_pc = entry_pc();
    context.visited_addresses.insert(current_pc);
    
    // Simplified: assume function is small for now
    metadata_.min_pc = current_pc;
    metadata_.max_pc = current_pc + 0x100;  // Assume 64 instructions max
    
    context.total_instructions = 64;  // Simplified
    context.basic_blocks.push_back(current_pc);
    
    return context;
}

bool CompiledFunction::generate_native_code(const CompilationContext& context) {
    // Simplified code generation
    // Real implementation would use AsmJit to generate x86/x64 code
    
    // Allocate code buffer (simplified)
    code_size_ = 1024;  // Placeholder
    // code_ptr_ would point to executable memory
    
    metadata_.native_code_size = code_size_;
    
    return true;
}

bool CompiledFunction::optimize_native_code(const CompilationContext& context) {
    // Apply optimizations to native code
    // Simplified version
    
    if (context.options.optimization >= OptimizationLevel::Aggressive) {
        // Apply aggressive optimizations
        log_compilation("Applied aggressive optimizations");
    }
    
    return true;
}

bool CompiledFunction::can_inline_function(FunctionId function_id, uint32_t call_site) const {
    if (inlining_info_.total_inlined_size >= config_.max_inline_size) {
        return false;
    }
    
    // Additional checks would go here
    return true;
}

size_t CompiledFunction::estimate_inline_savings(FunctionId function_id) const {
    // Estimate cycle savings from inlining
    // Simplified: assume fixed savings
    return 10;  // 10 cycles saved per inline
}

bool CompiledFunction::should_specialize(const SpecializationContext& context) const {
    if (specializations_.size() >= config_.max_specializations) {
        return false;
    }
    
    // Check if specialization would be beneficial
    return stats_.is_hot();
}

SpecializationContext CompiledFunction::extract_context() const {
    SpecializationContext context;
    // Would extract current execution context
    return context;
}

void CompiledFunction::capture_memory_snapshot() {
    memory_snapshot_ = MemorySnapshot{};
    
    // Would capture memory values at key addresses
    // Simplified version doesn't actually capture
}

void CompiledFunction::update_flags() {
    if (metadata_.instruction_count > 100) {
        flags_ = flags_ | FunctionFlags::HasLoops;
    }
    
    if (stats_.is_hot()) {
        flags_ = flags_ | FunctionFlags::Hot;
    } else if (stats_.is_cold()) {
        flags_ = flags_ | FunctionFlags::Cold;
    }
}

void CompiledFunction::update_hash() {
    // Calculate hash of function code
    // Simplified: use entry PC as hash
    hash_ = static_cast<uint64_t>(entry_pc());
    metadata_.hash = hash_;
}

void CompiledFunction::log_compilation(const std::string& message) {
    debug_log_ += message + "\n";
    // TRACE(TraceLevel::Info, "Function {}: {}", static_cast<uint32_t>(id_), message);
    Log::infof("Function %u: %s", static_cast<uint32_t>(id_), message.c_str());
}

void CompiledFunction::set_status(CompilationStatus status) {
    status_ = status;
}

// ============================================================================
// FunctionManager Implementation
// ============================================================================

FunctionManager::FunctionManager(Recompiler& recompiler, const Config& config)
    : recompiler_(recompiler)
    , config_(config)
{
    functions_.reserve(config_.max_functions);
}

FunctionManager::~FunctionManager() {
    clear();
}

CompiledFunction* FunctionManager::get_or_compile(uint32_t entry_pc, 
                                                   const CompiledFunction::CompileOptions& options) {
    // Try to get existing function
    {
        std::shared_lock lock(mutex_);
        auto it = pc_to_function_id_.find(entry_pc);
        if (it != pc_to_function_id_.end()) {
            auto& entry = functions_.at(it->second);
            touch(it->second);
            stats_.total_compilation_time++;
            return entry.function.get();
        }
    }
    
    // Compile new function
    std::unique_lock lock(mutex_);
    
    auto function = std::make_unique<CompiledFunction>(recompiler_, entry_pc);
    FunctionId id = generate_id();
    
    // Compile the function
    auto result = function->compile(options);
    if (result != CompiledFunction::CompileResult::Success) {
        return nullptr;
    }
    
    CompiledFunction* func_ptr = function.get();
    
    uint64_t timestamp = 0;  // Would get from system
    functions_.emplace(id, FunctionEntry(std::move(function), timestamp));
    pc_to_function_id_[entry_pc] = id;
    
    ++stats_.total_functions;
    ++stats_.compiled_functions;
    
    return func_ptr;
}

CompiledFunction* FunctionManager::get(uint32_t entry_pc) const {
    std::shared_lock lock(mutex_);
    
    auto it = pc_to_function_id_.find(entry_pc);
    if (it == pc_to_function_id_.end()) {
        return nullptr;
    }
    
    auto func_it = functions_.find(it->second);
    if (func_it == functions_.end()) {
        return nullptr;
    }
    
    return func_it->second.function.get();
}

CompiledFunction* FunctionManager::get_by_id(FunctionId id) const {
    std::shared_lock lock(mutex_);
    
    auto it = functions_.find(id);
    if (it == functions_.end()) {
        return nullptr;
    }
    
    return it->second.function.get();
}

void FunctionManager::insert(std::unique_ptr<CompiledFunction> function) {
    std::unique_lock lock(mutex_);
    
    FunctionId id = generate_id();
    uint32_t entry_pc = function->entry_pc();
    uint64_t timestamp = 0;
    
    functions_.emplace(id, FunctionEntry(std::move(function), timestamp));
    pc_to_function_id_[entry_pc] = id;
    
    ++stats_.total_functions;
}

void FunctionManager::remove(FunctionId id) {
    std::unique_lock lock(mutex_);
    
    auto it = functions_.find(id);
    if (it != functions_.end()) {
        uint32_t entry_pc = it->second.function->entry_pc();
        pc_to_function_id_.erase(entry_pc);
        functions_.erase(it);
        --stats_.total_functions;
    }
}

void FunctionManager::remove(uint32_t entry_pc) {
    std::unique_lock lock(mutex_);
    
    auto it = pc_to_function_id_.find(entry_pc);
    if (it != pc_to_function_id_.end()) {
        functions_.erase(it->second);
        pc_to_function_id_.erase(it);
        --stats_.total_functions;
    }
}

void FunctionManager::invalidate(uint32_t address) {
    std::unique_lock lock(mutex_);
    
    // Find all functions containing this address
    for (auto& [id, entry] : functions_) {
        if (entry.function->metadata().contains(address)) {
            entry.function->invalidate_if_modified();
        }
    }
}

void FunctionManager::invalidate_range(uint32_t start, uint32_t end) {
    std::unique_lock lock(mutex_);
    
    for (auto& [id, entry] : functions_) {
        const auto& metadata = entry.function->metadata();
        if (metadata.min_pc <= end && metadata.max_pc >= start) {
            entry.function->invalidate_if_modified();
        }
    }
}

void FunctionManager::clear() {
    std::unique_lock lock(mutex_);
    
    functions_.clear();
    pc_to_function_id_.clear();
    hot_functions_.clear();
    stats_ = ManagerStats{};
}

void FunctionManager::optimize_all() {
    std::shared_lock lock(mutex_);
    
    for (auto& [id, entry] : functions_) {
        entry.function->optimize();
    }
}

void FunctionManager::profile_all() {
    std::shared_lock lock(mutex_);
    
    // Collect profiling data from all functions
    for (const auto& [id, entry] : functions_) {
        [[maybe_unused]] const auto& stats = entry.function->statistics();
        // Would aggregate profiling data
    }
}

void FunctionManager::invalidate_modified_functions() {
    std::shared_lock lock(mutex_);
    
    for (auto& [id, entry] : functions_) {
        entry.function->invalidate_if_modified();
    }
}

std::vector<FunctionId> FunctionManager::find_functions_containing(uint32_t pc) const {
    std::shared_lock lock(mutex_);
    
    std::vector<FunctionId> result;
    for (const auto& [id, entry] : functions_) {
        if (entry.function->metadata().contains(pc)) {
            result.push_back(id);
        }
    }
    
    return result;
}

std::vector<FunctionId> FunctionManager::find_caller_functions(FunctionId callee) const {
    std::shared_lock lock(mutex_);
    
    std::vector<FunctionId> result;
    
    auto callee_it = functions_.find(callee);
    if (callee_it == functions_.end()) {
        return result;
    }
    
    uint32_t callee_pc = callee_it->second.function->entry_pc();
    
    // Find functions that call this function
    for (const auto& [id, entry] : functions_) {
        const auto& called = entry.function->metadata().called_functions;
        if (std::find(called.begin(), called.end(), callee_pc) != called.end()) {
            result.push_back(id);
        }
    }
    
    return result;
}

std::vector<FunctionId> FunctionManager::find_callee_functions(FunctionId caller) const {
    std::shared_lock lock(mutex_);
    
    std::vector<FunctionId> result;
    
    auto caller_it = functions_.find(caller);
    if (caller_it == functions_.end()) {
        return result;
    }
    
    const auto& called_pcs = caller_it->second.function->metadata().called_functions;
    
    // Convert PCs to function IDs
    for (uint32_t pc : called_pcs) {
        auto it = pc_to_function_id_.find(pc);
        if (it != pc_to_function_id_.end()) {
            result.push_back(it->second);
        }
    }
    
    return result;
}

void FunctionManager::promote_to_hot(FunctionId id) {
    std::unique_lock lock(mutex_);
    
    auto it = functions_.find(id);
    if (it != functions_.end() && !it->second.is_hot) {
        it->second.is_hot = true;
        hot_functions_.insert(id);
        ++hot_function_count_;
        ++stats_.hot_functions;
    }
}

void FunctionManager::demote_from_hot(FunctionId id) {
    std::unique_lock lock(mutex_);
    
    auto it = functions_.find(id);
    if (it != functions_.end() && it->second.is_hot) {
        it->second.is_hot = false;
        hot_functions_.erase(id);
        --hot_function_count_;
        --stats_.hot_functions;
    }
}

std::vector<FunctionId> FunctionManager::get_hot_functions() const {
    std::shared_lock lock(mutex_);
    return std::vector<FunctionId>(hot_functions_.begin(), hot_functions_.end());
}

std::vector<FunctionId> FunctionManager::get_cold_functions() const {
    std::shared_lock lock(mutex_);
    
    std::vector<FunctionId> result;
    for (const auto& [id, entry] : functions_) {
        if (entry.function->statistics().is_cold() && !entry.is_hot) {
            result.push_back(id);
        }
    }
    
    return result;
}

bool FunctionManager::fuse_functions(FunctionId f1, FunctionId f2) {
    std::unique_lock lock(mutex_);
    
    if (!config_.enable_function_fusion) {
        return false;
    }
    
    if (!functions_can_fuse(f1, f2)) {
        return false;
    }
    
    auto fused = fuse_functions_impl(f1, f2);
    if (fused) {
        // Replace f1 with fused, remove f2
        ++stats_.fused_functions;
        return true;
    }
    
    return false;
}

FunctionManager::ManagerStats FunctionManager::statistics() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void FunctionManager::reset_statistics() {
    std::lock_guard lock(stats_mutex_);
    stats_ = ManagerStats{};
}

std::string FunctionManager::ManagerStats::to_string() const {
    std::ostringstream oss;
    oss << "Function Manager Statistics:\n";
    oss << "  Total functions: " << total_functions << "\n";
    oss << "  Compiled: " << compiled_functions << "\n";
    oss << "  Optimized: " << optimized_functions << "\n";
    oss << "  Hot: " << hot_functions << ", Cold: " << cold_functions << "\n";
    oss << "  Specialized: " << specialized_functions << "\n";
    oss << "  Inlined: " << inlined_functions << "\n";
    oss << "  Fused: " << fused_functions << "\n";
    oss << "  Total native code: " << total_native_code_size << " bytes\n";
    oss << "  Average compilation time: " << average_compilation_time() << " ns\n";
    return oss.str();
}

FunctionManager::SerializedManager FunctionManager::serialize() const {
    std::shared_lock lock(mutex_);
    
    SerializedManager data;
    data.stats = stats_;
    data.config = config_;
    
    for (const auto& [id, entry] : functions_) {
        SerializedManager::SerializedFunctionEntry se;
        se.function = entry.function->serialize();
        se.is_hot = entry.is_hot;
        data.functions.push_back(se);
    }
    
    return data;
}

bool FunctionManager::deserialize(const SerializedManager& data) {
    std::unique_lock lock(mutex_);
    
    clear();
    stats_ = data.stats;
    config_ = data.config;
    
    for (const auto& se : data.functions) {
        auto function = std::make_unique<CompiledFunction>(recompiler_, 
                                                           se.function.metadata.entry_pc);
        if (function->deserialize(se.function)) {
            FunctionId id = se.function.id;
            uint64_t timestamp = 0;
            
            FunctionEntry entry(std::move(function), timestamp);
            entry.is_hot = se.is_hot;
            
            functions_[id] = std::move(entry);
            pc_to_function_id_[se.function.metadata.entry_pc] = id;
            
            if (se.is_hot) {
                hot_functions_.insert(id);
            }
        }
    }
    
    return true;
}

size_t FunctionManager::native_code_size() const {
    std::shared_lock lock(mutex_);
    
    size_t total = 0;
    for (const auto& [id, entry] : functions_) {
        total += entry.function->code_size();
    }
    
    return total;
}

FunctionId FunctionManager::generate_id() {
    static std::atomic<uint32_t> next_id{1};
    return static_cast<FunctionId>(next_id.fetch_add(1));
}

void FunctionManager::touch(FunctionId id) {
    auto it = functions_.find(id);
    if (it != functions_.end()) {
        ++it->second.access_count;
        it->second.last_access = 0;  // Would be timestamp
        
        // Check if should promote to hot
        if (should_promote_to_hot(id)) {
            promote_to_hot(id);
        }
    }
}

void FunctionManager::evict_cold_functions() {
    // Evict least recently used cold functions if cache is full
    if (functions_.size() < config_.max_functions) {
        return;
    }
    
    std::vector<std::pair<FunctionId, uint64_t>> candidates;
    for (const auto& [id, entry] : functions_) {
        if (!entry.is_hot) {
            candidates.push_back({id, entry.last_access});
        }
    }
    
    // Sort by last access time
    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; }
    );
    
    // Remove oldest
    if (!candidates.empty()) {
        remove(candidates.front().first);
    }
}

bool FunctionManager::should_promote_to_hot(FunctionId id) const {
    auto it = functions_.find(id);
    if (it == functions_.end() || it->second.is_hot) {
        return false;
    }
    
    // Promote if accessed frequently
    return it->second.access_count > 100 &&
           it->second.function->statistics().is_hot();
}

bool FunctionManager::functions_can_fuse(FunctionId f1, FunctionId f2) const {
    auto it1 = functions_.find(f1);
    auto it2 = functions_.find(f2);
    
    if (it1 == functions_.end() || it2 == functions_.end()) {
        return false;
    }
    
    // Check if functions are adjacent or can be fused
    const auto& meta1 = it1->second.function->metadata();
    const auto& meta2 = it2->second.function->metadata();
    
    return meta1.max_pc + 4 == meta2.min_pc;
}

std::unique_ptr<CompiledFunction> FunctionManager::fuse_functions_impl(FunctionId f1, 
                                                                        FunctionId f2) {
    // Create new fused function
    // Simplified implementation
    return nullptr;
}

// ============================================================================
// FunctionAnalyzer Implementation
// ============================================================================

FunctionAnalyzer::FunctionAnalyzer() {
}

FunctionAnalyzer::AnalysisResult FunctionAnalyzer::analyze(
    const CompiledFunction& function) const {
    std::shared_lock lock(mutex_);
    
    AnalysisResult result;
    
    const auto& metadata = function.metadata();
    const auto& stats = function.statistics();
    
    // Determine optimization potential
    result.can_inline = metadata.instruction_count < 50 && 
                       !has_flag(function.flags(), FunctionFlags::HasExceptions);
    
    result.can_specialize = stats.is_hot() && 
                           metadata.instruction_count < 200;
    
    result.has_side_effects = has_flag(function.flags(), FunctionFlags::HasMemoryOps);
    result.is_pure = !result.has_side_effects;
    
    result.estimated_cycles = metadata.instruction_count * 3;  // Rough estimate
    result.estimated_instructions = metadata.instruction_count;
    
    return result;
}

FunctionAnalyzer::AnalysisResult FunctionAnalyzer::analyze(uint32_t entry_pc, 
                                                           const MipsMemory& memory) const {
    std::shared_lock lock(mutex_);
    
    AnalysisResult result;
    
    // Analyze function starting at entry_pc
    // Simplified analysis
    result.estimated_instructions = 50;
    result.estimated_cycles = 150;
    
    return result;
}

std::string FunctionAnalyzer::AnalysisResult::to_string() const {
    std::ostringstream oss;
    oss << "Analysis Result:\n";
    oss << "  Can inline: " << (can_inline ? "yes" : "no") << "\n";
    oss << "  Can specialize: " << (can_specialize ? "yes" : "no") << "\n";
    oss << "  Can vectorize: " << (can_vectorize ? "yes" : "no") << "\n";
    oss << "  Has side effects: " << (has_side_effects ? "yes" : "no") << "\n";
    oss << "  Is pure: " << (is_pure ? "yes" : "no") << "\n";
    oss << "  Estimated cycles: " << estimated_cycles << "\n";
    oss << "  Estimated instructions: " << estimated_instructions << "\n";
    return oss.str();
}

bool FunctionAnalyzer::is_memory_copy_pattern(uint32_t start_pc, 
                                              const MipsMemory& memory) const {
    std::shared_lock lock(mutex_);
    return matches_memory_copy(start_pc, memory);
}

bool FunctionAnalyzer::is_memory_set_pattern(uint32_t start_pc,
                                             const MipsMemory& memory) const {
    std::shared_lock lock(mutex_);
    return matches_memory_set(start_pc, memory);
}

bool FunctionAnalyzer::is_math_function_pattern(uint32_t start_pc,
                                               const MipsMemory& memory) const {
    std::shared_lock lock(mutex_);
    return matches_simple_math(start_pc, memory);
}

bool FunctionAnalyzer::is_string_function_pattern(uint32_t start_pc,
                                                  const MipsMemory& memory) const {
    // Check for string manipulation patterns
    return false;
}

size_t FunctionAnalyzer::estimate_inline_cost(const CompiledFunction& function) const {
    return function.metadata().instruction_count * 2;
}

size_t FunctionAnalyzer::estimate_specialization_benefit(
    const CompiledFunction& function,
    const SpecializationContext& context) const {
    // Estimate benefit from specialization
    size_t base_cost = function.statistics().average_cycles;
    size_t specialized_cost = base_cost * 0.7;  // 30% improvement estimate
    return base_cost - specialized_cost;
}

FunctionAnalyzer::CallGraph FunctionAnalyzer::build_call_graph(
    const FunctionManager& manager) const {
    CallGraph graph;
    
    // Build call graph from all functions
    for (const auto& [id, entry] : manager) {
        graph.add_function(id);
    }
    
    // Add call edges
    for (const auto& [id, entry] : manager) {
        const auto& callees = entry.function->metadata().called_functions;
        for (uint32_t callee_pc : callees) {
            auto* callee = manager.get(callee_pc);
            if (callee) {
                graph.add_call(id, callee->id());
            }
        }
    }
    
    graph.analyze();
    return graph;
}

bool FunctionAnalyzer::has_side_effects(uint32_t pc, const MipsMemory& memory,
                                       std::unordered_set<uint32_t>& visited) const {
    // Simplified check for side effects
    return true;
}

bool FunctionAnalyzer::is_pure_function(uint32_t pc, const MipsMemory& memory,
                                       std::unordered_set<uint32_t>& visited) const {
    return !has_side_effects(pc, memory, visited);
}

bool FunctionAnalyzer::matches_memory_copy(uint32_t pc, const MipsMemory& memory) const {
    // Pattern matching for memcpy-like functions
    return false;
}

bool FunctionAnalyzer::matches_memory_set(uint32_t pc, const MipsMemory& memory) const {
    // Pattern matching for memset-like functions
    return false;
}

bool FunctionAnalyzer::matches_simple_math(uint32_t pc, const MipsMemory& memory) const {
    // Pattern matching for simple math functions
    return false;
}

void FunctionAnalyzer::CallGraph::add_function(FunctionId id) {
    nodes[id].id = id;
}

void FunctionAnalyzer::CallGraph::add_call(FunctionId caller, FunctionId callee) {
    nodes[caller].calls.push_back(callee);
    nodes[callee].called_by.push_back(caller);
}

void FunctionAnalyzer::CallGraph::analyze() {
    // Analyze call graph for recursive functions, depth, etc.
    for (auto& [id, node] : nodes) {
        // Check if function calls itself (direct recursion)
        if (std::find(node.calls.begin(), node.calls.end(), id) != node.calls.end()) {
            node.recursive = true;
        }
    }
}

std::vector<FunctionId> FunctionAnalyzer::CallGraph::get_recursive_functions() const {
    std::vector<FunctionId> result;
    for (const auto& [id, node] : nodes) {
        if (node.recursive) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<std::vector<FunctionId>> FunctionAnalyzer::CallGraph::get_call_chains(
    FunctionId start, FunctionId end) const {
    // Find all call paths from start to end
    std::vector<std::vector<FunctionId>> chains;
    // Simplified: would use BFS/DFS to find paths
    return chains;
}

size_t FunctionAnalyzer::CallGraph::get_max_depth() const {
    size_t max_depth = 0;
    for (const auto& [id, node] : nodes) {
        max_depth = std::max(max_depth, node.depth);
    }
    return max_depth;
}

} // namespace Nova64::Recompiler
