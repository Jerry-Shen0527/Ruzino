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
    """Build mock_stroke -> emitter -> zone(deposit->bristle->fluid->commit).

    The simulation zone carries TWO values across the boundary frame-to-frame:
      * BrushPoint  -- the per-frame brush sample (emitter -> sim_in; fresh
                       each frame, not fed back)
      * SimState    -- the shared WetbrushSimState (fed back sim_out -> sim_in;
                       deposit allocates it on the init frame, the wb chain
                       mutates it, commit writes it out for next-frame feedback)

    Returns (graph, sim_in, sim_out, mock, emitter, commit) for assertions.
    """
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("WetbrushZoneSim")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    mock = g.createNode("mock_stroke", name="MockStroke")
    emitter = g.createNode("mock_point_emitter", name="Emitter")
    sim_in, sim_out = g.createSimulationZone()
    deposit = g.createNode("brush_wb_deposit", name="Deposit")
    bristle = g.createNode("brush_wb_bristle", name="Bristle")
    fluid = g.createNode("brush_wb_fluid", name="Fluid")
    commit = g.createNode("brush_wb_commit", name="Commit")
    write = g.createNode("write_usd", name="Output")

    # mock_stroke -> emitter (programmatic stroke trajectory).
    g.addEdge(mock, "Stroke Curves", emitter, "Stroke Curves")
    # emitter -> simulation_in: BrushPoint enters the zone each frame. The zone
    # carries it on "Simulation Out" to the wb chain.
    g.addEdge(emitter, "Current Point", sim_in, "Simulation In")

    # Inside the zone, sim_in's "Simulation Out" provides BOTH the BrushPoint
    # and the fed-back SimState (the zone forwards every materialized slot).
    # Connect both to deposit's "Brush Point" and "SimState" inputs. On the
    # init frame the SimState slot is absent/empty, so deposit allocates it.
    g.addEdge(sim_in, "Simulation Out", deposit, "Brush Point")
    g.addEdge(sim_in, "Simulation Out", deposit, "SimState")
    # The wb chain: each forwards SimState + the 2-node socket fields.
    g.addEdge(deposit, "SimState", bristle, "SimState")
    g.addEdge(deposit, "Height Field", bristle, "Height Field")
    g.addEdge(bristle, "SimState", fluid, "SimState")
    g.addEdge(bristle, "Bristle Samples", fluid, "Bristle Samples")
    g.addEdge(fluid, "SimState", commit, "SimState")
    # BrushPoint also reaches bristle/fluid/commit (they read it for logging /
    # kinematics in the stub; Stage 3 uses it for physics).
    g.addEdge(sim_in, "Simulation Out", bristle, "Brush Point")
    g.addEdge(sim_in, "Simulation Out", fluid, "Brush Point")
    g.addEdge(sim_in, "Simulation Out", commit, "Brush Point")

    # FEEDBACK: commit's SimState -> simulation_out "Simulation In", so the
    # zone feeds the (canvas + live fields + control) SimState back to sim_in
    # for the next frame. This is what makes the persistent canvas survive.
    g.addEdge(commit, "SimState", sim_out, "Simulation In")
    # Paint Particles leaves the zone (one-shot output each frame, not fed back).
    g.addEdge(commit, "Paint Particles", sim_out, "Simulation In")
    # simulation_out -> write_usd.
    g.addEdge(sim_out, "Simulation Out", write, "Geometry")

    # Brush/material params via socket defaults.
    g.setSocketDefaults({
        (mock, "Num Points"): 30,
        (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (deposit, "Resolution"): 256,
        (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02,
        (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        # NOTE: "Ink Color" (vec3) has no default_val support (see node.hpp
        # ValueTrait); deposit/bristle apply a red fallback when it is unset.
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
    for needed in ("MockStroke", "Emitter", "SimulationIn", "Deposit",
                   "Bristle", "Fluid", "Commit", "SimulationOut", "Output"):
        assert needed in labels, f"missing node {needed}: {labels}"

    assert sim_in.paired_node is sim_out
    assert sim_out.paired_node is sim_in
    # The wb chain + dual boundary slots + feedback + output are all wired.
    assert len(g.links) >= 14, f"expected >=14 links, got {len(g.links)}"


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

    g.apply_to_stage(stage, prim_path)

    # GATE 1: prim must carry Animatable=true or Stage.tick never cooks it.
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    # GATE 2: render_time must stay >= accumulated sim time, else
    # should_simulate() short-circuits after frame 1.
    for i in range(NUM_FRAMES):
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()

    stage.save()
    print(f"  streaming wetbrush: {NUM_FRAMES} frames cooked, "
          f"prim={prim_path}")
