#!/usr/bin/env python3
"""
5x5 TreeGen parameter grid + orbit video.

X axis (columns): Gravitropism 0.1 -> 0.9 (broad spread -> vertical broom)
Y axis (rows):    Apical Control 0.8 -> 3.2 (spherical crown -> strong trunk)

NOTE: branch angle was tried as the X axis but F6a's gravitropism 0.61
re-verticalizes laterals within 1-2 internodes, washing the gradient out —
gravitropism itself gives the visible narrow<->wide dimension.

All other parameters stay at the paper's Table 2 F6a defaults; trees are
scaled down (Internode Length 0.5) so the whole grid fits inside the
path tracer's ~15-unit camera radius.

Run from Binaries/Release:
    python ../../source/tests/render_treegen_grid.py

Outputs:
    test_output/treegen_grid/scene.usdc          the 25-tree scene
    test_output/treegen_grid/frame_XXX.png       orbit frames
    test_output/treegen_grid_orbit.mp4           the video
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

N = 5
SPACING = 3.2
GRAVITROPISMS = [0.1, 0.3, 0.5, 0.7, 0.9]             # X / columns
APICAL_CONTROLS = [0.8, 1.4, 2.0, 2.6, 3.2]           # Y / rows
GROWTH_YEARS = 6
INTERNODE_LENGTH = 0.4

OUT_DIR = BIN / "test_output" / "treegen_grid"
OUT_DIR.mkdir(parents=True, exist_ok=True)

# ---- 1. Generate 25 trees via the node graph -------------------------------
import stage_py
import geometry_py as geom
from ruzino_graph import RuzinoGraph

g = RuzinoGraph("TreeGenGrid")
g.loadConfiguration(str(BIN / "geometry_nodes.json"))
g.loadConfiguration(str(BIN / "Plugins" / "TreeGen_geometry_nodes.json"))

cells = []
for j in range(N):
    for i in range(N):
        t = g.createNode("tree_generate", name=f"tree_{i}_{j}")
        m = g.createNode("tree_to_mesh", name=f"mesh_{i}_{j}")
        g.addEdge(t, "Tree Branches", m, "Tree Branches")
        g.addEdge(t, "Leaves", m, "Leaves")
        cells.append((i, j, t, m))

grid_usd = OUT_DIR / "scene.usdc"
stage = stage_py.Stage(str(grid_usd))
geom_payload = stage_py.create_payload_from_stage(stage, "/treegen_grid")
g.setGlobalParams(geom_payload)

b_verts, b_counts, b_indices, b_normals = [], [], [], []
l_verts, l_counts, l_indices, l_normals = [], [], [], []


def append_mesh(acc_v, acc_c, acc_i, acc_n, mesh_comp, offset):
    base = len(acc_v)
    for v in mesh_comp.get_vertices():
        try:
            vx, vy, vz = float(v.x), float(v.y), float(v.z)
        except AttributeError:
            vx, vy, vz = float(v[0]), float(v[1]), float(v[2])
        acc_v.append((vx + offset[0], vy + offset[1], vz + offset[2]))
    # One normal per vertex (both tree nodes emit them in vertex order);
    # omitting normals triggers the renderer's red-NaN artifact
    normals = mesh_comp.get_normals()
    if normals is not None and len(normals) > 0:
        for n in normals:
            try:
                acc_n.append((float(n.x), float(n.y), float(n.z)))
            except AttributeError:
                acc_n.append((float(n[0]), float(n[1]), float(n[2])))
    acc_c.extend(int(c) for c in mesh_comp.get_face_vertex_counts())
    acc_i.extend(int(idx) + base for idx in mesh_comp.get_face_vertex_indices())


total_leaves = 0
for (i, j, t, m) in cells:
    inputs = {
        (t, "Growth Years"): GROWTH_YEARS,
        (t, "Gravitropism"): GRAVITROPISMS[i],
        (t, "Apical Control"): APICAL_CONTROLS[j],
        (t, "Internode Length"): INTERNODE_LENGTH,
    }
    g.prepare_and_execute(inputs, required_node=m)

    bm = geom.extract_geometry_from_meta_any(
        g.getOutput(m, "Branch Mesh")).get_mesh_component(0)
    lm = geom.extract_geometry_from_meta_any(
        g.getOutput(m, "Leaf Mesh")).get_mesh_component(0)

    offset = ((i - (N - 1) / 2.0) * SPACING, 0.0, (j - (N - 1) / 2.0) * SPACING)
    append_mesh(b_verts, b_counts, b_indices, b_normals, bm, offset)
    append_mesh(l_verts, l_counts, l_indices, l_normals, lm, offset)
    total_leaves += len(lm.get_face_vertex_counts()) // 2  # 2 tris per leaf
    print(f"[grid] cell ({i},{j}) gt={GRAVITROPISMS[i]:>3} "
          f"ac={APICAL_CONTROLS[j]} leaves={len(lm.get_face_vertex_counts())//2}")

stage.save()
print(f"[grid] generated {N*N} trees, ~{total_leaves} leaves total")

# ---- 2. Author the render scene --------------------------------------------
from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf
import numpy as np

scene = OUT_DIR / "grid_scene.usdc"
if scene.exists():
    scene.unlink()
rstage = Usd.Stage.CreateNew(str(scene))


def add_mesh(prim_path, verts, counts, indices, color, roughness, normals=None):
    mesh = UsdGeom.Mesh.Define(rstage, prim_path)
    mesh.CreatePointsAttr().Set([Gf.Vec3f(*v) for v in verts])
    mesh.CreateFaceVertexCountsAttr().Set(counts)
    mesh.CreateFaceVertexIndicesAttr().Set(indices)
    if normals and len(normals) == len(verts):
        mesh.CreateNormalsAttr().Set([Gf.Vec3f(*n) for n in normals])
        mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    mat = UsdShade.Material.Define(rstage, prim_path + "_mat")
    shader = UsdShade.Shader.Define(rstage, prim_path + "_mat/Shader")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(color)
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(roughness)
    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)


add_mesh("/Grid/Branches", b_verts, b_counts, b_indices,
         (0.42, 0.30, 0.20), 0.8, b_normals)
add_mesh("/Grid/Leaves", l_verts, l_counts, l_indices,
         (0.22, 0.55, 0.16), 0.6, l_normals)

half_span = (N - 1) / 2.0 * SPACING + SPACING * 0.6
ground = UsdGeom.Mesh.Define(rstage, "/Ground")
ground.CreatePointsAttr().Set([
    Gf.Vec3f(-half_span * 1.6, 0, -half_span * 1.6),
    Gf.Vec3f(half_span * 1.6, 0, -half_span * 1.6),
    Gf.Vec3f(half_span * 1.6, 0, half_span * 1.6),
    Gf.Vec3f(-half_span * 1.6, 0, half_span * 1.6)])
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

# Key + fill lights (camera-side key, as for the single-tree renders)
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

# Orbit camera. NOTE: the render delegate reads the camera transform ONCE
# at construction and ignores time-sampled edits (black frames otherwise),
# so the orbit renders one HydraRenderer instance per frame with a static
# transform rewritten before each construction.
FRAMES = 72
FPS = 12
ORBIT_RADIUS = 10.5
ORBIT_HEIGHT = 4.8
LOOK_AT = (0.0, 1.2, 0.0)

cam = UsdGeom.Camera.Define(rstage, "/Camera")
cam.GetFocalLengthAttr().Set(20.0)
cam.GetHorizontalApertureAttr().Set(36.0)
cam.GetVerticalApertureAttr().Set(20.25)
cam.GetClippingRangeAttr().Set((0.01, 1000.0))
cam_xform = UsdGeom.Xformable(cam)
cam_op = cam_xform.AddTransformOp()

_center = np.array(LOOK_AT, dtype=np.float64)


def look_at_matrix(eye, target):
    eye = np.asarray(eye, dtype=np.float64)
    target = np.asarray(target, dtype=np.float64)
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


def set_orbit_camera(frame):
    ang = 2.0 * np.pi * frame / FRAMES
    eye = (_center[0] + ORBIT_RADIUS * np.sin(ang),
           ORBIT_HEIGHT,
           _center[2] + ORBIT_RADIUS * np.cos(ang))
    cam_op.Set(look_at_matrix(eye, LOOK_AT))
    rstage.GetRootLayer().Save()


set_orbit_camera(0)
print(f"[grid] scene written: {scene}")

# ---- 3. Render the orbit ----------------------------------------------------
import hd_RUZINO_py as renderer
import nodes_core_py as core

WIDTH, HEIGHT, SPP = 960, 540, 32


def build_renderer():
    """One HydraRenderer per frame: it snapshots the camera at construction."""
    hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(BIN / "render_nodes.json"))
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

for f in range(FRAMES):
    set_orbit_camera(f)
    hydra = build_renderer()
    for _ in range(SPP):
        hydra.render(0.0)
    tex = hydra.get_output_texture()
    if not tex or len(tex) != WIDTH * HEIGHT * 4:
        print(f"[grid] frame {f}: bad texture")
        hydra.stop()
        continue
    img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
    rgb = np.clip(img[:, :, :3], 0.0, 1.0)
    rgb = (rgb * 255).astype(np.uint8)
    rgb = np.flipud(rgb)
    Image.fromarray(rgb).save(OUT_DIR / f"frame_{f:03d}.png")
    hydra.stop()
    if f % 12 == 0:
        print(f"[grid] frame {f}/{FRAMES - 1}")

# ---- 4. Encode the video ----------------------------------------------------
import subprocess

mp4 = BIN / "test_output" / "treegen_grid_orbit.mp4"
subprocess.run([
    "ffmpeg", "-y", "-framerate", str(FPS),
    "-i", str(OUT_DIR / "frame_%03d.png"),
    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
    str(mp4),
], check=True, capture_output=True)
print(f"[grid] video: {mp4} ({mp4.stat().st_size // 1024} KB)")
