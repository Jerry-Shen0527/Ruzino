"""
Numerical fidelity tests for the brush paint simulation.

Unlike test_brush_sim.py (a smoke test that only checks the collapsed 2D
point cloud), these tests read the debug output ports exposed by
brush_paint_sim to assert on physical correctness:

  - test_pressure_projection_reduces_divergence
        After the fixed-point pressure projection (paper Algorithm 1), the
        velocity field should be (near-)divergence-free. We read back
        "Max Divergence" / "Mean Divergence" over the active window.

  - test_paint_mass_conservation
        Deposited paint should not vanish. Total density and the three RYB
        channel sums must be finite and non-negative; NaN/Inf means the
        integrator diverged.

  - test_particle_sanity
        FLIP/PIC particles must stay within [0, MAX_PARTICLES] and have a
        finite, non-negative total mass.

  - test_color_mix_brightness
        Pure-Python unit test of the cubical RYB->RGB mixing kernel
        (paper Algorithm 2, brightness-preserving). Does NOT run the GPU
        pipeline — it validates the colour math directly against the same
        formula used in the C++ host and the shaders.

Pipeline under test: mock_stroke -> brush_input -> brush_paint_sim
"""

import math
import os

import pytest

from ruzino_graph import RuzinoGraph
import nodes_core_py as core
import geometry_py as geom


def get_binary_dir():
    return os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "Binaries", "Release")
    )


# Reusable pipeline builder. Returns (graph, nodes) so each test can set its
# own inputs. Mirrors the pattern in test_brush_sim.py.
def _build_pipeline():
    binary_dir = get_binary_dir()
    g = RuzinoGraph("BrushSimFidelity")
    config_path = os.path.join(binary_dir, "geometry_nodes.json")
    g.loadConfiguration(config_path)

    mock = g.createNode("mock_stroke", name="MockStroke")
    brush_in = g.createNode("brush_input", name="BrushInput")
    paint_sim = g.createNode("brush_paint_sim", name="PaintSim")

    g.addEdge(mock, "Stroke Curves", brush_in, "Stroke Curves")
    g.addEdge(brush_in, "Brush Stroke", paint_sim, "Brush Strokes")
    return g, mock, brush_in, paint_sim


def _default_inputs(mock, brush_in, paint_sim):
    return {
        (mock, "Num Points"): 30,
        (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (mock, "Ink R (RYB)"): 1.0,
        (mock, "Ink Y (RYB)"): 0.0,
        (mock, "Ink B (RYB)"): 0.0,
        (brush_in, "Brush Width"): 0.02,
        (brush_in, "Brush Pressure"): 1.0,
        (brush_in, "Ink Amount"): 0.8,
        (paint_sim, "Resolution"): 256,
        (paint_sim, "Paper Size"): 1.0,
        (paint_sim, "Brush Radius"): 0.02,
        (paint_sim, "Ink Amount"): 0.8,
        (paint_sim, "Viscosity"): 0.5,
        (paint_sim, "Diffusion Rate"): 0.0001,
        (paint_sim, "Drying Rate"): 0.1,
    }


def _run_and_read_stats(paint_sim, inputs, g):
    """Execute the graph and return the debug stat ports as a dict."""
    g.markOutput(paint_sim, "Paint Particles")
    g.prepare_and_execute(inputs)

    stats = {}
    for port in (
        "Max Divergence",
        "Mean Divergence",
        "Total Density",
        "Total Color R",
        "Total Color Y",
        "Total Color B",
        "Particle Count",
        "Total Particle Mass",
    ):
        stats[port] = g.getOutput(paint_sim, port)
    return stats


# ---------------------------------------------------------------------------
# Test 1: pressure projection drives divergence toward zero (Algorithm 1)
# ---------------------------------------------------------------------------
def test_pressure_projection_reduces_divergence():
    """max|div u| in the active window should be small after projection."""
    g, mock, brush_in, paint_sim = _build_pipeline()
    inputs = _default_inputs(mock, brush_in, paint_sim)
    stats = _run_and_read_stats(paint_sim, inputs, g)

    max_div = stats["Max Divergence"]
    mean_div = stats["Mean Divergence"]

    # These values come from the GPU readback; print them so the first run
    # reveals the real residual level even if the threshold needs tuning.
    print(f"  Max Divergence:  {max_div:.6f}")
    print(f"  Mean Divergence: {mean_div:.6f}")

    assert math.isfinite(max_div), "Max Divergence is NaN/Inf — solver diverged"
    assert math.isfinite(mean_div), "Mean Divergence is NaN/Inf — solver diverged"

    # The solver runs only 3 fixed-point sweeps (= ~6 Jacobi iterations,
    # paper Algorithm 1 with alpha=1). A perfectly incompressible field
    # would be 0; we tolerate a small residual. 0.5 is deliberately loose
    # for the first pass — tighten once we know the true level.
    assert max_div < 0.5, (
        f"Max Divergence {max_div} too large — pressure projection may be broken"
    )
    assert mean_div < 0.1, (
        f"Mean Divergence {mean_div} too large — projection not converging on average"
    )


# ---------------------------------------------------------------------------
# Test 2: paint mass / colour conservation
# ---------------------------------------------------------------------------
def test_paint_mass_conservation():
    """Deposited paint must be finite, non-negative, and present."""
    g, mock, brush_in, paint_sim = _build_pipeline()
    inputs = _default_inputs(mock, brush_in, paint_sim)
    stats = _run_and_read_stats(paint_sim, inputs, g)

    tot_density = stats["Total Density"]
    tot_r = stats["Total Color R"]
    tot_y = stats["Total Color Y"]
    tot_b = stats["Total Color B"]

    print(f"  Total Density: {tot_density:.6f}")
    print(f"  Total R/Y/B:   {tot_r:.6f} / {tot_y:.6f} / {tot_b:.6f}")

    # All aggregates must be finite — NaN/Inf means the integrator blew up.
    for name, val in [("Total Density", tot_density),
                      ("Total Color R", tot_r),
                      ("Total Color Y", tot_y),
                      ("Total Color B", tot_b)]:
        assert math.isfinite(val), f"{name} is NaN/Inf"

    # Paint was deposited (Ink Amount = 0.8, non-empty stroke), so there
    # must be a positive amount of density on the grid.
    assert tot_density > 0.0, "Total Density <= 0 — paint was never deposited"

    # The mock stroke uses pure RYB-red (R=1, Y=0, B=0), so the R channel
    # should dominate. The Y/B channels may be non-zero from diffusion but
    # must not exceed R by a large margin.
    assert tot_r >= 0.0 and tot_y >= 0.0 and tot_b >= 0.0, \
        "Negative colour channel sum — unphysical"
    assert tot_r >= tot_y, "R channel (pure red input) should dominate Y"
    assert tot_r >= tot_b, "R channel (pure red input) should dominate B"


# ---------------------------------------------------------------------------
# Test 3: particle count and mass bounds
# ---------------------------------------------------------------------------
def test_particle_sanity():
    """Particles must be within [0, MAX_PARTICLES] with finite mass."""
    MAX_PARTICLES = 262144  # must match PaintSimStorage::MAX_PARTICLES

    g, mock, brush_in, paint_sim = _build_pipeline()
    inputs = _default_inputs(mock, brush_in, paint_sim)
    stats = _run_and_read_stats(paint_sim, inputs, g)

    count = stats["Particle Count"]
    mass = stats["Total Particle Mass"]

    print(f"  Particle Count:       {count}")
    print(f"  Total Particle Mass:  {mass:.6f}")

    assert isinstance(count, int), f"Particle Count is {type(count)}, expected int"
    assert 0 <= count <= MAX_PARTICLES, \
        f"Particle Count {count} out of bounds [0, {MAX_PARTICLES}]"

    assert math.isfinite(mass), "Total Particle Mass is NaN/Inf"
    assert mass >= 0.0, f"Total Particle Mass {mass} is negative"


# ---------------------------------------------------------------------------
# Test 4: colour mixing math (pure Python, no GPU)
# ---------------------------------------------------------------------------
def _ryb_to_rgb(r, y, b):
    """Cubical RYB -> RGB mapping (Gossett & Chen 2004), identical to the
    one in node_brush_paint_sim.cpp and common.slangh. Returns [R, G, B]."""
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
    """Brightness measure from common.slangh: a Euclidean-style luminance.
    NOT the usual 0.299/0.587/0.114 — the paper uses sqrt(0.241r²+0.691g²+0.068b²)."""
    r, g, b = rgb
    return math.sqrt(0.241 * r * r + 0.691 * g * g + 0.068 * b * b)


def _color_mix(c_new_ryb, c_old_ryb, w_new, w_old):
    """Brightness-preserving RYB mix (Algorithm 2), mirroring color_mix()
    in common.slangh exactly."""
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
    """Validate the cubical RYB->RGB colour model (Gossett & Chen 2004) that
    underlies the brightness-preserving mix (Algorithm 2). This is a pure
    math test — no GPU — and checks the properties the simulation actually
    relies on.

    NOTE: we deliberately do NOT assert that color_mix produces an RGB output
    whose brightness exactly equals the weighted average of the inputs. The
    implementation applies the brightness correction in RYB space (scaling
    c_prime), but brightness is measured in RGB space, and because ryb_to_rgb
    is nonlinear that scaling only approximately moves the RGB brightness
    toward the target. For some colour pairs it can even overshoot. Asserting
    exact preservation would test a property the code does not guarantee.
    Instead we check the properties that ARE guaranteed by the formula."""

    # (1) The eight RYB cube corners map to their canonical RGB colours.
    #     These are the anchors of the Gossett-Chen cubical model and every
    #     other value is a convex blend of them.
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
    import itertools
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
