#include "JsonEditor.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>

void JsonEditor::loadIntoBuffer(const std::string& path, TemplateManager& mgr) {
    std::string raw;
    nlohmann::json j;
    if (!mgr.load(path, raw, j, m_error)) {
        return;
    }
    std::fill(m_buffer.begin(), m_buffer.end(), 0);
    if (raw.size() >= kBufferSize) {
        // Large registries (e.g. gw2_packfile.json) don't belong in this text
        // editor — they'd be truncated. Point the user at the GW2 panel instead.
        std::string note = "// Registry too large to edit here (" + std::to_string(raw.size() / 1024) +
                           " KB).\n// Use the \"GW2 Packfile\" panel: Detect PF -> pick file type -> Parse.\n";
        std::memcpy(m_buffer.data(), note.data(), std::min(note.size(), kBufferSize - 1));
        m_error.clear();
        return;
    }
    std::memcpy(m_buffer.data(), raw.data(), raw.size());
}

void JsonEditor::draw(const char* childId, TemplateManager& mgr, const std::function<void(const nlohmann::json&)>& onRun) {
    ImGui::BeginChild(childId, ImVec2(0, 0), true);

    if (ImGui::Button("Refresh")) {
        mgr.refresh("templates");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##newname", m_newTemplateName, sizeof(m_newTemplateName));
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        std::string outPath, err;
        if (mgr.createNew("templates", m_newTemplateName, outPath, err)) {
            mgr.refresh("templates");
        } else {
            m_error = err;
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Templates:");
    ImGui::BeginChild("##templateList", ImVec2(0, 100), true);
    const auto& templates = mgr.templates();
    for (int i = 0; i < (int)templates.size(); ++i) {
        bool selected = (i == m_selectedIndex);
        if (ImGui::Selectable(templates[i].name.c_str(), selected)) {
            m_selectedIndex = i;
            loadIntoBuffer(templates[i].path, mgr);
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextUnformatted("Editor:");
    ImGui::InputTextMultiline("##jsoneditor", m_buffer.data(), m_buffer.size(), ImVec2(-1, 220));

    bool hasSelection = m_selectedIndex >= 0 && m_selectedIndex < (int)templates.size();
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("Save")) {
        std::string err;
        if (!mgr.save(templates[m_selectedIndex].path, std::string(m_buffer.data()), err)) {
            m_error = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Run")) {
        try {
            nlohmann::json j = nlohmann::json::parse(std::string(m_buffer.data()));
            m_error.clear();
            onRun(j);
        } catch (const std::exception& e) {
            m_error = std::string("JSON parse error: ") + e.what();
        }
    }
    ImGui::EndDisabled();

    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_error.c_str());
    }

    ImGui::EndChild();
}
