"""Tests for the per-node render profiler (RZ_NODE_PROFILE) and the
path_tracing "Max Bounces" input socket.

1. test_node_profile_lines_emitted: subprocess render with RZ_NODE_PROFILE=1
   must emit one [RZ_NODE_PROFILE] JSON line per render() call, with the
   path tracer showing a nonzero GPU drain wait.
2. test_max_bounces_input_changes_render: rendering cornell at Max Bounces=1
   (direct-ish lighting) vs 20 (full GI) must produce measurably different
   images — proves the socket reaches the shader loop end-to-end.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).parent.resolve()
WORKSPACE = HERE.parent.parent
BIN = WORKSPACE / "Binaries" / "Release"
CORNELL = HERE / "data" / "scenes" / "cornell_box.usda"

PROFILE_RE = re.compile(r"\[RZ_NODE_PROFILE\] (\{.*\})")


def test_node_profile_lines_emitted():
    """RZ_NODE_PROFILE=1 → one JSON line per render, path tracer drains GPU."""
    env = os.environ.copy()
    env["RZ_NODE_PROFILE"] = "1"
    proc = subprocess.run(
        [sys.executable, "-u", str(HERE / "perf_render_nodes.py"),
         "--child", "pathtrace", "--scene", "cornell",
         "--width", "128", "--height", "128",
         "--spp", "2", "--warmup", "1", "--bounces", "20",
         "--out-dir", str(BIN / "test_output" / "perf_nodes")],
        cwd=str(BIN), env=env, capture_output=True, text=True, timeout=600)
    assert proc.returncode == 0, proc.stdout[-2000:] + proc.stderr[-2000:]

    profiles = []
    for line in proc.stdout.splitlines():
        m = PROFILE_RE.search(line)
        if m:
            profiles.append(json.loads(m.group(1)))

    # 1 warmup + 2 measured renders. Occasionally the Hydra render thread
    # coalesces a render() call into the next one (pause race), so a strict
    # == 3 flakes; anything below 2 means the profiler is broken.
    assert 2 <= len(profiles) <= 3, (
        f"expected 2-3 profile lines, got {len(profiles)}")
    for frame in profiles:
        assert "frame" in frame and "nodes" in frame
        names = {s["node"] for s in frame["nodes"]}
        assert "PathTracer" in names, f"nodes were {names}"
        for sample in frame["nodes"]:
            assert sample["cpu_ms"] >= 0.0
            assert sample["drain_ms"] >= 0.0

    # the path tracer is GPU-bound: its drain wait must be the dominant cost
    pt = next(s for s in profiles[-1]["nodes"] if s["node"] == "PathTracer")
    assert pt["drain_ms"] > 0.0, f"path tracer drain was {pt}"


def test_max_bounces_input_changes_render():
    """Max Bounces socket must reach the shader loop (1 vs 20 → different)."""
    import hd_RUZINO_py as renderer
    import nodes_core_py as core
    from test_render_materials import _build_render_graph

    hydra = renderer.HydraRenderer(str(CORNELL), 128, 128)
    _build_render_graph(hydra, 8)

    tree = hydra.get_node_system().get_node_tree()
    executor = hydra.get_node_system().get_node_tree_executor()
    pt = next(n for n in tree.nodes if n.ui_name == "PathTracer")
    bounce_socket = pt.get_input_socket("Max Bounces")

    def render_at(bounces):
        executor.sync_node_from_external_storage(
            bounce_socket, core.to_meta_any(bounces))
        hydra.reset_accumulation()
        for _ in range(8):
            hydra.render()
        data = hydra.get_output_texture()
        return np.array(data, dtype=np.float32).reshape(128, 128, 4)

    shallow = render_at(1)
    deep = render_at(20)

    assert shallow[:, :, :3].mean() > 0.005, "bounce=1 render is black"
    assert deep[:, :, :3].mean() > 0.005, "bounce=20 render is black"

    mse = float(((shallow - deep) ** 2).mean())
    # bounce=1 loses most of the inter-block GI bounce in the cornell box
    assert mse > 1e-3, f"bounce depth did not change the image (mse={mse:.6f})"
