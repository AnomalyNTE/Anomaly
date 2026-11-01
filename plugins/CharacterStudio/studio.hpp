#pragma once
#include "ue.hpp"

namespace character_studio {
inline constexpr float LeaseSeconds = 5.0f;
inline constexpr double RenewSeconds = 1.0;

struct Character {
    Name id{};
    std::string code, chinese;
    bool named{};
};
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
    std::string animation{"参考姿势：资源中没有兼容的待机序列"};
};

// Game-domain owner. Render sees copies of Character/Model, never UE addresses.
class Studio {
public:
    explicit Studio(Engine& engine) : ue_(engine) {}
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
        if (!ue_.Resolve(world_)) throw std::runtime_error("等待大世界就绪");
        const auto library = ue_.Find("/Script/HTGame.Default__HTGameData");
        const auto data = ue_.Call("/Script/HTGame.HTGameData.GetGameData", library)
                              .Get<Address>("ReturnValue");
        if (!ue_.IsA(data, "HTGameData")) throw std::runtime_error("角色数据服务尚未就绪");
        const auto table = ue_.ObjectField(data, "CharacterDataTable");
        const auto rows = ue_.Rows(table, "StaticItemData");
        if (rows.rows.empty()) throw std::runtime_error("角色数据表尚未加载，请刷新完整角色目录");
        const auto text = ue_.Expect(rows.type, "ItemName", "TextProperty", 16);
        // A whole catalog is published atomically. No inventory, ownership, unlock,
        // IsCharacterShow, time-gate, or loaded-Blueprint filter is applied.
        std::vector<Character> next;
        next.reserve(rows.rows.size());
        for (const auto& [key, row] : rows.rows) {
            Character item;
            item.id = {static_cast<std::uint32_t>(key),
                       static_cast<std::uint32_t>(key >> 32)};
            item.code = ue_.Spelling(item.id);
            item.chinese = ue_.Text(row + text.offset);
            item.named = !item.chinese.empty();
            if (!item.named) item.chinese = "中文名待加载";
            next.push_back(std::move(item));
        }
        std::sort(next.begin(), next.end(), [](const Character& a, const Character& b) {
            return a.code < b.code;
        });
        const auto character_handle = ue_.Pin(table);
        if (!character_handle.id)
            throw std::runtime_error("角色目录对象身份校验失败");
        character_table_ = character_handle;
        default_table_ = appearance_table_ = {};
        characters_ = std::move(next);
    }
    std::uint64_t Spawn(Name character, Vector position, double yaw) {
        if (!position.finite() || !std::isfinite(yaw))
            throw std::runtime_error("坐标与角度必须是有限数值");
        const auto chosen = std::find_if(characters_.begin(), characters_.end(),
            [&](const Character& item) { return item.id == character; });
        if (chosen == characters_.end()) throw std::runtime_error("角色目录已刷新，请重新选择");
        const auto world = ue_.Resolve(world_);
        if (!world) throw std::runtime_error("世界已失效");
        // All resource loading and all function validation precede allocation.
        const auto appearance = ReadAppearance(character);
        Preflight();
        const auto cls = ue_.Find("/Script/Engine.SkeletalMeshActor");
        const auto statics = ue_.Find("/Script/Engine.Default__GameplayStatics");
        if (!ue_.Derived(cls, "SkeletalMeshActor"))
            throw std::runtime_error("模型 Actor 类校验失败");
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
        if (!actor) throw std::runtime_error("引擎未创建模型");
        try {
            record.actor = ue_.Pin(actor);
            if (!record.actor.id) throw std::runtime_error("新模型身份校验失败");
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
                throw std::runtime_error("模型构建未完成");
            const auto body = ue_.ObjectField(actor, "SkeletalMeshComponent");
            Configure(body, appearance.parts.front().mesh);
            Playback playback{ue_.Pin(body), 0, appearance.duration};
            if (appearance.idle) {
                StartIdle(body, appearance.idle);
                record.animation = "循环待机：" + ue_.ObjectName(appearance.idle);
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
                if (!added) throw std::runtime_error("模型附属网格创建失败");
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
                if (!attached) throw std::runtime_error("模型附属网格挂接失败");
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
                throw std::runtime_error("模型身体朝向回读不一致");
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
        if (!Same(found->world, world_)) throw std::runtime_error("模型属于其他世界");
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
            throw std::runtime_error("坐标与角度必须是有限数值");
        auto found = std::find_if(models_.begin(), models_.end(),
            [&](const Model& model) { return model.id == id; });
        if (found == models_.end() || !Same(found->world, world_))
            throw std::runtime_error("模型不存在或世界已变更");
        const auto actor = ue_.Resolve(found->actor);
        if (!actor) throw std::runtime_error("模型已销毁");
        const auto moved = ue_.Call(
            "/Script/Engine.Actor.K2_SetActorLocationAndRotation", actor,
            [&](Engine::Parameters& p) {
                p.Put("NewLocation", position);
                p.Put("NewRotation", Vector{0, yaw, 0});
                p.Put("bSweep", false);
                p.Put("bTeleport", true);
            }).Get<bool>("ReturnValue");
        if (!moved) throw std::runtime_error("引擎拒绝移动模型");
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
        for (const auto& [path, unused] : ue_.resource().at("functions").items()) {
            static_cast<void>(unused);
            ue_.Bind(path);
        }
    }
    Address LoadAppearance(Address soft, Address type, std::string_view field) {
        const auto property = ue_.Expect(type, field, "SoftObjectProperty", 40);
        std::array<std::uint8_t, 40> reference{};
        ue_.Bytes(soft + property.offset, reference.data(), reference.size());
        const auto library = ue_.Find("/Script/Engine.Default__KismetSystemLibrary");
        const auto asset = ue_.Call("/Script/Engine.KismetSystemLibrary.LoadAsset_Blocking",
            library, [&](Engine::Parameters& p) { p.Put("Asset", reference); })
            .Get<Address>("ReturnValue");
        if (!ue_.IsA(asset, "HTPlayerAppearance"))
            throw std::runtime_error("该角色没有可用的本地外观资源");
        return asset;
    }
    Appearance ReadAppearance(Name id) {
        // Listing names must not depend on appearance assets being resident.
        // Resolve these only when the user actually requests a model.
        if (!ue_.Resolve(default_table_) && !ue_.Resolve(appearance_table_)) {
            const auto library = ue_.Find("/Script/HTGame.Default__HTGameData");
            const auto data = ue_.Call("/Script/HTGame.HTGameData.GetGameData", library)
                                  .Get<Address>("ReturnValue");
            const auto asset = ue_.ObjectField(data, "CharacterDataAsset");
            if (!ue_.IsA(asset, "CharacterDataAsset"))
                throw std::runtime_error("外观资源尚未就绪，角色目录仍可浏览");
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
                throw std::runtime_error("角色资源表已失效，请刷新");
            const auto rows = ue_.Rows(character_table, "StaticItemData");
            const auto item = rows.rows.find(Key(id));
            if (item == rows.rows.end()) throw std::runtime_error("角色不在资源目录");
            const auto character = ue_.Instanced(item->second, rows.type,
                                                 "ElementData", "CharacterItemData");
            const auto fashion = ue_.Value<Name>(character.data, character.type,
                                                 "DefaultFashionID", "NameProperty");
            const auto fashions = ue_.Rows(appearance_table, "StaticAppearanceData");
            const auto row = fashions.rows.find(Key(fashion));
            if (!fashion.index || row == fashions.rows.end())
                throw std::runtime_error("角色默认外观缺失，未生成空模型");
            const auto actual = ue_.Value<Name>(row->second, fashions.type,
                                                "CharacterID", "NameProperty");
            if (actual != id) throw std::runtime_error("外观与角色 ID 不一致");
            const auto data = ue_.Instanced(row->second, fashions.type,
                                            "AppearanceData", "FashionData");
            appearance = LoadAppearance(data.data, data.type, "PlayerAppearanceAsset");
        }
        Appearance recipe;
        const auto type = ue_.Class(appearance);
        const auto mesh_data = ue_.Expect(type, "FashionMeshData", "StructProperty",
                                          16, "CharacterMeshData");
        Part body;
        body.mesh = ue_.Value<Address>(appearance + mesh_data.offset, mesh_data.structure,
                                       "CharacterMesh", "ObjectProperty");
        if (!ue_.IsA(body.mesh, "SkeletalMesh"))
            throw std::runtime_error("角色主网格缺失，未生成空模型");
        recipe.parts.push_back(body);
        ReadIdle(appearance + mesh_data.offset, mesh_data.structure, body.mesh, recipe);
        const auto attached = ue_.Expect(type, "ArrayFashionAttachedMeshData", "ArrayProperty", 16);
        const auto inner = ue_.Layout<Address>(attached.field, "farrayProperty.inner");
        const auto inner_type = ue_.Layout<Address>(inner, "fstructProperty.struct");
        const auto stride = ue_.Layout<std::int32_t>(inner, "fproperty.elementSize");
        const auto array = appearance + attached.offset;
        const auto count = ue_.Read<std::int32_t>(array + 8);
        const auto capacity = ue_.Read<std::int32_t>(array + 12);
        const auto data = ue_.Read<Address>(array);
        if (!inner_type || ue_.ObjectName(inner_type) != "AttachedMeshData" ||
            stride <= 0 || stride > 65536 || count < 0 || count > 4096 ||
            capacity < count || (count && !data))
            throw std::runtime_error("附属网格数组布局不匹配");
        for (std::int32_t i = 0; i < count; ++i) {
            const auto item = data + std::size_t(i) * stride;
            Part part;
            part.mesh = ue_.Value<Address>(item, inner_type, "CharacterMesh", "ObjectProperty");
            if (!part.mesh) continue; // Authored optional component.
            if (!ue_.IsA(part.mesh, "SkeletalMesh"))
                throw std::runtime_error("附属资源不是骨骼网格");
            part.socket = ue_.Value<Name>(item, inner_type, "SocketName", "NameProperty");
            part.transform = At(
                ue_.Value<Vector>(item, inner_type, "RelativeLocation", "StructProperty"), 0);
            const auto rotation = ue_.Value<Vector>(item, inner_type,
                                                    "RelativeRotation", "StructProperty");
            part.transform = Rotation(part.transform, rotation);
            part.transform.scale = ue_.Value<Vector>(item, inner_type,
                                                     "RelativeScale3D", "StructProperty");
            if (!part.transform.translation.finite() || !rotation.finite() ||
                !part.transform.scale.finite()) throw std::runtime_error("附属网格变换不合法");
            recipe.parts.push_back(part);
        }
        // Facial/secondary meshes are authored as a separate sparse map.
        const auto child = ue_.Expect(type, "ChildMeshDataMap", "MapProperty", 80);
        for (const auto item : ue_.Sparse(appearance + child.offset, 32)) {
            Part part;
            part.mesh = ue_.Value<Address>(item + 8, mesh_data.structure,
                                           "CharacterMesh", "ObjectProperty");
            if (!part.mesh) continue;
            if (!ue_.IsA(part.mesh, "SkeletalMesh"))
                throw std::runtime_error("子网格资源类型不匹配");
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
            throw std::runtime_error("模型组件类型不匹配");
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
        if (got != mesh) throw std::runtime_error("网格构建后校验失败");
    }
    Address ClassDefault(Address type, std::string_view base) {
        if (!type || !ue_.Derived(type, base)) return 0;
        const auto object = ue_.Layout<Address>(type, "uclass.classDefaultObject");
        return ue_.IsA(object, base) ? object : 0;
    }
    Address SoftAsset(Address address) {
        std::array<std::uint8_t, 40> soft{};
        ue_.Bytes(address, soft.data(), soft.size());
        return ue_.Call("/Script/Engine.KismetSystemLibrary.LoadAsset_Blocking",
            ue_.Find("/Script/Engine.Default__KismetSystemLibrary"),
            [&](Engine::Parameters& p) { p.Put("Asset", soft); }).Get<Address>("ReturnValue");
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
    void ReadIdle(Address mesh_data, Address type, Address mesh, Appearance& recipe) {
        const auto cls = ue_.Value<Address>(mesh_data, type, "AnimInstance", "ClassProperty");
        const auto defaults = ClassDefault(cls, "HTAnimInstance");
        if (!defaults) return;
        // These are authored resources on CDOs, not initialized gameplay instances.
        // Read only; do not call any HT initialization / layer / character method.
        const auto layer_cls = ue_.Value<Address>(defaults, ue_.Class(defaults),
                                                  "HTNotMovingAnimLayer", "ClassProperty");
        if (const auto layer = ClassDefault(layer_cls, "HTNotMovingAnimLayer")) {
            const auto assets = ue_.Expect(ue_.Class(layer), "NotMovingAnimAssets",
                                           "StructProperty", 184, "NotMovingAnimAssets");
            const auto data = layer + assets.offset;
            if (IdleCandidate(ue_.Value<Address>(data, assets.structure,
                              "base_stand_pe", "ObjectProperty"), mesh, recipe)) return;
            const auto exclusive = ue_.Expect(assets.structure, "base_stand_exclusive",
                                                "SoftObjectProperty", 40);
            if (IdleCandidate(SoftAsset(data + exclusive.offset), mesh, recipe)) return;
        }
        if (ue_.IsA(defaults, "HTPlayerAnimInstance")) {
            const auto assets = ue_.Expect(ue_.Class(defaults), "PlayerAnims",
                                           "StructProperty", 2112, "HTPlayerAnim");
            IdleCandidate(ue_.Value<Address>(defaults + assets.offset, assets.structure,
                          "base_stand_pe", "ObjectProperty"), mesh, recipe);
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
            throw std::runtime_error("待机初始化失败：没有有效的 SingleNode 实例");
        ue_.Set(instance, "AnimSingleNodeInstance.SetPlaying", "bIsPlaying", false);
        ue_.Set(instance, "AnimSingleNodeInstance.SetLooping", "bIsLooping", true);
        ue_.Set(instance, "AnimSingleNodeInstance.SetPlayRate", "InPlayRate", 1.0f);
        if (ue_.Call("/Script/Engine.AnimSingleNodeInstance.GetAnimationAsset", instance)
                .Get<Address>("ReturnValue") != sequence)
            throw std::runtime_error("待机初始化失败：实例未接受该角色动画");
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
            throw std::runtime_error("模型位置回读不一致");
    }
    Engine& ue_;
    Handle world_{}, character_table_{}, default_table_{}, appearance_table_{};
    std::vector<Character> characters_;
    std::vector<Model> models_;
    std::map<std::uint64_t, Playback> playback_;
    std::uint64_t next_id_{1};
};
} // namespace character_studio
