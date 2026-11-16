#pragma once
#include <imgui.h>
#include <imgui_node_editor.h>

namespace NovaUI {

/// Matches the node editor to the launcher's palette (UiKit.hpp).
/// Call with the editor context current.
inline void ApplyNodeEditorTheme() {
    namespace ed = ax::NodeEditor;
    ed::Style& s = ed::GetStyle();
    s.Colors[ed::StyleColor_Bg] = ImColor(45, 20, 6, 255);
    s.Colors[ed::StyleColor_Grid] = ImColor(106, 43, 9, 110);
    s.Colors[ed::StyleColor_NodeBg] = ImColor(4, 4, 4, 245);
    s.Colors[ed::StyleColor_NodeBorder] = ImColor(197, 98, 11, 255);
    s.Colors[ed::StyleColor_HovNodeBorder] = ImColor(252, 184, 97, 255);
    s.Colors[ed::StyleColor_SelNodeBorder] = ImColor(252, 184, 97, 255);
}

} // namespace NovaUI
