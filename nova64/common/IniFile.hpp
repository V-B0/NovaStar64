#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "LockGuard.hpp"
#include "file.hpp"
#include "platform.hpp"

/**
 * @class IniFileBase
 * @brief Base class for INI file parsing and manipulation
 * 
 * Provides thread-safe INI file operations with support for sections, keys, and values.
 * Case-insensitive keys, with automatic flushing and caching for performance.
 */
class IniFileBase
{
public:
    // Type aliases
    using KeyValueMap = std::map<std::string, std::string>;
    using KeyValueEntry = std::pair<const std::string*, const std::string*>;
    using KeyValueVector = std::vector<KeyValueEntry>;
    using SectionSet = std::set<std::string>;
    using StringList = std::list<std::string>;
    
    // Sorting callback type
    using SortCallback = std::function<void(KeyValueVector&)>;
    
    // Case-insensitive comparator for section/key names
    struct CaseInsensitiveCompare
    {
        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept;
    };

    /**
     * @brief Constructs an INI file handler
     * @param fileObject File object for I/O operations
     * @param fileName Path to the INI file
     */
    IniFileBase(FileSystem::File& fileObject, const std::filesystem::path& fileName);
    
    virtual ~IniFileBase();
    
    // Non-copyable, non-movable (file handles are not transferable)
    IniFileBase(const IniFileBase&) = delete;
    IniFileBase(IniFileBase&&) = delete;
    IniFileBase& operator=(const IniFileBase&) = delete;
    IniFileBase& operator=(IniFileBase&&) = delete;
    
    // File operations
    [[nodiscard]] bool IsEmpty() const;
    [[nodiscard]] bool IsFileOpen() const;
    [[nodiscard]] bool IsReadOnly() const;
    
    // Section operations
    bool DeleteSection(std::string_view sectionName);
    void GetSections(SectionSet& sections) const;
    [[nodiscard]] SectionSet GetSections() const;
    
    // String value operations
    [[nodiscard]] std::optional<std::string> GetString(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] std::string GetString(
        std::string_view sectionName, 
        std::string_view keyName, 
        std::string_view defaultValue) const;
    
    bool GetString(
        std::string_view sectionName, 
        std::string_view keyName, 
        std::string_view defaultValue, 
        std::string& output) const;
    
    // Numeric value operations
    [[nodiscard]] std::optional<int32_t> GetNumber(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] int32_t GetNumber(
        std::string_view sectionName, 
        std::string_view keyName, 
        int32_t defaultValue) const;
    
    bool GetNumber(
        std::string_view sectionName, 
        std::string_view keyName, 
        int32_t defaultValue, 
        int32_t& output) const;
    
    // Floating point operations
    [[nodiscard]] std::optional<float> GetFloat(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] float GetFloat(
        std::string_view sectionName, 
        std::string_view keyName, 
        float defaultValue) const;
    
    // Boolean operations
    [[nodiscard]] std::optional<bool> GetBool(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] bool GetBool(
        std::string_view sectionName, 
        std::string_view keyName, 
        bool defaultValue) const;
    
    // Write operations
    virtual void SetString(
        std::string_view sectionName, 
        std::string_view keyName, 
        std::string_view value);
    
    virtual void SetNumber(
        std::string_view sectionName, 
        std::string_view keyName, 
        int32_t value);
    
    virtual void SetFloat(
        std::string_view sectionName, 
        std::string_view keyName, 
        float value);
    
    virtual void SetBool(
        std::string_view sectionName, 
        std::string_view keyName, 
        bool value);
    
    // Bulk operations
    void GetKeys(std::string_view sectionName, StringList& keys) const;
    [[nodiscard]] StringList GetKeys(std::string_view sectionName) const;
    
    void GetKeyValues(std::string_view sectionName, KeyValueMap& values) const;
    [[nodiscard]] KeyValueMap GetKeyValues(std::string_view sectionName) const;
    
    // Entry management
    [[nodiscard]] bool EntryExists(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    bool DeleteKey(std::string_view sectionName, std::string_view keyName);
    
    // Sorting
    void SetCustomSort(SortCallback sortFunction);
    
    // Flushing control
    void SetAutoFlush(bool autoFlush);
    void Flush();
    
    // File information
    [[nodiscard]] const std::filesystem::path& GetFileName() const { return m_fileName; }
    
    // Static utility methods
    static std::string EscapeString(std::string_view str);
    static std::string UnescapeString(std::string_view str);
    static bool IsValidSectionName(std::string_view name);
    static bool IsValidKeyName(std::string_view name);

protected:
    // File operations
    void OpenFile(bool readOnly = false, bool createIfMissing = true);
    void SaveCurrentSection();
    
    // String formatting (internal use)
    std::string FormatString(const char* format, ...) const;
    std::string FormatStringV(const char* format, va_list args) const;

protected:
    FileSystem::File& m_file;                 ///< File I/O object
    std::filesystem::path m_fileName;         ///< INI file path
    mutable CriticalSection m_criticalSection;        ///< Thread safety
    std::atomic<bool> m_autoFlush{true};      ///< Automatic flush after writes

private:
    // Internal types
    using FilePositionMap = std::map<std::string, int64_t, CaseInsensitiveCompare>;
    using SectionDataMap = std::map<std::string, std::string, CaseInsensitiveCompare>;
    
    // Internal state
    std::string m_currentSection;             ///< Currently loaded section
    SectionDataMap m_currentSectionData;      ///< Cached data for current section
    int64_t m_currentSectionFilePos{-1};      ///< File position of current section
    bool m_currentSectionDirty{false};        ///< Whether current section has unsaved changes
    FilePositionMap m_sectionPositions;       ///< Map of section positions in file
    int64_t m_lastSectionSearch{-1};          ///< Last searched section position (optimization)
    SortCallback m_sortFunction;              ///< Custom sorting callback
    const char* m_lineEnding{"\r\n"};         ///< Line ending characters
    
    // Helper methods
    void ClearSectionPositionCache(int64_t filePos);
    bool MoveToSection(std::string_view sectionName, bool changeCurrentSection);
    [[nodiscard]] const char* CleanLine(char* line) const;
    int GetLineFromFile(char*& line, std::unique_ptr<char[]>& buffer, 
                       int& bufferSize, int& dataSize, int& readPos);
    void InsertSpaces(int position, int count);
    
    // Internal data access
    [[nodiscard]] std::optional<std::string> GetValueInternal(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    void SetValueInternal(
        std::string_view sectionName, 
        std::string_view keyName, 
        std::string_view value);
};

/**
 * @class IniFile
 * @brief Templated INI file implementation with specific file storage
 * 
 * @tparam FileStorageType Type of file storage to use (e.g., CFile, MemoryFile)
 */
template<typename FileStorageType>
class IniFile final : public IniFileBase
{
public:
    /**
     * @brief Opens an INI file for reading and writing
     * @param fileName Path to the INI file
     */
    explicit IniFile(const std::filesystem::path& fileName) 
        : IniFileBase(m_fileStorage, fileName)
    {
        OpenFile(false, true);
    }
    
    /**
     * @brief Opens an INI file with specific options
     * @param fileName Path to the INI file
     * @param createIfMissing Create file if it doesn't exist
     * @param readOnly Open in read-only mode
     */
    explicit IniFile(const std::filesystem::path& fileName, 
                    bool createIfMissing, 
                    bool readOnly)
        : IniFileBase(m_fileStorage, fileName)
    {
        OpenFile(readOnly, createIfMissing);
    }
    
    ~IniFile() override
    {
        SaveCurrentSection();
    }
    
    // Allow moving but not copying
    IniFile(IniFile&&) = default;
    IniFile& operator=(IniFile&&) = default;
    
    // Non-copyable
    IniFile(const IniFile&) = delete;
    IniFile& operator=(const IniFile&) = delete;

private:
    FileStorageType m_fileStorage;  ///< File storage implementation
};

// Common aliases
using CIniFile = IniFile<FileSystem::File>;    ///< Regular file-based INI

/**
 * @brief Lightweight INI reader (read-only, faster than full IniFile)
 */
class IniReader
{
public:
    explicit IniReader(const std::filesystem::path& fileName);
    
    [[nodiscard]] std::optional<std::string> GetString(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] std::optional<int32_t> GetNumber(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] std::optional<float> GetFloat(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] std::optional<bool> GetBool(
        std::string_view sectionName, 
        std::string_view keyName) const;
    
    [[nodiscard]] bool SectionExists(std::string_view sectionName) const;
    [[nodiscard]] bool KeyExists(std::string_view sectionName, 
                                 std::string_view keyName) const;
    
    void GetSections(IniFileBase::SectionSet& sections) const;
    void GetKeys(std::string_view sectionName, 
                 IniFileBase::StringList& keys) const;
    
private:
    std::map<std::string, IniFileBase::KeyValueMap, 
             IniFileBase::CaseInsensitiveCompare> m_data;
};