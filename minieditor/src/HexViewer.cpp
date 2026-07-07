#include "HexViewer.h"
#include <imgui.h>

void HexViewer::draw(const char* childId, const std::vector<uint8_t>& data, size_t selOffset, size_t selSize) {
    ImGui::BeginChild(childId, ImVec2(0, 0), true);

    if (data.empty()) {
        ImGui::TextDisabled("Drop a binary file onto the window to begin.");
        ImGui::EndChild();
        return;
    }

    const ImVec4 highlightCol(0.95f, 0.65f, 0.15f, 0.35f);
    const size_t rowCount = (data.size() + bytesPerRow - 1) / bytesPerRow;

    ImGuiListClipper clipper;
    clipper.Begin((int)rowCount);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t base = (size_t)row * bytesPerRow;

            ImGui::Text("%08zX", base);

            char ascii[65] = {0};
            for (int col = 0; col < bytesPerRow; ++col) {
                const size_t idx = base + col;
                ImGui::SameLine();

                if (idx < data.size()) {
                    const bool highlighted = selSize > 0 && idx >= selOffset && idx < selOffset + selSize;
                    if (highlighted) {
                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                        ImVec2 p1(p0.x + ImGui::CalcTextSize("FF").x, p0.y + ImGui::GetTextLineHeight());
                        ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(highlightCol));
                    }
                    ImGui::Text("%02X", data[idx]);
                    ascii[col] = (data[idx] >= 32 && data[idx] < 127) ? (char)data[idx] : '.';
                } else {
                    ImGui::TextUnformatted("  ");
                    ascii[col] = ' ';
                }
            }

            ImGui::SameLine();
            ImGui::TextUnformatted(" | ");
            ImGui::SameLine();
            ImGui::TextUnformatted(ascii);
        }
    }

    ImGui::EndChild();
}
