/**
 * @file Span.hpp
 * @brief Span declarations and types.
 */
#pragma once
#include <cstddef>
#include <type_traits>
#include <array>
#include <vector>

namespace Common {

// Define dynamic_extent locally to avoid <span dependency
inline constexpr std::size_t dynamic_extent = static_cast<std::size_t>(-1);

template <typename T, std::size_t Extent = dynamic_extent>
class Span {
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using iterator = pointer;

    constexpr Span() noexcept : m_data(nullptr), m_size(0) {}
    constexpr Span(pointer ptr, size_type count) : m_data(ptr), m_size(count) {}
    constexpr Span(pointer first, pointer last) : m_data(first), m_size(last - first) {}
    
    template <std::size_t N>
    constexpr Span(element_type (&arr)[N]) noexcept : m_data(arr), m_size(N) {}
    
    // Support for std::array
    template <std::size_t N>
    constexpr Span(std::array<value_type, N>& arr) noexcept : m_data(arr.data()), m_size(N) {}

    template <std::size_t N>
    constexpr Span(const std::array<value_type, N>& arr) noexcept : m_data(arr.data()), m_size(N) {}
    
    // Support for std::vector
    constexpr Span(std::vector<value_type>& vec) noexcept : m_data(vec.data()), m_size(vec.size()) {}
    constexpr Span(const std::vector<value_type>& vec) noexcept : m_data(vec.data()), m_size(vec.size()) {}

    constexpr pointer data() const noexcept { return m_data; }
    constexpr size_type size() const noexcept { return m_size; }
    constexpr bool empty() const noexcept { return m_size == 0; }
    constexpr reference operator[](size_type idx) const { return m_data[idx]; }
    
    constexpr iterator begin() const noexcept { return m_data; }
    constexpr iterator end() const noexcept { return m_data + m_size; }

private:
    pointer m_data;
    size_type m_size;
};

}
