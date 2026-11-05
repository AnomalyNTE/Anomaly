#pragma once

#include "blueprint_edits.hpp"
#include "native_layout.hpp"
#include <anomaly/sdk/base.h>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>

namespace summon_lab {

struct EditorField {
    BlueprintFieldEdit data; // CDO default, copied for display
    std::uint32_t offset{};
    std::uint8_t mask{}, set_mask{};
};

struct EditorSnapshot {
    std::string path, message;
    std::vector<EditorField> fields;
    bool ready{}, pending{};
};

struct EditorScan {
    std::string path;
    std::uintptr_t type{}, cdo{}, owner{}, next_field{};
    AnomalyGenerationHandleV1 type_handle{}, cdo_handle{};
    std::int32_t object_size{};
    std::uint32_t depth{}, field_count{};
    std::vector<EditorField> fields;
};

struct EditorAssetCatalog {
    std::vector<std::string> skills, effects;
    std::string message;
};

struct EditorRule {
    std::string path;
    SoftClassRef reference{};
    bool effect{};
};

struct EditorRuntime {
    std::vector<EditorRule> rules;
    std::size_t next{};
    std::chrono::steady_clock::time_point due{};
    bool cycle_failed{};
};

struct EditorState {
    // UI/settings state, protected by Context::mutex.
    std::map<std::string, BlueprintEdits, std::less<>> blueprints;
    EditorSnapshot snapshot;
    std::string requested_path;
    bool load_requested{};
    std::array<char, 96> field_filter{}, skill_filter{}, effect_filter{};
    std::size_t field_page{}, skill_page{}, effect_page{};
    int customized_only{}, skills_selected_only{}, effects_selected_only{};
    // Asset enumeration runs on Worker and publishes values only.
    std::atomic<std::shared_ptr<const EditorAssetCatalog>> catalog;
    std::atomic<bool> catalog_requested{false}, catalog_busy{false};
    AnomalyGenerationHandleV1 catalog_task{}, birth_observer{};
    // Game owns these cursors; neither Render nor Worker touches UObject data.
    EditorScan scan;
    std::uint64_t record_cursor{};
};

} // namespace summon_lab
