#!/usr/bin/env python3
"""
Render the gridbox simulation output with the Ruzino path tracer.

This is the render half of the build->simulate->render flow. Unlike the pytest
tests (which validate graph construction), this script drives an actual GPU
render via rz_render.exe, so it needs:

  * a machine with an OpenGL 4.5-capable GPU (the headless sandbox cannot
    provide one — Hydra rejects software GL with
    "HgiGL minimum OpenGL requirements not met"), and
  * the sim output USD from test_sim_gridbox.py at
    source/tests/data/output/gridbox_sim.usdc.

Run from Binaries/Release (so node-plugin DLLs resolve):

    python ../../source/tests/render_gridbox.py

It will:
  1. build a path-tracing render graph from the Python API and serialize it,
  2. prepare a render scene (sim geometry + camera + light),
  3. invoke rz_render.exe --json <graph> --usd <scene>.
"""
import os
import sys
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
# Register BIN as a DLL search dir so C++ LoadLibraryEx(LOAD_LIBRARY_SEARCH_*
# _DEFAULT_DIRS) finds node-plugin DLLs (accumulate/hd_RUZINO/...). Setting
# PATH alone is not enough once any AddDllDirectory() has been called.
os.add_dll_directory(str(BIN))

# Locate the render-node config the HydraRenderer's node system loads. Mirrors
# test_hydra_renderer._locate_config.
def _locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"
    if fallback.exists():
        return fallback
    sys.exit("render node config not found (render_nodes.json)")

# 2. Render scene: reference the sim output, add camera + light.
#
# IMPORTANT: test_sim_gridbox writes the box geometry to a *modifier/session*
# layer (gridbox_sim_modifiers.usdc), NOT into gridbox_sim.usdc itself — the
# .usdc only holds the empty /Grid Xform skeleton. So a plain composition of
# gridbox_sim.usdc has no points and the path tracer hits nothing (blank
# output). We compose the real geometry by layering the modifier file on top of
# the skeleton as a sublayer, then referencing that composed /Grid.
from pxr import Usd, UsdGeom, UsdLux, Sdf

sim_usd = HERE / "data" / "output" / "gridbox_sim.usdc"
sim_mod = HERE / "data" / "output" / "gridbox_sim_modifiers.usdc"
if not sim_usd.exists():
    sys.exit(f"sim output not found: {sim_usd} — run test_sim_gridbox first")

# Build a composed copy of the sim stage with the modifier geometry baked in,
# so /Grid actually has points/faceVertexCounts/faceVertexIndices.
composed = HERE / "data" / "output" / "gridbox_composed.usda"
if composed.exists():
    composed.unlink()
layer = Sdf.Layer.CreateNew(str(composed))
layer.subLayerPaths = [str(sim_mod.resolve()), str(sim_usd.resolve())]
layer.Save()

# Sanity-check: confirm the composed /Grid actually has geometry before we
# hand it to the (expensive) renderer.
_check = Usd.Stage.Open(str(composed))
_grid = UsdGeom.Mesh.Get(_check, "/Grid")
_npts = _grid.GetPointsAttr().Get()
if not _npts or len(_npts) == 0:
    sys.exit(f"composed /Grid has no points — sim modifier layer missing geometry: {sim_mod}")
print(f"[render] composed /Grid has {len(_npts)} points from {sim_mod.name}")

scene = BIN / "gridbox_render.usdc"
if scene.exists():
    scene.unlink()
stage = Usd.Stage.CreateNew(str(scene))
from pxr import Gf, Vt, UsdShade
import numpy as np

# Bake the composed sim geometry into a self-contained mesh, preserving the
# ANIMATION. The sim authored the box's `points` as 60 time samples (box
# translates X: 0.1 -> 6.0 over 60 frames). write_geometry_as_over_spec
# (node_write_usd) now writes CORRECT flat per-face normals (faceVarying) for
# every frame right at the source, so we just copy points + normals verbatim
# from the composed stage -- no per-frame recompute here. Hd_RUZINO_Mesh
# triangulates quad faces itself via HdMeshUtil::ComputeTriangleIndices.
_composed_stage = Usd.Stage.Open(str(composed))
_src = UsdGeom.Mesh(_composed_stage.GetPrimAtPath("/Grid"))
_src_pts_attr = _src.GetPointsAttr()
_src_nrm_attr = _src.GetNormalsAttr()
_frame_times = _src_pts_attr.GetTimeSamples()
if not _frame_times:
    sys.exit("composed /Grid points has no time samples -- run test_sim_gridbox")
print(f"[render] anim: {len(_frame_times)} time samples "
      f"(t={_frame_times[0]:.4f}..{_frame_times[-1]:.4f})")

# Topology is constant; sample it once at t0.
_src_fvc = _src.GetFaceVertexCountsAttr().Get(_frame_times[0])
_src_fvi = _src.GetFaceVertexIndicesAttr().Get(_frame_times[0])

mesh = UsdGeom.Mesh.Define(stage, "/Grid")
mesh.CreateFaceVertexCountsAttr().Set(_src_fvc)
mesh.CreateFaceVertexIndicesAttr().Set(_src_fvi)
mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
# Sanity-check: the source normals must be faceVarying (authored by
# write_geometry_as_over_spec). If they aren't, the renderer falls back to
# smooth shading and the red-NaN artifacts return -- fail loudly here.
_interp = _src.GetNormalsInterpolation()
if _interp != UsdGeom.Tokens.faceVarying:
    sys.exit(f"source normals interpolation is '{_interp}', expected "
             f"'faceVarying' -- rebuild the sim so write_usd authors flat normals")
mesh.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)
# Author points AND the source's flat normals at every animated time sample.
for _t in _frame_times:
    mesh.GetPointsAttr().Set(_src_pts_attr.Get(_t), _t)
    mesh.GetNormalsAttr().Set(_src_nrm_attr.Get(_t), _t)
# Stage time range so Hydra knows the full anim span.
stage.SetStartTimeCode(_frame_times[0])
stage.SetEndTimeCode(_frame_times[-1])
stage.SetTimeCodesPerSecond(
    _composed_stage.GetTimeCodesPerSecond() or 60.0)
grid = mesh.GetPrim()

# The path tracer returns black for geometry without a material (no BRDF to
# evaluate), so bind a simple UsdPreviewSurface. Mirrors test_scene.usda that
# test_hydra_renderer.py renders successfully.
mat = UsdShade.Material.Define(stage, "/BoxMaterial")
shader = UsdShade.Shader.Define(stage, "/BoxMaterial/Shader")
shader.CreateIdAttr("UsdPreviewSurface")
shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((0.2, 0.6, 0.9))
shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.5)
mat.CreateSurfaceOutput().ConnectToSource(
    UsdShade.ConnectableAPI(shader), "surface",
    UsdShade.AttributeType.Output)
UsdShade.MaterialBindingAPI.Apply(grid).Bind(mat)

cam = UsdGeom.Camera.Define(stage, "/Camera")
cam.GetFocalLengthAttr().Set(50.0)
# Use the same aperture as test_scene.usda (proven to render).
cam.GetHorizontalApertureAttr().Set(36.0)
cam.GetVerticalApertureAttr().Set(20.25)
cam.GetClippingRangeAttr().Set((0.1, 100.0))
# The box translates along X from 0.1 to 6.0, so frame the FULL travel path:
# aim the camera at the path midpoint (X~3) and pull back so both ends are
# in view. A 3/4 view shows three faces (+X, +Y, +Z) at once, which reads as
# an unambiguous 3D box rather than a flat square. We build a world-space
# LookAt (eye -> target) and set it as the camera transform. USD cameras look
# down their local -Z with +Y up, so the world transform's rows are
# (right, up, -forward, translation).
_eye = np.array([3.0, 4.5, 7.5])      # above the path midpoint, pulled back
_target = np.array([3.0, 0.0, 0.0])   # box path midpoint (X: 0.1 -> 6.0)
_up = np.array([0.0, 1.0, 0.0])
_fwd = _target - _eye
_fwd = _fwd / np.linalg.norm(_fwd)
_right = np.cross(_fwd, _up); _right = _right / np.linalg.norm(_right)
_up2 = np.cross(_right, _fwd)
m = Gf.Matrix4d()
m.SetIdentity()
m.SetRow(0, Gf.Vec4d(*_right.tolist(),  0.0))
m.SetRow(1, Gf.Vec4d(*_up2.tolist(),    0.0))
m.SetRow(2, Gf.Vec4d(*(-_fwd).tolist(), 0.0))
m.SetRow(3, Gf.Vec4d(*_eye.tolist(),    1.0))
UsdGeom.Xformable(cam).AddTransformOp().Set(m)

# DistantLight. Hd_RUZINO_Light (light.cpp:119,127) derives a distant light's
# direction from the Z row of its world transform and treats it as a
# directional vector (w=0). With NO transform, the Z row is the identity's
# (0,0,1), so the light shines along +Z -- from BEHIND the box (whose visible
# faces face the camera), leaving them unlit and the render black. We aim the
# light from above-right, matching the camera's viewing side, so the three
# visible faces are lit. The Z row of the light transform is the direction the
# light SHINES TOWARD; we want it to point roughly toward (-0.4, -0.5, -0.8)
# (down and toward the box from the camera side).
light_xf = Gf.Matrix4d()
light_xf.SetIdentity()
# Z row = direction light shines toward (down-left-front, hitting visible faces)
light_xf.SetRow(2, Gf.Vec4d(-0.4, -0.5, -0.8, 0.0))
UsdLux.DistantLight.Define(stage, "/Sun")
sun = UsdLux.DistantLight.Get(stage, "/Sun")
# Path tracer multiplies diffuseColor by intensity (light.cpp:247-249). An
# intensity sweep on a (0.8,0.2,0.2) surface showed 3.0 keeps the hue visible
# (mean RGB ~216,77,77) while 10.0+ clips to white. 3.0 also leaves headroom
# for the brighter face-on NdotL peaks.
sun.CreateIntensityAttr().Set(3.0)
UsdGeom.Xformable(sun).AddTransformOp().Set(light_xf)
stage.GetRootLayer().Save()
print(f"[render] scene: {scene.name} ({len(_frame_times)} animated frames)")

# 3. Render the animation IN-PROCESS with the HydraRenderer. We do NOT shell
# out to rz_render.exe: the stage is loaded once, the render graph is built
# once, and each frame is rendered by passing a different time code to
# HydraRenderer.render(time_code). Hydra then incrementally updates only the
# animated primvars (points/normals) rather than rebuilding the scene, and the
# render delegate / GPU pipeline persist across frames.
import hd_RUZINO_py as renderer

WIDTH, HEIGHT, SPP = 640, 480, 32
out_dir = BIN / "box_sequence"
out_dir.mkdir(exist_ok=True)
# Clear any previous sequence so a partial run can't mix frames.
for _old in out_dir.glob("frame_*.png"):
    _old.unlink()

hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)

# Build the same path-tracing render graph the subprocess build used, but
# in-process via the HydraRenderer's node system (matches
# test_hydra_renderer._build_render_graph).
import nodes_core_py as core
node_system = hydra.get_node_system()
node_system.load_configuration(str(_locate_render_cfg()))
node_system.init()
tree = node_system.get_node_tree()
executor = node_system.get_node_tree_executor()
rng = tree.add_node("rng_texture"); rng.ui_name = "RNG"
ray_gen = tree.add_node("node_render_ray_generation"); ray_gen.ui_name = "RayGen"
path_trace = tree.add_node("path_tracing"); path_trace.ui_name = "PathTracer"
accumulate = tree.add_node("accumulate"); accumulate.ui_name = "Accumulate"
rng_buffer = tree.add_node("rng_buffer"); rng_buffer.ui_name = "RNGBuffer"
present = tree.add_node("present_color"); present.ui_name = "Present"
tree.add_link(rng.get_output_socket("Random Number"), ray_gen.get_input_socket("random seeds"))
tree.add_link(ray_gen.get_output_socket("Pixel Target"), path_trace.get_input_socket("Pixel Target"))
tree.add_link(ray_gen.get_output_socket("Rays"), path_trace.get_input_socket("Rays"))
tree.add_link(rng_buffer.get_output_socket("Random Number"), path_trace.get_input_socket("Random Seeds"))
tree.add_link(path_trace.get_output_socket("Output"), accumulate.get_input_socket("Texture"))
tree.add_link(accumulate.get_output_socket("Accumulated"), present.get_input_socket("Color"))
executor.reset_allocator()
executor.prepare_tree(tree, present)
# Required parameters (from test_hydra_renderer._build_render_graph).
for (node, socket_name), value in {
    (ray_gen, "Aperture"): 0.0,
    (ray_gen, "Focus Distance"): 2.0,
    (ray_gen, "Scatter Rays"): False,
    (accumulate, "Max Samples"): SPP,
}.items():
    socket = node.get_input_socket(socket_name)
    executor.sync_node_from_external_storage(socket, core.to_meta_any(value))

from PIL import Image

print(f"[render] rendering {len(_frame_times)} frames "
      f"({WIDTH}x{HEIGHT}, {SPP} spp) in-process -> {out_dir.name}/")
for i, t in enumerate(_frame_times):
    for _ in range(SPP):           # accumulate SPP samples for this frame
        hydra.render(float(t))
    tex = hydra.get_output_texture()
    if not tex or len(tex) != WIDTH * HEIGHT * 4:
        print(f"  [warn] frame {i} (t={t:.4f}): bad texture len {len(tex) if tex else 0}")
        continue
    img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
    rgb = np.clip(img[:, :, :3], 0.0, 1.0)
    rgb = (rgb * 255).astype(np.uint8)
    rgb = np.flipud(rgb)            # USD/Hydra is bottom-row-first
    Image.fromarray(rgb).save(out_dir / f"frame_{i:04d}.png")
    print(f"  frame {i:3d}/{len(_frame_times)-1} (t={t:.4f}) saved "
          f"lit={float(rgb.max(axis=2).mean()/255)*100:4.1f}%")

hydra.stop()
print(f"[render] done: {len(list(out_dir.glob('frame_*.png')))} frames in {out_dir}")
