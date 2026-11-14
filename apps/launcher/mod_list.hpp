#pragma once

#include <imgui.h>

namespace anomaly::launcher {

inline float BeginModsList() {
    // Measure the borderless child in its parent and reserve the scrollbar gutter
    // even while it is hidden. Otherwise a scrollbar narrows the cards, their
    // aspect-ratio covers get shorter, and the scrollbar can disappear next frame.
    const float grid_width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    ImGui::BeginChild("ModsList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    return grid_width;
}

}  // namespace anomaly::launcher
