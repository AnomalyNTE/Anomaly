#include "anomaly/plugin_catalog.hpp"
#include "anomaly/plugin_dependency_resolver.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures{};
void Expect(bool value, std::string_view message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

struct TempDirectory final {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
            (L"anomaly-catalog-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string Manifest(
    std::string_view id, std::string_view version = "1.0.0",
    std::string_view dependencies = "[]") {
    return "{\"schemaVersion\":2,\"id\":\"" + std::string(id) +
        "\",\"name\":\"Fixture\",\"version\":\"" + std::string(version) +
        "\",\"entry\":\"plugin.dll\",\"api\":{\"major\":3,\"minMinor\":0,\"maxMinor\":0},"
        "\"games\":[\"nte\"],\"builds\":[\"nte-*\"],\"loadPhase\":\"game-ready\","
        "\"dependencies\":" + std::string(dependencies) + ",\"capabilities\":[]}";
}

void AddPackage(
    const std::filesystem::path& root, std::wstring_view directory,
    const std::string& manifest, bool entry = true) {
    const std::filesystem::path package = root / directory;
    std::filesystem::create_directories(package);
    std::ofstream(package / L"manifest.json", std::ios::binary) << manifest;
    if (entry) std::ofstream(package / L"plugin.dll", std::ios::binary) << "fixture";
}

}  // namespace

int main() {
    TempDirectory temp;
    AddPackage(temp.path, L"Core", Manifest("fixture.core", "1.2.0"));
    AddPackage(temp.path, L"Feature", Manifest(
        "fixture.feature", "2.0.0",
        "[{\"id\":\"fixture.core\",\"version\":\">=1.0.0 <2.0.0\"}]"));
    AddPackage(temp.path, L"Broken", Manifest("fixture.broken"), false);
    std::filesystem::create_directories(temp.path / L"NoManifest");
    std::ofstream(temp.path / L"legacy.dll") << "ignored";

    const anomaly::PluginCatalogSnapshot catalog = anomaly::DiscoverPluginCatalog(temp.path);
    Expect(catalog.Entries().size() == 4, "only directory packages discovered");
    Expect(catalog.Find("fixture.core") != nullptr, "catalog index core");
    Expect(catalog.Find("fixture.feature") != nullptr, "catalog index feature");
    Expect(catalog.Find("fixture.broken") == nullptr, "invalid package excluded from index");
    Expect(catalog.AvailablePlugins().size() == 2, "available versions include valid packages");

    anomaly::CompatibilityIndex compatibility(catalog, {{"anomaly.ui", 1}});
    Expect(compatibility.Valid(), "compatibility index unique");
    Expect(compatibility.Plugin("fixture.core") != nullptr, "compatibility plugin lookup");
    Expect(compatibility.Service("anomaly.ui") != nullptr, "compatibility service lookup");

    anomaly::PluginDependencyPlan plan = anomaly::ResolvePluginDependencies(catalog);
    Expect(plan.load_order.size() == 2, "two plugins loadable");
    Expect(plan.load_order[0] == "fixture.core" && plan.load_order[1] == "fixture.feature",
        "topological load order");
    Expect(plan.stop_order[0] == "fixture.feature" && plan.stop_order[1] == "fixture.core",
        "reverse stop order");

    TempDirectory failures_root;
    AddPackage(failures_root.path, L"A", Manifest(
        "fixture.a", "1.0.0",
        "[{\"id\":\"fixture.b\",\"version\":\">=1.0.0\"}]"));
    AddPackage(failures_root.path, L"B", Manifest(
        "fixture.b", "1.0.0",
        "[{\"id\":\"fixture.a\",\"version\":\">=1.0.0\"}]"));
    AddPackage(failures_root.path, L"Dependent", Manifest(
        "fixture.dependent", "1.0.0",
        "[{\"id\":\"fixture.a\",\"version\":\">=1.0.0\"}]"));
    AddPackage(failures_root.path, L"Missing", Manifest(
        "fixture.missing", "1.0.0",
        "[{\"id\":\"fixture.absent\",\"version\":\">=1.0.0\"}]"));
    const auto failure_catalog = anomaly::DiscoverPluginCatalog(failures_root.path);
    const auto failure_plan = anomaly::ResolvePluginDependencies(failure_catalog);
    Expect(failure_plan.Find("fixture.a")->state == anomaly::PluginDependencyState::DependencyCycle,
        "cycle member A");
    Expect(failure_plan.Find("fixture.b")->state == anomaly::PluginDependencyState::DependencyCycle,
        "cycle member B");
    Expect(failure_plan.Find("fixture.dependent")->state ==
        anomaly::PluginDependencyState::BlockedTransitively, "cycle dependent blocked");
    Expect(failure_plan.Find("fixture.missing")->state ==
        anomaly::PluginDependencyState::MissingDependency, "missing dependency blocked");

    AddPackage(failures_root.path, L"Duplicate", Manifest("fixture.a"));
    const auto duplicate_catalog = anomaly::DiscoverPluginCatalog(failures_root.path);
    Expect(duplicate_catalog.Find("fixture.a") == nullptr, "duplicate IDs excluded");

    if (failures != 0) return 1;
    std::cout << "plugin catalog and dependency resolver passed\n";
    return 0;
}
