/**
 * @file UiKit.hpp
 * @brief The launcher's look: colours, fonts, vector icons and the few widgets built on them.
 *
 * Everything the launcher draws that is not plain Dear ImGui lives here, so the pages only
 * say what they show and the style can change in one place. Icons are drawn as vector
 * shapes, so they stay sharp at any size and DPI and need no image files.
 */

#pragma once

#include <imgui.h>

#include <string>

namespace NovaUI::kit {

// --- Colours ---

/// 0xRRGGBB as an ImGui colour with the given alpha.
constexpr ImVec4 Rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

namespace color {
inline constexpr ImVec4 kBackground = Rgb(0xF4F1EA);   ///< window
inline constexpr ImVec4 kSidebar = Rgb(0xECE8DF);      ///< navigation column
inline constexpr ImVec4 kSurface = Rgb(0xFFFFFF);      ///< cards, inputs
inline constexpr ImVec4 kSurfaceHover = Rgb(0xF7F3EA); ///< a surface under the pointer
inline constexpr ImVec4 kBorder = Rgb(0xDDD7CA);
inline constexpr ImVec4 kText = Rgb(0x2A2723);
inline constexpr ImVec4 kTextMuted = Rgb(0x7B746A);
inline constexpr ImVec4 kAccent = Rgb(0xD96E42);
inline constexpr ImVec4 kAccentHover = Rgb(0xE37E52);
inline constexpr ImVec4 kAccentPressed = Rgb(0xBF5A31);
inline constexpr ImVec4 kAccentSoft = Rgb(0xF6E0D4); ///< a selected row or chip
inline constexpr ImVec4 kDanger = Rgb(0xC0392B);
inline constexpr ImVec4 kDangerSoft = Rgb(0xF8E1DE);
inline constexpr ImVec4 kSuccess = Rgb(0x3C8D5A);
inline constexpr ImVec4 kOnAccent = Rgb(0xFFFFFF); ///< text on the accent colour
} // namespace color

/// Dear ImGui's colours and metrics set to the launcher's palette. Call once after creating the context.
void ApplyTheme();

// --- Fonts ---

struct Fonts {
    ImFont* body = nullptr;    ///< default text
    ImFont* small = nullptr;   ///< captions, hints
    ImFont* heading = nullptr; ///< page and card titles
    ImFont* display = nullptr; ///< the wordmark on the home page
};

/// Loads the interface fonts at `pixelScale` times their logical size (the display's pixel
/// density), makes the body font the default and sets the global scale so text lines up with
/// layout in logical units. Falls back to ImGui's built-in font if the embedded one fails.
void LoadFonts(float pixelScale);

/// The loaded fonts (all null before LoadFonts).
const Fonts& GetFonts();

// --- Icons ---

enum class Icon {
    None,
    Home,
    Games,
    Settings,
    Scripts,
    Memory,
    Trophy,
    Simulator,
    Play,
    Stop,
    Rewind,
    Plus,
    Edit,
    Trash,
    Folder,
    Image,
    Search,
    Check,
    Info,
    Warning,
    ChevronRight,
    ChevronDown,
    Close,
    Minimize,
    Maximize,
    Restore,
};

/// Draws an icon centred on `center`, `size` pixels square.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color);

// --- Widgets ---

enum class ButtonStyle {
    Primary,   ///< the main action of a screen: filled with the accent colour
    Secondary, ///< ordinary actions: a light button with a border
    Ghost,     ///< quiet actions and toolbars: no background until hovered
    Danger,    ///< destructive actions: red outline, filled on hover
};

/// A button with an optional icon before the label. `size` of 0 fits the content; a negative
/// width fills the available width. Returns true when clicked.
bool Button(const char* label, ButtonStyle style = ButtonStyle::Secondary, Icon icon = Icon::None,
            ImVec2 size = ImVec2(0, 0));

/// A square button showing only an icon, with a tooltip. `id` must be unique in the window.
bool IconButton(const char* id, Icon icon, ButtonStyle style = ButtonStyle::Ghost, const char* tooltip = nullptr,
                float size = 0.0f);

/// A navigation row: icon, label and a marker on the selected one. Fills the available width.
bool NavItem(Icon icon, const char* label, bool selected);

/// A title-bar button (minimise, maximise, close): flat and as tall as the bar. The close button
/// turns red under the pointer.
bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size);

/// A rounded panel. Everything up to EndCard is drawn inside it. `size` of 0 in a dimension
/// takes the available space; `autoHeight` fits the content instead. Always call EndCard.
bool BeginCard(const char* id, ImVec2 size = ImVec2(0, 0), bool autoHeight = true,
               ImVec2 padding = ImVec2(18.0f, 16.0f));
void EndCard();

/// The title of a page: heading text, an optional muted line under it, then a rule.
void PageTitle(const char* title, const char* subtitle = nullptr);

/// The same without the rule under it, for a title that shares its row with a button.
void PageTitleText(const char* title, const char* subtitle = nullptr);

/// A small muted heading above a group of controls.
void SectionLabel(const char* text);

/// Draws `text` as a keyboard key.
void Keycap(const char* text);

/// A label on the left and the control on the right, aligned across rows. Call before the
/// widget; the widget should be given an ID that starts with "##". `labelWidth` is in pixels.
void FormLabel(const char* label, float labelWidth = 180.0f);

/// Text in the muted style, wrapped to the available width.
void HintText(const char* text);

// --- Game artwork ---

/// Up to four characters standing for a game's title ("SM64", "ZMM"), for artwork that has no picture.
std::string Initials(const std::string& title);

/// A muted colour for a game's placeholder artwork: the same one every time for the same title.
ImU32 PlaceholderColor(const std::string& title);

} // namespace NovaUI::kit
