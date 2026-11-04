bl_info = {
    "name": "BetterPose Blender Bridge",
    "author": "Anomaly",
    "version": (1, 3, 4),
    "blender": (3, 0, 0),
    "location": "View3D > Sidebar > BetterPose",
    "category": "Animation",
}

import math
import json
import time
from pathlib import Path

import bpy
from mathutils import Euler, Matrix, Vector, Quaternion
from bpy.props import StringProperty, PointerProperty
from bpy_extras.io_utils import ImportHelper, ExportHelper

from . import clip_io
from .transport import Client


_client = None
_session = 0
_sequence = 0
_connected = False
_pull_document = None
_pull_next = 0.0
_pull_fps = 30.0
_pull_length = 0.0
_pull_batch = 64
_last_sync_frame = None
_live_cache = {}
_last_sync_time = 0.0
_last_status_time = 0.0
_document_cache = {}
_last_packet_signature = None
_last_heartbeat = 0.0
_camera_baseline_scene = 0
_camera_baseline_data = None
# UE vectors are mapped to Blender as (x, y, z) -> (y, -x, z), a -90°
# rotation around Z.  Quaternion's constructor is WXYZ; the previous value
# was accidentally written as XYZW and introduced an unintended half-turn.
def _ue_to_blender_vector(value):
    # NTE's Bip001 reference uses +X for the character's left, +Y for
    # forward, and +Z for up. Blender's front-view X points to the character's
    # right, so this is the required spatial reflection.
    return [float(-value[0]), float(value[1]), float(value[2])]


def _blender_to_ue_vector(value):
    return [float(-value[0]), float(value[1]), float(value[2])]


def _ue_to_blender_quaternion(value):
    q = Quaternion((value[3], value[0], value[1], value[2]))
    converted = Quaternion((q.w, q.x, -q.y, -q.z))
    return [converted.x, converted.y, converted.z, converted.w]


def _blender_to_ue_quaternion(value):
    q = Quaternion((value[3], value[0], value[1], value[2]))
    converted = Quaternion((q.w, q.x, -q.y, -q.z))
    return [converted.x, converted.y, converted.z, converted.w]


def _reflect_game_quaternion(q):
    return Quaternion((q.w, q.x, -q.y, -q.z))


def _transport():
    global _client
    if _client is None:
        _client = Client()
    return _client


def _set_status(text):
    for scene in bpy.data.scenes:
        scene.bp_status = text


def _scene_document(scene, armature):
    names = [bone.name for bone in armature.data.bones]
    parents = [names.index(bone.parent.name) if bone.parent else -1 for bone in armature.data.bones]
    return {"format": clip_io.FORMAT, "version": clip_io.VERSION, "model": scene.get("betterpose_model", ""),
            "fps": 30, "length": 30, "bone_names": names, "bone_parents": parents,
            "frames": [{"time": 0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None}]}


def _frame_packet(scene):
    global _sequence
    # The live timer can run between a timeline drag and Blender's deferred
    # dependency-graph evaluation. Sampling matrix_world in that window sends
    # the new frame's pose with the previous frame's camera, which stays offset
    # until playback forces another evaluation.
    bpy.context.view_layer.update()
    armature = _armature(scene)
    camera = (_camera_key(scene, scene.camera)
              if scene.bp_sync_camera else None)
    if not armature and camera is None:
        return None
    bones = []
    if armature:
        document = _cached_armature_document(armature)
        if document:
            rotations, _ = _live_game_pose(armature, document)
            for index, rotation in rotations.items():
                if any(abs(value) > 1e-5 for value in rotation):
                    game_index = armature.pose.bones[index].bone.get("betterpose_game_index", index)
                    bones.append({"i": int(game_index), "r": rotation})
        else:
            for index, bone in enumerate(armature.pose.bones):
                rotation = [math.degrees(value) for value in bone.rotation_euler]
                if any(abs(value) > 1e-5 for value in rotation):
                    game_index = bone.bone.get("betterpose_game_index", index)
                    bones.append({"i": int(game_index), "r": rotation})
    _sequence += 1
    packet = {"session": _session, "sequence": _sequence,
              "time": max(0.0, (scene.frame_current - scene.frame_start) /
                           max(scene.render.fps / scene.render.fps_base, 1.0)),
              "root": (_blender_to_ue_vector([value * 100.0 for value in armature.location])
                       if armature else [0.0, 0.0, 0.0]),
              "bones": bones}
    packet["camera"] = camera
    faces = {}
    if armature:
        for name in armature.get("betterpose_face_names", []):
            key = "betterpose.face." + name
            if key in armature:
                faces[name] = max(0.0, min(1.0, float(armature[key])))
    packet["faces"] = faces
    return packet


def _packet_signature(packet):
    camera = packet.get("camera")
    camera_key = None if not camera else (
        tuple(round(float(v), 6) for v in camera.get("focus", [])),
        round(float(camera.get("yaw", 0.0)), 6),
        round(float(camera.get("pitch", 0.0)), 6),
        round(float(camera.get("distance", 0.0)), 6),
        round(float(camera.get("fov", 0.0)), 6),
        tuple(round(float(v), 6) for v in camera.get("position", [])),
        tuple(round(float(v), 6)
              for v in camera.get("relative_position", [])),
        tuple(round(float(v), 6) for v in camera.get("forward", [])),
        tuple(round(float(v), 6) for v in camera.get("look_forward", [])),
        tuple(round(float(v), 6) for v in camera.get("rotation", [])),
        tuple(round(float(v), 6) for v in camera.get("delta_position", [])),
        tuple(round(float(v), 6) for v in camera.get("delta_rotation", [])),
        tuple(round(float(v), 6)
              for v in camera.get("motion_delta_position", [])),
        tuple(round(float(v), 6)
              for v in camera.get("motion_delta_rotation", [])),
        tuple(round(float(v), 6)
              for v in camera.get("focus_delta_rotation", [])),
        round(float(camera.get("lens_scale", 1.0)), 6),
    )
    return (round(float(packet["time"]), 6),
            tuple(round(float(v), 6) for v in packet["root"]),
            tuple((int(item["i"]), tuple(round(float(v), 6) for v in item["r"]))
                  for item in packet["bones"]),
            tuple((name, round(float(value), 6))
                  for name, value in sorted(packet.get("faces", {}).items())),
            camera_key)


def _transfer_pose_animation(scene, source, target):
    source_data = source.animation_data if source else None
    if source_data is None:
        return
    _leave_layer_tweak(scene)
    action = source_data.action
    action_blend = source_data.action_blend_type
    action_extrapolation = source_data.action_extrapolation
    action_influence = source_data.action_influence
    tracks = []
    for track in source_data.nla_tracks:
        tracks.append((
            track.name,
            track.mute,
            [(
                strip.name,
                strip.action,
                int(round(strip.frame_start)),
                strip.blend_type,
                strip.extrapolation,
                strip.influence,
            ) for strip in track.strips],
        ))
    properties = {
        key: source[key]
        for key in ("betterpose_base_action", "betterpose_correction_action")
        if key in source
    }
    actions = {action} if action is not None else set()
    actions.update(
        strip_action
        for _, _, strips in tracks
        for _, strip_action, _, _, _, _ in strips
        if strip_action is not None
    )
    quaternion = any(
        curve.data_path.endswith("rotation_quaternion")
        for source_action in actions
        for curve in source_action.fcurves
    )
    animation_data = target.animation_data_create()
    for bone in target.pose.bones:
        bone.rotation_mode = "QUATERNION" if quaternion else "XYZ"
    for track_name, muted, strips in tracks:
        track = animation_data.nla_tracks.new()
        track.name = track_name
        track.mute = muted
        for strip_name, strip_action, start, blend, extrapolation, influence in strips:
            if strip_action is None:
                continue
            strip = track.strips.new(strip_name, start, strip_action)
            strip.blend_type = blend
            strip.extrapolation = extrapolation
            strip.influence = influence
    if action is not None:
        animation_data.action = action
        animation_data.action_blend_type = action_blend
        animation_data.action_extrapolation = action_extrapolation
        animation_data.action_influence = action_influence
    for key, value in properties.items():
        target[key] = value


def _remap_motion_document(source_document, skeleton_document):
    source_names = list(source_document.get("bone_names", []))
    target_names = list(skeleton_document.get("bone_names", []))
    target_by_name = {name: index for index, name in enumerate(target_names)}
    frames = []
    for frame in source_document.get("frames", []):
        remapped = dict(frame)
        bones = []
        for item in frame.get("bones", []):
            source_index = int(item.get("i", -1))
            if source_index < 0 or source_index >= len(source_names):
                continue
            target_index = target_by_name.get(source_names[source_index])
            if target_index is None:
                continue
            mapped = dict(item)
            mapped["i"] = target_index
            bones.append(mapped)
        remapped["bones"] = bones
        frames.append(remapped)
    result = dict(skeleton_document)
    result["model"] = skeleton_document.get("model", "")
    result["fps"] = source_document.get("fps", skeleton_document.get("fps", 30))
    result["length"] = source_document.get(
        "length", skeleton_document.get("length", 0))
    result["frames"] = frames
    result["face_names"] = list(dict.fromkeys(
        list(skeleton_document.get("face_names", [])) +
        list(source_document.get("face_names", []))))
    source_camera = _camera_track_document(
        source_document.get("camera_track"))
    if source_camera is not None:
        result["camera_track"] = source_camera
    return result


def _on_begin(result):
    global _session, _connected, _sequence, _last_sync_frame, _last_sync_time
    global _last_status_time, _last_packet_signature, _last_heartbeat
    if not result.get("ok"):
        _set_status("连接失败：" + result.get("error", "未知错误"))
        return
    _session = int(result.get("session", 0))
    _sequence = 0
    _last_sync_frame = None
    _last_sync_time = 0.0
    _last_status_time = 0.0
    _last_packet_signature = None
    _last_heartbeat = 0.0
    scene = bpy.context.scene
    source_bones = [item for item in result.get("bones", []) if item.get("editable", False)]
    if not source_bones:
        # A camera-only session does not need a game skeleton. Keep any
        # armature already present in Blender and send just the camera track.
        scene["betterpose_game_camera_loaded"] = bool(
            result.get("camera_loaded", False))
        scene.bp_live = False
        if "betterpose_camera_delta_baseline" in scene:
            del scene["betterpose_camera_delta_baseline"]
        scene.bp_sync_camera = scene.camera is not None
        _capture_camera_baseline(scene)
        scene.bp_live = True
        _connected = True
        _set_status("已连接 BetterPose；仅同步 Blender 镜头")
        return
    source_to_compact = {item["i"]: compact for compact, item in enumerate(source_bones)}
    compact_names = [item["name"] for item in source_bones]
    compact_parents = []
    for item in source_bones:
        parent = item.get("parent", -1)
        while parent >= 0 and parent not in source_to_compact:
            parent = result.get("bones", [])[parent].get("parent", -1)
        compact_parents.append(source_to_compact.get(parent, -1))
    document = {"format": clip_io.FORMAT, "version": clip_io.VERSION,
                "model": result.get("model", ""), "fps": result.get("fps", 30),
                "length": result.get("length", 30),
                "bone_names": compact_names,
                "bone_parents": compact_parents,
                "bone_positions": [_ue_to_blender_vector(item.get("p", [0, 0, 0])) for item in source_bones],
                "bone_rotations": [_ue_to_blender_quaternion(item.get("lq", item.get("q", [0, 0, 0, 1]))) for item in source_bones],
                "bone_component_rotations": [_ue_to_blender_quaternion(item.get("q", [0, 0, 0, 1])) for item in source_bones],
                "bone_position_space": result.get("bone_position_space", "local"),
                "game_indices": [item["i"] for item in source_bones],
                "face_names": result.get("faces", []),
                "frames": [{"time": 0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None}]}
    if not document["bone_names"]:
        document["bone_names"] = [item["name"] for item in result.get("bones", [])]
        document["bone_parents"] = [item.get("parent", -1) for item in result.get("bones", [])]
        document["bone_positions"] = [_ue_to_blender_vector(item.get("p", [0, 0, 0])) for item in result.get("bones", [])]
        document["bone_rotations"] = [_ue_to_blender_quaternion(item.get("lq", item.get("q", [0, 0, 0, 1]))) for item in result.get("bones", [])]
        document["bone_component_rotations"] = [_ue_to_blender_quaternion(item.get("q", [0, 0, 0, 1])) for item in result.get("bones", [])]
        document["game_indices"] = list(range(len(document["bone_names"])))
        document["face_names"] = result.get("faces", [])
    old = _armature(scene)
    old_names = [bone.name for bone in old.data.bones] if old else []
    migrated_motion = False
    # Keep an existing imported action when it contains the live body's bones.
    # A game-exported file may also contain helper/accessory bones, so requiring
    # an exact list match would silently delete the action on connect.
    reuse = (old is not None and
             len(old_names) == len(document["bone_names"]) and
             all(name in old_names for name in document["bone_names"]))
    if reuse:
        armature = old
        active_document = json.loads(armature.get("betterpose_document", "{}"))
        if not active_document.get("bone_names"):
            active_document = document
        active_names = list(active_document["bone_names"])
        active_count = len(active_names)
        live_by_name = {item["name"]: item for item in source_bones}
        for key, identity in (
                ("game_indices", None),
                ("bone_rotations", [0.0, 0.0, 0.0, 1.0]),
                ("bone_component_rotations", [0.0, 0.0, 0.0, 1.0])):
            values = active_document.get(key)
            if not isinstance(values, list) or len(values) != active_count:
                active_document[key] = (
                    list(range(active_count)) if identity is None
                    else [list(identity) for _ in range(active_count)])
        for index, name in enumerate(active_names):
            live = live_by_name.get(name)
            if live is None:
                continue
            active_document["game_indices"][index] = int(live["i"])
            active_document["bone_rotations"][index] = _ue_to_blender_quaternion(
                live.get("lq", live.get("q", [0, 0, 0, 1])))
            active_document["bone_component_rotations"][index] = (
                _ue_to_blender_quaternion(
                    live.get("q", [0, 0, 0, 1])))
        armature["betterpose_document"] = json.dumps(
            active_document, ensure_ascii=False)
    else:
        offline_document = None
        if old:
            old_document = json.loads(old.get("betterpose_document", "{}"))
            if (old_document.get("bone_names") and
                    (len(old_document["bone_names"]) !=
                     len(document["bone_names"]) or
                     old_document.get("camera_track"))):
                offline_document = old_document
        armature = _create_armature(document)
        if old:
            if offline_document is None:
                _transfer_pose_animation(scene, old, armature)
            bpy.data.objects.remove(old, do_unlink=True)
        if offline_document is not None:
            active_document = _remap_motion_document(
                offline_document, document)
            _apply_document(scene, armature, active_document)
            armature["betterpose_document"] = json.dumps(
                active_document, ensure_ascii=False)
            migrated_motion = True
        else:
            active_document = document
    _ensure_face_properties(
        armature,
        list(active_document.get("face_names", [])) +
        list(document.get("face_names", [])))
    scene.bp_armature = armature
    scene["betterpose_model"] = document["model"]
    if not reuse and not migrated_motion:
        armature["betterpose_document"] = json.dumps(document, ensure_ascii=False)
    scene["betterpose_game_indices"] = active_document.get(
        "game_indices", document.get("game_indices", []))
    live_by_name = {item["name"]: item for item in source_bones}
    for bone in armature.data.bones:
        live = live_by_name.get(bone.name)
        if live is not None:
            bone["betterpose_game_index"] = int(live["i"])
            bone["betterpose_game_rest_q"] = _ue_to_blender_quaternion(
                live.get("q", [0, 0, 0, 1]))
    _apply_edit_display(armature)
    scene.bp_live = False
    if "betterpose_camera_delta_baseline" in scene:
        del scene["betterpose_camera_delta_baseline"]
    scene.bp_sync_camera = scene.camera is not None
    _capture_camera_baseline(scene)
    scene.bp_live = True
    _connected = True
    scene["betterpose_game_camera_loaded"] = bool(
        result.get("camera_loaded", False))
    if result.get("camera_loaded") and result.get("camera_enabled"):
        _set_status("已连接 BetterPose；驱动镜头随 Blender 时间轴播放")
    else:
        _set_status("已连接 BetterPose；勾选实时同步到游戏")


def _on_command(result):
    if not result.get("ok"):
        _set_status("游戏请求失败：" + result.get("error", "未知错误"))
    elif "camera_loaded" in result:
        bpy.context.scene["betterpose_game_camera_loaded"] = bool(
            result.get("camera_loaded"))


def _finish_pull():
    global _pull_document
    scene = bpy.context.scene
    old = _armature(scene)
    if old:
        bpy.data.objects.remove(old, do_unlink=True)
    armature = _create_armature(_pull_document)
    scene.bp_armature = armature
    scene["betterpose_model"] = _pull_document["model"]
    armature["betterpose_document"] = json.dumps(_pull_document, ensure_ascii=False)
    scene["betterpose_game_indices"] = _pull_document.get("game_indices", [])
    game_indices = _pull_document.get("game_indices", [])
    game_rotations = _pull_document.get("bone_rotations", [])
    component_rotations = _pull_document.get("bone_component_rotations", [])
    for index, bone in enumerate(armature.data.bones):
        bone["betterpose_game_index"] = game_indices[index] if index < len(game_indices) else index
        bone["betterpose_game_rest_q"] = component_rotations[index] if index < len(component_rotations) else [0, 0, 0, 1]
    _apply_document(scene, armature, _pull_document)
    _set_status("已拉取当前 MMD 动作到 Blender，可继续编辑")


def _pull_chunk(result):
    global _pull_next, _pull_length
    if not result.get("ok"):
        _set_status("拉取 MMD 失败：" + result.get("error", "未知错误"))
        return
    if result.get("length") is not None:
        _pull_length = max(_pull_length, float(result.get("length", 0.0)))
    game_to_compact = {game: index for index, game in enumerate(_pull_document.get("game_indices", []))}
    frames = []
    for raw in result.get("frames", []):
        frame = dict(raw)
        frame["bones"] = []
        for item in raw.get("bones", []):
            compact = game_to_compact.get(item.get("i"))
            if compact is None:
                continue
            mapped = dict(item)
            mapped["i"] = compact
            frame["bones"].append(mapped)
        frames.append(frame)
    _pull_document["frames"].extend(frames)
    if frames:
        _pull_next = float(frames[-1]["time"]) + 1.0 / _pull_fps
    if _pull_next < _pull_length - 1e-5:
        _transport().request("sample", {"session": _session, "start": _pull_next,
                                         "count": _pull_batch, "fps": _pull_fps}, _pull_chunk)
        _set_status("正在拉取 MMD 动作…")
    else:
        _pull_document["length"] = _pull_length * _pull_fps
        _transport().request("end", {"session": _session}, _on_command)
        _finish_pull()


def _on_pull_begin(result):
    global _session, _pull_document, _pull_next, _pull_fps, _pull_length
    if not result.get("ok"):
        _set_status("拉取 MMD 失败：" + result.get("error", "未知错误"))
        return
    _session = int(result["session"])
    _pull_fps = 30.0
    _pull_length = 0.0
    _pull_next = 0.0
    bones = result.get("bones", [])
    editable = [item for item in bones if item.get("editable", False)]
    camera_track = _camera_track_document(result.get("camera_track"))
    _pull_document = {"format": clip_io.FORMAT, "version": clip_io.VERSION,
                      "model": result.get("model", ""), "fps": _pull_fps,
                      "length": 0, "bone_names": [item["name"] for item in editable],
                      "bone_parents": [-1 for _ in editable],
                      "game_bone_names": [item["name"] for item in bones],
                      "game_bone_parents": [item.get("parent", -1) for item in bones],
                      "bone_positions": [_ue_to_blender_vector(item.get("p", [0, 0, 0])) for item in editable],
                      "bone_rotations": [_ue_to_blender_quaternion(item.get("lq", item.get("q", [0, 0, 0, 1]))) for item in editable],
                      "bone_component_rotations": [_ue_to_blender_quaternion(item.get("q", [0, 0, 0, 1])) for item in editable],
                      "bone_position_space": result.get("bone_position_space", "component"),
                      "game_indices": [item["i"] for item in editable],
                      "face_names": result.get("faces", []),
                      "camera_track": camera_track,
                      "frames": []}
    source_index = {item["i"]: index for index, item in enumerate(editable)}
    for index, item in enumerate(editable):
        parent = item.get("parent", -1)
        _pull_document["bone_parents"][index] = source_index.get(parent, -1)
    _transport().request("sample", {"session": _session, "start": 0.0, "count": _pull_batch,
                                     "fps": _pull_fps}, _pull_chunk)
    _set_status("正在拉取 MMD 动作…")


def _timer():
    global _connected, _last_sync_frame, _last_sync_time, _last_status_time
    global _last_packet_signature, _last_heartbeat
    try:
        if _client:
            _client.poll()
        scene = bpy.context.scene
        if _connected and scene.bp_live:
            playing = bool(bpy.context.screen and bpy.context.screen.is_animation_playing)
            frame_key = (scene.frame_current, round(scene.frame_subframe, 4))
            now = time.monotonic()
            if playing and frame_key == _last_sync_frame:
                return 1.0 / 60.0
            packet = _frame_packet(scene)
            if packet:
                _last_sync_frame = frame_key if playing else None
                _last_sync_time = now
                signature = _packet_signature(packet)
                if signature == _last_packet_signature:
                    if now - _last_heartbeat >= 1.0:
                        _last_heartbeat = now
                        _transport().request(
                            "ping", {"session": _session}, _on_command)
                    return 1.0 / 60.0
                _last_packet_signature = signature
                _last_heartbeat = now
                if now - _last_status_time >= 0.25:
                    scene.bp_status = (f"实时同步中：帧 {scene.frame_current}，"
                                       f"骨骼 {len(packet['bones'])}")
                    _last_status_time = now
                _transport().preview(packet, _on_command)
    except Exception as exc:
        _set_status("实时同步失败：" + str(exc))
    return 1.0 / 60.0


def _armature(scene):
    return scene.bp_armature


def _ensure_face_properties(armature, names):
    unique = list(dict.fromkeys(str(name) for name in names if str(name)))
    armature["betterpose_face_names"] = unique
    for name in unique:
        key = "betterpose.face." + name
        if key not in armature:
            armature[key] = 0.0
        try:
            armature.id_properties_ui(key).update(
                min=0.0, max=1.0, soft_min=0.0, soft_max=1.0,
                description=name, default=0.0)
        except (AttributeError, TypeError):
            pass


def _cached_armature_document(armature):
    text = armature.get("betterpose_document", "")
    key = (id(armature), text)
    cached = _document_cache.get(key)
    if cached is None:
        cached = json.loads(text) if text else None
        _document_cache.clear()
        _document_cache[key] = cached
    return cached


def _create_armature(document):
    armature_data = bpy.data.armatures.new("BetterPose Skeleton")
    armature = bpy.data.objects.new("BetterPose Skeleton", armature_data)
    bpy.context.collection.objects.link(armature)
    bpy.context.view_layer.objects.active = armature
    armature.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = []
    positions = document.get("bone_positions") or [[0, 0, 0] for _ in document["bone_names"]]
    if len(positions) != len(document["bone_names"]):
        positions = [[0, 0, 0] for _ in document["bone_names"]]
    component_space = document.get("bone_position_space") == "component"
    world_positions = [Vector(position) * 0.01 for position in positions]
    if not component_space:
        world_positions = [None] * len(document["bone_names"])

        def resolve_position(index, visiting=None):
            if world_positions[index] is not None:
                return world_positions[index]
            visiting = set() if visiting is None else visiting
            if index in visiting:
                raise ValueError("骨架父级关系存在循环")
            visiting.add(index)
            parent = document["bone_parents"][index]
            local = Vector(positions[index]) * 0.01
            world_positions[index] = (resolve_position(parent, visiting) + local
                                      if parent >= 0 else local)
            visiting.remove(index)
            return world_positions[index]

        for index in range(len(world_positions)):
            resolve_position(index)
    for index, name in enumerate(document["bone_names"]):
        bone = armature_data.edit_bones.new(name)
        parent = document["bone_parents"][index]
        world = world_positions[index]
        bone.head = world
        children = [child for child, candidate in enumerate(document["bone_parents"])
                    if candidate == index]
        if name == "Root":
            bone.tail = world + Vector((0, 0, 0.1))
        elif children:
            tail = world_positions[children[0]]
            if (tail - world).length > 1e-4:
                bone.tail = tail
            else:
                bone.tail = world + Vector((0, 0, 0.1))
        elif parent >= 0:
            direction = world - world_positions[parent]
            bone.tail = world + (direction.normalized() * 0.1 if direction.length > 1e-4
                                  else Vector((0, 0, 0.1)))
        else:
            bone.tail = world + Vector((0, 0, 0.1))
        edit_bones.append(bone)
    for index, bone in enumerate(edit_bones):
        parent = document["bone_parents"][index]
        if 0 <= parent < len(edit_bones):
            bone.parent = edit_bones[parent]
    # Blender interprets edit-bone coordinates in the parent space after a
    # parent is assigned.  Write the component-space geometry once more so a
    # child does not collapse to its parent's origin when the input order is
    # not a strict parent-before-child order.
    for index, bone in enumerate(edit_bones):
        world = world_positions[index]
        bone.head = world
        children = [child for child, candidate in enumerate(document["bone_parents"])
                    if candidate == index]
        if document["bone_names"][index] == "Root":
            bone.tail = world + Vector((0, 0, 0.1))
        elif children:
            tail = world_positions[children[0]]
            bone.tail = (tail if (tail - world).length > 1e-4
                         else world + Vector((0, 0, 0.1)))
        elif document["bone_parents"][index] >= 0:
            direction = world - world_positions[document["bone_parents"][index]]
            bone.tail = world + (direction.normalized() * 0.1
                                 if direction.length > 1e-4 else Vector((0, 0, 0.1)))
        else:
            bone.tail = world + Vector((0, 0, 0.1))
    bpy.ops.object.mode_set(mode="POSE")
    for bone in armature.pose.bones:
        bone.rotation_mode = "XYZ"
    bpy.ops.object.mode_set(mode="OBJECT")
    _ensure_face_properties(armature, document.get("face_names", []))
    return armature


def _clear_animation(armature):
    armature.animation_data_clear()
    for bone in armature.pose.bones:
        bone.rotation_mode = "QUATERNION"


def _camera_from_key(camera):
    focus = Vector(camera["focus"])
    yaw = math.radians(camera["yaw"])
    pitch = math.radians(camera["pitch"])
    direction = Vector((math.cos(pitch) * math.cos(yaw),
                        math.cos(pitch) * math.sin(yaw), math.sin(pitch)))
    focus *= 0.01
    position = focus + direction * (camera["distance"] * 0.01)
    rotation = direction.to_track_quat("-Z", "Y").to_euler()
    return position, rotation


def _motion_root_world_position(armature, depsgraph):
    if armature is None:
        return Vector((0.0, 0.0, 0.0))
    sampled = armature.evaluated_get(depsgraph)
    root = next((bone for bone in sampled.pose.bones
                 if bone.name in {"Root", "root", "Bip001"}), None)
    if root is None:
        root = next((bone for bone in sampled.pose.bones
                     if bone.parent is None), None)
    return ((sampled.matrix_world @ root.matrix).translation.copy()
            if root is not None else Vector((0.0, 0.0, 0.0)))


def _camera_track_document(value, current_time=0.0):
    if isinstance(value, list) and value:
        return {
            "kind": "mmd",
            "fps": 30.0,
            "current_time": max(0.0, float(current_time)),
            "keys": value,
        }
    if isinstance(value, dict) and isinstance(value.get("keys"), list) and value["keys"]:
        return value
    return None


def _rest_focus_height(armature):
    if armature is None:
        return 1.0
    points = []
    for bone in armature.data.bones:
        points.append(armature.matrix_world @ bone.head_local)
        points.append(armature.matrix_world @ bone.tail_local)
    if not points:
        return 1.0
    low = min(point.z for point in points)
    high = max(point.z for point in points)
    return max(0.5, (high - low) * 0.6)


def _camera_key(scene, camera):
    if not _is_betterpose_camera(scene):
        return None
    depsgraph = bpy.context.evaluated_depsgraph_get()
    sampled_camera = camera.evaluated_get(depsgraph)
    armature = _armature(scene)
    matrix = sampled_camera.matrix_world
    sampled_armature = (armature.evaluated_get(depsgraph)
                        if armature else None)
    camera_position = matrix.translation
    configured_focus = scene.get("betterpose_camera_focus")
    focus = Vector(configured_focus if configured_focus else [0, 0, 0])
    if not scene.get("betterpose_game_camera_loaded", False):
        # Camera files are relative to the animated root bone, not the armature
        # object. The latter commonly carries an import rotation; applying its
        # inverse rotates the whole shot. Remove only world-space root travel,
        # then preserve the camera's authored world orientation.
        root_position = _motion_root_world_position(armature, depsgraph)
        camera_position = matrix.translation - root_position
        # Keep the subject centred with a stable upper-chest target. Bone
        # bounds and imported camera focus curves both move with pose/library
        # data and produced either jitter or a large pitch offset.
        focus = Vector((0.0, 0.0, _rest_focus_height(armature)))
    delta = camera_position - focus
    distance = max(delta.length, 0.01)
    direction = delta.normalized()
    relative = matrix.translation - _motion_root_world_position(
        armature, depsgraph)
    rotation = matrix.to_quaternion()
    horizontal_fov = math.degrees(sampled_camera.data.angle)
    vertical_fov = math.degrees(2.0 * math.atan(
        math.tan(math.radians(horizontal_fov) * 0.5) / (16.0 / 9.0)))
    packet = {"focus": [value * 100.0 for value in focus],
              "yaw": math.degrees(math.atan2(direction.y, direction.x)),
              "pitch": math.degrees(math.asin(max(-1.0, min(1.0, direction.z)))),
              "distance": distance * 100.0,
              "fov": vertical_fov,
              "position": _blender_to_ue_vector(
                  [value * 100.0 for value in matrix.translation]),
              "relative_position": _blender_to_ue_vector(
                  [value * 100.0 for value in relative]),
              "rotation": _blender_to_ue_quaternion(
                  [rotation.x, rotation.y, rotation.z, rotation.w])}
    packet.update(_camera_delta(scene, scene.camera))
    motion_delta = _camera_delta(scene, scene.camera, fixed_baseline=True)
    packet["motion_delta_position"] = motion_delta["delta_position"]
    packet["motion_delta_rotation"] = motion_delta["delta_rotation"]
    return packet


def _camera_track_payload(scene):
    camera = scene.camera
    if camera is None:
        return None
    original_frame = scene.frame_current
    original_subframe = scene.frame_subframe
    armature = _armature(scene)
    document = _cached_armature_document(armature) if armature else None
    current_time = ((original_frame - scene.frame_start) /
                    max(scene.render.fps / scene.render.fps_base, 1.0))
    source_track = _camera_track_document(
        document.get("camera_track") if document else None,
        current_time)
    if source_track is not None:
        result = dict(source_track)
        result["current_time"] = current_time
        return result
    frames = []
    try:
        for frame in range(int(round(scene.frame_start)),
                           int(round(scene.frame_end)) + 1):
            scene.frame_set(frame)
            bpy.context.view_layer.update()
            depsgraph = bpy.context.evaluated_depsgraph_get()
            sampled_camera = camera.evaluated_get(depsgraph)
            matrix = sampled_camera.matrix_world
            root_position = _motion_root_world_position(armature, depsgraph)
            camera_position = matrix.translation - root_position
            forward = matrix.to_quaternion() @ Vector((0.0, 0.0, -1.0))
            focus = camera_position + forward * 2.0
            direction = camera_position - focus
            horizontal_fov = math.degrees(sampled_camera.data.angle)
            vertical_fov = math.degrees(2.0 * math.atan(
                math.tan(math.radians(horizontal_fov) * 0.5) /
                (16.0 / 9.0)))
            frames.append({
                "time": (frame - scene.frame_start) /
                        max(scene.render.fps / scene.render.fps_base, 1.0),
                "camera": {
                    "focus": [value * 100.0 for value in focus],
                    "yaw": math.degrees(math.atan2(direction.y, direction.x)),
                    "pitch": math.degrees(math.asin(
                        max(-1.0, min(1.0, direction.z)))),
                    "distance": 200.0,
                    "fov": vertical_fov,
                },
            })
    finally:
        scene.frame_set(original_frame, subframe=original_subframe)
        bpy.context.view_layer.update()
    return {
        "kind": "baked",
        "fps": 30.0,
        "current_time": (original_frame - scene.frame_start) /
                        max(scene.render.fps / scene.render.fps_base, 1.0),
        "frames": frames,
    }


def _quat_xyzw(value):
    return Quaternion((float(value[3]), float(value[0]), float(value[1]), float(value[2]))).normalized()


def _game_quat_to_rotator(q):
    q = q.normalized()
    singularity = q.z * q.x - q.w * q.y
    yaw = math.atan2(2.0 * (q.w * q.z + q.x * q.y),
                     1.0 - 2.0 * (q.y * q.y + q.z * q.z))
    if singularity < -0.4999995:
        pitch = -math.pi * 0.5
        roll = -yaw - 2.0 * math.atan2(q.x, q.w)
    elif singularity > 0.4999995:
        pitch = math.pi * 0.5
        roll = yaw - 2.0 * math.atan2(q.x, q.w)
    else:
        pitch = math.asin(max(-1.0, min(1.0, 2.0 * singularity)))
        roll = math.atan2(-2.0 * (q.w * q.x + q.y * q.z),
                          1.0 - 2.0 * (q.x * q.x + q.y * q.y))
    return [math.degrees(pitch), math.degrees(yaw), math.degrees(roll)]


def _live_game_pose(armature, document):
    """Convert Blender component poses back to game-local deltas and translations."""
    count = len(document["bone_names"])
    cache_key = (id(armature), armature.get("betterpose_document", ""))
    cached = _live_cache.get(cache_key)
    if cached is None:
        component_rest = [_quat_xyzw(_blender_to_ue_quaternion(value))
                          for value in document.get("bone_component_rotations", [[0, 0, 0, 1]] * count)]
        local_rest = [_quat_xyzw(_blender_to_ue_quaternion(value))
                      for value in document.get("bone_rotations", [[0, 0, 0, 1]] * count)]
        required = set()
        for index, name in enumerate(document["bone_names"]):
            if name == "Root" or name == "Bip001" or name.startswith("Bip001-"):
                at = index
                while at >= 0 and at not in required:
                    required.add(at)
                    at = document["bone_parents"][at]
        cached = (component_rest, local_rest, required)
        _live_cache.clear()
        _live_cache[cache_key] = cached
    component_rest, local_rest, required = cached
    current_component = [None] * count
    current_component_position = [None] * count
    pose_component_blender = [None] * count
    deltas = {}

    def resolve(index, visiting=None):
        if current_component[index] is not None:
            return
        visiting = set() if visiting is None else visiting
        if index in visiting:
            raise ValueError("骨架父级关系存在循环")
        visiting.add(index)
        pose_bone = armature.pose.bones[index]
        rest_blender = pose_bone.bone.matrix_local.to_quaternion()
        # matrix_basis is the already-evaluated local action value. Building
        # the component quaternion from it avoids asking Blender for the
        # final dependency-graph matrix on every sync tick.
        parent = document["bone_parents"][index]
        rest_matrix = pose_bone.bone.matrix_local
        if parent >= 0:
            resolve(parent, visiting)
            parent_bone = armature.pose.bones[parent].bone
            pose_matrix = (pose_component_blender[parent] @
                           parent_bone.matrix_local.inverted() @
                           rest_matrix @ pose_bone.matrix_basis)
        else:
            pose_matrix = rest_matrix @ pose_bone.matrix_basis
        pose_component_blender[index] = pose_matrix
        pose_blender = pose_matrix.to_quaternion()
        world_delta_blender = pose_blender @ rest_blender.inverted()
        world_delta_game = _quat_xyzw(_blender_to_ue_quaternion(
            [world_delta_blender.x, world_delta_blender.y,
             world_delta_blender.z, world_delta_blender.w]))
        current_component[index] = world_delta_game @ component_rest[index]
        current_component_position[index] = _blender_to_ue_vector([
            value * 100.0 for value in pose_matrix.translation])
        if parent >= 0:
            current_local = current_component[parent].inverted() @ current_component[index]
        else:
            current_local = current_component[index]
        if index in required:
            deltas[index] = _game_quat_to_rotator(current_local @ local_rest[index].inverted())
        visiting.remove(index)

    for index in sorted(required):
        resolve(index)
    positions = {}
    for index in sorted(required):
        parent = document["bone_parents"][index]
        if parent >= 0:
            world_delta = (Vector(current_component_position[index]) -
                           Vector(current_component_position[parent]))
            local = current_component[parent].inverted() @ world_delta
        else:
            local = Vector(current_component_position[index])
        positions[index] = list(local)
    return deltas, positions


def _document_rest_state(document):
    """Return component rest rotations and positions in Blender coordinates."""
    count = len(document["bone_names"])
    positions = document.get("bone_positions") or [[0, 0, 0] for _ in range(count)]
    if len(positions) != count:
        positions = [[0, 0, 0] for _ in range(count)]
    raw_rotations = document.get("bone_component_rotations") or [[0, 0, 0, 1] for _ in range(count)]
    if len(raw_rotations) != count:
        raw_rotations = [[0, 0, 0, 1] for _ in range(count)]
    component_rotations = [_quat_xyzw(value) for value in raw_rotations]
    component_positions = [None] * count
    if document.get("bone_position_space") == "component":
        component_positions = [Vector(value) * 0.01 for value in positions]
    else:
        local_positions = [Vector(value) * 0.01 for value in positions]

        def resolve(index, visiting=None):
            if component_positions[index] is not None:
                return component_positions[index]
            visiting = set() if visiting is None else visiting
            if index in visiting:
                raise ValueError("骨架父级关系存在循环")
            visiting.add(index)
            parent = document["bone_parents"][index]
            if parent >= 0:
                component_positions[index] = resolve(parent, visiting) + (
                    component_rotations[parent] @ local_positions[index])
            else:
                component_positions[index] = local_positions[index]
            visiting.remove(index)
            return component_positions[index]

        for index in range(count):
            resolve(index)
    local_rotations = document.get("bone_rotations") or [[0, 0, 0, 1] for _ in range(count)]
    if len(local_rotations) != count:
        local_rotations = [[0, 0, 0, 1] for _ in range(count)]
    local_rotations = [_quat_xyzw(value) for value in local_rotations]
    local_positions = [Vector((0, 0, 0)) for _ in range(count)]
    for index, parent in enumerate(document["bone_parents"]):
        if parent >= 0:
            local_positions[index] = component_rotations[parent].inverted() @ (
                component_positions[index] - component_positions[parent])
        else:
            local_positions[index] = component_positions[index]
    return component_rotations, local_rotations, component_positions, local_positions


def _frame_component_matrices(armature, document, frame):
    """Build Blender component matrices from game-local MMD deltas.

    The game applies each MMD delta before the local reference rotation.  We
    reproduce that hierarchy first, then express the component delta relative
    to Blender's actual edit-bone rest matrix.  This keeps bone roll out of the
    game coordinate conversion and makes the zero frame exactly the rest pose.
    """
    count = len(document["bone_names"])
    component_rest, local_rest, rest_positions, local_positions = _document_rest_state(document)
    deltas = [Quaternion((1, 0, 0, 0)) for _ in range(count)]
    for item in frame.get("bones", []):
        index = item["i"]
        if "q" in item:
            deltas[index] = _quat_xyzw(_ue_to_blender_quaternion(item["q"]))
        else:
            # Legacy files contain UE Rotator values.  Keep this fallback for
            # files made before the lossless q field was added.
            pitch, yaw, roll = (math.radians(value) for value in item.get("r", [0, 0, 0]))
            half = 0.5
            sp, cp = math.sin(pitch * half), math.cos(pitch * half)
            sy, cy = math.sin(yaw * half), math.cos(yaw * half)
            sr, cr = math.sin(roll * half), math.cos(roll * half)
            game_delta = Quaternion((cr * cp * cy + sr * sp * sy,
                                     cr * sp * sy - sr * cp * cy,
                                     -cr * sp * cy - sr * cp * sy,
                                     cr * cp * sy - sr * sp * cy))
            deltas[index] = _reflect_game_quaternion(game_delta)
    current_local = [deltas[index] @ local_rest[index] for index in range(count)]
    for item in frame.get("bones", []):
        if "p" in item:
            local_positions[item["i"]] = Vector(_ue_to_blender_vector(item["p"])) * 0.01
    current_component = [None] * count
    current_positions = [None] * count

    def resolve(index, visiting=None):
        if current_component[index] is not None:
            return
        visiting = set() if visiting is None else visiting
        if index in visiting:
            raise ValueError("骨架父级关系存在循环")
        visiting.add(index)
        parent = document["bone_parents"][index]
        if parent >= 0:
            resolve(parent, visiting)
            current_component[index] = current_component[parent] @ current_local[index]
            current_positions[index] = current_positions[parent] + (
                current_component[parent] @ local_positions[index])
        else:
            current_component[index] = current_local[index]
            current_positions[index] = local_positions[index]
        visiting.remove(index)

    for index in range(count):
        resolve(index)
    matrices = []
    for index, pose_bone in enumerate(armature.pose.bones):
        component_delta = current_component[index] @ component_rest[index].inverted()
        target_rotation = component_delta @ pose_bone.bone.matrix_local.to_quaternion()
        matrices.append(Matrix.LocRotScale(current_positions[index], target_rotation, Vector((1, 1, 1))))
    return matrices


def _parent_order(parents):
    order = []
    state = [0] * len(parents)

    def visit(index):
        if state[index] == 2:
            return
        if state[index] == 1:
            raise ValueError("骨架父级关系存在循环")
        state[index] = 1
        parent = parents[index]
        if parent >= 0:
            visit(parent)
        state[index] = 2
        order.append(index)

    for index in range(len(parents)):
        visit(index)
    return order


def _apply_document(scene, armature, document):
    _clear_animation(armature)
    fps = document["fps"]
    scene.render.fps = round(fps)
    scene.render.fps_base = scene.render.fps / fps
    scene.frame_start = 1
    # `length` is expressed in source-FPS frames.  BetterPose writes 30 FPS;
    # Blender exports use the scene's own FPS and the game retimes on import.
    scene.frame_end = max(2, round(document["length"]) + 1)
    camera_object = None
    if any(frame.get("camera") for frame in document["frames"]):
        camera_data = bpy.data.cameras.new("BetterPose Camera")
        camera_object = bpy.data.objects.new("BetterPose Camera", camera_data)
        bpy.context.collection.objects.link(camera_object)
        camera_object["betterpose_managed_camera"] = True
        scene.camera = camera_object
    # Build F-curves in memory and publish them once.  Calling RNA
    # keyframe_insert plus a dependency-graph update for every bone/frame made
    # long motions appear to stall; Endfield's bridge uses the same batched
    # channel construction.
    channels = {}

    def add_channel(owner, path, index, frame, value):
        channels.setdefault((owner, path, index), []).extend((float(frame), float(value)))

    face_names = list(document.get("face_names", []))
    for frame in document["frames"]:
        face_names.extend(name for name in frame.get("faces", {})
                          if name not in face_names)
    _ensure_face_properties(armature, face_names)
    animated_faces = []
    for frame in document["frames"]:
        animated_faces.extend(name for name in frame.get("faces", {})
                              if name not in animated_faces)

    frames_by_number = {}
    for frame in document["frames"]:
        frames_by_number[scene.frame_start + round(frame["time"] * fps)] = frame
    previous_quaternions = {}
    for blender_frame, frame in sorted(frames_by_number.items()):
        matrices = _frame_component_matrices(armature, document, frame)
        root = Vector(_ue_to_blender_vector([value * 0.01 for value in frame["root"]]))
        for axis, value in enumerate(root):
            add_channel(armature, "location", axis, blender_frame, value)
        for index in _parent_order(document["bone_parents"]):
            bone = armature.pose.bones[index]
            bone.rotation_mode = "QUATERNION"
            parent = bone.parent
            if parent:
                parent_index = document["bone_parents"][index]
                basis = (bone.bone.matrix_local.inverted() @ parent.bone.matrix_local @
                         matrices[parent_index].inverted() @ matrices[index])
            else:
                basis = bone.bone.matrix_local.inverted() @ matrices[index]
            location, rotation, scale = basis.decompose()
            previous = previous_quaternions.get(index)
            if previous is not None and rotation.dot(previous) < 0.0:
                rotation.negate()
            previous_quaternions[index] = rotation.copy()
            path_location = bone.path_from_id("location")
            path_rotation = bone.path_from_id("rotation_quaternion")
            path_scale = bone.path_from_id("scale")
            for axis, value in enumerate(location):
                add_channel(armature, path_location, axis, blender_frame, value)
            for axis, value in enumerate(rotation):
                add_channel(armature, path_rotation, axis, blender_frame, value)
            for axis, value in enumerate(scale):
                add_channel(armature, path_scale, axis, blender_frame, value)
        for name in animated_faces:
            add_channel(armature, f'["betterpose.face.{name}"]', 0, blender_frame,
                        frame["faces"].get(name, 0.0))
        if camera_object and frame.get("camera"):
            camera = frame["camera"]
            position, rotation = _camera_from_key(camera)
            for axis, value in enumerate(position):
                add_channel(camera_object, "location", axis, blender_frame, value)
            for axis, value in enumerate(rotation):
                add_channel(camera_object, "rotation_euler", axis, blender_frame, value)
            if camera["fov"] > 0:
                horizontal_fov = math.degrees(2.0 * math.atan(
                    math.tan(math.radians(camera["fov"]) * 0.5) * (16.0 / 9.0)))
                lens = 36.0 / (2.0 * math.tan(
                    math.radians(horizontal_fov) / 2.0))
                add_channel(camera_object.data, "lens", 0, blender_frame, lens)
            focus_distance = max(
                0.1, (Vector(camera["focus"]) * 0.01 - position).length)
            add_channel(camera_object.data, "dof.focus_distance", 0,
                        blender_frame, focus_distance)
            scene["betterpose_camera_focus"] = [value * 0.01 for value in camera["focus"]]

    action = bpy.data.actions.new("BetterPose Base")
    action["betterpose_base"] = True
    armature.animation_data_create().action = action
    armature["betterpose_base_action"] = action.name
    camera_action = None
    camera_data_action = None
    if camera_object is not None:
        camera_action = bpy.data.actions.new("BetterPose Camera Base")
        camera_action["betterpose_camera_base"] = True
        camera_object.animation_data_create().action = camera_action
        camera_object["betterpose_camera_base_action"] = camera_action.name
        camera_source_action = camera_action.copy()
        camera_source_action.name = "BetterPose Camera Source Transform"
        camera_source_action["betterpose_camera_source"] = True
        camera_source_action.use_fake_user = True
        camera_object["betterpose_camera_source_action"] = camera_source_action.name
        camera_data_action = bpy.data.actions.new("BetterPose Camera Lens Base")
        camera_data_action["betterpose_camera_base"] = True
        camera_object.data.animation_data_create().action = camera_data_action
        camera_object.data["betterpose_camera_base_action"] = camera_data_action.name
        camera_source_lens = camera_data_action.copy()
        camera_source_lens.name = "BetterPose Camera Source Lens"
        camera_source_lens["betterpose_camera_source"] = True
        camera_source_lens.use_fake_user = True
        camera_object.data["betterpose_camera_source_action"] = camera_source_lens.name
    for (owner, path, index), values in channels.items():
        if owner == armature:
            target_action = action
        elif owner == camera_object:
            target_action = camera_action
        elif owner == camera_object.data:
            target_action = camera_data_action
        else:
            target_action = None
        if target_action is None:
            continue
        curve = target_action.fcurves.new(data_path=path, index=index)
        curve.keyframe_points.add(len(values) // 2)
        curve.keyframe_points.foreach_set("co", values)
        for point in curve.keyframe_points:
            point.interpolation = "LINEAR"
        curve.update()
    _apply_edit_display(armature)
    scene.frame_set(scene.frame_start)


def _is_edit_body_bone(name):
    return name == "Root" or name == "Bip001" or name.startswith("Bip001-")


def _apply_edit_display(armature):
    if not armature or armature.type != "ARMATURE":
        return
    for pose_bone in armature.pose.bones:
        visible = _is_edit_body_bone(pose_bone.name)
        pose_bone.bone.hide = not visible
        pose_bone.bone.hide_select = not visible
        if not visible:
            pose_bone.bone.select = False


def _selected_edit_bones(armature):
    selected = [bone for bone in armature.pose.bones
                if bone.bone.select and not bone.bone.hide]
    active = bpy.context.active_pose_bone
    if not selected and active is not None and active.id_data == armature:
        selected = [active]
    return selected


def _has_nla_action(animation_data, action):
    return any(strip.action == action
               for track in animation_data.nla_tracks
               for strip in track.strips)


def _stored_action(owner, property_name, marker):
    name = owner.get(property_name, "") if owner else ""
    action = bpy.data.actions.get(name) if name else None
    return action if action and action.get(marker, False) else None


def _find_action_track(animation_data, action):
    if animation_data is None or action is None:
        return None
    return next((track for track in animation_data.nla_tracks
                 if any(strip.action == action for strip in track.strips)), None)


def _ensure_action_nla_track(animation_data, action, name):
    if animation_data is None or action is None:
        return None
    track = _find_action_track(animation_data, action)
    if track is not None:
        return track
    if not action.fcurves:
        return None
    track = animation_data.nla_tracks.new()
    track.name = name
    strip = track.strips.new(action.name, int(round(action.frame_range[0])), action)
    strip.name = "原动作"
    strip.extrapolation = "HOLD"
    strip.blend_type = "REPLACE"
    track.mute = False
    return track


def _leave_layer_tweak(scene):
    if not scene.is_nla_tweakmode:
        return
    context = bpy.context
    area = next((item for item in context.screen.areas
                 if item.type in {"DOPESHEET_EDITOR", "NLA_EDITOR"}), None)
    if area is None:
        return
    previous = area.type
    area.type = "NLA_EDITOR"
    region = next(item for item in area.regions if item.type == "WINDOW")
    with context.temp_override(area=area, region=region):
        bpy.ops.nla.tweakmode_exit(isolate_action=False)
    area.type = previous


def _enter_layer_tweak(scene, owner, action):
    animation_data = owner.animation_data if owner else None
    track = _find_action_track(animation_data, action)
    if track is None:
        return False
    _leave_layer_tweak(scene)
    animation_data.use_tweak_mode = False
    context = bpy.context
    area = next((item for item in context.screen.areas
                 if item.type in {"DOPESHEET_EDITOR", "NLA_EDITOR", "VIEW_3D"}),
                None)
    if area is None:
        return False
    previous = area.type
    area.type = "NLA_EDITOR"
    for item in animation_data.nla_tracks:
        item.select = item == track
        for strip in item.strips:
            strip.select = False
    animation_data.nla_tracks.active = track
    track.select = True
    if not track.strips:
        area.type = previous
        return False
    track.strips[0].select = True
    region = next(item for item in area.regions if item.type == "WINDOW")
    try:
        with context.temp_override(area=area, region=region):
            result = bpy.ops.nla.tweakmode_enter(isolate_action=False,
                                                 use_upper_stack_evaluation=True)
    finally:
        area.type = previous
    return "FINISHED" in result


def _ensure_correction_layer(scene, armature):
    animation_data = armature.animation_data_create()
    current = animation_data.action
    if current and current.get("betterpose_correction", False):
        _ensure_action_nla_track(
            animation_data, _pose_base_action(armature), "BetterPose Base")
        if (animation_data.action_blend_type != "COMBINE" or
                any("betterpose.face." in curve.data_path for curve in current.fcurves)):
            _migrate_body_correction(scene, armature, current)
        animation_data.action_blend_type = "COMBINE"
        animation_data.action_influence = 1.0
        return current
    stored = _stored_action(armature, "betterpose_correction_action",
                            "betterpose_correction")
    if stored is not None:
        _ensure_action_nla_track(
            animation_data, _pose_base_action(armature), "BetterPose Base")
        animation_data.action = stored
        animation_data.action_blend_type = "COMBINE"
        animation_data.action_extrapolation = "HOLD"
        animation_data.action_influence = 1.0
        return stored

    captured = {bone.name: bone.matrix_basis.copy()
                for bone in _selected_edit_bones(armature)}
    base = current
    if base is not None:
        base["betterpose_base"] = True
        armature["betterpose_base_action"] = base.name
        if _find_action_track(animation_data, base) is None:
            track = animation_data.nla_tracks.new()
            track.name = "BetterPose Base"
            strip = track.strips.new(base.name, int(round(base.frame_range[0])), base)
            strip.name = "原动作"
            strip.extrapolation = "HOLD"
            strip.blend_type = "REPLACE"
            track.mute = False

    correction = bpy.data.actions.new("BetterPose Correction")
    correction["betterpose_correction"] = True
    correction.use_fake_user = True
    animation_data.action = correction
    animation_data.action_blend_type = "COMBINE"
    animation_data.action_extrapolation = "HOLD"
    animation_data.action_influence = 1.0
    armature["betterpose_correction_action"] = correction.name
    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    bpy.context.view_layer.update()
    for name, basis in captured.items():
        bone = armature.pose.bones.get(name)
        if bone:
            bone.matrix_basis = basis
    return correction


def _pose_base_action(armature):
    animation_data = armature.animation_data
    if animation_data and animation_data.use_tweak_mode and animation_data.action:
        if not animation_data.action.get("betterpose_correction", False):
            return animation_data.action
    stored = _stored_action(armature, "betterpose_base_action",
                            "betterpose_base")
    if stored is not None:
        return stored
    if animation_data and animation_data.action:
        if not animation_data.action.get("betterpose_correction", False):
            return animation_data.action
    return _base_action(animation_data)


def _migrate_body_correction(scene, armature, correction):
    base = _pose_base_action(armature)
    animation_data = armature.animation_data_create()
    if base is not None:
        for curve in list(correction.fcurves):
            if "betterpose.face." not in curve.data_path:
                continue
            target = next((item for item in base.fcurves
                           if item.data_path == curve.data_path and
                           item.array_index == curve.array_index), None)
            if target is None:
                target = base.fcurves.new(curve.data_path,
                                          index=curve.array_index,
                                          action_group="MMD 表情")
            for point in curve.keyframe_points:
                old = next((item for item in target.keyframe_points
                            if abs(item.co.x - point.co.x) < 1e-5), None)
                if old is None:
                    old = target.keyframe_points.insert(point.co.x, point.co.y)
                else:
                    old.co.y = point.co.y
                old.interpolation = point.interpolation
                old.handle_left_type = point.handle_left_type
                old.handle_right_type = point.handle_right_type
            target.update()
            correction.fcurves.remove(curve)

    curves = list(correction.fcurves)
    frames = sorted({point.co.x for curve in curves
                     for point in curve.keyframe_points})
    previous_influence = animation_data.action_influence
    base_values = {}
    try:
        animation_data.action_influence = 0.0
        for frame in frames:
            scene.frame_set(int(frame), subframe=frame - int(frame))
            bpy.context.view_layer.update()
            for curve in curves:
                try:
                    resolved = armature.path_resolve(curve.data_path)
                    base_values[(curve.data_path, curve.array_index, frame)] = (
                        resolved[curve.array_index]
                        if hasattr(resolved, "__len__") else resolved)
                except (TypeError, ValueError):
                    continue
    finally:
        animation_data.action_influence = previous_influence
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()

    for curve in curves:
        neutral = (1.0 if (curve.data_path.endswith("scale") or
                           (curve.data_path.endswith("rotation_quaternion") and
                            curve.array_index == 0)) else 0.0)
        for point in curve.keyframe_points:
            base_value = base_values.get(
                (curve.data_path, curve.array_index, point.co.x))
            if base_value is not None:
                point.co.y = neutral + point.co.y - float(base_value)
            point.interpolation = "BEZIER"
            point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
        if (curve.keyframe_points and
                min(point.co.x for point in curve.keyframe_points) >
                scene.frame_start):
            curve.keyframe_points.insert(scene.frame_start, neutral)
        curve.update()
    animation_data.action_blend_type = "COMBINE"
    animation_data.action_extrapolation = "HOLD"
    animation_data.action_influence = 1.0
    correction.update_tag()
    armature.update_tag()
    bpy.context.view_layer.update()


def _enter_pose_base_edit(scene, armature):
    if (getattr(scene, "bp_pose_edit_base", False) and armature.animation_data and
            armature.animation_data.use_tweak_mode):
        return armature.animation_data.action
    _ensure_correction_layer(scene, armature)
    base = _pose_base_action(armature)
    if base is not None:
        _ensure_action_nla_track(armature.animation_data, base,
                                 "BetterPose Base")
    if base is None:
        raise ValueError("找不到原动作 NLA 层")
    if not _enter_layer_tweak(scene, armature, base):
        raise ValueError("无法进入原动作 NLA 编辑模式")
    scene.bp_pose_edit_base = True
    return base


def _enter_pose_correction_edit(scene, armature):
    _leave_layer_tweak(scene)
    correction = _ensure_correction_layer(scene, armature)
    animation_data = armature.animation_data_create()
    animation_data.action = correction
    animation_data.action_blend_type = "COMBINE"
    animation_data.action_extrapolation = "HOLD"
    animation_data.action_influence = 1.0
    scene.bp_pose_edit_base = False
    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    bpy.context.view_layer.update()
    return correction


def _restore_selected_to_base(scene, armature, selected):
    animation_data = armature.animation_data_create()
    if getattr(scene, "bp_pose_edit_base", False):
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
        return
    correction = _ensure_correction_layer(scene, armature)
    old_action = animation_data.action
    try:
        animation_data.action = None
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
        restored = {bone.name: bone.matrix_basis.copy() for bone in selected}
    finally:
        animation_data.action = correction or old_action
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
    for name, basis in restored.items():
        bone = armature.pose.bones.get(name)
        if bone:
            bone.matrix_basis = basis


def _key_selected_pose(scene, armature, selected):
    animation_data = armature.animation_data_create()
    base_mode = bool(getattr(scene, "bp_pose_edit_base", False) or
                     (animation_data.use_tweak_mode and animation_data.action and
                      not animation_data.action.get("betterpose_correction", False)))
    if base_mode:
        action = animation_data.action or _pose_base_action(armature)
        if action is None:
            raise ValueError("找不到原动作层")
    else:
        action = _ensure_correction_layer(scene, armature)
    frame = scene.frame_current
    for bone in selected:
        bone.rotation_mode = "QUATERNION"
        bone.keyframe_insert(data_path="location", frame=frame, group=bone.name)
        bone.keyframe_insert(data_path="rotation_quaternion", frame=frame, group=bone.name)
        bone.keyframe_insert(data_path="scale", frame=frame, group=bone.name)
        if base_mode:
            continue
        for path, channels in (("location", 3), ("rotation_quaternion", 4),
                               ("scale", 3)):
            for index in range(channels):
                curve = action.fcurves.find(bone.path_from_id(path), index=index)
                if curve is None:
                    continue
                if len(curve.keyframe_points) == 1 and frame > scene.frame_start:
                    neutral = (1.0 if path == "scale" or
                               (path == "rotation_quaternion" and index == 0)
                               else 0.0)
                    curve.keyframe_points.insert(scene.frame_start, neutral)
                for point in curve.keyframe_points:
                    if point.co.x in (frame, scene.frame_start):
                        point.interpolation = "BEZIER"
                        point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
                curve.update()
    action.update_tag()
    armature.update_tag()
    bpy.context.view_layer.update()


def _key_faces(scene, armature):
    names = list(armature.get("betterpose_face_names", []))
    base = _pose_base_action(armature)
    if base is None:
        raise ValueError("找不到原动作层")
    correction = _stored_action(armature, "betterpose_correction_action",
                                "betterpose_correction")
    if correction is not None:
        for curve in list(correction.fcurves):
            if "betterpose.face." in curve.data_path:
                correction.fcurves.remove(curve)

    animation_data = armature.animation_data_create()
    original_action = animation_data.action
    was_tweak = bool(animation_data.use_tweak_mode)
    properties = {}
    for name in names:
        prop = "betterpose.face." + name
        if prop in armature:
            properties[prop] = float(armature[prop])
    switched = not was_tweak and original_action != base
    if switched:
        animation_data.action = base
        animation_data.action_blend_type = "REPLACE"
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
    try:
        for prop, value in properties.items():
            armature[prop] = value
        for name in names:
            prop = "betterpose.face." + name
            if prop not in properties:
                continue
            armature.keyframe_insert(data_path=f'["{prop}"]',
                                     frame=scene.frame_current,
                                     group="MMD 表情")
        base.update_tag()
        armature.update_tag()
        bpy.context.view_layer.update()
    finally:
        if switched and correction is not None:
            animation_data.action = correction
            animation_data.action_blend_type = "COMBINE"
            animation_data.action_extrapolation = "HOLD"
            animation_data.action_influence = 1.0
            scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
            for prop, value in properties.items():
                armature[prop] = value
            armature.update_tag()
            bpy.context.view_layer.update()
    return len(names)


def _camera_owners(camera):
    return (
        (camera, ("location", "rotation_euler"), "Transform"),
        (camera.data, ("lens", "dof.focus_distance"), "Lens"),
    )


def _camera_source_actions(camera):
    _split_legacy_camera_data(camera)
    result = {}
    for owner, _, label in _camera_owners(camera):
        name = owner.get("betterpose_camera_source_action", "")
        source = bpy.data.actions.get(name) if name else None
        if source is None:
            base = _base_action(owner.animation_data)
            if base is None:
                continue
            source = base.copy()
            source.name = "BetterPose Camera Source " + label
            source["betterpose_camera_source"] = True
            source.use_fake_user = True
            owner["betterpose_camera_source_action"] = source.name
        result["data" if owner == camera.data else "object"] = source
    return result


def _curve_value(action, path, index, default):
    if action is None:
        return default
    curve = next((item for item in action.fcurves
                  if item.data_path == path and item.array_index == index), None)
    return curve.evaluate(bpy.context.scene.frame_current) if curve else default


def _is_betterpose_camera(scene):
    camera = scene.camera
    return bool(camera and (
        camera.get("betterpose_managed_camera", False) or
        camera.get("betterpose_camera_base_action") or
        camera.name.startswith("BetterPose Camera") or
        "betterpose_camera_focus" in scene))


def _capture_camera_baseline(scene):
    global _camera_baseline_scene, _camera_baseline_data
    _camera_baseline_scene = 0
    _camera_baseline_data = None
    if scene.camera is None:
        if "betterpose_camera_baseline" in scene:
            del scene["betterpose_camera_baseline"]
        return
    camera = scene.camera
    original_frame = scene.frame_current
    original_subframe = scene.frame_subframe
    start = int(round(scene.frame_start))
    end = int(round(scene.frame_end))
    samples = []
    for frame in range(start, end + 1):
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        rotation = camera.rotation_euler.to_quaternion()
        samples.append([
            float(camera.location.x), float(camera.location.y), float(camera.location.z),
            float(rotation.w), float(rotation.x), float(rotation.y), float(rotation.z),
            float(camera.data.lens),
        ])
    fixed_index = max(0, min(int(round(original_frame)) - start,
                             len(samples) - 1))
    data = {"start": start, "samples": samples,
            "fixed_sample": samples[fixed_index] if samples else None}
    scene["betterpose_camera_baseline"] = json.dumps(
        data, separators=(",", ":"))
    _camera_baseline_scene = scene.as_pointer()
    _camera_baseline_data = data
    scene.frame_set(original_frame, subframe=original_subframe)
    bpy.context.view_layer.update()


def _camera_baseline_sample(scene, fixed=False):
    global _camera_baseline_scene, _camera_baseline_data
    serialized = scene.get("betterpose_camera_baseline", "")
    if _camera_baseline_data is None or _camera_baseline_scene != scene.as_pointer():
        if not serialized:
            return None
        _camera_baseline_data = json.loads(serialized)
        _camera_baseline_scene = scene.as_pointer()
    samples = _camera_baseline_data.get("samples", [])
    if not samples:
        return None
    if fixed:
        return _camera_baseline_data.get("fixed_sample") or samples[0]
    index = int(round(scene.frame_current)) - int(_camera_baseline_data.get("start", 0))
    index = max(0, min(index, len(samples) - 1))
    return samples[index]


def _camera_delta(scene, camera, fixed_baseline=False):
    baseline = _camera_baseline_sample(scene, fixed_baseline)
    if baseline is not None:
        source_location = Vector(baseline[:3])
        source_rotation = Quaternion((
            baseline[3], baseline[4], baseline[5], baseline[6]))
        source_lens = float(baseline[7])
    else:
        sources = _camera_source_actions(camera)
        transform = sources.get("object")
        lens_action = sources.get("data")
        source_location = Vector(tuple(
            _curve_value(transform, "location", axis, 0.0)
            for axis in range(3)))
        source_rotation = Euler(tuple(
            _curve_value(transform, "rotation_euler", axis, 0.0)
            for axis in range(3)), "XYZ").to_quaternion()
        source_lens = float(_curve_value(lens_action, "lens", 0, camera.data.lens))
    depsgraph = bpy.context.evaluated_depsgraph_get()
    sampled_camera = camera.evaluated_get(depsgraph)
    final_rotation = sampled_camera.rotation_euler.to_quaternion()
    rotation_delta = final_rotation @ source_rotation.inverted()
    position_delta = sampled_camera.location - source_location
    lens_scale = (sampled_camera.data.lens / source_lens
                  if source_lens > 1e-6 else 1.0)
    # Blender's camera-local X is the opposite of the game view's horizontal
    # axis. Mirror it here; the game then rotates the delta by the character's
    # current yaw before adding it to the original CameraTrack.
    raw_position = _blender_to_ue_vector(
        [float(value) * 100.0 for value in position_delta])
    raw_rotation = _blender_to_ue_quaternion(
        [rotation_delta.x, rotation_delta.y, rotation_delta.z, rotation_delta.w])
    return {
        "delta_position": raw_position,
        "delta_rotation": raw_rotation,
        "lens_scale": lens_scale,
    }


def _nested_value(owner, path):
    value = owner
    for part in path.split("."):
        value = getattr(value, part)
    return value.copy() if hasattr(value, "copy") else float(value)


def _set_nested_value(owner, path, value):
    parent = owner
    parts = path.split(".")
    for part in parts[:-1]:
        parent = getattr(parent, part)
    setattr(parent, parts[-1], value)


def _path_default(owner, path):
    parent = owner
    parts = path.split(".")
    for part in parts[:-1]:
        parent = getattr(parent, part)
    prop = parent.bl_rna.properties[parts[-1]]
    return list(prop.default_array) if prop.is_array else prop.default


def _camera_correction_action(animation_data):
    action = animation_data.action if animation_data else None
    if action and action.get("betterpose_camera_correction", False):
        return action
    if animation_data is None:
        return None
    return _stored_action(animation_data.id_data,
                          "betterpose_camera_correction_action",
                          "betterpose_camera_correction")


def _copy_fcurve(target_action, source_curve, path, group):
    curve = target_action.fcurves.new(data_path=path,
                                      index=source_curve.array_index,
                                      action_group=group)
    for source in source_curve.keyframe_points:
        point = curve.keyframe_points.insert(source.co.x, source.co.y)
        point.interpolation = source.interpolation
        point.handle_left_type = source.handle_left_type
        point.handle_right_type = source.handle_right_type
        point.handle_left = source.handle_left
        point.handle_right = source.handle_right
    curve.update()
    return curve


def _base_action(animation_data):
    if animation_data is None:
        return None
    current = animation_data.action
    if (current and not current.get("betterpose_correction", False) and
            not current.get("betterpose_camera_correction", False)):
        return current
    for track in animation_data.nla_tracks:
        for strip in track.strips:
            if (strip.action and not strip.action.get("betterpose_correction", False) and
                    not strip.action.get("betterpose_camera_correction", False)):
                return strip.action
    return None


def _split_legacy_camera_data(camera):
    data_animation = camera.data.animation_data_create()
    if data_animation.action or data_animation.nla_tracks:
        return
    source = _base_action(camera.animation_data)
    if source is None:
        return
    paths = {
        "data.lens": "lens",
        "data.dof.focus_distance": "dof.focus_distance",
    }
    curves = [curve for curve in source.fcurves if curve.data_path in paths]
    if not curves:
        return
    base = bpy.data.actions.new("BetterPose Camera Lens Base")
    base["betterpose_camera_base"] = True
    base.use_fake_user = True
    for curve in list(curves):
        _copy_fcurve(base, curve, paths[curve.data_path], "原镜头")
        source.fcurves.remove(curve)
    data_animation.action = base


def _push_camera_base(owner, animation_data):
    base = animation_data.action
    if base is None or base.get("betterpose_camera_correction", False):
        return
    base["betterpose_camera_base"] = True
    base.use_fake_user = True
    owner["betterpose_camera_base_action"] = base.name
    existing = [(track, strip) for track in animation_data.nla_tracks
                for strip in track.strips if strip.action == base]
    if not existing:
        track = animation_data.nla_tracks.new()
        track.name = "BetterPose Camera Base"
        strip = track.strips.new(base.name, int(round(base.frame_range[0])), base)
        strip.name = "原镜头"
        strip.extrapolation = "HOLD"
        strip.blend_type = "REPLACE"
        strip.influence = 1.0
        track.mute = False
    else:
        for track, strip in existing:
            track.mute = False
            strip.blend_type = "REPLACE"
            strip.extrapolation = "HOLD"
            strip.influence = 1.0
    animation_data.action = None


def _camera_corrections_ready(camera):
    for owner, _, _ in _camera_owners(camera):
        animation_data = owner.animation_data
        action = _camera_correction_action(animation_data)
        if (action is None or
                (animation_data.action == action and
                 animation_data.action_blend_type != "COMBINE") or
                any(curve.data_path.startswith("data.") for curve in action.fcurves)):
            return False
    return True


def _collect_legacy_camera_keys(camera):
    keys = {}
    for owner, paths, _ in _camera_owners(camera):
        animation_data = owner.animation_data
        action = _camera_correction_action(animation_data)
        if action is None:
            continue
        for curve in action.fcurves:
            path = curve.data_path
            target = owner
            if path.startswith("data.") and owner == camera:
                path = path[5:]
                target = camera.data
            target_paths = (("lens", "dof.focus_distance")
                            if target == camera.data else paths)
            if path not in target_paths:
                continue
            key = ("data" if target == camera.data else "object",
                   path, curve.array_index)
            keys.setdefault(key, []).extend(
                (point.co.x, point.co.y, point.interpolation)
                for point in curve.keyframe_points)
    return keys


def _ensure_camera_correction_layer(scene):
    camera = scene.camera
    if camera is None:
        raise ValueError("当前场景没有可编辑镜头")
    if _camera_corrections_ready(camera):
        corrections = {
            "object": _camera_correction_action(camera.animation_data),
            "data": _camera_correction_action(camera.data.animation_data),
        }
        for owner, _, _ in _camera_owners(camera):
            animation_data = owner.animation_data_create()
            target = "data" if owner == camera.data else "object"
            animation_data.action = corrections[target]
            animation_data.action_blend_type = "COMBINE"
            animation_data.action_extrapolation = "HOLD"
            animation_data.action_influence = 1.0
            owner.update_tag()
        return camera, corrections

    captured = {
        owner: {path: _nested_value(owner, path) for path in paths}
        for owner, paths, _ in _camera_owners(camera)
    }
    _split_legacy_camera_data(camera)
    for owner, _, _ in _camera_owners(camera):
        _push_camera_base(owner, owner.animation_data_create())

    legacy_keys = _collect_legacy_camera_keys(camera)
    base_values = {}
    sampled_frames = sorted({frame for values in legacy_keys.values()
                             for frame, _, _ in values})
    influences = []
    for owner, _, _ in _camera_owners(camera):
        animation_data = owner.animation_data
        if animation_data and _camera_correction_action(animation_data):
            influences.append((animation_data, animation_data.action_influence))
            animation_data.action_influence = 0.0
    try:
        for frame in sampled_frames:
            scene.frame_set(int(frame), subframe=frame - int(frame))
            bpy.context.view_layer.update()
            for owner, paths, _ in _camera_owners(camera):
                target = "data" if owner == camera.data else "object"
                for path in paths:
                    base_values[(target, path, int(frame))] = _nested_value(owner, path)
    finally:
        for animation_data, influence in influences:
            animation_data.action_influence = influence

    corrections = {}
    for owner, paths, label in _camera_owners(camera):
        animation_data = owner.animation_data_create()
        correction = bpy.data.actions.new("BetterPose Camera " + label + " Correction")
        correction["betterpose_camera_correction"] = True
        correction.use_fake_user = True
        owner["betterpose_camera_correction_action"] = correction.name
        animation_data.action = correction
        animation_data.action_blend_type = "COMBINE"
        animation_data.action_extrapolation = "HOLD"
        animation_data.action_influence = 1.0
        target = "data" if owner == camera.data else "object"
        corrections[target] = correction
        for key, points in legacy_keys.items():
            key_target, path, index = key
            if key_target != target or path not in paths:
                continue
            curve = correction.fcurves.new(data_path=path, index=index,
                                           action_group="镜头修正")
            neutral = _path_default(owner, path)[index] if isinstance(
                _path_default(owner, path), (list, tuple)) else _path_default(owner, path)
            for frame, desired, interpolation in points:
                base = base_values.get((target, path, int(frame)))
                if base is None:
                    continue
                base_value = base[index] if hasattr(base, "__len__") else base
                raw = neutral + float(desired) - float(base_value)
                point = curve.keyframe_points.insert(frame, raw)
                point.interpolation = interpolation
            if (len(curve.keyframe_points) != 0 and
                    min(point.co.x for point in curve.keyframe_points) > scene.frame_start):
                curve.keyframe_points.insert(scene.frame_start, neutral)
            curve.update()

    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    bpy.context.view_layer.update()
    for owner, paths, _ in _camera_owners(camera):
        for path, value in captured[owner].items():
            _set_nested_value(owner, path, value)
        owner.update_tag()
    bpy.context.view_layer.update()
    return camera, corrections


def _enter_camera_base_edit(scene):
    camera = scene.camera
    if (getattr(scene, "bp_camera_edit_base", False) and camera and camera.animation_data and
            camera.animation_data.use_tweak_mode):
        return camera
    camera, _ = _ensure_camera_correction_layer(scene)
    object_base = _base_action(camera.animation_data)
    data_base = _base_action(camera.data.animation_data)
    if object_base is None or data_base is None:
        raise ValueError("找不到原镜头 NLA 层")
    data_animation = camera.data.animation_data_create()
    data_animation.action = data_base
    data_animation.action_blend_type = "REPLACE"
    data_animation.action_extrapolation = "HOLD"
    data_animation.action_influence = 1.0
    if not _enter_layer_tweak(scene, camera, object_base):
        raise ValueError("无法进入原镜头 NLA 编辑")
    scene.bp_camera_edit_base = True
    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    camera.update_tag()
    camera.data.update_tag()
    bpy.context.view_layer.update()
    return camera


def _enter_camera_correction_edit(scene):
    _leave_layer_tweak(scene)
    camera, _ = _ensure_camera_correction_layer(scene)
    scene.bp_camera_edit_base = False
    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    camera.update_tag()
    camera.data.update_tag()
    bpy.context.view_layer.update()
    return camera


def _key_camera_base(scene):
    camera = (_enter_camera_base_edit(scene)
              if not getattr(scene, "bp_camera_edit_base", False) else scene.camera)
    if camera is None:
        raise ValueError("当前场景没有可编辑镜头")
    frame = scene.frame_current
    bases = {
        "object": _base_action(camera.animation_data),
        "data": _base_action(camera.data.animation_data),
    }
    for owner, paths, _ in _camera_owners(camera):
        target = "data" if owner == camera.data else "object"
        action = bases[target]
        if action is None:
            raise ValueError("找不到原镜头层")
        for path in paths:
            desired = _nested_value(owner, path)
            value_count = len(desired) if hasattr(desired, "__len__") else 1
            for index in range(value_count):
                value = desired[index] if value_count > 1 else desired
                curve = next((item for item in action.fcurves
                              if item.data_path == path and item.array_index == index),
                             None)
                if curve is None:
                    curve = action.fcurves.new(path, index=index,
                                               action_group="原镜头")
                point = next((item for item in curve.keyframe_points
                              if abs(item.co.x - frame) < 1e-5), None)
                if point is None:
                    point = curve.keyframe_points.insert(frame, float(value))
                else:
                    point.co.y = float(value)
                point.interpolation = "BEZIER"
                point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
                curve.update()
        action.update_tag()
        owner.update_tag()
    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    bpy.context.view_layer.update()
    return camera


def _key_camera(scene):
    camera, corrections = _ensure_camera_correction_layer(scene)
    frame = scene.frame_current
    desired = {
        owner: {path: _nested_value(owner, path) for path in paths}
        for owner, paths, _ in _camera_owners(camera)
    }
    influences = [(owner.animation_data, owner.animation_data.action_influence)
                  for owner, _, _ in _camera_owners(camera)
                  if owner.animation_data]
    base_values = {}
    try:
        for animation_data, _ in influences:
            animation_data.action_influence = 0.0
        scene.frame_set(frame, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
        for owner, paths, _ in _camera_owners(camera):
            for path in paths:
                base_values[(owner, path)] = _nested_value(owner, path)
    finally:
        for animation_data, influence in influences:
            animation_data.action_influence = influence
        scene.frame_set(frame, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()

    for owner, paths, _ in _camera_owners(camera):
        action = corrections["data" if owner == camera.data else "object"]
        owner_values = desired[owner]
        for path, desired_value in owner_values.items():
            _set_nested_value(owner, path, desired_value)
        for path in paths:
            defaults = _path_default(owner, path)
            default_values = list(defaults) if hasattr(defaults, "__len__") else [defaults]
            desired_value = desired[owner][path]
            value_count = len(desired_value) if hasattr(desired_value, "__len__") else 1
            base_value = base_values[(owner, path)]
            for index in range(value_count):
                value = desired_value[index] if value_count > 1 else desired_value
                base = base_value[index] if hasattr(base_value, "__len__") else base_value
                raw = default_values[index] + float(value) - float(base)
                curve = next((item for item in action.fcurves
                              if item.data_path == path and item.array_index == index),
                             None)
                if curve is None:
                    curve = action.fcurves.new(path, index=index,
                                               action_group="镜头修正")
                point = next((item for item in curve.keyframe_points
                              if abs(item.co.x - frame) < 1e-5), None)
                if point is None:
                    point = curve.keyframe_points.insert(frame, raw)
                else:
                    point.co.y = raw
                point.interpolation = "BEZIER"
                point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
                if (len(curve.keyframe_points) == 1 and
                        frame > scene.frame_start):
                    curve.keyframe_points.insert(scene.frame_start,
                                                 default_values[index])
                for point in curve.keyframe_points:
                    if point.co.x in (frame, scene.frame_start):
                        point.interpolation = "BEZIER"
                        point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
                curve.update()
        action.update_tag()
        owner.update_tag()
    scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
    for owner, _, _ in _camera_owners(camera):
        for path, value in desired[owner].items():
            _set_nested_value(owner, path, value)
    bpy.context.view_layer.update()
    return camera


def _restore_camera_to_base(scene):
    camera, corrections = _ensure_camera_correction_layer(scene)
    animations = [owner.animation_data for owner, _, _ in _camera_owners(camera)
                  if owner.animation_data]
    old_influences = [animation_data.action_influence
                      for animation_data in animations]
    try:
        for animation_data in animations:
            animation_data.action_influence = 0.0
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
        restored = {
            owner: {path: _nested_value(owner, path) for path in paths}
            for owner, paths, _ in _camera_owners(camera)
        }
    finally:
        for animation_data, influence in zip(animations, old_influences):
            animation_data.action_influence = influence
        scene.frame_set(scene.frame_current, subframe=scene.frame_subframe)
        bpy.context.view_layer.update()
    for owner, paths, _ in _camera_owners(camera):
        for path, value in restored[owner].items():
            _set_nested_value(owner, path, value)
        owner.update_tag()
    bpy.context.view_layer.update()
    return camera


def _show_camera_action(context, owner, action, open_editor=False, filter_name=""):
    if action is None:
        return False
    if owner is not None:
        if context.object and context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.context.view_layer.objects.active = owner
        owner.select_set(True)
    editors = [area for area in context.screen.areas
               if area.type == "DOPESHEET_EDITOR"]
    if not editors and open_editor:
        timeline = next((area for area in context.screen.areas
                         if area.type == "TIMELINE"), None)
        if timeline is not None:
            timeline.type = "DOPESHEET_EDITOR"
            editors = [timeline]
    for area in editors:
        space = area.spaces.active
        if hasattr(space, "ui_mode"):
            space.ui_mode = "ACTION"
        if hasattr(space, "action"):
            space.action = action
            space.dopesheet.show_only_selected = False
            space.dopesheet.show_hidden = True
            space.dopesheet.filter_fcurve_name = filter_name
    return bool(editors)


def _document_from_scene(scene, armature):
    if not armature or armature.type != "ARMATURE":
        raise ValueError("请先导入 BetterPose 骨架")
    names = [bone.name for bone in armature.data.bones]
    indices = {name: index for index, name in enumerate(names)}
    parents = [indices.get(bone.parent.name, -1) if bone.parent else -1 for bone in armature.data.bones]
    bridge_text = armature.get("betterpose_document", "")
    bridge_document = json.loads(bridge_text) if bridge_text else None
    output_names = (bridge_document.get("game_bone_names") if bridge_document else None) or names
    output_parents = (bridge_document.get("game_bone_parents") if bridge_document else None) or parents
    fps = scene.render.fps / scene.render.fps_base
    frames = []
    face_names = list(armature.get("betterpose_face_names", []))
    rest_local_positions = (_document_rest_state(bridge_document)[3]
                            if bridge_document else [])
    previous_signature = None
    for frame_number in range(scene.frame_start, scene.frame_end + 1):
        scene.frame_set(frame_number)
        time = (frame_number - scene.frame_start) / fps
        bones = []
        if bridge_document:
            rotations, positions = _live_game_pose(armature, bridge_document)
            for index in sorted(set(rotations) | set(positions)):
                rotation = rotations.get(index, [0.0, 0.0, 0.0])
                position = positions.get(index)
                item = {}
                if any(abs(value) > 1e-5 for value in rotation):
                    item["r"] = rotation
                if position is not None and index < len(rest_local_positions):
                    rest = _blender_to_ue_vector([
                        value * 100.0 for value in rest_local_positions[index]])
                    # Imported actions key bone location even when it remains
                    # at rest. Reconstructing game-local positions from those
                    # rotating component transforms introduced a non-zero
                    # pelvis-to-spine "translation" on every frame. Exporting
                    # that absolute source-rig length moved another
                    # character's shoulders and made hands clip into the
                    # chest. matrix_basis is the actual Blender translation
                    # channel, so only real edits become bone offsets.
                    translated = (
                        armature.pose.bones[index].matrix_basis.translation
                        .length_squared > 1e-8)
                    if translated and any(
                            abs(position[axis] - rest[axis]) > 1e-3
                            for axis in range(3)):
                        # Game import adds this delta to the target rig's own
                        # reference local position, keeping body proportions
                        # owned by the character that plays the action.
                        item["p"] = [
                            position[axis] - rest[axis] for axis in range(3)]
                if item:
                    game_index = armature.pose.bones[index].bone.get(
                        "betterpose_game_index", index)
                    item["i"] = int(game_index)
                    bones.append(item)
        else:
            for index, bone in enumerate(armature.pose.bones):
                rotation = [math.degrees(value) for value in bone.rotation_euler]
                if any(abs(value) > 1e-5 for value in rotation):
                    game_index = bone.bone.get("betterpose_game_index", index)
                    bones.append({"i": int(game_index), "r": rotation})
        root = _blender_to_ue_vector([value * 100.0 for value in armature.location])
        faces = {}
        for name in face_names:
            key = "betterpose.face." + name
            if key in armature:
                faces[name] = max(0.0, min(1.0, float(armature[key])))
        camera = _camera_key(scene, scene.camera)
        signature = (
            tuple((int(item["i"]),
                   tuple(round(float(value), 6) for value in item.get("r", [])),
                   tuple(round(float(value), 6) for value in item.get("p", [])))
                  for item in bones),
            tuple(round(float(value), 6) for value in root),
            tuple((name, round(float(value), 6))
                  for name, value in sorted(faces.items())),
            None if camera is None else tuple(
                (key, tuple(round(float(value), 6) for value in camera[key])
                 if isinstance(camera[key], list) else round(float(camera[key]), 6))
                for key in sorted(camera)),
        )
        if (frame_number == scene.frame_start or
                frame_number == scene.frame_end or
                signature != previous_signature):
            frames.append({"time": time, "root": root, "bones": bones,
                           "faces": faces, "camera": camera})
            previous_signature = signature
    if not frames:
        frames = [{"time": 0.0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None}]
    result = {"format": clip_io.FORMAT, "version": clip_io.VERSION,
              "model": scene.get("betterpose_model", ""), "fps": fps,
              "length": scene.frame_end - scene.frame_start,
              "bone_names": output_names, "bone_parents": output_parents,
              "bone_position_mode": "delta",
              "face_names": face_names, "frames": frames}
    source_camera = _camera_track_document(
        bridge_document.get("camera_track") if bridge_document else None)
    if source_camera is not None:
        result["camera_track"] = source_camera
    return result


def _read_motion_file(path):
    with Path(path).open("r", encoding="utf-8-sig") as stream:
        source = json.load(stream)
    document = clip_io.read_document(source)
    camera_track = _camera_track_document(source.get("camera_track"))
    if camera_track is not None:
        document["camera_track"] = camera_track
    return document


def _write_motion_file(path, document):
    clean = clip_io.read_document(document)
    camera_track = _camera_track_document(document.get("camera_track"))
    if camera_track is not None:
        clean["camera_track"] = camera_track
    Path(path).write_text(
        json.dumps(clean, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")


class BP_OT_import(bpy.types.Operator, ImportHelper):
    bl_idname = "betterpose.import_motion"
    bl_label = "导入 .bpmotion"
    filename_ext = ".bpmotion"
    filter_glob: StringProperty(default="*.bpmotion", options={"HIDDEN"})

    def execute(self, context):
        try:
            document = _read_motion_file(self.filepath)
            old = _armature(context.scene)
            if old:
                bpy.data.objects.remove(old, do_unlink=True)
            armature = _create_armature(document)
            context.scene.bp_armature = armature
            context.scene["betterpose_model"] = document["model"]
            armature["betterpose_document"] = json.dumps(document, ensure_ascii=False)
            _apply_document(context.scene, armature, document)
            context.scene.bp_status = "已导入 .bpmotion，可在 Blender 中编辑后导出"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "导入失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_connect(bpy.types.Operator):
    bl_idname = "betterpose.connect"
    bl_label = "连接游戏"

    def execute(self, context):
        try:
            body = {}
            if context.scene.camera is not None:
                camera_track = _camera_track_payload(context.scene)
                if camera_track is not None:
                    body["camera_track"] = camera_track
            _transport().request("begin", body, _on_begin)
            context.scene.bp_status = "正在连接游戏…"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}


class BP_OT_disconnect(bpy.types.Operator):
    bl_idname = "betterpose.disconnect"
    bl_label = "断开并恢复游戏"

    def execute(self, context):
        global _connected, _session, _last_sync_frame, _last_sync_time
        global _last_packet_signature, _last_heartbeat
        if _connected:
            _transport().request("end", {"session": _session}, _on_command)
        _connected = False
        _last_sync_frame = None
        _last_sync_time = 0.0
        _last_packet_signature = None
        _last_heartbeat = 0.0
        if "betterpose_camera_delta_baseline" in context.scene:
            del context.scene["betterpose_camera_delta_baseline"]
        context.scene.bp_sync_camera = False
        context.scene.bp_live = False
        context.scene.bp_status = "已请求断开；游戏将恢复原姿态"
        return {"FINISHED"}


class BP_OT_pull(bpy.types.Operator):
    bl_idname = "betterpose.pull_motion"
    bl_label = "拉取当前 MMD 动作"

    def execute(self, context):
        if _connected:
            self.report({"ERROR"}, "请先断开当前 Blender 编辑会话")
            return {"CANCELLED"}
        try:
            _transport().request("begin", {"mode": "pull"}, _on_pull_begin)
            context.scene.bp_status = "正在准备拉取当前 MMD 动作…"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}


class BP_OT_export(bpy.types.Operator, ExportHelper):
    bl_idname = "betterpose.export_motion"
    bl_label = "导出 .bpmotion（可离线载入 MMD）"
    filename_ext = ".bpmotion"
    filter_glob: StringProperty(default="*.bpmotion", options={"HIDDEN"})

    def execute(self, context):
        try:
            document = _document_from_scene(context.scene, _armature(context.scene))
            _write_motion_file(self.filepath, document)
            context.scene.bp_status = "已导出 .bpmotion，可在 BetterPose 的 MMD 页离线载入"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "导出失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_edit_pose_layer(bpy.types.Operator):
    bl_idname = "betterpose.edit_pose_layer"
    bl_label = "编辑动作层"
    bl_description = "切换编辑原动作或叠加修正层"
    target: StringProperty(default="correction")

    def execute(self, context):
        armature = _armature(context.scene)
        if armature is None:
            self.report({"ERROR"}, "请先导入 BetterPose 骨架")
            return {"CANCELLED"}
        try:
            if context.scene.bp_camera_edit_base:
                _enter_camera_correction_edit(context.scene)
            if self.target == "base":
                action = _enter_pose_base_edit(context.scene, armature)
                label = "原动作"
            else:
                action = _enter_pose_correction_edit(context.scene, armature)
                label = "动作修正层"
            if context.object and context.object.mode != "OBJECT":
                bpy.ops.object.mode_set(mode="OBJECT")
            bpy.context.view_layer.objects.active = armature
            armature.select_set(True)
            bpy.ops.object.mode_set(mode="POSE")
            _show_camera_action(context, armature, action, open_editor=True)
            context.scene.bp_status = "正在编辑" + label
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "切换动作层失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_key_selected(bpy.types.Operator):
    bl_idname = "betterpose.key_selected"
    bl_label = "给选中关节打关键帧"
    bl_description = "只记录选中的身体关节；首次使用会自动建立修正层"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        armature = _armature(context.scene)
        selected = _selected_edit_bones(armature) if armature else []
        if not selected:
            self.report({"ERROR"}, "请先在 Pose 模式选中至少一个关节")
            return {"CANCELLED"}
        try:
            _key_selected_pose(context.scene, armature, selected)
            layer = "原动作" if context.scene.bp_pose_edit_base else "动作修正层"
            context.scene.bp_status = f"已给 {len(selected)} 个关节写入{layer}"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "打关键帧失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_restore_selected(bpy.types.Operator):
    bl_idname = "betterpose.restore_selected"
    bl_label = "恢复选中关节状态"
    bl_description = "把选中关节恢复到下层原动作；随后打关键帧即可平滑回退"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        armature = _armature(context.scene)
        selected = _selected_edit_bones(armature) if armature else []
        if not selected:
            self.report({"ERROR"}, "请先在 Pose 模式选中至少一个关节")
            return {"CANCELLED"}
        if context.scene.bp_pose_edit_base:
            context.scene.bp_status = "当前已在编辑原动作，不需要恢复下层"
            return {"FINISHED"}
        try:
            _restore_selected_to_base(context.scene, armature, selected)
            context.scene.bp_status = "已恢复选中关节；再点“给选中关节打关键帧”保存恢复点"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "恢复关节失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_face_key(bpy.types.Operator):
    bl_idname = "betterpose.face_key"
    bl_label = "表情打关键帧"
    bl_description = "把全部 MMD 表情当前权重写入原动作层"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        armature = _armature(context.scene)
        if not armature:
            self.report({"ERROR"}, "请先导入 BetterPose 骨架")
            return {"CANCELLED"}
        try:
            count = _key_faces(context.scene, armature)
            context.scene.bp_status = f"已将 {count} 个 MMD 表情写入原动作层"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "表情打帧失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_face_zero(bpy.types.Operator):
    bl_idname = "betterpose.face_zero"
    bl_label = "表情归零"
    bl_description = "把全部 MMD 表情权重恢复为零"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        armature = _armature(context.scene)
        if not armature:
            return {"CANCELLED"}
        for name in armature.get("betterpose_face_names", []):
            key = "betterpose.face." + name
            if key in armature:
                armature[key] = 0.0
        context.scene.bp_status = "MMD 表情已归零"
        return {"FINISHED"}


class BP_OT_show_face_keys(bpy.types.Operator):
    bl_idname = "betterpose.show_face_keys"
    bl_label = "查看表情关键帧"
    bl_description = "切换到 Action Editor 并只显示 BetterPose 表情曲线"

    def execute(self, context):
        armature = _armature(context.scene)
        if not armature:
            return {"CANCELLED"}
        action = _pose_base_action(armature)
        if action is None:
            self.report({"INFO"}, "当前没有原动作层")
            return {"CANCELLED"}
        curve_count = sum("betterpose.face." in curve.data_path
                          for curve in action.fcurves)
        if curve_count == 0:
            self.report({"INFO"}, "原动作层还没有表情关键帧")
            return {"CANCELLED"}
        shown = _show_camera_action(
            context, armature, action, open_editor=True,
            filter_name="betterpose.face.")
        if not shown:
            self.report({"INFO"}, "没有找到可切换的 Timeline 或 Dope Sheet 窗口")
            return {"CANCELLED"}
        context.scene.bp_status = f"正在查看 {curve_count} 条表情曲线"
        return {"FINISHED"}


class BP_OT_show_camera_keys(bpy.types.Operator):
    bl_idname = "betterpose.show_camera_keys"
    bl_label = "查看镜头关键帧"
    bl_description = "切换编辑原镜头或镜头修正层"
    target: StringProperty(default="base")

    def execute(self, context):
        camera = context.scene.camera
        if camera is None:
            self.report({"ERROR"}, "当前场景没有可编辑镜头")
            return {"CANCELLED"}
        try:
            if self.target == "correction":
                camera = _enter_camera_correction_edit(context.scene)
                action = _camera_correction_action(camera.animation_data)
                label = "镜头修正层"
            else:
                camera = _enter_camera_base_edit(context.scene)
                action = _base_action(camera.animation_data)
                label = "原镜头层"
        except Exception as exc:
            self.report({"ERROR"}, "切换镜头层失败：" + str(exc))
            return {"CANCELLED"}
        if action is None:
            self.report({"INFO"}, label + "还没有关键帧")
            return {"CANCELLED"}
        _show_camera_action(context, camera, action, open_editor=True)
        context.scene.bp_status = f"正在查看{label}：{action.name}"
        return {"FINISHED"}


class BP_OT_edit_camera(bpy.types.Operator):
    bl_idname = "betterpose.edit_camera"
    bl_label = "编辑镜头"
    bl_description = "选中当前镜头并编辑原镜头或修正层"
    target: StringProperty(default="correction")

    def execute(self, context):
        camera = context.scene.camera
        if camera is None:
            self.report({"ERROR"}, "当前场景没有可编辑镜头")
            return {"CANCELLED"}
        try:
            armature = _armature(context.scene)
            if context.scene.bp_pose_edit_base and armature is not None:
                _enter_pose_correction_edit(context.scene, armature)
            if self.target == "base":
                camera = _enter_camera_base_edit(context.scene)
                action = _base_action(camera.animation_data)
                label = "原镜头"
            else:
                camera = _enter_camera_correction_edit(context.scene)
                action = _camera_correction_action(camera.animation_data)
                label = "镜头修正层"
            if context.object and context.object.mode != "OBJECT":
                bpy.ops.object.mode_set(mode="OBJECT")
            bpy.context.view_layer.objects.active = camera
            camera.select_set(True)
            _show_camera_action(context, camera, action)
            context.scene.bp_status = "正在编辑" + label + "；使用 G / R 调整后打关键帧"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "切换镜头层失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_create_camera(bpy.types.Operator):
    bl_idname = "betterpose.create_camera"
    bl_label = "新建预览镜头"
    bl_description = "创建 BetterPose Camera 并设为当前场景镜头"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        camera_data = bpy.data.cameras.new("BetterPose Camera")
        camera = bpy.data.objects.new("BetterPose Camera", camera_data)
        context.collection.objects.link(camera)
        camera["betterpose_managed_camera"] = True
        camera.location = (0.0, -4.0, 1.3)
        camera.rotation_euler = (math.radians(90.0), 0.0, 0.0)
        camera.data.lens = 50.0
        context.scene.camera = camera
        bpy.context.view_layer.objects.active = camera
        camera.select_set(True)
        context.scene.bp_status = "已新建预览镜头；可用 G / R 调整"
        return {"FINISHED"}


class BP_OT_camera_key(bpy.types.Operator):
    bl_idname = "betterpose.camera_key"
    bl_label = "镜头打关键帧"
    bl_description = "把镜头位置、旋转、焦距和对焦距离写入镜头修正层"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            if context.scene.bp_camera_edit_base:
                camera = _key_camera_base(context.scene)
                action = _base_action(camera.animation_data)
                status = "原镜头关键帧已写入"
            else:
                camera = _key_camera(context.scene)
                action = _camera_correction_action(camera.animation_data)
                status = "镜头修正关键帧已写入；原镜头保留在 NLA 底层"
            _show_camera_action(context, camera, action)
            context.scene.bp_status = status
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "镜头打帧失败：" + str(exc))
            return {"CANCELLED"}


class BP_OT_camera_restore(bpy.types.Operator):
    bl_idname = "betterpose.camera_restore"
    bl_label = "恢复下层镜头"
    bl_description = "恢复当前帧的底层镜头；随后打帧即可平滑返回"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            if context.scene.bp_camera_edit_base:
                _enter_camera_correction_edit(context.scene)
            _restore_camera_to_base(context.scene)
            context.scene.bp_status = "已恢复底层镜头；再点“镜头打关键帧”保存"
            return {"FINISHED"}
        except Exception as exc:
            self.report({"ERROR"}, "恢复镜头失败：" + str(exc))
            return {"CANCELLED"}


class BP_PT_panel(bpy.types.Panel):
    bl_label = "BetterPose"
    bl_idname = "BP_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "BetterPose"

    def draw(self, context):
        layout = self.layout
        layout.label(text="MMD / Blender 动作交换")
        layout.prop(context.scene, "bp_armature", text="骨架")
        armature = context.scene.bp_armature
        if armature and armature.animation_data and armature.animation_data.action:
            layout.label(text="当前层：" + armature.animation_data.action.name)
        row = layout.row(align=True)
        base_action = row.operator(BP_OT_edit_pose_layer.bl_idname,
                                   text="编辑原动作", icon="ACTION")
        base_action.target = "base"
        correction_action = row.operator(BP_OT_edit_pose_layer.bl_idname,
                                         text="编辑修正动作", icon="MODIFIER")
        correction_action.target = "correction"
        if armature:
            layout.label(text="当前模式：" +
                         ("原动作" if context.scene.bp_pose_edit_base else "动作修正层"))
        layout.operator(BP_OT_key_selected.bl_idname, icon="KEY_HLT")
        layout.operator(BP_OT_restore_selected.bl_idname, icon="LOOP_BACK")
        face_names = list(armature.get("betterpose_face_names", [])) if armature else []
        if face_names:
            layout.separator()
            expanded = context.scene.bp_show_faces
            layout.prop(context.scene, "bp_show_faces",
                        text=f"MMD 表情（{len(face_names)}）",
                        icon="TRIA_DOWN" if expanded else "TRIA_RIGHT")
            if expanded:
                box = layout.box()
                box.prop(context.scene, "bp_face_search", text="搜索")
                query = context.scene.bp_face_search.casefold()
                shown = 0
                for name in face_names:
                    if query and query not in name.casefold():
                        continue
                    key = "betterpose.face." + name
                    if key in armature:
                        box.prop(armature, f'["{key}"]', text=name, slider=True)
                    shown += 1
                    if shown >= 32:
                        box.label(text="继续输入名称可筛选更多表情")
                        break
                row = box.row(align=True)
                row.operator(BP_OT_face_key.bl_idname, icon="KEY_HLT")
                row.operator(BP_OT_face_zero.bl_idname, icon="LOOP_BACK")
                base_action = _pose_base_action(armature)
                face_curves = (sum("betterpose.face." in curve.data_path
                                   for curve in base_action.fcurves)
                               if base_action else 0)
                box.label(text=f"原动作表情曲线：{face_curves}")
                box.label(text="表情关键帧只写入原动作层")
                box.operator(BP_OT_show_face_keys.bl_idname, icon="ACTION")
        camera = context.scene.camera
        layout.separator()
        expanded = context.scene.bp_show_camera
        layout.prop(context.scene, "bp_show_camera", text="镜头编辑",
                    icon="TRIA_DOWN" if expanded else "TRIA_RIGHT")
        if expanded:
            box = layout.box()
            if camera is None:
                box.label(text="当前场景没有镜头")
                box.operator(BP_OT_create_camera.bl_idname, icon="ADD")
            else:
                box.label(text="当前镜头：" + camera.name)
                row = box.row(align=True)
                base_edit = row.operator(BP_OT_edit_camera.bl_idname,
                                         text="编辑原镜头", icon="CAMERA_DATA")
                base_edit.target = "base"
                correction_edit = row.operator(BP_OT_edit_camera.bl_idname,
                                               text="编辑修正", icon="MODIFIER")
                correction_edit.target = "correction"
                row = box.row(align=True)
                row.operator(BP_OT_camera_key.bl_idname, icon="KEY_HLT")
                if not context.scene.bp_camera_edit_base:
                    row.operator(BP_OT_camera_restore.bl_idname, icon="LOOP_BACK")
                base_action = _base_action(camera.animation_data)
                base_keys = (sum(len(curve.keyframe_points)
                                 for curve in base_action.fcurves)
                             if base_action else 0)
                correction_action = _camera_correction_action(camera.animation_data)
                correction_keys = (sum(len(curve.keyframe_points)
                                       for curve in correction_action.fcurves)
                                   if correction_action else 0)
                box.label(text=f"原镜头层：{base_action.name if base_action else '无'}"
                               f"（{base_keys} 个关键帧）")
                box.label(text=f"修正层：{correction_action.name if correction_action else '无'}"
                               f"（{correction_keys} 个关键帧）")
                box.label(text="当前模式：" +
                               ("原镜头" if context.scene.bp_camera_edit_base else "镜头修正层"))
                box.label(text="默认播放游戏原始 CameraTrack")
                box.prop(context.scene, "bp_sync_camera",
                         text="将 Blender 最终镜头同步到游戏")
        if not _connected:
            layout.operator(BP_OT_connect.bl_idname, icon="LINKED")
            layout.operator(BP_OT_pull.bl_idname, icon="IMPORT")
        else:
            layout.operator(BP_OT_disconnect.bl_idname, icon="UNLINKED")
            layout.prop(context.scene, "bp_live", text="实时同步到游戏")
        layout.operator(BP_OT_import.bl_idname, icon="IMPORT")
        layout.operator(BP_OT_export.bl_idname, icon="EXPORT")
        if context.scene.bp_status:
            layout.label(text=context.scene.bp_status)


CLASSES = (BP_OT_import, BP_OT_export, BP_OT_edit_pose_layer,
           BP_OT_key_selected, BP_OT_restore_selected,
           BP_OT_face_key, BP_OT_face_zero, BP_OT_show_face_keys,
           BP_OT_show_camera_keys, BP_OT_edit_camera, BP_OT_create_camera, BP_OT_camera_key,
           BP_OT_camera_restore,
           BP_OT_connect, BP_OT_disconnect, BP_OT_pull, BP_PT_panel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.bp_armature = PointerProperty(type=bpy.types.Object)
    bpy.types.Scene.bp_status = StringProperty(default="")
    bpy.types.Scene.bp_live = bpy.props.BoolProperty(default=False)
    bpy.types.Scene.bp_face_search = StringProperty(default="")
    bpy.types.Scene.bp_show_faces = bpy.props.BoolProperty(
        name="MMD 表情", default=False)
    bpy.types.Scene.bp_show_camera = bpy.props.BoolProperty(
        name="镜头编辑", default=False)
    bpy.types.Scene.bp_pose_edit_base = bpy.props.BoolProperty(
        name="编辑原动作", default=False)
    bpy.types.Scene.bp_camera_edit_base = bpy.props.BoolProperty(
        name="编辑原镜头", default=False)
    bpy.types.Scene.bp_sync_camera = bpy.props.BoolProperty(
        name="将 Blender 最终镜头同步到游戏", default=False)
    bpy.app.timers.register(_timer, first_interval=1.0 / 30.0, persistent=True)


def unregister():
    global _connected, _client
    _connected = False
    if _client:
        _client.close()
        _client = None
    if bpy.app.timers.is_registered(_timer):
        bpy.app.timers.unregister(_timer)
    del bpy.types.Scene.bp_live
    del bpy.types.Scene.bp_status
    del bpy.types.Scene.bp_armature
    del bpy.types.Scene.bp_pose_edit_base
    del bpy.types.Scene.bp_camera_edit_base
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
