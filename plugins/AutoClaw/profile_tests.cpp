#include "profile_test_setup.hpp"
#include "plugin.cpp"
#include <fstream>
#include <iostream>

namespace {
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Package {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("autoclaw-profile-fixture-"+std::to_string(ue::Now()));
    Package(){std::filesystem::create_directories(path/"profile");}
    ~Package(){std::error_code error;std::filesystem::remove(path/"profile/compatibility.profile.json",error);
        std::filesystem::remove(path/"profile",error);std::filesystem::remove(path,error);}
    void Write(const std::string& text){std::ofstream(path/"profile/compatibility.profile.json")<<text;}
};
}
int main() {
    try {
        anomaly::testing::ProfileHost host("AutoClaw");PrepareClawProfile(host);
        const auto package=host.package;
        auto profile=claw::compat::Load(&host.api);Check(static_cast<bool>(profile),"production Profile did not load");
        std::uintptr_t registry{},world{};
        Check(claw::compat::Resolve(&host.api,&host.core,*profile,registry,world)&&registry==0x10000&&world==0x10040,
            "recorded instructions did not resolve independent native targets");
        host.signatures.resolve=[](void*,AnomalyStringViewV1,AnomalyStringViewV1,AnomalyStringViewV1,std::uintptr_t*) {
            Check(false,"default-off lifecycle scanned game signatures");return Ok();
        };
        void* context{};Check(Load(&host.api,&context).code==0&&context,"valid Profile failed lifecycle load");
        Check(Start(context).code==0&&!static_cast<Context*>(context)->detection.load(),"startup enabled detection");
        Update(context,0.5);Draw(context,nullptr);Stop(context,0);Unload(context);
        anomaly::testing::ProfileHost clean("AutoClaw");PrepareClawProfile(clean);host.signatures.resolve=clean.signatures.resolve;
        std::copy_n(host.image.begin()+128,48,host.image.begin()+256);registry=world=123;
        Check(!claw::compat::Resolve(&host.api,&host.core,*profile,registry,world)&&!registry&&!world,
            "ambiguous signatures reused previous addresses");
        PrepareClawProfile(host);
        nlohmann::json original;std::ifstream(package/"profile/compatibility.profile.json")>>original;
        Package temporary;host.package=temporary.path;
        const auto reject=[&](const nlohmann::json& changed){temporary.Write(changed.dump());
            Check(!claw::compat::Load(&host.api),"invalid compatibility data activated fallback layout");
            Check(host.OutstandingHandles()==0,"Profile rejection leaked JSON handles");};
        auto changed=original;changed["layout"].erase("ObjectClass");reject(changed);
        changed=original;changed["layout"]["MapValue"]=1;reject(changed);
        changed=original;changed["layout"]["MapBitsData"]=16;reject(changed);
        changed=original;changed["layout"]["LevelActors"]=1;reject(changed);
        changed=original;changed["layout"]["MapEntryStride"]=0;reject(changed);
        changed=original;changed["contract"]="unknown";reject(changed);
        changed=original;changed["contract"]=1;reject(changed);
        changed=original;changed["symbols"][0]["candidates"][0]["entry"]="CC";temporary.Write(changed.dump());
        auto bad_entry=claw::compat::Load(&host.api);Check(static_cast<bool>(bad_entry),"valid schema was rejected before symbol validation");
        registry=world=123;Check(!claw::compat::Resolve(&host.api,&host.core,*bad_entry,registry,world)&&!registry&&!world,
            "wrong native entry bytes were accepted");
        temporary.Write("{broken");Check(!claw::compat::Load(&host.api),"malformed JSON activated detection");
        host.package=temporary.path/"absent";context=reinterpret_cast<void*>(1);
        Check(Load(&host.api,&context).code==ANOMALY_STATUS_V1_UNAVAILABLE&&!context,"missing Profile created an active plugin");
        Check(host.OutstandingHandles()==0,"Profile lifecycle leaked JSON handles");
        std::cout<<"autoclaw: production Profile, default-off lifecycle, worker resolution and failure gates passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
