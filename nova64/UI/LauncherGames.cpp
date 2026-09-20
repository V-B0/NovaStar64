/**
 * @file LauncherGames.cpp
 * @brief Game library, the Games tab, and starting games.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"

#include <glad/gl.h>
#include <imgui.h>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace NovaUI {

// ============================================================
// Shared helpers
// ============================================================
namespace detail {

std::string PickRomFileNative() {
#if defined(_WIN32)
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"N64 games (*.z64;*.n64;*.v64;*.ndd;*.d64)\0*.z64;*.n64;*.v64;*.ndd;*.d64\0All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Select an N64 ROM";
    // NOCHANGEDIR: settings/, roms/ and saves/ are found relative to the working
    // directory, which the dialog would otherwise change.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    try {
        return fs::path(file).string();
    } catch (const std::exception&) {
        return {};   // a name the narrow code page can't represent
    }
#else
#if defined(__APPLE__)
    // No `of type {...}` filter: macOS matches it against UTIs, and z64/n64/v64
    // have none registered, so the ROMs would be greyed out.
    const char* cmd = "osascript -e 'POSIX path of (choose file with prompt \"Select an N64 ROM\")' 2>/dev/null";
#else
    // Whichever of the GNOME or KDE file pickers is installed.
    const char* cmd = "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --title='Select an N64 ROM';"
                      " elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename . --title 'Select an N64 ROM'; fi 2>/dev/null";
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
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Select a cover image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    try {
        return fs::path(file).string();
    } catch (const std::exception&) {
        return {};
    }
#else
#if defined(__APPLE__)
    const char* cmd = "osascript -e 'POSIX path of (choose file of type {\"public.png\", \"public.jpeg\"} with prompt \"Select a cover image\")' 2>/dev/null";
#else
    const char* cmd = "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --title='Select a cover image' --file-filter='Pictures | *.png *.jpg *.jpeg';"
                      " elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename . 'Pictures (*.png *.jpg *.jpeg)' --title 'Select a cover image'; fi 2>/dev/null";
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
    const fs::path absSrc    = fs::absolute(src, ec).lexically_normal();
    if (absSrc.parent_path() == absCovers) return (covers / src.filename()).generic_string();

    std::string stem;
    for (unsigned char c : gameName)
        stem += (std::isalnum(c) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
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
    int swap;   // index mask that maps a big-endian offset to this file's layout
    if      (magic == 0x80371240) swap = 0;   // .z64, big-endian
    else if (magic == 0x37804012) swap = 1;   // .v64, byte-swapped pairs
    else if (magic == 0x40123780) swap = 3;   // .n64, little-endian words
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

} // namespace detail

namespace {

std::string Escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) { out += s[i]; continue; }
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

// ============================================================
// Library
// ============================================================
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
        rom.path  = Unescape(cols[0]);
        rom.name  = cols.size() > 1 ? Unescape(cols[1]) : std::string{};
        rom.notes = cols.size() > 2 ? Unescape(cols[2]) : std::string{};
        rom.cover = cols.size() > 3 ? Unescape(cols[3]) : std::string{};
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
        m_library.push_back({path, title.empty() ? it->path().stem().string() : title, {}});
        added = true;
    }
    if (added) SaveLibrary();
}

void NovaLauncher::SaveLibrary() const {
    std::error_code ec;
    fs::create_directories("settings", ec);
    std::ofstream f(detail::kLibraryPath, std::ios::trunc);
    f << "# path<TAB>name<TAB>notes<TAB>cover\n";
    for (const auto& r : m_library)
        f << Escape(r.path) << '\t' << Escape(r.name) << '\t' << Escape(r.notes) << '\t' << Escape(r.cover) << '\n';
}

void NovaLauncher::AddToLibrary(const std::string& path, const std::string& name, const std::string& notes,
                                const std::string& cover) {
    const bool known = std::any_of(m_library.begin(), m_library.end(),
                                   [&](const RomMetadata& r) { return SamePath(r.path, path); });
    if (known) return;
    m_library.push_back({path, name.empty() ? fs::path(path).stem().string() : name, notes, cover});
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
    std::snprintf(m_editPathBuffer,  sizeof(m_editPathBuffer),  "%s", rom.path.c_str());
    std::snprintf(m_editNameBuffer,  sizeof(m_editNameBuffer),  "%s", rom.name.c_str());
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

// ============================================================
// Cover pictures
// ============================================================
namespace {

// Big pictures are shrunk on load: a card shows one at a few hundred pixels, and a
// full-size scan of a box would waste tens of megabytes of GPU memory per game.
constexpr int kMaxCoverSize = 512;

std::vector<uint8_t> ShrinkToFit(const uint8_t* rgba, int w, int h, int* outW, int* outH) {
    const int longest = std::max(w, h);
    if (longest <= kMaxCoverSize) {
        *outW = w; *outH = h;
        return std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(w) * h * 4);
    }
    const float scale = static_cast<float>(kMaxCoverSize) / static_cast<float>(longest);
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
            for (int c = 0; c < 4; ++c) out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    *outW = nw; *outH = nh;
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
            : stbi_load_from_memory(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int>(bytes.size()),
                                    &w, &h, &channels, 4);
        if (!pixels || w <= 0 || h <= 0) {
            cover.failed = true;
        } else {
            int sw = 0, sh = 0;
            const std::vector<uint8_t> small = ShrinkToFit(pixels, w, h, &sw, &sh);
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, small.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            cover.texture = tex; cover.width = sw; cover.height = sh;
        }
        if (pixels) stbi_image_free(pixels);
        it = m_coverTextures.emplace(path, cover).first;
    }
    return it->second.failed ? nullptr : &it->second;
}

void NovaLauncher::ForgetCoverTexture(const std::string& path) {
    auto it = m_coverTextures.find(path);
    if (it == m_coverTextures.end()) return;
    if (it->second.texture) { const GLuint t = it->second.texture; glDeleteTextures(1, &t); }
    m_coverTextures.erase(it);
}

void NovaLauncher::ReleaseCoverTextures() {
    for (auto& [path, cover] : m_coverTextures)
        if (cover.texture) { const GLuint t = cover.texture; glDeleteTextures(1, &t); }
    m_coverTextures.clear();
}

bool NovaLauncher::DrawCover(const std::string& coverPath, float width) {
    const float height = width * 0.72f;   // N64 boxes are wider than tall
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##cover", ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(40, 36, 34, 255), 3.0f);

    if (const CoverTexture* cover = GetCoverTexture(coverPath)) {
        // Fit inside the box without stretching the picture.
        const float scale = std::min(width / cover->width, height / cover->height);
        const float w = cover->width * scale, h = cover->height * scale;
        const ImVec2 p0(origin.x + (width - w) * 0.5f, origin.y + (height - h) * 0.5f);
        draw->AddImage(static_cast<ImTextureID>(cover->texture), p0, ImVec2(p0.x + w, p0.y + h));
    } else if (m_iconTex) {
        const float size = height * 0.45f;
        const ImVec2 p0(origin.x + (width - size) * 0.5f, origin.y + (height - size) * 0.5f);
        draw->AddImage(static_cast<ImTextureID>(m_iconTex), p0, ImVec2(p0.x + size, p0.y + size),
                       ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 70));
    }
    if (ImGui::IsItemHovered())
        draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(217, 110, 66, 255), 3.0f, 0, 2.0f);
    return clicked;
}

// ============================================================
// Games tab
// ============================================================
void NovaLauncher::RenderGamesTab() {
    const ImVec4 kWarn(0.75f, 0.25f, 0.15f, 1.0f);

    ImGui::Text("Games");
    const char* addLabel = "Add Game...";
    const float addWidth = ImGui::CalcTextSize(addLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - addWidth);
    if (ImGui::Button(addLabel)) OpenAddDialog();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_library.empty())
        ImGui::TextDisabled("No games yet. Add one, or drag a ROM file onto this window.");

    int removeIndex = -1;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 240.0f));
    if (!m_library.empty() && ImGui::BeginTable("LibraryGrid", columns)) {
        for (int i = 0; i < static_cast<int>(m_library.size()); ++i) {
            const auto& rom = m_library[i];
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::BeginChild("GameCard", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
            {
                std::error_code fileEc;
                const bool playable = fs::exists(rom.path, fileEc);
                if (DrawCover(rom.cover, ImGui::GetContentRegionAvail().x) && playable) PlayRom(rom.path, rom.name);
                if (ImGui::IsItemHovered() && playable) ImGui::SetTooltip("Click to play");
            }
            ImGui::TextWrapped("%s", rom.name.c_str());
            ImGui::TextDisabled("%s", fs::path(rom.path).filename().string().c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rom.path.c_str());

            std::error_code ec;
            const bool exists = fs::exists(rom.path, ec);
            if (!exists) ImGui::TextColored(kWarn, "File not found");

            ImGui::BeginDisabled(!exists);
            if (ImGui::Button("Play", ImVec2(-1, 0))) PlayRom(rom.path, rom.name);
            ImGui::EndDisabled();

            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Edit", ImVec2(half, 0))) OpenEditDialog(i);
            ImGui::SameLine();
            if (ImGui::Button("Remove", ImVec2(-1, 0))) removeIndex = i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes it from the library. The file is not deleted.");

            ImGui::EndChild();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex >= 0) {
        m_library.erase(m_library.begin() + removeIndex);
        SaveLibrary();
    }

    // Shared validation for both dialogs. Returns false and sets m_modalError on failure.
    auto validate = [&](std::string& path, std::string& title) {
        path = Trim(m_editPathBuffer);
        std::error_code ec;
        if (path.empty())                         m_modalError = "Choose a ROM file.";
        else if (!fs::is_regular_file(path, ec))  m_modalError = "That file doesn't exist.";
        else if (!IsN64DDDisk(path) && !detail::ReadN64RomHeader(path, &title))
                                                  m_modalError = "That file isn't an N64 ROM.";
        else if (m_editCoverBuffer[0] != '\0' && !fs::is_regular_file(Trim(m_editCoverBuffer), ec))
                                                  m_modalError = "The cover image doesn't exist.";
        else return true;
        return false;
    };
    auto fields = [&]() {
        ImGui::SetNextItemWidth(360);
        ImGui::InputText("ROM file", m_editPathBuffer, sizeof(m_editPathBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string picked = detail::PickRomFileNative();
            if (!picked.empty()) {
                std::snprintf(m_editPathBuffer, sizeof(m_editPathBuffer), "%s", picked.c_str());
                std::string title;
                if (m_editNameBuffer[0] == '\0' && detail::ReadN64RomHeader(picked, &title))
                    std::snprintf(m_editNameBuffer, sizeof(m_editNameBuffer), "%s", title.c_str());
            }
        }
        ImGui::SetNextItemWidth(360);
        ImGui::InputText("Name", m_editNameBuffer, sizeof(m_editNameBuffer));
        ImGui::SetNextItemWidth(360);
        ImGui::InputText("Cover image", m_editCoverBuffer, sizeof(m_editCoverBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##cover")) {
            const std::string picked = detail::PickImageFileNative();
            if (!picked.empty()) std::snprintf(m_editCoverBuffer, sizeof(m_editCoverBuffer), "%s", picked.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear##cover")) m_editCoverBuffer[0] = '\0';
        {
            std::error_code previewEc;
            const std::string cover = Trim(m_editCoverBuffer);
            if (!cover.empty() && fs::is_regular_file(cover, previewEc)) {
                if (const CoverTexture* c = GetCoverTexture(cover)) {
                    const float scale = std::min(160.0f / c->width, 112.0f / c->height);
                    ImGui::Image(static_cast<ImTextureID>(c->texture), ImVec2(c->width * scale, c->height * scale));
                }
            } else {
                ImGui::TextDisabled("Optional. A PNG or JPEG shown on the game's card.");
            }
        }
        ImGui::InputTextMultiline("Notes", m_editNotesBuffer, sizeof(m_editNotesBuffer), ImVec2(360, 80));
        if (!m_modalError.empty()) ImGui::TextColored(kWarn, "%s", m_modalError.c_str());
    };

    if (m_showAddModal) ImGui::OpenPopup("Add Game");
    if (ImGui::BeginPopupModal("Add Game", &m_showAddModal, ImGuiWindowFlags_AlwaysAutoResize)) {
        fields();
        if (ImGui::Button("Add", ImVec2(120, 0))) {
            std::string path, title;
            if (validate(path, title)) {
                std::string name = Trim(m_editNameBuffer);
                const std::string finalName = name.empty() ? title : name;
                std::string cover;
                if (const std::string picked = Trim(m_editCoverBuffer); !picked.empty()) {
                    cover = detail::ImportCoverImage(picked, finalName);
                    ForgetCoverTexture(cover);   // the file may have replaced an older picture
                }
                AddToLibrary(path, finalName, m_editNotesBuffer, cover);
                m_showAddModal = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showAddModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_showEditModal) ImGui::OpenPopup("Edit Game");
    if (ImGui::BeginPopupModal("Edit Game", &m_showEditModal, ImGuiWindowFlags_AlwaysAutoResize)) {
        fields();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            std::string path, title;
            if (m_editingRomIndex >= 0 && m_editingRomIndex < static_cast<int>(m_library.size()) &&
                validate(path, title)) {
                auto& rom = m_library[m_editingRomIndex];
                const std::string name = Trim(m_editNameBuffer);
                rom.path  = path;
                rom.name  = !name.empty() ? name : !title.empty() ? title : fs::path(path).stem().string();
                rom.notes = m_editNotesBuffer;
                if (const std::string picked = Trim(m_editCoverBuffer); picked.empty()) {
                    rom.cover.clear();
                } else if (picked != rom.cover) {
                    const std::string cover = detail::ImportCoverImage(picked, rom.name);
                    ForgetCoverTexture(cover);
                    rom.cover = cover;
                }
                SaveLibrary();
                m_showEditModal = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showEditModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace NovaUI
