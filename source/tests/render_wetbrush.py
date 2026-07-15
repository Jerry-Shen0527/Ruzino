#!/usr/bin/env python3
"""
Render the Wetbrush streaming-zone paint animation with the Ruzino path tracer.

Stage 1 (PLACEHOLDER, no volume rprim yet): drives the streaming Wetbrush zone
(mock_stroke -> simulation_in -> brush_wb_deposit -> bristle -> fluid -> commit
-> simulation_out, feedback), reads the commit node's Paint Particles output
(one point per painted canvas cell, RYB->RGB color baked in, thickness in Z),
bakes it into a self-contained render scene, and renders a per-frame PNG
sequence in-process via HydraRenderer.render(time_code).

The POINTS rprim renders each paint cell as an analytic sphere sized by
`widths`; color is single-material (Points rprim has no per-point color path
-- displayColor primvar is written but unread by the renderer). So this first
pass renders the stroke SHAPE and thickness; the real volume rendering (with
per-cell color via the new Hd_RUZINO_WetbrushVolume rprim + custom intersection
shader) replaces this scene in a later step.

Reuses the proven render_gridbox.py three-stage pattern:
  1. run the streaming zone into an in-memory stage (time-sampled points),
  2. bake a render scene (camera + light + the points geometry, animated),
  3. in-process HydraRenderer.render(t) loop -> PNG sequence.

Run from Binaries/Release (so node-plugin DLLs resolve):

    python ../../source/tests/render_wetbrush.py
"""
import math
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))

from pxr import Usd, UsdGeom, UsdLux, UsdShade, UsdVol, Sdf, Gf, Vt  # noqa: E402

import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

NUM_FRAMES = 60
FPS = 60.0
DT = 1.0 / FPS

OUTPUT_DIR = BIN / "wetbrush_sequence"


# ---------------------------------------------------------------------------
# Stage 1: build the streaming Wetbrush zone (same topology as
# test_wetbrush_zone._build_streaming_graph) and drive it NUM_FRAMES ticks.
# Returns (pxr_stage, prim_path) where prim_path carries time-sampled `points`.
# ---------------------------------------------------------------------------
def run_streaming_zone(out_usd: Path):
    """Drive the streaming zone and return a COMPOSED pxr stage.

    write_usd writes the per-frame points into a session/modifier layer, not
    into the skeleton .usdc (which only holds the /Brush prim skeleton). A plain
    Usd.Stage.Open(skeleton) composes the session layer only at cook time; to
    read the persisted animation afterwards we compose skeleton + modifier
    explicitly via a subLayerPaths layer (mirrors render_gridbox.py:62-76).
    """
    g = RuzinoGraph("WetbrushRender")
    g.loadConfiguration(str(BIN / "geometry_nodes.json"))

    mock = g.createNode("mock_stroke", name="MockStroke")
    init_state = g.createNode("brush_wb_init_state", name="InitState")
    sim_in, sim_out = g.createSimulationZone()
    emitter = g.createNode("mock_point_emitter", name="Emitter")
    deposit = g.createNode("brush_wb_deposit", name="Deposit")
    bristle = g.createNode("brush_wb_bristle", name="Bristle")
    fluid = g.createNode("brush_wb_fluid", name="Fluid")
    commit = g.createNode("brush_wb_commit", name="Commit")
    write = g.createNode("write_usd", name="Output")

    g.addEdge(mock, "Stroke Curves", sim_in, "Simulation In")
    g.addEdge(init_state, "State", sim_in, "Simulation In")
    g.addEdge(sim_in, "Simulation Out", emitter, "Stroke Curves")
    g.addEdge(emitter, "Current Point", deposit, "Brush Point")
    g.addEdge(sim_in, "Simulation Out", deposit, "State")
    g.addEdge(deposit, "Brush Point", fluid, "Brush Point")
    g.addEdge(deposit, "State", bristle, "State")
    g.addEdge(bristle, "State", fluid, "State")
    g.addEdge(fluid, "State", commit, "State")
    g.addEdge(sim_in, "Simulation Out", commit, "Stroke Curves")
    # Read the 3D density window (paper §6 render target), not the 2D canvas
    # accumulation. commit emits one point per painted 3D voxel of the
    # brush-local active window, in world space, with the instantaneous density
    # in widths.
    g.addEdge(commit, "Paint Field 3D", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): 30, (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (deposit, "Resolution"): 256, (deposit, "Paper Size"): 1.0,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1, (fluid, "Brush Radius"): 0.02,
    })
    assert sim_in.paired_node is sim_out, "zone pairing not established"

    if out_usd.exists():
        out_usd.unlink()
    stage = stage_py.Stage(str(out_usd))
    prim_path = "/Brush"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)  # placeholder prim
    g.apply_to_stage(stage, prim_path)

    # The three simulation gates (AGENTS.md §Simulation).
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    for i in range(NUM_FRAMES):
        stage.set_render_time((i + 1) * DT)
        stage.tick(DT)
        stage.finish_tick()
    stage.save()

    # Compose skeleton + modifier so the time-sampled points are readable.
    # The modifier layer path mirrors what render_gridbox.py expects; find it
    # next to the skeleton.
    modifier = out_usd.with_name(out_usd.stem + "_modifiers.usdc")
    if not modifier.exists():
        sys.exit(f"modifier layer not found: {modifier} -- sim did not export")
    composed = out_usd.parent / "wetbrush_render_composed.usda"
    if composed.exists():
        composed.unlink()
    layer = Sdf.Layer.CreateNew(str(composed))
    layer.subLayerPaths = [
        str(modifier.resolve()), str(out_usd.resolve())]
    layer.Save()
    composed_stage = Usd.Stage.Open(str(composed))

    # Sanity-check the points are actually there before returning.
    _p = composed_stage.GetPrimAtPath(Sdf.Path(prim_path))
    _pts_attr = _p.GetAttribute("points")
    _pts_ts = _pts_attr.GetTimeSamples() if _pts_attr else []
    if not _pts_ts:
        sys.exit("composed stage has no time-sampled points -- sim failed")
    _final = _pts_attr.Get(_pts_ts[-1])
    print(f"[render] stage 1: composed {len(_pts_ts)} time samples "
          f"({len(_final) if _final else 0} pts at final frame)")
    return composed_stage, prim_path, g  # keep g alive so sim GPU buffers survive


# ---------------------------------------------------------------------------
# Stage 2: bake a self-contained render scene. Splat the sim's per-frame paint
# points (one per painted canvas cell, RYB->RGB baked in) into a 2D paint grid
# (Float4: density, r, g, b) and author it on a UsdGeomVolume prim as the
# `paintField` primvar (+ gridRes/gridPaper/gridCenter/canvasFloorZ metadata).
# Hd_RUZINO_WetbrushVolume reads these primvars, builds the density slab, and
# the wetbrush_render node raymarches it via the VolumeClosestHit shader.
# ---------------------------------------------------------------------------
def bake_render_scene(sim_stage, prim_path: str, scene_path: Path):
    """Accumulate the 3D density windows into one large 3D paint field.

    The commit node emits, per frame, the 3D density window (paper §6 active
    window) as a point cloud: one point per painted 3D voxel, in world space,
    widths = instantaneous density (bounded), displayColor = RYB->RGB.

    For a "growing stroke" animation, each render frame N shows the UNION of
    sim frames 0..N rasterized into a 3D grid covering the whole stroke bbox.
    """
    src = UsdGeom.Points.Get(sim_stage, prim_path)
    if not src.GetPrim().IsValid():
        sys.exit(f"sim prim {prim_path} is not a valid Points prim "
                 f"(write_usd may not have run)")

    src_pts_attr = src.GetPointsAttr()
    frame_times = src_pts_attr.GetTimeSamples()
    if not frame_times:
        sys.exit("sim points has no time samples -- the zone did not cook")
    print(f"[render] anim: {len(frame_times)} time samples "
          f"(t={frame_times[0]:.4f}..{frame_times[-1]:.4f})")

    src_color_primvar = UsdGeom.PrimvarsAPI(src).GetPrimvar("displayColor")
    src_widths_attr = src.GetWidthsAttr()

    # Collect ALL frames' points (positions, density, color) and compute the
    # 3D union bbox so the volume grid covers the whole stroke.
    all_pts = []        # list of np.array(N,3) per frame
    all_density = []    # list of np.array(N,) per frame
    all_color = []      # list of np.array(N,3) per frame
    pmin = np.array([1e9, 1e9, 1e9], dtype=np.float32)
    pmax = np.array([-1e9, -1e9, -1e9], dtype=np.float32)
    for t in frame_times:
        pts = src_pts_attr.Get(t)
        widths = src_widths_attr.Get(t) if src_widths_attr else None
        colors = src_color_primvar.Get(t) if src_color_primvar else None
        if not pts or len(pts) == 0:
            all_pts.append(np.zeros((0, 3), dtype=np.float32))
            all_density.append(np.zeros((0,), dtype=np.float32))
            all_color.append(np.zeros((0, 3), dtype=np.float32))
            continue
        pa = np.array([[p[0], p[1], p[2]] for p in pts], dtype=np.float32)
        da = np.array([float(widths[i]) if (widths and i < len(widths))
                       else 0.5 for i in range(len(pts))], dtype=np.float32)
        ca = (np.array([[colors[i][0], colors[i][1], colors[i][2]]
                        for i in range(len(pts))], dtype=np.float32)
              if (colors and len(colors)) else
              np.ones((len(pts), 3), dtype=np.float32))
        all_pts.append(pa); all_density.append(da); all_color.append(ca)
        pmin = np.minimum(pmin, pa.min(axis=0))
        pmax = np.maximum(pmax, pa.max(axis=0))

    if pmax[0] - pmin[0] <= 0:
        sys.exit("all frames empty -- nothing to render")
    print(f"[render] stroke 3D bbox: [{pmin[0]:.4f},{pmin[1]:.4f},"
          f"{pmin[2]:.4f}]..[{pmax[0]:.4f},{pmax[1]:.4f},{pmax[2]:.4f}]")

    # 3D grid covering the stroke bbox (square XY, real Z range), with margin.
    margin_xy = max((pmax[0] - pmin[0]), (pmax[1] - pmin[1])) * 0.2 + 0.005
    margin_z = (pmax[2] - pmin[2]) * 0.2 + 0.002
    grid_min = np.array([pmin[0] - margin_xy, pmin[1] - margin_xy,
                         pmin[2] - margin_z], dtype=np.float32)
    grid_max = np.array([pmax[0] + margin_xy, pmax[1] + margin_xy,
                         pmax[2] + margin_z], dtype=np.float32)
    # Use the sim's actual cell size so the render grid matches the data
    # resolution (no up/downsampling): cell_sz = grid_paper / grid_res.
    SIM_RES = 256; SIM_PAPER = 1.0
    cell_sz = SIM_PAPER / SIM_RES  # ~0.0039, the source voxel size
    GRID_X = int(np.ceil((grid_max[0] - grid_min[0]) / cell_sz))
    GRID_Y = int(np.ceil((grid_max[1] - grid_min[1]) / cell_sz))
    GRID_Z = int(np.ceil((grid_max[2] - grid_min[2]) / cell_sz))
    print(f"[render] 3D grid: {GRID_X}x{GRID_Y}x{GRID_Z} cells "
          f"(cell_sz={cell_sz:.5f}, total {GRID_X*GRID_Y*GRID_Z} voxels)")

    grid_extent = grid_max - grid_min
    grid_center_world = ((grid_min + grid_max) * 0.5).tolist()

    if scene_path.exists():
        scene_path.unlink()
    stage = Usd.Stage.CreateNew(str(scene_path))

    # UsdVolVolume -> Hydra token "volume" -> Hd_RUZINO_WetbrushVolume.
    vol = UsdVol.Volume.Define(stage, "/BrushPaint")
    primvar_api = UsdGeom.PrimvarsAPI(vol)
    paint_primvar = primvar_api.CreatePrimvar(
        "paintField", Sdf.ValueTypeNames.Float4Array)
    paint_primvar.SetInterpolation(UsdGeom.Tokens.constant)
    # 3D grid metadata primvars (read by the rprim + shader).
    primvar_api.CreatePrimvar(
        "gridResX", Sdf.ValueTypeNames.Int).Set(int(GRID_X))
    primvar_api.CreatePrimvar(
        "gridResY", Sdf.ValueTypeNames.Int).Set(int(GRID_Y))
    primvar_api.CreatePrimvar(
        "gridResZ", Sdf.ValueTypeNames.Int).Set(int(GRID_Z))
    primvar_api.CreatePrimvar(
        "cellSize", Sdf.ValueTypeNames.Float).Set(float(cell_sz))
    gm = Gf.Vec3f(float(grid_min[0]), float(grid_min[1]), float(grid_min[2]))
    primvar_api.CreatePrimvar(
        "gridMin", Sdf.ValueTypeNames.Float3).Set(gm)

    # For each render frame, rasterize the sim's 3D window points into the
    # render grid. Last-write semantics: each frame's window data is the
    # current state of those cells (the sim's global grid is now persistent,
    # paper §4.2). Non-zero points overwrite; zero points are skipped so they
    # don't erase previously painted cells that the window has moved past.
    GRID_TOTAL = GRID_X * GRID_Y * GRID_Z
    accum_density = np.zeros(GRID_TOTAL, dtype=np.float32)
    accum_color = np.zeros((GRID_TOTAL, 3), dtype=np.float32)
    for fi, t in enumerate(frame_times):
        pa = all_pts[fi]; da = all_density[fi]; ca = all_color[fi]
        for i in range(len(pa)):
            if da[i] <= 0:
                continue
            ix = int((pa[i, 0] - grid_min[0]) / cell_sz)
            iy = int((pa[i, 1] - grid_min[1]) / cell_sz)
            iz = int((pa[i, 2] - grid_min[2]) / cell_sz)
            if (ix < 0 or iy < 0 or iz < 0 or
                    ix >= GRID_X or iy >= GRID_Y or iz >= GRID_Z):
                continue
            idx = (iz * GRID_Y + iy) * GRID_X + ix
            accum_density[idx] = da[i]
            accum_color[idx] = ca[i]
        # Pack the accumulated field into the primvar at this frame's time.
        vt_arr = Vt.Vec4fArray(GRID_TOTAL)
        for k in range(GRID_TOTAL):
            vt_arr[k] = Gf.Vec4f(
                float(accum_density[k]),
                float(accum_color[k, 0]),
                float(accum_color[k, 1]),
                float(accum_color[k, 2]))
        paint_primvar.Set(vt_arr, t)

    stage.SetStartTimeCode(frame_times[0])
    stage.SetEndTimeCode(frame_times[-1])
    stage.SetTimeCodesPerSecond(FPS)

    nz = int((accum_density > 0).sum())
    print(f"[render] final 3D field: {nz}/{GRID_TOTAL} painted voxels "
          f"(density max={accum_density.max():.4f})")

    # The volume hit path sets payload.isVolume + accumulates the paint color
    # directly; the bound material is only a fallback (per-cell color comes
    # from the field). Bind a neutral one so the rprim has a material id.
    mat = UsdShade.Material.Define(stage, "/PaintMaterial")
    shader = UsdShade.Shader.Define(stage, "/PaintMaterial/Shader")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.85, 0.25, 0.18))
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.55)
    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(vol.GetPrim()).Bind(mat)

    # Paper ground plane at the stroke's Z floor.
    pz = float(pmin[2]) - 0.0005
    pm = float(margin_xy)
    px0, px1 = float(grid_min[0]) - pm, float(grid_max[0]) + pm
    py0, py1 = float(grid_min[1]) - pm, float(grid_max[1]) + pm
    paper = UsdGeom.Mesh.Define(stage, "/Paper")
    paper.CreatePointsAttr().Set(Vt.Vec3fArray([
        Gf.Vec3f(float(px0), float(py0), float(pz)),
        Gf.Vec3f(float(px1), float(py0), float(pz)),
        Gf.Vec3f(float(px1), float(py1), float(pz)),
        Gf.Vec3f(float(px0), float(py1), float(pz))]))
    paper.CreateFaceVertexCountsAttr().Set([4])
    paper.CreateFaceVertexIndicesAttr().Set([0, 1, 2, 3])
    paper.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    nrm = Gf.Vec3f(0.0, 0.0, 1.0)
    paper.CreateNormalsAttr().Set(Vt.Vec3fArray([nrm, nrm, nrm, nrm]))
    paper.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)
    paper_mat = UsdShade.Material.Define(stage, "/PaperMaterial")
    paper_shader = UsdShade.Shader.Define(stage, "/PaperMaterial/Shader")
    paper_shader.CreateIdAttr("UsdPreviewSurface")
    paper_shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.92, 0.90, 0.85))
    paper_shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.8)
    paper_mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(paper_shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(paper.GetPrim()).Bind(paper_mat)

    # Camera: 3/4 view above the stroke center.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(50.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 100.0))
    canvas_size = float(max(grid_extent[0], grid_extent[1]))
    eye = np.array([grid_center_world[0] + canvas_size * 0.4,
                    grid_center_world[1] - canvas_size * 1.4,
                    grid_center_world[2] + canvas_size * 2.0])
    target = np.array(grid_center_world)
    up = np.array([0.0, 0.0, 1.0])
    fwd = target - eye
    fwd = fwd / np.linalg.norm(fwd)
    right = np.cross(fwd, up); right = right / np.linalg.norm(right)
    up2 = np.cross(right, fwd)
    m = Gf.Matrix4d(); m.SetIdentity()
    m.SetRow(0, Gf.Vec4d(*right.tolist(), 0.0))
    m.SetRow(1, Gf.Vec4d(*up2.tolist(), 0.0))
    m.SetRow(2, Gf.Vec4d(*(-fwd).tolist(), 0.0))
    m.SetRow(3, Gf.Vec4d(*eye.tolist(), 1.0))
    UsdGeom.Xformable(cam).AddTransformOp().Set(m)

    # DistantLight from above-front.
    light_xf = Gf.Matrix4d(); light_xf.SetIdentity()
    light_xf.SetRow(2, Gf.Vec4d(-0.3, 0.4, -0.85, 0.0))
    UsdLux.DistantLight.Define(stage, "/Sun")
    sun = UsdLux.DistantLight.Get(stage, "/Sun")
    sun.CreateIntensityAttr().Set(3.0)
    UsdGeom.Xformable(sun).AddTransformOp().Set(light_xf)

    stage.GetRootLayer().Save()
    print(f"[render] scene: {scene_path.name} ({len(frame_times)} frames)")
    return frame_times


# ---------------------------------------------------------------------------
# Stage 3: in-process HydraRenderer.render(t) loop -> PNG.
# ---------------------------------------------------------------------------
def _locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = (ROOT / "Assets" / "Hd_RUZINO_RendererPlugin"
                / "render_nodes_save.json")
    if fallback.exists():
        return fallback
    sys.exit("render node config not found (render_nodes.json)")


def render_loop(scene_path: Path, frame_times):
    import hd_RUZINO_py as renderer
    import nodes_core_py as core
    from PIL import Image

    WIDTH, HEIGHT, SPP = 640, 480, 32
    OUTPUT_DIR.mkdir(exist_ok=True)
    for old in OUTPUT_DIR.glob("frame_*.png"):
        old.unlink()

    hydra = renderer.HydraRenderer(str(scene_path), WIDTH, HEIGHT)
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_render_cfg()))
    node_system.init()
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture"); rng.ui_name = "RNG"
    ray_gen = tree.add_node("node_render_ray_generation"); ray_gen.ui_name = "RayGen"
    # Use the wetbrush_render node (path_tracing + 2 volume hit groups) instead
    # of path_tracing, so the Hd_RUZINO_WetbrushVolume density slab is hit.
    path_trace = tree.add_node("wetbrush_render"); path_trace.ui_name = "WetbrushRender"
    accumulate = tree.add_node("accumulate"); accumulate.ui_name = "Accumulate"
    rng_buffer = tree.add_node("rng_buffer"); rng_buffer.ui_name = "RNGBuffer"
    present = tree.add_node("present_color"); present.ui_name = "Present"
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

    print(f"[render] rendering {len(frame_times)} frames "
          f"({WIDTH}x{HEIGHT}, {SPP} spp) in-process -> {OUTPUT_DIR.name}/")
    for i, t in enumerate(frame_times):
        for _ in range(SPP):
            hydra.render(float(t))
        tex = hydra.get_output_texture()
        if not tex or len(tex) != WIDTH * HEIGHT * 4:
            print(f"  [warn] frame {i} (t={t:.4f}): bad texture len "
                  f"{len(tex) if tex else 0}")
            continue
        img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
        rgb = np.clip(img[:, :, :3], 0.0, 1.0)
        rgb = (rgb * 255).astype(np.uint8)
        rgb = np.flipud(rgb)
        Image.fromarray(rgb).save(OUTPUT_DIR / f"frame_{i:04d}.png")
        print(f"  frame {i:3d}/{len(frame_times)-1} (t={t:.4f}) saved "
              f"lit={float(rgb.max(axis=2).mean()/255)*100:4.1f}%")

    hydra.stop()
    n = len(list(OUTPUT_DIR.glob("frame_*.png")))
    print(f"[render] done: {n} frames in {OUTPUT_DIR}")


def main():
    sim_usd = HERE / "data" / "output" / "wetbrush_render_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print("[render] stage 1: running streaming zone for "
          f"{NUM_FRAMES} frames -> {sim_usd.name}")
    sim_stage, prim_path, _sim_graph = run_streaming_zone(sim_usd)

    scene = BIN / "wetbrush_render.usdc"
    print(f"[render] stage 2: baking render scene -> {scene.name}")
    frame_times = bake_render_scene(sim_stage, prim_path, scene)

    print("[render] stage 3: in-process render loop")
    render_loop(scene, frame_times)


if __name__ == "__main__":
    main()
