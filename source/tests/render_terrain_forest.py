#!/usr/bin/env python3
"""
Terrain x procedural forest: the full node-graph pipeline —
terrain_heightfield -> erode (GPU pipes) -> thermal -> texture_bake
    -> terrain_scatter_points (biome grass mask, slope/height filters)
tree_generate -> tree_to_mesh
    -> instance_on_points x2 (branches / leaves, Y-up, width-as-scale)
    -> write_usd (terrain + 2 PointInstancers)
then an offline hd_RUZINO path-traced render of the composed stage.

The world is authored at meters (200 m terrain, ~12 m trees) and scaled
x0.1 into the renderer like render_terrain_image.py — the path tracer
goes black past ~20 units of camera distance, so the whole forest lives
in a ~20-unit scene.

Run from Binaries/Release:
    python ../../source/tests/render_terrain_forest.py [tag]

Env knobs: FOREST_TREES (default 300), FOREST_SPP (default 32),
FOREST_SEED, FOREST_RES (terrain resolution, default 512),
FOREST_ERODE_IT (default 250), FOREST_SPACING (default 11 m),
FOREST_TEX (bake texture resolution, default 2048).
"""
import os
import sys
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
from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt  # noqa: E402

OUT_DIR = BIN / "test_output"
OUT_DIR.mkdir(exist_ok=True)

TAG = sys.argv[1] if len(sys.argv) > 1 else "forest"
TREES = int(os.environ.get("FOREST_TREES", "300"))
SPP = int(os.environ.get("FOREST_SPP", "32"))
SEED = int(os.environ.get("FOREST_SEED", "1234"))
RES = int(os.environ.get("FOREST_RES", "512"))
ERODE_IT = int(os.environ.get("FOREST_ERODE_IT", "250"))
SPACING = float(os.environ.get("FOREST_SPACING", "11.0"))
TEX_RES = int(os.environ.get("FOREST_TEX", "2048"))
TREE_SCALE = float(os.environ.get("FOREST_TREE_SCALE", "2.0"))
SCALE = 0.1  # meters -> renderer units (see module docstring)

LEAF_ATLAS = str(
    (ROOT / "source" / "Plugins" / "TreeGen" / "assets" /
     "leaf_atlas.png").resolve())

# ---- 1. Generate everything through the node graph ----
import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

stage_file = OUT_DIR / f"terrain_forest_{TAG}.usdc"
for f in (stage_file, OUT_DIR / f"terrain_forest_{TAG}_modifiers.usdc"):
    if f.exists():
        f.unlink()

g = RuzinoGraph("ForestScene")
g.loadConfiguration(str(BIN / "Plugins" / "TerrainGen_geometry_nodes.json"))
g.loadConfiguration(str(BIN / "Plugins" / "TreeGen_geometry_nodes.json"))
g.loadConfiguration(str(BIN / "geometry_nodes.json"))

hf = g.createNode("terrain_heightfield", name="terrain")
erode = g.createNode("terrain_erode_hydraulic", name="erode")
thermal = g.createNode("terrain_erode_thermal", name="thermal")
bake = g.createNode("terrain_texture_bake", name="bake")
scatter = g.createNode("terrain_scatter_points", name="scatter")

tree = g.createNode("tree_generate", name="tree")
to_mesh = g.createNode("tree_to_mesh", name="tree_mesh")

inst_branch = g.createNode("instance_on_points", name="inst_branch")
inst_leaf = g.createNode("instance_on_points", name="inst_leaf")

w_terrain = g.createNode("write_usd", name="w_terrain")
w_branch = g.createNode("write_usd", name="w_branch")
w_leaf = g.createNode("write_usd", name="w_leaf")

g.addEdge(hf, "Height Field", erode, "Height Field")
g.addEdge(erode, "Height Field", thermal, "Height Field")
g.addEdge(thermal, "Height Field", bake, "Height Field")
g.addEdge(bake, "Height Field", scatter, "Height Field")

g.addEdge(tree, "Tree Branches", to_mesh, "Tree Branches")
g.addEdge(tree, "Leaves", to_mesh, "Leaves")

g.addEdge(to_mesh, "Branch Mesh", inst_branch, "Geometry")
g.addEdge(scatter, "Points", inst_branch, "Points")
g.addEdge(to_mesh, "Leaf Mesh", inst_leaf, "Geometry")
g.addEdge(scatter, "Points", inst_leaf, "Points")

g.addEdge(bake, "Height Field", w_terrain, "Geometry")
g.addEdge(inst_branch, "Geometry", w_branch, "Geometry")
g.addEdge(inst_leaf, "Geometry", w_leaf, "Geometry")

inputs = {
    # Terrain: one eroded mountain block with grass in the valleys.
    (hf, "Resolution"): RES,
    (hf, "Size"): 200.0,
    (hf, "Height"): float(os.environ.get("FOREST_H", "32.0")),
    (hf, "Seed"): SEED,
    (hf, "Noise Type"): "Ridged Multifractal",
    (hf, "Ridge Blend"): float(os.environ.get("FOREST_RIDGE", "0.45")),
    (erode, "Method"): "Virtual Pipes (GPU)",
    (erode, "Iterations"): ERODE_IT,
    (thermal, "Iterations"): 30,
    (bake, "Texture Resolution"): TEX_RES,
    (bake, "Grass Line"): float(os.environ.get("FOREST_GRASS_LINE", "0.5")),
    (bake, "Output Path"):
        str((OUT_DIR / f"terrain_forest_{TAG}_albedo.png").resolve()),
    (bake, "Normal Output Path"):
        str((OUT_DIR / f"terrain_forest_{TAG}_normal.png").resolve()),
    # Forest: grass biome only, gentle slopes, below the rock line.
    (scatter, "Count"): TREES,
    (scatter, "Min Distance"): SPACING,
    (scatter, "Seed"): 7,
    (scatter, "Slope Max"): 45.0,
    (scatter, "Height Max"): 0.72,
    (scatter, "Mask Field"): "biome",
    (scatter, "Mask Min"): 2.0,
    (scatter, "Mask Max"): 2.0,
    (scatter, "Scale Min"): 0.75,
    (scatter, "Scale Max"): 1.3,
    # Tree: proven render_treegen.py tune (small dense leaves), ~12 m tall.
    (tree, "Growth Years"): 8,
    (tree, "Random Seed"): 42,
    (tree, "Leaf Size"): 0.15,
    (tree, "Leaves Per Internode"): 40,
    (to_mesh, "Radial Segments"): 8,
    # Instances stand upright (Y-up), size from the scatter's width field.
    (inst_branch, "Y-up"): True,
    (inst_leaf, "Y-up"): True,
    (w_terrain, "Sub Path"): "terrain",
    (w_branch, "Sub Path"): "forest_branches",
    (w_leaf, "Sub Path"): "forest_leaves",
}
for inst in (inst_branch, inst_leaf):
    inputs[(inst, "Use Width as Scale")] = True
    inputs[(inst, "Scale Multiplier")] = TREE_SCALE

stage = stage_py.Stage(str(stage_file))
g.setGlobalParams(stage_py.create_payload_from_stage(stage, "/ForestScene"))
for writer in (w_terrain, w_branch, w_leaf):
    g.prepare_and_execute(inputs, required_node=writer)
stage.save()
print(f"[forest] node graph wrote {stage_file}")

# ---- 2. Assemble the render scene from the modifier sidecar ----
sidecar = OUT_DIR / f"terrain_forest_{TAG}_modifiers.usdc"
if not sidecar.exists():
    sys.exit(f"modifier sidecar missing: {sidecar}")
src = Usd.Stage.Open(str(sidecar))


def get_instancer_data(path):
    inst = src.GetPrimAtPath(path)
    if not inst.IsValid():
        sys.exit(f"[forest] PointInstancer missing: {path}")
    pos = np.array(
        inst.GetAttribute("positions").Get(), dtype=np.float32)
    scales = np.array(
        inst.GetAttribute("scales").Get() or [], dtype=np.float32)
    orients = inst.GetAttribute("orientations").Get() or []
    return inst, pos, scales, orients


def add_mesh(rstage, src_prim, path, scale=SCALE):
    pts = np.array(src_prim.GetAttribute("points").Get(), dtype=np.float32)
    fvc = src_prim.GetAttribute("faceVertexCounts").Get()
    fvi = src_prim.GetAttribute("faceVertexIndices").Get()
    mesh = UsdGeom.Mesh.Define(rstage, path)
    mesh.CreatePointsAttr().Set(
        Vt.Vec3fArray.FromNumpy(pts * scale))
    mesh.CreateFaceVertexCountsAttr().Set(fvc)
    mesh.CreateFaceVertexIndicesAttr().Set(fvi)
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    src_mesh = UsdGeom.Mesh(src_prim)
    nrm = src_mesh.GetNormalsAttr().Get()
    if nrm:
        mesh.CreateNormalsAttr().Set(Vt.Vec3fArray.FromNumpy(
            np.array(nrm, dtype=np.float32) * scale))
        mesh.SetNormalsInterpolation(src_mesh.GetNormalsInterpolation())
    uv_pv = UsdGeom.PrimvarsAPI(src_prim).GetPrimvar("UVMap")
    uv = uv_pv.Get() if uv_pv else None
    if uv:
        pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
            "UVMap", Sdf.ValueTypeNames.TexCoord2fArray,
            uv_pv.GetInterpolation())
        pv.Set(uv)
    return mesh


scene = OUT_DIR / f"terrain_forest_{TAG}_scene.usdc"
if scene.exists():
    scene.unlink()
rstage = Usd.Stage.CreateNew(str(scene))

# Terrain with the baked albedo + normal maps.
terrain_prim = src.GetPrimAtPath("/ForestScene/terrain")
terrain_mesh = add_mesh(rstage, terrain_prim, "/Terrain")
tmat = UsdShade.Material.Define(rstage, "/Terrain/Terrain_mat")
pbr = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/PBRShader")
pbr.CreateIdAttr("UsdPreviewSurface")
pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.95)
st_reader = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/stReader")
st_reader.CreateIdAttr("UsdPrimvarReader_float2")
st_reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("UVMap")


def add_tex(name, file_path):
    tex = UsdShade.Shader.Define(rstage, f"/Terrain/Terrain_mat/{name}")
    tex.CreateIdAttr("UsdUVTexture")
    tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(Path(file_path).as_posix()))
    tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
        st_reader.ConnectableAPI(), "result")
    tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
    tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")
    return tex


albedo_png = OUT_DIR / f"terrain_forest_{TAG}_albedo.png"
normal_png = OUT_DIR / f"terrain_forest_{TAG}_normal.png"
if albedo_png.exists():
    pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f). \
        ConnectToSource(add_tex("diffuseTexture", albedo_png)
                        .CreateOutput("rgb", Sdf.ValueTypeNames.Color3f))
else:
    print(f"[forest] WARN: baked albedo missing: {albedo_png}")
    pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.32, 0.30, 0.24))
if normal_png.exists():
    # Tangent-space decode via scale/bias (USD-standard wiring); the
    # renderer's preview-surface mtlx graph routes it through mx_normalmap
    # with the vertex TBN (render_terrain_image.py recipe).
    nrm_tex = UsdShade.Shader.Define(
        rstage, "/Terrain/Terrain_mat/normalTexture")
    nrm_tex.CreateIdAttr("UsdUVTexture")
    nrm_tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(normal_png.as_posix()))
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
    print("[forest] normal map connected")
tmat.CreateSurfaceOutput().ConnectToSource(
    UsdShade.ConnectableAPI(pbr), "surface")
UsdShade.MaterialBindingAPI.Apply(terrain_mesh.GetPrim()).Bind(tmat)

# Tree prototypes: bark flat color, leaves atlas with alpha cutout.
branch_prim = src.GetPrimAtPath("/ForestScene/forest_branches/Prototype")
leaf_prim = src.GetPrimAtPath("/ForestScene/forest_leaves/Prototype")
branch_mesh = add_mesh(rstage, branch_prim, "/Forest/Branches")
leaf_mesh = add_mesh(rstage, leaf_prim, "/Forest/Leaves")

bmat = UsdShade.Material.Define(rstage, "/Forest/Branch_mat")
bsh = UsdShade.Shader.Define(rstage, "/Forest/Branch_mat/Shader")
bsh.CreateIdAttr("UsdPreviewSurface")
bsh.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
    (0.30, 0.21, 0.14))
bsh.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.9)
bmat.CreateSurfaceOutput().ConnectToSource(
    UsdShade.ConnectableAPI(bsh), "surface")
UsdShade.MaterialBindingAPI.Apply(branch_mesh.GetPrim()).Bind(bmat)

lmat = UsdShade.Material.Define(rstage, "/Forest/Leaf_mat")
lsh = UsdShade.Shader.Define(rstage, "/Forest/Leaf_mat/PreviewSurface")
lsh.CreateIdAttr("UsdPreviewSurface")
lsh.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.55)
lsh.CreateInput("opacityMode", Sdf.ValueTypeNames.String).Set("mask")
lsh.CreateInput("opacityThreshold", Sdf.ValueTypeNames.Float).Set(0.5)
luvr = UsdShade.Shader.Define(rstage, "/Forest/Leaf_mat/UVReader")
luvr.CreateIdAttr("UsdPrimvarReader_float2")
luvr.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("UVMap")
luv_out = luvr.CreateOutput("result", Sdf.ValueTypeNames.Float2)


def add_leaf_tex(name):
    tex = UsdShade.Shader.Define(rstage, f"/Forest/Leaf_mat/{name}")
    tex.CreateIdAttr("UsdUVTexture")
    tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(Path(LEAF_ATLAS).as_posix()))
    tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(luv_out)
    return tex


lsh.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(
    add_leaf_tex("TexDiffuse").CreateOutput(
        "rgb", Sdf.ValueTypeNames.Color3f))
lsh.CreateInput("opacity", Sdf.ValueTypeNames.Float).ConnectToSource(
    add_leaf_tex("TexOpacity").CreateOutput(
        "a", Sdf.ValueTypeNames.Float))
lmat.CreateSurfaceOutput().ConnectToSource(
    UsdShade.ConnectableAPI(lsh), "surface")
UsdShade.MaterialBindingAPI.Apply(leaf_mesh.GetPrim()).Bind(lmat)

# The two PointInstancers: positions scaled into the render scene,
# orientations/scales passed through from the node graph.
for path, proto in (("/forest_branches", "/Forest/Branches"),
                    ("/forest_leaves", "/Forest/Leaves")):
    _, pos, scales, orients = get_instancer_data("/ForestScene" + path)
    instancer = UsdGeom.PointInstancer.Define(rstage, path)
    instancer.CreatePrototypesRel().SetTargets([Sdf.Path(proto)])
    instancer.CreatePositionsAttr().Set(Vt.Vec3fArray.FromNumpy(
        pos * SCALE))
    n = len(pos)
    instancer.CreateProtoIndicesAttr().Set(Vt.IntArray(n, 0))
    if len(orients) == n:
        instancer.CreateOrientationsAttr().Set(orients)
    if len(scales) == n:
        instancer.CreateScalesAttr().Set(
            Vt.Vec3fArray.FromNumpy(scales))
    print(f"[forest] {path}: {n} instances -> {proto}")

# Camera: across-valley view framing the treeline.
n_trees = len(np.array(src.GetPrimAtPath(
    "/ForestScene/forest_branches").GetAttribute("positions").Get()))
terrain_pts = np.array(
    terrain_prim.GetAttribute("points").Get(), dtype=np.float32) * SCALE
mn, mx = terrain_pts.min(axis=0), terrain_pts.max(axis=0)
center = (mn + mx) * 0.5
center[1] += 2.0

cam = UsdGeom.Camera.Define(rstage, "/Camera")
cam.GetFocalLengthAttr().Set(30.0)
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


spread = float(max(mx[0] - mn[0], mx[2] - mn[2]))
eye = center + np.array([0.0, spread * 0.22, spread * 0.60])
UsdGeom.Xformable(cam).AddTransformOp().Set(look_at(eye, center))

dome = UsdLux.DomeLight.Define(rstage, "/Sky")
dome.CreateIntensityAttr().Set(1.3)
dome.CreateColorAttr().Set(Gf.Vec3f(0.55, 0.68, 0.95))
dome.CreateExposeRawAttr().Set(False) if False else None

sun_xf = Gf.Matrix4d()
sun_xf.SetIdentity()
sun_xf.SetRow(2, Gf.Vec4d(-0.35, -0.7, -0.45, 0.0))
sun = UsdLux.DistantLight.Define(rstage, "/Sun")
sun.CreateIntensityAttr().Set(9.0)
UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)
fill_xf = Gf.Matrix4d()
fill_xf.SetIdentity()
fill_xf.SetRow(2, Gf.Vec4d(0.55, -0.35, 0.6, 0.0))
fill = UsdLux.DistantLight.Define(rstage, "/Fill")
fill.CreateIntensityAttr().Set(2.2)
UsdGeom.Xformable(fill).AddTransformOp().Set(fill_xf)
rstage.GetRootLayer().Save()
print(f"[forest] render scene: {scene} ({n_trees} trees, camera at "
      f"{np.round(eye, 1).tolist()})")

# ---- 3. Render in-process (path tracer), render_treegen.py recipe ----

import hd_RUZINO_py as renderer  # noqa: E402
import nodes_core_py as core  # noqa: E402
from PIL import Image  # noqa: E402
import time  # noqa: E402

WIDTH, HEIGHT = 960, 540


def locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" /         "render_nodes_save.json"
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
    executor.sync_node_from_external_storage(
        socket, core.to_meta_any(value))

t0 = time.time()
for frame in range(SPP):
    hydra.render(0.0)
    if (frame + 1) % max(1, SPP // 4) == 0:
        print(f"[forest] spp {frame + 1}/{SPP} ({time.time() - t0:.1f}s)")

tex = hydra.get_output_texture()
if not tex or len(tex) != WIDTH * HEIGHT * 4:
    sys.exit("[forest] bad output texture")
img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
rgb = np.clip(img[:, :, :3], 0.0, 1.0)
rgb = (rgb * 255).astype(np.uint8)
rgb = np.flipud(rgb)
out_png = OUT_DIR / f"terrain_forest_{TAG}.png"
Image.fromarray(rgb).save(out_png)
hydra.stop()
print(f"[forest] wrote {out_png} ({TREES} trees requested, {n_trees} "
      f"placed, SPP={SPP}, {time.time() - t0:.1f}s)")
