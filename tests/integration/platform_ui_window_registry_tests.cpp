#include "anomaly/host_ui_service.hpp"
#include "platform_host.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "plugin_manager.hpp"

#include <Windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kExpectedStableId =
    "24;anomaly.host.platform-ui;16;management-shell;";
constexpr std::string_view kExpectedWindowName =
    "Anomaly Plugin Platform###24;anomaly.host.platform-ui;16;management-shell;";

bool Expect(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool RepositoryUpdatesFollowInstalledManifest() {
    const auto manifest_version = anomaly::ParseSemanticVersion("0.0.1");
    if (!Expect(manifest_version.has_value(), "installed version fixture did not parse")) {
        return false;
    }

    anomaly::PluginManifest manifest;
    manifest.id = "com.example.ui-gallery";
    manifest.name = "UI Gallery";
    manifest.version = *manifest_version;
    anomaly::PluginCatalogEntry entry;
    entry.manifest = std::move(manifest);
    entry.status = anomaly::PluginCatalogStatus::Valid;
    anomaly::PluginCatalogSnapshot catalog({std::move(entry)});
    const auto dependencies = anomaly::ResolvePluginDependencies(catalog);

    ue5mem::PluginView runtime;
    runtime.id = "com.example.ui-gallery";
    runtime.name = "UI Gallery";
    runtime.version = "1.0.0";
    const auto snapshot = anomaly::BuildPlatformUiSnapshot(
        1, {std::move(runtime)}, catalog, dependencies, {}, {}, {});
    const auto* installed = snapshot.FindPlugin("com.example.ui-gallery");
    if (!Expect(installed != nullptr && installed->version == "0.0.1",
            "installed manifest did not override stale runtime version")) {
        return false;
    }

    anomaly::RepositoryPluginView available;
    available.entry.internal_name = installed->id;
    available.entry.version = "1.0.0";

    const auto outdated = anomaly::ResolveRepositoryPluginInstallState(
        installed, available);
    bool result = Expect(outdated.installed && outdated.update_available,
        "older installed repository plugin was not updateable");

    anomaly::InstalledPluginView current;
    current.id = installed->id;
    current.version = available.entry.version;
    const auto latest = anomaly::ResolveRepositoryPluginInstallState(&current, available);
    result = Expect(latest.installed && !latest.update_available,
                 "current repository plugin remained updateable") &&
        result;
    return result;
}

bool DrawFrame(std::string* rendered_text = nullptr) {
    ImGui::NewFrame();
    if (rendered_text != nullptr) ImGui::LogToBuffer();
    anomaly::PrepareHostUiFrame();
    ue5mem::DrawPlatformUi();
    if (rendered_text != nullptr) {
        *rendered_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::EndFrame();
    return true;
}

bool ClickAt(ImGuiIO& io, const ImVec2 position) {
    io.AddMousePosEvent(position.x, position.y);
    if (!DrawFrame()) return false;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (!DrawFrame()) return false;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    return DrawFrame();
}

ImGuiWindow* FindWindowContaining(const std::string_view fragment) {
    for (ImGuiWindow* const window : GImGui->Windows) {
        if (window != nullptr && window->Name != nullptr &&
            std::string_view(window->Name).find(fragment) != std::string_view::npos) {
            return window;
        }
    }
    return nullptr;
}

bool FindGlyphOriginX(const ImGuiWindow* const window, const ImFontGlyph& glyph,
    float* const output) {
    if (window == nullptr || output == nullptr || window->DrawList == nullptr) return false;
    constexpr float kUvTolerance = 0.001F;
    for (const ImDrawVert& vertex : window->DrawList->VtxBuffer) {
        if (std::abs(vertex.uv.x - glyph.U0) <= kUvTolerance &&
            std::abs(vertex.uv.y - glyph.V0) <= kUvTolerance) {
            *output = vertex.pos.x;
            return true;
        }
    }
    return false;
}

bool FindGlyphOrigin(const ImGuiWindow* const window, const ImFontGlyph& glyph,
    ImVec2* const output) {
    if (window == nullptr || output == nullptr || window->DrawList == nullptr) return false;
    constexpr float kUvTolerance = 0.001F;
    for (const ImDrawVert& vertex : window->DrawList->VtxBuffer) {
        if (std::abs(vertex.uv.x - glyph.U0) <= kUvTolerance &&
            std::abs(vertex.uv.y - glyph.V0) <= kUvTolerance) {
            *output = vertex.pos;
            return true;
        }
    }
    return false;
}

}  // namespace

int wmain() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"anomaly-platform-ui-window-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / L"plugins", error);
    if (error) return 2;

    IMGUI_CHECKVERSION();
    ImGuiContext* const context = ImGui::CreateContext();
    if (context == nullptr) return 3;
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 720.0F);
    static_cast<void>(ue5mem::ConfigurePlatformUiFontAtlas());
    unsigned char* pixels{};
    int width{};
    int height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    bool result = RepositoryUpdatesFollowInstalledManifest();
    auto manager = std::make_shared<ue5mem::PluginManager>(root, root / L"plugins");
    manager->SetUiService(anomaly::HostUiServiceTable());
    result = Expect(
                 manager->UiResources().ImportPersistentWindowState({{
                     std::string(kExpectedStableId), false, 1040.0F, 700.0F, {}}}),
                 "legacy closed management shell state was not imported") &&
        result;
    const bool initialized = ue5mem::InitializePlatformUi(*manager, {}, manager);
    result = Expect(initialized, "platform UI did not initialize") && result;
    if (initialized) {
        result = Expect(!anomaly::HostUiDeveloperModeEnabled(),
                     "platform UI did not reset developer mode for a new host") &&
            result;
        std::string plugins_page_text;
        result = Expect(DrawFrame(&plugins_page_text), "platform UI frame did not complete") && result;
        result = Expect(
                      plugins_page_text.find("Reload all") == std::string::npos,
                      "plugin page still rendered Reload all outside the actions menu") &&
            result;
        result = Expect(
                      plugins_page_text.find("NTE Compatibility") == std::string::npos,
                      "user navigation still exposes NTE compatibility") &&
            result;
        ImGuiWindow* management_window =
            ImGui::FindWindowByName(kExpectedWindowName.data());
        result = Expect(
                      management_window != nullptr && !management_window->HasCloseButton,
                      "management shell still exposed a permanent close button") &&
            result;
        result = Expect(
                      management_window != nullptr && management_window->Pos.x == 12.0F &&
                          management_window->Pos.y == 12.0F,
                      "management shell did not start at the viewport's top-left margin") &&
            result;
        io.DisplaySize = ImVec2(640.0F, 480.0F);
        result = Expect(DrawFrame(), "compact viewport frame did not complete") && result;
        management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
        result = Expect(
                      management_window != nullptr && management_window->Pos.x == 12.0F &&
                          management_window->Pos.y == 12.0F &&
                          management_window->Size.x <= 616.0F &&
                          management_window->Size.y <= 456.0F,
                      "management shell did not fit the compact viewport") &&
            result;
        ImGuiWindow* header_window = FindWindowContaining("PlatformGlobalHeader");
        result = Expect(header_window != nullptr,
                     "management shell did not render a global header") &&
            result;
        if (header_window != nullptr) {
            ImGuiWindow* const plugins_header = FindWindowContaining("PlatformPageHeader");
            result = Expect(plugins_header != nullptr,
                         "plugin page did not render an actions header") &&
                result;
            if (plugins_header != nullptr) {
                const ImVec2 more_center(
                    plugins_header->Pos.x + plugins_header->Size.x - 31.0F,
                    plugins_header->Pos.y + plugins_header->Size.y * 0.5F);
                io.AddMousePosEvent(more_center.x, more_center.y);
                result = Expect(DrawFrame(), "plugin actions hover frame did not complete") && result;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                result = Expect(DrawFrame(), "plugin actions press frame did not complete") && result;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                std::string plugin_actions_text;
                result = Expect(DrawFrame(&plugin_actions_text),
                             "plugin actions release frame did not complete") &&
                    result;
                result = Expect(
                             plugin_actions_text.find("Reload all") != std::string::npos,
                             "plugin actions menu did not offer Reload all") &&
                    result;
                ImGui::ClosePopupToLevel(0, true);
                result = Expect(DrawFrame(), "plugin actions close frame did not complete") && result;
            }

            ImGuiWindow* const initial_navigation_window = FindWindowContaining("PlatformShellNavigation");
            result = Expect(initial_navigation_window != nullptr,
                         "management shell did not render navigation") && result;
            if (initial_navigation_window != nullptr) {
                const ImVec2 diagnostics_center(
                    initial_navigation_window->Pos.x + initial_navigation_window->Size.x * 0.5F,
                    initial_navigation_window->Pos.y + 10.0F + 1.0F * 44.0F + 20.0F);
                io.AddMousePosEvent(diagnostics_center.x, diagnostics_center.y);
                result = Expect(DrawFrame(), "diagnostics navigation hover frame did not complete") && result;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                result = Expect(DrawFrame(), "diagnostics navigation press frame did not complete") && result;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                std::string diagnostics_page_text;
                result = Expect(DrawFrame(&diagnostics_page_text),
                             "diagnostics navigation release frame did not complete") && result;
                result = Expect(
                              diagnostics_page_text.find("NTE Compatibility") == std::string::npos,
                              "ordinary diagnostics still exposes NTE compatibility") &&
                    result;
            }

            anomaly::SetHostUiMenusCollapsed(true);
            result = Expect(DrawFrame(), "management shell global collapse frame did not complete") && result;
            management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
            ImGuiWindow* const compact_header_window = FindWindowContaining("PlatformGlobalHeader");
            result = Expect(
                         management_window != nullptr && !management_window->Collapsed &&
                             management_window->Size.y == 52.0F &&
                             management_window->Size.x <= 132.0F &&
                             compact_header_window != nullptr &&
                             compact_header_window->Size.x <= 132.0F,
                         "management shell global collapse did not produce the compact identity header") &&
                result;
            anomaly::SetHostUiMenusCollapsed(false);
            result = Expect(DrawFrame(), "management shell global expansion frame did not complete") && result;
            management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
            result = Expect(
                         management_window != nullptr && management_window->Size.y > 52.0F &&
                             management_window->Size.x > 132.0F,
                         "management shell did not restore its expanded geometry") &&
                result;

            if (management_window != nullptr) {
                const ImVec2 requested_position(
                    management_window->Pos.x + 36.0F, management_window->Pos.y + 24.0F);
                ImGui::SetWindowPos(management_window, requested_position, ImGuiCond_Always);
                result = Expect(DrawFrame(), "management shell moved-position frame did not complete") && result;
                management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                result = Expect(
                             management_window != nullptr &&
                                 management_window->Pos.x == requested_position.x &&
                                 management_window->Pos.y == requested_position.y,
                             "management shell position was reset instead of remaining movable") &&
                    result;
            }

            io.DisplaySize = ImVec2(1024.0F, 720.0F);
            result = Expect(DrawFrame(), "resizable management shell viewport frame did not complete") && result;
            management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
            if (management_window != nullptr) {
                ImVec2 requested_size(960.0F, 650.0F);
                ImGui::SetWindowSize(management_window, requested_size, ImGuiCond_Always);
                result = Expect(DrawFrame(), "management shell resized frame did not complete") && result;
                management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                result = Expect(
                             management_window != nullptr &&
                                 (management_window->Flags & ImGuiWindowFlags_NoResize) == 0 &&
                                 management_window->Size.x == requested_size.x &&
                                 management_window->Size.y == requested_size.y,
                             "management shell size was not retained after resizing") &&
                    result;

                if (management_window != nullptr) {
                    const ImVec2 resize_start(
                        management_window->Pos.x + management_window->Size.x - 1.0F,
                        management_window->Pos.y + management_window->Size.y - 1.0F);
                    io.AddMousePosEvent(resize_start.x, resize_start.y);
                    result = Expect(DrawFrame(), "management shell resize hover frame did not complete") && result;
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                    result = Expect(DrawFrame(), "management shell resize press frame did not complete") && result;
                    const ImVec2 resize_delta(-40.0F, -30.0F);
                    io.AddMousePosEvent(
                        resize_start.x + resize_delta.x, resize_start.y + resize_delta.y);
                    result = Expect(DrawFrame(), "management shell resize move frame did not complete") && result;
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                    result = Expect(DrawFrame(), "management shell resize release frame did not complete") && result;
                    requested_size.x += resize_delta.x;
                    requested_size.y += resize_delta.y;
                    management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                    result = Expect(
                                 management_window != nullptr &&
                                     management_window->Size.x == requested_size.x &&
                                     management_window->Size.y == requested_size.y,
                                 "management shell resize grip did not update its geometry") &&
                        result;
                }

                anomaly::SetHostUiMenusCollapsed(true);
                result = Expect(DrawFrame(), "resized management shell collapse frame did not complete") && result;
                management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                result = Expect(
                             management_window != nullptr && management_window->Size.x == 132.0F &&
                                 management_window->Size.y == 52.0F,
                             "resized management shell did not enter its compact global state") &&
                    result;
                anomaly::SetHostUiMenusCollapsed(false);
                result = Expect(DrawFrame(), "resized management shell expansion frame did not complete") && result;
                management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                result = Expect(
                             management_window != nullptr &&
                                 management_window->Size.x == requested_size.x &&
                                 management_window->Size.y == requested_size.y,
                             "resized management shell did not restore its expanded size") &&
                    result;

                header_window = FindWindowContaining("PlatformGlobalHeader");
                if (header_window != nullptr) {
                    const ImVec2 collapse_center(
                        header_window->Pos.x + header_window->Size.x - 95.0F,
                        header_window->Pos.y + header_window->Size.y * 0.5F);
                    io.AddMousePosEvent(collapse_center.x, collapse_center.y);
                    result = Expect(DrawFrame(), "management shell collapse hover frame did not complete") && result;
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                    result = Expect(DrawFrame(), "management shell collapse press frame did not complete") && result;
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                    result = Expect(DrawFrame(), "management shell collapse release frame did not complete") && result;
                    result = Expect(DrawFrame(), "management shell collapsed layout frame did not complete") && result;
                    management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                    result = Expect(
                                 management_window != nullptr &&
                                     management_window->Size.x == requested_size.x &&
                                     management_window->Size.y == 52.0F,
                                 "management shell manual collapse did not preserve its width") &&
                        result;

                    header_window = FindWindowContaining("PlatformGlobalHeader");
                    if (header_window != nullptr) {
                        const ImVec2 expand_center(
                            header_window->Pos.x + header_window->Size.x - 95.0F,
                            header_window->Pos.y + header_window->Size.y * 0.5F);
                        io.AddMousePosEvent(expand_center.x, expand_center.y);
                        result = Expect(DrawFrame(), "management shell expand hover frame did not complete") && result;
                        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                        result = Expect(DrawFrame(), "management shell expand press frame did not complete") && result;
                        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                        result = Expect(DrawFrame(), "management shell expand release frame did not complete") && result;
                        result = Expect(DrawFrame(), "management shell expanded layout frame did not complete") && result;
                        management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                        result = Expect(
                                     management_window != nullptr &&
                                         management_window->Size.x == requested_size.x &&
                                         management_window->Size.y == requested_size.y,
                                     "management shell manual expansion did not restore its size") &&
                            result;

                        header_window = FindWindowContaining("PlatformGlobalHeader");
                        management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                        if (header_window != nullptr && management_window != nullptr) {
                            const ImVec2 drag_start(
                                header_window->Pos.x + 180.0F,
                                header_window->Pos.y + header_window->Size.y * 0.5F);
                            io.AddMousePosEvent(drag_start.x, drag_start.y);
                            result = Expect(DrawFrame(), "management shell drag hover frame did not complete") && result;
                            management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                            const ImVec2 position_before = management_window == nullptr
                                ? ImVec2{} : management_window->Pos;
                            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                            result = Expect(DrawFrame(), "management shell drag press frame did not complete") && result;
                            const ImVec2 drag_delta(28.0F, 18.0F);
                            io.AddMousePosEvent(
                                drag_start.x + drag_delta.x, drag_start.y + drag_delta.y);
                            result = Expect(DrawFrame(), "management shell drag move frame did not complete") && result;
                            management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
                            result = Expect(
                                         management_window != nullptr &&
                                             management_window->Pos.x == position_before.x + drag_delta.x &&
                                             management_window->Pos.y == position_before.y + drag_delta.y,
                                         "management shell header drag did not move the root window") &&
                                result;
                            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                            result = Expect(DrawFrame(), "management shell drag release frame did not complete") && result;
                        }
                    }
                }
            }

            header_window = FindWindowContaining("PlatformGlobalHeader");
            if (header_window == nullptr) {
                result = Expect(false, "management shell header disappeared after moving") && result;
            } else {
            ImGuiWindow* developer_navigation = FindWindowContaining("PlatformShellNavigation");
            result = Expect(developer_navigation != nullptr,
                         "management shell did not render navigation for developer mode") &&
                result;
            if (developer_navigation != nullptr) {
                const ImVec2 settings_center(
                    developer_navigation->Pos.x + developer_navigation->Size.x * 0.5F,
                    developer_navigation->Pos.y + 10.0F + 2.0F * 44.0F + 20.0F);
                result = Expect(ClickAt(io, settings_center),
                             "settings navigation click did not complete") &&
                    result;
            }

            ImGuiWindow* const settings_window = FindWindowContaining("PlatformSettingsRoute");
            const ImFontGlyph* const enable_glyph = io.FontDefault == nullptr
                ? nullptr
                : io.FontDefault->FindGlyphNoFallback(static_cast<ImWchar>('E'));
            ImVec2 enable_origin{};
            const bool enable_found = enable_glyph != nullptr &&
                FindGlyphOrigin(settings_window, *enable_glyph, &enable_origin);
            result = Expect(enable_found,
                         "settings page did not render the developer mode control") &&
                result;
            if (enable_found) {
                const ImVec2 enable_center(enable_origin.x + 4.0F, enable_origin.y + 4.0F);
                result = Expect(ClickAt(io, enable_center),
                             "developer mode control click did not complete") &&
                    result;
            }
            result = Expect(anomaly::HostUiDeveloperModeEnabled(),
                         "developer mode control did not publish the UI gate") &&
                result;

            developer_navigation = FindWindowContaining("PlatformShellNavigation");
            result = Expect(developer_navigation != nullptr,
                         "management shell lost navigation after enabling developer mode") &&
                result;
            if (developer_navigation != nullptr) {
                const ImVec2 diagnostics_center(
                    developer_navigation->Pos.x + developer_navigation->Size.x * 0.5F,
                    developer_navigation->Pos.y + 10.0F + 1.0F * 44.0F + 20.0F);
                result = Expect(ClickAt(io, diagnostics_center),
                             "diagnostics navigation click did not complete") &&
                    result;
            }

            ImGuiWindow* const diagnostic_tabs = FindWindowContaining("PlatformDiagnosticTabs");
            const ImFontGlyph* const developer_glyph = io.FontDefault == nullptr
                ? nullptr
                : io.FontDefault->FindGlyphNoFallback(static_cast<ImWchar>('D'));
            ImVec2 developer_tab_origin{};
            const bool developer_tab_found = developer_glyph != nullptr &&
                FindGlyphOrigin(diagnostic_tabs, *developer_glyph, &developer_tab_origin);
            result = Expect(developer_tab_found,
                         "developer diagnostics tab was not rendered after enabling developer mode") &&
                result;
            if (developer_tab_found) {
                const ImVec2 developer_tab_center(
                    developer_tab_origin.x + 4.0F, developer_tab_origin.y + 4.0F);
                result = Expect(ClickAt(io, developer_tab_center),
                             "developer diagnostics tab click did not complete") &&
                    result;
            }

            ImGuiWindow* const developer_rail = FindWindowContaining("PlatformDeveloperRail");
            result = Expect(developer_rail != nullptr,
                         "developer diagnostics did not render its desktop rail") &&
                result;
            if (developer_rail != nullptr) {
                const ImFontGlyph* const services_interior_glyph = io.FontDefault == nullptr
                    ? nullptr
                    : io.FontDefault->FindGlyphNoFallback(static_cast<ImWchar>('v'));
                ImVec2 services_origin{};
                const bool services_label_complete = services_interior_glyph != nullptr &&
                    FindGlyphOrigin(developer_rail, *services_interior_glyph, &services_origin);
                result = Expect(services_label_complete,
                             "developer rail clipped the Services label") &&
                    result;
                if (services_label_complete) {
                    const ImVec2 services_right_edge(
                        developer_rail->WorkRect.Max.x - 4.0F, services_origin.y + 4.0F);
                    result = Expect(ClickAt(io, services_right_edge),
                                 "developer rail service selection click did not complete") &&
                        result;
                    std::string services_text;
                    result = Expect(DrawFrame(&services_text),
                                 "developer services selection frame did not complete") &&
                        result;
                    result = Expect(services_text.find("Service graph") != std::string::npos,
                                 "developer rail did not accept a click across its full width") &&
                        result;
                }
            }

            // The fixed header action column is [collapse] [lock] [disabled close].
            // It has a 12px right inset and 4px gaps between 30px controls.
            const ImVec2 lock_center(
                header_window->Pos.x + header_window->Size.x - 61.0F,
                header_window->Pos.y + header_window->Size.y * 0.5F);
            io.AddMousePosEvent(lock_center.x, lock_center.y);
            result = Expect(DrawFrame(), "management shell lock hover frame did not complete") && result;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            result = Expect(DrawFrame(), "management shell lock press frame did not complete") && result;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            result = Expect(DrawFrame(), "management shell lock release frame did not complete") && result;

            anomaly::SetHostUiMenusCollapsed(true);
            result = Expect(DrawFrame(), "locked management shell collapse frame did not complete") && result;
            management_window = ImGui::FindWindowByName(kExpectedWindowName.data());
            result = Expect(
                         management_window != nullptr && !management_window->Collapsed &&
                             management_window->Size.x > 132.0F && management_window->Size.y > 52.0F,
                         "locked management shell was collapsed by the host menu request") &&
                result;
            anomaly::SetHostUiMenusCollapsed(false);
            result = Expect(DrawFrame(), "host menu expansion frame did not complete") && result;

            // The disabled close control is the last 30px action. Clicking it
            // must leave the host-owned management shell registered and open.
            const ImVec2 close_center(
                header_window->Pos.x + header_window->Size.x - 27.0F,
                header_window->Pos.y + header_window->Size.y * 0.5F);
            io.AddMousePosEvent(close_center.x, close_center.y);
            result = Expect(DrawFrame(), "management shell close hover frame did not complete") && result;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            result = Expect(DrawFrame(), "management shell close press frame did not complete") && result;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            result = Expect(DrawFrame(), "management shell close release frame did not complete") && result;
            const auto after_disabled_close = manager->UiResources().ExportPersistentWindowState();
            result = Expect(
                         after_disabled_close.size() == 1 && after_disabled_close.front().open,
                         "disabled management shell close control closed the main interface") &&
                result;

            // The resized shell is now in the standard layout. Verify its
            // arrow-only navigation control collapses and restores the rail.
            result = Expect(DrawFrame(), "standard-layout navigation frame did not complete") && result;
            ImGuiWindow* navigation_window = FindWindowContaining("PlatformShellNavigation");
            result = Expect(navigation_window != nullptr,
                         "management shell did not render navigation") &&
                result;
            if (navigation_window != nullptr) {
                const float expanded_navigation_width = navigation_window->Size.x;
                const ImFontGlyph* const navigation_package = io.FontDefault == nullptr
                    ? nullptr
                    : io.FontDefault->FindGlyphNoFallback(static_cast<ImWchar>(0xE7B8));
                float expanded_navigation_package_x{};
                const bool expanded_package_found = navigation_package != nullptr &&
                    FindGlyphOriginX(navigation_window, *navigation_package,
                        &expanded_navigation_package_x);
                result = Expect(expanded_package_found,
                             "expanded navigation did not render the Plugins glyph") &&
                    result;
                const ImVec2 navigation_arrow_center(
                    navigation_window->Pos.x + 24.0F,
                    navigation_window->WorkRect.Max.y - 24.0F);
                io.AddMousePosEvent(navigation_arrow_center.x, navigation_arrow_center.y);
                result = Expect(DrawFrame(), "navigation collapse hover frame did not complete") && result;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                result = Expect(DrawFrame(), "navigation collapse press frame did not complete") && result;
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                result = Expect(DrawFrame(), "navigation collapse release frame did not complete") && result;
                result = Expect(DrawFrame(), "collapsed navigation layout frame did not complete") && result;
                navigation_window = FindWindowContaining("PlatformShellNavigation");
                result = Expect(
                             navigation_window != nullptr &&
                                 navigation_window->Size.x < expanded_navigation_width,
                             "navigation arrow did not collapse the left rail") &&
                    result;
                if (navigation_window != nullptr) {
                    const float collapsed_navigation_width = navigation_window->Size.x;
                    float collapsed_navigation_package_x{};
                    const bool collapsed_package_found = navigation_package != nullptr &&
                        FindGlyphOriginX(navigation_window, *navigation_package,
                            &collapsed_navigation_package_x);
                    result = Expect(collapsed_package_found,
                                 "collapsed navigation did not render the Plugins glyph") &&
                        result;
                    if (expanded_package_found && collapsed_package_found) {
                        result = Expect(
                                     std::abs(expanded_navigation_package_x -
                                          collapsed_navigation_package_x) <= 0.1F,
                                     "navigation icons shifted horizontally when the rail collapsed") &&
                            result;
                    }
                    const ImVec2 expand_arrow_center(
                        navigation_window->Pos.x + 24.0F,
                        navigation_window->WorkRect.Max.y - 24.0F);
                    io.AddMousePosEvent(expand_arrow_center.x, expand_arrow_center.y);
                    result = Expect(DrawFrame(), "navigation expand hover frame did not complete") && result;
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                    result = Expect(DrawFrame(), "navigation expand press frame did not complete") && result;
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                    result = Expect(DrawFrame(), "navigation expand release frame did not complete") && result;
                    result = Expect(DrawFrame(), "expanded navigation layout frame did not complete") && result;
                    navigation_window = FindWindowContaining("PlatformShellNavigation");
                    result = Expect(
                                 navigation_window != nullptr &&
                                     navigation_window->Size.x > collapsed_navigation_width,
                                 "navigation arrow did not restore the left rail") &&
                        result;
                }
            }
            io.DisplaySize = ImVec2(640.0F, 480.0F);
            result = Expect(DrawFrame(), "compact-layout restoration frame did not complete") && result;
            }
        }
        result = Expect(
                      manager->UiResources().ResourceLeaseCount() == 2,
                      "management shell did not hold its window and logo leases") &&
            result;
        const auto persisted = manager->UiResources().ExportPersistentWindowState();
        result = Expect(
                     persisted.size() == 1 && persisted.front().stable_id == kExpectedStableId &&
                         persisted.front().open &&
                         persisted.front().width > 0.0F && persisted.front().width <= 616.0F &&
                         persisted.front().height > 0.0F && persisted.front().height <= 456.0F,
                     "management shell did not repair its legacy closed state") &&
            result;

        auto closed_persisted = persisted;
        closed_persisted.front().open = false;
        result = Expect(
                     manager->UiResources().ImportPersistentWindowState(
                         std::move(closed_persisted)),
                     "runtime closed management shell state was not imported") &&
            result;
        result = Expect(
                     ue5mem::RevealPlatformUi(),
                     "management shell reveal did not reopen a closed window") &&
            result;
        const auto revealed = manager->UiResources().ExportPersistentWindowState();
        result = Expect(
                     revealed.size() == 1 && revealed.front().open,
                     "management shell reveal did not repair persistent state") &&
            result;

        manager->PersistUiWindowState();
        result = Expect(
                     std::filesystem::exists(root / L"state" / L"ui-window-state.json"),
                     "management shell state was not persisted by the shared registry path") &&
            result;
        result = Expect(ue5mem::ShutdownPlatformUi(), "platform UI did not shut down") && result;
        result = Expect(!anomaly::HostUiDeveloperModeEnabled(),
                     "platform UI shutdown did not clear the UI gate") &&
            result;
        result = Expect(
                     manager->UiResources().ResourceLeaseCount() == 0,
                     "management shell registry lease survived shutdown") &&
            result;
    }

    manager.reset();
    ImGui::DestroyContext(context);
    std::filesystem::remove_all(root, error);
    return result ? 0 : 1;
}
