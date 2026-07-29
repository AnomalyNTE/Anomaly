#pragma once

#include "anomaly/module_memory_service.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace anomaly {

class PatternService final {
public:
    explicit PatternService(std::shared_ptr<const ModuleMemoryService> memory);

    [[nodiscard]] ue5mem::Pattern Parse(std::string_view text) const;
    [[nodiscard]] std::vector<std::uintptr_t> ScanSection(
        const ue5mem::ModuleInfo& module,
        std::string_view section_name,
        const ue5mem::Pattern& pattern,
        std::size_t limit) const;
    [[nodiscard]] std::shared_ptr<const ModuleMemoryService> Memory() const noexcept;

private:
    std::shared_ptr<const ModuleMemoryService> memory_;
};

struct CoreMemoryServices final {
    std::shared_ptr<const ModuleMemoryService> memory;
    std::shared_ptr<const PatternService> patterns;
};

[[nodiscard]] CoreMemoryServices CreateCoreMemoryServices();
[[nodiscard]] CoreMemoryServices NormalizeCoreMemoryServices(
    CoreMemoryServices memory_services);

}  // namespace anomaly
