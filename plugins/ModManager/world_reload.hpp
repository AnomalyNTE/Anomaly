#pragma once
#include "native_access.hpp"
#include <atomic>
#include <chrono>

namespace mod_manager {
class WorldReload {
public:
    using Clock = std::chrono::steady_clock;
    enum class Result { Idle, Waiting, Reloaded, Unavailable, DifferentWorld, Timeout, Retained };

    bool Install(const AnomalyHostApiV1* api, NativeAccess* native) {
        native_ = native;
        hooks_ = anomaly::sdk::Host(api).Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID).get();
        const auto target = native_->profile.Address("CleanupWorld");
        if (!target || !hooks_ || hooks_->struct_size < sizeof(*hooks_) ||
            !hooks_->create || !hooks_->release || !hooks_->begin_callback || !hooks_->end_callback) return false;
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION; request.target = target;
        request.detour = reinterpret_cast<void*>(&CleanupWorld);
        request.label = anomaly::sdk::StringView("mod-world-cleanup");
        active_.store(this, std::memory_order_release);
        // The service publishes both outputs before enabling the detour.
        const auto status = hooks_->create(hooks_->user, &request, &original_cleanup_, &hook_);
        if (status.code) return false;
        running_.store(true, std::memory_order_release);
        return true;
    }
    AnomalyStatusV1 Stop() noexcept {
        running_.store(false, std::memory_order_release);
        if (!hook_.id) return {ANOMALY_STATUS_V1_OK};
        const auto status = hooks_->release(hooks_->user, hook_);
        if (!status.code || status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            active_.store(nullptr, std::memory_order_release);
            hook_ = {}; return {ANOMALY_STATUS_V1_OK};
        }
        return status; // Keep callback state alive if the host could not drain it.
    }
    void Bind(NativeAccess* native, const AnomalyNteSessionServiceV1* session) {
        native_ = native; session_ = session; Cancel();
    }
    void Cancel() {
        armed_ = false; replaced_ = false; result_ = Result::Idle;
        original_ = {}; settings_ = {}; old_data_ = {}; old_class_ = {}; map_.clear();
    }
    bool Busy() const { return result_ == Result::Waiting; }
    Result State() const { return result_; }

    Result Begin(Clock::time_point now) {
        Cancel();
        if (!running_.load(std::memory_order_acquire) || !Ready() || !Prepare())
            return result_ = Result::Unavailable;
        game_thread_.store(GetCurrentThreadId(), std::memory_order_release);
        armed_ = true;
        if (!native_->ReloadWorld(original_, map_)) {
            armed_ = false;
            return result_ = Result::Unavailable;
        }
        deadline_ = now + std::chrono::seconds(120);
        next_poll_ = now + std::chrono::milliseconds(250);
        return result_ = Result::Waiting;
    }
    Result Poll(Clock::time_point now) {
        if (!Busy() || now < next_poll_) return result_;
        next_poll_ = now + std::chrono::milliseconds(250);
        Handle world{}; std::string map;
        if (Ready() && native_->CurrentWorld(world, map) && world.id != original_.id) {
            armed_ = false;
            if (map != map_) return result_ = Result::DifferentWorld;
            // Replacing UWorld alone leaves the global GameData class/CDO and its meshes
            // alive. Require their original serials to be gone before reporting success.
            AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
            const auto gone = [&](Handle handle) {
                return native_->objects->snapshot_by_handle(native_->objects->user, handle, &snapshot).code ==
                    ANOMALY_STATUS_V1_NOT_FOUND;
            };
            const auto data = GameData(); uintptr_t cls{}, package{};
            uint64_t class_name{}, package_name{};
            const bool restored = data && native_->Read(data + native_->l.ObjectClass, cls) &&
                native_->Read(cls + native_->l.ObjectOuter, package) &&
                native_->Read(cls + native_->l.ObjectName, class_name) &&
                native_->Read(package + native_->l.ObjectName, package_name) &&
                class_name == saved_.asset && package_name == saved_.package;
            return result_ = replaced_ && restored && gone(old_data_) && gone(old_class_) ? Result::Reloaded : Result::Retained;
        }
        if (now >= deadline_) { armed_ = false; return result_ = Result::Timeout; }
        return result_;
    }
private:
    // UE value-type call contracts, verified against reflected parameter/property sizes.
    // Only top-level class paths are accepted, so no borrowed FString owns heap memory.
    struct String { const wchar_t* data{}; int32_t count{}, capacity{}; };
    struct SoftClass { uint64_t weak{}, package{}, asset{}; String subpath{}; };
    struct Setter { uintptr_t object{}; uint64_t name{}; SoftClass value{}; };
    static_assert(sizeof(String) == 16 && sizeof(SoftClass) == 40 && sizeof(Setter) == 56);
    using Parameter = NativeAccess::Parameter;
    using CleanupFn = void(__fastcall*)(uintptr_t, bool, bool, uintptr_t);

    Handle ObjectHandle(uintptr_t object) const {
        uint32_t index{}; AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (!object || !native_->Read(object + native_->l.ObjectIndex, index) ||
            native_->objects->snapshot_at(native_->objects->user, index, &snapshot).code ||
            native_->Resolve(snapshot.handle) != object) return {};
        return snapshot.handle;
    }
    uintptr_t GameData() const {
        uintptr_t data{};
        return native_->Invoke(native_->Find("/Script/HTGame.Default__HTGameData"),
            "/Script/HTGame.HTGameData:GetGameData", &data, sizeof(data), "ReturnValue", "ObjectProperty", true,
            "/Script/HTGame.HTGameData") && native_->IsA(data, native_->Resolve(native_->Find("/Script/HTGame.HTGameData")))
            ? data : 0;
    }
    bool MakeName(const wchar_t* text, uint64_t& name) const {
        const auto size = static_cast<int32_t>(wcslen(text) + 1);
        struct Params { String text; uint64_t name{}; } params{{text, size, size}};
        const std::array<Parameter, 2> shape{{
            {"InString", "StrProperty", 0, sizeof(String)},
            {"ReturnValue", "NameProperty", offsetof(Params, name), sizeof(name)}
        }};
        if (!native_->InvokeParameters(native_->Find("/Script/Engine.Default__KismetStringLibrary"),
            "/Script/Engine.KismetStringLibrary:Conv_StringToName", &params, sizeof(params), shape, offsetof(Params, name)))
            return false;
        name = params.name; return name != 0;
    }
    bool Prepare() {
        auto& n = *native_;
        Handle pawn{}, controller{}; std::string map;
        if (!n.PlayerWorld(pawn, controller, original_, map)) return false;
        struct Params { uintptr_t actor{}, settings{}; } params{n.Resolve(pawn)};
        const std::array<Parameter, 2> shape{{
            {"HostActor", "ObjectProperty", 0, sizeof(uintptr_t), "/Script/Engine.Actor"},
            {"ReturnValue", "ObjectProperty", offsetof(Params, settings), sizeof(uintptr_t), "/Script/HTGame.HTWorldSettings"}
        }};
        if (!n.InvokeParameters(n.Find("/Script/HTGame.Default__HTUtil"),
            "/Script/HTGame.HTUtil:K2_GetHTWorldSetting", &params, sizeof(params), shape, offsetof(Params, settings))) return false;
        settings_ = ObjectHandle(params.settings);
        const auto type = n.Resolve(n.Find("/Script/HTGame.HTWorldSettings"));
        if (!settings_.id || !n.IsA(params.settings, type)) return false;
        uintptr_t field{}; int32_t type_size{};
        if (!n.Read(type + n.l.StructProperties, field) || !n.Read(type + n.l.StructSize, type_size)) return false;
        for (unsigned i = 0; field && i < 1024; ++i) {
            if (n.Name(field + n.l.PropertyName) == "HTGameDataClass") break;
            if (!n.Read(field + n.l.PropertyNext, field)) return false;
        }
        uintptr_t field_type{}, property_class{}; int32_t size{}, count{}, offset{};
        if (!field || n.Name(field + n.l.PropertyName) != "HTGameDataClass" ||
            !n.Read(field + n.l.PropertyClass, field_type) || n.Name(field_type + n.l.FieldClassName) != "SoftClassProperty" ||
            !n.Read(field + n.l.PropertySize, size) || size != sizeof(SoftClass) ||
            !n.Read(field + n.l.PropertyDim, count) || count != 1 ||
            !n.Read(field + n.l.ObjectPropertyClass, property_class) || property_class != n.Resolve(n.Find("/Script/CoreUObject.Class")) ||
            !n.Read(field + n.l.PropertyOffset, offset) || offset < 0 || type_size < size || offset > type_size - size ||
            !n.Read(field + n.l.PropertyName, property_name_) || !n.Read(params.settings + offset, saved_) ||
            !saved_.package || !saved_.asset || saved_.subpath.data || saved_.subpath.count || saved_.subpath.capacity) return false;
        property_offset_ = static_cast<uint32_t>(offset);
        old_data_ = ObjectHandle(GameData());
        uintptr_t cls{}, package{}; uint64_t class_name{}, package_name{};
        if (!old_data_.id || !n.Read(n.Resolve(old_data_) + n.l.ObjectClass, cls) ||
            !n.Read(cls + n.l.ObjectOuter, package) || !n.Read(cls + n.l.ObjectName, class_name) ||
            !n.Read(package + n.l.ObjectName, package_name) || saved_.asset != class_name || saved_.package != package_name) return false;
        old_class_ = ObjectHandle(cls);
        updater_ = {};
        if (!old_class_.id || !MakeName(L"/Game/Blueprints/GameMode/BP_GameData_Updater", updater_.package) ||
            !MakeName(L"BP_GameData_Updater_C", updater_.asset) || saved_.package == updater_.package) return false;
        setter_receiver_ = n.Find("/Script/Engine.Default__KismetSystemLibrary");
        const std::array<Parameter, 3> setter_shape{{
            {"Object", "ObjectProperty", 0, sizeof(uintptr_t), "/Script/CoreUObject.Object"},
            {"PropertyName", "NameProperty", offsetof(Setter, name), sizeof(uint64_t)},
            {"Value", "SoftClassProperty", offsetof(Setter, value), sizeof(SoftClass), "/Script/CoreUObject.Class"}
        }};
        setter_ = n.ValidateFunction(setter_receiver_, "/Script/Engine.KismetSystemLibrary:SetSoftClassPropertyByName",
            sizeof(Setter), setter_shape);
        initializer_ = n.ValidateFunction(settings_, "/Script/HTGame.HTWorldSettings:InitPrimalGamedata", 0, {});
        return setter_.id && initializer_.id;
    }
    bool SetClass(uintptr_t settings, const SoftClass& value) const {
        Setter params{settings, property_name_, value};
        return !native_->events->invoke(native_->events->user, setter_receiver_, setter_, &params, sizeof(params)).code;
    }
    void ResetGameData() {
        auto& n = *native_;
        const auto settings = n.Resolve(settings_);
        SoftClass current{};
        if (!settings || !n.Read(settings + property_offset_, current) ||
            current.package != saved_.package || current.asset != saved_.asset ||
            GameData() != n.Resolve(old_data_)) return;
        // LoadMap has ended actors and unregistered components, but has not marked the
        // world for GC yet. Reuse the login map's native GameData switch at this boundary.
        // Its lightweight class has no FinalTower -> sequence -> player mesh reference.
        // New-world InitPrimalGamedata restores the normal class after the intervening GC.
        struct Restore {
            WorldReload& owner; uintptr_t settings;
            ~Restore() { owner.SetClass(settings, owner.saved_); }
        } restore{*this, settings};
        if (!SetClass(settings, updater_) ||
            n.events->invoke(n.events->user, settings_, initializer_, nullptr, 0).code) return;
        const auto data = GameData(); uintptr_t cls{}, package{};
        uint64_t class_name{}, package_name{};
        replaced_ = data && data != n.Resolve(old_data_) && n.Read(data + n.l.ObjectClass, cls) &&
            n.Read(cls + n.l.ObjectOuter, package) && n.Read(cls + n.l.ObjectName, class_name) &&
            n.Read(package + n.l.ObjectName, package_name) &&
            class_name == updater_.asset && package_name == updater_.package;
    }
    static void __fastcall CleanupWorld(uintptr_t world, bool session_ended, bool cleanup_resources, uintptr_t new_world) noexcept {
        auto* self = active_.load(std::memory_order_acquire);
        if (!self) return;
        Handle lease{};
        const bool leased = !self->hooks_->begin_callback(self->hooks_->user, self->hook_, &lease).code;
        try {
            reinterpret_cast<CleanupFn>(self->original_cleanup_)(world, session_ended, cleanup_resources, new_world);
            // Only the explicitly requested top-level world on its owning thread.
            // Sublevels, seamless travel and unrelated world teardown pass through.
            if (leased && self->running_.load(std::memory_order_acquire) &&
                GetCurrentThreadId() == self->game_thread_.load(std::memory_order_acquire) && self->armed_ && session_ended && cleanup_resources &&
                !new_world && world == self->native_->Resolve(self->original_)) {
                self->armed_ = false;
                self->ResetGameData();
            }
        } catch (...) { /* Hook ABI boundary; the engine still owns teardown/GC. */ }
        if (leased) self->hooks_->end_callback(self->hooks_->user, lease);
    }
    bool Ready() const {
        AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
        return session_ && session_->struct_size >= sizeof(*session_) && session_->snapshot &&
            !session_->snapshot(session_->user, &snapshot).code && snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY && snapshot.world.id;
    }
    inline static std::atomic<WorldReload*> active_{};
    std::atomic<bool> running_{};
    const AnomalyHookServiceV1* hooks_{};
    uintptr_t original_cleanup_{};
    Handle hook_{};
    NativeAccess* native_{};
    const AnomalyNteSessionServiceV1* session_{};
    Handle original_{}, settings_{}, old_data_{}, old_class_{}, setter_receiver_{}, setter_{}, initializer_{};
    std::atomic<DWORD> game_thread_{};
    uint32_t property_offset_{};
    uint64_t property_name_{};
    SoftClass saved_{}, updater_{};
    bool armed_{}, replaced_{};
    std::string map_;
    Result result_{};
    Clock::time_point next_poll_{}, deadline_{};
};
} // namespace mod_manager
