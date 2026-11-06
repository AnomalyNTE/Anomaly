#pragma once
#include "native_access.hpp"
#include "vehicle_templates.hpp"
#include <chrono>
#include <vector>

namespace mod_manager {
class AssetRefresh {
public:
    using Clock = std::chrono::steady_clock;
    enum class Result { Idle, WaitingActors, Waiting, VehiclesCleared, Released, Retained, NotResident, ActorPresent,
        Occupied, DestroyRejected, Unavailable, ChangedWorld, InvalidTag, TemplateInUse };
    struct Services {
        const AnomalyUe5ObjectsServiceV1* objects{};
        const AnomalyNtePlayerServiceV1* player{};
        const AnomalyNteActorsServiceV1* actors{};
        const AnomalyNteSessionServiceV1* session{};
        NativeAccess* native{};
    };
    void Bind(Services services) { services_ = services; Cancel(); }
    void Cancel() { result_ = Result::Idle; watched_.clear(); retained_.clear(); }
    Result State() const { return result_; }
    bool Busy() const { return result_ == Result::Waiting || result_ == Result::WaitingActors; }
    const std::string& Retained() const { return retained_; }
    const std::vector<std::string>& Vehicles() const { return tags_; }
    bool DestroyOnly() const { return destroy_only_; }
    static bool ValidTag(std::string_view tag) {
        if (!tag.starts_with("Vehicle_") || tag.size() < 9 || tag.size() > 14) return false;
        return std::all_of(tag.begin() + 8, tag.end(), [](char c) { return c >= '0' && c <= '9'; });
    }
    Result Begin(const std::vector<std::string>& vehicles, const std::vector<std::string>& packages,
                 bool destroy_only, Clock::time_point now) {
        if (Busy()) return result_;
        Cancel(); tags_ = vehicles; destroy_only_ = destroy_only;
        if (tags_.empty() || !std::all_of(tags_.begin(), tags_.end(), ValidTag)) return result_ = Result::InvalidTag;
        if (!Ready() || (!destroy_only_ && packages.empty())) return result_ = Result::Unavailable;
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        if (services_.session->snapshot(services_.session->user, &session).code || !session.world.id)
            return result_ = Result::Unavailable;
        world_ = session.world;
        generation_ = services_.objects->generation(services_.objects->user);
        if (!generation_) return result_ = Result::Unavailable;
        if (!destroy_only_) for (const auto& path : packages) {
            // These are package paths from container indices, not guessed object names.
            if (!path.starts_with("/game/") || path.find("..") != std::string::npos) return result_ = Result::Unavailable;
            Handle handle{};
            const auto status = services_.objects->find_exact(services_.objects->user, anomaly::sdk::StringView(path), &handle);
            if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) continue;
            if (status.code || !handle.id) return result_ = Result::Unavailable;
            watched_.push_back({handle, path});
        }
        std::vector<std::pair<Handle, std::string>> actors;
        if (!Actors(actors)) return result_ = Result::Unavailable;
        if (!actors.empty() && !destroy_only_) return result_ = Result::ActorPresent;
        if (!actors.empty()) {
            Handle pawn{}, vehicle{};
            if (!services_.native->PlayerVehicle(pawn, vehicle))
                return result_ = Result::Unavailable;
            for (const auto& [handle, name] : actors) {
                if (handle.id == pawn.id || handle.id == vehicle.id) return result_ = Result::Occupied;
                if (!services_.native->Actor(handle, name)) return result_ = Result::Unavailable;
            }
            for (const auto& [handle, name] : actors) {
                if (!services_.native->Destroy(handle)) return result_ = Result::DestroyRejected;
            }
            // SafeDestroyVehicle schedules the game's Mass entity removal or Actor lifespan.
            // Confirm that route has finished; resource release is a separate request.
            deadline_ = now + std::chrono::seconds(3);
            return result_ = Result::WaitingActors;
        }
        if (destroy_only_) return result_ = Result::VehiclesCleared;
        return Clear(now);
    }
    Result Poll(Clock::time_point now) {
        if (!Busy()) return result_;
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        if (services_.session->snapshot(services_.session->user, &session).code) return result_ = Result::Unavailable;
        if (session.world.id != world_.id || session.world.generation != world_.generation ||
            services_.objects->generation(services_.objects->user) != generation_) return result_ = Result::ChangedWorld;
        if (now < deadline_) return result_;
        std::vector<std::pair<Handle, std::string>> actors;
        if (!Actors(actors)) return result_ = Result::Unavailable;
        if (result_ == Result::WaitingActors) {
            if (!actors.empty()) return result_ = Result::DestroyRejected;
            return result_ = Result::VehiclesCleared;
        }
        if (!actors.empty()) return result_ = Result::ActorPresent;
        retained_.clear();
        for (const auto& entry : watched_) {
            AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
            const auto status = services_.objects->snapshot_by_handle(services_.objects->user, entry.handle, &snapshot);
            // Only the original package can be retained. A new package at the same path
            // may already have loaded from the replacement container during the GC wait.
            if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) continue;
            if (status.code) return result_ = Result::Unavailable;
            if (retained_.empty()) retained_ = entry.path.substr(entry.path.find_last_of('/') + 1);
        }
        return result_ = retained_.empty() ? Result::Released : Result::Retained;
    }
private:
    bool Ready() const {
        return services_.native && services_.objects && services_.objects->struct_size >= sizeof(*services_.objects) &&
            services_.objects->snapshot_at && services_.objects->snapshot_by_handle && services_.objects->generation && services_.objects->find_exact &&
            services_.session && services_.session->struct_size >= sizeof(*services_.session) && services_.session->snapshot &&
            services_.actors && services_.actors->struct_size >= offsetof(AnomalyNteActorsServiceV1, bool_property) + sizeof(services_.actors->bool_property) && services_.actors->frame &&
            services_.actors->snapshot_at && services_.actors->class_name_utf8 && services_.actors->bool_property && services_.player &&
            services_.player->struct_size >= offsetof(AnomalyNtePlayerServiceV1, get_object_handles) + sizeof(services_.player->get_object_handles) &&
            services_.player->get_object_handles;
    }
    bool Actors(std::vector<std::pair<Handle, std::string>>& out) const {
        AnomalyNteEntityFrameV1 frame{sizeof(frame)};
        if (services_.actors->frame(services_.actors->user, &frame).code || !(frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) ||
            (frame.flags & (ANOMALY_NTE_SNAPSHOT_V1_PARTIAL | ANOMALY_NTE_SNAPSHOT_V1_STALE))) return false;
        for (uint32_t i = 0; i < frame.entity_count; ++i) {
            AnomalyNteEntitySnapshotV1 actor{sizeof(actor)};
            if (services_.actors->snapshot_at(services_.actors->user, frame.generation, i, &actor).code) return false;
            std::array<char, 512> text{}; size_t size = text.size();
            if (services_.actors->class_name_utf8(services_.actors->user, actor.class_id, text.data(), &size).code ||
                !size || size > text.size() || text[size - 1] != 0) return false;
            const std::string name(text.data()); bool matches{};
            for (const auto& tag : tags_) {
                matches |= NativeAccess::MatchesVehicle(name, tag);
            }
            if (!matches) continue;
            if (!actor.entity_id || actor.entity_id > UINT32_MAX) return false;
            // Actor-cache handles are a different domain. Obtain a fresh serial-bearing UObject handle.
            AnomalyUe5ObjectSnapshotV1 object{sizeof(object)};
            const auto status = services_.objects->snapshot_at(services_.objects->user, static_cast<uint32_t>(actor.entity_id - 1), &object);
            if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) continue;
            if (status.code || !object.handle.id) return false;
            AnomalyNteEntityBoolPropertyV1 pooled{sizeof(pooled)};
            if (services_.actors->bool_property(services_.actors->user, actor.handle,
                    anomaly::sdk::StringView("bInMassPool"), &pooled).code ||
                !(pooled.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) ||
                (pooled.flags & (ANOMALY_NTE_SNAPSHOT_V1_PARTIAL | ANOMALY_NTE_SNAPSHOT_V1_STALE))) return false;
            // SafeDestroyVehicle can return the Actor to the Mass pool without destroying its UObject.
            // It is no longer an active vehicle; package residency is checked separately after GC.
            if (pooled.value) continue;
            out.emplace_back(object.handle, name);
        }
        return true;
    }
    Result Clear(Clock::time_point now) {
        if (!services_.native->ReleasePools(tags_)) return result_ = Result::Unavailable;
        if (watched_.empty()) return result_ = Result::NotResident;
        const auto templates = VehicleTemplates(*services_.native).Clear(tags_);
        if (templates == VehicleTemplates::Result::InUse) return result_ = Result::TemplateInUse;
        if (templates == VehicleTemplates::Result::Unavailable) return result_ = Result::Unavailable;
        if (!services_.native->Clear()) return result_ = Result::Unavailable;
        deadline_ = now + std::chrono::seconds(3);
        return result_ = Result::Waiting;
    }
    struct Watched { Handle handle; std::string path; };
    Services services_;
    Result result_{Result::Idle};
    Handle world_{};
    uint64_t generation_{};
    bool destroy_only_{};
    Clock::time_point deadline_{};
    std::vector<Watched> watched_;
    std::vector<std::string> tags_;
    std::string retained_;
};
} // namespace mod_manager
