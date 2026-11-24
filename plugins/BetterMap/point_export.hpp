#pragma once
#include "city_events.hpp"
#include "monster_points.hpp"
#include <anomaly/sdk/localization.hpp>
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>

namespace collectible_map {
struct ExportPoint {
    std::string id, row_id, level, area_id, gameplay_area_id, floor_area_id;
    int32_t floor{};
    Position location{};
};

struct ExportCategory {
    std::string kind, group_id, name;
    std::vector<ExportPoint> points;
};

struct ExportSnapshot {
    std::array<bool, 4> available{};
    std::vector<ExportCategory> categories;
};

inline constexpr std::array<std::string_view, 4> ExportKinds{
    "oracle-stone", "gift-21", "monster", "city-crime"};

inline std::string ExportName(Engine& engine, NameId key) {
    if (!key.index) return {};
    auto name = engine.NameById(key.index);
    if (!name.empty() && key.number) name += "_" + std::to_string(key.number - 1);
    return name;
}

inline std::string ExportLevel(const std::wstring& text) {
    const auto length = static_cast<int>(text.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid level name");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size, nullptr, nullptr))
        throw std::runtime_error("Invalid level name");
    return result;
}

inline bool ExportMarkers(Engine& engine, std::span<const Marker> markers, ExportCategory& category) {
    for (const auto& marker : markers) {
        ExportPoint point;
        point.row_id = ExportName(engine, marker.key);
        if (point.row_id.empty()) return false;
        point.id = category.kind + "/" + point.row_id;
        point.level = ExportLevel(marker.level);
        point.area_id = ExportName(engine, marker.area);
        point.gameplay_area_id = ExportName(engine, marker.gameplay_area);
        point.floor_area_id = ExportName(engine, marker.floor_area);
        point.floor = marker.floor;
        point.location = marker.location;
        category.points.push_back(std::move(point));
    }
    return true;
}

inline ExportSnapshot CapturePointExport(Engine& engine, const anomaly::sdk::Profile& profile,
                                         const anomaly::sdk::Localizer& localizer) {
    ExportSnapshot result;
    Catalog catalog;
    if (ReadCatalog(engine, profile, catalog)) {
        const std::array labels{
            localizer.Text("export.oracle", "Oracle Stone"),
            localizer.Text("export.gift", "Twenty-One's Gift")};
        for (size_t kind = 0; kind < catalog.size(); ++kind) {
            ExportCategory category{std::string(ExportKinds[kind]), {}, labels[kind]};
            if (ExportMarkers(engine, catalog[kind], category)) {
                result.available[kind] = true;
                result.categories.push_back(std::move(category));
            }
        }
    }
    MonsterCatalog monsters;
    if (ReadMonsterGroups(engine, profile, monsters)) {
        result.available[2] = true;
        for (const auto& group : monsters.groups) {
            ExportCategory category{"monster", group.id, group.label};
            std::set<std::string> ids;
            for (const auto& position : group.positions) {
                ExportPoint point;
                point.id = "monster/" + group.id;
                // Coordinates keep identities stable if the table array is reordered.
                for (const auto axis : position) {
                    std::array<char, 64> buffer{};
                    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), axis,
                        std::chars_format::general, std::numeric_limits<double>::max_digits10);
                    point.id += "/";
                    point.id.append(buffer.data(), converted.ptr);
                }
                if (!ids.insert(point.id).second) continue;
                point.row_id = group.id;
                point.level = "XL_map_bigworld_test";
                point.location = position;
                category.points.push_back(std::move(point));
            }
            result.categories.push_back(std::move(category));
        }
    }
    std::vector<Marker> crimes;
    if (ReadCityEventMarkers(engine, profile, crimes)) {
        ExportCategory category{"city-crime", {}, localizer.Text("export.crime", "City Crime")};
        if (ExportMarkers(engine, crimes, category)) {
            result.available[3] = true;
            result.categories.push_back(std::move(category));
        }
    }
    return result;
}

inline std::string ExportCategoryFile(const ExportCategory& category) {
    if (category.kind != "monster") return category.kind + ".json";
    // A stable ASCII filename also supports non-ASCII game row names on Windows.
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : category.group_id) hash = (hash ^ byte) * 1099511628211ull;
    std::array<char, 16> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), hash, 16);
    return "monster-" + std::string(buffer.data(), converted.ptr) + ".json";
}

inline auto BuildPointExport(const ExportSnapshot& snapshot) {
    using Json = nlohmann::json;
    const Json header{{"schemaVersion", 1}, {"source", "b1ank.better-map"},
                      {"coordinateSystem", "ue-world-centimeters"}};
    Json index = header;
    index["datasets"] = Json::object();
    index["files"] = Json::array();
    for (size_t kind = 0; kind < ExportKinds.size(); ++kind)
        index["datasets"][std::string(ExportKinds[kind])] = snapshot.available[kind];
    std::vector<std::pair<std::string, std::string>> files;
    std::set<std::string> paths;
    for (const auto& category : snapshot.categories) {
        const auto path = ExportCategoryFile(category);
        if (!paths.insert(path).second) throw std::runtime_error("Duplicate export category filename");
        Json document = header;
        document["kind"] = category.kind;
        document["groupId"] = category.group_id;
        document["name"] = category.name;
        document["positionKind"] = category.kind == "city-crime" ? "map-icon" : "static-point";
        document["points"] = Json::array();
        for (const auto& point : category.points) {
            document["points"].push_back({{"id", point.id}, {"rowId", point.row_id},
                {"level", point.level}, {"areaId", point.area_id}, {"gameplayAreaId", point.gameplay_area_id},
                {"floorAreaId", point.floor_area_id}, {"floor", point.floor},
                {"x", point.location[0]}, {"y", point.location[1]}, {"z", point.location[2]}});
        }
        index["files"].push_back({{"path", path}, {"kind", category.kind}, {"groupId", category.group_id},
                                   {"name", category.name}, {"count", category.points.size()}});
        files.emplace_back(path, document.dump(2) + "\n");
    }
    // Publish the current file list last; readers must use this index, not a directory scan.
    files.emplace_back("index.json", index.dump(2) + "\n");
    return files;
}

inline AnomalyStatusV1 WritePointExport(const AnomalyStorageServiceV1& storage, const ExportSnapshot& snapshot) {
    for (const auto& [file, document] : BuildPointExport(snapshot)) {
        // Storage creates the plugin state root, but does not create child directories.
        const auto status = storage.write_atomic(storage.user, anomaly::sdk::StringView(file),
            {reinterpret_cast<const uint8_t*>(document.data()), document.size()});
        if (!anomaly::sdk::Succeeded(status)) return status;
    }
    return anomaly::sdk::Ok();
}

class PointExporter {
public:
    enum class State { Unavailable, Idle, Requested, Writing, Complete, Partial, NoData, Failed };

    void Load(const anomaly::sdk::Host& host) {
        const auto storage = host.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID);
        const auto scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        if (!storage || storage->struct_size < sizeof(*storage.get()) || !storage->write_atomic ||
            !scheduler || scheduler->struct_size < sizeof(*scheduler.get()) || !scheduler->schedule) return;
        storage_ = storage.get();
        scheduler_ = scheduler.get();
        state_ = State::Idle;
    }

    void Request() {
        auto state = state_.load();
        if (state != State::Unavailable && state != State::Requested && state != State::Writing)
            state_.compare_exchange_strong(state, State::Requested);
    }

    void Update(Engine& engine, const anomaly::sdk::Profile& profile, const anomaly::sdk::Localizer& localizer) noexcept {
        if (state_.load() != State::Requested) return;
        state_ = State::Writing;
        try {
            if (!engine.registry) { state_ = State::NoData; return; }
            snapshot_ = CapturePointExport(engine, profile, localizer);
            if (snapshot_.categories.empty()) { state_ = State::NoData; return; }
            AnomalyGenerationHandleV1 task{};
            const auto status = scheduler_->schedule(scheduler_->user, 0, WriteTask, this, &task);
            if (!anomaly::sdk::Succeeded(status)) { error_ = status.code; state_ = State::Failed; }
        } catch (...) { error_ = ANOMALY_STATUS_V1_FAILED; state_ = State::Failed; }
    }

    void Draw(const AnomalyUiServiceV1* ui, const anomaly::sdk::Localizer& localizer) {
        using namespace anomaly::sdk;
        if (ui->button && ui->button(ui->user, StringView(localizer.Text("export.button", "Export all point categories")), 0, 0))
            Request();
        std::string message;
        switch (state_.load()) {
        case State::Unavailable: message = localizer.Text("export.unavailable", "Point export needs storage and scheduler services."); break;
        case State::Idle: message = localizer.Text("export.hint", "Exports all points by category, including hidden and completed points."); break;
        case State::Requested: message = localizer.Text("export.waiting", "Waiting for the game thread to read point tables…"); break;
        case State::Writing: message = localizer.Text("export.writing", "Exporting point categories…"); break;
        case State::Complete:
        case State::Partial: {
            const auto count = std::to_string(count_.load());
            message = localizer.Format("export.saved", "Exported {0} points to state/plugins/b1ank.better-map/.",
                                       std::array<std::string_view, 1>{count});
            (ui->text_wrapped ? ui->text_wrapped : ui->text)(ui->user, StringView(message));
            message = state_.load() == State::Partial
                ? localizer.Text("export.partial", "Some tables were unavailable; index.json identifies the exported categories.") : std::string{};
            break;
        }
        case State::NoData: message = localizer.Text("export.no_data", "No point tables are ready. Enter the main world and try again."); break;
        case State::Failed: {
            const auto code = std::to_string(error_.load());
            message = localizer.Format("export.failed", "Point export failed (status {0}).", std::array<std::string_view, 1>{code});
            break;
        }
        }
        if (!message.empty()) (ui->text_wrapped ? ui->text_wrapped : ui->text)(ui->user, StringView(message));
    }

private:
    static void ANOMALY_CALL WriteTask(void* raw, AnomalyGenerationHandleV1) noexcept {
        auto& self = *static_cast<PointExporter*>(raw);
        try {
            const auto snapshot = std::move(self.snapshot_);
            size_t count{};
            for (const auto& category : snapshot.categories) count += category.points.size();
            const auto status = WritePointExport(*self.storage_, snapshot);
            if (!anomaly::sdk::Succeeded(status)) { self.error_ = status.code; self.state_ = State::Failed; return; }
            self.count_ = count;
            self.state_ = std::ranges::all_of(snapshot.available, [](bool ready) { return ready; }) ? State::Complete : State::Partial;
        } catch (...) { self.error_ = ANOMALY_STATUS_V1_FAILED; self.state_ = State::Failed; }
    }

    const AnomalyStorageServiceV1* storage_{};
    const AnomalySchedulerServiceV1* scheduler_{};
    ExportSnapshot snapshot_; // Game produces owned values; Worker never accesses UE objects.
    std::atomic<State> state_{State::Unavailable};
    std::atomic<size_t> count_{};
    std::atomic<uint32_t> error_{};
};
}
