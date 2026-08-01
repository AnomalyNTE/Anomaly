#include "anomaly/nte_profile_runtime.hpp"

#include "anomaly/ue5_outbound_bit_count_probe.hpp"
#include "anomaly/ue5_process_event.hpp"
#include "anomaly/ue5_process_event_hook.hpp"
#include "anomaly/ue5_reflection_query.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

inline constexpr std::string_view kOutgoingTransformMetadataFeature =
    "ue5.network.outgoing-transform-metadata";
inline constexpr std::string_view kAhudFeature = "ue5.ahud";
inline constexpr std::string_view kOutboundPreHandlerDispatchSymbol =
    "ue5.PacketHandler.OutboundDispatchPreHandler";
inline constexpr std::string_view kOutgoingTransformSymbol =
    "ue5.PacketHandler.OutgoingTransform";
inline constexpr std::string_view kOutgoingTransformAbiValidator =
    "ue5-outgoing-transform-abi-v1";
inline constexpr std::string_view kEscMenuButtonFeature =
    "nte.esc-menu-button";
inline constexpr std::string_view kEscMenuHooksValidator = "nte-esc-menu-hooks-v1";
inline constexpr std::string_view kAddMenuPageSymbol =
    "nte.HTUI_MenuExtension.AddMenuPage";
inline constexpr std::string_view kExecAddMenuPageSymbol =
    "nte.HTUI_MenuExtension.execAddMenuPage";
inline constexpr std::string_view kHandleButtonClickedSymbol =
    "nte.CommonButtonBase.HandleButtonClicked";
inline constexpr std::string_view kButtonClickedSymbol =
    "nte.CommonButtonBase.BP_OnClicked";

template <std::size_t Size>
bool MatchesBytes(
    const SymbolMemory& memory,
    const std::uintptr_t address,
    const std::array<std::uint8_t, Size>& expected) noexcept {
    std::array<std::uint8_t, Size> observed{};
    return memory.Read(address, observed.data(), observed.size()) && observed == expected;
}

bool FeatureRequires(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::string_view symbol) {
    const auto found = profile.features.find(std::string(feature));
    return found != profile.features.end() &&
        std::find(found->second.begin(), found->second.end(), std::string(symbol)) !=
            found->second.end();
}

bool HasMatchingUnwindEntry(
    const ue5mem::ModuleInfo& module,
    const ResolvedSymbol& symbol) noexcept {
    DWORD64 image_base{};
    const auto* const unwind = RtlLookupFunctionEntry(
        static_cast<DWORD64>(symbol.address), &image_base, nullptr);
    return unwind != nullptr && image_base == static_cast<DWORD64>(module.base) &&
        unwind->BeginAddress == symbol.rva;
}

bool ProfileLayoutValue(
    const BuildProfile& profile,
    const std::string_view key,
    std::uint64_t* const value) {
    if (value == nullptr) return false;
    const auto found = profile.layout.find(key);
    if (found == profile.layout.end() || found->second < 0) return false;
    *value = static_cast<std::uint64_t>(found->second);
    return true;
}

bool ResolveRel32Target(
    const SymbolMemory& memory,
    const std::uintptr_t instruction,
    const std::size_t displacement_offset,
    const std::size_t instruction_size,
    std::uintptr_t* const target) noexcept {
    std::int32_t displacement{};
    if (target == nullptr || displacement_offset + sizeof(displacement) > instruction_size ||
        !memory.Read(
            instruction + displacement_offset, &displacement, sizeof(displacement))) {
        return false;
    }
    const auto next = static_cast<std::intptr_t>(instruction + instruction_size);
    const auto resolved = next + static_cast<std::intptr_t>(displacement);
    if (resolved <= 0) return false;
    *target = static_cast<std::uintptr_t>(resolved);
    return true;
}

FeatureValidationResult ValidateOutgoingTransformAbi(
    const BuildProfile& profile,
    const std::string_view feature,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    if (feature != kOutgoingTransformMetadataFeature ||
        !FeatureRequires(profile, feature, kOutboundPreHandlerDispatchSymbol) ||
        !FeatureRequires(profile, feature, kOutgoingTransformSymbol)) {
        return {false, "profile does not declare the required outbound transform topology"};
    }
    const auto* const dispatch = snapshot.FindSymbol(kOutboundPreHandlerDispatchSymbol);
    const auto* const transform = snapshot.FindSymbol(kOutgoingTransformSymbol);
    if (dispatch == nullptr || transform == nullptr || !dispatch->Available() ||
        !transform->Available()) {
        return {false, "outbound transform symbols are unavailable"};
    }

    const auto module = memory.FindModule(transform->module);
    if (!module) return {false, "profile module is unavailable"};
    if (!HasMatchingUnwindEntry(*module, *dispatch) ||
        !HasMatchingUnwindEntry(*module, *transform)) {
        return {false, "outgoing transform topology has no matching unwind entry"};
    }

    constexpr std::array<std::uint8_t, 13> kDispatchArgumentMoves{
        0x4C, 0x8B, 0xF9, 0x4C, 0x8B, 0xE2, 0x48,
        0x8B, 0x89, 0x40, 0x01, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 27> kDispatchOutputMapping{
        0x40, 0x38, 0x7D, 0xAC, 0x75, 0x0E, 0x4C,
        0x8B, 0x65, 0xA0, 0x44, 0x8B, 0x45, 0xA8,
        0x4C, 0x89, 0x65, 0x80, 0xEB, 0x03, 0x44,
        0x8B, 0xC7, 0x41, 0x8D, 0x40, 0x07};
    constexpr std::array<std::uint8_t, 41> kTransformSretPrologue{
        0x40, 0x53, 0x55, 0x57, 0x41, 0x55, 0x41,
        0x57, 0x48, 0x83, 0xEC, 0x50, 0x33, 0xDB,
        0x4D, 0x63, 0xF9, 0xF6, 0x41, 0x02, 0x01,
        0x4D, 0x8B, 0xE8, 0x48, 0x8B, 0xEA, 0x48,
        0x89, 0x1A, 0x48, 0x8B, 0xF9, 0x89, 0x5A,
        0x08, 0x88, 0x5A, 0x0C, 0x0F, 0x85};
    constexpr std::array<std::uint8_t, 17> kTransformFirstTwoStackArguments{
        0x44, 0x0F, 0xB6, 0xAC, 0x24, 0xA8, 0x00, 0x00, 0x00,
        0x4C, 0x8B, 0xA4, 0x24, 0xA0, 0x00, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 23> kTransformThirdStackArgument{
        0x45, 0x84, 0xED, 0x74, 0x14, 0x48, 0x8B, 0x94, 0x24, 0xB0,
        0x00, 0x00, 0x00, 0x4C, 0x8D, 0x47, 0x40, 0x4D, 0x8B, 0xCC,
        0xFF, 0x50, 0x38};
    constexpr auto kTransformNormalResultReturn = std::to_array<std::uint8_t>({
        0x48, 0x8B, 0x87, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44,
        0x24, 0x30, 0x8B, 0x87, 0xE0, 0x00, 0x00, 0x00, 0x89,
        0x44, 0x24, 0x38, 0x48, 0x8B, 0xC5, 0xC6, 0x44, 0x24, 0x3C,
        0x00, 0x0F, 0x10, 0x44, 0x24, 0x30, 0x0F, 0x11, 0x45, 0x00,
        0x48, 0x83, 0xC4, 0x50, 0x41, 0x5F, 0x41, 0x5D, 0x5F, 0x5D,
        0x5B, 0xC3});
    static_assert(kTransformNormalResultReturn.size() == 51U);
    constexpr std::array<std::uint8_t, 46> kTransformErrorResultReturn{
        0x48, 0xC7, 0x44, 0x24, 0x30, 0x00, 0x00, 0x00, 0x00, 0x48,
        0x8B, 0xC5, 0xC7, 0x44, 0x24, 0x38, 0x00, 0x00, 0x00, 0x00,
        0xC6, 0x44, 0x24, 0x3C, 0x01, 0x0F, 0x10, 0x44, 0x24, 0x30,
        0x0F, 0x11, 0x45, 0x00, 0x48, 0x83, 0xC4, 0x50, 0x41, 0x5F,
        0x41, 0x5D, 0x5F, 0x5D, 0x5B, 0xC3};
    constexpr std::array<std::uint8_t, 37> kTransformFastPathResultReturn{
        0x4C, 0x89, 0x6C, 0x24, 0x30, 0x48, 0x8B, 0xC5, 0x44, 0x89,
        0x7C, 0x24, 0x38, 0x88, 0x5C, 0x24, 0x3C, 0x0F, 0x10, 0x44,
        0x24, 0x30, 0x0F, 0x11, 0x02, 0x48, 0x83, 0xC4, 0x50, 0x41,
        0x5F, 0x41, 0x5D, 0x5F, 0x5D, 0x5B, 0xC3};
    if (!MatchesBytes(memory, dispatch->address + 0x2BU, kDispatchArgumentMoves) ||
        !MatchesBytes(memory, dispatch->address + 0xA8U, kDispatchOutputMapping) ||
        !MatchesBytes(memory, transform->address, kTransformSretPrologue) ||
        !MatchesBytes(memory, transform->address + 0xA0U, kTransformFirstTwoStackArguments) ||
        !MatchesBytes(memory, transform->address + 0xF4U, kTransformThirdStackArgument) ||
        !MatchesBytes(memory, transform->address + 0x3FFU, kTransformNormalResultReturn) ||
        !MatchesBytes(memory, transform->address + 0x432U, kTransformErrorResultReturn) ||
        !MatchesBytes(memory, transform->address + 0x460U, kTransformFastPathResultReturn)) {
        return {false, "outbound transform ABI instruction contract changed"};
    }

    std::array<std::uint8_t, 5> call{};
    if (!memory.Read(dispatch->address + 0x6CU, call.data(), call.size()) || call[0] != 0xE8U) {
        return {false, "outbound dispatcher no longer calls the transform directly"};
    }
    std::int32_t displacement{};
    std::memcpy(&displacement, call.data() + 1U, sizeof(displacement));
    const auto target = static_cast<std::intptr_t>(dispatch->address + 0x71U) +
        static_cast<std::intptr_t>(displacement);
    if (target <= 0 || static_cast<std::uintptr_t>(target) != transform->address) {
        return {false, "outbound dispatcher call target changed"};
    }
    return {true, {}};
}

FeatureValidationResult ValidateEscMenuHooks(
    const BuildProfile& profile,
    const std::string_view feature,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    if (feature != kEscMenuButtonFeature ||
        !FeatureRequires(profile, feature, kAddMenuPageSymbol) ||
        !FeatureRequires(profile, feature, kExecAddMenuPageSymbol) ||
        !FeatureRequires(profile, feature, kHandleButtonClickedSymbol) ||
        !FeatureRequires(profile, feature, kButtonClickedSymbol)) {
        return {false, "profile does not declare the ESC menu hook topology"};
    }
    const auto* const add_menu_page = snapshot.FindSymbol(kAddMenuPageSymbol);
    const auto* const exec_add_menu_page = snapshot.FindSymbol(kExecAddMenuPageSymbol);
    const auto* const handle_button_clicked =
        snapshot.FindSymbol(kHandleButtonClickedSymbol);
    const auto* const button_clicked = snapshot.FindSymbol(kButtonClickedSymbol);
    if (add_menu_page == nullptr || exec_add_menu_page == nullptr ||
        handle_button_clicked == nullptr || button_clicked == nullptr ||
        !add_menu_page->Available() || !exec_add_menu_page->Available() ||
        !handle_button_clicked->Available() || !button_clicked->Available()) {
        return {false, "ESC menu hook symbols are unavailable"};
    }
    const auto module = memory.FindModule(add_menu_page->module);
    if (!module || exec_add_menu_page->module != add_menu_page->module ||
        handle_button_clicked->module != add_menu_page->module ||
        button_clicked->module != add_menu_page->module) {
        return {false, "ESC menu hook symbols do not share the profile module"};
    }
    if (!HasMatchingUnwindEntry(*module, *add_menu_page) ||
        !HasMatchingUnwindEntry(*module, *exec_add_menu_page) ||
        !HasMatchingUnwindEntry(*module, *handle_button_clicked) ||
        !HasMatchingUnwindEntry(*module, *button_clicked)) {
        return {false, "ESC menu hook topology has no matching unwind entry"};
    }

    // These bytes begin after MinHook's entry patch, so deferred feature
    // validation remains valid while the bridge owns the two detours.
    constexpr std::array<std::uint8_t, 5> kAddMenuPageArguments{
        0x8B, 0xEA, 0x48, 0x8B, 0xF9};
    if (!MatchesBytes(memory, add_menu_page->address + 0x0EU, kAddMenuPageArguments)) {
        return {false, "AddMenuPage argument contract changed"};
    }
    constexpr std::array<std::uint8_t, 8> kHandleClickVirtualDispatch{
        0x48, 0x8B, 0x03, 0x48, 0x8B, 0xCB, 0xFF, 0x90};
    std::uint32_t handle_click_vtable_offset{};
    std::uint64_t declared_vtable_offset{};
    if (!MatchesBytes(
            memory, handle_button_clicked->address + 0x7FU,
            kHandleClickVirtualDispatch) ||
        !memory.Read(
            handle_button_clicked->address + 0x87U,
            &handle_click_vtable_offset, sizeof(handle_click_vtable_offset)) ||
        !ProfileLayoutValue(
            profile, "escMenu.buttonClickedVtableOffset", &declared_vtable_offset) ||
        declared_vtable_offset != handle_click_vtable_offset) {
        return {false, "HandleButtonClicked virtual dispatch contract changed"};
    }

    constexpr std::array<std::uint8_t, 25> kAddMenuPageExecDispatch{
        0x48, 0x8B, 0x43, 0x20, 0x48, 0x8B, 0xCE, 0x8B, 0x54,
        0x24, 0x38, 0x48, 0x85, 0xC0, 0x40, 0x0F, 0x95, 0xC7,
        0x48, 0x03, 0xF8, 0x48, 0x89, 0x7B, 0x20};
    std::uint8_t call_opcode{};
    std::uintptr_t add_menu_page_call_target{};
    if (!MatchesBytes(
            memory, exec_add_menu_page->address + 0x55U,
            kAddMenuPageExecDispatch) ||
        !memory.Read(
            exec_add_menu_page->address + 0x6EU,
            &call_opcode, sizeof(call_opcode)) ||
        call_opcode != 0xE8U ||
        !ResolveRel32Target(
            memory, exec_add_menu_page->address + 0x6EU, 1U, 5U,
            &add_menu_page_call_target) ||
        add_menu_page_call_target != add_menu_page->address) {
        return {false, "AddMenuPage Exec wrapper no longer calls the hook target"};
    }

    constexpr std::array<std::uint8_t, 10> kButtonClickedOverrideCheck{
        0xF6, 0x81, 0x68, 0x04, 0x00, 0x00, 0x02, 0x48, 0x8B, 0xD9};
    constexpr std::array<std::uint8_t, 19> kButtonClickedScriptDispatch{
        0x4C, 0x8B, 0x0B, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0xD0, 0x48,
        0x8B, 0xCB, 0x41, 0xFF, 0x91, 0x60, 0x02, 0x00, 0x00};
    if (!MatchesBytes(
            memory, button_clicked->address + 0x06U,
            kButtonClickedOverrideCheck) ||
        !MatchesBytes(
            memory, button_clicked->address + 0x22U,
            kButtonClickedScriptDispatch)) {
        return {false, "BP_OnClicked wrapper contract changed"};
    }
    return {true, {}};
}

FeatureLayoutValidatorRegistry NteFeatureLayoutValidators(
    const bool preserve_actor_process_event_abi) {
    FeatureLayoutValidatorRegistry validators = Ue5FeatureLayoutValidators();
    validators.Register(
        std::string(kOutgoingTransformAbiValidator), ValidateOutgoingTransformAbi);
    validators.Register(std::string(kEscMenuHooksValidator), ValidateEscMenuHooks);
    if (preserve_actor_process_event_abi) {
        validators.Register(
            std::string(kUe5ActorProcessEventAbiValidator), [](
                const BuildProfile&,
                const std::string_view feature,
                const ProfileResolutionSnapshot& snapshot,
                const SymbolMemory&) {
                if (feature != kUe5ActorProcessEventFeature) {
                    return FeatureValidationResult{
                        false,
                        "startup AActor ProcessEvent ABI evidence used by another feature"};
                }
                const auto* const actor_process_event =
                    snapshot.FindSymbol(kUe5ActorProcessEventSymbol);
                if (actor_process_event == nullptr ||
                    !actor_process_event->Available()) {
                    return FeatureValidationResult{
                        false, "ue5.AActorProcessEvent is unavailable"};
                }
                // Runtime owns the entry bytes after installing its detour. The
                // unhooked ABI was validated before the adapter was constructed.
                return FeatureValidationResult{true, {}};
            });
    }
    return validators;
}

void AppendOutgoingTransformProbeSnapshot(
    std::string& json,
    const Ue5OutboundBitCountProbe* const probe) {
    if (probe == nullptr) {
        json += "{\"started\":false}";
        return;
    }
    const auto snapshot = probe->Snapshot();
    json += "{\"started\":" + std::string(snapshot.started ? "true" : "false");
    json += ",\"callCount\":" + std::to_string(snapshot.call_count);
    json += ",\"successfulResultCount\":" + std::to_string(snapshot.successful_result_count);
    json += ",\"errorResultCount\":" + std::to_string(snapshot.error_result_count);
    json += ",\"inputNonzeroBitCountCallCount\":" +
        std::to_string(snapshot.input_nonzero_bit_count_call_count);
    json += ",\"outputNonzeroBitCountCallCount\":" +
        std::to_string(snapshot.output_nonzero_bit_count_call_count);
    json += ",\"nullInputDataCount\":" + std::to_string(snapshot.null_input_data_count);
    json += ",\"nullOutputDataCount\":" + std::to_string(snapshot.null_output_data_count);
    json += ",\"sameDataPointerResultCount\":" +
        std::to_string(snapshot.same_data_pointer_result_count);
    json += ",\"invalidInputArgumentCount\":" +
        std::to_string(snapshot.invalid_input_argument_count);
    json += ",\"invalidOutputResultCount\":" +
        std::to_string(snapshot.invalid_output_result_count);
    json += ",\"resultPointerMismatchCount\":" +
        std::to_string(snapshot.result_pointer_mismatch_count);
    json += ",\"unexpectedErrorValueCount\":" +
        std::to_string(snapshot.unexpected_error_value_count);
    json += ",\"aggregateOverflowCount\":" +
        std::to_string(snapshot.aggregate_overflow_count);
    json += ",\"maximumInputBitCount\":" +
        std::to_string(snapshot.maximum_input_bit_count);
    json += ",\"maximumOutputBitCount\":" +
        std::to_string(snapshot.maximum_output_bit_count);
    json += ",\"inputCeilByteTotal\":" +
        std::to_string(snapshot.input_ceil_byte_total);
    json += ",\"outputCeilByteTotal\":" +
        std::to_string(snapshot.output_ceil_byte_total);
    json += '}';
}

std::filesystem::path ModulePath(HMODULE module) {
    if (module == nullptr) module = GetModuleHandleW(nullptr);
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

std::string Quote(std::string_view value) {
    std::string result{"\""};
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result.push_back('"');
    return result;
}

void AppendStringArray(std::string& json, const std::vector<std::string>& values) {
    json.push_back('[');
    bool first = true;
    for (const auto& value : values) {
        if (!first) json.push_back(',');
        first = false;
        json += Quote(value);
    }
    json.push_back(']');
}

void AppendStringArray(
    std::string& json,
    const std::set<std::string, std::less<>>& values) {
    json.push_back('[');
    bool first = true;
    for (const auto& value : values) {
        if (!first) json.push_back(',');
        first = false;
        json += Quote(value);
    }
    json.push_back(']');
}

void AppendFeatureStringLists(
    std::string& json,
    const std::map<std::string, std::vector<std::string>, std::less<>>& lists) {
    json.push_back('{');
    bool first = true;
    for (const auto& [id, values] : lists) {
        if (!first) json.push_back(',');
        first = false;
        json += Quote(id);
        json.push_back(':');
        AppendStringArray(json, values);
    }
    json.push_back('}');
}

std::string HexAddress(std::uintptr_t address) {
    if (address == 0) return {};
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << address;
    return stream.str();
}

std::string_view ResolutionStateName(ProfileResolutionState state) noexcept {
    switch (state) {
    case ProfileResolutionState::NoProfile: return "no-profile";
    case ProfileResolutionState::ProfileLoaded: return "profile-loaded";
    case ProfileResolutionState::Degraded: return "degraded";
    case ProfileResolutionState::Ready: return "ready";
    }
    return "unknown";
}

std::string_view SymbolStateName(SymbolResolutionState state) noexcept {
    switch (state) {
    case SymbolResolutionState::Unavailable: return "unavailable";
    case SymbolResolutionState::CacheTrusted: return "cache-trusted";
    case SymbolResolutionState::Resolved: return "resolved";
    case SymbolResolutionState::ModuleMissing: return "module-missing";
    case SymbolResolutionState::SectionMissing: return "section-missing";
    case SymbolResolutionState::PatternInvalid: return "pattern-invalid";
    case SymbolResolutionState::NotFound: return "not-found";
    case SymbolResolutionState::Ambiguous: return "ambiguous";
    case SymbolResolutionState::AddressResolutionFailed: return "address-resolution-failed";
    case SymbolResolutionState::ValidationFailed: return "validation-failed";
    }
    return "unknown";
}

struct RetainedNteProfileGeneration final {
    std::unique_ptr<GameTickHook> tick_hook;
    std::unique_ptr<Ue5ProcessEventHook> process_event_hook;
    std::unique_ptr<Ue5OutboundBitCountProbe> outgoing_transform_probe;
    std::shared_ptr<Ue5NteAdapter> adapter;
    RetainedNteProfileGeneration* next{};
};

struct NteProfileQuarantineRegistry final {
    std::mutex mutex;
    std::atomic_bool quarantined{};
    RetainedNteProfileGeneration* generations{};
};

NteProfileQuarantineRegistry* ProcessNteProfileQuarantine() noexcept {
    // Deliberately process-lived: a permanently blocked game callback makes
    // destruction of its hook generation invalid even during static teardown.
    static auto* registry = new (std::nothrow) NteProfileQuarantineRegistry;
    return registry;
}

bool NteProfileGenerationQuarantined() noexcept {
    const auto* registry = ProcessNteProfileQuarantine();
    // If the fence cannot be allocated, conservatively prevent another exact
    // profile generation from being activated in the same process.
    return registry == nullptr ||
        registry->quarantined.load(std::memory_order_acquire);
}

class TickEvidenceObserverGate final {
public:
    using Observer = std::function<void(std::uint32_t, double)>;

    explicit TickEvidenceObserverGate(Observer observer)
        : observer_(std::move(observer)) {}

    void Observe(std::uint32_t thread_id, double duration_micros) noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (!open_) return;
            ++active_;
        }
        try {
            observer_(thread_id, duration_micros);
        } catch (...) {
        }
        {
            std::scoped_lock lock(mutex_);
            --active_;
        }
        condition_.notify_all();
    }

    // Close is the observer-side stop linearization point. Any detour which
    // reaches the callback after this call returns is ignored, while callers
    // already inside Observe remain counted until the observer returns.
    void Close() noexcept {
        std::scoped_lock lock(mutex_);
        open_ = false;
    }

    bool Drain(std::chrono::milliseconds timeout) noexcept {
        const auto bounded_timeout =
            (std::max)(timeout, std::chrono::milliseconds::zero());
        std::unique_lock lock(mutex_);
        const auto drained = [this] { return active_ == 0; };
        if (bounded_timeout == std::chrono::milliseconds::max()) {
            condition_.wait(lock, drained);
            return true;
        }
        return condition_.wait_for(lock, bounded_timeout, drained);
    }

private:
    Observer observer_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_{true};
    std::size_t active_{};
};

void RetainNteProfileGeneration(
    std::unique_ptr<GameTickHook> tick_hook,
    std::unique_ptr<Ue5ProcessEventHook> process_event_hook,
    std::unique_ptr<Ue5OutboundBitCountProbe> outgoing_transform_probe,
    std::shared_ptr<Ue5NteAdapter> adapter) noexcept {
    auto* registry = ProcessNteProfileQuarantine();
    if (registry == nullptr) {
        // The hook generation itself is intentionally leaked. Adapter State
        // has shared ownership and zero-budget hook destruction preserves any
        // still-running callback without extending the stop call.
        static_cast<void>(tick_hook.release());
        static_cast<void>(process_event_hook.release());
        static_cast<void>(outgoing_transform_probe.release());
        return;
    }
    registry->quarantined.store(true, std::memory_order_release);
    auto* generation = new (std::nothrow) RetainedNteProfileGeneration{
        std::move(tick_hook), std::move(process_event_hook),
        std::move(outgoing_transform_probe), std::move(adapter), nullptr};
    if (generation == nullptr) {
        // Both hook types and Ue5NteAdapter independently preserve their live
        // state when a zero-budget drain still reports in-flight.
        static_cast<void>(tick_hook.release());
        static_cast<void>(process_event_hook.release());
        static_cast<void>(outgoing_transform_probe.release());
        return;
    }
    std::scoped_lock lock(registry->mutex);
    generation->next = registry->generations;
    registry->generations = generation;
}

}  // namespace

class NteProfileRuntime::Impl final {
public:
    explicit Impl(NteProfileRuntimeOptions options)
        : options_(std::move(options)),
          memory_(std::make_shared<LiveSymbolMemory>(options_.memory_services)) {
        options_.runtime_root = std::filesystem::absolute(options_.runtime_root);
        if (!options_.profile_directory.is_absolute()) {
            options_.profile_directory = options_.runtime_root / options_.profile_directory;
        }
        if (!options_.local_profile_directory.is_absolute()) {
            options_.local_profile_directory = options_.runtime_root / options_.local_profile_directory;
        }
        if (!options_.managed_profile_directory.is_absolute()) {
            options_.managed_profile_directory = options_.runtime_root / options_.managed_profile_directory;
        }
    }

    bool Start(std::stop_token stop_token) {
        std::scoped_lock lock(mutex_);
        if (started_ || stopping_) return false;
        diagnostics_.clear();
        catalog_ = {};
        profile_.reset();
        resolution_.reset();
        adapter_.reset();
        tick_evidence_gate_.reset();
        tick_hook_.reset();
        process_event_hook_.reset();
        outgoing_transform_probe_.reset();
        tick_hook_ready_ = false;
        ahud_hook_ready_ = false;
        quarantined_ = false;
        module_path_ = ModulePath(options_.game_module);
        std::vector<BuildProfileCatalogLayer> layers;
        if (options_.profile_overrides_enabled) {
            layers.push_back({
                options_.local_profile_directory / options_.game_id,
                "local-override", 300, true});
            layers.push_back({
                options_.managed_profile_directory / options_.game_id,
                "repository", 200, true});
        } else {
            diagnostics_.push_back("Profile overrides suspended by Runtime recovery policy");
        }
        layers.push_back({
            options_.profile_directory / options_.game_id, "bundled", 100, false});
        BuildProfileCatalog catalog;
        catalog_ = catalog.ScanLayered(layers);
        for (const auto& diagnostic : catalog_.diagnostics) {
            diagnostics_.push_back(
                diagnostic.source.string() + diagnostic.path + ": " + diagnostic.message);
        }
        std::optional<BuildProfile> selected;
        for (const BuildProfile& source : catalog_.profiles) {
            if (source.game != options_.game_id) continue;
            diagnostics_.push_back(
                "profile recipe selected source=" + source.source.string());
            selected = source;
            break;
        }

        BuildFingerprint adapter_context;
        adapter_context.game = options_.game_id;
        adapter_context.module = module_path_.filename().wstring();
        adapter_context.canonical_path_tail = adapter_context.module;
        diagnostics_.push_back(
            "runtime PE fingerprint disabled; active Profile and RVA cache are trusted");

        if (selected && !WaitForProfileSections(*selected, stop_token)) {
            return false;
        }

        SymbolResolver resolver(memory_, {}, {}, NteFeatureLayoutValidators(false));
        SymbolCache cache(options_.runtime_root / L"state" / L"profile-symbol-cache.json");
        ProfileResolutionSnapshot resolved =
            resolver.Resolve(adapter_context, selected ? &*selected : nullptr, &cache);
        if (!selected) {
            resolution_ = std::make_shared<ProfileResolutionSnapshot>(std::move(resolved));
            diagnostics_.push_back("no active profile recipe for game " + options_.game_id);
        } else {
            profile_ = std::move(*selected);
            resolution_ = std::make_shared<ProfileResolutionSnapshot>(std::move(resolved));
        }
        if (profile_ && NteProfileGenerationQuarantined()) {
            diagnostics_.push_back(
                "profile generation not activated: prior hook generation is quarantined");
            quarantined_ = true;
            started_ = true;
            return true;
        }
        BuildProfile discovery_profile;
        discovery_profile.game = options_.game_id;
        const BuildProfile& adapter_profile = profile_ ? *profile_ : discovery_profile;
        adapter_ = std::make_shared<Ue5NteAdapter>(
            std::move(adapter_context), adapter_profile, *resolution_, memory_,
            ProcessAdapterServices(),
            options_.snapshot_sampling,
            NteFeatureLayoutValidators(
                resolution_->FeatureAvailable(kUe5ActorProcessEventFeature)),
            profile_ ? CreateUe5ProcessEventInvoker(*profile_, *resolution_, *memory_)
                     : Ue5NteAdapter::ProcessEventInvoker{});
        bool hook_ready{};
        const auto* tick = resolution_->FindSymbol("ue5.GameTick");
        if (tick != nullptr && tick->Available() &&
            resolution_->FeatureAvailable("ue5.framework")) {
            if (options_.tick_evidence_observer) {
                tick_evidence_gate_ = std::make_shared<TickEvidenceObserverGate>(
                    options_.tick_evidence_observer);
            }
            tick_hook_ = std::make_unique<GameTickHook>(
                [weak = std::weak_ptr<Ue5NteAdapter>(adapter_),
                 evidence = tick_evidence_gate_](double delta) {
                    const auto started_at = std::chrono::steady_clock::now();
                    // Keep the adapter generation alive through evidence
                    // observation. This also protects the service tables if
                    // process-quarantine bookkeeping allocation ever fails.
                    const auto adapter = weak.lock();
                    if (adapter) adapter->OnGameTick(delta);
                    if (evidence) {
                        const double duration_micros =
                            std::chrono::duration<double, std::micro>(
                                std::chrono::steady_clock::now() - started_at).count();
                        evidence->Observe(GetCurrentThreadId(), duration_micros);
                    }
                });
            hook_ready = tick_hook_->Start(reinterpret_cast<void*>(tick->address));
            if (!hook_ready) diagnostics_.push_back("game tick hook activation failed");
        }
        bool ahud_hook_ready{};
        const auto* const actor_process_event =
            resolution_->FindSymbol(kUe5ActorProcessEventSymbol);
        if (profile_ && profile_->features.contains(std::string(kAhudFeature))) {
            if (!hook_ready) {
                diagnostics_.push_back(
                    "optional AHUD capability unavailable: game tick hook failed");
            } else if (!resolution_->FeatureAvailable(kUe5ProcessEventFeature) ||
                !resolution_->FeatureAvailable(kUe5ActorProcessEventFeature) ||
                actor_process_event == nullptr || !actor_process_event->Available()) {
                diagnostics_.push_back(
                    "optional AHUD capability unavailable: Actor ProcessEvent gate failed");
            } else {
                try {
                    process_event_hook_ = std::make_unique<Ue5ProcessEventHook>(
                        [weak = std::weak_ptr<Ue5NteAdapter>(adapter_)](
                            const std::uintptr_t object,
                            const std::uintptr_t function,
                            void* const parameters,
                            const Ue5ProcessEventInvoker& original) {
                            const auto adapter = weak.lock();
                            if (adapter) {
                                adapter->OnProcessEvent(
                                    object, function, parameters, original);
                            }
                        });
                    ahud_hook_ready = process_event_hook_->Start(
                        reinterpret_cast<void*>(actor_process_event->address));
                    if (!ahud_hook_ready) {
                        diagnostics_.push_back(
                            "AHUD Actor ProcessEvent hook activation failed");
                    }
                } catch (...) {
                    process_event_hook_.reset();
                    diagnostics_.push_back("AHUD ProcessEvent hook allocation failed");
                }
                if (ahud_hook_ready && !resolution_->FeatureAvailable(kAhudFeature)) {
                    diagnostics_.push_back(
                        "optional AHUD service pending: reflection gate not ready");
                }
            }
        }
        if (!adapter_->Start(hook_ready, ahud_hook_ready)) {
            diagnostics_.push_back("adapter service publication failed");
            if (process_event_hook_) process_event_hook_->Stop();
            process_event_hook_.reset();
            if (tick_hook_) tick_hook_->Stop();
            tick_hook_.reset();
            tick_evidence_gate_.reset();
            adapter_.reset();
        } else {
            tick_hook_ready_ = hook_ready;
            ahud_hook_ready_ = ahud_hook_ready;
        }
        const bool player_service_published = adapter_ &&
            ProcessAdapterServices().Query(
                ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
                ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION,
                false) != nullptr;
        if (adapter_ && !resolution_->FeatureAvailable("nte.player")) {
            diagnostics_.push_back(
                "service anomaly.nte.player initially unavailable: nte.player feature unavailable");
        } else if (adapter_ && !hook_ready) {
            diagnostics_.push_back(
                "service anomaly.nte.player initially unavailable: game tick hook unavailable");
        } else if (adapter_ && !player_service_published) {
            diagnostics_.push_back(
                "service anomaly.nte.player initially unavailable: service registry rejected publication");
        }
        if (profile_ && profile_->features.contains(
                std::string(kOutgoingTransformMetadataFeature))) {
            const auto* const target = resolution_->FindSymbol(kOutgoingTransformSymbol);
            if (!resolution_->FeatureAvailable(kOutgoingTransformMetadataFeature) ||
                target == nullptr || !target->Available()) {
                diagnostics_.push_back(
                    "optional outgoing transform metadata capability unavailable: exact ABI gate failed");
            } else {
                try {
                    auto probe = std::make_unique<Ue5OutboundBitCountProbe>(
                        CreateMinHookBackend());
                    if (!probe->Start(reinterpret_cast<void*>(target->address))) {
                        diagnostics_.push_back("outgoing transform metadata probe activation failed");
                    } else {
                        outgoing_transform_probe_ = std::move(probe);
                    }
                } catch (...) {
                    diagnostics_.push_back("outgoing transform metadata probe allocation failed");
                }
            }
        }
        started_ = true;
        return true;
    }

    bool Stop(std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex_);
        if (!started_) return !stopping_;
        auto tick_hook = std::move(tick_hook_);
        auto process_event_hook = std::move(process_event_hook_);
        auto outgoing_transform_probe = std::move(outgoing_transform_probe_);
        auto adapter = std::move(adapter_);
        auto tick_evidence_gate = std::move(tick_evidence_gate_);
        // Close while holding the Runtime state mutex so Started()==false is
        // only observable after the smoke observer has been fenced.
        if (tick_evidence_gate) tick_evidence_gate->Close();
        tick_hook_ready_ = false;
        ahud_hook_ready_ = false;
        started_ = false;
        stopping_ = true;
        lock.unlock();

        const auto bounded_timeout =
            (std::max)(timeout, std::chrono::milliseconds::zero());
        const auto started_at = std::chrono::steady_clock::now();
        const auto remaining = [&]() noexcept {
            if (bounded_timeout == std::chrono::milliseconds::max()) {
                return std::chrono::milliseconds::max();
            }
            const auto elapsed = std::chrono::steady_clock::now() - started_at;
            if (elapsed >= bounded_timeout) return std::chrono::milliseconds::zero();
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                bounded_timeout - elapsed);
        };

        // An observer samples the adapter service registry. Drain observers
        // which crossed the close boundary before revoking those services;
        // late detours are rejected by the closed gate. If the deadline is
        // exceeded the entire generation is quarantined with its services
        // still valid for the in-flight observer.
        const bool outgoing_transform_probe_drained = outgoing_transform_probe == nullptr ||
            outgoing_transform_probe->Stop(remaining());
        const bool evidence_drained = tick_evidence_gate == nullptr ||
            tick_evidence_gate->Drain(remaining());
        bool adapter_drained = adapter == nullptr;
        if (evidence_drained && adapter != nullptr) {
            // Revoke services and detach the inner callback before waiting on
            // the external detour. A racing detour then observes
            // Started()==false and cannot create a new adapter invocation.
            adapter_drained = adapter->Stop(std::chrono::milliseconds::zero());
        }
        const bool tick_hook_drained = tick_hook == nullptr || tick_hook->Stop(remaining());
        const bool process_event_hook_drained = process_event_hook == nullptr ||
            process_event_hook->Stop(remaining());
        if (evidence_drained && !adapter_drained && adapter != nullptr) {
            adapter_drained = adapter->Stop(remaining());
        }
        const bool drained = outgoing_transform_probe_drained && evidence_drained &&
            tick_hook_drained && process_event_hook_drained && adapter_drained;
        if (!drained) {
            RetainNteProfileGeneration(
                std::move(tick_hook), std::move(process_event_hook),
                std::move(outgoing_transform_probe), std::move(adapter));
        }

        lock.lock();
        stopping_ = false;
        quarantined_ = !drained;
        if (!drained) {
            diagnostics_.push_back(
                "profile stop deadline exceeded: hook generation quarantined");
        }
        return drained;
    }

    bool Started() const noexcept {
        std::scoped_lock lock(mutex_);
        return started_;
    }

    std::optional<BuildFingerprint> Fingerprint() const {
        std::scoped_lock lock(mutex_);
        return std::nullopt;
    }

    std::shared_ptr<const ProfileResolutionSnapshot> Resolution() const {
        std::scoped_lock lock(mutex_);
        return CurrentResolutionLocked();
    }

    std::shared_ptr<Ue5NteAdapter> Adapter() const {
        std::scoped_lock lock(mutex_);
        return adapter_;
    }

    NteProfileEvidenceSnapshot Evidence() const {
        std::scoped_lock lock(mutex_);
        NteProfileEvidenceSnapshot snapshot;
        snapshot.profile = profile_;
        if (const auto resolution = CurrentResolutionLocked()) {
            snapshot.resolution = *resolution;
        }
        snapshot.tick_hook_ready = tick_hook_ready_;
        snapshot.ahud_hook_ready = ahud_hook_ready_;
        if (adapter_) {
            snapshot.game_thread_id = adapter_->GameThreadId();
            snapshot.tick_sequence = adapter_->TickSequence();
            snapshot.rejected_thread_ticks = adapter_->RejectedThreadTicks();
            snapshot.ahud_binding_ready = adapter_->AhudBindingReady();
            snapshot.ahud_frame_count = adapter_->AhudFrameCount();
            snapshot.ahud_process_event_call_count =
                adapter_->AhudProcessEventCallCount();
        }
        return snapshot;
    }

    std::vector<HookRecordView> Hooks() const {
        std::scoped_lock lock(mutex_);
        std::vector<HookRecordView> hooks;
        if (tick_hook_) hooks = tick_hook_->Snapshot();
        if (process_event_hook_) {
            auto process_event_hooks = process_event_hook_->Snapshot();
            hooks.insert(
                hooks.end(),
                std::make_move_iterator(process_event_hooks.begin()),
                std::make_move_iterator(process_event_hooks.end()));
        }
        return hooks;
    }

    std::string ExecuteReflectionQuery(const std::string_view request) const {
        std::optional<BuildProfile> profile;
        std::shared_ptr<const ProfileResolutionSnapshot> resolution;
        std::shared_ptr<const SymbolMemory> memory;
        {
            std::scoped_lock lock(mutex_);
            if (!started_ || stopping_ || !profile_) {
                return "{\"ok\":false,\"error\":\"UE reflection queries are unavailable\"}";
            }
            profile = profile_;
            resolution = CurrentResolutionLocked();
            memory = memory_;
        }
        if (!resolution || !memory) {
            return "{\"ok\":false,\"error\":\"UE reflection queries are unavailable\"}";
        }
        return ExecuteUe5ReflectionQuery(
            {*profile, *resolution, *memory}, request);
    }

    std::string DiagnosticsJson() const {
        std::scoped_lock lock(mutex_);
        const auto resolution = CurrentResolutionLocked();
        const ProfileResolutionState state = resolution
            ? resolution->state
            : ProfileResolutionState::NoProfile;
        std::string json = "{\"ok\":true,\"state\":" +
            Quote(ResolutionStateName(state));
        json += ",\"buildId\":" +
            Quote(resolution ? resolution->build_id : std::string{});
        json += ",\"modulePath\":" + Quote(module_path_.string());
        json += ",\"fingerprintAvailable\":" +
            std::string{"false"};
        json += ",\"profileHash\":" + Quote(
            resolution ? resolution->profile_hash : std::string{});
        json += ",\"profileChannel\":" + Quote(profile_ ? profile_->source_channel : std::string{});
        json += ",\"profileSource\":" + Quote(profile_ ? profile_->source.string() : std::string{});
        json += ",\"cacheLoaded\":" +
            std::string(resolution && resolution->cache_loaded ? "true" : "false");
        json += ",\"adapterStarted\":" +
            std::string(adapter_ && adapter_->Started() ? "true" : "false");
        json += ",\"profileQuarantined\":" +
            std::string(quarantined_ ? "true" : "false");
        json += ",\"tickHookReady\":" +
            std::string(tick_hook_ready_ ? "true" : "false");
        json += ",\"ahudHookReady\":" +
            std::string(ahud_hook_ready_ ? "true" : "false");
        json += ",\"ahudBindingReady\":" +
            std::string(
                adapter_ && adapter_->AhudBindingReady() ? "true" : "false");
        json += ",\"ahudFrameCount\":" +
            std::to_string(adapter_ ? adapter_->AhudFrameCount() : 0U);
        json += ",\"ahudProcessEventCallCount\":" +
            std::to_string(
                adapter_ ? adapter_->AhudProcessEventCallCount() : 0U);
        const bool player_service_published = adapter_ &&
            ProcessAdapterServices().Query(
                ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
                ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION,
                false) != nullptr;
        json += ",\"ntePlayerPublished\":" +
            std::string(player_service_published ? "true" : "false");
        json += ",\"ntePlayerRefreshMode\":" + Quote(
            player_service_published
                ? "game-tick"
                : std::string{});
        json += ",\"playerSnapshotTickInterval\":" +
            std::to_string(options_.snapshot_sampling.player_tick_interval);
        json += ",\"entitySnapshotTickInterval\":" +
            std::to_string(options_.snapshot_sampling.entity_tick_interval);
        json += ",\"outgoingTransformMetadataProbe\":";
        AppendOutgoingTransformProbeSnapshot(json, outgoing_transform_probe_.get());
        json += ",\"optionalFeatures\":";
        if (profile_) {
            AppendStringArray(json, profile_->optional_features);
        } else {
            json += "[]";
        }
        json += ",\"featureLayoutValidators\":";
        if (profile_) {
            AppendFeatureStringLists(json, profile_->feature_layout_validators);
        } else {
            json += "{}";
        }
        json += ",\"featureDependencies\":";
        if (profile_) {
            AppendFeatureStringLists(json, profile_->feature_dependencies);
        } else {
            json += "{}";
        }
        json += ",\"symbols\":[";
        bool first = true;
        if (resolution) {
            for (const auto& [id, symbol] : resolution->symbols) {
                if (!first) json.push_back(',');
                first = false;
                const std::string candidate_address = HexAddress(symbol.address);
                json += "{\"id\":" + Quote(id) + ",\"state\":" +
                    Quote(SymbolStateName(symbol.state)) + ",\"available\":" +
                    (symbol.Available() ? std::string("true") : std::string("false")) +
                    ",\"rva\":" + std::to_string(symbol.rva) +
                    ",\"address\":" + Quote(
                        symbol.Available() ? candidate_address : std::string{}) +
                    ",\"candidateAddress\":" + Quote(
                        symbol.Available() ? std::string{} : candidate_address) +
                    ",\"diagnostics\":[";
                bool first_diagnostic = true;
                for (const auto& diagnostic : symbol.diagnostics) {
                    if (!first_diagnostic) json.push_back(',');
                    first_diagnostic = false;
                    json += Quote(diagnostic);
                }
                json += "]}";
            }
        }
        json += "],\"features\":[";
        first = true;
        if (resolution) {
            for (const auto& [id, feature] : resolution->features) {
                if (!first) json.push_back(',');
                first = false;
                json += "{\"id\":" + Quote(id) + ",\"available\":" +
                    (feature.available ? std::string("true") : std::string("false"));
                json += ",\"missingSymbols\":";
                AppendStringArray(json, feature.missing_symbols);
                json += ",\"unavailableDependencies\":";
                AppendStringArray(json, feature.unavailable_dependencies);
                json += ",\"validationDiagnostics\":";
                AppendStringArray(json, feature.validation_diagnostics);
                json += "}";
            }
        }
        json += "],\"diagnostics\":[";
        first = true;
        for (const auto& diagnostic : diagnostics_) {
            if (!first) json.push_back(',');
            first = false;
            json += Quote(diagnostic);
        }
        json += "]}";
        return json;
    }

private:
    [[nodiscard]] std::vector<std::string> MissingProfileSections(
        const BuildProfile& profile) const {
        std::map<std::wstring, std::set<std::string, std::less<>>, std::less<>> required;
        for (const auto& [id, symbol] : profile.symbols) {
            static_cast<void>(id);
            required[symbol.module].insert(symbol.section);
        }

        std::vector<std::string> missing;
        for (const auto& [module_name, section_names] : required) {
            const auto module = memory_->FindModule(module_name);
            const std::string module_text = std::filesystem::path(module_name).string();
            if (!module) {
                missing.push_back(module_text + ": module unavailable");
                continue;
            }
            const auto sections = memory_->Sections(*module);
            for (const auto& section_name : section_names) {
                const bool available = std::ranges::any_of(
                    sections, [&](const auto& section) { return section.name == section_name; });
                if (!available) missing.push_back(module_text + ":" + section_name);
            }
        }
        return missing;
    }

    [[nodiscard]] bool WaitForProfileSections(
        const BuildProfile& profile,
        std::stop_token stop_token) {
        const auto started_at = std::chrono::steady_clock::now();
        const auto timeout = (std::max)(
            options_.section_readiness_timeout, std::chrono::milliseconds::zero());
        const auto poll_interval = (std::max)(
            options_.section_readiness_poll_interval, std::chrono::milliseconds(1));
        bool waited{};
        for (;;) {
            const auto missing = MissingProfileSections(profile);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at);
            if (missing.empty()) {
                if (waited) {
                    diagnostics_.push_back(
                        "profile sections became ready after " +
                        std::to_string(elapsed.count()) + " ms");
                }
                return true;
            }
            if (stop_token.stop_requested()) {
                diagnostics_.push_back(
                    "profile section readiness cancelled after " +
                    std::to_string(elapsed.count()) + " ms");
                return false;
            }
            if (elapsed >= timeout) {
                std::string diagnostic =
                    "profile section readiness timeout after " +
                    std::to_string(elapsed.count()) + " ms; missing=";
                for (std::size_t index{}; index < missing.size(); ++index) {
                    if (index != 0) diagnostic += ',';
                    diagnostic += missing[index];
                }
                diagnostics_.push_back(std::move(diagnostic));
                return true;
            }
            waited = true;
            std::this_thread::sleep_for((std::min)(poll_interval, timeout - elapsed));
        }
    }

    std::shared_ptr<const ProfileResolutionSnapshot> CurrentResolutionLocked() const {
        if (adapter_) {
            return std::make_shared<ProfileResolutionSnapshot>(adapter_->Resolution());
        }
        return resolution_;
    }

    NteProfileRuntimeOptions options_;
    std::shared_ptr<const SymbolMemory> memory_;
    mutable std::mutex mutex_;
    bool started_{};
    bool stopping_{};
    bool quarantined_{};
    bool tick_hook_ready_{};
    bool ahud_hook_ready_{};
    std::filesystem::path module_path_;
    BuildProfileCatalogSnapshot catalog_;
    std::optional<BuildProfile> profile_;
    std::shared_ptr<ProfileResolutionSnapshot> resolution_;
    std::shared_ptr<Ue5NteAdapter> adapter_;
    std::shared_ptr<TickEvidenceObserverGate> tick_evidence_gate_;
    std::unique_ptr<GameTickHook> tick_hook_;
    std::unique_ptr<Ue5ProcessEventHook> process_event_hook_;
    std::unique_ptr<Ue5OutboundBitCountProbe> outgoing_transform_probe_;
    std::vector<std::string> diagnostics_;
};

NteProfileRuntime::NteProfileRuntime(NteProfileRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
NteProfileRuntime::~NteProfileRuntime() {
    static_cast<void>(Stop(std::chrono::milliseconds::zero()));
}
bool NteProfileRuntime::Start(std::stop_token stop_token) noexcept {
    try {
        return impl_->Start(stop_token);
    } catch (...) {
        return false;
    }
}
bool NteProfileRuntime::Stop(std::chrono::milliseconds timeout) noexcept {
    return impl_->Stop(timeout);
}
bool NteProfileRuntime::Started() const noexcept { return impl_->Started(); }
std::optional<BuildFingerprint> NteProfileRuntime::Fingerprint() const { return impl_->Fingerprint(); }
std::shared_ptr<const ProfileResolutionSnapshot> NteProfileRuntime::Resolution() const {
    return impl_->Resolution();
}
std::shared_ptr<Ue5NteAdapter> NteProfileRuntime::Adapter() const { return impl_->Adapter(); }
NteProfileEvidenceSnapshot NteProfileRuntime::Evidence() const { return impl_->Evidence(); }
std::string NteProfileRuntime::DiagnosticsJson() const { return impl_->DiagnosticsJson(); }
std::vector<HookRecordView> NteProfileRuntime::Hooks() const { return impl_->Hooks(); }
std::string NteProfileRuntime::ExecuteReflectionQuery(const std::string_view request) const {
    return impl_->ExecuteReflectionQuery(request);
}

}  // namespace anomaly
