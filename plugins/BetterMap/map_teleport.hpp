#pragma once
#include "map_api.hpp"
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace collectible_map {
enum class TeleportStatus { Waiting, Ready, Unavailable, OutOfScene, InvalidPoint, Loading, NoGround, Rejected, Submitted };

class MapTeleport {
    using Clock = std::chrono::steady_clock;
    using Point = std::array<double, 2>;
    using ReadMapData = uintptr_t(ANOMALY_CALL*)(uintptr_t);
    using ReadVector = Position*(ANOMALY_CALL*)(uintptr_t, Position*);
    using MapToWorld = Position*(ANOMALY_CALL*)(uintptr_t, Position*, uintptr_t, const NameId*,
                                              int32_t, const Point*, const Point*, const Position*,
                                              const Position*, const Position*);
    struct Click {
        uintptr_t container, widget;
        Point screen;
        Clock::time_point time;
    };
    struct MousePress { POINT start; bool moved{}; };
    struct Target {
        AnomalyGenerationHandleV1 world, player, pawn;
        Position position, origin;
        Clock::time_point deadline, next_trace;
    };

public:
    std::atomic<bool> enabled{};
    std::atomic<bool> pick_once{};
    std::atomic<bool> cancel_pending{};
    std::atomic<TeleportStatus> status{TeleportStatus::Waiting};

    void RequestAtCursor() noexcept {
        if (enabled.load() && Foreground()) hotkey_time_ = GetTickCount64();
    }

    void Resolve(const anomaly::sdk::Host& host, Engine& engine, const anomaly::sdk::Profile& profile) {
        const auto signatures = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto symbol = [&](const char* name) {
            return anomaly::sdk::ResolveProfileSymbol(signatures.get(), engine.core, profile, name).address;
        };
        convert_ = reinterpret_cast<MapToWorld>(symbol("MapClickToWorld"));
        map_data_ = reinterpret_cast<ReadMapData>(symbol("BigMapData"));
        map_extent_ = reinterpret_cast<ReadVector>(symbol("BigMapExtent"));
        map_center_ = reinterpret_cast<ReadVector>(symbol("BigMapCenter"));
        layout_valid_ = symbol("BigMapLayout") != 0;
        level_offset_ = profile.Value("ClickMapLevel", 0x2000);
        context_offset_ = profile.Value("ClickMapContext", 0x2000);
        floor_area_offset_ = profile.Value("ClickMapFloorArea", 0x2000);
        floor_offset_ = profile.Value("ClickMapFloor", 0x2000);
        bounds_offset_ = profile.Value("ClickMapBounds", 0x100);
        data_slot_ = profile.Value("ClickMapDataSlot", 0x1000);
        extent_slot_ = profile.Value("ClickMapExtentSlot", 0x1000);
        center_slot_ = profile.Value("ClickMapCenterSlot", 0x1000);
    }

    AnomalyStatusV1 Detach() noexcept {
        pick_once = false;
        cancel_pending = false;
        ClearTarget();
        if (subscription_.id) {
            const auto result = events_->unsubscribe(events_->user, subscription_);
            if (!anomaly::sdk::Succeeded(result) && result.code != ANOMALY_STATUS_V1_NOT_FOUND) return result;
            subscription_ = {};
        }
        pending_.reset();
        hotkey_time_ = 0;
        mouse_press_.reset();
        input_ready_ = false;
        left_down_ = false;
        retry_at_ = {};
        next_teleport_ = {};
        status = TeleportStatus::Waiting;
        return anomaly::sdk::Ok();
    }

    void Update(const anomaly::sdk::Host& host, Engine& engine, const AnomalyUe5FrameworkServiceV1* framework,
                const AnomalyUe5WorldServiceV1* worlds, const AnomalyNtePlayerServiceV1* players,
                Clock::time_point now) noexcept {
        try { Tick(host, engine, framework, worlds, players, now); }
        catch (...) {
            ClearTarget();
            pending_.reset();
            status = TeleportStatus::Unavailable;
        }
    }

private:
    void Tick(const anomaly::sdk::Host& host, Engine& engine, const AnomalyUe5FrameworkServiceV1* framework,
              const AnomalyUe5WorldServiceV1* worlds, const AnomalyNtePlayerServiceV1* players,
              Clock::time_point now) {
        if (!checked_) {
            checked_ = true;
            binding_ready_ = Bind(host, engine, players);
            if (engine.core && engine.core->log) {
                const auto message = binding_ready_ ? "Collectible Map: click teleport interfaces validated"
                                                    : "Collectible Map: click teleport interfaces unavailable";
                engine.core->log(engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
                if (binding_ready_ && FindMapAvailable())
                    engine.core->log(engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                                     anomaly::sdk::StringView("Collectible Map: direct map input interfaces validated"));
                if (binding_ready_ && hold_api_.engage && hold_api_.release && hold_api_.snapshot &&
                    teleport_->struct_size >= sizeof(*teleport_) && teleport_->preload && teleport_->cancel_preload)
                    engine.core->log(engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                                     anomaly::sdk::StringView("Collectible Map: destination preload available"));
            }
        }
        if (!enabled.load()) {
            if (subscription_.id || retry_at_ != Clock::time_point{}) static_cast<void>(Detach());
            return;
        }
        if (!input_ready_ && !subscription_.id) {
            if (now < retry_at_) return;
            retry_at_ = now + std::chrono::seconds(2);
            if (!binding_ready_) binding_ready_ = Bind(host, engine, players);
            framework_ = framework;
            events_ = engine.events;
            const bool observing = binding_ready_ &&
                anomaly::sdk::Succeeded(events_->subscribe(events_->user, Observe, this, &subscription_));
            input_ready_ = binding_ready_ && FindMapAvailable();
            status = observing || input_ready_ ? TeleportStatus::Ready : TeleportStatus::Unavailable;
            if (input_ready_ && engine.core && engine.core->log)
                engine.core->log(engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                                 anomaly::sdk::StringView("Collectible Map: direct map mouse input ready"));
        }
        const bool map_mouse_click = input_ready_ && PollMouse();
        const auto hotkey_time = hotkey_time_.exchange(0);
        const bool map_hotkey = hotkey_time && GetTickCount64() - hotkey_time <= 300 && Foreground();
        if (target_) {
            pending_.reset();
            if (cancel_pending.exchange(false)) {
                ClearTarget();
                status = TeleportStatus::Ready;
                return;
            }
            ContinueTarget(engine, players, now);
            return;
        }
        cancel_pending = false;
        if (now < next_teleport_) {
            pending_.reset();
            return;
        }
        if (!pending_ && (map_mouse_click || map_hotkey) && input_ready_) {
            pending_ = ReadHoveredMap(engine, players, now);
            if (!pending_) {
                Finish(engine, TeleportStatus::OutOfScene, now);
                return;
            }
        }
        if (map_hotkey && !input_ready_ && !pending_) {
            Finish(engine, TeleportStatus::Unavailable, now);
            return;
        }
        if (!pending_) return;
        const auto click = *pending_;
        pending_.reset();
        if (now - click.time > std::chrono::milliseconds(500)) return;
        pick_once = false;
        next_teleport_ = now + std::chrono::milliseconds(250);
        Finish(engine, RunClick(engine, worlds, players, click, now), now);
    }

    void Finish(Engine& engine, TeleportStatus result, Clock::time_point now) {
        status = result;
        if (result == TeleportStatus::Submitted) next_teleport_ = now + std::chrono::seconds(3);
        if (!engine.core || !engine.core->log) return;
        constexpr std::array messages{"waiting", "ready", "interface unavailable", "outside main map",
                                      "coordinate conversion failed", "loading destination",
                                      "no ground confirmed", "request rejected", "request accepted"};
        const auto message = std::string("Collectible Map: click teleport ") + messages[static_cast<size_t>(result)];
        engine.core->log(engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
    }

    static bool Foreground() {
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    bool PollMouse() {
        const bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool was_pressed = std::exchange(left_down_, pressed);
        const bool start_click = pressed && !was_pressed &&
            (pick_once.load() || (GetAsyncKeyState(VK_CONTROL) & 0x8000));
        if (!start_click && !mouse_press_) return false;
        if (!Foreground()) {
            mouse_press_.reset();
            return false;
        }
        POINT cursor{};
        if (!GetCursorPos(&cursor)) {
            mouse_press_.reset();
            return false;
        }
        if (start_click) mouse_press_ = MousePress{cursor};
        if (!mouse_press_) return false;
        const auto& start = mouse_press_->start;
        if (std::abs(cursor.x - start.x) > 6 || std::abs(cursor.y - start.y) > 6)
            mouse_press_->moved = true;
        if (pressed) return false;
        const bool click = !mouse_press_->moved;
        mouse_press_.reset();
        return click;
    }

    bool FindMapAvailable() const {
        return find_map_.function && mouse_position_.function && visible_.function && hovered_.function &&
               widget_library_ && layout_library_ && container_class_;
    }

    std::optional<Click> ReadHoveredMap(Engine& engine, const AnomalyNtePlayerServiceV1* players,
                                        Clock::time_point now) const {
        if (!engine.free_memory) return {};
        AnomalyGenerationHandleV1 controller_handle{};
        if (!anomaly::sdk::Succeeded(players->get_object_handles(players->user, nullptr, &controller_handle)))
            return {};
        const auto controller = engine.Resolve(controller_handle);
        if (!controller) return {};

        alignas(8) std::array<uint8_t, 40> params{};
        Write(params, 0, controller.address);
        Write(params, 24, container_class_.address);
        if (!engine.Call(widget_library_, find_map_, params.data(), find_map_.size)) return {};
        const auto found = Read<Array>(params, 8);
        if (!found.data) return {};
        const auto release = [&](void* data) { engine.free_memory(data); };
        const std::unique_ptr<void, decltype(release)> owned(reinterpret_cast<void*>(found.data), release);
        if (found.count <= 0 || found.count > 64 || found.capacity < found.count) return {};

        Point screen{};
        if (!engine.Call(layout_library_, mouse_position_, &screen, sizeof(screen)) || !Finite(screen)) return {};
        for (int32_t index = 0; index < found.count; ++index) {
            uintptr_t address{}, widget_address{};
            uint8_t visible{}, hovered{};
            if (!engine.Read(found.data + index * sizeof(address), address)) break;
            const auto container = engine.Reference(address);
            if (!engine.IsA(container, "HTUI_BigMapContainer") ||
                !engine.Call(container, visible_, &visible, sizeof(visible)) || !visible ||
                !engine.Member(container, "BigMapWidget", "ObjectProperty", widget_address)) continue;
            const auto widget = engine.Reference(widget_address);
            if (engine.IsA(widget, "HTUI_BigMap") &&
                engine.Call(widget, hovered_, &hovered, sizeof(hovered)) && hovered)
                return Click{container.address, widget.address, screen, now};
        }
        return {};
    }

    static void ANOMALY_CALL Observe(void* user, uintptr_t object, uintptr_t function, void* parameters) noexcept {
        auto& self = *static_cast<MapTeleport*>(user);
        if (!function || (function != self.click_.function.address && function != self.icon_click_.function.address) ||
            !parameters || !self.enabled.load() ||
            !self.framework_->is_game_thread(self.framework_->user) || !Foreground() ||
            (!self.pick_once.load() && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))) return;

        struct ClickParams { uintptr_t widget; Point screen; } input{};
        std::memcpy(&input, parameters, sizeof(input));
        if (!input.widget || !Finite(input.screen)) return;
        self.pending_ = Click{object, input.widget, input.screen, Clock::now()};
    }

    static bool Finite(const auto& vector) {
        return std::ranges::all_of(vector, [](double value) { return std::isfinite(value); });
    }

    static bool SameHandle(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) {
        return a.id != 0 && a.id == b.id && a.generation == b.generation;
    }

    bool Bind(const anomaly::sdk::Host& host, Engine& engine, const AnomalyNtePlayerServiceV1* players) {
        if (!convert_ || !map_data_ || !map_extent_ || !map_center_ || !layout_valid_ ||
            !level_offset_ || !context_offset_ || !floor_area_offset_ || !floor_offset_ ||
            !bounds_offset_ || !data_slot_ || !extent_slot_ || !center_slot_ ||
            !engine.events || !engine.events->subscribe || !engine.events->unsubscribe) return false;
        session_ = host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
        teleport_ = host.Query<AnomalyNtePlayerTeleportServiceV1>(ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID).get();
        const auto hold = host.Query<AnomalyNtePlayerHoldServiceV1>(ANOMALY_NTE_PLAYER_HOLD_SERVICE_V1_ID);
        if (hold && hold->struct_size >= sizeof(*hold.get()) && hold->engage && hold->release && hold->snapshot)
            hold_api_ = *hold.get();
        else
            hold_api_ = {sizeof(hold_api_), 1, players->user,
                         players->hold_engage, players->hold_release, players->hold_snapshot};
        if (!session_ || session_->struct_size < sizeof(*session_) || !session_->snapshot ||
            !teleport_ || teleport_->struct_size < offsetof(AnomalyNtePlayerTeleportServiceV1, preload) ||
            !teleport_->teleport) return false;
        const auto map_click = [&](const char* name) {
            return engine.Bind("HTGame", "HTUI_BigMapContainer_Base", name, 4, 48,
                {{"MiniMapWidget", 0, 8, "ObjectProperty"}, {"InScreenPosition", 8, 16, "StructProperty"},
                 {"ClickedIconObject", 24, 8, "ObjectProperty"}, {"ClickedIcons", 32, 16, "ArrayProperty"}});
        };
        click_ = map_click("OnClickBigMap");
        icon_click_ = map_click("OnClickBigMapObject");
        find_map_ = engine.Bind("UMG", "WidgetBlueprintLibrary", "GetAllWidgetsOfClass", 4, 33,
            {{"WorldContextObject", 0, 8, "ObjectProperty"}, {"FoundWidgets", 8, 16, "ArrayProperty"},
             {"WidgetClass", 24, 8, "ClassProperty"}, {"TopLevelOnly", 32, 1, "BoolProperty"}});
        mouse_position_ = engine.Bind("UMG", "WidgetLayoutLibrary", "GetMousePositionOnPlatform", 1, 16,
            {{"ReturnValue", 0, 16, "StructProperty"}});
        visible_ = engine.Bind("UMG", "Widget", "IsVisible", 1, 1, {{"ReturnValue", 0, 1, "BoolProperty"}});
        hovered_ = engine.Bind("UMG", "Widget", "IsHovered", 1, 1, {{"ReturnValue", 0, 1, "BoolProperty"}});
        widget_library_ = engine.Find("/Script/UMG.Default__WidgetBlueprintLibrary");
        layout_library_ = engine.Find("/Script/UMG.Default__WidgetLayoutLibrary");
        container_class_ = engine.Find("/Script/HTGame.HTUI_BigMapContainer");
        geometry_ = engine.Bind("UMG", "Widget", "GetCachedGeometry", 1, 56,
            {{"ReturnValue", 0, 56, "StructProperty"}});
        to_local_ = engine.Bind("UMG", "SlateBlueprintLibrary", "AbsoluteToLocal", 3, 88,
            {{"Geometry", 0, 56, "StructProperty"}, {"AbsoluteCoordinate", 56, 16, "StructProperty"},
             {"ReturnValue", 72, 16, "StructProperty"}});
        local_size_ = engine.Bind("UMG", "SlateBlueprintLibrary", "GetLocalSize", 2, 72,
            {{"Geometry", 0, 56, "StructProperty"}, {"ReturnValue", 56, 16, "StructProperty"}});
        get_subsystem_ = engine.Bind("Engine", "SubsystemBlueprintLibrary", "GetWorldSubsystem", 3, 24,
            {{"ContextObject", 0, 8, "ObjectProperty"}, {"Class", 8, 8, "ClassProperty"},
             {"ReturnValue", 16, 8, "ObjectProperty"}});
        ground_trace_ = engine.Bind("Engine", "KismetSystemLibrary", "LineTraceSingleForObjects", 13, 409,
            {{"WorldContextObject", 0, 8, "ObjectProperty"}, {"Start", 8, 24, "StructProperty"},
             {"End", 32, 24, "StructProperty"}, {"ObjectTypes", 56, 16, "ArrayProperty"},
             {"ActorsToIgnore", 80, 16, "ArrayProperty"}, {"OutHit", 104, 264, "StructProperty"},
             {"bIgnoreSelf", 368, 1, "BoolProperty"}, {"ReturnValue", 408, 1, "BoolProperty"}});
        capsule_height_ = engine.Bind("Engine", "CapsuleComponent", "GetScaledCapsuleHalfHeight", 1, 4,
            {{"ReturnValue", 0, 4, "FloatProperty"}});
        slate_ = engine.Find("/Script/UMG.Default__SlateBlueprintLibrary");
        subsystem_library_ = engine.Find("/Script/Engine.Default__SubsystemBlueprintLibrary");
        subsystem_type_ = engine.Find("/Script/HTGame.HTMiniMapSubsystem");
        system_library_ = engine.Find("/Script/Engine.Default__KismetSystemLibrary");
        const auto hit = engine.Find("/Script/Engine.HitResult");
        if ((!click_.function && !icon_click_.function && !FindMapAvailable()) ||
            !geometry_.function || !to_local_.function || !local_size_.function ||
            !get_subsystem_.function || !ground_trace_.function || !capsule_height_.function || !slate_ || !subsystem_library_ ||
            !subsystem_type_ || !system_library_ || !hit ||
            engine.Field(hit.address, "ImpactPoint", 24, "StructProperty") != 40 ||
            engine.Field(hit.address, "ImpactNormal", 24, "StructProperty") != 88 ||
            engine.Field(hit.address, "bStartPenetrating", 1, "BoolProperty") != 173 ||
            engine.Field(hit.address, "bBlockingHit", 1, "BoolProperty") != 173) return false;

        return true;
    }

    bool MapCoordinates(Engine& engine, Ref container, Ref widget, Ref subsystem,
                        const Point& screen, Position& position) const {
        alignas(16) std::array<uint8_t, 88> local{};
        alignas(16) std::array<uint8_t, 72> size{};
        if (!engine.Call(widget, geometry_, local.data(), geometry_.size)) return false;
        std::memcpy(size.data(), local.data(), geometry_.size);
        if (!engine.Call(slate_, local_size_, size.data(), size.size())) return false;
        const auto widget_size = Read<Point>(size, 56);
        Write(local, 56, screen);
        if (!engine.Call(slate_, to_local_, local.data(), local.size()) ||
            !engine.Call(container, geometry_, size.data(), geometry_.size) ||
            !engine.Call(slate_, local_size_, size.data(), size.size())) return false;
        const auto point = Read<Point>(local, 72);
        const auto viewport = Read<Point>(size, 56);
        if (!Finite(point) || !Finite(widget_size) || !Finite(viewport) || viewport[0] < 1 || viewport[1] < 1 ||
            point[0] < 0 || point[1] < 0 || point[0] >= widget_size[0] || point[1] >= widget_size[1]) return false;

        uintptr_t context{}, vtable{}, data_getter{}, extent_getter{}, center_getter{};
        NameId floor_area{};
        int32_t floor{};
        if (!engine.Read(widget.address + context_offset_, context) || !context ||
            !engine.Read(widget.address + floor_area_offset_, floor_area) ||
            !engine.Read(widget.address + floor_offset_, floor) ||
            !engine.Read(widget.address, vtable) ||
            !engine.Read(vtable + data_slot_, data_getter) || data_getter != reinterpret_cast<uintptr_t>(map_data_) ||
            !engine.Read(vtable + extent_slot_, extent_getter) || extent_getter != reinterpret_cast<uintptr_t>(map_extent_) ||
            !engine.Read(vtable + center_slot_, center_getter) || center_getter != reinterpret_cast<uintptr_t>(map_center_))
            return false;

        position.fill(std::numeric_limits<double>::quiet_NaN());
        return Convert(engine, widget.address, subsystem.address, context, floor_area, floor, point, viewport, position) &&
               Finite(position) && position != Position{} &&
               std::ranges::all_of(position, [](double value) { return std::abs(value) < 10'000'000.; });
    }

    bool Convert(Engine& engine, uintptr_t widget, uintptr_t subsystem, uintptr_t context, const NameId& floor_area,
                 int32_t floor, const Point& point, const Point& viewport, Position& position) const noexcept {
        __try {
            Position bounds{}, center{}, extent{};
            const auto data = map_data_(widget);
            // These inputs follow the game's map click handler, including its tile center and zoom extent.
            if (!data || !engine.Read(data + bounds_offset_, bounds) || !Finite(bounds) ||
                map_center_(widget, &center) != &center || !Finite(center) ||
                map_extent_(widget, &extent) != &extent || !Finite(extent) || extent[0] <= 0 || extent[1] <= 0)
                return false;
            return convert_(subsystem, &position, context, &floor_area, floor, &point, &viewport,
                            &bounds, &center, &extent) == &position;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool Ground(Engine& engine, Ref pawn, Position& position) const {
        constexpr double trace_height = 30'000.;
        constexpr size_t hit_offset = 104;
        constexpr std::array<uint8_t, 1> object_types{0}; // WorldStatic
        alignas(16) std::array<uint8_t, 416> params{};
        Write(params, 0, pawn.address);
        Write(params, 8, Position{position[0], position[1], position[2] + trace_height});
        Write(params, 32, Position{position[0], position[1], position[2] - trace_height});
        Write(params, 56, Borrow(std::span<const uint8_t>(object_types)));
        Write(params, 368, uint8_t{1});
        if (!engine.Call(system_library_, ground_trace_, params.data(), ground_trace_.size) ||
            !params[408] || (params[hit_offset + 173] & 3) != 1) return false;
        const auto impact = Read<Position>(params, hit_offset + 40);
        const auto normal = Read<Position>(params, hit_offset + 88);
        if (!Finite(impact) || !Finite(normal) || normal[2] < .7 ||
            std::abs(impact[0] - position[0]) > 1 || std::abs(impact[1] - position[1]) > 1 ||
            std::abs(impact[2] - position[2]) > trace_height) return false;
        uintptr_t capsule_address{};
        float half_height{};
        if (!engine.Member(pawn, "CapsuleComponent", "ObjectProperty", capsule_address)) return false;
        const auto capsule = engine.Reference(capsule_address);
        if (!engine.IsA(capsule, "CapsuleComponent") ||
            !engine.Call(capsule, capsule_height_, &half_height, sizeof(half_height)) ||
            !std::isfinite(half_height) || half_height < 1.f || half_height > 300.f) return false;
        position[2] = impact[2] + half_height + 5.;
        return true;
    }

    TeleportStatus RunClick(Engine& engine, const AnomalyUe5WorldServiceV1* worlds,
                            const AnomalyNtePlayerServiceV1* players, const Click& click, Clock::time_point now) {
        using anomaly::sdk::Succeeded;
        if (!Foreground() || !worlds->current || !worlds->snapshot || !players->snapshot)
            return TeleportStatus::OutOfScene;
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
        AnomalyGenerationHandleV1 world{}, pawn_handle{};
        AnomalyUe5WorldSnapshotV1 world_info{sizeof(world_info)};
        if (!Succeeded(session_->snapshot(session_->user, &session)) ||
            session.state != ANOMALY_NTE_SESSION_V1_WORLD_READY ||
            !Succeeded(worlds->current(worlds->user, &world)) || !SameHandle(world, session.world) ||
            !Succeeded(worlds->snapshot(worlds->user, world, &world_info)) ||
            !MainWorld(engine, world_info.name_id) ||
            !Succeeded(players->snapshot(players->user, &player)) || !player.handle.id ||
            (player.flags & (ANOMALY_NTE_SNAPSHOT_V1_VALID | ANOMALY_NTE_SNAPSHOT_V1_STALE)) != ANOMALY_NTE_SNAPSHOT_V1_VALID ||
            !Succeeded(players->get_object_handles(players->user, &pawn_handle, nullptr))) return TeleportStatus::OutOfScene;
        const auto pawn = engine.Resolve(pawn_handle);
        const auto container = engine.Reference(click.container);
        const auto widget = engine.Reference(click.widget);
        uintptr_t current_widget{};
        Array level_text{};
        std::wstring level;
        if (!pawn || !engine.IsA(container, "HTUI_BigMapContainer") || !engine.IsA(widget, "HTUI_BigMap") ||
            !engine.Member(container, "BigMapWidget", "ObjectProperty", current_widget) || current_widget != widget.address ||
            !engine.Read(widget.address + level_offset_, level_text) || !ReadString(engine, level_text, level) ||
            _wcsicmp(level.c_str(), L"XL_map_bigworld_test") != 0) return TeleportStatus::OutOfScene;

        std::array<uintptr_t, 3> subsystem_params{pawn.address, subsystem_type_.address, 0};
        if (!engine.Call(subsystem_library_, get_subsystem_, subsystem_params.data(), sizeof(subsystem_params)))
            return TeleportStatus::Unavailable;
        const auto subsystem = engine.Reference(subsystem_params[2]);
        Position position{};
        if (!engine.IsA(subsystem, "HTMiniMapSubsystem") ||
            !MapCoordinates(engine, container, widget, subsystem, click.screen, position)) return TeleportStatus::InvalidPoint;
        if (Ground(engine, pawn, position)) return Submit(session.world, player.handle, position);
        return Preload(session.world, player, pawn_handle, position, now);
    }

    TeleportStatus Submit(AnomalyGenerationHandleV1 world, AnomalyGenerationHandleV1 player,
                          const Position& position) const {
        if (!enabled.load()) return TeleportStatus::Waiting;
        AnomalyNtePlayerTeleportRequestV1 request{sizeof(request), 0, world, player,
            {position[0], position[1], position[2]}};
        return anomaly::sdk::Succeeded(teleport_->teleport(teleport_->user, &request))
            ? TeleportStatus::Submitted : TeleportStatus::Rejected;
    }

    TeleportStatus Preload(AnomalyGenerationHandleV1 world, const AnomalyNtePlayerSnapshotV1& player,
                            AnomalyGenerationHandleV1 pawn, const Position& position, Clock::time_point now) {
        using anomaly::sdk::Succeeded;
        if (!enabled.load() || teleport_->struct_size < sizeof(*teleport_) ||
            !teleport_->preload || !teleport_->cancel_preload) return TeleportStatus::NoGround;
        if (!hold_api_.engage || !hold_api_.release || !hold_api_.snapshot) return TeleportStatus::NoGround;
        AnomalyNtePlayerHoldSnapshotV1 hold{sizeof(hold)};
        if (!Succeeded(hold_api_.snapshot(hold_api_.user, &hold)) || (hold.flags & ANOMALY_NTE_PLAYER_HOLD_V1_HELD) ||
            !Succeeded(hold_api_.engage(hold_api_.user))) return TeleportStatus::NoGround;

        constexpr auto duration = std::chrono::milliseconds(3500);
        AnomalyNtePlayerTeleportPreloadRequestV1 preload{sizeof(preload), 0,
            {position[0], position[1], position[2]}, static_cast<uint32_t>(duration.count()), 0};
        if (!Succeeded(teleport_->preload(teleport_->user, &preload))) {
            static_cast<void>(hold_api_.release(hold_api_.user));
            return TeleportStatus::NoGround;
        }
        target_ = Target{world, player.handle, pawn, position, {player.position[0], player.position[1], player.position[2]},
                         now + duration, now + std::chrono::milliseconds(250)};
        return TeleportStatus::Loading;
    }

    void ClearTarget() noexcept {
        if (!target_) return;
        target_.reset();
        static_cast<void>(teleport_->cancel_preload(teleport_->user));
        static_cast<void>(hold_api_.release(hold_api_.user));
    }

    void ContinueTarget(Engine& engine, const AnomalyNtePlayerServiceV1* players, Clock::time_point now) {
        using anomaly::sdk::Succeeded;
        if (now < target_->next_trace) return;
        const auto target = *target_;
        target_->next_trace = now + std::chrono::milliseconds(200);
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
        AnomalyGenerationHandleV1 pawn_handle{};
        const bool same_player = Succeeded(session_->snapshot(session_->user, &session)) &&
            session.state == ANOMALY_NTE_SESSION_V1_WORLD_READY && SameHandle(session.world, target.world) &&
            Succeeded(players->snapshot(players->user, &player)) && SameHandle(player.handle, target.player) &&
            (player.flags & (ANOMALY_NTE_SNAPSHOT_V1_VALID | ANOMALY_NTE_SNAPSHOT_V1_STALE)) == ANOMALY_NTE_SNAPSHOT_V1_VALID &&
            Succeeded(players->get_object_handles(players->user, &pawn_handle, nullptr)) &&
            SameHandle(pawn_handle, target.pawn);
        if (!same_player) {
            ClearTarget();
            Finish(engine, TeleportStatus::OutOfScene, now);
            return;
        }
        if (std::hypot(player.position[0] - target.origin[0], player.position[1] - target.origin[1],
                       player.position[2] - target.origin[2]) >= 150.) {
            ClearTarget();
            Finish(engine, TeleportStatus::OutOfScene, now);
            return;
        }
        if (!Foreground()) {
            ClearTarget();
            Finish(engine, TeleportStatus::Ready, now);
            return;
        }
        auto position = target.position;
        const auto pawn = engine.Resolve(pawn_handle);
        if (pawn && Ground(engine, pawn, position)) {
            // The host's arrival hold takes over from the preload hold on success.
            const auto result = Submit(target.world, target.player, position);
            if (result == TeleportStatus::Submitted) target_.reset();
            else ClearTarget();
            Finish(engine, result, now);
        } else if (now >= target.deadline) {
            ClearTarget();
            Finish(engine, TeleportStatus::NoGround, now);
        }
    }

    static bool MainWorld(const Engine& engine, uint32_t name_id) {
        std::array<char, 128> name{};
        size_t size = name.size();
        return engine.names && engine.names->resolve_utf8 &&
            anomaly::sdk::Succeeded(engine.names->resolve_utf8(engine.names->user, name_id, name.data(), &size)) &&
            size < name.size() && _stricmp(name.data(), "XL_map_bigworld_test") == 0;
    }

    const AnomalyUe5FrameworkServiceV1* framework_{};
    const AnomalyUe5ProcessEventServiceV1* events_{};
    const AnomalyNteSessionServiceV1* session_{};
    const AnomalyNtePlayerTeleportServiceV1* teleport_{};
    AnomalyNtePlayerHoldServiceV1 hold_api_{};
    AnomalyGenerationHandleV1 subscription_{};
    Bound click_, icon_click_, geometry_, to_local_, local_size_, get_subsystem_, ground_trace_, capsule_height_;
    Bound find_map_, mouse_position_, visible_, hovered_;
    Ref slate_, subsystem_library_, subsystem_type_, system_library_;
    Ref widget_library_, layout_library_, container_class_;
    MapToWorld convert_{};
    ReadMapData map_data_{};
    ReadVector map_extent_{}, map_center_{};
    bool layout_valid_{}, checked_{}, binding_ready_{};
    bool input_ready_{}, left_down_{};
    uint32_t level_offset_{}, context_offset_{}, floor_area_offset_{}, floor_offset_{}, bounds_offset_{};
    uint32_t data_slot_{}, extent_slot_{}, center_slot_{};
    std::optional<Click> pending_;
    std::atomic<uint64_t> hotkey_time_{};
    std::optional<MousePress> mouse_press_;
    std::optional<Target> target_;
    Clock::time_point retry_at_{}, next_teleport_{};
};
}
