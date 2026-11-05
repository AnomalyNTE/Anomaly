#pragma once
#include <anomaly/sdk/localization.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace lite_i18n {
// Initialized exactly once by the plugin's Load callback. All feature modules
// share the same host service table; no service calls during static setup.
inline anomaly::sdk::Localizer localizer;
inline std::string PlacementLabel(std::string_view type, uint8_t part) {
    if(part==1 && type=="EYE") return lite_i18n::localizer.Text("slot.eyes", "Eyes");
    if(part==1 && type=="OverHead") return lite_i18n::localizer.Text("slot.crown", "Crown");
    if(part==1) return lite_i18n::localizer.Text("slot.head", "Head");
    if(part==2) return lite_i18n::localizer.Text("slot.front", "Front");
    if(part==3) return lite_i18n::localizer.Text("slot.back", "Back");
    if(part==4) return lite_i18n::localizer.Text("slot.legs", "Legs");
    return lite_i18n::localizer.Text("common.unknown", "Unknown");
}
inline std::string ResourceLabel(std::string_view name,std::string_view id,std::string_view group,bool selected,std::string_view prefix) {
    const auto suffix=selected?lite_i18n::localizer.Text("resource.selected", " (selected)"):std::string{};
    std::string text;
    if(name.empty()) text=lite_i18n::localizer.Format("resource.id", "{0}{1}", std::array<std::string_view,2>{id,suffix});
    else if(group.empty()) text=lite_i18n::localizer.Format("resource.item", "{0} [{1}]{2}", std::array<std::string_view,3>{name,id,suffix});
    else text=lite_i18n::localizer.Format("resource.group_item", "{0} - {1} [{2}]{3}", std::array<std::string_view,4>{group,name,id,suffix});
    return text+"###"+std::string(prefix)+std::string(id);
}

}
