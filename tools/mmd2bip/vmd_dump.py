#!/usr/bin/env python3
"""VMD (Vocaloid Motion Data) reader.

Bone keyframe record = 111 bytes:
  15B Shift-JIS name + u32 frame + 3f position + 4f quaternion(x,y,z,w)
  + 64B bezier interpolation
Header is 30B ("Vocaloid Motion Data file" v1 / "... 0002" v2) + 10B/20B model
name.  After the bone block come morphs, camera, light, self-shadow and (v2
only) the model/IK/visibility frames.

Usage: python vmd_dump.py <motion.vmd> [bones.json]
"""
import json
import struct
import sys

SHIFT_JIS = "shift_jis"


class Reader:
    def __init__(self, data: bytes) -> None:
        self.d = data
        self.p = 0

    def take(self, n: int) -> bytes:
        b = self.d[self.p:self.p + n]
        if len(b) != n:
            raise EOFError(f"read past end at {self.p} (+{n}, have {len(b)})")
        self.p += n
        return b

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]

    def f32s(self, n: int):
        return list(struct.unpack("<%df" % n, self.take(4 * n)))

    def name(self, n: int) -> str:
        raw = self.take(n)
        end = raw.find(b"\x00")
        if end >= 0:
            raw = raw[:end]
        return raw.decode(SHIFT_JIS, errors="replace")


def main() -> int:
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(path, "rb").read()
    r = Reader(data)

    header = r.name(30)
    if header.startswith("Vocaloid Motion Data file"):
        version = 1
    elif "0002" in header:
        version = 2
    else:
        raise SystemExit(f"not a VMD file: header={header!r}")
    model_name = r.name(10 if version == 1 else 20)

    tracks = {}
    order = []
    bone_keys = r.u32()
    for _ in range(bone_keys):
        name = r.name(15)
        frame = r.u32()
        pos = r.f32s(3)
        quat = r.f32s(4)          # x, y, z, w
        interp = r.take(64)
        if name not in tracks:
            tracks[name] = []
            order.append(name)
        tracks[name].append({"frame": frame, "pos": pos, "quat": quat,
                             "interp": interp[:4].hex()})

    morphs = {}
    morph_order = []
    morph_keys = r.u32()
    for _ in range(morph_keys):
        name = r.name(15)
        frame = r.u32()
        value = r.f32()
        if name not in morphs:
            morphs[name] = []
            morph_order.append(name)
        morphs[name].append({"frame": frame, "value": value})

    camera_keys = r.u32()
    r.take(camera_keys * (61 if version == 2 else 32))
    light_keys = r.u32()
    r.take(light_keys * 28)
    shadow_keys = r.u32()
    r.take(shadow_keys * 9)

    visible_frames = 0
    ik_states = {}
    if version == 2 and r.p < len(data):
        visible_frames = r.u32()
        for _ in range(visible_frames):
            frame = r.u32()
            visible = r.u8()
            for _ in range(r.u32()):
                ik_name = r.name(20)
                on = r.u8()
                ik_states.setdefault(ik_name, []).append(
                    {"frame": frame, "visible": visible, "on": on})

    frames = [k["frame"] for t in tracks.values() for k in t]
    morph_frames = [k["frame"] for t in morphs.values() for k in t]

    doc = {
        "file": path,
        "version": version,
        "modelName": model_name,
        "boneKeyCount": bone_keys,
        "boneTrackCount": len(tracks),
        "boneTracks": {name: len(keys) for name, keys in tracks.items()},
        "boneFrameRange": [min(frames), max(frames)] if frames else None,
        "morphKeyCount": morph_keys,
        "morphTracks": {name: len(keys) for name, keys in morphs.items()},
        "morphFrameRange": [min(morph_frames), max(morph_frames)] if morph_frames
        else None,
        "cameraKeys": camera_keys,
        "lightKeys": light_keys,
        "shadowKeys": shadow_keys,
        "visibleFrames": visible_frames,
        "ikStates": {k: len(v) for k, v in ik_states.items()},
        "bytesConsumed": r.p,
        "fileSize": len(data),
        "bones": {name: tracks[name] for name in order},
        "morphs": {name: morphs[name] for name in morph_order},
    }
    print(f"version={version} modelName={model_name!r}")
    print(f"boneKeys={bone_keys} tracks={len(tracks)} "
          f"morphKeys={morph_keys} morphTracks={len(morphs)}")
    if frames:
        print(f"bone frames {min(frames)}..{max(frames)}  "
              f"(= {(max(frames) - min(frames)) / 30.0:.2f}s at 30fps)")
    if morph_frames:
        print(f"morph frames {min(morph_frames)}..{max(morph_frames)}")
    print(f"camera={camera_keys} light={light_keys} shadow={shadow_keys} "
          f"visibleFrames={visible_frames} ikStates={dict(ik_states) and {k: len(v) for k, v in ik_states.items()}}")
    print(f"walk: consumed {r.p} of {len(data)} -> exact={r.p == len(data)}")
    if out:
        json.dump(doc, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print(f"wrote {out}")
    return 0 if r.p == len(data) else 1


if __name__ == "__main__":
    raise SystemExit(main())
