#include "avatar_images.hpp"
#include "account_archive.hpp"
#include "game_bridge.hpp"

#include <anomaly/sdk/cpp.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace equipment_manager {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::optional<std::vector<std::uint8_t>> ReadCompletedPng(const std::filesystem::path& path) {
    // The engine creates the output before its asynchronous write finishes.
    // Only publish a bounded PNG whose final IEND chunk has already arrived.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto size = input.tellg();
    if (size < 45 || size > 512 * 1024) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) return std::nullopt;
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    constexpr std::array<std::uint8_t, 12> end{0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    if (!std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        !std::equal(end.begin(), end.end(), bytes.end() - end.size())) return std::nullopt;
    return bytes;
}

std::string HeadPath(const Json& character) {
    for (const auto* key : {"head_icon_big_asset_path", "head_icon_asset_path"})
        if (character.contains(key) && character.at(key).is_string() &&
            !character.at(key).get_ref<const std::string&>().empty()) return character.at(key).get<std::string>();
    return {};
}

std::string PngDataUrl(std::span<const std::uint8_t> bytes) {
    std::string result = "data:image/png;base64,";
    result.reserve(result.size() + (bytes.size() + 2) / 3 * 4);
    // Reuse the existing bounded encoder. Full blocks are multiples of three
    // so only the final block can contain Base64 padding.
    constexpr auto block_size = kAccountTransferChunk / 3 * 3;
    for (std::size_t offset{}; offset < bytes.size(); offset += block_size)
        result += EncodeAccountChunk(bytes.subspan(offset, (std::min)(block_size, bytes.size() - offset)));
    return result;
}

std::string CacheKey(std::string_view source) {
    // Rendering contract, not a game build identity. A color-space correction
    // must not reuse an image produced by the old linear export path.
    std::string identity = "avatar-srgb-rgba8-256-v1\n";
    identity.append(source);
    for (auto& value : identity) if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(identity.data()), static_cast<ULONG>(identity.size()),
        digest.data(), static_cast<ULONG>(digest.size())) < 0) throw std::runtime_error("Avatar cache hashing failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (const auto value : digest) { key += hex[value >> 4]; key += hex[value & 15]; }
    return key;
}

Json Describe(std::string_view role_id, const Json& character) {
    const auto source = HeadPath(character);
    const auto fashion = character.contains("fashion_id") && character.at("fashion_id").is_string()
        ? character.at("fashion_id").get<std::string>() : std::string{};
    return {{"role_id", role_id}, {"source_asset_path", source.empty() ? Json(nullptr) : Json(source)},
        {"source_fingerprint", source.empty() ? Json(nullptr) : Json(CacheKey(source + '\n' + fashion))},
        {"fashion_id", fashion.empty() ? Json(nullptr) : Json(fashion)}};
}
} // namespace

struct AvatarImages::Impl {
    struct Read {
        std::string role, source, status{"queued"}, data_uri;
        std::filesystem::path path, cache_path;
        Clock::time_point started{}, last_attempt{};
        bool cache_checked{}, cache_hit{}, released{};
    };
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyCoreServiceV1* core{};
    std::filesystem::path directory;
    std::string prefix;
    std::mutex mutex;
    // Only Worker callers use this mutex. Source-index file I/O cannot block
    // the mutex shared with the Game callback's image export mailbox.
    std::mutex sources_mutex;
    std::map<std::string, Json, std::less<>> sources;
    std::map<std::string, std::shared_ptr<Read>, std::less<>> reads;
    AnomalyGenerationHandleV1 task{};
    std::uint64_t serial{};
    bool stopping{}, worker_running{};

    std::filesystem::path SourcesPath(std::string_view account) const {
        return directory / std::filesystem::path("sources-" + CacheKey(account) + ".json");
    }
    Json& SourcesLocked(std::string_view account) {
        const auto found = sources.find(account);
        if (found != sources.end()) return found->second;
        Json characters = Json::object();
        if (!account.empty()) {
            std::ifstream input(SourcesPath(account), std::ios::binary | std::ios::ate);
            if (input) {
                const auto size = input.tellg();
                if (size > 0 && size <= 512 * 1024) {
                    std::string text(static_cast<std::size_t>(size), '\0');
                    input.seekg(0); input.read(text.data(), static_cast<std::streamsize>(size));
                    const auto document = input ? Json::parse(text, nullptr, false) : Json();
                    if (document.is_object() && document.contains("schema_version") && document.at("schema_version") == 1 &&
                        document.contains("characters") && document.at("characters").is_object())
                        characters = document.at("characters");
                }
            }
        }
        return sources.emplace(std::string(account), std::move(characters)).first->second;
    }
    Json SourceCharacter(std::string_view account, std::string_view role, const Json& fallback) {
        std::scoped_lock lock(sources_mutex);
        const auto& characters = SourcesLocked(account);
        const auto found = characters.find(role);
        return found != characters.end() && found->is_object() && !HeadPath(*found).empty() ? *found : fallback;
    }

    void Log(std::string_view message) const noexcept {
        if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
    }
    void ScheduleLocked() {
        if (stopping || task.id || worker_running || !std::any_of(reads.begin(), reads.end(), [](const auto& entry) {
            return !entry.second->cache_checked || entry.second->status == "exporting";
        })) return;
        if (scheduler->schedule(scheduler->user, 100, Poll, this, &task).code != ANOMALY_STATUS_V1_OK) {
            task = {};
            for (auto& [role, read] : reads) if (!read->cache_checked || read->status == "exporting") {
                read->cache_checked = true; read->status = "failed";
            }
            Log("EquipmentManager: avatar PNG polling could not be scheduled");
        }
    }
    static void ANOMALY_CALL Poll(void* user, AnomalyGenerationHandleV1) noexcept {
        auto& self = *static_cast<Impl*>(user);
        try {
            std::vector<std::shared_ptr<Read>> pending;
            {
                std::scoped_lock lock(self.mutex); self.task = {};
                if (self.stopping) return;
                self.worker_running = true;
                for (const auto& [role, read] : self.reads)
                    if (!read->cache_checked || read->status == "exporting") pending.push_back(read);
            }
            for (const auto& read : pending) {
                const bool cache_check = !read->cache_checked;
                const auto png = ReadCompletedPng(cache_check ? read->cache_path : read->path);
                auto uri = png ? PngDataUrl(*png) : std::string{};
                // Only a complete engine output becomes the persistent cache.
                // Unfinished writes from an unloaded generation stay staging
                // files and can never masquerade as a cached avatar.
                if (png && !cache_check && !MoveFileExW(read->path.c_str(), read->cache_path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    std::scoped_lock lock(self.mutex); read->status = "failed";
                    self.Log("EquipmentManager: avatar cache publication failed"); continue;
                }
                std::scoped_lock lock(self.mutex);
                if (self.stopping) break;
                if (cache_check) read->cache_checked = true;
                if (png) {
                    read->data_uri = std::move(uri); read->status = "completed"; read->cache_hit = cache_check;
                    if (cache_check) read->released = true; // No UE request was created for a cache hit.
                }
                else if (cache_check) continue;
                else if (Clock::now() - read->started > std::chrono::seconds(15)) {
                    read->status = "failed";
                    self.Log("EquipmentManager: the engine did not publish a complete avatar PNG");
                }
            }
            std::scoped_lock lock(self.mutex); self.worker_running = false; self.ScheduleLocked();
        } catch (...) {
            std::scoped_lock lock(self.mutex); self.worker_running = false;
            for (auto& [role, read] : self.reads) if (!read->cache_checked || read->status == "exporting") {
                read->cache_checked = true; read->status = "failed";
            }
            self.Log("EquipmentManager: avatar PNG reading failed");
        }
    }
};

AvatarImages::AvatarImages() : impl_(std::make_unique<Impl>()) {}
AvatarImages::~AvatarImages() { Stop(); }

void AvatarImages::Load(const AnomalyHostApiV1* host, std::uint64_t generation) {
    auto& self = *impl_;
    const anomaly::sdk::Host api(host);
    self.scheduler = api.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
    self.core = api.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
    const auto* state = api.Query<AnomalyPluginStateServiceV1>(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID).get();
    if (!self.scheduler || self.scheduler->struct_size < sizeof(*self.scheduler) ||
        !self.scheduler->schedule || !self.scheduler->cancel || !state ||
        state->struct_size < sizeof(*state) || !state->directory) throw std::runtime_error("Avatar services are unavailable");
    std::array<char, 32768> buffer{}; std::size_t size = buffer.size();
    if (state->directory(state->user, buffer.data(), &size).code != ANOMALY_STATUS_V1_OK || size < 2 || size > buffer.size())
        throw std::runtime_error("The avatar cache directory is unavailable");
    self.directory = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buffer.data()), size - 1)) / L"avatar-images";
    std::filesystem::create_directories(self.directory);
    std::array<unsigned char, 8> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("The avatar cache identity is unavailable");
    constexpr char hex[] = "0123456789abcdef";
    self.prefix = std::to_string(generation) + '-';
    for (const auto byte : nonce) { self.prefix += hex[byte >> 4]; self.prefix += hex[byte & 15]; }
}

void AvatarImages::Stop() noexcept {
    auto& self = *impl_; AnomalyGenerationHandleV1 task{};
    {
        std::scoped_lock lock(self.mutex); self.stopping = true; task = std::exchange(self.task, {});
    }
    if (task.id && self.scheduler) self.scheduler->cancel(self.scheduler->user, task);
}

Json AvatarImages::Cached(std::string_view role_id, const Json& character, std::string_view account_binding) {
    auto& self = *impl_;
    const auto selected = self.SourceCharacter(account_binding, role_id, character);
    const auto source = HeadPath(selected);
    auto result = Describe(role_id, selected);
    result.update({{"status", source.empty() ? "unavailable" : "missing"}, {"cache_hit", false}, {"avatar_url", nullptr}});
    if (source.empty()) return result;
    std::shared_ptr<const Impl::Read> completed;
    {
        std::scoped_lock lock(self.mutex);
        if (self.stopping) { result["status"] = "unavailable"; return result; }
        const auto found = self.reads.find(source);
        if (found != self.reads.end() && found->second->status == "completed") completed = found->second;
    }
    // A completed read is immutable. Keep its lease while copying the image
    // outside the mutex used by the Game callback.
    if (completed) result["avatar_url"] = completed->data_uri;
    else {
        const auto png = ReadCompletedPng(self.directory / std::filesystem::path("srgb-256-" + CacheKey(source) + ".png"));
        if (!png) return result;
        result["avatar_url"] = PngDataUrl(*png);
    }
    result["status"] = "completed"; result["cache_hit"] = true;
    return result;
}

Json AvatarImages::Request(std::string_view role_id, const Json& character, std::string_view account_binding, bool allow_export) {
    if (!allow_export) {
        auto result = Cached(role_id, character, account_binding);
        if (result.at("status") != "completed") result["status"] = "unavailable";
        return result;
    }
    auto& self = *impl_;
    const auto selected = self.SourceCharacter(account_binding, role_id, character);
    const auto source = HeadPath(selected);
    auto result = Describe(role_id, selected);
    std::scoped_lock lock(self.mutex);
    if (self.stopping || source.empty()) {
        result.update({{"status", "unavailable"}, {"avatar_url", nullptr}}); return result;
    }
    auto& read = self.reads[source];
    if (!read) {
        read = std::make_shared<Impl::Read>(); read->role = role_id; read->source = source;
        const auto key = CacheKey(source);
        read->cache_path = self.directory / std::filesystem::path("srgb-256-" + key + ".png");
        read->path = self.directory / std::filesystem::path(key + '-' + self.prefix + '-' + std::to_string(++self.serial) + ".pending.png");
    }
    self.ScheduleLocked();
    result.update({{"status", read->status}, {"cache_hit", read->cache_hit},
        {"avatar_url", read->data_uri.empty() ? Json(nullptr) : Json(read->data_uri)}});
    return result;
}

Json AvatarImages::PublishSources(std::string_view account_binding, const Json& characters) {
    auto& self = *impl_;
    std::scoped_lock lock(self.sources_mutex);
    auto& stored = self.SourcesLocked(account_binding);
    auto updated = stored;
    Json result = Json::array();
    for (const auto& character : characters) {
        const auto& role = character.at("item_id").get_ref<const std::string&>();
        if (!character.value("appearance_complete", false) || HeadPath(character).empty()) continue;
        // Keep only derived image metadata, never game handles or raw account UID.
        Json source = Json::object();
        for (const auto* key : {"fashion_id", "head_icon_asset_path", "head_icon_big_asset_path"})
            if (character.contains(key)) source[key] = character.at(key);
        updated[role] = std::move(source);
        result.push_back(Describe(role, updated.at(role)));
    }
    if (updated != stored && !account_binding.empty()) {
        const auto path = self.SourcesPath(account_binding);
        auto pending = path; pending += std::filesystem::path('.' + self.prefix + ".pending");
        const auto document = Json{{"schema_version", 1}, {"characters", updated}}.dump();
        {
            std::ofstream output(pending, std::ios::binary | std::ios::trunc);
            output.write(document.data(), static_cast<std::streamsize>(document.size()));
            output.flush();
            if (!output) throw std::runtime_error("Avatar source cache writing failed");
        }
        if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Avatar source cache publication failed");
        stored = std::move(updated);
    }
    return result;
}

void AvatarImages::TickGame(GameBridge& game) {
    auto& self = *impl_; std::shared_ptr<Impl::Read> next;
    std::vector<std::filesystem::path> release;
    {
        std::scoped_lock lock(self.mutex);
        if (self.stopping) return;
        for (const auto& [role, read] : self.reads) {
            if (!read->released && (read->status == "completed" || read->status == "failed")) {
                read->released = true; release.push_back(read->path);
            }
            if (read->cache_checked && (read->status == "queued" || read->status == "loading") &&
                (!next || read->last_attempt < next->last_attempt)) next = read;
        }
        if (next) {
            if (next->started == Clock::time_point{}) next->started = Clock::now();
            next->last_attempt = Clock::now();
        }
    }
    for (const auto& path : release) game.ReleaseAvatar(path.native());
    if (!next) return;
    if (Clock::now() - next->started > std::chrono::seconds(15)) {
        std::scoped_lock lock(self.mutex); next->status = "failed";
        self.Log("EquipmentManager: the requested avatar texture did not become available"); return;
    }
    std::string error;
    const auto state = game.ExportAvatar(next->role, next->source, next->path.native(), error);
    std::scoped_lock lock(self.mutex);
    if (state == AvatarExportResult::exported) { next->status = "exporting"; self.ScheduleLocked(); }
    else if (state == AvatarExportResult::waiting) next->status = "loading";
    else { next->status = "failed"; self.Log("EquipmentManager avatar: " + error); }
}
} // namespace equipment_manager
