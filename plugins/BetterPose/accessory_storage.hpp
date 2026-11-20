#pragma once
#include <cstdint>

namespace better_pose::accessory_storage {
enum class Decision { Unavailable, Ready, Rebuild };

// An unreadable/temporarily cleared asset is not evidence of a different skeleton.
inline Decision Asset(const bool readable, const std::uintptr_t live,
                      const std::uintptr_t mapped) noexcept {
  if (!readable || live == 0) return Decision::Unavailable;
  return mapped != 0 && live != mapped ? Decision::Rebuild : Decision::Ready;
}

// UE may release one or both component-space buffers between evaluations. Only
// nonempty readable buffers can demonstrate that the existing mapping is too large.
inline Decision Buffers(const bool readable, const std::uintptr_t first,
                        const std::uint32_t first_count, const std::uintptr_t second,
                        const std::uint32_t second_count, const std::uint32_t required) noexcept {
  if (!readable || first == 0 || second == 0 || first_count == 0 || second_count == 0)
    return Decision::Unavailable;
  return first_count < required || second_count < required ? Decision::Rebuild : Decision::Ready;
}
} // namespace better_pose::accessory_storage
