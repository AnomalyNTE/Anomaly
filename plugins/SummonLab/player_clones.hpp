// Included by plugin.cpp after the common spawn and reflection helpers.

void RemoveOne(std::uint64_t record_id);
void CancelSummonsLocked();
bool SetReflectedBool(std::uintptr_t object, const char* name, std::uint8_t value);
void ANOMALY_CALL CloneBuildDetour(std::uintptr_t owner, std::uintptr_t actor) noexcept;

bool IsCloneAttackAttribute(std::string_view name) {
    return name == "AtkBase" || name == "AtkUp" || name == "AtkAdd" || name == "OnlineAtkRatio";
}

void InitializePlayerClones(const anomaly::sdk::Profile& profile) {
    g_clones.ready = false;
    const auto unavailable = [](std::string_view reason) {
        if (g_context.core && g_context.core->log)
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView(std::string("SummonLab: clone compatibility failed: ") + std::string(reason)));
    };
    try {
        if (!summon_lab::LoadCloneLayout(profile, g_clones.layout)) return unavailable("layout");
        const auto spawn = g_profile_scans.Resolve(g_context.core, profile, "player-clone-spawn");
        const auto target = g_profile_scans.Resolve(g_context.core, profile, "player-clone-target");
        const auto build = g_profile_scans.Resolve(g_context.core, profile, "player-clone-build");
        const auto data = g_profile_scans.Resolve(g_context.core, profile, "player-clone-ability-data");
        const auto copy = g_profile_scans.Resolve(g_context.core, profile, "clone-attribute-copy");
        if (!spawn) return unavailable("player-clone-spawn");
        if (!target) return unavailable("player-clone-target");
        if (!build) return unavailable("player-clone-build");
        if (!data) return unavailable("player-clone-ability-data");
        if (!copy) return unavailable("clone-attribute-copy");
        g_clones.attribute_size = copy.candidate->values.at("attributeSize");
        if (!g_clones.attribute_size || g_clones.attribute_size > 128) return unavailable("attribute ABI");
        g_clones.attribute_copy = copy.address;
        auto& layout = g_clones.layout;
        layout.target = target.candidate->values.at("targetWeak");
        if (layout.target % alignof(std::int32_t) || layout.target > 65536)
            return unavailable("target layout");
        g_clones.spawn = spawn.address;
        g_clones.assign_weak = target.address;
        g_clones.ability_data = data.address;
        if (!g_context.hooks) return unavailable("hook service");
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = build.address;
        request.detour = reinterpret_cast<void*>(&CloneBuildDetour);
        request.label = anomaly::sdk::StringView("summon-player-clone-build");
        auto& hook = g_clones.build_hook;
        if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original,
                &hook.handle).code != ANOMALY_STATUS_V1_OK || !hook.handle.id || !hook.original)
            return unavailable("build hook");
        g_clones.ready = true;
    } catch (const std::out_of_range&) { unavailable("missing profile value"); }
}

std::uintptr_t LoadCloneObject(const char* path) {
    AnomalyGenerationHandleV1 library{};
    if (!FindHandle("/Script/HTGame.Default__HTUtil", &library)) return 0;
    std::array<wchar_t, 512> wide{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
        wide.data(), static_cast<int>(wide.size()));
    if (!length) return 0;
    struct { summon_lab::NativeString path; std::uintptr_t result{}; } parameters{
        {wide.data(), length, length}};
    return InvokeByPath(library, "/Script/HTGame.HTUtil:LoadObjectByPath", &parameters,
        sizeof(parameters)) ? parameters.result : 0;
}

bool CloneTypeNamed(std::uintptr_t type, const char* expected) {
    std::uint32_t name{};
    char text[96]{};
    return ReadValue(type + g_layout.objectName, &name) && ResolveName(name, text, sizeof(text)) &&
        std::strcmp(text, expected) == 0;
}

bool ReadPlayableClones(std::vector<ClassEntry>& entries) {
    const auto table = LoadCloneObject("/Game/DataTable/Character/DT_Character.DT_Character");
    std::uintptr_t row_struct{};
    if (!table || !ReadReflectedValue(table, "RowStruct", "ObjectProperty", &row_struct) ||
        !CloneTypeNamed(row_struct, "StaticItemData")) return false;
    std::vector<std::uintptr_t> rows;
    if (!summon_lab::ReadCloneRows(table + g_clones.layout.row_map, g_clones.layout, ReadBytes, rows))
        return false;
    for (const auto row : rows) {
        struct { std::uintptr_t type{}, memory{}; } element;
        if (!ReadRowValue(row, row_struct, "ElementData", "StructProperty", &element) ||
            !element.type || !element.memory || !CloneTypeNamed(element.type, "CharacterItemData")) return false;
        std::uint8_t is_public{};
        summon_lab::SoftClassRef soft{};
        if (!ReadRowValue(element.memory, element.type, "bIsPublic", "BoolProperty", &is_public) ||
            is_public > 1 || !ReadRowValue(element.memory, element.type,
                "CharacterActorClass", "SoftClassProperty", &soft)) return false;
        if (!is_public) continue;
        std::uint64_t package_name{}, asset_name{};
        summon_lab::NativeString subpath{};
        std::memcpy(&package_name, soft.data() + g_clones.layout.soft_package, sizeof(package_name));
        std::memcpy(&asset_name, soft.data() + g_clones.layout.soft_asset, sizeof(asset_name));
        std::memcpy(&subpath, soft.data() + g_clones.layout.soft_subpath, sizeof(subpath));
        char package[512]{}, asset[128]{};
        if (subpath.count != 0 || (package_name >> 32) || (asset_name >> 32) ||
            !ResolveName(static_cast<std::uint32_t>(package_name), package, sizeof(package)) ||
            !ResolveName(static_cast<std::uint32_t>(asset_name), asset, sizeof(asset))) return false;
        if (!summon_lab::PlayableClonePath(true, package, asset)) continue;
        ClassEntry entry;
        entry.path = std::string(package) + "." + asset;
        entry.label = asset;
        std::array<std::uint8_t, 16> name{};
        if (ReadRowValue(row, row_struct, "ItemName", "TextProperty", &name) &&
            g_context.names && g_context.names->resolve_ftext_utf8) {
            std::array<char, 256> text{};
            std::size_t size = text.size();
            if (g_context.names->resolve_ftext_utf8(g_context.names->user,
                reinterpret_cast<std::uintptr_t>(name.data()), text.data(), &size).code == ANOMALY_STATUS_V1_OK && text[0])
                entry.label = text.data();
        }
        entry.search_text = entry.label + " " + entry.path;
        entries.push_back(std::move(entry));
    }
    return !entries.empty();
}

void RefreshCloneCatalog() {
    std::vector<ClassEntry> entries;
    const bool ok = g_clones.ready && ReadPlayableClones(entries);
    if (g_context.core && g_context.core->log) {
        char message[96]{};
        std::snprintf(message, sizeof(message), "SummonLab: clone catalog compatible=%d loaded=%d entries=%zu",
            g_clones.ready, ok, ok ? entries.size() : 0);
        g_context.core->log(g_context.core->user, ok ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView(message));
    }
    auto labels = std::make_shared<Context::CatalogLabels>();
    if (ok) for (const auto& entry : entries) labels->emplace(entry.path, entry.label);
    g_context.clone_labels.store(std::move(labels), std::memory_order_release);
    std::scoped_lock lock(g_context.mutex);
    g_context.clone_catalog = ok ? std::move(entries) : std::vector<ClassEntry>{};
    g_context.clone_catalog_status = ok ? std::string{} : (g_clones.ready
        ? g_context.localizer.Text("clone.catalog_failed", "Character catalog could not be read; refresh to retry")
        : g_context.localizer.Text("clone.unavailable", "Character compatibility validation failed"));
}

bool LocalCloneTarget(AnomalyGenerationHandleV1& pawn, std::uintptr_t& pointer) {
    std::uintptr_t controller{};
    AnomalyGenerationHandleV1 controller_handle{};
    return FindLocalPawn(&pawn) && InvokeByPath(pawn, "/Script/Engine.Pawn:GetController",
        &controller, sizeof(controller)) && HandleFromPointer(controller, &controller_handle) &&
        InvokeByPath(controller_handle, "/Script/Engine.Controller:K2_GetPawn", &pointer, sizeof(pointer)) && pointer != 0;
}

bool ValidCloneClass(std::uintptr_t type) {
    std::int32_t size{};
    if (!ReadValue(type + g_layout.structSize, &size) || size <= 0 ||
        g_clones.layout.target + sizeof(std::uint64_t) > static_cast<std::uint32_t>(size)) return false;
    const auto anchor = FindProperty(type, "OpponentSummonerCharacter");
    std::int32_t offset{}, width{};
    if (!anchor || !ReadValue(anchor + g_layout.propertyOffset, &offset) ||
        !ReadValue(anchor + g_layout.propertyElementSize, &width) || width != 8 ||
        offset != g_clones.layout.target + sizeof(std::uint64_t)) return false;
    for (int depth = 0; type && depth < 32; ++depth) {
        if (CloneTypeNamed(type, "HTPlayerCharacter")) return true;
        if (!ReadValue(type + g_layout.structSuper, &type)) return false;
    }
    return false;
}

bool SetCloneTarget(const PlayerClone& clone, std::uintptr_t target) {
    std::uintptr_t current{};
    if (!InvokeByPath(clone.handle, "/Script/HTGame.HTAbilityCharacter:GetAttackTarget", &current, sizeof(current)))
        return false;
    // Match the native mirror's weak target as well as its ability attack target.
    // The engine setter allocates a serial when needed; never manufacture weak handles.
    using AssignWeak = void (ANOMALY_CALL*)(std::uintptr_t, const std::uintptr_t*);
    reinterpret_cast<AssignWeak>(g_clones.assign_weak)(clone.actor + g_clones.layout.target, &target);
    if (current == target) return true;
    struct { std::uintptr_t target; std::uint64_t socket{}; } parameters{target};
    return InvokeByPath(clone.handle, "/Script/HTGame.HTAbilityCharacter:SetAttackTarget",
        &parameters, sizeof(parameters));
}

bool SetCloneCombatState(const PlayerClone& clone, std::uintptr_t target) {
    std::uint8_t enabled{};
    if (!InvokeByPath(clone.handle, "/Script/HTGame.HTPlayerCharacter:IsAutoCombat", &enabled, sizeof(enabled)))
        return false;
    const std::uint8_t wanted = target != 0;
    if (!SetCloneTarget(clone, target)) return false;
    if (enabled != wanted) {
        auto value = wanted;
        if (!InvokeByPath(clone.handle, "/Script/HTGame.HTPlayerCharacter:SetAutoCombat", &value, sizeof(value)))
            return false;
        if (!target) {
            std::uintptr_t controller{};
            AnomalyGenerationHandleV1 handle{};
            if (!InvokeByPath(clone.handle, "/Script/Engine.Pawn:GetController", &controller, sizeof(controller)))
                return false;
            if (controller && (!HandleFromPointer(controller, &handle) ||
                !InvokeByPath(handle, "/Script/Engine.Controller:StopMovement", nullptr, 0))) return false;
        }
    }
    return true;
}

void ANOMALY_CALL CloneBuildDetour(std::uintptr_t owner, std::uintptr_t actor) noexcept {
    using Build = void (ANOMALY_CALL*)(std::uintptr_t, std::uintptr_t);
    const auto original = reinterpret_cast<Build>(g_clones.build_hook.original);
    AnomalyGenerationHandleV1 lease{};
    bool leased{};
    try {
        if (g_context.hooks && g_clones.build_hook.handle.id)
            leased = g_context.hooks->begin_callback(g_context.hooks->user,
                g_clones.build_hook.handle, &lease).code == ANOMALY_STATUS_V1_OK;
        if (original) original(owner, actor);
        if (leased && actor && g_clones.game_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
            BirthInitialization* birth{};
            if (g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId() &&
                g_birth_initialization && g_birth_initialization->player_clone) {
                std::uintptr_t type{};
                if (ReadValue(actor + g_layout.objectClass, &type) && type == g_birth_initialization->actor_class &&
                    (!g_birth_initialization->actor || g_birth_initialization->actor == actor))
                    birth = g_birth_initialization;
            }
            if (!birth) for (const auto& clone : g_clones.actors)
                if (clone.actor == actor && clone.birth) { birth = clone.birth.get(); break; }
            if (birth) {
                PlayerClone clone;
                clone.actor = actor;
                std::uint8_t enabled{};
                const bool completed = HandleFromPointer(actor, &clone.handle) &&
                    InvokeByPath(clone.handle, "/Script/HTGame.HTPlayerCharacter:IsAutoCombat", &enabled, sizeof(enabled)) && enabled;
                // The native callback equips the character and enables AutoCombat.
                // Its initial-HP flag is consumed by spawning/tick, so latch the
                // callback instead and apply exclusion before returning to gameplay.
                birth->clone_build_finished = completed;
                if (completed && birth->clone_hold)
                    birth->clone_control_failed = !SetCloneCombatState(clone, 0);
            }
        }
    } catch (...) {}
    if (leased) {
        try { static_cast<void>(g_context.hooks->end_callback(g_context.hooks->user, lease)); }
        catch (...) {}
    }
}

// FBufferData is a fixed ProcessEvent parameter ABI, validated by the host's
// function parameter-size check. Empty tag containers remain engine-owned.
struct CloneBuffData {
    std::uint64_t name{};
    std::array<std::uint8_t, 32> tags{};
    std::int32_t level{1};
    float duration{}, modify_duration{}, period{};
    std::int32_t stacks{};
    float strength{1}, strength_add{};
    std::uint32_t padding{};
    std::int64_t start_time{};
    std::uint8_t offline{1};
    std::array<std::uint8_t, 7> tail{};
};
static_assert(sizeof(CloneBuffData) == 88 && offsetof(CloneBuffData, level) == 40);

bool LoadCloneBuff(AnomalyGenerationHandleV1 library, std::uintptr_t& effect) {
    using GetData = std::uintptr_t (ANOMALY_CALL*)();
    const auto data = reinterpret_cast<GetData>(g_clones.ability_data)();
    summon_lab::SoftClassRef soft{};
    if (!data || !ReadReflectedValue(data, "OpponentSummonerCharacterBuff", "SoftClassProperty", &soft)) return false;
    summon_lab::LoadClass load{soft};
    if (!InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking", &load, sizeof(load))) return false;
    effect = load.result;
    return effect != 0;
}

SummonResult SummonPlayerClone(SummonBatch& batch) {
    SummonResult result;
    const auto fail = [&](const std::string& message) {
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    };
    if (!g_clones.ready)
        return fail(g_context.localizer.Text("clone.unavailable", "Character compatibility validation failed"));
    if (!g_context.birth_attributes_ready.load(std::memory_order_acquire))
        return fail(g_context.localizer.Text("param.birth_unavailable", "The native summon initializer is unavailable"));
    std::string label;
    {
        std::scoped_lock lock(g_context.mutex);
        const auto found = std::find_if(g_context.clone_catalog.begin(), g_context.clone_catalog.end(),
            [&](const ClassEntry& entry) { return entry.path == batch.settings.class_path; });
        if (found == g_context.clone_catalog.end())
            return fail(g_context.localizer.Text("clone.select", "Select a playable character from the Characters tab"));
        label = found->label;
    }
    AnomalyGenerationHandleV1 pawn{}, library{};
    std::uintptr_t owner{};
    if (!LocalCloneTarget(pawn, owner) || !FindHandle(kGameplayStaticsCdo, &library) ||
        (batch.completed == 0 && !CaptureBatchOrigin(pawn, batch)))
        return fail(g_context.localizer.Text("clone.no_player", "The controlled player is unavailable"));
    summon_lab::SoftClassRef soft{};
    if (!BuildSoftClassRef(library, batch.settings.class_path.c_str(), &soft))
        return fail(g_context.localizer.Text("clone.load_failed", "The playable character could not be loaded"));
    summon_lab::LoadClass load{soft};
    if (!InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking", &load, sizeof(load)) ||
        !load.result || !ValidCloneClass(load.result))
        return fail(g_context.localizer.Text("clone.load_failed", "The playable character could not be loaded"));
    std::uintptr_t effect{};
    if (!LoadCloneBuff(library, effect))
        return fail(g_context.localizer.Text("clone.buff_failed", "The native mirror combat effect is unavailable"));
    auto birth = std::make_shared<BirthInitialization>();
    if (!HandleFromPointer(effect, &birth->clone_effect_handle))
        return fail(g_context.localizer.Text("clone.buff_failed", "The native mirror combat effect is unavailable"));
    birth->clone_effect_class = effect;
    birth->player_clone = true;
    birth->clone_hold = batch.settings.ignore_player;
    birth->actor_class = load.result;
    const auto error = BirthPresetError(batch.settings.presets);
    if (!error.empty()) { std::snprintf(result.detail, sizeof(result.detail), "%s", error.c_str()); return result; }
    for (std::size_t i = 0; i < batch.settings.presets.size(); ++i) {
        const auto& preset = batch.settings.presets[i];
        if (preset.enabled) birth->attributes.push_back({std::string(kBirthAttributeNames[i]), static_cast<float>(preset.value)});
    }
    if (batch.settings.presets[kMaxHpAttribute].enabled)
        birth->exact_hp = static_cast<float>(batch.settings.presets[kMaxHpAttribute].value);
    summon_lab::SummonParameters placement{};
    SetBatchSpawnPlacement(batch, placement);
    using Spawn = std::uintptr_t (ANOMALY_CALL*)(std::uintptr_t, const std::uintptr_t*, const summon_lab::Vector*, bool, float);
    {
        BirthScope scope(*birth);
        result.pointer = reinterpret_cast<Spawn>(g_clones.spawn)(owner, &load.result,
            &placement.transform.translation, true, 1.0F);
    }
    SummonFailureCleanup cleanup{result};
    if (birth->registration_controller && HandleFromPointer(birth->registration_controller, &result.registration.controller)) {
        result.registration.leader = birth->registration_leader;
        result.registration.index = birth->registration_index;
    }
    if (!result.pointer || !HandleFromPointer(result.pointer, &result.handle))
        return fail(g_context.localizer.Text("clone.spawn_failed", "Native character creation failed"));
    birth->actor = result.pointer;
    PlayerClone clone{result.handle, result.pointer, birth, std::chrono::steady_clock::now()};
    clone.battle_mode = batch.settings.ignore_player;
    if (!ResolveActorAttributeSet(clone.actor, &result.ability_system, &result.attribute_set) ||
        !result.ability_system || !result.attribute_set) {
        static_cast<void>(InvokeByPath(result.handle, kPathDestroyActor, nullptr, 0));
        return fail(g_context.localizer.Text("clone.spawn_failed", "Native character creation failed"));
    }
    struct { std::uintptr_t context, effect; CloneBuffData data; } buff{result.pointer, effect};
    static_assert(sizeof(buff) == 104);
    std::uint8_t previous_cues{};
    const bool hide_effect = batch.settings.hide_clone_effect;
    if (hide_effect && (!ReadReflectedValue(result.ability_system, "bSuppressGameplayCues", "BoolProperty", &previous_cues) ||
        !SetReflectedBool(result.ability_system, "bSuppressGameplayCues", 1))) {
        static_cast<void>(InvokeByPath(result.handle, kPathDestroyActor, nullptr, 0));
        return fail(g_context.localizer.Text("clone.effect_failed", "Could not configure the mirror visual effect"));
    }
    bool buff_applied{};
    {
        BirthScope scope(*birth);
        buff_applied = InvokeByPath(result.handle, "/Script/HTGame.HTAbilityCharacter:K2_ActivateGameEffectsWithClass",
            &buff, sizeof(buff));
    }
    // Suppress only this mirror buff's visual cue. Restore immediately so the
    // character's own skills keep their VFX; retain the buff's attribute modifiers.
    const bool cues_restored = !hide_effect || SetReflectedBool(result.ability_system, "bSuppressGameplayCues", previous_cues);
    if (result.registration.controller.id == 0 || result.registration.index < 0 ||
        !buff_applied || !cues_restored || birth->clone_control_failed ||
        !(birth->clone_hold ? SetCloneCombatState(clone, 0) : SetCloneTarget(clone, owner)) ||
        !FinishBatchPlacement(result.handle, batch, placement.transform)) {
        static_cast<void>(InvokeByPath(result.handle, kPathDestroyActor, nullptr, 0));
        return fail(g_context.localizer.Text("clone.spawn_failed", "Native character creation failed"));
    }
    RegisterLocalBirth(clone.handle, clone.actor, result.ability_system, result.attribute_set,
        birth->exact_hp, {}, result.registration,
        std::any_of(birth->attributes.begin(), birth->attributes.end(),
            [](const BirthAttribute& value) { return IsCloneAttackAttribute(value.name); }));
    if (birth->exact_hp > 0) {
        std::uint8_t camp{};
        if (!ReadCamp(clone.actor, &camp)) {
            static_cast<void>(InvokeByPath(result.handle, kPathDestroyActor, nullptr, 0));
            return fail(g_context.localizer.Text("clone.spawn_failed", "Native character creation failed"));
        }
        std::scoped_lock lock(g_local_birth_mutex);
        g_birth_configurations.push_back({clone.handle, result.registration.controller, result.ability_system,
            ConfigureSummon{result.registration.leader, result.registration.index, camp, 1.0F}, birth->exact_hp});
    }
    // Registration is native; asynchronous character building keeps the native
    // completion callback that equips skills and enables AutoCombat.
    g_clones.actors.push_back(std::move(clone));
    std::snprintf(result.label, sizeof(result.label), "%s",
        batch.settings.display_name.empty() ? label.c_str() : batch.settings.display_name.c_str());
    const auto message = g_context.localizer.Text("clone.building", "Preparing character and skills...");
    std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
    result.ok = true;
    return result;
}

bool NormalizeCloneHealthBonus(const PlayerClone& clone, std::uintptr_t asc, std::uintptr_t set,
                               const std::vector<AttributeValue>& values) {
    const auto found = std::find_if(values.begin(), values.end(), [](const AttributeValue& v) { return v.name == "HPMaxAdd"; });
    if (found == values.end() || !std::isfinite(found->base) || !std::isfinite(found->current)) return false;
    const auto& birth = *clone.birth;
    std::uintptr_t cdo{}, set_class{}, asc_class{}, container{}, container_type{};
    if (!ProbeHandle(birth.clone_effect_handle) ||
        !ReadValue(birth.clone_effect_class + g_layout.classDefaultObject, &cdo) || !cdo ||
        !ReadValue(set + g_layout.objectClass, &set_class) ||
        !ReadValue(asc + g_layout.objectClass, &asc_class) ||
        !DiagnosticNestedStruct(asc, asc_class, "ActiveGameplayEffects", "ActiveGameplayEffectsContainer",
            &container, &container_type, 4096)) return false;
    const auto property = FindProperty(set_class, "HPMaxAdd");
    std::int32_t attribute_offset{};
    if (!IsGameplayAttributeProperty(property, &attribute_offset)) return false;
    struct Array { std::uintptr_t data{}; std::int32_t count{}, capacity{}; } modifiers;
    std::uintptr_t inner{}, modifier_type{};
    std::int32_t stride{};
    const auto modifiers_property = FindProperty(birth.clone_effect_class, "Modifiers");
    if (!ReadRowValue(cdo, birth.clone_effect_class, "Modifiers", "ArrayProperty", &modifiers) || !modifiers.data ||
        modifiers.count <= 0 || modifiers.count > 64 || modifiers.capacity < modifiers.count ||
        !ReadValue(modifiers_property + g_layout.arrayInner, &inner) ||
        !DiagnosticStruct(inner, "GameplayModifierInfo", &modifier_type, &stride, 4096)) return false;
    for (std::int32_t i = 0; i < modifiers.count; ++i) {
        const auto modifier = modifiers.data + static_cast<std::size_t>(i) * stride;
        std::uintptr_t attribute{}, attribute_type{}, field{};
        if (!DiagnosticNestedStruct(modifier, modifier_type, "Attribute", "GameplayAttribute", &attribute, &attribute_type) ||
            !ReadValue(attribute + g_layout.attributeProperty, &field)) return false;
        if (field != property) continue;
        std::uint8_t operation{};
        std::uint32_t size{};
        const float base = found->base - found->current;
        if (!ReadRowValue(modifier, modifier_type, "ModifierOp", "ByteProperty", &operation) || operation != 0 ||
            !ReadValue(attribute_type + g_layout.structSize, &size) || size != g_clones.attribute_size ||
            !std::isfinite(base) || !g_context.attribute_original) return false;
        // This native API takes FGameplayAttribute by value and destroys its
        // FString/field path. Use the engine copy constructor, never shallow-copy
        // or lend storage from the shared effect CDO to the consuming setter.
        alignas(16) std::array<std::uint8_t, 128> argument{};
        using Copy = void* (ANOMALY_CALL*)(void*, const void*);
        reinterpret_cast<Copy>(g_clones.attribute_copy)(argument.data(), reinterpret_cast<const void*>(attribute));
        reinterpret_cast<AttributeBaseInputFn>(g_context.attribute_original)(
            reinterpret_cast<void*>(container), argument.data(), base);
        // Change the GAS base/aggregator once, preserving the mirror effect,
        // skill cues and later damage reevaluation. Never patch CurrentValue.
        return true;
    }
    return false;
}

bool RefreshCloneAttack(const PlayerClone& clone, std::uintptr_t asc, std::uintptr_t set) {
    const auto& presets = clone.birth->attributes;
    const auto preset = std::find_if(presets.begin(), presets.end(),
        [](const BirthAttribute& value) { return IsCloneAttackAttribute(value.name); });
    if (preset == presets.end()) return true;

    // The native build can deserialize Atk after GAS computes it. Reapply one
    // attack input after build completion to run the game's derived calculation;
    // the local mirror filter preserves that cache without freezing later buffs.
    std::uintptr_t asc_class{}, set_class{}, container{}, container_type{};
    struct Array { std::uintptr_t data{}; std::int32_t count{}, capacity{}; } attributes;
    std::uintptr_t inner{}, attribute_type{};
    std::int32_t stride{};
    float before{};
    if (!ReadValue(asc + g_layout.objectClass, &asc_class) ||
        !ReadValue(set + g_layout.objectClass, &set_class) ||
        !DiagnosticNestedStruct(asc, asc_class, "ActiveGameplayEffects", "ActiveGameplayEffectsContainer",
            &container, &container_type, 4096) ||
        !ReadRowValue(asc, asc_class, "GameplayAttributes", "ArrayProperty", &attributes) ||
        !attributes.data || attributes.count <= 0 || attributes.count > 512 || attributes.capacity < attributes.count ||
        !ReadValue(FindProperty(asc_class, "GameplayAttributes") + g_layout.arrayInner, &inner) ||
        !DiagnosticStruct(inner, "GameplayAttribute", &attribute_type, &stride) ||
        static_cast<std::uint32_t>(stride) != g_clones.attribute_size || !g_context.attribute_original ||
        !ReadReflectedValue(asc, "Atk", "FloatProperty", &before)) return false;
    const auto property = FindProperty(set_class, preset->name.c_str());
    std::int32_t offset{};
    if (!IsGameplayAttributeProperty(property, &offset)) return false;
    for (std::int32_t i = 0; i < attributes.count; ++i) {
        const auto descriptor = attributes.data + static_cast<std::size_t>(i) * stride;
        std::uintptr_t field{};
        if (!ReadValue(descriptor + g_layout.attributeProperty, &field)) return false;
        if (field != property) continue;
        alignas(16) std::array<std::uint8_t, 128> argument{};
        using Copy = void* (ANOMALY_CALL*)(void*, const void*);
        reinterpret_cast<Copy>(g_clones.attribute_copy)(argument.data(), reinterpret_cast<const void*>(descriptor));
        reinterpret_cast<AttributeBaseInputFn>(g_context.attribute_original)(
            reinterpret_cast<void*>(container), argument.data(), preset->value);
        float after{};
        if (!ReadReflectedValue(asc, "Atk", "FloatProperty", &after) || !std::isfinite(after) || after < 0) return false;
        char message[192]{};
        std::snprintf(message, sizeof(message),
            "SummonLab character-attack: actor=%p input=%s requested=%.9g before=%.9g after=%.9g",
            reinterpret_cast<void*>(clone.actor), preset->name.c_str(), preset->value, before, after);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
        return true;
    }
    return false;
}

bool FinishPlayerClone(PlayerClone& clone) {
    std::uintptr_t asc{}, set{};
    std::uintptr_t asc_class{}, ability_container{}, container_type{};
    struct { std::uintptr_t data{}; std::int32_t count{}, capacity{}; } abilities;
    float hp{}, maximum{};
    std::vector<AttributeValue> values;
    const auto fail = [&](const char* stage) {
        const auto& birth = *clone.birth;
        char message[320]{};
        std::snprintf(message, sizeof(message),
            "SummonLab character-init: stage=%s actor=%p set=%p birthSet=%p hp=%.9g max=%.9g requested=%.9g",
            stage, reinterpret_cast<void*>(clone.actor), reinterpret_cast<void*>(set),
            reinterpret_cast<void*>(birth.attribute_set), hp, maximum, birth.exact_hp);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
        for (const auto& preset : birth.attributes) {
            const auto found = std::find_if(values.begin(), values.end(), [&](const AttributeValue& v) { return v.name == preset.name; });
            std::snprintf(message, sizeof(message),
                "SummonLab character-attribute: name=%s requested=%.9g intercepted=%d found=%d base=%.9g current=%.9g",
                preset.name.c_str(), preset.value, preset.applied, found != values.end(),
                found == values.end() ? 0.0F : found->base, found == values.end() ? 0.0F : found->current);
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
        }
        return false;
    };
    if (!ResolveActorAttributeSet(clone.actor, &asc, &set) || !asc || !set ||
        !ReadHealth(clone.handle, &hp, &maximum) || !std::isfinite(maximum) || maximum <= 0 ||
        !ReadAttributeValues(set, &values) ||
        !ReadValue(asc + g_layout.objectClass, &asc_class) ||
        !DiagnosticNestedStruct(asc, asc_class, "ActivatableAbilities", "GameplayAbilitySpecContainer",
            &ability_container, &container_type) ||
        !ReadRowValue(ability_container, container_type, "Items", "ArrayProperty", &abilities) ||
        !abilities.data || abilities.count <= 0 || abilities.capacity < abilities.count)
        return fail("ability-state");
    const auto& birth = *clone.birth;
    if (!birth.attributes.empty() && (birth.actor != clone.actor || birth.attribute_set != set)) return fail("attribute-owner");
    for (const auto& preset : birth.attributes) {
        const auto found = std::find_if(values.begin(), values.end(), [&](const AttributeValue& v) { return v.name == preset.name; });
        if (!preset.applied || found == values.end() || !HealthMatches(found->base, preset.value)) return fail("attribute-base");
    }
    if (!RefreshCloneAttack(clone, asc, set)) return fail("attack-cache");
    if (birth.exact_hp > 0 && !HealthMatches(maximum, birth.exact_hp)) {
        if (!NormalizeCloneHealthBonus(clone, asc, set, values) ||
            !ReadAttributeValues(set, &values) || !ReadHealth(clone.handle, &hp, &maximum) ||
            !HealthMatches(maximum, birth.exact_hp)) return fail("maximum-health");
    }
    summon_lab::SetHealth health{maximum};
    if (!InvokeByPath(clone.handle, "/Script/HTGame.HTAbilityCharacter:SetHP", &health, sizeof(health)) ||
        !ReadMatchingHealth(clone.handle, maximum, &hp, &maximum)) return fail("initial-health");
    if (!birth.attributes.empty()) {
        char message[192]{};
        std::snprintf(message, sizeof(message), "SummonLab character-init: ready actor=%p hp=%.9g max=%.9g attributes=%zu",
            reinterpret_cast<void*>(clone.actor), hp, maximum, birth.attributes.size());
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
    }
    clone.birth.reset();
    std::scoped_lock lock(g_context.mutex);
    if (!g_context.last_result_failed) {
        const auto message = g_context.localizer.Text("clone.ready", "Character ready");
        std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s", message.c_str());
    }
    for (auto& record : g_context.records) {
        if (record.pointer != clone.actor) continue;
        record.clone_pending = false;
        record.hp = hp;
        record.max_hp = maximum;
        record.attributes = std::move(values);
        break;
    }
    return true;
}

void UpdatePlayerClones() {
    if (g_clones.actors.empty()) return;
    AnomalyGenerationHandleV1 pawn{};
    std::uintptr_t player{};
    const bool has_player = LocalCloneTarget(pawn, player);
    std::vector<std::uint64_t> failed;
    for (auto& clone : g_clones.actors) {
        std::uint64_t id{};
        AnomalyGenerationHandleV1 battle_target{};
        std::uintptr_t battle_pointer{};
        AggroSettings aggro;
        {
            std::scoped_lock lock(g_context.mutex);
            const auto record = std::find_if(g_context.records.begin(), g_context.records.end(),
                [&](const Record& r) { return r.pointer == clone.actor; });
            if (record == g_context.records.end()) continue;
            id = record->id;
            aggro = record->aggro;
            if (clone.battle_mode && g_context.battles.Linked(id, record->current_target_id)) {
                const auto opponent = std::find_if(g_context.records.begin(), g_context.records.end(),
                    [&](const Record& r) { return r.id == record->current_target_id; });
                if (opponent != g_context.records.end()) {
                    battle_target = opponent->handle;
                    battle_pointer = opponent->pointer;
                }
            }
        }
        if (clone.birth) {
            const bool ready = clone.birth->clone_build_finished;
            if (clone.birth->clone_control_failed ||
                (clone.birth->clone_hold && !SetCloneCombatState(clone, 0)) ||
                (ready && !FinishPlayerClone(clone)) ||
                (!ready && std::chrono::steady_clock::now() - clone.started > std::chrono::seconds(30))) {
                failed.push_back(id);
                continue;
            }
            if (clone.birth) continue;
        }
        if (!IsActorAlive(clone.handle, clone.actor)) continue;
        if (clone.battle_mode) {
            // Native AutoCombat searches again when its weak target expires.
            // Pause before that fallback can select the player or an unlinked
            // summon; shared battle maintenance chooses the next opponent.
            if (!battle_pointer || !IsActorAlive(battle_target, battle_pointer)) {
                if (!SetCloneCombatState(clone, 0)) { failed.push_back(id); continue; }
                std::scoped_lock lock(g_context.mutex);
                for (auto& record : g_context.records) if (record.id == id) {
                    record.target_paused = true;
                    record.current_target_id = 0;
                    break;
                }
            } else if (!SetCloneTarget(clone, battle_pointer)) failed.push_back(id);
            continue;
        }
        bool in_range = has_player;
        if (in_range && aggro.chase_distance > 0) {
            summon_lab::Vector own{}, target{};
            in_range = InvokeByPath(clone.handle, "/Script/Engine.Actor:K2_GetActorLocation", &own, sizeof(own)) &&
                InvokeByPath(pawn, "/Script/Engine.Actor:K2_GetActorLocation", &target, sizeof(target)) &&
                std::hypot(own[0] - target[0], own[1] - target[1], own[2] - target[2]) <= aggro.chase_distance * 100.0;
        }
        if (!SetCloneCombatState(clone, in_range && !aggro.paused ? player : 0)) failed.push_back(id);
    }
    for (const auto id : failed) RemoveOne(id);
    if (!failed.empty()) {
        std::scoped_lock lock(g_context.mutex);
        const auto message = g_context.localizer.Text("clone.init_failed",
            "Character skills or custom attributes could not be initialized; character removed");
        std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s", message.c_str());
        g_context.last_result_failed = true;
        CancelSummonsLocked();
    }
}
