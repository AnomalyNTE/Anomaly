#pragma once
#include "anomaly/sdk/base.h"

#define ANOMALY_UI_SERVICE_V1_ID "anomaly.ui"
#define ANOMALY_UI_SERVICE_V1_VERSION 1u
#define ANOMALY_RGBA_V1(red, green, blue, alpha) \
    ((uint32_t)(red) | ((uint32_t)(green) << 8u) | \
     ((uint32_t)(blue) << 16u) | ((uint32_t)(alpha) << 24u))

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyEspBoxFlagsV1 {
    ANOMALY_ESP_BOX_V1_NONE = 0,
    ANOMALY_ESP_BOX_V1_OUTLINE = 1u << 0u
} AnomalyEspBoxFlagsV1;

typedef struct AnomalyEspCameraV1 {
    uint32_t struct_size;
    uint32_t flags;
    double position[3];
    double rotation[3];
    float horizontal_fov_degrees;
    uint32_t reserved;
} AnomalyEspCameraV1;

typedef struct AnomalyEspEntityBoundsV1 {
    uint32_t struct_size;
    uint32_t flags;
    double center[3];
    double extent[3];
} AnomalyEspEntityBoundsV1;

typedef struct AnomalyEspBoxStyleV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t color_rgba;
    uint32_t outline_color_rgba;
    float thickness;
    float outline_thickness;
} AnomalyEspBoxStyleV1;

typedef enum AnomalyUiFrameStateV1 {
    ANOMALY_UI_FRAME_V1_NONE = 0,
    ANOMALY_UI_FRAME_V1_ITEM_HOVERED = 1u << 0u,
    ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED = 1u << 1u,
    ANOMALY_UI_FRAME_V1_ITEM_ACTIVE = 1u << 2u,
    ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE = 1u << 3u,
    ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD = 1u << 4u,
    ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT = 1u << 5u
} AnomalyUiFrameStateV1;

typedef enum AnomalyUiTextInputFlagsV1 {
    ANOMALY_UI_TEXT_INPUT_V1_NONE = 0,
    ANOMALY_UI_TEXT_INPUT_V1_DIGITS = 1u << 0u
} AnomalyUiTextInputFlagsV1;

typedef enum AnomalyUiTableFlagsV1 {
    ANOMALY_UI_TABLE_V1_NONE = 0,
    ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT = 1u << 0u
} AnomalyUiTableFlagsV1;

// Stable C facade over the host UI implementation. Plugins never exchange C++ UI types.
// Draw callbacks are valid only during the current on_draw callback unless documented otherwise.
typedef struct AnomalyUiServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    void (ANOMALY_CALL *set_next_window_size)(
        void* user, float width, float height, uint32_t condition);
    int (ANOMALY_CALL *begin_window)(
        void* user, AnomalyStringViewV1 title, int* open, uint32_t flags);
    void (ANOMALY_CALL *end_window)(void* user);
    void (ANOMALY_CALL *text)(void* user, AnomalyStringViewV1 text);
    int (ANOMALY_CALL *button)(
        void* user, AnomalyStringViewV1 label, float width, float height);
    int (ANOMALY_CALL *draw_entity_bbox)(
        void* user, const AnomalyEspCameraV1* camera,
        const AnomalyEspEntityBoundsV1* bounds, const AnomalyEspBoxStyleV1* style);
    int (ANOMALY_CALL *checkbox)(void* user, AnomalyStringViewV1 label, int* value);
    int (ANOMALY_CALL *slider_float)(
        void* user, AnomalyStringViewV1 label, float* value,
        float minimum, float maximum);
    int (ANOMALY_CALL *color_edit4)(
        void* user, AnomalyStringViewV1 label, float rgba[4]);
    int (ANOMALY_CALL *draw_entity_box3d)(
        void* user, const AnomalyEspCameraV1* camera,
        const AnomalyEspEntityBoundsV1* bounds, const AnomalyEspBoxStyleV1* style);
    int (ANOMALY_CALL *draw_entity_label)(
        void* user, const AnomalyEspCameraV1* camera,
        const AnomalyEspEntityBoundsV1* bounds, AnomalyStringViewV1 text,
        uint32_t color_rgba);
    void (ANOMALY_CALL *separator)(void* user);
    int (ANOMALY_CALL *begin_child)(
        void* user, AnomalyStringViewV1 id,
        float width, float height, uint32_t flags);
    void (ANOMALY_CALL *end_child)(void* user);
    int (ANOMALY_CALL *begin_table)(
        void* user, AnomalyStringViewV1 id, int32_t columns,
        uint32_t flags, float outer_width, float outer_height);
    void (ANOMALY_CALL *table_next_row)(void* user);
    int (ANOMALY_CALL *table_next_column)(void* user);
    void (ANOMALY_CALL *end_table)(void* user);
    int (ANOMALY_CALL *begin_menu)(
        void* user, AnomalyStringViewV1 label, int enabled);
    void (ANOMALY_CALL *end_menu)(void* user);
    void (ANOMALY_CALL *open_popup)(void* user, AnomalyStringViewV1 id);
    int (ANOMALY_CALL *begin_popup_modal)(
        void* user, AnomalyStringViewV1 id, int* open, uint32_t flags);
    void (ANOMALY_CALL *end_popup)(void* user);
    void (ANOMALY_CALL *close_current_popup)(void* user);
    int (ANOMALY_CALL *filter_match)(
        void* user, AnomalyStringViewV1 filter, AnomalyStringViewV1 value);
    uint32_t (ANOMALY_CALL *frame_state)(void* user);
    void (ANOMALY_CALL *set_next_window_size_constraints)(
        void* user, float minimum_width, float minimum_height,
        float maximum_width, float maximum_height);
    void (ANOMALY_CALL *get_window_size)(void* user, float* width, float* height);
    int (ANOMALY_CALL *input_uint32)(
        void* user, AnomalyStringViewV1 label, uint32_t* value,
        uint32_t step, uint32_t step_fast);
    int (ANOMALY_CALL *input_double)(
        void* user, AnomalyStringViewV1 label, double* value,
        double step, double step_fast);
    // Valid during on_draw and on_update.
    int (ANOMALY_CALL *developer_mode_enabled)(void* user);
    // The buffer is plugin-owned, must have capacity for a trailing null, and is
    // accessed only during the current on_draw callback.
    int (ANOMALY_CALL *input_text)(
        void* user, AnomalyStringViewV1 label, char* buffer,
        size_t buffer_capacity, uint32_t flags);
    int (ANOMALY_CALL *button_enabled)(
        void* user, AnomalyStringViewV1 label,
        float width, float height, int enabled);
    void (ANOMALY_CALL *same_line)(
        void* user, float offset_from_start_x, float spacing);
    void (ANOMALY_CALL *set_cursor_pos_x)(void* user, float local_x);
    // Displays a text link and queues a host-owned browser launch when it is
    // left-clicked. The URL must use the http:// or https:// scheme.
    int (ANOMALY_CALL *text_link)(
        void* user, AnomalyStringViewV1 label, AnomalyStringViewV1 url);
} AnomalyUiServiceV1;

#ifdef __cplusplus
}
#endif
