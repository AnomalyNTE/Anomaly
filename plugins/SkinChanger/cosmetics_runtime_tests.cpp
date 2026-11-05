#include "profile_test_setup.hpp"
#include "cosmetics.cpp"
#include <cstdlib>
#include <iostream>

void Check(bool value,const char* message) {if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main() {
    LoadTestProfile();
    Context context;
    // A matching layout alone cannot enable unmatched native glider functions.
    Check(!BindGlider(context),"unresolved glider functions attempted native binding");
    ScheduleCharacterLook(context,0);
    AnomalySignatureServiceV1 signatures{sizeof(signatures),1};
    signatures.resolve=[](void*,AnomalyStringViewV1,AnomalyStringViewV1,AnomalyStringViewV1,std::uintptr_t*) {
        return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
    };
    Context unsupported;unsupported.signatures=&signatures;
    Check(CosmeticsStart(&unsupported).code==ANOMALY_STATUS_V1_UNAVAILABLE && !unsupported.waiting_message &&
        unsupported.message.find("signatures did not match")!=std::string::npos && unsupported.glider_message==unsupported.message,
        "native binding failure was hidden behind a permanent catalog-loading message");
    std::vector<Row> catalog;
    for(uint32_t n=1;n<=500;++n) catalog.push_back({{{n,0},1,{10,0}},std::to_string(n),"Game resource "+std::to_string(n),"EYE"});
    context.rows=std::make_shared<const std::vector<Row>>(std::move(catalog));context.ready=true;
    std::weak_ptr<const std::vector<Row>> previous=context.rows;
    struct UiProbe {Context* context;std::weak_ptr<const std::vector<Row>> previous;unsigned resources{};bool replaced{};} probe{&context,previous};
    AnomalyUiServiceV1 ui{sizeof(ui),1,&probe};
    ui.text=[](void*,AnomalyStringViewV1){};ui.same_line=[](void*,float,float){};
    ui.button=[](void*,AnomalyStringViewV1,float,float){return 0;};
    ui.input_text=[](void*,AnomalyStringViewV1,char*,size_t,uint32_t){return 0;};
    ui.begin_child=[](void*,AnomalyStringViewV1,float,float,uint32_t){return 1;};ui.end_child=[](void*){};
    ui.button_enabled=[](void* user,AnomalyStringViewV1 value,float,float,int){
        auto& p=*static_cast<UiProbe*>(user);const std::string_view label(value.data,value.size);
        if(label.find("###appearance-")!=std::string_view::npos){
            ++p.resources;
            if(!p.replaced){
                std::scoped_lock lock(p.context->mutex);
                p.context->rows=std::make_shared<const std::vector<Row>>();p.replaced=true;
                Check(!p.previous.expired() && p.previous.use_count()==1,"render copied the catalog or failed to retain its immutable snapshot");
            }
        }
        return 0;
    };
    DrawCosmetics(&context,&ui,1);
    Check(probe.resources==500 && previous.expired() && context.rows->empty(),"catalog replacement invalidated rendering or leaked the old snapshot");
    probe.resources=0;DrawCosmetics(&context,&ui,1);Check(probe.resources==0,"next draw reused stale catalog rows");
    context.waiting_message=true;Message(context,"Example message");Check(!context.waiting_message,"messages retained an unrelated loading flag");
    Message(context,"Waiting",true);Check(context.waiting_message,"waiting state depends on translated message content");
}
