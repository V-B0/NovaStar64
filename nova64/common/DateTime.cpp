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
