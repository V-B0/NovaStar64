#include "AppIcon.hpp"

#include <SDL.h>
#include <stb_image.h>

namespace NovaUI {

namespace {

// Generated from assets/icon/Novastar.png by CMake: a comma-separated list of
// byte values.
const unsigned char kIconPng[] = {
#include "NovastarIcon.inc"
};

} // namespace

const IconImage& AppIconImage() {
    static const IconImage image = []() {
        IconImage out;
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels =
            stbi_load_from_memory(kIconPng, static_cast<int>(sizeof(kIconPng)), &w, &h, &channels, 4);
        if (pixels) {
            out.width = w;
            out.height = h;
            out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            stbi_image_free(pixels);
        }
        return out;
    }();
    return image;
}

void ApplyAppIcon(SDL_Window* window) {
    const IconImage& icon = AppIconImage();
    if (!window || icon.width == 0) return;

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(icon.rgba.data()), icon.width,
                                                              icon.height, 32, icon.width * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return;
    SDL_SetWindowIcon(window, surface); // SDL copies what it needs
    SDL_FreeSurface(surface);
}

} // namespace NovaUI
