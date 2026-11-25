#pragma once
// Included in plugin.cpp: AHUD supplies current-frame projection; ImGui draws
// the resulting primitives. Render only sends typed intents to the Game owner.
void ANOMALY_CALL DrawAhud(void* raw, const AnomalyUe5AhudFrameV1* native) noexcept {
    if (!raw || !native || native->struct_size < sizeof(*native) || !native->project || !native->viewport_width || !native->viewport_height) return;
    auto& c = *static_cast<Context*>(raw);
    try {
        if (!c.active || !c.ready) { c.editor_frame.store({}); return; }
        const auto controls = c.editor_settings.load();
        if (!controls || !controls->editor_enabled) { c.editor_frame.store({}); return; }
        if (c.pending_pick) {
            const auto mouse = *c.pending_pick; c.pending_pick.reset();
            double closest = std::numeric_limits<double>::max(); std::uint64_t chosen{};
            // Project full geometry only on a click, rather than projecting all
            // 512 possible objects on every frame. Original world props are excluded.
            for (const auto& item : c.items) {
                const auto box = editor::ProjectBox(*native,item.corners);
                if (editor::HitBox(box,mouse) && box.depth < closest) { chosen = item.id; closest = box.depth; }
            }
            if (chosen) { c.focus = chosen; c.highlight = chosen; ++c.edit_revision; }
        }
        auto frame = std::make_shared<editor::Frame>(); frame->world = c.world;
        frame->width = native->viewport_width; frame->height = native->viewport_height;
        const LiveItem* selected{};
        for (const auto& item : c.items) if (item.id == c.highlight.load()) {
            selected = &item; frame->selected_box = editor::ProjectBox(*native,item.corners); break;
        }
        if (c.preview) { frame->preview_box = editor::ProjectBox(*native,c.preview->corners); selected = &*c.preview; frame->preview = true; }
        if (selected) {
            frame->id = selected->id; frame->pose = selected->item.pose; frame->selected = true;
            editor::BuildHandles(*frame,*native,*controls);
        }
        c.editor_frame.store(std::move(frame));
    } catch (...) { c.editor_frame.store({}); }
}
bool SceneInput(Context& c, const AnomalyUiServiceV1& ui) {
    if (!c.input || !c.input->snapshot) return false;
    auto& r = c.render;
    AnomalyInputSnapshotV1 input{sizeof(input)};
    if (c.input->snapshot(c.input->user,&input).code != 0) return false;
    struct Remember { RenderState& r; const AnomalyInputSnapshotV1& input; ~Remember() { r.previous_input = input; } } remember{r,input};
    const bool pressed = (input.mouse_buttons&1) && !(r.previous_input.mouse_buttons&1);
    const bool right = (input.mouse_buttons&2) && !(r.previous_input.mouse_buttons&2);
    r.input = input; r.left_pressed = pressed; r.left_released = !(input.mouse_buttons&1) && (r.previous_input.mouse_buttons&1);
    const auto frame = c.editor_frame.load(); const auto view = c.view.load();
    const auto key = [&](unsigned code, const AnomalyInputSnapshotV1& state) { return (state.keys[code/8] & (1u << (code%8))) != 0; };
    const auto edge = [&](unsigned code) { return key(code,input) && !key(code,r.previous_input); };
    const auto flags = ui.frame_state ? ui.frame_state(ui.user) : 0u;
    const bool typing = (flags&ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT) != 0;
    const bool over_ui = (flags&ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE) != 0;
    const bool captured = (input.capture_flags&ANOMALY_INPUT_CAPTURE_V1_MOUSE) != 0;
    if (!r.dragging_asset.empty()) {
        if (r.left_released) {
            if (frame && Same(frame->world,view->world) && captured && !over_ui) {
                Command command{Action::drop_asset,frame->world,r.settings}; command.settings.asset = r.dragging_asset;
                command.cursor = {input.mouse_x,input.mouse_y}; Request(c,std::move(command));
            }
            r.dragging_asset.clear();
        } else if (!(input.mouse_buttons&1)) r.dragging_asset.clear();
        return false;
    }
    if (!frame || !Same(frame->world,view->world) || !captured || !r.settings.editor_enabled) {
        r.drag.Clear(); r.hover_axis = -1; return false;
    }
    const auto send = [&](Action action) { Request(c,{action,frame->world,r.settings,frame->id}); };
    if (r.drag.Active() && (!Same(r.drag.start->world,frame->world) || r.drag.start->id != frame->id || r.drag.start->preview != frame->preview))
        r.drag.Clear();
    if (r.drag.Active()) {
        if (right || (!typing && edge(VK_ESCAPE))) {
            c.drag_intent.store(std::make_shared<const editor::TransformIntent>(r.drag.Intent(r.drag.start->pose)));
            r.drag.Clear(); return false;
        }
        if (!(input.mouse_buttons&1)) { r.drag.Clear(); return false; }
        const auto intent = r.drag.Move({input.mouse_x,input.mouse_y},
            (input.modifiers&ANOMALY_INPUT_MODIFIER_V1_SHIFT) != 0,
            (input.modifiers&ANOMALY_INPUT_MODIFIER_V1_CONTROL) != 0,r.settings);
        // The newest gesture wins; a fast Render loop cannot build a lagging
        // queue of old transforms behind the Game dispatcher.
        c.drag_intent.store(std::make_shared<const editor::TransformIntent>(intent));
        return false;
    }
    bool changed{};
    if (!typing && (input.capture_flags&ANOMALY_INPUT_CAPTURE_V1_KEYBOARD)) {
        if (edge('W')) { r.settings.move_handles = !r.settings.move_handles; changed = true; }
        if (edge('E')) { r.settings.rotate_handles = !r.settings.rotate_handles; changed = true; }
        if (edge('R')) { r.settings.scale_handles = !r.settings.scale_handles; changed = true; }
        if (edge(VK_RETURN) && frame->preview) send(Action::place);
        if (edge(VK_ESCAPE) && frame->preview) send(Action::cancel);
        if (edge(VK_DELETE) && frame->selected && !frame->preview) send(Action::remove);
    }
    r.hover_axis = -1;
    if (over_ui || typing) return changed;
    const editor::Point mouse{input.mouse_x,input.mouse_y};
    const auto pick = editor::PickHandle(*frame,mouse); r.hover_axis = pick.axis; r.hover_mode = pick.mode;
    if (pressed) {
        if (r.drag.Begin(frame,mouse)) {
            if (frame->preview) send(Action::freeze_preview);
        } else {
            Command command{Action::select_screen,frame->world,r.settings}; command.cursor = mouse; Request(c,std::move(command));
        }
    }
    return changed;
}
void DrawSceneEditor(Context& c, const AnomalyUiServiceV1& ui) {
    if (ui.struct_size < offsetof(AnomalyUiServiceV1,draw_screen_primitives)+sizeof(ui.draw_screen_primitives) || !ui.draw_screen_primitives) return;
    const auto frame = c.editor_frame.load(); const auto view = c.view.load();
    if (!frame || !Same(frame->world,view->world)) return;
    std::vector<AnomalyUiScreenPrimitiveV1> lines;
    const auto segment = [&](editor::Point a, editor::Point b, std::uint32_t color, float width) {
        lines.push_back({sizeof(AnomalyUiScreenPrimitiveV1),ANOMALY_UI_SCREEN_PRIMITIVE_V1_LINE,color,a[0],a[1],b[0],b[1],width});
    };
    const auto rect = [&](editor::Point p, float size, std::uint32_t color) {
        lines.push_back({sizeof(AnomalyUiScreenPrimitiveV1),ANOMALY_UI_SCREEN_PRIMITIVE_V1_FILLED_RECT,color,p[0]-size/2,p[1]-size/2,size,size,1});
    };
    const auto box = [&](const editor::Box& b, std::uint32_t color) {
        for (const auto& edge : editor::Edges) if (b.visible[edge[0]] && b.visible[edge[1]]) segment(b.points[edge[0]],b.points[edge[1]],color,1.6f);
    };
    box(frame->selected_box,ANOMALY_RGBA_V1(70,220,255,255));
    if (frame->preview) box(frame->preview_box,ANOMALY_RGBA_V1(255,210,60,255));
    constexpr std::array<std::uint32_t,3> colors{ANOMALY_RGBA_V1(255,85,95,255),ANOMALY_RGBA_V1(85,230,125,255),ANOMALY_RGBA_V1(75,160,255,255)};
    for (std::size_t kind = 0; kind < frame->gizmos.size(); ++kind) {
        const auto mode = static_cast<editor::Mode>(kind);
        const auto& g = frame->gizmos[kind];
        if (!g.visible) continue;
        const int active = c.render.drag.Active() ? c.render.drag.axis : c.render.hover_axis;
        const auto active_mode = c.render.drag.Active() ? c.render.drag.mode : c.render.hover_mode;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto highlighted = active_mode == mode && active == static_cast<int>(axis);
            const auto color = highlighted ? ANOMALY_RGBA_V1(255,245,160,255) : colors[axis];
            if (mode == editor::Mode::Rotate) {
                const auto& ring = g.rings[axis];
                for (std::size_t i = 1; i < ring.points.size(); ++i) if (ring.visible[i-1] && ring.visible[i])
                    segment(ring.points[i-1],ring.points[i],ring.front[i-1] || ring.front[i] ? color : (color&0x00ffffffu)|0x50000000u,highlighted ? 3.5f : 2.0f);
            } else if (g.arrows[axis].visible) {
                const auto& a = g.arrows[axis]; const auto dx = a.tip[0]-a.base[0], dy = a.tip[1]-a.base[1];
                const auto length = std::hypot(dx,dy);
                if (length < 12) continue;
                const editor::Point stem = mode == editor::Mode::Scale ? editor::Point{a.base[0]+dx*0.88f,a.base[1]+dy*0.88f} : a.base;
                segment(stem,a.tip,color,highlighted ? 4 : 2.5f);
                if (mode == editor::Mode::Scale) rect(a.tip,9,color);
                else {
                    const auto x = dx/length, y = dy/length;
                    segment(a.tip,{a.tip[0]-x*11+y*5,a.tip[1]-y*11-x*5},color,2.5f);
                    segment(a.tip,{a.tip[0]-x*11-y*5,a.tip[1]-y*11+x*5},color,2.5f);
                }
            }
        }
        if (mode == editor::Mode::Scale) rect(g.origin,10,ANOMALY_RGBA_V1(245,245,245,255));
    }
    if (!lines.empty()) ui.draw_screen_primitives(ui.user,lines.data(),lines.size());
    const auto& move = frame->gizmos[static_cast<std::size_t>(editor::Mode::Move)];
    if (move.visible && ui.struct_size >= offsetof(AnomalyUiServiceV1,overlay_text)+sizeof(ui.overlay_text) && ui.overlay_text) {
        const std::array labels{c.localizer.Text("editor.axis_x","X"),c.localizer.Text("editor.axis_y","Y"),c.localizer.Text("editor.axis_z","Z")};
        for (std::size_t i = 0; i < 3; ++i) if (move.arrows[i].visible)
            ui.overlay_text(ui.user,move.arrows[i].tip[0]+7,move.arrows[i].tip[1]-8,StringView(labels[i]),colors[i],15);
    }
}
bool DrawEditorToolbar(Context& c, const AnomalyUiServiceV1& ui) {
    if (!c.toolbar_handle.id) return false;
    const auto v = c.view.load();
    if (v->items.empty() && !v->has_preview) return false;
    AnomalyWindowStateV1 state{sizeof(state)};
    if (c.window->state(c.window->user,c.toolbar_handle,&state).code != 0 || !state.open) return false;
    int visible{};
    if (c.window->begin(c.window->user,c.toolbar_handle,0,&visible).code != 0) return false;
    struct End { Context& c; ~End() { c.window->end(c.window->user,c.toolbar_handle); } } end{c};
    if (!visible) return false;
    auto& s = c.render.settings; bool changed{};
    const auto toggle = [&](const char* key,const char* fallback,bool& value) {
        int checked = value;
        if (ui.checkbox(ui.user,StringView(c.localizer.Text(key,fallback)),&checked)) { value = checked != 0; changed = true; }
    };
    toggle("editor.move","Move · W",s.move_handles); ui.same_line(ui.user,0,-1);
    toggle("editor.rotate","Rotate · E",s.rotate_handles); ui.same_line(ui.user,0,-1);
    toggle("editor.scale","Scale · R",s.scale_handles);
    if (v->has_preview) {
        if (ui.button_enabled(ui.user,StringView(c.localizer.Text("action.place","Place here")),0,0,v->ready))
            Request(c,{Action::place,v->world,s});
        ui.same_line(ui.user,0,-1);
        if (ui.button(ui.user,StringView(c.localizer.Text("action.cancel","Cancel preview")),0,0)) Request(c,{Action::cancel,v->world,s});
    }
    const auto help = c.localizer.Text("editor.help","Arrows move, rings rotate, squares scale. Handle toggles are independent. Shift: fine · Ctrl: snap · Right click: cancel drag");
    if (ui.text_wrapped) ui.text_wrapped(ui.user,StringView(help)); else ui.text(ui.user,StringView(help));
    return changed;
}
