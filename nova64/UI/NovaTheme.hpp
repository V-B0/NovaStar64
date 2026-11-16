#pragma once
#include <imgui.h>
#include <imgui_node_editor.h>

namespace NovaUI {

inline void ApplyWin95Theme() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Modern/Win95 Hybrid colors (beige/orange palette)
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);     // Dark text
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.96f, 0.95f, 0.91f, 1.00f); // Warm beige background
    colors[ImGuiCol_ChildBg] = ImVec4(0.93f, 0.92f, 0.88f, 1.00f);  // Slightly darker for sidebar
    colors[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.95f, 0.91f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.80f, 0.78f, 0.74f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f); // White inputs
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f); // Orange title
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.70f, 0.35f, 0.20f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.96f, 0.95f, 0.91f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.90f, 0.89f, 0.85f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.75f, 0.73f, 0.69f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.65f, 0.63f, 0.59f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.53f, 0.49f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f); // Orange checkmark
    colors[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.70f, 0.35f, 0.20f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f); // Orange buttons
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.48f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.35f, 0.20f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.85f, 0.43f, 0.26f, 0.50f); // Selection
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.85f, 0.43f, 0.26f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.80f, 0.78f, 0.74f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.70f, 0.68f, 0.64f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.60f, 0.58f, 0.54f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.80f, 0.78f, 0.74f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.75f, 0.35f, 0.20f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.90f, 0.89f, 0.85f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.85f, 0.43f, 0.26f, 0.80f);
    colors[ImGuiCol_TabActive] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.90f, 0.89f, 0.85f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.80f, 0.78f, 0.74f, 1.00f);
    // Anything not set here keeps ImGui's dark-theme value, which is unreadable
    // against dark text (table headers were dark grey on dark grey).
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.88f, 0.86f, 0.81f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.80f, 0.78f, 0.74f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.87f, 0.85f, 0.81f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.00f, 0.00f, 0.00f, 0.03f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.85f, 0.43f, 0.26f, 0.35f);
    colors[ImGuiCol_NavCursor] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.85f, 0.43f, 0.26f, 1.00f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.18f, 0.15f, 0.35f);

    // Rounded/Square aesthetics
    style.WindowRounding = 8.0f; // Softer corners for modern look
    style.ChildRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
}

/// Matches the node editor to the light theme. Its defaults are a near-black
/// canvas and node background, on which the theme's dark text disappears.
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
