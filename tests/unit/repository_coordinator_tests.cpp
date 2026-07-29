// Coverage for the plugin-list/config parsers, safe ZIP extraction, and the
// coordinator's local-fixture refresh/install/recovery path.

#include "anomaly/plugin_list.hpp"
#include "anomaly/repository_coordinator.hpp"
#include "anomaly/plugin_repository_config.hpp"
#include "anomaly/safe_zip.hpp"

#include "miniz.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

std::filesystem::path WorkDirectory() {
    return std::filesystem::temp_directory_path() / "anomaly_plugin_repo_tests";
}

// Writes a zip with the given (name, contents) entries to `path`.
void WriteZip(const std::filesystem::path& path,
              const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_file(&zip, path.string().c_str(), 0);
    for (const auto& [name, contents] : entries) {
        mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                              static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION));
    }
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
}

void WriteText(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
}

std::string FileUri(const std::filesystem::path& path) {
    return "file:///" + std::filesystem::absolute(path).generic_string();
}

std::string Manifest(std::string_view id, std::string_view version) {
    return std::string(R"({
  "schemaVersion": 2,
  "id": ")") + std::string(id) + R"(",
  "name": "Repository Fixture",
  "author": "Tests",
  "license": "MIT",
  "version": ")" + std::string(version) + R"(",
  "entry": "plugin.dll",
  "api": {"major": 1, "minMinor": 0, "maxMinor": 0},
  "games": ["nte"],
  "builds": ["nte-*"],
  "loadPhase": "game-ready",
  "capabilities": []
})";
}

template <typename Predicate>
bool WaitFor(Predicate predicate) {
    for (int attempt = 0; attempt != 300; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void TestPluginListParsing() {
    const auto result = anomaly::ParsePluginList(R"([
        {"Name":"UI Gallery","InternalName":"com.example.ui-gallery","Version":"1.0.0",
         "Author":"AnomalyNTE","Games":["nte"],"ApiMajor":1,"Tags":["ui","demo"],
         "DownloadLinkInstall":"https://example.test/ui.zip"}
    ])");
    Check(result.Ok(), "valid plugin list parses");
    Check(result.entries.size() == 1, "one entry parsed");
    if (!result.entries.empty()) {
        const auto& entry = result.entries.front();
        Check(entry.internal_name == "com.example.ui-gallery", "internal name parsed");
        Check(entry.version == "1.0.0", "version parsed");
        Check(entry.api_major == 1, "api major parsed");
        Check(entry.games.size() == 1 && entry.games[0] == "nte", "games parsed");
        Check(entry.tags.size() == 2, "tags parsed");
        Check(entry.download_link_update == entry.download_link_install,
              "update link falls back to install link");
    }

    Check(!anomaly::ParsePluginList("{ not json").Ok(), "malformed JSON rejected");
    Check(anomaly::ParsePluginList("{}").error == anomaly::PluginListParseError::NotAnArray,
          "non-array root rejected");

    const auto partial = anomaly::ParsePluginList(
        R"([{"Name":"missing fields"},
            {"Name":"ok","InternalName":"a.b","Version":"1.0.0","DownloadLinkInstall":"https://x/y.zip"}])");
    Check(partial.Ok() && partial.entries.size() == 1 && partial.skipped == 1,
          "incomplete entry skipped, complete entry kept");

    const auto unsafe = anomaly::ParsePluginList(
        R"([{"Name":"bad id","InternalName":"../escape","Version":"1.0.0",
             "DownloadLinkInstall":"https://x/bad.zip"},
            {"Name":"bad version","InternalName":"a.c","Version":"latest",
             "DownloadLinkInstall":"https://x/bad.zip"},
            {"Name":"bad api","InternalName":"a.d","Version":"1.0.0",
             "ApiMajor":4294967296,
             "DownloadLinkInstall":"https://x/bad.zip"}])");
    Check(unsafe.Ok() && unsafe.entries.empty() && unsafe.skipped == 3,
          "unsafe ids, versions, and API values are skipped");
}

void TestConfigParsing() {
    // String entries (the legacy flat form) parse as enabled channels.
    const auto result = anomaly::ParsePluginRepositoryConfig(
        R"({"schemaVersion":1,"enabled":true,
            "repositories":["https://raw.example.test/pluginmaster.json"]})");
    Check(result.ok, "config parses");
    Check(result.config.enabled, "enabled parsed");
    Check(result.config.repositories.size() == 1, "repository url parsed");
    if (!result.config.repositories.empty()) {
        Check(result.config.repositories[0].url == "https://raw.example.test/pluginmaster.json",
              "string channel url parsed");
        Check(result.config.repositories[0].enabled, "string channel defaults to enabled");
    }

    // Object entries carry a per-channel enable flag; a missing master switch is on.
    const auto objects = anomaly::ParsePluginRepositoryConfig(
        R"({"repositories":[
              {"url":"https://a.test/pm.json","enabled":false},
              {"url":"https://b.test/pm.json"}]})");
    Check(objects.ok && objects.config.enabled, "master enabled defaults to true");
    Check(objects.config.repositories.size() == 2, "object channels parsed");
    if (objects.config.repositories.size() == 2) {
        Check(!objects.config.repositories[0].enabled, "per-channel disable parsed");
        Check(objects.config.repositories[1].enabled, "per-channel enable defaults on");
    }

    const auto disabled = anomaly::ParsePluginRepositoryConfig(R"({"enabled":false})");
    Check(disabled.ok && !disabled.config.enabled && disabled.config.repositories.empty(),
          "disabled config parses with no repositories");
    Check(!anomaly::ParsePluginRepositoryConfig("[]").ok, "non-object config rejected");

    // Serialize -> parse round-trips the configuration losslessly.
    anomaly::PluginRepositoryConfig config;
    config.enabled = true;
    config.allow_insecure_sources = true;
    config.repositories.push_back({"https://c.test/pm.json", true});
    config.repositories.push_back({"https://d.test/pm.json", false});
    const auto round_trip = anomaly::ParsePluginRepositoryConfig(
        anomaly::SerializePluginRepositoryConfig(config));
    Check(round_trip.ok && round_trip.config == config,
          "config round-trips through serialization");

    Check(anomaly::IsPluginRepositoryUriAllowed("https://example.test/list.json", false),
          "HTTPS repository URL accepted");
    Check(!anomaly::IsPluginRepositoryUriAllowed("http://example.test/list.json", true),
          "HTTP repository URL rejected");
    Check(!anomaly::IsPluginRepositoryUriAllowed("file:///fixture/list.json", false),
          "file repository URL requires explicit opt-in");
    Check(anomaly::IsPluginRepositoryUriAllowed("file:///fixture/list.json", true),
          "file repository URL accepted for controlled fixtures");
    Check(!anomaly::IsPluginRepositoryUriAllowed("https://", false),
          "empty HTTPS repository target rejected");
    Check(!anomaly::IsPluginRepositoryUriAllowed("https://example.test/list json", false),
          "repository URL containing whitespace rejected");
}

void TestSafeZip() {
    std::error_code ec;
    const auto work = WorkDirectory();
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);

    // A well-formed plugin package.
    const auto good = work / "good.zip";
    WriteZip(good, {{"manifest.json", R"({"id":"a.b"})"},
                    {"plugin.dll", std::string(64, '\x4d')},
                    {"locales/zh-CN.json", "{}"}});
    const auto dest = work / "good-out";
    const auto extracted = anomaly::ExtractZip(good, dest);
    Check(extracted.Ok(), "good package extracts");
    Check(extracted.entries == 3, "three files extracted");
    Check(std::filesystem::exists(dest / "manifest.json"), "manifest extracted");
    Check(std::filesystem::exists(dest / "plugin.dll"), "dll extracted");
    Check(std::filesystem::exists(dest / "locales" / "zh-CN.json"), "nested file extracted");

    // A traversal attempt must be rejected and must not escape the destination.
    const auto evil = work / "evil.zip";
    WriteZip(evil, {{"../escape.txt", "pwned"}, {"ok.txt", "fine"}});
    const auto blocked = anomaly::ExtractZip(evil, work / "evil-out");
    Check(blocked.error == anomaly::SafeZipError::UnsafePath, "traversal entry rejected");
    Check(!std::filesystem::exists(work / "escape.txt"), "no file escaped the destination");

    const auto alternate = work / "alternate-paths.zip";
    WriteZip(alternate, {{"manifest.json", "one"}, {"MANIFEST.JSON", "two"}});
    Check(anomaly::ExtractZip(alternate, work / "alternate-out").error ==
              anomaly::SafeZipError::UnsafePath,
          "case-insensitive duplicate destinations rejected");

    const auto ads = work / "ads.zip";
    WriteZip(ads, {{"plugin.dll:payload", "bad"}});
    Check(anomaly::ExtractZip(ads, work / "ads-out").error ==
              anomaly::SafeZipError::UnsafePath,
          "Windows alternate data stream path rejected");

    Check(!anomaly::ExtractZip(work / "missing.zip", work / "none").Ok(),
          "missing archive reported");

    std::filesystem::remove_all(work, ec);
}

void TestCoordinatorInstallAndRecovery() {
    std::error_code ec;
    const auto work = WorkDirectory();
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);

    const auto runtime = work / "runtime";
    const auto package = work / "fixture.zip";
    const std::string id = "com.example.repository-fixture";
    WriteZip(package, {{"manifest.json", Manifest(id, "1.2.3")},
                       {"plugin.dll", "fixture-binary"}});

    const auto list = work / "pluginmaster.json";
    WriteText(list, std::string(R"([{
      "Name":"Repository Fixture",
      "InternalName":")") + id + R"(",
      "Version":"1.2.3",
      "Author":"Tests",
      "Games":["nte"],
      "ApiMajor":1,
      "DownloadLinkInstall":")" + FileUri(package) + R"("
    }])");

    anomaly::PluginRepositoryConfig config;
    config.allow_insecure_sources = true;
    config.repositories.push_back({FileUri(list), true});
    WriteText(runtime / "plugin-repositories.json",
              anomaly::SerializePluginRepositoryConfig(config));

    // Simulate a process interruption after plugins/<id> was moved to backup.
    const std::string recovered_id = "com.example.recovered";
    const auto backup = runtime / ".anomaly-plugin-transactions" / recovered_id / "backup";
    WriteText(backup / "manifest.json", Manifest(recovered_id, "1.0.0"));
    WriteText(backup / "plugin.dll", "recovered-binary");

    // An interrupted uninstall is completed instead of restoring a plugin the
    // user already chose to remove.
    const std::string removed_id = "com.example.removed";
    const auto removed_transaction = runtime / ".anomaly-plugin-transactions" / removed_id;
    WriteText(removed_transaction / "operation", "uninstall");
    WriteText(removed_transaction / "backup" / "manifest.json", Manifest(removed_id, "1.0.0"));
    WriteText(removed_transaction / "backup" / "plugin.dll", "removed-binary");

    anomaly::RepositoryCoordinatorOptions options;
    options.runtime_root = runtime;
    options.game = "nte";
    options.api_major = 1;
    anomaly::RepositoryCoordinator coordinator(std::move(options));
    Check(coordinator.Start(), "coordinator starts");
    Check(ReadText(runtime / "plugins" / recovered_id / "plugin.dll") == "recovered-binary",
          "interrupted install backup restored during startup");
    Check(!std::filesystem::exists(runtime / "plugins" / removed_id) &&
              !std::filesystem::exists(removed_transaction),
          "interrupted uninstall completed during startup");
    Check(WaitFor([&] {
        const auto snapshot = coordinator.Snapshot();
        return snapshot.state == anomaly::RepositoryCoordinatorState::Ready &&
            snapshot.plugins.size() == 1;
    }), "local fixture repository refreshed");

    const auto snapshot = coordinator.Snapshot();
    Check(snapshot.online_sources == 1 && snapshot.cached_sources == 0,
          "online repository counts published");
    if (!snapshot.plugins.empty()) {
        Check(snapshot.plugins[0].entry.internal_name == id, "catalog plugin identity published");
        Check(snapshot.plugins[0].compatible, "matching game and API are compatible");
    }

    const auto install = coordinator.InstallPlugin(id, "1.2.3");
    Check(install.accepted && install.operation_id != 0, "plugin install queued");
    Check(WaitFor([&] {
        const auto current = coordinator.Snapshot();
        return !current.operations.empty() &&
            current.operations.back().state == anomaly::RepositoryOperationState::Succeeded;
    }), "plugin install completed");
    Check(ReadText(runtime / "plugins" / id / "plugin.dll") == "fixture-binary",
          "installed plugin content published");

    Check(!coordinator.UninstallPlugin("com.example.unknown").accepted,
          "plugin outside the third-party catalog cannot be uninstalled");
    WriteText(runtime / "plugins" / id / "manifest.json",
              Manifest("com.example.identity-mismatch", "1.2.3"));
    const auto rejected_uninstall = coordinator.UninstallPlugin(id);
    Check(rejected_uninstall.accepted && WaitFor([&] {
        const auto current = coordinator.Snapshot();
        return !current.operations.empty() &&
            current.operations.back().state == anomaly::RepositoryOperationState::Failed;
    }), "identity-mismatched plugin uninstall rejected by worker");
    Check(std::filesystem::exists(runtime / "plugins" / id / "plugin.dll"),
          "identity-mismatched plugin was preserved");

    WriteText(runtime / "plugins" / id / "manifest.json", Manifest(id, "1.2.3"));
    const auto uninstall = coordinator.UninstallPlugin(id);
    Check(uninstall.accepted && uninstall.operation_id != 0, "plugin uninstall queued");
    Check(WaitFor([&] {
        const auto current = coordinator.Snapshot();
        return !current.operations.empty() &&
            current.operations.back().kind == anomaly::RepositoryOperationKind::Uninstall &&
            current.operations.back().state == anomaly::RepositoryOperationState::Succeeded;
    }), "plugin uninstall completed");
    Check(!std::filesystem::exists(runtime / "plugins" / id),
          "uninstalled plugin directory removed");

    anomaly::PluginRepositoryConfig invalid;
    invalid.repositories.push_back({"http://example.test/pluginmaster.json", true});
    Check(!coordinator.Configure(invalid).accepted,
          "unsupported channel transport rejected before persistence");

    anomaly::PluginRepositoryConfig disabled;
    disabled.enabled = false;
    Check(coordinator.Configure(disabled).accepted, "channels can be disabled at runtime");
    Check(coordinator.Snapshot().state == anomaly::RepositoryCoordinatorState::Disabled,
          "disabled configuration clears the live catalog");
    coordinator.Stop();

    std::filesystem::remove_all(work, ec);
}

}  // namespace

int main() {
    TestPluginListParsing();
    TestConfigParsing();
    TestSafeZip();
    TestCoordinatorInstallAndRecovery();
    if (g_failures == 0) {
        std::printf("repository_coordinator_tests: all checks passed\n");
        return 0;
    }
    std::printf("repository_coordinator_tests: %d failures\n", g_failures);
    return 1;
}
