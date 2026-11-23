#include "recommended_weights.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <utility>

namespace equipment_manager {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxCatalogBytes = 512U * 1024U;
constexpr std::size_t kMaxCharacters = 512U;
constexpr std::size_t kMaxWeightEntries = 128U;
constexpr std::size_t kMaxIdLength = 128U;

constexpr wchar_t kUserAgent[] = L"Anomaly-EquipmentManager/1.0";
constexpr int kResolveTimeoutMs = 2500;
constexpr int kConnectTimeoutMs = 2500;
constexpr int kSendTimeoutMs = 4000;
constexpr int kReceiveTimeoutMs = 4000;

struct PropertyAlias {
    std::string_view upstream;
    std::string_view canonical;
};

// Upstream lowercase keys -> canonical game property IDs used by EquipmentManager.
constexpr std::array<PropertyAlias, 26> kPropertyIdMap{{
    {"atkadd", "AtkAdd"},
    {"atkbase", "AtkBase"},
    {"atkup", "AtkUp"},
    {"chargegetefficiency", "ChargeGetEfficiencyBase"},
    {"chargegetefficiencybase", "ChargeGetEfficiencyBase"},
    {"critbase", "CritBase"},
    {"critdamagebase", "CritDamageBase"},
    {"damageupchaosbase", "DamageUpChaosBase"},
    {"damageupcosmosbase", "DamageUpCosmosBase"},
    {"damageupgeneralbase", "DamageUpGeneralBase"},
    {"damageupincantationbase", "DamageUpIncantationBase"},
    {"damageuplakshanabase", "DamageUpLakshanaBase"},
    {"damageupnaturebase", "DamageUpNatureBase"},
    {"damageuppsychicallybase", "DamageUpPsychicallyBase"},
    {"damageuppsychebase", "DamageUpPsycheBase"},
    {"damageupsychebase", "DamageUpPsycheBase"},
    {"defadd", "DefAdd"},
    {"defbase", "DefBase"},
    {"defup", "DefUp"},
    {"healbeup", "HealBeUp"},
    {"healup", "HealUp"},
    {"hpmaxadd", "HPMaxAdd"},
    {"hpmaxbase", "HPMaxBase"},
    {"hpmaxup", "HPMaxUp"},
    {"magbase", "MagBase"},
    {"unbalintensitybase", "UnbalIntensityBase"},
}};

std::string LowerAscii(std::string_view text) {
    std::string result(text);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return result;
}

std::string_view CanonicalPropertyId(std::string_view raw_key) {
    const std::string lower = LowerAscii(raw_key);
    for (const auto& entry : kPropertyIdMap) {
        if (entry.upstream == lower) return entry.canonical;
    }
    return {};
}

class WinHttpHandle final {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~WinHttpHandle() { Reset(nullptr); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    void Reset(HINTERNET handle) noexcept {
        if (handle_ != nullptr) WinHttpCloseHandle(handle_);
        handle_ = handle;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }

private:
    HINTERNET handle_{};
};

bool Utf8ToWide(std::string_view text, std::wstring& wide) {
    if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return false;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), length) != length) {
        return false;
    }
    wide = std::move(result);
    return true;
}

bool ValidateWeightMap(const Json& value, Json& normalized, bool require_positive, std::string& error) {
    if (!value.is_object() || value.size() > kMaxWeightEntries) {
        error = "Weight map must be an object within size bounds";
        return false;
    }
    normalized = Json::object();
    bool has_positive = false;
    for (const auto& [stat_id, entry] : value.items()) {
        if (stat_id.empty() || stat_id.size() > kMaxIdLength || stat_id.find('\0') != std::string::npos) {
            error = "Weight stat ID is invalid";
            return false;
        }
        if (!entry.is_number()) {
            error = "Weight value is not a number";
            return false;
        }
        const double weight = entry.get<double>();
        if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
            error = "Weight value must be finite and in [0, 1]";
            return false;
        }
        if (weight > 0.0) {
            has_positive = true;
            normalized[stat_id] = entry;
        }
    }
    if (require_positive && !has_positive) {
        error = "Weight map has no positive entries";
        return false;
    }
    return true;
}

bool ReadOptionalWeight(const Json& item, const char* field, double& value, std::string& error) {
    value = 0.0;
    const auto found = item.find(field);
    if (found == item.end() || found->is_null()) return true;
    if (!found->is_number()) {
        error = std::string("Upstream weight field is not numeric: ") + field;
        return false;
    }
    const double numeric = found->get<double>();
    if (!std::isfinite(numeric) || numeric < 0.0 || numeric > 1.0) {
        error = std::string("Upstream weight value out of range: ") + field;
        return false;
    }
    value = numeric;
    return true;
}

bool ParseHttpsUrl(std::string_view url, std::wstring& host, INTERNET_PORT& port,
                   std::wstring& path, std::string& error) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() <= kScheme.size() || url.substr(0, kScheme.size()) != kScheme) {
        error = "Recommended weights URL must use HTTPS";
        return false;
    }
    std::string_view rest = url.substr(kScheme.size());
    if (const auto fragment = rest.find('#'); fragment != std::string_view::npos) {
        rest = rest.substr(0, fragment);
    }
    const auto authority_end = rest.find_first_of("/?");
    const auto authority = rest.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        authority.find('[') != std::string_view::npos || authority.find(']') != std::string_view::npos) {
        error = "Recommended weights URL authority is unsupported";
        return false;
    }
    std::string_view host_part = authority;
    port = INTERNET_DEFAULT_HTTPS_PORT;
    if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host_part = authority.substr(0, colon);
        const auto port_text = authority.substr(colon + 1);
        if (port_text.empty() || port_text.size() > 5) {
            error = "Recommended weights URL port is invalid";
            return false;
        }
        unsigned parsed = 0;
        for (const char ch : port_text) {
            if (ch < '0' || ch > '9') {
                error = "Recommended weights URL port is invalid";
                return false;
            }
            parsed = parsed * 10U + static_cast<unsigned>(ch - '0');
        }
        if (parsed == 0 || parsed > 65535U) {
            error = "Recommended weights URL port is out of range";
            return false;
        }
        port = static_cast<INTERNET_PORT>(parsed);
    }
    if (host_part.empty() || host_part.size() > 253U) {
        error = "Recommended weights URL host is invalid";
        return false;
    }
    std::string path_str = authority_end == std::string_view::npos
                               ? "/"
                               : rest[authority_end] == '?'
                                     ? "/" + std::string(rest.substr(authority_end))
                                     : std::string(rest.substr(authority_end));
    for (const char ch : path_str) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F) {
            error = "Recommended weights URL path contains control characters";
            return false;
        }
    }
    if (!Utf8ToWide(host_part, host) || !Utf8ToWide(path_str, path)) {
        error = "Recommended weights URL is not valid UTF-8";
        return false;
    }
    return true;
}

std::string WinHttpError(std::string_view step) {
    return std::string(step) + " failed (error " + std::to_string(GetLastError()) + ")";
}

}  // namespace

Json DefaultRecommendedWeightsCatalog() {
    const Json general_weights{
        {"AtkUp", 0.7},
        {"CritBase", 1},
        {"CritDamageBase", 1},
        {"DamageUpGeneralBase", 0.75},
    };
    return {
        {"schema_version", 1},
        {"source", "builtin"},
        {"attribution", ""},
        {"generated_at", ""},
        {"general", {{"weights", general_weights}, {"main_weights", general_weights}}},
        {"characters", Json::object()},
    };
}

bool ParseRecommendedWeightsCatalog(std::string_view text, Json& out, std::string& error) {
    if (text.empty() || text.size() > kMaxCatalogBytes) {
        error = "Recommended weights document size is invalid";
        return false;
    }
    const Json parsed = Json::parse(text.begin(), text.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "Recommended weights document is not a JSON object";
        return false;
    }
    if (!parsed.contains("schema_version") || !parsed["schema_version"].is_number_integer() ||
        parsed["schema_version"].get<int>() != 1) {
        error = "Recommended weights schema_version must be 1";
        return false;
    }
    if (!parsed.contains("characters") || !parsed["characters"].is_object() ||
        parsed["characters"].empty() || parsed["characters"].size() > kMaxCharacters) {
        error = "Recommended weights characters table is missing or invalid";
        return false;
    }

    Json result = DefaultRecommendedWeightsCatalog();
    for (const auto* field : {"source", "attribution", "generated_at"}) {
        if (parsed.contains(field)) {
            if (!parsed[field].is_string() || parsed[field].get_ref<const std::string&>().size() > kMaxIdLength) {
                error = std::string("Recommended weights metadata field is invalid: ") + field;
                return false;
            }
            result[field] = parsed[field];
        }
    }

    if (parsed.contains("general")) {
        const auto& general = parsed["general"];
        if (!general.is_object() || !general.contains("weights")) {
            error = "Recommended weights general entry is invalid";
            return false;
        }
        Json sub_weights;
        if (!ValidateWeightMap(general["weights"], sub_weights, true, error)) return false;
        Json main_weights = sub_weights;
        if (general.contains("main_weights")) {
            if (!ValidateWeightMap(general["main_weights"], main_weights, false, error)) return false;
            if (main_weights.empty()) main_weights = sub_weights;
        }
        result["general"] = {{"weights", std::move(sub_weights)}, {"main_weights", std::move(main_weights)}};
    }

    Json characters = Json::object();
    for (const auto& [character_id, entry] : parsed["characters"].items()) {
        if (character_id.empty() || character_id.size() > 64U || character_id.find('\0') != std::string::npos ||
            !entry.is_object() || !entry.contains("weights")) {
            error = "Recommended weights character entry is invalid";
            return false;
        }
        Json sub_weights;
        if (!ValidateWeightMap(entry["weights"], sub_weights, true, error)) return false;
        Json main_weights = sub_weights;
        if (entry.contains("main_weights")) {
            if (!ValidateWeightMap(entry["main_weights"], main_weights, false, error)) return false;
            if (main_weights.empty()) main_weights = sub_weights;
        }
        Json record{
            {"weights", std::move(sub_weights)},
            {"main_weights", std::move(main_weights)},
        };
        if (entry.contains("name")) {
            if (!entry["name"].is_string() || entry["name"].get_ref<const std::string&>().size() > kMaxIdLength) {
                error = "Recommended weights character name is invalid";
                return false;
            }
            record["name"] = entry["name"];
        }
        characters[character_id] = std::move(record);
    }

    result["characters"] = std::move(characters);
    out = std::move(result);
    error.clear();
    return true;
}

bool ParseUpstreamWeightsPayload(std::string_view text, Json& out, std::string& error) {
    if (text.empty() || text.size() > kMaxCatalogBytes) {
        error = "Upstream weights payload size is invalid";
        return false;
    }
    const Json parsed = Json::parse(text.begin(), text.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "Upstream weights payload is not a JSON object";
        return false;
    }
    if (!parsed.contains("data") || !parsed["data"].is_array() ||
        parsed["data"].empty() || parsed["data"].size() > kMaxCharacters) {
        error = "Upstream weights payload has no character data";
        return false;
    }

    Json characters = Json::object();
    for (const auto& entry : parsed["data"]) {
        if (!entry.is_object() || !entry.contains("itemId")) continue;
        std::string character_id;
        if (entry["itemId"].is_string()) {
            character_id = entry["itemId"].get<std::string>();
        } else if (entry["itemId"].is_number_integer()) {
            character_id = std::to_string(entry["itemId"].get<std::int64_t>());
        }
        if (character_id.empty() || character_id.size() > 64U || character_id.find('\0') != std::string::npos) {
            continue;
        }
        const Json* raw_weights = nullptr;
        if (const auto wc = entry.find("weightConfig"); wc != entry.end()) {
            if (wc->is_object()) {
                if (const auto list = wc->find("weights"); list != wc->end() && list->is_array()) {
                    raw_weights = &(*list);
                }
            } else if (wc->is_array()) {
                raw_weights = &(*wc);
            }
        }
        if (!raw_weights || raw_weights->size() > kMaxWeightEntries) continue;

        Json sub_weights = Json::object();
        Json main_weights = Json::object();
        for (const auto& item : *raw_weights) {
            if (!item.is_object() || !item.contains("key") || !item["key"].is_string()) continue;
            double sub_value = 0.0;
            double main_value = 0.0;
            if (!ReadOptionalWeight(item, "value", sub_value, error) ||
                !ReadOptionalWeight(item, "main_value", main_value, error)) {
                return false;
            }
            if (sub_value == 0.0 && main_value == 0.0) continue;
            const auto canonical = CanonicalPropertyId(item["key"].get_ref<const std::string&>());
            if (canonical.empty()) continue;
            const std::string key(canonical);
            if (sub_value > 0.0) sub_weights[key] = sub_value;
            if (main_value > 0.0) main_weights[key] = main_value;
        }
        if (sub_weights.empty()) continue;
        Json record{
            {"weights", sub_weights},
            {"main_weights", main_weights.empty() ? sub_weights : main_weights},
        };
        if (entry.contains("name") && entry["name"].is_string() &&
            !entry["name"].get_ref<const std::string&>().empty() &&
            entry["name"].get_ref<const std::string&>().size() <= kMaxIdLength) {
            record["name"] = entry["name"];
        }
        characters[character_id] = std::move(record);
    }

    if (characters.empty()) {
        error = "Upstream weights payload produced no valid character entries";
        return false;
    }

    Json result = DefaultRecommendedWeightsCatalog();
    result["source"] = "yihuan_workshop";
    result["characters"] = std::move(characters);
    out = std::move(result);
    error.clear();
    return true;
}

bool LoadRecommendedWeightsFile(const std::filesystem::path& path, Json& out, std::string& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "Local recommended weights file was not found";
        return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        error = "Local recommended weights path is not a regular file";
        return false;
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Local recommended weights file could not be opened";
        return false;
    }
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > kMaxCatalogBytes) {
        error = "Local recommended weights file size is out of bounds";
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), size)) {
        error = "Local recommended weights file could not be read";
        return false;
    }
    return ParseRecommendedWeightsCatalog(text, out, error);
}

bool FetchRecommendedWeightsHttps(std::string_view url, const std::atomic<bool>& cancel,
                                  std::string& body, std::string& error) {
    body.clear();
    if (cancel.load(std::memory_order_acquire)) {
        error = "Request cancelled";
        return false;
    }
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    if (!ParseHttpsUrl(url, host, port, path, error)) return false;

    WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr) {
        error = WinHttpError("WinHttpOpen");
        return false;
    }
    if (!WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs,
                            kSendTimeoutMs, kReceiveTimeoutMs)) {
        error = WinHttpError("WinHttpSetTimeouts");
        return false;
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirect_policy, sizeof(redirect_policy))) {
        error = WinHttpError("WinHttpSetOption");
        return false;
    }
    if (cancel.load(std::memory_order_acquire)) {
        error = "Request cancelled";
        return false;
    }

    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (connection.get() == nullptr) {
        error = WinHttpError("WinHttpConnect");
        return false;
    }
    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
    if (request.get() == nullptr) {
        error = WinHttpError("WinHttpOpenRequest");
        return false;
    }
    constexpr wchar_t kHeaders[] = L"Accept: application/json\r\nCache-Control: no-cache\r\n";
    if (!WinHttpAddRequestHeaders(request.get(), kHeaders, static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        error = WinHttpError("WinHttpAddRequestHeaders");
        return false;
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = WinHttpError("WinHttpSendRequest");
        return false;
    }
    if (cancel.load(std::memory_order_acquire)) {
        error = "Request cancelled";
        return false;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        error = WinHttpError("WinHttpReceiveResponse");
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = WinHttpError("WinHttpQueryHeaders");
        return false;
    }
    if (status_code != 200) {
        error = "Remote weights request returned HTTP " + std::to_string(status_code);
        return false;
    }

    for (;;) {
        if (cancel.load(std::memory_order_acquire)) {
            body.clear();
            error = "Request cancelled";
            return false;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            body.clear();
            error = WinHttpError("WinHttpQueryDataAvailable");
            return false;
        }
        if (available == 0) break;
        if (body.size() + available > kMaxCatalogBytes) {
            body.clear();
            error = "Remote weights response exceeds size limit";
            return false;
        }
        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &bytes_read)) {
            body.clear();
            error = WinHttpError("WinHttpReadData");
            return false;
        }
        body.resize(offset + bytes_read);
        if (bytes_read == 0) break;
    }
    error.clear();
    return true;
}

RecommendedWeightsStore::RecommendedWeightsStore(std::string api_url, std::string rawcontent_url,
                                                 RecommendedWeightsFetcher fetcher)
    : api_url_(std::move(api_url)),
      rawcontent_url_(std::move(rawcontent_url)),
      fetcher_(fetcher ? std::move(fetcher) : RecommendedWeightsFetcher(FetchRecommendedWeightsHttps)),
      catalog_(DefaultRecommendedWeightsCatalog()) {}

RecommendedWeightsStore::~RecommendedWeightsStore() {
    Stop();
}

void RecommendedWeightsStore::Load(const std::filesystem::path& plugin_directory,
                                   std::function<void(std::string_view)> logger) {
    logger_ = std::move(logger);
    Json local_catalog;
    std::string error;
    const auto path = plugin_directory.empty()
                          ? std::filesystem::path{}
                          : plugin_directory / std::filesystem::path(kRecommendedWeightsRelativePath);
    const bool loaded = !path.empty() && LoadRecommendedWeightsFile(path, local_catalog, error);
    std::size_t character_count = 0;
    {
        std::scoped_lock lock(mutex_);
        if (loaded) {
            character_count = local_catalog.at("characters").size();
            catalog_ = std::move(local_catalog);
            origin_ = "local";
        } else {
            catalog_ = DefaultRecommendedWeightsCatalog();
            origin_ = "builtin";
        }
        status_ = "idle";
        remote_error_.clear();
        ++revision_;
    }
    if (loaded) {
        Log("EquipmentManager: loaded local fallback recommended weights (" +
            std::to_string(character_count) + " characters)");
    } else if (!path.empty()) {
        Log("EquipmentManager: local recommended weights unavailable, using built-in general weights: " + error);
    }
}

void RecommendedWeightsStore::StartRefresh() {
    std::thread previous;
    {
        std::scoped_lock lock(mutex_);
        if (cancel_.load(std::memory_order_acquire) || fetching_.load(std::memory_order_acquire)) {
            return;
        }
        previous = std::move(fetch_thread_);
        fetching_.store(true, std::memory_order_release);
        status_ = "fetching";
        ++revision_;
    }
    if (previous.joinable()) previous.join();
    try {
        std::scoped_lock lock(mutex_);
        if (cancel_.load(std::memory_order_acquire)) {
            fetching_.store(false, std::memory_order_release);
            return;
        }
        fetch_thread_ = std::thread([this] { FetchWorker(); });
    } catch (...) {
        std::scoped_lock lock(mutex_);
        fetching_.store(false, std::memory_order_release);
        status_ = "fallback";
        remote_error_ = "Failed to start recommended weights refresh thread";
        ++revision_;
    }
}

void RecommendedWeightsStore::Stop() noexcept {
    cancel_.store(true, std::memory_order_release);
    std::thread worker;
    {
        std::scoped_lock lock(mutex_);
        worker = std::move(fetch_thread_);
    }
    if (worker.joinable()) worker.join();
}

Json RecommendedWeightsStore::Snapshot() const {
    std::scoped_lock lock(mutex_);
    Json snapshot = catalog_;
    snapshot["origin"] = origin_;
    snapshot["status"] = status_;
    snapshot["revision"] = revision_;
    snapshot["api_url"] = api_url_;
    snapshot["rawcontent_url"] = rawcontent_url_;
    snapshot["remote_error"] = remote_error_;
    return snapshot;
}

void RecommendedWeightsStore::FetchWorker() noexcept {
    try {
        std::string body;
        std::string api_error;
        Json catalog;
        const bool api_ok = fetcher_ && !api_url_.empty() &&
                            fetcher_(api_url_, cancel_, body, api_error) &&
                            ParseUpstreamWeightsPayload(body, catalog, api_error);
        if (cancel_.load(std::memory_order_acquire)) {
            fetching_.store(false, std::memory_order_release);
            return;
        }
        if (api_ok) {
            std::size_t count = 0;
            {
                std::scoped_lock lock(mutex_);
                count = catalog.at("characters").size();
                catalog_ = std::move(catalog);
                origin_ = "api";
                status_ = "ready";
                remote_error_.clear();
                ++revision_;
                fetching_.store(false, std::memory_order_release);
            }
            Log("EquipmentManager: loaded recommended weights from live API (" +
                std::to_string(count) + " characters)");
            return;
        }

        std::string raw_error;
        const bool raw_ok = fetcher_ && !rawcontent_url_.empty() &&
                            fetcher_(rawcontent_url_, cancel_, body, raw_error) &&
                            ParseRecommendedWeightsCatalog(body, catalog, raw_error);
        if (cancel_.load(std::memory_order_acquire)) {
            fetching_.store(false, std::memory_order_release);
            return;
        }
        if (raw_ok) {
            std::size_t count = 0;
            {
                std::scoped_lock lock(mutex_);
                count = catalog.at("characters").size();
                catalog_ = std::move(catalog);
                origin_ = "github";
                status_ = "ready";
                remote_error_ = api_error;
                ++revision_;
                fetching_.store(false, std::memory_order_release);
            }
            Log("EquipmentManager: live weights API failed (" + api_error +
                "), fell back to GitHub rawcontent (" + std::to_string(count) + " characters)");
            return;
        }

        const std::string combined_error = !api_error.empty() && !raw_error.empty()
                                               ? "API: " + api_error + "; GitHub: " + raw_error
                                               : !raw_error.empty() ? raw_error : api_error;
        {
            std::scoped_lock lock(mutex_);
            status_ = "fallback";
            remote_error_ = combined_error.empty() ? "Remote recommended weights unavailable" : combined_error;
            ++revision_;
            fetching_.store(false, std::memory_order_release);
        }
        Log("EquipmentManager: remote recommended weights unavailable, keeping local fallback (" +
            combined_error + ")");
    } catch (...) {
        std::scoped_lock lock(mutex_);
        status_ = "fallback";
        remote_error_ = "Unexpected error while loading remote recommended weights";
        ++revision_;
        fetching_.store(false, std::memory_order_release);
    }
}

void RecommendedWeightsStore::Log(std::string_view message) const noexcept {
    try {
        if (logger_) logger_(message);
    } catch (...) {}
}

}  // namespace equipment_manager
