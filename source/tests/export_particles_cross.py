#!/usr/bin/env python3
"""
Export Wetbrush FLIP/PIC particles as a time-sampled USD file for
visualisation in Ruzino.exe (hdStorm).

Same simulation as render_wetbrush_cross.py (red horizontal + blue vertical
crossing strokes), but skips the bake and render — just captures per-frame
particle positions and colours from the commit node's "Paint Particles" output.

Run from Binaries/Release:

    python ../../source/tests/export_particles_cross.py
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

os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")

import numpy as np  # noqa: E402
from pxr import Usd, UsdGeom, Sdf, Gf, Vt  # noqa: E402

import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

NUM_FRAMES = 60
FPS = 60.0
DT = 1.0 / FPS

OUT_USD = BIN / "wetbrush_cross_particles.usdc"


def main():
    if OUT_USD.exists():
        OUT_USD.unlink()

    g = RuzinoGraph("WetbrushParticles")
    g.loadConfiguration(str(BIN / "geometry_nodes.json"))

    mock = g.createNode("mock_strokes", name="MockStrokes")
    init_state = g.createNode("brush_wb_init_state", name="InitState")
    sim_in, sim_out = g.createSimulationZone()
    emitter = g.createNode("mock_point_emitter", name="Emitter")
    deposit = g.createNode("brush_wb_deposit", name="Deposit")
    bristle = g.createNode("brush_wb_bristle", name="Bristle")
    fluid = g.createNode("brush_wb_fluid", name="Fluid")
    commit = g.createNode("brush_wb_commit", name="Commit")
    write_ptcl = g.createNode("write_usd", name="WriteParticles")

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
    g.addEdge(commit, "Paint Particles", write_ptcl, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): 30, (mock, "Length"): 0.3,
        (mock, "Stroke 0 Duration"): 0.5, (mock, "Stroke 1 Start"): 0.5,
        (deposit, "Resolution"): 512, (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 2.0, (fluid, "Brush Radius"): 0.02,
        # Write particles under /Brush/Particles so they don't redefine /Brush
        # (created as a Mesh below — UsdGeomPoints::Define on the same path
        # would crash Ruzino.exe's stage inspector).
        (write_ptcl, "Sub Path"): "Particles",
    })
    assert sim_in.paired_node is sim_out, "zone pairing not established"

    stage = stage_py.Stage(str(OUT_USD))
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

    modifier = OUT_USD.with_name(OUT_USD.stem + "_modifiers.usdc")
    if modifier.exists():
        composed = OUT_USD.parent / f"{OUT_USD.stem}_composed.usda"
        if composed.exists():
            composed.unlink()
        layer = Sdf.Layer.CreateNew(str(composed))
        layer.subLayerPaths = [
            str(modifier.resolve()), str(OUT_USD.resolve())]
        layer.Save()
        stage2 = Usd.Stage.Open(str(composed))
        # Particles live at /Brush/Particles (Sub Path); /Brush itself is an
        # empty Mesh defined only so write_usd has a parent prim to author
        # the modifier over.
        p = stage2.GetPrimAtPath(Sdf.Path(prim_path + "/Particles"))
        pts_attr = p.GetAttribute("points") if p else None
        ts = pts_attr.GetTimeSamples() if pts_attr else []
        if ts:
            final_pts = pts_attr.Get(ts[-1])
            print(f"done: {len(ts)} time samples, "
                  f"{len(final_pts) if final_pts else 0} pts at final frame")
            print(f"  composed USD: {composed}")
        else:
            print("WARNING: no time-sampled points found at "
                  f"{prim_path}/Particles")
    else:
        print("WARNING: modifier layer not found, sim may have failed")
        print(f"  raw USD: {OUT_USD}")

    print(f"\nOpen this file in Ruzino.exe to visualise particles:")
    print(f"  {OUT_USD}")


if __name__ == "__main__":
    main()
