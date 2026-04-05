/**
 * @file File.cpp
 * @brief File implementation.
 */
#include "file.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <dirent.h>
#endif

namespace FileSystem {

// =============================================================================
// FileError implementation
// =============================================================================

FileError FileError::from_last_error(const std::string& operation)
{
#ifdef _WIN32
    const DWORD err = ::GetLastError();
    return FileError(operation, static_cast<int>(err));
#else
    return FileError(operation, errno);
#endif
}

const char* FileError::what() const noexcept
{
    return std::system_error::what();
}

// =============================================================================
// FileImpl - pimpl implementation
// =============================================================================

class FileImpl {
public:
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    std::string fileName;
    OpenMode mode = OpenMode::Read;
    ShareMode share = ShareMode::All;
    std::error_code lastError;

    FileImpl() = default;
    ~FileImpl() { close_handle(); }

    FileImpl(FileImpl&& other) noexcept
        : fileName(std::move(other.fileName))
        , mode(other.mode)
        , share(other.share)
        , lastError(other.lastError)
    {
#ifdef _WIN32
        hFile = other.hFile;
        other.hFile = INVALID_HANDLE_VALUE;
#else
        fd = other.fd;
        other.fd = -1;
#endif
    }

    FileImpl& operator=(FileImpl&& other) noexcept
    {
        if (this != &other)
        {
            close_handle();
            fileName = std::move(other.fileName);
            mode = other.mode;
            share = other.share;
            lastError = other.lastError;
#ifdef _WIN32
            hFile = other.hFile;
            other.hFile = INVALID_HANDLE_VALUE;
#else
            fd = other.fd;
            other.fd = -1;
#endif
        }
        return *this;
    }

    [[nodiscard]] bool is_open() const noexcept
    {
#ifdef _WIN32
        return hFile != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }

    void set_error_from_system()
    {
#ifdef _WIN32
        lastError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
#else
        lastError = std::error_code(errno, std::system_category());
#endif
    }

    void clear_error() noexcept
    {
        lastError.clear();
    }

    void close_handle() noexcept
    {
#ifdef _WIN32
        if (hFile != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
#endif
        fileName.clear();
    }
};

// =============================================================================
// File - Constructors / Destructor / Move
// =============================================================================

File::File()
    : m_impl(std::make_unique<FileImpl>())
{
}

File::File(const std::string& filename, OpenMode mode, ShareMode share)
    : m_impl(std::make_unique<FileImpl>())
{
    open(filename, mode, share);
}

File::~File() = default;

File::File(File&& other) noexcept = default;
File& File::operator=(File&& other) noexcept = default;

// =============================================================================
// File - Open / Close / State
// =============================================================================

void File::open(const std::string& filename, OpenMode mode, ShareMode share)
{
    close();
    m_impl->clear_error();
    m_impl->mode = mode;
    m_impl->share = share;

#ifdef _WIN32
    // Map OpenMode to Windows access flags
    DWORD dwAccess = 0;
    if (has_flag(mode, OpenMode::Read))
    {
        dwAccess |= GENERIC_READ;
    }
    if (has_flag(mode, OpenMode::Write) || has_flag(mode, OpenMode::Append))
    {
        dwAccess |= GENERIC_WRITE;
    }

    // Map ShareMode
    DWORD dwShare = 0;
    if (has_flag(static_cast<OpenMode>(static_cast<uint32_t>(share)),
                 static_cast<OpenMode>(static_cast<uint32_t>(ShareMode::Read))))
    {
        dwShare |= FILE_SHARE_READ;
    }
    if (static_cast<uint32_t>(share) & static_cast<uint32_t>(ShareMode::Write))
    {
        dwShare |= FILE_SHARE_WRITE;
    }
    if (static_cast<uint32_t>(share) & static_cast<uint32_t>(ShareMode::Delete))
    {
        dwShare |= FILE_SHARE_DELETE;
    }

    // Map creation disposition
    DWORD dwCreation = OPEN_EXISTING;
    if (has_flag(mode, OpenMode::CreateNew))
    {
        dwCreation = CREATE_NEW;
    }
    else if (has_flag(mode, OpenMode::Create) && has_flag(mode, OpenMode::Truncate))
    {
        dwCreation = CREATE_ALWAYS;
    }
    else if (has_flag(mode, OpenMode::Create))
    {
        dwCreation = OPEN_ALWAYS;
    }
    else if (has_flag(mode, OpenMode::Truncate))
    {
        dwCreation = TRUNCATE_EXISTING;
    }

    // Convert filename to wide string for proper Unicode support
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        m_impl->set_error_from_system();
        throw FileError::from_last_error("Failed to convert filename: " + filename);
    }

    auto wideName = std::make_unique<wchar_t[]>(static_cast<size_t>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wideName.get(), wideLen);

    m_impl->hFile = ::CreateFileW(
        wideName.get(), dwAccess, dwShare, nullptr,
        dwCreation, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (m_impl->hFile == INVALID_HANDLE_VALUE)
    {
        m_impl->set_error_from_system();
        throw FileError::from_last_error("Failed to open file: " + filename);
    }

    // Handle append mode: seek to end
    if (has_flag(mode, OpenMode::Append))
    {
        ::SetFilePointerEx(m_impl->hFile, LARGE_INTEGER{}, nullptr, FILE_END);
    }

#else
    // Map OpenMode to POSIX flags
    int flags = 0;
    const bool wantRead = has_flag(mode, OpenMode::Read);
    const bool wantWrite = has_flag(mode, OpenMode::Write) || has_flag(mode, OpenMode::Append);

    if (wantRead && wantWrite)
    {
        flags = O_RDWR;
    }
    else if (wantWrite)
    {
        flags = O_WRONLY;
    }
    else
    {
        flags = O_RDONLY;
    }

    if (has_flag(mode, OpenMode::Create))
    {
        flags |= O_CREAT;
    }
    if (has_flag(mode, OpenMode::CreateNew))
    {
        flags |= O_CREAT | O_EXCL;
    }
    if (has_flag(mode, OpenMode::Truncate))
    {
        flags |= O_TRUNC;
    }
    if (has_flag(mode, OpenMode::Append))
    {
        flags |= O_APPEND;
    }

    m_impl->fd = ::open(filename.c_str(), flags, 0644);
    if (m_impl->fd < 0)
    {
        m_impl->set_error_from_system();
        throw FileError::from_last_error("Failed to open file: " + filename);
    }
#endif

    m_impl->fileName = filename;
}

void File::close()
{
    m_impl->close_handle();
}

bool File::is_open() const noexcept
{
    return m_impl->is_open();
}

// =============================================================================
// File - Read operations
// =============================================================================

size_t File::read(void* buffer, size_t count)
{
    if (!is_open() || count == 0 || buffer == nullptr)
    {
        return 0;
    }

#ifdef _WIN32
    DWORD bytesRead = 0;
    if (!::ReadFile(m_impl->hFile, buffer, static_cast<DWORD>(count), &bytesRead, nullptr))
    {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(bytesRead);
#else
    const ssize_t result = ::read(m_impl->fd, buffer, count);
    if (result < 0)
    {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(result);
#endif
}

std::vector<uint8_t> File::read_all()
{
    if (!is_open())
    {
        return {};
    }

    const int64_t fileSize = size();
    if (fileSize <= 0)
    {
        return {};
    }

    seek_to_begin();

    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    const size_t bytesRead = read(data.data(), data.size());
    data.resize(bytesRead);
    return data;
}

std::string File::read_text()
{
    if (!is_open())
    {
        return {};
    }

    const int64_t fileSize = size();
    if (fileSize <= 0)
    {
        return {};
    }

    seek_to_begin();

    std::string text(static_cast<size_t>(fileSize), '\0');
    const size_t bytesRead = read(text.data(), text.size());
    text.resize(bytesRead);
    return text;
}

// =============================================================================
// File - Write operations
// =============================================================================

size_t File::write(const void* buffer, size_t count)
{
    if (!is_open() || count == 0 || buffer == nullptr)
    {
        return 0;
    }

#ifdef _WIN32
    DWORD bytesWritten = 0;
    if (!::WriteFile(m_impl->hFile, buffer, static_cast<DWORD>(count), &bytesWritten, nullptr))
    {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(bytesWritten);
#else
    const ssize_t result = ::write(m_impl->fd, buffer, count);
    if (result < 0)
    {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(result);
#endif
}

size_t File::write(const std::string& text)
{
    return write(text.data(), text.size());
}

size_t File::write(const std::vector<uint8_t>& data)
{
    return write(data.data(), data.size());
}

// =============================================================================
// File - Seek / Position operations
// =============================================================================

int64_t File::tell() const
{
    if (!is_open())
    {
        return -1;
    }

#ifdef _WIN32
    LARGE_INTEGER pos;
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(m_impl->hFile, zero, &pos, FILE_CURRENT))
    {
        return -1;
    }
    return pos.QuadPart;
#else
    return static_cast<int64_t>(::lseek(m_impl->fd, 0, SEEK_CUR));
#endif
}

int64_t File::seek(int64_t offset, SeekOrigin origin)
{
    if (!is_open())
    {
        return -1;
    }

#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = offset;
    LARGE_INTEGER newPos;
    DWORD dwOrigin;
    switch (origin)
    {
    case SeekOrigin::Begin:   dwOrigin = FILE_BEGIN; break;
    case SeekOrigin::Current: dwOrigin = FILE_CURRENT; break;
    case SeekOrigin::End:     dwOrigin = FILE_END; break;
    default:                  return -1;
    }
    if (!::SetFilePointerEx(m_impl->hFile, li, &newPos, dwOrigin))
    {
        m_impl->set_error_from_system();
        return -1;
    }
    return newPos.QuadPart;
#else
    int whence;
    switch (origin)
    {
    case SeekOrigin::Begin:   whence = SEEK_SET; break;
    case SeekOrigin::Current: whence = SEEK_CUR; break;
    case SeekOrigin::End:     whence = SEEK_END; break;
    default:                  return -1;
    }
    const off_t result = ::lseek(m_impl->fd, static_cast<off_t>(offset), whence);
    if (result == static_cast<off_t>(-1))
    {
        m_impl->set_error_from_system();
    }
    return static_cast<int64_t>(result);
#endif
}

void File::seek_to_begin()
{
    seek(0, SeekOrigin::Begin);
}

int64_t File::seek_to_end()
{
    return seek(0, SeekOrigin::End);
}

// =============================================================================
// File - Properties
// =============================================================================

int64_t File::size() const
{
    if (!is_open())
    {
        return -1;
    }

#ifdef _WIN32
    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(m_impl->hFile, &fileSize))
    {
        return -1;
    }
    return fileSize.QuadPart;
#else
    struct stat st;
    if (::fstat(m_impl->fd, &st) != 0)
    {
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
#endif
}

void File::resize(int64_t new_size)
{
    if (!is_open())
    {
        return;
    }

#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = new_size;
    if (::SetFilePointerEx(m_impl->hFile, li, nullptr, FILE_BEGIN))
    {
        ::SetEndOfFile(m_impl->hFile);
    }
#else
    ::ftruncate(m_impl->fd, static_cast<off_t>(new_size));
#endif
}

bool File::is_eof() const
{
    if (!is_open())
    {
        return true;
    }

    const int64_t current = const_cast<File*>(this)->tell();
    const int64_t fileSize = size();
    return current >= fileSize;
}

void File::flush()
{
    if (!is_open())
    {
        return;
    }

#ifdef _WIN32
    ::FlushFileBuffers(m_impl->hFile);
#else
    ::fsync(m_impl->fd);
#endif
}

NativeHandle File::native_handle() const
{
    if (!is_open())
    {
#ifdef _WIN32
        return INVALID_HANDLE_VALUE;
#else
        return -1;
#endif
    }

#ifdef _WIN32
    return m_impl->hFile;
#else
    return m_impl->fd;
#endif
}

// =============================================================================
// File - Error state
// =============================================================================

const std::error_code& File::last_error() const noexcept
{
    return m_impl->lastError;
}

void File::clear_error() noexcept
{
    m_impl->clear_error();
}

bool File::has_error() const noexcept
{
    return static_cast<bool>(m_impl->lastError);
}

// =============================================================================
// File - Metadata accessors
// =============================================================================

std::string File::filename() const
{
    return m_impl->fileName;
}

OpenMode File::open_mode() const
{
    return m_impl->mode;
}

ShareMode File::share_mode() const
{
    return m_impl->share;
}

// =============================================================================
// File - Timestamps
// =============================================================================

std::chrono::system_clock::time_point File::get_last_write_time() const
{
    if (!is_open())
    {
        return {};
    }

#ifdef _WIN32
    FILETIME ftWrite;
    if (!::GetFileTime(m_impl->hFile, nullptr, nullptr, &ftWrite))
    {
        return {};
    }

    // Convert FILETIME (100ns intervals since 1601-01-01) to system_clock
    // Windows epoch offset: 11644473600 seconds between 1601 and 1970
    constexpr int64_t kWindowsTicksPerSecond = 10'000'000LL;
    constexpr int64_t kEpochDifference = 11'644'473'600LL;

    ULARGE_INTEGER uli;
    uli.LowPart = ftWrite.dwLowDateTime;
    uli.HighPart = ftWrite.dwHighDateTime;

    const int64_t unixSeconds =
        static_cast<int64_t>(uli.QuadPart / kWindowsTicksPerSecond) - kEpochDifference;

    return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(unixSeconds));
#else
    struct stat st;
    if (::fstat(m_impl->fd, &st) != 0)
    {
        return {};
    }
    return std::chrono::system_clock::from_time_t(st.st_mtime);
#endif
}

void File::set_last_write_time(const std::chrono::system_clock::time_point& time)
{
    if (!is_open())
    {
        return;
    }

#ifdef _WIN32
    constexpr int64_t kWindowsTicksPerSecond = 10'000'000LL;
    constexpr int64_t kEpochDifference = 11'644'473'600LL;

    const auto tt = std::chrono::system_clock::to_time_t(time);
    const int64_t windowsTicks =
        (static_cast<int64_t>(tt) + kEpochDifference) * kWindowsTicksPerSecond;

    FILETIME ft;
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<ULONGLONG>(windowsTicks);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    ::SetFileTime(m_impl->hFile, nullptr, nullptr, &ft);
#else
    // POSIX: use utimensat via fd
    const auto tt = std::chrono::system_clock::to_time_t(time);
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT; // Don't change access time
    times[1].tv_sec = tt;
    times[1].tv_nsec = 0;
    ::futimens(m_impl->fd, times);
#endif
}

// =============================================================================
// File - Static utility methods
// =============================================================================

bool File::exists(const std::string& filename)
{
#ifdef _WIN32
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        return false;
    }
    auto wideName = std::make_unique<wchar_t[]>(static_cast<size_t>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wideName.get(), wideLen);

    const DWORD attrs = ::GetFileAttributesW(wideName.get());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(filename.c_str(), &st) == 0;
#endif
}

bool File::remove(const std::string& filename)
{
#ifdef _WIN32
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        return false;
    }
    auto wideName = std::make_unique<wchar_t[]>(static_cast<size_t>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wideName.get(), wideLen);

    // Remove read-only attribute if present
    const DWORD attrs = ::GetFileAttributesW(wideName.get());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
    {
        ::SetFileAttributesW(wideName.get(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }

    return ::DeleteFileW(wideName.get()) != 0;
#else
    return ::unlink(filename.c_str()) == 0;
#endif
}

bool File::rename(const std::string& old_name, const std::string& new_name)
{
#ifdef _WIN32
    const int wideOldLen = MultiByteToWideChar(CP_UTF8, 0, old_name.c_str(), -1, nullptr, 0);
    const int wideNewLen = MultiByteToWideChar(CP_UTF8, 0, new_name.c_str(), -1, nullptr, 0);
    if (wideOldLen <= 0 || wideNewLen <= 0)
    {
        return false;
    }

    auto wideOld = std::make_unique<wchar_t[]>(static_cast<size_t>(wideOldLen));
    auto wideNew = std::make_unique<wchar_t[]>(static_cast<size_t>(wideNewLen));
    MultiByteToWideChar(CP_UTF8, 0, old_name.c_str(), -1, wideOld.get(), wideOldLen);
    MultiByteToWideChar(CP_UTF8, 0, new_name.c_str(), -1, wideNew.get(), wideNewLen);

    return ::MoveFileW(wideOld.get(), wideNew.get()) != 0;
#else
    return ::rename(old_name.c_str(), new_name.c_str()) == 0;
#endif
}

int64_t File::get_size(const std::string& filename)
{
#ifdef _WIN32
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        return -1;
    }
    auto wideName = std::make_unique<wchar_t[]>(static_cast<size_t>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wideName.get(), wideLen);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!::GetFileAttributesExW(wideName.get(), GetFileExInfoStandard, &fad))
    {
        return -1;
    }
    LARGE_INTEGER fileSize;
    fileSize.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
    fileSize.LowPart = fad.nFileSizeLow;
    return fileSize.QuadPart;
#else
    struct stat st;
    if (::stat(filename.c_str(), &st) != 0)
    {
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
#endif
}

std::string File::get_current_directory()
{
#ifdef _WIN32
    const DWORD len = ::GetCurrentDirectoryW(0, nullptr);
    if (len == 0)
    {
        return {};
    }
    auto wideBuf = std::make_unique<wchar_t[]>(len);
    ::GetCurrentDirectoryW(len, wideBuf.get());

    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideBuf.get(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(utf8Len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideBuf.get(), -1, result.data(), utf8Len, nullptr, nullptr);
    return result;
#else
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)) != nullptr)
    {
        return std::string(buf);
    }
    return {};
#endif
}

bool File::set_current_directory(const std::string& path)
{
#ifdef _WIN32
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        return false;
    }
    auto widePath = std::make_unique<wchar_t[]>(static_cast<size_t>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.get(), wideLen);

    return ::SetCurrentDirectoryW(widePath.get()) != 0;
#else
    return ::chdir(path.c_str()) == 0;
#endif
}

} // namespace FileSystem
