#pragma once

// Included inside plugin.cpp's private namespace, after its UE access helpers.
// All UObject work below runs on Game; the catalog worker handles paths only.

std::string EditorName(std::uint64_t name) {
    char text[256]{};
    if (!ResolveName(static_cast<std::uint32_t>(name), text, sizeof(text))) return {};
    const auto number = static_cast<std::uint32_t>(name >> 32);
    return number == 0 ? std::string(text) : std::string(text) + "_" + std::to_string(number - 1);
}

std::string EditorClassPath(std::uintptr_t object) {
    std::string result;
    for (int depth = 0; object && depth < 8; ++depth) {
        std::uint64_t name{};
        std::uintptr_t outer{};
        if (!ReadValue(object + g_layout.objectName, &name) ||
            !ReadValue(object + g_layout.objectOuter, &outer)) return {};
        const auto part = EditorName(name);
        if (part.empty()) return {};
        result = result.empty() ? part : part + "." + result;
        if (!outer) return result;
        object = outer;
    }
    return {};
}

bool EditorClassDerives(std::uintptr_t type, std::string_view base) {
    for (int depth = 0; type && depth < 32; ++depth) {
        if (EditorClassPath(type) == base) return true;
        if (!ReadValue(type + g_layout.structSuper, &type)) return false;
    }
    return false;
}

bool ReadEditorValue(std::uintptr_t object, const summon_lab::EditorField& field, double& value) {
    const auto address = object + field.offset;
    switch (field.data.kind) {
    case summon_lab::BlueprintFieldKind::Boolean: {
        std::uint8_t data{};
        if (!ReadValue(address, &data)) return false;
        value = (data & field.mask) ? 1 : 0;
        break;
    }
    case summon_lab::BlueprintFieldKind::Integer: {
        std::int32_t data{};
        if (!ReadValue(address, &data)) return false;
        value = data;
        break;
    }
    case summon_lab::BlueprintFieldKind::Float: {
        float data{};
        if (!ReadValue(address, &data)) return false;
        value = data;
        break;
    }
    case summon_lab::BlueprintFieldKind::Double:
        if (!ReadValue(address, &value)) return false;
        break;
    }
    return std::isfinite(value);
}

bool DescribeEditorField(std::uintptr_t property, std::uintptr_t cdo, std::int32_t object_size,
                         std::string_view owner, summon_lab::EditorField& result) {
    std::uintptr_t field_class{};
    std::uint32_t type_id{};
    std::uint64_t name{}, flags{};
    std::int32_t dimension{}, size{}, offset{};
    char type[64]{};
    if (!summon_lab::ValidBlueprintPath(owner) ||
        !ReadValue(property + g_layout.propertyFlags, &flags) ||
        // CPF_Edit | CPF_BlueprintVisible; reject readonly/transient/parameter state.
        (flags & (0x1ULL | 0x4ULL)) == 0 || (flags & (0x10ULL | 0x80ULL | 0x2000ULL | 0x20000ULL)) != 0 ||
        !ReadValue(property + g_layout.propertyArrayDim, &dimension) || dimension != 1 ||
        !ReadValue(property + g_layout.propertyElementSize, &size) ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(property + g_layout.fieldClass, &field_class) ||
        !ReadValue(field_class + g_layout.fieldClassName, &type_id) || !ResolveName(type_id, type, sizeof(type)) ||
        !ReadValue(property + g_layout.fieldName, &name)) return false;
    using Kind = summon_lab::BlueprintFieldKind;
    result = {};
    if (std::strcmp(type, "BoolProperty") == 0 && size == sizeof(std::uint8_t)) {
        std::uint8_t byte{};
        if (!ReadValue(property + g_layout.boolByteOffset, &byte) || byte != 0 ||
            !ReadValue(property + g_layout.boolFieldMask, &result.mask) ||
            !ReadValue(property + g_layout.boolByteMask, &result.set_mask) ||
            result.mask == 0 || result.set_mask == 0 ||
            (result.set_mask & (result.set_mask - 1)) != 0 ||
            (result.mask != 0xff && result.mask != result.set_mask)) return false;
        result.data.kind = Kind::Boolean;
    } else if (std::strcmp(type, "IntProperty") == 0 && size == sizeof(std::int32_t)) result.data.kind = Kind::Integer;
    else if (std::strcmp(type, "FloatProperty") == 0 && size == sizeof(float)) result.data.kind = Kind::Float;
    else if (std::strcmp(type, "DoubleProperty") == 0 && size == sizeof(double)) result.data.kind = Kind::Double;
    else return false;
    if (offset > object_size || size > object_size - offset) return false;
    result.offset = static_cast<std::uint32_t>(offset);
    result.data.owner = owner;
    result.data.name = EditorName(name);
    return !result.data.name.empty() && ReadEditorValue(cdo, result, result.data.value);
}

bool ResolveEditorFields(std::uintptr_t type, const summon_lab::BlueprintEdits& edits,
                         std::vector<summon_lab::EditorField>& result) {
    std::uintptr_t cdo{};
    std::int32_t size{};
    if (edits.fields.empty()) return true;
    if (!ReadValue(type + g_layout.classDefaultObject, &cdo) || !cdo ||
        !ReadValue(type + g_layout.structSize, &size) || size <= 0) return false;
    for (const auto& requested : edits.fields) {
        if (!summon_lab::ValidFieldValue(requested.kind, requested.value)) return false;
        auto owner = type;
        bool found{};
        for (int depth = 0; owner && depth < 16 && !found; ++depth) {
            const auto path = EditorClassPath(owner);
            if (!summon_lab::ValidBlueprintPath(path)) break;
            if (path == requested.owner) {
                std::uintptr_t property{};
                if (!ReadValue(owner + g_layout.structProperties, &property)) return false;
                for (int count = 0; property && count < 512; ++count) {
                    summon_lab::EditorField field;
                    if (DescribeEditorField(property, cdo, size, path, field) &&
                        field.data.name == requested.name && field.data.kind == requested.kind) {
                        field.data.value = requested.value;
                        result.push_back(std::move(field));
                        found = true;
                        break;
                    }
                    if (!ReadValue(property + g_layout.fieldNext, &property)) return false;
                }
                break;
            }
            if (!ReadValue(owner + g_layout.structSuper, &owner)) return false;
        }
        if (!found) return false;
    }
    return true;
}

template <typename T>
bool WriteEditorScalar(std::uintptr_t address, T value) {
    const auto core = g_context.core;
    return core && core->write_memory && core->write_memory(core->user, address,
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}).code == ANOMALY_STATUS_V1_OK;
}

bool ApplyEditorFields(std::uintptr_t actor, const std::vector<summon_lab::EditorField>& fields) {
    bool applied = true;
    for (const auto& field : fields) {
        const auto address = actor + field.offset;
        const auto value = field.data.value;
        bool written{};
        double expected = value, actual{};
        switch (field.data.kind) {
        case summon_lab::BlueprintFieldKind::Boolean: {
            std::uint8_t data{};
            if (ReadValue(address, &data))
                written = WriteEditorScalar(address, static_cast<std::uint8_t>(
                    (data & ~field.mask) | (value != 0 ? field.set_mask : 0)));
            break;
        }
        case summon_lab::BlueprintFieldKind::Integer:
            written = WriteEditorScalar(address, static_cast<std::int32_t>(value));
            break;
        case summon_lab::BlueprintFieldKind::Float:
            expected = static_cast<float>(value);
            written = WriteEditorScalar(address, static_cast<float>(value));
            break;
        case summon_lab::BlueprintFieldKind::Double:
            written = WriteEditorScalar(address, value);
            break;
        }
        applied = written && ReadEditorValue(actor, field, actual) && actual == expected && applied;
    }
    return applied;
}

void ANOMALY_CALL EditorBirthEvent(void*, std::uintptr_t object, std::uintptr_t function, void*) noexcept {
    try {
        // A birth scope identifies exactly the SummonAI call initiated by this plugin.
        if (g_birth_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return;
        auto* birth = g_birth_initialization;
        if (!birth || birth->editor_fields.empty() || birth->editor_attempted) return;
        std::uint64_t function_name{};
        std::uintptr_t type{};
        std::uint32_t flags{};
        if (!ReadValue(function + g_layout.objectName, &function_name) || EditorName(function_name) != "ReceiveBeginPlay" ||
            !ReadValue(object + g_layout.objectClass, &type) || type != birth->actor_class ||
            !ReadValue(object + g_layout.objectFlags, &flags) || (flags & 0x30U) != 0 ||
            (birth->actor && object != birth->actor)) return;
        birth->editor_attempted = true;
        birth->editor_actor = object;
        birth->editor_applied = ApplyEditorFields(object, birth->editor_fields);
    } catch (...) {}
}

void StartBlueprintEditor() {
    if (g_context.events && g_context.events->subscribe)
        static_cast<void>(g_context.events->subscribe(g_context.events->user, EditorBirthEvent, nullptr,
            &g_context.editor.birth_observer));
}

void FinishEditorScan(std::string message = {}, bool ready = false) {
    auto& editor = g_context.editor;
    std::scoped_lock lock(g_context.mutex);
    if (editor.snapshot.path == editor.scan.path) {
        editor.snapshot.message = std::move(message);
        editor.snapshot.ready = ready;
        editor.snapshot.pending = false;
        if (ready) editor.snapshot.fields = std::move(editor.scan.fields);
    }
    editor.scan = {};
}

void UpdateEditorFields() {
    auto& editor = g_context.editor;
    std::string request;
    bool load{};
    {
        std::scoped_lock lock(g_context.mutex);
        request.swap(editor.requested_path);
        load = editor.load_requested;
        editor.load_requested = false;
    }
    if (!request.empty()) {
        editor.scan = {};
        editor.scan.path = std::move(request);
        AnomalyGenerationHandleV1 library{};
        summon_lab::LoadClass params;
        if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) ||
            !BuildSoftClassRef(library, editor.scan.path.c_str(), &params.input) ||
            !InvokeByPath(library, load ? "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking" :
                "/Script/Engine.KismetSystemLibrary:Conv_SoftClassReferenceToClass", &params, sizeof(params)) || !params.result) {
            FinishEditorScan(g_context.localizer.Text("editor.fields.unloaded", "Load this blueprint to read its fields"));
            return;
        }
        editor.scan.type = editor.scan.owner = params.result;
        if (EditorClassPath(params.result) != editor.scan.path ||
            !HandleFromPointer(params.result, &editor.scan.type_handle) ||
            !ReadValue(params.result + g_layout.classDefaultObject, &editor.scan.cdo) ||
            !HandleFromPointer(editor.scan.cdo, &editor.scan.cdo_handle) ||
            !ReadValue(params.result + g_layout.structSize, &editor.scan.object_size) || editor.scan.object_size <= 0 ||
            !ReadValue(params.result + g_layout.structProperties, &editor.scan.next_field)) {
            FinishEditorScan(g_context.localizer.Text("editor.fields.failed", "Blueprint fields unavailable"));
            return;
        }
    }
    auto& scan = editor.scan;
    if (scan.path.empty()) return;
    if (!ProbeHandle(scan.type_handle) || !ProbeHandle(scan.cdo_handle)) {
        FinishEditorScan(g_context.localizer.Text("editor.fields.failed", "Blueprint fields unavailable"));
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    for (int work = 0; work < 16; ++work) {
        const auto owner = EditorClassPath(scan.owner);
        if (!scan.owner || !summon_lab::ValidBlueprintPath(owner)) { FinishEditorScan({}, true); return; }
        if (scan.depth >= 16 || scan.field_count >= 512) {
            FinishEditorScan(g_context.localizer.Text("editor.fields.failed", "Blueprint fields unavailable"));
            return;
        }
        if (!scan.next_field) {
            if (!ReadValue(scan.owner + g_layout.structSuper, &scan.owner) ||
                (scan.owner && !ReadValue(scan.owner + g_layout.structProperties, &scan.next_field))) {
                FinishEditorScan(g_context.localizer.Text("editor.fields.failed", "Blueprint fields unavailable"));
                return;
            }
            ++scan.depth;
            scan.field_count = 0;
            continue;
        }
        summon_lab::EditorField field;
        if (DescribeEditorField(scan.next_field, scan.cdo, scan.object_size, owner, field))
            scan.fields.push_back(std::move(field));
        ++scan.field_count;
        if (!ReadValue(scan.next_field + g_layout.fieldNext, &scan.next_field)) {
            FinishEditorScan(g_context.localizer.Text("editor.fields.failed", "Blueprint fields unavailable"));
            return;
        }
        if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1)) break;
    }
}

struct EditorAssetCollection { std::vector<std::string> paths; bool failed{}; };

int ANOMALY_CALL CollectEditorAsset(void* user, AnomalyStringViewV1 path) noexcept {
    auto& collection = *static_cast<EditorAssetCollection*>(user);
    try {
        if (g_context.catalog_stop.load(std::memory_order_acquire)) return 0;
        const std::string_view text(path.data, path.size);
        if (summon_lab::ValidBlueprintPath(text)) collection.paths.emplace_back(text);
        return 1;
    } catch (...) { collection.failed = true; return 0; }
}

void ANOMALY_CALL CollectEditorCatalog(void*, AnomalyGenerationHandleV1) noexcept {
    struct Complete { ~Complete() { g_context.editor.catalog_busy.store(false, std::memory_order_release); } } complete;
    try {
        auto catalog = std::make_shared<summon_lab::EditorAssetCatalog>();
        for (const bool effect : {false, true}) {
            EditorAssetCollection collection;
            const auto base = effect ? "/Script/GameplayAbilities.GameplayEffect" : "/Script/GameplayAbilities.GameplayAbility";
            const auto result = g_context.assets->enumerate_derived(g_context.assets->user,
                anomaly::sdk::StringView(base), CollectEditorAsset, &collection);
            if (g_context.catalog_stop.load(std::memory_order_acquire)) return;
            if (result.code != ANOMALY_STATUS_V1_OK || collection.failed) {
                catalog->message = g_context.localizer.Text("editor.catalog.failed", "Skill and effect catalog unavailable");
                catalog->skills.clear(); catalog->effects.clear();
                break;
            }
            std::sort(collection.paths.begin(), collection.paths.end());
            collection.paths.erase(std::unique(collection.paths.begin(), collection.paths.end()), collection.paths.end());
            (effect ? catalog->effects : catalog->skills) = std::move(collection.paths);
        }
        g_context.editor.catalog.store(std::move(catalog), std::memory_order_release);
    } catch (...) {
        if (g_context.core && g_context.core->log)
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: editor catalog collection failed"));
    }
}

void AdvanceEditorCatalog() {
    auto& editor = g_context.editor;
    if (editor.catalog_busy.load(std::memory_order_acquire) ||
        !editor.catalog_requested.exchange(false, std::memory_order_acq_rel)) return;
    const auto assets = g_context.assets;
    bool prepared = assets && g_context.scheduler;
    for (const auto* base : {"/Script/GameplayAbilities.GameplayAbility", "/Script/GameplayAbilities.GameplayEffect"})
        prepared = prepared && assets->prepare_class(assets->user, anomaly::sdk::StringView(base)).code == ANOMALY_STATUS_V1_OK;
    if (prepared) {
        editor.catalog_busy.store(true, std::memory_order_release);
        prepared = g_context.scheduler->schedule(g_context.scheduler->user, 0, CollectEditorCatalog,
            nullptr, &editor.catalog_task).code == ANOMALY_STATUS_V1_OK;
    }
    if (!prepared) {
        editor.catalog_busy.store(false, std::memory_order_release);
        auto result = std::make_shared<summon_lab::EditorAssetCatalog>();
        result->message = g_context.localizer.Text("editor.catalog.failed", "Skill and effect catalog unavailable");
        editor.catalog.store(std::move(result), std::memory_order_release);
    }
}

bool PrepareEditorRules(const summon_lab::BlueprintEdits& edits, std::shared_ptr<summon_lab::EditorRuntime>& runtime) {
    if (edits.disabled_skills.empty() && edits.removed_effects.empty()) return true;
    AnomalyGenerationHandleV1 library{};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library)) return false;
    runtime = std::make_shared<summon_lab::EditorRuntime>();
    for (const bool effect : {false, true}) {
        for (const auto& path : effect ? edits.removed_effects : edits.disabled_skills) {
            summon_lab::EditorRule rule{path, {}, effect};
            if (!summon_lab::ValidBlueprintPath(path) || !BuildSoftClassRef(library, path.c_str(), &rule.reference)) return false;
            runtime->rules.push_back(std::move(rule));
        }
    }
    return true;
}

// Fixed native UFunction parameter ABIs, present in both supplied SDKs.
struct EditorAbilityLookup { std::uintptr_t ability{}, source{}; std::int32_t handle{}; };
struct EditorEffectRemove { std::uintptr_t effect{}; std::int32_t stacks{-1}; std::uintptr_t instigator{}; };
static_assert(sizeof(EditorAbilityLookup) == 24 && offsetof(EditorAbilityLookup, handle) == 16);
static_assert(sizeof(EditorEffectRemove) == 24 && offsetof(EditorEffectRemove, instigator) == 16);

bool ApplyEditorRule(AnomalyGenerationHandleV1 actor, std::uintptr_t pointer, const summon_lab::EditorRule& rule) {
    AnomalyGenerationHandleV1 library{};
    summon_lab::LoadClass resolve{rule.reference};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) || !InvokeByPath(library,
            "/Script/Engine.KismetSystemLibrary:Conv_SoftClassReferenceToClass", &resolve, sizeof(resolve))) return false;
    // Unloaded classes cannot currently be granted/active. Never load the entire catalog.
    if (!resolve.result) return true;
    const auto base = rule.effect ? "/Script/GameplayAbilities.GameplayEffect" : "/Script/GameplayAbilities.GameplayAbility";
    if (EditorClassPath(resolve.result) != rule.path || !EditorClassDerives(resolve.result, base)) return false;
    if (rule.effect) {
        summon_lab::ObjectBool have{resolve.result};
        if (!InvokeByPath(actor, "/Script/HTGame.HTAbilityCharacter:K2_HaveGameplayEffectWithClass", &have, sizeof(have))) return false;
        if (!have.value) return true;
        EditorEffectRemove remove{resolve.result};
        // Use the game's wrapper so removal follows its normal native combat path.
        if (!InvokeByPath(actor, "/Script/HTGame.HTAbilityCharacter:K2_RemoveGameEffectsWithClass", &remove, sizeof(remove)) ||
            !IsActorUsable(actor, pointer)) return false;
        have.value = 0;
        return InvokeByPath(actor, "/Script/HTGame.HTAbilityCharacter:K2_HaveGameplayEffectWithClass", &have, sizeof(have)) &&
            have.value == 0;
    }
    std::uintptr_t asc{};
    AnomalyGenerationHandleV1 asc_handle{};
    if (!ReadReflectedValue(pointer, "HTAbilitySystemComponent", "ObjectProperty", &asc) ||
        !HandleFromPointer(asc, &asc_handle)) return false;
    EditorAbilityLookup lookup{resolve.result};
    constexpr auto find = "/Script/HTGame.HTAbilitySystemComponent:FindAbilitySpecHandleForClass";
    if (!InvokeByPath(asc_handle, find, &lookup, sizeof(lookup))) return false;
    if (lookup.handle == 0) return true;
    if (!InvokeByPath(asc_handle, "/Script/GameplayAbilities.AbilitySystemComponent:ClearAbility",
            &lookup.handle, sizeof(lookup.handle)) || !IsActorUsable(actor, pointer) || !ProbeHandle(asc_handle)) return false;
    lookup.handle = 0;
    return InvokeByPath(asc_handle, find, &lookup, sizeof(lookup)) && lookup.handle == 0;
}

void MaintainBlueprintEdits() {
    const auto started = std::chrono::steady_clock::now();
    // One rule per admission, with a total soft budget. Large battles stay bounded.
    for (int work = 0; work < 16; ++work) {
        std::shared_ptr<summon_lab::EditorRuntime> runtime;
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t pointer{};
        std::uint64_t id{};
        {
            std::scoped_lock lock(g_context.mutex);
            auto& cursor = g_context.editor.record_cursor;
            const auto eligible = [&](const Record& record) {
                return record.editor && record.editor->due <= started;
            };
            auto it = std::find_if(g_context.records.begin(), g_context.records.end(),
                [&](const Record& record) { return record.id > cursor && eligible(record); });
            if (it == g_context.records.end()) it = std::find_if(g_context.records.begin(), g_context.records.end(), eligible);
            if (it == g_context.records.end()) break;
            cursor = id = it->id; handle = it->handle; pointer = it->pointer; runtime = it->editor;
        }
        if (!IsActorAlive(handle, pointer)) { runtime->due = started + std::chrono::seconds(1); continue; }
        const auto& rule = runtime->rules[runtime->next];
        const bool applied = ApplyEditorRule(handle, pointer, rule);
        if (!applied) {
            runtime->cycle_failed = true;
            const std::array<std::string_view, 1> args{rule.path};
            const auto message = g_context.localizer.Format("editor.rule.failed", "Could not apply rule: {0}", args);
            std::scoped_lock lock(g_context.mutex);
            for (auto& record : g_context.records) if (record.id == id) record.editor_rule_warning = message;
        }
        if (++runtime->next == runtime->rules.size()) {
            runtime->next = 0;
            runtime->due = started + std::chrono::milliseconds(250);
            if (!runtime->cycle_failed) {
                std::scoped_lock lock(g_context.mutex);
                for (auto& record : g_context.records) if (record.id == id) record.editor_rule_warning.clear();
            }
            runtime->cycle_failed = false;
        }
        if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1)) break;
    }
}
