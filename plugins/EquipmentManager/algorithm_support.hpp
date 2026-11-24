#pragma once
#include "algorithm.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace equipment_manager::algorithm_detail {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
// Remaining module stock after higher-priority reservations. Core reservations
// remain explicit UID exclusions rather than interchangeable stock.
struct InventoryCapacity {
    std::set<std::uint64_t> items;
    std::size_t maximum{};
};
using InventoryCapacities = std::vector<InventoryCapacity>;
// One caller owns the executor for a complete single, TopK or team job. Threads
// start on the first parallel solve and are joined before that job returns.
class SearchExecutor {
public:
    explicit SearchExecutor(unsigned workers);
    ~SearchExecutor();
    void Execute(std::size_t count, const std::function<void(std::size_t)>& worker,
        const std::function<void()>& caller_monitor, const std::function<void()>& abort);
private:
    struct State;
    std::unique_ptr<State> state_;
};
// One frozen role and inventory. Solve calls may vary only exclusions, budgets
// and reservation capacities; scoring is prepared once and items scored lazily.
class PreparedLoadout {
public:
    PreparedLoadout(const Json& request, const Json& inventory, SearchExecutor* executor = nullptr);
    ~PreparedLoadout();
    Json Solve(const Json& request, const InventoryCapacities& capacities, const AlgorithmCancelCheck& cancel,
        const AlgorithmProgressCallback& progress = {}, bool first_feasible = false);
private:
    struct State;
    std::unique_ptr<State> state_;
};
Json OptimizeReservedLoadout(const Json& request, const Json& inventory, const InventoryCapacities& capacities,
    const AlgorithmCancelCheck& cancel, const AlgorithmProgressCallback& progress = {});
// Production feasibility mode used by the team coverage pass. It validates
// the same inventory, board, ownership, set, keep and stat constraints.
Json OptimizeFirstLoadout(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel,
    const AlgorithmProgressCallback& progress = {});
[[noreturn]] inline void Fail(AlgorithmFailure reason = AlgorithmFailure::invalid_request) { throw InvalidAlgorithm{reason}; }
inline double Number(const Json& value, AlgorithmFailure reason = AlgorithmFailure::invalid_request) {
    if (!value.is_number()) Fail(reason);
    const auto result = value.get<double>();
    if (!std::isfinite(result)) Fail(reason);
    return result;
}
inline int Integer(const Json& value, int low, int high) {
    if (!value.is_number_integer()) Fail();
    const auto result = Number(value);
    if (result < low || result > high) Fail();
    return static_cast<int>(result);
}
inline unsigned SearchWorkers(const Json& request) {
    const auto requested = request.contains("worker_count") ? Integer(request.at("worker_count"), 0, 32) : 0;
    const auto available = std::max(1U, std::thread::hardware_concurrency());
    if (requested) return std::min(static_cast<unsigned>(requested), available);
    const auto reserved = std::max(2U, available / 4);
    return std::min(16U, available > reserved ? available - reserved : 1U);
}
inline std::string Id(const Json& value) {
    if (!value.is_string()) Fail();
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > 128) Fail();
    return result;
}
inline std::uint64_t Key(const Json& value) {
    if (!value.is_object()) Fail();
    const auto component = [&](const char* field) {
        if (!value.contains(field) || !value.at(field).is_number_integer()) Fail();
        const auto part = Number(value.at(field));
        if (part < 0 || part > UINT32_MAX) Fail();
        return static_cast<std::uint32_t>(part);
    };
    const auto slot = component("slot"), serial = component("serial");
    if (!slot && !serial) Fail();
    return (static_cast<std::uint64_t>(serial) << 32U) | slot;
}
inline Json Uid(std::uint64_t key) { return {{"slot", static_cast<std::uint32_t>(key)}, {"serial", key >> 32U}}; }
inline bool SoftwareRole(const Json& role) {
    return (!role.contains("character") || role.at("character").is_null()) && role.contains("role_id");
}
inline std::string RoleKey(const Json& role) {
    if (role.contains("slot") && role.contains("serial")) return "native:" + std::to_string(Key(role));
    if (!SoftwareRole(role)) return "native:" + std::to_string(Key(role.at("character")));
    const auto id = Id(role.at("role_id"));
    return "software:" + id;
}
inline Json RoleIdentity(const Json& role) {
    if (!SoftwareRole(role)) return role.at("character");
    static_cast<void>(RoleKey(role)); return {{"character", nullptr}, {"role_id", role.at("role_id")}};
}
inline void Snapshot(const Json& request, const Json& inventory) {
    if (!inventory.is_object() || !inventory.value("complete", false) || !inventory.contains("items") ||
        !inventory.at("items").is_array() || !inventory.contains("characters") || !inventory.at("characters").is_array())
        Fail(AlgorithmFailure::inventory_unavailable);
    if (!request.is_object() || !request.contains("snapshot_id")) Fail();
    if (!inventory.contains("snapshot_id") || request.at("snapshot_id") != inventory.at("snapshot_id")) Fail(AlgorithmFailure::stale_snapshot);
}
inline std::set<std::uint64_t> Used(const Json& plan) {
    std::set<std::uint64_t> result;
    if (plan.contains("core") && !plan.at("core").is_null()) result.insert(Key(plan.at("core")));
    for (const auto& place : plan.at("placements")) if (!result.insert(Key(place.at("equipment"))).second)
        Fail(AlgorithmFailure::invalid_inventory);
    return result;
}

// Owned by one computation thread. Only the callback crosses to its mailbox;
// inner searches share the outer phase and report cumulative work offsets.
struct ProgressReporter {
    AlgorithmProgressCallback callback;
    AlgorithmProgressSnapshot value;
    Clock::time_point begin, last{};

    ProgressReporter(const AlgorithmProgressCallback& publish, Clock::time_point start,
                     std::int64_t duration, std::size_t nodes, std::size_t combinations, bool count_combinations)
        : callback(publish), begin(start) {
        value.time_limit_ms = duration; value.max_nodes = nodes;
        value.combination_budget = combinations; value.combinations_available = count_combinations;
    }
    void Sample(std::size_t nodes, std::size_t combinations, bool force = false) {
        if (!callback) return;
        const auto now = Clock::now();
        if (!force && now - last < std::chrono::milliseconds(100)) return;
        value.nodes = nodes; value.combinations = combinations;
        value.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
        callback(value); last = now;
    }
};

struct Budget {
    const AlgorithmCancelCheck& cancel;
    Clock::time_point begin{Clock::now()}, deadline;
    std::size_t nodes{}, maximum_nodes{}, calls{}, maximum_calls{};
    bool exhaustive{true};
    std::string reason{"complete"};
    std::shared_ptr<ProgressReporter> progress;
    std::size_t base_nodes{}, base_calls{}, sample_checks{};
    unsigned configured_workers{}, peak_workers{1};
    double solver_ms{}, preparation_ms{}, base_solver_ms{}, base_preparation_ms{};
    Budget(const Json& request, const AlgorithmCancelCheck& check, const AlgorithmProgressCallback& publish = {},
           bool count_combinations = true) : cancel(check) {
        configured_workers = request.contains("worker_count") ? static_cast<unsigned>(Integer(request.at("worker_count"), 0, 32)) : 0;
        const auto duration = request.contains("time_limit_ms") ? Integer(request.at("time_limit_ms"), 1, algorithm_limits::kMaximumTimeLimitMs) : algorithm_limits::kDefaultTimeLimitMs;
        maximum_nodes = request.contains("max_nodes") ? static_cast<std::size_t>(Integer(request.at("max_nodes"), 1, algorithm_limits::kMaximumNodes)) : algorithm_limits::kDefaultNodes;
        maximum_calls = request.contains("combination_budget") ? static_cast<std::size_t>(Integer(request.at("combination_budget"), 1, algorithm_limits::kMaximumCombinations)) : algorithm_limits::kDefaultCombinations;
        deadline = begin + std::chrono::milliseconds(duration);
        if (publish) {
            progress = std::make_shared<ProgressReporter>(publish, begin, duration, maximum_nodes, maximum_calls, count_combinations);
            Sample(true);
        }
    }
    Budget(const Budget& parent, std::size_t units, std::size_t remaining_units, std::size_t minimum_nodes)
        : cancel(parent.cancel), progress(parent.progress), base_nodes(parent.base_nodes + parent.nodes),
          base_calls(parent.base_calls + parent.calls), configured_workers(parent.configured_workers),
          base_solver_ms(parent.base_solver_ms + parent.solver_ms), base_preparation_ms(parent.base_preparation_ms + parent.preparation_ms) {
        const auto available_nodes = parent.maximum_nodes - parent.nodes;
        const auto available_calls = parent.maximum_calls - parent.calls;
        maximum_nodes = std::min(available_nodes, std::max(minimum_nodes, available_nodes * units / remaining_units));
        maximum_calls = std::min(available_calls, std::max(units, available_calls * units / remaining_units));
        const auto available_time = std::chrono::duration_cast<std::chrono::milliseconds>(parent.deadline - begin).count();
        deadline = std::min(parent.deadline, begin + std::chrono::milliseconds(
            std::max<std::int64_t>(1, available_time * static_cast<std::int64_t>(units) / static_cast<std::int64_t>(remaining_units))));
    }
    bool Stop() {
        if (cancel && cancel()) Fail(AlgorithmFailure::cancelled);
        if ((++sample_checks & 127U) == 0) Sample();
        if (Clock::now() >= deadline) { exhaustive = false; reason = "time_budget"; }
        else if (nodes >= maximum_nodes) { exhaustive = false; reason = "node_budget"; }
        else if (calls >= maximum_calls) { exhaustive = false; reason = "combination_budget"; }
        return !exhaustive;
    }
    void Sample(bool force = false) const {
        if (progress) {
            progress->value.solver_ms = base_solver_ms + solver_ms;
            progress->value.preparation_ms = base_preparation_ms + preparation_ms;
            progress->Sample(base_nodes + nodes, base_calls + calls, force);
        }
    }
    void Phase(AlgorithmProgressPhase phase, std::uint32_t group = 0) const {
        if (!progress) return;
        progress->value.phase = phase; progress->value.current_group = group;
        Sample(true);
    }
    void Team(std::size_t groups, std::size_t roles) const {
        if (!progress) return;
        progress->value.multi_role = true;
        progress->value.groups_total = static_cast<std::uint32_t>(groups);
        progress->value.roles_total = static_cast<std::uint32_t>(roles);
        Sample(true);
    }
    void Covered(std::size_t groups, std::size_t roles) const {
        if (!progress) return;
        progress->value.groups_visited = static_cast<std::uint32_t>(groups);
        progress->value.roles_completed = static_cast<std::uint32_t>(roles);
        Sample(true);
    }
    AlgorithmProgressCallback ChildProgress() const {
        if (!progress) return {};
        return [reporter = progress, offset = base_nodes + nodes, combinations = base_calls + calls,
                solver = base_solver_ms + solver_ms, preparation = base_preparation_ms + preparation_ms](const AlgorithmProgressSnapshot& child) {
            reporter->value.worker_count = child.worker_count;
            reporter->value.solver_ms = solver + static_cast<double>(child.elapsed_ms);
            reporter->value.preparation_ms = preparation + child.preparation_ms;
            reporter->Sample(offset + static_cast<std::size_t>(child.nodes), combinations);
        };
    }
    void Child(Json& request) const {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        request["time_limit_ms"] = std::max<std::int64_t>(1, remaining);
        request["max_nodes"] = std::max<std::size_t>(1, maximum_nodes - nodes);
        request["worker_count"] = configured_workers;
    }
    void Account(const Json& result, bool stop_on_child_limit = true) {
        ++calls; nodes += result.at("search").value("nodes", std::size_t{});
        peak_workers = std::max(peak_workers, result.at("search").value("worker_count", 1U));
        solver_ms += result.at("search").value("elapsed_ms_exact", result.at("search").value("elapsed_ms", 0.));
        preparation_ms += result.at("search").value("preparation_ms", 0.);
        Sample();
        if (stop_on_child_limit && !result.at("search").value("exhaustive", false)) {
            exhaustive = false; reason = result.at("search").value("stop_reason", std::string("node_budget"));
        }
    }
    void Absorb(const Budget& child) { nodes += child.nodes; calls += child.calls; peak_workers = std::max(peak_workers, child.peak_workers); solver_ms += child.solver_ms; preparation_ms += child.preparation_ms; Sample(); }
    Json Report() const {
        Sample(true);
        return {{"exhaustive", exhaustive}, {"stop_reason", reason}, {"nodes", nodes}, {"combinations", calls}, {"worker_count", peak_workers},
            {"solver_ms", solver_ms}, {"preparation_ms", preparation_ms},
            {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count()}};
    }
};
} // namespace equipment_manager::algorithm_detail
