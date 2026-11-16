/**
 * @file SectionInfo.hpp
 * @brief Section information structures for code blocks in dynamic recompilation
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This file defines data structures that track relationships between code sections
 * and their parent/child connections, essential for control flow analysis and
 * optimization during dynamic recompilation.
 */

#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <algorithm>
#include <cassert>

namespace Nova64 {
namespace Recompiler {

class CCodeSection;
class CodeBlock;
class CJumpInfo;

/**
 * @enum ParentRelationshipType
 * @brief Type of relationship between sections
 */
enum class ParentRelationshipType : uint8_t {
    DirectJump,        ///< Direct jump/branch
    ConditionalBranch, ///< Conditional branch (taken path)
    FallThrough,       ///< Fall-through execution
    ExceptionHandler,  ///< Exception/Interrupt handler
    IndirectJump,      ///< Indirect/jump via register
    Link,              ///< Function call with return
    LoopBackEdge,      ///< Back edge in a loop
    Unreachable        ///< Theoretical connection (for analysis)
};

/**
 * @struct BlockParent
 * @brief Represents a parent section and its associated jump information
 * 
 * This structure tracks the relationship between a code section and its
 * parent, including the specific jump that connects them. This is crucial
 * for control flow analysis and register state propagation.
 */
struct BlockParent {
    /// Constructs an empty parent relationship
    BlockParent() noexcept = default;

    /// Constructs a parent relationship with given section and jump info
    BlockParent(CCodeSection* parent, CJumpInfo* jumpInfo,
                ParentRelationshipType type = ParentRelationshipType::DirectJump) noexcept :
        parent(parent), jumpInfo(jumpInfo), relationshipType(type) {}

    /**
     * @brief Check if this parent relationship is valid
     * @return true if both parent and jumpInfo are non-null
     */
    bool IsValid() const noexcept { return parent != nullptr && jumpInfo != nullptr; }

    bool IsConditional() const noexcept { return relationshipType == ParentRelationshipType::ConditionalBranch; }

    bool IsFallThrough() const noexcept { return relationshipType == ParentRelationshipType::FallThrough; }

    bool IsLoopBackEdge() const noexcept { return relationshipType == ParentRelationshipType::LoopBackEdge; }

    /**
     * @brief Get the jump target address from the jump info
     * @return Target address if available, empty optional otherwise
     */
    std::optional<uint32_t> GetTargetAddress() const noexcept;

    /**
     * @brief Get the jump source address from the jump info
     * @return Source address if available, empty optional otherwise
     */
    std::optional<uint32_t> GetSourceAddress() const noexcept;

    CCodeSection* parent = nullptr;                                               ///< Pointer to parent section
    CJumpInfo* jumpInfo = nullptr;                                                ///< Associated jump information
    ParentRelationshipType relationshipType = ParentRelationshipType::DirectJump; ///< Type of connection

    bool operator==(const BlockParent& other) const noexcept {
        return parent == other.parent && jumpInfo == other.jumpInfo && relationshipType == other.relationshipType;
    }

    bool operator!=(const BlockParent& other) const noexcept { return !(*this == other); }

    std::string ToString() const;
};

/**
 * @class BlockParentList
 * @brief List of parent relationships for a code section
 * 
 * This class provides a specialized container for managing parent relationships
 * with efficient lookup operations and validation.
 */
class BlockParentList {
public:
    using value_type = BlockParent;
    using iterator = std::vector<BlockParent>::iterator;
    using const_iterator = std::vector<BlockParent>::const_iterator;
    using size_type = std::vector<BlockParent>::size_type;

    BlockParentList() = default;
    ~BlockParentList() = default;

    // Allow copying for analysis purposes
    BlockParentList(const BlockParentList&) = default;
    BlockParentList& operator=(const BlockParentList&) = default;
    BlockParentList(BlockParentList&&) = default;
    BlockParentList& operator=(BlockParentList&&) = default;

    // Iterator support
    iterator begin() noexcept { return m_parents.begin(); }
    iterator end() noexcept { return m_parents.end(); }
    const_iterator begin() const noexcept { return m_parents.begin(); }
    const_iterator end() const noexcept { return m_parents.end(); }
    const_iterator cbegin() const noexcept { return m_parents.cbegin(); }
    const_iterator cend() const noexcept { return m_parents.cend(); }

    // Capacity
    bool empty() const noexcept { return m_parents.empty(); }
    size_type size() const noexcept { return m_parents.size(); }
    size_type capacity() const noexcept { return m_parents.capacity(); }
    void reserve(size_type count) { m_parents.reserve(count); }

    // Element access
    value_type& operator[](size_type index) noexcept {
        assert(index < size());
        return m_parents[index];
    }

    const value_type& operator[](size_type index) const noexcept {
        assert(index < size());
        return m_parents[index];
    }

    value_type& front() noexcept {
        assert(!empty());
        return m_parents.front();
    }

    const value_type& front() const noexcept {
        assert(!empty());
        return m_parents.front();
    }

    value_type& back() noexcept {
        assert(!empty());
        return m_parents.back();
    }

    const value_type& back() const noexcept {
        assert(!empty());
        return m_parents.back();
    }

    // Modifiers
    void clear() noexcept { m_parents.clear(); }

    void push_back(const BlockParent& parent) {
        // Don't add duplicate parent relationships
        if (!Contains(parent.parent)) {
            m_parents.push_back(parent);
        }
    }

    void push_back(BlockParent&& parent) {
        // Don't add duplicate parent relationships
        if (!Contains(parent.parent)) {
            m_parents.push_back(std::move(parent));
        }
    }

    template <typename... Args> void emplace_back(Args&&... args) {
        BlockParent parent(std::forward<Args>(args)...);
        if (!Contains(parent.parent)) {
            m_parents.emplace_back(std::move(parent));
        }
    }

    void pop_back() {
        assert(!empty());
        m_parents.pop_back();
    }

    iterator erase(const_iterator position) { return m_parents.erase(position); }

    iterator erase(const_iterator first, const_iterator last) { return m_parents.erase(first, last); }

    // Lookup operations
    bool Contains(const CCodeSection* section) const noexcept { return Find(section) != m_parents.end(); }

    const_iterator Find(const CCodeSection* section) const noexcept {
        return std::find_if(m_parents.begin(), m_parents.end(),
                            [section](const BlockParent& parent) { return parent.parent == section; });
    }

    iterator Find(const CCodeSection* section) noexcept {
        return std::find_if(m_parents.begin(), m_parents.end(),
                            [section](const BlockParent& parent) { return parent.parent == section; });
    }

    const_iterator FindByJumpInfo(const CJumpInfo* jumpInfo) const noexcept {
        return std::find_if(m_parents.begin(), m_parents.end(),
                            [jumpInfo](const BlockParent& parent) { return parent.jumpInfo == jumpInfo; });
    }

    BlockParentList GetParentsByType(ParentRelationshipType type) const {
        BlockParentList result;
        for (const auto& parent : m_parents) {
            if (parent.relationshipType == type) {
                result.push_back(parent);
            }
        }
        return result;
    }

    bool HasConditionalParent() const noexcept {
        return std::any_of(m_parents.begin(), m_parents.end(),
                           [](const BlockParent& parent) { return parent.IsConditional(); });
    }

    bool HasLoopBackEdge() const noexcept {
        return std::any_of(m_parents.begin(), m_parents.end(),
                           [](const BlockParent& parent) { return parent.IsLoopBackEdge(); });
    }

    void RemoveByType(ParentRelationshipType type) {
        m_parents.erase(std::remove_if(m_parents.begin(), m_parents.end(),
                                       [type](const BlockParent& parent) { return parent.relationshipType == type; }),
                        m_parents.end());
    }

    /**
     * @brief Remove duplicate parent entries (by section)
     */
    void RemoveDuplicates() {
        // Sort by parent pointer to group duplicates
        std::sort(m_parents.begin(), m_parents.end(),
                  [](const BlockParent& a, const BlockParent& b) { return a.parent < b.parent; });

        // Remove consecutive duplicates
        auto last = std::unique(m_parents.begin(), m_parents.end(),
                                [](const BlockParent& a, const BlockParent& b) { return a.parent == b.parent; });

        m_parents.erase(last, m_parents.end());
    }

    template <typename Func> void ForEach(Func&& func) const {
        std::for_each(m_parents.begin(), m_parents.end(), std::forward<Func>(func));
    }

    template <typename Func> void ForEach(Func&& func) {
        std::for_each(m_parents.begin(), m_parents.end(), std::forward<Func>(func));
    }

    /**
     * @brief Validate all parent relationships
     * @return true if all relationships are valid
     */
    bool Validate() const noexcept {
        return std::all_of(m_parents.begin(), m_parents.end(),
                           [](const BlockParent& parent) { return parent.IsValid(); });
    }

    struct Statistics {
        size_t totalParents = 0;
        size_t conditionalParents = 0;
        size_t loopBackEdges = 0;
        size_t maxParentsInChain = 0;
    };

    Statistics GetStatistics() const;

private:
    std::vector<BlockParent> m_parents;
};

// Alias for backward compatibility (deprecated)
using BLOCK_PARENT = BlockParent;
using BLOCK_PARENT_LIST [[deprecated("Use BlockParentList instead")]] = BlockParentList;

} // namespace Recompiler
} // namespace Nova64
