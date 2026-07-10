#include "TemplateManager.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void TemplateManager::refresh(const std::string& directory) {
    m_directory = directory;
    m_templates.clear();

    std::error_code ec;
    if (!fs::exists(directory, ec)) {
        fs::create_directories(directory, ec);
    }
    for (auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            TemplateInfo info;
            info.name = entry.path().stem().string();
            info.path = entry.path().string();
            m_templates.push_back(info);
        }
    }
}

bool TemplateManager::load(const std::string& path, std::string& outRawText, nlohmann::json& outJson, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open template file: " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    outRawText = ss.str();

    try {
        outJson = nlohmann::json::parse(outRawText);
    } catch (const std::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }
    return true;
}

bool TemplateManager::save(const std::string& path, const std::string& rawText, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Could not write template file: " + path;
        return false;
    }
    out << rawText;
    return true;
}

bool TemplateManager::createNew(const std::string& directory, const std::string& name, std::string& outPath, std::string& error) {
    std::error_code ec;
    fs::create_directories(directory, ec);

    outPath = (fs::path(directory) / (name + ".json")).string();
    if (fs::exists(outPath)) {
        error = "A template named '" + name + ".json' already exists.";
        return false;
    }

    std::ofstream out(outPath);
    if (!out) {
        error = "Could not create file: " + outPath;
        return false;
    }
    out << "{\n"
        << "  \"name\": \"" << name << "\",\n"
        << "  \"endian\": \"little\",\n"
        << "  \"fields\": [\n"
        << "    { \"name\": \"magic\", \"type\": \"char\", \"count\": 4 }\n"
        << "  ]\n"
        << "}\n";
    return true;
}
