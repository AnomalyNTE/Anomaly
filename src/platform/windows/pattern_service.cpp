#include "anomaly/pattern_service.hpp"

#include <stdexcept>
#include <utility>

namespace anomaly {

PatternService::PatternService(std::shared_ptr<const ModuleMemoryService> memory)
    : memory_(std::move(memory)) {
    if (!memory_) throw std::invalid_argument("PatternService requires memory");
}

ue5mem::Pattern PatternService::Parse(std::string_view text) const {
    return ue5mem::Pattern::Parse(text);
}

std::vector<std::uintptr_t> PatternService::ScanSection(
    const ue5mem::ModuleInfo& module,
    std::string_view section_name,
    const ue5mem::Pattern& pattern,
    std::size_t limit) const {
    return memory_->ScanSection(module, section_name, pattern, limit);
}

std::shared_ptr<const ModuleMemoryService> PatternService::Memory() const noexcept {
    return memory_;
}

CoreMemoryServices CreateCoreMemoryServices() {
    auto memory = std::make_shared<const ModuleMemoryService>();
    auto pattern = std::make_shared<const PatternService>(memory);
    return {std::move(memory), std::move(pattern)};
}

CoreMemoryServices NormalizeCoreMemoryServices(CoreMemoryServices memory_services) {
    if (!memory_services.memory && !memory_services.patterns) {
        return CreateCoreMemoryServices();
    }
    if (!memory_services.memory || !memory_services.patterns) {
        throw std::invalid_argument("core memory service bundle is incomplete");
    }

    const auto pattern_memory = memory_services.patterns->Memory();
    if (pattern_memory.get() != memory_services.memory.get() ||
        pattern_memory.owner_before(memory_services.memory) ||
        memory_services.memory.owner_before(pattern_memory)) {
        throw std::invalid_argument("pattern service is bound to a different memory service");
    }
    return memory_services;
}

}  // namespace anomaly
