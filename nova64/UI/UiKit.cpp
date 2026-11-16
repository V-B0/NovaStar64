#include "UiKit.hpp"

#include <imgui_internal.h> // FindRenderedTextEnd

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>

namespace NovaUI::kit {

namespace {

// Generated from assets/fonts/Roboto-Medium.ttf by CMake: a comma-separated list of byte values.
// Roboto is licensed under the Apache License 2.0 (see assets/fonts/README.md).
const unsigned char kRobotoMedium[] = {
#include "RobotoMedium.inc"
};

Fonts g_fonts;

// Colour with the current style alpha applied, so disabled widgets fade with everything else.
ImU32 Col(const ImVec4& c) { return ImGui::GetColorU32(c); }

constexpr float kPi = 3.14159265358979f;

} // namespace

// --- Theme ---

void ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = color::kText;
    c[ImGuiCol_TextDisabled] = color::kTextMuted;
    c[ImGuiCol_WindowBg] = color::kBackground;
    c[ImGuiCol_ChildBg] = color::kSurface;
    c[ImGuiCol_PopupBg] = color::kSurface;
    c[ImGuiCol_Border] = color::kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = color::kSurface;
    c[ImGuiCol_FrameBgHovered] = color::kSurfaceHover;
    c[ImGuiCol_FrameBgActive] = color::kSurfaceHover;
    c[ImGuiCol_TitleBg] = color::kSidebar;
    c[ImGuiCol_TitleBgActive] = color::kSidebar;
    c[ImGuiCol_TitleBgCollapsed] = color::kSidebar;
    c[ImGuiCol_MenuBarBg] = color::kBackground;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Rgb(0xCFC8B9);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0xB9B1A0);
    c[ImGuiCol_ScrollbarGrabActive] = Rgb(0xA39A88);
    c[ImGuiCol_CheckMark] = color::kAccent;
    c[ImGuiCol_SliderGrab] = color::kAccent;
    c[ImGuiCol_SliderGrabActive] = color::kAccentPressed;
    c[ImGuiCol_Button] = color::kSurface;
    c[ImGuiCol_ButtonHovered] = color::kSurfaceHover;
    c[ImGuiCol_ButtonActive] = color::kSidebar;
    c[ImGuiCol_Header] = color::kAccentSoft;
    c[ImGuiCol_HeaderHovered] = Rgb(0xF0D2C2);
    c[ImGuiCol_HeaderActive] = Rgb(0xE9C2AE);
    c[ImGuiCol_Separator] = color::kBorder;
    c[ImGuiCol_SeparatorHovered] = Rgb(0xC4BCAB);
    c[ImGuiCol_SeparatorActive] = color::kAccent;
    c[ImGuiCol_ResizeGrip] = color::kBorder;
    c[ImGuiCol_ResizeGripHovered] = color::kAccent;
    c[ImGuiCol_ResizeGripActive] = color::kAccentPressed;
    c[ImGuiCol_Tab] = color::kSidebar;
    c[ImGuiCol_TabHovered] = color::kAccentSoft;
    c[ImGuiCol_TabActive] = color::kSurface;
    c[ImGuiCol_TabUnfocused] = color::kSidebar;
    c[ImGuiCol_TabUnfocusedActive] = color::kSurface;
    c[ImGuiCol_TabSelectedOverline] = color::kAccent;
    c[ImGuiCol_TabDimmedSelectedOverline] = color::kAccent;
    c[ImGuiCol_TableHeaderBg] = color::kSidebar;
    c[ImGuiCol_TableBorderStrong] = color::kBorder;
    c[ImGuiCol_TableBorderLight] = Rgb(0xE8E3D8);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Rgb(0x000000, 0.025f);
    c[ImGuiCol_TextSelectedBg] = Rgb(0xD96E42, 0.30f);
    c[ImGuiCol_NavCursor] = color::kAccent;
    c[ImGuiCol_DragDropTarget] = color::kAccent;
    c[ImGuiCol_ModalWindowDimBg] = Rgb(0x1F1C18, 0.40f);

    style.WindowRounding = 0.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(11.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 12.0f;
    style.IndentSpacing = 18.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.45f;
}

// --- Fonts ---

void LoadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    const float scale = pixelScale >= 1.0f ? pixelScale : 1.0f;

    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // the bytes are static
    auto add = [&](float logicalSize) {
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoMedium),
                                              static_cast<int>(sizeof(kRobotoMedium)), logicalSize * scale, &config);
    };

    g_fonts.body = add(15.0f);
    g_fonts.small = add(13.0f);
    g_fonts.heading = add(21.0f);
    g_fonts.display = add(32.0f);

    if (!g_fonts.body || !g_fonts.small || !g_fonts.heading || !g_fonts.display) {
        // Fall back to ImGui's own font for everything rather than draw with a null font.
        io.Fonts->Clear();
        ImFontConfig fallback;
        fallback.SizePixels = 13.0f * scale;
        ImFont* builtin = io.Fonts->AddFontDefault(&fallback);
        g_fonts = Fonts{builtin, builtin, builtin, builtin};
    }

    io.FontDefault = g_fonts.body;
    io.FontGlobalScale = 1.0f / scale;
}

const Fonts& GetFonts() { return g_fonts; }

// --- Icons ---

namespace {

/// Strokes shapes laid out in a unit square onto a square of `size` pixels.
class Painter {
public:
    Painter(ImDrawList* draw, ImVec2 center, float size, ImU32 col) :
        m_draw(draw), m_origin(center.x - size * 0.5f, center.y - size * 0.5f), m_size(size), m_col(col),
        m_width(std::max(1.4f, size * 0.085f)) {}

    ImVec2 P(float x, float y) const { return ImVec2(m_origin.x + x * m_size, m_origin.y + y * m_size); }
    float Len(float u) const { return u * m_size; }

    void Line(float x0, float y0, float x1, float y1) { Path({{x0, y0}, {x1, y1}}, false); }

    /// A stroked path through points given in unit coordinates, with round ends when open.
    void Path(std::initializer_list<ImVec2> unitPoints, bool closed) {
        ImVec2 pts[24];
        int n = 0;
        for (const ImVec2& u : unitPoints) {
            if (n == 24) break;
            pts[n++] = P(u.x, u.y);
        }
        m_draw->AddPolyline(pts, n, m_col, closed ? ImDrawFlags_Closed : ImDrawFlags_None, m_width);
        if (!closed && n >= 2) {
            m_draw->AddCircleFilled(pts[0], m_width * 0.5f, m_col, 8);
            m_draw->AddCircleFilled(pts[n - 1], m_width * 0.5f, m_col, 8);
        }
    }

    void Circle(float cx, float cy, float r) { m_draw->AddCircle(P(cx, cy), Len(r), m_col, 0, m_width); }
    void Dot(float cx, float cy, float r) { m_draw->AddCircleFilled(P(cx, cy), std::max(Len(r), 1.0f), m_col); }

    void Rect(float x0, float y0, float x1, float y1, float rounding) {
        m_draw->AddRect(P(x0, y0), P(x1, y1), m_col, Len(rounding), 0, m_width);
    }
    void FillRect(float x0, float y0, float x1, float y1, float rounding) {
        m_draw->AddRectFilled(P(x0, y0), P(x1, y1), m_col, Len(rounding));
    }
    void FillTriangle(ImVec2 a, ImVec2 b, ImVec2 c) {
        m_draw->AddTriangleFilled(P(a.x, a.y), P(b.x, b.y), P(c.x, c.y), m_col);
    }

    void Curve(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
        m_draw->AddBezierCubic(P(a.x, a.y), P(b.x, b.y), P(c.x, c.y), P(d.x, d.y), m_col, m_width);
    }

    ImDrawList* Draw() const { return m_draw; }
    float Width() const { return m_width; }
    ImVec2 Origin() const { return m_origin; }
    float Size() const { return m_size; }

private:
    ImDrawList* m_draw;
    ImVec2 m_origin;
    float m_size;
    ImU32 m_col;
    float m_width;
};

} // namespace

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 col) {
    Painter p(draw, center, size, col);

    switch (icon) {
    case Icon::None: break;

    case Icon::Home:
        p.Path({{0.14f, 0.50f}, {0.50f, 0.18f}, {0.86f, 0.50f}}, false);
        p.Path({{0.24f, 0.44f}, {0.24f, 0.82f}, {0.76f, 0.82f}, {0.76f, 0.44f}}, false);
        p.Path({{0.42f, 0.82f}, {0.42f, 0.60f}, {0.58f, 0.60f}, {0.58f, 0.82f}}, false);
        break;

    case Icon::Games:
        p.Rect(0.08f, 0.30f, 0.92f, 0.74f, 0.20f);
        p.Line(0.22f, 0.52f, 0.38f, 0.52f);
        p.Line(0.30f, 0.44f, 0.30f, 0.60f);
        p.Dot(0.64f, 0.48f, 0.045f);
        p.Dot(0.76f, 0.57f, 0.045f);
        break;

    case Icon::Settings: {
        constexpr int kTeeth = 8;
        ImVec2 pts[kTeeth * 4];
        int n = 0;
        for (int k = 0; k < kTeeth; ++k) {
            const float a = static_cast<float>(k) * (2.0f * kPi / kTeeth);
            const float spans[4][2] = {{0.30f, -0.24f}, {0.43f, -0.13f}, {0.43f, 0.13f}, {0.30f, 0.24f}};
            for (const auto& s : spans)
                pts[n++] = p.P(0.5f + s[0] * std::cos(a + s[1]), 0.5f + s[0] * std::sin(a + s[1]));
        }
        draw->AddPolyline(pts, n, col, ImDrawFlags_Closed, p.Width());
        p.Circle(0.5f, 0.5f, 0.13f);
        break;
    }

    case Icon::Scripts:
        p.Path({{0.36f, 0.30f}, {0.16f, 0.50f}, {0.36f, 0.70f}}, false);
        p.Path({{0.64f, 0.30f}, {0.84f, 0.50f}, {0.64f, 0.70f}}, false);
        p.Line(0.56f, 0.24f, 0.44f, 0.76f);
        break;

    case Icon::Memory:
        p.Rect(0.28f, 0.28f, 0.72f, 0.72f, 0.05f);
        for (float t : {0.38f, 0.50f, 0.62f}) {
            p.Line(t, 0.28f, t, 0.14f);
            p.Line(t, 0.72f, t, 0.86f);
            p.Line(0.28f, t, 0.14f, t);
            p.Line(0.72f, t, 0.86f, t);
        }
        break;

    case Icon::Trophy:
        p.Path({{0.30f, 0.42f}, {0.30f, 0.18f}, {0.70f, 0.18f}, {0.70f, 0.42f}}, false);
        p.Curve({0.70f, 0.42f}, {0.70f, 0.62f}, {0.30f, 0.62f}, {0.30f, 0.42f});
        p.Path({{0.30f, 0.26f}, {0.14f, 0.26f}, {0.14f, 0.36f}, {0.30f, 0.46f}}, false);
        p.Path({{0.70f, 0.26f}, {0.86f, 0.26f}, {0.86f, 0.36f}, {0.70f, 0.46f}}, false);
        p.Line(0.50f, 0.58f, 0.50f, 0.74f);
        p.Line(0.36f, 0.82f, 0.64f, 0.82f);
        break;

    case Icon::Simulator:
        p.Circle(0.24f, 0.50f, 0.10f);
        p.Circle(0.76f, 0.26f, 0.10f);
        p.Circle(0.76f, 0.74f, 0.10f);
        p.Line(0.33f, 0.46f, 0.67f, 0.30f);
        p.Line(0.33f, 0.54f, 0.67f, 0.70f);
        break;

    case Icon::Play: p.FillTriangle({0.34f, 0.20f}, {0.34f, 0.80f}, {0.82f, 0.50f}); break;

    case Icon::Stop: p.FillRect(0.26f, 0.26f, 0.74f, 0.74f, 0.08f); break;

    case Icon::Rewind:
        p.FillTriangle({0.50f, 0.24f}, {0.50f, 0.76f}, {0.12f, 0.50f});
        p.FillTriangle({0.88f, 0.24f}, {0.88f, 0.76f}, {0.50f, 0.50f});
        break;

    case Icon::Plus:
        p.Line(0.50f, 0.22f, 0.50f, 0.78f);
        p.Line(0.22f, 0.50f, 0.78f, 0.50f);
        break;

    case Icon::Edit:
        p.Path({{0.20f, 0.80f}, {0.26f, 0.58f}, {0.64f, 0.20f}, {0.80f, 0.36f}, {0.42f, 0.74f}}, true);
        p.Line(0.56f, 0.28f, 0.72f, 0.44f);
        break;

    case Icon::Trash:
        p.Line(0.18f, 0.28f, 0.82f, 0.28f);
        p.Path({{0.40f, 0.28f}, {0.40f, 0.17f}, {0.60f, 0.17f}, {0.60f, 0.28f}}, false);
        p.Path({{0.26f, 0.28f}, {0.31f, 0.84f}, {0.69f, 0.84f}, {0.74f, 0.28f}}, false);
        p.Line(0.43f, 0.44f, 0.43f, 0.70f);
        p.Line(0.57f, 0.44f, 0.57f, 0.70f);
        break;

    case Icon::Folder:
        p.Path({{0.12f, 0.24f}, {0.38f, 0.24f}, {0.47f, 0.35f}, {0.88f, 0.35f}, {0.88f, 0.78f}, {0.12f, 0.78f}}, true);
        break;

    case Icon::Image:
        p.Rect(0.14f, 0.22f, 0.86f, 0.78f, 0.08f);
        p.Dot(0.34f, 0.42f, 0.055f);
        p.Path({{0.18f, 0.74f}, {0.42f, 0.50f}, {0.58f, 0.64f}, {0.70f, 0.54f}, {0.84f, 0.70f}}, false);
        break;

    case Icon::Search:
        p.Circle(0.44f, 0.44f, 0.24f);
        p.Line(0.62f, 0.62f, 0.84f, 0.84f);
        break;

    case Icon::Check: p.Path({{0.20f, 0.52f}, {0.42f, 0.72f}, {0.82f, 0.30f}}, false); break;

    case Icon::Info:
        p.Circle(0.50f, 0.50f, 0.36f);
        p.Dot(0.50f, 0.33f, 0.04f);
        p.Line(0.50f, 0.46f, 0.50f, 0.70f);
        break;

    case Icon::Warning:
        p.Path({{0.50f, 0.16f}, {0.88f, 0.82f}, {0.12f, 0.82f}}, true);
        p.Line(0.50f, 0.40f, 0.50f, 0.60f);
        p.Dot(0.50f, 0.71f, 0.04f);
        break;

    case Icon::ChevronRight: p.Path({{0.38f, 0.24f}, {0.64f, 0.50f}, {0.38f, 0.76f}}, false); break;
    case Icon::ChevronDown:  p.Path({{0.24f, 0.38f}, {0.50f, 0.64f}, {0.76f, 0.38f}}, false); break;

    case Icon::Close:
        p.Line(0.26f, 0.26f, 0.74f, 0.74f);
        p.Line(0.74f, 0.26f, 0.26f, 0.74f);
        break;

    case Icon::Minimize: p.Line(0.24f, 0.60f, 0.76f, 0.60f); break;
    case Icon::Maximize: p.Rect(0.26f, 0.26f, 0.74f, 0.74f, 0.0f); break;

    case Icon::Restore:
        p.Rect(0.20f, 0.34f, 0.66f, 0.80f, 0.0f);
        p.Path({{0.34f, 0.34f}, {0.34f, 0.20f}, {0.80f, 0.20f}, {0.80f, 0.66f}, {0.66f, 0.66f}}, false);
        break;
    }
}

// --- Widgets ---

namespace {

struct ButtonColors {
    ImVec4 fill;
    ImVec4 text;
    ImVec4 border;
};

ButtonColors ColorsFor(ButtonStyle style, bool hovered, bool held) {
    switch (style) {
    case ButtonStyle::Primary:
        return {held      ? color::kAccentPressed
                : hovered ? color::kAccentHover
                          : color::kAccent,
                color::kOnAccent, ImVec4(0, 0, 0, 0)};
    case ButtonStyle::Ghost:
        return {held      ? Rgb(0x000000, 0.10f)
                : hovered ? Rgb(0x000000, 0.06f)
                          : ImVec4(0, 0, 0, 0),
                color::kText, ImVec4(0, 0, 0, 0)};
    case ButtonStyle::Danger:
        return {held      ? Rgb(0xC0392B, 0.22f)
                : hovered ? color::kDangerSoft
                          : color::kSurface,
                color::kDanger, held || hovered ? color::kDanger : Rgb(0xC0392B, 0.45f)};
    case ButtonStyle::Secondary:
    default:
        return {held      ? color::kSidebar
                : hovered ? color::kSurfaceHover
                          : color::kSurface,
                color::kText, hovered ? Rgb(0xC9C1B0) : color::kBorder};
    }
}

constexpr float kButtonHeight = 34.0f;
constexpr float kButtonRounding = 7.0f;

void FocusRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemFocused() && io.NavActive && io.NavVisible)
        draw->AddRect(ImVec2(min.x - 2, min.y - 2), ImVec2(max.x + 2, max.y + 2), Col(color::kAccent), rounding + 2.0f,
                      0, 2.0f);
}

} // namespace

bool Button(const char* label, ButtonStyle style, Icon icon, ImVec2 size) {
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd);
    const float iconSize = icon != Icon::None ? std::round(ImGui::GetFontSize() * 1.2f) : 0.0f;
    const float gap = (icon != Icon::None && textSize.x > 0.0f) ? 8.0f : 0.0f;
    const float contentWidth = iconSize + gap + textSize.x;

    float width = size.x;
    if (width == 0.0f) width = contentWidth + 32.0f;
    else if (width < 0.0f) width = std::max(4.0f, ImGui::GetContentRegionAvail().x + width);
    const float height = size.y > 0.0f ? size.y : kButtonHeight;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ButtonColors colors = ColorsFor(style, hovered, held);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (colors.fill.w > 0.0f) draw->AddRectFilled(min, max, Col(colors.fill), kButtonRounding);
    if (colors.border.w > 0.0f) draw->AddRect(min, max, Col(colors.border), kButtonRounding, 0, 1.0f);
    FocusRing(draw, min, max, kButtonRounding);

    const float midY = min.y + height * 0.5f;
    float x = min.x + (width - contentWidth) * 0.5f;
    if (icon != Icon::None) {
        DrawIcon(draw, icon, ImVec2(x + iconSize * 0.5f, midY), iconSize, Col(colors.text));
        x += iconSize + gap;
    }
    if (textSize.x > 0.0f)
        draw->AddText(ImVec2(x, std::round(midY - textSize.y * 0.5f)), Col(colors.text), label, labelEnd);
    return clicked;
}

bool IconButton(const char* id, Icon icon, ButtonStyle style, const char* tooltip, float size) {
    const float side = size > 0.0f ? size : kButtonHeight;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + side, min.y + side);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const ButtonColors colors = ColorsFor(style, hovered, held);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (colors.fill.w > 0.0f) draw->AddRectFilled(min, max, Col(colors.fill), kButtonRounding);
    if (colors.border.w > 0.0f) draw->AddRect(min, max, Col(colors.border), kButtonRounding, 0, 1.0f);
    FocusRing(draw, min, max, kButtonRounding);
    DrawIcon(draw, icon, ImVec2(min.x + side * 0.5f, min.y + side * 0.5f), side * 0.52f, Col(colors.text));

    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool NavItem(Icon icon, const char* label, bool selected) {
    constexpr float kHeight = 38.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + kHeight);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, kHeight));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (selected) draw->AddRectFilled(min, max, Col(color::kAccentSoft), 8.0f);
    else if (hovered) draw->AddRectFilled(min, max, Col(Rgb(0x000000, 0.05f)), 8.0f);
    if (selected)
        draw->AddRectFilled(ImVec2(min.x, min.y + 9), ImVec2(min.x + 3, max.y - 9), Col(color::kAccent), 2.0f);
    FocusRing(draw, min, max, 8.0f);

    const ImVec4& fg = selected ? color::kAccentPressed : color::kText;
    const float midY = min.y + kHeight * 0.5f;
    DrawIcon(draw, icon, ImVec2(min.x + 26.0f, midY), 20.0f, Col(selected ? color::kAccent : color::kTextMuted));
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(min.x + 44.0f, std::round(midY - textSize.y * 0.5f)), Col(fg), label);
    return clicked;
}

bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec4 fg = color::kText;
    if (isClose && hovered) {
        draw->AddRectFilled(min, max, Col(held ? Rgb(0xA82317) : Rgb(0xC42B1C)));
        fg = color::kOnAccent;
    } else if (hovered) {
        draw->AddRectFilled(min, max, Col(Rgb(0x000000, held ? 0.12f : 0.07f)));
    }
    DrawIcon(draw, icon, ImVec2(min.x + size.x * 0.5f, min.y + size.y * 0.5f), 15.0f, Col(fg));
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool BeginCard(const char* id, ImVec2 size, bool autoHeight, ImVec2 padding) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color::kSurface);
    ImGui::PushStyleColor(ImGuiCol_Border, color::kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (autoHeight) flags |= ImGuiChildFlags_AutoResizeY;
    const bool visible = ImGui::BeginChild(id, size, flags, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return visible;
}

void EndCard() { ImGui::EndChild(); }

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
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
}

void SectionLabel(const char* text) {
    ImGui::PushFont(g_fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, color::kTextMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Keycap(const char* text) {
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 size(std::max(textSize.x + 16.0f, 28.0f), textSize.y + 8.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(min.x, min.y + 1), ImVec2(max.x, max.y + 1), Col(Rgb(0xD5CEBF)), 5.0f); // the key's edge
    draw->AddRectFilled(min, max, Col(color::kSurface), 5.0f);
    draw->AddRect(min, max, Col(color::kBorder), 5.0f);
    draw->AddText(ImVec2(min.x + (size.x - textSize.x) * 0.5f, min.y + (size.y - textSize.y) * 0.5f), Col(color::kText),
                  text);
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
    static const unsigned kColors[] = {0x6B7F99, 0x5F8575, 0x9A6B6B, 0x7A6C99, 0xA2814B,
                                       0x4F7F8F, 0x8A7461, 0x697A52, 0x9B6A88, 0x607A9E};
    unsigned hash = 2166136261u;
    for (unsigned char ch : title) hash = (hash ^ ch) * 16777619u;
    const unsigned rgb = kColors[hash % (sizeof(kColors) / sizeof(kColors[0]))];
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

} // namespace NovaUI::kit
