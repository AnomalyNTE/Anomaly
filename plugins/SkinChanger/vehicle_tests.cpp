#include "vehicle_rules.hpp"
#include "vehicle_paint.hpp"
#include <cstring>
#include <iostream>
int main() {
    using namespace vehicle_rules;int failed{};
    const auto check=[&](bool ok,const char* why){if(!ok){++failed;std::cerr<<why<<'\n';}};
    std::vector<Row> rows{{10,1,1},{20,1,23},{21,1,23},{30,1,24},{40,1,6},{41,1,9},{50,2,23},{60,1,19},{61,1,19},{70,1,5}};
    std::vector<uint64_t> before{10,20,30,40,41,60,999};
    auto plan=Replace(before,rows,1,21);
    check(plan==std::optional<std::vector<uint64_t>>{{10,30,40,41,60,999,21}},"smoke change preserves engine, marks, separate attachment slots and unknown modules");
    check(!Replace(before,rows,1,50),"cross-model module refused");
    check(!Replace(before,rows,1,10),"unrequested engine module refused");
    check(!Replace(before,rows,1,123),"unlisted module refused");
    auto skin=Replace(before,rows,1,61);
    check(skin && std::find(skin->begin(),skin->end(),60)==skin->end() && std::find(skin->begin(),skin->end(),40)!=skin->end(),"skin replaces its own slot without losing attachments");
    auto repeated=Replace(*plan,rows,1,21);
    check(repeated==plan,"repeated selection cannot duplicate a module");
    check(!Replace(std::vector<uint64_t>(129,999),rows,1,21),"invalid module array is bounded");
    check(Category(5)==4 && Category(10)==0 && Category(16)==1 && Category(23)==2 && Category(24)==3,"five user categories map to native cosmetic slots");
    rows.insert(rows.end(),{{80,1,4},{81,1,4},{90,1,25},{91,1,25},{92,2,25}});
    auto with_hook=before;with_hook.insert(with_hook.end(),{80,90});
    const auto hook=Replace(with_hook,rows,1,91);
    check(hook && std::find(hook->begin(),hook->end(),90)==hook->end() && std::find(hook->begin(),hook->end(),80)!=hook->end(),
        "hook replacement preserves suspension and all other installed slots");
    const auto suspension=Replace(*hook,rows,1,81);
    check(suspension && std::find(suspension->begin(),suspension->end(),80)==suspension->end() &&
        std::find(suspension->begin(),suspension->end(),91)!=suspension->end() && std::find(suspension->begin(),suspension->end(),10)!=suspension->end(),
        "suspension replacement preserves hook and engine modules");
    check(!Replace(with_hook,rows,1,92),"hook catalog still refuses a different vehicle model");
    constexpr std::array<uint8_t,7> groups{6,8,9,11,25,4,16};
    for(int group=0;group<7;++group) for(int other=0;other<7;++other)
        check(Matches(groups[group],1,other)==(group==other),"attachment sections cannot display or apply another section's selection");
    check(Matches(7,1,6) && Matches(12,1,6) && Matches(14,1,6) && !Matches(1,1,6) && !Matches(23,1,6),
        "other accessories retain side kits, hood and roof without including engine or smoke");
    std::array<float,10> incoming{0,123,0,4,5,6,7,8,9,10};std::array<float,36> patched{};
    check(vehicle_paint::Override(0,incoming,3,patched) && patched[2]==3 && patched[1]==123 && patched[3]==4 && incoming[2]==0,
        "bulk native data retains entity and damage fields while overriding only the paint slot, without mutating caller input");
    check(vehicle_paint::Override(2,std::span<const float>(incoming.data(),1),7,patched) && patched[0]==7,"single-float writes to paint slot are handled");
    check(!vehicle_paint::Override(3,incoming,3,patched) && !vehicle_paint::Override(-1,incoming,3,patched) &&
        !vehicle_paint::Override(0,std::span<const float>(incoming.data(),2),3,patched),"unrelated or out-of-range writes remain untouched");
    return failed?1:0;
}
