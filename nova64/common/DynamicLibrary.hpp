/**
 * @file DynamicLibrary.hpp
 * @brief Platform-independent dynamic library loader declarations.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

/**
 * @class DynamicLibrary
 * @brief Platform-independent dynamic library (DLL/SO) loader
 * 
 * Provides RAII wrapper for loading and accessing dynamic/shared libraries.
 * On Windows, loads DLLs; on Unix/Linux, loads shared objects (.so).
 */
class DynamicLibrary
{
public:
    /**
     * @brief Platform-specific library handle type
     */
    using Handle = void*;

    /**
     * @brief Represents a loaded symbol (function or variable) from the library
     */
    using Symbol = void*;

    /**
     * @brief Constructs an empty (unloaded) library instance
     */
    DynamicLibrary() noexcept;
    
    /**
     * @brief Loads a dynamic library from the specified path
     * @param libraryPath Path to the dynamic library file
     * @param loadFlags Platform-specific loading flags (0 for defaults)
     * @throws std::system_error if loading fails
     */
    explicit DynamicLibrary(const std::filesystem::path& libraryPath, int loadFlags = 0);
    
    /**
     * @brief Move constructor
     */
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    
    /**
     * @brief Move assignment operator
     */
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    
    /**
     * @brief Destructor - automatically unloads the library
     */
    ~DynamicLibrary();
    
    // Non-copyable
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    
    /**
     * @brief Loads a dynamic library
     * @param libraryPath Path to the dynamic library file
     * @param loadFlags Platform-specific loading flags
     * @throws std::system_error if loading fails
     */
    void Load(const std::filesystem::path& libraryPath, int loadFlags = 0);
    
    /**
     * @brief Unloads the current library (if loaded)
     */
    void Unload() noexcept;
    
    /**
     * @brief Checks if a library is currently loaded
     * @return true if a library is loaded
     */
    [[nodiscard]] bool IsLoaded() const noexcept { return m_handle != nullptr; }
    
    /**
     * @brief Gets a symbol (function or variable) from the library
     * @param symbolName Name of the symbol to retrieve
     * @return Pointer to the symbol, or nullptr if not found
     */
    [[nodiscard]] Symbol GetSymbol(std::string_view symbolName) const noexcept;
    
    /**
     * @brief Gets a typed symbol from the library
     * @tparam T Type of the symbol (function pointer, data pointer, etc.)
     * @param symbolName Name of the symbol to retrieve
     * @return Pointer to the typed symbol, or nullptr if not found
     */
    template<typename T>
    [[nodiscard]] T GetSymbol(std::string_view symbolName) const noexcept
    {
        static_assert(std::is_pointer_v<T>, "T must be a pointer type");
        return reinterpret_cast<T>(GetSymbol(symbolName));
    }
    
    /**
     * @brief Gets a symbol or throws an exception if not found
     * @param symbolName Name of the symbol to retrieve
     * @return Pointer to the symbol
     * @throws std::system_error if symbol is not found
     */
    [[nodiscard]] Symbol GetSymbolOrThrow(std::string_view symbolName) const;
    
    /**
     * @brief Gets the platform-specific error message from the last operation
     * @return Error message string
     */
    [[nodiscard]] static std::string GetLastError() noexcept;
    
    /**
     * @brief Gets the native library handle
     * @return Native handle (void* on POSIX, HMODULE on Windows)
     */
    [[nodiscard]] Handle GetHandle() const noexcept { return m_handle; }
    
    /**
     * @brief Gets the path to the loaded library
     * @return Library path, or empty if no library loaded
     */
    [[nodiscard]] std::filesystem::path GetPath() const noexcept { return m_path; }
    
    /**
     * @brief Platform-specific loading flags
     */
    enum LoadFlags : int
    {
        DEFAULT = 0,
        
#ifdef _WIN32
        // Windows-specific flags
        LOAD_LIBRARY_AS_DATAFILE = 0x00000002,
        LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008,
        LOAD_IGNORE_CODE_AUTHZ_LEVEL = 0x00000010,
#else
        // POSIX-specific flags
        RTLD_LAZY = 0x00001,       // Lazy binding
        RTLD_NOW = 0x00002,        // Immediate binding
        RTLD_LOCAL = 0x00000,      // Symbols not available globally
        RTLD_GLOBAL = 0x00100,     // Symbols available globally
        RTLD_NODELETE = 0x01000,   // Don't unload on dlclose()
        RTLD_NOLOAD = 0x00004,     // Don't load, just check if loaded
        RTLD_DEEPBIND = 0x00008,   // Use deep binding
#endif
    };

private:
    Handle m_handle = nullptr;                 ///< Platform-specific library handle
    std::filesystem::path m_path;              ///< Path to the loaded library
};

/**
 * @brief Loads a dynamic library with error handling
 * @param libraryPath Path to the dynamic library
 * @param showErrors Whether to log/display errors (default: true)
 * @return Library handle, or nullptr on failure
 */
[[nodiscard]] DynamicLibrary::Handle DynamicLibraryOpen(const char* libraryPath, bool showErrors = true);

/**
 * @brief Unloads a dynamic library
 * @param libraryHandle Handle to the library to unload
 */
void DynamicLibraryClose(DynamicLibrary::Handle libraryHandle) noexcept;

/**
 * @brief Gets a symbol from a dynamic library
 * @param libraryHandle Handle to the library
 * @param symbolName Name of the symbol to retrieve
 * @return Pointer to the symbol, or nullptr if not found
 */
[[nodiscard]] DynamicLibrary::Symbol DynamicLibraryGetProc(DynamicLibrary::Handle libraryHandle, 
                                                          const char* symbolName) noexcept;

/**
 * @brief Type alias for backward compatibility
 */
using DynLibHandle = DynamicLibrary::Handle;