#pragma once
#include "ParsedNode.h"
#include <unordered_map>
#include <vector>

// Renders the parsed node tree as a striped table (Field | Value | Offset |
// Size), with group/struct nodes acting as collapsible "category" rows.
// Reports the currently selected byte range (out params) so the HexViewer
// can highlight it.
//
// Rendering is virtualized: the tree is flattened (skipping collapsed
// subtrees) into a row list, then only the rows visible in the scroll
// region are actually formatted/drawn, via ImGuiListClipper -- the same
// approach HexViewer uses for the byte grid. This keeps parses with
// thousands of nodes (e.g. big GW2 array fields) smooth, since building
// a node's label string no longer happens for off-screen rows every frame.
//
// Rows are intentionally NOT indented per depth -- everything stays flush
// left so deeply nested categories don't creep the layout rightward; only
// the row's own bracket marker ([+]/[-]) and text color distinguish a
// category row from a leaf field.
class TreeView {
public:
    void draw(const char* childId, const ParsedNodePtr& root, size_t& outSelOffset, size_t& outSelSize);

private:
    struct FlatRow {
        const ParsedNode* node;
        int depth; // kept for potential future use; not used for indentation
    };

    // Explicit open/closed state per node (identity via raw pointer). Not
    // using ImGui's built-in TreeNode open-state storage because that
    // requires actually calling TreeNodeEx for every node (expensive at
    // scale); we need to know open/closed *before* deciding what to draw.
    std::unordered_map<const ParsedNode*, bool> m_openState;

    bool isOpen(const ParsedNode* node) const;
    void flatten(const ParsedNodePtr& node, int depth, std::vector<FlatRow>& out) const;
};
