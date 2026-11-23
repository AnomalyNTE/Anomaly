#!/usr/bin/env python3
"""Generate plugins/EquipmentCalculator/assets/web/recommended_weights.json.

Fetches the public character weight table from the Yihuan Workshop open API
(referenced by https://github.com/jiabaili-qwq/NTE-score-jiabaili) and writes a
normalized static JSON file to be committed into the repository.

The EquipmentCalculator plugin queries the public API on startup and uses the
committed GitHub rawcontent file and bundled local static copy as fallbacks.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import pathlib
import sys
import urllib.request
from typing import Any

UPSTREAM_API_URL = "https://yh.zzzmap.com/api/open/game-character/weight-configs"
DEFAULT_OUTPUT = pathlib.Path(__file__).resolve().parent / "assets" / "web" / "recommended_weights.json"

GENERAL_WEIGHTS = {
    "AtkUp": 0.7,
    "CritBase": 1,
    "CritDamageBase": 1,
    "DamageUpGeneralBase": 0.75,
}

# Upstream lowercase keys -> canonical game property IDs used by EquipmentCalculator.
# Note: upstream uses "damageupsychebase" (single p) and "chargegetefficiency"
# (without Base); map both to the live DataTable property IDs.
PROPERTY_ID_MAP = {
    "atkadd": "AtkAdd",
    "atkbase": "AtkBase",
    "atkup": "AtkUp",
    "chargegetefficiency": "ChargeGetEfficiencyBase",
    "chargegetefficiencybase": "ChargeGetEfficiencyBase",
    "critbase": "CritBase",
    "critdamagebase": "CritDamageBase",
    "damageupchaosbase": "DamageUpChaosBase",
    "damageupcosmosbase": "DamageUpCosmosBase",
    "damageupgeneralbase": "DamageUpGeneralBase",
    "damageupincantationbase": "DamageUpIncantationBase",
    "damageuplakshanabase": "DamageUpLakshanaBase",
    "damageupnaturebase": "DamageUpNatureBase",
    "damageuppsychicallybase": "DamageUpPsychicallyBase",
    "damageuppsychebase": "DamageUpPsycheBase",
    "damageupsychebase": "DamageUpPsycheBase",
    "defadd": "DefAdd",
    "defbase": "DefBase",
    "defup": "DefUp",
    "healbeup": "HealBeUp",
    "healup": "HealUp",
    "hpmaxadd": "HPMaxAdd",
    "hpmaxbase": "HPMaxBase",
    "hpmaxup": "HPMaxUp",
    "magbase": "MagBase",
    "unbalintensitybase": "UnbalIntensityBase",
}


def normalize_weight(value: Any, *, field: str, character_id: str, key: str) -> int | float:
    if value is None:
        return 0
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"Non-numeric {field} for character {character_id} key {key}: {value!r}")
    numeric = float(value)
    if not math.isfinite(numeric) or numeric < 0.0 or numeric > 1.0:
        raise ValueError(f"Out-of-range {field} for character {character_id} key {key}: {numeric}")
    if numeric == 0.0:
        return 0
    if numeric.is_integer():
        return int(numeric)
    return round(numeric, 6)


def convert_payload(payload: dict[str, Any], *, generated_at: str) -> dict[str, Any]:
    entries = payload.get("data")
    if not isinstance(entries, list) or not entries:
        raise ValueError("Upstream payload has no character data entries")

    characters: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        character_id = str(entry.get("itemId") or "").strip()
        if not character_id:
            continue
        name = str(entry.get("name") or "").strip()
        weight_config = entry.get("weightConfig") or {}
        if isinstance(weight_config, dict):
            raw_weights = weight_config.get("weights") or []
        elif isinstance(weight_config, list):
            raw_weights = weight_config
        else:
            raw_weights = []

        sub_weights: dict[str, int | float] = {}
        main_weights: dict[str, int | float] = {}
        for item in raw_weights:
            if not isinstance(item, dict):
                continue
            raw_key = str(item.get("key") or "").strip().lower()
            if not raw_key:
                continue
            sub_value = normalize_weight(item.get("value"), field="value", character_id=character_id, key=raw_key)
            main_value = normalize_weight(item.get("main_value"), field="main_value", character_id=character_id, key=raw_key)
            if sub_value == 0 and main_value == 0:
                continue
            canonical_key = PROPERTY_ID_MAP.get(raw_key)
            if canonical_key is None:
                raise ValueError(f"Unknown nonzero property key {raw_key!r} for character {character_id} ({name})")
            if sub_value > 0:
                sub_weights[canonical_key] = sub_value
            if main_value > 0:
                main_weights[canonical_key] = main_value

        if not sub_weights:
            continue
        record: dict[str, Any] = {}
        if name:
            record["name"] = name
        record["weights"] = dict(sorted(sub_weights.items()))
        record["main_weights"] = dict(sorted((main_weights or sub_weights).items()))
        characters[character_id] = record

    if not characters:
        raise ValueError("No valid character weight configurations were produced")

    def sort_key(item: tuple[str, dict[str, Any]]) -> tuple[int, int | str]:
        char_id = item[0]
        return (0, int(char_id)) if char_id.isdigit() else (1, char_id)

    sorted_characters = dict(sorted(characters.items(), key=sort_key))
    return {
        "schema_version": 1,
        "source": "yihuan_workshop",
        "attribution": "异环工坊",
        "generated_at": generated_at,
        "general": {
            "weights": dict(sorted(GENERAL_WEIGHTS.items())),
            "main_weights": dict(sorted(GENERAL_WEIGHTS.items())),
        },
        "characters": sorted_characters,
    }


def fetch_payload(url: str, timeout: float) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "Anomaly-EquipmentCalculator-WeightsGenerator/1.0",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        if response.status != 200:
            raise RuntimeError(f"Unexpected HTTP status {response.status} from {url}")
        raw = response.read().decode("utf-8")
    payload = json.loads(raw)
    if not isinstance(payload, dict):
        raise ValueError("Upstream response root is not a JSON object")
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate static recommended weights JSON for EquipmentCalculator.")
    parser.add_argument("--url", default=UPSTREAM_API_URL, help="Upstream API URL to fetch")
    parser.add_argument("--input", type=pathlib.Path, help="Read upstream JSON payload from a local file instead of HTTP")
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT, help="Output static JSON path")
    parser.add_argument("--timeout", type=float, default=15.0, help="HTTP request timeout in seconds")
    args = parser.parse_args(argv)

    if args.input is not None:
        payload = json.loads(args.input.read_text(encoding="utf-8"))
    else:
        payload = fetch_payload(args.url, args.timeout)

    generated_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    document = convert_payload(payload, generated_at=generated_at)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(document['characters'])} character weight presets to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
