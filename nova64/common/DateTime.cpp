/**
 * @file DateTime.cpp
 * @brief Local-time conversion and formatting for DateTime.
 */
#include "DateTime.hpp"

#include <cstring>
#include <array>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <ctime>
#endif

static bool SafeLocalTime(const std::time_t& time, std::tm& result) {
    std::memset(&result, 0, sizeof(result));
#ifdef _WIN32
    return localtime_s(&result, &time) == 0;
#else
    return localtime_r(&time, &result) != nullptr;
#endif
}

DateTime::DateTime() : m_time(std::time(nullptr)) {}

DateTime::DateTime(std::time_t time) : m_time(time) {}

DateTime& DateTime::SetToNow() {
    m_time = std::time(nullptr);
    return *this;
}

std::string DateTime::Format(std::string_view format) const {
    std::tm tm{};
    if (!SafeLocalTime(m_time, tm)) {
        return {};
    }

    std::array<char, 256> buffer{};

    const std::string fmt(format);
    const std::size_t len = std::strftime(buffer.data(), buffer.size(), fmt.c_str(), &tm);
    if (len == 0) {
        return {};
    }
    return std::string(buffer.data(), len);
}

std::string DateTime::ToString() const { return Format("%Y-%m-%d %H:%M:%S"); }

std::string DateTime::ToISO8601() const { return Format("%Y-%m-%dT%H:%M:%S"); }

std::string DateTime::ToShortDate() const { return Format("%Y-%m-%d"); }

std::string DateTime::ToShortTime() const { return Format("%H:%M:%S"); }

DateTime DateTime::Now() { return DateTime(std::time(nullptr)); }

std::time_t DateTime::CurrentTime() { return std::time(nullptr); }

std::string DateTime::FormatTime(std::time_t time, std::string_view format) { return DateTime(time).Format(format); }
