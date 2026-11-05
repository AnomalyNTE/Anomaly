#pragma once
#include <anomaly/sdk/profile.hpp>
namespace anomaly::plugins::combat::profile {
struct Layout {
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kUStructChildrenOffset{};
    std::uint32_t kUStructSuperStructOffset{};
    std::uint32_t kUFieldNextOffset{};
    std::uint32_t kUFunctionNumParmsOffset{};
    std::uint32_t kUFunctionParmsSizeOffset{};
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerPlayerStateOffset{};
    std::uint32_t kProcessEventVtableIndex{};
    std::uint32_t kDataTableRowMapOffset{};
    std::uint32_t kDataTableRowStride{};
    std::uint32_t kUStructPropertyLinkOffset{};
    std::uint32_t kFFieldNameOffset{};
    std::uint32_t kFFieldClassOffset{};
    std::uint32_t kFPropertyElementSizeOffset{};
    std::uint32_t kFPropertyOffsetInternalOffset{};
    std::uint32_t kFPropertyPropertyLinkNextOffset{};
    std::uint32_t kArrayNumOffset{};
    std::uint32_t kArrayMaxOffset{};
    std::uint32_t kRowPointerOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline const AnomalyCoreServiceV1* core{};
inline bool Configure(const AnomalyHostApiV1* host, const anomaly::sdk::ProfileSnapshot& source) noexcept {
    try {
        Layout parsed;
        parsed.kObjectClassOffset=source.Value("combat.kObjectClassOffset",65536);
        parsed.kObjectNameOffset=source.Value("combat.kObjectNameOffset",65536);
        parsed.kUStructChildrenOffset=source.Value("combat.kUStructChildrenOffset",65536);
        parsed.kUStructSuperStructOffset=source.Value("combat.kUStructSuperStructOffset",65536);
        parsed.kUFieldNextOffset=source.Value("combat.kUFieldNextOffset",65536);
        parsed.kUFunctionNumParmsOffset=source.Value("combat.kUFunctionNumParmsOffset",65536);
        parsed.kUFunctionParmsSizeOffset=source.Value("combat.kUFunctionParmsSizeOffset",65536);
        parsed.kWorldGameInstanceOffset=source.Value("combat.kWorldGameInstanceOffset",65536);
        parsed.kGameInstanceLocalPlayersOffset=source.Value("combat.kGameInstanceLocalPlayersOffset",65536);
        parsed.kLocalPlayerControllerOffset=source.Value("combat.kLocalPlayerControllerOffset",65536);
        parsed.kControllerPlayerStateOffset=source.Value("combat.kControllerPlayerStateOffset",65536);
        parsed.kProcessEventVtableIndex=source.Value("combat.kProcessEventVtableIndex",65536);
        parsed.kDataTableRowMapOffset=source.Value("combat.kDataTableRowMapOffset",65536);
        parsed.kDataTableRowStride=source.Value("combat.kDataTableRowStride",65536);
        parsed.kUStructPropertyLinkOffset=source.Value("combat.kUStructPropertyLinkOffset",65536);
        parsed.kFFieldNameOffset=source.Value("combat.kFFieldNameOffset",65536);
        parsed.kFFieldClassOffset=source.Value("combat.kFFieldClassOffset",65536);
        parsed.kFPropertyElementSizeOffset=source.Value("combat.kFPropertyElementSizeOffset",65536);
        parsed.kFPropertyOffsetInternalOffset=source.Value("combat.kFPropertyOffsetInternalOffset",65536);
        parsed.kFPropertyPropertyLinkNextOffset=source.Value("combat.kFPropertyPropertyLinkNextOffset",65536);
        parsed.kArrayNumOffset=source.Value("combat.kArrayNumOffset",65536);
        parsed.kArrayMaxOffset=source.Value("combat.kArrayMaxOffset",65536);
        parsed.kRowPointerOffset=source.Value("combat.kRowPointerOffset",65536);
        if (!parsed.kDataTableRowStride || parsed.kRowPointerOffset+sizeof(void*)>parsed.kDataTableRowStride ||
            parsed.kProcessEventVtableIndex>1024) return false;
        compatibility=source;
        core=sdk::Host(host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
