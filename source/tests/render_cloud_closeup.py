#!/usr/bin/env python3
"""
Close-up cloud render: a SINGLE cloud just above the ground, camera close in.
Purpose: inspect the cloud's texture/shading detail at near distance — the
same scene zoomed out looks like a cartoon, so we want to see what's actually
there before tuning further.

Run from Binaries/Release:

    python ../../source/tests/render_cloud_closeup.py

Output: Binaries/Release/test_output/clouds/closeup.png
"""
import os
import sys
import math
import time
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


def _locate_render_cfg():
    primary = BIN / "render_nodes.json"
    return primary if primary.exists() else (
        ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json")


from pxr import Usd, UsdGeom, UsdLux, UsdShade, UsdVol, Sdf, Gf


def _look_at(eye, target, up_world=(0.0, 1.0, 0.0)):
    eye = Gf.Vec3d(*eye); target = Gf.Vec3d(*target); up_world = Gf.Vec3d(*up_world)
    fwd = target - eye; fwd.Normalize()
    right = Gf.Cross(fwd, up_world); right.Normalize()
    up = Gf.Cross(right, fwd)
    m = Gf.Matrix4d(); m.SetIdentity()
    m.SetRow(0, Gf.Vec4d(right[0], right[1], right[2], 0.0))
    m.SetRow(1, Gf.Vec4d(up[0], up[1], up[2], 0.0))
    m.SetRow(2, Gf.Vec4d(-fwd[0], -fwd[1], -fwd[2], 0.0))
    m.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    return m


def _build_scene(path):
    if path.exists():
        path.unlink()
    stage = Usd.Stage.CreateNew(str(path))

    # A single cloud volume sitting just above the ground. AABB ~ 80x40x80 m,
    # base at y=5 (a bit above the ground plane), so the camera can get close.
    # Noise is normalized over the AABB, so a small AABB = one cloud with a few
    # cells of detail (freq ~2-3 gives a couple of puffs).
    vol = UsdVol.Volume.Define(stage, "/Cloud")
    pv = UsdGeom.PrimvarsAPI(vol)
    pv.CreatePrimvar("volumeType", Sdf.ValueTypeNames.Token).Set("cloud")
    pv.CreatePrimvar("boundsMin", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(-40.0, 5.0, -40.0))
    pv.CreatePrimvar("boundsMax", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(40.0, 45.0, 40.0))
    # Physical extinction: sigma_t ~0.2/m so the 40 m-thick cloud reads as a
    # dense small cumulus (tau ~ 3-6 through the middle), not a wisp.
    pv.CreatePrimvar("coverage", Sdf.ValueTypeNames.Float).Set(0.15)
    pv.CreatePrimvar("densityScale", Sdf.ValueTypeNames.Float).Set(0.2)
    pv.CreatePrimvar("phaseG", Sdf.ValueTypeNames.Float).Set(0.7)
    pv.CreatePrimvar("layerTop", Sdf.ValueTypeNames.Float).Set(1.0)
    pv.CreatePrimvar("layerBottom", Sdf.ValueTypeNames.Float).Set(0.0)
    # High freq for visible fluff detail at close range: ~10 puffs per axis
    # over the 80m AABB => each puff ~8m, clearly resolvable at 60m camera
    # distance. Strong detail erosion to carve wispy curled edges (the
    # signature cumulus silhouette) instead of one smooth blob.
    pv.CreatePrimvar("noiseFreq", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(6.0, 4.0, 6.0))
    pv.CreatePrimvar("worleyFreq", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(6.0, 4.0, 6.0))
    pv.CreatePrimvar("detailErosion", Sdf.ValueTypeNames.Float).Set(0.8)

    # Sun — clear directional light from upper right.
    elev = math.radians(45.0)
    sun_shine = Gf.Vec3f(-math.cos(elev), -math.sin(elev), -0.3)
    sun_shine.Normalize()
    sun_xf = Gf.Matrix4d(); sun_xf.SetIdentity()
    sun_xf.SetRow(2, Gf.Vec4d(sun_shine[0], sun_shine[1], sun_shine[2], 0.0))
    sun = UsdLux.DistantLight.Define(stage, "/Sun")
    sun.CreateIntensityAttr().Set(2.5)
    sun.CreateAngleAttr().Set(0.53)
    UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)

    # Sky dome (Hosek) for ambient fill.
    sd = Gf.Vec3f(math.cos(elev), math.sin(elev), 0.3)
    sd.Normalize()
    dome = UsdLux.DomeLight.Define(stage, "/Sky")
    dome.CreateIntensityAttr().Set(1.0)
    dome.GetPrim().CreateAttribute(
        "inputs:shader_path", Sdf.ValueTypeNames.String).Set(
        "callables/eval_dome_light_hosek_wilkie.slang")
    dome.GetPrim().CreateAttribute("inputs:turbidity", Sdf.ValueTypeNames.Float).Set(3.0)
    dome.GetPrim().CreateAttribute("inputs:groundAlbedo", Sdf.ValueTypeNames.Float).Set(0.3)
    dome.GetPrim().CreateAttribute("inputs:sunDirection", Sdf.ValueTypeNames.Float3).Set(sd)

    # Small checkerboard ground (10x10 grid over ±200m).
    ground_mat = UsdShade.Material.Define(stage, "/GroundChecker")
    ground_mat.GetPrim().CreateAttribute(
        "config:shader_path", Sdf.ValueTypeNames.String).Set(
        "callables/checkerboard_ground.slang")
    surface_out = ground_mat.CreateSurfaceOutput()
    dummy = UsdShade.Shader.Define(stage, "/GroundChecker/PreviewSurface")
    dummy.CreateIdAttr("UsdPreviewSurface")
    dummy.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.5, 0.5, 0.5))
    surface_out.ConnectToSource(dummy.ConnectableAPI(),
                                "surface", UsdShade.AttributeType.Output)
    plane = UsdGeom.Mesh.Define(stage, "/Ground")
    DIV = 10
    HALF = 200.0
    step = (2.0 * HALF) / DIV
    pts = []
    idx = []
    for iz in range(DIV + 1):
        for ix in range(DIV + 1):
            pts.append((ix * step - HALF, 0.0, iz * step - HALF))
    for iz in range(DIV):
        for ix in range(DIV):
            base = iz * (DIV + 1) + ix
            idx.append(base); idx.append(base + 1)
            idx.append(base + (DIV + 1) + 1); idx.append(base + (DIV + 1))
    plane.CreatePointsAttr().Set(pts)
    plane.CreateFaceVertexCountsAttr().Set([4] * (DIV * DIV))
    plane.CreateFaceVertexIndicesAttr().Set(idx)
    plane.CreateNormalsAttr().Set([(0, 1, 0)] * len(pts))
    plane.SetNormalsInterpolation("vertex")
    plane.GetPrim().ApplyAPI("MaterialBindingAPI")
    UsdShade.MaterialBindingAPI(plane).Bind(ground_mat)

    # Camera: pulled back so the cloud no longer fills the whole frame (at
    # 60 m the 80 m-wide cloud filled the 54-deg FOV -> solid gray). At 130 m
    # the cloud subtends ~34 deg, leaving sky/ground around it for context.
    # Cloud spans y 5..45, so aim at y=28; eye level with the cloud middle.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(35.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 1e5))
    UsdGeom.Xformable(cam).AddTransformOp().Set(
        _look_at(eye=(0.0, 30.0, 130.0), target=(0.0, 28.0, 0.0)))

    stage.GetRootLayer().Save()
    return path


def _build_render_graph(hydra, samples):
    import nodes_core_py as core
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_render_cfg()))
    node_system.init()
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture")
    ray_gen = tree.add_node("node_render_ray_generation")
    path_trace = tree.add_node("path_tracing")
    accumulate = tree.add_node("accumulate")
    rng_buffer = tree.add_node("rng_buffer")
    lpm = tree.add_node("lpm")
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
                  lpm.get_input_socket("Input Color"))
    tree.add_link(lpm.get_output_socket("Output Color"),
                  present.get_input_socket("Color"))

    vec_params = {(lpm, "Crosstalk"): [0.471, 0.49, 0.504]}
    for (node, sn), val in vec_params.items():
        node.get_input_socket(sn).set_default_value(val)

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    scalar_params = {
        (ray_gen, "Aperture"): 0.0, (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
        (lpm, "HDR Max"): 4.0, (lpm, "LPM Exposure"): 2.0,
        (lpm, "Contrast"): 1.0, (lpm, "Shoulder"): 1.0,
        (lpm, "Shoulder Contrast"): 1.0, (lpm, "Soft Gap"): 0.0,
        (lpm, "Color Space"): 0, (lpm, "Display Mode"): 0,
        (lpm, "Display Max Luminance"): 1000.0, (lpm, "Display Min Luminance"): 0.0,
    }
    for (node, sn), val in scalar_params.items():
        socket = node.get_input_socket(sn)
        executor.sync_node_from_external_storage(socket, core.to_meta_any(val))
    for (node, sn), val in vec_params.items():
        node.get_input_socket(sn).set_default_value(val)


def main():
    import numpy as np
    import hd_RUZINO_py as renderer

    out_dir = BIN / "test_output" / "clouds"
    out_dir.mkdir(parents=True, exist_ok=True)
    scene_dir = BIN / "cloud_scenes"
    scene_dir.mkdir(parents=True, exist_ok=True)

    WIDTH = HEIGHT = int(os.environ.get("CLOUD_RES", "640"))
    SPP = int(os.environ.get("CLOUD_SPP", "1024"))
    # Yield time between frames so the desktop compositor keeps GPU slices
    # (single-GPU machine: a full-rate loop otherwise stutters the desktop).
    FRAME_SLEEP = float(os.environ.get("CLOUD_FRAME_SLEEP", "0.005"))

    scene = scene_dir / "closeup.usda"
    _build_scene(scene)
    print(f"[closeup] single cloud at y=5..45, camera at (0,20,60)")

    hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
    _build_render_graph(hydra, SPP)
    for _ in range(SPP):
        hydra.render()
        if FRAME_SLEEP:
            time.sleep(FRAME_SLEEP)
    tex = hydra.get_output_texture()
    img = np.array(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
    img = np.flipud(img)
    rgb = np.clip(img[:, :, :3], 0, 1)
    finite = np.isfinite(img).all()

    out = out_dir / "closeup.png"
    from PIL import Image
    Image.fromarray((rgb * 255).astype(np.uint8)).save(out)
    print(f"  saved {out}")
    print(f"  finite={finite} mean={rgb.mean():.4f}")
    print(f"  sky/cloud/ground means:")
    h = HEIGHT
    print(f"    top third   = {rgb[:h//3].mean(axis=(0,1))}")
    print(f"    middle      = {rgb[h//3:2*h//3].mean(axis=(0,1))}")
    print(f"    bottom      = {rgb[2*h//3:].mean(axis=(0,1))}")


if __name__ == "__main__":
    main()
