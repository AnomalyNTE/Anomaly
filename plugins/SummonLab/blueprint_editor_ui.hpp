#pragma once

// Included after the shared UI helpers; Render reads copied values only.
std::string EditorShortName(std::string_view path) {
    const auto dot = path.rfind('.');
    return std::string(dot == std::string_view::npos ? path : path.substr(dot + 1));
}

std::string EditorSearchSeed(std::string_view path) {
    auto name = EditorShortName(path);
    for (char& ch : name) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    for (const std::string_view prefix : {"boss_", "mon_"}) {
        const auto at = name.find(prefix);
        if (at == std::string::npos) continue;
        const auto start = at + prefix.size();
        const auto end = name.find('_', start);
        const auto begin = prefix == "mon_" ? start : at;
        return name.substr(begin, end == std::string::npos ? end : end - begin);
    }
    return {};
}

bool EditorMatches(const AnomalyUiServiceV1* ui, std::string_view filter, std::string_view text) {
    return filter.empty() || (ui->filter_match
        ? ui->filter_match(ui->user, anomaly::sdk::StringView(filter), anomaly::sdk::StringView(text)) != 0
        : text.find(filter) != std::string_view::npos);
}

void DrawEditorPages(const AnomalyUiServiceV1* ui, std::size_t count, std::size_t& page, std::string_view id) {
    constexpr std::size_t size = 10;
    const auto pages = std::max<std::size_t>(1, (count + size - 1) / size);
    page = std::min(page, pages - 1);
    const auto& localizer = g_context.localizer;
    const auto previous = localizer.Label("editor.previous", "Previous", std::string(id) + "-previous");
    const auto next = localizer.Label("editor.next", "Next", std::string(id) + "-next");
    if (ActionButton(ui, previous.c_str(), page != 0)) --page;
    if (ui->same_line) ui->same_line(ui->user, 0, 8);
    const auto current = std::to_string(page + 1), total = std::to_string(pages), matches = std::to_string(count);
    const std::array<std::string_view, 3> args{current, total, matches};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format("editor.page", "{0} / {1}  ({2})", args)));
    if (ui->same_line) ui->same_line(ui->user, 0, 8);
    if (ActionButton(ui, next.c_str(), page + 1 < pages)) ++page;
}

void RequestEditorFieldsLocked(bool load) {
    auto& editor = g_context.editor;
    editor.snapshot = {};
    editor.snapshot.path = g_context.class_path.data();
    editor.snapshot.pending = true;
    editor.requested_path = editor.snapshot.path;
    editor.load_requested = load;
}

void DrawEditorFields(const AnomalyUiServiceV1* ui, summon_lab::BlueprintEdits& edits, bool ready) {
    auto& editor = g_context.editor;
    const auto& localizer = g_context.localizer;
    auto& snapshot = editor.snapshot;
    const auto read = localizer.Text("editor.fields.read", "Read fields");
    if (ActionButton(ui, read.c_str(), ready && !snapshot.pending)) RequestEditorFieldsLocked(true);
    if (ui->same_line) ui->same_line(ui->user, 0, 8);
    const auto reset = localizer.Text("editor.fields.reset", "Reset fields");
    if (ActionButton(ui, reset.c_str(), !edits.fields.empty())) {
        edits.fields.clear();
        ConfigurationChangedLocked();
    }
    if (snapshot.pending) WrappedText(ui, localizer.Text("editor.fields.reading", "Reading fields"));
    if (!snapshot.message.empty()) WrappedText(ui, snapshot.message);
    if (ui->input_text && ui->input_text(ui->user,
            anomaly::sdk::StringView(localizer.Text("editor.fields.filter", "Find field")),
            editor.field_filter.data(), editor.field_filter.size(), 0)) editor.field_page = 0;
    if (ui->checkbox && ui->checkbox(ui->user,
            anomaly::sdk::StringView(localizer.Text("editor.fields.customized", "Only customized fields")),
            &editor.customized_only)) editor.field_page = 0;

    const auto same = [](const auto& a, const auto& b) { return a.owner == b.owner && a.name == b.name && a.kind == b.kind; };
    std::vector<const summon_lab::EditorField*> matches;
    for (const auto& field : snapshot.fields) {
        const bool selected = std::any_of(edits.fields.begin(), edits.fields.end(), [&](const auto& edit) { return same(edit, field.data); });
        if ((!editor.customized_only || selected) &&
            EditorMatches(ui, editor.field_filter.data(), field.data.name + "\n" + field.data.owner)) matches.push_back(&field);
    }
    DrawEditorPages(ui, matches.size(), editor.field_page, "editor-fields");
    const bool child = ui->begin_child && ui->end_child;
    const bool visible = !child || ui->begin_child(ui->user, anomaly::sdk::StringView("##editor-field-list"), 0, 330, 0);
    if (visible) {
        for (std::size_t i = editor.field_page * 10; i < matches.size() && i < (editor.field_page + 1) * 10; ++i) {
            const auto& field = *matches[i];
            auto found = std::find_if(edits.fields.begin(), edits.fields.end(), [&](const auto& edit) { return same(edit, field.data); });
            int enabled = found != edits.fields.end();
            const auto id = field.data.owner + ":" + field.data.name;
            const auto label = field.data.name + "###editor-enabled-" + id;
            if (ui->checkbox && ui->checkbox(ui->user, anomaly::sdk::StringView(label), &enabled)) {
                if (enabled) { edits.fields.push_back(field.data); found = std::prev(edits.fields.end()); }
                else { edits.fields.erase(found); found = edits.fields.end(); }
                ConfigurationChangedLocked();
            }
            if (found != edits.fields.end()) {
                if (ui->same_line) ui->same_line(ui->user, 0, 12);
                bool changed{};
                const auto input = "##editor-value-" + id;
                if (found->kind == summon_lab::BlueprintFieldKind::Boolean) {
                    int value = found->value != 0;
                    if (ui->checkbox && ui->checkbox(ui->user, anomaly::sdk::StringView(input), &value)) {
                        found->value = value; changed = true;
                    }
                } else if (ui->input_double) {
                    if (ui->struct_size >= offsetof(AnomalyUiServiceV1, set_next_item_width) +
                            sizeof(ui->set_next_item_width) && ui->set_next_item_width)
                        ui->set_next_item_width(ui->user, 160);
                    changed = ui->input_double(ui->user, anomaly::sdk::StringView(input), &found->value, 0, 0) != 0;
                }
                if (changed) ConfigurationChangedLocked();
                if (!summon_lab::ValidFieldValue(found->kind, found->value))
                    WrappedText(ui, localizer.Text("editor.fields.invalid", "Value is outside this field's type range"));
            }
            char value[64]{};
            std::snprintf(value, sizeof(value), "%.9g", field.data.value);
            const std::array<std::string_view, 2> args{summon_lab::FieldKindName(field.data.kind), value};
            WrappedText(ui, localizer.Format("editor.fields.default", "{0} / Default {1}", args));
        }
        if (snapshot.ready && matches.empty()) WrappedText(ui,
            localizer.Text("editor.fields.empty", "No editable scalar fields"));
        // Imported entries remain visible/removable even if a field was removed in an update.
        for (std::size_t i = 0; i < edits.fields.size();) {
            const auto& field = edits.fields[i];
            if (std::any_of(snapshot.fields.begin(), snapshot.fields.end(), [&](const auto& item) { return same(item.data, field); })) { ++i; continue; }
            const std::array<std::string_view, 1> args{field.name};
            WrappedText(ui, localizer.Format("editor.fields.unresolved", "Unresolved field: {0}", args));
            const auto remove = localizer.Label("editor.remove", "Remove", "editor-unresolved-" + field.owner + field.name);
            if (ActionButton(ui, remove.c_str(), true)) {
                edits.fields.erase(edits.fields.begin() + i);
                ConfigurationChangedLocked();
            } else ++i;
        }
    }
    if (child) ui->end_child(ui->user);
}

void DrawEditorClassRules(const AnomalyUiServiceV1* ui, summon_lab::BlueprintEdits& edits, bool effects) {
    auto& editor = g_context.editor;
    const auto& localizer = g_context.localizer;
    auto& selected = effects ? edits.removed_effects : edits.disabled_skills;
    auto& filter = effects ? editor.effect_filter : editor.skill_filter;
    auto& page = effects ? editor.effect_page : editor.skill_page;
    auto& only_selected = effects ? editor.effects_selected_only : editor.skills_selected_only;
    const auto catalog = editor.catalog.load(std::memory_order_acquire);
    if (!catalog && !editor.catalog_busy.load(std::memory_order_acquire))
        editor.catalog_requested.store(true, std::memory_order_release);
    WrappedText(ui, effects
        ? localizer.Text("editor.effects.hint", "Continuously remove checked effects; instant effects cannot be undone")
        : localizer.Text("editor.skills.hint", "Remove checked skills from new summons"));
    if (ui->input_text && ui->input_text(ui->user, anomaly::sdk::StringView(effects
            ? localizer.Text("editor.effects.filter", "Find effect") : localizer.Text("editor.skills.filter", "Find skill")),
            filter.data(), filter.size(), 0)) page = 0;
    if (ui->checkbox && ui->checkbox(ui->user,
            anomaly::sdk::StringView(localizer.Text("editor.selected", "Only selected")), &only_selected)) page = 0;
    if (ui->same_line) ui->same_line(ui->user, 0, 8);
    const auto reset = localizer.Text("editor.clear_selection", "Clear selection");
    if (ActionButton(ui, reset.c_str(), !selected.empty())) { selected.clear(); ConfigurationChangedLocked(); }
    if (ui->same_line) ui->same_line(ui->user, 0, 8);
    const auto refresh = localizer.Text("editor.catalog.refresh", "Refresh catalog");
    if (ActionButton(ui, refresh.c_str(), !editor.catalog_busy.load(std::memory_order_acquire)))
        editor.catalog_requested.store(true, std::memory_order_release);
    if (catalog && !catalog->message.empty()) WrappedText(ui, catalog->message);
    else if (!catalog) WrappedText(ui, localizer.Text("editor.catalog.loading", "Reading skill and effect catalog"));

    std::vector<std::string_view> matches;
    const auto add = [&](const std::string& path) {
        const bool checked = std::find(selected.begin(), selected.end(), path) != selected.end();
        if ((!only_selected || checked) && EditorMatches(ui, filter.data(), path)) matches.push_back(path);
    };
    if (catalog) for (const auto& path : effects ? catalog->effects : catalog->skills) add(path);
    for (const auto& path : selected) {
        const auto* paths = catalog ? &(effects ? catalog->effects : catalog->skills) : nullptr;
        if (!paths || !std::binary_search(paths->begin(), paths->end(), path)) add(path);
    }
    DrawEditorPages(ui, matches.size(), page, effects ? "editor-effects" : "editor-skills");
    const bool child = ui->begin_child && ui->end_child;
    const bool visible = !child || ui->begin_child(ui->user, anomaly::sdk::StringView(effects
        ? "##editor-effects-list" : "##editor-skills-list"), 0, 330, 0);
    if (visible) {
        // Own copies protect rows when a click reallocates the selected-path vector.
        std::vector<std::string> rows;
        for (std::size_t i = page * 10; i < matches.size() && i < (page + 1) * 10; ++i) rows.emplace_back(matches[i]);
        for (const auto& path : rows) {
            auto found = std::find(selected.begin(), selected.end(), path);
            int checked = found != selected.end();
            const auto label = EditorShortName(path) + "###editor-rule-" + path;
            if (ui->checkbox && ui->checkbox(ui->user, anomaly::sdk::StringView(label), &checked)) {
                if (checked) selected.push_back(path); else selected.erase(found);
                ConfigurationChangedLocked();
            }
        }
    }
    if (child) ui->end_child(ui->user);
}

void DrawBlueprintEditor(const AnomalyUiServiceV1* ui, bool ready) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    const std::string path = g_context.class_path.data();
    if (!summon_lab::ValidBlueprintPath(path)) return;
    auto& editor = g_context.editor;
    if (editor.snapshot.path != path && ready) {
        RequestEditorFieldsLocked(false);
        editor.field_page = editor.skill_page = editor.effect_page = 0;
        const auto seed = EditorSearchSeed(path);
        std::snprintf(editor.skill_filter.data(), editor.skill_filter.size(), "%s", seed.c_str());
        std::snprintf(editor.effect_filter.data(), editor.effect_filter.size(), "%s", seed.c_str());
    }
    WrappedText(ui, BlueprintLabel(path));
    WrappedText(ui, localizer.Text("editor.hint", "Applies to new summons and is saved with presets"));
    auto& edits = editor.blueprints[path];
    if (!ui->begin_tab_bar(ui->user, anomaly::sdk::StringView("##blueprint-editor-tabs"), 0)) return;
    const auto fields = localizer.Label("editor.fields", "Fields", "editor-fields");
    if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(fields), nullptr, 0, 1)) {
        DrawEditorFields(ui, edits, ready); ui->end_tab_item(ui->user);
    }
    const auto skills = localizer.Label("editor.skills", "Skills", "editor-skills");
    if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(skills), nullptr, 0, 1)) {
        DrawEditorClassRules(ui, edits, false); ui->end_tab_item(ui->user);
    }
    const auto effects = localizer.Label("editor.effects", "Effects", "editor-effects");
    if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(effects), nullptr, 0, 1)) {
        DrawEditorClassRules(ui, edits, true); ui->end_tab_item(ui->user);
    }
    ui->end_tab_bar(ui->user);
}
