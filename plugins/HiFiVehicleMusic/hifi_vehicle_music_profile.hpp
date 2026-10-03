#pragma once

#include <cstdint>
#include <string_view>

namespace hifi_vehicle_music_profile {

// Player-music entry points in HTGame 1.4a (UE 5.6.1). All are resolved in
// HTGame.exe .text; any miss disables the takeover.
inline constexpr std::string_view kModule = "HTGame.exe";
inline constexpr std::string_view kSection = ".text";

// __int64 PostPlayerMusicSound(this, UAkAudioEvent* event, FName* list_id, float position)
inline constexpr std::string_view kPostPattern =
    "48 8B C4 4C 89 40 18 55 53 56 57 41 54 41 56 41 57 48 8D 68 88 48 81 EC 40 01 00 00 "
    "0F 29 70 B8";
// void StopPlayerMusicSound(this, uint8 no_transition)
inline constexpr std::string_view kStopPattern = "48 83 EC 28 0F B6 C2 48 89 5C 24 38";
// void PausePlayerMusicSound(this) / ResumePlayerMusicSound(this): identical
// bodies except for the value written to the Paused flag.
inline constexpr std::string_view kPausePattern =
    "40 53 48 83 EC 20 8B 91 20 04 00 00 48 8B D9 85 D2 7E ?? 33 C9 41 B1 04 44 8D 41 64 "
    "E8 ?? ?? ?? ?? 48 8D 8B 80 03 00 00 C6 83 24 04 00 00 01";
inline constexpr std::string_view kResumePattern =
    "40 53 48 83 EC 20 8B 91 20 04 00 00 48 8B D9 85 D2 7E ?? 33 C9 41 B1 04 44 8D 41 64 "
    "E8 ?? ?? ?? ?? 48 8D 8B 80 03 00 00 C6 83 24 04 00 00 00";
// void SetMusicPlayerType(this, EHTPlayerMusicType type). On a type change it
// stops the current Wwise music by playing ID, bypassing StopPlayerMusicSound.
inline constexpr std::string_view kSetPlayerTypePattern =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 99 90 04 00 00 "
    "4C 89 74 24 30 48 8D B1 28 04 00 00 8B EA 48 8B F9 39 91 28 04 00 00";

// void AHTPlayerCharacter::EndGetOffVehicle(this, bool set_location_and_rotation)
// (vtable +0x15A0): runs when the character has left its seat. Resolved with
// the others, so a miss disables the takeover rather than leaking music.
inline constexpr std::string_view kEndGetOffPattern =
    "40 55 53 56 57 41 54 41 55 41 57 48 8D AC 24 10 FF FF FF 48 81 EC F0 01 00 00 "
    "44 0F B6 EA 48 8B F9 E8";
// APawn::Controller, APlayerController::Player and UPlayer::PlayerController:
// a player character's controller owns a UPlayer that points back at it.
inline constexpr std::uint32_t kPawnControllerOffset = 0x2F8;
inline constexpr std::uint32_t kPlayerControllerPlayerOffset = 0x368;
inline constexpr std::uint32_t kPlayerPlayerControllerOffset = 0x30;

// UHTSoundSubsystem::PlayerType (EHTPlayerMusicType); Vehicle == 1.
inline constexpr std::uint32_t kSubsystemPlayerTypeOffset = 0x428;
inline constexpr std::uint8_t kPlayerTypeVehicle = 1;
// UObject::NamePrivate.ComparisonIndex of the UAkAudioEvent.
inline constexpr std::uint32_t kObjectNameOffset = 0x18;

// In-game album holding the library. Optional: any miss keeps the game's
// music list unchanged (the takeover then never triggers).
//
// Patterns marked "call site" match an E8 rel32 that calls the function.
// UHTGameInstance* GetGameInstance()
inline constexpr std::string_view kGameInstancePattern =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 80 10 03 00 00 48 85 C0 74 ?? 48 8B 40 30 C3";
// UDataTable* of FPlayerMusicData / FMusicAlbumData rows.
inline constexpr std::uint32_t kGameInstanceMusicTableOffset = 0x1840;
inline constexpr std::uint32_t kGameInstanceAlbumTableOffset = 0x1848;
// (call site) FPlayerMusicData* FindRow(UDataTable*, FName row, const TCHAR* context, bool warn)
inline constexpr std::string_view kFindRowCallPattern =
    "E8 ?? ?? ?? ?? 48 8B 4C 24 40 48 8B F0 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 85 F6 74 ?? 48 8B 46 68";
// (call site) void ForeachRow<FPlayerMusicData>(UDataTable*, const TCHAR* context, Callback*)
// and the FMusicAlbumData instance. Callback is {fn, user}: fn(user, {FName, row}*, row).
inline constexpr std::string_view kForEachRowCallPattern =
    "E8 ?? ?? ?? ?? 48 8B 0F 48 85 C9 74 ?? E8 ?? ?? ?? ?? 44 0F B6 44 24 30";
inline constexpr std::string_view kAlbumForEachCallPattern =
    "E8 ?? ?? ?? ?? 48 8B 0B 48 85 C9 74 ?? E8 ?? ?? ?? ?? 44 0F B6 85 80 00 00 00";
// FMusicAlbumData* UHTGameInstance::GetAlbumData(this, const FName* album, bool warn)
inline constexpr std::string_view kAlbumRowPattern =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 99 48 18 00 00";
// int32 TArray<FName>::AddUnique(TArray<FName>* array, const FName* name)
inline constexpr std::string_view kAddUniquePattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 09 4C 8B C1 4C 63 4B 08";
// TArray<FName>* UHTSoundSubsystem::GetOwnedMusicListIDs(this, TArray<FName>* out)
inline constexpr std::string_view kOwnedCopyPattern =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B DA 33 D2 48 89 13 48 8D 43 0C "
    "48 63 B9 E8 03 00 00";
// void UHTSoundSubsystem::ReGenerateNewMusicListIDs(this): builds the play queue
// from the owned IDs (album mode filters FindRow(id)->AlbumID).
inline constexpr std::string_view kReGeneratePattern = "4C 8B DC 53 55 56 57 41 55 48 81 EC 90 00 00 00";
// void UHTSoundSubsystem::SetCurrentMusicListID(this, FName id): prunes the
// play queue against the owned IDs before inserting `id`.
inline constexpr std::string_view kSetCurrentPattern =
    "48 89 54 24 10 48 89 4C 24 08 53 55 41 56 48 81 EC 80 00 00 00 83 B9 10 04 00 00 00 "
    "4C 8D B1 08 04 00 00";
// UHTSoundSubsystem owned music list IDs, TArray<FName>.
inline constexpr std::uint32_t kSubsystemOwnedIdsOffset = 0x3E0;
// UHTSoundSubsystem::CurrentPlayerMusicListID (FName).
inline constexpr std::uint32_t kSubsystemCurrentIdOffset = 0x400;
// Further readers of the owned IDs that bypass GetOwnedMusicListIDs; without
// the song ids they show the album songs as locked (state 3) or move the
// current song back to a game song.
// void UHTUI_MusicListPanel::RefreshItemStates(this)
inline constexpr std::string_view kItemRefreshPattern =
    "41 57 48 81 EC 80 00 00 00 48 8B 81 40 10 00 00 4C 8B F9 48 85 C0 0F 84 ?? ?? ?? ?? "
    "8B 40 08 C1 E8 1E F6 D0 A8 01";
// char UHTUI_VehicleMusicPanel::RefreshList(this, void*, bool)
inline constexpr std::string_view kVehiclePanelPattern =
    "40 55 41 54 41 55 41 56 48 8D AC 24 B8 FE FF FF 48 81 EC 48 02 00 00";
// void UHTSoundSubsystem::ResolveCurrentMusicListID(this): falls back to the
// first owned ID when the current one is not owned.
inline constexpr std::string_view kResolveCurrentPattern =
    "40 56 48 83 EC 70 48 8B F1 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 8B 8E 28 04 00 00";
// void UHTSoundSubsystem::SyncCurrentMusicListID(this): same fallback.
inline constexpr std::string_view kSyncCurrentPattern =
    "40 53 57 48 83 EC 28 48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? "
    "48 8B 93 00 04 00 00";
// char UHTUI_MusicListPanel::RefreshPageList(this): builds the album page
// entries and their lock state from the owned IDs.
inline constexpr std::string_view kPageListPattern =
    "48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 00 FF FF FF "
    "48 81 EC 00 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F0 00 00 00 45 33 FF "
    "4C 8B E9 44 89 7D 88 48 8B 01";
// void UHTSoundSubsystem::PlayerMusicEndCallBack(this, uint8 cancelled, UAkCallbackInfo*):
// Wwise's end-of-event callback; advances CurrentPlayerMusicListID to the next
// queue entry and posts it.
inline constexpr std::string_view kMusicEndPattern =
    "84 D2 0F 85 ?? ?? ?? ?? 55 53 56 48 8D 6C 24 E0 48 81 EC 20 01 00 00 49 8B F0 48 8B D9 "
    "4D 85 C0 0F 84";
// void UHTSoundSubsystem::SetCurrentPlayerMusicListID(this, const FName* id):
// stores the id and broadcasts OnCurrentMusicIDChanged (the UI title follows).
// Optional: a miss leaves the game's title on the song it last picked.
inline constexpr std::string_view kSetCurrentIdPattern =
    "48 8B 02 48 39 81 00 04 00 00 74 13 48 89 81 00 04 00 00 48 81 C1 F0 02 00 00 E9";
// In-game player progress. The UI reads the position of PlayingID through
// these Wwise wrappers and skips it while PlayingID is 0; a taken-over library
// song therefore gets kPlayingId, which these detours answer from the engine.
// float GetPlayingFraction(uint32 playing_id) / float GetPlayingSeconds(uint32 playing_id)
inline constexpr std::string_view kPositionFractionPattern =
    "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 8B D9 E8 ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 32 80 3D ?? ?? ?? ?? 03 72 78";
inline constexpr std::string_view kPositionSecondsPattern =
    "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 8B D9 E8 ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 32 80 3D ?? ?? ?? ?? 03 72 77";
// void MusicDurationCallback({uint32 playing_id; UHTUI_MusicPlayer* ui}*, info*):
// the song length shown by the player, float seconds at info+0x58. Synthetic
// rows carry the template's event, so its duration is the template song's.
inline constexpr std::string_view kDurationCallbackPattern =
    "48 89 5C 24 10 57 48 83 EC 30 4C 8B 41 08 48 8B F9 8B 42 58 48 89 74 24 40 41 89 80 DC 10 00 00";
inline constexpr std::uint32_t kDurationInfoSecondsOffset = 0x58;
// The same length callback on the album view's play/resume path:
// void ({UHTUI_MusicPlayer* ui}*, info** info).
inline constexpr std::string_view kDurationCallback2Pattern =
    "40 57 48 83 EC 30 48 8B 02 48 8B 11 8B 40 58 89 82 DC 10 00 00 48 8B 09 48 8B B9 18 10 00 00";
// UHTSoundSubsystem::PlayingID (int32; the Pause/Resume/Stop paths test > 0).
inline constexpr std::uint32_t kSubsystemPlayingIdOffset = 0x420;
// UHTSoundSubsystem::bPaused (uint8; Pause writes 1, Resume 0).
inline constexpr std::uint32_t kSubsystemPausedOffset = 0x424;
// Never issued by Wwise in practice (IDs count up from 1); Wwise ignores it.
inline constexpr std::uint32_t kPlayingId = 0x7FFFFF00;
// void UHTSoundSubsystem::ChangePlayerMusicSound(this, float position): the
// progress slider's seek. It reloads the row's event asynchronously and posts
// it again, which for a library song never reaches the engine as a seek.
inline constexpr std::string_view kChangeSoundPattern =
    "48 89 5C 24 18 57 48 81 EC C0 00 00 00 48 8B F9 0F 29 BC 24 A0 00 00 00 8B 89 00 04 00 00 "
    "0F 28 F9 E8";
// void UHTUI_MusicListPanel::OnMusicDetailedViewEntryClicked(this, const FName* id):
// a song clicked in the album view. It only plays songs it finds among the
// panel's own list entries, which with few library songs lack them.
inline constexpr std::string_view kEntryClickPattern =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 8B 12 E8 ?? ?? ?? ?? 48 8B 13 48 8B CF "
    "48 8B 5C 24 30 48 83 C4 20 5F";
// Album cover widgets. Each sets its UImage from the album row's cover soft
// pointer; after the original runs, a library album's image gets the imported
// texture through UImage::SetBrushFromTexture (vtable +0x320).
// void UHTUI_MusicDetailedView::SetAlbumCover(this, FName album)
inline constexpr std::string_view kDetailCoverPattern =
    "48 89 54 24 10 56 48 83 EC 50 48 8B 81 98 0F 00 00 48 8B F1 48 85 C0 0F 84";
inline constexpr std::uint32_t kDetailCoverImageOffset = 0xF98;
// void UHTUI_MusicAlbumPageListItem::SetListItem(this, UHTMusicAlbumPageListObject*)
inline constexpr std::string_view kPageCoverPattern =
    "48 89 5C 24 10 56 48 83 EC 50 48 8B DA 48 8B F1 E8 ?? ?? ?? ?? 48 85 DB 0F 84 ?? ?? ?? ?? E8";
inline constexpr std::uint32_t kPageCoverImageOffset = 0x658;
inline constexpr std::uint32_t kPageItemAlbumIdOffset = 0x180;
inline constexpr std::uint32_t kImageSetBrushFromTextureSlot = 0x320;
// void UHTUI_VehicleMusicPanel::OnMusicItemSelected(this, UHTVehicleMusicListObject*):
// hooked only to log when a list entry is selected (diagnostic). The entry's
// song id (FName) is at kVehicleItemSongIdOffset.
inline constexpr std::uint32_t kVehicleItemSongIdOffset = 0x180;
inline constexpr std::string_view kItemSelectedPattern =
    "48 85 D2 0F 84 ?? ?? ?? ?? 53 57 48 83 EC 68 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8B 57 10 4C 8D 40 30";
// Diagnostic: the list row's click handler and the list's click broadcaster.
// Hooked only to log whether a single click reaches them and which gate fields
// the row has (row +0x420 click method, +0x38C, +0x4B8, +0x35E).
// bool SObjectTableRow::ProcessClick(this)
inline constexpr std::string_view kRowClickPattern =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 01 48 8B F9 FF 90 C0 04 00 00 84 C0 0F 84";
// bool BroadcastListObjectClicked(UListView** list, UObject* item)
inline constexpr std::string_view kListClickPattern =
    "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B 7A 10 48 8B DA 48 8B F1 48 85 FF 74 ?? E8";
// UHTSoundSubsystem* GetSoundSubsystem(const UObject* world_context), called at
// the E8 rel32 `kSoundSubsystemCallOffset` bytes into the match.
inline constexpr std::string_view kSoundSubsystemCallPattern =
    "49 8B CD 48 89 B4 24 80 02 00 00 E8 ?? ?? ?? ?? 48 89 45 10";
inline constexpr std::uint32_t kSoundSubsystemCallOffset = 11;

// Album cover. Optional: any miss keeps the template album's cover.
// UTexture2D* UKismetRenderingLibrary::ImportFileAsTexture2D(const FString& path)
inline constexpr std::string_view kImportTexturePattern =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 33 F6 48 8D 3D ?? ?? ?? ?? 0F 57 C0 "
    "48 8B D9 F3 0F 7F 44 24 30";
// bool FUObjectItem::SetFlags(FUObjectItem*, EInternalObjectFlags)
inline constexpr std::string_view kSetItemFlagsPattern =
    "40 53 56 48 83 EC 48 48 8B D9 48 89 7C 24 70 48 8D 0D ?? ?? ?? ?? 8B FA FF 15 ?? ?? ?? ?? "
    "8B 43 08 A9 00 00 50 6E";
inline constexpr std::uint32_t kRootSetFlag = 0x40000000;
// FChunkedFixedUObjectArray::Objects (FUObjectItem**), rip-relative at +3 of a
// 7-byte mov; NumElements is the int32 at +0x14 from it.
inline constexpr std::string_view kObjectArrayPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3";
inline constexpr std::uint32_t kObjectArrayRelOffset = 3;
inline constexpr std::uint32_t kObjectArrayInstructionSize = 7;
inline constexpr std::uint32_t kObjectArrayCountOffset = 0x14;
inline constexpr std::uint32_t kObjectsPerChunk = 65536;
inline constexpr std::uint32_t kObjectItemSize = 24;
inline constexpr std::uint32_t kObjectItemSerialOffset = 0x10;  // int32, 0 = none yet
// GUObjectArray.MasterSerialNumber, from AllocateSerialNumber's `lock xadd`;
// rip-relative at +16 of the match, instruction ends at +20.
inline constexpr std::string_view kSerialCounterPattern =
    "8B 5F 10 85 DB 75 ?? BB 01 00 00 00 F0 0F C1 1D";
inline constexpr std::uint32_t kSerialCounterRelOffset = 16;
inline constexpr std::uint32_t kSerialCounterInstructionEnd = 20;
// UObjectBase::InternalIndex / NamePrivate / OuterPrivate.
inline constexpr std::uint32_t kObjectIndexOffset = 0x0C;
inline constexpr std::uint32_t kObjectOuterOffset = 0x20;
// TSoftObjectPtr: FWeakObjectPtr (8), FTopLevelAssetPath {package, asset} (16),
// SubPathString (FString, 16). The cover gets a live weak pointer {index,
// serial} to the imported texture plus its path ("/Engine/Transient.<name>"):
// the UI skips the cover while the package name is None, and a path alone would
// send the transient texture through the asset loader.
inline constexpr std::uint32_t kSoftPtrSize = 0x28;
inline constexpr std::uint32_t kSoftPtrAssetPathOffset = 0x08;
inline constexpr std::uint32_t kSoftPtrSubPathOffset = 0x18;
// void UHTUI_MusicListItem::SetListItem(this, UHTMusicListObject*): only used
// to find FText::FromString through the E8 rel32 at the offset.
inline constexpr std::string_view kSetListItemPattern = "40 55 41 56 48 83 EC 58 4C 8B F2 48 8B E9 E8";
inline constexpr std::uint32_t kFTextFromStringCallOffset = 0xB8;

// FPlayerMusicData (FTableRowBase, 0xB0).
inline constexpr std::uint32_t kFTextSize = 0x10;  // TRefCountPtr<ITextData> + flags
inline constexpr std::uint32_t kFStringSize = 0x10;
inline constexpr std::uint32_t kMusicRowSize = 0xB0;
inline constexpr std::uint32_t kMusicTitleOffset = 0x08;        // FText
inline constexpr std::uint32_t kMusicSortIndexOffset = 0x18;    // int32
inline constexpr std::uint32_t kMusicCommentOffset = 0x20;      // FString
inline constexpr std::uint32_t kMusicDescriptionOffset = 0x58;  // FText
inline constexpr std::uint32_t kMusicAlbumIdOffset = 0x68;      // FName
// TSoftObjectPtr<UAkAudioEvent>: the song-end path posts it, so synthetic rows
// carry a copy of a real row's event and the takeover replaces the sound.
inline constexpr std::uint32_t kMusicEventOffset = 0x70;
inline constexpr std::uint32_t kMusicEventSize = 0x28;
// FSoftObjectPath::SubPathString (FString) inside the event; cleared in the
// copy so it shares no heap buffer with the real row.
inline constexpr std::uint32_t kMusicEventSubPathOffset = 0x18;
inline constexpr std::uint32_t kMusicSourceItemOffset = 0x98;   // FName
inline constexpr std::uint32_t kMusicSourceQuestOffset = 0xA0;  // FName
inline constexpr std::uint32_t kMusicIsListOffset = 0xA8;       // bool, must be set
inline constexpr std::uint32_t kMusicIsHiddenOffset = 0xA9;     // bool, must be clear
// FMusicAlbumData (FTableRowBase, 0x58).
inline constexpr std::uint32_t kAlbumRowSize = 0x58;
inline constexpr std::uint32_t kAlbumNameOffset = 0x08;         // FText
inline constexpr std::uint32_t kAlbumSortIndexOffset = 0x18;    // int32
inline constexpr std::uint32_t kAlbumCoverOffset = 0x20;        // TSoftObjectPtr<UTexture2D>
inline constexpr std::uint32_t kAlbumDescriptionOffset = 0x48;  // FText
// Synthetic row names: a real music row's ComparisonIndex with these Numbers.
inline constexpr std::uint32_t kSongNumberBase = 0x48460000;    // song k: base + k + 1
inline constexpr std::uint32_t kMaxSongs = 0xFFFE;
inline constexpr std::uint32_t kAlbumNumber = 0x4846FFFF;
inline constexpr std::int32_t kSortIndexBase = 0x7FFF0000;      // after every game entry

} // namespace hifi_vehicle_music_profile
