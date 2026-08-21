#!/usr/bin/env python3
"""
Wetbrush DEBUG visualization: render the raw simulation state directly — no
paper §6 rendering pipeline, no SDF, no volume raymarch.

What you see (all zero-copy GPU buffers packed by brush_wb_commit each tick):

  /DebugBristles  the 600 bristle chains as capsule segments ("线段/圆柱").
                  Color = the liquid each segment currently carries: dry =
                  neutral gray, loaded = pigment color (RYB->RGB).
  /DebugParticles the active-window FLIP/PIC particles as small spheres,
                  colored by their pigment.
  /DebugVoxels    every non-zero global-grid voxel as a small sphere,
                  colored by its normalized pigment (premultiplied RYB /
                  density).

Data path: brush_wb_commit -> debug_pack_*.slang -> SharedGPUBufferRegistry
(keys wetbrush_debug_{bristles,particles,voxels}) -> Hd_RUZINO_Points
("debugKey" primvar) -> AABB BLAS -> sphere/capsule procedural hit groups
(inline debugShade, no material system).

Usage (from Binaries/Release):

    python ../../source/tests/render_wetbrush_debug.py

Env knobs:
    WB_DEBUG_RES=512 WB_DEBUG_RES_Z=32   sim grid resolution (debug default
                                          512x512x32; paper is 1.0 so a cell
                                          is 1/512 ~= brush_radius/10)
    WB_DEBUG_DRAW=bristles,particles,voxels   comma list to include
    WB_DEBUG_FRAMES=60 WB_DEBUG_SPP=8
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

from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt  # noqa: E402

import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

NUM_FRAMES = int(os.environ.get("WB_DEBUG_FRAMES", "60"))
FPS = 60.0
DT = 1.0 / FPS
SPP = int(os.environ.get("WB_DEBUG_SPP", "8"))
DRAW = os.environ.get("WB_DEBUG_DRAW", "bristles,particles,voxels").split(",")
DRAW = [d.strip() for d in DRAW if d.strip()]

OUTPUT_DIR = BIN / "wetbrush_debug_sequence"

# Debug-friendly grid: 512x512x32 (cell ~= brush_radius/10) — small enough to
# iterate fast, fine enough that the brush footprint covers ~10x10 cells.
SIM_RES = int(os.environ.get("WB_DEBUG_RES", "512"))
SIM_RES_Z = int(os.environ.get("WB_DEBUG_RES_Z", "32"))
SIM_PAPER = 1.0


# ---------------------------------------------------------------------------
# Stage 1a: the streaming Wetbrush sim zone (same graph as render_wetbrush.py;
# the commit node's debug-draw pack runs every tick as part of the zone).
# ---------------------------------------------------------------------------
def build_sim_graph(sim_usd: Path):
    g = RuzinoGraph("WetbrushDebug")
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
    g.addEdge(commit, "Paint Field 3D", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        # One trajectory point per frame (the emitter synthesizes 1/60s
        # spacing per point): the stroke must span the whole run or the
        # trajectory exhausts mid-run, the pen lifts, and everything freezes
        # (that looked like a sim bug on 2026-08-18 — it was the fixture).
        (mock, "Num Points"): NUM_FRAMES, (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
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

    # The three simulation gates (AGENTS.md Section "Simulation").
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path(prim_path))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    return g, stage, prim_path


# ---------------------------------------------------------------------------
# Stage 1b: the marker render scene. Three UsdGeom.Points prims whose
# "debugKey" primvar routes them to a SharedGPUBufferRegistry buffer; plus the
# paper quad, camera, and lights. NO volume prim — this is the whole point:
# we render the raw sim state, not the §6 paint surface.
# ---------------------------------------------------------------------------
def add_debug_points(stage, path, key, color):
    pts = UsdGeom.Points.Define(stage, path)
    # One placeholder point keeps the USD data valid; the registry buffer
    # supplies the real geometry once the sim ticks.
    pts.CreatePointsAttr().Set([Gf.Vec3f(0.0, 0.0, -10.0)])
    pv = UsdGeom.PrimvarsAPI(pts.GetPrim())
    pv.CreatePrimvar("debugKey", Sdf.ValueTypeNames.String).Set(key)
    # Per-frame re-sync driver: time samples on the builtin `widths` attr make
    # UsdImaging re-dirty this prim at every render-time change, so the
    # rprim's Sync re-checks the registry version and rebuilds the (moving)
    # point cloud. Without it Hydra would sync the prim once (the stage
    # itself never changes) and the debug geometry would freeze at frame 1.
    # The values are never read — the registry supplies real radii.
    widths = pts.CreateWidthsAttr()
    for i in range(NUM_FRAMES):
        widths.Set(Vt.FloatArray([0.001 * (i + 1)]), (i + 1) * DT)
    # Neutral material — the debug hit groups never consult it, but the
    # rprim's TLAS update expects one to exist.
    mat = UsdShade.Material.Define(stage, f"{path}Material")
    shader = UsdShade.Shader.Define(stage, f"{path}Material/Shader")
    shader.CreateIdAttr("UsdPreviewSurface")
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

    if "ghost" in DRAW:
        # Isolation probe: a debug prim whose registry key never exists. Its
        # Sync runs + marks geometry dirty every frame but contributes no
        # geometry — separates "TLAS rebuild churn" effects from actual
        # capsule/particle rendering.
        add_debug_points(stage, "/DebugGhost",
                         "wetbrush_debug_nonexistent", (0.9, 0.9, 0.2))
    if "bristles" in DRAW:
        add_debug_points(stage, "/DebugBristles",
                         "wetbrush_debug_bristles", (0.7, 0.7, 0.75))
    if "particles" in DRAW:
        add_debug_points(stage, "/DebugParticles",
                         "wetbrush_debug_particles", (0.8, 0.3, 0.3))
    if "voxels" in DRAW:
        add_debug_points(stage, "/DebugVoxels",
                         "wetbrush_debug_voxels", (0.3, 0.5, 0.9))

    # Paper reference plane at the canvas (z = 0, canvas_z default). Slightly
    # warm gray so pigment colors pop against it.
    pz = -0.0005
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
    paper_shader.CreateIdAttr("UsdPreviewSurface")
    paper_shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.72, 0.70, 0.66))
    paper_shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.8)
    paper_mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(paper_shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(paper.GetPrim()).Bind(paper_mat)

    # Camera: 3/4 view of the stroke region. The paint slab is z in
    # [0, SIM_PAPER*SIM_RES_Z/SIM_RES] and the brush hovers just above it —
    # aim at the slab mid-height so both brush and paint are in frame.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(50.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 100.0))
    frame_size = 0.35
    cz = SIM_PAPER * SIM_RES_Z / SIM_RES * 0.5
    eye = np.array([frame_size * 0.5, -frame_size * 1.1, cz + frame_size * 1.2])
    target = np.array([0.0, 0.0, cz])
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

    # Key light + soft dome (same rig as render_wetbrush.py).
    light_xf = Gf.Matrix4d(); light_xf.SetIdentity()
    light_xf.SetRow(2, Gf.Vec4d(-0.3, 0.4, -0.85, 0.0))
    UsdLux.DistantLight.Define(stage, "/Sun")
    sun = UsdLux.DistantLight.Get(stage, "/Sun")
    sun.CreateIntensityAttr().Set(3.0)
    UsdGeom.Xformable(sun).AddTransformOp().Set(light_xf)

    dome = UsdLux.DomeLight.Define(stage, "/Dome")
    dome.CreateIntensityAttr().Set(0.25)
    dome.CreateColorAttr().Set((0.6, 0.75, 1.0))

    # Declare the stage's animation span so UsdImaging tracks the time-sampled
    # `widths` on the debug prims as varying (render_gridbox.py does the same;
    # without it the delegate treats the stage as static and never re-dirties
    # the prims on render-time changes).
    stage.SetStartTimeCode(DT)
    stage.SetEndTimeCode(NUM_FRAMES * DT)
    stage.SetTimeCodesPerSecond(FPS)

    stage.GetRootLayer().Save()
    print(f"[debug] marker scene: {scene_path.name} (draw={DRAW})")
    return scene_path


# ---------------------------------------------------------------------------
# Stage 2: interleaved { tick(dt) -> render(t) x SPP -> save PNG }.
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


def run_interleaved(scene_path: Path, stage, sim_graph):
    import hd_RUZINO_py as renderer
    import nodes_core_py as core
    from PIL import Image

    WIDTH, HEIGHT = 1280, 960
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
    # wetbrush_render carries the debug sphere/capsule hit groups in addition
    # to triangles + volume.
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

    print(f"[debug] interleaved {NUM_FRAMES} frames "
          f"({WIDTH}x{HEIGHT}, {SPP} spp) -> {OUTPUT_DIR.name}/")
    for i in range(NUM_FRAMES):
        t = (i + 1) * DT
        # Sim first: commit packs the three debug buffers and bumps their
        # registry versions; the points rprims' per-frame Sync rebuilds.
        stage.set_render_time(t)
        stage.tick(DT)
        stage.finish_tick()
        hydra.reset_accumulation()
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
        print(f"  frame {i:3d}/{NUM_FRAMES-1} (t={t:.4f}) saved "
              f"lit={float(rgb.max(axis=2).mean()/255)*100:4.1f}%")

    hydra.stop()
    n = len(list(OUTPUT_DIR.glob("frame_*.png")))
    print(f"[debug] done: {n} frames in {OUTPUT_DIR}")


def main():
    sim_usd = BIN / "wetbrush_debug_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print("[debug] stage 1a: building sim graph")
    sim_graph, stage, prim_path = build_sim_graph(sim_usd)

    scene = BIN / "wetbrush_debug.usdc"
    print(f"[debug] stage 1b: building marker render scene -> {scene.name}")
    build_marker_scene(scene)

    print("[debug] stage 2: interleaved sim+render loop")
    # sim_graph must stay alive — it owns the GPU buffers the rprims read.
    run_interleaved(scene, stage, sim_graph)


if __name__ == "__main__":
    main()
