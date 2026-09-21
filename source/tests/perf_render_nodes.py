#!/usr/bin/env python3
"""Per-node render-path profiler over a fixed scenario set.

Answers "how long does each render-graph node take" — including GPU-driven
nodes whose synchronization does not happen inside the node. Enabled by
running each scenario with RZ_NODE_PROFILE=1: the render executor then wraps
every node with CPU timing plus an EventQuery drain wait (serialized GPU
attribution) and emits one JSON line per frame:

    [RZ_NODE_PROFILE] {"frame":3,"nodes":[{"node":"PathTracer","cpu_ms":...,"drain_ms":...},...]}

This harness (run from anywhere; children run with cwd=Binaries/Release):

  python ../../source/tests/perf_render_nodes.py                # default set
  python ../../source/tests/perf_render_nodes.py --only cornell,transmission
  python ../../source/tests/perf_render_nodes.py --include-heavy # + cloud volume

Fixed scenario set (each in its own subprocess — the HD plugin is a
process-level singleton):

  cornell            RectLight + indirect GI baseline      256x256, 32 spp
  many_lights        12 DistantLights, analytic NEE        128x128, 16 spp
  emissive_many      5 emissive panels + LightBVH          192x128, 32 spp
  transmission       3 glass spheres (BTDF)                256x256, 32 spp
  complex_materials  MaterialX mixed shading networks      256x256, 32 spp
  bounce_N           cornell at Max Bounces {2,5,10,20}    128x128, 16 spp
  raster_deferred    rasterize -> deferred direct lighting 256x256, 60 frames
  forest             (script) terrain+scatter+2 instancers 300 trees, 32 spp
  cloud_volume       (script, heavy) procedural volume     320px, gated

Writes profile_<stamp>.csv/.json into test_output/perf_nodes/ and prints a
per-scenario table: avg cpu_ms / drain_ms / total per node, share of frame,
and the unprofiled reference frame time (serialization overhead = how much
the per-node drain waits perturb the frame; the per-node split itself stays
valid).
"""
import argparse
import csv
import json
import os
import re
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).parent.resolve()
WORKSPACE = HERE.parent.parent
BIN = WORKSPACE / "Binaries" / "Release"
RENDERER_TESTS = WORKSPACE / "source" / "Runtime" / "renderer" / "tests"
SCENES = HERE / "data" / "scenes"

PROFILE_RE = re.compile(r"\[RZ_NODE_PROFILE\] (\{.*\})")
WALL_RE = re.compile(r"\[RZ_WALL_SUMMARY\] (\{.*\})")

WARMUP_GRAPH = 3   # first frames: shader compile + scene sync + light flush
WARMUP_SCRIPT = 2  # script scenarios cannot control warmup precisely

# scene key -> usda path (graph mode)
SCENE_FILES = {
    "cornell": SCENES / "cornell_box.usda",
    "emissive_many": SCENES / "emissive_many_lights.usda",
    "transmission": SCENES / "materials" / "mat_transmission_sphere.usda",
    "complex_materials": SCENES / "materials" / "mat_mixed.usda",
}

# The default scenario set, in run order. "kind": pathtrace | raster | script
SCENARIOS = [
    dict(name="cornell", kind="pathtrace", scene="cornell",
         size=(256, 256), spp=32, bounces=20,
         desc="RectLight + indirect GI baseline"),
    dict(name="many_lights", kind="pathtrace", scene="many_lights",
         size=(128, 128), spp=16, bounces=20,
         desc="12 DistantLights, analytic-light NEE"),
    dict(name="emissive_many", kind="pathtrace", scene="emissive_many",
         size=(192, 128), spp=32, bounces=20,
         desc="5 emissive panels + LightBVH"),
    dict(name="transmission", kind="pathtrace", scene="transmission",
         size=(256, 256), spp=32, bounces=20,
         desc="3 glass spheres, rough BTDF"),
    dict(name="complex_materials", kind="pathtrace", scene="complex_materials",
         size=(256, 256), spp=32, bounces=20,
         desc="MaterialX mixed shading networks"),
    dict(name="raster_deferred", kind="raster",
         size=(256, 256), frames=60,
         desc="rasterize -> deferred direct lighting (realtime path)"),
    dict(name="forest", kind="script",
         cmd=[sys.executable, str(HERE / "render_terrain_forest.py")],
         env={"FOREST_TREES": "300", "FOREST_SPP": "32", "FOREST_RES": "512"},
         desc="terrain + scatter + 2 tree instancers + dome/distant (heavy)"),
]
# bounce curve expands to bounce_2/5/10/20 after the base cornell scenario
BOUNCE_CURVE = dict(base="cornell", size=(128, 128), spp=16, bounces=[2, 5, 10, 20])
# gated behind --include-heavy (long RT dispatches; see memory on cloud freezes)
HEAVY_SCENARIOS = [
    dict(name="cloud_volume", kind="script",
         cmd=[sys.executable, str(HERE / "render_clouds.py")],
         env={"CLOUD_RES": "320", "CLOUD_SPP": "16"},
         desc="Hosek sky + procedural volume, delta tracking"),
]


# ---------------------------------------------------------------- child mode

def _prepare_child_env():
    """Mirror the conftest DLL/PATH dance so the child can import hd_RUZINO_py."""
    os.environ.setdefault("PXR_USD_WINDOWS_DLL_PATH", str(BIN))
    mtlx_stdlib = BIN / "libraries"
    if mtlx_stdlib.exists():
        os.environ.setdefault("PXR_MTLX_STDLIB_SEARCH_PATHS", str(mtlx_stdlib))
    os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN))
    # HERE must win the conftest resolution: test_render_materials does
    # `from conftest import TEST_OUTPUT_DIR` and RENDERER_TESTS ships its own
    # conftest without it. HERE is often already in sys.path as the script
    # dir — remove it and re-insert at 0 so it beats RENDERER_TESTS.
    for p in (str(RENDERER_TESTS), str(BIN)):
        if p not in sys.path:
            sys.path.insert(0, p)
    here_str = str(HERE)
    if here_str in sys.path:
        sys.path.remove(here_str)
    sys.path.insert(0, here_str)


def _make_many_lights_scene(out_dir):
    """Reuse the mutation-consistency 12-distant-light scene generator."""
    from test_scene_mutation_consistency import _lights, _scene_text

    path = out_dir / "many_lights.usda"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_scene_text(_lights(12)))
    return path


def _child(args):
    _prepare_child_env()
    import nodes_core_py as core  # noqa: F401
    import hd_RUZINO_py as renderer

    out_dir = Path(args.out_dir)

    if args.child == "pathtrace":
        if args.scene == "many_lights":
            scene_path = _make_many_lights_scene(out_dir)
        else:
            scene_path = SCENE_FILES[args.scene]

        hydra = renderer.HydraRenderer(
            str(scene_path), args.width, args.height)
        from test_render_materials import _build_render_graph
        # accumulate "Max Samples" counts every render() call, warmup included
        _build_render_graph(hydra, args.warmup + args.spp)

        tree = hydra.get_node_system().get_node_tree()
        pt = next(n for n in tree.nodes if n.ui_name == "PathTracer")
        executor = hydra.get_node_system().get_node_tree_executor()
        executor.sync_node_from_external_storage(
            pt.get_input_socket("Max Bounces"), core.to_meta_any(args.bounces))
        total = args.warmup + args.spp
    else:  # raster
        from test_raster_pipeline import (
            _build_full_raster_graph, _build_instancer_scene)
        scene_path = out_dir / "raster_instancer.usda"
        scene_path.parent.mkdir(parents=True, exist_ok=True)
        _build_instancer_scene(scene_path)
        hydra = renderer.HydraRenderer(
            str(scene_path), args.width, args.height)
        _build_full_raster_graph(hydra, BIN)
        total = args.warmup + args.frames

    walls = []
    for _ in range(total):
        t0 = time.perf_counter()
        hydra.render()
        walls.append((time.perf_counter() - t0) * 1000.0)

    measured = walls[args.warmup:]
    summary = {
        "avg_frame_ms": round(statistics.mean(measured), 3),
        "min_frame_ms": round(min(measured), 3),
        "max_frame_ms": round(max(measured), 3),
        "median_frame_ms": round(statistics.median(measured), 3),
        "measured_frames": len(measured),
    }
    print(f"[RZ_WALL_SUMMARY] {json.dumps(summary)}", flush=True)


# ----------------------------------------------------------------- parent

def _parse_stream(stdout):
    profiles = []
    wall = None
    for line in stdout.splitlines():
        m = PROFILE_RE.search(line)
        if m:
            profiles.append(json.loads(m.group(1)))
            continue
        m = WALL_RE.search(line)
        if m:
            wall = json.loads(m.group(1))
    return profiles, wall


def _run_child(scenario, out_dir, profiled, overrides):
    """Run one graph-mode scenario subprocess; return (profiles, wall, exit)."""
    env = os.environ.copy()
    env.pop("RZ_NODE_PROFILE", None)
    if profiled:
        env["RZ_NODE_PROFILE"] = "1"
    cmd = [
        sys.executable, "-u", str(HERE / "perf_render_nodes.py"),
        "--child", scenario["kind"],
        "--scene", scenario.get("scene", ""),
        "--width", str(overrides.get("width", scenario["size"][0])),
        "--height", str(overrides.get("height", scenario["size"][1])),
        "--warmup", str(overrides.get("warmup", WARMUP_GRAPH)),
        "--spp", str(overrides.get("spp", scenario.get("spp", 1))),
        "--frames", str(overrides.get("frames", scenario.get("frames", 1))),
        "--bounces", str(overrides.get("bounces", scenario.get("bounces", 20))),
        "--out-dir", str(out_dir),
    ]
    proc = subprocess.run(cmd, cwd=str(BIN), env=env, capture_output=True,
                          text=True, timeout=1200)
    profiles, wall = _parse_stream(proc.stdout)
    return profiles, wall, proc


def _run_script(scenario, profiled, overrides):
    """Run one script-mode scenario (existing render_*.py) with profiling."""
    env = os.environ.copy()
    env.pop("RZ_NODE_PROFILE", None)
    if profiled:
        env["RZ_NODE_PROFILE"] = "1"
    env.update(scenario.get("env", {}))
    t0 = time.perf_counter()
    proc = subprocess.run(scenario["cmd"], cwd=str(BIN), env=env,
                          capture_output=True, text=True, timeout=3600)
    wall_s = time.perf_counter() - t0
    profiles, _ = _parse_stream(proc.stdout)
    return profiles, {"wall_s": round(wall_s, 3), "exit_code": proc.returncode,
                      "measured_frames": max(len(profiles) - WARMUP_SCRIPT, 0)}, proc


def _aggregate(profiles, warmup):
    """Average per-node cpu/drain over frames [warmup:]."""
    per_node = defaultdict(list)
    kept = 0
    for frame in profiles[warmup:]:
        for sample in frame["nodes"]:
            per_node[sample["node"]].append(sample)
        kept += 1
    stats = {}
    for node, samples in per_node.items():
        cpu = statistics.mean(s["cpu_ms"] for s in samples)
        drain = statistics.mean(s["drain_ms"] for s in samples)
        stats[node] = {"cpu_ms": cpu, "drain_ms": drain,
                       "total_ms": cpu + drain, "frames": len(samples)}
    return stats, kept


def _print_table(name, desc, stats, kept, wall_ref, wall_profiled):
    frame_total = sum(s["total_ms"] for s in stats.values())
    print(f"\n=== {name} — {desc}")
    suffix = ""
    if wall_profiled:
        suffix = (f" | profiled wall avg {wall_profiled['avg_frame_ms']:.1f} ms"
                  f" ({wall_profiled['measured_frames']} frames)")
    if wall_ref and "avg_frame_ms" in wall_ref:
        suffix += f" | UNPROFILED ref avg {wall_ref['avg_frame_ms']:.1f} ms"
        if wall_profiled:
            over = (frame_total / wall_ref["avg_frame_ms"] - 1.0) * 100.0
            suffix += f" | serialization overhead {over:+.1f}%"
    print(f"    nodes {len(stats)}, measured frames {kept}{suffix}")
    print(f"    {'node':<24}{'cpu_ms':>10}{'drain_ms':>11}{'total_ms':>10}{'%frame':>8}")
    for node, s in sorted(stats.items(), key=lambda kv: -kv[1]["total_ms"]):
        pct = 100.0 * s["total_ms"] / frame_total if frame_total else 0.0
        print(f"    {node:<24}{s['cpu_ms']:>10.2f}{s['drain_ms']:>11.2f}"
              f"{s['total_ms']:>10.2f}{pct:>7.1f}%")
    print(f"    {'(frame sum)':<24}{'':>10}{'':>11}{frame_total:>10.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--child", metavar="KIND", help=argparse.SUPPRESS)
    parser.add_argument("--scene", default="", help=argparse.SUPPRESS)
    parser.add_argument("--width", type=int, default=256, help=argparse.SUPPRESS)
    parser.add_argument("--height", type=int, default=256, help=argparse.SUPPRESS)
    parser.add_argument("--spp", type=int, default=1, help=argparse.SUPPRESS)
    parser.add_argument("--frames", type=int, default=1, help=argparse.SUPPRESS)
    parser.add_argument("--warmup", type=int, default=WARMUP_GRAPH, help=argparse.SUPPRESS)
    parser.add_argument("--bounces", type=int, default=20, help=argparse.SUPPRESS)
    parser.add_argument("--out-dir", default="", help=argparse.SUPPRESS)
    parser.add_argument("--only", default="all",
                        help="comma list of scenario names (see --list)")
    parser.add_argument("--include-heavy", action="store_true",
                        help="add the heavy/gated scenarios (cloud volume)")
    parser.add_argument("--skip-ref", action="store_true",
                        help="skip the unprofiled reference run")
    parser.add_argument("--list", action="store_true", help="list scenarios")
    args = parser.parse_args()

    if args.child:
        _child(args)
        return

    out_dir = BIN / "test_output" / "perf_nodes"
    out_dir.mkdir(parents=True, exist_ok=True)

    scenarios = list(SCENARIOS)
    for b in BOUNCE_CURVE["bounces"]:
        scenarios.append(dict(
            name=f"bounce_{b}", kind="pathtrace", scene=BOUNCE_CURVE["base"],
            size=BOUNCE_CURVE["size"], spp=BOUNCE_CURVE["spp"], bounces=b,
            desc=f"cornell Max Bounces={b} (bounce scaling curve)"))
    if args.include_heavy:
        scenarios += HEAVY_SCENARIOS

    if args.list:
        for s in scenarios:
            print(f"{s['name']:<20} {s['kind']:<10} {s['desc']}")
        return

    if args.only != "all":
        wanted = {x.strip() for x in args.only.split(",")}
        scenarios = [s for s in scenarios if s["name"] in wanted]
        missing = wanted - {s["name"] for s in scenarios}
        if missing:
            sys.exit(f"unknown scenario(s): {','.join(sorted(missing))} (see --list)")

    rows = []
    report = {"timestamp": time.strftime("%Y%m%d_%H%M%S"), "scenarios": []}
    for s in scenarios:
        name = s["name"]
        print(f"\n>>>>>> {name} ({s['kind']})", flush=True)

        if s["kind"] in ("pathtrace", "raster"):
            wall_ref = None
            if not args.skip_ref:
                _, wall_ref, proc = _run_child(s, out_dir, profiled=False,
                                               overrides={})
                if proc.returncode != 0 or wall_ref is None:
                    print(f"  [ref run FAILED exit={proc.returncode}]")
                    print(proc.stdout[-1500:], proc.stderr[-1500:])
            profiles, wall_prof, proc = _run_child(s, out_dir, profiled=True,
                                                   overrides={})
            warmup = WARMUP_GRAPH
        else:  # script
            profiles, wall_prof, proc = _run_script(s, profiled=True,
                                                    overrides={})
            wall_ref = wall_prof  # only a whole-run wall is available
            warmup = WARMUP_SCRIPT

        if proc.returncode != 0 or not profiles:
            print(f"  [PROFILED run FAILED exit={proc.returncode}, "
                  f"{len(profiles)} profile lines]")
            print(proc.stdout[-1500:], proc.stderr[-1500:])
            report["scenarios"].append({"name": name, "error": proc.returncode})
            continue

        stats, kept = _aggregate(profiles, warmup)
        _print_table(name, s["desc"], stats, kept, wall_ref, wall_prof)

        report["scenarios"].append({
            "name": name, "desc": s["desc"], "measured_frames": kept,
            "wall_profiled": wall_prof, "wall_reference": wall_ref,
            "nodes": {n: {k: round(v, 4) for k, v in st.items()}
                      for n, st in stats.items()},
        })
        for node, st in sorted(stats.items(), key=lambda kv: -kv[1]["total_ms"]):
            rows.append({
                "scenario": name, "node": node,
                "cpu_ms": round(st["cpu_ms"], 4),
                "drain_ms": round(st["drain_ms"], 4),
                "total_ms": round(st["total_ms"], 4),
                "frames": kept,
                "ref_avg_frame_ms": wall_ref.get("avg_frame_ms")
                if wall_ref and "avg_frame_ms" in wall_ref else "",
            })

    stamp = time.strftime("%Y%m%d_%H%M%S")
    csv_path = out_dir / f"profile_{stamp}.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else
                                ["scenario", "node"])
        writer.writeheader()
        writer.writerows(rows)
    (out_dir / f"profile_{stamp}.json").write_text(json.dumps(report, indent=2))
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
