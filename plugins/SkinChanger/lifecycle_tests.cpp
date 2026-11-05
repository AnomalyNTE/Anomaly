#include "profile_test_setup.hpp"
#include "plugin.cpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace probe {
bool awakening_visible{},vehicle_visible{};
unsigned loads{},queries{},begins{},ends{},releases{};
int open{1},expanded{1},pick{},fail_begin{};
std::string title;
void Check(bool value,const char* message) {if(!value){std::cerr<<message<<'\n';std::exit(1);}}
std::string Text(AnomalyStringViewV1 value){return {value.data,value.size};}
}
AnomalyStatusV1 CosmeticsLoad(const AnomalyHostApiV1*,void** context) noexcept {*context=nullptr;return Ok();}
AnomalyStatusV1 CosmeticsStart(void*) noexcept {return Ok();}
AnomalyStatusV1 CosmeticsStop(void*,uint32_t) noexcept {return Ok();}
void CosmeticsUnload(void*) noexcept {}
void CosmeticsUpdate(void*,double) noexcept {}
void CosmeticsDraw(void*,const AnomalyUiServiceV1*,int) noexcept {}
namespace lite_awakening {
void Load(const AnomalyHostApiV1*) noexcept {++probe::loads;}
void Start() noexcept {}
AnomalyStatusV1 Stop() noexcept {return Ok();}
void Update() noexcept {}
void SetVisible(bool value) noexcept {probe::awakening_visible=value;}
void Draw(const AnomalyUiServiceV1*) noexcept {}
}
namespace lite_plates {
void Load(const AnomalyHostApiV1*) noexcept {++probe::loads;}
void Start() noexcept {}
void Stop() noexcept {}
void Update() noexcept {}
void Draw(const AnomalyUiServiceV1*) noexcept {}
}
namespace lite_vehicle {
void Load(const AnomalyHostApiV1*) noexcept {++probe::loads;}
void Start() noexcept {}
AnomalyStatusV1 Stop() noexcept {return Ok();}
void Update() noexcept {}
void SetVisible(bool value) noexcept {probe::vehicle_visible=value;}
void Draw(const AnomalyUiServiceV1*) noexcept {}
}
int main() {
    using namespace probe;
    anomaly::testing::ProfileHost profile_host("SkinChanger");
    SetTestPackage(profile_host);
    AnomalyLocalizationServiceV1 language{sizeof(language),1};
    language.translate=[](void*,AnomalyStringViewV1 key,AnomalyStringViewV1,const AnomalyStringViewV1* args,size_t count,char* out,size_t* size) {
        std::string value;
        const auto id=Text(key);
        if(id=="window.title") value="\u6362\u80a4\u5668 Lite";
        else if(id=="slot.eyes") value="\u773c\u90e8";
        else if(id=="resource.selected") value=" (chosen)";
        else if(id=="resource.group_item") {
            Check(count==4,"resource text did not pass all localization arguments");
            value=Text(args[2])+" / "+Text(args[0])+" / "+Text(args[1])+Text(args[3]);
        } else return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
        const auto required=value.size()+1;
        if(!out || *size<required){*size=required;return AnomalyStatusV1{ANOMALY_STATUS_V1_BUFFER_TOO_SMALL,0,{}};}
        std::memcpy(out,value.c_str(),required);*size=required;return Ok();
    };
    AnomalyWindowServiceV1 windows{sizeof(windows),1};
    windows.register_window=[](void*,const AnomalyWindowSpecV1* spec,AnomalyGenerationHandleV1* handle){title=Text(spec->title);*handle={1,1};return Ok();};
    windows.release_window=[](void*,AnomalyGenerationHandleV1){++releases;return Ok();};
    windows.state=[](void*,AnomalyGenerationHandleV1,AnomalyWindowStateV1* state){state->open=open;return Ok();};
    windows.begin=[](void*,AnomalyGenerationHandleV1,uint32_t,int32_t* shown){if(fail_begin)return AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE,0,{}};++begins;*shown=expanded;return Ok();};
    windows.end=[](void*,AnomalyGenerationHandleV1){++ends;return Ok();};
    struct Services {const AnomalyLocalizationServiceV1* language;const AnomalyWindowServiceV1* windows;anomaly::testing::ProfileHost* profile;} services{&language,&windows,&profile_host};
    AnomalyHostApiV1 host{};host.struct_size=sizeof(host);host.api_major=1;host.host_context=&services;
    host.query_service=[](void* user,AnomalyStringViewV1 id,uint32_t,const void** out){
        const auto& source=*static_cast<Services*>(user);*out=nullptr;
        if(Text(id)==ANOMALY_LOCALIZATION_SERVICE_V1_ID){++queries;*out=source.language;}
        else if(Text(id)==ANOMALY_WINDOW_SERVICE_V1_ID)*out=source.windows;
        else return anomaly::testing::ProfileHost::Query(source.profile,id,1,out);
        return *out?Ok():AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
    };
    void* context{};
    Check(queries==0,"localization queried a host during static initialization");
    Check(Load(&host,&context).code==0 && loads==3 && queries==1,"features did not share a single localization query at Load");
    Check(Start(context).code==0 && title=="\u6362\u80a4\u5668 Lite","window title did not use the host translation");
    AnomalyUiServiceV1 ui{sizeof(ui),1};
    ui.begin_window=[](void*,AnomalyStringViewV1,int*,uint32_t){return expanded;};ui.end_window=[](void*){};
    ui.text=[](void*,AnomalyStringViewV1){};ui.same_line=[](void*,float,float){};
    ui.button=[](void*,AnomalyStringViewV1 label,float,float){const auto text=Text(label);return (pick==3 && text.ends_with("###category-awakening")) || (pick==5 && text.ends_with("###category-vehicle")) ? 1:0;};
    pick=5;Draw(context,&ui);pick=0;
    Check(vehicle_visible && !awakening_visible,"visible vehicle page did not receive interest");
    open=0;Draw(context,&ui);Check(!vehicle_visible && !awakening_visible,"closed window retained background page interest");
    open=1;expanded=0;Draw(context,&ui);Check(!vehicle_visible,"collapsed window retained background vehicle polling");
    expanded=1;Draw(context,&ui);Check(vehicle_visible,"reopened page failed to resume");
    fail_begin=1;Draw(context,&ui);Check(!vehicle_visible,"failed window begin retained page interest");fail_begin=0;
    pick=3;Draw(context,&ui);pick=0;Check(awakening_visible && !vehicle_visible,"page switch left the previous feature interested");
    Draw(context,nullptr);Check(!awakening_visible && !vehicle_visible,"missing UI retained stale interest");
    Check(begins==ends && queries==1,"draw leaked a window or queried another localizer");
    Check(lite_i18n::PlacementLabel("EYE",1)=="\u773c\u90e8","accessory slot did not use localization");
    const auto item=lite_i18n::ResourceLabel("Game name","Native_ID","Eyes",true,"appearance-");
    const auto other=lite_i18n::ResourceLabel("Game name","Native_ID","Eyes",false,"appearance-");
    Check(item=="Native_ID / Eyes / Game name (chosen)###appearance-Native_ID" && other.ends_with("###appearance-Native_ID"),
        "formatted resource text altered game identifiers, argument order or stable selection ID");
    Check(Stop(context,0).code==0 && releases==1 && !awakening_visible && !vehicle_visible,"stop retained its window/interest");
    window_service=nullptr;fallback_open=1;category=5;Draw(context,&ui);Check(vehicle_visible,"fallback window failed to show the selected page");
    fallback_open=0;Draw(context,&ui);Check(!vehicle_visible,"closed fallback window retained background interest");
    lite_i18n::localizer=anomaly::sdk::Localizer{};
    Check(lite_i18n::localizer.Text("window.title","SkinChanger Lite")=="SkinChanger Lite","missing localization did not fall back to English");
    Check(lite_i18n::localizer.Format("fixture","{0} / {{original}}",std::array<std::string_view,1>{"A"})=="A / {original}","English placeholder fallback failed");
    AnomalyPluginDescriptorV1 descriptor{sizeof(descriptor)};
    Check(AnomalyPluginEntryV1(&descriptor).code==0 && Text(descriptor.name)=="SkinChanger Lite" && Text(descriptor.version)=="0.1.44","descriptor metadata differs from the manifest");
    Unload(context);
    profile_host.image[0]=0xCC;
    context=reinterpret_cast<void*>(1);
    const auto before=loads;
    Check(Load(&host,&context).code==ANOMALY_STATUS_V1_UNAVAILABLE && !context && loads==before,
        "incompatible profile started features or returned a usable context");
    Check(profile_host.OutstandingHandles()==0,"failed lifecycle load leaked Profile JSON handles");
}
