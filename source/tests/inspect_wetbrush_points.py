"""Inspect the actual paint-particle point cloud the streaming Wetbrush zone
produces. Throwaway diagnostic — run from Binaries/Release so DLLs resolve.

Prints: per-frame point counts (accumulation), final-frame count, bounding
box, NaN check, and a coordinate sample, so we can see the actual painted
points rather than just trusting the test's n>0 assertion.
"""
import math
import os
from pathlib import Path

import stage_py
from pxr import Usd, UsdGeom, Sdf
from ruzino_graph import RuzinoGraph

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data" / "output"
OUTPUT_DIR = DATA_DIR

NUM_FRAMES = 12
FPS = 60.0
DT = 1.0 / FPS


def build_graph():
    g = RuzinoGraph("WetbrushInspect")
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
        (mock, "Num Points"): 30, (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (deposit, "Resolution"): 256, (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1, (fluid, "Brush Radius"): 0.02,
    })
    return g, commit


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    g, commit = build_graph()

    out_usd = str(OUTPUT_DIR / "wetbrush_inspect.usdc")
    if os.path.exists(out_usd):
        os.remove(out_usd)
    stage = stage_py.Stage(out_usd)
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

    pxr_stage = stage.get_pxr_stage()
    brush = pxr_stage.GetPrimAtPath(Sdf.Path(prim_path))
    points_attr = brush.GetAttribute("points")
    times = points_attr.GetTimeSamples() if points_attr else []

    out = []
    out.append("=== AUTHORED FRAMES (time samples of points) ===")
    out.append("  %d frames" % len(times))
    out.append("")
    out.append("=== PAINT PARTICLES PER FRAME (accumulation) ===")
    for t in times:
        pts = points_attr.Get(t)
        out.append("  t=%.4f: %d points" % (t, len(pts) if pts else 0))
    out.append("")

    if times:
        pts = points_attr.Get(max(times))
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        zs = [p[2] for p in pts]
        has_nan = any(
            not (math.isfinite(p[0]) and math.isfinite(p[1])
                 and math.isfinite(p[2])) for p in pts)
        out.append("=== FINAL FRAME (t=%.4f) ===" % max(times))
        out.append("  total painted points: %d" % len(pts))
        out.append("  has NaN/Inf: %s" % has_nan)
        out.append("  X [%.4f, %.4f]  span %.4f" %
                   (min(xs), max(xs), max(xs) - min(xs)))
        out.append("  Y [%.4f, %.4f]  span %.4f" %
                   (min(ys), max(ys), max(ys) - min(ys)))
        out.append("  Z [%.4f, %.4f]  span %.4f" %
                   (min(zs), max(zs), max(zs) - min(zs)))
        out.append("  first 6 points:")
        for p in pts[:6]:
            out.append("    (%.4f, %.4f, %.4f)" % (p[0], p[1], p[2]))

    report = "\n".join(out)
    print(report)
    with open(OUTPUT_DIR / "inspect_report.txt", "w") as f:
        f.write(report)


if __name__ == "__main__":
    main()
