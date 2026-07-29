#pragma once

#include "anomaly/sdk/anomaly_sdk.h"

#include "loot_catalog.generated.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pink_paw_heist_esp {

enum class LootClassResolution {
    resolved,
    unresolved,
    retry,
};

struct LootClassMetadata final {
    std::uint32_t name_id{};
    std::string name;
    const catalog::ItemDefinition* item{};
    bool bank_box{};
};

class LootClassCache final {
public:
    template <typename EntityService>
    [[nodiscard]] LootClassResolution Resolve(
        const EntityService* entities,
        const std::uint64_t class_id,
        const std::uint32_t class_name_id,
        const LootClassMetadata*& metadata) {
        metadata = nullptr;
        if (const auto found = entries_.find(class_id); found != entries_.end()) {
            if (found->second.name_id == class_name_id) {
                metadata = &found->second;
                return LootClassResolution::resolved;
            }
            entries_.erase(found);
        }

        constexpr std::size_t kMaximumResolvedNameBytes = 1024;
        constexpr std::size_t kClassNameFieldEnd =
            offsetof(EntityService, class_name_utf8) +
            sizeof(decltype(EntityService::class_name_utf8));
        if (entities == nullptr || entities->struct_size < kClassNameFieldEnd ||
            entities->class_name_utf8 == nullptr) {
            return LootClassResolution::retry;
        }

        std::size_t size{};
        const AnomalyStatusV1 size_status =
            entities->class_name_utf8(entities->user, class_id, nullptr, &size);
        if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            return LootClassResolution::unresolved;
        }
        if (size_status.code != ANOMALY_STATUS_V1_OK || size <= 1 ||
            size > kMaximumResolvedNameBytes) {
            return LootClassResolution::retry;
        }

        LootClassMetadata candidate;
        candidate.name_id = class_name_id;
        candidate.name.assign(size, '\0');
        const AnomalyStatusV1 copy_status = entities->class_name_utf8(
            entities->user, class_id, candidate.name.data(), &size);
        if (copy_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            return LootClassResolution::unresolved;
        }
        if (copy_status.code != ANOMALY_STATUS_V1_OK || size == 0 ||
            size > candidate.name.size()) {
            return LootClassResolution::retry;
        }
        if (const void* terminator =
                std::memchr(candidate.name.data(), '\0', candidate.name.size());
            terminator != nullptr) {
            candidate.name.resize(
                static_cast<const char*>(terminator) - candidate.name.data());
        }
        if (candidate.name.empty()) return LootClassResolution::unresolved;

        candidate.bank_box =
            catalog::FindAsciiInsensitive(candidate.name, "BankBox_") != std::string_view::npos;
        if (candidate.bank_box) candidate.item = catalog::FindItemDefinition(candidate.name);
        const auto [inserted, added] = entries_.emplace(class_id, std::move(candidate));
        if (!added) return LootClassResolution::retry;
        metadata = &inserted->second;
        return LootClassResolution::resolved;
    }

    void Clear() noexcept { entries_.clear(); }

private:
    std::unordered_map<std::uint64_t, LootClassMetadata> entries_;
};

}  // namespace pink_paw_heist_esp
