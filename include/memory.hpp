#pragma once

#include "pattern.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ue5mem {

struct ModuleInfo {
    std::wstring name;
    std::wstring path;
    std::uintptr_t base{};
    std::size_t size{};
};

struct SectionInfo {
    std::string name;
    std::uintptr_t base{};
    std::size_t virtual_size{};
    DWORD characteristics{};
};

struct RegionInfo {
    std::uintptr_t base{};
    std::size_t size{};
    DWORD state{};
    DWORD protection{};
    DWORD type{};
};

std::vector<ModuleInfo> EnumerateModules();
std::optional<ModuleInfo> FindModule(std::wstring_view name);
std::vector<SectionInfo> EnumerateSections(const ModuleInfo& module);
std::vector<RegionInfo> EnumerateRegions(const ModuleInfo& module);
std::vector<std::uintptr_t> ScanSection(
    const ModuleInfo& module,
    std::string_view section_name,
    const Pattern& pattern,
    std::size_t limit);
std::optional<std::uintptr_t> ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend);
std::optional<std::vector<std::uint8_t>> ReadMemory(std::uintptr_t address, std::size_t size);
bool ReadMemoryInto(std::uintptr_t address, void* destination, std::size_t size);
bool WriteMemory(std::uintptr_t address, const void* source, std::size_t size);
bool PatchMemory(std::uintptr_t address, const void* source, std::size_t size);
bool ProtectMemory(std::uintptr_t address, std::size_t size, DWORD protection, DWORD& previous);
void* AllocateMemory(std::size_t size, DWORD protection = PAGE_READWRITE);
bool FreeMemory(void* address);
std::optional<std::uintptr_t> ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count);

std::string ProtectionName(DWORD protection);
std::string StateName(DWORD state);
std::string TypeName(DWORD type);

}  // namespace ue5mem
