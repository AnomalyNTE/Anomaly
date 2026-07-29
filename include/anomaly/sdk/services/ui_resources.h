#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_WINDOW_SERVICE_V1_ID "anomaly.window"
#define ANOMALY_WINDOW_SERVICE_V1_VERSION 1u
#define ANOMALY_FONT_SERVICE_V1_ID "anomaly.font"
#define ANOMALY_FONT_SERVICE_V1_VERSION 1u
#define ANOMALY_TEXTURE_SERVICE_V1_ID "anomaly.texture"
#define ANOMALY_TEXTURE_SERVICE_V1_VERSION 1u
#define ANOMALY_INPUT_SERVICE_V1_ID "anomaly.input"
#define ANOMALY_INPUT_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyWindowFlagsV1 {
    ANOMALY_WINDOW_V1_NONE = 0,
    ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS = 1u << 0u,
    ANOMALY_WINDOW_V1_NO_COLLAPSE = 1u << 1u
} AnomalyWindowFlagsV1;

typedef struct AnomalyWindowSpecV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyStringViewV1 id;
    AnomalyStringViewV1 title;
    float initial_width;
    float initial_height;
    float minimum_width;
    float minimum_height;
    float maximum_width;
    float maximum_height;
    int32_t default_open;
    uint32_t reserved;
} AnomalyWindowSpecV1;

typedef struct AnomalyWindowStateV1 {
    uint32_t struct_size;
    uint32_t flags;
    float width;
    float height;
    uint64_t ui_generation;
    int32_t open;
    uint32_t reserved;
} AnomalyWindowStateV1;

typedef enum AnomalyGlyphRangeV1 {
    ANOMALY_GLYPH_RANGE_V1_DEFAULT = 0,
    ANOMALY_GLYPH_RANGE_V1_LATIN = 1,
    ANOMALY_GLYPH_RANGE_V1_CYRILLIC = 2,
    ANOMALY_GLYPH_RANGE_V1_JAPANESE = 3,
    ANOMALY_GLYPH_RANGE_V1_CHINESE_FULL = 4
} AnomalyGlyphRangeV1;

typedef struct AnomalyFontRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyStringViewV1 relative_path;
    float size_pixels;
    uint32_t glyph_range;
    uint32_t reserved;
} AnomalyFontRequestV1;

typedef enum AnomalyFontStateFlagsV1 {
    ANOMALY_FONT_STATE_V1_NONE = 0,
    ANOMALY_FONT_STATE_V1_QUEUED = 1u << 0u,
    ANOMALY_FONT_STATE_V1_READY = 1u << 1u,
    ANOMALY_FONT_STATE_V1_FAILED = 1u << 2u,
    ANOMALY_FONT_STATE_V1_STALE_DEVICE = 1u << 3u
} AnomalyFontStateFlagsV1;

typedef struct AnomalyFontStateV1 {
    uint32_t struct_size;
    uint32_t flags;
    float effective_size_pixels;
    float scale;
    uint64_t device_generation;
    int32_t ready;
    uint32_t reserved;
} AnomalyFontStateV1;

typedef enum AnomalyTextureFormatV1 {
    ANOMALY_TEXTURE_FORMAT_V1_AUTO = 0,
    ANOMALY_TEXTURE_FORMAT_V1_RGBA8 = 1
} AnomalyTextureFormatV1;

typedef enum AnomalyTextureStateFlagsV1 {
    ANOMALY_TEXTURE_STATE_V1_NONE = 0,
    ANOMALY_TEXTURE_STATE_V1_QUEUED = 1u << 0u,
    ANOMALY_TEXTURE_STATE_V1_READY = 1u << 1u,
    ANOMALY_TEXTURE_STATE_V1_FAILED = 1u << 2u,
    ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE = 1u << 3u
} AnomalyTextureStateFlagsV1;

typedef struct AnomalyTextureRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyStringViewV1 relative_path;
    AnomalyByteSpanV1 encoded_bytes;
    uint32_t format;
    // Required for ANOMALY_TEXTURE_FORMAT_V1_RGBA8. Encoded formats discover
    // dimensions during decode and leave these fields zero.
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} AnomalyTextureRequestV1;

typedef struct AnomalyTextureStateV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint64_t device_generation;
    uint64_t byte_size;
} AnomalyTextureStateV1;

typedef enum AnomalyInputModifiersV1 {
    ANOMALY_INPUT_MODIFIER_V1_NONE = 0,
    ANOMALY_INPUT_MODIFIER_V1_SHIFT = 1u << 0u,
    ANOMALY_INPUT_MODIFIER_V1_CONTROL = 1u << 1u,
    ANOMALY_INPUT_MODIFIER_V1_ALT = 1u << 2u,
    ANOMALY_INPUT_MODIFIER_V1_SUPER = 1u << 3u
} AnomalyInputModifiersV1;

typedef enum AnomalyInputCaptureFlagsV1 {
    ANOMALY_INPUT_CAPTURE_V1_NONE = 0,
    ANOMALY_INPUT_CAPTURE_V1_MOUSE = 1u << 0u,
    ANOMALY_INPUT_CAPTURE_V1_KEYBOARD = 1u << 1u,
    ANOMALY_INPUT_CAPTURE_V1_TEXT = 1u << 2u
} AnomalyInputCaptureFlagsV1;

typedef struct AnomalyInputSnapshotV1 {
    uint32_t struct_size;
    uint32_t modifiers;
    uint64_t sequence;
    uint64_t timestamp_milliseconds;
    float mouse_x;
    float mouse_y;
    int32_t mouse_wheel;
    uint32_t capture_flags;
    uint8_t keys[32];
    uint8_t mouse_buttons;
    uint8_t reserved[7];
} AnomalyInputSnapshotV1;

typedef struct AnomalyHotkeySpecV1 {
    uint32_t struct_size;
    uint32_t modifiers;
    uint32_t virtual_key;
    uint32_t flags;
    AnomalyStringViewV1 id;
} AnomalyHotkeySpecV1;

typedef enum AnomalyHotkeyFlagsV1 {
    ANOMALY_HOTKEY_V1_NONE = 0,
    // By default modifiers match exactly. This permits additional modifiers.
    ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS = 1u << 0u,
    ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED = 1u << 1u,
    ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED = 1u << 2u
} AnomalyHotkeyFlagsV1;

typedef void (ANOMALY_CALL *AnomalyHotkeyCallbackV1)(
    void* user, AnomalyGenerationHandleV1 hotkey, const AnomalyInputSnapshotV1* snapshot);

typedef struct AnomalyWindowServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_window)(
        void* user, const AnomalyWindowSpecV1* spec, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release_window)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *set_open)(
        void* user, AnomalyGenerationHandleV1 handle, int32_t open);
    AnomalyStatusV1 (ANOMALY_CALL *toggle)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *state)(
        void* user, AnomalyGenerationHandleV1 handle, AnomalyWindowStateV1* state);
    AnomalyStatusV1 (ANOMALY_CALL *begin)(
        void* user, AnomalyGenerationHandleV1 handle, uint32_t flags, int32_t* visible);
    AnomalyStatusV1 (ANOMALY_CALL *end)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyWindowServiceV1;

typedef struct AnomalyFontServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *request)(
        void* user, const AnomalyFontRequestV1* request, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *state)(
        void* user, AnomalyGenerationHandleV1 handle, AnomalyFontStateV1* state);
    AnomalyStatusV1 (ANOMALY_CALL *push)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *pop)(void* user);
} AnomalyFontServiceV1;

typedef struct AnomalyTextureServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *request)(
        void* user, const AnomalyTextureRequestV1* request, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *state)(
        void* user, AnomalyGenerationHandleV1 handle, AnomalyTextureStateV1* state);
    AnomalyStatusV1 (ANOMALY_CALL *draw)(
        void* user, AnomalyGenerationHandleV1 handle, float width, float height,
        uint32_t tint_rgba);
} AnomalyTextureServiceV1;

typedef struct AnomalyInputServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyInputSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *was_pressed)(
        void* user, uint32_t virtual_key, int32_t* pressed);
    AnomalyStatusV1 (ANOMALY_CALL *register_hotkey)(
        void* user, const AnomalyHotkeySpecV1* spec, AnomalyHotkeyCallbackV1 callback,
        void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release_hotkey)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *capture_state)(void* user, uint32_t* capture_flags);
} AnomalyInputServiceV1;

#ifdef __cplusplus
}
#endif
