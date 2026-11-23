#include "game_bridge.hpp"
#include "operations.hpp"
#include "web_messages.hpp"
#include "automation_api.hpp"
#include "algorithm.hpp"
#include "character_math.hpp"
#include "warehouse_rules.hpp"
#include "reference_stamina_model.hpp"
#include "recommended_weights.hpp"
#include "workspace_store.hpp"
#include "avatar_images.hpp"

#include <anomaly/sdk/localization.hpp>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/web.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstring>
#include <deque>
#include <iterator>
#include <exception>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace equipment_manager {
namespace {
using anomaly::sdk::Host;
using anomaly::sdk::Ok;
using anomaly::sdk::StringView;
using Clock = std::chrono::steady_clock;

struct Operation {
    std::string id, action, status, message;
    std::vector<Json> steps;
    std::size_t completed{};
    std::uint64_t context{}, sent_notification{};
    Clock::time_point sent_at{};
    Json sent;
    bool waiting{}, cancel{};
};

struct Optimization {
    Json request;
    std::shared_ptr<const Json> inventory, result;
    Json source_role_profile;
    Json native_request;
    std::string id, source_snapshot_id, kind{"single"}, status{"queued"}, message, error_code;
    Json warehouse_inputs;
    std::atomic<bool> cancel{};
    std::atomic<std::shared_ptr<const AlgorithmProgressSnapshot>> progress;
    Clock::time_point started{}, finished{};
    bool native_model_pending{};
    bool native_model_reading{};
    bool historical{};
};

// Only frozen values and this job's cancellation/progress mailboxes enter the private thread.
// The Context retains its job until Stop or the Worker completion pump joins it.
struct OptimizationInput {
    Json request, native_request;
    std::shared_ptr<const Json> inventory;
    std::string id, source_snapshot_id, kind;
    bool historical{};
    const std::atomic<bool>* cancel{};
    std::atomic<std::shared_ptr<const AlgorithmProgressSnapshot>>* progress{};
};

struct OptimizationExecution {
    std::shared_ptr<Optimization> job;
    std::shared_ptr<const Json> result;
    std::exception_ptr failure;
    std::atomic<bool> finished{};
    std::thread thread;
};

struct NativeRead {
    Json request, result;
    std::string id, status{"queued"}, account_generation, error_code, message;
    std::uint64_t context{};
};

struct AvatarRefresh {
    std::string id, status{"queued"}, account_generation, account_binding, error_code;
    Json characters = Json::array();
    Json avatars = Json::array();
    bool metadata_ready{};
};

struct Context {
    anomaly::sdk::Localizer localizer;
    std::string locale;
    const AnomalyCoreServiceV1* core{};
    const AnomalyConfigServiceV1* config{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyRuntimeInfoServiceV1* runtime{};
    const AnomalyIpcServiceV1* ipc{};
    const AnomalyWebServiceV1* web{};
    const AnomalyNteAccountServiceV1* account{};
    GameBridge game;
    AvatarImages avatars;
    RecommendedWeightsStore recommended_weights;
    std::shared_ptr<WorkspaceStore> workspace;
    Json workspace_view;
    std::string account_id, account_generation, account_binding;
    bool workspace_busy{}, live_account_matches{};
    std::mutex mutex;
    std::shared_ptr<const Json> inventory = std::make_shared<const Json>(Json{
        {"phase", "waiting"}, {"complete", false}, {"characters", Json::array()}, {"items", Json::array()}});
    Json settings{{"filters", {{"search", ""}, {"kind", "all"}, {"state", "all"}, {"owner", "all"}}},
                  {"selected_character", nullptr}, {"draft", nullptr}};
    Json messages;
    std::deque<Operation> operations;
    std::shared_ptr<Optimization> optimization;
    std::unique_ptr<OptimizationExecution> optimization_execution;
    std::deque<std::shared_ptr<NativeRead>> native_reads;
    std::deque<std::shared_ptr<AvatarRefresh>> avatar_refreshes;
    AnomalyGenerationHandleV1 save_task{}, optimization_task{}, persistence_task{}, avatar_publish_task{}, application{}, automation_endpoint{};
    std::string url, diagnostic, operation_error, model_error, load_error;
    std::uint64_t generation{}, revision{}, live_context{}, next_operation{}, settings_revision{}, saved_revision{};
    std::uint64_t next_optimization{}, next_native_read{};
    std::uint64_t next_avatar_refresh{}, avatar_metadata_reads{};
    std::uint64_t persistence_revision{};
    std::uint64_t workspace_publication{};
    std::string persistence_operation;
    double read_milliseconds{}, maximum_read_milliseconds{};
    bool stopping{}, loaded{};
    bool sync_requested{}, sync_running{};
    std::string sync_status{"idle"};
    std::uint64_t sync_request_id{}, read_attempts{}, snapshot_reads{}, snapshot_notification{};
    Clock::time_point next_read{};

    bool Active() const {
        return !operations.empty() && (operations.back().status == "queued" ||
                                      operations.back().status == "dispatched");
    }
    void Log(std::string_view value) const {
        if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(value));
    }
};

std::string SnapshotId(const Context& c) {
    return c.account_generation + '-' + std::to_string(c.revision);
}

Json AvatarCharactersLocked(const Context& c) {
    std::map<std::string, Json, std::less<>> known;
    for (const auto& role : c.workspace_view.at("roles")) {
        if (!role.contains("observed") || !role.at("observed").is_object()) continue;
        auto character = role.at("observed");
        character["item_id"] = role.at("id");
        known[role.at("id").get<std::string>()] = std::move(character);
    }
    if (c.live_account_matches) for (const auto& character : c.inventory->at("characters"))
        known[character.at("item_id").get<std::string>()] = character;
    Json result = Json::array();
    for (auto& [id, character] : known) result.push_back(std::move(character));
    return result;
}

std::string RoleForRequest(const Context& c, const Json& request) {
    if (request.contains("character") && request["character"].is_object())
        for (const auto& character : c.inventory->at("characters")) if (SameUid(character.at("uid"), request["character"]))
            return character.at("item_id").get<std::string>();
    return request.value("role_id", std::string{});
}

template<class Visitor> void VisitPlanEquipment(const Json& plan, Visitor&& visitor) {
    if (plan.contains("core") && !plan["core"].is_null()) visitor(plan["core"]);
    if (plan.contains("placements")) for (const auto& placement : plan["placements"])
        visitor(placement.at("equipment"));
}

void AddReservations(const Context& c, Json& request) {
    if (request.contains("groups")) {
        for (auto& group : request["groups"]) for (auto& role : group) AddReservations(c, role);
        return;
    }
    const auto role_id = RoleForRequest(c, request);
    auto& excluded = request["exclude_uids"];
    if (excluded.is_null()) excluded = Json::array();
    if (!excluded.is_array()) throw InvalidCommand{CommandFailure::invalid_request};
    for (const auto& reservation : c.workspace_view.at("reservations")) if (reservation.at("role_id") != role_id) {
        const auto& uid = reservation.at("equipment");
        if (std::find(excluded.begin(), excluded.end(), uid) == excluded.end()) excluded.push_back(uid);
    }
}

void ValidateReservations(const Context& c, const Json& request) {
    if (request.contains("plans")) {
        for (const auto& plan : request["plans"]) ValidateReservations(c, plan);
        return;
    }
    const auto role_id = RoleForRequest(c, request);
    const auto check = [&](const Json& uid) {
        for (const auto& reservation : c.workspace_view.at("reservations")) if (reservation.at("role_id") != role_id &&
            SameUid(uid, reservation.at("equipment"))) throw InvalidWorkspace{WorkspaceFailure::slot_locked};
    };
    const auto action = request.value("action", std::string{});
    if (action.empty() || action == "equip_one_key") VisitPlanEquipment(request, check);
    else if (action == "equip_module" || action == "equip_core" || action == "replace_core" ||
        action == "move_module_to_character" || action == "move_core_to_character") check(request.at("equipment"));
}

Json WarehouseInputsLocked(const Context& c) {
    Json roles = Json::array(), excluded = Json::array();
    for (const auto& source : c.workspace_view.at("roles")) {
        Json role{{"id", source.at("id")}};
        for (const auto* key : {"weights", "main_weights", "usable_shape_ids", "usable_suit_ids"})
            if (source.contains(key)) role[key] = source[key];
        roles.push_back(std::move(role));
    }
    Json reservations = Json::object(); AddReservations(c, reservations); excluded = reservations.at("exclude_uids");
    std::sort(excluded.begin(), excluded.end());
    return {{"roles", roles}, {"exclude_uids", excluded}};
}

void UseHistoricalRole(const Context& c, Json& request, const Json& inventory) {
    if (request.contains("groups")) {
        for (auto& group : request["groups"]) for (auto& role : group) UseHistoricalRole(c, role, inventory);
        return;
    }
    const auto id = request.at("role_id").get<std::string>();
    for (const auto& role : c.workspace_view.at("roles")) if (role.at("id") == id) {
        request["character"] = nullptr; request["allowed_cells"] = role.at("allowed_cells");
        request["board_source"] = role.value("official", true) ? "saved_role_definition" : "custom_profile";
        if (role.value("official", true)) {
            for (const auto& character : inventory.at("characters")) if (character.at("item_id") == id) {
                request["allowed_cells"] = character.at("allowed_cells"); request["board_source"] = "inventory_snapshot"; break;
            }
        }
        request["native_assemblable"] = false; return;
    }
    throw InvalidAlgorithm{AlgorithmFailure::missing_character};
}

std::string FailureMessage(const Context& c, CommandFailure reason) {
    switch (reason) {
    case CommandFailure::missing_character:
        return c.localizer.Text("command.character_missing", "The character is no longer in the current inventory");
    case CommandFailure::missing_equipment:
        return c.localizer.Text("command.equipment_missing", "The equipment is no longer in the current inventory");
    case CommandFailure::wrong_type:
        return c.localizer.Text("command.wrong_type", "The equipment type does not match this operation");
    case CommandFailure::invalid_layout:
        return c.localizer.Text("command.invalid_layout", "The layout has unknown shapes or extends outside the equipment grid");
    case CommandFailure::occupied:
        return c.localizer.Text("command.occupied", "Equipment overlaps an occupied grid cell");
    case CommandFailure::duplicate_uid:
        return c.localizer.Text("command.duplicate_uid", "A character, equipment item or state field is selected more than once");
    case CommandFailure::unsupported_action:
        return c.localizer.Text("command.unsupported", "This operation is not supported");
    default:
        return c.localizer.Text("command.invalid", "The operation contains invalid or missing fields");
    }
}

std::string AlgorithmMessage(const Context& c, AlgorithmFailure reason) {
    switch (reason) {
    case AlgorithmFailure::inventory_unavailable:
        return c.localizer.Text("algorithm.inventory_unavailable", "A complete live inventory is required for optimization");
    case AlgorithmFailure::stale_snapshot:
        return c.localizer.Text("algorithm.stale_snapshot", "The inventory has changed Refresh before optimizing");
    case AlgorithmFailure::missing_character:
        return c.localizer.Text("algorithm.missing_character", "The selected character is no longer in the inventory");
    case AlgorithmFailure::invalid_inventory:
        return c.localizer.Text("algorithm.invalid_inventory", "The live inventory does not satisfy the optimization contract");
    case AlgorithmFailure::incomplete_attributes:
        return c.localizer.Text("algorithm.incomplete_attributes", "Verified sub-stat data is missing");
    case AlgorithmFailure::incomplete_main_stat:
        return c.localizer.Text("algorithm.incomplete_main_stat", "Verified core main-stat data is missing");
    case AlgorithmFailure::incomplete_shape:
        return c.localizer.Text("algorithm.incomplete_shape", "A module shape has not been verified");
    case AlgorithmFailure::incomplete_set:
        return c.localizer.Text("algorithm.incomplete_set", "Verified core set requirements are missing");
    case AlgorithmFailure::unsupported_quality:
        return c.localizer.Text("algorithm.unsupported_quality", "This equipment quality is not supported by the scoring rules");
    case AlgorithmFailure::cancelled:
        return c.localizer.Text("algorithm.cancelled", "Optimization was cancelled");
    case AlgorithmFailure::budget_exhausted:
        return c.localizer.Text("algorithm.budget_exhausted", "The calculation reached its budget before completion");
    default:
        return c.localizer.Text("algorithm.invalid_request", "The optimization request is invalid");
    }
}

std::string WorkspaceMessage(const Context& c, WorkspaceFailure reason) {
    switch (reason) {
    case WorkspaceFailure::stale_account: return c.localizer.Text("workspace.stale_account", "The selected account has changed Refresh before continuing");
    case WorkspaceFailure::missing_account: return c.localizer.Text("workspace.missing_account", "The selected account no longer exists");
    case WorkspaceFailure::last_account: return c.localizer.Text("workspace.last_account", "Keep at least one account");
    case WorkspaceFailure::missing_role: return c.localizer.Text("workspace.missing_role", "The selected character profile no longer exists");
    case WorkspaceFailure::missing_slot: return c.localizer.Text("workspace.missing_slot", "The selected loadout slot no longer exists");
    case WorkspaceFailure::slot_limit: return c.localizer.Text("workspace.slot_limit", "A character can have at most three visible loadout slots");
    case WorkspaceFailure::last_slot: return c.localizer.Text("workspace.last_slot", "Keep at least one visible loadout slot for this character");
    case WorkspaceFailure::slot_locked: return c.localizer.Text("workspace.slot_locked", "Unlock the saved loadout before changing or borrowing its equipment");
    case WorkspaceFailure::conflict: return c.localizer.Text("workspace.conflict", "The saved loadout changed Refresh before continuing");
    case WorkspaceFailure::invalid_plan: return c.localizer.Text("workspace.invalid_plan", "The loadout does not match the current inventory or character board");
    case WorkspaceFailure::duplicate_uid: return c.localizer.Text("workspace.duplicate_uid", "The requested loadouts repeat an equipment UID");
    case WorkspaceFailure::stale_snapshot: return c.localizer.Text("workspace.stale_snapshot", "Refresh the live inventory before saving this loadout");
    case WorkspaceFailure::identity_unavailable: return c.localizer.Text("workspace.identity_unavailable", "The current game account identity has not been verified");
    case WorkspaceFailure::account_mismatch: return c.localizer.Text("workspace.account_mismatch", "The selected software account is not linked to the current game account");
    case WorkspaceFailure::invalid_archive: return c.localizer.Text("workspace.invalid_archive", "The account archive is invalid or unsupported");
    case WorkspaceFailure::io_error: return c.localizer.Text("workspace.io_error", "The workspace could not be saved or read");
    default: return c.localizer.Text("workspace.invalid_request", "The workspace request is invalid");
    }
}

bool PublishWorkspace(Context& c, Json view, Json preferences,
    const std::shared_ptr<const Json>& source = {}, std::uint64_t expected_publication = 0) {
    std::scoped_lock lock(c.mutex);
    // A GET may finish after a sync or an account/settings mutation. It must
    // not overwrite the workspace that was published by the newer request.
    if (source && (c.workspace_busy || source != c.inventory || expected_publication != c.workspace_publication))
        return false;
    const auto& account = view.at("account");
    const bool changed = account.at("id") != c.account_id || account.at("generation") != c.account_generation;
    c.account_id = account.at("id").get<std::string>(); c.account_generation = account.at("generation").get<std::string>();
    c.account_binding = account.at("binding_key").get<std::string>();
    c.live_account_matches = view.at("live_account_matches").get<bool>();
    c.workspace_view = std::move(view);
    ++c.workspace_publication;
    if (changed) {
        c.settings = std::move(preferences);
        if (c.optimization) c.optimization->cancel.store(true, std::memory_order_release);
        c.optimization.reset(); c.operations.clear();
        c.persistence_revision = 0; c.persistence_operation.clear();
    }
    return true;
}

void PersistWorkspace(Context& c) {
    Json inventory, operations = Json::array();
    std::shared_ptr<const Json> snapshot;
    std::string snapshot_id, account_id, account_generation;
    {
        std::scoped_lock lock(c.mutex);
        snapshot = c.inventory; snapshot_id = SnapshotId(c);
        account_id = c.account_id; account_generation = c.account_generation;
        for (const auto& operation : c.operations) if (operation.status != "queued" && operation.status != "dispatched")
            operations.push_back({{"id", operation.id}, {"action", operation.action}, {"status", operation.status},
                {"message", operation.message}, {"completed", operation.completed}, {"total", operation.steps.size()},
                {"account_id", c.account_id}, {"account_generation", c.account_generation}});
    }
    inventory = *snapshot; inventory["snapshot_id"] = snapshot_id;
    inventory["account_id"] = account_id; inventory["account_generation"] = account_generation;
    c.workspace->Capture(inventory);
    for (const auto& operation : operations) c.workspace->RecordOperation(operation);
}

void ANOMALY_CALL PersistenceWorker(void* user, AnomalyGenerationHandleV1) noexcept {
    auto& c = *static_cast<Context*>(user);
    try { PersistWorkspace(c); }
    catch (const InvalidWorkspace& error) {
        if (error.reason != WorkspaceFailure::stale_account) c.Log("EquipmentManager: workspace persistence failed");
    }
    catch (...) { c.Log("EquipmentManager: workspace persistence failed"); }
    std::scoped_lock lock(c.mutex); c.persistence_task = {};
}

void SchedulePersistenceLocked(Context& c) {
    if (c.stopping || c.Active() || c.persistence_task.id || !c.live_account_matches) return;
    const auto terminal = !c.operations.empty() && !c.Active() ? c.operations.back().id : c.persistence_operation;
    if (c.persistence_revision == c.revision && terminal == c.persistence_operation) return;
    c.persistence_revision = c.revision; c.persistence_operation = terminal;
    if (c.scheduler->schedule(c.scheduler->user, 0, PersistenceWorker, &c, &c.persistence_task).code != 0) {
        c.persistence_task = {}; c.Log("EquipmentManager: workspace persistence could not be scheduled");
    }
}

Json WorkspaceApi(Context& c, std::string_view method, std::string_view path, const Json& input, std::uint32_t& status) {
    Json inventory;
    std::shared_ptr<const Json> snapshot;
    std::string snapshot_id;
    std::uint64_t publication{};
    const bool mutation = method == "POST";
    const bool selection = mutation && path == "/api/accounts" && input.value("action", std::string{}) == "select";
    // The caller holds c.mutex. Live identity can advance independently of the
    // saved workspace, so its flags come from the current published inventory.
    const auto published_view = [&] {
        auto view = c.workspace_view;
        view["live_account_matches"] = c.live_account_matches;
        view["live_identity_available"] = c.inventory->value("account_identity_complete", false);
        return view;
    };
    {
        std::scoped_lock lock(c.mutex);
        if (!c.stopping && c.workspace_busy && !mutation && path == "/api/workspace") return published_view();
        if (c.stopping || c.workspace_busy || (mutation && (c.Active() || (selection && (c.save_task.id || c.persistence_task.id)) ||
            (c.optimization && (c.optimization->status == "queued" || c.optimization->status == "running")))))
            throw InvalidWorkspace{WorkspaceFailure::conflict};
        if (mutation) c.workspace_busy = true;
        snapshot = c.inventory; snapshot_id = SnapshotId(c); publication = c.workspace_publication;
    }
    try {
        inventory = *snapshot; inventory["snapshot_id"] = snapshot_id;
        Json historical;
        if (mutation && (path == "/api/slots" || path == "/api/slots/batch") && input.contains("inventory_snapshot_id"))
            historical = c.workspace->ReadSnapshot(input.at("inventory_snapshot_id").get<std::string>(), input);
        const auto& source = historical.is_null() ? inventory : historical;
        auto result = c.workspace->Request(method, path, input, source, status);
        if (mutation || path == "/api/workspace") {
            auto view = path == "/api/workspace" ? result : c.workspace->View(inventory);
            auto preferences = c.workspace->Preferences();
            if (!PublishWorkspace(c, std::move(view), std::move(preferences), mutation ? nullptr : snapshot, publication)) {
                std::scoped_lock lock(c.mutex);
                result = published_view();
            }
        }
        if (mutation) { std::scoped_lock lock(c.mutex); c.workspace_busy = false; }
        return result;
    } catch (...) {
        if (mutation) { std::scoped_lock lock(c.mutex); c.workspace_busy = false; }
        throw;
    }
}

Json OptimizationProgress(const Optimization& job) {
    const auto sample = job.progress.load(std::memory_order_acquire);
    const bool terminal = job.status != "queued" && job.status != "running";
    const auto end = terminal ? job.finished : Clock::now();
    const auto elapsed = job.started == Clock::time_point{} ? std::int64_t{} :
        std::chrono::duration_cast<std::chrono::milliseconds>(end - job.started).count();
    const auto phase = terminal ? std::string("finished") :
        job.native_model_pending || job.native_model_reading ? std::string("reading_model") :
        job.status == "queued" ? std::string("queued") :
        sample ? std::string(AlgorithmProgressPhaseName(sample->phase)) : std::string("computing");
    Json result{{"phase", phase}, {"elapsed_ms", elapsed},
        {"budget_elapsed_ms", sample ? Json(sample->elapsed_ms) : Json(nullptr)},
        {"time_limit_ms", sample ? Json(sample->time_limit_ms) : Json(nullptr)},
        {"nodes", sample ? Json(sample->nodes) : Json(nullptr)},
        {"max_nodes", sample ? Json(sample->max_nodes) : Json(nullptr)},
        {"combinations", sample && sample->combinations_available ? Json(sample->combinations) : Json(nullptr)},
        {"combination_budget", sample && sample->combinations_available ? Json(sample->combination_budget) : Json(nullptr)}};
    const bool team = sample && sample->multi_role;
    result["current_group"] = team && sample->current_group && !terminal ? Json(sample->current_group) : Json(nullptr);
    result["groups_total"] = team ? Json(sample->groups_total) : Json(nullptr);
    result["groups_visited"] = team ? Json(sample->groups_visited) : Json(nullptr);
    result["roles_completed"] = team ? Json(sample->roles_completed) : Json(nullptr);
    result["roles_total"] = team ? Json(sample->roles_total) : Json(nullptr);
    return result;
}

Json OptimizationLocked(const Context& c, std::shared_ptr<const Json>* payload = nullptr) {
    if (!c.optimization) return {{"status", "idle"},
        {"message", c.localizer.Text("algorithm.idle", "No optimization has been requested")}};
    const auto& job = *c.optimization;
    Json result{{"id", job.id}, {"status", job.status}, {"source_snapshot_id", job.source_snapshot_id},
        {"kind", job.kind},
        {"stale", !job.historical && (!c.inventory->value("complete", false) || job.source_snapshot_id != SnapshotId(c))},
        {"historical", job.historical}, {"message", job.message},
        {"cancel_requested", job.cancel.load(std::memory_order_acquire)}, {"progress", OptimizationProgress(job)}};
    if (job.historical) result["inventory_snapshot_id"] = job.request.at("inventory_snapshot_id");
    if (job.kind == "panel" || job.kind == "contributions" || job.kind == "replacements") {
        bool same_profile{};
        for (const auto& role : c.workspace_view.at("roles")) if (role.at("id") == job.request.at("role_id"))
            same_profile = role.value("profile", Json::object()) == job.source_role_profile;
        result["profile_stale"] = !same_profile;
        result["stale"] = result["stale"].get<bool>() || !same_profile;
    }
    if (job.kind == "warehouse_rules" && job.warehouse_inputs != WarehouseInputsLocked(c))
        result["stale"] = true;
    if (!job.error_code.empty()) result["error_code"] = job.error_code;
    if (payload) *payload = job.result;
    return result;
}

Json OptimizationApi(Context& c, std::string_view path) {
    Json result;
    std::shared_ptr<const Json> payload;
    std::size_t cursor{};
    {
        std::scoped_lock lock(c.mutex);
        constexpr std::string_view prefix = "/api/optimize/results/";
        if (path.starts_with(prefix)) {
            const auto suffix = path.substr(prefix.size());
            const auto separator = suffix.find('/');
            if (separator == suffix.npos || !c.optimization || suffix.substr(0, separator) != c.optimization->id)
                throw InvalidCommand{CommandFailure::invalid_request};
            const auto number = suffix.substr(separator + 1);
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), cursor);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                throw InvalidCommand{CommandFailure::invalid_request};
        }
        result = OptimizationLocked(c, &payload);
    }
    if (!payload) return result;
    if (result.at("kind") == "blueprints" && payload->contains("blueprints")) {
        auto& page_result = result["result"] = Json::object();
        for (const auto& [key, value] : payload->items()) if (key != "blueprints") page_result[key] = value;
        const auto& rows = payload->at("blueprints");
        if (cursor > rows.size()) throw InvalidCommand{CommandFailure::invalid_request};
        const auto end = (std::min)(cursor + 200U, rows.size());
        auto& page = page_result["blueprints"] = Json::array();
        for (auto index = cursor; index < end; ++index) page.push_back(rows[index]);
        result["page_start"] = cursor; result["total_results"] = rows.size();
        result["next_cursor"] = end < rows.size() ? Json(end) : Json(nullptr);
    } else result["result"] = *payload;
    return result;
}

Json ComputeOptimization(const OptimizationInput& input) {
    Json result;
    Json inventory = input.inventory ? *input.inventory : Json();
    if (input.inventory) inventory["snapshot_id"] = input.source_snapshot_id;
    const auto cancelled = [&] { return input.cancel->load(std::memory_order_acquire); };
    const AlgorithmProgressCallback progress = [mailbox = input.progress](const AlgorithmProgressSnapshot& sample) {
        mailbox->store(std::make_shared<const AlgorithmProgressSnapshot>(sample), std::memory_order_release);
    };
    if (cancelled()) throw InvalidAlgorithm{AlgorithmFailure::cancelled};
    if (input.kind == "panel" || input.kind == "contributions" || input.kind == "replacements") {
        if (input.request.value("complete", false)) {
            if (input.kind == "contributions") result = CalculateEquipmentContributions(input.request, cancelled);
            else if (input.kind == "replacements") {
                auto ranking = input.request; ranking["model"] = input.request;
                ranking["equipment"] = input.native_request.at("equipment");
                result = RankEquipmentReplacements(ranking, inventory, cancelled);
            } else result = CalculateCharacter(input.request, cancelled);
            if (!result.contains("complete")) result["complete"] = true;
        } else {
            result = {{"available", false}, {"complete", false}, {"sources", input.request.at("sources")}, {"missing", Json::array()}};
            for (const auto& source : input.request.at("sources")) if (!source.value("complete", false))
                result["missing"].push_back({{"source", source.at("id")}, {"reason_code", source.value("reason_code", std::string("source_incomplete"))}});
        }
        result["role_id"] = input.request.at("role_id");
        result["profile"] = input.request.at("profile");
        result["attributes_metadata"] = input.request.value("attributes_metadata", Json::array());
        result["equipment_projection"] = "maximum_level";
        result["source_kind"] = "live_data_table";
        result["character_level"] = input.request.at("character_level");
    }
    else if (input.kind == "experience") result = CalculateExperienceMaterials(input.request, cancelled);
    else if (input.kind == "cultivation") {
        result = CalculateCultivation(input.request, cancelled);
        result["source_plans"] = input.native_request.at("plans");
        for (const auto* key : {"source_kind", "owned_complete", "owned_source", "owned_includes_bound", "owned_scope", "owned_gaps"})
            if (input.request.contains(key)) result[key] = input.request[key];
        result["source_tracks"] = Json::array();
        for (const auto& plan : input.request.at("plans")) {
            Json source{{"id", plan.at("id")}, {"role_id", plan.at("role_id")}, {"tracks", Json::array()}};
            for (const auto* key : {"from_source", "incomplete_tracks", "gaps"}) if (plan.contains(key)) source[key] = plan[key];
            for (const auto& track : plan.at("tracks")) {
                Json detail{{"id", track.at("id")}};
                for (const auto* key : {"current", "target", "provenance", "complete", "reason_code", "experience_scope"})
                    if (track.contains(key)) detail[key] = track[key];
                source["tracks"].push_back(std::move(detail));
            }
            result["source_tracks"].push_back(std::move(source));
        }
    }
    else if (input.kind == "stamina") result = CalculateStamina(input.request, cancelled);
    else if (input.kind == "warehouse_rules") {
        result = PreviewWarehouseRules(input.request, inventory, cancelled);
        result["preview_id"] = input.id;
        result["rules_snapshot"] = input.request.at("rules");
    }
    else if (input.kind == "multi") result = OptimizeLoadouts(input.request, inventory, cancelled, progress);
    else if (input.kind == "top_k") result = OptimizeTopK(input.request, inventory, cancelled, progress);
    else if (input.kind == "blueprints") {
        result = GenerateBlueprints(input.request, cancelled, progress);
        result["allowed_cells"] = input.request.at("allowed_cells");
        result["shapes"] = input.request.at("shapes");
    }
    else result = OptimizeLoadout(input.request, inventory, cancelled, progress);
    if (input.historical) {
        result["inventory_snapshot_id"] = input.request.at("inventory_snapshot_id");
        result["native_assemblable"] = false;
        result["board_sources"] = Json::array();
        const auto append_board = [&](const Json& request) {
            if (request.contains("role_id") && request.contains("allowed_cells"))
                result["board_sources"].push_back({{"role_id", request.at("role_id")},
                    {"allowed_cells", request.at("allowed_cells")}, {"source", request.value("board_source", std::string("saved_role_definition"))}});
        };
        if (input.request.contains("groups")) for (const auto& group : input.request.at("groups"))
            for (const auto& request : group) append_board(request);
        else append_board(input.request);
        if (input.kind == "panel" || input.kind == "contributions" || input.kind == "replacements")
            result["source_kind"] = "live_data_table_and_account_snapshot";
    }
    return result;
}

void ANOMALY_CALL OptimizeWorker(void* user, AnomalyGenerationHandleV1) noexcept {
    auto& c = *static_cast<Context*>(user);
    OptimizationExecution* execution{};
    std::shared_ptr<Optimization> job;
    bool schedule_failed{};
    try {
        {
            std::scoped_lock lock(c.mutex);
            c.optimization_task = {};
            if (c.stopping) return;
            if (!c.optimization_execution) {
                job = c.optimization;
                if (!job || job->native_model_pending || job->status != "queued") return;
                job->status = "running";
                job->started = Clock::now();
                job->message = c.localizer.Text("algorithm.running", "Searching equipment combinations");
                OptimizationInput input{job->request, job->native_request, job->inventory,
                    job->id, job->source_snapshot_id, job->kind, job->historical, &job->cancel, &job->progress};
                c.optimization_execution = std::make_unique<OptimizationExecution>();
                execution = c.optimization_execution.get();
                execution->job = job;
                // Long searches cannot retain a Worker callback lease: HTTP
                // cancellation uses Worker too, and host drain precedes Stop.
                // This thread performs no SDK calls or Context publication.
                execution->thread = std::thread([execution, input = std::move(input)] {
                    try { execution->result = std::make_shared<const Json>(ComputeOptimization(input)); }
                    catch (...) { execution->failure = std::current_exception(); }
                    execution->finished.store(true, std::memory_order_release);
                });
            } else {
                execution = c.optimization_execution.get();
                job = execution->job;
                if (job != c.optimization) job->cancel.store(true, std::memory_order_release);
            }
            if (!execution->finished.load(std::memory_order_acquire)) {
                if (c.scheduler->schedule(c.scheduler->user, 50, OptimizeWorker, &c, &c.optimization_task).code == ANOMALY_STATUS_V1_OK)
                    return;
                // A frozen generation cannot queue another completion pump.
                // Cancel and join here before returning its current lease.
                schedule_failed = true;
                job->cancel.store(true, std::memory_order_release);
            }
        }
        if (execution->thread.joinable()) execution->thread.join();
        std::string status, message, error_code;
        try {
            if (schedule_failed) {
                status = "failed"; error_code = "algorithm.schedule_failed";
                message = c.localizer.Text("algorithm.schedule_failed", "Optimization could not be scheduled");
            } else {
                if (execution->failure) std::rethrow_exception(execution->failure);
                if (job->cancel.load(std::memory_order_acquire)) throw InvalidAlgorithm{AlgorithmFailure::cancelled};
                status = "completed";
                message = c.localizer.Text("algorithm.completed", "Optimization finished Review the preview before applying it");
            }
        } catch (const InvalidAlgorithm& failure) {
            status = failure.reason == AlgorithmFailure::cancelled ? "cancelled" : "failed";
            error_code = AlgorithmErrorCode(failure.reason);
            message = AlgorithmMessage(c, failure.reason);
        } catch (...) {
            status = "failed"; error_code = "algorithm.failed";
            message = c.localizer.Text("algorithm.failed", "Optimization could not be completed");
        }
        const auto source = job->inventory;
        std::scoped_lock lock(c.mutex);
        if (job == c.optimization && !c.stopping) {
            job->status = std::move(status);
            job->finished = Clock::now();
            job->message = std::move(message);
            job->error_code = std::move(error_code);
            job->result = std::move(execution->result);
            job->inventory.reset();
        }
        c.optimization_execution.reset();
    } catch (...) {
        // Thread creation and completion publication failures use the same
        // ownership boundary; a joinable thread never escapes the Context.
        {
            std::scoped_lock lock(c.mutex);
            execution = c.optimization_execution.get();
            if (execution) execution->job->cancel.store(true, std::memory_order_release);
        }
        if (execution && execution->thread.joinable()) execution->thread.join();
        std::scoped_lock lock(c.mutex);
        c.optimization_task = {};
        c.optimization_execution.reset();
        if (job && job == c.optimization) {
            job->status = "failed";
            job->finished = Clock::now();
            job->error_code = "algorithm.failed";
            job->message = c.localizer.Text("algorithm.failed", "Optimization could not be completed");
        }
    }
}

Json StateApi(Context& c, std::string_view path, std::uint32_t& status) {
    Json result = Json::object();
    std::shared_ptr<const Json> inventory;
    std::string snapshot_id;
    {
        std::scoped_lock lock(c.mutex);
        inventory = c.inventory; snapshot_id = SnapshotId(c);
        result["snapshot_id"] = snapshot_id;
        result["account_id"] = c.account_id; result["account_generation"] = c.account_generation;
        result["live_account_matches"] = c.live_account_matches;
        result["sync_pending"] = c.sync_requested || c.sync_running;
        result["sync_status"] = c.sync_status; result["sync_request_id"] = c.sync_request_id;
        result["settings_status"] = c.settings_revision == c.saved_revision ? "saved" : c.save_task.id ? "saving" : "failed";
        result["operations"] = Json::array();
        for (const auto& operation : c.operations)
            result["operations"].push_back({{"id", operation.id}, {"action", operation.action},
                {"status", operation.status}, {"message", operation.message},
                {"completed", operation.completed}, {"total", operation.steps.size()}});
    }
    std::size_t cursor{};
    constexpr std::string_view prefix = "/api/state/";
    if (path.starts_with(prefix)) {
        const auto parts = path.substr(prefix.size());
        const auto separator = parts.find('/');
        if (separator == parts.npos) throw InvalidCommand{CommandFailure::invalid_request};
        if (parts.substr(0, separator) != snapshot_id) {
            status = 409;
            return {{"error_code", "command.stale"}, {"message", c.localizer.Text("command.stale", "The inventory has changed Refresh the selection before applying it")}};
        }
        const auto number = parts.substr(separator + 1);
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), cursor);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || cursor > inventory->at("items").size())
            throw InvalidCommand{CommandFailure::invalid_request};
    }
    for (const auto& [key, value] : inventory->items())
        if (key != "items" && key != "game_account_id") result[key] = value;
    const auto& items = inventory->at("items");
    const auto end = (std::min)(cursor + 128U, items.size());
    result["items"] = Json::array();
    for (std::size_t i = cursor; i < end; ++i) result["items"].push_back(items[i]);
    result["total_items"] = items.size();
    result["page_start"] = cursor;
    result["items_complete"] = end == items.size();
    result["next_cursor"] = end < items.size() ? Json(end) : Json(nullptr);
    result["live_identity_available"] = inventory->value("account_identity_complete", false);
    return result;
}

std::uint64_t SaveSettings(Context& c) {
    std::string document;
    std::uint64_t revision{};
    {
        std::scoped_lock lock(c.mutex);
        if (c.settings_revision == c.saved_revision) return c.settings_revision;
        document = c.settings.dump();
        revision = c.settings_revision;
    }
    c.workspace->SavePreferences(Json::parse(document));
    std::scoped_lock lock(c.mutex);
    c.saved_revision = revision;
    return revision;
}

void ANOMALY_CALL SaveWorker(void* user, AnomalyGenerationHandleV1) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        const auto attempted = SaveSettings(c);
        std::scoped_lock lock(c.mutex);
        c.save_task = {};
        // A preference edit admitted during the write needs a new Worker job.
        // A failed write of the same revision is not retried in a loop.
        if (!c.stopping && c.settings_revision > attempted)
            c.scheduler->schedule(c.scheduler->user, 50, SaveWorker, &c, &c.save_task);
    } catch (...) {
        std::scoped_lock lock(c.mutex);
        c.save_task = {};
        c.Log("EquipmentManager: configuration worker failed");
    }
}

Json Api(Context& c, std::string_view method, std::string_view path, std::string_view body,
         std::uint32_t& http_status, const Json* historical = nullptr) {
    Json input;
    if (method == "POST") input = Json::parse(body);
    const auto historical_inventory = historical ? std::make_shared<const Json>(*historical) : nullptr;
    std::unique_lock lock(c.mutex);
    if (c.stopping) {
        http_status = 503;
        return {{"message", c.localizer.Text("status.stopped", "The plugin has stopped")}};
    }
    if (method == "POST" && (c.workspace_busy || input.value("account_id", std::string{}) != c.account_id ||
        input.value("account_generation", std::string{}) != c.account_generation))
        throw InvalidWorkspace{WorkspaceFailure::stale_account};
    if (method == "GET" && path == "/api/bootstrap")
        return {{"locale", c.locale}, {"messages", c.messages}, {"settings", c.settings},
            {"recommended_weights", c.recommended_weights.Snapshot()}, {"features", {
            {"workspace", true}, {"library", true}, {"roles", true}, {"accounts", true}, {"assessment", true},
            {"optimize_top_k", true}, {"optimize_multi", true}, {"blueprints", true}, {"rewind", true},
            {"role_panel", true}, {"account_transfer", true}, {"catalog", true}, {"warehouse_rules", true},
            {"custom_boards", true}, {"cultivation", true}, {"role_comparisons", true}, {"account_snapshots", true},
            {"cultivation_stamina_reference", true}, {"dynamic_avatars", true}, {"recommended_weights", true}}}};
    if (method == "GET" && path == "/api/recommended-weights") {
        lock.unlock();
        return c.recommended_weights.Snapshot();
    }
    if (method == "POST" && path == "/api/recommended-weights/refresh") {
        lock.unlock();
        c.recommended_weights.StartRefresh();
        http_status = 202;
        return c.recommended_weights.Snapshot();
    }
    if (method == "GET" && (path == "/api/state" || path.starts_with("/api/state/"))) {
        lock.unlock(); return StateApi(c, path, http_status);
    }
    if (method == "POST" && path == "/api/avatars/refresh") {
        // A page opens one metadata job. Polling its result never queues another
        // Game read, and overlapping openings can share the same pending job.
        for (const auto& read : c.avatar_refreshes)
            if (read->account_generation == c.account_generation && (read->status == "queued" || read->status == "running")) {
                http_status = 202; return {{"id", read->id}, {"status", read->status}};
            }
        auto read = std::make_shared<AvatarRefresh>();
        read->id = c.account_generation + "-avatars-" + std::to_string(++c.next_avatar_refresh);
        read->account_generation = c.account_generation; read->account_binding = c.account_binding;
        if (read->account_binding.empty()) { read->status = "unavailable"; read->error_code = "workspace.identity_unavailable"; }
        c.avatar_refreshes.push_back(read);
        while (c.avatar_refreshes.size() > 16 && c.avatar_refreshes.front()->status != "queued" &&
            c.avatar_refreshes.front()->status != "running") c.avatar_refreshes.pop_front();
        http_status = 202; return {{"id", read->id}, {"status", read->status}};
    }
    constexpr std::string_view avatar_refresh_prefix = "/api/avatars/refresh/";
    if (method == "GET" && path.starts_with(avatar_refresh_prefix)) {
        for (const auto& read : c.avatar_refreshes) if (read->id == path.substr(avatar_refresh_prefix.size())) {
            const bool stale = read->account_generation != c.account_generation || read->account_binding != c.account_binding;
            Json result{{"id", read->id}, {"status", stale ? "unavailable" : read->status}, {"stale", stale},
                {"metadata_reads", c.avatar_metadata_reads}, {"avatars", stale ? Json::array() : read->avatars}};
            if (!read->error_code.empty()) result["error_code"] = read->error_code;
            return result;
        }
        throw InvalidCommand{CommandFailure::invalid_request};
    }
    constexpr std::string_view avatar_cache_prefix = "/api/avatars/cache/";
    if (method == "GET" && path.starts_with(avatar_cache_prefix)) {
        const auto number = path.substr(avatar_cache_prefix.size());
        std::size_t cursor{};
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), cursor);
        const auto characters = AvatarCharactersLocked(c);
        const auto binding = c.account_binding;
        const auto snapshot_id = SnapshotId(c);
        lock.unlock();
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || cursor > characters.size())
            throw InvalidCommand{CommandFailure::invalid_request};
        Json avatars = Json::array();
        std::size_t bytes{};
        constexpr auto maximum = ANOMALY_WEB_MAX_RESPONSE_BYTES_V1 - 4096U;
        for (; cursor < characters.size(); ++cursor) {
            const auto& character = characters[cursor];
            auto avatar = c.avatars.Cached(character.at("item_id").get_ref<const std::string&>(), character, binding);
            const auto size = avatar.dump().size() + 1;
            if (!avatars.empty() && bytes + size > maximum) break;
            bytes += size; avatars.push_back(std::move(avatar));
        }
        return {{"snapshot_id", snapshot_id}, {"avatars", std::move(avatars)},
            {"next_cursor", cursor < characters.size() ? Json(cursor) : Json(nullptr)}};
    }
    constexpr std::string_view avatar_prefix = "/api/avatars/";
    if (method == "GET" && path.starts_with(avatar_prefix)) {
        const auto role = path.substr(avatar_prefix.size());
        const auto characters = AvatarCharactersLocked(c);
        const auto binding = c.account_binding;
        bool allow_export{};
        for (auto read = c.avatar_refreshes.rbegin(); read != c.avatar_refreshes.rend(); ++read)
            if ((*read)->account_generation == c.account_generation) {
                allow_export = (*read)->status == "completed" && (*read)->account_binding == binding; break;
            }
        lock.unlock();
        for (const auto& character : characters) if (character.at("item_id").get_ref<const std::string&>() == role)
            return c.avatars.Request(role, character, binding, allow_export);
        throw InvalidWorkspace{WorkspaceFailure::missing_role};
    }
    if (method == "GET" && (path == "/api/optimize" || path == "/api/roles/panel" || path == "/api/cultivation" ||
        path == "/api/roles/contributions" || path == "/api/roles/replacements" || path == "/api/warehouse/rules/preview" ||
        path.starts_with("/api/optimize/results/"))) {
        lock.unlock(); return OptimizationApi(c, path);
    }
    if (method == "GET" && path == "/api/diagnostics")
        return {{"phase", c.inventory->value("phase", "waiting")}, {"detail", c.diagnostic},
                {"operation_error", c.operation_error},
                {"model_error", c.model_error},
                {"read_milliseconds", c.read_milliseconds}, {"maximum_read_milliseconds", c.maximum_read_milliseconds},
                {"read_attempts", c.read_attempts}, {"snapshot_reads", c.snapshot_reads},
                {"avatar_refresh_requests", c.next_avatar_refresh}, {"avatar_metadata_reads", c.avatar_metadata_reads},
                {"sync_pending", c.sync_requested || c.sync_running}, {"sync_status", c.sync_status},
                {"context", c.live_context}, {"generation", c.generation},
                {"items", c.inventory->at("items").size()}, {"characters", c.inventory->at("characters").size()}};
    if (method == "POST" && path == "/api/sync") {
        if (!c.sync_requested && !c.sync_running) {
            c.sync_requested = true; c.sync_status = "queued"; ++c.sync_request_id;
        }
        http_status = 202;
        return {{"accepted", true}, {"sync_pending", true}, {"sync_status", c.sync_status}, {"sync_request_id", c.sync_request_id}};
    }
    if (method == "POST" && path == "/api/settings") {
        if (!input.is_object() || input.dump().size() > 49152)
            throw InvalidCommand{CommandFailure::invalid_request};
        if ((input.contains("filters") && !input["filters"].is_object()) ||
            (input.contains("selected_character") && !input["selected_character"].is_null() && !input["selected_character"].is_object()) ||
            (input.contains("draft") && !input["draft"].is_null() && !input["draft"].is_object()))
            throw InvalidCommand{CommandFailure::invalid_request};
        if (input.contains("filters"))
            for (const auto& [key, value] : input["filters"].items())
                if (!value.is_string()) throw InvalidCommand{CommandFailure::invalid_request};
        // Preferences and drafts are durable inputs. Dispatch intents are never persisted.
        Json settings = Json::object();
        for (const auto* field : {"filters", "selected_character", "draft"})
            if (input.contains(field)) settings[field] = input[field];
        c.settings = std::move(settings);
        ++c.settings_revision;
        if (!c.save_task.id) {
            const auto status = c.scheduler->schedule(c.scheduler->user, 50, SaveWorker, &c, &c.save_task);
            if (status.code != ANOMALY_STATUS_V1_OK) {
                c.save_task = {};
                http_status = 503;
                return {{"message", c.localizer.Text("settings.save_failed", "Settings could not be queued for saving")}};
            }
        }
        return {{"accepted", true}};
    }
    if (method == "POST" && path == "/api/optimize/cancel") {
        if (c.optimization) c.optimization->cancel.store(true, std::memory_order_release);
        return {{"accepted", true}};
    }
    if (method == "POST" && (path == "/api/optimize" || path == "/api/blueprints")) {
        const auto& inventory = historical ? *historical : *c.inventory;
        const auto source_id = historical ? historical->at("snapshot_id").get<std::string>() : SnapshotId(c);
        if (c.optimization_execution || (c.optimization && (c.optimization->status == "queued" || c.optimization->status == "running"))) {
            http_status = 409;
            return {{"message", c.localizer.Text("algorithm.busy", "Wait for the current optimization or cancel it")}};
        }
        if (!input.is_object()) throw InvalidCommand{CommandFailure::invalid_request};
        // Searches run outside Worker; Stop cancels and joins their owner.
        if (const auto limit = input.find("time_limit_ms"); limit != input.end() &&
            (!limit->is_number_integer() || limit->get<std::int64_t>() < 1 || limit->get<std::int64_t>() > algorithm_limits::kMaximumTimeLimitMs))
            throw InvalidCommand{CommandFailure::invalid_request};
        if (path != "/api/blueprints" && (!inventory.value("complete", false) || inventory.value("phase", "waiting") != "ready")) {
            http_status = 409;
            return {{"message", AlgorithmMessage(c, AlgorithmFailure::inventory_unavailable)}};
        }
        if (path != "/api/blueprints" && !historical && !c.live_account_matches) throw InvalidWorkspace{WorkspaceFailure::account_mismatch};
        if ((path != "/api/blueprints" || input.contains("snapshot_id")) &&
            (!input.contains("snapshot_id") || input["snapshot_id"] != source_id)) {
            http_status = 409;
            return {{"message", AlgorithmMessage(c, AlgorithmFailure::stale_snapshot)}};
        }
        if (historical && path != "/api/blueprints") UseHistoricalRole(c, input, inventory);
        if (path != "/api/blueprints") AddReservations(c, input);
        auto job = std::make_shared<Optimization>();
        job->id = c.account_generation + '-' + std::to_string(++c.next_optimization);
        job->source_snapshot_id = source_id; job->historical = historical != nullptr;
        job->kind = path == "/api/blueprints" ? "blueprints" : input.contains("groups") ? "multi" :
            input.value("top_k", 1) > 1 ? "top_k" : "single";
        job->request = std::move(input);
        job->inventory = historical ? historical_inventory : c.inventory;
        job->message = c.localizer.Text("algorithm.queued", "Optimization is queued");
        c.optimization = std::move(job);
        const auto status = c.scheduler->schedule(c.scheduler->user, 0, OptimizeWorker, &c, &c.optimization_task);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            c.optimization_task = {};
            c.optimization->status = "failed";
            c.optimization->error_code = "algorithm.schedule_failed";
            c.optimization->message = c.localizer.Text("algorithm.schedule_failed", "Optimization could not be scheduled");
            http_status = 503;
            return OptimizationLocked(c);
        }
        http_status = 202;
        return {{"accepted", true}, {"id", c.optimization->id}};
    }
    if (method != "POST" || (path != "/api/command" && path != "/api/warehouse/rules/apply")) {
        http_status = 404;
        return {{"message", c.localizer.Text("command.not_found", "The requested endpoint does not exist")}};
    }
    const std::string action = path == "/api/warehouse/rules/apply" ? "apply_warehouse_rules" : input.at("action").get<std::string>();
    if (action == "cancel") {
        if (c.Active()) c.operations.back().cancel = true;
        return {{"accepted", true}};
    }
    if (input.contains("inventory_snapshot_id") || input.value("historical", false) ||
        (input.contains("native_assemblable") && input["native_assemblable"] == false) ||
        input.value("source_kind", std::string{}) == "account_snapshot" ||
        input.value("source_snapshot_id", std::string{}).starts_with("snapshot-"))
        throw InvalidCommand{CommandFailure::invalid_request};
    if (c.Active()) {
        http_status = 409;
        return {{"message", c.localizer.Text("command.busy", "Wait for the current operation or cancel its remaining steps")}};
    }
    if (!c.live_account_matches) throw InvalidWorkspace{WorkspaceFailure::account_mismatch};
    if (!c.inventory->value("complete", false) || c.inventory->value("phase", "waiting") != "ready") {
        http_status = 409;
        return {{"message", c.localizer.Text("command.not_ready", "A complete live inventory is required before changing equipment")}};
    }
    if (!input.contains("snapshot_id") || input["snapshot_id"] != SnapshotId(c)) {
        http_status = 409;
        return {{"error_code", "command.stale"}, {"message", c.localizer.Text("command.stale", "The inventory has changed Refresh the selection before applying it")}};
    }
    std::vector<Json> steps;
    if (action == "apply_warehouse_rules") {
        if (!c.optimization || c.optimization->kind != "warehouse_rules" || c.optimization->status != "completed" ||
            input.value("preview_id", std::string{}) != c.optimization->id ||
            c.optimization->source_snapshot_id != SnapshotId(c)) throw InvalidWorkspace{WorkspaceFailure::stale_snapshot};
        if (c.optimization->warehouse_inputs != WarehouseInputsLocked(c))
            throw InvalidWorkspace{WorkspaceFailure::conflict};
        Json batch{{"action", "set_item_states"}, {"operations", Json::array()}};
        const auto flush = [&] {
            if (batch["operations"].empty()) return;
            auto part = PrepareOperation(batch, *c.inventory);
            steps.insert(steps.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
            batch["operations"] = Json::array();
        };
        for (const auto& change : c.optimization->result->at("changes")) {
            const auto target = change.at("state").get<std::string>();
            if (target == "normal") {
                batch["operations"].push_back({{"equipment", change.at("equipment")}, {"field", "locked"}, {"value", false}});
                batch["operations"].push_back({{"equipment", change.at("equipment")}, {"field", "discarded"}, {"value", false}});
            } else batch["operations"].push_back({{"equipment", change.at("equipment")},
                {"field", target == "lock" ? "locked" : "discarded"}, {"value", true}});
            if (batch["operations"].size() >= 510) flush();
        }
        flush();
    } else {
        ValidateReservations(c, input);
        steps = PrepareOperation(input, *c.inventory);
    }
    Operation operation;
    operation.id = c.account_generation + '-' + std::to_string(++c.next_operation);
    operation.action = action;
    operation.status = "queued";
    operation.message = c.localizer.Text("operation.queued", "Queued for the game thread");
    operation.steps = std::move(steps);
    operation.context = c.live_context;
    c.operations.push_back(std::move(operation));
    while (c.operations.size() > 32) c.operations.pop_front();
    http_status = 202;
    return {{"accepted", true}, {"id", c.operations.back().id}};
}

Json Assess(Context& c, const Json& input, const Json* historical = nullptr) {
    Json roles; std::string source, account_generation;
    std::shared_ptr<const Json> snapshot;
    {
        std::scoped_lock lock(c.mutex);
        if (c.workspace_busy || input.value("account_id", std::string{}) != c.account_id ||
            input.value("account_generation", std::string{}) != c.account_generation)
            throw InvalidWorkspace{WorkspaceFailure::stale_account};
        if (!historical && !c.live_account_matches) throw InvalidWorkspace{WorkspaceFailure::account_mismatch};
        source = historical ? historical->at("snapshot_id").get<std::string>() : SnapshotId(c); account_generation = c.account_generation;
        snapshot = c.inventory;
        const auto& observed = historical ? *historical : *snapshot;
        if (!observed.value("complete", false) || input.value("snapshot_id", std::string{}) != source)
            throw InvalidWorkspace{WorkspaceFailure::stale_snapshot};
        roles = c.workspace_view.at("roles");
    }
    const auto& inventory = historical ? *historical : *snapshot;
    const Json* item{};
    for (const auto& candidate : inventory.at("items")) if (SameUid(candidate.at("uid"), input.at("equipment"))) { item = &candidate; break; }
    if (!item) throw InvalidCommand{CommandFailure::missing_equipment};
    Json results = Json::array();
    for (const auto& role : roles) {
        if (input.contains("role_ids") && std::find(input["role_ids"].begin(), input["role_ids"].end(), role["id"]) == input["role_ids"].end()) continue;
        Json row{{"role_id", role["id"]}, {"eligible", false}, {"score", nullptr}, {"grade", nullptr}};
        if (!role.contains("weights")) {
            row["error_code"] = "assessment.weights_missing";
            row["message"] = c.localizer.Text("assessment.weights_missing", "Configure character weights before assessing equipment");
        } else try {
            row.update(ScoreEquipment(*item, role)); row["eligible"] = true;
        } catch (const InvalidAlgorithm& failure) {
            row["error_code"] = AlgorithmErrorCode(failure.reason); row["message"] = AlgorithmMessage(c, failure.reason);
        }
        results.push_back(std::move(row));
    }
    bool stale;
    { std::scoped_lock lock(c.mutex); stale = (!historical && source != SnapshotId(c)) || account_generation != c.account_generation; }
    return {{"source_snapshot_id", source}, {"equipment", item->at("uid")}, {"results", results}, {"stale", stale}, {"historical", historical != nullptr}};
}

Json StartCalculation(Context& c, std::string_view path, const Json& input, std::uint32_t& status, const Json* historical = nullptr) {
    Json saved_plan;
    const auto historical_inventory = historical ? std::make_shared<const Json>(*historical) : nullptr;
    const bool character_model = path == "/api/roles/panel" || path == "/api/roles/contributions" || path == "/api/roles/replacements";
    if (character_model && input.contains("slot_id")) {
        const auto detail_path = "/api/slots/" + input.at("role_id").get<std::string>() + '/' + input.at("slot_id").get<std::string>();
        saved_plan = WorkspaceApi(c, "GET", detail_path, Json::object(), status).at("slot").at("plan");
    }
    std::scoped_lock lock(c.mutex);
    if (c.stopping || c.workspace_busy || input.value("account_id", std::string{}) != c.account_id ||
        input.value("account_generation", std::string{}) != c.account_generation)
        throw InvalidWorkspace{WorkspaceFailure::stale_account};
    if (c.optimization_execution || (c.optimization && (c.optimization->status == "queued" || c.optimization->status == "running")))
        throw InvalidWorkspace{WorkspaceFailure::conflict};
    const auto& inventory = historical ? *historical : *c.inventory;
    auto job = std::make_shared<Optimization>(); job->request = input; job->native_request = input;
    job->id = c.account_generation + '-' + std::to_string(++c.next_optimization);
    job->source_snapshot_id = historical ? historical->at("snapshot_id").get<std::string>() : SnapshotId(c);
    job->historical = historical != nullptr;
    if (input.contains("stamina_model_id")) {
        const auto model = ReferenceStaminaModel();
        if (input["stamina_model_id"] != model.at("id")) throw InvalidCommand{CommandFailure::invalid_request};
        Json stamina{{"scope", model.at("scope")}, {"stages", model.at("stages")}, {"hunter_level", input.at("hunter_level")},
            {"model_source", model.at("source")}, {"model_assumptions", model.at("assumptions")}};
        if (input.contains("effective_identification_level")) stamina["effective_identification_level"] = input["effective_identification_level"];
        job->request["stamina"] = std::move(stamina);
    }
    if (character_model) {
        if (!inventory.value("complete", false)) throw InvalidAlgorithm{AlgorithmFailure::inventory_unavailable};
        const auto id = input.at("role_id").get<std::string>(); Json role;
        for (const auto& candidate : c.workspace_view.at("roles")) if (candidate.at("id") == id) { role = candidate; break; }
        // A panel is a read-only calculation. An unbound account may inspect an
        // explicitly selected native definition without binding or saving it.
        if (role.is_null()) for (const auto& candidate : inventory.at("characters")) if (candidate.at("item_id") == id) {
            role = {{"id", id}, {"character", candidate.at("uid")}, {"observed", candidate}, {"allowed_cells", candidate.at("allowed_cells")}}; break;
        }
        if (role.is_null() || role.value("official", true) == false) throw InvalidAlgorithm{AlgorithmFailure::missing_character};
        Json source_character = role.at("character");
        if (historical) {
            source_character = nullptr;
            for (const auto& candidate : inventory.at("characters")) if (candidate.at("item_id") == id) {
                source_character = candidate.at("uid"); role["allowed_cells"] = candidate.at("allowed_cells"); break;
            }
            role["character"] = nullptr;
            job->request["board_source"] = source_character.is_null() ? "saved_role_definition" : "inventory_snapshot";
        }
        job->request["allowed_cells"] = role.at("allowed_cells");
        auto profile = role.value("profile", Json::object()); job->source_role_profile = profile;
        if (input.contains("profile")) { if (!input["profile"].is_object()) throw InvalidCommand{CommandFailure::invalid_request}; profile.update(input["profile"]); }
        job->request["profile"] = std::move(profile);
        Json plan = input.value("plan", saved_plan);
        if (plan.is_null()) {
            plan = {{"character", role["character"]}, {"core", nullptr}, {"placements", Json::array()}};
            for (const auto& item : inventory.at("items")) if (SameUid(item.at("character"), source_character)) {
                if (item["kind"] == "core") plan["core"] = item["uid"];
                else plan["placements"].push_back({{"equipment", item["uid"]}, {"row", item["row"]}, {"column", item["column"]}});
            }
        }
        plan["character"] = role["character"]; plan = ValidateSavedLoadout(plan, inventory, role.at("allowed_cells"));
        job->request["equipment"] = Json::array();
        for (const auto& item : inventory.at("items")) {
            bool selected = SameUid(item.at("uid"), plan.value("core", Json(nullptr)));
            for (const auto& placement : plan.at("placements")) selected = selected || SameUid(item.at("uid"), placement.at("equipment"));
            if (selected) job->request["equipment"].push_back(item);
        }
        if (role.contains("weights")) job->request["base_weights"] = role["weights"];
        if (role.contains("main_weights")) job->request["base_main_weights"] = role["main_weights"];
        if (input.contains("snapshot_id") && input["snapshot_id"] != job->source_snapshot_id)
            throw InvalidAlgorithm{AlgorithmFailure::stale_snapshot};
        job->request["snapshot_id"] = job->source_snapshot_id;
        job->request["character"] = role["character"];
        job->inventory = historical ? historical_inventory : c.inventory;
        if (path == "/api/roles/replacements") AddReservations(c, job->request);
        job->native_model_pending = true; job->kind = path == "/api/roles/panel" ? "panel" :
            path == "/api/roles/contributions" ? "contributions" : "replacements";
    } else if (path == "/api/cultivation/plan") {
        if (!inventory.value("complete", false)) throw InvalidAlgorithm{AlgorithmFailure::inventory_unavailable};
        if (!historical && !c.live_account_matches) throw InvalidWorkspace{WorkspaceFailure::account_mismatch};
        if (historical) {
            job->request["include_owned"] = false;
            for (auto& plan : job->request.at("plans")) {
                if (plan.value("use_observed", input.value("use_observed", false)))
                    throw InvalidCommand{CommandFailure::invalid_request};
                plan["use_observed"] = false;
            }
            job->request["use_observed"] = false;
        }
        job->kind = "cultivation"; job->native_model_pending = true;
    } else if (path == "/api/warehouse/rules/preview") {
        if (!inventory.value("complete", false)) throw InvalidAlgorithm{AlgorithmFailure::inventory_unavailable};
        if (input.value("snapshot_id", std::string{}) != job->source_snapshot_id) throw InvalidAlgorithm{AlgorithmFailure::stale_snapshot};
        if (!historical && !c.live_account_matches) throw InvalidWorkspace{WorkspaceFailure::account_mismatch};
        job->kind = "warehouse_rules"; job->inventory = historical ? historical_inventory : c.inventory;
        job->warehouse_inputs = WarehouseInputsLocked(c);
        job->request["roles"] = job->warehouse_inputs.at("roles");
        auto& excluded = job->request["exclude_uids"];
        if (excluded.is_null()) excluded = Json::array();
        if (!excluded.is_array()) throw InvalidCommand{CommandFailure::invalid_request};
        for (const auto& uid : job->warehouse_inputs.at("exclude_uids"))
            if (std::find(excluded.begin(), excluded.end(), uid) == excluded.end()) excluded.push_back(uid);
    } else {
        job->kind = path == "/api/cultivation/experience" ? "experience" : path == "/api/cultivation/stamina" ? "stamina" : "cultivation";
    }
    if (const auto limit = job->request.find("time_limit_ms"); limit != job->request.end()) {
        if (!limit->is_number_integer() || limit->get<std::int64_t>() < 1 ||
            limit->get<std::int64_t>() > algorithm_limits::kMaximumTimeLimitMs)
            throw InvalidCommand{CommandFailure::invalid_request};
    } else job->request["time_limit_ms"] = algorithm_limits::kDefaultTimeLimitMs;
    job->message = c.localizer.Text("algorithm.queued", "Optimization is queued"); c.optimization = job;
    if (!job->native_model_pending && c.scheduler->schedule(c.scheduler->user, 0, OptimizeWorker, &c, &c.optimization_task).code != 0) {
        job->status = "failed"; job->error_code = "algorithm.schedule_failed";
        job->message = c.localizer.Text("algorithm.schedule_failed", "Optimization could not be scheduled"); status = 503;
        return OptimizationLocked(c);
    }
    status = 202; return {{"accepted", true}, {"id", job->id}};
}

void ProcessNativeModel(Context& c) {
    std::shared_ptr<Optimization> job;
    {
        std::scoped_lock lock(c.mutex);
        if (c.stopping || !c.loaded || !c.optimization || !c.optimization->native_model_pending) return;
        job = c.optimization; job->native_model_pending = false; job->native_model_reading = true;
    }
    Json model; std::string diagnostic;
    const auto success = job->kind == "cultivation" ? c.game.ReadCultivationModel(job->request, model, diagnostic) :
        c.game.ReadCharacterModel(job->request.at("role_id").get<std::string>(), job->request.at("profile"), model, diagnostic);
    std::scoped_lock lock(c.mutex);
    job->native_model_reading = false;
    c.model_error = success ? std::string{} : diagnostic;
    if (success) {
        if (job->historical && job->kind == "cultivation") {
            model["owned"] = nullptr; model["known_owned"] = Json::object(); model["owned_complete"] = false;
            model["owned_source"] = "unavailable_account_snapshot"; model["owned_gaps"] = Json::array();
        }
        job->request.update(model);
        if (job->request.contains("damage")) {
            auto& damage = job->request["damage"];
            if (!damage.contains("property") && model.value("damage_property_complete", false)) damage["property"] = model.at("damage_property");
            if (damage.contains("enemy") && damage["enemy"].value("neutral", false))
                damage["enemy"] = {{"defense_base", 0}, {"resistance", 0}, {"model_kind", "explicit_neutral"}};
        }
        if (c.scheduler->schedule(c.scheduler->user, 0, OptimizeWorker, &c, &c.optimization_task).code == 0) return;
        job->error_code = "algorithm.schedule_failed";
        job->message = c.localizer.Text("algorithm.schedule_failed", "Optimization could not be scheduled");
    } else {
        job->error_code = "status.read_failed";
        job->message = c.localizer.Text("status.read_failed", "The live inventory could not be read safely");
        c.Log("EquipmentManager role model: " + diagnostic);
    }
    job->status = "failed";
}

Json CatalogReadApi(Context& c, std::string_view method, std::string_view path,
    const Json& input, std::uint32_t& status) {
    std::scoped_lock lock(c.mutex);
    if (method == "POST") {
        if (c.stopping || c.workspace_busy || input.value("account_id", std::string{}) != c.account_id ||
            input.value("account_generation", std::string{}) != c.account_generation)
            throw InvalidWorkspace{WorkspaceFailure::stale_account};
        if (path != "/api/catalog/read" || !input.contains("section") || !input["section"].is_string())
            throw InvalidCommand{CommandFailure::invalid_request};
        const auto offset = input.value("offset", std::int64_t{}), limit = input.value("limit", std::int64_t{32});
        if (offset < 0 || offset > 1000000 || limit < 1 || limit > 64)
            throw InvalidCommand{CommandFailure::invalid_request};
        if (std::count_if(c.native_reads.begin(), c.native_reads.end(), [](const auto& read) {
            return read->status == "queued" || read->status == "running";
        }) >= 8) throw InvalidWorkspace{WorkspaceFailure::conflict};
        auto read = std::make_shared<NativeRead>(); read->request = input;
        read->id = c.account_generation + "-catalog-" + std::to_string(++c.next_native_read);
        read->context = c.live_context; read->account_generation = c.account_generation;
        c.native_reads.push_back(read);
        while (c.native_reads.size() > 16 && c.native_reads.front()->status != "queued" &&
            c.native_reads.front()->status != "running") c.native_reads.pop_front();
        status = 202; return {{"id", read->id}, {"status", read->status}};
    }
    constexpr std::string_view prefix = "/api/catalog/jobs/";
    if (method != "GET" || !path.starts_with(prefix)) throw InvalidCommand{CommandFailure::invalid_request};
    for (const auto& read : c.native_reads) if (read->id == path.substr(prefix.size())) {
        Json result{{"id", read->id}, {"status", read->status}, {"stale", read->context != c.live_context ||
            read->account_generation != c.account_generation}};
        if (!read->result.is_null()) result["result"] = read->result;
        if (!read->message.empty()) result["message"] = read->message;
        if (!read->error_code.empty()) result["error_code"] = read->error_code;
        return result;
    }
    status = 404; return {{"error_code", "command.not_found"},
        {"message", c.localizer.Text("command.not_found", "The requested endpoint does not exist")}};
}

void ProcessNativeReads(Context& c) {
    std::shared_ptr<NativeRead> read;
    {
        std::scoped_lock lock(c.mutex);
        if (c.stopping || !c.loaded) return;
        for (const auto& candidate : c.native_reads) if (candidate->status == "queued") {
            read = candidate; read->status = "running"; break;
        }
    }
    if (!read) return;
    Json result; std::string diagnostic;
    const auto success = c.game.ReadCatalogPage(read->request.at("section").get<std::string>(),
        read->request.value("offset", std::size_t{}), read->request.value("limit", std::size_t{32}), result, diagnostic);
    std::scoped_lock lock(c.mutex);
    read->status = success ? "completed" : "failed";
    if (success) read->result = std::move(result);
    else {
        read->error_code = "status.read_failed";
        read->message = c.localizer.Text("status.read_failed", "The live inventory could not be read safely");
        c.Log("EquipmentManager catalog: " + diagnostic);
    }
}

Json HandleApi(Context& c, std::string_view method, std::string_view path,
               std::string_view body, std::uint32_t& status) {
    try {
        if (method == "GET" && path == "/api/cultivation/stamina-model") return ReferenceStaminaModel();
        if (method == "POST" && (path == "/api/roles/panel" || path == "/api/roles/contributions" || path == "/api/roles/replacements" || path == "/api/cultivation/experience" ||
            path == "/api/cultivation/plan" || path == "/api/cultivation/stamina" || path == "/api/warehouse/rules/preview")) {
            const auto input = Json::parse(body); Json historical;
            if (input.contains("inventory_snapshot_id")) historical = c.workspace->ReadSnapshot(input.at("inventory_snapshot_id").get<std::string>(), input);
            return StartCalculation(c, path, input, status, historical.is_null() ? nullptr : &historical);
        }
        if (path == "/api/catalog/read" || path.starts_with("/api/catalog/jobs/"))
            return CatalogReadApi(c, method, path, method == "POST" ? Json::parse(body) : Json::object(), status);
        if (method == "POST" && (path == "/api/assessment" || path == "/api/optimize" || path == "/api/blueprints")) {
            const auto input = Json::parse(body); Json historical;
            if (input.contains("inventory_snapshot_id")) historical = c.workspace->ReadSnapshot(input.at("inventory_snapshot_id").get<std::string>(), input);
            if (path == "/api/assessment") return Assess(c, input, historical.is_null() ? nullptr : &historical);
            return Api(c, method, path, body, status, historical.is_null() ? nullptr : &historical);
        }
        if (c.workspace && c.workspace->Handles(path)) {
            const auto input = method == "POST" ? Json::parse(body) : Json::object();
            return WorkspaceApi(c, method, path, input, status);
        }
        if (method == "POST" && path == "/api/rewind/recommend") {
            const auto input = Json::parse(body);
            {
                std::scoped_lock lock(c.mutex);
                if (c.workspace_busy || input.value("account_id", std::string{}) != c.account_id ||
                    input.value("account_generation", std::string{}) != c.account_generation)
                    throw InvalidWorkspace{WorkspaceFailure::stale_account};
            }
            return RecommendRewind(input);
        }
        return Api(c, method, path, body, status);
    } catch (const InvalidWorkspace& error) {
        switch (error.reason) {
        case WorkspaceFailure::io_error: status = 503; break;
        case WorkspaceFailure::stale_account: case WorkspaceFailure::conflict: case WorkspaceFailure::slot_locked:
        case WorkspaceFailure::stale_snapshot: case WorkspaceFailure::account_mismatch: status = 409; break;
        default: status = 400; break;
        }
        return {{"error_code", WorkspaceErrorCode(error.reason)}, {"message", WorkspaceMessage(c, error.reason)}};
    }
    catch (const InvalidAlgorithm& error) {
        status = 400;
        return {{"error_code", AlgorithmErrorCode(error.reason)}, {"message", AlgorithmMessage(c, error.reason)}};
    }
    catch (const InvalidCommand& error) {
        status = 400;
        return {{"message", FailureMessage(c, error.reason)}};
    } catch (const Json::exception&) {
        status = 400;
        return {{"message", FailureMessage(c, CommandFailure::invalid_request)}};
    }
}

AnomalyStatusV1 ANOMALY_CALL AutomationRequest(void* user, const AnomalyIpcRequestContextV1*,
    AnomalyByteSpanV1 request, AnomalyMutableByteSpanV1 response, std::size_t* size) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        if (!size || request.size > automation::kMaximumRequestBytes)
            return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
        // Negotiate capacity before inspecting/admitting an intent. A small
        // output buffer must never cause the caller to repeat an accepted POST.
        if (!response.data || response.size < automation::kMaximumResponseBytes) {
            *size = automation::kMaximumResponseBytes;
            return {ANOMALY_STATUS_V1_BUFFER_TOO_SMALL};
        }
        Json result;
        std::uint32_t status = 200;
        try {
            const auto envelope = Json::parse(request.data, request.data + request.size);
            const auto method = envelope.at("method").get<std::string>();
            const auto path = envelope.at("path").get<std::string>();
            if (!envelope.is_object() || (method != "GET" && method != "POST"))
                throw InvalidCommand{CommandFailure::invalid_request};
            for (const auto& [key, value] : envelope.items())
                if (key != "method" && key != "path" && key != "body")
                    throw InvalidCommand{CommandFailure::invalid_request};
            const auto body = envelope.contains("body") ? envelope["body"].dump() : std::string{};
            result = HandleApi(c, method, path, body, status);
        } catch (const InvalidCommand& error) {
            status = 400;
            result = {{"message", FailureMessage(c, error.reason)}};
        } catch (const Json::exception&) {
            status = 400;
            result = {{"message", FailureMessage(c, CommandFailure::invalid_request)}};
        }
        result["http_status"] = status;
        const auto bytes = result.dump();
        *size = bytes.size();
        if (bytes.size() > response.size) return {ANOMALY_STATUS_V1_BUFFER_TOO_SMALL};
        std::memcpy(response.data, bytes.data(), bytes.size());
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

AnomalyStatusV1 ANOMALY_CALL WebRequest(void* user, const AnomalyWebRequestV1* request,
                                      AnomalyWebResponseV1* response) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        if (!request || request->struct_size < sizeof(*request) || !response ||
            response->struct_size < sizeof(*response)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
        std::uint32_t status = 200;
        const auto result = HandleApi(c, {request->method.data, request->method.size},
                {request->path.data, request->path.size},
                {request->body.data ? request->body.data : "", request->body.size}, status);
        const auto bytes = result.dump();
        if (bytes.size() > response->body.size) return {ANOMALY_STATUS_V1_BUFFER_TOO_SMALL};
        std::memcpy(response->body.data, bytes.data(), bytes.size());
        response->body_size = bytes.size();
        response->http_status = status;
        response->content_type = ANOMALY_WEB_CONTENT_TYPE_V1_JSON;
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

void Advance(Context& c) {
    if (!c.Active()) return;
    auto& operation = c.operations.back();
    if (operation.context != c.live_context || !c.live_account_matches) {
        operation.status = operation.waiting ? "unknown" : "cancelled";
        operation.message = c.localizer.Text("operation.context_changed", "The game context changed Refresh the inventory before continuing");
        return;
    }
    if (operation.waiting) {
        if (c.snapshot_notification > operation.sent_notification &&
            StepSatisfied(operation.sent, *c.inventory)) {
            ++operation.completed;
            operation.waiting = false;
        } else if (Clock::now() - operation.sent_at >= std::chrono::seconds(10)) {
            operation.status = "unknown";
            operation.message = c.localizer.Text("operation.unknown", "The server result was not confirmed Remaining steps were stopped; refresh before retrying");
            return;
        } else return;
    }
    if (operation.cancel) {
        operation.status = "cancelled";
        operation.message = c.localizer.Text("operation.cancelled", "Remaining steps were cancelled Confirmed changes are shown in the live inventory");
        return;
    }
    while (operation.completed < operation.steps.size()) {
        auto command = ResolveStep(operation.steps[operation.completed], *c.inventory);
        if (StepSatisfied(command, *c.inventory)) { ++operation.completed; continue; }
        if (command["action"] == "verify_loadout") {
            operation.status = "failed";
            operation.message = c.localizer.Text("operation.mismatch", "The observed equipment does not match the requested loadout");
            return;
        }
        operation.sent_notification = c.game.NotificationSequence();
        std::string error;
        c.operation_error.clear();
        const auto outcome = c.game.Dispatch(command, *c.inventory, error);
        if (outcome != DispatchResult::dispatched) {
            c.operation_error = error;
            operation.status = outcome == DispatchResult::unknown ? "unknown" : "failed";
            operation.message = outcome == DispatchResult::unknown
                ? c.localizer.Text("operation.unknown", "The server result was not confirmed Remaining steps were stopped; refresh before retrying")
                : c.localizer.Text("operation.dispatch_failed", "The game rejected the operation before dispatch Remaining steps were stopped");
            c.Log("EquipmentManager: " + error);
            return;
        }
        operation.sent = std::move(command);
        operation.waiting = true;
        operation.sent_at = Clock::now();
        operation.status = "dispatched";
        operation.message = c.localizer.Text("operation.dispatched", "RPC dispatched Waiting for a server inventory update");
        return;
    }
    operation.status = "confirmed";
    operation.message = c.localizer.Text("operation.confirmed", "The live inventory confirms all requested changes");
}

void ANOMALY_CALL AvatarSourcesWorker(void* user, AnomalyGenerationHandleV1) noexcept {
    auto& c = *static_cast<Context*>(user);
    std::shared_ptr<AvatarRefresh> read;
    try {
        // This is the sole source-index publisher. One task drains the Game
        // completion mailbox in FIFO order; each ready bit is consumed once.
        for (;;) {
            {
                std::scoped_lock lock(c.mutex);
                if (c.stopping) { c.avatar_publish_task = {}; return; }
                for (const auto& candidate : c.avatar_refreshes) if (candidate->metadata_ready) {
                    candidate->metadata_ready = false;
                    if (candidate->account_generation != c.account_generation || candidate->account_binding != c.account_binding) {
                        candidate->status = "unavailable"; continue;
                    }
                    read = candidate; break;
                }
                if (!read) { c.avatar_publish_task = {}; return; }
            }
            // File I/O uses AvatarImages' Worker-only source mutex. Game and
            // HTTP can keep reading their mailboxes while publication runs.
            auto avatars = c.avatars.PublishSources(read->account_binding, read->characters);
            std::scoped_lock lock(c.mutex);
            const bool stale = c.stopping || read->account_generation != c.account_generation || read->account_binding != c.account_binding;
            read->status = stale ? "unavailable" : "completed";
            if (!stale) read->avatars = std::move(avatars);
            read->characters = Json::array();
            read.reset();
        }
    } catch (...) {
        std::scoped_lock lock(c.mutex);
        c.avatar_publish_task = {};
        if (read) { read->status = "failed"; read->error_code = "status.read_failed"; }
        for (auto& candidate : c.avatar_refreshes) if (candidate->metadata_ready) {
            candidate->metadata_ready = false; candidate->status = "failed"; candidate->error_code = "status.read_failed";
        }
        c.Log("EquipmentManager: avatar source cache publication failed");
    }
}

void ProcessAvatarRefresh(Context& c) {
    std::shared_ptr<AvatarRefresh> read;
    {
        std::scoped_lock lock(c.mutex);
        if (c.stopping || !c.loaded) return;
        for (const auto& candidate : c.avatar_refreshes) if (candidate->status == "queued") {
            read = candidate; read->status = "running"; break;
        }
    }
    if (!read) return;
    Json characters = Json::array();
    bool attempted{}, success{};
    std::string error_code, diagnostic;
    try {
        // A stored software account can use its cache while another game
        // account is active. Verify through the host account service before
        // touching current appearance; never infer identity from a role id.
        std::array<char, 64> uid{}; std::size_t size = uid.size();
        if (!c.account || c.account->uid_utf8(c.account->user, uid.data(), &size).code != ANOMALY_STATUS_V1_OK ||
            size <= 1 || size > uid.size()) error_code = "workspace.identity_unavailable";
        else {
            const Json identity{{"account_identity_complete", true}, {"game_account_id", std::string(uid.data(), size - 1)}};
            if (WorkspaceStore::IdentityKey(identity) != read->account_binding) error_code = "workspace.account_mismatch";
            else {
                attempted = true;
                success = c.game.ReadAvatarMetadata(characters, diagnostic);
                if (!success) error_code = "status.read_failed";
            }
        }
    } catch (const std::exception& exception) { diagnostic = exception.what(); error_code = "status.read_failed"; }
    catch (...) { diagnostic = "Unknown avatar metadata read failure"; error_code = "status.read_failed"; }
    std::scoped_lock lock(c.mutex);
    if (attempted) ++c.avatar_metadata_reads;
    const bool stale = c.stopping || read->account_generation != c.account_generation || read->account_binding != c.account_binding;
    read->status = stale || !attempted ? "unavailable" : success ? "running" : "failed";
    if (!stale && success) {
        read->characters = std::move(characters); read->metadata_ready = true;
        if (!c.avatar_publish_task.id && c.scheduler->schedule(c.scheduler->user, 0, AvatarSourcesWorker, &c,
            &c.avatar_publish_task).code != ANOMALY_STATUS_V1_OK) {
            c.avatar_publish_task = {}; read->metadata_ready = false;
            read->status = "failed"; error_code = "status.read_failed";
            c.Log("EquipmentManager: avatar source cache publication could not be scheduled");
        }
    }
    read->error_code = std::move(error_code);
    if (!diagnostic.empty()) c.Log("EquipmentManager avatar metadata: " + diagnostic);
}

void ANOMALY_CALL Update(void* user, double) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        ProcessNativeModel(c);
        ProcessNativeReads(c);
        ProcessAvatarRefresh(c);
        c.avatars.TickGame(c.game);
        bool manual{};
        std::shared_ptr<const Json> previous;
        {
            std::scoped_lock lock(c.mutex);
            if (c.stopping || !c.loaded) return;
            manual = c.sync_requested;
            // Idle web reads only see the published immutable snapshot. Full
            // inventory collection is an explicit sync or an RPC confirmation.
            if (!manual && !c.Active()) return;
            const auto now = Clock::now();
            if (!manual && now < c.next_read) return;
            c.next_read = now + std::chrono::milliseconds(50);
            if (manual) {
                c.sync_requested = false; c.sync_running = true; c.sync_status = "running";
            }
            previous = c.inventory;
        }
        Json inventory;
        std::string error;
        const auto read_started = Clock::now();
        const auto outcome = c.game.ReadIfDirty(inventory, error, manual);
        const auto read_milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - read_started).count();
        const bool ready = outcome == SnapshotReadResult::updated || outcome == SnapshotReadResult::unchanged;
        std::shared_ptr<const Json> published;
        if (outcome == SnapshotReadResult::updated) {
            inventory["message"] = c.localizer.Text("status.ready", "Live inventory is ready");
            if (inventory != *previous) published = std::make_shared<const Json>(std::move(inventory));
        } else if (!ready) {
            const auto phase = outcome == SnapshotReadResult::waiting ? "waiting" : "unavailable";
            Json unavailable{{"phase", phase}, {"complete", false}, {"characters", Json::array()}, {"items", Json::array()},
                {"message", outcome == SnapshotReadResult::waiting
                    ? c.localizer.Text("status.waiting", "Waiting for a complete live inventory")
                    : c.localizer.Text("status.read_failed", "The live inventory could not be read safely")}};
            if (unavailable != *previous) published = std::make_shared<const Json>(std::move(unavailable));
        }
        const auto& observed = published ? *published : *previous;
        const auto live_identity = ready ? WorkspaceStore::IdentityKey(observed) : std::string{};
        std::scoped_lock lock(c.mutex);
        ++c.read_attempts;
        if (outcome == SnapshotReadResult::updated) {
            ++c.snapshot_reads; c.snapshot_notification = c.game.SourceNotificationSequence();
        }
        if (manual) { c.sync_running = false; c.sync_status = ready ? "completed" : "failed"; }
        c.read_milliseconds = read_milliseconds;
        c.maximum_read_milliseconds = (std::max)(c.maximum_read_milliseconds, read_milliseconds);
        c.live_context = c.game.Context();
        c.live_account_matches = !live_identity.empty() && live_identity == c.account_binding;
        if (published) { c.inventory = std::move(published); ++c.revision; }
        if (ready) {
            c.diagnostic.clear();
            Advance(c);
            SchedulePersistenceLocked(c);
        } else {
            if (error != c.diagnostic) { c.diagnostic = error; c.Log("EquipmentManager: " + error); }
            if (c.Active() && (c.live_context != c.operations.back().context || c.operations.back().cancel ||
                (c.operations.back().waiting && Clock::now() - c.operations.back().sent_at >= std::chrono::seconds(10)))) {
                c.operations.back().status = c.operations.back().waiting ? "unknown" : "cancelled";
                c.operations.back().message = c.localizer.Text("operation.context_changed", "The game context changed Refresh the inventory before continuing");
            }
        }
    } catch (const InvalidCommand& error) {
        std::scoped_lock lock(c.mutex);
        if (c.sync_running) { c.sync_running = false; c.sync_status = "failed"; }
        if (c.Active()) { c.operations.back().status = "failed"; c.operations.back().message = FailureMessage(c, error.reason); }
    } catch (...) {
        auto unavailable = std::make_shared<const Json>(Json{
            {"phase", "unavailable"}, {"complete", false}, {"characters", Json::array()}, {"items", Json::array()},
            {"message", c.localizer.Text("status.read_failed", "The live inventory could not be read safely")}});
        std::scoped_lock lock(c.mutex);
        c.inventory.swap(unavailable); ++c.revision; c.live_account_matches = false;
        if (c.sync_running) { c.sync_running = false; c.sync_status = "failed"; }
        if (c.Active()) {
            c.operations.back().status = c.operations.back().waiting ? "unknown" : "failed";
            c.operations.back().message = c.localizer.Text("operation.unknown", "The server result was not confirmed Remaining steps were stopped; refresh before retrying");
        }
    }
}

std::filesystem::path PluginDirectory(const AnomalyCoreServiceV1* core) {
    if (!core || core->struct_size < offsetof(AnomalyCoreServiceV1, plugin_directory) + sizeof(core->plugin_directory) ||
        !core->plugin_directory) return {};
    std::size_t size{};
    const auto measured = core->plugin_directory(core->user, nullptr, &size);
    if ((measured.code != ANOMALY_STATUS_V1_OK && measured.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) ||
        size <= 1 || size > 32768) return {};
    std::string utf8(size, '\0');
    if (core->plugin_directory(core->user, utf8.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const auto end = utf8.find('\0'); end != std::string::npos) utf8.resize(end);
    if (utf8.empty()) return {};
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* api, void** result) noexcept {
    try {
        if (!result) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
        auto owner = std::make_unique<Context>();
        auto& c = *owner;
        c.localizer = anomaly::sdk::Localizer(api);
        c.locale = c.localizer.Locale();
        const Host host(api);
        c.core = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        c.config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID).get();
        c.scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
        c.runtime = host.Query<AnomalyRuntimeInfoServiceV1>(ANOMALY_RUNTIME_INFO_SERVICE_V1_ID).get();
        c.ipc = host.Query<AnomalyIpcServiceV1>(ANOMALY_IPC_SERVICE_V1_ID).get();
        c.web = host.Query<AnomalyWebServiceV1>(ANOMALY_WEB_SERVICE_V1_ID).get();
        c.account = host.Query<AnomalyNteAccountServiceV1>(ANOMALY_NTE_ACCOUNT_SERVICE_V1_ID).get();
        if (c.account && (c.account->struct_size < sizeof(*c.account) || !c.account->uid_utf8)) c.account = nullptr;
        if (!c.core || !c.config || c.config->struct_size < sizeof(*c.config) ||
            !c.scheduler || c.scheduler->struct_size < sizeof(*c.scheduler) ||
            !c.runtime || c.runtime->struct_size < sizeof(*c.runtime) ||
            !c.web || c.web->struct_size < sizeof(*c.web))
            return {ANOMALY_STATUS_V1_UNAVAILABLE};
        AnomalyRuntimeInfoV1 runtime_info{sizeof(runtime_info)};
        const auto runtime_status = c.runtime->snapshot(c.runtime->user, &runtime_info);
        if (runtime_status.code != ANOMALY_STATUS_V1_OK) return runtime_status;
        c.generation = runtime_info.plugin_generation;
        c.recommended_weights.Load(PluginDirectory(c.core), [&c](std::string_view message) { c.Log(message); });
        c.workspace = std::make_shared<WorkspaceStore>(); c.workspace->Load(api, c.generation);
        c.avatars.Load(api, c.generation);
        c.settings = c.workspace->Preferences();
        PublishWorkspace(c, c.workspace->View(*c.inventory), c.settings);
        c.messages = BuildWebMessages(c.localizer);
        c.loaded = c.game.Load(api, c.load_error);
        c.inventory = std::make_shared<const Json>(Json{
            {"phase", "waiting"}, {"complete", false}, {"characters", Json::array()}, {"items", Json::array()},
            {"message", c.localizer.Text("status.sync_required", "Click Sync inventory to read your equipment")}});
        if (!c.loaded) {
            c.Log("EquipmentManager: " + c.load_error);
            c.game.Stop();
            return {ANOMALY_STATUS_V1_UNAVAILABLE};
        }
        *result = owner.release();
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

void CloseApplications(Context& c) {
    if (const auto handle = std::exchange(c.application, {}); handle.id)
        c.web->unregister_application(c.web->user, handle);
    if (const auto handle = std::exchange(c.automation_endpoint, {}); handle.id)
        c.ipc->unregister_endpoint(c.ipc->user, handle);
}

AnomalyStatusV1 ANOMALY_CALL Start(void* user) noexcept {
    auto& c = *static_cast<Context*>(user);
    const auto fail = [&](AnomalyStatusV1 status) { CloseApplications(c); return status; };
    try {
        const auto registered = c.web->register_application(c.web->user, StringView("assets/web"), WebRequest, &c, &c.application);
        if (registered.code != ANOMALY_STATUS_V1_OK) return fail(registered);
        std::string url(4096, '\0');
        std::size_t size = url.size();
        const auto resolved = c.web->application_url(c.web->user, c.application, url.data(), &size);
        if (resolved.code != ANOMALY_STATUS_V1_OK) return fail(resolved);
        if (size == 0 || size > url.size()) return fail({ANOMALY_STATUS_V1_FAILED});
        url.resize(size - 1);
        c.url = std::move(url);
        // Publish the intent endpoint only after every fallible preparation
        // step. A failed Start must not leave a running job using its Context.
        if (c.ipc && c.ipc->struct_size >= sizeof(*c.ipc)) {
            const AnomalyIpcEndpointDescriptorV1 descriptor{sizeof(descriptor), StringView(automation::kEndpoint),
                automation::kMajorVersion, automation::kMinorVersion, automation::kRequestSchema,
                automation::kResponseSchema, {}, ANOMALY_IPC_MODE_V1_SYNC_REQUEST,
                ANOMALY_IPC_AFFINITY_V1_CALLER, 2000, ANOMALY_IPC_REENTRANCY_V1_REJECT,
                automation::kMaximumRequestBytes, automation::kMaximumResponseBytes, 0, 1};
            const auto status = c.ipc->register_endpoint(c.ipc->user, &descriptor,
                AutomationRequest, &c, &c.automation_endpoint);
            if (status.code != ANOMALY_STATUS_V1_OK) return fail(status);
        }
        c.recommended_weights.StartRefresh();
        return Ok();
    } catch (...) { return fail({ANOMALY_STATUS_V1_FAILED}); }
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* user, std::uint32_t) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        AnomalyGenerationHandleV1 task{};
        AnomalyGenerationHandleV1 optimization_task{};
        AnomalyGenerationHandleV1 persistence_task{};
        AnomalyGenerationHandleV1 avatar_publish_task{};
        {
            std::scoped_lock lock(c.mutex);
            c.stopping = true;
            task = std::exchange(c.save_task, {});
            if (c.optimization) c.optimization->cancel.store(true, std::memory_order_release);
            if (c.optimization_execution) c.optimization_execution->job->cancel.store(true, std::memory_order_release);
            optimization_task = std::exchange(c.optimization_task, {});
            persistence_task = std::exchange(c.persistence_task, {});
            avatar_publish_task = std::exchange(c.avatar_publish_task, {});
        }
        // The host holds the lifecycle generation lease until this returns.
        // No DLL unload is safe before its private computation thread exits.
        if (c.optimization_execution && c.optimization_execution->thread.joinable())
            c.optimization_execution->thread.join();
        c.optimization_execution.reset();
        c.recommended_weights.Stop();
        CloseApplications(c);
        if (task.id) c.scheduler->cancel(c.scheduler->user, task);
        if (optimization_task.id) c.scheduler->cancel(c.scheduler->user, optimization_task);
        if (persistence_task.id) c.scheduler->cancel(c.scheduler->user, persistence_task);
        if (avatar_publish_task.id) c.scheduler->cancel(c.scheduler->user, avatar_publish_task);
        c.avatars.Stop();
        SaveSettings(c);
        if (c.loaded) PersistWorkspace(c);
        c.game.Stop();
        c.workspace->Close();
        c.loaded = false;
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

void ANOMALY_CALL Unload(void* user) noexcept {
    Stop(user, 0);
    try { delete static_cast<Context*>(user); } catch (...) {}
}

void ANOMALY_CALL Draw(void* user, const AnomalyUiServiceV1* ui) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        const auto title = c.localizer.Text("window.title", "Equipment Manager");
        anomaly::sdk::UiWindow window(ui, title);
        if (!window) return;
        std::scoped_lock lock(c.mutex);
        const auto message = c.inventory->value("message", std::string{});
        if (ui->text) ui->text(ui->user, StringView(message));
        if (!c.url.empty() && ui->struct_size >= offsetof(AnomalyUiServiceV1, text_link) + sizeof(ui->text_link) && ui->text_link) {
            const auto label = c.localizer.Text("window.open_web", "Open equipment manager in your browser");
            ui->text_link(ui->user, StringView(label), StringView(c.url));
        } else if (ui->text) {
            const auto runtime_message = c.localizer.Text("window.runtime_update", "Load the current Runtime to enable the web interface");
            ui->text(ui->user, StringView(runtime_message));
        }
    } catch (...) {}
}

} // namespace
} // namespace equipment_manager

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
    using namespace equipment_manager;
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("anomaly.equipment-manager"), StringView("Equipment Manager"),
        StringView("Anomaly"), StringView("0.1.0"), Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
