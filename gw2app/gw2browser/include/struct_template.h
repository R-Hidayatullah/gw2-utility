#ifndef GW2_STRUCT_TEMPLATE_H
#define GW2_STRUCT_TEMPLATE_H

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

// Global registry for the packfile struct template (gw2_packfile.json) that
// gw2model::Extractor needs to walk .modl / AMAT chunks. The template is loaded
// once (auto-detected at startup, or picked via "File -> Load Struct JSON..."),
// stored behind a mutex, and handed to background extraction threads as a
// shared_ptr snapshot so a reload never invalidates an in-flight read.
namespace gw2tpl {

// Returns the current template (nullptr if none loaded yet).
std::shared_ptr<const nlohmann::json> get();

// Parses `path` and installs it as the active template. Returns false (leaving
// the previous template untouched) on read/parse failure; `error` gets why.
bool load_from_file(const std::string& path, std::string& error);

// Tries a list of conventional locations relative to the running exe and cwd
// (templates/gw2_packfile.json, ../gw2mcp/templates/..., etc). Returns true if
// one loaded. Safe to call once at startup.
bool auto_load();

} // namespace gw2tpl

#endif // GW2_STRUCT_TEMPLATE_H
