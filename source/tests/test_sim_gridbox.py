"""
Headless simulation end-to-end test, built purely from the Python node-graph
API.

Pipeline under test:

    create_box_grid --Geometry--> [ simulation_in ]
    [ simulation_in ] --Geometry--> transform_geom (Translate X = 0.1 / frame)
    transform_geom --Geometry--> [ simulation_out ]
    [ simulation_out ] --Geometry--> write_usd

The whole thing is embedded on a single USD prim as a modifier graph
(``node_json`` + ``Animatable``), then driven 60 ticks by ``Stage.tick(dt)``.
Per-frame accumulation is provided by the simulation-zone feedback loop:
after each cook the eager executor moves ``simulation_out``'s storage into
``simulation_in``'s (node_exec_eager.cpp), so frame N's transform applies on
top of frame N-1's geometry. Expected final X offset = 60 * 0.1 = 6.0.

This exercises, end to end:
  * RuzinoGraph.createSimulationZone() (Python-built zone + paired_node loop)
  * the three gates that headless simulation must open:
      - stage_py.tick / set_render_time bindings
      - prim "Animatable" attribute
      - render_time kept >= accumulated time (should_simulate)
  * final geometry read back from the composed stage.

Environment is set up by source/tests/conftest.py.
"""

import os
from pathlib import Path

import numpy as np
import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "sim_gridbox"

NUM_FRAMES = 60
FPS = 60.0
DT = 1.0 / FPS
PER_FRAME_DX = 0.1  # transform_geom Translate X applied every tick


def _build_zone_graph():
    """Build the create_box_grid -> zone -> transform -> write_usd graph.

    Returns the graph and the zone nodes (for invariant assertions).
    """
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("GridBoxSim")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    grid = g.createNode("create_box_grid", name="Grid")
    sim_in, sim_out = g.createSimulationZone()
    transform = g.createNode("transform_geom", name="Transform")
    write = g.createNode("write_usd", name="Output")

    # Connect into the zone by socket-group NAME ("Simulation In" /
    # "Simulation Out"). addEdge resolves each group name to its placeholder,
    # and the link itself auto-instantiates a real Geometry socket (borrowing
    # type/name from the peer end) - same workflow as dragging a link to a
    # dynamic group in the UI. createSimulationZone wired group sync, so the
    # socket propagates to all four zone boundaries.
    g.addEdge(grid, "Geometry", sim_in, "Simulation In")
    g.addEdge(sim_in, "Simulation Out", transform, "Geometry")
    g.addEdge(transform, "Geometry", sim_out, "Simulation In")
    g.addEdge(sim_out, "Simulation Out", write, "Geometry")

    # Per-frame translate; accumulation is handled by the zone feedback loop,
    # not by this value growing each frame.
    g.setSocketDefault(transform, "Translate X", PER_FRAME_DX)

    # A small, deterministic box grid.
    g.setSocketDefaults({
        (grid, "resolution_x"): 2,
        (grid, "resolution_y"): 2,
        (grid, "resolution_z"): 2,
        (grid, "width"): 1.0,
        (grid, "height"): 1.0,
        (grid, "depth"): 1.0,
    })

    assert sim_in.paired_node is sim_out, "zone pairing not established"
    return g, sim_in, sim_out


def test_zone_graph_builds():
    """The graph and its zone invariants build correctly from Python."""
    g, sim_in, sim_out = _build_zone_graph()

    # n.name is the node's ui_name (getName() returns ui_name). Check the
    # labels we assigned at creation are all present.
    labels = [n.name for n in g.nodes]
    for needed in ("Grid", "SimulationIn", "Transform", "SimulationOut", "Output"):
        assert needed in labels, f"missing node {needed}: {labels}"

    assert len(g.links) == 4, f"expected 4 links, got {len(g.links)}"
    assert sim_in.paired_node is sim_out
    assert sim_out.paired_node is sim_in


def test_zone_auto_socket_from_group_name():
    """addEdge by socket-group name must auto-instantiate a real socket.

    Regression for the unification: connecting to "Simulation In" /
    "Simulation Out" (the group identifiers) instead of pre-seeding a
    "Geometry" socket with group_add_socket must produce at least one real,
    non-placeholder Geometry socket on every zone boundary. (The zone's
    group sync propagates each created socket across all four boundaries, so
    multiple Geometry sockets may appear - that is expected and correct.)
    """
    g, sim_in, sim_out = _build_zone_graph()

    def has_real_geo_socket(node, is_input):
        socks = node.inputs if is_input else node.outputs
        # A materialized socket has a non-empty ui_name ("Geometry") and a
        # non-empty identifier; a placeholder's ui_name is "".
        real_geo = [
            s for s in socks
            if s.ui_name == "Geometry" and s.identifier
        ]
        assert real_geo, (
            f"{node.name} has no materialized Geometry socket, "
            f"got {[(s.ui_name, s.identifier) for s in socks]}")
        return real_geo[0]

    # All four zone boundaries must carry at least one materialized Geometry
    # socket created by addEdge auto-instantiation (+ group sync).
    has_real_geo_socket(sim_in, is_input=True)    # grid -> sim_in
    has_real_geo_socket(sim_in, is_input=False)   # sim_in -> transform
    has_real_geo_socket(sim_out, is_input=True)   # transform -> sim_out
    has_real_geo_socket(sim_out, is_input=False)  # sim_out -> write


def test_gridbox_simulation_accumulates():
    """60 ticks of +0.1 should move the box ~6.0 along X."""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    from pxr import UsdGeom, Sdf

    g, sim_in, sim_out = _build_zone_graph()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_usd = str(OUTPUT_DIR / "gridbox_sim.usdc")
    if os.path.exists(out_usd):
        os.remove(out_usd)

    stage = stage_py.Stage(out_usd)
    prim_path = "/Grid"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)

    # 1. Serialize the graph onto the prim's node_json attribute.
    #    apply_to_stage writes node_json + sets up the GeomPayload.
    g.apply_to_stage(stage, prim_path)

    # 2. GATE: prim must carry Animatable=true or Stage.tick never cooks it
    #    (animation.cpp is_animatable). write_usd normally sets this on first
    #    cook, but the very first tick needs it already present.
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    # 3. Drive 60 ticks. GATE: render_time must stay >= accumulated
    #    simulation time, else WithDynamicLogicPrim::should_simulate() returns
    #    false and execute() is skipped after frame 1.
    for i in range(NUM_FRAMES):
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()

    stage.save()

    # 4. Read back the final geometry from the COMPOSED stage (write_usd writes
    #    each frame's result as an over-spec on the modifier layer; the
    #    composed value is the accumulated geometry).
    points = prim.GetAttribute("points").Get()
    assert points is not None and len(points) > 0, "no points written to stage"

    verts = np.asarray([[p[0], p[1], p[2]] for p in points])
    x_mean = float(np.mean(verts[:, 0]))
    expected = NUM_FRAMES * PER_FRAME_DX  # 6.0

    print(f"  {NUM_FRAMES} frames, {len(verts)} verts, "
          f"final x_mean={x_mean:.4f} (expected ~{expected})")

    assert abs(x_mean - expected) < 0.5, (
        f"expected x_mean ~ {expected} after {NUM_FRAMES} frames, got {x_mean:.4f}")
