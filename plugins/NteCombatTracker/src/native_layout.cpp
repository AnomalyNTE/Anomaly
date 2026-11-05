#include "native_layout.hpp"

#include <anomaly/sdk/cpp.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <string_view>
#include <utility>

namespace nte::native {
namespace {

constexpr std::string_view kProfileFilename = "compatibility.profile.json";
constexpr std::string_view kContract = "nte-combat-tracker-v1";
constexpr std::uint32_t kMaximumOffset = 65536;

// The plugin's supported NTE build keeps every layout value inside this bound;
// the host schema only guarantees uint32, so the semantic range stays here.
[[nodiscard]] bool ReadLayoutFields(
    const anomaly::sdk::Profile& profile, Layout& out,
    std::string& error) noexcept {
    try {
        out.object_name_offset = profile.Value("kObjectNameOffset", kMaximumOffset, 4);
        out.object_registry_items_offset =
            profile.Value("kObjectRegistryItemsOffset", kMaximumOffset, 8);
        out.object_count_offset = profile.Value("kObjectCountOffset", kMaximumOffset, 4);
        out.object_max_count_offset =
            profile.Value("kObjectMaxCountOffset", kMaximumOffset, 4);
        out.object_max_chunks_offset =
            profile.Value("kObjectMaxChunksOffset", kMaximumOffset, 4);
        out.object_num_chunks_offset =
            profile.Value("kObjectNumChunksOffset", kMaximumOffset, 4);
        out.object_chunk_size = profile.Value("kObjectChunkSize", kMaximumOffset);
        out.object_item_stride = profile.Value("kObjectItemStride", kMaximumOffset);
        out.object_item_serial_offset =
            profile.Value("kObjectItemSerialOffset", kMaximumOffset, 4);
        out.text_field_offset = profile.Value("kTextFieldOffset", kMaximumOffset, 8);
        out.set_text_vtable_offset =
            profile.Value("kSetTextVtableOffset", kMaximumOffset, 8);
        out.gobjects_addend = -static_cast<std::int32_t>(
            profile.Value("kGObjectsAddendMagnitude", kMaximumOffset));
        out.instanced_struct_data =
            profile.Value("kInstancedStructData", kMaximumOffset, 8);
        out.struct_super_struct = profile.Value("kStructSuperStruct", kMaximumOffset, 8);
        out.struct_child_properties =
            profile.Value("kStructChildProperties", kMaximumOffset, 8);
        out.field_name = profile.Value("kFieldName", kMaximumOffset, 4);
        out.field_class = profile.Value("kFieldClass", kMaximumOffset, 8);
        out.field_next = profile.Value("kFieldNext", kMaximumOffset, 8);
        out.property_element_size =
            profile.Value("kPropertyElementSize", kMaximumOffset, 4);
        out.property_offset_internal =
            profile.Value("kPropertyOffsetInternal", kMaximumOffset, 4);
        out.data_table_row_struct =
            profile.Value("kDataTableRowStruct", kMaximumOffset, 8);
        out.data_table_row_map = profile.Value("kDataTableRowMap", kMaximumOffset, 8);
        out.row_map_count = profile.Value("kRowMapCount", kMaximumOffset, 4);
        out.row_map_max = profile.Value("kRowMapMax", kMaximumOffset, 4);
        out.row_stride = profile.Value("kRowStride", kMaximumOffset);
        out.row_key_number = profile.Value("kRowKeyNumber", kMaximumOffset, 4);
        out.row_data = profile.Value("kRowData", kMaximumOffset, 8);
        out.diagnostic_slot_start =
            profile.Value("kDiagnosticSlotStart", kMaximumOffset, 8);
        out.diagnostic_slot_end =
            profile.Value("kDiagnosticSlotEnd", kMaximumOffset, 8);
        out.diagnostic_slot_step = profile.Value("kDiagnosticSlotStep", kMaximumOffset);
    } catch (const std::exception& failure) {
        if (error.empty()) error = failure.what();
        return false;
    }
    return error.empty();
}

// Field-overlap and record-shape checks belong to the implementation contract,
// not to the profile: only a layout that passes them may be used for reads.
[[nodiscard]] bool ValidateLayout(const Layout& layout, std::string& error) {
    if (layout.object_chunk_size == 0) {
        error = "kObjectChunkSize must not be zero";
    } else if (layout.object_item_stride < sizeof(void*)) {
        error = "kObjectItemStride is smaller than a pointer";
    } else if (layout.object_item_serial_offset + sizeof(std::uint32_t) >
        layout.object_item_stride) {
        error = "kObjectItemSerialOffset overlaps the item stride";
    } else if (layout.object_count_offset == layout.object_max_count_offset ||
        layout.object_num_chunks_offset == layout.object_max_chunks_offset) {
        // FUObjectArray stores capacity before the live counter; the two fields
        // must still be distinct slots.
        error = "object registry counters overlap";
    } else if (layout.field_next == layout.field_name) {
        error = "kFieldNext and kFieldName name the same field slot";
    } else if (layout.struct_child_properties == layout.struct_super_struct) {
        error = "kStructChildProperties and kStructSuperStruct name the same slot";
    } else if (layout.row_map_max < layout.row_map_count) {
        error = "kRowMapMax precedes kRowMapCount";
    } else if (layout.row_stride < layout.row_data + sizeof(void*)) {
        error = "kRowStride is smaller than the row payload";
    } else if (layout.row_key_number + sizeof(std::uint32_t) > layout.row_stride) {
        error = "kRowKeyNumber overlaps the row stride";
    } else if (layout.diagnostic_slot_step == 0) {
        error = "kDiagnosticSlotStep must not be zero";
    } else if (layout.diagnostic_slot_end < layout.diagnostic_slot_start) {
        error = "the diagnostic slot range is inverted";
    } else if ((layout.diagnostic_slot_end - layout.diagnostic_slot_start) %
            layout.diagnostic_slot_step != 0) {
        error = "the diagnostic slot range is not a whole number of steps";
    }
    return error.empty();
}

}  // namespace

Layout ParseLayout(const anomaly::sdk::Profile& profile,
    std::string& error) {
    error.clear();
    Layout layout;
    if (!ReadLayoutFields(profile, layout, error)) return {};
    if (!ValidateLayout(layout, error)) return {};
    return layout;
}

Snapshot Load(const AnomalyHostApiV1* host) noexcept {
    Snapshot snapshot;
    try {
        if (host == nullptr) {
            snapshot.error = "the host API is unavailable";
            return snapshot;
        }
        anomaly::sdk::ProfileSnapshot profile;
        if (!profile.Load(host, kProfileFilename,
                {"nte", "HTGame.exe", ".text", kContract})) {
            snapshot.error = "compatibility.profile.json was rejected";
            return snapshot;
        }
        snapshot.layout = ParseLayout(profile.Document(), snapshot.error);
        if (!snapshot.error.empty()) return snapshot;
        snapshot.bindings.set_text = profile.Address("kSetTextPattern");
        snapshot.bindings.free_string = profile.Address("kFreeStringPattern");
        snapshot.bindings.text_to_string = profile.Address("kTextToStringPattern");
        snapshot.bindings.gobjects_accessor = profile.Address("kGObjectsPattern");
        const std::array<std::pair<std::string_view, std::uintptr_t>, 4> required{{
            {"kSetTextPattern", snapshot.bindings.set_text},
            {"kFreeStringPattern", snapshot.bindings.free_string},
            {"kTextToStringPattern", snapshot.bindings.text_to_string},
            {"kGObjectsPattern", snapshot.bindings.gobjects_accessor},
        }};
        std::string missing;
        for (const auto& [name, address] : required) {
            if (address != 0) continue;
            if (!missing.empty()) missing += ", ";
            missing.append(name);
        }
        if (!missing.empty()) {
            snapshot.error = "compatibility profile did not resolve: " + missing;
            return snapshot;
        }
        snapshot.complete = true;
        return snapshot;
    } catch (...) {
        snapshot.error = "compatibility profile load failed";
        snapshot.complete = false;
        return snapshot;
    }
}

}  // namespace nte::native
