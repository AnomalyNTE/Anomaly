#include "anomaly/nte_profile_runtime.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::atomic<HANDLE> tick_target_entered{};
std::atomic<HANDLE> tick_target_release{};

extern "C" __declspec(noinline) void __fastcall ProfileRuntimeRaceTickTarget(
    void*, float, bool) {
    const HANDLE entered = tick_target_entered.load(std::memory_order_acquire);
    if (entered != nullptr) SetEvent(entered);
    const HANDLE release = tick_target_release.load(std::memory_order_acquire);
    if (release != nullptr) static_cast<void>(WaitForSingleObject(release, INFINITE));
}

using TickTarget = void(__fastcall*)(void*, float, bool);

#pragma optimize("", off)
extern "C" __declspec(noinline) void __fastcall ProfileRuntimeProcessEventTarget(
    void* object, void* function, void* parameters) {
    volatile std::uintptr_t value = reinterpret_cast<std::uintptr_t>(object) ^
        reinterpret_cast<std::uintptr_t>(function) ^
        reinterpret_cast<std::uintptr_t>(parameters);
#define ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(base) \
    value += base + 0U; value ^= base + 1U; value += base + 2U; value ^= base + 3U; \
    value += base + 4U; value ^= base + 5U; value += base + 6U; value ^= base + 7U; \
    value += base + 8U; value ^= base + 9U; value += base + 10U; value ^= base + 11U; \
    value += base + 12U; value ^= base + 13U; value += base + 14U; value ^= base + 15U
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(0U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(16U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(32U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(48U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(64U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(80U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(96U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(112U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(128U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(144U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(160U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(176U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(192U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(208U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(224U);
    ANOMALY_PROCESS_EVENT_FIXTURE_STEPS(240U);
#undef ANOMALY_PROCESS_EVENT_FIXTURE_STEPS
    if (value == 0xA19D3E57U) OutputDebugStringA("ProcessEvent fixture");
}
#pragma optimize("", on)

constexpr auto kProcessEventPrologue = std::to_array<std::uint8_t>({
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x00,
    0x01, 0x00, 0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30,
    0x48, 0x89, 0x9D, 0x28, 0x01, 0x00, 0x00});
constexpr auto kProcessEventArgumentSetup = std::to_array<std::uint8_t>({
    0x48, 0x33, 0xC5, 0x48, 0x89, 0x85, 0xC0, 0x00,
    0x00, 0x00, 0x8B, 0x41, 0x08, 0x4D, 0x8B, 0xF0,
    0xC1, 0xE8, 0x1E, 0x48, 0x8B, 0xFA, 0xF6, 0xD0,
    0x4C, 0x8B, 0xF9, 0xA8, 0x01});
constexpr auto kProcessEventOutParmSetup = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x4F, 0x50, 0x48, 0x8D, 0x95, 0x90,
    0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x56,
    0x90, 0x48, 0x8B, 0x41, 0x38, 0x84, 0xC0, 0x79,
    0x4D, 0x48, 0x0F, 0xBA, 0xE0, 0x08, 0x73, 0x3D,
    0x8B, 0x04, 0x24, 0x48, 0x83, 0xEC, 0x30, 0x4C,
    0x8D, 0x44, 0x24, 0x30, 0x41, 0x8B, 0x00, 0x48,
    0x63, 0x41, 0x44, 0x49, 0x83, 0xC0, 0x0F, 0x49,
    0x83, 0xE0, 0xF0, 0x49, 0x03, 0xC6, 0x49, 0x89,
    0x40, 0x08, 0x49, 0x89, 0x08, 0x48, 0x8B, 0x02,
    0x48, 0x85, 0xC0, 0x74, 0x0D, 0x4C, 0x89, 0x40,
    0x10, 0x48, 0x8B, 0x12, 0x48, 0x83, 0xC2, 0x10,
    0xEB, 0x03, 0x4C, 0x89, 0x02, 0x48, 0x8B, 0x49,
    0x18, 0x48, 0x85, 0xC9, 0x75, 0xAB, 0x48, 0x8B,
    0x02, 0x48, 0x85, 0xC0, 0x74, 0x04, 0x48, 0x89,
    0x70, 0x10, 0x4D, 0x85, 0xE4, 0x75, 0x50});

struct ProcessEventCodeFixture final {
    decltype(kProcessEventPrologue) prologue;
    std::array<std::uint8_t, 0x26U - kProcessEventPrologue.size()> gap_before_arguments;
    decltype(kProcessEventArgumentSetup) arguments;
    std::array<
        std::uint8_t,
        0x20FU - 0x26U - kProcessEventArgumentSetup.size()> gap_before_out_parameters;
    decltype(kProcessEventOutParmSetup) out_parameters;
    std::array<
        std::uint8_t,
        0x300U - 0x20FU - kProcessEventOutParmSetup.size()> tail;
};
static_assert(offsetof(ProcessEventCodeFixture, arguments) == 0x26U);
static_assert(offsetof(ProcessEventCodeFixture, out_parameters) == 0x20FU);

const ProcessEventCodeFixture g_process_event_code{
    kProcessEventPrologue, {}, kProcessEventArgumentSetup, {},
    kProcessEventOutParmSetup, {}};

constexpr std::array<std::uint8_t, 16> kObjectRegistryMarker{
    0xA1, 0x31, 0xC7, 0x42, 0x56, 0xE8, 0x99, 0x0B,
    0xD2, 0x6D, 0x17, 0xF4, 0x83, 0xAC, 0x5E, 0x70};
constexpr std::array<std::uint8_t, 16> kNamePoolMarker{
    0xB4, 0x28, 0x6C, 0xD1, 0x9A, 0x05, 0xE7, 0x53,
    0x1F, 0x88, 0x32, 0xCA, 0x74, 0xBD, 0x60, 0x0E};

std::array<std::uintptr_t, 1> g_object_chunks{};
struct alignas(std::uintptr_t) MarkedObjectRegistry final {
    std::array<std::uint8_t, 16> marker;
    std::uintptr_t items;
    std::uint32_t count;
    std::uint32_t max_count;
    std::uint32_t max_chunks;
    std::uint32_t num_chunks;
};
MarkedObjectRegistry g_object_registry{
    kObjectRegistryMarker,
    reinterpret_cast<std::uintptr_t>(g_object_chunks.data()),
    0,
    1,
    1,
    0};
struct alignas(std::uintptr_t) MarkedNamePool final {
    std::array<std::uint8_t, 16> marker;
    std::uintptr_t payload;
};
MarkedNamePool g_name_pool{kNamePoolMarker, 1};

class ScopedCodePatch final {
public:
    bool Apply(void* target, const void* replacement, const std::size_t size) {
        if (target == nullptr || replacement == nullptr ||
            size == 0 || size > original_.size()) {
            return false;
        }
        DWORD old_protection{};
        if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) {
            return false;
        }
        target_ = target;
        size_ = size;
        protection_ = old_protection;
        active_ = true;
        std::memcpy(original_.data(), target, size);
        std::memcpy(target, replacement, size);
        static_cast<void>(FlushInstructionCache(GetCurrentProcess(), target, size));
        DWORD ignored{};
        if (!VirtualProtect(target, size, old_protection, &ignored)) {
            return false;
        }
        return true;
    }

    bool Restore() noexcept {
        if (!active_) return true;
        DWORD old_protection{};
        if (!VirtualProtect(target_, size_, PAGE_EXECUTE_READWRITE, &old_protection)) {
            return false;
        }
        std::memcpy(target_, original_.data(), size_);
        static_cast<void>(FlushInstructionCache(GetCurrentProcess(), target_, size_));
        DWORD ignored{};
        const bool restored =
            VirtualProtect(target_, size_, protection_, &ignored) != FALSE;
        if (restored) active_ = false;
        return restored;
    }

    ~ScopedCodePatch() { static_cast<void>(Restore()); }

private:
    std::array<std::uint8_t, sizeof(ProcessEventCodeFixture)> original_{};
    void* target_{};
    std::size_t size_{};
    DWORD protection_{};
    bool active_{};
};

void* ResolveLinkedFunction(void* entry) {
    auto address = reinterpret_cast<std::uintptr_t>(entry);
    for (std::size_t depth{}; depth < 4; ++depth) {
        const auto* const code = reinterpret_cast<const std::uint8_t*>(address);
        if (code[0] == 0xE9U) {
            std::int32_t displacement{};
            std::memcpy(&displacement, code + 1, sizeof(displacement));
            address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(address + 5U) + displacement);
            continue;
        }
        if (code[0] == 0xFFU && code[1] == 0x25U) {
            std::int32_t displacement{};
            std::memcpy(&displacement, code + 2, sizeof(displacement));
            const auto slot = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(address + 6U) + displacement);
            std::memcpy(&address, reinterpret_cast<const void*>(slot), sizeof(address));
            continue;
        }
        break;
    }
    return reinterpret_cast<void*>(address);
}

std::string BytesPattern(const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::ostringstream pattern;
    pattern << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        if (index != 0) pattern << ' ';
        pattern << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return pattern.str();
}

std::string FunctionPattern(const void* function) {
    constexpr std::size_t kPatternBytes = 64;
    return BytesPattern(function, kPatternBytes);
}

std::string Narrow(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) result.push_back(static_cast<char>(character));
    return result;
}

std::string MissingHookProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{)"
         << R"("ue5.GameTick":{"module":")" << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":"DE AD BE EF 01 23 45 67 89 AB CD EF 10 32 54 76","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]},)"
         << R"("ue5.GObjects":{"module":")" << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":"FE ED FA CE 76 54 32 10 EF CD AB 89 67 45 23 01","resolve":{"kind":"direct"},"validators":["readable","object-registry-v1"],"requiredBy":["anomaly.ue5.objects"]}},)"
         << R"("features":{"ue5.framework":["ue5.GameTick"],"ue5.objects":["ue5.GObjects","ue5.GameTick"],"nte.player":["ue5.GameTick"]}})";
    return json.str();
}

std::string MissingSectionProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".late","pattern":"DE AD BE EF 01 23 45 67","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]}},"features":{"ue5.framework":["ue5.GameTick"]}})";
    return json.str();
}

std::string RaceHookProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")"
         << FunctionPattern(reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget))
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]}},"features":{"ue5.framework":["ue5.GameTick"],"nte.player":["ue5.GameTick"]}})";
    return json.str();
}

void ResetProfileLayers(const std::filesystem::path& root);

std::string AhudHookProfile(
    const anomaly::BuildFingerprint& fingerprint,
    const bool valid_ahud_layout) {
    const std::string module = Narrow(fingerprint.module);
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << module << R"(","section":".text","pattern":")"
         << FunctionPattern(reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget))
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]},"ue5.GObjects":{"module":")"
         << module << R"(","section":".data","pattern":")"
         << BytesPattern(g_object_registry.marker.data(), g_object_registry.marker.size())
         << R"(","resolve":{"kind":"direct","addend":16},"validators":["readable","object-registry-v1"],"requiredBy":["anomaly.ue5.functions"]},"ue5.FNamePool":{"module":")"
         << module << R"(","section":".data","pattern":")"
         << BytesPattern(g_name_pool.marker.data(), g_name_pool.marker.size())
         << R"(","resolve":{"kind":"direct","addend":16},"validators":["readable","name-pool-v1"],"requiredBy":["anomaly.ue5.functions"]},"ue5.ProcessEvent":{"module":")"
         << module << R"(","section":".text","pattern":")"
         << BytesPattern(&g_process_event_code, 64)
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","executable"],"requiredBy":["anomaly.ue5.ahud"]}},"features":{"ue5.framework":["ue5.GameTick"],"ue5.objects":["ue5.GObjects","ue5.GameTick"],"ue5.names":["ue5.FNamePool"],"ue5.functions":["ue5.GObjects","ue5.GameTick","ue5.FNamePool"],"ue5.process-event":["ue5.ProcessEvent"],"ue5.ahud":["ue5.ProcessEvent"]},"optionalFeatures":["ue5.objects","ue5.names","ue5.functions","ue5.process-event","ue5.ahud"],"featureLayoutValidators":{"ue5.functions":["ue5-functions-reflection-v1"],"ue5.process-event":["ue5-process-event-abi-v1"],"ue5.ahud":["ue5-ahud-reflection-v1"]},"featureDependencies":{"ue5.functions":["ue5.objects","ue5.names"],"ue5.ahud":["ue5.functions","ue5.process-event"]},"layout":{"object.class":16,"object.nameOffset":24,"object.outer":32,"ufunction.numParms":180,"ufunction.parmsSize":182,"ufunction.returnValueOffset":184,"names.blocksOffset":16,"names.blockBits":16,"names.entryStride":2,"names.headerLengthShift":6,"objects.itemsOffset":0,"objects.maxCountOffset":12,"objects.countOffset":8,"objects.maxChunksOffset":16,"objects.numChunksOffset":20,"objects.chunkCountSize":4,"objects.chunkSize":65536,"objects.itemStride":24,"objects.objectOffset":0,"objects.serialOffset":16,"ustruct.propertyLink":112,"ffield.class":8,"ffield.name":32,"ffieldClass.name":)"
         << (valid_ahud_layout ? 0 : 4097)
         << R"(,"fproperty.arrayDim":48,"fproperty.elementSize":52,"fproperty.offsetInternal":68,"fproperty.propertyLinkNext":72,"fstructProperty.struct":112,"fboolProperty.fieldSize":112,"fboolProperty.byteOffset":113,"fboolProperty.byteMask":114,"fboolProperty.fieldMask":115}})";
    return json.str();
}

bool TestAhudHookLifecycle(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    ResetProfileLayers(root);

    void* const process_event_target = ResolveLinkedFunction(
        reinterpret_cast<void*>(&ProfileRuntimeProcessEventTarget));
    const auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    DWORD64 unwind_image_base{};
    const auto* const unwind_entry = RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(process_event_target), &unwind_image_base, nullptr);
    const auto process_event_rva =
        reinterpret_cast<std::uintptr_t>(process_event_target) - module_base;
    if (unwind_entry == nullptr || unwind_image_base != module_base ||
        unwind_entry->BeginAddress != process_event_rva ||
        unwind_entry->EndAddress - unwind_entry->BeginAddress <
            sizeof(g_process_event_code)) {
        std::cerr << "ProcessEvent fixture rva=" << process_event_rva
                  << " unwindBase=" << unwind_image_base
                  << " moduleBase=" << module_base;
        if (unwind_entry != nullptr) {
            std::cerr << " begin=" << unwind_entry->BeginAddress
                      << " end=" << unwind_entry->EndAddress
                      << " size=" << unwind_entry->EndAddress - unwind_entry->BeginAddress;
        }
        std::cerr << " required=" << sizeof(g_process_event_code) << '\n';
        ResetProfileLayers(root);
        return Check(false, "ProcessEvent fixture has no sufficiently large unwind entry");
    }
    ScopedCodePatch process_event_patch;
    if (!process_event_patch.Apply(
            process_event_target, &g_process_event_code,
            sizeof(g_process_event_code))) {
        ResetProfileLayers(root);
        return Check(false, "ProcessEvent fixture code patch failed");
    }
    bool result = true;
    std::ofstream(root / L"profiles" / L"nte" / L"ahud-hook.json")
        << AhudHookProfile(fingerprint, false);
    {
        anomaly::NteProfileRuntimeOptions options;
        options.runtime_root = root;
        options.game_module = GetModuleHandleW(nullptr);
        anomaly::NteProfileRuntime runtime(std::move(options));
        const bool started = runtime.Start();
        if (started) {
            ProfileRuntimeRaceTickTarget(nullptr, 1.0F / 60.0F, false);
        }
        const auto resolution = runtime.Resolution();
        const auto evidence = runtime.Evidence();
        const auto diagnostics = runtime.DiagnosticsJson();
        const auto hooks = runtime.Hooks();
        const bool tick_hook = std::ranges::any_of(hooks, [](const auto& hook) {
            return hook.owner == "anomaly.ue5.framework" &&
                hook.label == "game-tick" && hook.enabled;
        });
        const bool process_event_hook = std::ranges::any_of(hooks, [](const auto& hook) {
            return hook.owner == "anomaly.ue5.ahud" &&
                hook.label == "process-event" && hook.enabled;
        });
        const bool armed_before_reflection =
            started && resolution &&
                resolution->FeatureAvailable("ue5.process-event") &&
                !resolution->FeatureAvailable("ue5.ahud") &&
                evidence.tick_hook_ready && evidence.ahud_hook_ready &&
                diagnostics.find("\"tickHookReady\":true") != std::string::npos &&
                diagnostics.find("\"ahudHookReady\":true") != std::string::npos &&
                diagnostics.find(
                    "optional AHUD service pending: reflection gate not ready") !=
                    std::string::npos &&
                hooks.size() == 2 && tick_hook && process_event_hook &&
                anomaly::ProcessAdapterServices().Query(
                    ANOMALY_UE5_AHUD_SERVICE_V1_ID,
                    ANOMALY_UE5_AHUD_SERVICE_V1_VERSION,
                    false) == nullptr;
        if (!armed_before_reflection) {
            std::cerr << "pre-reflection AHUD runtime diagnostics: "
                      << diagnostics << '\n';
            for (const auto& hook : hooks) {
                std::cerr << "hook owner=" << hook.owner << " label=" << hook.label
                          << " enabled=" << hook.enabled << '\n';
            }
        }
        result = Check(
                     armed_before_reflection,
                     "ProcessEvent hook waited for the full AHUD reflection gate") &&
            result;
        result = Check(
                     runtime.Stop(),
                     "pre-reflection AHUD hook runtime did not stop") &&
            result;
        result = Check(
                     runtime.Hooks().empty() &&
                         anomaly::ProcessAdapterServices().Snapshot().empty(),
                     "pre-reflection AHUD hook or service survived Runtime stop") &&
            result;
    }

    ResetProfileLayers(root);
    std::ofstream(root / L"profiles" / L"nte" / L"ahud-hook.json")
        << AhudHookProfile(fingerprint, true);
    {
        anomaly::NteProfileRuntimeOptions options;
        options.runtime_root = root;
        options.game_module = GetModuleHandleW(nullptr);
        anomaly::NteProfileRuntime runtime(std::move(options));
        const bool started = runtime.Start();
        if (started) {
            ProfileRuntimeRaceTickTarget(nullptr, 1.0F / 60.0F, false);
        }
        const auto resolution = runtime.Resolution();
        const auto evidence = runtime.Evidence();
        const auto diagnostics = runtime.DiagnosticsJson();
        const auto hooks = runtime.Hooks();
        const bool tick_hook = std::ranges::any_of(hooks, [](const auto& hook) {
            return hook.owner == "anomaly.ue5.framework" &&
                hook.label == "game-tick" && hook.enabled;
        });
        const bool process_event_hook = std::ranges::any_of(hooks, [](const auto& hook) {
            return hook.owner == "anomaly.ue5.ahud" &&
                hook.label == "process-event" && hook.enabled;
        });
        const bool activated =
            started && resolution && resolution->FeatureAvailable("ue5.ahud") &&
                evidence.tick_hook_ready && evidence.ahud_hook_ready &&
                diagnostics.find("\"tickHookReady\":true") != std::string::npos &&
                diagnostics.find("\"ahudHookReady\":true") != std::string::npos &&
                hooks.size() == 2 && tick_hook && process_event_hook &&
                anomaly::ProcessAdapterServices().Query(
                    ANOMALY_UE5_AHUD_SERVICE_V1_ID,
                    ANOMALY_UE5_AHUD_SERVICE_V1_VERSION,
                    false) != nullptr;
        if (!activated) {
            std::cerr << "AHUD runtime diagnostics: " << diagnostics << '\n';
            for (const auto& hook : hooks) {
                std::cerr << "hook owner=" << hook.owner << " label=" << hook.label
                          << " enabled=" << hook.enabled << '\n';
            }
        }
        result = Check(
            activated,
            "AHUD feature did not activate the Runtime-owned ProcessEvent hook");
        result = Check(runtime.Stop(), "AHUD hook runtime did not stop") && result;
        result = Check(
            runtime.Hooks().empty() &&
                anomaly::ProcessAdapterServices().Snapshot().empty(),
            "AHUD ProcessEvent hook or service survived Runtime stop") && result;
    }
    result = Check(
        process_event_patch.Restore(),
        "ProcessEvent fixture code restoration failed") && result;
    ResetProfileLayers(root);
    return result;
}

std::string HistoricalProfile(
    const anomaly::BuildFingerprint& fingerprint,
    const bool valid) {
    const std::string pattern = valid
        ? FunctionPattern(reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget))
        : "DE AD BE EF 01 23 45 67 89 AB CD EF 10 32 54 76";
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]}},"features":{"ue5.framework":["ue5.GameTick"]}})";
    return json.str();
}

std::string DeferredHistoricalProfile(
    const anomaly::BuildFingerprint& fingerprint) {
    const std::string pattern =
        FunctionPattern(reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget));
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]},"ue5.GObjects":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["readable","object-registry-v1"],"requiredBy":["anomaly.ue5.objects"]}},"features":{"ue5.framework":["ue5.GameTick"],"ue5.objects":["ue5.GObjects","ue5.GameTick"]}})";
    return json.str();
}

void ResetProfileLayers(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root / L"profiles", error);
    std::filesystem::remove_all(root / L"profiles-local", error);
    std::filesystem::remove_all(root / L"state", error);
    std::filesystem::remove_all(root / L"cache", error);
    std::filesystem::create_directories(root / L"profiles" / L"nte", error);
}

bool TestSectionReadinessTimeout(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    ResetProfileLayers(root);
    std::ofstream(root / L"profiles" / L"nte" / L"late-section.json")
        << MissingSectionProfile(fingerprint);

    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    options.section_readiness_timeout = std::chrono::milliseconds(20);
    options.section_readiness_poll_interval = std::chrono::milliseconds(5);
    anomaly::NteProfileRuntime runtime(std::move(options));
    const bool started = runtime.Start();
    const auto resolution = runtime.Resolution();
    const auto diagnostics = runtime.DiagnosticsJson();
    const bool result = Check(
        started && resolution &&
            resolution->state == anomaly::ProfileResolutionState::Degraded &&
            diagnostics.find("profile section readiness timeout") != std::string::npos &&
            diagnostics.find(".late") != std::string::npos,
        "profile section readiness wait was not bounded or diagnosed");
    const bool stopped = !started || runtime.Stop();
    ResetProfileLayers(root);
    return result && Check(stopped, "timed-out profile runtime did not stop");
}

bool TestBuildProfileRefresh(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    std::error_code error;
    bool result = true;
    const auto start = [&](const char* message) {
        anomaly::NteProfileRuntimeOptions options;
        options.runtime_root = root;
        options.game_module = GetModuleHandleW(nullptr);
        auto runtime = std::make_unique<anomaly::NteProfileRuntime>(std::move(options));
        result = Check(runtime->Start(), message) && result;
        return runtime;
    };

    ResetProfileLayers(root);
    const auto unrelated_cache = root / L"cache" / L"profiles" / L"old-build" / L"old.json";
    std::filesystem::create_directories(unrelated_cache.parent_path(), error);
    std::ofstream(unrelated_cache) << "unrelated";
    const auto single = root / L"profiles" / L"nte" / L"single.json";
    const std::string recipe = HistoricalProfile(fingerprint, true);
    std::ofstream(single) << recipe;
    auto refreshed = start("build-independent profile binding did not start");
    const auto single_evidence = refreshed->Evidence();
    result = Check(
                 refreshed->Adapter() != nullptr && single_evidence.profile &&
                     single_evidence.profile->source == single &&
                     refreshed->DiagnosticsJson().find("profile recipe selected") !=
                         std::string::npos &&
                     ReadFile(single) == recipe &&
                      ReadFile(unrelated_cache) == "unrelated" &&
                     std::filesystem::is_regular_file(
                         root / L"state" / L"profile-symbol-cache.json"),
                 "profile recipe was not selected without a build gate") && result;
    result = Check(refreshed->Stop(), "bound profile runtime did not stop") && result;
    auto rebound_again = start("second build-independent profile binding did not start");
    result = Check(
                 rebound_again->Adapter() != nullptr &&
                      rebound_again->DiagnosticsJson().find("profile recipe selected") !=
                          std::string::npos && rebound_again->Resolution() &&
                      rebound_again->Resolution()->cache_loaded,
                 "profile recipe unexpectedly became gated after restart") && result;
    result = Check(rebound_again->Stop(), "second bound runtime did not stop") && result;

    ResetProfileLayers(root);
    const auto valid_with_malformed = root / L"profiles" / L"nte" / L"valid-with-malformed.json";
    const auto malformed = root / L"profiles" / L"nte" / L"malformed.json";
    std::ofstream(valid_with_malformed) << HistoricalProfile(fingerprint, true);
    std::ofstream(malformed) << "{ invalid profile";
    auto malformed_ignored = start("malformed profile isolation did not start");
    result = Check(
                 malformed_ignored->Adapter() != nullptr &&
                     anomaly::LoadBuildProfile(valid_with_malformed).Ok() &&
                     std::filesystem::exists(malformed),
                 "malformed profile was mutated during startup") && result;
    result = Check(
                 malformed_ignored->Stop(),
                 "malformed profile runtime did not stop") && result;

    ResetProfileLayers(root);
    const auto invalid = root / L"profiles" / L"nte" / L"invalid.json";
    std::ofstream(invalid) << HistoricalProfile(fingerprint, false);
    auto degraded = start("unresolved signature profile did not start degraded");
    result = Check(
                 degraded->Adapter() != nullptr && degraded->Resolution() &&
                     degraded->Resolution()->state == anomaly::ProfileResolutionState::Degraded &&
                     std::filesystem::exists(invalid) &&
                     anomaly::ProcessAdapterServices().Query(
                         ANOMALY_NTE_BUILD_SERVICE_V1_ID,
                         ANOMALY_NTE_BUILD_SERVICE_V1_VERSION,
                         false) != nullptr &&
                     anomaly::ProcessAdapterServices().Query(
                         ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID,
                         ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION,
                         false) == nullptr,
                 "signature failure disabled build discovery instead of its feature") && result;
    result = Check(degraded->Stop(), "degraded profile runtime did not stop") && result;

    ResetProfileLayers(root);
    const auto deferred = root / L"profiles" / L"nte" / L"deferred.json";
    std::ofstream(deferred) << DeferredHistoricalProfile(fingerprint);
    auto deferred_runtime = start("deferred validator profile did not start");
    result = Check(
                 deferred_runtime->Adapter() != nullptr && deferred_runtime->Resolution() &&
                     deferred_runtime->Resolution()->FeatureAvailable("ue5.framework") &&
                     !deferred_runtime->Resolution()->FeatureAvailable("ue5.objects") &&
                     std::filesystem::exists(deferred),
                 "runtime validator failure blocked an otherwise stable signature recipe") && result;
    result = Check(deferred_runtime->Stop(), "deferred profile runtime did not stop") && result;

    ResetProfileLayers(root);
    const auto first_valid = root / L"profiles" / L"nte" / L"first.json";
    const auto second_valid = root / L"profiles" / L"nte" / L"second.json";
    std::ofstream(first_valid) << HistoricalProfile(fingerprint, true);
    std::ofstream(second_valid) << HistoricalProfile(fingerprint, true);
    auto ordered = start("ordered profile selection did not start");
    const auto ordered_evidence = ordered->Evidence();
    result = Check(
                 ordered->Adapter() != nullptr && ordered_evidence.profile &&
                     ordered_evidence.profile->source == first_valid &&
                     std::filesystem::exists(first_valid) &&
                     std::filesystem::exists(second_valid),
                 "ordered recipes competed as build-validation candidates") && result;
    result = Check(ordered->Stop(), "ordered profile runtime did not stop") && result;

    ResetProfileLayers(root);
    const auto bundled = root / L"profiles" / L"nte" / L"bundled.json";
    const auto local = root / L"profiles-local" / L"nte" / L"local.json";
    std::filesystem::create_directories(local.parent_path(), error);
    std::ofstream(bundled) << HistoricalProfile(fingerprint, true);
    std::ofstream(local) << HistoricalProfile(fingerprint, false);
    auto prioritized = start("profile layer priority did not start");
    const auto prioritized_evidence = prioritized->Evidence();
    result = Check(
                 prioritized->Adapter() != nullptr && prioritized_evidence.profile &&
                     prioritized_evidence.profile->source == local &&
                     prioritized_evidence.resolution &&
                     prioritized_evidence.resolution->state ==
                         anomaly::ProfileResolutionState::Degraded &&
                     std::filesystem::exists(local) && std::filesystem::exists(bundled),
                 "runtime bypassed configured Profile layer priority") && result;
    result = Check(prioritized->Stop(), "prioritized profile runtime did not stop") && result;

    anomaly::NteProfileRuntimeOptions bundled_only_options;
    bundled_only_options.runtime_root = root;
    bundled_only_options.game_module = GetModuleHandleW(nullptr);
    bundled_only_options.profile_overrides_enabled = false;
    auto bundled_only = std::make_unique<anomaly::NteProfileRuntime>(
        std::move(bundled_only_options));
    result = Check(
                 bundled_only->Start() && bundled_only->Evidence().profile &&
                     bundled_only->Evidence().profile->source == bundled &&
                     bundled_only->DiagnosticsJson().find("overrides suspended") !=
                         std::string::npos,
                 "Runtime recovery policy did not restrict selection to bundled Profiles") &&
        result;
    result = Check(bundled_only->Stop(), "bundled-only profile runtime did not stop") && result;

    ResetProfileLayers(root);
    return result;
}

std::string FeatureDiagnosticsProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")"
         << FunctionPattern(reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget))
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]}},"features":{"ue5.framework":["ue5.GameTick"],"nte.player":["ue5.GameTick"],"nte.player-esp":["ue5.GameTick"]},"featureLayoutValidators":{"nte.player":["fixture-layout-v1"]},"featureDependencies":{"nte.player-esp":["nte.player"]}})";
    return json.str();
}

std::string OutgoingTransformRejectedProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    const std::string pattern = FunctionPattern(
        reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget));
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]},"ue5.PacketHandler.OutboundDispatchPreHandler":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","executable"],"requiredBy":["ue5.network.outgoing-transform-metadata"]},)"
         << R"("ue5.PacketHandler.OutgoingTransform":{"module":")"
         << Narrow(fingerprint.module)
         << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","executable"],"requiredBy":["ue5.network.outgoing-transform-metadata"]}},)"
         << R"("features":{"ue5.framework":["ue5.GameTick"],"ue5.network.outgoing-transform-metadata":["ue5.PacketHandler.OutboundDispatchPreHandler","ue5.PacketHandler.OutgoingTransform"]},"optionalFeatures":["ue5.network.outgoing-transform-metadata"],"featureLayoutValidators":{"ue5.network.outgoing-transform-metadata":["ue5-outgoing-transform-abi-v1"]}})";
    return json.str();
}

std::string ProcessEventRejectedProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    const std::string pattern = FunctionPattern(
        reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget));
    const std::string module = Narrow(fingerprint.module);
    json << R"({"schemaVersion":1,"game":"nte","symbols":{"ue5.GameTick":{"module":")"
         << module << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","tick-anchor-v1"],"requiredBy":["anomaly.ue5.framework"]},"ue5.ProcessEvent":{"module":")"
         << module << R"(","section":".text","pattern":")" << pattern
         << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","executable"],"requiredBy":["anomaly.ue5.framework"]}},"features":{"ue5.framework":["ue5.GameTick"],"ue5.process-event":["ue5.ProcessEvent"]},"optionalFeatures":["ue5.process-event"],"featureLayoutValidators":{"ue5.process-event":["ue5-process-event-abi-v1"]}})";
    return json.str();
}

std::string EscMenuHooksRejectedProfile(const anomaly::BuildFingerprint& fingerprint) {
    std::ostringstream json;
    const std::string pattern = FunctionPattern(
        reinterpret_cast<const void*>(&ProfileRuntimeRaceTickTarget));
    const std::string module = Narrow(fingerprint.module);
    const auto symbol = [&](const std::string_view id, const std::string_view required_by) {
        json << '"' << id << R"(":{"module":")" << module
             << R"(","section":".text","pattern":")" << pattern
             << R"(","resolve":{"kind":"direct"},"validators":["address-in-module","executable"],"requiredBy":[")"
             << required_by << R"("]})";
    };
    json << R"({"schemaVersion":1,"game":"nte","symbols":{)";
    symbol("ue5.GameTick", "anomaly.ue5.framework");
    json << ',';
    symbol("ue5.GObjects", "anomaly.nte.esc-menu-button");
    json << ',';
    symbol("nte.HTUI_MenuExtension.AddMenuPage", "anomaly.nte.esc-menu-button");
    json << ',';
    symbol("nte.HTUI_MenuExtension.execAddMenuPage", "anomaly.nte.esc-menu-button");
    json << ',';
    symbol("nte.CommonButtonBase.HandleButtonClicked", "anomaly.nte.esc-menu-button");
    json << ',';
    symbol("nte.CommonButtonBase.BP_OnClicked", "anomaly.nte.esc-menu-button");
    json << R"(},"features":{"ue5.framework":["ue5.GameTick"],"nte.esc-menu-button":["ue5.GObjects","nte.HTUI_MenuExtension.AddMenuPage","nte.HTUI_MenuExtension.execAddMenuPage","nte.CommonButtonBase.HandleButtonClicked","nte.CommonButtonBase.BP_OnClicked"]},"optionalFeatures":["nte.esc-menu-button"],"featureLayoutValidators":{"nte.esc-menu-button":["nte-esc-menu-hooks-v1"]}})";
    return json.str();
}

const anomaly::FeatureResolution* FindResolvedFeature(
    const anomaly::ProfileResolutionSnapshot& resolution,
    std::string_view id) {
    const auto found = resolution.features.find(id);
    return found == resolution.features.end() ? nullptr : &found->second;
}

bool TestFeatureDiagnosticsPreserveParsedProfileEvidence(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    const auto profile = root / L"profiles" / L"nte" / L"feature-diagnostics.json";
    std::ofstream(profile) << FeatureDiagnosticsProfile(fingerprint);

    const auto parsed = anomaly::LoadBuildProfile(profile);
    bool result = Check(
        parsed.Ok() && parsed.profile &&
            parsed.profile->feature_layout_validators.at("nte.player") ==
                std::vector<std::string>{"fixture-layout-v1"} &&
            parsed.profile->feature_dependencies.at("nte.player-esp") ==
                std::vector<std::string>{"nte.player"},
        "feature diagnostics fixture did not parse profile gate metadata");

    if (result) {
        anomaly::NteProfileRuntimeOptions options;
        options.runtime_root = root;
        options.game_module = GetModuleHandleW(nullptr);
        anomaly::NteProfileRuntime runtime(std::move(options));
        const bool started = runtime.Start();
        result = Check(started, "feature diagnostics profile runtime did not start") && result;
        if (started) {
            const auto resolution = runtime.Resolution();
            const auto diagnostics = runtime.DiagnosticsJson();
            const auto* player = resolution
                ? FindResolvedFeature(*resolution, "nte.player") : nullptr;
            const auto* player_esp = resolution
                ? FindResolvedFeature(*resolution, "nte.player-esp") : nullptr;
            result = Check(
                player != nullptr && !player->available &&
                    player->validation_diagnostics == std::vector<std::string>{
                        "fixture-layout-v1: validator is not registered"} &&
                    player_esp != nullptr && !player_esp->available &&
                    player_esp->unavailable_dependencies ==
                        std::vector<std::string>{"nte.player"},
                "parsed profile did not produce feature validation/dependency diagnostics") && result;
            result = Check(
                diagnostics.find(
                    "\"featureLayoutValidators\":{\"nte.player\":[\"fixture-layout-v1\"]}") !=
                        std::string::npos &&
                    diagnostics.find(
                    "\"featureDependencies\":{\"nte.player-esp\":[\"nte.player\"]}") !=
                        std::string::npos &&
                    diagnostics.find(
                    "\"id\":\"nte.player\",\"available\":false,\"missingSymbols\":[],"
                    "\"unavailableDependencies\":[],\"validationDiagnostics\":["
                    "\"fixture-layout-v1: validator is not registered\"]}") != std::string::npos &&
                    diagnostics.find(
                    "\"id\":\"nte.player-esp\",\"available\":false,\"missingSymbols\":[],"
                    "\"unavailableDependencies\":[\"nte.player\"],\"validationDiagnostics\":[]}") !=
                        std::string::npos,
                "profile diagnostics dropped parsed gate metadata or feature failure details") && result;
        }
        static_cast<void>(runtime.Stop());
    }

    std::error_code error;
    std::filesystem::remove(profile, error);
    return result;
}

bool TestOutgoingTransformFeatureRejectsUnverifiedAbi(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    const auto profile = root / L"profiles" / L"nte" / L"outgoing-transform-rejected.json";
    std::ofstream(profile) << OutgoingTransformRejectedProfile(fingerprint);

    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    anomaly::NteProfileRuntime runtime(std::move(options));
    bool result = Check(runtime.Start(), "outgoing transform rejection profile did not start");
    if (result) {
        const auto resolution = runtime.Resolution();
        const auto diagnostics = runtime.DiagnosticsJson();
        const auto* const feature = resolution
            ? FindResolvedFeature(*resolution, "ue5.network.outgoing-transform-metadata")
            : nullptr;
        bool validator_rejected{};
        if (feature != nullptr) {
            for (const auto& diagnostic : feature->validation_diagnostics) {
                if (diagnostic.find("ue5-outgoing-transform-abi-v1:") == 0U) {
                    validator_rejected = true;
                    break;
                }
            }
        }
        result = Check(
            resolution && resolution->state == anomaly::ProfileResolutionState::Ready &&
                feature != nullptr && !feature->available && validator_rejected &&
                diagnostics.find("\"outgoingTransformMetadataProbe\":{\"started\":false}") !=
                    std::string::npos &&
                diagnostics.find("optional outgoing transform metadata capability unavailable: exact ABI gate failed") !=
                    std::string::npos,
            "unverified outgoing transform ABI activated a runtime probe") && result;
    }
    static_cast<void>(runtime.Stop());
    std::error_code error;
    std::filesystem::remove(profile, error);
    return result;
}

bool TestProcessEventFeatureRejectsUnverifiedAbi(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    const auto profile = root / L"profiles" / L"nte" / L"process-event-rejected.json";
    std::ofstream(profile) << ProcessEventRejectedProfile(fingerprint);

    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    anomaly::NteProfileRuntime runtime(std::move(options));
    bool result = Check(runtime.Start(), "ProcessEvent rejection profile did not start");
    if (result) {
        const auto resolution = runtime.Resolution();
        const auto* const feature = resolution
            ? FindResolvedFeature(*resolution, "ue5.process-event") : nullptr;
        const bool validator_rejected = feature != nullptr &&
            std::ranges::any_of(
                feature->validation_diagnostics, [](const std::string& diagnostic) {
                    return diagnostic.starts_with("ue5-process-event-abi-v1:");
                });
        result = Check(
            resolution && resolution->state == anomaly::ProfileResolutionState::Ready &&
                feature != nullptr && !feature->available && validator_rejected,
            "unverified ProcessEvent ABI activated the framework capability") && result;
    }
    static_cast<void>(runtime.Stop());
    std::error_code error;
    std::filesystem::remove(profile, error);
    return result;
}

bool TestEscMenuFeatureRejectsUnverifiedHooks(
    const std::filesystem::path& root,
    const anomaly::BuildFingerprint& fingerprint) {
    const auto profile = root / L"profiles" / L"nte" / L"esc-menu-hooks-rejected.json";
    std::ofstream(profile) << EscMenuHooksRejectedProfile(fingerprint);

    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    anomaly::NteProfileRuntime runtime(std::move(options));
    bool result = Check(runtime.Start(), "ESC menu hook rejection profile did not start");
    if (result) {
        const auto resolution = runtime.Resolution();
        const auto* const feature = resolution
            ? FindResolvedFeature(*resolution, "nte.esc-menu-button") : nullptr;
        const bool validator_rejected = feature != nullptr &&
            std::ranges::any_of(
                feature->validation_diagnostics, [](const std::string& diagnostic) {
                    return diagnostic.starts_with("nte-esc-menu-hooks-v1:");
                });
        result = Check(
            resolution && resolution->state == anomaly::ProfileResolutionState::Ready &&
                feature != nullptr && !feature->available && validator_rejected,
            "unverified ESC menu hook topology activated the optional feature") && result;
    }
    static_cast<void>(runtime.Stop());
    std::error_code error;
    std::filesystem::remove(profile, error);
    return result;
}

bool WaitUntilStopped(anomaly::NteProfileRuntime& runtime) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (runtime.Started() && std::chrono::steady_clock::now() < deadline) {
        Sleep(1);
    }
    return !runtime.Started();
}

bool RunLateDetourObserverRace(const std::filesystem::path& root) {
    std::atomic_uint32_t observer_calls{};
    std::atomic_uint32_t missing_services{};
    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    options.tick_evidence_observer = [&](std::uint32_t, double) {
        observer_calls.fetch_add(1, std::memory_order_relaxed);
        if (anomaly::ProcessAdapterServices().Query(
                ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1, false) == nullptr) {
            missing_services.fetch_add(1, std::memory_order_relaxed);
        }
    };
    anomaly::NteProfileRuntime runtime(std::move(options));
    if (!runtime.Start() || runtime.Adapter() == nullptr ||
        runtime.DiagnosticsJson().find("\"tickHookReady\":true") == std::string::npos) {
        std::cerr << "race profile did not activate the game tick hook: "
                  << runtime.DiagnosticsJson() << '\n';
        return false;
    }

    const HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (entered == nullptr || release == nullptr) {
        if (entered != nullptr) CloseHandle(entered);
        if (release != nullptr) CloseHandle(release);
        static_cast<void>(runtime.Stop());
        return false;
    }
    tick_target_entered.store(entered, std::memory_order_release);
    tick_target_release.store(release, std::memory_order_release);
    std::thread tick([] {
        const auto target = reinterpret_cast<TickTarget>(&ProfileRuntimeRaceTickTarget);
        target(nullptr, 1.0F / 60.0F, false);
    });
    if (WaitForSingleObject(entered, 2000) != WAIT_OBJECT_0) {
        SetEvent(release);
        tick.join();
        tick_target_entered.store(nullptr, std::memory_order_release);
        tick_target_release.store(nullptr, std::memory_order_release);
        CloseHandle(entered);
        CloseHandle(release);
        static_cast<void>(runtime.Stop());
        return false;
    }

    std::atomic_bool stop_result{};
    std::thread stop([&] {
        stop_result.store(runtime.Stop(std::chrono::seconds(2)), std::memory_order_release);
    });
    const bool stop_started = WaitUntilStopped(runtime);
    SetEvent(release);
    tick.join();
    stop.join();
    tick_target_entered.store(nullptr, std::memory_order_release);
    tick_target_release.store(nullptr, std::memory_order_release);
    CloseHandle(entered);
    CloseHandle(release);
    return stop_started && stop_result.load(std::memory_order_acquire) &&
        observer_calls.load(std::memory_order_acquire) == 0 &&
        missing_services.load(std::memory_order_acquire) == 0 &&
        anomaly::ProcessAdapterServices().Snapshot().empty();
}

bool RunInFlightObserverDrainRace(const std::filesystem::path& root) {
    const HANDLE observer_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE observer_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE target_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE target_release = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (observer_entered == nullptr || observer_release == nullptr ||
        target_entered == nullptr || target_release == nullptr) {
        if (observer_entered != nullptr) CloseHandle(observer_entered);
        if (observer_release != nullptr) CloseHandle(observer_release);
        if (target_entered != nullptr) CloseHandle(target_entered);
        if (target_release != nullptr) CloseHandle(target_release);
        return false;
    }

    std::atomic_uint32_t missing_services{};
    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    options.tick_evidence_observer = [&](std::uint32_t, double) {
        SetEvent(observer_entered);
        static_cast<void>(WaitForSingleObject(observer_release, INFINITE));
        if (anomaly::ProcessAdapterServices().Query(
                ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1, false) == nullptr) {
            missing_services.fetch_add(1, std::memory_order_relaxed);
        }
    };
    anomaly::NteProfileRuntime runtime(std::move(options));
    if (!runtime.Start() || runtime.Adapter() == nullptr ||
        runtime.DiagnosticsJson().find("\"tickHookReady\":true") == std::string::npos) {
        CloseHandle(observer_entered);
        CloseHandle(observer_release);
        CloseHandle(target_entered);
        CloseHandle(target_release);
        return false;
    }

    tick_target_entered.store(target_entered, std::memory_order_release);
    tick_target_release.store(target_release, std::memory_order_release);
    std::thread tick([] {
        const auto target = reinterpret_cast<TickTarget>(&ProfileRuntimeRaceTickTarget);
        target(nullptr, 1.0F / 60.0F, false);
    });
    if (WaitForSingleObject(observer_entered, 2000) != WAIT_OBJECT_0) {
        SetEvent(observer_release);
        tick.join();
        tick_target_entered.store(nullptr, std::memory_order_release);
        tick_target_release.store(nullptr, std::memory_order_release);
        static_cast<void>(runtime.Stop());
        CloseHandle(observer_entered);
        CloseHandle(observer_release);
        CloseHandle(target_entered);
        CloseHandle(target_release);
        return false;
    }

    std::atomic_bool stop_finished{};
    std::atomic_bool stop_result{};
    std::thread stop([&] {
        stop_result.store(runtime.Stop(std::chrono::seconds(2)), std::memory_order_release);
        stop_finished.store(true, std::memory_order_release);
    });
    const bool stop_started = WaitUntilStopped(runtime);
    Sleep(20);
    const bool waited_for_observer = !stop_finished.load(std::memory_order_acquire);
    const bool services_preserved = anomaly::ProcessAdapterServices().Query(
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1, false) != nullptr;
    SetEvent(observer_release);
    tick.join();
    stop.join();
    tick_target_entered.store(nullptr, std::memory_order_release);
    tick_target_release.store(nullptr, std::memory_order_release);
    CloseHandle(observer_entered);
    CloseHandle(observer_release);
    CloseHandle(target_entered);
    CloseHandle(target_release);
    return stop_started && waited_for_observer && services_preserved &&
        stop_result.load(std::memory_order_acquire) &&
        missing_services.load(std::memory_order_acquire) == 0 &&
        anomaly::ProcessAdapterServices().Snapshot().empty();
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-profile-runtime-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / L"profiles", error);
    anomaly::NteProfileRuntimeOptions options;
    options.runtime_root = root;
    options.game_module = GetModuleHandleW(nullptr);
    anomaly::NteProfileRuntime runtime(std::move(options));
    if (!runtime.Start() || !runtime.Started()) {
        std::cerr << "no-profile runtime failed to start\n";
        return 1;
    }
    const auto fingerprint = runtime.Fingerprint();
    std::array<wchar_t, 32768> module_path{};
    const DWORD module_path_size = GetModuleFileNameW(
        GetModuleHandleW(nullptr), module_path.data(), static_cast<DWORD>(module_path.size()));
    anomaly::BuildFingerprint fixture_identity;
    fixture_identity.game = "nte";
    fixture_identity.module = module_path_size == 0
        ? std::wstring{L"nte_profile_runtime_tests.exe"}
        : std::filesystem::path(
              std::wstring_view(module_path.data(), module_path_size)).filename().wstring();
    const auto resolution = runtime.Resolution();
    const auto diagnostics = runtime.DiagnosticsJson();
    const bool build_discovery_published =
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_UE5_BUILD_SERVICE_V1_ID,
            ANOMALY_UE5_BUILD_SERVICE_V1_VERSION,
            false) != nullptr &&
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID,
            ANOMALY_NTE_BUILD_SERVICE_V1_VERSION,
            false) != nullptr;
    if (fingerprint || !resolution ||
        resolution->state != anomaly::ProfileResolutionState::NoProfile ||
        runtime.Adapter() == nullptr || !build_discovery_published ||
        diagnostics.find("no-profile") == std::string::npos ||
        diagnostics.find("\"fingerprintAvailable\":false") == std::string::npos ||
        diagnostics.find("\"adapterStarted\":true") == std::string::npos ||
        diagnostics.find("\"tickHookReady\":false") == std::string::npos ||
        diagnostics.find("\"ahudHookReady\":false") == std::string::npos ||
        diagnostics.find("\"ntePlayerPublished\":false") == std::string::npos ||
        diagnostics.find("\"outgoingTransformMetadataProbe\":{\"started\":false}") ==
            std::string::npos ||
        diagnostics.find("no active profile recipe") == std::string::npos ||
        anomaly::ProcessAdapterServices().Snapshot().size() != 2) {
        std::cerr << "missing profile did not preserve build discovery\n";
        return 2;
    }
    runtime.Stop();
    if (runtime.Started() || !anomaly::ProcessAdapterServices().Snapshot().empty()) {
        std::cerr << "profile runtime did not stop\n";
        return 3;
    }

    if (!TestBuildProfileRefresh(root, fixture_identity)) {
        return 11;
    }
    if (!TestSectionReadinessTimeout(root, fixture_identity)) {
        return 12;
    }
    if (!TestAhudHookLifecycle(root, fixture_identity)) {
        return 13;
    }

    std::filesystem::create_directories(root / L"profiles" / L"nte", error);
    const auto race_profile = root / L"profiles" / L"nte" / L"race-hook.json";
    std::ofstream(race_profile) << RaceHookProfile(fixture_identity);
    if (!RunLateDetourObserverRace(root)) {
        std::cerr << "late detour invoked the tick evidence observer during stop\n";
        return 4;
    }
    if (!RunInFlightObserverDrainRace(root)) {
        std::cerr << "in-flight tick observer raced adapter service revocation\n";
        return 5;
    }
    std::filesystem::remove(race_profile, error);

    if (!TestFeatureDiagnosticsPreserveParsedProfileEvidence(root, fixture_identity)) {
        std::cerr << "profile diagnostics dropped parsed feature gate evidence\n";
        return 6;
    }
    if (!TestOutgoingTransformFeatureRejectsUnverifiedAbi(root, fixture_identity)) {
        std::cerr << "unverified outgoing transform profile escaped the ABI gate\n";
        return 10;
    }
    if (!TestEscMenuFeatureRejectsUnverifiedHooks(root, fixture_identity)) {
        std::cerr << "unverified ESC menu hook topology escaped the ABI gate\n";
        return 11;
    }
    if (!TestProcessEventFeatureRejectsUnverifiedAbi(root, fixture_identity)) {
        std::cerr << "unverified ProcessEvent escaped the framework ABI gate\n";
        return 12;
    }

    std::filesystem::remove(root / L"state" / L"profile-symbol-cache.json", error);
    std::ofstream(root / L"profiles" / L"nte" / L"missing-hook.json")
        << MissingHookProfile(fixture_identity);
    anomaly::NteProfileRuntimeOptions degraded_options;
    degraded_options.runtime_root = root;
    degraded_options.game_module = GetModuleHandleW(nullptr);
    degraded_options.snapshot_sampling = {2, 3};
    anomaly::NteProfileRuntime degraded(std::move(degraded_options));
    const bool degraded_started = degraded.Start();
    const auto unavailable_reflection = degraded_started
        ? degraded.ExecuteReflectionQuery("actors *") : std::string{};
    if (!degraded_started || !degraded.Started() || degraded.Adapter() == nullptr ||
        degraded.Resolution() == nullptr ||
        degraded.Resolution()->state != anomaly::ProfileResolutionState::Degraded ||
        anomaly::ProcessAdapterServices().Snapshot().size() != 2 ||
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_UE5_BUILD_SERVICE_V1_ID, 1, false) == nullptr ||
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID, 1, false) == nullptr ||
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1, false) != nullptr ||
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1, false) != nullptr ||
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1, false) != nullptr ||
        degraded.DiagnosticsJson().find("\"tickHookReady\":false") == std::string::npos ||
        degraded.DiagnosticsJson().find("\"playerSnapshotTickInterval\":2") ==
            std::string::npos ||
        degraded.DiagnosticsJson().find("\"entitySnapshotTickInterval\":3") ==
            std::string::npos ||
        unavailable_reflection.find("ue5.actors is unavailable") == std::string::npos) {
        std::cerr << "missing tick/object symbols published hook-dependent services\n";
        return 7;
    }
    degraded.Stop();
    if (!anomaly::ProcessAdapterServices().Snapshot().empty()) {
        std::cerr << "degraded profile services survived stop\n";
        return 8;
    }

    anomaly::NteProfileRuntimeOptions missing_options;
    missing_options.runtime_root = root;
    missing_options.game_module = reinterpret_cast<HMODULE>(1);
    anomaly::NteProfileRuntime missing_module(std::move(missing_options));
    const bool missing_started = missing_module.Start();
    const auto missing_diagnostics = missing_module.DiagnosticsJson();
    const bool missing_build_discovery_published =
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_UE5_BUILD_SERVICE_V1_ID,
            ANOMALY_UE5_BUILD_SERVICE_V1_VERSION,
            false) != nullptr &&
        anomaly::ProcessAdapterServices().Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID,
            ANOMALY_NTE_BUILD_SERVICE_V1_VERSION,
            false) != nullptr;
    if (!missing_started || missing_module.Fingerprint() ||
        missing_module.Adapter() == nullptr || !missing_build_discovery_published ||
        missing_diagnostics.find("\"fingerprintAvailable\":false") ==
            std::string::npos ||
        missing_diagnostics.find("\"adapterStarted\":true") == std::string::npos ||
        missing_diagnostics.find("runtime PE fingerprint disabled") ==
            std::string::npos) {
        std::cerr << "missing module blocked trusted Profile resolution or discovery services\n";
        return 9;
    }
    missing_module.Stop();
    std::filesystem::remove_all(root, error);
    return 0;
}
