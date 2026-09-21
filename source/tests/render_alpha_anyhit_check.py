#!/usr/bin/env python3
"""Any-hit alpha validation: leaf-card close-up + a provably-invisible quad.

Scene:
  - ground plane (light grey)
  - quad A: 1.2 m, fully-transparent material (own 4x4 alpha=0 texture) at
    z=0.5, dead-center in front of the camera -> if the any-hit alpha test
    works, A is completely invisible (see-through, no shadow); if it is
    broken, A renders as an opaque dark rectangle and casts a hard shadow.
  - quad B: 1.2 m leaf-card (TreeGen atlas, tile 0 = ovate serrate) at z=0
    directly behind A -> seen THROUGH A: leaf shape with internal holes
    (sky visible through the cutout gaps), dappled shadow on the ground.
  - quad C: angled leaf-card low right, for shadow variety.
  - sun (DistantLight, angled) + bluish dome -> hard-ish dappled shadows.

Programmatic checks (plus the saved PNG for eyeballing):
  1. frame-center region (looking through A at B): greenish (G > R, G > B)
  2. ground strip under B/C: both bright and dark pixels (dappled shadow)
  3. whole image: not black, not blown out
  4. sky reference region top corners: blue-dominant

Run from Binaries/Release:
    python ../../source/tests/render_alpha_anyhit_check.py [tag]
Env: AHC_SPP (default 64), AHC_WIDTH/AHC_HEIGHT (default 960/540).
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
from PIL import Image  # noqa: E402
from pxr import Sdf, Usd, UsdGeom, UsdLux, UsdShade, Gf, Vt  # noqa: E402

TAG = sys.argv[1] if len(sys.argv) > 1 else "v1"
SPP = int(os.environ.get("AHC_SPP", "64"))
WIDTH = int(os.environ.get("AHC_WIDTH", "960"))
HEIGHT = int(os.environ.get("AHC_HEIGHT", "540"))

OUT_DIR = BIN / "test_output" / "alpha_anyhit"
OUT_DIR.mkdir(parents=True, exist_ok=True)
LEAF_ATLAS = ROOT / "source" / "Plugins" / "TreeGen" / "assets" / "leaf_atlas.png"

# A fully-transparent 4x4 RGBA texture (alpha = 0 everywhere).
GAP_PNG = OUT_DIR / "alpha_gap.png"
gap = np.zeros((4, 4, 4), dtype=np.uint8)
Image.fromarray(gap, "RGBA").save(GAP_PNG)

stage = Usd.Stage.CreateInMemory()
root = UsdGeom.Xform.Define(stage, "/World")

# ---- ground plane ----
gmesh = UsdGeom.Mesh.Define(stage, "/World/Ground")
gmesh.CreatePointsAttr([
    (-10, 0, -10), (10, 0, -10), (10, 0, 10), (-10, 0, 10)])
gmesh.CreateFaceVertexIndicesAttr([0, 1, 3, 1, 2, 3])
gmesh.CreateFaceVertexCountsAttr([3, 3])
gmesh.CreateNormalsAttr([(0, 1, 0)] * 4)
gmesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)


def quad(stage, stage_path, center, size, uv0, yaw=0.0, pitch=0.0):
    """Camera-facing quad (in the XY plane at given z-offset), rotated by
    yaw (around Y) / pitch (around X). uv0 = bottom-left UV, tile spans 0.49
    (same inset math as node_tree_generate.cpp)."""
    cx, cy, cz = center
    hx, hy = size * 0.5, size * 0.5
    pts = [(-hx, -hy, 0), (hx, -hy, 0), (hx, hy, 0), (-hx, hy, 0)]
    u0, v0 = uv0
    uvs = [(u0, v0), (u0 + 0.49, v0), (u0 + 0.49, v0 + 0.49), (u0, v0 + 0.49)]
    mesh = UsdGeom.Mesh.Define(stage, stage_path)
    mesh.CreatePointsAttr(pts)
    mesh.CreateFaceVertexIndicesAttr([0, 1, 3, 1, 2, 3])
    mesh.CreateFaceVertexCountsAttr([3, 3])
    mesh.CreateNormalsAttr([(0, 0, 1)] * 4)
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    # face-varying UVs
    fvi, fvu = [], []
    for tri in ([0, 1, 3], [1, 2, 3]):
        fvi.extend(tri)
        for c in tri:
            fvu.append(uvs[c])
    mesh.CreateFaceVertexIndicesAttr(fvi)
    mesh.CreateFaceVertexCountsAttr([3, 3])
    pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "UVMap", Sdf.ValueTypeNames.TexCoord2fArray,
        UsdGeom.Tokens.faceVarying)
    pv.Set(Vt.Vec2fArray([Gf.Vec2f(*u) for u in fvu]))
    xf = Gf.Matrix4d().SetTranslate(Gf.Vec3d(cx, cy, cz))
    if yaw:
        r = Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(0, 1, 0), yaw))
        xf = r * xf
    if pitch:
        r = Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), pitch))
        xf = r * xf
    UsdGeom.Xformable(mesh).AddTransformOp().Set(xf)
    return mesh


def make_material(mat_path, shader_name, tex_path, use_alpha, color=None):
    mat = UsdShade.Material.Define(stage, mat_path)
    sh = UsdShade.Shader.Define(stage, f"{mat_path}/{shader_name}")
    sh.CreateIdAttr("UsdPreviewSurface")
    sh.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.6)
    uvr = UsdShade.Shader.Define(stage, f"{mat_path}/UVReader")
    uvr.CreateIdAttr("UsdPrimvarReader_float2")
    uvr.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("UVMap")
    st = uvr.CreateOutput("result", Sdf.ValueTypeNames.Float2)

    tex = None
    if tex_path is not None:
        tex = UsdShade.Shader.Define(stage, f"{mat_path}/Tex")
        tex.CreateIdAttr("UsdUVTexture")
        tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
            Sdf.AssetPath(Path(tex_path).as_posix()))
        tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st)
        tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
        tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")

    if color is not None:
        sh.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(color)
    else:
        sh.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f). \
            ConnectToSource(tex.CreateOutput("rgb", Sdf.ValueTypeNames.Color3f))
    if use_alpha:
        sh.CreateInput("opacityMode", Sdf.ValueTypeNames.String).Set("mask")
        sh.CreateInput("opacityThreshold", Sdf.ValueTypeNames.Float).Set(0.5)
        sh.CreateInput("opacity", Sdf.ValueTypeNames.Float).ConnectToSource(
            tex.CreateOutput("a", Sdf.ValueTypeNames.Float))
    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(sh), "surface")
    return mat


gmat = make_material(
    "/World/Ground_mat", "Shader", None, False, color=(0.55, 0.55, 0.55))
UsdShade.MaterialBindingAPI.Apply(gmesh.GetPrim()).Bind(gmat)

# Quad A: provably fully transparent (own alpha=0 texture), dead center.
qa = quad(stage, "/World/QuadGap", (0.0, 1.1, 0.5), 1.2, (0.0, 0.0))
amat = make_material("/World/Gap_mat", "Shader", GAP_PNG, use_alpha=True)
UsdShade.MaterialBindingAPI.Apply(qa.GetPrim()).Bind(amat)

# Quad B: leaf atlas tile 0 (lower-left: ovate serrate), behind A.
# Constant UVs across the whole quad would show one texel — no: uv0 spans
# 0.49 so the quad shows the whole tile.
qb = quad(stage, "/World/QuadLeaf", (0.0, 1.1, 0.0), 1.2, (0.005, 0.505))
lmat = make_material(
    "/World/Leaf_mat", "Shader", LEAF_ATLAS, use_alpha=True)
UsdShade.MaterialBindingAPI.Apply(qb.GetPrim()).Bind(lmat)

# Quad C: angled leaf card, low right.
qc = quad(
    stage, "/World/QuadLeafC", (0.9, 0.55, 0.3), 0.7, (0.005, 0.505),
    yaw=25.0, pitch=15.0)
UsdShade.MaterialBindingAPI.Apply(qc.GetPrim()).Bind(lmat)

# ---- camera / lights (render_terrain_forest.py recipe) ----
cam = UsdGeom.Camera.Define(stage, "/World/Cam")
cam.CreateFocalLengthAttr().Set(24.0)
cam.GetClippingRangeAttr().Set((0.01, 1000.0))


def look_at(eye, target):
    eye, target = np.asarray(eye, np.float64), np.asarray(target, np.float64)
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


UsdGeom.Xformable(cam).AddTransformOp().Set(
    look_at((0.0, 1.05, 2.4), (0.0, 1.05, 0.0)))

dome = UsdLux.DomeLight.Define(stage, "/World/Sky")
dome.CreateIntensityAttr().Set(1.3)
dome.CreateColorAttr().Set(Gf.Vec3f(0.55, 0.68, 0.95))

sun_xf = Gf.Matrix4d()
sun_xf.SetIdentity()
sun_xf.SetRow(2, Gf.Vec4d(-0.35, -0.7, -0.45, 0.0))
sun = UsdLux.DistantLight.Define(stage, "/World/Sun")
sun.CreateIntensityAttr().Set(9.0)
UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)

scene = OUT_DIR / f"alpha_anyhit_{TAG}.usdc"
stage.Export(str(scene))
print(f"[ahc] scene: {scene}")

# ---- render (render_terrain_forest.py recipe) ----
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


hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
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
    sys.exit("[ahc] bad output texture")
img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
rgb = np.clip(img[:, :, :3], 0.0, 1.0)
hydra.stop()

out_png = OUT_DIR / f"alpha_anyhit_{TAG}.png"
Image.fromarray((np.flipud(rgb) * 255).astype(np.uint8)).save(out_png)
print(f"[ahc] wrote {out_png} ({SPP} spp, {time.time() - t0:.1f}s)")

# ---- programmatic checks ----
fails = []


def region(y0, y1, x0, x1):
    return rgb[y0:y1, x0:x1, :]


# 1) center (through transparent quad A at leaf B): greenish
c = region(HEIGHT // 2 - 60, HEIGHT // 2 + 60, WIDTH // 2 - 60, WIDTH // 2 + 60)
cm = c.reshape(-1, 3).mean(axis=0)
print(f"[ahc] center (through gap quad at leaf) mean RGB = {np.round(cm, 3)}")
if not (cm[1] > cm[0] * 1.05 and cm[1] > cm[2] * 1.05 and cm[1] > 0.05):
    fails.append(f"center not leaf-green: {cm}")

# 2) ground strip under the cards: dappled shadow (bright AND dark pixels)
gy = int(HEIGHT * 0.80)
g = region(gy, gy + 60, int(WIDTH * 0.25), int(WIDTH * 0.75)).reshape(-1, 3)
lum = g.mean(axis=1)
frac_dark = float((lum < 0.35).mean())
frac_bright = float((lum > 0.6).mean())
print(f"[ahc] ground strip: {frac_dark:.2f} dark / {frac_bright:.2f} bright")
if frac_dark < 0.02 or frac_bright < 0.02:
    fails.append(
        f"ground not dappled: dark={frac_dark:.3f} bright={frac_bright:.3f}")

# 3) global sanity
gm = rgb.reshape(-1, 3).mean(axis=0)
print(f"[ahc] image mean RGB = {np.round(gm, 3)}")
if gm.mean() < 0.03 or gm.mean() > 0.95:
    fails.append(f"image mean out of range: {gm}")

# 4) sky corner: blue-dominant
s = region(0, 60, 0, 200).reshape(-1, 3).mean(axis=0)
print(f"[ahc] sky corner mean RGB = {np.round(s, 3)}")
if not (s[2] > s[0]):
    fails.append(f"sky not blue-dominant: {s}")

if fails:
    print("[ahc] FAILED checks:")
    for f in fails:
        print(f"  - {f}")
    sys.exit(1)
print("[ahc] all checks passed")
