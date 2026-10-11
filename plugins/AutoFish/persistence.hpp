#pragma once
#include <anomaly/sdk/cpp.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
namespace fishing {
// Game/Render callers publish owned byte snapshots; only scheduler tasks or the
// lifecycle stop write files. No UE object or borrowed parameter enters a task.
class Persistence {
    const AnomalySchedulerServiceV1* scheduler_{};
    std::mutex snapshot_mutex_,io_mutex_;
    std::map<std::filesystem::path,std::vector<uint8_t>> files_;
    AnomalyGenerationHandleV1 task_{};
    std::atomic_bool scheduled_{},stopping_{};
    std::atomic_uint64_t revision_{};
    uint64_t Write(bool final) {
        std::lock_guard io(io_mutex_);
        if(stopping_.load() && !final) return revision_.load();
        std::map<std::filesystem::path,std::vector<uint8_t>> snapshot;
        uint64_t version{};
        {std::lock_guard lock(snapshot_mutex_);snapshot=files_;version=revision_.load();}
        for(const auto& [path,bytes]:snapshot) {
            std::ofstream out(path,std::ios::binary|std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        }
        return version;
    }
    void Schedule() {
        if(stopping_.load() || !scheduler_ || !scheduler_->schedule || scheduled_.exchange(true)) return;
        AnomalyGenerationHandleV1 task{};
        const auto result=scheduler_->schedule(scheduler_->user,100,Run,this,&task);
        if(result.code!=0) scheduled_=false;
        else {std::lock_guard lock(snapshot_mutex_);task_=task;}
    }
    static void ANOMALY_CALL Run(void* user,AnomalyGenerationHandleV1) noexcept {
        auto& self=*static_cast<Persistence*>(user);
        try {
            const auto version=self.Write(false);self.scheduled_=false;
            if(!self.stopping_.load() && self.revision_.load()!=version) self.Schedule();
        } catch(...) {self.scheduled_=false;}
    }
public:
    void Start(const AnomalySchedulerServiceV1* scheduler) {
        scheduler_=scheduler;stopping_=false;scheduled_=false;revision_=0;
        std::lock_guard lock(snapshot_mutex_);files_.clear();task_={};
    }
    bool Store(const std::filesystem::path& path,const void* bytes,size_t size) {
        if(path.empty()) return false;
        const auto begin=static_cast<const uint8_t*>(bytes);
        {std::lock_guard lock(snapshot_mutex_);files_[path]={begin,begin+size};++revision_;}
        Schedule();return true;
    }
    void BeginStop() {
        stopping_=true;AnomalyGenerationHandleV1 task{};
        {std::lock_guard lock(snapshot_mutex_);task=task_;}
        if(task.id && scheduler_ && scheduler_->cancel) scheduler_->cancel(scheduler_->user,task);
    }
    void FinishStop() {Write(true);}
};
}
