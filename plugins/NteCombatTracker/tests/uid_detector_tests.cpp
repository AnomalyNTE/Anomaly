#include "uid_detector.hpp"

#include <anomaly/sdk/anomaly_sdk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kRoleIdName = 42;
constexpr std::uint32_t kOtherName = 43;

AnomalyStatusV1 Ok() { return {ANOMALY_STATUS_V1_OK, 0, {}}; }
AnomalyStatusV1 Missing() { return {ANOMALY_STATUS_V1_NOT_FOUND, 0, {}}; }

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string_view View(const AnomalyStringViewV1 value) {
    return {value.data, value.size};
}

struct UnrealString {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct UnrealText {
    void* data{};
    std::uint32_t flags{};
    std::uint32_t padding{};
};

struct TextValue {
    std::wstring value;
    std::uint32_t failures{};
};

UnrealString* ANOMALY_CALL TextToString(
    UnrealString* output, const UnrealText* text) {
    auto* value = static_cast<TextValue*>(text->data);
    if (value == nullptr) return nullptr;
    if (value->failures != 0) {
        --value->failures;
        return nullptr;
    }
    output->count = static_cast<std::int32_t>(value->value.size() + 1);
    output->capacity = output->count;
    output->data = new wchar_t[static_cast<std::size_t>(output->count)];
    std::copy(value->value.begin(), value->value.end(), output->data);
    output->data[output->count - 1] = L'\0';
    return output;
}

void ANOMALY_CALL FreeString(void* value) {
    delete[] static_cast<wchar_t*>(value);
}

struct FakeRuntime {
    struct Object {
        std::array<std::byte, 1024> bytes{};
        TextValue text;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(AnomalyUe5ObjectSnapshotV1)};
    };

    FakeRuntime() {
        host.struct_size = sizeof(host);
        host.host_context = this;
        host.query_service = QueryService;

        signature = {sizeof(signature), ANOMALY_SIGNATURE_SERVICE_V1_VERSION,
            this, ResolveSignature};
        objects = {sizeof(objects), ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION,
            this, Generation, Count, SnapshotAt, SnapshotByHandle, FindExact};
        names = {sizeof(names), ANOMALY_UE5_NAMES_SERVICE_V1_VERSION,
            this, ResolveName};

        vtable.fill(0);
        set_text = 0x123456789ABCULL;
        vtable[856 / sizeof(std::uintptr_t)] = set_text;
        chunks[0] = reinterpret_cast<std::uintptr_t>(items.data());
        *reinterpret_cast<std::uintptr_t**>(registry.data() + 16) = chunks.data();

        const auto accessor = reinterpret_cast<std::uintptr_t>(gobjects.data());
        const auto registry_address = reinterpret_cast<std::uintptr_t>(registry.data());
        const auto displacement = static_cast<std::int32_t>(
            registry_address - (accessor + 7) + 16);
        std::memcpy(gobjects.data() + 3, &displacement, sizeof(displacement));
        AddObject(L"template", 0, true);
    }

    Object& AddObject(
        std::wstring text, const std::uint32_t failures = 0,
        const bool is_template = false,
        const std::uint32_t name_id = kRoleIdName) {
        const std::uint32_t index = static_cast<std::uint32_t>(entries.size());
        const std::uint32_t serial = index + 10;
        auto object = std::make_unique<Object>();
        object->text = {std::move(text), failures};
        *reinterpret_cast<std::uintptr_t*>(object->bytes.data()) =
            reinterpret_cast<std::uintptr_t>(vtable.data());
        *reinterpret_cast<std::uint32_t*>(object->bytes.data() + 24) = name_id;
        auto* unreal_text = reinterpret_cast<UnrealText*>(object->bytes.data() + 392);
        unreal_text->data = &object->text;
        object->snapshot.handle = {
            (static_cast<std::uint64_t>(serial) << 32U) | (index + 1U), generation};
        object->snapshot.name_id = name_id;

        auto* item = items.data() + static_cast<std::size_t>(index) * 24;
        *reinterpret_cast<std::uintptr_t*>(item) =
            reinterpret_cast<std::uintptr_t>(object->bytes.data());
        *reinterpret_cast<std::uint32_t*>(item + 16) = serial;
        entries.push_back(std::move(object));
        if (is_template) template_handle = entries.back()->snapshot.handle;
        return *entries.back();
    }

    void MakeRoleId(const std::size_t index, std::wstring text) {
        auto& object = *entries.at(index);
        object.text.value = std::move(text);
        object.snapshot.name_id = kRoleIdName;
        *reinterpret_cast<std::uint32_t*>(object.bytes.data() + 24) = kRoleIdName;
    }

    void ShrinkTo(const std::size_t count) {
        entries.resize(count);
    }

    void ReplaceRoleId(const std::size_t index, std::wstring text,
        const std::uint32_t serial) {
        auto& object = *entries.at(index);
        object.text.value = std::move(text);
        object.snapshot.handle.id = (static_cast<std::uint64_t>(serial) << 32U) |
            static_cast<std::uint64_t>(index + 1U);
        object.snapshot.name_id = kRoleIdName;
        auto* item = items.data() + index * 24;
        *reinterpret_cast<std::uint32_t*>(item + 16) = serial;
    }

    void ChangeGeneration(const std::wstring& uid) {
        ++generation;
        entries.clear();
        items.fill(std::byte{});
        AddObject(L"template", 0, true);
        AddObject(uid);
    }

    static AnomalyStatusV1 ANOMALY_CALL QueryService(
        void* user, const AnomalyStringViewV1 id, std::uint32_t, const void** service) {
        auto& self = *static_cast<FakeRuntime*>(user);
        if (!self.services_available) return Missing();
        if (View(id) == ANOMALY_SIGNATURE_SERVICE_V1_ID) *service = &self.signature;
        else if (View(id) == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) *service = &self.objects;
        else if (View(id) == ANOMALY_UE5_NAMES_SERVICE_V1_ID) *service = &self.names;
        else return Missing();
        return Ok();
    }

    static AnomalyStatusV1 ANOMALY_CALL ResolveSignature(
        void* user, AnomalyStringViewV1, AnomalyStringViewV1,
        const AnomalyStringViewV1 pattern, std::uintptr_t* target) {
        auto& self = *static_cast<FakeRuntime*>(user);
        if (!self.bindings_available) return Missing();
        const auto value = View(pattern);
        if (value.starts_with("48 89 5C 24 10")) *target = self.set_text;
        else if (value.starts_with("48 85 C9 74 2E")) {
            *target = reinterpret_cast<std::uintptr_t>(&FreeString);
        } else if (value.starts_with("40 53 48 83 EC 20")) {
            *target = reinterpret_cast<std::uintptr_t>(&TextToString);
        } else if (value.starts_with("48 8B 05")) {
            *target = reinterpret_cast<std::uintptr_t>(self.gobjects.data());
        } else return Missing();
        return Ok();
    }

    static std::uint64_t ANOMALY_CALL Generation(void* user) {
        return static_cast<FakeRuntime*>(user)->generation;
    }
    static std::uint32_t ANOMALY_CALL Count(void* user) {
        return static_cast<std::uint32_t>(
            static_cast<FakeRuntime*>(user)->entries.size());
    }
    static AnomalyStatusV1 ANOMALY_CALL SnapshotAt(
        void* user, const std::uint32_t index, AnomalyUe5ObjectSnapshotV1* output) {
        auto& self = *static_cast<FakeRuntime*>(user);
        if (index >= self.entries.size()) return Missing();
        *output = self.entries[index]->snapshot;
        return Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL SnapshotByHandle(
        void* user, const AnomalyGenerationHandleV1 handle,
        AnomalyUe5ObjectSnapshotV1* output) {
        auto& self = *static_cast<FakeRuntime*>(user);
        for (const auto& entry : self.entries) {
            if (entry->snapshot.handle.id == handle.id &&
                entry->snapshot.handle.generation == handle.generation) {
                *output = entry->snapshot;
                return Ok();
            }
        }
        return Missing();
    }
    static AnomalyStatusV1 ANOMALY_CALL FindExact(
        void* user, AnomalyStringViewV1, AnomalyGenerationHandleV1* output) {
        *output = static_cast<FakeRuntime*>(user)->template_handle;
        return Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL ResolveName(
        void*, const std::uint32_t id, char* output, std::size_t* size) {
        if (size == nullptr) return Missing();
        const std::string_view name = id == kRoleIdName ?
            "TextBlock_RoleID" : "UnrelatedWidget";
        if (output == nullptr) {
            *size = name.size() + 1;
            return Ok();
        }
        if (*size < name.size() + 1) return Missing();
        std::memcpy(output, name.data(), name.size());
        output[name.size()] = '\0';
        *size = name.size() + 1;
        return Ok();
    }

    AnomalyHostApiV1 host{};
    AnomalySignatureServiceV1 signature{};
    AnomalyUe5ObjectsServiceV1 objects{};
    AnomalyUe5NamesServiceV1 names{};
    bool services_available{};
    bool bindings_available{};
    std::uint64_t generation{1};
    std::uintptr_t set_text{};
    std::array<std::uintptr_t, 110> vtable{};
    std::array<std::byte, 16> gobjects{};
    std::array<std::byte, 32> registry{};
    std::array<std::uintptr_t, 1> chunks{};
    std::array<std::byte, 24 * 256> items{};
    std::vector<std::unique_ptr<Object>> entries;
    AnomalyGenerationHandleV1 template_handle{};
};

std::optional<nte::account::AccountIdentity> RunUntilDetected(
    nte::account::UidDetector& detector, const std::uint32_t updates = 700) {
    std::optional<nte::account::AccountIdentity> detected;
    for (std::uint32_t index{}; index < updates; ++index) {
        if (const auto value = detector.Update()) detected = value;
    }
    return detected;
}

void DetectorRecoversFromLateServicesAndBindings() {
    FakeRuntime runtime;
    runtime.AddObject(L"UID: 216065736008");
    nte::account::UidDetector detector(&runtime.host);
    static_cast<void>(detector.Update());
    Check(detector.diagnostics().phase ==
            nte::account::UidDetectorPhase::WaitingForServices,
        "missing optional services should be observable, not terminal");

    runtime.services_available = true;
    runtime.bindings_available = true;
    const auto detected = RunUntilDetected(detector);
    Check(detected && detected->uid == "216065736008" &&
            detector.diagnostics().phase ==
                nte::account::UidDetectorPhase::Detected,
        "the detector should recover when services and signatures appear later");
}

void DetectorRetainsTemporarilyUnreadableAndNewestWidgets() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.bindings_available = true;
    runtime.AddObject(L"UID: 111", 1);
    for (std::uint32_t index{}; index < 65; ++index) {
        runtime.AddObject(L"UID: " + std::to_wstring(1000 + index));
    }
    nte::account::UidDetector detector(&runtime.host);
    const auto detected = RunUntilDetected(detector, 200);
    if (!detected || detected->uid != "1064" ||
        detector.diagnostics().tracked_widgets != 64) {
        std::cerr << "observed uid=" << (detected ? detected->uid : "none")
                  << " widgets=" << detector.diagnostics().tracked_widgets
                  << " failure=" << detector.diagnostics().last_failure << '\n';
    }
    Check(detected && detected->uid == "1064" &&
            detector.diagnostics().tracked_widgets == 64,
        "bounded tracking should prefer the newest live RoleID layers");

    runtime.ChangeGeneration(L"UID: 9002");
    const auto changed = RunUntilDetected(detector, 200);
    Check(changed && changed->uid == "9002",
        "a HUD generation change should rediscover the replacement RoleID widget");
}

void DetectorRetriesTemporarilyUnreadableText() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.bindings_available = true;
    runtime.AddObject(L"UID: 777", 1);
    nte::account::UidDetector detector(&runtime.host);
    const auto first = RunUntilDetected(detector, 25);
    Check(!first && detector.diagnostics().tracked_widgets == 1 &&
            detector.diagnostics().failed_reads == 1,
        "a temporary text conversion failure should retain the live widget");
    const auto retried = RunUntilDetected(detector, 40);
    Check(retried && retried->uid == "777",
        "a retained widget should be read again after the verification interval");
}

void DetectorRevisitsLateInitializedSlotsAndGrowth() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.bindings_available = true;
    runtime.AddObject(L"not ready", 0, false, kOtherName);
    nte::account::UidDetector detector(&runtime.host);
    Check(!RunUntilDetected(detector, 40),
        "an unrelated stable slot should not be reported as a UID widget");

    runtime.MakeRoleId(1, L"UID: 4242");
    const auto late = RunUntilDetected(detector, 400);
    Check(late && late->uid == "4242",
        "an empty detector should periodically revisit a late-initialized stable slot");

    runtime.AddObject(L"UID: 5252");
    const auto grown = RunUntilDetected(detector, 80);
    Check(grown && grown->uid == "5252",
        "object table growth should incrementally discover the newest RoleID widget");
}

void DetectorRecoversWhenTheTableShrinksDuringAnIncrementalScan() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.bindings_available = true;
    runtime.AddObject(L"UID: 111");
    nte::account::UidDetector detector(&runtime.host);
    const auto initial = RunUntilDetected(detector, 30);
    Check(initial && initial->uid == "111",
        "the initial widget should be detected before the shrink scenario");

    for (std::uint32_t index{}; index < 200; ++index) {
        runtime.AddObject(L"other", 0, false, kOtherName);
    }
    static_cast<void>(detector.Update());
    runtime.ShrinkTo(1);
    runtime.AddObject(L"UID: 222");
    const auto recovered = RunUntilDetected(detector, 400);
    Check(recovered && recovered->uid == "222",
        "a mid-scan table shrink should restart a bounded recovery scan without underflow");
}

void InvalidTextDoesNotReplaceAKnownUid() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.bindings_available = true;
    auto& widget = runtime.AddObject(L"UID: 7001");
    nte::account::UidDetector detector(&runtime.host);
    const auto known = RunUntilDetected(detector, 40);
    Check(known && known->uid == "7001" &&
            detector.diagnostics().current_observation_valid,
        "the initial valid UID should be known and current");

    widget.text.value = L"UID unavailable";
    std::optional<nte::account::AccountIdentity> retained;
    for (std::uint32_t index{}; index < 35; ++index) {
        retained = detector.Update();
    }
    Check(retained && retained->uid == "7001" &&
            !detector.diagnostics().current_observation_valid,
        "invalid text should retain log identity but revoke current observation validity");
    widget.text.value = L"UID: 7002";
    std::optional<nte::account::AccountIdentity> changed;
    for (std::uint32_t index{}; index < 35; ++index) {
        changed = detector.Update();
    }
    Check(changed && changed->uid == "7002" &&
            detector.diagnostics().current_observation_valid,
        "a different valid UID should replace the prior identity and restore validity");
}

void DetectorRecoversFromAStaleReusedHandle() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.bindings_available = true;
    runtime.AddObject(L"UID: 8101");
    nte::account::UidDetector detector(&runtime.host);
    const auto initial = RunUntilDetected(detector, 40);
    Check(initial && initial->uid == "8101",
        "the original widget should be detected before handle reuse");

    runtime.ReplaceRoleId(1, L"UID: 8102", 500);
    const auto recovered = RunUntilDetected(detector, 100);
    Check(recovered && recovered->uid == "8102",
        "a stale serial/index handle should trigger a nearby recovery scan");
}

}  // namespace

int main() {
    DetectorRecoversFromLateServicesAndBindings();
    DetectorRetainsTemporarilyUnreadableAndNewestWidgets();
    DetectorRetriesTemporarilyUnreadableText();
    DetectorRevisitsLateInitializedSlotsAndGrowth();
    DetectorRecoversWhenTheTableShrinksDuringAnIncrementalScan();
    InvalidTextDoesNotReplaceAKnownUid();
    DetectorRecoversFromAStaleReusedHandle();
    std::cout << "All UID detector tests passed\n";
}
