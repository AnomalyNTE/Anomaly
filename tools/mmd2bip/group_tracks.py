"""Which bones a motion file drives, and which it leaves with no track at all.

A bone with no track is not driven by the file; the runtime's fallback for undriven
bones is what left pieces pinned in the scene (the tip of every hair chain, an
earring ring). Leaf bones are the usual victims: the accessory spring needs a child
offset to know which way a bone points, so it skips them -- which is why
`--track-untracked 1` (the default) emits a constant track for them instead.

    python group_tracks.py <motion.json> [skeleton.json] [name substring ...]
"""

import json
import re
import sys

motion_path = sys.argv[1]
skel_path = sys.argv[2] if len(sys.argv) > 2 else r"D:\pose.skeleton.json"
want = [w.lower() for w in sys.argv[3:]] or [
    "hair", "tail", "pony", "twin", "braid", "ring", "horn", "ear"]

sk = json.load(open(skel_path, encoding="utf-8"))["bones"]
by = {b["name"]: b for b in sk if b["name"]}
kids = {}
for b in sk:
    kids.setdefault(b["parent"], []).append(b)
motion = json.load(open(motion_path, encoding="utf-8"))["bones"]

groups = {}
for b in sk:
    name = b["name"]
    if not name or not any(w in name.lower() for w in want):
        continue
    groups.setdefault(re.sub(r"_\d+$", "", name), []).append(name)

print(f"motion: {motion_path.split(chr(92))[-1]}   tracks {len(motion)}")
print(f"{'group':<26} {'have track':>10}  untracked bones")
for key in sorted(groups):
    have = [n for n in sorted(groups[key]) if n in motion]
    missing = [n for n in sorted(groups[key]) if n not in motion]
    detail = ", ".join(
        f"{n}{'' if any(c['name'] for c in kids.get(by[n]['index'], [])) else ' (leaf)'}"
        for n in missing[:6])
    print(f"{key:<26} {len(have):>4}/{len(groups[key]):<5} {detail}")

bn = [b["name"] for b in sk if (b["name"] or "").startswith("Bn_")]
untracked = [n for n in bn if n not in motion]
leaves = [n for n in untracked
          if not any(c["name"] for c in kids.get(by[n]["index"], []))]
print(f"\nBn_* bones {len(bn)}, tracked {len(bn) - len(untracked)}, "
      f"untracked {len(untracked)} (of which leaves {len(leaves)})")
if untracked:
    print("first untracked:", untracked[:10])
