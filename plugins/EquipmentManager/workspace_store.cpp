#include "workspace_store.hpp"
#include "algorithm.hpp"
#include "operations.hpp"
#include "account_archive.hpp"

#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/services/plugin_state.h>
#include <Windows.h>
#include <bcrypt.h>
#include <winsqlite/winsqlite3.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace equipment_manager {
namespace {
using Json = nlohmann::json;
using anomaly::sdk::StringView;
[[noreturn]] void Reject(WorkspaceFailure reason) { throw InvalidWorkspace{reason}; }
void SqlCheck(int status) {
    if (status != SQLITE_OK && status != SQLITE_ROW && status != SQLITE_DONE) Reject(WorkspaceFailure::io_error);
}
std::string NewId() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        Reject(WorkspaceFailure::io_error);
    constexpr char hex[] = "0123456789abcdef";
    std::string value; value.reserve(bytes.size() * 2);
    for (auto byte : bytes) { value += hex[byte >> 4]; value += hex[byte & 15]; }
    return value;
}
std::string Stamp() {
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string Text(const Json& input, const char* key, std::size_t maximum = 160) {
    if (!input.contains(key) || !input[key].is_string()) Reject(WorkspaceFailure::invalid_request);
    auto result = input[key].get<std::string>();
    if (result.empty() || result.size() > maximum || result.find('\0') != result.npos)
        Reject(WorkspaceFailure::invalid_request);
    return result;
}
bool Uid(const Json& value) {
    return value.is_object() && value.contains("slot") && value.contains("serial") &&
        value["slot"].is_number_integer() && value["serial"].is_number_integer() &&
        value["slot"].get<std::uint64_t>() > 0 && value["slot"].get<std::uint64_t>() < UINT32_MAX &&
        value["serial"].get<std::uint64_t>() > 0 && value["serial"].get<std::uint64_t>() < UINT32_MAX;
}
std::string UidKey(const Json& uid) {
    if (!Uid(uid)) Reject(WorkspaceFailure::invalid_plan);
    return std::to_string(uid["slot"].get<std::uint32_t>()) + ':' + std::to_string(uid["serial"].get<std::uint32_t>());
}
Json DefaultPreferences() {
    return {{"filters", {{"search", ""}, {"kind", "all"}, {"state", "all"}, {"owner", "all"}}},
        {"selected_character", nullptr}, {"draft", nullptr}, {"roles", Json::object()}, {"selected_slots", Json::object()}};
}
struct Statement {
    sqlite3_stmt* value{};
    Statement(sqlite3* db, const char* sql) { SqlCheck(sqlite3_prepare_v2(db, sql, -1, &value, nullptr)); }
    ~Statement() { sqlite3_finalize(value); }
    Statement(const Statement&) = delete;
    void Bind(int column, std::string_view text) {
        SqlCheck(sqlite3_bind_text(value, column, text.empty() ? "" : text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT));
    }
    void Bind(int column, std::int64_t integer) { SqlCheck(sqlite3_bind_int64(value, column, integer)); }
    bool Next() { const auto status = sqlite3_step(value); SqlCheck(status); return status == SQLITE_ROW; }
    std::string String(int column) const {
        const auto* text = sqlite3_column_text(value, column);
        return text ? std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(value, column)) : std::string{};
    }
    std::int64_t Integer(int column) const { return sqlite3_column_int64(value, column); }
};
void Execute(sqlite3* db, const char* sql) { SqlCheck(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
struct Transaction {
    sqlite3* db; bool committed{};
    explicit Transaction(sqlite3* database) : db(database) { Execute(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void Commit() { Execute(db, "COMMIT"); committed = true; }
};
std::string RoleId(const Json& character) {
    if (character.contains("role_id") && character["role_id"].is_string()) return character["role_id"].get<std::string>();
    return character.at("item_id").get<std::string>();
}
const Json* FindCharacter(const Json& live, std::string_view id) {
    if (!live.contains("characters")) return nullptr;
    for (const auto& character : live["characters"]) if (RoleId(character) == id) return &character;
    return nullptr;
}
void ValidateCustomBoard(const Json& role) {
    if (!role.contains("allowed_cells") || !role["allowed_cells"].is_array() || role["allowed_cells"].size() != 20)
        Reject(WorkspaceFailure::invalid_plan);
    std::set<std::pair<int, int>> allowed;
    const auto coordinate = [](const Json& point) {
        if (!point.is_array() || point.size() != 2 || !point[0].is_number_integer() || !point[1].is_number_integer())
            Reject(WorkspaceFailure::invalid_plan);
        const auto row = point[0].get<int>(), column = point[1].get<int>();
        if (row < 1 || row > 5 || column < 1 || column > 5) Reject(WorkspaceFailure::invalid_plan);
        return std::make_pair(row, column);
    };
    for (const auto& cell : role["allowed_cells"]) if (!allowed.insert(coordinate(cell)).second) Reject(WorkspaceFailure::invalid_plan);
    if (role.contains("locked_cells")) {
        if (!role["locked_cells"].is_array()) Reject(WorkspaceFailure::invalid_plan);
        std::set<std::pair<int, int>> protected_cells;
        for (const auto& cell : role["locked_cells"]) {
            const auto point = coordinate(cell);
            if (!allowed.contains(point) || !protected_cells.insert(point).second) Reject(WorkspaceFailure::invalid_plan);
        }
    }
}
} // namespace

struct WorkspaceStore::Impl {
    const AnomalyConfigServiceV1* config{};
    sqlite3* db{};
    mutable std::mutex mutex;
    std::map<std::string, AnomalyGenerationHandleV1, std::less<>> schemas;
    std::map<std::string, Json, std::less<>> preferences;
    Json legacy_preferences;
    std::string active, generation_prefix;
    std::uint64_t generation{1}, revision{1};
    struct Transfer {
        std::string generation, name;
        std::vector<std::uint8_t> data;
        std::size_t expected{};
        bool uploading{};
    };
    std::map<std::string, Transfer, std::less<>> transfers;

    std::string Generation() const { return generation_prefix + '-' + std::to_string(generation); }
    void Register(const std::string& id) {
        if (schemas.contains(id)) return;
        constexpr std::string_view schema = R"({"type":"object"})";
        AnomalyGenerationHandleV1 handle{};
        const auto status = config->register_schema(config->user, StringView(id), 1,
            {reinterpret_cast<const std::uint8_t*>(schema.data()), schema.size()}, &handle);
        if (status.code != ANOMALY_STATUS_V1_OK) Reject(WorkspaceFailure::io_error);
        schemas.emplace(id, handle);
    }
    Json ReadConfig(const std::string& id, Json fallback) {
        Register(id);
        std::size_t size{}; std::uint32_t version{};
        const auto measured = config->read(config->user, StringView(id), &version, {}, &size);
        if (measured.code == ANOMALY_STATUS_V1_NOT_FOUND) return fallback;
        if (measured.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL && measured.code != ANOMALY_STATUS_V1_OK)
            Reject(WorkspaceFailure::io_error);
        if (!size || size > 1024U * 1024U || version != 1) Reject(WorkspaceFailure::io_error);
        std::vector<std::uint8_t> bytes(size);
        if (config->read(config->user, StringView(id), &version, {bytes.data(), bytes.size()}, &size).code != 0)
            Reject(WorkspaceFailure::io_error);
        if (size && bytes[size - 1] == 0) --size;
        auto result = Json::parse(bytes.data(), bytes.data() + size, nullptr, false);
        if (!result.is_object()) Reject(WorkspaceFailure::io_error);
        return result;
    }
    void WriteConfig(const std::string& id, const Json& value) {
        Register(id); const auto bytes = value.dump();
        if (config->write_atomic(config->user, StringView(id), 1,
            {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}).code != 0) Reject(WorkspaceFailure::io_error);
    }
    Json& Preferences() {
        auto found = preferences.find(active);
        if (found == preferences.end()) found = preferences.emplace(active, ReadConfig("account-" + active,
            active == "default" ? legacy_preferences : DefaultPreferences())).first;
        auto& value = found->second;
        const auto defaults = DefaultPreferences();
        for (const auto& [key, entry] : defaults.items()) if (!value.contains(key)) value[key] = entry;
        return value;
    }
    void SavePreferences(const Json& value) {
        WriteConfig("account-" + active, value); preferences[active] = value; ++revision;
    }
    void ValidateAccount(const Json& input) const {
        if (!input.is_object() || input.value("account_id", std::string{}) != active ||
            input.value("account_generation", std::string{}) != Generation()) Reject(WorkspaceFailure::stale_account);
    }
    std::string Binding() const {
        Statement query(db, "SELECT binding FROM accounts WHERE id=?"); query.Bind(1, active);
        if (!query.Next()) Reject(WorkspaceFailure::missing_account);
        return query.String(0);
    }
    bool Matches(const Json& live) const {
        const auto key = WorkspaceStore::IdentityKey(live);
        return !key.empty() && key == Binding();
    }
    void RequireLive(const Json& input, const Json& live) const {
        if (!live.value("complete", false) || live.value("phase", std::string{}) != "ready" ||
            !live.contains("snapshot_id") || live["snapshot_id"] != input.value("source_snapshot_id", input.value("snapshot_id", std::string{})))
            Reject(WorkspaceFailure::stale_snapshot);
        if (live.value("source_kind", std::string{}) == "account_snapshot") {
            ValidateAccount(live);
            if (live.at("inventory_snapshot_id") != input.value("inventory_snapshot_id", std::string{}))
                Reject(WorkspaceFailure::stale_snapshot);
            return;
        }
        if (WorkspaceStore::IdentityKey(live).empty()) Reject(WorkspaceFailure::identity_unavailable);
        if (!Matches(live)) Reject(WorkspaceFailure::account_mismatch);
    }
    Json Snapshot(std::string_view id) const {
        Statement query(db, "SELECT document FROM snapshots WHERE account=? AND id=?"); query.Bind(1, active); query.Bind(2, id);
        if (!query.Next()) Reject(WorkspaceFailure::stale_snapshot);
        auto inventory = Json::parse(query.String(0)); inventory.erase("game_account_id"); inventory.erase("message");
        inventory["inventory_snapshot_id"] = id; inventory["snapshot_id"] = "snapshot-" + active + '-' + std::string(id);
        inventory["account_id"] = active; inventory["account_generation"] = Generation();
        inventory["source_identity_verified"] = inventory.value("account_identity_complete", false);
        inventory["account_identity_complete"] = false; inventory["account_service_available"] = false;
        inventory["account_identity_source"] = nullptr; inventory["live_identity_available"] = false;
        inventory["live_account_matches"] = false; inventory["native_assemblable"] = false;
        inventory["source_kind"] = "account_snapshot";
        inventory["complete"] = inventory.value("complete", false) && !inventory.at("characters").empty();
        inventory["computable"] = inventory["complete"];
        return inventory;
    }
    Json ExportAccount(const std::string& id) {
        Statement account(db, "SELECT name,binding FROM accounts WHERE id=?"); account.Bind(1, id);
        if (!account.Next()) Reject(WorkspaceFailure::missing_account);
        Json document{{"schema_version", 1}, {"name", account.String(0)}, {"binding_key", account.String(1)},
            {"preferences", preferences.contains(id) ? preferences.at(id) : ReadConfig("account-" + id,
                id == "default" ? legacy_preferences : DefaultPreferences())},
            {"slots", Json::array()}, {"versions", Json::array()}, {"snapshots", Json::array()}, {"operations", Json::array()}};
        Statement slots(db, "SELECT role,id,name,locked,archived,current_revision FROM slots WHERE account=? ORDER BY rowid"); slots.Bind(1, id);
        while (slots.Next()) document["slots"].push_back({{"role_id", slots.String(0)}, {"id", slots.String(1)},
            {"name", slots.String(2)}, {"locked", slots.Integer(3) != 0}, {"archived", slots.Integer(4) != 0}, {"current_revision", slots.String(5)}});
        Statement versions(db, "SELECT role,slot,id,created,snapshot,document FROM versions WHERE account=? ORDER BY rowid"); versions.Bind(1, id);
        while (versions.Next()) document["versions"].push_back({{"role_id", versions.String(0)}, {"slot_id", versions.String(1)},
            {"id", versions.String(2)}, {"created_at", versions.String(3)}, {"snapshot_id", versions.String(4)}, {"document", Json::parse(versions.String(5))}});
        for (const bool snapshots : {true, false}) {
            Statement rows(db, snapshots ? "SELECT id,created,document FROM snapshots WHERE account=? ORDER BY created" :
                "SELECT id,created,document FROM jobs WHERE account=? ORDER BY created"); rows.Bind(1, id);
            while (rows.Next()) document[snapshots ? "snapshots" : "operations"].push_back({{"id", rows.String(0)},
                {"created_at", rows.String(1)}, {"document", Json::parse(rows.String(2))}});
        }
        return document;
    }
    std::string ImportAccount(const Json& document, const std::string& requested_name) {
        const auto invalid = [] { Reject(WorkspaceFailure::invalid_archive); };
        if (!document.is_object() || document.value("schema_version", 0) != 1 ||
            !document.contains("preferences") || !document["preferences"].is_object() ||
            document["preferences"].dump().size() > 1024U * 1024U) invalid();
        for (const auto* key : {"slots", "versions", "snapshots", "operations"})
            if (!document.contains(key) || !document[key].is_array()) invalid();
        if (document["slots"].size() > 10000 || document["versions"].size() > 50000 ||
            document["snapshots"].size() > 3 || document["operations"].size() > 10000) invalid();
        const auto name = requested_name.empty() ? document.value("name", std::string{}) : requested_name;
        if (name.size() > 160 || name.find('\0') != name.npos) invalid();
        const auto binding = document.value("binding_key", std::string{});
        if ((!binding.empty() && binding.size() != 64) || std::any_of(binding.begin(), binding.end(), [](char value) {
            return !(value >= '0' && value <= '9') && !(value >= 'a' && value <= 'f'); })) invalid();
        std::map<std::string, std::string> slot_revisions; std::set<std::string> version_ids;
        std::map<std::string, unsigned> visible;
        for (const auto& slot : document["slots"]) {
            const auto role = Text(slot, "role_id"), id = Text(slot, "id");
            if (!slot.contains("name") || !slot["name"].is_string() || slot["name"].get_ref<const std::string&>().size() > 160 ||
                !slot.contains("locked") || !slot["locked"].is_boolean() || !slot.contains("archived") || !slot["archived"].is_boolean() ||
                !slot.contains("current_revision") || !slot["current_revision"].is_string() ||
                !slot_revisions.emplace(role + '/' + id, slot["current_revision"].get<std::string>()).second) invalid();
            if (!slot["archived"].get<bool>() && ++visible[role] > 3) invalid();
        }
        for (const auto& version : document["versions"]) {
            const auto slot_key = Text(version, "role_id") + '/' + Text(version, "slot_id");
            const auto id = Text(version, "id");
            if (!slot_revisions.contains(slot_key) || !version_ids.insert(slot_key + '/' + id).second) invalid();
            Text(version, "created_at"); Text(version, "snapshot_id");
            const auto& frozen = version.at("document");
            if (!frozen.is_object() || !frozen.contains("plan") || !frozen["plan"].is_object() ||
                !frozen.contains("items") || !frozen["items"].is_array() || frozen["items"].size() > 26 ||
                !frozen["plan"].contains("placements") || !frozen["plan"]["placements"].is_array() ||
                frozen["plan"]["placements"].size() > 25) invalid();
            std::set<std::string> items;
            for (const auto& item : frozen["items"]) if (!item.is_object() || !item.contains("uid") ||
                !Uid(item["uid"]) || !items.insert(UidKey(item["uid"])).second) invalid();
            std::set<std::string> used;
            const auto include = [&](const Json& uid) {
                if (!Uid(uid) || !items.contains(UidKey(uid)) || !used.insert(UidKey(uid)).second) invalid();
            };
            if (frozen["plan"].contains("core") && !frozen["plan"]["core"].is_null()) include(frozen["plan"]["core"]);
            for (const auto& placement : frozen["plan"]["placements"]) {
                include(placement.at("equipment"));
                if (!placement.at("row").is_number_integer() || !placement.at("column").is_number_integer() ||
                    placement["row"].get<int>() < 1 || placement["row"].get<int>() > 5 ||
                    placement["column"].get<int>() < 1 || placement["column"].get<int>() > 5) invalid();
            }
            if (used != items) invalid();
        }
        for (const auto& [slot, current] : slot_revisions) if (!current.empty() && !version_ids.contains(slot + '/' + current)) invalid();
        for (const bool snapshots : {true, false}) {
            std::set<std::string> ids;
            for (const auto& row : document[snapshots ? "snapshots" : "operations"]) {
                if (!ids.insert(Text(row, "id")).second || !row.at("document").is_object()) invalid();
                Text(row, "created_at");
                if (snapshots && (!row["document"].contains("items") || !row["document"]["items"].is_array() ||
                    !row["document"].contains("characters") || !row["document"]["characters"].is_array() ||
                    row["document"].contains("game_account_id"))) invalid();
            }
        }
        auto imported_preferences = document.at("preferences");
        if ((imported_preferences.contains("roles") && !imported_preferences["roles"].is_object()) ||
            (imported_preferences.contains("selected_slots") && !imported_preferences["selected_slots"].is_object())) invalid();
        const auto id = NewId(); Transaction transaction(db);
        Statement account(db, "INSERT INTO accounts(id,name,binding) VALUES(?,?,?)"); account.Bind(1, id); account.Bind(2, name); account.Bind(3, binding); account.Next();
        for (const auto& slot : document["slots"]) {
            Statement write(db, "INSERT INTO slots(account,role,id,name,locked,archived,current_revision) VALUES(?,?,?,?,?,?,?)");
            write.Bind(1, id); write.Bind(2, slot["role_id"].get<std::string>()); write.Bind(3, slot["id"].get<std::string>());
            write.Bind(4, slot["name"].get<std::string>()); write.Bind(5, std::int64_t(slot["locked"].get<bool>()));
            write.Bind(6, std::int64_t(slot["archived"].get<bool>())); write.Bind(7, slot["current_revision"].get<std::string>()); write.Next();
        }
        for (const auto& version : document["versions"]) {
            Statement write(db, "INSERT INTO versions(account,role,slot,id,created,snapshot,document) VALUES(?,?,?,?,?,?,?)");
            write.Bind(1, id); write.Bind(2, version["role_id"].get<std::string>()); write.Bind(3, version["slot_id"].get<std::string>());
            write.Bind(4, version["id"].get<std::string>()); write.Bind(5, version["created_at"].get<std::string>());
            write.Bind(6, version["snapshot_id"].get<std::string>()); write.Bind(7, version["document"].dump()); write.Next();
        }
        for (const bool snapshots : {true, false}) for (const auto& row : document[snapshots ? "snapshots" : "operations"]) {
            Statement write(db, snapshots ? "INSERT INTO snapshots(account,id,created,document) VALUES(?,?,?,?)" :
                "INSERT INTO jobs(account,id,created,document) VALUES(?,?,?,?)");
            write.Bind(1, id); write.Bind(2, row["id"].get<std::string>()); write.Bind(3, row["created_at"].get<std::string>());
            write.Bind(4, row["document"].dump()); write.Next();
        }
        WriteConfig("account-" + id, imported_preferences); transaction.Commit(); preferences[id] = std::move(imported_preferences);
        ++revision; return id;
    }
    Json Roles(const Json& live, bool include_archived = false) {
        Json result = Json::array(); auto& configs = Preferences()["roles"];
        const bool matches = Matches(live); std::map<std::string, Json, std::less<>> facts;
        Statement saved(db, "SELECT s.role,v.document FROM slots s JOIN versions v ON v.account=s.account AND v.role=s.role AND v.slot=s.id AND v.id=s.current_revision WHERE s.account=? ORDER BY v.created DESC");
        saved.Bind(1, active);
        while (saved.Next()) {
            const auto document = Json::parse(saved.String(1));
            if (!document.contains("role") || !document["role"].value("official", true)) continue;
            const auto& role = document["role"]; const auto id = saved.String(0);
            if (!facts.contains(id) && role.contains("allowed_cells")) {
                auto observed = role.value("observed", Json::object());
                observed["item_id"] = id; observed["name"] = role.at("name"); observed["allowed_cells"] = role.at("allowed_cells");
                facts.emplace(id, std::move(observed));
            }
        }
        Statement snapshots(db, "SELECT document FROM snapshots WHERE account=? ORDER BY created DESC"); snapshots.Bind(1, active);
        while (snapshots.Next()) {
            const auto snapshot = Json::parse(snapshots.String(0));
            for (const auto& character : snapshot.at("characters")) facts.try_emplace(RoleId(character), character);
        }
        if ((matches || live.value("source_kind", std::string{}) == "account_snapshot") && live.contains("characters")) for (const auto& character : live["characters"])
            facts[RoleId(character)] = character;
        for (const auto& [id, character] : facts) {
            const auto* current = matches ? FindCharacter(live, id) : nullptr;
            Json role{{"id", id}, {"name", character.at("name")}, {"official", true}, {"character", nullptr},
                {"allowed_cells", character.at("allowed_cells")}, {"level", character.value("level", Json(nullptr))}};
            if (configs.contains(id) && configs[id].is_object()) role.update(configs[id]);
            role["id"] = id; role["official"] = true; role["character"] = current ? current->at("uid") : Json(nullptr);
            role["native_assemblable"] = current != nullptr; role["observed_from"] = current ? "live_inventory" : "saved_snapshot";
            role["allowed_cells"] = character.at("allowed_cells"); role["observed"] = character;
            if (include_archived || !role.value("archived", false)) result.push_back(std::move(role));
        }
        for (const auto& [id, config_value] : configs.items()) if (config_value.value("official", true) == false &&
            (include_archived || !config_value.value("archived", false))) {
            auto role = config_value; role["id"] = id; role["character"] = nullptr; role["native_assemblable"] = false; result.push_back(std::move(role));
        }
        return result;
    }
    Json Role(const Json& live, std::string_view id) {
        for (const auto& role : Roles(live)) if (role["id"].get_ref<const std::string&>() == id) return role;
        Reject(WorkspaceFailure::missing_role);
    }
    void EnsurePrimary(std::string_view role) {
        Statement insert(db, "INSERT OR IGNORE INTO slots(account,role,id,name) VALUES(?,?,'primary','')");
        insert.Bind(1, active); insert.Bind(2, role); insert.Next();
    }
    Json Slots(std::string_view role, bool include_archived = false) {
        Statement query(db, "SELECT id,name,locked,archived,current_revision FROM slots WHERE account=? AND role=? ORDER BY rowid");
        query.Bind(1, active); query.Bind(2, role); Json result = Json::array();
        while (query.Next()) {
            if (query.Integer(3) && !include_archived) continue;
            Json slot{{"id", query.String(0)}, {"role_id", role}, {"name", query.String(1)},
                {"locked", query.Integer(2) != 0}, {"archived", query.Integer(3) != 0}, {"current_revision", query.String(4)},
                {"plan", nullptr}, {"source_snapshot_id", nullptr}, {"score", nullptr}, {"grade", nullptr}};
            if (!query.String(4).empty()) {
                Statement plan(db, "SELECT snapshot,document FROM versions WHERE account=? AND role=? AND slot=? AND id=?");
                plan.Bind(1, active); plan.Bind(2, role); plan.Bind(3, query.String(0)); plan.Bind(4, query.String(4));
                if (!plan.Next()) Reject(WorkspaceFailure::io_error);
                const auto document = Json::parse(plan.String(1));
                slot["source_snapshot_id"] = plan.String(0); slot["plan"] = document.at("plan");
                slot["score"] = document.value("score", Json(nullptr)); slot["grade"] = document.value("grade", Json(nullptr));
                slot["complete"] = document.value("complete", false);
            }
            result.push_back(std::move(slot));
        }
        return result;
    }
    Json Slot(std::string_view role, std::string_view id) {
        for (const auto& slot : Slots(role)) if (slot["id"].get_ref<const std::string&>() == id) return slot;
        Reject(WorkspaceFailure::missing_slot);
    }
    Json Freeze(const Json& input, const Json& live, const Json& role, bool validate_live = true) {
        if (validate_live) RequireLive(input, live);
        auto plan = input.at("plan");
        plan["character"] = role["character"];
        try { plan = ValidateSavedLoadout(plan, live, role.at("allowed_cells")); }
        catch (const InvalidCommand&) { Reject(WorkspaceFailure::invalid_plan); }
        plan["role_id"] = role.at("id"); plan["native_assemblable"] = !role["character"].is_null();
        std::set<std::string> ids;
        if (!plan.value("core", Json(nullptr)).is_null()) ids.insert(UidKey(plan["core"]));
        for (const auto& placement : plan.at("placements")) ids.insert(UidKey(placement.at("equipment")));
        Json items = Json::array(), scores = Json::array(); double total{}; bool scored = role.contains("weights");
        std::size_t module_cells{};
        for (const auto& item : live.at("items")) if (ids.contains(UidKey(item.at("uid")))) {
            items.push_back(item);
            if (item["kind"] == "module") module_cells += item.at("cells").size();
            if (scored) try { auto score = ScoreEquipment(item, role); score["equipment"] = item["uid"]; total += score["score"].get<double>(); scores.push_back(std::move(score)); }
            catch (const InvalidAlgorithm&) { scored = false; scores = Json::array(); }
        }
        if (items.size() != ids.size()) Reject(WorkspaceFailure::invalid_plan);
        const bool complete = !plan.value("core", Json(nullptr)).is_null() && module_cells == role.at("allowed_cells").size();
        return {{"plan", plan}, {"items", items}, {"role", role}, {"complete", complete},
            {"score", scored ? Json(total) : Json(nullptr)}, {"grade", scored ? Json(GradeLoadout(total)) : Json(nullptr)}, {"assignment_scores", scores}};
    }
    void AddVersion(std::string_view role, std::string_view slot, std::string_view snapshot, const Json& document) {
        const auto id = NewId();
        Statement insert(db, "INSERT INTO versions(account,role,slot,id,created,snapshot,document) VALUES(?,?,?,?,?,?,?)");
        insert.Bind(1, active); insert.Bind(2, role); insert.Bind(3, slot); insert.Bind(4, id);
        insert.Bind(5, Stamp()); insert.Bind(6, snapshot); insert.Bind(7, document.dump()); insert.Next();
        Statement update(db, "UPDATE slots SET current_revision=? WHERE account=? AND role=? AND id=?");
        update.Bind(1, id); update.Bind(2, active); update.Bind(3, role); update.Bind(4, slot); update.Next(); ++revision;
    }
    void ReleaseBorrowed(std::string_view target_role, const Json& incoming) {
        std::set<std::string> borrowed;
        for (const auto& item : incoming.at("items")) borrowed.insert(UidKey(item.at("uid")));
        struct Change { std::string role, slot, snapshot; Json document; };
        std::vector<Change> changes;
        Statement query(db, "SELECT s.role,s.id,s.locked,v.snapshot,v.document FROM slots s JOIN versions v ON v.account=s.account AND v.role=s.role AND v.slot=s.id AND v.id=s.current_revision WHERE s.account=? AND s.role<>? AND s.archived=0");
        query.Bind(1, active); query.Bind(2, target_role);
        while (query.Next()) {
            auto document = Json::parse(query.String(4));
            bool changed{};
            auto& plan = document.at("plan");
            Json placements = Json::array();
            for (auto placement : plan.at("placements")) {
                if (placement.at("equipment").is_null() || !borrowed.contains(UidKey(placement.at("equipment")))) { placements.push_back(std::move(placement)); continue; }
                changed = true;
                if (!plan.contains("placeholders")) plan["placeholders"] = Json::array();
                placement["missing_equipment"] = placement["equipment"]; placement["equipment"] = nullptr;
                plan["placeholders"].push_back(std::move(placement));
            }
            if (!plan.value("core", Json(nullptr)).is_null() && borrowed.contains(UidKey(plan["core"]))) {
                plan["missing_core"] = plan["core"]; plan["core"] = nullptr; changed = true;
            }
            if (!changed) continue;
            if (query.Integer(2)) Reject(WorkspaceFailure::slot_locked);
            plan["placements"] = std::move(placements);
            Json items = Json::array(), scores = Json::array();
            for (const auto& item : document.at("items")) if (!borrowed.contains(UidKey(item.at("uid")))) items.push_back(item);
            for (const auto& score : document.value("assignment_scores", Json::array())) if (!borrowed.contains(UidKey(score.at("equipment")))) scores.push_back(score);
            document["items"] = std::move(items); document["assignment_scores"] = std::move(scores);
            document["score"] = nullptr; document["grade"] = nullptr;
            document["complete"] = false; document["incomplete_reason"] = "equipment_borrowed";
            changes.push_back({query.String(0), query.String(1), query.String(3), std::move(document)});
        }
        for (const auto& change : changes) AddVersion(change.role, change.slot, change.snapshot, change.document);
    }
    Json Reservations() {
        Json result = Json::array();
        Statement query(db, "SELECT s.role,v.document FROM slots s JOIN versions v ON v.account=s.account AND v.role=s.role AND v.slot=s.id AND v.id=s.current_revision WHERE s.account=? AND s.locked=1 AND s.archived=0");
        query.Bind(1, active);
        while (query.Next()) {
            const auto document = Json::parse(query.String(1));
            for (const auto& item : document.at("items"))
                result.push_back({{"role_id", query.String(0)}, {"equipment", item.at("uid")}});
        }
        return result;
    }
    Json View(const Json& live) {
        const auto live_key = WorkspaceStore::IdentityKey(live);
        if (!live_key.empty()) {
            Statement bind(db, "UPDATE accounts SET binding=? WHERE id='default' AND binding='' AND ?='default'");
            bind.Bind(1, live_key); bind.Bind(2, active); bind.Next();
        }
        Json accounts = Json::array(); Statement query(db, "SELECT id,name FROM accounts ORDER BY rowid");
        std::string name;
        while (query.Next()) { accounts.push_back({{"id", query.String(0)}, {"name", query.String(1)}}); if (query.String(0) == active) name = query.String(1); }
        const auto roles = Roles(live); Json slots = Json::array(), archived_roles = Json::array();
        for (const auto& role : Roles(live, true)) if (role.value("archived", false)) archived_roles.push_back(role);
        for (const auto& role : roles) {
            const auto id = role["id"].get<std::string>(); EnsurePrimary(id);
            for (auto slot : Slots(id)) slots.push_back(std::move(slot));
        }
        return {{"account", {{"id", active}, {"name", name}, {"generation", Generation()}, {"binding_key", Binding()}}}, {"accounts", accounts},
            {"roles", roles}, {"archived_roles", archived_roles}, {"slots", slots}, {"reservations", Reservations()},
            {"selected_slots", Preferences()["selected_slots"]}, {"revision", std::to_string(revision)},
            {"save_status", "saved"}, {"live_account_matches", Matches(live)}, {"live_identity_available", !live_key.empty()}};
    }
};

WorkspaceStore::WorkspaceStore() : impl_(std::make_unique<Impl>()) {}
WorkspaceStore::~WorkspaceStore() { Close(); }
void WorkspaceStore::Load(const AnomalyHostApiV1* host, std::uint64_t generation) {
    auto& self = *impl_; const anomaly::sdk::Host api(host);
    self.config = api.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID).get();
    const auto* state = api.Query<AnomalyPluginStateServiceV1>(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID).get();
    if (!self.config || !state || state->struct_size < sizeof(*state) || !state->directory) Reject(WorkspaceFailure::io_error);
    std::array<char, 32768> buffer{}; std::size_t size = buffer.size();
    if (state->directory(state->user, buffer.data(), &size).code != 0 || size < 2 || size > buffer.size()) Reject(WorkspaceFailure::io_error);
    const auto directory = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buffer.data()), size - 1));
    std::filesystem::create_directories(directory);
    const auto file = (directory / L"equipment-workspace.sqlite3").u8string();
    SqlCheck(sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &self.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr));
    sqlite3_busy_timeout(self.db, 500);
    Execute(self.db, "PRAGMA foreign_keys=ON");
    {
        Statement version(self.db, "PRAGMA user_version"); version.Next();
        if (version.Integer(0) != 0 && version.Integer(0) != 1) Reject(WorkspaceFailure::io_error);
    }
    Execute(self.db,
        "CREATE TABLE IF NOT EXISTS accounts(id TEXT PRIMARY KEY,name TEXT NOT NULL,binding TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS slots(account TEXT NOT NULL,role TEXT NOT NULL,id TEXT NOT NULL,name TEXT NOT NULL DEFAULT '',locked INTEGER NOT NULL DEFAULT 0,archived INTEGER NOT NULL DEFAULT 0,current_revision TEXT NOT NULL DEFAULT '',PRIMARY KEY(account,role,id));"
        "CREATE TABLE IF NOT EXISTS versions(account TEXT NOT NULL,role TEXT NOT NULL,slot TEXT NOT NULL,id TEXT NOT NULL,created TEXT NOT NULL,snapshot TEXT NOT NULL,document TEXT NOT NULL,PRIMARY KEY(account,role,slot,id));"
        "CREATE TABLE IF NOT EXISTS snapshots(account TEXT NOT NULL,id TEXT NOT NULL,created TEXT NOT NULL,document TEXT NOT NULL,PRIMARY KEY(account,id));"
        "CREATE TABLE IF NOT EXISTS jobs(account TEXT NOT NULL,id TEXT NOT NULL,created TEXT NOT NULL,document TEXT NOT NULL,PRIMARY KEY(account,id));"
        "INSERT OR IGNORE INTO accounts(id,name) VALUES('default','');PRAGMA user_version=1;");
    self.generation_prefix = std::to_string(generation) + '-' + NewId();
    self.legacy_preferences = self.ReadConfig("settings", DefaultPreferences());
    const auto index = self.ReadConfig("workspace-index", {{"active_account_id", "default"}});
    self.active = index.value("active_account_id", std::string("default"));
    Statement account(self.db, "SELECT 1 FROM accounts WHERE id=?"); account.Bind(1, self.active);
    if (!account.Next()) self.active = "default";
    self.Preferences();
}
void WorkspaceStore::Close() noexcept {
    if (!impl_) return;
    for (const auto& [id, handle] : impl_->schemas) if (impl_->config) impl_->config->unregister_schema(impl_->config->user, handle);
    impl_->schemas.clear();
    if (impl_->db) sqlite3_close_v2(std::exchange(impl_->db, nullptr));
}
std::string WorkspaceStore::IdentityKey(const Json& inventory) {
    if (!inventory.value("account_identity_complete", false) || !inventory.contains("game_account_id") || !inventory["game_account_id"].is_string()) return {};
    const auto raw = inventory["game_account_id"].get<std::string>(); if (raw.empty()) return {};
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(raw.data())), static_cast<ULONG>(raw.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        Reject(WorkspaceFailure::io_error);
    constexpr char hex[] = "0123456789abcdef"; std::string key;
    for (const auto byte : digest) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    return key;
}
bool WorkspaceStore::Handles(std::string_view path) const {
    return path == "/api/workspace" || path == "/api/accounts" || path == "/api/roles" ||
        path.starts_with("/api/accounts/") || path == "/api/slots" || path.starts_with("/api/slots/") ||
        path == "/api/snapshots" || path.starts_with("/api/snapshots/") || path == "/api/history";
}
Json WorkspaceStore::View(const Json& live) { std::scoped_lock lock(impl_->mutex); return impl_->View(live); }
Json WorkspaceStore::Preferences() { std::scoped_lock lock(impl_->mutex); return impl_->Preferences(); }
Json WorkspaceStore::ReadSnapshot(std::string_view id, const Json& owner) const {
    std::scoped_lock lock(impl_->mutex); impl_->ValidateAccount(owner); return impl_->Snapshot(id);
}
void WorkspaceStore::SavePreferences(const Json& value) {
    std::scoped_lock lock(impl_->mutex);
    auto merged = impl_->Preferences();
    for (const auto* key : {"filters", "selected_character", "draft"}) if (value.contains(key)) merged[key] = value[key];
    impl_->SavePreferences(merged);
}
void WorkspaceStore::ValidateAccount(const Json& input) const { std::scoped_lock lock(impl_->mutex); impl_->ValidateAccount(input); }
Json WorkspaceStore::ReservedEquipment(std::string_view except_role) const {
    std::scoped_lock lock(impl_->mutex); auto& self = *impl_; Json result = Json::array(); std::set<std::string> unique;
    for (const auto& reservation : self.Reservations()) if (reservation.at("role_id").get_ref<const std::string&>() != except_role &&
        unique.insert(UidKey(reservation.at("equipment"))).second) result.push_back(reservation.at("equipment"));
    return result;
}
void WorkspaceStore::Capture(const Json& inventory) {
    std::scoped_lock lock(impl_->mutex); auto& self = *impl_;
    self.ValidateAccount(inventory);
    if (!inventory.value("complete", false) || !self.Matches(inventory)) return;
    auto document = inventory; document.erase("game_account_id"); document.erase("message");
    Statement insert(self.db, "INSERT OR IGNORE INTO snapshots(account,id,created,document) VALUES(?,?,?,?)");
    insert.Bind(1, self.active); insert.Bind(2, inventory.at("snapshot_id").get<std::string>()); insert.Bind(3, Stamp()); insert.Bind(4, document.dump()); insert.Next();
    Statement trim(self.db, "DELETE FROM snapshots WHERE account=? AND id NOT IN(SELECT id FROM snapshots WHERE account=? ORDER BY created DESC LIMIT 3)");
    trim.Bind(1, self.active); trim.Bind(2, self.active); trim.Next();
}
void WorkspaceStore::RecordOperation(const Json& operation) {
    std::scoped_lock lock(impl_->mutex); auto& self = *impl_;
    self.ValidateAccount(operation);
    Statement write(self.db, "INSERT OR IGNORE INTO jobs(account,id,created,document) VALUES(?,?,?,?)");
    write.Bind(1, self.active); write.Bind(2, operation.at("id").get<std::string>()); write.Bind(3, Stamp()); write.Bind(4, operation.dump()); write.Next();
}
Json WorkspaceStore::Request(std::string_view method, std::string_view path, const Json& input, const Json& live, std::uint32_t& status) {
    std::scoped_lock lock(impl_->mutex); auto& self = *impl_;
    if (method == "GET" && path == "/api/workspace") return self.View(live);
    if (method == "POST") self.ValidateAccount(input);
    constexpr std::string_view snapshot_prefix = "/api/snapshots/";
    if (method == "GET" && path.starts_with(snapshot_prefix)) {
        const auto suffix = path.substr(snapshot_prefix.size()); const auto separator = suffix.find('/');
        const auto id = suffix.substr(0, separator); auto inventory = self.Snapshot(id); std::size_t cursor{};
        if (separator != suffix.npos) {
            const auto number = suffix.substr(separator + 1);
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), cursor);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) Reject(WorkspaceFailure::invalid_request);
        }
        const auto& items = inventory.at("items");
        if (cursor > items.size()) Reject(WorkspaceFailure::invalid_request);
        const auto end = (std::min)(cursor + 128U, items.size()); auto page = Json::array();
        for (auto index = cursor; index < end; ++index) page.push_back(items[index]);
        inventory["total_items"] = items.size(); inventory["page_start"] = cursor;
        inventory["items_complete"] = end == items.size(); inventory["next_cursor"] = end < items.size() ? Json(end) : Json(nullptr);
        inventory["items"] = std::move(page); return inventory;
    }
    if (method == "POST" && (path == "/api/accounts/export" || path == "/api/accounts/import/start")) {
        const bool uploading = path == "/api/accounts/import/start";
        for (auto entry = self.transfers.begin(); entry != self.transfers.end();) {
            if (entry->second.uploading == uploading) entry = self.transfers.erase(entry); else ++entry;
        }
        Impl::Transfer transfer; transfer.uploading = uploading; transfer.generation = self.Generation();
        if (uploading) {
            if (!input.contains("total_bytes") || !input["total_bytes"].is_number_integer() ||
                input["total_bytes"].get<std::int64_t>() <= 0 || input["total_bytes"].get<std::int64_t>() > kAccountArchiveLimit)
                Reject(WorkspaceFailure::invalid_archive);
            transfer.expected = input["total_bytes"].get<std::size_t>(); transfer.name = input.value("name", std::string{});
            if (transfer.name.size() > 160 || transfer.name.find('\0') != transfer.name.npos) Reject(WorkspaceFailure::invalid_request);
        } else {
            transfer.data = BuildAccountArchive(self.ExportAccount(Text(input, "id")));
            transfer.expected = transfer.data.size();
        }
        const auto id = NewId(); const auto total = transfer.expected; self.transfers.emplace(id, std::move(transfer));
        return {{"transfer_id", id}, {"total_bytes", total}, {"chunk_size", kAccountTransferChunk}};
    }
    constexpr std::string_view export_prefix = "/api/accounts/export/";
    if (method == "GET" && path.starts_with(export_prefix)) {
        const auto suffix = path.substr(export_prefix.size()); const auto slash = suffix.find('/');
        if (slash == suffix.npos) Reject(WorkspaceFailure::invalid_request);
        const auto found = self.transfers.find(suffix.substr(0, slash));
        if (found == self.transfers.end() || found->second.uploading) Reject(WorkspaceFailure::invalid_request);
        auto& transfer = found->second;
        if (transfer.generation != self.Generation()) Reject(WorkspaceFailure::stale_account);
        const auto number = suffix.substr(slash + 1); std::size_t offset{};
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), offset);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || offset >= transfer.data.size() ||
            offset % kAccountTransferChunk) Reject(WorkspaceFailure::invalid_request);
        const auto size = (std::min)(kAccountTransferChunk, transfer.data.size() - offset);
        return {{"offset", offset}, {"next_offset", offset + size}, {"total_bytes", transfer.data.size()},
            {"data_base64", EncodeAccountChunk(std::span<const std::uint8_t>(transfer.data).subspan(offset, size))}};
    }
    if (method == "POST" && (path == "/api/accounts/import/chunk" || path == "/api/accounts/import/finish")) {
        const auto found = self.transfers.find(Text(input, "transfer_id"));
        if (found == self.transfers.end() || !found->second.uploading) Reject(WorkspaceFailure::invalid_request);
        auto& transfer = found->second;
        if (transfer.generation != self.Generation()) Reject(WorkspaceFailure::stale_account);
        if (path == "/api/accounts/import/chunk") {
            if (!input.contains("offset") || !input["offset"].is_number_integer() ||
                input["offset"].get<std::int64_t>() < 0 || input["offset"].get<std::size_t>() != transfer.data.size())
                Reject(WorkspaceFailure::conflict);
            const auto bytes = DecodeAccountChunk(Text(input, "data_base64", ((kAccountTransferChunk + 2) / 3) * 4));
            if (transfer.data.size() + bytes.size() > transfer.expected) Reject(WorkspaceFailure::invalid_archive);
            transfer.data.insert(transfer.data.end(), bytes.begin(), bytes.end());
            return {{"next_offset", transfer.data.size()}};
        }
        if (transfer.data.size() != transfer.expected) Reject(WorkspaceFailure::invalid_archive);
        std::string id;
        try { id = self.ImportAccount(ReadAccountArchive(transfer.data), transfer.name); }
        catch (const Json::exception&) { Reject(WorkspaceFailure::invalid_archive); }
        catch (const InvalidWorkspace& error) {
            if (error.reason == WorkspaceFailure::io_error) throw;
            Reject(WorkspaceFailure::invalid_archive);
        }
        self.transfers.erase(found);
        Json accounts = Json::array(); Statement query(self.db, "SELECT id,name FROM accounts ORDER BY rowid");
        while (query.Next()) accounts.push_back({{"id", query.String(0)}, {"name", query.String(1)}});
        return {{"account_id", id}, {"accounts", accounts}};
    }
    if (method == "POST" && path == "/api/accounts") {
        const auto action = Text(input, "action");
        if (action == "create") {
            const auto id = NewId(); const auto name = Text(input, "name");
            Statement insert(self.db, "INSERT INTO accounts(id,name) VALUES(?,?)"); insert.Bind(1, id); insert.Bind(2, name); insert.Next();
            status = 201; return {{"id", id}, {"accepted", true}};
        }
        const auto id = Text(input, "id");
        Statement check(self.db, "SELECT 1 FROM accounts WHERE id=?"); check.Bind(1, id);
        if (!check.Next()) Reject(WorkspaceFailure::missing_account);
        if (action == "rename") {
            const auto name = Text(input, "name"); Statement update(self.db, "UPDATE accounts SET name=? WHERE id=?"); update.Bind(1, name); update.Bind(2, id); update.Next();
        } else if (action == "select") {
            self.WriteConfig("workspace-index", {{"active_account_id", id}}); self.active = id; ++self.generation; self.Preferences();
        } else if (action == "bind_live") {
            const auto key = IdentityKey(live); if (key.empty()) Reject(WorkspaceFailure::identity_unavailable);
            Statement bind(self.db, "UPDATE accounts SET binding=? WHERE id=?"); bind.Bind(1, key); bind.Bind(2, id); bind.Next();
        } else if (action == "delete") {
            Statement count(self.db, "SELECT COUNT(*) FROM accounts"); count.Next(); if (count.Integer(0) <= 1) Reject(WorkspaceFailure::last_account);
            if (id == self.active) Reject(WorkspaceFailure::conflict);
            Transaction transaction(self.db);
            for (const char* sql : {"DELETE FROM slots WHERE account=?", "DELETE FROM versions WHERE account=?", "DELETE FROM snapshots WHERE account=?", "DELETE FROM jobs WHERE account=?", "DELETE FROM accounts WHERE id=?"}) {
                Statement remove(self.db, sql); remove.Bind(1, id); remove.Next();
            }
            self.WriteConfig("account-" + id, Json::object()); transaction.Commit(); self.preferences.erase(id);
        } else Reject(WorkspaceFailure::invalid_request);
        ++self.revision; return self.View(live);
    }
    if (method == "POST" && path == "/api/roles") {
        const auto action = Text(input, "action"); auto preferences = self.Preferences(); auto& roles = preferences["roles"];
        const auto id = action == "create" ? "custom-" + NewId() : Text(input, "id");
        if (action == "restore") {
            if (!roles.contains(id) || !roles[id].value("archived", false)) Reject(WorkspaceFailure::missing_role);
            roles[id]["archived"] = false;
        }
        else if (action == "archive") {
            self.Role(live, id);
            if (!roles.contains(id)) roles[id] = {{"id", id}, {"official", true}};
            roles[id]["archived"] = true;
        }
        else if (action == "create" || action == "update") {
            if (!input.contains("role") || !input["role"].is_object()) Reject(WorkspaceFailure::invalid_request);
            auto role = action == "update" && roles.contains(id) ? roles[id] : Json::object();
            role.update(input["role"]); role.erase("character"); role["id"] = id;
            if (action == "create") { role["name"] = Text(role, "name"); role["official"] = false; }
            else { const auto previous = self.Role(live, id); role["official"] = previous["official"]; }
            if (role["official"] == false) ValidateCustomBoard(role);
            roles[id] = std::move(role);
        } else Reject(WorkspaceFailure::invalid_request);
        self.SavePreferences(preferences); return self.View(live);
    }
    if (method == "POST" && path == "/api/slots/batch") {
        if (!input.contains("entries") || !input["entries"].is_array() || input["entries"].empty() || input["entries"].size() > 32)
            Reject(WorkspaceFailure::invalid_request);
        self.RequireLive(input, live);
        struct Entry { std::string role, slot; Json frozen; };
        std::vector<Entry> entries; std::set<std::string> roles, equipment;
        for (const auto& entry : input["entries"]) {
            const auto role_id = Text(entry, "role_id"), slot_id = Text(entry, "slot_id");
            if (!roles.insert(role_id).second) Reject(WorkspaceFailure::invalid_request);
            const auto role = self.Role(live, role_id), slot = self.Slot(role_id, slot_id);
            if (slot.at("locked") == true) Reject(WorkspaceFailure::slot_locked);
            if (slot["current_revision"] != entry.value("expected_revision", std::string{})) Reject(WorkspaceFailure::conflict);
            auto frozen = self.Freeze(entry, live, role, false);
            for (const auto& item : frozen["items"]) if (!equipment.insert(UidKey(item.at("uid"))).second) Reject(WorkspaceFailure::duplicate_uid);
            entries.push_back({role_id, slot_id, std::move(frozen)});
        }
        Transaction transaction(self.db);
        for (const auto& entry : entries) self.ReleaseBorrowed(entry.role, entry.frozen);
        for (const auto& entry : entries) self.AddVersion(entry.role, entry.slot, live.at("snapshot_id").get<std::string>(), entry.frozen);
        transaction.Commit(); ++self.revision;
        return {{"accepted", true}, {"saved", entries.size()}};
    }
    if (method == "GET" && path.starts_with("/api/slots/")) {
        const auto suffix = path.substr(std::string_view("/api/slots/").size()); const auto slash = suffix.find('/');
        const auto role = suffix.substr(0, slash); self.Role(live, role); self.EnsurePrimary(role);
        if (slash == suffix.npos) return {{"slots", self.Slots(role)}, {"selected_slot_id", self.Preferences()["selected_slots"].value(std::string(role), std::string("primary"))}};
        const auto tail = suffix.substr(slash + 1); const auto separator = tail.find('/');
        if (separator == tail.npos) {
            const auto slot = self.Slot(role, tail);
            Json result{{"slot", slot}, {"items", Json::array()}, {"assignment_scores", Json::array()}, {"role", self.Role(live, role)}};
            if (slot["current_revision"].get_ref<const std::string&>().empty()) return result;
            Statement query(self.db, "SELECT document FROM versions WHERE account=? AND role=? AND slot=? AND id=?");
            query.Bind(1, self.active); query.Bind(2, role); query.Bind(3, tail); query.Bind(4, slot["current_revision"].get<std::string>());
            if (!query.Next()) Reject(WorkspaceFailure::io_error);
            const auto frozen = Json::parse(query.String(0)); result["items"] = frozen["items"];
            result["assignment_scores"] = frozen.value("assignment_scores", Json::array()); result["role"] = frozen.at("role");
            return result;
        }
        if (tail.substr(separator) != "/history") Reject(WorkspaceFailure::invalid_request);
        const auto slot = tail.substr(0, separator); self.Slot(role, slot); Json revisions = Json::array();
        Statement query(self.db, "SELECT id,created,snapshot,document FROM versions WHERE account=? AND role=? AND slot=? ORDER BY created DESC");
        query.Bind(1, self.active); query.Bind(2, role); query.Bind(3, slot);
        while (query.Next()) { const auto document = Json::parse(query.String(3)); revisions.push_back({{"id", query.String(0)}, {"created_at", query.String(1)}, {"source_snapshot_id", query.String(2)}, {"plan", document["plan"]}, {"score", document.value("score", Json(nullptr))}, {"grade", document.value("grade", Json(nullptr))}}); }
        return {{"revisions", revisions}};
    }
    if (method == "POST" && path == "/api/slots") {
        const auto action = Text(input, "action"), role_id = Text(input, "role_id"); const auto role = self.Role(live, role_id); self.EnsurePrimary(role_id);
        if (action == "create") {
            if (self.Slots(role_id).size() >= 3) Reject(WorkspaceFailure::slot_limit);
            const auto id = NewId(); Statement insert(self.db, "INSERT INTO slots(account,role,id,name) VALUES(?,?,?,?)");
            insert.Bind(1, self.active); insert.Bind(2, role_id); insert.Bind(3, id); insert.Bind(4, Text(input, "name")); insert.Next(); ++self.revision;
            status = 201; return {{"id", id}, {"slots", self.Slots(role_id)}};
        }
        const auto id = Text(input, "slot_id"); const auto slot = self.Slot(role_id, id);
        if (action != "select" && slot["current_revision"] != input.value("expected_revision", std::string{})) Reject(WorkspaceFailure::conflict);
        if (slot["locked"] == true && action != "select" && action != "lock") Reject(WorkspaceFailure::slot_locked);
        if (action == "select") { auto preferences = self.Preferences(); preferences["selected_slots"][role_id] = id; self.SavePreferences(preferences); }
        else if (action == "rename") { Statement update(self.db, "UPDATE slots SET name=? WHERE account=? AND role=? AND id=?"); update.Bind(1, Text(input, "name")); update.Bind(2, self.active); update.Bind(3, role_id); update.Bind(4, id); update.Next(); }
        else if (action == "archive") {
            if (self.Slots(role_id).size() <= 1) Reject(WorkspaceFailure::last_slot);
            Statement update(self.db, "UPDATE slots SET archived=1 WHERE account=? AND role=? AND id=?"); update.Bind(1, self.active); update.Bind(2, role_id); update.Bind(3, id); update.Next();
            auto preferences = self.Preferences();
            if (preferences["selected_slots"].value(role_id, std::string("primary")) == id) {
                preferences["selected_slots"][role_id] = self.Slots(role_id).front()["id"];
                self.SavePreferences(preferences);
            }
        } else if (action == "lock") {
            if (!input.contains("locked") || !input["locked"].is_boolean()) Reject(WorkspaceFailure::invalid_request);
            if (input["locked"] == true && (slot["plan"].is_null() || (slot["plan"].value("core", Json(nullptr)).is_null() && slot["plan"]["placements"].empty()))) Reject(WorkspaceFailure::invalid_plan);
            Statement update(self.db, "UPDATE slots SET locked=? WHERE account=? AND role=? AND id=?"); update.Bind(1, input["locked"].get<bool>() ? 1LL : 0LL); update.Bind(2, self.active); update.Bind(3, role_id); update.Bind(4, id); update.Next();
        } else if (action == "save" || action == "import_game") {
            auto request = input;
            if (action == "import_game") {
                Json plan{{"character", role["character"]}, {"core", nullptr}, {"placements", Json::array()}};
                auto owner = role["character"];
                if (live.value("source_kind", std::string{}) == "account_snapshot") {
                    const auto* observed = FindCharacter(live, role_id);
                    owner = observed ? observed->at("uid") : Json(nullptr);
                }
                for (const auto& item : live.at("items")) if (SameUid(item.at("character"), owner)) {
                    if (item["kind"] == "core") plan["core"] = item["uid"];
                    else plan["placements"].push_back({{"equipment", item["uid"]}, {"row", item["row"]}, {"column", item["column"]}});
                }
                request["plan"] = std::move(plan);
            }
            const auto frozen = self.Freeze(request, live, role); Transaction transaction(self.db);
            self.ReleaseBorrowed(role_id, frozen);
            self.AddVersion(role_id, id, live.at("snapshot_id").get<std::string>(), frozen); transaction.Commit();
        } else if (action == "restore") {
            Statement query(self.db, "SELECT snapshot,document FROM versions WHERE account=? AND role=? AND slot=? AND id=?"); query.Bind(1, self.active); query.Bind(2, role_id); query.Bind(3, id); query.Bind(4, Text(input, "revision_id"));
            if (!query.Next()) Reject(WorkspaceFailure::invalid_plan);
            const auto snapshot = query.String(0);
            const auto document = Json::parse(query.String(1)); Transaction transaction(self.db);
            self.ReleaseBorrowed(role_id, document); self.AddVersion(role_id, id, snapshot, document); transaction.Commit();
        } else Reject(WorkspaceFailure::invalid_request);
        ++self.revision; return {{"slots", self.Slots(role_id)}, {"selected_slot_id", self.Preferences()["selected_slots"].value(role_id, std::string("primary"))}};
    }
    if (method == "GET" && (path == "/api/snapshots" || path == "/api/history")) {
        const bool snapshots = path == "/api/snapshots"; Json entries = Json::array();
        Statement query(self.db, snapshots ? "SELECT id,created,document FROM snapshots WHERE account=? ORDER BY created DESC" : "SELECT id,created,document FROM jobs WHERE account=? ORDER BY created DESC LIMIT 100"); query.Bind(1, self.active);
        while (query.Next()) {
            const auto document = Json::parse(query.String(2));
            if (snapshots) entries.push_back({{"id", query.String(0)}, {"created_at", query.String(1)},
                {"items", document.at("items").size()}, {"characters", document.at("characters").size()},
                {"computable", document.value("complete", false) && !document.at("characters").empty()},
                {"source_kind", "account_snapshot"}, {"native_assemblable", false}});
            else entries.push_back({{"id", query.String(0)}, {"created_at", query.String(1)}, {"document", document}});
        }
        return {{snapshots ? "snapshots" : "operations", entries}, {"account_id", self.active}, {"account_generation", self.Generation()}};
    }
    Reject(WorkspaceFailure::invalid_request);
}
std::string_view WorkspaceErrorCode(WorkspaceFailure reason) noexcept {
    switch (reason) {
    case WorkspaceFailure::stale_account: return "workspace.stale_account";
    case WorkspaceFailure::missing_account: return "workspace.missing_account";
    case WorkspaceFailure::last_account: return "workspace.last_account";
    case WorkspaceFailure::missing_role: return "workspace.missing_role";
    case WorkspaceFailure::missing_slot: return "workspace.missing_slot";
    case WorkspaceFailure::slot_limit: return "workspace.slot_limit";
    case WorkspaceFailure::last_slot: return "workspace.last_slot";
    case WorkspaceFailure::slot_locked: return "workspace.slot_locked";
    case WorkspaceFailure::conflict: return "workspace.conflict";
    case WorkspaceFailure::invalid_plan: return "workspace.invalid_plan";
    case WorkspaceFailure::duplicate_uid: return "workspace.duplicate_uid";
    case WorkspaceFailure::stale_snapshot: return "workspace.stale_snapshot";
    case WorkspaceFailure::identity_unavailable: return "workspace.identity_unavailable";
    case WorkspaceFailure::account_mismatch: return "workspace.account_mismatch";
    case WorkspaceFailure::invalid_archive: return "workspace.invalid_archive";
    case WorkspaceFailure::io_error: return "workspace.io_error";
    default: return "workspace.invalid_request";
    }
}
} // namespace equipment_manager
