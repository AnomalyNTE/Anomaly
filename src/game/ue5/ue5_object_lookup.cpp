#include "anomaly/ue5_object_lookup.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
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

bool Contains(
    const ue5mem::ModuleInfo& module,
    const std::uintptr_t address,
    const std::size_t size) noexcept {
    return address >= module.base && size <= module.size &&
        address - module.base <= module.size - size;
}

bool HasMatchingUnwindEntry(
    const ue5mem::ModuleInfo& module,
    const std::uintptr_t address) noexcept {
    if (!Contains(module, address, 1U)) return false;
    DWORD64 image_base{};
    const auto* const unwind = RtlLookupFunctionEntry(
        static_cast<DWORD64>(address), &image_base, nullptr);
    return unwind != nullptr && image_base == static_cast<DWORD64>(module.base) &&
        unwind->BeginAddress == address - module.base;
}

FeatureValidationResult ValidateStaticFindObjectAbi(
    const BuildProfile& profile,
    const std::string_view feature,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    if (feature != kUe5ObjectFindFeature ||
        !FeatureRequires(profile, feature, kUe5StaticFindObjectSymbol)) {
        return {false, "profile does not declare the UE5 object-find capability"};
    }

    const auto* const wrapper = snapshot.FindSymbol(kUe5StaticFindObjectSymbol);
    if (wrapper == nullptr || !wrapper->Available()) {
        return {false, "StaticFindObject symbol is unavailable"};
    }
    const auto module = memory.FindModule(wrapper->module);
    if (!module) return {false, "profile module is unavailable"};
    if (!HasMatchingUnwindEntry(*module, wrapper->address)) {
        return {false, "StaticFindObject wrapper has no matching unwind entry"};
    }

    constexpr auto kWrapperEntry = std::to_array<std::uint8_t>({
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
        0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xE9, 0x41,
        0x0F, 0xB6, 0xD9, 0x33, 0xC9, 0x49, 0x8B, 0xF8,
        0x48, 0x8B, 0xF2});
    constexpr std::uintptr_t kImplementationCallOffset = 0x4CU;
    if (!MatchesBytes(memory, wrapper->address, kWrapperEntry)) {
        return {false, "StaticFindObject wrapper instruction contract changed"};
    }

    std::uint8_t opcode{};
    std::int32_t displacement{};
    if (!memory.Read(
            wrapper->address + kImplementationCallOffset, &opcode, sizeof(opcode)) ||
        opcode != 0xE8U ||
        !memory.Read(
            wrapper->address + kImplementationCallOffset + 1U,
            &displacement,
            sizeof(displacement))) {
        return {false, "StaticFindObject implementation call is unreadable"};
    }
    const auto call_end = wrapper->address + kImplementationCallOffset + 5U;
    std::uintptr_t target{};
    if (displacement < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-
            static_cast<std::int64_t>(displacement));
        if (magnitude >= call_end) {
            return {false, "StaticFindObject implementation target overflows"};
        }
        target = call_end - static_cast<std::uintptr_t>(magnitude);
    } else if (static_cast<std::uint64_t>(displacement) >
        (std::numeric_limits<std::uintptr_t>::max)() - call_end) {
        return {false, "StaticFindObject implementation target overflows"};
    } else {
        target = call_end + static_cast<std::uintptr_t>(displacement);
    }
    if (!Contains(*module, target, 45U) || !HasMatchingUnwindEntry(*module, target)) {
        return {false, "StaticFindObject implementation has no matching unwind entry"};
    }

    constexpr auto kImplementationEntry = std::to_array<std::uint8_t>({
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x81, 0xEC, 0x80, 0x04, 0x00, 0x00, 0x48,
        0x8B, 0x05});
    constexpr auto kImplementationStackCookie = std::to_array<std::uint8_t>({
        0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24, 0x70,
        0x04, 0x00, 0x00, 0x48, 0x83, 0xFA, 0xFF});
    if (!MatchesBytes(memory, target, kImplementationEntry) ||
        !MatchesBytes(memory, target + 0x1EU, kImplementationStackCookie)) {
        return {false, "StaticFindObject implementation contract changed"};
    }
    return {true, {}};
}

}  // namespace

void RegisterUe5ObjectLookupValidator(FeatureLayoutValidatorRegistry& validators) {
    validators.Register(
        std::string(kUe5StaticFindObjectAbiValidator),
        ValidateStaticFindObjectAbi);
}

Ue5ObjectLookup CreateUe5ObjectLookup(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    const SymbolMemory& memory) {
    if (!ValidateStaticFindObjectAbi(
            profile, kUe5ObjectFindFeature, resolution, memory).valid) {
        return {};
    }
    const auto* const symbol = resolution.FindSymbol(kUe5StaticFindObjectSymbol);
    if (symbol == nullptr || !symbol->Available()) return {};

    using StaticFindObjectFn = void*(__fastcall*)(void*, void*, const wchar_t*, bool);
    const auto find_object = reinterpret_cast<StaticFindObjectFn>(symbol->address);
    return [find_object](const wchar_t* const full_path) -> std::uintptr_t {
        if (full_path == nullptr) return 0;
        return reinterpret_cast<std::uintptr_t>(
            find_object(nullptr, nullptr, full_path, false));
    };
}

}  // namespace anomaly
