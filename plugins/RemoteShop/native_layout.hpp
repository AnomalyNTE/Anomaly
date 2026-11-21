#pragma once
#include <anomaly/sdk/profile.hpp>

namespace shop {
struct NativeLayout {
    uint32_t objectFlags{};
    uint32_t objectIndex{};
    uint32_t objectClass{};
    uint32_t objectName{};
    uint32_t objectOuter{};
    uint32_t structSuper{};
    uint32_t structProperties{};
    uint32_t propertyName{};
    uint32_t propertySize{};
    uint32_t propertyOffset{};
    uint32_t propertyLinkNext{};
    uint32_t structChildren{};
    uint32_t fieldNext{};
    uint32_t functionFlags{};
    uint32_t functionParameterCount{};
    uint32_t functionParameterSize{};
    uint32_t actorProcessEvent{};
    uint32_t textDisplayGetter{};
    uint32_t textSharedGetter{};
    uint32_t tableRowStruct{};
    uint32_t tableRowMap{};
    uint32_t mapStride{};
    uint32_t mapKey{};
    uint32_t mapValue{};
    uint32_t iconPackage{};
    uint32_t iconAsset{};
    bool Load(const anomaly::sdk::Profile& profile) {
        try {
            objectFlags = profile.Value("objectFlags",4096,4);
            objectIndex = profile.Value("objectIndex",4096,4);
            objectClass = profile.Value("objectClass",4096,8);
            objectName = profile.Value("objectName",4096,4);
            objectOuter = profile.Value("objectOuter",4096,8);
            structSuper = profile.Value("structSuper",4096,8);
            structProperties = profile.Value("structProperties",4096,8);
            propertyName = profile.Value("propertyName",4096,4);
            propertySize = profile.Value("propertySize",4096,4);
            propertyOffset = profile.Value("propertyOffset",4096,4);
            propertyLinkNext = profile.Value("propertyLinkNext",4096,8);
            structChildren = profile.Value("structChildren",4096,8);
            fieldNext = profile.Value("fieldNext",4096,8);
            functionFlags = profile.Value("functionFlags",4096,4);
            functionParameterCount = profile.Value("functionParameterCount",4096,1);
            functionParameterSize = profile.Value("functionParameterSize",4096,2);
            actorProcessEvent = profile.Value("actorProcessEvent",4096,8);
            textDisplayGetter = profile.Value("textDisplayGetter",4096,8);
            textSharedGetter = profile.Value("textSharedGetter",4096,8);
            tableRowStruct = profile.Value("tableRowStruct",4096,8);
            tableRowMap = profile.Value("tableRowMap",4096,8);
            mapStride = profile.Value("mapStride",4096,8);
            mapKey = profile.Value("mapKey",4096,8);
            mapValue = profile.Value("mapValue",4096,8);
            iconPackage = profile.Value("iconPackage",4096,8);
            iconAsset = profile.Value("iconAsset",4096,8);
            return mapStride >= sizeof(uint64_t) + sizeof(uintptr_t) && mapStride <= 256 &&
                mapKey <= mapStride - sizeof(uint64_t) && mapValue <= mapStride - sizeof(uintptr_t) &&
                (mapKey + sizeof(uint64_t) <= mapValue || mapValue + sizeof(uintptr_t) <= mapKey);
        } catch (...) { return false; }
    }
};
} // namespace shop
