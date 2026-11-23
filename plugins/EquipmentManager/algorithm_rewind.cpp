#include "algorithm_support.hpp"
#include <array>
#include <map>
#include <numeric>
#include <vector>

namespace equipment_manager {
namespace {
using namespace algorithm_detail;
struct Shape { std::string id; int area{}, owned{}, demand{}, quantity{}; double gap{}, priority{}; };
double Grade(std::string_view grade) {
    constexpr std::array<std::string_view, 8> names{"D", "C", "B", "A", "S", "SS", "SSS", "ACE"};
    constexpr std::array ratios{0., .2, .3, .4, .5, .6, .7, .8};
    const auto found = std::find(names.begin(), names.end(), grade);
    if (found == names.end()) Fail();
    return ratios[static_cast<std::size_t>(found - names.begin())];
}
Json Rewind(const Json& request) {
    if (!request.is_object() || !request.contains("shapes") || !request.at("shapes").is_array()) Fail();
    std::map<std::string, Shape, std::less<>> shapes;
    for (const auto& entry : request.at("shapes")) {
        Shape shape; shape.id = Id(entry.at("shape_id")); shape.area = Integer(entry.at("area"), 1, 4);
        if (request.contains("owned_counts") && request.at("owned_counts").contains(shape.id))
            shape.owned = Integer(request.at("owned_counts").at(shape.id), 0, INT32_MAX);
        if (!shapes.emplace(shape.id, shape).second) Fail();
    }
    if (shapes.empty()) Fail();
    const auto mode = request.value("mode", request.contains("deficits") ? std::string("score_shortfall") : std::string("demand"));
    const auto strategy = request.value("strategy", std::string("balanced"));
    if (strategy != "balanced" && strategy != "focus") Fail();
    const auto focused = strategy == "focus";
    const int slots = request.contains("selection_limit") ? Integer(request.at("selection_limit"), 1, 8) : 8;
    const auto find = [&](const Json& id) -> Shape& {
        const auto found = shapes.find(Id(id)); if (found == shapes.end()) Fail(); return found->second;
    };
    if (request.contains("requirements")) {
        if (!request.at("requirements").is_array()) Fail();
        for (const auto& value : request.at("requirements")) ++find(value).demand;
    }
    std::string reason;
    if (mode == "score_shortfall") {
        if (!request.contains("deficits") || !request.at("deficits").is_array()) Fail();
        for (const auto& row : request.at("deficits")) {
            if (focused && !row.value("main", false)) continue;
            auto& shape = find(row.at("shape_id"));
            const auto area = row.contains("area") ? Integer(row.at("area"), 1, 4) : shape.area;
            double ratio = Grade(row.value("target_grade", request.value("target_grade", std::string("S"))));
            if (row.contains("target_percent") || request.contains("target_percent")) {
                ratio = Number(row.contains("target_percent") ? row.at("target_percent") : request.at("target_percent"));
                if (ratio < .01 || ratio > 1) Fail();
            }
            const auto score = Number(row.at("score")); if (score < 0) Fail();
            const auto gap = std::max(0., ratio * area * 10 - score);
            if (gap > 0) { ++shape.demand; shape.gap += gap; }
        }
        std::vector<Shape*> selected;
        for (auto& [id, shape] : shapes) if (shape.gap > 0) {
            shape.quantity = 1; shape.priority = focused ? shape.gap : shape.gap / std::max(1, shape.owned);
            selected.push_back(&shape);
        }
        if (selected.empty()) reason = "no_shortfall";
        else if (selected.size() > static_cast<std::size_t>(slots)) reason = "too_many_shapes";
        else {
            const auto remaining = slots - static_cast<int>(selected.size());
            const auto total = std::accumulate(selected.begin(), selected.end(), 0., [](double sum, const Shape* shape) { return sum + shape->priority; });
            std::map<std::string, double, std::less<>> remainders;
            int assigned{};
            for (auto* shape : selected) {
                const auto quota = remaining * shape->priority / total;
                const auto count = static_cast<int>(quota); shape->quantity += count; assigned += count;
                remainders.emplace(shape->id, quota - count);
            }
            std::sort(selected.begin(), selected.end(), [&](const Shape* a, const Shape* b) {
                if (remainders.at(a->id) != remainders.at(b->id)) return remainders.at(a->id) > remainders.at(b->id);
                if (a->priority != b->priority) return a->priority > b->priority;
                return a->id < b->id;
            });
            for (int i{}; i < remaining - assigned; ++i) ++selected[static_cast<std::size_t>(i)]->quantity;
        }
    } else if (mode == "demand") {
        for (int i{}; i < slots; ++i) {
            Shape* winner{}; double best{-1};
            for (auto& [id, shape] : shapes) {
                const auto value = static_cast<double>(shape.demand) / (shape.quantity + 1 + (focused ? 0 : shape.owned));
                if (!winner || value > best || (value == best && (shape.demand > winner->demand ||
                    (shape.demand == winner->demand && shape.id > winner->id)))) { winner = &shape; best = value; }
            }
            if (!winner->quantity) winner->priority = best;
            ++winner->quantity;
        }
    } else if (mode == "types") {
        std::vector<Shape*> ordered;
        for (auto& [id, shape] : shapes) { shape.priority = static_cast<double>(shape.demand) / (shape.owned + 1.); ordered.push_back(&shape); }
        std::sort(ordered.begin(), ordered.end(), [](const Shape* a, const Shape* b) {
            if (a->priority != b->priority) return a->priority > b->priority;
            if (a->demand != b->demand) return a->demand > b->demand;
            if (a->owned != b->owned) return a->owned < b->owned;
            return a->id < b->id;
        });
        for (std::size_t i{}; i < std::min<std::size_t>(ordered.size(), slots); ++i) ordered[i]->quantity = 1;
    } else Fail();
    Json rows = Json::array(); double cost{}; int selected_slots{};
    if (reason.empty()) for (const auto& [id, shape] : shapes) if (shape.quantity) {
        const auto price = 10. + 5. * (shape.quantity - 1);
        rows.push_back({{"shape_id", id}, {"area", shape.area}, {"quantity", shape.quantity}, {"owned_count", shape.owned},
            {"suit_demand", shape.demand}, {"quality_gap", shape.gap}, {"priority_score", shape.priority},
            {"probability", shape.quantity / 8.}, {"unit_cost", price}, {"cost", price * shape.quantity}});
        cost += price * shape.quantity; selected_slots += shape.quantity;
    }
    std::sort(rows.begin(), rows.end(), [&](const Json& a, const Json& b) {
        if (mode == "score_shortfall" && a.at("quality_gap") != b.at("quality_gap")) return a.at("quality_gap") > b.at("quality_gap");
        if (a.at("priority_score") != b.at("priority_score")) return a.at("priority_score") > b.at("priority_score");
        return a.at("shape_id") < b.at("shape_id");
    });
    return {{"valid", reason.empty()}, {"reason", reason}, {"recommendations", rows}, {"slots", selected_slots},
        {"selection_limit", slots}, {"total_cost", cost}, {"mode", mode}, {"strategy", strategy}};
}
} // namespace
Json RecommendRewind(const Json& request) {
    try { return Rewind(request); } catch (const Json::exception&) { Fail(); }
}
} // namespace equipment_manager
