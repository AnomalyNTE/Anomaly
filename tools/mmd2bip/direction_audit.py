"""Which child defines each bone's direction, and how big the rest correction is.

This is the check that catches a garment standing in for an anatomy direction.
The shipped rule takes a bone's direction from its anatomical (Bip001-*) child,
and only when there is none, from the segment arriving at the bone; accessory
bones (Bn_*) never define a direction, because their offsets are large and point
wherever the garment hangs -- a hair strand off the head is 14.8 cm long and
points up-forward-right, and using it tilted the head 37 degrees.

Usage: python direction_audit.py [skeleton.json] [pmx.json]
"""

import json
import math
import sys

import mmd2bip as M

skel_path = sys.argv[1] if len(sys.argv) > 1 else r"D:\pose.skeleton.json"
pmx_path = sys.argv[2] if len(sys.argv) > 2 else "pmx1.json"

sk = json.load(open(skel_path, encoding="utf-8"))["bones"]
if all("refLocal" in b for b in sk):
    for b in sk:
        b["baseLocal"] = b["refLocal"]
skel = {b["name"]: b for b in sk if b["name"]}
pby, pdirs = M.pmx_rest_dirs(pmx_path)
pos = {n: tuple(b["position"]) for n, b in pby.items()}
table = M.build_mapping(set(pby), skel, pos)
mmd_mapped = M.pmx_mapped_child_dirs(
    pmx_path, {s for m in table for s in m["sources"]} | {m["dir"] for m in table})
first_child = M.pmx_first_children(pmx_path)
tgt_of = {m["dir"]: m["target"] for m in table}

bind = [M.qnorm(b["baseLocal"]["rotation"]) for b in sk]
C = [None] * len(sk)
for b in sk:
    i, p = b["index"], b["parent"]
    C[i] = bind[i] if (p < 0 or C[p] is None) else M.qmul(C[p], bind[i])


def ang(a, b):
    d = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    return math.degrees(math.acos(d))


def origin(b, src, u):
    """Name the branch the shipped rule took for this bone's direction."""
    kids = [c for c in sk if c["parent"] == b["index"] and c["name"]]
    child_target = tgt_of.get(first_child.get(src))
    if (child_target in skel and skel[child_target]["parent"] == b["index"]
            and u is not None
            and ang(M.unit(skel[child_target]["baseLocal"]["translation"]), u) < 0.5):
        return f"mmd child {child_target}"
    anatomical = [c for c in kids if c["name"].startswith("Bip001")]
    pool = anatomical or [c for c in kids if not c["name"].startswith("Bn_")]
    best, best_len, name = None, 1e-3, ""
    for c in pool:
        t = c["baseLocal"]["translation"]
        n = math.sqrt(sum(x * x for x in t))
        if n > best_len:
            best, best_len, name = t, n, c["name"]
    if best is not None and u is not None and ang(M.unit(best), u) < 0.5:
        return f"child {name}"
    return "own segment (no anatomical child)"


print(f"{'target':<24} {'<-mmd':<10} {'corr':>6}  direction taken from")
flagged = []
for m in sorted(table, key=lambda m: m["target"]):
    name, src = m["target"], m["dir"]
    b = skel.get(name)
    r_m = mmd_mapped.get(src) or pdirs.get(src)
    if b is None or r_m is None:
        continue
    u = M.chain_direction(sk, skel, name, src, first_child, tgt_of)
    if u is None:
        print(f"{name:<24} {src:<10} {'n/a':>6}  none (kept at the reference pose)")
        continue
    corr = ang(M.qrot(C[b["index"]], u), M.qrot(M.A_AXIS, r_m))
    where = origin(b, src, u)
    note = ""
    if corr > 60.0:
        note = "   <-- over 60 deg: suspect the direction reference"
        flagged.append((corr, name, where))
    elif "(no anatomical child)" in where:
        kids = [c["name"] for c in sk
                if c["parent"] == b["index"] and c["name"].startswith("Bn_")]
        note = f"   <-- only accessories hang here ({len(kids)})"
    print(f"{name:<24} {src:<10} {corr:6.1f}  {where}{note}")

print(f"\n{len(table)} mapped bones, {len(flagged)} over the 60 deg watch line")
