#pragma once

#include <anomaly/sdk/plugin.h>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <memory>
#include <string_view>

namespace equipment_manager {
class GameBridge;

// Game submits an engine-owned asynchronous texture readback. Worker reads the
// resulting PNG; HTTP only reads the published image and never touches UE data.
class AvatarImages final {
public:
    AvatarImages();
    ~AvatarImages();
    void Load(const AnomalyHostApiV1* host, std::uint64_t generation);
    void Stop() noexcept;
    // Worker-only cache lookup. Missing images do not start a UE request.
    nlohmann::json Cached(std::string_view role_id, const nlohmann::json& character,
                          std::string_view account_binding);
    nlohmann::json Request(std::string_view role_id, const nlohmann::json& character,
                           std::string_view account_binding, bool allow_export);
    // Worker-only publication of one verified Game metadata snapshot. The
    // per-account source index preserves the last skin across plugin reloads.
    nlohmann::json PublishSources(std::string_view account_binding, const nlohmann::json& characters);
    void TickGame(GameBridge& game);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace equipment_manager
