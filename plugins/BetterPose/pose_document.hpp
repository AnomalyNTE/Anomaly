#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Mapping a saved pose onto the character it is loaded into.
//
// A pose file lists edited bones. Older files carry only each bone's index,
// which is its position in *that* character's skeleton; characters order
// their bones differently (costume bones sit between the arms), so an index
// from one lands on an unrelated bone of another -- a right arm's angles on a
// sleeve. Files now carry the bone's name as well, and a name is matched
// first: the index is only trusted when the file has no name, or when the
// name is missing and the character's bone at that index still has it.
namespace better_pose::pose_document {

struct SavedBone {
  std::uint64_t index{};
  std::string name;  // empty in files written before names were saved
  double pitch{};
  double yaw{};
  double roll{};
};

struct Placed {
  std::uint32_t bone{};
  double pitch{};
  double yaw{};
  double roll{};
};

struct Result {
  std::vector<Placed> placed;
  std::vector<std::string> missing;  // named bones this character does not have
  std::size_t by_name{};
  std::size_t by_index{};
};

// `names` is the loading character's skeleton (index -> name), possibly empty
// when the names are not loaded yet; then every bone falls back to its index.
inline Result Place(const std::vector<SavedBone> &saved, const std::vector<std::string> &names,
                    const std::uint64_t maximum_index) {
  Result result;
  std::unordered_map<std::string_view, std::uint32_t> lookup;
  lookup.reserve(names.size());
  for (std::uint32_t i{}; i != names.size(); ++i)
    if (!names[i].empty())
      lookup.emplace(names[i], i);
  for (const auto &bone : saved) {
    if (!bone.name.empty() && !lookup.empty()) {
      const auto found = lookup.find(bone.name);
      if (found == lookup.end()) {
        result.missing.push_back(bone.name);
        continue;
      }
      result.placed.push_back({found->second, bone.pitch, bone.yaw, bone.roll});
      ++result.by_name;
      continue;
    }
    // No name to go by (an old file, or the skeleton's names are not known
    // yet): the index is all there is.
    if (bone.index >= maximum_index)
      continue;
    result.placed.push_back({static_cast<std::uint32_t>(bone.index), bone.pitch, bone.yaw,
                             bone.roll});
    ++result.by_index;
  }
  return result;
}

}  // namespace better_pose::pose_document
