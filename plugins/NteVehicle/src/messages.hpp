#pragma once
#include <anomaly/sdk/localization.hpp>
#include <map>
#include <string>
#include <vector>

namespace nte_vehicle {
// The UI creates the sole Localizer in Load. The backend borrows it for the
// same plugin generation; cached literals keep status message pointers valid.
inline const anomaly::sdk::Localizer* backend_localizer{};
inline std::map<std::string, std::string, std::less<>> backend_messages;

inline const char* TextLocalized(std::string_view key, std::string_view fallback) {
    auto found = backend_messages.find(key);
    if (found == backend_messages.end())
        found = backend_messages.emplace(key, backend_localizer->Text(key, fallback)).first;
    return found->second.c_str();
}
inline std::string LocalizedStatus(std::string_view key, std::string_view fallback,
                                   std::initializer_list<std::string> arguments) {
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& value : arguments) views.emplace_back(value);
    return backend_localizer->Format(key, fallback, views);
}
} // namespace nte_vehicle
