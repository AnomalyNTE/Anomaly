#pragma once
#include <anomaly/sdk/cpp.hpp>
#include <initializer_list>
#include <iostream>
#include <string_view>

namespace compatibility_test {
template<class Check> bool Run(Check check, std::initializer_list<const char*> required) {
    struct Probe { std::string_view blocked; } probe;
    AnomalyUe5BuildServiceV1 build{sizeof(build), 1, &probe};
    build.feature_state = [](void* user, AnomalyStringViewV1 key) -> uint32_t {
        return std::string_view(key.data, key.size) == static_cast<Probe*>(user)->blocked ?
            ANOMALY_FEATURE_V1_UNAVAILABLE : ANOMALY_FEATURE_V1_AVAILABLE;
    };
    if (!check(&build).empty() || check(nullptr).empty()) return false;
    for (const auto feature : required) {
        probe.blocked = feature;
        if (check(&build) != feature) { std::cerr << "Required taxi feature was not identified\n"; return false; }
    }
    probe.blocked = {};
    build.struct_size = offsetof(AnomalyUe5BuildServiceV1, feature_state);
    if (check(&build).empty()) return false;
    build.struct_size = sizeof(build) + 64;
    if (!check(&build).empty()) return false;
    build.feature_state = nullptr;
    return !check(&build).empty();
}
}
