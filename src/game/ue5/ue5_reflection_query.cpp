#include "anomaly/ue5_reflection_query.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr std::uint32_t kMaximumActors = 16384;
constexpr std::uint32_t kMaximumObjects = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumObjectChunks = 4096;
constexpr std::int64_t kMaximumLayoutOffset = 64LL * 1024LL * 1024LL;
constexpr std::size_t kMaximumNameBytes = 1024;

std::string Quote(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(kHex[character >> 4U]);
                result.push_back(kHex[character & 0x0fU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string Error(std::string_view message) {
    return "{\"ok\":false,\"error\":" + Quote(message) + "}";
}

std::string Ok(std::string_view payload) {
    return "{\"ok\":true," + std::string(payload) + "}";
}

std::string_view Trim(std::string_view value) noexcept {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::pair<std::string_view, std::string_view> Shift(std::string_view value) noexcept {
    value = Trim(value);
    const auto split = value.find_first_of(" \t");
    if (split == std::string_view::npos) return {value, {}};
    return {value.substr(0, split), Trim(value.substr(split + 1U))};
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    std::uint64_t result{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result, 10);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
        ? std::optional(result)
        : std::nullopt;
}

std::string HexAddress(std::uintptr_t value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result{"0x"};
    bool started{};
    for (int shift = static_cast<int>(sizeof(value) * 8U) - 4; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xfU);
        if (nibble != 0U || started || shift == 0) {
            started = true;
            result.push_back(kHex[nibble]);
        }
    }
    return result;
}

bool AddAddress(
    const std::uintptr_t base,
    const std::int64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset < 0 ||
        static_cast<std::uint64_t>(offset) >
            (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool AddUnsignedAddress(
    const std::uintptr_t base,
    const std::uint64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

template <typename Value>
bool ReadValue(
    const SymbolMemory& memory,
    const std::uintptr_t address,
    Value& value) noexcept {
    return address != 0 && memory.Read(address, &value, sizeof(value));
}

std::optional<std::int64_t> Layout(
    const BuildProfile& profile,
    const std::string_view key) noexcept {
    const auto found = profile.layout.find(std::string(key));
    if (found == profile.layout.end() || found->second < 0 ||
        found->second > kMaximumLayoutOffset) {
        return std::nullopt;
    }
    return found->second;
}

bool HasLayout(
    const BuildProfile& profile,
    std::initializer_list<std::string_view> keys) noexcept {
    return std::all_of(keys.begin(), keys.end(), [&profile](const std::string_view key) {
        return Layout(profile, key).has_value();
    });
}

char LowerAscii(const char value) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool EqualsInsensitive(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::equal(
        left.begin(), left.end(), right.begin(), [](const char first, const char second) {
            return LowerAscii(first) == LowerAscii(second);
        });
}

bool ContainsInsensitive(std::string_view value, std::string_view filter) noexcept {
    if (filter == "*") return true;
    if (filter.empty() || filter.size() > value.size()) return false;
    return std::search(
               value.begin(), value.end(), filter.begin(), filter.end(),
               [](const char left, const char right) {
                   return LowerAscii(left) == LowerAscii(right);
               }) != value.end();
}

struct ParsedRequest final {
    enum class Kind : std::uint8_t { Actors, Functions };

    Kind kind{};
    std::string_view filter;
    std::size_t limit{};
    std::uint32_t cursor{};
};

std::optional<ParsedRequest> ParseRequest(
    std::string_view request,
    const Ue5ReflectionQueryOptions& options,
    std::string& error) {
    const auto [kind, after_kind] = Shift(request);
    ParsedRequest parsed;
    if (kind == "actors") {
        parsed.kind = ParsedRequest::Kind::Actors;
    } else if (kind == "functions") {
        parsed.kind = ParsedRequest::Kind::Functions;
    } else {
        error = "usage: ue actors <filter|*> [limit] [cursor] | ue functions <filter|*> [limit] [cursor]";
        return std::nullopt;
    }

    const auto [filter, after_filter] = Shift(after_kind);
    if (filter.empty() || filter.size() > 128U) {
        error = "reflection filter must contain 1 to 128 bytes";
        return std::nullopt;
    }
    parsed.filter = filter;
    parsed.limit = (std::min)(options.maximum_results, std::size_t{64});
    if (parsed.limit == 0 || options.maximum_objects_per_request == 0) {
        error = "reflection query limits are disabled";
        return std::nullopt;
    }

    const auto [limit_text, after_limit] = Shift(after_filter);
    if (!limit_text.empty()) {
        const auto limit = ParseUnsigned(limit_text);
        if (!limit || *limit == 0 || *limit > options.maximum_results) {
            error = "reflection limit is outside the configured bound";
            return std::nullopt;
        }
        parsed.limit = static_cast<std::size_t>(*limit);
    }

    const auto [cursor_text, trailing] = Shift(after_limit);
    if (!cursor_text.empty()) {
        const auto cursor = ParseUnsigned(cursor_text);
        if (!cursor || *cursor > (std::numeric_limits<std::uint32_t>::max)()) {
            error = "reflection cursor is invalid";
            return std::nullopt;
        }
        parsed.cursor = static_cast<std::uint32_t>(*cursor);
    }
    if (!trailing.empty()) {
        error = "reflection query has too many arguments";
        return std::nullopt;
    }
    return parsed;
}

class NameResolver final {
public:
    NameResolver(const BuildProfile& profile, const ProfileResolutionSnapshot& resolution,
                 const SymbolMemory& memory) noexcept
        : profile_(profile), resolution_(resolution), memory_(memory) {}

    [[nodiscard]] bool Available() const noexcept {
        const auto* const symbol = resolution_.FindSymbol("ue5.FNamePool");
        return symbol != nullptr && symbol->Available() &&
            Layout(profile_, "names.blocksOffset").has_value() &&
            Layout(profile_, "names.blockBits").has_value() &&
            Layout(profile_, "names.entryStride").has_value() &&
            Layout(profile_, "names.headerLengthShift").has_value();
    }

    [[nodiscard]] std::string Resolve(const std::uint32_t id) const noexcept {
        if (id == 0) return {};
        const auto* const names = resolution_.FindSymbol("ue5.FNamePool");
        const auto blocks_offset = Layout(profile_, "names.blocksOffset");
        const auto block_bits = Layout(profile_, "names.blockBits");
        const auto entry_stride = Layout(profile_, "names.entryStride");
        const auto length_shift = Layout(profile_, "names.headerLengthShift");
        if (names == nullptr || !names->Available() || !blocks_offset || !block_bits ||
            !entry_stride || !length_shift || *block_bits <= 0 || *block_bits >= 31 ||
            *entry_stride <= 0 || *length_shift <= 0 || *length_shift >= 16) {
            return {};
        }

        const std::uint32_t block_index = id >> *block_bits;
        const std::uint32_t entry_index = id & ((std::uint32_t{1} << *block_bits) - 1U);
        const auto block_stride = static_cast<std::uint64_t>(block_index) *
            sizeof(std::uintptr_t);
        std::uintptr_t block_slot{};
        std::uintptr_t block{};
        if (block_stride > static_cast<std::uint64_t>(kMaximumLayoutOffset) ||
            !AddUnsignedAddress(
                names->address, static_cast<std::uint64_t>(*blocks_offset) + block_stride,
                block_slot) ||
            !ReadValue(memory_, block_slot, block) || block == 0) {
            return {};
        }

        const auto entry_distance = static_cast<std::uint64_t>(entry_index) *
            static_cast<std::uint64_t>(*entry_stride);
        std::uintptr_t entry{};
        std::uint16_t header{};
        if (!AddUnsignedAddress(block, entry_distance, entry) ||
            !ReadValue(memory_, entry, header)) {
            return {};
        }
        const std::size_t length = header >> *length_shift;
        const bool wide = (header & 1U) != 0;
        if (wide || length == 0 || length > kMaximumNameBytes) return {};

        std::string result(length, '\0');
        std::uintptr_t text{};
        if (!AddUnsignedAddress(entry, sizeof(header), text) ||
            !memory_.Read(text, result.data(), result.size())) {
            return {};
        }
        return result;
    }

private:
    const BuildProfile& profile_;
    const ProfileResolutionSnapshot& resolution_;
    const SymbolMemory& memory_;
};

struct ObjectDescription final {
    std::string name;
    std::string class_name;
    std::string outer_name;
};

bool DescribeObject(
    const Ue5ReflectionQueryContext& context,
    const NameResolver& names,
    const std::uintptr_t object,
    ObjectDescription& output) noexcept {
    const auto name_offset = Layout(context.profile, "object.nameOffset");
    const auto class_offset = Layout(context.profile, "object.class");
    const auto outer_offset = Layout(context.profile, "object.outer");
    if (!name_offset || !class_offset || !outer_offset) return false;

    std::uintptr_t field{};
    std::uint32_t name_id{};
    if (!AddAddress(object, *name_offset, field) || !ReadValue(context.memory, field, name_id)) {
        return false;
    }
    output.name = names.Resolve(name_id);

    std::uintptr_t class_object{};
    if (AddAddress(object, *class_offset, field) && ReadValue(context.memory, field, class_object) &&
        class_object != 0 && AddAddress(class_object, *name_offset, field) &&
        ReadValue(context.memory, field, name_id)) {
        output.class_name = names.Resolve(name_id);
    }

    std::uintptr_t outer_object{};
    if (AddAddress(object, *outer_offset, field) && ReadValue(context.memory, field, outer_object) &&
        outer_object != 0 && AddAddress(outer_object, *name_offset, field) &&
        ReadValue(context.memory, field, name_id)) {
        output.outer_name = names.Resolve(name_id);
    }
    return true;
}

bool Matches(const ObjectDescription& value, const std::string_view filter) noexcept {
    return ContainsInsensitive(value.name, filter) ||
        ContainsInsensitive(value.class_name, filter) ||
        ContainsInsensitive(value.outer_name, filter);
}

struct ObjectRegistry final {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
    std::uint32_t chunk_size{};
    std::uint32_t item_stride{};
    std::uint32_t object_offset{};
    std::uint32_t serial_offset{};
};

bool LoadObjectRegistry(
    const Ue5ReflectionQueryContext& context,
    ObjectRegistry& output) noexcept {
    const auto* const objects = context.resolution.FindSymbol("ue5.GObjects");
    const auto items_offset = Layout(context.profile, "objects.itemsOffset");
    const auto count_offset = Layout(context.profile, "objects.countOffset");
    const auto max_count_offset = Layout(context.profile, "objects.maxCountOffset");
    const auto max_chunks_offset = Layout(context.profile, "objects.maxChunksOffset");
    const auto num_chunks_offset = Layout(context.profile, "objects.numChunksOffset");
    const auto chunk_size = Layout(context.profile, "objects.chunkSize");
    const auto item_stride = Layout(context.profile, "objects.itemStride");
    const auto object_offset = Layout(context.profile, "objects.objectOffset");
    const auto serial_offset = Layout(context.profile, "objects.serialOffset");
    const auto chunk_count_size = Layout(context.profile, "objects.chunkCountSize").value_or(
        static_cast<std::int64_t>(sizeof(std::uint32_t)));
    if (objects == nullptr || !objects->Available() || !items_offset || !count_offset ||
        !max_count_offset || !max_chunks_offset || !num_chunks_offset || !chunk_size ||
        !item_stride || !object_offset || !serial_offset || *items_offset > 4096 ||
        *count_offset > 4096 || *max_count_offset > 4096 || *max_chunks_offset > 4096 ||
        *num_chunks_offset > 4096 || *chunk_size <= 0 ||
        *chunk_size > static_cast<std::int64_t>(kMaximumObjects) ||
        (*chunk_size & (*chunk_size - 1)) != 0 ||
        *item_stride < static_cast<std::int64_t>(sizeof(std::uintptr_t)) ||
        *item_stride > 4096 || *object_offset > *item_stride -
            static_cast<std::int64_t>(sizeof(std::uintptr_t)) ||
        *serial_offset > *item_stride - static_cast<std::int64_t>(sizeof(std::uint32_t)) ||
        (chunk_count_size != static_cast<std::int64_t>(sizeof(std::uint16_t)) &&
         chunk_count_size != static_cast<std::int64_t>(sizeof(std::uint32_t)))) {
        return false;
    }

    ObjectRegistry registry;
    registry.chunk_size = static_cast<std::uint32_t>(*chunk_size);
    registry.item_stride = static_cast<std::uint32_t>(*item_stride);
    registry.object_offset = static_cast<std::uint32_t>(*object_offset);
    registry.serial_offset = static_cast<std::uint32_t>(*serial_offset);
    std::uintptr_t field{};
    if (!AddAddress(objects->address, *items_offset, field) ||
        !ReadValue(context.memory, field, registry.items) || registry.items == 0 ||
        !AddAddress(objects->address, *count_offset, field) ||
        !ReadValue(context.memory, field, registry.count) ||
        !AddAddress(objects->address, *max_count_offset, field) ||
        !ReadValue(context.memory, field, registry.max_count) ||
        !AddAddress(objects->address, *max_chunks_offset, field) ||
        !AddAddress(objects->address, *num_chunks_offset, field)) {
        return false;
    }

    const auto read_chunk_count = [&](const std::uintptr_t address, std::uint32_t& value) {
        if (chunk_count_size == static_cast<std::int64_t>(sizeof(std::uint16_t))) {
            std::uint16_t packed{};
            if (!ReadValue(context.memory, address, packed)) return false;
            value = packed;
            return true;
        }
        return ReadValue(context.memory, address, value);
    };
    std::uintptr_t max_chunks_address{};
    std::uintptr_t num_chunks_address{};
    if (!AddAddress(objects->address, *max_chunks_offset, max_chunks_address) ||
        !AddAddress(objects->address, *num_chunks_offset, num_chunks_address) ||
        !read_chunk_count(max_chunks_address, registry.max_chunks) ||
        !read_chunk_count(num_chunks_address, registry.num_chunks) || registry.count > registry.max_count ||
        registry.max_count == 0 || registry.max_count > kMaximumObjects ||
        registry.max_chunks == 0 || registry.max_chunks > kMaximumObjectChunks ||
        registry.num_chunks > registry.max_chunks ||
        static_cast<std::uint64_t>(registry.max_count) >
            static_cast<std::uint64_t>(registry.max_chunks) * registry.chunk_size) {
        return false;
    }
    const auto required_chunks = registry.count == 0 ? 0U :
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(registry.count) + registry.chunk_size - 1U) /
            registry.chunk_size);
    if (required_chunks > registry.num_chunks) return false;
    output = registry;
    return true;
}

bool ReadObjectSlot(
    const Ue5ReflectionQueryContext& context,
    const ObjectRegistry& registry,
    const std::uint32_t index,
    std::uintptr_t& object,
    std::uint32_t& serial) noexcept {
    if (index >= registry.count || registry.chunk_size == 0) return false;
    const std::uint32_t page = index / registry.chunk_size;
    const std::uint32_t slot = index % registry.chunk_size;
    if (page >= registry.num_chunks) return false;
    std::uintptr_t page_slot{};
    std::uintptr_t chunk{};
    std::uintptr_t item{};
    std::uintptr_t object_address{};
    std::uintptr_t serial_address{};
    return AddUnsignedAddress(
               registry.items, static_cast<std::uint64_t>(page) * sizeof(std::uintptr_t), page_slot) &&
        ReadValue(context.memory, page_slot, chunk) && chunk != 0 &&
        AddUnsignedAddress(
            chunk, static_cast<std::uint64_t>(slot) * registry.item_stride, item) &&
        AddUnsignedAddress(item, registry.object_offset, object_address) &&
        AddUnsignedAddress(item, registry.serial_offset, serial_address) &&
        ReadValue(context.memory, object_address, object) &&
        ReadValue(context.memory, serial_address, serial);
}

std::string ActorsJson(
    const Ue5ReflectionQueryContext& context,
    const ParsedRequest& request) {
    if (!context.resolution.FeatureAvailable("ue5.actors")) {
        return Error("ue5.actors is unavailable for the selected profile");
    }
    if (!HasLayout(context.profile, {
            "world.persistentLevel", "level.actors", "object.class", "object.nameOffset", "object.outer"})) {
        return Error("ue5.actors layout is unavailable for the selected profile");
    }
    const auto* const world_symbol = context.resolution.FindSymbol("ue5.GWorld");
    if (world_symbol == nullptr || !world_symbol->Available()) {
        return Error("ue5.GWorld is unavailable for the selected profile");
    }
    const NameResolver names(context.profile, context.resolution, context.memory);
    if (!names.Available()) return Error("ue5.FNamePool layout is unavailable for the selected profile");

    std::uintptr_t world{};
    const auto persistent_level_offset = *Layout(context.profile, "world.persistentLevel");
    const auto actors_offset = *Layout(context.profile, "level.actors");
    std::uintptr_t field{};
    std::uintptr_t level{};
    if (!ReadValue(context.memory, world_symbol->address, world) || world == 0 ||
        !AddAddress(world, persistent_level_offset, field) ||
        !ReadValue(context.memory, field, level) || level == 0) {
        return Error("current world or persistent level is unavailable");
    }

    struct ArrayHeader final {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } actors;
    if (!AddAddress(level, actors_offset, field) ||
        !ReadValue(context.memory, field, actors) || actors.count < 0 ||
        actors.count > static_cast<std::int32_t>(kMaximumActors) || actors.capacity < actors.count ||
        (actors.count != 0 && actors.data == 0)) {
        return Error("persistent level actor array is unavailable or outside the supported bound");
    }
    const std::uint32_t count = static_cast<std::uint32_t>(actors.count);
    if (request.cursor > count) return Error("actor cursor is beyond the current actor array");
    const std::uint32_t maximum_scan = static_cast<std::uint32_t>((std::min)(
        context.options.maximum_objects_per_request,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    const std::uint32_t end = (std::min)(count, request.cursor + (std::min)(
        maximum_scan, count - request.cursor));

    std::string entries;
    std::size_t matched{};
    std::size_t unreadable{};
    std::uint32_t next = end;
    for (std::uint32_t slot = request.cursor; slot < end; ++slot) {
        std::uintptr_t object_slot{};
        std::uintptr_t actor{};
        if (!AddUnsignedAddress(
                actors.data, static_cast<std::uint64_t>(slot) * sizeof(std::uintptr_t), object_slot) ||
            !ReadValue(context.memory, object_slot, actor)) {
            ++unreadable;
            continue;
        }
        if (actor == 0) continue;
        ObjectDescription description;
        if (!DescribeObject(context, names, actor, description)) {
            ++unreadable;
            continue;
        }
        if (!Matches(description, request.filter)) continue;
        if (matched != 0) entries.push_back(',');
        entries += "{\"slot\":" + std::to_string(slot) +
            ",\"address\":" + Quote(HexAddress(actor)) +
            ",\"name\":" + Quote(description.name) +
            ",\"class\":" + Quote(description.class_name) +
            ",\"outer\":" + Quote(description.outer_name) + "}";
        ++matched;
        if (matched == request.limit) {
            next = slot + 1U;
            break;
        }
    }
    const bool complete = next >= count;
    return Ok(std::string{"\"profileMode\":\"exact\""} +
              ",\"kind\":\"actors\",\"filter\":" + Quote(request.filter) +
              ",\"actorCount\":" + std::to_string(count) +
              ",\"cursor\":" + std::to_string(request.cursor) +
              ",\"nextCursor\":" + (complete ? std::string("null") : std::to_string(next)) +
              ",\"complete\":" + (complete ? "true" : "false") +
              ",\"scanned\":" + std::to_string(next - request.cursor) +
              ",\"unreadable\":" + std::to_string(unreadable) +
              ",\"actors\":[" + entries + "]");
}

std::string FunctionsJson(
    const Ue5ReflectionQueryContext& context,
    const ParsedRequest& request) {
    if (!context.resolution.FeatureAvailable("ue5.functions")) {
        return Error("ue5.functions is unavailable for the selected profile");
    }
    if (!HasLayout(context.profile, {
            "object.class", "object.nameOffset", "object.outer", "ufunction.numParms",
            "ufunction.parmsSize", "ufunction.returnValueOffset"})) {
        return Error("ue5.functions layout is unavailable for the selected profile");
    }
    const NameResolver names(context.profile, context.resolution, context.memory);
    if (!names.Available()) return Error("ue5.FNamePool layout is unavailable for the selected profile");
    ObjectRegistry registry;
    if (!LoadObjectRegistry(context, registry)) {
        return Error("ue5.GObjects registry is unavailable for the selected profile");
    }
    if (request.cursor > registry.count) return Error("function cursor is beyond the current object registry");

    const std::uint32_t maximum_scan = static_cast<std::uint32_t>((std::min)(
        context.options.maximum_objects_per_request,
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    const std::uint32_t end = (std::min)(registry.count, request.cursor + (std::min)(
        maximum_scan, registry.count - request.cursor));
    const auto num_parms_offset = *Layout(context.profile, "ufunction.numParms");
    const auto parms_size_offset = *Layout(context.profile, "ufunction.parmsSize");
    const auto return_value_offset = *Layout(context.profile, "ufunction.returnValueOffset");

    std::string entries;
    std::size_t matched{};
    std::size_t unreadable{};
    std::uint32_t next = end;
    for (std::uint32_t index = request.cursor; index < end; ++index) {
        std::uintptr_t object{};
        std::uint32_t serial{};
        if (!ReadObjectSlot(context, registry, index, object, serial)) {
            ++unreadable;
            continue;
        }
        if (object == 0) continue;
        ObjectDescription description;
        if (!DescribeObject(context, names, object, description)) {
            ++unreadable;
            continue;
        }
        if (!EqualsInsensitive(description.class_name, "Function") ||
            !Matches(description, request.filter)) {
            continue;
        }

        std::uintptr_t field{};
        std::uint8_t num_parms{};
        std::uint16_t parms_size{};
        std::uint16_t return_offset{};
        const bool metadata_readable =
            AddAddress(object, num_parms_offset, field) && ReadValue(context.memory, field, num_parms) &&
            AddAddress(object, parms_size_offset, field) && ReadValue(context.memory, field, parms_size) &&
            AddAddress(object, return_value_offset, field) && ReadValue(context.memory, field, return_offset);
        if (matched != 0) entries.push_back(',');
        entries += "{\"index\":" + std::to_string(index) +
            ",\"serial\":" + std::to_string(serial) +
            ",\"address\":" + Quote(HexAddress(object)) +
            ",\"name\":" + Quote(description.name) +
            ",\"class\":" + Quote(description.class_name) +
            ",\"owner\":" + Quote(description.outer_name) +
            ",\"metadataReadable\":" + (metadata_readable ? "true" : "false");
        if (metadata_readable) {
            entries += ",\"numParms\":" + std::to_string(num_parms) +
                ",\"parmsSize\":" + std::to_string(parms_size) +
                ",\"returnValueOffset\":" + std::to_string(return_offset);
        }
        entries += '}';
        ++matched;
        if (matched == request.limit) {
            next = index + 1U;
            break;
        }
    }
    const bool complete = next >= registry.count;
    return Ok(std::string{"\"profileMode\":\"exact\""} +
              ",\"kind\":\"functions\",\"filter\":" + Quote(request.filter) +
              ",\"objectCount\":" + std::to_string(registry.count) +
              ",\"cursor\":" + std::to_string(request.cursor) +
              ",\"nextCursor\":" + (complete ? std::string("null") : std::to_string(next)) +
              ",\"complete\":" + (complete ? "true" : "false") +
              ",\"scanned\":" + std::to_string(next - request.cursor) +
              ",\"unreadable\":" + std::to_string(unreadable) +
              ",\"functions\":[" + entries + "]");
}

}  // namespace

std::string ExecuteUe5ReflectionQuery(
    const Ue5ReflectionQueryContext& context,
    const std::string_view request) noexcept {
    try {
        std::string error;
        const auto parsed = ParseRequest(request, context.options, error);
        if (!parsed) return Error(error);
        return parsed->kind == ParsedRequest::Kind::Actors
            ? ActorsJson(context, *parsed)
            : FunctionsJson(context, *parsed);
    } catch (...) {
        return Error("UE reflection query failed");
    }
}

}  // namespace anomaly
