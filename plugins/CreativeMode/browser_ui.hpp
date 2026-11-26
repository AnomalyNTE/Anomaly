#pragma once
struct ChildPanel {
    const AnomalyUiServiceV1& ui; bool visible;
    ChildPanel(const AnomalyUiServiceV1& ui,const char* id,float width,float height) : ui(ui),visible(ui.begin_child(ui.user,StringView(id),width,height,0) != 0) {}
    ~ChildPanel() { ui.end_child(ui.user); }
};
void DrawContents(Context& c,const AnomalyUiServiceV1& ui) {
    auto& r = c.render; auto& s = r.settings; const auto view = c.view.load();
    const auto TextLocalized = [&](const char* key,const char* fallback) { return c.localizer.Text(key,fallback); };
    const auto text = [&](std::string_view value) { ui.text(ui.user,StringView(value)); };
    const auto wrapped = [&](std::string_view value) {
        if (ui.struct_size >= offsetof(AnomalyUiServiceV1,text_wrapped)+sizeof(ui.text_wrapped) && ui.text_wrapped) ui.text_wrapped(ui.user,StringView(value));
        else text(value);
    };
    const auto button = [&](std::string_view label,bool enabled = true,float width = 0,float height = 0) {
        return ui.button_enabled(ui.user,StringView(label),width,height,enabled) != 0;
    };
    const auto selected_button = [&](std::string_view label,bool selected,float width = -1.0f) {
        if (ui.struct_size >= offsetof(AnomalyUiServiceV1,button_selected)+sizeof(ui.button_selected) && ui.button_selected)
            return ui.button_selected(ui.user,StringView(label),width,0,selected,1) != 0;
        return button(label,true,width);
    };
    const auto line = [&] { ui.same_line(ui.user,0,-1); };
    const auto check = [&](std::string_view label,bool& value) {
        int flag = value; if (!ui.checkbox(ui.user,StringView(label),&flag)) return false; value = flag != 0; return true;
    };
    const auto set_width = ui.struct_size >= offsetof(AnomalyUiServiceV1,set_next_item_width)+sizeof(ui.set_next_item_width)
        ? ui.set_next_item_width : nullptr;
    const auto number = [&](std::string_view label,double& value,double step) {
        wrapped(label);
        if (set_width) set_width(ui.user,-1);
        return ui.input_double(ui.user,StringView("##"+std::string(label)),&value,step,step*10) != 0;
    };
    const auto uniform_scale = [&](Vector& scale) {
        double percent = scale[0]*100;
        if (!number(TextLocalized("label.scale_percent","Scale (%)"),percent,0.01) || !std::isfinite(percent) || percent <= 0 || !Pose{{},{},scale}.Valid()) return false;
        ScaleUniformly(scale,percent/(scale[0]*100));
        return true;
    };
    const auto send = [&](Action action,std::uint64_t id = 0) { Request(c,{action,view->world,s,id,r.pose,r.collision}); };
    const auto choose = [&](const Asset& asset) {
        s.asset = asset.path; std::fill(std::begin(r.asset),std::end(r.asset),'\0'); std::copy(asset.path.begin(),asset.path.end(),r.asset);
    };
    bool changed{};
    const auto select_item = [&](const LiveItem& item) { r.selected = item.id; r.pose = item.item.pose; r.collision = item.item.collision; c.highlight = item.id; };
    if (view->focus != r.last_focus || (!r.drag.Active() && view->edit_revision != r.last_edit_revision)) {
        r.selected = view->focus;
        r.last_focus = view->focus; r.last_edit_revision = view->edit_revision;
        for (const auto& item : view->items) if (item.id == r.selected) select_item(item);
    }
    Tabs tabs{&ui,ui.begin_tab_bar(ui.user,StringView("creative-mode-tabs"),0) != 0};
    if (!tabs.open) return;
    {
        const auto label = TextLocalized("browser.assets","Asset browser");
        Tab tab{&ui,ui.begin_tab_item(ui.user,StringView(label),nullptr,0,1) != 0};
        if (tab.open) {
            if (r.textures.Available() && view->ready) r.textures.Active();
            float window_width = 1180,window_height = 760;
            if (ui.get_window_size) ui.get_window_size(ui.user,&window_width,&window_height);
            text(TextLocalized("label.search","Search models"));
            if (set_width) set_width(ui.user,std::max(180.0f,window_width-225));
            if (ui.input_text(ui.user,StringView("##asset-search"),r.filter,sizeof(r.filter),0)) { s.filter = r.filter; r.catalog_page = 0; changed = true; }
            line();
            if (button(TextLocalized("action.refresh","Refresh"),view->ready && !view->scanning,180)) send(Action::refresh);
            if (view->scanning) {
                text(TextLocalized("status.catalog_work","Preparing the versioned asset catalog in the background..."));
            }
            constexpr float sidebar_width = 208, inspector_width = 298;
            // EndTabBar contributes trailing item spacing after the last child.
            // Reserve it here so only the three panes scroll, not the host body.
            constexpr float pane_height = -20;
            const auto middle_width = std::max(240.0f,window_width-sidebar_width-inspector_width-56);
            const std::array categories{TextLocalized("category.all","All assets"),TextLocalized("category.shapes","Basic shapes"),
                TextLocalized("category.furniture","Furniture"),TextLocalized("category.roads","Roads"),
                TextLocalized("category.vehicles","Vehicles"),TextLocalized("category.buildings","Architecture"),
                TextLocalized("category.nature","Nature"),TextLocalized("category.props","Props")};
            {
                ChildPanel sidebar{ui,"asset-sidebar",sidebar_width,pane_height};
                if (sidebar.visible) {
                    text(TextLocalized("browser.categories","Categories"));
                    for (std::size_t i = 0; i < categories.size(); ++i) {
                        const auto category_label = categories[i]+"###category-"+std::to_string(i);
                        if (selected_button(category_label,s.category == i)) { s.category = static_cast<std::uint32_t>(i); r.catalog_page = 0; changed = true; }
                    }
                    ui.separator(ui.user); text(TextLocalized("browser.folders","Folders"));
                    const auto folder = [&](const std::string& path,std::string_view name) {
                        if (selected_button(std::string(name)+"##folder-"+path,s.folder == path)) { s.folder = path; r.catalog_page = 0; changed = true; }
                    };
                    folder({},TextLocalized("browser.all_folders","All folders"));
                    if (s.folder.empty()) {
                        folder("/Game/",TextLocalized("browser.game_content","Game content"));
                        folder("/Engine/",TextLocalized("browser.engine_content","Engine content"));
                    } else {
                        auto parent = s.folder.substr(0,s.folder.size()-1); const auto slash = parent.rfind('/');
                        parent = slash > 0 ? parent.substr(0,slash+1) : std::string{};
                        folder(parent,TextLocalized("browser.parent",".. Parent folder"));
                        std::set<std::string> children;
                        for (const auto& asset : *view->catalog) if (asset.path.starts_with(s.folder)) {
                            const auto slash_at = asset.path.find('/',s.folder.size());
                            if (slash_at != std::string::npos) children.insert(asset.path.substr(0,slash_at+1));
                        }
                        for (const auto& path : children) {
                            // Folder paths are engine identifiers; moving or translating them would break asset lookup.
                            auto name = path.substr(s.folder.size()); if (!name.empty()) name.pop_back(); folder(path,name);
                        }
                    }
                    ui.separator(ui.user); text(TextLocalized("browser.quick_shapes","Quick shapes"));
                    const auto shape = [&](const char* key,const char* fallback,const char* path) {
                        if (selected_button(c.localizer.Text(key,fallback),s.asset == path)) { choose(Asset{path}); changed = true; }
                    };
                    shape("primitive.cube","Cube","/Engine/BasicShapes/Cube.Cube");
                    shape("primitive.sphere","Sphere","/Engine/BasicShapes/Sphere.Sphere");
                    shape("primitive.cylinder","Cylinder","/Engine/BasicShapes/Cylinder.Cylinder");
                    shape("primitive.cone","Cone","/Engine/BasicShapes/Cone.Cone");
                }
            }
            line();
            {
                ChildPanel assets{ui,"asset-cards",middle_width,pane_height};
                if (assets.visible) {
                    if (!view->ready) wrapped(view->status);
                    if (!view->catalog_error.empty()) wrapped(view->catalog_error);
                    if (r.filtered_catalog != view->catalog || r.filtered_text != s.filter || r.filtered_folder != s.folder || r.filtered_category != s.category) {
                        r.filtered_catalog = view->catalog; r.filtered_text = s.filter; r.filtered_folder = s.folder; r.filtered_category = s.category; r.filtered.clear();
                        const AssetFilter filter(s.filter);
                        for (std::size_t i = 0; i < view->catalog->size(); ++i) {
                            const auto& asset = (*view->catalog)[i];
                            if ((!s.folder.empty() && !asset.path.starts_with(s.folder)) || (s.category && static_cast<std::uint32_t>(asset.name.category) != s.category)) continue;
                            if (filter.Match(asset)) r.filtered.push_back(i);
                        }
                    }
                    const auto count = std::to_string(r.filtered.size()); const std::array args{std::string_view(count)};
                    text(c.localizer.Format("browser.results","{0} matching assets",args));
                    if (!s.folder.empty()) text(s.folder);
                    constexpr std::size_t page_size = 12;
                    const auto pages = (r.filtered.size()+page_size-1)/page_size;
                    if (r.catalog_page >= pages) r.catalog_page = pages ? pages-1 : 0;
                    if (button(TextLocalized("action.previous","Previous"),r.catalog_page > 0)) --r.catalog_page;
                    line();
                    if (button(TextLocalized("action.next","Next"),r.catalog_page+1 < pages)) ++r.catalog_page;
                    line();
                    const auto current = std::to_string(pages ? r.catalog_page+1 : 0), total = std::to_string(pages);
                    const std::array page_args{std::string_view(current),std::string_view(total)};
                    text(c.localizer.Format("text.page","Page {0} / {1}",page_args));
                    const auto columns = middle_width >= 720 ? 4 : middle_width >= 510 ? 3 : 2;
                    const auto card_width = std::max(110.0f,(middle_width-48)/static_cast<float>(columns)-12);
                    if (ui.begin_table(ui.user,StringView("asset-grid"),columns,ANOMALY_UI_TABLE_V1_SIZING_STRETCH_PROP,0,0)) {
                        struct EndTable { const AnomalyUiServiceV1& ui; ~EndTable() { ui.end_table(ui.user); } } end{ui};
                        for (std::size_t i = r.catalog_page*page_size; i < r.filtered.size() && i < (r.catalog_page+1)*page_size; ++i) {
                            if ((i-r.catalog_page*page_size)%static_cast<std::size_t>(columns) == 0) ui.table_next_row(ui.user);
                            ui.table_next_column(ui.user);
                            const auto& asset = (*view->catalog)[r.filtered[i]];
                            auto& image = r.textures.Need(asset.path);
                            const bool drawn = r.textures.Draw(image,card_width,c.localizer);
                            bool clicked{};
                            if (!drawn) {
                                const bool loading = r.textures.Available() && view->ready && image.error.empty();
                                const auto card_label = (loading ? (image.active ? TextLocalized("browser.thumbnail_loading","Loading") : TextLocalized("browser.thumbnail_queued","Queued")) : TextLocalized("browser.thumbnail_failed","No preview"))+"###asset-preview-"+asset.path;
                                clicked = button(card_label,true,card_width,card_width);
                            }
                            if (ui.frame_state && (ui.frame_state(ui.user)&ANOMALY_UI_FRAME_V1_ITEM_HOVERED) && r.left_pressed) {
                                choose(asset); changed = true; r.dragging_asset = asset.path;
                            }
                            if (clicked) { choose(asset); changed = true; }
                            if (selected_button(asset.name.label+"###asset-name-"+asset.path,s.asset == asset.path)) { choose(asset); changed = true; }
                        }
                    }
                    if (r.filtered.empty()) wrapped(TextLocalized("browser.empty","No matching models. Change the filter or refresh the asset catalog."));
                }
            }
            line();
            {
                ChildPanel inspector{ui,"asset-inspector",inspector_width,pane_height};
                if (inspector.visible) {
                    const auto name = c.names->Name(s.asset);
                    wrapped(name.label);
                    const auto now = GetTickCount64();
                    auto& detail = r.textures.Detail(s.asset,s.preview_yaw,s.preview_elevation,s.preview_zoom,now >= r.next_viewer_capture);
                    if (now >= r.next_viewer_capture) r.next_viewer_capture = now+33;
                    const bool drawn = r.textures.Draw(detail,256,c.localizer);
                    const bool hovered = drawn && ui.frame_state && (ui.frame_state(ui.user)&ANOMALY_UI_FRAME_V1_ITEM_HOVERED);
                    if (!drawn) button(((r.textures.Available() && view->ready && detail.error.empty()) ? TextLocalized("browser.rendering","Rendering model...") : TextLocalized("browser.preview_failed","Preview unavailable"))+"###detail-preview",false,256,256);
                    if (!r.textures.Available()) wrapped(TextLocalized("error.preview_host","GPU preview support requires the updated Host. Restart the game after updating the runtime."));
                    else if (!detail.error.empty()) wrapped(detail.error);
                    if (drawn && !r.preview_reported && c.core && c.core->log) {
                        r.preview_reported = true;
                        const auto size = std::to_string(PreviewSize); const std::array preview_args{std::string_view(size),std::string_view(size)};
                        c.core->log(c.core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,StringView("CreativeMode: "+c.localizer.Format("status.preview_rendered","Native model preview rendered ({0} x {1})",preview_args)));
                    }
                    const bool right = (r.input.mouse_buttons&2) != 0;
                    const editor::Point mouse{r.input.mouse_x,r.input.mouse_y};
                    if (hovered && right && !r.orbit_dragging) { r.orbit_dragging = true; r.orbit_mouse = mouse; }
                    if (r.orbit_dragging && right) {
                        const auto dx = mouse[0]-r.orbit_mouse[0],dy = mouse[1]-r.orbit_mouse[1]; r.orbit_mouse = mouse;
                        if (dx != 0 || dy != 0) { s.preview_yaw = std::remainder(s.preview_yaw+dx*0.6,360); s.preview_elevation = std::clamp(s.preview_elevation+dy*0.4,-85.0,85.0); changed = true; }
                    } else if (!right) r.orbit_dragging = false;
                    if (r.orbit_dragging) r.textures.Interacting();
                    if (hovered && r.input.sequence != r.viewer_input_sequence && r.input.mouse_wheel) {
                        s.preview_zoom = std::clamp(s.preview_zoom*std::pow(0.9,r.input.mouse_wheel/120.0),0.25,4.0); changed = true;
                    }
                    r.viewer_input_sequence = r.input.sequence;
                    wrapped(TextLocalized("browser.orbit_help","Right drag: rotate · Wheel: zoom"));
                    if (button(TextLocalized("browser.start_placement","Place"),view->ready && view->camera_valid && Valid(s),-1)) send(Action::preview);
                    if (button(TextLocalized("editor.show_tools","Controls"),true,-1)) ShowEditorToolbar(c);
                    if (button(TextLocalized("browser.reset_view","Reset preview view"),true,-1)) { s.preview_yaw = 35; s.preview_elevation = 20; s.preview_zoom = 1; r.next_viewer_capture = 0; changed = true; }
                    if (button(TextLocalized("browser.render_again","Refresh images"),r.textures.Available(),-1)) r.textures.Refresh();
                    changed |= check(TextLocalized("label.collision","Enable collision"),s.collision);
                    ui.separator(ui.user);
                    changed |= check(TextLocalized("browser.details","Fine adjustment"),s.fine_tune);
                    if (s.fine_tune) {
                        wrapped(TextLocalized("label.asset","Mesh asset path"));
                        if (set_width) set_width(ui.user,-1);
                        if (ui.input_text(ui.user,StringView("##mesh-asset-path"),r.asset,sizeof(r.asset),0) && AssetPath(r.asset)) { s.asset = r.asset; changed = true; }
                        changed |= number(TextLocalized("label.distance","View distance (cm)"),s.distance,50);
                        changed |= number(TextLocalized("label.height","Vertical offset (cm)"),s.height,10);
                        changed |= number(TextLocalized("label.pitch","Pitch (degrees)"),s.rotation[0],s.angle_step);
                        changed |= number(TextLocalized("label.yaw","Yaw (degrees)"),s.rotation[1],s.angle_step);
                        changed |= number(TextLocalized("label.roll","Roll (degrees)"),s.rotation[2],s.angle_step);
                        auto scale = view->preview ? view->preview->item.pose.scale : s.scale;
                        bool scale_changed = uniform_scale(scale);
                        scale_changed |= number(TextLocalized("label.scale_x","Scale X"),scale[0],0.1);
                        scale_changed |= number(TextLocalized("label.scale_y","Scale Y"),scale[1],0.1);
                        scale_changed |= number(TextLocalized("label.scale_z","Scale Z"),scale[2],0.1);
                        if (scale_changed) {
                            s.scale = scale; changed = true;
                            if (view->preview) {
                                s.follow_player = false;
                                auto pose = view->preview->item.pose; pose.scale = scale;
                                if (pose.Valid()) c.drag_intent.store(std::make_shared<const editor::TransformIntent>(editor::TransformIntent{view->world,0,true,pose}));
                            }
                        }
                    }
                }
            }
        }
    }
    {
        const auto label = TextLocalized("tab.edit","Edit placements");
        Tab tab{&ui,ui.begin_tab_item(ui.user,StringView(label),nullptr,0,1) != 0};
        if (tab.open) {
            if (button(TextLocalized("action.pick","Select placed object at view center"),view->ready)) send(Action::select_aim);
            const LiveItem* selected{};
            for (const auto& item : view->items) if (item.id == r.selected) selected = &item;
            {
                ChildPanel list{ui,"placed-objects",290,440};
                if (list.visible) for (const auto& item : view->items) {
                    const auto name = c.names->Name(item.item.asset).label, id = std::to_string(item.id);
                    const std::array args{std::string_view(id),std::string_view(name)};
                    if (selected_button(c.localizer.Format("text.item","#{0} {1}",args),r.selected == item.id,260)) { select_item(item); send(Action::select_item,item.id); selected = &item; }
                }
            }
            line();
            {
                ChildPanel properties{ui,"placement-properties",0,440};
                if (properties.visible && selected) {
                    text(c.names->Name(selected->item.asset).label);
                    if (button(TextLocalized("action.duplicate","Duplicate along X"),view->ready && view->items.size() < MaximumItems && Valid(s))) send(Action::duplicate,r.selected);
                    line();
                    if (button(TextLocalized("action.remove","Delete selected"),view->ready)) send(Action::remove,r.selected);
                    changed |= check(TextLocalized("browser.details","Details and fine adjustment"),s.fine_tune);
                    if (s.fine_tune) {
                        number(TextLocalized("label.x","X (cm)"),r.pose.position[0],s.move_step);
                        number(TextLocalized("label.y","Y (cm)"),r.pose.position[1],s.move_step);
                        number(TextLocalized("label.z","Z (cm)"),r.pose.position[2],s.move_step);
                        number(TextLocalized("label.pitch","Pitch (degrees)"),r.pose.rotation[0],s.angle_step);
                        number(TextLocalized("label.yaw","Yaw (degrees)"),r.pose.rotation[1],s.angle_step);
                        number(TextLocalized("label.roll","Roll (degrees)"),r.pose.rotation[2],s.angle_step);
                        uniform_scale(r.pose.scale);
                        number(TextLocalized("label.scale_x","Scale X"),r.pose.scale[0],0.1);
                        number(TextLocalized("label.scale_y","Scale Y"),r.pose.scale[1],0.1);
                        number(TextLocalized("label.scale_z","Scale Z"),r.pose.scale[2],0.1);
                        check(TextLocalized("label.collision","Enable collision"),r.collision);
                        if (button(TextLocalized("action.apply","Apply transform"),view->ready && r.pose.Valid())) send(Action::move,r.selected);
                        line();
                        if (button(TextLocalized("action.snap","Snap to grid"),view->ready && r.pose.Valid() && Valid(s))) { Snap(r.pose,s.grid,s.angle_step); send(Action::move,r.selected); }
                        wrapped(selected->item.asset);
                    }
                    if (selected->item.collision && !selected->has_collision_data) wrapped(TextLocalized("hint.missing_collision","This mesh has no collision body or physics asset; enabling collision cannot make it solid"));
                } else if (properties.visible) wrapped(TextLocalized("hint.select","Select one of your placed objects to edit it"));
            }
            if (button(TextLocalized("action.clear","Remove all placed objects"),view->ready && (!view->items.empty() || view->has_preview))) send(Action::clear);
        }
    }
    {
        const auto label = TextLocalized("tab.layout","Layout and settings");
        Tab tab{&ui,ui.begin_tab_item(ui.user,StringView(label),nullptr,0,1) != 0};
        if (tab.open) {
            if (c.persistence.Failed()) wrapped(TextLocalized("error.config","Settings or layout could not be saved; existing saved data is preserved"));
            changed |= check(TextLocalized("editor.enabled","Show world editor handles"),s.editor_enabled);
            changed |= check(TextLocalized("label.snap","Snap new placements to grid"),s.snap);
            changed |= number(TextLocalized("label.grid","Grid size (cm)"),s.grid,10);
            changed |= number(TextLocalized("label.move_step","Move step (cm)"),s.move_step,10);
            changed |= number(TextLocalized("label.angle_step","Angle step (degrees)"),s.angle_step,5);
            if (button(TextLocalized("action.save","Save current layout"),view->ready && !view->restoring)) send(Action::save);
            line();
            if (button(TextLocalized("action.restore","Restore saved layout"),view->ready && view->items.empty() && !view->has_preview && !view->restoring && view->saved.world == view->world_path && !view->saved.items.empty())) send(Action::restore);
            if (view->restoring && button(TextLocalized("action.stop_restore","Stop restoring"))) send(Action::stop_restore);
            const auto placed = std::to_string(view->items.size()),saved = std::to_string(view->saved.items.size());
            const std::array args{std::string_view(placed),std::string_view(saved)};
            text(c.localizer.Format("text.counts","Placed: {0} / 512; saved: {1}",args));
            wrapped(TextLocalized("hint.settings","Settings save automatically. Save layout stores the current placements separately; it replaces the previous saved layout."));
            wrapped(TextLocalized("hint.restore","Restore requires an empty placement list and the same world. Existing world props are never changed."));
            wrapped(TextLocalized("hint.collision","Collision uses the mesh's original collision data. Some decorative meshes have none; use scaled cubes for solid platforms."));
            wrapped(TextLocalized("hint.stop","Disabling or reloading removes these local objects within five seconds of unpaused game time. Save the layout before stopping."));
        }
    }
    if (changed && Valid(s)) c.persistence.SettingsChanged(s);
    if (!Valid(s)) wrapped(TextLocalized("error.settings","Enter valid distances, steps and positive scale values before placing objects"));
}
