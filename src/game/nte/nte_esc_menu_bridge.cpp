#include "anomaly/nte_esc_menu_bridge.hpp"

#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/hook_manager.hpp"
#include "anomaly/sdk/anomaly_sdk.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr std::uintptr_t kObjectsItemsOffset = 16U;
constexpr std::uint32_t kObjectChunkSize = 65536U;
constexpr std::uintptr_t kObjectItemStride = 24U;
constexpr std::uintptr_t kObjectClassOffset = 16U;
constexpr std::uintptr_t kObjectNameOffset = 24U;
constexpr std::uintptr_t kObjectOuterOffset = 32U;
constexpr std::uintptr_t kStructPropertyLinkOffset = 112U;
constexpr std::uintptr_t kFieldNameOffset = 32U;
constexpr std::uintptr_t kPropertyOffsetInternalOffset = 68U;
constexpr std::uintptr_t kPropertyLinkNextOffset = 72U;
constexpr std::uint32_t kObjectsPerUpdate = 16384U;
constexpr auto kScanBudget = std::chrono::milliseconds(1);
constexpr std::uint32_t kStableContainerUpdates = 12U;
constexpr auto kMenuPageQuiescence = std::chrono::milliseconds(250);
constexpr auto kRegistryPollInterval = std::chrono::milliseconds(250);
constexpr auto kAppendRetryInterval = std::chrono::seconds(1);
constexpr std::uint32_t kIconImportAttemptsBeforeFallback = 3U;
constexpr std::size_t kMaximumNameBytes = 1024U;
constexpr std::uint32_t kMissingPropertyOffset = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::string_view kHookOwner = "anomaly.nte.esc-menu";
constexpr std::uint64_t kHookGeneration = 1U;

enum class BridgeFunction : std::size_t {
    Create,
    AddChild,
    GetChildAt,
    GetChildIndex,
    GetChildrenCount,
    RemoveChildAt,
    StringToText,
    SetText,
    ImportBufferAsTexture2D,
    SetBrushFromTexture,
    Count,
};

struct FunctionSpec final {
    std::string_view name;
    std::string_view owner;
};

constexpr std::array<FunctionSpec, static_cast<std::size_t>(BridgeFunction::Count)>
    kFunctionSpecs{{
        {"Create", "WidgetBlueprintLibrary"},
        {"AddChild", "PanelWidget"},
        {"GetChildAt", "PanelWidget"},
        {"GetChildIndex", "PanelWidget"},
        {"GetChildrenCount", "PanelWidget"},
        {"RemoveChildAt", "PanelWidget"},
        {"Conv_StringToText", "KismetTextLibrary"},
        {"SetText", "TextBlock"},
        {"ImportBufferAsTexture2D", "KismetRenderingLibrary"},
        {"SetBrushFromTexture", "Image"},
    }};

// These indices belong to the v1 NTE build and are always validated before use.
constexpr std::array<std::uint32_t, 8> kExactBuildObjectIndices{
    13302U, 12745U, 12748U, 12749U, 12750U, 12754U, 36134U, 13642U};

struct FNameValue final {
    std::uint32_t comparison_index{};
    std::uint32_t number{};
};

struct UnrealString final {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct UnrealByteArray final {
    std::uint8_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct DesiredButton final {
    AnomalyGenerationHandleV1 handle{};
    std::string id;
    std::string label;
    std::uint32_t icon_format{};
    std::vector<std::uint8_t> icon_bytes;
};

struct WidgetBinding final {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t widget{};
    std::int32_t child_index{-1};
};

struct IconImportFailure final {
    AnomalyGenerationHandleV1 handle{};
    std::uint32_t attempts{};
};

struct MenuCandidate final {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t container{};
    std::uintptr_t page_item{};
    std::uintptr_t pagination{};
    std::uintptr_t menu_root{};
    std::uint32_t index{};
    std::uint64_t discovery_order{};
};

struct Context final {
    CoreMemoryServices memory_services;
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    NteEscMenuBridge::SnapshotProvider snapshot_provider;
    NteEscMenuBridge::InvokeButton invoke_button;
    NteEscMenuBridge::Logger logger;
    std::unique_ptr<HookManager> hooks;
    std::atomic_bool stopping{true};
    std::uintptr_t process_event{};
    std::uintptr_t add_menu_page{};
    std::uintptr_t button_clicked{};
    std::uintptr_t object_registry{};
    std::uintptr_t object_chunks{};
    std::array<std::uintptr_t, 64> chunks{};
    std::uint32_t reflection_scan_cursor{};
    bool reflection_scan_complete{};
    bool exact_build_hints_checked{};
    std::uintptr_t function_class{};
    std::uintptr_t class_class{};
    std::array<std::uintptr_t, kFunctionSpecs.size()> function_owner_classes{};
    std::uintptr_t feature_button_class{};
    std::uintptr_t widget_blueprint_library_cdo{};
    std::uintptr_t kismet_text_library_cdo{};
    std::uintptr_t kismet_rendering_library_cdo{};
    std::uint32_t text_game_feature_name_offset{kMissingPropertyOffset};
    std::uint32_t image_icon_offset{kMissingPropertyOffset};
    std::uint32_t menu_pagination_offset{kMissingPropertyOffset};
    std::uint32_t pagination_scroll_box_offset{kMissingPropertyOffset};
    std::uint32_t page_menu_buttons_offset{kMissingPropertyOffset};
    std::uintptr_t menu_container{};
    std::uint32_t menu_container_index{};
    std::uintptr_t menu_scroll_box{};
    std::int32_t menu_page_index{-1};
    std::int32_t observed_child_count{-1};
    std::uint32_t stable_container_updates{};
    bool container_stable{};
    std::uint64_t next_discovery_order{};
    std::atomic<std::uint64_t> menu_rebuild_sequence{};
    std::atomic<std::int32_t> last_menu_page_index{-1};
    std::atomic<std::uintptr_t> last_menu_root{};
    std::atomic<std::uint64_t> last_menu_page_event_ms{};
    std::uint64_t observed_menu_rebuild_sequence{};
    std::int32_t expected_menu_page_index{-1};
    std::uintptr_t expected_menu_root{};
    std::uint64_t observed_menu_page_event_ms{};
    bool menu_rebuild_active{};
    bool reconciliation_complete{};
    std::chrono::steady_clock::time_point next_registry_poll{};
    std::uint32_t reconcile_stage{kMissingPropertyOffset};
    std::uint32_t append_failure_stage{};
    AnomalyGenerationHandleV1 menu_container_handle{};
    std::recursive_mutex bridge_mutex;
    std::vector<MenuCandidate> menu_candidates;
    std::vector<WidgetBinding> bindings;
    std::vector<IconImportFailure> icon_import_failures;
    std::vector<AnomalyGenerationHandleV1> pending_clicks;
};

using ProcessEventFn = void(ANOMALY_CALL*)(void*, void*, void*);
using AddMenuPageFn = void(ANOMALY_CALL*)(void*, std::int32_t);
using ButtonClickedFn = void(ANOMALY_CALL*)(void*);

std::atomic<Context*> g_active{};
std::atomic<HookManager*> g_hook_manager{};
std::atomic<AddMenuPageFn> g_add_menu_page_original{};
std::atomic<ButtonClickedFn> g_button_clicked_original{};
std::mutex g_process_mutex;
std::array<std::atomic<std::uintptr_t>, static_cast<std::size_t>(BridgeFunction::Count)>
    g_functions{};

template <typename Table, typename Field>
bool HasField(const Table* table, const std::size_t offset) noexcept {
    return table != nullptr && table->struct_size >= offset + sizeof(Field);
}

bool NamesReady(const AnomalyUe5NamesServiceV1* names) noexcept {
    return HasField<AnomalyUe5NamesServiceV1, decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
        names->resolve_utf8 != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* objects) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::count)>(
               objects, offsetof(AnomalyUe5ObjectsServiceV1, count)) &&
        HasField<AnomalyUe5ObjectsServiceV1,
            decltype(AnomalyUe5ObjectsServiceV1::snapshot_at)>(
            objects, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_at)) &&
        HasField<AnomalyUe5ObjectsServiceV1,
            decltype(AnomalyUe5ObjectsServiceV1::snapshot_by_handle)>(
            objects, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_by_handle)) &&
        objects->count != nullptr && objects->snapshot_at != nullptr &&
        objects->snapshot_by_handle != nullptr;
}

template <typename T>
bool ReadValue(const Context& context, const std::uintptr_t address, T* value) noexcept {
    return value != nullptr && address != 0U && context.memory_services.memory != nullptr &&
        context.memory_services.memory->ReadMemoryInto(address, value, sizeof(T));
}

template <typename T>
bool ReadGameThreadValue(
    const std::uintptr_t address, T* const value) noexcept {
    if (address == 0U || value == nullptr) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
#endif
}

void Log(Context& context, const std::uint32_t level, const std::string_view message) noexcept {
    if (!context.logger) return;
    try {
        context.logger(level, std::string(message));
    } catch (...) {
    }
}

bool ResolveName(
    const AnomalyUe5NamesServiceV1* names, const std::uint32_t name_id,
    std::string* value) noexcept {
    if (value == nullptr || !NamesReady(names)) return false;
    std::size_t size{};
    if (names->resolve_utf8(names->user, name_id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1U || size > kMaximumNameBytes) {
        return false;
    }
    try {
        std::string resolved(size, '\0');
        if (names->resolve_utf8(names->user, name_id, resolved.data(), &size).code !=
                ANOMALY_STATUS_V1_OK ||
            size == 0U || size > resolved.size()) {
            return false;
        }
        const std::size_t terminator = resolved.find('\0');
        if (terminator == std::string::npos) return false;
        resolved.resize(terminator);
        *value = std::move(resolved);
        return true;
    } catch (...) {
        return false;
    }
}

bool ResolveObjectName(
    const Context& context, const std::uintptr_t object, std::string* name) noexcept {
    std::uint32_t name_id{};
    return object != 0U && ReadValue(context, object + kObjectNameOffset, &name_id) &&
        ResolveName(context.names, name_id, name);
}

bool ResolveClassName(
    const Context& context, const std::uintptr_t object, std::string* name) noexcept {
    std::uintptr_t class_object{};
    return ReadValue(context, object + kObjectClassOffset, &class_object) &&
        ResolveObjectName(context, class_object, name);
}

std::uintptr_t ObjectAddressAt(Context& context, const std::uint32_t index) noexcept {
    if (context.object_chunks == 0U) return 0U;
    const std::uint32_t chunk_index = index / kObjectChunkSize;
    if (chunk_index >= context.chunks.size()) return 0U;
    std::uintptr_t chunk = context.chunks[chunk_index];
    if (chunk == 0U &&
        !ReadValue(
            context, context.object_chunks + sizeof(std::uintptr_t) * chunk_index, &chunk)) {
        return 0U;
    }
    context.chunks[chunk_index] = chunk;
    if (chunk == 0U) return 0U;
    std::uintptr_t object{};
    const std::uintptr_t item = chunk +
        static_cast<std::uintptr_t>(index % kObjectChunkSize) * kObjectItemStride;
    return ReadGameThreadValue(item, &object) ? object : 0U;
}

bool RefreshObjectChunks(Context& context) noexcept {
    std::uintptr_t chunks{};
    if (!ReadValue(
            context, context.object_registry + kObjectsItemsOffset, &chunks) ||
        chunks == context.object_chunks) {
        return context.object_chunks != 0U;
    }
    context.object_chunks = chunks;
    context.chunks.fill(0U);
    context.reflection_scan_cursor = 0U;
    context.reflection_scan_complete = false;
    context.exact_build_hints_checked = false;
    if (chunks != 0U) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge object registry chunks are ready");
    }
    return chunks != 0U;
}

std::uintptr_t BridgeFunctionAddress(const BridgeFunction function) noexcept {
    return g_functions[static_cast<std::size_t>(function)].load(std::memory_order_acquire);
}

void DiscoverFunction(
    Context& context, const std::uint32_t object_index,
    const std::uintptr_t object, const std::string_view name) noexcept {
    for (std::size_t index{}; index < kFunctionSpecs.size(); ++index) {
        if (name != kFunctionSpecs[index].name ||
            g_functions[index].load(std::memory_order_acquire) != 0U) {
            continue;
        }
        std::uintptr_t function_class{};
        std::uintptr_t owner_class{};
        std::uintptr_t class_class{};
        std::string function_class_name;
        std::string owner_name;
        if (!ReadValue(context, object + kObjectClassOffset, &function_class) ||
            !ReadValue(context, object + kObjectOuterOffset, &owner_class) ||
            !ReadValue(context, owner_class + kObjectClassOffset, &class_class) ||
            !ResolveObjectName(context, function_class, &function_class_name) ||
            function_class_name != "Function" ||
            !ResolveObjectName(context, owner_class, &owner_name) ||
            owner_name != kFunctionSpecs[index].owner) {
            continue;
        }
        if ((context.function_class != 0U && context.function_class != function_class) ||
            (context.class_class != 0U && context.class_class != class_class)) {
            continue;
        }
        context.function_class = function_class;
        context.class_class = class_class;
        context.function_owner_classes[index] = owner_class;
        g_functions[index].store(object, std::memory_order_release);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge function ready: " + std::string(kFunctionSpecs[index].owner) +
                "." + std::string(kFunctionSpecs[index].name) +
                " index=" + std::to_string(object_index));
    }
}

bool IsEscMenuContainer(
    const Context& context, const std::uintptr_t object,
    std::uintptr_t* page_item, std::uintptr_t* pagination,
    std::uintptr_t* menu_root) noexcept {
    if (page_item == nullptr || pagination == nullptr || menu_root == nullptr) return false;
    *page_item = 0U;
    *pagination = 0U;
    *menu_root = 0U;
    std::string class_name;
    if (!ResolveClassName(context, object, &class_name) || class_name != "WrapBox") return false;
    bool pagination_item{};
    bool pagination_scroll_box{};
    std::uintptr_t current = object;
    for (std::uint32_t depth{}; depth < 8U && current != 0U; ++depth) {
        std::string name;
        static_cast<void>(ResolveObjectName(context, current, &name));
        if (name == "BPUI_PaginationItem_C") {
            pagination_item = true;
            *page_item = current;
        }
        if (name == "PaginationScrollBox") {
            pagination_scroll_box = true;
            *pagination = current;
        }
        if (name == "BPUI_MenuExtension_C") *menu_root = current;
        std::uintptr_t outer{};
        if (!ReadValue(context, current + kObjectOuterOffset, &outer)) break;
        current = outer;
    }
    return pagination_item && pagination_scroll_box && *page_item != 0U &&
        *pagination != 0U && *menu_root != 0U;
}

bool FindPropertyOffset(
    Context& context, const std::uintptr_t struct_object,
    const std::string_view target, std::uint32_t* result) noexcept {
    if (struct_object == 0U || result == nullptr) return false;
    std::uintptr_t property{};
    if (!ReadValue(context, struct_object + kStructPropertyLinkOffset, &property)) return false;
    std::array<std::uintptr_t, 512> visited{};
    std::size_t visited_count{};
    while (property != 0U && visited_count < visited.size()) {
        if (std::find(visited.begin(), visited.begin() + visited_count, property) !=
            visited.begin() + visited_count) {
            break;
        }
        visited[visited_count++] = property;
        FNameValue property_name{};
        std::string name;
        std::int32_t offset{};
        if (ReadValue(context, property + kFieldNameOffset, &property_name) &&
            ResolveName(context.names, property_name.comparison_index, &name) &&
            ReadValue(context, property + kPropertyOffsetInternalOffset, &offset) &&
            offset >= 0) {
            if (name == target) {
                *result = static_cast<std::uint32_t>(offset);
                return true;
            }
        }
        std::uintptr_t next{};
        if (!ReadValue(context, property + kPropertyLinkNextOffset, &next)) break;
        property = next;
    }
    return false;
}

void DiscoverFeatureButtonProperties(Context& context) noexcept {
    if (context.feature_button_class == 0U) return;
    if (context.text_game_feature_name_offset == kMissingPropertyOffset && FindPropertyOffset(
            context, context.feature_button_class, "TextGameFeatureName",
            &context.text_game_feature_name_offset)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge property ready: TextGameFeatureName offset=" +
                std::to_string(context.text_game_feature_name_offset));
    }
    if (context.image_icon_offset == kMissingPropertyOffset && FindPropertyOffset(
            context, context.feature_button_class, "ImageIcon",
            &context.image_icon_offset)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge property ready: ImageIcon offset=" +
                std::to_string(context.image_icon_offset));
    }
}

void DiscoverPaginationProperties(
    Context& context, const std::uintptr_t pagination) noexcept {
    if (context.pagination_scroll_box_offset != kMissingPropertyOffset) return;
    std::uintptr_t pagination_class{};
    if (ReadValue(context, pagination + kObjectClassOffset, &pagination_class) &&
        FindPropertyOffset(
            context, pagination_class, "ScrollBox",
            &context.pagination_scroll_box_offset)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge property ready: PaginationScrollBox.ScrollBox offset=" +
                std::to_string(context.pagination_scroll_box_offset));
    }
}

void DiscoverObject(
    Context& context, const std::uint32_t index, const AnomalyGenerationHandleV1 handle,
    const std::uintptr_t object, const std::string_view name) noexcept {
    DiscoverFunction(context, index, object, name);
    if (name == "WB_SystematicGameFeatureButton_C" && context.feature_button_class == 0U) {
        std::string class_name;
        if (ResolveClassName(context, object, &class_name) &&
            class_name == "WidgetBlueprintGeneratedClass") {
            context.feature_button_class = object;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "NTE ESC bridge class ready: WB_SystematicGameFeatureButton_C");
        }
    } else if (name == "Default__WidgetBlueprintLibrary" &&
        context.widget_blueprint_library_cdo == 0U) {
        context.widget_blueprint_library_cdo = object;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge CDO ready: WidgetBlueprintLibrary");
    } else if (name == "Default__KismetTextLibrary" &&
        context.kismet_text_library_cdo == 0U) {
        context.kismet_text_library_cdo = object;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge CDO ready: KismetTextLibrary");
    } else if (name == "Default__KismetRenderingLibrary" &&
        context.kismet_rendering_library_cdo == 0U) {
        context.kismet_rendering_library_cdo = object;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge CDO ready: KismetRenderingLibrary");
    } else if (name == "WrapBoxMenuButtons") {
        std::uintptr_t page_item{};
        std::uintptr_t pagination{};
        std::uintptr_t menu_root{};
        if (IsEscMenuContainer(
                context, object, &page_item, &pagination, &menu_root)) {
            DiscoverPaginationProperties(context, pagination);
            const auto existing = std::find_if(
                context.menu_candidates.begin(), context.menu_candidates.end(),
                [&](const auto& candidate) {
                    return candidate.handle.id == handle.id &&
                        candidate.handle.generation == handle.generation;
                });
            if (existing == context.menu_candidates.end()) {
                try {
                    const std::uint64_t discovery_order = ++context.next_discovery_order;
                    context.menu_candidates.push_back(
                        {handle, object, page_item, pagination, menu_root, index,
                            discovery_order});
                    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                        "NTE ESC bridge container candidate index=" +
                            std::to_string(index) + " order=" +
                            std::to_string(discovery_order) +
                            " accepted");
                } catch (...) {
                }
            } else {
                if (existing->container != object || existing->page_item != page_item ||
                    existing->pagination != pagination) {
                    existing->discovery_order = ++context.next_discovery_order;
                }
                existing->container = object;
                existing->page_item = page_item;
                existing->pagination = pagination;
                existing->menu_root = menu_root;
                existing->index = index;
            }
        }
    }
    DiscoverFeatureButtonProperties(context);
}

void DiscoverClassDefaultObject(
    Context& context, const BridgeFunction function, const std::string_view expected_name,
    const std::string_view expected_class, std::uintptr_t* result) noexcept {
    if (result == nullptr || *result != 0U) return;
    const std::uintptr_t function_object = BridgeFunctionAddress(function);
    std::uintptr_t owner_class{};
    std::string owner_name;
    if (function_object == 0U ||
        !ReadValue(context, function_object + kObjectOuterOffset, &owner_class) ||
        !ResolveObjectName(context, owner_class, &owner_name) ||
        owner_name != expected_class) {
        return;
    }
    for (std::uintptr_t offset = 0x80U; offset <= 0x200U;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t candidate{};
        std::string name;
        std::string class_name;
        if (!ReadValue(context, owner_class + offset, &candidate) || candidate == 0U ||
            !ResolveObjectName(context, candidate, &name) || name != expected_name ||
            !ResolveClassName(context, candidate, &class_name) ||
            class_name != expected_class) {
            continue;
        }
        *result = candidate;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge CDO ready from validated class field: " +
                std::string(expected_class) + " offset=" + std::to_string(offset));
        return;
    }
}

void DiscoverClassDefaultObjects(Context& context) noexcept {
    DiscoverClassDefaultObject(
        context, BridgeFunction::Create, "Default__WidgetBlueprintLibrary",
        "WidgetBlueprintLibrary", &context.widget_blueprint_library_cdo);
    DiscoverClassDefaultObject(
        context, BridgeFunction::StringToText, "Default__KismetTextLibrary",
        "KismetTextLibrary", &context.kismet_text_library_cdo);
    DiscoverClassDefaultObject(
        context, BridgeFunction::ImportBufferAsTexture2D,
        "Default__KismetRenderingLibrary", "KismetRenderingLibrary",
        &context.kismet_rendering_library_cdo);
}

void DiscoverExactBuildObjects(Context& context) noexcept {
    if (context.exact_build_hints_checked || !ObjectsReady(context.objects) ||
        !NamesReady(context.names)) {
        return;
    }
    const std::uint32_t count = context.objects->count(context.objects->user);
    if (context.object_chunks == 0U ||
        count <= *std::ranges::max_element(kExactBuildObjectIndices)) {
        return;
    }
    context.exact_build_hints_checked = true;
    std::uint32_t validated{};
    for (const std::uint32_t index : kExactBuildObjectIndices) {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (context.objects->snapshot_at(context.objects->user, index, &snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        std::string name;
        const std::uintptr_t object = ObjectAddressAt(context, index);
        if (object == 0U || !ResolveName(context.names, snapshot.name_id, &name)) continue;
        const auto functions_before = std::count_if(
            g_functions.begin(), g_functions.end(), [](const auto& function) {
                return function.load(std::memory_order_acquire) != 0U;
            });
        const std::uintptr_t feature_class_before = context.feature_button_class;
        DiscoverObject(context, index, snapshot.handle, object, name);
        const auto functions_after = std::count_if(
            g_functions.begin(), g_functions.end(), [](const auto& function) {
                return function.load(std::memory_order_acquire) != 0U;
            });
        if (functions_after > functions_before ||
            (feature_class_before == 0U && context.feature_button_class != 0U)) {
            ++validated;
        }
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "NTE ESC bridge validated exact-build object hints=" +
            std::to_string(validated));
    DiscoverClassDefaultObjects(context);
}

bool StaticReflectionDiscoveryComplete(const Context& context) noexcept {
    return context.widget_blueprint_library_cdo != 0U &&
        context.kismet_text_library_cdo != 0U &&
        context.kismet_rendering_library_cdo != 0U &&
        std::ranges::all_of(g_functions, [](const auto& function) {
            return function.load(std::memory_order_acquire) != 0U;
        });
}

void DiscoverReflectionCandidate(
    Context& context, const std::uint32_t index,
    const std::uintptr_t object) noexcept {
    std::uintptr_t object_class{};
    if (object == 0U ||
        !ReadGameThreadValue(object + kObjectClassOffset, &object_class)) {
        return;
    }
    if (object_class == context.class_class) {
        std::string name;
        if (!ResolveObjectName(context, object, &name)) return;
        bool discovered{};
        for (std::size_t function{}; function < kFunctionSpecs.size(); ++function) {
            if (context.function_owner_classes[function] == 0U &&
                name == kFunctionSpecs[function].owner) {
                context.function_owner_classes[function] = object;
                discovered = true;
            }
        }
        if (discovered) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "NTE ESC bridge function owner ready: " + name +
                    " index=" + std::to_string(index));
        }
        return;
    }
    if (object_class != context.function_class) return;

    std::uintptr_t owner{};
    if (!ReadGameThreadValue(object + kObjectOuterOffset, &owner)) return;
    bool relevant{};
    for (std::size_t function{}; function < kFunctionSpecs.size(); ++function) {
        if (g_functions[function].load(std::memory_order_acquire) == 0U &&
            context.function_owner_classes[function] == owner) {
            relevant = true;
            break;
        }
    }
    if (!relevant) return;

    std::uint32_t name_id{};
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    std::string name;
    if (!ReadGameThreadValue(object + kObjectNameOffset, &name_id) ||
        context.objects->snapshot_at(context.objects->user, index, &snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        snapshot.name_id != name_id || !ResolveName(context.names, name_id, &name)) {
        return;
    }
    DiscoverObject(context, index, snapshot.handle, object, name);
}

void ScanObjects(Context& context) noexcept {
    if (StaticReflectionDiscoveryComplete(context)) return;
    if (!ObjectsReady(context.objects) || !NamesReady(context.names)) return;
    if (!RefreshObjectChunks(context)) return;
    DiscoverExactBuildObjects(context);
    DiscoverClassDefaultObjects(context);
    if (StaticReflectionDiscoveryComplete(context)) return;
    const std::uint32_t count = context.objects->count(context.objects->user);
    if (count == 0U) return;
    if (context.reflection_scan_complete) {
        if (context.reflection_scan_cursor >= count) return;
        context.reflection_scan_complete = false;
    }

    const auto started = std::chrono::steady_clock::now();
    std::uint32_t scanned{};
    while (context.reflection_scan_cursor < count && scanned < kObjectsPerUpdate) {
        if ((scanned & 0x3fU) == 0U) {
            if (StaticReflectionDiscoveryComplete(context)) break;
            if (std::chrono::steady_clock::now() - started >= kScanBudget) break;
        }
        const std::uint32_t index = context.reflection_scan_cursor++;
        ++scanned;
        const std::uintptr_t object = ObjectAddressAt(context, index);
        DiscoverReflectionCandidate(context, index, object);
    }
    DiscoverClassDefaultObjects(context);
    if (context.reflection_scan_cursor >= count) context.reflection_scan_complete = true;
}

bool SameHandle(
    const AnomalyGenerationHandleV1 left, const AnomalyGenerationHandleV1 right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

bool UseDefaultIconAfterFailure(
    Context& context, const AnomalyGenerationHandleV1 handle) noexcept {
    try {
        const auto found = std::find_if(
            context.icon_import_failures.begin(), context.icon_import_failures.end(),
            [&](const auto& failure) { return SameHandle(failure.handle, handle); });
        std::uint32_t attempts{};
        if (found == context.icon_import_failures.end()) {
            context.icon_import_failures.push_back({handle, 1U});
            attempts = 1U;
        } else {
            attempts = ++found->attempts;
        }
        if (attempts < kIconImportAttemptsBeforeFallback) return false;
        if (attempts == kIconImportAttemptsBeforeFallback) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "NTE ESC custom icon import failed three times; using the default icon");
        }
        return true;
    } catch (...) {
        return false;
    }
}

void ClearIconImportFailures(
    Context& context, const AnomalyGenerationHandleV1 handle) noexcept {
    std::erase_if(context.icon_import_failures, [&](const auto& failure) {
        return SameHandle(failure.handle, handle);
    });
}

bool ResolveWideLabel(const std::string_view label, std::wstring* wide) noexcept {
    if (wide == nullptr || label.empty() || label.size() > INT_MAX) return false;
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, label.data(), static_cast<int>(label.size()),
        nullptr, 0);
    if (size <= 0) return false;
    try {
        wide->resize(static_cast<std::size_t>(size));
        return MultiByteToWideChar(
                   CP_UTF8, MB_ERR_INVALID_CHARS, label.data(),
                   static_cast<int>(label.size()), wide->data(), size) == size;
    } catch (...) {
        return false;
    }
}

bool InvokeProcessEvent(
    void* object, const BridgeFunction function, void* parameters) noexcept {
    Context* const context = g_active.load(std::memory_order_acquire);
    const ProcessEventFn process_event = context == nullptr
        ? nullptr : reinterpret_cast<ProcessEventFn>(context->process_event);
    const std::uintptr_t reflected_function = BridgeFunctionAddress(function);
    if (object == nullptr || process_event == nullptr || reflected_function == 0U) return false;
    process_event(object, reinterpret_cast<void*>(reflected_function), parameters);
    return true;
}

std::int32_t GetChildrenCount(const std::uintptr_t container) noexcept {
    struct Parameters final {
        std::int32_t result{};
    } parameters;
    return InvokeProcessEvent(
               reinterpret_cast<void*>(container), BridgeFunction::GetChildrenCount, &parameters)
        ? parameters.result
        : -1;
}

std::uintptr_t GetChildAt(
    const std::uintptr_t container, const std::int32_t index) noexcept {
    struct Parameters final {
        std::int32_t index{};
        std::uint32_t padding{};
        std::uintptr_t result{};
    } parameters{index};
    static_assert(sizeof(Parameters) == 16U);
    return InvokeProcessEvent(
               reinterpret_cast<void*>(container), BridgeFunction::GetChildAt, &parameters)
        ? parameters.result
        : 0U;
}

std::int32_t GetChildIndex(
    const std::uintptr_t container, const std::uintptr_t widget) noexcept {
    struct Parameters final {
        std::uintptr_t content{};
        std::int32_t result{-1};
        std::uint32_t padding{};
    } parameters{widget};
    static_assert(sizeof(Parameters) == 16U);
    return InvokeProcessEvent(
               reinterpret_cast<void*>(container), BridgeFunction::GetChildIndex, &parameters)
        ? parameters.result
        : -1;
}

bool ApplyButtonIcon(
    Context& context, const std::uintptr_t widget,
    const DesiredButton& desired) noexcept {
    if (desired.icon_format == ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE) return true;
    if (desired.icon_format != ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_PNG ||
        desired.icon_bytes.empty() || desired.icon_bytes.size() > INT_MAX ||
        context.image_icon_offset == kMissingPropertyOffset ||
        context.kismet_rendering_library_cdo == 0U) {
        return UseDefaultIconAfterFailure(context, desired.handle);
    }
    std::uintptr_t image{};
    if (!ReadValue(context, widget + context.image_icon_offset, &image) || image == 0U) {
        return UseDefaultIconAfterFailure(context, desired.handle);
    }
    struct ImportParameters final {
        std::uintptr_t world_context{};
        UnrealByteArray buffer{};
        std::uintptr_t result{};
    } import{
        widget,
        {const_cast<std::uint8_t*>(desired.icon_bytes.data()),
            static_cast<std::int32_t>(desired.icon_bytes.size()),
            static_cast<std::int32_t>(desired.icon_bytes.size())}};
    static_assert(sizeof(ImportParameters) == 32U);
    if (!InvokeProcessEvent(
            reinterpret_cast<void*>(context.kismet_rendering_library_cdo),
            BridgeFunction::ImportBufferAsTexture2D, &import) ||
        import.result == 0U) {
        return UseDefaultIconAfterFailure(context, desired.handle);
    }
    struct SetBrushParameters final {
        std::uintptr_t texture{};
        std::uint8_t match_size{};
        std::array<std::uint8_t, 7> padding{};
    } set_brush{import.result, 0U};
    static_assert(sizeof(SetBrushParameters) == 16U);
    if (!InvokeProcessEvent(
            reinterpret_cast<void*>(image), BridgeFunction::SetBrushFromTexture, &set_brush)) {
        return UseDefaultIconAfterFailure(context, desired.handle);
    }
    ClearIconImportFailures(context, desired.handle);
    return true;
}

bool RemoveChildAt(
    const std::uintptr_t container, const std::int32_t index) noexcept {
    struct Parameters final {
        std::int32_t index{};
        std::uint8_t result{};
        std::array<std::uint8_t, 3> padding{};
    } parameters{index};
    static_assert(sizeof(Parameters) == 8U);
    return InvokeProcessEvent(
               reinterpret_cast<void*>(container), BridgeFunction::RemoveChildAt, &parameters) &&
        parameters.result != 0U;
}

std::uintptr_t CreateButtonWidget(
    Context& context, const std::uintptr_t container, const DesiredButton& desired,
    std::uint32_t* failure_stage) noexcept {
    const auto fail = [&](const std::uint32_t stage) {
        if (failure_stage != nullptr) *failure_stage = stage;
        return std::uintptr_t{};
    };
    if (context.feature_button_class == 0U ||
        context.widget_blueprint_library_cdo == 0U ||
        context.kismet_text_library_cdo == 0U ||
        context.text_game_feature_name_offset == kMissingPropertyOffset ||
        (desired.icon_format != ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE &&
            context.image_icon_offset == kMissingPropertyOffset)) {
        return fail(2U);
    }
    std::wstring wide_label;
    if (!ResolveWideLabel(desired.label, &wide_label)) return fail(3U);

    struct CreateParameters final {
        std::uintptr_t world_context{};
        std::uintptr_t widget_type{};
        std::uintptr_t owning_player{};
        std::uintptr_t result{};
    } create{container, context.feature_button_class};
    if (!InvokeProcessEvent(
            reinterpret_cast<void*>(context.widget_blueprint_library_cdo),
            BridgeFunction::Create, &create) ||
        create.result == 0U) {
        return fail(4U);
    }

    std::uintptr_t text_block{};
    if (!ReadValue(
            context, create.result + context.text_game_feature_name_offset, &text_block) ||
        text_block == 0U) {
        return fail(5U);
    }
    struct StringToTextParameters final {
        UnrealString input{};
        std::array<std::uint8_t, 16> result{};
    } conversion{{
        wide_label.data(), static_cast<std::int32_t>(wide_label.size() + 1U),
        static_cast<std::int32_t>(wide_label.size() + 1U)}};
    static_assert(sizeof(StringToTextParameters) == 32U);
    if (!InvokeProcessEvent(
            reinterpret_cast<void*>(context.kismet_text_library_cdo),
            BridgeFunction::StringToText, &conversion)) {
        return fail(6U);
    }
    struct SetTextParameters final {
        std::array<std::uint8_t, 16> text{};
    } set_text{conversion.result};
    if (!InvokeProcessEvent(
            reinterpret_cast<void*>(text_block), BridgeFunction::SetText, &set_text)) {
        return fail(7U);
    }
    if (!ApplyButtonIcon(context, create.result, desired)) return fail(11U);
    return create.result;
}

bool AppendButton(
    Context& context, const std::uintptr_t container, const DesiredButton& desired,
    WidgetBinding* binding) noexcept {
    const auto fail = [&](const std::uint32_t stage, const std::string& message) {
        if (context.append_failure_stage != stage) {
            context.append_failure_stage = stage;
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, message);
        }
        return false;
    };
    if (binding == nullptr) return fail(10U, "NTE ESC append failed: output binding missing");
    const std::int32_t before = GetChildrenCount(container);
    if (before < 0) return fail(1U, "NTE ESC append failed: GetChildrenCount");
    std::uint32_t create_failure{};
    const std::uintptr_t widget =
        CreateButtonWidget(context, container, desired, &create_failure);
    if (widget == 0U) {
        return fail(
            create_failure,
            "NTE ESC append failed: CreateButtonWidget stage=" +
                std::to_string(create_failure));
    }
    struct AddChildParameters final {
        std::uintptr_t content{};
        std::uintptr_t result{};
    } add_child{widget};
    if (!InvokeProcessEvent(
            reinterpret_cast<void*>(container), BridgeFunction::AddChild, &add_child) ||
        add_child.result == 0U) {
        return fail(8U, "NTE ESC append failed: PanelWidget.AddChild");
    }
    if (GetChildrenCount(container) != before + 1) {
        return fail(9U, "NTE ESC append failed: child count did not increase");
    }
    *binding = {desired.handle, widget, before};
    context.append_failure_stage = 0U;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "NTE ESC menu button appended at index " + std::to_string(before) +
            ": " + desired.id);
    return true;
}

void DiscoverFeatureButtonClassFromContainer(Context& context) noexcept {
    if (context.feature_button_class != 0U || context.menu_container == 0U) return;
    const std::int32_t children = GetChildrenCount(context.menu_container);
    for (std::int32_t index{}; index < children; ++index) {
        const std::uintptr_t child = GetChildAt(context.menu_container, index);
        std::uintptr_t candidate_class{};
        std::string name;
        std::string meta_class;
        if (child == 0U ||
            !ReadValue(context, child + kObjectClassOffset, &candidate_class) ||
            candidate_class == 0U ||
            !ResolveObjectName(context, candidate_class, &name) ||
            name != "WB_SystematicGameFeatureButton_C" ||
            !ResolveClassName(context, candidate_class, &meta_class) ||
            meta_class != "WidgetBlueprintGeneratedClass") {
            continue;
        }
        context.feature_button_class = candidate_class;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge class ready from validated native menu button: " +
                std::to_string(index));
        DiscoverFeatureButtonProperties(context);
        return;
    }
}

bool SelectExpectedPageDirect(Context& context) noexcept {
    if (context.expected_menu_root == 0U || context.expected_menu_page_index < 0) {
        return false;
    }
    std::string name;
    std::string class_name;
    std::uintptr_t root_class{};
    if (!ResolveObjectName(context, context.expected_menu_root, &name) ||
        name != "BPUI_MenuExtension_C" ||
        !ResolveClassName(context, context.expected_menu_root, &class_name) ||
        class_name != "BPUI_MenuExtension_C" ||
        !ReadValue(
            context, context.expected_menu_root + kObjectClassOffset, &root_class) ||
        root_class == 0U) {
        return false;
    }
    if (context.menu_pagination_offset == kMissingPropertyOffset) {
        if (!FindPropertyOffset(
                context, root_class, "PaginationScrollBox",
                &context.menu_pagination_offset)) {
            return false;
        }
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge property ready: MenuExtension.PaginationScrollBox offset=" +
                std::to_string(context.menu_pagination_offset));
    }
    std::uintptr_t pagination{};
    if (!ReadValue(
            context, context.expected_menu_root + context.menu_pagination_offset,
            &pagination) ||
        pagination == 0U || !ResolveObjectName(context, pagination, &name) ||
        name != "PaginationScrollBox") {
        return false;
    }
    DiscoverPaginationProperties(context, pagination);
    if (context.pagination_scroll_box_offset == kMissingPropertyOffset) return false;
    std::uintptr_t scroll_box{};
    if (!ReadValue(
            context, pagination + context.pagination_scroll_box_offset, &scroll_box) ||
        scroll_box == 0U || !ResolveClassName(context, scroll_box, &class_name) ||
        class_name != "HTPageScrollBox") {
        return false;
    }
    const std::uintptr_t page_item = GetChildAt(scroll_box, context.expected_menu_page_index);
    std::uintptr_t page_class{};
    if (page_item == 0U || !ResolveObjectName(context, page_item, &name) ||
        name != "BPUI_PaginationItem_C" ||
        !ReadValue(context, page_item + kObjectClassOffset, &page_class) ||
        page_class == 0U) {
        return false;
    }
    if (context.page_menu_buttons_offset == kMissingPropertyOffset) {
        if (!FindPropertyOffset(
                context, page_class, "WrapBoxMenuButtons",
                &context.page_menu_buttons_offset)) {
            return false;
        }
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge property ready: PaginationItem.WrapBoxMenuButtons offset=" +
                std::to_string(context.page_menu_buttons_offset));
    }
    std::uintptr_t container{};
    std::uintptr_t current_page_item{};
    std::uintptr_t current_pagination{};
    std::uintptr_t current_menu_root{};
    if (!ReadValue(
            context, page_item + context.page_menu_buttons_offset, &container) ||
        container == 0U || !ResolveObjectName(context, container, &name) ||
        name != "WrapBoxMenuButtons" ||
        !IsEscMenuContainer(
            context, container, &current_page_item, &current_pagination,
            &current_menu_root) ||
        current_page_item != page_item || current_pagination != pagination ||
        current_menu_root != context.expected_menu_root) {
        return false;
    }
    if (container != context.menu_container || scroll_box != context.menu_scroll_box) {
        std::scoped_lock lock(context.bridge_mutex);
        context.bindings.clear();
        context.icon_import_failures.clear();
        context.menu_container = container;
        context.menu_container_index = 0U;
        context.menu_container_handle = {};
        context.menu_scroll_box = scroll_box;
        context.menu_page_index = context.expected_menu_page_index;
        context.observed_child_count = -1;
        context.stable_container_updates = 0U;
        context.container_stable = false;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge selected expected menu page directly page=" +
                std::to_string(context.expected_menu_page_index));
    }
    return true;
}

bool SelectAttachedLastPage(Context& context) noexcept {
    if (SelectExpectedPageDirect(context)) return true;
    if (!ObjectsReady(context.objects) ||
        context.pagination_scroll_box_offset == kMissingPropertyOffset) {
        return false;
    }
    MenuCandidate selected{};
    std::uintptr_t selected_scroll_box{};
    std::int32_t selected_page_index{-1};
    bool found{};
    for (auto candidate = context.menu_candidates.begin();
         candidate != context.menu_candidates.end();) {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        std::uintptr_t current_page_item{};
        std::uintptr_t current_pagination{};
        std::uintptr_t current_menu_root{};
        if (context.objects->snapshot_by_handle(
                context.objects->user, candidate->handle, &snapshot).code !=
                ANOMALY_STATUS_V1_OK ||
            ObjectAddressAt(context, candidate->index) != candidate->container ||
            !IsEscMenuContainer(
                context, candidate->container, &current_page_item,
                &current_pagination, &current_menu_root) ||
            current_page_item != candidate->page_item ||
            current_pagination != candidate->pagination ||
            current_menu_root != candidate->menu_root) {
            candidate = context.menu_candidates.erase(candidate);
            continue;
        }
        if (context.expected_menu_root != 0U &&
            candidate->menu_root != context.expected_menu_root) {
            ++candidate;
            continue;
        }
        std::uintptr_t scroll_box{};
        std::string scroll_box_class;
        if (!ReadValue(
                context,
                candidate->pagination + context.pagination_scroll_box_offset,
                &scroll_box) ||
            scroll_box == 0U ||
            !ResolveClassName(context, scroll_box, &scroll_box_class) ||
            scroll_box_class != "HTPageScrollBox") {
            ++candidate;
            continue;
        }
        const std::int32_t page_index = GetChildIndex(scroll_box, candidate->page_item);
        if (page_index >= 0 &&
            (context.expected_menu_page_index < 0 ||
                page_index == context.expected_menu_page_index) &&
            (!found || page_index > selected_page_index ||
                (page_index == selected_page_index &&
                    candidate->discovery_order > selected.discovery_order))) {
            selected = *candidate;
            selected_scroll_box = scroll_box;
            selected_page_index = page_index;
            found = true;
        }
        ++candidate;
    }
    if (!found) return false;
    if (selected.container != context.menu_container ||
        selected_scroll_box != context.menu_scroll_box) {
        std::scoped_lock lock(context.bridge_mutex);
        context.bindings.clear();
        context.icon_import_failures.clear();
        context.menu_container = selected.container;
        context.menu_container_index = selected.index;
        context.menu_container_handle = selected.handle;
        context.menu_scroll_box = selected_scroll_box;
        context.menu_page_index = selected_page_index;
        context.observed_child_count = -1;
        context.stable_container_updates = 0U;
        context.container_stable = false;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge selected attached last page index=" +
                std::to_string(selected.index) +
                " page=" + std::to_string(selected_page_index) +
                " expected=" + std::to_string(context.expected_menu_page_index));
    }
    return true;
}

void ReconcileButtons(Context& context) noexcept {
    const auto report_stage = [&](const std::uint32_t stage, const std::string_view message) {
        if (context.reconcile_stage == stage) return;
        context.reconcile_stage = stage;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
    };
    if (!context.snapshot_provider) {
        report_stage(0U, "NTE ESC bridge waiting: button ABI unavailable");
        return;
    }
    const bool functions_ready =
        std::all_of(g_functions.begin(), g_functions.end(), [](const auto& function) {
            return function.load(std::memory_order_acquire) != 0U;
        });
    if (!functions_ready || context.widget_blueprint_library_cdo == 0U ||
        context.kismet_text_library_cdo == 0U ||
        context.kismet_rendering_library_cdo == 0U) {
        report_stage(2U, "NTE ESC bridge waiting: reflected construction data incomplete");
        return;
    }
    if (context.menu_rebuild_active) {
        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (now < context.observed_menu_page_event_ms +
                static_cast<std::uint64_t>(kMenuPageQuiescence.count())) {
            report_stage(7U, "NTE ESC bridge waiting: menu page events are still arriving");
            return;
        }
    }
    if (!SelectAttachedLastPage(context)) {
        report_stage(1U, "NTE ESC bridge waiting: attached last menu page not found");
        return;
    }
    DiscoverFeatureButtonClassFromContainer(context);
    if (context.feature_button_class == 0U ||
        context.text_game_feature_name_offset == kMissingPropertyOffset ||
        context.image_icon_offset == kMissingPropertyOffset) {
        report_stage(2U, "NTE ESC bridge waiting: reflected construction data incomplete");
        return;
    }

    std::vector<DesiredButton> desired;
    try {
        auto snapshots = context.snapshot_provider();
        desired.reserve(snapshots.size());
        for (auto& snapshot : snapshots) {
            desired.push_back({snapshot.handle, std::move(snapshot.id),
                std::move(snapshot.label), snapshot.icon_format,
                std::move(snapshot.icon_bytes)});
        }
    } catch (...) {
        report_stage(3U, "NTE ESC bridge waiting: button enumeration failed");
        return;
    }
    std::scoped_lock lock(context.bridge_mutex);
    std::int32_t children = GetChildrenCount(context.menu_container);
    if (children < 0) {
        report_stage(1U, "NTE ESC bridge waiting: selected menu page is unavailable");
        return;
    }
    const bool binding_lost = std::any_of(
        context.bindings.begin(), context.bindings.end(), [&](const auto& binding) {
            return binding.child_index < 0 || binding.child_index >= children ||
                GetChildAt(context.menu_container, binding.child_index) != binding.widget;
        });
    if (binding_lost) {
        context.bindings.clear();
        context.container_stable = false;
        context.observed_child_count = -1;
        context.stable_container_updates = 0U;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge observed a rebuilt menu page; waiting for stability");
    }
    for (auto binding = context.bindings.begin(); binding != context.bindings.end();) {
        const bool retained = std::any_of(desired.begin(), desired.end(), [&](const auto& entry) {
            return SameHandle(entry.handle, binding->handle);
        });
        if (retained) {
            ++binding;
        } else {
            const std::int32_t removed_index = binding->child_index;
            if (GetChildAt(context.menu_container, removed_index) == binding->widget &&
                RemoveChildAt(context.menu_container, removed_index)) {
                --children;
                for (auto& entry : context.bindings) {
                    if (entry.child_index > removed_index) --entry.child_index;
                }
            }
            binding = context.bindings.erase(binding);
        }
    }
    if (!context.container_stable) {
        if (context.observed_child_count == children) {
            ++context.stable_container_updates;
        } else {
            context.observed_child_count = children;
            context.stable_container_updates = 1U;
        }
        if (context.stable_container_updates < kStableContainerUpdates) {
            report_stage(6U, "NTE ESC bridge waiting: last menu page is still changing");
            return;
        }
        context.container_stable = true;
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "NTE ESC bridge last menu page stabilized with children=" +
                std::to_string(children));
    }
    bool append_failed{};
    for (const DesiredButton& entry : desired) {
        const bool exists = std::any_of(
            context.bindings.begin(), context.bindings.end(), [&](const auto& binding) {
                return SameHandle(entry.handle, binding.handle);
            });
        if (exists) continue;
        WidgetBinding binding;
        if (AppendButton(context, context.menu_container, entry, &binding)) {
            context.bindings.push_back(binding);
        } else {
            append_failed = true;
        }
    }
    report_stage(
        append_failed ? 5U : 4U,
        append_failed ? "NTE ESC bridge failed to append button widget"
                      : "NTE ESC bridge ready to reconcile buttons");
    context.reconciliation_complete = !append_failed &&
        std::all_of(desired.begin(), desired.end(), [&](const auto& entry) {
            return std::any_of(
                context.bindings.begin(), context.bindings.end(), [&](const auto& binding) {
                    return SameHandle(entry.handle, binding.handle);
                });
        });
    if (context.reconciliation_complete) context.menu_rebuild_active = false;
}

std::uintptr_t ReadPointerDirect(const std::uintptr_t address) noexcept {
    std::uintptr_t value{};
    if (address != 0U) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool QueueClick(Context& context, const std::uintptr_t object) noexcept {
    std::array<std::uintptr_t, 5> candidates{};
    candidates[0] = object;
    for (std::size_t index = 1U; index < candidates.size(); ++index) {
        if (candidates[index - 1U] == 0U) break;
        candidates[index] = ReadPointerDirect(candidates[index - 1U] + kObjectOuterOffset);
    }
    std::scoped_lock lock(context.bridge_mutex);
    const auto binding = std::find_if(
        context.bindings.begin(), context.bindings.end(), [&](const auto& entry) {
            return std::find(candidates.begin(), candidates.end(), entry.widget) != candidates.end();
        });
    if (binding == context.bindings.end()) return false;
    if (std::none_of(
            context.pending_clicks.begin(), context.pending_clicks.end(), [&](const auto handle) {
                return SameHandle(handle, binding->handle);
            })) {
        context.pending_clicks.push_back(binding->handle);
    }
    return true;
}

void PublishMenuPageAdded(
    Context& context, void* const object, const std::int32_t menu_page_index) noexcept {
    const auto event_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    context.last_menu_page_index.store(menu_page_index, std::memory_order_relaxed);
    context.last_menu_root.store(
        reinterpret_cast<std::uintptr_t>(object), std::memory_order_relaxed);
    context.last_menu_page_event_ms.store(event_ms, std::memory_order_relaxed);
    context.menu_rebuild_sequence.fetch_add(1U, std::memory_order_release);
}

void ANOMALY_CALL AddMenuPageDetour(void* object, const std::int32_t menu_page_index) noexcept {
    const AddMenuPageFn original =
        g_add_menu_page_original.load(std::memory_order_acquire);
    if (original == nullptr) return;
    HookManager* const hooks = g_hook_manager.load(std::memory_order_acquire);
    Context* const context = g_active.load(std::memory_order_acquire);
    if (hooks == nullptr || context == nullptr ||
        context->stopping.load(std::memory_order_acquire)) {
        original(object, menu_page_index);
        return;
    }
    auto lease = hooks->AcquireCallback(
        kHookOwner, kHookGeneration, reinterpret_cast<void*>(context->add_menu_page));
    if (!lease) {
        original(object, menu_page_index);
        return;
    }
    original(object, menu_page_index);
    if (menu_page_index >= 0) PublishMenuPageAdded(*context, object, menu_page_index);
}

void ANOMALY_CALL ButtonClickedDetour(void* object) noexcept {
    const ButtonClickedFn original =
        g_button_clicked_original.load(std::memory_order_acquire);
    if (original == nullptr) return;
    HookManager* const hooks = g_hook_manager.load(std::memory_order_acquire);
    Context* const context = g_active.load(std::memory_order_acquire);
    if (hooks == nullptr || context == nullptr ||
        context->stopping.load(std::memory_order_acquire)) {
        original(object);
        return;
    }
    auto lease = hooks->AcquireCallback(
        kHookOwner, kHookGeneration, reinterpret_cast<void*>(context->button_clicked));
    if (!lease) {
        original(object);
        return;
    }
    static_cast<void>(QueueClick(*context, reinterpret_cast<std::uintptr_t>(object)));
    original(object);
}

void DrainClicks(Context& context) noexcept {
    std::vector<AnomalyGenerationHandleV1> clicks;
    {
        std::scoped_lock lock(context.bridge_mutex);
        clicks.swap(context.pending_clicks);
    }
    if (!context.invoke_button) return;
    for (const AnomalyGenerationHandleV1 handle : clicks) {
        try {
            context.invoke_button(handle);
        } catch (...) {
        }
    }
}

void RefreshServices(Context& context) noexcept {
    context.names = static_cast<const AnomalyUe5NamesServiceV1*>(
        ProcessAdapterServices().Query(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID,
            ANOMALY_UE5_NAMES_SERVICE_V1_VERSION, false));
    context.objects = static_cast<const AnomalyUe5ObjectsServiceV1*>(
        ProcessAdapterServices().Query(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
            ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION, false));
}

void ObserveMenuRebuild(Context& context) noexcept {
    const std::uint64_t sequence =
        context.menu_rebuild_sequence.load(std::memory_order_acquire);
    if (sequence == context.observed_menu_rebuild_sequence) return;
    context.observed_menu_rebuild_sequence = sequence;
    const std::int32_t latest_page_index =
        context.last_menu_page_index.load(std::memory_order_relaxed);
    const std::uintptr_t latest_menu_root =
        context.last_menu_root.load(std::memory_order_relaxed);
    const std::uint64_t latest_event_ms =
        context.last_menu_page_event_ms.load(std::memory_order_relaxed);
    const bool new_event_batch = !context.menu_rebuild_active ||
        latest_menu_root != context.expected_menu_root ||
        (context.observed_menu_page_event_ms != 0U &&
            latest_event_ms > context.observed_menu_page_event_ms +
                    static_cast<std::uint64_t>(kMenuPageQuiescence.count()));
    context.expected_menu_page_index = latest_page_index;
    context.expected_menu_root = latest_menu_root;
    context.observed_menu_page_event_ms = latest_event_ms;
    context.reconciliation_complete = false;
    context.reconcile_stage = kMissingPropertyOffset;
    if (new_event_batch) {
        context.menu_rebuild_active = true;
        context.feature_button_class = 0U;
        context.append_failure_stage = 0U;
        context.menu_container = 0U;
        context.menu_container_index = 0U;
        context.menu_container_handle = {};
        context.menu_scroll_box = 0U;
        context.menu_page_index = -1;
        context.observed_child_count = -1;
        context.stable_container_updates = 0U;
        context.container_stable = false;
        std::scoped_lock lock(context.bridge_mutex);
        context.menu_candidates.clear();
        context.bindings.clear();
        context.icon_import_failures.clear();
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "NTE ESC bridge observed menu page rebuild sequence=" +
            std::to_string(sequence) +
            " expected_page=" + std::to_string(context.expected_menu_page_index) +
            " new_batch=" + (new_event_batch ? "true" : "false"));
}

void Update(Context& context) noexcept {
    if (context.stopping.load(std::memory_order_acquire)) return;
    DrainClicks(context);
    if (!NamesReady(context.names) || !ObjectsReady(context.objects)) RefreshServices(context);
    ObserveMenuRebuild(context);
    ScanObjects(context);
    const auto now = std::chrono::steady_clock::now();
    const bool fast_reconcile = context.menu_rebuild_active ||
        (!context.reconciliation_complete && !context.menu_candidates.empty());
    const bool retry_ready = context.append_failure_stage == 0U ||
        now >= context.next_registry_poll;
    if (fast_reconcile && retry_ready) {
        ReconcileButtons(context);
        context.next_registry_poll = context.append_failure_stage == 0U
            ? (context.reconciliation_complete
                    ? now + kRegistryPollInterval
                    : std::chrono::steady_clock::time_point{})
            : now + kAppendRetryInterval;
        return;
    }
    if (now >= context.next_registry_poll) {
        ReconcileButtons(context);
        context.next_registry_poll = now +
            (context.append_failure_stage == 0U
                ? kRegistryPollInterval : kAppendRetryInterval);
    }
}

void Reset(Context& context) noexcept {
    context.object_chunks = 0U;
    context.chunks.fill(0U);
    static_cast<void>(ReadValue(
        context, context.object_registry + kObjectsItemsOffset, &context.object_chunks));
    context.reflection_scan_cursor = 0U;
    context.reflection_scan_complete = false;
    context.exact_build_hints_checked = false;
    context.function_class = 0U;
    context.class_class = 0U;
    context.function_owner_classes.fill(0U);
    context.feature_button_class = 0U;
    context.widget_blueprint_library_cdo = 0U;
    context.kismet_text_library_cdo = 0U;
    context.kismet_rendering_library_cdo = 0U;
    context.text_game_feature_name_offset = kMissingPropertyOffset;
    context.image_icon_offset = kMissingPropertyOffset;
    context.menu_pagination_offset = kMissingPropertyOffset;
    context.pagination_scroll_box_offset = kMissingPropertyOffset;
    context.page_menu_buttons_offset = kMissingPropertyOffset;
    context.menu_container = 0U;
    context.menu_container_index = 0U;
    context.menu_scroll_box = 0U;
    context.menu_page_index = -1;
    context.observed_child_count = -1;
    context.stable_container_updates = 0U;
    context.container_stable = false;
    context.next_discovery_order = 0U;
    context.menu_rebuild_sequence.store(0U, std::memory_order_release);
    context.last_menu_page_index.store(-1, std::memory_order_release);
    context.last_menu_root.store(0U, std::memory_order_release);
    context.last_menu_page_event_ms.store(0U, std::memory_order_release);
    context.observed_menu_rebuild_sequence = 0U;
    context.expected_menu_page_index = -1;
    context.expected_menu_root = 0U;
    context.observed_menu_page_event_ms = 0U;
    context.menu_rebuild_active = false;
    context.reconciliation_complete = false;
    context.next_registry_poll = {};
    context.reconcile_stage = kMissingPropertyOffset;
    context.append_failure_stage = 0U;
    context.menu_container_handle = {};
    {
        std::scoped_lock lock(context.bridge_mutex);
        context.menu_candidates.clear();
        context.bindings.clear();
        context.icon_import_failures.clear();
        context.pending_clicks.clear();
    }
    for (auto& function : g_functions) function.store(0U, std::memory_order_release);
}

}  // namespace

class NteEscMenuBridge::Impl final {
public:
    Impl(
        CoreMemoryServices memory_services,
        ProfileResolutionSnapshot resolution,
        SnapshotProvider snapshot_provider,
        InvokeButton invoke_button,
        Logger logger) {
        context_.memory_services = NormalizeCoreMemoryServices(std::move(memory_services));
        context_.snapshot_provider = std::move(snapshot_provider);
        context_.invoke_button = std::move(invoke_button);
        context_.logger = std::move(logger);
        const auto* process_event = resolution.FindSymbol("ue5.ProcessEvent");
        const auto* add_menu_page =
            resolution.FindSymbol("nte.HTUI_MenuExtension.AddMenuPage");
        const auto* button_clicked =
            resolution.FindSymbol("nte.CommonButtonBase.BP_OnClicked");
        const auto* objects = resolution.FindSymbol("ue5.GObjects");
        if (resolution.FeatureAvailable("nte.esc-menu-button") &&
            process_event != nullptr && process_event->Available() &&
            add_menu_page != nullptr && add_menu_page->Available() &&
            button_clicked != nullptr && button_clicked->Available()) {
            context_.process_event = process_event->address;
            context_.add_menu_page = add_menu_page->address;
            context_.button_clicked = button_clicked->address;
        }
        if (objects != nullptr && objects->Available()) {
            context_.object_registry = objects->address;
        }
        RefreshServices(context_);
        Reset(context_);
    }

    ~Impl() = default;

    [[nodiscard]] bool Start() {
        std::scoped_lock process_lock(g_process_mutex);
        if (context_.process_event == 0U || context_.add_menu_page == 0U ||
            context_.button_clicked == 0U || context_.object_registry == 0U ||
            !context_.snapshot_provider || !context_.invoke_button || context_.hooks != nullptr ||
            g_active.load(std::memory_order_acquire) != nullptr) {
            return false;
        }
        context_.hooks = std::make_unique<HookManager>(CreateMinHookBackend());
        void* add_menu_page_original{};
        void* button_clicked_original{};
        if (!context_.hooks->Create(
                std::string(kHookOwner), kHookGeneration, "add-menu-page",
                reinterpret_cast<void*>(context_.add_menu_page),
                reinterpret_cast<void*>(&AddMenuPageDetour), &add_menu_page_original) ||
            add_menu_page_original == nullptr ||
            !context_.hooks->Create(
                std::string(kHookOwner), kHookGeneration, "button-clicked",
                reinterpret_cast<void*>(context_.button_clicked),
                reinterpret_cast<void*>(&ButtonClickedDetour), &button_clicked_original) ||
            button_clicked_original == nullptr) {
            static_cast<void>(
                context_.hooks->RemoveOwner(kHookOwner, kHookGeneration));
            context_.hooks.reset();
            return false;
        }
        g_add_menu_page_original.store(
            reinterpret_cast<AddMenuPageFn>(add_menu_page_original),
            std::memory_order_release);
        g_button_clicked_original.store(
            reinterpret_cast<ButtonClickedFn>(button_clicked_original),
            std::memory_order_release);
        g_hook_manager.store(context_.hooks.get(), std::memory_order_release);
        context_.stopping.store(false, std::memory_order_release);
        g_active.store(&context_, std::memory_order_release);
        if (!context_.hooks->EnableOwner(kHookOwner, kHookGeneration)) {
            g_active.store(nullptr, std::memory_order_release);
            context_.stopping.store(true, std::memory_order_release);
            static_cast<void>(context_.hooks->RemoveOwner(kHookOwner, kHookGeneration));
            g_hook_manager.store(nullptr, std::memory_order_release);
            g_add_menu_page_original.store(nullptr, std::memory_order_release);
            g_button_clicked_original.store(nullptr, std::memory_order_release);
            context_.hooks.reset();
            return false;
        }
        Log(context_, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "NTE ESC menu ABI bridge started");
        return true;
    }

    void Update() noexcept { ::anomaly::Update(context_); }

    [[nodiscard]] bool Stop(const std::chrono::milliseconds timeout) noexcept {
        std::unique_lock process_lock(g_process_mutex);
        if (context_.hooks == nullptr) return true;
        context_.stopping.store(true, std::memory_order_release);
        static_cast<void>(context_.hooks->DisableOwner(kHookOwner, kHookGeneration));
        Context* expected = &context_;
        static_cast<void>(g_active.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel));
        process_lock.unlock();
        if (!context_.hooks->RemoveOwner(kHookOwner, kHookGeneration, timeout)) return false;
        process_lock.lock();
        g_hook_manager.store(nullptr, std::memory_order_release);
        g_add_menu_page_original.store(nullptr, std::memory_order_release);
        g_button_clicked_original.store(nullptr, std::memory_order_release);
        context_.hooks.reset();
        context_.names = nullptr;
        context_.objects = nullptr;
        {
            std::scoped_lock bridge_lock(context_.bridge_mutex);
            context_.menu_candidates.clear();
            context_.bindings.clear();
            context_.icon_import_failures.clear();
            context_.pending_clicks.clear();
        }
        return true;
    }

    [[nodiscard]] bool Started() const noexcept {
        return context_.hooks != nullptr &&
            !context_.stopping.load(std::memory_order_acquire);
    }

private:
    Context context_;
};

NteEscMenuBridge::NteEscMenuBridge(
    CoreMemoryServices memory_services,
    ProfileResolutionSnapshot resolution,
    SnapshotProvider snapshot_provider,
    InvokeButton invoke_button,
    Logger logger)
    : impl_(std::make_unique<Impl>(
          std::move(memory_services), std::move(resolution),
          std::move(snapshot_provider), std::move(invoke_button), std::move(logger))) {}

NteEscMenuBridge::~NteEscMenuBridge() {
    if (impl_ != nullptr && !impl_->Stop(std::chrono::milliseconds::zero())) {
        static_cast<void>(impl_.release());
    }
}

bool NteEscMenuBridge::Start() { return impl_->Start(); }
void NteEscMenuBridge::Update(double) noexcept { impl_->Update(); }
bool NteEscMenuBridge::Stop(const std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}
bool NteEscMenuBridge::Started() const noexcept { return impl_->Started(); }

}  // namespace anomaly
