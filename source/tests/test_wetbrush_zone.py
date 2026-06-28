"""
Headless simulation-zone test for the STREAMING Wetbrush decomposition.

Builds the streaming brush pipeline purely from the Python node-graph API:

    mock_stroke --Stroke Curves--> mock_point_emitter --BrushPoint-->
        [ simulation_in ]
        [ simulation_in ] --SimState--> brush_wb_deposit
        brush_wb_deposit --SimState+Height--> brush_wb_bristle
        brush_wb_bristle --SimState+BristleSamples--> brush_wb_fluid
        brush_wb_fluid --SimState--> brush_wb_commit
        brush_wb_commit --Paint Particles--> [ simulation_out ]
        [ simulation_out ] --Paint Particles--> write_usd

Then drives N ticks of stage.tick(dt). The four brush_wb_* nodes are
ALWAYS_DIRTY and consume delta_time / is_simulating from the global
GeomPayload, so they only advance under stage.tick (NOT under a manual
prepare_and_execute loop). The simulation zone's paired_node feedback carries
the per-frame SimState forward.

This is the SKELETON connectivity test (Stage 2 of the Wetbrush decomposition
plan): it validates that the multi-node chain + simulation zone + stage.tick
execute end-to-end without error, and that the per-frame BrushPoint flows
mock_point_emitter -> simulation_in -> ... -> brush_wb_commit. The physics in
each brush_wb_* node is still a TODO stub (emit_empty), so Paint Particles is
empty and the debug ports read 0 -- that is expected here. Stage 3 lifts the
physics and adds numerical assertions.

Environment is set up by source/tests/conftest.py. Run from Binaries/Release
so node-plugin DLLs (brush_wb_*.dll, mock_point_emitter.dll, ...) resolve.
"""

import os
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = DATA_DIR / "output"

NUM_FRAMES = 10
FPS = 60.0
DT = 1.0 / FPS


def _build_streaming_graph():
    """Build the streaming Wetbrush zone graph.

    The simulation zone carries ONE typed value across the boundary:
    WetbrushFrame (bundles the per-frame BrushPoint + the shared
    WetbrushSimState). Carrying two different-typed slots (BrushPoint +
    shared_ptr<WetbrushSimState>) broke the zone's slot matching and the wb
    nodes never cooked (MISSING_INPUT); bundling into one aggregate fixes it.

    Topology:
      mock_stroke --Stroke Curves--> [ simulation_in ]
      [ simulation_in ] --Stroke Curves--> mock_point_emitter   (zone interior;
          the curve is forwarded into the zone on the init frame and fed back
          on advance frames -- emitter caches it internally so re-reading the
          same curve is fine)
      mock_point_emitter --BrushPoint--> brush_wb_deposit "Brush Point"
      brush_wb_deposit --Frame--> brush_wb_bristle --Frame--> brush_wb_fluid
        --Frame--> brush_wb_commit
      brush_wb_commit --Frame--> [ simulation_out ]   (fed back as WetbrushFrame)
      [ simulation_out ] --Frame--> write_usd

    Returns (graph, sim_in, sim_out, mock, emitter, commit).
    """
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("WetbrushZoneSim")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    mock = g.createNode("mock_stroke", name="MockStroke")
    entry = g.createNode("brush_wb_entry", name="Entry")  # Geometry -> WetbrushFrame
    sim_in, sim_out = g.createSimulationZone()
    emitter = g.createNode("mock_point_emitter", name="Emitter")
    deposit = g.createNode("brush_wb_deposit", name="Deposit")
    bristle = g.createNode("brush_wb_bristle", name="Bristle")
    fluid = g.createNode("brush_wb_fluid", name="Fluid")
    commit = g.createNode("brush_wb_commit", name="Commit")
    # write_usd lives INSIDE the zone (after commit) so Paint Particles flows
    # commit -> write_usd directly, NOT through the boundary. This keeps the
    # zone boundary single-typed: only WetbrushFrame is fed back.
    write = g.createNode("write_usd", name="Output")

    # mock_stroke -> entry: pack Stroke Curves into a WetbrushFrame so the zone
    # boundary stays single-typed (input == feedback type).
    g.addEdge(mock, "Stroke Curves", entry, "Stroke Curves")
    # entry -> simulation_in: WetbrushFrame enters the zone (and is fed back on
    # advance frames carrying the canvas + state).
    g.addEdge(entry, "Frame", sim_in, "Simulation In")
    # sim_in's "Simulation Out" carries the WetbrushFrame to the emitter
    # (interior). The emitter reads stroke_curves from it and advances its
    # cursor every frame via payload.delta_time.
    g.addEdge(sim_in, "Simulation Out", emitter, "Frame")
    # emitter -> deposit: the fresh per-frame BrushPoint (interior edge).
    g.addEdge(emitter, "Current Point", deposit, "Brush Point")
    # sim_in -> deposit: the fed-back WetbrushFrame (init: entry's frame with
    # stroke_curves; advance: commit's frame with canvas+state). The same
    # boundary slot feeds both the emitter (reads stroke_curves) and deposit
    # (reads/allocates state).
    g.addEdge(sim_in, "Simulation Out", deposit, "Frame")
    # The wb chain: WetbrushFrame flows deposit -> bristle -> fluid -> commit.
    g.addEdge(deposit, "Frame", bristle, "Frame")
    g.addEdge(deposit, "Height Field", bristle, "Height Field")
    g.addEdge(bristle, "Frame", fluid, "Frame")
    g.addEdge(bristle, "Bristle Samples", fluid, "Bristle Samples")
    g.addEdge(fluid, "Frame", commit, "Frame")
    # commit -> write_usd (interior): Paint Particles reaches write_usd without
    # crossing the boundary, so the zone feedback stays single-typed.
    g.addEdge(commit, "Paint Particles", write, "Geometry")
    # commit -> simulation_out: ONLY the WetbrushFrame is fed back (carries the
    # persistent canvas + live fields). Single typed boundary slot.
    g.addEdge(commit, "Frame", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): 30,
        (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (deposit, "Resolution"): 256,
        (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02,
        (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Viscosity"): 0.5,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5,
        (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1,
        (fluid, "Brush Radius"): 0.02,
    })

    assert sim_in.paired_node is sim_out, "zone pairing not established"
    return g, sim_in, sim_out, mock, emitter, commit


def test_streaming_graph_builds():
    """The streaming graph and zone invariants build correctly from Python."""
    g, sim_in, sim_out, mock, emitter, commit = _build_streaming_graph()

    labels = [n.name for n in g.nodes]
    for needed in ("MockStroke", "SimulationIn", "Emitter", "Deposit",
                   "Bristle", "Fluid", "Commit", "SimulationOut", "Output"):
        assert needed in labels, f"missing node {needed}: {labels}"

    assert sim_in.paired_node is sim_out
    assert sim_out.paired_node is sim_in
    assert len(g.links) >= 10, f"expected >=10 links, got {len(g.links)}"


def test_streaming_simulation_runs():
    """N ticks of stage.tick drive the streaming chain without error.

    Skeleton stage: physics is stubbed, so we only assert that the cook
    completes every frame and the graph doesn't throw. Stage 3 will add
    assertions on the debug ports once physics is lifted.
    """
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    from pxr import UsdGeom, Sdf

    g, sim_in, sim_out, mock, emitter, commit = _build_streaming_graph()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_usd = str(OUTPUT_DIR / "wetbrush_zone_sim.usdc")
    if os.path.exists(out_usd):
        os.remove(out_usd)

    stage = stage_py.Stage(out_usd)
    prim_path = "/Brush"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)

    print("MARKER: before apply_to_stage", flush=True)
    # Serialize the graph ourselves to see if THAT is where it breaks.
    try:
        j = g.serialize()
        print(f"MARKER: serialize OK, len={len(j)}", flush=True)
    except Exception as e:
        print(f"MARKER: serialize FAILED: {e}", flush=True)
        raise
    g.apply_to_stage(stage, prim_path)
    print("MARKER: after apply_to_stage", flush=True)

    # GATE 1: prim must carry Animatable=true or Stage.tick never cooks it.
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    # GATE 2: render_time must stay >= accumulated sim time, else
    # should_simulate() short-circuits after frame 1.
    for i in range(NUM_FRAMES):
        print(f"MARKER: before tick {i}", flush=True)
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()
        print(f"MARKER: after tick {i}", flush=True)

    stage.save()
    print(f"  streaming wetbrush: {NUM_FRAMES} frames cooked, "
          f"prim={prim_path}")
