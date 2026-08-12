#!/usr/bin/env python3
"""Render the mirror-floor + emissive-box scene (Plan B), save a clear PNG.

Uses the SAME render graph as render_clouds.py (the visually-verified cloud
test): NO gamma_correction node (LPM already applies display gamma in LDR
mode — chaining gamma would double-gamma and wash the image out), LPM
Exposure=2.0.
"""
import sys, os
from pathlib import Path
import numpy as np

binary_dir = Path(r"C:\Users\Jerry\WorkSpace\Ruzino\Binaries\Release")
os.environ['PATH'] = str(binary_dir) + os.pathsep + os.environ.get('PATH', '')
os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', str(binary_dir))
mtlx = binary_dir / 'libraries'
if mtlx.exists():
    os.environ.setdefault('PXR_MTLX_STDLIB_SEARCH_PATHS', str(mtlx))
if hasattr(os, 'add_dll_directory'):
    try:
        os.add_dll_directory(str(binary_dir))
    except Exception:
        pass
sys.path.insert(0, str(binary_dir))
sys.path.insert(0, r"C:\Users\Jerry\WorkSpace\Ruzino\source\tests")
import nodes_core_py as core
import hd_RUZINO_py as renderer
from PIL import Image


def _build_render_graph_cloud(hydra, samples):
    """Render graph matching render_clouds.py: LPM only, no gamma node."""
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(binary_dir / "render_nodes.json"))
    node_system.init()
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture")
    ray_gen = tree.add_node("node_render_ray_generation")
    path_trace = tree.add_node("path_tracing")
    accumulate = tree.add_node("accumulate")
    rng_buffer = tree.add_node("rng_buffer")
    lpm = tree.add_node("lpm")
    present = tree.add_node("present_color")
    # NO gamma_correction node — LPM applies display gamma in LDR mode already.

    tree.add_link(rng.get_output_socket("Random Number"),
                  ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"),
                  path_trace.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"),
                  path_trace.get_input_socket("Rays"))
    tree.add_link(rng_buffer.get_output_socket("Random Number"),
                  path_trace.get_input_socket("Random Seeds"))
    tree.add_link(path_trace.get_output_socket("Output"),
                  accumulate.get_input_socket("Texture"))
    tree.add_link(accumulate.get_output_socket("Accumulated"),
                  lpm.get_input_socket("Input Color"))
    tree.add_link(lpm.get_output_socket("Output Color"),
                  present.get_input_socket("Color"))

    lpm.get_input_socket("Crosstalk").set_default_value([0.471, 0.49, 0.504])
    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    scalar_params = {
        (ray_gen, "Aperture"): 0.0, (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
        # emission=0.8 keeps the box's raw radiance < 1.0 (no source clamp).
        # Exposure 2.0 lifts midtones; HDR Max 2.0 shoulder rolls off the box
        # highlight gently instead of hard-clamping.
        (lpm, "HDR Max"): 2.0, (lpm, "LPM Exposure"): 2.0,
        (lpm, "Contrast"): 1.0, (lpm, "Shoulder"): 1.0,
        (lpm, "Shoulder Contrast"): 1.0, (lpm, "Soft Gap"): 0.0,
        (lpm, "Color Space"): 0, (lpm, "Display Mode"): 0,
        (lpm, "Display Max Luminance"): 1000.0,
        (lpm, "Display Min Luminance"): 0.0,
    }
    for (node, sn), val in scalar_params.items():
        socket = node.get_input_socket(sn)
        executor.sync_node_from_external_storage(socket, core.to_meta_any(val))


scene = r"C:\Users\Jerry\WorkSpace\Ruzino\source\tests\data\scenes\emissive_mirror_symmetry.usda"
W, H, SPP = 768, 432, 512
print(f"Rendering {Path(scene).name} ({W}x{H} @ {SPP} SPP)...", flush=True)
hydra = renderer.HydraRenderer(scene, W, H)
_build_render_graph_cloud(hydra, SPP)
for i in range(SPP):
    hydra.render()
    if (i + 1) % 128 == 0:
        print(f"  {i+1}/{SPP}...", flush=True)

tex = hydra.get_output_texture()
img = np.array(tex, dtype=np.float32).reshape(H, W, 4)
img = np.flipud(img)

out_dir = binary_dir / "test_output" / "emissive_validation"
out_dir.mkdir(parents=True, exist_ok=True)
rgb8 = np.clip(img[:, :, :3] * 255, 0, 255).astype(np.uint8)
Image.fromarray(rgb8).save(out_dir / "planB_mirror.png")
print(f"Saved: {out_dir / 'planB_mirror.png'}")
rgb = rgb8.astype(float) / 255.0
print(f"min={rgb.min():.3f} max={rgb.max():.3f} mean={rgb.mean():.3f}")
# Report clamp: how many pixels hit 255 (over-exposed / lost color)?
clamped = int((rgb8 >= 254).sum())
print(f"pixels clamped to ~255: {clamped} ({clamped/rgb8.size*100:.2f}%)")

# Measure: emissive box is brightest; threshold at 90th pct of upper half.
mid = H // 2
lum = rgb.mean(axis=2)
thr = float(np.percentile(lum[:mid], 90))
mask = lum > thr
upper = rgb[:mid][mask[:mid]]
lower = rgb[mid:][mask[mid:]]
print(f"threshold (90th pct upper) = {thr:.4f}")
print(f"Direct (upper)    px={len(upper):6d}  mean={float(upper.mean()) if len(upper) else 0:.4f}")
print(f"Reflected (lower) px={len(lower):6d}  mean={float(lower.mean()) if len(lower) else 0:.4f}")
if len(upper) > 10 and len(lower) > 10:
    print(f"Ratio reflected/direct = {float(lower.mean())/float(upper.mean()):.4f}")
