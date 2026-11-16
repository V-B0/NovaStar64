/**
 * @file DynamicLibrary.cpp
 * @brief DynamicLibrary implementation on top of LoadLibrary (Windows) or dlopen (POSIX).
 */
#include "DynamicLibrary.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif
#include <utility>

DynamicLibrary::DynamicLibrary() noexcept = default;

DynamicLibrary::DynamicLibrary(const std::filesystem::path& libraryPath, int loadFlags) {
    Load(libraryPath, loadFlags);
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept :
    m_handle(std::exchange(other.m_handle, nullptr)), m_path(std::move(other.m_path)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        Unload();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_path = std::move(other.m_path);
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() { Unload(); }

void DynamicLibrary::Load(const std::filesystem::path& libraryPath, int loadFlags) {
    Unload();

#ifdef _WIN32
    HMODULE hModule = nullptr;
    if (loadFlags != 0) {
        hModule = LoadLibraryExW(libraryPath.c_str(), nullptr, static_cast<DWORD>(loadFlags));
    } else {
        hModule = LoadLibraryW(libraryPath.c_str());
    }
    m_handle = static_cast<Handle>(hModule);
#else
    int flags = (loadFlags != 0) ? loadFlags : RTLD_NOW;
    m_handle = dlopen(libraryPath.c_str(), flags);
#endif

    if (m_handle == nullptr) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                "Failed to load library: " + libraryPath.string() + " - " + GetLastError());
    }

    m_path = libraryPath;
}

void DynamicLibrary::Unload() noexcept {
    if (m_handle != nullptr) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_handle));
#else
        dlclose(m_handle);
#endif
        m_handle = nullptr;
        m_path.clear();
    }
}

DynamicLibrary::Symbol DynamicLibrary::GetSymbol(std::string_view symbolName) const noexcept {
    if (m_handle == nullptr || symbolName.empty()) {
        return nullptr;
    }

    const std::string name(symbolName);

#ifdef _WIN32
    return reinterpret_cast<Symbol>(GetProcAddress(static_cast<HMODULE>(m_handle), name.c_str()));
#else
    return dlsym(m_handle, name.c_str());
#endif
}

std::string DynamicLibrary::GetLastError() noexcept {
#ifdef _WIN32
    const DWORD error = ::GetLastError();
    if (error == 0) {
        return {};
    }

    LPWSTR msgBuf = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&msgBuf), 0, nullptr);

    if (len == 0 || msgBuf == nullptr) {
        return "Unknown error (code " + std::to_string(error) + ")";
    }

    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, msgBuf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, msgBuf, static_cast<int>(len), result.data(), utf8Len, nullptr, nullptr);

    LocalFree(msgBuf);

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }

    return result;
#else
    const char* err = dlerror();
    return err ? std::string(err) : std::string{};
#endif
}
