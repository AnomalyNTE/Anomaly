#include "anomaly/pak_mod_catalog.hpp"
#include "anomaly/reliable_storage.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace anomaly {
namespace {
constexpr std::size_t MaxIndex = 64 * 1024 * 1024;
constexpr std::uint32_t MaxEntries = 1000000, Invalid = 0xffffffffu;
struct IndexedAsset {
    std::string path, fingerprint;
};
struct ContainerIndex {
    std::vector<IndexedAsset> assets;
    std::map<std::string, std::string> chunks;
};
void MergeFingerprint(std::map<std::string, std::string>& values,
    const std::string& key, const std::string& fingerprint) {
    const auto [it, inserted] = values.try_emplace(key, fingerprint);
    if (!inserted && it->second != fingerprint) it->second.clear();
}
std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
        static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("invalid UTF-16 index string");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::string PathText(const std::filesystem::path& p) { return Utf8(p.generic_wstring()); }
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t pos{};
    void Skip(std::uint64_t n) {
        if (n > data.size() - pos) throw std::runtime_error("truncated container index");
        pos += static_cast<std::size_t>(n);
    }
    template<class T> T Get() {
        T value{}; const auto old = pos; Skip(sizeof(T));
        std::memcpy(&value, data.data() + old, sizeof(T)); return value;
    }
    template<class T> T At(std::size_t at) const { Reader r{data}; r.Skip(at); return r.Get<T>(); }
    std::uint32_t Count() {
        const auto n = Get<std::uint32_t>();
        if (n > MaxEntries) throw std::runtime_error("container index count exceeds limit");
        return n;
    }
    std::string String() {
        const auto n = Get<std::int32_t>();
        if (n == 0) return {};
        if (n > 65536 || n < -65536) throw std::runtime_error("invalid index string length");
        const auto old = pos;
        if (n > 0) {
            Skip(n);
            if (data[pos - 1]) throw std::runtime_error("unterminated index string");
            std::string out(reinterpret_cast<const char*>(data.data() + old), n - 1);
            if (out.find('\0') != std::string::npos) throw std::runtime_error("embedded NUL in index string");
            return out;
        }
        Skip(static_cast<std::uint64_t>(-n) * 2);
        std::wstring wide(static_cast<std::size_t>(-n), 0);
        std::memcpy(wide.data(), data.data() + old, wide.size() * 2);
        if (wide.back()) throw std::runtime_error("unterminated wide index string");
        wide.pop_back();
        if (wide.find(L'\0') != std::wstring::npos) throw std::runtime_error("embedded NUL in index string");
        return Utf8(wide);
    }
};
std::vector<std::uint8_t> ReadRange(const std::filesystem::path& path, std::uint64_t at, std::uint64_t size) {
    const auto length = std::filesystem::file_size(path);
    if (size > MaxIndex || at > length || size > length - at) throw std::runtime_error("invalid container index range");
    std::ifstream in(path, std::ios::binary);
    if (!in || !in.seekg(static_cast<std::streamoff>(at))) throw std::runtime_error("cannot open container index");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read container index");
    return bytes;
}
ContainerIndex ReadUtoc(const std::filesystem::path& path) {
    // On-disk FIoStoreTocHeader and FIoDirectoryIndexResource, UE 5.6.1 Core/IO.
    const auto header = ReadRange(path, 0, 144); Reader h{header};
    constexpr char magic[] = "-==--==--==--==-";
    if (std::memcmp(header.data(), magic, 16)) throw std::runtime_error("invalid IoStore magic");
    const auto version = h.At<std::uint8_t>(16), flags = h.At<std::uint8_t>(80);
    if (version < 2 || version > 8 || h.At<std::uint32_t>(20) != 144 || h.At<std::uint32_t>(32) != 12)
        throw std::runtime_error("unsupported IoStore header");
    if (flags & 2) throw std::runtime_error("encrypted IoStore index");
    if (!(flags & 8)) throw std::runtime_error("IoStore directory index absent");
    const auto entries = h.At<std::uint32_t>(24), blocks = h.At<std::uint32_t>(28);
    if (entries > MaxEntries) throw std::runtime_error("too many IoStore chunks");
    const auto index_size = h.At<std::uint32_t>(48);
    std::uint64_t offset = 144ull + std::uint64_t(entries) * 22 + std::uint64_t(blocks) * 12 +
        std::uint64_t(h.At<std::uint32_t>(36)) * h.At<std::uint32_t>(40);
    if (version >= 4) offset += std::uint64_t(h.At<std::uint32_t>(84)) * 4;
    if (version >= 5) offset += std::uint64_t(h.At<std::uint32_t>(96)) * 4;
    if (flags & 4) {
        const auto length = ReadRange(path, offset, 4); Reader sign{length};
        offset += 4ull + std::uint64_t(sign.Count()) * 2 + std::uint64_t(blocks) * 20;
    }
    if (!index_size) {
        if (entries) throw std::runtime_error("IoStore directory index absent");
        return {};
    }
    const auto chunk_data = ReadRange(path, 144, std::uint64_t(entries) * 22);
    // UE 5.6.1 IoStore.cpp: v8 stores FIoHash (20 bytes) in a 24-byte meta record;
    // older versions store a 32-byte hash in a 33-byte record. Never mix hash formats.
    const std::size_t hash_size = version >= 8 ? 20 : 32, meta_size = version >= 8 ? 24 : 33;
    const auto metadata = ReadRange(path, offset + index_size, std::uint64_t(entries) * meta_size);
    std::vector<std::string> fingerprints(entries);
    ContainerIndex result;
    for (std::uint32_t i = 0; i < entries; ++i) {
        const auto* hash = metadata.data() + std::size_t(i) * meta_size;
        auto& fingerprint = fingerprints[i];
        if (std::any_of(hash, hash + hash_size, [](auto byte) { return byte != 0; })) {
            fingerprint.assign(reinterpret_cast<const char*>(hash), hash_size);
            // Include the source length, not compressed size or physical placement.
            fingerprint.append(reinterpret_cast<const char*>(chunk_data.data() +
                std::size_t(entries) * 12 + std::size_t(i) * 10 + 5), 5);
        }
        const auto* id = chunk_data.data() + std::size_t(i) * 12;
        // ContainerHeader is container metadata, not a replaceable game resource. Include
        // all other chunks, including bulk data absent from the directory index.
        if (id[11] != 6) MergeFingerprint(result.chunks,
            std::string(reinterpret_cast<const char*>(id), 12), fingerprint);
    }
    const auto bytes = ReadRange(path, offset, index_size); Reader r{bytes};
    const auto mount = r.String();
    using Dir = std::array<std::uint32_t, 4>; using File = std::array<std::uint32_t, 3>;
    const auto nd = r.Count();
    if (nd > (r.data.size() - r.pos) / sizeof(Dir)) throw std::runtime_error("truncated directories");
    std::vector<Dir> dirs; dirs.reserve(nd);
    for (std::uint32_t i = 0; i < nd; ++i) dirs.push_back(r.Get<Dir>());
    const auto nf = r.Count();
    if (nf > (r.data.size() - r.pos) / sizeof(File)) throw std::runtime_error("truncated files");
    std::vector<File> files; files.reserve(nf);
    for (std::uint32_t i = 0; i < nf; ++i) files.push_back(r.Get<File>());
    const auto ns = r.Count();
    if (ns > (r.data.size() - r.pos) / 4) throw std::runtime_error("truncated strings");
    std::vector<std::string> strings; strings.reserve(ns);
    for (std::uint32_t i = 0; i < ns; ++i) strings.push_back(r.String());
    if (dirs.empty()) { if (nf) throw std::runtime_error("files without root directory"); return result; }
    std::vector<std::pair<std::uint32_t, std::string>> pending{{0, mount}};
    std::vector<bool> seen_dirs(nd), seen_files(nf);
    std::size_t expanded_bytes = mount.size();
    const auto account = [&](std::size_t size) {
        if (size > MaxIndex - expanded_bytes) throw std::runtime_error("expanded index exceeds limit");
        expanded_bytes += size;
    };
    while (!pending.empty()) {
        auto [id, parent] = std::move(pending.back()); pending.pop_back();
        if (id >= nd || seen_dirs[id]) throw std::runtime_error("cyclic or invalid directory index");
        seen_dirs[id] = true; const auto& dir = dirs[id];
        if (dir[2] != Invalid) { account(parent.size()); pending.emplace_back(dir[2], parent); }
        if (id != 0) {
            if (dir[0] >= ns) throw std::runtime_error("invalid directory name index");
            parent += strings[dir[0]] + '/';
        }
        if (parent.size() > 65536) throw std::runtime_error("directory path exceeds limit");
        if (dir[1] != Invalid) { account(parent.size()); pending.emplace_back(dir[1], parent); }
        for (auto f = dir[3]; f != Invalid; f = files[f][1]) {
            if (f >= nf || seen_files[f]) throw std::runtime_error("cyclic or invalid file index");
            seen_files[f] = true;
            if (files[f][0] >= ns || files[f][2] >= entries) throw std::runtime_error("invalid file index entry");
            account(parent.size() + strings[files[f][0]].size());
            result.assets.push_back({parent + strings[files[f][0]], fingerprints[files[f][2]]});
        }
    }
    if (std::find(seen_dirs.begin(), seen_dirs.end(), false) != seen_dirs.end() ||
        std::find(seen_files.begin(), seen_files.end(), false) != seen_files.end())
        throw std::runtime_error("unreachable file index entries");
    return result;
}
std::vector<std::string> ReadPak(const std::filesystem::path& path) {
    const auto length = std::filesystem::file_size(path);
    // Current mod producers use pak v8-v11. Unknown/frozen formats are not guessed.
    for (const std::uint32_t expected : {11u, 10u, 8u}) {
        constexpr std::size_t footer_size = 221;
        if (length < footer_size) break;
        const auto footer = ReadRange(path, length - footer_size, footer_size); Reader f{footer};
        if (f.At<std::uint32_t>(17) != 0x5a6f12e1 || f.At<std::uint32_t>(21) != expected) continue;
        if (f.At<std::uint8_t>(16)) throw std::runtime_error("encrypted pak index");
        const auto bytes = ReadRange(path, f.At<std::uint64_t>(25), f.At<std::uint64_t>(33)); Reader r{bytes};
        const auto mount = r.String(); const auto count = r.Count();
        std::vector<std::string> result;
        std::size_t expanded_bytes{};
        const auto append = [&](std::string name) {
            if (name.size() > MaxIndex - expanded_bytes) throw std::runtime_error("expanded pak index exceeds limit");
            expanded_bytes += name.size(); result.push_back(std::move(name));
        };
        if (expected == 8) {
            for (std::uint32_t i = 0; i < count; ++i) {
                append(mount + r.String()); r.Skip(24);
                const auto compression = r.Get<std::uint32_t>(); r.Skip(20);
                if (compression) r.Skip(std::uint64_t(r.Count()) * 16);
                r.Skip(5);
            }
            return result;
        }
        r.Skip(8); // PathHashSeed
        if (r.Get<std::uint32_t>()) r.Skip(36);
        if (!r.Get<std::uint32_t>()) {
            if (!count) return {};
            throw std::runtime_error("pak full directory index absent");
        }
        const auto at = r.Get<std::uint64_t>(), size = r.Get<std::uint64_t>();
        const auto directory = ReadRange(path, at, size); Reader d{directory};
        const auto nd = d.Count();
        for (std::uint32_t i = 0; i < nd; ++i) {
            const auto parent = d.String(); const auto nf = d.Count();
            if (nf > MaxEntries - result.size()) throw std::runtime_error("too many pak files");
            for (std::uint32_t j = 0; j < nf; ++j) {
                const auto name = d.String(); const auto location = d.Get<std::int32_t>();
                // Deleted entries also override this path and therefore conflict with replacements.
                static_cast<void>(location);
                append(mount + parent + name);
            }
        }
        return result;
    }
    throw std::runtime_error("unsupported pak footer");
}
ContainerIndex ReadIndex(const std::filesystem::path& file) {
    auto logical = file;
    if (NormalizePakModKey(PathText(logical.extension())) == ".disabled") logical.replace_extension();
    if (NormalizePakModKey(PathText(logical.extension())) == ".utoc") return ReadUtoc(file);
    ContainerIndex result;
    for (auto& path : ReadPak(file)) result.assets.push_back({std::move(path), {}});
    return result;
}
std::string VehicleToken(std::string_view path) {
    const auto at = path.find("vehicle_"); if (at == std::string_view::npos) return {};
    auto end = at + 8;
    while (end < path.size() && path[end] >= '0' && path[end] <= '9') ++end;
    if (end == at + 8 || end > at + 14 || (end < path.size() && path[end] != '/' && path[end] != '_' && path[end] != '.')) return {};
    return "Vehicle_" + std::string(path.substr(at + 8, end - at - 8));
}
void AddAsset(PakModCatalogEntry& row, IndexedAsset asset) {
    auto path = NormalizePakModKey(std::move(asset.path));
    const auto content = path.find("/ht/content/");
    if (content != std::string::npos) path = "/game/" + path.substr(content + 12);
    else {
        // Some IoStore producers write content-relative character paths. Runtime identity
        // still comes from the chunk ID; recognize their directory ownership as well.
        std::string_view relative = path;
        while (relative.starts_with("../")) relative.remove_prefix(3);
        if (relative.starts_with("characters/player/")) path = "/game/" + std::string(relative);
    }
    row.assets.insert(path);
    MergeFingerprint(row.asset_fingerprints, path, asset.fingerprint);
    if (const auto token = VehicleToken(path); !token.empty()) {
        row.vehicles.insert(token);
        if (path.ends_with(".uasset") && (path.find("/carbody/") != std::string::npos ||
            path.find("/blueprints/vehicle/") != std::string::npos)) row.model_vehicles.insert(token);
    }
}
void ClassifyResources(PakModCatalogEntry& row) {
    row.resource_groups = row.vehicles;
    std::set<std::string> directories, players, model_players;
    for (const auto& asset : row.assets) {
        const std::string_view path = asset;
        if (!path.starts_with("/game/")) continue;
        const auto top = path.find('/', 6);
        if (top == std::string_view::npos) continue;
        directories.emplace(path.substr(0, top));

        // Ownership comes from the directory, not texture names borrowed from another character.
        constexpr std::string_view player_root = "/game/characters/player/";
        if (path.starts_with(player_root)) {
            const auto rest = path.substr(player_root.size());
            const auto slash = rest.find('/');
            if (slash == std::string_view::npos) continue;
            const auto owner = rest.substr(0, slash);
            const auto digits = owner.find_first_not_of("0123456789");
            const auto length = digits == std::string_view::npos ? owner.size() : digits;
            if (length > 0 && length <= 6 && (digits == std::string_view::npos || owner[digits] == '_')) {
                const auto group = "Player_" + std::string(owner);
                players.insert(group);
                const auto name = rest.substr(slash + 1);
                if (name.starts_with("player_" + std::string(owner.substr(0, length)) + "_") &&
                    name.ends_with("_skin.uasset")) model_players.insert(group);
            }
            continue;
        }
        constexpr std::string_view pendant = "/game/characters/pendant_";
        if (path.starts_with(pendant)) {
            const auto rest = path.substr(pendant.size());
            const auto slash = rest.find('/');
            if (slash != std::string_view::npos && slash > 0 && slash <= 6 &&
                rest.substr(0, slash).find_first_not_of("0123456789") == std::string_view::npos)
                row.resource_groups.insert("Pendant_" + std::string(rest.substr(0, slash)));
        }
    }
    // Skin meshes identify the primary replacement; shared materials and variant helpers
    // must not add unrelated skins. Texture-only mods retain their full owner directories.
    const auto& character_groups = model_players.empty() ? players : model_players;
    row.resource_groups.insert(character_groups.begin(), character_groups.end());
    // Companion materials and custom folders do not split a known character or vehicle group.
    if (row.resource_groups.empty()) row.resource_groups = std::move(directories);
}
bool Intersects(const std::set<std::string>& a, const std::set<std::string>& b) {
    auto x = a.begin(), y = b.begin();
    while (x != a.end() && y != b.end()) { if (*x == *y) return true; if (*x < *y) ++x; else ++y; }
    return false;
}
bool ConflictingContents(const std::map<std::string, std::string>& a,
    const std::map<std::string, std::string>& b) {
    auto x = a.begin(), y = b.begin();
    while (x != a.end() && y != b.end()) {
        if (x->first == y->first) {
            if (x->second.empty() || y->second.empty() || x->second != y->second) return true;
            ++x; ++y;
        } else if (x->first < y->first) ++x;
        else ++y;
    }
    return false;
}
struct ConfigLock {
    HANDLE handle{};
    explicit ConfigLock(const std::filesystem::path& path) {
        auto key = std::filesystem::absolute(path).lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        std::uint64_t hash = 14695981039346656037ull;
        for (wchar_t c : key) { hash ^= static_cast<std::uint16_t>(c); hash *= 1099511628211ull; }
        const auto name = L"Local\\Anomaly.PakSettings." + std::to_wstring(hash);
        handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle) throw std::runtime_error("cannot create mod settings mutex");
        const auto wait = WaitForSingleObject(handle, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(handle); handle = nullptr; throw std::runtime_error("mod settings busy"); }
    }
    ~ConfigLock() { if (handle) { ReleaseMutex(handle); CloseHandle(handle); } }
};
} // namespace

std::vector<std::string> ReadModContainerIndex(const std::filesystem::path& file) {
    std::vector<std::string> paths;
    for (auto& asset : ReadIndex(file).assets) paths.push_back(std::move(asset.path));
    return paths;
}
std::shared_ptr<const std::vector<PakModCatalogEntry>> PakModCatalogCache::Read(
    const std::filesystem::path& directory, const nlohmann::json& settings) {
    const auto root = std::filesystem::absolute(directory).lexically_normal();
    const auto states = settings.value("modStates", nlohmann::json::object());
    ValidatePakModStates(states);
    decltype(files_) files;
    if (std::filesystem::exists(root)) {
        if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("mod root is a reparse point");
        std::size_t scanned{};
        for (auto it = std::filesystem::recursive_directory_iterator(root);
             it != std::filesystem::recursive_directory_iterator{}; ++it) {
            if (++scanned > 20000) throw std::runtime_error("mod directory exceeds scan limit");
            const auto attributes = GetFileAttributesW(it->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot inspect mod file");
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file()) continue;
            auto logical = it->path();
            if (NormalizePakModKey(PathText(logical.extension())) == ".disabled") logical.replace_extension();
            const auto extension = NormalizePakModKey(PathText(logical.extension()));
            if (extension == ".pak" || extension == ".utoc")
                files.emplace(it->path(), std::pair{it->last_write_time(), it->file_size()});
        }
    }
    if (snapshot_ && directory_ == root && files_ == files && states_ == states) return snapshot_;
    auto snapshot = std::make_shared<const std::vector<PakModCatalogEntry>>(ReadPakModCatalog(root, settings));
    directory_ = root;
    files_ = std::move(files);
    states_ = states;
    snapshot_ = std::move(snapshot);
    return snapshot_;
}
std::vector<PakModCatalogEntry> ReadPakModCatalog(const std::filesystem::path& directory, const nlohmann::json& settings) {
    const auto states = settings.value("modStates", nlohmann::json::object()); ValidatePakModStates(states);
    std::map<std::string, PakModCatalogEntry> found;
    if (!std::filesystem::exists(directory)) return {};
    if (GetFileAttributesW(directory.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("mod root is a reparse point");
    std::size_t scanned{};
    for (auto it = std::filesystem::recursive_directory_iterator(directory); it != std::filesystem::recursive_directory_iterator{}; ++it) {
        if (++scanned > 20000) throw std::runtime_error("mod directory exceeds scan limit");
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot inspect mod file");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file()) continue;
        const auto rel = it->path().lexically_relative(directory);
        std::string logical; bool disabled{};
        for (const auto& part : rel) {
            auto name = PathText(part);
            if (NormalizePakModKey(name).ends_with(".disabled")) { name.resize(name.size() - 9); disabled = true; }
            if (!logical.empty()) logical += '/';
            logical += name;
        }
        const auto extension = NormalizePakModKey(PathText(std::filesystem::u8path(logical).extension()));
        if (extension != ".pak" && extension != ".utoc") continue;
        const auto slash = logical.find('/');
        const auto key = slash == std::string::npos ? logical.substr(0, logical.size() - extension.size()) + ".pak" : logical.substr(0, slash + 1);
        auto& row = found[key]; row.key = key;
        row.name = slash == std::string::npos ? key.substr(0, key.size() - 4) : key.substr(0, key.size() - 1);
        row.disabled_files = row.disabled_files || disabled;
        auto selection_path = logical;
        if (extension == ".utoc") selection_path.replace(selection_path.size() - 5, 5, ".pak");
        if (!disabled && PakModEnabled(states, selection_path)) row.enabled = true;
        try {
            auto index = ReadIndex(it->path());
            for (auto& asset : index.assets) AddAsset(row, std::move(asset));
            for (const auto& [id, fingerprint] : index.chunks)
                MergeFingerprint(row.chunk_fingerprints, id, fingerprint);
        }
        catch (const std::exception& e) { row.index_complete = false; row.analysis_error = e.what(); }
    }
    std::vector<PakModCatalogEntry> result;
    for (auto& [key, row] : found) {
        ClassifyResources(row);
        result.push_back(std::move(row));
    }
    return result;
}
bool PakModsConflict(const PakModCatalogEntry& a, const PakModCatalogEntry& b) {
    return NormalizePakModKey(a.key) != NormalizePakModKey(b.key) &&
        (ConflictingContents(a.asset_fingerprints, b.asset_fingerprints) ||
            ConflictingContents(a.chunk_fingerprints, b.chunk_fingerprints) ||
            Intersects(a.model_vehicles, b.model_vehicles));
}
void SelectPakMod(nlohmann::json& settings, const std::vector<PakModCatalogEntry>& catalog,
    std::string_view key, bool enabled) {
    const auto normalized = NormalizePakModKey(std::string(key));
    const auto selected = std::find_if(catalog.begin(), catalog.end(), [&](const auto& row) { return NormalizePakModKey(row.key) == normalized; });
    if (selected == catalog.end()) throw std::runtime_error("mod no longer exists");
    if (enabled) for (const auto& row : catalog) if (PakModsConflict(*selected, row)) SetPakModEnabled(settings["modStates"], row.key, false);
    SetPakModEnabled(settings["modStates"], selected->key, enabled);
}
void UpdatePakModSettings(const std::vector<std::filesystem::path>& paths,
    const std::function<void(nlohmann::json&)>& update) {
    std::vector<std::filesystem::path> sorted;
    for (const auto& path : paths) {
        auto normalized = std::filesystem::absolute(path).lexically_normal().wstring();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        sorted.emplace_back(normalized);
    }
    std::sort(sorted.begin(), sorted.end()); sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<std::unique_ptr<ConfigLock>> locks;
    for (const auto& path : sorted) locks.push_back(std::make_unique<ConfigLock>(path));
    std::vector<std::pair<std::filesystem::path, std::string>> writes;
    for (const auto& path : sorted) {
        auto settings = ReadPakModSettings(path.parent_path().parent_path());
        update(settings); settings["schemaVersion"] = 1;
        static_cast<void>(PakModCompatibilityMode(settings)); static_cast<void>(settings.value("enabled", true));
        ValidatePakModStates(settings.value("modStates", nlohmann::json::object()));
        writes.emplace_back(path, settings.dump(2) + '\n');
    }
    for (const auto& [path, text] : writes) {
        std::filesystem::create_directories(path.parent_path());
        ReliableStorage storage(path.parent_path().wstring());
        if (!storage.WriteAtomic(path.filename().wstring(), std::as_bytes(std::span(text)))) throw std::runtime_error("cannot save mod settings");
    }
}
} // namespace anomaly
