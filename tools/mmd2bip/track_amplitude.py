"""Per-track motion amplitude of a shipped motion JSON.

For every track: the largest rotation away from the first frame.  A value near
zero means that bone is effectively frozen for the whole take.
"""

import json
import math
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"D:\pose-motion-zaowu2.json"
doc = json.load(open(path, encoding="utf-8"))
groups = {}


def ang(a, b):
    """Rotation angle between two quaternions, in degrees.

    The dot product of two unit quaternions is cos(angle/2), so the angle is
    twice the arc cosine -- using the arc cosine directly reported every amplitude
    at half its real value. |dot| because q and -q are the same rotation, and the
    inputs are rounded to six decimals so they are normalised first.
    """
    na = math.sqrt(sum(x * x for x in a)) or 1.0
    nb = math.sqrt(sum(x * x for x in b)) or 1.0
    d = abs(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b)) / (na * nb))))
    return math.degrees(2.0 * math.acos(d))


for name, track in doc["bones"].items():
    ref = track[0]
    amp = max(ang(ref, q) for q in track)
    if "Finger" in name:
        key = "finger"
    elif "Twist" in name:
        key = "twist"
    elif name.startswith("Bn_"):
        key = "accessory"
    else:
        key = "body"
    groups.setdefault(key, []).append((amp, name))

for key in ("body", "finger", "twist", "accessory"):
    rows = sorted(groups.get(key, []), reverse=True)
    if not rows:
        continue
    amps = [r[0] for r in rows]
    frozen = [r for r in rows if r[0] < 3.0]
    print(f"--- {key}: n={len(rows)}  mean {sum(amps)/len(amps):6.1f}  "
          f"min {min(amps):6.1f}  frozen(<3 deg) {len(frozen)}")
    for amp, name in rows[:3]:
        print(f"      {amp:6.1f}  {name}")
    for amp, name in frozen[:3]:
        print(f"      frozen {amp:5.1f}  {name}")
