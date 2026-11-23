#pragma once

#include <anomaly/sdk/plugin.h>
#include <nlohmann/json_fwd.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace equipment_manager {

enum class DispatchResult { rejected, dispatched, unknown };
enum class AvatarExportResult { waiting, exported, rejected };
enum class SnapshotReadResult { unchanged, updated, waiting, unavailable };

class GameBridge final {
public:
    GameBridge();
    ~GameBridge();
    GameBridge(const GameBridge&) = delete;
    GameBridge& operator=(const GameBridge&) = delete;

    bool Load(const AnomalyHostApiV1* host, std::string& error);
    void Stop();
    // These operations run only in the Host's Game callback domain.
    bool Read(nlohmann::json& state, std::string& error);
    SnapshotReadResult ReadIfDirty(nlohmann::json& state, std::string& error, bool force = false);
    bool ReadCatalogPage(std::string_view section, std::size_t offset, std::size_t limit,
                         nlohmann::json& page, std::string& error);
    bool ReadCharacterModel(std::string_view role_id, const nlohmann::json& profile,
                            nlohmann::json& model, std::string& error);
    bool ReadCultivationModel(const nlohmann::json& request, nlohmann::json& model, std::string& error);
    bool ReadAvatarMetadata(nlohmann::json& characters, std::string& error);
    AvatarExportResult ExportAvatar(std::string_view role_id, std::string_view expected_asset_path,
                                    std::wstring_view filename, std::string& error);
    void ReleaseAvatar(std::wstring_view filename);
    DispatchResult Dispatch(const nlohmann::json& command, const nlohmann::json& observed, std::string& error);
    std::uint64_t Context() const;
    std::uint64_t NotificationSequence() const;
    std::uint64_t SourceNotificationSequence() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace equipment_manager
