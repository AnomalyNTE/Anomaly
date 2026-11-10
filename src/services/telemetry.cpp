#include "anomaly/telemetry.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

namespace anomaly {
namespace {

struct FileHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value != nullptr) WinHttpCloseHandle(value); }
};

bool ValidUuid(std::string_view value) {
    if (value.size() != 36 || value[14] != '4' ||
        std::string_view("89ab").find(value[19]) == std::string_view::npos) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

bool ValidPluginId(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

std::filesystem::path DefaultIdentityFile() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
        return {};
    const std::filesystem::path root(local);
    CoTaskMemFree(local);
    return root / L"Anomaly" / L"telemetry" / L"identity-v1";
}

std::string LoadIdentity(const std::filesystem::path& path, std::stop_token stop) {
    if (path.empty()) return {};
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return {};
    // Exclusive open serializes concurrent processes and Windows sessions. Never
    // replace an unreadable/corrupt identity with a fresh ID and inflate counts.
    FileHandle file;
    for (unsigned attempt = 0; attempt < 20 && !stop.stop_requested(); ++attempt) {
        file.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION) return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (file.value == INVALID_HANDLE_VALUE || stop.stop_requested()) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        info.nFileSizeHigh != 0) return {};
    if (info.nFileSizeLow != 0) {
        if (info.nFileSizeLow != 36) return {};
        std::string uuid(36, '\0');
        DWORD read{};
        if (!ReadFile(file.value, uuid.data(), 36, &read, nullptr) || read != 36 || !ValidUuid(uuid))
            return {};
        return uuid;
    }
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) uuid += '-';
        uuid += hex[bytes[i] >> 4];
        uuid += hex[bytes[i] & 15];
    }
    DWORD written{};
    if (!WriteFile(file.value, uuid.data(), 36, &written, nullptr) || written != 36 ||
        !FlushFileBuffers(file.value)) return {};
    return uuid;
}

struct DailyState {
    std::string day;
    unsigned attempts{};
    std::int64_t followup_ms{};
    bool active{};
    std::set<std::string> sent;
};

bool ReadDailyState(HANDLE file, const std::string& day, DailyState& state) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        info.nFileSizeHigh != 0 || info.nFileSizeLow > 32768) return false;
    state.day = day;
    if (info.nFileSizeLow == 0) return true;
    std::string data(info.nFileSizeLow, '\0');
    DWORD read{};
    if (!ReadFile(file, data.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow) return false;
    const auto value = nlohmann::json::parse(data, nullptr, false);
    if (!value.is_object() || !value.contains("day") || !value["day"].is_string()) return false;
    if (value["day"].get<std::string>() != day) return true;
    if (!value.contains("attempts") || !value["attempts"].is_number_unsigned() ||
        value["attempts"].get<unsigned>() > 2 ||
        !value.contains("followup") || !value["followup"].is_number_integer() ||
        !value.contains("active") || !value["active"].is_boolean() ||
        !value.contains("sent") || !value["sent"].is_array() || value["sent"].size() > 128) return false;
    state.attempts = value["attempts"].get<unsigned>();
    state.followup_ms = value["followup"].get<std::int64_t>();
    state.active = value["active"].get<bool>();
    for (const auto& id : value["sent"]) {
        if (!id.is_string() || !ValidPluginId(id.get_ref<const std::string&>())) return false;
        state.sent.insert(id.get<std::string>());
    }
    return true;
}

bool WriteDailyState(HANDLE file, const DailyState& state) {
    const auto data = nlohmann::json{{"day", state.day}, {"attempts", state.attempts},
        {"followup", state.followup_ms}, {"active", state.active}, {"sent", state.sent}}.dump();
    LARGE_INTEGER zero{};
    DWORD written{};
    return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) &&
        WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
        written == data.size() && SetEndOfFile(file) && FlushFileBuffers(file);
}

struct Endpoint {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port{};
    bool secure{};
};

bool ParseEndpoint(const TelemetryOptions& options, Endpoint& endpoint) {
    if (options.endpoint.empty() || options.endpoint.size() > 2048) return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(options.endpoint.c_str(), 0, 0, &parts) ||
        parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0 ||
        parts.dwExtraInfoLength != 0 || parts.dwHostNameLength == 0) return false;
    endpoint.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    endpoint.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    endpoint.port = parts.nPort;
    endpoint.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopback = endpoint.host == L"127.0.0.1" || endpoint.host == L"[::1]" || endpoint.host == L"::1";
    return endpoint.path == L"/v1/collect" && (endpoint.secure ||
        (options.allow_loopback_http && loopback && parts.nScheme == INTERNET_SCHEME_HTTP));
}

bool Request(const Endpoint& endpoint, const wchar_t* path, const std::string& body,
    const std::wstring& extra_headers, DWORD expected_status, std::string* response, std::stop_token stop) {
    if (stop.stop_requested()) return false;
    HttpHandle session{WinHttpOpen(L"Anomaly-Telemetry/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 1500, 1500, 1500, 1500)) return false;
    DWORD retries = 0;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_CONNECT_RETRIES, &retries, sizeof(retries)))
        return false;
    HttpHandle connection{WinHttpConnect(session.value, endpoint.host.c_str(), endpoint.port, 0)};
    if (!connection.value) return false;
    HttpHandle request{WinHttpOpenRequest(connection.value, L"POST", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        endpoint.secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.value) return false;
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD receive_timeout = 2000;
    DWORD maximum_headers = 8192;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
            &receive_timeout, sizeof(receive_timeout)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
            &maximum_headers, sizeof(maximum_headers))) return false;
    if (stop.stop_requested()) return false;
    const std::wstring headers = L"Content-Type: application/json\r\n" + extra_headers;
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0) || stop.stop_requested() ||
        !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != expected_status)
        return false;
    if (!response) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::array<char, 512> buffer{};
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        DWORD received{};
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) return false;
        if (received == 0) return true;
        if (response->size() + received > 2048) return false;
        response->append(buffer.data(), received);
    }
    return false;
}

std::string SolveProof(const std::string& challenge, unsigned bits, std::stop_token stop) {
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle{};
        ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm;
    struct Hash {
        BCRYPT_HASH_HANDLE handle{};
        ~Hash() { if (handle) BCryptDestroyHash(handle); }
    } hash;
    if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG) < 0)
        return {};
    std::string input = challenge + ':' + std::string(16, '0');
    const auto offset = challenge.size() + 1;
    constexpr char hex[] = "0123456789abcdef";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (std::uint64_t nonce = 0;; ++nonce) {
        if ((nonce & 255) == 0 && (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline))
            return {};
        for (unsigned i = 0; i < 16; ++i) input[offset + 15 - i] = hex[(nonce >> (i * 4)) & 15];
        std::array<unsigned char, 32> digest{};
        if (BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0) < 0 ||
            BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
        unsigned leading{};
        for (const auto byte : digest) {
            if (byte == 0) { leading += 8; continue; }
            for (unsigned mask = 128; (byte & mask) == 0; mask >>= 1) ++leading;
            break;
        }
        if (leading >= bits) return input.substr(offset);
    }
}

bool Send(const Endpoint& endpoint, const std::string& body, std::stop_token stop) {
    std::string response;
    if (!Request(endpoint, L"/v1/challenge", body, {}, 200, &response, stop)) return false;
    const auto document = nlohmann::json::parse(response, nullptr, false);
    if (!document.is_object() || !document.contains("challenge") || !document["challenge"].is_string() ||
        !document.contains("difficulty") || !document["difficulty"].is_number_unsigned()) return false;
    const auto challenge = document["challenge"].get<std::string>();
    const auto bits = document["difficulty"].get<unsigned>();
    // Bound server-requested CPU work even if the endpoint is misconfigured.
    if (bits < 12 || bits > 20 || challenge.empty() || challenge.size() > 1024 ||
        !std::ranges::all_of(challenge, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        })) return false;
    const auto nonce = SolveProof(challenge, bits, stop);
    if (nonce.empty()) return false;
    const std::wstring headers = L"X-Anomaly-Challenge: " + std::wstring(challenge.begin(), challenge.end()) +
        L"\r\nX-Anomaly-Nonce: " + std::wstring(nonce.begin(), nonce.end()) + L"\r\n";
    return Request(endpoint, endpoint.path.c_str(), body, headers, 204, nullptr, stop);
}

} // namespace

std::string TelemetryDay(std::chrono::system_clock::time_point now) {
    const std::chrono::year_month_day day(std::chrono::floor<std::chrono::days>(now + std::chrono::hours(8)));
    char value[16]{};
    std::snprintf(value, sizeof(value), "%04d-%02u-%02u", static_cast<int>(day.year()),
        static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
    return value;
}

void ReportLauncherUsage(const TelemetryOptions& options,
    const LauncherTelemetryUsage& usage, std::stop_token stop) noexcept {
    try {
        Endpoint endpoint;
        constexpr std::array<std::string_view, 7> themes{
            "moss", "aurora", "ember", "paper", "anomalyhub", "naiwa", "custom"};
        if (!options.enabled || stop.stop_requested() ||
            (options.language != "en-US" && options.language != "zh-CN") ||
            std::ranges::find(themes, usage.theme) == themes.end() || !ParseEndpoint(options, endpoint)) return;
        const auto identity = options.identity_file.empty() ? DefaultIdentityFile() : options.identity_file;
        const auto uuid = LoadIdentity(identity, stop);
        if (uuid.empty() || stop.stop_requested()) return;
        auto path = identity;
        path += L".launcher-daily-v1";
        FileHandle file{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.nFileSizeHigh != 0 ||
            (info.nFileSizeLow != 0 && info.nFileSizeLow != 10)) return;
        const auto day = TelemetryDay(std::chrono::system_clock::now());
        if (info.nFileSizeLow != 0) {
            std::string previous(10, '\0');
            DWORD read{};
            if (!ReadFile(file.value, previous.data(), 10, &read, nullptr) || read != 10 ||
                previous[4] != '-' || previous[7] != '-' ||
                !std::ranges::all_of(previous, [](char c) { return (c >= '0' && c <= '9') || c == '-'; }) ||
                previous >= day) return;
        }
        LARGE_INTEGER zero{};
        DWORD written{};
        if (!SetFilePointerEx(file.value, zero, nullptr, FILE_BEGIN) ||
            !WriteFile(file.value, day.data(), 10, &written, nullptr) || written != 10 ||
            !FlushFileBuffers(file.value)) return;
        const auto body = nlohmann::json{{"schemaVersion", 2}, {"source", "launcher"},
            {"uuid", uuid}, {"day", day}, {"language", options.language},
            {"theme", usage.theme}, {"customBackground", usage.custom_background}}.dump();
        static_cast<void>(Send(endpoint, body, stop));
    } catch (...) {} // Optional statistics never fail launcher initialization.
}

class TelemetryClient::Impl {
public:
    explicit Impl(TelemetryOptions value) : options(std::move(value)) {}

    void AdvanceDay(const std::string& today) {
        if (day == today) return;
        day = today;
        pending.clear();
        sent.clear();
    }

    void Observe(const std::vector<std::string>& ids, std::chrono::system_clock::time_point observed_at) {
        if (!options.enabled || options.endpoint.empty()) return;
        const auto today = TelemetryDay(std::chrono::system_clock::now());
        if (TelemetryDay(observed_at) != today) return;
        std::scoped_lock lock(mutex);
        AdvanceDay(today);
        for (const auto& id : ids) {
            if (pending.size() + sent.size() >= 256) break;
            if (ValidPluginId(id) && !sent.contains(id)) pending.insert(id);
        }
    }

    void Run(std::stop_token stop) {
        Endpoint endpoint;
        if (!options.enabled || (!options.mode.empty() && options.mode != "inject" && options.mode != "proxy") ||
            (options.language != "en-US" && options.language != "zh-CN") ||
            !ParseEndpoint(options, endpoint)) return;
        const auto identity = options.identity_file.empty() ? DefaultIdentityFile() : options.identity_file;
        const auto uuid = LoadIdentity(identity, stop);
        if (uuid.empty()) return;
        auto state_path = identity;
        state_path += L".daily-v1";
        const auto started = std::chrono::steady_clock::now();
        auto next_send = started + std::chrono::seconds(1);
        auto schedule_day = TelemetryDay(std::chrono::system_clock::now());
        unsigned attempts = 0;
        while (!stop.stop_requested()) {
            std::string request_day;
            std::vector<std::string> batch;
            FileHandle state_file;
            DailyState state;
            {
                std::unique_lock lock(mutex);
                wake.wait_until(lock, stop,
                    std::min(next_send, std::chrono::steady_clock::now() + std::chrono::seconds(30)),
                    [] { return false; });
                if (stop.stop_requested()) return;
                AdvanceDay(TelemetryDay(std::chrono::system_clock::now()));
                const auto now = std::chrono::steady_clock::now();
                if (schedule_day != day) {
                    schedule_day = day;
                    attempts = 0;
                    next_send = now + std::chrono::seconds(1);
                    continue;
                }
                if (attempts == 2) {
                    next_send = now + std::chrono::seconds(30);
                    continue;
                }
                if (now < next_send) continue;
                // Serialize installations sharing this identity, including restarts.
                // Persist the attempt before networking so crashes cannot create a loop.
                state_file.value = CreateFileW(state_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (state_file.value == INVALID_HANDLE_VALUE) {
                    if (GetLastError() != ERROR_SHARING_VIOLATION) return;
                    next_send = now + std::chrono::seconds(30);
                    continue;
                }
                if (!ReadDailyState(state_file.value, day, state)) return;
                attempts = state.attempts;
                sent = state.sent;
                for (const auto& id : sent) pending.erase(id);
                if (attempts == 2) continue;
                const auto wall_now = std::chrono::system_clock::now();
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_now.time_since_epoch()).count();
                if (attempts == 1 && now_ms < state.followup_ms) {
                    next_send = now + std::chrono::milliseconds(state.followup_ms - now_ms);
                    continue;
                }
                if (attempts == 0) state.followup_ms = now_ms + 600000;
                ++attempts;
                state.attempts = attempts;
                if (!WriteDailyState(state_file.value, state)) return;
                next_send = attempts == 1 ? now + std::chrono::minutes(10) : now + std::chrono::seconds(30);
                if (attempts == 2 && state.active && pending.empty()) continue;
                request_day = day;
                for (const auto& id : pending) {
                    if (batch.size() == 64) break;
                    batch.push_back(id);
                }
            }
            nlohmann::json report{{"schemaVersion", 1}, {"uuid", uuid}, {"day", request_day},
                {"language", options.language}, {"plugins", batch}};
            if (!options.mode.empty()) {
                report["schemaVersion"] = 2;
                report["source"] = "core";
                report["mode"] = options.mode;
            }
            const bool success = Send(endpoint, report.dump(), stop);
            if (!success) continue;
            state.active = true;
            for (const auto& id : batch) state.sent.insert(id);
            if (!WriteDailyState(state_file.value, state)) return;
            std::scoped_lock lock(mutex);
            if (day != request_day) continue;
            for (const auto& id : batch) { pending.erase(id); sent.insert(id); }
        }
    }

    TelemetryOptions options;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::string day;
    std::set<std::string> pending;
    std::set<std::string> sent;
};

TelemetryClient::TelemetryClient(TelemetryOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
TelemetryClient::~TelemetryClient() = default;
void TelemetryClient::ObservePlugins(const std::vector<std::string>& ids,
    std::chrono::system_clock::time_point observed_at) noexcept {
    try { impl_->Observe(ids, observed_at); } catch (...) {}
}
void TelemetryClient::Run(std::stop_token stop) noexcept {
    try { impl_->Run(stop); } catch (...) {}
}

} // namespace anomaly
