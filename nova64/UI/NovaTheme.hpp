#pragma once
#include <imgui.h>
#include <imgui_node_editor.h>

namespace NovaUI {

/// Matches the node editor to the launcher's palette (UiKit.hpp).
/// Call with the editor context current.
inline void ApplyNodeEditorTheme() {
    namespace ed = ax::NodeEditor;
    ed::Style& s = ed::GetStyle();
    s.Colors[ed::StyleColor_Bg] = ImColor(12, 11, 36, 235);
    s.Colors[ed::StyleColor_Grid] = ImColor(142, 130, 255, 38);
    s.Colors[ed::StyleColor_NodeBg] = ImColor(26, 24, 66, 245);
    s.Colors[ed::StyleColor_NodeBorder] = ImColor(91, 77, 227, 255);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImColor(142, 130, 255, 255);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImColor(246, 196, 83, 255);
    s.NodeRounding = 12.0f;
    s.NodeBorderWidth = 1.5f;
}

} // namespace NovaUI
