/**
 * @file LauncherGames.cpp
 * @brief Game library, its pages and dialogs, cover pictures and starting games.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"
#include "GameInfo.hpp"
#include "UiKit.hpp"

#include <glad/gl.h>
#include <imgui.h>
#include <stb_image.h>

#include "../NovaGraphics/Plugin/FrameCallback.hpp"
#include "../NovaGraphics/Texture/ImageWrite.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace NovaUI {

// --- Shared helpers ---
namespace detail {

std::string PickRomFileNative() {
#if defined(_WIN32)
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"N64 games (*.z64;*.n64;*.v64;*.ndd;*.d64)\0*.z64;*.n64;*.v64;*.ndd;*.d64\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select an N64 ROM";
    // NOCHANGEDIR: settings/, roms/ and saves/ are found relative to the working
    // directory, which the dialog would otherwise change.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    try {
        return fs::path(file).string();
    } catch (const std::exception&) {
        return {}; // a name the narrow code page can't represent
    }
#else
#if defined(__APPLE__)
    // No `of type {...}` filter: macOS matches it against UTIs, and z64/n64/v64
    // have none registered, so the ROMs would be greyed out.
    const char* cmd = "osascript -e 'POSIX path of (choose file with prompt \"Select an N64 ROM\")' 2>/dev/null";
#else
    // Whichever of the GNOME or KDE file pickers is installed.
    const char* cmd = "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --title='Select an N64 ROM';"
                      " elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename . --title 'Select an "
                      "N64 ROM'; fi 2>/dev/null";
#endif
    FILE* pipe = ::popen(cmd, "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

std::string PickImageFileNative() {
#if defined(_WIN32)
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select a cover image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    try {
        return fs::path(file).string();
    } catch (const std::exception&) {
        return {};
    }
#else
#if defined(__APPLE__)
    const char* cmd = "osascript -e 'POSIX path of (choose file of type {\"public.png\", \"public.jpeg\"} with prompt "
                      "\"Select a cover image\")' 2>/dev/null";
#else
    const char* cmd = "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --title='Select a cover "
                      "image' --file-filter='Pictures | *.png *.jpg *.jpeg';"
                      " elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename . 'Pictures (*.png "
                      "*.jpg *.jpeg)' --title 'Select a cover image'; fi 2>/dev/null";
#endif
    FILE* pipe = ::popen(cmd, "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

// Copies a cover picture into covers/ (inside the data folder, so the library keeps
// working if the original moves) and returns its path relative to that folder.
// Returns an empty string if it couldn't be copied.
std::string ImportCoverImage(const std::string& source, const std::string& gameName) {
    std::error_code ec;
    const fs::path src(source);
    if (!fs::is_regular_file(src, ec)) return {};

    // Already in covers/? Then there is nothing to copy.
    const fs::path covers = fs::path("covers");
    const fs::path absCovers = fs::absolute(covers, ec).lexically_normal();
    const fs::path absSrc = fs::absolute(src, ec).lexically_normal();
    if (absSrc.parent_path() == absCovers) return (covers / src.filename()).generic_string();

    std::string stem;
    for (unsigned char c : gameName) stem += (std::isalnum(c) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    if (stem.empty()) stem = "game";
    std::string ext = src.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") ext = ".png";

    fs::create_directories(covers, ec);
    const fs::path dst = covers / (stem + ext);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return ec ? std::string{} : dst.generic_string();
}

bool ReadN64RomHeader(const std::string& path, std::string* internalName) {
    std::ifstream f(path, std::ios::binary);
    unsigned char h[0x40] = {};
    if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) return false;

    const uint32_t magic = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    int swap;                               // index mask that maps a big-endian offset to this file's layout
    if (magic == 0x80371240) swap = 0;      // .z64, big-endian
    else if (magic == 0x37804012) swap = 1; // .v64, byte-swapped pairs
    else if (magic == 0x40123780) swap = 3; // .n64, little-endian words
    else return false;

    if (internalName) {
        // 20-byte title at 0x20.
        std::string name;
        for (int i = 0x20; i < 0x34; ++i) {
            const unsigned char c = h[i ^ swap];
            name += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
        }
        const auto end = name.find_last_not_of(' ');
        *internalName = (end == std::string::npos) ? std::string{} : name.substr(0, end + 1);
    }
    return true;
}

RomInfo ReadRomInfo(const std::string& path) {
    RomInfo info;
    std::error_code ec;
    info.size = fs::is_regular_file(path, ec) ? static_cast<unsigned long long>(fs::file_size(path, ec)) : 0;
    std::ifstream f(path, std::ios::binary);
    unsigned char h[0x40] = {};
    if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) return info;

    const uint32_t magic = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    int swap;
    if (magic == 0x80371240) {
        swap = 0;
        info.format = "z64 (big-endian)";
    } else if (magic == 0x37804012) {
        swap = 1;
        info.format = "v64 (byte-swapped)";
    } else if (magic == 0x40123780) {
        swap = 3;
        info.format = "n64 (little-endian)";
    } else {
        return info;
    }
    auto at = [&](int offset) { return h[offset ^ swap]; };
    auto word = [&](int offset) {
        return (unsigned(at(offset)) << 24) | (unsigned(at(offset + 1)) << 16) | (unsigned(at(offset + 2)) << 8) |
               at(offset + 3);
    };

    std::string name;
    for (int i = 0x20; i < 0x34; ++i) name += (at(i) >= 0x20 && at(i) < 0x7F) ? static_cast<char>(at(i)) : ' ';
    const auto end = name.find_last_not_of(' ');
    info.title = end == std::string::npos ? std::string{} : name.substr(0, end + 1);
    info.gameId = {static_cast<char>(at(0x3C)), static_cast<char>(at(0x3D))};
    info.crc1 = word(0x10);
    info.crc2 = word(0x14);
    info.revision = at(0x3F);

    info.regionCode = static_cast<char>(at(0x3E));
    switch (info.regionCode) {
    case 'E': info.region = "USA"; break;
    case 'J': info.region = "Japan"; break;
    case 'A': info.region = "Asia"; break;
    case '7': info.region = "Beta"; break;
    case 'B': info.region = "Brazil"; break;
    case 'P':
    case 'X':
    case 'Y':
        info.region = "Europe";
        info.pal = true;
        break;
    case 'D':
        info.region = "Germany";
        info.pal = true;
        break;
    case 'F':
        info.region = "France";
        info.pal = true;
        break;
    case 'I':
        info.region = "Italy";
        info.pal = true;
        break;
    case 'S':
        info.region = "Spain";
        info.pal = true;
        break;
    case 'U':
        info.region = "Australia";
        info.pal = true;
        break;
    default: break;
    }
    info.valid = true;
    return info;
}

// Every game started from the launcher or the command line is kept in roms/.
std::string CopyIntoRomsFolder(const std::string& romPath) {
    std::error_code ec;
    const fs::path src(romPath);
    if (!fs::is_regular_file(src, ec)) return romPath;
    fs::create_directories("roms", ec);
    const fs::path dst = fs::path("roms") / src.filename();
    if (fs::equivalent(src, dst, ec)) return dst.string();

    std::error_code sizeEc;
    const bool upToDate = fs::exists(dst, sizeEc) && fs::file_size(dst, sizeEc) == fs::file_size(src, sizeEc);
    if (!upToDate) {
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return romPath;
    }
    return dst.string();
}

std::string ShotPathFor(const std::string& romPath) {
    std::string stem;
    for (unsigned char c : fs::path(romPath).stem().string())
        stem += (std::isalnum(c) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    if (stem.empty()) stem = "game";
    return (fs::path("shots") / (stem + ".png")).generic_string();
}

} // namespace detail

bool NovaLauncher::IsN64DDDisk(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ndd" || ext == ".d64";
}

namespace {

std::string Escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char n = s[++i];
        out += (n == 't') ? '\t' : (n == 'n') ? '\n' : n;
    }
    return out;
}

bool SamePath(const std::string& a, const std::string& b) {
    if (a == b) return true;
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

std::string Trim(const char* s) {
    std::string t(s);
    const auto b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return t.substr(b, t.find_last_not_of(" \t\r\n") - b + 1);
}

} // namespace

// --- Library ---
void NovaLauncher::LoadLibrary() {
    m_library.clear();

    std::ifstream f(detail::kLibraryPath);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        size_t start = 0;
        for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1)
            cols.push_back(line.substr(start, tab - start));
        cols.push_back(line.substr(start));

        RomMetadata rom;
        rom.path = Unescape(cols[0]);
        rom.name = cols.size() > 1 ? Unescape(cols[1]) : std::string{};
        rom.notes = cols.size() > 2 ? Unescape(cols[2]) : std::string{};
        rom.cover = cols.size() > 3 ? Unescape(cols[3]) : std::string{};
        rom.favorite = cols.size() > 4 && cols[4] == "1";
        rom.lastPlayed = cols.size() > 5 ? std::atoll(cols[5].c_str()) : 0;
        rom.plays = cols.size() > 6 ? std::max(0, std::atoi(cols[6].c_str())) : 0;
        if (rom.path.empty()) continue;
        if (rom.name.empty()) rom.name = fs::path(rom.path).stem().string();
        const bool dup = std::any_of(m_library.begin(), m_library.end(),
                                     [&](const RomMetadata& r) { return SamePath(r.path, rom.path); });
        if (!dup) m_library.push_back(std::move(rom));
    }

    // Pick up anything already in roms/ that the library doesn't list.
    bool added = false;
    std::error_code ec;
    for (auto it = fs::directory_iterator("roms", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string path = it->path().string();
        std::string title;
        if (!IsN64DDDisk(path) && !detail::ReadN64RomHeader(path, &title)) continue;
        const bool known = std::any_of(m_library.begin(), m_library.end(),
                                       [&](const RomMetadata& r) { return SamePath(r.path, path); });
        if (known) continue;
        RomMetadata rom;
        rom.path = path;
        rom.name = title.empty() ? it->path().stem().string() : title;
        m_library.push_back(std::move(rom));
        added = true;
    }
    if (added) SaveLibrary();
}

void NovaLauncher::SaveLibrary() const {
    std::error_code ec;
    fs::create_directories("settings", ec);
    std::ofstream f(detail::kLibraryPath, std::ios::trunc);
    f << "# path<TAB>name<TAB>notes<TAB>cover<TAB>favorite<TAB>last played<TAB>plays\n";
    for (const auto& r : m_library)
        f << Escape(r.path) << '\t' << Escape(r.name) << '\t' << Escape(r.notes) << '\t' << Escape(r.cover) << '\t'
          << (r.favorite ? '1' : '0') << '\t' << r.lastPlayed << '\t' << r.plays << '\n';
}

void NovaLauncher::AddToLibrary(const std::string& path, const std::string& name, const std::string& notes,
                                const std::string& cover) {
    const bool known =
        std::any_of(m_library.begin(), m_library.end(), [&](const RomMetadata& r) { return SamePath(r.path, path); });
    if (known) return;
    RomMetadata rom;
    rom.path = path;
    rom.name = name.empty() ? fs::path(path).stem().string() : name;
    rom.notes = notes;
    rom.cover = cover;
    m_library.push_back(std::move(rom));
    SaveLibrary();
}

int NovaLauncher::FindGame(const std::string& path) const {
    std::error_code ec;
    for (size_t i = 0; i < m_library.size(); ++i)
        if (SamePath(m_library[i].path, path)) return static_cast<int>(i);
    // A game started from outside roms/ is copied there, so look for the copy's twin: the same file name and size.
    const fs::path wanted = fs::path(path).filename();
    const auto wantedSize = fs::file_size(path, ec);
    if (ec) return -1;
    for (size_t i = 0; i < m_library.size(); ++i) {
        if (fs::path(m_library[i].path).filename() != wanted) continue;
        if (fs::file_size(m_library[i].path, ec) == wantedSize && !ec) return static_cast<int>(i);
    }
    return -1;
}

void NovaLauncher::RecordPlay(const std::string& path) {
    const int index = FindGame(path);
    if (index < 0) return;
    RomMetadata& rom = m_library[static_cast<size_t>(index)];
    rom.lastPlayed = static_cast<int64_t>(std::time(nullptr));
    ++rom.plays;
    m_selectedGame = index;
    SaveLibrary();
}

std::vector<int> NovaLauncher::RecentGames() const {
    std::vector<int> order;
    for (size_t i = 0; i < m_library.size(); ++i)
        if (m_library[i].lastPlayed > 0) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return m_library[static_cast<size_t>(a)].lastPlayed > m_library[static_cast<size_t>(b)].lastPlayed;
    });
    return order;
}

void NovaLauncher::RemoveGame(int index) {
    if (index < 0 || index >= static_cast<int>(m_library.size())) return;
    m_library.erase(m_library.begin() + index);
    if (m_selectedGame == index) m_selectedGame = -1;
    else if (m_selectedGame > index) --m_selectedGame;
    m_simLoaded = -1;
    if (m_simGame == index) m_simGame = -1;
    else if (m_simGame > index) --m_simGame;
    SaveLibrary();
}

void NovaLauncher::OpenAddDialog() {
    m_editPathBuffer[0] = m_editNameBuffer[0] = m_editNotesBuffer[0] = m_editCoverBuffer[0] = '\0';
    m_modalError.clear();
    m_showAddModal = true;
}

void NovaLauncher::OpenEditDialog(int index) {
    if (index < 0 || index >= static_cast<int>(m_library.size())) return;
    const auto& rom = m_library[index];
    m_editingRomIndex = index;
    std::snprintf(m_editPathBuffer, sizeof(m_editPathBuffer), "%s", rom.path.c_str());
    std::snprintf(m_editNameBuffer, sizeof(m_editNameBuffer), "%s", rom.name.c_str());
    std::snprintf(m_editNotesBuffer, sizeof(m_editNotesBuffer), "%s", rom.notes.c_str());
    std::snprintf(m_editCoverBuffer, sizeof(m_editCoverBuffer), "%s", rom.cover.c_str());
    m_modalError.clear();
    m_showEditModal = true;
}

void NovaLauncher::OpenRomWithPicker() {
    const std::string picked = detail::PickRomFileNative();
    if (picked.empty()) return;
    std::string title;
    detail::ReadN64RomHeader(picked, &title);
    AddToLibrary(picked, title);
    PlayRom(picked, title.empty() ? fs::path(picked).stem().string() : title);
}

// --- Cover pictures ---
namespace {

// Big pictures are shrunk on load: a card shows one at a few hundred pixels, and a
// full-size scan of a box would waste tens of megabytes of GPU memory per game.
constexpr int kMaxCoverSize = 640;
constexpr int kMaxShotWidth = 640;

std::vector<uint8_t> ShrinkToFit(const uint8_t* rgba, int w, int h, int limit, int* outW, int* outH) {
    const int longest = std::max(w, h);
    if (longest <= limit) {
        *outW = w;
        *outH = h;
        return std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(w) * h * 4);
    }
    const float scale = static_cast<float>(limit) / static_cast<float>(longest);
    const int nw = std::max(1, static_cast<int>(w * scale));
    const int nh = std::max(1, static_cast<int>(h * scale));
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    // Box filter: average the source pixels each destination pixel covers.
    for (int y = 0; y < nh; ++y) {
        const int y0 = y * h / nh, y1 = std::max(y0 + 1, (y + 1) * h / nh);
        for (int x = 0; x < nw; ++x) {
            const int x0 = x * w / nw, x1 = std::max(x0 + 1, (x + 1) * w / nw);
            uint32_t sum[4] = {};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += rgba[(static_cast<size_t>(sy) * w + sx) * 4 + c];
            const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c)
                out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    *outW = nw;
    *outH = nh;
    return out;
}

} // namespace

const NovaLauncher::CoverTexture* NovaLauncher::GetCoverTexture(const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = m_coverTextures.find(path);
    if (it == m_coverTextures.end()) {
        CoverTexture cover;
        std::ifstream f(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = bytes.empty() ? nullptr
                                        : stbi_load_from_memory(reinterpret_cast<const uint8_t*>(bytes.data()),
                                                                static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (!pixels || w <= 0 || h <= 0) {
            cover.failed = true;
        } else {
            int sw = 0, sh = 0;
            const std::vector<uint8_t> small = ShrinkToFit(pixels, w, h, kMaxCoverSize, &sw, &sh);
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, small.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            cover.texture = tex;
            cover.width = sw;
            cover.height = sh;
        }
        if (pixels) stbi_image_free(pixels);
        it = m_coverTextures.emplace(path, cover).first;
    }
    return it->second.failed ? nullptr : &it->second;
}

void NovaLauncher::ForgetCoverTexture(const std::string& path) {
    auto it = m_coverTextures.find(path);
    if (it == m_coverTextures.end()) return;
    if (it->second.texture) {
        const GLuint t = it->second.texture;
        glDeleteTextures(1, &t);
    }
    m_coverTextures.erase(it);
}

void NovaLauncher::ReleaseCoverTextures() {
    for (auto& [path, cover] : m_coverTextures)
        if (cover.texture) {
            const GLuint t = cover.texture;
            glDeleteTextures(1, &t);
        }
    m_coverTextures.clear();
}

// --- Screenshot of the last session ---
void NovaLauncher::SaveGameShot() {
    if (m_gameTex == 0 || m_emuRomPath.empty()) return; // this game never drew a frame

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(Nova64::Video::g_frameMutex);
        w = Nova64::Video::g_frameW.load(std::memory_order_relaxed);
        h = Nova64::Video::g_frameH.load(std::memory_order_relaxed);
        const size_t bytes = static_cast<size_t>(std::max(w, 0)) * static_cast<size_t>(std::max(h, 0)) * 4;
        if (bytes == 0 || Nova64::Video::g_framePixels.size() < bytes) return;
        rgba.assign(Nova64::Video::g_framePixels.begin(),
                    Nova64::Video::g_framePixels.begin() + static_cast<std::ptrdiff_t>(bytes));
    }

    // A black frame (a scene change, a game that is still loading) makes a poor picture: keep the old one.
    uint64_t brightness = 0, samples = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 64) {
        brightness += static_cast<uint64_t>(rgba[i]) + rgba[i + 1] + rgba[i + 2];
        samples += 3;
    }
    if (samples == 0 || brightness / samples < 10) return;

    int sw = 0, sh = 0;
    std::vector<uint8_t> small = ShrinkToFit(rgba.data(), w, h, kMaxShotWidth, &sw, &sh);
    for (size_t i = 3; i < small.size(); i += 4) small[i] = 255;

    std::error_code ec;
    fs::create_directories("shots", ec);
    const std::string path = detail::ShotPathFor(m_emuRomPath);
    if (Nova::WritePNG(path.c_str(), sw, sh, 4, small.data())) ForgetCoverTexture(path);
}

// --- Drawing a game ---
bool NovaLauncher::DrawCover(const std::string& coverPath, const std::string& title, ImVec2 size, bool selected,
                             float rounding) {
    using namespace kit;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool clicked = kit::NavButton("##cover", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (const CoverTexture* cover = GetCoverTexture(coverPath)) {
        DrawImageCover(draw, cover->texture, cover->width, cover->height, min, max, rounding);
    } else {
        // No picture: the title's initials on a colour that is always the same for this game.
        DrawGradientFill(draw, min, max, PlaceholderColor(title), rounding, 0.38f);
        const Fonts& fonts = GetFonts();
        ImFont* font = size.x >= 220.0f ? fonts.display : size.x >= 130.0f ? fonts.title : fonts.heading;
        const std::string initials = Initials(title);
        const ImVec2 textSize = Measure(font, initials.c_str());
        PutText(
            draw, font,
            ImVec2(std::round(min.x + (size.x - textSize.x) * 0.5f), std::round(min.y + (size.y - textSize.y) * 0.5f)),
            ImGui::GetColorU32(ImVec4(1, 1, 1, 0.92f)), initials.c_str());
    }
    draw->AddRect(min, max, ImGui::GetColorU32(Rgb(0xFFEADB, hovered ? 0.30f : 0.12f)), rounding);

    if (hovered) {
        // A play button over the middle of the picture
        draw->AddRectFilled(min, max, IM_COL32(10, 5, 4, 115), rounding);
        const ImVec2 mid(min.x + size.x * 0.5f, min.y + size.y * 0.5f);
        const float radius = std::clamp(std::min(size.x, size.y) * 0.18f, 18.0f, 30.0f);
        draw->AddCircleFilled(mid, radius, ImGui::GetColorU32(color::kAccent));
        draw->AddCircle(mid, radius, ImGui::GetColorU32(Rgb(0xFFEADB, 0.5f)), 0, 1.5f);
        DrawIcon(draw, Icon::Play, ImVec2(mid.x + 1.0f, mid.y), std::round(radius * 1.0f),
                 ImGui::GetColorU32(color::kOnAccent));
    }
    if (selected || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)) DrawSelectionRing(draw, min, max, rounding);
    return clicked;
}

// --- Library pages ---
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

void NovaLauncher::RenderLibraryPage(Tab tab) {
    using namespace kit;
    const Fonts& fonts = GetFonts();
    const char* heading = tab == Tab::Library ? "Library" : tab == Tab::Recent ? "Recently Played" : "Favorites";

    // The games on this page, filtered by the search box
    std::vector<int> all;
    if (tab == Tab::Recent) {
        all = RecentGames();
    } else {
        for (int i = 0; i < static_cast<int>(m_library.size()); ++i)
            if (tab == Tab::Library || m_library[static_cast<size_t>(i)].favorite) all.push_back(i);
    }
    const std::string needle = Lower(Trim(m_searchBuffer));
    std::vector<int> shown;
    for (int i : all)
        if (needle.empty() || Lower(m_library[static_cast<size_t>(i)].name).find(needle) != std::string::npos)
            shown.push_back(i);

    char count[64];
    if (needle.empty())
        std::snprintf(count, sizeof(count), "%d game%s", static_cast<int>(all.size()), all.size() == 1 ? "" : "s");
    else std::snprintf(count, sizeof(count), "%d of %d", static_cast<int>(shown.size()), static_cast<int>(all.size()));

    const float top = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    PageTitleText(heading, count);
    ImGui::EndGroup();
    const float addWidth = 150.0f, searchWidth = 260.0f;
    const float right = ImGui::GetContentRegionMax().x;
    ImGui::SetCursorPos(ImVec2(right - addWidth - 12.0f - searchWidth, top + 14.0f));
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::InputTextWithHint("##search", "Search games", m_searchBuffer, sizeof(m_searchBuffer));
    ImGui::SetCursorPos(ImVec2(right - addWidth, top + 10.0f));
    if (Button("Add game", ButtonStyle::Primary, Icon::Plus, ImVec2(addWidth, 44.0f))) OpenAddDialog();
    ImGui::SetCursorPosY(top + 76.0f);

    if (shown.empty()) {
        BeginCard("Empty", ImVec2(0, 0), true, ImVec2(28.0f, 24.0f));
        const char* message = !needle.empty()       ? "No game matches that search."
                              : tab == Tab::Library ? "No games yet. Add one, or drop a ROM file onto this window."
                              : tab == Tab::Recent  ? "Nothing played yet. Games you start show up here, newest first."
                                                    : "No favorites yet. Use the heart on a game to keep it here.";
        ImGui::TextUnformatted(message);
        EndCard();
        return;
    }

    int removeIndex = -1;
    const float gap = space::k4, pad = space::k3;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::clamp(static_cast<int>((avail + gap) / (212.0f + gap)), 1, 6);
    const float cellW = std::floor((avail - gap * (columns - 1)) / columns);
    const float coverW = cellW - pad * 2.0f;
    const float coverH = std::floor(coverW * 0.75f);
    const float buttonH = 36.0f;
    const float cellH = pad + coverH + space::k3 + 22.0f + 20.0f + space::k3 + buttonH + pad;

    for (size_t n = 0; n < shown.size(); ++n) {
        const int i = shown[n];
        RomMetadata& rom = m_library[static_cast<size_t>(i)];
        if (n % static_cast<size_t>(columns) != 0) ImGui::SameLine(0.0f, gap);
        else if (n > 0) ImGui::Dummy(ImVec2(0.0f, gap - ImGui::GetStyle().ItemSpacing.y));

        ImGui::PushID(i);
        BeginCard("GameCard", ImVec2(cellW, cellH), false, ImVec2(pad, pad), 18.0f);
        std::error_code ec;
        const bool exists = fs::exists(rom.path, ec);
        if (DrawCover(rom.cover, rom.name, ImVec2(coverW, coverH), false, 12.0f) && exists) PlayRom(rom.path, rom.name);
        if (ImGui::IsItemHovered() && !exists) ImGui::SetTooltip("The ROM file is missing.");

        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PushFont(fonts.label);
        ImGui::TextUnformatted(Ellipsize(fonts.label, rom.name, coverW).c_str());
        ImGui::PopFont();
        if (!exists) {
            ImGui::PushFont(fonts.small);
            ImGui::TextColored(color::kDanger, "File not found");
            ImGui::PopFont();
        } else {
            const GameInfo* info = FindGameInfo(rom.name);
            const std::string sub = info ? std::string(info->publisher) + "  \xC2\xB7  " + std::to_string(info->year)
                                         : fs::path(rom.path).filename().string();
            ImGui::PushFont(fonts.small);
            ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
            ImGui::TextUnformatted(Ellipsize(fonts.small, sub, coverW).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rom.path.c_str());
        }

        ImGui::SetCursorPosY(cellH - pad - buttonH);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::BeginDisabled(!exists);
        if (Button("Play", ButtonStyle::Primary, Icon::Play, ImVec2(coverW - (buttonH + spacing) * 2.0f, buttonH)))
            PlayRom(rom.path, rom.name);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (IconButton("##favorite", Icon::Heart, ButtonStyle::Ghost,
                       rom.favorite ? "Remove from favorites" : "Add to favorites", buttonH, rom.favorite)) {
            rom.favorite = !rom.favorite;
            SaveLibrary();
        }
        ImGui::SameLine();
        if (IconButton("##more", Icon::More, ButtonStyle::Ghost, "More", buttonH)) ImGui::OpenPopup("##menu");
        if (ImGui::BeginPopup("##menu")) {
            if (ImGui::MenuItem("Setup...")) OpenSimulator(i);
            if (ImGui::MenuItem("Edit details...")) OpenEditDialog(i);
            if (ImGui::MenuItem("Remove from library")) removeIndex = i;
            ImGui::EndPopup();
        }
        EndCard();
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (removeIndex >= 0) RemoveGame(removeIndex);
}

// --- Add and edit dialogs ---
void NovaLauncher::RenderGameDialogs() {
    using namespace kit;
    constexpr float kLabelWidth = 120.0f, kInputWidth = 380.0f;
    const float kRowHeight = ImGui::GetFrameHeight(); // buttons beside an input are as tall as the input

    // Shared validation for both dialogs. Returns false and sets m_modalError on failure.
    auto validate = [&](std::string& path, std::string& title) {
        path = Trim(m_editPathBuffer);
        std::error_code ec;
        if (path.empty()) m_modalError = "Choose a ROM file.";
        else if (!fs::is_regular_file(path, ec)) m_modalError = "That file doesn't exist.";
        else if (!IsN64DDDisk(path) && !detail::ReadN64RomHeader(path, &title))
            m_modalError = "That file isn't an N64 ROM.";
        else if (m_editCoverBuffer[0] != '\0' && !fs::is_regular_file(Trim(m_editCoverBuffer), ec))
            m_modalError = "The cover image doesn't exist.";
        else return true;
        return false;
    };
    auto fields = [&]() {
        FormLabel("ROM file", kLabelWidth);
        ImGui::SetNextItemWidth(kInputWidth);
        ImGui::InputText("##rom", m_editPathBuffer, sizeof(m_editPathBuffer));
        ImGui::SameLine();
        if (Button("Browse...##rom", ButtonStyle::Secondary, Icon::None, ImVec2(0.0f, kRowHeight))) {
            const std::string picked = detail::PickRomFileNative();
            if (!picked.empty()) {
                std::snprintf(m_editPathBuffer, sizeof(m_editPathBuffer), "%s", picked.c_str());
                std::string title;
                if (m_editNameBuffer[0] == '\0' && detail::ReadN64RomHeader(picked, &title))
                    std::snprintf(m_editNameBuffer, sizeof(m_editNameBuffer), "%s", title.c_str());
            }
        }
        FormLabel("Name", kLabelWidth);
        ImGui::SetNextItemWidth(kInputWidth);
        ImGui::InputText("##name", m_editNameBuffer, sizeof(m_editNameBuffer));

        FormLabel("Cover image", kLabelWidth);
        ImGui::SetNextItemWidth(kInputWidth);
        ImGui::InputText("##cover", m_editCoverBuffer, sizeof(m_editCoverBuffer));
        ImGui::SameLine();
        if (Button("Browse...##cover", ButtonStyle::Secondary, Icon::None, ImVec2(0.0f, kRowHeight))) {
            const std::string picked = detail::PickImageFileNative();
            if (!picked.empty()) std::snprintf(m_editCoverBuffer, sizeof(m_editCoverBuffer), "%s", picked.c_str());
        }
        ImGui::SameLine();
        if (Button("Clear##cover", ButtonStyle::Ghost, Icon::None, ImVec2(0.0f, kRowHeight)))
            m_editCoverBuffer[0] = '\0';

        ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kLabelWidth);
        std::error_code previewEc;
        const std::string cover = Trim(m_editCoverBuffer);
        if (!cover.empty() && fs::is_regular_file(cover, previewEc)) {
            if (const CoverTexture* c = GetCoverTexture(cover)) {
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const ImVec2 size(168.0f, 126.0f);
                ImGui::Dummy(size);
                DrawImageCover(ImGui::GetWindowDrawList(), c->texture, c->width, c->height, min,
                               ImVec2(min.x + size.x, min.y + size.y), 12.0f);
            }
        } else {
            // Not wrapped: wrapped text makes an auto-sizing window open at the wrong size and position.
            ImGui::PushFont(GetFonts().small);
            ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
            ImGui::TextUnformatted("Optional. A PNG or JPEG shown on the game's card.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        FormLabel("Notes", kLabelWidth);
        ImGui::InputTextMultiline("##notes", m_editNotesBuffer, sizeof(m_editNotesBuffer), ImVec2(kInputWidth, 90.0f));
        if (!m_modalError.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kLabelWidth);
            ImGui::TextColored(color::kDanger, "%s", m_modalError.c_str());
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    };

    auto beginDialog = [&](const char* title, bool* open) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 22.0f));
        const bool visible = ImGui::BeginPopupModal(title, open, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleVar(2);
        return visible;
    };

    if (m_showAddModal) ImGui::OpenPopup("Add Game");
    if (beginDialog("Add Game", &m_showAddModal)) {
        fields();
        if (Button("Add game", ButtonStyle::Primary, Icon::Plus, ImVec2(150.0f, 0.0f))) {
            std::string path, title;
            if (validate(path, title)) {
                std::string name = Trim(m_editNameBuffer);
                const std::string finalName = name.empty() ? title : name;
                std::string cover;
                if (const std::string picked = Trim(m_editCoverBuffer); !picked.empty()) {
                    cover = detail::ImportCoverImage(picked, finalName);
                    ForgetCoverTexture(cover); // the file may have replaced an older picture
                }
                AddToLibrary(path, finalName, m_editNotesBuffer, cover);
                m_showAddModal = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (Button("Cancel", ButtonStyle::Secondary, Icon::None, ImVec2(120.0f, 0.0f))) {
            m_showAddModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_showEditModal) ImGui::OpenPopup("Edit Game");
    if (beginDialog("Edit Game", &m_showEditModal)) {
        fields();
        if (Button("Save", ButtonStyle::Primary, Icon::Check, ImVec2(150.0f, 0.0f))) {
            std::string path, title;
            if (m_editingRomIndex >= 0 && m_editingRomIndex < static_cast<int>(m_library.size()) &&
                validate(path, title)) {
                auto& rom = m_library[static_cast<size_t>(m_editingRomIndex)];
                const std::string name = Trim(m_editNameBuffer);
                rom.path = path;
                rom.name = !name.empty() ? name : !title.empty() ? title : fs::path(path).stem().string();
                rom.notes = m_editNotesBuffer;
                if (const std::string picked = Trim(m_editCoverBuffer); picked.empty()) {
                    rom.cover.clear();
                } else if (picked != rom.cover) {
                    const std::string cover = detail::ImportCoverImage(picked, rom.name);
                    ForgetCoverTexture(cover);
                    rom.cover = cover;
                }
                SaveLibrary();
                m_simLoaded = -1;
                m_showEditModal = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (Button("Cancel", ButtonStyle::Secondary, Icon::None, ImVec2(120.0f, 0.0f))) {
            m_showEditModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace NovaUI
