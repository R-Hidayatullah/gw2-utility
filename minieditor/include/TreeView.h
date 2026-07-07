#pragma once
#include "ParsedNode.h"

// Renders the parsed node tree with ImGui and reports the currently selected
// byte range (out params) so the HexViewer can highlight it.
class TreeView {
public:
    void draw(const char* childId, const ParsedNodePtr& root, size_t& outSelOffset, size_t& outSelSize);

private:
    void drawNode(const ParsedNodePtr& node, size_t& outSelOffset, size_t& outSelSize);
};
