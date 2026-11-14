#include "anomaly/pak_mod_runtime.hpp"
#include "anomaly/pak_mod_settings.hpp"
#include "anomaly/pak_mod_catalog.hpp"
#include "anomaly/pak_mod_layout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <tuple>
#include <thread>
#include <utility>

namespace anomaly {
namespace {

constexpr std::string_view kPakExtension = ".pak";
constexpr std::string_view kDisabledSuffix = ".disabled";
constexpr std::size_t kMaximumScanFiles = 4096;

struct DesiredPak {
    std::string id;
    std::filesystem::path file;
    std::uint32_t order{};
    std::uint64_t bytes{};
    std::uint64_t stamp{};
    std::string fingerprint;
};

std::string Narrow(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length,
        nullptr, nullptr);
    return utf8;
}

// The presence of the IoStore index determines whether Mount should open a container sidecar.
bool IsPakFileContainer(const std::filesystem::path& pak) {
    auto utoc = pak;
    utoc.replace_extension(L".utoc");
    return !std::filesystem::exists(utoc);
}
std::string NarrowText(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
        nullptr, nullptr);
    return utf8;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

[[nodiscard]] std::string LowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](const char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    return text;
}

[[nodiscard]] bool IsContainerFile(const std::filesystem::path& path) {
    return LowerAscii(path.extension().string()) == kPakExtension;
}

[[nodiscard]] bool IsDisabledName(const std::string& name) {
    return name.size() > kDisabledSuffix.size() &&
        name.compare(name.size() - kDisabledSuffix.size(), kDisabledSuffix.size(),
            kDisabledSuffix) == 0;
}

// A container's identity for change detection: its modification stamp plus its size. A released
// container that keeps both is the same file the user has not replaced yet.
[[nodiscard]] std::uint64_t FileStamp(const std::filesystem::path& path,
    std::uint64_t* bytes) noexcept {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return 0;
    if (bytes != nullptr) {
        const auto size = std::filesystem::file_size(path, error);
        *bytes = error ? 0 : static_cast<std::uint64_t>(size);
    }
    return static_cast<std::uint64_t>(time.time_since_epoch().count());
}

std::string ContainerFingerprint(const std::filesystem::path& pak) {
    std::string result;
    for (const auto* extension : {L".pak", L".utoc", L".ucas", L".sig"}) {
        auto path = pak;
        path.replace_extension(extension);
        result.push_back('/');
        if (!std::filesystem::exists(path)) continue;
        result += std::to_string(std::filesystem::last_write_time(path).time_since_epoch().count());
        result.push_back(':');
        result += std::to_string(std::filesystem::file_size(path));
    }
    return result;
}

[[nodiscard]] std::string_view StateName(const PakModState state) noexcept {
    switch (state) {
    case PakModState::Pending: return "pending";
    case PakModState::Mounted: return "mounted";
    case PakModState::Released: return "released";
    case PakModState::Failed: return "failed";
    }
    return "unknown";
}

}  // namespace

struct PakModRuntime::Impl final {
    enum class SignatureState : std::uint8_t { Unknown, Cleared, Unavailable };
    enum class CacheVerifyState : std::uint8_t { Unknown, Detoured, Unavailable };

    explicit Impl(PakModRuntimeOptions value) : options(std::move(value)) {
        enabled = options.startup_enabled;
        reconcile_pending = !options.compatibility_mode;
        if (options.compatibility_mode) configuration_source = "engine-startup";
        if (options.settings_paths.empty()) options.settings_paths.push_back(ConfigPath());
    }

    ~Impl() { StopWorker(); }

    PakModRuntimeOptions options;
    mutable std::mutex mutex;
    // Hold across planning, engine calls and state publication, including diagnostic commands.
    // A call-only lock leaves stale plans and container pointers alive across an unmount.
    std::mutex operation_mutex;
    std::mutex worker_mutex;
    bool stopped{};
    bool shutdown_complete{};
    std::vector<PakModEntrySnapshot> entries;
    PakModCatalogCache catalog_cache;
    DirectoryChangeMonitor mod_changes, settings_changes, fallback_settings_changes;
    std::map<std::filesystem::path, std::unique_ptr<DirectoryChangeMonitor>> external_changes;
    bool reconcile_pending{true};
    std::uint64_t reconcile_passes{}, mounted_list_reads{};
    // Where one mod begins and ends, as the catalogue keys it. Rebuilt with every desired set and
    // published atomically: the mount paths ask for a key outside the planning pass as well, and a
    // key that named the first folder above a mod would not match a category's own keys.
    std::atomic<std::shared_ptr<const PakModLayout>> layout_;
    // Strong references transferred by Mount, retained for explicit diagnostic reads and released
    // after unmount or shutdown. Engine-owned references have their own lifetime.
    std::map<std::string, std::uintptr_t, std::less<>> containers;
    struct CompatibilityAttachment { std::filesystem::path file; std::uintptr_t container{}; };
    std::map<std::string, CompatibilityAttachment, std::less<>> compatibility_containers;
    std::uintptr_t platform_file{};
    std::uint64_t mounts{};
    std::uint64_t unmounts{};
    std::string last_error;
    std::set<std::string> conflicting_mods;
    // Conflicts the player chose to load anyway, reported for diagnostics and never frozen.
    std::set<std::string> forced_conflicts;
    std::string configuration_source{"scan"};
    bool enabled{true};
    SignatureState signature_state{SignatureState::Unknown};
    CacheVerifyState cache_verify_state{CacheVerifyState::Unknown};
    // The verifier is a hot engine read path. Keep only bounded first-failure evidence;
    // unsigned containers can hit this path thousands of times during ordinary streaming.
    std::set<std::tuple<std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint8_t>> cache_verify_reports;
    static constexpr std::size_t kMaximumVerifyReports = 8;
    // Ids whose mount entry was already announced, so a retry loop cannot flood the log.
    std::set<std::string> announced_mounts;
    mutable std::mutex publish_mutex;
    mutable std::string published_status;
    std::jthread worker;
    AnomalyModsServiceV1 service{sizeof(service), 1, this, Mode, Enumerate, SetEnabled, Assets, Synchronize};

    static AnomalyStatusV1 ANOMALY_CALL Synchronize(void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        if (self.options.compatibility_mode) return Error(ANOMALY_STATUS_V1_CONFLICT);
        try {
            self.Apply();
            std::scoped_lock operation(self.operation_mutex);
            if (self.stopped) return Error(ANOMALY_STATUS_V1_UNAVAILABLE);
            {
                std::scoped_lock lock(self.mutex);
                if (!self.last_error.empty()) return Error(ANOMALY_STATUS_V1_UNAVAILABLE);
            }
            const auto desired = self.Desired();
            std::scoped_lock lock(self.mutex);
            std::size_t mounted{};
            for (const auto& entry : self.entries) if (entry.state == PakModState::Mounted) {
                ++mounted;
                if (std::none_of(desired.begin(), desired.end(), [&](const auto& want) {
                    return want.id == entry.id && want.file == entry.file && want.order == entry.order && want.fingerprint == entry.fingerprint;
                })) return Error(ANOMALY_STATUS_V1_UNAVAILABLE);
            }
            return Error(mounted == desired.size() ? 0 : ANOMALY_STATUS_V1_UNAVAILABLE);
        } catch (...) { return Error(ANOMALY_STATUS_V1_FAILED); }
    }

    static std::string Copy(AnomalyStringViewV1 value) {
        if ((!value.data && value.size) || value.size > 65536) throw std::runtime_error("invalid mod key");
        std::string result(value.data ? value.data : "", value.size);
        if (result.find('\0') != std::string::npos) throw std::runtime_error("invalid mod key");
        return result;
    }
    static AnomalyStatusV1 Error(std::uint32_t code) { return {code, 0, {}}; }
    static std::uint32_t ANOMALY_CALL Mode(void* user) noexcept { return static_cast<Impl*>(user)->options.compatibility_mode ? 1u : 0u; }
    std::shared_ptr<const std::vector<PakModCatalogEntry>> Catalog() {
        std::scoped_lock lock(operation_mutex);
        if (stopped) throw std::runtime_error("mod service stopped");
        return catalog_cache.Read(options.mod_directory, ReadSettings());
    }
    static AnomalyStatusV1 ANOMALY_CALL Enumerate(void* user, AnomalyModVisitorV1 visitor, void* visitor_user) noexcept {
        if (!visitor) return Error(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        try {
            const auto catalog = static_cast<Impl*>(user)->Catalog();
            for (const auto& row : *catalog) {
                if (row.restricted) continue;
                std::string vehicles, groups;
                for (const auto& vehicle : row.vehicles) { if (!vehicles.empty()) vehicles += ", "; vehicles += vehicle; }
                for (const auto& group : row.resource_groups) { if (!groups.empty()) groups += ", "; groups += group; }
                const auto view = [](const std::string& s) { return AnomalyStringViewV1{s.data(), s.size()}; };
                const AnomalyModEntryV1 entry{sizeof(entry), row.enabled, row.disabled_files, row.index_complete,
                    view(row.key), view(row.name), view(vehicles), view(row.analysis_error), view(groups)};
                if (!visitor(visitor_user, &entry)) break;
            }
            return Error(0);
        } catch (...) { return Error(ANOMALY_STATUS_V1_FAILED); }
    }
    static AnomalyStatusV1 Change(Impl& self, const std::string& key, bool enabled) {
        std::scoped_lock lock(self.operation_mutex);
        if (self.stopped) return Error(ANOMALY_STATUS_V1_UNAVAILABLE);
        if (self.options.compatibility_mode) return Error(ANOMALY_STATUS_V1_CONFLICT);
        UpdatePakModSettings(self.options.settings_paths, [&](auto& document) {
            if (document.empty()) document = self.ReadSettings();
            if (PakModCompatibilityMode(document)) throw std::runtime_error("mode changed; restart required");
            const auto catalog = self.catalog_cache.Read(self.options.mod_directory, document);
            const auto selected = std::find_if(catalog->begin(), catalog->end(), [&](const auto& row) { return row.key == key; });
            if (selected == catalog->end()) throw std::runtime_error("mod no longer exists");
            if (enabled && selected->disabled_files) throw std::runtime_error("restore disabled filenames in Launcher first");
            SelectPakMod(document, *catalog, key, enabled);
        });
        return Error(0);
    }
    static AnomalyStatusV1 ANOMALY_CALL SetEnabled(void* user, AnomalyStringViewV1 key, std::uint32_t enabled) noexcept {
        if (enabled > 1) return Error(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        try { return Change(*static_cast<Impl*>(user), Copy(key), enabled != 0); }
        catch (...) { return Error(ANOMALY_STATUS_V1_FAILED); }
    }
    static AnomalyStatusV1 ANOMALY_CALL Assets(void* user, AnomalyStringViewV1 key, AnomalyModAssetVisitorV1 visitor, void* data) noexcept {
        if (!visitor) return Error(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        try {
            const auto catalog = static_cast<Impl*>(user)->Catalog(); const auto id = Copy(key);
            for (const auto& row : *catalog) if (row.key == id && !row.restricted) {
                if (!row.index_complete) return Error(ANOMALY_STATUS_V1_UNAVAILABLE);
                for (const auto& asset : row.assets) if (asset.starts_with("/game/") && (asset.ends_with(".uasset") || asset.ends_with(".umap"))) {
                    const auto path = asset.substr(0, asset.find_last_of('.'));
                    if (!visitor(data, {path.data(), path.size()})) break;
                }
                return Error(0);
            }
            return Error(ANOMALY_STATUS_V1_NOT_FOUND);
        } catch (...) { return Error(ANOMALY_STATUS_V1_FAILED); }
    }

    void Log(const LogLevel level, std::string_view message) noexcept {
        if (!options.log) return;
        try {
            options.log(level, message);
        } catch (...) {
        }
    }

    [[nodiscard]] std::filesystem::path ConfigPath() const {
        return options.settings_paths.empty() ? options.runtime_root / L"config" / kPakModConfigName
                                             : options.settings_paths.front();
    }

    nlohmann::json ReadSettings() const {
        return ReadPakModSettings(std::filesystem::exists(ConfigPath())
            ? ConfigPath().parent_path().parent_path() : options.runtime_root);
    }

    [[nodiscard]] std::filesystem::path StatusPath() const {
        return options.runtime_root / L"state" / kPakModStatusName;
    }

    // The engine keeps a mounted container open, so a replacement can only land after the unmount
    // below. Resolving lazily also keeps this usable before the platform file chain exists.
    [[nodiscard]] std::uintptr_t Engine() {
        if (platform_file != 0) return platform_file;
        if (!options.resolve_pak_platform_file) return 0;
        const auto resolved = options.resolve_pak_platform_file();
        std::scoped_lock lock(mutex);
        platform_file = resolved;
        return platform_file;
    }

    // ------------------------------------------------------------------ desired set

    [[nodiscard]] std::uint32_t DefaultOrder(std::uint32_t index) const {
        if (index > (std::numeric_limits<std::uint32_t>::max)() - options.base_order)
            throw std::runtime_error("default pak order overflow");
        return options.base_order + index;
    }

    void CollectDirectory(std::vector<DesiredPak>& desired) const {
        std::error_code error;
        if (!std::filesystem::exists(options.mod_directory)) return;
        std::vector<std::filesystem::path> files;
        for (auto it = std::filesystem::recursive_directory_iterator(options.mod_directory);
             it != std::filesystem::recursive_directory_iterator{}; ++it) {
            const auto& item = *it;
            const auto attributes = GetFileAttributesW(item.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                throw std::runtime_error("cannot inspect mod path: " + Narrow(item.path()));
            }
            if (IsDisabledName(LowerAscii(Narrow(item.path().filename()))) ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                it.disable_recursion_pending();
                continue;
            }
            if (!item.is_regular_file()) continue;
            if (!IsContainerFile(item.path())) continue;
            if (files.size() >= kMaximumScanFiles) throw std::runtime_error("too many pak files");
            files.push_back(item.path());
        }
        std::sort(files.begin(), files.end());
        std::uint32_t index{};
        for (const auto& file : files) {
            DesiredPak entry{};
            entry.stamp = FileStamp(file, &entry.bytes);
            if (entry.stamp == 0) continue;
            entry.id = DefaultId(file);
            entry.file = std::filesystem::absolute(file).lexically_normal();
            entry.fingerprint = ContainerFingerprint(entry.file);
            entry.order = DefaultOrder(index);
            desired.push_back(std::move(entry));
            ++index;
        }
    }

    [[nodiscard]] std::string DefaultId(const std::filesystem::path& file) const {
        auto path = std::filesystem::absolute(file).lexically_normal().lexically_relative(
            std::filesystem::absolute(options.mod_directory).lexically_normal());
        path.replace_extension();
        auto id = Narrow(path);
        std::replace(id.begin(), id.end(), '\\', '/');
        return id;
    }

    [[nodiscard]] std::string ModKey(const std::filesystem::path& file) const {
        auto root = EnginePathKey(std::filesystem::absolute(options.mod_directory));
        if (!root.ends_with(L'/')) root += L'/';
        const auto path = EnginePathKey(file);
        if (!path.starts_with(root)) return {};
        const auto relative = NarrowText(path.substr(root.size()));
        std::string logical;
        for (std::size_t begin = 0; begin <= relative.size();) {
            auto end = relative.find('/', begin);
            if (end == std::string::npos) end = relative.size();
            auto name = relative.substr(begin, end - begin);
            if (!logical.empty()) logical += '/';
            if (IsDisabledName(LowerAscii(name))) name.resize(name.size() - 9);
            logical += name;
            begin = end + 1;
        }
        // A container in the mods directory is a mod of its own and keys by its own file name, which
        // is how the catalogue spells it.
        const auto loose = [&] {
            const auto dot = logical.rfind('.');
            return dot == std::string::npos ? logical : logical.substr(0, dot) + ".pak";
        };
        const auto layout = layout_.load();
        if (!layout) {
            const auto slash = logical.find('/');
            return slash == std::string::npos ? loose() : logical.substr(0, slash + 1);
        }
        const auto folder = layout->Root(logical);
        return folder.empty() ? loose() : folder + "/";
    }

    bool Restricted(const std::filesystem::path& file, const std::vector<PakModCatalogEntry>& catalog) const {
        // Match physical paths with Windows casing; ASCII selection keys cannot represent every
        // case-equivalent Unicode spelling accepted by an explicit manifest or CLI path.
        for (const auto& row : catalog)
            for (const auto& container : row.containers)
                if (CompareStringOrdinal(container.c_str(), -1, file.c_str(), -1, TRUE) == CSTR_EQUAL)
                    return row.restricted;
        if (ModKey(file).empty()) return PakModFileRestricted(file);
        throw std::runtime_error("container is absent from the mod catalog");
    }

    // One walk of the mods tree per planned set, keyed the way the catalogue keys it. A folder that
    // only groups mods is a category on both sides, so a mod filed in one keeps its own key instead
    // of the folder's, and a selection stored for either still reaches every container under it.
    std::set<std::string> BuildLayout() {
        auto layout = std::make_shared<PakModLayout>();
        std::vector<std::string> logical_containers;
        const auto root = std::filesystem::absolute(options.mod_directory).lexically_normal();
        std::error_code error;
        if (std::filesystem::is_directory(root, error)) {
            std::size_t scanned{};
            for (auto it = std::filesystem::recursive_directory_iterator(root, error);
                 it != std::filesystem::recursive_directory_iterator{}; ++it) {
                if (++scanned > 20000) throw std::runtime_error("mod directory exceeds scan limit");
                const auto attributes = GetFileAttributesW(it->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) continue;
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file()) continue;
                std::string logical;
                for (const auto& part : it->path().lexically_relative(root)) {
                    if (!logical.empty()) logical += '/';
                    auto wide = part.wstring();
                    std::transform(wide.begin(), wide.end(), wide.begin(),
                        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    auto name = NarrowText(wide);
                    if (IsDisabledName(LowerAscii(name))) name.resize(name.size() - 9);
                    logical += name;
                }
                const auto extension = std::filesystem::u8path(logical).extension();
                if (extension == L".pak" || extension == L".utoc" || extension == L".ucas" ||
                    extension == L".sig" || PakModShippedFile(logical))
                    layout->AddOwnerFile(logical);
                if (extension == L".pak" || extension == L".utoc") logical_containers.push_back(std::move(logical));
            }
        }
        // Count distinct logical mods across the complete tree, including disabled files and
        // mods omitted by a manifest. Container count must not turn one multi-pak mod into namesakes.
        std::set<std::string> mod_keys;
        for (const auto& logical : logical_containers) {
            const auto folder = layout->Root(logical);
            mod_keys.insert(folder.empty() ? logical.substr(0, logical.rfind('.')) + ".pak" : folder + "/");
        }
        layout_.store(std::move(layout));
        return mod_keys;
    }

    // `config/pak-mods.json` is optional. When it lists containers it is authoritative, which
    // lets a launcher point at a staged replacement without renaming anything under a live game.
    bool CollectManifest(std::vector<DesiredPak>& desired, const nlohmann::json& parsed) {
        const bool configured_enabled = parsed.value("enabled", true);
        {
            std::scoped_lock lock(mutex);
            enabled = configured_enabled;
        }
        if (!configured_enabled) { external_changes.clear(); return true; }
        const auto mods = parsed.find("mods");
        if (mods == parsed.end()) { external_changes.clear(); return false; }
        if (!mods->is_array()) throw std::runtime_error("mods must be an array");
        if (mods->size() > kMaximumScanFiles) throw std::runtime_error("too many manifest entries");
        {
            std::scoped_lock lock(mutex);
            configuration_source = "manifest";
        }
        auto mod_root = EnginePathKey(std::filesystem::absolute(options.mod_directory));
        if (!mod_root.ends_with(L'/')) mod_root += L'/';
        std::set<std::filesystem::path> external_directories;
        std::uint32_t index{};
        for (const auto& item : *mods) {
            std::string relative;
            std::string id;
            std::uint32_t order{};
            if (item.is_string()) {
                relative = item.get<std::string>();
            } else if (item.is_object()) {
                relative = item.value("pak", std::string{});
                id = item.value("id", std::string{});
                if (const auto field = item.find("order"); field != item.end()) {
                    if (!field->is_number_integer() || field->get<std::int64_t>() < 0 ||
                        field->get<std::uint64_t>() > (std::numeric_limits<std::uint32_t>::max)()) {
                        throw std::runtime_error("invalid pak order");
                    }
                    order = field->get<std::uint32_t>();
                }
            }
            if (relative.empty()) throw std::runtime_error("manifest entry has no pak path");
            std::filesystem::path file = Widen(relative);
            if (file.is_relative()) file = options.mod_directory / file;
            std::error_code error;
            if (!std::filesystem::is_regular_file(file, error)) {
                throw std::runtime_error("manifest entry is missing: " + Narrow(file));
            }
            if (!IsContainerFile(file)) throw std::runtime_error("manifest entry is not a pak file");
            const auto path = EnginePathKey(std::filesystem::absolute(file));
            if (!path.starts_with(mod_root)) {
                const auto directory = std::filesystem::path(path).parent_path();
                external_directories.insert(directory);
                if (!external_changes.contains(directory)) {
                    auto monitor = std::make_unique<DirectoryChangeMonitor>();
                    // Arm before reading the fingerprint so a concurrent replacement is not lost.
                    static_cast<void>(monitor->Changed(directory));
                    external_changes.emplace(directory, std::move(monitor));
                }
            }
            DesiredPak entry{};
            entry.stamp = FileStamp(file, &entry.bytes);
            if (entry.stamp == 0) throw std::runtime_error("cannot read manifest entry: " + Narrow(file));
            entry.file = std::filesystem::absolute(file).lexically_normal();
            entry.fingerprint = ContainerFingerprint(entry.file);
            entry.id = id.empty() ? DefaultId(file) : std::move(id);
            entry.order = order == 0 ? DefaultOrder(index) : order;
            desired.push_back(std::move(entry));
            ++index;
        }
        // Keep watching excluded manifest entries too: an external replacement can make one eligible.
        std::erase_if(external_changes, [&](const auto& entry) { return !external_directories.contains(entry.first); });
        return true;
    }

    [[nodiscard]] std::vector<DesiredPak> Desired() {
        const auto settings = ReadSettings();
        if (PakModCompatibilityMode(settings) != options.compatibility_mode)
            throw std::runtime_error("pak loading mode changed; restart the game to apply it");
        const auto states = settings.value("modStates", nlohmann::json::object());
        // Validate even when no containers exist; a corrupt selection must never unload the set.
        ValidatePakModStates(states);
        // A patch mod ships the same body as the mod it patches, so the player can ask for the
        // overlapping mounts to be kept: the engine's effective read priority selects shared paths.
        const bool force_load = PakModForceLoad(settings);
        {
            std::scoped_lock lock(mutex);
            enabled = true;
            configuration_source = "scan";
            last_error.clear();
            conflicting_mods.clear();
            forced_conflicts.clear();
        }
        std::vector<DesiredPak> desired;
        // Keyed before the set is planned: the conflict pass compares these keys with the catalogue's
        // and the mount paths need them when a mod is frozen or replaced.
        const auto mod_keys = BuildLayout();
        if (!CollectManifest(desired, settings)) {
            desired.clear();
            CollectDirectory(desired);
        }
        // A selection is stored under the mod's path, so a folder moved by hand leaves it behind.
        // Resolving the leftover entry by mod name is what keeps a switched-off variant off: reading
        // it as enabled would mount it again, and the conflict it was switched off for would return.
        std::map<std::string, std::size_t> mod_names;
        for (const auto& key : mod_keys) ++mod_names[PakModSelectionName(key)];
        std::erase_if(desired, [&](const DesiredPak& entry) {
            auto relative = Narrow(entry.file.lexically_relative(
                std::filesystem::absolute(options.mod_directory).lexically_normal()));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            if (PakModSelectionApplied(states, relative)) return !PakModEnabled(states, relative);
            const auto name = PakModSelectionName(ModKey(entry.file));
            if (const auto inherited = PakModInheritedSelection(states, name, mod_names[name] == 1))
                return !*inherited;
            // No saved entry names this mod at all: a mod is mounted unless it was switched off.
            return false;
        });
        // Validate only the effective mount set, after master switch and explicit manifest filtering.
        // An unselected variant on disk must never prevent disabling all mods.
        if (!enabled) desired.clear();
        std::set<std::string> conflicts;
        if (!desired.empty()) {
            const auto catalog = catalog_cache.Read(options.mod_directory, settings);
            std::erase_if(desired, [&](const auto& pak) {
                return PakModNameRestricted(pak.id) || Restricted(pak.file, *catalog);
            });
            std::set<std::string> selected_keys;
            for (const auto& pak : desired) selected_keys.insert(ModKey(pak.file));
            std::vector<const PakModCatalogEntry*> active;
            for (const auto& row : *catalog) {
                if (selected_keys.contains(NormalizePakModKey(row.key))) active.push_back(&row);
            }
            for (std::size_t i = 0; i < active.size(); ++i)
                for (std::size_t j = i + 1; j < active.size(); ++j)
                    if (PakModsConflict(*active[i], *active[j])) {
                        conflicts.insert(NormalizePakModKey(active[i]->key));
                        conflicts.insert(NormalizePakModKey(active[j]->key));
                    }
        }
        std::set<std::string> ids;
        std::set<std::wstring> paths;
        for (const auto& entry : desired) {
            auto path = entry.file.wstring();
            std::transform(path.begin(), path.end(), path.begin(),
                [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
            if (!ids.insert(entry.id).second || !paths.insert(path).second) {
                throw std::runtime_error("duplicate pak id or path: " + entry.id);
            }
        }
        std::scoped_lock lock(mutex);
        if (force_load) {
            // Keep both containers; the engine's effective read priority selects their shared paths.
            // The set is still reported so the state file says what is being overridden.
            forced_conflicts = std::move(conflicts);
        } else {
            conflicting_mods = std::move(conflicts);
        }
        if (!conflicting_mods.empty()) {
            // Freeze only the conflicting mods at their currently mounted versions. Other
            // selections and file replacements must still reach the engine in this pass.
            std::erase_if(desired, [&](const auto& pak) { return conflicting_mods.contains(ModKey(pak.file)); });
            for (const auto& entry : entries)
                if (entry.state == PakModState::Mounted && conflicting_mods.contains(ModKey(entry.file)))
                    desired.push_back({entry.id, entry.file, entry.order, entry.bytes, entry.stamp, entry.fingerprint});
        }
        if (!enabled) {
            desired.clear();
        }
        return desired;
    }

    // ------------------------------------------------------------------ engine calls

    std::string PrepareMount() {
        const std::uintptr_t engine = Engine();
        if (engine == 0 || !options.mount_pak) return "pak platform file is unavailable";
        EnsureCacheVerify();
        if (options.apply_cache_verify && cache_verify_state != CacheVerifyState::Detoured) {
            return "cache verification support is unavailable";
        }
        if (signature_state != SignatureState::Cleared && options.clear_signature_requirement) {
            signature_state = options.clear_signature_requirement(engine)
                ? SignatureState::Cleared
                : SignatureState::Unavailable;
            Log(signature_state == SignatureState::Cleared
                    ? LogLevel::Info
                    : LogLevel::Warning,
                signature_state == SignatureState::Cleared
                    ? "pak_mods.signature_requirement=cleared"
                    : "pak_mods.signature_requirement=unavailable");
        }
        if (options.clear_signature_requirement && signature_state != SignatureState::Cleared) {
            return "signature support is unavailable";
        }
        return {};
    }

    PakModMountOutcome Mount(const DesiredPak& desired) {
        Ue5PakMountRequest request{};
        request.pak_file = desired.file.wstring();
        request.order = desired.order;
        request.mount_point = options.mount_point;
        request.pak_only = IsPakFileContainer(desired.file);

        {
            // Announced before the engine call: if that call takes the process down, the last line
            // in the log still says which container was being mounted.
            std::scoped_lock lock(mutex);
            if (announced_mounts.insert(desired.id).second) {
                Log(LogLevel::Info,
                    "pak_mods.mount_enter id=" + desired.id +
                        " order=" + std::to_string(desired.order) +
                        (request.pak_only ? " mode=pak" : " mode=container"));
            }
        }
        return options.mount_pak(platform_file, request);
    }

    bool Unmount(const std::filesystem::path& file) {
        const std::uintptr_t engine = Engine();
        if (engine == 0 || !options.unmount_pak) return false;
        return options.unmount_pak(engine, file.wstring());
    }

    bool ReleaseCompatibilityAttachment(std::string_view id) {
        const auto attached = compatibility_containers.find(id);
        if (attached == compatibility_containers.end()) return true;
        if (!Unmount(attached->second.file)) return false;
        if (options.release_container) options.release_container(attached->second.container);
        compatibility_containers.erase(attached);
        return true;
    }

    bool ReleaseOrphanedCompatibility() {
        std::vector<std::string> orphaned;
        for (const auto& [id, attachment] : compatibility_containers) {
            static_cast<void>(attachment);
            if (!containers.contains(id)) orphaned.push_back(id);
        }
        bool complete = true;
        for (const auto& id : orphaned) {
            if (!ReleaseCompatibilityAttachment(id)) complete = false;
            else SetEntryState(id, PakModState::Failed, "compatibility cleanup completed; source container is not mounted");
        }
        return complete;
    }

    // ------------------------------------------------------------------ reconcile

    std::wstring EnginePathKey(const std::filesystem::path& file) const {
        auto path = file;
        if (path.is_relative()) {
            if (options.engine_base_directory.empty()) throw std::runtime_error("engine base directory unavailable");
            path = options.engine_base_directory / path;
        }
        auto key = path.lexically_normal().generic_wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return key;
    }

    void ReleaseUnownedMounts(const std::vector<DesiredPak>& desired) {
        const auto engine = Engine();
        if (!engine || !options.mounted_files || !options.unmount_pak)
            throw std::runtime_error("mounted pak list unavailable");
        ++mounted_list_reads;
        const auto mounted = options.mounted_files(engine);
        if (!mounted) throw std::runtime_error("cannot read mounted pak list");
        auto root = EnginePathKey(std::filesystem::absolute(options.mod_directory));
        if (!root.ends_with(L'/')) root += L'/';
        std::set<std::wstring> requested;
        for (const auto& entry : desired) requested.insert(EnginePathKey(entry.file));
        for (const auto& file : *mounted) {
            // Strong references identify our own entries even when another mount uses an alias
            // of the same filename. Never infer ownership from the path alone.
            if (std::any_of(containers.begin(), containers.end(), [&](const auto& own) {
                    return own.second == file.container;
                })) continue;
            const auto path = EnginePathKey(file.filename);
            if (!path.starts_with(root) && !requested.contains(path)) continue;
            // Startup mounts of a conflicting mod are frozen too. Do not tear them down
            // merely to process an unrelated mod's replacement.
            if (conflicting_mods.contains(ModKey(std::filesystem::path(path)))) continue;
            // Unmount compares the stored filename literally. Use the native spelling, which
            // may be relative, and also remove startup mounts excluded by the saved selection.
            if (!options.unmount_pak(engine, file.filename))
                throw std::runtime_error("cannot release startup pak: " + NarrowText(file.filename));
            { std::scoped_lock lock(mutex); ++unmounts; }
            Log(LogLevel::Info, "pak_mods.startup_unmounted file=" + NarrowText(file.filename));
        }
    }

    [[nodiscard]] std::vector<PakModEntrySnapshot> Mounted() const {
        std::vector<PakModEntrySnapshot> result;
        std::scoped_lock lock(mutex);
        for (const auto& entry : entries) {
            if (entry.state == PakModState::Mounted) result.push_back(entry);
        }
        return result;
    }

    // Normal reads need no diagnostic allocation or runtime lock. Report exceptional attribution
    // only once and cap the session total so logging cannot stall the engine's I/O completion.
    void ReportCacheVerify(const Ue5PakCacheVerifySample& sample) noexcept {
        if (sample.element == 1) return;
        try {
            {
                std::scoped_lock lock(mutex);
                if (cache_verify_reports.size() >= kMaximumVerifyReports ||
                    !cache_verify_reports.insert({sample.cache, sample.container, sample.index, sample.element}).second) return;
            }
            const std::string_view state = sample.element == 0   ? std::string_view{"null"}
                : sample.element == 1                            ? std::string_view{"set"}
                                                                 : std::string_view{"unreadable"};
            char text[256]{};
            const int written = std::snprintf(text, sizeof(text),
                "pak_mods.cache_verify id=0x%llX container=0x%llX handle=0x%llX index=%u "
                "tag=0x%08X element=%.*s request=%u",
                static_cast<unsigned long long>(sample.cache),
                static_cast<unsigned long long>(sample.container),
                static_cast<unsigned long long>(sample.handle), sample.index, sample.tag,
                static_cast<int>(state.size()), state.data(), sample.request);
            if (written <= 0) return;
            Log(LogLevel::Warning,
                std::string_view(text,
                    (std::min)(static_cast<std::size_t>(written), sizeof(text) - 1)));
        } catch (...) {
        }
    }

    static void __fastcall CacheVerifyThunk(
        void* context, const Ue5PakCacheVerifySample& sample) noexcept {
        static_cast<Impl*>(context)->ReportCacheVerify(sample);
    }

    // Installs before any mount; missing support blocks new mounts and is retried after Profile startup.
    void EnsureCacheVerify() {
        if (cache_verify_state == CacheVerifyState::Detoured) return;
        if (!options.apply_cache_verify) return;
        std::uintptr_t target{};
        const Ue5PakCacheVerifyStatus status =
            options.apply_cache_verify(&Impl::CacheVerifyThunk, this, target);
        const bool detoured = status == Ue5PakCacheVerifyStatus::Detoured ||
            status == Ue5PakCacheVerifyStatus::AlreadyInstalled;
        // The Profile may not be ready on the first poll. Retry unavailable bindings.
        const auto previous = cache_verify_state;
        cache_verify_state = detoured ? CacheVerifyState::Detoured : CacheVerifyState::Unavailable;
        if (previous == cache_verify_state) return;
        std::string line{"pak_mods.cache_verify="};
        line += status == Ue5PakCacheVerifyStatus::Detoured     ? "detoured"
            : status == Ue5PakCacheVerifyStatus::AlreadyInstalled ? "already"
                                                                  : "unavailable";
        if (detoured && target != 0) {
            char address[32]{};
            std::snprintf(address, sizeof(address), " target=0x%llX",
                static_cast<unsigned long long>(target));
            line += address;
        }
        Log(detoured ? LogLevel::Info : LogLevel::Warning, line);
    }

    void Apply(bool force = true) {
        std::scoped_lock operation_lock(operation_mutex);
        if (stopped) return;
        // Consume each source independently, even on a forced pass. Nothing in the mounted set can
        // change on an ordinary idle poll, so avoid discovery, conflict checks and the engine pak lock.
        bool changed = mod_changes.Changed(options.mod_directory);
        changed = settings_changes.Changed(ConfigPath().parent_path()) || changed;
        const auto fallback = options.runtime_root / L"config";
        if (fallback != ConfigPath().parent_path())
            changed = fallback_settings_changes.Changed(fallback) || changed;
        for (auto& [directory, monitor] : external_changes)
            changed = monitor->Changed(directory) || changed;
        if (!force && !changed && !reconcile_pending) return;
        reconcile_pending = true;
        ++reconcile_passes;
        if (!ReleaseOrphanedCompatibility()) {
            { std::scoped_lock lock(mutex); last_error = "compatibility overlay unmount is still pending"; }
            PublishStatus();
            return;
        }
        if (options.compatibility_mode) {
            PublishStatus();
            return;
        }
        // Before the first mount of the session, exactly like the signature requirement: the detour has
        // to be in place before the engine can schedule a verification for a container we mounted.
        std::vector<DesiredPak> desired;
        try {
            desired = Desired();
        } catch (const std::exception& error) {
            {
                std::scoped_lock lock(mutex);
                last_error = error.what();
            }
            PublishStatus();
            return;
        }
        try {
            // Prepare before adopting a working startup mount, so unavailable signature/cache
            // support cannot remove it and then prevent its replacement from mounting.
            if (!desired.empty()) {
                const auto error = PrepareMount();
                if (!error.empty()) throw std::runtime_error(error);
            }
        } catch (const std::exception& error) {
            { std::scoped_lock lock(mutex); last_error = error.what(); }
            PublishStatus();
            return;
        }
        std::vector<PakModEntrySnapshot> releases;
        std::vector<DesiredPak> to_mount;
        {
            std::scoped_lock lock(mutex);
            const auto entry_for = [&](std::string_view id) -> PakModEntrySnapshot* {
                for (auto& entry : entries) {
                    if (entry.id == id) return &entry;
                }
                return nullptr;
            };
            for (const auto& candidate : desired) {
                PakModEntrySnapshot* existing = entry_for(candidate.id);
                const bool same_file = existing != nullptr &&
                    existing->file == candidate.file && existing->stamp == candidate.stamp &&
                    existing->bytes == candidate.bytes && existing->order == candidate.order &&
                    existing->fingerprint == candidate.fingerprint;
                if (same_file && (existing->state == PakModState::Mounted ||
                                     existing->state == PakModState::Released ||
                                     (existing->native_fault && !force))) {
                    continue;
                }
                if (existing != nullptr && existing->state == PakModState::Mounted) {
                    releases.push_back(*existing);
                }
                to_mount.push_back(candidate);
            }
            for (const auto& entry : entries) {
                if (entry.state != PakModState::Mounted) continue;
                const bool wanted = std::any_of(desired.begin(), desired.end(),
                    [&](const DesiredPak& candidate) { return candidate.id == entry.id; });
                if (!wanted) releases.push_back(entry);
            }
        }
        std::map<std::string, Ue5ModCompatibilityPlan> compatibility_plans;
        try {
            if (options.prepare_compatibility) for (const auto& candidate : to_mount)
                compatibility_plans.emplace(candidate.id, options.prepare_compatibility(candidate.file));
            // A failed preflight keeps the currently mounted set intact.
            ReleaseUnownedMounts(desired);
        } catch (const std::exception& error) {
            { std::scoped_lock lock(mutex); last_error = error.what(); }
            Log(LogLevel::Warning, "pak_mods.compatibility_refused reason=" + std::string(error.what()));
            PublishStatus(); return;
        }
        bool release_failed{};
        for (const auto& entry : releases) {
            if (!ReleaseEntry(entry)) {
                release_failed = true;
            }
        }
        for (const auto& candidate : to_mount) {
            // A selection may replace A with B even when their ids and filenames differ. If A
            // cannot be removed, mounting B would leave both conflicting variants active.
            if (!release_failed) {
                MountEntry(candidate, compatibility_plans[candidate.id]);
                if (compatibility_containers.contains(candidate.id) && !containers.contains(candidate.id)) break;
            }
        }
        PruneRemoved(desired);
        // Ordinary failures retry; native faults require a changed file or an explicit operation.
        reconcile_pending = release_failed || std::any_of(compatibility_containers.begin(), compatibility_containers.end(),
            [&](const auto& entry) { return !containers.contains(entry.first); });
        {
            std::scoped_lock lock(mutex);
            reconcile_pending = reconcile_pending || std::any_of(entries.begin(), entries.end(),
                [](const auto& entry) { return entry.state == PakModState::Failed && !entry.native_fault; });
        }
        PublishStatus();
    }

    void SetEntryState(std::string_view id, const PakModState state, std::string message) {
        std::scoped_lock lock(mutex);
        for (auto& entry : entries) {
            if (entry.id != id) continue;
            entry.state = state;
            entry.message = std::move(message);
            return;
        }
    }

    bool ReleaseEntry(const PakModEntrySnapshot& entry) {
        const auto found = containers.find(entry.id);
        const bool source_present = found != containers.end();
        const bool unmounted = !source_present || Unmount(entry.file);
        if (unmounted && source_present) {
            if (found != containers.end() && options.release_container) {
                options.release_container(found->second);
            }
            std::scoped_lock lock(mutex);
            ++unmounts;
            // The container object the mount produced is gone with the unmount, so nothing may keep
            // pointing at it.
            containers.erase(entry.id);
        }
        // Keep the compatibility overlay visible until the unsafe source has left
        // the dispatcher. The same async-load barrier applies to both containers.
        const bool compatibility_released = unmounted && ReleaseCompatibilityAttachment(entry.id);
        // Keep ownership if the engine refuses or the unload safety barrier is unavailable;
        // reporting the entry as released would silently leave a mounted container behind.
        SetEntryState(entry.id,
            unmounted && compatibility_released ? PakModState::Released : PakModState::Mounted,
            unmounted && compatibility_released ? std::string{} : std::string{"engine refused unmount or async-load barrier unavailable"});
        // Failures are retried on every scan, so only a state transition is worth a log line.
        if (entry.state == PakModState::Mounted &&
            (unmounted || entry.message != "engine refused unmount or async-load barrier unavailable")) {
            Log(unmounted ? LogLevel::Info : LogLevel::Warning,
                unmounted ? "pak_mods.unmounted id=" + entry.id
                          : "pak_mods.unmount_failed id=" + entry.id);
        }
        return unmounted && compatibility_released;
    }

    void MountEntry(const DesiredPak& desired, const Ue5ModCompatibilityPlan& compatibility = {}) {
        PakModMountOutcome outcome;
        if (!ReleaseCompatibilityAttachment(desired.id)) {
            SetEntryState(desired.id, PakModState::Failed, "previous compatibility overlay is still mounted");
            { std::scoped_lock lock(mutex); last_error = "previous compatibility overlay is still mounted"; }
            return;
        }
        if (!compatibility.overlay.empty()) {
            Ue5PakMountRequest request;
            request.pak_file = compatibility.overlay.wstring();
            // FPakPlatformFile and the package store use uint32, but the IoStore backend
            // sorts int32. UINT32_MAX becomes -1 there and exposes the unsafe source bytes.
            request.order = static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
            const auto overlay = options.mount_pak(platform_file, request);
            if (overlay.mounted) {
                compatibility_containers.insert_or_assign(desired.id, CompatibilityAttachment{compatibility.overlay, overlay.container});
                outcome = Mount(desired);
                if (outcome.mounted) {
                    outcome.detail = "retained current game serialization definitions and dependencies";
                    for (const auto& path : compatibility.retained_packages) {
                        outcome.detail += " " + path;
                        Log(LogLevel::Warning, "pak_mods.schema_retained id=" + desired.id + " package=" + path);
                    }
                } else if (!ReleaseCompatibilityAttachment(desired.id))
                    outcome.detail += "; compatibility overlay cleanup is pending";
            } else {
                outcome.detail = "compatibility overlay mount failed: " + overlay.detail;
                outcome.native_fault = overlay.native_fault;
            }
        } else outcome = Mount(desired);
        const bool mounted = outcome.mounted;
        std::string detail = outcome.detail.empty()
            ? std::string{"engine rejected the container"}
            : "engine rejected the container: " + outcome.detail;
        PakModState previous_state{PakModState::Pending};
        std::string previous_message;
        {
            std::scoped_lock lock(mutex);
            PakModEntrySnapshot* existing{};
            for (auto& entry : entries) {
                if (entry.id == desired.id) existing = &entry;
            }
            if (existing == nullptr) {
                entries.push_back(PakModEntrySnapshot{});
                existing = &entries.back();
            }
            previous_state = existing->state;
            previous_message = existing->message;
            existing->id = desired.id;
            existing->file = desired.file;
            existing->order = desired.order;
            existing->bytes = desired.bytes;
            existing->stamp = desired.stamp;
            existing->fingerprint = desired.fingerprint;
            existing->state = mounted ? PakModState::Mounted : PakModState::Failed;
            existing->native_fault = outcome.native_fault;
            existing->message = mounted ? outcome.detail : detail;
            if (mounted) {
                ++mounts;
                containers.insert_or_assign(desired.id, outcome.container);
            } else {
                containers.erase(desired.id);
                last_error = "mount failed: " + Narrow(desired.file) + " (" + detail + ')';
            }
            if (previous_state == existing->state && previous_message == existing->message) {
                return;  // A retried attempt that ends in the same place is not news.
            }
        }
        if (mounted) {
            Log(LogLevel::Info, "pak_mods.mounted id=" + desired.id + " order=" +
                    std::to_string(desired.order));
        } else {
            Log(LogLevel::Warning, "pak_mods.mount_failed id=" + desired.id + " reason=" + detail);
        }
    }

    void PruneRemoved(const std::vector<DesiredPak>& desired) {
        std::scoped_lock lock(mutex);
        std::erase_if(entries, [&](const PakModEntrySnapshot& entry) {
            if (entry.state == PakModState::Mounted) return false;
            if (std::any_of(desired.begin(), desired.end(),
                    [&](const DesiredPak& candidate) { return candidate.id == entry.id; })) {
                return false;
            }
            containers.erase(entry.id);
            return true;
        });
    }

    // ------------------------------------------------------------------ status

    [[nodiscard]] nlohmann::json SnapshotJson() const {
        nlohmann::json result = nlohmann::json::object();
        std::scoped_lock lock(mutex);
        result["schemaVersion"] = 1;
        result["pid"] = GetCurrentProcessId();
        result["mode"] = options.compatibility_mode ? "compatibility" : "hot";
        result["residentAssetsReloaded"] = false;
        result["enabled"] = enabled;
        result["available"] = platform_file != 0;
        result["source"] = configuration_source;
        result["mods"] = nlohmann::json::array();
        for (const auto& entry : entries) {
            nlohmann::json item = nlohmann::json::object();
            item["id"] = entry.id;
            item["file"] = Narrow(entry.file);
            item["order"] = entry.order;
            item["bytes"] = entry.bytes;
            item["state"] = std::string(StateName(entry.state));
            if (!entry.message.empty()) item["message"] = entry.message;
            if (entry.native_fault) item["nativeFault"] = true;
            result["mods"].push_back(std::move(item));
        }
        result["mounts"] = mounts;
        result["unmounts"] = unmounts;
        if (!last_error.empty()) result["lastError"] = last_error;
        if (!forced_conflicts.empty()) result["forcedConflicts"] = forced_conflicts;
        if (!conflicting_mods.empty()) {
            result["conflicts"] = conflicting_mods;
            if (last_error.empty()) {
                std::string names;
                for (const auto& key : conflicting_mods) {
                    if (!names.empty()) names += " / ";
                    names += key;
                }
                result["lastError"] = "conflicting enabled mods: " + names;
            }
        }
        return result;
    }

    void PublishStatus() const noexcept {
        try {
            const std::string text = SnapshotJson().dump(2) + "\n";
            // Keep the temporary file and rename in the same critical section; cache only success.
            std::scoped_lock lock(publish_mutex);
            if (text == published_status) return;
            const auto path = StatusPath();
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            const auto temporary = path.parent_path() / (path.filename().wstring() + L".tmp-" +
                std::to_wstring(GetCurrentProcessId()));
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) return;
                output << text;
                output.flush();
                if (!output) {
                    output.close();
                    static_cast<void>(DeleteFileW(temporary.c_str()));
                    return;
                }
            }
            if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) published_status = text;
        } catch (...) {
        }
    }

    // ------------------------------------------------------------------ commands

    [[nodiscard]] static nlohmann::json Failure(std::string message) {
        nlohmann::json result = nlohmann::json::object();
        result["ok"] = false;
        result["error"] = std::move(message);
        return result;
    }

    [[nodiscard]] std::string ExecuteRelease(const std::string& id) {
        std::optional<PakModEntrySnapshot> target;
        {
            std::scoped_lock lock(mutex);
            for (const auto& entry : entries) {
                if (entry.id == id && entry.state == PakModState::Mounted) target = entry;
            }
        }
        if (!target) return Failure("no mounted container with that id").dump();
        if (!ReleaseEntry(*target)) {
            PublishStatus();
            return Failure("engine refused to unmount the container").dump();
        }
        PublishStatus();
        return SnapshotJson().dump();
    }

    [[nodiscard]] std::string ExecuteMount(const std::string& relative, std::uint32_t order) {
        std::filesystem::path file = Widen(relative);
        if (file.is_relative()) file = options.mod_directory / file;
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) {
            return Failure("container is missing: " + Narrow(file)).dump();
        }
        DesiredPak desired{};
        desired.stamp = FileStamp(file, &desired.bytes);
        if (!IsContainerFile(file)) return Failure("container must be a pak file").dump();
        desired.id = DefaultId(file);
        desired.file = std::filesystem::absolute(file).lexically_normal();
        static_cast<void>(BuildLayout());
        const auto catalog = catalog_cache.Read(options.mod_directory, ReadSettings());
        if (Restricted(desired.file, *catalog)) return Failure("mod is restricted").dump();
        if (!ReleaseOrphanedCompatibility()) return Failure("compatibility overlay cleanup is still pending").dump();
        desired.fingerprint = ContainerFingerprint(desired.file);
        desired.order = order == 0 ? options.base_order : order;
        if (const auto failure = PrepareMount(); !failure.empty()) return Failure(failure).dump();
        const auto compatibility = options.prepare_compatibility ? options.prepare_compatibility(desired.file) : Ue5ModCompatibilityPlan{};
        ReleaseUnownedMounts({desired});
        // Replacing a mounted container needs the release first; the engine would otherwise hold
        // two containers under one path.
        for (const auto& entry : Mounted()) {
            if ((entry.id == desired.id || entry.file == desired.file) && !ReleaseEntry(entry)) {
                return Failure("engine refused to unmount the previous container").dump();
            }
        }
        MountEntry(desired, compatibility);
        PublishStatus();
        return SnapshotJson().dump();
    }

    // Re-runs the post-mount priming read for one mounted entry. Diagnostic: it reports the stage and
    // changes no state, so it can be repeated from the pipe while the game runs.
    [[nodiscard]] std::string ExecutePrime(const std::string& id) {
        std::uintptr_t container{};
        {
            std::scoped_lock lock(mutex);
            const auto found = containers.find(id);
            if (found == containers.end()) {
                return Failure("no mounted container with that id").dump();
            }
            container = found->second;
        }
        if (!options.prime_pak) return Failure("pak priming is unavailable").dump();
        const std::uintptr_t engine = Engine();
        if (engine == 0) return Failure("the pak platform file is unavailable").dump();
        const PakModPrimeOutcome outcome = options.prime_pak(engine, container);
        // Same rule as the mount path: "ok" may only appear under the success tag.
        const bool stage_ok = outcome.stage == "ok";
        const bool primed = outcome.primed && stage_ok;
        const std::string stage =
            outcome.primed == stage_ok ? outcome.stage : std::string{"inconsistent-report"};
        nlohmann::json result = nlohmann::json::object();
        result["ok"] = primed;
        result["id"] = id;
        result["stage"] = stage;
        result["detail"] = outcome.detail;
        Log(primed ? LogLevel::Info : LogLevel::Warning,
            std::string(primed ? "pak_mods.primed id=" : "pak_mods.prime_failed id=") + id +
                " stage=" + stage + ' ' + outcome.detail);
        return result.dump();
    }

    void Run(std::stop_token stop) noexcept {
        std::mutex wait_mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            try {
                Apply(false);
            } catch (...) {
                Log(LogLevel::Warning, "pak_mods.scan_failed");
            }
            try {
                const auto interval = (std::max)(options.poll_interval, std::chrono::milliseconds(50));
                std::unique_lock lock(wait_mutex);
                wake.wait_for(lock, stop, interval, [] { return false; });
                if (stop.stop_requested()) break;
            } catch (...) {
                Log(LogLevel::Warning, "pak_mods.watcher_failed");
                return;
            }
        }
    }

    void StopWorker() noexcept {
        if (!worker.joinable()) return;
        worker.request_stop();
        worker.join();
    }

    void UnmountAll() {
        for (const auto& entry : Mounted()) {
            static_cast<void>(ReleaseEntry(entry));
        }
        static_cast<void>(ReleaseOrphanedCompatibility());
        PublishStatus();
    }
};

PakModRuntime::PakModRuntime(PakModRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PakModRuntime::~PakModRuntime() { Stop(); }

bool PakModRuntime::Start() {
    std::scoped_lock worker_lock(impl_->worker_mutex);
    std::scoped_lock operation_lock(impl_->operation_mutex);
    if (impl_->stopped) return false;
    if (impl_->options.compatibility_mode) {
        impl_->PublishStatus();
        return true;
    }
    if (impl_->worker.joinable()) return true;
    impl_->worker =
        std::jthread([impl = impl_.get()](std::stop_token stop) { impl->Run(stop); });
    return impl_->worker.joinable();
}

void PakModRuntime::Stop() noexcept {
    std::scoped_lock worker_lock(impl_->worker_mutex);
    if (impl_->shutdown_complete) return;
    try {
        {
            std::scoped_lock lock(impl_->operation_mutex);
            impl_->stopped = true;
        }
        impl_->StopWorker();
        std::scoped_lock lock(impl_->operation_mutex);
        impl_->UnmountAll();
    } catch (...) {
        impl_->Log(LogLevel::Warning, "pak_mods.stop_failed");
    }
    // A failed unmount still has the engine's ownership. Drop our additional output reference
    // after stopping diagnostics instead of leaking it when the session map is destroyed.
    try {
        if (impl_->options.remove_cache_verify &&
            impl_->cache_verify_state == Impl::CacheVerifyState::Detoured)
            impl_->options.remove_cache_verify();
        std::scoped_lock operation_lock(impl_->operation_mutex);
        for (const auto& [id, container] : impl_->containers) {
            if (impl_->options.release_container) impl_->options.release_container(container);
        }
        for (const auto& [id, attachment] : impl_->compatibility_containers) {
            if (impl_->options.release_container) impl_->options.release_container(attachment.container);
        }
        impl_->compatibility_containers.clear();
        std::scoped_lock lock(impl_->mutex);
        impl_->containers.clear();
    } catch (...) {
        impl_->Log(LogLevel::Warning, "pak_mods.cleanup_failed");
    }
    impl_->shutdown_complete = true;
}

PakModRuntimeOptions PakModRuntime::Options() const { return impl_->options; }

std::string PakModRuntime::StatusJson() const { return impl_->SnapshotJson().dump(); }
const AnomalyModsServiceV1* PakModRuntime::Service() const noexcept { return &impl_->service; }

std::string PakModRuntime::Execute(std::string_view arguments) noexcept {
    try {
        std::string_view text = arguments;
        const auto shift = [](std::string_view& input) {
            while (!input.empty() && input.front() == ' ') input.remove_prefix(1);
            if (!input.empty() && input.front() == '"') {
                input.remove_prefix(1);
                const auto end = input.find('"');
                if (end == std::string_view::npos) throw std::runtime_error("unterminated quoted argument");
                std::string token(input.substr(0, end));
                input.remove_prefix(end + 1);
                return token;
            }
            const auto space = input.find(' ');
            if (space == std::string_view::npos) {
                std::string token(input);
                input = {};
                return token;
            }
            std::string token(input.substr(0, space));
            input.remove_prefix(space + 1);
            return token;
        };
        const std::string command = shift(text);
        if (command.empty() || command == "status") return impl_->SnapshotJson().dump();
        if (command == "performance") {
            std::scoped_lock lock(impl_->operation_mutex);
            return nlohmann::json{{"ok", true}, {"reconcilePasses", impl_->reconcile_passes},
                {"mountedListReads", impl_->mounted_list_reads},
                {"catalogMetadataScans", impl_->catalog_cache.MetadataScans()},
                {"retryPending", impl_->reconcile_pending}}.dump();
        }
        if (impl_->options.compatibility_mode)
            return Impl::Failure("compatibility mode uses engine startup loading; restart required").dump();
        if (command == "reload" || command == "rescan") {
            impl_->Apply();
            return impl_->SnapshotJson().dump();
        }
        std::scoped_lock operation_lock(impl_->operation_mutex);
        if (impl_->stopped) return Impl::Failure("pak runtime is stopped").dump();
        // Explicit diagnostic mutations are reconciled on the next watcher pass as before.
        if (command == "unmountall" || command == "unload" || command == "release" ||
            command == "unmount" || command == "mount") impl_->reconcile_pending = true;
        if (command == "unmountall" || command == "unload") {
            impl_->UnmountAll();
            return impl_->SnapshotJson().dump();
        }
        const std::string name = shift(text);
        if (name.empty()) return Impl::Failure("missing container name").dump();
        if (command == "release" || command == "unmount") return impl_->ExecuteRelease(name);
        // Re-runs the priming read for one mounted container without touching the mount itself, so
        // the stage can be re-checked after a fix without restarting the game.
        if (command == "prime") return impl_->ExecutePrime(name);
        if (command == "mount") {
            std::uint32_t order{};
            const std::string order_text = shift(text);
            if (!order_text.empty()) {
                const auto [end, error] = std::from_chars(
                    order_text.data(), order_text.data() + order_text.size(), order);
                if (error != std::errc{} || end != order_text.data() + order_text.size())
                    return Impl::Failure("invalid pak order").dump();
            }
            return impl_->ExecuteMount(name, order);
        }
        return Impl::Failure("unknown pak command").dump();
    } catch (...) {
        return "{\"ok\":false,\"error\":\"pak command failed\"}";
    }
}

}  // namespace anomaly
