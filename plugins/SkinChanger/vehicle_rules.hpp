#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>
namespace vehicle_rules {
struct Row {uint64_t id{},vehicle{};uint8_t type{};};
inline int AttachmentGroup(uint8_t type) {
    switch(type) {
    case 6:return 0;  // Front lip/body kit.
    case 8:return 1;  // Rear lip/body kit.
    case 9:return 2;  // Spoiler.
    case 11:return 3; // Wheels.
    case 25:return 4; // Hook.
    case 4:return 5;  // Suspension.
    case 7:case 12:case 14:case 16:return 6; // Side kits, hood, roof and accessories.
    default:return -1;
    }
}
inline int Category(uint8_t type) {
    if(type==10 || type==19) return 0;
    if(AttachmentGroup(type)>=0) return 1;
    if(type==23) return 2;
    if(type==24) return 3;
    if(type==5) return 4;
    return -1;
}
inline bool Matches(uint8_t type,int category,int attachment_group) {
    return Category(type)==category && (category!=1 || AttachmentGroup(type)==attachment_group);
}
inline std::optional<std::vector<uint64_t>> Replace(const std::vector<uint64_t>& current,
    const std::vector<Row>& catalog,uint64_t vehicle,uint64_t selected) {
    if(!vehicle || !selected || current.size()>128) return {};
    const auto row=std::find_if(catalog.begin(),catalog.end(),[&](const Row& r){return r.id==selected && r.vehicle==vehicle;});
    if(row==catalog.end() || Category(row->type)<0) return {};
    std::vector<uint64_t> result;
    for(auto id:current) {
        if(id==selected) continue;
        const auto old=std::find_if(catalog.begin(),catalog.end(),[&](const Row& r){return r.id==id && r.vehicle==vehicle;});
        if(old==catalog.end() || old->type!=row->type) result.push_back(id);
    }
    result.push_back(selected);
    if(result.size()>128) return {};
    return result;
}
}
