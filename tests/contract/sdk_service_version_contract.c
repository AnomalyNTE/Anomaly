#include "anomaly/sdk/services/ui_resources.h"
#include "anomaly/sdk/anomaly_sdk.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(_WIN32) || !defined(_WIN64)
#error "The frozen SDK ABI contract targets Windows x64"
#endif

#if defined(__cplusplus)
#error "Compile this contract as C11"
#endif

#define ANOMALY_TYPE_IS(expression, type) _Generic((expression), type: 1, default: 0)
#define ANOMALY_ASSERT_OFFSET(type, member, expected) \
    _Static_assert(offsetof(type, member) == (expected), #type "." #member " offset")
#define ANOMALY_ASSERT_LAYOUT(type, expected_size, expected_alignment) \
    _Static_assert(sizeof(type) == (expected_size), #type " size"); \
    _Static_assert(_Alignof(type) == (expected_alignment), #type " alignment")
#define ANOMALY_ASSERT_TAIL(type, member) \
    _Static_assert( \
        offsetof(type, member) + sizeof(((type*)0)->member) == sizeof(type), \
        #type " must only extend after " #member)
#define ANOMALY_ASSERT_SERVICE_PREFIX(type) \
    ANOMALY_ASSERT_OFFSET(type, struct_size, 0); \
    ANOMALY_ASSERT_OFFSET(type, service_version, 4); \
    ANOMALY_ASSERT_OFFSET(type, user, 8)

typedef void* (ANOMALY_CALL *ContractAllocateFn)(void*, size_t, size_t);
typedef void* (ANOMALY_CALL *ContractReallocateFn)(void*, void*, size_t, size_t);
typedef void (ANOMALY_CALL *ContractReleaseFn)(void*, void*, size_t);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractQueryServiceFn)(
    void*, AnomalyStringViewV1, uint32_t, const void**);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractPluginEntryFn)(AnomalyPluginDescriptorV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractOnLoadFn)(const AnomalyHostApiV1*, void**);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractStatusContextFn)(void*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractOnStopFn)(void*, uint32_t);
typedef void (ANOMALY_CALL *ContractVoidContextFn)(void*);
typedef void (ANOMALY_CALL *ContractUpdateFn)(void*, double);
typedef void (ANOMALY_CALL *ContractDrawFn)(void*, const AnomalyUiServiceV1*);

typedef void (ANOMALY_CALL *ContractLogFn)(void*, uint32_t, AnomalyStringViewV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractReadMemoryFn)(
    void*, uintptr_t, AnomalyMutableByteSpanV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractWriteMemoryFn)(
    void*, uintptr_t, AnomalyByteSpanV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractStringOutputFn)(void*, char*, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractConfigRegisterFn)(
    void*, AnomalyStringViewV1, uint32_t, AnomalyByteSpanV1, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractConfigReadFn)(
    void*, AnomalyStringViewV1, uint32_t*, AnomalyMutableByteSpanV1, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractConfigWriteFn)(
    void*, AnomalyStringViewV1, uint32_t, AnomalyByteSpanV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractConfigMigrateFn)(
    void*, AnomalyStringViewV1, AnomalyConfigMigrationV1, void*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractStorageReadFn)(
    void*, AnomalyStringViewV1, AnomalyMutableByteSpanV1, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractStorageWriteFn)(
    void*, AnomalyStringViewV1, AnomalyByteSpanV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractStringOnlyFn)(void*, AnomalyStringViewV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRuntimeInfoSnapshotFn)(
    void*, AnomalyRuntimeInfoV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRegisterSelfTestFn)(
    void*, AnomalyStringViewV1, AnomalyDiagnosticSelfTestV1, void*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRunSelfTestFn)(
    void*, AnomalyStringViewV1, AnomalyMutableByteSpanV1, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractSnapshotJsonFn)(
    void*, AnomalyMutableByteSpanV1, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractScheduleFn)(
    void*, uint32_t, AnomalyTaskCallbackV1, void*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractHandleFn)(void*, AnomalyGenerationHandleV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRegisterCommandFn)(
    void*, AnomalyStringViewV1, AnomalyStringViewV1, AnomalyCommandCallbackV1, void*,
    AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractInvokeCommandFn)(
    void*, AnomalyStringViewV1, AnomalyStringViewV1, AnomalyMutableByteSpanV1, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractPostNotificationFn)(
    void*, AnomalyNotificationSeverityV1, AnomalyStringViewV1, AnomalyStringViewV1, uint32_t,
    AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractResolveSignatureFn)(
    void*, AnomalyStringViewV1, AnomalyStringViewV1, AnomalyStringViewV1, uintptr_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractCreateHookFn)(
    void*, const AnomalyHookRequestV1*, uintptr_t*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractBeginHookFn)(
    void*, AnomalyGenerationHandleV1, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractApplyPatchFn)(
    void*, uintptr_t, AnomalyByteSpanV1, AnomalyStringViewV1, AnomalyGenerationHandleV1*);

typedef void (ANOMALY_CALL *ContractSetNextWindowSizeFn)(void*, float, float, uint32_t);
typedef int (ANOMALY_CALL *ContractBeginWindowFn)(void*, AnomalyStringViewV1, int*, uint32_t);
typedef void (ANOMALY_CALL *ContractEndWindowFn)(void*);
typedef void (ANOMALY_CALL *ContractTextFn)(void*, AnomalyStringViewV1);
typedef int (ANOMALY_CALL *ContractButtonFn)(void*, AnomalyStringViewV1, float, float);
typedef int (ANOMALY_CALL *ContractDrawEntityBboxFn)(
    void*, const AnomalyEspCameraV1*, const AnomalyEspEntityBoundsV1*,
    const AnomalyEspBoxStyleV1*);
typedef int (ANOMALY_CALL *ContractCheckboxFn)(void*, AnomalyStringViewV1, int*);
typedef int (ANOMALY_CALL *ContractSliderFloatFn)(
    void*, AnomalyStringViewV1, float*, float, float);
typedef int (ANOMALY_CALL *ContractColorEdit4Fn)(void*, AnomalyStringViewV1, float[4]);
typedef int (ANOMALY_CALL *ContractDrawEntityLabelFn)(
    void*, const AnomalyEspCameraV1*, const AnomalyEspEntityBoundsV1*,
    AnomalyStringViewV1, uint32_t);
typedef void (ANOMALY_CALL *ContractUiSeparatorFn)(void*);
typedef int (ANOMALY_CALL *ContractUiBeginChildFn)(
    void*, AnomalyStringViewV1, float, float, uint32_t);
typedef int (ANOMALY_CALL *ContractUiBeginTableFn)(
    void*, AnomalyStringViewV1, int32_t, uint32_t, float, float);
typedef void (ANOMALY_CALL *ContractUiTableNextRowFn)(void*);
typedef int (ANOMALY_CALL *ContractUiTableNextColumnFn)(void*);
typedef int (ANOMALY_CALL *ContractUiBeginMenuFn)(void*, AnomalyStringViewV1, int);
typedef void (ANOMALY_CALL *ContractUiOpenPopupFn)(void*, AnomalyStringViewV1);
typedef int (ANOMALY_CALL *ContractUiBeginPopupModalFn)(
    void*, AnomalyStringViewV1, int*, uint32_t);
typedef int (ANOMALY_CALL *ContractUiFilterMatchFn)(
    void*, AnomalyStringViewV1, AnomalyStringViewV1);
typedef uint32_t (ANOMALY_CALL *ContractUiFrameStateFn)(void*);
typedef void (ANOMALY_CALL *ContractUiSetNextWindowSizeConstraintsFn)(
    void*, float, float, float, float);
typedef void (ANOMALY_CALL *ContractUiGetWindowSizeFn)(void*, float*, float*);
typedef int (ANOMALY_CALL *ContractUiInputUInt32Fn)(
    void*, AnomalyStringViewV1, uint32_t*, uint32_t, uint32_t);
typedef int (ANOMALY_CALL *ContractUiInputDoubleFn)(
    void*, AnomalyStringViewV1, double*, double, double);
typedef int (ANOMALY_CALL *ContractUiDeveloperModeEnabledFn)(void*);
typedef int (ANOMALY_CALL *ContractUiInputTextFn)(
    void*, AnomalyStringViewV1, char*, size_t, uint32_t);
typedef int (ANOMALY_CALL *ContractUiButtonEnabledFn)(
    void*, AnomalyStringViewV1, float, float, int);

typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRegisterWindowFn)(
    void*, const AnomalyWindowSpecV1*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractWindowOpenFn)(
    void*, AnomalyGenerationHandleV1, int32_t);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractWindowStateFn)(
    void*, AnomalyGenerationHandleV1, AnomalyWindowStateV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractWindowBeginFn)(
    void*, AnomalyGenerationHandleV1, uint32_t, int32_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractFontRequestFn)(
    void*, const AnomalyFontRequestV1*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractFontStateFn)(
    void*, AnomalyGenerationHandleV1, AnomalyFontStateV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractFontPushFn)(
    void*, AnomalyGenerationHandleV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractTextureRequestFn)(
    void*, const AnomalyTextureRequestV1*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractTextureStateFn)(
    void*, AnomalyGenerationHandleV1, AnomalyTextureStateV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractTextureDrawFn)(
    void*, AnomalyGenerationHandleV1, float, float, uint32_t);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractInputSnapshotFn)(
    void*, AnomalyInputSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractInputWasPressedFn)(
    void*, uint32_t, int32_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRegisterHotkeyFn)(
    void*, const AnomalyHotkeySpecV1*, AnomalyHotkeyCallbackV1, void*,
    AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractInputCaptureStateFn)(void*, uint32_t*);
typedef void (ANOMALY_CALL *ContractHotkeyCallbackFn)(
    void*, AnomalyGenerationHandleV1, const AnomalyInputSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractLocalizationTranslateFn)(
    void*, AnomalyStringViewV1, AnomalyStringViewV1, const AnomalyStringViewV1*,
    size_t, char*, size_t*);

typedef uint32_t (ANOMALY_CALL *ContractFeatureStateFn)(void*, AnomalyStringViewV1);
typedef uint32_t (ANOMALY_CALL *ContractUint32ContextFn)(void*);
typedef uint64_t (ANOMALY_CALL *ContractUint64ContextFn)(void*);
typedef int (ANOMALY_CALL *ContractIntContextFn)(void*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractResolveNameFn)(
    void*, uint32_t, char*, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractObjectSnapshotFn)(
    void*, uint32_t, AnomalyUe5ObjectSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractObjectSnapshotByHandleFn)(
    void*, AnomalyGenerationHandleV1, AnomalyUe5ObjectSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractWorldCurrentFn)(
    void*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractWorldSnapshotFn)(
    void*, AnomalyGenerationHandleV1, AnomalyUe5WorldSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteSessionSnapshotFn)(
    void*, AnomalyNteSessionSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteSessionNextEventFn)(
    void*, uint64_t, AnomalyNteSessionEventV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNtePlayerSnapshotFn)(
    void*, AnomalyNtePlayerSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNtePlayerEspSnapshotFn)(
    void*, AnomalyNtePlayerEspSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteCameraSnapshotFn)(
    void*, AnomalyNteCameraSnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNtePlayerTeleportFn)(
    void*, const AnomalyNtePlayerTeleportRequestV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntityFrameFn)(
    void*, AnomalyNteEntityFrameV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntitySnapshotFn)(
    void*, uint64_t, uint32_t, AnomalyNteEntitySnapshotV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntityNameFn)(
    void*, uint64_t, char*, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntityPageFn)(
    void*, const AnomalyNteEntityPageRequestV1*, AnomalyNteEntitySnapshotV1*,
    AnomalyNteEntityPageResultV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntityComponentBoundsFn)(
    void*, AnomalyGenerationHandleV1, AnomalyStringViewV1,
    AnomalyNteEntityComponentBoundsV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntityBoolPropertyFn)(
    void*, AnomalyGenerationHandleV1, AnomalyStringViewV1,
    AnomalyNteEntityBoolPropertyV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEntityFNamePropertyFn)(
    void*, AnomalyGenerationHandleV1, AnomalyStringViewV1, char*, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteMetricsSnapshotFn)(
    void*, AnomalyNteSnapshotMetricsV1*);
typedef uint32_t (ANOMALY_CALL *ContractNteEscMenuButtonCallbackFn)(
    void*, AnomalyGenerationHandleV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractNteEscMenuButtonRegisterFn)(
    void*, const AnomalyNteEscMenuButtonSpecV1*, AnomalyNteEscMenuButtonCallbackV1,
    void*, AnomalyGenerationHandleV1*);

_Static_assert(sizeof(void*) == 8, "the ABI requires 64-bit pointers");
_Static_assert(sizeof(size_t) == 8, "the ABI requires 64-bit size_t");
_Static_assert(sizeof(uintptr_t) == 8, "the ABI requires 64-bit uintptr_t");
_Static_assert(ANOMALY_PLUGIN_API_V1_MAJOR == 1u, "plugin API major changed");
_Static_assert(ANOMALY_PLUGIN_API_V1_MINOR == 0u, "plugin API minor policy changed");
_Static_assert(sizeof(ANOMALY_PLUGIN_V1_ENTRY_NAME) == 21, "plugin entry name length changed");
_Static_assert(ANOMALY_CORE_SERVICE_V1_VERSION == 1u, "core service version changed");
_Static_assert(ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION == 1u,
    "plugin state service version changed");
_Static_assert(ANOMALY_CONFIG_SERVICE_V1_VERSION == 1u, "config service version changed");
_Static_assert(ANOMALY_STORAGE_SERVICE_V1_VERSION == 1u, "storage service version changed");
_Static_assert(ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION == 1u,
    "runtime info service version changed");
_Static_assert(ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION == 1u,
    "diagnostics service version changed");
_Static_assert(ANOMALY_SCHEDULER_SERVICE_V1_VERSION == 1u,
    "scheduler service version changed");
_Static_assert(ANOMALY_COMMANDS_SERVICE_V1_VERSION == 1u, "commands service version changed");
_Static_assert(ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION == 1u,
    "notifications service version changed");
_Static_assert(ANOMALY_SIGNATURE_SERVICE_V1_VERSION == 1u,
    "signature service version changed");
_Static_assert(ANOMALY_HOOK_SERVICE_V1_VERSION == 1u, "hook service version changed");
_Static_assert(ANOMALY_PATCH_SERVICE_V1_VERSION == 1u, "patch service version changed");
_Static_assert(ANOMALY_UI_SERVICE_V1_VERSION == 1u, "UI service version changed");
_Static_assert(ANOMALY_WINDOW_SERVICE_V1_VERSION == 1u, "window service version changed");
_Static_assert(ANOMALY_FONT_SERVICE_V1_VERSION == 1u, "font service version changed");
_Static_assert(ANOMALY_TEXTURE_SERVICE_V1_VERSION == 1u, "texture service version changed");
_Static_assert(ANOMALY_INPUT_SERVICE_V1_VERSION == 1u, "input service version changed");
_Static_assert(ANOMALY_LOCALIZATION_SERVICE_V1_VERSION == 1u,
    "localization service version changed");
_Static_assert(ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION == 1u,
    "NTE entities service version changed");
_Static_assert(ANOMALY_UE5_BUILD_SERVICE_V1_VERSION == 1u, "UE5 build version changed");
_Static_assert(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION == 1u, "UE5 framework version changed");
_Static_assert(ANOMALY_UE5_NAMES_SERVICE_V1_VERSION == 1u, "UE5 names version changed");
_Static_assert(ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION == 1u, "UE5 objects version changed");
_Static_assert(ANOMALY_UE5_WORLD_SERVICE_V1_VERSION == 1u, "UE5 world version changed");
_Static_assert(ANOMALY_NTE_BUILD_SERVICE_V1_VERSION == 1u, "NTE build version changed");
_Static_assert(ANOMALY_NTE_SESSION_SERVICE_V1_VERSION == 1u, "NTE session version changed");
_Static_assert(ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION == 1u, "NTE player version changed");
_Static_assert(ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION == 1u,
    "NTE player-teleport service version changed");
_Static_assert(ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY == 256u,
    "NTE entity page capacity changed");
_Static_assert(ANOMALY_NTE_METRICS_SERVICE_V1_VERSION == 1u, "NTE metrics version changed");
_Static_assert(ANOMALY_NTE_METRICS_V1_VALID == 1u, "NTE metrics valid flag changed");
_Static_assert(ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION == 1u,
    "NTE ESC menu button version changed");
_Static_assert(sizeof(AnomalyNteEscMenuButtonFlagsV1) == 4,
    "NTE ESC menu button flags width changed");
_Static_assert(sizeof(AnomalyNteEscMenuButtonIconFormatV1) == 4,
    "NTE ESC menu button icon format width changed");
_Static_assert(sizeof(AnomalyNteEscMenuButtonResultV1) == 4,
    "NTE ESC menu button result width changed");
_Static_assert(ANOMALY_NTE_ESC_MENU_BUTTON_V1_NONE == 0,
    "NTE ESC menu button flag changed");
_Static_assert(ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE == 0,
    "NTE ESC menu button empty icon format changed");
_Static_assert(ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_PNG == 1,
    "NTE ESC menu button PNG icon format changed");
_Static_assert(ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_NONE == 0,
    "NTE ESC menu button callback result changed");
_Static_assert(ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_EXPAND_ANOMALY == 1,
    "NTE ESC menu button host action changed");

_Static_assert(ANOMALY_STATUS_V1_OK == 0, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_INVALID_ARGUMENT == 1, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_UNAVAILABLE == 2, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_NOT_FOUND == 3, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL == 4, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_FAILED == 5, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_TIMEOUT == 6, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_PERMISSION_DENIED == 7, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_CONFLICT == 8, "status value changed");
_Static_assert(ANOMALY_STATUS_V1_CANCELLED == 9, "status value changed");
_Static_assert(ANOMALY_FEATURE_V1_UNAVAILABLE == 0, "feature value changed");
_Static_assert(ANOMALY_FEATURE_V1_AVAILABLE == 1, "feature value changed");
_Static_assert(ANOMALY_NTE_SESSION_V1_UNKNOWN == 0, "session value changed");
_Static_assert(ANOMALY_NTE_SESSION_V1_LOADING == 1, "session value changed");
_Static_assert(ANOMALY_NTE_SESSION_V1_WORLD_READY == 2, "session value changed");
_Static_assert(sizeof(AnomalyNteSessionEventKindV1) == 4, "session event enum width changed");
_Static_assert(ANOMALY_NTE_SESSION_EVENT_V1_NONE == 0, "session event value changed");
_Static_assert(ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY == 1, "session event value changed");
_Static_assert(ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED == 2, "session event value changed");
_Static_assert(ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE == 3, "session event value changed");
_Static_assert(sizeof(AnomalyNteSnapshotFlagsV1) == 4, "NTE snapshot enum width changed");
_Static_assert(ANOMALY_NTE_SNAPSHOT_V1_INVALID == 0u, "NTE invalid flag changed");
_Static_assert(ANOMALY_NTE_SNAPSHOT_V1_VALID == 0x20000000u, "NTE valid flag changed");
_Static_assert(ANOMALY_NTE_SNAPSHOT_V1_STALE == 0x40000000u, "NTE stale flag changed");
_Static_assert(ANOMALY_NTE_SNAPSHOT_V1_PARTIAL == 0x80000000u, "NTE partial flag changed");
_Static_assert(sizeof(AnomalyStatusCodeV1) == 4, "status enum width changed");
_Static_assert(sizeof(AnomalyFeatureStateV1) == 4, "feature enum width changed");
_Static_assert(sizeof(AnomalyNteSessionStateV1) == 4, "session enum width changed");
_Static_assert(sizeof(AnomalyEspBoxFlagsV1) == 4, "ESP box enum width changed");
_Static_assert(sizeof(AnomalyNteEntityFlagsV1) == 4, "NTE entity enum width changed");
_Static_assert(sizeof(AnomalyNotificationSeverityV1) == 4,
    "notification severity enum width changed");
_Static_assert(sizeof(AnomalyHookKindV1) == 4, "hook kind enum width changed");
_Static_assert(sizeof(AnomalyUiFrameStateV1) == 4, "UI frame-state enum width changed");
_Static_assert(sizeof(AnomalyUiTextInputFlagsV1) == 4, "UI text-input enum width changed");
_Static_assert(sizeof(AnomalyWindowFlagsV1) == 4, "window flags enum width changed");
_Static_assert(sizeof(AnomalyGlyphRangeV1) == 4, "glyph-range enum width changed");
_Static_assert(sizeof(AnomalyFontStateFlagsV1) == 4,
    "font state flags enum width changed");
_Static_assert(sizeof(AnomalyTextureFormatV1) == 4, "texture format enum width changed");
_Static_assert(sizeof(AnomalyTextureStateFlagsV1) == 4,
    "texture state flags enum width changed");
_Static_assert(sizeof(AnomalyInputModifiersV1) == 4, "input modifiers enum width changed");
_Static_assert(sizeof(AnomalyInputCaptureFlagsV1) == 4,
    "input capture flags enum width changed");
_Static_assert(sizeof(AnomalyHotkeyFlagsV1) == 4, "hotkey flags enum width changed");
_Static_assert(ANOMALY_NOTIFICATION_V1_INFO == 0, "notification info value changed");
_Static_assert(ANOMALY_NOTIFICATION_V1_WARNING == 1, "notification warning value changed");
_Static_assert(ANOMALY_NOTIFICATION_V1_ERROR == 2, "notification error value changed");
_Static_assert(ANOMALY_HOOK_V1_FUNCTION == 1, "hook function value changed");
_Static_assert(ANOMALY_HOOK_V1_IAT == 2, "hook IAT value changed");
_Static_assert(ANOMALY_HOOK_V1_EXPORT == 3, "hook export value changed");
_Static_assert(ANOMALY_HOOK_V1_VTABLE == 4, "hook vtable value changed");
_Static_assert(ANOMALY_NTE_ENTITY_V1_STATIC == 1u, "NTE static flag changed");
_Static_assert(ANOMALY_NTE_ENTITY_V1_STATIONARY == 2u, "NTE stationary flag changed");
_Static_assert(ANOMALY_NTE_ENTITY_V1_MOVABLE == 4u, "NTE movable flag changed");
_Static_assert(ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER == 8u, "NTE local flag changed");
_Static_assert(ANOMALY_UI_FRAME_V1_ITEM_HOVERED == 1u, "UI hovered frame flag changed");
_Static_assert(ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED == 2u, "UI focused frame flag changed");
_Static_assert(ANOMALY_UI_FRAME_V1_ITEM_ACTIVE == 4u, "UI active frame flag changed");
_Static_assert(ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE == 8u,
    "UI mouse capture frame flag changed");
_Static_assert(ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD == 16u,
    "UI keyboard capture frame flag changed");
_Static_assert(ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT == 32u,
    "UI text capture frame flag changed");
_Static_assert(ANOMALY_UI_TEXT_INPUT_V1_NONE == 0u, "UI text-input none flag changed");
_Static_assert(ANOMALY_UI_TEXT_INPUT_V1_DIGITS == 1u, "UI text-input digits flag changed");
_Static_assert(ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS == 1u,
    "window no-saved-settings flag changed");
_Static_assert(ANOMALY_WINDOW_V1_NO_COLLAPSE == 2u, "window no-collapse flag changed");
_Static_assert(ANOMALY_GLYPH_RANGE_V1_DEFAULT == 0, "default glyph range changed");
_Static_assert(ANOMALY_GLYPH_RANGE_V1_LATIN == 1, "Latin glyph range changed");
_Static_assert(ANOMALY_GLYPH_RANGE_V1_CYRILLIC == 2, "Cyrillic glyph range changed");
_Static_assert(ANOMALY_GLYPH_RANGE_V1_JAPANESE == 3, "Japanese glyph range changed");
_Static_assert(ANOMALY_GLYPH_RANGE_V1_CHINESE_FULL == 4,
    "Chinese glyph range changed");
_Static_assert(ANOMALY_FONT_STATE_V1_NONE == 0u, "none font state changed");
_Static_assert(ANOMALY_FONT_STATE_V1_QUEUED == 1u, "queued font state changed");
_Static_assert(ANOMALY_FONT_STATE_V1_READY == 2u, "ready font state changed");
_Static_assert(ANOMALY_FONT_STATE_V1_FAILED == 4u, "failed font state changed");
_Static_assert(ANOMALY_FONT_STATE_V1_STALE_DEVICE == 8u,
    "stale-device font state changed");
_Static_assert(ANOMALY_TEXTURE_FORMAT_V1_AUTO == 0, "automatic texture format changed");
_Static_assert(ANOMALY_TEXTURE_FORMAT_V1_RGBA8 == 1, "RGBA8 texture format changed");
_Static_assert(ANOMALY_TEXTURE_STATE_V1_QUEUED == 1u, "queued texture state changed");
_Static_assert(ANOMALY_TEXTURE_STATE_V1_READY == 2u, "ready texture state changed");
_Static_assert(ANOMALY_TEXTURE_STATE_V1_FAILED == 4u, "failed texture state changed");
_Static_assert(ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE == 8u,
    "stale-device texture state changed");
_Static_assert(ANOMALY_INPUT_MODIFIER_V1_SHIFT == 1u, "input shift modifier changed");
_Static_assert(ANOMALY_INPUT_MODIFIER_V1_CONTROL == 2u,
    "input control modifier changed");
_Static_assert(ANOMALY_INPUT_MODIFIER_V1_ALT == 4u, "input alt modifier changed");
_Static_assert(ANOMALY_INPUT_MODIFIER_V1_SUPER == 8u, "input super modifier changed");
_Static_assert(ANOMALY_INPUT_CAPTURE_V1_MOUSE == 1u, "input mouse capture changed");
_Static_assert(ANOMALY_INPUT_CAPTURE_V1_KEYBOARD == 2u,
    "input keyboard capture changed");
_Static_assert(ANOMALY_INPUT_CAPTURE_V1_TEXT == 4u, "input text capture changed");
_Static_assert(ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS == 1u,
    "hotkey extra-modifiers flag changed");
_Static_assert(ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED == 2u,
    "hotkey UI-captured flag changed");
_Static_assert(ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED == 4u,
    "hotkey UI-captured-only flag changed");

ANOMALY_ASSERT_LAYOUT(AnomalyStringViewV1, 16, 8);
ANOMALY_ASSERT_OFFSET(AnomalyStringViewV1, data, 0);
ANOMALY_ASSERT_OFFSET(AnomalyStringViewV1, size, 8);
ANOMALY_ASSERT_TAIL(AnomalyStringViewV1, size);

ANOMALY_ASSERT_LAYOUT(AnomalyByteSpanV1, 16, 8);
ANOMALY_ASSERT_OFFSET(AnomalyByteSpanV1, data, 0);
ANOMALY_ASSERT_OFFSET(AnomalyByteSpanV1, size, 8);
ANOMALY_ASSERT_TAIL(AnomalyByteSpanV1, size);

ANOMALY_ASSERT_LAYOUT(AnomalyMutableByteSpanV1, 16, 8);
ANOMALY_ASSERT_OFFSET(AnomalyMutableByteSpanV1, data, 0);
ANOMALY_ASSERT_OFFSET(AnomalyMutableByteSpanV1, size, 8);
ANOMALY_ASSERT_TAIL(AnomalyMutableByteSpanV1, size);

ANOMALY_ASSERT_LAYOUT(AnomalyStatusV1, 24, 8);
ANOMALY_ASSERT_OFFSET(AnomalyStatusV1, code, 0);
ANOMALY_ASSERT_OFFSET(AnomalyStatusV1, reserved, 4);
ANOMALY_ASSERT_OFFSET(AnomalyStatusV1, message, 8);
ANOMALY_ASSERT_TAIL(AnomalyStatusV1, message);

ANOMALY_ASSERT_LAYOUT(AnomalyAllocatorV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyAllocatorV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyAllocatorV1, reserved, 4);
ANOMALY_ASSERT_OFFSET(AnomalyAllocatorV1, user, 8);
ANOMALY_ASSERT_OFFSET(AnomalyAllocatorV1, allocate, 16);
ANOMALY_ASSERT_OFFSET(AnomalyAllocatorV1, reallocate, 24);
ANOMALY_ASSERT_OFFSET(AnomalyAllocatorV1, release, 32);
ANOMALY_ASSERT_TAIL(AnomalyAllocatorV1, release);

ANOMALY_ASSERT_LAYOUT(AnomalyGenerationHandleV1, 16, 8);
ANOMALY_ASSERT_OFFSET(AnomalyGenerationHandleV1, id, 0);
ANOMALY_ASSERT_OFFSET(AnomalyGenerationHandleV1, generation, 8);
ANOMALY_ASSERT_TAIL(AnomalyGenerationHandleV1, generation);

ANOMALY_ASSERT_LAYOUT(AnomalyHostApiV1, 64, 8);
ANOMALY_ASSERT_OFFSET(AnomalyHostApiV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyHostApiV1, api_major, 4);
ANOMALY_ASSERT_OFFSET(AnomalyHostApiV1, api_minor, 6);
ANOMALY_ASSERT_OFFSET(AnomalyHostApiV1, host_context, 8);
ANOMALY_ASSERT_OFFSET(AnomalyHostApiV1, allocator, 16);
ANOMALY_ASSERT_OFFSET(AnomalyHostApiV1, query_service, 56);
ANOMALY_ASSERT_TAIL(AnomalyHostApiV1, query_service);

ANOMALY_ASSERT_LAYOUT(AnomalyPluginDescriptorV1, 120, 8);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, api_major, 4);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, api_minor, 6);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, id, 8);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, name, 24);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, author, 40);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, version, 56);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, on_load, 72);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, on_start, 80);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, on_stop, 88);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, on_unload, 96);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, on_update, 104);
ANOMALY_ASSERT_OFFSET(AnomalyPluginDescriptorV1, on_draw, 112);
ANOMALY_ASSERT_TAIL(AnomalyPluginDescriptorV1, on_draw);

ANOMALY_ASSERT_LAYOUT(AnomalyCoreServiceV1, 48, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyCoreServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyCoreServiceV1, log, 16);
ANOMALY_ASSERT_OFFSET(AnomalyCoreServiceV1, read_memory, 24);
ANOMALY_ASSERT_OFFSET(AnomalyCoreServiceV1, write_memory, 32);
ANOMALY_ASSERT_OFFSET(AnomalyCoreServiceV1, plugin_directory, 40);
ANOMALY_ASSERT_TAIL(AnomalyCoreServiceV1, plugin_directory);

ANOMALY_ASSERT_LAYOUT(AnomalyPluginStateServiceV1, 24, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyPluginStateServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyPluginStateServiceV1, directory, 16);
ANOMALY_ASSERT_TAIL(AnomalyPluginStateServiceV1, directory);

ANOMALY_ASSERT_LAYOUT(AnomalyRuntimeInfoV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, runtime_version_major, 4);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, runtime_version_minor, 8);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, runtime_version_patch, 12);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, process_id, 16);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, thread_id, 20);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, uptime_milliseconds, 24);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoV1, plugin_generation, 32);
ANOMALY_ASSERT_TAIL(AnomalyRuntimeInfoV1, plugin_generation);

ANOMALY_ASSERT_LAYOUT(AnomalyConfigServiceV1, 56, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyConfigServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyConfigServiceV1, register_schema, 16);
ANOMALY_ASSERT_OFFSET(AnomalyConfigServiceV1, unregister_schema, 24);
ANOMALY_ASSERT_OFFSET(AnomalyConfigServiceV1, read, 32);
ANOMALY_ASSERT_OFFSET(AnomalyConfigServiceV1, write_atomic, 40);
ANOMALY_ASSERT_OFFSET(AnomalyConfigServiceV1, migrate, 48);
ANOMALY_ASSERT_TAIL(AnomalyConfigServiceV1, migrate);

ANOMALY_ASSERT_LAYOUT(AnomalyStorageServiceV1, 40, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyStorageServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyStorageServiceV1, read, 16);
ANOMALY_ASSERT_OFFSET(AnomalyStorageServiceV1, write_atomic, 24);
ANOMALY_ASSERT_OFFSET(AnomalyStorageServiceV1, remove, 32);
ANOMALY_ASSERT_TAIL(AnomalyStorageServiceV1, remove);

ANOMALY_ASSERT_LAYOUT(AnomalyRuntimeInfoServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyRuntimeInfoServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoServiceV1, snapshot, 16);
ANOMALY_ASSERT_OFFSET(AnomalyRuntimeInfoServiceV1, runtime_version_utf8, 24);
ANOMALY_ASSERT_TAIL(AnomalyRuntimeInfoServiceV1, runtime_version_utf8);

ANOMALY_ASSERT_LAYOUT(AnomalyDiagnosticsServiceV1, 48, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyDiagnosticsServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyDiagnosticsServiceV1, register_self_test, 16);
ANOMALY_ASSERT_OFFSET(AnomalyDiagnosticsServiceV1, unregister_self_test, 24);
ANOMALY_ASSERT_OFFSET(AnomalyDiagnosticsServiceV1, run_self_test, 32);
ANOMALY_ASSERT_OFFSET(AnomalyDiagnosticsServiceV1, snapshot_json, 40);
ANOMALY_ASSERT_TAIL(AnomalyDiagnosticsServiceV1, snapshot_json);

ANOMALY_ASSERT_LAYOUT(AnomalySchedulerServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalySchedulerServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalySchedulerServiceV1, schedule, 16);
ANOMALY_ASSERT_OFFSET(AnomalySchedulerServiceV1, cancel, 24);
ANOMALY_ASSERT_TAIL(AnomalySchedulerServiceV1, cancel);

ANOMALY_ASSERT_LAYOUT(AnomalyCommandsServiceV1, 40, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyCommandsServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyCommandsServiceV1, register_command, 16);
ANOMALY_ASSERT_OFFSET(AnomalyCommandsServiceV1, unregister_command, 24);
ANOMALY_ASSERT_OFFSET(AnomalyCommandsServiceV1, invoke, 32);
ANOMALY_ASSERT_TAIL(AnomalyCommandsServiceV1, invoke);

ANOMALY_ASSERT_LAYOUT(AnomalyNotificationsServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNotificationsServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNotificationsServiceV1, post, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNotificationsServiceV1, dismiss, 24);
ANOMALY_ASSERT_TAIL(AnomalyNotificationsServiceV1, dismiss);

ANOMALY_ASSERT_LAYOUT(AnomalyHookRequestV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyHookRequestV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyHookRequestV1, kind, 4);
ANOMALY_ASSERT_OFFSET(AnomalyHookRequestV1, target, 8);
ANOMALY_ASSERT_OFFSET(AnomalyHookRequestV1, detour, 16);
ANOMALY_ASSERT_OFFSET(AnomalyHookRequestV1, label, 24);
ANOMALY_ASSERT_TAIL(AnomalyHookRequestV1, label);

ANOMALY_ASSERT_LAYOUT(AnomalySignatureServiceV1, 24, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalySignatureServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalySignatureServiceV1, resolve, 16);
ANOMALY_ASSERT_TAIL(AnomalySignatureServiceV1, resolve);

ANOMALY_ASSERT_LAYOUT(AnomalyHookServiceV1, 48, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyHookServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyHookServiceV1, create, 16);
ANOMALY_ASSERT_OFFSET(AnomalyHookServiceV1, release, 24);
ANOMALY_ASSERT_OFFSET(AnomalyHookServiceV1, begin_callback, 32);
ANOMALY_ASSERT_OFFSET(AnomalyHookServiceV1, end_callback, 40);
ANOMALY_ASSERT_TAIL(AnomalyHookServiceV1, end_callback);

ANOMALY_ASSERT_LAYOUT(AnomalyPatchServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyPatchServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyPatchServiceV1, apply, 16);
ANOMALY_ASSERT_OFFSET(AnomalyPatchServiceV1, release, 24);
ANOMALY_ASSERT_TAIL(AnomalyPatchServiceV1, release);

ANOMALY_ASSERT_LAYOUT(AnomalyEspCameraV1, 64, 8);
ANOMALY_ASSERT_OFFSET(AnomalyEspCameraV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyEspCameraV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyEspCameraV1, position, 8);
ANOMALY_ASSERT_OFFSET(AnomalyEspCameraV1, rotation, 32);
ANOMALY_ASSERT_OFFSET(AnomalyEspCameraV1, horizontal_fov_degrees, 56);
ANOMALY_ASSERT_OFFSET(AnomalyEspCameraV1, reserved, 60);
ANOMALY_ASSERT_TAIL(AnomalyEspCameraV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyEspEntityBoundsV1, 56, 8);
ANOMALY_ASSERT_OFFSET(AnomalyEspEntityBoundsV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyEspEntityBoundsV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyEspEntityBoundsV1, center, 8);
ANOMALY_ASSERT_OFFSET(AnomalyEspEntityBoundsV1, extent, 32);
ANOMALY_ASSERT_TAIL(AnomalyEspEntityBoundsV1, extent);

ANOMALY_ASSERT_LAYOUT(AnomalyEspBoxStyleV1, 24, 4);
ANOMALY_ASSERT_OFFSET(AnomalyEspBoxStyleV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyEspBoxStyleV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyEspBoxStyleV1, color_rgba, 8);
ANOMALY_ASSERT_OFFSET(AnomalyEspBoxStyleV1, outline_color_rgba, 12);
ANOMALY_ASSERT_OFFSET(AnomalyEspBoxStyleV1, thickness, 16);
ANOMALY_ASSERT_OFFSET(AnomalyEspBoxStyleV1, outline_thickness, 20);
ANOMALY_ASSERT_TAIL(AnomalyEspBoxStyleV1, outline_thickness);

ANOMALY_ASSERT_LAYOUT(AnomalyUiServiceV1, 280, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyUiServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, set_next_window_size, 16);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, begin_window, 24);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, end_window, 32);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, text, 40);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, button, 48);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, draw_entity_bbox, 56);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, checkbox, 64);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, slider_float, 72);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, color_edit4, 80);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, draw_entity_box3d, 88);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, draw_entity_label, 96);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, separator, 104);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, begin_child, 112);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, end_child, 120);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, begin_table, 128);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, table_next_row, 136);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, table_next_column, 144);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, end_table, 152);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, begin_menu, 160);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, end_menu, 168);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, open_popup, 176);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, begin_popup_modal, 184);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, end_popup, 192);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, close_current_popup, 200);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, filter_match, 208);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, frame_state, 216);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, set_next_window_size_constraints, 224);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, get_window_size, 232);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, input_uint32, 240);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, input_double, 248);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, developer_mode_enabled, 256);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, input_text, 264);
ANOMALY_ASSERT_OFFSET(AnomalyUiServiceV1, button_enabled, 272);
ANOMALY_ASSERT_TAIL(AnomalyUiServiceV1, button_enabled);

ANOMALY_ASSERT_LAYOUT(AnomalyWindowSpecV1, 72, 8);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, id, 8);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, title, 24);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, initial_width, 40);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, initial_height, 44);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, minimum_width, 48);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, minimum_height, 52);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, maximum_width, 56);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, maximum_height, 60);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, default_open, 64);
ANOMALY_ASSERT_OFFSET(AnomalyWindowSpecV1, reserved, 68);
ANOMALY_ASSERT_TAIL(AnomalyWindowSpecV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyWindowStateV1, 32, 8);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, width, 8);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, height, 12);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, ui_generation, 16);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, open, 24);
ANOMALY_ASSERT_OFFSET(AnomalyWindowStateV1, reserved, 28);
ANOMALY_ASSERT_TAIL(AnomalyWindowStateV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyFontRequestV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyFontRequestV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyFontRequestV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyFontRequestV1, relative_path, 8);
ANOMALY_ASSERT_OFFSET(AnomalyFontRequestV1, size_pixels, 24);
ANOMALY_ASSERT_OFFSET(AnomalyFontRequestV1, glyph_range, 28);
ANOMALY_ASSERT_OFFSET(AnomalyFontRequestV1, reserved, 32);

ANOMALY_ASSERT_LAYOUT(AnomalyFontStateV1, 32, 8);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, effective_size_pixels, 8);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, scale, 12);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, device_generation, 16);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, ready, 24);
ANOMALY_ASSERT_OFFSET(AnomalyFontStateV1, reserved, 28);
ANOMALY_ASSERT_TAIL(AnomalyFontStateV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyTextureRequestV1, 56, 8);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, relative_path, 8);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, encoded_bytes, 24);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, format, 40);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, width, 44);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, height, 48);
ANOMALY_ASSERT_OFFSET(AnomalyTextureRequestV1, reserved, 52);
ANOMALY_ASSERT_TAIL(AnomalyTextureRequestV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyTextureStateV1, 32, 8);
ANOMALY_ASSERT_OFFSET(AnomalyTextureStateV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyTextureStateV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyTextureStateV1, width, 8);
ANOMALY_ASSERT_OFFSET(AnomalyTextureStateV1, height, 12);
ANOMALY_ASSERT_OFFSET(AnomalyTextureStateV1, device_generation, 16);
ANOMALY_ASSERT_OFFSET(AnomalyTextureStateV1, byte_size, 24);
ANOMALY_ASSERT_TAIL(AnomalyTextureStateV1, byte_size);

ANOMALY_ASSERT_LAYOUT(AnomalyInputSnapshotV1, 80, 8);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, modifiers, 4);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, sequence, 8);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, timestamp_milliseconds, 16);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, mouse_x, 24);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, mouse_y, 28);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, mouse_wheel, 32);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, capture_flags, 36);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, keys, 40);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, mouse_buttons, 72);
ANOMALY_ASSERT_OFFSET(AnomalyInputSnapshotV1, reserved, 73);
ANOMALY_ASSERT_TAIL(AnomalyInputSnapshotV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyHotkeySpecV1, 32, 8);
ANOMALY_ASSERT_OFFSET(AnomalyHotkeySpecV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyHotkeySpecV1, modifiers, 4);
ANOMALY_ASSERT_OFFSET(AnomalyHotkeySpecV1, virtual_key, 8);
ANOMALY_ASSERT_OFFSET(AnomalyHotkeySpecV1, flags, 12);
ANOMALY_ASSERT_OFFSET(AnomalyHotkeySpecV1, id, 16);
ANOMALY_ASSERT_TAIL(AnomalyHotkeySpecV1, id);

ANOMALY_ASSERT_LAYOUT(AnomalyWindowServiceV1, 72, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyWindowServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, register_window, 16);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, release_window, 24);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, set_open, 32);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, toggle, 40);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, state, 48);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, begin, 56);
ANOMALY_ASSERT_OFFSET(AnomalyWindowServiceV1, end, 64);
ANOMALY_ASSERT_TAIL(AnomalyWindowServiceV1, end);

ANOMALY_ASSERT_LAYOUT(AnomalyFontServiceV1, 56, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyFontServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyFontServiceV1, request, 16);
ANOMALY_ASSERT_OFFSET(AnomalyFontServiceV1, release, 24);
ANOMALY_ASSERT_OFFSET(AnomalyFontServiceV1, state, 32);
ANOMALY_ASSERT_OFFSET(AnomalyFontServiceV1, push, 40);
ANOMALY_ASSERT_OFFSET(AnomalyFontServiceV1, pop, 48);
ANOMALY_ASSERT_TAIL(AnomalyFontServiceV1, pop);

ANOMALY_ASSERT_LAYOUT(AnomalyTextureServiceV1, 48, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyTextureServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyTextureServiceV1, request, 16);
ANOMALY_ASSERT_OFFSET(AnomalyTextureServiceV1, release, 24);
ANOMALY_ASSERT_OFFSET(AnomalyTextureServiceV1, state, 32);
ANOMALY_ASSERT_OFFSET(AnomalyTextureServiceV1, draw, 40);
ANOMALY_ASSERT_TAIL(AnomalyTextureServiceV1, draw);

ANOMALY_ASSERT_LAYOUT(AnomalyInputServiceV1, 56, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyInputServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyInputServiceV1, snapshot, 16);
ANOMALY_ASSERT_OFFSET(AnomalyInputServiceV1, was_pressed, 24);
ANOMALY_ASSERT_OFFSET(AnomalyInputServiceV1, register_hotkey, 32);
ANOMALY_ASSERT_OFFSET(AnomalyInputServiceV1, release_hotkey, 40);
ANOMALY_ASSERT_OFFSET(AnomalyInputServiceV1, capture_state, 48);
ANOMALY_ASSERT_TAIL(AnomalyInputServiceV1, capture_state);

ANOMALY_ASSERT_LAYOUT(AnomalyLocalizationServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyLocalizationServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyLocalizationServiceV1, locale, 16);
ANOMALY_ASSERT_OFFSET(AnomalyLocalizationServiceV1, translate, 24);
ANOMALY_ASSERT_TAIL(AnomalyLocalizationServiceV1, translate);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5BuildServiceV1, 40, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyUe5BuildServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyUe5BuildServiceV1, build_id, 16);
ANOMALY_ASSERT_OFFSET(AnomalyUe5BuildServiceV1, profile_hash, 24);
ANOMALY_ASSERT_OFFSET(AnomalyUe5BuildServiceV1, feature_state, 32);
ANOMALY_ASSERT_TAIL(AnomalyUe5BuildServiceV1, feature_state);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5FrameworkServiceV1, 40, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyUe5FrameworkServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyUe5FrameworkServiceV1, game_thread_id, 16);
ANOMALY_ASSERT_OFFSET(AnomalyUe5FrameworkServiceV1, tick_sequence, 24);
ANOMALY_ASSERT_OFFSET(AnomalyUe5FrameworkServiceV1, is_game_thread, 32);
ANOMALY_ASSERT_TAIL(AnomalyUe5FrameworkServiceV1, is_game_thread);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5NamesServiceV1, 24, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyUe5NamesServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyUe5NamesServiceV1, resolve_utf8, 16);
ANOMALY_ASSERT_TAIL(AnomalyUe5NamesServiceV1, resolve_utf8);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5ObjectSnapshotV1, 32, 8);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectSnapshotV1, reserved, 4);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectSnapshotV1, handle, 8);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectSnapshotV1, name_id, 24);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectSnapshotV1, flags, 28);
ANOMALY_ASSERT_TAIL(AnomalyUe5ObjectSnapshotV1, flags);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5ObjectsServiceV1, 48, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyUe5ObjectsServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectsServiceV1, generation, 16);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectsServiceV1, count, 24);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectsServiceV1, snapshot_at, 32);
ANOMALY_ASSERT_OFFSET(AnomalyUe5ObjectsServiceV1, snapshot_by_handle, 40);
ANOMALY_ASSERT_TAIL(AnomalyUe5ObjectsServiceV1, snapshot_by_handle);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5WorldSnapshotV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldSnapshotV1, reserved, 4);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldSnapshotV1, handle, 8);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldSnapshotV1, change_sequence, 24);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldSnapshotV1, name_id, 32);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldSnapshotV1, flags, 36);
ANOMALY_ASSERT_TAIL(AnomalyUe5WorldSnapshotV1, flags);

ANOMALY_ASSERT_LAYOUT(AnomalyUe5WorldServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyUe5WorldServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldServiceV1, current, 16);
ANOMALY_ASSERT_OFFSET(AnomalyUe5WorldServiceV1, snapshot, 24);
ANOMALY_ASSERT_TAIL(AnomalyUe5WorldServiceV1, snapshot);

ANOMALY_ASSERT_LAYOUT(AnomalyNteBuildServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNteBuildServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNteBuildServiceV1, build_id, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteBuildServiceV1, feature_state, 24);
ANOMALY_ASSERT_TAIL(AnomalyNteBuildServiceV1, feature_state);

ANOMALY_ASSERT_LAYOUT(AnomalyNteSessionSnapshotV1, 32, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionSnapshotV1, state, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionSnapshotV1, sequence, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionSnapshotV1, world, 16);
ANOMALY_ASSERT_TAIL(AnomalyNteSessionSnapshotV1, world);

ANOMALY_ASSERT_LAYOUT(AnomalyNteSessionEventV1, 56, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionEventV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionEventV1, kind, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionEventV1, sequence, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionEventV1, tick_sequence, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionEventV1, previous_world, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionEventV1, world, 40);
ANOMALY_ASSERT_TAIL(AnomalyNteSessionEventV1, world);

ANOMALY_ASSERT_LAYOUT(AnomalyNteSessionServiceV1, 40, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNteSessionServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionServiceV1, snapshot, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionServiceV1, next_event, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteSessionServiceV1, latest_event_sequence, 32);
ANOMALY_ASSERT_TAIL(AnomalyNteSessionServiceV1, latest_event_sequence);

ANOMALY_ASSERT_LAYOUT(AnomalyNtePlayerSnapshotV1, 56, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerSnapshotV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerSnapshotV1, handle, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerSnapshotV1, sequence, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerSnapshotV1, position, 32);
ANOMALY_ASSERT_TAIL(AnomalyNtePlayerSnapshotV1, position);

ANOMALY_ASSERT_LAYOUT(AnomalyNtePlayerEspSnapshotV1, 136, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, handle, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, sequence, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, bounds_center, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, bounds_extent, 56);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, camera_position, 80);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, camera_rotation, 104);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, horizontal_fov_degrees, 128);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerEspSnapshotV1, reserved, 132);
ANOMALY_ASSERT_TAIL(AnomalyNtePlayerEspSnapshotV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyNteCameraSnapshotV1, 104, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, world, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, player, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, sequence, 40);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, position, 48);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, rotation, 72);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, horizontal_fov_degrees, 96);
ANOMALY_ASSERT_OFFSET(AnomalyNteCameraSnapshotV1, reserved, 100);
ANOMALY_ASSERT_TAIL(AnomalyNteCameraSnapshotV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyNtePlayerServiceV1, 40, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNtePlayerServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerServiceV1, snapshot, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerServiceV1, esp_snapshot, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerServiceV1, camera_snapshot, 32);
ANOMALY_ASSERT_TAIL(AnomalyNtePlayerServiceV1, camera_snapshot);

ANOMALY_ASSERT_LAYOUT(AnomalyNtePlayerTeleportRequestV1, 64, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerTeleportRequestV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerTeleportRequestV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerTeleportRequestV1, world, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerTeleportRequestV1, player, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerTeleportRequestV1, position, 40);
ANOMALY_ASSERT_TAIL(AnomalyNtePlayerTeleportRequestV1, position);

ANOMALY_ASSERT_LAYOUT(AnomalyNtePlayerTeleportServiceV1, 24, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNtePlayerTeleportServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNtePlayerTeleportServiceV1, teleport, 16);
ANOMALY_ASSERT_TAIL(AnomalyNtePlayerTeleportServiceV1, teleport);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntityFrameV1, 88, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, generation, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, sequence, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, entity_count, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, reserved, 28);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, camera_position, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, camera_rotation, 56);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, horizontal_fov_degrees, 80);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityFrameV1, reserved2, 84);
ANOMALY_ASSERT_TAIL(AnomalyNteEntityFrameV1, reserved2);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntitySnapshotV1, 96, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, handle, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, entity_id, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, class_id, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, entity_name_id, 40);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, class_name_id, 44);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, bounds_center, 48);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitySnapshotV1, bounds_extent, 72);
ANOMALY_ASSERT_TAIL(AnomalyNteEntitySnapshotV1, bounds_extent);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntityPageRequestV1, 48, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, generation, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, offset, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, capacity, 20);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, class_id, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, class_name_id, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, entity_name_id, 36);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, required_flags, 40);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageRequestV1, excluded_flags, 44);
ANOMALY_ASSERT_TAIL(AnomalyNteEntityPageRequestV1, excluded_flags);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntityPageResultV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, generation, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, sequence, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, total_matches, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, returned, 28);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, next_offset, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityPageResultV1, reserved, 36);
ANOMALY_ASSERT_TAIL(AnomalyNteEntityPageResultV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntityComponentBoundsV1, 80, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityComponentBoundsV1, entity, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityComponentBoundsV1, sequence, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityComponentBoundsV1, bounds_center, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityComponentBoundsV1, bounds_extent, 56);
ANOMALY_ASSERT_TAIL(AnomalyNteEntityComponentBoundsV1, bounds_extent);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntityBoolPropertyV1, 40, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityBoolPropertyV1, entity, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityBoolPropertyV1, sequence, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityBoolPropertyV1, value, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntityBoolPropertyV1, reserved, 36);
ANOMALY_ASSERT_TAIL(AnomalyNteEntityBoolPropertyV1, reserved);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEntitiesServiceV1, 80, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNteEntitiesServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, frame, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, snapshot_at, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, class_name_utf8, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, entity_name_utf8, 40);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, page, 48);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, component_bounds, 56);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, bool_property, 64);
ANOMALY_ASSERT_OFFSET(AnomalyNteEntitiesServiceV1, fname_property_utf8, 72);
ANOMALY_ASSERT_TAIL(AnomalyNteEntitiesServiceV1, fname_property_utf8);

ANOMALY_ASSERT_LAYOUT(AnomalyNteActorsServiceV1, 80, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNteActorsServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNteActorsServiceV1, frame, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteActorsServiceV1, page, 48);
ANOMALY_ASSERT_OFFSET(AnomalyNteActorsServiceV1, component_bounds, 56);
ANOMALY_ASSERT_OFFSET(AnomalyNteActorsServiceV1, bool_property, 64);
ANOMALY_ASSERT_OFFSET(AnomalyNteActorsServiceV1, fname_property_utf8, 72);
ANOMALY_ASSERT_TAIL(AnomalyNteActorsServiceV1, fname_property_utf8);

ANOMALY_ASSERT_LAYOUT(AnomalyNteSnapshotMetricsV1, 104, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, tick_sequence, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, session_event_sequence, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, snapshot_tick_count, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, latest_snapshot_cost_micros, 32);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, total_snapshot_cost_micros, 40);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, max_snapshot_cost_micros, 48);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, player_refresh_count, 56);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, player_cache_hit_count, 64);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, entity_refresh_count, 72);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, entity_cache_hit_count, 80);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, entity_page_request_count, 88);
ANOMALY_ASSERT_OFFSET(AnomalyNteSnapshotMetricsV1, entity_page_cache_hit_count, 96);
ANOMALY_ASSERT_TAIL(AnomalyNteSnapshotMetricsV1, entity_page_cache_hit_count);

ANOMALY_ASSERT_LAYOUT(AnomalyNteMetricsServiceV1, 24, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNteMetricsServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNteMetricsServiceV1, snapshot, 16);
ANOMALY_ASSERT_TAIL(AnomalyNteMetricsServiceV1, snapshot);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEscMenuButtonSpecV1, 64, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, struct_size, 0);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, flags, 4);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, id, 8);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, label, 24);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, icon_format, 40);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, reserved, 44);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonSpecV1, icon_bytes, 48);
ANOMALY_ASSERT_TAIL(AnomalyNteEscMenuButtonSpecV1, icon_bytes);

ANOMALY_ASSERT_LAYOUT(AnomalyNteEscMenuButtonServiceV1, 32, 8);
ANOMALY_ASSERT_SERVICE_PREFIX(AnomalyNteEscMenuButtonServiceV1);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonServiceV1, register_button, 16);
ANOMALY_ASSERT_OFFSET(AnomalyNteEscMenuButtonServiceV1, unregister_button, 24);
ANOMALY_ASSERT_TAIL(AnomalyNteEscMenuButtonServiceV1, unregister_button);

_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyAllocatorV1*)0)->allocate, ContractAllocateFn),
    "allocator.allocate signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyAllocatorV1*)0)->reallocate, ContractReallocateFn),
    "allocator.reallocate signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyAllocatorV1*)0)->release, ContractReleaseFn),
    "allocator.release signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyHostApiV1*)0)->query_service, ContractQueryServiceFn),
    "host.query_service signature");
_Static_assert(
    ANOMALY_TYPE_IS((AnomalyPluginEntryV1Fn)0, ContractPluginEntryFn),
    "plugin entry signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginDescriptorV1*)0)->on_load, ContractOnLoadFn),
    "plugin.on_load signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginDescriptorV1*)0)->on_start, ContractStatusContextFn),
    "plugin.on_start signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginDescriptorV1*)0)->on_stop, ContractOnStopFn),
    "plugin.on_stop signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginDescriptorV1*)0)->on_unload, ContractVoidContextFn),
    "plugin.on_unload signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginDescriptorV1*)0)->on_update, ContractUpdateFn),
    "plugin.on_update signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginDescriptorV1*)0)->on_draw, ContractDrawFn),
    "plugin.on_draw signature");

_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCoreServiceV1*)0)->log, ContractLogFn),
    "core.log signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCoreServiceV1*)0)->read_memory, ContractReadMemoryFn),
    "core.read_memory signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCoreServiceV1*)0)->write_memory, ContractWriteMemoryFn),
    "core.write_memory signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCoreServiceV1*)0)->plugin_directory, ContractStringOutputFn),
    "core.plugin_directory signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPluginStateServiceV1*)0)->directory, ContractStringOutputFn),
    "plugin-state.directory signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyConfigServiceV1*)0)->register_schema, ContractConfigRegisterFn),
    "config.register_schema signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyConfigServiceV1*)0)->unregister_schema, ContractHandleFn),
    "config.unregister_schema signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyConfigServiceV1*)0)->read, ContractConfigReadFn),
    "config.read signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyConfigServiceV1*)0)->write_atomic, ContractConfigWriteFn),
    "config.write_atomic signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyConfigServiceV1*)0)->migrate, ContractConfigMigrateFn),
    "config.migrate signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyStorageServiceV1*)0)->read, ContractStorageReadFn),
    "storage.read signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyStorageServiceV1*)0)->write_atomic, ContractStorageWriteFn),
    "storage.write_atomic signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyStorageServiceV1*)0)->remove, ContractStringOnlyFn),
    "storage.remove signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyRuntimeInfoServiceV1*)0)->snapshot, ContractRuntimeInfoSnapshotFn),
    "runtime-info.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyRuntimeInfoServiceV1*)0)->runtime_version_utf8, ContractStringOutputFn),
    "runtime-info.runtime_version_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyDiagnosticsServiceV1*)0)->register_self_test,
                    ContractRegisterSelfTestFn),
    "diagnostics.register_self_test signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyDiagnosticsServiceV1*)0)->unregister_self_test, ContractHandleFn),
    "diagnostics.unregister_self_test signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyDiagnosticsServiceV1*)0)->run_self_test, ContractRunSelfTestFn),
    "diagnostics.run_self_test signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyDiagnosticsServiceV1*)0)->snapshot_json, ContractSnapshotJsonFn),
    "diagnostics.snapshot_json signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalySchedulerServiceV1*)0)->schedule, ContractScheduleFn),
    "scheduler.schedule signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalySchedulerServiceV1*)0)->cancel, ContractHandleFn),
    "scheduler.cancel signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCommandsServiceV1*)0)->register_command,
                    ContractRegisterCommandFn),
    "commands.register_command signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCommandsServiceV1*)0)->unregister_command, ContractHandleFn),
    "commands.unregister_command signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyCommandsServiceV1*)0)->invoke, ContractInvokeCommandFn),
    "commands.invoke signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNotificationsServiceV1*)0)->post, ContractPostNotificationFn),
    "notifications.post signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNotificationsServiceV1*)0)->dismiss, ContractHandleFn),
    "notifications.dismiss signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalySignatureServiceV1*)0)->resolve, ContractResolveSignatureFn),
    "signature.resolve signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyHookServiceV1*)0)->create, ContractCreateHookFn),
    "hook.create signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyHookServiceV1*)0)->release, ContractHandleFn),
    "hook.release signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyHookServiceV1*)0)->begin_callback, ContractBeginHookFn),
    "hook.begin_callback signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyHookServiceV1*)0)->end_callback, ContractHandleFn),
    "hook.end_callback signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPatchServiceV1*)0)->apply, ContractApplyPatchFn),
    "patch.apply signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyPatchServiceV1*)0)->release, ContractHandleFn),
    "patch.release signature");

_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->set_next_window_size, ContractSetNextWindowSizeFn),
    "ui.set_next_window_size signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->begin_window, ContractBeginWindowFn),
    "ui.begin_window signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->end_window, ContractEndWindowFn),
    "ui.end_window signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->text, ContractTextFn),
    "ui.text signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->button, ContractButtonFn),
    "ui.button signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->draw_entity_bbox, ContractDrawEntityBboxFn),
    "ui.draw_entity_bbox signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->checkbox, ContractCheckboxFn),
    "ui.checkbox signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->slider_float, ContractSliderFloatFn),
    "ui.slider_float signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->color_edit4, ContractColorEdit4Fn),
    "ui.color_edit4 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->draw_entity_box3d, ContractDrawEntityBboxFn),
    "ui.draw_entity_box3d signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->draw_entity_label, ContractDrawEntityLabelFn),
    "ui.draw_entity_label signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->set_next_window_size,
                    ContractSetNextWindowSizeFn),
    "ui set_next_window_size signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->begin_window, ContractBeginWindowFn),
    "ui begin_window signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->end_window, ContractEndWindowFn),
    "ui end_window signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->text, ContractTextFn),
    "ui text signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->button, ContractButtonFn),
    "ui button signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->draw_entity_bbox, ContractDrawEntityBboxFn),
    "ui draw_entity_bbox signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->checkbox, ContractCheckboxFn),
    "ui checkbox signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->slider_float, ContractSliderFloatFn),
    "ui slider_float signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->color_edit4, ContractColorEdit4Fn),
    "ui color_edit4 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->draw_entity_box3d,
                    ContractDrawEntityBboxFn),
    "ui draw_entity_box3d signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->draw_entity_label,
                    ContractDrawEntityLabelFn),
    "ui draw_entity_label signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->separator, ContractUiSeparatorFn),
    "ui separator signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->begin_child, ContractUiBeginChildFn),
    "ui begin_child signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->end_child, ContractUiSeparatorFn),
    "ui end_child signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->begin_table, ContractUiBeginTableFn),
    "ui begin_table signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->table_next_row, ContractUiTableNextRowFn),
    "ui table_next_row signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->table_next_column, ContractUiTableNextColumnFn),
    "ui table_next_column signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->end_table, ContractUiSeparatorFn),
    "ui end_table signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->begin_menu, ContractUiBeginMenuFn),
    "ui begin_menu signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->end_menu, ContractUiSeparatorFn),
    "ui end_menu signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->open_popup, ContractUiOpenPopupFn),
    "ui open_popup signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->begin_popup_modal,
                    ContractUiBeginPopupModalFn),
    "ui begin_popup_modal signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->end_popup, ContractUiSeparatorFn),
    "ui end_popup signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->close_current_popup, ContractUiSeparatorFn),
    "ui close_current_popup signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->filter_match, ContractUiFilterMatchFn),
    "ui filter_match signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->frame_state, ContractUiFrameStateFn),
    "ui frame_state signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->set_next_window_size_constraints,
                    ContractUiSetNextWindowSizeConstraintsFn),
    "ui set_next_window_size_constraints signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->get_window_size,
                    ContractUiGetWindowSizeFn),
    "ui get_window_size signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->input_uint32, ContractUiInputUInt32Fn),
    "ui input_uint32 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->input_double, ContractUiInputDoubleFn),
    "ui input_double signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->developer_mode_enabled,
                    ContractUiDeveloperModeEnabledFn),
    "ui developer_mode_enabled signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->input_text, ContractUiInputTextFn),
    "ui input_text signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUiServiceV1*)0)->button_enabled,
                    ContractUiButtonEnabledFn),
    "ui button_enabled signature");

_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->register_window,
                    ContractRegisterWindowFn),
    "window.register_window signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->release_window, ContractHandleFn),
    "window.release_window signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->set_open, ContractWindowOpenFn),
    "window.set_open signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->toggle, ContractHandleFn),
    "window.toggle signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->state, ContractWindowStateFn),
    "window.state signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->begin, ContractWindowBeginFn),
    "window.begin signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyWindowServiceV1*)0)->end, ContractHandleFn),
    "window.end signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyFontServiceV1*)0)->request, ContractFontRequestFn),
    "font.request signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyFontServiceV1*)0)->release, ContractHandleFn),
    "font.release signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyFontServiceV1*)0)->state, ContractFontStateFn),
    "font.state signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyFontServiceV1*)0)->push, ContractFontPushFn),
    "font.push signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyFontServiceV1*)0)->pop, ContractStatusContextFn),
    "font.pop signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyTextureServiceV1*)0)->request, ContractTextureRequestFn),
    "texture.request signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyTextureServiceV1*)0)->release, ContractHandleFn),
    "texture.release signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyTextureServiceV1*)0)->state, ContractTextureStateFn),
    "texture.state signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyTextureServiceV1*)0)->draw, ContractTextureDrawFn),
    "texture.draw signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyInputServiceV1*)0)->snapshot, ContractInputSnapshotFn),
    "input.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyInputServiceV1*)0)->was_pressed, ContractInputWasPressedFn),
    "input.was_pressed signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyInputServiceV1*)0)->register_hotkey,
                    ContractRegisterHotkeyFn),
    "input.register_hotkey signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyInputServiceV1*)0)->release_hotkey, ContractHandleFn),
    "input.release_hotkey signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyInputServiceV1*)0)->capture_state,
                    ContractInputCaptureStateFn),
    "input.capture_state signature");
_Static_assert(
    ANOMALY_TYPE_IS((AnomalyHotkeyCallbackV1)0, ContractHotkeyCallbackFn),
    "input hotkey callback signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyLocalizationServiceV1*)0)->locale, ContractStringOutputFn),
    "localization.locale signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyLocalizationServiceV1*)0)->translate,
                    ContractLocalizationTranslateFn),
    "localization.translate signature");

_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5BuildServiceV1*)0)->build_id, ContractStringOutputFn),
    "ue5.build.build_id signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5BuildServiceV1*)0)->profile_hash, ContractStringOutputFn),
    "ue5.build.profile_hash signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5BuildServiceV1*)0)->feature_state, ContractFeatureStateFn),
    "ue5.build.feature_state signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5FrameworkServiceV1*)0)->game_thread_id, ContractUint32ContextFn),
    "ue5.framework.game_thread_id signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5FrameworkServiceV1*)0)->tick_sequence, ContractUint64ContextFn),
    "ue5.framework.tick_sequence signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5FrameworkServiceV1*)0)->is_game_thread, ContractIntContextFn),
    "ue5.framework.is_game_thread signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5NamesServiceV1*)0)->resolve_utf8, ContractResolveNameFn),
    "ue5.names.resolve_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5ObjectsServiceV1*)0)->generation, ContractUint64ContextFn),
    "ue5.objects.generation signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5ObjectsServiceV1*)0)->count, ContractUint32ContextFn),
    "ue5.objects.count signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5ObjectsServiceV1*)0)->snapshot_at, ContractObjectSnapshotFn),
    "ue5.objects.snapshot_at signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5ObjectsServiceV1*)0)->snapshot_by_handle,
                    ContractObjectSnapshotByHandleFn),
    "ue5.objects.snapshot_by_handle signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5WorldServiceV1*)0)->current, ContractWorldCurrentFn),
    "ue5.world.current signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyUe5WorldServiceV1*)0)->snapshot, ContractWorldSnapshotFn),
    "ue5.world.snapshot signature");

_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteBuildServiceV1*)0)->build_id, ContractStringOutputFn),
    "nte.build.build_id signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteBuildServiceV1*)0)->feature_state, ContractFeatureStateFn),
    "nte.build.feature_state signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteSessionServiceV1*)0)->snapshot, ContractNteSessionSnapshotFn),
    "nte.session.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteSessionServiceV1*)0)->snapshot, ContractNteSessionSnapshotFn),
    "nte.session.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteSessionServiceV1*)0)->next_event, ContractNteSessionNextEventFn),
    "nte.session.next_event signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteSessionServiceV1*)0)->latest_event_sequence,
                    ContractUint64ContextFn),
    "nte.session.latest_event_sequence signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNtePlayerServiceV1*)0)->snapshot, ContractNtePlayerSnapshotFn),
    "nte.player.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNtePlayerServiceV1*)0)->esp_snapshot,
                    ContractNtePlayerEspSnapshotFn),
    "nte.player.esp_snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNtePlayerServiceV1*)0)->snapshot, ContractNtePlayerSnapshotFn),
    "nte.player.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNtePlayerServiceV1*)0)->esp_snapshot,
                    ContractNtePlayerEspSnapshotFn),
    "nte.player.esp_snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNtePlayerServiceV1*)0)->camera_snapshot,
                    ContractNteCameraSnapshotFn),
    "nte.player.camera_snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNtePlayerTeleportServiceV1*)0)->teleport,
                    ContractNtePlayerTeleportFn),
    "nte.player-teleport.teleport signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->frame, ContractNteEntityFrameFn),
    "nte.entities.frame signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->snapshot_at,
                    ContractNteEntitySnapshotFn),
    "nte.entities.snapshot_at signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->class_name_utf8,
                    ContractNteEntityNameFn),
    "nte.entities.class_name_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->entity_name_utf8,
                    ContractNteEntityNameFn),
    "nte.entities.entity_name_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->frame, ContractNteEntityFrameFn),
    "nte.entities.frame signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->snapshot_at,
                    ContractNteEntitySnapshotFn),
    "nte.entities.snapshot_at signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->class_name_utf8,
                    ContractNteEntityNameFn),
    "nte.entities.class_name_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->entity_name_utf8,
                    ContractNteEntityNameFn),
    "nte.entities.entity_name_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->page, ContractNteEntityPageFn),
    "nte.entities.page signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->component_bounds,
                    ContractNteEntityComponentBoundsFn),
    "nte.entities.component_bounds signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->bool_property,
                    ContractNteEntityBoolPropertyFn),
    "nte.entities.bool_property signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEntitiesServiceV1*)0)->fname_property_utf8,
                    ContractNteEntityFNamePropertyFn),
    "nte.entities.fname_property_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteActorsServiceV1*)0)->frame, ContractNteEntityFrameFn),
    "nte.actors.frame signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteActorsServiceV1*)0)->page, ContractNteEntityPageFn),
    "nte.actors.page signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteActorsServiceV1*)0)->component_bounds,
                    ContractNteEntityComponentBoundsFn),
    "nte.actors.component_bounds signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteActorsServiceV1*)0)->bool_property,
                    ContractNteEntityBoolPropertyFn),
    "nte.actors.bool_property signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteActorsServiceV1*)0)->fname_property_utf8,
                    ContractNteEntityFNamePropertyFn),
    "nte.actors.fname_property_utf8 signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteMetricsServiceV1*)0)->snapshot,
                    ContractNteMetricsSnapshotFn),
    "nte.metrics.snapshot signature");
_Static_assert(
    ANOMALY_TYPE_IS((AnomalyNteEscMenuButtonCallbackV1)0,
                    ContractNteEscMenuButtonCallbackFn),
    "nte.esc-menu-button callback signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEscMenuButtonServiceV1*)0)->register_button,
                    ContractNteEscMenuButtonRegisterFn),
    "nte.esc-menu-button register signature");
_Static_assert(
    ANOMALY_TYPE_IS(((AnomalyNteEscMenuButtonServiceV1*)0)->unregister_button,
                    ContractHandleFn),
    "nte.esc-menu-button unregister signature");

int main(void) {
    if (strcmp(ANOMALY_PLUGIN_V1_ENTRY_NAME, "AnomalyPluginEntryV1") != 0) return 1;
    if (strcmp(ANOMALY_CORE_SERVICE_V1_ID, "anomaly.core") != 0) return 2;
    if (strcmp(ANOMALY_UI_SERVICE_V1_ID, "anomaly.ui") != 0) return 3;
    if (strcmp(ANOMALY_WINDOW_SERVICE_V1_ID, "anomaly.window") != 0) return 4;
    if (strcmp(ANOMALY_FONT_SERVICE_V1_ID, "anomaly.font") != 0) return 5;
    if (strcmp(ANOMALY_TEXTURE_SERVICE_V1_ID, "anomaly.texture") != 0) return 6;
    if (strcmp(ANOMALY_INPUT_SERVICE_V1_ID, "anomaly.input") != 0) return 7;
    if (strcmp(ANOMALY_LOCALIZATION_SERVICE_V1_ID, "anomaly.localization") != 0) return 8;
    if (strcmp(ANOMALY_UE5_BUILD_SERVICE_V1_ID, "anomaly.ue5.build") != 0) return 9;
    if (strcmp(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, "anomaly.ue5.framework") != 0) return 10;
    if (strcmp(ANOMALY_UE5_NAMES_SERVICE_V1_ID, "anomaly.ue5.names") != 0) return 11;
    if (strcmp(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, "anomaly.ue5.objects") != 0) return 12;
    if (strcmp(ANOMALY_UE5_WORLD_SERVICE_V1_ID, "anomaly.ue5.world") != 0) return 13;
    if (strcmp(ANOMALY_NTE_BUILD_SERVICE_V1_ID, "anomaly.nte.build") != 0) return 14;
    if (strcmp(ANOMALY_NTE_SESSION_SERVICE_V1_ID, "anomaly.nte.session") != 0) return 15;
    if (strcmp(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, "anomaly.nte.player") != 0) return 16;
    if (strcmp(ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID, "anomaly.nte.player-teleport") != 0) return 17;
    if (strcmp(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, "anomaly.nte.entities") != 0) return 18;
    if (strcmp(ANOMALY_NTE_ACTORS_SERVICE_V1_ID, "anomaly.nte.actors") != 0) return 19;
    if (strcmp(ANOMALY_NTE_METRICS_SERVICE_V1_ID, "anomaly.nte.metrics") != 0) return 20;
    if (strcmp(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID, "anomaly.plugin-state") != 0) return 21;
    if (strcmp(ANOMALY_CONFIG_SERVICE_V1_ID, "anomaly.config") != 0) return 22;
    if (strcmp(ANOMALY_STORAGE_SERVICE_V1_ID, "anomaly.storage") != 0) return 23;
    if (strcmp(ANOMALY_RUNTIME_INFO_SERVICE_V1_ID, "anomaly.runtime-info") != 0) return 24;
    if (strcmp(ANOMALY_DIAGNOSTICS_SERVICE_V1_ID, "anomaly.diagnostics") != 0) return 25;
    if (strcmp(ANOMALY_SCHEDULER_SERVICE_V1_ID, "anomaly.scheduler") != 0) return 26;
    if (strcmp(ANOMALY_IPC_SERVICE_V1_ID, "anomaly.ipc") != 0) return 27;
    if (strcmp(ANOMALY_COMMANDS_SERVICE_V1_ID, "anomaly.commands") != 0) return 28;
    if (strcmp(ANOMALY_NOTIFICATIONS_SERVICE_V1_ID, "anomaly.notifications") != 0) return 29;
    if (strcmp(ANOMALY_SIGNATURE_SERVICE_V1_ID, "anomaly.interop.signature") != 0) return 30;
    if (strcmp(ANOMALY_HOOK_SERVICE_V1_ID, "anomaly.interop.hook") != 0) return 31;
    if (strcmp(ANOMALY_PATCH_SERVICE_V1_ID, "anomaly.interop.patch") != 0) return 32;
    if (strcmp(ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_ID,
            "anomaly.nte.esc-menu-button") != 0) return 33;
    return 0;
}
