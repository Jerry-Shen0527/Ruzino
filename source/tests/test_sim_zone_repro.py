"""
Minimal reproducer for the Wetbrush zone tick-1 crash.

Strategy: take the KNOWN-WORKING test_sim_gridbox topology (single Geometry
slot, write_usd OUTSIDE the zone drives the REQUIRED chain) and mutate one
variable at a time to bisect what makes the wb graph crash on tick 1.

Matrix of variants (each is an independent test):
  A. baseline      = gridbox -> sim_in -> transform -> sim_out -> write_usd(OUT)
                     [KNOWN WORKING -- mirrors test_sim_gridbox]
  B. write_inside  = gridbox -> sim_in -> transform -> write_usd -> sim_out(OUT)
                     [only change: write_usd lives INSIDE the zone]
  C. chain3_inside = gridbox -> sim_in -> t1 -> t2 -> t3 -> sim_out(OUT)
                     [3-node chain inside, no write_usd]
  D. chain4_inside = gridbox -> sim_in -> t1 -> t2 -> t3 -> t4 -> sim_out(OUT)
                     [4-node chain, exactly the wb chain length, no write_usd]

All carry a single Geometry slot (NOT WetbrushFrame), so this isolates the
TOPOLOGY question from the TYPE question. If any of B/C/D crash where A does
not, the bug is in zone topology / REQUIRED propagation, not in WetbrushFrame.

Run:  pytest source/tests/test_sim_zone_repro.py -s   (from Binaries/Release)
"""

import os
from pathlib import Path

import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "sim_zone_repro"

NUM_FRAMES = 5  # crash is at tick 1; 5 frames is plenty to surface it
FPS = 60.0
DT = 1.0 / FPS
PER_FRAME_DX = 0.1


def _common_setup(variant_name):
    """Build a fresh stage for one variant. Returns (stage, prim_path, out_usd)."""
    import stage_py
    from pxr import UsdGeom

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_usd = str(OUTPUT_DIR / f"zone_repro_{variant_name}.usdc")
    if os.path.exists(out_usd):
        os.remove(out_usd)

    stage = stage_py.Stage(out_usd)
    prim_path = f"/{variant_name}"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)

    return stage, prim_path, out_usd


def _drive(stage, prim_path, label):
    """Set the Animatable gate and drive NUM_FRAMES ticks with MARKER logs.

    Returns the number of ticks completed before failure (or NUM_FRAMES).
    """
    from pxr import Sdf

    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    completed = 0
    for i in range(NUM_FRAMES):
        print(f"[{label}] MARKER: before tick {i}", flush=True)
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()
        print(f"[{label}] MARKER: after tick {i}", flush=True)
        completed = i + 1
    return completed


# ---------------------------------------------------------------------------
# Variant A: baseline (KNOWN WORKING) -- write_usd OUTSIDE the zone
# ---------------------------------------------------------------------------

def _build_A():
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("ZoneReproA")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    grid = g.createNode("create_box_grid", name="Grid")
    sim_in, sim_out = g.createSimulationZone()
    transform = g.createNode("transform_geom", name="Transform")
    write = g.createNode("write_usd", name="Output")  # OUTSIDE zone

    g.addEdge(grid, "Geometry", sim_in, "Simulation In")
    g.addEdge(sim_in, "Simulation Out", transform, "Geometry")
    g.addEdge(transform, "Geometry", sim_out, "Simulation In")
    g.addEdge(sim_out, "Simulation Out", write, "Geometry")  # crosses boundary

    g.setSocketDefault(transform, "Translate X", PER_FRAME_DX)
    g.setSocketDefaults({
        (grid, "resolution_x"): 2, (grid, "resolution_y"): 2,
        (grid, "resolution_z"): 2, (grid, "width"): 1.0,
        (grid, "height"): 1.0, (grid, "depth"): 1.0,
    })
    return g


def test_A_baseline_write_outside():
    """KNOWN-WORKING: write_usd outside the zone. Should complete all frames."""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    stage, prim_path, _ = _common_setup("ZoneReproA")
    g = _build_A()
    g.apply_to_stage(stage, prim_path)
    n = _drive(stage, prim_path, "A")
    assert n == NUM_FRAMES, f"A (baseline) failed at tick {n}, expected {NUM_FRAMES}"


# ---------------------------------------------------------------------------
# Variant B: write_usd INSIDE the zone (the wb-graph's structural choice)
# ---------------------------------------------------------------------------

def _build_B():
    """Matches the wb graph's structure: transform feeds BOTH an interior
    write_usd sink AND the sim_out feedback. write_usd is a sink (no output),
    so it does NOT sit between transform and sim_out -- it branches off.

        grid -> sim_in -> transform --+-> write_usd (sink, interior)
                                      +-> sim_out (feedback)
        sim_out -> (nothing external)
    """
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("ZoneReproB")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    grid = g.createNode("create_box_grid", name="Grid")
    sim_in, sim_out = g.createSimulationZone()
    transform = g.createNode("transform_geom", name="Transform")
    write = g.createNode("write_usd", name="Output")  # interior sink

    g.addEdge(grid, "Geometry", sim_in, "Simulation In")
    g.addEdge(sim_in, "Simulation Out", transform, "Geometry")
    # transform branches: one edge to the interior write_usd sink...
    g.addEdge(transform, "Geometry", write, "Geometry")
    # ...and one edge to sim_out for feedback.
    g.addEdge(transform, "Geometry", sim_out, "Simulation In")
    # sim_out has nothing downstream (no external write_usd) -- does REQUIRED
    # still propagate to the interior sink?

    g.setSocketDefault(transform, "Translate X", PER_FRAME_DX)
    g.setSocketDefaults({
        (grid, "resolution_x"): 2, (grid, "resolution_y"): 2,
        (grid, "resolution_z"): 2, (grid, "width"): 1.0,
        (grid, "height"): 1.0, (grid, "depth"): 1.0,
    })
    return g


def test_B_write_inside():
    """write_usd INSIDE the zone. Does the zone still cook / not crash?"""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    stage, prim_path, _ = _common_setup("ZoneReproB")
    g = _build_B()
    g.apply_to_stage(stage, prim_path)
    n = _drive(stage, prim_path, "B")
    # We don't assert success yet -- the GOAL is to see if it crashes.
    # If it completes, the bug is NOT "write_usd inside the zone".
    print(f"[B] write_usd-inside completed {n}/{NUM_FRAMES} frames")


# ---------------------------------------------------------------------------
# Variants C/D: multi-node transform chain INSIDE the zone, no write_usd
# ---------------------------------------------------------------------------

def _build_chain(variant, n_transforms):
    """Build grid -> sim_in -> [t1..tN] -> sim_out, no write_usd at all.

    Each transform adds a fraction of +0.1/frame to X so total accumulation
    still = 0.1/frame. Tests whether a multi-node INTERIOR chain alone
    (without write_usd) survives the zone feedback.
    """
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph(f"ZoneRepro{variant}")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    grid = g.createNode("create_box_grid", name="Grid")
    sim_in, sim_out = g.createSimulationZone()

    transforms = [
        g.createNode("transform_geom", name=f"T{k}") for k in range(n_transforms)
    ]

    g.addEdge(grid, "Geometry", sim_in, "Simulation In")
    g.addEdge(sim_in, "Simulation Out", transforms[0], "Geometry")
    for k in range(n_transforms - 1):
        g.addEdge(transforms[k], "Geometry", transforms[k + 1], "Geometry")
    g.addEdge(transforms[-1], "Geometry", sim_out, "Simulation In")
    # No downstream consumer of sim_out at all. Most extreme "nothing outside
    # the zone" case.

    for t in transforms:
        g.setSocketDefault(t, "Translate X", PER_FRAME_DX / n_transforms)

    g.setSocketDefaults({
        (grid, "resolution_x"): 2, (grid, "resolution_y"): 2,
        (grid, "resolution_z"): 2, (grid, "width"): 1.0,
        (grid, "height"): 1.0, (grid, "depth"): 1.0,
    })
    return g


def test_C_chain3_inside():
    """3-node interior chain, no write_usd. Crash or survive?"""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    stage, prim_path, _ = _common_setup("ZoneReproC")
    g = _build_chain("C", 3)
    g.apply_to_stage(stage, prim_path)
    n = _drive(stage, prim_path, "C")
    print(f"[C] chain3-inside completed {n}/{NUM_FRAMES} frames")


def test_D_chain4_inside():
    """4-node interior chain (same length as the wb chain). Crash or survive?"""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    stage, prim_path, _ = _common_setup("ZoneReproD")
    g = _build_chain("D", 4)
    g.apply_to_stage(stage, prim_path)
    n = _drive(stage, prim_path, "D")
    print(f"[D] chain4-inside completed {n}/{NUM_FRAMES} frames")
