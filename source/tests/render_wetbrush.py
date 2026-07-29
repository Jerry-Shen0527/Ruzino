#!/usr/bin/env python3
"""
Render the Wetbrush streaming-zone paint animation with the Ruzino path tracer
via the ZERO-COPY GPU buffer path (no bake / no CPU readback / no USD primvar).

Architecture (interleaved per-frame animation):
  Stage 1: build the streaming Wetbrush zone graph (mock_stroke -> simulation_in
           -> brush_wb_deposit -> bristle -> fluid -> commit -> simulation_out,
           feedback) and a marker render scene (UsdVolVolume carrying grid
           metadata primvars but NO paintField, plus the Paper mesh, camera,
           lights). The graph is NOT driven here.
  Stage 2: interleave { stage.tick(dt) -> hydra.render(t) x SPP -> save PNG }
           for NUM_FRAMES. Each tick, the commit node packs density+color into a
           Float4 GPU buffer and registers it in SharedGPUBufferRegistry under
           "wetbrush_paint_field"; the Hd_RUZINO_WetbrushVolume rprim's Sync()
           detects the version bump and rebinds the SAME buffer (zero copy).
           No data crosses back to the CPU.

The volume is rendered by the custom Hd_RUZINO_WetbrushVolume rprim, raymarched
by the VolumeClosestHit shader (paper Section 6: first-cross + penetration blend
+ Lambertian + 64-ray ambient occlusion).

Run from Binaries/Release (so node-plugin DLLs resolve):

    python ../../source/tests/render_wetbrush.py
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

NUM_FRAMES = 60
FPS = 60.0
DT = 1.0 / FPS

OUTPUT_DIR = BIN / "wetbrush_sequence"

# Sim grid parameters — MUST match what build_sim_graph configures on the
# deposit node, because the marker scene's gridResX/Y/Z + cellSize primvars
# describe the SAME grid the sim packs into the registry buffer.
SIM_RES = 4096
SIM_RES_Z = 64
SIM_PAPER = 1.0
CELL_SZ = SIM_PAPER / SIM_RES


# ---------------------------------------------------------------------------
# Stage 1a: build the streaming Wetbrush zone graph. Returns (graph, stage,
# prim_path). The graph is NOT driven — caller interleaves tick()+render().
# The returned graph must be kept alive for the whole render loop so the sim's
# GPU buffers (incl. the registry-registered packed_paint) are not freed.
# ---------------------------------------------------------------------------
def build_sim_graph(sim_usd: Path):
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
    # commit's Paint Field 3D output still feeds write_usd so the sim USD has
    # a populated prim for downstream inspection, but the renderer does NOT
    # read it — it consumes the zero-copy registry buffer.
    g.addEdge(commit, "Paint Field 3D", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")
    g.addEdge(commit, "Stroke Curves", sim_out, "Simulation In")

    g.setSocketDefaults({
        (mock, "Num Points"): 30, (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        # Resolution 4096 (paper Section 4.2: "we typically set the grid
        # resolution to 4096x4096x64"). At lower resolutions the brush
        # footprint covered too few cells, so trilinear filtering + the
        # 2-cell gradient normal rode cell-boundary density steps and the
        # Lambertian shading flickered across the stroke.
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
# Stage 1b: build the marker render scene. UsdVolVolume with grid metadata
# primvars (NO paintField — the rprim reads paint from the registry buffer),
# plus the Paper mesh, camera, and lights. Returns the scene path.
# ---------------------------------------------------------------------------
def build_marker_scene(scene_path: Path):
    if scene_path.exists():
        scene_path.unlink()
    stage = Usd.Stage.CreateNew(str(scene_path))

    # UsdVolVolume -> Hydra token "volume" -> Hd_RUZINO_WetbrushVolume.
    # The metadata primvars describe the SAME grid the sim packs. The rprim's
    # Sync() reads them, then create_gpu_resources() Phase 1 overrides the
    # buffer source with the registry buffer (and re-asserts the same grid
    # geometry from the registry metadata blob).
    vol = UsdVol.Volume.Define(stage, "/BrushPaint")
    pv = UsdGeom.PrimvarsAPI(vol)
    pv.CreatePrimvar("gridResX", Sdf.ValueTypeNames.Int).Set(int(SIM_RES))
    pv.CreatePrimvar("gridResY", Sdf.ValueTypeNames.Int).Set(int(SIM_RES))
    pv.CreatePrimvar("gridResZ", Sdf.ValueTypeNames.Int).Set(int(SIM_RES_Z))
    pv.CreatePrimvar("cellSize", Sdf.ValueTypeNames.Float).Set(float(CELL_SZ))
    # gridMin matches the sim's grid layout EXACTLY — see node_brush_wb_commit.cpp
    # PaintFieldMeta: gridMinZ = grid_center_z - grid_height/2 = canvas_z (since
    # grid_center_z = canvas_z + height/2). Canvas Z defaults to 0 in the
    # deposit node, so paint volume occupies Z in [0, grid_height]. The paper
    # mesh sits just below at Z = -0.0005. (A previous version used
    # -grid_height/2 here, which mismatched the registry metadata by 32 cells
    # in Z and smeared the rendered paint.)
    grid_height = SIM_PAPER * SIM_RES_Z / SIM_RES
    canvas_z = 0.0
    gm = Gf.Vec3f(-SIM_PAPER * 0.5, -SIM_PAPER * 0.5, float(canvas_z))
    pv.CreatePrimvar("gridMin", Sdf.ValueTypeNames.Float3).Set(gm)

    # Neutral fallback material (the volume hit path colors from the field).
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

    # Paper = the whole canvas (not just a frame around the stroke bbox). The
    # volume's empty cells are transparent (VolumeIntersection only reports on
    # paint), so the paper underneath shows through and the paint reads as
    # painted ON the paper.
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
    paper_shader.CreateIdAttr("UsdPreviewSurface")
    paper_shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        (0.92, 0.90, 0.85))
    paper_shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.8)
    paper_mat.CreateSurfaceOutput().ConnectToSource(
        UsdShade.ConnectableAPI(paper_shader), "surface",
        UsdShade.AttributeType.Output)
    UsdShade.MaterialBindingAPI.Apply(paper.GetPrim()).Bind(paper_mat)

    # Camera: tight 3/4 view framing the stroke region (the strokes span
    # ~0.3×0.3 around the origin — `length`=0.3 in mock_strokes). Aiming at the
    # whole 1.0 canvas from ~2.0 away left the brush a tiny speck; pull in to
    # ~0.55 distance and target the stroke center so the paint fills the frame.
    # Paper mesh still extends ±0.5 so it shows as the paper around the stroke.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(50.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 100.0))
    frame_size = 0.35  # roughly the stroke span + a little breathing room
    cx = cy = cz = 0.0
    eye = np.array([cx + frame_size * 0.5,
                    cy - frame_size * 1.1,
                    cz + frame_size * 1.5])
    target = np.array([cx, cy, cz])
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

    # Key DistantLight from above-front. intensity=3.0 — paper reads near its
    # diffuseColor (0.92,0.90,0.85) without saturating once the dome fill is
    # added underneath.
    light_xf = Gf.Matrix4d(); light_xf.SetIdentity()
    light_xf.SetRow(2, Gf.Vec4d(-0.3, 0.4, -0.85, 0.0))
    UsdLux.DistantLight.Define(stage, "/Sun")
    sun = UsdLux.DistantLight.Get(stage, "/Sun")
    sun.CreateIntensityAttr().Set(3.0)
    UsdGeom.Xformable(sun).AddTransformOp().Set(light_xf)

    # Dome light — fills the background off black; dim so the warm key light
    # still dominates the paper.
    dome = UsdLux.DomeLight.Define(stage, "/Dome")
    dome.CreateIntensityAttr().Set(0.25)
    dome.CreateColorAttr().Set((0.6, 0.75, 1.0))

    stage.GetRootLayer().Save()
    print(f"[render] marker scene: {scene_path.name}")
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
    # wetbrush_render = path_tracing + 2 procedural volume hit groups, so the
    # Hd_RUZINO_WetbrushVolume density slab is hit.
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

    print(f"[render] interleaved {NUM_FRAMES} frames "
          f"({WIDTH}x{HEIGHT}, {SPP} spp) -> {OUTPUT_DIR.name}/")
    for i in range(NUM_FRAMES):
        t = (i + 1) * DT
        # Drive one sim step FIRST so the registry holds this frame's packed
        # paint before the rprim's Sync() runs during render().
        stage.set_render_time(t)
        stage.tick(DT)
        stage.finish_tick()
        # Belt-and-suspenders: the auto path (renderer.cpp polls the registry
        # version → DirtyGeometry → wetbrush_render geom_dirty → reset) should
        # already trigger a clean reset for this fresh sim frame. This explicit
        # host request is the escape hatch in case that auto path ever lags.
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
    print(f"[render] done: {n} frames in {OUTPUT_DIR}")


def main():
    sim_usd = BIN / "wetbrush_render_sim.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    print("[render] stage 1a: building sim graph (zero-copy, no 60-frame drive)")
    sim_graph, stage, prim_path = build_sim_graph(sim_usd)

    scene = BIN / "wetbrush_render.usdc"
    print(f"[render] stage 1b: building marker render scene -> {scene.name}")
    build_marker_scene(scene)

    print("[render] stage 2: interleaved sim+render loop")
    # sim_graph MUST stay alive through the loop — it owns the GPU buffers the
    # rprim reads zero-copy from the registry.
    run_interleaved(scene, stage, sim_graph)


if __name__ == "__main__":
    main()
