#include "anomaly/nte_pak_signature_bypass.hpp"

#include "anomaly/hook_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
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
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;  // 1 = compare, 0 = wildcard
    std::uintptr_t jump_offset{};
};

struct Settings final {
    bool enabled{};
    std::string module;
    std::string pattern;
    std::uintptr_t jump_offset{};
};

// Deliberately tiny JSON reader: the config only holds a handful of flat fields and this runs
// before the regular configuration/JSON services exist.
std::optional<std::string> ReadField(const std::string& text, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_at = text.find(needle);
    if (key_at == std::string::npos) return std::nullopt;
    const std::size_t colon = text.find(':', key_at + needle.size());
    if (colon == std::string::npos) return std::nullopt;
    std::size_t at = colon + 1;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' ||
                                   text[at] == '\n')) {
        ++at;
    }
    if (at >= text.size()) return std::nullopt;
    if (text[at] == '"') {
        const std::size_t end = text.find('"', at + 1);
        if (end == std::string::npos) return std::nullopt;
        return text.substr(at + 1, end - at - 1);
    }
    std::size_t end = at;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n') ++end;
    std::string raw = text.substr(at, end - at);
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\r')) raw.pop_back();
    return raw;
}

bool ParsePattern(const std::string_view text, Pattern& pattern) {
    pattern.bytes.clear();
    pattern.mask.clear();
    const auto hex = [](const char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::size_t index{};
    while (index + 1 < text.size()) {
        while (index < text.size() && text[index] == ' ') ++index;
        if (index + 1 >= text.size()) break;
        if (text[index] == '?' || text[index + 1] == '?') {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(0);
        } else {
            const int high = hex(text[index]);
            const int low = hex(text[index + 1]);
            if (high < 0 || low < 0) return false;
            pattern.bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
            pattern.mask.push_back(1);
        }
        index += 2;
    }
    return pattern.bytes.size() >= 8;
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
    std::ifstream stream(path);
    if (!stream) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    Settings settings{};
    if (const auto enabled = ReadField(text, "enabled")) {
        settings.enabled = *enabled == "true" || *enabled == "1";
    }
    if (const auto module = ReadField(text, "module")) settings.module = *module;
    if (const auto pattern = ReadField(text, "pattern")) settings.pattern = *pattern;
    if (const auto offset = ReadField(text, "jumpOffset")) {
        try {
            settings.jump_offset = static_cast<std::uintptr_t>(std::stoull(*offset));
        } catch (...) {
            settings.jump_offset = 0;
        }
    }
    return settings;
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
        if (installed_ && hooks_.DisableOwner(kOwner, kGeneration)) {
            static_cast<void>(hooks_.RemoveOwner(kOwner, kGeneration));
        }
        installed_ = false;
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

    std::uintptr_t FindPattern() noexcept {
        std::uintptr_t base{};
        std::uintptr_t start{};
        std::uintptr_t end{};
        const HMODULE module = game_module_;
        if (module == nullptr) return 0;
        base = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (std::uint16_t index{}; index < nt->FileHeader.NumberOfSections; ++index) {
            if (std::memcmp(section[index].Name, ".text", 5) != 0) continue;
            start = base + section[index].VirtualAddress;
            end = start + section[index].Misc.VirtualSize;
            break;
        }
        if (end <= start) return 0;

        const std::size_t size = pattern_.bytes.size();
        std::size_t first_fixed{};
        for (; first_fixed < size; ++first_fixed) {
            if (pattern_.mask[first_fixed] != 0) break;
        }
        if (first_fixed == size) return 0;

        std::vector<std::uint8_t> buffer(1u << 20);
        std::uintptr_t cursor = start;
        while (cursor + size < end) {
            const std::size_t want = static_cast<std::size_t>(
                (std::min)(static_cast<std::uintptr_t>(buffer.size()), end - cursor));
            SIZE_T got{};
            if (ReadProcessMemory(
                    GetCurrentProcess(), reinterpret_cast<const void*>(cursor), buffer.data(),
                    want, &got) == 0 ||
                got <= size) {
                cursor += want;
                continue;
            }
            for (std::size_t offset{}; offset + size < got;) {
                const void* found = std::memchr(
                    buffer.data() + offset + first_fixed, pattern_.bytes[first_fixed],
                    got - offset - first_fixed);
                if (found == nullptr) break;
                const std::size_t at = static_cast<std::size_t>(
                    static_cast<const std::uint8_t*>(found) - buffer.data());
                const std::size_t candidate = at - first_fixed;
                bool matched = true;
                for (std::size_t i{}; i < size; ++i) {
                    if (pattern_.mask[i] == 0) continue;
                    if (buffer[candidate + i] != pattern_.bytes[i]) {
                        matched = false;
                        break;
                    }
                }
                if (matched) return cursor + candidate;
                offset = at + 1;
            }
            cursor += got > size ? got - size : got;
        }
        return 0;
    }

    void Run() noexcept {
        try {
            Log("worker started");
            for (int attempt{}; attempt < 2400 && !stop_.load(); ++attempt) {
                const std::uintptr_t match = FindPattern();
                if (match != 0) {
                    std::uintptr_t target{};
                    if (!FollowJump(match + pattern_.jump_offset, target)) {
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
                    installed_ = true;
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
    bool installed_{};
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
