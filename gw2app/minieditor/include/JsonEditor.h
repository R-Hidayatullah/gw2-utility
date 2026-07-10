#pragma once
#include "TemplateManager.h"
#include <functional>
#include <string>
#include <vector>

// Lists template files, provides an inline editor, and fires `onRun` with the
// parsed JSON when the user presses "Run".
class JsonEditor {
public:
    void draw(const char* childId, TemplateManager& mgr, const std::function<void(const nlohmann::json&)>& onRun);

private:
    static constexpr size_t kBufferSize = 65536;

    int m_selectedIndex = -1;
    std::string m_error;
    std::vector<char> m_buffer = std::vector<char>(kBufferSize, 0);
    char m_newTemplateName[64] = "new_template";

    void loadIntoBuffer(const std::string& path, TemplateManager& mgr);
};
