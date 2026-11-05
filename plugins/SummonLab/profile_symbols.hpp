#pragma once

#include <anomaly/sdk/profile.hpp>
#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace summon_lab {

// Per-start scan results. Entry bytes, E8 targets and unique candidates are
// still checked by the SDK resolver when each dependent feature starts.
class ProfileScans {
public:
    void Prepare(const anomaly::sdk::Host& host, const anomaly::sdk::Profile& profile,
                 std::span<const std::string_view> symbols) {
        results_.clear();
        batch_ = false;
        for (const auto& symbol : profile.symbols) {
            if (std::find(symbols.begin(), symbols.end(), symbol.name) == symbols.end()) continue;
            for (const auto& candidate : symbol.candidates)
                results_.try_emplace(candidate.pattern);
        }
        const auto v3 = host.Query<AnomalySignatureServiceV3>(ANOMALY_SIGNATURE_SERVICE_V3_ID,
            ANOMALY_SIGNATURE_SERVICE_V3_VERSION);
        if (v3 && v3->struct_size >= offsetof(AnomalySignatureServiceV3, resolve_batch) +
                sizeof(v3->resolve_batch) && v3->resolve_batch) {
            batch_ = true;
            std::vector<AnomalyStringViewV1> patterns;
            std::vector<Result*> outputs;
            for (auto& [pattern, result] : results_) {
                patterns.push_back(anomaly::sdk::StringView(pattern));
                outputs.push_back(&result);
            }
            for (std::size_t begin = 0; begin < patterns.size(); begin += ANOMALY_SIGNATURE_BATCH_MAX_PATTERNS_V3) {
                const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(
                    patterns.size() - begin, ANOMALY_SIGNATURE_BATCH_MAX_PATTERNS_V3));
                std::vector<std::uint32_t> statuses(count, ANOMALY_STATUS_V1_UNAVAILABLE);
                std::vector<std::uintptr_t> addresses(count);
                const auto status = v3->resolve_batch(v3->user, anomaly::sdk::StringView(profile.module),
                    anomaly::sdk::StringView(profile.section), patterns.data() + begin, count,
                    statuses.data(), addresses.data());
                for (std::size_t i = 0; i < count; ++i)
                    *outputs[begin + i] = status.code == ANOMALY_STATUS_V1_OK
                        ? Result{statuses[i], statuses[i] == ANOMALY_STATUS_V1_OK ? addresses[i] : 0}
                        : Result{status.code, 0};
            }
            return;
        }
        const auto v1 = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        if (!v1 || v1->struct_size < sizeof(AnomalySignatureServiceV1) || !v1->resolve) return;
        for (auto& [pattern, result] : results_) {
            result.status = v1->resolve(v1->user, anomaly::sdk::StringView(profile.module),
                anomaly::sdk::StringView(profile.section), anomaly::sdk::StringView(pattern), &result.address).code;
            if (result.status != ANOMALY_STATUS_V1_OK) result.address = 0;
        }
    }

    anomaly::sdk::ProfileSymbolResult Resolve(const AnomalyCoreServiceV1* core,
            const anomaly::sdk::Profile& profile, std::string_view name) const {
        return anomaly::sdk::ResolveProfileSymbolWith(core, profile, name,
            [&](const anomaly::sdk::ProfileCandidate& candidate, std::uintptr_t* address) -> AnomalyStatusV1 {
                const auto found = results_.find(candidate.pattern);
                if (found == results_.end()) return {ANOMALY_STATUS_V1_NOT_FOUND, 0, {}};
                *address = found->second.address;
                return {found->second.status, 0, {}};
            });
    }
    bool batched() const noexcept { return batch_; }
    std::size_t size() const noexcept { return results_.size(); }
    void Clear() { results_.clear(); }

private:
    struct Result { std::uint32_t status{ANOMALY_STATUS_V1_UNAVAILABLE}; std::uintptr_t address{}; };
    std::map<std::string, Result, std::less<>> results_;
    bool batch_{};
};

} // namespace summon_lab
