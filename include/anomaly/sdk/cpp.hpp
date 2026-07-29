#pragma once
#include "anomaly/sdk/anomaly_sdk.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
namespace anomaly::sdk {
[[nodiscard]] constexpr AnomalyStringViewV1 StringView(std::string_view value) noexcept { return {value.data(), value.size()}; }
[[nodiscard]] constexpr AnomalyStatusV1 Ok() noexcept { return {ANOMALY_STATUS_V1_OK, 0, {nullptr, 0}}; }
[[nodiscard]] constexpr bool Succeeded(const AnomalyStatusV1& status) noexcept { return status.code == ANOMALY_STATUS_V1_OK; }
template <typename Service> class ServiceRef final {
public:
    constexpr ServiceRef() noexcept = default;
    explicit constexpr ServiceRef(const Service* value) noexcept : value_(value) {}
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] constexpr const Service* get() const noexcept { return value_; }
    [[nodiscard]] constexpr const Service* operator->() const noexcept { return value_; }
private: const Service* value_{};
};
class Host final {
public:
    explicit constexpr Host(const AnomalyHostApiV1* host) noexcept : host_(host) {}
    template <typename Service> [[nodiscard]] ServiceRef<Service> Query(std::string_view id, std::uint32_t version = 1) const noexcept {
        static_assert(std::is_standard_layout_v<Service>);
        constexpr std::size_t host_query_size =
            offsetof(AnomalyHostApiV1, query_service) +
            sizeof(decltype(AnomalyHostApiV1::query_service));
        if (host_ == nullptr || host_->struct_size < host_query_size ||
            host_->api_major != ANOMALY_PLUGIN_API_V1_MAJOR || host_->query_service == nullptr) return {};
        const void* table{};
        const auto status = host_->query_service(host_->host_context, StringView(id), version, &table);
        if (!Succeeded(status) || table == nullptr) return {};
        const auto* service = static_cast<const Service*>(table);
        constexpr std::size_t service_prefix_size =
            offsetof(Service, user) + sizeof(decltype(Service::user));
        if (service->struct_size < service_prefix_size || service->service_version < version) return {};
        return ServiceRef<Service>(service);
    }
private: const AnomalyHostApiV1* host_{};
};
class UiWindow final {
public:
    UiWindow(const AnomalyUiServiceV1* ui, std::string_view title, int* open = nullptr, std::uint32_t flags = 0) noexcept
        : ui_(ui), active_(ui != nullptr && ui->begin_window != nullptr && ui->begin_window(ui->user, StringView(title), open, flags) != 0) {}
    ~UiWindow() { if (ui_ != nullptr && ui_->end_window != nullptr) ui_->end_window(ui_->user); }
    UiWindow(const UiWindow&) = delete; UiWindow& operator=(const UiWindow&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return active_; }
private: const AnomalyUiServiceV1* ui_{}; bool active_{};
};
}  // namespace anomaly::sdk
