#include "struct_template.h"

#include <fstream>
#include <mutex>
#include <vector>

#include <windows.h>

namespace gw2tpl {

namespace {

std::mutex g_mutex;
std::shared_ptr<const nlohmann::json> g_template;

// Directory the running .exe lives in (with trailing backslash), for locating a
// bundled templates/ folder regardless of the process working directory.
std::string exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, n);
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool file_exists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

std::shared_ptr<const nlohmann::json> get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_template;
}

bool load_from_file(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open " + path;
        return false;
    }
    try {
        auto parsed = std::make_shared<nlohmann::json>();
        in >> *parsed;
        if (!parsed->contains("types")) {
            error = "Template has no 'types' section: " + path;
            return false;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_template = parsed;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Parse error: ") + e.what();
        return false;
    }
}

bool auto_load() {
    const std::string dir = exe_dir();
    const std::vector<std::string> candidates = {
        dir + "gw2_packfile.json",
        dir + "templates\\gw2_packfile.json",
        dir + "..\\gw2mcp\\templates\\gw2_packfile.json",
        dir + "..\\..\\gw2mcp\\templates\\gw2_packfile.json",
        "templates\\gw2_packfile.json",
        "gw2mcp\\templates\\gw2_packfile.json",
        "..\\gw2mcp\\templates\\gw2_packfile.json",
    };
    std::string error;
    for (const std::string& path : candidates) {
        if (file_exists(path) && load_from_file(path, error)) {
            return true;
        }
    }
    return false;
}

} // namespace gw2tpl
