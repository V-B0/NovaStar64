/**
 * @file DateTime.hpp
 * @brief Date Time declarations and types.
 */
#pragma once

#include <ctime>
#include <string>
#include <string_view>

class DateTime
{
public:
    DateTime();
    explicit DateTime(std::time_t time);
    
    
    DateTime& SetToNow();
    std::string Format(std::string_view format) const;
    
    
    std::string ToString() const;                     // Default format
    std::string ToISO8601() const;                    // YYYY-MM-DDTHH:MM:SS
    std::string ToShortDate() const;                  // YYYY-MM-DD
    std::string ToShortTime() const;                  // HH:MM:SS
    
    
    std::time_t GetTime() const { return m_time; }
    void SetTime(std::time_t time) { m_time = time; }
    
    
    static DateTime Now();
    static std::time_t CurrentTime();
    static std::string FormatTime(std::time_t time, std::string_view format);
    
private:
    std::time_t m_time;
};