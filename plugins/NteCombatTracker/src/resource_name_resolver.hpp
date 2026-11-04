#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nte::combat_stats {

enum class DisplayNameOrigin { Resource, Technical, Unknown };

[[nodiscard]] std::string_view DisplayNameOriginName(
    DisplayNameOrigin origin) noexcept;

struct ResolvedParticipantName {
    std::string technical_name;
    std::string display_name;
    std::string enemy_config_id;
    std::string monster_id;
    std::string attribute;
    DisplayNameOrigin origin{DisplayNameOrigin::Unknown};
    std::string resource_version;
    std::optional<std::uint32_t> character_id;
};

struct ResolvedDamageName {
    std::string technical_name;
    std::string display_name;
    DisplayNameOrigin origin{DisplayNameOrigin::Unknown};
    std::string resource_version;
    std::string gameplay_effect_name;
    std::string ability_name;
};

struct ResolvedAbilityName {
    std::string technical_name;
    std::string display_name;
    DisplayNameOrigin origin{DisplayNameOrigin::Unknown};
    std::string resource_version;
};

class ResourceNameResolver {
public:
    struct Entries;

    [[nodiscard]] static ResourceNameResolver Load(
        const std::filesystem::path& dict_root,
        std::string& error);

    [[nodiscard]] ResolvedParticipantName ResolveParticipant(
        std::string_view technical_path) const;
    [[nodiscard]] ResolvedDamageName ResolveDamageSource(
        std::string_view technical_path) const;
    [[nodiscard]] ResolvedAbilityName ResolveAbilityDisplay(
        std::string_view technical_path) const;
    [[nodiscard]] const std::string& version() const noexcept { return version_; }

private:
    std::string version_;
    std::shared_ptr<const Entries> entries_;
};

}  // namespace nte::combat_stats
