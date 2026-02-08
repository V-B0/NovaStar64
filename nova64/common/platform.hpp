/**
 * @file platform.hpp
 * @brief Cross-platform compatibility macros and declarations
 *
 * Provides compatibility shims for Windows-specific functions on POSIX systems
 * and other platform abstraction utilities.
 */

#pragma once

#include <cstdint>
#include <cstddef>

// =============================================================================
// Platform Detection
// =============================================================================

#if defined(_WIN32) || defined(_WIN64)
    #define PLATFORM_WINDOWS 1
    #define PLATFORM_POSIX 0
#elif defined(__linux__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_POSIX 1
    #define PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_POSIX 1
    #define PLATFORM_MACOS 1
#elif defined(__unix__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_POSIX 1
#else
    #error "Unsupported platform"
#endif

// =============================================================================
// Architecture Detection
// =============================================================================

#if defined(_M_X64) || defined(__x86_64__)
    #define ARCH_X64 1
    #define ARCH_X86 0
    #define ARCH_ARM64 0
#elif defined(_M_IX86) || defined(__i386__)
    #define ARCH_X64 0
    #define ARCH_X86 1
    #define ARCH_ARM64 0
#elif defined(_M_ARM64) || defined(__aarch64__)
    #define ARCH_X64 0
    #define ARCH_X86 0
    #define ARCH_ARM64 1
#else
    #define ARCH_X64 0
    #define ARCH_X86 0
    #define ARCH_ARM64 0
#endif

// =============================================================================
// Compiler Detection
// =============================================================================

#if defined(_MSC_VER)
    #define COMPILER_MSVC 1
    #define COMPILER_GCC 0
    #define COMPILER_CLANG 0
#elif defined(__clang__)
    #define COMPILER_MSVC 0
    #define COMPILER_GCC 0
    #define COMPILER_CLANG 1
#elif defined(__GNUC__)
    #define COMPILER_MSVC 0
    #define COMPILER_GCC 1
    #define COMPILER_CLANG 0
#else
    #define COMPILER_MSVC 0
    #define COMPILER_GCC 0
    #define COMPILER_CLANG 0
#endif

// =============================================================================
// Function Attributes
// =============================================================================

#if COMPILER_MSVC
    #define FORCE_INLINE __forceinline
    #define NO_INLINE __declspec(noinline)
    #define ALIGN(n) __declspec(align(n))
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
    #define UNREACHABLE() __assume(0)
    #define EXPORT __declspec(dllexport)
    #define IMPORT __declspec(dllimport)
#else
    #define FORCE_INLINE __attribute__((always_inline)) inline
    #define NO_INLINE __attribute__((noinline))
    #define ALIGN(n) __attribute__((aligned(n)))
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define UNREACHABLE() __builtin_unreachable()
    #define EXPORT __attribute__((visibility("default")))
    #define IMPORT
#endif

// =============================================================================
// Debug Break
// =============================================================================

#if COMPILER_MSVC
    #define DEBUG_BREAK() __debugbreak()
#elif COMPILER_GCC || COMPILER_CLANG
    #if ARCH_X64 || ARCH_X86
        #define DEBUG_BREAK() __asm__ volatile("int $3")
    #elif ARCH_ARM64
        #define DEBUG_BREAK() __builtin_trap()
    #else
        #include <signal.h>
        #define DEBUG_BREAK() raise(SIGTRAP)
    #endif
#else
    #include <signal.h>
    #define DEBUG_BREAK() raise(SIGTRAP)
#endif

// =============================================================================
// POSIX Compatibility Layer
// =============================================================================

#ifndef _WIN32
#include <alloca.h>
#include <cstdarg>
#include <cstring>
#include <cmath>

#define stricmp strcasecmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define _isnan std::isnan

/**
 * @brief Calculate the number of characters needed for formatted output
 * @param format Printf-style format string
 * @param pargs Variable argument list
 * @return Number of characters that would be written (excluding null terminator)
 */
int _vscprintf(const char* format, va_list pargs);

/**
 * @brief Safe string copy
 * @param dest Destination buffer
 * @param dest_size Size of destination buffer
 * @param src Source string
 * @return Zero on success, non-zero on error
 */
int strcpy_s(char* dest, size_t dest_size, const char* src);

/**
 * @brief Safe string concatenation
 * @param dest Destination buffer
 * @param dest_size Size of destination buffer
 * @param src Source string
 * @return Zero on success, non-zero on error
 */
int strcat_s(char* dest, size_t dest_size, const char* src);

/**
 * @brief Safe formatted output
 * @param buffer Destination buffer
 * @param size Size of destination buffer
 * @param format Printf-style format string
 * @param ... Format arguments
 * @return Number of characters written (excluding null terminator)
 */
int sprintf_s(char* buffer, size_t size, const char* format, ...);

/**
 * @brief Open file with security enhancements
 * @param pFile Pointer to file pointer
 * @param filename File name
 * @param mode File mode
 * @return Zero on success, non-zero on error
 */
int fopen_s(FILE** pFile, const char* filename, const char* mode);

#endif // !_WIN32

// =============================================================================
// Utility Macros
// =============================================================================

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

#define UNUSED(x) (void)(x)

// Alignment helpers
#define ALIGN_UP(value, alignment) (((value) + ((alignment) - 1)) & ~((alignment) - 1))
#define ALIGN_DOWN(value, alignment) ((value) & ~((alignment) - 1))
#define IS_ALIGNED(value, alignment) (((value) & ((alignment) - 1)) == 0)

// Bit manipulation
#define BIT(n) (1ULL << (n))
#define SET_BIT(value, n) ((value) | BIT(n))
#define CLEAR_BIT(value, n) ((value) & ~BIT(n))
#define TEST_BIT(value, n) (((value) & BIT(n)) != 0)
#define TOGGLE_BIT(value, n) ((value) ^ BIT(n))

// Byte extraction
#define BYTE0(value) ((uint8_t)((value) & 0xFF))
#define BYTE1(value) ((uint8_t)(((value) >> 8) & 0xFF))
#define BYTE2(value) ((uint8_t)(((value) >> 16) & 0xFF))
#define BYTE3(value) ((uint8_t)(((value) >> 24) & 0xFF))

// Sign extension
#define SIGN_EXTEND_8(value) ((int8_t)(value))
#define SIGN_EXTEND_16(value) ((int16_t)(value))
#define SIGN_EXTEND_32(value) ((int32_t)(value))
