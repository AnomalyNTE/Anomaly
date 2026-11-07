#pragma once
#include "anomaly/sdk/base.h"
#define ANOMALY_MODS_SERVICE_V1_ID "anomaly.mods"
#define ANOMALY_MODS_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AnomalyModEntryV1 {
    uint32_t struct_size;
    uint32_t enabled;
    uint32_t disabled_files;
    uint32_t index_complete;
    AnomalyStringViewV1 key;
    AnomalyStringViewV1 name;
    // Comma-separated vehicle identifiers, derived from container paths; vehicle cleanup only.
    AnomalyStringViewV1 vehicles;
    AnomalyStringViewV1 analysis_error;
    // Optional tail: comma-separated classification identifiers from resource directories.
    // Presentation only; a shared group does not imply a conflict or support for cleanup.
    AnomalyStringViewV1 resource_groups;
} AnomalyModEntryV1;
typedef int (ANOMALY_CALL *AnomalyModVisitorV1)(void* user, const AnomalyModEntryV1* entry);
typedef int (ANOMALY_CALL *AnomalyModAssetVisitorV1)(void* user, AnomalyStringViewV1 package_path);
typedef struct AnomalyModsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    // Any thread, immutable process startup mode: 1 = compatibility, 0 = hot.
    uint32_t (ANOMALY_CALL *compatibility_mode)(void* user);
    // Lifecycle/Worker only. Visitors run synchronously without internal locks held;
    // all views expire on callback return. No callbacks are retained.
    AnomalyStatusV1 (ANOMALY_CALL *enumerate)(void* user, AnomalyModVisitorV1 visitor, void* visitor_user);
    // Writes the same settings as Launcher; enabling atomically disables conflicting mods.
    // The runtime watcher applies the selection. OK is a saved choice, not proof of mounting/GC.
    AnomalyStatusV1 (ANOMALY_CALL *set_enabled)(void* user, AnomalyStringViewV1 key, uint32_t enabled);
    // Enumerates package paths from the complete index; fails if any container is unreadable.
    AnomalyStatusV1 (ANOMALY_CALL *assets)(void* user, AnomalyStringViewV1 key,
        AnomalyModAssetVisitorV1 visitor, void* visitor_user);
    // Worker only. Applies and verifies the effective selection. Conflicting mods keep their
    // current mounts and are reported in pak-mods.json; unrelated mods still synchronize.
    // Does not clear resident UObjects. Compatibility mode returns CONFLICT.
    AnomalyStatusV1 (ANOMALY_CALL *synchronize)(void* user);
} AnomalyModsServiceV1;
#ifdef __cplusplus
}
#endif
