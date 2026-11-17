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

// --- Colours ---

/// 0xRRGGBB as an ImGui colour with the given alpha.
constexpr ImVec4 Rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

namespace color {
inline constexpr ImVec4 kBackground = Rgb(0x080718);
inline constexpr ImVec4 kPanel = Rgb(0x15133A, 0.975f); ///< popups and solid panels
inline constexpr ImVec4 kGlass = Rgb(0xFFFFFF, 0.055f); ///< translucent cards
inline constexpr ImVec4 kGlassHover = Rgb(0xFFFFFF, 0.10f);
inline constexpr ImVec4 kGlassBorder = Rgb(0xFFFFFF, 0.10f);
inline constexpr ImVec4 kText = Rgb(0xF4F3FB);
inline constexpr ImVec4 kTextMuted = Rgb(0xB1AFD0);
inline constexpr ImVec4 kTextDim = Rgb(0x7A789F);
inline constexpr ImVec4 kAccent = Rgb(0x5B4DE3); ///< primary actions and the selected row
inline constexpr ImVec4 kAccentHover = Rgb(0x7064F2);
inline constexpr ImVec4 kAccentPressed = Rgb(0x4637C8);
inline constexpr ImVec4 kAccentSoft = Rgb(0x5B4DE3, 0.30f);
inline constexpr ImVec4 kStar = Rgb(0xF6C453); ///< favourites and ratings
inline constexpr ImVec4 kDanger = Rgb(0xEB5B6E);
inline constexpr ImVec4 kSuccess = Rgb(0x4DDC9F);
inline constexpr ImVec4 kOnAccent = Rgb(0xFFFFFF);
} // namespace color

/// Dear ImGui's colours and metrics set to the launcher's look. Call once after creating the context.
void ApplyTheme();

// --- Fonts and pictures ---

struct Fonts {
    ImFont* body = nullptr;    ///< default text, Inter Regular 15
    ImFont* small = nullptr;   ///< captions and hints, Inter Regular 13
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

/// A title-bar button (minimise, maximise, close). The close button turns red under the pointer.
bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size);

/// A translucent rounded panel. Everything up to EndCard is drawn inside it. `size` of 0 in a
/// dimension takes the available space; `autoHeight` fits the content instead. Always call EndCard.
bool BeginCard(const char* id, ImVec2 size = ImVec2(0, 0), bool autoHeight = true,
               ImVec2 padding = ImVec2(20.0f, 18.0f), float rounding = 18.0f);
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

} // namespace NovaUI::kit
