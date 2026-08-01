#!/usr/bin/env python3
"""
DIAGNOSTIC: single BLUE stroke (no yellow) — isolates whether the blue-stroke
speckle exists for a blue brush on clean paper (brush/fluid/render issue) or
only appears when blue crosses the yellow stroke (wet-in-wet interaction).

Same interleaved architecture as render_wetbrush.py, only the mock_stroke ink
color is set to RYB blue (0,0,1).

Run from Binaries/Release:

    python ../../source/tests/render_wetbrush_blue.py
"""
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
sys.path.insert(0, str(HERE))

os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")

from pxr import Usd, UsdGeom, Sdf  # noqa: E402

import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

import render_wetbrush as rw
from render_wetbrush import build_marker_scene, run_interleaved  # noqa: E402

OUTPUT_DIR = BIN / "wetbrush_blue_sequence"


def build_sim_graph_blue(sim_usd: Path):
    g = RuzinoGraph("WetbrushBlue")
    g.loadConfiguration(str(BIN / "geometry_nodes.json"))

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
    g.addEdge(commit, "Paint Field 3D", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): 30, (mock, "Amplitude"): 0.02,
        (mock, "Length"): 0.3,
        (mock, "Ink R (RYB)"): 0.0, (mock, "Ink Y (RYB)"): 0.0,
        (mock, "Ink B (RYB)"): 1.0,
        (deposit, "Resolution"): rw.SIM_RES, (deposit, "Resolution Z"): rw.SIM_RES_Z,
        (deposit, "Paper Size"): rw.SIM_PAPER,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 2.0, (fluid, "Brush Radius"): 0.02,
    })
    assert sim_in.paired_node is sim_out, "zone pairing not established"

    if sim_usd.exists():
        sim_usd.unlink()
    stage = stage_py.Stage(str(sim_usd))
    prim_path = "/Brush"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)
    g.apply_to_stage(stage, prim_path)

    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)
    return g, stage, prim_path


def main():
    rw.OUTPUT_DIR = OUTPUT_DIR

    sim_usd = BIN / "wetbrush_blue_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print("[blue] stage 1a: building single-blue-stroke sim graph (zero-copy)")
    sim_graph, stage, prim_path = build_sim_graph_blue(sim_usd)

    scene = BIN / "wetbrush_blue.usdc"
    print(f"[blue] stage 1b: building marker render scene -> {scene.name}")
    build_marker_scene(scene)

    print("[blue] stage 2: interleaved sim+render loop")
    run_interleaved(scene, stage, sim_graph)


if __name__ == "__main__":
    main()
