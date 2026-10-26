#!/usr/bin/env python3
"""Generate BetterPose's Endfield Standard MMD / A-pose reference table.

The bone layout is derived from Endfield-Poser's StandardRig definition
(AGPL-3.0-only). The output contains positions, hierarchy, and IK metadata;
it does not contain model geometry, textures, motion, or local paths.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


Vec3 = tuple[float, float, float]


def add(a: Vec3, b: Vec3) -> Vec3:
    return a[0] + b[0], a[1] + b[1], a[2] + b[2]


def sub(a: Vec3, b: Vec3) -> Vec3:
    return a[0] - b[0], a[1] - b[1], a[2] - b[2]


def mul(a: Vec3, scale: float) -> Vec3:
    return a[0] * scale, a[1] * scale, a[2] * scale


def dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a: Vec3, b: Vec3) -> Vec3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def norm(a: Vec3) -> Vec3:
    length = math.sqrt(dot(a, a))
    if length <= 1e-12:
        return 0.0, 0.0, 0.0
    return mul(a, 1.0 / length)


class Rig:
    def __init__(self) -> None:
        self.bones: list[dict] = []
        self.indices: dict[str, int] = {}

    def add(self, name: str, parent: str | None, position: Vec3) -> int:
        index = len(self.bones)
        parent_index = self.indices[parent] if parent is not None else -1
        self.bones.append(
            {
                "name": name,
                "index": index,
                "parent": parent_index,
                "position": position,
            }
        )
        self.indices[name] = index
        return index

    def find(self, name: str) -> int:
        return self.indices[name]


def build_reference() -> dict:
    rig = Rig()
    rig.add("全ての親", None, (0.0, 0.0, 0.0))
    rig.add("センター", "全ての親", (0.0, 10.0, 0.0))
    rig.add("グルーブ", "センター", (0.0, 10.0, 0.0))
    rig.add("腰", "グルーブ", (0.0, 10.0, 0.0))
    rig.add("下半身", "腰", (0.0, 10.0, 0.0))
    rig.add("上半身", "腰", (0.0, 11.3, 0.0))
    rig.add("上半身2", "上半身", (0.0, 13.0, 0.0))
    rig.add("上半身3", "上半身2", (0.0, 14.0, 0.0))
    rig.add("首", "上半身3", (0.0, 15.6, 0.0))
    head = rig.add("頭", "首", (0.0, 16.6, 0.0))
    # The eyes are below and in front of the skull. Falling back to "first child" for the
    # head therefore makes its anatomical axis point 45 degrees down-forward, and every
    # StandardRig motion reads as looking at the floor. Give the head the same explicit
    # vertical tail direction the standard Miku references use; the eyes remain children
    # for gaze/rig topology but no longer define the cranial rest axis.
    rig.bones[head]["tailOffset"] = (0.0, 1.0, 0.0)
    rig.add("両目", "頭", (0.0, 17.1, -0.5))

    for side_index, side in enumerate(("左", "右")):
        x = -1.0 if side_index else 1.0
        rig.add(f"{side}目", "両目", (x * 0.35, 17.1, -0.6))
        rig.add(f"{side}肩P", "上半身3", (x * 0.7, 14.9, 0.0))
        rig.add(f"{side}肩", f"{side}肩P", (x * 0.9, 14.9, 0.0))
        rig.add(f"{side}肩C", f"{side}肩", (x * 1.7, 14.8, 0.0))
        rig.add(f"{side}腕", f"{side}肩C", (x * 1.7, 14.8, 0.0))
        rig.add(f"{side}腕捩", f"{side}腕", (x * 2.5, 14.0, 0.0))
        rig.add(f"{side}ひじ", f"{side}腕捩", (x * 3.7, 12.8, 0.0))
        rig.add(f"{side}手捩", f"{side}ひじ", (x * 4.6, 11.9, 0.0))
        rig.add(f"{side}手首", f"{side}手捩", (x * 5.6, 10.9, 0.0))

        for finger_index, finger in enumerate(("親指", "人指", "中指", "薬指", "小指")):
            parent = f"{side}手首"
            for joint in range(3):
                suffix = joint if finger_index == 0 else joint + 1
                name = f"{side}{finger}{suffix}"
                rig.add(
                    name,
                    parent,
                    (
                        x * (5.8 + joint * 0.3),
                        10.7 - joint * 0.3,
                        (finger_index - 2) * 0.17,
                    ),
                )
                parent = name

        rig.add(f"{side}足", "下半身", (x * 0.85, 9.5, 0.0))
        rig.add(f"{side}ひざ", f"{side}足", (x * 0.85, 5.0, -0.15))
        rig.add(f"{side}足首", f"{side}ひざ", (x * 0.85, 1.0, 0.0))
        rig.add(f"{side}つま先", f"{side}足首", (x * 0.85, 0.25, -1.2))
        rig.add(f"{side}足IK親", "全ての親", (x * 0.85, 1.0, 0.0))
        foot = rig.add(f"{side}足IK", f"{side}足IK親", (x * 0.85, 1.0, 0.0))
        rig.bones[foot]["ik"] = {
            "target": rig.find(f"{side}足首"),
            "loop": 64,
            "limitRadian": 0.6,
            "links": [
                {
                    "bone": rig.find(f"{side}ひざ"),
                    "limitMin": [-3.13, 0.0, 0.0],
                    "limitMax": [-0.001, 0.0, 0.0],
                },
                {"bone": rig.find(f"{side}足")},
            ],
        }
        toe = rig.add(
            f"{side}つま先IK",
            f"{side}足IK",
            (x * 0.85, 0.25, -1.2),
        )
        rig.bones[toe]["ik"] = {
            "target": rig.find(f"{side}つま先"),
            "loop": 8,
            "limitRadian": 0.5,
            "links": [{"bone": rig.find(f"{side}足首")}],
        }

    # StandardRig builds an open, out-of-plane thumb from the palm basis after
    # choosing the A/T arm preset.
    thumb = ((0.14, 0.20, -0.16), (0.47, 0.40, -0.30), (0.80, 0.61, -0.44))
    for side_index, side in enumerate(("左", "右")):
        wrist = rig.bones[rig.find(f"{side}手首")]["position"]
        palm = sub(rig.bones[rig.find(f"{side}中指1")]["position"], wrist)
        length = math.sqrt(dot(palm, palm))
        forward = norm(palm)
        radial = sub(
            rig.bones[rig.find(f"{side}人指1")]["position"],
            rig.bones[rig.find(f"{side}小指1")]["position"],
        )
        radial = norm(sub(radial, mul(forward, dot(radial, forward))))
        normal = mul(cross(forward, radial), -1.0 if side_index else 1.0)
        for joint, part in enumerate(thumb):
            position = add(
                wrist,
                mul(
                    add(add(mul(forward, part[0]), mul(radial, part[1])), mul(normal, part[2])),
                    length,
                ),
            )
            rig.bones[rig.find(f"{side}親指{joint}")]["position"] = position

    return {
        "kind": "better-pose-reference-pmx",
        "modelName": "Endfield Standard MMD / A-pose",
        "source": "Endfield-Poser StandardRig",
        "license": "AGPL-3.0-only",
        "boneCount": len(rig.bones),
        "bones": rig.bones,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("plugins/BetterPose/data/reference-endfield-standard-a.json"),
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(build_reference(), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
