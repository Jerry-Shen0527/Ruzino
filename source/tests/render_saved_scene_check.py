#!/usr/bin/env python3
"""Black-frame repro harness: render a SAVED scene cold and report the mean.

Run from Binaries/Release:
    python ../../source/tests/render_saved_scene_check.py <scene.usdc> <tag> [spp]

Pairs with a bash loop that clears Binaries/Release/shader_cache between runs
to force the cold-start (first-run-after-build) condition under which the
black frame was observed. Prints "[rsc] mean = R G B" and exits 1 on a black
frame (mean < 0.01).
"""
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))
os.chdir(str(BIN))

import numpy as np  # noqa: E402

scene_arg, tag = sys.argv[1], sys.argv[2]
SPP = int(sys.argv[3]) if len(sys.argv) > 3 else 8
WIDTH, HEIGHT = 960, 540

OUT_DIR = BIN / "test_output" / "blackframe_repro"
OUT_DIR.mkdir(parents=True, exist_ok=True)

import hd_RUZINO_py as renderer  # noqa: E402
import nodes_core_py as core  # noqa: E402


def locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / \
        "render_nodes_save.json"
    if fallback.exists():
        return fallback
    sys.exit("render node config not found")


hydra = renderer.HydraRenderer(str(scene_arg), WIDTH, HEIGHT)
node_system = hydra.get_node_system()
node_system.load_configuration(str(locate_render_cfg()))
node_system.init()
tree_nodes = node_system.get_node_tree()
executor = node_system.get_node_tree_executor()
rng = tree_nodes.add_node("rng_texture")
ray_gen = tree_nodes.add_node("node_render_ray_generation")
path_trace = tree_nodes.add_node("path_tracing")
accumulate = tree_nodes.add_node("accumulate")
rng_buffer = tree_nodes.add_node("rng_buffer")
present = tree_nodes.add_node("present_color")
tree_nodes.add_link(rng.get_output_socket("Random Number"),
                    ray_gen.get_input_socket("random seeds"))
tree_nodes.add_link(ray_gen.get_output_socket("Pixel Target"),
                    path_trace.get_input_socket("Pixel Target"))
tree_nodes.add_link(ray_gen.get_output_socket("Rays"),
                    path_trace.get_input_socket("Rays"))
tree_nodes.add_link(rng_buffer.get_output_socket("Random Number"),
                    path_trace.get_input_socket("Random Seeds"))
tree_nodes.add_link(path_trace.get_output_socket("Output"),
                    accumulate.get_input_socket("Texture"))
tree_nodes.add_link(accumulate.get_output_socket("Accumulated"),
                    present.get_input_socket("Color"))
executor.reset_allocator()
executor.prepare_tree(tree_nodes, present)
for (node, socket_name), value in {
    (ray_gen, "Aperture"): 0.0,
    (ray_gen, "Focus Distance"): 2.0,
    (ray_gen, "Scatter Rays"): False,
    (accumulate, "Max Samples"): SPP,
}.items():
    socket = node.get_input_socket(socket_name)
    executor.sync_node_from_external_storage(socket, core.to_meta_any(value))

t0 = time.time()
for _ in range(SPP):
    hydra.render(0.0)

tex = hydra.get_output_texture()
if not tex or len(tex) != WIDTH * HEIGHT * 4:
    print(f"[rsc] {tag}: BAD OUTPUT TEXTURE len={0 if not tex else len(tex)}")
    sys.exit(2)
img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
rgb = np.clip(img[:, :, :3], 0.0, 1.0)
mean = rgb.reshape(-1, 3).mean(axis=0)
render_s = time.time() - t0

from PIL import Image  # noqa: E402
Image.fromarray((np.flipud(rgb) * 255).astype(np.uint8)).save(
    OUT_DIR / f"{tag}.png")
print(f"[rsc] {tag}: mean = {mean[0]:.4f} {mean[1]:.4f} {mean[2]:.4f} "
      f"({SPP} spp, {render_s:.1f}s)")
hydra.stop()
if mean.mean() < 0.01:
    sys.exit(1)
