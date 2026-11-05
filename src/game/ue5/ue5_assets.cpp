#include "anomaly/ue5_assets.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace anomaly {
namespace {

FeatureValidationResult ValidateAssets(const BuildProfile& profile, std::string_view feature,
    const ProfileResolutionSnapshot& resolution, const SymbolMemory&) {
    if (feature != kUe5AssetsFeature) return {false, "asset validator used by another feature"};
    // These are the layout of the native call's value types, not a second set of
    // game-object offsets. A changed allocator ABI must not reach native code.
    const std::pair<std::string_view, std::size_t> fields[]{
        {"assets.pathSize", sizeof(Ue5AssetPath)},
        {"assets.setSize", sizeof(Ue5AssetSet)},
        {"assets.setElementStride", sizeof(Ue5AssetSetElement)},
        {"assets.setFlags", offsetof(Ue5AssetSet, inline_flags)},
        {"assets.setHeapFlags", offsetof(Ue5AssetSet, heap_flags)},
        {"assets.setFlagCount", offsetof(Ue5AssetSet, flag_count)},
        {"assets.setFlagCapacity", offsetof(Ue5AssetSet, flag_capacity)},
        {"assets.setFirstFree", offsetof(Ue5AssetSet, first_free)},
        {"assets.setFreeCount", offsetof(Ue5AssetSet, free_count)},
        {"assets.setHeapHash", offsetof(Ue5AssetSet, heap_hash)},
        {"assets.setHashSize", offsetof(Ue5AssetSet, hash_size)},
        {"assets.setInlineFlagBits", sizeof(Ue5AssetSet::inline_flags) * 8U}};
    for (const auto& [key, value] : fields) {
        const auto found = profile.layout.find(key);
        if (found == profile.layout.end() || found->second != static_cast<std::int64_t>(value))
            return {false, "unsupported asset ABI: " + std::string(key)};
    }
    for (const auto id : {kUe5DerivedClassesSymbol, kUe5MemoryFreeSymbol}) {
        const auto* symbol = resolution.FindSymbol(id);
        if (!symbol || !symbol->Available()) return {false, "asset entry unavailable: " + std::string(id)};
    }
    const auto slot = profile.layout.find("assets.derivedClassesVtableSlot");
    if (slot == profile.layout.end() || slot->second < 0 || slot->second > 4096 || slot->second % 8 != 0)
        return {false, "asset registry slot unavailable"};
    return {true, {}};
}

bool InvokeQuery(const Ue5AssetRegistryBinding& binding, Ue5AssetSet& result) noexcept {
    Ue5AssetArray bases{reinterpret_cast<std::uintptr_t>(&binding.base_class), 1, 1};
    Ue5AssetSet excluded;
    using Query = void(__fastcall*)(std::uintptr_t, const Ue5AssetArray*,
                                   const Ue5AssetSet*, Ue5AssetSet*);
    __try {
        reinterpret_cast<Query>(binding.get_derived)(binding.registry, &bases, &excluded, &result);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void RegisterUe5AssetsValidator(FeatureLayoutValidatorRegistry& validators) {
    validators.Register(std::string(kUe5AssetsValidator), ValidateAssets);
}

std::optional<std::string> ReadUe5AssetString(const Ue5AssetArray& string, const SymbolMemory& memory) {
    if (string.count < 0 || string.count > 65536 || string.capacity < string.count || string.capacity > 65536)
        return {};
    if (string.count == 0) return std::string{};
    if (!string.data) return {};
    std::wstring wide(static_cast<std::size_t>(string.count), L'\0');
    if (!memory.Read(string.data, wide.data(), wide.size() * sizeof(wchar_t)) || wide.back() != L'\0') return {};
    wide.pop_back();
    if (wide.empty()) return std::string{};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string value(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            value.data(), size, nullptr, nullptr) != size) return {};
    return value;
}

std::string ReadUe5AssetName(const Ue5AssetNamePool& pool, const SymbolMemory& memory,
                           Ue5AssetName name) {
    if (!pool.blocks || pool.block_bits == 0 || pool.block_bits > 16 ||
        pool.stride == 0 || pool.stride > 16 || pool.length_shift == 0 || pool.length_shift >= 16)
        return {};
    const auto index = name.comparison >> pool.block_bits;
    if (index >= 8192) return {};
    std::uintptr_t block{};
    if (!memory.Read(pool.blocks + index * sizeof(block), &block, sizeof(block)) || !block) return {};
    const auto offset = name.comparison & ((1U << pool.block_bits) - 1U);
    const auto entry = block + static_cast<std::uintptr_t>(offset) * pool.stride;
    std::uint16_t header{};
    if (!memory.Read(entry, &header, sizeof(header))) return {};
    const auto length = header >> pool.length_shift;
    if (length == 0 || length > 1024) return {};
    std::string value;
    if ((header & 1U) == 0) {
        value.resize(length);
        if (!memory.Read(entry + sizeof(header), value.data(), value.size())) return {};
    } else {
        std::wstring wide(length, L'\0');
        if (!memory.Read(entry + sizeof(header), wide.data(), wide.size() * sizeof(wchar_t))) return {};
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
            length, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        value.resize(static_cast<std::size_t>(size));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                value.data(), size, nullptr, nullptr) != size) return {};
    }
    if (name.number) value += "_" + std::to_string(name.number - 1U);
    return value;
}

void FreeUe5AssetMemory(std::uintptr_t function, std::uintptr_t allocation) noexcept {
    if (!allocation) return;
    using Free = void(__fastcall*)(void*);
    __try { reinterpret_cast<Free>(function)(reinterpret_cast<void*>(allocation)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

void FreeUe5AssetStrings(std::uintptr_t function, const Ue5AssetArray& strings,
                        std::int32_t first, const SymbolMemory& memory) noexcept {
    if (!strings.data) return;
    if (first >= 0 && strings.count >= first && strings.count <= 65536 &&
        strings.capacity >= strings.count && strings.capacity <= 65536) {
        for (auto i = first; i < strings.count; ++i) {
            Ue5AssetArray string;
            if (memory.Read(strings.data + static_cast<std::size_t>(i) * sizeof(string), &string, sizeof(string)))
                FreeUe5AssetMemory(function, string.data);
        }
    }
    FreeUe5AssetMemory(function, strings.data);
}

bool ReadUe5AssetSet(const Ue5AssetSet& set, const SymbolMemory& memory,
                    std::vector<Ue5AssetPath>& paths) {
    constexpr std::int32_t maximum = 262144;
    const auto count = set.elements.count;
    if (count < 0 || count > maximum || set.elements.capacity < count ||
        set.elements.capacity > maximum || set.free_count < 0 || set.free_count > count ||
        set.flag_count != count || set.flag_capacity < count || set.flag_capacity > maximum ||
        (count != 0 && set.elements.data == 0)) return false;
    const auto words = (static_cast<std::size_t>(count) + 31U) / 32U;
    std::vector<std::uint32_t> flags(words);
    if (set.heap_flags) {
        if (!memory.Read(set.heap_flags, flags.data(), words * sizeof(std::uint32_t))) return false;
    } else {
        if (words > set.inline_flags.size()) return false;
        std::copy_n(set.inline_flags.begin(), words, flags.begin());
    }
    std::vector<Ue5AssetSetElement> entries(static_cast<std::size_t>(count));
    if (count && !memory.Read(set.elements.data, entries.data(), entries.size() * sizeof(entries[0])))
        return false;
    std::vector<Ue5AssetPath> result;
    result.reserve(static_cast<std::size_t>(count - set.free_count));
    for (std::int32_t i = 0; i < count; ++i) {
        if ((flags[static_cast<std::size_t>(i) / 32U] & (1U << (i % 32))) != 0)
            result.push_back(entries[static_cast<std::size_t>(i)].path);
    }
    if (result.size() != static_cast<std::size_t>(count - set.free_count)) return false;
    paths = std::move(result);
    return true;
}

bool QueryUe5DerivedClasses(const Ue5AssetRegistryBinding& binding,
                           const SymbolMemory& memory, std::vector<Ue5AssetPath>& paths) {
    Ue5AssetSet output;
    if (!binding.registry || !binding.get_derived || !binding.free_memory || !InvokeQuery(binding, output))
        return false;
    struct Release {
        const Ue5AssetRegistryBinding& binding;
        Ue5AssetSet& value;
        ~Release() {
            FreeUe5AssetMemory(binding.free_memory, value.elements.data);
            FreeUe5AssetMemory(binding.free_memory, value.heap_flags);
            FreeUe5AssetMemory(binding.free_memory, value.heap_hash);
        }
    } release{binding, output};
    return ReadUe5AssetSet(output, memory, paths);
}

} // namespace anomaly
