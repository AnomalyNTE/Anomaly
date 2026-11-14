#pragma once
#include "map_data.hpp"
#include <optional>

namespace collectible_map {
class Collection {
public:
    void Resolve(const anomaly::sdk::Host& host, const Engine& engine, const anomaly::sdk::Profile& profile) {
        using namespace anomaly::sdk;
        const auto signatures = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto query = ResolveProfileSymbol(signatures.get(), engine.core, profile, "ReadOracleProgress");
        if (!query) return;
        const auto offset = query.candidate->values.find("OracleRecordOwner");
        if (offset == query.candidate->values.end() || !offset->second ||
            offset->second > 0x10000 || offset->second % alignof(uintptr_t)) return;
        oracle_owner_ = offset->second;
        oracle_state_ = reinterpret_cast<OracleState>(query.address);
    }

    void Bind(Engine& engine) {
        if (gift_state_.function) return;
        gift_state_ = engine.Bind("HTGame", "HTPlayerState", "GetTreasureboxState", 2, 12,
            {{"TreasureboxID", 0, 8, "NameProperty"}, {"ReturnValue", 8, 4, "IntProperty"}});
    }

    std::optional<bool> Completed(Engine& engine, Ref player, size_t kind, const Marker& marker) const {
        int32_t state{};
        if (kind == 0) {
            std::array<int32_t, 2> owner{};
            if (!oracle_state_ || !engine.Read(player.address + oracle_owner_, owner) || owner[0] < 0 || owner[1] <= 0) return {};
            // The game's oracle actor reads this same per-player record to decide whether it was opened.
            state = oracle_state_(player.address + oracle_owner_, &marker.key);
        } else {
            struct Params { NameId key; int32_t result{-1}; } params{marker.key};
            if (!engine.Call(player, gift_state_, &params, sizeof(params))) return {};
            state = params.result;
        }
        if (state < 0 || state > 2) return {};
        return state == 2;
    }

private:
    using OracleState = int32_t(ANOMALY_CALL*)(uintptr_t, const NameId*);
    OracleState oracle_state_{};
    uint32_t oracle_owner_{};
    Bound gift_state_;
};
}
