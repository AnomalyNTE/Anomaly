#include "profile_test_setup.hpp"
#include "plugin.cpp"
#include <fstream>
#include <iostream>

namespace {
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Package {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("taxi-profile-fixture-"+std::to_string(Clock::now().time_since_epoch().count()));
    Package() { std::filesystem::create_directories(path/"profile"); }
    ~Package() {
        std::error_code error;
        std::filesystem::remove(path/"profile/compatibility.profile.json",error);
        std::filesystem::remove(path/"profile",error);
        std::filesystem::remove(path,error);
    }
    void Write(const std::string& value) { std::ofstream(path/"profile/compatibility.profile.json")<<value; }
};
}
int main() {
    try {
        anomaly::testing::ProfileHost host("Autotaxi");PrepareProfileHost(host);
        const auto package=host.package;
        const auto load=[&] {
            const auto ok=taxi::profile::Load(&host.api);
            Check(host.OutstandingHandles()==0,"Profile leaked JSON handles");
            return ok;
        };
        if(!load()) throw std::runtime_error("recorded taxi instructions did not match the production Profile: "+host.error);
        Check(taxi::profile::snapshot.registry==0x11000 && taxi::profile::snapshot.world==0x11800,
            "Profile did not resolve the independent fixture targets");
        void* context=reinterpret_cast<void*>(1);
        Check(Load(&host.api,&context).code==0 && !context,"valid Profile failed lifecycle load");
        host.signatures.resolve=[](void*,AnomalyStringViewV1,AnomalyStringViewV1,AnomalyStringViewV1,uintptr_t*) {
            Check(false,"Start/Game/Render rescanned a Profile");return Ok();
        };
        Check(Start(context).code==0 && !enabled && registry==0x11000 && world_slot==0x11800,
            "startup did not use its cached Profile or defaulted automation on");
        Update(context,.5);Draw(context,nullptr);Stop(context,0);Unload(context);
        Check(!registry && !world_slot && !taxi::profile::snapshot.registry,"unload retained native addresses");
        PrepareProfileHost(host);
        // Restore the real signature transport after the no-rescan lifecycle check.
        anomaly::testing::ProfileHost clean("Autotaxi");PrepareProfileHost(clean);
        host.signatures.resolve=clean.signatures.resolve;
        Check(load(),"valid Profile did not reload");
        std::copy_n(host.image.begin()+32,48,host.image.begin()+256);
        Check(!load() && !taxi::profile::snapshot.registry,"ambiguous signature reused old addresses");
        PrepareProfileHost(host);
        nlohmann::json original;std::ifstream(package/"profile/compatibility.profile.json")>>original;
        Package temporary;host.package=temporary.path;
        auto changed=original;changed["layout"].erase("ObjectClass");temporary.Write(changed.dump());
        Check(!load(),"missing field fell back to a code offset");
        changed=original;changed["layout"]["ProcessEventSlot"]=1;temporary.Write(changed.dump());
        Check(!load(),"misaligned virtual slot was accepted");
        changed=original;changed["contract"]="unknown";temporary.Write(changed.dump());
        Check(!load(),"unsupported semantic contract was accepted");
        changed=original;changed["contract"]=1;temporary.Write(changed.dump());
        Check(!load(),"invalid schema bypassed the production validator");
        changed=original;changed["symbols"][0]["candidates"][0]["entry"]="CC";temporary.Write(changed.dump());
        Check(!load(),"wrong native entry bytes were accepted");
        temporary.Write("{broken");Check(!load(),"malformed JSON activated a fallback");
        host.package=temporary.path/"absent";
        context=reinterpret_cast<void*>(1);
        Check(Load(&host.api,&context).code==ANOMALY_STATUS_V1_UNAVAILABLE && !context && !registry && !world_slot,
            "missing Profile started native work");
        host.package=package;Check(load(),"valid Profile failed after rejection");
        host.image.resize(34);anomaly::testing::RecordedProcessEventEntry({host.image.data(),host.image.size()});
        Check(taxi::profile::ProcessEventEntry(&host.core,0x10000),"owned ProcessEvent entry was rejected");
        host.image[0]=0xE9;Check(taxi::profile::ProcessEventEntry(&host.core,0x10000),"host observer entry was rejected");
        host.image[7]^=1;Check(!taxi::profile::ProcessEventEntry(&host.core,0x10000),"wrong entry tail was accepted");
        Check(host.OutstandingHandles()==0,"failed lifecycle load leaked handles");
        std::cout<<"Taxi production Profile, native-entry gating, cached lifecycle and rejection checks passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
