#include "anomaly/pattern_service.hpp"
#include "analyzer.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

volatile std::uint64_t g_xref_probe = 0x6A09E667F3BCC909ULL;

__declspec(noinline) std::uint64_t ReadXrefProbe() {
    return g_xref_probe;
}

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool IsOk(std::string_view response) {
    return response.find("\"ok\":true") != std::string_view::npos;
}

std::string PatternText(std::span<const std::uint8_t> bytes) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 3);
    for (const std::uint8_t byte : bytes) {
        if (!result.empty()) result.push_back(' ');
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0FU]);
    }
    return result;
}

bool TestBundleOwnershipAndPatternParsing() {
    const anomaly::CoreMemoryServices services = anomaly::CreateCoreMemoryServices();
    if (!Check(services.memory && services.patterns,
               "Core memory service factory returned an empty facade")) {
        return false;
    }
    const auto pattern_memory = services.patterns->Memory();
    bool result = Check(
        pattern_memory.get() == services.memory.get() &&
            !pattern_memory.owner_before(services.memory) &&
            !services.memory.owner_before(pattern_memory),
        "Pattern facade does not share the bundle memory instance");

    const std::array<std::uint8_t, 10> bytes{
        0x48, 0x8B, 0x15, 0xAA, 0x48, 0x8B, 0x25, 0xBB, 0xCC, 0xDD};
    const auto wildcard = services.patterns->Parse("48 8b ?5 ??").FindAll(bytes);
    result = Check(wildcard == std::vector<std::size_t>{0, 4},
                   "Pattern facade changed wildcard parsing semantics") && result;

    bool invalid_rejected{};
    try {
        static_cast<void>(services.patterns->Parse("GG"));
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    return Check(invalid_rejected,
                 "Pattern facade did not preserve parse failures") && result;
}

bool TestBundleValidation() {
    const auto first = anomaly::CreateCoreMemoryServices();
    const auto second = anomaly::CreateCoreMemoryServices();
    bool partial_rejected{};
    try {
        static_cast<void>(anomaly::NormalizeCoreMemoryServices({first.memory, {}}));
    } catch (const std::invalid_argument&) {
        partial_rejected = true;
    }

    bool mismatch_rejected{};
    try {
        static_cast<void>(
            anomaly::NormalizeCoreMemoryServices({first.memory, second.patterns}));
    } catch (const std::invalid_argument&) {
        mismatch_rejected = true;
    }

    const auto defaults = anomaly::NormalizeCoreMemoryServices({});
    return Check(partial_rejected, "Partial memory service bundle was accepted") &&
        Check(mismatch_rejected, "Mismatched memory service bundle was accepted") &&
        Check(defaults.memory && defaults.patterns &&
                  defaults.patterns->Memory().get() == defaults.memory.get(),
              "Default memory service bundle was not normalized");
}

bool TestCurrentModuleDiscoveryAndScan() {
    const anomaly::CoreMemoryServices services = anomaly::CreateCoreMemoryServices();
    const auto modules = services.memory->EnumerateModules();
    const auto current = services.memory->FindModule(L"");
    const auto expected_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    bool result = Check(!modules.empty(),
                        "Memory facade did not enumerate the current process") &&
        Check(current.has_value() && current->base == expected_base && current->size != 0,
              "Memory facade did not find the current module") &&
        Check(current && std::ranges::any_of(modules, [&](const auto& module) {
                  return module.base == current->base;
              }),
              "Current module is absent from the facade enumeration");
    if (!current) return false;

    const auto sections = services.memory->EnumerateSections(*current);
    const auto regions = services.memory->EnumerateRegions(*current);
    result = Check(!sections.empty(),
                   "Memory facade did not enumerate PE sections") &&
        Check(!regions.empty(),
              "Memory facade did not enumerate module regions") &&
        Check(services.memory->ProtectionName(PAGE_READWRITE) == "rw" &&
                  services.memory->StateName(MEM_COMMIT) == "commit" &&
                  services.memory->TypeName(MEM_IMAGE) == "image",
              "Memory facade did not preserve region label formatting") && result;
    const auto text = std::ranges::find_if(sections, [](const auto& section) {
        return section.name == ".text" && section.virtual_size >= 16;
    });
    if (!Check(text != sections.end(), "Current module has no scannable .text section")) {
        return false;
    }

    constexpr std::size_t signature_size = 16;
    const auto signature = services.memory->ReadMemory(text->base, signature_size);
    if (!Check(signature.has_value() && signature->size() == signature_size,
               "Memory facade did not read the .text signature")) {
        return false;
    }
    const auto pattern = services.patterns->Parse(PatternText(*signature));
    const auto direct_matches =
        services.memory->ScanSection(*current, text->name, pattern, 1);
    const auto pattern_matches =
        services.patterns->ScanSection(*current, text->name, pattern, 1);
    return Check(direct_matches == std::vector<std::uintptr_t>{text->base},
                 "Memory facade section scan did not find the section prefix") &&
        Check(pattern_matches == direct_matches,
              "Pattern facade did not delegate section scanning to memory") && result;
}

bool TestAnalyzerUsesInjectedServices() {
    const auto services = anomaly::CreateCoreMemoryServices();
    ue5mem::AnalyzerConfig config;
    const ue5mem::Analyzer analyzer(
        std::filesystem::current_path(), config, {}, services);
    const auto& analyzer_services = analyzer.MemoryServices();
    const auto analyzer_pattern_memory = analyzer_services.patterns->Memory();
    bool result = Check(
        analyzer_services.memory.get() == services.memory.get() &&
            analyzer_services.patterns.get() == services.patterns.get() &&
            analyzer_pattern_memory.get() == analyzer_services.memory.get() &&
            !analyzer_pattern_memory.owner_before(analyzer_services.memory) &&
            !analyzer_services.memory.owner_before(analyzer_pattern_memory),
        "Analyzer did not retain the injected memory service bundle");

    result = Check(IsOk(analyzer.Execute("modules")),
                   "Analyzer modules command failed through the memory facade") && result;
    result = Check(IsOk(analyzer.Execute("sections .")),
                   "Analyzer sections command failed through the memory facade") && result;
    result = Check(IsOk(analyzer.Execute("regions .")),
                   "Analyzer regions command failed through the memory facade") && result;
    result = Check(IsOk(analyzer.Execute("scan . .text ??")),
                   "Analyzer scan command failed through the pattern facade") && result;
    std::string reflection_arguments;
    const ue5mem::Analyzer reflection_analyzer(
        std::filesystem::current_path(), config, {}, services, {},
        [&](std::string_view arguments) {
            reflection_arguments = arguments;
            return std::string{"{\"ok\":true,\"routed\":true}"};
        });
    result = Check(
                 IsOk(reflection_analyzer.Execute("ue functions autopilot 32 4096")) &&
                     reflection_arguments == "functions autopilot 32 4096" &&
                     !IsOk(analyzer.Execute("ue functions autopilot")),
                 "Analyzer did not route UE reflection queries through its provider") && result;
    const std::string xrefs = analyzer.Execute(
        "xrefs . " + std::to_string(reinterpret_cast<std::uintptr_t>(&g_xref_probe)));
    result = Check(
        ReadXrefProbe() == g_xref_probe && IsOk(xrefs) &&
            xrefs.find("\"count\":0") == std::string::npos &&
            xrefs.find("\"kind\":\"mov-load-rip\"") != std::string::npos,
        "Analyzer xrefs command did not find the executable RIP-relative fixture") && result;

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page_size = system_info.dwPageSize;
    void* allocation = services.memory->AllocateMemory(page_size);
    if (!Check(allocation != nullptr, "Analyzer command fixture allocation failed")) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(allocation);
    const std::string address_text = std::to_string(address);
    result = Check(IsOk(analyzer.Execute("write " + address_text + " DE AD BE EF")),
                   "Analyzer write command failed through the memory facade") && result;
    const std::string read = analyzer.Execute("read " + address_text + " 4");
    result = Check(IsOk(read) && read.find("de ad be ef") != std::string::npos,
                   "Analyzer read command did not return facade-written bytes") && result;
    result = Check(IsOk(analyzer.Execute(
                       "protect " + address_text + " " +
                       std::to_string(page_size) + " r")),
                   "Analyzer protect command failed through the memory facade") && result;
    result = Check(IsOk(analyzer.Execute("patch " + address_text + " AA BB")),
                   "Analyzer patch command failed through the memory facade") && result;

    DWORD ignored{};
    result = Check(services.memory->ProtectMemory(
                       address, page_size, PAGE_READWRITE, ignored),
                   "Analyzer command fixture did not restore page protection") && result;
    const std::string freed = analyzer.Execute("free " + address_text);
    if (!IsOk(freed)) static_cast<void>(services.memory->FreeMemory(allocation));
    return Check(IsOk(freed),
                 "Analyzer free command failed through the memory facade") && result;
}

bool TestAnalyzerReadLimit() {
    const anomaly::CoreMemoryServices services = anomaly::CreateCoreMemoryServices();
    ue5mem::AnalyzerConfig config;
    const ue5mem::Analyzer analyzer(
        std::filesystem::current_path(), config, {}, services);

    constexpr std::size_t read_limit = 1024U * 1024U;
    auto* allocation = static_cast<std::uint8_t*>(services.memory->AllocateMemory(read_limit));
    if (!Check(allocation != nullptr, "Analyzer read-limit fixture allocation failed")) {
        return false;
    }
    allocation[0] = 0xAB;
    const std::string address = std::to_string(reinterpret_cast<std::uintptr_t>(allocation));
    const std::string accepted = analyzer.Execute(
        "read " + address + " " + std::to_string(read_limit));
    const std::string rejected = analyzer.Execute(
        "read " + address + " " + std::to_string(read_limit + 1));
    const std::string help = analyzer.Execute("help");
    const bool released = services.memory->FreeMemory(allocation);

    return Check(IsOk(accepted) && accepted.find("\"bytes\":\"ab") != std::string::npos,
                 "Analyzer did not allow a 1 MiB read") &&
        Check(!IsOk(rejected) &&
                  rejected.find("usage: read <address> <size<=1048576>") != std::string::npos,
              "Analyzer did not reject a read larger than 1 MiB") &&
        Check(IsOk(help) &&
                  help.find("read <address> <size<=1048576>") != std::string::npos,
              "Analyzer help did not describe the 1 MiB read limit") &&
        Check(released, "Analyzer read-limit fixture cleanup failed");
}

bool TestControlledMemoryOperations() {
    const anomaly::CoreMemoryServices services = anomaly::CreateCoreMemoryServices();
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page_size = system_info.dwPageSize;
    auto* allocation = static_cast<std::uint8_t*>(
        services.memory->AllocateMemory(page_size));
    if (!Check(allocation != nullptr, "Memory facade allocation failed")) return false;

    const auto base = reinterpret_cast<std::uintptr_t>(allocation);
    const std::array<std::uint8_t, 4> initial{0x10, 0x20, 0x30, 0x40};
    std::array<std::uint8_t, 4> copied{};
    bool result = Check(
                        services.memory->WriteMemory(base, initial.data(), initial.size()),
                        "Memory facade write failed") &&
        Check(services.memory->ReadMemoryInto(base, copied.data(), copied.size()) &&
                  copied == initial,
              "Memory facade ReadMemoryInto did not preserve bytes");
    const auto owned_read = services.memory->ReadMemory(base, initial.size());
    result = Check(owned_read &&
                       std::ranges::equal(*owned_read, initial),
                   "Memory facade ReadMemory did not preserve bytes") && result;

    const auto pointer_target = base + 512;
    result = Check(
                 services.memory->WriteMemory(
                     base + 128, &pointer_target, sizeof(pointer_target)),
                 "Memory facade pointer fixture write failed") && result;
    constexpr std::ptrdiff_t pointer_offsets[]{8};
    const auto pointer_chain = services.memory->ResolvePointerChain(
        base + 128, pointer_offsets, std::size(pointer_offsets));
    result = Check(pointer_chain && *pointer_chain == pointer_target + 8,
                   "Memory facade pointer-chain resolution changed semantics") && result;

    constexpr std::size_t displacement_offset = 3;
    constexpr std::size_t instruction_size = 7;
    constexpr std::ptrdiff_t addend = 5;
    const auto instruction = base + 256;
    const auto rip_target = base + 1024;
    const auto displacement_value = static_cast<std::intptr_t>(rip_target) -
        static_cast<std::intptr_t>(instruction + instruction_size) - addend;
    if (!Check(
            displacement_value >= (std::numeric_limits<std::int32_t>::min)() &&
                displacement_value <= (std::numeric_limits<std::int32_t>::max)(),
            "RIP fixture displacement is out of range")) {
        static_cast<void>(services.memory->FreeMemory(allocation));
        return false;
    }
    const auto displacement = static_cast<std::int32_t>(displacement_value);
    result = Check(
                 services.memory->WriteMemory(
                     instruction + displacement_offset,
                     &displacement,
                     sizeof(displacement)),
                 "Memory facade RIP fixture write failed") && result;
    const auto resolved_rip = services.memory->ResolveRipRelative(
        instruction, displacement_offset, instruction_size, addend);
    result = Check(resolved_rip && *resolved_rip == rip_target,
                   "Memory facade RIP-relative resolution changed semantics") && result;

    DWORD previous{};
    const bool protected_read_only = services.memory->ProtectMemory(
        base, page_size, PAGE_READONLY, previous);
    result = Check(protected_read_only,
                   "Memory facade protection change failed") && result;
    const std::array<std::uint8_t, 2> patch{0xAA, 0xBB};
    if (protected_read_only) {
        result = Check(
                     !services.memory->WriteMemory(base, patch.data(), patch.size()),
                     "Memory facade wrote through read-only protection") && result;
        result = Check(
                     services.memory->PatchMemory(base, patch.data(), patch.size()),
                     "Memory facade patch did not bridge read-only protection") && result;
        std::array<std::uint8_t, 2> patched{};
        result = Check(
                     services.memory->ReadMemoryInto(base, patched.data(), patched.size()) &&
                         patched == patch,
                     "Memory facade patch did not persist bytes") && result;

        MEMORY_BASIC_INFORMATION information{};
        const bool queried = VirtualQuery(allocation, &information, sizeof(information)) != 0;
        result = Check(queried && (information.Protect & 0xFFU) == PAGE_READONLY,
                       "Memory facade patch did not restore page protection") && result;
        DWORD ignored{};
        result = Check(
                     services.memory->ProtectMemory(base, page_size, previous, ignored),
                     "Memory facade did not restore writable protection") && result;
    }

    return Check(services.memory->FreeMemory(allocation),
                 "Memory facade free failed") && result;
}

}  // namespace

int main() {
    if (!TestBundleOwnershipAndPatternParsing()) return 1;
    if (!TestBundleValidation()) return 2;
    if (!TestCurrentModuleDiscoveryAndScan()) return 3;
    if (!TestAnalyzerUsesInjectedServices()) return 4;
    if (!TestAnalyzerReadLimit()) return 5;
    if (!TestControlledMemoryOperations()) return 6;
    return 0;
}
