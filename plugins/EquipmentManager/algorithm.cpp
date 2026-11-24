#include "algorithm_support.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace equipment_manager {
namespace {
using Json = nlohmann::json;
using Cell = std::array<int, 2>;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Reject(AlgorithmFailure reason) { throw InvalidAlgorithm{reason}; }

double Number(const Json& value, AlgorithmFailure reason = AlgorithmFailure::invalid_request) {
    if (!value.is_number()) Reject(reason);
    const auto number = value.get<double>();
    if (!std::isfinite(number)) Reject(reason);
    return number;
}

int Integer(const Json& value, int minimum, int maximum,
            AlgorithmFailure reason = AlgorithmFailure::invalid_request) {
    if (!value.is_number_integer()) Reject(reason);
    const auto number = Number(value, reason);
    if (number < minimum || number > maximum) Reject(reason);
    return static_cast<int>(number);
}

std::uint64_t UidKey(const Json& uid, AlgorithmFailure reason) {
    if (!uid.is_object() || !uid.contains("slot") || !uid.contains("serial")) Reject(reason);
    const auto component = [&](const char* key) {
        if (!uid.at(key).is_number_integer()) Reject(reason);
        const auto number = Number(uid.at(key), reason);
        if (number < 0 || number > UINT32_MAX) Reject(reason);
        return static_cast<std::uint32_t>(number);
    };
    const auto slot = component("slot"), serial = component("serial");
    if (!slot && !serial) Reject(reason);
    return (static_cast<std::uint64_t>(serial) << 32U) | slot;
}

std::string Id(const Json& value, AlgorithmFailure reason) {
    if (!value.is_string()) Reject(reason);
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > 128) Reject(reason);
    return result;
}

bool Flag(const Json& input, const char* key, bool fallback) {
    const auto found = input.find(key);
    if (found == input.end()) return fallback;
    if (!found->is_boolean()) Reject(AlgorithmFailure::invalid_request);
    return found->get<bool>();
}

std::map<std::string, int, std::less<>> FourPieceShapes(const Json& shapes) {
    if (!shapes.is_array() || shapes.size() != 4) Reject(AlgorithmFailure::incomplete_set);
    std::map<std::string, int, std::less<>> result;
    for (const auto& shape : shapes)
        if (!result.emplace(Id(shape, AlgorithmFailure::incomplete_set), 1).second)
            Reject(AlgorithmFailure::incomplete_set);
    return result;
}

std::vector<std::string> Ids(const Json& input, const char* key, std::size_t maximum = 32) {
    const auto found = input.find(key);
    if (found == input.end()) return {};
    if (!found->is_array() || found->size() > maximum) Reject(AlgorithmFailure::invalid_request);
    std::vector<std::string> result;
    for (const auto& value : *found) {
        auto id = Id(value, AlgorithmFailure::invalid_request);
        if (std::find(result.begin(), result.end(), id) != result.end()) Reject(AlgorithmFailure::invalid_request);
        result.push_back(std::move(id));
    }
    return result;
}

bool MainOnly(std::string_view id) {
    constexpr std::array<std::string_view, 8> properties{
        "HealUp", "DamageUpChaosBase", "DamageUpCosmosBase", "DamageUpIncantationBase",
        "DamageUpLakshanaBase", "DamageUpNatureBase", "DamageUpPsycheBase", "DamageUpPsychicallyBase"};
    return std::find(properties.begin(), properties.end(), id) != properties.end();
}

using Weights = std::map<std::string, double, std::less<>>;
Weights ReadWeights(const Json& value) {
    if (!value.is_object() || value.size() > 128) Reject(AlgorithmFailure::invalid_request);
    Weights result;
    for (const auto& [id, entry] : value.items()) {
        if (id.empty() || id.size() > 128) Reject(AlgorithmFailure::invalid_request);
        const auto weight = Number(entry);
        if (weight < 0 || weight > 1) Reject(AlgorithmFailure::invalid_request);
        result.emplace(id, weight);
    }
    return result;
}

double Weight(const Weights& weights, std::string_view id) {
    const auto found = weights.find(id);
    return found == weights.end() ? 0 : found->second;
}

struct Preferences {
    struct Filter {
        std::vector<std::string> qualities, sub_blacklist, main_blacklist, main_allow;
        bool zero{};
    } module_filter, core_filter;
    struct Limit { std::string id; double minimum{}, maximum{}; bool has_minimum{}, has_maximum{}; };
    Weights weights, main_weights;
    double maximum_weight{1};
    std::vector<std::string> priority;
    bool equal_priority{};
    double priority_min_ratio{.4};
    std::vector<std::string> types;
    std::vector<Limit> limits;
    std::set<std::string, std::less<>> properties;
    Weights extra_buffs, extra_bases;
    int extra_area{};
    double crit_minimum{}, crit_maximum{}, crit_minimum_base{}, crit_maximum_base{};
    bool has_crit_minimum{}, has_crit_maximum{};
};

Preferences ReadPreferences(const Json& input) {
    if (!input.is_object() || !input.contains("weights")) Reject(AlgorithmFailure::invalid_request);
    Preferences result;
    result.weights = ReadWeights(input.at("weights"));
    result.main_weights = input.contains("main_weights") ? ReadWeights(input.at("main_weights")) : result.weights;
    std::vector<double> weights;
    for (const auto& [id, weight] : result.weights) if (!MainOnly(id) && weight > 0) weights.push_back(weight);
    std::sort(weights.begin(), weights.end(), std::greater<>{});
    const auto count = std::min<std::size_t>(weights.size(), 4);
    const auto total = std::accumulate(weights.begin(), weights.begin() + count, 0.0);
    if (total > 0) result.maximum_weight = total;
    result.priority = Ids(input, "priority_stats", 4);
    result.equal_priority = Flag(input, "priority_equal", false);
    if (const auto grade = input.find("priority_min_grade"); grade != input.end()) {
        const auto id = Id(*grade, AlgorithmFailure::invalid_request);
        constexpr std::array<std::string_view, 8> grades{"D", "C", "B", "A", "S", "SS", "SSS", "ACE"};
        const auto found = std::find(grades.begin(), grades.end(), id);
        if (found == grades.end()) Reject(AlgorithmFailure::invalid_request);
        const auto index = static_cast<std::size_t>(found - grades.begin());
        constexpr std::array ratios{0., .2, .3, .4, .5, .6, .7, .8};
        result.priority_min_ratio = ratios[index];
    }
    if (Flag(input, "priority_ignore_grade", false)) result.priority_min_ratio = 0;
    result.types = Ids(input, "types", 2);
    for (const auto& type : result.types)
        if (type != "core" && type != "module") Reject(AlgorithmFailure::invalid_request);
    if (const auto filters = input.find("filters"); filters != input.end()) {
        if (!filters->is_object()) Reject(AlgorithmFailure::invalid_request);
        const auto read = [&](const char* kind, Preferences::Filter& filter) {
            const auto found = filters->find(kind);
            if (found == filters->end()) return;
            if (!found->is_object()) Reject(AlgorithmFailure::invalid_request);
            filter.qualities = Ids(*found, "qualities", 8);
            filter.sub_blacklist = Ids(*found, "sub_blacklist");
            filter.main_blacklist = Ids(*found, "main_blacklist");
            filter.main_allow = Ids(*found, "main_allow");
            const auto mode = found->value("blacklist_mode", std::string("exclude"));
            if (mode != "exclude" && mode != "zero_weight") Reject(AlgorithmFailure::invalid_request);
            filter.zero = mode == "zero_weight";
        };
        read("module", result.module_filter); read("core", result.core_filter);
    }
    if (const auto limits = input.find("property_limits"); limits != input.end()) {
        if (!limits->is_array() || limits->size() > 64) Reject(AlgorithmFailure::invalid_request);
        for (const auto& value : *limits) {
            Preferences::Limit limit;
            limit.id = Id(value.at("id"), AlgorithmFailure::invalid_request);
            if (value.contains("minimum")) { limit.minimum = Number(value.at("minimum")); limit.has_minimum = true; }
            if (value.contains("maximum")) { limit.maximum = Number(value.at("maximum")); limit.has_maximum = true; }
            if ((!limit.has_minimum && !limit.has_maximum) ||
                (limit.has_minimum && limit.has_maximum && limit.minimum > limit.maximum)) Reject(AlgorithmFailure::invalid_request);
            result.properties.insert(limit.id); result.limits.push_back(std::move(limit));
        }
    }
    const auto crit = [&](const char* bound, const char* baseline, double& target, double& base, bool& present) {
        if (!input.contains(bound)) return;
        target = Number(input.at(bound));
        if (target < 0 || target > 1) Reject(AlgorithmFailure::invalid_request);
        if (target == 0 && std::string_view(bound) == "crit_maximum") return;
        if (!input.contains(baseline)) Reject(AlgorithmFailure::incomplete_attributes);
        base = Number(input.at(baseline)); present = true; result.properties.insert("CritBase");
    };
    crit("crit_minimum", "crit_minimum_base", result.crit_minimum, result.crit_minimum_base, result.has_crit_minimum);
    crit("crit_maximum", "crit_maximum_base", result.crit_maximum, result.crit_maximum_base, result.has_crit_maximum);
    if (const auto extra = input.find("extra_shape"); extra != input.end()) {
        if (!extra->is_object()) Reject(AlgorithmFailure::invalid_request);
        result.extra_area = Integer(extra->at("area"), 1, 4);
        const auto read_values = [&](const char* name, Weights& values, bool positive) {
            if (!extra->contains(name) || !extra->at(name).is_object()) Reject(AlgorithmFailure::incomplete_attributes);
            for (const auto& [id, value] : extra->at(name).items()) {
                const auto number = Number(value);
                if (id.empty() || (positive && number <= 0)) Reject(AlgorithmFailure::invalid_request);
                values.emplace(id, number);
            }
        };
        read_values("buffs", result.extra_buffs, false); read_values("gold_base_values", result.extra_bases, true);
        for (const auto& [id, value] : result.extra_buffs)
            if (value > 0 && Weight(result.weights, id) > 0 && !result.extra_bases.contains(id))
                Reject(AlgorithmFailure::incomplete_attributes);
    }
    return result;
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool Eligible(const Json& item, const Preferences& preferences) {
    const auto kind = item.value("kind", std::string{});
    if (!preferences.types.empty() && !Contains(preferences.types, kind)) return false;
    const auto& filter = kind == "core" ? preferences.core_filter : preferences.module_filter;
    if (!filter.qualities.empty() && !Contains(filter.qualities, item.value("quality", std::string{}))) return false;
    if (kind == "core" && item.contains("main_stat")) {
        const auto id = item.at("main_stat").value("id", std::string{});
        if ((!filter.main_allow.empty() && !Contains(filter.main_allow, id)) ||
            (!filter.zero && Contains(filter.main_blacklist, id))) return false;
    }
    if (kind == "module" && !filter.zero && !filter.main_blacklist.empty() && item.contains("main_stats"))
        for (const auto& stat : item.at("main_stats"))
            if (Contains(filter.main_blacklist, stat.value("id", std::string{}))) return false;
    if (!filter.zero && item.contains("sub_stats"))
        for (const auto& stat : item.at("sub_stats"))
            if (Contains(filter.sub_blacklist, stat.value("id", std::string{}))) return false;
    return true;
}

std::int64_t Cents(double value) {
    // Fixed decimal conversion rounds the actual binary value to nearest,
    // ties-to-even, matching Python round(value, 2), including 2.675 cases.
    if (!std::isfinite(value) || value < 0 || value >= static_cast<double>(INT64_MAX) / 100)
        Reject(AlgorithmFailure::invalid_inventory);
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                          std::chars_format::fixed, 2);
    if (converted.ec != std::errc{}) Reject(AlgorithmFailure::invalid_inventory);
    std::int64_t result{};
    for (auto cursor = buffer.data(); cursor != converted.ptr; ++cursor)
        if (*cursor != '.') result = result * 10 + (*cursor - '0');
    return result;
}

const char* ItemGrade(double score, std::size_t area) {
    const auto ratio = area ? score / (static_cast<double>(area) * 10) : 0;
    if (ratio >= .8) return "ACE";
    if (ratio >= .7) return "SSS";
    if (ratio >= .6) return "SS";
    if (ratio >= .5) return "S";
    if (ratio >= .4) return "A";
    if (ratio >= .3) return "B";
    if (ratio >= .2) return "C";
    return "D";
}

const char* TotalGrade(double score) {
    if (score >= 280) return "ACE";
    if (score >= 260) return "SSS";
    if (score >= 240) return "SS";
    if (score >= 220) return "S";
    if (score >= 200) return "A";
    if (score >= 180) return "B";
    if (score >= 160) return "C";
    return "D";
}

std::vector<Cell> ReadCells(const Json& item) {
    const auto found = item.find("cells");
    if (found == item.end() || !found->is_array() || found->empty() || found->size() > 4)
        Reject(AlgorithmFailure::incomplete_shape);
    std::vector<Cell> cells;
    for (const auto& cell : *found) {
        if (!cell.is_array() || cell.size() != 2) Reject(AlgorithmFailure::incomplete_shape);
        const Cell point{Integer(cell[0], -4, 4, AlgorithmFailure::incomplete_shape),
                         Integer(cell[1], -4, 4, AlgorithmFailure::incomplete_shape)};
        if (std::find(cells.begin(), cells.end(), point) != cells.end()) Reject(AlgorithmFailure::incomplete_shape);
        cells.push_back(point);
    }
    std::sort(cells.begin(), cells.end());
    return cells;
}

struct ScoredItem {
    const Json* source{};
    std::uint64_t uid{};
    std::string shape;
    std::vector<Cell> cells;
    std::int64_t score{};
    double main_score{}, sub_score{}, quality{};
    std::size_t area{};
    int depth{};
    std::vector<std::string> matched_stats;
    Weights stats;
    double hidden_bonus{}, ranking{};
    double maximum_weight{1};
};

ScoredItem Score(const Json& item, const Preferences& preferences) {
    if (!item.is_object() || !item.contains("kind") || !item.at("kind").is_string())
        Reject(AlgorithmFailure::invalid_inventory);
    const bool core = item.at("kind") == "core";
    if (!core && item.at("kind") != "module") Reject(AlgorithmFailure::invalid_inventory);
    if (!item.value("sub_stats_complete", false) || !item.contains("sub_stats") || !item.at("sub_stats").is_array())
        Reject(AlgorithmFailure::incomplete_attributes);
    const auto quality = item.value("quality", std::string{});
    const double coefficient = quality == "Gold" || quality == "Red" ? 1 : quality == "Purple" ? .8 : quality == "Blue" ? .6 : 0;
    if (coefficient == 0) Reject(AlgorithmFailure::unsupported_quality);
    ScoredItem result;
    const auto& filter = core ? preferences.core_filter : preferences.module_filter;
    auto maximum_weight = preferences.maximum_weight;
    if (filter.zero && !filter.sub_blacklist.empty()) {
        std::vector<double> effective;
        for (const auto& [id, weight] : preferences.weights)
            if (!MainOnly(id) && !Contains(filter.sub_blacklist, id) && weight > 0) effective.push_back(weight);
        std::sort(effective.begin(), effective.end(), std::greater<>{});
        maximum_weight = std::accumulate(effective.begin(), effective.begin() + std::min<std::size_t>(4, effective.size()), 0.0);
        if (maximum_weight <= 0) maximum_weight = 1;
    }
    result.maximum_weight = maximum_weight;
    result.source = &item;
    result.quality = coefficient;
    if (core) result.area = 15;
    else {
        if (!item.contains("shape_id")) Reject(AlgorithmFailure::incomplete_shape);
        result.shape = Id(item.at("shape_id"), AlgorithmFailure::incomplete_shape);
        result.cells = ReadCells(item);
        result.area = result.cells.size();
    }
    std::set<std::string, std::less<>> sub_ids;
    double actual_weight{};
    if (item.at("sub_stats").size() > 4) Reject(AlgorithmFailure::invalid_inventory);
    for (const auto& stat : item.at("sub_stats")) {
        if (!stat.is_object() || !stat.contains("id")) Reject(AlgorithmFailure::invalid_inventory);
        const auto id = Id(stat.at("id"), AlgorithmFailure::invalid_inventory);
        if (!sub_ids.insert(id).second) Reject(AlgorithmFailure::invalid_inventory);
        const auto weight = filter.zero && Contains(filter.sub_blacklist, id) ? 0 : Weight(preferences.weights, id);
        actual_weight += weight;
        if (weight > 0) result.matched_stats.push_back(id);
    }
    const double normalization = 10.0 / maximum_weight;
    // Preserve the reference's ordinary floating-point order. Its reciprocal
    // can overflow for legal subnormal weights, so divide those weights first.
    const double weighted = std::isfinite(normalization) ? normalization * actual_weight
        : (actual_weight / maximum_weight) * 10.0;
    result.sub_score = weighted * static_cast<double>(core ? 10 : result.area) * coefficient;
    if (core) {
        const auto main = item.find("main_stat");
        if (main == item.end() || !main->is_object() || !main->contains("id") ||
            !main->contains("max_value") || !main->contains("gold_max_value"))
            Reject(AlgorithmFailure::incomplete_main_stat);
        const auto id = Id(main->at("id"), AlgorithmFailure::incomplete_main_stat);
        const auto maximum = Number(main->at("max_value"), AlgorithmFailure::incomplete_main_stat);
        const auto gold = Number(main->at("gold_max_value"), AlgorithmFailure::incomplete_main_stat);
        if (maximum < 0 || gold <= 0) Reject(AlgorithmFailure::incomplete_main_stat);
        const auto main_weight = filter.zero && Contains(filter.main_blacklist, id) ? 0 : Weight(preferences.main_weights, id);
        result.main_score = main_weight * 50 * (maximum / gold);
    }
    result.score = Cents(result.main_score + result.sub_score);
    if (preferences.extra_area == static_cast<int>(result.area) && !core) {
        double maximum_single{};
        for (const auto& [id, weight] : preferences.weights)
            if (!MainOnly(id)) maximum_single = std::max(maximum_single, weight);
        if (maximum_single <= 0) maximum_single = 1;
        for (const auto& [id, value] : preferences.extra_buffs) if (value > 0 && Weight(preferences.weights, id) > 0)
            result.hidden_bonus += 10 * (Weight(preferences.weights, id) / maximum_single) *
                (value / preferences.extra_bases.at(id)) / static_cast<double>(result.area);
    }
    result.ranking = static_cast<double>(result.score) / 100 + result.hidden_bonus;
    if (!std::isfinite(result.ranking)) Reject(AlgorithmFailure::invalid_inventory);
    if (!preferences.properties.empty()) {
        if (!item.value("sub_stats_values_complete", false) || !item.value("main_stats_complete", false))
            Reject(AlgorithmFailure::incomplete_attributes);
        for (const auto& id : preferences.properties) result.stats.emplace(id, 0);
        const auto add = [&](const Json& values, bool main) {
            if (!values.is_array()) Reject(AlgorithmFailure::incomplete_attributes);
            for (const auto& value : values) {
                const auto id = Id(value.at("id"), AlgorithmFailure::incomplete_attributes);
                if (!preferences.properties.contains(id)) continue;
                const auto key = main && value.contains("max_value") ? "max_value" : "value";
                result.stats[id] += Number(value.at(key), AlgorithmFailure::incomplete_attributes);
                if (!std::isfinite(result.stats[id])) Reject(AlgorithmFailure::invalid_inventory);
            }
        };
        add(item.at("sub_stats"), false);
        if (core) add(Json::array({item.at("main_stat")}), true);
        else add(item.at("main_stats"), true);
        if (!core && preferences.extra_area == static_cast<int>(result.area))
            for (const auto& [id, value] : preferences.extra_buffs) if (preferences.properties.contains(id)) result.stats[id] += value;
    }
    if (static_cast<double>(result.score) / 100 >= static_cast<double>(result.area) * 10 * preferences.priority_min_ratio) {
        for (const auto& stat : preferences.priority) {
            if (sub_ids.contains(stat)) ++result.depth;
            else if (!preferences.equal_priority) break;
        }
    }
    return result;
}

Json ScoreJson(const ScoredItem& item) {
    return {{"score", static_cast<double>(item.score) / 100},
        {"grade", ItemGrade(static_cast<double>(item.score) / 100, item.area)},
        {"area", item.area}, {"main_score", item.main_score}, {"sub_score", item.sub_score},
        {"max_weight", item.maximum_weight}, {"quality_coefficient", item.quality},
        {"priority_depth", item.depth}, {"weighted_sub_stats", item.matched_stats}, {"eligible", true},
        {"hidden_bonus", item.hidden_bonus}, {"ranking_score", item.ranking}, {"constraint_stats", item.stats}};
}

struct Placement { int row{}, column{}; std::uint32_t mask{}; };
struct Group {
    std::string shape;
    std::vector<Cell> cells;
    int depth{};
    std::vector<ScoredItem> items;
    std::vector<Placement> placements;
    std::size_t used{};
    Weights stats;
};
struct Choice { std::size_t group{}, placement{}; };
struct Pick { std::size_t group{}, item{}, placement{}; };
struct Core {
    ScoredItem item;
    std::map<std::string, int, std::less<>> shapes;
    int required{};
};

bool HigherDensity(std::int64_t left, std::size_t left_area, std::int64_t right, std::size_t right_area) {
    const auto l = static_cast<std::int64_t>(left_area), r = static_cast<std::int64_t>(right_area);
    if (left / l != right / r) return left / l > right / r;
    // Areas are at most four; remainders can be cross-multiplied safely even
    // when a valid single score is close to the integer representation limit.
    return (left % l) * r > (right % r) * l;
}

std::int64_t ScoreUpperBound(std::int64_t score, std::int64_t remaining,
                            std::int64_t numerator, std::size_t denominator) {
    const auto divisor = static_cast<std::int64_t>(denominator);
    if (numerator > (INT64_MAX - divisor + 1) / remaining) return INT64_MAX;
    const auto additional = (remaining * numerator + divisor - 1) / divisor;
    return additional > INT64_MAX - score ? INT64_MAX : score + additional;
}

bool BetterItem(const ScoredItem& left, const ScoredItem& right) {
    if (left.depth != right.depth) return left.depth > right.depth;
    if (left.ranking != right.ranking) return left.ranking > right.ranking;
    return left.uid < right.uid;
}

struct Search {
    std::vector<Group> groups;
    std::array<std::vector<Choice>, 25> choices;
    std::vector<Core> cores;
    std::uint32_t board{};
    std::map<std::string, int, std::less<>> shape_counts;
    std::vector<Pick> picks, best_picks;
    std::array<int, 4> depth_counts{}, best_depths{};
    std::int64_t best_score{-1};
    double best_ranking{-1};
    const Core* best_core{};
    std::size_t nodes{}, maximum_nodes{};
    Clock::time_point started, deadline;
    const AlgorithmCancelCheck& cancel;
    algorithm_detail::ProgressReporter progress;
    std::size_t sample_checks{};
    bool priorities{}, exhausted{}, first_feasible{};
    const Preferences* preferences{};
    Weights stats;
    Weights best_stats;
    const char* stop_reason{"complete"};

    Search(std::size_t budget, Clock::time_point start, Clock::time_point end,
           const AlgorithmCancelCheck& check, bool with_priorities, const AlgorithmProgressCallback& publish)
        : maximum_nodes(budget), started(start), deadline(end), cancel(check),
          progress(publish, start, std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), budget, 0, false),
          priorities(with_priorities) { progress.Sample(0, 0, true); }

    bool Stop() {
        if (cancel && cancel()) Reject(AlgorithmFailure::cancelled);
        if ((++sample_checks & 127U) == 0) progress.Sample(nodes, 0);
        if (nodes >= maximum_nodes) { exhausted = true; stop_reason = "node_budget"; }
        else if (Clock::now() >= deadline) { exhausted = true; stop_reason = "time_budget"; }
        return exhausted;
    }

    bool Better(double ranking) const {
        if (best_score < 0) return true;
        if (priorities && depth_counts != best_depths) return depth_counts > best_depths;
        return ranking > best_ranking;
    }

    bool LimitsSatisfied() const {
        for (const auto& limit : preferences->limits) {
            const auto value = Weight(stats, limit.id);
            if ((limit.has_minimum && value + 1e-12 < limit.minimum) ||
                (limit.has_maximum && value - 1e-12 > limit.maximum)) return false;
        }
        const auto crit = Weight(stats, "CritBase");
        return (!preferences->has_crit_minimum || crit + preferences->crit_minimum_base + 1e-12 >= preferences->crit_minimum) &&
            (!preferences->has_crit_maximum || crit + preferences->crit_maximum_base - 1e-12 <= preferences->crit_maximum);
    }

    int Matches(const Core& core) const {
        int result{};
        for (const auto& [shape, count] : core.shapes) {
            const auto found = shape_counts.find(shape);
            if (found != shape_counts.end()) result += std::min(count, found->second);
        }
        return result;
    }

    bool PossibleSet(const Core& core) const {
        int possible = Matches(core);
        for (const auto& [shape, count] : core.shapes) {
            const auto found = shape_counts.find(shape);
            const int placed = found == shape_counts.end() ? 0 : found->second;
            if (placed >= count) continue;
            std::size_t available{};
            for (const auto& group : groups) if (group.shape == shape) available += group.items.size() - group.used;
            possible += std::min(count - placed, static_cast<int>(available));
        }
        return possible >= core.required;
    }

    void Visit(const Core& core, std::uint32_t occupied, std::int64_t score, double ranking) {
        if (Stop()) return;
        ++nodes;
        if (occupied == board) {
            if (Matches(core) >= core.required && LimitsSatisfied() && Better(ranking)) {
                best_score = score; best_picks = picks; best_depths = depth_counts; best_core = &core;
                best_ranking = ranking; best_stats = stats;
                if (first_feasible) { exhausted = true; stop_reason = "first_feasible"; }
            }
            return;
        }
        if (!PossibleSet(core)) return;
        if (!priorities && preferences->extra_area == 0 && best_score >= 0) {
            std::int64_t numerator{};
            std::size_t denominator{1};
            for (const auto& group : groups) if (group.used < group.items.size()) {
                const auto value = group.items[group.used].score;
                if (HigherDensity(value, group.cells.size(), numerator, denominator)) {
                    numerator = value; denominator = group.cells.size();
                }
            }
            const auto remaining = static_cast<std::int64_t>(std::popcount(board & ~occupied));
            const auto upper = ScoreUpperBound(score, remaining, numerator, denominator);
            if (upper <= best_score) return;
        }
        const auto target = std::countr_zero(board & ~occupied);
        for (const auto& choice : choices[target]) {
            auto& group = groups[choice.group];
            const auto& placement = group.placements[choice.placement];
            if (group.used == group.items.size() || (placement.mask & occupied)) continue;
            const auto index = group.used++;
            const auto& item = group.items[index];
            picks.push_back({choice.group, index, choice.placement});
            ++shape_counts[group.shape];
            if (item.depth) ++depth_counts[4 - item.depth];
            if (item.score > INT64_MAX - score) Reject(AlgorithmFailure::invalid_inventory);
            for (const auto& [id, value] : item.stats) stats[id] += value;
            Visit(core, occupied | placement.mask, score + item.score, ranking + item.ranking);
            for (const auto& [id, value] : item.stats) stats[id] -= value;
            if (item.depth) --depth_counts[4 - item.depth];
            --shape_counts[group.shape];
            picks.pop_back();
            --group.used;
            if (exhausted) return;
        }
    }
};

Json Optimize(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel,
              const AlgorithmProgressCallback& progress, bool first_feasible = false) {
    const auto preferences = ReadPreferences(request);
    if (!inventory.is_object() || !inventory.value("complete", false) || !inventory.contains("snapshot_id") ||
        !inventory.at("snapshot_id").is_string() || !inventory.contains("characters") || !inventory.at("characters").is_array() ||
        !inventory.contains("items") || !inventory.at("items").is_array()) Reject(AlgorithmFailure::inventory_unavailable);
    if (!request.contains("snapshot_id") || !request.at("snapshot_id").is_string()) Reject(AlgorithmFailure::invalid_request);
    if (request.at("snapshot_id") != inventory.at("snapshot_id")) Reject(AlgorithmFailure::stale_snapshot);
    const bool software = (!request.contains("character") || request.at("character").is_null()) && request.contains("role_id");
    Json software_character;
    std::uint64_t character{};
    const Json* target{};
    if (software) {
        static_cast<void>(Id(request.at("role_id"), AlgorithmFailure::invalid_request));
        if (!request.contains("allowed_cells") ||
            !request.at("allowed_cells").is_array() || request.at("allowed_cells").size() != 20)
            Reject(AlgorithmFailure::invalid_request);
        software_character = {{"uid", nullptr}, {"allowed_cells", request.at("allowed_cells")}};
        target = &software_character;
    } else {
        if (!request.contains("character")) Reject(AlgorithmFailure::invalid_request);
        character = UidKey(request.at("character"), AlgorithmFailure::invalid_request);
        for (const auto& entry : inventory.at("characters"))
            if (UidKey(entry.at("uid"), AlgorithmFailure::invalid_inventory) == character) { target = &entry; break; }
    }
    if (!target) Reject(AlgorithmFailure::missing_character);
    if (!target->contains("allowed_cells") || !target->at("allowed_cells").is_array() || target->at("allowed_cells").empty())
        Reject(AlgorithmFailure::invalid_inventory);
    const auto started = Clock::now();
    const auto maximum_nodes = request.contains("max_nodes") ? Integer(request.at("max_nodes"), 1, algorithm_limits::kMaximumNodes) : algorithm_limits::kDefaultNodes;
    const auto milliseconds = request.contains("time_limit_ms") ? Integer(request.at("time_limit_ms"), 1, algorithm_limits::kMaximumTimeLimitMs) : algorithm_limits::kDefaultTimeLimitMs;
    Search search(static_cast<std::size_t>(maximum_nodes), started,
                  started + std::chrono::milliseconds(milliseconds), cancel, !preferences.priority.empty(), progress);
    search.preferences = &preferences;
    search.first_feasible = first_feasible;
    for (const auto& cell : target->at("allowed_cells")) {
        if (!cell.is_array() || cell.size() != 2) Reject(AlgorithmFailure::invalid_inventory);
        const auto row = Integer(cell[0], 1, 5, AlgorithmFailure::invalid_inventory);
        const auto column = Integer(cell[1], 1, 5, AlgorithmFailure::invalid_inventory);
        const auto bit = 1U << ((row - 1) * 5 + column - 1);
        if (search.board & bit) Reject(AlgorithmFailure::invalid_inventory);
        search.board |= bit;
    }
    const bool require_core = Flag(request, "require_core", true);
    const bool borrow = Flag(request, "allow_borrow", true);
    const bool discarded = Flag(request, "include_discarded", false);
    const auto set_mode = request.value("set_effect_mode", std::string(require_core ? "four_piece" : "none"));
    if (set_mode != (require_core ? "four_piece" : "none")) Reject(AlgorithmFailure::invalid_request);
    const auto core_suit = request.contains("core_suit_id") ? Id(request.at("core_suit_id"), AlgorithmFailure::invalid_request) : std::string{};
    const auto mains = Ids(request, "core_main_stats");
    std::set<std::uint64_t> excluded;
    if (const auto entries = request.find("exclude_uids"); entries != request.end()) {
        if (!entries->is_array()) Reject(AlgorithmFailure::invalid_request);
        for (const auto& entry : *entries) excluded.insert(UidKey(entry, AlgorithmFailure::invalid_request));
    }
    std::map<std::uint64_t, Placement> kept;
    std::uint64_t kept_core{};
    if (const auto keep = request.find("keep"); keep != request.end()) {
        if (!keep->is_object()) Reject(AlgorithmFailure::invalid_request);
        if (keep->contains("core") && !keep->at("core").is_null()) {
            kept_core = UidKey(keep->at("core"), AlgorithmFailure::invalid_request);
            if (!require_core || excluded.contains(kept_core)) Reject(AlgorithmFailure::invalid_request);
        }
        if (keep->contains("placements")) {
            if (!keep->at("placements").is_array()) Reject(AlgorithmFailure::invalid_request);
            for (const auto& place : keep->at("placements")) {
                const auto uid = UidKey(place.at("equipment"), AlgorithmFailure::invalid_request);
                if (excluded.contains(uid) || !kept.emplace(uid, Placement{
                    Integer(place.at("row"), 1, 5), Integer(place.at("column"), 1, 5)}).second)
                    Reject(AlgorithmFailure::invalid_request);
            }
        }
    }
    std::uint32_t fixed_occupied{};
    std::int64_t fixed_score{};
    double fixed_ranking{};
    Weights fixed_stats;
    std::array<int, 4> fixed_depths{};
    std::size_t fixed_count{};
    std::set<std::uint64_t> all_uids;
    std::size_t candidate_modules{}, candidate_cores{};
    for (const auto& item : inventory.at("items")) {
        if (search.Stop()) break;
        if (!item.is_object() || !item.contains("uid")) Reject(AlgorithmFailure::invalid_inventory);
        const auto uid = UidKey(item.at("uid"), AlgorithmFailure::invalid_inventory);
        if (!all_uids.insert(uid).second) Reject(AlgorithmFailure::invalid_inventory);
        if (excluded.contains(uid) || (!discarded && item.value("discarded", false))) continue;
        if (!borrow && item.contains("character") && !item.at("character").is_null() &&
            (software || UidKey(item.at("character"), AlgorithmFailure::invalid_inventory) != character)) continue;
        const bool core = item.value("kind", std::string{}) == "core";
        if (core && kept.contains(uid)) Reject(AlgorithmFailure::invalid_request);
        if (core && !require_core) continue;
        if (core && kept_core && uid != kept_core) continue;
        if (!kept.contains(uid) && uid != kept_core && !Eligible(item, preferences)) continue;
        if (core && !core_suit.empty() && item.value("suit_id", std::string{}) != core_suit) continue;
        if (core && !mains.empty() && item.contains("main_stat") && item.at("main_stat").is_object()) {
            const auto main_id = item.at("main_stat").value("id", std::string{});
            if (std::find(mains.begin(), mains.end(), main_id) == mains.end()) continue;
        }
        {
            auto scored = Score(item, preferences);
            scored.uid = uid;
            if (core) {
                Core candidate{std::move(scored)};
                const auto shapes = item.find("required_shapes");
                if (shapes == item.end() || !item.value("required_shapes_complete", true))
                    Reject(AlgorithmFailure::incomplete_set);
                candidate.shapes = FourPieceShapes(*shapes);
                candidate.required = 4;
                search.cores.push_back(std::move(candidate)); ++candidate_cores;
            } else {
                if (const auto fixed = kept.find(uid); fixed != kept.end()) {
                    auto placement = fixed->second;
                    for (const auto& cell : scored.cells) {
                        const auto row = placement.row + cell[0], column = placement.column + cell[1];
                        if (row < 1 || row > 5 || column < 1 || column > 5) Reject(AlgorithmFailure::invalid_request);
                        placement.mask |= 1U << ((row - 1) * 5 + column - 1);
                    }
                    if ((placement.mask & search.board) != placement.mask || (placement.mask & fixed_occupied))
                        Reject(AlgorithmFailure::invalid_request);
                    fixed_occupied |= placement.mask;
                    if (scored.score > INT64_MAX - fixed_score) Reject(AlgorithmFailure::invalid_inventory);
                    fixed_score += scored.score; fixed_ranking += scored.ranking;
                    for (const auto& [id, value] : scored.stats) fixed_stats[id] += value;
                    if (scored.depth) ++fixed_depths[4 - scored.depth];
                    ++search.shape_counts[scored.shape];
                    search.picks.push_back({search.groups.size(), 0, 0});
                    Group group{scored.shape, scored.cells, scored.depth};
                    group.items.push_back(std::move(scored)); group.placements.push_back(placement); group.used = 1;
                    search.groups.push_back(std::move(group)); ++fixed_count; ++candidate_modules;
                    continue;
                }
                auto found = std::find_if(search.groups.begin(), search.groups.end(), [&](const Group& group) {
                    return group.used == 0 && group.shape == scored.shape && group.cells == scored.cells &&
                        group.depth == scored.depth && group.stats == scored.stats;
                });
                if (found == search.groups.end()) {
                    search.groups.push_back({scored.shape, scored.cells, scored.depth});
                    found = search.groups.end() - 1;
                    found->stats = scored.stats;
                }
                found->items.push_back(std::move(scored)); ++candidate_modules;
            }
        }
    }
    if (!search.exhausted && fixed_count != kept.size()) Reject(AlgorithmFailure::invalid_request);
    if (!require_core) search.cores.emplace_back();
    bool missing_core{};
    if (require_core && search.cores.empty() && Flag(request, "allow_missing_core", false) && !kept_core) {
        Core fallback;
        if (!request.contains("required_shapes")) Reject(AlgorithmFailure::incomplete_set);
        fallback.shapes = FourPieceShapes(request.at("required_shapes"));
        fallback.required = 4;
        search.cores.push_back(std::move(fallback)); missing_core = true;
    }
    for (auto& group : search.groups) {
        if (search.Stop()) break;
        if (group.used) continue;
        std::sort(group.items.begin(), group.items.end(), [&](const ScoredItem& left, const ScoredItem& right) {
            if (cancel && cancel()) Reject(AlgorithmFailure::cancelled);
            return BetterItem(left, right);
        });
        for (int row = 1; row <= 5; ++row) for (int column = 1; column <= 5; ++column) {
            std::uint32_t mask{};
            for (const auto& cell : group.cells) {
                const auto r = row + cell[0], c = column + cell[1];
                if (r < 1 || r > 5 || c < 1 || c > 5) { mask = 0; break; }
                mask |= 1U << ((r - 1) * 5 + c - 1);
            }
            if (mask && (mask & search.board) == mask) group.placements.push_back({row, column, mask});
        }
    }
    for (std::size_t g{}; g < search.groups.size(); ++g)
        for (std::size_t p{}; p < search.groups[g].placements.size(); ++p)
            for (std::size_t bit{}; bit < search.choices.size(); ++bit)
                if (search.groups[g].placements[p].mask & (1U << bit)) search.choices[bit].push_back({g, p});
    for (auto& choices : search.choices) std::stable_sort(choices.begin(), choices.end(), [&](const Choice& left, const Choice& right) {
        if (cancel && cancel()) Reject(AlgorithmFailure::cancelled);
        const auto& l = search.groups[left.group]; const auto& r = search.groups[right.group];
        if (l.depth != r.depth) return l.depth > r.depth;
        if (preferences.extra_area) {
            const auto a = l.items.front().ranking / static_cast<double>(l.cells.size());
            const auto b = r.items.front().ranking / static_cast<double>(r.cells.size());
            if (a != b) return a > b;
        }
        if (HigherDensity(l.items.front().score, l.cells.size(), r.items.front().score, r.cells.size())) return true;
        if (HigherDensity(r.items.front().score, r.cells.size(), l.items.front().score, l.cells.size())) return false;
        return left.group < right.group;
    });
    std::stable_sort(search.cores.begin(), search.cores.end(), [&](const Core& left, const Core& right) {
        if (cancel && cancel()) Reject(AlgorithmFailure::cancelled);
        return BetterItem(left.item, right.item);
    });
    // With identical placement constraints, a lower-ranked core cannot improve
    // any plan. Keep one; this does not discard a distinct module solution.
    std::vector<Core> distinct;
    for (auto& core : search.cores) {
        if (search.Stop()) break;
        if (std::none_of(distinct.begin(), distinct.end(), [&](const Core& existing) {
            if (cancel && cancel()) Reject(AlgorithmFailure::cancelled);
            return existing.required == core.required && existing.shapes == core.shapes && existing.item.stats == core.item.stats;
        })) distinct.push_back(std::move(core));
    }
    search.cores = std::move(distinct);
    search.progress.value.phase = AlgorithmProgressPhase::searching;
    search.progress.Sample(search.nodes, 0, true);
    for (const auto& core : search.cores) {
        if (search.Stop()) break;
        search.depth_counts = fixed_depths;
        if (core.item.depth) ++search.depth_counts[4 - core.item.depth];
        search.stats = fixed_stats;
        for (const auto& [id, value] : core.item.stats) search.stats[id] += value;
        if (core.item.score > INT64_MAX - fixed_score) Reject(AlgorithmFailure::invalid_inventory);
        search.Visit(core, fixed_occupied, core.item.score + fixed_score, core.item.ranking + fixed_ranking);
    }
    if (cancel && cancel()) Reject(AlgorithmFailure::cancelled);
    search.progress.Sample(search.nodes, 0, true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    Json result{{"snapshot_id", inventory.at("snapshot_id")}, {"character", target->at("uid")},
        {"native_assemblable", !software},
        {"valid", search.best_score >= 0}, {"core", nullptr}, {"placements", Json::array()},
        {"assignment_scores", Json::array()}, {"score", nullptr}, {"grade", nullptr},
        {"search", {{"exhaustive", !search.exhausted}, {"stop_reason", search.stop_reason},
            {"nodes", search.nodes}, {"elapsed_ms", elapsed}, {"candidate_modules", candidate_modules},
            {"candidate_cores", candidate_cores}}}};
    if (request.contains("role_id")) result["role_id"] = request.at("role_id");
    if (search.best_score < 0) {
        result["reason"] = search.exhausted ? "budget_exhausted" : "no_solution";
        return result;
    }
    const auto add_score = [&](const ScoredItem& item) {
        auto score = ScoreJson(item);
        score["equipment"] = item.source->at("uid");
        result["assignment_scores"].push_back(std::move(score));
    };
    if (search.best_core->item.source) {
        result["core"] = search.best_core->item.source->at("uid");
        add_score(search.best_core->item);
    }
    for (const auto& pick : search.best_picks) {
        const auto& group = search.groups[pick.group];
        const auto& item = group.items[pick.item];
        const auto& place = group.placements[pick.placement];
        result["placements"].push_back({{"equipment", item.source->at("uid")}, {"row", place.row}, {"column", place.column}});
        add_score(item);
    }
    result["score"] = static_cast<double>(search.best_score) / 100;
    result["ranking_score"] = search.best_ranking;
    result["priority_counts"] = search.best_depths;
    result["constraint_stats"] = search.best_stats;
    result["grade"] = TotalGrade(static_cast<double>(search.best_score) / 100);
    result["set_effect_mode"] = set_mode;
    result["equipment_complete"] = !require_core || search.best_core->item.source != nullptr;
    if (missing_core) result["missing_core_reason"] = "no_matching_core";
    return result;
}
} // namespace

std::string_view AlgorithmErrorCode(AlgorithmFailure failure) noexcept {
    switch (failure) {
    case AlgorithmFailure::invalid_request: return "algorithm.invalid_request";
    case AlgorithmFailure::inventory_unavailable: return "algorithm.inventory_unavailable";
    case AlgorithmFailure::stale_snapshot: return "algorithm.stale_snapshot";
    case AlgorithmFailure::missing_character: return "algorithm.missing_character";
    case AlgorithmFailure::invalid_inventory: return "algorithm.invalid_inventory";
    case AlgorithmFailure::incomplete_attributes: return "algorithm.incomplete_attributes";
    case AlgorithmFailure::incomplete_main_stat: return "algorithm.incomplete_main_stat";
    case AlgorithmFailure::incomplete_shape: return "algorithm.incomplete_shape";
    case AlgorithmFailure::incomplete_set: return "algorithm.incomplete_set";
    case AlgorithmFailure::unsupported_quality: return "algorithm.unsupported_quality";
    case AlgorithmFailure::cancelled: return "algorithm.cancelled";
    case AlgorithmFailure::budget_exhausted: return "algorithm.budget_exhausted";
    }
    return "algorithm.invalid_request";
}

Json ScoreEquipment(const Json& item, const Json& preferences) {
    try {
        const auto rules = ReadPreferences(preferences);
        if (!Eligible(item, rules)) return {{"eligible", false}};
        return ScoreJson(Score(item, rules));
    }
    catch (const Json::exception&) { Reject(AlgorithmFailure::invalid_inventory); }
}

const char* GradeLoadout(double score) {
    if (!std::isfinite(score) || score < 0) Reject(AlgorithmFailure::invalid_request);
    return TotalGrade(score);
}

Json OptimizeLoadout(const Json& request, const Json& frozen_inventory, const AlgorithmCancelCheck& cancel_check,
                     const AlgorithmProgressCallback& progress) {
    try { return Optimize(request, frozen_inventory, cancel_check, progress); }
    catch (const Json::exception&) { Reject(AlgorithmFailure::invalid_request); }
}

namespace algorithm_detail {
Json OptimizeFirstLoadout(const Json& request, const Json& inventory, const AlgorithmCancelCheck& cancel,
                          const AlgorithmProgressCallback& progress) {
    try { return Optimize(request, inventory, cancel, progress, true); }
    catch (const Json::exception&) { Reject(AlgorithmFailure::invalid_request); }
}
} // namespace algorithm_detail

} // namespace equipment_manager
