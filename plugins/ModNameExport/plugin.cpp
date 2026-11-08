#include "data_table.hpp"

#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/plugin.h>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/platform.h>
#include <anomaly/sdk/services/ue5.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace anomaly::sdk;

// The container folders name their category with a game identifier: `Player_051` is the actor whose
// model folder carries that number, `Vehicle_037` the car whose item row does. The two tables below
// hold the names, and they are the only thing this plugin reads.
constexpr std::string_view kCharacterTable =
    "/Game/DataTable/Character/DT_Character.DT_Character";
constexpr std::string_view kVehicleTable =
    "/Game/DataTable/Vehicle/DT_VehicleItemData.DT_VehicleItemData";
// The one file this plugin writes: the map the mods page reads after the shipped table.
constexpr std::string_view kCategoryNames = "mod-group-names.json";
constexpr std::size_t kTableCount = 2;
constexpr std::size_t kRowsPerFrame = 4;

struct Context {
    const AnomalyHostApiV1* api{};
    const AnomalyStorageServiceV1* storage{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyCoreServiceV1* core{};
    mod_name_export::Access access;
    AnomalyGenerationHandleV1 task{};
    std::mutex mutex;
    bool stopping{true};
    bool running{};
    bool written{};
    std::size_t table_index{};
    std::string table_path;
    std::vector<mod_name_export::Access::Field> fields;
    std::vector<mod_name_export::Access::Row> rows;
    std::size_t cursor{};
    // Row values of the finished tables, the table being read, and the map prepared from them.
    std::vector<std::pair<std::string, nlohmann::json>> tables;
    nlohmann::json rows_json = nlohmann::json::array();
    std::string document;
};

Context g;

void Report(const AnomalyCoreLogLevelV1 level, const std::string& text) {
    const auto core = g.core;
    if (core != nullptr && core->log != nullptr) core->log(core->user, level, StringView(text));
}

bool Write(const std::string_view path, const std::string& text) {
    const auto storage = g.storage;
    if (storage == nullptr || storage->write_atomic == nullptr) return false;
    const AnomalyByteSpanV1 source{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    return storage->write_atomic(storage->user, StringView(path), source).code ==
        ANOMALY_STATUS_V1_OK;
}

// One row name becomes the identifier a container folder uses: a character row is `1` plus that
// number (`1051` -> `Player_051`), a vehicle row is `vehicle` plus it (`vehicle037` -> `Vehicle_037`).
bool CategoryToken(std::string key, const bool vehicles, std::string& token) {
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (vehicles) {
        if (key.size() != 10 || !key.starts_with("vehicle")) return false;
        if (key.find_first_not_of("0123456789", 7) != std::string::npos) return false;
        token = "Vehicle_" + key.substr(7);
        return true;
    }
    if (key.size() != 4 || key.front() != '1') return false;
    if (key.find_first_not_of("0123456789") != std::string::npos) return false;
    token = "Player_" + key.substr(1);
    return true;
}

// The names come wrapped in corner brackets in the table; the page shows the bare name.
void TrimName(std::string& name) {
    while (name.size() >= 3 && name.compare(0, 3, "\xe3\x80\x8c") == 0) name.erase(0, 3);
    while (name.size() >= 3 && name.compare(name.size() - 3, 3, "\xe3\x80\x8d") == 0)
        name.resize(name.size() - 3);
}

// 零 exists as both the male and the female character, so a name that repeats is kept apart by the
// body the row's MeltPackID names, or by its own number when the row says nothing.
std::string Body(const std::string& melt) {
    if (melt.find("emale") != std::string::npos) return "\xe5\xa5\xb3";
    if (melt.find("player") != std::string::npos) return "\xe7\x94\xb7";
    return {};
}

void ANOMALY_CALL WriteCategories(void*, AnomalyGenerationHandleV1) noexcept {
    try {
        std::string document;
        {
            std::scoped_lock lock(g.mutex);
            document = g.document;
        }
        if (document.empty()) return;
        const auto written = Write(kCategoryNames, document);
        {
            std::scoped_lock lock(g.mutex);
            if (written) g.written = true;
        }
        Report(written ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            written ? "ModNameExport: wrote " + std::string(kCategoryNames)
                    : "ModNameExport: cannot write " + std::string(kCategoryNames));
    } catch (...) {
        Report(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "ModNameExport: write failed");
    }
}

// Game: every table is walked a few rows at a time, because FText is resolved by the host only on
// this domain. The finished rows become the map, which a Worker task then writes.
void Finish() {
    struct Named final {
        std::string token, name, body;
    };
    std::vector<Named> named;
    std::map<std::string, std::size_t> counts;
    for (const auto& [path, table] : g.tables) {
        const bool vehicles = path.find("Vehicle") != std::string::npos;
        for (const auto& row : table) {
            const auto& values = row["values"];
            if (!values.is_object()) continue;
            std::string name = values.value("ItemName", std::string{});
            TrimName(name);
            if (name.empty()) continue;
            std::string token;
            if (!CategoryToken(row.value("row", std::string{}), vehicles, token)) continue;
            ++counts[name];
            named.push_back({std::move(token), std::move(name),
                Body(values.value("MeltPackID", std::string{}))});
        }
    }
    nlohmann::json groups = nlohmann::json::object();
    for (const auto& entry : named) {
        std::string display = entry.name;
        if (counts[entry.name] > 1) {
            display += "\xef\xbc\x88" +
                (entry.body.empty() ? entry.token.substr(entry.token.find('_') + 1) : entry.body) +
                "\xef\xbc\x89";
        }
        // The name is the one this installation of the game carries, which for a Chinese client is
        // its Chinese spelling: it is the zh-CN name and never the en-US one, or an English mods page
        // would read a Chinese name for a category it has no English name for.
        groups[entry.token] = {{"zh-CN", display}};
    }
    if (groups.empty()) {
        Report(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "ModNameExport: no category names read");
        return;
    }
    g.document = nlohmann::json{{"schemaVersion", 1}, {"groups", groups}}.dump(2) + '\n';
    Report(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "ModNameExport: " +
        std::to_string(groups.size()) + " category names");
    if (g.scheduler->schedule(g.scheduler->user, 0, WriteCategories, nullptr, &g.task).code !=
        ANOMALY_STATUS_V1_OK)
        Report(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "ModNameExport: cannot schedule the write");
}

void ANOMALY_CALL Update(void*, double) noexcept {
    try {
        std::scoped_lock lock(g.mutex);
        if (g.stopping || !g.running || g.written) return;
        if (g.table_index >= kTableCount) {
            g.running = false;
            Finish();
            return;
        }
        if (g.rows.empty()) {
            const auto path = g.table_index == 0 ? kCharacterTable : kVehicleTable;
            const auto table = g.access.Resolve(g.access.Find(path));
            if (!table) {
                ++g.table_index;
                return;
            }
            g.table_path = std::string(path);
            g.fields = g.access.Fields(table);
            g.rows = g.access.Rows(table);
            g.cursor = 0;
            g.rows_json = nlohmann::json::array();
            if (g.rows.empty()) {
                ++g.table_index;
                return;
            }
        }
        std::size_t processed{};
        while (g.cursor < g.rows.size() && processed < kRowsPerFrame) {
            const auto& row = g.rows[g.cursor++];
            ++processed;
            nlohmann::json values = nlohmann::json::object();
            for (const auto& field : g.fields) {
                if (field.offset + field.size > 0x4000) continue;
                std::string value;
                if (field.type == "TextProperty") value = g.access.TextAt(row.data + field.offset);
                else if (field.type == "StrProperty") value = g.access.StringAt(row.data + field.offset);
                else if (field.type == "NameProperty") value = g.access.NameAt(row.data + field.offset);
                else continue;
                if (value.empty()) continue;
                values[field.name] = value;
            }
            g.rows_json.push_back({{"row", row.name}, {"values", values}});
        }
        if (g.cursor < g.rows.size()) return;
        g.tables.emplace_back(g.table_path, g.rows_json);
        g.rows_json = nlohmann::json::array();
        g.rows.clear();
        g.fields.clear();
        g.cursor = 0;
        ++g.table_index;
    } catch (...) {
        Report(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "ModNameExport: read failed");
        g.running = false;
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1*) noexcept {}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* api, void** context) noexcept {
    try {
        const Host host(api);
        g.api = api;
        g.core = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
            ANOMALY_CORE_SERVICE_V1_VERSION).get();
        g.storage = host.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID,
            ANOMALY_STORAGE_SERVICE_V1_VERSION).get();
        g.scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID,
            ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
        if (!context || !g.storage || !g.storage->write_atomic || !g.scheduler ||
            !g.scheduler->schedule || !g.scheduler->cancel)
            return {ANOMALY_STATUS_V1_UNAVAILABLE};
        // A profile that does not validate leaves the plugin idle instead of reading wrong bytes.
        if (!g.access.Load(api)) {
            Report(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "ModNameExport: profile unavailable: " + g.access.Error());
            g.access = {};
        }
        *context = &g;
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) noexcept {
    std::scoped_lock lock(g.mutex);
    g.stopping = false;
    g.written = false;
    g.table_index = 0;
    g.cursor = 0;
    g.rows.clear();
    g.fields.clear();
    g.tables.clear();
    g.rows_json = nlohmann::json::array();
    g.document.clear();
    g.task = {};
    if (!g.access.Ready()) return Ok();
    g.running = true;
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) noexcept {
    AnomalyGenerationHandleV1 task{};
    {
        std::scoped_lock lock(g.mutex);
        g.stopping = true;
        g.running = false;
        task = g.task;
        g.task = {};
    }
    if (task.id) g.scheduler->cancel(g.scheduler->user, task);
    return Ok();
}

void ANOMALY_CALL Unload(void*) noexcept {}
} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("anomaly.mod-name-export"), StringView("Mod Name Export"),
        StringView("Anomaly"), StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
