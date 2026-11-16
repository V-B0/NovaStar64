/**
 * @file UiKit.hpp
 * @brief The launcher's look: a pixel-art interface drawn from five colours, a 5x7 bitmap font and 16x16 sprites.
 *
 * The art lives in assets/ui/*.aseprite (edit it in Aseprite, then run scripts/ui-art/export.sh); the
 * exported PNG sheets are compiled into the program. Everything is drawn by whole numbers of "art
 * pixels" (Px() display pixels each) and never smoothed, so it stays crisp at any display scale.
 * Everything the launcher draws that is not plain Dear ImGui lives here, so the pages only say what they show.
 */

#pragma once

#include <imgui.h>

#include <string>

namespace NovaUI::kit {

// --- Scale ---

/// Display pixels per art pixel. All layout is in multiples of this.
int Px();

/// `n` art pixels, in display pixels.
inline float Px(float n) { return n * static_cast<float>(Px()); }

// --- Colours ---

/// 0xRRGGBB as an ImGui colour with the given alpha.
constexpr ImVec4 Rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

namespace color {
// The five colours of the palette.
inline constexpr ImVec4 kBlack = Rgb(0x040404);
inline constexpr ImVec4 kBrown = Rgb(0x6A2B09);
inline constexpr ImVec4 kOrange = Rgb(0xC5620B);
inline constexpr ImVec4 kPeach = Rgb(0xFCB861);
inline constexpr ImVec4 kGrey = Rgb(0x6F7781);

// What they are used for.
inline constexpr ImVec4 kBackground = kBlack;
inline constexpr ImVec4 kPanel = Rgb(0x2D1406); ///< the dark brown inside panels: black with a little brown
inline constexpr ImVec4 kSidebar = kPanel;
inline constexpr ImVec4 kSurface = kPanel;
inline constexpr ImVec4 kSurfaceHover = kBrown;
inline constexpr ImVec4 kBorder = kBrown;
inline constexpr ImVec4 kText = kPeach;
inline constexpr ImVec4 kTextMuted = kOrange;
inline constexpr ImVec4 kTextDisabled = kGrey;
inline constexpr ImVec4 kAccent = kOrange;
inline constexpr ImVec4 kAccentHover = kPeach;
inline constexpr ImVec4 kAccentPressed = kPeach;
inline constexpr ImVec4 kAccentSoft = kBrown; ///< a selected row or chip
inline constexpr ImVec4 kDanger = kOrange;
inline constexpr ImVec4 kOnAccent = kBlack;   ///< text on the accent colour
} // namespace color

/// Dear ImGui's colours and metrics set to the launcher's palette. Call once after creating the
/// context and after the pixel scale is known (see LoadFonts).
void ApplyTheme();

// --- Fonts and art ---

struct Fonts {
    ImFont* body = nullptr;    ///< default text, 5x7 pixels at 2 art pixels each
    ImFont* small = nullptr;   ///< captions and hints (the same size: a pixel font does not shrink)
    ImFont* heading = nullptr; ///< page and card titles
    ImFont* display = nullptr; ///< the wordmark on the home page
};

/// Loads the pixel art (needs the GL context) and builds the fonts from it for a display with the given
/// pixel density (1 on an ordinary screen, 2 on Retina). Makes the body font the default and sets the
/// global font scale so text lines up with layout in display pixels. Call before the first frame.
void LoadFonts(float pixelScale);

/// Call once per frame after the GL backend's NewFrame. The first time, it switches the font texture
/// to unsmoothed sampling.
void OnNewFrame();

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

/// Draws an icon sprite centred on `center`, `size` pixels square (use 16 * Px() for whole-pixel scaling).
/// The sprite keeps its own colours; `tint` only fades it (for example with the alpha of a disabled widget).
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 tint = IM_COL32_WHITE);

// --- Frames ---

/// Which of the frame sprites to draw: the colour of the border.
enum class Frame { Brown, Orange, Peach, Grey };

/// Draws a stepped-corner frame around [min, max], filled with `fill` (the fill can be transparent).
/// Corners are cut in two steps, as in the rest of the interface.
void DrawFrame(ImDrawList* draw, ImVec2 min, ImVec2 max, Frame frame, ImU32 fill);

/// A strip of dither along the bottom edge of [min, max], denser towards the bottom.
void DrawDitherBand(ImDrawList* draw, ImVec2 min, ImVec2 max, int rows);

// --- Widgets ---

enum class ButtonStyle {
    Primary,   ///< the main action of a screen: filled with the accent colour
    Secondary, ///< ordinary actions: a dark button with a border
    Ghost,     ///< quiet actions and toolbars: no frame until hovered
    Danger,    ///< destructive actions: outlined, filled on hover
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
/// is filled when the pointer is over it.
bool WindowControl(const char* id, Icon icon, bool isClose, const char* tooltip, ImVec2 size);

/// A framed panel. Everything up to EndCard is drawn inside it. `size` of 0 in a dimension
/// takes the available space; `autoHeight` fits the content instead. Always call EndCard.
bool BeginCard(const char* id, ImVec2 size = ImVec2(0, 0), bool autoHeight = true,
               ImVec2 padding = ImVec2(16.0f, 14.0f), Frame frame = Frame::Brown);
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

/// The same with printf formatting.
void Muted(const char* fmt, ...) IM_FMTARGS(1);

// --- Game artwork ---

/// Up to four characters standing for a game's title ("SM64", "ZMM"), for artwork that has no picture.
std::string Initials(const std::string& title);

/// A muted colour for a game's placeholder artwork: the same one every time for the same title.
ImU32 PlaceholderColor(const std::string& title);

} // namespace NovaUI::kit
