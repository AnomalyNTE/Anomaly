#include "plugins/PinkPawHeistESP/loot_class_cache.hpp"
#include "plugins/PinkPawHeistESP/rob_bank_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::uintptr_t kGWorldInstruction = 0x100000;
constexpr std::uintptr_t kGObjectsInstruction = 0x100100;
constexpr std::uintptr_t kWorldStorage = 0x200000;
constexpr std::uintptr_t kObjectRegistry = 0x210000;
constexpr std::uintptr_t kWorld = 0x220000;
constexpr std::uintptr_t kGameInstance = 0x230000;
constexpr std::uintptr_t kLocalPlayers = 0x240000;
constexpr std::uintptr_t kLocalPlayer = 0x250000;
constexpr std::uintptr_t kController = 0x260000;
constexpr std::uintptr_t kPlayerState = 0x270000;
constexpr std::uintptr_t kObjectChunks = 0x280000;
constexpr std::uintptr_t kObjectChunk = 0x290000;
constexpr std::uintptr_t kActor = 0x300000;
constexpr std::uintptr_t kActorClass = 0x310000;
constexpr std::uintptr_t kRobBankBaseClass = 0x311000;
constexpr std::uintptr_t kRootComponent = 0x320000;
constexpr std::uintptr_t kInterfaceVtable = 0x330000;
constexpr std::uintptr_t kInteractEntries = 0x340000;
constexpr std::uintptr_t kPointTable = 0x350000;
constexpr std::uintptr_t kDataTableClass = 0x351000;
constexpr std::uintptr_t kPointTableOuter = 0x352000;
constexpr std::uintptr_t kPointRows = 0x353000;
constexpr std::uintptr_t kPointRow = 0x354000;

constexpr std::uint32_t kActorSerial = 101;
constexpr std::uint32_t kActorNameId = 1;
constexpr std::uint32_t kRobBankBaseNameId = 2;
constexpr std::uint32_t kPointTableNameId = 3;
constexpr std::uint32_t kDataTableNameId = 4;
constexpr std::uint32_t kPointTableOuterNameId = 5;
constexpr std::uint32_t kPointUidNameId = 6;
constexpr std::uint32_t kWorldMarkerNameId = 7;
constexpr std::uint32_t kOtherClassNameId = 8;

std::uintptr_t g_expected_interface{};
std::uintptr_t g_expected_controller{};
std::uint32_t g_pickup_calls{};
bool g_pickup_result{true};

bool __fastcall PickupStub(void* interface_object, void* player_controller) {
    ++g_pickup_calls;
    return reinterpret_cast<std::uintptr_t>(interface_object) == g_expected_interface &&
        reinterpret_cast<std::uintptr_t>(player_controller) == g_expected_controller &&
        g_pickup_result;
}

AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
}

bool Check(const bool condition, const char* const message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
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

class Fixture final {
public:
    Fixture() {
        core_ = {
            sizeof(core_), ANOMALY_CORE_SERVICE_V1_VERSION, this,
            nullptr, ReadMemory, nullptr, nullptr};
        signatures_ = {
            sizeof(signatures_), ANOMALY_SIGNATURE_SERVICE_V1_VERSION,
            this, ResolveSignature};
        names_ = {
            sizeof(names_), ANOMALY_UE5_NAMES_SERVICE_V1_VERSION,
            this, ResolveName};
        framework_ = {
            sizeof(framework_), ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION,
            this, nullptr, nullptr, IsGameThread};
        session_ = {
            sizeof(session_), ANOMALY_NTE_SESSION_SERVICE_V1_VERSION,
            this, SessionSnapshot, nullptr, nullptr};
        entities_ = {
            sizeof(entities_), ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION,
            this, EntityFrame, nullptr, EntityClassName, nullptr, EntityPage,
            nullptr, nullptr, nullptr};
        host_ = {
            sizeof(host_), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
            this, {}, QueryService};
        names_by_id_ = {
            {kActorNameId, "BankBox_Test_C"},
            {kRobBankBaseNameId, "HTRobBankItemActor"},
            {kPointTableNameId, "DT_RobBankPoint"},
            {kDataTableNameId, "DataTable"},
            {kPointTableOuterNameId, "/Game/DataTable/RobBank/DT_RobBankPoint"},
            {kPointUidNameId, "RobBankPoint_Test"},
            {kWorldMarkerNameId, std::string(pink_paw_heist_esp::kWorldMarkerClassName)},
            {kOtherClassNameId, "OpenWorldActor_C"}};
        Populate();
    }

    const AnomalyHostApiV1* Host() const noexcept { return &host_; }

    template <typename T>
    void Put(const std::uintptr_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t index{}; index < sizeof(value); ++index) {
            memory_[address + index] = bytes[index];
        }
    }

    void SetGameThread(const bool value) noexcept { game_thread_ = value; }
    void ProvideSignatures(const bool value) noexcept { provide_signatures_ = value; }
    void SetWorld(const AnomalyGenerationHandleV1 world) noexcept { world_ = world; }
    void SetEntityClassNames(std::vector<std::uint32_t> names) {
        entity_class_names_ = std::move(names);
        ++entity_generation_;
        ++entity_sequence_;
    }
    void SetResolvedClassName(const std::uint32_t id, std::string name) {
        names_by_id_[id] = std::move(name);
    }
    [[nodiscard]] std::uint32_t EntityPageCalls() const noexcept {
        return entity_page_calls_;
    }
    [[nodiscard]] std::uint32_t NameCalls() const noexcept { return name_calls_; }
    [[nodiscard]] std::uint32_t EntityClassNameCalls() const noexcept {
        return entity_class_name_calls_;
    }
    [[nodiscard]] const AnomalyNteEntitiesServiceV1* Entities() const noexcept {
        return &entities_;
    }

private:
    static std::string_view View(const AnomalyStringViewV1 value) noexcept {
        return value.data == nullptr ? std::string_view{} :
            std::string_view(value.data, value.size);
    }

    static AnomalyStatusV1 ANOMALY_CALL QueryService(
        void* user,
        const AnomalyStringViewV1 id,
        const std::uint32_t minimum_version,
        const void** output) noexcept {
        if (user == nullptr || output == nullptr) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& fixture = *static_cast<Fixture*>(user);
        *output = nullptr;
        const std::string_view service = View(id);
        if (service == ANOMALY_CORE_SERVICE_V1_ID &&
            minimum_version <= fixture.core_.service_version) {
            *output = &fixture.core_;
        } else if (service == ANOMALY_SIGNATURE_SERVICE_V1_ID &&
            fixture.provide_signatures_ &&
            minimum_version <= fixture.signatures_.service_version) {
            *output = &fixture.signatures_;
        } else if (service == ANOMALY_UE5_NAMES_SERVICE_V1_ID &&
            minimum_version <= fixture.names_.service_version) {
            *output = &fixture.names_;
        } else if (service == ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID &&
            minimum_version <= fixture.framework_.service_version) {
            *output = &fixture.framework_;
        } else if (service == ANOMALY_NTE_SESSION_SERVICE_V1_ID &&
            minimum_version <= fixture.session_.service_version) {
            *output = &fixture.session_;
        } else if (service == ANOMALY_NTE_ENTITIES_SERVICE_V1_ID &&
            minimum_version <= fixture.entities_.service_version) {
            *output = &fixture.entities_;
        }
        return *output == nullptr
            ? Status(ANOMALY_STATUS_V1_NOT_FOUND)
            : Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ReadMemory(
        void* user,
        const std::uintptr_t address,
        const AnomalyMutableByteSpanV1 destination) noexcept {
        if (user == nullptr || destination.data == nullptr || destination.size == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& fixture = *static_cast<Fixture*>(user);
        constexpr std::array<std::uint8_t, 16> prologue{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
            0xEC, 0x40, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
        constexpr std::array<std::uint8_t, 13> success{
            0xB0, 0x01, 0x48, 0x8B, 0x5C, 0x24, 0x58,
            0x48, 0x83, 0xC4, 0x40, 0x5F, 0xC3};
        constexpr std::array<std::uint8_t, 13> failure{
            0x48, 0x8B, 0x5C, 0x24, 0x58, 0x32, 0xC0,
            0x48, 0x83, 0xC4, 0x40, 0x5F, 0xC3};
        const std::uintptr_t pickup = reinterpret_cast<std::uintptr_t>(&PickupStub);
        const auto copy_known = [&](const std::uintptr_t expected_address, const auto& bytes) {
            if (address != expected_address || destination.size != bytes.size()) return false;
            std::memcpy(destination.data, bytes.data(), bytes.size());
            return true;
        };
        if (copy_known(pickup, prologue) ||
            copy_known(pickup + 0xD6U, success) ||
            copy_known(pickup + 0xE3U, failure)) {
            return Status(ANOMALY_STATUS_V1_OK);
        }
        for (std::size_t index{}; index < destination.size; ++index) {
            const auto found = fixture.memory_.find(address + index);
            if (found == fixture.memory_.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            destination.data[index] = found->second;
        }
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ResolveSignature(
        void* user,
        const AnomalyStringViewV1 module,
        const AnomalyStringViewV1 section,
        const AnomalyStringViewV1 pattern,
        std::uintptr_t* output) noexcept {
        if (user == nullptr || output == nullptr || View(module) != "HTGame.exe" ||
            View(section) != ".text") {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& fixture = *static_cast<Fixture*>(user);
        ++fixture.signature_calls_;
        const std::string_view value = View(pattern);
        if (value.starts_with("48 8B 1D")) {
            *output = kGWorldInstruction;
        } else if (value.starts_with("48 8B 05")) {
            *output = kGObjectsInstruction;
        } else if (value.starts_with("48 89 5C 24 10 57")) {
            *output = reinterpret_cast<std::uintptr_t>(&PickupStub);
        } else {
            *output = 0;
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ResolveName(
        void* user,
        const std::uint32_t name_id,
        char* destination,
        std::size_t* size) noexcept {
        if (user == nullptr || size == nullptr) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& fixture = *static_cast<Fixture*>(user);
        ++fixture.name_calls_;
        const auto found = fixture.names_by_id_.find(name_id);
        if (found == fixture.names_by_id_.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const std::size_t required = found->second.size() + 1U;
        if (destination == nullptr || *size < required) {
            *size = required;
            return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
        }
        std::memcpy(destination, found->second.c_str(), required);
        *size = required;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static int ANOMALY_CALL IsGameThread(void* user) noexcept {
        return user != nullptr && static_cast<Fixture*>(user)->game_thread_ ? 1 : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL SessionSnapshot(
        void* user,
        AnomalyNteSessionSnapshotV1* snapshot) noexcept {
        if (user == nullptr || snapshot == nullptr ||
            snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        const auto& fixture = *static_cast<Fixture*>(user);
        *snapshot = {
            sizeof(*snapshot), ANOMALY_NTE_SESSION_V1_WORLD_READY,
            fixture.entity_sequence_, fixture.world_};
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityFrame(
        void* user,
        AnomalyNteEntityFrameV1* frame) noexcept {
        if (user == nullptr || frame == nullptr || frame->struct_size < sizeof(*frame)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        const auto& fixture = *static_cast<Fixture*>(user);
        *frame = AnomalyNteEntityFrameV1{sizeof(*frame)};
        frame->flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
        frame->generation = fixture.entity_generation_;
        frame->sequence = fixture.entity_sequence_;
        frame->entity_count = static_cast<std::uint32_t>(fixture.entity_class_names_.size());
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityClassName(
        void* user,
        const std::uint64_t class_id,
        char* destination,
        std::size_t* size) noexcept {
        if (user == nullptr || size == nullptr ||
            class_id > (std::numeric_limits<std::uint32_t>::max)()) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& fixture = *static_cast<Fixture*>(user);
        ++fixture.entity_class_name_calls_;
        const auto found = fixture.names_by_id_.find(static_cast<std::uint32_t>(class_id));
        if (found == fixture.names_by_id_.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const std::size_t required = found->second.size() + 1U;
        if (destination == nullptr) {
            *size = required;
            return Status(ANOMALY_STATUS_V1_OK);
        }
        if (*size < required) {
            *size = required;
            return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
        }
        std::memcpy(destination, found->second.c_str(), required);
        *size = required;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityPage(
        void* user,
        const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination,
        AnomalyNteEntityPageResultV1* result) noexcept {
        if (user == nullptr || request == nullptr || result == nullptr ||
            request->struct_size < sizeof(*request) || result->struct_size < sizeof(*result) ||
            request->capacity > ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY ||
            (request->capacity != 0 && destination == nullptr)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& fixture = *static_cast<Fixture*>(user);
        ++fixture.entity_page_calls_;
        if (request->generation != fixture.entity_generation_) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }

        std::vector<std::size_t> matches;
        for (std::size_t index{}; index < fixture.entity_class_names_.size(); ++index) {
            if (request->class_name_id == 0 ||
                request->class_name_id == fixture.entity_class_names_[index]) {
                matches.push_back(index);
            }
        }
        const std::uint32_t total = static_cast<std::uint32_t>(matches.size());
        const std::uint32_t begin = (std::min)(request->offset, total);
        const std::uint32_t returned = (std::min)(request->capacity, total - begin);
        for (std::uint32_t index{}; index < returned; ++index) {
            if (destination[index].struct_size < sizeof(destination[index])) {
                return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
            }
            const std::size_t source = matches[begin + index];
            destination[index] = AnomalyNteEntitySnapshotV1{sizeof(destination[index])};
            destination[index].flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
            destination[index].handle = {
                static_cast<std::uint64_t>(source + 1U), fixture.entity_generation_};
            destination[index].entity_id = source + 1U;
            destination[index].class_id = fixture.entity_class_names_[source];
            destination[index].class_name_id = fixture.entity_class_names_[source];
        }
        *result = AnomalyNteEntityPageResultV1{sizeof(*result)};
        result->flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
        result->generation = fixture.entity_generation_;
        result->sequence = fixture.entity_sequence_;
        result->total_matches = total;
        result->returned = returned;
        result->next_offset = begin + returned;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    void PutRipTarget(
        const std::uintptr_t instruction,
        const std::uintptr_t target) {
        const auto displacement = static_cast<std::int32_t>(
            static_cast<std::intptr_t>(target) -
            static_cast<std::intptr_t>(instruction + 7U));
        Put(instruction + 3U, displacement);
    }

    void Populate() {
        PutRipTarget(kGWorldInstruction, kWorldStorage);
        PutRipTarget(kGObjectsInstruction, kObjectRegistry + 16U);
        Put(kWorldStorage, kWorld);
        Put(kWorld + 560U, kGameInstance);
        Put(kGameInstance + 56U, kLocalPlayers);
        const std::int32_t local_player_count = 1;
        Put(kGameInstance + 64U, local_player_count);
        Put(kLocalPlayers, kLocalPlayer);
        Put(kLocalPlayer + 48U, kController);
        Put(kController + 720U, kPlayerState);
        Put(kPlayerState, std::uintptr_t{0x400000});
        Put(kPlayerState + 36928U, ArrayHeader{});

        Put(kObjectRegistry + 16U, kObjectChunks);
        const std::uint32_t maximum_objects = 65536;
        const std::uint32_t object_count = 2;
        const std::uint32_t maximum_chunks = 1;
        const std::uint32_t chunk_count = 1;
        Put(kObjectRegistry + 32U, maximum_objects);
        Put(kObjectRegistry + 36U, object_count);
        Put(kObjectRegistry + 40U, maximum_chunks);
        Put(kObjectRegistry + 44U, chunk_count);
        Put(kObjectChunks, kObjectChunk);
        Put(kObjectChunk, kActor);
        Put(kObjectChunk + 16U, kActorSerial);
        Put(kObjectChunk + 24U, kPointTable);
        Put(kObjectChunk + 40U, std::uint32_t{202});

        Put(kActor + 12U, std::int32_t{0});
        Put(kActor + 16U, kActorClass);
        Put(kActor + 456U, kRootComponent);
        Put(kActor + 2944U, FNameValue{kPointUidNameId, 0});
        Put(kActor + 3064U, std::uint8_t{1});
        Put(kActor + 3112U, ArrayHeader{kInteractEntries, 1, 1});
        Put(kActor + 3136U, std::uint8_t{1});
        Put(kActor + 864U, kInterfaceVtable);
        Put(kInterfaceVtable + 24U, reinterpret_cast<std::uintptr_t>(&PickupStub));

        Put(kActorClass + 24U, kActorNameId);
        Put(kActorClass + 64U, kRobBankBaseClass);
        Put(kRobBankBaseClass + 24U, kRobBankBaseNameId);

        Put(kPointTable + 24U, kPointTableNameId);
        Put(kPointTable + 16U, kDataTableClass);
        Put(kPointTable + 32U, kPointTableOuter);
        Put(kDataTableClass + 24U, kDataTableNameId);
        Put(kPointTableOuter + 24U, kPointTableOuterNameId);
        Put(kPointTable + 48U, ArrayHeader{kPointRows, 1, 1});
        Put(kPointRows, FNameValue{kPointUidNameId, 0});
        Put(kPointRows + 8U, kPointRow);
        Put(kPointRows + 16U, std::uint64_t{});
        Put(kPointRow + 196U, FNameValue{});

        g_expected_interface = kActor + 864U;
        g_expected_controller = kController;
        g_pickup_calls = 0;
        g_pickup_result = true;
    }

    AnomalyCoreServiceV1 core_{};
    AnomalySignatureServiceV1 signatures_{};
    AnomalyUe5NamesServiceV1 names_{};
    AnomalyUe5FrameworkServiceV1 framework_{};
    AnomalyNteSessionServiceV1 session_{};
    AnomalyNteEntitiesServiceV1 entities_{};
    AnomalyHostApiV1 host_{};
    std::unordered_map<std::uintptr_t, std::uint8_t> memory_;
    std::unordered_map<std::uint32_t, std::string> names_by_id_;
    std::uint32_t signature_calls_{};
    AnomalyGenerationHandleV1 world_{100, 1};
    std::vector<std::uint32_t> entity_class_names_{kOtherClassNameId};
    std::uint64_t entity_generation_{70};
    std::uint64_t entity_sequence_{1};
    std::uint32_t entity_page_calls_{};
    std::uint32_t entity_class_name_calls_{};
    std::uint32_t name_calls_{};
    bool game_thread_{true};
    bool provide_signatures_{true};
};

bool RequiredSignatureServiceIsEnforced() {
    Fixture fixture;
    fixture.ProvideSignatures(false);
    pink_paw_heist_esp::RobBankRuntime runtime;
    return Check(
        !runtime.Start(fixture.Host()) && !runtime.Available(),
        "Pink Paw runtime started without the required signature service");
}

bool RuntimeOwnsRobBankValidationAndInvocation() {
    Fixture fixture;
    pink_paw_heist_esp::RobBankRuntime runtime;
    bool result = Check(runtime.Start(fixture.Host()), "Pink Paw runtime did not start");
    result = Check(runtime.Refresh(), "Pink Paw runtime did not refresh") && result;

    const auto candidate = runtime.Inspect(1, "BankBox_Test_C");
    result = Check(
        candidate.entity.object_index == 0 &&
            candidate.entity.object_serial == kActorSerial &&
            candidate.pickability == pink_paw_heist_esp::RobBankPickability::candidate,
        "Pink Paw runtime did not identify a pickable BankBox") && result;

    fixture.Put(kActor + 3064U, std::uint8_t{0});
    const auto blocked = runtime.Inspect(1, "BankBox_Test_C");
    result = Check(
        blocked.pickability == pink_paw_heist_esp::RobBankPickability::blocked &&
            runtime.Pickup(candidate.entity).code == ANOMALY_STATUS_V1_CONFLICT,
        "Pink Paw runtime did not enforce the RobBank interaction flags") && result;
    fixture.Put(kActor + 3064U, std::uint8_t{1});

    fixture.Put(kObjectChunk + 16U, std::uint32_t{kActorSerial + 1U});
    result = Check(
        runtime.Pickup(candidate.entity).code == ANOMALY_STATUS_V1_NOT_FOUND,
        "Pink Paw runtime accepted a stale UObject serial") && result;
    fixture.Put(kObjectChunk + 16U, kActorSerial);

    fixture.Put(kInterfaceVtable + 24U, std::uintptr_t{0x1234});
    result = Check(
        runtime.Pickup(candidate.entity).code == ANOMALY_STATUS_V1_CONFLICT,
        "Pink Paw runtime accepted a changed pickup vtable") && result;
    fixture.Put(kInterfaceVtable + 24U, reinterpret_cast<std::uintptr_t>(&PickupStub));

    fixture.SetGameThread(false);
    result = Check(
        runtime.Pickup(candidate.entity).code == ANOMALY_STATUS_V1_CONFLICT,
        "Pink Paw runtime escaped the Game callback domain") && result;
    fixture.SetGameThread(true);

    result = Check(
        runtime.Pickup(candidate.entity).code == ANOMALY_STATUS_V1_OK &&
            g_pickup_calls == 1,
        "Pink Paw runtime did not invoke the validated native pickup") && result;
    runtime.Stop();
    return Check(!runtime.Available(), "Pink Paw runtime remained available after stop") && result;
}

bool WorldGateUsesOneCachedFNameMarker() {
    Fixture fixture;
    pink_paw_heist_esp::PinkPawWorldGate gate;
    fixture.SetEntityClassNames(std::vector<std::uint32_t>(300, kOtherClassNameId));
    bool result = Check(
        gate.Refresh(fixture.Host()) == pink_paw_heist_esp::PinkPawWorldState::outside,
        "Pink Paw world gate accepted an unrelated World");
    const std::uint32_t outside_page_calls = fixture.EntityPageCalls();
    const std::uint32_t outside_name_calls = fixture.NameCalls();
    result = Check(
        outside_page_calls == 2 &&
            gate.Refresh(fixture.Host()) == pink_paw_heist_esp::PinkPawWorldState::outside &&
            fixture.EntityPageCalls() == outside_page_calls &&
            fixture.NameCalls() == outside_name_calls,
        "Pink Paw world gate rescanned a completed negative World") && result;

    fixture.SetWorld({100, 2});
    std::vector<std::uint32_t> marker_on_second_page(300, kOtherClassNameId);
    marker_on_second_page.push_back(kWorldMarkerNameId);
    fixture.SetEntityClassNames(std::move(marker_on_second_page));
    result = Check(
        gate.Refresh(fixture.Host()) == pink_paw_heist_esp::PinkPawWorldState::active,
        "Pink Paw world gate did not identify its unique class marker") && result;
    const std::uint32_t marker_name_calls = fixture.NameCalls();

    fixture.SetWorld({100, 3});
    fixture.SetEntityClassNames({kOtherClassNameId});
    result = Check(
        gate.Refresh(fixture.Host()) == pink_paw_heist_esp::PinkPawWorldState::outside &&
            fixture.NameCalls() == marker_name_calls,
        "Pink Paw world gate did not reuse the cached marker FName id") && result;

    fixture.SetEntityClassNames({kWorldMarkerNameId});
    gate.Invalidate();
    result = Check(
        gate.Refresh(fixture.Host()) == pink_paw_heist_esp::PinkPawWorldState::active,
        "Pink Paw world gate did not honor an explicit same-World refresh") && result;
    gate.Reset();
    return result;
}

bool LootClassMetadataIsResolvedOncePerClassIdentity() {
    Fixture fixture;
    pink_paw_heist_esp::LootClassCache cache;
    const pink_paw_heist_esp::LootClassMetadata* metadata{};

    const auto first = cache.Resolve(
        fixture.Entities(), kActorNameId, kActorNameId, metadata);
    bool result = Check(
        first == pink_paw_heist_esp::LootClassResolution::resolved &&
            metadata != nullptr && metadata->bank_box &&
            metadata->name == "BankBox_Test_C" &&
            fixture.EntityClassNameCalls() == 2,
        "Pink Paw loot class cache did not resolve the initial class identity");

    metadata = nullptr;
    const auto cached = cache.Resolve(
        fixture.Entities(), kActorNameId, kActorNameId, metadata);
    result = Check(
        cached == pink_paw_heist_esp::LootClassResolution::resolved &&
            metadata != nullptr && metadata->bank_box &&
            fixture.EntityClassNameCalls() == 2,
        "Pink Paw loot class cache repeated a class-name service lookup") && result;

    metadata = nullptr;
    fixture.SetResolvedClassName(kActorNameId, "HTRobBankItemActor");
    const auto replaced = cache.Resolve(
        fixture.Entities(), kActorNameId, kRobBankBaseNameId, metadata);
    return Check(
        replaced == pink_paw_heist_esp::LootClassResolution::resolved &&
            metadata != nullptr && !metadata->bank_box &&
            metadata->name == "HTRobBankItemActor" &&
            fixture.EntityClassNameCalls() == 4,
        "Pink Paw loot class cache did not invalidate a changed class identity") && result;
}

}  // namespace

int main() {
    bool result = RequiredSignatureServiceIsEnforced();
    result = RuntimeOwnsRobBankValidationAndInvocation() && result;
    result = WorldGateUsesOneCachedFNameMarker() && result;
    result = LootClassMetadataIsResolvedOncePerClassIdentity() && result;
    return result ? 0 : 1;
}
