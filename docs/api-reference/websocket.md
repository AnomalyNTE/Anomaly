# WebSocket Service

`anomaly.websocket` is a general Runtime service. It is independent of UE, NTE,
Profiles, and rendering. The Runtime owns the listener, connection lifecycle,
and socket I/O; a plugin may only enqueue UTF-8 text frames for broadcast.

## Endpoint

- Listener: `127.0.0.1:14514` by default; the port can be changed while running
- Transport: RFC 6455 WebSocket, text frames
- Scope: process-local clients only
- Queue: bounded; `publish_text` returns `CONFLICT` when a frame cannot be admitted

The service does not assign meaning to JSON payloads and does not dispatch
inbound application messages. Protocol producers and consumers define their own
message schemas above this transport boundary.

## ABI

```c
#define ANOMALY_WEBSOCKET_SERVICE_V1_ID "anomaly.websocket"
#define ANOMALY_WEBSOCKET_SERVICE_V1_VERSION 1u

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
    AnomalyStatusV1 (ANOMALY_CALL *set_port)(void* user, uint16_t port);
} AnomalyWebSocketServiceV1;
```

`publish_text` copies the frame into the Runtime queue and returns before any
network I/O. It returns `UNAVAILABLE` after Runtime shutdown and `CONFLICT`
when the bounded queue is full. `server_info` requires a caller-initialized
`struct_size` and reports `port = 0` while the listener is unavailable.

`set_port` is an optional tail field. A caller must confirm that `struct_size`
reaches the field before dereferencing it. It accepts ports `1..65535` and
returns after queueing the change; socket binding runs on the Runtime-owned
worker. Poll `server_info` to observe the active port. The worker binds the
replacement listener before closing the current listener, so a failed bind
leaves the current listener and its port unchanged.

When multiple changes are queued before the worker processes them, the latest
request replaces earlier pending requests.

Plugins must declare the `websocket` capability to query this service. The
service may be marked optional when a plugin can operate without a local map
consumer.
