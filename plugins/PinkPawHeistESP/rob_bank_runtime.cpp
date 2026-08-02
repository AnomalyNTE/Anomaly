#include "rob_bank_runtime.hpp"

#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pink_paw_heist_esp {
namespace {

constexpr std::string_view kModuleName = "HTGame.exe";
constexpr std::string_view kTextSection = ".text";

constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
constexpr std::string_view kPickupPattern =
    "48 89 5C 24 10 57 48 83 EC 40 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? "
    "84 C0 0F 84 ?? ?? ?? ?? 48 83 BB D0 02 00 00 00 0F 84 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ??";
constexpr std::string_view kTextToStringPattern =
    "40 53 48 83 EC 20 48 8B D9 48 8B CA E8 ?? ?? ?? ?? 48 8B D0 48 8B CB "
    "E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3";
constexpr std::string_view kFreeStringPattern =
    "48 85 C9 74 2E 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 "
    "75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 48 "
    "48 83 C4 20 5B C3";

constexpr std::uint32_t kRipDisplacementOffset = 3;
constexpr std::uint32_t kRipInstructionSize = 7;
constexpr std::ptrdiff_t kGObjectsAddend = -16;

// UE and NTE layout used only by the Pink Paw RobBank interaction path.
// Generic session/player/entity/actor/teleport behavior remains host-owned.
constexpr std::ptrdiff_t kWorldGameInstanceOffset = 560;
constexpr std::ptrdiff_t kGameInstanceLocalPlayersOffset = 56;
constexpr std::ptrdiff_t kLocalPlayerControllerOffset = 48;
constexpr std::ptrdiff_t kControllerPlayerStateOffset = 720;
constexpr std::ptrdiff_t kActorRootComponentOffset = 456;
constexpr std::ptrdiff_t kRobBankContainerInterfaceOffset = 864;
constexpr std::ptrdiff_t kRobBankPickupVtableSlot = 24;

constexpr std::ptrdiff_t kObjectInternalIndexOffset = 12;
constexpr std::ptrdiff_t kObjectClassOffset = 16;
constexpr std::ptrdiff_t kObjectNameOffset = 24;
constexpr std::ptrdiff_t kObjectOuterOffset = 32;
constexpr std::ptrdiff_t kStructSuperOffset = 64;

constexpr std::ptrdiff_t kObjectItemsOffset = 16;
constexpr std::ptrdiff_t kObjectMaxCountOffset = 32;
constexpr std::ptrdiff_t kObjectCountOffset = 36;
constexpr std::ptrdiff_t kObjectMaxChunksOffset = 40;
constexpr std::ptrdiff_t kObjectNumChunksOffset = 44;
constexpr std::uint32_t kObjectChunkSize = 65536;
constexpr std::uint32_t kObjectItemStride = 24;
constexpr std::uint32_t kObjectPointerOffset = 0;
constexpr std::uint32_t kObjectSerialOffset = 16;

constexpr std::ptrdiff_t kRobBankCanInteractOffset = 3064;
constexpr std::uint8_t kRobBankCanInteractMask = 1;
constexpr std::ptrdiff_t kRobBankDelayInteractOffset = 3136;
constexpr std::uint8_t kRobBankDelayInteractMask = 1;
constexpr std::ptrdiff_t kRobBankPointUidOffset = 2944;
constexpr std::ptrdiff_t kRobBankPointKeyDoorIdOffset = 196;
constexpr std::ptrdiff_t kRobBankAwardDropIdOffset = 3048;
constexpr std::ptrdiff_t kRobBankCloneDataAssetItemOffset = 104;
constexpr std::ptrdiff_t kPlayerStateKeyDoorsOffset = 36928;
constexpr std::int32_t kMaximumKeyDoors = 4096;

constexpr std::ptrdiff_t kDataTableRowMapOffset = 48;
constexpr std::size_t kDataTableRowStride = 24;
constexpr std::size_t kDataTableRowPointerOffset = 8;
constexpr std::int32_t kMaximumDataTableRows = 4096;
constexpr std::ptrdiff_t kStaticItemNameOffset = 160;
constexpr std::ptrdiff_t kStaticItemElementDataOffset = 392;
constexpr std::ptrdiff_t kInstancedStructDataOffset = 8;
constexpr std::ptrdiff_t kRobBankItemValueOffset = 0;
constexpr std::ptrdiff_t kRobBankItemCoinOffset = 4;

constexpr std::uint32_t kMaximumObjects = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumObjectChunks = 4096;
constexpr std::uint32_t kObjectDiscoveryBatch = 2048;
constexpr std::size_t kMaximumResolvedNameBytes = 1024;
constexpr std::uint32_t kMaximumMarkerEntityCount = 32768;
constexpr std::size_t kMarkerPageCapacity = ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY;

constexpr std::array<std::uint8_t, 16> kPickupPrologue{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
    0xEC, 0x40, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
constexpr std::array<std::uint8_t, 13> kPickupSuccessReturn{
    0xB0, 0x01, 0x48, 0x8B, 0x5C, 0x24, 0x58,
    0x48, 0x83, 0xC4, 0x40, 0x5F, 0xC3};
constexpr std::array<std::uint8_t, 13> kPickupFailureReturn{
    0x48, 0x8B, 0x5C, 0x24, 0x58, 0x32, 0xC0,
    0x48, 0x83, 0xC4, 0x40, 0x5F, 0xC3};

AnomalyStatusV1 Status(
    const std::uint32_t code,
    const char* const message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool AddAddress(
    const std::uintptr_t base,
    const std::uint64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool AddSignedAddress(
    const std::uintptr_t base,
    const std::ptrdiff_t offset,
    std::uintptr_t& result) noexcept {
    if (offset < 0) {
        const auto magnitude = static_cast<std::uintptr_t>(-(offset + 1)) + 1U;
        if (base <= magnitude) return false;
        result = base - magnitude;
        return true;
    }
    return AddAddress(base, static_cast<std::uint64_t>(offset), result);
}

struct ArrayHeader final {
    std::uintptr_t data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct FNameValue final {
    std::uint32_t comparison_index{};
    std::uint32_t number{};
};

struct UnrealString final {
    char16_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

using TextToStringFunction = UnrealString*(ANOMALY_CALL*)(UnrealString*, const void*);
using FreeStringFunction = void(ANOMALY_CALL*)(void*);

std::uint64_t EncodeFName(const FNameValue value) noexcept {
    return static_cast<std::uint64_t>(value.comparison_index) |
        (static_cast<std::uint64_t>(value.number) << 32U);
}

struct ObjectRegistry final {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
    std::uint64_t chunk_signature{};
};

struct PointTable final {
    std::uintptr_t table{};
    std::uint64_t registry_generation{};
    std::uint32_t observed_object_count{};
    std::unordered_map<std::uint64_t, std::uintptr_t> rows;
    std::unordered_map<std::string, std::uintptr_t> rows_by_name;
    std::unordered_map<std::uintptr_t, FNameValue> key_doors;
    bool available{};
    bool discovery_complete{};
};

struct DataTableRows final {
    std::uintptr_t table{};
    std::unordered_map<std::uint64_t, std::uintptr_t> rows;
    std::unordered_map<std::string, std::uintptr_t> rows_by_name;
};

struct RobBankItemMetadata final {
    std::string name_utf8;
    std::uint32_t fons_value{};
    std::uint32_t pink_paw_coin_value{};
};

struct RobBankItemTables final {
    std::uint64_t registry_generation{};
    std::uint32_t observed_object_count{};
    std::unordered_map<std::string, RobBankItemMetadata> items;
    bool available{};
    bool discovery_complete{};
};

}  // namespace

struct PinkPawWorldGate::Impl final {
    AnomalyGenerationHandleV1 world{};
    PinkPawWorldState state{PinkPawWorldState::unavailable};
    std::uint32_t marker_name_id{};

    [[nodiscard]] static bool SameHandle(
        const AnomalyGenerationHandleV1 left,
        const AnomalyGenerationHandleV1 right) noexcept {
        return left.id == right.id && left.generation == right.generation;
    }

    [[nodiscard]] static bool Complete(const std::uint32_t flags) noexcept {
        return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            (flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) == 0;
    }

    [[nodiscard]] static std::string ResolveName(
        const AnomalyUe5NamesServiceV1& names,
        const std::uint32_t name_id) {
        if (name_id == 0 || names.resolve_utf8 == nullptr) return {};
        std::array<char, 128> local{};
        std::size_t size = local.size();
        AnomalyStatusV1 status = names.resolve_utf8(
            names.user, name_id, local.data(), &size);
        if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size()) {
            return std::string(local.data(), size - 1U);
        }
        if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
            size > kMaximumResolvedNameBytes) {
            return {};
        }
        std::string value(size, '\0');
        status = names.resolve_utf8(names.user, name_id, value.data(), &size);
        if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size()) {
            return {};
        }
        value.resize(size - 1U);
        return value;
    }

    [[nodiscard]] PinkPawWorldState Probe(
        const AnomalyNteEntitiesServiceV1& entities,
        const AnomalyUe5NamesServiceV1& names) {
        AnomalyNteEntityFrameV1 frame{sizeof(frame)};
        if (entities.frame == nullptr || entities.page == nullptr ||
            entities.frame(entities.user, &frame).code != ANOMALY_STATUS_V1_OK ||
            !Complete(frame.flags) || frame.generation == 0 ||
            frame.entity_count > kMaximumMarkerEntityCount) {
            return PinkPawWorldState::unavailable;
        }

        std::array<AnomalyNteEntitySnapshotV1, kMarkerPageCapacity> page{};
        const auto request_page = [&](const std::uint32_t offset,
                                      const std::uint32_t capacity,
                                      AnomalyNteEntityPageResultV1& result) {
            for (auto& snapshot : page) {
                snapshot = AnomalyNteEntitySnapshotV1{sizeof(snapshot)};
            }
            AnomalyNteEntityPageRequestV1 request{};
            request.struct_size = sizeof(request);
            request.generation = frame.generation;
            request.offset = offset;
            request.capacity = capacity;
            request.class_name_id = marker_name_id;
            return entities.page(entities.user, &request, page.data(), &result).code ==
                ANOMALY_STATUS_V1_OK;
        };

        if (marker_name_id != 0) {
            AnomalyNteEntityPageResultV1 result{sizeof(result)};
            if (!request_page(0, 1, result) || !Complete(result.flags) ||
                result.generation != frame.generation || result.returned > 1 ||
                result.total_matches > frame.entity_count ||
                result.returned != (result.total_matches == 0 ? 0U : 1U)) {
                return PinkPawWorldState::unavailable;
            }
            return result.total_matches == 0
                ? PinkPawWorldState::outside
                : PinkPawWorldState::active;
        }

        std::unordered_set<std::uint32_t> resolved_names;
        std::uint32_t offset{};
        for (;;) {
            AnomalyNteEntityPageResultV1 result{sizeof(result)};
            if (!request_page(
                    offset, static_cast<std::uint32_t>(page.size()), result) ||
                !Complete(result.flags) || result.generation != frame.generation ||
                result.total_matches != frame.entity_count ||
                result.returned > page.size() || offset > result.total_matches ||
                result.returned > result.total_matches - offset) {
                return PinkPawWorldState::unavailable;
            }
            for (std::uint32_t index{}; index < result.returned; ++index) {
                const auto& snapshot = page[index];
                if (!Complete(snapshot.flags) ||
                    snapshot.handle.generation != frame.generation) {
                    return PinkPawWorldState::unavailable;
                }
                if (snapshot.class_name_id == 0 ||
                    !resolved_names.insert(snapshot.class_name_id).second) {
                    continue;
                }
                if (ResolveName(names, snapshot.class_name_id) == kWorldMarkerClassName) {
                    marker_name_id = snapshot.class_name_id;
                    return PinkPawWorldState::active;
                }
            }
            const std::uint32_t consumed = offset + result.returned;
            if (consumed >= result.total_matches) {
                return result.next_offset == result.total_matches
                    ? PinkPawWorldState::outside
                    : PinkPawWorldState::unavailable;
            }
            if (result.next_offset != consumed || result.next_offset <= offset) {
                return PinkPawWorldState::unavailable;
            }
            offset = result.next_offset;
        }
    }
};

PinkPawWorldGate::PinkPawWorldGate() : impl_(std::make_unique<Impl>()) {}

PinkPawWorldGate::~PinkPawWorldGate() = default;

PinkPawWorldState PinkPawWorldGate::Refresh(
    const AnomalyHostApiV1* const host) noexcept {
    if (host == nullptr) return PinkPawWorldState::unavailable;
    try {
        const anomaly::sdk::Host services(host);
        const auto session = services.Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID,
            ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
        if (!session || session->snapshot == nullptr) {
            return PinkPawWorldState::unavailable;
        }
        AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
        if (session->snapshot(session->user, &snapshot).code != ANOMALY_STATUS_V1_OK ||
            snapshot.struct_size < sizeof(snapshot) ||
            snapshot.state != ANOMALY_NTE_SESSION_V1_WORLD_READY ||
            snapshot.world.id == 0 || snapshot.world.generation == 0) {
            impl_->world = {};
            impl_->state = PinkPawWorldState::unavailable;
            return impl_->state;
        }
        if (Impl::SameHandle(impl_->world, snapshot.world) &&
            impl_->state != PinkPawWorldState::unavailable) {
            return impl_->state;
        }

        const auto entities = services.Query<AnomalyNteEntitiesServiceV1>(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
            ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION);
        const auto names = services.Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID,
            ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
        if (!entities || !names || names->resolve_utf8 == nullptr) {
            return PinkPawWorldState::unavailable;
        }
        const PinkPawWorldState next = impl_->Probe(*entities.get(), *names.get());
        if (next != PinkPawWorldState::unavailable) {
            impl_->world = snapshot.world;
        }
        impl_->state = next;
        return next;
    } catch (...) {
        return PinkPawWorldState::unavailable;
    }
}

void PinkPawWorldGate::Invalidate() noexcept {
    impl_->world = {};
    impl_->state = PinkPawWorldState::unavailable;
}

void PinkPawWorldGate::Reset() noexcept {
    Invalidate();
    impl_->marker_name_id = 0;
}

struct RobBankRuntime::Impl final {
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signatures{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    std::uintptr_t world_storage{};
    std::uintptr_t object_registry_address{};
    std::uintptr_t pickup_function{};
    std::uintptr_t text_to_string_function{};
    std::uintptr_t free_string_function{};
    std::uintptr_t world{};
    std::uintptr_t player_controller{};
    ObjectRegistry registry;
    std::uint64_t registry_generation{};
    PointTable point_table;
    RobBankItemTables item_tables;
    std::unordered_set<std::uint64_t> unlocked_key_doors;
    std::unordered_map<std::uintptr_t, bool> rob_bank_classes;
    std::unordered_map<std::uintptr_t, bool> rob_bank_clone_data_asset_classes;
    bool key_door_context_available{};
    bool started{};
    bool refreshed{};

    [[nodiscard]] bool ReadBytes(
        const std::uintptr_t address,
        void* const destination,
        const std::size_t size) const noexcept {
        if (core == nullptr || core->read_memory == nullptr || address == 0 ||
            destination == nullptr || size == 0) {
            return false;
        }
        const AnomalyMutableByteSpanV1 output{
            static_cast<std::uint8_t*>(destination), size};
        return core->read_memory(core->user, address, output).code ==
            ANOMALY_STATUS_V1_OK;
    }

    template <typename T>
    [[nodiscard]] bool Read(
        const std::uintptr_t address,
        T& value) const noexcept {
        return ReadBytes(address, &value, sizeof(value));
    }

    [[nodiscard]] bool ReadPointerAt(
        const std::uintptr_t base,
        const std::ptrdiff_t offset,
        std::uintptr_t& value) const noexcept {
        std::uintptr_t address{};
        return AddSignedAddress(base, offset, address) && Read(address, value) && value != 0;
    }

    [[nodiscard]] bool IsGameThread() const noexcept {
        return framework != nullptr && framework->is_game_thread != nullptr &&
            framework->is_game_thread(framework->user) != 0;
    }

    [[nodiscard]] bool ResolveDirect(
        const std::string_view pattern,
        std::uintptr_t& address) const noexcept {
        address = 0;
        return signatures != nullptr && signatures->resolve != nullptr &&
            signatures->resolve(
                signatures->user, anomaly::sdk::StringView(kModuleName),
                anomaly::sdk::StringView(kTextSection),
                anomaly::sdk::StringView(pattern), &address).code ==
                ANOMALY_STATUS_V1_OK &&
            address != 0;
    }

    [[nodiscard]] bool ResolveRipRelative(
        const std::string_view pattern,
        const std::ptrdiff_t addend,
        std::uintptr_t& address) const noexcept {
        std::uintptr_t instruction{};
        if (!ResolveDirect(pattern, instruction)) return false;
        std::uintptr_t displacement_address{};
        std::int32_t displacement{};
        if (!AddAddress(instruction, kRipDisplacementOffset, displacement_address) ||
            !Read(displacement_address, displacement)) {
            return false;
        }
        const auto resolved = static_cast<std::intptr_t>(instruction) +
            static_cast<std::intptr_t>(kRipInstructionSize) + displacement;
        if (resolved <= 0) return false;
        return AddSignedAddress(
            static_cast<std::uintptr_t>(resolved), addend, address);
    }

    template <std::size_t Size>
    [[nodiscard]] bool Matches(
        const std::uintptr_t address,
        const std::array<std::uint8_t, Size>& expected) const noexcept {
        std::array<std::uint8_t, Size> actual{};
        return ReadBytes(address, actual.data(), actual.size()) && actual == expected;
    }

    [[nodiscard]] bool ResolvePluginProfile() noexcept {
        std::uintptr_t pickup{};
        std::uintptr_t text_to_string{};
        std::uintptr_t free_string{};
        if (!ResolveRipRelative(kGWorldPattern, 0, world_storage) ||
            !ResolveRipRelative(
                kGObjectsPattern, kGObjectsAddend, object_registry_address) ||
            !ResolveDirect(kPickupPattern, pickup) ||
            !ResolveDirect(kTextToStringPattern, text_to_string) ||
            !ResolveDirect(kFreeStringPattern, free_string)) {
            return false;
        }
        std::uintptr_t success_return{};
        std::uintptr_t failure_return{};
        if (!AddAddress(pickup, 0xD6U, success_return) ||
            !AddAddress(pickup, 0xE3U, failure_return) ||
            !Matches(pickup, kPickupPrologue) ||
            !Matches(success_return, kPickupSuccessReturn) ||
            !Matches(failure_return, kPickupFailureReturn)) {
            return false;
        }
        pickup_function = pickup;
        text_to_string_function = text_to_string;
        free_string_function = free_string;
        return true;
    }

    [[nodiscard]] std::string ResolveName(const std::uint32_t name_id) const {
        if (name_id == 0 || names == nullptr || names->resolve_utf8 == nullptr) return {};
        std::array<char, 128> local{};
        std::size_t size = local.size();
        AnomalyStatusV1 status = names->resolve_utf8(
            names->user, name_id, local.data(), &size);
        if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size()) {
            return std::string(local.data(), size - 1U);
        }
        if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
            size > kMaximumResolvedNameBytes) {
            return {};
        }
        std::string value(size, '\0');
        status = names->resolve_utf8(names->user, name_id, value.data(), &size);
        if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size()) return {};
        value.resize(size - 1U);
        return value;
    }

    [[nodiscard]] std::string ResolveObjectName(const std::uintptr_t object) const {
        std::uintptr_t address{};
        std::uint32_t name_id{};
        if (!AddSignedAddress(object, kObjectNameOffset, address) ||
            !Read(address, name_id)) {
            return {};
        }
        return ResolveName(name_id);
    }

    [[nodiscard]] std::string RenderFName(const FNameValue value) const {
        std::string name = ResolveName(value.comparison_index);
        if (name.empty() || value.number == 0) return name;
        name += '_';
        name += std::to_string(value.number - 1U);
        return name;
    }

    [[nodiscard]] std::string ReadText(const std::uintptr_t text) const {
        UnrealString converted;
        const auto text_to_string = reinterpret_cast<TextToStringFunction>(text_to_string_function);
        const auto free_string = reinterpret_cast<FreeStringFunction>(free_string_function);
        if (text_to_string(&converted, reinterpret_cast<const void*>(text)) == nullptr ||
            converted.data == nullptr || converted.count <= 1) {
            if (converted.data != nullptr) free_string(converted.data);
            return {};
        }

        std::string utf8;
        utf8.reserve((static_cast<std::size_t>(converted.count) - 1U) * 3U);
        for (std::int32_t index{}; index + 1 < converted.count; ++index) {
            const std::uint32_t code_point = converted.data[index];
            if (code_point <= 0x7FU) {
                utf8.push_back(static_cast<char>(code_point));
            } else if (code_point <= 0x7FFU) {
                utf8.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
                utf8.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            } else {
                utf8.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
                utf8.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                utf8.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
        }
        free_string(converted.data);
        return utf8;
    }

    [[nodiscard]] bool ClassIsOrDerivesFrom(
        std::uintptr_t class_object,
        const std::string_view expected_name) const {
        for (std::uint32_t depth{}; class_object != 0 && depth < 128; ++depth) {
            if (ResolveObjectName(class_object) == expected_name) return true;
            std::uintptr_t next{};
            if (!ReadPointerAt(class_object, kStructSuperOffset, next)) return false;
            class_object = next;
        }
        return false;
    }

    [[nodiscard]] bool IsRobBankClass(const std::uintptr_t class_object) {
        if (const auto found = rob_bank_classes.find(class_object);
            found != rob_bank_classes.end()) {
            return found->second;
        }
        const bool matches = ClassIsOrDerivesFrom(class_object, "HTRobBankItemActor");
        rob_bank_classes.emplace(class_object, matches);
        return matches;
    }

    [[nodiscard]] bool IsRobBankCloneDataAssetClass(const std::uintptr_t class_object) {
        if (const auto found = rob_bank_clone_data_asset_classes.find(class_object);
            found != rob_bank_clone_data_asset_classes.end()) {
            return found->second;
        }
        const bool matches = ClassIsOrDerivesFrom(class_object, "RobBankCloneDataAsset");
        rob_bank_clone_data_asset_classes.emplace(class_object, matches);
        return matches;
    }

    [[nodiscard]] bool ReadObjectChunk(
        const ObjectRegistry& current,
        const std::uint32_t page,
        std::uintptr_t& chunk) const noexcept {
        if (current.items == 0 || page >= current.num_chunks) return false;
        std::uintptr_t address{};
        return AddAddress(
                   current.items,
                   static_cast<std::uint64_t>(page) * sizeof(std::uintptr_t), address) &&
            Read(address, chunk) && chunk != 0 &&
            (chunk & (alignof(std::uintptr_t) - 1U)) == 0;
    }

    [[nodiscard]] bool ReadObjectSlot(
        const ObjectRegistry& current,
        const std::uint32_t index,
        std::uintptr_t& object,
        std::uint32_t& serial) const noexcept {
        if (current.items == 0 || index >= current.count ||
            current.count > current.max_count || current.num_chunks > current.max_chunks) {
            return false;
        }
        const std::uint32_t page = index / kObjectChunkSize;
        const std::uint32_t slot = index % kObjectChunkSize;
        std::uintptr_t chunk{};
        std::uintptr_t item{};
        std::uintptr_t object_address{};
        std::uintptr_t serial_address{};
        return ReadObjectChunk(current, page, chunk) &&
            AddAddress(
                chunk, static_cast<std::uint64_t>(slot) * kObjectItemStride, item) &&
            AddAddress(item, kObjectPointerOffset, object_address) &&
            AddAddress(item, kObjectSerialOffset, serial_address) &&
            Read(object_address, object) && Read(serial_address, serial);
    }

    [[nodiscard]] bool ReadObjectPointer(
        const ObjectRegistry& current,
        const std::uint32_t index,
        std::uintptr_t& object) const noexcept {
        if (current.items == 0 || index >= current.count ||
            current.count > current.max_count || current.num_chunks > current.max_chunks) {
            return false;
        }
        const std::uint32_t page = index / kObjectChunkSize;
        const std::uint32_t slot = index % kObjectChunkSize;
        std::uintptr_t chunk{};
        std::uintptr_t item{};
        std::uintptr_t object_address{};
        return ReadObjectChunk(current, page, chunk) &&
            AddAddress(
                chunk, static_cast<std::uint64_t>(slot) * kObjectItemStride, item) &&
            AddAddress(item, kObjectPointerOffset, object_address) &&
            Read(object_address, object);
    }

    [[nodiscard]] bool LoadObjectRegistry(ObjectRegistry& next) const noexcept {
        ObjectRegistry candidate;
        if (!ReadPointerAt(object_registry_address, kObjectItemsOffset, candidate.items)) {
            return false;
        }
        std::uintptr_t address{};
        if (!AddSignedAddress(object_registry_address, kObjectCountOffset, address) ||
            !Read(address, candidate.count) ||
            !AddSignedAddress(object_registry_address, kObjectMaxCountOffset, address) ||
            !Read(address, candidate.max_count) ||
            !AddSignedAddress(object_registry_address, kObjectMaxChunksOffset, address) ||
            !Read(address, candidate.max_chunks) ||
            !AddSignedAddress(object_registry_address, kObjectNumChunksOffset, address) ||
            !Read(address, candidate.num_chunks) || candidate.max_count == 0 ||
            candidate.max_count > kMaximumObjects || candidate.count > candidate.max_count ||
            candidate.max_chunks == 0 || candidate.max_chunks > kMaximumObjectChunks ||
            candidate.num_chunks > candidate.max_chunks ||
            static_cast<std::uint64_t>(candidate.max_count) >
                static_cast<std::uint64_t>(candidate.max_chunks) * kObjectChunkSize) {
            return false;
        }
        const std::uint64_t required_chunks = candidate.count == 0
            ? 0
            : (static_cast<std::uint64_t>(candidate.count) + kObjectChunkSize - 1U) /
                kObjectChunkSize;
        if (required_chunks > candidate.num_chunks) return false;

        constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
        candidate.chunk_signature = kFnvOffset;
        for (std::uint32_t page{}; page < required_chunks; ++page) {
            std::uintptr_t chunk{};
            if (!ReadObjectChunk(candidate, page, chunk)) return false;
            candidate.chunk_signature ^= static_cast<std::uint64_t>(chunk);
            candidate.chunk_signature *= kFnvPrime;
        }
        next = candidate;
        return true;
    }

    [[nodiscard]] bool RegistryIdentityChanged(
        const ObjectRegistry& next) const noexcept {
        return registry.items == 0 || next.items != registry.items ||
            next.count < registry.count || next.max_count != registry.max_count ||
            next.max_chunks != registry.max_chunks ||
            next.num_chunks < registry.num_chunks ||
            next.chunk_signature != registry.chunk_signature;
    }

    [[nodiscard]] bool ReadWorldAndController(
        std::uintptr_t& next_world,
        std::uintptr_t& next_controller) const noexcept {
        next_world = 0;
        next_controller = 0;
        if (!Read(world_storage, next_world) || next_world == 0) return false;
        std::uintptr_t game_instance{};
        std::uintptr_t local_players{};
        std::uintptr_t local_player{};
        std::uintptr_t count_address{};
        std::int32_t player_count{};
        return ReadPointerAt(next_world, kWorldGameInstanceOffset, game_instance) &&
            ReadPointerAt(
                game_instance, kGameInstanceLocalPlayersOffset, local_players) &&
            AddSignedAddress(
                game_instance,
                kGameInstanceLocalPlayersOffset +
                    static_cast<std::ptrdiff_t>(sizeof(std::uintptr_t)),
                count_address) &&
            Read(count_address, player_count) && player_count >= 1 && player_count <= 64 &&
            Read(local_players, local_player) && local_player != 0 &&
            ReadPointerAt(local_player, kLocalPlayerControllerOffset, next_controller);
    }

    void ResetWorldState() noexcept {
        point_table = {};
        item_tables = {};
        unlocked_key_doors.clear();
        rob_bank_classes.clear();
        rob_bank_clone_data_asset_classes.clear();
        key_door_context_available = false;
    }

    [[nodiscard]] bool BuildPointTable(
        const std::uintptr_t table,
        PointTable& result) {
        if (ResolveObjectName(table) != "DT_RobBankPoint") return false;
        std::uintptr_t class_object{};
        std::uintptr_t outer_object{};
        if (!ReadPointerAt(table, kObjectClassOffset, class_object) ||
            !ReadPointerAt(table, kObjectOuterOffset, outer_object) ||
            ResolveObjectName(class_object) != "DataTable" ||
            ResolveObjectName(outer_object) !=
                "/Game/DataTable/RobBank/DT_RobBankPoint") {
            return false;
        }

        std::uintptr_t row_map_address{};
        ArrayHeader header;
        if (!AddSignedAddress(table, kDataTableRowMapOffset, row_map_address) ||
            !Read(row_map_address, header) || header.count <= 0 ||
            header.capacity < header.count || header.capacity > kMaximumDataTableRows ||
            header.data == 0) {
            return false;
        }
        const std::size_t byte_count = static_cast<std::size_t>(header.count) *
            kDataTableRowStride;
        std::vector<std::uint8_t> elements(byte_count);
        if (!ReadBytes(header.data, elements.data(), elements.size())) return false;

        PointTable candidate;
        candidate.table = table;
        candidate.rows.reserve(static_cast<std::size_t>(header.count));
        candidate.rows_by_name.reserve(static_cast<std::size_t>(header.count));
        for (std::int32_t index{}; index < header.count; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) *
                kDataTableRowStride;
            FNameValue key;
            std::uintptr_t row{};
            std::memcpy(&key, elements.data() + offset, sizeof(key));
            std::memcpy(
                &row, elements.data() + offset + kDataTableRowPointerOffset,
                sizeof(row));
            const std::string rendered = RenderFName(key);
            if (key.comparison_index == 0 || row == 0 || rendered.empty()) return false;
            candidate.rows.emplace(EncodeFName(key), row);
            candidate.rows_by_name.emplace(rendered, row);
        }
        candidate.available = true;
        result = std::move(candidate);
        return true;
    }

    void RefreshPointTable() noexcept {
        try {
            if (registry.items == 0 || registry.count == 0) {
                point_table = {};
                return;
            }
            if (point_table.registry_generation != registry_generation) {
                point_table = {};
                point_table.registry_generation = registry_generation;
            }
            if (point_table.available ||
                point_table.observed_object_count >= registry.count) {
                point_table.discovery_complete = true;
                return;
            }

            const std::uint32_t begin = point_table.observed_object_count;
            const std::uint32_t end = begin + (std::min)(
                kObjectDiscoveryBatch, registry.count - begin);
            point_table.discovery_complete = false;
            for (std::uint32_t index = begin; index < end; ++index) {
                std::uintptr_t object{};
                if (!ReadObjectPointer(registry, index, object) || object == 0 ||
                    ResolveObjectName(object) != "DT_RobBankPoint") {
                    continue;
                }
                PointTable candidate;
                if (!BuildPointTable(object, candidate)) continue;
                candidate.registry_generation = registry_generation;
                candidate.observed_object_count = registry.count;
                point_table = std::move(candidate);
                return;
            }
            point_table.observed_object_count = end;
            point_table.discovery_complete = end == registry.count;
        } catch (...) {
            point_table.discovery_complete = true;
        }
    }

    [[nodiscard]] bool BuildDataTableRows(
        const std::uintptr_t table,
        DataTableRows& result) {
        std::uintptr_t class_object{};
        if (!ReadPointerAt(table, kObjectClassOffset, class_object) ||
            ResolveObjectName(class_object) != "DataTable") {
            return false;
        }

        std::uintptr_t row_map_address{};
        ArrayHeader header;
        if (!AddSignedAddress(table, kDataTableRowMapOffset, row_map_address) ||
            !Read(row_map_address, header) || header.count <= 0 ||
            header.capacity < header.count || header.capacity > kMaximumDataTableRows ||
            header.data == 0) {
            return false;
        }
        const std::size_t byte_count = static_cast<std::size_t>(header.count) *
            kDataTableRowStride;
        std::vector<std::uint8_t> elements(byte_count);
        if (!ReadBytes(header.data, elements.data(), elements.size())) return false;

        DataTableRows candidate;
        candidate.table = table;
        candidate.rows.reserve(static_cast<std::size_t>(header.count));
        candidate.rows_by_name.reserve(static_cast<std::size_t>(header.count));
        for (std::int32_t index{}; index < header.count; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) *
                kDataTableRowStride;
            FNameValue key;
            std::uintptr_t row{};
            std::memcpy(&key, elements.data() + offset, sizeof(key));
            std::memcpy(
                &row, elements.data() + offset + kDataTableRowPointerOffset,
                sizeof(row));
            const std::string rendered = RenderFName(key);
            if (key.comparison_index == 0 || row == 0 || rendered.empty()) return false;
            candidate.rows.emplace(EncodeFName(key), row);
            candidate.rows_by_name.emplace(rendered, row);
        }
        result = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool BuildItemTables(const std::uintptr_t data_asset) {
        std::uintptr_t table{};
        DataTableRows rows;
        if (!ReadPointerAt(data_asset, kRobBankCloneDataAssetItemOffset, table) ||
            !BuildDataTableRows(table, rows)) {
            return false;
        }
        std::unordered_map<std::string, RobBankItemMetadata> items;
        if (!BuildItemMetadata(rows, items)) return false;
        for (auto& [award_drop_id, item] : items) {
            item_tables.items.emplace(std::move(award_drop_id), std::move(item));
        }
        return true;
    }

    [[nodiscard]] bool BuildItemMetadata(
        const DataTableRows& rows,
        std::unordered_map<std::string, RobBankItemMetadata>& items) const {
        if (rows.rows_by_name.empty()) return false;

        items.reserve(rows.rows_by_name.size());
        for (const auto& [row_name, row] : rows.rows_by_name) {
            std::uintptr_t address{};
            std::uintptr_t element_data{};
            std::int32_t fons_value{};
            std::int32_t pink_paw_coin_value{};
            if (!AddSignedAddress(row, kStaticItemElementDataOffset, address) ||
                !AddSignedAddress(address, kInstancedStructDataOffset, address) ||
                !Read(address, element_data) || element_data == 0 ||
                !AddSignedAddress(element_data, kRobBankItemValueOffset, address) ||
                !Read(address, fons_value) ||
                !AddSignedAddress(element_data, kRobBankItemCoinOffset, address) ||
                !Read(address, pink_paw_coin_value) ||
                fons_value < 0 || pink_paw_coin_value < 0) {
                continue;
            }
            if (!AddSignedAddress(row, kStaticItemNameOffset, address)) continue;
            std::string name = ReadText(address);
            if (name.empty()) name = row_name;
            items.emplace(
                "drop_" + row_name,
                RobBankItemMetadata{
                    std::move(name), static_cast<std::uint32_t>(fons_value),
                    static_cast<std::uint32_t>(pink_paw_coin_value)});
        }
        if (items.empty()) return false;
        return true;
    }

    void RefreshItemTables() noexcept {
        try {
            if (registry.items == 0 || registry.count == 0) {
                item_tables = {};
                return;
            }
            if (item_tables.registry_generation != registry_generation) {
                item_tables = {};
                item_tables.registry_generation = registry_generation;
            }
            if (item_tables.observed_object_count >= registry.count) {
                item_tables.discovery_complete = true;
                return;
            }

            const std::uint32_t begin = item_tables.observed_object_count;
            const std::uint32_t end = begin + (std::min)(
                kObjectDiscoveryBatch, registry.count - begin);
            item_tables.discovery_complete = false;
            for (std::uint32_t index = begin; index < end; ++index) {
                std::uintptr_t object{};
                std::uintptr_t class_object{};
                if (!ReadObjectPointer(registry, index, object) || object == 0 ||
                    !ReadPointerAt(object, kObjectClassOffset, class_object) ||
                    !IsRobBankCloneDataAssetClass(class_object) ||
                    !BuildItemTables(object)) {
                    continue;
                }
                item_tables.available = !item_tables.items.empty();
                item_tables.observed_object_count = registry.count;
                item_tables.discovery_complete = true;
                return;
            }
            item_tables.available = !item_tables.items.empty();
            item_tables.observed_object_count = end;
            item_tables.discovery_complete = end == registry.count;
        } catch (...) {
            item_tables.discovery_complete = true;
        }
    }

    void ResolveItemMetadata(
        const std::uintptr_t actor,
        RobBankInspection& inspection) const {
        if (!item_tables.available) return;
        std::uintptr_t address{};
        FNameValue award_drop_id;
        if (!AddSignedAddress(actor, kRobBankAwardDropIdOffset, address) ||
            !Read(address, award_drop_id)) {
            return;
        }
        const auto item = item_tables.items.find(RenderFName(award_drop_id));
        if (item == item_tables.items.end()) return;
        inspection.name_utf8 = item->second.name_utf8;
        inspection.fons_value = item->second.fons_value;
        inspection.pink_paw_coin_value = item->second.pink_paw_coin_value;
        inspection.item_resolved = true;
    }

    [[nodiscard]] bool RefreshKeyDoorContext(bool* const changed = nullptr) {
        std::unordered_set<std::uint64_t> next_unlocked_key_doors;
        std::uintptr_t player_state{};
        if (player_controller == 0 ||
            !ReadPointerAt(
                player_controller, kControllerPlayerStateOffset, player_state)) {
            return false;
        }
        std::uintptr_t address{};
        ArrayHeader header;
        if (!AddSignedAddress(player_state, kPlayerStateKeyDoorsOffset, address) ||
            !Read(address, header) || header.count < 0 ||
            header.capacity < header.count || header.capacity > kMaximumKeyDoors ||
            (header.count != 0 && header.data == 0)) {
            return false;
        }
        if (header.count != 0) {
            std::vector<FNameValue> values(static_cast<std::size_t>(header.count));
            if (!ReadBytes(
                    header.data, values.data(), values.size() * sizeof(FNameValue))) {
                return false;
            }
            next_unlocked_key_doors.reserve(values.size());
            for (const FNameValue value : values) {
                next_unlocked_key_doors.insert(EncodeFName(value));
            }
        }
        if (changed != nullptr) *changed = next_unlocked_key_doors != unlocked_key_doors;
        unlocked_key_doors = std::move(next_unlocked_key_doors);
        return true;
    }

    [[nodiscard]] bool ResolveEntity(
        const std::uint64_t entity_id,
        const std::string_view expected_class_name,
        RobBankEntity& identity,
        std::uintptr_t& actor,
        std::uintptr_t& class_object) {
        if (entity_id == 0 ||
            entity_id - 1U > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        const auto index = static_cast<std::uint32_t>(entity_id - 1U);
        std::uint32_t serial{};
        if (!ReadObjectSlot(registry, index, actor, serial) || actor == 0 || serial == 0) {
            return false;
        }
        std::uintptr_t index_address{};
        std::int32_t internal_index{-1};
        if (!AddSignedAddress(actor, kObjectInternalIndexOffset, index_address) ||
            !Read(index_address, internal_index) || internal_index < 0 ||
            static_cast<std::uint32_t>(internal_index) != index ||
            !ReadPointerAt(actor, kObjectClassOffset, class_object)) {
            return false;
        }
        const std::string class_name = ResolveObjectName(class_object);
        if (!class_name.starts_with("BankBox_") ||
            (!expected_class_name.empty() && class_name != expected_class_name) ||
            !IsRobBankClass(class_object)) {
            return false;
        }
        identity = {index, serial};
        return true;
    }

    [[nodiscard]] bool ResolveEntity(
        const RobBankEntity identity,
        std::uintptr_t& actor,
        std::uintptr_t& class_object) {
        std::uint32_t serial{};
        if (!identity.Valid() ||
            !ReadObjectSlot(registry, identity.object_index, actor, serial) ||
            actor == 0 || serial != identity.object_serial) {
            return false;
        }
        std::uintptr_t index_address{};
        std::int32_t internal_index{-1};
        return AddSignedAddress(actor, kObjectInternalIndexOffset, index_address) &&
            Read(index_address, internal_index) && internal_index >= 0 &&
            static_cast<std::uint32_t>(internal_index) == identity.object_index &&
            ReadPointerAt(actor, kObjectClassOffset, class_object) &&
            ResolveObjectName(class_object).starts_with("BankBox_") &&
            IsRobBankClass(class_object);
    }

    [[nodiscard]] bool ReadFlag(
        const std::uintptr_t object,
        const std::ptrdiff_t offset,
        const std::uint8_t mask,
        bool& value) const noexcept {
        std::uintptr_t address{};
        std::uint8_t byte{};
        return AddSignedAddress(object, offset, address) && Read(address, byte) &&
            (value = (byte & mask) != 0, true);
    }

    [[nodiscard]] bool FindPointRow(
        const FNameValue point_uid,
        std::uintptr_t& row) const {
        if (!point_table.available) return false;
        if (const auto exact = point_table.rows.find(EncodeFName(point_uid));
            exact != point_table.rows.end()) {
            row = exact->second;
            return true;
        }
        std::string point_name = RenderFName(point_uid);
        const auto separator = point_name.find('$');
        if (separator == std::string::npos) return false;
        point_name.resize(separator);
        const auto normalized = point_table.rows_by_name.find(point_name);
        if (normalized == point_table.rows_by_name.end()) return false;
        row = normalized->second;
        return true;
    }

    [[nodiscard]] bool ReadPointKeyDoor(
        const std::uintptr_t row,
        FNameValue& key_door) {
        if (const auto found = point_table.key_doors.find(row);
            found != point_table.key_doors.end()) {
            key_door = found->second;
            return true;
        }
        std::uintptr_t address{};
        if (!AddSignedAddress(row, kRobBankPointKeyDoorIdOffset, address) ||
            !Read(address, key_door)) {
            return false;
        }
        point_table.key_doors.emplace(row, key_door);
        return true;
    }

    [[nodiscard]] bool EvaluatePickability(
        const std::uintptr_t actor,
        bool& blocked) {
        blocked = false;
        std::uintptr_t root_address{};
        std::uintptr_t root{};
        bool delay_interact{};
        bool can_interact{};
        if (!AddSignedAddress(actor, kActorRootComponentOffset, root_address) ||
            !Read(root_address, root) ||
            !ReadFlag(
                actor, kRobBankDelayInteractOffset,
                kRobBankDelayInteractMask, delay_interact) ||
            !ReadFlag(
                actor, kRobBankCanInteractOffset,
                kRobBankCanInteractMask, can_interact)) {
            return false;
        }
        // OutInteractEntries is populated by the player's nearby-interaction query. It cannot
        // participate in map-wide pickability or distant BankBoxes remain falsely blocked.
        blocked = root == 0 || !delay_interact || !can_interact;
        if (blocked) return true;
        if (!point_table.available || !key_door_context_available) return false;

        std::uintptr_t point_uid_address{};
        FNameValue point_uid;
        if (!AddSignedAddress(actor, kRobBankPointUidOffset, point_uid_address) ||
            !Read(point_uid_address, point_uid)) {
            return false;
        }
        std::uintptr_t row{};
        if (!FindPointRow(point_uid, row)) {
            blocked = true;
            return true;
        }
        FNameValue key_door;
        if (!ReadPointKeyDoor(row, key_door)) return false;
        const std::uint64_t encoded = EncodeFName(key_door);
        blocked = encoded != 0 && !unlocked_key_doors.contains(encoded);
        return true;
    }

    [[nodiscard]] RobBankInspection Inspect(
        const std::uint64_t entity_id,
        const std::string_view expected_class_name) noexcept {
        RobBankInspection inspection;
        if (!started || !refreshed || !IsGameThread()) return inspection;
        try {
            std::uintptr_t actor{};
            std::uintptr_t class_object{};
            if (!ResolveEntity(
                    entity_id, expected_class_name, inspection.entity,
                    actor, class_object)) {
                return inspection;
            }
            ResolveItemMetadata(actor, inspection);
            bool blocked{};
            if (!EvaluatePickability(actor, blocked)) return inspection;
            inspection.pickability = blocked
                ? RobBankPickability::blocked
                : RobBankPickability::candidate;
            return inspection;
        } catch (...) {
            return {};
        }
    }
};

RobBankRuntime::RobBankRuntime() : impl_(std::make_unique<Impl>()) {}

RobBankRuntime::~RobBankRuntime() = default;

bool RobBankRuntime::Start(const AnomalyHostApiV1* const host) noexcept {
    Stop();
    if (host == nullptr) return false;
    try {
        const anomaly::sdk::Host services(host);
        impl_->core = services.Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID,
            ANOMALY_CORE_SERVICE_V1_VERSION).get();
        impl_->signatures = services.Query<AnomalySignatureServiceV1>(
            ANOMALY_SIGNATURE_SERVICE_V1_ID,
            ANOMALY_SIGNATURE_SERVICE_V1_VERSION).get();
        impl_->names = services.Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID,
            ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
        impl_->framework = services.Query<AnomalyUe5FrameworkServiceV1>(
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID,
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION).get();
        if (!HasField<AnomalyCoreServiceV1,
                decltype(AnomalyCoreServiceV1::read_memory)>(
                impl_->core, offsetof(AnomalyCoreServiceV1, read_memory)) ||
            impl_->core->read_memory == nullptr ||
            !HasField<AnomalySignatureServiceV1,
                decltype(AnomalySignatureServiceV1::resolve)>(
                impl_->signatures, offsetof(AnomalySignatureServiceV1, resolve)) ||
            impl_->signatures->resolve == nullptr ||
            !HasField<AnomalyUe5NamesServiceV1,
                decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
                impl_->names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) ||
            impl_->names->resolve_utf8 == nullptr ||
            !HasField<AnomalyUe5FrameworkServiceV1,
                decltype(AnomalyUe5FrameworkServiceV1::is_game_thread)>(
                impl_->framework,
                offsetof(AnomalyUe5FrameworkServiceV1, is_game_thread)) ||
            impl_->framework->is_game_thread == nullptr ||
            !impl_->ResolvePluginProfile()) {
            Stop();
            return false;
        }
        impl_->started = true;
        return true;
    } catch (...) {
        Stop();
        return false;
    }
}

void RobBankRuntime::Stop() noexcept {
    impl_->core = nullptr;
    impl_->signatures = nullptr;
    impl_->names = nullptr;
    impl_->framework = nullptr;
    impl_->world_storage = 0;
    impl_->object_registry_address = 0;
    impl_->pickup_function = 0;
    impl_->text_to_string_function = 0;
    impl_->free_string_function = 0;
    impl_->world = 0;
    impl_->player_controller = 0;
    impl_->registry = {};
    impl_->registry_generation = 0;
    impl_->point_table = {};
    impl_->item_tables = {};
    impl_->unlocked_key_doors.clear();
    impl_->rob_bank_classes.clear();
    impl_->rob_bank_clone_data_asset_classes.clear();
    impl_->key_door_context_available = false;
    impl_->started = false;
    impl_->refreshed = false;
}

bool RobBankRuntime::Refresh() noexcept {
    impl_->refreshed = false;
    if (!impl_->started || !impl_->IsGameThread()) return false;
    try {
        std::uintptr_t next_world{};
        std::uintptr_t next_controller{};
        ObjectRegistry next_registry;
        if (!impl_->ReadWorldAndController(next_world, next_controller) ||
            !impl_->LoadObjectRegistry(next_registry)) {
            return false;
        }
        if (next_world != impl_->world) impl_->ResetWorldState();
        if (impl_->RegistryIdentityChanged(next_registry)) {
            ++impl_->registry_generation;
            impl_->point_table = {};
            impl_->item_tables = {};
            impl_->rob_bank_classes.clear();
            impl_->rob_bank_clone_data_asset_classes.clear();
        }
        impl_->world = next_world;
        impl_->player_controller = next_controller;
        impl_->registry = next_registry;
        impl_->RefreshPointTable();
        impl_->RefreshItemTables();
        impl_->key_door_context_available = impl_->RefreshKeyDoorContext();
        impl_->refreshed = true;
        return true;
    } catch (...) {
        return false;
    }
}

RobBankContextRefresh RobBankRuntime::RefreshPickabilityContext() noexcept {
    if (!impl_->started || !impl_->refreshed || !impl_->IsGameThread()) {
        return RobBankContextRefresh::unavailable;
    }
    try {
        const bool context_was_available = impl_->key_door_context_available;
        std::uintptr_t live_world{};
        std::uintptr_t live_controller{};
        if (!impl_->ReadWorldAndController(live_world, live_controller) ||
            live_world != impl_->world || live_controller != impl_->player_controller) {
            impl_->key_door_context_available = false;
            return RobBankContextRefresh::unavailable;
        }
        bool changed{};
        if (!impl_->RefreshKeyDoorContext(&changed)) {
            impl_->key_door_context_available = false;
            return RobBankContextRefresh::unavailable;
        }
        impl_->key_door_context_available = true;
        return changed || !context_was_available
            ? RobBankContextRefresh::changed
            : RobBankContextRefresh::unchanged;
    } catch (...) {
        impl_->key_door_context_available = false;
        return RobBankContextRefresh::unavailable;
    }
}

RobBankInspection RobBankRuntime::Inspect(
    const std::uint64_t entity_id,
    const std::string_view expected_class_name) noexcept {
    return impl_->Inspect(entity_id, expected_class_name);
}

AnomalyStatusV1 RobBankRuntime::Pickup(const RobBankEntity entity) noexcept {
    if (!impl_->started || !impl_->refreshed) {
        return Status(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "Pink Paw RobBank runtime is unavailable");
    }
    if (!impl_->IsGameThread()) {
        return Status(
            ANOMALY_STATUS_V1_CONFLICT,
            "RobBank pickup requires the Game thread");
    }
    try {
        std::uintptr_t actor{};
        std::uintptr_t class_object{};
        if (!impl_->ResolveEntity(entity, actor, class_object)) {
            return Status(
                ANOMALY_STATUS_V1_NOT_FOUND,
                "RobBank entity identity changed");
        }
        bool blocked{};
        if (!impl_->EvaluatePickability(actor, blocked)) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "RobBank pickability is unavailable");
        }
        if (blocked) {
            return Status(
                ANOMALY_STATUS_V1_CONFLICT,
                "RobBank entity is not currently pickable");
        }

        std::uintptr_t live_world{};
        std::uintptr_t live_controller{};
        if (!impl_->ReadWorldAndController(live_world, live_controller) ||
            live_world != impl_->world || live_controller != impl_->player_controller) {
            return Status(
                ANOMALY_STATUS_V1_NOT_FOUND,
                "local player identity changed");
        }
        std::uintptr_t player_state{};
        std::uintptr_t player_state_vtable{};
        if (!impl_->ReadPointerAt(
                live_controller, kControllerPlayerStateOffset, player_state) ||
            !impl_->Read(player_state, player_state_vtable) ||
            player_state_vtable == 0) {
            return Status(
                ANOMALY_STATUS_V1_FAILED,
                "local PlayerState is unreadable");
        }

        std::uintptr_t interface_object{};
        std::uintptr_t interface_vtable{};
        std::uintptr_t method_slot{};
        std::uintptr_t method{};
        if (!AddSignedAddress(
                actor, kRobBankContainerInterfaceOffset, interface_object) ||
            !impl_->Read(interface_object, interface_vtable) || interface_vtable == 0 ||
            !AddSignedAddress(
                interface_vtable, kRobBankPickupVtableSlot, method_slot) ||
            !impl_->Read(method_slot, method) || method != impl_->pickup_function) {
            return Status(
                ANOMALY_STATUS_V1_CONFLICT,
                "RobBank pickup vtable changed");
        }

        using PickupFunction = bool(__fastcall*)(void*, void*);
        const auto pickup = reinterpret_cast<PickupFunction>(method);
        if (!pickup(
                reinterpret_cast<void*>(interface_object),
                reinterpret_cast<void*>(live_controller))) {
            return Status(
                ANOMALY_STATUS_V1_FAILED,
                "RobBank pickup returned false");
        }
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(
            ANOMALY_STATUS_V1_FAILED,
            "RobBank pickup invocation failed");
    }
}

bool RobBankRuntime::Available() const noexcept {
    return impl_->started;
}

bool RobBankRuntime::CanInspect() const noexcept {
    return impl_->started && impl_->refreshed;
}

bool RobBankRuntime::DiscoveryPending() const noexcept {
    return impl_->started &&
        (!impl_->refreshed ||
         (!impl_->point_table.available && !impl_->point_table.discovery_complete) ||
         (!impl_->item_tables.available && !impl_->item_tables.discovery_complete));
}

bool RobBankRuntime::PickabilityReady() const noexcept {
    return impl_->started && impl_->refreshed && impl_->point_table.available &&
        impl_->key_door_context_available;
}

}  // namespace pink_paw_heist_esp
