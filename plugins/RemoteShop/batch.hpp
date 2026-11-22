#pragma once
#include "purchase.hpp"
#include <string>
#include <vector>
namespace shop {
struct BatchLine { std::string shop,goods; uint32_t quantity{1}; };
struct Batch {
    std::vector<BatchLine> lines;
    size_t completed{};
    bool active{},inflight{},cancel{};
    void Start(std::vector<BatchLine> value) { lines = std::move(value); completed = 0; active = !lines.empty(); inflight = cancel = false; }
    bool Ready() const { return active && !inflight && !cancel && completed < lines.size(); }
    void Observe(Outcome outcome) {
        if (!active) return;
        if (!inflight) { if(cancel) active = false; return; }
        if (outcome == Outcome::Waiting) return;
        inflight = false;
        if (outcome == Outcome::Confirmed) ++completed;
        if (outcome != Outcome::Confirmed || cancel || completed == lines.size()) active = false;
    }
};
inline std::optional<int64_t> RemainingStock(int64_t limit, std::optional<int64_t> bought, bool variable_limit) {
    if (limit <= 0 || !bought || *bought < 0 || variable_limit) return {};
    return *bought >= limit ? 0 : limit - *bought;
}
}
