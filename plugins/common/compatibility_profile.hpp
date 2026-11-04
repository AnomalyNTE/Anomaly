#pragma once

#include <anomaly/sdk/profile.hpp>
#include <limits>
#include <algorithm>
#include <initializer_list>

namespace anomaly::plugins {

// One immutable compatibility snapshot per DLL generation. Load is called on
// Lifecycle before publishing callbacks; Game/Render only read cached values.
class CompatibilityProfile final {
public:
    bool Load(const AnomalyHostApiV1* api, std::string_view filename,
              std::string_view contract,
              std::initializer_list<std::string_view> known_entries = {},
              bool batch_signatures = false) noexcept {
        try {
            const sdk::Host host(api);
            auto loaded = sdk::LoadProfile(host, filename);
            if (!loaded) return false;
            auto parsed = std::move(loaded).value();
            if (parsed.game != "nte" || parsed.module != "HTGame.exe" ||
                parsed.section != ".text" || parsed.contract != contract) return false;
            const auto core = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
            const auto signatures = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
            const auto batch_service = batch_signatures
                ? host.Query<AnomalySignatureServiceV3>(
                    ANOMALY_SIGNATURE_SERVICE_V3_ID, ANOMALY_SIGNATURE_SERVICE_V3_VERSION)
                : sdk::ServiceRef<AnomalySignatureServiceV3>{};
            std::map<std::string_view, std::size_t, std::less<>> pattern_indices;
            std::vector<AnomalyStringViewV1> patterns;
            if (batch_service) {
                for (const auto& symbol : parsed.symbols) {
                    if (std::find(known_entries.begin(), known_entries.end(), symbol.name) != known_entries.end()) continue;
                    for (const auto& candidate : symbol.candidates) {
                        if (pattern_indices.emplace(candidate.pattern, patterns.size()).second)
                            patterns.push_back(sdk::StringView(candidate.pattern));
                    }
                }
            }
            std::vector<std::uint32_t> status_codes(patterns.size());
            std::vector<std::uintptr_t> batch_addresses(patterns.size());
            const bool batch = batch_service && batch_service->struct_size >= sizeof(AnomalySignatureServiceV3) &&
                batch_service->resolve_batch && patterns.size() >= 16 &&
                patterns.size() <= ANOMALY_SIGNATURE_BATCH_MAX_PATTERNS_V3 &&
                sdk::Succeeded(batch_service->resolve_batch(batch_service->user,
                    sdk::StringView(parsed.module), sdk::StringView(parsed.section),
                    patterns.data(), static_cast<std::uint32_t>(patterns.size()),
                    status_codes.data(), batch_addresses.data()));
            std::map<std::string, std::uintptr_t, std::less<>> addresses;
            std::map<std::string, std::size_t, std::less<>> selections;
            for (const auto& symbol : parsed.symbols) {
                if (std::find(known_entries.begin(), known_entries.end(), symbol.name) != known_entries.end())
                    continue; // Validate at an object/vtable-owned address, never globally scan a short body.
                const auto resolved = batch
                    ? sdk::ResolveProfileSymbolWith(core.get(), parsed, symbol.name,
                        [&](const sdk::ProfileCandidate& candidate, std::uintptr_t* address) {
                            const auto index = pattern_indices.at(candidate.pattern);
                            *address = batch_addresses[index];
                            return AnomalyStatusV1{status_codes[index], 0, {nullptr, 0}};
                        })
                    : sdk::ResolveProfileSymbol(signatures.get(), core.get(), parsed, symbol.name);
                if (symbol.name == "layout" && !resolved) return false;
                addresses.emplace(symbol.name, resolved.address);
                if (resolved) selections.emplace(symbol.name,
                    static_cast<std::size_t>(resolved.candidate - symbol.candidates.data()));
                if (resolved && symbol.name == "layout")
                    for (const auto& [key, value] : resolved.candidate->values)
                        parsed.layout.insert_or_assign(key, value);
            }
            profile_ = std::move(parsed);
            addresses_ = std::move(addresses);
            selections_ = std::move(selections);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::uintptr_t Address(std::string_view symbol) const noexcept {
        const auto found = addresses_.find(symbol);
        return found == addresses_.end() ? 0 : found->second;
    }

    [[nodiscard]] const sdk::ProfileCandidate* Selected(std::string_view symbol) const {
        const auto found = selections_.find(symbol);
        return found == selections_.end() ? nullptr : &profile_.symbol(symbol).candidates.at(found->second);
    }

    [[nodiscard]] std::string_view Pattern(std::string_view symbol) const {
        const auto* candidate = Selected(symbol);
        if (candidate) return candidate->pattern;
        // A member-learning witness must retain its declared seed even when the
        // address-only resolver rejected changed field displacements. This does
        // not publish an address or a layout: ResolveLayout validates both.
        const auto& candidates = profile_.symbol(symbol).candidates;
        return candidates.size() == 1 ? std::string_view(candidates.front().pattern) : std::string_view{};
    }

    [[nodiscard]] bool MatchesEntry(const AnomalyCoreServiceV1* core, std::string_view symbol,
                                    std::uintptr_t address, bool observer_detour = false) const noexcept {
        try {
            if (!core || core->struct_size < offsetof(AnomalyCoreServiceV1, read_memory) + sizeof(core->read_memory) ||
                core->service_version < ANOMALY_CORE_SERVICE_V1_VERSION || !core->read_memory || !address) return false;
            for (const auto& candidate : profile_.symbol(symbol).candidates) {
                std::vector<std::uint8_t> actual(candidate.entry.size());
                if (actual.empty() || !sdk::Succeeded(core->read_memory(core->user, address,
                        {actual.data(), actual.size()}))) continue;
                if (actual == candidate.entry) return true;
                // The host's ProcessEvent observer installs a five-byte E9 jump.
                if (observer_detour && actual.size() > 5 && actual[0] == 0xE9 &&
                    std::equal(actual.begin() + 5, actual.end(), candidate.entry.begin() + 5)) return true;
            }
        } catch (...) {}
        return false;
    }

    // The caller supplies the semantic range/alignment for each named field.
    [[nodiscard]] std::uint32_t Value(std::string_view key, std::uint32_t maximum,
                                      std::uint32_t alignment = 1) const {
        const auto found = profile_.layout.find(key);
        if (found == profile_.layout.end() || found->second > maximum ||
            alignment == 0 || found->second % alignment != 0)
            throw std::runtime_error("unsupported compatibility layout field: " + std::string(key));
        return found->second;
    }

private:
    sdk::Profile profile_;
    std::map<std::string, std::uintptr_t, std::less<>> addresses_;
    std::map<std::string, std::size_t, std::less<>> selections_;
};

} // namespace anomaly::plugins
