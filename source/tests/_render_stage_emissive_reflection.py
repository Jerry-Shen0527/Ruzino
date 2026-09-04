#!/usr/bin/env python3
"""Render the user's default scene (Assets/stage.usdc) with the offline
hd_RUZINO path tracer, reproducing what Ruzino.exe shows in its viewport.

Scene: mesh_1 = emissive unit cube (material_1 emission=1.0), mesh_0 = 20x20
floor (material_0 metalness=1.0 specular_roughness=0.01), dome intensity=0.

The app folds the modifier sidecar into the session layer, so we compose
stage.usdc + stage_modifiers.usdc and flatten to a temp usda before rendering
(HydraRenderer does a plain UsdStage::Open and would otherwise miss the
modifier opinions). The render graph mirrors the viewport's saved graph in
Assets/Hd_RUZINO_RendererPlugin/render_nodes_save.json (same nodes; only the
accumulate cap is raised so the diagnostic image is converged instead of the
viewport's 16-spp average).
"""
import sys, os
from pathlib import Path
import numpy as np

binary_dir = Path(r"C:\Users\Jerry\WorkSpace\Ruzino\Binaries\Release")
project_root = r"C:\Users\Jerry\WorkSpace\Ruzino"
# The app runs with cwd = Binaries/Release; the shader compiler and the
# MaterialX generator resolve several paths (generated_shaders/, libraries/,
# usd/hd_RUZINO/resources) relative to it, so match that.
os.chdir(binary_dir)
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
sys.path.insert(0, os.path.join(project_root, "source", "tests"))

from pxr import Usd, Sdf

# --- compose stage + modifier sidecar, flatten (mirrors Stage::load_modifier_layer)
stage_path = os.path.join(project_root, "Assets", "stage.usdc")
stem, ext = os.path.splitext(os.path.basename(stage_path))
mod_path = os.path.join(os.path.dirname(stage_path), stem + "_modifiers" + ext)

session = Sdf.Layer.CreateAnonymous("session")
stage = Usd.Stage.Open(Sdf.Layer.FindOrOpen(stage_path), session)
if os.path.exists(mod_path):
    file_layer = Sdf.Layer.FindOrOpen(mod_path)
    session.ImportFromString(file_layer.ExportToString())
    print(f"Folded modifier sidecar: {mod_path}", flush=True)
else:
    print("No modifier sidecar found", flush=True)

out_dir = binary_dir / "test_output" / "emissive_reflection"
out_dir.mkdir(parents=True, exist_ok=True)
flat_path = out_dir / "stage_flat.usda"
stage.Export(str(flat_path), addSourceFileComment=False)
print(f"Flattened composed stage -> {flat_path}", flush=True)

import nodes_core_py as core
import hd_RUZINO_py as renderer
from PIL import Image

W, H, SPP = 1280, 720, int(os.environ.get("RENDER_SPP", "512"))
tag = os.environ.get("RENDER_TAG", "")
print(f"Rendering {flat_path.name} ({W}x{H} @ {SPP} spp){' [' + tag + ']' if tag else ''}...", flush=True)
hydra = renderer.HydraRenderer(str(flat_path), W, H)

# --- render graph: identical topology to the viewport's saved graph
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
    (accumulate, "Max Samples"): SPP,
    # LPM params as authored in the user's viewport save
    # (Assets/Hd_RUZINO_RendererPlugin/render_nodes_save.json)
    (lpm, "HDR Max"): 2.0, (lpm, "LPM Exposure"): 0.0,
    (lpm, "Contrast"): 1.0, (lpm, "Shoulder"): 1.0,
    (lpm, "Shoulder Contrast"): 1.0, (lpm, "Soft Gap"): 0.0,
    (lpm, "Color Space"): 0, (lpm, "Display Mode"): 0,
    (lpm, "Display Max Luminance"): 1000.0,
    (lpm, "Display Min Luminance"): 0.0,
}
for (node, sn), val in scalar_params.items():
    socket = node.get_input_socket(sn)
    executor.sync_node_from_external_storage(socket, core.to_meta_any(val))

for i in range(SPP):
    hydra.render()
    if (i + 1) % 64 == 0:
        print(f"  {i+1}/{SPP} spp...", flush=True)

tex = hydra.get_output_texture()
img = np.array(tex, dtype=np.float32).reshape(H, W, 4)
img = np.flipud(img)

png_path = out_dir / ("stage_render%s.png" % ("_" + tag if tag else ""))
rgb8 = np.clip(img[:, :, :3] * 255, 0, 255).astype(np.uint8)
Image.fromarray(rgb8).save(png_path)
print(f"Saved: {png_path}", flush=True)
rgb = rgb8.astype(float) / 255.0
print(f"min={rgb.min():.3f} max={rgb.max():.3f} mean={rgb.mean():.3f}")
