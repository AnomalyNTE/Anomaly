// Fixture for the native television media preparation pipeline.
//
// The pipeline no longer shells out to yt-dlp/FFmpeg/ffprobe: it resolves the
// pasted link, calls the bilibili web API, streams the muxed MP4 over HTTPS and
// validates it with Media Foundation, all in-process. The fixture therefore
// injects every external boundary through PrepareOptions and lets the real
// state machine, the real API parser, the real downloader and the real Media
// Foundation validation run unmodified:
//
//   resolve_link      -> yields a fixed BilibiliPage per fixture link. The
//                        resolver sleeps briefly so the Resolving stage stays
//                        observable to the polling assertions below; its latency
//                        is not part of the pipeline contract.
//   fetch_api         -> canned view/nav/playurl documents keyed by the video id
//                        found in the request URL; every endpoint that is not
//                        expected for a scenario fails loudly.
//   open_media_stream -> scripted HttpMediaStream that serves the checked-in
//                        MP4 fixtures (or scripted transport failures) and
//                        counts factory/open/read/close calls.
//   fetch_redirect    -> counted and must never be called.
//
// Only the bytes and latency of the network boundary are under fixture control;
// the artifact itself, its cache placement and its Media Foundation validation
// are produced by production code. Fixture links use the form
// "prep:<bvid>:p<page>" and the API documents are plain JSON.
#include "plugins/BetterTV/media_prepare.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace tv = better_tv_video;

// ---------------------------------------------------------------------------
// Shared constants and check harness
// ---------------------------------------------------------------------------

constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kMissingIndex = std::numeric_limits<std::size_t>::max();
constexpr std::int64_t kFixtureDurationMs = 1000;

constexpr std::string_view kMediaOrigin =
    "https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/prepare/";
constexpr std::string_view kViewPrefix = "https://api.bilibili.com/x/web-interface/view?";
constexpr std::string_view kWbiViewPrefix =
    "https://api.bilibili.com/x/web-interface/wbi/view?";
constexpr std::string_view kNavUrl = "https://api.bilibili.com/x/web-interface/nav";
constexpr std::string_view kWbiPlayUrlPrefix = "https://api.bilibili.com/x/player/wbi/playurl?";
constexpr std::string_view kPlayUrlPrefix = "https://api.bilibili.com/x/player/playurl?";
constexpr std::string_view kWbiImgKey = "7cd084941338484aae1ad9425b84077c";
constexpr std::string_view kWbiSubKey = "4932caff0ff746eab6f01bf08b70ac45";

int g_checks = 0;
int g_failures = 0;
std::atomic<int> g_redirect_calls{0};

void Check(const bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        std::cout << "ok: " << what << '\n';
    } else {
        ++g_failures;
        std::cout << "FAIL: " << what << '\n';
    }
}

bool StartsWith(const std::string_view text, const std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// ---------------------------------------------------------------------------
// Win32 / STL helpers
// ---------------------------------------------------------------------------

std::string Utf8FromWide(const std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring WideFromUtf8(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring JoinPath(const std::wstring_view directory, const std::wstring_view name) {
    std::wstring path(directory);
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
    path.append(name);
    return path;
}

std::wstring Lowercase(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& character : result) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    return result;
}

bool StartsWithIgnoreCase(const std::wstring_view text, const std::wstring_view prefix) {
    return text.size() >= prefix.size() &&
           Lowercase(text.substr(0, prefix.size())) == Lowercase(prefix);
}

bool EndsWithIgnoreCase(const std::wstring_view text, const std::wstring_view suffix) {
    return text.size() >= suffix.size() &&
           Lowercase(text.substr(text.size() - suffix.size())) == Lowercase(suffix);
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool ReadBinaryFile(const std::wstring& path, std::string* content) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) return false;
    content->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

bool WriteBinaryFile(const std::wstring& path, const std::string_view content) {
    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(stream);
}

bool CreateDirectories(const std::wstring& path) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path), error);
    return !error && std::filesystem::is_directory(std::filesystem::path(path), error);
}

void RemoveTree(const std::wstring& path) {
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(path), error);
}

std::size_t DirectoryEntryCount(const std::wstring& path) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(std::filesystem::path(path), error);
    if (error) return 0;
    const std::filesystem::recursive_directory_iterator end;
    std::size_t count = 0;
    while (iterator != end) {
        ++count;
        iterator.increment(error);
        if (error) break;
    }
    return count;
}

std::size_t DirectoryMp4Count(const std::wstring& path) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(std::filesystem::path(path), error);
    if (error) return 0;
    const std::filesystem::directory_iterator end;
    std::size_t count = 0;
    while (iterator != end) {
        std::error_code entry_error;
        if (iterator->is_regular_file(entry_error) &&
            EndsWithIgnoreCase(iterator->path().extension().wstring(), L".mp4")) {
            ++count;
        }
        iterator.increment(error);
        if (error) break;
    }
    return count;
}

bool WaitForEmptyDirectory(const std::wstring& path, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (DirectoryEntryCount(path) == 0) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(2);
    }
}

std::wstring TempDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || static_cast<std::size_t>(length) >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

std::vector<std::wstring> Arguments() {
    std::vector<std::wstring> arguments;
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr) return arguments;
    for (int index = 0; index < count; ++index) arguments.emplace_back(values[index]);
    LocalFree(values);
    return arguments;
}

const char* StageName(const tv::PrepareStage stage) {
    switch (stage) {
        case tv::PrepareStage::Idle: return "Idle";
        case tv::PrepareStage::Resolving: return "Resolving";
        case tv::PrepareStage::Downloading: return "Downloading";
        case tv::PrepareStage::Validating: return "Validating";
        case tv::PrepareStage::Ready: return "Ready";
        case tv::PrepareStage::Failed: return "Failed";
        case tv::PrepareStage::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

const char* ErrorName(const tv::PrepareError error) {
    switch (error) {
        case tv::PrepareError::None: return "None";
        case tv::PrepareError::InvalidLink: return "InvalidLink";
        case tv::PrepareError::Network: return "Network";
        case tv::PrepareError::Api: return "Api";
        case tv::PrepareError::NoPlayableStream: return "NoPlayableStream";
        case tv::PrepareError::SegmentedStream: return "SegmentedStream";
        case tv::PrepareError::Download: return "Download";
        case tv::PrepareError::InvalidMedia: return "InvalidMedia";
        case tv::PrepareError::Oversize: return "Oversize";
        case tv::PrepareError::Budget: return "Budget";
        case tv::PrepareError::Cancelled: return "Cancelled";
        case tv::PrepareError::Internal: return "Internal";
    }
    return "Unknown";
}

bool IsTerminal(const tv::PrepareStage stage) {
    return stage == tv::PrepareStage::Ready || stage == tv::PrepareStage::Failed ||
           stage == tv::PrepareStage::Cancelled;
}

// ---------------------------------------------------------------------------
// Scripted media transport
// ---------------------------------------------------------------------------

struct MediaScript final {
    std::string bytes;
    // kNever uses bytes.size() as the length reported by Open.
    std::uint64_t reported_total{kNever};
    std::size_t chunk_bytes{64 * 1024};
    DWORD delay_ms{};
    // Maximum number of payload bytes this script serves before EOF.
    std::uint64_t serve_limit{kNever};
    // Absolute payload offset where Read returns a transport error.
    std::uint64_t fail_read_at{kNever};
    bool fail_open{};
};

// Keyed by the exact media URL placed in the playurl document.
std::map<std::string, MediaScript> g_media_scripts;

struct StreamStats final {
    std::atomic<int> factories{};
    std::atomic<int> opens{};
    std::atomic<int> reads{};
    std::atomic<int> closes{};
    mutable std::mutex mutex;
    std::string opened_url;
};

class FakeMediaStream final : public tv::HttpMediaStream {
public:
    explicit FakeMediaStream(StreamStats* stats) : stats_(stats) {}

    bool Open(const std::string_view url, std::uint64_t* total_bytes,
              std::string* detail) override {
        stats_->opens.fetch_add(1);
        {
            std::scoped_lock lock(stats_->mutex);
            stats_->opened_url.assign(url);
        }
        const auto found = g_media_scripts.find(std::string(url));
        if (found == g_media_scripts.end()) {
            if (detail != nullptr) *detail = "fixture: no media script for this URL";
            return false;
        }
        script_ = &found->second;
        if (script_->fail_open) {
            if (detail != nullptr) *detail = "fixture: scripted media request failure";
            return false;
        }
        if (total_bytes != nullptr) {
            *total_bytes = script_->reported_total == kNever
                               ? static_cast<std::uint64_t>(script_->bytes.size())
                               : script_->reported_total;
        }
        return true;
    }

    int Read(const std::span<std::uint8_t> buffer, std::string* detail) override {
        stats_->reads.fetch_add(1);
        if (script_ == nullptr) {
            if (detail != nullptr) *detail = "fixture: media stream is not open";
            return -1;
        }
        if (script_->delay_ms != 0) Sleep(script_->delay_ms);
        if (served_ >= script_->serve_limit) return 0;
        if (served_ >= script_->fail_read_at) {
            if (detail != nullptr) *detail = "fixture: scripted media read failure";
            return -1;
        }
        std::uint64_t available = static_cast<std::uint64_t>(script_->bytes.size()) - served_;
        available = std::min<std::uint64_t>({available, script_->chunk_bytes,
                                             static_cast<std::uint64_t>(buffer.size()),
                                             script_->serve_limit - served_,
                                             script_->fail_read_at - served_});
        const auto count = static_cast<std::size_t>(available);
        if (count != 0) {
            std::copy_n(reinterpret_cast<const std::uint8_t*>(script_->bytes.data()) +
                            static_cast<std::ptrdiff_t>(served_),
                        count, buffer.begin());
            served_ += count;
        }
        return static_cast<int>(count);
    }

    void Close() override { stats_->closes.fetch_add(1); }

private:
    StreamStats* stats_{};
    const MediaScript* script_{};
    std::uint64_t served_{};
};

tv::HttpMediaStreamFactory MakeMediaFactory(StreamStats* stats) {
    return [stats] {
        stats->factories.fetch_add(1);
        return std::unique_ptr<tv::HttpMediaStream>(std::make_unique<FakeMediaStream>(stats));
    };
}

// ---------------------------------------------------------------------------
// Scripted bilibili API
// ---------------------------------------------------------------------------

tv::HttpTextResult Ok(std::string body) {
    tv::HttpTextResult result;
    result.ok = true;
    result.status = 200;
    result.body = std::move(body);
    return result;
}

tv::HttpTextResult Unexpected(std::string detail) {
    tv::HttpTextResult result;
    result.detail = std::move(detail);
    return result;
}

std::string ViewJson(const std::string_view bvid, const std::int64_t aid,
                     const std::vector<std::int64_t>& cids) {
    std::string json = R"({"code":0,"message":"ok","data":{"bvid":")";
    json += bvid;
    json += R"(","aid":)";
    json += std::to_string(aid);
    json += R"(,"pages":[)";
    for (std::size_t index = 0; index < cids.size(); ++index) {
        if (index != 0) json.push_back(',');
        json += R"({"cid":)";
        json += std::to_string(cids[index]);
        json += R"(,"page":)";
        json += std::to_string(index + 1);
        json += '}';
    }
    json += "]}}";
    return json;
}

std::string ApiErrorJson(const std::int64_t code, const std::string_view message) {
    std::string json = R"({"code":)";
    json += std::to_string(code);
    json += R"(,"message":")";
    json += message;
    json += R"(","data":null})";
    return json;
}

// The anonymous nav answer reports code -101 but still carries the wbi keys.
std::string NavJson(const std::string_view img_key, const std::string_view sub_key) {
    std::string json =
        R"({"code":-101,"message":"\u672a\u767b\u5f55","data":{"wbi_img":{"img_url":)"
        R"("https://i0.hdslb.com/bfs/wbi/)";
    json += img_key;
    json += R"(.png","sub_url":"https://i0.hdslb.com/bfs/wbi/)";
    json += sub_key;
    json += R"(.png"}}})";
    return json;
}

std::string PlayUrlJson(const int quality, const std::int64_t timelength_ms,
                        const std::uint64_t size, const std::string_view media_url,
                        const std::size_t segment_count = 1) {
    std::string json = R"({"code":0,"message":"ok","data":{"quality":)";
    json += std::to_string(quality);
    json += R"(,"timelength":)";
    json += std::to_string(timelength_ms);
    json += R"(,"durl":[)";
    for (std::size_t index = 0; index < segment_count; ++index) {
        if (index != 0) json.push_back(',');
        json += R"({"order":)";
        json += std::to_string(index + 1);
        json += R"(,"size":)";
        json += std::to_string(size);
        json += R"(,"url":")";
        json += media_url;
        json += R"("})";
    }
    json += "]}}";
    return json;
}

std::string MediaUrl(const std::string_view scene) {
    return std::string(kMediaOrigin) + std::string(scene) + ".mp4";
}

std::string QueryValue(const std::string_view url, const std::string_view name) {
    const std::string marker(name);
    std::size_t position = url.find(marker + "=");
    if (position == std::string_view::npos) return {};
    position += marker.size() + 1;
    const std::size_t end = url.find('&', position);
    return std::string(url.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                                          : end - position));
}

struct ApiCase final {
    tv::HttpTextResult view{Unexpected("fixture: unexpected unsigned view request")};
    tv::HttpTextResult wbi_view{Unexpected("fixture: unexpected signed view request")};
    tv::HttpTextResult wbi_playurl{Unexpected("fixture: unexpected signed playurl request")};
    tv::HttpTextResult playurl{Unexpected("fixture: unexpected unsigned playurl request")};
};

// Keyed by the video id in the request URL; built once before the tests run and
// read-only afterwards.
std::map<std::string, ApiCase> g_api_cases;

class FakeApi final {
public:
    FakeApi() : nav_(Ok(NavJson(kWbiImgKey, kWbiSubKey))) {}

    tv::HttpTextResult operator()(const std::string_view url) const {
        {
            std::scoped_lock lock(mutex_);
            requests_.emplace_back(url);
        }
        if (StartsWith(url, kNavUrl)) return nav_;
        const std::string bvid = QueryValue(url, "bvid");
        const auto found = g_api_cases.find(bvid);
        if (found == g_api_cases.end()) {
            return Unexpected("fixture: unknown video id in request: " + std::string(url));
        }
        const ApiCase& scenario = found->second;
        if (StartsWith(url, kWbiViewPrefix)) return scenario.wbi_view;
        if (StartsWith(url, kViewPrefix)) return scenario.view;
        if (StartsWith(url, kWbiPlayUrlPrefix)) return scenario.wbi_playurl;
        if (StartsWith(url, kPlayUrlPrefix)) return scenario.playurl;
        return Unexpected("fixture: unknown api endpoint: " + std::string(url));
    }

    // The nav document has no video id in its URL; set this before submitting a
    // task when a scenario needs the wbi keys to be unavailable.
    tv::HttpTextResult nav_;

    std::size_t Count(const std::string_view prefix) const {
        std::scoped_lock lock(mutex_);
        std::size_t count = 0;
        for (const std::string& request : requests_) {
            if (StartsWith(request, prefix)) ++count;
        }
        return count;
    }

    std::size_t Total() const {
        std::scoped_lock lock(mutex_);
        return requests_.size();
    }

    std::vector<std::string> Requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::vector<std::string> requests_;
};

// ---------------------------------------------------------------------------
// Injected link resolution
// ---------------------------------------------------------------------------

std::int64_t NextCid() {
    static std::int64_t cid = 137649000;
    return ++cid;
}

std::int64_t NextAid() {
    static std::int64_t aid = 170000;
    return ++aid;
}

std::string Link(const std::string_view bvid, const int page_number = 1) {
    return "prep:" + std::string(bvid) + ":p" + std::to_string(page_number);
}

tv::BilibiliPage PageFor(const std::string_view bvid, const int page_number) {
    tv::BilibiliPage page;
    page.bvid = std::string(bvid);
    page.page = page_number;
    page.canonical_url = "https://www.bilibili.com/video/" + page.bvid;
    page.cache_key = "bv:" + page.bvid + ":p" + std::to_string(page_number);
    return page;
}

tv::BilibiliLinkResult ResolveLink(const std::string_view input) {
    tv::BilibiliLinkResult result;
    constexpr std::string_view kPrefix = "prep:";
    if (!StartsWith(input, kPrefix)) {
        result.error = tv::BilibiliLinkError::UnsupportedInput;
        result.detail = "fixture: expected a prep: fixture link";
        return result;
    }
    const std::string_view rest = input.substr(kPrefix.size());
    const std::size_t marker = rest.rfind(":p");
    if (marker == std::string_view::npos || marker == 0) {
        result.error = tv::BilibiliLinkError::Malformed;
        result.detail = "fixture: fixture link has no page suffix";
        return result;
    }
    const std::string_view bvid = rest.substr(0, marker);
    const std::string page_text(rest.substr(marker + 2));
    int page_number = page_text.empty() ? 1 : std::atoi(page_text.c_str());
    if (page_number < 1) page_number = 1;
    // Hold the Resolving stage open long enough for the poll loop below to see
    // it; the resolver is injected, so its latency is not part of the contract.
    Sleep(10);
    result.page = PageFor(bvid, page_number);
    return result;
}

// ---------------------------------------------------------------------------
// Task observation
// ---------------------------------------------------------------------------

struct Observation final {
    std::uint64_t request_id{};
    std::vector<tv::PrepareSnapshot> samples;
    std::array<std::size_t, 7> first_index{};
    tv::PrepareSnapshot last{};
    bool terminal{};

    Observation() { first_index.fill(kMissingIndex); }
};

bool SameSnapshot(const tv::PrepareSnapshot& left, const tv::PrepareSnapshot& right) {
    return left.request_id == right.request_id && left.stage == right.stage &&
           left.error == right.error && left.received_bytes == right.received_bytes &&
           left.total_bytes == right.total_bytes && left.progress == right.progress &&
           left.path == right.path && left.detail == right.detail;
}

void Observe(Observation* observation, const tv::PrepareSnapshot& snapshot) {
    const std::size_t stage_index = static_cast<std::size_t>(snapshot.stage);
    if (stage_index < observation->first_index.size() &&
        observation->first_index[stage_index] == kMissingIndex) {
        observation->first_index[stage_index] = observation->samples.size();
    }
    if (!observation->samples.empty() && SameSnapshot(observation->samples.back(), snapshot)) {
        return;
    }
    observation->samples.push_back(snapshot);
}

bool Observed(const Observation& observation, const tv::PrepareStage stage) {
    return observation.first_index[static_cast<std::size_t>(stage)] != kMissingIndex;
}

// Polls tightly so short-lived stages (Validating) and the final download
// progress sample are not missed.
Observation Await(tv::MediaPreparer& preparer, const std::uint64_t request_id,
                  const std::chrono::milliseconds timeout) {
    Observation observation;
    observation.request_id = request_id;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (std::uint64_t spins = 0;; ++spins) {
        const tv::PrepareSnapshot snapshot = preparer.Poll();
        Observe(&observation, snapshot);
        if (snapshot.request_id == request_id && IsTerminal(snapshot.stage)) {
            observation.terminal = true;
            observation.last = snapshot;
            return observation;
        }
        if ((spins & 0x3Fu) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                observation.last = snapshot;
                return observation;
            }
            SwitchToThread();
        }
    }
}

Observation Run(tv::MediaPreparer& preparer, const std::string_view link,
                const std::chrono::milliseconds timeout) {
    return Await(preparer, preparer.Submit(std::string(link)), timeout);
}

bool WaitForStage(tv::MediaPreparer& preparer, const tv::PrepareStage stage,
                  const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (preparer.Poll().stage == stage) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(1);
    }
}

void CheckOutcome(const Observation& observation, const tv::PrepareStage stage,
                  const tv::PrepareError error, const std::string& label) {
    Check(observation.request_id != 0, label + ": Submit returns a request id");
    Check(observation.terminal, label + ": reaches a terminal state");
    if (!observation.terminal) {
        std::cout << "DIAG " << label << ": request " << observation.request_id
                  << " did not finish, last stage=" << StageName(observation.last.stage)
                  << " error=" << ErrorName(observation.last.error)
                  << " detail=" << observation.last.detail << '\n';
        return;
    }
    Check(observation.last.request_id == observation.request_id,
          label + ": the final snapshot belongs to the submitted request");
    const bool stage_matches = observation.last.stage == stage;
    Check(stage_matches,
          label + ": reports stage " + StageName(stage) +
              (stage_matches ? std::string()
                             : " (got " + std::string(StageName(observation.last.stage)) +
                                   ", detail: " + observation.last.detail + ")"));
    const bool error_matches = observation.last.error == error;
    Check(error_matches,
          label + ": reports error " + ErrorName(error) +
              (error_matches ? std::string()
                             : " (got " + std::string(ErrorName(observation.last.error)) + ")"));
    if (error != tv::PrepareError::None) {
        Check(!observation.last.detail.empty(),
              label + ": the failure carries an english technical detail");
    }
}

void CheckStageOrder(const Observation& observation, const std::string& label) {
    Check(Observed(observation, tv::PrepareStage::Resolving),
          label + ": observes the Resolving stage");
    Check(Observed(observation, tv::PrepareStage::Downloading),
          label + ": observes the Downloading stage");
    Check(Observed(observation, tv::PrepareStage::Validating),
          label + ": observes the Validating stage");
    Check(Observed(observation, tv::PrepareStage::Ready), label + ": observes the Ready stage");
    const std::size_t resolving = observation.first_index[static_cast<std::size_t>(
        tv::PrepareStage::Resolving)];
    const std::size_t downloading = observation.first_index[static_cast<std::size_t>(
        tv::PrepareStage::Downloading)];
    const std::size_t validating = observation.first_index[static_cast<std::size_t>(
        tv::PrepareStage::Validating)];
    const std::size_t ready = observation.first_index[static_cast<std::size_t>(
        tv::PrepareStage::Ready)];
    Check(resolving != kMissingIndex && downloading != kMissingIndex &&
              validating != kMissingIndex && ready != kMissingIndex && resolving < downloading &&
              downloading < validating && validating < ready,
          label + ": stage order is Resolving -> Downloading -> Validating -> Ready");
}

void CheckDownloadProgress(const Observation& observation, const std::uint64_t artifact_size,
                           const std::string& label) {
    std::size_t samples = 0;
    bool monotonic = true;
    bool within_total = true;
    bool total_reported = false;
    bool reached_full = false;
    bool full_sample_is_complete = false;
    std::uint64_t previous_received = 0;
    float previous_progress = -1.0F;
    for (const tv::PrepareSnapshot& snapshot : observation.samples) {
        if (snapshot.stage != tv::PrepareStage::Downloading) continue;
        ++samples;
        if (snapshot.received_bytes < previous_received) monotonic = false;
        if (snapshot.progress < previous_progress) monotonic = false;
        if (snapshot.total_bytes != 0) {
            total_reported = true;
            if (snapshot.received_bytes > snapshot.total_bytes) within_total = false;
        }
        previous_received = snapshot.received_bytes;
        previous_progress = snapshot.progress;
        if (snapshot.progress >= 0.9999F) {
            reached_full = true;
            full_sample_is_complete = snapshot.received_bytes == artifact_size &&
                                      snapshot.total_bytes == artifact_size;
        }
    }
    Check(samples != 0, label + ": observes download progress samples");
    Check(monotonic, label + ": download progress never goes backwards");
    Check(within_total, label + ": download progress stays inside the reported total");
    Check(total_reported, label + ": the download reports a total byte count");
    Check(reached_full, label + ": download progress reaches 1.0");
    Check(full_sample_is_complete,
          label + ": the 1.0 progress sample reports the complete artifact size");
}

void CheckCleanCache(const std::wstring& cache_directory, const std::string& label) {
    Check(DirectoryMp4Count(cache_directory) == 0, label + ": no artifact entered the cache");
    Check(DirectoryEntryCount(JoinPath(cache_directory, L"tmp")) == 0,
          label + ": temporary files were cleaned");
}

// ---------------------------------------------------------------------------
// Fixture environment
// ---------------------------------------------------------------------------

struct Fixture final {
    std::wstring root;
    std::wstring data_directory;

    [[nodiscard]] std::wstring Cache(const std::wstring_view name) const {
        std::wstring folder(L"cache-");
        folder.append(name);
        return JoinPath(root, folder);
    }
};

// Small checked-in MP4 files, loaded once so the scripted transport is
// independent of how long the tests run.
std::string g_valid_mp4;
std::string g_non_h264_mp4;
std::string g_video_only_mp4;
std::string g_five_channel_mp4;
std::string g_truncated_mp4;

bool LoadMediaFiles(const std::wstring& data_directory) {
    struct Entry final {
        const wchar_t* name;
        std::string* target;
    };
    const Entry entries[] = {
        {L"tv_muxed_h264_aac.mp4", &g_valid_mp4},
        {L"tv_muxed_non_h264.mp4", &g_non_h264_mp4},
        {L"tv_muxed_video_only.mp4", &g_video_only_mp4},
        {L"tv_muxed_5ch.mp4", &g_five_channel_mp4},
    };
    for (const Entry& entry : entries) {
        const std::wstring path = JoinPath(data_directory, entry.name);
        if (!ReadBinaryFile(path, entry.target) || entry.target->empty()) {
            std::cout << "FAIL: media fixture is missing or empty: " << Utf8FromWide(path) << '\n';
            return false;
        }
    }
    // A truncated MP4 is a real container failure: the size still matches what
    // the playurl document declared, so only Media Foundation can reject it.
    constexpr std::size_t kTruncatedBytes = 512;
    g_truncated_mp4 = g_valid_mp4.substr(0, kTruncatedBytes);
    return true;
}

void RegisterMediaScripts() {
    const std::uint64_t valid_size = static_cast<std::uint64_t>(g_valid_mp4.size());

    MediaScript normal;
    normal.bytes = g_valid_mp4;
    normal.chunk_bytes = 512;
    normal.delay_ms = 10;
    g_media_scripts[MediaUrl("normal")] = normal;

    MediaScript quick;
    quick.bytes = g_valid_mp4;
    quick.chunk_bytes = 1024;
    quick.delay_ms = 1;
    g_media_scripts[MediaUrl("quick")] = quick;

    MediaScript slow;
    slow.bytes = g_valid_mp4;
    slow.chunk_bytes = 128;
    slow.delay_ms = 20;
    g_media_scripts[MediaUrl("slow")] = slow;

    MediaScript open_fail;
    open_fail.bytes = g_valid_mp4;
    open_fail.fail_open = true;
    g_media_scripts[MediaUrl("openfail")] = open_fail;

    MediaScript read_fail;
    read_fail.bytes = g_valid_mp4;
    read_fail.chunk_bytes = 256;
    read_fail.fail_read_at = 256;
    g_media_scripts[MediaUrl("readfail")] = read_fail;

    MediaScript short_body;
    short_body.bytes = g_valid_mp4;
    short_body.serve_limit = 100;
    g_media_scripts[MediaUrl("short")] = short_body;

    // The oversize and budget scenarios must fail before a transport is opened;
    // if the pipeline opens one anyway the check below would see it.
    MediaScript never_opened;
    never_opened.reported_total = valid_size;
    never_opened.fail_open = true;
    g_media_scripts[MediaUrl("oversize")] = never_opened;
    g_media_scripts[MediaUrl("budget")] = never_opened;

    MediaScript non_h264;
    non_h264.bytes = g_non_h264_mp4;
    non_h264.chunk_bytes = 4096;
    non_h264.delay_ms = 1;
    g_media_scripts[MediaUrl("nonh264")] = non_h264;

    MediaScript video_only;
    video_only.bytes = g_video_only_mp4;
    video_only.chunk_bytes = 4096;
    video_only.delay_ms = 1;
    g_media_scripts[MediaUrl("videoonly")] = video_only;

    MediaScript five_channel;
    five_channel.bytes = g_five_channel_mp4;
    five_channel.chunk_bytes = 4096;
    five_channel.delay_ms = 1;
    g_media_scripts[MediaUrl("fiftyone")] = five_channel;

    MediaScript truncated;
    truncated.bytes = g_truncated_mp4;
    truncated.chunk_bytes = 4096;
    truncated.delay_ms = 1;
    g_media_scripts[MediaUrl("truncated")] = truncated;
}

ApiCase& ApiCaseFor(const std::string_view bvid) {
    return g_api_cases[std::string(bvid)];
}

void SetHappyCase(const std::string_view bvid, const std::string_view scene,
                  const std::uint64_t size, const int page_count = 1) {
    ApiCase& scenario = ApiCaseFor(bvid);
    std::vector<std::int64_t> cids;
    for (int index = 0; index < page_count; ++index) cids.push_back(NextCid());
    scenario.view = Ok(ViewJson(bvid, NextAid(), cids));
    scenario.wbi_playurl =
        Ok(PlayUrlJson(64, kFixtureDurationMs, size, MediaUrl(scene)));
}

void RegisterApiScenarios() {
    const std::uint64_t valid_size = static_cast<std::uint64_t>(g_valid_mp4.size());
    const std::uint64_t non_h264_size = static_cast<std::uint64_t>(g_non_h264_mp4.size());
    const std::uint64_t video_only_size = static_cast<std::uint64_t>(g_video_only_mp4.size());
    const std::uint64_t five_channel_size = static_cast<std::uint64_t>(g_five_channel_mp4.size());
    const std::uint64_t truncated_size = static_cast<std::uint64_t>(g_truncated_mp4.size());

    // Happy path, cache reuse, discard and replacement downloads.
    SetHappyCase("BV1prepareOk1", "normal", valid_size);
    SetHappyCase("BV1prepareOk2", "normal", valid_size);
    SetHappyCase("BV1prepareSlow", "slow", valid_size);
    SetHappyCase("BV1prepareFast", "quick", valid_size);

    // The unsigned view and the WBI-signed fallback both reject the video.
    {
        ApiCase& scenario = ApiCaseFor("BV1viewReject");
        scenario.view = Ok(ApiErrorJson(-404, "not found"));
        scenario.wbi_view = Ok(ApiErrorJson(-404, "not found"));
    }
    // Both playurl endpoints reject the request.
    {
        ApiCase& scenario = ApiCaseFor("BV1playReject");
        scenario.view = Ok(ViewJson("BV1playReject", NextAid(), {NextCid()}));
        scenario.wbi_playurl = Ok(ApiErrorJson(-404, "not found"));
        scenario.playurl = Ok(ApiErrorJson(-404, "not found"));
    }
    // DASH-only page: the muxed profile has no stream to play. Both playurl
    // endpoints answer the same way, so the unsigned fallback cannot mask it.
    {
        ApiCase& scenario = ApiCaseFor("BV1emptyDurl");
        scenario.view = Ok(ViewJson("BV1emptyDurl", NextAid(), {NextCid()}));
        const std::string document =
            PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("normal"), 0);
        scenario.wbi_playurl = Ok(document);
        scenario.playurl = Ok(document);
    }
    // Multi-segment page: only one muxed segment is supported.
    {
        ApiCase& scenario = ApiCaseFor("BV1segmented");
        scenario.view = Ok(ViewJson("BV1segmented", NextAid(), {NextCid()}));
        const std::string document =
            PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("normal"), 2);
        scenario.wbi_playurl = Ok(document);
        scenario.playurl = Ok(document);
    }
    // Requested page outside the view page list.
    {
        ApiCase& scenario = ApiCaseFor("BV1pageRange");
        scenario.view = Ok(ViewJson("BV1pageRange", NextAid(), {NextCid(), NextCid()}));
        scenario.wbi_playurl = Ok(PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("normal")));
    }
    // WBI keys unavailable: the unsigned playurl endpoint still resolves.
    {
        ApiCase& scenario = ApiCaseFor("BV1wbiFallback");
        scenario.view = Ok(ViewJson("BV1wbiFallback", NextAid(), {NextCid()}));
        scenario.playurl =
            Ok(PlayUrlJson(16, kFixtureDurationMs, valid_size, MediaUrl("quick")));
    }

    // Download failures.
    {
        ApiCase& scenario = ApiCaseFor("BV1openFail");
        scenario.view = Ok(ViewJson("BV1openFail", NextAid(), {NextCid()}));
        scenario.wbi_playurl = Ok(PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("openfail")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1readFail");
        scenario.view = Ok(ViewJson("BV1readFail", NextAid(), {NextCid()}));
        scenario.wbi_playurl = Ok(PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("readfail")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1shortBody");
        scenario.view = Ok(ViewJson("BV1shortBody", NextAid(), {NextCid()}));
        scenario.wbi_playurl = Ok(PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("short")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1oversize");
        scenario.view = Ok(ViewJson("BV1oversize", NextAid(), {NextCid()}));
        scenario.wbi_playurl =
            Ok(PlayUrlJson(64, kFixtureDurationMs, valid_size + 512, MediaUrl("oversize")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1budget");
        scenario.view = Ok(ViewJson("BV1budget", NextAid(), {NextCid()}));
        scenario.wbi_playurl = Ok(PlayUrlJson(64, kFixtureDurationMs, valid_size, MediaUrl("budget")));
    }

    // Validation failures; the declared size matches the served bytes, so only
    // the Media Foundation checks can reject them.
    {
        ApiCase& scenario = ApiCaseFor("BV1nonH264");
        scenario.view = Ok(ViewJson("BV1nonH264", NextAid(), {NextCid()}));
        scenario.wbi_playurl =
            Ok(PlayUrlJson(64, kFixtureDurationMs, non_h264_size, MediaUrl("nonh264")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1videoOnly");
        scenario.view = Ok(ViewJson("BV1videoOnly", NextAid(), {NextCid()}));
        scenario.wbi_playurl =
            Ok(PlayUrlJson(64, kFixtureDurationMs, video_only_size, MediaUrl("videoonly")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1fiveOne");
        scenario.view = Ok(ViewJson("BV1fiveOne", NextAid(), {NextCid()}));
        scenario.wbi_playurl =
            Ok(PlayUrlJson(64, kFixtureDurationMs, five_channel_size, MediaUrl("fiftyone")));
    }
    {
        ApiCase& scenario = ApiCaseFor("BV1truncated");
        scenario.view = Ok(ViewJson("BV1truncated", NextAid(), {NextCid()}));
        scenario.wbi_playurl =
            Ok(PlayUrlJson(64, kFixtureDurationMs, truncated_size, MediaUrl("truncated")));
    }
}

tv::PrepareOptions MakeOptions(const Fixture& fixture, const std::wstring_view name,
                               FakeApi& api, StreamStats& stats,
                               const tv::PrepareLimits& limits = tv::PrepareLimits{}) {
    tv::PrepareOptions options;
    options.cache_directory = fixture.Cache(name);
    options.limits = limits;
    options.resolve_link = [](const std::string_view link) { return ResolveLink(link); };
    options.fetch_api = [&api](const std::string_view url) { return api(url); };
    options.open_media_stream = MakeMediaFactory(&stats);
    options.fetch_redirect = [](const std::string_view) {
        g_redirect_calls.fetch_add(1);
        return tv::HttpRedirectResult{};
    };
    return options;
}

// ---------------------------------------------------------------------------
// Lifecycle and paths
// ---------------------------------------------------------------------------

void CaseLifecycle() {
    tv::MediaPreparer preparer;
    Check(!preparer.Started(), "lifecycle: the preparer is not started before Start");
    Check(preparer.Submit("prep:BV1notStarted:p1") == 0,
          "lifecycle: Submit before Start returns 0");
    const tv::PrepareSnapshot idle = preparer.Poll();
    Check(idle.request_id == 0 && idle.stage == tv::PrepareStage::Idle,
          "lifecycle: Poll before Start is idle");
    Check(preparer.Stop(100), "lifecycle: Stop before Start succeeds");

    tv::PrepareOptions empty;
    Check(!preparer.Start(empty), "lifecycle: Start without a cache directory fails");
    Check(!preparer.Started(), "lifecycle: a failed Start leaves the preparer stopped");
}

void CasePaths() {
    const std::wstring cache = tv::DefaultMediaCacheDirectory();
    Check(!cache.empty(), "paths: the default cache directory is available");
    Check(Lowercase(cache).find(L"\\anomaly\\bettertv\\media-cache") != std::wstring::npos,
          "paths: the default cache directory uses the LocalAppData layout");
}

// ---------------------------------------------------------------------------
// Happy path, cache reuse and cache reclamation
// ---------------------------------------------------------------------------

void CaseHappy(const Fixture& fixture) {
    FakeApi api;
    StreamStats stats;
    tv::MediaPreparer preparer;
    const tv::PrepareOptions options = MakeOptions(fixture, L"happy", api, stats);
    const std::string link = Link("BV1prepareOk1");
    const std::uint64_t artifact_size = static_cast<std::uint64_t>(g_valid_mp4.size());

    Check(preparer.Start(options), "happy: Start succeeds");
    Check(preparer.Start(options), "happy: a repeated Start is idempotent");
    Check(preparer.Started(), "happy: Started reports true");

    const Observation ready = Run(preparer, link, 20s);
    CheckOutcome(ready, tv::PrepareStage::Ready, tv::PrepareError::None, "happy");
    CheckStageOrder(ready, "happy");
    CheckDownloadProgress(ready, artifact_size, "happy");

    Check(!ready.last.path.empty(), "happy: the ready snapshot carries an artifact path");
    const std::wstring artifact = WideFromUtf8(ready.last.path);
    Check(FileExists(artifact), "happy: the ready artifact exists");
    Check(FileSize(artifact) == artifact_size,
          "happy: the artifact is the downloaded MP4 byte for byte");
    Check(StartsWithIgnoreCase(artifact, options.cache_directory + L"\\"),
          "happy: the artifact is placed inside the cache directory");
    Check(EndsWithIgnoreCase(artifact, L".mp4"), "happy: the artifact is an MP4 file");
    Check(stats.opens.load() == 1, "happy: exactly one media request was opened");
    Check(stats.closes.load() == 1, "happy: the media stream was closed once");
    Check(api.Count(kViewPrefix) == 1 && api.Count(kNavUrl) == 1 &&
              api.Count(kWbiPlayUrlPrefix) == 1,
          "happy: the API was asked for view, nav and the signed playurl once each");
    Check(api.Count(kPlayUrlPrefix) == 0,
          "happy: the unsigned playurl fallback was not needed");
    const std::vector<std::string> requests = api.Requests();
    Check(requests.size() == 3 && StartsWith(requests[0], kViewPrefix) &&
              requests[1] == kNavUrl && StartsWith(requests[2], kWbiPlayUrlPrefix),
          "happy: API request order is view -> nav -> signed playurl");

    // Cache reuse: the same link must resolve from the in-process map without a
    // second download and without touching the API again.
    const int opens_before = stats.opens.load();
    const std::size_t requests_before = api.Total();
    const Observation cached = Run(preparer, link, 5s);
    CheckOutcome(cached, tv::PrepareStage::Ready, tv::PrepareError::None, "cache");
    Check(cached.last.path == ready.last.path, "cache: reuse returns the same artifact path");
    Check(stats.opens.load() == opens_before, "cache: reuse opens no media stream");
    Check(api.Total() == requests_before, "cache: reuse issues no API request");
    Check(!Observed(cached, tv::PrepareStage::Downloading) &&
              !Observed(cached, tv::PrepareStage::Validating),
          "cache: reuse skips Downloading and Validating");

    // Discard removes the registered artifact and its cache entry.
    Check(preparer.Discard(ready.last.path), "discard: a registered artifact is discarded");
    Check(!FileExists(artifact), "discard: the artifact file is removed");
    Check(!preparer.Discard(ready.last.path), "discard: discarding the same path again fails");
    const std::wstring unregistered = JoinPath(options.cache_directory, L"unregistered.mp4");
    Check(WriteBinaryFile(unregistered, "not a registered artifact"),
          "discard: setup writes an unregistered cache file");
    Check(!preparer.Discard(Utf8FromWide(unregistered)),
          "discard: an unregistered cache file is not discarded");
    Check(FileExists(unregistered), "discard: an unregistered cache file is left untouched");
    static_cast<void>(DeleteFileW(unregistered.c_str()));

    const Observation redownloaded = Run(preparer, link, 20s);
    CheckOutcome(redownloaded, tv::PrepareStage::Ready, tv::PrepareError::None, "discard");
    Check(stats.opens.load() == opens_before + 1,
          "discard: the next submit of the same link downloads again");
    Check(FileExists(WideFromUtf8(redownloaded.last.path)),
          "discard: the re-downloaded artifact exists");

    // A second video, then DiscardAll must remove every ready artifact.
    const Observation second = Run(preparer, Link("BV1prepareOk2"), 20s);
    CheckOutcome(second, tv::PrepareStage::Ready, tv::PrepareError::None, "discard-all");
    Check(second.last.path != redownloaded.last.path,
          "discard-all: distinct videos use distinct artifacts");
    Check(FileExists(WideFromUtf8(redownloaded.last.path)) &&
              FileExists(WideFromUtf8(second.last.path)),
          "discard-all: both artifacts exist before the purge");
    preparer.DiscardAll();
    Check(!FileExists(WideFromUtf8(redownloaded.last.path)) &&
              !FileExists(WideFromUtf8(second.last.path)),
          "discard-all: every ready artifact file is removed");
    const Observation after_purge = Run(preparer, link, 20s);
    CheckOutcome(after_purge, tv::PrepareStage::Ready, tv::PrepareError::None, "discard-all");
    Check(stats.opens.load() == opens_before + 3,
          "discard-all: a purged link is downloaded again");

    Check(preparer.Stop(5000), "happy: Stop succeeds inside the deadline");
    Check(!preparer.Started(), "happy: the preparer reports stopped");
    Check(FileExists(WideFromUtf8(after_purge.last.path)),
          "stop: a ready artifact is retained after Stop");
    Check(!DirectoryExists(JoinPath(options.cache_directory, L"tmp")),
          "stop: Stop removes the temporary directory");
    Check(preparer.Stop(100), "happy: a second Stop succeeds");
}

// ---------------------------------------------------------------------------
// API failures and the WBI fallback
// ---------------------------------------------------------------------------

void CaseApiFailures(const Fixture& fixture) {
    FakeApi api;
    StreamStats stats;
    tv::MediaPreparer preparer;
    const tv::PrepareOptions options = MakeOptions(fixture, L"api", api, stats);
    Check(preparer.Start(options), "api: Start succeeds");

    {
        const std::size_t signed_views_before = api.Count(kWbiViewPrefix);
        const Observation run = Run(preparer, Link("BV1viewReject"), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Api,
                     "api: view rejected on both endpoints");
        Check(run.last.detail.find("-404") != std::string::npos,
              "api: view rejection detail keeps the API code");
        Check(api.Count(kWbiViewPrefix) == signed_views_before + 1,
              "api: a rejected unsigned view falls back to the signed view");
        Check(api.Count(kWbiPlayUrlPrefix) == 0,
              "api: a rejected view stops before playurl");
        CheckCleanCache(options.cache_directory, "api: view rejected");
    }
    {
        const std::size_t signed_before = api.Count(kWbiPlayUrlPrefix);
        const std::size_t unsigned_before = api.Count(kPlayUrlPrefix);
        const Observation run = Run(preparer, Link("BV1playReject"), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Api,
                     "api: playurl rejected on both endpoints");
        Check(run.last.detail.find("-404") != std::string::npos,
              "api: playurl rejection detail keeps the API code");
        Check(api.Count(kWbiPlayUrlPrefix) == signed_before + 1 &&
                  api.Count(kPlayUrlPrefix) == unsigned_before + 1,
              "api: a rejected signed playurl falls back to the unsigned endpoint");
        CheckCleanCache(options.cache_directory, "api: playurl rejected");
    }
    {
        const Observation run = Run(preparer, Link("BV1emptyDurl"), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::NoPlayableStream,
                     "api: DASH-only page (empty durl)");
        CheckCleanCache(options.cache_directory, "api: empty durl");
    }
    {
        const Observation run = Run(preparer, Link("BV1segmented"), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::SegmentedStream,
                     "api: multi-segment durl");
        CheckCleanCache(options.cache_directory, "api: segmented durl");
    }
    {
        const std::size_t nav_before = api.Count(kNavUrl);
        const Observation run = Run(preparer, Link("BV1pageRange", 3), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Api,
                     "api: page out of range");
        Check(api.Count(kNavUrl) == nav_before,
              "api: an out-of-range page stops before the wbi keys are fetched");
        CheckCleanCache(options.cache_directory, "api: page out of range");
    }
    {
        // WBI keys unavailable: the unsigned playurl endpoint still works.
        api.nav_ = Unexpected("fixture: scripted nav failure");
        const std::size_t signed_before = api.Count(kWbiPlayUrlPrefix);
        const std::size_t unsigned_before = api.Count(kPlayUrlPrefix);
        const Observation run = Run(preparer, Link("BV1wbiFallback"), 15s);
        CheckOutcome(run, tv::PrepareStage::Ready, tv::PrepareError::None,
                     "api: WBI fallback to unsigned playurl");
        Check(api.Count(kWbiPlayUrlPrefix) == signed_before,
              "api: no signed playurl is attempted without wbi keys");
        Check(api.Count(kPlayUrlPrefix) == unsigned_before + 1,
              "api: the unsigned playurl endpoint resolves the fallback");
        Check(FileExists(WideFromUtf8(run.last.path)),
              "api: the WBI fallback artifact exists");
    }

    Check(preparer.Stop(5000), "api: Stop succeeds");
}

// ---------------------------------------------------------------------------
// Download failures and limits
// ---------------------------------------------------------------------------

void CaseDownloadFailures(const Fixture& fixture) {
    const std::uint64_t valid_size = static_cast<std::uint64_t>(g_valid_mp4.size());
    {
        FakeApi api;
        StreamStats stats;
        tv::MediaPreparer preparer;
        const tv::PrepareOptions options = MakeOptions(fixture, L"download", api, stats);
        Check(preparer.Start(options), "download: Start succeeds");

        {
            const int opens_before = stats.opens.load();
            const Observation run = Run(preparer, Link("BV1openFail"), 10s);
            CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Network,
                         "download: media request failure");
            Check(stats.opens.load() == opens_before + 1,
                  "download: the failing request reached the transport");
            CheckCleanCache(options.cache_directory, "download: media request failure");
        }
        {
            const int reads_before = stats.reads.load();
            const Observation run = Run(preparer, Link("BV1readFail"), 10s);
            CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Network,
                         "download: interrupted media stream");
            Check(stats.reads.load() > reads_before,
                  "download: the interrupted transfer read bytes first");
            CheckCleanCache(options.cache_directory, "download: interrupted stream");
        }
        {
            const Observation run = Run(preparer, Link("BV1shortBody"), 10s);
            CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Download,
                         "download: body shorter than the API size");
            Check(run.last.detail.find(std::to_string(valid_size)) != std::string::npos,
                  "download: the length mismatch detail names the declared size");
            CheckCleanCache(options.cache_directory, "download: short body");
        }

        Check(preparer.Stop(5000), "download: Stop succeeds");
    }
    {
        // A declared size above the artifact limit is rejected before any
        // connection is opened.
        FakeApi api;
        StreamStats stats;
        tv::MediaPreparer preparer;
        tv::PrepareLimits limits;
        limits.max_artifact_bytes = valid_size - 1;
        const tv::PrepareOptions options = MakeOptions(fixture, L"oversize", api, stats, limits);
        Check(preparer.Start(options), "oversize: Start succeeds");
        const Observation run = Run(preparer, Link("BV1oversize"), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Oversize,
                     "oversize: declared size above the artifact limit");
        Check(stats.factories.load() == 0, "oversize: no media transport was created");
        Check(stats.opens.load() == 0, "oversize: no media stream was opened");
        CheckCleanCache(options.cache_directory, "oversize");
        Check(preparer.Stop(5000), "oversize: Stop succeeds");
    }
    {
        // A small process budget rejects the artifact before it is downloaded.
        FakeApi api;
        StreamStats stats;
        tv::MediaPreparer preparer;
        tv::PrepareLimits limits;
        limits.max_process_bytes = valid_size - 1;
        const tv::PrepareOptions options = MakeOptions(fixture, L"budget", api, stats, limits);
        Check(preparer.Start(options), "budget: Start succeeds");
        const Observation run = Run(preparer, Link("BV1budget"), 10s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::Budget,
                     "budget: artifact above the process budget");
        Check(stats.opens.load() == 0, "budget: no media stream was opened");
        Check(run.last.detail.find("budget") != std::string::npos,
              "budget: the detail mentions the process budget");
        CheckCleanCache(options.cache_directory, "budget");
        Check(preparer.Stop(5000), "budget: Stop succeeds");
    }
}

// ---------------------------------------------------------------------------
// Media Foundation validation failures
// ---------------------------------------------------------------------------

void CaseValidationFailures(const Fixture& fixture) {
    FakeApi api;
    StreamStats stats;
    tv::MediaPreparer preparer;
    const tv::PrepareOptions options = MakeOptions(fixture, L"validate", api, stats);
    Check(preparer.Start(options), "validation: Start succeeds");

    struct FailureCase final {
        const char* bvid;
        const char* label;
    };
    const FailureCase cases[] = {
        {"BV1nonH264", "non H.264 video"},
        {"BV1videoOnly", "video without audio"},
        {"BV1fiveOne", "5.1 audio"},
        {"BV1truncated", "truncated MP4"},
    };
    for (const FailureCase& item : cases) {
        const std::string label = std::string("validation: ") + item.label;
        const Observation run = Run(preparer, Link(item.bvid), 20s);
        CheckOutcome(run, tv::PrepareStage::Failed, tv::PrepareError::InvalidMedia, label);
        CheckCleanCache(options.cache_directory, label);
    }

    Check(preparer.Stop(5000), "validation: Stop succeeds");
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void CaseCancel(const Fixture& fixture) {
    FakeApi api;
    StreamStats stats;
    tv::MediaPreparer preparer;
    const tv::PrepareOptions options = MakeOptions(fixture, L"cancel", api, stats);
    Check(preparer.Start(options), "cancel: Start succeeds");

    const std::uint64_t request = preparer.Submit(Link("BV1prepareSlow"));
    Check(request != 0, "cancel: Submit returns a request id");
    Check(WaitForStage(preparer, tv::PrepareStage::Downloading, 10s),
          "cancel: the slow task reaches the downloading stage");
    Check(stats.opens.load() == 1, "cancel: the slow transfer is open");
    preparer.Cancel();
    const tv::PrepareSnapshot cancelled = preparer.Poll();
    Check(cancelled.request_id == request && cancelled.stage == tv::PrepareStage::Cancelled &&
              cancelled.error == tv::PrepareError::Cancelled,
          "cancel: Cancel reports the cancelled stage for the active request");
    Check(WaitForEmptyDirectory(JoinPath(options.cache_directory, L"tmp"), 5s),
          "cancel: the cancelled task cleans its temporary files");
    Check(DirectoryMp4Count(options.cache_directory) == 0,
          "cancel: the cancelled task caches no artifact");
    Check(stats.closes.load() >= 1, "cancel: the cancelled transfer was closed");
    const tv::PrepareSnapshot settled = preparer.Poll();
    Check(settled.stage == tv::PrepareStage::Cancelled,
          "cancel: the cancelled stage is not replaced by a late result");

    const auto started = std::chrono::steady_clock::now();
    Check(preparer.Stop(5000), "cancel: Stop succeeds inside the deadline");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    Check(elapsed < 5000, "cancel: Stop honours its deadline");
    Check(!preparer.Started(), "cancel: the preparer reports stopped");
    Check(!DirectoryExists(JoinPath(options.cache_directory, L"tmp")),
          "cancel: Stop removes the temporary directory");
}

void CaseStopDuringDownload(const Fixture& fixture) {
    FakeApi api;
    StreamStats stats;
    tv::MediaPreparer preparer;
    const tv::PrepareOptions options = MakeOptions(fixture, L"stop", api, stats);
    Check(preparer.Start(options), "stop: Start succeeds");
    Check(preparer.Submit(Link("BV1prepareSlow")) != 0, "stop: the slow task is submitted");
    Check(WaitForStage(preparer, tv::PrepareStage::Downloading, 10s),
          "stop: the slow task reaches the downloading stage");
    Check(stats.opens.load() == 1, "stop: the slow transfer is open");

    const auto started = std::chrono::steady_clock::now();
    const bool stopped = preparer.Stop(3000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    Check(stopped, "stop: Stop succeeds while a download is active");
    Check(elapsed < 3000, "stop: Stop returns inside the deadline");
    Check(!preparer.Started(), "stop: the preparer reports stopped");
    Check(!DirectoryExists(JoinPath(options.cache_directory, L"tmp")),
          "stop: the temporary directory is removed");
    Check(DirectoryMp4Count(options.cache_directory) == 0,
          "stop: the interrupted task caches no artifact");
}

// ---------------------------------------------------------------------------
// Superseding an in-flight task
// ---------------------------------------------------------------------------

void CaseSupersede(const Fixture& fixture) {
    FakeApi api;
    StreamStats stats;
    tv::MediaPreparer preparer;
    const tv::PrepareOptions options = MakeOptions(fixture, L"supersede", api, stats);
    Check(preparer.Start(options), "supersede: Start succeeds");

    const std::uint64_t slow_request = preparer.Submit(Link("BV1prepareSlow"));
    Check(slow_request != 0, "supersede: the slow task is submitted");
    Check(WaitForStage(preparer, tv::PrepareStage::Downloading, 10s),
          "supersede: the slow task reaches the downloading stage");
    Check(stats.opens.load() == 1, "supersede: the slow transfer is open");

    const std::uint64_t fast_request = preparer.Submit(Link("BV1prepareFast"));
    Check(fast_request > slow_request,
          "supersede: the replacement task has a newer request id");
    const Observation run = Await(preparer, fast_request, 20s);
    CheckOutcome(run, tv::PrepareStage::Ready, tv::PrepareError::None, "supersede");
    Check(run.last.request_id == fast_request,
          "supersede: only the newest request becomes ready");
    Check(stats.opens.load() == 2,
          "supersede: each submitted task opened the media stream exactly once");
    Check(api.Count(kWbiPlayUrlPrefix) == 2,
          "supersede: each submitted task resolved one playurl document");

    Sleep(200);
    const tv::PrepareSnapshot later = preparer.Poll();
    Check(later.request_id == fast_request && later.stage == tv::PrepareStage::Ready,
          "supersede: the superseded task's late result is dropped");
    Check(DirectoryMp4Count(options.cache_directory) == 1,
          "supersede: only the newest artifact is cached");
    Check(DirectoryEntryCount(JoinPath(options.cache_directory, L"tmp")) == 0,
          "supersede: temporary files were cleaned");
    Check(FileExists(WideFromUtf8(run.last.path)), "supersede: the ready artifact exists");

    Check(preparer.Stop(5000), "supersede: Stop succeeds");
    Check(FileExists(WideFromUtf8(run.last.path)),
          "supersede: the ready artifact survives Stop");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int RunFixture(const std::wstring& data_directory) {
    const std::wstring temporary = TempDirectory();
    if (temporary.empty()) {
        std::cout << "FAIL: could not resolve the temporary directory\n";
        return 1;
    }
    Fixture fixture;
    fixture.data_directory = data_directory;
    fixture.root = JoinPath(temporary, L"anomaly-nte-tv-prepare-" +
                                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                                           std::to_wstring(GetTickCount64()));
    RemoveTree(fixture.root);
    if (!CreateDirectories(fixture.root)) {
        std::cout << "FAIL: could not create the fixture directory\n";
        return 1;
    }
    if (!LoadMediaFiles(fixture.data_directory)) {
        RemoveTree(fixture.root);
        return 1;
    }
    RegisterMediaScripts();
    RegisterApiScenarios();

    CaseLifecycle();
    CasePaths();
    CaseHappy(fixture);
    CaseApiFailures(fixture);
    CaseDownloadFailures(fixture);
    CaseValidationFailures(fixture);
    CaseCancel(fixture);
    CaseStopDuringDownload(fixture);
    CaseSupersede(fixture);

    // Short links are resolved by the injected resolver; the production WinHTTP
    // redirect fetcher must never run from this fixture.
    Check(g_redirect_calls.load() == 0,
          "offline: the production redirect fetcher is never called");

    RemoveTree(fixture.root);
    if (g_failures != 0) {
        std::cout << "FAIL: " << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "PASS: nte tv prepare fixture (" << g_checks << " checks)\n";
    return 0;
}

}  // namespace

int main() {
    const std::vector<std::wstring> arguments = Arguments();
    const std::wstring data_directory =
        arguments.size() > 1 ? arguments[1] : std::wstring(L"plugins/BetterTV/tests/data");
    // The plugin resolves the default cache directory through the shell and the
    // fixture checks it; a shell apartment is harmless for the Media Foundation
    // worker threads, which initialize COM on their own threads.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = RunFixture(data_directory);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
