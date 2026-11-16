/**
 * @file IniFile.hpp
 * @brief Minimal INI configuration reader/writer.
 *
 * Supports the usual INI shape: `[section]` headers, `key = value` pairs, and
 * `;` / `#` comments (a whole line, or inline after whitespace). Section and key
 * lookups are case-insensitive. The file is written back only when something
 * changed, and writing regenerates it, so comments in it are not preserved.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

class CIniFile {
public:
    /// Opens (and parses) fileName. A missing file is not an error when
    /// createIfMissing is set — it is created on the first Flush instead.
    explicit CIniFile(const std::filesystem::path& fileName, bool readOnly = false, bool createIfMissing = true);
    ~CIniFile();

    CIniFile(const CIniFile&) = delete;
    CIniFile& operator=(const CIniFile&) = delete;

    [[nodiscard]] bool IsFileOpen() const { return m_open; }
    [[nodiscard]] bool IsReadOnly() const { return m_readOnly; }

    /// Decimal, or hexadecimal with a 0x prefix. Empty if the value is not entirely a number.
    [[nodiscard]] std::optional<int32_t> GetNumber(std::string_view section, std::string_view key) const;
    [[nodiscard]] int32_t GetNumber(std::string_view section, std::string_view key, int32_t defaultValue) const;

    /// Accepts 1/0, true/false, yes/no and on/off, case-insensitively.
    [[nodiscard]] std::optional<bool> GetBool(std::string_view section, std::string_view key) const;
    [[nodiscard]] bool GetBool(std::string_view section, std::string_view key, bool defaultValue) const;

    void SetString(std::string_view section, std::string_view key, std::string_view value);
    void SetBool(std::string_view section, std::string_view key, bool value);

    /// Writes pending changes to disk. A no-op when read-only or unmodified.
    /// Called automatically on destruction.
    bool Flush();

private:
    struct CaseInsensitiveLess {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept;
    };

    using KeyMap = std::map<std::string, std::string, CaseInsensitiveLess>;
    using SectionMap = std::map<std::string, KeyMap, CaseInsensitiveLess>;

    const std::string* Find(std::string_view section, std::string_view key) const;
    void Load();

    std::filesystem::path m_path;
    SectionMap m_sections;
    bool m_open = false;
    bool m_readOnly = false;
    bool m_dirty = false;
};
