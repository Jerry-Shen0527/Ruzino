#!/usr/bin/env python3
"""
Generate a TreeGen tree headlessly and render it with the in-process
HydraRenderer (path tracer), following the render_gridbox.py pattern.

Run from Binaries/Release:
    python ../../source/tests/render_treegen.py [tag]

Saves test_output/treegen_<tag>.png (full view) and
treegen_<tag>_closeup.png (foliage close-up).
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))

TAG = sys.argv[1] if len(sys.argv) > 1 else "tree"

# ---- 1. Generate the tree via the node graph (headless, like the pytest) ----
import stage_py
from ruzino_graph import RuzinoGraph

OUT_DIR = BIN / "test_output"
OUT_DIR.mkdir(exist_ok=True)
LEAF_ATLAS = os.environ.get(
    "TREEGEN_LEAF_ATLAS",
    str((ROOT / "source" / "Plugins" / "TreeGen" / "assets" /
         "leaf_atlas.png").resolve()))
# Species tile index in the 2x2 atlas: 0 ovate-serrate, 1 lanceolate,
# 2 palmate(maple), 3 obovate
TREEGEN_SPECIES = int(os.environ.get("TREEGEN_SPECIES", "0"))
# Leaf shrink + densify: baseline 0.28/14 produced leaves ~7.5% of tree
# height (real broadleaf: 1.5-3%). 0.12/28 lands ~3% with double density.
TREEGEN_LEAF_SIZE = float(os.environ.get("TREEGEN_LEAF_SIZE", "0.15"))
TREEGEN_LPI = int(os.environ.get("TREEGEN_LPI", "40"))


tree_usd = OUT_DIR / f"treegen_{TAG}.usdc"

g = RuzinoGraph("TreeGenRender")
g.loadConfiguration(str(BIN / "geometry_nodes.json"))
g.loadConfiguration(str(BIN / "Plugins" / "TreeGen_geometry_nodes.json"))

NODE_MATERIAL = os.environ.get("TREEGEN_NODE_MATERIAL") == "1"

tree_gen = g.createNode("tree_generate", name="tree")
to_mesh = g.createNode("tree_to_mesh", name="mesh_converter")
write_branches = g.createNode("write_usd", name="writer_branches")
write_leaves = g.createNode("write_usd", name="writer_leaves")

g.addEdge(tree_gen, "Tree Branches", to_mesh, "Tree Branches")

if NODE_MATERIAL:
    # Pure node-graph material flow: the atlas travels as an in-graph
    # texture object (storage deferred to write time), the bark is a flat
    # color material. No script-side material authoring.
    leaf_tex = g.createNode("load_texture_2d", name="leaf_atlas_texture")
    leaf_create = g.createNode("create_material", name="leaf_material")
    leaf_apply = g.createNode("set_material", name="apply_leaf_material")
    bark_create = g.createNode("create_material", name="bark_material")
    bark_apply = g.createNode("set_material", name="apply_bark_material")
    g.addEdge(leaf_tex, "Texture", leaf_create, "Texture")
    g.addEdge(leaf_create, "Material", leaf_apply, "Material")
    g.addEdge(tree_gen, "Leaves", leaf_apply, "Geometry")
    g.addEdge(leaf_apply, "Geometry", to_mesh, "Leaves")
    g.addEdge(to_mesh, "Branch Mesh", bark_apply, "Geometry")
    g.addEdge(bark_create, "Material", bark_apply, "Material")
    g.addEdge(bark_apply, "Geometry", write_branches, "Geometry")
else:
    g.addEdge(tree_gen, "Leaves", to_mesh, "Leaves")
    g.addEdge(to_mesh, "Branch Mesh", write_branches, "Geometry")
g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")

inputs = {
    (tree_gen, "Growth Years"): 8,
    (tree_gen, "Generate Leaves"): True,
    (tree_gen, "Leaf Size"): TREEGEN_LEAF_SIZE,
    (tree_gen, "Leaves Per Internode"): TREEGEN_LPI,
    (write_branches, "Sub Path"): "branches",
    (write_leaves, "Sub Path"): "leaves",
}
if NODE_MATERIAL:
    # Pure node-graph materials: leaf atlas as an in-graph texture object
    # (materialized to disk at write time), bark as a flat PBR color.
    inputs[(leaf_tex, "Path")] = LEAF_ATLAS
    inputs[(leaf_create, "Alpha Cutout")] = True
    inputs[(leaf_create, "Opacity Threshold")] = 0.5
    inputs[(leaf_create, "Roughness")] = 0.55
    inputs[(leaf_create, "Wrap Mode")] = "clamp"
    inputs[(bark_create, "Base Color")] = (0.42, 0.30, 0.20)
    inputs[(bark_create, "Roughness")] = 0.8
    inputs[(bark_create, "Wrap Mode")] = "clamp"
stage = stage_py.Stage(str(tree_usd))
geom_payload = stage_py.create_payload_from_stage(stage, "/treegen")
g.setGlobalParams(geom_payload)
g.prepare_and_execute(inputs, required_node=write_branches)
g.prepare_and_execute(inputs, required_node=write_leaves)
stage.save()
print(f"[treegen] wrote {tree_usd}")

# ---- 2. Compose + bake into a render scene ----
from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt
import numpy as np

tree_mod = tree_usd.with_suffix("").with_suffix("")  # strip nothing
tree_mod = Path(str(tree_usd).replace(".usdc", "_modifiers.usdc"))
if not tree_mod.exists():
    sys.exit(f"modifier layer missing: {tree_mod}")

composed = OUT_DIR / f"treegen_{TAG}_composed.usda"
if composed.exists():
    composed.unlink()
layer = Sdf.Layer.CreateNew(str(composed))
layer.subLayerPaths = [str(tree_mod.resolve()), str(tree_usd.resolve())]
layer.Save()

_check = Usd.Stage.Open(str(composed))
src_branches = UsdGeom.Mesh.Get(_check, "/treegen/branches")
src_leaves = UsdGeom.Mesh.Get(_check, "/treegen/leaves")
if not src_branches:
    sys.exit("/treegen/branches not found in composed stage")


def bake(stage, src_mesh, prim_path, color, roughness, leaf_cards=False):
    pts = src_mesh.GetPointsAttr().Get()
    fvc = src_mesh.GetFaceVertexCountsAttr().Get()
    fvi = src_mesh.GetFaceVertexIndicesAttr().Get()
    if not pts:
        print(f"[treegen] WARN: {prim_path} has no points")
        return None
    mesh = UsdGeom.Mesh.Define(stage, prim_path)
    mesh.CreatePointsAttr().Set(pts)
    mesh.CreateFaceVertexCountsAttr().Set(fvc)
    mesh.CreateFaceVertexIndicesAttr().Set(fvi)
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    src_nrm = src_mesh.GetNormalsAttr().Get()
    if src_nrm:
        mesh.CreateNormalsAttr().Set(src_nrm)
        mesh.SetNormalsInterpolation(src_mesh.GetNormalsInterpolation())
    if leaf_cards:
        _leaf_uv_material(stage, prim_path, mesh, fvi)
        return np.array(pts, dtype=np.float32)
    mat = UsdShade.Material.Define(stage, prim_path + "_mat")
    shader = UsdShade.Shader.Define(stage, prim_path + "_mat/Shader")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(color)
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(roughness)
    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(
        mesh.GetPrim()).Bind(mat)
    pts_np = np.array(pts, dtype=np.float32)
    print(f"[treegen] {prim_path}: {len(pts)} verts, {len(fvc)} faces")
    return pts_np


def _leaf_uv_material(stage, prim_path, mesh, fvi):
    """Face-varying UVs into the leaf atlas + textured alpha-cutout material.

    Leaf quads are 4 verts (tip, right, base, left) x 4 tri faces; pick the
    species tile per leaf so one atlas serves species/variation studies.
    """
    n_faces_per_leaf = 4
    n_leaves = len(fvi) // (n_faces_per_leaf * 3)
    rng = np.random.default_rng(42)
    tiles = rng.integers(0, 4, n_leaves)  # per-leaf tile pick (uniform mix)
    tiles[:] = TREEGEN_SPECIES  # single-species tree for now

    st = np.zeros((len(fvi), 2), dtype=np.float32)
    corner_uv = np.array([
        [0.5, 1.0],  # tip
        [1.0, 0.5],  # right
        [0.5, 0.0],  # base
        [0.0, 0.5],  # left
    ], dtype=np.float32)
    for leaf in range(n_leaves):
        t = int(tiles[leaf])
        tx, ty = t % 2, 1 - t // 2  # tiles 0,1 = bottom UV row
        u0, v0 = tx * 0.5 + 0.005, ty * 0.5 + 0.005
        uv = np.stack([u0 + corner_uv[:, 0] * 0.49,
                       v0 + corner_uv[:, 1] * 0.49], axis=1)
        base = leaf * n_faces_per_leaf * 3
        for f in range(n_faces_per_leaf):
            for c in range(3):
                vi = fvi[base + f * 3 + c]
                st[base + f * 3 + c] = uv[vi % 4]
    pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
    pv.Set([Gf.Vec2f(float(p[0]), float(p[1])) for p in st])

    mat = UsdShade.Material.Define(stage, prim_path + "_mat")
    surf = UsdShade.Shader.Define(stage, prim_path + "_mat/PreviewSurface")
    surf.CreateIdAttr("UsdPreviewSurface")
    surf.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.55)
    surf.CreateInput("opacityMode", Sdf.ValueTypeNames.String).Set("mask")
    surf.CreateInput("opacityThreshold", Sdf.ValueTypeNames.Float).Set(0.5)
    uvr = UsdShade.Shader.Define(stage, prim_path + "_mat/UVReader")
    uvr.CreateIdAttr("UsdPrimvarReader_float2")
    uvr.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
    uv_out = uvr.CreateOutput("result", Sdf.ValueTypeNames.Float2)

    def add_tex(name):
        tex = UsdShade.Shader.Define(stage, f"{prim_path}_mat/{name}")
        tex.CreateIdAttr("UsdUVTexture")
        tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
            Path(LEAF_ATLAS).as_posix())
        st_in = tex.CreateInput("st", Sdf.ValueTypeNames.Float2)
        st_in.ConnectToSource(uv_out)
        return tex

    tex_diff = add_tex("TexDiffuse")
    surf.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f). \
        ConnectToSource(tex_diff.CreateOutput("rgb",
                                              Sdf.ValueTypeNames.Color3f))
    tex_op = add_tex("TexOpacity")
    surf.CreateInput("opacity", Sdf.ValueTypeNames.Float).ConnectToSource(
        tex_op.CreateOutput("a", Sdf.ValueTypeNames.Float))

    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(surf), "surface")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)
    print(f"[treegen] {prim_path}: textured leaf cards "
          f"({n_leaves} leaves, species tile {TREEGEN_SPECIES})")


if NODE_MATERIAL:
    # Passthrough: render the composed stage as written by the C++ nodes.
    # Leaves keep their UVMap + bound material; only dressing (ground,
    # lights, camera below) is added to the composed root layer.
    scene = composed
    rstage = _check

    def _passthrough_points(path):
        m = UsdGeom.Mesh.Get(rstage, path)
        return np.array(m.GetPointsAttr().Get(), dtype=np.float32)

    bp = _passthrough_points("/treegen/branches") \
        if os.environ.get("TREEGEN_PART", "all") not in ("leaves",) else None
    lp = _passthrough_points("/treegen/leaves") \
        if os.environ.get("TREEGEN_PART", "all") not in ("branches",) else None
    print("[treegen] NODE_MATERIAL passthrough: rendering C++-authored stage")
else:
    scene = OUT_DIR / f"treegen_{TAG}_scene.usdc"
    if scene.exists():
        scene.unlink()
    rstage = Usd.Stage.CreateNew(str(scene))

    bp = bake(rstage, src_branches, "/Tree/Branches", (0.42, 0.30, 0.20), 0.8) \
        if os.environ.get("TREEGEN_PART", "all") not in ("leaves",) else None
    lp = bake(rstage, src_leaves, "/Tree/Leaves", (0.22, 0.55, 0.16), 0.6,
              leaf_cards=True) \
        if os.environ.get("TREEGEN_PART", "all") not in ("branches",) else None

# Ground plane so the tree reads against something
all_pts_list = [p for p in (bp, lp) if p is not None]
if not all_pts_list:
    sys.exit("no geometry generated")
all_pts = np.vstack(all_pts_list)
mn, mx = all_pts.min(axis=0), all_pts.max(axis=0)
center = (mn + mx) * 0.5
size = float(np.linalg.norm(mx - mn))
print(f"[treegen] bbox min={mn} max={mx} size={size:.2f}")

ground = UsdGeom.Mesh.Define(rstage, "/Ground")
spread = size * 2.0
ground.CreatePointsAttr().Set(
    [Gf.Vec3f(-spread, 0, -spread), Gf.Vec3f(spread, 0, -spread),
     Gf.Vec3f(spread, 0, spread), Gf.Vec3f(-spread, 0, spread)])
ground.CreateFaceVertexCountsAttr().Set([4])
ground.CreateFaceVertexIndicesAttr().Set([0, 1, 2, 3])
ground.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
gmat = UsdShade.Material.Define(rstage, "/Ground/Ground_mat")
gshader = UsdShade.Shader.Define(rstage, "/Ground/Ground_mat/Shader")
gshader.CreateIdAttr("UsdPreviewSurface")
gshader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
    (0.35, 0.38, 0.3))
gshader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.9)
gmat.CreateSurfaceOutput().ConnectToSource(
    UsdShade.ConnectableAPI(gshader), "surface",
    UsdShade.AttributeType.Output)
UsdShade.MaterialBindingAPI.Apply(ground.GetPrim()).Bind(gmat)

cam = UsdGeom.Camera.Define(rstage, "/Camera")
# 24mm: wide enough to frame the whole tree from ~13 units — the path
# tracer renders black when the camera sits 20+ units out (ray tMax limit),
# so we stay close and widen the FOV instead of pulling back
cam.GetFocalLengthAttr().Set(24.0)
cam.GetHorizontalApertureAttr().Set(36.0)
cam.GetVerticalApertureAttr().Set(20.25)
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
    look_at(center + np.array([0.55, 0.15, 0.9]) * size, center + np.array([0, 0.1, 0]) * size))

# Key light from the camera side + softer fill from the left
light_xf = Gf.Matrix4d()
light_xf.SetIdentity()
light_xf.SetRow(2, Gf.Vec4d(-0.3, -0.75, -0.5, 0.0))
sun = UsdLux.DistantLight.Define(rstage, "/Sun")
sun.CreateIntensityAttr().Set(9.0)
UsdGeom.Xformable(sun).AddTransformOp().Set(light_xf)

fill_xf = Gf.Matrix4d()
fill_xf.SetIdentity()
fill_xf.SetRow(2, Gf.Vec4d(0.6, -0.3, 0.55, 0.0))
fill = UsdLux.DistantLight.Define(rstage, "/Fill")
fill.CreateIntensityAttr().Set(2.5)
UsdGeom.Xformable(fill).AddTransformOp().Set(fill_xf)
rstage.GetRootLayer().Save()

if NODE_MATERIAL:
    # HydraRenderer's scene assembler reads the scene file directly and does
    # not traverse sublayer references, so hand it a flattened copy of the
    # composed stage (all prims resolved into one layer). The node-written
    # prims are `over`s there (modifier-layer non-destructive semantics);
    # the assembler only draws concrete defs, so flip the specifier.
    scene = OUT_DIR / f"treegen_{TAG}_nodescene.usda"
    if scene.exists():
        scene.unlink()
    rstage.Export(str(scene))
    text = scene.read_text()
    text = text.replace('over "treegen"', 'def "treegen"')
    scene.write_text(text)
    print(f"[treegen] flattened C++-authored stage -> {scene}")

# ---- 3. Render in-process (path tracer) ----
import hd_RUZINO_py as renderer
import nodes_core_py as core

WIDTH, HEIGHT, SPP = 800, 600, 48


def locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"
    if fallback.exists():
        return fallback
    sys.exit("render node config not found")


def make_renderer():
    """One HydraRenderer per view: the render delegate does not pick up
    camera edits saved after construction, so each view reopens the scene."""
    hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
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
        executor.sync_node_from_external_storage(socket, core.to_meta_any(value))
    return hydra


from PIL import Image


def render_view(out_name, eye_dir, target_up_frac, dist_scale=1.0):
    # Frame with the 24mm camera (vertical half-FOV ~22.9 deg) while keeping
    # the camera inside the ~15-unit radius the path tracer handles
    eye_dir = np.asarray(eye_dir, np.float64)
    eye_dir = eye_dir / np.linalg.norm(eye_dir)
    half_fov = np.arctan(20.25 / 2.0 / 24.0)
    fit_dist = (size * 0.55) / np.tan(half_fov) * \
        float(os.environ.get("TREEGEN_FIT", "1.05")) * dist_scale
    eye = center + eye_dir * fit_dist
    target = center + np.array([0.0, target_up_frac, 0.0]) * size
    xform = UsdGeom.Xformable(cam)
    ops = xform.GetOrderedXformOps()
    ops[0].Set(look_at(eye, target))
    rstage.GetRootLayer().Save()
    if NODE_MATERIAL:
        # Re-flatten per view so the renderer picks up this view's camera
        # (its assembler reads the scene file, not the live stage).
        if scene.exists():
            scene.unlink()
        rstage.Export(str(scene))
        text = scene.read_text()
        text = text.replace('over "treegen"', 'def "treegen"')
        scene.write_text(text)
    hydra = make_renderer()
    for _ in range(SPP):
        hydra.render(0.0)
    tex = hydra.get_output_texture()
    if not tex or len(tex) != WIDTH * HEIGHT * 4:
        print(f"[treegen] bad texture for {out_name}")
        hydra.stop()
        return
    img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
    rgb = np.clip(img[:, :, :3], 0.0, 1.0)
    rgb = (rgb * 255).astype(np.uint8)
    rgb = np.flipud(rgb)
    out = OUT_DIR / out_name
    Image.fromarray(rgb).save(out)
    print(f"[treegen] saved {out}")
    hydra.stop()


render_view(f"treegen_{TAG}.png", (0.55, 0.25, 0.9), -0.05)
render_view(f"treegen_{TAG}_closeup.png", (0.3, 0.08, 0.5), 0.32, 0.45)

print("[treegen] done")
