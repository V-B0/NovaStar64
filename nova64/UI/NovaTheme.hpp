#pragma once
#include <imgui.h>
#include <imgui_node_editor.h>

namespace NovaUI {

/// Matches the node editor to the launcher's palette (UiKit.hpp).
/// Call with the editor context current.
inline void ApplyNodeEditorTheme() {
    namespace ed = ax::NodeEditor;
    ed::Style& s = ed::GetStyle();
    s.Colors[ed::StyleColor_Bg] = ImColor(27, 23, 68, 255);
    s.Colors[ed::StyleColor_Grid] = ImColor(75, 59, 181, 110);
    s.Colors[ed::StyleColor_NodeBg] = ImColor(11, 11, 30, 245);
    s.Colors[ed::StyleColor_NodeBorder] = ImColor(255, 122, 26, 255);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImColor(246, 240, 220, 255);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImColor(246, 240, 220, 255);
}

} // namespace NovaUI
