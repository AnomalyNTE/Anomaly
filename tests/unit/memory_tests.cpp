#include "memory.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <ranges>

int main() {
    using namespace ue5mem;
    auto* page = static_cast<std::uint8_t*>(AllocateMemory(4096));
    auto* target = static_cast<std::uint8_t*>(AllocateMemory(8192));
    if (page == nullptr || target == nullptr) return 1;

    const std::array<std::uint8_t, 4> first{0x10, 0x20, 0x30, 0x40};
    if (!WriteMemory(reinterpret_cast<std::uintptr_t>(target), first.data(), first.size())) return 2;
    std::array<std::uint8_t, 4> read{};
    if (!ReadMemoryInto(reinterpret_cast<std::uintptr_t>(target), read.data(), read.size()) || read != first) {
        return 3;
    }

    const auto pointer = reinterpret_cast<std::uintptr_t>(target);
    if (!WriteMemory(reinterpret_cast<std::uintptr_t>(page), &pointer, sizeof(pointer))) return 4;
    constexpr std::ptrdiff_t offsets[]{2};
    const auto chain = ResolvePointerChain(reinterpret_cast<std::uintptr_t>(page), offsets, 1);
    if (!chain || *chain != pointer + 2) return 5;

    DWORD previous{};
    DWORD ignored{};
    if (!ProtectMemory(reinterpret_cast<std::uintptr_t>(target), 4096, PAGE_READONLY, previous)) return 6;
    const std::array<std::uint8_t, 2> patch{0xAA, 0xBB};
    if (WriteMemory(reinterpret_cast<std::uintptr_t>(target), patch.data(), patch.size())) return 7;
    if (!PatchMemory(reinterpret_cast<std::uintptr_t>(target), patch.data(), patch.size())) return 8;
    if (!ReadMemoryInto(reinterpret_cast<std::uintptr_t>(target), read.data(), patch.size()) ||
        read[0] != patch[0] || read[1] != patch[1]) {
        return 9;
    }

    if (!ProtectMemory(reinterpret_cast<std::uintptr_t>(target), 4096, PAGE_READWRITE, ignored)) return 10;
    if (!ProtectMemory(reinterpret_cast<std::uintptr_t>(target + 4096), 4096, PAGE_READONLY, ignored)) return 11;
    if (!PatchMemory(reinterpret_cast<std::uintptr_t>(target + 4095), patch.data(), patch.size())) return 12;
    MEMORY_BASIC_INFORMATION first_page{};
    MEMORY_BASIC_INFORMATION second_page{};
    VirtualQuery(target, &first_page, sizeof(first_page));
    VirtualQuery(target + 4096, &second_page, sizeof(second_page));
    if ((first_page.Protect & 0xff) != PAGE_READWRITE ||
        (second_page.Protect & 0xff) != PAGE_READONLY) {
        return 13;
    }

    const auto current_module = FindModule({});
    if (!current_module) return 14;
    auto* decoy_headers = static_cast<std::uint8_t*>(AllocateMemory(4096));
    if (decoy_headers == nullptr) return 15;
    IMAGE_DOS_HEADER decoy_dos{};
    decoy_dos.e_magic = IMAGE_DOS_SIGNATURE;
    decoy_dos.e_lfanew = 0x80;
    IMAGE_NT_HEADERS64 decoy_nt{};
    decoy_nt.Signature = IMAGE_NT_SIGNATURE;
    decoy_nt.FileHeader.NumberOfSections = 1;
    decoy_nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    decoy_nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    decoy_nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(current_module->size);
    IMAGE_SECTION_HEADER decoy_section{};
    constexpr char decoy_name[] = ".decoy";
    std::memcpy(decoy_section.Name, decoy_name, sizeof(decoy_name) - 1);
    decoy_section.VirtualAddress = 0x1000;
    decoy_section.Misc.VirtualSize = 0x1000;
    const auto decoy_nt_address = reinterpret_cast<std::uintptr_t>(decoy_headers) +
        static_cast<std::uintptr_t>(decoy_dos.e_lfanew);
    const auto decoy_section_address = decoy_nt_address + sizeof(DWORD) +
        sizeof(IMAGE_FILE_HEADER) + decoy_nt.FileHeader.SizeOfOptionalHeader;
    if (!WriteMemory(
            reinterpret_cast<std::uintptr_t>(decoy_headers),
            &decoy_dos, sizeof(decoy_dos)) ||
        !WriteMemory(decoy_nt_address, &decoy_nt, sizeof(decoy_nt)) ||
        !WriteMemory(decoy_section_address, &decoy_section, sizeof(decoy_section))) {
        return 16;
    }
    ModuleInfo decoy_module = *current_module;
    decoy_module.base = reinterpret_cast<std::uintptr_t>(decoy_headers);
    const auto memory_sections = EnumerateSections(decoy_module);
    if (std::ranges::none_of(memory_sections, [](const SectionInfo& section) {
            return section.name == ".decoy";
        }) || std::ranges::any_of(memory_sections, [](const SectionInfo& section) {
            return section.name == ".text";
        })) {
        return 17;
    }

    ModuleInfo stripped_module = *current_module;
    stripped_module.base = 1;
    const auto disk_sections = EnumerateSections(stripped_module);
    if (std::ranges::none_of(disk_sections, [](const SectionInfo& section) {
            return section.name == ".text" && section.virtual_size != 0;
        })) {
        return 18;
    }

    ProtectMemory(reinterpret_cast<std::uintptr_t>(target), 4096, previous, ignored);
    if (!FreeMemory(decoy_headers) || !FreeMemory(target) || !FreeMemory(page)) return 19;
    std::cout << "memory operations passed\n";
    return 0;
}
