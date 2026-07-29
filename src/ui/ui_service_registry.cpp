#include "anomaly/ui_service_registry.hpp"

namespace anomaly {

bool UiServiceRegistry::Publish(const AnomalyUiServiceV1* service) noexcept {
    if (service == nullptr || service->struct_size < sizeof(AnomalyUiServiceV1) ||
        service->service_version != ANOMALY_UI_SERVICE_V1_VERSION ||
        service->begin_window == nullptr ||
        service->end_window == nullptr || service->text == nullptr) {
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        service_ = service;
        ++generation_;
    }
    condition_.notify_all();
    return true;
}

void UiServiceRegistry::Withdraw(const AnomalyUiServiceV1* service) noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (service_ != service) return;
        service_ = nullptr;
        ++generation_;
    }
    condition_.notify_all();
}

UiServiceSnapshot UiServiceRegistry::Query(std::uint32_t minimum_version) const noexcept {
    std::scoped_lock lock(mutex_);
    if (service_ == nullptr || service_->service_version < minimum_version) return {};
    return {service_, generation_};
}

UiServiceSnapshot UiServiceRegistry::WaitFor(
    std::uint32_t minimum_version, std::chrono::milliseconds timeout) const noexcept {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, timeout, [&] {
        return service_ != nullptr && service_->service_version >= minimum_version;
    });
    if (service_ == nullptr || service_->service_version < minimum_version) return {};
    return {service_, generation_};
}

}  // namespace anomaly
