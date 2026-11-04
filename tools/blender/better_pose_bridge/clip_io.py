"""Pure .bpmotion reader/writer used by the Blender add-on and its checks."""

import json
import math
from pathlib import Path


FORMAT = "betterpose-blender-motion"
VERSION = 1
MAX_BYTES = 256 * 1024 * 1024
MAX_BONES = 4096
MAX_FRAMES = 108001


def _number(value, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("文件中的数值格式无效")
    if not math.isfinite(value) or value < low or value > high:
        raise ValueError("文件中的数值超出支持范围")
    return float(value)


def _vec(value, size, low=-10000.0, high=10000.0):
    if not isinstance(value, list) or len(value) != size:
        raise ValueError("文件中的向量格式无效")
    return [_number(item, low, high) for item in value]


def _frame(item, bone_count):
    if not isinstance(item, dict):
        raise ValueError("动作帧格式无效")
    time = _number(item.get("time"), 0.0, 86400.0)
    root = _vec(item.get("root", [0, 0, 0]), 3)
    bones = item.get("bones", [])
    if not isinstance(bones, list) or len(bones) > bone_count:
        raise ValueError("动作帧骨骼数量无效")
    seen = set()
    clean_bones = []
    for bone in bones:
        if not isinstance(bone, dict) or type(bone.get("i")) is not int:
            raise ValueError("动作帧骨骼索引无效")
        index = bone["i"]
        if index < 0 or index >= bone_count or index in seen:
            raise ValueError("动作帧骨骼索引重复或越界")
        seen.add(index)
        position = None
        if "p" in bone:
            position = _vec(bone["p"], 3, -1000000.0, 1000000.0)
        if "q" in bone:
            q = _vec(bone["q"], 4, -1.01, 1.01)
            if not 0.5 <= sum(value * value for value in q) <= 1.5:
                raise ValueError("动作帧四元数无效")
            clean = {"i": index, "q": q}
        else:
            clean = {"i": index, "r": _vec(bone.get("r", [0, 0, 0]), 3, -360, 360)}
        if position is not None:
            clean["p"] = position
        clean_bones.append(clean)
    faces = item.get("faces", {})
    if not isinstance(faces, dict) or len(faces) > 4096:
        raise ValueError("表情轨道格式无效")
    clean_faces = {}
    for name, weight in faces.items():
        if not isinstance(name, str) or not name or len(name) > 192:
            raise ValueError("表情名称无效")
        clean_faces[name] = _number(weight, 0, 1)
    camera = item.get("camera")
    clean_camera = None
    if camera is not None:
        if not isinstance(camera, dict):
            raise ValueError("镜头轨道格式无效")
        clean_camera = {
            "focus": _vec(camera.get("focus", [0, 0, 0]), 3),
            "yaw": _number(camera.get("yaw", 0), -360, 360),
            "pitch": _number(camera.get("pitch", 0), -89.9, 89.9),
            "distance": _number(camera.get("distance", 300), 0.01, 100000),
            "fov": _number(camera.get("fov", 0), 0, 179),
        }
    return {"time": time, "root": root, "bones": clean_bones, "faces": clean_faces,
            "camera": clean_camera}


def _skeleton_metadata(document, bone_count):
    """Keep optional rest data used by the Blender bridge."""
    result = {}
    positions = document.get("bone_positions")
    if isinstance(positions, list) and len(positions) == bone_count:
        result["bone_positions"] = [_vec(value, 3, -1000000.0, 1000000.0) for value in positions]
    for key in ("bone_rotations", "bone_component_rotations"):
        values = document.get(key)
        if not isinstance(values, list) or len(values) != bone_count:
            continue
        cleaned = []
        for value in values:
            q = _vec(value, 4, -1.01, 1.01)
            if not 0.5 <= sum(item * item for item in q) <= 1.5:
                raise ValueError("参考骨骼四元数无效")
            cleaned.append(q)
        result[key] = cleaned
    space = document.get("bone_position_space")
    if space in ("local", "component"):
        result["bone_position_space"] = space
    position_mode = document.get("bone_position_mode")
    if position_mode in ("absolute", "delta"):
        result["bone_position_mode"] = position_mode
    indices = document.get("game_indices")
    if isinstance(indices, list) and len(indices) == bone_count and all(type(item) is int and item >= 0 for item in indices):
        result["game_indices"] = list(indices)
    for key in ("game_bone_names", "game_bone_parents"):
        values = document.get(key)
        if key.endswith("names"):
            if isinstance(values, list) and all(isinstance(item, str) and item for item in values):
                result[key] = list(values)
        elif isinstance(values, list) and all(type(item) is int and item >= -1 for item in values):
            result[key] = list(values)
    faces = document.get("face_names")
    if (isinstance(faces, list) and len(faces) <= 4096 and
            all(isinstance(item, str) and item and len(item) <= 192 for item in faces)):
        result["face_names"] = list(faces)
    return result


def _camera_track(document):
    source = document.get("camera_track")
    if source is None:
        return None
    if isinstance(source, list):
        source = {"kind": "mmd", "fps": 30, "current_time": 0,
                  "keys": source}
    if not isinstance(source, dict):
        raise ValueError("原始镜头轨道格式无效")
    keys = source.get("keys")
    if not isinstance(keys, list) or not 1 <= len(keys) <= MAX_FRAMES:
        raise ValueError("原始镜头关键帧数量无效")
    clean_keys = []
    previous = -1
    for item in keys:
        if not isinstance(item, dict) or type(item.get("frame")) is not int:
            raise ValueError("原始镜头关键帧格式无效")
        frame = item["frame"]
        if frame < 0 or frame <= previous:
            raise ValueError("原始镜头关键帧必须递增")
        curves = item.get("curves", [])
        if not isinstance(curves, list) or len(curves) != 6:
            raise ValueError("原始镜头贝塞尔曲线无效")
        clean_curves = []
        for curve in curves:
            if not isinstance(curve, dict):
                raise ValueError("原始镜头贝塞尔曲线无效")
            clean_curves.append({
                "x1": _number(curve.get("x1", 0), 0, 1),
                "y1": _number(curve.get("y1", 0), 0, 1),
                "x2": _number(curve.get("x2", 1), 0, 1),
                "y2": _number(curve.get("y2", 1), 0, 1),
            })
        clean_keys.append({
            "frame": frame,
            "distance": _number(item.get("distance"), -100000, 100000),
            "position": _vec(item.get("position"), 3, -100000, 100000),
            "rotation": _vec(item.get("rotation"), 3, -10000, 10000),
            "curves": clean_curves,
            "view_angle": _number(item.get("view_angle"), 0, 179),
            "perspective": bool(item.get("perspective", True)),
        })
        previous = frame
    return {
        "kind": "mmd",
        "fps": _number(source.get("fps", 30), 1, 120),
        "current_time": _number(source.get("current_time", 0), 0, 86400),
        "keys": clean_keys,
    }


def read(path):
    path = Path(path)
    if path.stat().st_size > MAX_BYTES:
        raise ValueError("动作文件超过 256 MB")
    with path.open("r", encoding="utf-8-sig") as stream:
        document = json.load(stream)
    if document.get("format") != FORMAT or document.get("version") != VERSION:
        raise ValueError("请选择 BetterPose .bpmotion 文件")
    names = document.get("bone_names")
    parents = document.get("bone_parents")
    if not isinstance(names, list) or not 1 <= len(names) <= MAX_BONES:
        raise ValueError("骨骼数量无效")
    if not isinstance(parents, list) or len(parents) != len(names):
        raise ValueError("骨骼父级列表无效")
    for index, (name, parent) in enumerate(zip(names, parents)):
        if not isinstance(name, str) or not name or len(name) > 256:
            raise ValueError("骨骼名称无效")
        if type(parent) is not int or parent < -1 or parent >= index:
            raise ValueError("骨骼父级关系无效")
    frames = document.get("frames")
    if not isinstance(frames, list) or not 1 <= len(frames) <= MAX_FRAMES:
        raise ValueError("动作帧数无效")
    clean_frames = []
    previous = -1.0
    for item in frames:
        clean = _frame(item, len(names))
        if clean["time"] <= previous:
            raise ValueError("动作时间轴必须从零开始并严格递增")
        if previous < 0 and clean["time"] != 0:
            raise ValueError("动作时间轴必须从零开始")
        previous = clean["time"]
        clean_frames.append(clean)
    fps = _number(document.get("fps", 30), 1, 120)
    result = {"format": FORMAT, "version": VERSION, "model": str(document.get("model", "")),
              "fps": fps, "bone_names": names, "bone_parents": parents,
              "frames": clean_frames, "length": _number(document.get("length", 0), 0, 30 * 60 * 30)}
    result.update(_skeleton_metadata(document, len(names)))
    raw_camera = _camera_track(document)
    if raw_camera is not None:
        result["camera_track"] = raw_camera
    return result


def write(path, document):
    # Round-trip through the same validator before touching the destination.
    clean = read_document(document)
    Path(path).write_text(json.dumps(clean, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_document(document):
    if not isinstance(document, dict):
        raise ValueError("动作文件根节点无效")
    # Keep the validation path identical to read() without requiring a temporary file.
    names = document.get("bone_names", [])
    frames = document.get("frames", [])
    if document.get("format") != FORMAT or document.get("version") != VERSION:
        raise ValueError("动作格式或版本不受支持")
    if not isinstance(names, list) or not 1 <= len(names) <= MAX_BONES:
        raise ValueError("骨骼数量无效")
    parents = document.get("bone_parents")
    if not isinstance(parents, list) or len(parents) != len(names):
        raise ValueError("骨骼父级列表无效")
    for index, (name, parent) in enumerate(zip(names, parents)):
        if not isinstance(name, str) or not name or len(name) > 256 or type(parent) is not int or parent < -1 or parent >= index:
            raise ValueError("骨骼名称或父级关系无效")
    if not isinstance(frames, list) or not 1 <= len(frames) <= MAX_FRAMES:
        raise ValueError("动作帧数无效")
    clean_frames = []
    previous = -1.0
    for item in frames:
        clean = _frame(item, len(names))
        if clean["time"] <= previous or (previous < 0 and clean["time"] != 0):
            raise ValueError("动作时间轴必须从零开始并严格递增")
        previous = clean["time"]
        clean_frames.append(clean)
    result = {"format": FORMAT, "version": VERSION, "model": str(document.get("model", "")),
              "fps": _number(document.get("fps", 30), 1, 120),
              "bone_names": list(names), "bone_parents": list(parents),
              "frames": clean_frames,
              "length": _number(document.get("length", 0), 0, 30 * 60 * 30)}
    result.update(_skeleton_metadata(document, len(names)))
    raw_camera = _camera_track(document)
    if raw_camera is not None:
        result["camera_track"] = raw_camera
    return result
