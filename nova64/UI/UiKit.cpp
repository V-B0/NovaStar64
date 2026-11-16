#include "UiKit.hpp"

#include <glad/gl.h>
#include <imgui_internal.h> // FindRenderedTextEnd
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace NovaUI::kit {

namespace {

// The art: PNG sheets exported from assets/ui/*.aseprite, compiled in by CMake as lists of byte values.
const unsigned char kIconsPng[] = {
#include "UiIcons.inc"
};
const unsigned char kFontPng[] = {
#include "UiFont.inc"
};
const unsigned char kFramesPng[] = {
#include "UiFrames.inc"
};
// The names of the icons in the icon sheet, one per line, in sheet order.
const char kIconNames[] = {
#include "UiIconNames.inc"
    0};

// Sheet layouts (see scripts/ui-art/seed.lua).
constexpr int kIconCell = 16;
constexpr int kIconColumns = 8;
constexpr int kGlyphCell = 8;
constexpr int kGlyphColumns = 16;
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kFrameCell = 10;
constexpr int kFrameCorner = 4;
constexpr int kDitherX = 0;
constexpr int kDitherY = 10;
constexpr int kDitherCell = 4;

struct Sheet {
    GLuint texture = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; ///< RGBA, kept so the font can be rebuilt at other sizes
};

Sheet g_icons, g_font, g_frames;
std::vector<std::string> g_iconNames;
Fonts g_fonts;
int g_px = 2;
bool g_fontFilterDone = false;

Sheet LoadSheet(const unsigned char* png, size_t size) {
    Sheet sheet;
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load_from_memory(png, static_cast<int>(size), &w, &h, &channels, 4);
    if (!data) return sheet;
    sheet.width = w;
    sheet.height = h;
    sheet.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);

    glGenTextures(1, &sheet.texture);
    glBindTexture(GL_TEXTURE_2D, sheet.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sheet.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return sheet;
}

// Colour with the current style alpha applied, so disabled widgets fade with everything else.
ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }

ImU32 Tint() { return ImGui::GetColorU32(ImVec4(1, 1, 1, 1)); }

bool GlyphPresent(int code) {
    if (g_font.pixels.empty() || code < 32 || code > 127) return false;
    const int cell = code - 32;
    const int cx = (cell % kGlyphColumns) * kGlyphCell, cy = (cell / kGlyphColumns) * kGlyphCell;
    for (int y = 0; y < kGlyphHeight; ++y)
        for (int x = 0; x < kGlyphWidth; ++x)
            if (g_font.pixels[(static_cast<size_t>(cy + y) * g_font.width + cx + x) * 4 + 3] != 0) return true;
    return false;
}

bool GlyphPixel(int code, int x, int y) {
    const int cell = code - 32;
    const int cx = (cell % kGlyphColumns) * kGlyphCell, cy = (cell / kGlyphColumns) * kGlyphCell;
    return g_font.pixels[(static_cast<size_t>(cy + y) * g_font.width + cx + x) * 4 + 3] != 0;
}

struct PendingGlyph {
    int rect; ///< ImGui custom rectangle
    int code; ///< ASCII glyph whose bitmap goes there; 0 for blank
    int scale;
};

} // namespace

int Px() { return g_px; }

// --- Theme ---

void ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    const ImVec4 clear(0, 0, 0, 0);

    c[ImGuiCol_Text] = color::kText;
    c[ImGuiCol_TextDisabled] = color::kTextDisabled;
    c[ImGuiCol_WindowBg] = color::kBackground;
    c[ImGuiCol_ChildBg] = color::kPanel;
    c[ImGuiCol_PopupBg] = color::kPanel;
    c[ImGuiCol_Border] = color::kOrange;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = color::kInk;
    c[ImGuiCol_FrameBgHovered] = color::kPanel;
    c[ImGuiCol_FrameBgActive] = color::kViolet;
    c[ImGuiCol_TitleBg] = color::kPanel;
    c[ImGuiCol_TitleBgActive] = color::kPanel;
    c[ImGuiCol_TitleBgCollapsed] = color::kPanel;
    c[ImGuiCol_MenuBarBg] = color::kBackground;
    c[ImGuiCol_ScrollbarBg] = color::kInk;
    c[ImGuiCol_ScrollbarGrab] = color::kViolet;
    c[ImGuiCol_ScrollbarGrabHovered] = color::kOrange;
    c[ImGuiCol_ScrollbarGrabActive] = color::kCream;
    c[ImGuiCol_CheckMark] = color::kCream;
    c[ImGuiCol_SliderGrab] = color::kOrange;
    c[ImGuiCol_SliderGrabActive] = color::kCream;
    c[ImGuiCol_Button] = color::kPanel;
    c[ImGuiCol_ButtonHovered] = color::kViolet;
    c[ImGuiCol_ButtonActive] = color::kOrange;
    c[ImGuiCol_Header] = color::kViolet;
    c[ImGuiCol_HeaderHovered] = Rgb(0x7567BC);
    c[ImGuiCol_HeaderActive] = color::kOrange;
    c[ImGuiCol_Separator] = color::kViolet;
    c[ImGuiCol_SeparatorHovered] = color::kOrange;
    c[ImGuiCol_SeparatorActive] = color::kCream;
    c[ImGuiCol_ResizeGrip] = color::kViolet;
    c[ImGuiCol_ResizeGripHovered] = color::kOrange;
    c[ImGuiCol_ResizeGripActive] = color::kCream;
    c[ImGuiCol_Tab] = color::kPanel;
    c[ImGuiCol_TabHovered] = Rgb(0x7567BC);
    c[ImGuiCol_TabActive] = color::kViolet;
    c[ImGuiCol_TabUnfocused] = color::kPanel;
    c[ImGuiCol_TabUnfocusedActive] = color::kViolet;
    c[ImGuiCol_TabSelectedOverline] = color::kOrange;
    c[ImGuiCol_TabDimmedSelectedOverline] = color::kOrange;
    c[ImGuiCol_TableHeaderBg] = color::kViolet;
    c[ImGuiCol_TableBorderStrong] = color::kViolet;
    c[ImGuiCol_TableBorderLight] = Rgb(0x2A2560);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = Rgb(0xF6F0DC, 0.04f);
    c[ImGuiCol_TextSelectedBg] = Rgb(0xFF7A1A, 0.40f);
    c[ImGuiCol_NavCursor] = color::kCream;
    c[ImGuiCol_DragDropTarget] = color::kCream;
    c[ImGuiCol_ModalWindowDimBg] = Rgb(0x05050F, 0.70f);

    // Square, outlined, and sized in whole art pixels. Antialiasing is off so edges stay hard.
    const float px = Px(1.0f);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = px;
    style.FrameBorderSize = px;
    style.PopupBorderSize = px;
    style.TabBorderSize = 0.0f;
    style.WindowPadding = ImVec2(Px(8.0f), Px(7.0f));
    style.FramePadding = ImVec2(Px(4.0f), Px(3.0f));
    style.ItemSpacing = ImVec2(Px(5.0f), Px(4.0f));
    style.ItemInnerSpacing = ImVec2(Px(3.0f), Px(3.0f));
    style.CellPadding = ImVec2(Px(4.0f), Px(3.0f));
    style.ScrollbarSize = Px(5.0f);
    style.GrabMinSize = Px(6.0f);
    style.IndentSpacing = Px(8.0f);
    style.SeparatorTextBorderSize = px;
    style.DisabledAlpha = 0.45f;
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;
}

// --- Fonts and art ---

void LoadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    const float dpi = pixelScale >= 1.0f ? pixelScale : 1.0f;

    g_icons = LoadSheet(kIconsPng, sizeof(kIconsPng));
    g_font = LoadSheet(kFontPng, sizeof(kFontPng));
    g_frames = LoadSheet(kFramesPng, sizeof(kFramesPng));
    g_iconNames.clear();
    for (size_t start = 0, n = std::strlen(kIconNames); start < n;) {
        const size_t end = std::min(n, std::string(kIconNames).find('\n', start));
        if (end > start) g_iconNames.emplace_back(kIconNames + start, end - start);
        start = end + 1;
    }

    // One font per size. Each glyph is a rectangle in Dear ImGui's atlas; the bitmaps are written into
    // those rectangles after the atlas is built, scaled up by whole numbers so nothing is smoothed.
    std::vector<PendingGlyph> pending;
    auto make = [&](int artScale) -> ImFont* {
        const int pd = std::max(1, static_cast<int>(std::lround(static_cast<float>(artScale) * dpi)));
        ImFontConfig config;
        config.SizePixels = 9.0f * static_cast<float>(pd); // 7 rows of glyph and 2 of leading
        config.OversampleH = 1;
        config.OversampleV = 1;
        ImFont* font = io.Fonts->AddFontDefault(&config);
        const float advance = 6.0f * static_cast<float>(pd);
        const ImVec2 offset(0.0f, static_cast<float>(pd)); // centre 7 rows in the 9-row line
        for (int code = 33; code <= 126; ++code) {
            int source = code;
            if (code >= 'a' && code <= 'z') source = code - 32; // the font has capitals only
            if (!GlyphPresent(source)) continue;
            const int rect = io.Fonts->AddCustomRectFontGlyph(font, static_cast<ImWchar>(code), kGlyphWidth * pd,
                                                              kGlyphHeight * pd, advance, offset);
            pending.push_back({rect, source, pd});
        }
        const int space = io.Fonts->AddCustomRectFontGlyph(font, ' ', 1, 1, 4.0f * static_cast<float>(pd), offset);
        pending.push_back({space, 0, pd});
        return font;
    };
    g_fonts.body = make(2);
    g_fonts.small = g_fonts.body;
    g_fonts.heading = make(3);
    g_fonts.display = make(5);

    unsigned char* atlas = nullptr;
    int atlasWidth = 0, atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &atlasWidth, &atlasHeight);
    for (const PendingGlyph& glyph : pending) {
        const ImFontAtlasCustomRect* rect = io.Fonts->GetCustomRectByIndex(glyph.rect);
        if (!rect || !atlas) continue;
        for (int y = 0; y < rect->Height; ++y) {
            for (int x = 0; x < rect->Width; ++x) {
                const bool on = glyph.code != 0 && GlyphPixel(glyph.code, x / glyph.scale, y / glyph.scale);
                auto* out =
                    reinterpret_cast<uint32_t*>(atlas) + static_cast<size_t>(rect->Y + y) * atlasWidth + rect->X + x;
                *out = on ? 0xFFFFFFFFu : 0x00FFFFFFu;
            }
        }
    }

    io.FontDefault = g_fonts.body;
    io.FontGlobalScale = 1.0f / dpi;
}

void OnNewFrame() {
    if (g_fontFilterDone) return;
    const ImTextureID id = ImGui::GetIO().Fonts->TexID;
    if (!id) return;
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(id));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_fontFilterDone = true;
}

const Fonts& GetFonts() { return g_fonts; }

// --- Icons ---

namespace {

const char* IconSpriteName(Icon icon) {
    switch (icon) {
    case Icon::Home:         return "home";
    case Icon::Games:        return "games";
    case Icon::Settings:     return "settings";
    case Icon::Scripts:      return "console";
    case Icon::Memory:       return "chip";
    case Icon::Trophy:       return "trophy";
    case Icon::Simulator:    return "controller";
    case Icon::Play:         return "play";
    case Icon::Stop:         return "stop";
    case Icon::Rewind:       return "rewind";
    case Icon::Plus:         return "plus";
    case Icon::Edit:         return "edit";
    case Icon::Trash:        return "trash";
    case Icon::Folder:       return "folder";
    case Icon::Image:        return "camera";
    case Icon::Search:       return "search";
    case Icon::Check:        return "check";
    case Icon::Info:         return "info";
    case Icon::Warning:      return "warning";
    case Icon::ChevronRight: return "chevron_right";
    case Icon::ChevronDown:  return "chevron_down";
    case Icon::Close:        return "cross";
    case Icon::Minimize:     return "minimize";
    case Icon::Maximize:     return "maximize";
    case Icon::Restore:      return "restore";
    case Icon::None:         break;
    }
    return nullptr;
}

int IconCell(Icon icon) {
    static int cache[64];
    static bool filled[64] = {};
    const int key = static_cast<int>(icon);
    if (key < 0 || key >= 64) return -1;
    if (!filled[key]) {
        cache[key] = -1;
        if (const char* name = IconSpriteName(icon)) {
            for (size_t i = 0; i < g_iconNames.size(); ++i)
                if (g_iconNames[i] == name) cache[key] = static_cast<int>(i);
        }
        filled[key] = true;
    }
    return cache[key];
}

float Snap(float v) { return std::round(v); }

} // namespace

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 tint) {
    if (icon == Icon::None || !g_icons.texture) return;
    const int cell = IconCell(icon);
    if (cell < 0) return;
    const float cx = static_cast<float>((cell % kIconColumns) * kIconCell);
    const float cy = static_cast<float>((cell / kIconColumns) * kIconCell);
    const float w = static_cast<float>(g_icons.width), h = static_cast<float>(g_icons.height);
    const ImVec2 p0(Snap(center.x - size * 0.5f), Snap(center.y - size * 0.5f));
    draw->AddImage(static_cast<ImTextureID>(g_icons.texture), p0, ImVec2(p0.x + size, p0.y + size),
                   ImVec2(cx / w, cy / h), ImVec2((cx + kIconCell) / w, (cy + kIconCell) / h), tint);
}

// --- Frames ---

void DrawFrame(ImDrawList* draw, ImVec2 min, ImVec2 max, Frame frame, ImU32 fill) {
    const float p = Px(1.0f);
    const float x0 = Snap(min.x), y0 = Snap(min.y), x1 = Snap(max.x), y1 = Snap(max.y);
    if (x1 - x0 < p * 2 * kFrameCorner || y1 - y0 < p * 2 * kFrameCorner) {
        if ((fill >> IM_COL32_A_SHIFT) != 0) draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill);
        return;
    }
    if ((fill >> IM_COL32_A_SHIFT) != 0) {
        draw->AddRectFilled(ImVec2(x0 + 2 * p, y0 + p), ImVec2(x1 - 2 * p, y1 - p), fill);
        draw->AddRectFilled(ImVec2(x0 + p, y0 + 2 * p), ImVec2(x1 - p, y1 - 2 * p), fill);
    }
    if (!g_frames.texture) return;

    const float ox = static_cast<float>(static_cast<int>(frame) * kFrameCell);
    const float sw = static_cast<float>(g_frames.width), sh = static_cast<float>(g_frames.height);
    const ImTextureID tex = static_cast<ImTextureID>(g_frames.texture);
    const ImU32 tint = Tint();
    const float c = kFrameCorner * p;
    auto piece = [&](float dx0, float dy0, float dx1, float dy1, float ux0, float uy0, float ux1, float uy1) {
        draw->AddImage(tex, ImVec2(dx0, dy0), ImVec2(dx1, dy1), ImVec2((ox + ux0) / sw, uy0 / sh),
                       ImVec2((ox + ux1) / sw, uy1 / sh), tint);
    };
    const float k = static_cast<float>(kFrameCorner), e = static_cast<float>(kFrameCell - kFrameCorner);
    const float f = static_cast<float>(kFrameCell);
    piece(x0, y0, x0 + c, y0 + c, 0, 0, k, k); // corners
    piece(x1 - c, y0, x1, y0 + c, e, 0, f, k);
    piece(x0, y1 - c, x0 + c, y1, 0, e, k, f);
    piece(x1 - c, y1 - c, x1, y1, e, e, f, f);
    piece(x0 + c, y0, x1 - c, y0 + c, k, 0, e, k); // edges
    piece(x0 + c, y1 - c, x1 - c, y1, k, e, e, f);
    piece(x0, y0 + c, x0 + c, y1 - c, 0, k, k, e);
    piece(x1 - c, y0 + c, x1, y1 - c, e, k, f, e);
}

void DrawDitherBand(ImDrawList* draw, ImVec2 min, ImVec2 max, int rows) {
    if (!g_frames.texture || rows <= 0) return;
    const float tile = static_cast<float>(kDitherCell) * Px(1.0f);
    const float sw = static_cast<float>(g_frames.width), sh = static_cast<float>(g_frames.height);
    const ImTextureID tex = static_cast<ImTextureID>(g_frames.texture);
    for (int r = 0; r < rows; ++r) {
        const int density = std::min(3, r * 4 / rows);
        const float y = Snap(max.y - static_cast<float>(rows - r) * tile);
        if (y + tile < min.y) continue;
        const float u0 = static_cast<float>(kDitherX + density * kDitherCell);
        const float v0 = static_cast<float>(kDitherY);
        for (float x = Snap(min.x); x < max.x; x += tile)
            draw->AddImage(tex, ImVec2(x, y), ImVec2(x + tile, y + tile), ImVec2(u0 / sw, v0 / sh),
                           ImVec2((u0 + kDitherCell) / sw, (v0 + kDitherCell) / sh), IM_COL32_WHITE);
    }
}

// --- Widgets ---

namespace {

struct ButtonLook {
    ImVec4 fill;
    ImVec4 text;
    Frame frame;
    bool framed;
};

ButtonLook LookFor(ButtonStyle style, bool hovered, bool held) {
    switch (style) {
    case ButtonStyle::Primary:
        if (held) return {color::kCream, color::kInk, Frame::Orange, true};
        return {hovered ? color::kCream : color::kOrange, color::kInk, Frame::Cream, true};
    case ButtonStyle::Ghost:
        if (held) return {color::kOrange, color::kInk, Frame::Orange, false};
        return {hovered ? color::kViolet : ImVec4(0, 0, 0, 0), color::kCream, Frame::Violet, false};
    case ButtonStyle::Danger:
        if (held) return {color::kCream, color::kInk, Frame::Cream, true};
        if (hovered) return {color::kOrange, color::kInk, Frame::Cream, true};
        return {color::kInk, color::kOrange, Frame::Orange, true};
    case ButtonStyle::Secondary:
    default:
        if (held) return {color::kOrange, color::kInk, Frame::Cream, true};
        if (hovered) return {color::kViolet, color::kCream, Frame::Orange, true};
        return {color::kPanel, color::kCream, Frame::Violet, true};
    }
}

void PaintButton(ImDrawList* draw, ImVec2 min, ImVec2 max, const ButtonLook& look) {
    if (look.framed) {
        DrawFrame(draw, min, max, look.frame, Col(look.fill));
    } else if (look.fill.w > 0.0f) {
        draw->AddRectFilled(min, max, Col(look.fill));
    }
}

void FocusRing(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemFocused() && io.NavActive && io.NavVisible) {
        const float p = Px(1.0f);
        draw->AddRect(ImVec2(min.x - p, min.y - p), ImVec2(max.x + p, max.y + p), Col(color::kCream), 0.0f, 0, p);
    }
}

} // namespace

bool Button(const char* label, ButtonStyle style, Icon icon, ImVec2 size) {
    const float p = Px(1.0f);
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd);
    const float iconSize = icon != Icon::None ? 16.0f * p : 0.0f;
    const float gap = (icon != Icon::None && textSize.x > 0.0f) ? 3.0f * p : 0.0f;
    const float contentWidth = iconSize + gap + textSize.x;

    float width = size.x;
    if (width == 0.0f) width = contentWidth + 14.0f * p;
    else if (width < 0.0f) width = std::max(8.0f * p, ImGui::GetContentRegionAvail().x + width);
    const float height = size.y > 0.0f ? size.y : 20.0f * p;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ButtonLook look = LookFor(style, hovered, held);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PaintButton(draw, min, max, look);
    FocusRing(draw, min, max);

    const float midY = min.y + height * 0.5f + (held ? p : 0.0f);
    float x = Snap(min.x + (width - contentWidth) * 0.5f);
    if (icon != Icon::None) {
        DrawIcon(draw, icon, ImVec2(x + iconSize * 0.5f, midY), iconSize, Tint());
        x += iconSize + gap;
    }
    if (textSize.x > 0.0f) draw->AddText(ImVec2(x, Snap(midY - textSize.y * 0.5f)), Col(look.text), label, labelEnd);
    return clicked;
}

bool IconButton(const char* id, Icon icon, ButtonStyle style, const char* tooltip, float size) {
    const float p = Px(1.0f);
    const float side = size > 0.0f ? size : 20.0f * p;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + side, min.y + side);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ButtonLook look = LookFor(style, hovered, held);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PaintButton(draw, min, max, look);
    FocusRing(draw, min, max);
    DrawIcon(draw, icon, ImVec2(min.x + side * 0.5f, min.y + side * 0.5f + (held ? p : 0.0f)), 16.0f * p, Tint());

    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool NavItem(Icon icon, const char* label, bool selected) {
    const float p = Px(1.0f);
    const float height = 22.0f * p;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (selected) draw->AddRectFilled(min, max, Col(color::kViolet));
    else if (hovered) draw->AddRectFilled(min, max, Col(color::kPanel));
    if (selected) draw->AddRectFilled(min, ImVec2(min.x + 2.0f * p, max.y), Col(color::kOrange));
    FocusRing(draw, min, max);

    const float midY = min.y + height * 0.5f;
    DrawIcon(draw, icon, ImVec2(min.x + 14.0f * p, midY), 16.0f * p, Tint());
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(min.x + 26.0f * p, Snap(midY - textSize.y * 0.5f)), Col(color::kText), label);
    return clicked;
}

bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered) draw->AddRectFilled(min, max, Col(isClose ? (held ? color::kCream : color::kOrange) : color::kViolet));
    DrawIcon(draw, icon, ImVec2(min.x + size.x * 0.5f, min.y + size.y * 0.5f), 16.0f * Px(1.0f), Tint());
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

namespace {
std::vector<Frame> g_cardFrames;
}

bool BeginCard(const char* id, ImVec2 size, bool autoHeight, ImVec2 padding, Frame frame) {
    // The panel itself is drawn in EndCard, once the final size is known.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGuiChildFlags flags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (autoHeight) flags |= ImGuiChildFlags_AutoResizeY;
    const bool visible = ImGui::BeginChild(id, size, flags, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    g_cardFrames.push_back(frame);
    return visible;
}

void EndCard() {
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::EndChild();
    const Frame frame = g_cardFrames.empty() ? Frame::Violet : g_cardFrames.back();
    if (!g_cardFrames.empty()) g_cardFrames.pop_back();
    // A child window draws after its parent, so this lands behind the card's contents.
    DrawFrame(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + size.x, pos.y + size.y), frame, Col(color::kPanel));
}

void PageTitleText(const char* title, const char* subtitle) {
    ImGui::PushFont(g_fonts.heading);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle && *subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopStyleColor();
    }
}

void PageTitle(const char* title, const char* subtitle) {
    PageTitleText(title, subtitle);
    ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + ImGui::GetContentRegionAvail().x, min.y + Px(1.0f)),
                                              Col(color::kViolet));
    ImGui::Dummy(ImVec2(0.0f, Px(5.0f)));
}

void SectionLabel(const char* text) {
    ImGui::PushFont(g_fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Keycap(const char* text) {
    const float p = Px(1.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 size(std::max(textSize.x + 8.0f * p, 13.0f * p), 13.0f * p);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawFrame(draw, min, max, Frame::Orange, Col(color::kInk));
    draw->AddText(ImVec2(Snap(min.x + (size.x - textSize.x) * 0.5f), Snap(min.y + (size.y - textSize.y) * 0.5f)),
                  Col(color::kText), text);
}

void FormLabel(const char* label, float labelWidth) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetCursorStartPos().x + labelWidth);
    ImGui::SetNextItemWidth(std::min(280.0f, ImGui::GetContentRegionAvail().x));
}

void HintText(const char* text) {
    ImGui::PushFont(g_fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Muted(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    HintText(text);
}

// --- Game artwork ---

std::string Initials(const std::string& title) {
    static const char* const kSkipped[] = {"THE", "OF", "A", "AN", "AND", "FOR", "IN"};
    auto wordChar = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '\''; };
    std::string out;
    size_t i = 0;
    while (i < title.size() && out.size() < 4) {
        while (i < title.size() && !wordChar(title[i])) ++i;
        std::string word;
        while (i < title.size() && wordChar(title[i])) {
            if (title[i] != '\'') word += static_cast<char>(std::toupper(static_cast<unsigned char>(title[i])));
            ++i;
        }
        if (word.empty()) continue;
        const bool numeric =
            std::all_of(word.begin(), word.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); });
        if (numeric) out += word.substr(0, 2);
        else if (std::none_of(std::begin(kSkipped), std::end(kSkipped), [&](const char* w) { return word == w; }))
            out += word[0];
    }
    if (out.empty()) {
        for (char ch : title)
            if (std::isalnum(static_cast<unsigned char>(ch)) && out.size() < 2)
                out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (out.size() > 4) out.resize(4);
    return out;
}

ImU32 PlaceholderColor(const std::string& title) {
    // Colours from the interface's family but clearly apart from each other, so games are easy to tell apart.
    static const unsigned kColors[] = {0x4B3BB5, 0x2A6F97, 0xB5541A, 0x1E8A7A, 0x8A3F8F, 0x9A3B5A};
    unsigned hash = 2166136261u;
    for (unsigned char ch : title) hash = (hash ^ ch) * 16777619u;
    const unsigned rgb = kColors[hash % (sizeof(kColors) / sizeof(kColors[0]))];
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

} // namespace NovaUI::kit
