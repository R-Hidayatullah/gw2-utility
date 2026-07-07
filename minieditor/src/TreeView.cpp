#include "TreeView.h"
#include <imgui.h>

void TreeView::draw(const char* childId, const ParsedNodePtr& root, size_t& outSelOffset, size_t& outSelSize) {
    ImGui::BeginChild(childId, ImVec2(0, 0), true);
    if (!root) {
        ImGui::TextDisabled("Load a template and click Run to parse the file.");
    } else {
        drawNode(root, outSelOffset, outSelSize);
    }
    ImGui::EndChild();
}

void TreeView::drawNode(const ParsedNodePtr& node, size_t& outSelOffset, size_t& outSelSize) {
    if (!node) return;

    if (node->isLeaf()) {
        std::string label = node->name + " = " + node->valueString +
                             "   (off 0x" + std::to_string(node->offset) +
                             ", " + std::to_string(node->size) + "B)";
        if (ImGui::Selectable(label.c_str())) {
            outSelOffset = node->offset;
            outSelSize = node->size;
        }
        return;
    }

    std::string header = node->name + "   (off 0x" + std::to_string(node->offset) +
                          ", " + std::to_string(node->size) + "B)";
    bool open = ImGui::TreeNodeEx(node.get(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", header.c_str());
    if (ImGui::IsItemClicked()) {
        outSelOffset = node->offset;
        outSelSize = node->size;
    }
    if (open) {
        for (auto& child : node->children) {
            drawNode(child, outSelOffset, outSelSize);
        }
        ImGui::TreePop();
    }
}
