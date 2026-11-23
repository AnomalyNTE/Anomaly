#pragma once

#include "anomaly/plugin_scope.hpp"
#include "anomaly/sdk/services/web.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace anomaly {

inline constexpr char kWebProviderId[] = "anomaly.internal.web-provider";
inline constexpr char kWebServerServiceId[] = "anomaly.internal.web-server";

class WebServer;

// AdapterServiceRegistry requires a size/version/user prefix even for an
// internal provider. The C++ server object itself is not a service table.
struct WebProviderV1 final {
    std::uint32_t struct_size;
    std::uint32_t service_version;
    WebServer* user;
};

class WebServer final {
public:
    using SubmitWorker = std::function<bool(std::string, std::uint64_t, std::function<void()>)>;
    explicit WebServer(SubmitWorker submit);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;
    [[nodiscard]] bool Start() noexcept;
    [[nodiscard]] std::uint32_t LastError() const noexcept;
    [[nodiscard]] const WebProviderV1* Provider() const noexcept;
    void Stop() noexcept;
    [[nodiscard]] AnomalyStatusV1 Register(
        std::shared_ptr<PluginScope> scope, const std::filesystem::path& package,
        std::string_view assets, AnomalyWebCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* application, std::shared_ptr<void> callback_lifetime) noexcept;
    [[nodiscard]] AnomalyStatusV1 Unregister(
        const std::shared_ptr<PluginScope>& scope, AnomalyGenerationHandleV1 application) noexcept;
    [[nodiscard]] AnomalyStatusV1 Url(
        const std::shared_ptr<PluginScope>& scope, AnomalyGenerationHandleV1 application,
        char* destination, std::size_t* inout_size) noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    WebProviderV1 provider_;
};

}  // namespace anomaly
