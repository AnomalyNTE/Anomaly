#include "anomaly/nte_pak_signature_bypass.hpp"

#include "anomaly/hook_manager.hpp"
#include "pattern.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace anomaly {
namespace {

constexpr std::string_view kOwner = "anomaly.nte.pak-signature";
constexpr std::uint64_t kGeneration = 1;
constexpr std::string_view kLabel = "nte-pak-signature";
constexpr std::string_view kConfigName = "pak-signature-bypass.json";
constexpr std::string_view kLogName = "pak-signature-bypass.log";

struct Pattern final {
    ue5mem::Pattern signature;
    // Legacy B0 01 C3 means a bool predicate at match + jumpOffset, installed as a hook.
    bool direct_predicate{};
    std::uintptr_t jump_offset{};
};

struct Settings final {
    bool enabled{};
    std::string module;
    std::string pattern;
    std::uintptr_t jump_offset{};
    std::string patch_bytes;
};

bool ParsePattern(const std::string_view text, Pattern& pattern) {
    try {
        pattern.signature = ue5mem::Pattern::Parse(text);
        const auto bytes = pattern.signature.Bytes();
        return bytes.size() >= 8 && bytes.size() <= 4096 &&
            std::any_of(bytes.begin(), bytes.end(), [](const auto& byte) { return byte.mask != 0; });
    } catch (...) {
        return false;
    }
}

void WriteDefaultConfig(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream(path) << "{\n"
                           "  \"schemaVersion\": 1,\n"
                           "  \"enabled\": false,\n"
                           "  \"verifiedFor\": \"1.4.18\",\n"
                           "  \"module\": \"HTGame.exe\",\n"
                           "  \"pattern\": \"\",\n"
                           "  \"jumpOffset\": 71,\n"
                           "  \"comment\": \"Drops the pak signature requirement so unsigned pak mods "
                           "mount. Disabled by default; enabling it modifies game code at runtime "
                           "and may conflict with the game's own anti-tamper checks.\"\n"
                           "}\n";
}

std::optional<Settings> LoadSettings(const std::filesystem::path& path) {
    try {
        std::ifstream stream(path);
        if (!stream) return std::nullopt;
        const auto json = nlohmann::json::parse(stream);
        if (!json.is_object() || json.value("schemaVersion", 1) != 1) return std::nullopt;
        Settings settings{};
        settings.enabled = json.value("enabled", false);
        settings.module = json.value("module", std::string{});
        settings.pattern = json.value("pattern", std::string{});
        settings.patch_bytes = json.value("patchBytes", std::string{});
        if (const auto offset = json.find("jumpOffset"); offset != json.end()) {
            if (!offset->is_number_integer() || offset->get<std::int64_t>() < 0 ||
                offset->get<std::uint64_t>() > 4096) return std::nullopt;
            settings.jump_offset = offset->get<std::uintptr_t>();
        }
        return settings;
    } catch (...) {
        return std::nullopt;
    }
}

bool IsTruePredicate(const std::string& text) {
    try {
        auto normalized = text;
        std::replace(normalized.begin(), normalized.end(), ',', ' ');
        const auto pattern = ue5mem::Pattern::Parse(normalized);
        const auto bytes = pattern.Bytes();
        constexpr std::uint8_t predicate[]{0xB0, 0x01, 0xC3};
        if (bytes.size() != std::size(predicate)) return false;
        for (std::size_t i{}; i < bytes.size(); ++i)
            if (bytes[i].mask != 0xFF || bytes[i].value != predicate[i]) return false;
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadBytes(const std::uintptr_t address, void* buffer, const std::size_t size) noexcept {
    SIZE_T got{};
    return ReadProcessMemory(
               GetCurrentProcess(), reinterpret_cast<const void*>(address), buffer, size,
               &got) != 0 &&
        got == size;
}

bool FollowJump(const std::uintptr_t address, std::uintptr_t& target) noexcept {
    std::uint8_t opcode{};
    if (!ReadBytes(address, &opcode, 1)) return false;
    if (opcode != 0xE9 && opcode != 0xE8) return false;
    std::int32_t displacement{};
    if (!ReadBytes(address + 1, &displacement, sizeof(displacement))) return false;
    target = address + 5 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(displacement));
    return target != 0;
}

}  // namespace

struct NtePakSignatureBypass::Impl final {
public:
    Impl() : hooks_(CreateMinHookBackend()) {}

    ~Impl() { Stop(); }

    void Start(const std::filesystem::path& runtime_root, HMODULE game_module,
        const std::filesystem::path& log_directory) {
        if (worker_.joinable()) return;
        config_path_ = runtime_root / L"config" / kConfigName;
        log_path_ = log_directory / kLogName;
        if (!std::filesystem::exists(config_path_)) WriteDefaultConfig(config_path_);
        const auto settings = LoadSettings(config_path_);
        if (!settings) {
            Log("config unreadable, bypass disabled");
            return;
        }
        if (!settings->enabled) {
            Log("disabled by config");
            return;
        }
        if (!ParsePattern(settings->pattern, pattern_)) {
            Log("config has no usable pattern, bypass disabled");
            return;
        }
        pattern_.jump_offset = settings->jump_offset;
        pattern_.direct_predicate = !settings->patch_bytes.empty();
        if (pattern_.direct_predicate && !IsTruePredicate(settings->patch_bytes)) {
            Log("unsupported patchBytes; only the B0 01 C3 bool predicate is supported");
            return;
        }
        if (!settings->module.empty()) {
            wchar_t module_path[MAX_PATH]{};
            if (GetModuleFileNameW(game_module, module_path, MAX_PATH) == 0 ||
                _stricmp(std::filesystem::path(module_path).filename().string().c_str(),
                    settings->module.c_str()) != 0) {
                Log("configured module does not match the supplied game module");
                return;
            }
        }
        game_module_ = game_module;
        stop_.store(false);
        worker_ = std::thread(&Impl::Run, this);
    }

    void Stop() noexcept {
        stop_.store(true);
        if (worker_.joinable()) {
            try {
                worker_.join();
            } catch (...) {
            }
        }
        // Also remove a created-but-never-enabled hook after EnableOwner failed.
        if (original_ != nullptr && hooks_.DisableOwner(kOwner, kGeneration)) {
            if (hooks_.RemoveOwner(kOwner, kGeneration)) original_ = nullptr;
        }
        patched_.store(false);
    }

    [[nodiscard]] bool Patched() const noexcept { return patched_.load(); }

private:
    // Reports "signature valid" without touching the game's own logic.
    static bool __fastcall Detour(void*) noexcept { return true; }

    void Log(const std::string& line) noexcept {
        try {
            std::error_code error;
            std::filesystem::create_directories(log_path_.parent_path(), error);
            std::ofstream(log_path_, std::ios::app) << line << '\n';
        } catch (...) {
        }
    }

    std::uintptr_t FindPattern() {
        const auto base = reinterpret_cast<std::uintptr_t>(game_module_);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS nt{};
        if (!ReadBytes(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || !ReadBytes(base + dos.e_lfanew, &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return 0;
        const auto section_table = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
            nt.FileHeader.SizeOfOptionalHeader;
        std::uintptr_t start{}, end{};
        for (std::uint16_t index{}; index < nt.FileHeader.NumberOfSections; ++index) {
            IMAGE_SECTION_HEADER section{};
            if (!ReadBytes(section_table + index * sizeof(section), &section, sizeof(section))) return 0;
            if (std::memcmp(section.Name, ".text", 5) != 0) continue;
            if (section.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
                section.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage - section.VirtualAddress)
                return 0;
            start = base + section.VirtualAddress;
            end = start + section.Misc.VirtualSize;
            break;
        }
        if (end <= start) return 0;
        const auto size = pattern_.signature.Size();
        std::vector<std::uint8_t> buffer(1u << 20);
        std::uintptr_t cursor = start, match{};
        while (!stop_.load() && end - cursor >= size) {
            const auto want = (std::min)(buffer.size(), static_cast<std::size_t>(end - cursor));
            // A partial image cannot establish uniqueness. Retry after the packed image is ready.
            if (!ReadBytes(cursor, buffer.data(), want)) return 0;
            for (const auto offset : pattern_.signature.FindAll(std::span(buffer.data(), want), 2)) {
                if (match != 0) return 0;
                match = cursor + offset;
            }
            cursor += want - size + 1;
        }
        if (stop_.load() || match == 0 || pattern_.jump_offset >= end - match) return 0;
        return match;
    }

    void Run() noexcept {
        try {
            Log("worker started");
            for (int attempt{}; attempt < 2400 && !stop_.load(); ++attempt) {
                const std::uintptr_t match = FindPattern();
                if (match != 0) {
                    std::uintptr_t target = match + pattern_.jump_offset;
                    if (!pattern_.direct_predicate && !FollowJump(target, target)) {
                        Log("pattern found but the jump could not be followed");
                        return;
                    }
                    if (!hooks_.Create(
                            std::string(kOwner), kGeneration, std::string(kLabel),
                            reinterpret_cast<void*>(target),
                            reinterpret_cast<void*>(&Impl::Detour),
                            reinterpret_cast<void**>(&original_))) {
                        Log("hook creation failed");
                        return;
                    }
                    if (!hooks_.EnableOwner(kOwner, kGeneration)) {
                        Log("hook enable failed");
                        return;
                    }
                    patched_.store(true);
                    char line[256]{};
                    std::snprintf(
                        line, sizeof(line), "hooked target=0x%llX (match=0x%llX attempt=%d)",
                        static_cast<unsigned long long>(target),
                        static_cast<unsigned long long>(match), attempt);
                    Log(line);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Log("pattern not found within the wait window");
        } catch (...) {
            Log("exception while applying the bypass");
        }
    }

    HookManager hooks_;
    void* original_{};
    Pattern pattern_{};
    std::filesystem::path config_path_;
    std::filesystem::path log_path_;
    HMODULE game_module_{};
    std::thread worker_;
    std::atomic_bool stop_{};
    std::atomic_bool patched_{};
};

NtePakSignatureBypass::NtePakSignatureBypass() : impl_(std::make_unique<Impl>()) {}

NtePakSignatureBypass::~NtePakSignatureBypass() = default;

void NtePakSignatureBypass::Start(const std::filesystem::path& runtime_root, HMODULE game_module,
    const std::filesystem::path& log_directory) {
    impl_->Start(runtime_root, game_module, log_directory);
}

void NtePakSignatureBypass::Stop() { impl_->Stop(); }

bool NtePakSignatureBypass::Patched() const noexcept { return impl_->Patched(); }

}  // namespace anomaly
