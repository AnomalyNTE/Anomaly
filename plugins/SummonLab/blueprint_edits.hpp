#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace summon_lab {

enum class BlueprintFieldKind { Boolean, Integer, Float, Double };

inline std::string_view FieldKindName(BlueprintFieldKind kind) {
    switch (kind) {
    case BlueprintFieldKind::Boolean: return "bool";
    case BlueprintFieldKind::Integer: return "int";
    case BlueprintFieldKind::Float: return "float";
    case BlueprintFieldKind::Double: return "double";
    }
    return {};
}

inline bool ValidFieldValue(BlueprintFieldKind kind, double value) {
    if (!std::isfinite(value)) return false;
    switch (kind) {
    case BlueprintFieldKind::Boolean: return value == 0 || value == 1;
    case BlueprintFieldKind::Integer:
        return std::trunc(value) == value && value >= std::numeric_limits<std::int32_t>::min() &&
            value <= std::numeric_limits<std::int32_t>::max();
    case BlueprintFieldKind::Float: return std::abs(value) <= std::numeric_limits<float>::max();
    case BlueprintFieldKind::Double: return true;
    }
    return false;
}

// Saved identities are names, never addresses, FNames, offsets or ability handles.
struct BlueprintFieldEdit {
    std::string owner, name;
    BlueprintFieldKind kind{};
    double value{};
};

struct BlueprintEdits {
    std::vector<BlueprintFieldEdit> fields;
    std::vector<std::string> disabled_skills, removed_effects;
    bool empty() const { return fields.empty() && disabled_skills.empty() && removed_effects.empty(); }
};

} // namespace summon_lab
