/**
 * @file LauncherSimulator.cpp
 * @brief Simulator tab: wire up a virtual console, cartridge and TV, then run it.
 */
#include "NovaLauncher.hpp"
#include "UiKit.hpp"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace ed = ax::NodeEditor;

namespace NovaUI {

extern ed::EditorContext* g_NodeEditor;

const char* NovaLauncher::NodeIcon(SimNodeKind k) {
    switch (k) {
    case SimNodeKind::N64Console:    return "[N64]";
    case SimNodeKind::TV:            return "[TV]";
    case SimNodeKind::N64DD:         return "[DD]";
    case SimNodeKind::Controller:    return "[CTL]";
    case SimNodeKind::ExpansionPak:  return "[EXP]";
    case SimNodeKind::Cartridge:     return "[CART]";
    case SimNodeKind::RumblePak:     return "[RMB]";
    case SimNodeKind::ControllerPak: return "[MEM]";
    case SimNodeKind::TransferPak:   return "[T-PAK]";
    }
    return "[?]";
}

const char* NovaLauncher::NodeLabel(SimNodeKind k) {
    switch (k) {
    case SimNodeKind::N64Console:    return "Console";
    case SimNodeKind::TV:            return "TV";
    case SimNodeKind::N64DD:         return "64DD Drive";
    case SimNodeKind::Controller:    return "Controller";
    case SimNodeKind::ExpansionPak:  return "Expansion Pak";
    case SimNodeKind::Cartridge:     return "Cartridge";
    case SimNodeKind::RumblePak:     return "Rumble Pak";
    case SimNodeKind::ControllerPak: return "Controller Pak";
    case SimNodeKind::TransferPak:   return "Transfer Pak";
    }
    return "Accessory";
}

bool NovaLauncher::IsN64DDDisk(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ndd" || ext == ".d64";
}

void NovaLauncher::SpawnNode(SimNodeKind kind, int cartIndex) {
    SimNode n;
    n.id = m_nextNodeId++;
    n.kind = kind;
    // Lay new nodes out in a grid so they don't land on top of each other.
    const int slot = static_cast<int>(m_simNodes.size());
    n.spawnX = 40.0f + static_cast<float>(slot % 3) * 250.0f;
    n.spawnY = 40.0f + static_cast<float>((slot / 3) % 3) * 190.0f;
    if (kind == SimNodeKind::Cartridge && cartIndex >= 0 && cartIndex < static_cast<int>(m_library.size())) {
        std::snprintf(n.cartName, sizeof(n.cartName), "%s", m_library[cartIndex].name.c_str());
        std::snprintf(n.cartPath, sizeof(n.cartPath), "%s", m_library[cartIndex].path.c_str());
    }
    m_simNodes.push_back(n);
}

void NovaLauncher::RenderSimNode(SimNode& node) {
    if (!node.placed) {
        ed::SetNodePosition(ed::NodeId(node.id), ImVec2(node.spawnX, node.spawnY));
        node.placed = true;
    }

    ed::BeginNode(ed::NodeId(node.id));
    ImGui::Text("%s %s", NodeIcon(node.kind),
                node.kind == SimNodeKind::Cartridge ? node.cartName : NodeLabel(node.kind));

    if (node.kind == SimNodeKind::N64Console) {
        for (int i = 1; i <= 4; ++i) {
            ed::BeginPin(ed::PinId(GetPinId(node.id, i)), ed::PinKind::Input);
            ImGui::Text("-> Controller %d", i);
            ed::EndPin();
        }
        ImGui::SameLine(140);
        ed::BeginPin(ed::PinId(GetPinId(node.id, 0)), ed::PinKind::Output);
        ImGui::Text("AV out ->");
        ed::EndPin();
    } else if (node.kind == SimNodeKind::TV) {
        ed::BeginPin(ed::PinId(GetPinId(node.id, 1)), ed::PinKind::Input);
        ImGui::Text("-> AV in");
        ed::EndPin();
        ImGui::Checkbox("Power", &node.isPowered);
    } else if (node.kind == SimNodeKind::Controller) {
        ed::BeginPin(ed::PinId(GetPinId(node.id, 0)), ed::PinKind::Output);
        ImGui::Text("Plug ->");
        ed::EndPin();
    }

    const bool isModule = node.kind == SimNodeKind::Cartridge || node.kind == SimNodeKind::ExpansionPak ||
                          node.kind == SimNodeKind::TransferPak || node.kind == SimNodeKind::RumblePak ||
                          node.kind == SimNodeKind::ControllerPak;
    if (isModule) {
        ImGui::Button("Drag onto the console");
        if (ImGui::BeginDragDropSource()) {
            const int uid = node.id;
            ImGui::SetDragDropPayload("HW_NODE", &uid, sizeof(int));
            ImGui::Text("Moving: %s", NodeLabel(node.kind));
            ImGui::EndDragDropSource();
        }
    }

    if (node.kind == SimNodeKind::N64Console) {
        ImGui::Spacing();
        ImGui::Checkbox("Power", &node.isPowered);
        if (node.hasExpansionPak) {
            kit::Muted("Expansion Pak inserted");
            if (ImGui::Button("Remove Expansion Pak")) {
                node.hasExpansionPak = false;
                node.ramMB = 4;
                SpawnNode(SimNodeKind::ExpansionPak);
            }
        }
        if (node.cartPath[0] != '\0') {
            kit::Muted("Cartridge: %s", node.cartName);
            if (ImGui::Button("Eject cartridge")) node.cartPath[0] = '\0';
        } else {
            kit::Muted("No cartridge");
        }
        if (node.isPowered && node.cartPath[0] != '\0') {
            if (ImGui::Button("Run", ImVec2(-1, 0))) LaunchEmulator(node);
        }
    }
    ed::EndNode();

    if (node.kind != SimNodeKind::N64Console) return;

    ed::Suspend();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HW_NODE")) {
            const int srcId = *static_cast<const int*>(pl->Data);
            auto it =
                std::find_if(m_simNodes.begin(), m_simNodes.end(), [srcId](const SimNode& n) { return n.id == srcId; });
            if (it != m_simNodes.end()) {
                if (it->kind == SimNodeKind::Cartridge) {
                    if (IsN64DDDisk(it->cartPath)) {
                        node.has64DD = true;
                        std::snprintf(node.ddCartName, sizeof(node.ddCartName), "%s", it->cartName);
                        std::snprintf(node.ddCartPath, sizeof(node.ddCartPath), "%s", it->cartPath);
                    } else {
                        std::snprintf(node.cartName, sizeof(node.cartName), "%s", it->cartName);
                        std::snprintf(node.cartPath, sizeof(node.cartPath), "%s", it->cartPath);
                    }
                    m_simNodes.erase(it);
                } else if (it->kind == SimNodeKind::ExpansionPak) {
                    node.hasExpansionPak = true;
                    node.ramMB = 8;
                    m_simNodes.erase(it);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ed::Resume();
}

void NovaLauncher::RenderSimulatorTab() {
    ed::SetCurrentEditor(g_NodeEditor);
    ImGui::BeginChild("SimWorkspace", ImVec2(0, -64));
    ed::Begin("Simulator");

    // Rendering a node can add one (ejecting an Expansion Pak) or erase one
    // (dropping a module onto a console). Reserving up front keeps the reference
    // RenderSimNode holds valid, and indexing avoids iterator invalidation.
    m_simNodes.reserve(m_simNodes.size() * 2 + 1);
    for (size_t i = 0; i < m_simNodes.size(); ++i) RenderSimNode(m_simNodes[i]);
    for (const auto& lnk : m_simLinks) ed::Link(ed::LinkId(lnk.id), ed::PinId(lnk.fromPin), ed::PinId(lnk.toPin));

    if (ed::BeginCreate()) {
        ed::PinId fromPin, toPin;
        if (ed::QueryNewLink(&fromPin, &toPin) && fromPin && toPin) {
            const int a = static_cast<int>(fromPin.Get());
            const int b = static_cast<int>(toPin.Get());
            if (IsInputPin(a) != IsInputPin(b) && ed::AcceptNewItem(ImVec4(0.2f, 0.7f, 0.3f, 1.0f), 2.0f)) {
                SimLink lnk;
                lnk.id = m_nextLinkId++;
                lnk.fromPin = IsInputPin(a) ? b : a;
                lnk.toPin = IsInputPin(a) ? a : b;
                m_simLinks.push_back(lnk);
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::LinkId deleted;
        while (ed::QueryDeletedLink(&deleted)) {
            if (!ed::AcceptDeletedItem()) continue;
            m_simLinks.erase(std::remove_if(m_simLinks.begin(), m_simLinks.end(),
                                            [&](const SimLink& l) { return l.id == static_cast<int>(deleted.Get()); }),
                             m_simLinks.end());
        }
    }
    ed::EndDelete();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::EndChild();

    ImGui::BeginChild("SimTools", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Add:");
    ImGui::SameLine();
    if (ImGui::Button("Console")) SpawnNode(SimNodeKind::N64Console);
    ImGui::SameLine();
    if (ImGui::Button("TV")) SpawnNode(SimNodeKind::TV);
    ImGui::SameLine();
    if (ImGui::Button("Controller")) SpawnNode(SimNodeKind::Controller);
    ImGui::SameLine();
    if (ImGui::Button("Expansion Pak")) SpawnNode(SimNodeKind::ExpansionPak);
    ImGui::SameLine();
    if (ImGui::Button("Cartridge...")) ImGui::OpenPopup("CartridgePicker");

    if (ImGui::BeginPopup("CartridgePicker")) {
        if (m_library.empty()) kit::Muted("Add games in the Games tab first.");
        for (int i = 0; i < static_cast<int>(m_library.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(m_library[i].name.c_str())) SpawnNode(SimNodeKind::Cartridge, i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_library[i].path.c_str());
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }

    // Runnable when a powered TV is wired to a powered console holding a cartridge.
    const SimNode* ready = nullptr;
    for (const auto& tv : m_simNodes) {
        if (tv.kind != SimNodeKind::TV || !tv.isPowered) continue;
        for (const auto& lnk : m_simLinks) {
            if (lnk.toPin != GetPinId(tv.id, 1)) continue;
            const int consoleId = GetNodeIdFromPin(lnk.fromPin);
            for (const auto& console : m_simNodes) {
                if (console.id == consoleId && console.kind == SimNodeKind::N64Console && console.isPowered &&
                    console.cartPath[0] != '\0')
                    ready = &console;
            }
        }
    }

    if (ready) {
        if (ImGui::Button("Run on TV")) LaunchEmulator(*ready);
    } else {
        kit::Muted("To run: connect a powered console with a cartridge to a powered TV.");
    }
    ImGui::EndChild();
}

} // namespace NovaUI
