"""
Test the Wetbrush paint simulation via the streaming 4-node zone chain.

Pipeline (simulation zone, fed back every frame):

    mock_stroke --Stroke Curves--> [ simulation_in ]   (boundary slot A)
    brush_wb_init_state --State--> [ simulation_in ]   (boundary slot B, seed)
      [ simulation_in ] --Stroke Curves--> mock_point_emitter
      mock_point_emitter --BrushPoint--> brush_wb_deposit
      [ simulation_in ] --State--> brush_wb_deposit
      brush_wb_deposit --State--> brush_wb_bristle --State--> brush_wb_fluid
        --State--> brush_wb_commit
      brush_wb_deposit --BrushPoint--> brush_wb_fluid
      brush_wb_commit --Paint Particles--> write_usd
      brush_wb_commit --State--> [ simulation_out ]   (fed back)

Validates (via stage.tick loop, reading USD stage geometry — the source of
truth after a zone cook, since the executor's node output cache is not
reliable across stage.tick):
  - The zone pipeline cooks every frame without error;
  - Paint particles are generated (canvas non-empty) and sit near the stroke;
  - RYB->RGB color deposit produces non-trivial (non-white) colors;
  - Empty stroke input degrades cleanly (no particles emitted).

Run from Binaries/Release so node-plugin DLLs (brush_wb_*.dll,
mock_point_emitter.dll, ...) resolve.
"""

import math
import os
import sys
from pathlib import Path

import pytest

# Reuse the shared TEST_OUTPUT_DIR from the top-level source/tests/conftest.py
# so all test output converges under Binaries/Release/test_output/.
_TESTS_ROOT = Path(__file__).resolve().parents[3] / "tests"
if str(_TESTS_ROOT) not in sys.path:
    sys.path.insert(0, str(_TESTS_ROOT))
from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "brush_sim"

NUM_FRAMES = 12
FPS = 60.0
DT = 1.0 / FPS


def _build_zone_graph(num_points=30):
    """Build the streaming Wetbrush zone graph and return
    (graph, sim_in, sim_out, mock, emitter, commit).

    Identical topology to test_wetbrush_zone._build_streaming_graph; kept
    inline here so this file is self-contained for the smoke tests.
    """
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("BrushSimTest")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    mock = g.createNode("mock_stroke", name="MockStroke")
    init_state = g.createNode("brush_wb_init_state", name="InitState")
    sim_in, sim_out = g.createSimulationZone()
    emitter = g.createNode("mock_point_emitter", name="Emitter")
    deposit = g.createNode("brush_wb_deposit", name="Deposit")
    bristle = g.createNode("brush_wb_bristle", name="Bristle")
    fluid = g.createNode("brush_wb_fluid", name="Fluid")
    commit = g.createNode("brush_wb_commit", name="Commit")
    write = g.createNode("write_usd", name="Output")

    g.addEdge(mock, "Stroke Curves", sim_in, "Simulation In")
    g.addEdge(init_state, "State", sim_in, "Simulation In")
    g.addEdge(sim_in, "Simulation Out", emitter, "Stroke Curves")
    g.addEdge(emitter, "Current Point", deposit, "Brush Point")
    g.addEdge(sim_in, "Simulation Out", deposit, "State")
    g.addEdge(deposit, "Brush Point", fluid, "Brush Point")
    g.addEdge(deposit, "State", bristle, "State")
    g.addEdge(bristle, "State", fluid, "State")
    g.addEdge(fluid, "State", commit, "State")
    g.addEdge(sim_in, "Simulation Out", commit, "Stroke Curves")
    g.addEdge(commit, "Paint Particles", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): num_points,
        (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (deposit, "Resolution"): 256,
        (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02,
        (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5,
        (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1,
        (fluid, "Brush Radius"): 0.02,
    })

    assert sim_in.paired_node is sim_out, "zone pairing not established"
    return g, sim_in, sim_out, mock, emitter, commit


def _run_zone_and_read_points(g, out_usd_name):
    """Drive NUM_FRAMES ticks and return (points, display_colors) from the
    final authored frame on the stage."""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    from pxr import UsdGeom, Sdf

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_usd = str(OUTPUT_DIR / out_usd_name)
    if os.path.exists(out_usd):
        os.remove(out_usd)

    stage = stage_py.Stage(out_usd)
    prim_path = "/Brush"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)
    g.apply_to_stage(stage, prim_path)

    # GATE 1: prim must carry Animatable=true or Stage.tick never cooks.
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    # GATE 2: render_time must stay >= accumulated sim time each tick.
    for i in range(NUM_FRAMES):
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()

    # Read the final authored time sample off the stage — the source of
    # truth after a zone cook (node output cache is unreliable across tick).
    pxr_stage = stage.get_pxr_stage()
    brush_prim = pxr_stage.GetPrimAtPath(Sdf.Path(prim_path))
    points_attr = brush_prim.GetAttribute("points")
    times = points_attr.GetTimeSamples() if points_attr else []
    points = points_attr.Get(max(times)) if times else None

    color_attr = brush_prim.GetAttribute("primvars:displayColor")
    colors = None
    if color_attr:
        ctimes = color_attr.GetTimeSamples()
        if ctimes:
            colors = color_attr.Get(max(ctimes))

    stage.save()
    return points, colors


def test_brush_sim_generates_particles():
    """Full zone pipeline cooks and deposits paint particles."""
    g, *_ = _build_zone_graph(num_points=30)
    points, colors = _run_zone_and_read_points(g, "brush_sim_smoke.usdc")

    n = len(points) if points else 0
    assert n > 0, "No paint particles generated (canvas empty)"

    has_nan = False
    for p in points:
        if not (math.isfinite(p[0]) and math.isfinite(p[1])
                and math.isfinite(p[2])):
            has_nan = True
            break
    assert not has_nan, "painted points contain NaN/Inf — field diverged"

    # Particles should sit near the stroke (centered at origin, ~0.3 long).
    max_dist = max(math.sqrt(p[0]**2 + p[1]**2 + p[2]**2) for p in points)
    assert max_dist < 5.0, (
        f"Max distance {max_dist:.1f} too large — particles may be unbounded")

    print(f"  Generated {n} paint particles, max_dist={max_dist:.4f}")
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    print(f"  Bounding X: [{min(xs):.4f}, {max(xs):.4f}]")
    print(f"  Bounding Y: [{min(ys):.4f}, {max(ys):.4f}]")

    # At least some particles should carry non-trivial color (not pure white),
    # proving color deposit reached the canvas.
    if colors:
        non_white = [c for c in colors
                     if c[0] < 0.95 or c[1] < 0.95 or c[2] < 0.95]
        print(f"  Non-white particles: {len(non_white)}/{n}")
        assert len(non_white) > 0, (
            "All particles are white — color deposit may be broken")


def test_brush_sim_empty_input():
    """When no stroke data is provided (Num Points=0), the canvas should
    stay empty — the emitter emits no BrushPoints, so nothing deposits."""
    g, *_ = _build_zone_graph(num_points=0)
    points, _ = _run_zone_and_read_points(g, "brush_sim_empty.usdc")

    n = len(points) if points else 0
    assert n == 0, (
        f"Expected 0 particles for empty input, got {n}")
    print(f"  Empty input: {n} particles (expected 0)")
