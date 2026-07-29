#include "anomaly/module_memory_service.hpp"

namespace anomaly {

std::vector<ue5mem::ModuleInfo> ModuleMemoryService::EnumerateModules() const {
    return ue5mem::EnumerateModules();
}

std::optional<ue5mem::ModuleInfo> ModuleMemoryService::FindModule(
    std::wstring_view name) const {
    return ue5mem::FindModule(name);
}

std::vector<ue5mem::SectionInfo> ModuleMemoryService::EnumerateSections(
    const ue5mem::ModuleInfo& module) const {
    return ue5mem::EnumerateSections(module);
}

std::vector<ue5mem::RegionInfo> ModuleMemoryService::EnumerateRegions(
    const ue5mem::ModuleInfo& module) const {
    return ue5mem::EnumerateRegions(module);
}

std::vector<std::uintptr_t> ModuleMemoryService::ScanSection(
    const ue5mem::ModuleInfo& module,
    std::string_view section_name,
    const ue5mem::Pattern& pattern,
    std::size_t limit) const {
    return ue5mem::ScanSection(module, section_name, pattern, limit);
}

std::optional<std::uintptr_t> ModuleMemoryService::ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend) const {
    return ue5mem::ResolveRipRelative(
        instruction, displacement_offset, instruction_size, addend);
}

std::optional<std::vector<std::uint8_t>> ModuleMemoryService::ReadMemory(
    std::uintptr_t address, std::size_t size) const {
    return ue5mem::ReadMemory(address, size);
}

bool ModuleMemoryService::ReadMemoryInto(
    std::uintptr_t address, void* destination, std::size_t size) const {
    return ue5mem::ReadMemoryInto(address, destination, size);
}

bool ModuleMemoryService::WriteMemory(
    std::uintptr_t address, const void* source, std::size_t size) const {
    return ue5mem::WriteMemory(address, source, size);
}

bool ModuleMemoryService::PatchMemory(
    std::uintptr_t address, const void* source, std::size_t size) const {
    return ue5mem::PatchMemory(address, source, size);
}

bool ModuleMemoryService::ProtectMemory(
    std::uintptr_t address,
    std::size_t size,
    DWORD protection,
    DWORD& previous) const {
    return ue5mem::ProtectMemory(address, size, protection, previous);
}

void* ModuleMemoryService::AllocateMemory(std::size_t size, DWORD protection) const {
    return ue5mem::AllocateMemory(size, protection);
}

bool ModuleMemoryService::FreeMemory(void* address) const {
    return ue5mem::FreeMemory(address);
}

std::optional<std::uintptr_t> ModuleMemoryService::ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count) const {
    return ue5mem::ResolvePointerChain(base, offsets, count);
}

std::string ModuleMemoryService::ProtectionName(DWORD protection) const {
    return ue5mem::ProtectionName(protection);
}

std::string ModuleMemoryService::StateName(DWORD state) const {
    return ue5mem::StateName(state);
}

std::string ModuleMemoryService::TypeName(DWORD type) const {
    return ue5mem::TypeName(type);
}

}  // namespace anomaly
