#include "GameFiles.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace NovaUI {

std::string FileName(const std::string& path) {
    const std::u8string name = fs::path(path).filename().u8string();
    return std::string(name.begin(), name.end());
}

std::string FileStem(const std::string& path) {
    const std::u8string name = fs::path(path).stem().u8string();
    return std::string(name.begin(), name.end());
}

std::string Lowercase(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string GameKey(const std::string& romPath) {
    const std::string stem = FileStem(romPath);

    std::string key;
    bool changed = false;
    for (const unsigned char c : stem) {
        const bool plain =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        key += plain ? static_cast<char>(c) : '_';
        changed = changed || !plain;
    }
    if (key.empty()) {
        key = "game";
        changed = true;
    }
    if (changed) {
        std::uint32_t hash = 2166136261u; // FNV-1a
        for (const unsigned char c : stem) hash = (hash ^ c) * 16777619u;
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "-%08x", static_cast<unsigned>(hash));
        key += suffix;
    }
    return key;
}

bool WriteFileAtomically(const std::string& path, const std::string& text) {
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << text;
        file.close();
        if (file.fail()) {
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace NovaUI
