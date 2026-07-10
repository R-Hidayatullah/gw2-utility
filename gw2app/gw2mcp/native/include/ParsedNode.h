#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// A single node in the parsed binary tree. Leaf nodes have a value and no
// children; group/struct nodes have children and represent a nested scope.
struct ParsedNode {
    std::string name;
    std::string typeName;
    size_t offset = 0;      // byte offset in the source buffer
    size_t size = 0;        // size in bytes covered by this node (incl. children)
    std::string valueString; // human-readable value (leaves only)
    std::vector<std::shared_ptr<ParsedNode>> children;

    bool isLeaf() const { return children.empty(); }
};

using ParsedNodePtr = std::shared_ptr<ParsedNode>;
