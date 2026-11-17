#include "UiKit.hpp"

#include <glad/gl.h>
#include <imgui_internal.h> // FindRenderedTextEnd
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
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

// The fonts and the background picture, compiled in by CMake as lists of byte values (see assets/fonts/ and
// assets/ui/). Inter is under the SIL Open Font License and Phosphor under the MIT licence.
const unsigned char kInterRegular[] = {
#include "InterRegular.inc"
};
const unsigned char kInterSemiBold[] = {
#include "InterSemiBold.inc"
};
const unsigned char kInterBold[] = {
#include "InterBold.inc"
};
const unsigned char kPhosphorRegular[] = {
#include "PhosphorRegular.inc"
};
const unsigned char kPhosphorFill[] = {
#include "PhosphorFill.inc"
};
const unsigned char kBackgroundJpg[] = {
#include "Background.inc"
};

struct Texture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

Texture g_background;
Texture g_ramp; ///< 1x64: opaque white at the bottom fading to nothing at the top
Fonts g_fonts;

struct IconFont {
    float size;
    ImFont* font;
};
std::vector<IconFont> g_iconsFilled, g_iconsRegular;

Texture MakeTexture(const unsigned char* rgba, int w, int h) {
    Texture t;
    t.width = w;
    t.height = h;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// Colour with the current style alpha applied, so disabled widgets fade with everything else.
ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }

ImVec4 WithAlpha(ImVec4 c, float a) {
    c.w *= a;
    return c;
}

float LogicalSize(ImFont* font) { return font->FontSize * ImGui::GetIO().FontGlobalScale; }

} // namespace

ImVec2 Measure(ImFont* font, const char* text) {
    if (!font) return ImVec2(0, 0);
    return font->CalcTextSizeA(LogicalSize(font), FLT_MAX, 0.0f, text);
}

void PutText(ImDrawList* draw, ImFont* font, ImVec2 pos, ImU32 color, const char* text, const char* end) {
    if (font) draw->AddText(font, LogicalSize(font), pos, color, text, end);
}

std::string Ellipsize(ImFont* font, const std::string& text, float maxWidth) {
    if (Measure(font, text.c_str()).x <= maxWidth) return text;
    std::string out = text;
    while (!out.empty()) {
        // Drop one whole UTF-8 character.
        size_t cut = out.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (Measure(font, (out + "...").c_str()).x <= maxWidth) break;
    }
    return out + "...";
}

std::string ClampLines(ImFont* font, const std::string& text, float width, int maxLines) {
    if (!font || maxLines < 1) return text;
    const float scale = ImGui::GetIO().FontGlobalScale;
    std::string out;
    const char* p = text.c_str();
    const char* const end = p + text.size();
    for (int line = 0; line < maxLines && p < end; ++line) {
        const char* stop = font->CalcWordWrapPositionA(scale, p, end, width);
        if (stop <= p) stop = p + 1;
        std::string piece(p, stop);
        while (!piece.empty() && piece.back() == ' ') piece.pop_back();
        p = stop;
        while (p < end && *p == ' ') ++p;
        const bool last = (line == maxLines - 1);
        if (last && p < end) piece = Ellipsize(font, piece + " ...", width);
        if (line > 0) out += '\n';
        out += piece;
    }
    return out;
}

// --- Theme ---

void ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    const ImVec4 clear(0, 0, 0, 0);

    c[ImGuiCol_Text] = color::kText;
    c[ImGuiCol_TextDisabled] = color::kTextDim;
    c[ImGuiCol_WindowBg] = color::kBackground;
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = color::kRaised;
    c[ImGuiCol_Border] = color::kHairline;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = color::kRaised;
    c[ImGuiCol_FrameBgHovered] = color::kHover;
    c[ImGuiCol_FrameBgActive] = color::kHairline;
    c[ImGuiCol_TitleBg] = color::kRaised;
    c[ImGuiCol_TitleBgActive] = color::kRaised;
    c[ImGuiCol_TitleBgCollapsed] = color::kRaised;
    c[ImGuiCol_MenuBarBg] = clear;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = Rgb(0xFFEADB, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0xFFEADB, 0.30f);
    c[ImGuiCol_ScrollbarGrabActive] = Rgb(0xFFEADB, 0.42f);
    c[ImGuiCol_CheckMark] = color::kAccentHover;
    c[ImGuiCol_SliderGrab] = color::kAccentHover;
    c[ImGuiCol_SliderGrabActive] = color::kAccentText;
    c[ImGuiCol_Button] = color::kRaised;
    c[ImGuiCol_ButtonHovered] = color::kHover;
    c[ImGuiCol_ButtonActive] = color::kHairline;
    c[ImGuiCol_Header] = color::kRaised;
    c[ImGuiCol_HeaderHovered] = color::kHover;
    c[ImGuiCol_HeaderActive] = color::kHairline;
    c[ImGuiCol_Separator] = color::kHairline;
    c[ImGuiCol_SeparatorHovered] = color::kAccentHover;
    c[ImGuiCol_SeparatorActive] = color::kAccentHover;
    c[ImGuiCol_ResizeGrip] = Rgb(0xFFEADB, 0.12f);
    c[ImGuiCol_ResizeGripHovered] = color::kAccentHover;
    c[ImGuiCol_ResizeGripActive] = color::kAccentPressed;
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = Rgb(0xE8702E, 0.40f);
    c[ImGuiCol_TabActive] = Rgb(0xE8702E, 0.55f);
    c[ImGuiCol_TabUnfocused] = clear;
    c[ImGuiCol_TabUnfocusedActive] = Rgb(0xE8702E, 0.40f);
    c[ImGuiCol_TabSelectedOverline] = color::kAccentHover;
    c[ImGuiCol_TabDimmedSelectedOverline] = color::kAccentHover;
    c[ImGuiCol_TableHeaderBg] = Rgb(0xFFEADB, 0.08f);
    c[ImGuiCol_TableBorderStrong] = Rgb(0xFFEADB, 0.12f);
    c[ImGuiCol_TableBorderLight] = Rgb(0xFFEADB, 0.06f);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = Rgb(0xFFEADB, 0.025f);
    c[ImGuiCol_TextSelectedBg] = Rgb(0xE8702E, 0.45f);
    c[ImGuiCol_NavCursor] = color::kAccentHover;
    c[ImGuiCol_DragDropTarget] = color::kAccentHover;
    c[ImGuiCol_ModalWindowDimBg] = Rgb(0x080403, 0.7f);

    style.WindowRounding = 0.0f;
    style.ChildRounding = radius::kCard;
    style.FrameRounding = radius::kControl;
    style.PopupRounding = radius::kCard;
    style.ScrollbarRounding = radius::kControl;
    style.GrabRounding = radius::kControl;
    style.TabRounding = radius::kControl;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.WindowPadding = ImVec2(space::k4, space::k4);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.ItemSpacing = ImVec2(space::k3, space::k2);
    style.ItemInnerSpacing = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(10.0f, 7.0f);
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 16.0f;
    style.IndentSpacing = 20.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.45f;
}

// --- Fonts and pictures ---

void LoadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    const float dpi = pixelScale >= 1.0f ? pixelScale : 1.0f;

    static const ImWchar kIconRange[] = {0xE000, 0xF8FF, 0};
    auto add = [&](const unsigned char* data, size_t size, float logical, const ImWchar* ranges = nullptr) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false; // the bytes are static
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.GlyphRanges = ranges;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size), logical * dpi,
                                              &config);
    };

    g_fonts.body = add(kInterRegular, sizeof(kInterRegular), 15.0f);
    g_fonts.small = add(kInterRegular, sizeof(kInterRegular), 13.0f);
    g_fonts.caption = add(kInterSemiBold, sizeof(kInterSemiBold), 12.0f);
    g_fonts.label = add(kInterSemiBold, sizeof(kInterSemiBold), 15.0f);
    g_fonts.heading = add(kInterBold, sizeof(kInterBold), 24.0f);
    g_fonts.title = add(kInterBold, sizeof(kInterBold), 32.0f);
    g_fonts.display = add(kInterBold, sizeof(kInterBold), 44.0f);
    g_iconsFilled.clear();
    g_iconsRegular.clear();
    for (float size : {16.0f, 20.0f, 24.0f, 32.0f, 48.0f}) {
        g_iconsFilled.push_back({size, add(kPhosphorFill, sizeof(kPhosphorFill), size, kIconRange)});
        g_iconsRegular.push_back({size, add(kPhosphorRegular, sizeof(kPhosphorRegular), size, kIconRange)});
    }

    if (!g_fonts.body || !g_fonts.small || !g_fonts.caption || !g_fonts.label || !g_fonts.heading || !g_fonts.title ||
        !g_fonts.display) {
        // Fall back to ImGui's own font for text rather than draw with a null font.
        io.Fonts->Clear();
        ImFontConfig fallback;
        fallback.SizePixels = 15.0f * dpi;
        ImFont* builtin = io.Fonts->AddFontDefault(&fallback);
        g_fonts = Fonts{builtin, builtin, builtin, builtin, builtin, builtin, builtin};
        g_iconsFilled.clear();
        g_iconsRegular.clear();
    }
    io.FontDefault = g_fonts.body;
    io.FontGlobalScale = 1.0f / dpi;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(kBackgroundJpg, static_cast<int>(sizeof(kBackgroundJpg)), &w, &h, &channels, 4);
    if (pixels) {
        g_background = MakeTexture(pixels, w, h);
        stbi_image_free(pixels);
    }
    unsigned char ramp[64 * 4];
    for (int i = 0; i < 64; ++i) {
        ramp[i * 4 + 0] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = 255;
        ramp[i * 4 + 3] = static_cast<unsigned char>(i * 255 / 63);
    }
    g_ramp = MakeTexture(ramp, 1, 64);
}

const Fonts& GetFonts() { return g_fonts; }

// --- Icons ---

namespace {

unsigned IconCodepoint(Icon icon) {
    switch (icon) {
    case Icon::Home:         return 0xE2C2;
    case Icon::Games:        return 0xE26E;
    case Icon::Recent:       return 0xE1A0;
    case Icon::Star:         return 0xE46A;
    case Icon::Settings:     return 0xE270;
    case Icon::Info:         return 0xE2CE;
    case Icon::Play:         return 0xE3D0;
    case Icon::Stop:         return 0xE46C;
    case Icon::Pause:        return 0xE39E;
    case Icon::Rewind:       return 0xE6A8;
    case Icon::FastForward:  return 0xE6A6;
    case Icon::Plus:         return 0xE3D4;
    case Icon::Edit:         return 0xE3B4;
    case Icon::Trash:        return 0xE4A6;
    case Icon::Folder:       return 0xE256;
    case Icon::Image:        return 0xE2CA;
    case Icon::Search:       return 0xE30C;
    case Icon::Check:        return 0xE182;
    case Icon::Warning:      return 0xE4E0;
    case Icon::ChevronRight: return 0xE13A;
    case Icon::ChevronDown:  return 0xE136;
    case Icon::ArrowRight:   return 0xE06C;
    case Icon::ArrowLeft:    return 0xE058;
    case Icon::Close:        return 0xE4F6;
    case Icon::Minimize:     return 0xE32A;
    case Icon::Maximize:     return 0xE45E;
    case Icon::Restore:      return 0xE1CA;
    case Icon::Scripts:      return 0xE1BC;
    case Icon::Memory:       return 0xE610;
    case Icon::Trophy:       return 0xE67E;
    case Icon::Simulator:    return 0xE408;
    case Icon::Heart:        return 0xE2A8;
    case Icon::More:         return 0xE1FE;
    case Icon::Calendar:     return 0xE10A;
    case Icon::Publisher:    return 0xE102;
    case Icon::Display:      return 0xE32E;
    case Icon::Sound:        return 0xE44A;
    case Icon::Keyboard:     return 0xE2D8;
    case Icon::Lightning:    return 0xE2DE;
    case Icon::Tag:          return 0xE478;
    case Icon::Link:         return 0xE2E2;
    case Icon::Download:     return 0xE20C;
    case Icon::None:         break;
    }
    return 0;
}

int Utf8(unsigned cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        out[1] = 0;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        out[2] = 0;
        return 2;
    }
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    out[3] = 0;
    return 3;
}

ImFont* IconFontFor(float size, bool filled) {
    const std::vector<IconFont>& fonts = filled ? g_iconsFilled : g_iconsRegular;
    ImFont* best = nullptr;
    float bestDiff = FLT_MAX;
    for (const IconFont& f : fonts) {
        const float diff = std::fabs(f.size - size);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = f.font;
        }
    }
    return best;
}

} // namespace

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color, bool filled) {
    const unsigned cp = IconCodepoint(icon);
    ImFont* font = IconFontFor(size, filled);
    if (!cp || !font) return;
    char text[8];
    Utf8(cp, text);
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    draw->AddText(font, size, ImVec2(std::round(center.x - extent.x * 0.5f), std::round(center.y - extent.y * 0.5f)),
                  color, text);
}

// --- Shapes ---

void DrawGradientFill(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 base, float rounding, float shine) {
    draw->AddRectFilled(min, max, base, rounding);
    if (!g_ramp.id || shine <= 0.0f) return;
    // The ramp is opaque at the bottom, so flip it to put the shine at the top.
    draw->AddImageRounded(static_cast<ImTextureID>(g_ramp.id), min, max, ImVec2(0, 1), ImVec2(1, 0),
                          ImGui::GetColorU32(ImVec4(1, 1, 1, shine)), rounding);
}

void DrawBackground(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    draw->AddRectFilled(min, max, ImGui::GetColorU32(color::kBackground));
    if (g_background.id) DrawImageCover(draw, g_background.id, g_background.width, g_background.height, min, max, 0.0f);
}

void DrawShade(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float strength) {
    if (!g_ramp.id) return;
    draw->AddImageRounded(static_cast<ImTextureID>(g_ramp.id), min, max, ImVec2(0, 0), ImVec2(1, 1),
                          ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.06f, strength)), rounding);
}

void DrawImageCover(ImDrawList* draw, unsigned texture, int texWidth, int texHeight, ImVec2 min, ImVec2 max,
                    float rounding, ImU32 tint) {
    const float boxW = max.x - min.x, boxH = max.y - min.y;
    if (boxW <= 0 || boxH <= 0 || texWidth <= 0 || texHeight <= 0) return;
    const float boxAspect = boxW / boxH, texAspect = static_cast<float>(texWidth) / static_cast<float>(texHeight);
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (texAspect > boxAspect) { // the picture is wider: crop the sides
        const float keep = boxAspect / texAspect;
        uv0.x = (1 - keep) * 0.5f;
        uv1.x = uv0.x + keep;
    } else { // taller: crop the top and bottom
        const float keep = texAspect / boxAspect;
        uv0.y = (1 - keep) * 0.5f;
        uv1.y = uv0.y + keep;
    }
    draw->AddImageRounded(static_cast<ImTextureID>(texture), min, max, uv0, uv1, tint, rounding);
}

void DrawSelectionRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding) {
    // A crisp outline with a gap, not a glow.
    draw->AddRect(ImVec2(min.x - 4, min.y - 4), ImVec2(max.x + 4, max.y + 4), ImGui::GetColorU32(color::kAccentHover),
                  rounding + 4.0f, 0, 2.0f);
}

// --- Widgets ---

bool NavButton(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    return pressed;
}

namespace {

struct ButtonLook {
    ImVec4 fill;
    ImVec4 border;
    ImVec4 text;
    bool gloss;
};

ButtonLook LookFor(ButtonStyle style, bool hovered, bool held) {
    switch (style) {
    case ButtonStyle::Primary:
        return {held      ? color::kAccentPressed
                : hovered ? color::kAccentHover
                          : color::kAccent,
                ImVec4(0, 0, 0, 0), color::kOnAccent, false};
    case ButtonStyle::Ghost:
        return {held      ? Rgb(0xFFEADB, 0.14f)
                : hovered ? Rgb(0xFFEADB, 0.08f)
                          : ImVec4(0, 0, 0, 0),
                ImVec4(0, 0, 0, 0), hovered ? color::kText : color::kTextMuted, false};
    case ButtonStyle::Danger:
        return {held      ? WithAlpha(color::kDanger, 0.32f)
                : hovered ? WithAlpha(color::kDanger, 0.22f)
                          : WithAlpha(color::kDanger, 0.10f),
                WithAlpha(color::kDanger, hovered ? 0.8f : 0.5f), hovered ? Rgb(0xFFD9DC) : color::kDanger, false};
    case ButtonStyle::Secondary:
    default:
        return {held      ? Rgb(0xFFEADB, 0.17f)
                : hovered ? Rgb(0xFFEADB, 0.12f)
                          : Rgb(0xFFEADB, 0.07f),
                Rgb(0xFFEADB, hovered ? 0.20f : 0.12f), color::kText, false};
    }
}

constexpr float kButtonHeight = 44.0f;
constexpr float kButtonRounding = radius::kControl;

void Paint(ImDrawList* draw, ImVec2 min, ImVec2 max, const ButtonLook& look, float rounding) {
    if (look.gloss) DrawGradientFill(draw, min, max, Col(look.fill), rounding);
    else if (look.fill.w > 0.0f) draw->AddRectFilled(min, max, Col(look.fill), rounding);
    if (look.border.w > 0.0f) draw->AddRect(min, max, Col(look.border), rounding, 0, 1.0f);
}

void FocusRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemFocused() && io.NavActive && io.NavVisible)
        draw->AddRect(ImVec2(min.x - 2, min.y - 2), ImVec2(max.x + 2, max.y + 2), Col(color::kAccentHover),
                      rounding + 2.0f, 0, 2.0f);
}

} // namespace

bool Button(const char* label, ButtonStyle style, Icon icon, ImVec2 size, bool iconAfterLabel) {
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 textSize = Measure(g_fonts.label, std::string(label, labelEnd).c_str());
    const float iconSize = icon != Icon::None ? 20.0f : 0.0f;
    const float gap = (icon != Icon::None && textSize.x > 0.0f) ? 10.0f : 0.0f;
    const float contentWidth = iconSize + gap + textSize.x;

    float width = size.x;
    if (width == 0.0f) width = contentWidth + 40.0f;
    else if (width < 0.0f) width = std::max(40.0f, ImGui::GetContentRegionAvail().x + width);
    const float height = size.y > 0.0f ? size.y : kButtonHeight;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    const bool clicked = NavButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ButtonLook look = LookFor(style, hovered, held);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Paint(draw, min, max, look, kButtonRounding);
    FocusRing(draw, min, max, kButtonRounding);

    const float midY = min.y + height * 0.5f;
    float x = std::round(min.x + (width - contentWidth) * 0.5f);
    if (icon != Icon::None && !iconAfterLabel) {
        DrawIcon(draw, icon, ImVec2(x + iconSize * 0.5f, midY), iconSize, Col(look.text));
        x += iconSize + gap;
    }
    if (textSize.x > 0.0f) {
        PutText(draw, g_fonts.label, ImVec2(x, std::round(midY - textSize.y * 0.5f)), Col(look.text), label, labelEnd);
        x += textSize.x + gap;
    }
    if (icon != Icon::None && iconAfterLabel)
        DrawIcon(draw, icon, ImVec2(x + iconSize * 0.5f, midY), iconSize, Col(look.text));
    return clicked;
}

bool IconButton(const char* id, Icon icon, ButtonStyle style, const char* tooltip, float size, bool filled) {
    const float side = size > 0.0f ? size : kButtonHeight;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + side, min.y + side);
    const bool clicked = NavButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ButtonLook look = LookFor(style, hovered, held);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Paint(draw, min, max, look, kButtonRounding);
    FocusRing(draw, min, max, kButtonRounding);
    DrawIcon(draw, icon, ImVec2(min.x + side * 0.5f, min.y + side * 0.5f), std::round(side * 0.5f), Col(look.text),
             filled);

    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool NavItem(Icon icon, const char* label, bool selected, bool compact) {
    const float kHeight = compact ? 36.0f : 44.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + kHeight);
    const bool clicked = NavButton(label, ImVec2(width, kHeight));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (selected) {
        draw->AddRectFilled(min, max, Col(color::kAccentSoft), radius::kControl);
        draw->AddRectFilled(ImVec2(min.x, min.y + 10.0f), ImVec2(min.x + 3.0f, max.y - 10.0f), Col(color::kAccentHover),
                            1.5f);
    } else if (hovered) {
        draw->AddRectFilled(min, max, Col(color::kHover), radius::kControl);
    }
    FocusRing(draw, min, max, radius::kControl);

    const ImVec4& fg = selected ? color::kText : hovered ? color::kText : color::kTextMuted;
    const float midY = min.y + kHeight * 0.5f;
    DrawIcon(draw, icon, ImVec2(min.x + 28.0f, midY), 20.0f, Col(fg));
    const ImVec2 textSize = Measure(g_fonts.label, label);
    PutText(draw, g_fonts.label, ImVec2(min.x + 52.0f, std::round(midY - textSize.y * 0.5f)), Col(fg), label);
    return clicked;
}

bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool clicked = NavButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec4 fg = hovered ? color::kText : color::kTextMuted;
    if (hovered) {
        draw->AddRectFilled(min, max,
                            Col(isClose ? (held ? Rgb(0xA82317) : Rgb(0xD13438)) : Rgb(0xFFEADB, held ? 0.16f : 0.10f)),
                            8.0f);
        if (isClose) fg = color::kOnAccent;
    }
    DrawIcon(draw, icon, ImVec2(min.x + size.x * 0.5f, min.y + size.y * 0.5f), 16.0f, Col(fg), false);
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool Toggle(const char* id, bool* value) {
    constexpr ImVec2 kSize(44.0f, 24.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + kSize.x, min.y + kSize.y);
    bool changed = false;
    if (NavButton(id, kSize)) {
        *value = !*value;
        changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();

    // The knob glides to its side instead of jumping.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetItemID();
    const float target = *value ? 1.0f : 0.0f;
    float t = storage->GetFloat(key, target);
    t += (target - t) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 22.0f));
    if (std::fabs(target - t) < 0.002f) t = target;
    storage->SetFloat(key, t);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 off = Rgb(0xFFEADB, hovered ? 0.18f : 0.12f);
    const ImVec4 on = hovered ? color::kAccentHover : color::kAccent;
    const ImVec4 track(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t,
                       off.w + (on.w - off.w) * t);
    draw->AddRectFilled(min, max, Col(track), kSize.y * 0.5f);
    const float radius = kSize.y * 0.5f - 3.0f;
    const float x = min.x + kSize.y * 0.5f + (kSize.x - kSize.y) * t;
    draw->AddCircleFilled(ImVec2(x, min.y + kSize.y * 0.5f), radius, Col(color::kKnob));
    FocusRing(draw, min, max, kSize.y * 0.5f);
    return changed;
}

float SegmentedWidth(const char* const* labels, int count) {
    float segment = 0.0f;
    for (int i = 0; i < count; ++i) segment = std::max(segment, Measure(g_fonts.label, labels[i]).x + 28.0f);
    return segment * static_cast<float>(count) + 6.0f;
}

bool Segmented(const char* id, const char* const* labels, int count, int* index, float width) {
    constexpr float kHeight = 32.0f, kPad = 3.0f;
    float segment = (SegmentedWidth(labels, count) - kPad * 2.0f) / static_cast<float>(count);
    if (width > 0.0f) segment = std::max(segment, (width - kPad * 2.0f) / static_cast<float>(count));
    const float total = segment * static_cast<float>(count) + kPad * 2.0f;

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + total, min.y + kHeight), Col(Rgb(0xFFEADB, 0.07f)), 10.0f);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(min.x + kPad + segment * static_cast<float>(i), min.y + kPad));
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + segment, a.y + kHeight - kPad * 2.0f);
        ImGui::PushID(i);
        if (NavButton("##segment", ImVec2(segment, kHeight - kPad * 2.0f)) && *index != i) {
            *index = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool selected = (*index == i);
        if (selected) draw->AddRectFilled(a, b, Col(color::kAccent), 8.0f);
        else if (hovered) draw->AddRectFilled(a, b, Col(Rgb(0xFFEADB, 0.08f)), 8.0f);
        FocusRing(draw, a, b, 8.0f);
        const ImVec2 text = Measure(g_fonts.label, labels[i]);
        PutText(draw, g_fonts.label,
                ImVec2(std::round(a.x + (segment - text.x) * 0.5f), std::round(a.y + (b.y - a.y - text.y) * 0.5f)),
                Col(selected ? color::kOnAccent : color::kTextMuted), labels[i]);
    }
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(total, kHeight));
    ImGui::PopID();
    return changed;
}

bool Slider(const char* id, int* value, int min, int max, const char* format, float width) {
    constexpr float kValueWidth = 52.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float track = width - kValueWidth - space::k3;

    // ImGui's slider does the input, keyboard and controller handling; it is drawn invisibly and replaced here.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    ImGui::SetNextItemWidth(track);
    const bool changed = ImGui::SliderInt(id, value, min, max, "", ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor(7);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float midY = (a.y + b.y) * 0.5f;
    const float left = a.x + 8.0f, right = b.x - 8.0f;
    const float t = max > min ? static_cast<float>(*value - min) / static_cast<float>(max - min) : 0.0f;
    const float knobX = left + (right - left) * t;
    draw->AddRectFilled(ImVec2(left, midY - 2.0f), ImVec2(right, midY + 2.0f), Col(color::kHover), 2.0f);
    draw->AddRectFilled(ImVec2(left, midY - 2.0f), ImVec2(knobX, midY + 2.0f), Col(color::kAccent), 2.0f);
    draw->AddCircleFilled(ImVec2(knobX, midY), hovered ? 9.0f : 8.0f, Col(color::kKnob));
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
        draw->AddCircle(ImVec2(knobX, midY), 12.0f, Col(color::kAccentHover), 0, 2.0f);

    char text[32];
    std::snprintf(text, sizeof(text), format, *value);
    const ImVec2 size = Measure(g_fonts.label, text);
    PutText(draw, g_fonts.label, ImVec2(std::round(origin.x + width - size.x), std::round(midY - size.y * 0.5f)),
            Col(color::kText), text);
    return changed;
}

bool TabStrip(const char* id, const char* const* labels, int count, int* index) {
    constexpr float kHeight = 40.0f;
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    bool changed = false;
    float x = origin.x;
    for (int i = 0; i < count; ++i) {
        const ImVec2 text = Measure(g_fonts.label, labels[i]);
        const float width = text.x + 32.0f;
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::PushID(i);
        if (NavButton("##tab", ImVec2(width, kHeight)) && *index != i) {
            *index = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool selected = (*index == i);
        PutText(draw, g_fonts.label,
                ImVec2(std::round(x + 16.0f), std::round(origin.y + (kHeight - text.y) * 0.5f - 1.0f)),
                Col(selected  ? color::kText
                    : hovered ? color::kText
                              : color::kTextMuted),
                labels[i]);
        if (selected)
            draw->AddRectFilled(ImVec2(x + 16.0f, origin.y + kHeight - 3.0f),
                                ImVec2(x + 16.0f + text.x, origin.y + kHeight - 1.0f), Col(color::kAccentHover), 1.0f);
        FocusRing(draw, ImVec2(x + 4.0f, origin.y + 4.0f), ImVec2(x + width - 4.0f, origin.y + kHeight - 4.0f), 8.0f);
        x += width;
    }
    const float total = ImGui::GetContentRegionAvail().x;
    draw->AddLine(ImVec2(origin.x, origin.y + kHeight - 0.5f), ImVec2(origin.x + total, origin.y + kHeight - 0.5f),
                  Col(Rgb(0xFFEADB, 0.08f)));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(total, kHeight));
    ImGui::PopID();
    return changed;
}

namespace {

constexpr float kRowPadX = space::k4;
constexpr float kRowMinHeight = 64.0f;
bool g_firstRow = true;

struct RowState {
    ImVec2 min;
    float width = 0.0f;
    float height = 0.0f;
};
RowState g_row;

} // namespace

void RowsReset() { g_firstRow = true; }

void InfoRow(const char* key, const char* value) {
    constexpr float kHeight = 40.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (!g_firstRow)
        draw->AddLine(ImVec2(min.x + kRowPadX, min.y), ImVec2(min.x + width - kRowPadX, min.y), Col(color::kHover));
    g_firstRow = false;
    const ImVec2 keySize = Measure(g_fonts.body, key);
    PutText(draw, g_fonts.body, ImVec2(min.x + kRowPadX, std::round(min.y + (kHeight - keySize.y) * 0.5f)),
            Col(color::kTextMuted), key);
    const std::string shown = Ellipsize(g_fonts.body, value, std::max(80.0f, width - keySize.x - kRowPadX * 3.0f));
    const ImVec2 valueSize = Measure(g_fonts.body, shown.c_str());
    PutText(
        draw, g_fonts.body,
        ImVec2(std::round(min.x + width - kRowPadX - valueSize.x), std::round(min.y + (kHeight - valueSize.y) * 0.5f)),
        Col(color::kText), shown.c_str());
    g_row = {min, width, kHeight};
    rows::EndRow();
}

void BeginColumn(float maxWidth) {
    const float width = std::min(maxWidth, ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##column", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void EndColumn() { ImGui::EndChild(); }

void BeginSettings(const char* id, const char* heading) {
    if (heading && *heading) {
        SectionLabel(heading);
        ImGui::Dummy(ImVec2(0.0f, space::k1));
    }
    BeginCard(id, ImVec2(0, 0), true, ImVec2(0.0f, 0.0f), 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    g_firstRow = true;
}

void EndSettings() {
    ImGui::PopStyleVar();
    EndCard();
    ImGui::Dummy(ImVec2(0.0f, space::k4));
}

namespace rows {

ImVec2 BeginRow(const char* title, const char* help, float controlWidth, float controlHeight, float* rowHeight,
                const char* badge) {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float textWidth = std::max(120.0f, width - controlWidth - kRowPadX * 3.0f);
    const bool hasHelp = help && *help;
    const std::string wrapped = hasHelp ? ClampLines(g_fonts.small, help, textWidth, 2) : std::string();
    const ImVec2 helpSize = hasHelp ? Measure(g_fonts.small, wrapped.c_str()) : ImVec2(0, 0);
    const ImVec2 titleSize = Measure(g_fonts.label, title);
    const float textBlock = titleSize.y + (hasHelp ? space::k1 + helpSize.y : 0.0f);
    *rowHeight = std::max({kRowMinHeight, textBlock + space::k3 * 2.0f, controlHeight + space::k3 * 2.0f});
    g_row = {min, width, *rowHeight};

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (!g_firstRow)
        draw->AddLine(ImVec2(min.x + kRowPadX, min.y), ImVec2(min.x + width - kRowPadX, min.y),
                      Col(Rgb(0xFFEADB, 0.07f)));
    g_firstRow = false;

    const float top = min.y + (*rowHeight - textBlock) * 0.5f;
    PutText(draw, g_fonts.label, ImVec2(min.x + kRowPadX, std::round(top)), Col(color::kText), title);
    if (badge && *badge) {
        std::string upper(badge);
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const ImVec2 text = Measure(g_fonts.caption, upper.c_str());
        const ImVec2 a(min.x + kRowPadX + titleSize.x + space::k2, top + (titleSize.y - text.y) * 0.5f - 3.0f);
        const ImVec2 b(a.x + text.x + space::k3, a.y + text.y + 6.0f);
        draw->AddRectFilled(a, b, Col(color::kAccentSoft), 5.0f);
        PutText(draw, g_fonts.caption, ImVec2(a.x + space::k3 * 0.5f, a.y + 3.0f), Col(color::kAccentText),
                upper.c_str());
    }
    if (hasHelp)
        PutText(draw, g_fonts.small, ImVec2(min.x + kRowPadX, std::round(top + titleSize.y + space::k1)),
                Col(color::kTextMuted), wrapped.c_str());
    return ImVec2(min.x + width - kRowPadX - controlWidth, min.y + (*rowHeight - controlHeight) * 0.5f);
}

void EndRow() {
    // The control was placed with SetCursorScreenPos; put the cursor under the row and extend the card to it.
    ImGui::SetCursorScreenPos(ImVec2(g_row.min.x, g_row.min.y + g_row.height - 1.0f));
    ImGui::Dummy(ImVec2(g_row.width, 1.0f));
}

} // namespace rows

namespace {
struct CardState {
    float rounding;
};
std::vector<CardState> g_cards;
} // namespace

bool BeginCard(const char* id, ImVec2 size, bool autoHeight, ImVec2 padding, float rounding) {
    // The panel itself is drawn in EndCard, once the final size is known.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGuiChildFlags flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened;
    if (autoHeight) flags |= ImGuiChildFlags_AutoResizeY;
    const bool visible = ImGui::BeginChild(id, size, flags, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    g_cards.push_back({rounding});
    return visible;
}

void EndCard() {
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::EndChild();
    const float rounding = g_cards.empty() ? radius::kCard : g_cards.back().rounding;
    if (!g_cards.empty()) g_cards.pop_back();
    // A child window draws after its parent, so this lands behind the card's contents.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, max, Col(color::kCard), rounding);
    draw->AddRect(pos, max, Col(color::kHairline), rounding, 0, 1.0f);
}

void PageTitleText(const char* title, const char* subtitle) {
    ImGui::PushFont(g_fonts.title);
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
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
}

void SectionTitle(const char* text) {
    ImGui::PushFont(g_fonts.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void SectionLabel(const char* text) {
    std::string upper(text);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    ImGui::PushFont(g_fonts.caption);
    ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Keycap(const char* text) {
    const ImVec2 textSize = Measure(g_fonts.label, text);
    const ImVec2 size(std::max(textSize.x + 22.0f, 34.0f), 30.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, Col(Rgb(0xFFEADB, 0.09f)), 8.0f);
    draw->AddRect(min, max, Col(Rgb(0xFFEADB, 0.22f)), 8.0f, 0, 1.0f);
    PutText(draw, g_fonts.label,
            ImVec2(std::round(min.x + (size.x - textSize.x) * 0.5f), std::round(min.y + (size.y - textSize.y) * 0.5f)),
            Col(color::kText), text);
}

void Chip(const char* text, Icon icon, ImU32 iconColor) {
    const ImVec2 textSize = Measure(g_fonts.small, text);
    const float iconSize = icon != Icon::None ? 16.0f : 0.0f;
    const float gap = icon != Icon::None ? 7.0f : 0.0f;
    const ImVec2 size(textSize.x + iconSize + gap + 2.0f, 24.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float midY = min.y + size.y * 0.5f;
    float x = min.x;
    if (icon != Icon::None) {
        DrawIcon(draw, icon, ImVec2(x + iconSize * 0.5f, midY), iconSize,
                 iconColor ? iconColor : Col(color::kTextMuted));
        x += iconSize + gap;
    }
    PutText(draw, g_fonts.small, ImVec2(x, std::round(midY - textSize.y * 0.5f)), Col(color::kTextMuted), text);
}

void ControllerHint(const char* glyph, const char* label) {
    const ImVec2 labelSize = Measure(g_fonts.small, label);
    const float circle = 26.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(circle + 8.0f + labelSize.x, circle));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mid(min.x + circle * 0.5f, min.y + circle * 0.5f);
    draw->AddCircle(mid, circle * 0.5f - 1.0f, Col(Rgb(0xFFEADB, 0.65f)), 0, 1.6f);
    const ImVec2 glyphSize = Measure(g_fonts.small, glyph);
    PutText(draw, g_fonts.small, ImVec2(std::round(mid.x - glyphSize.x * 0.5f), std::round(mid.y - glyphSize.y * 0.5f)),
            Col(Rgb(0xFFEADB, 0.85f)), glyph);
    PutText(draw, g_fonts.small, ImVec2(min.x + circle + 8.0f, std::round(mid.y - labelSize.y * 0.5f)),
            Col(color::kTextMuted), label);
}

void FormLabel(const char* label, float labelWidth) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetCursorStartPos().x + labelWidth);
    ImGui::SetNextItemWidth(std::min(300.0f, ImGui::GetContentRegionAvail().x));
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
    static const unsigned kColors[] = {0xD8622A, 0xB8402B, 0xC9352F, 0xB87A1D, 0x9C4A32, 0xD4506A};
    unsigned hash = 2166136261u;
    for (unsigned char ch : title) hash = (hash ^ ch) * 16777619u;
    const unsigned rgb = kColors[hash % (sizeof(kColors) / sizeof(kColors[0]))];
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

} // namespace NovaUI::kit
