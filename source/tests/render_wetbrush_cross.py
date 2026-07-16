#!/usr/bin/env python3
"""
Render TWO crossing Wetbrush strokes (red horizontal, then blue vertical)
to verify wet-in-wet color mixing.

Reuses render_wetbrush.py's bake + render stages; only the sim graph differs:
it uses the `mock_strokes` node (two curve segments with delayed absolute
timestamps) instead of the single `mock_stroke`.

Stroke 0 (red,  RYB 1,0,0): horizontal along X, frames  0..~30
Stroke 1 (blue, RYB 0,0,1): vertical along Y, frames ~30..60, crossing stroke 0

Run from Binaries/Release:

    python ../../source/tests/render_wetbrush_cross.py
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
sys.path.insert(0, str(HERE))  # so we can import render_wetbrush

os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")

import numpy as np
from pxr import Usd, UsdGeom, Sdf, Gf, Vt

import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

# Reuse the proven bake + render stages from render_wetbrush.
import render_wetbrush as rw

NUM_FRAMES = 60
FPS = 60.0
DT = 1.0 / FPS

OUTPUT_DIR = BIN / "wetbrush_cross_sequence"


def run_streaming_zone_cross(out_usd: Path):
    """Same topology as render_wetbrush.run_streaming_zone but with mock_strokes."""
    g = RuzinoGraph("WetbrushCross")
    g.loadConfiguration(str(BIN / "geometry_nodes.json"))

    mock = g.createNode("mock_strokes", name="MockStrokes")
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
    g.addEdge(commit, "Paint Field 3D", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): 30, (mock, "Length"): 0.3,
        (mock, "Stroke 0 Duration"): 0.5, (mock, "Stroke 1 Start"): 0.5,
        (deposit, "Resolution"): 256, (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1, (fluid, "Brush Radius"): 0.02,
    })
    assert sim_in.paired_node is sim_out, "zone pairing not established"

    if out_usd.exists():
        out_usd.unlink()
    stage = stage_py.Stage(str(out_usd))
    prim_path = "/Brush"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)
    g.apply_to_stage(stage, prim_path)

    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    for i in range(NUM_FRAMES):
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()
    stage.save()

    # Compose skeleton + modifier (same as render_wetbrush).
    modifier = out_usd.with_name(out_usd.stem + "_modifiers.usdc")
    if not modifier.exists():
        sys.exit(f"modifier layer not found: {modifier} -- sim did not export")
    composed = out_usd.parent / "wetbrush_cross_composed.usda"
    if composed.exists():
        composed.unlink()
    layer = Sdf.Layer.CreateNew(str(composed))
    layer.subLayerPaths = [
        str(modifier.resolve()), str(out_usd.resolve())]
    layer.Save()
    composed_stage = Usd.Stage.Open(str(composed))

    _p = composed_stage.GetPrimAtPath(Sdf.Path(prim_path))
    _pts_attr = _p.GetAttribute("points")
    _pts_ts = _pts_attr.GetTimeSamples() if _pts_attr else []
    if not _pts_ts:
        sys.exit("composed stage has no time-sampled points -- sim failed")
    _final = _pts_attr.Get(_pts_ts[-1])
    print(f"[cross] stage 1: composed {len(_pts_ts)} time samples "
          f"({len(_final) if _final else 0} pts at final frame)")
    return composed_stage, prim_path, g


def main():
    # Point render_wetbrush's OUTPUT_DIR at our cross-sequence dir.
    rw.OUTPUT_DIR = OUTPUT_DIR

    sim_usd = BIN / "wetbrush_cross_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print("[cross] stage 1: running streaming zone (2 crossing strokes)")
    sim_stage, prim_path, sim_graph = run_streaming_zone_cross(sim_usd)

    scene = BIN / "wetbrush_cross.usdc"
    print(f"[cross] stage 2: baking render scene -> {scene.name}")
    frame_times = rw.bake_render_scene(sim_stage, prim_path, scene)

    print("[cross] stage 3: in-process render loop")
    rw.render_loop(scene, frame_times)


if __name__ == "__main__":
    main()
