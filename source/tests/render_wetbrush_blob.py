#!/usr/bin/env python3
"""
Blob settling test — does deposited paint behave like a liquid once the brush
lifts?

Scenario (per user request):
  1. Press the brush at the center for ~0.3 s (a dipped brush deposits a blob
     of ink inside the active window).
  2. Lift the brush to z=0.08 over ~0.15 s — above the bristle extent
     (radius*1.5 = 0.03) and the D0 zone (1.8*radius = 0.036) — then the
     trajectory ends: the emitter freezes here and goes pen-up, and the
     active window stays at the press XY (the window follows the brush).
  3. Watch the blob for another ~1 s: does it settle/spread like a fluid
     (e.g. under gravity), or does it hover frozen?

The camera is a SIDE view (profile) so the Z distribution of the paint is
directly visible — the "hovering ink" question is about Z. The brush bristle
context (/BrushBristles) is kept ON as the position reference.

Run from Binaries/Release:

    python ../../source/tests/render_wetbrush_blob.py
    WETBRUSH_RES=1024 python ../../source/tests/render_wetbrush_blob.py
"""
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

NUM_FRAMES = 90          # 27 press+lift, 63 pen-up observation
FPS = 60.0
DT = 1.0 / FPS

OUTPUT_DIR = BIN / "wetbrush_blob_sequence"

SIM_RES = int(os.environ.get("WETBRUSH_RES", "1024"))
SIM_RES_Z = int(os.environ.get("WETBRUSH_RES_Z", "64"))
SIM_PAPER = 1.0
CELL_SZ = SIM_PAPER / SIM_RES

# Trajectory (seconds at 30 samples/s inside mock_press_lift): press 0.3 s at
# z=0 (full press — the bristles, length 0.03, squash onto the canvas), then
# lift to z=0.08 over 0.15 s (clear of the bristle extent AND the D0 zone).
PRESS_Z = 0.0
LIFT_Z = 0.08
PRESS_DUR = 0.3
LIFT_DUR = 0.15


def build_sim_graph(sim_usd: Path):
    g = RuzinoGraph("WetbrushBlob")
    g.loadConfiguration(str(BIN / "geometry_nodes.json"))

    mock = g.createNode("mock_press_lift", name="PressLift")
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
    g.addEdge(emitter, "Stroke Sample", deposit, "Stroke Sample")
    g.addEdge(sim_in, "Simulation Out", deposit, "State")
    g.addEdge(deposit, "Stroke Sample", fluid, "Stroke Sample")
    g.addEdge(deposit, "State", bristle, "State")
    g.addEdge(bristle, "State", fluid, "State")
    g.addEdge(fluid, "State", commit, "State")
    g.addEdge(sim_in, "Simulation Out", commit, "Stroke Curves")
    g.addEdge(commit, "Paint Field 3D", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Center X"): 0.0, (mock, "Center Y"): 0.0,
        (mock, "Press Z"): PRESS_Z, (mock, "Lift Z"): LIFT_Z,
        (mock, "Press Duration"): PRESS_DUR,
        (mock, "Lift Duration"): LIFT_DUR,
        (deposit, "Resolution"): SIM_RES, (deposit, "Resolution Z"): SIM_RES_Z,
        (deposit, "Paper Size"): SIM_PAPER,
        (deposit, "Brush Radius"): 0.02, (deposit, "Brush Pressure"): 1.0,
        (deposit, "Ink Amount"): 0.8,
        (bristle, "Brush Radius"): 0.02,
        (fluid, "Viscosity"): 0.5, (fluid, "Diffusion Rate"): 0.0001,
        (fluid, "Drying Rate"): 0.1, (fluid, "Brush Radius"): 0.02,
    })
    assert sim_in.paired_node is sim_out, "zone pairing not established"

    if sim_usd.exists():
        sim_usd.unlink()
    stage = stage_py.Stage(str(sim_usd))
    prim_path = "/Brush"
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), prim_path)  # placeholder prim
    g.apply_to_stage(stage, prim_path)

    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    return g, stage, prim_path


def add_registry_points(stage, path, key, color):
    pts = UsdGeom.Points.Define(stage, path)
    pts.CreatePointsAttr().Set([Gf.Vec3f(0.0, 0.0, -10.0)])
    pv = UsdGeom.PrimvarsAPI(pts.GetPrim())
    pv.CreatePrimvar("debugKey", Sdf.ValueTypeNames.String).Set(key)
    widths = pts.CreateWidthsAttr()
    for i in range(NUM_FRAMES):
        widths.Set(Vt.FloatArray([0.001 * (i + 1)]), (i + 1) * DT)
    mat = UsdShade.Material.Define(stage, f"{path}Material")
    shader = UsdShade.Shader.Define(stage, f"{path}Material/Shader")
    shader.CreateIdAttr().Set("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(color)
    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(pts.GetPrim()).Bind(mat)
    return pts


def build_marker_scene(scene_path: Path):
    if scene_path.exists():
        scene_path.unlink()
    stage = Usd.Stage.CreateNew(str(scene_path))

    vol = UsdVol.Volume.Define(stage, "/BrushPaint")
    pv = UsdGeom.PrimvarsAPI(vol)
    pv.CreatePrimvar("gridResX", Sdf.ValueTypeNames.Int).Set(int(SIM_RES))
    pv.CreatePrimvar("gridResY", Sdf.ValueTypeNames.Int).Set(int(SIM_RES))
    pv.CreatePrimvar("gridResZ", Sdf.ValueTypeNames.Int).Set(int(SIM_RES_Z))
    pv.CreatePrimvar("cellSize", Sdf.ValueTypeNames.Float).Set(float(CELL_SZ))
    grid_height = SIM_PAPER * SIM_RES_Z / SIM_RES
    canvas_z = 0.0
    gm = Gf.Vec3f(-SIM_PAPER * 0.5, -SIM_PAPER * 0.5, float(canvas_z))
    pv.CreatePrimvar("gridMin", Sdf.ValueTypeNames.Float3).Set(gm)

    # Brush context ON — the stationary brush is the position/height reference
    # the observer asked for. Particles opt-in like the main script.
    if os.environ.get("WB_DRAW_PARTICLES", "0") == "1":
        add_registry_points(stage, "/LiquidParticles",
                            "wetbrush_debug_particles", (0.8, 0.3, 0.3))
    if os.environ.get("WB_DRAW_BRISTLES", "1") == "1":
        add_registry_points(stage, "/BrushBristles",
                            "wetbrush_debug_bristles", (0.7, 0.7, 0.75))

    mat = UsdShade.Material.Define(stage, "/PaintMaterial")
    shader = UsdShade.Shader.Define(stage, "/PaintMaterial/Shader")
    shader.CreateIdAttr().Set("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.85, 0.25, 0.18))
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.55)
    mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(vol.GetPrim()).Bind(mat)

    pz = float(canvas_z) - 0.0005
    paper = UsdGeom.Mesh.Define(stage, "/Paper")
    paper.CreatePointsAttr().Set(Vt.Vec3fArray([
        Gf.Vec3f(-SIM_PAPER * 0.5, -SIM_PAPER * 0.5, pz),
        Gf.Vec3f( SIM_PAPER * 0.5, -SIM_PAPER * 0.5, pz),
        Gf.Vec3f( SIM_PAPER * 0.5,  SIM_PAPER * 0.5, pz),
        Gf.Vec3f(-SIM_PAPER * 0.5,  SIM_PAPER * 0.5, pz)]))
    paper.CreateFaceVertexCountsAttr().Set([4])
    paper.CreateFaceVertexIndicesAttr().Set([0, 1, 2, 3])
    paper.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    nrm = Gf.Vec3f(0.0, 0.0, 1.0)
    paper.CreateNormalsAttr().Set(Vt.Vec3fArray([nrm, nrm, nrm, nrm]))
    paper.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)
    paper_mat = UsdShade.Material.Define(stage, "/PaperMaterial")
    paper_shader = UsdShade.Shader.Define(stage, "/PaperMaterial/Shader")
    paper_shader.CreateIdAttr().Set("UsdPreviewSurface")
    paper_shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.92, 0.90, 0.85))
    paper_shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.8)
    paper_mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(paper_shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(paper.GetPrim()).Bind(paper_mat)

    # SIDE VIEW: X horizontal, Z vertical — the blob's Z profile (settling,
    # hovering) is directly readable. Slight X offset for a depth cue; the
    # frame (~0.16) covers the blob (~0.05 footprint), the volume height
    # (0.0625) and the lifted brush (z up to 0.08+bristles).
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(50.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 100.0))
    frame_size = 0.16
    eye = np.array([frame_size * 0.35, -frame_size * 2.6,
                    frame_size * 0.45])
    target = np.array([0.0, 0.0, 0.03])
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

    light_xf = Gf.Matrix4d(); light_xf.SetIdentity()
    light_xf.SetRow(2, Gf.Vec4d(-0.3, 0.4, -0.85, 0.0))
    UsdLux.DistantLight.Define(stage, "/Sun")
    sun = UsdLux.DistantLight.Get(stage, "/Sun")
    sun.CreateIntensityAttr().Set(3.0)
    UsdGeom.Xformable(sun).AddTransformOp().Set(light_xf)

    dome = UsdLux.DomeLight.Define(stage, "/Dome")
    dome.CreateIntensityAttr().Set(0.25)
    dome.CreateColorAttr().Set((0.6, 0.75, 1.0))

    stage.GetRootLayer().Save()
    print(f"[blob] marker scene: {scene_path.name}")
    return scene_path


def _locate_render_cfg():
    primary = BIN / "render_nodes.json"
    if primary.exists():
        return primary
    fallback = (ROOT / "Assets" / "Hd_RUZINO_RendererPlugin"
                / "render_nodes_save.json")
    if fallback.exists():
        return fallback
    sys.exit("render node config not found (render_nodes.json)")


def run_interleaved(scene_path: Path, stage, sim_graph):
    import hd_RUZINO_py as renderer
    import nodes_core_py as core
    from PIL import Image

    WIDTH, HEIGHT, SPP = 1280, 960, 32
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

    print(f"[blob] interleaved {NUM_FRAMES} frames "
          f"({WIDTH}x{HEIGHT}, {SPP} spp) -> {OUTPUT_DIR.name}/")
    for i in range(NUM_FRAMES):
        t = (i + 1) * DT
        stage.set_render_time(t)
        stage.tick(DT)
        stage.finish_tick()
        hydra.reset_accumulation()
        for _ in range(SPP):
            hydra.render(float(t))
        tex = hydra.get_output_texture()
        if not tex or len(tex) != WIDTH * HEIGHT * 4:
            print(f"  [warn] frame {i}: bad texture len "
                  f"{len(tex) if tex else 0}")
            continue
        img = np.asarray(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
        rgb = np.clip(img[:, :, :3], 0.0, 1.0)
        rgb = (rgb * 255).astype(np.uint8)
        rgb = np.flipud(rgb)
        Image.fromarray(rgb).save(OUTPUT_DIR / f"frame_{i:04d}.png")
        print(f"  frame {i:3d}/{NUM_FRAMES-1} (t={t:.4f}) saved")

    hydra.stop()
    n = len(list(OUTPUT_DIR.glob("frame_*.png")))
    print(f"[blob] done: {n} frames in {OUTPUT_DIR}")


def main():
    sim_usd = BIN / "wetbrush_blob_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    for stale in (sim_usd, BIN / "wetbrush_blob_sim_modifiers.usdc"):
        if stale.exists():
            stale.unlink()
    print("[blob] stage 1a: building sim graph (press-and-lift trajectory)")
    sim_graph, stage, prim_path = build_sim_graph(sim_usd)

    scene = BIN / "wetbrush_blob.usdc"
    print(f"[blob] stage 1b: building marker render scene -> {scene.name}")
    build_marker_scene(scene)

    print("[blob] stage 2: interleaved sim+render loop")
    run_interleaved(scene, stage, sim_graph)


if __name__ == "__main__":
    main()
