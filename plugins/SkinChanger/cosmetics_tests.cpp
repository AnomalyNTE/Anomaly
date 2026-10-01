#include "appearance.hpp"
#include "glider.hpp"
#include "cosmetic_profiles.hpp"
#include <cstring>
#include <iostream>
#include <cstdlib>
using namespace accessory;
void Check(bool condition,const char* message){if(!condition){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    Check(Placement("EYE",1)=="眼部"&&Placement("OverHead",1)=="头顶"&&
          Placement("",3)=="背部","decoration placement mislabeled");
    Name eye_type{100,0},top_type{101,0};
    std::array<ItemKey,4> head_catalog{{{{10,0},1,eye_type},{{11,0},1,eye_type},
                                        {{20,0},1,top_type},{{21,0},1,top_type}}};
    std::array<ItemKey,2> two_slots{{{{11,0},1,eye_type},{{21,0},1,top_type}}};
    std::array<Name,2> original_head{{{10,0},{20,0}}};
    std::array<Name,MaxAccessories> composed{};int composed_count{};
    Check(ComposeSelections(original_head,head_catalog,two_slots,false,composed,composed_count)&&
          composed_count==2&&composed[0]==two_slots[0].id&&composed[1]==two_slots[1].id,
          "eye and overhead accessories did not coexist");
    Check(ComposeSelections(original_head,head_catalog,
          std::span<const ItemKey>(two_slots.data()+1,1),false,composed,composed_count)&&
          composed_count==2&&composed[0]==original_head[0]&&composed[1]==two_slots[1].id,
          "restoring eye slot changed overhead slot");
    cosmetic_profiles::Profiles profiles;
    cosmetic_profiles::SetAccessory(profiles,"1076","1:EYE","Eye_One");
    cosmetic_profiles::SetAccessory(profiles,"1076","1:OverHead","Hat_One");
    cosmetic_profiles::SetGlider(profiles,"1076","Fashion_Glide_all_2");
    cosmetic_profiles::SetAccessory(profiles,"1070","1:EYE","Mask_Two");
    Check(profiles.at("1076").accessories.at("1:EYE")=="Eye_One" &&
          profiles.at("1076").accessories.at("1:OverHead")=="Hat_One" &&
          profiles.at("1076").glider=="Fashion_Glide_all_2" &&
          profiles.at("1070").accessories.at("1:EYE")=="Mask_Two","character looks collided");
    cosmetic_profiles::ResetAccessorySlot(profiles,"1076","1:EYE");
    Check(!profiles.at("1076").accessories.contains("1:EYE") &&
          profiles.at("1076").accessories.at("1:OverHead")=="Hat_One" &&
          profiles.at("1076").glider=="Fashion_Glide_all_2" &&
          profiles.at("1070").accessories.at("1:EYE")=="Mask_Two","reset affected another look");
    Name hat{10,0},mask{11,0},back{20,0},unknown{99,7};
    std::array<ItemKey,3> catalog{{{hat,1},{mask,1},{back,3}}};
    std::array<Name,3> old{hat,back,unknown};
    std::array<Name,MaxAccessories> out{};int count{};
    Check(Compose(old,catalog,{Mode::ReplacePart,{mask,1}},out,count),"replace failed");
    Check(count==3&&out[0]==back&&out[1]==unknown&&out[2]==mask,"wrong slot changed");
    Check(old[0]==hat,"source array modified");
    Check(Compose(old,catalog,{Mode::Original,{}},out,count)&&count==3&&out[0]==hat,"restore lost original");
    Check(Compose(old,catalog,{Mode::HideAll,{}},out,count)&&count==0,"hide left accessories");
    Check(!Compose(old,catalog,{Mode::ReplacePart,{{45,0},1}},out,count),"unknown selection accepted");
    Check(!Compose(old,catalog,{Mode::ReplacePart,{mask,3}},out,count),"incorrect slot accepted");
    Check(Compose({},catalog,{Mode::ReplacePart,{mask,1}},out,count)&&count==1&&out[0]==mask,"empty equipment failed");
    std::array<Name,MaxAccessories> full;full.fill(unknown);
    Check(!Compose(full,catalog,{Mode::ReplacePart,{mask,1}},out,count),"capacity overflow accepted");
    Check(Compose(std::span<const Name>(out.data(),0),catalog,{Mode::HideAll,{}},out,count)&&count==0,"empty hide failed");
    std::uint8_t subpath[]{'s','u','b',0};SoftClass soft{0,{100,0},{101,0},subpath,4,4};
    auto read=[&](std::uintptr_t address,void* output,std::size_t size){
        if((address==reinterpret_cast<std::uintptr_t>(&soft)&&size==sizeof(soft))||
           (address==reinterpret_cast<std::uintptr_t>(subpath)&&size==sizeof(subpath))){
            std::memcpy(output,reinterpret_cast<void*>(address),size);return true;
        }return false;
    };
    GliderPath path;Check(path.Capture(reinterpret_cast<std::uintptr_t>(&soft),read),"Glider path capture failed");
    subpath[0]='x';auto view=path.View();
    Check(view.subpath!=subpath&&view.subpath[0]=='s'&&view.count==4,"Glider snapshot aliases game memory");
    auto cloned=path;path={};Check(cloned.View().subpath[0]=='s',"Copied glider path refers to old owner");
    soft.count=4097;Check(!path.Capture(reinterpret_cast<std::uintptr_t>(&soft),read),"Oversized glider path accepted");
    soft.count=4;soft.capacity=3;Check(!path.Capture(reinterpret_cast<std::uintptr_t>(&soft),read),"Invalid glider string capacity accepted");
    soft.count=soft.capacity=0;soft.subpath=nullptr;
    Check(path.Capture(reinterpret_cast<std::uintptr_t>(&soft),read)&&path.View().subpath==nullptr,"Empty subobject path rejected");
    soft.asset={};Check(!path.Capture(reinterpret_cast<std::uintptr_t>(&soft),read),"Null glider asset accepted");
    std::cout<<"Accessory rules and owned glider resource snapshots passed\n";
}
