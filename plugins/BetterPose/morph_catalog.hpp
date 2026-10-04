#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Morph target (blend shape) catalogue for the expression page: groups the
// names a mesh carries into the categories an animator thinks in, and keeps
// the per-morph weights the panel edits. Pure data, no game access.
//
// The groups follow the NTE facial rig's naming, measured on the live body
// mesh (148 morphs): look_* (gaze), EL_* (eyelids/eyes), EB_* (brows),
// jawOpen* (mouth shapes by emotion), mouth* (lip shapes), and the rest.
namespace better_pose::morph {

enum class Group : std::uint8_t { Eyes, Gaze, Brows, Mouth, Other, Count };

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Group::Count)> kGroupKeys{
    "morph.group.eyes", "morph.group.gaze", "morph.group.brows", "morph.group.mouth",
    "morph.group.other"};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Group::Count)> kGroupNames{
    "Eyes", "Gaze", "Brows", "Mouth", "Other"};

inline bool StartsWith(const std::string_view name, const std::string_view prefix) {
  return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
}

inline Group GroupOf(const std::string_view name) {
  if (StartsWith(name, "look_"))
    return Group::Gaze;
  if (StartsWith(name, "EL_") || StartsWith(name, "eye") || StartsWith(name, "biyan") ||
      name.find("EyesClo") != std::string_view::npos)
    return Group::Eyes;
  if (StartsWith(name, "EB_"))
    return Group::Brows;
  if (StartsWith(name, "jaw") || StartsWith(name, "mouth"))
    return Group::Mouth;
  return Group::Other;
}

struct Entry {
  std::string name;
  std::array<std::uint8_t, 8> fname{};  // the engine FName, {ComparisonIndex, Number}
  Group group{Group::Other};
};

// The mesh's morphs in their authored order, grouped. `Order` lists entry
// indices group by group (stable within a group), which is how the panel
// shows them.
struct Catalog {
  std::uintptr_t asset{};
  std::vector<Entry> entries;
  std::vector<std::uint32_t> order;

  void Build(std::uintptr_t mesh_asset, std::vector<Entry> list) {
    asset = mesh_asset;
    entries = std::move(list);
    for (auto &entry : entries)
      entry.group = GroupOf(entry.name);
    order.resize(entries.size());
    for (std::uint32_t i{}; i != order.size(); ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](const std::uint32_t a, const std::uint32_t b) {
      return entries[a].group < entries[b].group;
    });
  }

  std::size_t CountIn(const Group group) const {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(), [&](const Entry &e) { return e.group == group; }));
  }
};

// Weights the user has set, by entry index. Only touched morphs are driven:
// an untouched morph is left to the game, so the character still blinks and
// talks unless the user took that morph over.
struct Weights {
  std::vector<float> value;
  std::vector<std::uint8_t> driven;

  void Resize(const std::size_t count) {
    value.assign(count, 0.0F);
    driven.assign(count, 0);
  }

  void Set(const std::size_t index, const float weight) {
    if (index >= value.size())
      return;
    value[index] = std::clamp(weight, 0.0F, 1.0F);
    driven[index] = 1;
  }

  // Release one morph back to the game.
  void Release(const std::size_t index) {
    if (index >= value.size())
      return;
    value[index] = 0.0F;
    driven[index] = 0;
  }

  std::size_t DrivenCount() const {
    return static_cast<std::size_t>(std::count(driven.begin(), driven.end(), std::uint8_t{1}));
  }
};

// An expression file stores morphs by name, so it survives a different mesh
// load order and carries over to another character that shares the names.
// Only driven morphs are saved: the rest belong to the game.
struct SavedMorph {
  std::string name;
  float weight{};
};

// Apply a saved expression to the catalogue. Every morph named in the file is
// driven at its weight; every other morph is released, so loading a file
// gives exactly that expression rather than mixing it into the current one.
// Returns how many names matched; `missing` collects the ones that did not.
inline std::size_t ApplySaved(const Catalog &catalog, const std::vector<SavedMorph> &saved,
                              Weights &weights, std::vector<std::string> &missing) {
  missing.clear();
  weights.Resize(catalog.entries.size());
  std::size_t matched{};
  for (const auto &morph : saved) {
    const auto it = std::find_if(catalog.entries.begin(), catalog.entries.end(),
                                 [&](const Entry &e) { return e.name == morph.name; });
    if (it == catalog.entries.end()) {
      missing.push_back(morph.name);
      continue;
    }
    weights.Set(static_cast<std::size_t>(it - catalog.entries.begin()), morph.weight);
    ++matched;
  }
  return matched;
}

inline std::vector<SavedMorph> CollectDriven(const Catalog &catalog, const Weights &weights) {
  std::vector<SavedMorph> out;
  const std::size_t count = (std::min)(catalog.entries.size(), weights.value.size());
  for (std::size_t i{}; i != count; ++i)
    if (weights.driven[i] != 0)
      out.push_back({catalog.entries[i].name, weights.value[i]});
  return out;
}

}  // namespace better_pose::morph
