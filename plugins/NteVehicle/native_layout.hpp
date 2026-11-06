#pragma once
#include <anomaly/sdk/profile.hpp>

namespace nte_vehicle {
// Validated snapshot of the plugin compatibility profile. Vehicle-specific fields
// are populated from runtime reflection, including nested engine config fields.
struct NativeLayout {
    std::int64_t dataTable_rowMap{};
    std::int64_t dataTable_rowMapData{};
    std::int64_t dataTable_rowMapElementStride{};
    std::int64_t dataTable_rowMapFlagsData{};
    std::int64_t dataTable_rowMapFlagsMax{};
    std::int64_t dataTable_rowMapFlagsNum{};
    std::int64_t dataTable_rowMapInlineFlags{};
    std::int64_t dataTable_rowMapMax{};
    std::int64_t dataTable_rowMapNum{};
    std::int64_t dataTable_rowMapNumFree{};
    std::int64_t dataTable_rowMapRowOffset{};
    std::int64_t dataTable_rowStruct{};
    std::int64_t entities_maxCount{};
    std::int64_t entities_maxLevels{};
    std::int64_t farrayProperty_inner{};
    std::int64_t fboolProperty_byteMask{};
    std::int64_t fboolProperty_byteOffset{};
    std::int64_t fboolProperty_fieldMask{};
    std::int64_t fboolProperty_fieldSize{};
    std::int64_t ffield_class{};
    std::int64_t ffield_name{};
    std::int64_t ffieldClass_name{};
    std::int64_t fproperty_arrayDim{};
    std::int64_t fproperty_elementSize{};
    std::int64_t fproperty_offsetInternal{};
    std::int64_t fproperty_propertyLinkNext{};
    std::int64_t fstructProperty_struct{};
    std::int64_t level_actors{};
    std::int64_t object_class{};
    std::int64_t object_internalIndex{};
    std::int64_t object_nameOffset{};
    std::int64_t object_outer{};
    std::int64_t objects_chunkSize{};
    std::int64_t objects_countOffset{};
    std::int64_t objects_instructionDisplacement{};
    std::int64_t objects_instructionSize{};
    std::int64_t objects_itemStride{};
    std::int64_t objects_itemsOffset{};
    std::int64_t objects_maxCountOffset{};
    std::int64_t objects_objectOffset{};
    std::int64_t objects_registrySubtract{};
    std::int64_t objects_serialOffset{};
    std::int64_t tarray_data{};
    std::int64_t tarray_max{};
    std::int64_t tarray_num{};
    std::int64_t uclass_classDefaultObject{};
    std::int64_t ufunction_numParms{};
    std::int64_t ufunction_parmsSize{};
    std::int64_t ufunction_returnValueOffset{};
    std::int64_t ustruct_propertiesSize{};
    std::int64_t ustruct_propertyLink{};
    std::int64_t ustruct_superStruct{};
    std::int64_t world_levels{};
    std::int64_t vehicle_movementComponent{-1};
    std::int64_t vehicle_movementComponentSize{-1};
    std::int64_t vehicle_enableTopSpeedLimit{-1};
    std::int64_t vehicle_maxEngineTorque{-1};
    std::int64_t vehicle_maxEngineRPM{-1};
    std::int64_t vehicle_maxLinearVelocity{-1};
    std::int64_t vehicle_topSpeedInKmH{-1};
    std::int64_t vehicle_reverseTopSpeedInKmH{-1};
    std::int64_t vehicle_topSpeedRatio{-1};
    std::int64_t vehicle_comfortMaxEngineTorque{-1};
    std::int64_t vehicle_comfortMaxEngineRPM{-1};
    std::int64_t vehicle_comfortTopSpeedInKmH{-1};

    void Load(const anomaly::sdk::Profile& profile) {
        dataTable_rowMap = profile.Value("dataTable.rowMap", 65536, 8);
        dataTable_rowMapData = profile.Value("dataTable.rowMapData", 65536, 8);
        dataTable_rowMapElementStride = profile.Value("dataTable.rowMapElementStride", 65536, 1);
        dataTable_rowMapFlagsData = profile.Value("dataTable.rowMapFlagsData", 65536, 8);
        dataTable_rowMapFlagsMax = profile.Value("dataTable.rowMapFlagsMax", 65536, 4);
        dataTable_rowMapFlagsNum = profile.Value("dataTable.rowMapFlagsNum", 65536, 4);
        dataTable_rowMapInlineFlags = profile.Value("dataTable.rowMapInlineFlags", 65536, 8);
        dataTable_rowMapMax = profile.Value("dataTable.rowMapMax", 65536, 4);
        dataTable_rowMapNum = profile.Value("dataTable.rowMapNum", 65536, 4);
        dataTable_rowMapNumFree = profile.Value("dataTable.rowMapNumFree", 65536, 4);
        dataTable_rowMapRowOffset = profile.Value("dataTable.rowMapRowOffset", 65536, 8);
        dataTable_rowStruct = profile.Value("dataTable.rowStruct", 65536, 8);
        entities_maxCount = profile.Value("entities.maxCount", 16777216, 1);
        entities_maxLevels = profile.Value("entities.maxLevels", 16777216, 1);
        farrayProperty_inner = profile.Value("farrayProperty.inner", 65536, 8);
        fboolProperty_byteMask = profile.Value("fboolProperty.byteMask", 65536, 1);
        fboolProperty_byteOffset = profile.Value("fboolProperty.byteOffset", 65536, 1);
        fboolProperty_fieldMask = profile.Value("fboolProperty.fieldMask", 65536, 1);
        fboolProperty_fieldSize = profile.Value("fboolProperty.fieldSize", 65536, 1);
        ffield_class = profile.Value("ffield.class", 65536, 8);
        ffield_name = profile.Value("ffield.name", 65536, 4);
        ffieldClass_name = profile.Value("ffieldClass.name", 65536, 4);
        fproperty_arrayDim = profile.Value("fproperty.arrayDim", 65536, 4);
        fproperty_elementSize = profile.Value("fproperty.elementSize", 65536, 4);
        fproperty_offsetInternal = profile.Value("fproperty.offsetInternal", 65536, 4);
        fproperty_propertyLinkNext = profile.Value("fproperty.propertyLinkNext", 65536, 8);
        fstructProperty_struct = profile.Value("fstructProperty.struct", 65536, 8);
        level_actors = profile.Value("level.actors", 65536, 8);
        object_class = profile.Value("object.class", 65536, 8);
        object_internalIndex = profile.Value("object.internalIndex", 65536, 4);
        object_nameOffset = profile.Value("object.nameOffset", 65536, 4);
        object_outer = profile.Value("object.outer", 65536, 8);
        objects_chunkSize = profile.Value("objects.chunkSize", 65536, 1);
        objects_countOffset = profile.Value("objects.countOffset", 65536, 4);
        objects_instructionDisplacement = profile.Value("objects.instructionDisplacement", 65536, 1);
        objects_instructionSize = profile.Value("objects.instructionSize", 65536, 1);
        objects_itemStride = profile.Value("objects.itemStride", 65536, 1);
        objects_itemsOffset = profile.Value("objects.itemsOffset", 65536, 8);
        objects_maxCountOffset = profile.Value("objects.maxCountOffset", 65536, 4);
        objects_objectOffset = profile.Value("objects.objectOffset", 65536, 8);
        objects_registrySubtract = profile.Value("objects.registrySubtract", 65536, 8);
        objects_serialOffset = profile.Value("objects.serialOffset", 65536, 4);
        tarray_data = profile.Value("tarray.data", 65536, 8);
        tarray_max = profile.Value("tarray.max", 65536, 4);
        tarray_num = profile.Value("tarray.num", 65536, 4);
        uclass_classDefaultObject = profile.Value("uclass.classDefaultObject", 65536, 8);
        ufunction_numParms = profile.Value("ufunction.numParms", 65536, 1);
        ufunction_parmsSize = profile.Value("ufunction.parmsSize", 65536, 2);
        ufunction_returnValueOffset = profile.Value("ufunction.returnValueOffset", 65536, 2);
        ustruct_propertiesSize = profile.Value("ustruct.propertiesSize", 65536, 4);
        ustruct_propertyLink = profile.Value("ustruct.propertyLink", 65536, 8);
        ustruct_superStruct = profile.Value("ustruct.superStruct", 65536, 8);
        world_levels = profile.Value("world.levels", 65536, 8);
    }

    std::int64_t VehicleField(std::string_view key) const {
        if (key == "vehicle.movementComponent") return vehicle_movementComponent;
        if (key == "vehicle.movementComponentSize") return vehicle_movementComponentSize;
        if (key == "vehicle.enableTopSpeedLimit") return vehicle_enableTopSpeedLimit;
        if (key == "vehicle.maxEngineTorque") return vehicle_maxEngineTorque;
        if (key == "vehicle.maxEngineRPM") return vehicle_maxEngineRPM;
        if (key == "vehicle.maxLinearVelocity") return vehicle_maxLinearVelocity;
        if (key == "vehicle.topSpeedInKmH") return vehicle_topSpeedInKmH;
        if (key == "vehicle.reverseTopSpeedInKmH") return vehicle_reverseTopSpeedInKmH;
        if (key == "vehicle.topSpeedRatio") return vehicle_topSpeedRatio;
        if (key == "vehicle.comfortMaxEngineTorque") return vehicle_comfortMaxEngineTorque;
        if (key == "vehicle.comfortMaxEngineRPM") return vehicle_comfortMaxEngineRPM;
        if (key == "vehicle.comfortTopSpeedInKmH") return vehicle_comfortTopSpeedInKmH;
        return -1;
    }
};
} // namespace nte_vehicle
