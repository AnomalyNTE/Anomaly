#include "model.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
void Check(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
int main() {
    try {
        Check(scene::CanEnter(2,true) && scene::CanEnter(0,true),"configured gameplay scenes are switchable");
        Check(!scene::CanEnter(1,true) && !scene::CanEnter(4,true) && !scene::CanEnter(3,false),"frontend or missing entrance cannot enter the gameplay transfer path");
        Check(scene::SameSceneSwitch("DLC_WuChao","DLC_WuChao",true),"another entrance of the current scene is switchable without leaving");
        Check(!scene::SameSceneSwitch("DLC_WuChao","DLC_WuChao",false) && !scene::SameSceneSwitch("city","hotel",true),"in-scene switching needs entrances and the same scene");
        {
            // Bytes captured from a real ClientDataLayerRPC: 2B 00 00 00 "DataLayer_843E..." 00 (47 bytes).
            const auto payload=scene::LayerNamePayload("DataLayer_843EE3904E1E56A4F9B8BFB67668019A");
            Check(payload.size()==47 && payload[0]==0x2B && payload[1]==0 && payload[2]==0 && payload[3]==0 &&
                payload[4]=='D' && payload[46]==0 && payload[45]=='A',"layer payload matches the captured 47-byte FString");
        }
        Check(std::string(scene::LayerStateName(2).fallback)=="Activated" && std::string(scene::LayerStateName(0).fallback)=="Unloaded" &&
            std::string(scene::LayerStateName(1).key)=="layer.loaded","layer states are labelled");
        scene::Transition t;
        Check(!t.Begin("city","city",0) && !t.Begin("city","",0),"ignore current/empty destinations");
        Check(t.Begin("city","hotel",1000) && !t.Begin("city","castle",1100),"only one transfer can be pending");
        Check(t.Check("city",5000)==scene::Transition::Result::Waiting && t.busy,"an accepted request alone is not success");
        Check(t.Check("hotel",6000)==scene::Transition::Result::Arrived && !t.busy,"actual destination world confirms arrival");
        t.Begin("hotel","city",7000);
        Check(t.Check("castle",8000)==scene::Transition::Result::Different && !t.busy,"other scene changes end the old wait");
        t.Begin("castle","city",9000);
        Check(t.Check("castle",39000)==scene::Transition::Result::Timeout && !t.busy,"requests time out without automatic resends");
        t.Begin("city","broken-map",40000);t.Recover("city","city",41000);
        Check(t.busy && t.target=="city","recovery replaces a stuck transfer even when still in the source world");
        Check(t.Check("city",42000)==scene::Transition::Result::Waiting,"being in the old world alone cannot confirm recovery");
        Check(t.Check("city",43000,true)==scene::Transition::Result::Arrived,"the game's transfer acknowledgement confirms same-world recovery");
        Check(scene::Nearest({{0,0,0},{100,50,20}},{{95,50,20}})==1,"return prefers the configured entrance nearest the last world position");
        Check(!scene::Verified("MassAI_wtao") && scene::Verified("DLC_Hotel"),"untested configured maps are distinguished from verified transfers");
        Check(scene::Unusable("DLC_Bank_Safety") && scene::Unusable("DLC_Castle_Demo") && scene::Unusable("DLC_DeadNumber") &&
            !scene::Verified("DLC_Bank_Safety"),"scenes that failed in live tests are unusable, not verified");
        Check(scene::Verified("DLC_SpiralDreams") && scene::Verified("DLC_MoonDog") && !scene::Unusable("DLC_SpiralDreams"),"scenes that worked in live tests are verified");
        Check(!scene::Verified("DLC_NeverTried") && !scene::Unusable("DLC_NeverTried") &&
            std::string(scene::StatusLabel("DLC_NeverTried").key)=="scene.unverified","untested scenes, switchable or not, stay unverified");
        Check(std::string(scene::StatusLabel("DLC_Bank_Safety").key)=="scene.unusable" && scene::UnusableReason("DLC_Bank_Safety").key &&
            !scene::UnusableReason("DLC_SpiralDreams").key,"unusable scenes carry a reason, the others none");
        {
            // Every text the model hands to the UI must exist in the shipped zh-CN catalog, or the
            // panel silently falls back to English for that line.
            std::ifstream file(SCENE_LOADER_ZH_CN_CATALOG,std::ios::binary);
            Check(file.good(),"the zh-CN catalog is readable");
            const std::string catalog((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            const auto has=[&](const scene::Text& text) {return catalog.find(std::string("\"")+text.key+"\"")!=std::string::npos;};
            for(uint8_t state=0;state<4;++state) Check(has(scene::LayerStateName(state)),"every layer state is translated");
            for(uint8_t type=0;type<10;++type) Check(has(scene::TypeName(type)),"every scene type is translated");
            for(const auto& known:scene::kKnownScenes) {
                Check(has(scene::StatusLabel(known.key)),"every scene status is translated");
                if(known.reason.key) Check(has(known.reason),"every unusable reason is translated");
            }
            Check(has(scene::StatusLabel("DLC_NeverTried")),"the unverified status is translated");
        }
        Check(scene::HoldsExit("DLC_Hockshop") && scene::Verified("DLC_Hockshop") && !scene::HoldsExit("DLC_WuChao"),
            "the pawnshop is entered with its own exit request held back; other scenes are left alone");
        // Flags read from the live ServerRequestExitSingleLevelSceneRPC: 0x00220CC0, and the value the
        // probe wrote and kept the player in the pawnshop with: 0x00220C80.
        Check(scene::CanHoldExit(0x00220CC0) && scene::HeldExitFlags(0x00220CC0)==0x00220C80,"holding clears only the network flag of the server RPC");
        Check(!scene::CanHoldExit(0x00220C80) && !scene::CanHoldExit(0x00000400),"a function that is not a server RPC is never touched");
        Check(scene::SceneConnectionError("目标场景失去联络信号，请稍后重试"),"identify the reported scene error");
        Check(!scene::SceneConnectionError("连接失败") && !scene::SceneConnectionError("确认购买？"),"unrelated network and purchase dialogs remain untouched");
        std::cout<<"Scene selection and transition tracking passed.\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
