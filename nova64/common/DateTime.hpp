/**
 * @file DateTime.hpp
 * @brief A wall-clock time (seconds since the epoch) with local-time formatting helpers.
 */
#pragma once

#include <ctime>
#include <string>
#include <string_view>

class DateTime {
public:
    DateTime();
    explicit DateTime(std::time_t time);

    DateTime& SetToNow();
    /// strftime() in the local time zone. Empty if the time can't be converted or
    /// the result does not fit in 256 bytes.
    std::string Format(std::string_view format) const;

    std::string ToString() const;    // YYYY-MM-DD HH:MM:SS
    std::string ToISO8601() const;   // YYYY-MM-DDTHH:MM:SS
    std::string ToShortDate() const; // YYYY-MM-DD
    std::string ToShortTime() const; // HH:MM:SS

    std::time_t GetTime() const { return m_time; }
    void SetTime(std::time_t time) { m_time = time; }

    static DateTime Now();
    static std::time_t CurrentTime();
    static std::string FormatTime(std::time_t time, std::string_view format);

private:
    std::time_t m_time;
};
