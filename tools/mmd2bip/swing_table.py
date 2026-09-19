"""How big is the converter's rest correction for a given skeleton export?

  python swing_table.py <skeleton.json>

Prints, per bone group, the angle the converter has to rotate the *captured*
frame to reach the MMD rest direction.  Small angles mean the capture is close
to the rig's real rest pose (good); large angles mean the capture is a curled /
relaxed pose and the frame's roll is being inherited from that pose (bad).

Only the game skeleton (captured) and the PMX (MMD rest) are used, so this is
independent of the converter's own numbers.
"""

import json
import math
import sys

sys.path.insert(0, ".")
import mmd2bip as M

path = sys.argv[1] if len(sys.argv) > 1 else r"D:\pose.skeleton.json"
sk = json.load(open(path, encoding="utf-8"))["bones"]
# Prefer the engine bind pose when the export carries it, exactly like the converter.
ref = sum(1 for b in sk if "refLocal" in b)
if ref == len(sk) and ref:
    for b in sk:
        b["baseLocal"] = b["refLocal"]
    print("using refLocal (engine reference pose) as the rest source")
else:
    print(f"using baseLocal (captured live pose); refLocal on {ref}/{len(sk)} bones")
skel = {b["name"]: b for b in sk if b["name"]}
pby, pdirs = M.pmx_rest_dirs("pmx1.json")
table = M.build_mapping(set(pby), skel,
                        {n: tuple(b["position"]) for n, b in pby.items()})
mmd_first_child = M.pmx_first_children("pmx1.json")
target_of_src = {m["dir"]: m["target"] for m in table}

bind = [M.qnorm(b["baseLocal"]["rotation"]) for b in sk]
crot = [None] * len(sk)
for b in sk:
    i, p = b["index"], b["parent"]
    crot[i] = bind[i] if (p < 0 or crot[p] is None) else M.qmul(crot[p], bind[i])


def ang(a, b):
    d = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    return math.degrees(math.acos(d))


rows = []
for m in table:
    tgt, src = m["target"], m["dir"]
    if tgt not in skel:
        continue
    u = M.chain_direction(sk, skel, tgt, src, mmd_first_child, target_of_src)
    r = pdirs.get(src)
    if u is None or r is None:
        continue
    i = skel[tgt]["index"]
    rows.append((ang(M.qrot(crot[i], u), M.qrot(M.A_AXIS, r)), tgt, src))

groups = ("Finger", "Hand", "Forearm", "ForeArm", "UpperArm", "Clavicle",
          "Thigh", "Calf", "Foot", "Spine", "Neck", "Head")
print(f"skeleton: {path}  ({len(sk)} bones, {len(rows)} mapped bones)")
for g in groups:
    sel = [r for r in rows if g in r[1]]
    if not sel:
        continue
    mean = sum(r[0] for r in sel) / len(sel)
    worst = max(sel)
    print(f"  {g:<9} n={len(sel):<3} mean {mean:6.1f} deg   max {worst[0]:6.1f} "
          f"({worst[1]} <- {worst[2]})")
big = sorted(rows, reverse=True)[:10]
print("  worst bones:")
for a, t, s in big:
    print(f"    {a:6.1f} deg  {t:<26} <- {s}")
