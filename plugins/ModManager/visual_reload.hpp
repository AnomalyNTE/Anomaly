#pragma once
#include "native_access.hpp"
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <source_location>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace mod_manager {
// A Game-thread transaction. Live renderer consumers are rebound through validated
// UE setters; unsupported reference owners keep the world-reload route.
class VisualReload {
public:
    enum class Result { Updated, Partial, NotResident, Unsupported, Unavailable, Failed };
    bool Bind(NativeAccess* native) {
        n_ = native;
        try {
            const auto& p = n_->profile;
            rename_ = p.Address("RenameResourcePackage"); load_ = p.Address("LoadResourceObject");
            acquire_ = p.Address("AcquireResourceReference"); release_ = p.Address("ReleaseResourceReference");
            flush_ = p.Address("FlushResourceLoading"); parent_ = p.Address("SetMaterialParent");
            exists_ = p.Address("ResourcePackageExists");
            for_each_class_ = p.Address("ForEachObjectOfClass");
            for_each_package_ = p.Address("ForEachObjectWithPackage");
            array_inner_ = p.Value("ResourceArrayInner", 4096, 8);
            array_flags_ = p.Value("ResourceArrayFlags", 4096);
            return available_ = rename_ && load_ && acquire_ && release_ && flush_ && parent_ &&
                exists_ && for_each_class_ && for_each_package_ &&
                lMaxHeader() <= 128;
        } catch (...) { return available_ = false; }
    }
    bool Available() const { return available_; }
    const std::string& Detail() const { return detail_; }
    // Worker-side observation only: a blocked native Game-thread call cannot be
    // cancelled safely. Record its phase without touching UE from the worker.
    void ReportStall() {
        const auto progress = progress_.load(std::memory_order_acquire);
        if (!progress || progress == reported_progress_ || GetTickCount64() - (progress >> 8) < 15000) return;
        reported_progress_ = progress;
        if (n_ && n_->core->log) {
            const auto message = "VR-stalled phase=" + std::to_string(progress & 255) +
                " object=" + std::to_string(progress_object_.load(std::memory_order_relaxed)) +
                " thread=" + std::to_string(progress_thread_.load(std::memory_order_relaxed));
            n_->core->log(n_->core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR, anomaly::sdk::StringView(message));
        }
    }
    Result Run(const std::vector<std::string>& paths) {
        detail_.clear();
        fields_.clear();
        classes_.clear();
        if (!available_ || !n_->ready) return Result::Unavailable;
        struct ProgressScope {
            VisualReload& owner;
            ~ProgressScope() { owner.progress_.store(0, std::memory_order_release); }
        } progress{*this};
        progress_thread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
        Transaction tx(*this);
        try {
            Progress(1); // Flush asynchronous loading.
            Handle world{}; std::string map;
            if (!n_->CurrentWorld(world, map) || !Flush(flush_)) return Result::Unavailable;
            Progress(2); // Discover packages and dependency closure.
            const auto package_class = Class("Package", "/Script/CoreUObject.");
            const auto uclass = Class("Class", "/Script/CoreUObject.");
            const auto wheel_class = OptionalClass("ChaosVehicleWheel", "/Script/ChaosVehicles.");
            const auto skel_class = Class("SkeletalMesh"), static_class = Class("StaticMesh");
            const auto material_class = Class("Material"), constant_class = Class("MaterialInstanceConstant");
            const auto texture_class = Class("Texture2D"), dynamic_class = Class("MaterialInstanceDynamic");
            const auto skeleton_class = Class("Skeleton"), animation_class = Class("AnimBlueprintGeneratedClass");
            const auto anim_sequence_class = Class("AnimSequence"), anim_montage_class = Class("AnimMontage");
            const auto blend_space_class = Class("BlendSpace"), anim_instance_class = Class("AnimInstance");
            const auto skel_component = Class("SkeletalMeshComponent"), static_component = Class("StaticMeshComponent");
            const auto child_actor_component = Class("ChildActorComponent"), actor_class = Class("Actor");
            std::set<uintptr_t> selected;
            for (const auto& path : paths) {
                if (!GamePath(path) || path.find("..") != std::string::npos) {
                    detail_ = path; return Result::Unsupported;
                }
                if (const auto package = n_->Resolve(n_->Find(path))) {
                    if (!n_->IsA(package, package_class)) { detail_ = path; return Result::Unsupported; }
                    selected.insert(package);
                }
            }
            if (selected.empty()) return Result::NotResident;
            std::unordered_map<uintptr_t, std::vector<Object>> package_objects;
            std::unordered_map<uintptr_t, unsigned> types;
            std::set<uintptr_t> active;
            for (const auto package : selected) {
                auto objects = ObjectsWithPackage(package);
                if (objects.empty()) continue;
                bool has_unsupported_export{};
                for (auto& object : objects) {
                    auto [it, inserted] = types.try_emplace(object.type);
                    if (inserted && (Derived(object.type, skel_class) || Derived(object.type, static_class) ||
                        Derived(object.type, material_class) || Derived(object.type, constant_class) ||
                        Derived(object.type, texture_class) || object.type == skeleton_class ||
                        Derived(object.type, animation_class) || Derived(object.type, anim_sequence_class) ||
                        Derived(object.type, anim_montage_class) || Derived(object.type, blend_space_class) ||
                        Derived(object.type, uclass))) it->second |= Asset;
                    object.kind = it->second;
                    if (!(object.flags & 0x30u) && !(object.kind & Asset) && object.outer == package)
                        has_unsupported_export = true;
                }
                const auto path = n_->Name(package + n_->l.ObjectName);
                if (!Exists(path)) continue;
                if (!GamePath(path) || path.find("_AnomalyRetired_") != std::string::npos || has_unsupported_export) {
                    if (detail_.empty()) detail_ = path.substr(path.find_last_of('/') + 1);
                    continue;
                }
                package_objects.emplace(package, std::move(objects));
                active.insert(package);
            }
            selected.swap(active);
            if (selected.empty()) return detail_.empty() ? Result::NotResident : Result::Unsupported;
            std::unordered_set<uintptr_t> identity;
            for (const auto package : selected) for (const auto& object : package_objects.at(package))
                identity.insert(object.address);
            std::unordered_set<uintptr_t> candidate_dynamic_materials;
            for (const auto package : selected) {
                PackageEntry entry; entry.old_package = package;
                entry.path = n_->Name(package + n_->l.ObjectName);
                entry.retired = entry.path + "_AnomalyRetired_" + std::to_string(GetTickCount64()) + "_" + std::to_string(package);
                for (const auto& object : package_objects.at(package)) if (object.outer == package &&
                    (object.kind & Asset) && !(object.flags & 0x30u)) {
                    entry.assets.push_back({object.address, 0, object.type,
                        entry.path + "." + n_->Name(object.address + n_->l.ObjectName)});
                    tx.Pin(object.address);
                }
                if (entry.assets.empty()) { detail_ = entry.path; return Result::Unsupported; }
                for (const auto& object : package_objects.at(package)) if (object.outer != package || (object.flags & 0x30u)) {
                    entry.children.push_back({object.address, 0, object.type, ObjectPath(object.address)});
                    tx.Pin(object.address);
                }
                tx.Pin(package); tx.packages.push_back(std::move(entry));
            }
            std::vector<Object> consumers;
            std::unordered_map<uintptr_t, size_t> consumer_indexes;
            AppendObjectsOfClass(skel_component, Skeletal, consumers, consumer_indexes, 0x30u);
            AppendObjectsOfClass(static_component, Static, consumers, consumer_indexes, 0x30u);
            AppendObjectsOfClass(child_actor_component, ChildActor, consumers, consumer_indexes, 0x30u);
            std::vector<Object> wheel_consumers;
            std::unordered_map<uintptr_t, size_t> wheel_indexes;
            if (wheel_class) {
                AppendObjectsOfClass(Class("ChaosWheeledVehicleMovementComponent", "/Script/ChaosVehicles."),
                    Movement, wheel_consumers, wheel_indexes, 0);
            }
            // Validate every consumer call before making any resource name unavailable.
            for (const auto& object : consumers) {
                if ((object.kind & (Skeletal | Static)) && !(object.flags & 0x30u)) {
                    Component component{{}, object.address, object.kind};
                    component.mesh = ObjectField(object.address, (object.kind & Skeletal) ? "SkinnedAsset" : "StaticMesh");
                    if (!component.mesh && (object.kind & Skeletal))
                        component.mesh = ObjectField(object.address, "SkeletalMeshAsset");
                    component.anim_class = (object.kind & Skeletal) ? ObjectField(object.address, "AnimClass", "ClassProperty") : 0;
                    component.post_class = (object.kind & Skeletal) ? ObjectField(object.address, "OverridePostProcessAnimBP", "ClassProperty") : 0;
                    component.main_instance = (object.kind & Skeletal) ? ObjectField(object.address, "AnimScriptInstance") : 0;
                    component.post_instance = (object.kind & Skeletal) ? ObjectField(object.address, "PostProcessAnimInstance") : 0;
                    if (object.kind & Skeletal) component.linked = ObjectArray(object.address, "LinkedInstances", anim_instance_class);
                    component.materials = Materials(object.address);
                    for (const auto material : component.materials)
                        if (material && n_->IsA(material, dynamic_class)) candidate_dynamic_materials.insert(material);
                    if (identity.contains(component.mesh) || identity.contains(component.anim_class) || identity.contains(component.post_class) ||
                            (component.main_instance && identity.contains(ObjectType(component.main_instance))) ||
                            (component.post_instance && identity.contains(ObjectType(component.post_instance))) ||
                            std::any_of(component.linked.begin(), component.linked.end(), [&](uintptr_t instance) {
                                return instance && identity.contains(ObjectType(instance));
                            }) || std::any_of(component.materials.begin(), component.materials.end(),
                            [&](uintptr_t value) { return identity.contains(value) ||
                                (value && n_->IsA(value, dynamic_class) && identity.contains(ObjectField(value, "Parent"))); })) {
                        component.handle = HandleOf(object.address);
                        if (object.kind & Skeletal) {
                            const auto mode = FieldAddress(object.address, "AnimationMode", "ByteProperty", 1);
                            if (!mode || !n_->Read(mode, component.animation_mode)) throw Failure{};
                            // Custom animation instances have an external lifecycle owner.
                            if (component.animation_mode > 1) { detail_ = n_->Name(object.address + n_->l.ObjectName); throw Unsupported{}; }
                        }
                        ValidateComponent(component); tx.Pin(object.address);
                        if (component.mesh) tx.Pin(component.mesh);
                        if (component.anim_class) tx.Pin(component.anim_class);
                        if (component.post_class) tx.Pin(component.post_class);
                        if (component.main_instance) tx.Pin(component.main_instance);
                        if (component.post_instance) tx.Pin(component.post_instance);
                        for (const auto instance : component.linked) if (instance) tx.Pin(instance);
                        tx.components.push_back(std::move(component));
                    }
                }
            }
            for (const auto material : candidate_dynamic_materials) {
                Object object;
                if (!ReadObject(material, object) || !n_->IsA(object.type, dynamic_class)) continue;
                const auto parent = ObjectField(material, "Parent");
                auto textures = TextureParameters(material);
                if (identity.contains(parent) || std::any_of(textures.begin(), textures.end(),
                        [&](const auto& texture) { return identity.contains(texture.texture); })) {
                    DynamicMaterial value{HandleOf(material), material, parent};
                    value.textures = std::move(textures);
                    ValidateTextures(value.handle); tx.Pin(material); tx.dynamic.push_back(std::move(value));
                }
            }
            if (tx.components.empty() && tx.dynamic.empty() && tx.child_actors.empty()) {
                detail_ = n_->Name(tx.packages.front().old_package + n_->l.ObjectName);
                return Result::Unsupported;
            }
            for (auto& package : tx.packages) {
                Progress(3, package.old_package); // Retire package names.
                if (!Rename(package.old_package, package.retired)) throw Failure{};
                package.renamed = true;
            }
            ReplacementMap replacements;
            for (auto& package : tx.packages) for (auto& asset : package.assets) {
                // Keep live Chaos wheel simulation classes intact while refreshing package visuals.
                if (wheel_class && Derived(asset.type, uclass) && Derived(asset.original, wheel_class)) continue;
                Progress(4, asset.original); // Load replacement exports.
                const auto wide = Wide(asset.path);
                if (!Load(load_, asset.type, wide.c_str(), asset.replacement) || !asset.replacement ||
                    asset.replacement == asset.original) throw Failure{};
                uintptr_t type{};
                if (!n_->Read(asset.replacement + n_->l.ObjectClass, type) || type != asset.type) throw Failure{};
                tx.Pin(asset.replacement);
                replacements.Add(asset.original, asset.replacement);
            }
            // Map matching subobjects by their real export paths (sockets, morphs,
            // cloth data and animation CDOs), never by their position in a package.
            for (auto& package : tx.packages) for (auto& child : package.children) {
                const auto fresh = n_->Resolve(n_->Find(child.path));
                if (!fresh) continue; // Removed subobject: old consumers retain it until GC.
                uintptr_t type{};
                const auto replacement_type = replacements.Find(child.type);
                if (!n_->Read(fresh + n_->l.ObjectClass, type) || type != (replacement_type ? replacement_type : child.type)) continue;
                child.replacement = fresh; tx.Pin(fresh); replacements.Add(child.original, fresh);
            }
            Progress(5); // Resolve component/material consumers.
            PrepareConsumers(tx, replacements, dynamic_class);
            PrepareChildActorConsumers(tx, consumers, replacements, child_actor_component, actor_class);
            if (wheel_class) PrepareWheelConsumers(tx, wheel_consumers, replacements, wheel_class, uclass);
            // UE setters rebuild the affected render and physics state themselves.
            tx.committing = true;
            Progress(7); // Apply native setters.
            ApplyConsumers(tx, replacements);
            for (const auto& refresh : tx.character_animation_refreshes) {
                const auto status = n_->events->invoke(n_->events->user, refresh.actor, refresh.function, nullptr, 0);
                if (status.code && detail_.empty()) detail_ = n_->Name(refresh.object + n_->l.ObjectName);
            }
            tx.committed = true;
            return detail_.empty() ? Result::Updated : Result::Partial;
        } catch (const Unsupported&) {
            return Result::Unsupported;
        } catch (const Failure& error) {
            detail_ = "VR-" + std::to_string(error.where.line());
            if (n_->core->log) {
                auto message = detail_ + " " + error.where.function_name();
                if (error.failed_type) {
                    message += " type=" + std::to_string(error.failed_type) +
                        " base=" + std::to_string(error.failed_base) +
                        " depth=" + std::to_string(error.depth) +
                        " caller=" + error.caller.function_name() + ":" + std::to_string(error.caller.line());
                }
                n_->core->log(n_->core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR, anomaly::sdk::StringView(message));
            }
            return Result::Failed;
        } catch (...) {
            detail_ = "VR-internal";
            return Result::Failed; // Transaction restores consumers and package names.
        }
    }
private:
    enum : unsigned { Asset = 1, Skeletal = 2, Static = 4, ChildActor = 64, Movement = 128 };
    struct Failure {
        std::source_location where;
        uintptr_t failed_type{}, failed_base{};
        unsigned depth{};
        std::source_location caller{};
        explicit Failure(std::source_location value = std::source_location::current(), uintptr_t type = 0,
                         uintptr_t base = 0, unsigned level = 0, std::source_location call = {})
            : where(value), failed_type(type), failed_base(base), depth(level), caller(call) {}
    };
    struct Unsupported {};
    struct Object { uintptr_t address{}, type{}, outer{}; uint32_t flags{}, kind{}; };
    struct AddressCollector { std::vector<uintptr_t> addresses; bool failed{}; };
    struct NativeFunctionRef { uintptr_t callable{}, context{}; };
    static_assert(sizeof(NativeFunctionRef) == 2 * sizeof(uintptr_t));
    struct ReplacementMap {
        void Add(uintptr_t old, uintptr_t fresh) {
            const auto [it, inserted] = values.emplace(old, fresh);
            if (!inserted && it->second != fresh) throw Failure{};
        }
        uintptr_t Find(uintptr_t old) const {
            const auto found = values.find(old);
            return found == values.end() ? 0 : found->second;
        }
        std::unordered_map<uintptr_t, uintptr_t> values;
    };
    struct AssetEntry { uintptr_t original{}, replacement{}, type{}; std::string path; };
    struct PackageEntry { uintptr_t old_package{}; std::string path, retired; std::vector<AssetEntry> assets, children; bool renamed{}; };
    struct Component {
        Handle handle{}; uintptr_t object{}; unsigned kind{}; uintptr_t mesh{};
        uintptr_t anim_class{}, updated_anim_class{}, post_class{}, updated_post_class{}, main_instance{}, post_instance{};
        uint8_t animation_mode{};
        std::vector<uintptr_t> linked;
        std::vector<uintptr_t> materials, updated_materials;
    };
    struct MaterialSlot { uintptr_t material{}; uint64_t name{}; };
    struct ParameterInfo { uint64_t name{}; uint8_t association{}; uint8_t padding[3]{}; int32_t index{}; };
    struct TextureParameter { ParameterInfo info; uintptr_t texture{}; };
    struct DynamicMaterial { Handle handle{}; uintptr_t object{}, parent{}; std::vector<TextureParameter> textures; uintptr_t updated_parent{}; };
    struct CharacterAnimationRefresh { Handle actor{}, function{}; uintptr_t object{}; };
    struct WheelSetupRef { uintptr_t address{}, wheel_class{}; };
    struct WheelSetupChange { uintptr_t owner{}, address{}, original{}, replacement{}; };
    struct WheelInstanceChange { Handle component{}; uintptr_t owner{}, original{}, replacement{}; int32_t index{}; bool attempted{}; };
    struct ChildActorChange { Handle component{}; uintptr_t object{}, original{}, replacement{}; bool had_actor{}, attempted{}; };
    struct Transaction {
        VisualReload& owner;
        std::vector<uintptr_t> held;
        std::vector<PackageEntry> packages;
        std::vector<Component> components;
        std::vector<DynamicMaterial> dynamic;
        std::vector<CharacterAnimationRefresh> character_animation_refreshes;
        std::vector<ChildActorChange> child_actors;
        std::vector<WheelSetupChange> wheel_setups;
        std::vector<WheelInstanceChange> wheels;
        bool committing{}, committed{};
        explicit Transaction(VisualReload& value) : owner(value) {}
        void Pin(uintptr_t object) {
            held.push_back(object);
            if (!Call(owner.acquire_, object)) { held.pop_back(); throw Failure{}; }
        }
        ~Transaction() {
            if (!committed) owner.Restore(*this);
            for (const auto object : held) Call(owner.release_, object);
        }
    };
    struct Array { uintptr_t data{}; int32_t count{}, capacity{}; };
    // UFunction::ParmsSize excludes the native struct's trailing alignment bytes.
    static constexpr uint16_t MeshParamsSize = sizeof(uintptr_t) + sizeof(uint8_t);
    using Parameter = NativeAccess::Parameter;
    NativeAccess* n_{};
    uintptr_t rename_{}, load_{}, acquire_{}, release_{}, flush_{}, parent_{}, exists_{};
    uintptr_t for_each_class_{}, for_each_package_{};
    uint32_t array_inner_{}, array_flags_{};
    bool available_{};
    std::atomic<uint64_t> progress_{};
    mutable std::atomic<uintptr_t> progress_object_{};
    std::atomic<uint32_t> progress_thread_{};
    uint64_t reported_progress_{}; // Worker only.
    std::string detail_;
    mutable std::map<std::tuple<uintptr_t, std::string, std::string, uint32_t>, uintptr_t> fields_;
    mutable std::unordered_map<std::string, uintptr_t> classes_;

    void Progress(uint8_t phase, uintptr_t object = 0) {
        progress_object_.store(object, std::memory_order_relaxed);
        progress_.store((GetTickCount64() << 8) | phase, std::memory_order_release);
    }
    static bool GamePath(std::string_view path) {
        // UE package names are FNames and compare without case. Display casing
        // can come from a lowercase mod index even for an ordinary Texture2D.
        return path.size() > 6 && _strnicmp(path.data(), "/game/", 6) == 0;
    }
    static std::wstring Wide(std::string_view value) {
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (!count) throw Failure{};
        std::wstring result(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
        return result;
    }
    static bool Call(uintptr_t fn, uintptr_t object) noexcept {
        __try { reinterpret_cast<void(__fastcall*)(uintptr_t)>(fn)(object); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool ReadObjectHeader(uintptr_t object, void* destination, size_t size) noexcept {
        if (object <= 65536 || !destination || !size) return false;
        __try { std::memcpy(destination, reinterpret_cast<const void*>(object), size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool __fastcall CollectPackageObject(void* context, uintptr_t* object) noexcept {
        auto& collector = *static_cast<AddressCollector*>(context);
        if (collector.failed) return false;
        try {
            if (object && *object) collector.addresses.push_back(*object);
            return true;
        } catch (...) {
            collector.failed = true;
            return false;
        }
    }
    static void __fastcall CollectClassObject(void* context, uintptr_t* object) noexcept {
        auto& collector = *static_cast<AddressCollector*>(context);
        if (collector.failed) return;
        try {
            if (object && *object) collector.addresses.push_back(*object);
        } catch (...) {
            collector.failed = true;
        }
    }
    static bool CallForEachPackage(uintptr_t fn, uintptr_t package, const NativeFunctionRef* callback) noexcept {
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, const NativeFunctionRef*, bool, uint32_t, uint32_t)>(fn)(
                package, callback, true, 0, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool CallForEachClass(uintptr_t fn, uintptr_t klass, const NativeFunctionRef* callback,
                                 uint32_t exclusion_flags) noexcept {
        __try {
            reinterpret_cast<void(__fastcall*)(uintptr_t, const NativeFunctionRef*, bool, uint32_t, uint32_t)>(fn)(
                klass, callback, true, exclusion_flags, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadObject(uintptr_t address, Object& object) const {
        std::array<uint8_t, 128> header{};
        const auto size = lMaxHeader();
        if (!ReadObjectHeader(address, header.data(), size)) throw Failure{};
        object.address = address;
        std::memcpy(&object.flags, header.data() + n_->l.ObjectFlags, sizeof(object.flags));
        if (object.flags & 0x4001b400u) return false;
        std::memcpy(&object.type, header.data() + n_->l.ObjectClass, sizeof(object.type));
        std::memcpy(&object.outer, header.data() + n_->l.ObjectOuter, sizeof(object.outer));
        if (!object.type) throw Failure{};
        return true;
    }
    std::vector<Object> ObjectsWithPackage(uintptr_t package) const {
        AddressCollector collector;
        collector.addresses.reserve(128);
        const NativeFunctionRef callback{reinterpret_cast<uintptr_t>(&CollectPackageObject), reinterpret_cast<uintptr_t>(&collector)};
        if (!CallForEachPackage(for_each_package_, package, &callback) || collector.failed) throw Failure{};
        std::vector<Object> objects;
        objects.reserve(collector.addresses.size());
        for (const auto address : collector.addresses) {
            Object object;
            if (ReadObject(address, object)) objects.push_back(object);
        }
        return objects;
    }
    void AppendObjectsOfClass(uintptr_t klass, unsigned kind, std::vector<Object>& objects,
                              std::unordered_map<uintptr_t, size_t>& indexes, uint32_t exclusion_flags) const {
        AddressCollector collector;
        collector.addresses.reserve(256);
        const NativeFunctionRef callback{reinterpret_cast<uintptr_t>(&CollectClassObject), reinterpret_cast<uintptr_t>(&collector)};
        if (!CallForEachClass(for_each_class_, klass, &callback, exclusion_flags) || collector.failed) throw Failure{};
        for (const auto address : collector.addresses) {
            Object object;
            if (!ReadObject(address, object)) continue;
            const auto [it, inserted] = indexes.emplace(address, objects.size());
            if (inserted) {
                object.kind = kind;
                objects.push_back(object);
            } else {
                objects[it->second].kind |= kind;
            }
        }
    }
    static bool Flush(uintptr_t fn) noexcept {
        const Array empty{};
        __try { reinterpret_cast<void(__fastcall*)(const Array*)>(fn)(&empty); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool Load(uintptr_t fn, uintptr_t cls, const wchar_t* path, uintptr_t& object) noexcept {
        __try {
            object = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t, const wchar_t*,
                const wchar_t*, uint32_t, uintptr_t, bool, uintptr_t)>(fn)(cls, 0, path, nullptr, 0, 0, true, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool RenameCall(uintptr_t fn, uintptr_t package, const wchar_t* name) noexcept {
        // ERenameFlags: DoNotDirty | DontCreateRedirectors | NonTransactional | SkipGeneratedClasses.
        __try { return reinterpret_cast<bool(__fastcall*)(uintptr_t, const wchar_t*, uintptr_t, uint32_t)>(fn)(package, name, 0, 0xb4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Rename(uintptr_t object, std::string_view path) const { return RenameCall(rename_, object, Wide(path).c_str()); }
    static bool ExistsCall(uintptr_t fn, const Array& path, bool& exists) noexcept {
        __try { exists = reinterpret_cast<bool(__fastcall*)(const Array*, uintptr_t, bool)>(fn)(&path, 0, true); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Exists(std::string_view path) const {
        const auto wide = Wide(path);
        const Array value{reinterpret_cast<uintptr_t>(wide.c_str()), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
        bool exists{};
        if (!ExistsCall(exists_, value, exists)) throw Failure{};
        return exists;
    }
    static bool SetParent(uintptr_t fn, uintptr_t object, uintptr_t parent) noexcept {
        __try { reinterpret_cast<bool(__fastcall*)(uintptr_t, uintptr_t, bool)>(fn)(object, parent, false); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // WheelSetups is a reflected TSubclassOf field; callers validate its owner, type, and offset first.
    bool WritePointer(uintptr_t address, uintptr_t value) const {
        const auto required = offsetof(AnomalyCoreServiceV1, write_memory) + sizeof(n_->core->write_memory);
        return address > 65536 && n_->core->struct_size >= required && n_->core->write_memory &&
            n_->core->write_memory(n_->core->user, address,
                {reinterpret_cast<const uint8_t*>(&value), sizeof(value)}).code == ANOMALY_STATUS_V1_OK;
    }
    uintptr_t OptionalClass(std::string_view name, std::string_view prefix = "/Script/Engine.") const {
        const auto path = std::string(prefix) + std::string(name);
        if (const auto found = classes_.find(path); found != classes_.end()) return found->second;
        const auto value = n_->Resolve(n_->Find(path));
        classes_.emplace(path, value);
        return value;
    }
    uintptr_t Class(std::string_view name, std::string_view prefix = "/Script/Engine.") const {
        const auto value = OptionalClass(name, prefix);
        if (!value) throw Failure{};
        return value;
    }
    bool Derived(uintptr_t type, uintptr_t base, std::source_location caller = std::source_location::current()) const {
        for (unsigned i = 0; type && i < 64; ++i) {
            if (type == base) return true;
            uintptr_t parent{};
            if (!n_->Read(type + n_->l.StructSuper, parent))
                throw Failure{std::source_location::current(), type, base, i, caller};
            type = parent;
        }
        return false;
    }
    Handle HandleOf(uintptr_t object) const {
        uint32_t index{}; AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (!n_->Read(object + n_->l.ObjectIndex, index) ||
            n_->objects->snapshot_at(n_->objects->user, index, &snapshot).code || n_->Resolve(snapshot.handle) != object) throw Failure{};
        return snapshot.handle;
    }
    size_t lMaxHeader() const {
        return std::max({size_t{n_->l.ObjectOuter} + sizeof(uintptr_t), size_t{n_->l.ObjectClass} + sizeof(uintptr_t),
            size_t{n_->l.ObjectFlags} + sizeof(uint32_t)});
    }
    uintptr_t Property(uintptr_t type, std::string_view name, std::string_view expected, uint32_t size) const {
        const auto key = std::make_tuple(type, std::string(name), std::string(expected), size);
        if (const auto found = fields_.find(key); found != fields_.end()) return found->second;
        for (unsigned depth = 0; type && depth < 64; ++depth) {
            uintptr_t field{};
            if (!n_->Read(type + n_->l.StructProperties, field)) throw Failure{};
            for (unsigned i = 0; field && i < 4096; ++i) {
                if (n_->Name(field + n_->l.PropertyName) == name) {
                    uintptr_t cls{}; int32_t width{}, dim{};
                    if (!n_->Read(field + n_->l.PropertyClass, cls) || n_->Name(cls + n_->l.FieldClassName) != expected ||
                        !n_->Read(field + n_->l.PropertySize, width) || width < 0 || static_cast<uint32_t>(width) != size ||
                        !n_->Read(field + n_->l.PropertyDim, dim) || dim != 1) throw Failure{};
                    fields_.emplace(key, field); return field;
                }
                if (!n_->Read(field + n_->l.PropertyNext, field)) throw Failure{};
            }
            if (!n_->Read(type + n_->l.StructSuper, type)) throw Failure{};
        }
        fields_.emplace(key, 0); return 0;
    }
    uintptr_t FieldAddress(uintptr_t object, std::string_view name, std::string_view type, uint32_t size) const {
        uintptr_t cls{}; int32_t offset{}, extent{};
        if (!n_->Read(object + n_->l.ObjectClass, cls)) throw Failure{};
        const auto field = Property(cls, name, type, size);
        if (!field) return 0;
        if (!n_->Read(field + n_->l.PropertyOffset, offset) || offset < 0 ||
            !n_->Read(cls + n_->l.StructSize, extent) || extent < 0 || size > static_cast<uint32_t>(extent) ||
            static_cast<uint32_t>(offset) > static_cast<uint32_t>(extent) - size) throw Failure{};
        return object + offset;
    }
    uintptr_t ObjectField(uintptr_t object, std::string_view name, std::string_view type = "ObjectProperty") const {
        const auto address = FieldAddress(object, name, type, sizeof(uintptr_t));
        uintptr_t result{};
        if (address && !n_->Read(address, result)) throw Failure{};
        return result;
    }
    uintptr_t ObjectType(uintptr_t object) const {
        uintptr_t type{};
        if (!n_->Read(object + n_->l.ObjectClass, type) || !type) throw Failure{};
        return type;
    }
    uintptr_t ArrayInner(uintptr_t field) const {
        uint8_t flags{}; uintptr_t inner{}; int32_t dim{};
        // EArrayPropertyFlags is uint8; the following padding is not initialized.
        // Memory-image arrays have a different data pointer contract.
        if (!field || !n_->Read(field + array_flags_, flags) || flags || !n_->Read(field + array_inner_, inner) || !inner ||
            !n_->Read(inner + n_->l.PropertyDim, dim) || dim != 1) throw Failure{};
        return inner;
    }
    std::vector<uintptr_t> ObjectArray(uintptr_t component, std::string_view name, uintptr_t expected) const {
        uintptr_t cls{}, inner{}, type{};
        if (!n_->Read(component + n_->l.ObjectClass, cls)) throw Failure{};
        const auto field = Property(cls, name, "ArrayProperty", sizeof(Array));
        inner = ArrayInner(field);
        int32_t width{};
        if (!n_->Read(inner + n_->l.PropertySize, width) || width != sizeof(uintptr_t) ||
            !n_->Read(inner + n_->l.PropertyClass, type) ||
            n_->Name(type + n_->l.FieldClassName) != "ObjectProperty" ||
            !n_->Read(inner + n_->l.ObjectPropertyClass, type) || type != expected) throw Failure{};
        const auto address = FieldAddress(component, name, "ArrayProperty", sizeof(Array));
        Array array;
        if (!address || !n_->Read(address, array) || array.count < 0 || array.count > 1024 || array.capacity < array.count ||
            (array.count && !array.data)) throw Failure{};
        std::vector<uintptr_t> result(static_cast<size_t>(array.count));
        if (!result.empty() && n_->core->read_memory(n_->core->user, array.data,
                {reinterpret_cast<uint8_t*>(result.data()), result.size() * sizeof(uintptr_t)}).code) throw Failure{};
        return result;
    }
    std::vector<WheelSetupRef> WheelSetups(uintptr_t component, uintptr_t wheel_class) const {
        uintptr_t cls{}, inner{}, inner_class{}, structure{};
        if (!n_->Read(component + n_->l.ObjectClass, cls)) throw Failure{};
        const auto field = Property(cls, "WheelSetups", "ArrayProperty", sizeof(Array));
        inner = ArrayInner(field);
        int32_t stride{};
        if (!n_->Read(inner + n_->l.PropertyClass, inner_class) ||
            n_->Name(inner_class + n_->l.FieldClassName) != "StructProperty" ||
            !n_->Read(inner + n_->l.StructPropertyType, structure) ||
            structure != Class("ChaosWheelSetup", "/Script/ChaosVehicles.") ||
            !n_->Read(inner + n_->l.PropertySize, stride) || stride <= 0 || stride > 1024) throw Failure{};
        const auto class_field = Property(structure, "WheelClass", "ClassProperty", sizeof(uintptr_t));
        int32_t class_offset{}, structure_size{};
        uintptr_t meta_class{};
        if (!class_field || !n_->Read(class_field + n_->l.ClassPropertyMetaClass, meta_class) || meta_class != wheel_class ||
            !n_->Read(class_field + n_->l.PropertyOffset, class_offset) || class_offset < 0 ||
            static_cast<uint32_t>(class_offset) % alignof(uintptr_t) ||
            !n_->Read(structure + n_->l.StructSize, structure_size) || structure_size != stride ||
            static_cast<uint32_t>(class_offset) + sizeof(uintptr_t) > static_cast<uint32_t>(stride)) throw Failure{};
        Array array;
        const auto address = FieldAddress(component, "WheelSetups", "ArrayProperty", sizeof(Array));
        if (!address || !n_->Read(address, array) || array.count < 0 || array.count > 1024 ||
            array.capacity < array.count || (array.count && !array.data)) throw Failure{};
        std::vector<WheelSetupRef> result;
        result.reserve(static_cast<size_t>(array.count));
        for (int32_t i = 0; i < array.count; ++i) {
            const auto value_address = array.data + static_cast<uint64_t>(i) * static_cast<uint32_t>(stride) + static_cast<uint32_t>(class_offset);
            uintptr_t value{};
            if (!n_->Read(value_address, value) || (value && !Derived(value, wheel_class))) throw Failure{};
            result.push_back({value_address, value});
        }
        return result;
    }
    std::vector<uintptr_t> Materials(uintptr_t component) const {
        return ObjectArray(component, "OverrideMaterials", Class("MaterialInterface"));
    }
    std::string ObjectPath(uintptr_t object) const {
        uintptr_t outer{}; if (!n_->Read(object + n_->l.ObjectOuter, outer)) throw Failure{};
        const auto name = n_->Name(object + n_->l.ObjectName);
        if (!outer) return name;
        uintptr_t parent{}; if (!n_->Read(outer + n_->l.ObjectOuter, parent)) throw Failure{};
        const auto prefix = ObjectPath(outer);
        return prefix + (parent && prefix.find(':') == std::string::npos ? ":" : ".") + name;
    }
    void ValidateComponent(const Component& component) const;
    std::vector<MaterialSlot> MeshMaterials(uintptr_t mesh, unsigned kind) const;
    void PrepareConsumers(Transaction& tx, const ReplacementMap& map, uintptr_t dynamic_class);
    void PrepareChildActorConsumers(Transaction& tx, const std::vector<Object>& all, const ReplacementMap& map,
        uintptr_t child_actor_component, uintptr_t actor_class);
    void PrepareWheelConsumers(Transaction& tx, const std::vector<Object>& all, const ReplacementMap& map,
        uintptr_t wheel_class, uintptr_t uclass);
    void ApplyWheelConsumers(Transaction& tx, bool restore);
    std::vector<TextureParameter> TextureParameters(uintptr_t object) const;
    void ValidateTextures(Handle material) const;
    void ApplyConsumers(Transaction& tx, const ReplacementMap& map, bool restore = false);
    void Restore(Transaction& tx) noexcept;
};

inline void VisualReload::ValidateComponent(const Component& component) const {
    if (component.kind & Skeletal) {
        const std::array<Parameter, 2> shape{{{"NewMesh", "ObjectProperty", 0, 8, "/Script/Engine.SkinnedAsset"},
            {"bReinitPose", "BoolProperty", 8, 1}}};
        if (!n_->ValidateFunction(component.handle, "/Script/Engine.SkinnedMeshComponent:SetSkinnedAssetAndUpdate", MeshParamsSize, shape).id) throw Failure{};
    } else {
        const std::array<Parameter, 2> shape{{{"NewMesh", "ObjectProperty", 0, 8, "/Script/Engine.StaticMesh"},
            {"ReturnValue", "BoolProperty", 8, 1}}};
        if (!n_->ValidateFunction(component.handle, "/Script/Engine.StaticMeshComponent:SetStaticMesh", MeshParamsSize, shape, 8).id) throw Failure{};
    }
    const std::array<Parameter, 2> material{{{"ElementIndex", "IntProperty", 0, 4},
        {"Material", "ObjectProperty", 8, 8, "/Script/Engine.MaterialInterface"}}};
    if (!n_->ValidateFunction(component.handle, "/Script/Engine.PrimitiveComponent:SetMaterial", 16, material).id) throw Failure{};
    if ((component.kind & Skeletal) && !n_->ValidateFunction(component.handle,
            "/Script/Engine.SkeletalMeshComponent:SetAnimInstanceClass", 8,
            std::array<Parameter, 1>{{{"NewClass", "ClassProperty", 0, 8, "/Script/CoreUObject.Class"}}}).id)
        throw Failure{};
    if (component.kind & Skeletal) {
        const std::array<Parameter, 2> post{{{"InPostProcessAnimBlueprint", "ClassProperty", 0, 8, "/Script/CoreUObject.Class"},
            {"ReinitAnimInstances", "BoolProperty", 8, 1}}};
        if (!n_->ValidateFunction(component.handle, "/Script/Engine.SkeletalMeshComponent:SetOverridePostProcessAnimBP", MeshParamsSize, post).id) throw Failure{};
    }
}

inline std::vector<VisualReload::MaterialSlot> VisualReload::MeshMaterials(uintptr_t mesh, unsigned kind) const {
    const auto name = kind & Skeletal ? "Materials" : "StaticMaterials";
    const auto type_name = kind & Skeletal ? "SkeletalMaterial" : "StaticMaterial";
    uintptr_t cls{}, inner{}, type{}, structure{};
    int32_t stride{}, material_offset{}, name_offset{};
    if (!n_->Read(mesh + n_->l.ObjectClass, cls)) throw Failure{};
    const auto field = Property(cls, name, "ArrayProperty", sizeof(Array));
    inner = ArrayInner(field);
    if (!n_->Read(inner + n_->l.PropertyClass, type) ||
        n_->Name(type + n_->l.FieldClassName) != "StructProperty" ||
        !n_->Read(inner + n_->l.StructPropertyType, structure) || structure != Class(type_name) ||
        !n_->Read(inner + n_->l.PropertySize, stride) || stride < sizeof(MaterialSlot) || stride > 1024) throw Failure{};
    const auto material = Property(structure, "MaterialInterface", "ObjectProperty", sizeof(uintptr_t));
    const auto slot = Property(structure, "MaterialSlotName", "NameProperty", sizeof(uint64_t));
    if (!material || !slot || !n_->Read(material + n_->l.ObjectPropertyClass, type) || type != Class("MaterialInterface") ||
        !n_->Read(material + n_->l.PropertyOffset, material_offset) || material_offset < 0 || material_offset + sizeof(uintptr_t) > stride ||
        !n_->Read(slot + n_->l.PropertyOffset, name_offset) || name_offset < 0 || name_offset + sizeof(uint64_t) > stride) throw Failure{};
    Array array;
    const auto address = FieldAddress(mesh, name, "ArrayProperty", sizeof(Array));
    if (!address || !n_->Read(address, array) || array.count < 0 || array.count > 1024 || array.capacity < array.count ||
        (array.count && !array.data)) throw Failure{};
    std::vector<MaterialSlot> result(static_cast<size_t>(array.count));
    for (size_t i = 0; i < result.size(); ++i) {
        const auto at = array.data + i * stride;
        if (!n_->Read(at + material_offset, result[i].material) || !n_->Read(at + name_offset, result[i].name)) throw Failure{};
    }
    return result;
}

inline void VisualReload::PrepareConsumers(Transaction& tx, const ReplacementMap& map, uintptr_t dynamic_class) {
    std::unordered_map<uintptr_t, uintptr_t> requested_parents;
    std::unordered_set<uint64_t> refreshed_actors;
    uintptr_t ability_character_class{};
    for (auto& material : tx.dynamic) material.updated_parent = map.Find(material.parent);
    for (auto& component : tx.components) {
        component.updated_anim_class = map.Find(component.anim_class);
        component.updated_post_class = map.Find(component.post_class);
        component.updated_materials = component.materials;
        for (auto& material : component.updated_materials) if (const auto fresh = map.Find(material)) material = fresh;
        const auto mesh = map.Find(component.mesh);
        if (mesh) {
            if (component.kind & Skeletal) {
                uintptr_t actor{};
                if (!n_->Read(component.object + n_->l.ObjectOuter, actor) || !actor) throw Failure{};
                if (!ability_character_class) {
                    ability_character_class = Class("HTAbilityCharacter", "/Script/HTGame.");
                }
                if (Derived(ObjectType(actor), ability_character_class)) {
                    const auto actor_handle = HandleOf(actor);
                    if (refreshed_actors.insert(actor_handle.id).second) {
                        const auto function_handle = n_->ValidateFunction(actor_handle,
                            "/Script/HTGame.HTAbilityCharacter:RefreshAnimInstance", 0,
                            std::span<const Parameter>{});
                        if (!function_handle.id) { detail_ = n_->Name(actor + n_->l.ObjectName); throw Unsupported{}; }
                        tx.Pin(actor);
                        tx.character_animation_refreshes.push_back({actor_handle, function_handle, actor});
                    }
                }
            }
            // Registration reinitializes animation and required bones against the
            // newly loaded skeleton; no old bone-index cache is carried across.
            const auto old_slots = MeshMaterials(component.mesh, component.kind);
            const auto new_slots = MeshMaterials(mesh, component.kind);
            component.updated_materials.assign(std::max(component.materials.size(), new_slots.size()), 0);
            for (size_t i = 0; i < component.materials.size(); ++i) {
                const auto original = component.materials[i];
                if (!original) continue;
                auto destination = i;
                if (i < old_slots.size() && old_slots[i].name) {
                    const auto match = std::find_if(new_slots.begin(), new_slots.end(), [&](const auto& slot) { return slot.name == old_slots[i].name; });
                    if (match != new_slots.end()) destination = static_cast<size_t>(match - new_slots.begin());
                }
                if (destination >= new_slots.size()) continue;
                auto replacement = map.Find(original);
                if (!replacement) replacement = original;
                const bool dynamic = n_->IsA(original, dynamic_class);
                const auto parent = dynamic ? ObjectField(original, "Parent") : 0;
                const bool follows_slot = i < old_slots.size() &&
                    (original == old_slots[i].material || (parent && parent == old_slots[i].material));
                if (follows_slot) {
                    const auto target = new_slots[destination].material;
                    if (dynamic && target) {
                        const auto [it, inserted] = requested_parents.emplace(original, target);
                        if (!inserted && it->second != target) {
                            detail_ = n_->Name(original + n_->l.ObjectName); throw Unsupported{};
                        }
                        auto entry = std::find_if(tx.dynamic.begin(), tx.dynamic.end(), [&](const auto& value) { return value.object == original; });
                        if (entry == tx.dynamic.end()) {
                            DynamicMaterial value{HandleOf(original), original, parent, TextureParameters(original), target};
                            ValidateTextures(value.handle); tx.Pin(original); tx.dynamic.push_back(std::move(value));
                        } else entry->updated_parent = target;
                        replacement = original; // Preserve the live MID's scalar/vector overrides.
                    } else replacement = target;
                }
                component.updated_materials[destination] = replacement;
            }
        }
        for (const auto material : component.materials) if (material) tx.Pin(material);
    }
    // Rollback must also retain dependencies belonging only to the disabled mod.
    for (const auto& material : tx.dynamic) {
        if (material.parent) tx.Pin(material.parent);
        for (const auto& texture : material.textures) if (texture.texture) tx.Pin(texture.texture);
    }
}

inline void VisualReload::PrepareChildActorConsumers(
    Transaction& tx, const std::vector<Object>& all, const ReplacementMap& map,
    uintptr_t child_actor_component, uintptr_t actor_class) {
    const std::array<Parameter, 1> shape{{
        {"InClass", "ClassProperty", 0, sizeof(uintptr_t), "/Script/CoreUObject.Class", {}, "/Script/Engine.Actor"}
    }};
    std::map<std::tuple<uintptr_t, std::string>, uintptr_t> template_classes;
    for (const auto& package : tx.packages) for (const auto& child : package.children) {
        if (!child.replacement) continue;
        uintptr_t type{}, outer{};
        if (!n_->Read(child.original + n_->l.ObjectClass, type) || !Derived(type, child_actor_component) ||
            !n_->Read(child.original + n_->l.ObjectOuter, outer) || !outer) continue;
        const auto owner_class = ObjectType(outer);
        if (!map.Find(owner_class)) continue;
        const auto name = n_->Name(child.original + n_->l.ObjectName);
        const auto value = ObjectField(child.replacement, "ChildActorClass", "ClassProperty");
        const auto [it, inserted] = template_classes.emplace(std::make_tuple(owner_class, name), value);
        if (!inserted && it->second != value) throw Failure{};
    }
    std::unordered_map<uintptr_t, bool> component_types;
    for (const auto& object : all) {
        if (object.flags & 0x30u) continue;
        auto [type, inserted] = component_types.try_emplace(object.type, false);
        if (inserted) type->second = Derived(object.type, child_actor_component);
        if (!type->second) continue;

        const auto field = Property(object.type, "ChildActorClass", "ClassProperty", sizeof(uintptr_t));
        uintptr_t meta_class{};
        const auto address = FieldAddress(object.address, "ChildActorClass", "ClassProperty", sizeof(uintptr_t));
        if (!field || !address || !n_->Read(field + n_->l.ClassPropertyMetaClass, meta_class) || meta_class != actor_class)
            throw Failure{};
        uintptr_t original{};
        if (!n_->Read(address, original)) throw Failure{};
        uintptr_t replacement = map.Find(original);
        bool has_replacement = replacement != 0;
        if (!has_replacement && object.outer) {
            const auto owner_class = ObjectType(object.outer);
            const auto fallback = template_classes.find(std::make_tuple(owner_class,
                n_->Name(object.address + n_->l.ObjectName)));
            if (fallback != template_classes.end()) {
                replacement = fallback->second;
                has_replacement = true;
            }
        }
        if (!has_replacement || replacement == original) continue;
        if ((original && !Derived(original, actor_class)) ||
            (replacement && !Derived(replacement, actor_class))) throw Failure{};

        const auto handle = HandleOf(object.address);
        if (!n_->ValidateFunction(handle, "/Script/Engine.ChildActorComponent:SetChildActorClass", sizeof(uintptr_t), shape).id)
            throw Failure{};
        const auto child_actor = ObjectField(object.address, "ChildActor");
        // Preserve the active actor's seat and interaction state; its visual components are rebound separately.
        if (child_actor) continue;
        tx.Pin(object.address);
        if (original) tx.Pin(original);
        if (replacement) tx.Pin(replacement);
        tx.child_actors.push_back({handle, object.address, original, replacement, false});
    }
}

inline void VisualReload::PrepareWheelConsumers(
    Transaction& tx, const std::vector<Object>& all, const ReplacementMap& map, uintptr_t wheel_class, uintptr_t uclass) {
    std::vector<uintptr_t> replaced_classes;
    for (const auto& package : tx.packages) for (const auto& asset : package.assets) {
        if (Derived(asset.type, uclass) && Derived(asset.original, wheel_class) && map.Find(asset.original)) {
            if (!Derived(map.Find(asset.original), wheel_class)) throw Failure{};
            replaced_classes.push_back(asset.original);
        }
    }
    if (replaced_classes.empty()) return;

    const auto movement_class = Class("ChaosWheeledVehicleMovementComponent", "/Script/ChaosVehicles.");
    const std::array<Parameter, 2> setter_shape{{
        {"WheelIndex", "IntProperty", 0, sizeof(int32_t)},
        {"InWheelClass", "ClassProperty", 8, sizeof(uintptr_t), "/Script/CoreUObject.Class", {}, "/Script/ChaosVehicles.ChaosVehicleWheel"}
    }};
    std::unordered_map<uintptr_t, bool> movement_types;
    for (const auto& object : all) {
        auto [movement_type, movement_type_inserted] = movement_types.try_emplace(object.type, false);
        if (movement_type_inserted) movement_type->second = Derived(object.type, movement_class);
        if (!movement_type->second) continue;
        const auto setups = WheelSetups(object.address, wheel_class);
        for (const auto& setup : setups) if (const auto replacement = map.Find(setup.wheel_class)) {
            tx.Pin(object.address);
            tx.Pin(setup.wheel_class);
            tx.wheel_setups.push_back({object.address, setup.address, setup.wheel_class, replacement});
        }
        // Templates carry the setup defaults for future instances; their transient
        // Wheels array is not an active physics consumer.
        if (object.flags & 0x30u) continue;
        const auto wheels = ObjectArray(object.address, "Wheels", wheel_class);
        for (size_t index = 0; index < wheels.size(); ++index) {
            const auto wheel = wheels[index];
            if (!wheel) continue;
            const auto type = ObjectType(wheel);
            if (const auto replacement = map.Find(type)) {
                if (index >= setups.size() || setups[index].wheel_class != type) {
                    detail_ = n_->Name(type + n_->l.ObjectName);
                    throw Unsupported{};
                }
                const auto handle = HandleOf(object.address);
                if (!n_->ValidateFunction(handle,
                        "/Script/ChaosVehicles.ChaosWheeledVehicleMovementComponent:SetWheelClass",
                        16, setter_shape).id) {
                    detail_ = n_->Name(type + n_->l.ObjectName);
                    throw Unsupported{};
                }
                tx.Pin(wheel);
                tx.wheels.push_back({handle, object.address, type, replacement, static_cast<int32_t>(index)});
            } else {
                for (const auto old_class : replaced_classes) if (type != old_class && Derived(type, old_class)) {
                    detail_ = n_->Name(type + n_->l.ObjectName);
                    throw Unsupported{};
                }
            }
        }
    }
    if (!tx.wheel_setups.empty() &&
        (!n_->core->write_memory || n_->core->struct_size <
            offsetof(AnomalyCoreServiceV1, write_memory) + sizeof(n_->core->write_memory))) {
        detail_ = n_->Name(replaced_classes.front() + n_->l.ObjectName);
        throw Unsupported{};
    }
}

inline std::vector<VisualReload::TextureParameter> VisualReload::TextureParameters(uintptr_t object) const {
    uintptr_t cls{}, inner{}, structure{}, inner_type{}; int32_t stride{}, info_offset{}, value_offset{};
    if (!n_->Read(object + n_->l.ObjectClass, cls)) throw Failure{};
    const auto field = Property(cls, "TextureParameterValues", "ArrayProperty", sizeof(Array));
    inner = ArrayInner(field);
    if (!n_->Read(inner + n_->l.PropertyClass, inner_type) ||
        n_->Name(inner_type + n_->l.FieldClassName) != "StructProperty" || !n_->Read(inner + n_->l.StructPropertyType, structure) ||
        structure != n_->Resolve(n_->Find("/Script/Engine.TextureParameterValue")) ||
        !n_->Read(inner + n_->l.PropertySize, stride) || stride < sizeof(TextureParameter) || stride > 1024) throw Failure{};
    const auto info = Property(structure, "ParameterInfo", "StructProperty", sizeof(ParameterInfo));
    const auto value = Property(structure, "ParameterValue", "ObjectProperty", sizeof(uintptr_t));
    uintptr_t info_type{}, value_type{};
    if (!info || !value || !n_->Read(info + n_->l.StructPropertyType, info_type) || info_type != n_->Resolve(n_->Find("/Script/Engine.MaterialParameterInfo")) ||
        !n_->Read(value + n_->l.ObjectPropertyClass, value_type) || value_type != Class("Texture") ||
        !n_->Read(info + n_->l.PropertyOffset, info_offset) || info_offset < 0 || info_offset + sizeof(ParameterInfo) > stride ||
        !n_->Read(value + n_->l.PropertyOffset, value_offset) || value_offset < 0 || value_offset + sizeof(uintptr_t) > stride) throw Failure{};
    // ParameterInfo is passed by value to ProcessEvent. Validate all of its fields.
    for (const auto& [name, type, offset, size] : std::array<std::tuple<std::string_view, std::string_view, size_t, size_t>, 3>{{
            {"Name", "NameProperty", offsetof(ParameterInfo, name), sizeof(uint64_t)},
            {"Association", "ByteProperty", offsetof(ParameterInfo, association), sizeof(uint8_t)},
            {"Index", "IntProperty", offsetof(ParameterInfo, index), sizeof(int32_t)}}}) {
        const auto member = Property(info_type, name, type, static_cast<uint32_t>(size)); int32_t actual{};
        if (!member || !n_->Read(member + n_->l.PropertyOffset, actual) || actual < 0 || static_cast<size_t>(actual) != offset) throw Failure{};
    }
    Array array;
    const auto address = FieldAddress(object, "TextureParameterValues", "ArrayProperty", sizeof(Array));
    if (!n_->Read(address, array) || array.count < 0 || array.count > 4096 || array.capacity < array.count || (array.count && !array.data)) throw Failure{};
    std::vector<TextureParameter> result(static_cast<size_t>(array.count));
    for (int32_t i = 0; i < array.count; ++i) {
        const auto at = array.data + static_cast<size_t>(i) * stride;
        if (!n_->Read(at + info_offset, result[i].info) || !n_->Read(at + value_offset, result[i].texture)) throw Failure{};
    }
    return result;
}

inline void VisualReload::ValidateTextures(Handle material) const {
    const std::array<Parameter, 2> shape{{{"ParameterInfo", "StructProperty", 0, sizeof(ParameterInfo), {}, "/Script/Engine.MaterialParameterInfo"},
        {"Value", "ObjectProperty", sizeof(ParameterInfo), sizeof(uintptr_t), "/Script/Engine.Texture"}}};
    if (!n_->ValidateFunction(material, "/Script/Engine.MaterialInstanceDynamic:SetTextureParameterValueByInfo", sizeof(TextureParameter), shape).id) throw Failure{};
}

inline void VisualReload::ApplyWheelConsumers(Transaction& tx, bool restore) {
    for (auto& setup : tx.wheel_setups) {
        progress_object_.store(setup.owner, std::memory_order_relaxed);
        const auto target = restore ? setup.original : setup.replacement;
        const auto expected = restore ? setup.replacement : setup.original;
        uintptr_t current{};
        if (!n_->Read(setup.address, current)) throw Failure{};
        if (current == target) continue;
        if (current != expected || !WritePointer(setup.address, target)) throw Failure{};
        if (!n_->Read(setup.address, current) || current != target) throw Failure{};
    }
    for (auto& wheel : tx.wheels) {
        if (restore && !wheel.attempted) continue;
        progress_object_.store(wheel.owner, std::memory_order_relaxed);
        const auto target = restore ? wheel.original : wheel.replacement;
        const auto wheels = ObjectArray(wheel.owner, "Wheels", Class("ChaosVehicleWheel", "/Script/ChaosVehicles."));
        if (wheel.index < 0 || static_cast<size_t>(wheel.index) >= wheels.size()) throw Failure{};
        const auto current = wheels[static_cast<size_t>(wheel.index)];
        if (!current) throw Failure{};
        const auto current_class = ObjectType(current);
        if (current_class == target) continue;
        if ((!restore && current_class != wheel.original) ||
            (restore && current_class != wheel.replacement)) throw Failure{};
        struct Params { int32_t index; int32_t padding{}; uintptr_t wheel_class; } params{wheel.index, 0, target};
        wheel.attempted = true;
        if (!n_->InvokeParameters(wheel.component,
                "/Script/ChaosVehicles.ChaosWheeledVehicleMovementComponent:SetWheelClass",
                &params, sizeof(params), std::array<Parameter, 2>{{
                    {"WheelIndex", "IntProperty", 0, sizeof(int32_t)},
                    {"InWheelClass", "ClassProperty", 8, sizeof(uintptr_t), "/Script/CoreUObject.Class", {}, "/Script/ChaosVehicles.ChaosVehicleWheel"}
                }})) throw Failure{};
        const auto updated = ObjectArray(wheel.owner, "Wheels", Class("ChaosVehicleWheel", "/Script/ChaosVehicles."));
        if (static_cast<size_t>(wheel.index) >= updated.size() || !updated[static_cast<size_t>(wheel.index)]) throw Failure{};
        const auto updated_type = ObjectType(updated[static_cast<size_t>(wheel.index)]);
        if (updated_type != target) {
            if (updated_type != wheel.original) throw Failure{};
            detail_ = n_->Name(wheel.original + n_->l.ObjectName);
            // UE silently no-ops without a live body/simulation; keep setup and active wheel atomic.
            throw Unsupported{};
        }
    }
}

inline void VisualReload::ApplyConsumers(Transaction& tx, const ReplacementMap& map, bool restore) {
    const std::array<Parameter, 1> child_actor_shape{{
        {"InClass", "ClassProperty", 0, sizeof(uintptr_t), "/Script/CoreUObject.Class", {}, "/Script/Engine.Actor"}
    }};
    for (auto& change : tx.child_actors) {
        if (restore && !change.attempted) continue;
        progress_object_.store(change.object, std::memory_order_relaxed);
        const auto address = FieldAddress(change.object, "ChildActorClass", "ClassProperty", sizeof(uintptr_t));
        uintptr_t current{};
        if (!address || !n_->Read(address, current)) throw Failure{};
        const auto target = restore ? change.original : change.replacement;
        const auto expected = restore ? change.replacement : change.original;
        if (current == target) continue;
        if (current != expected) throw Failure{};
        uintptr_t params = target;
        change.attempted = true;
        if (!n_->InvokeParameters(change.component, "/Script/Engine.ChildActorComponent:SetChildActorClass",
                &params, sizeof(params), child_actor_shape) ||
            !n_->Read(address, current) || current != target) throw Failure{};
        const auto child_actor = ObjectField(change.object, "ChildActor");
        if ((child_actor && (!target || ObjectType(child_actor) != target)) ||
            (!child_actor && change.had_actor && target)) throw Failure{};
    }
    for (const auto& value : tx.dynamic) {
        progress_object_.store(value.object, std::memory_order_relaxed);
        if (value.updated_parent) {
            const auto target = restore ? value.parent : value.updated_parent;
            if (!SetParent(parent_, value.object, target) || ObjectField(value.object, "Parent") != target) throw Failure{};
        }
        for (const auto& texture : value.textures) if (const auto replacement = map.Find(texture.texture)) {
            TextureParameter p{texture.info, restore ? texture.texture : replacement};
            const std::array<Parameter, 2> shape{{{"ParameterInfo", "StructProperty", 0, sizeof(ParameterInfo), {}, "/Script/Engine.MaterialParameterInfo"},
                {"Value", "ObjectProperty", sizeof(ParameterInfo), sizeof(uintptr_t), "/Script/Engine.Texture"}}};
            if (!n_->InvokeParameters(value.handle, "/Script/Engine.MaterialInstanceDynamic:SetTextureParameterValueByInfo", &p, sizeof(p), shape)) throw Failure{};
        }
    }
    for (const auto& component : tx.components) {
        progress_object_.store(component.object, std::memory_order_relaxed);
        if (const auto replacement = map.Find(component.mesh)) {
            const auto target = restore ? component.mesh : replacement;
            struct Params { uintptr_t mesh; bool flag{}; uint8_t padding[7]{}; } p{target, true};
            if (component.kind & Skeletal) {
                const std::array<Parameter, 2> shape{{{"NewMesh", "ObjectProperty", 0, 8, "/Script/Engine.SkinnedAsset"}, {"bReinitPose", "BoolProperty", 8, 1}}};
                if (!n_->InvokeParameters(component.handle, "/Script/Engine.SkinnedMeshComponent:SetSkinnedAssetAndUpdate", &p, MeshParamsSize, shape) ||
                    ObjectField(component.object, "SkinnedAsset") != target) throw Failure{};
            } else {
                const std::array<Parameter, 2> shape{{{"NewMesh", "ObjectProperty", 0, 8, "/Script/Engine.StaticMesh"}, {"ReturnValue", "BoolProperty", 8, 1}}};
                if (!n_->InvokeParameters(component.handle, "/Script/Engine.StaticMeshComponent:SetStaticMesh", &p, MeshParamsSize, shape, 8) ||
                    ObjectField(component.object, "StaticMesh") != target) throw Failure{};
            }
        }
        if (component.kind & Skeletal) {
            const auto updated_class = component.updated_anim_class;
            if (component.animation_mode == 0 && updated_class && updated_class != component.anim_class) {
                uintptr_t target = restore ? component.anim_class : updated_class;
                if (ObjectField(component.object, "AnimClass", "ClassProperty") != target) {
                    const std::array<Parameter, 1> shape{{{"NewClass", "ClassProperty", 0, 8, "/Script/CoreUObject.Class"}}};
                    if (!n_->InvokeParameters(component.handle,
                            "/Script/Engine.SkeletalMeshComponent:SetAnimInstanceClass", &target, sizeof(target), shape)) throw Failure{};
                }
            }
            const auto updated_post_class = component.updated_post_class;
            if (updated_post_class && updated_post_class != component.post_class) {
                const auto target = restore ? component.post_class : updated_post_class;
                if (ObjectField(component.object, "OverridePostProcessAnimBP", "ClassProperty") != target) {
                    struct PostParams { uintptr_t klass; bool reinit{}; } post{target, true};
                    const std::array<Parameter, 2> shape{{{"InPostProcessAnimBlueprint", "ClassProperty", 0, 8, "/Script/CoreUObject.Class"},
                        {"ReinitAnimInstances", "BoolProperty", 8, 1}}};
                    if (!n_->InvokeParameters(component.handle,
                            "/Script/Engine.SkeletalMeshComponent:SetOverridePostProcessAnimBP", &post, MeshParamsSize, shape)) throw Failure{};
                }
            }
        }
        for (size_t i = 0; i < component.updated_materials.size(); ++i) {
            const auto original = i < component.materials.size() ? component.materials[i] : 0;
            const bool refresh_parent = !map.Find(component.mesh) && original &&
                std::any_of(tx.dynamic.begin(), tx.dynamic.end(), [&](const auto& material) {
                    return material.object == original && material.updated_parent && material.updated_parent != material.parent;
                });
            if (original == component.updated_materials[i] && !refresh_parent) continue;
            struct Params { int32_t index; int32_t padding{}; uintptr_t material; } p{static_cast<int32_t>(i), 0, restore ? original : component.updated_materials[i]};
            const std::array<Parameter, 2> shape{{{"ElementIndex", "IntProperty", 0, 4}, {"Material", "ObjectProperty", 8, 8, "/Script/Engine.MaterialInterface"}}};
            // SetMaterial skips an unchanged MID pointer. Rebind through its native
            // setter so a changed parent also invalidates cached mesh draw commands.
            if (refresh_parent) {
                auto clear = p; clear.material = 0;
                if (!n_->InvokeParameters(component.handle, "/Script/Engine.PrimitiveComponent:SetMaterial", &clear, sizeof(clear), shape)) throw Failure{};
            }
            if (!n_->InvokeParameters(component.handle, "/Script/Engine.PrimitiveComponent:SetMaterial", &p, sizeof(p), shape)) throw Failure{};
        }
    }
    ApplyWheelConsumers(tx, restore);
}

inline void VisualReload::Restore(Transaction& tx) noexcept {
    try {
        Progress(12); // Restore after a failed transaction.
        ReplacementMap forward;
        for (const auto& package : tx.packages) {
            if (!package.renamed) continue;
            for (const auto* entries : {&package.assets, &package.children}) for (const auto& asset : *entries) if (asset.replacement) {
                forward.Add(asset.original, asset.replacement);
            }
        }
        if (tx.committing) {
            ApplyConsumers(tx, forward, true);
        }
        for (const auto& package : tx.packages) if (package.renamed) {
            const auto fresh = n_->Resolve(n_->Find(package.path));
            if (fresh && fresh != package.old_package && !Rename(fresh, package.retired + "_Failed")) throw Failure{};
            if (!Rename(package.old_package, package.path)) throw Failure{};
        }
    } catch (...) {
        // Do not start another transaction after an incomplete native rollback.
        available_ = false;
    }
}
} // namespace mod_manager
