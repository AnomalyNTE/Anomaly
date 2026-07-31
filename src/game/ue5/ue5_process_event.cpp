#include "anomaly/ue5_process_event.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace anomaly {
namespace {

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

FeatureValidationResult ValidateProcessEventAbi(
    const BuildProfile& profile,
    const std::string_view feature,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    if (feature != kUe5ProcessEventFeature ||
        !FeatureRequires(profile, feature, kUe5ProcessEventSymbol)) {
        return {false, "profile does not declare the UE5 ProcessEvent capability"};
    }

    const auto* const process_event = snapshot.FindSymbol(kUe5ProcessEventSymbol);
    if (process_event == nullptr || !process_event->Available()) {
        return {false, "ProcessEvent symbol is unavailable"};
    }
    const auto module = memory.FindModule(process_event->module);
    if (!module) return {false, "profile module is unavailable"};
    if (!HasMatchingUnwindEntry(*module, *process_event)) {
        return {false, "ProcessEvent has no matching unwind entry"};
    }

    constexpr auto kPrologue = std::to_array<std::uint8_t>({
        0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x00,
        0x01, 0x00, 0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30,
        0x48, 0x89, 0x9D, 0x28, 0x01, 0x00, 0x00});
    constexpr auto kArgumentSetup = std::to_array<std::uint8_t>({
        0x48, 0x33, 0xC5, 0x48, 0x89, 0x85, 0xC0, 0x00,
        0x00, 0x00, 0x8B, 0x41, 0x08, 0x4D, 0x8B, 0xF0,
        0xC1, 0xE8, 0x1E, 0x48, 0x8B, 0xFA, 0xF6, 0xD0,
        0x4C, 0x8B, 0xF9, 0xA8, 0x01});
    constexpr auto kOutParmSetup = std::to_array<std::uint8_t>({
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
    if (!MatchesBytes(memory, process_event->address, kPrologue) ||
        !MatchesBytes(memory, process_event->address + 0x26U, kArgumentSetup) ||
        !MatchesBytes(memory, process_event->address + 0x20FU, kOutParmSetup)) {
        return {false, "ProcessEvent ABI instruction contract changed"};
    }
    return {true, {}};
}

}  // namespace

FeatureLayoutValidatorRegistry Ue5FeatureLayoutValidators() {
    FeatureLayoutValidatorRegistry validators;
    validators.Register(std::string(kUe5ProcessEventAbiValidator), ValidateProcessEventAbi);
    return validators;
}

Ue5ProcessEventInvoker CreateUe5ProcessEventInvoker(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    const SymbolMemory& memory) {
    if (!ValidateProcessEventAbi(
            profile, kUe5ProcessEventFeature, resolution, memory).valid) {
        return {};
    }
    const auto* const process_event = resolution.FindSymbol(kUe5ProcessEventSymbol);
    if (process_event == nullptr || !process_event->Available()) return {};

    using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
    const auto invoke = reinterpret_cast<ProcessEventFn>(process_event->address);
    return [invoke](
               const std::uintptr_t object,
               const std::uintptr_t function,
               void* const parameters,
               const std::size_t parameter_size) {
        if (object == 0 || function == 0 || parameters == nullptr || parameter_size == 0 ||
            parameter_size > 4096U) {
            return false;
        }
        invoke(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
        return true;
    };
}

}  // namespace anomaly
