#!/usr/bin/env python3
"""Independent checks for VMD Bezier decoding and keyframe sampling."""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmd2bip as mmd  # noqa: E402


def main() -> int:
    midpoint = 0.125 + 0.375 * (32.0 / 127.0)
    curve = (0.0, 0.0, 1.0, 32.0 / 127.0)
    assert abs(mmd.sample_vmd_curve(mmd.LINEAR_CURVE, 0.5) - 0.5) < 1e-12
    assert abs(mmd.sample_vmd_curve(curve, 0.5) - midpoint) < 1e-6

    interpolation = bytearray(16)
    interpolation[8] = interpolation[11] = 127
    interpolation[12] = interpolation[15] = 32
    curves = mmd.vmd_interpolation_curves(interpolation.hex())
    assert curves[0] == curve
    assert curves[3] == curve
    assert curves[1] == mmd.LINEAR_CURVE

    half = math.sqrt(0.5)
    tracks = {
        "center": {
            0: ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0),
                (mmd.LINEAR_CURVE,) * 4),
            10: ((0.0, 0.0, half, half), (10.0, 0.0, 0.0), curves),
        }
    }
    rotation, position = mmd.build_sampler(tracks)("center", 5.0)
    assert abs(position[0] - midpoint * 10.0) < 1e-6
    assert abs(rotation[2] - math.sin(midpoint * math.pi / 4.0)) < 1e-6
    print("PASS VMD Bezier decode and sampler")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
