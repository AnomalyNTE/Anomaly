#pragma once

// Plugin-owned compatibility snapshot. Every signature, game-object offset,
// vtable slot and record size/stride the plugin uses comes from
// `profile/compatibility.profile.json`; this header only declares the named
// layout the rest of the plugin reads. Loading happens once on the Lifecycle
// thread, and Game/Render callbacks only ever see this immutable snapshot.

#include <anomaly/sdk/anomaly_sdk.h>
#include <anomaly/sdk/profile.hpp>

#include <cstdint>
#include <string>

namespace nte::native {

// Instruction decoding rule, not compatibility data: a RIP-relative `48 8B 05`
// access is seven bytes long and its displacement starts at byte three.
inline constexpr std::uint32_t kRipDisplacementOffset = 3;
inline constexpr std::uint32_t kRipInstructionSize = 7;

// Named layout snapshot parsed from the profile's `layout` values.
struct Layout {
    std::uint32_t object_name_offset{};
    std::uint32_t object_registry_items_offset{};
    std::uint32_t object_count_offset{};
    std::uint32_t object_max_count_offset{};
    std::uint32_t object_max_chunks_offset{};
    std::uint32_t object_num_chunks_offset{};
    std::uint32_t object_chunk_size{};
    std::uint32_t object_item_stride{};
    std::uint32_t object_item_serial_offset{};
    std::uint32_t text_field_offset{};
    std::uint32_t set_text_vtable_offset{};
    std::int32_t gobjects_addend{};
    std::uint32_t instanced_struct_data{};
    std::uint32_t struct_super_struct{};
    std::uint32_t struct_child_properties{};
    std::uint32_t field_name{};
    std::uint32_t field_class{};
    std::uint32_t field_next{};
    std::uint32_t property_element_size{};
    std::uint32_t property_offset_internal{};
    std::uint32_t data_table_row_struct{};
    std::uint32_t data_table_row_map{};
    std::uint32_t row_map_count{};
    std::uint32_t row_map_max{};
    std::uint32_t row_stride{};
    std::uint32_t row_key_number{};
    std::uint32_t row_data{};
    std::uint32_t diagnostic_slot_start{};
    std::uint32_t diagnostic_slot_end{};
    std::uint32_t diagnostic_slot_step{};
};

// Addresses resolved from the profile's declared symbols and validated against
// their entry bytes by the shared compatibility loader.
struct Bindings {
    std::uintptr_t set_text{};
    std::uintptr_t free_string{};
    std::uintptr_t text_to_string{};
    std::uintptr_t gobjects_accessor{};
};

struct Snapshot {
    Layout layout;
    Bindings bindings;
    bool complete{};
    std::string error;
};

// Loads and validates `profile/compatibility.profile.json` through the host's
// profile/JSON services. Never throws: an unavailable or incompatible profile
// yields `complete == false` with a concrete reason, and the dependent features
// stay disabled instead of falling back to code defaults.
[[nodiscard]] Snapshot Load(const AnomalyHostApiV1* host) noexcept;

// Parses and range/alignment/overlap-validates the profile's `layout` values.
// Exposed so the offline fixture can exercise the exact production parser.
[[nodiscard]] Layout ParseLayout(
    const anomaly::sdk::Profile& profile, std::string& error);

}  // namespace nte::native
