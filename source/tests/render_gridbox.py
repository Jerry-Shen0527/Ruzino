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

# 1. Build + serialize the path-tracing render graph from Python.
from ruzino_render_graph import RuzinoRenderGraph

g = RuzinoRenderGraph("GridBoxRender")
g.loadConfiguration(str(BIN / "render_nodes.json"))
# Build the render graph to match the proven-good test_hydra_renderer.py
# _build_render_graph: rng_texture + rng_buffer feed the path tracer's random
# seeds, and ray_gen feeds both Pixel Target and Rays. Without rng_buffer /
# the Pixel Target link, the path tracer has no random numbers and produces
# a black image.
rng = g.createNode("rng_texture", name="RNG")
ray_gen = g.createNode("node_render_ray_generation", name="RayGen")
path_trace = g.createNode("path_tracing", name="PathTracer")
accumulate = g.createNode("accumulate", name="Accumulate")
rng_buffer = g.createNode("rng_buffer", name="RNGBuffer")
present = g.createNode("present_color", name="Present")
g.addEdge(rng, "Random Number", ray_gen, "random seeds")
g.addEdge(ray_gen, "Pixel Target", path_trace, "Pixel Target")
g.addEdge(ray_gen, "Rays", path_trace, "Rays")
g.addEdge(rng_buffer, "Random Number", path_trace, "Random Seeds")
g.addEdge(path_trace, "Output", accumulate, "Texture")
g.addEdge(accumulate, "Accumulated", present, "Color")
g.markOutput(present, "Color")
# Required parameters (from test_hydra_renderer._build_render_graph).
g.setSocketDefaults({
    (ray_gen, "Aperture"): 0.0,
    (ray_gen, "Focus Distance"): 2.0,
    (ray_gen, "Scatter Rays"): False,
    (accumulate, "Max Samples"): 8,
})
graph_json = BIN / "gridbox_render_graph.json"
graph_json.write_text(g.serialize(), encoding="utf-8")
print(f"[render] graph: {len(g.nodes)} nodes, {len(g.links)} links")

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

# Bake the composed sim geometry into a self-contained mesh. Hd_RUZINO_Mesh
# triangulates quad faces itself via HdMeshUtil::ComputeTriangleIndices, so we
# copy the topology verbatim (no manual fan-triangulation). Sampling frame 0
# (rz_render renders time=0).
_composed_stage = Usd.Stage.Open(str(composed))
_src = UsdGeom.Mesh(_composed_stage.GetPrimAtPath("/Grid"))
_t0 = Usd.TimeCode(0.0)
_src_pts = _src.GetPointsAttr().Get(_t0)
_src_fvc = _src.GetFaceVertexCountsAttr().Get(_t0)
_src_fvi = _src.GetFaceVertexIndicesAttr().Get(_t0)

# Author FLAT per-face normals (faceVarying interpolation). The source grid is
# a faceted box (48 quad faces, 27 shared verts). Without authored normals
# Hd_RUZINO_Mesh computes SMOOTH normals by averaging adjacent face normals at
# each shared vertex, which blends to a near-tangential direction along the
# sharp edges. Those blended grazing normals then drive the MaterialX BSDF at
# grazing angles and sporadically evaluate to NaN/Inf, which the path tracer
# flags with its red debug color (path_tracing.slang, the throughput-isValid
# check). Giving every vertex of a face that face's own geometric normal --
# so normalW == faceNormalW everywhere -- removes the blended grazing normals
# at the edges and is the physically correct shading for a hard-edged box.
_flat_nrm = []
_idx = 0
for _count in _src_fvc:
    _fv = list(_src_fvi[_idx:_idx + _count])
    _idx += _count
    _p0 = Gf.Vec3f(_src_pts[_fv[0]])
    _p1 = Gf.Vec3f(_src_pts[_fv[1]])
    _p2 = Gf.Vec3f(_src_pts[_fv[2]])
    _n = (_p1 - _p0).GetCross(_p2 - _p0)
    _n.Normalize()
    _flat_nrm.extend([_n] * _count)  # one normal per face-vertex

mesh = UsdGeom.Mesh.Define(stage, "/Grid")
mesh.CreatePointsAttr().Set(_src_pts)
mesh.CreateFaceVertexCountsAttr().Set(_src_fvc)
mesh.CreateFaceVertexIndicesAttr().Set(_src_fvi)
mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
_nrm_attr = mesh.CreateNormalsAttr()
_nrm_attr.Set(_flat_nrm)
mesh.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)
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
# rz_render renders time=0 (frame 0), where the box sits near the origin
# (center ~0.1,0,0). A 3/4 view shows three faces (+X, +Y, +Z) at once, which
# reads as an unambiguous 3D box rather than a flat square. We build a
# world-space LookAt (eye -> target) and set it as the camera transform.
# USD cameras look down their local -Z with +Y up, so the world transform's
# rows are (right, up, -forward, translation).
import numpy as _np
_eye = _np.array([3.0, 2.5, 3.5])
_target = _np.array([0.0, 0.0, 0.0])
_up = _np.array([0.0, 1.0, 0.0])
_fwd = _target - _eye
_fwd = _fwd / _np.linalg.norm(_fwd)
_right = _np.cross(_fwd, _up); _right = _right / _np.linalg.norm(_right)
_up2 = _np.cross(_right, _fwd)
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
print(f"[render] scene: {scene.name}")

# 3. Invoke the renderer.
out_png = BIN / "gridbox_render.png"
cmd = [str(BIN / "rz_render.exe"),
       "-u", str(scene), "-j", str(graph_json), "-o", str(out_png),
       "-w", "800", "-h", "600", "-s", "32", "-c", "/Camera"]
print("[render] running rz_render.exe")
r = subprocess.run(cmd, cwd=str(BIN), capture_output=True, text=True)
print(r.stdout[-1500:])
if r.returncode != 0:
    print("STDERR:", r.stderr[-1500:])
print(f"[render] exit {r.returncode}, {out_png.name} exists: {out_png.exists()}")
