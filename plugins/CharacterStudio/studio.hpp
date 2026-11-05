#pragma once
#include "catalog.hpp"

namespace character_studio {
inline constexpr float LeaseSeconds = 5.0f;
inline constexpr double RenewSeconds = 1.0;

struct Model {
    std::uint64_t id{};
    Handle actor{}, world{};
    std::string label;
    Vector position{};
    double yaw{};
    std::string animation;
};
struct Part {
    Address mesh{};
    Name socket{};
    Transform transform{};
    bool leader{};
};
struct Appearance {
    std::vector<Part> parts;
    Address idle{};
    float duration{};
    // Stable marker ("pose" / "loop"): the panel localizes it, records never hold text.
    std::string animation{"pose"};
};

// Game-domain owner. Render sees copies of Character/Model, never UE addresses.
class Studio {
public:
    explicit Studio(Engine& engine, const anomaly::sdk::Localizer& l10n)
        : ue_(engine), localizer(l10n) {}
    const std::vector<Character>& characters() const { return characters_; }
    const std::vector<Model>& models() const { return models_; }
    Handle world() const { return world_; }
    void World(Handle current) {
        if (Same(world_, current)) return;
        // Old-world actors are engine-owned and expire through their native lease.
        // Never replay an old identity against a new World, including at the same address.
        world_ = current;
        characters_.clear();
        models_.clear();
        playback_.clear();
        character_table_ = default_table_ = appearance_table_ = {};
        ue_.Reset();
    }
    void Catalog() {
        if (!ue_.Resolve(world_))
            throw std::runtime_error(
                localizer.Text("error.world_pending", "The world is not ready yet"));
        const auto library = ue_.Find("/Script/HTGame.Default__HTGameData");
        const auto data = ue_.Call("/Script/HTGame.HTGameData.GetGameData", library)
                              .Get<Address>("ReturnValue");
        if (!ue_.IsA(data, "HTGameData"))
            throw std::runtime_error(localizer.Text("error.character_data_not_ready",
                "The character data service is not ready yet"));
        const auto table = ue_.ObjectField(data, "CharacterDataTable");
        const auto rows = ue_.Rows(table, "StaticItemData");
        if (rows.rows.empty())
            throw std::runtime_error(localizer.Text("error.character_table_not_loaded",
                "The character data table is not loaded; refresh the complete catalog"));
        const auto text = ue_.Expect(rows.type, "ItemName", "TextProperty", ue_.data_layout().text_size);
        const auto asset = ue_.ObjectField(data, "CharacterDataAsset");
        if (!ue_.IsA(asset, "CharacterDataAsset"))
            throw std::runtime_error(localizer.Text("error.appearance_not_ready",
                "The appearance catalog is not ready; refresh once the character data has loaded"));
        const auto defaults = ue_.ObjectField(asset, "DefaultAppearanceDataTable");
        const auto fashions = ue_.ObjectField(asset, "AppearanceDataTable");
        if (!defaults && !fashions)
            throw std::runtime_error(localizer.Text("error.appearance_not_ready",
                "The appearance catalog is not ready; refresh once the character data has loaded"));
        std::set<std::uint64_t> default_ids;
        if (defaults) {
            for (const auto& [key, row] : ue_.Rows(defaults, "StaticDefaultAppearanceData").rows)
                default_ids.insert(key);
        }
        std::map<std::uint64_t, Name> fashion_owners;
        if (fashions) {
            const auto appearances = ue_.Rows(fashions, "StaticAppearanceData");
            for (const auto& [key, row] : appearances.rows)
                fashion_owners.emplace(key, ue_.Value<Name>(row, appearances.type,
                    "CharacterID", "NameProperty"));
        }
        // Publish atomically after joining the same two appearance routes used
        // by Spawn. This reads metadata without loading any model assets.
        std::vector<CatalogRow> next;
        next.reserve(rows.rows.size());
        for (const auto& [key, row] : rows.rows) {
            CatalogRow candidate;
            auto& item = candidate.character;
            item.id = {static_cast<std::uint32_t>(key),
                       static_cast<std::uint32_t>(key >> 32)};
            item.code = ue_.Spelling(item.id);
            item.chinese = ue_.Text(row + text.offset);
            if (!default_ids.contains(key)) {
                const auto character = ue_.Instanced(row, rows.type,
                    "ElementData", "CharacterItemData");
                candidate.default_fashion = ue_.Value<Name>(character.data, character.type,
                    "DefaultFashionID", "NameProperty");
            }
            next.push_back(std::move(candidate));
        }
        auto catalog = BuildCatalog(std::move(next), default_ids, fashion_owners);
        const auto character_handle = ue_.Pin(table);
        const auto default_handle = ue_.Pin(defaults);
        const auto appearance_handle = ue_.Pin(fashions);
        if (!character_handle.id || (defaults && !default_handle.id) ||
            (fashions && !appearance_handle.id))
            throw std::runtime_error(localizer.Text("error.catalog_handle_invalid",
                "The character catalog object failed identity validation"));
        character_table_ = character_handle;
        default_table_ = default_handle;
        appearance_table_ = appearance_handle;
        characters_ = std::move(catalog);
    }
    std::uint64_t Spawn(Name character, Vector position, double yaw) {
        if (!position.finite() || !std::isfinite(yaw))
            throw std::runtime_error(localizer.Text("error.transform_not_finite",
                "The position and angle must be finite numbers"));
        const auto chosen = std::find_if(characters_.begin(), characters_.end(),
            [&](const Character& item) { return item.id == character; });
        if (chosen == characters_.end())
            throw std::runtime_error(localizer.Text("error.catalog_refreshed",
                "The catalog was refreshed, so select the character again"));
        const auto world = ue_.Resolve(world_);
        if (!world)
            throw std::runtime_error(
                localizer.Text("error.world_stale", "The world is no longer valid"));
        // All resource loading and all function validation precede allocation.
        const auto appearance = ReadAppearance(character);
        Preflight();
        const auto cls = ue_.Find("/Script/Engine.SkeletalMeshActor");
        const auto statics = ue_.Find("/Script/Engine.Default__GameplayStatics");
        if (!ue_.Derived(cls, "SkeletalMeshActor"))
            throw std::runtime_error(localizer.Text("error.model_class_invalid",
                "The model actor class failed validation"));
        // Reserve before spawning so an allocation failure cannot lose a live actor.
        if (models_.size() == models_.capacity())
            models_.reserve(std::max(models_.size() + 1, models_.capacity() * 2));
        Model record{next_id_, {}, world_, chosen->chinese + " [" + chosen->code + "]",
                     position, yaw};
        Transform transform = At(position, yaw);
        const auto begin = ue_.Call(
            "/Script/Engine.GameplayStatics.BeginDeferredActorSpawnFromClass", statics,
            [&](Engine::Parameters& p) {
                p.Put("WorldContextObject", world);
                p.Put("ActorClass", cls);
                p.Put("SpawnTransform", transform);
                p.Put("CollisionHandlingOverride", std::uint8_t{1}); // AlwaysSpawn, not Undefined.
                p.Put("Owner", Address{}); // No controller, pawn, inventory, or network owner.
                p.Put("TransformScaleMethod", std::uint8_t{0});
            });
        const auto actor = begin.Get<Address>("ReturnValue");
        if (!actor)
            throw std::runtime_error(
                localizer.Text("error.spawn_rejected", "The engine did not create the model"));
        try {
            record.actor = ue_.Pin(actor);
            if (!record.actor.id)
                throw std::runtime_error(localizer.Text("error.new_model_handle_invalid",
                    "The new model failed identity validation"));
            // Engine-owned expiry is safe even after Host freezes Game callbacks for stop.
            // There is no plugin function pointer, timer delegate, or detour on the actor.
            ue_.Set(actor, "Actor.SetLifeSpan", "InLifespan", LeaseSeconds);
            ue_.Set(actor, "Actor.SetReplicates", "bInReplicates", false);
            ue_.Set(actor, "Actor.SetReplicateMovement", "bInReplicateMovement", false);
            ue_.Set(actor, "Actor.SetActorEnableCollision", "bNewActorEnableCollision", false);
            const auto finish = ue_.Call(
                "/Script/Engine.GameplayStatics.FinishSpawningActor", statics,
                [&](Engine::Parameters& p) {
                    p.Put("Actor", actor);
                    p.Put("SpawnTransform", transform);
                    p.Put("TransformScaleMethod", std::uint8_t{0});
                });
            if (finish.Get<Address>("ReturnValue") != actor)
                throw std::runtime_error(localizer.Text("error.spawn_incomplete",
                    "Model construction did not complete"));
            const auto body = ue_.ObjectField(actor, "SkeletalMeshComponent");
            Configure(body, appearance.parts.front().mesh);
            Playback playback{ue_.Pin(body), 0, appearance.duration};
            if (appearance.idle) {
                StartIdle(body, appearance.idle);
                record.animation = "loop";
            } else {
                record.animation = appearance.animation;
            }
            // This component IS the Actor root. The appearance RelativeLocation
            // belongs to the game's capsule, not to this capsule-free model.
            // Applying it here would move Actor.Location behind the user's back.
            const auto component_class = ue_.Find("/Script/Engine.SkeletalMeshComponent");
            for (std::size_t i = 1; i < appearance.parts.size(); ++i) {
                const auto& part = appearance.parts[i];
                const auto added = ue_.Call("/Script/Engine.Actor.AddComponentByClass", actor,
                    [&](Engine::Parameters& p) {
                        p.Put("Class", component_class);
                        p.Put("bManualAttachment", true);
                        p.Put("RelativeTransform", part.transform);
                        p.Put("bDeferredFinish", false);
                    }).Get<Address>("ReturnValue");
                if (!added)
                    throw std::runtime_error(localizer.Text("error.attached_mesh_failed",
                        "Creating an attached model mesh failed"));
                Configure(added, part.mesh);
                const auto attached = ue_.Call(
                    "/Script/Engine.SceneComponent.K2_AttachToComponent", added,
                    [&](Engine::Parameters& p) {
                        p.Put("Parent", body);
                        p.Put("SocketName", part.socket);
                        p.Put("LocationRule", std::uint8_t{0}); // KeepRelative.
                        p.Put("RotationRule", std::uint8_t{0});
                        p.Put("ScaleRule", std::uint8_t{0});
                        p.Put("bWeldSimulatedBodies", false);
                    }).Get<bool>("ReturnValue");
                if (!attached)
                    throw std::runtime_error(localizer.Text("error.attached_mesh_attach_failed",
                        "Attaching an attached model mesh failed"));
                Relative(added, part.transform);
                if (part.leader || (!part.socket.index &&
                    ue_.ObjectField(part.mesh, "Skeleton") ==
                    ue_.ObjectField(appearance.parts.front().mesh, "Skeleton"))) {
                    ue_.Call("/Script/Engine.SkinnedMeshComponent.SetLeaderPoseComponent", added,
                        [&](Engine::Parameters& p) {
                            p.Put("NewLeaderBoneComponent", body);
                            p.Put("bForceUpdate", true);
                            p.Put("bInFollowerShouldTickPose", false);
                        });
                }
            }
            // Actor setters are checked after construction; BeginPlay cannot silently
            // leave a replicated or colliding shell in a successful result.
            ue_.Set(actor, "Actor.SetReplicates", "bInReplicates", false);
            ue_.Set(actor, "Actor.SetActorEnableCollision", "bNewActorEnableCollision", false);
            VerifyLocation(actor, position);
            const auto facing = ue_.Call(
                "/Script/Engine.SceneComponent.K2_GetComponentRotation", body)
                .Get<Vector>("ReturnValue");
            if (!std::isfinite(facing.y) ||
                std::abs(std::remainder(facing.y - yaw, 360.0)) > 0.05)
                throw std::runtime_error(localizer.Text("error.facing_mismatch",
                    "The model body facing does not match after reading it back"));
            if (appearance.idle) playback_.emplace(record.id, playback);
            models_.push_back(std::move(record)); // no allocation after reserve above
            return next_id_++;
        } catch (...) {
            try { ue_.Call("/Script/Engine.Actor.K2_DestroyActor", actor); } catch (...) {}
            throw;
        }
    }
    bool Destroy(std::uint64_t id) {
        const auto found = std::find_if(models_.begin(), models_.end(),
            [&](const Model& model) { return model.id == id; });
        if (found == models_.end()) return false;
        if (!Same(found->world, world_))
            throw std::runtime_error(localizer.Text("error.model_other_world",
                "The model belongs to another world"));
        if (const auto actor = ue_.Resolve(found->actor)) {
            ue_.Call("/Script/Engine.Actor.K2_DestroyActor", actor);
        }
        // K2_DestroyActor is latent. Once accepted, retire the UI record and
        // animation immediately instead of waiting for destruction flags or GC.
        // Stop renewing the existing native lifespan as well.
        playback_.erase(id);
        models_.erase(found);
        return true;
    }
    void DestroyAll() {
        const auto copy = models_;
        for (const auto& model : copy) Destroy(model.id);
    }
    void Move(std::uint64_t id, Vector position, double yaw) {
        if (!position.finite() || !std::isfinite(yaw))
            throw std::runtime_error(localizer.Text("error.transform_not_finite",
                "The position and angle must be finite numbers"));
        auto found = std::find_if(models_.begin(), models_.end(),
            [&](const Model& model) { return model.id == id; });
        if (found == models_.end() || !Same(found->world, world_))
            throw std::runtime_error(localizer.Text("error.model_missing",
                "The model does not exist or the world changed"));
        const auto actor = ue_.Resolve(found->actor);
        if (!actor)
            throw std::runtime_error(
                localizer.Text("error.model_destroyed", "The model was already destroyed"));
        const auto moved = ue_.Call(
            "/Script/Engine.Actor.K2_SetActorLocationAndRotation", actor,
            [&](Engine::Parameters& p) {
                p.Put("NewLocation", position);
                p.Put("NewRotation", Vector{0, yaw, 0});
                p.Put("bSweep", false);
                p.Put("bTeleport", true);
            }).Get<bool>("ReturnValue");
        if (!moved)
            throw std::runtime_error(
                localizer.Text("error.move_rejected", "The engine rejected moving the model"));
        VerifyLocation(actor, position);
        found->position = position;
        found->yaw = yaw;
    }
    void Renew() {
        std::erase_if(models_, [&](const Model& model) {
            if (!Same(model.world, world_)) return true;
            const auto actor = ue_.Resolve(model.actor);
            return !actor || ue_.Call("/Script/Engine.Actor.IsActorBeingDestroyed", actor)
                                 .Get<bool>("ReturnValue");
        });
        for (const auto& model : models_) {
            if (const auto actor = ue_.Resolve(model.actor))
                ue_.Set(actor, "Actor.SetLifeSpan", "InLifespan", LeaseSeconds);
        }
        std::erase_if(playback_, [&](const auto& item) {
            return std::none_of(models_.begin(), models_.end(),
                [&](const Model& model) { return model.id == item.first; });
        });
    }
    void Animate(double delta) {
        if (!std::isfinite(delta) || delta <= 0) return;
        for (auto& [id, playback] : playback_) {
            static_cast<void>(id);
            const auto body = ue_.Resolve(playback.component);
            if (!body) continue;
            // SingleNode evaluates the sampled pose in its normal component tick.
            // Explicit time advancement loops without dispatching game AnimNotifies.
            playback.time = std::fmod(playback.time + delta, playback.duration);
            Position(body, static_cast<float>(playback.time));
        }
    }
private:
    struct Playback {
        Handle component{};
        double time{};
        float duration{};
    };
    static Transform At(Vector position, double yaw) {
        Transform transform;
        const auto half = yaw * 3.14159265358979323846 / 360.0;
        transform.rotation[2] = std::sin(half);
        transform.rotation[3] = std::cos(half);
        transform.translation = position;
        return transform;
    }
    void Preflight() {
        for (const auto& [path, unused] : ue_.contracts()) {
            static_cast<void>(unused);
            ue_.Bind(path);
        }
    }
    Address LoadAppearance(Address soft, Address type, std::string_view field) {
        const auto property = ue_.Expect(type, field, "SoftObjectProperty", ue_.data_layout().soft_object_size);
        const auto asset = SoftAsset(soft + property.offset);
        if (!ue_.IsA(asset, "HTPlayerAppearance"))
            throw std::runtime_error(localizer.Text("error.appearance_unavailable",
                "This character has no usable local appearance asset"));
        return asset;
    }
    Appearance ReadAppearance(Name id) {
        // Reacquire expired metadata handles; model assets still load only here.
        if (!ue_.Resolve(default_table_) && !ue_.Resolve(appearance_table_)) {
            const auto library = ue_.Find("/Script/HTGame.Default__HTGameData");
            const auto data = ue_.Call("/Script/HTGame.HTGameData.GetGameData", library)
                                  .Get<Address>("ReturnValue");
            const auto asset = ue_.ObjectField(data, "CharacterDataAsset");
            if (!ue_.IsA(asset, "CharacterDataAsset"))
                throw std::runtime_error(localizer.Text("error.appearance_not_ready",
                    "The appearance catalog is not ready; refresh once the character data has loaded"));
            default_table_ = ue_.Pin(ue_.ObjectField(asset, "DefaultAppearanceDataTable"));
            appearance_table_ = ue_.Pin(ue_.ObjectField(asset, "AppearanceDataTable"));
        }
        Address appearance{};
        const auto defaults = ue_.Resolve(default_table_);
        if (defaults) {
            const auto rows = ue_.Rows(defaults, "StaticDefaultAppearanceData");
            if (const auto row = rows.rows.find(Key(id)); row != rows.rows.end())
                appearance = LoadAppearance(row->second, rows.type, "PlayerAppearanceAsset");
        }
        if (!appearance) {
            const auto character_table = ue_.Resolve(character_table_);
            const auto appearance_table = ue_.Resolve(appearance_table_);
            if (!character_table || !appearance_table)
                throw std::runtime_error(localizer.Text("error.character_tables_stale",
                    "The character resource tables are no longer valid; refresh"));
            const auto rows = ue_.Rows(character_table, "StaticItemData");
            const auto item = rows.rows.find(Key(id));
            if (item == rows.rows.end())
                throw std::runtime_error(localizer.Text("error.character_missing",
                    "The character is not in the resource catalog"));
            const auto character = ue_.Instanced(item->second, rows.type,
                                                 "ElementData", "CharacterItemData");
            const auto fashion = ue_.Value<Name>(character.data, character.type,
                                                 "DefaultFashionID", "NameProperty");
            const auto fashions = ue_.Rows(appearance_table, "StaticAppearanceData");
            const auto row = fashions.rows.find(Key(fashion));
            if (!fashion.index || row == fashions.rows.end())
                throw std::runtime_error(localizer.Text("error.default_appearance_missing",
                    "The character's default appearance is missing, so no empty model was created"));
            const auto actual = ue_.Value<Name>(row->second, fashions.type,
                                                "CharacterID", "NameProperty");
            if (actual != id)
                throw std::runtime_error(localizer.Text("error.appearance_id_mismatch",
                    "The appearance does not match the character id"));
            const auto data = ue_.Instanced(row->second, fashions.type,
                                            "AppearanceData", "FashionData");
            appearance = LoadAppearance(data.data, data.type, "PlayerAppearanceAsset");
        }
        Appearance recipe;
        const auto type = ue_.Class(appearance);
        const auto mesh_data = ue_.ExpectStruct(type, "FashionMeshData", "CharacterMeshData");
        const auto mesh_field = ue_.ExpectMember(mesh_data, "CharacterMesh", "ObjectProperty", sizeof(Address));
        Part body;
        body.mesh = ue_.Read<Address>(appearance + mesh_data.offset + mesh_field.offset);
        if (!ue_.IsA(body.mesh, "SkeletalMesh"))
            throw std::runtime_error(localizer.Text("error.body_mesh_missing",
                "The character's main mesh is missing, so no empty model was created"));
        recipe.parts.push_back(body);
        ReadIdle(appearance + mesh_data.offset, mesh_data, body.mesh, recipe);
        const auto& layout = ue_.data_layout();
        const auto attached = ue_.Expect(type, "ArrayFashionAttachedMeshData", "ArrayProperty", layout.array_size);
        const auto inner = ue_.Layout<Address>(attached.field, "farrayProperty.inner");
        const auto inner_type = ue_.Layout<Address>(inner, "fstructProperty.struct");
        const auto stride = ue_.Layout<std::int32_t>(inner, "fproperty.elementSize");
        const auto array = appearance + attached.offset;
        const auto count = ue_.Read<std::int32_t>(array + layout.array_count);
        const auto capacity = ue_.Read<std::int32_t>(array + layout.array_capacity);
        const auto data = ue_.Read<Address>(array + layout.array_data);
        if (!inner_type || ue_.ObjectName(inner_type) != "AttachedMeshData" ||
            stride <= 0 || stride > 65536 || count < 0 || count > 4096 ||
            capacity < count || (count && !data))
            throw std::runtime_error(localizer.Text("error.attached_array_layout",
                "The attached mesh array layout does not match"));
        for (std::int32_t i = 0; i < count; ++i) {
            const auto item = data + std::size_t(i) * stride;
            Part part;
            part.mesh = ue_.Value<Address>(item, inner_type, "CharacterMesh", "ObjectProperty");
            if (!part.mesh) continue; // Authored optional component.
            if (!ue_.IsA(part.mesh, "SkeletalMesh"))
                throw std::runtime_error(localizer.Text("error.attached_not_skeletal",
                    "The attached asset is not a skeletal mesh"));
            part.socket = ue_.Value<Name>(item, inner_type, "SocketName", "NameProperty");
            part.transform = At(
                ue_.Value<Vector>(item, inner_type, "RelativeLocation", "StructProperty"), 0);
            const auto rotation = ue_.Value<Vector>(item, inner_type,
                                                    "RelativeRotation", "StructProperty");
            part.transform = Rotation(part.transform, rotation);
            part.transform.scale = ue_.Value<Vector>(item, inner_type,
                                                     "RelativeScale3D", "StructProperty");
            if (!part.transform.translation.finite() || !rotation.finite() ||
                !part.transform.scale.finite())
                throw std::runtime_error(localizer.Text("error.attached_transform_invalid",
                    "An attached mesh transform is not valid"));
            recipe.parts.push_back(part);
        }
        // Facial/secondary meshes are authored as a separate sparse map.
        const auto child = ue_.Expect(type, "ChildMeshDataMap", "MapProperty", layout.map_size);
        if (static_cast<std::size_t>(mesh_data.size) > layout.child_stride - layout.child_value)
            throw std::runtime_error(localizer.Text("error.child_mesh_type",
                "The child mesh asset type does not match"));
        for (const auto item : ue_.Sparse(appearance + child.offset, layout.child_stride)) {
            Part part;
            part.mesh = ue_.Read<Address>(item + layout.child_value + mesh_field.offset);
            if (!part.mesh) continue;
            if (!ue_.IsA(part.mesh, "SkeletalMesh"))
                throw std::runtime_error(localizer.Text("error.child_mesh_type",
                    "The child mesh asset type does not match"));
            part.leader = true;
            recipe.parts.push_back(part);
        }
        return recipe;
    }
    static Transform Rotation(Transform transform, Vector degrees) {
        // UE FRotator: Pitch, Yaw, Roll, in degrees; match its quaternion handedness.
        const auto factor = 3.14159265358979323846 / 360.0;
        const auto sp = std::sin(degrees.x * factor), cp = std::cos(degrees.x * factor);
        const auto sy = std::sin(degrees.y * factor), cy = std::cos(degrees.y * factor);
        const auto sr = std::sin(degrees.z * factor), cr = std::cos(degrees.z * factor);
        transform.rotation[0] = cr * sp * sy - sr * cp * cy;
        transform.rotation[1] = -cr * sp * cy - sr * cp * sy;
        transform.rotation[2] = cr * cp * sy - sr * sp * cy;
        transform.rotation[3] = cr * cp * cy + sr * sp * sy;
        return transform;
    }
    void Configure(Address component, Address mesh) {
        if (!ue_.IsA(component, "SkeletalMeshComponent"))
            throw std::runtime_error(localizer.Text("error.component_type",
                "The model component type does not match"));
        ue_.Set(component, "PrimitiveComponent.SetSimulatePhysics", "bSimulate", false);
        ue_.Set(component, "PrimitiveComponent.SetCollisionEnabled", "NewType", std::uint8_t{0});
        // Neither the mesh's post-process BP nor a game UHTAnimInstance may run
        // on this capsule-free actor: both can assume a gameplay-character owner.
        ue_.Set(component, "SkeletalMeshComponent.SetDisablePostProcessBlueprint",
                "bInDisablePostProcess", true);
        ue_.Call("/Script/Engine.SkeletalMeshComponent.SetAnimationMode", component,
            [](Engine::Parameters& p) {
                p.Put("InAnimationMode", std::uint8_t{2}); // AnimationCustomMode.
                p.Put("bForceInitAnimScriptInstance", false);
            });
        ue_.Set(component, "SkeletalMeshComponent.SetSkeletalMeshAsset", "NewMesh", mesh);
        const auto got = ue_.Call("/Script/Engine.SkeletalMeshComponent.GetSkeletalMeshAsset",
                                  component).Get<Address>("ReturnValue");
        if (got != mesh)
            throw std::runtime_error(localizer.Text("error.mesh_verify_failed",
                "The mesh failed validation after construction"));
    }
    Address ClassDefault(Address type, std::string_view base) {
        if (!type || !ue_.Derived(type, base)) return 0;
        const auto object = ue_.Layout<Address>(type, "uclass.classDefaultObject");
        return ue_.IsA(object, base) ? object : 0;
    }
    Address SoftAsset(Address address) {
        std::vector<std::uint8_t> soft(ue_.data_layout().soft_object_size);
        ue_.Bytes(address, soft.data(), soft.size());
        return ue_.Call("/Script/Engine.KismetSystemLibrary.LoadAsset_Blocking",
            ue_.Find("/Script/Engine.Default__KismetSystemLibrary"),
            [&](Engine::Parameters& p) { p.PutBytes("Asset", soft.data(), soft.size()); }).Get<Address>("ReturnValue");
    }
    bool IdleCandidate(Address sequence, Address mesh, Appearance& recipe) {
        if (!ue_.IsA(sequence, "AnimSequence")) return false;
        const auto skeleton = ue_.ObjectField(mesh, "Skeleton");
        if (!ue_.IsA(skeleton, "Skeleton") ||
            ue_.ObjectField(sequence, "Skeleton") != skeleton)
            return false; // Never retarget an arbitrary character's clip.
        const auto length = ue_.Value<float>(sequence, ue_.Class(sequence),
                                            "SequenceLength", "FloatProperty");
        if (!std::isfinite(length) || length <= 1.0f / 30.0f) return false;
        recipe.idle = sequence;
        recipe.duration = length;
        return true;
    }
    void ReadIdle(Address mesh_data, const Property& type, Address mesh, Appearance& recipe) {
        const auto anim = ue_.ExpectMember(type, "AnimInstance", "ClassProperty", sizeof(Address));
        const auto cls = ue_.Read<Address>(mesh_data + anim.offset);
        const auto defaults = ClassDefault(cls, "HTAnimInstance");
        if (!defaults) return;
        // These are authored resources on CDOs, not initialized gameplay instances.
        // Read only; do not call any HT initialization / layer / character method.
        const auto layer_cls = ue_.Value<Address>(defaults, ue_.Class(defaults),
                                                  "HTNotMovingAnimLayer", "ClassProperty");
        if (const auto layer = ClassDefault(layer_cls, "HTNotMovingAnimLayer")) {
            const auto assets = ue_.ExpectStruct(ue_.Class(layer), "NotMovingAnimAssets",
                                                 "NotMovingAnimAssets");
            const auto data = layer + assets.offset;
            const auto idle = ue_.ExpectMember(assets, "base_stand_pe",
                                               "ObjectProperty", sizeof(Address));
            if (IdleCandidate(ue_.Read<Address>(data + idle.offset), mesh, recipe)) return;
            const auto exclusive = ue_.ExpectMember(assets, "base_stand_exclusive",
                                                     "SoftObjectProperty", ue_.data_layout().soft_object_size);
            if (IdleCandidate(SoftAsset(data + exclusive.offset), mesh, recipe)) return;
        }
        if (ue_.IsA(defaults, "HTPlayerAnimInstance")) {
            const auto assets = ue_.ExpectStruct(ue_.Class(defaults), "PlayerAnims",
                                                 "HTPlayerAnim");
            const auto idle = ue_.ExpectMember(assets, "base_stand_pe",
                                               "ObjectProperty", sizeof(Address));
            IdleCandidate(ue_.Read<Address>(defaults + assets.offset + idle.offset), mesh, recipe);
        }
    }
    void StartIdle(Address component, Address sequence) {
        ue_.Call("/Script/Engine.SkeletalMeshComponent.SetAnimationMode", component,
            [](Engine::Parameters& p) {
                p.Put("InAnimationMode", std::uint8_t{1}); // AnimationSingleNode.
                p.Put("bForceInitAnimScriptInstance", true);
            });
        ue_.Call("/Script/Engine.SkeletalMeshComponent.OverrideAnimationData", component,
            [&](Engine::Parameters& p) {
                p.Put("InAnimToPlay", sequence);
                p.Put("bIsLooping", true);
                // Our Game callback advances time with bFireNotifies=false.
                // Native auto-playing would run gameplay notifies on this model.
                p.Put("bIsPlaying", false);
                p.Put("position", 0.0f);
                p.Put("PlayRate", 1.0f);
            });
        // OverrideAnimationData writes the saved component data; initialize the
        // live SingleNode asset explicitly as well, without native auto-play.
        ue_.Set(component, "SkeletalMeshComponent.SetAnimation", "NewAnimToPlay", sequence);
        const auto instance = ue_.Call("/Script/Engine.SkeletalMeshComponent.GetAnimInstance",
                                       component).Get<Address>("ReturnValue");
        if (!ue_.IsA(instance, "AnimSingleNodeInstance"))
            throw std::runtime_error(localizer.Text("error.idle_instance_missing",
                "Idle initialization failed: no valid SingleNode instance"));
        ue_.Set(instance, "AnimSingleNodeInstance.SetPlaying", "bIsPlaying", false);
        ue_.Set(instance, "AnimSingleNodeInstance.SetLooping", "bIsLooping", true);
        ue_.Set(instance, "AnimSingleNodeInstance.SetPlayRate", "InPlayRate", 1.0f);
        if (ue_.Call("/Script/Engine.AnimSingleNodeInstance.GetAnimationAsset", instance)
                .Get<Address>("ReturnValue") != sequence)
            throw std::runtime_error(localizer.Text("error.idle_rejected",
                "Idle initialization failed: the instance did not accept this character's animation"));
        Position(component, 0);
    }
    void Position(Address component, float time) {
        ue_.Call("/Script/Engine.SkeletalMeshComponent.SetPosition", component,
            [&](Engine::Parameters& p) {
                p.Put("InPos", time);
                p.Put("bFireNotifies", false);
            });
    }
    void Relative(Address component, const Transform& transform) {
        ue_.Call("/Script/Engine.SceneComponent.K2_SetRelativeTransform", component,
            [&](Engine::Parameters& p) {
                p.Put("NewTransform", transform);
                p.Put("bSweep", false);
                p.Put("bTeleport", true);
            });
    }
    void VerifyLocation(Address actor, Vector expected) {
        const auto actual = ue_.Call("/Script/Engine.Actor.K2_GetActorLocation", actor)
                                .Get<Vector>("ReturnValue");
        if (!actual.finite() || std::abs(actual.x - expected.x) > 0.05 ||
            std::abs(actual.y - expected.y) > 0.05 || std::abs(actual.z - expected.z) > 0.05)
            throw std::runtime_error(localizer.Text("error.location_mismatch",
                "The model position does not match after reading it back"));
    }
    Engine& ue_;
    Handle world_{}, character_table_{}, default_table_{}, appearance_table_{};
    std::vector<Character> characters_;
    std::vector<Model> models_;
    std::map<std::uint64_t, Playback> playback_;
    std::uint64_t next_id_{1};
    const anomaly::sdk::Localizer& localizer;
};
} // namespace character_studio
