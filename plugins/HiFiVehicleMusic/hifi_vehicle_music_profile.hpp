#pragma once

#include "anomaly/sdk/signature.hpp"
#include <anomaly/sdk/profile.hpp>
#include <array>

namespace hifi_vehicle_music_profile {
inline anomaly::sdk::ProfileSnapshot Compatibility;
inline constexpr std::string_view kModule = "HTGame.exe";
inline constexpr std::string_view kSection = ".text";
inline constexpr std::uint8_t kPlayerTypeVehicle = 1;
inline constexpr std::string_view kDiscTextureParameter = "Tex1";
inline constexpr std::string_view kPlayerDiscImageName = "AlbumCoverImg";
inline constexpr std::string_view kPlayerViewClassName = "BPUI_MusicDetailedView_C";
inline constexpr std::string_view kSetTextureParameterFunction = "SetTextureParameterValue";
inline constexpr std::uint32_t kPlayingId = 0x7FFFFF00;
inline constexpr std::uint32_t kRootSetFlag = 0x40000000;
inline constexpr std::string_view kRenderingLibraryClass = "KismetRenderingLibrary";
inline constexpr std::string_view kImportBufferFunction = "ImportBufferAsTexture2D";
inline constexpr std::uint32_t kSongNumberBase = 0x48460000;
inline constexpr std::uint32_t kMaxSongs = 0xFFFE;
inline constexpr std::uint32_t kAlbumNumber = 0x4846FFFF;
inline constexpr std::uint32_t kMetingSongNumberBase = 0x48470000;
inline constexpr std::uint32_t kMetingAlbumNumber = 0x4847FFFF;
inline constexpr std::int32_t kSortIndexBase = 0x7FFF0000;
struct NativeLayout final {
    std::uint32_t kPawnControllerOffset{};
    std::uint32_t kPlayerControllerPlayerOffset{};
    std::uint32_t kPlayerPlayerControllerOffset{};
    std::uint32_t kSubsystemPlayerTypeOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kSubsystemOwnedIdsOffset{};
    std::uint32_t kSubsystemCurrentIdOffset{};
    std::uint32_t kDurationInfoSecondsOffset{};
    std::uint32_t kUiDurationCacheOffset{};
    std::uint32_t kUiDurationLabelOffset{};
    std::uint32_t kUiOuterOffset{};
    std::uint32_t kImageBrushResourceOffset{};
    std::uint32_t kMaterialTextureParametersOffset{};
    std::uint32_t kTextureParameterSize{};
    std::uint32_t kTextureParameterValueOffset{};
    std::uint32_t kProcessEventVtableIndex{};
    std::uint32_t kUiOwnerPanelOffset{};
    std::uint32_t kUiOwnerStripOffset{};
    std::uint32_t kUiStripDurationLabelOffset{};
    std::uint32_t kUiStripDurationCacheOffset{};
    std::uint32_t kSubsystemPlayingIdOffset{};
    std::uint32_t kSubsystemPausedOffset{};
    std::uint32_t kDetailCoverImageOffset{};
    std::uint32_t kPageCoverImageOffset{};
    std::uint32_t kPageItemAlbumIdOffset{};
    std::uint32_t kImageSetBrushFromTextureSlot{};
    std::uint32_t kVehicleItemSongIdOffset{};
    std::uint32_t kObjectArrayRelOffset{};
    std::uint32_t kObjectArrayInstructionSize{};
    std::uint32_t kObjectArrayCountOffset{};
    std::uint32_t kObjectsPerChunk{};
    std::uint32_t kObjectItemSize{};
    std::uint32_t kObjectItemSerialOffset{};
    std::uint32_t kObjectItemFlagsOffset{};
    std::uint32_t kSerialCounterRelOffset{};
    std::uint32_t kSerialCounterInstructionEnd{};
    std::uint32_t kObjectIndexOffset{};
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kSoftPtrSize{};
    std::uint32_t kSoftPtrAssetPathOffset{};
    std::uint32_t kSoftPtrSubPathOffset{};
    std::uint32_t kFTextSize{};
    std::uint32_t kFStringSize{};
    std::uint32_t kMusicRowSize{};
    std::uint32_t kMusicTitleOffset{};
    std::uint32_t kMusicSortIndexOffset{};
    std::uint32_t kMusicCommentOffset{};
    std::uint32_t kMusicDescriptionOffset{};
    std::uint32_t kMusicAlbumIdOffset{};
    std::uint32_t kMusicEventOffset{};
    std::uint32_t kMusicEventSize{};
    std::uint32_t kMusicEventSubPathOffset{};
    std::uint32_t kMusicSourceItemOffset{};
    std::uint32_t kMusicSourceQuestOffset{};
    std::uint32_t kMusicIsListOffset{};
    std::uint32_t kMusicIsHiddenOffset{};
    std::uint32_t kAlbumRowSize{};
    std::uint32_t kAlbumNameOffset{};
    std::uint32_t kAlbumSortIndexOffset{};
    std::uint32_t kAlbumCoverOffset{};
    std::uint32_t kAlbumDescriptionOffset{};
    std::uint32_t kFunctionNumParms{};
    std::uint32_t kFunctionParmsSize{};
    std::uint32_t kFunctionReturnOffset{};
    std::uint32_t kPlayerDiscImageOffset{};
    std::uint32_t kGameInstanceMusicTableOffset{};
    std::uint32_t kGameInstanceAlbumTableOffset{};
};
inline NativeLayout Layout;
inline constexpr std::string_view kPostPattern = "kPostPattern";
inline constexpr std::string_view kStopPattern = "kStopPattern";
inline constexpr std::string_view kPausePattern = "kPausePattern";
inline constexpr std::string_view kResumePattern = "kResumePattern";
inline constexpr std::string_view kSetPlayerTypePattern = "kSetPlayerTypePattern";
inline constexpr std::string_view kEndGetOffPattern = "kEndGetOffPattern";
inline constexpr std::string_view kGameInstancePattern = "kGameInstancePattern";
inline constexpr std::string_view kFindRowCallPattern = "kFindRowCallPattern";
inline constexpr std::string_view kForEachRowCallPattern = "kForEachRowCallPattern";
inline constexpr std::string_view kAlbumForEachCallPattern = "kAlbumForEachCallPattern";
inline constexpr std::string_view kAlbumRowPattern = "kAlbumRowPattern";
inline constexpr std::string_view kAddUniquePattern = "kAddUniquePattern";
inline constexpr std::string_view kOwnedCopyPattern = "kOwnedCopyPattern";
inline constexpr std::string_view kReGeneratePattern = "kReGeneratePattern";
inline constexpr std::string_view kSetCurrentPattern = "kSetCurrentPattern";
inline constexpr std::string_view kItemRefreshPattern = "kItemRefreshPattern";
inline constexpr std::string_view kVehiclePanelPattern = "kVehiclePanelPattern";
inline constexpr std::string_view kResolveCurrentPattern = "kResolveCurrentPattern";
inline constexpr std::string_view kSyncCurrentPattern = "kSyncCurrentPattern";
inline constexpr std::string_view kPageListPattern = "kPageListPattern";
inline constexpr std::string_view kMusicEndPattern = "kMusicEndPattern";
inline constexpr std::string_view kSetCurrentIdPattern = "kSetCurrentIdPattern";
inline constexpr std::string_view kPositionFractionPattern = "kPositionFractionPattern";
inline constexpr std::string_view kPositionSecondsPattern = "kPositionSecondsPattern";
inline constexpr std::string_view kDurationCallbackPattern = "kDurationCallbackPattern";
inline constexpr std::string_view kDurationCallback2Pattern = "kDurationCallback2Pattern";
inline constexpr std::string_view kChangeSoundPattern = "kChangeSoundPattern";
inline constexpr std::string_view kEntryClickPattern = "kEntryClickPattern";
inline constexpr std::string_view kDetailCoverPattern = "kDetailCoverPattern";
inline constexpr std::string_view kPageCoverPattern = "kPageCoverPattern";
inline constexpr std::string_view kItemSelectedPattern = "kItemSelectedPattern";
inline constexpr std::string_view kRowClickPattern = "kRowClickPattern";
inline constexpr std::string_view kListClickPattern = "kListClickPattern";
inline constexpr std::string_view kSoundSubsystemCallPattern = "kSoundSubsystemCallPattern";
inline constexpr std::string_view kImportTexturePattern = "kImportTexturePattern";
inline constexpr std::string_view kSetItemFlagsPattern = "kSetItemFlagsPattern";
inline constexpr std::string_view kObjectArrayPattern = "kObjectArrayPattern";
inline constexpr std::string_view kSerialCounterPattern = "kSerialCounterPattern";
inline constexpr std::string_view kSetListItemPattern = "kSetListItemPattern";
inline constexpr AnomalySignatureMemberV2 kPauseMembers[]{
    anomaly::sdk::MemberUse(0, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2),
    anomaly::sdk::MemberUse(1, 1, ANOMALY_SIGNATURE_MEMBER_WRITE_V2),
};
inline constexpr AnomalySignatureMemberV2 kPlayerTypeMembers[]{
    anomaly::sdk::MemberUse(2, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2 | ANOMALY_SIGNATURE_MEMBER_ADDRESS_V2),
};
inline constexpr AnomalySignatureMemberV2 kPlayerTypeReadMembers[]{
    anomaly::sdk::MemberUse(2, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2),
};
inline constexpr AnomalySignatureMemberV2 kCurrentIdMembers[]{
    anomaly::sdk::MemberUse(0, 8, ANOMALY_SIGNATURE_MEMBER_READ_V2 | ANOMALY_SIGNATURE_MEMBER_WRITE_V2),
};
inline constexpr AnomalySignatureMemberV2 kSyncCurrentMembers[]{
    anomaly::sdk::MemberUse(0, 8, ANOMALY_SIGNATURE_MEMBER_READ_V2),
};
inline constexpr AnomalySignatureMemberV2 kOwnedMembers[]{
    // TArray::Num is eight bytes after the array base.
    anomaly::sdk::MemberUse(1, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2, 8),
};
inline constexpr AnomalySignatureMemberV2 kCurrentLowMembers[]{
    anomaly::sdk::MemberUse(0, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2),
};
inline constexpr AnomalySignatureMemberV2 kResolveTypeMembers[]{
    anomaly::sdk::MemberUse(2, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2),
};
struct PlaybackSchema final {
    std::array<AnomalySignatureFieldV2, 3> fields{{
    anomaly::sdk::FieldSeed("PlayingID", Layout.kSubsystemPlayingIdOffset, 4),
    anomaly::sdk::FieldSeed("Paused", Layout.kSubsystemPausedOffset),
    anomaly::sdk::FieldSeed("PlayerType", Layout.kSubsystemPlayerTypeOffset, 4),
    }};
    std::array<AnomalySignatureWitnessV2, 4> witnesses{{
    anomaly::sdk::Witness("pause", Compatibility.Pattern(kPausePattern), kPauseMembers),
    anomaly::sdk::Witness("resume", Compatibility.Pattern(kResumePattern), kPauseMembers),
    anomaly::sdk::Witness("player-type", Compatibility.Pattern(kSetPlayerTypePattern), kPlayerTypeMembers),
    anomaly::sdk::Witness("player-type-read", Compatibility.Pattern(kResolveCurrentPattern), kPlayerTypeReadMembers),
    }};
    anomaly::sdk::LayoutSchema View() const { return {kModule, kSection, fields, witnesses}; }
};
struct AlbumSchema final {
    std::array<AnomalySignatureFieldV2, 3> fields{{
    anomaly::sdk::FieldSeed("CurrentID", Layout.kSubsystemCurrentIdOffset, 8),
    anomaly::sdk::FieldSeed("OwnedIDs", Layout.kSubsystemOwnedIdsOffset, 8),
    anomaly::sdk::FieldSeed("PlayerType", Layout.kSubsystemPlayerTypeOffset, 4),
    }};
    std::array<AnomalySignatureWitnessV2, 6> witnesses{{
    anomaly::sdk::Witness("current-id", Compatibility.Pattern(kSetCurrentIdPattern), kCurrentIdMembers),
    anomaly::sdk::Witness("sync-current", Compatibility.Pattern(kSyncCurrentPattern), kSyncCurrentMembers),
    anomaly::sdk::Witness("owned-copy", Compatibility.Pattern(kOwnedCopyPattern), kOwnedMembers),
    anomaly::sdk::Witness("change-sound", Compatibility.Pattern(kChangeSoundPattern), kCurrentLowMembers),
    anomaly::sdk::Witness("player-type", Compatibility.Pattern(kSetPlayerTypePattern), kPlayerTypeMembers),
    anomaly::sdk::Witness("resolve-current", Compatibility.Pattern(kResolveCurrentPattern), kResolveTypeMembers),
    }};
    anomaly::sdk::LayoutSchema View() const { return {kModule, kSection, fields, witnesses}; }
};
inline bool Load(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!Compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "hifi-vehicle-music-v1"},
                                {"ProcessEvent.entry"}, true, "layout")) return false;
        NativeLayout next;
        next.kPawnControllerOffset = Compatibility.Value("kPawnControllerOffset", 65536, 1);
        next.kPlayerControllerPlayerOffset = Compatibility.Value("kPlayerControllerPlayerOffset", 65536, 1);
        next.kPlayerPlayerControllerOffset = Compatibility.Value("kPlayerPlayerControllerOffset", 65536, 1);
        next.kSubsystemPlayerTypeOffset = Compatibility.Value("kSubsystemPlayerTypeOffset", 65536, 1);
        next.kObjectNameOffset = Compatibility.Value("kObjectNameOffset", 65536, 1);
        next.kSubsystemOwnedIdsOffset = Compatibility.Value("kSubsystemOwnedIdsOffset", 65536, 1);
        next.kSubsystemCurrentIdOffset = Compatibility.Value("kSubsystemCurrentIdOffset", 65536, 1);
        next.kDurationInfoSecondsOffset = Compatibility.Value("kDurationInfoSecondsOffset", 65536, 1);
        next.kUiDurationCacheOffset = Compatibility.Value("kUiDurationCacheOffset", 65536, 1);
        next.kUiDurationLabelOffset = Compatibility.Value("kUiDurationLabelOffset", 65536, 8);
        next.kUiOuterOffset = Compatibility.Value("kUiOuterOffset", 65536, 1);
        next.kImageBrushResourceOffset = Compatibility.Value("kImageBrushResourceOffset", 65536, 1);
        next.kMaterialTextureParametersOffset = Compatibility.Value("kMaterialTextureParametersOffset", 65536, 1);
        next.kTextureParameterSize = Compatibility.Value("kTextureParameterSize", 65536, 8);
        next.kTextureParameterValueOffset = Compatibility.Value("kTextureParameterValueOffset", 65536, 1);
        next.kProcessEventVtableIndex = Compatibility.Value("kProcessEventVtableIndex", 65536, 1);
        next.kUiOwnerPanelOffset = Compatibility.Value("kUiOwnerPanelOffset", 65536, 8);
        next.kUiOwnerStripOffset = Compatibility.Value("kUiOwnerStripOffset", 65536, 8);
        next.kUiStripDurationLabelOffset = Compatibility.Value("kUiStripDurationLabelOffset", 65536, 1);
        next.kUiStripDurationCacheOffset = Compatibility.Value("kUiStripDurationCacheOffset", 65536, 1);
        next.kSubsystemPlayingIdOffset = Compatibility.Value("kSubsystemPlayingIdOffset", 65536, 1);
        next.kSubsystemPausedOffset = Compatibility.Value("kSubsystemPausedOffset", 65536, 1);
        next.kDetailCoverImageOffset = Compatibility.Value("kDetailCoverImageOffset", 65536, 1);
        next.kPageCoverImageOffset = Compatibility.Value("kPageCoverImageOffset", 65536, 1);
        next.kPageItemAlbumIdOffset = Compatibility.Value("kPageItemAlbumIdOffset", 65536, 1);
        next.kImageSetBrushFromTextureSlot = Compatibility.Value("kImageSetBrushFromTextureSlot", 65536, 1);
        next.kVehicleItemSongIdOffset = Compatibility.Value("kVehicleItemSongIdOffset", 65536, 1);
        next.kObjectArrayRelOffset = Compatibility.Value("kObjectArrayRelOffset", 65536, 1);
        next.kObjectArrayInstructionSize = Compatibility.Value("kObjectArrayInstructionSize", 65536, 1);
        next.kObjectArrayCountOffset = Compatibility.Value("kObjectArrayCountOffset", 65536, 1);
        next.kObjectsPerChunk = Compatibility.Value("kObjectsPerChunk", 65536, 1);
        next.kObjectItemSize = Compatibility.Value("kObjectItemSize", 65536, 8);
        next.kObjectItemSerialOffset = Compatibility.Value("kObjectItemSerialOffset", 65536, 1);
        next.kObjectItemFlagsOffset = Compatibility.Value("kObjectItemFlagsOffset", 65536, 1);
        next.kSerialCounterRelOffset = Compatibility.Value("kSerialCounterRelOffset", 65536, 1);
        next.kSerialCounterInstructionEnd = Compatibility.Value("kSerialCounterInstructionEnd", 65536, 1);
        next.kObjectIndexOffset = Compatibility.Value("kObjectIndexOffset", 65536, 1);
        next.kObjectClassOffset = Compatibility.Value("kObjectClassOffset", 65536, 1);
        next.kObjectOuterOffset = Compatibility.Value("kObjectOuterOffset", 65536, 1);
        next.kSoftPtrSize = Compatibility.Value("kSoftPtrSize", 65536, 1);
        next.kSoftPtrAssetPathOffset = Compatibility.Value("kSoftPtrAssetPathOffset", 65536, 1);
        next.kSoftPtrSubPathOffset = Compatibility.Value("kSoftPtrSubPathOffset", 65536, 1);
        next.kFTextSize = Compatibility.Value("kFTextSize", 65536, 1);
        next.kFStringSize = Compatibility.Value("kFStringSize", 65536, 1);
        next.kMusicRowSize = Compatibility.Value("kMusicRowSize", 65536, 1);
        next.kMusicTitleOffset = Compatibility.Value("kMusicTitleOffset", 65536, 1);
        next.kMusicSortIndexOffset = Compatibility.Value("kMusicSortIndexOffset", 65536, 1);
        next.kMusicCommentOffset = Compatibility.Value("kMusicCommentOffset", 65536, 1);
        next.kMusicDescriptionOffset = Compatibility.Value("kMusicDescriptionOffset", 65536, 1);
        next.kMusicAlbumIdOffset = Compatibility.Value("kMusicAlbumIdOffset", 65536, 1);
        next.kMusicEventOffset = Compatibility.Value("kMusicEventOffset", 65536, 1);
        next.kMusicEventSize = Compatibility.Value("kMusicEventSize", 65536, 1);
        next.kMusicEventSubPathOffset = Compatibility.Value("kMusicEventSubPathOffset", 65536, 1);
        next.kMusicSourceItemOffset = Compatibility.Value("kMusicSourceItemOffset", 65536, 1);
        next.kMusicSourceQuestOffset = Compatibility.Value("kMusicSourceQuestOffset", 65536, 1);
        next.kMusicIsListOffset = Compatibility.Value("kMusicIsListOffset", 65536, 1);
        next.kMusicIsHiddenOffset = Compatibility.Value("kMusicIsHiddenOffset", 65536, 1);
        next.kAlbumRowSize = Compatibility.Value("kAlbumRowSize", 65536, 1);
        next.kAlbumNameOffset = Compatibility.Value("kAlbumNameOffset", 65536, 1);
        next.kAlbumSortIndexOffset = Compatibility.Value("kAlbumSortIndexOffset", 65536, 1);
        next.kAlbumCoverOffset = Compatibility.Value("kAlbumCoverOffset", 65536, 1);
        next.kAlbumDescriptionOffset = Compatibility.Value("kAlbumDescriptionOffset", 65536, 1);
        next.kFunctionNumParms = Compatibility.Value("kFunctionNumParms", 65536, 1);
        next.kFunctionParmsSize = Compatibility.Value("kFunctionParmsSize", 65536, 1);
        next.kFunctionReturnOffset = Compatibility.Value("kFunctionReturnOffset", 65536, 1);
        next.kPlayerDiscImageOffset = Compatibility.Value("kPlayerDiscImageOffset", 65536, 8);
        next.kGameInstanceMusicTableOffset = Compatibility.Value("kGameInstanceMusicTableOffset", 65536, 8);
        next.kGameInstanceAlbumTableOffset = Compatibility.Value("kGameInstanceAlbumTableOffset", 65536, 8);
        if (next.kObjectsPerChunk == 0 || next.kObjectItemSize < next.kObjectItemSerialOffset + sizeof(std::int32_t) ||
            next.kObjectItemSize < next.kObjectItemFlagsOffset + sizeof(std::int32_t) ||
            next.kTextureParameterSize < next.kTextureParameterValueOffset + sizeof(void*) ||
            next.kFTextSize != 16 || next.kFStringSize != 16 ||
            next.kUiDurationLabelOffset + sizeof(void*) > next.kUiDurationCacheOffset ||
            next.kDurationInfoSecondsOffset + sizeof(float) > 96) return false;
        const auto fits = [](std::uint32_t offset, std::size_t width, std::uint32_t size) {
            return offset <= size && width <= size - offset;
        };
        if (!fits(next.kMusicTitleOffset, 16, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicSortIndexOffset, 4, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicCommentOffset, 16, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicDescriptionOffset, 16, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicAlbumIdOffset, 8, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicEventOffset, 40, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicSourceItemOffset, 8, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicSourceQuestOffset, 8, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicIsListOffset, 1, next.kMusicRowSize)) return false;
        if (!fits(next.kMusicIsHiddenOffset, 1, next.kMusicRowSize)) return false;
        if (!fits(next.kAlbumNameOffset, 16, next.kAlbumRowSize)) return false;
        if (!fits(next.kAlbumSortIndexOffset, 4, next.kAlbumRowSize)) return false;
        if (!fits(next.kAlbumCoverOffset, 40, next.kAlbumRowSize)) return false;
        if (!fits(next.kAlbumDescriptionOffset, 16, next.kAlbumRowSize)) return false;
        if (!fits(next.kMusicEventSubPathOffset, next.kFStringSize, next.kMusicEventSize)) return false;
        if (const auto* detail = Compatibility.Selected(kDetailCoverPattern);
            detail && (detail->values.find("albumIndirect") == detail->values.end() || detail->values.at("albumIndirect") > 1)) return false;
        Layout = next;
        return true;
    } catch (...) { return false; }
}
} // namespace hifi_vehicle_music_profile
