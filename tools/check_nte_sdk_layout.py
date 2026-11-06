"""Check compatibility layouts against independent Dumper-7 SDK declarations."""
import argparse
import json
import pathlib
import re


def read_sdk(directory):
    result = {}
    for path in (pathlib.Path(directory) / "CppSDK" / "SDK").glob("*.hpp"):
        if path.name != "Basic.hpp" and not path.name.endswith(("_classes.hpp", "_structs.hpp")):
            continue
        text = path.read_text(encoding="utf-8")
        pattern = (r"// (?:Class|ScriptStruct|Predefined struct) [^\n]+\n// 0x[0-9A-F]+ "
                   r"\((0x[0-9A-F]+) - [^\n]+\n(?:#pragma[^\n]+\n)?"
                   r"(?:class|struct) (?:(?:SDK_ALIGN|alignas)\([^)]*\) )?(\w+)[^\n]*\n\{(.*?)\n\};")
        for match in re.finditer(pattern, text, re.S):
            fields = {field[1]: (int(field[2], 16), int(field[3], 16))
                      for field in re.finditer(
                          r"\b(\w+)\s*(?:\[[^]]+\])?;\s*// (0x[0-9A-F]+)\((0x[0-9A-F]+)\)",
                          match[3])}
            result[match[2]] = {"size": int(match[1], 16), "fields": fields}
    return result


# Names are public reflection/SDK identities, not a second table of offsets.
CORE_FIELDS = {
    "playerState.roleName": ("AHottaPlayerState", "m_strRoleName"),
    "abilitySpawnActor.triggerAbilityHandle": ("AHTAbilitySpawnActor", "TriggerAbilityHandle"),
    "abilitySpawnActor.savedTriggerSkillCDO": ("AHTAbilitySpawnActor", "SavedTriggerSkillCDO"),
    "abilityCharacter.abilitySystemComponent": ("AHTAbilityCharacter", "HTAbilitySystemComponent"),
    "abilityCharacter.characterConfigId": ("AHTAICharacter", "m_CharacterConfigID"),
    "playerCharacter.defaultCharacterId": ("AHTPlayerCharacter", "DefaultCharacterID"),
    "playerCharacter.currentDisplayCharacterId": ("AHTPlayerCharacter", "CurrentDisplayCharacterID"),
    "abilitySystem.currentDamageCauserOrInstigator": ("UHTAbilitySystemComponent", "CurrentDamageCauserOrInstigator"),
    "abilitySystem.currentDamageGameplayEffect": ("UHTAbilitySystemComponent", "CurrentDamageGameplayEffect"),
    "gameData.abilityDataAsset": ("UHTGameData", "HTAbilityDataAsset"),
    "gameData.gameplayAbilityTipsDataTable": ("UHTGameData", "GameplayAbilityTipsDataTable"),
    "gameData.gameplayEffectTipsDataTable": ("UHTGameData", "GameplayEffectTipsDataTable"),
    "gameData.monsterInfoDataTable": ("UHTGameData", "MonsterInfoDataTable"),
    "gameData.characterDataTable": ("UHTGameData", "CharacterDataTable"),
    "gameData.characterDataAsset": ("UHTGameData", "CharacterDataAsset"),
    "gameData.survivalGamePlayDataAsset": ("UHTGameData", "SurvivalGamePlayDataAsset"),
    "gameData.teleportPointDataTable": ("UHTGameData", "TeleportPointDataTable"),
}
CORE_SIZES = {
    "gameData.size": "UHTGameData",
    "abilityData.size": "UHTAbilityDataAsset",
    "abilityCharacter.size": "AHTAbilityCharacter",
    "aiCharacter.size": "AHTAICharacter",
    "playerState.size": "AHottaPlayerState",
    "abilitySystem.size": "UHTAbilitySystemComponent",
    "abilitySpawnActor.size": "AHTAbilitySpawnActor",
    "dataTable.size": "UDataTable",
}

# Explicit semantic aliases, never inferred from coincident numeric offsets.
PLUGIN_ALIASES = {
    ("UObject", "Class"): ["ObjectClass", "kObjectClassOffset", "L_object_class", "kClassPrivateOffset"],
    ("UObject", "Name"): ["ObjectName", "kObjectNameOffset", "L_object_name", "kNamePrivateOffset"],
    ("UObject", "Outer"): ["ObjectOuter", "kObjectOuterOffset", "L_object_outer"],
    ("UObject", "Index"): ["ObjectIndex", "kObjectIndexOffset", "kObjectInternalIndexOffset", "L_object_internalIndex"],
    ("UStruct", "SuperStruct"): ["Super", "StructSuper", "kStructSuperOffset", "kUStructSuperStructOffset", "L_struct_superStruct"],
    ("UStruct", "Children"): ["kStructChildren", "kUStructChildrenOffset", "L_struct_children"],
    ("UStruct", "ChildProperties"): ["kUStructChildPropertiesOffset", "L_struct_childProperties"],
    ("UStruct", "Size"): ["kStructSize", "awakening.ScriptStructSize"],
    ("UField", "Next"): ["kUFieldNextOffset", "kFieldNext", "L_field_next"],
    ("FField", "Name"): ["FieldName", "PropertyName", "kFieldName", "kPropertyName", "kFFieldNameOffset", "L_field_name"],
    ("FField", "ClassPrivate"): ["kFieldClass", "kPropertyClass", "kFFieldClassOffset", "L_ffield_class"],
    ("FField", "Next"): ["L_ffield_next"],
    ("FProperty", "Offset"): ["PropertyOffset", "kPropertyOffset", "kFPropertyOffsetInternalOffset", "L_property_offsetInternal"],
    ("FProperty", "ElementSize"): ["PropertySize", "kPropertySize", "kPropertyElementSize", "kFPropertyElementSizeOffset", "L_property_elementSize"],
    ("FArrayProperty", "InnerProperty"): ["kFArrayPropertyInnerOffset", "kArrayPropertyInner"],
    ("FStructProperty", "Struct"): ["kFStructPropertyStructOffset", "kStructPropertyStruct"],
    ("UFunction", "FunctionFlags"): ["FunctionFlags", "kUFunctionFlagsOffset", "L_function_flags"],
    ("UFunction", "ExecFunction"): ["FunctionNative", "kFunctionNativeOffset", "kUFunctionFuncOffset", "L_functionNative"],
    ("UDataTable", "RowStruct"): ["RowStruct", "DataTableType", "kDataTableRowStructOffset", "awakening.DataTableRowStruct"],
    ("UDataTable", "RowMap"): ["RowMap", "DataTableMap", "kDataTableRowMapOffset", "awakening.DataTableRowMap"],
    ("UWorld", "OwningGameInstance"): ["WorldGameInstance", "kWorldGameInstanceOffset", "L_world_gameInstance", "awakening.WorldGameInstance"],
    ("UGameInstance", "LocalPlayers"): ["LocalPlayers", "kGameInstanceLocalPlayersOffset", "L_gameInstance_localPlayers", "awakening.GameInstanceLocalPlayers"],
    ("UPlayer", "PlayerController"): ["Controller", "kLocalPlayerControllerOffset", "kPlayerPlayerControllerOffset", "L_localPlayer_controller", "awakening.LocalPlayerController"],
    ("AController", "Pawn"): ["Pawn", "kControllerPawnOffset", "L_controller_pawn"],
    ("AController", "PlayerState"): ["kControllerPlayerStateOffset", "awakening.ControllerPlayerState"],
    ("AActor", "RootComponent"): ["kActorRootComponentOffset", "L_actor_rootComponent"],
    ("ACharacter", "Mesh"): ["kCharacterMeshOffset"],
    ("USceneComponent", "AttachParent"): ["kMeshAttachParentOffset"],
    ("UWidget", "Slot"): ["kWidgetSlotOffset"],
}

PLUGIN_FIELDS = {
    "BetterPose": {
        "kMeshSkeletalMeshOffset": ("USkinnedMeshComponent", "SkeletalMesh"),
        "kMeshMorphTargetsOffset": ("USkeletalMesh", "MorphTargets"),
    },
    "CloneEnter": {
        "kModifiersArrayOffset": ("UGameplayEffect", "Modifiers"),
        "kModifierOpOffset": ("FGameplayModifierInfo", "ModifierOp"),
        "kModifierMagnitudeOffset": ("FGameplayModifierInfo", "ModifierMagnitude"),
        "kAttributeOwnerOffset": ("FGameplayAttribute", "AttributeOwner"),
        "kAttributePropertyOffset": ("FGameplayAttribute", "Attribute"),
        "kMagnitudeTypeOffset": ("FGameplayEffectModifierMagnitude", "MagnitudeCalculationType"),
        "kMagnitudeScalableOffset": ("FGameplayEffectModifierMagnitude", "ScalableFloatMagnitude"),
        "kMagnitudeSetByCallerOffset": ("FGameplayEffectModifierMagnitude", "SetByCallerMagnitude"),
        "kMagnitudeCustomOffset": ("FGameplayEffectModifierMagnitude", "CustomMagnitude"),
        "kMagnitudeAttributeBasedOffset": ("FGameplayEffectModifierMagnitude", "AttributeBasedMagnitude"),
        "kEnumNamesOffset": ("UEnum", "Names"),
    },
    "SkinChanger": {
        "DisplayCharacter": ("AHTPlayerCharacter", "CurrentDisplayCharacterID"),
        "awakening.ActorPlayerState": ("AHTPlayerCharacter", "SavedPlayerState"),
    },
    "HiFiVehicleMusic": {
        "kPawnControllerOffset": ("APawn", "Controller"),
        "kPlayerControllerPlayerOffset": ("APlayerController", "Player"),
    },
}


def check_plugins(root, sdk, candidate):
    checked = 0
    failures = []
    aliases = {key: field for field, keys in PLUGIN_ALIASES.items() for key in keys}
    for path in (root / "plugins").glob("*/profile/*.json"):
        document = json.loads(path.read_text(encoding="utf-8"))
        actual = dict(document["layout"])
        for symbol in document["symbols"]:
            if symbol["name"] == "layout":
                actual.update(symbol["candidates"][candidate if len(symbol["candidates"]) > 1 else 0]["values"])
        fields = aliases | PLUGIN_FIELDS.get(path.parent.parent.name, {})
        for key, value in actual.items():
            semantic = fields.get(key) or (aliases.get(key[7:]) if key.startswith("combat.") else None)
            if not semantic:
                continue
            owner, member = semantic
            expected = sdk[owner]["fields"][member][0]
            checked += 1
            if value != expected:
                failures.append(f"{path.parent.parent.name}.{key}: profile={value}, SDK={expected}")
        if path.parent.parent.name == "BoxAuto":
            checked += 1
            if actual["kInteractEntryStride"] != sdk["FInteractEntry"]["size"]:
                failures.append("BoxAuto interaction entry stride differs from SDK")
        if path.parent.parent.name == "CloneEnter":
            expected = sdk["UGameplayAttributeSet"]["fields"]["DamageUpGeneralBase"][0] + sdk["FGameplayAttributeData"]["fields"]["BaseValue"][0]
            checked += 2
            if actual["kDamageUpGeneralBaseOffset"] != expected or actual["kModifierInfoSize"] != sdk["FGameplayModifierInfo"]["size"]:
                failures.append("CloneEnter attribute base or modifier record size differs from SDK")
    return checked, failures


def expected_core(sdk):
    values = {key: sdk[owner]["fields"][field][0]
              for key, (owner, field) in CORE_FIELDS.items()}
    values.update({key: sdk[owner]["size"] for key, owner in CORE_SIZES.items()})
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=pathlib.Path)
    parser.add_argument("--candidate", type=int, required=True)
    parser.add_argument("--profile", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / "profiles/nte/nte-current.json")
    args = parser.parse_args()
    document = json.loads(args.profile.read_text(encoding="utf-8"))
    actual = document["layout"] | document["layoutCandidates"][args.candidate]["values"]
    failures = []
    sdk = read_sdk(args.sdk)
    expected = expected_core(sdk)
    for key, value in expected.items():
        if actual.get(key) != value:
            failures.append(f"{key}: profile={actual.get(key)}, SDK={value}")
    plugin_count, plugin_failures = check_plugins(pathlib.Path(__file__).resolve().parents[1], sdk, args.candidate)
    failures.extend(plugin_failures)
    if failures:
        raise SystemExit("\n".join(failures))
    print(f"PASS: {len(expected)} core and {plugin_count} plugin SDK layout fields, candidate {args.candidate}")


if __name__ == "__main__":
    main()
