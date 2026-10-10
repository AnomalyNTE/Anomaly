#include "persistence.hpp"
#include <iostream>
#include <stdexcept>
struct Scheduled {AnomalyTaskCallbackV1 callback{};void* user{};unsigned calls{},cancels{};};
void Check(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
int main() {
    try {
        const auto directory=std::filesystem::current_path()/".build/windows-vs2022/autofish-persistence-fixture";
        std::filesystem::create_directories(directory);
        const auto path=directory/"settings.bin";
        std::filesystem::remove(path);
        Scheduled pending;
        AnomalySchedulerServiceV1 scheduler{sizeof(scheduler),1};scheduler.user=&pending;
        scheduler.schedule=[](void* user,uint32_t,AnomalyTaskCallbackV1 callback,void* context,AnomalyGenerationHandleV1* handle) {
            auto& task=*static_cast<Scheduled*>(user);task.callback=callback;task.user=context;++task.calls;*handle={1,1};return anomaly::sdk::Ok();
        };
        scheduler.cancel=[](void* user,AnomalyGenerationHandleV1) {++static_cast<Scheduled*>(user)->cancels;return anomaly::sdk::Ok();};
        fishing::Persistence files;files.Start(&scheduler);
        uint32_t value=5;files.Store(path,&value,sizeof(value));value=9;files.Store(path,&value,sizeof(value));value=99;
        Check(!std::filesystem::exists(path),"publishing settings must not write from the calling thread");
        Check(pending.calls==1,"rapid edits should coalesce into one scheduled write");
        pending.callback(pending.user,{1,1});
        uint32_t saved{};{std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(&saved),sizeof(saved));}
        Check(saved==9,"worker must write the latest owned snapshot, not borrowed caller memory");
        value=12;files.Store(path,&value,sizeof(value));files.BeginStop();value=14;files.Store(path,&value,sizeof(value));files.FinishStop();
        pending.callback(pending.user,{1,1});
        {std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(&saved),sizeof(saved));}
        Check(saved==14 && pending.cancels==1,"a late task must not overwrite the final lifecycle snapshot");
        std::filesystem::remove(path);
        std::cout<<"Deferred persistence and stop ordering passed.\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
