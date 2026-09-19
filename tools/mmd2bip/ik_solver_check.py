"""Independent checks of the MMD IK solver in mmd2bip.py.

There is no MMD here to compare against, so these cases assert what an IK solver must do
and what can be computed by hand:

1. a *reachable* target is reached -- the effector lands on the IK bone;
2. an *unreachable* target leaves the chain straight, pointing at the target, with a
   residual equal to (distance from the chain root) - (sum of the chain's segments). This
   model's own rest pose is in this regime: its IK bone sits 0.168 units beyond the leg's
   reach, so a small residual at rest is the model, not the solver;
3. a bent leg must bend the way the *model's rest pose* already bends: the knee stays on
   the same side of the hip-to-ankle line. A sign error in the knee's angle limit turns the
   leg inside out, and this is what catches it;
4. every limited link stays inside its own PMX range.

Run:  python tools/mmd2bip/ik_solver_check.py [pmx.json]
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmd2bip as ik  # noqa: E402  (path set above)

DEFAULT_PMX = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "..", ".local", "vmd2anim-ref", "pmx1.json")
IK_BONE = "左足ＩＫ"
SEGMENTS = (("左足", "左ひざ"), ("左ひざ", "左足首"))
TOLERANCE = 1e-2      # mmd units; 0.01 unit is 0.8 mm on this model


def build(path):
    doc = json.load(open(path, encoding="utf-8"))
    return ik.MmdIkPose({b["name"]: b for b in doc["bones"] if b["name"]})


def chain_of(pose):
    for chain in pose.chains:
        if pose.name[chain["bone"]] == IK_BONE:
            return chain
    raise SystemExit(f"{IK_BONE} is missing from the model")


def solve(pose, offset):
    def sample(name):
        return ik.ident, (offset if name == IK_BONE else (0.0, 0.0, 0.0))
    pose.set_frame(sample)
    return pose.solve()


def reach(pose):
    """How far the chain can extend, from the rest pose's own segment lengths."""
    return sum(math.dist(pose.rest[pose.name.index(a)], pose.rest[pose.name.index(b)])
               for a, b in SEGMENTS)


def knee_side(pose):
    """Unit vector from the hip->ankle line out to the knee (which way the knee points)."""
    hip = pose.wpos[pose.name.index("左足")]
    knee = pose.wpos[pose.name.index("左ひざ")]
    ankle = pose.wpos[pose.name.index("左足首")]
    axis = tuple(ankle[k] - hip[k] for k in range(3))
    length = math.sqrt(sum(c * c for c in axis))
    along = sum((knee[k] - hip[k]) * axis[k] for k in range(3)) / (length * length)
    out = tuple(knee[k] - hip[k] - along * axis[k] for k in range(3))
    norm = math.sqrt(sum(c * c for c in out))
    return tuple(c / norm for c in out)


def main(argv):
    path = argv[1] if len(argv) > 1 else DEFAULT_PMX
    pose = build(path)
    chain = chain_of(pose)
    reach_length = reach(pose)
    failures = []

    # The model's own rest relation, measured independently of the solver.
    solve(pose, (0.0, 0.0, 0.0))
    hip = pose.wpos[pose.name.index("左足")]
    target = pose.wpos[pose.name.index(IK_BONE)]
    rest_distance = math.dist(hip, target)
    rest_slack = max(0.0, rest_distance - reach_length)
    rest_gap = pose.effector_gap(chain)
    print(f"rest pose: |hip->IK| {rest_distance:.4f}, reach {reach_length:.4f}, "
          f"slack {rest_slack:.4f}, solver residual {rest_gap:.4f}")
    if abs(rest_gap - rest_slack) > TOLERANCE:
        failures.append(f"rest residual {rest_gap:.4f} != slack {rest_slack:.4f}")

    # 1. reachable: move the IK bone up until the leg can certainly reach it.
    for offset in ((0.0, 2.0, 0.0), (0.0, 3.0, 0.0)):
        solve(pose, offset)
        gap = pose.effector_gap(chain)
        print(f"reachable   {str(offset):18s} residual {gap:.6f}")
        if gap > 1e-3:
            failures.append(f"reachable target {offset} left {gap:.6f}")

    # 2. unreachable: residual must equal distance - reach.
    for offset in ((0.0, 0.0, 0.0), (2.15, 0.0, 0.0), (0.0, 60.0, 0.0)):
        solve(pose, offset)
        hip = pose.wpos[pose.name.index("左足")]
        target = pose.wpos[pose.name.index(IK_BONE)]
        expected = max(0.0, math.dist(hip, target) - reach_length)
        gap = pose.effector_gap(chain)
        print(f"unreachable {str(offset):18s} residual {gap:9.4f}  expected "
              f"{expected:9.4f}  (distance {math.dist(hip, target):.3f})")
        if abs(gap - expected) > TOLERANCE:
            failures.append(f"target {offset}: residual {gap:.4f} != {expected:.4f}")

    # 3. bent leg bends the way the rest pose bends.
    solve(pose, (0.0, 0.0, 0.0))
    rest_side = knee_side(pose)
    solve(pose, (0.0, 2.0, 0.0))
    agreement = sum(a * b for a, b in zip(rest_side, knee_side(pose)))
    print(f"knee side rest -> bent: {agreement:+.4f} (1.0 = same side)")
    if agreement < 0.9:
        failures.append(f"knee bent to the wrong side ({agreement:.4f})")

    # 4. every limited link inside its PMX range, in every case above.
    for offset in ((0.0, 0.0, 0.0), (0.0, 2.0, 0.0), (2.15, 0.0, 0.0), (0.0, 60.0, 0.0)):
        solve(pose, offset)
        for link, limit in zip(chain["links"], chain["limits"]):
            if limit is None:
                continue
            angles = ik.quat_euler_xyz(pose.rot[link])
            low, high = limit["limitMin"], limit["limitMax"]
            inside = all(low[k] - 1e-6 <= angles[k] <= high[k] + 1e-6 for k in range(3))
            print(f"limit {pose.name[link]:6s} {str(offset):18s} euler "
                  f"{angles[0]:+.4f} {angles[1]:+.4f} {angles[2]:+.4f}  inside={inside}")
            if not inside:
                failures.append(f"{pose.name[link]} outside its limit for {offset}")

    if failures:
        print("\nFAIL")
        for line in failures:
            print("  -", line)
        return 1
    print(f"\nOK: {len(pose.chains)} chains; reachable targets closed, unreachable "
          f"residual == distance - reach, knee side kept, limits respected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
