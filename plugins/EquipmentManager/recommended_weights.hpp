#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace equipment_manager {

inline constexpr std::string_view kRecommendedWeightsApiUrl =
    "https://yh.zzzmap.com/api/open/game-character/weight-configs";
inline constexpr std::string_view kRecommendedWeightsRawContentUrl =
    "https://raw.githubusercontent.com/AnomalyNTE/Anomaly/main/plugins/EquipmentManager/assets/web/recommended_weights.json";
inline constexpr std::string_view kRecommendedWeightsRelativePath =
    "assets/web/recommended_weights.json";

using RecommendedWeightsFetcher =
    std::function<bool(std::string_view url, const std::atomic<bool>& cancel, std::string& body, std::string& error)>;

nlohmann::json DefaultRecommendedWeightsCatalog();
bool ParseRecommendedWeightsCatalog(std::string_view text, nlohmann::json& out, std::string& error);
bool ParseUpstreamWeightsPayload(std::string_view text, nlohmann::json& out, std::string& error);
bool LoadRecommendedWeightsFile(const std::filesystem::path& path, nlohmann::json& out, std::string& error);
bool FetchRecommendedWeightsHttps(std::string_view url, const std::atomic<bool>& cancel,
                                  std::string& body, std::string& error);

// Owns the active recommended weight catalog for EquipmentManager.
// Load() synchronously loads the bundled local static file as immediate fallback.
// StartRefresh() fetches live weights from the public API on a private thread,
// falling back to GitHub rawcontent and then the local static file if unavailable.
// Stop() cancels and joins the background thread before unload.
class RecommendedWeightsStore final {
public:
    explicit RecommendedWeightsStore(std::string api_url = std::string(kRecommendedWeightsApiUrl),
                                     std::string rawcontent_url = std::string(kRecommendedWeightsRawContentUrl),
                                     RecommendedWeightsFetcher fetcher = {});
    ~RecommendedWeightsStore();

    RecommendedWeightsStore(const RecommendedWeightsStore&) = delete;
    RecommendedWeightsStore& operator=(const RecommendedWeightsStore&) = delete;

    void Load(const std::filesystem::path& plugin_directory, std::function<void(std::string_view)> logger = {});
    void StartRefresh();
    void Stop() noexcept;
    nlohmann::json Snapshot() const;

private:
    void FetchWorker() noexcept;
    void Log(std::string_view message) const noexcept;

    std::string api_url_;
    std::string rawcontent_url_;
    RecommendedWeightsFetcher fetcher_;
    std::function<void(std::string_view)> logger_;

    mutable std::mutex mutex_;
    nlohmann::json catalog_;
    std::string origin_{"builtin"};
    std::string status_{"idle"};
    std::string remote_error_;
    std::uint64_t revision_{0};

    std::atomic<bool> cancel_{false};
    std::atomic<bool> fetching_{false};
    std::thread fetch_thread_;
};

}  // namespace equipment_manager
