/**
 * @file File.cpp
 * @brief FileSystem::File implementation on top of CreateFileW (Windows) or open() (POSIX).
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

// --- FileError implementation ---

FileError FileError::from_last_error(const std::string& operation) {
#ifdef _WIN32
    const DWORD err = ::GetLastError();
    return FileError(operation, static_cast<int>(err));
#else
    return FileError(operation, errno);
#endif
}

const char* FileError::what() const noexcept { return std::system_error::what(); }

// --- FileImpl - pimpl implementation ---

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

    FileImpl(FileImpl&& other) noexcept :
        fileName(std::move(other.fileName)), mode(other.mode), share(other.share), lastError(other.lastError) {
#ifdef _WIN32
        hFile = other.hFile;
        other.hFile = INVALID_HANDLE_VALUE;
#else
        fd = other.fd;
        other.fd = -1;
#endif
    }

    FileImpl& operator=(FileImpl&& other) noexcept {
        if (this != &other) {
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

    [[nodiscard]] bool is_open() const noexcept {
#ifdef _WIN32
        return hFile != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }

    void set_error_from_system() {
#ifdef _WIN32
        lastError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
#else
        lastError = std::error_code(errno, std::system_category());
#endif
    }

    void clear_error() noexcept { lastError.clear(); }

    void close_handle() noexcept {
#ifdef _WIN32
        if (hFile != INVALID_HANDLE_VALUE) {
            ::CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
        fileName.clear();
    }
};

// --- File - Constructors / Destructor / Move ---

File::File() : m_impl(std::make_unique<FileImpl>()) {}

File::File(const std::string& filename, OpenMode mode, ShareMode share) : m_impl(std::make_unique<FileImpl>()) {
    open(filename, mode, share);
}

File::~File() = default;

File::File(File&& other) noexcept = default;
File& File::operator=(File&& other) noexcept = default;

// --- File - Open / Close / State ---

void File::open(const std::string& filename, OpenMode mode, ShareMode share) {
    close();
    m_impl->clear_error();
    m_impl->mode = mode;
    m_impl->share = share;

#ifdef _WIN32
    // Map OpenMode to Windows access flags
    DWORD dwAccess = 0;
    if (has_flag(mode, OpenMode::Read)) {
        dwAccess |= GENERIC_READ;
    }
    if (has_flag(mode, OpenMode::Write) || has_flag(mode, OpenMode::Append)) {
        dwAccess |= GENERIC_WRITE;
    }

    // Map ShareMode
    DWORD dwShare = 0;
    if (has_flag(static_cast<OpenMode>(static_cast<uint32_t>(share)),
                 static_cast<OpenMode>(static_cast<uint32_t>(ShareMode::Read)))) {
        dwShare |= FILE_SHARE_READ;
    }
    if (static_cast<uint32_t>(share) & static_cast<uint32_t>(ShareMode::Write)) {
        dwShare |= FILE_SHARE_WRITE;
    }
    if (static_cast<uint32_t>(share) & static_cast<uint32_t>(ShareMode::Delete)) {
        dwShare |= FILE_SHARE_DELETE;
    }

    // Map creation disposition
    DWORD dwCreation = OPEN_EXISTING;
    if (has_flag(mode, OpenMode::CreateNew)) {
        dwCreation = CREATE_NEW;
    } else if (has_flag(mode, OpenMode::Create) && has_flag(mode, OpenMode::Truncate)) {
        dwCreation = CREATE_ALWAYS;
    } else if (has_flag(mode, OpenMode::Create)) {
        dwCreation = OPEN_ALWAYS;
    } else if (has_flag(mode, OpenMode::Truncate)) {
        dwCreation = TRUNCATE_EXISTING;
    }

    // Convert filename to wide string for proper Unicode support
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        m_impl->set_error_from_system();
        throw FileError::from_last_error("Failed to convert filename: " + filename);
    }

    auto wideName = std::make_unique<wchar_t[]>(static_cast<size_t>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wideName.get(), wideLen);

    m_impl->hFile =
        ::CreateFileW(wideName.get(), dwAccess, dwShare, nullptr, dwCreation, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (m_impl->hFile == INVALID_HANDLE_VALUE) {
        m_impl->set_error_from_system();
        throw FileError::from_last_error("Failed to open file: " + filename);
    }

    // Handle append mode: seek to end
    if (has_flag(mode, OpenMode::Append)) {
        ::SetFilePointerEx(m_impl->hFile, LARGE_INTEGER{}, nullptr, FILE_END);
    }

#else
    // Map OpenMode to POSIX flags
    int flags = 0;
    const bool wantRead = has_flag(mode, OpenMode::Read);
    const bool wantWrite = has_flag(mode, OpenMode::Write) || has_flag(mode, OpenMode::Append);

    if (wantRead && wantWrite) {
        flags = O_RDWR;
    } else if (wantWrite) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    if (has_flag(mode, OpenMode::Create)) {
        flags |= O_CREAT;
    }
    if (has_flag(mode, OpenMode::CreateNew)) {
        flags |= O_CREAT | O_EXCL;
    }
    if (has_flag(mode, OpenMode::Truncate)) {
        flags |= O_TRUNC;
    }
    if (has_flag(mode, OpenMode::Append)) {
        flags |= O_APPEND;
    }

    m_impl->fd = ::open(filename.c_str(), flags, 0644);
    if (m_impl->fd < 0) {
        m_impl->set_error_from_system();
        throw FileError::from_last_error("Failed to open file: " + filename);
    }
#endif

    m_impl->fileName = filename;
}

void File::close() { m_impl->close_handle(); }

bool File::is_open() const noexcept { return m_impl->is_open(); }

// --- File - Read operations ---

size_t File::read(void* buffer, size_t count) {
    if (!is_open() || count == 0 || buffer == nullptr) {
        return 0;
    }

#ifdef _WIN32
    DWORD bytesRead = 0;
    if (!::ReadFile(m_impl->hFile, buffer, static_cast<DWORD>(count), &bytesRead, nullptr)) {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(bytesRead);
#else
    const ssize_t result = ::read(m_impl->fd, buffer, count);
    if (result < 0) {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(result);
#endif
}

// --- File - Write operations ---

size_t File::write(const void* buffer, size_t count) {
    if (!is_open() || count == 0 || buffer == nullptr) {
        return 0;
    }

#ifdef _WIN32
    DWORD bytesWritten = 0;
    if (!::WriteFile(m_impl->hFile, buffer, static_cast<DWORD>(count), &bytesWritten, nullptr)) {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(bytesWritten);
#else
    const ssize_t result = ::write(m_impl->fd, buffer, count);
    if (result < 0) {
        m_impl->set_error_from_system();
        return 0;
    }
    return static_cast<size_t>(result);
#endif
}

// --- File - Seek / Position operations ---

int64_t File::seek(int64_t offset, SeekOrigin origin) {
    if (!is_open()) {
        return -1;
    }

#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = offset;
    LARGE_INTEGER newPos;
    DWORD dwOrigin;
    switch (origin) {
    case SeekOrigin::Begin:   dwOrigin = FILE_BEGIN; break;
    case SeekOrigin::Current: dwOrigin = FILE_CURRENT; break;
    case SeekOrigin::End:     dwOrigin = FILE_END; break;
    default:                  return -1;
    }
    if (!::SetFilePointerEx(m_impl->hFile, li, &newPos, dwOrigin)) {
        m_impl->set_error_from_system();
        return -1;
    }
    return newPos.QuadPart;
#else
    int whence;
    switch (origin) {
    case SeekOrigin::Begin:   whence = SEEK_SET; break;
    case SeekOrigin::Current: whence = SEEK_CUR; break;
    case SeekOrigin::End:     whence = SEEK_END; break;
    default:                  return -1;
    }
    const off_t result = ::lseek(m_impl->fd, static_cast<off_t>(offset), whence);
    if (result == static_cast<off_t>(-1)) {
        m_impl->set_error_from_system();
    }
    return static_cast<int64_t>(result);
#endif
}

// --- File - Properties ---

int64_t File::size() const {
    if (!is_open()) {
        return -1;
    }

#ifdef _WIN32
    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(m_impl->hFile, &fileSize)) {
        return -1;
    }
    return fileSize.QuadPart;
#else
    struct stat st;
    if (::fstat(m_impl->fd, &st) != 0) {
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
#endif
}

void File::flush() {
    if (!is_open()) {
        return;
    }

#ifdef _WIN32
    ::FlushFileBuffers(m_impl->hFile);
#else
    ::fsync(m_impl->fd);
#endif
}

// --- File - Error state ---

// --- File - Metadata accessors ---

std::string File::filename() const { return m_impl->fileName; }

// --- File - Timestamps ---

// --- File - Static utility methods ---

} // namespace FileSystem
