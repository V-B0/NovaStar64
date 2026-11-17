/**
 * @file UiKit.hpp
 * @brief The launcher's look: colours, fonts, icons and the widgets built on them.
 *
 * Everything the launcher draws that is not plain Dear ImGui lives here, so the pages only say what
 * they show and the style can change in one place. Text is Inter, icons are Phosphor (both compiled
 * in, see assets/fonts/), and panels are translucent "glass" over a dark background picture.
 */

#pragma once

#include <imgui.h>

#include <string>

namespace NovaUI::kit {

// --- Spacing ---

/// The spacing scale, in logical pixels. Every gap, margin and padding in the launcher is one of these.
namespace space {
inline constexpr float k1 = 4.0f;   ///< inside a control, between a label and what it describes
inline constexpr float k2 = 8.0f;   ///< between related items, icon to label
inline constexpr float k3 = 12.0f;  ///< between rows
inline constexpr float k4 = 16.0f;  ///< card padding, between cards
inline constexpr float k6 = 24.0f;  ///< between groups of rows
inline constexpr float k8 = 32.0f;  ///< page margin, between sections
inline constexpr float k12 = 48.0f; ///< around an empty state
} // namespace space

// --- Colours ---

/// 0xRRGGBB as an ImGui colour with the given alpha.
constexpr ImVec4 Rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

namespace color {
// A neutral ramp tinted slightly toward the icon's orange. Depth comes from one lightness step per layer and a hairline,
// not from shadows or transparency.
inline constexpr ImVec4 kBackground = Rgb(0x0F0A09); ///< the window, behind the picture
inline constexpr ImVec4 kSidebar = Rgb(0x17100E);    ///< bars at the edges of the window
inline constexpr ImVec4 kCard = Rgb(0x1F1613);       ///< a card at rest
inline constexpr ImVec4 kRaised = Rgb(0x291C18);     ///< inputs, menus and dialogs, one step above a card
inline constexpr ImVec4 kHover = Rgb(0x342420);      ///< a hovered row or button
inline constexpr ImVec4 kHairline = Rgb(0x46322C);   ///< borders and dividers
inline constexpr ImVec4 kText = Rgb(0xF8EEE8);       ///< 15:1 or better on every surface
inline constexpr ImVec4 kTextMuted = Rgb(0xC7B2A8);  ///< secondary text, 7:1 or better
inline constexpr ImVec4 kTextDim =
    Rgb(0xA89588); ///< captions and hints, at least 4.5:1 on every surface but a pressed row
inline constexpr ImVec4 kAccent = Rgb(0xE8702E); ///< the primary action, the selected row, a switch that is on
inline constexpr ImVec4 kAccentHover = Rgb(0xF58A4B);
inline constexpr ImVec4 kAccentPressed = Rgb(0xC85A1E);
inline constexpr ImVec4 kAccentSoft = Rgb(0xE8702E, 0.18f);
inline constexpr ImVec4 kAccentText = Rgb(0xFF9A63); ///< the accent as the colour of text or an icon
inline constexpr ImVec4 kStar = Rgb(0xFFC247);       ///< favourites and warnings
inline constexpr ImVec4 kDanger = Rgb(0xFF5A6A);
inline constexpr ImVec4 kSuccess = Rgb(0x55D68A);
inline constexpr ImVec4 kOnAccent = Rgb(0x120905); ///< text and icons on an accent fill: dark, 6:1 on the accent
inline constexpr ImVec4 kKnob = Rgb(0xF8EEE8);     ///< the knob of a switch or slider
} // namespace color

/// The two corner radii: small controls, and cards and pictures.
namespace radius {
inline constexpr float kControl = 8.0f;
inline constexpr float kCard = 14.0f;
} // namespace radius

/// Dear ImGui's colours and metrics set to the launcher's look. Call once after creating the context.
void ApplyTheme();

// --- Fonts and pictures ---

struct Fonts {
    ImFont* body = nullptr;    ///< default text, Inter Regular 15
    ImFont* small = nullptr;   ///< captions and hints, Inter Regular 13
    ImFont* caption = nullptr; ///< section labels in capitals, Inter SemiBold 12
    ImFont* label = nullptr;   ///< buttons and labels, Inter SemiBold 15
    ImFont* heading = nullptr; ///< section titles, Inter Bold 24
    ImFont* title = nullptr;   ///< card and page titles, Inter Bold 32
    ImFont* display = nullptr; ///< the wordmark, Inter Bold 44
};

/// Loads the fonts and the pictures (needs the GL context) for a display with the given pixel density (1 on an
/// ordinary screen, 2 on Retina). Makes the body font the default and sets the global font scale so text lines
/// up with layout in logical pixels. Call before the first frame.
void LoadFonts(float pixelScale);

/// The loaded fonts (all null before LoadFonts).
const Fonts& GetFonts();

/// Size of `text` in `font`, in logical pixels.
ImVec2 Measure(ImFont* font, const char* text);

/// Draws `text` at `pos` (logical pixels, top left) in `font` through the draw list, without moving the cursor.
void PutText(ImDrawList* draw, ImFont* font, ImVec2 pos, ImU32 color, const char* text, const char* end = nullptr);

/// `text` shortened with "..." so that it is at most `maxWidth` wide in `font`.
std::string Ellipsize(ImFont* font, const std::string& text, float maxWidth);

/// `text` word-wrapped to `width` and cut after `maxLines` lines (with "..." when something was left out), with
/// the line breaks written as '\n'.
std::string ClampLines(ImFont* font, const std::string& text, float width, int maxLines);

// --- Icons ---

enum class Icon {
    None,
    Home,
    Games,
    Recent,
    Star,
    Settings,
    Info,
    Play,
    Stop,
    Pause,
    Rewind,
    FastForward,
    Plus,
    Edit,
    Trash,
    Folder,
    Image,
    Search,
    Check,
    Warning,
    ChevronRight,
    ChevronDown,
    ArrowRight,
    ArrowLeft,
    Close,
    Minimize,
    Maximize,
    Restore,
    Scripts,
    Memory,
    Trophy,
    Simulator,
    Heart,
    More,
    Calendar,
    Publisher,
    Display,
    Sound,
    Keyboard,
    Lightning,
    Tag,
    Link,
    Download,
};

/// Draws an icon centred on `center`, `size` logical pixels tall. Filled by default.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color, bool filled = true);

// --- Shapes ---

/// A rounded rectangle filled with a soft top-to-bottom shine, for primary buttons and the selected row.
void DrawGradientFill(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 base, float rounding, float shine = 0.22f);

/// The window background: the picture, scaled to cover [min, max].
void DrawBackground(ImDrawList* draw, ImVec2 min, ImVec2 max);

/// Darkens the lower part of [min, max] so text stays readable over a picture.
void DrawShade(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float strength = 0.75f);

/// A GL texture drawn to fill [min, max], cropped to fit rather than stretched, with rounded corners.
void DrawImageCover(ImDrawList* draw, unsigned texture, int texWidth, int texHeight, ImVec2 min, ImVec2 max,
                    float rounding, ImU32 tint = IM_COL32_WHITE);

/// A ring around [min, max] with a soft glow, to mark the selected tile.
void DrawSelectionRing(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding);

// --- Widgets ---

/// An invisible button that keyboard and gamepad navigation can reach (ImGui skips InvisibleButton by default).
/// ImGui's own focus ring is not drawn; the widget built on it draws its own, see FocusRing in UiKit.cpp.
bool NavButton(const char* id, ImVec2 size);

enum class ButtonStyle {
    Primary,   ///< the main action of a screen: filled with the accent colour
    Secondary, ///< ordinary actions: translucent with a border
    Ghost,     ///< quiet actions and toolbars: no background until hovered
    Danger,    ///< destructive actions: red outline, filled on hover
};

/// A button with an optional icon before the label. `size` of 0 fits the content; a negative
/// width fills the available width. Returns true when clicked.
bool Button(const char* label, ButtonStyle style = ButtonStyle::Secondary, Icon icon = Icon::None,
            ImVec2 size = ImVec2(0, 0), bool iconAfterLabel = false);

/// A square button showing only an icon, with a tooltip. `id` must be unique in the window.
bool IconButton(const char* id, Icon icon, ButtonStyle style = ButtonStyle::Ghost, const char* tooltip = nullptr,
                float size = 0.0f, bool filled = true);

/// A navigation row: icon, label, and the accent colour behind the selected one. Fills the available width.
/// `compact` makes it shorter, for the secondary group.
bool NavItem(Icon icon, const char* label, bool selected, bool compact = false);

/// An on/off switch. Returns true when it was flipped. `id` must be unique in the window.
bool Toggle(const char* id, bool* value);

/// A row of labelled choices with one selected, for two or three short options. A width of 0 fits the labels.
/// Returns true when the selection changed.
bool Segmented(const char* id, const char* const* labels, int count, int* index, float width = 0.0f);

/// A slider with a track, a knob and its value written at the right end. `width` includes the value.
bool Slider(const char* id, int* value, int min, int max, const char* format, float width);

/// The width `Segmented` takes for these labels when it is given no width.
float SegmentedWidth(const char* const* labels, int count);

/// Text tabs with an underline under the selected one. Returns true when the selection changed.
bool TabStrip(const char* id, const char* const* labels, int count, int* index);

/// One row of a settings page: `title` and a line of `help` on the left, and whatever `control` draws on the
/// right, vertically centred, `controlWidth` wide. `control` returns true when the value changed, which is also
/// what this returns. Rows inside a card from BeginSettings are separated by hairlines.
template <class Control>
bool SettingRow(const char* title, const char* help, float controlWidth, float controlHeight, Control&& control,
                const char* badge = nullptr);

/// Starts a list of rows outside BeginSettings (the first row gets no divider above it).
void RowsReset();

/// A compact row of a settings card showing a fact: `key` on the left, `value` on the right.
void InfoRow(const char* key, const char* value);

/// A column no wider than `maxWidth`, left aligned, for pages made of rows: wide windows keep their text and
/// controls close together. Always call EndColumn.
void BeginColumn(float maxWidth);
void EndColumn();

/// A card holding rows. `heading` is shown above it. Always call EndSettings.
void BeginSettings(const char* id, const char* heading);
void EndSettings();

/// A title-bar button (minimise, maximise, close). The close button turns red under the pointer.
bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size);

/// A translucent rounded panel. Everything up to EndCard is drawn inside it. `size` of 0 in a
/// dimension takes the available space; `autoHeight` fits the content instead. Always call EndCard.
bool BeginCard(const char* id, ImVec2 size = ImVec2(0, 0), bool autoHeight = true,
               ImVec2 padding = ImVec2(space::k6, space::k4), float rounding = radius::kCard);
void EndCard();

/// The title of a page: heading text, an optional muted line under it, then some space.
void PageTitle(const char* title, const char* subtitle = nullptr);

/// The same without the space under it, for a title that shares its row with a button.
void PageTitleText(const char* title, const char* subtitle = nullptr);

/// A heading above a group of tiles or controls.
void SectionTitle(const char* text);

/// A small muted heading above a group of controls.
void SectionLabel(const char* text);

/// Draws `text` as a keyboard key.
void Keycap(const char* text);

/// A small rounded tag, with an optional icon before the text.
void Chip(const char* text, Icon icon = Icon::None, ImU32 iconColor = 0);

/// A controller button hint: a circle holding `glyph` (or an icon), then `label`.
void ControllerHint(const char* glyph, const char* label);

/// A label on the left and the control on the right, aligned across rows. Call before the
/// widget; the widget should be given an ID that starts with "##". `labelWidth` is in pixels.
void FormLabel(const char* label, float labelWidth = 180.0f);

/// Text in the muted style, wrapped to the available width.
void HintText(const char* text);

/// The same with printf formatting.
void Muted(const char* fmt, ...) IM_FMTARGS(1);

// --- Game artwork ---

/// Up to four characters standing for a game's title ("SM64", "ZMM"), for artwork that has no picture.
std::string Initials(const std::string& title);

/// A colour for a game's placeholder artwork: the same one every time for the same title.
ImU32 PlaceholderColor(const std::string& title);

namespace rows {
/// Starts a settings row: draws its text and divider, and returns the top left of the row's control area.
ImVec2 BeginRow(const char* title, const char* help, float controlWidth, float controlHeight, float* rowHeight,
                const char* badge);
/// Moves the cursor below the row.
void EndRow();
} // namespace rows

template <class Control>
bool SettingRow(const char* title, const char* help, float controlWidth, float controlHeight, Control&& control,
                const char* badge) {
    float rowHeight = 0.0f;
    const ImVec2 at = rows::BeginRow(title, help, controlWidth, controlHeight, &rowHeight, badge);
    ImGui::PushID(title);
    ImGui::SetCursorScreenPos(at);
    const bool changed = control();
    ImGui::PopID();
    rows::EndRow();
    return changed;
}

} // namespace NovaUI::kit
