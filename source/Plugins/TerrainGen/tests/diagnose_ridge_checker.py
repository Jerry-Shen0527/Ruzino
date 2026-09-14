#!/usr/bin/env python3
"""
Ridge-sawtooth attribution: measure the 2-cell (checkerboard) mode energy
of the heightfield A) pure noise, B) after 250 hydraulic iterations. The
checker response of the 5-point Laplacian is ~2x the alternating
amplitude, ~0 for smooth modes. Same seed => same generator field.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
BIN = os.path.join(ROOT, "Binaries", "Release")
sys.path.insert(0, BIN)
sys.path.insert(0, os.path.join(ROOT, "source", "Core", "rznode", "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = BIN
os.environ["PATH"] = BIN + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(BIN)
os.chdir(BIN)

import numpy as np  # noqa: E402
from pxr import Usd, UsdGeom  # noqa: E402

RES = 2049
SIZE = 200.0
HF_ONLY = os.path.join(BIN, "test_output", "diagnose_hf_only.usdc")


def build_heightfield_only():
    import stage_py
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("DiagnoseHF")
    g.loadConfiguration(os.path.join(
        BIN, "Plugins", "TerrainGen_geometry_nodes.json"))
    g.loadConfiguration(os.path.join(BIN, "geometry_nodes.json"))
    hf = g.createNode("terrain_heightfield", name="hf")
    writer = g.createNode("write_usd", name="writer")
    g.addEdge(hf, "Height Field", writer, "Geometry")
    stage = stage_py.Stage(HF_ONLY)
    payload = stage_py.create_payload_from_stage(stage, "/terrain")
    g.setGlobalParams(payload)
    inputs = {
        (hf, "Resolution"): RES,
        (hf, "Size"): SIZE,
        (hf, "Height"): 40.0,
        (hf, "Seed"): 1234,
        (hf, "Octaves"): 6,
        (hf, "Persistence"): 0.45,
        (hf, "Ridge Blend"): 0.55,
        (hf, "Warp Strength"): 0.5,
    }
    g.prepare_and_execute(inputs, required_node=writer)
    stage.save()
    print("heightfield-only stage saved")


def build_thermal_field(iters=30):
    """Dense field after hydraulic + thermal, for checker measurement."""
    import stage_py
    from ruzino_graph import RuzinoGraph

    path = os.path.join(BIN, "test_output", "diagnose_hf_thermal.usdc")
    g = RuzinoGraph("DiagnoseThermal")
    g.loadConfiguration(os.path.join(
        BIN, "Plugins", "TerrainGen_geometry_nodes.json"))
    g.loadConfiguration(os.path.join(BIN, "geometry_nodes.json"))
    hf = g.createNode("terrain_heightfield", name="hf")
    erode = g.createNode("terrain_erode_hydraulic", name="erode")
    thermal = g.createNode("terrain_erode_thermal", name="thermal")
    writer = g.createNode("write_usd", name="writer")
    g.addEdge(hf, "Height Field", erode, "Height Field")
    g.addEdge(erode, "Height Field", thermal, "Height Field")
    g.addEdge(thermal, "Height Field", writer, "Geometry")
    stage = stage_py.Stage(path)
    payload = stage_py.create_payload_from_stage(stage, "/terrain")
    g.setGlobalParams(payload)
    inputs = {
        (hf, "Resolution"): RES,
        (hf, "Size"): SIZE,
        (hf, "Height"): 40.0,
        (hf, "Seed"): 1234,
        (hf, "Octaves"): 6,
        (hf, "Persistence"): 0.45,
        (hf, "Ridge Blend"): 0.55,
        (hf, "Warp Strength"): 0.5,
        (erode, "Method"): "Virtual Pipes (GPU)",
        (erode, "Iterations"): 250,
        (thermal, "Iterations"): iters,
    }
    g.prepare_and_execute(inputs, required_node=writer)
    stage.save()
    print(f"thermal field stage saved ({iters} iters)")
    return path


def load_field(usd_path):
    stage = Usd.Stage.Open(usd_path.replace(".usdc", "_modifiers.usdc"))
    prim = UsdGeom.Mesh(stage.GetPrimAtPath("/terrain"))
    pts = np.array(prim.GetPointsAttr().Get(), dtype=np.float64)
    n = len(pts)
    res = int(round(n ** 0.5))
    assert res * res == n, f"not a square grid: {n}"
    return pts[:, 1].reshape(res, res)


def checker_energy(h):
    """Alternating 2-cell amplitude per cell: |h - avg(4 neighbors)| / 2."""
    c = h - 0.25 * (
        np.roll(h, 1, 0) + np.roll(h, -1, 0)
        + np.roll(h, 1, 1) + np.roll(h, -1, 1))
    return np.abs(c) * 0.5


def report(tag, h):
    e = checker_energy(h)
    # interior only (rolls wrap the border)
    e_in = e[2:-2, 2:-2]
    h_in = h[2:-2, 2:-2]
    hi = h_in > np.percentile(h_in, 90)
    lo = h_in < np.percentile(h_in, 40)
    print(f"\n=== {tag} ===")
    print(f"checker amplitude: mean {e_in.mean()*100:.2f}cm  "
          f"p99 {np.percentile(e_in, 99)*100:.2f}cm  "
          f"max {e_in.max()*100:.2f}cm  (cell 9.77cm)")
    print(f"ridges(top 10% h): mean {e_in[hi].mean()*100:.2f}cm   "
          f"valleys(bot 40%): mean {e_in[lo].mean()*100:.2f}cm")
    # 48x56 block map of mean checker amplitude (relative)
    bs = 42
    blk = e_in[: e_in.shape[0] // bs * bs, : e_in.shape[1] // bs * bs]
    blk = blk.reshape(blk.shape[0] // bs, bs,
                      blk.shape[1] // bs, bs).mean(axis=(1, 3))
    ramp = " .:-=+*#%@"
    top = max(blk.max(), 1e-9)
    print("checker energy map (each block 4.1m, relative):")
    for row in blk:
        print("".join(ramp[min(int(v / top * 9), 9)] for v in row))
    return e


if __name__ == "__main__":
    thermal = "thermal" in sys.argv
    if not os.path.exists(HF_ONLY):
        build_heightfield_only()
    h_noise = load_field(HF_ONLY)
    if thermal:
        b_path = build_thermal_field(30)
    else:
        b_path = os.path.join(BIN, "test_output", "terrain_preview.usdc")
    h_eroded = load_field(b_path)
    tag = "B: hydraulic 250 + thermal 30" if thermal \
        else "B: after 250 hydraulic iterations"
    e_noise = report("A: pure noise field (no erosion)", h_noise)
    e_eroded = report(tag, h_eroded)
    gain = e_eroded[2:-2, 2:-2].mean() / max(e_noise[2:-2, 2:-2].mean(), 1e-12)
    print(f"\nchecker energy ratio eroded/noise: {gain:.2f}x")
    print(">1.5x => solver-generated; ~1x => generator-native aliasing")
