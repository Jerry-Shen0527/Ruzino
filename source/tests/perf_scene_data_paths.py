#!/usr/bin/env python3
"""Performance baseline for the scene data-path (pools / lights / TLAS).

Guards the DeviceMemoryPool -> SceneStore rework: "the new version must not
be significantly slower than the old version" (single frame, multi frame,
high SPP, light churn). Run this on the OLD code to record a baseline, and
again after each migration step; compare the CSVs.

Scenarios
  1. single_frame_high_spp : terrain forest scene, 32 spp path tracing
                             (exercises full scene sync + TLAS + one pass)
  2. multi_frame           : 20 x (sim tick + render) wetbrush interleaved
                             loop (exercises per-frame re-sync + accumulate)
  3. light_churn           : 12-light scene, 10 rounds of
                             {delete 2 lights, add 2 lights, render 4 spp}
                             (exercises light_pool compress + rebuild path —
                             the exact path the rework replaces)

Usage (from Binaries/Release):
  python ../../source/tests/perf_scene_data_paths.py [--out DIR] [--rounds N]
Writes baseline.csv + baseline.json into the out dir (default
test_output/perf_baseline/).
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent.resolve()
WORKSPACE = HERE.parent.parent
BIN = WORKSPACE / "Binaries" / "Release"


def _run_logged(cmd, env_extra, tag, results, cwd):
    env = os.environ.copy()
    env.update(env_extra)
    t0 = time.perf_counter()
    proc = subprocess.run(
        cmd, cwd=str(cwd), env=env, capture_output=True, text=True)
    wall = time.perf_counter() - t0
    results.append({
        "scenario": tag,
        "wall_s": round(wall, 3),
        "exit_code": proc.returncode,
    })
    tail = "\n".join(proc.stdout.strip().splitlines()[-3:])
    print(f"[{tag}] wall={wall:.2f}s exit={proc.returncode}\n  {tail}")
    if proc.returncode != 0:
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
    return wall


def scenario_forest(results, rounds):
    """Single-frame high-SPP path tracing (existing script, env-tuned)."""
    for i in range(rounds):
        _run_logged(
            [sys.executable, str(HERE / "render_terrain_forest.py")],
            {"FOREST_TREES": "300", "FOREST_SPP": "32"},
            f"single_frame_high_spp_r{i}", results, BIN)


def scenario_wetbrush(results, rounds):
    """Multi-frame interleaved sim+render, shortened for baseline use."""
    for i in range(rounds):
        _run_logged(
            [sys.executable, str(HERE / "render_wetbrush.py")],
            {"WB_FRAMES": "20", "WETBRUSH_RES": "512", "WB_SPP": "4",
             "WB_OUT_DIR": f"perf_wetbrush_r{i}"},
            f"multi_frame_r{i}", results, BIN)


def scenario_light_churn(results, rounds):
    """Light churn: delete/add lights then render, measuring the round trip
    that today goes through light_pool.compress() (full-device sync)."""
    sys.path.insert(0, str(BIN))
    tests_dir = WORKSPACE / "source" / "Runtime" / "renderer" / "tests"
    sys.path.insert(0, str(tests_dir))
    os.environ.setdefault("PXR_USD_WINDOWS_DLL_PATH", str(BIN))
    os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN))

    import numpy as np  # noqa: F401
    import pxr.Usd as Usd
    import pxr.UsdGeom as UsdGeom
    from pxr import Gf, Sdf
    import hd_RUZINO_py as renderer
    from test_scene_mutation_consistency import (
        _scene_text, _build_render_graph, _render, _lights, SIZE)

    initial = _lights(12)
    scene = BIN / "test_output" / "perf_baseline" / "churn_scene.usda"
    scene.parent.mkdir(parents=True, exist_ok=True)
    scene.write_text(_scene_text(initial))

    def add_light(name, color):
        prim = stage.DefinePrim("/" + name, "DistantLight")
        prim.CreateAttribute("inputs:color", Sdf.ValueTypeNames.Color3f).Set(
            Gf.Vec3f(*color))
        prim.CreateAttribute("inputs:intensity", Sdf.ValueTypeNames.Float).Set(2.0)
        UsdGeom.Xformable(prim).AddTransformOp().Set(Gf.Matrix4d(
            1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1))

    hydra = renderer.HydraRenderer(str(scene), SIZE, SIZE)
    _build_render_graph(hydra, BIN, 4)
    _render(hydra, 4)  # warm-up: first render compiles shaders etc.

    stage = Usd.Stage.Open(str(scene))
    palette = {
        name: tuple(float(c) for c in color.strip("()").split(","))
        for name, color, _ in initial
    }
    round_times = []
    for r in range(rounds * 10):
        t0 = time.perf_counter()
        # delete two lights, add two churn lights, render, then restore
        victims = [f"L{((r * 2) % 12) + 1:02d}", f"L{((r * 2 + 1) % 12) + 1:02d}"]
        for name in victims:
            prim = stage.GetPrimAtPath(f"/{name}")
            if prim.IsValid():
                stage.RemovePrim(prim.GetPath())
        for k in range(2):
            add_light(f"CHURN_{r}_{k}", (0.3, 0.3, 0.9))
        _render(hydra, 4)
        round_times.append(time.perf_counter() - t0)
        # restore the original stage shape so light count stays constant
        for k in range(2):
            prim = stage.GetPrimAtPath(f"/CHURN_{r}_{k}")
            if prim.IsValid():
                stage.RemovePrim(prim.GetPath())
        for name in victims:
            if name in palette:
                add_light(name, palette[name])
    hydra.stop()

    avg = sum(round_times) / len(round_times)
    results.append({
        "scenario": "light_churn_avg_round",
        "wall_s": round(avg, 4),
        "exit_code": 0,
        "note": f"avg of {len(round_times)} rounds (delete2+add2+render4spp)",
    })
    print(f"[light_churn] avg round = {avg*1000:.1f}ms over {len(round_times)} rounds")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(BIN / "test_output" / "perf_baseline"))
    parser.add_argument("--rounds", type=int, default=2,
                        help="repetitions per subprocess scenario")
    parser.add_argument("--only", default="all",
                        choices=["all", "forest", "wetbrush", "churn"])
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    results = []
    if args.only in ("all", "forest"):
        scenario_forest(results, args.rounds)
    if args.only in ("all", "wetbrush"):
        scenario_wetbrush(results, args.rounds)
    if args.only in ("all", "churn"):
        scenario_light_churn(results, args.rounds)

    stamp = time.strftime("%Y%m%d_%H%M%S")
    csv_path = out / f"baseline_{stamp}.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["scenario", "wall_s", "exit_code", "note"])
        writer.writeheader()
        writer.writerows(results)
    (out / f"baseline_{stamp}.json").write_text(json.dumps({
        "timestamp": stamp,
        "git_note": "record with: git rev-parse HEAD",
        "results": results,
    }, indent=2))
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
