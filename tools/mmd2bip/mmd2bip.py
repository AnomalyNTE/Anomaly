#!/usr/bin/env python3
"""MMD motion (VMD + source PMX) -> Bip001 local-rotation tracks, as JSON.

Design (and why it does NOT depend on capturing a neutral pose in game)
----------------------------------------------------------------------
The engine's reference pose (T-pose) is not reachable from the plugin, and a
live capture is whatever the character happened to be doing.  So instead of
treating the capture as "the rest pose", we *construct* a rest pose whose limb
directions are exactly MMD's rest directions:

    C_g(rest) = swing( C_g(capture) . u_g  ->  A . r_g ) . C_g(capture)

where u_g is the direction of the bone's own child offset (constant, read from
the export) and r_g is the MMD bone's rest direction (from the PMX tail).  The
swing is the minimal rotation, so the rig's own *roll* survives from the
capture.  Directions are therefore taken from MMD, and only the roll about each
bone axis is inherited -- which is what the capture is actually good for.

Animation then follows the world-delta rule, per bone, with no FK needed:

    G_local(t) = conj(C_parent(rest), A . Qmmd(t) . A^-1) . R_g(rest)

Because C_g(t) = A . M_mmd(t) . A^-1 . C_g(rest) holds for every bone by
construction, and M_mmd = M_parent . Qmmd.

Axis change A (measured, not assumed; see skel_axis_check.py):
  MMD  left = +X, up = +Y, forward = -Z
  Bip001 left = +X, up = +Z, forward = +Y   =>  A = +90 deg about X.

Bones whose child offset is zero or unknown (root chain, leaf bones) simply
keep the captured roll with no direction correction; that count is reported.
"""
import argparse
import bisect
import json
import math
import sys

# ---------------------------------------------------------------- quaternions
# `ident` is the value main() uses locally too; the IK pose class below needs it at module
# scope because it is defined before main().
ident = (0.0, 0.0, 0.0, 1.0)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty,
            vy + w * ty + z * tx - x * tz,
            vz + w * tz + x * ty - y * tx)


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q))
    if n == 0.0:
        return (0.0, 0.0, 0.0, 1.0)
    return tuple(c / n for c in q)


def qslerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0.0:
        b = tuple(-c for c in b)
        d = -d
    if d > 0.9995:
        return qnorm(tuple(a[i] + (b[i] - a[i]) * t for i in range(4)))
    th0 = math.acos(max(-1.0, min(1.0, d)))
    th = th0 * t
    s0 = math.sin(th0 - th) / math.sin(th0)
    s1 = math.sin(th) / math.sin(th0)
    return qnorm(tuple(a[i] * s0 + b[i] * s1 for i in range(4)))


def qswing(u, v):
    """Minimal rotation taking unit vector u to unit vector v."""
    d = sum(a * b for a, b in zip(u, v))
    if d > 1.0 - 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    if d < -1.0 + 1e-12:
        # antiparallel: any axis perpendicular to u
        axis = (u[1], -u[0], 0.0)
        if sum(c * c for c in axis) < 1e-9:
            axis = (0.0, u[2], -u[1])
        n = math.sqrt(sum(c * c for c in axis))
        return (axis[0] / n, axis[1] / n, axis[2] / n, 0.0)
    c = (u[1] * v[2] - u[2] * v[1],
         u[2] * v[0] - u[0] * v[2],
         u[0] * v[1] - u[1] * v[0])
    return qnorm((c[0], c[1], c[2], 1.0 + d))


def unit(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n > 1e-12 else (0.0, 0.0, 0.0)


def ik_swing(u, v):
    """Minimal rotation taking unit vector u to unit vector v (mirrors motion_builder.cpp's
    Swing; used by the target-side leg IK to re-aim a bone without disturbing its roll)."""
    d = sum(u[k] * v[k] for k in range(3))
    if d > 1.0 - 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    if d < -1.0 + 1e-12:
        axis = (u[1], -u[0], 0.0)
        if sum(c * c for c in axis) < 1e-9:
            axis = (0.0, u[2], -u[1])
        n = unit(axis)
        return (n[0], n[1], n[2], 0.0)
    cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
    return qnorm((cross[0], cross[1], cross[2], 1.0 + d))


A_AXIS = (0.7071067811865476, 0.0, 0.0, 0.7071067811865476)  # MMD -> Bip001


# ------------------------------------------------------------------ mappings
# target bone <- (list of MMD local rotations composed in chain order, MMD dir)
# The MMD local rotation for target bone g is the product of the listed MMD
# bones' local rotations, which lines the two chains up joint for joint.
MAPPING = [
    ("Bip001",           ["センター"],            "センター", True),
    # 髋（下半身）的旋转装在**骨盆**上，而不是大腿上：MMD 里「屁股往后顶」是 `下半身` 旋转、
    # 髋部网格跟着转，而腿被 IK 抵消保持踩地。装到大腿上会让髋网格不动、腿在转（用户实测：
    # 「原片是屁股动，这边变成腿斜了」）。大腿因此只保留 左足/右足，其世界朝向不变
    # （验收：feet_check 的腿段方向仍须 0.00°）。
    ("Bip001-Pelvis",    ["腰", "下半身"],         "腰", False, []),
    # 脊柱不跟 `下半身`（MMD 里 `上半身` 挂在 `腰` 上，两者是兄弟），所以骨盆多带进来的那份
    # `下半身` 必须**左乘**乘回去：q 按链序相乘，`下半身⁻¹` 排在 `上半身` 前面才等于
    # `C(骨盆)⁻¹ · Δ(下半身)⁻¹ · Δ(上半身)`。写成右乘（第 5 项的旧写法）得到的是
    # `Δ(上半身)·Δ(下半身)⁻¹`，只在两者可交换时才等价 —— 骨盆一转髋，躯干就跟着倾。
    ("Bip001-Spine",     ["上半身"],                "上半身", False, [], ["下半身"]),
    # 参考模型的上半身是 上半身/上半身1/上半身2 三节，目标骨架的脊柱也是三节，一一对应。
    # 原来把 上半身2 接到 Spine1、Spine2 完全不驱动，等于丢掉躯干中段。
    ("Bip001-Spine1",    ["上半身1"],               "上半身1", False),
    ("Bip001-Spine2",    ["上半身2"],               "上半身2", False),
    ("Bip001-Neck",      ["首"],                   "首", False),
    ("Bip001-Head",      ["頭"],                   "頭", False),
    ("Bip001-L-Clavicle", ["左肩"],                 "左肩", False),
    ("Bip001-L-UpperArm", ["左腕"],                 "左腕", False),
    ("Bip001-L-Forearm", ["左ひじ"],                "左ひじ", False),
    ("Bip001-L-Hand",    ["左手首"],                "左手首", False),
    ("Bip001-R-Clavicle", ["右肩"],                 "右肩", False),
    ("Bip001-R-UpperArm", ["右腕"],                 "右腕", False),
    ("Bip001-R-Forearm", ["右ひじ"],                "右ひじ", False),
    ("Bip001-R-Hand",    ["右手首"],                "右手首", False),
    # MMD has one more joint in the legs (腰 -> 下半身 -> 足); the target hips
    # attach straight to the pelvis, so 下半身 folds into the thigh.
    ("Bip001-L-Thigh",   ["左足"],                 "左足", False, []),
    ("Bip001-L-Calf",    ["左ひざ"],                "左ひざ", False),
    ("Bip001-L-Foot",    ["左足首"],                "左足首", False),
    ("Bip001-L-Toe0",    ["左つま先"],               "左つま先", False),
    ("Bip001-R-Thigh",   ["右足"],                 "右足", False, []),
    ("Bip001-R-Calf",    ["右ひざ"],                "右ひざ", False),
    ("Bip001-R-Foot",    ["右足首"],                "右足首", False),
    ("Bip001-R-Toe0",    ["右つま先"],               "右つま先", False),
]

FINGER_PREFIX = {"親指": "Finger0", "人指": "Finger1", "中指": "Finger2",
                 "薬指": "Finger3", "小指": "Finger4"}
FINGER_JOINTS = [("0", ""), ("1", "1"), ("2", "2"), ("3", "2")]

# Hair is swing-only for every character, whatever the bone group is called: this rig
# family says "hair" (Bn_*_hair*), a hand-made MMD model may spell it hari/kami/髪.
# The names are matched as substrings of the whole bone name, so "backHairB" and
# "hariB" both qualify. Anything a new character calls something else can still be
# added with --sim-swing-only.
SWING_HAIR_WORDS = ("hair", "hairline", "hari", "kami", "髪", "かみ")

# Head rings (and similar hard ornaments) are kept rigid by bone name, together with
# everything below them: the spring drags them around and they are the one group that
# must not move at all. Only a few characters have one and their names differ, so a table
# beats a keyword rule -- a keyword rule has to guess, and guessing there either freezes
# the hair or lets the ring swing. Entries are substrings of the bone name, so one entry
# can cover a whole group.
HEAD_ORNAMENT_BONES = (
    "Bn_m_headProp",      # 小吱: the ring over the head (one bone)
    "Bn_m_headProA",      # 浔: the floating bell-like pieces on the back of the head
    "Bn_m_headProB",      #     (Bn_m_headProAa/Ab/Ac_001 and Bn_m_headProB_001)
    "Bn_m_headLineA",     # 伊洛伊: the ring, 30 bones in two chains
    "Bn_m_headLineB",
    "hat",                # 九原: the big hat -- matched as a substring, case-insensitively,
                          # so Bn_m_Hat_001 / Hat_01 / whatever the rig calls it is rigid
)


def target_finger_chain(skel, base):
    """The target's finger chain: base, base+"1", base+"2", base+"3" while present."""
    chain = []
    for suffix in ("", "1", "2", "3"):
        name = base + suffix
        if name in skel:
            chain.append(name)
    return chain


def cumulative_fractions(lengths):
    total = sum(lengths)
    if total <= 1e-9:
        return [0.0] * len(lengths)
    out = []
    run = 0.0
    for value in lengths:
        run += value
        out.append(run / total)
    return out


def align_chains(source_fractions, target_fractions):
    """Greedy nearest-fraction alignment, strictly increasing, source -> target."""
    pairs = []
    used = -1
    for frac in source_fractions:
        best, best_gap = -1, None
        for index in range(used + 1, len(target_fractions)):
            gap = abs(target_fractions[index] - frac)
            if best_gap is None or gap < best_gap:
                best, best_gap = index, gap
        if best < 0:
            continue
        pairs.append(best)
        used = best
    return pairs


def build_mapping(mm_names, skel=None, pmx_positions=None):
    """Explicit table plus the finger chain, skipping anything absent.

    MMD bone names use full width digits (左人指１) while the target uses plain
    ones, so names are compared with digits normalised -- without that every
    finger joint silently failed to map and the hands stayed in their captured
    pose even though the motion does carry finger tracks.

    When the skeleton is available the finger chains are aligned by cumulative
    length instead of by index. This rig has FOUR bones per finger
    (Finger1, Finger11, Finger12, Finger13) while MMD has THREE (人指１/２/３), so
    mapping by index left the actual fingertip bone (Finger13) undriven and bent
    the finger from one joint too early -- the whole hand looked wrong no matter
    what the rest pose was. The leading target bone that MMD has no counterpart
    for is emitted as a static track, which holds the rest local rotation and so
    follows the hand rigidly.
    """
    full_width = "０１２３４５６７８９"
    table_names = {name.translate(str.maketrans(full_width, "0123456789"))
                   for name in mm_names}

    def has(name):
        return name.translate(str.maketrans(full_width, "0123456789")) in table_names

    table = []
    for entry in MAPPING:
        target, sources, dir_src, has_pos = entry[:4]
        # 第 5 项可选：需要把父链多带进来的旋转乘回去的目标（见骨盆/脊柱那两条的注释）。
        inverse = entry[4] if len(entry) > 4 else []
        # 第 6 项可选：同样是被乘回去的源，但排在 sources **之前** —— 父链带进来的是左乘，
        # 要抵消它就必须左乘回来（见 Bip001-Spine 的注释）。
        prefix = entry[5] if len(entry) > 5 else []
        # `腰` is missing from a number of exports: a PMD hangs 下半身 straight under センター
        # and has no waist bone at all. The pelvis is then driven by 下半身 with 下半身's own
        # rest direction -- the same fallback motion_builder.cpp carries. Without it this entry
        # is dropped and the pelvis is left rigid, so the whole lower body rides a bone that
        # never turns (58 mapped bones instead of 59, and every leg/twist track drifts).
        if (target == "Bip001-Pelvis" and not has(dir_src) and has("下半身")
                and all(has(s) for s in inverse + prefix)):
            sources = [s for s in sources if has(s)] or ["下半身"]
            dir_src = "下半身"
        if all(has(s) for s in sources + inverse + prefix) and has(dir_src):
            table.append({"target": target, "sources": sources, "dir": dir_src,
                          "position": has_pos, "inverse": inverse, "prefix": prefix})
    for side, side_en in (("左", "L"), ("右", "R")):
        for jp, en in FINGER_PREFIX.items():
            digits = [d for d in "０１２３４" if has(f"{side}{jp}{d}")]
            mm_chain = [f"{side}{jp}{d}" for d in digits[:3]]
            if not mm_chain:
                continue
            base = f"Bip001-{side_en}-{en}"
            if skel is None or pmx_positions is None:
                # Legacy positional mapping, kept for the measurement scripts.
                for position, mm in enumerate(mm_chain):
                    target = base + ("" if position == 0 else str(position))
                    table.append({"target": target, "sources": [mm], "dir": mm,
                                  "position": False})
                continue
            chain = target_finger_chain(skel, base)
            if not chain:
                continue
            target_fractions = cumulative_fractions([
                math.sqrt(sum(c * c for c in skel[n]["baseLocal"]["translation"]))
                for n in chain])
            # MMD measures the same chain from the wrist bone into the last joint.
            mm_positions = [pmx_positions[n] for n in mm_chain]
            source_lengths = []
            hand = pmx_positions.get(f"{side}手首")
            if hand is not None:
                source_lengths.append(math.dist(hand, mm_positions[0]))
            for i in range(len(mm_positions) - 1):
                source_lengths.append(math.dist(mm_positions[i], mm_positions[i + 1]))
            source_fractions = cumulative_fractions(source_lengths)
            # Skip the wrist-to-first-joint length when the target chain starts at
            # the same place (both start at the hand), so the fractions line up.
            pairs = align_chains(source_fractions, target_fractions)
            assigned = {chain[index]: mm for index, mm in zip(pairs, mm_chain)}
            static = [n for n in chain if n not in assigned]
            for target in chain:
                mm = assigned.get(target)
                table.append({"target": target, "sources": [mm] if mm else [],
                              "dir": mm if mm else target,
                              "position": False, "static": mm is None})
    return table



def build_sampler(tracks):
    """Sample a track at an arbitrary frame: hold at the ends, slerp between keys.

    VMD keyframes are sparse -- MMD interpolates between them itself. Reading only
    exact frames and treating a miss as the identity quaternion made every
    undersampled track jump between its pose and identity, which shows up as
    twitching (the finger tracks are the most visible case). The offline verifier
    used the same shortcut, so it never caught this: a check that shares the
    implementation's assumptions cannot falsify it.
    """
    keyed = {}
    for name, keys in tracks.items():
        frames = sorted(keys)
        if frames:
            keyed[name] = (frames, keys)

    def sample(name, frame):
        entry = keyed.get(name)
        if entry is None:
            return None
        frames, keys = entry
        index = bisect.bisect_right(frames, frame) - 1
        if index < 0:
            index = 0
        if index >= len(frames) - 1:
            return keys[frames[index]]
        first, second = frames[index], frames[index + 1]
        if second == first:
            return keys[first]
        blend = (frame - first) / float(second - first)
        q0, p0 = keys[first]
        q1, p1 = keys[second]
        position = tuple(p0[k] + (p1[k] - p0[k]) * blend for k in range(3))
        return (qslerp(qnorm(q0), qnorm(q1), blend), position)

    return sample


def q_from_axis_angle(axis, angle):
    half = angle * 0.5
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def scale_quat_angle(q, gain):
    """The same rotation axis with the angle multiplied by gain (0 gives identity).

    Used to scale how far a spring-driven accessory leaves its rigid pose. Blending
    the *direction* instead degenerates whenever the lagged direction is close to
    opposite the rigid one -- two nearly antiparallel directions have a near-zero
    blend -- which is how a front strand kept 87 degrees of swing through a knob that
    was supposed to halve it.
    """
    w = q[3]
    if w < 0.0:                       # q and -q are one rotation; pick w >= 0 first
        q = (-q[0], -q[1], -q[2], -w)
        w = -w
    angle = 2.0 * math.acos(max(-1.0, min(1.0, w)))
    if angle < 1e-9:
        return (0.0, 0.0, 0.0, 1.0)
    return q_from_axis_angle(unit((q[0], q[1], q[2])), angle * gain)


def build_twist_list(skel, mapped_targets, axis_of):
    """Target twist helper bones and how much of the limb's twist they take.

    These bones exist to spread a joint's twist across the mesh, and the target
    skeleton has sixteen of them. Leaving them at the captured pose is what makes
    elbow and knee deformation look wrong once the limb rotates a lot.
    """
    import re
    pattern = re.compile(
        r"^(Bone|Bip001)-(L|R)-(UpperArm|ForeArm|Thigh|Calf)-Twist(1?)$")
    twist = []
    for bone in skel.values():
        match = pattern.match(bone["name"])
        if match is None:
            continue
        side = match.group(2)
        limb = {"UpperArm": "腕", "ForeArm": "ひじ",
                "Thigh": "足", "Calf": "ひざ"}[match.group(3)]
        source = f"{'左' if side == 'L' else '右'}{limb}"
        arm_target = None
        for mmd_name, target_name in mapped_targets.items():
            if mmd_name == source:
                arm_target = target_name
                break
        if arm_target is None or arm_target not in skel:
            continue
        axis = axis_of(arm_target)
        if axis is None:
            continue
        twist.append({"target": bone["name"], "index": bone["index"],
                      "parent": bone["parent"],
                      "arm_index": skel[arm_target]["index"], "axis": axis,
                      # Twist takes half of the limb twist, Twist1 a quarter, so
                      # the two of them spread three quarters along the limb.
                      "weight": 0.25 if match.group(4) == "1" else 0.5})
    twist.sort(key=lambda item: item["index"])
    return twist


# -------------------------------------------------------------------- loading
def load_skeleton(path):
    doc = json.load(open(path, encoding="utf-8"))
    bones = doc["bones"]
    for b in bones:                     # parent before child
        if b["parent"] >= 0 and b["parent"] >= b["index"]:
            raise SystemExit(f"skeleton not topologically ordered at {b['name']}")
    return doc, {b["name"]: b for b in bones if b["name"]}


def compute_component(bones):
    crot = [None] * len(bones)
    for b in bones:
        i, p = b["index"], b["parent"]
        lr = b["baseLocal"]["rotation"]
        crot[i] = lr if (p < 0 or crot[p] is None) else qmul(crot[p], lr)
    return crot


def bone_dir_local(bones, by_name, name):
    """Direction of a bone's own child offset, in the bone's local frame."""
    b = by_name.get(name)
    if b is None:
        return None
    kids = [c for c in bones if c["parent"] == b["index"]]

    def leaf_direction():
        # Leaf bone: no child offset to read, so assume it continues the segment
        # that arrives at it (its own offset from its parent). Expressed in the
        # leaf's own frame that is just its captured local rotation applied to the
        # incoming offset, which is how a fingertip gets a usable axis.
        translation = b["baseLocal"]["translation"]
        length = math.sqrt(sum(x * x for x in translation))
        if length < 1e-3:
            return None
        incoming = unit(translation)
        return qrot(qconj(qnorm(b["baseLocal"]["rotation"])), incoming)

    if not kids:
        return leaf_direction()
    mapped_kids = [c for c in kids if c["name"] in by_name]
    # Prefer the anatomical child. Characters in this game hang accessory and
    # cloth bones (Bn_*) off the spine and pelvis with *larger* offsets than the
    # real next bone, so "biggest offset wins" silently used a bag strap as the
    # spine's direction and rotated the whole torso. Every anatomical bone in
    # this rig family is named Bip001-*, so restrict the search to those first.
    anatomical = [c for c in kids if c["name"].startswith("Bip001")]
    # A bone whose only children are accessories has no anatomical continuation at
    # all, and then the biggest-offset rule picks a garment: Bip001-Head carries
    # nothing but hair, hat and earring bones (the longest is a 14.8 cm hair strand
    # pointing up-forward-right), and taking that as the head's direction swung the
    # head 37 degrees to the upper right -- the neck looked permanently tilted.  When
    # there is no anatomical child, continue the segment that arrives at the bone
    # instead, exactly as a leaf does.
    continuation = anatomical or [c for c in mapped_kids
                                  if not c["name"].startswith("Bn_")]
    best = None
    best_len = 0.0
    for c in continuation:
        t = c["baseLocal"]["translation"]
        n = math.sqrt(sum(x * x for x in t))
        if n > best_len:
            best, best_len = t, n
    if best is None or best_len < 1e-3:
        return leaf_direction()
    return unit(best)


def ang_between(a, b):
    """Angle between two vectors in degrees (0 when either is degenerate)."""
    na = math.sqrt(sum(c * c for c in a))
    nb = math.sqrt(sum(c * c for c in b))
    if na < 1e-9 or nb < 1e-9:
        return 0.0
    d = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b)) / (na * nb)))
    return math.degrees(math.acos(d))


# ------------------------------------------------------------------------ MMD IK
def quat_matrix(q):
    """3x3 rotation matrix, rows, from a quaternion (x, y, z, w)."""
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return ((1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)),
            (2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)),
            (2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)))


def quat_euler_xyz(q):
    """Euler angles of R = Rx * Ry * Rz (MMD's limit axes are a local XYZ triple)."""
    m = quat_matrix(q)
    sy = max(-1.0, min(1.0, m[0][2]))
    if abs(sy) > 0.999999:
        return (math.atan2(m[1][0], m[1][1]), math.asin(sy), 0.0)
    return (math.atan2(-m[1][2], m[2][2]), math.asin(sy), math.atan2(-m[0][1], m[0][0]))


def euler_xyz_quat(rx, ry, rz):
    return qmul(qmul(q_from_axis_angle((1.0, 0.0, 0.0), rx),
                     q_from_axis_angle((0.0, 1.0, 0.0), ry)),
                q_from_axis_angle((0.0, 0.0, 1.0), rz))


def quat_angle(a, b):
    """Angle between two rotations in degrees (both are normalised first)."""
    na = math.sqrt(sum(c * c for c in a))
    nb = math.sqrt(sum(c * c for c in b))
    if na < 1e-12 or nb < 1e-12:
        return 0.0
    d = abs(sum(x * y for x, y in zip(a, b))) / (na * nb)
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, d))))


def clamp_limits(q, low, high):
    """Clamp a local rotation to a PMX IK link's per-axis limits.

    The knee's limits are `[-pi, 0, 0] .. [0, 0, 0]`: exactly one degree of freedom, which
    is what stops a leg IK from bending the knee forwards.
    """
    rx, ry, rz = quat_euler_xyz(q)
    return euler_xyz_quat(max(low[0], min(high[0], rx)),
                          max(low[1], min(high[1], ry)),
                          max(low[2], min(high[2], rz)))


class MmdIkPose:
    """The source model's own skeleton, posed per frame, with MMD's CCD solve on top.

    A VMD carries rotations for most bones but only *positions* for the IK bones: a motion
    whose legs are IK-driven has empty rotations on the leg bones (``主角.vmd``: nine
    identity keys on 左足/左ひざ/左つま先, thirty moved frames on 左足ＩＫ), so without a solver
    the legs stay in the rest pose and never follow the body.

    Bone transform, mmd_tools' convention: the animation translation is applied in the
    bone's own rest frame,
        world = parent_world * T(rest_offset) * T(vmd_position) * R(vmd_rotation)
    and the rest offset is (position - parent.position) in MMD (Y-up) coordinates.
    """

    def __init__(self, pmx_bones):
        self.bones = sorted(pmx_bones.values(), key=lambda b: b["index"])
        self.name = [b["name"] for b in self.bones]
        self.parent = [b["parent"] for b in self.bones]
        self.rest = [tuple(b["position"]) for b in self.bones]
        self.count = len(self.bones)
        children = {}
        for bone in self.bones:
            if bone["parent"] >= 0:
                children.setdefault(bone["parent"], []).append(bone["index"])
        self.subtrees = {}
        for bone in self.bones:
            index = bone["index"]
            stack, order = [index], []
            while stack:
                current = stack.pop()
                order.append(current)
                stack.extend(children.get(current, ()))
            order.sort()                       # parents before children
            self.subtrees[index] = order
        self.rot = [ident] * self.count
        self.pos = [(0.0, 0.0, 0.0)] * self.count
        self.wrot = [ident] * self.count
        self.wpos = [(0.0, 0.0, 0.0)] * self.count
        self.chains = []
        for bone in self.bones:
            if not bone.get("ik"):
                continue
            data = bone["ikData"]
            self.chains.append({
                "bone": bone["index"], "target": data["target"],
                "loop": max(0, int(data["loop"])),
                "limit": float(data["limitRadian"]),
                "links": [link["bone"] for link in data["links"]],
                "limits": [link if "limitMin" in link else None
                           for link in data["links"]]})

    def set_frame(self, sample):
        """Pose the whole skeleton from the motion (sample(name) -> (quat, pos) | None)."""
        for index, name in enumerate(self.name):
            value = sample(name)
            if value is None:
                self.rot[index] = ident
                self.pos[index] = (0.0, 0.0, 0.0)
            else:
                self.rot[index] = qnorm(value[0])
                self.pos[index] = tuple(value[1])
        self.refresh(range(self.count))

    def refresh(self, indices):
        """Recompute world transforms for the given bones (parents before children)."""
        for index in indices:
            parent = self.parent[index]
            if parent < 0:
                prot, ppos = ident, (0.0, 0.0, 0.0)
                base = self.rest[index]
            else:
                prot, ppos = self.wrot[parent], self.wpos[parent]
                p = self.rest[parent]
                base = tuple(self.rest[index][k] - p[k] for k in range(3))
            self.wrot[index] = qmul(prot, self.rot[index])
            moved = qrot(prot, self.pos[index])
            offset = qrot(prot, base)
            self.wpos[index] = tuple(ppos[k] + offset[k] + moved[k] for k in range(3))

    def update(self, root):
        self.refresh(self.subtrees[root])

    def chain_translation(self, chain):
        """World displacement the chain's own *translations* contribute.

        The target's root link absorbs the whole MMD root chain (全ての親 -> センター ->
        グルーブ -> 腰, and 下半身 when a motion drives its travel), so every layer's
        translation has to end up in the emitted root translation. Only the translation part
        counts here: the rotation the chain above already carries is reproduced by the mapped
        rotation of the root bone, and adding it again would double-count it. Each bone's
        translation is applied in its parent's frame (`world = parent * T(rest) * T(pos) *
        R(rot)`), so the displacement is the parent's world rotation applied to it, and the
        rotations of the bones below leave that accumulated vector alone.
        """
        total = (0.0, 0.0, 0.0)
        for index in chain:
            parent = self.parent[index]
            frame = self.wrot[parent] if parent >= 0 else ident
            moved = qrot(frame, self.pos[index])
            total = tuple(total[k] + moved[k] for k in range(3))
        return total

    def effector_gap(self, chain):
        return math.dist(self.wpos[chain["target"]], self.wpos[chain["bone"]])

    def solve(self, disabled=frozenset()):
        """Run every enabled chain, in bone order, MMD's way.

        One iteration walks the chain from the bone closest to the effector outwards and
        rotates each link so the effector moves towards the IK bone, capping the step at
        `limitRadian` and clamping a limited link to its own per-axis range.
        """
        gaps = []
        for chain in self.chains:
            if self.name[chain["bone"]] in disabled:
                continue
            before, after, applied = self.apply_chain(chain)
            gaps.append((self.name[chain["bone"]], before, after, applied))
        return gaps

    def delta(self, chain):
        """How far the chain's own bones were rotated in total (degrees)."""
        worst = 0.0
        for link in chain["links"]:
            w = max(-1.0, min(1.0, abs(self.rot[link][3])))
            worst = max(worst, math.degrees(2.0 * math.acos(w)))
        return worst

    def apply_chain(self, chain):
        """Solve one chain and report how much of it was the solver's own doing."""
        before = [self.rot[link] for link in chain["links"]]
        gap_before = self.effector_gap(chain)
        self.solve_chain(chain)
        applied = max((quat_angle(before[i], self.rot[link])
                       for i, link in enumerate(chain["links"])), default=0.0)
        return gap_before, self.effector_gap(chain), applied

    def solve_chain(self, chain):
        target, ik_index = chain["target"], chain["bone"]
        for _ in range(chain["loop"]):
            moved = False
            for link, limit in zip(chain["links"], chain["limits"]):
                link_pos = self.wpos[link]
                effector = self.wpos[target]
                ik_pos = self.wpos[ik_index]
                a = tuple(effector[k] - link_pos[k] for k in range(3))
                b = tuple(ik_pos[k] - link_pos[k] for k in range(3))
                la, lb = math.sqrt(sum(c * c for c in a)), math.sqrt(sum(c * c for c in b))
                if la < 1e-9 or lb < 1e-9:
                    continue
                cross = (a[1] * b[2] - a[2] * b[1],
                         a[2] * b[0] - a[0] * b[2],
                         a[0] * b[1] - a[1] * b[0])
                sin = math.sqrt(sum(c * c for c in cross)) / (la * lb)
                cos = sum(a[k] * b[k] for k in range(3)) / (la * lb)
                angle = math.atan2(sin, max(-1.0, min(1.0, cos)))
                if angle < 1e-9 or sin < 1e-12:
                    continue
                step = min(angle, abs(chain["limit"]))
                axis = tuple(c / (sin * la * lb) for c in cross)
                world = q_from_axis_angle(axis, step)
                parent = self.parent[link]
                frame = self.wrot[parent] if parent >= 0 else ident
                local = qnorm(qmul(qconj(frame), qmul(world, self.wrot[link])))
                if limit is not None:
                    local = clamp_limits(local, limit["limitMin"], limit["limitMax"])
                self.rot[link] = local
                self.update(link)
                moved = True
            if not moved:
                break


def chain_direction(sk, skel, target_name, mmd_name, mmd_first_child, target_of_src):
    """Direction of the child that continues *this* chain.

    "Longest child offset wins" holds for most bones but not where a branch is
    longer than the chain: the pelvis continues into the spine (6.8 cm) while a
    thigh offset is longer (7.3 cm), so the pelvis used to point down a leg and the
    whole torso rest was 90 degrees off. The MMD model says which child continues
    the chain and the mapping says which target bone that is, so ask that first and
    only fall back to the offset heuristic.
    """
    child_target = target_of_src.get(mmd_first_child.get(mmd_name))
    if child_target is not None and child_target in skel:
        if skel[child_target]["parent"] == skel[target_name]["index"]:
            return unit(skel[child_target]["baseLocal"]["translation"])
    return bone_dir_local(sk, skel, target_name)


def pmx_first_children(pmx_path):
    """name -> the bone that continues the chain (first child in PMX index order)."""
    doc = json.load(open(pmx_path, encoding="utf-8"))
    bones = doc["bones"]
    out = {}
    for b in bones:
        if b["parent"] >= 0:
            parent = bones[b["parent"]]["name"]
            if parent and parent not in out:
                out[parent] = b["name"]
    return out


def pmx_mapped_child_dirs(pmx_path, mapped_names):
    """name -> direction towards the child that is itself mapped.

    Preferred over the PMX tail for the MMD side. The tail is authoritative for
    most bones, but a few use it as a marker rather than as their direction: the
    waist (腰) carries a forward marker, and taking it at face value made the
    converter rotate the character's whole pelvis rest frame by 67 degrees, which
    arched the spine backwards and lost height. Restricting "the chain child" to
    children that are actually mapped skips the eye and hair bones that hang off
    the head and still picks the real continuation where one exists (腰 -> 上半身).
    """
    doc = json.load(open(pmx_path, encoding="utf-8"))
    bones = doc["bones"]
    positions = {b["name"]: tuple(b["position"]) for b in bones}
    out = {}
    for b in bones:
        for child in bones:
            if child.get("parent") != b["index"]:
                continue
            if child["name"] not in mapped_names:
                continue
            v = tuple(positions[child["name"]][k] - positions[b["name"]][k]
                      for k in range(3))
            if sum(c * c for c in v) > 1e-12:
                out[b["name"]] = unit(v)
            break
    return out


def pmx_rest_dirs(pmx_path):
    doc = json.load(open(pmx_path, encoding="utf-8"))
    bones = doc["bones"]
    by_name = {b["name"]: b for b in bones if b["name"]}
    children = {}
    for b in bones:
        if b["name"] and b["parent"] >= 0:
            parent = bones[b["parent"]]["name"]
            if parent:
                children.setdefault(parent, []).append(b["name"])
    dirs = {}
    for name, b in by_name.items():
        pos = b["position"]
        tip = None
        if "tailBone" in b and b["tailBone"] >= 0 and b["tailBone"] < len(bones):
            tip = bones[b["tailBone"]]["position"]
        elif "tailOffset" in b:
            off = b["tailOffset"]
            if sum(c * c for c in off) > 1e-9:
                tip = [pos[k] + off[k] for k in range(3)]
        if tip is None:
            # No tail: take the child that continues the chain. MMD finger (and
            # toe) bones usually have none, and losing the direction silently
            # demoted them to "captured pose + delta" -- which is why every finger
            # twisted by its own captured offset instead of following the motion.
            for child in children.get(name, []):
                child_position = by_name[child]["position"]
                step = unit([child_position[k] - pos[k] for k in range(3)])
                if sum(c * c for c in step) > 0.5:
                    tip = child_position
                    break
        if tip is None and b["parent"] >= 0:
            # Leaf bone (fingertip, toe): no child and no tail, so it has no axis
            # of its own -- assume it continues the segment that arrives at it,
            # which is what a last phalanx does in a straight chain.
            parent_position = bones[b["parent"]]["position"]
            step = unit([pos[k] - parent_position[k] for k in range(3)])
            if sum(c * c for c in step) > 0.5:
                dirs[name] = step
                continue
        if tip is not None:
            d = unit([tip[k] - pos[k] for k in range(3)])
            if sum(c * c for c in d) > 0.5:
                dirs[name] = d
    return by_name, dirs


def vmd_tracks(path):
    doc = json.load(open(path, encoding="utf-8"))
    doc["tracks"] = {}
    return doc


def vmd_track_dict(path):
    """Frame list -> dict name -> {frame: (quat, pos)} (sampled VMD layout)."""
    doc = json.load(open(path, encoding="utf-8"))
    out = {}
    for name, keys in doc["bones"].items():
        q = {}
        for k in keys:
            q[int(k["frame"])] = (tuple(k["quat"]), tuple(k["pos"]))
        out[name] = q
    return out, doc


def sample(track, frame):
    """Value at an integer frame; keys are sparse but here every frame exists."""
    v = track.get(frame)
    if v is not None:
        return v
    return None


# ----------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pmx", required=True)
    ap.add_argument("--vmd", required=True)
    ap.add_argument("--vmd-json", required=True, help="vmd_dump.py output")
    ap.add_argument("--skeleton", required=True, help="BetterPose skeleton export")
    ap.add_argument("--out", required=True)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--sim-secondary", type=int, default=1,
                    help="1 = add spring-driven secondary motion to Bn_* bones")
    ap.add_argument("--sim-stiffness", type=float, default=200.0)
    ap.add_argument("--sim-damping", type=float, default=16.0)
    ap.add_argument("--sim-gravity", type=float, default=0.0,
                    help="cm/s^2 pulling the tip towards -Z in mesh space")
    ap.add_argument("--sim-max-lag", type=float, default=25.0,
                    help="hard clamp on the secondary lag, in degrees")
    ap.add_argument("--no-direction", default="",
                    help="comma separated name substrings whose direction correction "
                         "is skipped, so they keep the engine reference pose (used to "
                         "test whether a correction is what spreads a thumb)")
    ap.add_argument("--hand-rest", choices=("live", "bind"), default="live",
                    help="which pose supplies the rest ROLL for the hand chain. The "
                         "reference pose is right for the body and the accessories, but "
                         "on this character it turns the hand into a flat wrench: the "
                         "files whose hands looked right used the captured pose there. "
                         "Default: captured pose for the hand chain, reference pose "
                         "everywhere else")
    ap.add_argument("--rest-source", choices=("bind", "live"), default="bind",
                    help="bind = use the engine reference pose from the export's "
                         "refLocal (current); live = use the captured pose in "
                         "baseLocal, which is what the files built before the "
                         "reference-pose reader used")
    ap.add_argument("--hand-plane", type=int, default=0,
                    help="1 = twist each hand so the finger fan matches MMD's palm "
                         "plane (measured, but on the strength of a measurement script "
                         "that later disagreed with itself); 0 = leave the hand's roll "
                         "as the reference pose gives it")
    ap.add_argument("--sim-drape", type=float, default=0.15,
                    help="0 = accessories stay rigid with the body; 1 = they hang "
                         "straight down. In between blends the spring's rest target "
                         "towards a hanging direction, which is what makes a skirt "
                         "hem or a ribbon swing instead of staying stiff")
    ap.add_argument("--sim-inertia", type=float, default=0.5,
                    help="scale of the parent's acceleration added to gravity when "
                         "computing that hanging direction")
    ap.add_argument("--finger-mapping", choices=("aligned", "positional"),
                    default="aligned",
                    help="aligned = chains matched by cumulative length and the tips "
                         "driven (current); positional = the older index mapping that "
                         "drove Finger1/Finger11/Finger12 and left the tip bone alone. "
                         "Kept so the two can be compared on one character")
    ap.add_argument("--finger-roll", choices=("own", "hand", "world"),
                    default="hand",
                    help="which frame supplies the finger rest roll: the finger's own "
                         "captured frame (own), the hand's frame (hand), or the "
                         "MMD-aligned world frame (world). Currently unreachable: the "
                         "hand chain is exempt from the direction correction (the "
                         "finger/hand/foot/toe branch returns first), so all three "
                         "values produce identical output -- verified byte for byte. "
                         "It only speaks again if that exemption is ever removed")
    ap.add_argument("--finger-twist", type=float, default=0.0,
                    help="extra twist, in degrees, applied to every finger rest "
                         "frame about that finger's own axis")
    ap.add_argument("--twist", type=int, default=1,
                    help="0 = do not spread the limb twist onto the Twist bones")
    ap.add_argument("--feet-anchor", type=int, default=0,
                    help="1 = anchor the character's placement to the *feet* (the mean of the "
                         "two ankles' world positions) instead of the pelvis. MMD pins the feet "
                         "through the leg IK and moves the body over them, so the feet read as "
                         "the fixed point; anchoring to the pelvis makes the whole body -- feet "
                         "included -- travel instead. Default 0 keeps the pelvis anchor.")
    ap.add_argument("--ik", type=int, default=1,
                    help="1 (default) = solve the source model's IK chains (left/right "
                         "leg and toe on this rig) before mapping, so a motion that only "
                         "carries IK-bone positions still moves the legs. 0 = skip the "
                         "solver, which reproduces the pre-IK output exactly")
    ap.add_argument("--track-untracked", type=int, default=1,
                    help="1 (default) = every bone the file would otherwise leave "
                         "undriven gets a constant track holding its reference local "
                         "rotation, so it follows its parent rigidly. 0 = the old "
                         "behaviour, which left leaf accessory bones (hair tips, an "
                         "earring ring) with no track at all and they showed up pinned "
                         "in the scene")
    ap.add_argument("--no-sim", default="",
                    help="comma separated name substrings: a matching accessory bone "
                         "and everything below it are kept rigid (no spring) instead of "
                         "being simulated. For ornaments that must not swing with the "
                         "cloth, e.g. an earring ring hanging at the end of an ear chain")
    ap.add_argument("--no-sim-names", default="",
                    help="exact bone names to keep rigid, the name not a substring: for "
                         "head rings and other hard ornaments, where a character only has "
                         "a couple of them and a keyword rule would catch the wrong "
                         "bones. The built-in list (HEAD_ORNAMENT_BONES) is used unless "
                         "--no-sim-builtin 0 is passed")
    ap.add_argument("--no-sim-builtin", type=int, default=1,
                    help="1 (default) = apply the built-in head-ornament name table; 0 = "
                         "skip it, so only --no-sim / --no-sim-names decide")
    ap.add_argument("--sim-scale", default="",
                    help="comma separated name-substring=gain pairs, gain 0..1: that "
                         "accessory's swing is scaled back towards its rigid pose "
                         "(1 = unchanged, 0.5 = half, 0 = rigid). Independently of "
                         "--sim-max-lag, which only limits the lag against the spring's "
                         "own target; this one is measured against the rigid pose, so a "
                         "front strand that reaches 90 degrees can be halved predictably")
    ap.add_argument("--sim-swing-only", default="",
                    help="comma separated name substrings: those accessories keep the "
                         "spring's direction (they still sway) but not its roll about "
                         "their own axis. Measured on this rig's hair, the springs move "
                         "the strands 4-7 degrees of bend but 27-44 degrees of roll, so "
                         "a flat strand reads as spinning in place instead of swaying - "
                         "and the sweeping flat side is what clips through the head. "
                         "Hair is swing-only automatically (see SWING_HAIR_WORDS); use "
                         "this for anything a character names differently")
    ap.add_argument("--no-swing-hair", action="store_true",
                    help="turn the automatic hair rule off, so hair keeps the spring's "
                         "roll as well (only useful for A/B testing; the roll is what "
                         "makes a flat strand spin instead of sway)")
    ap.add_argument("--root-vertical", type=float, default=1.0,
                    help="scale of the up component of the root motion (0..1). 0 keeps "
                         "the body at the captured height, but then the feet sink "
                         "whenever the dance lifts the centre: in MMD the centre rises "
                         "*because* the legs push, so freezing it while the legs still "
                         "move puts the feet under the floor")
    args = ap.parse_args()

    skel_doc, skel = load_skeleton(args.skeleton)
    bones = skel_doc["bones"]
    # The engine's reference (bind) pose, when the exporter managed to read it, is
    # the rest pose the geometry is really skinned in. Everything downstream wants
    # that rather than the pose the character happened to be standing in, so it
    # replaces baseLocal wholesale: directions, leg-length scale and the FK
    # reference all then come from the bind pose.
    ref_bones = sum(1 for b in bones if "refLocal" in b)
    use_ref = (ref_bones == len(bones) and ref_bones > 0
               and args.rest_source == "bind")
    live_rot = [b["baseLocal"]["rotation"] for b in bones]
    if use_ref:
        for b in bones:
            b["baseLocal"] = b["refLocal"]
    # The hand chain is the exception: on this character the reference pose's hand
    # roll reads as a flat wrench, while the captured pose gives a normal hand (the
    # files built before the reference-pose reader, e.g. pose-fingers-t0.json, look
    # right). So keep the reference translation but switch those bones' rest rotation
    # back to the captured one. Rotations of everything else stay on the bind pose.
    hand_rest_switched = 0
    hand_rest_choice = ""
    print(f"rest source: {'engine reference pose (refLocal)' if use_ref else 'captured live pose'}"
          f" ({ref_bones}/{len(bones)} bones carry refLocal)")
    crot_cap = compute_component(bones)
    cpos = [None] * len(bones)
    for b in bones:
        i, p = b["index"], b["parent"]
        lt = b["baseLocal"]["translation"]
        cpos[i] = (tuple(lt) if (p < 0 or cpos[p] is None)
                   else tuple(cpos[p][k] + qrot(crot_cap[p], lt)[k] for k in range(3)))

    pmx_bones, mmd_dirs = pmx_rest_dirs(args.pmx)
    mmd_names = set(pmx_bones)
    pmx_positions = {name: tuple(b["position"]) for name, b in pmx_bones.items()}
    table = build_mapping(mmd_names, skel, pmx_positions)
    if args.finger_mapping == "positional":
        # The mapping as it was before the length alignment: index-by-index, which on a
        # four-bone finger chain drove Finger1/Finger11/Finger12 and never the tip.
        table = build_mapping(mmd_names)
    tracks, vmd_doc = vmd_track_dict(args.vmd_json)
    native_fps = 30.0
    last_frame = int(vmd_doc.get("boneFrameRange", [0, 0])[1]) if vmd_doc.get(
        "boneFrameRange") else 0

    # ---- synthetic rest pose -------------------------------------------------
    mapped_targets = {m["target"]: m for m in table}
    target_of_src = {m["dir"]: m["target"] for m in table}
    target_src_of = {m["target"]: m["dir"] for m in table}
    mmd_first_child = pmx_first_children(args.pmx)
    # A handful of MMD bones use their PMX tail as a marker; for those, the mapped
    # child is the real direction (see pmx_mapped_child_dirs).
    mmd_mapped_dirs = pmx_mapped_child_dirs(
        args.pmx, {src for m in table for src in m["sources"]} | {m["dir"] for m in table})
    # The whole hand chain - hand bone, fingers and thumb - is exempt from the
    # direction correction (see the rest loop below); noted here because this mapping
    # is what the exemption is justified against.

    # ---- hand chain rest: captured pose or reference pose? -------------------
    # Which one gives the hand chain the saner rest is a property of the capture: on
    # one character the captured hand is relaxed and reads as a normal hand, on another
    # it is curled, and using it there makes every finger joint need a 95-144 degree
    # correction, which the bogus-reference guard then rejects - leaving the fingers
    # stuck in the curled pose. So measure both candidates and take the smaller one.
    if use_ref and args.hand_rest == "live":
        mapped_by_target = {m["target"]: m for m in table}

        def chain_correction(source_live):
            probe = []
            for index, b in enumerate(bones):
                name = b["name"] or ""
                use_live = source_live and (name.endswith("-Hand") or "Finger" in name)
                probe.append({"index": b["index"], "name": b["name"],
                              "parent": b["parent"],
                              "baseLocal": {
                                  "rotation": live_rot[index] if use_live
                                              else b["refLocal"]["rotation"],
                                  "translation": b["refLocal"]["translation"]}})
            component = compute_component(probe)
            worst = 0.0
            for b in bones:
                name = b["name"] or ""
                entry = mapped_by_target.get(name)
                if entry is None or not (name.endswith("-Hand") or "Finger" in name):
                    continue
                u = bone_dir_local(bones, skel, name)
                r = (mmd_mapped_dirs.get(entry["dir"]) or mmd_dirs.get(entry["dir"]))
                if u is None or r is None:
                    continue
                worst = max(worst, ang_between(qrot(component[b["index"]], u),
                                               qrot(A_AXIS, r)))
            return worst

        live_worst = chain_correction(True)
        bind_worst = chain_correction(False)
        if live_worst <= bind_worst and live_worst <= 90.0:
            for index, b in enumerate(bones):
                name = b["name"] or ""
                if name.endswith("-Hand") or "Finger" in name:
                    b["baseLocal"] = {"rotation": live_rot[index],
                                      "translation": b["refLocal"]["translation"],
                                      "scale": b["refLocal"]["scale"]}
                    hand_rest_switched += 1
        hand_rest_choice = (
            f"hand chain rest: {'captured' if hand_rest_switched else 'reference'} "
            f"pose (worst finger correction {live_worst:.0f} deg captured vs "
            f"{bind_worst:.0f} deg reference)")
        print(hand_rest_choice)
        # The captured hand rest changes crot_cap, which everything else builds on.
        crot_cap = compute_component(bones)
        for b in bones:
            i, p = b["index"], b["parent"]
            lt = b["baseLocal"]["translation"]
            cpos[i] = (tuple(lt) if (p < 0 or cpos[p] is None)
                       else tuple(cpos[p][k] + qrot(crot_cap[p], lt)[k]
                                  for k in range(3)))

    def rest_direction(target_name, mmd_name):
        return chain_direction(bones, skel, target_name, mmd_name,
                               mmd_first_child, target_of_src)

    no_direction = [n for n in args.no_direction.split(",") if n]
    rest_crot = list(crot_cap)
    rest_local = [b["baseLocal"]["rotation"] for b in bones]
    dir_ok, dir_skipped, dir_bogus = [], [], []
    for b in bones:
        i, p = b["index"], b["parent"]
        cp_rest = rest_crot[p] if p >= 0 else (0.0, 0.0, 0.0, 1.0)
        m = mapped_targets.get(b["name"])
        # The hand chain (hand bone, fingers, thumb) and the foot chain (foot, toe) are
        # exempt from the direction correction. Measurements say the source's direction
        # is the wrong reference for both:
        #   * every finger tail in the PMX is authored straight along +X, so all four
        #     finger directions are identical (33.4 deg from the middle finger for all
        #     of them) and aligning to them flattened the target's own fan into a slab;
        #   * the character's own hand wants a 76.3 degree correction, and applying it
        #     bent the wrist towards the chest;
        #   * the foot's direction is "towards the toe", and MMD's toe is a leaf whose
        #     PMX tail is a forward marker (its own correction is 119.7 deg and already
        #     rejected) - so both sides of that comparison are markers and the 41 deg
        #     result is noise, which tilted the whole foot upwards.
        # Their rotation still arrives as a delta on top of the bone's own direction.
        if ("Finger" in b["name"] or b["name"].endswith("-Hand")
                or "Foot" in b["name"] or "Toe" in b["name"]
                or any(n in b["name"] for n in no_direction)):
            rest_crot[i] = qmul(cp_rest, rest_local[i])
            dir_skipped.append(b["name"])
            continue
        u = rest_direction(b["name"], m["dir"]) if m is not None else None
        r_m = ((mmd_mapped_dirs.get(m["dir"]) or mmd_dirs.get(m["dir"]))
               if m is not None else None)
        if (m is not None and not m.get("no_dir") and u is not None
                and r_m is not None):
            want = qrot(A_AXIS, r_m)                  # desired direction, component
            # A correction this large means the direction reference is wrong, not the
            # character: MMD's toe (つま先) is a leaf whose PMX tail points forward as a
            # marker, and swinging her toe onto it pulled the toes down by 80 degrees
            # (rest correction 119.7 deg). Fingers come out at 50 deg, the hand at 65 and the pelvis at
            # 18.5; only the toe is past 90, so the cap is set well clear of the borderline cases.
            # safe answer is to keep the engine's reference pose for that bone.
            have = qrot(crot_cap[i], u)
            if ang_between(have, want) > 90.0:
                dir_bogus.append((b["name"], m["dir"],
                                  ang_between(have, want)))
                rest_crot[i] = qmul(cp_rest, rest_local[i])
                continue
            base = crot_cap[i]
            # Finger bones take their roll from the HAND instead of from their own
            # captured frame. The capture is a relaxed hand, so each finger's frame
            # already carries its curl; turning that into the MMD rest direction
            # leaves the curl's twist inside the rest frame, which is what made the
            # fingers look screwed around. The hand is not curled, so a swing from
            # its frame is a pure rotation and carries no leftover twist.
            if "Finger" in b["name"] and args.finger_roll != "own":
                side = b["name"].split("-")[1]
                if args.finger_roll == "world":
                    base = A_AXIS
                else:
                    hand = skel.get(f"Bip001-{side}-Hand")
                    if hand is not None:
                        base = rest_crot[hand["index"]]
            have = qrot(base, u)                      # captured direction, component
            rest_crot[i] = qmul(qswing(have, want), base)
            if "Finger" in b["name"] and args.finger_twist:
                # Twist about the bone's own axis: only the geometry's rotation
                # around the finger changes, never where the finger points.
                rest_crot[i] = qmul(rest_crot[i],
                                    q_from_axis_angle(u, math.radians(args.finger_twist)))
            rest_local[i] = qmul(qconj(cp_rest), rest_crot[i])
            dir_ok.append(b["name"])
        else:
            # Undriven bone (or one with no direction reference): keep the
            # captured local rotation, but rebuild the component rotation from
            # the *corrected* parent -- carrying the stale captured value here
            # silently skews every mapped descendant of this bone.
            rest_crot[i] = qmul(cp_rest, rest_local[i])
            if m is not None:
                dir_skipped.append(b["name"])

    # ---- hand plane (the "crab hand") ----------------------------------------
    # The finger roots are children of the hand, so the plane they fan out in is set
    # by the hand's own roll. A hand whose roll is inherited from the reference pose
    # can be a quarter turn out of the palm plane, which reads as a crab claw: the
    # fingers point the right way individually but the fan itself is rotated. The
    # plane is measurable, so measure it and twist the hand by exactly the difference.
    def rest_positions(crot):
        out = [None] * len(bones)
        for b in bones:
            i, p = b["index"], b["parent"]
            t = b["baseLocal"]["translation"]
            out[i] = (tuple(t) if (p < 0 or out[p] is None)
                      else tuple(out[p][k] + qrot(crot[p], t)[k] for k in range(3)))
        return out

    def perp_to(v, n):
        d = sum(v[k] * n[k] for k in range(3))
        return tuple(v[k] - n[k] * d for k in range(3))

    hand_plane_fixes = []
    if pmx_positions and args.hand_plane:
        rest_pos = rest_positions(rest_crot)
        for side, side_en in (("左", "L"), ("右", "R")):
            hand_name = f"Bip001-{side_en}-Hand"
            index_name = f"Bip001-{side_en}-Finger1"
            pinky_name = f"Bip001-{side_en}-Finger4"
            mmd_hand = f"{side}手首"
            mmd_index = f"{side}人指１"
            mmd_pinky = f"{side}小指１"
            if not all(n in skel for n in (hand_name, index_name, pinky_name)):
                continue
            if not all(n in pmx_positions for n in (mmd_hand, mmd_index, mmd_pinky)):
                continue

            def cross(a, b):
                return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                        a[0] * b[1] - a[1] * b[0])

            def sub(a, b):
                return tuple(a[k] - b[k] for k in range(3))

            def normal(hand, index, pinky):
                v = cross(sub(index, hand), sub(pinky, hand))
                return unit(v)

            got = normal(rest_pos[skel[hand_name]["index"]],
                         rest_pos[skel[index_name]["index"]],
                         rest_pos[skel[pinky_name]["index"]])
            want = qrot(A_AXIS, normal(pmx_positions[mmd_hand],
                                       pmx_positions[mmd_index],
                                       pmx_positions[mmd_pinky]))
            hand_bone = skel[hand_name]
            axis = qrot(rest_crot[hand_bone["index"]],
                        rest_direction(hand_name, target_src_of.get(hand_name) or ""))
            def perp(v):
                d = sum(v[k] * axis[k] for k in range(3))
                return tuple(v[k] - axis[k] * d for k in range(3))
            p1, p2 = perp(got), perp(want)
            if sum(c * c for c in p1) > 1e-9 and sum(c * c for c in p2) > 1e-9:
                angle = math.degrees(math.atan2(
                    sum(cross(p1, p2)[k] * axis[k] for k in range(3)),
                    sum(p1[k] * p2[k] for k in range(3))))
                before = ang_between(got, want)
                u = rest_direction(hand_name, target_src_of.get(hand_name) or "")
                rest_crot[hand_bone["index"]] = qmul(
                    rest_crot[hand_bone["index"]],
                    q_from_axis_angle(u, math.radians(angle)))
                cp = rest_crot[hand_bone["parent"]] if hand_bone["parent"] >= 0 else ident
                rest_local[hand_bone["index"]] = qmul(
                    qconj(cp), rest_crot[hand_bone["index"]])
                after_pos = rest_positions(rest_crot)
                after = ang_between(normal(after_pos[skel[hand_name]["index"]],
                                           after_pos[skel[index_name]["index"]],
                                           after_pos[skel[pinky_name]["index"]]), want)
                # Second degree of freedom: with the plane right, the fan can still be
                # rotated *inside* that plane, which reads as "all four fingers lean to
                # one side". Measure that in-plane angle and rotate the hand about the
                # palm normal to remove it. (Rotating about the bone axis cannot do it -
                # the axis is perpendicular to the normal, which is already correct.)
                spread_before = spread_after = float("nan")
                in_plane = 0.0
                axis_n = unit(want)
                got_root = sub(after_pos[skel[index_name]["index"]],
                               after_pos[skel[hand_name]["index"]])
                want_root = qrot(A_AXIS, sub(pmx_positions[mmd_index],
                                             pmx_positions[mmd_hand]))
                g1 = perp_to(got_root, axis_n)
                g2 = perp_to(want_root, axis_n)
                if sum(c * c for c in g1) > 1e-9 and sum(c * c for c in g2) > 1e-9:
                    spread_before = math.degrees(math.acos(max(-1.0, min(1.0, sum(
                        a * b for a, b in zip(unit(got_root), unit(want_root)))))))
                    in_plane = math.degrees(math.atan2(
                        sum(cross(g1, g2)[k] * axis_n[k] for k in range(3)),
                        sum(g1[k] * g2[k] for k in range(3))))
                    rest_crot[hand_bone["index"]] = qmul(
                        q_from_axis_angle(axis_n, math.radians(in_plane)),
                        rest_crot[hand_bone["index"]])
                    cp = rest_crot[hand_bone["parent"]] if hand_bone["parent"] >= 0 else ident
                    rest_local[hand_bone["index"]] = qmul(
                        qconj(cp), rest_crot[hand_bone["index"]])
                    spread_pos = rest_positions(rest_crot)
                    spread_after = math.degrees(math.acos(max(-1.0, min(1.0, sum(
                        a * b for a, b in zip(
                            unit(sub(spread_pos[skel[index_name]["index"]],
                                     spread_pos[skel[hand_name]["index"]])),
                            unit(want_root)))))))
                hand_plane_fixes.append((side_en, angle, before, after,
                                         in_plane, spread_before, spread_after))

    # ---- frames --------------------------------------------------------------
    frames = sorted({f for tr in tracks.values() for f in tr})
    if not frames:
        raise SystemExit("no bone keyframes in the VMD")
    frame_lo, frame_hi = frames[0], frames[-1]
    step = max(1, int(round(native_fps / args.fps)))
    out_frames = list(range(frame_lo, frame_hi + 1, step))
    if args.max_frames:
        out_frames = out_frames[:args.max_frames]

    ident = (0.0, 0.0, 0.0, 1.0)
    qcache = {}
    for name, q in tracks.items():
        qcache[name] = q

    out_bones = {m["target"]: [] for m in table}
    out_pos = []
    out_offsets = {}
    pelvis_idx = skel.get("Bip001-Pelvis", {}).get("index")
    for name in ("Bip001-Spine",):
        if name in skel:
            out_offsets[name] = []
    neg_hip = {name: True for name in ("Bip001-Spine",)}
    world_shift = {name: (0.0, 0.0, 0.0) for name in out_offsets}

    # ---- secondary bones (hair / cloth / skirt / ribbons) ---------------------
    # These have no source data we could use: the VMD's hair tracks were authored
    # on a model the user does not have, so their rest positions are unknown. We
    # therefore simulate them instead: each one lags behind the pose it would
    # have rigidly, via a spring-damper on its tip. With no movement the spring
    # settles exactly on the rigid pose, so this can only add life -- it cannot
    # change the silhouette at rest.
    def is_secondary(name):
        low = name.lower()
        if "twist" in low or "finger" in low or "ik" in low:
            return False
        if name.startswith("Bn_"):
            return True
        return any(k in low for k in
                   ("hair", "qun", "cloth", "tail", "piao", "gongpai", "lalian",
                    "xiong", "tie", "skirt", "ribbon"))

    def bone_axis(name):
        """(unit direction of the child offset in the bone's local frame, length)."""
        b = skel.get(name)
        if b is None:
            return None, 0.0
        kids = [c for c in bones if c["parent"] == b["index"] and c["name"] in skel]
        best, bl = None, 0.0
        for c in kids:
            t = c["baseLocal"]["translation"]
            n = math.sqrt(sum(x * x for x in t))
            if n > bl:
                best, bl = t, n
        if best is None or bl < 1e-3:
            return None, 0.0
        return unit(best), bl

    def twist_about(q, axis):
        v = (q[0], q[1], q[2])
        d = sum(v[i] * axis[i] for i in range(3))
        n = math.sqrt(d * d + q[3] * q[3])
        if n < 1e-9:
            return (0.0, 0.0, 0.0, 1.0)
        return (axis[0] * d / n, axis[1] * d / n, axis[2] * d / n, q[3] / n)

    # Ornaments that must not swing. A matching name -- and everything below it -- is
    # left out of the simulation, so the runtime keeps it on the engine reference pose
    # and it follows its parent rigidly. An earring asked for this: the ring bone at
    # the end of the ear chain has no track of its own, but the ear chain above it was
    # simulated, so the ring was dragged around by a 46 degree swing.
    no_sim = [token.lower() for token in args.no_sim.split(",") if token]
    # Head rings and other hard ornaments: matched by bone name (with their whole
    # subtree) from a built-in table, because only a handful of characters have one and
    # no keyword can tell a ring from the hair sitting next to it. Entries are matched as
    # substrings, so one entry covers a whole group (伊洛伊's ring is 30 bones named
    # Bn_m_headLineA_*/Bn_m_headLineB_* -- two entries); --no-sim-names adds to the table
    # for a character that is not in it yet.
    ornament_names = {n.lower() for n in HEAD_ORNAMENT_BONES} if args.no_sim_builtin \
        else set()
    ornament_names |= {t.strip().lower() for t in args.no_sim_names.split(",")
                       if t.strip()}
    rigid = set()
    for b in bones:
        name = (b["name"] or "").lower()
        if (b["parent"] >= 0 and b["parent"] in rigid) \
                or any(t in name for t in no_sim) \
                or any(t in name for t in ornament_names):
            rigid.add(b["index"])

    sim_scale = []
    for token in args.sim_scale.split(","):
        sub, sep, value = token.partition("=")
        if not sep or not sub.strip():
            continue
        try:
            gain = float(value)
        except ValueError:
            raise SystemExit(f"--sim-scale: {token!r} is not name=gain")
        sim_scale.append((sub.strip().lower(), max(0.0, min(1.0, gain))))

    def sim_gain(name):
        """Smallest gain any matching token asks for (1.0 when nothing matches)."""
        low = name.lower()
        gain = 1.0
        for sub, value in sim_scale:
            if sub in low:
                gain = min(gain, value)
        return gain

    swing_only = [token.lower() for token in args.sim_swing_only.split(",") if token]
    hair_rule = not args.no_swing_hair

    def is_swing_only(name):
        # Hair is swing-only by default, for every character: the spring bends a strand
        # only 4-7 degrees but rolls it 27-44, and a flat strand whose roll sweeps reads
        # as spinning in place (on one character's front strand the roll was 179.7 deg --
        # the whole of its motion -- and it scraped through the head). Character bone
        # names differ, so match the group word plus a few obvious spellings, and keep
        # --sim-swing-only for anything a new character calls something else.
        low = name.lower()
        if hair_rule and any(sub in low for sub in SWING_HAIR_WORDS):
            return True
        return any(sub in low for sub in swing_only)

    secondary = []
    if args.sim_secondary:
        for b in bones:
            if not b["name"] or b["name"] in mapped_targets:
                continue
            if b["index"] in rigid or not is_secondary(b["name"]):
                continue
            u, length = bone_axis(b["name"])
            if u is None:
                continue
            secondary.append({
                "index": b["index"], "name": b["name"], "parent": b["parent"],
                "u": u, "len": length,
                "local": rest_local[b["index"]],
                "trans": b["baseLocal"]["translation"],
            })
    secondary_by_index = {s["index"]: s for s in secondary}
    sim_state = {s["index"]: {"dir": None, "w": [0.0, 0.0, 0.0],
                             "pos": None, "pos2": None} for s in secondary}
    for s in secondary:
        out_bones[s["name"]] = []
    base_trans = [b["baseLocal"]["translation"] for b in bones]

    root = mapped_targets.get("Bip001")
    root_rest_t = None
    root_rest_index = 0
    if root:
        rb = skel["Bip001"]
        root_rest_t = rb["baseLocal"]["translation"]
        root_rest_index = rb["index"]
    scale = 0.0
    mm_leg = 0.0
    if root:
        # cm per MMD unit, from the leg length ratio. Both sides use the *segment*
        # lengths, so the ratio does not depend on the pose the export was captured
        # in (an idle capture with bent knees used to shrink the number).
        def leg_len(mm_hip, mm_knee, mm_ankle):
            a, b, c = (pmx_bones[mm_hip]["position"], pmx_bones[mm_knee]["position"],
                       pmx_bones[mm_ankle]["position"])
            return (math.dist(a, b) + math.dist(b, c))
        mm_leg = leg_len("左足", "左ひざ", "左足首")
        tl = 0.0
        # The thigh bone's own translation is the hip's *lateral* offset (~7.7 cm sideways),
        # not a leg segment -- counting it inflated the scale ~10% and lifted the ankles
        # ~13 cm, which reads as the legs slanting and the body sitting low.
        chain = ("Bip001-L-Calf", "Bip001-L-Foot")
        if all(n in skel for n in chain):
            tl = sum(math.dist((0.0, 0.0, 0.0), skel[n]["baseLocal"]["translation"])
                     for n in chain)
        scale = (tl / mm_leg) if mm_leg > 1e-6 else 8.0

    # ---- target-side leg IK -------------------------------------------------
    # The legs cannot simply map the source's rotations: the source model's leg proportions
    # (thigh/shin ratio and the 腰 -> 左足 hip offset) differ from this rig's, so a baked leg
    # lands the ankle several centimetres off and the pelvis sinks below the source's. Instead
    # the TARGET's own leg is re-solved: the hip goes where the source's hip is (scaled), the
    # ankle is aimed at the source's ankle, and a two-bone solve bends the knee with the
    # target's own lengths -- what MMD does live, which is why "any model" plays a motion the
    # same way. Mirrors motion_builder.cpp's `leg_solves`.
    leg_solves = []
    leg_by_thigh = {}
    # No waist bone is needed: the solve is taken in the source's own hip frame, so a PMD
    # skeleton (下半身 straight under センター, no 腰) solves exactly like a PMX one.
    if root and scale > 1e-9:
        for side, jp in (("L", "左"), ("R", "右")):
            names = {k: f"Bip001-{side}-{k}" for k in ("Thigh", "Calf", "Foot")}
            src = {k: jp + v for k, v in (("thigh", "足"), ("knee", "ひざ"), ("ankle", "足首"))}
            if not all(n in skel for n in names.values()):
                continue
            if not all(n in pmx_bones for n in src.values()):
                continue
            thigh_len = math.dist((0, 0, 0), skel[names["Calf"]]["baseLocal"]["translation"])
            shin_len = math.dist((0, 0, 0), skel[names["Foot"]]["baseLocal"]["translation"])
            if thigh_len < 1e-6 or shin_len < 1e-6:
                continue
            solve = {"name": names["Thigh"], "calf_name": names["Calf"],
                     "foot_name": names["Foot"], "calf": skel[names["Calf"]]["index"],
                     "foot": skel[names["Foot"]]["index"],
                     "src_hip": src["thigh"], "src_knee": src["knee"],
                     "src_ankle": src["ankle"],
                     "thigh_len": thigh_len, "shin_len": shin_len}
            leg_solves.append(solve)
            leg_by_thigh[skel[names["Thigh"]]["index"]] = solve
    leg_thigh_names = {s["name"] for s in leg_solves}

    dt = 1.0 / (args.fps if args.fps > 0 else 30.0)
    gravity = (0.0, 0.0, -args.sim_gravity)
    root_center_base = None
    root_bump_base = None
    # The target's own root -> pelvis offset, when the pelvis is the root's direct child: the
    # mapped root rotation already swings it, so it must be subtracted from the emission.
    root_lever = None
    if "Bip001-Pelvis" in skel and "Bip001" in skel:
        pelvis_bone = skel["Bip001-Pelvis"]
        parent_name = bones[pelvis_bone["parent"]]["name"] if pelvis_bone["parent"] >= 0 else ""
        if parent_name == "Bip001":
            root_lever = tuple(pelvis_bone["baseLocal"]["translation"])
    sample_track = build_sampler(tracks)
    worst_unit = 0.0
    worst_secondary_deviation = 0.0
    lag_samples = []
    clamped = 0
    # ---- source-model IK --------------------------------------------------------------
    # MMD drives its own skeleton first: a motion can carry nothing but IK-bone positions
    # for the legs (`主角.vmd`), and those have to be solved into bone rotations before any
    # of it can be mapped onto the target rig. `--ik 0` skips the solver entirely, which is
    # the regression switch: with it the output must be byte-identical to the pre-IK
    # converter.
    mmd_pose = MmdIkPose(pmx_bones) if pmx_bones else None
    # ---- which bone carries the body's travel -----------------------------------------
    # The target's root link (Bip001) absorbs the whole MMD root chain, and the pelvis it
    # carries (mapped from 腰) never moves its own local offset, so the chain's *combined*
    # translation is the root motion. Taking センター's own translation alone dropped the
    # layers below it: 3.95 mmd units (38 cm, mostly vertical) on Chu-Chu-U-Chu and 27.6 on
    # 主角, while the legs -- which do see the whole chain, through the IK solve -- bent for a
    # lift the body never made, so the character slid (moonwalked) instead of rising.
    # The end of the chain is the deepest bone that actually animates a position: a motion
    # that drives its travel through 下半身 (TDA_30fps) instead of センター is a real file, and
    # stopping at センター would drop it.
    named_chain = [(name, pmx_bones[name]["index"])
                   for name in ("全ての親", "センター", "グルーブ", "腰", "下半身")
                   if name in pmx_bones]
    root_spans = {}
    for name in ("全ての親", "センター", "グルーブ", "腰", "下半身"):
        keys = tracks.get(name)
        if keys:
            root_spans[name] = max(max(key[1][axis] for key in keys.values())
                                  - min(key[1][axis] for key in keys.values())
                                  for axis in range(3))

    def deepest_with_travel(names):
        found = [name for name in names
                 if name in pmx_bones and root_spans.get(name, 0.0) > 1e-6]
        return found[-1] if found else ""

    # Placement layers (全ての親/センター/グルーブ) say where the character *stands*; 腰/下半身 say
    # where the hips sit relative to the planted feet. Only the former may move the whole
    # character: folding the hip layer in drags the feet along (rigoutput.vmd: 43 cm of
    # vertical hip sway became a global lift, so the feet floated). The hip layer is still the
    # answer when nothing above it moves -- a TDA export drives its travel through 下半身.
    #
    # The reference is the bone that *is* the target's pelvis (`Bip001-Pelvis <- 腰`), not the
    # topmost moving layer: 腰 hangs below グルーブ, so a センター rotation swings it by its own
    # lever. Emitting センター's own world position instead left rigoutput.vmd 68.8 cm short
    # (measured independently by feet_check.py), i.e. the body pivoted while the pelvis stayed
    # on the old arc. The hip layer is only used when the whole pelvis chain is static.
    pelvis_source = next((entry["sources"][0] for entry in table
                          if entry["target"] == "Bip001-Pelvis" and entry["sources"]), "")
    upper_moves = any(root_spans.get(name, 0.0) > 1e-6
                      or (tracks.get(name) and any(
                          any(abs(v) > 1e-6 for v in key[0][:3])
                          for key in tracks[name].values()))
                      for name in ("全ての親", "センター", "グルーブ"))
    root_source = (pelvis_source if pelvis_source in pmx_bones and
                   (upper_moves or root_spans.get(pelvis_source, 0.0) > 1e-6)
                   else deepest_with_travel(("全ての親", "センター", "グルーブ"))
                   or deepest_with_travel(("腰", "下半身")) or "センター")
    root_end = next((slot for slot, item in enumerate(named_chain)
                     if item[0] == root_source), 0)
    root_chain = [index for _, index in named_chain[:root_end + 1]]
    ik_records = {}
    for name, records in (vmd_doc.get("ikStateRecords") or {}).items():
        ik_records[name] = sorted(records, key=lambda item: item["frame"])
    ik_gaps = []
    twist_bones = build_twist_list(
        skel, {src: m["target"] for m in table for src in m["sources"]},
        lambda target: rest_direction(target, mapped_targets[target]["dir"])
        if target in mapped_targets else bone_dir_local(bones, skel, target))
    if not args.twist:
        # Keep the tracks (so the bone set stays identical between runs) but stop
        # spreading the limb twist, to A/B whether those synthesised rotations are
        # what makes an elbow look wrong.
        for item in twist_bones:
            item["weight"] = 0.0
    for item in twist_bones:
        out_bones[item["target"]] = []
    for f in out_frames:
        local_rot = list(rest_local)
        local_trans = list(base_trans)
        comp_rot_frame = [None] * len(bones)
        comp_pos_frame = [None] * len(bones)
        ik_local = None
        if mmd_pose is not None:
            mmd_pose.set_frame(lambda name: sample_track(name, f))
            if args.ik:
                disabled = set()
                for name, records in ik_records.items():
                    state = None
                    for record in records:
                        if record["frame"] > f:
                            break
                        state = record["on"]
                    if state == 0:
                        disabled.add(name)
                ik_gaps.extend(mmd_pose.solve(disabled))
                ik_local = {mmd_pose.name[i]: mmd_pose.rot[i]
                            for i in range(mmd_pose.count)}
        for m in table:
            q = ident
            # 前置逆源先乘：父链多带的旋转是左乘进来的，只有左乘回去才能抵消。
            for src in m.get("prefix", ()):
                if ik_local is not None:
                    q = qmul(q, qconj(ik_local.get(src, ident)))
                    continue
                v = sample_track(src, f)
                if v is not None:
                    q = qmul(q, qconj(qnorm(v[0])))
            for src in m["sources"]:
                if ik_local is not None:
                    q = qmul(q, ik_local.get(src, ident))
                    continue
                v = sample_track(src, f)
                if v is not None:
                    q = qmul(q, qnorm(v[0]))
            # 后置逆源：用于抵消同一条链里后段源多带进来的旋转。
            for src in m.get("inverse", ()):
                if ik_local is not None:
                    q = qmul(q, qconj(ik_local.get(src, ident)))
                    continue
                v = sample_track(src, f)
                if v is not None:
                    q = qmul(q, qconj(qnorm(v[0])))
            tgt = m["target"]
            b = skel[tgt]
            p = b["parent"]
            cp_rest = rest_crot[p] if p >= 0 else ident
            delta = qmul(qmul(A_AXIS, q), qconj(A_AXIS))     # world delta
            # conj(Cp(rest), delta) * Cg(rest): the conjugation brings the world
            # delta into the parent's frame, and Cg(rest) (not Rg(rest)) puts it
            # back into this bone's local frame. At Q=identity both coincide,
            # which is why the rest check alone cannot catch a mix-up here.
            local = qmul(qmul(qconj(cp_rest), delta), rest_crot[b["index"]])
            local = qnorm(local)
            worst_unit = max(worst_unit, abs(1.0 - math.sqrt(sum(c * c for c in local))))
            local_rot[b["index"]] = local
            out_bones[tgt].append([round(c, 6) for c in local])
        # Counter-rotate the spine's base so the pelvis -- which also carries 下半身 for the
        # hips -- does not drag the torso with it. The rotation above is fixed by the left-
        # multiplied inverse; the base *position* still needs a per-frame local translation,
        # t = C^-1 · H^-1 · C · rest, with C the pelvis's rest component rotation and H the
        # 下半身 world delta.
        if out_offsets and mmd_pose is not None and pelvis_idx is not None and \
                pelvis_idx < len(rest_crot) and "下半身" in pmx_bones:
            hip_local = mmd_pose.rot[pmx_bones["下半身"]["index"]]
            hip_delta = qmul(qmul(A_AXIS, hip_local), qconj(A_AXIS))
            frame_pelvis = rest_crot[pelvis_idx]
            for name in out_offsets:
                if name in leg_thigh_names:
                    continue  # the leg offsets are written by the leg IK pass below
                idx = skel[name]["index"]
                body = qmul(qmul(qconj(frame_pelvis),
                                 qconj(hip_delta) if neg_hip.get(name) else hip_delta),
                            frame_pelvis)
                offset = qrot(body, base_trans[idx])
                shift = world_shift.get(name, (0.0, 0.0, 0.0))
                if shift != (0.0, 0.0, 0.0):
                    # Express the hip drop in the pelvis's *rest* frame so the FK below
                    # carries it with the body's tilt (like the source's 腰->左足 drop).
                    local_shift = qrot(qconj(frame_pelvis), shift)
                    offset = tuple(offset[k] + local_shift[k] for k in range(3))
                local_trans[idx] = list(offset)
                out_offsets[name].append([round(c, 6) for c in offset])
        if root and root_rest_t is not None and mmd_pose is not None:
            # The reference bone's *world* position, not just its own translation. The target
            # rig's pelvis sits exactly on its root bone (Bip001 and Bip001-Pelvis share an
            # origin, lever 0), so the root translation *is* the pelvis placement, and the
            # source's pelvis moves with the rotation of the chain above it as well as with
            # its translations. Emitting only the translation part left rigoutput.vmd up to
            # 7.4 mmd units (71 cm) short on the frames where センター turns hardest -- the
            # whole body swept around the hip while the feet stayed where the previous frame
            # left them, which reads as skating backwards around a centre point.
            absolute = mmd_pose.wpos[root_chain[-1]] if root_chain else (0.0, 0.0, 0.0)
            if args.feet_anchor:
                # The character's placement follows the feet instead of the pelvis. The legs
                # already reproduce the source's ankle *relative* to the pelvis (measured
                # within 0.1-0.7 cm), so anchoring here lands the feet where the source's feet
                # are and the body swings over them -- which is what "the fixed point is on the
                # feet" means. Anchoring to the pelvis instead moves the feet with the body.
                anchors = [mmd_pose.wpos[pmx_bones[name]["index"]]
                           for name in ("左足首", "右足首") if name in pmx_bones]
                if anchors:
                    absolute = tuple(sum(point[k] for point in anchors) / len(anchors)
                                     for k in range(3))
            if root_center_base is None:
                root_center_base = absolute
            delta = qrot(A_AXIS, tuple(absolute[k] - root_center_base[k] for k in range(3)))
            # Whatever the target's own root->pelvis lever already produces must not be
            # counted twice: the mapped root rotation swings that offset by itself.
            if root_lever is not None and scale > 1e-9:
                bump = qrot(local_rot[root_rest_index], root_lever)
                if root_bump_base is None:
                    root_bump_base = bump
                delta = tuple(delta[k] - (bump[k] - root_bump_base[k]) / scale
                              for k in range(3))
            if args.root_vertical < 1.0:
                delta = (delta[0], delta[1], delta[2] * args.root_vertical)
            out_pos.append([round(delta[k], 6) for k in range(3)])
            moved = qrot(A_AXIS, tuple(c * scale for c in absolute))
            local_trans[root_rest_index] = [root_rest_t[k] + moved[k]
                                            for k in range(3)]

        # Component-space pass (parents before children). Driven bones use the
        # motion, secondary bones are simulated, everything else stays rigid.
        for b in bones:
            i, p = b["index"], b["parent"]
            if p < 0 or comp_rot_frame[p] is None:
                prot = ident
                ppos = (0.0, 0.0, 0.0)
            else:
                prot = comp_rot_frame[p]
                ppos = comp_pos_frame[p]
            # Target-side leg IK (see leg_solves): the pelvis has been processed by now, so its
            # live component rotation is available. Mirrors motion_builder.cpp.
            lk = leg_by_thigh.get(i)
            if lk is not None and mmd_pose is not None:
                # Origin is the source's own *hip*, not its pelvis: how far the hips hang below
                # the waist is a rig constant (0.68 units on the author's model, 2.49 on the PMD
                # the user plays in MMD) and transferring it hung this rig's legs 21 cm below its
                # own hips -- the ankles went from +3.3 cm to -10.9 cm on the same frame.
                hip_now = mmd_pose.wpos[pmx_bones[lk["src_hip"]]["index"]]
                def rel(nm):
                    pt = mmd_pose.wpos[pmx_bones[nm]["index"]]
                    return qrot(A_AXIS, tuple((pt[k] - hip_now[k]) * scale for k in range(3)))
                hip_rel = (0.0, 0.0, 0.0)
                goal = rel(lk["src_ankle"])
                knee_ref = rel(lk["src_knee"])
                # MMD pins the foot to the 足ＩＫ target (fixed in the model's space) and lets the
                # legs connect it to the hips; ours is placed relative to the hip while the *body*
                # follows 腰/下半身, so the hips' travel around that bone (up to 21 cm when 下半身
                # pitches -- every crouch) landed in the feet: measured 19.4 cm off on average and
                # 11.7% of frames 3-18 cm below the ground, where MMD never goes below rest. Add
                # that travel back to the target only; the body keeps its placement.
                if root_chain and "waist_index" not in lk:
                    # Resolved here (not at setup) because the root chain is known by now; the bone
                    # is the same one the body's placement follows. Mirrors motion_builder.cpp.
                    lk["waist_index"] = root_chain[-1]
                    wb = next(b["position"] for b in pmx_bones.values() if b["index"] == lk["waist_index"])
                    hb = pmx_bones[lk["src_hip"]]["position"]
                    lk["hip_from_waist_rest"] = tuple(hb[k] - wb[k] for k in range(3))
                if "waist_index" in lk:
                    w_now = mmd_pose.wpos[lk["waist_index"]]
                    arc = qrot(A_AXIS, tuple(
                        ((hip_now[k] - w_now[k]) - lk["hip_from_waist_rest"][k]) * scale
                        for k in range(3)))
                    goal = tuple(goal[k] + arc[k] for k in range(3))
                a, c = lk["thigh_len"], lk["shin_len"]
                reach = a + c
                to_goal = tuple(goal[k] - hip_rel[k] for k in range(3))
                d = math.sqrt(sum(v * v for v in to_goal))
                if d < 1e-6:
                    thigh_dir = (0.0, 0.0, -1.0)
                    shin_dir = thigh_dir
                elif d >= reach:
                    thigh_dir = unit(to_goal)
                    shin_dir = thigh_dir
                else:
                    u = unit(to_goal)
                    cos_theta = max(-1.0, min(1.0, (a * a + d * d - c * c) / (2.0 * a * d)))
                    theta = math.acos(cos_theta)
                    nrm = (knee_ref[1] * to_goal[2] - knee_ref[2] * to_goal[1],
                           knee_ref[2] * to_goal[0] - knee_ref[0] * to_goal[2],
                           knee_ref[0] * to_goal[1] - knee_ref[1] * to_goal[0])
                    n_hat = unit(nrm) if math.sqrt(sum(v * v for v in nrm)) > 1e-6 else (1.0, 0.0, 0.0)
                    w = unit((n_hat[1] * u[2] - n_hat[2] * u[1],
                              n_hat[2] * u[0] - n_hat[0] * u[2],
                              n_hat[0] * u[1] - n_hat[1] * u[0]))
                    sn, cs = math.sin(theta), math.cos(theta)
                    dir1 = tuple(u[k] * cs + w[k] * sn for k in range(3))
                    dir2 = tuple(u[k] * cs - w[k] * sn for k in range(3))
                    k1 = tuple(hip_rel[k] + dir1[k] * a for k in range(3))
                    k2 = tuple(hip_rel[k] + dir2[k] * a for k in range(3))
                    knee = k1 if math.dist(k1, knee_ref) <= math.dist(k2, knee_ref) else k2
                    thigh_dir = unit(tuple(knee[k] - hip_rel[k] for k in range(3)))
                    shin_dir = unit(tuple(goal[k] - knee[k] for k in range(3)))
                # The thigh keeps this rig's own rest translation (no override): the hip must stay
                # where this rig puts it and swing with the pelvis rotation the mapped source
                # rotation already carries. Overwriting it with the source's waist->hip offset
                # hangs the legs at the source rig's height (feet through the floor); overwriting
                # it with the source hip's travel is not a rigid rotation of one offset and spread
                # the two hips apart (14.8 cm -> 30-45 cm separation).
                mapped_thigh = qmul(prot, local_rot[i])
                mapped_calf = qmul(mapped_thigh, local_rot[lk["calf"]])
                mapped_foot = qmul(mapped_calf, local_rot[lk["foot"]])
                thigh_world = qmul(ik_swing(qrot(mapped_thigh, (1.0, 0.0, 0.0)), thigh_dir),
                                   mapped_thigh)
                calf_world = qmul(ik_swing(qrot(mapped_calf, (1.0, 0.0, 0.0)), shin_dir),
                                  mapped_calf)
                local_rot[i] = qmul(qconj(prot), thigh_world)
                local_rot[lk["calf"]] = qmul(qconj(thigh_world), calf_world)
                local_rot[lk["foot"]] = qmul(qconj(calf_world), mapped_foot)
                for nm, val in ((lk["name"], local_rot[i]), (lk["calf_name"], local_rot[lk["calf"]]),
                                (lk["foot_name"], local_rot[lk["foot"]])):
                    if out_bones.get(nm):
                        out_bones[nm][-1] = [round(v, 6) for v in val]
            offset = local_trans[i]
            rotated = qrot(prot, offset)
            head = (ppos[0] + rotated[0], ppos[1] + rotated[1],
                    ppos[2] + rotated[2])
            comp_pos_frame[i] = head
            s = secondary_by_index.get(i)
            if s is None:
                comp_rot_frame[i] = qmul(prot, local_rot[i])
                continue
            # rigid target: the direction this bone would have with no lag
            rigid_rot = qmul(prot, s["local"])
            axis_world = qrot(rigid_rot, s["u"])
            # Angle-space spring: the tip-based version blows up on the many
            # short bones in a hair/cloth chain, because a millimetre of tip
            # error is a hundred degrees once the lever arm is 1 cm. Working on
            # the direction itself is length independent, so it behaves the same
            # for a 1 cm accessory bone and a 10 cm hair chain.
            state = sim_state[i]
            if state["dir"] is None:
                state["dir"] = list(axis_world)
                state["w"] = [0.0, 0.0, 0.0]
            # Hanging target: gravity plus the parent's own acceleration (so a chain
            # swings back when the body accelerates forward), blended towards the
            # rigid direction by --sim-drape. Without this the spring's rest target is
            # the rigid pose, and for a chain whose parent barely rotates -- a skirt
            # hem on the pelvis, a ribbon on a hand -- the lag stays at zero and the
            # accessory is simply stiff.
            target = axis_world
            head = comp_pos_frame[i]
            previous, older = state["pos"], state["pos2"]
            state["pos2"], state["pos"] = previous, list(head)
            if args.sim_drape > 0.0:
                gravity_down = (0.0, 0.0, -981.0)          # cm/s^2, target up is +Z
                if previous is not None and older is not None and dt > 1e-9:
                    accel = tuple((head[k] - 2.0 * previous[k] + older[k]) /
                                  (dt * dt) for k in range(3))
                    # Cap the acceleration at 3 g: the centre track is sparse and its
                    # second difference can spike, and an uncapped term made the hang
                    # direction whip around so hard that the spring sat on its clamp
                    # 5% of the frames.
                    magnitude = math.sqrt(sum(c * c for c in accel))
                    limit = 3.0 * 981.0
                    if magnitude > limit:
                        accel = tuple(c * limit / magnitude for c in accel)
                    gravity_down = tuple(gravity_down[k] -
                                         args.sim_inertia * accel[k]
                                         for k in range(3))
                hang = unit(gravity_down)
                if hang != (0.0, 0.0, 0.0):
                    blended = tuple(axis_world[k] * (1.0 - args.sim_drape) +
                                    hang[k] * args.sim_drape for k in range(3))
                    if sum(c * c for c in blended) > 1e-9:
                        target = unit(blended)
            d = state["dir"]
            w = state["w"]
            # Substep: at 30 fps a single explicit step with stiffness 200 is only
            # marginally stable, and with a *moving* target (drape + inertia) it sat on
            # its clamp for 5% of the frames. Four substeps make the same spring track
            # the target instead of oscillating around it.
            substeps = 4
            h = dt / substeps
            for _ in range(substeps):
                cross = (d[1] * target[2] - d[2] * target[1],
                         d[2] * target[0] - d[0] * target[2],
                         d[0] * target[1] - d[1] * target[0])
                for k in range(3):
                    w[k] += (args.sim_stiffness * cross[k]
                             - args.sim_damping * w[k]) * h
                step = (w[0] * h, w[1] * h, w[2] * h)
                d = unit((d[0] + step[1] * d[2] - step[2] * d[1],
                          d[1] + step[2] * d[0] - step[0] * d[2],
                          d[2] + step[0] * d[1] - step[1] * d[0]))
                if d == (0.0, 0.0, 0.0):
                    d = target
            alignment = max(-1.0, min(1.0,
                                      sum(a * b for a, b in zip(d, target))))
            lag = math.degrees(math.acos(alignment))
            if lag > args.sim_max_lag:
                # clamp: never let the secondary motion swing past the clamp away
                # from its target (the rigid pose, or the hanging pose when drape is
                # on - clamping against the rigid pose would fight the drape forever)
                t = args.sim_max_lag / lag
                d = unit(tuple(d[k] + (target[k] - d[k]) * t for k in range(3)))
                for k in range(3):
                    w[k] *= 0.5
                lag = args.sim_max_lag
            state["dir"] = d
            was_clamped = lag >= args.sim_max_lag - 1e-6
            deviation = lag
            lag_samples.append(lag)
            if was_clamped:
                clamped += 1
            local_axis = unit(qrot(qconj(prot), d))
            local = qmul(qswing(s["u"], local_axis), twist_about(s["local"], s["u"]))
            if is_swing_only(s["name"]):
                # Keep where the strand points, hold its roll at the rigid pose. The
                # rig's hair springs move the strands only 4-7 degrees of bend but
                # 27-44 degrees of roll, and a flat strand's roll is what reads as
                # "spinning in place" and sweeps through the head.
                deviation_q = qmul(local, qconj(s["local"]))
                roll = twist_about(deviation_q, qrot(s["local"], s["u"]))
                local = qmul(qmul(deviation_q, qconj(roll)), s["local"])
            gain = sim_gain(s["name"])
            if gain < 1.0:
                # How far does this accessory leave its rigid pose? Keep that rotation's
                # axis, scale its angle. gain 0.5 = half the swing, gain 0 = rigid.
                deviation_q = qmul(local, qconj(s["local"]))
                local = qmul(scale_quat_angle(deviation_q, gain), s["local"])
            local = qnorm(local)
            local_rot[i] = local
            comp_rot_frame[i] = qmul(prot, local)
            out_bones[s["name"]].append([round(c, 6) for c in local])
            worst_secondary_deviation = max(worst_secondary_deviation, deviation)

        # Twist helpers: a share of the limb's twist about the limb axis, taken
        # from the driven limb bone's delta. Applied as a rotation *about the
        # axis* rather than a fraction of the whole delta, otherwise the limb
        # would bend in the middle instead of twisting.
        for item in twist_bones:
            index = item["index"]
            arm_index = item["arm_index"]
            if arm_index < 0 or comp_rot_frame[arm_index] is None:
                continue
            base = rest_crot[arm_index]
            delta_world = qmul(comp_rot_frame[arm_index], qconj(base))
            delta_local = qmul(qmul(qconj(base), delta_world), base)
            u = item["axis"]
            angle = 2.0 * math.atan2(
                sum(delta_local[k] * u[k] for k in range(3)), delta_local[3])
            # Clamped hard on purpose: an unclamped share measured up to 164 deg,
            # which would spin the forearm. A twist helper only ever needs a small
            # correction, and a bounded one cannot wreck the limb.
            angle = max(-0.35, min(0.35, angle * item["weight"]))
            share = q_from_axis_angle(u, angle)
            local = qmul(share, rest_local[index])
            local_rot[index] = qnorm(local)
            parent = item["parent"]
            comp_rot_frame[index] = qmul(
                comp_rot_frame[parent] if parent >= 0 and
                comp_rot_frame[parent] is not None else (0.0, 0.0, 0.0, 1.0),
                local_rot[index])
            out_bones[item["target"]].append(
                [round(c, 6) for c in local_rot[index]])

    # Every bone gets a track. A bone the file does not drive at all is left to the
    # runtime's fallback for undriven bones, and that produced pieces pinned in the
    # scene: the accessory spring needs a *child offset* to know which way a bone
    # points, so it skips leaf bones entirely and those ends were never driven -- the
    # tip of every hair chain, the earring ring hanging off an ear chain. Emitting the
    # bone's reference local rotation as a constant track makes it follow its parent
    # rigidly instead (same trick as the static palm bone in the finger chain).
    rigid_tracks = 0
    rigid_names = []
    for b in bones:
        name = b["name"] or ""
        if not name or name in out_bones or name in mapped_targets:
            continue
        if b["index"] not in rigid and not args.track_untracked:
            continue
        out_bones[name] = [[round(c, 6) for c in rest_local[b["index"]]]
                           for _ in out_frames]
        rigid_names.append(name)
        rigid_tracks += 1

    track_names = set(tracks)
    mapped_mmd = {s for m in table for s in m["sources"]}
    unmapped_tracks = sorted(n for n in track_names if n not in mapped_mmd)

    doc = {
        "schemaVersion": 1,
        "kind": "better-pose-motion",
        "source": {"vmd": args.vmd, "pmx": args.pmx,
                   "skeleton": args.skeleton,
                   "skeletonBasis": skel_doc.get("basis"),
                   "vmdModel": vmd_doc.get("modelName")},
        "targetMesh": skel_doc.get("mesh"),
        "rootBone": "Bip001" if root else "",
        "fps": args.fps,
        "nativeFps": native_fps,
        "firstFrame": frame_lo,
        "frameCount": len(out_frames),
        "unitScaleCmPerMmdUnit": round(scale, 4),
        "mmdLegLength": round(mm_leg, 6),
        "rootTranslationUnit": "mmd",
        "axisChange": {"quaternion": list(A_AXIS),
                       "description": "+90 deg about X (MMD Y-up/Z-forward -> "
                                      "Bip001 left=+X, up=+Z, forward=+Y)"},
        "rest": {m["target"]: [round(c, 6) for c in rest_local[skel[m["target"]]["index"]]]
                 for m in table},
        "bones": out_bones,
        "rootTranslation": out_pos,
        "boneOffsets": out_offsets,
        "diagnostics": {
            "mappedBones": len(table),
            "directionCorrected": dir_ok,
            "directionSkipped": dir_skipped,
            "unmappedVmdTracks": unmapped_tracks,
            "worstUnitError": worst_unit,
            "secondaryBones": len(secondary),
            "rigidBones": rigid_names,
            "secondaryStiffness": args.sim_stiffness,
            "secondaryDamping": args.sim_damping,
            "secondaryGravity": args.sim_gravity,
            "worstSecondaryDeviationDeg": round(worst_secondary_deviation, 2),
        },
    }
    json.dump(doc, open(args.out, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"frames {len(out_frames)} ({frame_lo}..{out_frames[-1]}) at {args.fps} fps")
    print(f"mapped bones {len(table)}; direction-corrected {len(dir_ok)}, "
          f"skipped {len(dir_skipped)}: {dir_skipped}")
    if dir_bogus:
        print(f"direction reference rejected as bogus ({len(dir_bogus)}): "
              + ", ".join(f"{name}({value:.0f}deg)" for name, _, value in dir_bogus))
    for side_en, angle, before, after, in_plane, sb, sa in hand_plane_fixes:
        print(f"hand {side_en}: axis twist {angle:+6.1f} deg "
              f"(palm normal {before:5.1f} -> {after:5.1f}), "
              f"in-plane {in_plane:+6.1f} deg (finger spread {sb:5.1f} -> {sa:5.1f})")
    print(f"mmd->cm scale {scale:.4f} (expect ~8.0); worst |q| error {worst_unit:.2e}")
    lag_samples.sort()
    lag_p95 = lag_samples[int(len(lag_samples) * 0.95)] if lag_samples else 0.0
    lag_mean = (sum(lag_samples) / len(lag_samples)) if lag_samples else 0.0
    print(f"secondary bones simulated: {len(secondary)} "
          f"(stiffness {args.sim_stiffness}, damping {args.sim_damping}, "
          f"gravity {args.sim_gravity})")
    print(f"  lag mean {lag_mean:.1f} deg, p95 {lag_p95:.1f} deg, "
          f"max {worst_secondary_deviation:.1f} deg, "
          f"clamped {clamped}/{len(lag_samples)}")
    print(f"unmapped VMD tracks ({len(unmapped_tracks)}): {unmapped_tracks[:12]} ...")
    print(f"root source: {root_source or '(none)'} "
          f"(chain {len(root_chain)} bone(s): "
          f"{', '.join(name for name, _ in named_chain[:root_end + 1])})")
    if ik_gaps:
        # Evidence, not decoration: each chain reports the effector-to-IK-bone distance
        # before and after the solve, so "the legs follow" is a number and not a feeling.
        by_chain = {}
        for name, before, after, applied in ik_gaps:
            entry = by_chain.setdefault(name, [0.0, 0.0, 0.0, 1e9])
            entry[0] = max(entry[0], before)
            entry[1] = max(entry[1], after)
            entry[2] = max(entry[2], applied)
            entry[3] = min(entry[3], after)
        print(f"IK chains solved: {len(by_chain)}; effector gap before -> after "
              f"(mmd units; worst / best after), worst solver rotation:")
        for name, (before, after, applied, best) in sorted(by_chain.items()):
            print(f"  {name}: {before:7.3f} -> {after:7.3f} (best {best:6.3f})   "
                  f"solver {applied:6.1f} deg")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
