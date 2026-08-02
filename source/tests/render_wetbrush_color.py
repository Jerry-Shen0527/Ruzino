#!/usr/bin/env python3
"""
Multi-color WetBrush single-stroke renderer.

Renders a single stroke in a user-specified RYB color so we can compare how
different pigments render (e.g. blue vs yellow vs green) and isolate whether a
visual artifact is color-specific or universal.

Usage (from Binaries/Release):

    # default: pure RYB blue
    python ../../source/tests/render_wetbrush_color.py

    # green (RYB yellow+blue)
    python ../../source/tests/render_wetbrush_color.py --ryb 0 1 1 --name green

    # red
    python ../../source/tests/render_wetbrush_color.py --ryb 1 0 0 --name red

    # custom resolution
    python ../../source/tests/render_wetbrush_color.py --ryb 0 0 1 --res 1024
"""
import argparse
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


def build_sim_graph(sim_usd: Path, ryb, res, res_z, paper):
    g = RuzinoGraph("WetbrushColor")
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
        (mock, "Ink R (RYB)"): float(ryb[0]),
        (mock, "Ink Y (RYB)"): float(ryb[1]),
        (mock, "Ink B (RYB)"): float(ryb[2]),
        (deposit, "Resolution"): res, (deposit, "Resolution Z"): res_z,
        (deposit, "Paper Size"): paper,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1, (fluid, "Brush Radius"): 0.02,
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--ryb", nargs=3, type=float, default=[0.0, 0.0, 1.0],
                    metavar=("R", "Y", "B"),
                    help="RYB ink color (each in [0,1])")
    ap.add_argument("--name", default=None,
                    help="output subdir name (default: from color)")
    ap.add_argument("--res", type=int, default=None,
                    help="sim resolution (default: env WETBRUSH_RES or 1024)")
    ap.add_argument("--res-z", type=int, default=None,
                    help="sim Z resolution (default: env WETBRUSH_RES_Z or 32)")
    args = ap.parse_args()

    res = args.res or int(os.environ.get("WETBRUSH_RES", "1024"))
    res_z = args.res_z or int(os.environ.get("WETBRUSH_RES_Z", "32"))
    paper = rw.SIM_PAPER

    ryb = tuple(max(0.0, min(1.0, c)) for c in args.ryb)
    if args.name:
        name = args.name
    else:
        name = f"r{ryb[0]:.1f}_y{ryb[1]:.1f}_b{ryb[2]:.1f}".replace(".", "p")

    # Override the shared sim-resolution module globals so build_marker_scene
    # bakes the marker primvars at the same resolution.
    rw.SIM_RES = res
    rw.SIM_RES_Z = res_z

    out_dir = BIN / f"wetbrush_{name}_sequence"
    rw.OUTPUT_DIR = out_dir

    sim_usd = BIN / f"wetbrush_{name}_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print(f"[{name}] RYB={ryb} res={res}x{res}x{res_z} -> {out_dir.name}")
    print(f"[{name}] stage 1a: building sim graph")
    sim_graph, stage, prim_path = build_sim_graph(sim_usd, ryb, res, res_z, paper)

    scene = BIN / f"wetbrush_{name}.usdc"
    print(f"[{name}] stage 1b: building marker render scene -> {scene.name}")
    build_marker_scene(scene)

    print(f"[{name}] stage 2: interleaved sim+render loop")
    run_interleaved(scene, stage, sim_graph)


if __name__ == "__main__":
    main()
