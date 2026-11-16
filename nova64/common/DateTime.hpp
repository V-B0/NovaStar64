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

    /// strftime() in the local time zone. Empty if the time can't be converted or
    /// the result does not fit in 256 bytes.
    std::string Format(std::string_view format) const;

    std::string ToString() const; // YYYY-MM-DD HH:MM:SS
                                  // YYYY-MM-DDTHH:MM:SS
                                  // YYYY-MM-DD
                                  // HH:MM:SS

    std::time_t GetTime() const { return m_time; }
    void SetTime(std::time_t time) { m_time = time; }

private:
    std::time_t m_time;
};
