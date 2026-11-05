#pragma once

#include "anomaly/symbol_resolver.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace anomaly {

inline constexpr std::string_view kUe5AssetsFeature = "ue5.assets";
inline constexpr std::string_view kUe5AssetsValidator = "ue5-assets-abi-v1";
inline constexpr std::string_view kUe5DerivedClassesSymbol = "ue5.AssetRegistry.GetDerivedClassNames";
inline constexpr std::string_view kUe5MemoryFreeSymbol = "ue5.FMemory.Free";

// Native call ABI, checked against the active Profile before any invocation.
struct Ue5AssetName { std::uint32_t comparison{}, number{}; };
struct Ue5AssetPath { Ue5AssetName package, asset; };
struct Ue5AssetArray { std::uintptr_t data{}; std::int32_t count{}, capacity{}; };
struct Ue5AssetSet {
    Ue5AssetArray elements;
    std::array<std::uint32_t, 4> inline_flags{};
    std::uintptr_t heap_flags{};
    std::int32_t flag_count{}, flag_capacity{128};
    std::int32_t first_free{-1}, free_count{};
    std::uint64_t inline_hash{};
    std::uintptr_t heap_hash{};
    std::int32_t hash_size{}, padding{};
};
struct Ue5AssetSetElement { Ue5AssetPath path; std::int32_t hash_next{}, hash_index{}; };
static_assert(sizeof(Ue5AssetPath) == 16 && sizeof(Ue5AssetArray) == 16);
static_assert(sizeof(Ue5AssetSet) == 80 && sizeof(Ue5AssetSetElement) == 24);

struct Ue5AssetRegistryBinding {
    std::uintptr_t registry{}, get_derived{}, free_memory{};
    Ue5AssetPath base_class;
};

struct Ue5AssetNamePool {
    std::uintptr_t blocks{};
    std::uint32_t block_bits{}, stride{}, length_shift{};
};
std::string ReadUe5AssetName(const Ue5AssetNamePool& pool, const SymbolMemory& memory,
                           Ue5AssetName name);
std::optional<std::string> ReadUe5AssetString(const Ue5AssetArray& string, const SymbolMemory& memory);

void RegisterUe5AssetsValidator(FeatureLayoutValidatorRegistry& validators);
// The native registry owns its synchronization. No UObject/ProcessEvent calls,
// adapter mutex or Game-thread wait is permitted inside this Worker operation.
bool QueryUe5DerivedClasses(const Ue5AssetRegistryBinding& binding,
                           const SymbolMemory& memory,
                           std::vector<Ue5AssetPath>& paths);
bool ReadUe5AssetSet(const Ue5AssetSet& set, const SymbolMemory& memory,
                    std::vector<Ue5AssetPath>& paths);
void FreeUe5AssetMemory(std::uintptr_t function, std::uintptr_t allocation) noexcept;
void FreeUe5AssetStrings(std::uintptr_t function, const Ue5AssetArray& strings,
                        std::int32_t first, const SymbolMemory& memory) noexcept;

} // namespace anomaly
