#pragma once

// Resolves a participant's element attribute out of the game's own
// DT_Character rows at runtime, instead of a checked-in character dictionary.
//
// Why this path: the Host publishes no character-attribute query, and its
// bounded reflection reads (anomaly.nte.entities / anomaly.nte.actors) accept
// only bool, FName and USceneComponent* properties. CharacterElementType is a
// one-byte enum, so none of those three can reach it. What is reachable is the
// UDataTable object itself: anomaly.ue5.objects.find_exact resolves it and
// core.read_memory reads its RowMap, which is the layout the built-in scanners
// already validate.
//
// Rows are keyed by character id ("1003"), which is the token the combat
// stream's participant technical path carries.

#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/ue5.h>

#include "character_identity.hpp"
#include "native_layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nte::combat_stats {

struct CharacterAttributeServices {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5NamesServiceV1* names{};
    // Profile-derived layout and resolved GObjects accessor; the attribute
    // source never scans a signature itself.
    const native::Snapshot* native{};
};

// The character id is derived from the participant object path, which is the
// only identity the combat stream carries; see character_identity.hpp.

class CharacterAttributeSource {
public:
    // Reads the shipped DT_Character rows once. Returns an empty source and
    // fills error when the table or its layout cannot be reached.
    [[nodiscard]] static CharacterAttributeSource Load(
        const CharacterAttributeServices& services,
        std::string& error);

    // Element enum name such as "CHAOS", or empty when unknown.
    [[nodiscard]] const std::string& FindElement(std::string_view character_id) const;
    // Resolves straight from a participant object path, which spells a
    // character as player_003_sagiri.
    [[nodiscard]] const std::string& FindElementForPath(std::string_view object_path) const;
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

private:
    std::unordered_map<std::string, std::string> entries_;
};

}  // namespace nte::combat_stats
