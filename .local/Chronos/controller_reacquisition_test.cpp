#include "plugin.cpp"

#include <cstdlib>
#include <iostream>

namespace {

constexpr AnomalyGenerationHandleV1 kStaleHandle{1, 1};
constexpr AnomalyGenerationHandleV1 kCurrentHandle{(7ULL << 32U) | 1U, 1};

struct Fixture final {
    std::array<std::uint8_t, 32> registry{};
    std::array<std::uintptr_t, 1> chunks{};
    std::array<std::uint8_t, kObjectItemStride> chunk{};
    std::array<std::uint8_t, 32> controller{};
    std::array<std::uint8_t, 80> controller_class{};
    std::uint32_t handle_lookups{};
    std::uint32_t index_lookups{};
    bool controller_available{true};
};

AnomalyStatusV1 OkStatus() noexcept {
    return {ANOMALY_STATUS_V1_OK, 0, {nullptr, 0}};
}

AnomalyStatusV1 NotFoundStatus() noexcept {
    return {ANOMALY_STATUS_V1_NOT_FOUND, 0, {nullptr, 0}};
}

AnomalyStatusV1 ANOMALY_CALL ReadMemory(
    void*, const std::uintptr_t address,
    const AnomalyMutableByteSpanV1 destination) noexcept {
    if (address == 0 || destination.data == nullptr) return NotFoundStatus();
    std::memcpy(destination.data, reinterpret_cast<const void*>(address), destination.size);
    return OkStatus();
}

AnomalyStatusV1 ANOMALY_CALL ResolveName(
    void*, const std::uint32_t name_id, char* const destination,
    std::size_t* const inout_size) noexcept {
    constexpr std::string_view name = kCurrentControllerName;
    if (name_id != 1 || inout_size == nullptr) return NotFoundStatus();
    const std::size_t required = name.size() + 1U;
    if (destination == nullptr) {
        *inout_size = required;
        return OkStatus();
    }
    if (*inout_size < required) return NotFoundStatus();
    std::memcpy(destination, name.data(), name.size());
    destination[name.size()] = '\0';
    *inout_size = required;
    return OkStatus();
}

std::uint64_t ANOMALY_CALL Generation(void*) noexcept {
    return 1;
}

std::uint32_t ANOMALY_CALL Count(void*) noexcept {
    return 1;
}

AnomalyStatusV1 ANOMALY_CALL SnapshotAt(
    void* const user, const std::uint32_t index,
    AnomalyUe5ObjectSnapshotV1* const snapshot) noexcept {
    auto& fixture = *static_cast<Fixture*>(user);
    ++fixture.index_lookups;
    if (!fixture.controller_available || index != 0 || snapshot == nullptr) {
        return NotFoundStatus();
    }
    *snapshot = {sizeof(*snapshot), 0, kCurrentHandle, 1, 0};
    return OkStatus();
}

AnomalyStatusV1 ANOMALY_CALL SnapshotByHandle(
    void* const user, const AnomalyGenerationHandleV1 handle,
    AnomalyUe5ObjectSnapshotV1* const snapshot) noexcept {
    auto& fixture = *static_cast<Fixture*>(user);
    ++fixture.handle_lookups;
    if (!fixture.controller_available || handle.id != kCurrentHandle.id ||
        snapshot == nullptr) {
        return NotFoundStatus();
    }
    *snapshot = {sizeof(*snapshot), 0, kCurrentHandle, 1, 0};
    return OkStatus();
}

bool Expect(const bool condition, const char* const message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

}  // namespace

int main() {
    Fixture fixture;
    const std::uintptr_t controller_address =
        reinterpret_cast<std::uintptr_t>(fixture.controller.data());
    const std::uintptr_t class_address =
        reinterpret_cast<std::uintptr_t>(fixture.controller_class.data());
    const std::uintptr_t chunks_address =
        reinterpret_cast<std::uintptr_t>(fixture.chunks.data());
    const std::uintptr_t chunk_address =
        reinterpret_cast<std::uintptr_t>(fixture.chunk.data());
    std::memcpy(
        fixture.registry.data() + kObjectRegistryItemsOffset,
        &chunks_address, sizeof(chunks_address));
    fixture.chunks[0] = chunk_address;
    std::memcpy(fixture.chunk.data(), &controller_address, sizeof(controller_address));
    const std::uint32_t serial = ANOMALY_UE5_OBJECT_HANDLE_SERIAL(kCurrentHandle);
    std::memcpy(
        fixture.chunk.data() + kObjectItemSerialOffset, &serial, sizeof(serial));
    std::memcpy(
        fixture.controller.data() + kObjectClassOffset,
        &class_address, sizeof(class_address));

    AnomalyCoreServiceV1 core{};
    core.struct_size = sizeof(core);
    core.service_version = ANOMALY_CORE_SERVICE_V1_VERSION;
    core.read_memory = ReadMemory;
    AnomalyUe5NamesServiceV1 names{};
    names.struct_size = sizeof(names);
    names.service_version = ANOMALY_UE5_NAMES_SERVICE_V1_VERSION;
    names.resolve_utf8 = ResolveName;
    AnomalyUe5ObjectsServiceV1 objects{};
    objects.struct_size = sizeof(objects);
    objects.service_version = ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION;
    objects.user = &fixture;
    objects.generation = Generation;
    objects.count = Count;
    objects.snapshot_at = SnapshotAt;
    objects.snapshot_by_handle = SnapshotByHandle;

    Context context;
    context.core = &core;
    context.names = &names;
    context.objects = &objects;
    context.object_registry = reinterpret_cast<std::uintptr_t>(fixture.registry.data());
    context.object_generation = 1;
    context.timed_pause_function = 1;
    context.player_controller_class = class_address;
    context.controller_handle = kStaleHandle;
    context.validated_controller = 1;
    context.state.store(
        static_cast<std::uint32_t>(ControlState::ready),
        std::memory_order_release);
    context.stopping.store(false, std::memory_order_release);

    Update(&context, 0.0);
    bool passed = true;
    passed &= Expect(
        context.validated_controller == controller_address,
        "stale controller was not reacquired in the same update");
    passed &= Expect(
        context.controller_handle.id == kCurrentHandle.id,
        "reacquired controller handle was not cached");
    passed &= Expect(
        static_cast<ControlState>(context.state.load(std::memory_order_acquire)) ==
            ControlState::ready,
        "controller state did not return to ready");
    passed &= Expect(
        fixture.handle_lookups == 1 && fixture.index_lookups == 1,
        "first update did not validate the stale handle and scan for a replacement");

    Update(&context, 0.0);
    passed &= Expect(
        fixture.handle_lookups == 2 && fixture.index_lookups == 1,
        "stable replacement was not retained on the next update");

    fixture.controller_available = false;
    Update(&context, 0.0);
    passed &= Expect(
        context.validated_controller == 0 && context.controller_scan_complete &&
            context.controller_rescan_delay_ticks == kControllerRescanDelayTicks,
        "missing replacement did not schedule another scan");
    const std::uint32_t lookups_before_retry = fixture.index_lookups;
    fixture.controller_available = true;
    for (std::uint32_t tick = 0; tick < kControllerRescanDelayTicks; ++tick) {
        Update(&context, 0.0);
    }
    passed &= Expect(
        context.validated_controller == 0 &&
            fixture.index_lookups == lookups_before_retry,
        "replacement scan was not throttled between complete passes");
    Update(&context, 0.0);
    passed &= Expect(
        context.validated_controller == controller_address &&
            fixture.index_lookups == lookups_before_retry + 1U,
        "controller appearing after a complete pass was not reacquired");
    if (!passed) return EXIT_FAILURE;
    std::cout << "Chronos controller reacquisition test passed\n";
    return EXIT_SUCCESS;
}
