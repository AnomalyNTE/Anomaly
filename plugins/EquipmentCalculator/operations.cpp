#include "operations.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace equipment_calculator {
namespace {

[[noreturn]] void Reject(CommandFailure reason) { throw InvalidCommand{reason}; }

std::uint32_t Component(const Json& uid, const char* field) {
    if (!uid.is_object() || !uid.contains(field) || !uid[field].is_number_integer())
        Reject(CommandFailure::invalid_request);
    const auto value = uid[field].get<std::int64_t>();
    if (value <= 0 || value >= std::numeric_limits<std::uint32_t>::max())
        Reject(CommandFailure::invalid_request);
    return static_cast<std::uint32_t>(value);
}

Json Uid(const Json& value) {
    return {{"slot", Component(value, "slot")}, {"serial", Component(value, "serial")}};
}

std::uint64_t UidKey(const Json& value) {
    return (std::uint64_t{Component(value, "serial")} << 32) | Component(value, "slot");
}

int Coordinate(const Json& value) {
    if (!value.is_number_integer()) Reject(CommandFailure::invalid_request);
    const auto number = value.get<std::int64_t>();
    if (number < 1 || number > 5) Reject(CommandFailure::invalid_layout);
    return static_cast<int>(number);
}

const Json* Find(const Json& entries, const Json& uid) {
    for (const auto& entry : entries)
        if (entry.contains("uid") && SameUid(entry["uid"], uid)) return &entry;
    return nullptr;
}

const Json& Character(const Json& inventory, const Json& uid) {
    const auto* value = Find(inventory.at("characters"), uid);
    if (!value) Reject(CommandFailure::missing_character);
    return *value;
}

const Json& Item(const Json& inventory, const Json& uid) {
    const auto* value = Find(inventory.at("items"), uid);
    if (!value) Reject(CommandFailure::missing_equipment);
    return *value;
}

bool Owned(const Json& item) {
    return item.contains("character") && item["character"].is_object();
}

std::vector<std::array<int, 2>> Cells(const Json& item, int row, int column) {
    if (row < 1 || row > 5 || column < 1 || column > 5 ||
        !item.contains("cells") || !item["cells"].is_array() || item["cells"].empty())
        Reject(CommandFailure::invalid_layout);
    std::vector<std::array<int, 2>> cells;
    for (const auto& cell : item["cells"]) {
        if (!cell.is_array() || cell.size() != 2 ||
            !cell[0].is_number_integer() || !cell[1].is_number_integer())
            Reject(CommandFailure::invalid_layout);
        const int r = row + cell[0].get<int>(), c = column + cell[1].get<int>();
        if (r < 1 || r > 5 || c < 1 || c > 5) Reject(CommandFailure::invalid_layout);
        cells.push_back({r, c});
    }
    return cells;
}

void Mark(const Json& item, int row, int column, std::set<std::array<int, 2>>& occupied,
          const Json* allowed = nullptr) {
    for (const auto& cell : Cells(item, row, column)) {
        if (allowed && std::find(allowed->begin(), allowed->end(), Json::array({cell[0], cell[1]})) == allowed->end())
            Reject(CommandFailure::invalid_layout);
        if (!occupied.insert(cell).second) Reject(CommandFailure::occupied);
    }
}

Json Placement(const Json& value, const Json& inventory, std::set<std::uint64_t>& ids,
               std::set<std::array<int, 2>>& cells, const Json& allowed) {
    const auto uid = Uid(value.at("equipment"));
    if (!ids.insert(UidKey(uid)).second) Reject(CommandFailure::duplicate_uid);
    const auto& item = Item(inventory, uid);
    if (item.at("kind") != "module") Reject(CommandFailure::wrong_type);
    const int row = Coordinate(value.at("row")), column = Coordinate(value.at("column"));
    Mark(item, row, column, cells, &allowed);
    return {{"equipment", uid}, {"row", row}, {"column", column}};
}

Json LoadoutForBoard(const Json& input, const Json& inventory, std::set<std::uint64_t>& ids,
                     const Json& character, const Json& allowed_cells) {
    const auto& placements = input.at("placements");
    if (!placements.is_array() || placements.size() > 25)
        Reject(CommandFailure::invalid_layout);
    Json result{{"character", character}, {"placements", Json::array()}, {"core", nullptr}};
    std::set<std::array<int, 2>> cells;
    for (const auto& placement : placements)
        result["placements"].push_back(Placement(placement, inventory, ids, cells, allowed_cells));
    if (input.contains("core") && !input["core"].is_null()) {
        const auto core = Uid(input["core"]);
        if (!ids.insert(UidKey(core)).second) Reject(CommandFailure::duplicate_uid);
        if (Item(inventory, core).at("kind") != "core") Reject(CommandFailure::wrong_type);
        result["core"] = core;
    }
    return result;
}

Json Loadout(const Json& input, const Json& inventory, std::set<std::uint64_t>& ids) {
    const auto character = Uid(input.at("character"));
    return LoadoutForBoard(input, inventory, ids, character, Character(inventory, character).at("allowed_cells"));
}

void AppendLoadout(const Json& plan, const Json& inventory, std::vector<Json>& steps) {
    const auto& character = plan["character"];
    bool native_batch = !plan["core"].is_null() && !plan["placements"].empty();
    const auto available = [&](const Json& uid) {
        const auto& item = Item(inventory, uid);
        return !Owned(item) || SameUid(item["character"], character);
    };
    if (!plan["core"].is_null()) native_batch = native_batch && available(plan["core"]);
    for (const auto& place : plan["placements"])
        native_batch = native_batch && available(place["equipment"]);
    if (native_batch) {
        auto command = plan;
        command["action"] = "equip_one_key";
        steps.push_back(std::move(command));
        return;
    }
    if (!plan["core"].is_null())
        steps.push_back({{"action", "place_core"}, {"character", character},
                         {"equipment", plan["core"]}});
    for (const auto& place : plan["placements"]) {
        auto command = place;
        command["action"] = "place_module";
        command["character"] = character;
        steps.push_back(std::move(command));
    }
}

void AppendState(const Json& input, const Json& inventory, std::vector<Json>& steps) {
    const auto uid = Uid(input.at("equipment"));
    const auto& item = Item(inventory, uid);
    const std::string field = input.at("field").get<std::string>();
    if ((field != "locked" && field != "discarded") || !input.at("value").is_boolean())
        Reject(CommandFailure::invalid_request);
    const bool value = input["value"].get<bool>();
    const std::string other = field == "locked" ? "discarded" : "locked";
    const auto add = [&](const std::string& name, bool target) {
        steps.push_back({{"action", name == "locked" ? "set_item_locked" : "set_item_discarded"},
                         {"equipment", uid}, {name, target}});
    };
    if (value && item.value(other, false)) add(other, false);
    add(field, value);
}

} // namespace

Json ValidateSavedLoadout(const Json& plan, const Json& inventory, const Json& allowed_cells) {
    std::set<std::uint64_t> ids;
    return LoadoutForBoard(plan, inventory, ids, plan.value("character", Json(nullptr)), allowed_cells);
}

bool SameUid(const Json& left, const Json& right) {
    return left.is_object() && right.is_object() && left.contains("slot") &&
        left.contains("serial") && right.contains("slot") && right.contains("serial") &&
        left["slot"] == right["slot"] && left["serial"] == right["serial"];
}

std::vector<Json> PrepareOperation(const Json& request, const Json& inventory) {
    const std::string action = request.at("action").get<std::string>();
    std::vector<Json> steps;
    if (action == "equip_one_key" || action == "apply_plans") {
        std::vector<Json> plans;
        std::set<std::uint64_t> ids, characters;
        if (action == "apply_plans") {
            if (!request.at("plans").is_array() || request["plans"].empty() || request["plans"].size() > 32)
                Reject(CommandFailure::invalid_request);
            for (const auto& value : request["plans"]) plans.push_back(Loadout(value, inventory, ids));
        } else plans.push_back(Loadout(request, inventory, ids));
        std::vector<Json> changed;
        for (const auto& plan : plans) {
            if (!characters.insert(UidKey(plan["character"])).second)
                Reject(CommandFailure::duplicate_uid);
            auto expected = plan;
            expected["action"] = "verify_loadout";
            if (StepSatisfied(expected, inventory)) continue;
            changed.push_back(plan);
            steps.push_back({{"action", "unequip_all"}, {"character", plan["character"]}});
        }
        for (const auto& plan : changed) AppendLoadout(plan, inventory, steps);
        // Exact final-state checks also cover empty and module-only plans.
        for (auto plan : plans) {
            plan["action"] = "verify_loadout";
            steps.push_back(std::move(plan));
        }
        return steps;
    }
    if (action == "set_item_states") {
        const auto& operations = request.at("operations");
        if (!operations.is_array() || operations.empty() || operations.size() > 512)
            Reject(CommandFailure::invalid_request);
        std::set<std::pair<std::uint64_t, std::string>> fields;
        std::map<std::uint64_t, std::set<std::string>> enabled;
        for (const auto& operation : operations) {
            const auto key = std::make_pair(UidKey(operation.at("equipment")),
                                            operation.at("field").get<std::string>());
            if (!fields.insert(key).second) Reject(CommandFailure::duplicate_uid);
            if (operation.at("value") == true && enabled[key.first].insert(key.second).second &&
                enabled[key.first].size() > 1) Reject(CommandFailure::invalid_request);
            AppendState(operation, inventory, steps);
        }
        return steps;
    }
    if (action == "set_item_locked" || action == "set_item_discarded") {
        const char* field = action == "set_item_locked" ? "locked" : "discarded";
        AppendState({{"equipment", request.at("equipment")}, {"field", field},
                     {"value", request.at(field)}}, inventory, steps);
        return steps;
    }
    static constexpr std::array supported{
        "equip_module", "equip_core", "replace_core", "unequip_module", "unequip_core",
        "unequip_all", "move_module_to_character", "move_core_to_character"};
    if (std::find(supported.begin(), supported.end(), action) == supported.end())
        Reject(CommandFailure::unsupported_action);
    Json command{{"action", action}, {"character", Uid(request.at("character"))}};
    const auto& target = Character(inventory, command["character"]);
    if (action != "unequip_all") {
        command["equipment"] = Uid(request.at("equipment"));
        const auto& item = Item(inventory, command["equipment"]);
        const bool core = action.find("core") != std::string::npos;
        if (item.at("kind") != (core ? "core" : "module")) Reject(CommandFailure::wrong_type);
        if (action == "equip_module" || action == "move_module_to_character") {
            command["row"] = Coordinate(request.at("row"));
            command["column"] = Coordinate(request.at("column"));
            std::set<std::array<int, 2>> occupied;
            for (const auto& other : inventory["items"])
                if (other["kind"] == "module" && Owned(other) &&
                    SameUid(other["character"], command["character"]) &&
                    !SameUid(other["uid"], command["equipment"]))
                    Mark(other, other.at("row").get<int>(), other.at("column").get<int>(), occupied);
            Mark(item, command["row"].get<int>(), command["column"].get<int>(), occupied, &target.at("allowed_cells"));
            if (Owned(item) && SameUid(item["character"], command["character"]) &&
                !StepSatisfied(command, inventory))
                steps.push_back({{"action", "unequip_module"}, {"character", command["character"]},
                                 {"equipment", command["equipment"]}});
            command["action"] = "place_module";
        } else if (action == "equip_core" || action == "move_core_to_character") {
            command["action"] = "place_core";
        } else if (action == "unequip_module" || action == "unequip_core") {
            if (Owned(item) && !SameUid(item["character"], command["character"]))
                Reject(CommandFailure::missing_character);
        }
    }
    steps.push_back(std::move(command));
    return steps;
}

Json ResolveStep(const Json& step, const Json& inventory) {
    auto result = step;
    const std::string action = step.at("action").get<std::string>();
    if (action == "verify_loadout") return result;
    if (step.contains("character")) Character(inventory, step["character"]);
    if (step.contains("equipment")) {
        const auto& item = Item(inventory, step["equipment"]);
        if (action == "place_module")
            result["action"] = Owned(item) ? "move_module_to_character" : "equip_module";
        if (action == "place_core") {
            if (Owned(item) && !SameUid(item["character"], step["character"]))
                result["action"] = "move_core_to_character";
            else {
                const bool has_core = std::any_of(inventory["items"].begin(), inventory["items"].end(),
                    [&](const auto& other) { return other["kind"] == "core" && Owned(other) &&
                        SameUid(other["character"], step["character"]); });
                result["action"] = has_core ? "replace_core" : "equip_core";
            }
        }
    }
    return result;
}

bool StepSatisfied(const Json& command, const Json& inventory) {
    const std::string action = command.at("action").get<std::string>();
    if (action == "unequip_all") {
        return std::none_of(inventory["items"].begin(), inventory["items"].end(), [&](const auto& item) {
            return Owned(item) && SameUid(item["character"], command["character"]);
        });
    }
    if (action == "verify_loadout" || action == "equip_one_key") {
        std::size_t observed{};
        for (const auto& item : inventory["items"])
            if (Owned(item) && SameUid(item["character"], command["character"])) ++observed;
        const bool has_core = command.contains("core") && !command["core"].is_null();
        if (observed != command["placements"].size() + (has_core ? 1U : 0U)) return false;
        if (has_core && !StepSatisfied({{"action", "equip_core"}, {"character", command["character"]},
                                       {"equipment", command["core"]}}, inventory)) return false;
        for (auto place : command["placements"]) {
            place["action"] = "equip_module";
            place["character"] = command["character"];
            if (!StepSatisfied(place, inventory)) return false;
        }
        return true;
    }
    const auto* item = Find(inventory["items"], command.at("equipment"));
    if (!item) return false;
    if (action == "set_item_locked") return (*item)["locked"] == command["locked"];
    if (action == "set_item_discarded") return (*item)["discarded"] == command["discarded"];
    if (action == "unequip_module" || action == "unequip_core") return !Owned(*item);
    if (!Owned(*item) || !SameUid((*item)["character"], command["character"])) return false;
    if (action == "equip_module" || action == "move_module_to_character" || action == "place_module")
        return (*item)["row"] == command["row"] && (*item)["column"] == command["column"];
    return true;
}

} // namespace equipment_calculator
