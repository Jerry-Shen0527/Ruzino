#!/usr/bin/env python3
"""Shared helpers for the Hydra 2.0 tests.

One subprocess render script serves every hydra2 test (ingest smoke stages
and the scene-index chain liveness probes): it loads a stage through
hd_RUZINO_py's offline HydraRenderer with a fixed node graph and a fixed
sample count, renders N frames, optionally saves the output texture, and
prints RENDER_DONE. Subprocess isolation keeps GPU/USD global state from
leaking between renders.

History: during the migration the ingest stages were A/B tests rendering
every stage twice, once on the direct-read path and once with
RZ_HYDRA2_PREFER_LEGACY=1 forcing the legacy HdSceneDelegate::Get* pull,
asserting pixel parity. The gate was removed after the migration passed
pixel-parity validation (2026-09-02); the stages remain as ingest
regression pins (geomSubsets / material networks / instancer / lights /
visibility).
"""

import os
import subprocess
import sys
from pathlib import Path

import numpy as np

_RENDER_SCRIPT = r"""
import os, sys

binary_dir, stage_path, rznode_py, out_npy, frames_s = sys.argv[1:6]
frames = int(frames_s)

sys.path.insert(0, binary_dir)
sys.path.insert(0, rznode_py)
os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', binary_dir)
os.environ['PATH'] = binary_dir + os.pathsep + os.environ.get('PATH', '')

import nodes_core_py as core
import hd_RUZINO_py as renderer

hydra = renderer.HydraRenderer(stage_path, 64, 64)

node_system = hydra.get_node_system()
config = os.path.join(binary_dir, 'render_nodes.json')
if not os.path.exists(config):
    print('NO_RENDER_NODES_CONFIG')
    sys.exit(0)
node_system.load_configuration(config)
node_system.init()

tree = node_system.get_node_tree()
executor = node_system.get_node_tree_executor()

rng = tree.add_node("rng_texture")
ray_gen = tree.add_node("node_render_ray_generation")
path_trace = tree.add_node("path_tracing")
accumulate = tree.add_node("accumulate")
rng_buffer = tree.add_node("rng_buffer")
present = tree.add_node("present_color")

tree.add_link(rng.get_output_socket("Random Number"), ray_gen.get_input_socket("random seeds"))
tree.add_link(ray_gen.get_output_socket("Pixel Target"), path_trace.get_input_socket("Pixel Target"))
tree.add_link(ray_gen.get_output_socket("Rays"), path_trace.get_input_socket("Rays"))
tree.add_link(rng_buffer.get_output_socket("Random Number"), path_trace.get_input_socket("Random Seeds"))
tree.add_link(path_trace.get_output_socket("Output"), accumulate.get_input_socket("Texture"))
tree.add_link(accumulate.get_output_socket("Accumulated"), present.get_input_socket("Color"))

executor.reset_allocator()
executor.prepare_tree(tree, present)

for _ in range(frames):
    hydra.render()
hydra.stop()

if out_npy:
    import numpy as np
    img = np.array(hydra.get_output_texture(), dtype=np.float32).reshape(64, 64, 4)
    np.save(out_npy, img)
print('RENDER_DONE')
"""


def prepare_env():
    """Return (workspace_root, binary_dir) honoring RZ_BUILD_TYPE."""
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
    build_type = os.environ.get("RZ_BUILD_TYPE", "Release")
    binary_dir = workspace_root / "Binaries" / build_type
    if not binary_dir.exists():
        binary_dir = workspace_root / "Binaries" / "Release"
    return workspace_root, binary_dir


def hydra_py_available(binary_dir: Path) -> bool:
    return bool(list(binary_dir.glob("hd_RUZINO_py*")))


def run_render_subprocess(
    binary_dir, stage_path, *, frames=1, out_npy=None, env=None, timeout=180
):
    """Render `stage_path` once in a subprocess; returns CompletedProcess.

    env=None copies os.environ with HD_RUZINO_SIM_SCENE_INDEX_DEBUG removed
    (debug logs off); pass a full env dict to override.
    """
    rznode_py = binary_dir.parent.parent / "source" / "Core" / "rznode" / "python"
    if env is None:
        env = os.environ.copy()
        env.pop("HD_RUZINO_SIM_SCENE_INDEX_DEBUG", None)
    return subprocess.run(
        [sys.executable, "-c", _RENDER_SCRIPT,
         str(binary_dir), str(stage_path), str(rznode_py),
         str(out_npy or ""), str(frames)],
        capture_output=True, text=True, timeout=timeout,
        cwd=str(binary_dir), env=env,
    )


def render_to_array(binary_dir, stage_path, tmp_dir, frames=4):
    """Render `stage_path` and return the 64x64x4 float image."""
    out_npy = Path(tmp_dir) / "render.npy"
    r = run_render_subprocess(binary_dir, stage_path, frames=frames,
                              out_npy=out_npy)
    combined = r.stdout + r.stderr
    assert "RENDER_DONE" in combined, (
        f"render failed. tail:\n{combined[-2000:]}")
    return np.load(out_npy)


def assert_renders(binary_dir, stage_path, tmp_dir, label, blank_threshold=1e-3):
    """Render `stage_path` and assert a lit, non-blank image.

    Returns the image mean for reporting.
    """
    img = render_to_array(binary_dir, stage_path, tmp_dir)
    mean = float(img[:, :, :3].mean())
    assert mean > blank_threshold, (
        f"{label}: render blank (mean={mean}).")
    return mean
