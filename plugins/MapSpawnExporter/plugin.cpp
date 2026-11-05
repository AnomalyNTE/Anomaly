#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/sdk/cpp.hpp"
#include "scanner_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace anomaly_map_spawn_exporter;

constexpr std::string_view kModuleName = "HTGame.exe";
constexpr std::string_view kTextSection = ".text";
constexpr std::string_view kExportPath = "map-spawns.json";
constexpr std::string_view kRandomItemAsset =
    "/Game/DataAssets/DataAssetSet/RandomItem/DA_RandomItem.DA_RandomItem";
constexpr std::string_view kTreasureboxAsset =
    "/Game/DataAssets/TreasureboxDataAsset.TreasureboxDataAsset";

constexpr std::array<std::string_view, 5> kOracleTables{{
    "/Game/DataTable/OracleStone/DT_OracleStoneSpawnPoint.DT_OracleStoneSpawnPoint",
    "/Game/DataTable/OracleStone/DT_OracleStoneSpawn.DT_OracleStoneSpawn",
    "/Game/DataTable/OracleStone/DT_OracleStonePoint.DT_OracleStonePoint",
    "/Game/DataTable/Spawn/DT_OracleStoneSpawnPoint.DT_OracleStoneSpawnPoint",
    "/Game/DataTable/Spawn/DT_OracleStonePoint.DT_OracleStonePoint"}};
constexpr std::array<std::string_view, 2> kOracleAssets{{
    "/Game/DataAssets/OracleStoneDataAsset.OracleStoneDataAsset",
    "/Game/DataAssets/OracleDataAsset.OracleDataAsset"}};

constexpr std::uint32_t kMaximumPoints = 32768;

enum class PointKind : std::uint8_t { teleport, monster, oracle_stone, wallet };

struct Point final {
    PointKind kind{};
    std::string id;
    std::string map;
    std::string source;
    std::array<double, 3> position{};
};

struct ArrayHeader final {
    std::uintptr_t data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct FNameValue final {
    std::uint32_t comparison_index{};
    std::uint32_t number{};
};

struct ObjectRegistry final {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
};

struct RowReference final {
    FNameValue id{};
    std::uintptr_t row{};
};

struct DataTableDescriptor final {
    std::uintptr_t table{};
    std::uintptr_t row_struct{};
    std::string object_name;
    std::string row_struct_name;
};

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyStorageServiceV1* storage{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyUiServiceV1* ui{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};

    std::uintptr_t g_objects{};
    ObjectRegistry registry{};
    std::uint32_t scan_retry_ticks{};
    std::atomic_bool scan_requested{true};
    std::atomic_bool export_requested{};
    std::mutex mutex;
    std::vector<Point> points;
    std::array<bool, 4> table_found{};
    std::vector<DataTableDescriptor> discovered_tables;
    std::string status{"Waiting for static map scan"};
    int window_open{1};
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const std::string_view id) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->query_service == nullptr) {
        return nullptr;
    }
    const void* table{};
    if (host->query_service(host->host_context, anomaly::sdk::StringView(id), 1, &table).code !=
            ANOMALY_STATUS_V1_OK || table == nullptr) {
        return nullptr;
    }
    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void*);
    return service->struct_size >= prefix && service->service_version >= 1 ? service : nullptr;
}

bool CoreReady(const Context& context) noexcept {
    return HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::read_memory)>(
               context.core, offsetof(AnomalyCoreServiceV1, read_memory)) &&
        context.core->read_memory != nullptr;
}

template <typename Value>
bool Read(Context& context, const std::uintptr_t address, Value& value) noexcept {
    if (!CoreReady(context) || address == 0) return false;
    AnomalyMutableByteSpanV1 destination{
        reinterpret_cast<std::uint8_t*>(&value), sizeof(value)};
    return context.core->read_memory(context.core->user, address, destination).code ==
        ANOMALY_STATUS_V1_OK;
}

bool ReadBytes(Context& context, const std::uintptr_t address, void* destination,
               const std::size_t size) noexcept {
    if (!CoreReady(context) || address == 0 || destination == nullptr || size == 0) return false;
    AnomalyMutableByteSpanV1 output{static_cast<std::uint8_t*>(destination), size};
    return context.core->read_memory(context.core->user, address, output).code ==
        ANOMALY_STATUS_V1_OK;
}

bool AddAddress(const std::uintptr_t base, const std::uint64_t offset,
                std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool AddSignedAddress(const std::uintptr_t base, const std::ptrdiff_t offset,
                      std::uintptr_t& result) noexcept {
    if (offset < 0) {
        const auto magnitude = static_cast<std::uintptr_t>(-(offset + 1)) + 1U;
        if (base <= magnitude) return false;
        result = base - magnitude;
        return true;
    }
    return AddAddress(base, static_cast<std::uint64_t>(offset), result);
}

bool ReadPointerAt(Context& context, const std::uintptr_t base, const std::ptrdiff_t offset,
                   std::uintptr_t& value) noexcept {
    std::uintptr_t address{};
    return AddSignedAddress(base, offset, address) && Read(context, address, value) && value != 0;
}

bool ResolveSignature(Context& context, const std::string_view pattern,
                      std::uintptr_t& address) noexcept {
    (void)context;
  address = compatibility.Address(pattern);
  return address != 0;
}

bool RefreshRegistry(Context& context) noexcept {
    if (context.g_objects == 0) {
        std::uintptr_t instruction{};
        if (!ResolveSignature(context, kGObjectsPattern, instruction)) return false;
        std::int32_t displacement{};
        std::uintptr_t displacement_address{};
        if (!AddAddress(instruction, kRipDisplacementOffset, displacement_address) ||
            !Read(context, displacement_address, displacement)) return false;
        const auto resolved = static_cast<std::intptr_t>(instruction) +
            static_cast<std::intptr_t>(kRipInstructionSize) + displacement;
        if (resolved <= 0 || !AddSignedAddress(static_cast<std::uintptr_t>(resolved),
                                                native_layout.kGObjectsAddend, context.g_objects)) return false;
    }
    ObjectRegistry next{};
    if (!ReadPointerAt(context, context.g_objects, native_layout.kObjectItemsOffset, next.items) ||
        !Read(context, context.g_objects + native_layout.kObjectCountOffset, next.count) ||
        !Read(context, context.g_objects + native_layout.kObjectMaxCountOffset, next.max_count) ||
        !Read(context, context.g_objects + native_layout.kObjectMaxChunksOffset, next.max_chunks) ||
        !Read(context, context.g_objects + native_layout.kObjectNumChunksOffset, next.num_chunks) ||
        next.count == 0 || next.count > kMaximumObjectCount || next.max_count < next.count ||
        next.num_chunks == 0 || next.num_chunks > next.max_chunks ||
        next.num_chunks > kMaximumObjectChunks) return false;
    context.registry = next;
    return true;
}

std::string ResolveName(Context& context, const std::uint32_t name_id) {
    if (context.names == nullptr || name_id == 0 ||
        !HasField<AnomalyUe5NamesServiceV1, decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
            context.names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) ||
        context.names->resolve_utf8 == nullptr) return {};
    std::array<char, 128> local{};
    std::size_t size = local.size();
    auto status = context.names->resolve_utf8(context.names->user, name_id, local.data(), &size);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size())
        return std::string(local.data(), size - 1U);
    if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
        size > kMaximumNameBytes) return {};
    std::string value(size, '\0');
    status = context.names->resolve_utf8(context.names->user, name_id, value.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size()) return {};
    value.resize(size - 1U);
    return value;
}

std::string RenderFName(Context& context, const FNameValue value) {
    std::string result = ResolveName(context, value.comparison_index);
    if (result.empty() || value.number == 0) return result;
    result += '_' + std::to_string(value.number - 1U);
    return result;
}

std::string Lowercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool ReadObjectName(Context& context, const std::uintptr_t object, std::string& name) {
    FNameValue value{};
    if (!Read(context, object + native_layout.kObjectNameOffset, value)) return false;
    name = RenderFName(context, value);
    return !name.empty();
}

bool IsDataTable(Context& context, const std::uintptr_t object) {
    std::uintptr_t class_object{};
    std::string class_name;
    return ReadPointerAt(context, object, native_layout.kObjectClassOffset, class_object) &&
        ReadObjectName(context, class_object, class_name) && class_name == "DataTable";
}

bool ReadObjectPointerAt(Context& context, const std::uint32_t index,
                         std::uintptr_t& object) {
    object = 0;
    if (index >= context.registry.count) return false;
    const auto chunk_index = index / native_layout.kObjectChunkSize;
    const auto within_chunk = index % native_layout.kObjectChunkSize;
    std::uintptr_t chunk{};
    return ReadPointerAt(context, context.registry.items,
                         static_cast<std::ptrdiff_t>(chunk_index * sizeof(void*)), chunk) &&
        ReadPointerAt(context, chunk,
                      static_cast<std::ptrdiff_t>(within_chunk * native_layout.kObjectItemStride), object);
}

bool ReadDataTableRows(Context& context, const std::uintptr_t table,
                       std::vector<RowReference>& rows) {
    rows.clear();
    ArrayHeader header{};
    std::uintptr_t row_map{};
    if (!AddAddress(table, native_layout.kDataTableRowMapOffset, row_map) ||
        !Read(context, row_map, header) || header.count <= 0 ||
        header.count > static_cast<std::int32_t>(kMaximumDataTableRows) ||
        header.capacity < header.count || header.data == 0) return false;

    std::vector<std::uint32_t> flags;
    std::int32_t flags_num{};
    std::int32_t flags_max{};
    std::uintptr_t flags_data{};
    bool have_flags =
        Read(context, row_map + native_layout.kMapBitCount, flags_num) && Read(context, row_map + native_layout.kMapBitCapacity, flags_max) &&
        flags_num >= header.count && flags_max >= flags_num &&
        ((Read(context, row_map + native_layout.kMapBitsData, flags_data) && flags_data != 0) ||
         flags_num <= 128);
    if (have_flags) {
        const auto word_count = static_cast<std::size_t>((flags_num + 31) / 32);
        flags.resize(word_count);
        if (flags_data != 0) {
            have_flags = ReadBytes(context, flags_data, flags.data(),
                                   flags.size() * sizeof(std::uint32_t));
        } else if (!ReadBytes(context, row_map + native_layout.kMapInlineBits, flags.data(),
                              flags.size() * sizeof(std::uint32_t))) {
            have_flags = false;
        }
    }

    const auto byte_count = static_cast<std::size_t>(header.count) * native_layout.kDataTableRowStride;
    std::vector<std::uint8_t> elements(byte_count);
    if (!ReadBytes(context, header.data, elements.data(), elements.size())) return false;
    rows.reserve(static_cast<std::size_t>(header.count));
    for (std::int32_t index{}; index < header.count; ++index) {
        if (have_flags && (flags[static_cast<std::size_t>(index) / 32U] &
                (1U << (static_cast<std::uint32_t>(index) & 31U))) == 0) continue;
        const auto offset = static_cast<std::size_t>(index) * native_layout.kDataTableRowStride;
        RowReference reference{};
        std::memcpy(&reference.id, elements.data() + offset, sizeof(reference.id));
        std::memcpy(&reference.row, elements.data() + offset + native_layout.kDataTableRowPointerOffset,
                     sizeof(reference.row));
        if (reference.id.comparison_index != 0 && reference.row != 0)
            rows.push_back(reference);
    }
    return !rows.empty();
}

bool DiscoverDataTables(Context& context, std::vector<DataTableDescriptor>& tables) {
    tables.clear();
    if (!RefreshRegistry(context) || context.objects == nullptr ||
        !HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::count)>(
            context.objects, offsetof(AnomalyUe5ObjectsServiceV1, count)) ||
        !HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::snapshot_at)>(
            context.objects, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_at)) ||
        context.objects->count == nullptr || context.objects->snapshot_at == nullptr) return false;

    const auto count = (std::min)(context.objects->count(context.objects->user),
                                  kMaximumObjectCount);
    for (std::uint32_t index{}; index < count; ++index) {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (context.objects->snapshot_at(context.objects->user, index, &snapshot).code !=
            ANOMALY_STATUS_V1_OK || snapshot.name_id == 0) continue;
        const std::string object_name = ResolveName(context, snapshot.name_id);
        const std::string lowered = Lowercase(object_name);
        if (lowered.find("teleport") == std::string::npos &&
            lowered.find("monster") == std::string::npos &&
            lowered.find("spawn") == std::string::npos &&
            lowered.find("spawner") == std::string::npos &&
            lowered.find("randomitem") == std::string::npos &&
            lowered.find("treasurebox") == std::string::npos &&
            lowered.find("oracle") == std::string::npos) continue;
        std::uintptr_t object{};
        if (!ReadObjectPointerAt(context, index, object) || !IsDataTable(context, object)) continue;
        std::uintptr_t row_struct{};
        std::string row_struct_name;
        if (!ReadPointerAt(context, object, native_layout.kDataTableRowStructOffset, row_struct) ||
            !ReadObjectName(context, row_struct, row_struct_name)) continue;
        tables.push_back({object, row_struct, object_name, std::move(row_struct_name)});
    }
    return !tables.empty();
}

void AppendUtf8(std::string& output, const std::uint32_t code_point) {
    if (code_point <= 0x7FU) output.push_back(static_cast<char>(code_point));
    else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

bool ReadUtf16Array(Context& context, const std::uintptr_t address, std::string& result) {
    result.clear();
    ArrayHeader header{};
    if (!Read(context, address, header) || header.count <= 0 || header.count > 8192 ||
        header.capacity < header.count || header.data == 0) return false;
    std::vector<char16_t> value(static_cast<std::size_t>(header.count));
    if (!ReadBytes(context, header.data, value.data(), value.size() * sizeof(char16_t))) return false;
    const std::size_t length = value.back() == u'\0' ? value.size() - 1U : value.size();
    for (std::size_t index{}; index < length; ++index) {
        std::uint32_t code_point = value[index];
        if (code_point >= 0xD800U && code_point <= 0xDBFFU && index + 1U < length) {
            const auto low = static_cast<std::uint32_t>(value[index + 1U]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
                ++index;
            }
        }
        AppendUtf8(result, code_point);
    }
    return true;
}

std::string ReadTextAt(Context& context, const std::uintptr_t address) {
    std::string result;
    static_cast<void>(ReadUtf16Array(context, address, result));
    return result;
}

bool FindObjectPointer(Context& context, const std::string_view path, std::uintptr_t& object) {
    object = 0;
    if (context.objects == nullptr ||
        !HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
            context.objects, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) ||
        context.objects->find_exact == nullptr || !RefreshRegistry(context)) return false;
    AnomalyGenerationHandleV1 handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(path), &handle).code !=
            ANOMALY_STATUS_V1_OK || handle.id == 0) return false;
    const std::uint64_t encoded_index = handle.id & 0xFFFFFFFFULL;
    if (encoded_index == 0) return false;
    const std::uint32_t index = static_cast<std::uint32_t>(encoded_index - 1U);
    const std::uint32_t chunk_index = index / native_layout.kObjectChunkSize;
    const std::uint32_t within_chunk = index % native_layout.kObjectChunkSize;
    if (index >= context.registry.count || chunk_index >= context.registry.num_chunks) return false;
    std::uintptr_t chunk{};
    return ReadPointerAt(context, context.registry.items,
                         static_cast<std::ptrdiff_t>(chunk_index * sizeof(void*)), chunk) &&
        ReadPointerAt(context, chunk,
                      static_cast<std::ptrdiff_t>(within_chunk * native_layout.kObjectItemStride), object);
}

bool ReadPosition(const std::vector<std::uint8_t>& row, const std::uint32_t offset,
                  std::array<double, 3>& position) noexcept {
    if (offset > row.size() || row.size() - offset < sizeof(double) * 3U) return false;
    std::memcpy(position.data(), row.data() + offset, sizeof(double) * 3U);
    if (std::all_of(position.begin(), position.end(), [](const double value) {
            return std::isfinite(value) && std::abs(value) < 20000000.0;
        }) && std::any_of(position.begin(), position.end(), [](const double value) {
            return std::abs(value) > 0.001;
        })) return true;
    if (row.size() - offset < sizeof(float) * 3U) return false;
    std::array<float, 3> floats{};
    std::memcpy(floats.data(), row.data() + offset, sizeof(float) * 3U);
    for (std::size_t index{}; index < 3; ++index) position[index] = floats[index];
    return std::all_of(position.begin(), position.end(), [](const double value) {
        return std::isfinite(value) && std::abs(value) < 20000000.0;
    }) && std::any_of(position.begin(), position.end(), [](const double value) {
        return std::abs(value) > 0.001;
    });
}

// Only declared, typed fields may supply coordinates or level names.
std::vector<std::uint32_t> ReflectedFields(Context& context, std::uintptr_t structure,
                                         bool positions) {
    std::vector<std::uint32_t> offsets;
    std::int32_t record_size{};
    std::uintptr_t property{};
    if (!Read(context, structure + native_layout.kStructSize, record_size) ||
        record_size <= 0 || record_size > kMaximumRowBytes ||
        !Read(context, structure + native_layout.kStructPropertyLink, property)) return offsets;
    for (std::size_t i{}; property && i < 128; ++i) {
        FNameValue name{}, type_name{};
        std::uintptr_t type{};
        std::int32_t offset{}, size{};
        if (!Read(context, property + native_layout.kPropertyName, name) ||
            !Read(context, property + native_layout.kPropertyClass, type) || !type ||
            !Read(context, type, type_name) ||
            !Read(context, property + native_layout.kPropertyOffset, offset) ||
            !Read(context, property + native_layout.kPropertyElementSize, size)) break;
        const auto field = Lowercase(RenderFName(context,name));
        const auto kind = RenderFName(context,type_name);
        if (offset >= 0 && size > 0 && offset <= record_size && size <= record_size-offset) {
            if (!positions && kind == "StrProperty" && size == sizeof(ArrayHeader) &&
                (field.find("level") != std::string::npos || field.find("map") != std::string::npos))
                offsets.push_back(static_cast<std::uint32_t>(offset));
            if (positions && kind == "StructProperty" &&
                (field.find("location") != std::string::npos || field.find("position") != std::string::npos ||
                 field.find("transform") != std::string::npos || field.find("spawnpoint") != std::string::npos ||
                 field.find("coordinate") != std::string::npos)) {
                std::uintptr_t value_type{};
                std::string value_name;
                if (Read(context, property + native_layout.kStructPropertyStruct, value_type) && value_type &&
                    ReadObjectName(context,value_type,value_name)) {
                    if (value_name == "Vector" && size == sizeof(double)*3)
                        offsets.push_back(static_cast<std::uint32_t>(offset));
                    else if (value_name == "Transform" && size == sizeof(double)*12 &&
                             native_layout.kTransformTranslationOffset + sizeof(double)*3 <= static_cast<std::uint32_t>(size))
                        offsets.push_back(static_cast<std::uint32_t>(offset) + native_layout.kTransformTranslationOffset);
                }
            }
        }
        std::uintptr_t next{};
        if (!Read(context,property + native_layout.kPropertyNext,next) || next == property) break;
        property=next;
    }
    return offsets;
}

std::string ReadLevel(Context& context, std::uintptr_t row,
                      const std::vector<std::uint8_t>& bytes,
                      const std::vector<std::uint32_t>& fields) {
    for (const auto offset : fields) {
        if (offset + sizeof(ArrayHeader) > bytes.size()) continue;
        const auto value=ReadTextAt(context,row+offset);
        if (!value.empty()) return value;
    }
    return {};
}

bool AddUnique(std::vector<Point>& points, std::unordered_set<std::string>& keys, Point point) {
    if (points.size() >= kMaximumPoints || point.id.empty() ||
        !std::all_of(point.position.begin(), point.position.end(), [](const double value) {
            return std::isfinite(value);
        })) return false;
    std::string key;
    key.reserve(point.id.size() + point.map.size() + 64);
    key += std::to_string(static_cast<unsigned>(point.kind));
    key.push_back('|');
    key += point.id;
    key.push_back('|');
    key += point.map;
    for (const double value : point.position) {
        key.push_back('|');
        key += std::to_string(static_cast<long long>(std::llround(value * 10.0)));
    }
    if (!keys.insert(std::move(key)).second) return false;
    points.push_back(std::move(point));
    return true;
}

bool ResolveTable(Context& context, const std::string_view path,
                  const std::uint32_t table_offset, std::uintptr_t& table) {
    std::uintptr_t object{};
    if (!FindObjectPointer(context, path, object)) return false;
    if (table_offset == 0) {
        table = object;
        return true;
    }
    return ReadPointerAt(context, object, static_cast<std::ptrdiff_t>(table_offset), table);
}

bool ScanDataTable(Context& context, const std::string_view path, const std::uint32_t table_offset,
                   const PointKind kind, std::vector<Point>& output,
                   std::unordered_set<std::string>& keys,
                   const std::string_view required_tag = {}) {
    std::uintptr_t table{};
    if (!ResolveTable(context, path, table_offset, table)) return false;
    std::uintptr_t structure{};
    std::int32_t record_size{};
    if (!Read(context, table + native_layout.kDataTableRowStructOffset, structure) || !structure ||
        !Read(context, structure + native_layout.kStructSize, record_size) ||
        record_size <= 0 || record_size > kMaximumRowBytes) return false;
    const auto position_fields=ReflectedFields(context,structure,true);
    const auto level_fields=ReflectedFields(context,structure,false);
    std::vector<RowReference> rows;
    if (!ReadDataTableRows(context, table, rows)) return false;
    for (const RowReference& reference : rows) {
        const FNameValue row_id = reference.id;
        const std::uintptr_t row = reference.row;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(record_size));
        if (!ReadBytes(context, row, bytes.data(), bytes.size())) continue;
        const std::string id = RenderFName(context, row_id);
        if (id.empty()) continue;
        std::array<double, 3> position{};
        std::string map = ReadLevel(context, row, bytes, level_fields);
        std::string type;
        if (kind == PointKind::wallet) {
            if (native_layout.kRandomItemTypeOffset + sizeof(FNameValue) > bytes.size()) continue;
            FNameValue type_name{};
            std::memcpy(&type_name, bytes.data() + native_layout.kRandomItemTypeOffset, sizeof(type_name));
            type = RenderFName(context, type_name);
            if (!type.starts_with("InteractBox_") && !type.starts_with("PropBox_")) continue;
            if (!ReadPosition(bytes, native_layout.kRandomItemTransformOffset + native_layout.kTransformTranslationOffset,
                              position)) continue;
        } else {
            if (!required_tag.empty()) {
                FNameValue row_type{};
                if (native_layout.kRandomItemTypeOffset + sizeof(row_type) <= bytes.size())
                    std::memcpy(&row_type, bytes.data() + native_layout.kRandomItemTypeOffset, sizeof(row_type));
                const std::string type_name = RenderFName(context, row_type);
                if (id.find(required_tag) == std::string::npos &&
                    type_name.find(required_tag) == std::string::npos) continue;
            }
            bool found{};
            for (const auto candidate : position_fields) {
                if (ReadPosition(bytes, candidate, position)) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }
        Point point{kind, id, std::move(map), std::string(path), position};
        if (kind == PointKind::wallet && !type.empty()) point.id += ":" + type;
        static_cast<void>(AddUnique(output, keys, std::move(point)));
    }
    return true;
}

bool ScanTeleportTable(Context& context, const DataTableDescriptor& descriptor,
                       std::vector<Point>& output, std::unordered_set<std::string>& keys) {
    std::vector<RowReference> rows;
    if (!ReadDataTableRows(context, descriptor.table, rows)) return false;
    for (const RowReference& reference : rows) {
        std::vector<std::uint8_t> bytes(native_layout.kTeleportOverrideTranslationOffset + sizeof(double) * 3U);
        if (!ReadBytes(context, reference.row, bytes.data(), bytes.size())) continue;
        std::uint8_t can_teleport{};
        std::memcpy(&can_teleport, bytes.data() + native_layout.kTeleportCanTeleportOffset,
                    sizeof(can_teleport));
        if (can_teleport == 0) continue;
        const std::uint8_t overridden = bytes[native_layout.kTeleportOverrideTransformOffset];
        const auto position_offset = overridden != 0
            ? native_layout.kTeleportOverrideTranslationOffset : native_layout.kTeleportTransformTranslationOffset;
        std::array<double, 3> position{};
        std::memcpy(position.data(), bytes.data() + position_offset, sizeof(position));
        if (!std::all_of(position.begin(), position.end(), [](const double value) {
                return std::isfinite(value) && std::abs(value) < 20000000.0;
            })) continue;
        const std::string id = RenderFName(context, reference.id);
        if (id.empty()) continue;
        const std::string map = ReadTextAt(
            context, reference.row + native_layout.kTeleportBelongsLevelOffset);
        Point point{PointKind::teleport, id, map, descriptor.object_name, position};
        static_cast<void>(AddUnique(output, keys, std::move(point)));
    }
    return true;
}


bool ScanDiscoveredMonsterTable(Context& context, const DataTableDescriptor& descriptor,
                                std::vector<Point>& output,
                                std::unordered_set<std::string>& keys) {
    std::vector<RowReference> rows;
    if (!ReadDataTableRows(context, descriptor.table, rows)) return false;
    const std::string table_tag = Lowercase(
        descriptor.object_name + " " + descriptor.row_struct_name);
    if (table_tag.find("monster") == std::string::npos &&
        table_tag.find("spawn") == std::string::npos &&
        table_tag.find("spawner") == std::string::npos) return false;
    const auto position_offsets = ReflectedFields(context, descriptor.row_struct, true);
    const auto level_fields = ReflectedFields(context, descriptor.row_struct, false);
    std::int32_t record_size{};
    if (!Read(context, descriptor.row_struct + native_layout.kStructSize, record_size) ||
        record_size <= 0 || record_size > kMaximumRowBytes) return false;
    if (position_offsets.empty()) return false;
    bool added{};
    for (const RowReference& reference : rows) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(record_size));
        if (!ReadBytes(context, reference.row, bytes.data(), bytes.size())) continue;
        std::array<double, 3> position{};
        bool found_position{};
        for (const auto candidate : position_offsets) {
            if (ReadPosition(bytes, candidate, position)) {
                found_position = true;
                break;
            }
        }
        if (!found_position) continue;
        const std::string id = RenderFName(context, reference.id);
        if (id.empty()) continue;
        std::string map = ReadLevel(context, reference.row, bytes, level_fields);
        if (map.empty()) map = descriptor.object_name;
        Point point{PointKind::monster, id, std::move(map), descriptor.object_name, position};
        added = AddUnique(output, keys, std::move(point)) || added;
    }
    return added;
}

bool ScanOracleStoneTable(Context& context, std::vector<Point>& output,
                          std::unordered_set<std::string>& keys) {
    std::uintptr_t table{};
    if (!ResolveTable(context, kTreasureboxAsset, native_layout.kOracleStoneDataAssetTableOffset, table))
        return false;
    std::vector<RowReference> rows;
    if (!ReadDataTableRows(context, table, rows)) return false;
    for (const RowReference& reference : rows) {
        const FNameValue row_id = reference.id;
        const std::uintptr_t row = reference.row;
        std::array<double, 3> position{};
        if (!Read(context, row + native_layout.kOracleStoneLocationOffset, position) ||
            !std::all_of(position.begin(), position.end(), [](const double value) {
                return std::isfinite(value) && std::abs(value) < 20000000.0;
            })) continue;
        const std::string id = RenderFName(context, row_id);
        if (id.empty()) continue;
        const std::string map = ReadTextAt(context, row + native_layout.kOracleStoneLevelOffset);
        Point point{PointKind::oracle_stone, id, map,
                    std::string(kTreasureboxAsset), position};
        static_cast<void>(AddUnique(output, keys, std::move(point)));
    }
    return true;
}

std::string CategoryName(const PointKind kind) {
    switch (kind) {
    case PointKind::teleport: return "teleport";
    case PointKind::monster: return "monster_spawn";
    case PointKind::oracle_stone: return "oracle_stone_spawn";
    case PointKind::wallet: return "wallet_spawn";
    }
    return "unknown";
}

void SetStatus(Context& context, std::string value) noexcept {
    try {
        std::scoped_lock lock(context.mutex);
        context.status = std::move(value);
    } catch (...) {
    }
}

void ScanAll(Context& context) {
    if (!RefreshRegistry(context)) {
        SetStatus(context, "UE5 object registry is unavailable; static tables were not scanned");
        return;
    }
    std::vector<Point> next;
    std::unordered_set<std::string> keys;
    next.reserve(1024);
    std::vector<DataTableDescriptor> discovered_tables;
    const bool discovered = DiscoverDataTables(context, discovered_tables);
    bool teleport_ok{};
    bool monster_ok{};
    for (const DataTableDescriptor& descriptor : discovered_tables) {
        if (descriptor.row_struct_name == "TeleportPoint" &&
            ScanTeleportTable(context, descriptor, next, keys)) {
            teleport_ok = true;
        }
        if (ScanDiscoveredMonsterTable(context, descriptor, next, keys)) {
            monster_ok = true;
        }
    }
    bool oracle_ok{};
    for (const auto path : kOracleTables) {
        if (ScanDataTable(context, path, 0, PointKind::oracle_stone, next, keys)) {
            oracle_ok = true;
            break;
        }
    }
    if (!oracle_ok) oracle_ok = ScanOracleStoneTable(context, next, keys);
    if (!oracle_ok) {
        for (const auto path : kOracleAssets) {
            if (ScanDataTable(context, path, native_layout.kRandomItemTableOffset, PointKind::oracle_stone,
                              next, keys, "Oracle")) {
                oracle_ok = true;
                break;
            }
        }
    }
    const bool wallet_ok = ScanDataTable(
        context, kRandomItemAsset, native_layout.kRandomItemTableOffset, PointKind::wallet, next, keys);
    std::array<bool, 4> found{teleport_ok, monster_ok, oracle_ok, wallet_ok};
    const std::size_t point_count = next.size();
    {
        std::scoped_lock lock(context.mutex);
        context.points = std::move(next);
        context.table_found = found;
        context.discovered_tables = discovered_tables;
    }
    context.export_requested.store(true, std::memory_order_release);
    std::string status = "Static scan complete: " + std::to_string(point_count) +
        " points; loaded Actor/Entity lists were not scanned";
    if (!discovered) status += "; no matching static DataTables discovered";
    if (!teleport_ok) status += "; teleport catalog unavailable";
    if (!monster_ok) status += "; no validated monster spawn table matched";
    SetStatus(context, std::move(status));
}

void AppendJsonString(std::string& output, const std::string_view value) {
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                constexpr char hex[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hex[character >> 4U]);
                output.push_back(hex[character & 0x0FU]);
            } else output.push_back(static_cast<char>(character));
            break;
        }
    }
    output.push_back('"');
}

std::string BuildJson(const std::vector<Point>& points) {
    std::string output;
    output.reserve(points.size() * 180U + 128U);
    output += "{\"schemaVersion\":1,\"source\":\"static-data-tables\",\"loadedActorsScanned\":false,\"points\":[";
    for (std::size_t index{}; index < points.size(); ++index) {
        if (index != 0) output.push_back(',');
        const Point& point = points[index];
        output += "{\"type\":";
        AppendJsonString(output, CategoryName(point.kind));
        output += ",\"id\":";
        AppendJsonString(output, point.id);
        output += ",\"map\":";
        AppendJsonString(output, point.map);
        output += ",\"source\":";
        AppendJsonString(output, point.source);
        output += ",\"position\":[" + std::to_string(point.position[0]) + "," +
            std::to_string(point.position[1]) + "," + std::to_string(point.position[2]) + "]}";
    }
    output += "]}\n";
    return output;
}

void ExportJson(Context& context) {
    if (context.storage == nullptr ||
        !HasField<AnomalyStorageServiceV1, decltype(AnomalyStorageServiceV1::write_atomic)>(
            context.storage, offsetof(AnomalyStorageServiceV1, write_atomic)) ||
        context.storage->write_atomic == nullptr) {
        SetStatus(context, "Storage service is unavailable; JSON was not written");
        return;
    }
    std::vector<Point> points;
    {
        std::scoped_lock lock(context.mutex);
        points = context.points;
    }
    const std::string document = BuildJson(points);
    const AnomalyByteSpanV1 source{
        reinterpret_cast<const std::uint8_t*>(document.data()), document.size()};
    const auto status = context.storage->write_atomic(
        context.storage->user, anomaly::sdk::StringView(kExportPath), source);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        SetStatus(context, "Exported map-spawns.json (" + std::to_string(points.size()) + " points)");
        return;
    }
    std::string detail = "JSON export failed with status " + std::to_string(status.code);
    if (status.message.data != nullptr && status.message.size != 0) {
        detail += ": ";
        detail.append(status.message.data, status.message.size);
    }
    SetStatus(context, detail);
}

void ANOMALY_CALL ExportJsonTask(void* value, AnomalyGenerationHandleV1) {
    if (value == nullptr) return;
    auto& context = *static_cast<Context*>(value);
    try {
        ExportJson(context);
    } catch (...) {
        SetStatus(context, "JSON export raised an exception");
    }
}

void ScheduleExport(Context& context) {
    if (context.scheduler == nullptr ||
        !HasField<AnomalySchedulerServiceV1, decltype(AnomalySchedulerServiceV1::schedule)>(
            context.scheduler, offsetof(AnomalySchedulerServiceV1, schedule)) ||
        context.scheduler->schedule == nullptr) {
        SetStatus(context, "Scheduler service is unavailable; JSON was not written");
        return;
    }
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = context.scheduler->schedule(
        context.scheduler->user, 0, ExportJsonTask, &context, &task);
    if (status.code == ANOMALY_STATUS_V1_OK && task.id != 0) {
        SetStatus(context, "JSON export queued");
        return;
    }
    std::string detail = "JSON export scheduling failed with status " + std::to_string(status.code);
    if (status.message.data != nullptr && status.message.size != 0) {
        detail += ": ";
        detail.append(status.message.data, status.message.size);
    }
    SetStatus(context, detail);
}

void Draw(Context& context, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || !HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_window)>(
            ui, offsetof(AnomalyUiServiceV1, begin_window)) || ui->begin_window == nullptr ||
        !HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
            ui, offsetof(AnomalyUiServiceV1, end_window)) || ui->end_window == nullptr) return;
    if (!ui->begin_window(ui->user, anomaly::sdk::StringView("Map Spawn Exporter"),
                          &context.window_open, 0)) {
        ui->end_window(ui->user);
        return;
    }
    const bool can_button = HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button)>(
        ui, offsetof(AnomalyUiServiceV1, button)) && ui->button != nullptr;
    const bool can_text = HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text)>(
        ui, offsetof(AnomalyUiServiceV1, text)) && ui->text != nullptr;
    if (can_button && ui->button(ui->user, anomaly::sdk::StringView("Scan static map"), 0, 0))
        context.scan_requested.store(true, std::memory_order_release);
    if (can_button && ui->button(ui->user, anomaly::sdk::StringView("Export JSON"), 0, 0))
        context.export_requested.store(true, std::memory_order_release);
    std::vector<Point> points;
    std::array<bool, 4> found{};
    std::string status;
    {
        std::scoped_lock lock(context.mutex);
        points = context.points;
        found = context.table_found;
        status = context.status;
    }
    if (can_text) {
        ui->text(ui->user, anomaly::sdk::StringView(status));
        const std::array<std::size_t, 4> counts{{
            static_cast<std::size_t>(std::count_if(points.begin(), points.end(),
                [](const Point& point) { return point.kind == PointKind::teleport; })),
            static_cast<std::size_t>(std::count_if(points.begin(), points.end(),
                [](const Point& point) { return point.kind == PointKind::monster; })),
            static_cast<std::size_t>(std::count_if(points.begin(), points.end(),
                [](const Point& point) { return point.kind == PointKind::oracle_stone; })),
            static_cast<std::size_t>(std::count_if(points.begin(), points.end(),
                [](const Point& point) { return point.kind == PointKind::wallet; }))}};
        const std::string summary = "Teleport " + std::to_string(counts[0]) +
            " | Monster " + std::to_string(counts[1]) + " | OracleStone " +
            std::to_string(counts[2]) + " | Wallet " + std::to_string(counts[3]);
        ui->text(ui->user, anomaly::sdk::StringView(summary));
        const std::string availability = "Tables: teleport=" + std::to_string(found[0]) +
            ", monster=" + std::to_string(found[1]) + ", oracle=" +
            std::to_string(found[2]) + ", wallet=" + std::to_string(found[3]);
        ui->text(ui->user, anomaly::sdk::StringView(availability));
        const std::size_t preview_count = (std::min)(points.size(), std::size_t{8});
        for (std::size_t index{}; index < preview_count; ++index) {
            const Point& point = points[index];
            const std::string row = CategoryName(point.kind) + " | " + point.id + " | " +
                point.map + " | (" + std::to_string(point.position[0]) + ", " +
                std::to_string(point.position[1]) + ", " + std::to_string(point.position[2]) + ")";
            ui->text(ui->user, anomaly::sdk::StringView(row));
        }
    }
    ui->end_window(ui->user);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (!LoadCompatibility(host)) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    if (host == nullptr || plugin_context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto* context = new (std::nothrow) Context;
    if (context == nullptr) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    context->host = host;
    context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID);
    context->storage = Query<AnomalyStorageServiceV1>(host, ANOMALY_STORAGE_SERVICE_V1_ID);
    context->scheduler = Query<AnomalySchedulerServiceV1>(host, ANOMALY_SCHEDULER_SERVICE_V1_ID);
    context->ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID);
    context->signature = Query<AnomalySignatureServiceV1>(host, ANOMALY_SIGNATURE_SERVICE_V1_ID);
    context->names = Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID);
    context->objects = Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
    if (!CoreReady(*context) || context->storage == nullptr || context->scheduler == nullptr ||
        context->ui == nullptr ||
        context->signature == nullptr || context->names == nullptr || context->objects == nullptr) {
        delete context;
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    *plugin_context = context;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void* value) {
    if (value == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    static_cast<Context*>(value)->scan_requested.store(true, std::memory_order_release);
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void ANOMALY_CALL Unload(void* value) { delete static_cast<Context*>(value); }

void ANOMALY_CALL UpdateThunk(void* value, double) {
    if (value == nullptr) return;
    auto& context = *static_cast<Context*>(value);
    try {
        const bool requested = context.scan_requested.exchange(false, std::memory_order_acq_rel);
        if (requested) {
            context.scan_retry_ticks = 0;
            ScanAll(context);
        } else if (++context.scan_retry_ticks >= 120U) {
            context.scan_retry_ticks = 0;
            bool retry{};
            {
                std::scoped_lock lock(context.mutex);
                retry = std::any_of(context.table_found.begin(), context.table_found.end(),
                    [](const bool found) { return !found; });
            }
            if (retry) context.scan_requested.store(true, std::memory_order_release);
        }
        if (context.export_requested.exchange(false, std::memory_order_acq_rel)) ScheduleExport(context);
    } catch (...) {
        SetStatus(context, "Static map scan raised an exception");
    }
}

void ANOMALY_CALL DrawThunk(void* value, const AnomalyUiServiceV1* ui) {
    if (value == nullptr) return;
    try {
        Draw(*static_cast<Context*>(value), ui);
    } catch (...) {
        SetStatus(*static_cast<Context*>(value), "Map Spawn Exporter UI raised an exception");
    }
}

}  // namespace

extern "C" ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.map-spawn-exporter"),
        anomaly::sdk::StringView("Map Spawn Exporter"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.1.0"), Load, Start, Stop, Unload, UpdateThunk, DrawThunk};
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}
