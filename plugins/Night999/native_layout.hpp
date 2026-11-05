#pragma once
#include "../common/combat/native_layout.hpp"
namespace night999_profile {
struct Layout {
    std::uint32_t kUeObjectClassOffset{};
    std::uint32_t kUeObjectNameOffset{};
    std::uint32_t kUeObjectOuterOffset{};
    std::uint32_t kUeStructSuperOffset{};
    std::uint32_t kUeStructPropertyLinkOffset{};
    std::uint32_t kUeFieldNextOffset{};
    std::uint32_t kUeFieldNameOffset{};
    std::uint32_t kUeFieldOffsetOffset{};
    std::uint32_t kUeFunctionParmsSizeOffset{};
    std::uint32_t kUeProcessEventVtableSlot{};
    std::uint32_t RegistryCount{};
    std::uint32_t RegistryChunks{};
    std::uint32_t RegistrySerial{};
    std::uint32_t ObjectIndex{};
    std::uint32_t ObjectsPerChunk{};
    std::uint32_t ObjectStride{};
    std::uint32_t RipDisplacement{};
    std::uint32_t RipSize{};

};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "night-combat-v1"}, {"ProcessEvent.entry"})) return false;
        Layout parsed;
        parsed.kUeObjectClassOffset = compatibility.Value("kUeObjectClassOffset", 65536);
        parsed.kUeObjectNameOffset = compatibility.Value("kUeObjectNameOffset", 65536);
        parsed.kUeObjectOuterOffset = compatibility.Value("kUeObjectOuterOffset", 65536);
        parsed.kUeStructSuperOffset = compatibility.Value("kUeStructSuperOffset", 65536);
        parsed.kUeStructPropertyLinkOffset = compatibility.Value("kUeStructPropertyLinkOffset", 65536);
        parsed.kUeFieldNextOffset = compatibility.Value("kUeFieldNextOffset", 65536);
        parsed.kUeFieldNameOffset = compatibility.Value("kUeFieldNameOffset", 65536);
        parsed.kUeFieldOffsetOffset = compatibility.Value("kUeFieldOffsetOffset", 65536);
        parsed.kUeFunctionParmsSizeOffset = compatibility.Value("kUeFunctionParmsSizeOffset", 65536);
        parsed.kUeProcessEventVtableSlot = compatibility.Value("kUeProcessEventVtableSlot", 65536);
        parsed.RegistryCount = compatibility.Value("RegistryCount", 65536);
        parsed.RegistryChunks = compatibility.Value("RegistryChunks", 65536);
        parsed.RegistrySerial = compatibility.Value("RegistrySerial", 65536);
        parsed.ObjectIndex = compatibility.Value("ObjectIndex", 65536);
        parsed.ObjectsPerChunk = compatibility.Value("ObjectsPerChunk", 65536);
        parsed.ObjectStride = compatibility.Value("ObjectStride", 65536);
        parsed.RipDisplacement = compatibility.Value("RipDisplacement", 65536);
        parsed.RipSize = compatibility.Value("RipSize", 65536);
        if (!parsed.ObjectsPerChunk || parsed.ObjectStride < parsed.RegistrySerial + sizeof(std::uint32_t) ||
            parsed.ObjectStride % sizeof(void*)) return false;

        if (!anomaly::plugins::combat::profile::Configure(host,compatibility)) return false;
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
