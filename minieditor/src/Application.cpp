#include "Application.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <algorithm>

static uint16_t rd16(const std::vector<uint8_t>& b, size_t o){ return (o + 1 < b.size()) ? (uint16_t)(b[o] | (b[o+1] << 8)) : 0; }
static uint32_t rd32(const std::vector<uint8_t>& b, size_t o){ uint32_t v = 0; for (int i = 0; i < 4; ++i) if (o + i < b.size()) v |= (uint32_t)b[o+i] << (8*i); return v; }

bool Application::init(GLFWwindow* window) {
    m_templateManager.refresh("templates");

    glfwSetWindowUserPointer(window, this);
    glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
        auto* app = static_cast<Application*>(glfwGetWindowUserPointer(w));
        if (app && count > 0) {
            app->onFileDropped(paths[0]);
        }
    });
    return true;
}

void Application::onFileDropped(const std::string& path) {
    openFile(path);
}

void Application::openFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        m_statusMessage = "Failed to open: " + path;
        return;
    }
    m_fileData.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    m_currentFilePath = path;
    m_root.reset();
    m_selOffset = m_selSize = 0;
    m_statusMessage = "Loaded " + path + " (" + std::to_string(m_fileData.size()) + " bytes)";

    // reset GW2 detection for the new file
    m_pfDetected = false; m_pfChunks.clear(); m_ftKeys.clear();
    m_chunkSelected = -1; m_schemaPreview.clear();
    m_gw2Status = (m_fileData.size() >= 2 && m_fileData[0]=='P' && m_fileData[1]=='F')
                    ? "PF signature found — click \"Detect PF\"." : "";

    std::strncpy(m_pathBuffer, path.c_str(), sizeof(m_pathBuffer) - 1);
    m_pathBuffer[sizeof(m_pathBuffer) - 1] = '\0';
}

void Application::runTemplate(const nlohmann::json& templateJson) {
    std::string error;
    if (!m_binaryParser.parse(m_fileData, templateJson, m_root, error)) {
        m_statusMessage = error;
        m_root.reset();
    } else {
        m_statusMessage = "Parsed successfully.";
    }
}

// ===================== GW2 packfile panel =====================
bool Application::loadGw2Registry() {
    if (m_gw2Loaded) return true;
    std::ifstream in("templates/gw2_packfile.json", std::ios::binary);
    if (!in) { m_gw2Status = "templates/gw2_packfile.json not found (run from minieditor/)."; return false; }
    try { in >> m_gw2Registry; }
    catch (const std::exception& e) { m_gw2Status = std::string("registry JSON error: ") + e.what(); return false; }
    m_gw2Loaded = true;
    return true;
}

std::string Application::resolveChunkType(const std::string& container, const std::string& fourcc, int version) {
    if (!m_gw2Loaded) return "";
    std::string vs = std::to_string(version);
    if (m_gw2Registry.contains("fileTypes")) {
        const auto& ft = m_gw2Registry["fileTypes"];
        if (ft.contains(container) && ft[container].contains(fourcc) && ft[container][fourcc].contains(vs))
            return ft[container][fourcc][vs].get<std::string>();
    }
    if (m_gw2Registry.contains("chunks")) {
        const auto& g = m_gw2Registry["chunks"];
        if (g.contains(fourcc) && g[fourcc].contains(vs))
            return g[fourcc][vs].get<std::string>();
    }
    return "";
}

void Application::detectPf() {
    m_pfDetected = false; m_pfChunks.clear(); m_ftKeys.clear(); m_chunkSelected = -1; m_schemaPreview.clear();
    if (m_fileData.size() < 12 || m_fileData[0] != 'P' || m_fileData[1] != 'F') { m_gw2Status = "Not a PF packfile."; return; }
    if (!loadGw2Registry()) return;

    m_pfVersion = rd16(m_fileData, 2);
    m_pfHeaderSize = rd16(m_fileData, 6); if (m_pfHeaderSize < 12) m_pfHeaderSize = 12;
    char c[5] = {0}; for (int i = 0; i < 4; ++i) c[i] = (char)m_fileData[8+i];
    m_pfContainer = c;
    m_pfPtrBits = (m_pfVersion & 4) ? 64 : 32;

    size_t pos = m_pfHeaderSize, size = m_fileData.size();
    while (pos + 16 <= size) {
        char f[5] = {0}; for (int i = 0; i < 4; ++i) f[i] = (char)m_fileData[pos+i];
        uint32_t chunkSize = rd32(m_fileData, pos + 4);
        int version = rd16(m_fileData, pos + 8);
        Gw2ChunkRow row; row.fourcc = f; row.version = version; row.offset = pos; row.size = (size_t)chunkSize + 8;
        m_pfChunks.push_back(row);
        size_t next = pos + 8 + (size_t)chunkSize; if (next <= pos) break; pos = next;
    }

    m_ftKeys.push_back("(auto: " + m_pfContainer + ")");
    if (m_gw2Registry.contains("fileTypes")) {
        std::vector<std::string> ks;
        for (auto it = m_gw2Registry["fileTypes"].begin(); it != m_gw2Registry["fileTypes"].end(); ++it) ks.push_back(it.key());
        std::sort(ks.begin(), ks.end());
        for (auto& k : ks) m_ftKeys.push_back(k);
    }
    m_ftSelected = 0;
    rebuildChunkTabs(m_pfContainer);   // fill per-row strucTab options + defaults
    m_pfDetected = true;
    m_gw2Status = "Detected " + m_pfContainer + " PF v" + std::to_string(m_pfVersion) +
                  " (" + std::to_string(m_pfPtrBits) + "-bit), " + std::to_string(m_pfChunks.size()) + " chunks.";
}

// Populate each row's available strucTabs (distinct schemas for that fourcc) and
// pick a default = the strucTab used by `container`. Labels/types don't depend on
// container; only the default choice does.
void Application::rebuildChunkTabs(const std::string& container) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const nlohmann::json& stall = m_gw2Registry.contains("strucTabs") ? m_gw2Registry["strucTabs"] : kEmpty;
    for (auto& row : m_pfChunks) {
        row.tabLabels.clear(); row.tabTypes.clear(); row.tabChoice = 0; row.type.clear();
        std::string vs = std::to_string(row.version);
        if (stall.contains(row.fourcc)) {
            const auto& list = stall[row.fourcc];
            int def = -1;
            for (int ti = 0; ti < (int)list.size(); ++ti) {
                const auto& e = list[ti];
                std::string used;
                for (const auto& u : e["usedBy"]) {
                    std::string us = u.get<std::string>();
                    if (!used.empty()) used += ",";
                    used += us;
                    if (us == container) def = ti;
                }
                // Label matches the output.txt header identity: "strucTab: 0xADDR, versions: N".
                int nv = e.value("nver", (int)e["versions"].size());
                std::string lbl = "strucTab " + e.value("tab", std::string("?")) +
                                  ", versions:" + std::to_string(nv) + " [" + used + "]";
                bool hasThis = e["versions"].contains(vs);
                if (!hasThis) lbl += " (no v" + vs + ")";
                row.tabLabels.push_back(lbl);
                row.tabTypes.push_back(hasThis ? e["versions"][vs].get<std::string>() : std::string());
            }
            if (def < 0) def = 0;
            row.tabChoice = def;
            row.type = (def < (int)row.tabTypes.size()) ? row.tabTypes[def] : std::string();
        } else {
            row.type = resolveChunkType(container, row.fourcc, row.version);
        }
    }
}

void Application::updateSchemaPreview() {
    m_schemaPreview.clear();
    if (m_chunkSelected < 0 || m_chunkSelected >= (int)m_pfChunks.size()) return;
    const std::string& key = m_pfChunks[m_chunkSelected].type;
    if (key.empty()) { m_schemaPreview = "(no schema for this chunk / version)"; return; }
    if (m_gw2Registry.contains("types") && m_gw2Registry["types"].contains(key)) {
        nlohmann::json one; one[key] = m_gw2Registry["types"][key];
        m_schemaPreview = one.dump(2);
    } else m_schemaPreview = "(type '" + key + "' missing)";
}

void Application::drawGw2Panel() {
    ImGui::BeginDisabled(m_fileData.empty());
    if (ImGui::Button("Detect PF (GW2)")) detectPf();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", m_gw2Status.c_str());

    if (!m_pfDetected) {
        ImGui::TextWrapped("Open a GW2 packfile (drag & drop or File>Open), then click \"Detect PF\" "
                           "to read its header and list its chunks + versions.");
        return;
    }

    ImGui::Separator();
    ImGui::Text("Header:  container=%s   PF v%d   %d-bit pointers   headerSize=%zu",
                m_pfContainer.c_str(), m_pfVersion, m_pfPtrBits, m_pfHeaderSize);

    // Bulk default: pick a file-type -> resets every row's strucTab to that file-type's.
    ImGui::SetNextItemWidth(280);
    if (!m_ftKeys.empty()) {
        std::vector<const char*> items; items.reserve(m_ftKeys.size());
        for (auto& s : m_ftKeys) items.push_back(s.c_str());
        if (ImGui::Combo("Default file type", &m_ftSelected, items.data(), (int)items.size())) {
            std::string container = (m_ftSelected > 0) ? m_ftKeys[m_ftSelected] : m_pfContainer;
            rebuildChunkTabs(container);
            updateSchemaPreview();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(or override per chunk in the strucTab column)");

    if (ImGui::BeginTable("gw2chunks", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 200))) {
        ImGui::TableSetupColumn("chunk",  ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("ver",    ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("size",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("struct");
        ImGui::TableSetupColumn("strucTab (pick)");
        ImGui::TableHeadersRow();

        // Clipper so huge chunk lists (thousands of chunks) don't build/draw
        // every row every frame -- mirrors HexViewer's approach.
        ImGuiListClipper clipper;
        clipper.Begin((int)m_pfChunks.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                auto& r = m_pfChunks[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                // NOTE: previously this used ImGuiSelectableFlags_SpanAllColumns so the
                // whole row was clickable to select it. That made the row-selectable's
                // hit-rect cover the strucTab combo in column 5 too. Even with
                // AllowOverlap, the combo's popup would open and immediately get
                // swallowed by the row's own click handling, so the dropdown looked
                // like it "didn't respond". Restricting the selectable to just this
                // column removes the overlap entirely and the combo works normally.
                ImGui::TableSetColumnIndex(0);
                bool sel = (i == m_chunkSelected);
                if (ImGui::Selectable(r.fourcc.c_str(), sel)) {
                    m_chunkSelected = i; updateSchemaPreview();
                }
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.version);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%zu", r.offset);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%zu", r.size);
                ImGui::TableSetColumnIndex(4);
                if (r.type.empty()) ImGui::TextDisabled("(no schema)");
                else ImGui::TextUnformatted(r.type.c_str());
                ImGui::TableSetColumnIndex(5);
                if (r.tabLabels.size() > 1) {
                    std::vector<const char*> its; its.reserve(r.tabLabels.size());
                    for (auto& s : r.tabLabels) its.push_back(s.c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##tab", &r.tabChoice, its.data(), (int)its.size())) {
                        r.type = (r.tabChoice < (int)r.tabTypes.size()) ? r.tabTypes[r.tabChoice] : std::string();
                        if (i == m_chunkSelected) updateSchemaPreview();
                    }
                } else if (r.tabLabels.size() == 1) {
                    ImGui::TextUnformatted(r.tabLabels[0].c_str());
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Parse file into Tree")) {
        if (loadGw2Registry()) {
            nlohmann::json ov = nlohmann::json::object();
            for (const auto& r : m_pfChunks)
                if (!r.type.empty()) ov[std::to_string(r.offset)] = r.type;
            m_gw2Registry["chunkOverrides"] = ov;   // explicit per-chunk strucTab picks
            m_gw2Registry.erase("containerOverride");
            runTemplate(m_gw2Registry);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(result appears in the Parsed Tree panel)");

    ImGui::Separator();
    ImGui::TextUnformatted("Selected chunk schema:");
    ImGui::BeginChild("##gw2schema", ImVec2(0, 220), true);
    ImGui::PushTextWrapPos(0.0f);   // wrap long lines
    if (m_schemaPreview.empty()) ImGui::TextDisabled("(select a chunk row above)");
    else ImGui::TextUnformatted(m_schemaPreview.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void Application::drawMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::SetNextItemWidth(320);
            ImGui::InputText("##path", m_pathBuffer, sizeof(m_pathBuffer));
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                openFile(m_pathBuffer);
            }
            ImGui::EndMenu();
        }
        if (!m_statusMessage.empty()) {
            ImGui::Text("  |  %s", m_statusMessage.c_str());
        }
        ImGui::EndMenuBar();
    }
}

void Application::drawDockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("DockSpaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    drawMenuBar();

    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

    ImGui::End();
}

void Application::drawUI() {
    drawDockspace();

    ImGui::Begin("Hex Viewer");
    m_hexViewer.draw("##hexchild", m_fileData, m_selOffset, m_selSize);
    ImGui::End();

    ImGui::Begin("Parsed Tree");
    m_treeView.draw("##treechild", m_root, m_selOffset, m_selSize);
    ImGui::End();

    ImGui::Begin("JSON Templates");
    m_jsonEditor.draw("##jsonchild", m_templateManager, [this](const nlohmann::json& j) {
        runTemplate(j);
    });
    ImGui::End();

    ImGui::Begin("GW2 Packfile");
    drawGw2Panel();
    ImGui::End();
}
