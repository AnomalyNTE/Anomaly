import json
import sys

motion = sys.argv[1] if len(sys.argv) > 1 else r"D:\pose-motion-zaowu.json"
skel = sys.argv[2] if len(sys.argv) > 2 else r"D:\pose.skeleton.json"
d = json.load(open(motion, encoding="utf-8"))
sk = json.load(open(skel, encoding="utf-8"))
names = {b["name"] for b in sk["bones"] if b["name"]}
print("kind:", d["kind"], "fps:", d["fps"], "frames:", d["frameCount"],
      "first:", d["firstFrame"], "root:", d.get("rootBone"))
print("targetMesh:", d.get("targetMesh"), "==", sk.get("mesh"), "->",
      d.get("targetMesh") == sk.get("mesh"))
bad = [k for k, v in d["bones"].items()
       if len(v) != d["frameCount"] or any(len(q) != 4 for q in v)]
print("tracks:", len(d["bones"]), "malformed:", bad[:5], "count", len(bad))
missing = [k for k in d["bones"] if k not in names]
print("tracks missing from the skeleton export:", len(missing), missing[:8])
print("rootTranslation frames:", len(d.get("rootTranslation", [])),
      "all len3:", all(len(v) == 3 for v in d.get("rootTranslation", [])))
