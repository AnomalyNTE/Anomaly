#include "anomaly/pak_mod_runtime.hpp"
#include "anomaly/pak_mod_settings.hpp"
#include "anomaly/pak_mod_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
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
    // Strong references transferred by Mount, retained for explicit diagnostic reads and released
    // after unmount or shutdown. Engine-owned references have their own lifetime.
    std::map<std::string, std::uintptr_t, std::less<>> containers;
    std::uintptr_t platform_file{};
    std::uint64_t mounts{};
    std::uint64_t unmounts{};
    std::string last_error;
    std::set<std::string> conflicting_mods;
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
            for (const auto& row : *catalog) if (row.key == id) {
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
        const auto slash = relative.find('/');
        return slash == std::string::npos ? relative : relative.substr(0, slash + 1);
    }

    // `config/pak-mods.json` is optional. When it lists containers it is authoritative, which
    // lets a launcher point at a staged replacement without renaming anything under a live game.
    bool CollectManifest(std::vector<DesiredPak>& desired, const nlohmann::json& parsed) {
        const bool configured_enabled = parsed.value("enabled", true);
        {
            std::scoped_lock lock(mutex);
            enabled = configured_enabled;
        }
        if (!configured_enabled) return true;
        const auto mods = parsed.find("mods");
        if (mods == parsed.end()) return false;
        if (!mods->is_array()) throw std::runtime_error("mods must be an array");
        if (mods->size() > kMaximumScanFiles) throw std::runtime_error("too many manifest entries");
        {
            std::scoped_lock lock(mutex);
            configuration_source = "manifest";
        }
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
        return true;
    }

    [[nodiscard]] std::vector<DesiredPak> Desired() {
        const auto settings = ReadSettings();
        if (PakModCompatibilityMode(settings) != options.compatibility_mode)
            throw std::runtime_error("pak loading mode changed; restart the game to apply it");
        const auto states = settings.value("modStates", nlohmann::json::object());
        // Validate even when no containers exist; a corrupt selection must never unload the set.
        ValidatePakModStates(states);
        {
            std::scoped_lock lock(mutex);
            enabled = true;
            configuration_source = "scan";
            last_error.clear();
            conflicting_mods.clear();
        }
        std::vector<DesiredPak> desired;
        if (!CollectManifest(desired, settings)) {
            desired.clear();
            CollectDirectory(desired);
        }
        std::erase_if(desired, [&](const DesiredPak& entry) {
            auto relative = Narrow(entry.file.lexically_relative(
                std::filesystem::absolute(options.mod_directory).lexically_normal()));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            return !PakModEnabled(states, relative);
        });
        // Validate only the effective mount set, after master switch and explicit manifest filtering.
        // An unselected variant on disk must never prevent disabling all mods.
        if (!enabled) desired.clear();
        std::set<std::string> conflicts;
        if (desired.size() > 1) {
            const auto catalog = catalog_cache.Read(options.mod_directory, settings);
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
        conflicting_mods = std::move(conflicts);
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

    void Apply() {
        std::scoped_lock operation_lock(operation_mutex);
        if (stopped) return;
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
            ReleaseUnownedMounts(desired);
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
                                     existing->state == PakModState::Released)) {
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
        bool release_failed{};
        for (const auto& entry : releases) {
            if (!ReleaseEntry(entry)) {
                release_failed = true;
            }
        }
        for (const auto& candidate : to_mount) {
            // A selection may replace A with B even when their ids and filenames differ. If A
            // cannot be removed, mounting B would leave both conflicting variants active.
            if (!release_failed) MountEntry(candidate);
        }
        PruneRemoved(desired);
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
        const bool unmounted = Unmount(entry.file);
        if (unmounted) {
            const auto found = containers.find(entry.id);
            if (found != containers.end() && options.release_container) {
                options.release_container(found->second);
            }
            std::scoped_lock lock(mutex);
            ++unmounts;
            // The container object the mount produced is gone with the unmount, so nothing may keep
            // pointing at it.
            containers.erase(entry.id);
        }
        // An engine refusal means the container was not mounted under that exact path; reporting
        // it as released would silently keep a file locked.
        SetEntryState(entry.id,
            unmounted ? PakModState::Released : PakModState::Mounted,
            unmounted ? std::string{} : std::string{"engine reported no mounted container"});
        // Failures are retried on every scan, so only a state transition is worth a log line.
        if (entry.state == PakModState::Mounted &&
            (unmounted || entry.message != "engine reported no mounted container")) {
            Log(unmounted ? LogLevel::Info : LogLevel::Warning,
                unmounted ? "pak_mods.unmounted id=" + entry.id
                          : "pak_mods.unmount_failed id=" + entry.id);
        }
        return unmounted;
    }

    void MountEntry(const DesiredPak& desired) {
        PakModMountOutcome outcome = Mount(desired);
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
            existing->message = mounted ? std::string{} : detail;
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
            result["mods"].push_back(std::move(item));
        }
        result["mounts"] = mounts;
        result["unmounts"] = unmounts;
        if (!last_error.empty()) result["lastError"] = last_error;
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
        desired.fingerprint = ContainerFingerprint(desired.file);
        desired.order = order == 0 ? options.base_order : order;
        if (const auto failure = PrepareMount(); !failure.empty()) return Failure(failure).dump();
        ReleaseUnownedMounts({desired});
        // Replacing a mounted container needs the release first; the engine would otherwise hold
        // two containers under one path.
        for (const auto& entry : Mounted()) {
            if ((entry.id == desired.id || entry.file == desired.file) && !ReleaseEntry(entry)) {
                return Failure("engine refused to unmount the previous container").dump();
            }
        }
        MountEntry(desired);
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
        while (!stop.stop_requested()) {
            try {
                Apply();
            } catch (...) {
                Log(LogLevel::Warning, "pak_mods.scan_failed");
            }
            try {
                const auto interval = (std::max)(options.poll_interval, std::chrono::milliseconds(50));
                const auto deadline = std::chrono::steady_clock::now() + interval;
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
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
        if (impl_->options.compatibility_mode)
            return Impl::Failure("compatibility mode uses engine startup loading; restart required").dump();
        if (command == "reload" || command == "rescan") {
            impl_->Apply();
            return impl_->SnapshotJson().dump();
        }
        std::scoped_lock operation_lock(impl_->operation_mutex);
        if (impl_->stopped) return Impl::Failure("pak runtime is stopped").dump();
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
