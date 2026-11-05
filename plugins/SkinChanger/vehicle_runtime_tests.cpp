#include "profile_test_setup.hpp"
#include "vehicle.cpp"
#include <cstdlib>
#include <iostream>

std::uintptr_t CosmeticsNameArrayCopyAddress() noexcept {return 0;}
void Check(bool value,const char* message) {if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main() {
    LoadTestProfile();
    lite_vehicle::Load(nullptr);lite_vehicle::Start();
    for(uint64_t n=1;n<=1000;++n) entries.push_back({{n,1,6},std::to_string(n),std::string(200,'x')});
    Target target;target.actor={42,1};target.vehicle={1,0};
    Publish(Reader{},target);
    const auto first=published.load()->rows;
    Check(first && first->size()==1000,"vehicle catalog was not published");
    for(int n=0;n<100;++n) {Publish(Reader{},target);Check(published.load()->rows==first,"unchanged vehicle catalog was copied again on an update");}
    entries.clear();entries.push_back({{2000,2,9},"New_ID","New catalog"});catalog_snapshot.reset();
    Publish(Reader{},target);
    Check(published.load()->rows!=first && published.load()->rows->at(0).id=="New_ID" && first->size()==1000,
        "catalog replacement changed an in-flight render snapshot or retained stale rows");
    Publish(Reader{},{});Check(!published.load()->rows,"missing vehicle continued publishing the old vehicle's resources");
    for(uint64_t n=1;n<=20;++n) saved.emplace(n,Saved{});
    monitor_cursor=10;
    Monitor(Reader{},nullptr,0);
    Check(saved.size()==19 && !saved.contains(11) && saved.contains(1),"vehicle maintenance was unbounded or restarted at the first ID");
    for(int n=0;n<19;++n) {const auto before=saved.size();Monitor(Reader{},nullptr,0);Check(saved.size()+1==before,"maintenance skipped or processed multiple invalid vehicles");}
    Check(saved.empty(),"round-robin maintenance starved saved vehicles");
    lite_vehicle::Start();Check(!catalog_snapshot && entries.empty() && monitor_cursor==0,"reload retained old catalog/maintenance state");
    Check(lite_vehicle::Stop().code==0,"vehicle shutdown failed");
}
