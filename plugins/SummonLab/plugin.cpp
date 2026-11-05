#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/plugin.h>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/interop.h>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/platform.h>
#include <anomaly/sdk/services/ui.h>
#include <anomaly/sdk/services/ue5.h>

#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/profile.hpp"
#include "attribute_catalog.hpp"
#include "attribute_descriptions.hpp"
#include "battle_graph.hpp"
#include "native_layout.hpp"
#include "blueprint_names.hpp"
#include "presets.hpp"
#include "blueprint_editor_state.hpp"
#include "preset_file_picker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <chrono>
#include <mutex>
#include <type_traits>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- 常量

constexpr std::size_t kManagementPageSize = 10;
constexpr std::size_t kPresetPageSize = 10;
// SummonAI accepts signed 32-bit levels; do not impose a gameplay cap.
constexpr std::uint32_t kMaxLevel = summon_lab::kMaximumLevel;
constexpr std::size_t kTextCapacity = 256;
constexpr std::size_t kPathCapacity = 512;
constexpr std::size_t kAttributeNameCapacity = 96;
constexpr std::size_t kDisplayNameCapacity = 96;
using summon_lab::kBirthAttributeNames;
using summon_lab::kMaxHpAttribute;
using summon_lab::BirthPreset;
using summon_lab::SummonSettings;
using summon_lab::SummonPreset;
using summon_lab::SavedConfiguration;

// Parsed once per DLL generation; callbacks only read this snapshot.
summon_lab::NativeLayout g_layout;
bool g_layout_ready{};
anomaly::sdk::ProfileScanCache g_profile_scans;
using summon_lab::NativeIdentity;
using summon_lab::MirrorEntry;
using summon_lab::ConfigureSummon;

// 枚举
constexpr std::uint8_t kAttackAggressive = 0;
constexpr std::uint8_t kAttackDefensive = 1;

// 路径
constexpr const char* kGameplayStaticsCdo = "/Script/Engine.Default__GameplayStatics";
constexpr const char* kPathMakeSoftClassPath =
    "/Script/Engine.KismetSystemLibrary:MakeSoftClassPath";
constexpr const char* kPathConvSoftClassRef =
    "/Script/Engine.KismetSystemLibrary:Conv_SoftClassPathToSoftClassRef";
constexpr const char* kPathGetTransform = "/Script/Engine.Actor:GetTransform";
constexpr const char* kPathDestroyActor = "/Script/Engine.Actor:K2_DestroyActor";
constexpr const char* kPathSummonAI = "/Script/HTGame.HTAbilityCharacter:SummonAI";
constexpr const char* kPathAddSummonedAI = "/Script/HTGame.HTAbilityCharacter:AddSummonedAI";
constexpr const char* kPathGetHP = "/Script/HTGame.HTAbilityCharacter:GetHP";
constexpr const char* kPathGetHPMax = "/Script/HTGame.HTAbilityCharacter:GetHPMax";

constexpr const char* kDefaultClassPath =
    "/Game/Blueprints/Character/Monster/BP_WoodTest.BP_WoodTest_C";

constexpr std::uint32_t kHealthRefreshInterval = 60;
constexpr float kBattleChaseRange = 1000000.0F;


// ---------------------------------------------------------------- 类型

enum class OpKind : std::uint32_t {
    Summon = 1,
    Remove = 2,
    ClearAll = 3,
    SetBossHpBar = 5,
    SetCollision = 6,
    SetHidden = 7,
    SetBattleEdge = 8,
    StopDuel = 9,
    SelectAllOpponents = 10,
    SetGroupBattle = 11,
    StopGroupBattle = 12,
    SetAggroSettings = 13
};

struct SummonBatch;

enum class TargetPolicy { Native, Distributed, Nearest, LowestHealth };

struct AggroSettings {
    TargetPolicy policy{TargetPolicy::Native};
    std::uint64_t priority_target{};
    double chase_distance{}; // Metres between combatants; zero keeps unrestricted pursuit.
    bool paused{};
    bool operator==(const AggroSettings&) const = default;
};

struct PendingOp {
    OpKind kind{};
    std::uint64_t record_id{};
    bool enabled{};
    std::uint64_t target_id{};
    std::shared_ptr<SummonBatch> batch;
    std::vector<std::uint64_t> members;
    std::vector<std::uint64_t> opponents;
    AggroSettings aggro;
};

struct ClassEntry {
    std::string label;
    std::string path;
    std::string search_text;
};

struct BlueprintOptions {
    bool use_catalog_name{};
    std::string default_display_name;
    std::string normal_combat_blueprint;
};

enum class BlueprintConfigState { Pending, Unavailable, NotApplicable, NoHealth, Available };
enum class BlueprintConfigStage { Native, Candidates, Borrowed, Complete };

struct CombatConfigChoice {
    std::string path;
    float hp{};
};

struct BlueprintConfigCheck {
    std::string path;
    BlueprintConfigState state{BlueprintConfigState::Pending};
    std::string source;
    std::string requested_source;
    BlueprintConfigStage stage{BlueprintConfigStage::Native};
    std::vector<std::string> candidates;
    std::size_t next_candidate{};
    std::vector<CombatConfigChoice> sources;
};

struct AdvancedConfigCheck {
    std::string path;
    BlueprintConfigState state{BlueprintConfigState::Pending};
    float hp{};
};

struct AttributeValue {
    std::string name;
    float base{};
    float current{};
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // The captured native FTransform retains its 16-byte alignment.
#endif
struct SummonBatch {
    SummonSettings settings;
    // Progress is protected by Context::mutex; placement is Game-thread-only.
    std::uint32_t completed{};
    bool stopped{}, in_progress{};
    summon_lab::Transform origin;
    summon_lab::Vector rotation{};
    std::uint64_t next_slot{};
    std::vector<summon_lab::Vector> occupied;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct SummonIdentity {
    AnomalyGenerationHandleV1 controller{};
    NativeIdentity leader{};
    std::int32_t index{-1};
};

struct Record {
    std::uint64_t id{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};      // SummonAI 返回的 Actor 指针，Dismiss 需要它
    float hp{};
    float max_hp{};
    bool pet{};
    bool mute_boss_music{}, mute_boss_dialogue{};
    SummonIdentity registration;
    std::string class_path;
    std::string server_combat_source;
    bool battle_tuned{};
    float original_chase_range{};
    std::uint8_t original_leave_battle{};
    bool boss_hp_bar_visible{true};
    AggroSettings aggro;
    std::uint64_t current_target_id{};
    bool target_paused{};
    std::vector<AttributeValue> attributes;
    std::shared_ptr<summon_lab::EditorRuntime> editor;
    std::string editor_warning;
    std::string editor_rule_warning;
    char label[96]{};
};

struct ManagementRecord {
    std::uint64_t id{};
    float hp{}, max_hp{};
    bool pet{};
    std::string class_path;
    std::array<char, 96> label{};
    AggroSettings aggro;
    std::uint64_t current_target_id{};
};

struct NativeCombatHook {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t original{};
};

struct NameTextConversion {
    const wchar_t* input{};
    std::int32_t count{};
    std::int32_t capacity{};
    std::array<std::uint8_t, 16> text{};
};
static_assert(sizeof(NameTextConversion) == 32);

struct PresetTransfer {
    bool importing{};
    std::string path;
    std::vector<SummonPreset> presets;
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::sdk::Localizer localizer;
    std::array<summon_lab::AttributeDescription, kBirthAttributeNames.size()> attribute_descriptions;
    const AnomalyUiServiceV1* ui{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    AnomalyGenerationHandleV1 health_observer{};
    std::array<AnomalyGenerationHandleV1, 4> health_events{};
    bool health_event_lookup_pending{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyHookServiceV1* hooks{};
    const AnomalyUe5AssetsServiceV1* assets{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyStorageServiceV1* storage{};
    const AnomalyConfigServiceV1* config{};
    AnomalyGenerationHandleV1 config_schema{}, config_task{}, preset_transfer_task{};
    bool config_writable{}, config_dirty{}, config_stopping{};
    std::uint64_t config_revision{};
    std::string config_error, preset_message;
    std::vector<SummonPreset> saved_presets;
    std::size_t selected_preset{summon_lab::kNoPreset};
    std::array<char, summon_lab::kPresetNameCapacity> new_preset_name{}, preset_rename{};
    std::array<char, 96> preset_filter{};
    std::size_t preset_page{};
    std::array<char, summon_lab::kPresetFileNameCapacity> preset_export_name{};
    std::string preset_export_folder, preset_import_file;
    summon_lab::PresetFilePicker preset_picker;
    int presets_current_blueprint{1};
    std::shared_ptr<PresetTransfer> preset_transfer;
    std::string catalog_base, catalog_name_table;
    std::set<std::string, std::less<>> catalog_exclusions;
    std::map<std::string, std::string, std::less<>> catalog_name_aliases;
    std::map<std::string, BlueprintOptions, std::less<>> catalog_policy;
    using CatalogLabels = std::map<std::string, std::string, std::less<>>;
    std::atomic<std::shared_ptr<const CatalogLabels>> catalog_labels;
    std::atomic<bool> catalog_stop{false}, catalog_busy{false}, catalog_refresh_requested{true};
    bool catalog_preparing{}; // Game only
    std::chrono::steady_clock::time_point catalog_preparation_started;
    AnomalyGenerationHandleV1 catalog_task{};
    std::array<char, 160> catalog_scan_status{};
    std::array<NativeCombatHook, 5> local_combat_hooks{};
    std::array<NativeCombatHook, 4> story_audio_hooks{};
    std::atomic<bool> story_audio_ready{false};
    struct DamageReportLayout {
        std::uint32_t size{}, source{}, target{}, effect{}, damage{}, hp{};
    } damage_report;
    std::uint32_t mirror_record_size{};
    std::vector<std::uint8_t> mirror_getter_entry;
    std::uint32_t camp_count{};
    std::uint32_t current_chase_range_offset{};
    std::size_t available_birth_attributes{};
    AnomalyGenerationHandleV1 attribute_hook{};
    std::uintptr_t attribute_original{};
    std::atomic<bool> birth_attributes_ready{false};
    std::uintptr_t monster_level_query{};
    std::string final_tower_scene_asset;
    std::map<std::string, std::string, std::less<>> final_tower_aliases;
    std::string final_tower_combat_blueprint;

    bool has_invoke{};

    std::mutex mutex;
    std::vector<Record> records;
    std::uint64_t next_record_id{1};

    std::deque<PendingOp> pending;
    std::shared_ptr<SummonBatch> summon_batch;

    bool session_world_ready{};
    std::uint64_t session_event_cursor{};
    std::uint32_t frame_counter{};

    // 可编辑参数
    std::array<char, kPathCapacity> class_path{};
    std::string combat_config_blueprint;
    std::string advanced_combat_blueprint;
    int advanced_combat_source{};
    std::array<char, 64> combat_source_filter{};
    std::size_t combat_source_page{};
    AdvancedConfigCheck advanced_config;
    BlueprintConfigCheck blueprint_config;
    double blueprint_check_delay{};
    std::array<char, kDisplayNameCapacity> display_name{};
    std::uint32_t level{};
    std::uint32_t skill_level{};
    std::array<BirthPreset, kBirthAttributeNames.size()> birth_presets{};
    std::array<char, 64> attribute_filter{};
    std::size_t attribute_help{kBirthAttributeNames.size()};
    int only_overrides{};
    std::size_t catalog_page{};
    int hostile{};
    int pet{};
    int aggressive{};
    int ignore_player{};
    int mute_boss_music{};
    int mute_boss_dialogue{};
    int skip_cutscenes{};
    int ft_compatibility{};
    std::uint32_t summon_count{1};
    double summon_spacing{5.0};

    // Game-thread-only scratch. Native Conv_StringToText replaces its previous
    // FText through UE's move assignment. Retain the final reference for this
    // DLL generation rather than guessing an ITextData destructor on unload.
    NameTextConversion name_text;

    // Immutable catalog contents are prepared on Worker and swapped under the mutex
    std::vector<ClassEntry> catalog;
    std::map<std::string, BlueprintOptions, std::less<>> blueprint_options;
    char catalog_filter[64]{};
    bool catalog_loaded{};
    std::uint64_t selected_record_id{};
    std::size_t management_page{};
    std::string management_blueprint;
    summon_lab::BattleGraph battles;
    bool show_aggro_editor{};
    std::vector<std::uint64_t> aggro_edit_members;
    AggroSettings aggro_edit;

    std::array<char, kTextCapacity> last_result{};
    bool last_result_failed{};
    summon_lab::EditorState editor;
} g_context;

std::atomic<std::uint32_t> g_pending_hint{0};

// Configuration snapshots contain values only, never live actor handles.
SummonSettings CaptureSettingsLocked() {
    SummonSettings settings;
    settings.class_path = g_context.class_path.data();
    settings.combat_config_blueprint = g_context.combat_config_blueprint;
    settings.advanced_combat_blueprint = g_context.advanced_combat_blueprint;
    settings.advanced_combat_source = g_context.advanced_combat_source != 0;
    settings.display_name = g_context.display_name.data();
    settings.level = g_context.level;
    settings.skill_level = g_context.skill_level;
    settings.hostile = g_context.hostile != 0;
    settings.pet = g_context.pet != 0;
    settings.aggressive = g_context.aggressive != 0;
    settings.ignore_player = g_context.ignore_player != 0;
    settings.mute_boss_music = g_context.mute_boss_music != 0;
    settings.mute_boss_dialogue = g_context.mute_boss_dialogue != 0;
    settings.skip_cutscenes = g_context.skip_cutscenes != 0;
    settings.compatibility = g_context.ft_compatibility != 0;
    settings.presets = g_context.birth_presets;
    settings.count = g_context.summon_count;
    settings.spacing = g_context.summon_spacing;
    if (const auto it = g_context.editor.blueprints.find(settings.class_path); it != g_context.editor.blueprints.end())
        settings.editor = it->second;
    return settings;
}

void ApplySettingsLocked(const SummonSettings& settings) {
    std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s", settings.class_path.c_str());
    g_context.combat_config_blueprint = settings.combat_config_blueprint;
    g_context.advanced_combat_blueprint = settings.advanced_combat_blueprint;
    g_context.advanced_combat_source = settings.advanced_combat_source;
    g_context.advanced_config = {};
    std::snprintf(g_context.display_name.data(), g_context.display_name.size(), "%s", settings.display_name.c_str());
    g_context.level = settings.level;
    g_context.skill_level = settings.skill_level;
    g_context.hostile = settings.hostile;
    g_context.pet = settings.pet;
    g_context.aggressive = settings.aggressive;
    g_context.ignore_player = settings.ignore_player;
    g_context.mute_boss_music = settings.mute_boss_music;
    g_context.mute_boss_dialogue = settings.mute_boss_dialogue;
    g_context.skip_cutscenes = settings.skip_cutscenes;
    g_context.ft_compatibility = settings.compatibility;
    g_context.birth_presets = settings.presets;
    g_context.summon_count = settings.count;
    g_context.summon_spacing = settings.spacing;
    g_context.editor.blueprints.insert_or_assign(settings.class_path, settings.editor);
    g_context.blueprint_config = {};
    g_context.blueprint_check_delay = 0;
}

SavedConfiguration CaptureConfigurationLocked() {
    SavedConfiguration saved;
    saved.current = CaptureSettingsLocked();
    saved.presets = g_context.saved_presets;
    saved.editor_blueprints = g_context.editor.blueprints;
    saved.catalog_filter = g_context.catalog_filter;
    saved.attribute_filter = g_context.attribute_filter.data();
    saved.only_overrides = g_context.only_overrides != 0;
    saved.only_current_blueprint = g_context.presets_current_blueprint != 0;
    saved.export_name = g_context.preset_export_name.data();
    saved.export_folder = g_context.preset_export_folder;
    saved.import_file = g_context.preset_import_file;
    return saved;
}

AnomalyByteSpanV1 ConfigurationBytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void ANOMALY_CALL SaveConfigurationWorker(void*, AnomalyGenerationHandleV1);

void ScheduleConfigurationSaveLocked() {
    if (!g_context.config_writable || g_context.config_stopping || g_context.config_task.id != 0 ||
        g_context.scheduler == nullptr) return;
    const auto status = g_context.scheduler->schedule(g_context.scheduler->user, 500,
        SaveConfigurationWorker, nullptr, &g_context.config_task);
    if (status.code != ANOMALY_STATUS_V1_OK)
        g_context.config_error = g_context.localizer.Text("presets.save_failed", "Configuration could not be saved");
}

void ConfigurationChangedLocked() {
    g_context.config_dirty = true;
    ++g_context.config_revision;
    ScheduleConfigurationSaveLocked();
}

bool SaveConfiguration() noexcept {
    try {
        SavedConfiguration saved;
        std::uint64_t revision{};
        {
            std::scoped_lock lock(g_context.mutex);
            if (!g_context.config_dirty || !g_context.config_writable) return true;
            saved = CaptureConfigurationLocked();
            revision = g_context.config_revision;
        }
        // Serialization, schema validation and disk I/O run on Worker/Lifecycle.
        const auto text = summon_lab::ConfigurationJson(saved);
        const auto status = g_context.config->write_atomic(g_context.config->user,
            anomaly::sdk::StringView(summon_lab::kSettingsSchemaId), summon_lab::kSettingsSchemaVersion,
            ConfigurationBytes(text));
        std::scoped_lock lock(g_context.mutex);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            g_context.config_error = g_context.localizer.Text("presets.save_failed", "Configuration could not be saved");
            return false;
        }
        if (revision == g_context.config_revision) g_context.config_dirty = false;
        g_context.config_error.clear();
        return true;
    } catch (...) {
        std::scoped_lock lock(g_context.mutex);
        g_context.config_error = g_context.localizer.Text("presets.save_failed", "Configuration could not be saved");
        return false;
    }
}

void ANOMALY_CALL SaveConfigurationWorker(void*, AnomalyGenerationHandleV1) {
    try {
        const bool saved = SaveConfiguration();
        std::scoped_lock lock(g_context.mutex);
        g_context.config_task = {};
        if (saved && g_context.config_dirty) ScheduleConfigurationSaveLocked();
    } catch (...) {}
}

bool LoadConfiguration() noexcept {
    try {
        std::uint32_t version{};
        std::size_t size{};
        const auto id = anomaly::sdk::StringView(summon_lab::kSettingsSchemaId);
        const auto status = g_context.config->read(g_context.config->user, id, &version, {}, &size);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            std::scoped_lock lock(g_context.mutex);
            g_context.config_writable = true;
            g_context.config_error.clear();
            return true;
        }
        if (status.code != ANOMALY_STATUS_V1_OK || version != summon_lab::kSettingsSchemaVersion ||
            size == 0 || size > summon_lab::kMaximumSettingsBytes) return false;
        std::string text(size, '\0');
        if (g_context.config->read(g_context.config->user, id, &version,
                {reinterpret_cast<std::uint8_t*>(text.data()), text.size()}, &size).code != ANOMALY_STATUS_V1_OK ||
            version != summon_lab::kSettingsSchemaVersion || size > text.size()) return false;
        text.resize(size);
        SavedConfiguration saved;
        if (!summon_lab::ParseConfiguration(text, saved)) return false;
        std::scoped_lock lock(g_context.mutex);
        g_context.editor.blueprints = std::move(saved.editor_blueprints);
        ApplySettingsLocked(saved.current);
        g_context.saved_presets = std::move(saved.presets);
        std::snprintf(g_context.catalog_filter, sizeof(g_context.catalog_filter), "%s", saved.catalog_filter.c_str());
        std::snprintf(g_context.attribute_filter.data(), g_context.attribute_filter.size(), "%s", saved.attribute_filter.c_str());
        std::snprintf(g_context.preset_export_name.data(), g_context.preset_export_name.size(), "%s", saved.export_name.c_str());
        g_context.preset_export_folder = std::move(saved.export_folder);
        g_context.preset_import_file = std::move(saved.import_file);
        g_context.only_overrides = saved.only_overrides;
        g_context.presets_current_blueprint = saved.only_current_blueprint;
        g_context.config_writable = true;
        g_context.config_dirty = false;
        g_context.config_error.clear();
        return true;
    } catch (...) { return false; }
}

void InitializeConfiguration(const anomaly::sdk::Host& host) {
    PWSTR documents{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        g_context.preset_export_folder = summon_lab::PathUtf8(std::filesystem::path(documents));
        CoTaskMemFree(documents);
    }
    g_context.config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID).get();
    const auto config = g_context.config;
    if (config != nullptr && config->struct_size >= offsetof(AnomalyConfigServiceV1, write_atomic) +
            sizeof(config->write_atomic) && config->register_schema && config->read && config->write_atomic) {
        const auto schema = summon_lab::ConfigurationSchema();
        const auto status = config->register_schema(config->user, anomaly::sdk::StringView(summon_lab::kSettingsSchemaId),
            summon_lab::kSettingsSchemaVersion, ConfigurationBytes(schema), &g_context.config_schema);
        if (status.code == ANOMALY_STATUS_V1_OK && g_context.config_schema.id != 0 && LoadConfiguration()) return;
    }
    // Never replace an unreadable library with the default empty one.
    g_context.config_error = g_context.localizer.Text("presets.load_failed", "Configuration could not be read; automatic saving is disabled");
}

std::filesystem::path PresetFilePath(std::string_view text) {
    const std::filesystem::path path(std::u8string(text.begin(), text.end()));
    if (!path.is_absolute() || _wcsicmp(path.extension().c_str(), L".json") != 0)
        throw std::invalid_argument("absolute JSON path required");
    return path;
}

std::filesystem::path PresetExportPath(std::string_view folder, std::string_view filename) {
    std::filesystem::path name(std::u8string(filename.begin(), filename.end()));
    if (name.empty() || name.has_parent_path() || !name.has_filename())
        throw std::invalid_argument("file name required");
    if (!name.has_extension()) name += L".json";
    return PresetFilePath(summon_lab::PathUtf8(
        std::filesystem::path(std::u8string(folder.begin(), folder.end())) / name));
}

void ConsumePresetPickerLocked() {
    auto result = g_context.preset_picker.TakeResult();
    if (!result || result->status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(result->status)) {
        g_context.preset_message = g_context.localizer.Text("presets.picker_failed", "File picker could not be opened");
        return;
    }
    if (result->kind == summon_lab::PresetFilePicker::Kind::Folder)
        g_context.preset_export_folder = std::move(result->path);
    else
        g_context.preset_import_file = std::move(result->path);
    g_context.preset_message.clear();
    ConfigurationChangedLocked();
}

void OpenPresetPickerLocked(summon_lab::PresetFilePicker::Kind kind) {
    std::string initial = g_context.preset_export_folder;
    if (kind == summon_lab::PresetFilePicker::Kind::ImportFile) {
        initial = g_context.preset_import_file;
        if (initial.empty()) {
            const auto folder = std::filesystem::path(std::u8string(
                g_context.preset_export_folder.begin(), g_context.preset_export_folder.end()));
            initial = summon_lab::PathUtf8(folder / summon_lab::kDefaultPresetFileName);
        }
    }
    const auto title = kind == summon_lab::PresetFilePicker::Kind::Folder
        ? g_context.localizer.Text("presets.choose_folder", "Choose folder")
        : g_context.localizer.Text("presets.choose_file", "Choose file");
    if (!g_context.preset_picker.Start(kind, std::move(initial), title))
        g_context.preset_message = g_context.localizer.Text("presets.picker_failed", "File picker could not be opened");
}

void WritePresetFile(const std::filesystem::path& path, std::string_view text) {
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("create export");
    DWORD written{};
    const bool complete = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 &&
        written == text.size() && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!complete || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("write export");
    }
}

std::string BirthPresetError(const std::array<BirthPreset, kBirthAttributeNames.size()>& presets);

void ANOMALY_CALL TransferPresetsWorker(void*, AnomalyGenerationHandleV1) {
    std::shared_ptr<PresetTransfer> transfer;
    {
        std::scoped_lock lock(g_context.mutex);
        transfer = g_context.preset_transfer;
    }
    try {
        const auto path = PresetFilePath(transfer->path);
        if (transfer->importing) {
            const auto size = std::filesystem::file_size(path);
            if (size == 0 || size > summon_lab::kMaximumSettingsBytes) throw std::runtime_error("import size");
            std::ifstream file(path, std::ios::binary);
            std::string text(static_cast<std::size_t>(size), '\0');
            if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("read import");
            std::vector<SummonPreset> imported;
            if (!summon_lab::ImportPresets(text, imported)) throw std::runtime_error("import format");
            for (const auto& preset : imported)
                if (!BirthPresetError(preset.settings.presets).empty()) throw std::runtime_error("import attributes");
            SavedConfiguration proposed;
            {
                std::scoped_lock lock(g_context.mutex);
                proposed = CaptureConfigurationLocked();
            }
            // Library edits are disabled while a transfer is pending. Append only;
            // current controls and existing presets are never replaced by import.
            proposed.presets.insert(proposed.presets.end(), imported.begin(), imported.end());
            static_cast<void>(summon_lab::ConfigurationJson(proposed));
            const auto count = std::to_string(imported.size());
            const std::array<std::string_view, 1> args{count};
            std::scoped_lock lock(g_context.mutex);
            g_context.saved_presets = std::move(proposed.presets);
            g_context.presets_current_blueprint = 0;
            g_context.preset_filter = {};
            g_context.preset_page = 0;
            g_context.selected_preset = summon_lab::kNoPreset;
            g_context.preset_message = g_context.localizer.Format("presets.imported", "Imported {0} presets", args);
            ConfigurationChangedLocked();
        } else {
            WritePresetFile(path, summon_lab::ExportPresets(transfer->presets));
            std::scoped_lock lock(g_context.mutex);
            g_context.preset_message = g_context.localizer.Text("presets.exported", "Presets exported");
        }
    } catch (...) {
        std::scoped_lock lock(g_context.mutex);
        g_context.preset_message = g_context.localizer.Text("presets.transfer_failed", "Transfer failed; check the JSON file and path");
    }
    std::scoped_lock lock(g_context.mutex);
    g_context.preset_transfer.reset();
    g_context.preset_transfer_task = {};
}

void QueuePresetTransferLocked(bool importing, bool all) {
    if (g_context.preset_transfer || !g_context.scheduler) return;
    auto transfer = std::make_shared<PresetTransfer>();
    transfer->importing = importing;
    if (importing) transfer->path = g_context.preset_import_file;
    else {
        try {
            transfer->path = summon_lab::PathUtf8(PresetExportPath(
                g_context.preset_export_folder, g_context.preset_export_name.data()));
        } catch (...) {
            g_context.preset_message = g_context.localizer.Text("presets.invalid_filename", "Enter a JSON file name without a path");
            return;
        }
    }
    if (!importing) {
        if (all) transfer->presets = g_context.saved_presets;
        else if (g_context.selected_preset < g_context.saved_presets.size())
            transfer->presets.push_back(g_context.saved_presets[g_context.selected_preset]);
        if (transfer->presets.empty()) return;
    }
    g_context.preset_transfer = std::move(transfer);
    g_context.preset_message = g_context.localizer.Text("presets.transferring", "Transferring presets");
    const auto status = g_context.scheduler->schedule(g_context.scheduler->user, 0,
        TransferPresetsWorker, nullptr, &g_context.preset_transfer_task);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        g_context.preset_transfer.reset();
        g_context.preset_message = g_context.localizer.Text("presets.transfer_failed", "Transfer failed; check the JSON file and path");
    }
}

// ---------------------------------------------------------------- 安全读 / 日志

bool ReadBytes(std::uintptr_t address, void* destination, std::size_t size) {
    if (g_context.core == nullptr || g_context.core->read_memory == nullptr || address == 0 ||
        destination == nullptr || size == 0)
        return false;
    AnomalyMutableByteSpanV1 span{};
    span.data = static_cast<std::uint8_t*>(destination);
    span.size = size;
    return g_context.core->read_memory(g_context.core->user, address, span).code ==
           ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool ReadValue(std::uintptr_t address, T* out) {
    return ReadBytes(address, static_cast<void*>(out), sizeof(T));
}

// ---------------------------------------------------------------- invoke

bool HasInvoke(const AnomalyUe5ProcessEventServiceV1* service) {
    if (service == nullptr) return false;
    const std::size_t required = offsetof(AnomalyUe5ProcessEventServiceV1, invoke) +
                                 sizeof(AnomalyUe5ProcessEventServiceV1::invoke);
    return service->struct_size >= required && service->invoke != nullptr;
}

bool FindHandle(const char* path, AnomalyGenerationHandleV1* handle) {
    if (g_context.objects == nullptr || g_context.objects->find_exact == nullptr) return false;
    handle->id = 0;
    handle->generation = 0;
    return g_context.objects->find_exact(g_context.objects->user, anomaly::sdk::StringView(path),
                                         handle).code == ANOMALY_STATUS_V1_OK &&
           handle->id != 0;
}

bool InvokeByPath(AnomalyGenerationHandleV1 object, const char* path, void* parms,
                  std::size_t size) {
    if (!g_context.has_invoke || g_context.events == nullptr) return false;
    AnomalyGenerationHandleV1 function{};
    if (!FindHandle(path, &function)) return false;
    const auto status =
        g_context.events->invoke(g_context.events->user, object, function, parms, size);
    return status.code == ANOMALY_STATUS_V1_OK;
}

// ---------------------------------------------------------------- 对象工具

bool ProbeHandle(AnomalyGenerationHandleV1 handle) {
    if (g_context.objects == nullptr || g_context.objects->snapshot_by_handle == nullptr) return false;
    AnomalyUe5ObjectSnapshotV1 probe{};
    probe.struct_size = sizeof(probe);
    return g_context.objects->snapshot_by_handle(g_context.objects->user, handle, &probe).code ==
           ANOMALY_STATUS_V1_OK;
}

// 裸指针 -> ue5.objects handle：读 InternalIndex 后由 snapshot_at 取回带正确 serial 的 handle
bool HandleFromPointer(std::uintptr_t pointer, AnomalyGenerationHandleV1* out) {
    if (pointer == 0 || g_context.objects == nullptr || g_context.objects->snapshot_at == nullptr)
        return false;
    std::uint32_t internal_index{};
    if (!ReadValue(pointer + static_cast<std::uintptr_t>(g_layout.objectInternalIndex), &internal_index)) return false;
    AnomalyUe5ObjectSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (g_context.objects->snapshot_at(g_context.objects->user, internal_index, &snapshot).code !=
        ANOMALY_STATUS_V1_OK)
        return false;
    *out = snapshot.handle;
    return out->id != 0;
}

// ---------------------------------------------------------------- session 就绪门

void ClearSummonedState();

void UpdateSessionState() {
    if (g_context.session == nullptr) return;

    bool clear_state = false;
    if (g_context.session->snapshot != nullptr) {
        AnomalyNteSessionSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        const bool ready = g_context.session->snapshot(g_context.session->user, &snapshot).code ==
            ANOMALY_STATUS_V1_OK && snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY;
        std::scoped_lock lock(g_context.mutex);
        clear_state = g_context.session_world_ready && !ready;
        g_context.session_world_ready = ready;
    }

    if (g_context.session->next_event != nullptr &&
        g_context.session->latest_event_sequence != nullptr) {
        const std::uint64_t latest =
            g_context.session->latest_event_sequence(g_context.session->user);
        for (int guard = 0; guard < 16 && g_context.session_event_cursor < latest; ++guard) {
            AnomalyNteSessionEventV1 event{};
            event.struct_size = sizeof(event);
            if (g_context.session->next_event(g_context.session->user,
                                              g_context.session_event_cursor, &event).code !=
                ANOMALY_STATUS_V1_OK)
                break;
            g_context.session_event_cursor = event.sequence;
            if (event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED ||
                event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE) {
                clear_state = true;
            }
        }
    }
    if (clear_state) ClearSummonedState();
}

// ---------------------------------------------------------------- 本地玩家

bool FindLocalPawn(AnomalyGenerationHandleV1* out) {
    out->id = 0;
    out->generation = 0;
    if (g_context.player == nullptr) return false;

    constexpr std::size_t kRequired =
        offsetof(AnomalyNtePlayerServiceV1, get_object_handles) +
        sizeof(AnomalyNtePlayerServiceV1::get_object_handles);
    if (g_context.player->struct_size < kRequired ||
        g_context.player->get_object_handles == nullptr) {
        return false;
    }

    AnomalyGenerationHandleV1 pawn{}, controller{};
    const auto status =
        g_context.player->get_object_handles(g_context.player->user, &pawn, &controller);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    if (pawn.id == 0) {
        // SummonAI 是 HTAbilityCharacter 的方法，Controller 不能当 receiver
        return false;
    }
    *out = pawn;
    return true;
}

// ---------------------------------------------------------------- 蓝图目录

// Only gameplay policy is shipped; the class/name catalog comes from ue5.assets.
// 宿主把整个插件包原样复制到 .cache\<pid>\packages\<id>\<generation>\ 再加载 DLL，
// assets/ 在 DLL 旁边；profile/ 由宿主的 anomaly.profile 服务加载。
std::wstring PackagePath(const wchar_t* relative_path) {
    std::array<wchar_t, 4096> buffer{};
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PackagePath), &module) == FALSE ||
        module == nullptr)
        return {};
    const DWORD size = GetModuleFileNameW(module, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    std::filesystem::path path =
        std::filesystem::path(std::wstring(buffer.data(), size)).parent_path() / relative_path;
    // The host may load an extended-length (\\?\) path, which does not normalize '/'.
    path.make_preferred();
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path.wstring() : std::wstring{};
}

bool LoadCatalogPolicy() {
    const auto path = PackagePath(L"assets/catalog-policy.json");
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const auto root = nlohmann::json::parse(input, nullptr, false);
    if (!root.is_object() || root.value("schemaVersion", 0) != 1) return false;
    g_context.catalog_base = root.at("baseClass").get<std::string>();
    g_context.catalog_name_table = root.at("nameTable").get<std::string>();
    g_context.catalog_exclusions = root.at("excludedBlueprints").get<std::set<std::string, std::less<>>>();
    for (const auto& [key, value] : root.at("blueprints").items()) {
        BlueprintOptions options;
        options.use_catalog_name = value.value("useCatalogName", false);
        options.normal_combat_blueprint = value.value("normalCombatBlueprint", std::string{});
        g_context.catalog_policy.emplace(key, std::move(options));
    }
    g_context.catalog_name_aliases = root.at("nameAliases").get<decltype(g_context.catalog_name_aliases)>();
    const auto& ft = root.at("finalTower");
    g_context.final_tower_scene_asset = ft.at("sceneDataAsset").get<std::string>();
    g_context.final_tower_aliases = ft.at("configAliases").get<std::map<std::string, std::string, std::less<>>>();
    g_context.final_tower_combat_blueprint = ft.at("serverCombatBlueprint").get<std::string>();
    return true;
}

void SetScanStatus(const char* key, const char* fallback, std::string_view argument = {}) {
    const std::array<std::string_view, 1> args{argument};
    const auto text = argument.empty() ? g_context.localizer.Text(key, fallback) :
        g_context.localizer.Format(key, fallback, args);
    std::scoped_lock lock(g_context.mutex);
    std::snprintf(g_context.catalog_scan_status.data(), g_context.catalog_scan_status.size(), "%s", text.c_str());
}

void LogCatalogFailure(const AnomalyStatusV1& status) {
    // Preserve the service's diagnostic detail without adding technical UI text.
    if (g_context.core && g_context.core->log && status.message.data && status.message.size)
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, status.message);
}

constexpr char kCatalogCacheFile[] = "catalog-cache.json";
struct CatalogCollection {
    std::vector<std::string> paths;
    std::map<std::string, std::string, std::less<>> names;
};

bool PublishCatalog(CatalogCollection& collection) {
    if (collection.paths.empty() || collection.names.empty()) return false;
    std::sort(collection.paths.begin(), collection.paths.end());
    collection.paths.erase(std::unique(collection.paths.begin(), collection.paths.end()), collection.paths.end());
    auto table = std::make_shared<summon_lab::ActorNameTable>();
    std::set<std::string> ambiguous;
    for (const auto& [key, name] : collection.names) {
        if (name.empty()) continue;
        auto identity = summon_lab::NameIdentity(key);
        if (identity.empty() || ambiguous.contains(identity)) continue;
        const auto [at, added] = table->emplace(identity, name);
        if (!added && at->second != name) { table->erase(at); ambiguous.insert(std::move(identity)); }
    }
    for (const auto& [identity, key] : g_context.catalog_name_aliases) {
        const auto name = collection.names.find(key);
        if (name != collection.names.end() && !name->second.empty())
            table->insert_or_assign(summon_lab::NameIdentity(identity), name->second);
    }
    std::vector<ClassEntry> parsed;
    parsed.reserve(collection.paths.size());
    auto labels = std::make_shared<Context::CatalogLabels>();
    auto options = g_context.catalog_policy;
    for (const auto& path : collection.paths) {
        if (path.size() > kPathCapacity - 1 || !path.starts_with('/') || !path.ends_with("_C") ||
            path.find('\0') != std::string::npos || path.find('.') == std::string::npos) return false;
        if (g_context.catalog_exclusions.contains(path)) continue;
        const auto name = path.substr(path.rfind('.') + 1);
        const auto names = summon_lab::ResolveBlueprintNames(name, *table);
        const auto& display = names.specific.empty() ? names.family : names.specific;
        const auto label = display.empty() ? name : display + " | " + name;
        parsed.push_back({label, path, label + "\n" + path + "\n" + names.family});
        labels->emplace(path, label);
        if (auto it = options.find(path); it != options.end() && it->second.use_catalog_name)
            it->second.default_display_name = display;
    }
    for (auto& [path, option] : options) {
        if (option.default_display_name.size() >= kDisplayNameCapacity) return false;
        auto& alternative = option.normal_combat_blueprint;
        if (alternative.size() >= kPathCapacity || (!alternative.empty() && alternative == path)) return false;
        if (!alternative.empty() && !labels->contains(alternative)) alternative.clear();
    }
    if (parsed.empty() || g_context.catalog_stop.load(std::memory_order_acquire)) return false;
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.catalog.swap(parsed);
        g_context.blueprint_options.swap(options);
        g_context.catalog_loaded = true;
        g_context.blueprint_config = {};
        g_context.advanced_config = {};
        g_context.catalog_labels.store(std::move(labels), std::memory_order_release);
    }
    return true;
}

void LoadCatalogCache() {
    if (!g_context.storage || g_context.catalog_base.empty()) return;
    std::size_t size{};
    const auto storage = g_context.storage;
    const auto path = anomaly::sdk::StringView(kCatalogCacheFile);
    if (storage->read(storage->user, path, {}, &size).code != ANOMALY_STATUS_V1_OK ||
        size == 0 || size > 4U * 1024U * 1024U) return;
    std::vector<std::uint8_t> bytes(size);
    if (storage->read(storage->user, path, {bytes.data(), bytes.size()}, &size).code != ANOMALY_STATUS_V1_OK) return;
    const auto root = nlohmann::json::parse(bytes.begin(), bytes.begin() + size, nullptr, false);
    if (!root.is_object() || root.value("schemaVersion", 0) != 1 ||
        root.value("baseClass", std::string{}) != g_context.catalog_base ||
        root.value("nameTable", std::string{}) != g_context.catalog_name_table) return;
    CatalogCollection collection;
    collection.paths = root.at("classes").get<std::vector<std::string>>();
    collection.names = root.at("names").get<decltype(collection.names)>();
    if (PublishCatalog(collection)) SetScanStatus("catalog.scan.cached", "Cached catalog");
}

int ANOMALY_CALL CollectAssetClass(void* user, AnomalyStringViewV1 path) noexcept {
    try {
        if (g_context.catalog_stop.load(std::memory_order_acquire)) return 0;
        static_cast<CatalogCollection*>(user)->paths.emplace_back(path.data, path.size);
        return 1;
    } catch (...) { return 0; }
}
int ANOMALY_CALL CollectAssetName(void* user, AnomalyStringViewV1 key, AnomalyStringViewV1 name) noexcept {
    try {
        if (g_context.catalog_stop.load(std::memory_order_acquire)) return 0;
        static_cast<CatalogCollection*>(user)->names.emplace(std::string(key.data, key.size), std::string(name.data, name.size));
        return 1;
    } catch (...) { return 0; }
}

void ANOMALY_CALL RefreshCatalogWorker(void*, AnomalyGenerationHandleV1) noexcept {
    struct Complete { ~Complete() { g_context.catalog_busy.store(false, std::memory_order_release); } } complete;
    try {
        CatalogCollection collection;
        const auto assets = g_context.assets;
        auto result = assets->enumerate_derived(assets->user, anomaly::sdk::StringView(g_context.catalog_base), CollectAssetClass, &collection);
        if (result.code == ANOMALY_STATUS_V1_OK)
            result = assets->enumerate_string_table(assets->user, anomaly::sdk::StringView(g_context.catalog_name_table), CollectAssetName, &collection);
        if (g_context.catalog_stop.load(std::memory_order_acquire)) return;
        if (result.code != ANOMALY_STATUS_V1_OK || !PublishCatalog(collection)) {
            LogCatalogFailure(result);
            SetScanStatus("catalog.scan.failed", "Catalog refresh failed");
            return;
        }
        const auto document = nlohmann::json{{"schemaVersion", 1}, {"baseClass", g_context.catalog_base},
            {"nameTable", g_context.catalog_name_table}, {"classes", collection.paths}, {"names", collection.names}}.dump();
        const auto storage = g_context.storage;
        if (!storage || storage->write_atomic(storage->user, anomaly::sdk::StringView(kCatalogCacheFile),
                {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()}).code != ANOMALY_STATUS_V1_OK) {
            SetScanStatus("catalog.scan.cache_failed", "Catalog ready, cache unavailable");
            return;
        }
        SetScanStatus("catalog.scan.done", "Catalog ready: {0} classes", std::to_string(collection.paths.size()));
    } catch (...) { SetScanStatus("catalog.scan.failed", "Catalog refresh failed"); }
}

void AdvanceCatalogPreparation() {
    if (g_context.catalog_stop.load(std::memory_order_acquire)) return;
    if (!g_context.catalog_preparing && !g_context.catalog_busy.load(std::memory_order_acquire) &&
        g_context.catalog_refresh_requested.exchange(false, std::memory_order_acq_rel)) {
        g_context.catalog_preparing = true;
        g_context.catalog_preparation_started = std::chrono::steady_clock::now();
        g_context.catalog_busy.store(true, std::memory_order_release);
        SetScanStatus("catalog.scan.running", "Refreshing catalog");
    }
    if (!g_context.catalog_preparing) return;
    const auto assets = g_context.assets;
    if (!assets || !g_context.scheduler || g_context.catalog_base.empty()) {
        SetScanStatus("catalog.scan.unavailable", "Asset registry is unavailable");
        g_context.catalog_preparing = false;
        g_context.catalog_busy.store(false, std::memory_order_release);
        return;
    }
    auto result = assets->prepare_class(assets->user, anomaly::sdk::StringView(g_context.catalog_base));
    if (result.code == ANOMALY_STATUS_V1_OK)
        result = assets->prepare_string_table(assets->user, anomaly::sdk::StringView(g_context.catalog_name_table));
    if (result.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
        std::chrono::steady_clock::now() - g_context.catalog_preparation_started < std::chrono::seconds(30)) return;
    g_context.catalog_preparing = false;
    if (result.code == ANOMALY_STATUS_V1_OK)
        result = g_context.scheduler->schedule(g_context.scheduler->user, 0, RefreshCatalogWorker, nullptr, &g_context.catalog_task);
    if (result.code != ANOMALY_STATUS_V1_OK) {
        LogCatalogFailure(result);
        SetScanStatus("catalog.scan.failed", "Catalog refresh failed");
        g_context.catalog_busy.store(false, std::memory_order_release);
    }
}

// ---------------------------------------------------------------- 软类引用

bool BuildSoftClassRef(AnomalyGenerationHandleV1 lib_cdo, const char* utf8_path,
                       summon_lab::SoftClassRef* output) {
    static wchar_t path_buffer[kPathCapacity];
    std::size_t length = 0;
    while (utf8_path[length] != '\0' && length + 1 < kPathCapacity) {
        path_buffer[length] = static_cast<wchar_t>(static_cast<unsigned char>(utf8_path[length]));
        ++length;
    }
    path_buffer[length] = L'\0';
    const std::int32_t count = static_cast<std::int32_t>(length) + 1;

    summon_lab::MakeSoftClassPath make{{path_buffer, count, count}};
    if (!InvokeByPath(lib_cdo, kPathMakeSoftClassPath, &make, sizeof(make))) return false;

    summon_lab::ConvertSoftClass conv{make.result};
    if (!InvokeByPath(lib_cdo, kPathConvSoftClassRef, &conv, sizeof(conv))) return false;

    *output = conv.result;
    return true;
}

// ---------------------------------------------------------------- 属性读写

bool ReadHealth(AnomalyGenerationHandleV1 target, float* hp, float* max_hp) {
    float hp_parms{};
    const bool hp_ok = InvokeByPath(target, kPathGetHP, &hp_parms, sizeof(hp_parms));
    summon_lab::HealthMaximum max_parms{};
    const bool max_ok = InvokeByPath(target, kPathGetHPMax, &max_parms, sizeof(max_parms));

    if (hp_ok) *hp = hp_parms;
    if (max_ok) *max_hp = max_parms.result;
    return hp_ok && max_ok;
}

bool IsActorUsable(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer) {
    // A readable address may already belong to another UObject after GC.
    // Validate the saved serial/generation before inspecting or passing that address.
    if (pointer == 0 || !ProbeHandle(handle)) return false;
    std::uint32_t index{};
    if (!ReadValue(pointer + g_layout.objectInternalIndex, &index) ||
        index != ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)) return false;

    // Registry membership alone includes actors pending destruction. Use the
    // engine's lifetime checks before calling gameplay methods on such actors.
    AnomalyGenerationHandleV1 library{};
    summon_lab::ObjectBool valid{pointer};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) ||
        !InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:IsValid",
                      &valid, sizeof(valid)) || valid.value == 0) return false;
    std::uint8_t destroying{};
    return InvokeByPath(handle, "/Script/Engine.Actor:IsActorBeingDestroyed",
                        &destroying, sizeof(destroying)) && destroying == 0;
}

bool IsActorAlive(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer) {
    if (!IsActorUsable(handle, pointer)) return false;
    std::uint8_t dead{};
    return InvokeByPath(handle, "/Script/HTGame.HTAbilityCharacter:GetIsDead",
                        &dead, sizeof(dead)) && dead == 0;
}

bool ResolveName(std::uint32_t name_id, char* destination, std::size_t capacity) {
    if (name_id == 0 || destination == nullptr || capacity == 0 || g_context.names == nullptr ||
        g_context.names->resolve_utf8 == nullptr)
        return false;
    std::size_t size = capacity;
    return g_context.names->resolve_utf8(g_context.names->user, name_id, destination, &size).code ==
           ANOMALY_STATUS_V1_OK;
}

bool IsGameplayAttributeProperty(std::uintptr_t field, int32_t* offset_out) {
    if (field == 0 || offset_out == nullptr) return false;
    std::uintptr_t field_class{};
    std::uint32_t field_class_name{};
    std::int32_t array_dim{};
    std::int32_t element_size{};
    std::int32_t offset{};
    if (!ReadValue(field + g_layout.fieldClass, &field_class) || field_class == 0 ||
        !ReadValue(field_class + g_layout.fieldClassName, &field_class_name) || !ReadValue(field + g_layout.propertyArrayDim, &array_dim) ||
        !ReadValue(field + g_layout.propertyElementSize, &element_size) ||
        !ReadValue(field + g_layout.propertyOffset, &offset))
        return false;

    char field_class_text[64]{};
    if (!ResolveName(field_class_name, field_class_text, sizeof(field_class_text)) ||
        std::strcmp(field_class_text, "StructProperty") != 0)
        return false;

    std::uintptr_t value_struct{};
    std::uint32_t value_struct_name{};
    if (!ReadValue(field + g_layout.propertyStruct, &value_struct) || value_struct == 0 ||
        !ReadValue(value_struct + g_layout.objectName, &value_struct_name))
        return false;
    char value_struct_text[64]{};
    if (!ResolveName(value_struct_name, value_struct_text, sizeof(value_struct_text)) ||
        std::strcmp(value_struct_text, "GameplayAttributeData") != 0 || array_dim != 1 ||
        element_size != static_cast<std::int32_t>(g_layout.attributeDataSize) || offset < 0 || offset >= 0x4000)
        return false;
    *offset_out = offset;
    return true;
}

std::uintptr_t FindProperty(std::uintptr_t uclass, const char* wanted,
                            std::uint32_t name_number = 0) {
    if (uclass == 0 || wanted == nullptr) return 0;
    for (int class_guard = 0; class_guard < 16 && uclass != 0; ++class_guard) {
        std::uintptr_t field{};
        if (!ReadValue(uclass + g_layout.structProperties, &field)) return 0;
        for (int field_guard = 0; field_guard < 512 && field != 0; ++field_guard) {
            std::uint64_t property_name{};
            char name[kAttributeNameCapacity]{};
            if (ReadValue(field + g_layout.fieldName, &property_name) &&
                static_cast<std::uint32_t>(property_name >> 32) == name_number &&
                ResolveName(static_cast<std::uint32_t>(property_name), name, sizeof(name)) &&
                std::strcmp(name, wanted) == 0)
                return field;
            if (!ReadValue(field + g_layout.fieldNext, &field)) return 0;
        }
        if (!ReadValue(uclass + g_layout.structSuper, &uclass)) return 0;
    }
    return 0;
}

bool RebaseWuchaoArena(AnomalyGenerationHandleV1 actor, std::uintptr_t pointer,
                      std::uintptr_t actor_class) {
    auto parent = actor_class;
    bool wuchao{};
    for (int guard = 0; guard < 16 && parent != 0; ++guard) {
        std::uint32_t id{};
        char name[kAttributeNameCapacity]{};
        if (!ReadValue(parent + g_layout.objectName, &id) ||
            !ResolveName(id, name, sizeof(name))) return false;
        // Game identifiers: this blueprint's skills use absolute arena points.
        if (std::strcmp(name, "Boss_32_Wuchao_BP_C") == 0) { wuchao = true; break; }
        if (!ReadValue(parent + g_layout.structSuper, &parent)) return false;
    }
    if (!wuchao) return true;

    // The two Act10_LOC properties differ in FName::Number. Preserve their
    // identities and relative positions; only rebase this summoned instance.
    struct Point {
        const char* name;
        std::uint32_t number;
        std::uint64_t property_name{};
        std::int32_t offset{};
        std::array<double, 3> value{};
    };
    std::array<Point, 4> points{{{"Scene_center", 0}, {"Act28_LOC", 0},
                                {"Act10_LOC", 2}, {"Act10_LOC", 3}}};
    for (auto& point : points) {
        const auto property = FindProperty(actor_class, point.name, point.number);
        std::uintptr_t field_class{};
        std::uint32_t type_id{};
        std::int32_t size{}, dimension{};
        char type[64]{};
        if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
            !ReadValue(field_class + g_layout.fieldClassName, &type_id) || !ResolveName(type_id, type, sizeof(type)) ||
            std::strcmp(type, "StructProperty") != 0 ||
            !ReadValue(property + g_layout.propertyElementSize, &size) || size != sizeof(point.value) ||
            !ReadValue(property + g_layout.propertyArrayDim, &dimension) || dimension != 1 ||
            !ReadValue(property + g_layout.propertyOffset, &point.offset) || point.offset < 0 ||
            !ReadValue(property + g_layout.fieldName, &point.property_name) ||
            !ReadValue(pointer + point.offset, &point.value) ||
            !std::all_of(point.value.begin(), point.value.end(), [](double v) { return std::isfinite(v); }))
            return false;
    }
    AnomalyGenerationHandleV1 library{};
    std::array<double, 3> location{};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) ||
        !InvokeByPath(actor, "/Script/Engine.Actor:K2_GetActorLocation",
                       location.data(), sizeof(location)) ||
        !std::all_of(location.begin(), location.end(), [](double v) { return std::isfinite(v); }))
        return false;

    const auto center = points[0].value;
    for (const auto& point : points) {
        struct {
            std::uintptr_t object;
            std::uint64_t property_name;
            std::array<double, 3> value;
        } params{pointer, point.property_name, {}};
        static_assert(sizeof(params) == 40);
        for (std::size_t axis = 0; axis < location.size(); ++axis)
            params.value[axis] = location[axis] + (point.value[axis] - center[axis]);
        // The native setter verifies FVector's reflected type and owns the
        // assignment. No blueprint offsets, class defaults or script bytes change.
        std::array<double, 3> actual{};
        if (!InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:SetVectorPropertyByName",
                           &params, sizeof(params)) ||
            !ReadValue(pointer + point.offset, &actual) || actual != params.value) return false;
    }
    if (g_context.core != nullptr && g_context.core->log != nullptr) {
        char detail[160]{};
        std::snprintf(detail, sizeof(detail), "SummonLab arena: actor=%p center=(%.1f, %.1f, %.1f)",
            reinterpret_cast<void*>(pointer), location[0], location[1], location[2]);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                            anomaly::sdk::StringView(detail));
    }
    return true;
}

bool SetSpawnDisplayName(AnomalyGenerationHandleV1 actor, std::uintptr_t pointer,
                         std::string_view name) {
    if (name.empty()) return true;
    if (!IsActorAlive(actor, pointer)) return false;
    std::uintptr_t attributes{};
    if (!InvokeByPath(actor, "/Script/HTGame.HTAbilityCharacter:GetHTCharacterAttributeSet",
                      &attributes, sizeof(attributes)) || attributes == 0) return false;
    // Only the new actor's instance may be renamed; shared defaults stay owned
    // by the engine. Obtain the outer through reflection, not a new layout.
    AnomalyGenerationHandleV1 system_library{}, text_library{}, conversion{}, setter{};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &system_library) ||
        !FindHandle("/Script/Engine.Default__KismetTextLibrary", &text_library) ||
        !FindHandle("/Script/Engine.KismetTextLibrary:Conv_StringToText", &conversion) ||
        !FindHandle("/Script/Engine.KismetSystemLibrary:SetTextPropertyByName", &setter)) return false;
    std::array<std::uintptr_t, 2> outer{attributes, 0};
    if (!InvokeByPath(system_library, "/Script/Engine.KismetSystemLibrary:GetOuterObject",
                      outer.data(), sizeof(outer)) || outer[1] != pointer) return false;

    std::uintptr_t uclass{}, field_class{};
    std::uint32_t field_class_name{};
    std::int32_t offset{}, element_size{}, array_dim{};
    std::uint64_t property_name{};
    if (!ReadValue(attributes + g_layout.objectClass, &uclass)) return false;
    const auto property = FindProperty(uclass, "DescriptiveName");
    char type[64]{};
    if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
        !ReadValue(field_class + g_layout.fieldClassName, &field_class_name) ||
        !ResolveName(field_class_name, type, sizeof(type)) || std::strcmp(type, "TextProperty") != 0 ||
        !ReadValue(property + g_layout.propertyElementSize, &element_size) || element_size != 16 ||
        !ReadValue(property + g_layout.propertyArrayDim, &array_dim) || array_dim != 1 ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(property + g_layout.fieldName, &property_name) ||
        g_context.names->resolve_ftext_utf8 == nullptr) return false;

    std::array<wchar_t, kDisplayNameCapacity> wide{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), wide.data(), static_cast<int>(wide.size() - 1));
    if (length <= 0) return false;
    auto& value = g_context.name_text;
    value.input = wide.data();
    value.count = length + 1;
    value.capacity = length + 1;
    const auto converted = g_context.events->invoke(g_context.events->user, text_library,
        conversion, &value, sizeof(value));
    value.input = nullptr;
    value.count = value.capacity = 0;
    if (converted.code != ANOMALY_STATUS_V1_OK) return false;

    // SetTextPropertyByName performs the FText reference-counted assignment.
    // The argument borrows the scratch text; no UObject/FText bytes are patched.
    summon_lab::TextProperty parameters{attributes, property_name, value.text};
    if (g_context.events->invoke(g_context.events->user, system_library, setter,
            &parameters, sizeof(parameters)).code != ANOMALY_STATUS_V1_OK) return false;
    std::array<char, kDisplayNameCapacity> actual{};
    std::size_t size = actual.size();
    return g_context.names->resolve_ftext_utf8(g_context.names->user,
        attributes + static_cast<std::uintptr_t>(offset), actual.data(), &size).code ==
            ANOMALY_STATUS_V1_OK && name == actual.data();
}

bool ReadAttributeValues(std::uintptr_t attribute_set, std::vector<AttributeValue>* values) {
    if (attribute_set == 0 || values == nullptr) return false;
    std::uintptr_t uclass{};
    if (!ReadValue(attribute_set + g_layout.objectClass, &uclass) || uclass == 0) return false;

    std::vector<AttributeValue> parsed;
    for (int class_guard = 0; class_guard < 16 && uclass != 0; ++class_guard) {
        std::uintptr_t field{};
        if (!ReadValue(uclass + g_layout.structProperties, &field)) return false;
        for (int field_guard = 0; field_guard < 512 && field != 0; ++field_guard) {
            int32_t offset{};
            std::uint32_t name_id{};
            if (IsGameplayAttributeProperty(field, &offset) &&
                ReadValue(field + g_layout.fieldName, &name_id) && name_id != 0) {
                AttributeValue value;
                char name[kAttributeNameCapacity]{};
                if (ResolveName(name_id, name, sizeof(name)) &&
                    ReadValue(attribute_set + static_cast<std::uintptr_t>(offset) +
                                  g_layout.attributeBaseValue,
                              &value.base) &&
                    ReadValue(attribute_set + static_cast<std::uintptr_t>(offset) +
                                  g_layout.attributeCurrentValue,
                              &value.current)) {
                    value.name = name;
                    parsed.push_back(std::move(value));
                }
            }
            if (!ReadValue(field + g_layout.fieldNext, &field)) return false;
        }
        if (!ReadValue(uclass + g_layout.structSuper, &uclass)) return false;
    }
    *values = std::move(parsed);
    return true;
}

// Resolve the fields that moved between the supplied 1.4b and 1.5a SDKs by
// reflected name/type/size, rather than trying multiple offsets on an object.
template <typename T>
bool ReadReflectedValue(std::uintptr_t object, const char* name,
                        std::string_view expected_type, T* value) {
    std::uintptr_t uclass{}, field_class{};
    std::uint32_t type_id{};
    std::int32_t offset{}, size{}, dimension{};
    char type[64]{};
    if (object == 0 || !ReadValue(object + g_layout.objectClass, &uclass)) return false;
    const auto property = FindProperty(uclass, name);
    return property != 0 && ReadValue(property + g_layout.fieldClass, &field_class) &&
        ReadValue(field_class + g_layout.fieldClassName, &type_id) && ResolveName(type_id, type, sizeof(type)) &&
        (expected_type == type || (expected_type == "ByteProperty" && std::strcmp(type, "EnumProperty") == 0)) &&
        ReadValue(property + g_layout.propertyElementSize, &size) && size == sizeof(T) &&
        ReadValue(property + g_layout.propertyArrayDim, &dimension) && dimension == 1 &&
        ReadValue(property + g_layout.propertyOffset, &offset) && offset >= 0 &&
        ReadValue(object + offset, value);
}

template <typename T>
bool ReadRowValue(std::uintptr_t row, std::uintptr_t row_struct, const char* name,
                  std::string_view expected_type, T* value) {
    const auto property = FindProperty(row_struct, name);
    std::uintptr_t field_class{};
    std::uint32_t type_id{};
    std::int32_t offset{}, size{}, dimension{};
    char type[64]{};
    return property != 0 && ReadValue(property + g_layout.fieldClass, &field_class) &&
        ReadValue(field_class + g_layout.fieldClassName, &type_id) && ResolveName(type_id, type, sizeof(type)) &&
        expected_type == type && ReadValue(property + g_layout.propertyElementSize, &size) &&
        size == sizeof(T) && ReadValue(property + g_layout.propertyArrayDim, &dimension) &&
        dimension == 1 && ReadValue(property + g_layout.propertyOffset, &offset) &&
        offset >= 0 && ReadValue(row + offset, value);
}

bool ReadCamp(std::uintptr_t actor, std::uint8_t* camp) {
    return ReadReflectedValue(actor, "CampType", "ByteProperty", camp);
}

bool ResolveActorAttributeSet(std::uintptr_t actor, std::uintptr_t* ability_system,
                              std::uintptr_t* attribute_set) {
    if (actor == 0 || ability_system == nullptr || attribute_set == nullptr) return false;
    *ability_system = 0;
    *attribute_set = 0;
    if (!ReadValue(actor + g_layout.characterAbilitySystem, ability_system)) return false;
    if (ReadValue(actor + g_layout.characterAttributeSet, attribute_set) && *attribute_set != 0)
        return true;
    if (*ability_system == 0) return false;
    if (ReadReflectedValue(*ability_system, "HTAttribute", "ObjectProperty", attribute_set) && *attribute_set != 0)
        return true;
    return ReadReflectedValue(*ability_system, "StandaloneHTAttribute", "ObjectProperty", attribute_set) &&
           *attribute_set != 0;
}

bool HealthMatches(float actual, float expected) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    return actual == expected;
}

bool ReadMatchingHealth(AnomalyGenerationHandleV1 actor, float expected,
                        float* hp, float* max_hp) {
    if (!ReadHealth(actor, hp, max_hp)) return false;
    summon_lab::HealthMaximum fixed_max{1};
    if (!InvokeByPath(actor, kPathGetHPMax, &fixed_max, sizeof(fixed_max))) return false;
    return HealthMatches(*hp, expected) && HealthMatches(*max_hp, expected) &&
           HealthMatches(fixed_max.result, expected);
}

// ---------------------------------------------------------------- 出生属性（只改变 GAS 初始化输入）

struct BirthAttribute {
    std::string name;
    float value{};
    bool applied{};
};

struct CombatConfigSource {
    std::string config_name;
    std::string row_name;
    std::string server_blueprint;
    std::uint64_t config{};
    std::uint64_t row_alias{};
    std::uint64_t pack{};
    std::uintptr_t row{};
    std::uintptr_t property_pack{};
    std::int32_t level{};
};

CombatConfigSource PrepareCombatConfig(std::string_view config_name, std::string_view source_blueprint = {});

struct BirthInitialization {
    std::uintptr_t actor_class{};
    std::uintptr_t actor{};
    std::uintptr_t attribute_set{};
    std::vector<BirthAttribute> attributes;
    float exact_hp{};
    std::uintptr_t registration_controller{};
    NativeIdentity registration_leader{};
    std::int32_t registration_index{-1};
    std::uintptr_t registration_cdo{};
    bool registration_substituted{};
    bool mute_boss_music{}, mute_boss_dialogue{};
    bool skip_cutscenes{};
    std::uintptr_t story_audio_actor{};
    std::vector<summon_lab::EditorField> editor_fields;
    std::uintptr_t editor_actor{};
    bool editor_attempted{}, editor_applied{};
};

using AttributeBaseInputFn = void (ANOMALY_CALL*)(void*, const void*, float);
std::atomic<DWORD> g_birth_thread{0};
BirthInitialization* g_birth_initialization{}; // Owned exclusively by g_birth_thread.

struct LocalBirthBinding {
    AnomalyGenerationHandleV1 actor_handle{};
    std::uintptr_t actor{};
    std::uintptr_t ability_system{};
    std::uintptr_t attribute_set{};
    float exact_hp{};
    CombatConfigSource combat_config;
    SummonIdentity registration;
    std::uint32_t health_observations{}, mirror_observations{}, damage_observations{};
};
struct PendingBirthConfiguration {
    AnomalyGenerationHandleV1 actor_handle{};
    AnomalyGenerationHandleV1 controller_handle{};
    std::uintptr_t ability_system{};
    ConfigureSummon parameters{};
    float exact_hp{};
    float native_max_hp{};
    bool invalid_max_hp{};
    bool require_native_capacity{};
};
std::mutex g_local_birth_mutex;
std::vector<LocalBirthBinding> g_local_births;
std::vector<PendingBirthConfiguration> g_birth_configurations;
std::atomic<bool> g_birth_configuration_ready{false};
std::vector<LocalBirthBinding> g_pending_local_births;
std::atomic<bool> g_restore_local_births{false};
std::atomic<bool> g_observe_ft_health{false};

void ForgetLocalBirth(AnomalyGenerationHandleV1 handle) {
    const auto same_actor = [&](const auto& entry) {
        return entry.actor_handle.id == handle.id &&
            entry.actor_handle.generation == handle.generation;
    };
    std::scoped_lock lock(g_local_birth_mutex);
    std::erase_if(g_local_births, same_actor);
    std::erase_if(g_birth_configurations, same_actor);
    g_observe_ft_health.store(std::any_of(g_local_births.begin(), g_local_births.end(),
        [](const auto& entry) { return entry.combat_config.config_name.starts_with("FT_"); }),
        std::memory_order_release);
}

void ClearSummonedState() {
    {
        std::scoped_lock lock(g_local_birth_mutex);
        g_local_births.clear();
        g_birth_configurations.clear();
    }
    g_birth_configuration_ready.store(false, std::memory_order_release);
    g_pending_local_births.clear();
    g_restore_local_births.store(false, std::memory_order_release);
    g_observe_ft_health.store(false, std::memory_order_release);
    g_context.health_events = {};
    g_context.health_event_lookup_pending = g_context.health_observer.id != 0;
    std::scoped_lock lock(g_context.mutex);
    g_context.records = {};
    g_context.selected_record_id = 0;
    g_context.management_page = 0;
    g_context.management_blueprint.clear();
    g_context.battles = {};
    g_context.aggro_edit_members.clear();
    g_context.aggro_edit = {};
    g_context.pending = {};
    g_context.summon_batch.reset();
    g_context.blueprint_config = {};
    g_context.advanced_config = {};
    g_context.blueprint_check_delay = 0;
    g_pending_hint.store(0, std::memory_order_release);
    g_context.editor.scan = {};
    g_context.editor.snapshot = {};
    g_context.editor.requested_path.clear();
    g_context.editor.load_requested = false;
    g_context.editor.record_cursor = 0;
}

void RegisterLocalBirth(AnomalyGenerationHandleV1 handle, std::uintptr_t actor,
                        std::uintptr_t ability_system, std::uintptr_t attribute_set,
                        float exact_hp, const CombatConfigSource& config = {},
                        const SummonIdentity& registration = {}) {
    std::scoped_lock lock(g_local_birth_mutex);
    g_local_births.erase(std::remove_if(g_local_births.begin(), g_local_births.end(),
        [](const LocalBirthBinding& binding) { return !ProbeHandle(binding.actor_handle); }),
        g_local_births.end());
    g_local_births.push_back({handle, actor, ability_system, attribute_set, exact_hp, config,
        registration});
    if (config.config_name.starts_with("FT_"))
        g_observe_ft_health.store(true, std::memory_order_release);
}

// Passive FT diagnostics: observe native replies before the engine consumes
// them. Never invoke gameplay code or retain a borrowed RPC parameter buffer.
bool DiagnosticStruct(std::uintptr_t property, const char* expected_name,
                      std::uintptr_t* type, std::int32_t* size) {
    std::uintptr_t field_class{};
    std::uint32_t id{};
    std::int32_t dimension{}, declared_size{};
    char name[96]{};
    return property != 0 && ReadValue(property + g_layout.fieldClass, &field_class) &&
        ReadValue(field_class + g_layout.fieldClassName, &id) && ResolveName(id, name, sizeof(name)) &&
        std::strcmp(name, "StructProperty") == 0 &&
        ReadValue(property + g_layout.propertyArrayDim, &dimension) && dimension == 1 &&
        ReadValue(property + g_layout.propertyElementSize, size) && *size > 0 && *size <= 512 &&
        ReadValue(property + g_layout.propertyStruct, type) && *type != 0 &&
        ReadValue(*type + g_layout.objectName, &id) && ResolveName(id, name, sizeof(name)) &&
        std::strcmp(name, expected_name) == 0 &&
        ReadValue(*type + g_layout.structSize, &declared_size) && declared_size == *size;
}

bool DiagnosticNestedStruct(std::uintptr_t data, std::uintptr_t owner, const char* field,
                            const char* expected_type, std::uintptr_t* nested_data,
                            std::uintptr_t* nested_type) {
    const auto property = FindProperty(owner, field);
    std::int32_t size{}, offset{}, owner_size{};
    if (!DiagnosticStruct(property, expected_type, nested_type, &size) ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(owner + g_layout.structSize, &owner_size) ||
        offset > owner_size || size > owner_size - offset) return false;
    *nested_data = data + offset;
    return true;
}

void ObserveFinalTowerHealth(const char* event, AnomalyGenerationHandleV1 controller,
                            std::uintptr_t data, std::uintptr_t type, const char* hp_field,
                            bool has_dead_state) {
    NativeIdentity identity{};
    float hp{};
    std::int32_t dead_state{-1};
    if (!ReadRowValue(data, type, "NetTarget", "StructProperty", &identity)) return;
    if (hp_field != nullptr) {
        if (!ReadRowValue(data, type, hp_field, "FloatProperty", &hp)) return;
    } else if (!ReadRowValue(data, type, "fCurHealth", "FloatProperty", &hp)) {
        // 1.5 nests CurrentHp in DamageDisplayInfo; 1.4 exposes fCurHealth.
        std::uintptr_t display{}, display_type{};
        if (!DiagnosticNestedStruct(data, type, "DamageDisplayInfo", "DamageDisplayInfo",
                &display, &display_type) ||
            !ReadRowValue(display, display_type, "CurrentHp", "FloatProperty", &hp)) return;
    }
    if (!std::isfinite(hp) ||
        (has_dead_state && !ReadRowValue(data, type, "DeadState", "IntProperty", &dead_state))) return;
    std::int32_t index{};
    std::memcpy(&index, identity.data() + g_layout.diagnosticSummonIndex, sizeof(index));
    std::uintptr_t actor{}, ability_system{};
    AnomalyGenerationHandleV1 handle{};
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (auto& binding : g_local_births) {
            const auto& registration = binding.registration;
            if (binding.health_observations >= 16 || index != registration.index ||
                controller.id != registration.controller.id ||
                controller.generation != registration.controller.generation ||
                std::memcmp(identity.data(), registration.leader.data(),
                    g_layout.diagnosticSummonIndex) != 0) continue;
            ++binding.health_observations;
            actor = binding.actor;
            ability_system = binding.ability_system;
            handle = binding.actor_handle;
            break;
        }
    }
    if (handle.id == 0 || !ProbeHandle(handle)) return;
    float local_hp{}, local_max{};
    const bool local_read = ReadReflectedValue(ability_system, "HPCurrent", "FloatProperty", &local_hp) &&
        ReadReflectedValue(ability_system, "MaxHPTemp", "FloatProperty", &local_max);
    char message[320]{};
    std::snprintf(message, sizeof(message),
        "SummonLab FT-health-reply: event=%s actor=%p index=%d incomingHP=%.9g deadState=%d localBefore=%.9g/%.9g localReadable=%s",
        event, reinterpret_cast<void*>(actor), index, hp, dead_state, local_hp, local_max,
        local_read ? "yes" : "no");
    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView(message));
}

void ObserveFinalTowerHealthArray(const char* event, AnomalyGenerationHandleV1 controller,
                                 std::uintptr_t data, std::uintptr_t type, const char* field,
                                 const char* element_name) {
    struct Array { std::uintptr_t data; std::int32_t count, capacity; } array{};
    if (!ReadRowValue(data, type, field, "ArrayProperty", &array) || array.data == 0 ||
        array.count <= 0 || array.count > array.capacity || array.count > 64) return;
    const auto property = FindProperty(type, field);
    std::uintptr_t inner{}, element_type{};
    std::int32_t stride{};
    if (!ReadValue(property + g_layout.arrayInner, &inner) ||
        !DiagnosticStruct(inner, element_name, &element_type, &stride)) return;
    for (std::int32_t i = 0; i < array.count; ++i)
        ObserveFinalTowerHealth(event, controller, array.data + static_cast<std::size_t>(i) * stride,
            element_type, nullptr, true);
}

void ANOMALY_CALL ObserveFinalTowerEvent(void*, std::uintptr_t object,
                                       std::uintptr_t function, void* parameters) noexcept {
    try {
        if (parameters == nullptr || !g_observe_ft_health.load(std::memory_order_acquire)) return;
        std::uint32_t index{};
        if (!ReadValue(function + g_layout.objectInternalIndex, &index)) return;
        for (std::size_t i = 0; i < g_context.health_events.size(); ++i) {
            const auto expected = g_context.health_events[i];
            if (expected.id == 0 || index != ANOMALY_UE5_OBJECT_HANDLE_INDEX(expected)) continue;
            AnomalyGenerationHandleV1 actual{}, controller{};
            if (!HandleFromPointer(function, &actual) || actual.id != expected.id ||
                actual.generation != expected.generation || !HandleFromPointer(object, &controller)) return;
            const auto data = reinterpret_cast<std::uintptr_t>(parameters);
            if (i < 2) {
                ObserveFinalTowerHealth(i == 0 ? "restore" : "set-current", controller, data,
                    function, i == 0 ? "CurrentHp" : "NewHP", false);
            } else if (i == 2) {
                std::uintptr_t container{}, container_type{};
                if (!DiagnosticNestedStruct(data, function, "TargetDataContainer",
                        "ClientReplicatedTargetDataContainer", &container, &container_type)) return;
                ObserveFinalTowerHealthArray("fight", controller, container, container_type,
                    "ClientFightDataArray", "ClientRepFightData");
                ObserveFinalTowerHealthArray("fight-extra", controller, container, container_type,
                    "ClientExtraDamageInfos", "ClientRepExtraDamageInfo");
            } else {
                ObserveFinalTowerHealthArray("extra", controller, data, function,
                    "TargetDataArray", "ClientRepExtraDamageInfo");
            }
            return;
        }
    } catch (...) {}
}

void ResolveFinalTowerHealthEvents() {
    if (!g_context.health_event_lookup_pending) return;
    g_context.health_event_lookup_pending = false;
    constexpr std::array<const char*, 4> paths{
        "/Script/HTGame.HTPlayerController:ClientRestoreServerAbilityAttributes",
        "/Script/HTGame.HTPlayerController:ClientServerAbilityActorSetCurrentHP",
        "/Script/HTGame.HTPlayerController:ClientSetReplicatedTargetData",
        "/Script/HTGame.HTPlayerController:ClientUpdateTargetExtraDamageInfos"};
    std::array<AnomalyGenerationHandleV1, 4> events{};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!FindHandle(paths[i], &events[i])) {
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: FT health observer function unavailable"));
            return;
        }
    }
    g_context.health_events = events;
    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView("SummonLab: passive FT health observer ready"));
}

void StartFinalTowerHealthObserver() {
    if (g_context.events == nullptr || g_context.events->subscribe == nullptr ||
        g_context.events->unsubscribe == nullptr || g_context.core == nullptr ||
        g_context.core->log == nullptr) return;
    const auto status = g_context.events->subscribe(g_context.events->user,
        ObserveFinalTowerEvent, nullptr, &g_context.health_observer);
    // Exact UObject lookup belongs to the next ready Game tick, not Start.
    g_context.health_event_lookup_pending = status.code == ANOMALY_STATUS_V1_OK;
    if (!g_context.health_event_lookup_pending)
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView("SummonLab: FT health observer unavailable"));
}

bool ReadNativeMirrorFloat(const MirrorEntry& entry, float* value,
                           bool require_positive = true) {
    std::uintptr_t data{};
    std::int32_t size{}, capacity{};
    std::memcpy(&data, entry.data() + g_layout.mirrorValueData, sizeof(data));
    std::memcpy(&size, entry.data() + g_layout.mirrorValueBytes, sizeof(size));
    std::memcpy(&capacity, entry.data() + g_layout.mirrorValueDataCapacity, sizeof(capacity));
    // 1.4's memory reader and 1.5's float NetSerializeItem/SerializeItem path
    // both consume four uncompressed bytes for this scalar.
    return entry[g_layout.mirrorValueCompressed] == 0 && size == sizeof(float) && capacity >= size &&
        ReadValue(data, value) && std::isfinite(*value) && (!require_positive || *value > 0.0F);
}

void ObserveFinalTowerMirror(std::uintptr_t ability_system, const char* name,
                            const MirrorEntry& entry) {
    float value{};
    if (!ReadNativeMirrorFloat(entry, &value, false)) return;
    std::uintptr_t actor{};
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (auto& binding : g_local_births) {
            if (binding.ability_system == ability_system && binding.mirror_observations < 12 &&
                binding.combat_config.config_name.starts_with("FT_")) {
                ++binding.mirror_observations;
                actor = binding.actor;
                break;
            }
        }
    }
    if (actor == 0 || g_context.core == nullptr || g_context.core->log == nullptr) return;
    char message[192]{};
    std::snprintf(message, sizeof(message), "SummonLab FT-health-mirror: actor=%p field=%s incoming=%.9g",
        reinterpret_cast<void*>(actor), name, value);
    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView(message));
}

void CaptureNativeBirthMaximum(std::uintptr_t ability_system,
                              const MirrorEntry& entry) {
    std::scoped_lock lock(g_local_birth_mutex);
    for (auto& pending : g_birth_configurations) {
        if (pending.ability_system != ability_system) continue;
        pending.invalid_max_hp = !ReadNativeMirrorFloat(entry, &pending.native_max_hp);
        g_birth_configuration_ready.store(true, std::memory_order_release);
        break;
    }
}

void ConfigureNativeBirthsOnGameThread() {
    if (!g_birth_configuration_ready.exchange(false, std::memory_order_acq_rel)) return;
    std::vector<PendingBirthConfiguration> ready;
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (auto it = g_birth_configurations.begin(); it != g_birth_configurations.end();) {
            if (it->native_max_hp > 0.0F || it->invalid_max_hp) {
                ready.push_back(*it);
                it = g_birth_configurations.erase(it);
            } else if (!ProbeHandle(it->actor_handle)) {
                it = g_birth_configurations.erase(it);
            } else ++it;
        }
    }
    for (auto& pending : ready) {
        if (!ProbeHandle(pending.actor_handle)) continue;
        // InitialHPPercent is relative to the native registration's maximum,
        // not the locally overridden GAS maximum. Sending 1 would seed the
        // native HP ledger with millions of HP and every reply would clamp to
        // the custom cap until all that invisible HP had been exhausted.
        const float percent = pending.invalid_max_hp ? 0.0F : pending.exact_hp / pending.native_max_hp;
        pending.parameters.hp_percent = percent;
        const bool within_capacity = !pending.require_native_capacity ||
            (std::isfinite(percent) && percent > 0.0F && percent <= 1.0F);
        const bool configured = !pending.invalid_max_hp && within_capacity &&
            InvokeByPath(pending.controller_handle,
                "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
                &pending.parameters, sizeof(pending.parameters));
        if (!configured) {
            static_cast<void>(InvokeByPath(pending.actor_handle, kPathDestroyActor, nullptr, 0));
            if (pending.require_native_capacity) {
                const auto message = g_context.localizer.Text("summon.compat_health_failed",
                    "Native source HP could not be synchronized; summon removed");
                std::scoped_lock lock(g_context.mutex);
                g_context.last_result_failed = true;
                std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s", message.c_str());
            }
        }
        if (g_context.core != nullptr) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "SummonLab birth-configuration: nativeMax=%.3f requestedHP=%.3f initialHPPercent=%.9g submitted=%s",
                pending.native_max_hp, pending.exact_hp, percent, configured ? "yes" : "no");
            g_context.core->log(g_context.core->user, configured ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
                ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
        }
    }
}

// Lifecycle-only state transfer: an actor survives a plugin DLL generation.
// Never adopt by class/name. Require the same process creation time and the
// engine's exact object serial/generation before restoring a local birth.
std::filesystem::path LocalBirthStatePath() {
    std::array<char, 4096> package{};
    std::size_t size = package.size();
    if (g_context.core == nullptr || g_context.core->plugin_directory == nullptr ||
        g_context.core->plugin_directory(g_context.core->user, package.data(), &size).code !=
            ANOMALY_STATUS_V1_OK) return {};
    const std::string utf8(package.data());
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).parent_path() /
           L"summon-lab-local-births.json";
}

std::uint64_t ProcessCreationTime() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

void TransferLocalBirths(bool save) noexcept {
    try {
        const auto path = LocalBirthStatePath();
        const auto created = ProcessCreationTime();
        if (path.empty() || created == 0) return;
        if (save) {
            nlohmann::json bindings = nlohmann::json::array();
            {
                std::scoped_lock lock(g_local_birth_mutex);
                for (const auto& binding : g_local_births)
                    bindings.push_back({{"id", binding.actor_handle.id},
                        {"generation", binding.actor_handle.generation},
                        {"actor", binding.actor}, {"abilitySystem", binding.ability_system},
                        {"attributeSet", binding.attribute_set},
                        {"exactHP", binding.exact_hp},
                        {"combatConfig", binding.combat_config.config_name},
                        {"combatSource", binding.combat_config.server_blueprint}});
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << nlohmann::json{{"schemaVersion", 1}, {"pid", GetCurrentProcessId()},
                {"processCreated", created}, {"bindings", std::move(bindings)}}.dump(2);
            return;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        const auto state = nlohmann::json::parse(file, nullptr, false);
        if (state.is_discarded() || state.value("schemaVersion", 0) != 1 ||
            state.value("pid", 0UL) != GetCurrentProcessId() ||
            state.value("processCreated", std::uint64_t{}) != created ||
            !state.contains("bindings") || !state["bindings"].is_array()) return;
        for (const auto& item : state["bindings"]) {
            LocalBirthBinding binding{{item.at("id").get<std::uint64_t>(),
                item.at("generation").get<std::uint64_t>()},
                item.at("actor").get<std::uintptr_t>(),
                item.at("abilitySystem").get<std::uintptr_t>(),
                item.value("attributeSet", static_cast<std::uintptr_t>(0)),
                item.value("exactHP", 0.0F)};
            binding.combat_config.config_name = item.value("combatConfig", std::string{});
            binding.combat_config.server_blueprint = item.value("combatSource", std::string{});
            g_pending_local_births.push_back(std::move(binding));
        }
        g_restore_local_births.store(!g_pending_local_births.empty(), std::memory_order_release);
    } catch (...) {
        if (g_context.core != nullptr)
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: local birth state transfer failed"));
    }
}

void RestoreLocalBirthsOnGameThread() {
    if (!g_restore_local_births.exchange(false, std::memory_order_acq_rel)) return;
    for (const auto& binding : g_pending_local_births) {
        AnomalyGenerationHandleV1 current{};
        std::uintptr_t asc{}, set{};
        if (IsActorAlive(binding.actor_handle, binding.actor) && HandleFromPointer(binding.actor, &current) &&
            current.id == binding.actor_handle.id &&
            current.generation == binding.actor_handle.generation &&
            ResolveActorAttributeSet(binding.actor, &asc, &set) && asc == binding.ability_system)
        {
            RegisterLocalBirth(current, binding.actor, asc, set, binding.exact_hp,
                PrepareCombatConfig(binding.combat_config.config_name, binding.combat_config.server_blueprint));
        }
    }
    g_pending_local_births.clear();
}

bool IsBirthAttributeSet(std::uintptr_t self, BirthInitialization& birth) {
    if (birth.attribute_set != 0) return self == birth.attribute_set;
    std::uintptr_t owner = self;
    for (int depth = 0; depth < 4; ++depth) {
        if (!ReadValue(owner + g_layout.objectOuter, &owner) || owner == 0)
            return false;
        std::uintptr_t owner_class{};
        if (!ReadValue(owner + g_layout.objectClass, &owner_class)) return false;
        if (owner_class != birth.actor_class) continue;
        std::uint32_t flags{};
        if (!ReadValue(owner + g_layout.objectFlags, &flags) || (flags & 0x30U) != 0) return false;
        birth.actor = owner;
        birth.attribute_set = self;
        return true;
    }
    return false;
}

void OverrideBirthBaseInput(std::uintptr_t container, const void* attribute, float& value) {
    auto* birth = g_birth_initialization;
    if (birth == nullptr || attribute == nullptr) return;

    std::uintptr_t property{};
    int32_t offset{};
    std::uint32_t name_id{};
    char name[kAttributeNameCapacity]{};
    if (!ReadValue(reinterpret_cast<std::uintptr_t>(attribute) +
                       g_layout.attributeProperty, &property) ||
        !IsGameplayAttributeProperty(property, &offset) ||
        !ReadValue(property + g_layout.fieldName, &name_id) ||
        !ResolveName(name_id, name, sizeof(name)))
        return;

    const auto preset = std::find_if(birth->attributes.begin(), birth->attributes.end(),
        [&](const BirthAttribute& input) { return input.name == name; });
    float requested{};
    if (preset != birth->attributes.end()) requested = preset->value;
    else if (birth->exact_hp > 0.0F &&
        (std::strcmp(name, "HPMaxUp") == 0 || std::strcmp(name, "HPMaxAdd") == 0 ||
         std::strcmp(name, "HPMaxUpTemp") == 0 || std::strcmp(name, "HPMaxAddTemp") == 0))
        requested = 0.0F;
    else if (birth->exact_hp > 0.0F && std::strcmp(name, "OnlineMaxHPRatio") == 0)
        requested = 1.0F;
    else return;

    // The container owns the ASC. Use its registered AttributeSets because
    // HTAttribute/OwnerAbilityCharacter can still be unset during construction.
    std::uintptr_t asc{};
    struct AttributeSets {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } sets;
    if (!ReadValue(container + g_layout.containerOwner, &asc) || asc == 0 ||
        !ReadValue(asc + g_layout.spawnedAttributes, &sets) || sets.data == 0 ||
        sets.count <= 0 || sets.count > 64 || sets.capacity < sets.count)
        return;
    for (std::int32_t i = 0; i < sets.count; ++i) {
        std::uintptr_t set{}, set_class{};
        if (!ReadValue(sets.data + static_cast<std::uintptr_t>(i) * sizeof(set), &set) ||
            set == 0 || !ReadValue(set + g_layout.objectClass, &set_class) ||
            FindProperty(set_class, name) != property || !IsBirthAttributeSet(set, *birth))
            continue;
        value = requested;
        if (preset != birth->attributes.end()) preset->applied = true;
        return;
    }
}

void ANOMALY_CALL AttributeBaseInputDetour(void* self, const void* attribute,
                                         float new_base) noexcept {
    const auto original = reinterpret_cast<AttributeBaseInputFn>(g_context.attribute_original);
    AnomalyGenerationHandleV1 lease{};
    bool leased = false;
    try {
        if (g_context.hooks != nullptr && g_context.attribute_hook.id != 0)
            leased = g_context.hooks->begin_callback(g_context.hooks->user,
                         g_context.attribute_hook, &lease).code == ANOMALY_STATUS_V1_OK;
        if (leased && g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId())
            OverrideBirthBaseInput(reinterpret_cast<std::uintptr_t>(self), attribute, new_base);
    } catch (...) {
    }
    // Supply the value before GAS stores the base and updates its aggregator.
    // Never override an evaluated CurrentValue: that leaves the original base
    // inside the aggregator and it resurfaces when damage causes reevaluation.
    try {
        if (original != nullptr) original(self, attribute, new_base);
    } catch (...) {
    }
    if (leased) {
        try { static_cast<void>(g_context.hooks->end_callback(g_context.hooks->user, lease)); }
        catch (...) {}
    }
}

bool ValidateSummonProfile(const anomaly::sdk::Profile& profile, summon_lab::NativeLayout* output) {
    if (!profile.Matches({"nte", "HTGame.exe", ".text", "summon-lab-v2"})) return false;
    try {
        summon_lab::NativeLayout layout;
        if (!summon_lab::LoadNativeLayout(profile, &layout)) return false;
        static_cast<void>(profile.symbol("attribute-base-input"));
        static_cast<void>(profile.symbol("summon-registration"));
        static_cast<void>(profile.symbol("monster-default-level"));
        // This address comes from the validated object's vtable, not a global
        // signature scan: the short accessor body is not globally unique.
        const auto& getters = profile.symbol("mirror-owner-getter").candidates;
        if (getters.size() != 1 || getters.front().call_offset || getters.front().entry.empty() ||
            getters.front().entry.size() > 256) return false;
        for (const auto& candidate : profile.symbol("local-attribute-mirror").candidates) {
            const auto size = candidate.values.at("recordSize");
            const auto camps = candidate.values.at("campCount");
            const auto attributes = candidate.values.at("birthAttributeCount");
            // The palette search enumerates subsets; bound its work and keep
            // every 1U << camp_count operation within the unsigned bit width.
            if (!summon_lab::ValidMirrorRecord(layout, size) || camps < 2 || camps > 16 ||
                attributes == 0 || attributes > kBirthAttributeNames.size()) return false;
        }
        *output = layout;
        return true;
    } catch (const std::out_of_range&) {
        return false;
    }
}

bool InstallBirthAttributeHook(const anomaly::sdk::Host& query, const anomaly::sdk::Profile& profile) {
    g_context.hooks = query.Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION).get();
    if (g_context.hooks == nullptr ||
        g_context.hooks->create == nullptr || g_context.hooks->release == nullptr ||
        g_context.hooks->begin_callback == nullptr || g_context.hooks->end_callback == nullptr)
        return false;
    const auto resolved = g_profile_scans.Resolve(g_context.core, profile, "attribute-base-input");
    if (!resolved) return false;
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = resolved.address;
    request.detour = reinterpret_cast<void*>(&AttributeBaseInputDetour);
    request.label = anomaly::sdk::StringView("summon-birth-base-input");
    return g_context.hooks->create(g_context.hooks->user, &request,
        &g_context.attribute_original, &g_context.attribute_hook).code == ANOMALY_STATUS_V1_OK &&
        g_context.attribute_hook.id != 0 && g_context.attribute_original != 0;
}


// Scoped callbacks must finish before the plugin generation can be unloaded.
class LocalCombatLease {
public:
    explicit LocalCombatLease(std::size_t index) : hooks_(g_context.hooks) {
        if (hooks_ != nullptr && g_context.local_combat_hooks[index].handle.id != 0)
            active_ = hooks_->begin_callback(hooks_->user,
                g_context.local_combat_hooks[index].handle, &lease_).code == ANOMALY_STATUS_V1_OK;
    }
    ~LocalCombatLease() noexcept {
        if (active_) {
            try { static_cast<void>(hooks_->end_callback(hooks_->user, lease_)); }
            catch (...) {}
        }
    }
    explicit operator bool() const noexcept { return active_; }
private:
    const AnomalyHookServiceV1* hooks_{};
    AnomalyGenerationHandleV1 lease_{};
    bool active_{};
};

using SummonRegistrationFn = void (ANOMALY_CALL*)(void*, const void*, void*, std::int32_t);
void ANOMALY_CALL LocalRegistrationDetour(void* controller, const void* leader,
                                        void* class_cdo, std::int32_t index) noexcept {
    const auto original = reinterpret_cast<SummonRegistrationFn>(
        g_context.local_combat_hooks[0].original);
    try {
        LocalCombatLease lease(0);
        if (lease && g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
            auto* birth = g_birth_initialization;
            std::uintptr_t uclass{};
            if (birth != nullptr && birth->actor_class != 0 && original != nullptr &&
                ReadValue(reinterpret_cast<std::uintptr_t>(class_cdo) + g_layout.objectClass, &uclass) &&
                uclass == birth->actor_class && index >= 0 && leader != nullptr) {
                // Preserve the exact native leader identity and summon index.
                // Configuration must use the same registration, never a fabricated actor ID.
                if (ReadBytes(reinterpret_cast<std::uintptr_t>(leader),
                        birth->registration_leader.data(), birth->registration_leader.size())) {
                    birth->registration_controller = reinterpret_cast<std::uintptr_t>(controller);
                    birth->registration_index = index;
                    // Only this birth's configured source may supply a server CDO.
                    // Keep the actor class, leader bytes and summon index intact.
                    if (birth->registration_cdo != 0) {
                        class_cdo = reinterpret_cast<void*>(birth->registration_cdo);
                        birth->registration_substituted = true;
                    }
                }
            }
        }
        if (original != nullptr) original(controller, leader, class_cdo, index);
    } catch (...) {}
}

AnomalyStatusV1 ReleaseLocalCombatHooks() {
    for (std::size_t i = g_context.local_combat_hooks.size(); i != 0; --i) {
        auto& hook = g_context.local_combat_hooks[i - 1];
        if (g_context.hooks != nullptr && hook.handle.id != 0) {
            const auto status = g_context.hooks->release(g_context.hooks->user, hook.handle);
            if (status.code != ANOMALY_STATUS_V1_OK &&
                status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
        }
        hook = {};
    }
    return anomaly::sdk::Ok();
}

// The native mirror deserializes cached fields after GAS has initialized them.
// Local custom births own their MaxHP caches; replicated players do not.
// Filter a borrowed local view before assignment, never rewrite HP or RPC data.
using LocalAttributeMirrorFn = void (ANOMALY_CALL*)(void*, void*, const void*, void*);
void ANOMALY_CALL PreserveLocalBirthMaxHP(void* context, void* owner,
                                       const void* record, void* scratch) noexcept {
    const auto original = reinterpret_cast<LocalAttributeMirrorFn>(g_context.local_combat_hooks[1].original);
    try {
        LocalCombatLease lease(1);
        // 1.5 adds GameplayModuleId to the end of the native container.
        alignas(void*) summon_lab::MirrorBuffer view{};
        std::vector<MirrorEntry> filtered;
        const void* input = record;
        try {
            std::uintptr_t target{};
            std::uintptr_t actor{}, current_ability_system{};
            bool protect_max{}, observe_ft{};
            AnomalyGenerationHandleV1 handle{};
            if (lease && record != nullptr &&
                ReadValue(reinterpret_cast<std::uintptr_t>(record) + g_layout.mirrorObject, &target)) {
                std::scoped_lock lock(g_local_birth_mutex);
                for (const auto& binding : g_local_births) {
                    if (binding.ability_system == target &&
                        (binding.exact_hp > 0.0F || binding.combat_config.config_name.starts_with("FT_"))) {
                        handle = binding.actor_handle;
                        actor = binding.actor;
                        protect_max = binding.exact_hp > 0.0F;
                        observe_ft = binding.combat_config.config_name.starts_with("FT_");
                        break;
                    }
                }
            }
            if (handle.id != 0 && ProbeHandle(handle) &&
                ReadValue(actor + g_layout.characterAbilitySystem, &current_ability_system) &&
                current_ability_system == target &&
                ReadBytes(reinterpret_cast<std::uintptr_t>(record), view.data(), g_context.mirror_record_size)) {
                std::uintptr_t vtable{}, getter{}, values{};
                std::int32_t count{}, capacity{};
                std::memcpy(&vtable, view.data() + g_layout.mirrorVtable, sizeof(vtable));
                std::memcpy(&values, view.data() + g_layout.mirrorValues, sizeof(values));
                std::memcpy(&count, view.data() + g_layout.mirrorValueCount, sizeof(count));
                std::memcpy(&capacity, view.data() + g_layout.mirrorValueCapacity, sizeof(capacity));
                const auto& getter_entry = g_context.mirror_getter_entry;
                std::array<std::uint8_t, 256> actual{};
                if (values != 0 && count > 0 && count <= capacity && count <= 512 &&
                    ReadValue(vtable + g_layout.mirrorGetterSlot, &getter) &&
                    ReadBytes(getter, actual.data(), getter_entry.size()) &&
                    std::equal(getter_entry.begin(), getter_entry.end(), actual.begin())) {
                    std::vector<MirrorEntry> entries(static_cast<std::size_t>(count));
                    if (ReadBytes(values, entries.data(), entries.size() * sizeof(entries[0]))) {
                        for (const auto& entry : entries) {
                            std::uint32_t id{};
                            char name[64]{};
                            std::memcpy(&id, entry.data() + g_layout.mirrorValueName, sizeof(id));
                            const bool named = ResolveName(id, name, sizeof(name));
                            const bool max_field = named &&
                                (std::strcmp(name, "MaxHP") == 0 || std::strcmp(name, "MaxHPTemp") == 0);
                            if (observe_ft && named && (max_field || std::strcmp(name, "HPCurrent") == 0))
                                ObserveFinalTowerMirror(target, name, entry);
                            const bool protected_max = protect_max && max_field;
                            if (protected_max && std::strcmp(name, "MaxHPTemp") == 0)
                                CaptureNativeBirthMaximum(target, entry);
                            if (!protected_max) filtered.push_back(entry);
                        }
                        if (filtered.size() != entries.size()) {
                            const auto pointer = reinterpret_cast<std::uintptr_t>(filtered.data());
                            const auto size = static_cast<std::int32_t>(filtered.size());
                            std::memcpy(view.data() + g_layout.mirrorValues, &pointer, sizeof(pointer));
                            std::memcpy(view.data() + g_layout.mirrorValueCount, &size, sizeof(size));
                            std::memcpy(view.data() + g_layout.mirrorValueCapacity, &size, sizeof(size));
                            input = view.data();
                        }
                    }
                }
            }
        } catch (...) { input = record; }
        if (original != nullptr) original(context, owner, input, scratch);
    } catch (...) {}
}

using DamageReportFn = std::uintptr_t (ANOMALY_CALL*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, float);

std::uintptr_t ANOMALY_CALL ObserveNativeDamageReport(std::uintptr_t controller,
    std::uintptr_t source, std::uintptr_t target, float damage) noexcept {
    const auto original = reinterpret_cast<DamageReportFn>(g_context.local_combat_hooks[4].original);
    std::uintptr_t report{};
    try {
        LocalCombatLease lease(4);
        if (original != nullptr) report = original(controller, source, target, damage);
        if (!lease || !g_observe_ft_health.load(std::memory_order_acquire)) return report;
        LocalBirthBinding attacker, victim;
        {
            std::scoped_lock lock(g_local_birth_mutex);
            const auto a = std::find_if(g_local_births.begin(), g_local_births.end(),
                [source](const auto& value) { return value.ability_system == source; });
            const auto b = std::find_if(g_local_births.begin(), g_local_births.end(),
                [target](const auto& value) { return value.ability_system == target; });
            if (a == g_local_births.end() || b == g_local_births.end() || a == b ||
                a->damage_observations >= 24 ||
                (!a->combat_config.config_name.starts_with("FT_") &&
                 !b->combat_config.config_name.starts_with("FT_"))) return report;
            ++a->damage_observations;
            attacker = *a;
            victim = *b;
        }
        if (!ProbeHandle(attacker.actor_handle) || !ProbeHandle(victim.actor_handle)) return report;
        const auto& layout = g_context.damage_report;
        NativeIdentity source_identity{}, target_identity{};
        std::int32_t effect{-1};
        float reported_damage{}, reported_hp{}, local_hp{};
        const bool readable = report != 0 &&
            ReadValue(report + layout.source, &source_identity) &&
            ReadValue(report + layout.target, &target_identity) &&
            ReadValue(report + layout.effect, &effect) &&
            ReadValue(report + layout.damage, &reported_damage) &&
            ReadValue(report + layout.hp, &reported_hp);
        const auto matches = [](NativeIdentity identity, const SummonIdentity& registration) {
            std::int32_t index{};
            std::memcpy(&index, identity.data() + g_layout.diagnosticSummonIndex, sizeof(index));
            return index == registration.index && std::memcmp(identity.data(), registration.leader.data(),
                g_layout.diagnosticSummonIndex) == 0;
        };
        static_cast<void>(ReadReflectedValue(target, "HPCurrent", "FloatProperty", &local_hp));
        char message[384]{};
        std::snprintf(message, sizeof(message),
            "SummonLab FT-damage-report: source=%p target=%p input=%.9g built=%s effect=%d damage=%.9g reportHP=%.9g localHP=%.9g sourceMatch=%d targetMatch=%d",
            reinterpret_cast<void*>(attacker.actor), reinterpret_cast<void*>(victim.actor), damage,
            readable ? "yes" : "no", effect, reported_damage, reported_hp, local_hp,
            readable && matches(source_identity, attacker.registration),
            readable && matches(target_identity, victim.registration));
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            anomaly::sdk::StringView(message));
    } catch (...) {}
    return report;
}

bool InstallDamageReportObserver(const anomaly::sdk::Profile& profile) {
    try {
        const auto resolved = g_profile_scans.Resolve(g_context.core, profile, "native-damage-report");
        if (!resolved) return false;
        const auto& values = resolved.candidate->values;
        Context::DamageReportLayout layout{values.at("recordSize"), values.at("sourceIdentity"),
            values.at("targetIdentity"), values.at("effectIndex"), values.at("damage"), values.at("currentHp")};
        if (layout.size > 1024 || layout.size % alignof(std::uintptr_t) != 0 ||
            layout.source % alignof(std::uint32_t) != 0 || layout.target % alignof(std::uint32_t) != 0 ||
            layout.effect % alignof(std::int32_t) != 0 || layout.damage % alignof(float) != 0 ||
            layout.hp % alignof(float) != 0 ||
            !summon_lab::ValidBuffer(layout.size, {{layout.source, sizeof(NativeIdentity)},
                {layout.target, sizeof(NativeIdentity)}, {layout.effect, sizeof(std::int32_t)},
                {layout.damage, sizeof(float)}, {layout.hp, sizeof(float)}})) return false;
        g_context.damage_report = layout;
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = resolved.address;
        request.detour = reinterpret_cast<void*>(&ObserveNativeDamageReport);
        request.label = anomaly::sdk::StringView("summon-native-damage-report");
        auto& hook = g_context.local_combat_hooks[4];
        return g_context.hooks->create(g_context.hooks->user, &request, &hook.original,
            &hook.handle).code == ANOMALY_STATUS_V1_OK && hook.handle.id != 0 && hook.original != 0;
    } catch (const std::out_of_range&) { return false; }
}

bool InstallLocalCombatHooks(const anomaly::sdk::Profile& profile) {
    if (g_context.hooks == nullptr ||
        g_context.hooks->create == nullptr || g_context.hooks->release == nullptr ||
        g_context.hooks->begin_callback == nullptr || g_context.hooks->end_callback == nullptr)
        return false;
    constexpr std::array<std::string_view, 2> names{
        "summon-registration", "local-attribute-mirror"};
    const auto registration = g_profile_scans.Resolve(g_context.core, profile, names[0]);
    const auto mirror = g_profile_scans.Resolve(g_context.core, profile, names[1]);
    if (!registration || !mirror) return false;
    const std::array<std::uintptr_t, 2> targets{registration.address, mirror.address};
    g_context.mirror_record_size = mirror.candidate->values.at("recordSize");
    g_context.camp_count = mirror.candidate->values.at("campCount");
    g_context.available_birth_attributes = mirror.candidate->values.at("birthAttributeCount");
    g_context.mirror_getter_entry = profile.symbol("mirror-owner-getter").candidates.front().entry;

    // Resolve and validate everything before publishing either callback.
    const std::array<void*, 2> detours{
        reinterpret_cast<void*>(&LocalRegistrationDetour),
        reinterpret_cast<void*>(&PreserveLocalBirthMaxHP)};
    for (std::size_t i = 0; i < names.size(); ++i) {
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = targets[i];
        request.detour = detours[i];
        request.label = anomaly::sdk::StringView(names[i]);
        auto& hook = g_context.local_combat_hooks[i];
        if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original,
                &hook.handle).code != ANOMALY_STATUS_V1_OK || hook.handle.id == 0 ||
                hook.original == 0) {
            static_cast<void>(ReleaseLocalCombatHooks());
            return false;
        }
    }
    return true;
}

class BirthScope {
public:
    explicit BirthScope(BirthInitialization& birth) {
        g_birth_initialization = &birth;
        g_birth_thread.store(GetCurrentThreadId(), std::memory_order_release);
    }
    ~BirthScope() {
        g_birth_thread.store(0, std::memory_order_release);
        g_birth_initialization = nullptr;
    }
};

#include "blueprint_editor_runtime.hpp"

// These native leaves are called directly by Blueprint VM instructions, which
// do not pass through UObject::ProcessEvent. Filter the source instance here;
// shared scripts, CDOs, FText arrays and global audio state remain engine-owned.
enum class StoryAudioKind { Music, Dialogue };

bool SuppressStoryAudio(std::uintptr_t source, StoryAudioKind kind) {
    if (source == 0) return false;
    if (g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
        auto* birth = g_birth_initialization;
        const bool mute_birth = birth != nullptr && (kind == StoryAudioKind::Music
            ? birth->mute_boss_music : birth->mute_boss_dialogue);
        if (mute_birth && source == birth->story_audio_actor)
            return true;
        if (mute_birth && birth->story_audio_actor == 0) {
            std::uintptr_t actor_class{};
            std::uint32_t flags{};
            if (ReadValue(source + g_layout.objectClass, &actor_class) &&
                actor_class == birth->actor_class &&
                ReadValue(source + g_layout.objectFlags, &flags) && (flags & 0x30U) == 0) {
                birth->story_audio_actor = source;
                return true;
            }
        }
    }
    AnomalyGenerationHandleV1 expected{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            const auto& record = g_context.records[i];
            const bool mute = kind == StoryAudioKind::Music
                ? record.mute_boss_music : record.mute_boss_dialogue;
            if (record.pointer == source && mute) {
                expected = record.handle;
                break;
            }
        }
    }
    // A destroyed actor's memory/index can be reused before the next refresh.
    AnomalyGenerationHandleV1 actual{};
    return expected.id != 0 && HandleFromPointer(source, &actual) &&
        actual.id == expected.id && actual.generation == expected.generation;
}

class StoryAudioLease {
public:
    explicit StoryAudioLease(std::size_t index) : hooks_(g_context.hooks) {
        if (hooks_ != nullptr && g_context.story_audio_hooks[index].handle.id != 0)
            active_ = hooks_->begin_callback(hooks_->user,
                g_context.story_audio_hooks[index].handle, &lease_).code == ANOMALY_STATUS_V1_OK;
    }
    ~StoryAudioLease() noexcept {
        if (active_) {
            try { static_cast<void>(hooks_->end_callback(hooks_->user, lease_)); }
            catch (...) {}
        }
    }
    explicit operator bool() const noexcept { return active_; }
private:
    const AnomalyHookServiceV1* hooks_{};
    AnomalyGenerationHandleV1 lease_{};
    bool active_{};
};

// Fixed native call ABIs: SpawnBossMusicVolume(), ShowMulVoiceMsg(context,
// type, const TArray<FText>&), ShowDebugMulVoiceMsg(context, const TArray<...>&),
// ShowVoiceMsg(context, type, const FText&). No ownership crosses these calls.
template <std::size_t Index, typename... Args>
void ANOMALY_CALL StoryAudioDetour(std::uintptr_t source, Args... args) noexcept {
    using Function = void (ANOMALY_CALL*)(std::uintptr_t, Args...);
    const auto original = reinterpret_cast<Function>(g_context.story_audio_hooks[Index].original);
    try {
        StoryAudioLease lease(Index);
        bool suppress{};
        try { suppress = lease && SuppressStoryAudio(source,
            Index == 0 ? StoryAudioKind::Music : StoryAudioKind::Dialogue); }
        catch (...) {}
        if (!suppress && original != nullptr) original(source, args...);
    } catch (...) {}
}

AnomalyStatusV1 ReleaseStoryAudioHooks() {
    g_context.story_audio_ready.store(false, std::memory_order_release);
    for (std::size_t i = g_context.story_audio_hooks.size(); i != 0; --i) {
        auto& hook = g_context.story_audio_hooks[i - 1];
        if (g_context.hooks != nullptr && hook.handle.id != 0) {
            const auto status = g_context.hooks->release(g_context.hooks->user, hook.handle);
            if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND)
                return status;
        }
        hook = {};
    }
    return anomaly::sdk::Ok();
}

bool InstallStoryAudioHooks(const anomaly::sdk::Profile& profile) {
    if (g_context.hooks == nullptr) return false;
    constexpr std::array<const char*, 4> names{
        "story-boss-music", "story-multiple-voices", "story-debug-voices", "story-single-voice"};
    const std::array<void*, 4> detours{
        reinterpret_cast<void*>(&StoryAudioDetour<0>),
        reinterpret_cast<void*>(&StoryAudioDetour<1, std::uint8_t, const void*>),
        reinterpret_cast<void*>(&StoryAudioDetour<2, const void*>),
        reinterpret_cast<void*>(&StoryAudioDetour<3, std::uint8_t, const void*>)};
    std::array<std::uintptr_t, names.size()> targets{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto resolved = g_profile_scans.Resolve(g_context.core, profile, names[i]);
        if (!resolved) return false;
        targets[i] = resolved.address;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = targets[i];
        request.detour = detours[i];
        request.label = anomaly::sdk::StringView(names[i]);
        auto& hook = g_context.story_audio_hooks[i];
        if (g_context.hooks->create(g_context.hooks->user, &request,
                &hook.original, &hook.handle).code != ANOMALY_STATUS_V1_OK ||
            hook.handle.id == 0 || hook.original == 0) {
            static_cast<void>(ReleaseStoryAudioHooks());
            return false;
        }
    }
    g_context.story_audio_ready.store(true, std::memory_order_release);
    return true;
}


bool DismissSummoned(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer,
                     float previous_max_hp = 0.0F);
void RefreshHealth(bool refresh_attributes = true);

// ---------------------------------------------------------------- 召唤

struct SummonResult {
    bool ok{};
    bool pet{};
    float hp{}, max_hp{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    std::uintptr_t ability_system{};
    std::uintptr_t attribute_set{};
    SummonIdentity registration;
    std::string server_combat_source;
    std::vector<AttributeValue> attributes;
    std::shared_ptr<summon_lab::EditorRuntime> editor;
    std::string editor_warning;
    char label[96]{};
    char detail[kTextCapacity]{};
};

bool UnregisterNativeSummon(const SummonIdentity& identity) {
    if (identity.controller.id == 0 || identity.index < 0) return true;
    // The native opponent-summoner explicitly sends this before destroying
    // its actor. DestroyActor alone does not release the hybrid combat entry.
    // Use the captured identity even after the actor has died or disappeared.
    struct {
        NativeIdentity leader;
        std::int32_t index;
    } parameters{identity.leader, identity.index};
    static_assert(sizeof(parameters) == 44);
    const bool released = InvokeByPath(identity.controller,
        "/Script/HTGame.HTPlayerController:ServerDismissSummonedAI_HybridMode",
        &parameters, sizeof(parameters));
    if (g_context.core != nullptr) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "SummonLab native-dismiss: index=%d submitted=%s",
            identity.index, released ? "yes" : "no");
        g_context.core->log(g_context.core->user, released ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
            ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
    }
    return released;
}

struct SummonFailureCleanup {
    const SummonResult& result;
    ~SummonFailureCleanup() noexcept {
        if (result.ok) return;
        try {
            ForgetLocalBirth(result.handle);
            static_cast<void>(UnregisterNativeSummon(result.registration));
        } catch (...) {}
    }
};

std::string MonsterConfigName(std::string_view path) {
    // SummonAI derives its config ID from the complete class name without _C.
    // Keep quest/difficulty suffixes: variants do not share the same config row.
    const auto start = path.find('/');
    if (start == std::string_view::npos) return {};
    path.remove_prefix(start);
    path = path.substr(0, path.find_first_of(":' \t\r\n"));
    const auto separator = path.find_last_of("/.");
    auto name = path.substr(separator + 1);
    // NPCs in the HTAICharacter catalog use the same generated-class identity.
    // Their lack of a monster row is a preflight hint, never a naming filter.
    if (name.size() >= 2 && _strnicmp(name.data() + name.size() - 2, "_C", 2) == 0)
        name.remove_suffix(2);
    return std::string(name);
}

bool MakeConfigName(std::string_view config_name, std::uint64_t* result) {
    AnomalyGenerationHandleV1 string_library{};
    if (!FindHandle("/Script/Engine.Default__KismetStringLibrary", &string_library)) return false;
    // POD parameters from the supplied SDK. Native FName conversion preserves
    // numeric suffixes; searching the name pool alone loses FName::Number.
    std::array<wchar_t, kPathCapacity> wide_name{};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        config_name.data(), static_cast<int>(config_name.size()), wide_name.data(),
        static_cast<int>(wide_name.size() - 1));
    if (count == 0) return false;
    struct NameParameters {
        const wchar_t* data;
        std::int32_t count;
        std::int32_t capacity;
        std::uint64_t result{};
    } name{wide_name.data(), count + 1, count + 1};
    static_assert(sizeof(NameParameters) == 24);
    if (!InvokeByPath(string_library, "/Script/Engine.KismetStringLibrary:Conv_StringToName",
                      &name, sizeof(name))) return false;
    *result = name.result;
    return true;
}

std::uintptr_t ResolveMonsterLevelQuery(const anomaly::sdk::Profile& profile) {
    // Read-only query taking HTPlayerState*, UWorld* and an eight-byte FName.
    return g_profile_scans.Resolve(g_context.core, profile, "monster-default-level").address;
}

std::uint32_t ResolveDuelChaseRange(const anomaly::sdk::Profile& profile) {
    const auto resolved = g_profile_scans.Resolve(g_context.core, profile, "duel-current-chase-range");
    if (!resolved) return 0;
    try {
        const auto offset = resolved.candidate->values.at("currentChaseRange");
        return offset > 0 && offset <= 16384 && offset % alignof(float) == 0 ? offset : 0;
    } catch (const std::out_of_range&) { return 0; }
}

// FinalTower separates scene monster rows from its own property-pack provider.
// Borrow those native rows only for our scoped queries and registered summons.
// Keep their owning assets rooted until all native callbacks have drained.
struct FinalTowerRuntime {
    struct Root {
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t object{};
        std::uintptr_t item{};
        bool owned{};
    };
    std::uintptr_t objects{};
    std::uintptr_t set_flags{};
    std::uintptr_t pack_query{};
    std::uintptr_t scene{};
    std::uintptr_t data{};
    std::uintptr_t monster_struct{};
    std::uintptr_t pack_struct{};
    std::uint32_t count_offset{}, chunk_size{}, item_size{}, object_offset{}, flags_offset{}, root_flag{};
    std::vector<Root> roots;
    bool ready{};
} g_final_tower;

std::atomic<DWORD> g_config_thread{0};
const CombatConfigSource* g_scoped_config{};

class CombatConfigScope {
public:
    explicit CombatConfigScope(const CombatConfigSource& source) {
        g_scoped_config = &source;
        g_config_thread.store(GetCurrentThreadId(), std::memory_order_release);
    }
    ~CombatConfigScope() {
        g_config_thread.store(0, std::memory_order_release);
        g_scoped_config = nullptr;
    }
};

struct LocalCombatConfigLookup {
    std::uintptr_t value{};
    std::uint64_t row_alias{};
};

LocalCombatConfigLookup FindLocalCombatConfig(std::uint64_t name, bool pack) {
    const auto matches = [&](const CombatConfigSource& config) {
        return name != 0 && name == (pack ? config.pack : config.config);
    };
    const auto select = [pack](const CombatConfigSource& config) {
        return LocalCombatConfigLookup{pack ? config.property_pack : config.row,
            pack ? 0 : config.row_alias};
    };
    if (g_config_thread.load(std::memory_order_acquire) == GetCurrentThreadId() &&
        g_scoped_config != nullptr && matches(*g_scoped_config))
        return select(*g_scoped_config);
    std::scoped_lock lock(g_local_birth_mutex);
    for (const auto& binding : g_local_births) {
        // Aliased births retain their donor ID on the individual actor. Only
        // FT needs a persistent scene fallback; different donors must not leak
        // into other actors of the same base class.
        const auto& config = binding.combat_config;
        if ((pack || config.row_alias == 0) && matches(config)) return select(config);
        if (!pack && config.row_alias != 0 && name == config.row_alias && config.row != 0)
            return {config.row, 0};
    }
    return {};
}

using MonsterRowFn = std::uintptr_t (ANOMALY_CALL*)(std::uintptr_t, std::uint64_t);
using PropertyPackFn = std::uintptr_t (ANOMALY_CALL*)(std::uintptr_t, std::uint64_t, int, std::uintptr_t);

std::uintptr_t ANOMALY_CALL FinalTowerMonsterRow(std::uintptr_t scene, std::uint64_t name) noexcept {
    try {
        LocalCombatLease lease(2);
        const auto original = reinterpret_cast<MonsterRowFn>(g_context.local_combat_hooks[2].original);
        if (original == nullptr) return 0;
        const auto configured = lease ? FindLocalCombatConfig(name, false) : LocalCombatConfigLookup{};
        // Resolve the donor in this scene, through the original query. Do not
        // cache a scene-owned row or recursively alias unrelated monsters.
        if (configured.row_alias != 0) {
            const auto row = original(scene, configured.row_alias);
            return row != 0 ? row : configured.value;
        }
        const auto row = original(scene, name);
        return row != 0 ? row : configured.value;
    } catch (...) { return 0; }
}

std::uintptr_t ANOMALY_CALL FinalTowerPropertyPack(std::uintptr_t data, std::uint64_t name,
                                                int kind, std::uintptr_t context) noexcept {
    try {
        LocalCombatLease lease(3);
        const auto original = reinterpret_cast<PropertyPackFn>(g_context.local_combat_hooks[3].original);
        const auto pack = original != nullptr ? original(data, name, kind, context) : 0;
        return pack != 0 || kind != 1 || !lease ? pack : FindLocalCombatConfig(name, true).value;
    } catch (...) { return 0; }
}

void ReleaseFinalTowerData() {
    for (const auto& root : g_final_tower.roots) {
        std::uintptr_t object{};
        if (root.owned && ProbeHandle(root.handle) &&
            ReadValue(root.item + g_final_tower.object_offset, &object) &&
            object == root.object) {
            // The native setter registers the GC barrier; removing our root is
            // an atomic flag clear after the hooks have stopped borrowing rows.
            InterlockedAnd(reinterpret_cast<volatile LONG*>(root.item + g_final_tower.flags_offset),
                           ~static_cast<LONG>(g_final_tower.root_flag));
        }
    }
    g_final_tower = {};
}

bool PinFinalTowerAsset(std::uintptr_t object) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t chunks{}, chunk{}, actual{};
    std::int32_t count{};
    if (!HandleFromPointer(object, &handle) ||
        !ReadValue(g_final_tower.objects, &chunks) || chunks == 0 ||
        !ReadValue(g_final_tower.objects + g_final_tower.count_offset, &count)) return false;
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    if (count <= 0 || index >= static_cast<std::uint32_t>(count) ||
        !ReadValue(chunks + sizeof(std::uintptr_t) * (index / g_final_tower.chunk_size), &chunk) ||
        chunk == 0) return false;
    const auto item = chunk + g_final_tower.item_size * static_cast<std::uintptr_t>(
        index % g_final_tower.chunk_size);
    std::uint32_t flags{};
    if (!ReadValue(item + g_final_tower.object_offset, &actual) || actual != object ||
        !ReadValue(item + g_final_tower.flags_offset, &flags)) return false;
    const bool owned = (flags & g_final_tower.root_flag) == 0;
    // Allocate the ledger entry before changing the engine's root flag.
    g_final_tower.roots.push_back({handle, object, item, owned});
    if (owned) {
        using SetFlags = std::uint8_t (ANOMALY_CALL*)(std::uintptr_t, std::int32_t);
        static_cast<void>(reinterpret_cast<SetFlags>(g_final_tower.set_flags)(
            item, static_cast<std::int32_t>(g_final_tower.root_flag)));
    }
    return ReadValue(item + g_final_tower.flags_offset, &flags) &&
           (flags & g_final_tower.root_flag) != 0;
}

bool LoadFinalTowerData() {
    if (!g_final_tower.ready || g_context.final_tower_scene_asset.empty()) return false;
    if (g_final_tower.scene != 0) return true;
    // A failed partial load is kept for orderly release, never repeatedly rooted.
    if (!g_final_tower.roots.empty()) return false;
    AnomalyGenerationHandleV1 util{}, data_library{};
    if (!FindHandle("/Script/HTGame.Default__HTUtil", &util) ||
        !FindHandle("/Script/HTGame.Default__HTGameData", &data_library)) return false;
    std::array<wchar_t, kPathCapacity> path{};
    const auto& asset = g_context.final_tower_scene_asset;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, asset.data(),
        static_cast<int>(asset.size()), path.data(), static_cast<int>(path.size() - 1));
    if (length == 0) return false;
    struct LoadParameters {
        const wchar_t* data;
        std::int32_t count, capacity;
        std::uintptr_t result{};
    } load{path.data(), length + 1, length + 1};
    static_assert(sizeof(LoadParameters) == 24);
    std::uintptr_t ft_data{};
    if (!InvokeByPath(util, "/Script/HTGame.HTUtil:LoadObjectByPath", &load, sizeof(load)) ||
        load.result == 0 || !PinFinalTowerAsset(load.result) ||
        !InvokeByPath(data_library, "/Script/HTGame.HTGameData:GetFinalTowerDataAsset",
                      &ft_data, sizeof(ft_data)) || ft_data == 0 || !PinFinalTowerAsset(ft_data)) return false;
    struct TableArray { std::uintptr_t data; std::int32_t count, capacity; } tables{};
    std::uintptr_t monster_table{}, pack_table{}, monster_struct{}, pack_struct{};
    if (!ReadReflectedValue(load.result, "MonsterArrayDataTable", "ArrayProperty", &tables) ||
        tables.count != 1 || tables.capacity < tables.count || tables.data == 0 ||
        !ReadValue(tables.data, &monster_table) ||
        !ReadReflectedValue(monster_table, "RowStruct", "ObjectProperty", &monster_struct) ||
        !ReadReflectedValue(ft_data, "MonsterFTPackDataTable", "ObjectProperty", &pack_table) ||
        !ReadReflectedValue(pack_table, "RowStruct", "ObjectProperty", &pack_struct)) return false;
    std::uint32_t monster_name{}, pack_name{};
    char monster_type[64]{}, pack_type[64]{};
    if (!ReadValue(monster_struct + g_layout.objectName, &monster_name) ||
        !ReadValue(pack_struct + g_layout.objectName, &pack_name) ||
        !ResolveName(monster_name, monster_type, sizeof(monster_type)) ||
        !ResolveName(pack_name, pack_type, sizeof(pack_type)) ||
        std::strcmp(monster_type, "MonsterStaticData") != 0 ||
        std::strcmp(pack_type, "PackIDStaticData") != 0) return false;
    g_final_tower.scene = load.result;
    g_final_tower.data = ft_data;
    g_final_tower.monster_struct = monster_struct;
    g_final_tower.pack_struct = pack_struct;
    return true;
}

CombatConfigSource PrepareFinalTowerConfig(std::string_view config_name) {
    CombatConfigSource source;
    if (!config_name.starts_with("FT_") || !LoadFinalTowerData()) return source;
    source.config_name = config_name;
    const auto alias = g_context.final_tower_aliases.find(config_name);
    source.row_name = alias == g_context.final_tower_aliases.end() ? source.config_name : alias->second;
    std::uint64_t row_name{};
    if (!MakeConfigName(source.config_name, &source.config) ||
        !MakeConfigName(source.row_name, &row_name)) return {};
    const auto query = reinterpret_cast<MonsterRowFn>(g_context.local_combat_hooks[2].original);
    source.row = query(g_final_tower.scene, row_name);
    if (source.row == 0 ||
        !ReadRowValue(source.row, g_final_tower.monster_struct, "PropModifyID", "NameProperty", &source.pack) ||
        !ReadRowValue(source.row, g_final_tower.monster_struct, "MonsterLevel", "IntProperty", &source.level) ||
        source.pack == 0 || source.level <= 0) return {};
    source.property_pack = reinterpret_cast<MonsterRowFn>(g_final_tower.pack_query)(g_final_tower.data, source.pack);
    float hp{};
    if (source.property_pack == 0 ||
        !ReadRowValue(source.property_pack, g_final_tower.pack_struct, "HPMaxBase", "FloatProperty", &hp) ||
        !std::isfinite(hp) || hp <= 0) return {};
    return source;
}

CombatConfigSource PrepareCombatConfig(std::string_view config_name, std::string_view source_blueprint) {
    if (source_blueprint.empty()) return PrepareFinalTowerConfig(config_name);
    if (!g_final_tower.ready) return {};
    const auto donor = MonsterConfigName(source_blueprint);
    if (donor.empty()) return {};
    auto source = PrepareFinalTowerConfig(donor);
    source.config_name = config_name;
    source.server_blueprint = source_blueprint;
    if (source.row_name.empty()) source.row_name = donor;
    if (!MakeConfigName(source.config_name, &source.config) ||
        !MakeConfigName(donor, &source.row_alias)) return {};
    return source;
}

bool InstallFinalTowerHooks(const anomaly::sdk::Profile& profile) {
    if (g_context.hooks == nullptr) return false;
    try {
        const auto& layout = profile.layout;
        const auto count_offset = layout.at("objectArrayCount");
        const auto chunk_size = layout.at("objectsPerChunk");
        const auto item_size = layout.at("objectItemSize");
        const auto object_offset = layout.at("objectItemObject");
        const auto flags_offset = layout.at("objectItemFlags");
        // The native setter and atomic clear operate on the same aligned flag
        // word; the object identity must occupy a separate field in that item.
        if (count_offset < sizeof(std::uintptr_t) || count_offset > 128 ||
            count_offset % alignof(std::int32_t) != 0 || chunk_size == 0 ||
            chunk_size > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            item_size > 256 || item_size % alignof(std::uintptr_t) != 0 ||
            object_offset % alignof(std::uintptr_t) != 0 || flags_offset % alignof(LONG) != 0 ||
            !summon_lab::ValidBuffer(item_size, {{object_offset, sizeof(std::uintptr_t)},
                {flags_offset, sizeof(LONG)}}) || layout.at("objectRootFlag") != 0x40000000) return false;
        const std::array<std::string_view, 5> names{"monster-scene-row", "monster-property-pack",
            "final-tower-property-pack", "object-item-set-flags", "object-array-reference"};
        std::array<std::uintptr_t, 5> addresses{};
        std::uint32_t displacement_offset{}, instruction_size{};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto resolved = g_profile_scans.Resolve(g_context.core, profile, names[i]);
            if (!resolved) return false;
            addresses[i] = resolved.address;
            if (i == 4) {
                displacement_offset = resolved.candidate->values.at("displacementOffset");
                instruction_size = resolved.candidate->values.at("instructionSize");
                if (displacement_offset != 3 || instruction_size != 7) return false;
            }
        }
        // Validated MOV RAX,[RIP+disp32] entry, not a callable function entry.
        std::int32_t displacement{};
        if (!ReadValue(addresses[4] + displacement_offset, &displacement)) return false;
        g_final_tower.objects = addresses[4] + instruction_size + displacement;
        g_final_tower.set_flags = addresses[3];
        g_final_tower.pack_query = addresses[2];
        g_final_tower.count_offset = count_offset;
        g_final_tower.chunk_size = chunk_size;
        g_final_tower.item_size = item_size;
        g_final_tower.object_offset = object_offset;
        g_final_tower.flags_offset = flags_offset;
        g_final_tower.root_flag = layout.at("objectRootFlag");
        const std::array<void*, 2> callbacks{reinterpret_cast<void*>(&FinalTowerMonsterRow),
                                           reinterpret_cast<void*>(&FinalTowerPropertyPack)};
        for (std::size_t i = 0; i < callbacks.size(); ++i) {
            AnomalyHookRequestV1 request{sizeof(request)};
            request.kind = ANOMALY_HOOK_V1_FUNCTION;
            request.target = addresses[i];
            request.detour = callbacks[i];
            request.label = anomaly::sdk::StringView(names[i]);
            auto& hook = g_context.local_combat_hooks[i + 2];
            if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original, &hook.handle).code !=
                ANOMALY_STATUS_V1_OK || hook.handle.id == 0 || hook.original == 0) return false;
        }
        g_final_tower.ready = true;
        return true;
    } catch (const std::out_of_range&) { return false; }
}

bool ResolveSummonLevel(AnomalyGenerationHandleV1 pawn, std::string_view path,
                        std::uint32_t requested, std::int32_t* level) {
    *level = static_cast<std::int32_t>(std::min(requested, kMaxLevel));
    if (*level > 0) return true;
    const auto config_name = MonsterConfigName(path);
    if (config_name.empty()) return true;
    if (g_context.monster_level_query == 0) return false;

    std::uint64_t config{};
    std::uintptr_t player_state{};
    AnomalyGenerationHandleV1 ht_util{};
    if (!MakeConfigName(config_name, &config) ||
        !FindHandle("/Script/HTGame.Default__HTUtil", &ht_util) ||
        !InvokeByPath(pawn, "/Script/HTGame.HTCharacter:GetHTPlayerState",
                      &player_state, sizeof(player_state)) || player_state == 0) return false;
    // PlayerState is an Actor in the same world as the summoning player.
    // Use the reflected getter instead of reading World/PlayerState offsets.
    std::array<std::uintptr_t, 2> world{player_state, 0};
    if (!InvokeByPath(ht_util, "/Script/HTGame.HTUtil:K2_GetWorld",
                      world.data(), sizeof(world)) || world[1] == 0) return false;
    using Query = std::int32_t (ANOMALY_CALL*)(std::uintptr_t, std::uintptr_t, std::uint64_t);
    *level = reinterpret_cast<Query>(g_context.monster_level_query)(player_state, world[1], config);
    if (*level <= 0) return false;
    if (g_context.core != nullptr && g_context.core->log != nullptr) {
        char detail[kPathCapacity + 64]{};
        std::snprintf(detail, sizeof(detail), "SummonLab default-level: %s level=%d",
                      config_name.c_str(), *level);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                            anomaly::sdk::StringView(detail));
    }
    return true;
}

BlueprintConfigState CheckBlueprintConfig(std::string_view path, CombatConfigSource* source = nullptr,
                                         AnomalyGenerationHandleV1 pawn = {}, float* default_hp = nullptr,
                                         std::string_view source_blueprint = {}) {
    if (default_hp != nullptr) *default_hp = 0.0F;
    const auto config_name = MonsterConfigName(path);
    if (config_name.empty()) return BlueprintConfigState::NotApplicable;

    const auto prepared = PrepareCombatConfig(config_name, source_blueprint);
    if (source != nullptr) *source = prepared;
    if (!source_blueprint.empty() && prepared.row_alias == 0)
        return BlueprintConfigState::Unavailable;
    CombatConfigScope config_scope(prepared);
    AnomalyGenerationHandleV1 ht_util{};
    std::uint64_t config{};
    if ((pawn.id == 0 && !FindLocalPawn(&pawn)) || !MakeConfigName(config_name, &config) ||
        !FindHandle("/Script/HTGame.Default__HTUtil", &ht_util))
        return BlueprintConfigState::Unavailable;

    struct HealthParameters {
        std::uintptr_t world_context{};
        std::uint64_t config_name{};
        float result{};
    } health;
    static_assert(sizeof(HealthParameters) == 24);
    if (!InvokeByPath(pawn, "/Script/Engine.Actor:GetLevel",
                      &health.world_context, sizeof(health.world_context)) ||
        health.world_context == 0) return BlueprintConfigState::Unavailable;
    health.config_name = config;
    // Verified against the running 1.4b: this queries the current scene's
    // monster row and property pack, including world-level overrides. It does
    // not load the selected class or return caller-owned UE allocations.
    if (!InvokeByPath(ht_util, "/Script/HTGame.HTUtil:GetMonsterStaticHPMaxByBPName",
                      &health, sizeof(health)) || !std::isfinite(health.result))
        return BlueprintConfigState::Unavailable;
    if (default_hp != nullptr) *default_hp = health.result;
    if (g_context.core != nullptr && g_context.core->log != nullptr) {
        char detail[kPathCapacity + 64]{};
        std::snprintf(detail, sizeof(detail), "SummonLab preflight: %s defaultHP=%.9g",
                      config_name.c_str(), static_cast<double>(health.result));
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                            anomaly::sdk::StringView(detail));
    }
    // Zero also covers intentionally noncombat variants and unavailable scene
    // data. It is a hint, not proof that loading or spawning a class is safe.
    return health.result > 0 ? BlueprintConfigState::Available : BlueprintConfigState::NoHealth;
}

void UpdateBlueprintConfig(double delta_seconds) {
    BlueprintConfigCheck check;
    {
        std::scoped_lock lock(g_context.mutex);
        auto& current = g_context.blueprint_config;
        if (current.path != g_context.class_path.data() ||
            current.requested_source != g_context.combat_config_blueprint) {
            current = {g_context.class_path.data(), BlueprintConfigState::Pending};
            current.requested_source = g_context.combat_config_blueprint;
            // Do not create FNames for every intermediate keystroke.
            g_context.blueprint_check_delay = 0.25;
            return;
        }
        if (current.stage == BlueprintConfigStage::Complete || current.path.empty()) return;
        g_context.blueprint_check_delay -= delta_seconds;
        if (g_context.blueprint_check_delay > 0) return;
        check = current;
    }
    const auto requested = check.requested_source;
    // At most one native health query per update. Candidates come from the
    // cached Asset Registry catalog; checking them does not load blueprints.
    switch (check.stage) {
    case BlueprintConfigStage::Native: {
        CombatConfigSource source;
        check.state = CheckBlueprintConfig(check.path, &source);
        check.source = source.row_name;
        check.stage = check.requested_source.empty()
            ? BlueprintConfigStage::Complete : BlueprintConfigStage::Borrowed;
        if (check.state == BlueprintConfigState::NoHealth) {
            std::scoped_lock lock(g_context.mutex);
            for (const auto& entry : g_context.catalog)
                if (summon_lab::IsCombatConfigCandidate(check.path, entry.path))
                    check.candidates.push_back(entry.path);
            check.stage = BlueprintConfigStage::Candidates;
        }
        break;
    }
    case BlueprintConfigStage::Candidates:
        if (check.next_candidate < check.candidates.size()) {
            const auto& candidate = check.candidates[check.next_candidate++];
            float hp{};
            if (CheckBlueprintConfig(candidate, nullptr, {}, &hp) == BlueprintConfigState::Available)
                check.sources.push_back({candidate, hp});
        }
        if (check.next_candidate == check.candidates.size())
            check.stage = check.requested_source.empty()
                ? BlueprintConfigStage::Complete : BlueprintConfigStage::Borrowed;
        break;
    case BlueprintConfigStage::Borrowed: {
        CombatConfigSource source;
        check.state = CheckBlueprintConfig(check.path, &source, {}, nullptr, check.requested_source);
        check.source = source.row_name;
        check.stage = BlueprintConfigStage::Complete;
        break;
    }
    case BlueprintConfigStage::Complete:
        break;
    }
    std::scoped_lock lock(g_context.mutex);
    if (check.path == g_context.class_path.data() && requested == g_context.combat_config_blueprint)
        g_context.blueprint_config = std::move(check);
}

void UpdateAdvancedConfig() {
    std::string path;
    {
        std::scoped_lock lock(g_context.mutex);
        if (!g_context.advanced_combat_source || g_context.advanced_combat_blueprint.empty()) return;
        auto& check = g_context.advanced_config;
        if (check.path != g_context.advanced_combat_blueprint)
            check = {g_context.advanced_combat_blueprint};
        if (check.state != BlueprintConfigState::Pending) return;
        path = check.path;
    }
    float hp{};
    const auto state = CheckBlueprintConfig(path, nullptr, {}, &hp);
    std::scoped_lock lock(g_context.mutex);
    if (path == g_context.advanced_combat_blueprint)
        g_context.advanced_config = {path, state, hp};
}

std::string BirthPresetError(const std::array<BirthPreset, kBirthAttributeNames.size()>& presets) {
    const auto& localizer = g_context.localizer;
    const auto& max_hp = presets[kMaxHpAttribute];
    if (max_hp.enabled && (!std::isfinite(max_hp.value) || max_hp.value <= 0.0 ||
        max_hp.value > 10000000.0 || static_cast<float>(max_hp.value) <= 0.0F)) {
        return localizer.Text("settings.invalid_hp", "HPMaxBase must be greater than 0 and at most 10000000");
    }
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].enabled) continue;
        const auto name = kBirthAttributeNames[i];
        const std::array<std::string_view, 1> args{name};
        if (!std::isfinite(presets[i].value) ||
            std::abs(presets[i].value) > std::numeric_limits<float>::max()) {
            return localizer.Format("settings.invalid_value", "{0} must be a finite float value", args);
        }
        if (max_hp.enabled && (name == "HPMaxUp" || name == "HPMaxAdd" ||
            name == "HPMaxUpTemp" || name == "HPMaxAddTemp" || name == "OnlineMaxHPRatio")) {
            return localizer.Format("settings.hp_conflict", "Disable {0} when setting HPMaxBase", args);
        }
    }
    return {};
}

std::uintptr_t LoadServerCombatCdo(AnomalyGenerationHandleV1 library, const std::string& path) {
    if (g_layout.classDefaultObject == 0 || path.size() >= kPathCapacity) return 0;
    summon_lab::LoadClass load{};
    if (!BuildSoftClassRef(library, path.c_str(), &load.input) ||
        !InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking",
                      &load, sizeof(load))) return 0;
    const auto uclass = load.result;
    std::uintptr_t cdo{}, cdo_class{};
    std::uint32_t flags{};
    AnomalyGenerationHandleV1 handle{};
    // ClassDefaultObject is profile-validated; require a real CDO of this exact
    // loaded class. Neither the class nor its defaults are ever modified.
    if (uclass == 0 || !ReadValue(uclass + g_layout.classDefaultObject, &cdo) ||
        cdo == 0 || !ReadValue(cdo + g_layout.objectClass, &cdo_class) || cdo_class != uclass ||
        !ReadValue(cdo + g_layout.objectFlags, &flags) || (flags & 0x10U) == 0 ||
        !HandleFromPointer(cdo, &handle)) return 0;
    // The native registration accepts AI character defaults. Advanced sources
    // may come from a different family, but cannot be arbitrary UObject CDOs.
    AnomalyGenerationHandleV1 ai_class{};
    if (!FindHandle("/Script/HTGame.HTAICharacter", &ai_class)) return 0;
    auto type = uclass;
    for (int depth = 0; depth < 64 && type != 0; ++depth) {
        AnomalyGenerationHandleV1 current{};
        if (!HandleFromPointer(type, &current)) return 0;
        if (current.id == ai_class.id && current.generation == ai_class.generation)
            return cdo;
        if (!ReadValue(type + g_layout.structSuper, &type)) return 0;
    }
    return 0;
}

bool SetActorCombatConfig(AnomalyGenerationHandleV1 handle, std::uintptr_t actor, std::uint64_t config) {
    if (!IsActorUsable(handle, actor)) return false;
    std::uintptr_t type{}, field_class{};
    std::uint32_t type_id{};
    std::uint64_t property_name{};
    std::int32_t offset{}, size{}, dimension{}, owner_size{};
    char name[64]{};
    if (!ReadValue(actor + g_layout.objectClass, &type)) return false;
    const auto property = FindProperty(type, "m_CharacterConfigID");
    if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
        !ReadValue(field_class + g_layout.fieldClassName, &type_id) || !ResolveName(type_id, name, sizeof(name)) ||
        std::strcmp(name, "NameProperty") != 0 ||
        !ReadValue(property + g_layout.propertyElementSize, &size) || size != sizeof(config) ||
        !ReadValue(property + g_layout.propertyArrayDim, &dimension) || dimension != 1 ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(type + g_layout.structSize, &owner_size) ||
        offset > owner_size || size > owner_size - offset ||
        !ReadValue(property + g_layout.fieldName, &property_name)) return false;
    AnomalyGenerationHandleV1 library{};
    summon_lab::NameProperty params{actor, property_name, config};
    std::uint64_t actual{};
    return FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) &&
        InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:SetNamePropertyByName", &params, sizeof(params)) &&
        ReadValue(actor + offset, &actual) && actual == config;
}

summon_lab::Vector BatchSpawnOffset(const SummonSettings& settings, std::uint64_t index) {
    if (settings.count == 1) return {};
    // Expand a square grid around the first cell. A wall in front must not
    // send every later attempt farther into that same wall.
    const auto ring = static_cast<std::int64_t>(std::ceil((std::sqrt(static_cast<double>(index) + 1.0) - 1.0) * 0.5));
    const auto side = 2 * ring;
    auto remaining = (side + 1) * (side + 1) - 1 - static_cast<std::int64_t>(index);
    std::int64_t x = ring, y = -ring;
    if (remaining < side) x -= remaining;
    else if ((remaining -= side) < side) { x = -ring; y += remaining; }
    else if ((remaining -= side) < side) { x = -ring + remaining; y = ring; }
    else { remaining -= side; y = ring - remaining; }
    return {(1.0 + static_cast<double>(x)) * settings.spacing * 100.0,
        static_cast<double>(y) * settings.spacing * 100.0, 0.0};
}

bool CaptureBatchOrigin(AnomalyGenerationHandleV1 pawn, SummonBatch& batch) {
    if (!InvokeByPath(pawn, kPathGetTransform, &batch.origin, sizeof(batch.origin))) return false;
    if (batch.settings.count == 1) return true;
    if (!InvokeByPath(pawn, "/Script/Engine.Actor:K2_GetActorRotation",
            &batch.rotation, sizeof(batch.rotation))) return false;
    batch.occupied.push_back(batch.origin.translation);

    // A second batch may start without moving the player. Reserve the current
    // positions of managed actors once, outside the Render lock.
    std::vector<AnomalyGenerationHandleV1> handles;
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i)
            handles.push_back(g_context.records[i].handle);
    }
    for (const auto handle : handles) {
        summon_lab::Vector location{};
        if (InvokeByPath(handle, "/Script/Engine.Actor:K2_GetActorLocation", &location, sizeof(location)))
            batch.occupied.push_back(location);
    }
    return true;
}

bool HasBatchSpacing(const SummonBatch& batch, const summon_lab::Vector& location) {
    return std::none_of(batch.occupied.begin(), batch.occupied.end(), [&](const auto& occupied) {
        return std::hypot(location[0] - occupied[0], location[1] - occupied[1], location[2] - occupied[2]) +
            0.001 < batch.settings.spacing * 100.0;
    });
}

void SetBatchSpawnPlacement(SummonBatch& batch, summon_lab::SummonParameters& parms) {
    parms.transform = batch.origin;
    if (batch.settings.count == 1) return;
    const auto yaw = batch.rotation[1] * (std::acos(-1.0) / 180.0);
    const auto forward = std::cos(yaw), right = std::sin(yaw);
    do {
        parms.offset = BatchSpawnOffset(batch.settings, batch.next_slot++);
        parms.transform.translation = {
            batch.origin.translation[0] + forward * parms.offset[0] - right * parms.offset[1],
            batch.origin.translation[1] + right * parms.offset[0] + forward * parms.offset[1],
            batch.origin.translation[2]};
    } while (!HasBatchSpacing(batch, parms.transform.translation));
}

bool FinishBatchPlacement(AnomalyGenerationHandleV1 actor, SummonBatch& batch,
                          const summon_lab::Transform& transform) {
    if (batch.settings.count == 1) return true;
    // SummonAI's offset is a follow-position search: bActiveFollow can skip it,
    // or navigation can redirect it around the leader. Finalize the grid using
    // the engine's collision-aware teleport before rebasing any arena points.
    summon_lab::Vector actual{};
    if (!InvokeByPath(actor, "/Script/Engine.Actor:K2_GetActorLocation", &actual, sizeof(actual))) return false;
    // Preserve native ground/capsule adjustments inside the intended cell.
    // Teleporting back to the player's exact Z can reject an otherwise valid spawn.
    if (std::hypot(actual[0] - transform.translation[0], actual[1] - transform.translation[1]) <=
            batch.settings.spacing * 50.0 && HasBatchSpacing(batch, actual)) {
        batch.occupied.push_back(actual);
        return true;
    }
    summon_lab::SummonParameters placement{};
    placement.transform = transform;
    // A blocked cell is expected in a populated scene. Try other cells for this
    // one actor, with bounded work per Game update, instead of aborting the batch.
    for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
        if (attempt != 0) SetBatchSpawnPlacement(batch, placement);
        summon_lab::TeleportParameters teleport{placement.transform.translation, batch.rotation};
        if (!InvokeByPath(actor, "/Script/Engine.Actor:K2_TeleportTo", &teleport, sizeof(teleport))) return false;
        if (!teleport.result) continue;
        if (!InvokeByPath(actor, "/Script/Engine.Actor:K2_GetActorLocation", &actual, sizeof(actual))) return false;
        if (!HasBatchSpacing(batch, actual)) continue;
        batch.occupied.push_back(actual);
        return true;
    }
    return false;
}

#include "cutscenes.hpp"

SummonResult SummonOne(SummonBatch& batch) {
    SummonResult result;
    const auto& settings = batch.settings;
    if (settings.skip_cutscenes && !g_cutscenes.ready.load(std::memory_order_acquire)) {
        const auto message = g_context.localizer.Text("summon.cutscenes_unavailable", "Cutscene control unavailable");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    if ((settings.mute_boss_music || settings.mute_boss_dialogue) &&
        !g_context.story_audio_ready.load(std::memory_order_acquire)) {
        const auto message = g_context.localizer.Text("summon.story_audio_unavailable",
            "Boss music and dialogue control unavailable");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    AnomalyGenerationHandleV1 pawn{};
    if (!FindLocalPawn(&pawn)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: no local pawn");
        return result;
    }

    const char* path = settings.class_path.c_str();
    if (path[0] == '\0') {
        std::snprintf(result.detail, sizeof(result.detail), "summon: empty class path");
        return result;
    }
    AnomalyGenerationHandleV1 lib_cdo{};
    if (!FindHandle(kGameplayStaticsCdo, &lib_cdo)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: GameplayStatics CDO missing");
        return result;
    }

    summon_lab::SoftClassRef soft_class{};
    if (!BuildSoftClassRef(lib_cdo, path, &soft_class)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: soft class path failed");
        return result;
    }

    if (batch.completed == 0 && !CaptureBatchOrigin(pawn, batch)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: spawn origin unavailable");
        return result;
    }

    const auto selected_source = summon_lab::SelectedCombatBlueprint(settings);
    const bool advanced_source = settings.advanced_combat_source && !settings.advanced_combat_blueprint.empty();
    const bool compatibility = settings.compatibility && !advanced_source;
    auto presets = settings.presets;

    CombatConfigSource combat_config;
    if (!advanced_source && !selected_source.empty() &&
        !summon_lab::IsCombatConfigCandidate(path, selected_source)) {
        const auto message = g_context.localizer.Text("summon.config_source_invalid", "Choose a combat config from this blueprint's variants");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    // Preflight is advisory. NPCs and other noncombat blueprints can legitimately
    // have no monster row; the spawn must not depend on a positive HP lookup.
    combat_config = PrepareCombatConfig(MonsterConfigName(path), selected_source);
    if (!selected_source.empty() && combat_config.row_alias == 0) {
        const auto message = g_context.localizer.Text("summon.compat_source_unavailable",
            "Server combat source unavailable in this scene");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    const auto& server_combat_blueprint = compatibility
        ? settings.server_combat_blueprint : combat_config.server_blueprint;
    if (compatibility || !server_combat_blueprint.empty()) {
        float source_hp{};
        if (compatibility && (server_combat_blueprint.empty() ||
            MonsterConfigName(server_combat_blueprint).starts_with("FT_") ||
            CheckBlueprintConfig(server_combat_blueprint, nullptr, pawn, &source_hp) !=
                BlueprintConfigState::Available)) {
            const auto message = g_context.localizer.Text("summon.compat_source_unavailable",
                "Server combat source unavailable in this scene");
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
        auto& hp = presets[kMaxHpAttribute];
        if (compatibility && !hp.enabled) hp = {1, source_hp};
        if (compatibility && hp.value > source_hp) {
            char capacity[48]{};
            std::snprintf(capacity, sizeof(capacity), "%.9g", source_hp);
            const std::array<std::string_view, 1> args{capacity};
            const auto message = g_context.localizer.Format("summon.compat_hp_limit",
                "Compatibility HP must not exceed the source maximum {0}", args);
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
        result.server_combat_source = MonsterConfigName(server_combat_blueprint);
    }
    CombatConfigScope config_scope(combat_config);
    std::int32_t ai_level{};
    if (!ResolveSummonLevel(pawn, path, settings.level, &ai_level)) ai_level = 0;

    BirthInitialization birth;
    birth.mute_boss_music = settings.mute_boss_music;
    birth.mute_boss_dialogue = settings.mute_boss_dialogue;
    birth.skip_cutscenes = settings.skip_cutscenes;
    const auto preset_error = BirthPresetError(presets);
    if (!preset_error.empty()) {
        std::snprintf(result.detail, sizeof(result.detail), "%s", preset_error.c_str());
        return result;
    }
    // Resolve exact HP first; the native registration and GAS base still share
    // this one input, independently of the order of the other presets.
    const auto& hp_preset = presets[kMaxHpAttribute];
    if (hp_preset.enabled) {
        birth.exact_hp = static_cast<float>(hp_preset.value);
    }
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].enabled || i == kMaxHpAttribute) continue;
        const auto name = kBirthAttributeNames[i];
        const double input = presets[i].value;
        birth.attributes.push_back(BirthAttribute{std::string(name), static_cast<float>(input)});
    }
    if (birth.exact_hp > 0)
        birth.attributes.push_back(BirthAttribute{"HPMaxBase", birth.exact_hp});
    // Local combat identity is independent of optional attribute overrides.
    // Resolve the exact class even when using the boss's native attributes.
    {
        if (!g_context.birth_attributes_ready.load(std::memory_order_acquire)) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: native local combat initializer unavailable");
            return result;
        }
        summon_lab::LoadClass load_class{soft_class};
        if (!InvokeByPath(lib_cdo, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking",
                          &load_class, sizeof(load_class))) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: class load failed");
            return result;
        }
        birth.actor_class = load_class.result;
        if (birth.actor_class == 0) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: class load returned null");
            return result;
        }
    }
    if (!ResolveEditorFields(birth.actor_class, settings.editor, birth.editor_fields) ||
        (!birth.editor_fields.empty() && g_context.editor.birth_observer.id == 0) ||
        !PrepareEditorRules(settings.editor, result.editor)) {
        const auto message = g_context.localizer.Text("editor.prepare_failed", "Editor settings could not be resolved");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    if (!server_combat_blueprint.empty()) {
        birth.registration_cdo = LoadServerCombatCdo(lib_cdo, server_combat_blueprint);
        if (birth.registration_cdo == 0) {
            const auto message = g_context.localizer.Text("summon.compat_source_unavailable",
                "Server combat source unavailable in this scene");
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
    }

    summon_lab::SummonParameters parms{};
    parms.uclass = soft_class;
    parms.pet = settings.pet ? 1 : 0;
    SetBatchSpawnPlacement(batch, parms);
    parms.attack_mode = settings.aggressive ? kAttackAggressive : kAttackDefensive;
    // A player leader skips the native default-level assignment. Pass the
    // resolved scene level explicitly, before construction and registration.
    parms.level = ai_level;
    // Zero still preserves the blueprint's InheritedSkillLevel.
    parms.skill_level = static_cast<std::int32_t>(std::min(settings.skill_level, kMaxLevel));
    parms.same_camp = settings.hostile ? 0 : 1;

    bool summoned{};
    {
        // SummonAI itself defers construction, configures level/leader/camp,
        // then finishes spawning. GAS receives custom base attributes inside
        // that call; the native registration is configured after ownership.
        BirthScope scope(birth);
        summoned = InvokeByPath(pawn, kPathSummonAI, &parms, sizeof(parms));
    }
    SummonFailureCleanup cleanup{result};
    if (birth.registration_controller != 0 &&
        HandleFromPointer(birth.registration_controller, &result.registration.controller)) {
        result.registration.leader = birth.registration_leader;
        result.registration.index = birth.registration_index;
    }
    if (!summoned) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: SummonAI rejected");
        return result;
    }

    auto ai_pointer = parms.result;
    if (ai_pointer == 0) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: returned null");
        return result;
    }

    AnomalyGenerationHandleV1 ai_handle{};
    if (!HandleFromPointer(ai_pointer, &ai_handle)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: AI handle unresolved");
        return result;
    }
    result.handle = ai_handle;
    result.pointer = ai_pointer;
    if (!birth.editor_fields.empty() && (!birth.editor_applied || birth.editor_actor != ai_pointer))
        result.editor_warning = g_context.localizer.Text("editor.birth_failed", "Field overrides were not applied before BeginPlay");
    if (birth.registration_cdo != 0 && !birth.registration_substituted) {
        static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
        const auto message = g_context.localizer.Text("summon.compat_registration_failed",
            "Server combat source was not applied; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    const bool attributes_resolved = ResolveActorAttributeSet(ai_pointer, &result.ability_system,
        &result.attribute_set) && result.ability_system != 0;

    // 未登记进召唤列表的 AI 没有归属，敌对化之后会在 AI tick 触发访问违例
    {
        if (!InvokeByPath(pawn, kPathAddSummonedAI, &ai_pointer, sizeof(ai_pointer))) {
            static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
            std::snprintf(result.detail, sizeof(result.detail),
                          "summon: AddSummonedAI failed");
            return result;
        }
    }

    // Preserve the chosen source on this instance so later native lookups use
    // its donor even when another instance of the same class uses a different one.
    if (combat_config.row_alias != 0 && !SetActorCombatConfig(ai_handle, ai_pointer, combat_config.row_alias)) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        const auto message = g_context.localizer.Text("summon.config_source_failed", "Combat config could not be applied; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    if (settings.ignore_player) {
        AnomalyGenerationHandleV1 controller{};
        std::uintptr_t controller_pointer{}, player_pointer{}, player_state{}, target{};
        bool ignored = InvokeByPath(pawn, "/Script/Engine.Pawn:GetController",
            &controller_pointer, sizeof(controller_pointer)) &&
            HandleFromPointer(controller_pointer, &controller) &&
            InvokeByPath(controller, "/Script/Engine.Controller:K2_GetPawn",
                &player_pointer, sizeof(player_pointer)) && player_pointer != 0 &&
            InvokeByPath(controller, "/Script/HTGame.HTPlayerController:GetHTPlayerState",
                &player_state, sizeof(player_state)) && player_state != 0 &&
            // The native ignore list stores the PlayerState, surviving character switches.
            InvokeByPath(ai_handle, "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget",
                &player_pointer, sizeof(player_pointer)) &&
            InvokeByPath(ai_handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                &target, sizeof(target));
        if (ignored && (target == player_pointer || target == player_state)) {
            target = 0;
            ignored = InvokeByPath(ai_handle, "/Script/HTGame.HTAICharacter:SetCurrentAggroTarget",
                &target, sizeof(target));
        }
        if (!ignored) {
            static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
            const auto message = g_context.localizer.Text("summon.ignore_player_failed",
                "Could not exclude the player as a target; summon removed");
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
    }

    if (!FinishBatchPlacement(ai_handle, batch, parms.transform)) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        const auto message = g_context.localizer.Text("summon.placement_failed",
            "No free spawn position nearby; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    if (!RebaseWuchaoArena(ai_handle, ai_pointer, birth.actor_class)) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        const auto message = g_context.localizer.Text("summon.arena_failed",
            "Arena initialization failed; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    result.pet = settings.pet;
    float initial_hp{}, initial_max{};
    if (!ReadHealth(ai_handle, &initial_hp, &initial_max) || !std::isfinite(initial_hp) ||
        !std::isfinite(initial_max)) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        const auto message = g_context.localizer.Text("summon.health_uninitialized",
            "Combat health was not initialized; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    const bool registered = result.registration.controller.id != 0 && result.registration.index >= 0;
    // A real combat entity needs a native identity and an ability system.
    // An entity born at 0/0 can still be managed and removed without either.
    if (initial_max > 0.0F && (!registered || !attributes_resolved)) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        std::snprintf(result.detail, sizeof(result.detail), "summon: native combat initialization unresolved");
        return result;
    }
    // These unconfigured base classes can inherit a scripted partial-health
    // birth. Complete the new instance through the native setter after spawn
    // and ownership registration, before validating custom HP or exposing it.
    // This runs once; later damage and native health replies remain untouched.
    if (combat_config.row_alias != 0 && initial_hp > 0.0F && initial_hp < initial_max) {
        summon_lab::SetHealth health{initial_max};
        if (!InvokeByPath(ai_handle, "/Script/HTGame.HTAbilityCharacter:SetHP", &health, sizeof(health)) ||
            !ReadMatchingHealth(ai_handle, initial_max, &initial_hp, &initial_max)) {
            static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
            const auto message = g_context.localizer.Text("summon.health_uninitialized",
                "Combat health was not initialized; summon removed");
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
    }
    result.hp = initial_hp;
    result.max_hp = initial_max;
    bool birth_ok = true;
    float applied_hp{};
    if (!birth.attributes.empty()) {
        birth_ok = birth.actor == ai_pointer && birth.attribute_set == result.attribute_set &&
                   result.attribute_set != 0;
        static_cast<void>(ReadAttributeValues(result.attribute_set, &result.attributes));
        for (const auto& preset : birth.attributes) {
            const auto found = std::find_if(result.attributes.begin(), result.attributes.end(),
                [&](const AttributeValue& value) { return value.name == preset.name; });
            birth_ok = birth_ok && preset.applied && found != result.attributes.end() &&
                       HealthMatches(found->base, preset.value);
            if (preset.name == "HPMaxBase" && preset.applied && found != result.attributes.end() &&
                birth.actor == ai_pointer && birth.attribute_set == result.attribute_set &&
                HealthMatches(found->base, preset.value) && HealthMatches(initial_max, birth.exact_hp))
                applied_hp = birth.exact_hp;
        }
        if (birth.exact_hp > 0) {
            float hp{}, max_hp{};
            birth_ok = birth_ok && ReadMatchingHealth(ai_handle, birth.exact_hp, &hp, &max_hp);
        }
    } else if (result.attribute_set != 0 && batch.completed == 0) {
        // The first member is selected automatically. Other members get their
        // full attribute list when selected; birth overrides are always verified above.
        static_cast<void>(ReadAttributeValues(result.attribute_set, &result.attributes));
    }
    if (attributes_resolved)
        RegisterLocalBirth(ai_handle, ai_pointer, result.ability_system,
            result.attribute_set, applied_hp, combat_config, result.registration);
    if (registered && initial_max > 0.0F) {
        // SummonAI registers the server ability data, but unlike the native
        // opponent-summoner path it does not configure the resulting camp.
        // Mirror that native second step using the actual finished actor's camp.
        const auto controller = result.registration.controller;
        std::uint8_t camp{};
        ConfigureSummon configuration{birth.registration_leader, birth.registration_index, 0, 1.0F};
        bool configured = ReadCamp(ai_pointer, &camp);
        configuration.camp = camp;
        if (configured && applied_hp > 0.0F) {
            std::scoped_lock lock(g_local_birth_mutex);
            g_birth_configurations.push_back({ai_handle, controller, result.ability_system,
                configuration, applied_hp, 0.0F, false, compatibility});
        } else {
            configured = configured && InvokeByPath(controller,
                "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
                &configuration, sizeof(configuration));
        }
        if (!configured) {
            static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
            result.ok = false;
            std::snprintf(result.detail, sizeof(result.detail), "summon: SummonAI rejected");
            return result;
        }
        if (g_context.core != nullptr) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "SummonLab native-configuration: actor=%p index=%d camp=%u %s",
                reinterpret_cast<void*>(ai_pointer), birth.registration_index, static_cast<unsigned>(camp),
                applied_hp > 0.0F ? "waiting for native maximum" : "initialHPPercent=1 submitted");
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                anomaly::sdk::StringView(message));
        }
    }
    if (birth.registration_substituted && g_context.core != nullptr) {
        char message[kPathCapacity + 160]{};
        std::snprintf(message, sizeof(message),
            "SummonLab combat-source: actor=%p index=%d blueprint=%s serverSource=%s requestedHP=%.9g",
            reinterpret_cast<void*>(ai_pointer), birth.registration_index, path,
            result.server_combat_source.c_str(), birth.exact_hp);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            anomaly::sdk::StringView(message));
    }
    // 取蓝图短名做显示
    const char* separator = std::strrchr(path, '.');
    if (separator == nullptr) separator = std::strrchr(path, '/');
    const char* name = separator != nullptr ? separator + 1 : path;
    const bool named = SetSpawnDisplayName(ai_handle, ai_pointer, settings.display_name.c_str());
    std::snprintf(result.label, sizeof(result.label), "%s",
                  named && !settings.display_name.empty() ? settings.display_name.c_str() : name);
    if (!birth_ok || initial_max <= 0.0F) {
        const auto message = !birth_ok
            ? g_context.localizer.Text("summon.attributes_unapplied", "Summoned; some custom attributes were not applied")
            : g_context.localizer.Text("summon.no_combat_health", "Summoned without combat health");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
    } else if (!named) {
        const auto message = g_context.localizer.Text("summon.name_failed",
            "Summoned, but the name could not be set");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
    } else {
        std::snprintf(result.detail, sizeof(result.detail),
                      "summoned %s (level %d, %s)", name, ai_level,
                      birth.attributes.empty() ? "native attributes" : "birth attributes initialized");
    }
    result.ok = true;
    return result;
}

bool DismissSummoned(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer,
                     float previous_max_hp) {
    // Dead actors already follow the game's death cleanup. A stale list entry
    // must never send their cached address back into the summon/destroy paths.
    if (!IsActorAlive(handle, pointer)) return true;
    float hp{}, max_hp{};
    if (!ReadHealth(handle, &hp, &max_hp)) return false;
    // A living actor can lack a native combat config and remain at 0/0 from
    // birth. Allow removing it, but retain the death guard for an actor that
    // had initialized health before its attributes were cleared during death.
    if (hp <= 0.0F && (max_hp > 0.0F || previous_max_hp > 0.0F)) return true;

    // DismissSummonedAI compacts the leader's array without updating the
    // survivors' native summon indices. Destroy just this actor instead;
    // UE clears its object reference and native AddSummonedAI reuses that
    // vacant slot without moving the other registrations.
    return InvokeByPath(handle, kPathDestroyActor, nullptr, 0) &&
        !IsActorUsable(handle, pointer);
}

struct DuelActor {
    std::uint64_t id{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    AnomalyGenerationHandleV1 controller{};
    bool pet{};
    SummonIdentity registration;
    bool involved{};
    bool alive{};
    std::uint8_t camp{};
    std::uintptr_t previous_target{};
    std::uintptr_t controller_pointer{};
    bool battle_tuned{};
    float original_chase_range{};
    std::uint8_t original_leave_battle{};
    bool boss_hp_bar_visible{true};
    AggroSettings aggro;
    float hp{}, max_hp{};
    summon_lab::Vector location{};
    bool location_valid{}, target_paused{};
};

const DuelActor* ChooseBattleTarget(const DuelActor& actor, const std::vector<DuelActor>& actors,
                                    const summon_lab::BattleGraph& graph) {
    if (actor.aggro.paused) return nullptr;
    std::vector<const DuelActor*> choices;
    const bool needs_distance = actor.aggro.chase_distance > 0 || actor.aggro.policy == TargetPolicy::Nearest;
    const auto distance = [&](const auto& other) {
        return std::hypot(actor.location[0] - other.location[0], actor.location[1] - other.location[1],
            actor.location[2] - other.location[2]);
    };
    for (const auto& candidate : actors) {
        if (!candidate.alive || candidate.pet || !graph.Linked(actor.id, candidate.id)) continue;
        if (needs_distance && (!actor.location_valid || !candidate.location_valid)) continue;
        if (actor.aggro.chase_distance > 0 && distance(candidate) > actor.aggro.chase_distance * 100.0) continue;
        choices.push_back(&candidate);
    }
    if (choices.empty()) return nullptr;
    for (const auto* candidate : choices)
        if (candidate->id == actor.aggro.priority_target) return candidate;
    if (actor.aggro.policy == TargetPolicy::Native) {
        for (const auto* candidate : choices)
            if (candidate->pointer == actor.previous_target) return candidate;
    }
    if (actor.aggro.policy == TargetPolicy::Nearest || actor.aggro.policy == TargetPolicy::LowestHealth) {
        const auto score = [&](const DuelActor* candidate) {
            return actor.aggro.policy == TargetPolicy::Nearest ? distance(*candidate) :
                (candidate->max_hp > 0 ? static_cast<double>(candidate->hp) / candidate->max_hp :
                    std::numeric_limits<double>::infinity());
        };
        return *std::min_element(choices.begin(), choices.end(), [&](const auto* a, const auto* b) {
            const auto left = score(a), right = score(b);
            // Keep a valid current target on ties to avoid gratuitous switching.
            return left < right || (left == right && a->pointer == actor.previous_target &&
                b->pointer != actor.previous_target);
        });
    }
    return choices[(actor.id - 1) % choices.size()];
}

void ReadBattleLocations(std::vector<DuelActor>& actors) {
    const bool needed = std::any_of(actors.begin(), actors.end(), [](const auto& actor) {
        return actor.aggro.chase_distance > 0 || actor.aggro.policy == TargetPolicy::Nearest;
    });
    if (!needed) return;
    for (auto& actor : actors)
        if (actor.alive)
            actor.location_valid = InvokeByPath(actor.handle, "/Script/Engine.Actor:K2_GetActorLocation",
                &actor.location, sizeof(actor.location));
}

bool ResolveDuelController(DuelActor& actor) {
    std::uint8_t finding{};
    return InvokeByPath(actor.handle, "/Script/Engine.Pawn:GetController",
                        &actor.controller_pointer, sizeof(actor.controller_pointer)) &&
        HandleFromPointer(actor.controller_pointer, &actor.controller) &&
        InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:GetUseFindAggroTarget",
                      &finding, sizeof(finding));
}

void DuelResult(bool ok, std::string_view detail) {
    if (g_context.core != nullptr && g_context.core->log != nullptr)
        g_context.core->log(g_context.core->user, ok ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
            ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(detail));
    std::scoped_lock lock(g_context.mutex);
    g_context.last_result_failed = !ok;
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%.*s",
                  static_cast<int>(detail.size()), detail.data());
}

bool PauseDuelActor(const DuelActor& actor) {
    std::uint8_t disabled = 1, searching = 0, forced = 0;
    std::array<std::uint8_t, 2> paused{1, 1};
    std::uintptr_t target{};
    // Try each independent stop even if another interface fails. The ignore
    // list intentionally remains in effect after the duel ends.
    bool ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetAttackDisable",
                           &disabled, sizeof(disabled));
    ok = InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetUseFindAggroTarget",
                       &searching, sizeof(searching)) && ok;
    ok = InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetAggroPaused",
                       paused.data(), paused.size()) && ok;
    ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetCurrentAggroTarget",
                       &target, sizeof(target)) && ok;
    ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetForceBattleBehavior",
                       &forced, sizeof(forced)) && ok;
    return ok;
}

bool ActivateBattleTarget(const DuelActor& actor, const DuelActor& opponent) {
    std::array<std::uint8_t, 2> resumed{0, 0};
    std::uint8_t disabled = 0, forced = 1;
    std::uint8_t searching = actor.aggro.policy == TargetPolicy::Native &&
        actor.aggro.priority_target == 0 && actor.aggro.chase_distance == 0 ? 1 : 0;
    struct { std::uintptr_t target; float aggro_percent; } target{opponent.pointer, 1.0F};
    return InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetAttackDisable",
                       &disabled, sizeof(disabled)) &&
        InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetAggroPaused",
                       resumed.data(), resumed.size()) &&
        InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetUseFindAggroTarget",
                       &searching, sizeof(searching)) &&
        InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetForceBattleBehavior",
                       &forced, sizeof(forced)) &&
        InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetCurrentAggroTarget",
                       &target, sizeof(target));
}

bool FindDuelCamps(std::uint8_t player_camp, std::vector<std::uint8_t>& camps) {
    AnomalyGenerationHandleV1 cdo{}, data{};
    std::uintptr_t pointer{};
    if (!FindHandle("/Script/HTGame.Default__HTGameData", &cdo) ||
        !InvokeByPath(cdo, "/Script/HTGame.HTGameData:GetGameData", &pointer, sizeof(pointer)) ||
        !HandleFromPointer(pointer, &data)) return false;
    const auto hostile = [&](std::uint8_t from, std::uint8_t to) {
        std::array<std::uint8_t, 3> params{from, to, 1};
        return InvokeByPath(data, "/Script/HTGame.HTGameData:GetTeamAttitude",
                             params.data(), params.size()) && params[2] == 2;
    };
    // Select the largest mutually hostile palette from the active game data.
    // Same-camp hostility is rejected by native code, even if its table says
    // otherwise. Never edit the global table to force an unsupported graph.
    const auto count = g_context.camp_count;
    std::vector<bool> eligible(count);
    std::vector<std::vector<bool>> enemies(count, std::vector<bool>(count));
    for (std::uint8_t a = 0; a < count; ++a) {
        eligible[a] = hostile(a, player_camp) && hostile(player_camp, a);
        for (std::uint8_t b = a + 1; b < count; ++b) {
            enemies[a][b] = enemies[b][a] = hostile(a, b) && hostile(b, a);
        }
    }
    for (unsigned mask = 1; mask < (1U << count); ++mask) {
        std::vector<std::uint8_t> candidate;
        bool valid = true;
        for (std::uint8_t a = 0; a < count && valid; ++a) {
            if ((mask & (1U << a)) == 0) continue;
            valid = eligible[a] && std::all_of(candidate.begin(), candidate.end(),
                [&](auto b) { return enemies[a][b]; });
            candidate.push_back(a);
        }
        if (valid && candidate.size() > camps.size()) camps = std::move(candidate);
    }
    return camps.size() >= 2;
}

bool FindLocalPlayerState(AnomalyGenerationHandleV1* player_state) {
    AnomalyGenerationHandleV1 pawn{}, controller{};
    std::uintptr_t controller_pointer{}, player_state_pointer{};
    return FindLocalPawn(&pawn) &&
        InvokeByPath(pawn, "/Script/Engine.Pawn:GetController",
                     &controller_pointer, sizeof(controller_pointer)) &&
        HandleFromPointer(controller_pointer, &controller) &&
        InvokeByPath(controller, "/Script/HTGame.HTPlayerController:GetHTPlayerState",
                     &player_state_pointer, sizeof(player_state_pointer)) &&
        HandleFromPointer(player_state_pointer, player_state);
}

bool ShowDuelBossHpBar(const DuelActor& actor, AnomalyGenerationHandleV1 player_state) {
    // SetDisableBossHPBar only notifies an aggro target that is a PlayerState.
    // A duel targets another AI, so register and show each Boss for the local
    // viewer explicitly. The native HUD owns the widgets and their lifetime.
    std::array<std::uint8_t, 2> enabled{0, 0};
    summon_lab::ObjectBool show{actor.pointer};
    // bIncludeDescUI stays false: opening a fight should only show its HP bars.
    summon_lab::ObjectBool unhide{actor.pointer};
    return InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetDisableBossHPBar",
                         enabled.data(), enabled.size()) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientRegisterBossHPBarCandidate",
                         &show.object, sizeof(show.object)) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientHideBossHPBarByCharacter",
                         &unhide, sizeof(unhide)) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientShowBossHPBar",
                         &show, sizeof(show));
}

bool HideDuelBossHpBar(const DuelActor& actor, AnomalyGenerationHandleV1 player_state) {
    std::array<std::uint8_t, 2> disabled{1, 1};
    summon_lab::ObjectBool hide{actor.pointer, 1};
    return InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetDisableBossHPBar",
                        disabled.data(), disabled.size()) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientHideBossHPBarByCharacter",
                     &hide, sizeof(hide));
}

bool SetReflectedBool(std::uintptr_t object, const char* name, std::uint8_t value) {
    std::uintptr_t actor_class{}, field_class{};
    std::uint32_t type_id{};
    std::int32_t offset{}, size{}, dimension{};
    std::uint64_t property_name{};
    char type[64]{};
    if (!ReadValue(object + g_layout.objectClass, &actor_class)) return false;
    const auto property = FindProperty(actor_class, name);
    if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
        !ReadValue(field_class + g_layout.fieldClassName, &type_id) || !ResolveName(type_id, type, sizeof(type)) ||
        std::strcmp(type, "BoolProperty") != 0 ||
        !ReadValue(property + g_layout.propertyElementSize, &size) || size != 1 ||
        !ReadValue(property + g_layout.propertyArrayDim, &dimension) || dimension != 1 ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(property + g_layout.fieldName, &property_name)) return false;
    AnomalyGenerationHandleV1 library{};
    summon_lab::BoolProperty params{object, property_name, value};
    std::uint8_t actual{};
    return FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) &&
        InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:SetBoolPropertyByName",
                       &params, sizeof(params)) &&
        ReadValue(object + offset, &actual) && actual == value;
}

bool DisableLeaderFollowing(const DuelActor& actor) {
    // Retain the leader registration; only disable movement following.
    return SetReflectedBool(actor.pointer, "bFollowMyLeader", 0);
}

bool SetControllerChaseRange(const DuelActor& actor, float range) {
    if (g_context.current_chase_range_offset == 0) return false;
    float actual{};
    return InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetCurChaseRange",
                        &range, sizeof(range)) &&
        ReadValue(actor.controller_pointer + g_context.current_chase_range_offset, &actual) &&
        actual == range;
}

bool TuneDuelController(DuelActor& actor) {
    if (!actor.battle_tuned) {
        if (g_context.current_chase_range_offset == 0 ||
            !ReadValue(actor.controller_pointer + g_context.current_chase_range_offset,
                       &actor.original_chase_range) ||
            !std::isfinite(actor.original_chase_range) || actor.original_chase_range <= 0 ||
            !ReadReflectedValue(actor.controller_pointer, "bUseLeaveBattle", "BoolProperty",
                                &actor.original_leave_battle)) return false;
        actor.battle_tuned = true;
    }
    return SetReflectedBool(actor.controller_pointer, "bUseLeaveBattle", 0) &&
        SetControllerChaseRange(actor, std::max(actor.original_chase_range, kBattleChaseRange));
}

bool RestoreDuelController(DuelActor& actor) {
    if (!actor.battle_tuned) return true;
    const bool range_restored = SetControllerChaseRange(actor, actor.original_chase_range);
    const bool leave_restored = SetReflectedBool(actor.controller_pointer, "bUseLeaveBattle",
                                                actor.original_leave_battle);
    const bool restored = range_restored && leave_restored;
    if (restored) actor.battle_tuned = false;
    return restored;
}

bool SetBattleCamp(const DuelActor& actor, std::uint8_t camp) {
    std::uint8_t actual{};
    if (!InvokeByPath(actor.handle, "/Script/HTGame.HTAbilityCharacter:SetCharacterCampType",
                      &camp, sizeof(camp)) ||
        !ReadCamp(actor.pointer, &actual) || actual != camp) return false;
    if (actor.registration.controller.id == 0) return actor.registration.index < 0;
    if (actor.registration.index < 0) return false;
    // Verified in 1.4b live code and 1.5a IDA: -1 updates
    // the registered combat camp and skips HP assignment. Never refill HP
    // while editing battle links, or leave server damage on the birth camp.
    ConfigureSummon configuration{actor.registration.leader, actor.registration.index, camp, -1.0F};
    return InvokeByPath(actor.registration.controller,
            "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
            &configuration, sizeof(configuration));
}

void ChangeBattle(const PendingOp& op, bool forgetting = false) {
    if (forgetting) {
        // Removing a dead actor cannot invalidate the survivors' camp colors.
        // Detach its edges once; maintenance retargets/restores survivors.
        std::scoped_lock lock(g_context.mutex);
        g_context.battles.Remove(op.record_id);
        return;
    }
    const bool group = op.kind == OpKind::SetGroupBattle || op.kind == OpKind::StopGroupBattle;
    const auto first = op.record_id;
    const auto second = op.target_id;
    const auto fail = [] {
        DuelResult(false, g_context.localizer.Text("duel.failed",
            "Battle unavailable: check that the Bosses are alive and ready"));
    };
    if (group ? op.members.empty() : (first == 0 || first == second ||
        (op.kind == OpKind::SetBattleEdge && second == 0))) { fail(); return; }
    summon_lab::BattleGraph previous;
    std::vector<DuelActor> actors;
    {
        std::scoped_lock lock(g_context.mutex);
        previous = g_context.battles;
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            const auto& record = g_context.records[i];
            DuelActor actor{record.id, record.handle, record.pointer, {}, record.pet, record.registration};
            actor.battle_tuned = record.battle_tuned;
            actor.original_chase_range = record.original_chase_range;
            actor.original_leave_battle = record.original_leave_battle;
            actor.boss_hp_bar_visible = record.boss_hp_bar_visible;
            actor.aggro = record.aggro;
            actors.push_back(actor);
        }
    }
    for (auto& actor : actors) {
        float hp{}, max_hp{};
        actor.alive = IsActorAlive(actor.handle, actor.pointer) &&
            ReadHealth(actor.handle, &hp, &max_hp) && hp > 0 && max_hp > 0;
        actor.hp = hp;
        actor.max_hp = max_hp;
    }
    ReadBattleLocations(actors);
    auto next = previous;
    if (op.kind == OpKind::SetGroupBattle) {
        auto members = op.members;
        auto opponents = op.opponents;
        const auto unavailable = [&](auto id) {
            return std::none_of(actors.begin(), actors.end(), [id](const auto& actor) {
                return actor.id == id && actor.alive && !actor.pet;
            });
        };
        std::erase_if(members, unavailable);
        std::erase_if(opponents, unavailable);
        if (members.empty() || opponents.empty()) { fail(); return; }
        next.SetBetween(members, opponents, op.enabled);
    } else if (op.kind == OpKind::StopGroupBattle) {
        for (const auto id : op.members) next.Remove(id);
    } else if (op.kind == OpKind::SelectAllOpponents) {
        // Build one graph from the complete management list, independent of
        // pagination. Validate and apply its camp assignment only once.
        for (const auto& actor : actors)
            if (actor.alive && !actor.pet) next.Set(first, actor.id, true);
    } else if (op.kind == OpKind::StopDuel) next.Remove(first);
    else next.Set(first, second, op.enabled);
    if (next.edges == previous.edges) return;
    auto combined = previous;
    for (const auto& [a, b] : next.edges) combined.Set(a, b, true);
    auto component = group ? std::vector<std::uint64_t>{} : combined.Component(first);
    if (group) {
        for (const auto id : op.members) {
            if (!combined.Contains(id)) continue;
            if (std::find(component.begin(), component.end(), id) != component.end()) continue;
            const auto connected = combined.Component(id);
            component.insert(component.end(), connected.begin(), connected.end());
        }
    }
    for (auto& actor : actors)
        actor.involved = std::find(component.begin(), component.end(), actor.id) != component.end();
    // On partial failure, pause only validated survivors in affected components.
    const auto stop_component = [&] {
        for (auto& actor : actors) {
            if (actor.involved && actor.alive && actor.controller.id != 0) {
                static_cast<void>(PauseDuelActor(actor));
                static_cast<void>(RestoreDuelController(actor));
            }
        }
        for (const auto id : component) previous.Remove(id);
        std::scoped_lock lock(g_context.mutex);
        g_context.battles = previous;
        for (auto& record : g_context.records)
            for (const auto& actor : actors)
                if (record.id == actor.id && actor.involved) {
                    record.battle_tuned = actor.battle_tuned;
                    record.current_target_id = 0;
                    record.target_paused = true;
                }
    };
    bool ready = g_context.current_chase_range_offset != 0;
    std::vector<summon_lab::BattleCamp> assignments;
    for (auto& actor : actors) {
        if (!actor.involved) continue;
        std::uint8_t role{};
        const bool resolved = actor.alive && !actor.pet &&
            InvokeByPath(actor.handle, "/Script/Engine.Actor:GetLocalRole", &role, sizeof(role)) &&
            role == 3 && ResolveDuelController(actor);
        ready = resolved && ready;
        if (!resolved || !next.Contains(actor.id)) continue;
        ready = ReadCamp(actor.pointer, &actor.camp) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                           &actor.previous_target, sizeof(actor.previous_target)) && ready;
        assignments.push_back({actor.id, actor.camp});
    }
    for (const auto& [a, b] : next.edges) {
        if (std::find(component.begin(), component.end(), a) == component.end()) continue;
        for (const auto id : {a, b})
            ready = std::any_of(assignments.begin(), assignments.end(),
                [id](const auto& actor) { return actor.id == id; }) && ready;
    }
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (const auto& actor : actors) {
            if (!actor.involved) continue;
            ready = std::none_of(g_birth_configurations.begin(), g_birth_configurations.end(),
                [&](const auto& pending) { return pending.actor_handle.id == actor.handle.id &&
                    pending.actor_handle.generation == actor.handle.generation; }) && ready;
        }
    }
    if (!ready) {
        fail();
        return;
    }

    AnomalyGenerationHandleV1 pawn{}, controller{}, player_state{};
    std::uintptr_t player_pointer{}, controller_pointer{}, player_state_pointer{};
    std::uint8_t player_camp{};
    std::vector<std::uint8_t> camps;
    if (!assignments.empty() && (!FindLocalPawn(&pawn) ||
        !InvokeByPath(pawn, "/Script/Engine.Pawn:GetController", &controller_pointer, sizeof(controller_pointer)) ||
        !HandleFromPointer(controller_pointer, &controller) ||
        !InvokeByPath(controller, "/Script/Engine.Controller:K2_GetPawn", &player_pointer, sizeof(player_pointer)) ||
        !InvokeByPath(controller, "/Script/HTGame.HTPlayerController:GetHTPlayerState",
                       &player_state_pointer, sizeof(player_state_pointer)) ||
        !HandleFromPointer(player_state_pointer, &player_state) ||
        !ReadCamp(player_pointer, &player_camp) ||
        !FindDuelCamps(player_camp, camps))) {
        fail(); return;
    }
    if (!summon_lab::AssignBattleCamps(next, camps, assignments)) {
        DuelResult(false, g_context.localizer.Text("duel.camps_unavailable",
            "This opponent graph exceeds the native camp limit"));
        return;
    }
    bool ok = true;
    for (auto& actor : actors) {
        if (!actor.involved || !actor.alive) continue;
        ok = PauseDuelActor(actor) && DisableLeaderFollowing(actor) && ok;
        if (!next.Contains(actor.id)) {
            ok = RestoreDuelController(actor) && ok;
            actor.previous_target = 0;
            actor.target_paused = true;
            continue;
        }
        ok = TuneDuelController(actor) && ok;
        // Native 1.4b converts a player pawn to its PlayerState here, so the
        // exclusion survives switching the active player character.
        ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget",
                           &player_pointer, sizeof(player_pointer)) && ok;
        for (const auto& other : actors) {
            if (!other.alive || other.id == actor.id) continue;
            const char* path = next.Linked(actor.id, other.id)
                ? "/Script/HTGame.HTAICharacter:RemoveIgnoredAggroTarget"
                : "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget";
            auto target = other.pointer;
            ok = InvokeByPath(actor.handle, path, &target, sizeof(target)) && ok;
        }
    }
    for (const auto& assignment : assignments) {
        if (!ok) break;
        const auto actor = std::find_if(actors.begin(), actors.end(),
            [&](const auto& value) { return value.id == assignment.id; });
        ok = SetBattleCamp(*actor, static_cast<std::uint8_t>(assignment.assigned));
    }
    // All participants must be attackable before assigning targets: forcing
    // a target also updates native battle behavior synchronously.
    for (const auto& actor : actors) {
        if (!ok) break;
        if (!actor.involved || !next.Contains(actor.id)) continue;
        std::uint8_t disabled = 0;
        ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetAttackDisable",
                           &disabled, sizeof(disabled));
    }
    for (auto& actor : actors) {
        if (!ok) break;
        if (!actor.involved || !next.Contains(actor.id)) continue;
        const auto* opponent = ChooseBattleTarget(actor, actors, next);
        std::uintptr_t actual_target{};
        ok = opponent ? ActivateBattleTarget(actor, *opponent) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                &actual_target, sizeof(actual_target)) && actual_target == opponent->pointer : PauseDuelActor(actor);
        actor.previous_target = actual_target;
        actor.target_paused = opponent == nullptr;
        ok = ok && (actor.boss_hp_bar_visible ? ShowDuelBossHpBar(actor, player_state) :
                HideDuelBossHpBar(actor, player_state));
    }
    if (!ok) {
        stop_component();
        DuelResult(false, g_context.localizer.Text("duel.apply_failed",
            "Battle update failed; connected participants were stopped"));
        return;
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.battles = std::move(next);
        for (auto& record : g_context.records)
            for (const auto& actor : actors)
                if (record.id == actor.id && actor.involved) {
                    record.battle_tuned = actor.battle_tuned;
                    record.original_chase_range = actor.original_chase_range;
                    record.original_leave_battle = actor.original_leave_battle;
                    record.target_paused = actor.target_paused;
                    const auto target = std::find_if(actors.begin(), actors.end(), [&](const auto& other) {
                        return other.pointer != 0 && other.pointer == actor.previous_target;
                    });
                    record.current_target_id = target == actors.end() ? 0 : target->id;
                }
    }
    DuelResult(true, g_context.localizer.Text("duel.updated", "Opponents updated"));
}

void ExcludeNewSummon(const SummonResult& result) {
    std::vector<DuelActor> fighters;
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            const auto& record = g_context.records[i];
            if (g_context.battles.Contains(record.id))
                fighters.push_back({record.id, record.handle, record.pointer});
        }
    }
    // An existing fight must not acquire a newly summoned, unlinked Boss.
    for (const auto& actor : fighters) {
        auto target = result.pointer;
        if (IsActorAlive(actor.handle, actor.pointer))
            static_cast<void>(InvokeByPath(actor.handle,
                "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget",
                &target, sizeof(target)));
    }
}

void ForgetRecord(std::uint64_t record_id) {
    ChangeBattle(PendingOp{OpKind::StopDuel, record_id}, true);
    AnomalyGenerationHandleV1 handle{};
    SummonIdentity registration;
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            if (g_context.records[i].id != record_id) continue;
            handle = g_context.records[i].handle;
            registration = g_context.records[i].registration;
            g_context.records.erase(g_context.records.begin() + i);
            if (g_context.selected_record_id == record_id) {
                g_context.selected_record_id = 0;
            }
            break;
        }
        const auto end = std::remove_if(g_context.pending.begin(),
            g_context.pending.begin() + g_context.pending.size(),
            [&](const PendingOp& op) { return op.record_id == record_id || op.target_id == record_id; });
        g_context.pending.erase(end, g_context.pending.end());
        g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending.size()),
                             std::memory_order_release);
        for (auto& record : g_context.records) {
            if (record.aggro.priority_target == record_id) record.aggro.priority_target = 0;
            if (record.current_target_id == record_id) record.current_target_id = 0;
        }
    }
    if (handle.id != 0) {
        ForgetLocalBirth(handle);
        static_cast<void>(UnregisterNativeSummon(registration));
    }
}

void RemoveOne(std::uint64_t record_id) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    float previous_max_hp{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            if (g_context.records[i].id != record_id) continue;
            handle = g_context.records[i].handle;
            pointer = g_context.records[i].pointer;
            previous_max_hp = g_context.records[i].max_hp;
            break;
        }
    }
    if (!DismissSummoned(handle, pointer, previous_max_hp)) {
        DuelResult(false, g_context.localizer.Text("manage.failed",
            "Action failed: the entity or function is unavailable"));
        return;
    }
    ForgetRecord(record_id);
}

void ClearAll() {
    std::scoped_lock lock(g_context.mutex);
    // Removal shares the Game update budget, even for an unlimited batch.
    // Put these before later commands so ClearAll retains its queue ordering.
    for (auto it = g_context.records.rbegin(); it != g_context.records.rend(); ++it)
        g_context.pending.push_front(PendingOp{OpKind::Remove, it->id});
    g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending.size()), std::memory_order_release);
}

void SetActorOption(const PendingOp& op) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            if (g_context.records[i].id == op.record_id) {
                handle = g_context.records[i].handle;
                pointer = g_context.records[i].pointer;
                break;
            }
        }
    }
    bool applied = false;
    if (IsActorAlive(handle, pointer)) {
        // Reflected parameter layouts from the 1.4b SDK; all calls run in DrainOne.
        if (op.kind == OpKind::SetBossHpBar) {
            AnomalyGenerationHandleV1 player_state{};
            const DuelActor actor{op.record_id, handle, pointer};
            applied = FindLocalPlayerState(&player_state) &&
                (op.enabled ? ShowDuelBossHpBar(actor, player_state) :
                    HideDuelBossHpBar(actor, player_state));
        } else {
            std::uint8_t enabled = op.enabled ? 1 : 0;
            const char* path = op.kind == OpKind::SetCollision
                ? "/Script/Engine.Actor:SetActorEnableCollision"
                : "/Script/Engine.Actor:SetActorHiddenInGame";
            applied = InvokeByPath(handle, path, &enabled, sizeof(enabled));
        }
    } else {
        ForgetRecord(op.record_id);
    }
    const auto detail = applied ? std::string{} : g_context.localizer.Text(
        "manage.failed", "Action failed: the entity or function is unavailable");
    std::scoped_lock lock(g_context.mutex);
    if (applied && op.kind == OpKind::SetBossHpBar)
        for (auto& record : g_context.records)
            if (record.id == op.record_id) record.boss_hp_bar_visible = op.enabled;
    g_context.last_result_failed = !applied;
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                  detail.c_str());
}


void RefreshHealth(bool refresh_attributes) {

    struct HealthTarget {
        std::uint64_t id{};
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t pointer{};
        float previous_max_hp{};
    };
    std::vector<HealthTarget> snapshot;
    std::size_t count{};
    std::uint64_t selected_id{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.records.size();
        snapshot.resize(count);
        selected_id = g_context.selected_record_id;
        for (std::size_t i = 0; i < count; ++i)
            snapshot[i] = {g_context.records[i].id, g_context.records[i].handle,
                           g_context.records[i].pointer, g_context.records[i].max_hp};
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!IsActorAlive(snapshot[i].handle, snapshot[i].pointer)) {
            ForgetRecord(snapshot[i].id);
            continue;
        }
        float hp{};
        float max_hp{};
        if (!ReadHealth(snapshot[i].handle, &hp, &max_hp)) continue;
        if (hp <= 0.0F && (max_hp > 0.0F || snapshot[i].previous_max_hp > 0.0F)) {
            ForgetRecord(snapshot[i].id);
            continue;
        }
        std::vector<AttributeValue> attributes;
        std::uintptr_t ability_system{}, attribute_set{};
        const bool attributes_read = refresh_attributes && snapshot[i].id == selected_id &&
            ResolveActorAttributeSet(snapshot[i].pointer, &ability_system, &attribute_set) &&
            ReadAttributeValues(attribute_set, &attributes);
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t k = 0; k < g_context.records.size(); ++k) {
            auto& record = g_context.records[k];
            if (record.id != snapshot[i].id) continue;
            record.hp = hp;
            record.max_hp = max_hp;
            if (attributes_read) {
                record.attributes = std::move(attributes);
            }
            break;
        }
    }
}

bool MaintainBattles(const std::vector<std::uint64_t>& changed = {}) {
    summon_lab::BattleGraph graph;
    std::vector<DuelActor> actors;
    {
        std::scoped_lock lock(g_context.mutex);
        graph = g_context.battles;
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            const auto& record = g_context.records[i];
            if ((!graph.Contains(record.id) && !record.battle_tuned) ||
                record.hp <= 0 || record.max_hp <= 0) continue;
            DuelActor actor{record.id, record.handle, record.pointer};
            actor.pet = record.pet;
            actor.battle_tuned = record.battle_tuned;
            actor.original_chase_range = record.original_chase_range;
            actor.original_leave_battle = record.original_leave_battle;
            actor.boss_hp_bar_visible = record.boss_hp_bar_visible;
            actor.aggro = record.aggro;
            actor.hp = record.hp;
            actor.max_hp = record.max_hp;
            actor.target_paused = record.target_paused;
            actors.push_back(actor);
        }
    }
    for (auto& actor : actors) {
        actor.alive = IsActorAlive(actor.handle, actor.pointer) && ResolveDuelController(actor);
    }
    ReadBattleLocations(actors);
    bool applied = true;
    for (auto& actor : actors) {
        const bool reapply = std::find(changed.begin(), changed.end(), actor.id) != changed.end();
        if (!actor.alive || !actor.battle_tuned) {
            if (reapply) applied = false;
            continue;
        }
        if (!graph.Contains(actor.id)) {
            if (PauseDuelActor(actor) && RestoreDuelController(actor)) {
                std::scoped_lock lock(g_context.mutex);
                for (auto& record : g_context.records)
                    if (record.id == actor.id) {
                        record.battle_tuned = false;
                        record.current_target_id = 0;
                        record.target_paused = true;
                    }
            }
            continue;
        }
        float range{};
        std::uint8_t leave_battle{};
        if (!ReadValue(actor.controller_pointer + g_context.current_chase_range_offset, &range) ||
            !ReadReflectedValue(actor.controller_pointer, "bUseLeaveBattle", "BoolProperty",
                                &leave_battle)) { if (reapply) applied = false; continue; }
        if (range < std::max(actor.original_chase_range, kBattleChaseRange) || leave_battle != 0)
            if (!TuneDuelController(actor)) { if (reapply) applied = false; continue; }
        if (!InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                          &actor.previous_target, sizeof(actor.previous_target))) {
            if (reapply) applied = false;
            continue;
        }
        const auto* opponent = ChooseBattleTarget(actor, actors, graph);
        const bool retarget = reapply || (opponent ? actor.target_paused ||
            actor.previous_target != opponent->pointer : !actor.target_paused || actor.previous_target != 0);
        if (retarget && !(opponent ? ActivateBattleTarget(actor, *opponent) : PauseDuelActor(actor))) {
            if (reapply) applied = false;
            continue;
        }
        if (retarget && !InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                &actor.previous_target, sizeof(actor.previous_target))) {
            if (reapply) applied = false;
            continue;
        }
        if (reapply && actor.previous_target != (opponent ? opponent->pointer : 0)) applied = false;
        const auto actual = std::find_if(actors.begin(), actors.end(), [&](const auto& candidate) {
            return candidate.alive && candidate.pointer != 0 && candidate.pointer == actor.previous_target;
        });
        {
            std::scoped_lock lock(g_context.mutex);
            for (auto& record : g_context.records)
                if (record.id == actor.id) {
                    record.current_target_id = actual == actors.end() ? 0 : actual->id;
                    record.target_paused = opponent == nullptr && actor.previous_target == 0;
                }
        }
        if (retarget && !actor.boss_hp_bar_visible) {
            AnomalyGenerationHandleV1 player_state{};
            if (FindLocalPlayerState(&player_state))
                static_cast<void>(HideDuelBossHpBar(actor, player_state));
        }
    }
    return applied;
}

void ApplyAggroSettings(const PendingOp& op) {
    if (!std::isfinite(op.aggro.chase_distance) || op.aggro.chase_distance < 0 ||
        op.aggro.chase_distance > kBattleChaseRange / 100.0) {
        DuelResult(false, g_context.localizer.Text("aggro.invalid_range", "Chase distance must be between 0 and 10000 m"));
        return;
    }
    std::vector<std::uint64_t> changed;
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.records.size(); ++i) {
            auto& record = g_context.records[i];
            if (record.pet || record.hp <= 0 || record.max_hp <= 0 ||
                std::find(op.members.begin(), op.members.end(), record.id) == op.members.end()) continue;
            record.aggro = op.aggro;
            changed.push_back(record.id);
        }
    }
    const bool applied = !changed.empty() && MaintainBattles(changed);
    DuelResult(applied, applied ? g_context.localizer.Text("aggro.applied", "Aggro settings applied") :
        g_context.localizer.Text("aggro.unavailable", "Aggro settings saved; some entities are unavailable"));
}

// ---------------------------------------------------------------- 队列

bool SummonBusyLocked() {
    const auto& batch = g_context.summon_batch;
    return batch && (batch->in_progress || (!batch->stopped && batch->completed < batch->settings.count));
}

void CancelSummonsLocked() {
    if (!SummonBusyLocked()) return;
    g_context.summon_batch->stopped = true;
    const auto end = std::remove_if(g_context.pending.begin(),
        g_context.pending.begin() + g_context.pending.size(),
        [](const PendingOp& op) { return op.kind == OpKind::Summon; });
    g_context.pending.erase(end, g_context.pending.end());
    g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending.size()), std::memory_order_release);
}

void AppendOpLocked(PendingOp op) {
    g_context.pending.push_back(std::move(op));
    g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending.size()), std::memory_order_release);
}

bool PushOp(const PendingOp& op) {
    std::scoped_lock lock(g_context.mutex);
    if (op.kind == OpKind::ClearAll) CancelSummonsLocked();
    AppendOpLocked(op);
    return true;
}

bool PushSelection(OpKind kind, const std::vector<std::uint64_t>& ids, bool enabled = false) {
    std::scoped_lock lock(g_context.mutex);
    for (const auto id : ids) AppendOpLocked(PendingOp{kind, id, enabled});
    return true;
}

bool PushSummon(std::uint32_t count) {
    std::scoped_lock lock(g_context.mutex);
    if (SummonBusyLocked()) return false;
    if (count == 0) return false;
    auto batch = std::make_shared<SummonBatch>();
    auto& settings = batch->settings;
    settings = CaptureSettingsLocked();
    if (settings.display_name.empty()) {
        const auto found = g_context.blueprint_options.find(settings.class_path);
        if (found != g_context.blueprint_options.end())
            settings.display_name = found->second.default_display_name;
    }
    settings.compatibility = settings.compatibility &&
        MonsterConfigName(settings.class_path).starts_with("FT_");
    if (settings.compatibility) settings.server_combat_blueprint = g_context.final_tower_combat_blueprint;
    settings.count = count;
    g_context.summon_batch = batch;
    g_context.last_result = {};
    g_context.last_result_failed = false;
    // Keep one continuation per batch, independent of its requested count.
    PendingOp op{OpKind::Summon};
    op.batch = batch;
    AppendOpLocked(std::move(op));
    return true;
}

// An explicitly supplied request is consumed once on plugin start. Queueing uses
// the same game-thread path as the summon button; no ProcessEvent runs here.
void ConsumeSpawnRequest() {
    // Core exposes the original package directory, not the disposable shadow.
    // Keep the request outside that package so it cannot be copied or reloaded.
    std::array<char, 4096> package{};
    std::size_t size = package.size();
    if (g_context.core == nullptr || g_context.core->plugin_directory == nullptr ||
        g_context.core->plugin_directory(g_context.core->user, package.data(), &size).code !=
            ANOMALY_STATUS_V1_OK) return;
    const auto path = std::filesystem::u8path(package.data()).parent_path() /
                      L"summon-lab-spawn-once.json";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    const auto request = nlohmann::json::parse(file, nullptr, false);
    file.close();
    if (request.is_discarded() || !request.is_object() ||
        !request.contains("classPath") || !request["classPath"].is_string() ||
        !request.contains("hp") || !request["hp"].is_number_unsigned() ||
        !request.contains("level") || !request["level"].is_number_unsigned() ||
        !request.contains("hostile") || !request["hostile"].is_boolean()) return;
    const auto class_path = request["classPath"].get<std::string>();
    const auto hp = request["hp"].get<std::uint64_t>();
    const auto level = request["level"].get<std::uint64_t>();
    if (class_path.empty() || class_path.size() >= g_context.class_path.size() ||
        hp > 10000000U || level > kMaxLevel ||
        !g_context.birth_attributes_ready.load(std::memory_order_acquire) ||
        !g_context.has_invoke) return;
    std::error_code error;
    if (!std::filesystem::remove(path, error) || error) return;
    {
        std::scoped_lock lock(g_context.mutex);
        std::snprintf(g_context.class_path.data(), g_context.class_path.size(),
                      "%s", class_path.c_str());
        g_context.combat_config_blueprint.clear();
        g_context.advanced_combat_source = 0;
        g_context.birth_presets[kMaxHpAttribute] = {hp != 0 ? 1 : 0, static_cast<double>(hp)};
        g_context.level = static_cast<std::uint32_t>(level);
        g_context.hostile = request["hostile"].get<bool>() ? 1 : 0;
        g_context.pet = 0;
        g_context.aggressive = 1;
        g_context.ignore_player = 0;
    }
    static_cast<void>(PushSummon(1));
}

void DrainOne() {
    PendingOp op{};
    {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.pending.size() == 0) {
            g_pending_hint.store(0, std::memory_order_release);
            return;
        }
        op = std::move(g_context.pending[0]);
        g_context.pending.pop_front();
        if (op.batch) op.batch->in_progress = true;
        g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending.size()),
                             std::memory_order_release);
    }

    switch (op.kind) {
    case OpKind::Summon: {
        auto result = SummonOne(*op.batch);
        if (result.ok) ExcludeNewSummon(result);
        if (g_context.core != nullptr) {
            g_context.core->log(g_context.core->user,
                result.ok ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView(result.detail));
        }
        {
            std::scoped_lock lock(g_context.mutex);
            if (result.ok) {
                {
                    auto& record = g_context.records.emplace_back();
                    record = {};
                    record.id = g_context.next_record_id++;
                    record.handle = result.handle;
                    record.pointer = result.pointer;
                    record.pet = result.pet;
                    record.mute_boss_music = op.batch->settings.mute_boss_music;
                    record.mute_boss_dialogue = op.batch->settings.mute_boss_dialogue;
                    if (op.batch->settings.skip_cutscenes)
                        g_cutscenes.sources[result.pointer] = result.handle;
                    record.hp = result.hp;
                    record.max_hp = result.max_hp;
                    record.registration = result.registration;
                    record.class_path = op.batch->settings.class_path;
                    record.server_combat_source = result.server_combat_source;
                    record.attributes = std::move(result.attributes);
                    record.editor = std::move(result.editor);
                    record.editor_warning = std::move(result.editor_warning);
                    std::snprintf(record.label, sizeof(record.label), "%s", result.label);
                    if (op.batch->completed == 0) {
                        g_context.selected_record_id = record.id;
                        g_context.management_blueprint = record.class_path;
                        const auto group_count = std::count_if(g_context.records.begin(),
                            g_context.records.begin() + g_context.records.size(),
                            [&](const auto& member) { return member.class_path == record.class_path; });
                        g_context.management_page = (group_count - 1) / kManagementPageSize;
                    }
                }
                ++op.batch->completed;
                g_context.last_result_failed = false;
            } else {
                g_context.last_result_failed = true;
                CancelSummonsLocked();
            }
            op.batch->in_progress = false;
            if (result.ok && !op.batch->stopped && op.batch->completed < op.batch->settings.count)
                AppendOpLocked(op);
            std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                          result.detail);
        }
        break;
    }
    case OpKind::Remove:
        RemoveOne(op.record_id);
        break;
    case OpKind::ClearAll:
        ClearAll();
        break;
    case OpKind::SetBossHpBar:
    case OpKind::SetCollision:
    case OpKind::SetHidden:
        SetActorOption(op);
        break;
    case OpKind::SetBattleEdge:
    case OpKind::StopDuel:
    case OpKind::SelectAllOpponents:
    case OpKind::SetGroupBattle:
    case OpKind::StopGroupBattle:
        ChangeBattle(op);
        break;
    case OpKind::SetAggroSettings:
        ApplyAggroSettings(op);
        break;
    }
}

// ---------------------------------------------------------------- 服务补查

void EnsureServices() {
    if (g_context.host == nullptr) return;
    if (g_context.objects != nullptr && g_context.session != nullptr &&
        g_context.player != nullptr && g_context.names != nullptr &&
        g_context.assets != nullptr)
        return;

    const anomaly::sdk::Host query(g_context.host);
    if (g_context.objects == nullptr) {
        g_context.objects = query.Query<AnomalyUe5ObjectsServiceV1>(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    }
    if (g_context.session == nullptr) {
        g_context.session = query.Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    }
    if (g_context.player == nullptr) {
        g_context.player = query.Query<AnomalyNtePlayerServiceV1>(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    }
    if (g_context.names == nullptr) {
        g_context.names = query.Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    }
    if (g_context.assets == nullptr) {
        g_context.assets = query.Query<AnomalyUe5AssetsServiceV1>(
            ANOMALY_UE5_ASSETS_SERVICE_V1_ID, ANOMALY_UE5_ASSETS_SERVICE_V1_VERSION).get();
    }

}

}  // namespace

// ---------------------------------------------------------------- ABI

namespace {

void ResetState() {
    ResetCutscenes();
    g_context.preset_picker.Stop();
    static_cast<void>(g_context.preset_picker.TakeResult());
    {
        std::scoped_lock lock(g_local_birth_mutex);
        g_local_births.clear();
        g_birth_configurations.clear();
    }
    g_birth_configuration_ready.store(false, std::memory_order_release);
    g_restore_local_births.store(false, std::memory_order_release);
    g_observe_ft_health.store(false, std::memory_order_release);
    g_pending_local_births.clear();
    std::scoped_lock lock(g_context.mutex);
    g_context.records = {};
    g_context.next_record_id = 1;
    g_context.pending = {};
    g_context.summon_batch.reset();
    g_context.session_world_ready = false;
    g_context.session_event_cursor = 0;
    g_context.frame_counter = 0;

    g_context.class_path = {};
    g_context.combat_config_blueprint.clear();
    g_context.advanced_combat_blueprint.clear();
    g_context.advanced_combat_source = 0;
    g_context.combat_source_filter = {};
    g_context.combat_source_page = 0;
    g_context.advanced_config = {};
    g_context.blueprint_config = {};
    g_context.blueprint_check_delay = 0;
    g_context.display_name = {};
    std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s",
                  kDefaultClassPath);
    g_context.level = 0;
    g_context.skill_level = 0;
    g_context.monster_level_query = 0;
    g_layout = {};
    g_layout_ready = false;
    g_profile_scans.Clear();
    g_context.mirror_getter_entry.clear();
    g_context.final_tower_scene_asset.clear();
    g_context.final_tower_aliases.clear();
    g_context.final_tower_combat_blueprint.clear();
    g_context.mirror_record_size = 0;
    g_context.camp_count = 0;
    g_context.current_chase_range_offset = 0;
    g_context.available_birth_attributes = 0;
    g_context.birth_presets = {};
    g_context.attribute_filter = {};
    g_context.attribute_help = kBirthAttributeNames.size();
    g_context.only_overrides = 0;
    g_context.catalog_page = 0;
    g_context.hostile = 0;
    g_context.pet = 0;
    g_context.aggressive = 0;
    g_context.ignore_player = 0;
    g_context.mute_boss_music = 0;
    g_context.mute_boss_dialogue = 0;
    g_context.skip_cutscenes = 0;
    g_context.ft_compatibility = 0;
    g_context.summon_count = 1;
    g_context.summon_spacing = 5.0;
    g_context.config = nullptr;
    g_context.config_schema = {};
    g_context.config_task = {};
    g_context.preset_transfer_task = {};
    g_context.config_writable = false;
    g_context.config_dirty = false;
    g_context.config_stopping = false;
    g_context.config_revision = 0;
    g_context.config_error.clear();
    g_context.preset_message.clear();
    g_context.saved_presets.clear();
    g_context.selected_preset = summon_lab::kNoPreset;
    g_context.new_preset_name = {};
    g_context.preset_rename = {};
    g_context.preset_filter = {};
    g_context.preset_page = 0;
    std::snprintf(g_context.preset_export_name.data(), g_context.preset_export_name.size(), "%s",
        summon_lab::kDefaultPresetFileName.data());
    g_context.preset_export_folder.clear();
    g_context.preset_import_file.clear();
    g_context.presets_current_blueprint = 1;
    g_context.preset_transfer.reset();
    g_context.last_result = {};
    g_context.last_result_failed = false;
    g_context.editor.blueprints.clear();
    g_context.editor.snapshot = {};
    g_context.editor.requested_path.clear();
    g_context.editor.load_requested = false;
    g_context.editor.field_filter = {};
    g_context.editor.skill_filter = {};
    g_context.editor.effect_filter = {};
    g_context.editor.field_page = g_context.editor.skill_page = g_context.editor.effect_page = 0;
    g_context.editor.customized_only = g_context.editor.skills_selected_only = g_context.editor.effects_selected_only = 0;
    g_context.editor.catalog.store({}, std::memory_order_release);
    g_context.editor.catalog_requested.store(false, std::memory_order_release);
    g_context.editor.catalog_busy.store(false, std::memory_order_release);
    g_context.editor.catalog_task = {};
    g_context.editor.birth_observer = {};
    g_context.editor.scan = {};
    g_context.editor.record_cursor = 0;
    g_context.catalog.clear();
    g_context.blueprint_options.clear();
    g_context.catalog_filter[0] = '\0';
    g_context.catalog_loaded = false;
    g_context.catalog_labels.store({}, std::memory_order_release);
    g_context.catalog_base.clear();
    g_context.catalog_name_table.clear();
    g_context.catalog_policy.clear();
    g_context.catalog_exclusions.clear();
    g_context.catalog_name_aliases.clear();
    g_context.catalog_stop.store(false, std::memory_order_release);
    g_context.catalog_busy.store(false, std::memory_order_release);
    g_context.catalog_refresh_requested.store(true, std::memory_order_release);
    g_context.catalog_preparing = false;
    g_context.catalog_task = {};
    g_context.catalog_scan_status = {};
    g_context.selected_record_id = 0;
    g_context.management_page = 0;
    g_context.management_blueprint.clear();
    g_context.battles = {};
    g_context.show_aggro_editor = false;
    g_context.aggro_edit_members.clear();
    g_context.aggro_edit = {};
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "idle");
    g_pending_hint.store(0, std::memory_order_release);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const anomaly::sdk::Host query(host);
    const auto ui = query.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                                    ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr)
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};

    g_context.host = host;
    g_context.ui = ui.get();
    g_context.localizer = anomaly::sdk::Localizer(host);
    g_context.attribute_descriptions = summon_lab::LoadAttributeDescriptions(g_context.localizer);
    ResetState();
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    ResetState();

    const anomaly::sdk::Host query(g_context.host);
    g_context.core = query.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                                       ANOMALY_CORE_SERVICE_V1_VERSION).get();
    g_context.objects = query.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    g_context.session = query.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    g_context.player = query.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    g_context.names = query.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    g_context.assets = query.Query<AnomalyUe5AssetsServiceV1>(
        ANOMALY_UE5_ASSETS_SERVICE_V1_ID, ANOMALY_UE5_ASSETS_SERVICE_V1_VERSION).get();
    g_context.scheduler = query.Query<AnomalySchedulerServiceV1>(
        ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
    g_context.storage = query.Query<AnomalyStorageServiceV1>(
        ANOMALY_STORAGE_SERVICE_V1_ID, ANOMALY_STORAGE_SERVICE_V1_VERSION).get();
    InitializeConfiguration(query);

    TransferLocalBirths(false);

    const auto events = query.Query<AnomalyUe5ProcessEventServiceV1>(
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION);
    g_context.events = events.get();
    g_context.has_invoke = HasInvoke(g_context.events);

    const auto loaded = anomaly::sdk::LoadProfile(query, "compatibility.profile.json",
        {"nte", "HTGame.exe", ".text", "summon-lab-v2"});
    if (loaded && ValidateSummonProfile(loaded.value(), &g_layout)) {
        const auto& profile = loaded.value();
        constexpr std::array<std::string_view, 15> symbols{
            "story-boss-music", "story-multiple-voices", "story-debug-voices", "story-single-voice",
            "duel-current-chase-range", "monster-scene-row", "monster-property-pack", "final-tower-property-pack",
            "object-item-set-flags", "object-array-reference", "attribute-base-input", "summon-registration",
            "local-attribute-mirror", "native-damage-report", "monster-default-level"};
        const auto scan_started = std::chrono::steady_clock::now();
        g_profile_scans.Prepare(query, profile, symbols);
        const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scan_started).count();
        if (g_context.core && g_context.core->log) {
            char message[160]{};
            std::snprintf(message, sizeof(message), "SummonLab: signatures mode=%s patterns=%zu elapsed_ms=%lld",
                g_profile_scans.batched() ? "v3-batch" : "v1", g_profile_scans.size(), static_cast<long long>(scan_ms));
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
        }
        g_layout_ready = true;
        g_cutscenes.profile = profile;
        g_cutscenes.pending = true;
        StartBlueprintEditor();
        g_context.monster_level_query = ResolveMonsterLevelQuery(profile);
        g_context.current_chase_range_offset = ResolveDuelChaseRange(profile);
        const bool attributes_ready = InstallBirthAttributeHook(query, profile);
        const bool combat_ready = attributes_ready && InstallLocalCombatHooks(profile);
        g_context.birth_attributes_ready.store(attributes_ready && combat_ready, std::memory_order_release);
        if (combat_ready && !InstallFinalTowerHooks(profile) && g_context.core != nullptr)
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: FinalTower config bridge unavailable"));
        if (combat_ready) {
            const bool audio_ready = InstallStoryAudioHooks(profile);
            g_context.core->log(g_context.core->user, audio_ready ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
                ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(audio_ready ?
                    "SummonLab: instance boss music and dialogue control ready" :
                    "SummonLab: instance boss music and dialogue control unavailable"));
            StartFinalTowerHealthObserver();
            if (!InstallDamageReportObserver(profile))
                g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    anomaly::sdk::StringView("SummonLab: native damage observer unavailable"));
        }
        g_profile_scans.Clear();
    } else if (g_context.core != nullptr && g_context.core->log != nullptr) {
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView(loaded ? "Unsupported SummonLab profile contract or layout" : loaded.error()));
    }
    try {
        if (LoadCatalogPolicy()) LoadCatalogCache();
    } catch (...) { SetScanStatus("catalog.scan.failed", "Catalog refresh failed"); }
    ConsumeSpawnRequest();

    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    g_context.preset_picker.Stop();
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.config_stopping = true;
        ConsumePresetPickerLocked();
    }
    // The host drains Worker callbacks before on_stop and retains config schemas
    // through it, so the latest pending edit can be flushed on Lifecycle.
    if (g_context.scheduler && g_context.config_task.id)
        static_cast<void>(g_context.scheduler->cancel(g_context.scheduler->user, g_context.config_task));
    if (g_context.scheduler && g_context.preset_transfer_task.id)
        static_cast<void>(g_context.scheduler->cancel(g_context.scheduler->user, g_context.preset_transfer_task));
    if (!SaveConfiguration() && g_context.core && g_context.core->log)
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView("SummonLab: configuration flush failed"));
    g_context.catalog_stop.store(true, std::memory_order_release);
    if (g_context.scheduler && g_context.catalog_task.id)
        static_cast<void>(g_context.scheduler->cancel(g_context.scheduler->user, g_context.catalog_task));
    if (g_context.scheduler && g_context.editor.catalog_task.id)
        static_cast<void>(g_context.scheduler->cancel(g_context.scheduler->user, g_context.editor.catalog_task));
    if (g_context.events && g_context.editor.birth_observer.id) {
        const auto status = g_context.events->unsubscribe(g_context.events->user, g_context.editor.birth_observer);
        if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
    }
    g_context.editor.birth_observer = {};
    if (g_context.events != nullptr && g_context.health_observer.id != 0) {
        const auto status = g_context.events->unsubscribe(g_context.events->user, g_context.health_observer);
        if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
    }
    g_context.health_observer = {};
    g_context.health_events = {};
    g_context.health_event_lookup_pending = false;
    g_context.events = nullptr;
    g_context.monster_level_query = 0;
    g_context.birth_attributes_ready.store(false, std::memory_order_release);
    const auto cutscene_status = ReleaseCutsceneHooks();
    if (cutscene_status.code != ANOMALY_STATUS_V1_OK) return cutscene_status;
    const auto audio_status = ReleaseStoryAudioHooks();
    if (audio_status.code != ANOMALY_STATUS_V1_OK) return audio_status;
    const auto local_status = ReleaseLocalCombatHooks();
    if (local_status.code != ANOMALY_STATUS_V1_OK) return local_status;
    ReleaseFinalTowerData();
    if (g_context.hooks != nullptr && g_context.attribute_hook.id != 0) {
        const auto status = g_context.hooks->release(g_context.hooks->user, g_context.attribute_hook);
        // PluginScope may revoke and drain the hook before on_stop runs.
        // NOT_FOUND means that cleanup is already complete, not an unload fault.
        if (status.code != ANOMALY_STATUS_V1_OK &&
            status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
    }
    g_context.attribute_hook = {};
    g_context.attribute_original = 0;
    g_context.hooks = nullptr;
    TransferLocalBirths(true);
    ResetState();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    if (context != &g_context) return;
    ResetState();
    g_context.host = nullptr;
    g_context.ui = nullptr;
    g_context.core = nullptr;
    g_context.objects = nullptr;
    g_context.session = nullptr;
    g_context.player = nullptr;
    g_context.names = nullptr;
    g_context.has_invoke = false;
}

void ANOMALY_CALL Update(void* /*context*/, double delta_seconds) {
    ++g_context.frame_counter;
    EnsureServices();
    UpdateSessionState();
    if (g_context.session_world_ready) AdvanceCatalogPreparation();
    if (!g_context.session_world_ready || !g_layout_ready) return;
    UpdateCutscenes();
    ResolveFinalTowerHealthEvents();
    // The host runs on_update at its Game tick boundary. Do not invoke spawn
    // or configuration from arbitrary ProcessEvent callbacks: those may be
    // nested inside UI scripts or a native attribute mirror's deserialization.
    RestoreLocalBirthsOnGameThread();
    ConfigureNativeBirthsOnGameThread();
    UpdateBlueprintConfig(delta_seconds);
    UpdateAdvancedConfig();
    UpdateEditorFields();
    AdvanceEditorCatalog();
    MaintainBlueprintEdits();
    bool spawning{};
    {
        std::scoped_lock lock(g_context.mutex);
        spawning = g_context.pending.size() != 0 && g_context.pending[0].kind == OpKind::Summon;
    }
    const bool refresh = (g_context.frame_counter % kHealthRefreshInterval) == 0;
    // Retire dead registrations before a spawn can reuse their native slots.
    // Battle maintenance must also keep running during a long summon batch.
    if (spawning || refresh) RefreshHealth(refresh);
    if (refresh) MaintainBattles();
    const auto work_started = std::chrono::steady_clock::now();
    while (g_pending_hint.load(std::memory_order_relaxed) != 0) {
        DrainOne();
        // A summon is indivisible; check the frame budget between native calls.
        // Finalize registration before starting the next member of the batch.
        RestoreLocalBirthsOnGameThread();
        ConfigureNativeBirthsOnGameThread();
        if (std::chrono::steady_clock::now() - work_started >= std::chrono::milliseconds(4)) break;
    }

}

// width 传 -1 表示填满当前可用宽度，避免列表按钮只占住左侧一小条。
bool ActionButton(const AnomalyUiServiceV1* ui, const char* label, bool enabled,
                  float width = 0.0F) {
    if (ui == nullptr) return false;
    if (ui->button_enabled != nullptr)
        return ui->button_enabled(ui->user, anomaly::sdk::StringView(label), width, 0.0F,
                                  enabled ? 1 : 0) != 0;
    return enabled && ui->button != nullptr &&
           ui->button(ui->user, anomaly::sdk::StringView(label), width, 0.0F) != 0;
}

void WrappedText(const AnomalyUiServiceV1* ui, std::string_view text) {
    if (ui->struct_size >= offsetof(AnomalyUiServiceV1, text_wrapped) + sizeof(ui->text_wrapped) &&
        ui->text_wrapped != nullptr)
        ui->text_wrapped(ui->user, anomaly::sdk::StringView(text));
    else
        ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawBlueprintConfig(const AnomalyUiServiceV1* ui, bool select_source) {
    BlueprintConfigState state;
    std::string source;
    std::string normal_combat_blueprint;
    std::string selected_source;
    std::vector<CombatConfigChoice> sources;
    {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.class_path[0] == '\0') return;
        const auto& check = g_context.blueprint_config;
        state = !g_context.session_world_ready ? BlueprintConfigState::Unavailable :
            check.path != g_context.class_path.data() ||
            check.requested_source != g_context.combat_config_blueprint
                ? BlueprintConfigState::Pending : check.state;
        if (check.path == g_context.class_path.data()) {
            sources = check.sources;
            selected_source = g_context.combat_config_blueprint;
            if (check.requested_source == selected_source) source = check.source;
        }
        const auto entry = g_context.blueprint_options.find(g_context.class_path.data());
        if (entry != g_context.blueprint_options.end())
            normal_combat_blueprint = entry->second.normal_combat_blueprint;
    }
    const auto& localizer = g_context.localizer;
    std::string text;
    switch (state) {
    case BlueprintConfigState::Pending:
        text = localizer.Text("catalog.config_pending", "Checking combat config...");
        break;
    case BlueprintConfigState::Unavailable:
        text = localizer.Text("catalog.config_unavailable", "Combat config check unavailable");
        break;
    case BlueprintConfigState::NotApplicable:
        text = localizer.Text("catalog.config_not_applicable", "Combat config check is not supported for this blueprint");
        break;
    case BlueprintConfigState::NoHealth:
        text = sources.empty()
            ? localizer.Text("catalog.config_no_health", "No valid HP config found in the current scene")
            : localizer.Text("catalog.config_choose", "No default HP; a variant's combat config can be selected");
        break;
    case BlueprintConfigState::Available:
        text = localizer.Text("catalog.config_available", "Combat config found");
        break;
    }
    WrappedText(ui, text);
    if (!source.empty()) {
        const std::array<std::string_view, 1> args{source};
        WrappedText(ui, localizer.Format("catalog.config_source", "Combat config: {0}", args));
    }
    if (select_source && (!sources.empty() || !selected_source.empty()) &&
        ui->begin_menu != nullptr && ui->end_menu != nullptr) {
        const auto label = localizer.Text("catalog.config_select", "Select source");
        if (ui->begin_menu(ui->user, anomaly::sdk::StringView(label), 1)) {
            const auto native = localizer.Text("catalog.config_native", "Blueprint defaults");
            if (ActionButton(ui, native.c_str(), !selected_source.empty())) {
                std::scoped_lock lock(g_context.mutex);
                g_context.combat_config_blueprint.clear();
                ConfigurationChangedLocked();
            }
            for (const auto& choice : sources) {
                const auto name = MonsterConfigName(choice.path);
                char hp[48]{};
                std::snprintf(hp, sizeof(hp), "%.9g", choice.hp);
                const std::array<std::string_view, 2> args{name, hp};
                const auto item = localizer.Format("catalog.config_choice", "{0} | HP {1}", args) +
                    "##combat-source-" + choice.path;
                if (ActionButton(ui, item.c_str(), choice.path != selected_source)) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.combat_config_blueprint = choice.path;
                    ConfigurationChangedLocked();
                }
            }
            ui->end_menu(ui->user);
        }
    }
    if (!normal_combat_blueprint.empty()) {
        WrappedText(ui, localizer.Text("catalog.story_balance",
            "Story balance can ignore monster damage"));
        const std::string name = MonsterConfigName(normal_combat_blueprint);
        const std::array<std::string_view, 1> args{name};
        const auto label = localizer.Format("catalog.normal_combat_variant", "Use combat variant: {0}", args);
        if (ActionButton(ui, label.c_str(), true)) {
            std::scoped_lock lock(g_context.mutex);
            std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s",
                normal_combat_blueprint.c_str());
            g_context.combat_config_blueprint.clear();
            ConfigurationChangedLocked();
        }
    }
    if (state == BlueprintConfigState::Unavailable) {
        const auto retry = localizer.Text("catalog.config_retry", "Recheck");
        if (ActionButton(ui, retry.c_str(), true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.blueprint_config = {};
        }
    }
}

void DrawBlueprintTab(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    char line[kTextCapacity];
    bool loaded{};
    std::size_t catalog_total{};
    {
        std::scoped_lock lock(g_context.mutex);
        loaded = g_context.catalog_loaded;
        catalog_total = g_context.catalog.size();
    }
    // ================= 目标 =================
    if (ui->input_text != nullptr) {
        const std::string label = localizer.Text("target.title", "Class path");
        std::scoped_lock lock(g_context.mutex);
        if (ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.class_path.data(),
                       g_context.class_path.size(), 0)) {
            g_context.combat_config_blueprint.clear();
            ConfigurationChangedLocked();
        }
    }

    const auto rescan_label = localizer.Text("catalog.scan", "Refresh catalog");
    if (ActionButton(ui, rescan_label.c_str(), !g_context.catalog_busy.load(std::memory_order_acquire)))
        g_context.catalog_refresh_requested.store(true, std::memory_order_release);
    std::array<char, 160> scan_status;
    {
        std::scoped_lock lock(g_context.mutex);
        scan_status = g_context.catalog_scan_status;
    }
    if (scan_status[0]) {
        if (ui->same_line) ui->same_line(ui->user, 0, 10);
        ui->text(ui->user, anomaly::sdk::StringView(scan_status.data()));
    }

    if (!loaded) {
        const std::string missing = localizer.Text("catalog.missing", "catalog not loaded");
        ui->text(ui->user, anomaly::sdk::StringView(missing));
    } else {
        // 过滤 + 匹配统计放在同一行
        if (ui->input_text != nullptr) {
            const std::string filter_label = localizer.Text("catalog.filter", "Search");
            {
                std::scoped_lock lock(g_context.mutex);
                if (ui->input_text(ui->user, anomaly::sdk::StringView(filter_label),
                                   g_context.catalog_filter, sizeof(g_context.catalog_filter), 0))
                {
                    g_context.catalog_page = 0;
                    ConfigurationChangedLocked();
                }
            }
        }

        constexpr std::size_t kPageSize = 100;
        std::vector<ClassEntry> visible;
        std::size_t matched{};
        std::size_t page{};
        std::size_t page_count{};
        {
            std::scoped_lock lock(g_context.mutex);
            // 先统计匹配总数，再按页截取，页码越界时钳回最后一页
            for (const auto& entry : g_context.catalog) {
                bool keep = true;
                if (g_context.catalog_filter[0] != '\0') {
                    keep = ui->filter_match != nullptr
                               ? ui->filter_match(ui->user,
                                                  anomaly::sdk::StringView(g_context.catalog_filter),
                                                  anomaly::sdk::StringView(entry.search_text)) != 0
                               : entry.search_text.find(g_context.catalog_filter) != std::string::npos;
                }
                if (keep) ++matched;
            }
            page_count = matched == 0 ? 1 : (matched + kPageSize - 1) / kPageSize;
            page = g_context.catalog_page < page_count ? g_context.catalog_page : page_count - 1;
            std::size_t seen{};
            for (const auto& entry : g_context.catalog) {
                bool keep = true;
                if (g_context.catalog_filter[0] != '\0') {
                    keep = ui->filter_match != nullptr
                               ? ui->filter_match(ui->user,
                                                  anomaly::sdk::StringView(g_context.catalog_filter),
                                                  anomaly::sdk::StringView(entry.search_text)) != 0
                               : entry.search_text.find(g_context.catalog_filter) != std::string::npos;
                }
                if (!keep) continue;
                if (seen >= page * kPageSize) {
                    if (visible.size() < kPageSize) visible.push_back(entry);
                    else break;
                }
                ++seen;
            }
        }

        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
        const std::string total_arg = std::to_string(catalog_total);
        const std::string matched_arg = std::to_string(matched);
        const std::array<std::string_view, 2> args{matched_arg, total_arg};
        const std::string stats = localizer.Format("catalog.stats", "{0} / {1}", args);
        std::snprintf(line, sizeof(line), "%s", stats.c_str());
        ui->text(ui->user, anomaly::sdk::StringView(line));

        // 分页：每页 kPageSize 条，页码在过滤变化时自动钳回有效范围
        {
            const std::string prev_label = localizer.Text("page.prev", "<");
            if (ActionButton(ui, prev_label.c_str(), page > 0)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.catalog_page = page > 0 ? page - 1 : 0;
            }
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
            const std::string page_arg = std::to_string(page + 1);
            const std::string pages_arg = std::to_string(page_count);
            const std::array<std::string_view, 2> page_args{page_arg, pages_arg};
            const std::string page_text = localizer.Format("page.indicator", "{0} / {1}", page_args);
            std::snprintf(line, sizeof(line), "%s", page_text.c_str());
            ui->text(ui->user, anomaly::sdk::StringView(line));
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
            const std::string next_label = localizer.Text("page.next", ">");
            if (ActionButton(ui, next_label.c_str(), page + 1 < page_count)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.catalog_page = page + 1 < page_count ? page + 1 : page;
            }
        }

        // 列表：按钮填满整行，选中项的完整路径在下面单独一行展示
        const bool has_child = ui->begin_child != nullptr && ui->end_child != nullptr;
        bool child_open = true;
        if (has_child) {
            // 列表用固定高度、内部滚动；窗口高度交给宿主按内容贴合。
            // 之前用「窗口高 - 底部估算」会因字体/DPI 算不准，内容一超出窗口
            // 宿主就给它加外层滚动条，越拉越难看。固定高度不会撑破窗口。
            constexpr float kListHeight = 360.0F;
            child_open = ui->begin_child(ui->user, anomaly::sdk::StringView("##catalog"), 0.0F,
                                         kListHeight, 0) != 0;
        }
        if (child_open) {
            for (const auto& entry : visible) {
                const std::string label = entry.label + "###blueprint-" + entry.path;
                if (ActionButton(ui, label.c_str(), true, -1.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s",
                                  entry.path.c_str());
                    g_context.combat_config_blueprint.clear();
                    ConfigurationChangedLocked();
                    g_context.last_result_failed = false;
                    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                                  entry.path.c_str());
                }
            }
        }
        if (has_child) ui->end_child(ui->user);
    }
    DrawBlueprintConfig(ui, false);
}

void DrawSettingsTab(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    if (ui->input_uint32 != nullptr) {
        const auto level_label = localizer.Text("param.level", "Level (0 = default)");
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(level_label), &g_context.level, 0, 0))
            ConfigurationChangedLocked();
        g_context.level = std::min(g_context.level, kMaxLevel);
        const auto skill_label = localizer.Text("param.skill_level", "Skill level (0 = default)");
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(skill_label), &g_context.skill_level, 0, 0))
            ConfigurationChangedLocked();
        g_context.skill_level = std::min(g_context.skill_level, kMaxLevel);
    }
    WrappedText(ui, localizer.Text("settings.hint",
        "Checked fields override defaults for the next summon"));
    if (ui->separator != nullptr) ui->separator(ui->user);
    if (ui->input_text != nullptr) {
        const auto label = localizer.Text("settings.filter", "Find attribute");
        if (ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.attribute_filter.data(),
                       g_context.attribute_filter.size(), 0)) ConfigurationChangedLocked();
    }
    if (ui->checkbox != nullptr) {
        const auto label = localizer.Text("settings.only_overrides", "Only customized");
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &g_context.only_overrides))
            ConfigurationChangedLocked();
    }
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
    const auto reset = localizer.Text("settings.reset", "Reset all attributes");
    if (ActionButton(ui, reset.c_str(), true)) {
        g_context.level = 0;
        g_context.skill_level = 0;
        g_context.birth_presets = {};
        ConfigurationChangedLocked();
    }

    const auto enabled_count = std::count_if(g_context.birth_presets.begin(), g_context.birth_presets.end(),
        [](const BirthPreset& preset) { return preset.enabled != 0; });
    const std::string total = std::to_string(g_context.available_birth_attributes);
    const std::string enabled = std::to_string(enabled_count);
    const std::array<std::string_view, 2> args{total, enabled};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
        "settings.count", "{0} attributes / {1} customized", args)));

    const bool has_child = ui->begin_child != nullptr && ui->end_child != nullptr;
    const bool visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##birth-attributes"), 0.0F, 360.0F, 0) != 0;
    if (visible) {
        const bool has_table = ui->begin_table != nullptr && ui->table_next_row != nullptr &&
            ui->table_next_column != nullptr && ui->end_table != nullptr;
        const bool table = has_table && ui->begin_table(ui->user,
            anomaly::sdk::StringView("##birth-attribute-table"), 2,
            ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT, 0.0F, 0.0F) != 0;
        std::size_t matched{};
        for (std::size_t i = 0; i < g_context.available_birth_attributes; ++i) {
            auto& preset = g_context.birth_presets[i];
            const auto name = kBirthAttributeNames[i];
            const auto& description = g_context.attribute_descriptions[i];
            if (g_context.only_overrides && !preset.enabled) continue;
            if (g_context.attribute_filter[0] != '\0') {
                const bool match = ui->filter_match != nullptr
                    ? ui->filter_match(ui->user, anomaly::sdk::StringView(g_context.attribute_filter.data()),
                                       anomaly::sdk::StringView(description.search_text)) != 0
                    : description.search_text.find(g_context.attribute_filter.data()) != std::string::npos;
                if (!match) continue;
            }
            ++matched;
            if (table) {
                ui->table_next_row(ui->user);
                ui->table_next_column(ui->user);
            }
            const auto help_id = localizer.Label("settings.help", "?", "birth-help-" + std::string(name));
            if (ActionButton(ui, help_id.c_str(), true))
                g_context.attribute_help = g_context.attribute_help == i ? kBirthAttributeNames.size() : i;
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 6.0F);
            // The localized label never changes the reflected field or preset identity.
            const std::string label = description.label + "###birth-enabled-" + std::string(name);
            if (ui->checkbox != nullptr) {
                if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &preset.enabled))
                    ConfigurationChangedLocked();
            }
            else
                ui->text(ui->user, anomaly::sdk::StringView(description.label));
            if (table) ui->table_next_column(ui->user);
            if (preset.enabled && ui->input_double != nullptr) {
                if (ui->struct_size >= offsetof(AnomalyUiServiceV1, set_next_item_width) +
                        sizeof(ui->set_next_item_width) && ui->set_next_item_width != nullptr)
                    ui->set_next_item_width(ui->user, 160.0F);
                const std::string value_id = "##birth-value-" + std::string(name);
                if (ui->input_double(ui->user, anomaly::sdk::StringView(value_id), &preset.value, 0.0, 0.0))
                    ConfigurationChangedLocked();
            } else {
                ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("settings.default", "Default")));
            }
        }
        if (table) ui->end_table(ui->user);
        if (matched == 0)
            ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("settings.no_match", "No matching attributes")));
    }
    if (has_child) ui->end_child(ui->user);
    if (g_context.attribute_help < g_context.available_birth_attributes) {
        const auto& help = g_context.attribute_descriptions[g_context.attribute_help];
        WrappedText(ui, help.label);
        WrappedText(ui, help.description);
    }
    const auto error = BirthPresetError(g_context.birth_presets);
    if (!error.empty()) WrappedText(ui, error);
}

void DrawPresetSaveArea(const AnomalyUiServiceV1* ui);
std::string BlueprintLabel(std::string_view path);

#include "blueprint_editor_ui.hpp"

bool DrawAdvancedSummonOptions(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    if (ui->checkbox) {
        const auto enabled = localizer.Text("summon.advanced_source", "Use any blueprint's combat attributes");
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(enabled), &g_context.advanced_combat_source)) {
            if (g_context.advanced_combat_source && g_context.advanced_combat_blueprint.empty())
                g_context.advanced_combat_blueprint = g_context.combat_config_blueprint.empty()
                    ? g_context.class_path.data() : g_context.combat_config_blueprint;
            ConfigurationChangedLocked();
        }
    }
    if (!g_context.advanced_combat_source) return false;

    const auto source = g_context.advanced_combat_blueprint.empty()
        ? localizer.Text("catalog.config_native", "Blueprint defaults")
        : BlueprintLabel(g_context.advanced_combat_blueprint);
    const std::array<std::string_view, 1> source_args{source};
    WrappedText(ui, localizer.Format("summon.advanced_selected", "Attribute source: {0}", source_args));
    if (ui->begin_menu && ui->end_menu) {
        const auto select = localizer.Label("catalog.config_select", "Select source", "advanced-source");
        if (ui->begin_menu(ui->user, anomaly::sdk::StringView(select), 1)) {
            if (ui->input_text) {
                const auto search = localizer.Label("catalog.filter", "Search", "combat-source-filter");
                if (ui->input_text(ui->user, anomaly::sdk::StringView(search), g_context.combat_source_filter.data(),
                        g_context.combat_source_filter.size(), 0)) g_context.combat_source_page = 0;
            }
            std::vector<const ClassEntry*> matches;
            for (const auto& entry : g_context.catalog) {
                const auto filter = g_context.combat_source_filter.data();
                if (*filter && !(ui->filter_match
                    ? ui->filter_match(ui->user, anomaly::sdk::StringView(filter), anomaly::sdk::StringView(entry.search_text)) != 0
                    : entry.search_text.find(filter) != std::string::npos)) continue;
                matches.push_back(&entry);
            }
            constexpr std::size_t page_size = 10;
            const auto pages = std::max<std::size_t>(1, (matches.size() + page_size - 1) / page_size);
            auto& page = g_context.combat_source_page;
            page = std::min(page, pages - 1);
            const auto previous = localizer.Label("page.prev", "<", "combat-source-prev");
            if (ActionButton(ui, previous.c_str(), page > 0)) --page;
            if (ui->same_line) ui->same_line(ui->user, 0, 10);
            const auto number = std::to_string(page + 1), count = std::to_string(pages);
            const std::array<std::string_view, 2> page_args{number, count};
            ui->text(ui->user, anomaly::sdk::StringView(localizer.Format("page.indicator", "{0} / {1}", page_args)));
            if (ui->same_line) ui->same_line(ui->user, 0, 10);
            const auto next = localizer.Label("page.next", ">", "combat-source-next");
            if (ActionButton(ui, next.c_str(), page + 1 < pages)) ++page;
            const auto end = std::min(matches.size(), (page + 1) * page_size);
            for (auto i = page * page_size; i < end; ++i) {
                const auto& entry = *matches[i];
                const auto label = entry.label + "###advanced-combat-source-" + entry.path;
                if (ActionButton(ui, label.c_str(), entry.path != g_context.advanced_combat_blueprint)) {
                    g_context.advanced_combat_blueprint = entry.path;
                    ConfigurationChangedLocked();
                    if (ui->close_current_popup) ui->close_current_popup(ui->user);
                }
            }
            if (matches.empty()) WrappedText(ui, localizer.Text("catalog.no_match", "No matching blueprints"));
            ui->end_menu(ui->user);
        }
    }
    if (g_context.advanced_combat_blueprint.empty()) return true;
    const auto& check = g_context.advanced_config;
    const auto state = check.path == g_context.advanced_combat_blueprint
        ? check.state : BlueprintConfigState::Pending;
    if (state == BlueprintConfigState::Available) {
        char hp[48]{};
        std::snprintf(hp, sizeof(hp), "%.9g", check.hp);
        const std::array<std::string_view, 1> args{hp};
        WrappedText(ui, localizer.Format("summon.source_hp", "Source default HP: {0}", args));
    } else if (state == BlueprintConfigState::Pending) {
        WrappedText(ui, localizer.Text("catalog.config_pending", "Checking combat config..."));
    } else if (state == BlueprintConfigState::NoHealth || state == BlueprintConfigState::NotApplicable) {
        WrappedText(ui, localizer.Text("catalog.config_no_health", "No valid HP config found in the current scene"));
    } else {
        WrappedText(ui, localizer.Text("catalog.config_unavailable", "Combat config check unavailable"));
        const auto retry = localizer.Label("catalog.config_retry", "Recheck", "advanced-source-retry");
        if (ActionButton(ui, retry.c_str(), true)) g_context.advanced_config = {};
    }
    return true;
}

void DrawSummonTab(const AnomalyUiServiceV1* ui, bool ready) {
    const auto& localizer = g_context.localizer;
    bool can_summon{};
    std::uint32_t summon_count{};
    {
        std::scoped_lock lock(g_context.mutex);
        const std::string_view path(g_context.class_path.data());
        const auto separator = path.find_last_of("/.");
        const auto name = separator == std::string_view::npos ? path : path.substr(separator + 1);
        const std::array<std::string_view, 1> target_args{name};
        WrappedText(ui, localizer.Format("summon.target", "Blueprint: {0}", target_args));
        if (ui->input_text != nullptr) {
            const auto label = localizer.Text("param.name", "Name (blank = default)");
            if (ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.display_name.data(),
                           g_context.display_name.size(), 0)) ConfigurationChangedLocked();
        }
        if (ui->input_uint32 != nullptr) {
            const auto label = localizer.Text("summon.count", "Count");
            if (ui->input_uint32(ui->user, anomaly::sdk::StringView(label), &g_context.summon_count, 0, 0))
                ConfigurationChangedLocked();
        }
        summon_count = g_context.summon_count;
        if (summon_count > 1 && ui->input_double != nullptr) {
            const auto label = localizer.Text("summon.spacing", "Spacing (m)");
            if (ui->input_double(ui->user, anomaly::sdk::StringView(label), &g_context.summon_spacing, 0, 0))
                ConfigurationChangedLocked();
        }
        const bool spacing_valid = std::isfinite(g_context.summon_spacing) &&
            g_context.summon_spacing >= 0.0 && g_context.summon_spacing <= 100.0;
        if (summon_count > 1 && !spacing_valid)
            WrappedText(ui, localizer.Text("summon.invalid_spacing", "Spacing must be between 0 and 100 m"));
        const std::string level = g_context.level == 0 ? localizer.Text("settings.default", "Default") :
            std::to_string(g_context.level);
        const std::string skill = g_context.skill_level == 0 ? localizer.Text("settings.default", "Default") :
            std::to_string(g_context.skill_level);
        const std::string count = std::to_string(std::count_if(
            g_context.birth_presets.begin(), g_context.birth_presets.end(),
            [](const BirthPreset& preset) { return preset.enabled != 0; }));
        const std::array<std::string_view, 3> args{level, skill, count};
        WrappedText(ui, localizer.Format("summon.summary",
            "Level {0} / Skill level {1} / {2} custom attributes", args));
        const auto& hp = g_context.birth_presets[kMaxHpAttribute];
        if (hp.enabled) {
            char value[64]{};
            std::snprintf(value, sizeof(value), "%.9g", hp.value);
            const std::array<std::string_view, 1> value_args{value};
            ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
                "summon.hp", "Max HP: {0}", value_args)));
        }
        if (ui->separator != nullptr) ui->separator(ui->user);
        if (ui->checkbox != nullptr) {
            const auto hostile = localizer.Text("param.hostile", "Hostile");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(hostile), &g_context.hostile)) ConfigurationChangedLocked();
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
            const auto pet = localizer.Text("param.pet", "Pet");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(pet), &g_context.pet)) ConfigurationChangedLocked();
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
            const auto aggressive = localizer.Text("param.aggressive", "Aggressive");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(aggressive), &g_context.aggressive)) ConfigurationChangedLocked();
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
            const auto ignore_player = localizer.Text("param.ignore_player", "Do not target the player");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(ignore_player), &g_context.ignore_player)) ConfigurationChangedLocked();
            const auto mute_music = localizer.Text("param.mute_boss_music", "Disable boss music");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(mute_music), &g_context.mute_boss_music)) ConfigurationChangedLocked();
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
            const auto mute_dialogue = localizer.Text("param.mute_boss_dialogue", "Disable boss dialogue");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(mute_dialogue), &g_context.mute_boss_dialogue)) ConfigurationChangedLocked();
            ui->same_line(ui->user, 0.0f, -1.0f);
            const auto skip_cutscenes = localizer.Text("param.skip_cutscenes", "Disable cutscenes");
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(skip_cutscenes), &g_context.skip_cutscenes)) ConfigurationChangedLocked();
        }
        if (MonsterConfigName(path).starts_with("FT_") &&
            !(g_context.advanced_combat_source && !g_context.advanced_combat_blueprint.empty())) {
            if (ui->checkbox != nullptr) {
                const auto label = localizer.Text("summon.compatibility", "FT compatibility (experimental)");
                if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &g_context.ft_compatibility)) ConfigurationChangedLocked();
            }
            if (g_context.ft_compatibility != 0) {
                const auto source = MonsterConfigName(g_context.final_tower_combat_blueprint);
                const std::array<std::string_view, 1> source_args{source};
                WrappedText(ui, localizer.Format("summon.server_source", "Server combat: {0}", source_args));
                WrappedText(ui, localizer.Text("summon.compatibility_hint",
                    "Uses source HP, defense and passives; custom HP cannot exceed its native maximum"));
            }
        }
        const auto error = BirthPresetError(g_context.birth_presets);
        if (!error.empty()) WrappedText(ui, error);
        can_summon = ready && g_context.class_path[0] != '\0' &&
            summon_count > 0 &&
            (summon_count == 1 || spacing_valid) && !SummonBusyLocked() && error.empty();
        if (g_context.summon_batch && g_context.summon_batch->settings.count > 1) {
            const auto& batch = *g_context.summon_batch;
            const auto done = std::to_string(batch.completed);
            const auto total = std::to_string(batch.settings.count);
            const std::array<std::string_view, 2> progress{done, total};
            WrappedText(ui, batch.stopped
                ? localizer.Format("summon.batch_stopped", "Stopped: {0} / {1}", progress)
                : localizer.Format("summon.batch_progress", "Summoned: {0} / {1}", progress));
        }
        if (SummonBusyLocked()) {
            const auto cancel = localizer.Text("summon.cancel", "Cancel remaining");
            if (ActionButton(ui, cancel.c_str(), true)) CancelSummonsLocked();
        }
    }
    if (!DrawAdvancedSummonOptions(ui)) DrawBlueprintConfig(ui, true);
    if (!g_context.birth_attributes_ready.load(std::memory_order_acquire)) {
        WrappedText(ui, localizer.Text("param.birth_unavailable",
            "The native summon initializer is unavailable"));
        can_summon = false;
    }
    const auto label = localizer.Text("action.summon", "Summon");
    if (ActionButton(ui, label.c_str(), can_summon, -1.0F))
        (void)PushSummon(summon_count);
    DrawPresetSaveArea(ui);
}

std::string BlueprintLabel(std::string_view path) {
    const auto labels = g_context.catalog_labels.load(std::memory_order_acquire);
    if (labels) {
        if (const auto found = labels->find(path); found != labels->end()) return found->second;
    }
    const auto separator = path.find_last_of("/.");
    const auto name = separator == std::string_view::npos ? path : path.substr(separator + 1);
    return std::string(name);
}

void DrawPresetSaveArea(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    if (ui->separator) ui->separator(ui->user);
    WrappedText(ui, localizer.Text("presets.save_area", "Save to presets"));
    if (ui->input_text) {
        const auto label = localizer.Text("presets.name", "Preset name");
        ui->input_text(ui->user, anomaly::sdk::StringView(label),
            g_context.new_preset_name.data(), g_context.new_preset_name.size(), 0);
    }
    const bool valid = g_context.config_writable && !g_context.preset_transfer &&
        summon_lab::ValidPresetName(g_context.new_preset_name.data()) &&
        summon_lab::ValidBlueprintPath(g_context.class_path.data()) &&
        g_context.summon_count > 0 && std::isfinite(g_context.summon_spacing) &&
        g_context.summon_spacing >= 0 && g_context.summon_spacing <= 100 &&
        BirthPresetError(g_context.birth_presets).empty();
    const auto save = localizer.Text("presets.save_new", "Save new preset");
    if (ActionButton(ui, save.c_str(), valid)) {
        g_context.saved_presets.push_back({g_context.new_preset_name.data(), CaptureSettingsLocked()});
        g_context.selected_preset = g_context.saved_presets.size() - 1;
        g_context.preset_rename = g_context.new_preset_name;
        g_context.preset_filter = {};
        const auto matching = std::count_if(g_context.saved_presets.begin(), g_context.saved_presets.end(),
            [](const SummonPreset& preset) {
                return !g_context.presets_current_blueprint || preset.settings.class_path == g_context.class_path.data();
            });
        g_context.preset_page = (static_cast<std::size_t>(matching) - 1) / kPresetPageSize;
        g_context.preset_message = localizer.Text("presets.added", "Preset added");
        ConfigurationChangedLocked();
    }
    const bool selected = g_context.selected_preset < g_context.saved_presets.size();
    if (selected) {
        if (ui->same_line) ui->same_line(ui->user, 0, 10);
        const auto replace = localizer.Text("presets.replace", "Update selected preset");
        if (ActionButton(ui, replace.c_str(), valid &&
                g_context.saved_presets[g_context.selected_preset].settings.class_path == g_context.class_path.data())) {
            g_context.saved_presets[g_context.selected_preset] = {g_context.new_preset_name.data(), CaptureSettingsLocked()};
            g_context.preset_rename = g_context.new_preset_name;
            g_context.preset_message = localizer.Text("presets.updated", "Preset updated");
            ConfigurationChangedLocked();
        }
    }
    if (!g_context.preset_message.empty()) WrappedText(ui, g_context.preset_message);
}

void DrawPresetsTab(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    if (ui->input_text) {
        const auto label = localizer.Text("presets.search", "Find preset");
        if (ui->input_text(ui->user, anomaly::sdk::StringView(label),
                g_context.preset_filter.data(), g_context.preset_filter.size(), 0))
            g_context.preset_page = 0;
    }
    if (ui->checkbox) {
        const auto label = localizer.Text("presets.current_blueprint", "Current blueprint only");
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &g_context.presets_current_blueprint)) {
            g_context.preset_page = 0;
            ConfigurationChangedLocked();
        }
    }
    std::vector<std::size_t> filtered;
    for (std::size_t i = 0; i < g_context.saved_presets.size(); ++i) {
        const auto& preset = g_context.saved_presets[i];
        if (g_context.presets_current_blueprint && preset.settings.class_path != g_context.class_path.data()) continue;
        const auto searchable = preset.name + "\n" + BlueprintLabel(preset.settings.class_path) + "\n" + preset.settings.class_path;
        if (g_context.preset_filter[0] && !(ui->filter_match ?
                ui->filter_match(ui->user, anomaly::sdk::StringView(g_context.preset_filter.data()), anomaly::sdk::StringView(searchable)) != 0 :
                searchable.find(g_context.preset_filter.data()) != std::string::npos)) continue;
        filtered.push_back(i);
    }
    const auto page_count = std::max<std::size_t>(1, (filtered.size() + kPresetPageSize - 1) / kPresetPageSize);
    auto& page = g_context.preset_page;
    page = std::min(page, page_count - 1);
    const auto previous = localizer.Label("page.prev", "<", "preset-prev");
    if (ActionButton(ui, previous.c_str(), page > 0)) --page;
    if (ui->same_line) ui->same_line(ui->user, 0, 10);
    const auto page_number = std::to_string(page + 1);
    const auto pages = std::to_string(page_count);
    const std::array<std::string_view, 2> page_args{page_number, pages};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format("page.indicator", "{0} / {1}", page_args)));
    if (ui->same_line) ui->same_line(ui->user, 0, 10);
    const auto next = localizer.Label("page.next", ">", "preset-next");
    if (ActionButton(ui, next.c_str(), page + 1 < page_count)) ++page;
    const auto first = page * kPresetPageSize;
    const auto last = std::min(first + kPresetPageSize, filtered.size());
    // Actions below the list always refer to a selection on the visible page.
    if (std::find(filtered.begin() + first, filtered.begin() + last, g_context.selected_preset) == filtered.begin() + last)
        g_context.selected_preset = summon_lab::kNoPreset;
    const bool has_child = ui->begin_child && ui->end_child;
    const bool visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##summon-presets"), 0, 230, 0);
    if (visible) {
        for (std::size_t row = first; row < last; ++row) {
            const auto i = filtered[row];
            const auto& preset = g_context.saved_presets[i];
            const auto label = std::string(g_context.selected_preset == i ? "> " : "") + preset.name + " | " +
                BlueprintLabel(preset.settings.class_path) + "###summon-preset-" + std::to_string(i);
            if (ActionButton(ui, label.c_str(), !g_context.preset_transfer, -1)) {
                g_context.selected_preset = i;
                std::snprintf(g_context.preset_rename.data(), g_context.preset_rename.size(), "%s", preset.name.c_str());
                ApplySettingsLocked(preset.settings);
                std::snprintf(g_context.new_preset_name.data(), g_context.new_preset_name.size(), "%s", preset.name.c_str());
                g_context.preset_message = localizer.Text("presets.applied", "Preset applied");
                ConfigurationChangedLocked();
            }
        }
        if (filtered.empty()) WrappedText(ui, localizer.Text("presets.empty", "No matching presets"));
    }
    if (has_child) ui->end_child(ui->user);
    const bool selected = g_context.selected_preset < g_context.saved_presets.size();
    const bool editable = g_context.config_writable && !g_context.preset_transfer;
    if (selected) {
        if (ui->separator) ui->separator(ui->user);
        if (ui->input_text) {
            const auto label = localizer.Text("presets.rename_value", "New name");
            ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.preset_rename.data(), g_context.preset_rename.size(), 0);
        }
        const auto rename = localizer.Text("presets.rename", "Rename");
        if (ActionButton(ui, rename.c_str(), editable && summon_lab::ValidPresetName(g_context.preset_rename.data()))) {
            g_context.saved_presets[g_context.selected_preset].name = g_context.preset_rename.data();
            g_context.new_preset_name = g_context.preset_rename;
            ConfigurationChangedLocked();
        }
        if (ui->same_line) ui->same_line(ui->user, 0, 10);
        const auto remove = localizer.Text("presets.remove", "Delete preset");
        if (ActionButton(ui, remove.c_str(), editable)) {
            g_context.saved_presets.erase(g_context.saved_presets.begin() + g_context.selected_preset);
            g_context.selected_preset = summon_lab::kNoPreset;
            g_context.preset_message = localizer.Text("presets.removed", "Preset deleted");
            ConfigurationChangedLocked();
        }
    }
    if (ui->separator) ui->separator(ui->user);
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("presets.file_name", "File name")));
    if (ui->same_line) ui->same_line(ui->user, 0, 6);
    if (ui->input_text) {
        if (ui->struct_size >= offsetof(AnomalyUiServiceV1, set_next_item_width) + sizeof(ui->set_next_item_width) &&
                ui->set_next_item_width)
            ui->set_next_item_width(ui->user, -368.0F);
        if (ui->input_text(ui->user, anomaly::sdk::StringView("##preset-export-name"),
                g_context.preset_export_name.data(), g_context.preset_export_name.size(), 0)) ConfigurationChangedLocked();
    }
    if (ui->same_line) ui->same_line(ui->user, 0, 6);
    const auto choose_folder = localizer.Text("presets.choose_folder", "Choose folder");
    if (ActionButton(ui, choose_folder.c_str(), !g_context.preset_transfer && !g_context.preset_picker.Busy(), 110))
        OpenPresetPickerLocked(summon_lab::PresetFilePicker::Kind::Folder);
    if (ui->same_line) ui->same_line(ui->user, 0, 6);
    const bool can_export = !g_context.preset_transfer && !g_context.preset_picker.Busy() &&
        !g_context.preset_export_folder.empty() && g_context.preset_export_name[0];
    const auto export_selected = localizer.Text("presets.export_selected", "Export selected");
    if (ActionButton(ui, export_selected.c_str(), can_export &&
            g_context.selected_preset < g_context.saved_presets.size(), 130))
        QueuePresetTransferLocked(false, false);
    if (ui->same_line) ui->same_line(ui->user, 0, 6);
    const auto export_all = localizer.Text("presets.export_all", "Export all");
    if (ActionButton(ui, export_all.c_str(), can_export && !g_context.preset_transfer && !g_context.saved_presets.empty(), 110))
        QueuePresetTransferLocked(false, true);
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("presets.import_file", "Import file")));
    if (ui->same_line) ui->same_line(ui->user, 0, 6);
    const auto choose_file = localizer.Text("presets.choose_file", "Choose file");
    if (ActionButton(ui, choose_file.c_str(), !g_context.preset_transfer && !g_context.preset_picker.Busy(), 110))
        OpenPresetPickerLocked(summon_lab::PresetFilePicker::Kind::ImportFile);
    if (ui->same_line) ui->same_line(ui->user, 0, 6);
    const auto import = localizer.Text("presets.import", "Import");
    if (ActionButton(ui, import.c_str(), editable && !g_context.preset_transfer &&
            !g_context.preset_picker.Busy() && !g_context.preset_import_file.empty())) QueuePresetTransferLocked(true, false);
    if (!g_context.preset_import_file.empty()) {
        if (ui->same_line) ui->same_line(ui->user, 0, 10);
        const auto& file = g_context.preset_import_file;
        const auto name = summon_lab::PathUtf8(std::filesystem::path(std::u8string(file.begin(), file.end())).filename());
        ui->text(ui->user, anomaly::sdk::StringView(name));
    }
    if (!g_context.preset_message.empty()) WrappedText(ui, g_context.preset_message);
}

void DrawAggroEditor(const AnomalyUiServiceV1* ui, bool ready,
                     const std::vector<ManagementRecord>& records, std::size_t count,
                     const std::vector<std::uint64_t>& members, const summon_lab::BattleGraph& battles) {
    const auto& localizer = g_context.localizer;
    bool open{};
    {
        std::scoped_lock lock(g_context.mutex);
        open = g_context.show_aggro_editor;
    }
    const auto title = localizer.Text("aggro.title", "Advanced aggro");
    if (ActionButton(ui, (std::string(open ? "v " : "> ") + title).c_str(), true, -1.0F)) {
        open = !open;
        std::scoped_lock lock(g_context.mutex);
        g_context.show_aggro_editor = open;
    }
    if (!open) return;
    std::vector<std::uint64_t> editable;
    const ManagementRecord* first{};
    bool mixed{};
    for (std::size_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        if (record.pet || record.hp <= 0 || record.max_hp <= 0 ||
            std::find(members.begin(), members.end(), record.id) == members.end()) continue;
        editable.push_back(record.id);
        if (first == nullptr) first = &record;
        else mixed = mixed || record.aggro != first->aggro;
    }
    if (first == nullptr) {
        WrappedText(ui, localizer.Text("aggro.no_entities", "No available combatants"));
        return;
    }
    AggroSettings draft;
    {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.aggro_edit_members != editable) {
            g_context.aggro_edit_members = editable;
            g_context.aggro_edit = first->aggro;
        }
        draft = g_context.aggro_edit;
    }
    const auto label_for = [&](std::uint64_t id) {
        for (std::size_t i = 0; i < count; ++i)
            if (records[i].id == id) return "#" + std::to_string(id) + " " + records[i].label.data();
        return localizer.Text("aggro.none", "None");
    };
    if (editable.size() == 1) {
        const auto target = label_for(first->current_target_id);
        const std::array<std::string_view, 1> args{target};
        WrappedText(ui, localizer.Format("aggro.current", "Current target: {0}", args));
    } else {
        const auto size = std::to_string(editable.size());
        const std::array<std::string_view, 1> args{size};
        WrappedText(ui, localizer.Format("aggro.scope", "Apply to {0} entities", args));
        if (mixed) WrappedText(ui, localizer.Text("aggro.mixed", "Group settings differ"));
    }
    const auto policy = [&](const std::string& label, TargetPolicy value) {
        if (ActionButton(ui, label.c_str(), draft.policy != value)) draft.policy = value;
    };
    policy(localizer.Text("aggro.native", "Native selection"), TargetPolicy::Native);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    policy(localizer.Text("aggro.distributed", "Distributed lock"), TargetPolicy::Distributed);
    policy(localizer.Text("aggro.nearest", "Nearest target"), TargetPolicy::Nearest);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    policy(localizer.Text("aggro.lowest_hp", "Lowest HP ratio"), TargetPolicy::LowestHealth);

    std::vector<const ManagementRecord*> targets;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& candidate = records[i];
        if (!candidate.pet && candidate.hp > 0 && candidate.max_hp > 0 &&
            std::any_of(editable.begin(), editable.end(), [&](auto id) { return battles.Linked(id, candidate.id); }))
            targets.push_back(&candidate);
    }
    if (std::none_of(targets.begin(), targets.end(), [&](const auto* record) { return record->id == draft.priority_target; }))
        draft.priority_target = 0;
    const auto automatic = localizer.Text("aggro.automatic", "Automatic");
    const auto priority = draft.priority_target ? label_for(draft.priority_target) : automatic;
    const std::array<std::string_view, 1> priority_args{priority};
    const auto priority_label = localizer.Format("aggro.priority", "Priority target: {0}", priority_args);
    if (ui->begin_menu != nullptr && ui->end_menu != nullptr) {
        if (ui->begin_menu(ui->user, anomaly::sdk::StringView(priority_label + "###aggro-priority"), ready)) {
            if (ActionButton(ui, automatic.c_str(), true)) draft.priority_target = 0;
            const bool child = ui->begin_child != nullptr && ui->end_child != nullptr;
            const bool visible = !child || ui->begin_child(ui->user,
                anomaly::sdk::StringView("##priority-targets"), 320.0F, 180.0F, 0) != 0;
            if (visible)
                for (const auto* candidate : targets) {
                    const auto label = label_for(candidate->id) + "###priority-" + std::to_string(candidate->id);
                    if (ActionButton(ui, label.c_str(), draft.priority_target != candidate->id, -1.0F))
                        draft.priority_target = candidate->id;
                }
            if (child) ui->end_child(ui->user);
            ui->end_menu(ui->user);
        }
    } else WrappedText(ui, priority_label);
    if (ui->input_double != nullptr) {
        const auto label = localizer.Text("aggro.range", "Chase distance (m, 0 = unlimited)");
        ui->input_double(ui->user, anomaly::sdk::StringView(label), &draft.chase_distance, 0, 0);
    }
    if (ui->checkbox != nullptr) {
        int paused = draft.paused ? 1 : 0;
        const auto label = localizer.Text("aggro.pause", "Pause aggro (keep opponents)");
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &paused)) draft.paused = paused != 0;
    }
    const auto reset = localizer.Text("aggro.defaults", "Defaults");
    if (ActionButton(ui, reset.c_str(), true)) draft = {};
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    const bool valid = std::isfinite(draft.chase_distance) && draft.chase_distance >= 0 &&
        draft.chase_distance <= kBattleChaseRange / 100.0;
    const auto apply = localizer.Text("aggro.apply", "Apply aggro settings");
    if (ActionButton(ui, apply.c_str(), ready && valid)) {
        PendingOp op{OpKind::SetAggroSettings};
        op.members = editable;
        op.aggro = draft;
        (void)PushOp(op);
    }
    if (!valid) WrappedText(ui, localizer.Text("aggro.invalid_range", "Chase distance must be between 0 and 10000 m"));
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.aggro_edit = draft;
    }
}

void DrawGroupActions(const AnomalyUiServiceV1* ui, bool ready,
                      const std::string& blueprint, const std::vector<std::uint64_t>& ids,
                      const std::map<std::string, std::vector<std::uint64_t>>& combat_groups,
                      const summon_lab::BattleGraph& battles) {
    const auto& localizer = g_context.localizer;
    WrappedText(ui, blueprint.empty() ? localizer.Text("manage.all", "All blueprints") : BlueprintLabel(blueprint));
    if (!blueprint.empty()) WrappedText(ui, blueprint);
    const auto count = std::to_string(ids.size());
    const std::array<std::string_view, 1> args{count};
    WrappedText(ui, localizer.Format("manage.group_count", "Group: {0} entities", args));
    const auto remove = localizer.Text("manage.remove_group", "Remove group");
    if (ActionButton(ui, remove.c_str(), ready)) {
        if (blueprint.empty()) (void)PushOp(PendingOp{OpKind::ClearAll});
        else (void)PushSelection(OpKind::Remove, ids);
    }
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    const auto stop = localizer.Text("manage.stop_group", "Stop group battles");
    const bool fighting = std::any_of(ids.begin(), ids.end(), [&](auto id) { return battles.Contains(id); });
    if (ActionButton(ui, stop.c_str(), ready && fighting)) {
        PendingOp op{OpKind::StopGroupBattle};
        op.members = ids;
        (void)PushOp(op);
    }
    if (ui->separator != nullptr) ui->separator(ui->user);
    const auto source = combat_groups.find(blueprint);
    if (source != combat_groups.end() && !source->second.empty()) {
        WrappedText(ui, localizer.Text("manage.group_opponents", "Enemy blueprints (check to fight)"));
        for (const auto& [path, opponents] : combat_groups) {
            if (path == blueprint || opponents.empty()) continue;
            const std::string label = BlueprintLabel(path) + "###group-battle-" + path;
            int linked = std::all_of(source->second.begin(), source->second.end(), [&](auto a) {
                return std::all_of(opponents.begin(), opponents.end(), [&](auto b) { return battles.Linked(a, b); });
            }) ? 1 : 0;
            if (ready && ui->checkbox != nullptr &&
                ui->checkbox(ui->user, anomaly::sdk::StringView(label), &linked)) {
                PendingOp op{OpKind::SetGroupBattle, 0, linked != 0};
                op.members = source->second;
                op.opponents = opponents;
                (void)PushOp(op);
            } else if (!ready || ui->checkbox == nullptr) WrappedText(ui, BlueprintLabel(path));
        }
        if (ui->separator != nullptr) ui->separator(ui->user);
    }
    const auto action = [&](std::string_view label, OpKind kind, bool enabled) {
        if (ActionButton(ui, std::string(label).c_str(), ready))
            (void)PushSelection(kind, ids, enabled);
    };
    action(localizer.Text("manage.show_hp", "Show boss HP bar"), OpKind::SetBossHpBar, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.hide_hp", "Hide boss HP bar"), OpKind::SetBossHpBar, false);
    action(localizer.Text("manage.enable_collision", "Enable collision"), OpKind::SetCollision, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.disable_collision", "Disable collision"), OpKind::SetCollision, false);
    action(localizer.Text("manage.hide_model", "Hide model"), OpKind::SetHidden, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.show_model", "Show model"), OpKind::SetHidden, false);
}

void DrawManagementTab(const AnomalyUiServiceV1* ui, bool ready) {
    const auto& localizer = g_context.localizer;
    std::vector<ManagementRecord> records;
    std::vector<AttributeValue> attributes;
    std::string server_combat_source;
    std::string editor_warning;
    std::string editor_rule_warning;
    std::string blueprint;
    std::map<std::string, std::vector<std::uint64_t>> groups, combat_groups;
    std::size_t count{};
    std::size_t page{};
    std::size_t page_count{};
    bool group_ready{};
    summon_lab::BattleGraph battles;
    const ManagementRecord* selected{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.records.size();
        records.resize(count);
        group_ready = ready && !SummonBusyLocked();
        page = g_context.management_page;
        blueprint = g_context.management_blueprint;
        battles = g_context.battles;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& source = g_context.records[i];
            records[i].id = source.id;
            records[i].hp = source.hp;
            records[i].max_hp = source.max_hp;
            records[i].pet = source.pet;
            records[i].class_path = source.class_path;
            records[i].aggro = source.aggro;
            records[i].current_target_id = source.current_target_id;
            groups[source.class_path].push_back(source.id);
            if (!source.pet && source.hp > 0 && source.max_hp > 0)
                combat_groups[source.class_path].push_back(source.id);
            std::memcpy(records[i].label.data(), source.label, records[i].label.size());
            if (source.id == g_context.selected_record_id) {
                selected = &records[i];
                attributes = source.attributes;
                server_combat_source = source.server_combat_source;
                editor_warning = source.editor_warning;
                editor_rule_warning = source.editor_rule_warning;
            }
        }
    }
    if (count == 0) {
        WrappedText(ui, localizer.Text("list.empty", "No summoned entities"));
        return;
    }
    const auto select_group = [&](const std::string& path) {
        blueprint = path;
        page = 0;
        selected = nullptr;
        attributes.clear();
        server_combat_source.clear();
        editor_warning.clear();
        editor_rule_warning.clear();
        std::scoped_lock lock(g_context.mutex);
        g_context.management_blueprint = path;
        g_context.management_page = 0;
        g_context.selected_record_id = 0;
    };
    if (!blueprint.empty() && !groups.contains(blueprint)) select_group({});
    if (selected != nullptr && !blueprint.empty() && selected->class_path != blueprint)
        select_group(blueprint);
    std::vector<std::size_t> filtered;
    std::vector<std::uint64_t> members;
    const bool has_child = ui->begin_child != nullptr && ui->end_child != nullptr &&
        ui->same_line != nullptr;
    const bool list_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##summoned-list"), 260.0F, 0.0F,
        ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y) != 0;
    if (list_visible) {
        const std::string count_text = std::to_string(count);
        const std::array<std::string_view, 1> count_args{count_text};
        ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
            "list.header", "Summoned ({0})", count_args)));
        const bool groups_visible = !has_child || ui->begin_child(ui->user,
            anomaly::sdk::StringView("##blueprint-groups"), 0.0F, 150.0F, 0) != 0;
        if (groups_visible) {
            const auto all = localizer.Text("manage.all", "All blueprints");
            if (ActionButton(ui, all.c_str(), !blueprint.empty() || selected != nullptr, -1.0F))
                select_group({});
            for (const auto& [path, ids] : groups) {
                const auto size = std::to_string(ids.size());
                const auto name = BlueprintLabel(path);
                const std::array<std::string_view, 2> args{name, size};
                const auto label = localizer.Format("manage.group_label", "{0} ({1})", args) +
                    "###blueprint-group-" + path;
                if (ActionButton(ui, label.c_str(), path != blueprint || selected != nullptr, -1.0F))
                    select_group(path);
            }
        }
        if (has_child) ui->end_child(ui->user);
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!blueprint.empty() && records[i].class_path != blueprint) continue;
        filtered.push_back(i);
        members.push_back(records[i].id);
    }
    page_count = (filtered.size() + kManagementPageSize - 1) / kManagementPageSize;
    page = page_count == 0 ? 0 : std::min(page, page_count - 1);
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.management_page = page;
    }
    if (list_visible) {
        const auto previous = localizer.Label("page.prev", "<", "entity-prev");
        if (ActionButton(ui, previous.c_str(), page > 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.management_page = --page;
        }
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const std::string page_text = std::to_string(page + 1);
        const std::string pages_text = std::to_string(page_count);
        const std::array<std::string_view, 2> page_args{page_text, pages_text};
        ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
            "manage.page", "{0} / {1}", page_args)));
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const auto next = localizer.Label("page.next", ">", "entity-next");
        if (ActionButton(ui, next.c_str(), page + 1 < page_count)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.management_page = ++page;
        }
        for (std::size_t i = page * kManagementPageSize;
             i < filtered.size() && i < (page + 1) * kManagementPageSize; ++i) {
            const auto& record = records[filtered[i]];
            char line[256]{};
            std::snprintf(line, sizeof(line), "#%llu %s###entity-%llu",
                static_cast<unsigned long long>(record.id), record.label.data(),
                static_cast<unsigned long long>(record.id));
            if (ActionButton(ui, line, selected == nullptr || record.id != selected->id, -1.0F)) {
                std::scoped_lock lock(g_context.mutex);
                selected = nullptr;
                attributes.clear();
                // Copy the selected entity's snapshot; no game reads happen in Draw.
                for (std::size_t k = 0; k < g_context.records.size(); ++k) {
                    if (g_context.records[k].id != record.id) continue;
                    g_context.selected_record_id = record.id;
                    selected = &record;
                    attributes = g_context.records[k].attributes;
                    server_combat_source = g_context.records[k].server_combat_source;
                    editor_warning = g_context.records[k].editor_warning;
                    editor_rule_warning = g_context.records[k].editor_rule_warning;
                    break;
                }
            }
        }
        const auto clear = localizer.Text("action.clear", "Clear all");
        if (ActionButton(ui, clear.c_str(), ready, -1.0F))
            (void)PushOp(PendingOp{OpKind::ClearAll, 0});
    }
    if (has_child) ui->end_child(ui->user);
    if (has_child) ui->same_line(ui->user, 0.0F, 12.0F);
    const bool editor_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##summoned-editor"), 0.0F, 0.0F,
        ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y) != 0;
    if (!editor_visible) {
        if (has_child) ui->end_child(ui->user);
        return;
    }
    if (selected == nullptr) {
        DrawGroupActions(ui, group_ready, blueprint, members, combat_groups, battles);
        DrawAggroEditor(ui, group_ready, records, count, members, battles);
        if (has_child) ui->end_child(ui->user);
        return;
    }
    const std::string selected_id = std::to_string(selected->id);
    const std::array<std::string_view, 1> selected_args{selected_id};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
        "manage.selected", "Manage entity #{0}", selected_args)));
    WrappedText(ui, selected->label.data());
    WrappedText(ui, BlueprintLabel(selected->class_path));
    if (!editor_warning.empty()) WrappedText(ui, editor_warning);
    if (!editor_rule_warning.empty()) WrappedText(ui, editor_rule_warning);
    if (!server_combat_source.empty()) {
        const std::array<std::string_view, 1> source_args{server_combat_source};
        WrappedText(ui, localizer.Format("summon.server_source", "Server combat: {0}", source_args));
    }
    char hp[64]{}, max_hp[64]{};
    std::snprintf(hp, sizeof(hp), "%.0f", static_cast<double>(selected->hp));
    std::snprintf(max_hp, sizeof(max_hp), "%.0f", static_cast<double>(selected->max_hp));
    const std::array<std::string_view, 2> hp_args{hp, max_hp};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
        "manage.hp", "HP {0} / {1}", hp_args)));
    const auto remove = localizer.Text("action.remove", "Remove");
    if (ActionButton(ui, remove.c_str(), ready))
        (void)PushOp(PendingOp{OpKind::Remove, selected->id});
    if (ui->separator != nullptr) ui->separator(ui->user);
    WrappedText(ui, localizer.Text("duel.opponents", "Opponents (check to fight)"));
    const bool has_unselected_opponent = std::any_of(records.begin(), records.begin() + count,
        [&](const auto& candidate) {
            return candidate.id != selected->id && !candidate.pet && candidate.hp > 0 &&
                candidate.max_hp > 0 && !battles.Linked(selected->id, candidate.id);
        });
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    const auto select_all = localizer.Text("duel.select_all", "Select all");
    if (ActionButton(ui, select_all.c_str(), ready && !selected->pet && selected->hp > 0 &&
        selected->max_hp > 0 && has_unselected_opponent))
        (void)PushOp(PendingOp{OpKind::SelectAllOpponents, selected->id});
    if (selected->pet) WrappedText(ui, localizer.Text("duel.no_pets",
        "Summon combatants with Pet unchecked"));
    const bool opponents_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##battle-opponents"), 0.0F, 130.0F, 0) != 0;
    if (opponents_visible) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& candidate = records[i];
            if (candidate.id == selected->id || candidate.pet) continue;
            const std::string text = "#" + std::to_string(candidate.id) + " " + candidate.label.data();
            const std::string label = text + "###battle-" + selected_id + "-" + std::to_string(candidate.id);
            int linked = battles.Linked(selected->id, candidate.id) ? 1 : 0;
            if (ready && !selected->pet && ui->checkbox != nullptr) {
                if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &linked))
                    (void)PushOp(PendingOp{OpKind::SetBattleEdge, selected->id, linked != 0, candidate.id});
            } else WrappedText(ui, text);
        }
    }
    if (has_child) ui->end_child(ui->user);
    const auto stop_duel = localizer.Text("duel.stop", "Stop this Boss");
    if (ActionButton(ui, stop_duel.c_str(), ready && battles.Contains(selected->id)))
        (void)PushOp(PendingOp{OpKind::StopDuel, selected->id});
    DrawAggroEditor(ui, group_ready, records, count, {selected->id}, battles);
    if (ui->separator != nullptr) ui->separator(ui->user);
    const auto action = [&](std::string_view label, OpKind kind, bool enabled) {
        if (ActionButton(ui, std::string(label).c_str(), ready))
            (void)PushOp(PendingOp{kind, selected->id, enabled});
    };
    action(localizer.Text("manage.show_hp", "Show boss HP bar"), OpKind::SetBossHpBar, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.hide_hp", "Hide boss HP bar"), OpKind::SetBossHpBar, false);
    action(localizer.Text("manage.enable_collision", "Enable collision"), OpKind::SetCollision, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.disable_collision", "Disable collision"), OpKind::SetCollision, false);
    action(localizer.Text("manage.hide_model", "Hide model"), OpKind::SetHidden, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.show_model", "Show model"), OpKind::SetHidden, false);
    if (ui->separator != nullptr) ui->separator(ui->user);
    WrappedText(ui, localizer.Text("manage.attributes_hint",
        "Current attributes (read only)"));
    const bool attributes_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##current-attributes"), 0.0F, 260.0F, 0) != 0;
    if (attributes_visible) {
        if (attributes.empty())
            WrappedText(ui, localizer.Text("editor.unavailable", "No readable attributes for this entity"));
        for (const auto& attribute : attributes) {
            char base[64]{}, current[64]{};
            std::snprintf(base, sizeof(base), "%.6g", static_cast<double>(attribute.base));
            std::snprintf(current, sizeof(current), "%.6g", static_cast<double>(attribute.current));
            const auto found = std::find(kBirthAttributeNames.begin(), kBirthAttributeNames.end(), attribute.name);
            const std::string_view label = found == kBirthAttributeNames.end() ? attribute.name :
                g_context.attribute_descriptions[static_cast<std::size_t>(found - kBirthAttributeNames.begin())].label;
            const std::array<std::string_view, 3> args{label, base, current};
            WrappedText(ui, localizer.Format(
                "manage.attribute", "{0}  (Base {1} / Current {2})", args));
        }
    }
    if (has_child) ui->end_child(ui->user);
    if (has_child) ui->end_child(ui->user);
}

void ANOMALY_CALL Draw(void* /*context*/, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr)
        return;
    const auto& localizer = g_context.localizer;
    int open = 1;
    // FirstUseEver keeps the window resizable after its first appearance.
    if (ui->set_next_window_size != nullptr)
        ui->set_next_window_size(ui->user, 820.0F, 0.0F, 4U);
    if (ui->set_next_window_size_constraints != nullptr)
        ui->set_next_window_size_constraints(ui->user, 640.0F, 240.0F, 100000.0F, 100000.0F);
    anomaly::sdk::UiWindow window(ui, localizer.Text("window.title", "Summon Lab"), &open);
    if (!window) return;
    const bool has_tabs = ui->struct_size >= offsetof(AnomalyUiServiceV1, end_tab_bar) +
        sizeof(ui->end_tab_bar) && ui->begin_tab_bar != nullptr && ui->begin_tab_item != nullptr &&
        ui->end_tab_item != nullptr && ui->end_tab_bar != nullptr;
    if (!has_tabs) {
        WrappedText(ui, localizer.Text("status.tabs_unavailable", "This host does not provide tab controls"));
        return;
    }
    bool world_ready{}, invoke_ready{}, failed{};
    std::uint32_t pending{};
    std::array<char, kTextCapacity> last_result{};
    {
        std::scoped_lock lock(g_context.mutex);
        ConsumePresetPickerLocked();
        world_ready = g_context.session_world_ready;
        invoke_ready = g_context.has_invoke;
        failed = g_context.last_result_failed;
        last_result = g_context.last_result;
        pending = static_cast<std::uint32_t>(g_context.pending.size());
    }
    if (ui->begin_tab_bar(ui->user, anomaly::sdk::StringView("##summon-lab-tabs"),
                         ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN)) {
        const auto blueprint = localizer.Label("tab.blueprint", "Blueprints", "blueprint");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(blueprint), nullptr, 0, 1)) {
            DrawBlueprintTab(ui);
            ui->end_tab_item(ui->user);
        }
        const auto settings = localizer.Label("tab.attributes", "Attributes", "attributes");
        const auto presets = localizer.Label("tab.presets", "Presets", "presets");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(presets), nullptr, 0, 1)) {
            DrawPresetsTab(ui);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(settings), nullptr, 0, 1)) {
            DrawSettingsTab(ui);
            ui->end_tab_item(ui->user);
        }
        const auto editor = localizer.Label("tab.editor", "Editor", "editor");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(editor), nullptr, 0, 1)) {
            DrawBlueprintEditor(ui, world_ready && invoke_ready);
            ui->end_tab_item(ui->user);
        }
        const auto summon = localizer.Label("tab.summon", "Summon", "summon");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(summon), nullptr, 0, 1)) {
            DrawSummonTab(ui, world_ready && invoke_ready);
            ui->end_tab_item(ui->user);
        }
        const auto manage = localizer.Label("tab.manage", "Manage", "manage");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(manage), nullptr, 0, 1)) {
            DrawManagementTab(ui, world_ready && invoke_ready);
            ui->end_tab_item(ui->user);
        }
        ui->end_tab_bar(ui->user);
    }
    if (ui->separator != nullptr) ui->separator(ui->user);
    {
        std::scoped_lock lock(g_context.mutex);
        if (!g_context.config_error.empty()) WrappedText(ui, g_context.config_error);
    }
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Text(
        world_ready ? "status.ready" : "status.not_ready", world_ready ? "World ready" : "World not ready")));
    if (pending != 0) {
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
        const std::string pending_text = std::to_string(pending);
        const std::array<std::string_view, 1> args{pending_text};
        ui->text(ui->user, anomaly::sdk::StringView(localizer.Format("status.pending", "{0} queued", args)));
    }
    if (failed) {
        const std::array<std::string_view, 1> args{last_result.data()};
        WrappedText(ui, localizer.Format("status.failed", "Last failure: {0}", args));
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   anomaly::sdk::StringView("anomaly.summon-lab"),
                   anomaly::sdk::StringView("Summon Lab"),
                   anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
