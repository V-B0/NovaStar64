#pragma once
#include <imgui.h>
#include <imgui_node_editor.h>

namespace NovaUI {

/// Matches the node editor to the launcher's light palette (UiKit.hpp). Its defaults are a
/// near-black canvas and node background, on which dark text disappears.
/// Call with the editor context current.
inline void ApplyNodeEditorTheme() {
    namespace ed = ax::NodeEditor;
    ed::Style& s = ed::GetStyle();
    s.Colors[ed::StyleColor_Bg] = ImColor(229, 225, 214, 255);
    s.Colors[ed::StyleColor_Grid] = ImColor(120, 110, 95, 45);
    s.Colors[ed::StyleColor_NodeBg] = ImColor(250, 248, 242, 245);
    s.Colors[ed::StyleColor_NodeBorder] = ImColor(150, 140, 125, 220);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImColor(217, 110, 66, 255);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImColor(217, 110, 66, 255);
}

} // namespace NovaUI
