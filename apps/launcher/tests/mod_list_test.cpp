#include "../mod_list.hpp"

#include <imgui_internal.h>

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Frame {
    float width{};
    bool scrollbar{};
    float scroll_max{};
    float viewport_height{};
    float content_height{};
    float content_bottom{};
    float viewport_bottom{};
    std::array<ImVec2, 8> cards{};
};

Frame Draw(float width, float height, bool imported, bool scroll_to_end = false) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin("Mods page", nullptr, ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted("Import archive");
    if (imported) ImGui::TextUnformatted("Imported 4 files");
    Frame result;
    result.width = anomaly::launcher::BeginModsList();
    result.scrollbar = ImGui::GetCurrentWindow()->ScrollbarY;
    result.scroll_max = ImGui::GetScrollMaxY();
    result.viewport_height = ImGui::GetWindowHeight();
    result.viewport_bottom = ImGui::GetWindowPos().y + result.viewport_height;
    if (scroll_to_end) ImGui::SetScrollY(result.scroll_max);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // Eight aspect-ratio cards in two rows reproduce the reported feedback loop.
    // Their exact artwork and captions do not affect the scrollbar contract.
    const float card_width = std::floor((result.width - 5.0f * 12.0f) / 6.0f);
    for (int index = 0; index < 8; ++index) {
        if (index % 6 != 0) ImGui::SameLine(0.0f, 12.0f);
        ImGui::PushID(index);
        ImGui::InvisibleButton("card", ImVec2(card_width, card_width * 0.75f + 40.0f));
        result.cards[index] = ImGui::GetItemRectMin();
        ImGui::PopID();
    }
    result.content_height = ImGui::GetItemRectMax().y - origin.y;
    result.content_bottom = ImGui::GetItemRectMax().y;
    ImGui::EndChild();
    ImGui::End();
    ImGui::Render();
    return result;
}

void Check(float width) {
    float grid_width{};
    bool saw_fit{}, saw_overflow{};
    for (float height = 340.0f; height <= 500.0f; height += 1.0f) {
        for (bool imported : {false, true, false}) {
            Frame previous;
            for (int frame = 0; frame < 12; ++frame) {
                const Frame current = Draw(width, height, imported);
                if (frame >= 4) {
                    if (grid_width == 0.0f) grid_width = current.width;
                    Require(current.scrollbar == previous.scrollbar,
                        "scrollbar oscillated after layout settled");
                    Require(current.width == grid_width,
                        "card width changed when a status line or scrollbar changed");
                    for (std::size_t i = 0; i < current.cards.size(); ++i) {
                        Require(current.cards[i].x == previous.cards[i].x &&
                                current.cards[i].y == previous.cards[i].y,
                            "cards moved between identical frames");
                    }
                    if (current.content_height < current.viewport_height - 1.0f) {
                        Require(!current.scrollbar, "scrollbar visible while cards fit");
                        saw_fit = true;
                    } else if (current.content_height > current.viewport_height + 1.0f) {
                        Require(current.scrollbar && current.scroll_max > 0.0f,
                            "overflowing cards cannot be scrolled");
                        saw_overflow = true;
                    }
                }
                previous = current;
            }
        }
    }
    Require(saw_fit && saw_overflow, "fixture did not cover both sides of the overflow threshold");
    // Real overflow must remain scrollable, with the last row reachable.
    Frame end;
    for (int frame = 0; frame < 12; ++frame) end = Draw(width, 340.0f, true, true);
    Require(end.scrollbar && end.scroll_max > 0.0f, "scrollbar disappeared while scrolling");
    Require(end.content_bottom <= end.viewport_bottom + 1.0f,
        "last row was not fully reachable by scrolling");
}

}  // namespace

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
    try {
        Check(1240.0f);
        Check(1170.0f);
        ImGui::DestroyContext();
        std::cout << "Launcher mod list scrollbar regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ImGui::DestroyContext();
        return 1;
    }
}
