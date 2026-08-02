#include "anomaly/ue5_nte_adapter.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool Check(const bool condition, const char* const message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

class FixtureMemory final : public anomaly::SymbolMemory {
public:
    FixtureMemory() : bytes_(0x10000) {
        module_.name = L"fixture.exe";
        module_.base = kBase;
        module_.size = bytes_.size();
        sections_.push_back({
            ".text", kBase, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE});
        sections_.push_back({
            ".data", kBase + 0x1000, bytes_.size() - 0x1000,
            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE});
    }

    template <typename Value>
    void Put(const std::uintptr_t address, const Value& value) {
        std::memcpy(bytes_.data() + (address - kBase), &value, sizeof(value));
    }

    void PutBytes(
        const std::uintptr_t address,
        const void* const value,
        const std::size_t size) {
        std::memcpy(bytes_.data() + (address - kBase), value, size);
    }

    std::optional<ue5mem::ModuleInfo> FindModule(
        const std::wstring_view name) const override {
        return name == module_.name ? std::optional(module_) : std::nullopt;
    }

    std::vector<ue5mem::SectionInfo> Sections(
        const ue5mem::ModuleInfo&) const override {
        return sections_;
    }

    std::vector<std::uintptr_t> Scan(
        const ue5mem::ModuleInfo&,
        std::string_view,
        std::string_view,
        std::size_t) const override {
        return {};
    }

    bool Read(
        const std::uintptr_t address,
        void* const destination,
        const std::size_t size) const override {
        if (destination == nullptr || address < kBase || size > bytes_.size() ||
            address - kBase > bytes_.size() - size) {
            return false;
        }
        std::memcpy(destination, bytes_.data() + (address - kBase), size);
        return true;
    }

    bool Write(std::uintptr_t, const void*, std::size_t) const override {
        return false;
    }

    std::optional<anomaly::SymbolMemoryRegion> Query(
        const std::uintptr_t address) const override {
        if (address < kBase || address - kBase >= bytes_.size()) return std::nullopt;
        return anomaly::SymbolMemoryRegion{
            kBase,
            bytes_.size(),
            MEM_COMMIT,
            static_cast<DWORD>(
                address < kBase + 0x1000 ? PAGE_EXECUTE_READ : PAGE_READWRITE),
            MEM_PRIVATE};
    }

    static constexpr std::uintptr_t kBase = 0x20000000;

private:
    ue5mem::ModuleInfo module_;
    std::vector<ue5mem::SectionInfo> sections_;
    std::vector<std::uint8_t> bytes_;
};

constexpr std::uintptr_t kGObjects = FixtureMemory::kBase + 0x2000;
constexpr std::uintptr_t kObjectPageTable = FixtureMemory::kBase + 0x2100;
constexpr std::uintptr_t kObjectChunk = FixtureMemory::kBase + 0x2200;
constexpr std::uintptr_t kFNamePool = FixtureMemory::kBase + 0x3000;
constexpr std::uintptr_t kNameBlock = FixtureMemory::kBase + 0x4000;
constexpr std::uintptr_t kFunctionClass = FixtureMemory::kBase + 0x5000;
constexpr std::uintptr_t kHudClass = FixtureMemory::kBase + 0x5100;
constexpr std::uintptr_t kVectorStruct = FixtureMemory::kBase + 0x5200;
constexpr std::uintptr_t kLinearColorStruct = FixtureMemory::kBase + 0x5300;
constexpr std::uintptr_t kIntPropertyClass = FixtureMemory::kBase + 0x5400;
constexpr std::uintptr_t kStructPropertyClass = FixtureMemory::kBase + 0x5440;
constexpr std::uintptr_t kBoolPropertyClass = FixtureMemory::kBase + 0x5480;
constexpr std::uintptr_t kStringPropertyClass = FixtureMemory::kBase + 0x54C0;
constexpr std::uintptr_t kObjectPropertyClass = FixtureMemory::kBase + 0x5500;
constexpr std::uintptr_t kFloatPropertyClass = FixtureMemory::kBase + 0x5540;
constexpr std::array<std::uintptr_t, 6> kFunctions{
    FixtureMemory::kBase + 0x6000,
    FixtureMemory::kBase + 0x6100,
    FixtureMemory::kBase + 0x6200,
    FixtureMemory::kBase + 0x6300,
    FixtureMemory::kBase + 0x6400,
    FixtureMemory::kBase + 0x6500};

enum FunctionIndex : std::size_t {
    kReceiveDrawHud,
    kProject,
    kDrawText,
    kDrawLine,
    kDrawRect,
    kGetTextSize,
};

struct ParameterSpec final {
    std::string_view name;
    std::string_view type;
    std::string_view structure;
    std::int32_t size{};
    std::int32_t offset{};
    bool packed_bool{};
};

class FixtureBuilder final {
public:
    explicit FixtureBuilder(FixtureMemory& memory) : memory_(memory) {
        memory_.Put(kFNamePool, kNameBlock);
        field_classes_ = {
            {"IntProperty", kIntPropertyClass},
            {"StructProperty", kStructPropertyClass},
            {"BoolProperty", kBoolPropertyClass},
            {"StrProperty", kStringPropertyClass},
            {"ObjectProperty", kObjectPropertyClass},
            {"FloatProperty", kFloatPropertyClass}};
        for (const auto& [name, address] : field_classes_) {
            memory_.Put(address, Name(name));
        }
        PutObjectName(kFunctionClass, "Function");
        PutObjectName(kHudClass, "HUD");
        PutObjectName(kVectorStruct, "Vector");
        PutObjectName(kLinearColorStruct, "LinearColor");
    }

    void Build() {
        BuildFunction(
            kReceiveDrawHud,
            "ReceiveDrawHUD",
            8,
            0xFFFF,
            {{"SizeX", "IntProperty", {}, 4, 0},
             {"SizeY", "IntProperty", {}, 4, 4}});
        BuildFunction(
            kProject,
            "Project",
            56,
            32,
            {{"Location", "StructProperty", "Vector", 24, 0},
             {"bClampToZeroPlane", "BoolProperty", {}, 1, 24, true},
             {"ReturnValue", "StructProperty", "Vector", 24, 32}});
        BuildFunction(
            kDrawText,
            "DrawText",
            53,
            0xFFFF,
            {{"Text", "StrProperty", {}, 16, 0},
             {"TextColor", "StructProperty", "LinearColor", 16, 16},
             {"ScreenX", "FloatProperty", {}, 4, 32},
             {"ScreenY", "FloatProperty", {}, 4, 36},
             {"Font", "ObjectProperty", {}, 8, 40},
             {"Scale", "FloatProperty", {}, 4, 48},
             {"bScalePosition", "BoolProperty", {}, 1, 52, true}});
        BuildFunction(
            kDrawLine,
            "DrawLine",
            36,
            0xFFFF,
            {{"StartScreenX", "FloatProperty", {}, 4, 0},
             {"StartScreenY", "FloatProperty", {}, 4, 4},
             {"EndScreenX", "FloatProperty", {}, 4, 8},
             {"EndScreenY", "FloatProperty", {}, 4, 12},
             {"LineColor", "StructProperty", "LinearColor", 16, 16},
             {"LineThickness", "FloatProperty", {}, 4, 32}});
        BuildFunction(
            kDrawRect,
            "DrawRect",
            32,
            0xFFFF,
            {{"RectColor", "StructProperty", "LinearColor", 16, 0},
             {"ScreenX", "FloatProperty", {}, 4, 16},
             {"ScreenY", "FloatProperty", {}, 4, 20},
             {"ScreenW", "FloatProperty", {}, 4, 24},
             {"ScreenH", "FloatProperty", {}, 4, 28}});
        BuildFunction(
            kGetTextSize,
            "GetTextSize",
            36,
            0xFFFF,
            {{"Text", "StrProperty", {}, 16, 0},
             {"OutWidth", "FloatProperty", {}, 4, 16},
             {"OutHeight", "FloatProperty", {}, 4, 20},
             {"Font", "ObjectProperty", {}, 8, 24},
             {"Scale", "FloatProperty", {}, 4, 32}});

        memory_.Put(kGObjects, kObjectPageTable);
        const std::uint32_t maximum_count = 64;
        const std::uint32_t count = static_cast<std::uint32_t>(kFunctions.size());
        const std::uint32_t chunk_count = 1;
        memory_.Put(kGObjects + 0x08, maximum_count);
        memory_.Put(kGObjects + 0x0C, count);
        memory_.Put(kGObjects + 0x10, chunk_count);
        memory_.Put(kGObjects + 0x14, chunk_count);
        memory_.Put(kObjectPageTable, kObjectChunk);
        for (std::size_t index{}; index < kFunctions.size(); ++index) {
            const std::uintptr_t item = kObjectChunk + index * 0x18;
            memory_.Put(item, kFunctions[index]);
            memory_.Put(item + 0x10, static_cast<std::uint32_t>(100 + index));
        }
    }

private:
    std::uint32_t Name(const std::string_view value) {
        const auto found = names_.find(std::string(value));
        if (found != names_.end()) return found->second;
        const std::uint32_t id = name_cursor_;
        const std::uintptr_t entry = kNameBlock + static_cast<std::uintptr_t>(id) * 2U;
        const auto header = static_cast<std::uint16_t>(value.size() << 6U);
        memory_.Put(entry, header);
        memory_.PutBytes(entry + sizeof(header), value.data(), value.size());
        name_cursor_ += static_cast<std::uint32_t>((value.size() + 3U) / 2U);
        names_.emplace(value, id);
        return id;
    }

    void PutObjectName(const std::uintptr_t object, const std::string_view name) {
        memory_.Put(object + 0x08, Name(name));
    }

    void BuildFunction(
        const std::size_t index,
        const std::string_view name,
        const std::uint16_t parameter_size,
        const std::uint16_t return_offset,
        const std::vector<ParameterSpec>& parameters) {
        const std::uintptr_t function = kFunctions[index];
        PutObjectName(function, name);
        memory_.Put(function + 0x10, kFunctionClass);
        memory_.Put(function + 0x20, kHudClass);
        const std::uintptr_t first_property = next_property_;
        memory_.Put(function + 0x30, first_property);
        memory_.Put(function + 0x40, static_cast<std::uint8_t>(parameters.size()));
        memory_.Put(function + 0x42, parameter_size);
        memory_.Put(function + 0x44, return_offset);

        for (std::size_t parameter_index{};
             parameter_index < parameters.size();
             ++parameter_index) {
            const ParameterSpec& parameter = parameters[parameter_index];
            const std::uintptr_t property = next_property_;
            next_property_ += 0x40;
            const std::uintptr_t next = parameter_index + 1U < parameters.size()
                ? next_property_
                : 0;
            memory_.Put(property, field_classes_.at(std::string(parameter.type)));
            memory_.Put(property + 0x08, Name(parameter.name));
            memory_.Put(property + 0x10, std::int32_t{1});
            memory_.Put(property + 0x14, parameter.size);
            memory_.Put(property + 0x18, parameter.offset);
            memory_.Put(property + 0x20, next);
            if (parameter.structure == "Vector") {
                memory_.Put(property + 0x28, kVectorStruct);
            } else if (parameter.structure == "LinearColor") {
                memory_.Put(property + 0x28, kLinearColorStruct);
            }
            if (parameter.packed_bool) {
                memory_.Put(property + 0x28, std::uint8_t{1});
                memory_.Put(property + 0x29, std::uint8_t{0});
                memory_.Put(property + 0x2A, std::uint8_t{1});
                memory_.Put(property + 0x2B, std::uint8_t{0xFF});
            }
        }
    }

    FixtureMemory& memory_;
    std::unordered_map<std::string, std::uint32_t> names_;
    std::unordered_map<std::string, std::uintptr_t> field_classes_;
    std::uint32_t name_cursor_{1};
    std::uintptr_t next_property_{FixtureMemory::kBase + 0x8000};
};

anomaly::BuildProfile Profile() {
    anomaly::BuildProfile profile;
    profile.game = "nte";
    profile.source_hash = std::string(64, 'b');
    profile.features = {
        {"ue5.framework", {"ue5.GameTick"}},
        {"ue5.names", {"ue5.FNamePool"}},
        {"ue5.objects", {"ue5.GObjects", "ue5.GameTick"}},
        {"ue5.process-event", {"ue5.ProcessEvent"}},
        {"ue5.actor-process-event", {"ue5.AActorProcessEvent"}},
        {"ue5.functions", {"ue5.GObjects", "ue5.GameTick", "ue5.FNamePool"}},
        {"ue5.ahud", {}}};
    profile.feature_layout_validators = {
        {"ue5.process-event", {"ue5-process-event-abi-v1"}},
        {"ue5.actor-process-event", {"ue5-actor-process-event-abi-v1"}},
        {"ue5.functions", {"ue5-functions-reflection-v1"}},
        {"ue5.ahud", {"ue5-ahud-reflection-v1"}}};
    profile.feature_dependencies = {
        {"ue5.functions", {"ue5.objects", "ue5.names"}},
        {"ue5.actor-process-event", {"ue5.process-event"}},
        {"ue5.ahud", {"ue5.functions", "ue5.actor-process-event"}}};
    profile.layout = {
        {"object.class", 0x10},
        {"object.nameOffset", 0x08},
        {"object.outer", 0x20},
        {"ustruct.propertyLink", 0x30},
        {"ufunction.numParms", 0x40},
        {"ufunction.parmsSize", 0x42},
        {"ufunction.returnValueOffset", 0x44},
        {"ffield.class", 0x00},
        {"ffield.name", 0x08},
        {"ffieldClass.name", 0x00},
        {"fproperty.arrayDim", 0x10},
        {"fproperty.elementSize", 0x14},
        {"fproperty.offsetInternal", 0x18},
        {"fproperty.propertyLinkNext", 0x20},
        {"fstructProperty.struct", 0x28},
        {"fboolProperty.fieldSize", 0x28},
        {"fboolProperty.byteOffset", 0x29},
        {"fboolProperty.byteMask", 0x2A},
        {"fboolProperty.fieldMask", 0x2B},
        {"names.blocksOffset", 0x00},
        {"names.blockBits", 16},
        {"names.entryStride", 2},
        {"names.headerLengthShift", 6},
        {"objects.itemsOffset", 0x00},
        {"objects.maxCountOffset", 0x08},
        {"objects.countOffset", 0x0C},
        {"objects.maxChunksOffset", 0x10},
        {"objects.numChunksOffset", 0x14},
        {"objects.chunkCountSize", 4},
        {"objects.chunkSize", 64},
        {"objects.itemStride", 0x18},
        {"objects.objectOffset", 0x00},
        {"objects.serialOffset", 0x10}};
    return profile;
}

anomaly::ResolvedSymbol Available(std::string id, const std::uintptr_t address) {
    anomaly::ResolvedSymbol symbol;
    symbol.id = std::move(id);
    symbol.module = L"fixture.exe";
    symbol.address = address;
    symbol.state = anomaly::SymbolResolutionState::Resolved;
    return symbol;
}

anomaly::ProfileResolutionSnapshot Resolution() {
    anomaly::ProfileResolutionSnapshot resolution;
    resolution.state = anomaly::ProfileResolutionState::Ready;
    resolution.build_id = "nte-ahud-fixture";
    resolution.profile_hash = std::string(64, 'b');
    resolution.symbols.emplace("ue5.GObjects", Available("ue5.GObjects", kGObjects));
    resolution.symbols.emplace("ue5.FNamePool", Available("ue5.FNamePool", kFNamePool));
    resolution.symbols.emplace(
        "ue5.GameTick", Available("ue5.GameTick", FixtureMemory::kBase + 0x200));
    resolution.symbols.emplace(
        "ue5.ProcessEvent", Available("ue5.ProcessEvent", FixtureMemory::kBase + 0x300));
    resolution.symbols.emplace(
        "ue5.AActorProcessEvent",
        Available("ue5.AActorProcessEvent", FixtureMemory::kBase + 0x400));
    for (const std::string id : {
             "ue5.framework", "ue5.names", "ue5.objects", "ue5.process-event",
             "ue5.actor-process-event", "ue5.functions", "ue5.ahud"}) {
        resolution.features.emplace(id, anomaly::FeatureResolution{id, true, {}});
    }
    return resolution;
}

anomaly::BuildFingerprint Fingerprint() {
    anomaly::BuildFingerprint fingerprint;
    fingerprint.game = "nte";
    fingerprint.id = "nte-ahud-fixture";
    fingerprint.module = L"fixture.exe";
    fingerprint.text_sha256 = std::string(64, 'a');
    return fingerprint;
}

anomaly::FeatureLayoutValidatorRegistry FeatureValidators() {
    anomaly::FeatureLayoutValidatorRegistry validators;
    const auto register_validator = [&validators](
        const std::string_view validator_id,
        const std::string_view expected_feature) {
        validators.Register(
            std::string(validator_id),
            [expected_feature](
                const anomaly::BuildProfile&,
                const std::string_view feature,
                const anomaly::ProfileResolutionSnapshot&,
                const anomaly::SymbolMemory&) {
                return anomaly::FeatureValidationResult{
                    feature == expected_feature, "fixture feature mismatch"};
            });
    };
    register_validator("ue5-process-event-abi-v1", "ue5.process-event");
    register_validator(
        "ue5-actor-process-event-abi-v1", "ue5.actor-process-event");
    register_validator("ue5-functions-reflection-v1", "ue5.functions");
    register_validator("ue5-ahud-reflection-v1", "ue5.ahud");
    return validators;
}

template <typename Value>
Value ReadParameter(const void* const parameters, const std::size_t offset) {
    Value value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(parameters) + offset, sizeof(value));
    return value;
}

template <typename Value>
void WriteParameter(void* const parameters, const std::size_t offset, const Value& value) {
    std::memcpy(static_cast<std::uint8_t*>(parameters) + offset, &value, sizeof(value));
}

struct NativeStringHeader final {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

static_assert(sizeof(NativeStringHeader) == 16);

struct NativeCallRecorder final {
    std::atomic_uint32_t calls{};
    std::atomic_bool valid{true};

    bool Invoke(
        const std::uintptr_t object,
        const std::uintptr_t function,
        void* const parameters,
        const std::size_t parameter_size) noexcept {
        ++calls;
        if (object != FixtureMemory::kBase + 0x7000 || parameters == nullptr) {
            valid.store(false, std::memory_order_release);
            return false;
        }
        bool call_valid{};
        if (function == kFunctions[kProject] && parameter_size == 56) {
            const auto world = ReadParameter<std::array<double, 3>>(parameters, 0);
            const auto clamp = ReadParameter<std::uint8_t>(parameters, 24);
            call_valid = world == std::array<double, 3>{1.0, 2.0, 3.0} && clamp == 0;
            WriteParameter(
                parameters, 32, std::array<double, 3>{11.0, 22.0, 3.0});
        } else if (function == kFunctions[kGetTextSize] && parameter_size == 36) {
            const auto text = ReadParameter<NativeStringHeader>(parameters, 0);
            const auto font = ReadParameter<std::uintptr_t>(parameters, 24);
            const auto scale = ReadParameter<float>(parameters, 32);
            call_valid = text.data != nullptr && text.count == 5 && text.capacity == 5 &&
                std::wstring_view(text.data, 4) == L"loot" && font == 0 && scale == 1.25F;
            WriteParameter(parameters, 16, 42.0F);
            WriteParameter(parameters, 20, 11.0F);
        } else if (function == kFunctions[kDrawText] && parameter_size == 53) {
            const auto text = ReadParameter<NativeStringHeader>(parameters, 0);
            const auto color = ReadParameter<std::array<float, 4>>(parameters, 16);
            call_valid = text.data != nullptr && std::wstring_view(text.data, 4) == L"loot" &&
                std::abs(color[0] - 10.0F / 255.0F) < 0.0001F &&
                std::abs(color[1] - 20.0F / 255.0F) < 0.0001F &&
                std::abs(color[2] - 30.0F / 255.0F) < 0.0001F &&
                std::abs(color[3] - 40.0F / 255.0F) < 0.0001F &&
                ReadParameter<float>(parameters, 32) == 100.0F &&
                ReadParameter<float>(parameters, 36) == 200.0F &&
                ReadParameter<std::uintptr_t>(parameters, 40) == 0 &&
                ReadParameter<float>(parameters, 48) == 1.5F &&
                ReadParameter<std::uint8_t>(parameters, 52) == 0;
        } else if (function == kFunctions[kDrawLine] && parameter_size == 36) {
            call_valid = ReadParameter<float>(parameters, 0) == 1.0F &&
                ReadParameter<float>(parameters, 4) == 2.0F &&
                ReadParameter<float>(parameters, 8) == 3.0F &&
                ReadParameter<float>(parameters, 12) == 4.0F &&
                ReadParameter<float>(parameters, 32) == 2.0F;
        } else if (function == kFunctions[kDrawRect] && parameter_size == 32) {
            call_valid = ReadParameter<float>(parameters, 16) == 5.0F &&
                ReadParameter<float>(parameters, 20) == 6.0F &&
                ReadParameter<float>(parameters, 24) == 7.0F &&
                ReadParameter<float>(parameters, 28) == 8.0F;
        }
        if (!call_valid) valid.store(false, std::memory_order_release);
        return call_valid;
    }
};

struct DrawCallbackState final {
    std::atomic_uint32_t calls{};
    std::atomic_bool valid{};
};

void ANOMALY_CALL DrawCallback(
    void* const user,
    const AnomalyUe5AhudFrameV1* const frame) {
    auto& state = *static_cast<DrawCallbackState*>(user);
    ++state.calls;
    bool valid = frame != nullptr && frame->struct_size >= sizeof(*frame) &&
        frame->viewport_width == 1920 && frame->viewport_height == 1080;
    double world[3]{1.0, 2.0, 3.0};
    float screen[2]{};
    double depth{};
    valid = valid && frame->project(frame->user, world, screen, &depth) != 0 &&
        screen[0] == 11.0F && screen[1] == 22.0F && depth == 3.0;
    float width{};
    float height{};
    valid = valid && frame->measure_text(
        frame->user, {"loot", 4}, 1.25F, &width, &height) != 0 &&
        width == 42.0F && height == 11.0F;
    const std::uint32_t color = ANOMALY_RGBA_V1(10, 20, 30, 40);
    valid = valid &&
        frame->draw_text(frame->user, {"loot", 4}, 100.0F, 200.0F, color, 1.5F) != 0 &&
        frame->draw_line(frame->user, 1.0F, 2.0F, 3.0F, 4.0F, color, 2.0F) != 0 &&
        frame->draw_rect(frame->user, 5.0F, 6.0F, 7.0F, 8.0F, color) != 0;
    state.valid.store(valid, std::memory_order_release);
}

void ANOMALY_CALL CountCallback(
    void* const user,
    const AnomalyUe5AhudFrameV1*) {
    ++*static_cast<std::atomic_uint32_t*>(user);
}

struct SelfUnsubscribeState final {
    const AnomalyUe5AhudServiceV1* service{};
    AnomalyGenerationHandleV1 handle{};
    std::atomic_uint32_t calls{};
    std::atomic_uint32_t status{ANOMALY_STATUS_V1_UNAVAILABLE};
};

void ANOMALY_CALL SelfUnsubscribeCallback(
    void* const user,
    const AnomalyUe5AhudFrameV1*) {
    auto& state = *static_cast<SelfUnsubscribeState*>(user);
    ++state.calls;
    state.status.store(
        state.service->unsubscribe(state.service->user, state.handle).code,
        std::memory_order_release);
}

struct BlockingCallbackState final {
    std::atomic_bool entered{};
    std::atomic_bool unsubscribe_started{};
    std::atomic_bool unsubscribe_done{};
    std::atomic_bool observed_drain{};
};

struct BlockingSelfUnsubscribeState final {
    const AnomalyUe5AhudServiceV1* service{};
    AnomalyGenerationHandleV1 handle{};
    std::atomic_uint32_t status{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::atomic_bool unsubscribed{};
    std::atomic_bool release{};
};

void ANOMALY_CALL BlockingSelfUnsubscribeCallback(
    void* const user,
    const AnomalyUe5AhudFrameV1*) {
    auto& state = *static_cast<BlockingSelfUnsubscribeState*>(user);
    state.status.store(
        state.service->unsubscribe(state.service->user, state.handle).code,
        std::memory_order_release);
    state.unsubscribed.store(true, std::memory_order_release);
    while (!state.release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void ANOMALY_CALL BlockingCallback(
    void* const user,
    const AnomalyUe5AhudFrameV1*) {
    auto& state = *static_cast<BlockingCallbackState*>(user);
    state.entered.store(true, std::memory_order_release);
    while (!state.unsubscribe_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    Sleep(25);
    state.observed_drain.store(
        !state.unsubscribe_done.load(std::memory_order_acquire),
        std::memory_order_release);
}

}  // namespace

int main() {
    auto memory = std::make_shared<FixtureMemory>();
    FixtureBuilder(*memory).Build();
    anomaly::AdapterServiceRegistry registry;
    NativeCallRecorder recorder;
    std::atomic_uint32_t base_invoker_calls{};
    const anomaly::Ue5NteAdapter::ProcessEventInvoker base_invoker =
        [&base_invoker_calls](std::uintptr_t, std::uintptr_t, void*, std::size_t) {
            base_invoker_calls.fetch_add(1, std::memory_order_relaxed);
            return false;
        };
    const anomaly::Ue5NteAdapter::ProcessEventInvoker actor_invoker =
        [&recorder](
            const std::uintptr_t object,
            const std::uintptr_t function,
            void* const parameters,
            const std::size_t parameter_size) {
            return recorder.Invoke(object, function, parameters, parameter_size);
        };
    anomaly::Ue5NteAdapter adapter(
        Fingerprint(), Profile(), Resolution(), memory, registry, {}, FeatureValidators(),
        base_invoker);
    bool result = Check(adapter.Start(true, true), "AHUD adapter did not start");
    const auto* service = static_cast<const AnomalyUe5AhudServiceV1*>(
        registry.Query(
            ANOMALY_UE5_AHUD_SERVICE_V1_ID,
            ANOMALY_UE5_AHUD_SERVICE_V1_VERSION));
    result = Check(
                 service != nullptr && service->struct_size >= sizeof(*service) &&
                     service->service_version == ANOMALY_UE5_AHUD_SERVICE_V1_VERSION &&
                     service->subscribe != nullptr && service->unsubscribe != nullptr,
                 "AHUD service was not published") &&
        result;
    if (service == nullptr) return 1;

    DrawCallbackState draw_state;
    std::atomic_uint32_t count_calls{};
    AnomalyGenerationHandleV1 draw_handle{};
    AnomalyGenerationHandleV1 count_handle{};
    result = Check(
                 service->subscribe(
                     service->user, DrawCallback, &draw_state, &draw_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                     service->subscribe(
                         service->user, CountCallback, &count_calls, &count_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                     draw_handle.id != 0 && count_handle.id != 0 &&
                     draw_handle.id != count_handle.id,
                 "AHUD service did not create independent subscriptions") &&
        result;

    adapter.OnGameTick(1.0 / 60.0);
    std::array<std::int32_t, 2> viewport{1920, 1080};
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    result = Check(
                 draw_state.calls.load(std::memory_order_acquire) == 1 &&
                     draw_state.valid.load(std::memory_order_acquire) &&
                     count_calls.load(std::memory_order_acquire) == 1 &&
                     recorder.calls.load(std::memory_order_acquire) == 5 &&
                     recorder.valid.load(std::memory_order_acquire) &&
                     base_invoker_calls.load(std::memory_order_acquire) == 0 &&
                     adapter.AhudBindingReady() &&
                     adapter.AhudFrameCount() == 1 &&
                     adapter.AhudProcessEventCallCount() == 5,
                 "AHUD frame did not marshal native HUD calls") &&
        result;

    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kDrawLine],
        viewport.data(), actor_invoker);
    std::array<std::int32_t, 2> invalid_viewport{0, 1080};
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        invalid_viewport.data(), actor_invoker);
    std::thread wrong_thread([&] {
        adapter.OnProcessEvent(
            FixtureMemory::kBase + 0x7000,
            kFunctions[kReceiveDrawHud],
            viewport.data(), actor_invoker);
    });
    wrong_thread.join();
    result = Check(
                 draw_state.calls.load(std::memory_order_acquire) == 1 &&
                     count_calls.load(std::memory_order_acquire) == 1 &&
                     recorder.calls.load(std::memory_order_acquire) == 5 &&
                     adapter.AhudFrameCount() == 1 &&
                     adapter.AhudProcessEventCallCount() == 5,
                 "AHUD dispatch accepted a wrong event, viewport, or thread") &&
        result;

    result = Check(
                 service->unsubscribe(service->user, draw_handle).code ==
                     ANOMALY_STATUS_V1_OK,
                 "AHUD explicit unsubscribe failed") &&
        result;
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    result = Check(
                 draw_state.calls.load(std::memory_order_acquire) == 1 &&
                     count_calls.load(std::memory_order_acquire) == 2 &&
                     recorder.calls.load(std::memory_order_acquire) == 5,
                 "AHUD explicit unsubscribe admitted a later callback") &&
        result;

    SelfUnsubscribeState self_state{service};
    result = Check(
                 service->subscribe(
                     service->user,
                     SelfUnsubscribeCallback,
                     &self_state,
                     &self_state.handle).code == ANOMALY_STATUS_V1_OK,
                 "AHUD self-unsubscribe fixture did not subscribe") &&
        result;
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    result = Check(
                 self_state.calls.load(std::memory_order_acquire) == 1 &&
                     self_state.status.load(std::memory_order_acquire) ==
                         ANOMALY_STATUS_V1_OK,
                 "AHUD callback could not remove its own future admission") &&
        result;

    BlockingCallbackState blocking_state;
    AnomalyGenerationHandleV1 blocking_handle{};
    result = Check(
                 service->subscribe(
                     service->user,
                     BlockingCallback,
                     &blocking_state,
                     &blocking_handle).code == ANOMALY_STATUS_V1_OK,
                 "AHUD blocking fixture did not subscribe") &&
        result;
    std::atomic_uint32_t unsubscribe_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::thread unsubscribe_thread([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!blocking_state.entered.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                unsubscribe_status.store(
                    ANOMALY_STATUS_V1_TIMEOUT, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
        blocking_state.unsubscribe_started.store(true, std::memory_order_release);
        unsubscribe_status.store(
            service->unsubscribe(service->user, blocking_handle).code,
            std::memory_order_release);
        blocking_state.unsubscribe_done.store(true, std::memory_order_release);
    });
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    unsubscribe_thread.join();
    result = Check(
                 blocking_state.observed_drain.load(std::memory_order_acquire) &&
                     blocking_state.unsubscribe_done.load(std::memory_order_acquire) &&
                     unsubscribe_status.load(std::memory_order_acquire) ==
                         ANOMALY_STATUS_V1_OK,
                 "AHUD unsubscribe did not drain an entered callback") &&
        result;

    const auto count_before_resubscribe = count_calls.load(std::memory_order_acquire);
    AnomalyGenerationHandleV1 resubscribed_handle{};
    result = Check(
                 service->unsubscribe(service->user, count_handle).code ==
                         ANOMALY_STATUS_V1_OK,
                 "AHUD service did not enter an empty subscription interval") &&
        result;
    adapter.OnGameTick(1.0 / 60.0);
    result = Check(
                 service->subscribe(
                         service->user,
                         CountCallback,
                         &count_calls,
                         &resubscribed_handle).code == ANOMALY_STATUS_V1_OK,
                 "AHUD service did not accept a subscriber after an empty interval") &&
        result;
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    result = Check(
                 count_calls.load(std::memory_order_acquire) ==
                         count_before_resubscribe + 1U &&
                     service->unsubscribe(service->user, resubscribed_handle).code ==
                         ANOMALY_STATUS_V1_OK,
                 "AHUD adapter did not retain its binding across an empty interval") &&
        result;

    BlockingSelfUnsubscribeState blocking_self{service};
    result = Check(
                 service->subscribe(
                     service->user,
                     BlockingSelfUnsubscribeCallback,
                     &blocking_self,
                     &blocking_self.handle).code == ANOMALY_STATUS_V1_OK,
                 "AHUD blocking self-unsubscribe fixture did not subscribe") &&
        result;
    if (blocking_self.handle.id == 0) {
        static_cast<void>(adapter.Stop());
        return 1;
    }
    std::atomic_bool stop_started{};
    std::atomic_bool stop_done{};
    std::atomic_bool stop_drained{};
    bool stop_result{};
    std::thread stop_thread([&] {
        const auto wait_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!blocking_self.unsubscribed.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= wait_deadline) {
                stop_started.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
        stop_started.store(true, std::memory_order_release);
        stop_result = adapter.Stop(std::chrono::seconds(2));
        stop_done.store(true, std::memory_order_release);
    });
    std::thread release_thread([&] {
        while (!stop_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        Sleep(25);
        stop_drained.store(
            !stop_done.load(std::memory_order_acquire),
            std::memory_order_release);
        blocking_self.release.store(true, std::memory_order_release);
    });
    adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    stop_thread.join();
    release_thread.join();
    result = Check(
                 blocking_self.status.load(std::memory_order_acquire) ==
                         ANOMALY_STATUS_V1_OK &&
                     stop_drained.load(std::memory_order_acquire) && stop_result &&
                     registry.Snapshot().empty() && adapter.Stop(),
                 "AHUD stop did not drain a removed self-subscription callback") &&
        result;
    result = Check(
                 adapter.Start(false, true) &&
                      registry.Query(
                          ANOMALY_UE5_AHUD_SERVICE_V1_ID,
                          ANOMALY_UE5_AHUD_SERVICE_V1_VERSION) == nullptr &&
                      adapter.Stop(),
                  "AHUD service published without a GameTick framework hook") &&
        result;

    anomaly::AdapterServiceRegistry missing_invoker_registry;
    anomaly::Ue5NteAdapter missing_invoker_adapter(
        Fingerprint(), Profile(), Resolution(), memory, missing_invoker_registry, {},
        FeatureValidators());
    result = Check(
                 missing_invoker_adapter.Start(true, true) &&
                     missing_invoker_registry.Query(
                         ANOMALY_UE5_AHUD_SERVICE_V1_ID,
                         ANOMALY_UE5_AHUD_SERVICE_V1_VERSION) == nullptr &&
                     missing_invoker_adapter.Stop(),
                 "AHUD service published without the base ProcessEvent invoker") &&
        result;

    auto missing_actor_validator_profile = Profile();
    missing_actor_validator_profile.feature_layout_validators.erase(
        "ue5.actor-process-event");
    anomaly::AdapterServiceRegistry missing_actor_validator_registry;
    anomaly::Ue5NteAdapter missing_actor_validator_adapter(
        Fingerprint(), std::move(missing_actor_validator_profile), Resolution(), memory,
        missing_actor_validator_registry, {}, FeatureValidators(), base_invoker);
    result = Check(
                 missing_actor_validator_adapter.Start(true, true) &&
                     missing_actor_validator_registry.Query(
                         ANOMALY_UE5_AHUD_SERVICE_V1_ID,
                         ANOMALY_UE5_AHUD_SERVICE_V1_VERSION) == nullptr &&
                     missing_actor_validator_adapter.Stop(),
                 "AHUD service published without the Actor ProcessEvent ABI validator") &&
        result;

    auto invalid_return_memory = std::make_shared<FixtureMemory>();
    FixtureBuilder(*invalid_return_memory).Build();
    invalid_return_memory->Put(
        kFunctions[kDrawLine] + 0x44,
        std::uint16_t{0});
    anomaly::AdapterServiceRegistry invalid_return_registry;
    anomaly::Ue5NteAdapter invalid_return_adapter(
        Fingerprint(), Profile(), Resolution(), invalid_return_memory,
        invalid_return_registry, {}, FeatureValidators(), base_invoker);
    result = Check(
                 invalid_return_adapter.Start(true, true),
                 "AHUD invalid-return fixture did not start") &&
        result;
    const auto* invalid_return_service = static_cast<const AnomalyUe5AhudServiceV1*>(
        invalid_return_registry.Query(
            ANOMALY_UE5_AHUD_SERVICE_V1_ID,
            ANOMALY_UE5_AHUD_SERVICE_V1_VERSION));
    std::atomic_uint32_t invalid_return_calls{};
    AnomalyGenerationHandleV1 invalid_return_handle{};
    const bool invalid_return_subscribed = invalid_return_service != nullptr &&
        invalid_return_service->subscribe(
            invalid_return_service->user,
            CountCallback,
            &invalid_return_calls,
            &invalid_return_handle).code == ANOMALY_STATUS_V1_OK;
    invalid_return_adapter.OnGameTick(1.0 / 60.0);
    invalid_return_adapter.OnProcessEvent(
        FixtureMemory::kBase + 0x7000,
        kFunctions[kReceiveDrawHud],
        viewport.data(), actor_invoker);
    result = Check(
                 invalid_return_subscribed &&
                     invalid_return_calls.load(std::memory_order_acquire) == 0 &&
                     invalid_return_adapter.Stop(),
                 "AHUD binding accepted a return offset on a void function") &&
        result;
    return result ? 0 : 1;
}
