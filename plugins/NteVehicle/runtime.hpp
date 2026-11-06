#pragma once
#include "native_access.hpp"
#include "vehicle_types.hpp"
#include "messages.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace nte_vehicle {
namespace nte_vehicle_helpers {

enum class SpeedCapMaintenanceResult {
    Unchanged,   // nothing to do / observed fields still cover the target
    Rewritten,   // the game reverted the cap and the targets were written again
    WriteFailed  // the rewrite failed; the caller must disarm
};

template <typename Fields>
class SpeedCapMaintenance final {
public:
    SpeedCapMaintenance() = default;

    void Reset() {
        targets_ = Fields{};
        requested_ratio_ = 1.0F;
        armed_ = false;
    }

    // Called after the speed fields were written and verified.
    void Arm(const Fields& targets, const float requested_ratio) {
        targets_ = targets;
        requested_ratio_ = std::isfinite(requested_ratio) ? requested_ratio : 1.0F;
        armed_ = targets_.valid && requested_ratio_ > 1.0F;
    }

    [[nodiscard]] bool Armed() const { return armed_; }

    // read_observed: bool(Fields&) - fills the current movement-component values.
    // write_targets: bool(const Fields&, std::string&) - writes and readback-validates.
    template <typename ReadObserved, typename WriteTargets>
    SpeedCapMaintenanceResult Maintain(
        const ReadObserved& read_observed,
        const WriteTargets& write_targets,
        std::string& failure) const {
        if (!armed_ || requested_ratio_ <= 1.001F) return SpeedCapMaintenanceResult::Unchanged;
        Fields observed{};
        if (!read_observed(observed)) return SpeedCapMaintenanceResult::Unchanged;
        // 1% slack covers float round-trip and the game's own rounding.
        if (observed.top_speed_kmh >= targets_.top_speed_kmh * 0.99F &&
            observed.max_linear_velocity >= targets_.max_linear_velocity * 0.99F) {
            return SpeedCapMaintenanceResult::Unchanged;
        }
        if (!write_targets(targets_, failure)) return SpeedCapMaintenanceResult::WriteFailed;
        return SpeedCapMaintenanceResult::Rewritten;
    }

private:
    Fields targets_{};
    float requested_ratio_{1.0F};
    bool armed_{};
};

// Dump 5.6.1: HTWheeledVehicleDrivable::RepairSelf(bool RefreshDisplay).
struct DurabilityRepairBinding final {
    std::uintptr_t function{};
    std::uint16_t parms_size{};
    std::uint16_t parameter_offset{};
    bool has_return{};
};

[[nodiscard]] inline bool DurabilityRepairBindingValid(
    const DurabilityRepairBinding& binding) {
    return binding.function != 0 && binding.parms_size == 1U &&
        binding.parameter_offset < binding.parms_size && !binding.has_return;
}

// invoke: bool(std::uintptr_t object, std::uintptr_t function, void* parms, std::size_t size)
template <typename Invoke>
bool InvokeDurabilityRepair(const DurabilityRepairBinding& binding, const std::uintptr_t vehicle,
                            const Invoke& invoke, std::string& failure) {
    if (!DurabilityRepairBindingValid(binding)) {
        failure = TextLocalized("backend.htgame_htwheeledvehicledrivable_repairself_bool_refreshdisplay_failed_dump_34", "HTGame.HTWheeledVehicleDrivable.RepairSelf(bool RefreshDisplay) failed Dump ABI validation");
        return false;
    }
    std::uint8_t parameters[1]{};
    parameters[binding.parameter_offset] = 1U;
    if (!invoke(vehicle, binding.function, parameters, binding.parms_size)) {
        failure = TextLocalized("backend.htwheeledvehicledrivable_repairself_processevent_failed_35", "HTWheeledVehicleDrivable.RepairSelf ProcessEvent failed");
        return false;
    }
    return true;
}

}

struct Runtime : NativeAccess {
    std::mutex mutex;
    void Tick();
    void ClearObjectCaches();

    struct VehicleFunctionBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t parameter_offset{};
        std::uint16_t return_offset{};
        bool has_return{};
    };
    struct VehicleBindings {
        VehicleFunctionBinding current_vehicle{};
        // Water vehicles have a separate player-character getter and absolute-speed API.
        VehicleFunctionBinding current_water_vehicle{};
        VehicleFunctionBinding water_speed_kmh{};
        VehicleFunctionBinding set_water_top_speed_limit{};
        VehicleFunctionBinding speed_kmh{};
        VehicleFunctionBinding set_top_speed_ratio{};
        // Dump 5.6.1: HTWheeledVehicleDrivable::RepairSelf(bool RefreshDisplay).
        VehicleFunctionBinding repair_self{};
        VehicleFunctionBinding get_external_torque_ratio{};
        VehicleFunctionBinding set_external_torque_ratio{};
        VehicleFunctionBinding set_max_engine_torque{};
        VehicleFunctionBinding summon_vehicle{};
        VehicleFunctionBinding set_wheel_friction{};
        std::uintptr_t set_delayed_velocity_restore{};
        std::uintptr_t current_vehicle_receiver{};
        std::uint64_t object_generation{};
    } vehicle_bindings;
    // Dump-verified mutable fields used by Anomaly's vehicle-speed path.
    // Field offsets are resolved by name, type and size from runtime reflection.
    struct VehicleSpeedFieldBaseline {
        float max_linear_velocity{};
        float top_speed_kmh{};
        float reverse_top_speed_kmh{};
        float top_speed_ratio{};
        float engine_max_torque{};
        float engine_max_rpm{};
        float comfort_max_engine_torque{};
        float comfort_max_engine_rpm{};
        float comfort_top_speed_kmh{};
        std::uint8_t enable_top_speed_limit{};
        bool valid{};
    };
    struct VehicleCatalogEntry {
        // One record per DT row, not duplicate entries for the row key and VehicleID alias.
        std::string id;             // Canonical RowName where readable
        std::string vehicle_id;     // Row's independent VehicleID FName
        std::string display_label;  // Anomaly-style "Name (VehicleID)"
        std::uint32_t name_comparison_index{}; // DataTable RowName used as canonical selection key
        std::uint32_t name_number{};
        std::uint32_t vehicle_id_comparison_index{}; // Independent FVehicleData.VehicleID field
        std::uint32_t vehicle_id_number{};
        bool vehicle_id_valid{};
        std::uintptr_t row{};
    };
    std::vector<VehicleCatalogEntry> vehicle_catalog;
    std::uint64_t vehicle_catalog_generation{};
    std::uint64_t vehicle_catalog_sequence{};
    std::uintptr_t vehicle_catalog_table{};
    AnomalyGenerationHandleV1 vehicle_catalog_handle{}, vehicle_data_asset_handle{};
    bool vehicle_catalog_valid{};
    // Vehicle data asset resolved by EnsureVehicleCatalogLocked, kept for the
    // customization tables (DT_vehicleModuleData / VehicleSkinDataTable).
    std::uintptr_t vehicle_data_asset_object{};
    struct VehicleBuildConfiguration {
        std::uint64_t vehicle_id{};
        std::uint64_t skin_id{};
        std::array<std::uint8_t, 0x1C> default_module_data{};
        std::vector<std::uint64_t> module_ids;
    };
    struct VehicleCustomizationOption {
        std::string id;
        std::string label;
        std::uint32_t kind{};
        std::uint32_t module_type{};
        std::uint32_t name_comparison_index{};
        std::uint32_t name_number{};
        std::uint64_t native_name{};
        bool is_default{};
    };
    std::vector<VehicleCustomizationOption> vehicle_customization_options;
    std::string vehicle_customization_vehicle_row_id;
    std::uint64_t vehicle_customization_generation{};
    std::uint64_t vehicle_customization_vehicle_sequence{};
    std::uint64_t vehicle_customization_sequence{};
    const char* vehicle_customization_failure_reason{TextLocalized("backend.customization_catalog_has_not_been_scanned_36", "customization catalog has not been scanned")};
    const char* vehicle_catalog_failure_reason{TextLocalized("backend.catalog_has_not_been_scanned_37", "catalog has not been scanned")};
    // Cache resolved display text by canonical row key until catalog refresh.
    std::unordered_map<std::string, std::string> vehicle_display_name_cache;
    std::uint64_t vehicle_display_name_cache_generation{};
    std::string selected_summon_vehicle_id;
    std::uintptr_t current_vehicle_object{};
    std::uintptr_t vehicle_movement_component{};
    bool vehicle_is_water{};
    float vehicle_top_speed_ratio{1.0F};
    AnomalyGenerationHandleV1 current_vehicle_handle{}, movement_handle{};
    // Track current-velocity scaling separately from the top-speed limiter. A failed
    // velocity restore must not advance this baseline or a retry would scale by 1.0.
    float vehicle_current_velocity_ratio{1.0F};
    std::string vehicle_delayed_velocity_restore_failure_reason;
    std::string vehicle_speed_restore_status;
    std::string vehicle_summon_status_message;
    float vehicle_engine_torque_ratio{1.0F};
    float vehicle_requested_engine_torque_ratio{1.0F};
    bool vehicle_engine_torque_fallback_active{};
    float vehicle_engine_torque_fallback_ratio{1.0F};
    float vehicle_base_engine_torque{};
    std::uintptr_t vehicle_base_torque_component{};
    bool vehicle_base_engine_torque_valid{};
    VehicleSpeedFieldBaseline vehicle_speed_field_baseline{};
    std::uintptr_t vehicle_speed_field_baseline_component{};
    bool vehicle_speed_fields_applied{};
    // Keeps the applied speed targets so the Game tick can re-assert them after the
    // movement component rebuilds its speed state from the data table.
    nte_vehicle_helpers::SpeedCapMaintenance<VehicleSpeedFieldBaseline>
        vehicle_speed_maintenance;
    bool vehicle_wheel_friction_enabled{true};
    // 按非玩家载具的 MovementComponent 分别保存速率倍率和原始扭矩基准，绝不与玩家载具共用状态。
    std::unordered_map<std::uintptr_t, AnomalyGenerationHandleV1> other_vehicle_handles;
    std::unordered_map<std::uintptr_t, float> other_vehicle_velocity_ratios;
    std::unordered_map<std::uintptr_t, float> other_vehicle_base_torques;
    std::unordered_map<std::uintptr_t, float> other_vehicle_torque_fallback_ratios;
    std::unordered_map<std::uintptr_t, float> other_vehicle_requested_torque_ratios;
    std::unordered_map<std::uintptr_t, VehicleSpeedFieldBaseline> other_vehicle_speed_field_baselines;
    std::unordered_map<std::uintptr_t, float> other_vehicle_speed_field_ratios;
    // Minimum speed ratio for loaded non-player vehicles. Zero means no floor.
    float vehicle_other_speed_multiplier_floor{};
    std::string vehicle_other_speed_status;
    std::string vehicle_other_torque_status;
    float vehicle_speed_kmh{};
    bool vehicle_valid{};

    [[nodiscard]] bool BuildVehicleBindingLocked(
        const std::uintptr_t function,
        const std::string_view expected_name,
        const std::string_view mode,
        VehicleFunctionBinding& binding) const {
        try {
            std::string function_name;
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != expected_name) return false;
            std::uintptr_t function_class{};
            std::uintptr_t outer{};
            std::string function_class_name;
            std::string outer_name;
            if (!ReadPointerAt(*memory, function, layout.object_class, function_class) ||
                !ReadPointerAt(*memory, function, layout.object_outer, outer) ||
                !ReadReflectedObjectNameLocked(function_class, function_class_name) ||
                !ReadReflectedObjectNameLocked(outer, outer_name) ||
                function_class_name != "Function") return false;
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            std::uintptr_t property{};
            if (!ReadValue(*memory, function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, function + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, function + layout.ufunction_returnValueOffset, return_offset) ||
                !ReadNullablePointerAt(*memory, function, layout.ustruct_propertyLink, property)) return false;
            const bool object_return = mode == "ObjectReturn";
            const bool float_return = mode == "FloatReturn";
            const bool float_input = mode == "FloatInput";
            const bool bool_input = mode == "BoolInput";
            const bool name_input = mode == "NameInput";
            const bool no_args = mode == "NoArgs";
            if (no_args) {
                if (num_parms != 0 || parms_size != 0 || return_offset != 0xFFFFu || property != 0) return false;
                binding = {function, 0, 0, 0xFFFFu, false};
                return true;
            } else if (object_return || float_return) {
                const std::uint16_t expected_size = object_return ? 8u : 4u;
                if (num_parms != 1 || parms_size != expected_size || return_offset == 0xFFFFu) return false;
            } else if (float_input || bool_input || name_input) {
                const std::uint16_t expected_size = name_input ? 8u : (float_input ? 4u : 1u);
                if (num_parms != 1 || parms_size != expected_size || return_offset != 0xFFFFu) return false;
            } else {
                return false;
            }
            ReflectedPropertyInfo info;
            if (!ReadReflectedPropertyLocked(property, info) || info.array_dim != 1 ||
                info.offset < 0 || info.element_size <= 0 ||
                info.offset + info.element_size > parms_size || info.next != 0 || info.name.empty()) {
                // The property-link chain normally ends at null. If the chain has
                // more than one property, reject this narrow vehicle ABI rather than guessing.
                return false;
            }
            const bool expected_return =
                ((object_return && info.type == "ObjectProperty" && info.element_size == 8) ||
                 (float_return && info.type == "FloatProperty" && info.element_size == 4)) &&
                static_cast<std::uint16_t>(info.offset) == return_offset;
            const bool expected_input =
                (float_input && info.type == "FloatProperty" && info.element_size == 4) ||
                (bool_input && info.type == "BoolProperty" && info.element_size == 1) ||
                (name_input && info.name == "VehicleID" &&
                    info.type == "NameProperty" && info.element_size == 8);
            if (!expected_return && !expected_input) return false;
            binding = {function, parms_size,
                static_cast<std::uint16_t>(info.offset), return_offset,
                object_return || float_return};
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool FindVehicleFunctionLocked(
        const std::string_view name,
        const std::span<const std::string_view> outers,
        const std::string_view mode,
        VehicleFunctionBinding& binding,
        const std::string_view script_module = "HTGame") const {
        for (const auto outer : outers) {
            std::wstring path = L"/Script/";
            path.reserve(path.size() + script_module.size() + 1U + outer.size() + 1U + name.size());
            for (char c : script_module) path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            path.push_back(L'.');
            for (char c : outer) path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            path.push_back(L'.');
            for (char c : name) path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            std::uintptr_t function{};
            if (FindExactObjectLocked(path.c_str(), function) &&
                BuildVehicleBindingLocked(function, name, mode, binding)) {
                return true;
            }
        }
        return false;
    }

    // Resolve display text through the host's read-only FText decoder.
    bool ResolveVehicleDisplayNameLocked(std::uintptr_t row,
        const ReflectedPropertyInfo& property, std::string& value) const {
        return ResolveText(row + property.offset, value);
    }

    [[nodiscard]] bool EnsureVehicleCatalogLocked() {
        const auto fail = [this](const char* reason) {
            vehicle_catalog_failure_reason = reason;
            return false;
        };
        try {
            if (!ready || object_registry.items == 0 ||
                !process_event_invoker || GetCurrentThreadId() !=
                    game_thread_id.load(std::memory_order_acquire)) {
                return fail(TextLocalized("backend.vehicle_catalog_requires_a_valid_profile_object_38", "Vehicle catalog requires a valid profile, object registry, ProcessEvent and Game thread."));
            }
            if (vehicle_display_name_cache_generation != object_generation) {
                vehicle_display_name_cache.clear();
                vehicle_display_name_cache_generation = object_generation;
            }
            if (vehicle_catalog_valid && vehicle_catalog_generation == object_generation &&
                IsCurrentObject(vehicle_catalog_handle, vehicle_catalog_table) &&
                IsCurrentObject(vehicle_data_asset_handle, vehicle_data_asset_object)) return true;
            vehicle_catalog_valid = false;
            vehicle_display_name_cache.clear();

            VehicleFunctionBinding asset_binding{};
            static constexpr std::array<std::string_view, 1> data_outer{"HTGameData"};
            if (!FindVehicleFunctionLocked("GetVehicleDataAsset", data_outer,
                    "ObjectReturn", asset_binding)) {
                return fail(TextLocalized("backend.htgamedata_getvehicledataasset_failed_reflection_validation_39", "HTGameData.GetVehicleDataAsset failed reflection validation."));
            }
            std::uintptr_t data_class{}, default_object{};
            if (!ReadPointerAt(*memory, asset_binding.function,
                    layout.object_outer, data_class) ||
                !ReadPointerAt(*memory, data_class,
                    layout.uclass_classDefaultObject, default_object) ||
                default_object == 0) {
                return fail(TextLocalized("backend.getvehicledataasset_class_or_cdo_could_not_be_40", "GetVehicleDataAsset class or CDO could not be resolved."));
            }
            std::array<std::uint8_t, 8> output{};
            if (!InvokeProcessEventGuarded(process_event_invoker, default_object,
                    asset_binding.function, output.data(), output.size())) {
                return fail(TextLocalized("backend.getvehicledataasset_processevent_failed_41", "GetVehicleDataAsset ProcessEvent failed."));
            }
            std::uintptr_t vehicle_asset{};
            std::memcpy(&vehicle_asset, output.data() + asset_binding.return_offset,
                sizeof(vehicle_asset));
            if (vehicle_asset == 0) return fail(TextLocalized("backend.getvehicledataasset_returned_no_object_42", "GetVehicleDataAsset returned no object."));

            std::uintptr_t asset_class{};
            ReflectedPropertyInfo data_table_property;
            if (!ReadPointerAt(*memory, vehicle_asset, layout.object_class, asset_class) ||
                !FindReflectedPropertyLocked(asset_class, "DT_VehicleData", data_table_property, true) ||
                data_table_property.type != "ObjectProperty" ||
                data_table_property.element_size != 8 || data_table_property.offset < 0) {
                return fail(TextLocalized("backend.htvehicledataasset_dt_vehicledata_failed_property_validation_43", "HTVehicleDataAsset.DT_VehicleData failed property validation."));
            }
            std::uintptr_t table{};
            if (!ReadValue(*memory, vehicle_asset + static_cast<std::uintptr_t>(data_table_property.offset), table) ||
                table == 0) {
                return fail(TextLocalized("backend.htvehicledataasset_dt_vehicledata_is_empty_or_unreadable_44", "HTVehicleDataAsset.DT_VehicleData is empty or unreadable."));
            }
            std::uintptr_t row_struct{};
            std::string row_struct_name;
            if (!ReadPointerAt(*memory, table, layout.dataTable_rowStruct, row_struct) ||
                !ReadReflectedObjectNameLocked(row_struct, row_struct_name) ||
                row_struct_name != "VehicleData") {
                return fail(TextLocalized("backend.dt_vehicledata_rowstruct_is_not_vehicledata_45", "DT_VehicleData.RowStruct is not VehicleData."));
            }
            ReflectedPropertyInfo vehicle_id_property;
            if (!FindReflectedPropertyLocked(row_struct, "VehicleID", vehicle_id_property, true) ||
                vehicle_id_property.type != "NameProperty" ||
                vehicle_id_property.element_size != 8 || vehicle_id_property.offset < 0) {
                return fail(TextLocalized("backend.vehicledata_vehicleid_failed_nameproperty_validation_46", "VehicleData.VehicleID failed NameProperty validation."));
            }
            ReflectedPropertyInfo vehicle_name_property;
            if (!FindReflectedPropertyLocked(row_struct, "Name", vehicle_name_property, true) ||
                vehicle_name_property.type != "TextProperty" ||
                vehicle_name_property.element_size != 0x10 || vehicle_name_property.offset < 0) {
                return fail(TextLocalized("backend.vehicledata_name_failed_ftext_validation_47", "VehicleData.Name failed FText validation."));
            }
            // FText conversion enriches the label, but it is not a prerequisite for
            // enumerating DT_VehicleData. If the optional conversion ABI is unavailable,
            // publish the canonical RowName / VehicleID fallback below rather than losing
            // otherwise valid rows such as numeric VehicleID entries (e.g. 918 or 911).
            const bool can_resolve_display_text = (names && names->resolve_ftext_utf8);

            const auto row_map_offset = layout.dataTable_rowMap;
            SparseMapView view;
            if (row_map_offset < 0 ||
                !ReadSparseMapViewLocked(table + static_cast<std::uintptr_t>(row_map_offset), view) ||
                view.num <= 0 || view.num > 4096 || view.num_free > view.num) {
                return fail(TextLocalized("backend.dt_vehicledata_rowmap_layout_or_count_is_48", "DT_VehicleData.RowMap layout or count is invalid."));
            }
            std::vector<std::uint8_t> elements(static_cast<std::size_t>(view.num) *
                static_cast<std::size_t>(view.stride));
            if (!memory->Read(view.data, elements.data(), elements.size())) {
                return fail(TextLocalized("backend.dt_vehicledata_rowmap_elements_are_unreadable_49", "DT_VehicleData.RowMap elements are unreadable."));
            }
            std::vector<VehicleCatalogEntry> next;
            next.reserve(static_cast<std::size_t>(view.num - view.num_free));
            std::vector<std::uint8_t> row_bytes(static_cast<std::size_t>(vehicle_id_property.offset) + 8U);
            for (std::int32_t slot = 0; slot < view.num; ++slot) {
                const auto u_slot = static_cast<std::uint32_t>(slot);
                if ((view.flags[static_cast<std::size_t>(u_slot) / 32U] &
                        (1U << (u_slot & 31U))) == 0) continue;
                const auto* element = elements.data() +
                    static_cast<std::size_t>(slot) * static_cast<std::size_t>(view.stride);
                std::uintptr_t row{};
                std::memcpy(&row, element + view.row_offset, sizeof(row));
                if (row == 0 || !ReadableRange(*memory, row, row_bytes.size()) ||
                    !memory->Read(row, row_bytes.data(), row_bytes.size())) continue;

                std::uint32_t row_name_comparison_index{}, row_name_number{};
                std::memcpy(&row_name_comparison_index, element, sizeof(row_name_comparison_index));
                std::memcpy(&row_name_number, element + sizeof(row_name_comparison_index),
                    sizeof(row_name_number));
                std::string row_name;
                const bool row_name_valid =
                    ResolveFNameLocked(row_name_comparison_index, row_name_number, row_name) &&
                    !row_name.empty() && row_name != "None" &&
                    row_name.size() < nte_vehicle::kVehicleIdMaxBytes;

                std::uint32_t comparison_index{}, number{};
                std::memcpy(&comparison_index,
                    row_bytes.data() + static_cast<std::size_t>(vehicle_id_property.offset), 4);
                std::memcpy(&number,
                    row_bytes.data() + static_cast<std::size_t>(vehicle_id_property.offset) + 4U, 4);
                std::string vehicle_id;
                const bool vehicle_id_valid =
                    ResolveFNameLocked(comparison_index, number, vehicle_id) &&
                    !vehicle_id.empty() && vehicle_id != "None" &&
                    vehicle_id.size() < nte_vehicle::kVehicleIdMaxBytes;
                if (!row_name_valid && !vehicle_id_valid) continue;

                // Anomaly displays one DataTable row as "RowName (VehicleID)".
                // Use the row key for selection, but keep VehicleID as a legacy alias.
                VehicleCatalogEntry entry{};
                entry.id = row_name_valid ? row_name : vehicle_id;
                entry.vehicle_id = vehicle_id_valid ? vehicle_id : entry.id;
                std::string display_name;
                bool display_name_valid = false;
                const auto cached_display_name = vehicle_display_name_cache.find(entry.id);
                if (cached_display_name != vehicle_display_name_cache.end()) {
                    display_name = cached_display_name->second;
                    display_name_valid = !display_name.empty();
                } else if (can_resolve_display_text &&
                    ResolveVehicleDisplayNameLocked(row, vehicle_name_property, display_name)) {
                    vehicle_display_name_cache.emplace(entry.id, display_name);
                    display_name_valid = true;
                }
                if (display_name_valid && vehicle_id_valid && display_name != vehicle_id) {
                    entry.display_label = LocalizedStatus("backend.0_1_33", "{0} ({1})", {display_name, vehicle_id});
                } else if (display_name_valid) {
                    entry.display_label = display_name;
                } else if (row_name_valid && vehicle_id_valid && row_name != vehicle_id) {
                    // Clearly retain a usable identifier if FText conversion is unavailable;
                    // never pass this display fallback back as a native function name.
                    entry.display_label = LocalizedStatus("backend.0_1_33", "{0} ({1})", {row_name, vehicle_id});
                } else {
                    entry.display_label = entry.id;
                }
                entry.name_comparison_index = row_name_valid
                    ? row_name_comparison_index : comparison_index;
                entry.name_number = row_name_valid ? row_name_number : number;
                entry.vehicle_id_comparison_index = comparison_index;
                entry.vehicle_id_number = number;
                entry.vehicle_id_valid = vehicle_id_valid;
                entry.row = row;
                next.push_back(std::move(entry));
            }
            std::ranges::sort(next, {}, &VehicleCatalogEntry::id);
            next.erase(std::unique(next.begin(), next.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.id == rhs.id;
            }), next.end());
            if (next.empty()) {
                return fail(TextLocalized("backend.dt_vehicledata_contains_no_valid_rowname_or_50", "DT_VehicleData contains no valid RowName or VehicleID. Refresh to retry."));
            }
            if (!ObjectHandleLocked(table, vehicle_catalog_handle) ||
                !ObjectHandleLocked(vehicle_asset, vehicle_data_asset_handle))
                return fail(TextLocalized("backend.catalog_expired", "Vehicle catalog objects are no longer available."));
            vehicle_catalog = std::move(next);
            vehicle_catalog_generation = object_generation;
            vehicle_catalog_table = table;
            vehicle_data_asset_object = vehicle_asset;
            vehicle_catalog_valid = true;
            ++vehicle_catalog_sequence;
            if (vehicle_catalog_sequence == 0) ++vehicle_catalog_sequence;
            vehicle_catalog_failure_reason = TextLocalized("backend.vehicle_catalog_loaded_51", "Vehicle catalog loaded.");
            if (!selected_summon_vehicle_id.empty()) {
                // Normalize a previously selected VehicleID alias to the unique canonical RowName.
                // The summon path always resolves the canonical row key, never a display label.
                const auto exact = std::ranges::find(
                    vehicle_catalog, selected_summon_vehicle_id, &VehicleCatalogEntry::id);
                if (exact != vehicle_catalog.end()) {
                    selected_summon_vehicle_id = exact->id;
                } else {
                    const VehicleCatalogEntry* alias_match{};
                    bool alias_ambiguous{};
                    for (const auto& entry : vehicle_catalog) {
                        if (entry.vehicle_id != selected_summon_vehicle_id) continue;
                        if (alias_match != nullptr && alias_match->row != entry.row) {
                            alias_ambiguous = true;
                            break;
                        }
                        alias_match = &entry;
                    }
                    selected_summon_vehicle_id =
                        alias_match != nullptr && !alias_ambiguous ? alias_match->id : std::string{};
                }
            }
            return true;
        } catch (...) {
            vehicle_catalog.clear();
            vehicle_catalog_valid = false;
            vehicle_catalog_table = 0;
            vehicle_catalog_failure_reason = TextLocalized("backend.an_exception_occurred_while_reading_dt_vehicledata_52", "An exception occurred while reading DT_VehicleData.");
            return false;
        }
    }

    // 车辆类名需要保留 vehicle token 检查；DT_VehicleData.VehicleID 则不能用这条规则筛选，
    // 因为表里的合法主键（例如 918）可以是纯数字。
    [[nodiscard]] static bool ContainsVehicleToken(std::string_view value) {
        constexpr std::string_view token = "vehicle";
        if (value.size() < token.size()) return false;
        for (std::size_t start = 0; start + token.size() <= value.size(); ++start) {
            bool match = true;
            for (std::size_t offset = 0; offset < token.size(); ++offset) {
                char character = value[start + offset];
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
                if (character != token[offset]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    [[nodiscard]] bool IsVehicleActorLocked(const std::uintptr_t object) const {
        if (object == 0) return false;
        try {
            std::uintptr_t klass{};
            std::string class_name;
            return ReadPointerAt(*memory, object, layout.object_class, klass) &&
                ReadReflectedObjectNameLocked(klass, class_name) &&
                ContainsVehicleToken(class_name) &&
                ObjectClassChainContainsLocked(object, "Actor");
        } catch (...) { return false; }
    }

    [[nodiscard]] bool InvokeVehicleSetOwnerLocked(
        const std::uintptr_t actor, const std::uintptr_t owner) {
        try {
            std::uintptr_t function{};
            std::string function_name;
            std::uintptr_t function_class{};
            std::uintptr_t outer{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (actor == 0 || owner == 0 ||
                !FindExactObjectLocked(L"/Script/Engine.Actor.SetOwner", function) ||
                !ReadReflectedObjectNameLocked(function, function_name) || function_name != "SetOwner" ||
                !ReadPointerAt(*memory, function, layout.object_class, function_class) ||
                !ReadPointerAt(*memory, function, layout.object_outer, outer) ||
                !ReadReflectedObjectNameLocked(function_class, function_name) || function_name != "Function" ||
                !ReadReflectedObjectNameLocked(outer, function_name) || function_name != "Actor" ||
                !ReadValue(*memory, function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, function + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, function + layout.ufunction_returnValueOffset, return_offset) ||
                num_parms != 1 || parms_size != 8 || return_offset != 0xFFFFu) return false;
            ReflectedPropertyInfo input;
            if (!FindReflectedPropertyLocked(function, "NewOwner", input, false) ||
                input.type != "ObjectProperty" || input.element_size != 8 || input.offset != 0) return false;
            std::array<std::uint8_t, 8> parameters{};
            std::memcpy(parameters.data(), &owner, sizeof(owner));
            return InvokeProcessEventGuarded(process_event_invoker, actor, function,
                parameters.data(), parameters.size());
        } catch (...) { return false; }
    }

    [[nodiscard]] bool RelocateSpawnedVehicleLocked(
        const std::uintptr_t actor, const std::array<double, 3>& destination) {
        try {
            std::uintptr_t function{};
            TeleportBinding binding;
            if (actor == 0 || !std::ranges::all_of(destination,
                    [](double value) { return std::isfinite(value); }) ||
                !FindExactObjectLocked(L"/Script/Engine.Actor.K2_SetActorLocation", function) ||
                !BuildTeleportBindingLocked(function, binding)) return false;
            const auto words = (static_cast<std::size_t>(binding.parms_size) + 7U) / 8U;
            std::vector<std::uint64_t> parameter_words(words, 0);
            auto* parameters = reinterpret_cast<std::uint8_t*>(parameter_words.data());
            std::memcpy(parameters + binding.new_location_offset, destination.data(), sizeof(destination));
            const auto set_bool = [parameters](const ReflectedBoolParameter& parameter, bool value) {
                auto& byte = parameters[parameter.byte_offset];
                byte = static_cast<std::uint8_t>((byte & static_cast<std::uint8_t>(~parameter.field_mask)) |
                    (value ? parameter.byte_mask : 0U));
            };
            set_bool(binding.b_sweep, false);
            set_bool(binding.b_teleport, true);
            if (!InvokeProcessEventGuarded(process_event_invoker, actor, function,
                    parameters, binding.parms_size) ||
                (parameters[binding.return_value.byte_offset] & binding.return_value.field_mask) == 0) return false;
            // Verify the final transform from the game's own K2 getter before reporting success.
            std::uintptr_t getter{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!FindExactObjectLocked(L"/Script/Engine.Actor.K2_GetActorLocation", getter) ||
                !ReadValue(*memory, getter + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, getter + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, getter + layout.ufunction_returnValueOffset, return_offset) ||
                num_parms != 1 || parms_size != 24 || return_offset != 0) return false;
            ReflectedPropertyInfo ret;
            if (!FindReflectedPropertyLocked(getter, "ReturnValue", ret, false) ||
                ret.type != "StructProperty" || ret.element_size != 24 || ret.offset != 0) return false;
            std::array<std::uint8_t, 24> location_bytes{};
            if (!InvokeProcessEventGuarded(process_event_invoker, actor, getter,
                    location_bytes.data(), location_bytes.size())) return false;
            std::array<double, 3> actual{};
            std::memcpy(actual.data(), location_bytes.data(), sizeof(actual));
            constexpr double tolerance = 0.05;
            for (std::size_t i = 0; i < destination.size(); ++i) {
                if (!std::isfinite(actual[i]) || std::fabs(actual[i] - destination[i]) > tolerance) return false;
            }
            return true;
        } catch (...) { return false; }
    }

    [[nodiscard]] bool ValidateDelayedVelocityRestoreLocked(
        const std::uintptr_t function,
        std::string& failure_reason) const {
        try {
            const auto fail = [&failure_reason](std::string message) {
                failure_reason = std::move(message);
                return false;
            };
            if (function == 0) return fail(TextLocalized("backend.the_ufunction_pointer_is_null_53", "The UFunction pointer is null."));
            std::uintptr_t outer{}, function_class{};
            std::string outer_name, function_class_name, function_name;
            std::uint8_t num_parms{};
            std::uint16_t parms_size{}, return_offset{};
            if (!ReadPointerAt(*memory, function, layout.object_outer, outer) ||
                !ReadPointerAt(*memory, function, layout.object_class, function_class) ||
                !ReadReflectedObjectNameLocked(outer, outer_name) ||
                !ReadReflectedObjectNameLocked(function_class, function_class_name) ||
                !ReadReflectedObjectNameLocked(function, function_name)) {
                return fail(TextLocalized("backend.could_not_read_ufunction_owner_class_or_54", "Could not read UFunction owner, class or name."));
            }
            if (outer_name != "HTVehicleMovementComponent" ||
                function_class_name != "Function" || function_name != "SetDelayedVelocityRestore") {
                return fail(LocalizedStatus("backend.ufunction_mismatch_owner_0_class_1_name_32", "UFunction mismatch: owner={0}\uff0cClass={1}\uff0cName={2}", {outer_name, function_class_name, function_name}));
            }
            if (!ReadValue(*memory, function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, function + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, function + layout.ufunction_returnValueOffset, return_offset)) {
                return fail(TextLocalized("backend.could_not_read_setdelayedvelocityrestore_parameter_metadata_55", "Could not read SetDelayedVelocityRestore parameter metadata."));
            }
            if (num_parms != 3 || parms_size != 0x34 || return_offset != 0xFFFFU) {
                return fail(LocalizedStatus("backend.setdelayedvelocityrestore_expects_numparms_3_parmssize_52_returnvalueoffset_31", "SetDelayedVelocityRestore expects NumParms=3, ParmsSize=52, ReturnValueOffset=65535; observed NumParms={0}, ParmsSize={1}, ReturnValueOffset={2}", {std::to_string(num_parms), std::to_string(parms_size), std::to_string(return_offset)}));
            }
            const auto vector_field = [&](std::string_view name, std::int32_t expected_offset) {
                ReflectedPropertyInfo property;
                if (!FindReflectedPropertyLocked(function, name, property, false)) {
                    failure_reason = LocalizedStatus("backend.missing_parameter_0_not_found_in_propertylink_30", "Missing parameter {0} (not found in PropertyLink)", {std::string(name)});
                    return false;
                }
                if (property.type != "StructProperty" || property.element_size != 0x18 ||
                    property.offset != expected_offset || property.array_dim != 1 || property.structure == 0) {
                    failure_reason = LocalizedStatus("backend.parameter_0_has_an_incompatible_layout_type_29", "Parameter {0} has an incompatible layout: type={1}, Size={2}, Offset={3}, ArrayDim={4}", {std::string(name), property.type, std::to_string(property.element_size), std::to_string(property.offset), std::to_string(property.array_dim)});
                    return false;
                }
                std::string struct_name;
                if (!ReadReflectedObjectNameLocked(property.structure, struct_name)) {
                    failure_reason = LocalizedStatus("backend.could_not_read_parameter_0_vector_structure_28", "Could not read parameter {0} Vector structure name.", {std::string(name)});
                    return false;
                }
                if (struct_name != "Vector" && struct_name != "FVector") {
                    failure_reason = LocalizedStatus("backend.parameter_0_has_an_incompatible_structure_expected_27", "Parameter {0} has an incompatible structure; expected Vector/FVector, observed {1}", {std::string(name), struct_name});
                    return false;
                }
                return true;
            };
            if (!vector_field("InLinearVelocity", 0x00) ||
                !vector_field("InAngularVelocity", 0x18)) return false;
            ReflectedPropertyInfo forward_speed;
            if (!FindReflectedPropertyLocked(function, "InForwardSpeed", forward_speed, false)) {
                return fail(TextLocalized("backend.missing_inforwardspeed_parameter_56", "Missing InForwardSpeed parameter."));
            }
            if (forward_speed.type != "FloatProperty" || forward_speed.element_size != 4 ||
                forward_speed.offset != 0x30 || forward_speed.array_dim != 1) {
                return fail(LocalizedStatus("backend.inforwardspeed_has_an_incompatible_layout_type_0_26", "InForwardSpeed has an incompatible layout: type={0}, Size={1}, Offset={2}, ArrayDim={3}", {forward_speed.type, std::to_string(forward_speed.element_size), std::to_string(forward_speed.offset), std::to_string(forward_speed.array_dim)}));
            }
            failure_reason.clear();
            return true;
        } catch (...) {
            try { failure_reason = TextLocalized("backend.an_exception_occurred_while_validating_setdelayedvelocityrestore_57", "An exception occurred while validating SetDelayedVelocityRestore."); } catch (...) {}
            return false;
        }
    }

    [[nodiscard]] bool RefreshVehicleLocked() {
        try {
            if (!ready || player_controller == 0 || !process_event_invoker) {
                vehicle_valid = false;
                current_vehicle_object = 0;
                vehicle_movement_component = 0;
                return false;
            }
            if (vehicle_bindings.object_generation != object_generation) {
                vehicle_bindings = {};
                vehicle_bindings.object_generation = object_generation;
                current_vehicle_object = 0;
                vehicle_movement_component = 0;
                vehicle_base_torque_component = 0;
                vehicle_base_engine_torque_valid = false;
                vehicle_speed_field_baseline = {};
                vehicle_speed_field_baseline_component = 0;
                other_vehicle_handles.clear();
                other_vehicle_velocity_ratios.clear();
                other_vehicle_base_torques.clear();
                other_vehicle_speed_field_baselines.clear();
                other_vehicle_speed_field_ratios.clear();
                other_vehicle_torque_fallback_ratios.clear();
                other_vehicle_requested_torque_ratios.clear();
            }

            static constexpr std::array<std::string_view, 1> controller_outer{"HTPlayerController"};
            static constexpr std::array<std::string_view, 1> player_character_outer{"HTPlayerCharacter"};
            static constexpr std::array<std::string_view, 1> vehicle_outer{"HTWheeledVehicleBase"};
            static constexpr std::array<std::string_view, 1> movement_outer{"HTVehicleMovementComponent"};

            if (vehicle_bindings.current_vehicle.function == 0) {
                if (FindVehicleFunctionLocked("BP_GetCurrentDriveVehicle", controller_outer,
                        "ObjectReturn", vehicle_bindings.current_vehicle)) {
                    vehicle_bindings.current_vehicle_receiver = player_controller;
                } else if (player_pawn != 0 && FindVehicleFunctionLocked(
                        "BP_GetCurrentDirvingVehicle", player_character_outer, "ObjectReturn",
                        vehicle_bindings.current_vehicle)) {
                    // The dump's fallback is owned by HTPlayerCharacter (not Controller).
                    vehicle_bindings.current_vehicle_receiver = player_pawn;
                }
            }
            if (vehicle_bindings.set_top_speed_ratio.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("SetTopSpeedRatio", vehicle_outer,
                    "FloatInput", vehicle_bindings.set_top_speed_ratio));
            }
            if (vehicle_bindings.repair_self.function == 0) {
                static constexpr std::array<std::string_view, 1> drivable_outer{
                    "HTWheeledVehicleDrivable"};
                static_cast<void>(FindVehicleFunctionLocked("RepairSelf", drivable_outer,
                    "BoolInput", vehicle_bindings.repair_self));
            }
            if (vehicle_bindings.speed_kmh.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("GetForwardSpeedKmH", movement_outer,
                    "FloatReturn", vehicle_bindings.speed_kmh));
            }
            if (vehicle_bindings.set_wheel_friction.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("SetEnableWheelFriction", movement_outer,
                    "BoolInput", vehicle_bindings.set_wheel_friction));
            }
            if (vehicle_bindings.get_external_torque_ratio.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("GetExternalTorqueRatio", movement_outer,
                    "FloatReturn", vehicle_bindings.get_external_torque_ratio));
            }
            if (vehicle_bindings.set_external_torque_ratio.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("SetExternalTorqueRatio", movement_outer,
                    "FloatInput", vehicle_bindings.set_external_torque_ratio));
            }
            if (vehicle_bindings.set_max_engine_torque.function == 0) {
                static constexpr std::array<std::string_view, 1> chaos_movement_outer{
                    "ChaosWheeledVehicleMovementComponent"};
                static_cast<void>(FindVehicleFunctionLocked("SetMaxEngineTorque", chaos_movement_outer,
                    "FloatInput", vehicle_bindings.set_max_engine_torque, "ChaosVehicles"));
            }

            if (vehicle_bindings.current_vehicle.function) {
                std::uintptr_t owner{}; std::string owner_name;
                if (!ReadPointerAt(*memory, vehicle_bindings.current_vehicle.function,
                        layout.object_outer, owner) ||
                    !ReadReflectedObjectNameLocked(owner, owner_name)) return false;
                vehicle_bindings.current_vehicle_receiver = owner_name == "HTPlayerController"
                    ? player_controller : player_pawn;
            }
            alignas(8) std::array<std::uint8_t, 8> out{};
            std::uintptr_t vehicle{};
            bool vehicle_return_valid = false;
            const auto invoke_current_vehicle = [&](VehicleFunctionBinding& binding,
                    std::uintptr_t receiver, std::uintptr_t& result) {
                result = 0;
                out.fill(0);
                if (receiver == 0 || binding.function == 0 ||
                    !InvokeProcessEventGuarded(process_event_invoker, receiver,
                        binding.function, out.data(), binding.parms_size)) {
                    return false;
                }
                std::memcpy(&result, out.data() + binding.return_offset, sizeof(result));
                return result != 0 && ReadableRange(*memory, result, 0x20U);
            };
            vehicle_return_valid = invoke_current_vehicle(
                vehicle_bindings.current_vehicle,
                vehicle_bindings.current_vehicle_receiver, vehicle);

            // Dump confirms both current-vehicle functions. If the Controller variant exists
            // but returns null for this live state, still try the Character-owned Anomaly fallback.
            if (!vehicle_return_valid && player_pawn != 0 &&
                vehicle_bindings.current_vehicle_receiver != player_pawn) {
                VehicleFunctionBinding character_binding{};
                if (FindVehicleFunctionLocked("BP_GetCurrentDirvingVehicle", player_character_outer,
                        "ObjectReturn", character_binding)) {
                    std::uintptr_t fallback_vehicle{};
                    if (invoke_current_vehicle(character_binding, player_pawn, fallback_vehicle)) {
                        vehicle_bindings.current_vehicle = character_binding;
                        vehicle_bindings.current_vehicle_receiver = player_pawn;
                        vehicle = fallback_vehicle;
                        vehicle_return_valid = true;
                    }
                }
            }
            // The wheeled getter does not cover HTWaterVehicleBasePawn. Detect the actual
            // board-water vehicle through the dump-defined HTPlayerCharacter getter instead.
            bool water_vehicle = false;
            if (vehicle_return_valid &&
                !ObjectClassChainContainsLocked(vehicle, "HTWheeledVehicleBase")) {
                vehicle_return_valid = false;
            }
            if (!vehicle_return_valid && player_pawn != 0) {
                static constexpr std::array<std::string_view, 1> water_character_outer{
                    "HTPlayerCharacter"};
                if (vehicle_bindings.current_water_vehicle.function == 0) {
                    static_cast<void>(FindVehicleFunctionLocked("GetBoardWaterVehicle",
                        water_character_outer, "ObjectReturn",
                        vehicle_bindings.current_water_vehicle));
                }
                std::uintptr_t water_vehicle_object{};
                if (invoke_current_vehicle(vehicle_bindings.current_water_vehicle,
                        player_pawn, water_vehicle_object) &&
                    ObjectClassChainContainsLocked(water_vehicle_object, "HTWaterVehicleBasePawn")) {
                    vehicle = water_vehicle_object;
                    vehicle_return_valid = true;
                    water_vehicle = true;
                }
            }
            if (!vehicle_return_valid) {
                vehicle_valid = false;
                current_vehicle_object = 0;
                vehicle_movement_component = 0;
                vehicle_is_water = false;
                return false;
            }

            AnomalyGenerationHandleV1 vehicle_handle{};
            if (!ObjectHandleLocked(vehicle, vehicle_handle)) return false;
            const bool vehicle_changed = !SameHandle(vehicle_handle, current_vehicle_handle) ||
                water_vehicle != vehicle_is_water;
            current_vehicle_handle = vehicle_handle;
            current_vehicle_object = vehicle;
            vehicle_is_water = water_vehicle;
            vehicle_valid = true;

            if (water_vehicle) {
                vehicle_movement_component = 0;
                if (vehicle_changed) {
                    vehicle_base_torque_component = 0;
                    vehicle_base_engine_torque_valid = false;
                    vehicle_engine_torque_ratio = 1.0F;
                    vehicle_requested_engine_torque_ratio = 1.0F;
                    vehicle_engine_torque_fallback_active = false;
                    vehicle_engine_torque_fallback_ratio = 1.0F;
                    vehicle_top_speed_ratio = 1.0F;
                    vehicle_current_velocity_ratio = 1.0F;
                    vehicle_speed_kmh = 0.0F;
                    vehicle_speed_field_baseline = {};
                    vehicle_speed_field_baseline_component = 0;
                    vehicle_speed_fields_applied = false;
                    vehicle_speed_maintenance.Reset();
                }
                static constexpr std::array<std::string_view, 1> water_vehicle_outer{
                    "HTWaterVehicleBasePawn"};
                if (vehicle_bindings.water_speed_kmh.function == 0) {
                    static_cast<void>(FindVehicleFunctionLocked("GetVelocityInKm",
                        water_vehicle_outer, "FloatReturn", vehicle_bindings.water_speed_kmh));
                }
                if (vehicle_bindings.set_water_top_speed_limit.function == 0) {
                    static_cast<void>(FindVehicleFunctionLocked("SetDynamicTopSpeedLimit",
                        water_vehicle_outer, "FloatInput",
                        vehicle_bindings.set_water_top_speed_limit));
                }
                if (vehicle_bindings.water_speed_kmh.function != 0) {
                    alignas(8) std::array<std::uint8_t, 8> water_speed_bytes{};
                    if (InvokeProcessEventGuarded(process_event_invoker, current_vehicle_object,
                            vehicle_bindings.water_speed_kmh.function, water_speed_bytes.data(),
                            vehicle_bindings.water_speed_kmh.parms_size)) {
                        float speed{};
                        std::memcpy(&speed, water_speed_bytes.data() +
                            vehicle_bindings.water_speed_kmh.return_offset, sizeof(speed));
                        if (std::isfinite(speed)) vehicle_speed_kmh = speed;
                    }
                }
                return true;
            }

            std::uintptr_t movement_component{};
            AnomalyGenerationHandleV1 component_handle{};
            const auto movement_offset = layout.vehicle_movementComponent;
            if (movement_offset < 0 ||
                !ReadPointerAt(*memory, vehicle, movement_offset, movement_component) ||
                movement_component == 0 || !ObjectHandleLocked(movement_component, component_handle) ||
                !ObjectClassChainContainsLocked(movement_component, "HTVehicleMovementComponent")) {
                vehicle_movement_component = 0;
                vehicle_valid = false;
                return false;
            }
            const bool component_changed = !SameHandle(component_handle, movement_handle);
            movement_handle = component_handle;
            vehicle_movement_component = movement_component;
            if (vehicle_changed || component_changed) {
                vehicle_base_torque_component = 0;
                vehicle_base_engine_torque_valid = false;
                vehicle_engine_torque_ratio = 1.0F;
                vehicle_requested_engine_torque_ratio = 1.0F;
                vehicle_engine_torque_fallback_active = false;
                vehicle_engine_torque_fallback_ratio = 1.0F;
                vehicle_top_speed_ratio = 1.0F;
                vehicle_current_velocity_ratio = 1.0F;
                vehicle_speed_kmh = 0.0F;
                vehicle_speed_field_baseline = {};
                vehicle_speed_field_baseline_component = 0;
                vehicle_speed_fields_applied = false;
                vehicle_speed_maintenance.Reset();
                // Revalidate this dump-defined UFunction for each movement component.
                vehicle_bindings.set_delayed_velocity_restore = 0;
                vehicle_delayed_velocity_restore_failure_reason.clear();
            }
            if (vehicle_bindings.set_delayed_velocity_restore == 0) {
                std::uintptr_t delayed_restore{};
                if (!FindExactObjectLocked(
                        L"/Script/HTGame.HTVehicleMovementComponent.SetDelayedVelocityRestore",
                        delayed_restore)) {
                    vehicle_delayed_velocity_restore_failure_reason =
                        TextLocalized("backend.htvehiclemovementcomponent_setdelayedvelocityrestore_could_not_be_found_58", "HTVehicleMovementComponent.SetDelayedVelocityRestore could not be found.");
                } else if (ValidateDelayedVelocityRestoreLocked(
                               delayed_restore, vehicle_delayed_velocity_restore_failure_reason)) {
                    vehicle_bindings.set_delayed_velocity_restore = delayed_restore;
                    vehicle_delayed_velocity_restore_failure_reason.clear();
                }
            }

            if (vehicle_bindings.speed_kmh.function != 0) {
                alignas(8) std::array<std::uint8_t, 8> speed_bytes{};
                if (InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                        vehicle_bindings.speed_kmh.function, speed_bytes.data(),
                        vehicle_bindings.speed_kmh.parms_size)) {
                    float speed{};
                    std::memcpy(&speed, speed_bytes.data() + vehicle_bindings.speed_kmh.return_offset,
                        sizeof(speed));
                    if (std::isfinite(speed)) vehicle_speed_kmh = speed;
                }
            }
            if (vehicle_bindings.get_external_torque_ratio.function != 0) {
                alignas(8) std::array<std::uint8_t, 8> torque_bytes{};
                if (InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                        vehicle_bindings.get_external_torque_ratio.function, torque_bytes.data(),
                        vehicle_bindings.get_external_torque_ratio.parms_size)) {
                    float ratio{};
                    std::memcpy(&ratio, torque_bytes.data() +
                        vehicle_bindings.get_external_torque_ratio.return_offset, sizeof(ratio));
                    if (std::isfinite(ratio) && ratio > 0.0F && ratio <= 10000.0F)
                        vehicle_engine_torque_ratio = ratio;
                }
            }

            // Historical fallback: SetMaxEngineTorque scales the original MaxTorque field.
            // This path is only used when the dump-declared HTVehicleMovementComponent
            // SetExternalTorqueRatio API did not validate. Both layout numbers come from
            // the supplied 5.6.1-0+UE5-HT dump, and the baseline is cached per component.
            // Keep the original MaxTorque baseline ready whenever the alternate API exists.
            // The primary API can resolve correctly but still fail at runtime; in that case the
            // fallback must not depend on whether the primary binding was found.
            if (vehicle_bindings.set_max_engine_torque.function != 0 &&
                (movement_component != vehicle_base_torque_component ||
                    !vehicle_base_engine_torque_valid)) {
                vehicle_base_torque_component = movement_component;
                const auto baseline_offset = layout.vehicle_maxEngineTorque;
                float baseline{};
                if (baseline_offset >= 0 && ReadValue(*memory,
                        movement_component + static_cast<std::uintptr_t>(baseline_offset), baseline) &&
                    std::isfinite(baseline) && baseline > 0.0F) {
                    vehicle_base_engine_torque = baseline;
                    vehicle_base_engine_torque_valid = true;
                } else {
                    vehicle_base_engine_torque_valid = false;
                }
            }
            return true;
        } catch (...) {
            vehicle_valid = false;
            current_vehicle_object = 0;
            vehicle_movement_component = 0;
            return false;
        }
    }

    AnomalyStatusV1 VehicleSnapshot(nte_vehicle::StateSnapshot* snapshot) {
        if (!snapshot) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, TextLocalized("backend.vehicle_snapshot_must_run_on_game_thread_59", "vehicle snapshot must run on Game thread"));
        std::scoped_lock lock(mutex);
        *snapshot = {};
        if (!RefreshVehicleLocked())
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.current_driving_vehicle_is_unavailable_60", "current driving vehicle is unavailable"));
        snapshot->flags = kSnapshotValid;
        if (vehicle_is_water) {
            if (vehicle_bindings.water_speed_kmh.function) snapshot->flags |= kHasSpeed;
            if (vehicle_bindings.set_water_top_speed_limit.function) snapshot->flags |= kHasTopSpeedRatio;
        } else {
            if (vehicle_bindings.speed_kmh.function) snapshot->flags |= kHasSpeed;
            if (vehicle_bindings.set_top_speed_ratio.function) snapshot->flags |= kHasTopSpeedRatio;
            if (vehicle_bindings.set_wheel_friction.function) snapshot->flags |= kHasWheelFriction;
            if (vehicle_bindings.set_external_torque_ratio.function ||
                (vehicle_bindings.set_max_engine_torque.function && vehicle_base_engine_torque_valid))
                snapshot->flags |= kHasEngineTorqueRatio;
        }
        if (EnsureVehicleCatalogLocked()) snapshot->flags |= kHasSummon;
        snapshot->speed_kmh = vehicle_speed_kmh;
        snapshot->top_speed_ratio = vehicle_top_speed_ratio;
        snapshot->wheel_friction_enabled = vehicle_wheel_friction_enabled;
        snapshot->engine_torque_ratio = vehicle_engine_torque_ratio;
        snapshot->vehicle_entity_id = static_cast<std::uint32_t>(current_vehicle_handle.id);
        std::uintptr_t class_object{};
        if (ReadPointerAt(*memory, current_vehicle_object, layout.object_class, class_object))
            ReadReflectedObjectNameLocked(class_object, snapshot->vehicle_class_name);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    // Dump-backed fields written by Anomaly's readable speed path. The binary uses
    // the input speed factor to derive an absolute km/h target, then computes the
    // actual TopSpeedRatio relative to the vehicle's original TopSpeedInKmH.

    [[nodiscard]] bool ReadVehicleSpeedFieldSnapshotLocked(
        const std::uintptr_t movement,
        VehicleSpeedFieldBaseline& snapshot) {
        try {
            snapshot = {};
            const auto component_size = layout.vehicle_movementComponentSize;
            if (component_size <= 0 || component_size > 65536 || movement == 0 ||
                !ReadableRange(*memory, movement, static_cast<std::size_t>(component_size))) return false;
            const auto read_float = [&](const std::string_view key, float& value) {
                const auto offset = layout.VehicleField(key);
                std::uintptr_t address{};
                if (offset < 0 ||
                    offset > static_cast<std::int64_t>(component_size) -
                        static_cast<std::int64_t>(sizeof(float)) ||
                    !AddAddress(movement, offset, address) ||
                    !ReadValue(*memory, address, value) ||
                    !std::isfinite(value) || value < 0.0F || value > 1000000000.0F) return false;
                return true;
            };
            if (!read_float("vehicle.maxLinearVelocity", snapshot.max_linear_velocity) ||
                !read_float("vehicle.topSpeedInKmH", snapshot.top_speed_kmh) ||
                !read_float("vehicle.reverseTopSpeedInKmH", snapshot.reverse_top_speed_kmh) ||
                !read_float("vehicle.topSpeedRatio", snapshot.top_speed_ratio) ||
                !read_float("vehicle.maxEngineTorque", snapshot.engine_max_torque) ||
                !read_float("vehicle.maxEngineRPM", snapshot.engine_max_rpm) ||
                !read_float("vehicle.comfortMaxEngineTorque", snapshot.comfort_max_engine_torque) ||
                !read_float("vehicle.comfortMaxEngineRPM", snapshot.comfort_max_engine_rpm) ||
                !read_float("vehicle.comfortTopSpeedInKmH", snapshot.comfort_top_speed_kmh)) {
                return false;
            }
            const auto enabled_offset = layout.vehicle_enableTopSpeedLimit;
            std::uintptr_t enabled_address{};
            if (enabled_offset < 0 ||
                enabled_offset >= static_cast<std::int64_t>(component_size) ||
                !AddAddress(movement, enabled_offset, enabled_address) ||
                !ReadValue(*memory, enabled_address, snapshot.enable_top_speed_limit) ||
                snapshot.enable_top_speed_limit > 1U) {
                return false;
            }
            snapshot.valid = true;
            return true;
        } catch (...) {
            snapshot = {};
            return false;
        }
    }

    [[nodiscard]] bool WriteVehicleSpeedFieldSnapshotLocked(
        const std::uintptr_t movement,
        const VehicleSpeedFieldBaseline& values,
        std::string& failure_reason) {
        try {
            failure_reason.clear();
            const auto component_size = layout.vehicle_movementComponentSize;
            if (component_size <= 0 || component_size > 65536 || !values.valid || movement == 0 ||
                !ReadableRange(*memory, movement, static_cast<std::size_t>(component_size))) {
                failure_reason = TextLocalized("backend.invalid_vehicle_speed_snapshot_or_movement_component_61", "Invalid vehicle speed snapshot or movement component layout.");
                return false;
            }
            const auto write_float = [&](const std::string_view key, const float value) {
                const auto offset = layout.VehicleField(key);
                std::uintptr_t address{};
                if (offset < 0 ||
                    offset > static_cast<std::int64_t>(component_size) -
                        static_cast<std::int64_t>(sizeof(float)) ||
                    !std::isfinite(value) || value < 0.0F || value > 1000000000.0F ||
                    !AddAddress(movement, offset, address) ||
                    !memory->Write(address, &value, sizeof(value))) {
                    failure_reason = LocalizedStatus("backend.could_not_write_speed_field_0_25", "Could not write speed field: {0}", {std::string(key)});
                    return false;
                }
                float readback{};
                if (!ReadValue(*memory, address, readback) || !std::isfinite(readback) ||
                    std::abs(readback - value) > (std::max)(0.001F, std::abs(value) * 0.0001F)) {
                    failure_reason = LocalizedStatus("backend.speed_field_readback_failed_0_24", "Speed field readback failed: {0}", {std::string(key)});
                    return false;
                }
                return true;
            };
            if (!write_float("vehicle.maxEngineTorque", values.engine_max_torque) ||
                !write_float("vehicle.maxEngineRPM", values.engine_max_rpm) ||
                !write_float("vehicle.maxLinearVelocity", values.max_linear_velocity) ||
                !write_float("vehicle.topSpeedInKmH", values.top_speed_kmh) ||
                !write_float("vehicle.reverseTopSpeedInKmH", values.reverse_top_speed_kmh) ||
                !write_float("vehicle.topSpeedRatio", values.top_speed_ratio) ||
                !write_float("vehicle.comfortMaxEngineTorque", values.comfort_max_engine_torque) ||
                !write_float("vehicle.comfortMaxEngineRPM", values.comfort_max_engine_rpm) ||
                !write_float("vehicle.comfortTopSpeedInKmH", values.comfort_top_speed_kmh)) {
                return false;
            }
            const auto enabled_offset = layout.vehicle_enableTopSpeedLimit;
            std::uintptr_t enabled_address{};
            if (enabled_offset < 0 ||
                enabled_offset >= static_cast<std::int64_t>(component_size) ||
                !AddAddress(movement, enabled_offset, enabled_address) ||
                !memory->Write(enabled_address, &values.enable_top_speed_limit,
                    sizeof(values.enable_top_speed_limit))) {
                failure_reason = TextLocalized("backend.could_not_write_the_speed_limiter_switch_62", "Could not write the speed limiter switch.");
                return false;
            }
            std::uint8_t enabled_readback{};
            if (!ReadValue(*memory, enabled_address, enabled_readback) ||
                enabled_readback != values.enable_top_speed_limit) {
                failure_reason = TextLocalized("backend.speed_limiter_switch_readback_failed_63", "Speed limiter switch readback failed.");
                return false;
            }
            return true;
        } catch (...) {
            try { failure_reason = TextLocalized("backend.an_exception_occurred_while_writing_vehicle_speed_64", "An exception occurred while writing vehicle speed fields."); } catch (...) {}
            return false;
        }
    }

    [[nodiscard]] bool MakeVehicleSpeedFieldTargets(
        const VehicleSpeedFieldBaseline& baseline,
        const float requested_top_speed_kmh,
        const float minimum_top_speed_kmh,
        VehicleSpeedFieldBaseline& targets) const {
        // The caller supplies an absolute target speed. The plugin derives the native ratio from
        // the stock speed captured from this vehicle and validates the resulting operation.
        constexpr float kAnomalyMinValidField = 0.0F;
        constexpr float kAnomalyDefaultTorque = 1000.0F;
        constexpr float kAnomalyDefaultRpm = 6000.0F;
        constexpr float kMinimumEffectiveRatio = 0.0000001F;
        constexpr float kMaximumEffectiveRatio = 2000.0F;
        if (!baseline.valid || !std::isfinite(requested_top_speed_kmh) ||
            requested_top_speed_kmh <= 0.0F || !std::isfinite(minimum_top_speed_kmh) ||
            minimum_top_speed_kmh < 0.0F || minimum_top_speed_kmh > requested_top_speed_kmh ||
            !std::isfinite(baseline.top_speed_kmh) ||
            baseline.top_speed_kmh <= kAnomalyMinValidField) return false;

        const float target_speed_kmh = (std::max)(requested_top_speed_kmh, minimum_top_speed_kmh);
        const float baseline_speed_kmh = baseline.top_speed_kmh;
        const float speed_ratio = target_speed_kmh / baseline_speed_kmh;
        if (!std::isfinite(target_speed_kmh) || !std::isfinite(speed_ratio) ||
            speed_ratio < kMinimumEffectiveRatio || speed_ratio > kMaximumEffectiveRatio) return false;

        const float base_engine_torque = baseline.engine_max_torque > kAnomalyMinValidField
            ? baseline.engine_max_torque : kAnomalyDefaultTorque;
        const float base_engine_rpm = baseline.engine_max_rpm > kAnomalyMinValidField
            ? baseline.engine_max_rpm : kAnomalyDefaultRpm;
        const float base_comfort_torque = baseline.comfort_max_engine_torque > kAnomalyMinValidField
            ? baseline.comfort_max_engine_torque : base_engine_torque;
        const float base_comfort_rpm = baseline.comfort_max_engine_rpm > kAnomalyMinValidField
            ? baseline.comfort_max_engine_rpm : base_engine_rpm;
        const float torque_scale = (std::max)(speed_ratio * 2.0F, 2.0F);

        targets = baseline;
        targets.max_linear_velocity = target_speed_kmh / 3.6F;
        targets.top_speed_kmh = target_speed_kmh;
        targets.reverse_top_speed_kmh = target_speed_kmh;
        targets.top_speed_ratio = speed_ratio;
        targets.engine_max_torque = base_engine_torque * torque_scale;
        targets.engine_max_rpm = base_engine_rpm * speed_ratio;
        targets.comfort_max_engine_torque = base_comfort_torque * torque_scale;
        targets.comfort_max_engine_rpm = base_comfort_rpm * speed_ratio;
        targets.comfort_top_speed_kmh = target_speed_kmh;
        targets.enable_top_speed_limit = 1U;
        targets.valid = std::ranges::all_of(
            std::array<float, 9>{
                targets.max_linear_velocity, targets.top_speed_kmh, targets.reverse_top_speed_kmh,
                targets.top_speed_ratio, targets.engine_max_torque, targets.engine_max_rpm,
                targets.comfort_max_engine_torque, targets.comfort_max_engine_rpm,
                targets.comfort_top_speed_kmh},
            [](const float value) {
                return std::isfinite(value) && value >= 0.0F && value <= 1000000000.0F;
            });
        return targets.valid;
    }

    // UHTVehicleMovementComponent recomputes its speed state, so a raised cap is reverted on a
    // later tick and the car falls back to its stock top speed. Re-assert the applied targets
    // whenever the observed fields no longer cover them.
    void MaintainVehicleSpeedFieldsLocked() {
        if (!vehicle_speed_maintenance.Armed()) return;
        if (!RefreshVehicleLocked()) {
            vehicle_speed_maintenance.Reset();
            vehicle_speed_fields_applied = false;
            return;
        }
        if (!vehicle_speed_fields_applied || vehicle_movement_component == 0) return;
        const std::uintptr_t movement = vehicle_movement_component;
        std::string failure;
        const auto result = vehicle_speed_maintenance.Maintain(
            [this, movement](VehicleSpeedFieldBaseline& observed) {
                return ReadVehicleSpeedFieldSnapshotLocked(movement, observed);
            },
            [this, movement](const VehicleSpeedFieldBaseline& targets,
                             std::string& write_failure) {
                return WriteVehicleSpeedFieldSnapshotLocked(movement, targets, write_failure);
            },
            failure);
        if (result == nte_vehicle_helpers::SpeedCapMaintenanceResult::WriteFailed) {
            vehicle_speed_fields_applied = false;
            vehicle_speed_restore_status = LocalizedStatus("backend.the_game_reset_the_speed_cap_and_23", "The game reset the speed cap and reapplying it failed: {0}", {failure});
        }
    }

    // Dump 5.6.1: HTWheeledVehicleDrivable::RepairSelf(bool RefreshDisplay). Resolve the live
    // current vehicle on every call; never reuse a previous vehicle's object.
    AnomalyStatusV1 VehicleRepairCurrentDurability() {
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, TextLocalized("backend.vehicle_repair_must_run_on_game_thread_68", "vehicle repair must run on Game thread"));

        std::scoped_lock lock(mutex);
        if (!RefreshVehicleLocked())
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.current_driving_vehicle_is_unavailable_60", "current driving vehicle is unavailable"));
        if (vehicle_is_water)
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.dump_5_6_1_does_not_expose_69", "Dump 5.6.1 does not expose a validated water-vehicle repair entry point"));
        if (current_vehicle_object == 0 ||
            !ObjectClassChainContainsLocked(current_vehicle_object, "HTWheeledVehicleDrivable"))
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.current_vehicle_is_not_a_validated_htwheeledvehicledrivable_70", "current vehicle is not a validated HTWheeledVehicleDrivable"));

        auto& binding = vehicle_bindings.repair_self;
        if (binding.function == 0) {
            static constexpr std::array<std::string_view, 1> drivable_outer{
                "HTWheeledVehicleDrivable"};
            static_cast<void>(FindVehicleFunctionLocked(
                "RepairSelf", drivable_outer, "BoolInput", binding));
        }
        const nte_vehicle_helpers::DurabilityRepairBinding repair{
            binding.function, binding.parms_size, binding.parameter_offset, binding.has_return};
        std::string failure;
        if (!nte_vehicle_helpers::InvokeDurabilityRepair(repair, current_vehicle_object,
                [this](std::uintptr_t object, std::uintptr_t function, void* parms,
                       std::size_t size) {
                    return InvokeProcessEventGuarded(
                        process_event_invoker, object, function, parms, size);
                }, failure)) {
            return Status(nte_vehicle_helpers::DurabilityRepairBindingValid(repair)
                    ? ANOMALY_STATUS_V1_FAILED : ANOMALY_STATUS_V1_UNAVAILABLE,
                Remember(failure));
        }
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleSetTopSpeedTargetValue(
        const float value,
        const bool value_is_ratio,
        const float minimum_top_speed_kmh) {
        if (value_is_ratio) {
            if (!std::isfinite(value) || value < 0.05F || value > 100.0F)
                return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                    TextLocalized("backend.compatibility_top_speed_ratio_must_be_0_71", "top speed ratio must be 0.05..100.0"));
        } else if (!std::isfinite(value) || value <= 0.0F ||
            !std::isfinite(minimum_top_speed_kmh) || minimum_top_speed_kmh < 0.0F ||
            minimum_top_speed_kmh > value) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.absolute_top_speed_target_and_minimum_must_72", "absolute top speed target and minimum must be finite, positive, and ordered"));
        }
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, TextLocalized("backend.vehicle_mutation_must_run_on_game_thread_73", "vehicle mutation must run on Game thread"));
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, TextLocalized("backend.current_driving_vehicle_is_unavailable_60", "current driving vehicle is unavailable"));
        if (vehicle_is_water) {
            if (value_is_ratio) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                    TextLocalized("backend.water_vehicles_require_an_absolute_speed_target_74", "water vehicles require an absolute speed target"));
            }
            const auto& water_limit = vehicle_bindings.set_water_top_speed_limit;
            if (water_limit.function == 0 || water_limit.parms_size == 0 ||
                water_limit.parms_size > 8U ||
                water_limit.parameter_offset + sizeof(float) > water_limit.parms_size) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                    TextLocalized("backend.htgame_htwatervehiclebasepawn_setdynamictopspeedlimit_float_failed_dump_abi_75", "HTGame.HTWaterVehicleBasePawn.SetDynamicTopSpeedLimit(float) failed dump ABI validation"));
            }
            alignas(8) std::array<std::uint8_t, 8> parameters{};
            std::memcpy(parameters.data() + water_limit.parameter_offset,
                &value, sizeof(value));
            if (!InvokeProcessEventGuarded(process_event_invoker, current_vehicle_object,
                    water_limit.function, parameters.data(), water_limit.parms_size)) {
                return Status(ANOMALY_STATUS_V1_FAILED,
                    TextLocalized("backend.htwatervehiclebasepawn_setdynamictopspeedlimit_processevent_failed_76", "HTWaterVehicleBasePawn.SetDynamicTopSpeedLimit ProcessEvent failed"));
            }
            vehicle_current_velocity_ratio = 1.0F;
            vehicle_top_speed_ratio = 1.0F;
            return Status(ANOMALY_STATUS_V1_OK,
                TextLocalized("backend.htwatervehiclebasepawn_setdynamictopspeedlimit_applied_the_requested_km_h_77", "HTWaterVehicleBasePawn.SetDynamicTopSpeedLimit applied the requested km/h target"));
        }
        if (vehicle_bindings.set_top_speed_ratio.function == 0)
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.htgame_htwheeledvehiclebase_settopspeedratio_float_failed_dump_abi_78", "HTGame.HTWheeledVehicleBase.SetTopSpeedRatio(float) failed dump ABI validation"));
        float previous_ratio = vehicle_current_velocity_ratio;
        float previous_top_speed_ratio = vehicle_top_speed_ratio;
        const bool previous_fields_applied = vehicle_speed_fields_applied;
        VehicleSpeedFieldBaseline current_fields_before{};
        if (!ReadVehicleSpeedFieldSnapshotLocked(vehicle_movement_component, current_fields_before)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.original_movement_speed_fields_failed_layout_or_79", "Original movement speed fields failed layout or value validation."));
        }
        if (vehicle_speed_field_baseline_component != vehicle_movement_component ||
            !vehicle_speed_field_baseline.valid) {
            vehicle_speed_field_baseline = current_fields_before;
            // If the native torque fallback baseline was captured before this speed change,
            // keep that original value; otherwise this first snapshot is the baseline.
            if (vehicle_base_torque_component == vehicle_movement_component &&
                vehicle_base_engine_torque_valid) {
                vehicle_speed_field_baseline.engine_max_torque = vehicle_base_engine_torque;
            }
            vehicle_speed_field_baseline_component = vehicle_movement_component;
            const float baseline_ratio = current_fields_before.top_speed_ratio > 0.0F &&
                current_fields_before.top_speed_ratio <= 2000.0F
                ? current_fields_before.top_speed_ratio : 1.0F;
            vehicle_top_speed_ratio = baseline_ratio;
            vehicle_current_velocity_ratio = baseline_ratio;
            previous_top_speed_ratio = baseline_ratio;
            previous_ratio = baseline_ratio;
            if (vehicle_base_torque_component != vehicle_movement_component ||
                !vehicle_base_engine_torque_valid) {
                vehicle_base_torque_component = vehicle_movement_component;
                vehicle_base_engine_torque = vehicle_speed_field_baseline.engine_max_torque;
                vehicle_base_engine_torque_valid = vehicle_base_engine_torque > 0.0F &&
                    std::isfinite(vehicle_base_engine_torque);
            }
        }
        if (!std::isfinite(previous_ratio) || previous_ratio <= 0.0F || previous_ratio > 2000.0F ||
            !std::isfinite(previous_top_speed_ratio) || previous_top_speed_ratio <= 0.0F ||
            previous_top_speed_ratio > 2000.0F) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.tracked_current_velocity_multiplier_is_invalid_refusing_80", "tracked current-velocity multiplier is invalid; refusing a partial update"));
        }

        // Always apply the known-native limit first. Failure to read movement velocity
        // must not disable the separate SetTopSpeedRatio cap that can work on its own.
        std::array<std::uint8_t, 8> top_speed_parameters{};
        // Derive the effective native TopSpeedRatio from the captured stock limits.
        // Ratio targets use each vehicle's stock speed; absolute targets are in km/h.
        const float requested_top_speed_kmh = value_is_ratio
            ? vehicle_speed_field_baseline.top_speed_kmh * value : value;
        const float requested_minimum_kmh = value_is_ratio ? 0.0F : minimum_top_speed_kmh;
        VehicleSpeedFieldBaseline field_targets{};
        std::string field_failure;
        if (!MakeVehicleSpeedFieldTargets(vehicle_speed_field_baseline,
                requested_top_speed_kmh, requested_minimum_kmh, field_targets)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.speed_target_produces_an_invalid_effective_topspeedratio_81", "speed target produces an invalid effective TopSpeedRatio for this vehicle"));
        }
        std::memcpy(top_speed_parameters.data() +
                vehicle_bindings.set_top_speed_ratio.parameter_offset,
            &field_targets.top_speed_ratio, sizeof(field_targets.top_speed_ratio));
        if (!InvokeProcessEventGuarded(process_event_invoker, current_vehicle_object,
                vehicle_bindings.set_top_speed_ratio.function, top_speed_parameters.data(),
                vehicle_bindings.set_top_speed_ratio.parms_size)) {
            return Status(ANOMALY_STATUS_V1_FAILED, TextLocalized("backend.settopspeedratio_processevent_failed_82", "SetTopSpeedRatio ProcessEvent failed"));
        }
        if (vehicle_engine_torque_fallback_active) {
            field_targets.engine_max_torque *= vehicle_engine_torque_fallback_ratio;
            if (!std::isfinite(field_targets.engine_max_torque)) {
                return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                    TextLocalized("backend.anomaly_engine_torque_target_overflow_after_applying_83", "Anomaly engine torque target overflow after applying the preserved fallback multiplier"));
            }
        }
        vehicle_top_speed_ratio = field_targets.top_speed_ratio;
        if (!WriteVehicleSpeedFieldSnapshotLocked(vehicle_movement_component, field_targets, field_failure)) {
            // Best-effort rollback of the native ratio and every field we touched this call.
            std::array<std::uint8_t, 8> rollback_parameters{};
            std::memcpy(rollback_parameters.data() +
                    vehicle_bindings.set_top_speed_ratio.parameter_offset,
                &previous_top_speed_ratio, sizeof(previous_top_speed_ratio));
            static_cast<void>(InvokeProcessEventGuarded(process_event_invoker, current_vehicle_object,
                vehicle_bindings.set_top_speed_ratio.function, rollback_parameters.data(),
                vehicle_bindings.set_top_speed_ratio.parms_size));
            std::string rollback_failure;
            const bool rollback_ok = WriteVehicleSpeedFieldSnapshotLocked(
                vehicle_movement_component, current_fields_before, rollback_failure);
            vehicle_top_speed_ratio = previous_top_speed_ratio;
            vehicle_speed_fields_applied = rollback_ok ? previous_fields_applied : false;
            // The armed targets no longer describe the component after a failed apply.
            vehicle_speed_maintenance.Reset();
            vehicle_speed_restore_status =
                std::string(TextLocalized("backend.settopspeedratio_succeeded_but_writing_vehicle_speed_fields_84", "SetTopSpeedRatio succeeded, but writing vehicle speed fields failed: ")) +
                (field_failure.empty() ? TextLocalized("backend.invalid_speed_field_targets_85", "Invalid speed field targets.") : field_failure) +
                (rollback_ok ? TextLocalized("backend.fields_restored_86", "; fields restored") : LocalizedStatus("backend.field_restoration_was_incomplete_0_22", "; field restoration was incomplete{0}", {rollback_failure}));
            return Status(ANOMALY_STATUS_V1_FAILED, vehicle_speed_restore_status.c_str());
        }

        bool engine_torque_refreshed = false;
        std::string engine_torque_refresh_failure;
        const auto& max_torque_binding = vehicle_bindings.set_max_engine_torque;
        if (max_torque_binding.function != 0 && max_torque_binding.parms_size != 0 &&
            max_torque_binding.parms_size <= 0x1000U &&
            max_torque_binding.parameter_offset + sizeof(float) <= max_torque_binding.parms_size) {
            std::vector<std::uint8_t> torque_parameters(max_torque_binding.parms_size);
            std::memcpy(torque_parameters.data() + max_torque_binding.parameter_offset,
                &field_targets.engine_max_torque, sizeof(field_targets.engine_max_torque));
            engine_torque_refreshed = InvokeProcessEventGuarded(process_event_invoker,
                vehicle_movement_component, max_torque_binding.function,
                torque_parameters.data(), torque_parameters.size());
            if (!engine_torque_refreshed)
                engine_torque_refresh_failure = TextLocalized("backend.setmaxenginetorque_processevent_failed_87", "SetMaxEngineTorque ProcessEvent failed.");
        } else {
            engine_torque_refresh_failure = TextLocalized("backend.setmaxenginetorque_parameter_layout_is_unavailable_88", "SetMaxEngineTorque parameter layout is unavailable.");
        }
        // Anomaly calls SetMaxEngineTorque after changing the engine setup. Retain the alternate
        // native ratio path if that call fails, combining it with the user's independent torque
        // factor rather than multiplying a previous effective speed ratio a second time.
        vehicle_speed_fields_applied = true;
        // Keep the written targets so the Game tick can re-assert them after the movement
        // component rebuilds its speed state from the data table.
        vehicle_speed_maintenance.Arm(field_targets, field_targets.top_speed_ratio);
        if (!engine_torque_refreshed) {
            const auto& external_binding = vehicle_bindings.set_external_torque_ratio;
            const float speed_torque_scale = (std::max)(2.0F * field_targets.top_speed_ratio, 2.0F);
            const float requested_external_ratio =
                speed_torque_scale * vehicle_requested_engine_torque_ratio;
            if (external_binding.function != 0 && external_binding.parms_size != 0 &&
                external_binding.parms_size <= 0x1000U &&
                external_binding.parameter_offset + sizeof(float) <= external_binding.parms_size &&
                std::isfinite(requested_external_ratio) && requested_external_ratio > 0.0F) {
                std::vector<std::uint8_t> torque_parameters(external_binding.parms_size);
                std::memcpy(torque_parameters.data() + external_binding.parameter_offset,
                    &requested_external_ratio, sizeof(requested_external_ratio));
                engine_torque_refreshed = InvokeProcessEventGuarded(process_event_invoker,
                    vehicle_movement_component, external_binding.function,
                    torque_parameters.data(), torque_parameters.size());
                if (engine_torque_refreshed) {
                    vehicle_engine_torque_fallback_active = true;
                    vehicle_engine_torque_fallback_ratio = vehicle_requested_engine_torque_ratio;
                } else {
                    engine_torque_refresh_failure += TextLocalized("backend.setexternaltorqueratio_processevent_failed_89", "; SetExternalTorqueRatio ProcessEvent failed");
                }
            } else {
                engine_torque_refresh_failure += TextLocalized("backend.setexternaltorqueratio_parameter_layout_is_unavailable_90", "; SetExternalTorqueRatio parameter layout is unavailable");
            }
        }

        if (vehicle_bindings.set_delayed_velocity_restore == 0) {
            vehicle_speed_restore_status =
                LocalizedStatus("backend.speed_caps_and_engine_fields_applied_but_21", "Speed caps and engine fields applied, but restoring current velocity failed: {0}", {(vehicle_delayed_velocity_restore_failure_reason.empty()
                    ? std::string(TextLocalized("backend.reflection_validation_failed_or_the_vehicle_class_91", "Reflection validation failed or the vehicle class is not loaded."))
                    : vehicle_delayed_velocity_restore_failure_reason)});
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_speed_restore_status.c_str());
        }

        // Read the current native Velocity FVector. The write is submitted through
        // SetDelayedVelocityRestore (the Anomaly path), not a raw property write that Chaos
        // can immediately overwrite. Velocity is 3 doubles (0x18) in the supplied dump.
        std::uintptr_t movement_class{};
        ReflectedPropertyInfo velocity_property;
        if (!ReadPointerAt(*memory, vehicle_movement_component,
                layout.object_class, movement_class) ||
            !FindHeldPropertyLocked(movement_class, "Velocity", velocity_property) ||
            velocity_property.type != "StructProperty" ||
            velocity_property.element_size != 0x18 ||
            velocity_property.offset < 0 ||
            velocity_property.structure == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.speed_cap_applied_velocity_property_failed_vector_92", "Speed cap applied; Velocity property failed Vector validation."));
        }
        std::string velocity_struct_name;
        if (!ReadReflectedObjectNameLocked(velocity_property.structure, velocity_struct_name) ||
            (velocity_struct_name != "Vector" && velocity_struct_name != "FVector")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.speed_cap_applied_velocity_has_an_incompatible_93", "Speed cap applied; Velocity has an incompatible structure type."));
        }
        std::array<double, 3> velocity{};
        if (!ReadValue(*memory,
                vehicle_movement_component + static_cast<std::uintptr_t>(velocity_property.offset),
                velocity) ||
            !std::ranges::all_of(velocity, [](double value) { return std::isfinite(value); })) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.speed_cap_applied_current_velocity_is_unreadable_94", "Speed cap applied; current Velocity is unreadable."));
        }

        const double velocity_scale = static_cast<double>(field_targets.top_speed_ratio) /
            static_cast<double>(previous_ratio);
        for (double& component : velocity) component *= velocity_scale;
        if (!std::ranges::all_of(velocity, [](double value) { return std::isfinite(value); })) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.speed_cap_applied_scaled_velocity_is_invalid_95", "Speed cap applied; scaled Velocity is invalid."));
        }

        // The Dump's GetForwardSpeedKmH is km/h; SetDelayedVelocityRestore's
        // InForwardSpeed is the native linear-speed unit used by Anomaly (km/h * 100 / 3.6).
        // Preserve reverse sign if the game getter exposes it. A failed optional getter
        // falls back to the signed speed magnitude inferred from the current Velocity.
        double forward_speed_cm_s{};
        bool forward_speed_valid = false;
        if (vehicle_bindings.speed_kmh.function != 0) {
            std::array<std::uint8_t, 8> speed_parameters{};
            if (InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                    vehicle_bindings.speed_kmh.function, speed_parameters.data(),
                    vehicle_bindings.speed_kmh.parms_size)) {
                float current_speed_kmh{};
                std::memcpy(&current_speed_kmh,
                    speed_parameters.data() + vehicle_bindings.speed_kmh.return_offset,
                    sizeof(current_speed_kmh));
                if (std::isfinite(current_speed_kmh)) {
                    forward_speed_cm_s = static_cast<double>(current_speed_kmh) *
                        (100.0 / 3.6) * velocity_scale;
                    forward_speed_valid = std::isfinite(forward_speed_cm_s);
                }
            }
        }
        if (!forward_speed_valid) {
            const double magnitude = std::sqrt(
                velocity[0] * velocity[0] + velocity[1] * velocity[1] +
                velocity[2] * velocity[2]);
            if (!std::isfinite(magnitude)) {
                return Status(ANOMALY_STATUS_V1_FAILED,
                    TextLocalized("backend.speed_cap_applied_could_not_calculate_forward_96", "Speed cap applied; could not calculate forward speed."));
            }
            forward_speed_cm_s = magnitude;
        }

        // Anomaly passes zero angular velocity and the desired forward speed in cm/s.
        // This function is dump-defined as three parameters, size 0x34 (52 bytes), offsets 0/0x18/0x30.
        alignas(16) std::array<std::uint8_t, 0x34> delayed_parameters{};
        std::memcpy(delayed_parameters.data(), velocity.data(), sizeof(velocity));
        constexpr std::array<double, 3> zero_angular_velocity{};
        std::memcpy(delayed_parameters.data() + 0x18,
            zero_angular_velocity.data(), sizeof(zero_angular_velocity));
        const float forward_speed = static_cast<float>(forward_speed_cm_s);
        if (!std::isfinite(forward_speed)) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.speed_cap_applied_inforwardspeed_is_out_of_97", "Speed cap applied; InForwardSpeed is out of range."));
        }
        std::memcpy(delayed_parameters.data() + 0x30, &forward_speed, sizeof(forward_speed));
        if (!InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                vehicle_bindings.set_delayed_velocity_restore, delayed_parameters.data(),
                delayed_parameters.size())) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.speed_cap_applied_setdelayedvelocityrestore_processevent_failed_98", "Speed cap applied; SetDelayedVelocityRestore ProcessEvent failed."));
        }
        vehicle_current_velocity_ratio = field_targets.top_speed_ratio;
        if (!engine_torque_refreshed) {
            vehicle_speed_restore_status = LocalizedStatus("backend.speed_fields_and_velocity_applied_but_both_20", "Speed fields and velocity applied, but both engine torque calls failed: {0}", {engine_torque_refresh_failure});
            return Status(ANOMALY_STATUS_V1_FAILED, vehicle_speed_restore_status.c_str());
        }
        return Status(ANOMALY_STATUS_V1_OK,
            TextLocalized("backend.target_speed_engine_parameters_and_current_velocity_99", "Target speed, engine parameters and current velocity applied."));
    }

    AnomalyStatusV1 VehicleSetTopSpeedRatio(float ratio) {
        return VehicleSetTopSpeedTargetValue(ratio, true, 0.0F);
    }

    AnomalyStatusV1 VehicleSetTopSpeedTarget(
        float top_speed_kmh,
        float minimum_speed_kmh) {
        return VehicleSetTopSpeedTargetValue(top_speed_kmh, false, minimum_speed_kmh);
    }

    AnomalyStatusV1 VehicleSetEngineTorqueRatio(float ratio) {
        if (!std::isfinite(ratio) || ratio < 0.05F || ratio > 20.0F)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.external_torque_ratio_must_be_0_05_100", "external torque ratio must be 0.05..20.0"));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_torque_mutation_requires_game_thread_101", "vehicle torque mutation requires Game thread"));
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleLocked() || vehicle_movement_component == 0)
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, TextLocalized("backend.current_vehicle_movement_component_is_unavailable_102", "current vehicle movement component is unavailable"));

        // Try the already-working game-native path first. A successful ProcessEvent is final;
        // never call both setters on the same request.
        const auto& external = vehicle_bindings.set_external_torque_ratio;
        std::array<std::uint8_t, 8> parameters{};
        bool external_binding_valid = external.function != 0 &&
            external.parms_size != 0 && external.parms_size <= parameters.size() &&
            external.parameter_offset + sizeof(ratio) <= external.parms_size;
        if (external_binding_valid) {
            parameters.fill(0);
            std::memcpy(parameters.data() + external.parameter_offset, &ratio, sizeof(ratio));
            if (InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                    external.function, parameters.data(), external.parms_size)) {
                vehicle_engine_torque_ratio = ratio;
                vehicle_requested_engine_torque_ratio = ratio;
                vehicle_engine_torque_fallback_active = false;
                vehicle_engine_torque_fallback_ratio = 1.0F;
                return Status(ANOMALY_STATUS_V1_OK, TextLocalized("backend.setexternaltorqueratio_applied_103", "SetExternalTorqueRatio applied"));
            }
        }

        // Fallback is also attempted after a runtime failure of SetExternalTorqueRatio, not only
        // when reflection could not find the primary function.
        const auto& fallback = vehicle_bindings.set_max_engine_torque;
        const bool fallback_binding_valid = fallback.function != 0 &&
            fallback.parms_size != 0 && fallback.parms_size <= parameters.size() &&
            fallback.parameter_offset + sizeof(float) <= fallback.parms_size;
        if (fallback_binding_valid) {
            if (vehicle_base_torque_component != vehicle_movement_component ||
                !vehicle_base_engine_torque_valid) {
                vehicle_base_torque_component = vehicle_movement_component;
                const auto baseline_offset = layout.vehicle_maxEngineTorque;
                float baseline{};
                if (baseline_offset >= 0 && ReadValue(*memory,
                        vehicle_movement_component + static_cast<std::uintptr_t>(baseline_offset), baseline) &&
                    std::isfinite(baseline) && baseline > 0.0F) {
                    vehicle_base_engine_torque = baseline;
                    vehicle_base_engine_torque_valid = true;
                } else {
                    vehicle_base_engine_torque_valid = false;
                }
            }
            if (vehicle_base_engine_torque_valid) {
                const bool anomaly_speed_fields_active = vehicle_speed_fields_applied &&
                    vehicle_speed_field_baseline_component == vehicle_movement_component &&
                    vehicle_speed_field_baseline.valid;
                const float speed_torque_scale = anomaly_speed_fields_active
                    ? (std::max)(2.0F * vehicle_top_speed_ratio, 2.0F) : 1.0F;
                const float torque = vehicle_base_engine_torque * ratio * speed_torque_scale;
                if (std::isfinite(torque)) {
                    parameters.fill(0);
                    std::memcpy(parameters.data() + fallback.parameter_offset, &torque, sizeof(torque));
                    if (InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                            fallback.function, parameters.data(), fallback.parms_size)) {
                        vehicle_engine_torque_ratio = ratio;
                        vehicle_requested_engine_torque_ratio = ratio;
                        vehicle_engine_torque_fallback_active = true;
                        vehicle_engine_torque_fallback_ratio = ratio;
                        return Status(ANOMALY_STATUS_V1_OK,
                            external_binding_valid
                                ? TextLocalized("backend.setexternaltorqueratio_failed_setmaxenginetorque_fallback_applied_104", "SetExternalTorqueRatio failed; SetMaxEngineTorque fallback applied")
                                : TextLocalized("backend.setmaxenginetorque_fallback_applied_105", "SetMaxEngineTorque fallback applied"));
                    }
                }
            }
            return Status(ANOMALY_STATUS_V1_FAILED,
                external_binding_valid
                    ? TextLocalized("backend.setexternaltorqueratio_and_setmaxenginetorque_both_failed_106", "SetExternalTorqueRatio and SetMaxEngineTorque both failed")
                    : TextLocalized("backend.setmaxenginetorque_fallback_failed_107", "SetMaxEngineTorque fallback failed"));
        }
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            external_binding_valid
                ? TextLocalized("backend.setexternaltorqueratio_failed_and_setmaxenginetorque_fallback_is_unavailable_108", "SetExternalTorqueRatio failed and SetMaxEngineTorque fallback is unavailable")
                : TextLocalized("backend.both_torque_setter_paths_are_unavailable_109", "both torque setter paths are unavailable"));
    }


    // 从当前 World 已加载的所有 Level 枚举实时 Actor 实例；它与 DT_VehicleData 目录分离，因为目录行是
    // 召唤定义，不是场景实例。UE 对象指针仅在插件内部使用，批量修改只在 Game 线程执行。
    [[nodiscard]] bool CollectOtherVehicleActorsLocked(
        std::vector<std::uintptr_t>& actors,
        std::size_t& unreadable_slots,
        std::string& failure_reason) {
        try {
            actors.clear();
            unreadable_slots = 0;
            failure_reason.clear();
            const auto levels_offset = layout.world_levels;
            const auto actors_offset = layout.level_actors;
            const auto maximum_actors = layout.entities_maxCount;
            const auto maximum_levels = layout.entities_maxLevels;
            if (world_pointer == 0 || !ready ||
                levels_offset < 0 || actors_offset < 0 ||
                maximum_actors <= 0 || maximum_actors > 1000000 ||
                maximum_levels <= 0 || maximum_levels > 4096) {
                failure_reason = TextLocalized("backend.cannot_enumerate_other_vehicles_world_or_actor_110", "Cannot enumerate other vehicles: world or actor layout is unavailable.");
                return false;
            }

            std::uintptr_t levels_array{}, levels_count_address{};
            std::int32_t level_count{};
            if (!ReadPointerAt(*memory, world_pointer, levels_offset, levels_array) ||
                !AddAddress(world_pointer,
                    levels_offset + static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                    levels_count_address) ||
                !ReadValue(*memory, levels_count_address, level_count) ||
                level_count <= 0 || level_count > maximum_levels || levels_array == 0) {
                failure_reason = TextLocalized("backend.cannot_enumerate_other_vehicles_world_levels_is_111", "Cannot enumerate other vehicles: World.Levels is invalid.");
                return false;
            }

            std::vector<std::uintptr_t> levels;
            levels.reserve(static_cast<std::size_t>(level_count));
            for (std::int32_t i = 0; i < level_count; ++i) {
                std::uintptr_t slot{}, level{};
                if (!AddAddress(levels_array,
                        static_cast<std::int64_t>(i) * sizeof(std::uintptr_t), slot) ||
                    !ReadValue(*memory, slot, level)) {
                    failure_reason = LocalizedStatus("backend.cannot_read_world_levels_0_19", "Cannot read World.Levels[{0}].", {std::to_string(i)});
                    return false;
                }
                if (level != 0) levels.push_back(level);
            }
            if (levels.empty()) {
                failure_reason = TextLocalized("backend.the_current_world_contains_no_readable_levels_112", "The current world contains no readable levels.");
                return false;
            }

            std::unordered_set<std::uintptr_t> seen;
            seen.reserve(256);
            std::int64_t total_actor_slots{};
            for (const std::uintptr_t level : levels) {
                std::uintptr_t actor_array{}, count_address{};
                std::int32_t actor_count{};
                if (!ReadPointerAt(*memory, level, actors_offset, actor_array) ||
                    !AddAddress(level, actors_offset +
                        static_cast<std::int64_t>(sizeof(std::uintptr_t)), count_address) ||
                    !ReadValue(*memory, count_address, actor_count) ||
                    actor_count < 0 ||
                    actor_count > maximum_actors - total_actor_slots ||
                    (actor_count != 0 && actor_array == 0)) {
                    failure_reason = TextLocalized("backend.cannot_enumerate_other_vehicles_level_actors_is_113", "Cannot enumerate other vehicles: Level.Actors is invalid.");
                    return false;
                }
                total_actor_slots += actor_count;
                for (std::int32_t i = 0; i < actor_count; ++i) {
                    std::uintptr_t slot{}, actor{};
                    if (!AddAddress(actor_array,
                            static_cast<std::int64_t>(i) * sizeof(std::uintptr_t), slot) ||
                        !ReadValue(*memory, slot, actor)) {
                        ++unreadable_slots;
                        continue;
                    }
                    if (actor == 0 || actor == current_vehicle_object ||
                        !seen.insert(actor).second ||
                        !ReadableRange(*memory, actor, 0x20U)) continue;

                    // 仅接受真实 Actor 实例及游戏的轮式载具继承链；排除 DataTable 行、CDO、组件和仅名称匹配。
                    if (!IsVehicleActorLocked(actor) ||
                        !ObjectClassChainContainsLocked(actor, "HTWheeledVehicleBase")) continue;
                    actors.push_back(actor);
                }
            }
            return true;
        } catch (...) {
            failure_reason = TextLocalized("backend.an_exception_interrupted_vehicle_enumeration_batch_mutation_114", "An exception interrupted vehicle enumeration; batch mutation was cancelled.");
            return false;
        }
    }

    // UObject addresses can be recycled without changing the registry generation.
    // Keep batch baselines only while the object's serial still matches.
    bool TrackOtherMovement(std::uintptr_t movement) {
        AnomalyGenerationHandleV1 current{};
        if (!ObjectHandleLocked(movement, current)) return false;
        auto& previous = other_vehicle_handles[movement];
        if (!SameHandle(previous, current)) {
            other_vehicle_velocity_ratios.erase(movement);
            other_vehicle_base_torques.erase(movement);
            other_vehicle_torque_fallback_ratios.erase(movement);
            other_vehicle_requested_torque_ratios.erase(movement);
            other_vehicle_speed_field_baselines.erase(movement);
            other_vehicle_speed_field_ratios.erase(movement);
            previous = current;
        }
        return true;
    }

    [[nodiscard]] bool EnsureOtherVehicleMutationBindingsLocked(
        const bool need_speed,
        const bool need_torque) {
        try {
            if (!ready || !process_event_invoker ||
                player_controller == 0) return false;
            if (vehicle_bindings.object_generation != object_generation) {
                vehicle_bindings = {};
                vehicle_bindings.object_generation = object_generation;
                other_vehicle_velocity_ratios.clear();
                other_vehicle_base_torques.clear();
                other_vehicle_speed_field_baselines.clear();
                other_vehicle_speed_field_ratios.clear();
                other_vehicle_torque_fallback_ratios.clear();
                other_vehicle_requested_torque_ratios.clear();
            }
            // 刷新当前玩家载具，以便批量枚举时排除它；玩家未驾驶时此调用可以返回 false，
            // 后续仍会独立解析批量修改所需的函数绑定。
            static_cast<void>(RefreshVehicleLocked());
            static constexpr std::array<std::string_view, 1> vehicle_outer{"HTWheeledVehicleBase"};
            static constexpr std::array<std::string_view, 1> movement_outer{"HTVehicleMovementComponent"};
            if (need_speed && vehicle_bindings.set_top_speed_ratio.function == 0 &&
                !FindVehicleFunctionLocked("SetTopSpeedRatio", vehicle_outer, "FloatInput",
                    vehicle_bindings.set_top_speed_ratio)) return false;
            if (need_speed && vehicle_bindings.speed_kmh.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("GetForwardSpeedKmH", movement_outer,
                    "FloatReturn", vehicle_bindings.speed_kmh));
            }
            if ((need_speed || need_torque) && vehicle_bindings.set_external_torque_ratio.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("SetExternalTorqueRatio", movement_outer,
                    "FloatInput", vehicle_bindings.set_external_torque_ratio));
            }
            if ((need_speed || need_torque) && vehicle_bindings.set_max_engine_torque.function == 0) {
                static constexpr std::array<std::string_view, 1> chaos_movement_outer{
                    "ChaosWheeledVehicleMovementComponent"};
                static_cast<void>(FindVehicleFunctionLocked("SetMaxEngineTorque", chaos_movement_outer,
                    "FloatInput", vehicle_bindings.set_max_engine_torque, "ChaosVehicles"));
            }
            if (need_speed && vehicle_bindings.set_max_engine_torque.function == 0) {
                static constexpr std::array<std::string_view, 1> chaos_movement_outer{
                    "ChaosWheeledVehicleMovementComponent"};
                static_cast<void>(FindVehicleFunctionLocked("SetMaxEngineTorque", chaos_movement_outer,
                    "FloatInput", vehicle_bindings.set_max_engine_torque, "ChaosVehicles"));
            }
            if (need_speed && vehicle_bindings.set_external_torque_ratio.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("SetExternalTorqueRatio", movement_outer,
                    "FloatInput", vehicle_bindings.set_external_torque_ratio));
            }
            if (need_speed && vehicle_bindings.set_delayed_velocity_restore == 0) {
                std::uintptr_t delayed_restore{};
                if (FindExactObjectLocked(
                        L"/Script/HTGame.HTVehicleMovementComponent.SetDelayedVelocityRestore",
                        delayed_restore) &&
                    ValidateDelayedVelocityRestoreLocked(
                        delayed_restore, vehicle_delayed_velocity_restore_failure_reason)) {
                    vehicle_bindings.set_delayed_velocity_restore = delayed_restore;
                    vehicle_delayed_velocity_restore_failure_reason.clear();
                } else if (vehicle_delayed_velocity_restore_failure_reason.empty()) {
                    vehicle_delayed_velocity_restore_failure_reason =
                        TextLocalized("backend.htvehiclemovementcomponent_setdelayedvelocityrestore_failed_validation_115", "HTVehicleMovementComponent.SetDelayedVelocityRestore failed validation.");
                }
            }
            return (need_speed
                    ? vehicle_bindings.set_top_speed_ratio.function != 0
                    : true) &&
                (need_torque
                    ? vehicle_bindings.set_external_torque_ratio.function != 0 ||
                        (vehicle_bindings.set_max_engine_torque.function != 0 &&
                            layout.vehicle_maxEngineTorque >= 0)
                    : true);
        } catch (...) {
            return false;
        }
    }

    AnomalyStatusV1 VehicleSetOtherVehiclesMinSpeedRatio(float ratio) {
        if (!std::isfinite(ratio) || ratio < 0.0F || ratio > 20.0F)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.non_player_vehicle_compatibility_minimum_ratio_must_116", "non-player vehicle compatibility minimum ratio must be 0.0..20.0"));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT,
                TextLocalized("backend.non_player_vehicle_speed_floor_mutation_requires_117", "non-player vehicle speed floor mutation requires Game thread"));
        std::scoped_lock lock(mutex);
        vehicle_other_speed_multiplier_floor = ratio;
        vehicle_other_speed_status = LocalizedStatus("backend.non_player_minimum_speed_ratio_set_to_18", "Non-player minimum speed ratio set to {0}x; apply a batch target to update loaded vehicles.", {std::to_string(ratio)});
        return Status(ANOMALY_STATUS_V1_OK, vehicle_other_speed_status.c_str());
    }

    AnomalyStatusV1 VehicleSetOtherVehiclesTargetValue(
        const float value,
        const bool value_is_ratio,
        const float minimum_speed_kmh) {
        if (value_is_ratio) {
            if (!std::isfinite(value) || value < 0.05F || value > 20.0F)
                return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                    TextLocalized("backend.non_player_compatibility_speed_ratio_must_be_118", "non-player compatibility speed ratio must be 0.05..20.0"));
        } else if (!std::isfinite(value) || value <= 0.0F ||
            !std::isfinite(minimum_speed_kmh) || minimum_speed_kmh < 0.0F ||
            minimum_speed_kmh > value) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.non_player_absolute_targets_must_be_finite_119", "non-player absolute targets must be finite, positive, and ordered"));
        }
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT,
                TextLocalized("backend.non_player_vehicle_speed_mutation_requires_game_120", "non-player vehicle speed mutation requires Game thread"));

        std::scoped_lock lock(mutex);
        vehicle_other_speed_status.clear();
        if (!EnsureOtherVehicleMutationBindingsLocked(true, false)) {
            vehicle_other_speed_status =
                TextLocalized("backend.non_player_speed_mutation_requires_validated_functions_121", "Non-player speed mutation requires validated functions and a player controller.");
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_other_speed_status.c_str());
        }

        std::vector<std::uintptr_t> actors;
        std::size_t unreadable_slots{};
        std::string scan_failure;
        if (!CollectOtherVehicleActorsLocked(actors, unreadable_slots, scan_failure)) {
            vehicle_other_speed_status = scan_failure;
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_other_speed_status.c_str());
        }
        if (actors.empty()) {
            vehicle_other_speed_status = TextLocalized("backend.no_loaded_non_player_htwheeledvehiclebase_instances_were_122", "No loaded non-player HTWheeledVehicleBase instances were found.");
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, vehicle_other_speed_status.c_str());
        }

        std::size_t cap_applied{}, speed_fields_applied{}, velocity_applied{}, partial{}, failed{};
        std::string first_failure;
        const auto record_failure = [&first_failure](std::string reason) {
            if (first_failure.empty()) first_failure = std::move(reason);
        };
        const auto delayed_function = vehicle_bindings.set_delayed_velocity_restore;
        for (const std::uintptr_t actor : actors) {
            std::uintptr_t movement{};
            const auto movement_offset = layout.vehicle_movementComponent;
            if (movement_offset < 0 ||
                !ReadPointerAt(*memory, actor, movement_offset, movement) || movement == 0 ||
                !TrackOtherMovement(movement) ||
                !ObjectClassChainContainsLocked(movement, "HTVehicleMovementComponent")) {
                ++failed;
                record_failure(TextLocalized("backend.vehicle_has_no_validated_htvehiclemovementcomponent_123", "Vehicle has no validated HTVehicleMovementComponent."));
                continue;
            }

            VehicleSpeedFieldBaseline fields_before{};
            if (!ReadVehicleSpeedFieldSnapshotLocked(movement, fields_before)) {
                ++failed;
                record_failure(TextLocalized("backend.movement_speed_fields_failed_layout_or_value_124", "Movement speed fields failed layout or value validation."));
                continue;
            }
            auto baseline_it = other_vehicle_speed_field_baselines.find(movement);
            if (baseline_it == other_vehicle_speed_field_baselines.end()) {
                VehicleSpeedFieldBaseline baseline = fields_before;
                const auto torque_baseline = other_vehicle_base_torques.find(movement);
                if (torque_baseline != other_vehicle_base_torques.end())
                    baseline.engine_max_torque = torque_baseline->second;
                else
                    other_vehicle_base_torques.emplace(movement, baseline.engine_max_torque);
                baseline_it = other_vehicle_speed_field_baselines.emplace(movement, baseline).first;
            }
            VehicleSpeedFieldBaseline field_targets{};
            std::string field_failure;
            const float requested_top_speed_kmh = value_is_ratio
                ? baseline_it->second.top_speed_kmh * value : value;
            const float requested_minimum_kmh = value_is_ratio
                ? baseline_it->second.top_speed_kmh * vehicle_other_speed_multiplier_floor
                : minimum_speed_kmh;
            if (!MakeVehicleSpeedFieldTargets(baseline_it->second,
                    requested_top_speed_kmh, requested_minimum_kmh, field_targets)) {
                ++failed;
                record_failure(TextLocalized("backend.speed_target_produces_an_invalid_effective_topspeedratio_81", "speed target produces an invalid effective TopSpeedRatio for this vehicle"));
                continue;
            }
            const auto torque_fallback_it = other_vehicle_torque_fallback_ratios.find(movement);
            if (torque_fallback_it != other_vehicle_torque_fallback_ratios.end()) {
                field_targets.engine_max_torque *= torque_fallback_it->second;
                if (!std::isfinite(field_targets.engine_max_torque)) {
                    ++failed;
                    record_failure(TextLocalized("backend.anomaly_engine_torque_target_overflow_after_applying_125", "Anomaly engine torque target overflow after applying preserved fallback multiplier"));
                    continue;
                }
            }

            const auto& speed_binding = vehicle_bindings.set_top_speed_ratio;
            if (speed_binding.function == 0 || speed_binding.parms_size == 0 ||
                speed_binding.parms_size > 0x1000U ||
                speed_binding.parameter_offset + sizeof(float) > speed_binding.parms_size) {
                ++failed;
                record_failure(TextLocalized("backend.settopspeedratio_parameter_layout_is_unavailable_126", "SetTopSpeedRatio parameter layout is unavailable."));
                continue;
            }
            std::vector<std::uint8_t> cap_parameters(speed_binding.parms_size);
            std::memcpy(cap_parameters.data() + speed_binding.parameter_offset,
                &field_targets.top_speed_ratio, sizeof(field_targets.top_speed_ratio));
            if (!InvokeProcessEventGuarded(process_event_invoker, actor,
                    speed_binding.function, cap_parameters.data(), cap_parameters.size())) {
                ++failed;
                record_failure(TextLocalized("backend.htwheeledvehiclebase_settopspeedratio_processevent_failed_127", "HTWheeledVehicleBase.SetTopSpeedRatio ProcessEvent failed."));
                continue;
            }
            ++cap_applied;

            if (!WriteVehicleSpeedFieldSnapshotLocked(movement, field_targets, field_failure)) {
                std::vector<std::uint8_t> rollback_parameters(speed_binding.parms_size);
                const float prior_native_ratio = fields_before.top_speed_ratio >= 0.05F &&
                    fields_before.top_speed_ratio <= 20.0F
                    ? fields_before.top_speed_ratio : 1.0F;
                std::memcpy(rollback_parameters.data() + speed_binding.parameter_offset,
                    &prior_native_ratio, sizeof(prior_native_ratio));
                static_cast<void>(InvokeProcessEventGuarded(process_event_invoker, actor,
                    speed_binding.function, rollback_parameters.data(), rollback_parameters.size()));
                std::string rollback_failure;
                static_cast<void>(WriteVehicleSpeedFieldSnapshotLocked(
                    movement, fields_before, rollback_failure));
                ++partial;
                record_failure(field_failure.empty()
                    ? TextLocalized("backend.could_not_calculate_vehicle_speed_field_targets_128", "Could not calculate vehicle speed field targets.")
                    : LocalizedStatus("backend.could_not_write_vehicle_speed_fields_0_17", "Could not write vehicle speed fields: {0}", {field_failure}));
            } else {
                other_vehicle_speed_field_ratios[movement] = field_targets.top_speed_ratio;
                ++speed_fields_applied;

                bool engine_torque_refreshed = false;
                std::string engine_torque_refresh_failure;
                const auto& max_torque_binding = vehicle_bindings.set_max_engine_torque;
                if (max_torque_binding.function != 0 && max_torque_binding.parms_size != 0 &&
                    max_torque_binding.parms_size <= 0x1000U &&
                    max_torque_binding.parameter_offset + sizeof(float) <= max_torque_binding.parms_size) {
                    std::vector<std::uint8_t> torque_parameters(max_torque_binding.parms_size);
                    std::memcpy(torque_parameters.data() + max_torque_binding.parameter_offset,
                        &field_targets.engine_max_torque, sizeof(field_targets.engine_max_torque));
                    engine_torque_refreshed = InvokeProcessEventGuarded(process_event_invoker,
                        movement, max_torque_binding.function, torque_parameters.data(),
                        torque_parameters.size());
                    if (!engine_torque_refreshed)
                        engine_torque_refresh_failure = TextLocalized("backend.setmaxenginetorque_processevent_failed_87", "SetMaxEngineTorque ProcessEvent failed.");
                } else {
                    engine_torque_refresh_failure = TextLocalized("backend.setmaxenginetorque_parameter_layout_is_unavailable_88", "SetMaxEngineTorque parameter layout is unavailable.");
                }

                if (!engine_torque_refreshed) {
                    const auto torque_ratio_it = other_vehicle_requested_torque_ratios.find(movement);
                    const float requested_torque_ratio =
                        torque_ratio_it == other_vehicle_requested_torque_ratios.end()
                            ? 1.0F : torque_ratio_it->second;
                    const auto& external_binding = vehicle_bindings.set_external_torque_ratio;
                    const float speed_torque_scale =
                        (std::max)(2.0F * field_targets.top_speed_ratio, 2.0F);
                    const float requested_external_ratio = speed_torque_scale * requested_torque_ratio;
                    if (external_binding.function != 0 && external_binding.parms_size != 0 &&
                        external_binding.parms_size <= 0x1000U &&
                        external_binding.parameter_offset + sizeof(float) <= external_binding.parms_size &&
                        std::isfinite(requested_external_ratio) && requested_external_ratio > 0.0F) {
                        std::vector<std::uint8_t> torque_parameters(external_binding.parms_size);
                        std::memcpy(torque_parameters.data() + external_binding.parameter_offset,
                            &requested_external_ratio, sizeof(requested_external_ratio));
                        engine_torque_refreshed = InvokeProcessEventGuarded(process_event_invoker,
                            movement, external_binding.function, torque_parameters.data(),
                            torque_parameters.size());
                        if (engine_torque_refreshed) {
                            other_vehicle_torque_fallback_ratios[movement] = requested_torque_ratio;
                        } else {
                            engine_torque_refresh_failure += TextLocalized("backend.setexternaltorqueratio_processevent_failed_89", "; SetExternalTorqueRatio ProcessEvent failed");
                        }
                    } else {
                        engine_torque_refresh_failure += TextLocalized("backend.setexternaltorqueratio_parameter_layout_is_unavailable_90", "; SetExternalTorqueRatio parameter layout is unavailable");
                    }
                }
                if (!engine_torque_refreshed) {
                    ++partial;
                    record_failure(LocalizedStatus("backend.speed_fields_applied_but_both_engine_torque_16", "Speed fields applied, but both engine torque calls failed: {0}", {engine_torque_refresh_failure}));
                }
            }

            if (delayed_function == 0) {
                ++partial;
                record_failure(vehicle_delayed_velocity_restore_failure_reason.empty()
                    ? TextLocalized("backend.setdelayedvelocityrestore_failed_reflection_validation_129", "SetDelayedVelocityRestore failed reflection validation.")
                    : vehicle_delayed_velocity_restore_failure_reason);
                continue;
            }

            std::uintptr_t movement_class{};
            ReflectedPropertyInfo velocity_property;
            if (!ReadPointerAt(*memory, movement, layout.object_class, movement_class) ||
                !FindHeldPropertyLocked(movement_class, "Velocity", velocity_property) ||
                velocity_property.type != "StructProperty" || velocity_property.element_size != 0x18 ||
                velocity_property.offset < 0 || velocity_property.array_dim != 1 ||
                velocity_property.structure == 0) {
                ++partial;
                record_failure(TextLocalized("backend.velocity_property_failed_vector_validation_130", "Velocity property failed Vector validation."));
                continue;
            }
            std::string velocity_struct_name;
            if (!ReadReflectedObjectNameLocked(velocity_property.structure, velocity_struct_name) ||
                (velocity_struct_name != "Vector" && velocity_struct_name != "FVector")) {
                ++partial;
                record_failure(TextLocalized("backend.velocity_has_an_incompatible_vector_structure_type_131", "Velocity has an incompatible Vector structure type."));
                continue;
            }
            std::array<double, 3> velocity{};
            if (!ReadValue(*memory, movement +
                    static_cast<std::uintptr_t>(velocity_property.offset), velocity) ||
                !std::ranges::all_of(velocity, [](double value) { return std::isfinite(value); })) {
                ++partial;
                record_failure(TextLocalized("backend.could_not_read_the_vehicle_s_current_132", "Could not read the vehicle's current Velocity."));
                continue;
            }

            auto previous_it = other_vehicle_velocity_ratios.find(movement);
            const float baseline_ratio = baseline_it->second.top_speed_ratio > 0.0F &&
                baseline_it->second.top_speed_ratio <= 2000.0F
                ? baseline_it->second.top_speed_ratio : 1.0F;
            const float previous_ratio = previous_it == other_vehicle_velocity_ratios.end()
                ? baseline_ratio : previous_it->second;
            if (!std::isfinite(previous_ratio) || previous_ratio <= 0.0F || previous_ratio > 2000.0F) {
                ++partial;
                record_failure(TextLocalized("backend.the_movement_component_s_previous_velocity_multiplier_133", "The movement component's previous velocity multiplier is invalid."));
                continue;
            }
            const double velocity_scale = static_cast<double>(field_targets.top_speed_ratio) /
                static_cast<double>(previous_ratio);
            for (double& component : velocity) component *= velocity_scale;
            if (!std::ranges::all_of(velocity, [](double value) { return std::isfinite(value); })) {
                ++partial;
                record_failure(TextLocalized("backend.scaled_velocity_is_invalid_134", "Scaled Velocity is invalid."));
                continue;
            }

            double forward_speed_cm_s{};
            bool forward_speed_valid = false;
            const auto& getter = vehicle_bindings.speed_kmh;
            if (getter.function != 0 && getter.parms_size <= 0x1000U &&
                getter.return_offset + sizeof(float) <= getter.parms_size) {
                std::vector<std::uint8_t> speed_parameters((std::max)(static_cast<std::size_t>(getter.parms_size), std::size_t{8}));
                if (InvokeProcessEventGuarded(process_event_invoker, movement,
                        getter.function, speed_parameters.data(), getter.parms_size)) {
                    float current_speed_kmh{};
                    std::memcpy(&current_speed_kmh,
                        speed_parameters.data() + getter.return_offset, sizeof(current_speed_kmh));
                    if (std::isfinite(current_speed_kmh)) {
                        forward_speed_cm_s = static_cast<double>(current_speed_kmh) *
                            (100.0 / 3.6) * velocity_scale;
                        forward_speed_valid = std::isfinite(forward_speed_cm_s);
                    }
                }
            }
            if (!forward_speed_valid) {
                const double magnitude = std::sqrt(
                    velocity[0] * velocity[0] + velocity[1] * velocity[1] +
                    velocity[2] * velocity[2]);
                if (!std::isfinite(magnitude)) {
                    ++partial;
                    record_failure(TextLocalized("backend.could_not_calculate_the_vehicle_s_forward_135", "Could not calculate the vehicle's forward speed."));
                    continue;
                }
                forward_speed_cm_s = magnitude;
            }
            const float forward_speed = static_cast<float>(forward_speed_cm_s);
            if (!std::isfinite(forward_speed)) {
                ++partial;
                record_failure(TextLocalized("backend.inforwardspeed_is_out_of_range_136", "InForwardSpeed is out of range."));
                continue;
            }
            alignas(16) std::array<std::uint8_t, 0x34> delayed_parameters{};
            std::memcpy(delayed_parameters.data(), velocity.data(), sizeof(velocity));
            constexpr std::array<double, 3> zero_angular_velocity{};
            std::memcpy(delayed_parameters.data() + 0x18,
                zero_angular_velocity.data(), sizeof(zero_angular_velocity));
            std::memcpy(delayed_parameters.data() + 0x30, &forward_speed, sizeof(forward_speed));
            if (!InvokeProcessEventGuarded(process_event_invoker, movement,
                    delayed_function, delayed_parameters.data(), delayed_parameters.size())) {
                ++partial;
                record_failure(TextLocalized("backend.setdelayedvelocityrestore_processevent_failed_137", "SetDelayedVelocityRestore ProcessEvent failed."));
                continue;
            }
            other_vehicle_velocity_ratios[movement] = field_targets.top_speed_ratio;
            ++velocity_applied;
        }

        vehicle_other_speed_status = LocalizedStatus("backend.non_player_speed_target_0_1_found_15", "Non-player speed target {0}{1}: found {2} vehicles; speed cap applied to {3}, engine and speed fields applied to {4}, velocity restored for {5}, partially applied to {6}, failed for {7} vehicles", {std::to_string(value), (value_is_ratio ? TextLocalized("backend.x_ratio_138", "x (ratio)") : TextLocalized("backend.km_h_139", " km/h")), std::to_string(actors.size()), std::to_string(cap_applied), std::to_string(speed_fields_applied), std::to_string(velocity_applied), std::to_string(partial), std::to_string(failed)});
        if (unreadable_slots != 0)
            vehicle_other_speed_status += LocalizedStatus("backend.unreadable_actor_slots_skipped_0_9", "; unreadable actor slots skipped: {0}", {std::to_string(unreadable_slots)});
        if (!first_failure.empty()) vehicle_other_speed_status += LocalizedStatus("backend.first_failure_0_8", "; first failure: {0}", {first_failure});
        if (cap_applied == 0 || partial != 0 || failed != 0 || unreadable_slots != 0)
            return Status(ANOMALY_STATUS_V1_FAILED, vehicle_other_speed_status.c_str());
        return Status(ANOMALY_STATUS_V1_OK, vehicle_other_speed_status.c_str());
    }

    AnomalyStatusV1 VehicleSetOtherVehiclesTopSpeedRatio(float ratio) {
        return VehicleSetOtherVehiclesTargetValue(ratio, true, 0.0F);
    }

    AnomalyStatusV1 VehicleSetOtherVehiclesTopSpeedTarget(
        float top_speed_kmh,
        float minimum_speed_kmh) {
        return VehicleSetOtherVehiclesTargetValue(
            top_speed_kmh, false, minimum_speed_kmh);
    }

    [[nodiscard]] bool EnsureVehicleCustomizationCatalogLocked(
        const VehicleCatalogEntry& selected) {
        const auto fail = [this](const char* reason) {
            vehicle_customization_failure_reason = reason;
            return false;
        };
        try {
            if (!selected.vehicle_id_valid || selected.vehicle_id_comparison_index == 0) {
                return fail(TextLocalized("backend.the_selected_vehicle_has_no_valid_vehicleid_140", "The selected vehicle has no valid VehicleID for module and skin matching."));
            }
            if (!ready || object_registry.items == 0 ||
                !process_event_invoker ||
                GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire)) {
                return fail(TextLocalized("backend.customization_requires_a_valid_profile_object_registry_141", "Customization requires a valid profile, object registry, ProcessEvent and Game thread."));
            }
            if (vehicle_customization_generation == object_generation &&
                vehicle_customization_vehicle_sequence == vehicle_catalog_sequence &&
                vehicle_customization_vehicle_row_id == selected.id) {
                return true;
            }
            if (!EnsureVehicleCatalogLocked() || vehicle_data_asset_object == 0) {
                return fail(TextLocalized("backend.a_validated_htvehicledataasset_is_unavailable_142", "A validated HTVehicleDataAsset is unavailable."));
            }

            std::uintptr_t asset_class{};
            if (!ReadPointerAt(*memory, vehicle_data_asset_object,
                    layout.object_class, asset_class)) {
                return fail(TextLocalized("backend.could_not_read_the_htvehicledataasset_class_143", "Could not read the HTVehicleDataAsset class."));
            }

            const auto resolve_table = [&](const std::string_view property_name,
                    const std::string_view expected_row_name,
                    std::uintptr_t& table_out,
                    std::uintptr_t& row_struct_out,
                    SparseMapView& view_out,
                    std::vector<std::uint8_t>& elements_out) -> bool {
                ReflectedPropertyInfo table_property;
                if (!FindReflectedPropertyLocked(
                        asset_class, property_name, table_property, true) ||
                    table_property.type != "ObjectProperty" ||
                    table_property.element_size != 8 ||
                    table_property.offset < 0 ||
                    table_property.array_dim != 1) {
                    vehicle_customization_failure_reason =
                        TextLocalized("backend.htvehicledataasset_table_property_failed_type_and_size_144", "HTVehicleDataAsset table property failed type and size validation.");
                    return false;
                }
                if (!ReadValue(*memory, vehicle_data_asset_object +
                        static_cast<std::uintptr_t>(table_property.offset), table_out) ||
                    table_out == 0) {
                    vehicle_customization_failure_reason =
                        TextLocalized("backend.the_module_or_skin_datatable_is_empty_145", "The module or skin DataTable is empty or unreadable.");
                    return false;
                }
                std::string row_name;
                if (!ReadPointerAt(*memory, table_out,
                        layout.dataTable_rowStruct, row_struct_out) ||
                    row_struct_out == 0 ||
                    !ReadReflectedObjectNameLocked(row_struct_out, row_name) ||
                    row_name != expected_row_name) {
                    vehicle_customization_failure_reason =
                        TextLocalized("backend.customization_datatable_rowstruct_has_an_incompatible_type_146", "Customization DataTable.RowStruct has an incompatible type.");
                    return false;
                }
                const auto row_map_offset = layout.dataTable_rowMap;
                if (row_map_offset < 0 ||
                    !ReadSparseMapViewLocked(
                        table_out + static_cast<std::uintptr_t>(row_map_offset), view_out) ||
                    view_out.num <= 0 || view_out.num > 16384 ||
                    view_out.num_free < 0 || view_out.num_free > view_out.num ||
                    view_out.stride <= 0 || view_out.stride > 4096) {
                    vehicle_customization_failure_reason =
                        TextLocalized("backend.customization_datatable_rowmap_layout_or_count_is_147", "Customization DataTable.RowMap layout or count is invalid.");
                    return false;
                }
                const auto element_count = static_cast<std::size_t>(view_out.num);
                const auto stride = static_cast<std::size_t>(view_out.stride);
                if (element_count > (std::numeric_limits<std::size_t>::max)() / stride) {
                    vehicle_customization_failure_reason = TextLocalized("backend.customization_datatable_rowmap_size_overflow_148", "Customization DataTable.RowMap size overflow.");
                    return false;
                }
                elements_out.assign(element_count * stride, 0);
                if (!memory->Read(view_out.data, elements_out.data(), elements_out.size())) {
                    vehicle_customization_failure_reason =
                        TextLocalized("backend.customization_datatable_rowmap_elements_are_unreadable_149", "Customization DataTable.RowMap elements are unreadable.");
                    return false;
                }
                return true;
            };

            std::vector<VehicleCustomizationOption> next;
            const bool can_resolve_display_text = (names && names->resolve_ftext_utf8);
            const auto scan_table = [&](const bool is_skin) -> bool {
                std::uintptr_t table{}, row_struct{};
                SparseMapView view{};
                std::vector<std::uint8_t> elements;
                if (!resolve_table(
                        is_skin ? "VehicleSkinDataTable" : "DT_vehicleModuleData",
                        is_skin ? "VehicleSkinData" : "VehicleModuleData",
                        table, row_struct, view, elements)) {
                    return false;
                }

                std::int32_t row_size{};
                if (!ReadValue(*memory, row_struct + layout.ustruct_propertiesSize, row_size) || row_size <= 0 || row_size > 65535) return false;
                ReflectedPropertyInfo id_property;
                ReflectedPropertyInfo vehicle_id_property;
                ReflectedPropertyInfo label_property;
                if (!FindReflectedPropertyLocked(row_struct,
                        is_skin ? "SkinID" : "ModuleID", id_property, true) ||
                    id_property.type != "NameProperty" || id_property.element_size != 8 ||
                    id_property.offset < 0 || id_property.array_dim != 1 ||
                    !FindReflectedPropertyLocked(row_struct, "VehicleID",
                        vehicle_id_property, true) ||
                    vehicle_id_property.type != "NameProperty" ||
                    vehicle_id_property.element_size != 8 ||
                    vehicle_id_property.offset < 0 ||
                    vehicle_id_property.array_dim != 1 ||
                    !FindReflectedPropertyLocked(row_struct,
                        is_skin ? "Name" : "ModuleName", label_property, true) ||
                    label_property.type != "TextProperty" ||
                    label_property.element_size != 0x10 ||
                    label_property.offset < 0 ||
                    label_property.array_dim != 1) {
                    vehicle_customization_failure_reason =
                        TextLocalized("backend.module_or_skin_id_vehicleid_or_label_150", "Module or skin ID, VehicleID or label property failed validation.");
                    return false;
                }

                ReflectedPropertyInfo module_type_property{};
                ReflectedPropertyInfo default_property{};
                ReflectedBoolParameter default_bool{};
                if (!is_skin) {
                    if (!FindReflectedPropertyLocked(row_struct,
                            "ModuleType", module_type_property, true) ||
                        (module_type_property.type != "ByteProperty" &&
                         module_type_property.type != "EnumProperty") ||
                        module_type_property.element_size != 1 ||
                        module_type_property.offset < 0 ||
                        module_type_property.array_dim != 1 ||
                        !FindReflectedPropertyLocked(row_struct,
                            "bWasDefault", default_property, true) ||
                        default_property.type != "BoolProperty" ||
                        default_property.element_size != 1 ||
                        default_property.offset < 0 ||
                        default_property.array_dim != 1 ||
                        !ReadReflectedBoolParameterLocked(default_property, static_cast<std::uint16_t>(row_size), default_bool)) {
                        vehicle_customization_failure_reason =
                            TextLocalized("backend.vehiclemoduledata_moduletype_or_bwasdefault_failed_validation_151", "VehicleModuleData.ModuleType or bWasDefault failed validation.");
                        return false;
                    }
                } else {
                    if (!FindReflectedPropertyLocked(row_struct,
                            "bDefault", default_property, true) ||
                        default_property.type != "BoolProperty" ||
                        default_property.element_size != 1 ||
                        default_property.offset < 0 ||
                        default_property.array_dim != 1 ||
                        !ReadReflectedBoolParameterLocked(default_property, static_cast<std::uint16_t>(row_size), default_bool)) {
                        vehicle_customization_failure_reason =
                            TextLocalized("backend.vehicleskindata_bdefault_failed_validation_152", "VehicleSkinData.bDefault failed validation.");
                        return false;
                    }
                }

                for (std::int32_t slot = 0; slot < view.num; ++slot) {
                    const auto u_slot = static_cast<std::uint32_t>(slot);
                    if ((view.flags[static_cast<std::size_t>(u_slot) / 32U] &
                            (1U << (u_slot & 31U))) == 0) {
                        continue;
                    }
                    const auto* element = elements.data() +
                        static_cast<std::size_t>(slot) * static_cast<std::size_t>(view.stride);
                    std::uintptr_t row{};
                    std::memcpy(&row, element + view.row_offset, sizeof(row));
                    const auto required_row_bytes = static_cast<std::size_t>(
                        (std::max)(id_property.offset + 8,
                            vehicle_id_property.offset + 8));
                    if (row == 0 || required_row_bytes == 0 ||
                        !ReadableRange(*memory, row, required_row_bytes)) {
                        continue;
                    }

                    std::uint32_t vehicle_comparison{}, vehicle_number{};
                    std::uint32_t id_comparison{}, id_number{};
                    if (!ReadValue(*memory,
                            row + static_cast<std::uintptr_t>(vehicle_id_property.offset),
                            vehicle_comparison) ||
                        !ReadValue(*memory,
                            row + static_cast<std::uintptr_t>(vehicle_id_property.offset) + 4U,
                            vehicle_number) ||
                        vehicle_comparison != selected.vehicle_id_comparison_index ||
                        vehicle_number != selected.vehicle_id_number ||
                        !ReadValue(*memory,
                            row + static_cast<std::uintptr_t>(id_property.offset), id_comparison) ||
                        !ReadValue(*memory,
                            row + static_cast<std::uintptr_t>(id_property.offset) + 4U, id_number)) {
                        continue;
                    }
                    std::string option_id;
                    if (!ResolveFNameLocked(id_comparison, id_number, option_id) ||
                        option_id.empty() || option_id == "None" ||
                        option_id.size() >= nte_vehicle::kVehicleIdMaxBytes) {
                        continue;
                    }
                    std::uint8_t default_byte{};
                    if (!ReadValue(*memory,
                            row + static_cast<std::uintptr_t>(default_bool.byte_offset),
                            default_byte)) {
                        vehicle_customization_failure_reason =
                            TextLocalized("backend.could_not_read_the_customization_default_flag_153", "Could not read the customization default flag.");
                        return false;
                    }
                    const bool is_default = (default_byte & default_bool.byte_mask) != 0;
                    std::string label;
                    if (!can_resolve_display_text ||
                        !ResolveVehicleDisplayNameLocked(row, label_property, label) ||
                        label.empty()) {
                        label = option_id;
                    }
                    VehicleCustomizationOption option{};
                    option.id = std::move(option_id);
                    option.label = std::move(label);
                    option.kind = is_skin
                        ? nte_vehicle::kCustomizationSkin
                        : nte_vehicle::kCustomizationModule;
                    option.module_type = 0;
                    option.name_comparison_index = id_comparison;
                    option.name_number = id_number;
                    option.native_name = static_cast<std::uint64_t>(id_comparison) |
                        (static_cast<std::uint64_t>(id_number) << 32U);
                    option.is_default = is_default;
                    if (!is_skin) {
                        std::uint8_t module_type{};
                        if (!ReadValue(*memory,
                                row + static_cast<std::uintptr_t>(module_type_property.offset),
                                module_type)) {
                            vehicle_customization_failure_reason =
                                TextLocalized("backend.could_not_read_vehiclemoduledata_moduletype_154", "Could not read VehicleModuleData.ModuleType.");
                            return false;
                        }
                        // EVehicleModuleType 0 (None) and 27 (Max) are sentinels in Dump 5.6.1.
                        if (module_type == 0U || module_type >= 27U) continue;
                        option.module_type = module_type;
                    }
                    next.push_back(std::move(option));
                }
                return true;
            };

            if (!scan_table(false) || !scan_table(true)) {
                return false;
            }
            std::ranges::sort(next, [](const auto& lhs, const auto& rhs) {
                if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
                if (lhs.module_type != rhs.module_type) return lhs.module_type < rhs.module_type;
                if (lhs.is_default != rhs.is_default) return lhs.is_default > rhs.is_default;
                if (lhs.label != rhs.label) return lhs.label < rhs.label;
                return lhs.id < rhs.id;
            });
            next.erase(std::unique(next.begin(), next.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.kind == rhs.kind && lhs.module_type == rhs.module_type && lhs.id == rhs.id;
            }), next.end());

            vehicle_customization_options = std::move(next);
            vehicle_customization_vehicle_row_id = selected.id;
            vehicle_customization_generation = object_generation;
            vehicle_customization_vehicle_sequence = vehicle_catalog_sequence;
            ++vehicle_customization_sequence;
            if (vehicle_customization_sequence == 0) ++vehicle_customization_sequence;
            vehicle_customization_failure_reason = TextLocalized("backend.module_and_skin_catalog_read_from_the_155", "Module and skin catalog read from the game's DataTables.");
            return true;
        } catch (...) {
            vehicle_customization_options.clear();
            vehicle_customization_vehicle_row_id.clear();
            vehicle_customization_generation = 0;
            vehicle_customization_vehicle_sequence = 0;
            vehicle_customization_failure_reason = TextLocalized("backend.an_exception_occurred_while_reading_the_customization_156", "An exception occurred while reading the customization catalog.");
            return false;
        }
    }

    AnomalyStatusV1 VehicleCustomizationCatalogSnapshot(
        const AnomalyStringViewV1 vehicle_row_id,
        nte_vehicle::CatalogSnapshot* snapshot) {
        if (snapshot == nullptr || vehicle_row_id.data == nullptr || vehicle_row_id.size == 0 ||
            vehicle_row_id.size >= nte_vehicle::kVehicleIdMaxBytes) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.invalid_customization_catalog_output_buffer_157", "Invalid customization catalog output buffer."));
        }
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire)) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.customization_catalog_requires_the_game_thread_158", "Customization catalog requires the Game thread."));
        }
        std::scoped_lock lock(mutex);
        if (!EnsureVehicleCatalogLocked()) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        }
        const std::string_view requested{vehicle_row_id.data, vehicle_row_id.size};
        const auto selected = std::ranges::find(
            vehicle_catalog, requested, &VehicleCatalogEntry::id);
        if (selected == vehicle_catalog.end()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.the_customization_vehicle_rowname_is_not_in_159", "The customization vehicle RowName is not in DT_VehicleData."));
        }
        if (!EnsureVehicleCustomizationCatalogLocked(*selected)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_customization_failure_reason);
        }
        snapshot->flags = nte_vehicle::kCatalogValid;
        snapshot->sequence = vehicle_customization_sequence;
        snapshot->entry_count = static_cast<std::uint32_t>(vehicle_customization_options.size());
        return Status(ANOMALY_STATUS_V1_OK, vehicle_customization_failure_reason);
    }

    AnomalyStatusV1 VehicleCustomizationOptionAt(
        const AnomalyStringViewV1 vehicle_row_id,
        const std::uint32_t index,
        nte_vehicle::CustomizationEntry* option) {
        if (option == nullptr || vehicle_row_id.data == nullptr || vehicle_row_id.size == 0 ||
            vehicle_row_id.size >= nte_vehicle::kVehicleIdMaxBytes) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.invalid_customization_option_output_buffer_160", "Invalid customization option output buffer."));
        }
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire)) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.customization_option_reads_require_the_game_thread_161", "Customization option reads require the Game thread."));
        }
        std::scoped_lock lock(mutex);
        if (!EnsureVehicleCatalogLocked()) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        }
        const std::string_view requested{vehicle_row_id.data, vehicle_row_id.size};
        const auto selected = std::ranges::find(
            vehicle_catalog, requested, &VehicleCatalogEntry::id);
        if (selected == vehicle_catalog.end()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.the_customization_vehicle_rowname_is_no_longer_162", "The customization vehicle RowName is no longer available."));
        }
        if (!EnsureVehicleCustomizationCatalogLocked(*selected)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_customization_failure_reason);
        }
        if (index >= vehicle_customization_options.size()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.customization_option_index_is_out_of_range_163", "Customization option index is out of range."));
        }
        const auto& source = vehicle_customization_options[index];
        *option = {};
        option->flags = nte_vehicle::kOptionValid |
            (source.is_default ? nte_vehicle::kOptionDefault : 0U);
        option->kind = source.kind;
        option->module_type = source.module_type;
        option->option_id = source.id;
        option->label = source.label;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleSetSpeedTarget(
        const nte_vehicle::SpeedTarget* request) {
        if (request == nullptr)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.vehicle_speed_target_request_is_missing_its_165", "vehicle speed target request is missing its required fields"));
        switch (request->scope) {
        case nte_vehicle::SpeedScope::Current:
            return VehicleSetTopSpeedTarget(
                request->top_speed_kmh, request->minimum_speed_kmh);
        case nte_vehicle::SpeedScope::OtherLoaded:
            return VehicleSetOtherVehiclesTopSpeedTarget(
                request->top_speed_kmh, request->minimum_speed_kmh);
        default:
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.vehicle_speed_target_scope_must_select_exactly_166", "vehicle speed target scope must select exactly one supported group"));
        }
    }

    AnomalyStatusV1 VehicleSetOtherVehiclesEngineTorqueRatio(float ratio) {
        if (!std::isfinite(ratio) || ratio < 0.05F || ratio > 20.0F)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.non_player_vehicle_torque_ratio_must_be_167", "non-player vehicle torque ratio must be 0.05..20.0"));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT,
                TextLocalized("backend.non_player_vehicle_torque_mutation_requires_game_168", "non-player vehicle torque mutation requires Game thread"));

        std::scoped_lock lock(mutex);
        vehicle_other_torque_status.clear();
        if (!EnsureOtherVehicleMutationBindingsLocked(false, true)) {
            vehicle_other_torque_status =
                TextLocalized("backend.neither_the_external_nor_fallback_torque_setter_169", "Neither the external nor fallback torque setter is available for other vehicles.");
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_other_torque_status.c_str());
        }

        std::vector<std::uintptr_t> actors;
        std::size_t unreadable_slots{};
        std::string scan_failure;
        if (!CollectOtherVehicleActorsLocked(actors, unreadable_slots, scan_failure)) {
            vehicle_other_torque_status = scan_failure;
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_other_torque_status.c_str());
        }
        if (actors.empty()) {
            vehicle_other_torque_status = TextLocalized("backend.no_loaded_non_player_htwheeledvehiclebase_instances_were_122", "No loaded non-player HTWheeledVehicleBase instances were found.");
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, vehicle_other_torque_status.c_str());
        }

        std::size_t applied{}, failed{}, external_applied{}, fallback_applied{};
        std::string first_failure;
        const auto record_failure = [&first_failure](std::string reason) {
            if (first_failure.empty()) first_failure = std::move(reason);
        };
        const auto torque_binding = vehicle_bindings.set_external_torque_ratio;
        const auto max_torque_binding = vehicle_bindings.set_max_engine_torque;
        for (const std::uintptr_t actor : actors) {
            std::uintptr_t movement{};
            const auto movement_offset = layout.vehicle_movementComponent;
            if (movement_offset < 0 ||
                !ReadPointerAt(*memory, actor, movement_offset, movement) || movement == 0 ||
                !TrackOtherMovement(movement) ||
                !ObjectClassChainContainsLocked(movement, "HTVehicleMovementComponent")) {
                ++failed;
                record_failure(TextLocalized("backend.vehicle_has_no_validated_htvehiclemovementcomponent_123", "Vehicle has no validated HTVehicleMovementComponent."));
                continue;
            }

            bool invoked{};
            bool used_fallback{};
            std::string primary_failure;
            if (torque_binding.function != 0 && torque_binding.parms_size != 0 &&
                torque_binding.parms_size <= 0x1000U &&
                torque_binding.parameter_offset + sizeof(float) <= torque_binding.parms_size) {
                std::vector<std::uint8_t> parameters(torque_binding.parms_size);
                std::memcpy(parameters.data() + torque_binding.parameter_offset, &ratio, sizeof(ratio));
                invoked = InvokeProcessEventGuarded(process_event_invoker, movement,
                    torque_binding.function, parameters.data(), parameters.size());
                if (!invoked) primary_failure = TextLocalized("backend.setexternaltorqueratio_processevent_failed_170", "SetExternalTorqueRatio ProcessEvent failed.");
            } else {
                primary_failure = TextLocalized("backend.setexternaltorqueratio_binding_or_parameter_layout_is_unavailable_171", "SetExternalTorqueRatio binding or parameter layout is unavailable.");
            }

            // A failed primary invocation must fall through to the alternate setter on this actor.
            if (!invoked && max_torque_binding.function != 0 &&
                max_torque_binding.parms_size != 0 &&
                max_torque_binding.parms_size <= 0x1000U &&
                max_torque_binding.parameter_offset + sizeof(float) <= max_torque_binding.parms_size) {
                auto baseline_it = other_vehicle_base_torques.find(movement);
                if (baseline_it == other_vehicle_base_torques.end()) {
                    const auto baseline_offset = layout.vehicle_maxEngineTorque;
                    float baseline{};
                    if (baseline_offset >= 0 &&
                        ReadValue(*memory, movement + static_cast<std::uintptr_t>(baseline_offset), baseline) &&
                        std::isfinite(baseline) && baseline > 0.0F) {
                        baseline_it = other_vehicle_base_torques.emplace(movement, baseline).first;
                    }
                }
                if (baseline_it != other_vehicle_base_torques.end()) {
                    const auto speed_it = other_vehicle_speed_field_ratios.find(movement);
                    const float speed_torque_scale = speed_it == other_vehicle_speed_field_ratios.end()
                        ? 1.0F : (std::max)(2.0F * speed_it->second, 2.0F);
                    const float torque = baseline_it->second * ratio * speed_torque_scale;
                    if (std::isfinite(torque)) {
                        std::vector<std::uint8_t> parameters(max_torque_binding.parms_size);
                        std::memcpy(parameters.data() + max_torque_binding.parameter_offset,
                            &torque, sizeof(torque));
                        invoked = InvokeProcessEventGuarded(process_event_invoker, movement,
                            max_torque_binding.function, parameters.data(), parameters.size());
                        if (invoked) used_fallback = true;
                        else record_failure(LocalizedStatus("backend.0_setmaxenginetorque_processevent_failed_14", "{0}; SetMaxEngineTorque ProcessEvent failed", {primary_failure}));
                    } else {
                        record_failure(LocalizedStatus("backend.0_scaled_maxtorque_is_invalid_13", "{0}; scaled MaxTorque is invalid", {primary_failure}));
                    }
                } else {
                    record_failure(LocalizedStatus("backend.0_the_original_maxtorque_baseline_is_unavailable_12", "{0}; the original MaxTorque baseline is unavailable", {primary_failure}));
                }
            } else if (!invoked) {
                record_failure(LocalizedStatus("backend.0_setmaxenginetorque_fallback_binding_is_unavailable_11", "{0}; SetMaxEngineTorque fallback binding is unavailable", {primary_failure}));
            }
            if (invoked) {
                ++applied;
                other_vehicle_requested_torque_ratios[movement] = ratio;
                if (used_fallback) {
                    ++fallback_applied;
                    other_vehicle_torque_fallback_ratios[movement] = ratio;
                } else {
                    ++external_applied;
                    other_vehicle_torque_fallback_ratios.erase(movement);
                }
            } else {
                ++failed;
            }
        }

        vehicle_other_torque_status = LocalizedStatus("backend.non_player_engine_torque_ratio_0_x_10", "Non-player engine torque ratio {0}x: found {1} vehicles, applied to {2} (SetExternalTorqueRatio: {3}, SetMaxEngineTorque fallback: {4}), failed for {5} vehicles", {std::to_string(ratio), std::to_string(actors.size()), std::to_string(applied), std::to_string(external_applied), std::to_string(fallback_applied), std::to_string(failed)});
        if (unreadable_slots != 0)
            vehicle_other_torque_status += LocalizedStatus("backend.unreadable_actor_slots_skipped_0_9", "; unreadable actor slots skipped: {0}", {std::to_string(unreadable_slots)});
        if (!first_failure.empty()) vehicle_other_torque_status += LocalizedStatus("backend.first_failure_0_8", "; first failure: {0}", {first_failure});
        if (applied == 0 || failed != 0 || unreadable_slots != 0)
            return Status(ANOMALY_STATUS_V1_FAILED, vehicle_other_torque_status.c_str());
        return Status(ANOMALY_STATUS_V1_OK, vehicle_other_torque_status.c_str());
    }

    AnomalyStatusV1 VehicleCatalogSnapshot(
        nte_vehicle::CatalogSnapshot* snapshot) {
        if (snapshot == nullptr)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.invalid_vehicle_catalog_snapshot_172", "invalid vehicle catalog snapshot"));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_catalog_read_requires_game_thread_173", "vehicle catalog read requires Game thread"));
        std::scoped_lock lock(mutex);
        if (!EnsureVehicleCatalogLocked()) {
            *snapshot = {0, vehicle_catalog_sequence, 0};
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        }
        snapshot->flags = nte_vehicle::kCatalogValid;
        snapshot->sequence = vehicle_catalog_sequence;
        snapshot->entry_count = static_cast<std::uint32_t>(vehicle_catalog.size());
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleCatalogRefresh() {
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_catalog_refresh_requires_the_game_thread_174", "Vehicle catalog refresh requires the Game thread."));
        std::scoped_lock lock(mutex);
        vehicle_catalog.clear();
        vehicle_catalog_valid = false;
        vehicle_catalog_table = 0;
        vehicle_catalog_generation = 0;
        vehicle_catalog_failure_reason = TextLocalized("backend.vehicle_catalog_refresh_requested_175", "Vehicle catalog refresh requested.");
        if (!EnsureVehicleCatalogLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        return Status(ANOMALY_STATUS_V1_OK, TextLocalized("backend.dt_vehicledata_refreshed_176", "DT_VehicleData refreshed."));
    }

    AnomalyStatusV1 VehicleIdAt(
        const std::uint32_t index, char* destination, std::size_t* inout_size) {
        if (inout_size == nullptr)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.catalog_string_size_is_null_177", "catalog string size is null"));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_catalog_read_requires_game_thread_173", "vehicle catalog read requires Game thread"));
        std::scoped_lock lock(mutex);
        if (!EnsureVehicleCatalogLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        if (index >= vehicle_catalog.size())
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.vehicle_catalog_index_is_out_of_range_178", "vehicle catalog index is out of range"));
        return CopyString(vehicle_catalog[index].id, destination, inout_size);
    }

    AnomalyStatusV1 VehicleCatalogLabelAt(
        const std::uint32_t index, char* destination, std::size_t* inout_size) {
        if (inout_size == nullptr)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.catalog_label_buffer_size_pointer_is_null_179", "Catalog label buffer size pointer is null."));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_catalog_labels_require_the_game_thread_180", "Vehicle catalog labels require the Game thread."));
        std::scoped_lock lock(mutex);
        if (!EnsureVehicleCatalogLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        if (index >= vehicle_catalog.size())
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.vehicle_catalog_label_index_is_out_of_181", "Vehicle catalog label index is out of range."));
        return CopyString(vehicle_catalog[index].display_label, destination, inout_size);
    }

    AnomalyStatusV1 VehicleSetSummonVehicleId(const AnomalyStringViewV1 id) {
        if (id.data == nullptr || id.size == 0 || id.size >= nte_vehicle::kVehicleIdMaxBytes)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, TextLocalized("backend.invalid_vehicleid_string_182", "invalid VehicleID string"));
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_selection_requires_game_thread_183", "vehicle selection requires Game thread"));
        std::scoped_lock lock(mutex);
        if (!EnsureVehicleCatalogLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        const std::string_view requested{id.data, id.size};
        const auto exact = std::ranges::find(vehicle_catalog, requested, &VehicleCatalogEntry::id);
        if (exact != vehicle_catalog.end()) {
            selected_summon_vehicle_id = exact->id;
            return Status(ANOMALY_STATUS_V1_OK);
        }
        const VehicleCatalogEntry* alias_match{};
        for (const auto& entry : vehicle_catalog) {
            if (entry.vehicle_id != requested) continue;
            if (alias_match != nullptr && alias_match->row != entry.row) {
                return Status(ANOMALY_STATUS_V1_CONFLICT,
                    TextLocalized("backend.vehicleid_alias_maps_to_multiple_rows_select_184", "VehicleID alias maps to multiple rows; select the displayed RowName instead"));
            }
            alias_match = &entry;
        }
        if (alias_match == nullptr)
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.rowname_vehicleid_is_not_in_dt_vehicledata_185", "RowName / VehicleID is not in DT_VehicleData"));
        selected_summon_vehicle_id = alias_match->id;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    // Anomaly's summon path uses GameplayStatics deferred spawning and finishes the actor
    // directly; CheatManager is retained only as a fallback when the row's soft class is not
    // already resolved by the game. All reflected signatures below are checked against the dump.
    [[nodiscard]] bool ResolveVehicleTemplateClassLocked(
        const std::uintptr_t row, std::uintptr_t& actor_class) {
        actor_class = 0;
        try {
            // HTGame.VehicleData declares InVehicleTemple as TSoftClassPtr<UClass>
            // at row-relative offset 0xE0, size 0x28 in dump 5.6.1-0+UE5-HT.
            // Pass the complete reflected property to LoadClassAsset_Blocking so Unreal
            // resolves the soft reference; do not guess at its private member layout.
            std::uintptr_t row_struct{};
            ReflectedPropertyInfo template_property;
            if (row == 0 || vehicle_catalog_table == 0 ||
                !ReadPointerAt(*memory, vehicle_catalog_table,
                    layout.dataTable_rowStruct, row_struct) ||
                !FindReflectedPropertyLocked(row_struct, "InVehicleTemple",
                    template_property, true) ||
                template_property.type != "SoftClassProperty" ||
                template_property.element_size != 0x28 || template_property.offset < 0 ||
                template_property.offset > 0x400) {
                return false;
            }

            std::uintptr_t load_function{};
            std::uintptr_t library_class{};
            std::uintptr_t library_cdo{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!FindExactObjectLocked(
                    L"/Script/Engine.KismetSystemLibrary.LoadClassAsset_Blocking",
                    load_function) ||
                !ReadPointerAt(*memory, load_function, layout.object_outer, library_class) ||
                !ReadPointerAt(*memory, library_class,
                    layout.uclass_classDefaultObject, library_cdo) ||
                library_cdo == 0 ||
                !ReadValue(*memory, load_function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, load_function + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, load_function + layout.ufunction_returnValueOffset, return_offset) ||
                num_parms != 2 || parms_size != 0x30 || return_offset != 0x28) {
                return false;
            }

            ReflectedPropertyInfo class_input, class_return;
            if (!FindReflectedPropertyLocked(load_function, "AssetClass", class_input, false) ||
                class_input.type != "SoftClassProperty" || class_input.element_size != 0x28 ||
                class_input.offset != 0 ||
                !FindReflectedPropertyLocked(load_function, "ReturnValue", class_return, false) ||
                class_return.type != "ClassProperty" || class_return.element_size != 8 ||
                class_return.offset != 0x28) {
                return false;
            }

            const auto soft_class_address =
                row + static_cast<std::uintptr_t>(template_property.offset);
            alignas(8) std::array<std::uint8_t, 0x30> parameters{};
            if (!ReadableRange(*memory, soft_class_address, 0x28) ||
                !memory->Read(soft_class_address, parameters.data(), 0x28) ||
                !InvokeProcessEventGuarded(process_event_invoker, library_cdo,
                    load_function, parameters.data(), parameters.size())) {
                return false;
            }
            std::memcpy(&actor_class, parameters.data() + 0x28, sizeof(actor_class));
            std::uintptr_t actor_base{};
            std::string class_name;
            if (actor_class == 0 ||
                !FindExactObjectLocked(L"/Script/Engine.Actor", actor_base) ||
                !IsClassDerivedFromLocked(actor_class, actor_base) ||
                !ReadReflectedObjectNameLocked(actor_class, class_name) ||
                !ContainsVehicleToken(class_name)) {
                actor_class = 0;
                return false;
            }
            return true;
        } catch (...) {
            actor_class = 0;
            return false;
        }
    }

    [[nodiscard]] bool VerifyActorLocationLocked(
        const std::uintptr_t actor, const std::array<double, 3>& destination) {
        try {
            std::uintptr_t getter{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (actor == 0 ||
                !FindExactObjectLocked(L"/Script/Engine.Actor.K2_GetActorLocation", getter) ||
                !ReadValue(*memory, getter + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, getter + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, getter + layout.ufunction_returnValueOffset, return_offset) ||
                num_parms != 1 || parms_size != 24 || return_offset != 0) {
                return false;
            }
            ReflectedPropertyInfo ret;
            if (!FindReflectedPropertyLocked(getter, "ReturnValue", ret, false) ||
                ret.type != "StructProperty" || ret.element_size != 24 || ret.offset != 0) {
                return false;
            }
            std::array<std::uint8_t, 24> location_bytes{};
            if (!InvokeProcessEventGuarded(process_event_invoker, actor, getter,
                    location_bytes.data(), location_bytes.size())) {
                return false;
            }
            std::array<double, 3> actual{};
            std::memcpy(actual.data(), location_bytes.data(), sizeof(actual));
            constexpr double tolerance = 0.05;
            for (std::size_t i = 0; i < destination.size(); ++i) {
                if (!std::isfinite(destination[i]) || !std::isfinite(actual[i]) ||
                    std::fabs(actual[i] - destination[i]) > tolerance) {
                    return false;
                }
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool ApplyVehicleSpawnTransformLocked(
        const std::uintptr_t actor,
        const std::uintptr_t actor_class,
        const std::array<std::uint8_t, 0x60>& transform,
        const std::array<double, 3>& destination) {
        try {
            if (actor == 0 || actor_class == 0) return false;
            std::uintptr_t wheeled_class{};
            if (!FindExactObjectLocked(
                    L"/Script/HTGame.HTWheeledVehicleDrivable", wheeled_class)) {
                // A non-wheeled vehicle class need not load the wheeled base class.
                // Keep the spawn usable through the validated generic Actor location route.
                return RelocateSpawnedVehicleLocked(actor, destination);
            }
            if (!IsClassDerivedFromLocked(actor_class, wheeled_class)) {
                // Non-wheeled vehicle actors keep the generic reflected location route.
                return RelocateSpawnedVehicleLocked(actor, destination);
            }

            // Anomaly's wheeled vehicle route calls SafeSetVehicleTransform so the
            // transform reaches the vehicle-specific movement state, not just Actor.Location.
            std::uintptr_t function{};
            std::uintptr_t outer{};
            std::string outer_name;
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!FindExactObjectLocked(
                    L"/Script/HTGame.HTWheeledVehicleDrivable.SafeSetVehicleTransform",
                    function) ||
                !ReadPointerAt(*memory, function, layout.object_outer, outer) ||
                !ReadReflectedObjectNameLocked(outer, outer_name) ||
                outer_name != "HTWheeledVehicleDrivable" ||
                !ReadValue(*memory, function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory, function + layout.ufunction_parmsSize, parms_size) ||
                !ReadValue(*memory, function + layout.ufunction_returnValueOffset, return_offset) ||
                num_parms != 2 || parms_size != 0x70 || return_offset != 0xFFFFu) {
                // The vehicle-specific helper is optional for this subclass/build.
                // Fall back to the reflected Actor route instead of aborting the spawn.
                return RelocateSpawnedVehicleLocked(actor, destination);
            }
            ReflectedPropertyInfo new_transform, stop_movement;
            if (!FindReflectedPropertyLocked(function, "NewTransform", new_transform, false) ||
                new_transform.type != "StructProperty" || new_transform.element_size != 0x60 ||
                new_transform.offset != 0 ||
                !FindReflectedPropertyLocked(function, "bStopMovementImmediately", stop_movement, false) ||
                stop_movement.type != "BoolProperty" || stop_movement.element_size != 1 ||
                stop_movement.offset != 0x60) {
                // Reflection details can differ by subclass; the generic Actor setter
                // remains independently ABI-validated before it is called.
                return RelocateSpawnedVehicleLocked(actor, destination);
            }
            alignas(16) std::array<std::uint8_t, 0x70> parameters{};
            std::memcpy(parameters.data(), transform.data(), transform.size());
            parameters[0x60] = 1;
            if (!InvokeProcessEventGuarded(process_event_invoker, actor,
                    function, parameters.data(), parameters.size())) {
                // Do not let a failed vehicle-specific call prevent the generic route
                // from attempting the same destination on the already-spawned actor.
                return RelocateSpawnedVehicleLocked(actor, destination);
            }
            if (VerifyActorLocationLocked(actor, destination)) return true;

            // SafeSetVehicleTransform is the vehicle-native route, but some vehicle
            // subclasses update their movement state without immediately exposing the
            // same value through Actor.K2_GetActorLocation. Fall back to the validated
            // Actor location setter and require its own postcondition before reporting
            // failure. This does not create a second actor; it only relocates the actor
            // already returned by FinishSpawningActor.
            return RelocateSpawnedVehicleLocked(actor, destination);
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool SpawnVehicleDirectLocked(
        const std::uintptr_t actor_class,
        const std::array<double, 3>& destination,
        std::uintptr_t& spawned,
        const char*& failure_reason) {
        spawned = 0;
        failure_reason = TextLocalized("backend.direct_spawn_arguments_or_live_player_objects_186", "direct spawn arguments or live player objects are invalid");
        try {
            if (actor_class == 0 || player_controller == 0 || player_pawn == 0 ||
                !std::ranges::all_of(destination,
                    [](double value) { return std::isfinite(value); })) {
                return false;
            }

            // DT/HTGame dump ABI: BeginDeferredActorSpawnFromClass accepts a 0x60-byte
            // FTransform. Build the known UE5.6 layout directly rather than depending on
            // K2_GetActorTransform, which is not required for the summon path.
            alignas(16) std::array<std::uint8_t, 0x60> transform{};
            const double identity = 1.0;
            const std::array<double, 3> unit_scale{1.0, 1.0, 1.0};
            // UE5.6 FTransform: Rotation.W @ 0x18, Translation @ 0x20, Scale3D @ 0x40.
            std::memcpy(transform.data() + 0x18, &identity, sizeof(identity));
            std::memcpy(transform.data() + 0x20, destination.data(), sizeof(double) * 3U);
            std::memcpy(transform.data() + 0x40, unit_scale.data(), sizeof(unit_scale));

            std::uintptr_t begin_function{};
            std::uintptr_t statics_class{};
            std::uintptr_t statics_cdo{};
            std::uint8_t begin_num_parms{};
            std::uint16_t begin_parms_size{};
            std::uint16_t begin_return_offset{};
            failure_reason = TextLocalized("backend.gameplaystatics_begindeferredactorspawnfromclass_reflection_abi_validation_failed_187", "GameplayStatics.BeginDeferredActorSpawnFromClass reflection ABI validation failed");
            if (!FindExactObjectLocked(
                    L"/Script/Engine.GameplayStatics.BeginDeferredActorSpawnFromClass",
                    begin_function) ||
                !ReadPointerAt(*memory, begin_function, layout.object_outer, statics_class) ||
                !ReadPointerAt(*memory, statics_class,
                    layout.uclass_classDefaultObject, statics_cdo) ||
                statics_cdo == 0 ||
                !ReadValue(*memory, begin_function + layout.ufunction_numParms,
                    begin_num_parms) ||
                !ReadValue(*memory, begin_function + layout.ufunction_parmsSize,
                    begin_parms_size) ||
                !ReadValue(*memory, begin_function + layout.ufunction_returnValueOffset,
                    begin_return_offset) ||
                begin_num_parms != 7 || begin_parms_size != 0x90 ||
                begin_return_offset != 0x88) {
                return false;
            }
            ReflectedPropertyInfo world_context, class_input, spawn_transform, collision_mode,
                owner_input, scale_mode, begin_return;
            if (!FindReflectedPropertyLocked(begin_function, "WorldContextObject", world_context, false) ||
                world_context.type != "ObjectProperty" || world_context.element_size != 8 ||
                world_context.offset != 0 ||
                !FindReflectedPropertyLocked(begin_function, "ActorClass", class_input, false) ||
                class_input.type != "ClassProperty" || class_input.element_size != 8 ||
                class_input.offset != 8 ||
                !FindReflectedPropertyLocked(begin_function, "SpawnTransform", spawn_transform, false) ||
                spawn_transform.type != "StructProperty" || spawn_transform.element_size != 0x60 ||
                spawn_transform.offset != 0x10 ||
                !FindReflectedPropertyLocked(begin_function, "CollisionHandlingOverride", collision_mode, false) ||
                (collision_mode.type != "ByteProperty" && collision_mode.type != "EnumProperty") ||
                collision_mode.element_size != 1 || collision_mode.offset != 0x70 ||
                !FindReflectedPropertyLocked(begin_function, "Owner", owner_input, false) ||
                owner_input.type != "ObjectProperty" || owner_input.element_size != 8 ||
                owner_input.offset != 0x78 ||
                !FindReflectedPropertyLocked(begin_function, "TransformScaleMethod", scale_mode, false) ||
                (scale_mode.type != "ByteProperty" && scale_mode.type != "EnumProperty") ||
                scale_mode.element_size != 1 || scale_mode.offset != 0x80 ||
                !FindReflectedPropertyLocked(begin_function, "ReturnValue", begin_return, false) ||
                begin_return.type != "ObjectProperty" || begin_return.element_size != 8 ||
                begin_return.offset != 0x88) {
                return false;
            }

            failure_reason = TextLocalized("backend.begindeferredactorspawnfromclass_parameter_layout_failed_validation_188", "BeginDeferredActorSpawnFromClass parameter layout failed validation");
            alignas(16) std::array<std::uint8_t, 0x90> begin_parameters{};
            std::memcpy(begin_parameters.data(), &player_controller, sizeof(player_controller));
            std::memcpy(begin_parameters.data() + 8, &actor_class, sizeof(actor_class));
            std::memcpy(begin_parameters.data() + 0x10, transform.data(), transform.size());
            begin_parameters[0x70] = 1; // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
            std::memcpy(begin_parameters.data() + 0x78, &player_pawn, sizeof(player_pawn));
            begin_parameters[0x80] = 0; // ESpawnActorScaleMethod::OverrideRootScale
            failure_reason = TextLocalized("backend.gameplaystatics_begindeferredactorspawnfromclass_processevent_failed_189", "GameplayStatics.BeginDeferredActorSpawnFromClass ProcessEvent failed");
            if (!InvokeProcessEventGuarded(process_event_invoker, statics_cdo,
                    begin_function, begin_parameters.data(), begin_parameters.size())) {
                return false;
            }
            std::memcpy(&spawned, begin_parameters.data() + 0x88, sizeof(spawned));
            failure_reason = TextLocalized("backend.begindeferredactorspawnfromclass_returned_a_null_actor_190", "BeginDeferredActorSpawnFromClass returned a null actor");
            if (spawned == 0) return false;

            std::uintptr_t finish_function{};
            std::uint8_t finish_num_parms{};
            std::uint16_t finish_parms_size{};
            std::uint16_t finish_return_offset{};
            failure_reason = TextLocalized("backend.gameplaystatics_finishspawningactor_reflection_abi_validation_failed_191", "GameplayStatics.FinishSpawningActor reflection ABI validation failed");
            if (!FindExactObjectLocked(
                    L"/Script/Engine.GameplayStatics.FinishSpawningActor", finish_function) ||
                !ReadValue(*memory, finish_function + layout.ufunction_numParms,
                    finish_num_parms) ||
                !ReadValue(*memory, finish_function + layout.ufunction_parmsSize,
                    finish_parms_size) ||
                !ReadValue(*memory, finish_function + layout.ufunction_returnValueOffset,
                    finish_return_offset) ||
                finish_num_parms != 4 || finish_parms_size != 0x80 ||
                finish_return_offset != 0x78) {
                return false;
            }
            ReflectedPropertyInfo finish_actor, finish_transform, finish_scale, finish_return;
            if (!FindReflectedPropertyLocked(finish_function, "Actor", finish_actor, false) ||
                finish_actor.type != "ObjectProperty" || finish_actor.element_size != 8 ||
                finish_actor.offset != 0 ||
                !FindReflectedPropertyLocked(finish_function, "SpawnTransform", finish_transform, false) ||
                finish_transform.type != "StructProperty" || finish_transform.element_size != 0x60 ||
                finish_transform.offset != 0x10 ||
                !FindReflectedPropertyLocked(finish_function, "TransformScaleMethod", finish_scale, false) ||
                (finish_scale.type != "ByteProperty" && finish_scale.type != "EnumProperty") ||
                finish_scale.element_size != 1 || finish_scale.offset != 0x70 ||
                !FindReflectedPropertyLocked(finish_function, "ReturnValue", finish_return, false) ||
                finish_return.type != "ObjectProperty" || finish_return.element_size != 8 ||
                finish_return.offset != 0x78) {
                return false;
            }
            failure_reason = TextLocalized("backend.finishspawningactor_parameter_layout_failed_validation_192", "FinishSpawningActor parameter layout failed validation");
            alignas(16) std::array<std::uint8_t, 0x80> finish_parameters{};
            std::memcpy(finish_parameters.data(), &spawned, sizeof(spawned));
            std::memcpy(finish_parameters.data() + 0x10, transform.data(), transform.size());
            finish_parameters[0x70] = 0;
            failure_reason = TextLocalized("backend.gameplaystatics_finishspawningactor_processevent_failed_193", "GameplayStatics.FinishSpawningActor ProcessEvent failed");
            if (!InvokeProcessEventGuarded(process_event_invoker, statics_cdo,
                    finish_function, finish_parameters.data(), finish_parameters.size())) {
                return false;
            }
            std::uintptr_t finished_actor{};
            std::memcpy(&finished_actor, finish_parameters.data() + 0x78, sizeof(finished_actor));
            // FinishSpawningActor declares an Actor* return in the supplied dump.
            // A successful ProcessEvent call without a non-null returned actor is not
            // sufficient evidence that deferred spawning completed.
            if (finished_actor == 0) {
                failure_reason = TextLocalized("backend.gameplaystatics_finishspawningactor_returned_a_null_actor_194", "GameplayStatics.FinishSpawningActor returned a null actor");
                return false;
            }
            spawned = finished_actor;

            std::uintptr_t spawned_class{};
            failure_reason = TextLocalized("backend.spawned_actor_did_not_validate_as_a_195", "spawned actor did not validate as a Vehicle-derived Actor");
            if (!IsVehicleActorLocked(spawned) ||
                !ReadPointerAt(*memory, spawned, layout.object_class, spawned_class)) {
                return false;
            }
            failure_reason = TextLocalized("backend.vehicle_transform_location_postcondition_failed_196", "vehicle transform/location postcondition failed");
            if (!ApplyVehicleSpawnTransformLocked(spawned, spawned_class, transform, destination)) {
                return false;
            }
            failure_reason = TextLocalized("backend.actor_setowner_processevent_or_reflected_abi_validation_197", "Actor.SetOwner ProcessEvent or reflected ABI validation failed");
            if (!InvokeVehicleSetOwnerLocked(spawned, player_pawn)) {
                return false;
            }
            ReflectedPropertyInfo owner_property;
            std::uintptr_t actual_owner{};
            failure_reason = TextLocalized("backend.spawned_actor_owner_did_not_resolve_to_198", "spawned Actor.Owner did not resolve to the player pawn");
            if (!ReadPointerAt(*memory, spawned, layout.object_class, spawned_class) ||
                !FindReflectedPropertyLocked(spawned_class, "Owner", owner_property, true) ||
                owner_property.type != "ObjectProperty" || owner_property.element_size != 8 ||
                owner_property.offset < 0 ||
                !ReadValue(*memory, spawned + static_cast<std::uintptr_t>(owner_property.offset), actual_owner) ||
                actual_owner != player_pawn) {
                return false;
            }
            failure_reason = nullptr;
            return true;
        } catch (...) {
            failure_reason = TextLocalized("backend.c_exception_while_running_the_direct_spawn_199", "C++ exception while running the direct spawn path");
            return false;
        }
    }

    [[nodiscard]] bool BuildVehicleConfigurationLocked(
        const VehicleCatalogEntry& selected,
        const nte_vehicle::SummonRequest& request,
        VehicleBuildConfiguration& build,
        std::string& failure) {
        build = {};
        failure.clear();
        try {
            if (!selected.vehicle_id_valid ||
                (selected.vehicle_id_comparison_index == 0 && selected.vehicle_id_number == 0)) {
                failure = TextLocalized("backend.the_selected_row_has_no_valid_vehicledata_200", "The selected row has no valid VehicleData.VehicleID.");
                return false;
            }
            if (request.module_selection_count > 32U ||
                (request.module_selection_count != 0 && request.module_selections == nullptr)) {
                failure = TextLocalized("backend.invalid_module_selection_count_or_array_pointer_201", "Invalid module selection count or array pointer.");
                return false;
            }
            if (!EnsureVehicleCustomizationCatalogLocked(selected)) {
                failure = vehicle_customization_failure_reason;
                return false;
            }
            build.vehicle_id = static_cast<std::uint64_t>(selected.vehicle_id_comparison_index) |
                (static_cast<std::uint64_t>(selected.vehicle_id_number) << 32U);

            const VehicleCustomizationOption* selected_skin{};
            if (request.skin_id.data != nullptr && request.skin_id.size != 0) {
                if (request.skin_id.size >= nte_vehicle::kVehicleIdMaxBytes) {
                    failure = TextLocalized("backend.skin_id_too_long", "Skin ID exceeds the supported length.");
                    return false;
                }
                const std::string_view requested_skin{request.skin_id.data, request.skin_id.size};
                for (const auto& option : vehicle_customization_options) {
                    if (option.kind == nte_vehicle::kCustomizationSkin &&
                        option.id == requested_skin) {
                        selected_skin = &option;
                        break;
                    }
                }
                if (selected_skin == nullptr) {
                    failure = TextLocalized("backend.the_selected_skin_does_not_belong_to_203", "The selected skin does not belong to this vehicle model.");
                    return false;
                }
            } else {
                for (const auto& option : vehicle_customization_options) {
                    if (option.kind == nte_vehicle::kCustomizationSkin &&
                        option.is_default) {
                        selected_skin = &option;
                        break;
                    }
                }
                if (selected_skin == nullptr && std::ranges::any_of(
                        vehicle_customization_options, [](const auto& option) {
                            return option.kind == nte_vehicle::kCustomizationSkin;
                        })) {
                    failure = TextLocalized("backend.select_a_skin_this_model_has_no_204", "Select a skin: this model has no validated default skin.");
                    return false;
                }
            }
            // Models without skin rows use FName None in the module update, as the
            // game's unskinned configuration does. They need no material override.
            build.skin_id = selected_skin != nullptr ? selected_skin->native_name : 0;
            if (selected_skin != nullptr && build.skin_id == 0) {
                failure = TextLocalized("backend.the_selected_skinid_is_none_customization_was_205", "The selected SkinID is None; customization was rejected.");
                return false;
            }

            // Start from this model's game-defined default module records. The plugin sends only
            // explicit per-slot overrides; this prevents an omitted UI slot from deleting the
            // stock module list or pulling another model's choices into the build.
            for (const auto& option : vehicle_customization_options) {
                if (option.kind == nte_vehicle::kCustomizationModule &&
                    option.is_default) {
                    if (option.native_name == 0) {
                        failure = TextLocalized("backend.the_model_s_default_modules_contain_an_206", "The model's default modules contain an invalid FName.");
                        return false;
                    }
                    if (std::ranges::find(build.module_ids, option.native_name) ==
                        build.module_ids.end()) {
                        build.module_ids.push_back(option.native_name);
                    }
                }
            }
            if (build.module_ids.size() > 128U) {
                failure = TextLocalized("backend.too_many_default_modules_207", "Too many default modules.");
                return false;
            }

            std::array<bool, 27> seen_module_types{};
            for (std::uint32_t index = 0; index < request.module_selection_count; ++index) {
                const auto& selection = request.module_selections[index];
                if (selection.module_type == 0 || selection.module_type >= seen_module_types.size() ||
                    selection.module_id.data == nullptr || selection.module_id.size == 0 ||
                    selection.module_id.size >= nte_vehicle::kVehicleIdMaxBytes) {
                    failure = TextLocalized("backend.invalid_module_selection_or_moduletype_208", "Invalid module selection or ModuleType.");
                    return false;
                }
                if (seen_module_types[selection.module_type]) {
                    failure = TextLocalized("backend.only_one_module_may_be_selected_for_209", "Only one module may be selected for each ModuleType.");
                    return false;
                }
                seen_module_types[selection.module_type] = true;
                const std::string_view module_id{
                    selection.module_id.data, selection.module_id.size};
                const VehicleCustomizationOption* selected_module{};
                for (const auto& option : vehicle_customization_options) {
                    if (option.kind == nte_vehicle::kCustomizationModule &&
                        option.module_type == selection.module_type && option.id == module_id) {
                        selected_module = &option;
                        break;
                    }
                }
                if (selected_module == nullptr || selected_module->native_name == 0) {
                    failure = TextLocalized("backend.the_module_id_or_type_does_not_210", "The module ID or type does not belong to this vehicle model.");
                    return false;
                }

                // Remove every default item in this slot before inserting the chosen module ID.
                std::erase_if(build.module_ids, [&](const std::uint64_t native_name) {
                    for (const auto& candidate : vehicle_customization_options) {
                        if (candidate.kind == nte_vehicle::kCustomizationModule &&
                            candidate.native_name == native_name &&
                            candidate.module_type == selection.module_type) {
                            return true;
                        }
                    }
                    return false;
                });
                build.module_ids.push_back(selected_module->native_name);
            }
            if (build.module_ids.size() > 128U) {
                failure = TextLocalized("backend.the_combined_module_list_exceeds_the_limit_211", "The combined module list exceeds the limit.");
                return false;
            }
            failure.clear();
            return true;
        } catch (...) {
            build = {};
            failure = TextLocalized("backend.an_exception_occurred_while_assembling_the_selected_212", "An exception occurred while assembling the selected customization.");
            return false;
        }
    }

    [[nodiscard]] bool ApplyVehicleConfigurationLocked(
        const std::uintptr_t spawned_vehicle,
        const VehicleBuildConfiguration& saved,
        std::size_t& configured_module_count,
        std::string& failure) {
        configured_module_count = 0;
        failure.clear();
        try {
            if (spawned_vehicle == 0 || saved.vehicle_id == 0 ||
                saved.module_ids.size() > 128U) {
                failure = TextLocalized("backend.invalid_spawned_vehicle_or_customization_parameters_213", "Invalid spawned vehicle or customization parameters.");
                return false;
            }
            std::uintptr_t spawned_class{};
            if (!ReadPointerAt(*memory, spawned_vehicle, layout.object_class, spawned_class)) {
                failure = TextLocalized("backend.could_not_resolve_the_spawned_vehicle_s_214", "Could not resolve the spawned vehicle's class.");
                return false;
            }
            ReflectedPropertyInfo module_manager_property;
            if (!FindReflectedPropertyLocked(
                    spawned_class, "ModuleManagerComponent", module_manager_property, true) ||
                module_manager_property.type != "ObjectProperty" ||
                module_manager_property.element_size != 8 ||
                module_manager_property.offset < 0 ||
                module_manager_property.array_dim != 1) {
                failure = TextLocalized("backend.the_spawned_vehicle_s_modulemanagercomponent_failed_reflection_215", "The spawned vehicle's ModuleManagerComponent failed reflection validation.");
                return false;
            }
            std::uintptr_t module_manager{};
            if (!ReadValue(*memory,
                    spawned_vehicle + static_cast<std::uintptr_t>(module_manager_property.offset),
                    module_manager) || module_manager == 0) {
                failure = TextLocalized("backend.the_spawned_vehicle_s_modification_component_is_216", "The spawned vehicle's modification component is not initialized.");
                return false;
            }
            std::uintptr_t expected_module_manager_class{};
            std::uintptr_t actual_module_manager_class{};
            if (!FindExactObjectLocked(
                    L"/Script/HTGame.HTVehicleModificationComponent", expected_module_manager_class) ||
                !ReadPointerAt(*memory, module_manager, layout.object_class,
                    actual_module_manager_class) ||
                !IsClassDerivedFromLocked(actual_module_manager_class, expected_module_manager_class)) {
                failure = TextLocalized("backend.the_spawned_modification_component_has_an_incompatible_217", "The spawned modification component has an incompatible class.");
                return false;
            }

            std::uintptr_t update_function{};
            if (!FindExactObjectLocked(
                    L"/Script/HTGame.HTVehicleModificationComponent.UpdateAllModulesWithIDs",
                    update_function) || update_function == 0) {
                failure = TextLocalized("backend.updateallmoduleswithids_is_unavailable_218", "UpdateAllModulesWithIDs is unavailable.");
                return false;
            }
            std::uintptr_t function_owner{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            if (!ReadPointerAt(*memory, update_function, layout.object_outer, function_owner) ||
                function_owner != expected_module_manager_class ||
                !ReadValue(*memory,
                    update_function + layout.ufunction_numParms, num_parms) ||
                !ReadValue(*memory,
                    update_function + layout.ufunction_parmsSize, parms_size) ||
                num_parms != 5 || parms_size != 0x40) {
                failure = TextLocalized("backend.updateallmoduleswithids_owner_or_parameter_abi_is_incompatible_219", "UpdateAllModulesWithIDs owner or parameter ABI is incompatible.");
                return false;
            }

            const auto validate_parameter = [&](const std::string_view name,
                    const std::string_view type, const std::int32_t element_size,
                    const std::int32_t offset, ReflectedPropertyInfo& property) {
                return FindReflectedPropertyLocked(update_function, name, property, false) &&
                    property.type == type && property.element_size == element_size &&
                    property.offset == offset && property.array_dim == 1;
            };
            ReflectedPropertyInfo module_ids_parameter;
            ReflectedPropertyInfo force_update_parameter;
            ReflectedPropertyInfo vehicle_id_parameter;
            ReflectedPropertyInfo skin_id_parameter;
            ReflectedPropertyInfo default_data_parameter;
            if (!validate_parameter("VehicleModuleIDList", "ArrayProperty", 0x10, 0x00,
                    module_ids_parameter) ||
                !validate_parameter("bForceUpdate", "BoolProperty", 1, 0x10,
                    force_update_parameter) ||
                !validate_parameter("VehicleID", "NameProperty", 8, 0x14,
                    vehicle_id_parameter) ||
                !validate_parameter("SkinID", "NameProperty", 8, 0x1C,
                    skin_id_parameter) ||
                !validate_parameter("DefaultModuleData", "StructProperty", 0x1C, 0x24,
                    default_data_parameter) ||
                default_data_parameter.structure == 0) {
                failure = TextLocalized("backend.updateallmoduleswithids_parameter_types_sizes_or_offsets_are_220", "UpdateAllModulesWithIDs parameter types, sizes or offsets are incompatible.");
                return false;
            }
            std::string update_default_data_type;
            if (!ReadReflectedObjectNameLocked(
                    default_data_parameter.structure, update_default_data_type) ||
                update_default_data_type != "VehicleModuleDefaultData") {
                failure = TextLocalized("backend.updateallmoduleswithids_defaultmoduledata_has_an_incompatible_structure_type_221", "UpdateAllModulesWithIDs.DefaultModuleData has an incompatible structure type.");
                return false;
            }
            const auto module_inner_offset = layout.farrayProperty_inner;
            std::uintptr_t module_inner{};
            std::string module_inner_type;
            if (module_inner_offset < 0 ||
                !ReadPointerAt(*memory, module_ids_parameter.property, module_inner_offset, module_inner) ||
                module_inner == 0 ||
                !ReadReflectedFieldClassNameLocked(module_inner, module_inner_type) ||
                module_inner_type != "NameProperty") {
                failure = TextLocalized("backend.updateallmoduleswithids_vehiclemoduleidlist_does_not_contain_fnames_222", "UpdateAllModulesWithIDs.VehicleModuleIDList does not contain FNames.");
                return false;
            }
            ReflectedBoolParameter force_bool;
            if (!ReadReflectedBoolParameterLocked(force_update_parameter, parms_size, force_bool) ||
                force_bool.byte_offset < 0x10 || force_bool.byte_offset >= parms_size) {
                failure = TextLocalized("backend.updateallmoduleswithids_bforceupdate_has_an_invalid_bool_mask_223", "UpdateAllModulesWithIDs.bForceUpdate has an invalid bool mask.");
                return false;
            }

            struct NativeArrayArgument {
                std::uintptr_t data{};
                std::int32_t count{};
                std::int32_t capacity{};
            };
            static_assert(sizeof(NativeArrayArgument) == 0x10);
            std::array<std::uint8_t, 0x40> parameters{};
            NativeArrayArgument module_ids_argument{};
            module_ids_argument.count = static_cast<std::int32_t>(saved.module_ids.size());
            module_ids_argument.capacity = module_ids_argument.count;
            if (!saved.module_ids.empty())
                module_ids_argument.data = reinterpret_cast<std::uintptr_t>(saved.module_ids.data());
            std::memcpy(parameters.data() + module_ids_parameter.offset,
                &module_ids_argument, sizeof(module_ids_argument));
            const auto force_offset = static_cast<std::size_t>(force_bool.byte_offset);
            parameters[force_offset] = static_cast<std::uint8_t>(
                (parameters[force_offset] & static_cast<std::uint8_t>(~force_bool.field_mask)) |
                force_bool.byte_mask);
            std::memcpy(parameters.data() + vehicle_id_parameter.offset,
                &saved.vehicle_id, sizeof(saved.vehicle_id));
            std::memcpy(parameters.data() + skin_id_parameter.offset,
                &saved.skin_id, sizeof(saved.skin_id));
            std::memcpy(parameters.data() + default_data_parameter.offset,
                saved.default_module_data.data(), saved.default_module_data.size());

            if (!InvokeProcessEventGuarded(process_event_invoker, module_manager,
                    update_function, parameters.data(), parms_size)) {
                failure = TextLocalized("backend.htvehiclemodificationcomponent_updateallmoduleswithids_failed_224", "HTVehicleModificationComponent.UpdateAllModulesWithIDs failed.");
                return false;
            }

            if (saved.skin_id != 0) {
                // The supplied Dump declares UpdateVehicleMat(FName VehicleID, FName SkinID).
                // Validate its owner and exact parameters before applying the selected skin.
                std::uintptr_t update_mat_function{};
                if (!FindExactObjectLocked(
                        L"/Script/HTGame.HTVehicleModificationComponent.UpdateVehicleMat",
                        update_mat_function) || update_mat_function == 0) {
                    failure = TextLocalized("backend.updatevehiclemat_is_unavailable_225", "UpdateVehicleMat is unavailable.");
                    return false;
                }
                std::uintptr_t update_mat_owner{};
                std::uint8_t update_mat_num_parms{};
                std::uint16_t update_mat_parms_size{};
                std::uint16_t update_mat_return_offset{};
                if (!ReadPointerAt(*memory, update_mat_function,
                        layout.object_outer, update_mat_owner) ||
                    update_mat_owner != expected_module_manager_class ||
                    !ReadValue(*memory, update_mat_function + layout.ufunction_numParms,
                        update_mat_num_parms) ||
                    !ReadValue(*memory, update_mat_function + layout.ufunction_parmsSize,
                        update_mat_parms_size) ||
                    !ReadValue(*memory, update_mat_function + layout.ufunction_returnValueOffset,
                        update_mat_return_offset) ||
                    update_mat_num_parms != 2 || update_mat_parms_size != 0x10 ||
                    update_mat_return_offset != 0xFFFFu) {
                    failure = TextLocalized("backend.updatevehiclemat_owner_or_parameter_abi_is_incompatible_226", "UpdateVehicleMat owner or parameter ABI is incompatible.");
                    return false;
                }
                ReflectedPropertyInfo update_mat_vehicle_id;
                ReflectedPropertyInfo update_mat_skin_id;
                if (!FindReflectedPropertyLocked(update_mat_function,
                        "VehicleID", update_mat_vehicle_id, false) ||
                    update_mat_vehicle_id.type != "NameProperty" ||
                    update_mat_vehicle_id.element_size != 8 || update_mat_vehicle_id.offset != 0 ||
                    update_mat_vehicle_id.array_dim != 1 ||
                    !FindReflectedPropertyLocked(update_mat_function,
                        "SkinID", update_mat_skin_id, false) ||
                    update_mat_skin_id.type != "NameProperty" ||
                    update_mat_skin_id.element_size != 8 || update_mat_skin_id.offset != 8 ||
                    update_mat_skin_id.array_dim != 1) {
                    failure = TextLocalized("backend.updatevehiclemat_vehicleid_or_skinid_failed_validation_227", "UpdateVehicleMat.VehicleID or SkinID failed validation.");
                    return false;
                }
                std::array<std::uint8_t, 0x10> update_mat_parameters{};
                std::memcpy(update_mat_parameters.data() + update_mat_vehicle_id.offset,
                    &saved.vehicle_id, sizeof(saved.vehicle_id));
                std::memcpy(update_mat_parameters.data() + update_mat_skin_id.offset,
                    &saved.skin_id, sizeof(saved.skin_id));
                if (!InvokeProcessEventGuarded(process_event_invoker, module_manager,
                        update_mat_function, update_mat_parameters.data(), update_mat_parameters.size())) {
                    failure = TextLocalized("backend.htvehiclemodificationcomponent_updatevehiclemat_failed_228", "HTVehicleModificationComponent.UpdateVehicleMat failed.");
                    return false;
                }
            }

            ReflectedPropertyInfo current_modules_property;
            if (!FindReflectedPropertyLocked(actual_module_manager_class,
                    "CurrentModuleIDList", current_modules_property, true) ||
                current_modules_property.type != "ArrayProperty" ||
                current_modules_property.element_size != 0x10 ||
                current_modules_property.offset < 0 ||
                current_modules_property.array_dim != 1) {
                failure = TextLocalized("backend.currentmoduleidlist_failed_reflection_validation_229", "CurrentModuleIDList failed reflection validation.");
                return false;
            }
            std::uintptr_t current_modules_address{};
            if (!AddAddress(module_manager, current_modules_property.offset, current_modules_address)) {
                failure = TextLocalized("backend.could_not_resolve_currentmoduleidlist_s_address_230", "Could not resolve CurrentModuleIDList's address.");
                return false;
            }
            NativeArrayHeader current_modules_array;
            if (!ReadNativeArrayHeaderLocked(current_modules_address, current_modules_array, 128) ||
                current_modules_array.count != static_cast<std::int32_t>(saved.module_ids.size())) {
                failure = TextLocalized("backend.applied_module_count_differs_from_the_selected_231", "Applied module count differs from the selected configuration.");
                return false;
            }
            std::vector<std::uint64_t> observed_module_ids(saved.module_ids.size());
            if (!observed_module_ids.empty() &&
                !memory->Read(current_modules_array.data, observed_module_ids.data(),
                    observed_module_ids.size() * sizeof(std::uint64_t))) {
                failure = TextLocalized("backend.could_not_read_currentmoduleidlist_after_customization_232", "Could not read CurrentModuleIDList after customization.");
                return false;
            }
            auto expected_module_ids = saved.module_ids;
            std::ranges::sort(expected_module_ids);
            std::ranges::sort(observed_module_ids);
            if (expected_module_ids != observed_module_ids) {
                failure = TextLocalized("backend.applied_modules_differ_from_the_selected_configuration_233", "Applied modules differ from the selected configuration.");
                return false;
            }
            configured_module_count = saved.module_ids.size();
            return true;
        } catch (...) {
            failure = TextLocalized("backend.an_exception_occurred_while_applying_vehicle_customization_234", "An exception occurred while applying vehicle customization.");
            return false;
        }
    }

    AnomalyStatusV1 VehicleSummon(
        const nte_vehicle::SummonRequest* request) {
        constexpr std::uint32_t kAllowedSummonFlags =
            nte_vehicle::kSummonHasPosition |
            nte_vehicle::kSummonSetOwnerToPlayer |
            nte_vehicle::kSummonCustomConfiguration;
        if (request == nullptr ||
            (request->flags & (nte_vehicle::kSummonHasPosition |
                nte_vehicle::kSummonSetOwnerToPlayer)) !=
                (nte_vehicle::kSummonHasPosition |
                 nte_vehicle::kSummonSetOwnerToPlayer) ||
            (request->flags & ~kAllowedSummonFlags) != 0 ||
            !std::ranges::all_of(request->world_position,
                [](double value) { return std::isfinite(value); })) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.summon_requires_finite_position_and_player_owner_235", "summon requires finite position and player-owner flags"));
        }
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_summon_requires_game_thread_236", "vehicle summon requires Game thread"));

        std::scoped_lock lock(mutex);
        if (player_controller == 0 || player_pawn == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.summon_failed_the_local_player_controller_or_237", "Summon failed: the local player controller or pawn is unavailable."));
        }
        if (object_registry.items == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.summon_failed_the_object_registry_is_unavailable_238", "Summon failed: the object registry is unavailable."));
        }
        if (!process_event_invoker) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.summon_failed_processevent_is_unavailable_239", "Summon failed: ProcessEvent is unavailable."));
        }
        if (!EnsureVehicleCatalogLocked()) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, vehicle_catalog_failure_reason);
        }
        if (selected_summon_vehicle_id.empty())
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.select_a_vehicleid_from_dt_vehicledata_first_240", "select a VehicleID from DT_VehicleData first"));
        const auto selected = std::ranges::find(
            vehicle_catalog, selected_summon_vehicle_id, &VehicleCatalogEntry::id);
        if (selected == vehicle_catalog.end()) {
            selected_summon_vehicle_id.clear();
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, TextLocalized("backend.selected_vehicleid_is_no_longer_in_the_241", "selected VehicleID is no longer in the current data table"));
        }

        const bool apply_custom_configuration =
            (request->flags & nte_vehicle::kSummonCustomConfiguration) != 0;
        VehicleBuildConfiguration selected_configuration;
        if (apply_custom_configuration) {
            std::string configuration_failure;
            if (!BuildVehicleConfigurationLocked(
                    *selected, *request, selected_configuration, configuration_failure)) {
                vehicle_summon_status_message = LocalizedStatus("backend.customized_summon_failed_0_7", "Customized summon failed: {0}", {configuration_failure});
                return Status(ANOMALY_STATUS_V1_FAILED, vehicle_summon_status_message.c_str());
            }
        } else if (request->skin_id.data != nullptr || request->skin_id.size != 0 ||
                   request->module_selections != nullptr || request->module_selection_count != 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                TextLocalized("backend.plain_summon_cannot_include_customization_selections_242", "Plain summon cannot include customization selections."));
        }
        std::size_t configured_module_count{};
        const auto apply_custom_modifications = [&](const std::uintptr_t spawned_actor) {
            if (!apply_custom_configuration) return true;
            std::string apply_failure;
            if (!ApplyVehicleConfigurationLocked(
                    spawned_actor, selected_configuration, configured_module_count, apply_failure)) {
                vehicle_summon_status_message = LocalizedStatus("backend.vehicle_spawned_but_customization_failed_0_6", "Vehicle spawned, but customization failed: {0}", {apply_failure});
                return false;
            }
            return true;
        };

        // Prefer the Anomaly-style direct route: resolve the VehicleData row's InVehicleTemple
        // soft class and use GameplayStatics deferred spawn. This path does not require a CheatManager.
        const std::array<double, 3> direct_destination{
            request->world_position[0], request->world_position[1], request->world_position[2]};
        std::uintptr_t direct_vehicle_class{};
        if (ResolveVehicleTemplateClassLocked(selected->row, direct_vehicle_class)) {
            std::uintptr_t direct_spawned{};
            const char* direct_spawn_failure{};
            if (SpawnVehicleDirectLocked(
                    direct_vehicle_class, direct_destination, direct_spawned, direct_spawn_failure)) {
                if (!apply_custom_modifications(direct_spawned))
                    return Status(ANOMALY_STATUS_V1_FAILED, vehicle_summon_status_message.c_str());
                std::string class_name = "Vehicle";
                std::uintptr_t class_object{};
                if (ReadPointerAt(*memory, direct_spawned, layout.object_class, class_object))
                    static_cast<void>(ReadReflectedObjectNameLocked(class_object, class_name));
                std::uint64_t entity_id{};
                std::uintptr_t index_address{};
                std::int32_t internal_index{-1};
                if (AddAddress(direct_spawned, layout.object_internalIndex, index_address) &&
                    ReadValue(*memory, index_address, internal_index) && internal_index >= 0)
                    entity_id = static_cast<std::uint64_t>(static_cast<std::uint32_t>(internal_index)) + 1U;
                vehicle_summon_status_message = LocalizedStatus("backend.vehicle_spawned_and_verified_0_5", "Vehicle spawned and verified: {0}", {(selected->display_label.empty() ? selected->id : selected->display_label)});
                if (entity_id != 0) vehicle_summon_status_message += LocalizedStatus("backend.0_4", " (#{0})", {std::to_string(entity_id)});
                vehicle_summon_status_message += LocalizedStatus("backend.position_0_1_2_actor_owner_verified_3", "; position=({0}, {1}, {2}); Actor.Owner verified as the local player pawn", {std::to_string(direct_destination[0]), std::to_string(direct_destination[1]), std::to_string(direct_destination[2])});
                if (apply_custom_configuration) {
                    vehicle_summon_status_message += LocalizedStatus("backend.customization_verified_modules", "; customization verified (module count={0})", {std::to_string(configured_module_count)});
                }
                return Status(ANOMALY_STATUS_V1_OK, vehicle_summon_status_message.c_str());
            }
            // The detailed failure stage tells us whether no actor was created or a
            // post-spawn invariant failed. Do not dispatch a second spawn when an actor
            // may already have been created.
            return Status(ANOMALY_STATUS_V1_FAILED,
                direct_spawn_failure != nullptr
                    ? direct_spawn_failure
                    : TextLocalized("backend.gameplaystatics_spawned_actor_passed_all_direct_spawn_243", "GameplayStatics spawned actor passed all direct-spawn checks"));
        }

        // The direct class may not yet be resident because InVehicleTemple is a soft reference.
        // Only in that case retain the dump-validated CheatManager route as a compatibility fallback.
        std::uintptr_t controller_class{};
        ReflectedPropertyInfo manager_property;
        if (!ReadPointerAt(*memory, player_controller, layout.object_class, controller_class) ||
            !FindReflectedPropertyLocked(controller_class, "CheatManager", manager_property, true) ||
            manager_property.type != "ObjectProperty" || manager_property.element_size != 8 ||
            manager_property.offset < 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, TextLocalized("backend.htplayercontroller_cheatmanager_property_failed_validation_244", "HTPlayerController.CheatManager property failed validation"));
        }
        std::uintptr_t cheat_manager{};
        if (!ReadValue(*memory, player_controller + static_cast<std::uintptr_t>(manager_property.offset), cheat_manager) ||
            cheat_manager == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, TextLocalized("backend.game_cheatmanager_instance_is_not_available_245", "game CheatManager instance is not available"));
        }
        VehicleFunctionBinding summon_binding{};
        static constexpr std::array<std::string_view, 1> cheat_manager_outer{"HTCheatManager"};
        if (!FindVehicleFunctionLocked("CheatSpawnVehicle", cheat_manager_outer, "NameInput", summon_binding)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, TextLocalized("backend.htcheatmanager_cheatspawnvehicle_fname_abi_validation_failed_246", "HTCheatManager.CheatSpawnVehicle(FName) ABI validation failed"));
        }

        // Capture existing vehicle actors so the spawned actor must be new before relocation/ownership.
        std::unordered_set<std::uintptr_t> existing;
        const std::uint32_t scan_count = (std::min)(object_registry.count, object_registry.max_count);
        existing.reserve((std::min)(static_cast<std::size_t>(scan_count), std::size_t{4096}));
        // Snapshot every live object pointer, not just current vehicles: a class may only
        // become identifiable after spawn, and must not be mistaken for a new object.
        for (std::uint32_t index = 0; index < scan_count; ++index) {
            std::uintptr_t object{};
            std::uint32_t serial{};
            if (ReadObjectSlot(*memory, object_registry, index, object, serial) && object != 0)
                existing.insert(object);
        }

        // CheatSpawnVehicle consumes a native FName VehicleID, not the DataTable RowName.
        // The direct soft-class route above uses the row pointer; this fallback must use
        // the independently validated VehicleData.VehicleID field and must never substitute
        // the displayed Name or the canonical RowName.
        if (!selected->vehicle_id_valid) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.the_soft_vehicle_class_and_native_vehicleid_247", "The soft vehicle class and native VehicleID are unavailable. Refusing to substitute RowName in CheatSpawnVehicle."));
        }
        std::array<std::uint8_t, 8> parameters{};
        std::memcpy(parameters.data() + summon_binding.parameter_offset,
            &selected->vehicle_id_comparison_index, sizeof(selected->vehicle_id_comparison_index));
        std::memcpy(parameters.data() + summon_binding.parameter_offset + sizeof(std::uint32_t),
            &selected->vehicle_id_number, sizeof(selected->vehicle_id_number));
        if (!InvokeProcessEventGuarded(process_event_invoker, cheat_manager,
                summon_binding.function, parameters.data(), summon_binding.parms_size)) {
            return Status(ANOMALY_STATUS_V1_FAILED, TextLocalized("backend.cheatspawnvehicle_fname_processevent_failed_248", "CheatSpawnVehicle(FName) ProcessEvent failed"));
        }

        // GObjects count can grow during CheatSpawnVehicle. Reload the live registry header
        // instead of scanning only the cached count from the beginning of this game tick.
        ObjectRegistryState after_spawn_registry;
        if (!LoadRegistry(after_spawn_registry)) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.spawn_dispatched_but_live_gobjects_could_not_249", "spawn dispatched, but live GObjects could not be refreshed; position/owner not verified"));
        }
        std::uintptr_t spawned{};
        const std::uint32_t after_spawn_count =
            (std::min)(after_spawn_registry.count, after_spawn_registry.max_count);
        for (std::uint32_t index = 0; index < after_spawn_count; ++index) {
            std::uintptr_t object{};
            std::uint32_t serial{};
            if (ReadObjectSlot(*memory, after_spawn_registry, index, object, serial) && object != 0 &&
                !existing.contains(object) && IsVehicleActorLocked(object)) {
                spawned = object;
                break;
            }
        }
        if (spawned == 0) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.spawn_was_dispatched_but_the_new_vehicle_250", "spawn was dispatched, but the new vehicle actor could not be identified; position/owner not verified"));
        }
        const std::array<double, 3> destination{
            request->world_position[0], request->world_position[1], request->world_position[2]};
        if (!RelocateSpawnedVehicleLocked(spawned, destination)) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.vehicle_actor_spawned_but_k2_setactorlocation_postcondition_251", "vehicle actor spawned but K2_SetActorLocation postcondition failed"));
        }
        if (!InvokeVehicleSetOwnerLocked(spawned, player_pawn)) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.vehicle_location_verified_but_actor_setowner_player_252", "vehicle location verified but Actor.SetOwner(player) call failed"));
        }
        // Verify the actual Actor.Owner field before reporting success.
        std::uintptr_t spawned_class{};
        ReflectedPropertyInfo owner_property;
        std::uintptr_t actual_owner{};
        if (!ReadPointerAt(*memory, spawned, layout.object_class, spawned_class) ||
            !FindReflectedPropertyLocked(spawned_class, "Owner", owner_property, true) ||
            owner_property.type != "ObjectProperty" || owner_property.element_size != 8 ||
            owner_property.offset < 0 ||
            !ReadValue(*memory, spawned + static_cast<std::uintptr_t>(owner_property.offset), actual_owner) ||
            actual_owner != player_pawn) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                TextLocalized("backend.vehicle_position_verified_but_actor_owner_is_253", "vehicle position verified but Actor.Owner is not the local player pawn"));
        }
        if (!apply_custom_modifications(spawned))
            return Status(ANOMALY_STATUS_V1_FAILED, vehicle_summon_status_message.c_str());
        std::string class_name = "Vehicle";
        std::uintptr_t class_object{};
        if (ReadPointerAt(*memory, spawned, layout.object_class, class_object))
            static_cast<void>(ReadReflectedObjectNameLocked(class_object, class_name));
        std::uint64_t entity_id{};
        std::uintptr_t index_address{};
        std::int32_t internal_index{-1};
        if (AddAddress(spawned, layout.object_internalIndex, index_address) &&
            ReadValue(*memory, index_address, internal_index) && internal_index >= 0)
            entity_id = static_cast<std::uint64_t>(static_cast<std::uint32_t>(internal_index)) + 1U;
        vehicle_summon_status_message = LocalizedStatus("backend.vehicle_spawned_and_verified_0_5", "Vehicle spawned and verified: {0}", {(selected->display_label.empty() ? selected->id : selected->display_label)});
        if (entity_id != 0) vehicle_summon_status_message += LocalizedStatus("backend.0_4", " (#{0})", {std::to_string(entity_id)});
        vehicle_summon_status_message += LocalizedStatus("backend.position_0_1_2_actor_owner_verified_3", "; position=({0}, {1}, {2}); Actor.Owner verified as the local player pawn", {std::to_string(destination[0]), std::to_string(destination[1]), std::to_string(destination[2])});
        if (apply_custom_configuration) {
            vehicle_summon_status_message += LocalizedStatus("backend.customization_verified_modules", "; customization verified (module count={0})", {std::to_string(configured_module_count)});
        }
        return Status(ANOMALY_STATUS_V1_OK, vehicle_summon_status_message.c_str());
    }

    AnomalyStatusV1 VehicleSetWheelFriction(bool enabled) {
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT, TextLocalized("backend.vehicle_mutation_must_run_on_game_thread_73", "vehicle mutation must run on Game thread"));
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleLocked() || vehicle_movement_component == 0 ||
            vehicle_bindings.set_wheel_friction.function == 0)
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                TextLocalized("backend.htvehiclemovementcomponent_setenablewheelfriction_bool_is_not_validated_254", "HTVehicleMovementComponent.SetEnableWheelFriction(bool) is not validated"));
        std::array<std::uint8_t, 8> parameters{};
        parameters[vehicle_bindings.set_wheel_friction.parameter_offset] = enabled ? 1 : 0;
        if (!InvokeProcessEventGuarded(process_event_invoker, vehicle_movement_component,
                vehicle_bindings.set_wheel_friction.function, parameters.data(),
                vehicle_bindings.set_wheel_friction.parms_size))
            return Status(ANOMALY_STATUS_V1_FAILED, TextLocalized("backend.setenablewheelfriction_processevent_failed_255", "SetEnableWheelFriction ProcessEvent failed"));
        vehicle_wheel_friction_enabled = enabled;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleReset() {
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_CONFLICT,
                TextLocalized("backend.vehicle_mutation_must_run_on_game_thread_73", "vehicle mutation must run on Game thread"));
        AnomalyStatusV1 speed = Status(ANOMALY_STATUS_V1_OK);
        {
            std::scoped_lock lock(mutex);
            if (!RefreshVehicleLocked()) {
                speed = Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                    TextLocalized("backend.current_driving_vehicle_is_unavailable_60", "current driving vehicle is unavailable"));
            } else if (vehicle_is_water) {
                speed = Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                    TextLocalized("backend.water_vehicle_stock_speed_limit_was_not_257", "water-vehicle stock speed limit was not captured"));
            } else if (vehicle_speed_fields_applied) {
                if (vehicle_speed_field_baseline_component != vehicle_movement_component ||
                    !vehicle_speed_field_baseline.valid) {
                    speed = Status(ANOMALY_STATUS_V1_FAILED,
                        TextLocalized("backend.stock_vehicle_speed_snapshot_is_unavailable_258", "stock vehicle speed snapshot is unavailable"));
                } else if (vehicle_bindings.set_top_speed_ratio.function == 0) {
                    speed = Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                        TextLocalized("backend.htgame_htwheeledvehiclebase_settopspeedratio_float_is_unavailable_259", "HTGame.HTWheeledVehicleBase.SetTopSpeedRatio(float) is unavailable"));
                } else {
                    VehicleSpeedFieldBaseline current_fields_before{};
                    if (!ReadVehicleSpeedFieldSnapshotLocked(
                            vehicle_movement_component, current_fields_before)) {
                        speed = Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                            TextLocalized("backend.current_vehicle_speed_fields_are_unreadable_260", "current vehicle speed fields are unreadable"));
                    } else {
                        std::array<std::uint8_t, 8> stock_parameters{};
                        std::memcpy(stock_parameters.data() +
                                vehicle_bindings.set_top_speed_ratio.parameter_offset,
                            &vehicle_speed_field_baseline.top_speed_ratio,
                            sizeof(vehicle_speed_field_baseline.top_speed_ratio));
                        if (!InvokeProcessEventGuarded(process_event_invoker, current_vehicle_object,
                                vehicle_bindings.set_top_speed_ratio.function,
                                stock_parameters.data(),
                                vehicle_bindings.set_top_speed_ratio.parms_size)) {
                            speed = Status(ANOMALY_STATUS_V1_FAILED,
                                TextLocalized("backend.restore_settopspeedratio_processevent_failed_261", "restore SetTopSpeedRatio ProcessEvent failed"));
                        } else {
                            std::string field_failure;
                            if (WriteVehicleSpeedFieldSnapshotLocked(vehicle_movement_component,
                                    vehicle_speed_field_baseline, field_failure)) {
                                vehicle_top_speed_ratio =
                                    vehicle_speed_field_baseline.top_speed_ratio;
                                vehicle_current_velocity_ratio =
                                    vehicle_speed_field_baseline.top_speed_ratio;
                                vehicle_speed_fields_applied = false;
                                vehicle_speed_maintenance.Reset();
                            } else {
                                std::array<std::uint8_t, 8> rollback_parameters{};
                                std::memcpy(rollback_parameters.data() +
                                        vehicle_bindings.set_top_speed_ratio.parameter_offset,
                                    &current_fields_before.top_speed_ratio,
                                    sizeof(current_fields_before.top_speed_ratio));
                                static_cast<void>(InvokeProcessEventGuarded(
                                    process_event_invoker, current_vehicle_object,
                                    vehicle_bindings.set_top_speed_ratio.function,
                                    rollback_parameters.data(),
                                    vehicle_bindings.set_top_speed_ratio.parms_size));
                                std::string rollback_failure;
                                static_cast<void>(WriteVehicleSpeedFieldSnapshotLocked(
                                    vehicle_movement_component, current_fields_before,
                                    rollback_failure));
                                vehicle_speed_restore_status =
                                    LocalizedStatus("backend.failed_to_restore_stock_speed_fields_0_1", "failed to restore stock speed fields: {0}", {field_failure});
                                speed = Status(
                                    ANOMALY_STATUS_V1_FAILED,
                                    vehicle_speed_restore_status.c_str());
                            }
                        }
                    }
                }
            }
        }
        if (speed.code != ANOMALY_STATUS_V1_OK) return speed;
        const auto torque = VehicleSetEngineTorqueRatio(1.0F);
        const auto friction = VehicleSetWheelFriction(true);
        if (torque.code != ANOMALY_STATUS_V1_OK) return torque;
        return friction;
    }


};
} // namespace nte_vehicle
