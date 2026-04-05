/**
 * @file IniFile.cpp
 * @brief Ini File implementation.
 */
// IniFile.cpp - INI file parsing and manipulation
// Inspired by Project64's CIniFileBase, modernized with C++17, string_view, optional
#include "IniFile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

// =============================================================================
// CaseInsensitiveCompare
// =============================================================================

bool IniFileBase::CaseInsensitiveCompare::operator()(
    std::string_view a, std::string_view b) const noexcept
{
    const size_t len = std::min(a.size(), b.size());
    for (size_t i = 0; i < len; ++i)
    {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb)
        {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

// =============================================================================
// Helpers - Normalize section name
// =============================================================================

static std::string NormalizeSection(std::string_view name)
{
    if (name.empty())
    {
        return "default";
    }
    return std::string(name);
}

// =============================================================================
// IniFileBase - Constructor / Destructor
// =============================================================================

IniFileBase::IniFileBase(FileSystem::File& fileObject, const std::filesystem::path& fileName)
    : m_file(fileObject)
    , m_fileName(fileName)
{
}

IniFileBase::~IniFileBase()
{
    SaveCurrentSection();
}

// =============================================================================
// IniFileBase - File state queries
// =============================================================================

bool IniFileBase::IsEmpty() const
{
    if (!m_file.is_open())
    {
        return true;
    }
    return m_file.size() == 0;
}

bool IniFileBase::IsFileOpen() const
{
    return m_file.is_open();
}

bool IniFileBase::IsReadOnly() const
{
    if (!m_file.is_open())
    {
        return true;
    }
    const auto mode = m_file.open_mode();
    return !FileSystem::has_flag(mode, FileSystem::OpenMode::Write) &&
           !FileSystem::has_flag(mode, FileSystem::OpenMode::ReadWrite);
}

// =============================================================================
// IniFileBase - Low-level file I/O helpers
// =============================================================================

int IniFileBase::GetLineFromFile(char*& line, std::unique_ptr<char[]>& buffer,
                                  int& bufferSize, int& dataSize, int& readPos)
{
    constexpr int BufferIncrease = 0x2000;

    if (bufferSize == 0)
    {
        readPos = 0;
        bufferSize = BufferIncrease;
        buffer.reset(new char[bufferSize]);
        dataSize = static_cast<int>(m_file.read(&buffer[dataSize], bufferSize));
    }

    for (;;)
    {
        for (int count = readPos; count < dataSize; ++count)
        {
            if (buffer[count] == '\n')
            {
                int len = (count - readPos) + 1;
                line = &buffer[readPos];
                line[len - 1] = '\0';
                readPos = count + 1;
                return len;
            }
        }

        if (readPos != 0)
        {
            if ((dataSize - readPos) > 0)
            {
                std::memmove(buffer.get(), &buffer[readPos], dataSize - readPos);
            }
            dataSize -= readPos;
            readPos = 0;
        }
        else
        {
            // Increase buffer size
            int newMaxSize = bufferSize + BufferIncrease;
            auto newBuffer = std::make_unique<char[]>(newMaxSize);
            std::memcpy(newBuffer.get(), buffer.get(), dataSize);
            bufferSize = newMaxSize;
            buffer = std::move(newBuffer);
        }

        int bytesRead = static_cast<int>(m_file.read(&buffer[dataSize], bufferSize - dataSize));
        if (bytesRead == 0)
        {
            if (dataSize > 0)
            {
                int len = dataSize + 1;
                line = &buffer[readPos];
                line[len - 1] = '\0';
                dataSize = 0;
                readPos = 0;
                return len;
            }
            return -1;
        }
        dataSize += bytesRead;
    }
}

const char* IniFileBase::CleanLine(char* line) const
{
    // Remove // comments (but not :// as in URLs)
    char* pos = line;
    while (pos != nullptr)
    {
        pos = std::strchr(pos, '/');
        if (pos != nullptr)
        {
            if (pos[1] == '/')
            {
                if (pos > line)
                {
                    char* prev = pos - 1;
                    if (*prev != ':')
                    {
                        *pos = '\0';
                    }
                    else
                    {
                        ++pos;
                    }
                }
                else
                {
                    *pos = '\0';
                }
            }
            else
            {
                ++pos;
            }
        }
    }

    // Strip trailing whitespace and \r
    for (int count = static_cast<int>(std::strlen(line)) - 1; count >= 0; --count)
    {
        if (line[count] != ' ' && line[count] != '\r' && line[count] != '\t')
        {
            break;
        }
        line[count] = '\0';
    }

    return line;
}

void IniFileBase::InsertSpaces(int position, int count)
{
    constexpr int MoveSize = 0x2000;
    unsigned char data[MoveSize + 1];

    m_file.seek(0, FileSystem::SeekOrigin::End);
    int64_t end = m_file.tell();

    if (count > 0)
    {
        // Insert space: shift content forward
        std::string spaceBuf(static_cast<size_t>(count), ' ');

        do
        {
            int sizeToRead = static_cast<int>(end - position);
            if (sizeToRead > MoveSize)
            {
                sizeToRead = MoveSize;
            }
            if (sizeToRead > 0)
            {
                m_file.seek(static_cast<int64_t>(-sizeToRead), FileSystem::SeekOrigin::Current);
                int64_t writePos = m_file.tell();
                std::memset(data, 0, sizeof(data));
                int result = static_cast<int>(m_file.read(data, sizeToRead));
                m_file.seek(writePos, FileSystem::SeekOrigin::Begin);
                end = writePos;

                m_file.write(spaceBuf.data(), spaceBuf.size());
                m_file.write(data, result);
                m_file.seek(writePos, FileSystem::SeekOrigin::Begin);
            }
        } while (static_cast<int>(end - position) > 0);
    }
    else if (count < 0)
    {
        // Remove space: shift content backward
        int readPos = position + (-count);
        int writePos = position;

        int sizeToRead;
        do
        {
            sizeToRead = static_cast<int>(end - readPos);
            if (sizeToRead > MoveSize)
            {
                sizeToRead = MoveSize;
            }
            m_file.seek(readPos, FileSystem::SeekOrigin::Begin);
            m_file.read(data, sizeToRead);
            m_file.seek(writePos, FileSystem::SeekOrigin::Begin);
            m_file.write(data, sizeToRead);
            readPos += sizeToRead;
            writePos += sizeToRead;
        } while (sizeToRead > 0);

        // Truncate file
        m_file.seek(writePos, FileSystem::SeekOrigin::Begin);
        m_file.resize(writePos);
        m_file.seek(0, FileSystem::SeekOrigin::Begin);
    }
}

void IniFileBase::ClearSectionPositionCache(int64_t filePos)
{
    if (filePos <= 0)
    {
        m_sectionPositions.clear();
        m_lastSectionSearch = 0;
    }
    else
    {
        auto iter = m_sectionPositions.begin();
        while (iter != m_sectionPositions.end())
        {
            auto current = iter++;
            if (current->second > filePos)
            {
                m_sectionPositions.erase(current);
            }
        }
        m_lastSectionSearch = filePos;
    }
}

// =============================================================================
// IniFileBase - Section navigation
// =============================================================================

bool IniFileBase::MoveToSection(std::string_view sectionName, bool changeCurrentSection)
{
    std::string section = NormalizeSection(sectionName);

    if (section == m_currentSection)
    {
        return true;
    }

    if (changeCurrentSection)
    {
        SaveCurrentSection();
        m_currentSection.clear();
    }

    std::unique_ptr<char[]> buffer;
    char* input = nullptr;
    int bufferSize = 0, dataSize = 0, readPos = 0, result;

    auto iter = m_sectionPositions.find(section);
    bool foundSection = false;

    if (iter != m_sectionPositions.end())
    {
        if (changeCurrentSection)
        {
            m_currentSection = iter->first;
            m_currentSectionFilePos = iter->second;
        }
        m_file.seek(iter->second, FileSystem::SeekOrigin::Begin);
        foundSection = true;
    }
    else
    {
        m_file.seek(m_lastSectionSearch, FileSystem::SeekOrigin::Begin);

        // BOM detection bytes
        const uint8_t utf8Bom[3] = { 0xEF, 0xBB, 0xBF };

        do
        {
            result = GetLineFromFile(input, buffer, bufferSize, dataSize, readPos);
            if (result <= 1)
            {
                continue;
            }
            if (std::strlen(CleanLine(input)) <= 1)
            {
                continue;
            }

            // Skip UTF-8 BOM at start of file
            char* currentSection = input;
            if (m_lastSectionSearch == 0 &&
                std::memcmp(currentSection, utf8Bom, 3) == 0)
            {
                currentSection += 3;
            }

            if (currentSection[0] != '[')
            {
                continue;
            }

            int lineEndPos = static_cast<int>(std::strlen(currentSection)) - 1;
            if (currentSection[lineEndPos] != ']')
            {
                continue;
            }

            // Strip brackets
            currentSection[lineEndPos] = '\0';
            ++currentSection;

            m_lastSectionSearch = (m_file.tell() - dataSize) + readPos;
            m_sectionPositions.insert({ std::string(currentSection), m_lastSectionSearch });

            if (_stricmp(section.c_str(), currentSection) != 0)
            {
                continue;
            }

            if (changeCurrentSection)
            {
                m_currentSection = section;
                m_currentSectionFilePos = m_lastSectionSearch;
            }
            else
            {
                m_file.seek(m_lastSectionSearch, FileSystem::SeekOrigin::Begin);
            }
            foundSection = true;
            break;
        } while (result >= 0);
    }

    // Handle "default" section at file start
    if (!foundSection && section == "default")
    {
        m_sectionPositions.insert({ section, 0 });
        if (changeCurrentSection)
        {
            m_currentSection = section;
            m_currentSectionFilePos = 0;
        }
        m_file.seek(m_lastSectionSearch, FileSystem::SeekOrigin::Begin);
        foundSection = true;
    }

    // Load section key-value data
    if (foundSection && changeCurrentSection)
    {
        m_currentSectionData.clear();
        do
        {
            result = GetLineFromFile(input, buffer, bufferSize, dataSize, readPos);
            if (result <= 1)
            {
                continue;
            }
            if (std::strlen(CleanLine(input)) <= 1)
            {
                continue;
            }
            if (input[0] == '[')
            {
                break;
            }

            char* pos = std::strchr(input, '=');
            if (pos == nullptr)
            {
                continue;
            }

            // Split key=value, trim trailing whitespace from key
            char* value = &pos[1];
            char* keyEnd = pos - 1;
            while (((*keyEnd == ' ') || (*keyEnd == '\t')) && (keyEnd > input))
            {
                --keyEnd;
            }
            keyEnd[1] = '\0';

            m_currentSectionData.insert({ std::string(input), std::string(value) });
        } while (result >= 0);
    }

    return foundSection;
}

// =============================================================================
// IniFileBase - Save current section to file
// =============================================================================

void IniFileBase::SaveCurrentSection()
{
    if (!m_currentSectionDirty || IsReadOnly())
    {
        return;
    }
    m_currentSectionDirty = false;

    if (m_currentSection.empty())
    {
        m_currentSection = "default";
    }

    const int lineFeedLen = static_cast<int>(std::strlen(m_lineEnding));

    if (m_currentSectionFilePos == -1)
    {
        // New section: append to end of file
        m_file.seek(0, FileSystem::SeekOrigin::End);

        std::string sectionHeader;
        if (m_file.size() < static_cast<int64_t>(std::strlen(m_lineEnding)))
        {
            sectionHeader = "[" + m_currentSection + "]" + m_lineEnding;
        }
        else
        {
            sectionHeader = std::string(m_lineEnding) + "[" + m_currentSection + "]" + m_lineEnding;
        }

        m_file.write(sectionHeader.data(), sectionHeader.size());
        m_currentSectionFilePos = m_file.tell();
        m_sectionPositions.insert({ m_currentSection, m_currentSectionFilePos });
    }
    else
    {
        // Existing section: calculate needed space vs current space
        int neededBufferLen = 0;
        {
            for (auto& [key, val] : m_currentSectionData)
            {
                // key=value\r\n
                neededBufferLen += static_cast<int>(key.size()) + static_cast<int>(val.size()) + lineFeedLen + 1;
            }
        }

        int currentLen = 0;
        m_file.seek(m_currentSectionFilePos, FileSystem::SeekOrigin::Begin);

        int bufferSize = 0, dataSize = 0, readPos = 0, result;
        std::unique_ptr<char[]> data;
        char* input = nullptr;

        int64_t startPos = m_currentSectionFilePos;
        int64_t endPos = startPos;

        do
        {
            result = GetLineFromFile(input, data, bufferSize, dataSize, readPos);
            if (result <= 1)
            {
                continue;
            }
            if (std::strlen(CleanLine(input)) <= 1 || input[0] != '[')
            {
                endPos = (m_file.tell() - dataSize) + readPos;
                continue;
            }
            if (input[0] == '[')
            {
                neededBufferLen += lineFeedLen;
            }
            break;
        } while (result >= 0);

        currentLen = static_cast<int>(endPos - startPos);

        if (neededBufferLen != currentLen)
        {
            InsertSpaces(static_cast<int>(startPos), neededBufferLen - currentLen);
            m_file.flush();
            ClearSectionPositionCache(startPos);
        }

        m_file.seek(startPos, FileSystem::SeekOrigin::Begin);
    }

    // Write key=value pairs
    if (m_sortFunction != nullptr)
    {
        KeyValueVector sortData;
        for (auto& [key, val] : m_currentSectionData)
        {
            sortData.push_back({ &key, &val });
        }
        m_sortFunction(sortData);

        for (auto& [keyPtr, valPtr] : sortData)
        {
            std::string line = *keyPtr + "=" + *valPtr + m_lineEnding;
            m_file.write(line.data(), line.size());
        }
    }
    else
    {
        for (auto& [key, val] : m_currentSectionData)
        {
            std::string line = key + "=" + val + m_lineEnding;
            m_file.write(line.data(), line.size());
        }
    }

    m_file.flush();
}

// =============================================================================
// IniFileBase - OpenFile
// =============================================================================

void IniFileBase::OpenFile(bool readOnly, bool createIfMissing)
{
    if (readOnly)
    {
        try
        {
            m_file.open(m_fileName.string(), FileSystem::OpenMode::Read);
        }
        catch (...)
        {
            // File doesn't exist or can't be opened
        }
    }
    else
    {
        try
        {
            m_file.open(m_fileName.string(),
                        FileSystem::OpenMode::ReadWrite,
                        FileSystem::ShareMode::Read);
        }
        catch (...)
        {
            // Try read-only
            try
            {
                m_file.open(m_fileName.string(), FileSystem::OpenMode::Read);
            }
            catch (...)
            {
                if (createIfMissing)
                {
                    try
                    {
                        m_file.open(m_fileName.string(),
                                    FileSystem::OpenMode::ReadWrite | FileSystem::OpenMode::Create,
                                    FileSystem::ShareMode::Read);
                    }
                    catch (...)
                    {
                        return;
                    }
                }
            }
        }
    }

    if (m_file.is_open())
    {
        m_file.seek(0, FileSystem::SeekOrigin::Begin);
    }
}

// =============================================================================
// IniFileBase - Internal get/set
// =============================================================================

std::optional<std::string> IniFileBase::GetValueInternal(
    std::string_view sectionName, std::string_view keyName) const
{
    LockGuard guard(const_cast<CriticalSection&>(m_criticalSection));

    std::string section = NormalizeSection(sectionName);

    if (m_file.is_open() && const_cast<IniFileBase*>(this)->MoveToSection(section, true))
    {
        auto iter = m_currentSectionData.find(std::string(keyName));
        if (iter != m_currentSectionData.end())
        {
            return iter->second;
        }
    }

    return std::nullopt;
}

void IniFileBase::SetValueInternal(
    std::string_view sectionName, std::string_view keyName, std::string_view value)
{
    if (IsReadOnly())
    {
        return;
    }

    LockGuard guard(m_criticalSection);

    if (!m_file.is_open())
    {
        OpenFile(false, true);
        if (!m_file.is_open())
        {
            return;
        }
    }

    std::string section = NormalizeSection(sectionName);
    std::string key(keyName);

    if (!MoveToSection(section, true))
    {
        m_currentSection = section;
        m_currentSectionData.clear();
        m_currentSectionFilePos = -1;
    }

    auto iter = m_currentSectionData.find(key);
    if (iter != m_currentSectionData.end())
    {
        if (iter->second != value)
        {
            iter->second = std::string(value);
            m_currentSectionDirty = true;
        }
    }
    else
    {
        m_currentSectionData.insert({ key, std::string(value) });
        m_currentSectionDirty = true;
    }

    if (m_autoFlush.load(std::memory_order_relaxed))
    {
        SaveCurrentSection();
    }
}

// =============================================================================
// IniFileBase - Section operations
// =============================================================================

bool IniFileBase::DeleteSection(std::string_view sectionName)
{
    LockGuard guard(m_criticalSection);

    if (!m_file.is_open())
    {
        return false;
    }

    std::string section = NormalizeSection(sectionName);

    SaveCurrentSection();
    if (!MoveToSection(section, true))
    {
        return false;
    }

    m_file.seek(m_currentSectionFilePos, FileSystem::SeekOrigin::Begin);

    int64_t deleteSectionStart = m_currentSectionFilePos -
        static_cast<int64_t>(section.size() + std::strlen(m_lineEnding) + 2);
    int64_t nextSectionStart = -1;

    {
        int bufferSize = 0, dataSize = 0, readPos = 0, result;
        int nextLine = 0;
        std::unique_ptr<char[]> data;
        char* input = nullptr;

        do
        {
            result = GetLineFromFile(input, data, bufferSize, dataSize, readPos);
            if (result <= 1)
            {
                continue;
            }
            if (std::strlen(CleanLine(input)) <= 1)
            {
                continue;
            }

            if (input[0] != '[')
            {
                nextLine = static_cast<int>((m_file.tell() - dataSize) + readPos);
                continue;
            }
            nextSectionStart = nextLine != 0 ? nextLine :
                static_cast<int64_t>((m_file.tell() - dataSize) + readPos);
            break;
        } while (result >= 0);
    }

    if (nextSectionStart != -1)
    {
        // Shift subsequent sections up
        m_file.seek(0, FileSystem::SeekOrigin::End);
        int64_t end = m_file.tell();
        int64_t rPos = nextSectionStart;
        int64_t wPos = deleteSectionStart;

        constexpr int MoveSize = 0x2000;
        unsigned char moveData[MoveSize + 1];
        int sizeToRead;

        do
        {
            sizeToRead = static_cast<int>(end - rPos);
            if (sizeToRead > MoveSize)
            {
                sizeToRead = MoveSize;
            }
            m_file.seek(rPos, FileSystem::SeekOrigin::Begin);
            m_file.read(moveData, sizeToRead);
            m_file.seek(wPos, FileSystem::SeekOrigin::Begin);
            m_file.write(moveData, sizeToRead);
            rPos += sizeToRead;
            wPos += sizeToRead;
        } while (sizeToRead > 0);

        m_file.resize(deleteSectionStart + (end - nextSectionStart));
    }
    else
    {
        // Last section: just truncate
        m_file.resize(deleteSectionStart);
    }

    m_file.flush();
    ClearSectionPositionCache(0);
    m_currentSection.clear();
    m_currentSectionData.clear();
    m_currentSectionFilePos = -1;
    return true;
}

void IniFileBase::GetSections(SectionSet& sections) const
{
    sections.clear();

    LockGuard guard(const_cast<CriticalSection&>(m_criticalSection));
    if (!m_file.is_open())
    {
        return;
    }

    // Force full file scan by searching for a section that can't exist
    static std::mt19937 rng(std::random_device{}());
    std::string fakeName = "DoesNotExist" + std::to_string(rng()) + std::to_string(rng());
    const_cast<IniFileBase*>(this)->MoveToSection(fakeName, false);

    for (auto& [name, pos] : m_sectionPositions)
    {
        sections.insert(name);
    }
}

IniFileBase::SectionSet IniFileBase::GetSections() const
{
    SectionSet sections;
    GetSections(sections);
    return sections;
}

// =============================================================================
// IniFileBase - String value operations
// =============================================================================

std::optional<std::string> IniFileBase::GetString(
    std::string_view sectionName, std::string_view keyName) const
{
    return GetValueInternal(sectionName, keyName);
}

std::string IniFileBase::GetString(
    std::string_view sectionName, std::string_view keyName,
    std::string_view defaultValue) const
{
    auto result = GetValueInternal(sectionName, keyName);
    return result.value_or(std::string(defaultValue));
}

bool IniFileBase::GetString(
    std::string_view sectionName, std::string_view keyName,
    std::string_view defaultValue, std::string& output) const
{
    auto result = GetValueInternal(sectionName, keyName);
    if (result.has_value())
    {
        output = result.value();
        return true;
    }
    output = defaultValue;
    return false;
}

// =============================================================================
// IniFileBase - Numeric value operations
// =============================================================================

std::optional<int32_t> IniFileBase::GetNumber(
    std::string_view sectionName, std::string_view keyName) const
{
    auto strVal = GetValueInternal(sectionName, keyName);
    if (!strVal.has_value())
    {
        return std::nullopt;
    }

    int32_t value = 0;
    auto [ptr, ec] = std::from_chars(
        strVal->data(), strVal->data() + strVal->size(), value);
    if (ec != std::errc{})
    {
        // Fallback: try sscanf for hex/octal formats
        if (std::sscanf(strVal->c_str(), "%d", &value) != 1)
        {
            return std::nullopt;
        }
    }
    return value;
}

int32_t IniFileBase::GetNumber(
    std::string_view sectionName, std::string_view keyName,
    int32_t defaultValue) const
{
    return GetNumber(sectionName, keyName).value_or(defaultValue);
}

bool IniFileBase::GetNumber(
    std::string_view sectionName, std::string_view keyName,
    int32_t defaultValue, int32_t& output) const
{
    auto result = GetNumber(sectionName, keyName);
    if (result.has_value())
    {
        output = result.value();
        return true;
    }
    output = defaultValue;
    return false;
}

// =============================================================================
// IniFileBase - Float operations
// =============================================================================

std::optional<float> IniFileBase::GetFloat(
    std::string_view sectionName, std::string_view keyName) const
{
    auto strVal = GetValueInternal(sectionName, keyName);
    if (!strVal.has_value())
    {
        return std::nullopt;
    }

    try
    {
        return std::stof(*strVal);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

float IniFileBase::GetFloat(
    std::string_view sectionName, std::string_view keyName,
    float defaultValue) const
{
    return GetFloat(sectionName, keyName).value_or(defaultValue);
}

// =============================================================================
// IniFileBase - Bool operations
// =============================================================================

std::optional<bool> IniFileBase::GetBool(
    std::string_view sectionName, std::string_view keyName) const
{
    auto strVal = GetValueInternal(sectionName, keyName);
    if (!strVal.has_value())
    {
        return std::nullopt;
    }

    const std::string& s = *strVal;

    // Handle numeric: 0 = false, non-zero = true
    if (!s.empty() && (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-'))
    {
        auto num = GetNumber(sectionName, keyName);
        if (num.has_value())
        {
            return num.value() != 0;
        }
    }

    // Handle string values
    auto lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "true" || lower == "yes" || lower == "on")
    {
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "off")
    {
        return false;
    }

    return std::nullopt;
}

bool IniFileBase::GetBool(
    std::string_view sectionName, std::string_view keyName,
    bool defaultValue) const
{
    return GetBool(sectionName, keyName).value_or(defaultValue);
}

// =============================================================================
// IniFileBase - Write operations
// =============================================================================

void IniFileBase::SetString(
    std::string_view sectionName, std::string_view keyName, std::string_view value)
{
    SetValueInternal(sectionName, keyName, value);
}

void IniFileBase::SetNumber(
    std::string_view sectionName, std::string_view keyName, int32_t value)
{
    SetValueInternal(sectionName, keyName, std::to_string(value));
}

void IniFileBase::SetFloat(
    std::string_view sectionName, std::string_view keyName, float value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", value);
    SetValueInternal(sectionName, keyName, buf);
}

void IniFileBase::SetBool(
    std::string_view sectionName, std::string_view keyName, bool value)
{
    SetValueInternal(sectionName, keyName, value ? "1" : "0");
}

// =============================================================================
// IniFileBase - Bulk operations
// =============================================================================

void IniFileBase::GetKeys(std::string_view sectionName, StringList& keys) const
{
    keys.clear();

    LockGuard guard(const_cast<CriticalSection&>(m_criticalSection));
    if (!m_file.is_open())
    {
        return;
    }

    std::string section = NormalizeSection(sectionName);
    if (const_cast<IniFileBase*>(this)->MoveToSection(section, true))
    {
        for (auto& [key, val] : m_currentSectionData)
        {
            keys.push_back(key);
        }
    }
}

IniFileBase::StringList IniFileBase::GetKeys(std::string_view sectionName) const
{
    StringList keys;
    GetKeys(sectionName, keys);
    return keys;
}

void IniFileBase::GetKeyValues(std::string_view sectionName, KeyValueMap& values) const
{
    values.clear();

    LockGuard guard(const_cast<CriticalSection&>(m_criticalSection));
    if (!m_file.is_open())
    {
        return;
    }

    std::string section = NormalizeSection(sectionName);

    // Don't change current section — read directly from file
    if (!const_cast<IniFileBase*>(this)->MoveToSection(section, false))
    {
        return;
    }

    int bufferSize = 0, dataSize = 0, readPos = 0, result;
    std::unique_ptr<char[]> data;
    char* input = nullptr;

    do
    {
        result = const_cast<IniFileBase*>(this)->GetLineFromFile(
            input, data, bufferSize, dataSize, readPos);
        if (result <= 1)
        {
            continue;
        }
        if (std::strlen(const_cast<IniFileBase*>(this)->CleanLine(input)) <= 1)
        {
            continue;
        }
        if (input[0] == '[')
        {
            break;
        }

        char* pos = std::strchr(input, '=');
        if (pos == nullptr)
        {
            continue;
        }
        *pos = '\0';

        // Trim key
        std::string key(input);
        auto endIt = key.find_last_not_of(" \t");
        if (endIt != std::string::npos)
        {
            key.resize(endIt + 1);
        }

        values.insert({ key, std::string(&pos[1]) });
    } while (result >= 0);
}

IniFileBase::KeyValueMap IniFileBase::GetKeyValues(std::string_view sectionName) const
{
    KeyValueMap values;
    GetKeyValues(sectionName, values);
    return values;
}

// =============================================================================
// IniFileBase - Entry management
// =============================================================================

bool IniFileBase::EntryExists(
    std::string_view sectionName, std::string_view keyName) const
{
    return GetValueInternal(sectionName, keyName).has_value();
}

bool IniFileBase::DeleteKey(std::string_view sectionName, std::string_view keyName)
{
    if (IsReadOnly())
    {
        return false;
    }

    LockGuard guard(m_criticalSection);

    if (!m_file.is_open())
    {
        return false;
    }

    std::string section = NormalizeSection(sectionName);
    std::string key(keyName);

    if (!MoveToSection(section, true))
    {
        return false;
    }

    auto iter = m_currentSectionData.find(key);
    if (iter == m_currentSectionData.end())
    {
        return false;
    }

    m_currentSectionData.erase(iter);
    m_currentSectionDirty = true;

    if (m_autoFlush.load(std::memory_order_relaxed))
    {
        SaveCurrentSection();
    }
    return true;
}

// =============================================================================
// IniFileBase - Sorting
// =============================================================================

void IniFileBase::SetCustomSort(SortCallback sortFunction)
{
    LockGuard guard(m_criticalSection);
    m_sortFunction = std::move(sortFunction);
}

// =============================================================================
// IniFileBase - Flushing
// =============================================================================

void IniFileBase::SetAutoFlush(bool autoFlush)
{
    m_autoFlush.store(autoFlush, std::memory_order_relaxed);
    if (autoFlush)
    {
        Flush();
    }
}

void IniFileBase::Flush()
{
    LockGuard guard(m_criticalSection);
    SaveCurrentSection();
}

// =============================================================================
// IniFileBase - String formatting
// =============================================================================

std::string IniFileBase::FormatString(const char* format, ...) const
{
    va_list args;
    va_start(args, format);
    std::string result = FormatStringV(format, args);
    va_end(args);
    return result;
}

std::string IniFileBase::FormatStringV(const char* format, va_list args) const
{
    va_list argsCopy;
    va_copy(argsCopy, args);

#ifdef _WIN32
    int len = _vscprintf(format, argsCopy);
#else
    int len = vsnprintf(nullptr, 0, format, argsCopy);
#endif
    va_end(argsCopy);

    if (len <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(len), '\0');
    std::vsnprintf(result.data(), result.size() + 1, format, args);
    return result;
}

// =============================================================================
// IniFileBase - Static utility methods
// =============================================================================

std::string IniFileBase::EscapeString(std::string_view str)
{
    std::string result;
    result.reserve(str.size());

    for (char c : str)
    {
        switch (c)
        {
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        case '=':  result += "\\="; break;
        case '[':  result += "\\["; break;
        case ']':  result += "\\]"; break;
        case ';':  result += "\\;"; break;
        case '#':  result += "\\#"; break;
        default:   result += c; break;
        }
    }
    return result;
}

std::string IniFileBase::UnescapeString(std::string_view str)
{
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '\\' && i + 1 < str.size())
        {
            switch (str[i + 1])
            {
            case '\\': result += '\\'; ++i; break;
            case 'n':  result += '\n'; ++i; break;
            case 'r':  result += '\r'; ++i; break;
            case 't':  result += '\t'; ++i; break;
            case '=':  result += '=';  ++i; break;
            case '[':  result += '[';  ++i; break;
            case ']':  result += ']';  ++i; break;
            case ';':  result += ';';  ++i; break;
            case '#':  result += '#';  ++i; break;
            default:   result += str[i]; break;
            }
        }
        else
        {
            result += str[i];
        }
    }
    return result;
}

bool IniFileBase::IsValidSectionName(std::string_view name)
{
    if (name.empty())
    {
        return false;
    }

    for (char c : name)
    {
        if (c == '[' || c == ']' || c == '\n' || c == '\r')
        {
            return false;
        }
    }
    return true;
}

bool IniFileBase::IsValidKeyName(std::string_view name)
{
    if (name.empty())
    {
        return false;
    }

    for (char c : name)
    {
        if (c == '=' || c == '\n' || c == '\r')
        {
            return false;
        }
    }
    return true;
}

// =============================================================================
// IniReader - Lightweight read-only INI parser
// =============================================================================

IniReader::IniReader(const std::filesystem::path& fileName)
{
    std::ifstream file(fileName);
    if (!file.is_open())
    {
        return;
    }

    std::string currentSection;
    std::string line;

    while (std::getline(file, line))
    {
        // Strip \r
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        // Strip leading/trailing whitespace
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
        {
            continue;
        }
        line = line.substr(start);

        // Skip comments
        if (line[0] == ';' || line[0] == '#')
        {
            continue;
        }
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/')
        {
            continue;
        }

        // Check for section header
        if (line[0] == '[')
        {
            auto endBracket = line.find(']');
            if (endBracket != std::string::npos)
            {
                currentSection = line.substr(1, endBracket - 1);
                // Ensure section exists in map
                m_data[currentSection];
            }
            continue;
        }

        // Parse key=value
        auto eqPos = line.find('=');
        if (eqPos == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);

        // Trim key
        auto keyEnd = key.find_last_not_of(" \t");
        if (keyEnd != std::string::npos)
        {
            key.resize(keyEnd + 1);
        }

        if (currentSection.empty())
        {
            currentSection = "default";
        }

        m_data[currentSection][key] = value;
    }
}

std::optional<std::string> IniReader::GetString(
    std::string_view sectionName, std::string_view keyName) const
{
    std::string section = sectionName.empty() ? "default" : std::string(sectionName);

    auto secIt = m_data.find(section);
    if (secIt == m_data.end())
    {
        return std::nullopt;
    }

    auto keyIt = secIt->second.find(std::string(keyName));
    if (keyIt == secIt->second.end())
    {
        return std::nullopt;
    }

    return keyIt->second;
}

std::optional<int32_t> IniReader::GetNumber(
    std::string_view sectionName, std::string_view keyName) const
{
    auto strVal = GetString(sectionName, keyName);
    if (!strVal.has_value())
    {
        return std::nullopt;
    }

    int32_t value = 0;
    auto [ptr, ec] = std::from_chars(
        strVal->data(), strVal->data() + strVal->size(), value);
    if (ec != std::errc{})
    {
        return std::nullopt;
    }
    return value;
}

std::optional<float> IniReader::GetFloat(
    std::string_view sectionName, std::string_view keyName) const
{
    auto strVal = GetString(sectionName, keyName);
    if (!strVal.has_value())
    {
        return std::nullopt;
    }

    try
    {
        return std::stof(*strVal);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<bool> IniReader::GetBool(
    std::string_view sectionName, std::string_view keyName) const
{
    auto strVal = GetString(sectionName, keyName);
    if (!strVal.has_value())
    {
        return std::nullopt;
    }

    const std::string& s = *strVal;

    if (!s.empty() && (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-'))
    {
        auto num = GetNumber(sectionName, keyName);
        if (num.has_value())
        {
            return num.value() != 0;
        }
    }

    auto lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "true" || lower == "yes" || lower == "on") return true;
    if (lower == "false" || lower == "no" || lower == "off") return false;

    return std::nullopt;
}

bool IniReader::SectionExists(std::string_view sectionName) const
{
    std::string section = sectionName.empty() ? "default" : std::string(sectionName);
    return m_data.find(section) != m_data.end();
}

bool IniReader::KeyExists(
    std::string_view sectionName, std::string_view keyName) const
{
    return GetString(sectionName, keyName).has_value();
}

void IniReader::GetSections(IniFileBase::SectionSet& sections) const
{
    sections.clear();
    for (auto& [name, data] : m_data)
    {
        sections.insert(name);
    }
}

void IniReader::GetKeys(
    std::string_view sectionName, IniFileBase::StringList& keys) const
{
    keys.clear();
    std::string section = sectionName.empty() ? "default" : std::string(sectionName);

    auto secIt = m_data.find(section);
    if (secIt == m_data.end())
    {
        return;
    }

    for (auto& [key, val] : secIt->second)
    {
        keys.push_back(key);
    }
}
