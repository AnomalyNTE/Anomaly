#pragma once

#include "memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

class ModuleMemoryService final {
public:
    [[nodiscard]] std::vector<ue5mem::ModuleInfo> EnumerateModules() const;
    [[nodiscard]] std::optional<ue5mem::ModuleInfo> FindModule(
        std::wstring_view name) const;
    [[nodiscard]] std::vector<ue5mem::SectionInfo> EnumerateSections(
        const ue5mem::ModuleInfo& module) const;
    [[nodiscard]] std::vector<ue5mem::RegionInfo> EnumerateRegions(
        const ue5mem::ModuleInfo& module) const;
    [[nodiscard]] std::vector<std::uintptr_t> ScanSection(
        const ue5mem::ModuleInfo& module,
        std::string_view section_name,
        const ue5mem::Pattern& pattern,
        std::size_t limit) const;

    [[nodiscard]] std::optional<std::uintptr_t> ResolveRipRelative(
        std::uintptr_t instruction,
        std::size_t displacement_offset,
        std::size_t instruction_size,
        std::ptrdiff_t addend) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadMemory(
        std::uintptr_t address, std::size_t size) const;
    [[nodiscard]] bool ReadMemoryInto(
        std::uintptr_t address, void* destination, std::size_t size) const;
    [[nodiscard]] bool WriteMemory(
        std::uintptr_t address, const void* source, std::size_t size) const;
    [[nodiscard]] bool PatchMemory(
        std::uintptr_t address, const void* source, std::size_t size) const;
    [[nodiscard]] bool ProtectMemory(
        std::uintptr_t address,
        std::size_t size,
        DWORD protection,
        DWORD& previous) const;
    [[nodiscard]] void* AllocateMemory(
        std::size_t size, DWORD protection = PAGE_READWRITE) const;
    [[nodiscard]] bool FreeMemory(void* address) const;
    [[nodiscard]] std::optional<std::uintptr_t> ResolvePointerChain(
        std::uintptr_t base,
        const std::ptrdiff_t* offsets,
        std::size_t count) const;

    [[nodiscard]] std::string ProtectionName(DWORD protection) const;
    [[nodiscard]] std::string StateName(DWORD state) const;
    [[nodiscard]] std::string TypeName(DWORD type) const;
};

}  // namespace anomaly
