#pragma once
#include "engine.hpp"
#include <optional>

namespace creative_mode {
inline constexpr std::uint32_t PreviewSize = 256;
struct PreviewRequest {
    std::string key,asset;
    std::uint64_t ticket{};
    double yaw{35},elevation{20},zoom{1};
};
struct BrowserRequests {
    bool active{};
    std::uint64_t frame{},milliseconds{};
    std::vector<PreviewRequest> images;
    bool interacting{};
};
struct GpuCapture {
    PreviewRequest request;
    Handle world{},target{};
    std::uint64_t ready_after_frame{},deadline{};
    std::string error;
};
// Game owns only UE objects and capture submission. Each tick performs one
// preparation stage. Pixel readback and waits for Render/GPU are absent.
class ModelPreview {
public:
    std::shared_ptr<const GpuCapture> Tick(Engine& engine,const BrowserRequests& wanted,
                                         Handle world,std::uint64_t acknowledged,const anomaly::sdk::Localizer& l10n) {
        if (!wanted.active) { Clear(engine); return {}; }
        for (auto it = completed_.begin(); it != completed_.end();) {
            if (std::none_of(wanted.images.begin(),wanted.images.end(),[&](const auto& r) { return r.ticket == *it; })) it = completed_.erase(it);
            else ++it;
        }
        if (pending_ && (acknowledged == pending_->ticket ||
            std::none_of(wanted.images.begin(),wanted.images.end(),[&](const auto& r) { return r.ticket == pending_->ticket; }))) {
            completed_.insert(pending_->ticket); pending_.reset(); output_.reset(); phase_ = 0; loading_asset_.clear();
        }
        if (output_) return output_;
        if (!pending_) {
            const auto next = std::find_if(wanted.images.begin(),wanted.images.end(),[&](const auto& r) { return !completed_.contains(r.ticket); });
            if (next == wanted.images.end()) return {};
            pending_ = *next; phase_ = last_key_ == pending_->key && target_.id ? 3 : 0;
            if (phase_ == 0) last_key_.clear();
            deadline_ = wanted.milliseconds+30000;
        }
        try {
            if (wanted.milliseconds > deadline_)
                throw std::runtime_error(l10n.Text("error.asset_timeout","The asset could not be loaded in time; select it again to retry"));
            Pose stage; stage.position = {0,0,800000};
            switch (phase_++) {
            case 0:
                if (!bindings_) { engine.PreviewPreflight(); bindings_ = true; }
                if (!capture_.actor.id || !engine.Alive(capture_)) {
                    capture_ = engine.SpawnAuxiliary("/Script/Engine.SceneCapture2D",stage);
                    const auto component = engine.ObjectField(engine.Resolve(capture_.actor),"CaptureComponent2D");
                    if (!engine.IsA(component,"SceneCaptureComponent2D"))
                        throw std::runtime_error(l10n.Text("error.preview_api","The native model preview interfaces are unavailable"));
                    component_ = engine.Pin(component);
                }
                engine.ConfigureCapture(engine.Resolve(component_));
                break;
            case 1:
                if (!EnsureMesh(engine,pending_->asset)) { --phase_; break; }
                if (asset_ != pending_->asset || !model_.actor.id || !engine.Alive(model_)) {
                    const auto component = engine.Resolve(component_);
                    engine.Call("/Script/Engine.SceneCaptureComponent:ClearShowOnlyComponents",component);
                    if (model_.actor.id) engine.Destroy(model_);
                    model_ = engine.Spawn({pending_->asset,stage,false},0,true); asset_ = pending_->asset;
                    engine.PreviewOnly(model_);
                    engine.Call("/Script/Engine.SceneCaptureComponent:ShowOnlyActorComponents",component,[&](Parameters& p) {
                        p.Put("InActor",engine.Resolve(model_.actor)); p.Put("bIncludeFromChildActors",false);
                    });
                }
                break;
            case 2:
                target_ = engine.CreateSharedTarget(engine.Resolve(component_),PreviewSize);
                break;
            case 3:
                Position(engine,*pending_);
                // Add one weak component reference to UE's deferred capture
                // queue. Normal viewport rendering consumes it, including when
                // gameplay is paused. Continuous capture stays disabled.
                engine.CaptureDeferred(component_);
                output_ = std::make_shared<const GpuCapture>(GpuCapture{*pending_,world,target_,wanted.frame+2,deadline_});
                last_key_ = pending_->key;
                break;
            }
        } catch (const std::exception& error) {
            output_ = std::make_shared<const GpuCapture>(GpuCapture{*pending_,world,{},0,deadline_,error.what()});
        }
        return output_ ? output_ : std::make_shared<const GpuCapture>(GpuCapture{*pending_,world,{},0,deadline_});
    }
    void Renew(Engine& engine) {
        for (const auto* item : {&capture_,&model_}) if (item->actor.id && engine.Alive(*item)) engine.Renew(*item);
    }
    void ResetWorld() {
        capture_ = {}; model_ = {}; component_ = {}; target_ = {};
        pending_.reset(); output_.reset(); completed_.clear(); asset_.clear(); last_key_.clear(); loading_asset_.clear(); bindings_ = false; phase_ = 0;
    }
    void Clear(Engine& engine) {
        for (const auto* item : {&capture_,&model_}) if (item->actor.id) engine.Destroy(*item);
        ResetWorld();
    }
private:
    bool EnsureMesh(Engine& engine,std::string_view asset) {
        if (engine.MeshLoaded(asset)) { loading_asset_.clear(); return true; }
        if (loading_asset_ != asset) { engine.RequestMesh(asset,capture_.actor,++load_uuid_); loading_asset_ = asset; }
        return false;
    }
    void Position(Engine& engine,const PreviewRequest& request) {
        const auto centre = model_.center;
        const auto radius = std::max(10.0,editor::geometry::Length(model_.extent));
        const auto distance = radius/std::sin(std::numbers::pi/8)*1.12*request.zoom;
        const auto yaw = request.yaw*std::numbers::pi/180, pitch = request.elevation*std::numbers::pi/180;
        const Vector offset{distance*std::cos(pitch)*std::cos(yaw),distance*std::cos(pitch)*std::sin(yaw),distance*std::sin(pitch)};
        Pose camera; camera.position = editor::geometry::Add(centre,offset); camera.rotation = {-request.elevation,request.yaw+180,0};
        engine.Move(capture_,camera,false);
    }
    LiveItem capture_,model_;
    Handle component_{},target_{};
    std::string asset_,loading_asset_,last_key_;
    std::int32_t load_uuid_{};
    unsigned phase_{};
    bool bindings_{};
    std::uint64_t deadline_{};
    std::optional<PreviewRequest> pending_;
    std::shared_ptr<const GpuCapture> output_;
    std::set<std::uint64_t> completed_;
};

// Render owns texture leases and shared-handle import. The imported NT handle
// pins its GPU allocation even after the transient UE render target is replaced.
class PreviewTextures {
public:
    struct Entry {
        Handle texture{},pending{},source_target{};
        PreviewRequest request;
        std::string error;
        std::uint64_t used{},started{};
        bool requested{},active{};
        std::uint32_t waiting_stage{};
    };
    void SetService(const AnomalyTextureServiceV1* texture,const AnomalyGpuTextureServiceV1* gpu,const AnomalyCoreServiceV1* core = nullptr) { texture_ = texture; gpu_ = gpu; core_ = core; }
    void BeginFrame(std::uint64_t milliseconds) { ++frame_; now_ = milliseconds; requests_ = {false,frame_,now_}; }
    void Active() { requests_.active = true; }
    void Interacting() { requests_.interacting = true; }
    Entry& Need(std::string_view asset) { return EntryFor(std::string(asset)+"|card",asset,35,20,1,false); }
    Entry& Detail(std::string_view asset,double yaw,double elevation,double zoom,bool update) {
        return EntryFor(std::string(asset)+"|detail",asset,yaw,elevation,zoom,update);
    }
    bool Available() const { return texture_ && gpu_ && gpu_->request_shared_d3d12; }
    // Returns true only when Game can replace the target (imported, failed, or
    // obsolete). Pending native resources are polled once per Render frame.
    bool Accept(const GpuCapture& capture,const Engine& engine,const anomaly::sdk::Localizer& l10n) {
        const auto it = entries_.find(capture.request.key);
        if (it == entries_.end() || it->second.request.ticket != capture.request.ticket) return true;
        auto& entry = it->second;
        entry.active = true;
        if (entry.requested || !entry.error.empty()) return true;
        if (!capture.error.empty()) { entry.error = capture.error; return true; }
        // The same selected model renders into its existing target on the host
        // graphics queue. Its imported SRV stays valid while the image changes.
        if (capture.target.id && entry.texture.id && Same(entry.source_target,capture.target)) {
            entry.requested = true; return true;
        }
        if (now_ > capture.deadline) {
            entry.error = l10n.Text("error.preview_timeout","Model preview timed out; refresh images to try again");
            Log(entry,l10n,"diagnostic.preview_export","Preview export timed out: {0}, native resource stage {1}",entry.waiting_stage); return true;
        }
        if (!capture.target.id) return false;
        if (frame_ < capture.ready_after_frame) return false;
        Address shared{};
        try { shared = engine.ShareTarget(capture.target,entry.waiting_stage); }
        catch (const std::exception& error) { entry.error = error.what(); return true; }
        if (!shared) return false;
        if (entry.pending.id && texture_->release) texture_->release(texture_->user,entry.pending);
        entry.pending = {};
        const auto result = gpu_->request_shared_d3d12(gpu_->user,shared,PreviewSize,PreviewSize,ANOMALY_GPU_TEXTURE_V1_FORCE_OPAQUE,&entry.pending);
        CloseSharedNative(shared);
        entry.requested = true; entry.started = now_;
        entry.source_target = capture.target;
        if (result.code != 0) entry.error = l10n.Text("error.preview_gpu","The model preview GPU resource is unavailable");
        return true;
    }
    bool Draw(Entry& entry,float size,const anomaly::sdk::Localizer& l10n) {
        if (!Available() || !texture_->state || !texture_->draw) return false;
        AnomalyTextureStateV1 state{sizeof(state)};
        if (entry.pending.id && texture_->state(texture_->user,entry.pending,&state).code == 0) {
            if (state.flags&ANOMALY_TEXTURE_STATE_V1_FAILED) {
                entry.error = l10n.Text("error.preview_gpu","The model preview GPU resource is unavailable");
                texture_->release(texture_->user,entry.pending); entry.pending = {};
            } else if (state.flags&ANOMALY_TEXTURE_STATE_V1_READY) {
                if (entry.texture.id) texture_->release(texture_->user,entry.texture);
                entry.texture = entry.pending; entry.pending = {};
            }
        }
        if (entry.pending.id && now_ > entry.started+6000) {
            entry.error = l10n.Text("error.preview_timeout","Model preview timed out; refresh images to try again");
            Log(entry,l10n,"diagnostic.preview_import","Preview texture import timed out: {0}, resource flags {1}",state.flags);
            texture_->release(texture_->user,entry.pending); entry.pending = {};
        }
        if (!entry.texture.id) return false;
        if (texture_->state(texture_->user,entry.texture,&state).code != 0 || (state.flags&ANOMALY_TEXTURE_STATE_V1_FAILED)) {
            entry.error = l10n.Text("error.preview_gpu","The model preview GPU resource is unavailable");
            texture_->release(texture_->user,entry.texture); entry.texture = {}; return false;
        }
        return (state.flags&ANOMALY_TEXTURE_STATE_V1_READY) &&
            texture_->draw(texture_->user,entry.texture,size,size,ANOMALY_RGBA_V1(255,255,255,255)).code == 0;
    }
    BrowserRequests FinishFrame() {
        if (requests_.interacting) std::erase_if(requests_.images,[](const auto& request) { return !request.key.ends_with("|detail"); });
        while (entries_.size() > 64) {
            const auto it = std::min_element(entries_.begin(),entries_.end(),[](const auto& a,const auto& b) { return a.second.used < b.second.used; });
            Release(it->second); entries_.erase(it);
        }
        return std::move(requests_);
    }
    void Refresh() { for (auto& [key,entry] : entries_) { static_cast<void>(key); Release(entry); } entries_.clear(); }
    std::uint64_t Frame() const { return frame_; }
private:
    void Log(const Entry& entry,const anomaly::sdk::Localizer& l10n,const char* key,const char* fallback,std::uint32_t value) {
        if (!core_ || !core_->log) return;
        const auto number = std::to_string(value); const std::array args{std::string_view(entry.request.asset),std::string_view(number)};
        const auto message = l10n.Format(key,fallback,args); core_->log(core_->user,ANOMALY_CORE_LOG_LEVEL_V1_WARNING,anomaly::sdk::StringView(message));
    }
    Entry& EntryFor(std::string key,std::string_view asset,double yaw,double elevation,double zoom,bool update) {
        auto [it,inserted] = entries_.try_emplace(key); auto& entry = it->second;
        if (inserted || (update && entry.requested && !entry.pending.id && (entry.request.yaw != yaw || entry.request.elevation != elevation || entry.request.zoom != zoom))) {
            if (entry.pending.id && texture_ && texture_->release) texture_->release(texture_->user,entry.pending);
            entry.pending = {}; entry.request = {std::move(key),std::string(asset),++ticket_,yaw,elevation,zoom};
            entry.requested = false; entry.active = false; entry.error.clear(); entry.started = now_;
        }
        entry.used = frame_;
        if (Available() && !entry.requested && entry.error.empty()) {
            if (entry.request.key.ends_with("|detail")) requests_.images.insert(requests_.images.begin(),entry.request);
            else requests_.images.push_back(entry.request);
        }
        return entry;
    }
    void Release(Entry& entry) {
        if (!texture_ || !texture_->release) return;
        if (entry.texture.id) texture_->release(texture_->user,entry.texture);
        if (entry.pending.id) texture_->release(texture_->user,entry.pending);
    }
    const AnomalyTextureServiceV1* texture_{};
    const AnomalyGpuTextureServiceV1* gpu_{};
    const AnomalyCoreServiceV1* core_{};
    std::map<std::string,Entry,std::less<>> entries_;
    BrowserRequests requests_;
    std::uint64_t frame_{},ticket_{},now_{};
};
}
