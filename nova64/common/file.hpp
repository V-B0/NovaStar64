/**
 * @file file.hpp
 * @brief file declarations and types.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <system_error>
#include <memory>
#include <type_traits>
#include <vector>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
using NativeHandle = HANDLE;
#else
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
using NativeHandle = int;
#endif

namespace FileSystem {

enum class OpenMode : uint32_t {
    Read = 0x01,
    Write = 0x02,
    ReadWrite = 0x03,
    Append = 0x08,
    Truncate = 0x10,
    Create = 0x20,
    CreateNew = 0x40
};

enum class ShareMode : uint32_t { None = 0x00, Read = 0x01, Write = 0x02, Delete = 0x04, All = 0x07 };

enum class SeekOrigin : uint32_t { Begin = 0, Current = 1, End = 2 };

enum class FileAttribute : uint32_t {
    Normal = 0x00,
    ReadOnly = 0x01,
    Hidden = 0x02,
    System = 0x04,
    Directory = 0x10,
    Archive = 0x20,
    Temporary = 0x100
};

inline constexpr OpenMode operator|(OpenMode a, OpenMode b) noexcept {
    return static_cast<OpenMode>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr OpenMode operator&(OpenMode a, OpenMode b) noexcept {
    return static_cast<OpenMode>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr OpenMode operator~(OpenMode a) noexcept { return static_cast<OpenMode>(~static_cast<uint32_t>(a)); }

inline constexpr OpenMode& operator|=(OpenMode& a, OpenMode b) noexcept {
    a = a | b;
    return a;
}

inline constexpr OpenMode& operator&=(OpenMode& a, OpenMode b) noexcept {
    a = a & b;
    return a;
}

inline constexpr bool has_flag(OpenMode value, OpenMode flag) noexcept { return (value & flag) == flag; }

class FileError : public std::system_error {
public:
    explicit FileError(const std::string& message, int error_code = 0) :
        std::system_error(error_code, std::system_category(), message) {}

    explicit FileError(const char* message, int error_code = 0) :
        std::system_error(error_code, std::system_category(), message) {}

    static FileError from_last_error(const std::string& operation);

    const char* what() const noexcept override;
};

class FileImpl;

class File {
public:
    File();
    explicit File(const std::string& filename, OpenMode mode = OpenMode::Read, ShareMode share = ShareMode::All);
    ~File();

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    void open(const std::string& filename, OpenMode mode = OpenMode::Read, ShareMode share = ShareMode::All);
    void close();
    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] size_t read(void* buffer, size_t count);
    [[nodiscard]] std::vector<uint8_t> read_all();
    [[nodiscard]] std::string read_text();

    template <typename T> [[nodiscard]] T read_as() {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>,
                      "Type must be trivially copyable and not a pointer");
        T value;
        if (read(&value, sizeof(T)) != sizeof(T)) {
            throw FileError("Failed to read complete type");
        }
        return value;
    }

    template <typename T> [[nodiscard]] bool read_as(T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>,
                      "Type must be trivially copyable and not a pointer");
        return read(&value, sizeof(T)) == sizeof(T);
    }

    [[nodiscard]] size_t write(const void* buffer, size_t count);
    [[nodiscard]] size_t write(const std::string& text);
    [[nodiscard]] size_t write(const std::vector<uint8_t>& data);

    template <typename T> [[nodiscard]] size_t write(const T& value) {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>,
                      "Type must be trivially copyable and not a pointer");
        return write(&value, sizeof(T));
    }

    [[nodiscard]] int64_t tell() const;
    [[nodiscard]] int64_t seek(int64_t offset, SeekOrigin origin = SeekOrigin::Begin);
    void seek_to_begin();
    [[nodiscard]] int64_t seek_to_end();

    // File properties
    [[nodiscard]] int64_t size() const;
    void resize(int64_t new_size);
    [[nodiscard]] bool is_eof() const;
    void flush();

    [[nodiscard]] NativeHandle native_handle() const;

    [[nodiscard]] static bool exists(const std::string& filename);
    [[nodiscard]] static bool remove(const std::string& filename);
    [[nodiscard]] static bool rename(const std::string& old_name, const std::string& new_name);
    [[nodiscard]] static int64_t get_size(const std::string& filename);
    [[nodiscard]] static std::string get_current_directory();
    [[nodiscard]] static bool set_current_directory(const std::string& path);

    [[nodiscard]] const std::error_code& last_error() const noexcept;
    void clear_error() noexcept;
    [[nodiscard]] bool has_error() const noexcept;

    [[nodiscard]] std::string filename() const;
    [[nodiscard]] OpenMode open_mode() const;
    [[nodiscard]] ShareMode share_mode() const;

    [[nodiscard]] std::chrono::system_clock::time_point get_last_write_time() const;

    void set_last_write_time(const std::chrono::system_clock::time_point& time);

private:
    std::unique_ptr<FileImpl> m_impl;
};

} // namespace FileSystem
