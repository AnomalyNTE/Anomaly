#include "anomaly/sdk/cpp.hpp"
#include "../common/localization.hpp"
#include "navmesh_profile.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

namespace {

using namespace navmesh_profile;

constexpr std::size_t kMaximumNameBytes = 96;
constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumObjectChunks = 4096;
constexpr std::uint32_t kDiscoveryBatchSize = 2048;
constexpr std::size_t kStatusCapacity = 192;

struct ObjectRegistry final {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
};

struct ReflectedBoolParameter final {
    std::uint16_t byte_offset{};
    std::uint8_t field_mask{};
    std::uint8_t byte_mask{};
};

struct MoveToLocationBinding final {
    std::uintptr_t function{};
    std::uint16_t parms_size{};
    std::uint16_t destination_offset{};
    std::uint16_t reason_type_offset{};
    std::uint16_t acceptance_radius_offset{};
    ReflectedBoolParameter stop_on_overlap{};
    ReflectedBoolParameter use_pathfinding{};
    ReflectedBoolParameter project_destination_to_navigation{};
    ReflectedBoolParameter can_strafe{};
    std::uint16_t filter_class_offset{};
    ReflectedBoolParameter allow_partial_path{};
    std::uint16_t return_value_offset{};
    std::uintptr_t registry_items{};
    std::uint32_t next_object_index{};
    std::uint32_t scanned_count{};
    bool available{};
    bool discovery_complete{};
};

struct Symbols final {
    std::uintptr_t g_world_address{};
    std::uintptr_t f_name_pool_address{};
    std::uintptr_t g_objects_address{};
    std::uintptr_t process_event{};
    bool available{};
    bool failed{};
};

struct PendingMove final {
    bool queued{};
    std::array<double, 3> destination{};
};

struct Context final {
    anomaly::plugins::Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalySignatureServiceV1* signature{};
    Symbols symbols{};
    MoveToLocationBinding binding{};
    std::array<double, 3> entered_destination{};
    PendingMove pending{};
    std::array<char, kStatusCapacity> status{};
    bool started{};
    std::mutex mutex;
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(
    const std::uint32_t code, const std::string_view message = {}) noexcept {
    return {code, 0, anomaly::sdk::StringView(message)};
}

template <typename Service>
const Service* Query(
    const AnomalyHostApiV1* host, const char* id,
    const std::uint32_t minimum_version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR ||
        host->query_service == nullptr) {
        return nullptr;
    }
    const void* table{};
    if (host->query_service(
            host->host_context, anomaly::sdk::StringView(id), minimum_version,
            &table).code != ANOMALY_STATUS_V1_OK ||
        table == nullptr) {
        return nullptr;
    }
    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t kServicePrefix = offsetof(Service, user) + sizeof(void*);
    return service->struct_size >= kServicePrefix &&
            service->service_version >= minimum_version
        ? service
        : nullptr;
}

bool CoreReady(const AnomalyCoreServiceV1* service) noexcept {
    return HasField<AnomalyCoreServiceV1,
               decltype(AnomalyCoreServiceV1::read_memory)>(
               service, offsetof(AnomalyCoreServiceV1, read_memory)) &&
        service->read_memory != nullptr;
}

bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::input_double)>(
               service, offsetof(AnomalyUiServiceV1, input_double)) &&
        service->begin_window != nullptr && service->end_window != nullptr &&
        service->text != nullptr && service->button != nullptr &&
        service->input_double != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* service) noexcept {
    return HasField<AnomalySignatureServiceV1,
               decltype(AnomalySignatureServiceV1::resolve)>(
               service, offsetof(AnomalySignatureServiceV1, resolve)) &&
        service->resolve != nullptr;
}

void Log(Context& context, const std::uint32_t level,
         const std::string_view message) noexcept {
    if (context.core != nullptr && context.core->log != nullptr) {
        context.core->log(
            context.core->user, level, anomaly::sdk::StringView(message));
    }
}

void SetStatusLocked(Context& context, const std::string_view message) noexcept {
    const std::size_t count = (std::min)(message.size(), context.status.size() - 1U);
    if (count != 0) std::memcpy(context.status.data(), message.data(), count);
    context.status[count] = '\0';
}

void SetStatus(Context& context, const std::string_view message) noexcept {
    std::scoped_lock lock(context.mutex);
    SetStatusLocked(context, message);
}

std::string CopyStatus(Context& context) {
    std::scoped_lock lock(context.mutex);
    return context.status.data();
}

template <typename T>
bool Read(Context& context, const std::uintptr_t address, T& value) noexcept {
    if (address == 0 || context.core == nullptr || context.core->read_memory == nullptr) {
        return false;
    }
    AnomalyMutableByteSpanV1 destination{
        reinterpret_cast<std::uint8_t*>(&value), sizeof(value)};
    return context.core->read_memory(context.core->user, address, destination).code ==
        ANOMALY_STATUS_V1_OK;
}

bool ReadBytes(
    Context& context, const std::uintptr_t address, void* destination,
    const std::size_t size) noexcept {
    if (address == 0 || destination == nullptr || size == 0 ||
        context.core == nullptr || context.core->read_memory == nullptr) {
        return false;
    }
    AnomalyMutableByteSpanV1 bytes{
        static_cast<std::uint8_t*>(destination), size};
    return context.core->read_memory(context.core->user, address, bytes).code ==
        ANOMALY_STATUS_V1_OK;
}

bool AddAddress(
    const std::uintptr_t base, const std::uint64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool AddSignedAddress(
    const std::uintptr_t base, const std::int64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0) return false;
    if (offset >= 0) return AddAddress(base, static_cast<std::uint64_t>(offset), result);
    const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1U;
    if (magnitude > base) return false;
    result = base - static_cast<std::uintptr_t>(magnitude);
    return true;
}

template <typename T>
bool ReadAtOffset(
    Context& context, const std::uintptr_t base, const std::uint64_t offset,
    T& value) noexcept {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && Read(context, address, value);
}

bool ReadPointerAtOffset(
    Context& context, const std::uintptr_t base, const std::uint64_t offset,
    std::uintptr_t& value) noexcept {
    return ReadAtOffset(context, base, offset, value) && value != 0;
}

bool ResolveSignature(
    Context& context, const std::string_view pattern,
    std::uintptr_t& address) noexcept {
    address = 0;
    return SignatureReady(context.signature) &&
        context.signature->resolve(
            context.signature->user, anomaly::sdk::StringView(kModuleName),
            anomaly::sdk::StringView(kTextSection), anomaly::sdk::StringView(pattern),
            &address).code == ANOMALY_STATUS_V1_OK &&
        address != 0;
}

bool ResolveRipRelative(
    Context& context, const std::string_view pattern,
    const std::int32_t addend, std::uintptr_t& address) noexcept {
    std::uintptr_t instruction{};
    if (!ResolveSignature(context, pattern, instruction)) return false;

    std::uintptr_t displacement_address{};
    std::uintptr_t instruction_end{};
    std::int32_t displacement{};
    if (!AddAddress(instruction, kRipDisplacementOffset, displacement_address) ||
        !AddAddress(instruction, kRipInstructionSize, instruction_end) ||
        !Read(context, displacement_address, displacement)) {
        return false;
    }
    std::uintptr_t resolved{};
    return AddSignedAddress(
        instruction_end, static_cast<std::int64_t>(displacement) + addend, resolved) &&
        ((address = resolved) != 0);
}

template <std::size_t Size>
bool MatchesBytes(
    Context& context, const std::uintptr_t address,
    const std::array<std::uint8_t, Size>& expected) noexcept {
    std::array<std::uint8_t, Size> observed{};
    return ReadBytes(context, address, observed.data(), observed.size()) && observed == expected;
}

bool ValidateProcessEvent(Context& context, const std::uintptr_t process_event) noexcept {
    const HMODULE module = GetModuleHandleW(L"HTGame.exe");
    const auto module_base = reinterpret_cast<std::uintptr_t>(module);
    DWORD64 unwind_base{};
    const auto* unwind = RtlLookupFunctionEntry(
        static_cast<DWORD64>(process_event), &unwind_base, nullptr);
    if (module_base == 0 || process_event < module_base || unwind == nullptr ||
        unwind_base != static_cast<DWORD64>(module_base) ||
        unwind->BeginAddress !=
            static_cast<DWORD>(process_event - module_base)) {
        return false;
    }

    constexpr auto kPrologue = std::to_array<std::uint8_t>({
        0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x00,
        0x01, 0x00, 0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30,
        0x48, 0x89, 0x9D, 0x28, 0x01, 0x00, 0x00});
    constexpr auto kArgumentSetup = std::to_array<std::uint8_t>({
        0x48, 0x33, 0xC5, 0x48, 0x89, 0x85, 0xC0, 0x00,
        0x00, 0x00, 0x8B, 0x41, 0x08, 0x4D, 0x8B, 0xF0,
        0xC1, 0xE8, 0x1E, 0x48, 0x8B, 0xFA, 0xF6, 0xD0,
        0x4C, 0x8B, 0xF9, 0xA8, 0x01});
    constexpr auto kOutParmSetup = std::to_array<std::uint8_t>({
        0x48, 0x8B, 0x4F, 0x50, 0x48, 0x8D, 0x95, 0x90,
        0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x56,
        0x90, 0x48, 0x8B, 0x41, 0x38, 0x84, 0xC0, 0x79,
        0x4D, 0x48, 0x0F, 0xBA, 0xE0, 0x08, 0x73, 0x3D,
        0x8B, 0x04, 0x24, 0x48, 0x83, 0xEC, 0x30, 0x4C,
        0x8D, 0x44, 0x24, 0x30, 0x41, 0x8B, 0x00, 0x48,
        0x63, 0x41, 0x44, 0x49, 0x83, 0xC0, 0x0F, 0x49,
        0x83, 0xE0, 0xF0, 0x49, 0x03, 0xC6, 0x49, 0x89,
        0x40, 0x08, 0x49, 0x89, 0x08, 0x48, 0x8B, 0x02,
        0x48, 0x85, 0xC0, 0x74, 0x0D, 0x4C, 0x89, 0x40,
        0x10, 0x48, 0x8B, 0x12, 0x48, 0x83, 0xC2, 0x10,
        0xEB, 0x03, 0x4C, 0x89, 0x02, 0x48, 0x8B, 0x49,
        0x18, 0x48, 0x85, 0xC9, 0x75, 0xAB, 0x48, 0x8B,
        0x02, 0x48, 0x85, 0xC0, 0x74, 0x04, 0x48, 0x89,
        0x70, 0x10, 0x4D, 0x85, 0xE4, 0x75, 0x50});
    std::uintptr_t argument_setup{};
    std::uintptr_t out_parameter_setup{};
    return AddAddress(process_event, 0x26U, argument_setup) &&
        AddAddress(process_event, 0x20FU, out_parameter_setup) &&
        MatchesBytes(context, process_event, kPrologue) &&
        MatchesBytes(context, argument_setup, kArgumentSetup) &&
        MatchesBytes(context, out_parameter_setup, kOutParmSetup);
}

bool EnsureSymbols(Context& context) noexcept {
    if (context.symbols.available) return true;
    if (context.symbols.failed) return false;

    Symbols candidate;
    if (!ResolveRipRelative(context, kGWorldPattern, 0, candidate.g_world_address) ||
        !ResolveRipRelative(context, kFNamePoolPattern, 0, candidate.f_name_pool_address) ||
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            candidate.g_objects_address) ||
        !ResolveSignature(context, kProcessEventPattern, candidate.process_event) ||
        !ValidateProcessEvent(context, candidate.process_event)) {
        context.symbols.failed = true;
        SetStatus(context, "Navmesh demo symbols or ProcessEvent ABI are unavailable.");
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            "navmesh demo signature or ProcessEvent validation failed");
        return false;
    }

    candidate.available = true;
    context.symbols = candidate;
    SetStatus(context, "Discovering HTPlayerController.MoveToLocation.");
    return true;
}

bool NameEquals(
    Context& context, const std::uint32_t name_id,
    const std::string_view expected) noexcept {
    if (expected.empty() || expected.size() > kMaximumNameBytes ||
        context.symbols.f_name_pool_address == 0) {
        return false;
    }
    const std::uint32_t block_index = name_id >> kNameBlockBits;
    const std::uint32_t entry_index = name_id & ((1U << kNameBlockBits) - 1U);
    std::uintptr_t block{};
    if (!ReadPointerAtOffset(
            context, context.symbols.f_name_pool_address,
            kNamePoolBlocksOffset +
                static_cast<std::uint64_t>(block_index) * sizeof(std::uintptr_t),
            block)) {
        return false;
    }
    std::uintptr_t entry{};
    if (!AddAddress(
            block, static_cast<std::uint64_t>(entry_index) * kNameEntryStride,
            entry)) {
        return false;
    }
    std::uint16_t header{};
    std::array<char, kMaximumNameBytes> name{};
    std::uintptr_t text{};
    return Read(context, entry, header) && (header & 1U) == 0 &&
        (header >> kNameLengthShift) == expected.size() &&
        AddAddress(entry, sizeof(header), text) &&
        ReadBytes(context, text, name.data(), expected.size()) &&
        std::equal(expected.begin(), expected.end(), name.begin());
}

bool NameAtOffsetEquals(
    Context& context, const std::uintptr_t base, const std::uint64_t offset,
    const std::string_view expected) noexcept {
    std::uint32_t name_id{};
    return ReadAtOffset(context, base, offset, name_id) &&
        NameEquals(context, name_id, expected);
}

bool ObjectNameEquals(
    Context& context, const std::uintptr_t object,
    const std::string_view expected) noexcept {
    return NameAtOffsetEquals(context, object, kObjectNameOffset, expected);
}

bool FieldClassEquals(
    Context& context, const std::uintptr_t field,
    const std::string_view expected) noexcept {
    std::uintptr_t field_class{};
    return ReadPointerAtOffset(context, field, kFFieldClassOffset, field_class) &&
        NameAtOffsetEquals(context, field_class, kFFieldClassNameOffset, expected);
}

bool FieldClassIsByteEnum(
    Context& context, const std::uintptr_t field) noexcept {
    return FieldClassEquals(context, field, "ByteProperty") ||
        FieldClassEquals(context, field, "EnumProperty");
}

bool LoadObjectRegistry(Context& context, ObjectRegistry& output) noexcept {
    ObjectRegistry registry;
    if (!ReadPointerAtOffset(
            context, context.symbols.g_objects_address,
            kObjectRegistryItemsOffset, registry.items) ||
        !ReadAtOffset(
            context, context.symbols.g_objects_address,
            kObjectRegistryCountOffset, registry.count) ||
        !ReadAtOffset(
            context, context.symbols.g_objects_address,
            kObjectRegistryMaxCountOffset, registry.max_count) ||
        !ReadAtOffset(
            context, context.symbols.g_objects_address,
            kObjectRegistryMaxChunksOffset, registry.max_chunks) ||
        !ReadAtOffset(
            context, context.symbols.g_objects_address,
            kObjectRegistryNumChunksOffset, registry.num_chunks) ||
        registry.count > registry.max_count || registry.max_count == 0 ||
        registry.max_count > kMaximumObjectCount || registry.max_chunks == 0 ||
        registry.max_chunks > kMaximumObjectChunks ||
        registry.num_chunks > registry.max_chunks ||
        static_cast<std::uint64_t>(registry.max_count) >
            static_cast<std::uint64_t>(registry.max_chunks) *
                kObjectRegistryChunkSize) {
        return false;
    }
    const std::uint64_t required_chunks = registry.count == 0
        ? 0
        : (static_cast<std::uint64_t>(registry.count) +
                kObjectRegistryChunkSize - 1U) /
            kObjectRegistryChunkSize;
    if (required_chunks > registry.num_chunks) return false;
    output = registry;
    return true;
}

bool ReadObjectSlot(
    Context& context, const ObjectRegistry& registry, const std::uint32_t index,
    std::uintptr_t& object) noexcept {
    if (index >= registry.count || registry.items == 0) return false;
    const std::uint32_t page = index / kObjectRegistryChunkSize;
    const std::uint32_t slot = index % kObjectRegistryChunkSize;
    if (page >= registry.num_chunks) return false;
    std::uintptr_t chunk{};
    std::uintptr_t item{};
    std::uint32_t serial{};
    if (!ReadPointerAtOffset(
            context, registry.items,
            static_cast<std::uint64_t>(page) * sizeof(std::uintptr_t), chunk) ||
        (chunk & (alignof(std::uintptr_t) - 1U)) != 0 ||
        !AddAddress(
            chunk, static_cast<std::uint64_t>(slot) * kObjectRegistryItemStride,
            item) ||
        !ReadAtOffset(
            context, item, kObjectRegistryObjectOffset, object) ||
        !ReadAtOffset(context, item, kObjectRegistrySerialOffset, serial)) {
        return false;
    }
    return true;
}

enum class ParameterKind : std::uint8_t {
    Vector,
    ByteEnum,
    Float,
    Bool,
    Class,
};

struct ParameterSpec final {
    std::string_view name;
    ParameterKind kind;
    std::int32_t element_size;
};

constexpr std::array kMoveToLocationParameters{
    ParameterSpec{"Dest", ParameterKind::Vector, 24},
    ParameterSpec{"ReasonType", ParameterKind::ByteEnum, 1},
    ParameterSpec{"AcceptanceRadius", ParameterKind::Float, 4},
    ParameterSpec{"bStopOnOverlap", ParameterKind::Bool, 1},
    ParameterSpec{"bUsePathfinding", ParameterKind::Bool, 1},
    ParameterSpec{"bProjectDestinationToNavigation", ParameterKind::Bool, 1},
    ParameterSpec{"bCanStrafe", ParameterKind::Bool, 1},
    ParameterSpec{"FilterClass", ParameterKind::Class, 8},
    ParameterSpec{"bAllowPartialPath", ParameterKind::Bool, 1},
    ParameterSpec{"ReturnValue", ParameterKind::ByteEnum, 1},
};

std::size_t ParameterIndex(const std::uint32_t name_id, Context& context) noexcept {
    for (std::size_t index{}; index < kMoveToLocationParameters.size(); ++index) {
        if (NameEquals(context, name_id, kMoveToLocationParameters[index].name)) {
            return index;
        }
    }
    return kMoveToLocationParameters.size();
}

bool ReadBoolParameter(
    Context& context, const std::uintptr_t property,
    const std::int32_t parameter_offset, const std::uint16_t parms_size,
    ReflectedBoolParameter& result) noexcept {
    std::uint8_t field_size{};
    std::uint8_t byte_offset{};
    std::uint8_t byte_mask{};
    std::uint8_t field_mask{};
    if (!ReadAtOffset(
            context, property, kFBoolPropertyFieldSizeOffset, field_size) ||
        !ReadAtOffset(
            context, property, kFBoolPropertyByteOffsetOffset, byte_offset) ||
        !ReadAtOffset(
            context, property, kFBoolPropertyByteMaskOffset, byte_mask) ||
        !ReadAtOffset(
            context, property, kFBoolPropertyFieldMaskOffset, field_mask) ||
        field_size == 0 || byte_offset >= field_size || byte_mask == 0 ||
        field_mask == 0 || (byte_mask & field_mask) != byte_mask ||
        parameter_offset < 0 ||
        static_cast<std::uint64_t>(parameter_offset) + byte_offset >= parms_size) {
        return false;
    }
    result = {
        static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(parameter_offset) + byte_offset),
        field_mask,
        byte_mask};
    return true;
}

bool BuildMoveToLocationBinding(
    Context& context, const std::uintptr_t function,
    MoveToLocationBinding& result) noexcept {
    if (!ObjectNameEquals(context, function, "MoveToLocation")) return false;

    std::uintptr_t class_object{};
    std::uintptr_t outer_object{};
    std::uintptr_t property{};
    std::uint8_t num_parms{};
    std::uint16_t parms_size{};
    std::uint16_t return_value_offset{};
    if (!ReadPointerAtOffset(context, function, kObjectClassOffset, class_object) ||
        !ReadPointerAtOffset(context, function, kObjectOuterOffset, outer_object) ||
        !ObjectNameEquals(context, class_object, "Function") ||
        !ObjectNameEquals(context, outer_object, "HTPlayerController") ||
        !ReadPointerAtOffset(context, function, kUStructPropertyLinkOffset, property) ||
        !ReadAtOffset(context, function, kUFunctionNumParmsOffset, num_parms) ||
        !ReadAtOffset(context, function, kUFunctionParmsSizeOffset, parms_size) ||
        !ReadAtOffset(
            context, function, kUFunctionReturnValueOffset, return_value_offset) ||
        num_parms != kMoveToLocationParameterCount ||
        parms_size != kMoveToLocationParmsSize) {
        return false;
    }

    MoveToLocationBinding candidate;
    candidate.function = function;
    candidate.parms_size = parms_size;
    std::array<bool, kMoveToLocationParameters.size()> found{};
    std::size_t property_count{};
    while (property != 0 && property_count < kMoveToLocationParameters.size()) {
        std::uintptr_t next{};
        std::uint32_t name_id{};
        std::int32_t array_dim{};
        std::int32_t element_size{};
        std::int32_t parameter_offset{};
        if (!ReadAtOffset(context, property, kFFieldNameOffset, name_id) ||
            !ReadAtOffset(context, property, kFPropertyArrayDimOffset, array_dim) ||
            !ReadAtOffset(
                context, property, kFPropertyElementSizeOffset, element_size) ||
            !ReadAtOffset(
                context, property, kFPropertyOffsetInternalOffset, parameter_offset) ||
            !ReadAtOffset(context, property, kFPropertyLinkNextOffset, next) ||
            array_dim != 1 || element_size <= 0 || parameter_offset < 0 ||
            static_cast<std::uint64_t>(parameter_offset) +
                    static_cast<std::uint64_t>(element_size) >
                parms_size) {
            return false;
        }

        const std::size_t index = ParameterIndex(name_id, context);
        if (index == kMoveToLocationParameters.size() || found[index] ||
            element_size != kMoveToLocationParameters[index].element_size ||
            static_cast<std::uint64_t>(parameter_offset) >
                (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }

        const ParameterKind kind = kMoveToLocationParameters[index].kind;
        bool valid_type{};
        switch (kind) {
        case ParameterKind::Vector: {
            std::uintptr_t structure{};
            valid_type = FieldClassEquals(context, property, "StructProperty") &&
                ReadPointerAtOffset(
                    context, property, kFStructPropertyStructOffset, structure) &&
                ObjectNameEquals(context, structure, "Vector");
            break;
        }
        case ParameterKind::ByteEnum:
            valid_type = FieldClassIsByteEnum(context, property);
            break;
        case ParameterKind::Float:
            valid_type = FieldClassEquals(context, property, "FloatProperty");
            break;
        case ParameterKind::Bool:
            valid_type = FieldClassEquals(context, property, "BoolProperty");
            break;
        case ParameterKind::Class:
            valid_type = FieldClassEquals(context, property, "ClassProperty");
            break;
        }
        if (!valid_type) return false;

        const auto offset = static_cast<std::uint16_t>(parameter_offset);
        switch (index) {
        case 0:
            candidate.destination_offset = offset;
            break;
        case 1:
            candidate.reason_type_offset = offset;
            break;
        case 2:
            candidate.acceptance_radius_offset = offset;
            break;
        case 3:
            if (!ReadBoolParameter(
                    context, property, parameter_offset, parms_size,
                    candidate.stop_on_overlap)) {
                return false;
            }
            break;
        case 4:
            if (!ReadBoolParameter(
                    context, property, parameter_offset, parms_size,
                    candidate.use_pathfinding)) {
                return false;
            }
            break;
        case 5:
            if (!ReadBoolParameter(
                    context, property, parameter_offset, parms_size,
                    candidate.project_destination_to_navigation)) {
                return false;
            }
            break;
        case 6:
            if (!ReadBoolParameter(
                    context, property, parameter_offset, parms_size,
                    candidate.can_strafe)) {
                return false;
            }
            break;
        case 7:
            candidate.filter_class_offset = offset;
            break;
        case 8:
            if (!ReadBoolParameter(
                    context, property, parameter_offset, parms_size,
                    candidate.allow_partial_path)) {
                return false;
            }
            break;
        case 9:
            if (return_value_offset != offset) return false;
            candidate.return_value_offset = offset;
            break;
        default:
            return false;
        }

        found[index] = true;
        ++property_count;
        property = next;
    }
    if (property != 0 || property_count != kMoveToLocationParameters.size() ||
        !std::all_of(found.begin(), found.end(), [](const bool value) { return value; })) {
        return false;
    }
    candidate.available = true;
    result = candidate;
    return true;
}

bool RefreshMoveToLocationBinding(
    Context& context, const ObjectRegistry& registry) noexcept {
    auto& binding = context.binding;
    if (binding.available) return true;
    if (binding.registry_items != registry.items ||
        binding.next_object_index > registry.count) {
        binding = {};
        binding.registry_items = registry.items;
    }
    if (binding.discovery_complete && registry.count > binding.scanned_count) {
        binding.discovery_complete = false;
    }
    if (binding.discovery_complete) return false;

    const std::uint32_t end = (std::min)(
        registry.count, binding.next_object_index + kDiscoveryBatchSize);
    for (std::uint32_t index = binding.next_object_index; index < end; ++index) {
        std::uintptr_t object{};
        if (!ReadObjectSlot(context, registry, index, object) || object == 0 ||
            !ObjectNameEquals(context, object, "MoveToLocation")) {
            continue;
        }
        MoveToLocationBinding candidate;
        if (BuildMoveToLocationBinding(context, object, candidate)) {
            candidate.registry_items = registry.items;
            candidate.next_object_index = index + 1U;
            candidate.scanned_count = registry.count;
            context.binding = candidate;
            SetStatus(context, "HTPlayerController.MoveToLocation is ready.");
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                "navmesh demo MoveToLocation binding is ready");
            return true;
        }
    }
    binding.next_object_index = end;
    binding.scanned_count = registry.count;
    if (end == registry.count) {
        binding.discovery_complete = true;
        SetStatus(context, "HTPlayerController.MoveToLocation was not found for this build.");
    } else {
        SetStatus(context, "Scanning game functions for HTPlayerController.MoveToLocation.");
    }
    return false;
}

bool ResolveLocalController(Context& context, std::uintptr_t& controller) noexcept {
    controller = 0;
    std::uintptr_t world{};
    std::uintptr_t game_instance{};
    std::uintptr_t local_players{};
    std::uintptr_t local_player{};
    std::int32_t local_player_count{};
    return Read(context, context.symbols.g_world_address, world) && world != 0 &&
        ReadPointerAtOffset(
            context, world, kWorldGameInstanceOffset, game_instance) &&
        ReadPointerAtOffset(
            context, game_instance, kGameInstanceLocalPlayersOffset, local_players) &&
        ReadAtOffset(
            context, game_instance,
            kGameInstanceLocalPlayersOffset + sizeof(std::uintptr_t),
            local_player_count) &&
        local_player_count > 0 && local_player_count <= 16 &&
        Read(context, local_players, local_player) && local_player != 0 &&
        ReadPointerAtOffset(
            context, local_player, kLocalPlayerControllerOffset, controller);
}

bool ObjectClassChainContains(
    Context& context, const std::uintptr_t object,
    const std::string_view expected_class) noexcept {
    std::uintptr_t current{};
    if (!ReadPointerAtOffset(context, object, kObjectClassOffset, current)) return false;
    for (std::size_t depth{}; current != 0 && depth < 32; ++depth) {
        if (ObjectNameEquals(context, current, expected_class)) return true;
        std::uintptr_t next{};
        if (!ReadAtOffset(context, current, kUStructSuperStructOffset, next) ||
            next == current) {
            return false;
        }
        current = next;
    }
    return false;
}

bool IsFiniteCoordinate(const std::array<double, 3>& coordinate) noexcept {
    return std::all_of(
        coordinate.begin(), coordinate.end(),
        [](const double value) { return std::isfinite(value); });
}

bool PeekPendingMove(Context& context, PendingMove& pending) noexcept {
    std::scoped_lock lock(context.mutex);
    if (!context.pending.queued) return false;
    pending = context.pending;
    return true;
}

bool TakePendingMove(Context& context, PendingMove& pending) noexcept {
    std::scoped_lock lock(context.mutex);
    if (!context.pending.queued) return false;
    pending = context.pending;
    context.pending = {};
    return true;
}

bool SetBool(
    std::array<std::uint8_t, kMoveToLocationParmsSize>& parameters,
    const ReflectedBoolParameter& parameter, const bool value) noexcept {
    if (parameter.byte_offset >= parameters.size() || parameter.field_mask == 0 ||
        parameter.byte_mask == 0 ||
        (parameter.byte_mask & parameter.field_mask) != parameter.byte_mask) {
        return false;
    }
    auto& byte = parameters[parameter.byte_offset];
    byte = static_cast<std::uint8_t>(
        (byte & ~parameter.field_mask) | (value ? parameter.byte_mask : 0U));
    return true;
}

std::uint8_t InvokeMoveToLocation(
    const MoveToLocationBinding& binding, const std::uintptr_t process_event,
    const std::uintptr_t controller,
    const std::array<double, 3>& destination) {
    if (!binding.available || binding.parms_size != kMoveToLocationParmsSize ||
        process_event == 0 || controller == 0 ||
        binding.destination_offset + sizeof(destination) > binding.parms_size ||
        binding.reason_type_offset >= binding.parms_size ||
        binding.acceptance_radius_offset + sizeof(float) > binding.parms_size ||
        binding.filter_class_offset + sizeof(std::uintptr_t) > binding.parms_size ||
        binding.return_value_offset >= binding.parms_size) {
        return kPathRequestFailed;
    }

    alignas(std::uint64_t) std::array<std::uint8_t, kMoveToLocationParmsSize> parameters{};
    std::memcpy(
        parameters.data() + binding.destination_offset, destination.data(),
        sizeof(destination));
    parameters[binding.reason_type_offset] = kPathFollowingReasonCommon;
    constexpr float kAcceptanceRadius = -1.0F;
    std::memcpy(
        parameters.data() + binding.acceptance_radius_offset, &kAcceptanceRadius,
        sizeof(kAcceptanceRadius));
    const std::uintptr_t filter_class{};
    std::memcpy(
        parameters.data() + binding.filter_class_offset, &filter_class,
        sizeof(filter_class));
    if (!SetBool(parameters, binding.stop_on_overlap, true) ||
        !SetBool(parameters, binding.use_pathfinding, true) ||
        !SetBool(parameters, binding.project_destination_to_navigation, true) ||
        !SetBool(parameters, binding.can_strafe, false) ||
        !SetBool(parameters, binding.allow_partial_path, false)) {
        return kPathRequestFailed;
    }

    using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
    const auto invoke = reinterpret_cast<ProcessEventFn>(process_event);
    invoke(
        reinterpret_cast<void*>(controller), reinterpret_cast<void*>(binding.function),
        parameters.data());
    return parameters[binding.return_value_offset];
}

void ProcessQueuedMove(Context& context) {
    PendingMove pending;
    if (!PeekPendingMove(context, pending)) return;

    MoveToLocationBinding verified_binding;
    if (!BuildMoveToLocationBinding(context, context.binding.function, verified_binding)) {
        context.binding = {};
        SetStatus(context, "MoveToLocation binding changed; rediscovering it.");
        return;
    }
    verified_binding.registry_items = context.binding.registry_items;
    verified_binding.next_object_index = context.binding.next_object_index;
    verified_binding.scanned_count = context.binding.scanned_count;
    context.binding = verified_binding;

    std::uintptr_t controller{};
    if (!ResolveLocalController(context, controller)) {
        SetStatus(context, "Move request is waiting for a local player controller.");
        return;
    }
    if (!ObjectClassChainContains(context, controller, "HTPlayerController")) {
        SetStatus(context, "Move request is waiting for HTPlayerController.");
        return;
    }
    if (!TakePendingMove(context, pending)) return;

    const std::uint8_t result = InvokeMoveToLocation(
        context.binding, context.symbols.process_event, controller,
        pending.destination);
    switch (result) {
    case kPathRequestSuccessful:
        SetStatus(context, "Move request accepted by the native path follower.");
        break;
    case kPathRequestAlreadyAtGoal:
        SetStatus(context, "Move request reports that the target is already reached.");
        break;
    default:
        SetStatus(context, "Move request failed.");
        break;
    }
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    try {
        {
            std::scoped_lock lock(context->mutex);
            if (!context->started) return;
        }
        if (!EnsureSymbols(*context)) return;

        ObjectRegistry registry;
        if (!LoadObjectRegistry(*context, registry)) {
            SetStatus(*context, "UE object registry is unavailable.");
            return;
        }
        if (!RefreshMoveToLocationBinding(*context, registry)) return;
        ProcessQueuedMove(*context);
    } catch (...) {
        SetStatus(*context, "Navmesh demo update failed.");
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            "navmesh demo update raised an exception");
    }
}

void QueueMove(Context& context, const std::array<double, 3>& destination) noexcept {
    std::scoped_lock lock(context.mutex);
    context.entered_destination = destination;
    context.pending = {true, destination};
    SetStatusLocked(context, "Move request queued.");
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || !UiReady(ui)) return;

    try {
        std::array<double, 3> destination{};
        {
            std::scoped_lock lock(context->mutex);
            destination = context->entered_destination;
        }

        int open = 1;
        const std::string title = context->localizer.Label(
            "window.title", "Navmesh Demo", "navmesh-demo");
        anomaly::sdk::UiWindow window(ui, title, &open);
        if (!window) return;

        const std::string hint = context->localizer.Text(
            "window.hint", "Enter a world coordinate and request native pathfinding.");
        ui->text(ui->user, anomaly::sdk::StringView(hint));

        const bool changed_x = ui->input_double(
            ui->user, anomaly::sdk::StringView("X###navmesh-x"),
            &destination[0], 0.0, 0.0) != 0;
        const bool changed_y = ui->input_double(
            ui->user, anomaly::sdk::StringView("Y###navmesh-y"),
            &destination[1], 0.0, 0.0) != 0;
        const bool changed_z = ui->input_double(
            ui->user, anomaly::sdk::StringView("Z###navmesh-z"),
            &destination[2], 0.0, 0.0) != 0;
        if (changed_x || changed_y || changed_z) {
            std::scoped_lock lock(context->mutex);
            context->entered_destination = destination;
        }

        const std::string move = context->localizer.Label(
            "action.move", "Move to coordinate", "navmesh-move");
        if (ui->button(ui->user, anomaly::sdk::StringView(move), 0.0F, 0.0F) != 0) {
            if (IsFiniteCoordinate(destination)) {
                QueueMove(*context, destination);
            } else {
                SetStatus(*context, "Coordinates must be finite.");
            }
        }

        const std::string status_label = context->localizer.Text("status.label", "Status");
        const std::string status = CopyStatus(*context);
        std::string rendered_status = status_label;
        rendered_status.append(": ").append(status);
        ui->text(ui->user, anomaly::sdk::StringView(rendered_status));
    } catch (...) {
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (plugin_context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);

    auto* context = new (std::nothrow) Context;
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
    context->core = Query<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    context->ui = Query<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    context->signature = Query<AnomalySignatureServiceV1>(
        host, ANOMALY_SIGNATURE_SERVICE_V1_ID,
        ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    if (!CoreReady(context->core) || !UiReady(context->ui) ||
        !SignatureReady(context->signature)) {
        delete context;
        return Status(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "required navmesh demo services are unavailable");
    }
    context->localizer = anomaly::plugins::Localizer(host);
    SetStatus(*context, "Waiting for a game update.");
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    context->started = true;
    context->symbols = {};
    context->binding = {};
    context->pending = {};
    SetStatusLocked(*context, "Waiting for a game update.");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    context->started = false;
    context->pending = {};
    SetStatusLocked(*context, "Stopped.");
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    static_cast<void>(Stop(context, 0));
    delete context;
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-navmesh-demo"),
        anomaly::sdk::StringView("Navmesh Demo"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
