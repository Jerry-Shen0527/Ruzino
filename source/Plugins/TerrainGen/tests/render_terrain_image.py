#!/usr/bin/env python3
"""
Render the eroded terrain preview to a real PNG via the in-process
HydraRenderer (path tracer), following render_treegen.py's proven recipe.

Reads the mesh written by render_terrain_preview.py from
test_output/terrain_preview_modifiers.usda, bakes it into a fresh scene at
1/10 scale (keeps the camera well inside the ray-tMax range that burned the
treegen renders), and saves:

    test_output/terrain_render.png         full view
    test_output/terrain_render_closeup.png drainage-channel close-up

Run from anywhere: python source/Plugins/TerrainGen/tests/render_terrain_image.py
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
SRC_MAIN = OUT_DIR / "terrain_preview.usdc"
SCENE = OUT_DIR / "terrain_render_scene.usdc"
SCALE = 0.1

# ---- 1. read the eroded terrain mesh ----
if not SRC_MOD.exists():
    sys.exit("run render_terrain_preview.py first (" + str(SRC_MOD) + ")")
src = Usd.Stage.Open(str(SRC_MOD))
terrain = UsdGeom.Mesh(src.GetPrimAtPath("/terrain"))
pts = np.array(terrain.GetPointsAttr().Get(), dtype=np.float32)
fvc = terrain.GetFaceVertexCountsAttr().Get()
fvi = terrain.GetFaceVertexIndicesAttr().Get()
nrm = terrain.GetNormalsAttr().Get()
dcolor = terrain.GetPrim().GetAttribute("primvars:displayColor").Get()
print(f"[terrain] source mesh: {len(pts)} verts, {len(fvc)} faces, "
      f"displayColor {'yes' if dcolor else 'MISSING'}")

pts = pts * SCALE

# ---- 2. bake the render scene ----
# Mesh + the SAME material network MaterialComponent::define_material authors
# (UsdPrimvarReader_float2("UVMap") -> UsdUVTexture(albedo) -> diffuseColor);
# authored directly here because the write_usd over spec needs the app's
# stage machinery to become a defined prim.
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

# UVs: the sidecar mesh carries primvars:UVMap (from texcoords_array).
uv = terrain.GetPrim().GetAttribute("primvars:UVMap").Get()
if uv:
    pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "UVMap", Sdf.ValueTypeNames.TexCoord2fArray,
        UsdGeom.Tokens.vertex)
    pv.Set(uv)
print(f"[terrain] UVMap primvar: {'ok' if uv else 'MISSING'}")

# Material: UsdPreviewSurface <- UsdUVTexture <- baked albedo PNG.
mat = UsdShade.Material.Define(rstage, "/Terrain/Terrain_mat")
pbr = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/PBRShader")
pbr.CreateIdAttr("UsdPreviewSurface")
st_reader = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/stReader")
st_reader.CreateIdAttr("UsdPrimvarReader_float2")
st_reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("UVMap")
tex = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/diffuseTexture")
tex.CreateIdAttr("UsdUVTexture")
ALBEDO = (OUT_DIR / "terrain_albedo.png").resolve()
tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
    Sdf.AssetPath(ALBEDO.as_posix()))
tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
    st_reader.ConnectableAPI(), "result")
tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")
pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(
    tex.ConnectableAPI(), "rgb")
pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.95)

# Tangent-space normal map: UsdUVTexture decodes [0,1]->[-1,1] via
# scale/bias (USD-standard wiring), then the preview-surface mtlx graph
# routes it through mx_normalmap with the vertex TBN.
import os as _os
NRM = Path(_os.environ.get(
    "TERRAIN_NORMAL_FILE",
    str((OUT_DIR / "terrain_normal.png").resolve())))
if NRM.exists():
    nrm_tex = UsdShade.Shader.Define(
        rstage, "/Terrain/Terrain_mat/normalTexture")
    nrm_tex.CreateIdAttr("UsdUVTexture")
    nrm_tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(NRM.as_posix()))
    nrm_tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
        st_reader.ConnectableAPI(), "result")
    nrm_tex.CreateInput("scale", Sdf.ValueTypeNames.Color4f).Set(
        (2.0, 2.0, 2.0, 1.0))
    nrm_tex.CreateInput("bias", Sdf.ValueTypeNames.Color4f).Set(
        (-1.0, -1.0, -1.0, 0.0))
    nrm_tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
    nrm_tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")
    pbr.CreateInput("normal", Sdf.ValueTypeNames.Vector3f).ConnectToSource(
        nrm_tex.ConnectableAPI(), "rgb")
    print("[terrain] normal map connected")
else:
    print("[terrain] normal map MISSING, shading without it")
mat.CreateSurfaceOutput().ConnectToSource(
    pbr.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)

center = pts.mean(axis=0)
extent = float(np.linalg.norm(pts.max(axis=0) - pts.min(axis=0)))
print(f"[terrain] scaled bbox center={center} extent={extent:.2f}")

cam = UsdGeom.Camera.Define(rstage, "/Camera")
cam.GetFocalLengthAttr().Set(24.0)
cam.GetHorizontalApertureAttr().Set(36.0)
cam.GetVerticalApertureAttr().Set(20.25)
cam.GetClippingRangeAttr().Set((0.01, 2000.0))
UsdGeom.Xformable(cam).AddTransformOp()


def look_at(eye, target):
    eye, target = (np.asarray(v, np.float64) for v in (eye, target))
    up = np.array([0.0, 1.0, 0.0])
    fwd = target - eye
    fwd = fwd / np.linalg.norm(fwd)
    right = np.cross(fwd, up)
    right = right / np.linalg.norm(right)
    up2 = np.cross(right, fwd)
    m = Gf.Matrix4d()
    m.SetIdentity()
    m.SetRow(0, Gf.Vec4d(*right.tolist(), 0.0))
    m.SetRow(1, Gf.Vec4d(*up2.tolist(), 0.0))
    m.SetRow(2, Gf.Vec4d(*(-fwd).tolist(), 0.0))
    m.SetRow(3, Gf.Vec4d(*eye.tolist(), 1.0))
    return m


sun = UsdLux.DistantLight.Define(rstage, "/Sun")
sun.CreateIntensityAttr().Set(3.0)
# Soft sun disc: the hard delta sun is what made snowfields read as
# crumpled foil (sunlit facet / black shadow alternating at mesh scale).
sun.CreateAngleAttr().Set(0.53)
sun_xf = Gf.Matrix4d()
sun_xf.SetIdentity()
# Steeper sun arriving from the +x/+z side so the valley walls that face
# the close-up camera are lit, not backlit.
sun_xf.SetRow(2, Gf.Vec4d(-0.45, -0.85, -0.3, 0.0))
UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)

fill = UsdLux.DistantLight.Define(rstage, "/Fill")
fill.CreateIntensityAttr().Set(1.5)
fill_xf = Gf.Matrix4d()
fill_xf.SetIdentity()
fill_xf.SetRow(2, Gf.Vec4d(0.6, -0.3, 0.55, 0.0))
UsdGeom.Xformable(fill).AddTransformOp().Set(fill_xf)

# Procedural sky dome (plain DomeLight + shader_path callable is the dome
# path the offline HydraRenderer wires; the HosekWilkieSky codeless schema
# is interactive-app-only so far). Sky ambient fills the shadows that made
# snowfields read black-and-foil under a pure black sky. The callable
# hardcodes its own sun-disk direction; inputs:* attrs are inert for it.
sky = rstage.DefinePrim("/Sky", "DomeLight")
sky.CreateAttribute(
    "shader_path", Sdf.ValueTypeNames.String).Set(
    "callables/eval_dome_light_procedural_sky.slang")
dome = UsdLux.DomeLight(sky)
dome.CreateIntensityAttr().Set(2.0)
rstage.GetRootLayer().Save()

# ---- 3. render in-process ----
import hd_RUZINO_py as renderer  # noqa: E402
import nodes_core_py as core  # noqa: E402
from PIL import Image  # noqa: E402

WIDTH = int(os.environ.get("TERRAIN_RENDER_W", "3840"))
HEIGHT = int(os.environ.get("TERRAIN_RENDER_H", "2160"))
SPP = int(os.environ.get("TERRAIN_RENDER_SPP", "96"))


def locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / \
        "render_nodes_save.json"
    if fallback.exists():
        return fallback
    sys.exit("render node config not found")


def make_renderer():
    hydra = renderer.HydraRenderer(str(SCENE), WIDTH, HEIGHT)
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(locate_render_cfg()))
    node_system.init()
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()
    rng = tree.add_node("rng_texture")
    ray_gen = tree.add_node("node_render_ray_generation")
    path_trace = tree.add_node("path_tracing")
    accumulate = tree.add_node("accumulate")
    rng_buffer = tree.add_node("rng_buffer")
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
                  present.get_input_socket("Color"))
    executor.reset_allocator()
    executor.prepare_tree(tree, present)
    for (node, socket_name), value in {
        (ray_gen, "Aperture"): 0.0,
        (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): SPP,
    }.items():
        socket = node.get_input_socket(socket_name)
        executor.sync_node_from_external_storage(
            socket, core.to_meta_any(value))
    return hydra


def render_view(out_name, eye_dir, target, fit_dist):
    xform = UsdGeom.Xformable(cam)
    ops = xform.GetOrderedXformOps()
    eye_dir = np.asarray(eye_dir, np.float64)
    eye_dir = eye_dir / np.linalg.norm(eye_dir)
    eye = center + eye_dir * fit_dist
    ops[0].Set(look_at(eye, np.asarray(target, np.float64)))
    rstage.GetRootLayer().Save()
    hydra = make_renderer()
    for _ in range(SPP):
        hydra.render(0.0)
    tex = hydra.get_output_texture()
    if not tex or len(tex) != WIDTH * HEIGHT * 4:
        print(f"[terrain] bad texture for {out_name}")
        hydra.stop()
        return
    img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
    rgb = np.clip(img[:, :, :3], 0.0, 1.0)
    rgb = (rgb * 255).astype(np.uint8)
    rgb = np.flipud(rgb)
    out = OUT_DIR / out_name
    Image.fromarray(rgb).save(out)
    mean = float(rgb.mean())
    print(f"[terrain] saved {out} (mean {mean:.1f})")
    hydra.stop()


# Full view: eye from the southeast, slightly above the peaks.
render_view("terrain_render.png", (0.7, 0.45, 0.8),
            center + np.array([0.0, 0.02, 0.0]) * extent, extent * 0.62)
# Close-up: low angle into the drainage valleys.
render_view("terrain_render_closeup.png", (0.85, 0.18, 0.45),
            center + np.array([-0.1, -0.06, 0.0]) * extent, extent * 0.30)

print("[terrain] done")
