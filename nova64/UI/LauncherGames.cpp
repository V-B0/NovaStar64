/**
 * @file LauncherGames.cpp
 * @brief Game library, the Games tab, and starting games.
 */
#include "NovaLauncher.hpp"
#include "LauncherInternal.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

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
    f << "# path<TAB>name<TAB>notes\n";
    for (const auto& r : m_library)
        f << Escape(r.path) << '\t' << Escape(r.name) << '\t' << Escape(r.notes) << '\n';
}

void NovaLauncher::AddToLibrary(const std::string& path, const std::string& name, const std::string& notes) {
    const bool known = std::any_of(m_library.begin(), m_library.end(),
                                   [&](const RomMetadata& r) { return SamePath(r.path, path); });
    if (known) return;
    m_library.push_back({path, name.empty() ? fs::path(path).stem().string() : name, notes});
    SaveLibrary();
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
// Games tab
// ============================================================
void NovaLauncher::RenderGamesTab() {
    const ImVec4 kWarn(0.75f, 0.25f, 0.15f, 1.0f);

    ImGui::Text("Games");
    const char* addLabel = "Add Game...";
    const float addWidth = ImGui::CalcTextSize(addLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - addWidth);
    if (ImGui::Button(addLabel)) {
        m_editPathBuffer[0] = m_editNameBuffer[0] = m_editNotesBuffer[0] = '\0';
        m_modalError.clear();
        m_showAddModal = true;
    }
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
            if (ImGui::Button("Edit", ImVec2(half, 0))) {
                m_editingRomIndex = i;
                std::snprintf(m_editPathBuffer,  sizeof(m_editPathBuffer),  "%s", rom.path.c_str());
                std::snprintf(m_editNameBuffer,  sizeof(m_editNameBuffer),  "%s", rom.name.c_str());
                std::snprintf(m_editNotesBuffer, sizeof(m_editNotesBuffer), "%s", rom.notes.c_str());
                m_modalError.clear();
                m_showEditModal = true;
            }
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
                AddToLibrary(path, name.empty() ? title : name, m_editNotesBuffer);
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
