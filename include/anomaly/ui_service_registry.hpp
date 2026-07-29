#pragma once

#include "anomaly/sdk/anomaly_sdk.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace anomaly {

struct UiServiceSnapshot {
    const AnomalyUiServiceV1* service{};
    std::uint64_t generation{};

    [[nodiscard]] explicit operator bool() const noexcept { return service != nullptr; }
};

class UiServiceRegistry final {
public:
    [[nodiscard]] bool Publish(const AnomalyUiServiceV1* service) noexcept;
    void Withdraw(const AnomalyUiServiceV1* service) noexcept;
    [[nodiscard]] UiServiceSnapshot Query(std::uint32_t minimum_version = 1) const noexcept;
    [[nodiscard]] UiServiceSnapshot WaitFor(
        std::uint32_t minimum_version,
        std::chrono::milliseconds timeout) const noexcept;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    const AnomalyUiServiceV1* service_{};
    std::uint64_t generation_{};
};

}  // namespace anomaly
