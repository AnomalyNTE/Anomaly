"""How much of a motion file actually lands on a given character?

Counts how many of each file's bone names exist in the current skeleton export, which
is what decides whether a file made for one character can drive another.
"""

import json
import sys

skel_path = r"D:\pose.skeleton.json"
names = {b["name"] for b in json.load(open(skel_path, encoding="utf-8"))["bones"]}
print(f"skeleton: {len(names)} bone names ({skel_path})")

for path in sys.argv[1:]:
    doc = json.load(open(path, encoding="utf-8"))
    tracks = list(doc["bones"])
    hit = [n for n in tracks if n in names]
    miss = [n for n in tracks if n not in names]
    body = [n for n in tracks if n.startswith("Bip001")]
    body_hit = [n for n in body if n in names]
    accessory = [n for n in tracks if n.startswith("Bn_")]
    accessory_hit = [n for n in accessory if n in names]
    print(f"\n{path.split(chr(92))[-1]}")
    print(f"    tracks {len(tracks)}  matched {len(hit)}  unmatched {len(miss)}")
    print(f"    body bones  {len(body_hit)}/{len(body)} matched")
    print(f"    accessories {len(accessory_hit)}/{len(accessory)} matched")
    if miss:
        print(f"    first unmatched: {miss[:8]}")
