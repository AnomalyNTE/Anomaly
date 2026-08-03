#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_WEBSOCKET_SERVICE_V1_ID "anomaly.websocket"
#define ANOMALY_WEBSOCKET_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// This service owns a process-local WebSocket broadcast server. Plugins only
// enqueue UTF-8 text frames; connection management and socket I/O stay in the
// Runtime worker owned by the host.
typedef struct AnomalyWebSocketServerInfoV1 {
    uint32_t struct_size;
    uint16_t port;
    uint16_t reserved;
    uint32_t connected_clients;
    uint64_t published_messages;
    uint64_t dropped_messages;
} AnomalyWebSocketServerInfoV1;

typedef struct AnomalyWebSocketServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *publish_text)(
        void* user, AnomalyStringViewV1 message);
    AnomalyStatusV1 (ANOMALY_CALL *server_info)(
        void* user, AnomalyWebSocketServerInfoV1* info);
    // Optional V1 tail. Check struct_size before accessing this operation.
    AnomalyStatusV1 (ANOMALY_CALL *set_port)(void* user, uint16_t port);
} AnomalyWebSocketServiceV1;

#ifdef __cplusplus
}
#endif
