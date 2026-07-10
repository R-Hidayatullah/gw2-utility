#include "TreeView.h"
#include <imgui.h>
#include <string>

bool TreeView::isOpen(const ParsedNode* node) const {
    auto it = m_openState.find(node);
    return it != m_openState.end() && it->second;
}

// Walks the tree but only descends into a group's children if that group is
// currently open, so a collapsed subtree with thousands of nodes costs O(1)
// here instead of O(subtree size).
void TreeView::flatten(const ParsedNodePtr& node, int depth, std::vector<FlatRow>& out) const {
    if (!node) return;
    out.push_back({node.get(), depth});
    if (!node->isLeaf() && isOpen(node.get())) {
        for (auto& child : node->children) {
            flatten(child, depth + 1, out);
        }
    }
}

void TreeView::draw(const char* childId, const ParsedNodePtr& root, size_t& outSelOffset, size_t& outSelSize) {
    ImGui::BeginChild(childId, ImVec2(0, 0), true);

    if (!root) {
        ImGui::TextDisabled("Load a template and click Run to parse the file.");
        ImGui::EndChild();
        return;
    }

    // Root starts expanded the first time we see it.
    if (m_openState.find(root.get()) == m_openState.end()) {
        m_openState[root.get()] = true;
    }

    std::vector<FlatRow> rows;
    rows.reserve(256);
    flatten(root, 0, rows);

    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##parsedTreeTable", 4, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Field",  ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        // Only rows inside the visible scroll range get their label string
        // built and drawn -- keeps thousands of parsed fields scrolling
        // smoothly instead of formatting every node every single frame.
        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const FlatRow& row = rows[i];
                const ParsedNode* node = row.node;
                const bool isCategory = !node->isLeaf();
                const bool open = isCategory && isOpen(node);

                ImGui::TableNextRow();
                ImGui::PushID(node);

                // Category ("group"/struct) rows get a tinted background band
                // on top of the normal zebra striping, so they read as
                // section headers rather than just another field.
                if (isCategory) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                        ImGui::GetColorU32(ImVec4(0.30f, 0.36f, 0.46f, 0.55f)));
                }

                ImGui::TableSetColumnIndex(0);
                // No depth-based indent -- everything stays flush left, only
                // the marker + color say "this is a category".
                std::string nameLabel = isCategory ? (open ? "[-] " : "[+] ") + node->name
                                                    : node->name;
                if (isCategory) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.35f, 1.0f));
                bool clicked = ImGui::Selectable(nameLabel.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (isCategory) ImGui::PopStyleColor();
                if (clicked) {
                    if (isCategory) m_openState[node] = !open;
                    outSelOffset = node->offset;
                    outSelSize = node->size;
                }

                ImGui::TableSetColumnIndex(1);
                if (isCategory) {
                    ImGui::TextDisabled("(%d field%s)", (int)node->children.size(),
                                         node->children.size() == 1 ? "" : "s");
                } else {
                    ImGui::TextUnformatted(node->valueString.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("0x%zX", node->offset);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%zu B", node->size);

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
}
