/**
 * @file AppIcon.hpp
 * @brief The NovaStar icon, embedded in the executable.
 *
 * assets/icon/Novastar.png is compiled into the binary (see CMakeLists.txt), so
 * the icon is available wherever the program runs from and needs no file next to it.
 */
#pragma once

#include <cstdint>
#include <vector>

struct SDL_Window;

namespace NovaUI {

struct IconImage {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   ///< width * height * 4, straight (non-premultiplied) alpha
};

/// The icon, decoded once from the embedded PNG. Empty (width == 0) if decoding failed.
const IconImage& AppIconImage();

/// Give an SDL window the icon: the title bar and taskbar on Windows and Linux,
/// and the Dock icon on macOS.
void ApplyAppIcon(SDL_Window* window);

} // namespace NovaUI
