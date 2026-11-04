import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))
from clip_io import read_document  # noqa: E402


def _valid_document():
    return {
        "format": "betterpose-blender-motion", "version": 1, "model": "demo",
        "fps": 30, "length": 30, "bone_names": ["Root", "Head"],
        "bone_parents": [-1, 0],
        "frames": [{"time": 0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None},
                   {"time": 1, "root": [1, 2, 3], "bones": [{"i": 1, "r": [1, 2, 3]}],
                    "faces": {"smile": 0.5}, "camera": None}],
    }


class ClipIoTests(unittest.TestCase):
    def test_round_trip_document(self):
        doc = _valid_document()
        clean = read_document(doc)
        self.assertEqual(clean["frames"][1]["bones"][0]["r"], [1.0, 2.0, 3.0])
        self.assertEqual(clean["frames"][1]["faces"]["smile"], 0.5)

    def test_accepts_lossless_quaternion_track(self):
        doc = _valid_document()
        doc["bone_positions"] = [[0, 0, 0], [0, 0, 100]]
        doc["bone_rotations"] = [[0, 0, 0, 1], [0, 0, 0, 1]]
        doc["bone_component_rotations"] = [[0, 0, 0, 1], [0, 0, 0, 1]]
        doc["bone_position_space"] = "component"
        doc["bone_position_mode"] = "delta"
        doc["face_names"] = ["smile", "blink"]
        doc["frames"][1]["bones"] = [{"i": 1, "q": [0, 0, 0.70710678, 0.70710678],
                                        "p": [0, 0, 99]}]
        clean = read_document(doc)
        self.assertEqual(clean["frames"][1]["bones"][0]["q"][2], 0.70710678)
        self.assertEqual(clean["frames"][1]["bones"][0]["p"][2], 99.0)
        self.assertEqual(clean["bone_positions"][1][2], 100.0)
        self.assertEqual(clean["bone_position_space"], "component")
        self.assertEqual(clean["bone_position_mode"], "delta")
        self.assertEqual(clean["face_names"], ["smile", "blink"])

    def test_rejects_non_monotonic_frames(self):
        doc = _valid_document()
        doc["frames"][1]["time"] = 0
        with self.assertRaises(ValueError):
            read_document(doc)


def test_round_trip_document():
    """Keep the functions callable for simple python -c smoke checks."""
    doc = {
        "format": "betterpose-blender-motion", "version": 1, "model": "demo",
        "fps": 30, "length": 30, "bone_names": ["Root", "Head"],
        "bone_parents": [-1, 0],
        "frames": [{"time": 0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None},
                   {"time": 1, "root": [1, 2, 3], "bones": [{"i": 1, "r": [1, 2, 3]}],
                    "faces": {"smile": 0.5}, "camera": None}],
    }
    clean = read_document(doc)
    assert clean["frames"][1]["bones"][0]["r"] == [1.0, 2.0, 3.0]
    assert clean["frames"][1]["faces"]["smile"] == 0.5


def test_rejects_non_monotonic_frames():
    doc = {"format": "betterpose-blender-motion", "version": 1, "model": "demo",
           "fps": 30, "length": 30, "bone_names": ["Root"], "bone_parents": [-1],
           "frames": [{"time": 0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None},
                      {"time": 0, "root": [0, 0, 0], "bones": [], "faces": {}, "camera": None}]}
    try:
        read_document(doc)
    except ValueError:
        return
    raise AssertionError("non-monotonic frames were accepted")
