#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fake_uid_profile {
struct Layout {
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kFromStringResolveOffset{};
    std::uint32_t kFromStringInstructionSize{};
    std::uint32_t kProcessEventMatchOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kObjectRegistryItemsOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kObjectItemSerialOffset{};
    std::uint32_t kTextFieldOffset{};
    std::uint32_t kWidgetSlotOffset{};
    std::uint32_t kWidgetVisibilityOffset{};
    std::uint32_t kUFunctionFlagsOffset{};
    std::uint32_t kUFunctionFuncOffset{};
    std::uint32_t kUFunctionNumParmsOffset{};
    std::uint32_t kUFunctionParmsSizeOffset{};
    std::uint32_t kUFunctionReturnValueOffset{};
    std::uintptr_t kSetTextVtableOffset{};
    std::int32_t kGObjectsAddend{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "fake-uid-v1"}, {"ProcessEvent.entry"})) return false;
        Layout parsed;
        parsed.kObjectClassOffset = compatibility.Value("kObjectClassOffset",65536,8);
        parsed.kObjectOuterOffset = compatibility.Value("kObjectOuterOffset",65536,8);
        parsed.kFromStringResolveOffset = compatibility.Value("kFromStringResolveOffset",65536);
        parsed.kFromStringInstructionSize = compatibility.Value("kFromStringInstructionSize",65536);
        parsed.kProcessEventMatchOffset = compatibility.Value("kProcessEventMatchOffset",65536);
        parsed.kObjectNameOffset = compatibility.Value("kObjectNameOffset",65536);
        parsed.kObjectRegistryItemsOffset = compatibility.Value("kObjectRegistryItemsOffset",65536);
        parsed.kObjectChunkSize = compatibility.Value("kObjectChunkSize",65536);
        parsed.kObjectItemStride = compatibility.Value("kObjectItemStride",65536);
        parsed.kObjectItemSerialOffset = compatibility.Value("kObjectItemSerialOffset",65536);
        parsed.kTextFieldOffset = compatibility.Value("kTextFieldOffset",65536);
        parsed.kWidgetSlotOffset = compatibility.Value("kWidgetSlotOffset",65536);
        parsed.kWidgetVisibilityOffset = compatibility.Value("kWidgetVisibilityOffset",65536);
        parsed.kUFunctionFlagsOffset = compatibility.Value("kUFunctionFlagsOffset",65536);
        parsed.kUFunctionFuncOffset = compatibility.Value("kUFunctionFuncOffset",65536);
        parsed.kUFunctionNumParmsOffset = compatibility.Value("kUFunctionNumParmsOffset",65536);
        parsed.kUFunctionParmsSizeOffset = compatibility.Value("kUFunctionParmsSizeOffset",65536);
        parsed.kUFunctionReturnValueOffset = compatibility.Value("kUFunctionReturnValueOffset",65536);
        parsed.kSetTextVtableOffset = compatibility.Value("kSetTextVtableOffset",65536);
        parsed.kGObjectsAddend = -static_cast<std::int32_t>(compatibility.Value("kGObjectsAddendMagnitude",65536));
        if (parsed.kObjectChunkSize == 0 || parsed.kObjectItemStride < sizeof(void*) || parsed.kObjectItemSerialOffset + sizeof(std::int32_t) > parsed.kObjectItemStride) return false;
        native_layout=parsed; return true;
    } catch (...) { return false; }
}


// FakeUID owns this NTE-specific compatibility contract. The host supplies the
// generic signature scanner and UE services, but does not publish these details.
inline constexpr std::string_view kSetTextPattern = "kSetTextPattern";
inline constexpr std::string_view kAssignStringPattern = "kAssignStringPattern";
inline constexpr std::string_view kFromStringPattern = "kFromStringPattern";


inline constexpr std::string_view kFreeStringPattern = "kFreeStringPattern";
inline constexpr std::string_view kTextToStringPattern = "kTextToStringPattern";
inline constexpr std::string_view kGObjectsPattern = "kGObjectsPattern";
inline constexpr std::string_view kProcessEventPattern = "kProcessEventPattern";

inline constexpr std::uint32_t kGObjectsResolveOffset = 3;
inline constexpr std::uint32_t kGObjectsInstructionSize = 7;


}  // namespace fake_uid_profile
