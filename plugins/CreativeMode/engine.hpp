#pragma once
#include "model.hpp"
#include "gizmo.hpp"
#include "catalog_names.hpp"
#include <anomaly/sdk/localization.hpp>
#include <anomaly/sdk/profile.hpp>
#include <chrono>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace creative_mode {
using Address = std::uintptr_t;
using Handle = AnomalyGenerationHandleV1;
inline bool Same(Handle a, Handle b) { return a.id == b.id && a.generation == b.generation; }
bool InvokeNative(Address entry, Address receiver, Address function, void* parameters) noexcept;
bool InvokeAccessor(Address entry, Address receiver, Address* result) noexcept;
bool InvokeDeferredCapture(Address entry, Address receiver) noexcept;
bool ExportSharedNative(Address resource, Address* shared_handle) noexcept;
void CloseSharedNative(Address shared_handle) noexcept;
struct NativeArray { Address data{}; std::int32_t count{}, capacity{}; };
static_assert(sizeof(NativeArray) == 16);
using NativeName = std::array<std::uint32_t,2>;
enum class CaptureSource : std::uint8_t { FinalColorLDR = 2 };
enum class CaptureUnlitViewmode : std::uint8_t { Capture = 1 };
struct NativeAssetPath { NativeName package,asset; };
struct SoftAssetPath { NativeAssetPath asset; NativeArray subpath; };
struct SoftAssetReference { std::uint64_t weak{}; SoftAssetPath path; };
struct LatentAction { std::int32_t linkage{-1},uuid{}; NativeName function{}; Address target{}; };
static_assert(sizeof(SoftAssetPath) == 32 && sizeof(SoftAssetReference) == 40 && sizeof(LatentAction) == 24);
std::wstring NativeWide(std::string_view text);
std::string NativeContentVersion();
bool QueryAssetRecords(Address entry,Address registry,const NativeAssetPath& type,NativeArray& records) noexcept;
bool ReleaseAssetRecords(Address destroy,Address free_memory,NativeArray records) noexcept;
bool FreeNativeMemory(Address free_memory,Address allocation) noexcept;
struct ShowFlagSetting { NativeArray name; bool enabled{}; std::array<std::uint8_t,7> padding{}; };
static_assert(sizeof(ShowFlagSetting) == 24 && offsetof(ShowFlagSetting,enabled) == 16);

struct FieldSpec { const char* name; const char* type; std::size_t size; const char* structure{}; };
struct Field {
    std::size_t offset{}, size{};
    std::array<std::uint32_t,2> name{};
    std::uint8_t bool_byte{},bool_mask{255};
};
struct Function { Handle handle; std::size_t size{}; std::map<std::string, Field, std::less<>> fields; };
struct Parameters {
    Parameters(const Function& f, const anomaly::sdk::Localizer& localizer) : function(f), localizer(localizer) {}
    template<class T> void Put(std::string_view name, const T& value) {
        const auto& f = function.fields.at(std::string(name));
        if (f.size != sizeof(T)) throw std::runtime_error(localizer.Text("error.parameter_size", "Engine argument size mismatch"));
        std::memcpy(bytes.data() + f.offset, &value, sizeof(T));
    }
    template<class T> T Get(std::string_view name) const {
        const auto& f = function.fields.at(std::string(name));
        if (f.size != sizeof(T)) throw std::runtime_error(localizer.Text("error.parameter_size", "Engine argument size mismatch"));
        T value{}; std::memcpy(&value, bytes.data() + f.offset, sizeof(T)); return value;
    }
    alignas(16) std::array<std::uint8_t, 1024> bytes{};
    const Function& function;
    const anomaly::sdk::Localizer& localizer;
};
struct Asset { std::string path; bool skeletal{}; AssetName name; std::string search; };
inline std::string SearchText(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c+'a'-'A');
    return text;
}
class AssetFilter {
public:
    explicit AssetFilter(std::string_view text) {
        while (!text.empty()) {
            const auto comma = text.find(','); auto term = text.substr(0,comma);
            while (!term.empty() && term.front() == ' ') term.remove_prefix(1);
            while (!term.empty() && term.back() == ' ') term.remove_suffix(1);
            if (!term.empty()) {
                const bool exclude = term.front() == '-'; if (exclude) term.remove_prefix(1);
                if (!term.empty()) terms_.push_back({SearchText(std::string(term)),exclude});
            }
            if (comma == std::string_view::npos) break; text.remove_prefix(comma+1);
        }
    }
    bool Match(const Asset& asset) const {
        if (terms_.empty()) return true;
        const auto temporary = asset.search.empty() ? SearchText(asset.name.label+" "+asset.path) : std::string{};
        const std::string_view text = asset.search.empty() ? temporary : asset.search;
        bool included{},positive{};
        for (const auto& [term,exclude] : terms_) {
            const bool match = text.find(term) != std::string_view::npos;
            if (exclude && match) return false;
            if (!exclude) { positive = true; included |= match; }
        }
        return !positive || included;
    }
private:
    std::vector<std::pair<std::string,bool>> terms_;
};
struct RegistryBinding {
    Address registry{},query{};
    NativeAssetPath static_mesh,skeletal_mesh;
    std::size_t stride{},package_offset{},name_offset{};
};
struct LiveItem {
    std::uint64_t id{};
    Item item;
    Handle actor;
    Vector center{}, extent{};
    bool has_collision_data{};
    std::array<Vector,8> corners{};
};

class Engine {
public:
    Engine(const AnomalyCoreServiceV1& core, const AnomalyUe5ObjectsServiceV1& objects,
           const AnomalyUe5NamesServiceV1& names, const AnomalyUe5FrameworkServiceV1& framework,
           const anomaly::sdk::Localizer& localizer, const CatalogNames& catalog_names)
        : core_(core), objects_(objects), names_(names), framework_(framework), l10n_(localizer), catalog_names_(catalog_names) {}

    void Initialize(const AnomalyHostApiV1* host) {
        if (!profile_.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "creative-mode-v1"},
                           {"ue5.ProcessEvent.prologue","ue5.RenderTarget.RhiRef.entry","ue5.RHITexture.NativeResource.entry","ue5.AssetRegistry.Query.entry"})) LocalizedStatus("error.profile", "The compatibility profile could not be loaded");
        registry_ = Rip("ue5.GObjects"); world_slot_ = Rip("ue5.GWorld");
        const auto* selected = profile_.Selected("ue5.ProcessEvent");
        if (!selected || !profile_.Address("ue5.ProcessEvent") || selected->values.at("entryOffset") != 5)
            LocalizedStatus("error.entry", "The engine invocation entry did not validate");
        entry_ = profile_.Address("ue5.ProcessEvent") - selected->values.at("entryOffset");
        if (!profile_.MatchesEntry(&core_, "ue5.ProcessEvent.prologue", entry_, true))
            LocalizedStatus("error.entry", "The engine invocation entry did not validate");
        // All offsets come from the package Profile. Structural constraints are
        // implementation contracts; there is no numeric layout fallback.
        if (Offset("objects.itemStride") < sizeof(Address) || !Offset("objects.chunkSize") ||
            Offset("objects.objectOffset") + sizeof(Address) > Offset("objects.itemStride") ||
            Offset("objects.serialOffset") + sizeof(std::uint32_t) > Offset("objects.itemStride"))
            LocalizedStatus("error.profile", "The compatibility profile could not be loaded");
    }
    template<class T> T Read(Address address) const {
        T value{};
        if (!address || core_.read_memory(core_.user, address,
                {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)}).code != 0)
            LocalizedStatus("error.memory", "Engine object memory is unavailable");
        return value;
    }
    std::uint32_t Offset(std::string_view key) const { return profile_.Value(key, 1048576); }
    template<class T> T Member(Address object, std::string_view key) const { return Read<T>(object + Offset(key)); }
    Address ObjectAt(std::uint32_t index, std::uint32_t& serial) const {
        const auto count = Member<std::int32_t>(registry_, "objects.countOffset");
        const auto chunk_count = Member<std::int32_t>(registry_, "objects.numChunksOffset");
        const auto chunk_size = Offset("objects.chunkSize");
        if (count <= 0 || index >= static_cast<std::uint32_t>(count) || chunk_count <= 0 ||
            index / chunk_size >= static_cast<std::uint32_t>(chunk_count)) return 0;
        const auto chunks = Member<Address>(registry_, "objects.itemsOffset");
        const auto chunk = Read<Address>(chunks + (index / chunk_size) * sizeof(Address));
        if (!chunk) return 0;
        const auto item = chunk + (index % chunk_size) * Offset("objects.itemStride");
        serial = Member<std::uint32_t>(item, "objects.serialOffset");
        return Member<Address>(item, "objects.objectOffset");
    }
    Handle Pin(Address object) const {
        if (!object) return {};
        const auto index = Member<std::int32_t>(object, "object.internalIndex");
        if (index < 0) return {};
        std::uint32_t serial{};
        if (ObjectAt(static_cast<std::uint32_t>(index), serial) != object) return {};
        return {(std::uint64_t(serial) << 32) | (std::uint64_t(index) + 1), objects_.generation(objects_.user)};
    }
    Address Resolve(Handle handle) const {
        if (!handle.id || !static_cast<std::uint32_t>(handle.id) || !handle.generation ||
            handle.generation != objects_.generation(objects_.user)) return 0;
        std::uint32_t serial{};
        const auto object = ObjectAt(ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle), serial);
        return object && serial == ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) &&
            Member<std::int32_t>(object, "object.internalIndex") ==
                static_cast<std::int32_t>(ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)) ? object : 0;
    }
    Address Find(std::string_view path) const {
        Handle handle{};
        if (objects_.find_exact(objects_.user, anomaly::sdk::StringView(path), &handle).code != 0) return 0;
        return Resolve(handle);
    }
    Address Require(std::string_view path) const {
        const auto result = Find(path);
        if (!result) LocalizedStatus("error.missing", "Engine object not found: {0}", path);
        return result;
    }
    std::string Name(Address object) const { return NameValue(Member<std::array<std::uint32_t, 2>>(object, "object.name")); }
    std::string Path(Address object) const {
        std::string path = Name(object);
        auto outer = Member<Address>(object, "object.outer");
        for (unsigned depth = 0; outer && depth < 32; ++depth) {
            path = Name(outer) + "." + path; outer = Member<Address>(outer, "object.outer");
        }
        return outer ? std::string{} : path;
    }
    Address Class(Address object) const { return Member<Address>(object, "object.class"); }
    bool IsA(Address object, std::string_view wanted) const {
        if (!object) return false;
        auto type = Class(object);
        for (unsigned depth = 0; type && depth < 64; ++depth) {
            if (Name(type) == wanted) return true;
            type = Member<Address>(type, "struct.super");
        }
        return false;
    }
    Field Property(Address type, const FieldSpec& spec, bool native_bool = true) const {
        auto field = Member<Address>(type, "struct.propertyLink");
        for (unsigned count = 0; field && count < 4096; ++count) {
            const auto name = Member<std::array<std::uint32_t,2>>(field,"field.name");
            if (NameValue(name) == spec.name) {
                const auto field_class = Member<Address>(field, "field.class");
                const auto kind = NameValue(Member<std::array<std::uint32_t, 2>>(field_class, "fieldClass.name"));
                const auto offset = Member<std::int32_t>(field, "property.offset");
                const auto size = Member<std::int32_t>(field, "property.size");
                const auto kinds = "|" + std::string(spec.type) + "|";
                if (offset < 0 || offset > 1048576 || size != static_cast<std::int32_t>(spec.size) ||
                    Member<std::int32_t>(field, "property.arrayDim") != 1 ||
                    kinds.find("|" + kind + "|") == std::string::npos)
                    LocalizedStatus("error.property", "Engine field contract mismatch: {0}", spec.name);
                if (spec.structure) {
                    const auto structure = Member<Address>(field, "property.struct");
                    if (!structure || Name(structure) != spec.structure ||
                        Member<std::int32_t>(structure, "struct.size") != size)
                        LocalizedStatus("error.property", "Engine field contract mismatch: {0}", spec.name);
                }
                Field result{static_cast<std::size_t>(offset),spec.size,name};
                if (kind == "BoolProperty") {
                    result.bool_byte = Member<std::uint8_t>(field,"bool.byteOffset");
                    result.bool_mask = Member<std::uint8_t>(field,"bool.fieldMask");
                    if (result.bool_byte >= result.size || !result.bool_mask ||
                        (native_bool && (result.bool_byte != 0 || result.bool_mask != 255)))
                        LocalizedStatus("error.property", "Engine field contract mismatch: {0}", spec.name);
                }
                return result;
            }
            field = Member<Address>(field, "property.next");
        }
        LocalizedStatus("error.property", "Engine field contract mismatch: {0}", spec.name);
    }
    Address ObjectField(Address object, const char* name) const {
        return Read<Address>(object + Property(Class(object), {name, "ObjectProperty", sizeof(Address)}).offset);
    }
    Function& Bind(const std::string& path, std::initializer_list<FieldSpec> specs) {
        auto found = functions_.find(path);
        if (found != functions_.end() && Resolve(found->second.handle)) return found->second;
        const auto function = Require(path);
        if (!IsA(function, "Function")) LocalizedStatus("error.function", "Engine function contract mismatch: {0}", path);
        Function bound{Pin(function), Member<std::uint16_t>(function, "function.size"), {}};
        if (!bound.handle.id || bound.size > 1024 ||
            Member<std::uint8_t>(function, "function.count") != specs.size() ||
            !(Member<std::uint32_t>(function, "function.flags") & Offset("function.nativeFlag")))
            LocalizedStatus("error.function", "Engine function contract mismatch: {0}", path);
        std::size_t end{};
        std::uint16_t returned = 0xffff;
        for (const auto& spec : specs) {
            const auto field = Property(function, spec);
            if (field.offset > bound.size || field.size > bound.size - field.offset)
                LocalizedStatus("error.function", "Engine function contract mismatch: {0}", path);
            for (const auto& [unused, previous] : bound.fields) {
                static_cast<void>(unused);
                if (field.offset < previous.offset + previous.size && previous.offset < field.offset + field.size)
                    LocalizedStatus("error.function", "Engine function contract mismatch: {0}", path);
            }
            bound.fields.emplace(spec.name, field);
            end = std::max(end, field.offset + field.size);
            if (std::string_view(spec.name) == "ReturnValue") returned = static_cast<std::uint16_t>(field.offset);
        }
        if (end != bound.size || returned != Member<std::uint16_t>(function, "function.return"))
            LocalizedStatus("error.function", "Engine function contract mismatch: {0}", path);
        return functions_.insert_or_assign(path, std::move(bound)).first->second;
    }
    template<class Fill> Parameters Call(const std::string& path, Address receiver, Fill fill) {
        auto& function = functions_.at(path);
        Parameters parameters(function, l10n_); fill(parameters);
        const auto target = Resolve(function.handle);
        if (faulted_ || !framework_.is_game_thread(framework_.user) || !Resolve(Pin(receiver)) || !target)
            LocalizedStatus("error.call", "Engine call is unavailable: {0}", path);
        if (!InvokeNative(entry_, receiver, target, function.size ? parameters.bytes.data() : nullptr)) {
            faulted_ = true;
            LocalizedStatus("error.call", "Engine call is unavailable: {0}", path);
        }
        return parameters;
    }
    Parameters Call(const std::string& path, Address receiver) { return Call(path, receiver, [](Parameters&) {}); }
    template<class T> void Set(Address object, const char* method, const char* argument, T value) {
        Call(std::string("/Script/Engine.") + method, object, [&](Parameters& p) { p.Put(argument, value); });
    }
    void Preflight() {
        Bind("/Script/Engine.KismetSystemLibrary:MakeSoftObjectPath",{{"PathString","StrProperty",16},{"ReturnValue","StructProperty",sizeof(SoftAssetPath),"SoftObjectPath"}});
        Bind("/Script/Engine.KismetSystemLibrary:LoadAsset",{{"WorldContextObject","ObjectProperty",8},{"Asset","SoftObjectProperty",sizeof(SoftAssetReference)},
            {"OnLoaded","DelegateProperty",16},{"LatentInfo","StructProperty",sizeof(LatentAction),"LatentActionInfo"}});
        Bind("/Script/Engine.Actor:K2_DestroyActor", {});
        Bind("/Script/Engine.Actor:IsActorBeingDestroyed", {{"ReturnValue", "BoolProperty", 1}});
        Bind("/Script/Engine.Actor:SetLifeSpan", {{"InLifespan", "FloatProperty", 4}});
        Bind("/Script/Engine.Actor:SetReplicates", {{"bInReplicates", "BoolProperty", 1}});
        Bind("/Script/Engine.Actor:SetReplicateMovement", {{"bInReplicateMovement", "BoolProperty", 1}});
        Bind("/Script/Engine.Actor:SetActorEnableCollision", {{"bNewActorEnableCollision", "BoolProperty", 1}});
        Bind("/Script/Engine.Actor:GetTransform", {{"ReturnValue", "StructProperty", sizeof(Transform), "Transform"}});
        Bind("/Script/Engine.Actor:K2_SetActorTransform", {{"NewTransform", "StructProperty", sizeof(Transform), "Transform"},
            {"bSweep", "BoolProperty", 1}, {"SweepHitResult", "StructProperty", 264, "HitResult"},
            {"bTeleport", "BoolProperty", 1}, {"ReturnValue", "BoolProperty", 1}});
        Bind("/Script/Engine.Actor:GetActorBounds", {{"bOnlyCollidingComponents", "BoolProperty", 1},
            {"Origin", "StructProperty", sizeof(Vector), "Vector"}, {"BoxExtent", "StructProperty", sizeof(Vector), "Vector"},
            {"bIncludeFromChildActors", "BoolProperty", 1}});
        Bind("/Script/Engine.SceneComponent:SetMobility", {{"NewMobility", "EnumProperty|ByteProperty", 1}});
        Bind("/Script/Engine.PrimitiveComponent:SetSimulatePhysics", {{"bSimulate", "BoolProperty", 1}});
        Bind("/Script/Engine.PrimitiveComponent:SetCollisionEnabled", {{"NewType", "EnumProperty|ByteProperty", 1}});
        Bind("/Script/Engine.PrimitiveComponent:SetCollisionResponseToAllChannels", {{"NewResponse", "EnumProperty|ByteProperty", 1}});
        Bind("/Script/Engine.StaticMeshComponent:SetStaticMesh", {{"NewMesh", "ObjectProperty", 8}, {"ReturnValue", "BoolProperty", 1}});
        Bind("/Script/Engine.StaticMeshComponent:GetLocalBounds", {{"Min", "StructProperty", sizeof(Vector), "Vector"},
            {"Max", "StructProperty", sizeof(Vector), "Vector"}});
        Bind("/Script/Engine.SkeletalMeshComponent:SetSkeletalMeshAsset", {{"NewMesh", "ObjectProperty", 8}});
        Bind("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass", {{"WorldContextObject", "ObjectProperty", 8},
            {"ActorClass", "ClassProperty", 8}, {"SpawnTransform", "StructProperty", sizeof(Transform), "Transform"},
            {"CollisionHandlingOverride", "EnumProperty|ByteProperty", 1}, {"Owner", "ObjectProperty", 8},
            {"TransformScaleMethod", "EnumProperty|ByteProperty", 1}, {"ReturnValue", "ObjectProperty", 8}});
        Bind("/Script/Engine.GameplayStatics:FinishSpawningActor", {{"Actor", "ObjectProperty", 8},
            {"SpawnTransform", "StructProperty", sizeof(Transform), "Transform"},
            {"TransformScaleMethod", "EnumProperty|ByteProperty", 1}, {"ReturnValue", "ObjectProperty", 8}});
        Bind("/Script/Engine.PlayerController:DeprojectScreenPositionToWorld",{{"ScreenX","FloatProperty",4},{"ScreenY","FloatProperty",4},
            {"WorldLocation","StructProperty",sizeof(Vector),"Vector"},{"WorldDirection","StructProperty",sizeof(Vector),"Vector"},{"ReturnValue","BoolProperty",1}});
        Bind("/Script/Engine.KismetSystemLibrary:LineTraceSingle",{{"WorldContextObject","ObjectProperty",8},
            {"Start","StructProperty",sizeof(Vector),"Vector"},{"End","StructProperty",sizeof(Vector),"Vector"},
            {"TraceChannel","EnumProperty|ByteProperty",1},{"bTraceComplex","BoolProperty",1},{"ActorsToIgnore","ArrayProperty",16},
            {"DrawDebugType","EnumProperty|ByteProperty",1},{"OutHit","StructProperty",264,"HitResult"},{"bIgnoreSelf","BoolProperty",1},
            {"TraceColor","StructProperty",16,"LinearColor"},{"TraceHitColor","StructProperty",16,"LinearColor"},
            {"DrawTime","FloatProperty",4},{"ReturnValue","BoolProperty",1}});
        // Validate reflected actor/component ownership before any allocation.
        Property(Require("/Script/Engine.StaticMeshActor"), {"StaticMeshComponent", "ObjectProperty", 8});
        Property(Require("/Script/Engine.SkeletalMeshActor"), {"SkeletalMeshComponent", "ObjectProperty", 8});
    }
    void RequestMesh(std::string_view path,Handle owner,std::int32_t uuid) {
        const auto wide = NativeWide(path);
        if (wide.empty()) LocalizedStatus("error.asset_encoding","The asset path has invalid text encoding");
        const auto receiver = Require("/Script/Engine.Default__KismetSystemLibrary");
        const auto soft = Call("/Script/Engine.KismetSystemLibrary:MakeSoftObjectPath",receiver,[&](Parameters& p) {
            const auto count = static_cast<std::int32_t>(wide.size()+1);
            p.Put("PathString",NativeArray{reinterpret_cast<Address>(wide.c_str()),count,count});
        }).Get<SoftAssetPath>("ReturnValue");
        if (soft.subpath.data) {
            FreeNativeMemory(profile_.Address("ue5.FMemory.Free"),soft.subpath.data);
            LocalizedStatus("error.mesh","The engine did not accept this mesh");
        }
        // Engine-owned latent action; its callback target is our live actor.
        // No delegate or function pointer back into this DLL is retained.
        Call("/Script/Engine.KismetSystemLibrary:LoadAsset",receiver,[&](Parameters& p) {
            p.Put("WorldContextObject",World()); p.Put("Asset",SoftAssetReference{0,soft});
            p.Put("OnLoaded",std::array<std::uint8_t,16>{});
            LatentAction latent; latent.uuid = uuid; latent.target = Resolve(owner); p.Put("LatentInfo",latent);
        });
    }
    bool MeshLoaded(std::string_view path) const { return Find(path) != 0; }
    RegistryBinding PrepareRegistry() {
        Bind("/Script/AssetRegistry.AssetRegistryHelpers:GetAssetRegistry",{{"ReturnValue","InterfaceProperty",16}});
        struct Interface { Address object{},pointer{}; };
        const auto result = Call("/Script/AssetRegistry.AssetRegistryHelpers:GetAssetRegistry",Require("/Script/AssetRegistry.Default__AssetRegistryHelpers")).Get<Interface>("ReturnValue");
        if (!result.pointer)
            LocalizedStatus("error.catalog_api","The asset registry is unavailable");
        RegistryBinding binding; binding.registry = result.pointer;
        binding.query = Read<Address>(Read<Address>(result.pointer)+Offset("registry.queryVtableOffset"));
        if (!profile_.MatchesEntry(&core_,"ue5.AssetRegistry.Query.entry",binding.query,true))
            LocalizedStatus("error.catalog_api","The asset registry is unavailable");
        const auto class_path = [&](const char* path) {
            const auto type = Require(path);
            return NativeAssetPath{Member<NativeName>(Member<Address>(type,"object.outer"),"object.name"),Member<NativeName>(type,"object.name")};
        };
        binding.static_mesh = class_path("/Script/Engine.StaticMesh"); binding.skeletal_mesh = class_path("/Script/Engine.SkeletalMesh");
        const auto data = Require("/Script/CoreUObject.AssetData");
        binding.stride = Offset("assetData.size"); binding.package_offset = Offset("assetData.package"); binding.name_offset = Offset("assetData.name");
        if (Member<std::int32_t>(data,"struct.size") != binding.stride ||
            Property(data,{"PackageName","NameProperty",8}).offset != binding.package_offset ||
            Property(data,{"AssetName","NameProperty",8}).offset != binding.name_offset ||
            binding.package_offset+8 > binding.stride || binding.name_offset+8 > binding.stride)
            LocalizedStatus("error.catalog_api","The asset registry is unavailable");
        return binding;
    }
    // Worker only. Native registry synchronization and allocations stay inside
    // the engine. Copy names, release every asset-data record, then decode them.
    std::vector<Asset> RegistryAssets(const RegistryBinding& binding,const std::atomic<bool>& stop) const {
        std::vector<Asset> result;
        for (const bool skeletal : {false,true}) {
            NativeArray records;
            const auto ok = QueryAssetRecords(binding.query,binding.registry,skeletal ? binding.skeletal_mesh : binding.static_mesh,records);
            struct Release {
                Address destroy,free_memory; NativeArray& data;
                ~Release() { ReleaseAssetRecords(destroy,free_memory,data); }
            } release{profile_.Address("ue5.AssetData.DestroyRange"),profile_.Address("ue5.FMemory.Free"),records};
            if (!ok || records.count < 0 || records.capacity < records.count || records.capacity > 1000000 ||
                (records.count && !records.data)) LocalizedStatus("error.catalog_api","The asset registry is unavailable");
            std::vector<NativeAssetPath> paths; paths.reserve(records.count);
            std::vector<std::uint8_t> block(binding.stride*512);
            for (std::int32_t at = 0; at < records.count && !stop.load(); at += 512) {
                const auto count = std::min(512,records.count-at); const auto size = binding.stride*count;
                if (core_.read_memory(core_.user,records.data+binding.stride*at,{block.data(),size}).code != 0)
                    LocalizedStatus("error.catalog_api","The asset registry is unavailable");
                for (int i = 0; i < count; ++i) {
                    NativeAssetPath path;
                    std::memcpy(&path.package,block.data()+i*binding.stride+binding.package_offset,8);
                    std::memcpy(&path.asset,block.data()+i*binding.stride+binding.name_offset,8); paths.push_back(path);
                }
            }
            ReleaseAssetRecords(release.destroy,release.free_memory,records); records = {};
            for (const auto& path : paths) {
                if (stop.load()) return {};
                auto text = NameValue(path.package)+"."+NameValue(path.asset);
                if (AssetPath(text)) result.push_back({std::move(text),skeletal,{}});
            }
        }
        return result;
    }
    void PreviewPreflight() {
        if (!core_.write_memory || !profile_.Address("ue5.Texture.GetResource") || !profile_.Address("ue5.SceneCapture.Deferred"))
            LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
        Bind("/Script/Engine.GameplayStatics:SpawnObject",{{"ObjectClass","ClassProperty",8},{"Outer","ObjectProperty",8},{"ReturnValue","ObjectProperty",8}});
        Bind("/Script/Engine.KismetRenderingLibrary:ResizeRenderTarget2D",{{"TextureRenderTarget","ObjectProperty",8},{"Width","IntProperty",4},{"Height","IntProperty",4}});
        Bind("/Script/Engine.SceneCaptureComponent:ClearShowOnlyComponents",{});
        Bind("/Script/Engine.SceneCaptureComponent:ShowOnlyActorComponents",{{"InActor","ObjectProperty",8},{"bIncludeFromChildActors","BoolProperty",1}});
        Bind("/Script/Engine.SceneCaptureComponent:SetShowFlagSettings",{{"InShowFlagSettings","ArrayProperty",16}});
        Bind("/Script/Engine.PrimitiveComponent:SetVisibleInSceneCaptureOnly",{{"bValue","BoolProperty",1}});
        Bind("/Script/Engine.KismetSystemLibrary:SetObjectPropertyByName",{{"Object","ObjectProperty",8},{"PropertyName","NameProperty",8},{"Value","ObjectProperty",8}});
        Bind("/Script/Engine.KismetSystemLibrary:SetBoolPropertyByName",{{"Object","ObjectProperty",8},{"PropertyName","NameProperty",8},{"Value","BoolProperty",1}});
        const auto flags = Require("/Script/Engine.EngineShowFlagsSetting");
        if (Member<std::int32_t>(flags,"struct.size") != sizeof(ShowFlagSetting) ||
            Property(flags,{"ShowFlagName","StrProperty",sizeof(NativeArray)}).offset != offsetof(ShowFlagSetting,name) ||
            Property(flags,{"Enabled","BoolProperty",sizeof(bool)}).offset != offsetof(ShowFlagSetting,enabled))
            LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
    }
    void ConfigureCapture(Address component) {
        SetNamed(component,"bCaptureEveryFrame","SetBoolPropertyByName",false);
        SetNamed(component,"bCaptureOnMovement","SetBoolPropertyByName",false);
        // One-shot captures still need a view state for their own exposure.
        // Persisting it does not enable continuous capture.
        SetNamed(component,"bAlwaysPersistRenderingState","SetBoolPropertyByName",true);
        SetNamed(component,"bUseRayTracingIfEnabled","SetBoolPropertyByName",false);
        WriteProperty(component,{"PrimitiveRenderMode","EnumProperty|ByteProperty",1},std::uint8_t{2});
        // Unlit capture sends a lit material's base color to emissive, while
        // retaining the game's emissive materials in the normal color pass.
        WriteProperty(component,{"CaptureSource","EnumProperty|ByteProperty",1},CaptureSource::FinalColorLDR);
        WriteProperty(component,{"UnlitViewmode","EnumProperty|ByteProperty",1},CaptureUnlitViewmode::Capture);
        WriteProperty(component,{"FOVAngle","FloatProperty",4},45.0f);
        WriteProperty(component,{"LODDistanceFactor","FloatProperty",4},3.0f);
        ConfigureExposure(component);
        // Engine show-flag identifiers are runtime data, not UI text. Only the
        // capture's view is affected; the world's rendering stays unchanged.
        const std::wstring_view disabled[]{L"Fog",L"Atmosphere",L"VolumetricFog",L"VolumetricCloud",
            L"Lighting",L"DirectLighting",L"DynamicShadows",L"AmbientOcclusion",L"GlobalIllumination",L"LumenGlobalIllumination",
            L"LumenReflections",L"ScreenSpaceReflections",L"ReflectionEnvironment",L"PathTracing",
            L"MotionBlur",L"TemporalAA",L"DepthOfField",L"Bloom",L"ScreenPercentage",L"LocalExposure",
            L"ColorGrading",L"PostProcessMaterial"};
        const std::wstring_view enabled[]{L"PostProcessing",L"EyeAdaptation",L"Tonemapper"};
        std::array<ShowFlagSetting,std::size(disabled)+std::size(enabled)> flags{};
        for (std::size_t i = 0; i < flags.size(); ++i) {
            const bool on = i >= std::size(disabled);
            const auto name = on ? enabled[i-std::size(disabled)] : disabled[i];
            const auto count = static_cast<std::int32_t>(name.size()+1);
            flags[i].name = {reinterpret_cast<Address>(name.data()),count,count};
            flags[i].enabled = on;
        }
        Call("/Script/Engine.SceneCaptureComponent:SetShowFlagSettings",component,[&](Parameters& p) {
            p.Put("InShowFlagSettings",NativeArray{reinterpret_cast<Address>(flags.data()),
                static_cast<std::int32_t>(flags.size()),static_cast<std::int32_t>(flags.size())});
        });
    }
    void ConfigureExposure(Address component) {
        const auto type = Require("/Script/Engine.PostProcessSettings");
        const auto size = Member<std::int32_t>(type,"struct.size");
        if (size <= 0 || size > 65536) LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
        const auto container = Property(Class(component),{"PostProcessSettings","StructProperty",static_cast<std::size_t>(size),"PostProcessSettings"});
        if (container.offset+size > Member<std::int32_t>(Class(component),"struct.size"))
            LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
        const auto write = [&]<class T>(const FieldSpec& spec,const T& value) {
            const auto field = Property(type,spec,false);
            if (field.offset+field.size > static_cast<std::size_t>(size))
                LocalizedStatus("error.property","Engine field contract mismatch: {0}",spec.name);
            auto address = component+container.offset+field.offset;
            if constexpr (std::is_same_v<T,bool>) {
                address += field.bool_byte;
                const auto bits = static_cast<std::uint8_t>((Read<std::uint8_t>(address)&~field.bool_mask)|(value ? field.bool_mask : 0));
                if (core_.write_memory(core_.user,address,{&bits,1}).code != 0)
                    LocalizedStatus("error.property","Engine field contract mismatch: {0}",spec.name);
            } else if (field.size != sizeof(value) ||
                core_.write_memory(core_.user,address,{reinterpret_cast<const std::uint8_t*>(&value),sizeof(value)}).code != 0)
                LocalizedStatus("error.property","Engine field contract mismatch: {0}",spec.name);
        };
        // A fixed studio exposure must override the world's night/day volumes.
        write({"bOverride_AutoExposureMethod","BoolProperty",1},true);
        write({"AutoExposureMethod","EnumProperty|ByteProperty",1},std::uint8_t{2});
        write({"bOverride_AutoExposureApplyPhysicalCameraExposure","BoolProperty",1},true);
        write({"AutoExposureApplyPhysicalCameraExposure","BoolProperty",1},false);
        write({"bOverride_AutoExposureBias","BoolProperty",1},true);
        write({"AutoExposureBias","FloatProperty",4},1.0f);
        write({"bOverride_AutoExposureBiasCurve","BoolProperty",1},true);
        write({"AutoExposureBiasCurve","ObjectProperty",8},Address{});
        write({"bOverride_SceneColorTint","BoolProperty",1},true);
        write({"SceneColorTint","StructProperty",16,"LinearColor"},std::array<float,4>{1,1,1,1});
        write({"bOverride_VignetteIntensity","BoolProperty",1},true);
        write({"VignetteIntensity","FloatProperty",4},0.0f);
        WriteProperty(component,{"PostProcessBlendWeight","FloatProperty",4},1.0f);
    }
    void CaptureDeferred(Handle component) {
        const auto object = Resolve(component);
        if (faulted_ || !framework_.is_game_thread(framework_.user) || !IsA(object,"SceneCaptureComponent2D") ||
            !InvokeDeferredCapture(profile_.Address("ue5.SceneCapture.Deferred"),object)) {
            faulted_ = true;
            LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
        }
    }
    void PreviewOnly(const LiveItem& model) {
        const auto actor = Resolve(model.actor);
        const auto body = ObjectField(actor,IsA(actor,"SkeletalMeshActor") ? "SkeletalMeshComponent" : "StaticMeshComponent");
        Set(body,"PrimitiveComponent:SetVisibleInSceneCaptureOnly","bValue",true);
    }
    LiveItem SpawnAuxiliary(const char* class_path,const Pose& pose) {
        const auto statics = Require("/Script/Engine.Default__GameplayStatics");
        const auto transform = NativeTransform(pose);
        const auto actor = Call("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass",statics,[&](Parameters& p) {
            p.Put("WorldContextObject",World()); p.Put("ActorClass",Require(class_path)); p.Put("SpawnTransform",transform);
            p.Put("CollisionHandlingOverride",std::uint8_t{1}); p.Put("Owner",Address{}); p.Put("TransformScaleMethod",std::uint8_t{0});
        }).Get<Address>("ReturnValue");
        if (!actor) LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
        try {
            Set(actor,"Actor:SetLifeSpan","InLifespan",LeaseSeconds);
            Set(actor,"Actor:SetReplicates","bInReplicates",false);
            Set(actor,"Actor:SetReplicateMovement","bInReplicateMovement",false);
            Set(actor,"Actor:SetActorEnableCollision","bNewActorEnableCollision",false);
            Set(ObjectField(actor,"RootComponent"),"SceneComponent:SetMobility","NewMobility",std::uint8_t{2});
            if (Call("/Script/Engine.GameplayStatics:FinishSpawningActor",statics,[&](Parameters& p) {
                p.Put("Actor",actor); p.Put("SpawnTransform",transform); p.Put("TransformScaleMethod",std::uint8_t{0});
            }).Get<Address>("ReturnValue") != actor) LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
            Set(actor,"Actor:SetLifeSpan","InLifespan",LeaseSeconds);
            return {0,Item{{},pose,false},Pin(actor)};
        } catch (...) { try { Call("/Script/Engine.Actor:K2_DestroyActor",actor); } catch (...) {} throw; }
    }
    template<class T> void SetNamed(Address object,const char* property,const char* method,const T& value) {
        const auto field = Property(Class(object),{property,std::is_same_v<T,bool> ? "BoolProperty" : "ObjectProperty",sizeof(T)},false);
        Call(std::string("/Script/Engine.KismetSystemLibrary:")+method,Require("/Script/Engine.Default__KismetSystemLibrary"),[&](Parameters& p) {
            p.Put("Object",object); p.Put("PropertyName",field.name); p.Put("Value",value);
        });
    }
    template<class T> void WriteProperty(Address object,const FieldSpec& spec,const T& value) {
        const auto type = Class(object); const auto field = Property(type,spec);
        const auto size = Member<std::int32_t>(type,"struct.size");
        if (field.size != sizeof(value) || size <= 0 || field.offset+sizeof(value) > static_cast<std::size_t>(size) ||
            core_.write_memory(core_.user,object+field.offset,{reinterpret_cast<const std::uint8_t*>(&value),sizeof(value)}).code != 0)
            LocalizedStatus("error.property","Engine field contract mismatch: {0}",spec.name);
    }
    Handle CreateSharedTarget(Address capture_component,std::uint32_t size) {
        const auto target = Call("/Script/Engine.GameplayStatics:SpawnObject",Require("/Script/Engine.Default__GameplayStatics"),[&](Parameters& p) {
            p.Put("ObjectClass",Require("/Script/Engine.TextureRenderTarget2D")); p.Put("Outer",World());
        }).Get<Address>("ReturnValue");
        if (!IsA(target,"TextureRenderTarget2D")) LocalizedStatus("error.preview_api","The native model preview interfaces are unavailable");
        // Set sharing before the first RHI allocation. Resize enqueues creation;
        // there is no ReadPixels, FlushRenderingCommands or CPU readback here.
        SetNamed(target,"bGPUSharedFlag","SetBoolPropertyByName",true);
        SetNamed(target,"bAutoGenerateMips","SetBoolPropertyByName",false);
        WriteProperty(target,{"RenderTargetFormat","EnumProperty|ByteProperty",1},std::uint8_t{2});
        WriteProperty(target,{"TargetGamma","FloatProperty",4},2.2f);
        WriteProperty(target,{"ClearColor","StructProperty",16,"LinearColor"},std::array<float,4>{0.04f,0.045f,0.055f,1});
        SetNamed(capture_component,"TextureTarget","SetObjectPropertyByName",target);
        Call("/Script/Engine.KismetRenderingLibrary:ResizeRenderTarget2D",Require("/Script/Engine.Default__KismetRenderingLibrary"),[&](Parameters& p) {
            p.Put("TextureRenderTarget",target); p.Put("Width",static_cast<std::int32_t>(size)); p.Put("Height",static_cast<std::int32_t>(size));
        });
        return Pin(target);
    }
    // Render-domain access to this plugin's pinned capture target. Both virtual
    // getters are validated at their controlled entries; only the native D3D12
    // resource is exported to a temporary shared NT handle, never to the SDK.
    Address ShareTarget(Handle target,std::uint32_t& pending_stage) const {
        pending_stage = 0;
        const auto texture = Resolve(target);
        if (!texture || !IsA(texture,"TextureRenderTarget2D")) return 0;
        pending_stage = 1; Address resource{};
        if (!InvokeAccessor(profile_.Address("ue5.Texture.GetResource"),texture,&resource))
            LocalizedStatus("error.preview_gpu","The model preview GPU resource is unavailable");
        if (!resource) return 0;
        const auto base = resource+Offset("renderTarget.baseOffset");
        const auto getter = Read<Address>(Read<Address>(base)+Offset("renderTarget.rhiRefVtableOffset"));
        if (!profile_.MatchesEntry(&core_,"ue5.RenderTarget.RhiRef.entry",getter,true))
            LocalizedStatus("error.preview_gpu","The model preview GPU resource is unavailable");
        Address reference{};
        if (!InvokeAccessor(getter,base,&reference) || !reference)
            LocalizedStatus("error.preview_gpu","The model preview GPU resource is unavailable");
        pending_stage = 2; const auto rhi = Read<Address>(reference);
        if (!rhi) return 0;
        const auto native_getter = Read<Address>(Read<Address>(rhi)+Offset("rhiTexture.nativeResourceVtableOffset"));
        if (!profile_.MatchesEntry(&core_,"ue5.RHITexture.NativeResource.entry",native_getter,true))
            LocalizedStatus("error.preview_gpu","The model preview GPU resource is unavailable");
        pending_stage = 3; Address native{},shared{};
        if (!InvokeAccessor(native_getter,rhi,&native)) LocalizedStatus("error.preview_gpu","The model preview GPU resource is unavailable");
        if (!native) return 0;
        if (!ExportSharedNative(native,&shared)) LocalizedStatus("error.preview_gpu","The model preview GPU resource is unavailable");
        return shared;
    }
    Address World() const { return Read<Address>(world_slot_); }
    std::pair<Vector,Vector> DropPoint(Handle controller,Handle pawn,editor::Point screen,double distance) {
        const auto ray = Call("/Script/Engine.PlayerController:DeprojectScreenPositionToWorld",Resolve(controller),[&](Parameters& p) {
            p.Put("ScreenX",screen[0]); p.Put("ScreenY",screen[1]);
        });
        if (!ray.Get<bool>("ReturnValue")) LocalizedStatus("error.drop","The cursor could not be projected into the current world");
        const auto start = ray.Get<Vector>("WorldLocation"), direction = ray.Get<Vector>("WorldDirection");
        const auto end = editor::geometry::Add(start,editor::geometry::Scale(direction,100000));
        std::array<Address,1> ignored{Resolve(pawn)};
        const auto trace = Call("/Script/Engine.KismetSystemLibrary:LineTraceSingle",Require("/Script/Engine.Default__KismetSystemLibrary"),[&](Parameters& p) {
            p.Put("WorldContextObject",World()); p.Put("Start",start); p.Put("End",end); p.Put("TraceChannel",std::uint8_t{0});
            p.Put("bTraceComplex",true); p.Put("ActorsToIgnore",NativeArray{reinterpret_cast<Address>(ignored.data()),ignored[0] ? 1 : 0,1});
            p.Put("DrawDebugType",std::uint8_t{0}); p.Put("bIgnoreSelf",true);
        });
        if (!trace.Get<bool>("ReturnValue")) return {editor::geometry::Add(start,editor::geometry::Scale(direction,distance)),Vector{0,0,1}};
        const auto hit = trace.Get<std::array<std::uint8_t,264>>("OutHit");
        const auto structure = Require("/Script/Engine.HitResult");
        const auto location = Property(structure,{"ImpactPoint","StructProperty",sizeof(Vector),"Vector_NetQuantize"});
        const auto normal = Property(structure,{"ImpactNormal","StructProperty",sizeof(Vector),"Vector_NetQuantizeNormal"});
        if (location.offset+sizeof(Vector) > hit.size() || normal.offset+sizeof(Vector) > hit.size())
            LocalizedStatus("error.drop","The cursor could not be projected into the current world");
        Vector point{},up{}; std::memcpy(point.data(),hit.data()+location.offset,sizeof(Vector)); std::memcpy(up.data(),hit.data()+normal.offset,sizeof(Vector));
        if (!Finite(point) || !Finite(up)) LocalizedStatus("error.drop","The cursor could not be projected into the current world");
        return {point,up};
    }
    Address LoadedMesh(std::string_view path) {
        if (!AssetPath(path)) LocalizedStatus("error.path", "Use a complete /Game or /Engine mesh asset path");
        const auto loaded = Find(path);
        if (!loaded) LocalizedStatus("error.unloaded", "This model is not loaded. Visit its area and refresh the catalog before placing it.");
        return Mesh(loaded);
    }
    LiveItem Spawn(const Item& item, std::uint64_t id, bool preview) {
        if (!item.pose.Valid()) LocalizedStatus("error.pose", "Position, angles and positive scale must be valid numbers");
        Preflight();
        const auto mesh = LoadedMesh(item.asset);
        const bool skeletal = IsA(mesh, "SkeletalMesh");
        const bool has_collision_data = ObjectField(mesh, skeletal ? "PhysicsAsset" : "BodySetup") != 0;
        const auto type = Require(skeletal ? "/Script/Engine.SkeletalMeshActor" : "/Script/Engine.StaticMeshActor");
        const auto statics = Require("/Script/Engine.Default__GameplayStatics");
        const auto world = World();
        if (!world || !IsA(world, "World")) LocalizedStatus("error.world", "The current world is unavailable");
        const auto transform = NativeTransform(item.pose);
        const auto actor = Call("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass", statics,
            [&](Parameters& p) {
                p.Put("WorldContextObject", world); p.Put("ActorClass", type); p.Put("SpawnTransform", transform);
                p.Put("CollisionHandlingOverride", std::uint8_t{1}); p.Put("Owner", Address{});
                p.Put("TransformScaleMethod", std::uint8_t{0});
            }).Get<Address>("ReturnValue");
        if (!actor) LocalizedStatus("error.spawn", "The engine could not create this object");
        try {
            // Engine-owned expiry survives plugin stop. No delegate or callback
            // into this DLL is ever installed on a generated actor.
            Set(actor, "Actor:SetLifeSpan", "InLifespan", LeaseSeconds);
            Set(actor, "Actor:SetReplicates", "bInReplicates", false);
            Set(actor, "Actor:SetReplicateMovement", "bInReplicateMovement", false);
            const auto body = ObjectField(actor, skeletal ? "SkeletalMeshComponent" : "StaticMeshComponent");
            if (!IsA(body, skeletal ? "SkeletalMeshComponent" : "StaticMeshComponent"))
                LocalizedStatus("error.mesh", "The engine did not accept this mesh");
            Set(body, "SceneComponent:SetMobility", "NewMobility", std::uint8_t{2});
            if (skeletal) Set(body, "SkeletalMeshComponent:SetSkeletalMeshAsset", "NewMesh", mesh);
            else if (!Call("/Script/Engine.StaticMeshComponent:SetStaticMesh", body,
                [&](Parameters& p) { p.Put("NewMesh", mesh); }).Get<bool>("ReturnValue"))
                LocalizedStatus("error.mesh", "The engine did not accept this mesh");
            Set(body, "PrimitiveComponent:SetSimulatePhysics", "bSimulate", false);
            Set(body, "PrimitiveComponent:SetCollisionResponseToAllChannels", "NewResponse", std::uint8_t{2});
            Collision(actor, body, false);
            if (Call("/Script/Engine.GameplayStatics:FinishSpawningActor", statics,
                [&](Parameters& p) { p.Put("Actor", actor); p.Put("SpawnTransform", transform);
                    p.Put("TransformScaleMethod", std::uint8_t{0}); }).Get<Address>("ReturnValue") != actor)
                LocalizedStatus("error.spawn", "The engine could not create this object");
            LiveItem record{id, item, Pin(actor)};
            if (!record.actor.id) LocalizedStatus("error.spawn", "The engine could not create this object");
            record.has_collision_data = has_collision_data;
            Set(actor, "Actor:SetLifeSpan", "InLifespan", LeaseSeconds);
            Move(record, item.pose);
            Collision(actor, body, item.collision && !preview);
            return record;
        } catch (...) {
            try { Call("/Script/Engine.Actor:K2_DestroyActor", actor); } catch (...) {}
            throw;
        }
    }
    bool Alive(const LiveItem& item) {
        const auto actor = Resolve(item.actor);
        return actor && !Call("/Script/Engine.Actor:IsActorBeingDestroyed", actor).Get<bool>("ReturnValue");
    }
    void Destroy(const LiveItem& item) {
        if (const auto actor = Resolve(item.actor); actor && Alive(item)) Call("/Script/Engine.Actor:K2_DestroyActor", actor);
    }
    void Renew(const LiveItem& item) {
        Set(Resolve(item.actor), "Actor:SetLifeSpan", "InLifespan", LeaseSeconds);
    }
    void Move(LiveItem& item, const Pose& pose,bool update_bounds = true) {
        if (!pose.Valid()) LocalizedStatus("error.pose", "Position, angles and positive scale must be valid numbers");
        const auto actor = Resolve(item.actor);
        if (!actor) LocalizedStatus("error.stale", "This placed object no longer exists");
        const auto requested = NativeTransform(pose);
        if (!Call("/Script/Engine.Actor:K2_SetActorTransform", actor, [&](Parameters& p) {
            p.Put("NewTransform", requested); p.Put("bSweep", false); p.Put("bTeleport", true);
        }).Get<bool>("ReturnValue")) LocalizedStatus("error.move", "The engine rejected this transform");
        const auto actual = Call("/Script/Engine.Actor:GetTransform", actor).Get<Transform>("ReturnValue");
        double quaternion_dot{};
        for (std::size_t i = 0; i < 4; ++i) quaternion_dot += requested.rotation[i] * actual.rotation[i];
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(actual.translation[axis]) || !std::isfinite(actual.scale[axis]) ||
                std::abs(actual.translation[axis] - requested.translation[axis]) > 0.1 ||
                std::abs(actual.scale[axis] - requested.scale[axis]) > 0.001)
                LocalizedStatus("error.move", "The engine rejected this transform");
        if (!std::isfinite(quaternion_dot) || std::abs(std::abs(quaternion_dot) - 1) > 0.0001)
            LocalizedStatus("error.move", "The engine rejected this transform");
        item.item.pose = pose;
        if (!update_bounds) return;
        const auto bounds = Call("/Script/Engine.Actor:GetActorBounds", actor, [&](Parameters& p) {
            p.Put("bOnlyCollidingComponents", false); p.Put("bIncludeFromChildActors", false);
        });
        item.center = bounds.Get<Vector>("Origin"); item.extent = bounds.Get<Vector>("BoxExtent");
        if (IsA(actor,"StaticMeshActor")) {
            const auto body = ObjectField(actor,"StaticMeshComponent");
            const auto local = Call("/Script/Engine.StaticMeshComponent:GetLocalBounds",body);
            const auto minimum = local.Get<Vector>("Min"), maximum = local.Get<Vector>("Max");
            for (std::size_t i = 0; i < 8; ++i) {
                Vector point{};
                for (std::size_t axis = 0; axis < 3; ++axis) point[axis] = (i & (std::size_t{1} << axis)) ? maximum[axis] : minimum[axis];
                item.corners[i] = editor::TransformPoint(actual,point);
            }
        } else for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t axis = 0; axis < 3; ++axis)
                item.corners[i][axis] = item.center[axis] + ((i & (std::size_t{1} << axis)) ? 1 : -1)*item.extent[axis];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            double low = item.corners[0][axis], high = low;
            for (const auto& point : item.corners) { low = std::min(low,point[axis]); high = std::max(high,point[axis]); }
            item.center[axis] = (low+high)/2; item.extent[axis] = (high-low)/2;
        }
    }
    void SetCollision(LiveItem& item, bool enabled) {
        const auto actor = Resolve(item.actor);
        if (!actor) LocalizedStatus("error.stale", "This placed object no longer exists");
        const auto body = ObjectField(actor, IsA(actor, "SkeletalMeshActor") ? "SkeletalMeshComponent" : "StaticMeshComponent");
        Collision(actor, body, enabled); item.item.collision = enabled;
    }
    bool Faulted() const { return faulted_; }
private:
    [[noreturn]] void LocalizedStatus(const char* key, const char* fallback) const { throw std::runtime_error(l10n_.Text(key, fallback)); }
    [[noreturn]] void LocalizedStatus(const char* key, const char* fallback, std::string_view detail) const {
        const std::array args{detail}; throw std::runtime_error(l10n_.Format(key, fallback, args));
    }
    std::string NameValue(std::array<std::uint32_t, 2> value) const {
        std::array<char, 1024> bytes{}; std::size_t size = bytes.size();
        if (names_.resolve_utf8(names_.user, value[0], bytes.data(), &size).code != 0 || !size || size > bytes.size())
            LocalizedStatus("error.name", "An engine object name could not be resolved");
        std::string name(bytes.data(), size - 1);
        if (value[1]) name += "_" + std::to_string(value[1] - 1);
        return name;
    }
    Address Rip(std::string_view name) const {
        const auto address = profile_.Address(name); const auto* selected = profile_.Selected(name);
        if (!address || !selected) LocalizedStatus("error.profile", "The compatibility profile could not be loaded");
        const auto& values = selected->values;
        const auto displacement = values.at("displacementOffset"), length = values.at("instructionSize");
        if (displacement > length || length - displacement < 4 || length > 16 || values.at("subtract") > 256)
            LocalizedStatus("error.profile", "The compatibility profile could not be loaded");
        return address + length + Read<std::int32_t>(address + displacement) - values.at("subtract");
    }
    Address Mesh(Address object) const {
        if (!object || (!IsA(object, "StaticMesh") && !IsA(object, "SkeletalMesh")))
            LocalizedStatus("error.mesh", "The engine did not accept this mesh");
        return object;
    }
    void Collision(Address actor, Address body, bool enabled) {
        Set(body, "PrimitiveComponent:SetCollisionEnabled", "NewType", std::uint8_t(enabled ? 3 : 0));
        Set(actor, "Actor:SetActorEnableCollision", "bNewActorEnableCollision", enabled);
    }
    const AnomalyCoreServiceV1& core_;
    const AnomalyUe5ObjectsServiceV1& objects_;
    const AnomalyUe5NamesServiceV1& names_;
    const AnomalyUe5FrameworkServiceV1& framework_;
    const anomaly::sdk::Localizer& l10n_;
    const CatalogNames& catalog_names_;
    anomaly::sdk::ProfileSnapshot profile_;
    Address registry_{}, world_slot_{}, entry_{};
    bool faulted_{};
    std::map<std::string, Function, std::less<>> functions_;
};
}
