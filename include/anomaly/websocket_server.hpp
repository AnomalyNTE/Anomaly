#pragma once

#include "anomaly/sdk/services/websocket.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace anomaly {

// General Runtime-owned local WebSocket broadcaster. It deliberately has no
// game or Profile dependency so any permitted plugin can publish text frames.
class WebSocketServer final {
public:
    explicit WebSocketServer(std::uint16_t port = 14514U);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    [[nodiscard]] bool Start() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool PublishText(std::string_view message) noexcept;
    [[nodiscard]] AnomalyWebSocketServerInfoV1 Snapshot() const noexcept;
    [[nodiscard]] const AnomalyWebSocketServiceV1* Service() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
