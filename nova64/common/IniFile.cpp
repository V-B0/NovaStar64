/**
 * @file IniFile.cpp
 * @brief Minimal INI configuration reader/writer.
 */

#include "IniFile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>

namespace {

char LowerAscii(char c) noexcept { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string_view Trim(std::string_view s) noexcept {
    const auto notSpace = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
    while (!s.empty() && !notSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && !notSpace(s.back())) s.remove_suffix(1);
    return s;
}

/// Strips a trailing `;` or `#` comment from a value. The marker only counts as
/// a comment when whitespace precedes it, so values that legitimately contain
/// these characters (paths, colour literals) survive intact.
std::string_view StripInlineComment(std::string_view s) noexcept {
    for (std::size_t i = 1; i < s.size(); ++i) {
        if ((s[i] == ';' || s[i] == '#') && std::isspace(static_cast<unsigned char>(s[i - 1]))) {
            return Trim(s.substr(0, i));
        }
    }
    return s;
}

} // namespace

bool CIniFile::CaseInsensitiveLess::operator()(std::string_view a, std::string_view b) const noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return LowerAscii(x) < LowerAscii(y); });
}

CIniFile::CIniFile(const std::filesystem::path& fileName, bool readOnly, bool createIfMissing) :
    m_path(fileName), m_readOnly(readOnly) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(m_path, ec);

    if (exists) {
        Load();
    } else {
        // A file we are allowed to create counts as open: it materialises on Flush.
        m_open = createIfMissing && !readOnly;
    }
}

CIniFile::~CIniFile() { Flush(); }

void CIniFile::Load() {
    std::ifstream in(m_path);
    if (!in) {
        m_open = false;
        return;
    }

    KeyMap* current = &m_sections[std::string{}]; // keys before any [section]
    std::string line;

    while (std::getline(in, line)) {
        std::string_view view = Trim(line);
        if (view.empty() || view.front() == ';' || view.front() == '#') continue;

        if (view.front() == '[') {
            const auto close = view.find(']');
            if (close == std::string_view::npos) continue; // malformed header
            current = &m_sections[std::string{Trim(view.substr(1, close - 1))}];
            continue;
        }

        const auto eq = view.find('=');
        if (eq == std::string_view::npos) continue; // not a key/value line

        std::string_view key = Trim(view.substr(0, eq));
        std::string_view value = StripInlineComment(Trim(view.substr(eq + 1)));
        if (key.empty()) continue;

        (*current)[std::string{key}] = std::string{value};
    }

    m_open = true;
}

const std::string* CIniFile::Find(std::string_view section, std::string_view key) const {
    const auto s = m_sections.find(section);
    if (s == m_sections.end()) return nullptr;

    const auto k = s->second.find(key);
    if (k == s->second.end()) return nullptr;

    return &k->second;
}

std::optional<std::string> CIniFile::GetString(std::string_view section, std::string_view key) const {
    if (const std::string* v = Find(section, key)) return *v;
    return std::nullopt;
}

std::string CIniFile::GetString(std::string_view section, std::string_view key, std::string_view defaultValue) const {
    if (const std::string* v = Find(section, key)) return *v;
    return std::string{defaultValue};
}

std::optional<int32_t> CIniFile::GetNumber(std::string_view section, std::string_view key) const {
    const std::string* v = Find(section, key);
    if (!v) return std::nullopt;

    std::string_view text = Trim(*v);
    if (text.empty()) return std::nullopt;

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }

    int32_t out = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out, base);
    if (result.ec != std::errc{} || result.ptr != last) return std::nullopt;

    return out;
}

int32_t CIniFile::GetNumber(std::string_view section, std::string_view key, int32_t defaultValue) const {
    return GetNumber(section, key).value_or(defaultValue);
}

std::optional<bool> CIniFile::GetBool(std::string_view section, std::string_view key) const {
    const std::string* v = Find(section, key);
    if (!v) return std::nullopt;

    std::string text{Trim(*v)};
    std::transform(text.begin(), text.end(), text.begin(), LowerAscii);

    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;

    return std::nullopt;
}

bool CIniFile::GetBool(std::string_view section, std::string_view key, bool defaultValue) const {
    return GetBool(section, key).value_or(defaultValue);
}

void CIniFile::SetString(std::string_view section, std::string_view key, std::string_view value) {
    if (m_readOnly) return;

    std::string& slot = m_sections[std::string{section}][std::string{key}];
    if (slot == value) return;

    slot = std::string{value};
    m_dirty = true;
}

void CIniFile::SetNumber(std::string_view section, std::string_view key, int32_t value) {
    SetString(section, key, std::to_string(value));
}

void CIniFile::SetBool(std::string_view section, std::string_view key, bool value) {
    SetString(section, key, value ? "1" : "0");
}

bool CIniFile::Flush() {
    if (m_readOnly || !m_dirty) return true;

    std::error_code ec;
    if (m_path.has_parent_path()) {
        std::filesystem::create_directories(m_path.parent_path(), ec);
    }

    std::ofstream out(m_path, std::ios::trunc);
    if (!out) return false;

    // Unnamed section first so its keys stay above any [header].
    if (const auto global = m_sections.find(std::string{}); global != m_sections.end()) {
        for (const auto& [key, value] : global->second) {
            out << key << " = " << value << '\n';
        }
        if (!global->second.empty()) out << '\n';
    }

    for (const auto& [name, keys] : m_sections) {
        if (name.empty()) continue;
        out << '[' << name << "]\n";
        for (const auto& [key, value] : keys) {
            out << key << " = " << value << '\n';
        }
        out << '\n';
    }

    if (!out) return false;

    m_dirty = false;
    m_open = true;
    return true;
}
