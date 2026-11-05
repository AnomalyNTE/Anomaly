#include "anomaly/ue5_assets.hpp"
#include "anomaly/ue5_nte_adapter.hpp"
#include "anomaly/ue5_process_event.hpp"

#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>

extern "C" int AssetsCContract(void);

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Memory final : public anomaly::SymbolMemory {
public:
    std::vector<std::span<const std::byte>> ranges;
    template<class T> void Add(const T& value) {
        ranges.emplace_back(reinterpret_cast<const std::byte*>(&value), sizeof(value));
    }
    std::optional<ue5mem::ModuleInfo> FindModule(std::wstring_view) const override { return {}; }
    std::vector<ue5mem::SectionInfo> Sections(const ue5mem::ModuleInfo&) const override { return {}; }
    std::vector<std::uintptr_t> Scan(const ue5mem::ModuleInfo&, std::string_view, std::string_view, std::size_t) const override { return {}; }
    std::optional<anomaly::SymbolMemoryRegion> Query(std::uintptr_t) const override { return {}; }
    bool Read(std::uintptr_t address, void* result, std::size_t size) const override {
        for (const auto range : ranges) {
            const auto base = reinterpret_cast<std::uintptr_t>(range.data());
            if (address >= base && address - base <= range.size() && size <= range.size() - (address - base)) {
                std::memcpy(result, reinterpret_cast<const void*>(address), size);
                return true;
            }
        }
        return false;
    }
};

anomaly::Ue5AssetSet g_output;
std::vector<std::uintptr_t> g_released;
bool g_called{};
void __fastcall NativeQuery(std::uintptr_t registry, const anomaly::Ue5AssetArray* bases,
                           const anomaly::Ue5AssetSet* excluded, anomaly::Ue5AssetSet* out) {
    g_called = registry == 1 && bases->count == 1 && bases->capacity == 1 &&
        excluded->elements.count == 0 && excluded->first_free == -1 && excluded->flag_capacity == 128;
    *out = g_output;
}
void __fastcall NativeFree(void* memory) { g_released.push_back(reinterpret_cast<std::uintptr_t>(memory)); }

void NativeContract() {
    Memory memory;
    // A hole contains a retired path that must never be exposed as a class.
    const anomaly::Ue5AssetSetElement entries[]{
        {{{21, 0}, {22, 0}}, -1, 0}, {{{999, 0}, {999, 0}}, -1, 0},
        {{{31, 0}, {32, 4}}, -1, 1}};
    memory.Add(entries);
    g_output = {};
    g_output.elements = {reinterpret_cast<std::uintptr_t>(entries), 3, 3};
    g_output.inline_flags[0] = 5;
    g_output.flag_count = 3;
    g_output.free_count = 1;
    anomaly::Ue5AssetRegistryBinding binding{1, reinterpret_cast<std::uintptr_t>(&NativeQuery),
        reinterpret_cast<std::uintptr_t>(&NativeFree), {{11, 0}, {12, 0}}};
    std::vector<anomaly::Ue5AssetPath> paths;
    Require(anomaly::QueryUe5DerivedClasses(binding, memory, paths), "valid native set rejected");
    Require(g_called && paths.size() == 2 && paths[0].asset.comparison == 22 &&
        paths[1].package.comparison == 31 && paths[1].asset.number == 4, "sparse class paths changed");
    Require(g_released == std::vector<std::uintptr_t>{g_output.elements.data}, "inline allocation freed incorrectly");

    const std::uint32_t flags[]{5};
    memory.Add(flags);
    g_output.heap_flags = reinterpret_cast<std::uintptr_t>(flags);
    g_output.heap_hash = 123;
    g_output.inline_flags[0] = 0;
    g_released.clear();
    Require(anomaly::QueryUe5DerivedClasses(binding, memory, paths), "heap flags ignored");
    Require(g_released == std::vector<std::uintptr_t>{g_output.elements.data, g_output.heap_flags, 123},
        "native allocator cleanup incomplete");
    g_output.free_count = 0;
    g_released.clear();
    Require(!anomaly::QueryUe5DerivedClasses(binding, memory, paths), "inconsistent occupancy accepted");
    Require(g_released.size() == 3, "failed decode leaked native allocations");
    g_output.elements.count = -1;
    Require(!anomaly::ReadUe5AssetSet(g_output, memory, paths), "negative array size accepted");
    g_output = {};
    Require(anomaly::ReadUe5AssetSet(g_output, memory, paths) && paths.empty(), "empty registry rejected");

    const wchar_t text[]{L'\x5A1C', L'\x5A1C', L'\x8389', 0};
    memory.Add(text);
    const anomaly::Ue5AssetArray string{reinterpret_cast<std::uintptr_t>(text), 4, 4};
    Require(anomaly::ReadUe5AssetString(string, memory) == "\xE5\xA8\x9C\xE5\xA8\x9C\xE8\x8E\x89", "source string is not UTF-8");
    Require(!anomaly::ReadUe5AssetString({string.data, 65537, 65537}, memory), "unbounded source string read");
    Require(!anomaly::ReadUe5AssetString({string.data, -1, 4}, memory), "negative source length accepted");
    Require(anomaly::ReadUe5AssetString({}, memory) == std::string{}, "empty source treated as a failed read");

    // Cancellation after one copied key must release only the remaining keys
    // and the outer array, including when another key header is unreadable.
    const anomaly::Ue5AssetArray keys[]{ {101, 2, 2}, {202, 2, 2}, {303, 2, 2} };
    memory.Add(keys);
    const anomaly::Ue5AssetArray key_array{reinterpret_cast<std::uintptr_t>(keys), 3, 3};
    g_released.clear();
    anomaly::FreeUe5AssetStrings(binding.free_memory, key_array, 1, memory);
    Require(g_released == std::vector<std::uintptr_t>{202, 303, key_array.data},
        "partial name-table copy leaked or double-freed keys");
    g_released.clear();
    anomaly::FreeUe5AssetStrings(binding.free_memory, {key_array.data, 4, 4}, 1, memory);
    Require(g_released == std::vector<std::uintptr_t>{202, 303, key_array.data},
        "unreadable key header escaped safe cleanup");
}

void ProfileAndEndpointContract(const std::filesystem::path& path) {
    const auto loaded = anomaly::LoadBuildProfile(path);
    Require(loaded.Ok(), "bundled Profile is invalid");
    auto profile = *loaded.profile;
    anomaly::ProfileResolutionSnapshot resolution;
    for (const auto id : {anomaly::kUe5DerivedClassesSymbol, anomaly::kUe5MemoryFreeSymbol}) {
        anomaly::ResolvedSymbol symbol;
        symbol.id = id;
        symbol.state = anomaly::SymbolResolutionState::Resolved;
        symbol.address = 1;
        resolution.symbols.emplace(id, symbol);
    }
    auto memory = std::make_shared<Memory>();
    auto validators = anomaly::Ue5FeatureLayoutValidators();
    const auto validate = [&] {
        return validators.Validate(anomaly::kUe5AssetsValidator, profile, anomaly::kUe5AssetsFeature,
                                   resolution, *memory).valid;
    };
    Require(validate(), "supported asset ABI rejected");
    profile.layout.erase("assets.setFreeCount");
    Require(!validate(), "missing asset layout accepted");
    profile = *loaded.profile;
    profile.layout["assets.setElementStride"] = 16;
    Require(!validate(), "wrong native stride accepted");
    profile = *loaded.profile;
    const auto free_symbol = resolution.symbols.extract(std::string(anomaly::kUe5MemoryFreeSymbol));
    Require(!validate(), "query allowed without allocator release");
    resolution.symbols.emplace(free_symbol.key(), free_symbol.mapped());

    // Exercise the public cached table after stop/destruction. This detects the
    // draft's endpoint-as-State cast without requiring or invoking a game.
    for (const std::string id : {"ue5.assets", "ue5.objects", "ue5.object-find"}) {
        anomaly::FeatureResolution feature;
        feature.id = id; feature.available = true;
        resolution.features.emplace(id, std::move(feature));
    }
    anomaly::AdapterServiceRegistry registry;
    const AnomalyUe5AssetsServiceV1* service{};
    const std::string base = "/Script/Engine.Actor";
    const AnomalyStringViewV1 view{base.data(), base.size()};
    {
        anomaly::Ue5NteAdapter adapter({}, profile, resolution, memory, registry, {}, {},
            [](auto, auto, auto, auto) { return false; }, [](const wchar_t*) { return std::uintptr_t{}; });
        Require(adapter.Start(true), "adapter did not start");
        service = static_cast<const AnomalyUe5AssetsServiceV1*>(registry.Query(ANOMALY_UE5_ASSETS_SERVICE_V1_ID, 1));
        Require(service && service->struct_size == sizeof(*service), "asset service not published");
        Require(service->prepare_class(service->user, view).code == ANOMALY_STATUS_V1_UNAVAILABLE,
            "unbound thread entered Game-only preparation");
        Require(service->enumerate_derived(service->user, {}, nullptr, nullptr).code == ANOMALY_STATUS_V1_INVALID_ARGUMENT,
            "invalid enumeration accepted");
        Require(adapter.Stop(), "asset endpoint failed to drain");
        Require(registry.Query(ANOMALY_UE5_ASSETS_SERVICE_V1_ID, 1) == nullptr, "stopped service still published");
    }
    Require(service->prepare_class(service->user, view).code == ANOMALY_STATUS_V1_UNAVAILABLE,
        "cached table accessed destroyed State");
    Require(service->prepare_string_table(service->user, view).code == ANOMALY_STATUS_V1_UNAVAILABLE,
        "cached name-table request accessed destroyed State");
}

// Exercise preparation through the public service, including the reflected
// engine boundary. An empty entry exists in the actual ST_ActorName asset;
// checking the FString decoder alone does not cover the adapter's key policy.
class StringTableEngine {
public:
    anomaly::BuildProfile profile;
    anomaly::ProfileResolutionSnapshot resolution;
    std::shared_ptr<Memory> memory{std::make_shared<Memory>()};
    std::map<std::wstring, std::uintptr_t> objects;
    std::vector<std::unique_ptr<std::byte[]>> allocations;
    std::uintptr_t names{}, name_block{}, registry{}, slots{}, library{};
    std::uint32_t name_cursor{2}, object_count{};
    std::map<std::uintptr_t, std::string> functions;
    std::vector<anomaly::Ue5AssetArray> keys;
    bool malformed_key{};
    unsigned source_queries{};

    std::uintptr_t Block(std::size_t size = 512) {
        auto block = std::make_unique<std::byte[]>(size);
        auto address = reinterpret_cast<std::uintptr_t>(block.get());
        memory->ranges.emplace_back(block.get(), size);
        allocations.push_back(std::move(block));
        return address;
    }
    template<class T> void Put(std::uintptr_t address, T value) {
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    }
    template<class T> void Field(std::uintptr_t object, const char* field, T value) {
        Put(object + profile.layout.at(field), value);
    }
    void Symbol(const char* id, std::uintptr_t address) {
        anomaly::ResolvedSymbol symbol;
        symbol.id = id; symbol.address = address; symbol.state = anomaly::SymbolResolutionState::Resolved;
        resolution.symbols.emplace(id, std::move(symbol));
    }
    std::uint32_t Name(std::string_view value) {
        const auto offset = name_cursor;
        Put(name_block + offset, static_cast<std::uint16_t>(value.size() << profile.layout.at("names.headerLengthShift")));
        std::memcpy(reinterpret_cast<void*>(name_block + offset + 2), value.data(), value.size());
        name_cursor += static_cast<std::uint32_t>((value.size() + 3) & ~std::size_t{1});
        return offset / static_cast<std::uint32_t>(profile.layout.at("names.entryStride"));
    }
    std::uintptr_t Object(std::wstring path = {}) {
        const auto object = Block();
        Field(object, "object.internalIndex", static_cast<std::int32_t>(object_count));
        Put(slots + object_count * profile.layout.at("objects.itemStride") + profile.layout.at("objects.objectOffset"), object);
        Field(registry, "objects.countOffset", ++object_count);
        if (!path.empty()) objects.emplace(std::move(path), object);
        return object;
    }
    struct Parameter { const char* name; const char* type; std::int32_t offset, size; };
    void Function(const wchar_t* path, const char* kind, std::uint16_t size, std::uint16_t returned,
                  std::initializer_list<Parameter> parameters) {
        const auto function = Object(path);
        functions.emplace(function, kind);
        Field(function, "object.outer", library);
        Field(function, "ufunction.numParms", static_cast<std::uint8_t>(parameters.size()));
        Field(function, "ufunction.parmsSize", size);
        Field(function, "ufunction.returnValueOffset", returned);
        auto link = function + profile.layout.at("ustruct.propertyLink");
        for (const auto& parameter : parameters) {
            const auto property = Block(), type = Block();
            Put(link, property);
            Field(type, "ffieldClass.name", Name(parameter.type));
            Field(property, "ffield.class", type);
            Field(property, "ffield.name", Name(parameter.name));
            Field(property, "fproperty.arrayDim", std::int32_t{1});
            Field(property, "fproperty.elementSize", parameter.size);
            Field(property, "fproperty.offsetInternal", parameter.offset);
            if (std::string_view(parameter.type) == "StructProperty")
                Field(property, "fstructProperty.struct", Block());
            link = property + profile.layout.at("fproperty.propertyLinkNext");
        }
    }
    anomaly::Ue5AssetArray String(std::wstring_view value) {
        const auto count = static_cast<std::int32_t>(value.size() + 1);
        const auto data = Block(static_cast<std::size_t>(count) * sizeof(wchar_t));
        std::memcpy(reinterpret_cast<void*>(data), value.data(), value.size() * sizeof(wchar_t));
        return {data, count, count};
    }
    explicit StringTableEngine(const std::filesystem::path& path) : profile(*anomaly::LoadBuildProfile(path).profile) {
        names = Block(); name_block = Block(8192); registry = Block(); slots = Block(4096);
        Field(names, "names.blocksOffset", name_block);
        const auto chunks = Block(); Put(chunks, slots);
        Field(registry, "objects.itemsOffset", chunks);
        Field(registry, "objects.maxCountOffset", std::uint32_t{64});
        Field(registry, "objects.maxChunksOffset", std::uint32_t{1});
        Field(registry, "objects.numChunksOffset", std::uint32_t{1});
        const auto world = Block(); Put(world, Block());
        Symbol("ue5.GWorld", world); Symbol("ue5.GObjects", registry); Symbol("ue5.FNamePool", names);
        Symbol(anomaly::kUe5MemoryFreeSymbol.data(), reinterpret_cast<std::uintptr_t>(&NativeFree));
        for (const std::string id : {"ue5.assets", "ue5.objects", "ue5.object-find", "ue5.names"}) {
            anomaly::FeatureResolution feature; feature.id = id; feature.available = true;
            resolution.features.emplace(id, std::move(feature));
        }
        library = Object(); Field(library, "uclass.classDefaultObject", Object());
        const auto table = Object(L"/Game/Text/ST_ActorName.ST_ActorName"), type = Object();
        Field(table, "object.class", type); Field(type, "object.nameOffset", Name("StringTable"));
        Function(L"/Script/Engine.KismetStringLibrary.Conv_StringToName", "name", 24, 16,
            {{"InString", "StrProperty", 0, 16}, {"ReturnValue", "NameProperty", 16, 8}});
        Function(L"/Script/Engine.KismetSystemLibrary.MakeSoftObjectPath", "path", 48, 16,
            {{"PathString", "StrProperty", 0, 16}, {"ReturnValue", "StructProperty", 16, 32}});
        Function(L"/Script/Engine.KismetSystemLibrary.LoadAsset", "load", 88, 65535,
            {{"WorldContextObject", "ObjectProperty", 0, 8}, {"Asset", "SoftObjectProperty", 8, 40},
             {"OnLoaded", "DelegateProperty", 48, 16}, {"LatentInfo", "StructProperty", 64, 24}});
        Function(L"/Script/Engine.KismetStringTableLibrary.GetKeysFromStringTable", "keys", 24, 8,
            {{"TableID", "NameProperty", 0, 8}, {"ReturnValue", "ArrayProperty", 8, 16}});
        Function(L"/Script/Engine.KismetStringTableLibrary.GetTableEntrySourceString", "source", 40, 24,
            {{"TableID", "NameProperty", 0, 8}, {"Key", "StrProperty", 8, 16}, {"ReturnValue", "StrProperty", 24, 16}});
    }
    bool Invoke(std::uintptr_t function, void* parameters) {
        const auto address = reinterpret_cast<std::uintptr_t>(parameters);
        const auto& kind = functions.at(function);
        if (kind == "name") Put(address + 16, anomaly::Ue5AssetName{1, 0});
        if (kind == "path") Put(address + 16, anomaly::Ue5AssetPath{{1, 0}, {2, 0}});
        if (kind == "keys") {
            keys.clear();
            for (unsigned i = 0; i < 8; ++i) keys.push_back(String(L"before_" + std::to_wstring(i)));
            // Cover a zero-allocation empty FString after a preparation batch.
            keys.push_back(malformed_key ? anomaly::Ue5AssetArray{0, 3, 3} : anomaly::Ue5AssetArray{});
            keys.push_back(String(L"boss_07_Name"));
            const auto data = Block(keys.size() * sizeof(keys[0]));
            std::memcpy(reinterpret_cast<void*>(data), keys.data(), keys.size() * sizeof(keys[0]));
            Put(address + 8, anomaly::Ue5AssetArray{data, 10, 10});
        }
        if (kind == "source") {
            ++source_queries;
            anomaly::Ue5AssetArray key;
            std::memcpy(&key, reinterpret_cast<void*>(address + 8), sizeof(key));
            const auto value = anomaly::ReadUe5AssetString(key, *memory);
            Require(value.has_value(), "invalid FString reached engine call");
            Put(address + 24, value->empty() ? anomaly::Ue5AssetArray{} : String(L"\x585E\x6DA6\x5C3C\x7F07"));
        }
        return true;
    }
};

void StringTablePreparationContract(const std::filesystem::path& path) {
    StringTableEngine engine(path);
    anomaly::AdapterServiceRegistry registry;
    anomaly::Ue5NteAdapter adapter({}, engine.profile, engine.resolution, engine.memory, registry, {}, {},
        [&](auto, auto function, auto parameters, auto) { return engine.Invoke(function, parameters); },
        [&](const wchar_t* object) { const auto at = engine.objects.find(object); return at == engine.objects.end() ? 0 : at->second; });
    Require(adapter.Start(true), "string-table adapter did not start");
    adapter.OnGameTick(1.0 / 60.0);
    const auto service = static_cast<const AnomalyUe5AssetsServiceV1*>(registry.Query(ANOMALY_UE5_ASSETS_SERVICE_V1_ID, 1));
    Require(service, "string-table service not published");
    const std::string asset = "/Game/Text/ST_ActorName.ST_ActorName";
    const AnomalyStringViewV1 view{asset.data(), asset.size()};
    std::string preparation_message;
    const auto prepare = [&]() -> std::uint32_t {
        for (unsigned tick = 0; tick < 64; ++tick) {
            const auto calls = engine.source_queries;
            const auto status = service->prepare_string_table(service->user, view);
            preparation_message.assign(status.message.data ? status.message.data : "", status.message.size);
            Require(engine.source_queries - calls <= 8, "string-table preparation exceeded its entry budget");
            if (status.code != ANOMALY_STATUS_V1_UNAVAILABLE) return status.code;
        }
        return ANOMALY_STATUS_V1_UNAVAILABLE;
    };
    using Entries = std::map<std::string, std::string>;
    Entries entries;
    const auto enumerate = [&] {
        std::uint32_t status{};
        std::thread worker([&] {
            status = service->enumerate_string_table(service->user, view,
                [](void* user, AnomalyStringViewV1 key, AnomalyStringViewV1 value) {
                    static_cast<Entries*>(user)->emplace(std::string(key.data, key.size), std::string(value.data, value.size));
                    return 1;
                }, &entries).code;
        });
        worker.join();
        return status;
    };
    engine.malformed_key = true;
    Require(prepare() == ANOMALY_STATUS_V1_FAILED, "unreadable key accepted");
    Require(engine.source_queries == 8, "fixture did not reach the unreadable key");
    Require(enumerate() == ANOMALY_STATUS_V1_UNAVAILABLE && entries.empty(), "failed copy published a partial table");
    engine.malformed_key = false;
    const auto prepared = prepare();
    Require(prepared == ANOMALY_STATUS_V1_OK,
        ("valid empty key aborted string-table preparation: " + preparation_message).c_str());
    Require(enumerate() == ANOMALY_STATUS_V1_OK && entries.size() == 10 && entries.at("").empty() &&
        entries.at("boss_07_Name") == "\xE5\xA1\x9E\xE6\xB6\xA6\xE5\xB0\xBC\xE7\xBC\x87",
        "complete table did not retain entries following an empty key");
    Require(adapter.Stop(), "string-table adapter failed to stop");
}
}

int main(int argc, char** argv) {
    try {
        Require(argc == 2 && AssetsCContract(), "C ABI contract failed");
        NativeContract();
        ProfileAndEndpointContract(argv[1]);
        StringTablePreparationContract(argv[1]);
        std::cout << "assets: ABI, sparse arrays, cleanup, UTF-8, empty-key preparation, Profile rejection and endpoint stop passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
