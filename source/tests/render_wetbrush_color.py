#!/usr/bin/env python3
"""Single-color WetBrush stroke renderer (parameterized RYB).

Usage (from Binaries/Release):
    python ../../source/tests/render_wetbrush_color.py --ryb 0 0 1 --name blue
    python ../../source/tests/render_wetbrush_color.py --ryb 0 1 0 --name yellow
    WETBRUSH_RES=512 python ... (--res overrides env)
"""
import argparse
import os
import sys
from pathlib import Path

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


def build_sim_graph(sim_usd, ryb, res, res_z, paper):
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
        (mock, "Num Points"): 30, (mock, "Amplitude"): 0.02, (mock, "Length"): 0.3,
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
                    metavar=("R", "Y", "B"))
    ap.add_argument("--name", default=None)
    ap.add_argument("--res", type=int, default=None)
    ap.add_argument("--res-z", type=int, default=None)
    args = ap.parse_args()

    res = args.res or int(os.environ.get("WETBRUSH_RES", "1024"))
    res_z = args.res_z or int(os.environ.get("WETBRUSH_RES_Z", "32"))
    paper = rw.SIM_PAPER
    ryb = tuple(max(0.0, min(1.0, c)) for c in args.ryb)
    name = args.name or f"r{ryb[0]:.1f}_y{ryb[1]:.1f}_b{ryb[2]:.1f}".replace(".", "p")

    rw.SIM_RES = res
    rw.SIM_RES_Z = res_z
    out_dir = BIN / f"wetbrush_{name}_sequence"
    rw.OUTPUT_DIR = out_dir

    sim_usd = BIN / f"wetbrush_{name}_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print(f"[{name}] RYB={ryb} res={res}x{res}x{res_z} -> {out_dir.name}")
    sim_graph, stage, prim_path = build_sim_graph(sim_usd, ryb, res, res_z, paper)
    scene = BIN / f"wetbrush_{name}.usdc"
    build_marker_scene(scene)
    run_interleaved(scene, stage, sim_graph)


if __name__ == "__main__":
    main()
