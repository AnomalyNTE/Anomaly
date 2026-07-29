#include "anomaly/plugin_shadow_store.hpp"

#include "anomaly/plugin_package.hpp"

#include <Windows.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace anomaly {
namespace {

bool CopyPackageTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& failure) {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) {
        failure = "shadow staging directory could not be created";
        return false;
    }
    for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error) break;
        if (std::filesystem::is_symlink(status)) {
            failure = "package tree contains a reparse point";
            return false;
        }
        const std::filesystem::path relative = iterator->path().lexically_relative(source);
        const std::filesystem::path target = destination / relative;
        if (std::filesystem::is_directory(status)) {
            std::filesystem::create_directory(target, error);
        } else if (std::filesystem::is_regular_file(status)) {
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error) {
                std::filesystem::copy_file(
                    iterator->path(), target, std::filesystem::copy_options::overwrite_existing,
                    error);
            }
        } else {
            failure = "package tree contains a non-regular entry";
            return false;
        }
        if (error) break;
    }
    if (error) {
        failure = "package tree copy failed: " + error.message();
        return false;
    }
    return true;
}

}  // namespace

PluginShadowStore::PluginShadowStore(std::filesystem::path root) : root_(std::move(root)) {}

PluginShadowResult PluginShadowStore::Stage(const PluginCatalogEntry& entry) {
    if (!entry.LoadCandidate() || !entry.manifest) return {{}, "catalog entry is not loadable"};
    const std::filesystem::path plugin_root = root_ / entry.manifest->id;
    std::error_code error;
    std::uint64_t generation{};
    std::filesystem::path destination;
    do {
        generation = next_generation_++;
        destination = plugin_root / std::to_wstring(generation);
        error.clear();
    } while (std::filesystem::exists(destination, error) && !error);
    if (error) return {{}, "shadow generation availability check failed"};
    const std::wstring suffix = L"-" + std::to_wstring(generation);
    const std::filesystem::path staging =
        plugin_root / (L".staging-" + std::to_wstring(GetCurrentProcessId()) + suffix);
    std::filesystem::remove_all(staging, error);
    error.clear();
    std::string failure;
    if (!CopyPackageTree(entry.package_root, staging, failure)) {
        std::filesystem::remove_all(staging, error);
        return {{}, std::move(failure)};
    }
    const PluginPackagePathResult confined = OpenConfinedPluginPackageFile(
        staging, entry.manifest->entry, true);
    if (!confined.Ok()) {
        std::filesystem::remove_all(staging, error);
        return {{}, "shadow entry validation failed: " + confined.message};
    }
    for (unsigned attempt = 0; attempt != 10; ++attempt) {
        error.clear();
        std::filesystem::rename(staging, destination, error);
        if (!error) break;
        Sleep(10u * (attempt + 1u));
    }
    if (error) {
        const std::string detail = error.message();
        std::filesystem::remove_all(staging, error);
        return {{}, "shadow generation publish failed: " + detail};
    }
    const PluginPackagePathResult published = OpenConfinedPluginPackageFile(
        destination, entry.manifest->entry, true);
    if (!published.Ok()) {
        std::filesystem::remove_all(destination, error);
        return {{}, "published shadow entry validation failed"};
    }
    return {PluginShadowGeneration{
        entry.manifest->id, generation, destination, published.path, *entry.manifest}, {}};
}

void PluginShadowStore::Retire(const PluginShadowGeneration& generation) noexcept {
    std::error_code error;
    std::filesystem::remove_all(generation.package_root, error);
}

void PluginShadowStore::CleanupPluginExcept(
    std::string_view plugin_id, std::optional<std::uint64_t> keep_generation) noexcept {
    const std::filesystem::path plugin_root = root_ / std::string(plugin_id);
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(plugin_root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (keep_generation && iterator->path().filename() == std::to_wstring(*keep_generation)) {
            continue;
        }
        std::filesystem::remove_all(iterator->path(), error);
        error.clear();
    }
}

void PluginShadowStore::Cleanup() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
}

}  // namespace anomaly
