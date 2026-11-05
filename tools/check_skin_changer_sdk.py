"""Compare Lite layouts with a supplied Dumper-7 CppSDK, without a game process.

This verifies reflected fields and parameter contracts, never native signatures,
unreflected bytes, resource ownership, or in-game appearance results.
"""
import argparse
import json
from pathlib import Path
import re


def definitions(path):
    text = path.read_text(encoding="utf-8-sig")
    result = {}
    for match in re.finditer(
        r"^(?:class|struct) (?:alignas\([^)]*\) )?(\w+)[^;\n]*\n\{(.*?)^\};",
        text, re.MULTILINE | re.DOTALL,
    ):
        fields = {}
        for field in re.finditer(
            r"\b(\w+)(?:\s*:\s*\d+)?;\s*//\s*(0x[\dA-Fa-f]+)\((0x[\dA-Fa-f]+)\)",
            match[2],
        ):
            fields[field[1]] = (int(field[2], 16), int(field[3], 16))
        header = text[max(0, match.start() - 180):match.start()]
        sizes = re.findall(r"// 0x[\dA-Fa-f]+ \((0x[\dA-Fa-f]+) - 0x[\dA-Fa-f]+\)", header)
        result[match[1]] = {"fields": fields, "size": int(sizes[-1], 16) if sizes else None}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="Dumper-7 output folder containing CppSDK")
    parser.add_argument("--profile-root", type=Path, default=Path(__file__).resolve().parents[1] / "plugins/SkinChanger")
    args = parser.parse_args()
    sdk = args.sdk / "CppSDK" / "SDK"
    types = {}
    for name in ("CoreUObject_classes.hpp", "Engine_classes.hpp", "Engine_structs.hpp", "HTGame_classes.hpp", "HTGame_structs.hpp", "Engine_parameters.hpp", "HTGame_parameters.hpp"):
        types.update(definitions(sdk / name))
    profile = json.loads((args.profile_root / "profile/compatibility.profile.json").read_text(encoding="utf-8"))
    candidates = next(symbol["candidates"] for symbol in profile["symbols"] if symbol["name"] == "layout")
    def check_layout(appearance):
        awakening = {key.removeprefix("awakening."): value for key, value in appearance.items() if key.startswith("awakening.")}
        failures = []
        checked = 0

        def expect(label, actual, expected):
            nonlocal checked
            checked += 1
            if actual != expected:
                failures.append(f"{label}: source={actual!r}, SDK={expected!r}")

        def field(type_name, name):
            return types[type_name]["fields"][name]

        awakening_fields = {
            "WorldGameInstance": ("UWorld", "OwningGameInstance"),
            "GameInstanceLocalPlayers": ("UGameInstance", "LocalPlayers"),
            "LocalPlayerController": ("UPlayer", "PlayerController"),
            "ControllerPlayerState": ("AController", "PlayerState"),
            "PlayerStateInventory": ("AHTPlayerState", "InventoryComponent"),
            "InventoryContainers": ("UHTInventoryComponent", "InventoryContainerMap"),
            "ItemId": ("UHTItem", "ItemID"),
            "ItemUniqueId": ("UHTItem", "UniqueID"),
            "ItemAwakenLevel": ("UHTCharacterItem", "AwakenLevel"),
            "ItemAwakenEffects": ("UHTCharacterItem", "AwakenEffectDataLists"),
            "AwakenUiInfo": ("UHTUI_CharacterAwaken", "m_CharacterInfoStruct"),
            "ActorUniqueId": ("AHTPlayerCharacter", "UniqueID"),
            "ActorPlayerState": ("AHTPlayerCharacter", "SavedPlayerState"),
            "ActorFullAwaken": ("AHTPlayerCharacter", "bReplicatedFullAwaken"),
            "ActorAppearance": ("AHTPlayerCharacter", "CurPlayerAppearance"),
            "ActorDisplayedFashion": ("AHTPlayerCharacter", "CurrentDisplayFashionID"),
            "DataTableRowStruct": ("UDataTable", "RowStruct"),
            "DataTableRowMap": ("UDataTable", "RowMap"),
            "ScriptStructSize": ("UStruct", "Size"),
            "AwakenEffectType": ("FAwakenEffectStruct", "AwakenType"),
        }
        for key, (owner, name) in awakening_fields.items():
            expect("awakening." + key, awakening[key], field(owner, name)[0])
        expect("awakening.AwakenUiInfoSize", awakening["AwakenUiInfoSize"], field("UHTUI_CharacterAwaken", "m_CharacterInfoStruct")[1])
        expect("awakening.AwakenUiLevel", awakening["AwakenUiLevel"], field("UHTUI_CharacterAwaken", "m_CharacterInfoStruct")[0] + field("FCharacterInfoStruct", "AwakenLevel")[0])
        expect("awakening.AwakenEffectStride", awakening["AwakenEffectStride"], types["FAwakenEffectStruct"]["size"])
        expect("awakening.EffectSize", awakening["AwakenDataStride"], types["FAwakenEffectData"]["size"])
        for key, owner, name in (
            ("DisplayCharacter", "AHTPlayerCharacter", "CurrentDisplayCharacterID"),
            ("GliderId", "AHTPlayerCharacter", "GlideFashionID"),
            ("GliderActor", "AHTPlayerCharacter", "GlidingActor"),
            ("AppearanceType", "FStaticAppearanceData", "AppearanceType"),
            ("AppearanceCharacter", "FStaticAppearanceData", "CharacterID"),
            ("AppearanceName", "FStaticAppearanceData", "Name"),
            ("AppearanceData", "FStaticAppearanceData", "AppearanceData"),
            ("AppearanceCharacterShow", "FStaticAppearanceData", "IsCharacterShow"),
        ):
            expect("appearance." + key, appearance[key], field(owner, name)[0])
        # Named Profile fields and fixed native parameter ABI contracts.
        contracts = {
            "FDecorationData": {"Part": (appearance["DecorationPart"], 1), "Type": (appearance["DecorationType"], 8), "Name": (appearance["DecorationName"], 16), "MeshDatas": (appearance["DecorationMeshDatas"], 16)},
            "FAwakenEffectData": {"Unlocked": (awakening["AwakenDataUnlocked"], 1), "EffectID": (awakening["AwakenDataId"], 8)},
            "FCharacterAwakenEffectData": {"AwakenEffectStructList": (appearance["AwakenList"], 16)},
            "HTUI_CharacterAwaken_OnInventoryUpdateCharacterCallback": {"NotifyType": (0, 1), "InventoryComponent": (8, 8), "TheUniqueId": (16, 8)},
            "FGlideData": {"ActorClass": (appearance["GlideActorClass"], 40)},
            "FVehicleModuleData": {"ModuleID": (appearance["VehicleRowId"], 8), "VehicleID": (appearance["VehicleRowOwner"], 8), "ModuleName": (appearance["VehicleRowName"], 16), "ModuleType": (appearance["VehicleRowType"], 1)},
            "HTVehicleModificationComponent_UpdateAllModulesWithIDs": {"VehicleModuleIDList": (0, 16), "bForceUpdate": (16, 1), "VehicleID": (20, 8), "SkinID": (28, 8), "DefaultModuleData": (36, 28)},
            "PrimitiveComponent_SetCustomPrimitiveDataFloatArray": {"DataIndex": (0, 4), "Values": (8, 16)},
            "MeshComponent_SetScalarParameterValueOnMaterials": {"ParameterName": (0, 8), "ParameterValue": (8, 4)},
        }
        for owner, fields in contracts.items():
            for name, contract in fields.items():
                expect(owner + "." + name, contract, field(owner, name))
        expect("driving_state.size", appearance["DrivingStateSize"], types["FCharacterVehicleState"]["size"])
        expect("driving_state.vehicle_pointer_size", 8, field("FCharacterVehicleState", "CurrentDrivingVehicle")[1])
        expect("driving_state.seat_size", 4, field("FCharacterVehicleState", "TargetSeatIndex")[1])
        expect("inventory.container_value_size", 160, types["FInventoryData"]["size"])
        expect("inventory.item_map", (0, 80), field("FInventoryData", "InventoryItemsMap"))
        expect("inventory.net_id_size", 8, types["FHTItemNetID"]["size"])
        expect("reflection.function_flags", (appearance["FunctionFlags"], 4), field("UFunction", "FunctionFlags"))
        expect("reflection.native_entry", (appearance["FunctionNative"], 8), field("UFunction", "ExecFunction"))
        expect("awakening.effect_id", awakening["AwakenEffectId"], field("FAwakenEffectStruct", "EffectID")[0])
        expect("scalar.record_size", appearance["ScalarRecordSize"], types["FScalarParameterValue"]["size"])
        expect("scalar.name", appearance["ScalarRecordName"], field("FScalarParameterValue", "ParameterInfo")[0])
        expect("scalar.value", appearance["ScalarRecordValue"], field("FScalarParameterValue", "ParameterValue")[0])
        return failures, checked

    results = [check_layout({**profile["layout"], **candidate["values"]}) for candidate in candidates]
    matches = [index for index, (errors, _) in enumerate(results) if not errors]
    if len(matches) == 1:
        index = matches[0]
        failures, checked = results[index]
        print(f"SDK field contract matches layout candidate {index + 1}; runtime selection still requires its native signature.")
    else:
        failures, checked = min(results, key=lambda result: len(result[0]))
        if not failures:
            failures.append(f"ambiguous SDK layout: {len(matches)} candidates match")
    for failure in failures:
        print("FAIL", failure)
    print(f"Dumper-7 layout contracts: {checked - len(failures)}/{checked} passed.")
    print("Native signatures, call sites, loading state and gameplay remain unverified by this check.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
