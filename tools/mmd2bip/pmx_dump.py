#!/usr/bin/env python3
"""Minimal PMX (2.0/2.1) reader: dumps the bone table plus a full-walk checksum.

The bone table is what a VMD retargeter needs: names, rest positions, parent
links, deform layer, tail targets, fixed/local axes and IK chains with limits.

Every section is walked end to end so `bytesConsumed == fileSize` proves the
whole layout (including the bone block) was decoded correctly.

Usage: python pmx_dump.py <model.pmx> [out.json]
"""
import json
import struct
import sys


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

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def i32(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]

    def f32s(self, n: int):
        return list(struct.unpack("<%df" % n, self.take(4 * n)))

    def idx(self, size: int) -> int:
        if size == 1:
            return struct.unpack("<b", self.take(1))[0]
        if size == 2:
            return struct.unpack("<h", self.take(2))[0]
        if size == 4:
            return struct.unpack("<i", self.take(4))[0]
        raise ValueError(f"bad index size {size}")


def main() -> int:
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(path, "rb").read()
    r = Reader(data)

    magic = r.take(4)
    if magic[:3] != b"PMX":
        raise SystemExit(f"not a PMX file: magic={magic!r}")
    version = r.f32()
    gcount = r.u8()
    g = [r.u8() for _ in range(gcount)]
    encoding, add_uv, vsz, tsz, msz, bsz, mo_sz, rb_sz = g[:8]
    codec = "utf-16-le" if encoding == 0 else "utf-8"

    def text() -> str:
        return r.take(r.i32()).decode(codec, errors="replace")

    def index(size: int) -> int:
        return r.idx(size)

    model_name = text()
    model_name_en = text()
    text()  # comment
    text()  # comment_en

    # --- vertices (walked so the section length is proven) ---
    vertex_count = r.i32()
    weight_types = {}
    for _ in range(vertex_count):
        r.take(12 + 12 + 8 + 16 * add_uv)      # pos, normal, uv, additional uv
        wt = r.u8()
        weight_types[wt] = weight_types.get(wt, 0) + 1
        if wt == 0:      # BDEF1
            r.take(bsz)
        elif wt == 1:    # BDEF2
            r.take(bsz * 2 + 4)
        elif wt == 2:    # BDEF4
            r.take(bsz * 4 + 16)
        elif wt == 3:    # SDEF
            r.take(bsz * 2 + 4 + 36)
        elif wt == 4:    # QDEF
            r.take(bsz * 4 + 16)
        else:
            raise SystemExit(f"unknown weight type {wt} at {r.p}")
        r.take(4)                              # edge scale

    # --- faces: the declared count is the number of *vertex indices* ---
    # (the spec writes "面数" plus a x3 multiplier, but that overruns the file;
    #  the x1 reading lands the following texture list on real filenames).
    face_index_count = r.i32()
    if face_index_count > 0:
        r.take(face_index_count * vsz)

    # --- textures ---
    textures = [text() for _ in range(r.i32())]

    # --- materials ---
    materials = []
    for _ in range(r.i32()):
        m = {"name": text(), "nameEn": text()}
        m["diffuse"] = r.f32s(4)
        m["specular"] = r.f32s(3)
        m["specularity"] = r.f32()
        m["ambient"] = r.f32s(3)
        m["drawFlags"] = r.u8()
        m["edgeColor"] = r.f32s(4)
        m["edgeSize"] = r.f32()
        m["textureIndex"] = index(tsz)
        m["sphereIndex"] = index(tsz)
        m["sphereMode"] = r.u8()
        share_toon = r.u8()
        m["toonIndex"] = r.u8() if share_toon == 0 else index(tsz)
        m["memo"] = text()
        m["surfaceCount"] = r.i32()
        materials.append(m)

    # --- bones ---
    bone_count = r.i32()
    bones = []
    for i in range(bone_count):
        start = r.p
        try:
            b = {"index": i, "name": text(), "nameEn": text()}
            b["position"] = r.f32s(3)
            b["parent"] = index(bsz)
            b["layer"] = r.i32()
            flags = r.u16()
            b["flags"] = flags
            b["rotatable"] = bool(flags & 0x0002)
            b["movable"] = bool(flags & 0x0004)
            b["visible"] = bool(flags & 0x0008)
            b["controllable"] = bool(flags & 0x0010)
            b["ik"] = bool(flags & 0x0020)
            if flags & 0x0001:
                b["tailBone"] = index(bsz)
            else:
                b["tailOffset"] = r.f32s(3)
            if flags & 0x0100:
                # "assign parent" (付与親): bone index + weight factor. Verified
                # against 左目/右目/左肩C in the sample model, where the index
                # points at 両目/左肩P and the weight is 1.0.
                b["assignParent"] = index(bsz)
                b["assignWeight"] = r.f32()
            if flags & 0x0200:
                # Unverified: no bone in the sample model sets this bit.
                b["externalParentKey"] = r.i32()
            if flags & 0x0400:
                # Fixed axis (軸固定): one direction vector, 3 floats. Verified by
                # scoring every candidate payload against bone-name sanity: only
                # 12 bytes keeps all 195 bones decoding to plausible text.
                b["fixedAxis"] = r.f32s(3)
            if flags & 0x0800:
                # Local axes (ローカル軸): two unit vectors (X and Z), 6 floats.
                # Verified against 両目/左目, whose payload after the tail is
                # exactly 24 bytes and whose two vectors are unit and orthogonal.
                b["localAxisX"] = r.f32s(3)
                b["localAxisZ"] = r.f32s(3)
            if flags & 0x1000:  # unverified: absent from the sample model
                b["unknownPayload1000"] = r.f32s(3)
            if flags & 0x2000:  # unverified: absent from the sample model
                b["unknownPayload2000"] = r.f32s(3)
            if flags & 0x4000:  # unverified: absent from the sample model
                b["unknownPayload4000"] = r.f32s(3)
            if flags & 0x0020:
                ik = {"target": index(bsz), "loop": r.i32(), "limitRadian": r.f32(),
                      "links": []}
                for _ in range(r.i32()):
                    link = {"bone": index(bsz)}
                    if r.u8() == 1:
                        link["limitMin"] = r.f32s(3)
                        link["limitMax"] = r.f32s(3)
                    ik["links"].append(link)
                b["ikData"] = ik
        except Exception as exc:
            raise SystemExit(
                f"bone {i} at {start} failed: {exc}\n"
                f"  name={b.get('name')!r} flags={b.get('flags', 0):#06x} "
                f"bytes={data[r.p:r.p + 16].hex(' ')}")
        bones.append(b)

    # --- morphs (walked only) ---
    morph_counts = {}
    morph_count = r.i32()
    for _ in range(morph_count):
        text(); text()
        r.u8()                                          # panel
        kind = r.u8()
        morph_counts[kind] = morph_counts.get(kind, 0) + 1
        for _ in range(r.i32()):
            if kind == 0 or kind == 9:                  # group / flip
                r.take(mo_sz + 4)
            elif kind == 1:                             # vertex
                r.take(vsz + 12)
            elif kind == 2:                             # bone
                r.take(bsz + 28)
            elif 3 <= kind <= 7:                        # uv
                r.take(vsz + 16)
            elif kind == 8:                             # material
                r.take(msz + 1 + 16 + 12 + 4 + 12 + 16 + 4 + 16 + 16 + 16)
            elif kind == 10:                            # impulse
                r.take(rb_sz + 1 + 24)
            else:
                raise SystemExit(f"unknown morph kind {kind} at {r.p}")

    # --- display frames (walked only) ---
    for _ in range(r.i32()):
        text(); text()
        r.u8()
        for _ in range(r.i32()):
            kind = r.u8()
            r.take(bsz if kind == 0 else mo_sz)

    # --- rigid bodies (walked only) ---
    for _ in range(r.i32()):
        text(); text()
        r.take(bsz + 1 + 2 + 1 + 12 + 12 + 12)
        r.take(4 * 5)
        r.u8()

    # --- joints (walked only) ---
    for _ in range(r.i32()):
        text(); text()
        r.u8()
        # Rigid body references use the rigid body index size, not int32.
        r.take(rb_sz * 2 + 12 + 12 + 12 * 4 + 12 * 2)

    soft_bodies = 0
    if version >= 2.1:
        soft_bodies = r.i32()
        for _ in range(soft_bodies):
            text(); text()
            r.u8()
            r.take(msz + 1 + 1 + 1 + 4 * 4)
            r.take(4 * 4)
            r.take(12 * 2)
            for _ in range(r.i32()):
                r.take(rb_sz + vsz + 4)

    doc = {
        "file": path,
        "version": version,
        "encoding": "utf16le" if encoding == 0 else "utf8",
        "additionalUvCount": add_uv,
        "indexSizes": {"vertex": vsz, "texture": tsz, "material": msz,
                       "bone": bsz, "morph": mo_sz, "rigidbody": rb_sz},
        "modelName": model_name,
        "modelNameEn": model_name_en,
        "vertexCount": vertex_count,
        "weightTypes": {str(k): v for k, v in sorted(weight_types.items())},
        "faceIndexCount": face_index_count,
        "triangleCount": face_index_count // 3,
        "textureCount": len(textures),
        "textures": textures,
        "materialCount": len(materials),
        "materials": [m["name"] for m in materials],
        "boneCount": bone_count,
        "morphCount": morph_count,
        "morphKinds": {str(k): v for k, v in sorted(morph_counts.items())},
        "softBodyCount": soft_bodies,
        "bytesConsumed": r.p,
        "fileSize": len(data),
        "walkExact": r.p == len(data),
        "bones": bones,
    }
    js = json.dumps(doc, ensure_ascii=False, indent=1)
    if out:
        open(out, "w", encoding="utf-8").write(js)
    print(f"model={model_name!r} version={version}")
    print(f"vertices={vertex_count} tris={doc['triangleCount']} textures={len(textures)} "
          f"materials={len(materials)} bones={bone_count} morphs={morph_count}")
    print(f"weightTypes={doc['weightTypes']}")
    print(f"walk: consumed {r.p} of {len(data)} bytes -> exact={doc['walkExact']}")
    if out:
        print(f"wrote {out}")
    return 0 if doc["walkExact"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
