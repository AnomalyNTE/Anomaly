#include "anomaly/sdk/anomaly_sdk.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <io.h>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32) || !defined(_WIN64)
#error "The SDK ABI snapshot is defined for Windows x64"
#endif

namespace {

struct Field final {
    std::string_view name;
    std::size_t offset;
};

void AppendJsonString(std::string& output, const std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"': output.append("\\\""); break;
        case '\\': output.append("\\\\"); break;
        case '\b': output.append("\\b"); break;
        case '\f': output.append("\\f"); break;
        case '\n': output.append("\\n"); break;
        case '\r': output.append("\\r"); break;
        case '\t': output.append("\\t"); break;
        default: {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20u) {
                output.append("\\u00");
                output.push_back(hex[(byte >> 4u) & 0x0fu]);
                output.push_back(hex[byte & 0x0fu]);
            } else {
                output.push_back(character);
            }
            break;
        }
        }
    }
    output.push_back('"');
}

void AppendUnsigned(std::string& output, const std::uint64_t value) {
    output.append(std::to_string(value));
}

void AppendStruct(
    std::string& output,
    const std::string_view name,
    const std::size_t size,
    const std::size_t alignment,
    const std::initializer_list<Field> fields,
    const bool last) {
    output.append("    ");
    AppendJsonString(output, name);
    output.append(": {\n      \"size\": ");
    AppendUnsigned(output, size);
    output.append(",\n      \"alignment\": ");
    AppendUnsigned(output, alignment);
    output.append(",\n      \"offsets\": {\n");
    auto iterator = fields.begin();
    while (iterator != fields.end()) {
        output.append("        ");
        AppendJsonString(output, iterator->name);
        output.append(": ");
        AppendUnsigned(output, iterator->offset);
        ++iterator;
        output.append(iterator == fields.end() ? "\n" : ",\n");
    }
    output.append("      }\n    }");
    output.append(last ? "\n" : ",\n");
}

void AppendEnum(
    std::string& output,
    const std::string_view name,
    const std::size_t size,
    const std::size_t alignment,
    const std::initializer_list<std::pair<std::string_view, std::int64_t>> values,
    const bool last) {
    output.append("    ");
    AppendJsonString(output, name);
    output.append(": {\n      \"size\": ");
    AppendUnsigned(output, size);
    output.append(",\n      \"alignment\": ");
    AppendUnsigned(output, alignment);
    output.append(",\n      \"values\": {\n");
    auto iterator = values.begin();
    while (iterator != values.end()) {
        output.append("        ");
        AppendJsonString(output, iterator->first);
        output.append(": ");
        output.append(std::to_string(iterator->second));
        ++iterator;
        output.append(iterator == values.end() ? "\n" : ",\n");
    }
    output.append("      }\n    }");
    output.append(last ? "\n" : ",\n");
}

void AppendService(
    std::string& output,
    const std::string_view id,
    const std::uint32_t version,
    const std::string_view table,
    const bool last) {
    output.append("    { \"id\": ");
    AppendJsonString(output, id);
    output.append(", \"version\": ");
    AppendUnsigned(output, version);
    output.append(", \"table\": ");
    AppendJsonString(output, table);
    output.append(last ? " }\n" : " },\n");
}

std::string BuildSnapshot() {
    static_assert(sizeof(void*) == 8u);
    static_assert(sizeof(std::size_t) == 8u);
    static_assert(sizeof(std::uintptr_t) == 8u);

    std::string output;
    output.reserve(12'000u);
    output.append("{\n");
    output.append("  \"schema_version\": 1,\n");
    output.append("  \"target\": {\n");
    output.append("    \"os\": \"windows\",\n");
    output.append("    \"architecture\": \"x86_64\",\n");
    output.append("    \"calling_convention\": \"cdecl\",\n");
    output.append("    \"pointer_size\": 8,\n");
    output.append("    \"size_t_size\": 8\n");
    output.append("  },\n");
    output.append("  \"constants\": {\n");
    output.append("    \"ANOMALY_SDK_VERSION_MAJOR\": ");
    AppendUnsigned(output, ANOMALY_SDK_VERSION_MAJOR);
    output.append(",\n    \"ANOMALY_SDK_VERSION_MINOR\": ");
    AppendUnsigned(output, ANOMALY_SDK_VERSION_MINOR);
    output.append(",\n    \"ANOMALY_SDK_VERSION_PATCH\": ");
    AppendUnsigned(output, ANOMALY_SDK_VERSION_PATCH);
    output.append(",\n    \"ANOMALY_SDK_VERSION_STRING\": ");
    AppendJsonString(output, ANOMALY_SDK_VERSION_STRING);
    output.append(",\n    \"ANOMALY_PLUGIN_API_V1_MAJOR\": ");
    AppendUnsigned(output, ANOMALY_PLUGIN_API_V1_MAJOR);
    output.append(",\n    \"ANOMALY_PLUGIN_API_V1_MINOR\": ");
    AppendUnsigned(output, ANOMALY_PLUGIN_API_V1_MINOR);
    output.append(",\n    \"ANOMALY_PLUGIN_V1_ENTRY_NAME\": ");
    AppendJsonString(output, ANOMALY_PLUGIN_V1_ENTRY_NAME);
    output.append(",\n    \"ANOMALY_CORE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_CORE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_CONFIG_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_STORAGE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_STORAGE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_SCHEDULER_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_IPC_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_IPC_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_WEBSOCKET_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_WEBSOCKET_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_IPC_SCHEMA_HASH_V1_SIZE\": ");
    AppendUnsigned(output, ANOMALY_IPC_SCHEMA_HASH_V1_SIZE);
    output.append(",\n    \"ANOMALY_COMMANDS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_COMMANDS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_SIGNATURE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_HOOK_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_HOOK_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_PATCH_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_PATCH_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UI_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UI_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_WINDOW_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_WINDOW_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_FONT_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_FONT_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_TEXTURE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_TEXTURE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_INPUT_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_INPUT_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_LOCALIZATION_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_LOCALIZATION_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UE5_BUILD_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UE5_BUILD_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UE5_AHUD_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UE5_AHUD_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UE5_NAMES_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UE5_WORLD_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UE5_WORLD_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_BUILD_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_SESSION_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES\": ");
    AppendUnsigned(output, ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES);
    output.append(",\n    \"ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES\": ");
    AppendUnsigned(output, ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES);
    output.append(",\n    \"ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_METRICS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_METRICS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY\": ");
    AppendUnsigned(output, ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY);
    output.append(",\n    \"ANOMALY_NTE_METRICS_V1_VALID\": ");
    AppendUnsigned(output, ANOMALY_NTE_METRICS_V1_VALID);
    output.append(",\n    \"ANOMALY_NTE_PICKUP_V1_NONE\": ");
    AppendUnsigned(output, ANOMALY_NTE_PICKUP_V1_NONE);
    output.append(",\n    \"ANOMALY_NTE_PICKUP_V1_VALID\": ");
    AppendUnsigned(output, ANOMALY_NTE_PICKUP_V1_VALID);
    output.append(",\n    \"ANOMALY_NTE_PICKUP_V1_CHECKING_FLAG\": ");
    AppendUnsigned(output, ANOMALY_NTE_PICKUP_V1_CHECKING_FLAG);
    output.append(",\n    \"ANOMALY_NTE_PICKUP_V1_HAS_UNCONFIRMED\": ");
    AppendUnsigned(output, ANOMALY_NTE_PICKUP_V1_HAS_UNCONFIRMED);
    output.append("\n  },\n");

    output.append("  \"enums\": {\n");
    AppendEnum(
        output,
        "AnomalyStatusCodeV1",
        sizeof(AnomalyStatusCodeV1),
        alignof(AnomalyStatusCodeV1),
        {{"ANOMALY_STATUS_V1_OK", ANOMALY_STATUS_V1_OK},
         {"ANOMALY_STATUS_V1_INVALID_ARGUMENT", ANOMALY_STATUS_V1_INVALID_ARGUMENT},
         {"ANOMALY_STATUS_V1_UNAVAILABLE", ANOMALY_STATUS_V1_UNAVAILABLE},
         {"ANOMALY_STATUS_V1_NOT_FOUND", ANOMALY_STATUS_V1_NOT_FOUND},
         {"ANOMALY_STATUS_V1_BUFFER_TOO_SMALL", ANOMALY_STATUS_V1_BUFFER_TOO_SMALL},
         {"ANOMALY_STATUS_V1_FAILED", ANOMALY_STATUS_V1_FAILED},
         {"ANOMALY_STATUS_V1_TIMEOUT", ANOMALY_STATUS_V1_TIMEOUT},
         {"ANOMALY_STATUS_V1_PERMISSION_DENIED", ANOMALY_STATUS_V1_PERMISSION_DENIED},
         {"ANOMALY_STATUS_V1_CONFLICT", ANOMALY_STATUS_V1_CONFLICT},
         {"ANOMALY_STATUS_V1_CANCELLED", ANOMALY_STATUS_V1_CANCELLED}},
        false);
    AppendEnum(
        output,
        "AnomalyFeatureStateV1",
        sizeof(AnomalyFeatureStateV1),
        alignof(AnomalyFeatureStateV1),
        {{"ANOMALY_FEATURE_V1_UNAVAILABLE", ANOMALY_FEATURE_V1_UNAVAILABLE},
         {"ANOMALY_FEATURE_V1_AVAILABLE", ANOMALY_FEATURE_V1_AVAILABLE}},
        false);
    AppendEnum(
        output,
        "AnomalyUe5AhudFrameFlagsV1",
        sizeof(AnomalyUe5AhudFrameFlagsV1),
        alignof(AnomalyUe5AhudFrameFlagsV1),
        {{"ANOMALY_UE5_AHUD_FRAME_V1_NONE", ANOMALY_UE5_AHUD_FRAME_V1_NONE}},
        false);
    AppendEnum(
        output,
        "AnomalyNteSessionStateV1",
        sizeof(AnomalyNteSessionStateV1),
        alignof(AnomalyNteSessionStateV1),
        {{"ANOMALY_NTE_SESSION_V1_UNKNOWN", ANOMALY_NTE_SESSION_V1_UNKNOWN},
         {"ANOMALY_NTE_SESSION_V1_LOADING", ANOMALY_NTE_SESSION_V1_LOADING},
         {"ANOMALY_NTE_SESSION_V1_WORLD_READY", ANOMALY_NTE_SESSION_V1_WORLD_READY}},
        false);
    AppendEnum(
        output,
        "AnomalyNteSessionEventKindV1",
        sizeof(AnomalyNteSessionEventKindV1),
        alignof(AnomalyNteSessionEventKindV1),
        {{"ANOMALY_NTE_SESSION_EVENT_V1_NONE", ANOMALY_NTE_SESSION_EVENT_V1_NONE},
         {"ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY", ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY},
         {"ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED", ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED},
         {"ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE", ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE}},
        false);
    AppendEnum(
        output,
        "AnomalyNteMapLandmarkFlagsV1",
        sizeof(AnomalyNteMapLandmarkFlagsV1),
        alignof(AnomalyNteMapLandmarkFlagsV1),
        {{"ANOMALY_NTE_MAP_LANDMARK_V1_VALID", ANOMALY_NTE_MAP_LANDMARK_V1_VALID},
         {"ANOMALY_NTE_MAP_LANDMARK_V1_DESTINATION_OVERRIDDEN",
             ANOMALY_NTE_MAP_LANDMARK_V1_DESTINATION_OVERRIDDEN}},
        false);
    AppendEnum(
        output,
        "AnomalyNteMapLandmarkTransferModeV1",
        sizeof(AnomalyNteMapLandmarkTransferModeV1),
        alignof(AnomalyNteMapLandmarkTransferModeV1),
        {{"ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL",
             ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL},
         {"ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_SELLING_INDULGENCES",
             ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_SELLING_INDULGENCES}},
        false);
    AppendEnum(
        output,
        "AnomalyNteEscMenuButtonFlagsV1",
        sizeof(AnomalyNteEscMenuButtonFlagsV1),
        alignof(AnomalyNteEscMenuButtonFlagsV1),
        {{"ANOMALY_NTE_ESC_MENU_BUTTON_V1_NONE", ANOMALY_NTE_ESC_MENU_BUTTON_V1_NONE}},
        false);
    AppendEnum(
        output,
        "AnomalyNteEscMenuButtonResultV1",
        sizeof(AnomalyNteEscMenuButtonResultV1),
        alignof(AnomalyNteEscMenuButtonResultV1),
        {{"ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_NONE",
             ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_NONE},
         {"ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_EXPAND_ANOMALY",
             ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_EXPAND_ANOMALY}},
        false);
    AppendEnum(
        output,
        "AnomalyNteEscMenuButtonIconFormatV1",
        sizeof(AnomalyNteEscMenuButtonIconFormatV1),
        alignof(AnomalyNteEscMenuButtonIconFormatV1),
        {{"ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE",
             ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE},
         {"ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_PNG",
             ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_PNG}},
        false);
    AppendEnum(
        output,
        "AnomalyEspBoxFlagsV1",
        sizeof(AnomalyEspBoxFlagsV1),
        alignof(AnomalyEspBoxFlagsV1),
        {{"ANOMALY_ESP_BOX_V1_NONE", ANOMALY_ESP_BOX_V1_NONE},
         {"ANOMALY_ESP_BOX_V1_OUTLINE", ANOMALY_ESP_BOX_V1_OUTLINE}},
        false);
    AppendEnum(
        output,
        "AnomalyNtePickupStateV1",
        sizeof(AnomalyNtePickupStateV1),
        alignof(AnomalyNtePickupStateV1),
        {{"ANOMALY_NTE_PICKUP_V1_IDLE", ANOMALY_NTE_PICKUP_V1_IDLE},
         {"ANOMALY_NTE_PICKUP_V1_QUEUED", ANOMALY_NTE_PICKUP_V1_QUEUED},
         {"ANOMALY_NTE_PICKUP_V1_CHECKING", ANOMALY_NTE_PICKUP_V1_CHECKING},
         {"ANOMALY_NTE_PICKUP_V1_COMPLETE", ANOMALY_NTE_PICKUP_V1_COMPLETE}},
        false);
    AppendEnum(
        output,
        "AnomalyNteEntityFlagsV1",
        sizeof(AnomalyNteEntityFlagsV1),
        alignof(AnomalyNteEntityFlagsV1),
        {{"ANOMALY_NTE_ENTITY_V1_NONE", ANOMALY_NTE_ENTITY_V1_NONE},
         {"ANOMALY_NTE_ENTITY_V1_STATIC", ANOMALY_NTE_ENTITY_V1_STATIC},
         {"ANOMALY_NTE_ENTITY_V1_STATIONARY", ANOMALY_NTE_ENTITY_V1_STATIONARY},
         {"ANOMALY_NTE_ENTITY_V1_MOVABLE", ANOMALY_NTE_ENTITY_V1_MOVABLE},
         {"ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER", ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER}},
        false);
    AppendEnum(
        output,
        "AnomalyNotificationSeverityV1",
        sizeof(AnomalyNotificationSeverityV1),
        alignof(AnomalyNotificationSeverityV1),
        {{"ANOMALY_NOTIFICATION_V1_INFO", ANOMALY_NOTIFICATION_V1_INFO},
         {"ANOMALY_NOTIFICATION_V1_WARNING", ANOMALY_NOTIFICATION_V1_WARNING},
         {"ANOMALY_NOTIFICATION_V1_ERROR", ANOMALY_NOTIFICATION_V1_ERROR}},
        false);
    AppendEnum(
        output,
        "AnomalyHookKindV1",
        sizeof(AnomalyHookKindV1),
        alignof(AnomalyHookKindV1),
        {{"ANOMALY_HOOK_V1_FUNCTION", ANOMALY_HOOK_V1_FUNCTION},
         {"ANOMALY_HOOK_V1_IAT", ANOMALY_HOOK_V1_IAT},
         {"ANOMALY_HOOK_V1_EXPORT", ANOMALY_HOOK_V1_EXPORT},
         {"ANOMALY_HOOK_V1_VTABLE", ANOMALY_HOOK_V1_VTABLE}},
        false);
    AppendEnum(
        output,
        "AnomalyUiFrameStateV1",
        sizeof(AnomalyUiFrameStateV1),
        alignof(AnomalyUiFrameStateV1),
        {{"ANOMALY_UI_FRAME_V1_NONE", ANOMALY_UI_FRAME_V1_NONE},
         {"ANOMALY_UI_FRAME_V1_ITEM_HOVERED", ANOMALY_UI_FRAME_V1_ITEM_HOVERED},
         {"ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED", ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED},
         {"ANOMALY_UI_FRAME_V1_ITEM_ACTIVE", ANOMALY_UI_FRAME_V1_ITEM_ACTIVE},
         {"ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE", ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE},
         {"ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD",
          ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD},
         {"ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT", ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT}},
        false);
    AppendEnum(
        output,
        "AnomalyUiTextInputFlagsV1",
        sizeof(AnomalyUiTextInputFlagsV1),
        alignof(AnomalyUiTextInputFlagsV1),
        {{"ANOMALY_UI_TEXT_INPUT_V1_NONE", ANOMALY_UI_TEXT_INPUT_V1_NONE},
         {"ANOMALY_UI_TEXT_INPUT_V1_DIGITS", ANOMALY_UI_TEXT_INPUT_V1_DIGITS}},
        false);
    AppendEnum(
        output,
        "AnomalyWindowFlagsV1",
        sizeof(AnomalyWindowFlagsV1),
        alignof(AnomalyWindowFlagsV1),
        {{"ANOMALY_WINDOW_V1_NONE", ANOMALY_WINDOW_V1_NONE},
         {"ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS", ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS},
         {"ANOMALY_WINDOW_V1_NO_COLLAPSE", ANOMALY_WINDOW_V1_NO_COLLAPSE}},
        false);
    AppendEnum(
        output,
        "AnomalyGlyphRangeV1",
        sizeof(AnomalyGlyphRangeV1),
        alignof(AnomalyGlyphRangeV1),
        {{"ANOMALY_GLYPH_RANGE_V1_DEFAULT", ANOMALY_GLYPH_RANGE_V1_DEFAULT},
         {"ANOMALY_GLYPH_RANGE_V1_LATIN", ANOMALY_GLYPH_RANGE_V1_LATIN},
         {"ANOMALY_GLYPH_RANGE_V1_CYRILLIC", ANOMALY_GLYPH_RANGE_V1_CYRILLIC},
         {"ANOMALY_GLYPH_RANGE_V1_JAPANESE", ANOMALY_GLYPH_RANGE_V1_JAPANESE},
         {"ANOMALY_GLYPH_RANGE_V1_CHINESE_FULL", ANOMALY_GLYPH_RANGE_V1_CHINESE_FULL}},
        false);
    AppendEnum(
        output,
        "AnomalyFontStateFlagsV1",
        sizeof(AnomalyFontStateFlagsV1),
        alignof(AnomalyFontStateFlagsV1),
        {{"ANOMALY_FONT_STATE_V1_NONE", ANOMALY_FONT_STATE_V1_NONE},
         {"ANOMALY_FONT_STATE_V1_QUEUED", ANOMALY_FONT_STATE_V1_QUEUED},
         {"ANOMALY_FONT_STATE_V1_READY", ANOMALY_FONT_STATE_V1_READY},
         {"ANOMALY_FONT_STATE_V1_FAILED", ANOMALY_FONT_STATE_V1_FAILED},
         {"ANOMALY_FONT_STATE_V1_STALE_DEVICE", ANOMALY_FONT_STATE_V1_STALE_DEVICE}},
        false);
    AppendEnum(
        output,
        "AnomalyTextureFormatV1",
        sizeof(AnomalyTextureFormatV1),
        alignof(AnomalyTextureFormatV1),
        {{"ANOMALY_TEXTURE_FORMAT_V1_AUTO", ANOMALY_TEXTURE_FORMAT_V1_AUTO},
         {"ANOMALY_TEXTURE_FORMAT_V1_RGBA8", ANOMALY_TEXTURE_FORMAT_V1_RGBA8}},
        false);
    AppendEnum(
        output,
        "AnomalyTextureStateFlagsV1",
        sizeof(AnomalyTextureStateFlagsV1),
        alignof(AnomalyTextureStateFlagsV1),
        {{"ANOMALY_TEXTURE_STATE_V1_NONE", ANOMALY_TEXTURE_STATE_V1_NONE},
         {"ANOMALY_TEXTURE_STATE_V1_QUEUED", ANOMALY_TEXTURE_STATE_V1_QUEUED},
         {"ANOMALY_TEXTURE_STATE_V1_READY", ANOMALY_TEXTURE_STATE_V1_READY},
         {"ANOMALY_TEXTURE_STATE_V1_FAILED", ANOMALY_TEXTURE_STATE_V1_FAILED},
         {"ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE", ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE}},
        false);
    AppendEnum(
        output,
        "AnomalyInputModifiersV1",
        sizeof(AnomalyInputModifiersV1),
        alignof(AnomalyInputModifiersV1),
        {{"ANOMALY_INPUT_MODIFIER_V1_NONE", ANOMALY_INPUT_MODIFIER_V1_NONE},
         {"ANOMALY_INPUT_MODIFIER_V1_SHIFT", ANOMALY_INPUT_MODIFIER_V1_SHIFT},
         {"ANOMALY_INPUT_MODIFIER_V1_CONTROL", ANOMALY_INPUT_MODIFIER_V1_CONTROL},
         {"ANOMALY_INPUT_MODIFIER_V1_ALT", ANOMALY_INPUT_MODIFIER_V1_ALT},
         {"ANOMALY_INPUT_MODIFIER_V1_SUPER", ANOMALY_INPUT_MODIFIER_V1_SUPER}},
        false);
    AppendEnum(
        output,
        "AnomalyInputCaptureFlagsV1",
        sizeof(AnomalyInputCaptureFlagsV1),
        alignof(AnomalyInputCaptureFlagsV1),
        {{"ANOMALY_INPUT_CAPTURE_V1_NONE", ANOMALY_INPUT_CAPTURE_V1_NONE},
         {"ANOMALY_INPUT_CAPTURE_V1_MOUSE", ANOMALY_INPUT_CAPTURE_V1_MOUSE},
         {"ANOMALY_INPUT_CAPTURE_V1_KEYBOARD", ANOMALY_INPUT_CAPTURE_V1_KEYBOARD},
         {"ANOMALY_INPUT_CAPTURE_V1_TEXT", ANOMALY_INPUT_CAPTURE_V1_TEXT}},
        false);
    AppendEnum(
        output,
        "AnomalyHotkeyFlagsV1",
        sizeof(AnomalyHotkeyFlagsV1),
        alignof(AnomalyHotkeyFlagsV1),
        {{"ANOMALY_HOTKEY_V1_NONE", ANOMALY_HOTKEY_V1_NONE},
         {"ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS",
          ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS},
         {"ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED",
          ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED},
         {"ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED",
          ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED}},
        false);
    AppendEnum(
        output,
        "AnomalyCoreLogLevelV1",
        sizeof(AnomalyCoreLogLevelV1),
        alignof(AnomalyCoreLogLevelV1),
        {{"ANOMALY_CORE_LOG_LEVEL_V1_TRACE", ANOMALY_CORE_LOG_LEVEL_V1_TRACE},
         {"ANOMALY_CORE_LOG_LEVEL_V1_INFO", ANOMALY_CORE_LOG_LEVEL_V1_INFO},
         {"ANOMALY_CORE_LOG_LEVEL_V1_WARNING", ANOMALY_CORE_LOG_LEVEL_V1_WARNING},
         {"ANOMALY_CORE_LOG_LEVEL_V1_ERROR", ANOMALY_CORE_LOG_LEVEL_V1_ERROR}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcModeV1",
        sizeof(AnomalyIpcModeV1),
        alignof(AnomalyIpcModeV1),
        {{"ANOMALY_IPC_MODE_V1_SYNC_REQUEST", ANOMALY_IPC_MODE_V1_SYNC_REQUEST},
         {"ANOMALY_IPC_MODE_V1_ASYNC_REQUEST", ANOMALY_IPC_MODE_V1_ASYNC_REQUEST},
         {"ANOMALY_IPC_MODE_V1_EVENT", ANOMALY_IPC_MODE_V1_EVENT}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcAffinityV1",
        sizeof(AnomalyIpcAffinityV1),
        alignof(AnomalyIpcAffinityV1),
        {{"ANOMALY_IPC_AFFINITY_V1_CALLER", ANOMALY_IPC_AFFINITY_V1_CALLER},
         {"ANOMALY_IPC_AFFINITY_V1_WORKER", ANOMALY_IPC_AFFINITY_V1_WORKER},
         {"ANOMALY_IPC_AFFINITY_V1_LIFECYCLE", ANOMALY_IPC_AFFINITY_V1_LIFECYCLE},
         {"ANOMALY_IPC_AFFINITY_V1_GAME", ANOMALY_IPC_AFFINITY_V1_GAME},
         {"ANOMALY_IPC_AFFINITY_V1_RENDER", ANOMALY_IPC_AFFINITY_V1_RENDER}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcReentrancyV1",
        sizeof(AnomalyIpcReentrancyV1),
        alignof(AnomalyIpcReentrancyV1),
        {{"ANOMALY_IPC_REENTRANCY_V1_REJECT", ANOMALY_IPC_REENTRANCY_V1_REJECT},
         {"ANOMALY_IPC_REENTRANCY_V1_ALLOW", ANOMALY_IPC_REENTRANCY_V1_ALLOW}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcErrorV1",
        sizeof(AnomalyIpcErrorV1),
        alignof(AnomalyIpcErrorV1),
        {{"ANOMALY_IPC_ERROR_V1_NONE", ANOMALY_IPC_ERROR_V1_NONE},
         {"ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING", ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING},
         {"ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH", ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH},
         {"ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH", ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH},
         {"ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE", ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE},
         {"ANOMALY_IPC_ERROR_V1_TIMEOUT", ANOMALY_IPC_ERROR_V1_TIMEOUT},
         {"ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE", ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE},
         {"ANOMALY_IPC_ERROR_V1_QUEUE_FULL", ANOMALY_IPC_ERROR_V1_QUEUE_FULL},
         {"ANOMALY_IPC_ERROR_V1_STALE_GENERATION", ANOMALY_IPC_ERROR_V1_STALE_GENERATION},
         {"ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED", ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED}},
        true);
    output.append("  },\n");

    output.append("  \"structs\": {\n");
    AppendStruct(
        output,
        "AnomalyStringViewV1",
        sizeof(AnomalyStringViewV1),
        alignof(AnomalyStringViewV1),
        {{"data", offsetof(AnomalyStringViewV1, data)}, {"size", offsetof(AnomalyStringViewV1, size)}},
        false);
    AppendStruct(
        output,
        "AnomalyByteSpanV1",
        sizeof(AnomalyByteSpanV1),
        alignof(AnomalyByteSpanV1),
        {{"data", offsetof(AnomalyByteSpanV1, data)}, {"size", offsetof(AnomalyByteSpanV1, size)}},
        false);
    AppendStruct(
        output,
        "AnomalyMutableByteSpanV1",
        sizeof(AnomalyMutableByteSpanV1),
        alignof(AnomalyMutableByteSpanV1),
        {{"data", offsetof(AnomalyMutableByteSpanV1, data)},
         {"size", offsetof(AnomalyMutableByteSpanV1, size)}},
        false);
    AppendStruct(
        output,
        "AnomalyStatusV1",
        sizeof(AnomalyStatusV1),
        alignof(AnomalyStatusV1),
        {{"code", offsetof(AnomalyStatusV1, code)},
         {"reserved", offsetof(AnomalyStatusV1, reserved)},
         {"message", offsetof(AnomalyStatusV1, message)}},
        false);
    AppendStruct(
        output,
        "AnomalyAllocatorV1",
        sizeof(AnomalyAllocatorV1),
        alignof(AnomalyAllocatorV1),
        {{"struct_size", offsetof(AnomalyAllocatorV1, struct_size)},
         {"reserved", offsetof(AnomalyAllocatorV1, reserved)},
         {"user", offsetof(AnomalyAllocatorV1, user)},
         {"allocate", offsetof(AnomalyAllocatorV1, allocate)},
         {"reallocate", offsetof(AnomalyAllocatorV1, reallocate)},
         {"release", offsetof(AnomalyAllocatorV1, release)}},
        false);
    AppendStruct(
        output,
        "AnomalyGenerationHandleV1",
        sizeof(AnomalyGenerationHandleV1),
        alignof(AnomalyGenerationHandleV1),
        {{"id", offsetof(AnomalyGenerationHandleV1, id)},
         {"generation", offsetof(AnomalyGenerationHandleV1, generation)}},
        false);
    AppendStruct(
        output,
        "AnomalyHostApiV1",
        sizeof(AnomalyHostApiV1),
        alignof(AnomalyHostApiV1),
        {{"struct_size", offsetof(AnomalyHostApiV1, struct_size)},
         {"api_major", offsetof(AnomalyHostApiV1, api_major)},
         {"api_minor", offsetof(AnomalyHostApiV1, api_minor)},
         {"host_context", offsetof(AnomalyHostApiV1, host_context)},
         {"allocator", offsetof(AnomalyHostApiV1, allocator)},
         {"query_service", offsetof(AnomalyHostApiV1, query_service)}},
        false);
    AppendStruct(
        output,
        "AnomalyPluginDescriptorV1",
        sizeof(AnomalyPluginDescriptorV1),
        alignof(AnomalyPluginDescriptorV1),
        {{"struct_size", offsetof(AnomalyPluginDescriptorV1, struct_size)},
         {"api_major", offsetof(AnomalyPluginDescriptorV1, api_major)},
         {"api_minor", offsetof(AnomalyPluginDescriptorV1, api_minor)},
         {"id", offsetof(AnomalyPluginDescriptorV1, id)},
         {"name", offsetof(AnomalyPluginDescriptorV1, name)},
         {"author", offsetof(AnomalyPluginDescriptorV1, author)},
         {"version", offsetof(AnomalyPluginDescriptorV1, version)},
         {"on_load", offsetof(AnomalyPluginDescriptorV1, on_load)},
         {"on_start", offsetof(AnomalyPluginDescriptorV1, on_start)},
         {"on_stop", offsetof(AnomalyPluginDescriptorV1, on_stop)},
         {"on_unload", offsetof(AnomalyPluginDescriptorV1, on_unload)},
         {"on_update", offsetof(AnomalyPluginDescriptorV1, on_update)},
         {"on_draw", offsetof(AnomalyPluginDescriptorV1, on_draw)}},
        false);
    AppendStruct(
        output,
        "AnomalyCoreServiceV1",
        sizeof(AnomalyCoreServiceV1),
        alignof(AnomalyCoreServiceV1),
        {{"struct_size", offsetof(AnomalyCoreServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyCoreServiceV1, service_version)},
         {"user", offsetof(AnomalyCoreServiceV1, user)},
         {"log", offsetof(AnomalyCoreServiceV1, log)},
         {"read_memory", offsetof(AnomalyCoreServiceV1, read_memory)},
         {"write_memory", offsetof(AnomalyCoreServiceV1, write_memory)},
         {"plugin_directory", offsetof(AnomalyCoreServiceV1, plugin_directory)}},
        false);
    AppendStruct(
        output,
        "AnomalyPluginStateServiceV1",
        sizeof(AnomalyPluginStateServiceV1),
        alignof(AnomalyPluginStateServiceV1),
        {{"struct_size", offsetof(AnomalyPluginStateServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyPluginStateServiceV1, service_version)},
         {"user", offsetof(AnomalyPluginStateServiceV1, user)},
         {"directory", offsetof(AnomalyPluginStateServiceV1, directory)}},
        false);
    AppendStruct(
        output,
        "AnomalyRuntimeInfoV1",
        sizeof(AnomalyRuntimeInfoV1),
        alignof(AnomalyRuntimeInfoV1),
        {{"struct_size", offsetof(AnomalyRuntimeInfoV1, struct_size)},
         {"runtime_version_major", offsetof(AnomalyRuntimeInfoV1, runtime_version_major)},
         {"runtime_version_minor", offsetof(AnomalyRuntimeInfoV1, runtime_version_minor)},
         {"runtime_version_patch", offsetof(AnomalyRuntimeInfoV1, runtime_version_patch)},
         {"process_id", offsetof(AnomalyRuntimeInfoV1, process_id)},
         {"thread_id", offsetof(AnomalyRuntimeInfoV1, thread_id)},
         {"uptime_milliseconds", offsetof(AnomalyRuntimeInfoV1, uptime_milliseconds)},
         {"plugin_generation", offsetof(AnomalyRuntimeInfoV1, plugin_generation)}},
        false);
    AppendStruct(
        output,
        "AnomalyConfigServiceV1",
        sizeof(AnomalyConfigServiceV1),
        alignof(AnomalyConfigServiceV1),
        {{"struct_size", offsetof(AnomalyConfigServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyConfigServiceV1, service_version)},
         {"user", offsetof(AnomalyConfigServiceV1, user)},
         {"register_schema", offsetof(AnomalyConfigServiceV1, register_schema)},
         {"unregister_schema", offsetof(AnomalyConfigServiceV1, unregister_schema)},
         {"read", offsetof(AnomalyConfigServiceV1, read)},
         {"write_atomic", offsetof(AnomalyConfigServiceV1, write_atomic)},
         {"migrate", offsetof(AnomalyConfigServiceV1, migrate)}},
        false);
    AppendStruct(
        output,
        "AnomalyStorageServiceV1",
        sizeof(AnomalyStorageServiceV1),
        alignof(AnomalyStorageServiceV1),
        {{"struct_size", offsetof(AnomalyStorageServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyStorageServiceV1, service_version)},
         {"user", offsetof(AnomalyStorageServiceV1, user)},
         {"read", offsetof(AnomalyStorageServiceV1, read)},
         {"write_atomic", offsetof(AnomalyStorageServiceV1, write_atomic)},
         {"remove", offsetof(AnomalyStorageServiceV1, remove)}},
        false);
    AppendStruct(
        output,
        "AnomalyRuntimeInfoServiceV1",
        sizeof(AnomalyRuntimeInfoServiceV1),
        alignof(AnomalyRuntimeInfoServiceV1),
        {{"struct_size", offsetof(AnomalyRuntimeInfoServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyRuntimeInfoServiceV1, service_version)},
         {"user", offsetof(AnomalyRuntimeInfoServiceV1, user)},
         {"snapshot", offsetof(AnomalyRuntimeInfoServiceV1, snapshot)},
         {"runtime_version_utf8", offsetof(AnomalyRuntimeInfoServiceV1, runtime_version_utf8)}},
        false);
    AppendStruct(
        output,
        "AnomalyDiagnosticsServiceV1",
        sizeof(AnomalyDiagnosticsServiceV1),
        alignof(AnomalyDiagnosticsServiceV1),
        {{"struct_size", offsetof(AnomalyDiagnosticsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyDiagnosticsServiceV1, service_version)},
         {"user", offsetof(AnomalyDiagnosticsServiceV1, user)},
         {"register_self_test", offsetof(AnomalyDiagnosticsServiceV1, register_self_test)},
         {"unregister_self_test", offsetof(AnomalyDiagnosticsServiceV1, unregister_self_test)},
         {"run_self_test", offsetof(AnomalyDiagnosticsServiceV1, run_self_test)},
         {"snapshot_json", offsetof(AnomalyDiagnosticsServiceV1, snapshot_json)}},
        false);
    AppendStruct(
        output,
        "AnomalySchedulerServiceV1",
        sizeof(AnomalySchedulerServiceV1),
        alignof(AnomalySchedulerServiceV1),
        {{"struct_size", offsetof(AnomalySchedulerServiceV1, struct_size)},
         {"service_version", offsetof(AnomalySchedulerServiceV1, service_version)},
         {"user", offsetof(AnomalySchedulerServiceV1, user)},
         {"schedule", offsetof(AnomalySchedulerServiceV1, schedule)},
         {"cancel", offsetof(AnomalySchedulerServiceV1, cancel)}},
        false);
    AppendStruct(
        output,
        "AnomalyWebSocketServerInfoV1",
        sizeof(AnomalyWebSocketServerInfoV1),
        alignof(AnomalyWebSocketServerInfoV1),
        {{"struct_size", offsetof(AnomalyWebSocketServerInfoV1, struct_size)},
         {"port", offsetof(AnomalyWebSocketServerInfoV1, port)},
         {"reserved", offsetof(AnomalyWebSocketServerInfoV1, reserved)},
         {"connected_clients", offsetof(AnomalyWebSocketServerInfoV1, connected_clients)},
         {"published_messages", offsetof(AnomalyWebSocketServerInfoV1, published_messages)},
         {"dropped_messages", offsetof(AnomalyWebSocketServerInfoV1, dropped_messages)}},
        false);
    AppendStruct(
        output,
        "AnomalyWebSocketServiceV1",
        sizeof(AnomalyWebSocketServiceV1),
        alignof(AnomalyWebSocketServiceV1),
        {{"struct_size", offsetof(AnomalyWebSocketServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyWebSocketServiceV1, service_version)},
         {"user", offsetof(AnomalyWebSocketServiceV1, user)},
         {"publish_text", offsetof(AnomalyWebSocketServiceV1, publish_text)},
         {"server_info", offsetof(AnomalyWebSocketServiceV1, server_info)},
         {"set_port", offsetof(AnomalyWebSocketServiceV1, set_port)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcSchemaHashV1",
        sizeof(AnomalyIpcSchemaHashV1),
        alignof(AnomalyIpcSchemaHashV1),
        {{"bytes", offsetof(AnomalyIpcSchemaHashV1, bytes)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcEndpointDescriptorV1",
        sizeof(AnomalyIpcEndpointDescriptorV1),
        alignof(AnomalyIpcEndpointDescriptorV1),
        {{"struct_size", offsetof(AnomalyIpcEndpointDescriptorV1, struct_size)},
         {"endpoint_id", offsetof(AnomalyIpcEndpointDescriptorV1, endpoint_id)},
         {"major_version", offsetof(AnomalyIpcEndpointDescriptorV1, major_version)},
         {"minor_version", offsetof(AnomalyIpcEndpointDescriptorV1, minor_version)},
         {"request_schema", offsetof(AnomalyIpcEndpointDescriptorV1, request_schema)},
         {"response_schema", offsetof(AnomalyIpcEndpointDescriptorV1, response_schema)},
         {"event_schema", offsetof(AnomalyIpcEndpointDescriptorV1, event_schema)},
         {"modes", offsetof(AnomalyIpcEndpointDescriptorV1, modes)},
         {"affinity", offsetof(AnomalyIpcEndpointDescriptorV1, affinity)},
         {"timeout_milliseconds",
          offsetof(AnomalyIpcEndpointDescriptorV1, timeout_milliseconds)},
         {"reentrancy", offsetof(AnomalyIpcEndpointDescriptorV1, reentrancy)},
         {"maximum_request_bytes",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_request_bytes)},
         {"maximum_response_bytes",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_response_bytes)},
         {"maximum_event_bytes",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_event_bytes)},
         {"maximum_queue_depth",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_queue_depth)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcEndpointSelectorV1",
        sizeof(AnomalyIpcEndpointSelectorV1),
        alignof(AnomalyIpcEndpointSelectorV1),
        {{"struct_size", offsetof(AnomalyIpcEndpointSelectorV1, struct_size)},
         {"endpoint_id", offsetof(AnomalyIpcEndpointSelectorV1, endpoint_id)},
         {"major_version", offsetof(AnomalyIpcEndpointSelectorV1, major_version)},
         {"minimum_minor_version",
          offsetof(AnomalyIpcEndpointSelectorV1, minimum_minor_version)},
         {"request_schema", offsetof(AnomalyIpcEndpointSelectorV1, request_schema)},
         {"response_schema", offsetof(AnomalyIpcEndpointSelectorV1, response_schema)},
         {"event_schema", offsetof(AnomalyIpcEndpointSelectorV1, event_schema)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcRequestContextV1",
        sizeof(AnomalyIpcRequestContextV1),
        alignof(AnomalyIpcRequestContextV1),
        {{"struct_size", offsetof(AnomalyIpcRequestContextV1, struct_size)},
         {"reserved", offsetof(AnomalyIpcRequestContextV1, reserved)},
         {"request_id", offsetof(AnomalyIpcRequestContextV1, request_id)},
         {"caller_plugin_id", offsetof(AnomalyIpcRequestContextV1, caller_plugin_id)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcServiceV1",
        sizeof(AnomalyIpcServiceV1),
        alignof(AnomalyIpcServiceV1),
        {{"struct_size", offsetof(AnomalyIpcServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyIpcServiceV1, service_version)},
         {"user", offsetof(AnomalyIpcServiceV1, user)},
         {"register_endpoint", offsetof(AnomalyIpcServiceV1, register_endpoint)},
         {"unregister_endpoint", offsetof(AnomalyIpcServiceV1, unregister_endpoint)},
         {"invoke", offsetof(AnomalyIpcServiceV1, invoke)},
         {"invoke_async", offsetof(AnomalyIpcServiceV1, invoke_async)},
         {"cancel", offsetof(AnomalyIpcServiceV1, cancel)},
         {"subscribe", offsetof(AnomalyIpcServiceV1, subscribe)},
         {"unsubscribe", offsetof(AnomalyIpcServiceV1, unsubscribe)},
         {"publish", offsetof(AnomalyIpcServiceV1, publish)}},
        false);
    AppendStruct(
        output,
        "AnomalyCommandsServiceV1",
        sizeof(AnomalyCommandsServiceV1),
        alignof(AnomalyCommandsServiceV1),
        {{"struct_size", offsetof(AnomalyCommandsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyCommandsServiceV1, service_version)},
         {"user", offsetof(AnomalyCommandsServiceV1, user)},
         {"register_command", offsetof(AnomalyCommandsServiceV1, register_command)},
         {"unregister_command", offsetof(AnomalyCommandsServiceV1, unregister_command)},
         {"invoke", offsetof(AnomalyCommandsServiceV1, invoke)}},
        false);
    AppendStruct(
        output,
        "AnomalyNotificationsServiceV1",
        sizeof(AnomalyNotificationsServiceV1),
        alignof(AnomalyNotificationsServiceV1),
        {{"struct_size", offsetof(AnomalyNotificationsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNotificationsServiceV1, service_version)},
         {"user", offsetof(AnomalyNotificationsServiceV1, user)},
         {"post", offsetof(AnomalyNotificationsServiceV1, post)},
         {"dismiss", offsetof(AnomalyNotificationsServiceV1, dismiss)}},
        false);
    AppendStruct(
        output,
        "AnomalyHookRequestV1",
        sizeof(AnomalyHookRequestV1),
        alignof(AnomalyHookRequestV1),
        {{"struct_size", offsetof(AnomalyHookRequestV1, struct_size)},
         {"kind", offsetof(AnomalyHookRequestV1, kind)},
         {"target", offsetof(AnomalyHookRequestV1, target)},
         {"detour", offsetof(AnomalyHookRequestV1, detour)},
         {"label", offsetof(AnomalyHookRequestV1, label)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureServiceV1",
        sizeof(AnomalySignatureServiceV1),
        alignof(AnomalySignatureServiceV1),
        {{"struct_size", offsetof(AnomalySignatureServiceV1, struct_size)},
         {"service_version", offsetof(AnomalySignatureServiceV1, service_version)},
         {"user", offsetof(AnomalySignatureServiceV1, user)},
         {"resolve", offsetof(AnomalySignatureServiceV1, resolve)}},
        false);
    AppendStruct(
        output,
        "AnomalyHookServiceV1",
        sizeof(AnomalyHookServiceV1),
        alignof(AnomalyHookServiceV1),
        {{"struct_size", offsetof(AnomalyHookServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyHookServiceV1, service_version)},
         {"user", offsetof(AnomalyHookServiceV1, user)},
         {"create", offsetof(AnomalyHookServiceV1, create)},
         {"release", offsetof(AnomalyHookServiceV1, release)},
         {"begin_callback", offsetof(AnomalyHookServiceV1, begin_callback)},
         {"end_callback", offsetof(AnomalyHookServiceV1, end_callback)}},
        false);
    AppendStruct(
        output,
        "AnomalyPatchServiceV1",
        sizeof(AnomalyPatchServiceV1),
        alignof(AnomalyPatchServiceV1),
        {{"struct_size", offsetof(AnomalyPatchServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyPatchServiceV1, service_version)},
         {"user", offsetof(AnomalyPatchServiceV1, user)},
         {"apply", offsetof(AnomalyPatchServiceV1, apply)},
         {"release", offsetof(AnomalyPatchServiceV1, release)}},
        false);
    AppendStruct(
        output,
        "AnomalyEspCameraV1",
        sizeof(AnomalyEspCameraV1),
        alignof(AnomalyEspCameraV1),
        {{"struct_size", offsetof(AnomalyEspCameraV1, struct_size)},
         {"flags", offsetof(AnomalyEspCameraV1, flags)},
         {"position", offsetof(AnomalyEspCameraV1, position)},
         {"rotation", offsetof(AnomalyEspCameraV1, rotation)},
         {"horizontal_fov_degrees", offsetof(AnomalyEspCameraV1, horizontal_fov_degrees)},
         {"reserved", offsetof(AnomalyEspCameraV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyEspEntityBoundsV1",
        sizeof(AnomalyEspEntityBoundsV1),
        alignof(AnomalyEspEntityBoundsV1),
        {{"struct_size", offsetof(AnomalyEspEntityBoundsV1, struct_size)},
         {"flags", offsetof(AnomalyEspEntityBoundsV1, flags)},
         {"center", offsetof(AnomalyEspEntityBoundsV1, center)},
         {"extent", offsetof(AnomalyEspEntityBoundsV1, extent)}},
        false);
    AppendStruct(
        output,
        "AnomalyEspBoxStyleV1",
        sizeof(AnomalyEspBoxStyleV1),
        alignof(AnomalyEspBoxStyleV1),
        {{"struct_size", offsetof(AnomalyEspBoxStyleV1, struct_size)},
         {"flags", offsetof(AnomalyEspBoxStyleV1, flags)},
         {"color_rgba", offsetof(AnomalyEspBoxStyleV1, color_rgba)},
         {"outline_color_rgba", offsetof(AnomalyEspBoxStyleV1, outline_color_rgba)},
         {"thickness", offsetof(AnomalyEspBoxStyleV1, thickness)},
         {"outline_thickness", offsetof(AnomalyEspBoxStyleV1, outline_thickness)}},
        false);
    AppendStruct(
        output,
        "AnomalyUiServiceV1",
        sizeof(AnomalyUiServiceV1),
        alignof(AnomalyUiServiceV1),
        {{"struct_size", offsetof(AnomalyUiServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUiServiceV1, service_version)},
         {"user", offsetof(AnomalyUiServiceV1, user)},
         {"set_next_window_size", offsetof(AnomalyUiServiceV1, set_next_window_size)},
         {"begin_window", offsetof(AnomalyUiServiceV1, begin_window)},
         {"end_window", offsetof(AnomalyUiServiceV1, end_window)},
         {"text", offsetof(AnomalyUiServiceV1, text)},
         {"button", offsetof(AnomalyUiServiceV1, button)},
         {"draw_entity_bbox", offsetof(AnomalyUiServiceV1, draw_entity_bbox)},
         {"checkbox", offsetof(AnomalyUiServiceV1, checkbox)},
         {"slider_float", offsetof(AnomalyUiServiceV1, slider_float)},
         {"color_edit4", offsetof(AnomalyUiServiceV1, color_edit4)},
         {"draw_entity_box3d", offsetof(AnomalyUiServiceV1, draw_entity_box3d)},
         {"draw_entity_label", offsetof(AnomalyUiServiceV1, draw_entity_label)},
         {"separator", offsetof(AnomalyUiServiceV1, separator)},
         {"begin_child", offsetof(AnomalyUiServiceV1, begin_child)},
         {"end_child", offsetof(AnomalyUiServiceV1, end_child)},
         {"begin_table", offsetof(AnomalyUiServiceV1, begin_table)},
         {"table_next_row", offsetof(AnomalyUiServiceV1, table_next_row)},
         {"table_next_column", offsetof(AnomalyUiServiceV1, table_next_column)},
         {"end_table", offsetof(AnomalyUiServiceV1, end_table)},
         {"begin_menu", offsetof(AnomalyUiServiceV1, begin_menu)},
         {"end_menu", offsetof(AnomalyUiServiceV1, end_menu)},
         {"open_popup", offsetof(AnomalyUiServiceV1, open_popup)},
         {"begin_popup_modal", offsetof(AnomalyUiServiceV1, begin_popup_modal)},
         {"end_popup", offsetof(AnomalyUiServiceV1, end_popup)},
         {"close_current_popup", offsetof(AnomalyUiServiceV1, close_current_popup)},
         {"filter_match", offsetof(AnomalyUiServiceV1, filter_match)},
         {"frame_state", offsetof(AnomalyUiServiceV1, frame_state)},
         {"set_next_window_size_constraints",
          offsetof(AnomalyUiServiceV1, set_next_window_size_constraints)},
         {"get_window_size", offsetof(AnomalyUiServiceV1, get_window_size)},
         {"input_uint32", offsetof(AnomalyUiServiceV1, input_uint32)},
         {"input_double", offsetof(AnomalyUiServiceV1, input_double)},
         {"developer_mode_enabled", offsetof(AnomalyUiServiceV1, developer_mode_enabled)},
         {"input_text", offsetof(AnomalyUiServiceV1, input_text)},
         {"button_enabled", offsetof(AnomalyUiServiceV1, button_enabled)},
         {"same_line", offsetof(AnomalyUiServiceV1, same_line)},
         {"set_cursor_pos_x", offsetof(AnomalyUiServiceV1, set_cursor_pos_x)},
         {"text_link", offsetof(AnomalyUiServiceV1, text_link)}},
        false);
    AppendStruct(
        output,
        "AnomalyWindowSpecV1",
        sizeof(AnomalyWindowSpecV1),
        alignof(AnomalyWindowSpecV1),
        {{"struct_size", offsetof(AnomalyWindowSpecV1, struct_size)},
         {"flags", offsetof(AnomalyWindowSpecV1, flags)},
         {"id", offsetof(AnomalyWindowSpecV1, id)},
         {"title", offsetof(AnomalyWindowSpecV1, title)},
         {"initial_width", offsetof(AnomalyWindowSpecV1, initial_width)},
         {"initial_height", offsetof(AnomalyWindowSpecV1, initial_height)},
         {"minimum_width", offsetof(AnomalyWindowSpecV1, minimum_width)},
         {"minimum_height", offsetof(AnomalyWindowSpecV1, minimum_height)},
         {"maximum_width", offsetof(AnomalyWindowSpecV1, maximum_width)},
         {"maximum_height", offsetof(AnomalyWindowSpecV1, maximum_height)},
         {"default_open", offsetof(AnomalyWindowSpecV1, default_open)},
         {"reserved", offsetof(AnomalyWindowSpecV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyWindowStateV1",
        sizeof(AnomalyWindowStateV1),
        alignof(AnomalyWindowStateV1),
        {{"struct_size", offsetof(AnomalyWindowStateV1, struct_size)},
         {"flags", offsetof(AnomalyWindowStateV1, flags)},
         {"width", offsetof(AnomalyWindowStateV1, width)},
         {"height", offsetof(AnomalyWindowStateV1, height)},
         {"ui_generation", offsetof(AnomalyWindowStateV1, ui_generation)},
         {"open", offsetof(AnomalyWindowStateV1, open)},
         {"reserved", offsetof(AnomalyWindowStateV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyFontRequestV1",
        sizeof(AnomalyFontRequestV1),
        alignof(AnomalyFontRequestV1),
        {{"struct_size", offsetof(AnomalyFontRequestV1, struct_size)},
         {"flags", offsetof(AnomalyFontRequestV1, flags)},
         {"relative_path", offsetof(AnomalyFontRequestV1, relative_path)},
         {"size_pixels", offsetof(AnomalyFontRequestV1, size_pixels)},
         {"glyph_range", offsetof(AnomalyFontRequestV1, glyph_range)},
         {"reserved", offsetof(AnomalyFontRequestV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyFontStateV1",
        sizeof(AnomalyFontStateV1),
        alignof(AnomalyFontStateV1),
        {{"struct_size", offsetof(AnomalyFontStateV1, struct_size)},
         {"flags", offsetof(AnomalyFontStateV1, flags)},
         {"effective_size_pixels", offsetof(AnomalyFontStateV1, effective_size_pixels)},
         {"scale", offsetof(AnomalyFontStateV1, scale)},
         {"device_generation", offsetof(AnomalyFontStateV1, device_generation)},
         {"ready", offsetof(AnomalyFontStateV1, ready)},
         {"reserved", offsetof(AnomalyFontStateV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyTextureRequestV1",
        sizeof(AnomalyTextureRequestV1),
        alignof(AnomalyTextureRequestV1),
        {{"struct_size", offsetof(AnomalyTextureRequestV1, struct_size)},
         {"flags", offsetof(AnomalyTextureRequestV1, flags)},
         {"relative_path", offsetof(AnomalyTextureRequestV1, relative_path)},
         {"encoded_bytes", offsetof(AnomalyTextureRequestV1, encoded_bytes)},
         {"format", offsetof(AnomalyTextureRequestV1, format)},
         {"width", offsetof(AnomalyTextureRequestV1, width)},
         {"height", offsetof(AnomalyTextureRequestV1, height)},
         {"reserved", offsetof(AnomalyTextureRequestV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyTextureStateV1",
        sizeof(AnomalyTextureStateV1),
        alignof(AnomalyTextureStateV1),
        {{"struct_size", offsetof(AnomalyTextureStateV1, struct_size)},
         {"flags", offsetof(AnomalyTextureStateV1, flags)},
         {"width", offsetof(AnomalyTextureStateV1, width)},
         {"height", offsetof(AnomalyTextureStateV1, height)},
         {"device_generation", offsetof(AnomalyTextureStateV1, device_generation)},
         {"byte_size", offsetof(AnomalyTextureStateV1, byte_size)}},
        false);
    AppendStruct(
        output,
        "AnomalyInputSnapshotV1",
        sizeof(AnomalyInputSnapshotV1),
        alignof(AnomalyInputSnapshotV1),
        {{"struct_size", offsetof(AnomalyInputSnapshotV1, struct_size)},
         {"modifiers", offsetof(AnomalyInputSnapshotV1, modifiers)},
         {"sequence", offsetof(AnomalyInputSnapshotV1, sequence)},
         {"timestamp_milliseconds", offsetof(AnomalyInputSnapshotV1, timestamp_milliseconds)},
         {"mouse_x", offsetof(AnomalyInputSnapshotV1, mouse_x)},
         {"mouse_y", offsetof(AnomalyInputSnapshotV1, mouse_y)},
         {"mouse_wheel", offsetof(AnomalyInputSnapshotV1, mouse_wheel)},
         {"capture_flags", offsetof(AnomalyInputSnapshotV1, capture_flags)},
         {"keys", offsetof(AnomalyInputSnapshotV1, keys)},
         {"mouse_buttons", offsetof(AnomalyInputSnapshotV1, mouse_buttons)},
         {"reserved", offsetof(AnomalyInputSnapshotV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyHotkeySpecV1",
        sizeof(AnomalyHotkeySpecV1),
        alignof(AnomalyHotkeySpecV1),
        {{"struct_size", offsetof(AnomalyHotkeySpecV1, struct_size)},
         {"modifiers", offsetof(AnomalyHotkeySpecV1, modifiers)},
         {"virtual_key", offsetof(AnomalyHotkeySpecV1, virtual_key)},
         {"flags", offsetof(AnomalyHotkeySpecV1, flags)},
         {"id", offsetof(AnomalyHotkeySpecV1, id)}},
        false);
    AppendStruct(
        output,
        "AnomalyWindowServiceV1",
        sizeof(AnomalyWindowServiceV1),
        alignof(AnomalyWindowServiceV1),
        {{"struct_size", offsetof(AnomalyWindowServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyWindowServiceV1, service_version)},
         {"user", offsetof(AnomalyWindowServiceV1, user)},
         {"register_window", offsetof(AnomalyWindowServiceV1, register_window)},
         {"release_window", offsetof(AnomalyWindowServiceV1, release_window)},
         {"set_open", offsetof(AnomalyWindowServiceV1, set_open)},
         {"toggle", offsetof(AnomalyWindowServiceV1, toggle)},
         {"state", offsetof(AnomalyWindowServiceV1, state)},
         {"begin", offsetof(AnomalyWindowServiceV1, begin)},
         {"end", offsetof(AnomalyWindowServiceV1, end)}},
        false);
    AppendStruct(
        output,
        "AnomalyFontServiceV1",
        sizeof(AnomalyFontServiceV1),
        alignof(AnomalyFontServiceV1),
        {{"struct_size", offsetof(AnomalyFontServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyFontServiceV1, service_version)},
         {"user", offsetof(AnomalyFontServiceV1, user)},
         {"request", offsetof(AnomalyFontServiceV1, request)},
         {"release", offsetof(AnomalyFontServiceV1, release)},
         {"state", offsetof(AnomalyFontServiceV1, state)},
         {"push", offsetof(AnomalyFontServiceV1, push)},
         {"pop", offsetof(AnomalyFontServiceV1, pop)}},
        false);
    AppendStruct(
        output,
        "AnomalyTextureServiceV1",
        sizeof(AnomalyTextureServiceV1),
        alignof(AnomalyTextureServiceV1),
        {{"struct_size", offsetof(AnomalyTextureServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyTextureServiceV1, service_version)},
         {"user", offsetof(AnomalyTextureServiceV1, user)},
         {"request", offsetof(AnomalyTextureServiceV1, request)},
         {"release", offsetof(AnomalyTextureServiceV1, release)},
         {"state", offsetof(AnomalyTextureServiceV1, state)},
         {"draw", offsetof(AnomalyTextureServiceV1, draw)}},
        false);
    AppendStruct(
        output,
        "AnomalyInputServiceV1",
        sizeof(AnomalyInputServiceV1),
        alignof(AnomalyInputServiceV1),
        {{"struct_size", offsetof(AnomalyInputServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyInputServiceV1, service_version)},
         {"user", offsetof(AnomalyInputServiceV1, user)},
         {"snapshot", offsetof(AnomalyInputServiceV1, snapshot)},
         {"was_pressed", offsetof(AnomalyInputServiceV1, was_pressed)},
         {"register_hotkey", offsetof(AnomalyInputServiceV1, register_hotkey)},
         {"release_hotkey", offsetof(AnomalyInputServiceV1, release_hotkey)},
         {"capture_state", offsetof(AnomalyInputServiceV1, capture_state)}},
        false);
    AppendStruct(
        output,
        "AnomalyLocalizationServiceV1",
        sizeof(AnomalyLocalizationServiceV1),
        alignof(AnomalyLocalizationServiceV1),
        {{"struct_size", offsetof(AnomalyLocalizationServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyLocalizationServiceV1, service_version)},
         {"user", offsetof(AnomalyLocalizationServiceV1, user)},
         {"locale", offsetof(AnomalyLocalizationServiceV1, locale)},
         {"translate", offsetof(AnomalyLocalizationServiceV1, translate)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5BuildServiceV1",
        sizeof(AnomalyUe5BuildServiceV1),
        alignof(AnomalyUe5BuildServiceV1),
        {{"struct_size", offsetof(AnomalyUe5BuildServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUe5BuildServiceV1, service_version)},
         {"user", offsetof(AnomalyUe5BuildServiceV1, user)},
         {"build_id", offsetof(AnomalyUe5BuildServiceV1, build_id)},
         {"profile_hash", offsetof(AnomalyUe5BuildServiceV1, profile_hash)},
         {"feature_state", offsetof(AnomalyUe5BuildServiceV1, feature_state)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5AhudFrameV1",
        sizeof(AnomalyUe5AhudFrameV1),
        alignof(AnomalyUe5AhudFrameV1),
        {{"struct_size", offsetof(AnomalyUe5AhudFrameV1, struct_size)},
         {"flags", offsetof(AnomalyUe5AhudFrameV1, flags)},
         {"user", offsetof(AnomalyUe5AhudFrameV1, user)},
         {"viewport_width", offsetof(AnomalyUe5AhudFrameV1, viewport_width)},
         {"viewport_height", offsetof(AnomalyUe5AhudFrameV1, viewport_height)},
         {"project", offsetof(AnomalyUe5AhudFrameV1, project)},
         {"measure_text", offsetof(AnomalyUe5AhudFrameV1, measure_text)},
         {"draw_text", offsetof(AnomalyUe5AhudFrameV1, draw_text)},
         {"draw_line", offsetof(AnomalyUe5AhudFrameV1, draw_line)},
         {"draw_rect", offsetof(AnomalyUe5AhudFrameV1, draw_rect)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5AhudServiceV1",
        sizeof(AnomalyUe5AhudServiceV1),
        alignof(AnomalyUe5AhudServiceV1),
        {{"struct_size", offsetof(AnomalyUe5AhudServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUe5AhudServiceV1, service_version)},
         {"user", offsetof(AnomalyUe5AhudServiceV1, user)},
         {"subscribe", offsetof(AnomalyUe5AhudServiceV1, subscribe)},
         {"unsubscribe", offsetof(AnomalyUe5AhudServiceV1, unsubscribe)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5FrameworkServiceV1",
        sizeof(AnomalyUe5FrameworkServiceV1),
        alignof(AnomalyUe5FrameworkServiceV1),
        {{"struct_size", offsetof(AnomalyUe5FrameworkServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUe5FrameworkServiceV1, service_version)},
         {"user", offsetof(AnomalyUe5FrameworkServiceV1, user)},
         {"game_thread_id", offsetof(AnomalyUe5FrameworkServiceV1, game_thread_id)},
         {"tick_sequence", offsetof(AnomalyUe5FrameworkServiceV1, tick_sequence)},
         {"is_game_thread", offsetof(AnomalyUe5FrameworkServiceV1, is_game_thread)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5NamesServiceV1",
        sizeof(AnomalyUe5NamesServiceV1),
        alignof(AnomalyUe5NamesServiceV1),
        {{"struct_size", offsetof(AnomalyUe5NamesServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUe5NamesServiceV1, service_version)},
         {"user", offsetof(AnomalyUe5NamesServiceV1, user)},
         {"resolve_utf8", offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5ObjectSnapshotV1",
        sizeof(AnomalyUe5ObjectSnapshotV1),
        alignof(AnomalyUe5ObjectSnapshotV1),
        {{"struct_size", offsetof(AnomalyUe5ObjectSnapshotV1, struct_size)},
         {"reserved", offsetof(AnomalyUe5ObjectSnapshotV1, reserved)},
         {"handle", offsetof(AnomalyUe5ObjectSnapshotV1, handle)},
         {"name_id", offsetof(AnomalyUe5ObjectSnapshotV1, name_id)},
         {"flags", offsetof(AnomalyUe5ObjectSnapshotV1, flags)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5ObjectsServiceV1",
        sizeof(AnomalyUe5ObjectsServiceV1),
        alignof(AnomalyUe5ObjectsServiceV1),
        {{"struct_size", offsetof(AnomalyUe5ObjectsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUe5ObjectsServiceV1, service_version)},
         {"user", offsetof(AnomalyUe5ObjectsServiceV1, user)},
         {"generation", offsetof(AnomalyUe5ObjectsServiceV1, generation)},
         {"count", offsetof(AnomalyUe5ObjectsServiceV1, count)},
         {"snapshot_at", offsetof(AnomalyUe5ObjectsServiceV1, snapshot_at)},
         {"snapshot_by_handle",
          offsetof(AnomalyUe5ObjectsServiceV1, snapshot_by_handle)},
         {"find_exact", offsetof(AnomalyUe5ObjectsServiceV1, find_exact)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5WorldSnapshotV1",
        sizeof(AnomalyUe5WorldSnapshotV1),
        alignof(AnomalyUe5WorldSnapshotV1),
        {{"struct_size", offsetof(AnomalyUe5WorldSnapshotV1, struct_size)},
         {"reserved", offsetof(AnomalyUe5WorldSnapshotV1, reserved)},
         {"handle", offsetof(AnomalyUe5WorldSnapshotV1, handle)},
         {"change_sequence", offsetof(AnomalyUe5WorldSnapshotV1, change_sequence)},
         {"name_id", offsetof(AnomalyUe5WorldSnapshotV1, name_id)},
         {"flags", offsetof(AnomalyUe5WorldSnapshotV1, flags)}},
        false);
    AppendStruct(
        output,
        "AnomalyUe5WorldServiceV1",
        sizeof(AnomalyUe5WorldServiceV1),
        alignof(AnomalyUe5WorldServiceV1),
        {{"struct_size", offsetof(AnomalyUe5WorldServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUe5WorldServiceV1, service_version)},
         {"user", offsetof(AnomalyUe5WorldServiceV1, user)},
         {"current", offsetof(AnomalyUe5WorldServiceV1, current)},
         {"snapshot", offsetof(AnomalyUe5WorldServiceV1, snapshot)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteBuildServiceV1",
        sizeof(AnomalyNteBuildServiceV1),
        alignof(AnomalyNteBuildServiceV1),
        {{"struct_size", offsetof(AnomalyNteBuildServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteBuildServiceV1, service_version)},
         {"user", offsetof(AnomalyNteBuildServiceV1, user)},
         {"build_id", offsetof(AnomalyNteBuildServiceV1, build_id)},
         {"feature_state", offsetof(AnomalyNteBuildServiceV1, feature_state)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteSessionSnapshotV1",
        sizeof(AnomalyNteSessionSnapshotV1),
        alignof(AnomalyNteSessionSnapshotV1),
        {{"struct_size", offsetof(AnomalyNteSessionSnapshotV1, struct_size)},
         {"state", offsetof(AnomalyNteSessionSnapshotV1, state)},
         {"sequence", offsetof(AnomalyNteSessionSnapshotV1, sequence)},
         {"world", offsetof(AnomalyNteSessionSnapshotV1, world)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteSessionEventV1",
        sizeof(AnomalyNteSessionEventV1),
        alignof(AnomalyNteSessionEventV1),
        {{"struct_size", offsetof(AnomalyNteSessionEventV1, struct_size)},
         {"kind", offsetof(AnomalyNteSessionEventV1, kind)},
         {"sequence", offsetof(AnomalyNteSessionEventV1, sequence)},
         {"tick_sequence", offsetof(AnomalyNteSessionEventV1, tick_sequence)},
         {"previous_world", offsetof(AnomalyNteSessionEventV1, previous_world)},
         {"world", offsetof(AnomalyNteSessionEventV1, world)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteSessionServiceV1",
        sizeof(AnomalyNteSessionServiceV1),
        alignof(AnomalyNteSessionServiceV1),
        {{"struct_size", offsetof(AnomalyNteSessionServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteSessionServiceV1, service_version)},
         {"user", offsetof(AnomalyNteSessionServiceV1, user)},
         {"snapshot", offsetof(AnomalyNteSessionServiceV1, snapshot)},
         {"next_event", offsetof(AnomalyNteSessionServiceV1, next_event)},
         {"latest_event_sequence", offsetof(AnomalyNteSessionServiceV1, latest_event_sequence)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePlayerSnapshotV1",
        sizeof(AnomalyNtePlayerSnapshotV1),
        alignof(AnomalyNtePlayerSnapshotV1),
        {{"struct_size", offsetof(AnomalyNtePlayerSnapshotV1, struct_size)},
         {"flags", offsetof(AnomalyNtePlayerSnapshotV1, flags)},
         {"handle", offsetof(AnomalyNtePlayerSnapshotV1, handle)},
         {"sequence", offsetof(AnomalyNtePlayerSnapshotV1, sequence)},
         {"position", offsetof(AnomalyNtePlayerSnapshotV1, position)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePlayerEspSnapshotV1",
        sizeof(AnomalyNtePlayerEspSnapshotV1),
        alignof(AnomalyNtePlayerEspSnapshotV1),
        {{"struct_size", offsetof(AnomalyNtePlayerEspSnapshotV1, struct_size)},
         {"flags", offsetof(AnomalyNtePlayerEspSnapshotV1, flags)},
         {"handle", offsetof(AnomalyNtePlayerEspSnapshotV1, handle)},
         {"sequence", offsetof(AnomalyNtePlayerEspSnapshotV1, sequence)},
         {"bounds_center", offsetof(AnomalyNtePlayerEspSnapshotV1, bounds_center)},
         {"bounds_extent", offsetof(AnomalyNtePlayerEspSnapshotV1, bounds_extent)},
         {"camera_position", offsetof(AnomalyNtePlayerEspSnapshotV1, camera_position)},
         {"camera_rotation", offsetof(AnomalyNtePlayerEspSnapshotV1, camera_rotation)},
         {"horizontal_fov_degrees", offsetof(AnomalyNtePlayerEspSnapshotV1, horizontal_fov_degrees)},
         {"reserved", offsetof(AnomalyNtePlayerEspSnapshotV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteCameraSnapshotV1",
        sizeof(AnomalyNteCameraSnapshotV1),
        alignof(AnomalyNteCameraSnapshotV1),
        {{"struct_size", offsetof(AnomalyNteCameraSnapshotV1, struct_size)},
         {"flags", offsetof(AnomalyNteCameraSnapshotV1, flags)},
         {"world", offsetof(AnomalyNteCameraSnapshotV1, world)},
         {"player", offsetof(AnomalyNteCameraSnapshotV1, player)},
         {"sequence", offsetof(AnomalyNteCameraSnapshotV1, sequence)},
         {"position", offsetof(AnomalyNteCameraSnapshotV1, position)},
         {"rotation", offsetof(AnomalyNteCameraSnapshotV1, rotation)},
         {"horizontal_fov_degrees", offsetof(AnomalyNteCameraSnapshotV1, horizontal_fov_degrees)},
         {"reserved", offsetof(AnomalyNteCameraSnapshotV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePlayerServiceV1",
        sizeof(AnomalyNtePlayerServiceV1),
        alignof(AnomalyNtePlayerServiceV1),
        {{"struct_size", offsetof(AnomalyNtePlayerServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNtePlayerServiceV1, service_version)},
         {"user", offsetof(AnomalyNtePlayerServiceV1, user)},
         {"snapshot", offsetof(AnomalyNtePlayerServiceV1, snapshot)},
         {"esp_snapshot", offsetof(AnomalyNtePlayerServiceV1, esp_snapshot)},
         {"camera_snapshot", offsetof(AnomalyNtePlayerServiceV1, camera_snapshot)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePlayerTeleportRequestV1",
        sizeof(AnomalyNtePlayerTeleportRequestV1),
        alignof(AnomalyNtePlayerTeleportRequestV1),
        {{"struct_size", offsetof(AnomalyNtePlayerTeleportRequestV1, struct_size)},
         {"flags", offsetof(AnomalyNtePlayerTeleportRequestV1, flags)},
         {"world", offsetof(AnomalyNtePlayerTeleportRequestV1, world)},
         {"player", offsetof(AnomalyNtePlayerTeleportRequestV1, player)},
         {"position", offsetof(AnomalyNtePlayerTeleportRequestV1, position)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePlayerTeleportServiceV1",
        sizeof(AnomalyNtePlayerTeleportServiceV1),
        alignof(AnomalyNtePlayerTeleportServiceV1),
        {{"struct_size", offsetof(AnomalyNtePlayerTeleportServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNtePlayerTeleportServiceV1, service_version)},
         {"user", offsetof(AnomalyNtePlayerTeleportServiceV1, user)},
         {"teleport",
          offsetof(AnomalyNtePlayerTeleportServiceV1, teleport)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteMapLandmarkSnapshotV1",
        sizeof(AnomalyNteMapLandmarkSnapshotV1),
        alignof(AnomalyNteMapLandmarkSnapshotV1),
        {{"struct_size", offsetof(AnomalyNteMapLandmarkSnapshotV1, struct_size)},
         {"flags", offsetof(AnomalyNteMapLandmarkSnapshotV1, flags)},
         {"sequence", offsetof(AnomalyNteMapLandmarkSnapshotV1, sequence)},
         {"point_type", offsetof(AnomalyNteMapLandmarkSnapshotV1, point_type)},
         {"floor", offsetof(AnomalyNteMapLandmarkSnapshotV1, floor)},
         {"world_position", offsetof(AnomalyNteMapLandmarkSnapshotV1, world_position)},
         {"destination", offsetof(AnomalyNteMapLandmarkSnapshotV1, destination)},
         {"teleport_id", offsetof(AnomalyNteMapLandmarkSnapshotV1, teleport_id)},
         {"world", offsetof(AnomalyNteMapLandmarkSnapshotV1, world)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteMapLandmarkTeleportRequestV1",
        sizeof(AnomalyNteMapLandmarkTeleportRequestV1),
        alignof(AnomalyNteMapLandmarkTeleportRequestV1),
        {{"struct_size", offsetof(AnomalyNteMapLandmarkTeleportRequestV1, struct_size)},
         {"mode", offsetof(AnomalyNteMapLandmarkTeleportRequestV1, mode)},
         {"sequence", offsetof(AnomalyNteMapLandmarkTeleportRequestV1, sequence)},
         {"index", offsetof(AnomalyNteMapLandmarkTeleportRequestV1, index)},
         {"flags", offsetof(AnomalyNteMapLandmarkTeleportRequestV1, flags)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteMapLandmarksServiceV1",
        sizeof(AnomalyNteMapLandmarksServiceV1),
        alignof(AnomalyNteMapLandmarksServiceV1),
        {{"struct_size", offsetof(AnomalyNteMapLandmarksServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteMapLandmarksServiceV1, service_version)},
         {"user", offsetof(AnomalyNteMapLandmarksServiceV1, user)},
         {"sequence", offsetof(AnomalyNteMapLandmarksServiceV1, sequence)},
         {"count", offsetof(AnomalyNteMapLandmarksServiceV1, count)},
         {"snapshot_at", offsetof(AnomalyNteMapLandmarksServiceV1, snapshot_at)},
         {"teleport", offsetof(AnomalyNteMapLandmarksServiceV1, teleport)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteNavigationServiceV1",
        sizeof(AnomalyNteNavigationServiceV1),
        alignof(AnomalyNteNavigationServiceV1),
        {{"struct_size", offsetof(AnomalyNteNavigationServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteNavigationServiceV1, service_version)},
         {"user", offsetof(AnomalyNteNavigationServiceV1, user)},
         {"move_to_location",
          offsetof(AnomalyNteNavigationServiceV1, move_to_location)},
         {"stop_movement",
          offsetof(AnomalyNteNavigationServiceV1, stop_movement)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePickupRequestV1",
        sizeof(AnomalyNtePickupRequestV1),
        alignof(AnomalyNtePickupRequestV1),
        {{"struct_size", offsetof(AnomalyNtePickupRequestV1, struct_size)},
         {"flags", offsetof(AnomalyNtePickupRequestV1, flags)},
         {"radius", offsetof(AnomalyNtePickupRequestV1, radius)},
         {"maximum_items", offsetof(AnomalyNtePickupRequestV1, maximum_items)},
         {"reserved", offsetof(AnomalyNtePickupRequestV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePickupSnapshotV1",
        sizeof(AnomalyNtePickupSnapshotV1),
        alignof(AnomalyNtePickupSnapshotV1),
        {{"struct_size", offsetof(AnomalyNtePickupSnapshotV1, struct_size)},
         {"flags", offsetof(AnomalyNtePickupSnapshotV1, flags)},
         {"sequence", offsetof(AnomalyNtePickupSnapshotV1, sequence)},
         {"state", offsetof(AnomalyNtePickupSnapshotV1, state)},
         {"status", offsetof(AnomalyNtePickupSnapshotV1, status)},
         {"nearby", offsetof(AnomalyNtePickupSnapshotV1, nearby)},
         {"triggered", offsetof(AnomalyNtePickupSnapshotV1, triggered)},
         {"confirmed", offsetof(AnomalyNtePickupSnapshotV1, confirmed)},
         {"checking", offsetof(AnomalyNtePickupSnapshotV1, checking)},
         {"unconfirmed", offsetof(AnomalyNtePickupSnapshotV1, unconfirmed)},
         {"skipped", offsetof(AnomalyNtePickupSnapshotV1, skipped)}},
        false);
    AppendStruct(
        output,
        "AnomalyNtePickupServiceV1",
        sizeof(AnomalyNtePickupServiceV1),
        alignof(AnomalyNtePickupServiceV1),
        {{"struct_size", offsetof(AnomalyNtePickupServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNtePickupServiceV1, service_version)},
         {"user", offsetof(AnomalyNtePickupServiceV1, user)},
         {"request_nearby", offsetof(AnomalyNtePickupServiceV1, request_nearby)},
         {"snapshot", offsetof(AnomalyNtePickupServiceV1, snapshot)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntityFrameV1",
        sizeof(AnomalyNteEntityFrameV1),
        alignof(AnomalyNteEntityFrameV1),
        {{"struct_size", offsetof(AnomalyNteEntityFrameV1, struct_size)},
         {"flags", offsetof(AnomalyNteEntityFrameV1, flags)},
         {"generation", offsetof(AnomalyNteEntityFrameV1, generation)},
         {"sequence", offsetof(AnomalyNteEntityFrameV1, sequence)},
         {"entity_count", offsetof(AnomalyNteEntityFrameV1, entity_count)},
         {"reserved", offsetof(AnomalyNteEntityFrameV1, reserved)},
         {"camera_position", offsetof(AnomalyNteEntityFrameV1, camera_position)},
         {"camera_rotation", offsetof(AnomalyNteEntityFrameV1, camera_rotation)},
         {"horizontal_fov_degrees", offsetof(AnomalyNteEntityFrameV1, horizontal_fov_degrees)},
         {"reserved2", offsetof(AnomalyNteEntityFrameV1, reserved2)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntitySnapshotV1",
        sizeof(AnomalyNteEntitySnapshotV1),
        alignof(AnomalyNteEntitySnapshotV1),
        {{"struct_size", offsetof(AnomalyNteEntitySnapshotV1, struct_size)},
         {"flags", offsetof(AnomalyNteEntitySnapshotV1, flags)},
         {"handle", offsetof(AnomalyNteEntitySnapshotV1, handle)},
         {"entity_id", offsetof(AnomalyNteEntitySnapshotV1, entity_id)},
         {"class_id", offsetof(AnomalyNteEntitySnapshotV1, class_id)},
         {"entity_name_id", offsetof(AnomalyNteEntitySnapshotV1, entity_name_id)},
         {"class_name_id", offsetof(AnomalyNteEntitySnapshotV1, class_name_id)},
         {"bounds_center", offsetof(AnomalyNteEntitySnapshotV1, bounds_center)},
         {"bounds_extent", offsetof(AnomalyNteEntitySnapshotV1, bounds_extent)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntityPageRequestV1",
        sizeof(AnomalyNteEntityPageRequestV1),
        alignof(AnomalyNteEntityPageRequestV1),
        {{"struct_size", offsetof(AnomalyNteEntityPageRequestV1, struct_size)},
         {"flags", offsetof(AnomalyNteEntityPageRequestV1, flags)},
         {"generation", offsetof(AnomalyNteEntityPageRequestV1, generation)},
         {"offset", offsetof(AnomalyNteEntityPageRequestV1, offset)},
         {"capacity", offsetof(AnomalyNteEntityPageRequestV1, capacity)},
         {"class_id", offsetof(AnomalyNteEntityPageRequestV1, class_id)},
         {"class_name_id", offsetof(AnomalyNteEntityPageRequestV1, class_name_id)},
         {"entity_name_id", offsetof(AnomalyNteEntityPageRequestV1, entity_name_id)},
         {"required_flags", offsetof(AnomalyNteEntityPageRequestV1, required_flags)},
         {"excluded_flags", offsetof(AnomalyNteEntityPageRequestV1, excluded_flags)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntityPageResultV1",
        sizeof(AnomalyNteEntityPageResultV1),
        alignof(AnomalyNteEntityPageResultV1),
        {{"struct_size", offsetof(AnomalyNteEntityPageResultV1, struct_size)},
         {"flags", offsetof(AnomalyNteEntityPageResultV1, flags)},
         {"generation", offsetof(AnomalyNteEntityPageResultV1, generation)},
         {"sequence", offsetof(AnomalyNteEntityPageResultV1, sequence)},
         {"total_matches", offsetof(AnomalyNteEntityPageResultV1, total_matches)},
         {"returned", offsetof(AnomalyNteEntityPageResultV1, returned)},
         {"next_offset", offsetof(AnomalyNteEntityPageResultV1, next_offset)},
         {"reserved", offsetof(AnomalyNteEntityPageResultV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntityComponentBoundsV1",
        sizeof(AnomalyNteEntityComponentBoundsV1),
        alignof(AnomalyNteEntityComponentBoundsV1),
        {{"struct_size", offsetof(AnomalyNteEntityComponentBoundsV1, struct_size)},
         {"flags", offsetof(AnomalyNteEntityComponentBoundsV1, flags)},
         {"entity", offsetof(AnomalyNteEntityComponentBoundsV1, entity)},
         {"sequence", offsetof(AnomalyNteEntityComponentBoundsV1, sequence)},
         {"bounds_center", offsetof(AnomalyNteEntityComponentBoundsV1, bounds_center)},
         {"bounds_extent", offsetof(AnomalyNteEntityComponentBoundsV1, bounds_extent)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntityBoolPropertyV1",
        sizeof(AnomalyNteEntityBoolPropertyV1),
        alignof(AnomalyNteEntityBoolPropertyV1),
        {{"struct_size", offsetof(AnomalyNteEntityBoolPropertyV1, struct_size)},
         {"flags", offsetof(AnomalyNteEntityBoolPropertyV1, flags)},
         {"entity", offsetof(AnomalyNteEntityBoolPropertyV1, entity)},
         {"sequence", offsetof(AnomalyNteEntityBoolPropertyV1, sequence)},
         {"value", offsetof(AnomalyNteEntityBoolPropertyV1, value)},
         {"reserved", offsetof(AnomalyNteEntityBoolPropertyV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEntitiesServiceV1",
        sizeof(AnomalyNteEntitiesServiceV1),
        alignof(AnomalyNteEntitiesServiceV1),
        {{"struct_size", offsetof(AnomalyNteEntitiesServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteEntitiesServiceV1, service_version)},
         {"user", offsetof(AnomalyNteEntitiesServiceV1, user)},
         {"frame", offsetof(AnomalyNteEntitiesServiceV1, frame)},
         {"snapshot_at", offsetof(AnomalyNteEntitiesServiceV1, snapshot_at)},
         {"class_name_utf8", offsetof(AnomalyNteEntitiesServiceV1, class_name_utf8)},
         {"entity_name_utf8", offsetof(AnomalyNteEntitiesServiceV1, entity_name_utf8)},
         {"page", offsetof(AnomalyNteEntitiesServiceV1, page)},
         {"component_bounds", offsetof(AnomalyNteEntitiesServiceV1, component_bounds)},
         {"bool_property", offsetof(AnomalyNteEntitiesServiceV1, bool_property)},
         {"fname_property_utf8", offsetof(AnomalyNteEntitiesServiceV1, fname_property_utf8)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteActorsServiceV1",
        sizeof(AnomalyNteActorsServiceV1),
        alignof(AnomalyNteActorsServiceV1),
        {{"struct_size", offsetof(AnomalyNteActorsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteActorsServiceV1, service_version)},
         {"user", offsetof(AnomalyNteActorsServiceV1, user)},
         {"frame", offsetof(AnomalyNteActorsServiceV1, frame)},
         {"snapshot_at", offsetof(AnomalyNteActorsServiceV1, snapshot_at)},
         {"class_name_utf8", offsetof(AnomalyNteActorsServiceV1, class_name_utf8)},
         {"entity_name_utf8", offsetof(AnomalyNteActorsServiceV1, entity_name_utf8)},
         {"page", offsetof(AnomalyNteActorsServiceV1, page)},
         {"component_bounds", offsetof(AnomalyNteActorsServiceV1, component_bounds)},
         {"bool_property", offsetof(AnomalyNteActorsServiceV1, bool_property)},
         {"fname_property_utf8", offsetof(AnomalyNteActorsServiceV1, fname_property_utf8)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteSnapshotMetricsV1",
        sizeof(AnomalyNteSnapshotMetricsV1),
        alignof(AnomalyNteSnapshotMetricsV1),
        {{"struct_size", offsetof(AnomalyNteSnapshotMetricsV1, struct_size)},
         {"flags", offsetof(AnomalyNteSnapshotMetricsV1, flags)},
         {"tick_sequence", offsetof(AnomalyNteSnapshotMetricsV1, tick_sequence)},
         {"session_event_sequence", offsetof(AnomalyNteSnapshotMetricsV1, session_event_sequence)},
         {"snapshot_tick_count", offsetof(AnomalyNteSnapshotMetricsV1, snapshot_tick_count)},
         {"latest_snapshot_cost_micros", offsetof(AnomalyNteSnapshotMetricsV1, latest_snapshot_cost_micros)},
         {"total_snapshot_cost_micros", offsetof(AnomalyNteSnapshotMetricsV1, total_snapshot_cost_micros)},
         {"max_snapshot_cost_micros", offsetof(AnomalyNteSnapshotMetricsV1, max_snapshot_cost_micros)},
         {"player_refresh_count", offsetof(AnomalyNteSnapshotMetricsV1, player_refresh_count)},
         {"player_cache_hit_count", offsetof(AnomalyNteSnapshotMetricsV1, player_cache_hit_count)},
         {"entity_refresh_count", offsetof(AnomalyNteSnapshotMetricsV1, entity_refresh_count)},
         {"entity_cache_hit_count", offsetof(AnomalyNteSnapshotMetricsV1, entity_cache_hit_count)},
         {"entity_page_request_count", offsetof(AnomalyNteSnapshotMetricsV1, entity_page_request_count)},
         {"entity_page_cache_hit_count", offsetof(AnomalyNteSnapshotMetricsV1, entity_page_cache_hit_count)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteMetricsServiceV1",
        sizeof(AnomalyNteMetricsServiceV1),
        alignof(AnomalyNteMetricsServiceV1),
        {{"struct_size", offsetof(AnomalyNteMetricsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteMetricsServiceV1, service_version)},
         {"user", offsetof(AnomalyNteMetricsServiceV1, user)},
         {"snapshot", offsetof(AnomalyNteMetricsServiceV1, snapshot)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEscMenuButtonSpecV1",
        sizeof(AnomalyNteEscMenuButtonSpecV1),
        alignof(AnomalyNteEscMenuButtonSpecV1),
        {{"struct_size", offsetof(AnomalyNteEscMenuButtonSpecV1, struct_size)},
         {"flags", offsetof(AnomalyNteEscMenuButtonSpecV1, flags)},
         {"id", offsetof(AnomalyNteEscMenuButtonSpecV1, id)},
         {"label", offsetof(AnomalyNteEscMenuButtonSpecV1, label)},
         {"icon_format", offsetof(AnomalyNteEscMenuButtonSpecV1, icon_format)},
         {"reserved", offsetof(AnomalyNteEscMenuButtonSpecV1, reserved)},
         {"icon_bytes", offsetof(AnomalyNteEscMenuButtonSpecV1, icon_bytes)}},
        false);
    AppendStruct(
        output,
        "AnomalyNteEscMenuButtonServiceV1",
        sizeof(AnomalyNteEscMenuButtonServiceV1),
        alignof(AnomalyNteEscMenuButtonServiceV1),
        {{"struct_size", offsetof(AnomalyNteEscMenuButtonServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNteEscMenuButtonServiceV1, service_version)},
         {"user", offsetof(AnomalyNteEscMenuButtonServiceV1, user)},
         {"register_button", offsetof(AnomalyNteEscMenuButtonServiceV1, register_button)},
         {"unregister_button", offsetof(AnomalyNteEscMenuButtonServiceV1, unregister_button)}},
        true);
    output.append("  },\n");

    output.append("  \"services\": [\n");
    AppendService(output, ANOMALY_CORE_SERVICE_V1_ID,
        ANOMALY_CORE_SERVICE_V1_VERSION, "AnomalyCoreServiceV1", false);
    AppendService(output, ANOMALY_PLUGIN_STATE_SERVICE_V1_ID,
        ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION, "AnomalyPluginStateServiceV1", false);
    AppendService(output, ANOMALY_CONFIG_SERVICE_V1_ID,
        ANOMALY_CONFIG_SERVICE_V1_VERSION, "AnomalyConfigServiceV1", false);
    AppendService(output, ANOMALY_STORAGE_SERVICE_V1_ID,
        ANOMALY_STORAGE_SERVICE_V1_VERSION, "AnomalyStorageServiceV1", false);
    AppendService(output, ANOMALY_RUNTIME_INFO_SERVICE_V1_ID,
        ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION, "AnomalyRuntimeInfoServiceV1", false);
    AppendService(output, ANOMALY_DIAGNOSTICS_SERVICE_V1_ID,
        ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION, "AnomalyDiagnosticsServiceV1", false);
    AppendService(output, ANOMALY_SCHEDULER_SERVICE_V1_ID,
        ANOMALY_SCHEDULER_SERVICE_V1_VERSION, "AnomalySchedulerServiceV1", false);
    AppendService(output, ANOMALY_IPC_SERVICE_V1_ID,
        ANOMALY_IPC_SERVICE_V1_VERSION, "AnomalyIpcServiceV1", false);
    AppendService(output, ANOMALY_WEBSOCKET_SERVICE_V1_ID,
        ANOMALY_WEBSOCKET_SERVICE_V1_VERSION, "AnomalyWebSocketServiceV1", false);
    AppendService(output, ANOMALY_COMMANDS_SERVICE_V1_ID,
        ANOMALY_COMMANDS_SERVICE_V1_VERSION, "AnomalyCommandsServiceV1", false);
    AppendService(output, ANOMALY_NOTIFICATIONS_SERVICE_V1_ID,
        ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION, "AnomalyNotificationsServiceV1", false);
    AppendService(output, ANOMALY_SIGNATURE_SERVICE_V1_ID,
        ANOMALY_SIGNATURE_SERVICE_V1_VERSION, "AnomalySignatureServiceV1", false);
    AppendService(output, ANOMALY_HOOK_SERVICE_V1_ID,
        ANOMALY_HOOK_SERVICE_V1_VERSION, "AnomalyHookServiceV1", false);
    AppendService(output, ANOMALY_PATCH_SERVICE_V1_ID,
        ANOMALY_PATCH_SERVICE_V1_VERSION, "AnomalyPatchServiceV1", false);
    AppendService(output, ANOMALY_UI_SERVICE_V1_ID,
        ANOMALY_UI_SERVICE_V1_VERSION, "AnomalyUiServiceV1", false);
    AppendService(output, ANOMALY_WINDOW_SERVICE_V1_ID,
        ANOMALY_WINDOW_SERVICE_V1_VERSION, "AnomalyWindowServiceV1", false);
    AppendService(output, ANOMALY_FONT_SERVICE_V1_ID,
        ANOMALY_FONT_SERVICE_V1_VERSION, "AnomalyFontServiceV1", false);
    AppendService(output, ANOMALY_TEXTURE_SERVICE_V1_ID,
        ANOMALY_TEXTURE_SERVICE_V1_VERSION, "AnomalyTextureServiceV1", false);
    AppendService(output, ANOMALY_INPUT_SERVICE_V1_ID,
        ANOMALY_INPUT_SERVICE_V1_VERSION, "AnomalyInputServiceV1", false);
    AppendService(output, ANOMALY_LOCALIZATION_SERVICE_V1_ID,
        ANOMALY_LOCALIZATION_SERVICE_V1_VERSION, "AnomalyLocalizationServiceV1", false);
    AppendService(output, ANOMALY_UE5_BUILD_SERVICE_V1_ID,
        ANOMALY_UE5_BUILD_SERVICE_V1_VERSION, "AnomalyUe5BuildServiceV1", false);
    AppendService(output, ANOMALY_UE5_AHUD_SERVICE_V1_ID,
        ANOMALY_UE5_AHUD_SERVICE_V1_VERSION, "AnomalyUe5AhudServiceV1", false);
    AppendService(
        output, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID,
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION, "AnomalyUe5FrameworkServiceV1", false);
    AppendService(output, ANOMALY_UE5_NAMES_SERVICE_V1_ID,
        ANOMALY_UE5_NAMES_SERVICE_V1_VERSION, "AnomalyUe5NamesServiceV1", false);
    AppendService(
        output, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
        ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION, "AnomalyUe5ObjectsServiceV1", false);
    AppendService(output, ANOMALY_UE5_WORLD_SERVICE_V1_ID,
        ANOMALY_UE5_WORLD_SERVICE_V1_VERSION, "AnomalyUe5WorldServiceV1", false);
    AppendService(output, ANOMALY_NTE_BUILD_SERVICE_V1_ID,
        ANOMALY_NTE_BUILD_SERVICE_V1_VERSION, "AnomalyNteBuildServiceV1", false);
    AppendService(output, ANOMALY_NTE_SESSION_SERVICE_V1_ID,
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION, "AnomalyNteSessionServiceV1", false);
    AppendService(output, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION, "AnomalyNtePlayerServiceV1", false);
    AppendService(output, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION, "AnomalyNtePlayerTeleportServiceV1", false);
    AppendService(output, ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID,
        ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION, "AnomalyNteMapLandmarksServiceV1", false);
    AppendService(output, ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION, "AnomalyNteNavigationServiceV1", false);
    AppendService(output, ANOMALY_NTE_PICKUP_SERVICE_V1_ID,
        ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION, "AnomalyNtePickupServiceV1", false);
    AppendService(output, ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
        ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION, "AnomalyNteEntitiesServiceV1", false);
    AppendService(output, ANOMALY_NTE_ACTORS_SERVICE_V1_ID,
        ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION, "AnomalyNteActorsServiceV1", false);
    AppendService(output, ANOMALY_NTE_METRICS_SERVICE_V1_ID,
        ANOMALY_NTE_METRICS_SERVICE_V1_VERSION, "AnomalyNteMetricsServiceV1", false);
    AppendService(output, ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_ID,
        ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION,
        "AnomalyNteEscMenuButtonServiceV1", true);
    output.append("  ]\n");
    output.append("}\n");
    return output;
}

bool WriteFile(const char* path, const std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return stream.good();
}

bool ReadFile(const char* path, std::string& contents) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

void PrintUsage(const char* executable) {
    std::cerr << "Usage: " << executable << " [--write PATH | --check PATH]\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
    const std::string snapshot = BuildSnapshot();
    if (argc == 1) {
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            std::cerr << "Failed to set stdout to binary mode\n";
            return 1;
        }
        std::cout.write(snapshot.data(), static_cast<std::streamsize>(snapshot.size()));
        return std::cout.good() ? 0 : 1;
    }
    if (argc != 3) {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::string_view option(argv[1]);
    if (option == "--write") {
        if (!WriteFile(argv[2], snapshot)) {
            std::cerr << "Failed to write ABI snapshot: " << argv[2] << '\n';
            return 1;
        }
        return 0;
    }
    if (option == "--check") {
        std::string expected;
        if (!ReadFile(argv[2], expected)) {
            std::cerr << "Failed to read ABI snapshot: " << argv[2] << '\n';
            return 1;
        }
        if (expected != snapshot) {
            std::size_t offset = 0u;
            while (offset < expected.size() && offset < snapshot.size() &&
                   expected[offset] == snapshot[offset]) {
                ++offset;
            }
            std::cerr << "ABI snapshot mismatch at byte " << offset << ": " << argv[2] << '\n';
            return 1;
        }
        return 0;
    }

    PrintUsage(argv[0]);
    return 2;
}
