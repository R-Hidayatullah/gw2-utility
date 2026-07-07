#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct TemplateInfo {
    std::string name; // filename without extension, shown in the UI list
    std::string path; // full filesystem path
};

// Discovers *.json template files in a directory, loads their raw text +
// parsed JSON, and writes edits back to disk. Used by JsonEditor.
class TemplateManager {
public:
    void refresh(const std::string& directory);

    const std::vector<TemplateInfo>& templates() const { return m_templates; }

    bool load(const std::string& path, std::string& outRawText, nlohmann::json& outJson, std::string& error);
    bool save(const std::string& path, const std::string& rawText, std::string& error);
    bool createNew(const std::string& directory, const std::string& name, std::string& outPath, std::string& error);

private:
    std::string m_directory;
    std::vector<TemplateInfo> m_templates;
};
