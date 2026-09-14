#!/usr/bin/env python3
"""
Dual-track G-buffer debug for the terrain render:

  Track A — rasterize node G-buffer (the pipeline under suspicion)
  Track B — standalone rt_gbuffer node: launches PRIMARY rays through the
            same TLAS/vertex-interpolation as path tracing and records
            world normal / position / texcoords / tangent directly.
            Geometry-only ground truth; rasterize must match it.

The scene here deliberately does NOT connect the normal map: both tracks
then expose pure GEOMETRY normals, so a diff isolates track disagreement
from material effects. Numerical diffs are printed per channel.

Run from anywhere: python source/Plugins/TerrainGen/tests/gbuffer_debug.py
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))
os.chdir(str(BIN))

import numpy as np  # noqa: E402
from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt  # noqa: E402

OUT_DIR = BIN / "test_output"
SRC_MOD = OUT_DIR / "terrain_preview_modifiers.usdc"
SCENE = OUT_DIR / "terrain_gbuffer_scene.usdc"
SCALE = 0.1
WIDTH, HEIGHT = 960, 540

if not SRC_MOD.exists():
    sys.exit("run render_terrain_preview.py first")

# ---- bake the debug scene (albedo only, NO normal-map connection) ----
src = Usd.Stage.Open(str(SRC_MOD))
terrain = UsdGeom.Mesh(src.GetPrimAtPath("/terrain"))
pts = np.array(terrain.GetPointsAttr().Get(), dtype=np.float32) * SCALE
fvc = terrain.GetFaceVertexCountsAttr().Get()
fvi = terrain.GetFaceVertexIndicesAttr().Get()
nrm = terrain.GetNormalsAttr().Get()
uv = terrain.GetPrim().GetAttribute("primvars:UVMap").Get()
print(f"[gbuffer] mesh: {len(pts)} verts, uv {'ok' if uv else 'MISSING'}")

if SCENE.exists():
    SCENE.unlink()
rstage = Usd.Stage.CreateNew(str(SCENE))
mesh = UsdGeom.Mesh.Define(rstage, "/Terrain")
mesh.CreatePointsAttr().Set(Vt.Vec3fArray.FromNumpy(pts))
mesh.CreateFaceVertexCountsAttr().Set(fvc)
mesh.CreateFaceVertexIndicesAttr().Set(fvi)
mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
if nrm:
    mesh.CreateNormalsAttr().Set(Vt.Vec3fArray.FromNumpy(
        np.array(nrm, dtype=np.float32) * SCALE))
    mesh.SetNormalsInterpolation(terrain.GetNormalsInterpolation())
if uv:
    pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "UVMap", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex)
    pv.Set(uv)

mat = UsdShade.Material.Define(rstage, "/Terrain/Terrain_mat")
pbr = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/PBRShader")
pbr.CreateIdAttr("UsdPreviewSurface")
st_reader = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/stReader")
st_reader.CreateIdAttr("UsdPrimvarReader_float2")
st_reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("UVMap")
tex = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/diffuseTexture")
tex.CreateIdAttr("UsdUVTexture")
tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath(
    (OUT_DIR / "terrain_albedo.png").resolve().as_posix()))
tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
    st_reader.ConnectableAPI(), "result")
tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")
pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(
    tex.ConnectableAPI(), "rgb")
pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.95)
# NOTE: no `normal` connection — geometry normals on both tracks.
mat.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)

cam = UsdGeom.Camera.Define(rstage, "/Camera")
cam.GetFocalLengthAttr().Set(24.0)
cam.GetHorizontalApertureAttr().Set(36.0)
cam.GetVerticalApertureAttr().Set(20.25)
cam.GetClippingRangeAttr().Set((0.01, 2000.0))
UsdGeom.Xformable(cam).AddTransformOp()

center = pts.mean(axis=0)
extent = float(np.linalg.norm(pts.max(axis=0) - pts.min(axis=0)))
eye_dir = np.array([0.7, 0.45, 0.8])
eye_dir /= np.linalg.norm(eye_dir)
eye = center + eye_dir * extent * 0.62

up = np.array([0.0, 1.0, 0.0])
fwd = -eye_dir
right = np.cross(fwd, up)
right /= np.linalg.norm(right)
up2 = np.cross(right, fwd)
m = Gf.Matrix4d()
m.SetIdentity()
m.SetRow(0, Gf.Vec4d(*right.tolist(), 0.0))
m.SetRow(1, Gf.Vec4d(*up2.tolist(), 0.0))
m.SetRow(2, Gf.Vec4d(*(-fwd).tolist(), 0.0))
m.SetRow(3, Gf.Vec4d(*eye.tolist(), 1.0))
UsdGeom.Xformable(cam).GetOrderedXformOps()[0].Set(m)

sun = UsdLux.DistantLight.Define(rstage, "/Sun")
sun.CreateIntensityAttr().Set(6.0)
sun_xf = Gf.Matrix4d()
sun_xf.SetIdentity()
sun_xf.SetRow(2, Gf.Vec4d(-0.3, -0.75, -0.5, 0.0))
UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)
rstage.GetRootLayer().Save()

# ---- dump helpers ----
import hd_RUZINO_py as renderer  # noqa: E402
import nodes_core_py as core  # noqa: E402
from PIL import Image  # noqa: E402

CFG = BIN / "render_nodes.json"


def run_rasterize(channel):
    hydra = renderer.HydraRenderer(str(SCENE), WIDTH, HEIGHT)
    ns = hydra.get_node_system()
    ns.load_configuration(str(CFG))
    ns.init()
    tree = ns.get_node_tree()
    executor = ns.get_node_tree_executor()
    raster = tree.add_node("rasterize")
    present = tree.add_node("present_color")
    tree.add_link(raster.get_output_socket(channel),
                  present.get_input_socket("Color"))
    executor.reset_allocator()
    executor.prepare_tree(tree, present)
    hydra.render(0.0)
    data = hydra.get_output_texture()
    hydra.stop()
    return np.asarray(data, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)


def run_rt_gbuffer(channel):
    # Standalone rt_gbuffer node: primary-ray ground truth, completely
    # independent of the path_tracing node.
    hydra = renderer.HydraRenderer(str(SCENE), WIDTH, HEIGHT)
    ns = hydra.get_node_system()
    ns.load_configuration(str(CFG))
    ns.init()
    tree = ns.get_node_tree()
    executor = ns.get_node_tree_executor()
    rng = tree.add_node("rng_texture")
    ray_gen = tree.add_node("node_render_ray_generation")
    gbuf = tree.add_node("rt_gbuffer")
    present = tree.add_node("present_color")
    tree.add_link(rng.get_output_socket("Random Number"),
                  ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"),
                  gbuf.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"),
                  gbuf.get_input_socket("Rays"))
    tree.add_link(gbuf.get_output_socket(channel),
                  present.get_input_socket("Color"))
    executor.reset_allocator()
    executor.prepare_tree(tree, present)
    hydra.render(0.0)
    data = hydra.get_output_texture()
    hydra.stop()
    return np.asarray(data, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)


def save(img_rgb, name, encode=lambda x: x):
    rgb = np.flipud((np.clip(encode(img_rgb), 0.0, 1.0) * 255)
                    .astype(np.uint8))
    out = OUT_DIR / name
    Image.fromarray(rgb).save(out)
    print(f"[gbuffer] saved {out.name} (mean {rgb.mean():.1f})")


results = {}

TRACKS = os.environ.get("GBUFFER_TRACKS", "raster+rt").split("+")

if "raster" in TRACKS:
    # Track A: rasterize. Normal stored raw world -> remap for viewing.
    results[("raster", "Normal")] = run_rasterize("Normal")
    results[("raster", "Texcoords")] = run_rasterize("Texcoords")
if "rt" in TRACKS:
    # Track B: raytraced primary G-buffer. Normal pre-encoded to [0,1].
    results[("rt", "Normal")] = run_rt_gbuffer("Normal")
    results[("rt", "Position")] = run_rt_gbuffer("Position")
    results[("rt", "Texcoords")] = run_rt_gbuffer("Texcoords")
    results[("rt", "Tangent")] = run_rt_gbuffer("Tangent")

save(results[("rt", "Normal")][:, :, :3], "gbuffer_rt_normal.png")
if ("raster", "Normal") in results:
    save(results[("raster", "Normal")][:, :, :3],
         "gbuffer_raster_normal.png",
         encode=lambda rgb: rgb * 0.5 + 0.5)

# Tangent frame dump (xyz + handedness w) from the dedicated output.
t = results[("rt", "Tangent")][:, :, :3]
t = t / (np.linalg.norm(t, axis=2, keepdims=True) + 1e-9)
tv = (np.linalg.norm(results[("rt", "Normal")][:, :, :3], axis=2) > 0.01)
tv = tv & (np.linalg.norm(t, axis=2) > 0.5)
if t[tv].size:
    print(f"[gbuffer] tangent mean: {t[tv].mean(axis=0).round(3)} "
          f"std: {t[tv].std(axis=0).round(3)}")
    w = results[("rt", "Tangent")][:, :, 3]
    print(f"[gbuffer] tangent w: unique {np.unique(w[tv])[:5]}")
    save(t * 0.5 + 0.5, "gbuffer_tangent.png")

# ---- consistency diff on valid pixels ----
valid = np.linalg.norm(results[("rt", "Position")][:, :, :3], axis=2) > 1e-3
print(f"[gbuffer] valid pixels: {valid.mean() * 100:.1f}%")

rt_n = results[("rt", "Normal")][:, :, :3] * 2.0 - 1.0
if ("raster", "Normal") in results:
    ra_n_raw = results[("raster", "Normal")][:, :, :3]
    for name, ra_n in (
            ("raw-as-is", ra_n_raw),
            ("remapped(2x-1)", ra_n_raw * 2.0 - 1.0),
            ("scaled(0.5+0.5)", ra_n_raw * 0.5 + 0.5)):
        diff = np.abs(rt_n - ra_n)[valid]
        print(f"[gbuffer] Normal diff raster[{name}] vs rt: "
              f"mean {diff.mean():.4f} max {diff.max():.4f}")

if ("raster", "Texcoords") in results:
    rt_uv = results[("rt", "Texcoords")][:, :, :2]
    ra_uv = results[("raster", "Texcoords")][:, :, :2]
    diff = np.abs(rt_uv - ra_uv)[valid]
    print(f"[gbuffer] Texcoords diff raster vs rt: "
          f"mean {diff.mean():.4f} max {diff.max():.4f}")

print("[gbuffer] done")
