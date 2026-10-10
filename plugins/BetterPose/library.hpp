#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace better_pose::library {

enum class Kind : std::uint8_t { Pose = 0, Motion = 1 };

struct Entry {
  std::string id;
  std::string name;
  Kind kind{Kind::Pose};
  std::uint32_t version{1};
};

inline constexpr std::size_t kMaximumEntries = 512;
inline constexpr std::size_t kMaximumNameBytes = 96;
inline constexpr std::string_view kIndexPath = "library-index.json";

inline std::string TrimName(const std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
    ++first;
  std::size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
    --last;
  std::string out(value.substr(first, last - first));
  if (out.size() > kMaximumNameBytes)
    out.resize(kMaximumNameBytes);
  return out;
}

inline bool ValidId(const std::string_view id) noexcept {
  if (id.empty() || id.size() > 64)
    return false;
  return std::all_of(id.begin(), id.end(), [](const char c) {
    return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '-';
  });
}

inline std::string PathFor(const Entry &entry) {
  // The host storage service intentionally does not create nested directories.
  // Keep entries flat in the plugin state root, with type encoded in the name.
  return std::string(entry.kind == Kind::Pose ? "library-pose-" : "library-motion-") +
         entry.id + ".json";
}

inline Entry *FindById(std::vector<Entry> &entries, const std::string_view id) noexcept {
  const auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const Entry &entry) { return entry.id == id; });
  return it == entries.end() ? nullptr : &*it;
}

inline const Entry *FindById(const std::vector<Entry> &entries,
                             const std::string_view id) noexcept {
  const auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const Entry &entry) { return entry.id == id; });
  return it == entries.end() ? nullptr : &*it;
}

inline bool HasName(const std::vector<Entry> &entries, const Kind kind,
                    const std::string_view name, const std::string_view except_id = {}) noexcept {
  return std::any_of(entries.begin(), entries.end(), [&](const Entry &entry) {
    return entry.kind == kind && entry.name == name && entry.id != except_id;
  });
}

}  // namespace better_pose::library
