#pragma once

#include "anomaly/nte_esc_menu_button.hpp"
#include "anomaly/pattern_service.hpp"
#include "anomaly/symbol_resolver.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace anomaly {

class NteEscMenuBridge final {
public:
    using SnapshotProvider = std::function<std::vector<NteEscMenuButtonSnapshot>()>;
    using InvokeButton = std::function<void(AnomalyGenerationHandleV1)>;
    using Logger = std::function<void(std::uint32_t level, std::string message)>;

    NteEscMenuBridge(
        CoreMemoryServices memory_services,
        ProfileResolutionSnapshot resolution,
        SnapshotProvider snapshot_provider,
        InvokeButton invoke_button,
        Logger logger = {});
    ~NteEscMenuBridge();

    NteEscMenuBridge(const NteEscMenuBridge&) = delete;
    NteEscMenuBridge& operator=(const NteEscMenuBridge&) = delete;

    [[nodiscard]] bool Start();
    // Runs bounded reflection discovery on the Runtime worker, outside the game callback.
    void Discover() noexcept;
    // Consumes discovery state and mutates NTE widgets only from the game callback.
    void Update(double delta_seconds) noexcept;
    [[nodiscard]] bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
