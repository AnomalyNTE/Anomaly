#include "analyzer.hpp"

#include "json.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ue5mem {
namespace {

constexpr std::size_t kMaximumReadBytes = 1024U * 1024U;

std::string Error(std::string_view message) {
    return "{\"ok\":false,\"error\":" + json::Quote(message) + "}";
}

std::string Ok(std::string_view payload = {}) {
    return payload.empty() ? "{\"ok\":true}" : "{\"ok\":true," + std::string(payload) + "}";
}

std::string_view Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::pair<std::string_view, std::string_view> Shift(std::string_view value) {
    value = Trim(value);
    const auto split = value.find_first_of(" \t");
    if (split == std::string_view::npos) return {value, {}};
    return {value.substr(0, split), Trim(value.substr(split + 1))};
}

std::wstring WideUtf8(std::string_view value) {
    if (value.empty() || value == ".") return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), size);
    return output;
}

std::string SectionFlags(DWORD value) {
    std::string flags;
    if ((value & IMAGE_SCN_MEM_READ) != 0) flags += 'r';
    if ((value & IMAGE_SCN_MEM_WRITE) != 0) flags += 'w';
    if ((value & IMAGE_SCN_MEM_EXECUTE) != 0) flags += 'x';
    return flags;
}

std::string TimestampFilename() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[64]{};
    std::snprintf(
        buffer, sizeof(buffer), "snapshot-%04u%02u%02u-%02u%02u%02u.json",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::optional<std::uint64_t> ParseInteger(std::string_view text) {
    const std::string owned(text);
    char* end{};
    const auto value = _strtoui64(owned.c_str(), &end, 0);
    if (end == owned.c_str() || *end != '\0') return std::nullopt;
    return value;
}

std::optional<std::int64_t> ParseSigned(std::string_view text) {
    const std::string owned(text);
    char* end{};
    const auto value = _strtoi64(owned.c_str(), &end, 0);
    if (end == owned.c_str() || *end != '\0') return std::nullopt;
    return value;
}

std::optional<std::vector<std::uint8_t>> ParseHexBytes(std::string_view text) {
    std::vector<std::uint8_t> result;
    while (true) {
        const auto [token, remaining] = Shift(text);
        if (token.empty()) break;
        unsigned value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 0xff) {
            return std::nullopt;
        }
        result.push_back(static_cast<std::uint8_t>(value));
        if (result.size() > 4096) return std::nullopt;
        text = remaining;
    }
    return result.empty() ? std::nullopt : std::optional<std::vector<std::uint8_t>>(std::move(result));
}

std::optional<DWORD> ParseProtection(std::string_view text) {
    if (text == "r") return PAGE_READONLY;
    if (text == "rw") return PAGE_READWRITE;
    if (text == "x") return PAGE_EXECUTE;
    if (text == "rx") return PAGE_EXECUTE_READ;
    if (text == "rwx") return PAGE_EXECUTE_READWRITE;
    const auto numeric = ParseInteger(text);
    return numeric && *numeric <= MAXDWORD ? std::optional<DWORD>(static_cast<DWORD>(*numeric))
                                            : std::nullopt;
}

std::string MutationJson(
    const anomaly::ModuleMemoryService& memory,
    std::uintptr_t address,
    const std::vector<std::uint8_t>& bytes,
    bool patch) {
    const auto before = memory.ReadMemory(address, bytes.size());
    if (!before) return Error("address is unreadable");
    const bool written = patch
        ? memory.PatchMemory(address, bytes.data(), bytes.size())
        : memory.WriteMemory(address, bytes.data(), bytes.size());
    if (!written) return Error(patch ? "patch failed" : "write failed; page is not writable");
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < before->size(); ++index) {
        if (index != 0) encoded << ' ';
        encoded << std::setw(2) << static_cast<unsigned>((*before)[index]);
    }
    return Ok("\"address\":" + json::Hex(address) + ",\"size\":" +
              std::to_string(bytes.size()) + ",\"previous\":" + json::Quote(encoded.str()));
}

std::string ReadJson(
    const anomaly::ModuleMemoryService& memory,
    std::uintptr_t address,
    std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    if (!memory.ReadMemoryInto(address, bytes.data(), bytes.size())) {
        return Error("address is unreadable or crosses a memory region");
    }
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) encoded << ' ';
        encoded << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return Ok("\"address\":" + json::Hex(address) + ",\"bytes\":" + json::Quote(encoded.str()));
}

struct RelativeReference {
    std::uintptr_t instruction{};
    std::string_view kind;
};

std::optional<std::uintptr_t> RelativeTarget(
    std::uintptr_t instruction,
    std::size_t instruction_size,
    const std::uint8_t* displacement) noexcept {
    std::int32_t value{};
    std::memcpy(&value, displacement, sizeof(value));
    const auto base = static_cast<std::uint64_t>(instruction) + instruction_size;
    if (value < 0 && static_cast<std::uint64_t>(-static_cast<std::int64_t>(value)) > base) {
        return std::nullopt;
    }
    const auto target = value < 0
        ? base - static_cast<std::uint64_t>(-static_cast<std::int64_t>(value))
        : base + static_cast<std::uint64_t>(value);
    if (target > (std::numeric_limits<std::uintptr_t>::max)()) return std::nullopt;
    return static_cast<std::uintptr_t>(target);
}

void FindRelativeReferences(
    const std::uint8_t* bytes,
    std::size_t size,
    std::uintptr_t base,
    std::uintptr_t target,
    std::size_t limit,
    std::vector<RelativeReference>& result) {
    const auto add = [&](std::size_t offset, std::size_t instruction_size,
                         std::size_t displacement_offset, std::string_view kind) {
        if (offset > size || instruction_size > size - offset ||
            displacement_offset + sizeof(std::int32_t) > instruction_size) return;
        const auto resolved = RelativeTarget(
            base + offset, instruction_size, bytes + offset + displacement_offset);
        if (resolved && *resolved == target) {
            result.push_back({base + offset, kind});
        }
    };

    for (std::size_t offset = 0; offset + 5 <= size && result.size() < limit; ++offset) {
        const std::uint8_t first = bytes[offset];
        if (first == 0xE8) {
            add(offset, 5, 1, "call-rel32");
            continue;
        }
        if (first == 0xE9) {
            add(offset, 5, 1, "jump-rel32");
            continue;
        }
        if (offset + 6 <= size && first == 0x0F &&
            (bytes[offset + 1] & 0xF0U) == 0x80U) {
            add(offset, 6, 2, "branch-rel32");
            continue;
        }

        std::size_t opcode = offset;
        if ((first & 0xF0U) == 0x40U) ++opcode;
        if (opcode + 6 > size) continue;
        const std::uint8_t operation = bytes[opcode];
        if (operation == 0x0F) {
            if (opcode + 7 > size) continue;
            const std::uint8_t extension = bytes[opcode + 1];
            const std::uint8_t modrm = bytes[opcode + 2];
            if ((modrm & 0xC7U) == 0x05U &&
                (extension == 0xB6 || extension == 0xB7 ||
                 extension == 0xBE || extension == 0xBF)) {
                add(offset, opcode - offset + 7, opcode - offset + 3, "rip-memory");
            }
            continue;
        }
        const std::uint8_t modrm = bytes[opcode + 1];
        if ((modrm & 0xC7U) != 0x05U) continue;
        const std::size_t instruction_size = opcode - offset + 6;
        const std::size_t displacement_offset = opcode - offset + 2;
        switch (operation) {
        case 0x8D: add(offset, instruction_size, displacement_offset, "lea-rip"); break;
        case 0x8B: add(offset, instruction_size, displacement_offset, "mov-load-rip"); break;
        case 0x89: add(offset, instruction_size, displacement_offset, "mov-store-rip"); break;
        case 0x39:
        case 0x3B: add(offset, instruction_size, displacement_offset, "cmp-rip"); break;
        case 0x85: add(offset, instruction_size, displacement_offset, "test-rip"); break;
        case 0xFF:
            if ((modrm & 0x38U) == 0x10U) {
                add(offset, instruction_size, displacement_offset, "call-indirect-rip");
            } else if ((modrm & 0x38U) == 0x20U) {
                add(offset, instruction_size, displacement_offset, "jmp-indirect-rip");
            }
            break;
        default: break;
        }
    }
}

template <typename T>
std::string ValuesJson(
    const anomaly::ModuleMemoryService& memory,
    std::uintptr_t address,
    std::size_t count) {
    if (count == 0 || count > 256) return Error("count must be between 1 and 256");
    const auto bytes = memory.ReadMemory(address, count * sizeof(T));
    if (!bytes) return Error("address is unreadable or crosses a memory region");
    std::string result = "{\"ok\":true,\"address\":" + json::Hex(address) + ",\"values\":[";
    for (std::size_t index = 0; index < count; ++index) {
        T value{};
        std::memcpy(&value, bytes->data() + index * sizeof(T), sizeof(T));
        if (index != 0) result += ',';
        if (std::isfinite(value)) {
            std::ostringstream formatted;
            formatted << std::setprecision(17) << value;
            result += formatted.str();
        } else {
            result += "null";
        }
    }
    result += "]}";
    return result;
}

}  // namespace

Analyzer::Analyzer(
    std::filesystem::path root,
    AnalyzerConfig config,
    RuntimeStatusProvider runtime_status,
    anomaly::CoreMemoryServices memory_services,
    ProfileStatusProvider profile_status,
    Ue5ReflectionQueryProvider ue5_reflection_query)
    : root_(std::move(root)),
      config_(std::move(config)),
      runtime_status_(std::move(runtime_status)),
      memory_services_(anomaly::NormalizeCoreMemoryServices(std::move(memory_services))),
      profile_status_(std::move(profile_status)),
      ue5_reflection_query_(std::move(ue5_reflection_query)) {}

std::string Analyzer::ModulesJson() const {
    const auto modules = memory_services_.memory->EnumerateModules();
    std::string result = "{\"ok\":true,\"modules\":[";
    for (std::size_t index = 0; index < modules.size(); ++index) {
        const auto& module = modules[index];
        if (index != 0) result += ',';
        result += "{\"name\":" + json::Quote(module.name) +
                  ",\"path\":" + json::Quote(module.path) +
                  ",\"base\":" + json::Hex(module.base) +
                  ",\"size\":" + std::to_string(module.size) + "}";
    }
    result += "]}";
    return result;
}

std::string Analyzer::SectionsJson(std::wstring_view module_name) const {
    const auto module = memory_services_.memory->FindModule(module_name);
    if (!module) return Error("module not found");
    const auto sections = memory_services_.memory->EnumerateSections(*module);
    std::string result = "{\"ok\":true,\"module\":" + json::Quote(module->name) + ",\"sections\":[";
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto& section = sections[index];
        if (index != 0) result += ',';
        result += "{\"name\":" + json::Quote(section.name) +
                  ",\"base\":" + json::Hex(section.base) +
                  ",\"size\":" + std::to_string(section.virtual_size) +
                  ",\"flags\":" + json::Quote(SectionFlags(section.characteristics)) + "}";
    }
    result += "]}";
    return result;
}

std::string Analyzer::RegionsJson(std::wstring_view module_name) const {
    const auto module = memory_services_.memory->FindModule(module_name);
    if (!module) return Error("module not found");
    const auto regions = memory_services_.memory->EnumerateRegions(*module);
    std::string result = "{\"ok\":true,\"module\":" + json::Quote(module->name) + ",\"regions\":[";
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& region = regions[index];
        if (index != 0) result += ',';
        result += "{\"base\":" + json::Hex(region.base) +
                   ",\"size\":" + std::to_string(region.size) +
                   ",\"state\":" +
                   json::Quote(memory_services_.memory->StateName(region.state)) +
                   ",\"protection\":" +
                   json::Quote(memory_services_.memory->ProtectionName(region.protection)) +
                   ",\"type\":" +
                   json::Quote(memory_services_.memory->TypeName(region.type)) + "}";
    }
    result += "]}";
    return result;
}

std::string Analyzer::ScanJson(
    std::wstring_view module_name,
    std::string_view section,
    std::string_view pattern_text) const {
    const auto module = memory_services_.memory->FindModule(module_name);
    if (!module) return Error("module not found");
    try {
        const auto pattern = memory_services_.patterns->Parse(pattern_text);
        const auto matches = memory_services_.patterns->ScanSection(
            *module, section, pattern, config_.max_scan_results);
        std::string result = "{\"ok\":true,\"module\":" + json::Quote(module->name) +
                             ",\"section\":" + json::Quote(section) +
                             ",\"count\":" + std::to_string(matches.size()) +
                             ",\"matches\":[";
        for (std::size_t index = 0; index < matches.size(); ++index) {
            if (index != 0) result += ',';
            result += json::Hex(matches[index]);
        }
        result += "]}";
        return result;
    } catch (const std::exception& exception) {
        return Error(exception.what());
    }
}

std::string Analyzer::XrefsJson(
    std::wstring_view module_name,
    std::uintptr_t target) const {
    const auto module = memory_services_.memory->FindModule(module_name);
    if (!module) return Error("module not found");

    constexpr std::size_t chunk_size = 1024 * 1024;
    constexpr std::size_t overlap = 15;
    std::vector<RelativeReference> references;
    const auto sections = memory_services_.memory->EnumerateSections(*module);
    for (const auto& section : sections) {
        if ((section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.virtual_size == 0 || references.size() >= config_.max_scan_results) continue;
        for (std::size_t consumed = 0;
             consumed < section.virtual_size && references.size() < config_.max_scan_results;) {
            const std::size_t remaining = section.virtual_size - consumed;
            const std::size_t requested = (std::min)(remaining, chunk_size + overlap);
            std::vector<std::uint8_t> bytes(requested);
            if (!memory_services_.memory->ReadMemoryInto(
                    section.base + consumed, bytes.data(), bytes.size())) break;
            FindRelativeReferences(
                bytes.data(), bytes.size(), section.base + consumed, target,
                config_.max_scan_results, references);
            if (remaining <= chunk_size) break;
            consumed += chunk_size;
        }
    }
    std::ranges::sort(references, {}, &RelativeReference::instruction);
    references.erase(std::ranges::unique(
        references, {}, &RelativeReference::instruction).begin(), references.end());

    std::string result = "{\"ok\":true,\"module\":" + json::Quote(module->name) +
        ",\"target\":" + json::Hex(target) + ",\"count\":" +
        std::to_string(references.size()) + ",\"references\":[";
    for (std::size_t index = 0; index < references.size(); ++index) {
        if (index != 0) result.push_back(',');
        result += "{\"instruction\":" + json::Hex(references[index].instruction) +
            ",\"rva\":" + std::to_string(references[index].instruction - module->base) +
            ",\"kind\":" + json::Quote(references[index].kind) + "}";
    }
    result += "]}";
    return result;
}

std::string Analyzer::UnrealJson() const {
    if (profile_status_) {
        try {
            return profile_status_();
        } catch (...) {
            return Error("profile diagnostics failed");
        }
    }
    std::string result = "{\"ok\":true,\"symbols\":[";
    for (std::size_t symbol_index = 0; symbol_index < config_.symbols.size(); ++symbol_index) {
        const auto& symbol = config_.symbols[symbol_index];
        if (symbol_index != 0) result += ',';
        result += "{\"name\":" + json::Quote(symbol.name);
        const auto module = memory_services_.memory->FindModule(symbol.module);
        if (!module) {
            result += ",\"found\":false,\"error\":\"module not found\"}";
            continue;
        }
        try {
            const auto pattern = memory_services_.patterns->Parse(symbol.pattern);
            const auto matches = memory_services_.patterns->ScanSection(
                *module, symbol.section, pattern, config_.max_scan_results);
            result += ",\"module\":" + json::Quote(module->name) +
                      ",\"found\":" + (matches.empty() ? std::string("false") : std::string("true")) +
                      ",\"matches\":[";
            for (std::size_t index = 0; index < matches.size(); ++index) {
                if (index != 0) result += ',';
                const auto resolved = symbol.instruction_size == 0
                    ? std::optional<std::uintptr_t>(matches[index] + symbol.addend)
                    : memory_services_.memory->ResolveRipRelative(
                          matches[index], symbol.rip_offset, symbol.instruction_size, symbol.addend);
                result += "{\"instruction\":" + json::Hex(matches[index]);
                if (resolved) result += ",\"address\":" + json::Hex(*resolved);
                result += '}';
            }
            result += "]}";
        } catch (const std::exception& exception) {
            result += ",\"found\":false,\"error\":" + json::Quote(exception.what()) + "}";
        }
    }
    result += "]}";
    return result;
}

std::string Analyzer::BuildSnapshot() const {
    const auto modules = ModulesJson();
    const auto unreal = UnrealJson();
    return "{\"ok\":true,\"pid\":" + std::to_string(GetCurrentProcessId()) +
           ",\"modules\":" + modules.substr(std::string("{\"ok\":true,\"modules\":").size(), modules.size() - std::string("{\"ok\":true,\"modules\":").size() - 1) +
           ",\"unreal\":" + unreal + "}";
}

std::string Analyzer::Execute(std::string_view command_line) const {
    const auto [command, arguments] = Shift(command_line);
    if (command == "ping") return Ok("\"pid\":" + std::to_string(GetCurrentProcessId()));
    if (command == "help") {
        return Ok("\"commands\":[\"ping\",\"status\",\"modules\",\"sections <module|.>\",\"regions <module|.>\",\"scan <module|.> <section> <pattern>\",\"xrefs <module|.> <target>\",\"ue\",\"ue actors <filter|*> [limit] [cursor]\",\"ue functions <filter|*> [limit] [cursor]\",\"read <address> <size<=1048576>\",\"write <address> <hex bytes>\",\"patch <address> <hex bytes>\",\"protect <address> <size> <r|rw|x|rx|rwx>\",\"alloc <size> [protection]\",\"free <address>\",\"chain <base> [offset ...]\",\"rip <instruction> <disp_offset> <instruction_size>\",\"ptr <address>\",\"f32 <address> [count]\",\"f64 <address> [count]\",\"snapshot [filename]\"]");
    }
    if (command == "status") {
        std::string runtime = "null";
        if (runtime_status_) {
            try {
                runtime = runtime_status_();
            } catch (...) {
                runtime = "{\"error\":\"runtime diagnostics failed\"}";
            }
        }
        return Ok("\"pid\":" + std::to_string(GetCurrentProcessId()) +
                  ",\"configured_symbols\":" + std::to_string(config_.symbols.size()) +
                  ",\"profile_game\":" + json::Quote(config_.game_id) +
                  ",\"platform_enabled\":" + (config_.platform_enabled ? std::string("true") : std::string("false")) +
                   ",\"platform_embedded\":" + (config_.platform_embedded ? std::string("true") : std::string("false")) +
                   ",\"root\":" + json::Quote(root_.wstring()) +
                   ",\"runtime\":" + runtime);
    }
    if (command == "modules") return ModulesJson();
    if (command == "sections") return SectionsJson(WideUtf8(arguments));
    if (command == "regions") return RegionsJson(WideUtf8(arguments));
    if (command == "ue") {
        if (arguments.empty()) return UnrealJson();
        if (!ue5_reflection_query_) return Error("UE reflection queries are unavailable");
        try {
            return ue5_reflection_query_(arguments);
        } catch (...) {
            return Error("UE reflection query failed");
        }
    }
    if (command == "read") {
        const auto [address_text, size_text] = Shift(arguments);
        const auto address = ParseInteger(address_text);
        const auto size = ParseInteger(size_text);
        if (!address || !size || *size > kMaximumReadBytes) {
            return Error("usage: read <address> <size<=1048576>");
        }
        return ReadJson(
            *memory_services_.memory,
            static_cast<std::uintptr_t>(*address),
            static_cast<std::size_t>(*size));
    }
    if (command == "write" || command == "patch") {
        const auto [address_text, bytes_text] = Shift(arguments);
        const auto address = ParseInteger(address_text);
        const auto bytes = ParseHexBytes(bytes_text);
        if (!address || !bytes) return Error("usage: write|patch <address> <hex bytes>");
        return MutationJson(
            *memory_services_.memory,
            static_cast<std::uintptr_t>(*address),
            *bytes,
            command == "patch");
    }
    if (command == "protect") {
        const auto [address_text, after_address] = Shift(arguments);
        const auto [size_text, protection_text] = Shift(after_address);
        const auto address = ParseInteger(address_text);
        const auto size = ParseInteger(size_text);
        const auto protection = ParseProtection(protection_text);
        if (!address || !size || !protection) {
            return Error("usage: protect <address> <size> <r|rw|x|rx|rwx>");
        }
        DWORD previous{};
        if (!memory_services_.memory->ProtectMemory(
                static_cast<std::uintptr_t>(*address), static_cast<std::size_t>(*size),
                *protection, previous)) {
            return Error("VirtualProtect failed");
        }
        return Ok("\"address\":" + json::Hex(static_cast<std::uintptr_t>(*address)) +
                  ",\"previous\":" + std::to_string(previous) +
                  ",\"protection\":" + std::to_string(*protection));
    }
    if (command == "alloc") {
        const auto [size_text, protection_text] = Shift(arguments);
        const auto size = ParseInteger(size_text);
        const auto protection = protection_text.empty()
            ? std::optional<DWORD>(PAGE_READWRITE)
            : ParseProtection(protection_text);
        if (!size || !protection || *size == 0 || *size > 256 * 1024 * 1024ULL) {
            return Error("usage: alloc <size<=268435456> [r|rw|x|rx|rwx]");
        }
        void* allocation = memory_services_.memory->AllocateMemory(
            static_cast<std::size_t>(*size), *protection);
        if (allocation == nullptr) return Error("VirtualAlloc failed");
        return Ok("\"address\":" + json::Hex(reinterpret_cast<std::uintptr_t>(allocation)) +
                  ",\"size\":" + std::to_string(*size));
    }
    if (command == "free") {
        const auto address = ParseInteger(arguments);
        if (!address) return Error("usage: free <allocation_address>");
        return memory_services_.memory->FreeMemory(
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(*address)))
            ? Ok()
            : Error("VirtualFree failed");
    }
    if (command == "chain") {
        auto [base_text, remaining] = Shift(arguments);
        const auto base = ParseInteger(base_text);
        if (!base) return Error("usage: chain <base> [offset ...]");
        std::vector<std::ptrdiff_t> offsets;
        while (!remaining.empty()) {
            const auto [offset_text, next] = Shift(remaining);
            const auto offset = ParseSigned(offset_text);
            if (!offset || offsets.size() == 64) return Error("invalid pointer-chain offset");
            offsets.push_back(static_cast<std::ptrdiff_t>(*offset));
            remaining = next;
        }
        const auto result = memory_services_.memory->ResolvePointerChain(
            static_cast<std::uintptr_t>(*base), offsets.data(), offsets.size());
        return result ? Ok("\"address\":" + json::Hex(*result))
                      : Error("pointer chain could not be resolved");
    }
    if (command == "ptr") {
        const auto address = ParseInteger(arguments);
        if (!address) return Error("usage: ptr <address>");
        const auto bytes = memory_services_.memory->ReadMemory(
            static_cast<std::uintptr_t>(*address), sizeof(std::uintptr_t));
        if (!bytes) return Error("address is unreadable");
        std::uintptr_t pointer{};
        std::memcpy(&pointer, bytes->data(), sizeof(pointer));
        return Ok("\"address\":" + json::Hex(static_cast<std::uintptr_t>(*address)) +
                  ",\"pointer\":" + json::Hex(pointer));
    }
    if (command == "rip") {
        const auto [instruction_text, after_instruction] = Shift(arguments);
        const auto [offset_text, size_text] = Shift(after_instruction);
        const auto instruction = ParseInteger(instruction_text);
        const auto offset = ParseInteger(offset_text);
        const auto size = ParseInteger(size_text);
        if (!instruction || !offset || !size) return Error("usage: rip <instruction> <disp_offset> <instruction_size>");
        const auto target = memory_services_.memory->ResolveRipRelative(
            static_cast<std::uintptr_t>(*instruction), static_cast<std::size_t>(*offset),
            static_cast<std::size_t>(*size), 0);
        if (!target) return Error("failed to read RIP displacement");
        std::string payload = "\"instruction\":" + json::Hex(static_cast<std::uintptr_t>(*instruction)) +
                              ",\"target\":" + json::Hex(*target);
        const auto pointer_bytes = memory_services_.memory->ReadMemory(
            *target, sizeof(std::uintptr_t));
        if (pointer_bytes) {
            std::uintptr_t pointer{};
            std::memcpy(&pointer, pointer_bytes->data(), sizeof(pointer));
            payload += ",\"pointer\":" + json::Hex(pointer);
        }
        return Ok(payload);
    }
    if (command == "f32" || command == "f64") {
        const auto [address_text, count_text] = Shift(arguments);
        const auto address = ParseInteger(address_text);
        const auto count = count_text.empty() ? std::optional<std::uint64_t>(3) : ParseInteger(count_text);
        if (!address || !count) return Error("usage: f32|f64 <address> [count]");
        return command == "f32"
            ? ValuesJson<float>(
                  *memory_services_.memory,
                  static_cast<std::uintptr_t>(*address),
                  static_cast<std::size_t>(*count))
            : ValuesJson<double>(
                  *memory_services_.memory,
                  static_cast<std::uintptr_t>(*address),
                  static_cast<std::size_t>(*count));
    }
    if (command == "scan") {
        const auto [module, after_module] = Shift(arguments);
        const auto [section, pattern] = Shift(after_module);
        if (module.empty() || section.empty() || pattern.empty()) {
            return Error("usage: scan <module|.> <section> <pattern>");
        }
        return ScanJson(WideUtf8(module), section, pattern);
    }
    if (command == "xrefs") {
        const auto [module, target_text] = Shift(arguments);
        const auto target = ParseInteger(target_text);
        if (module.empty() || !target ||
            *target > (std::numeric_limits<std::uintptr_t>::max)()) {
            return Error("usage: xrefs <module|.> <target>");
        }
        return XrefsJson(WideUtf8(module), static_cast<std::uintptr_t>(*target));
    }
    if (command == "snapshot") {
        auto filename = arguments.empty() ? TimestampFilename() : std::string(arguments);
        filename = std::filesystem::path(filename).filename().string();
        if (filename.empty()) return Error("snapshot filename is empty");
        const auto path = root_ / filename;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return Error("failed to create snapshot");
        output << BuildSnapshot() << '\n';
        output.close();
        return Ok("\"path\":" + json::Quote(path.wstring()));
    }
    return Error("unknown command; use help");
}

}  // namespace ue5mem
