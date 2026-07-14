"""
Pure-Python unit tests for the Wetbrush colour mixing model.

These validate the cubical RYB->RGB mapping (Gossett & Chen 2004) that
underlies the brightness-preserving mix (paper Algorithm 2). No GPU, no
nodes, no simulation — this checks the colour math directly against the
same formula used in the C++ host and the shaders. Extracted from the old
test_brush_sim_fidelity.py so it survives independent of the simulation
pipeline.
"""

import math
import itertools

import pytest


def _ryb_to_rgb(r, y, b):
    """Cubical RYB -> RGB mapping (Gossett & Chen 2004), identical to the
    one in the brush shaders. Returns [R, G, B]."""
    rm, ym, bm = 1.0 - r, 1.0 - y, 1.0 - b
    out = [0.0, 0.0, 0.0]
    for w, c in [
        (rm * ym * bm, (1.0, 1.0, 1.0)),
        (r * ym * bm, (1.0, 0.0, 0.0)),
        (rm * y * bm, (1.0, 1.0, 0.0)),
        (rm * ym * b, (0.163, 0.373, 0.6)),
        (r * y * bm, (1.0, 0.5, 0.0)),
        (r * ym * b, (0.5, 0.0, 0.5)),
        (rm * y * b, (0.0, 0.66, 0.2)),
        (r * y * b, (0.2, 0.094, 0.029)),
    ]:
        for i in range(3):
            out[i] += w * c[i]
    return out


def _color_brightness(rgb):
    """Brightness measure from the shaders: a Euclidean-style luminance.
    NOT the usual 0.299/0.587/0.114 — the paper uses
    sqrt(0.241r^2 + 0.691g^2 + 0.068b^2)."""
    r, g, b = rgb
    return math.sqrt(0.241 * r * r + 0.691 * g * g + 0.068 * b * b)


def _color_mix(c_new_ryb, c_old_ryb, w_new, w_old):
    """Brightness-preserving RYB mix (Algorithm 2), mirroring color_mix()
    in the shaders exactly."""
    b_new = _color_brightness(_ryb_to_rgb(*c_new_ryb))
    b_old = _color_brightness(_ryb_to_rgb(*c_old_ryb))

    w_total = w_new + w_old
    c_prime = [(w_new * c_new_ryb[i] + w_old * c_old_ryb[i]) / w_total
               for i in range(3)]
    b_prime = _color_brightness(_ryb_to_rgb(*c_prime))
    b_dprime = (w_new * b_new + w_old * b_old) / w_total

    if b_prime > 1e-6:
        return [b_dprime * c / b_prime for c in c_prime]
    return c_prime


def test_color_mix_brightness():
    """Validate the cubical RYB->RGB colour model and the
    brightness-preserving mix.

    NOTE: we deliberately do NOT assert that color_mix produces an RGB output
    whose brightness exactly equals the weighted average of the inputs. The
    implementation applies the brightness correction in RYB space (scaling
    c_prime), but brightness is measured in RGB space, and because ryb_to_rgb
    is nonlinear that scaling only approximately moves the RGB brightness
    toward the target. For some colour pairs it can even overshoot. Asserting
    exact preservation would test a property the code does not guarantee.
    Instead we check the properties that ARE guaranteed by the formula."""

    # (1) The eight RYB cube corners map to their canonical RGB colours.
    corner_cases = [
        ((0.0, 0.0, 0.0), (1.0, 1.0, 1.0)),     # RYB white  -> RGB white
        ((1.0, 0.0, 0.0), (1.0, 0.0, 0.0)),     # RYB red    -> RGB red
        ((0.0, 1.0, 0.0), (1.0, 1.0, 0.0)),     # RYB yellow -> RGB yellow
        ((0.0, 0.0, 1.0), (0.163, 0.373, 0.6)), # RYB blue   -> RGB blue
        ((1.0, 1.0, 0.0), (1.0, 0.5, 0.0)),     # RYB orange -> RGB orange
        ((1.0, 0.0, 1.0), (0.5, 0.0, 0.5)),     # RYB purple -> RGB purple
        ((0.0, 1.0, 1.0), (0.0, 0.66, 0.2)),    # RYB green  -> RGB green
        ((1.0, 1.0, 1.0), (0.2, 0.094, 0.029)), # RYB black  -> RGB black
    ]
    for ryb, expected_rgb in corner_cases:
        got = _ryb_to_rgb(*ryb)
        for i in range(3):
            assert abs(got[i] - expected_rgb[i]) < 1e-6, \
                f"corner {ryb} channel {i}: got {got[i]}, expected {expected_rgb[i]}"

    # (2) The corner weights sum to 1 for any input in [0,1]^3, so the output
    #     is a convex combination of the 8 corner RGBs and stays in their
    #     bounding box (no NaN, no negative, channels bounded by max corner).
    for r in (0.0, 0.25, 0.5, 0.75, 1.0):
        for y in (0.0, 0.25, 0.5, 0.75, 1.0):
            for b in (0.0, 0.25, 0.5, 0.75, 1.0):
                rgb = _ryb_to_rgb(r, y, b)
                for ch in rgb:
                    assert math.isfinite(ch), f"non-finite at ({r},{y},{b})"
                    assert -1e-6 <= ch <= 1.0 + 1e-6, \
                        f"channel {ch} out of [0,1] at ({r},{y},{b})"

    # (3) color_mix with a zero-weight side returns the other side's colour
    #     (degenerate but well-defined boundary case the sim can hit when a
    #     cell has no existing paint).
    pure = (0.7, 0.3, 0.1)
    res = _color_mix(pure, (0.0, 0.0, 0.0), 1.0, 0.0)
    for i in range(3):
        assert abs(res[i] - pure[i]) < 1e-6, \
            f"color_mix(.,.,1,0) should be identity, got {res} for {pure}"

    # (4) color_mix of two identical colours returns that colour (idempotent
    #     on equal inputs — the brightness scaling is a no-op then).
    for c in [(1.0, 0.0, 0.0), (0.5, 0.5, 0.5), (0.2, 0.094, 0.029)]:
        res = _color_mix(c, c, 0.4, 0.6)
        for i in range(3):
            assert abs(res[i] - c[i]) < 1e-4, \
                f"color_mix of identical {c} drifted to {res}"
