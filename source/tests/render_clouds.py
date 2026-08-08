#!/usr/bin/env python3
"""
Procedural path-traced cloud render test.

Authors a scene with a Hosek sky + an aligned DistantLight (sun) + a
UsdVol.Volume prim tagged volumeType="cloud" (the procedural cloud), then
renders it with the standard path_tracing node. The cloud's density is
generated on the GPU from fbm+Worley noise; it transmits sky/terrain behind
it and casts soft shadows.

Run from Binaries/Release:

    python ../../source/tests/render_clouds.py

Outputs land in Binaries/Release/test_output/clouds/.
"""
import os
import sys
import math
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


from pxr import Usd, UsdGeom, UsdLux, UsdVol, Sdf, Gf


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


def _build_scene(path, coverage=0.35, density=1.2, sun_elev_deg=45.0):
    if path.exists():
        path.unlink()
    stage = Usd.Stage.CreateNew(str(path))

    # A ground plane so we can see soft cloud shadows. (No material binding:
    # uses the path tracer's fallback shader — isolates whether a MaterialX
    # material is causing the flat render.)
    plane = UsdGeom.Mesh.Define(stage, "/Ground")
    plane.CreatePointsAttr().Set([
        (-50, 0, -50), (50, 0, -50), (50, 0, 50), (-50, 0, 50)])
    plane.CreateFaceVertexCountsAttr().Set([4])
    plane.CreateFaceVertexIndicesAttr().Set([0, 1, 2, 3])
    plane.CreateNormalsAttr().Set([(0, 1, 0)] * 4)
    plane.SetNormalsInterpolation("constant")

    # Camera: looking up across the cloud layer.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(35.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 1e5))
    UsdGeom.Xformable(cam).AddTransformOp().Set(
        _look_at(eye=(0.0, 8.0, 30.0), target=(0.0, 12.0, 0.0)))

    # Hosek sky (dome-local sun dir, +Y up).
    elev = math.radians(sun_elev_deg)
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

    # Sun (DistantLight): direction matches the sky sun (world space).
    # The sky's sunDirection was dome-local; for the DistantLight we shine
    # toward the scene from the same elevation/azimuth in world space.
    sun_shine = Gf.Vec3f(-math.cos(elev), -math.sin(elev), -0.3)
    sun_shine.Normalize()
    sun_xf = Gf.Matrix4d(); sun_xf.SetIdentity()
    sun_xf.SetRow(2, Gf.Vec4d(sun_shine[0], sun_shine[1], sun_shine[2], 0.0))
    sun = UsdLux.DistantLight.Define(stage, "/Sun")
    sun.CreateIntensityAttr().Set(5.0)
    sun.CreateAngleAttr().Set(0.53)
    UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)

    # Procedural cloud volume.
    vol = UsdVol.Volume.Define(stage, "/CloudLayer")
    pv = UsdGeom.PrimvarsAPI(vol)
    pv.CreatePrimvar("volumeType", Sdf.ValueTypeNames.Token).Set("cloud")
    pv.CreatePrimvar("boundsMin", Sdf.ValueTypeNames.Float3).Set(Gf.Vec3f(-40.0, 6.0, -40.0))
    pv.CreatePrimvar("boundsMax", Sdf.ValueTypeNames.Float3).Set(Gf.Vec3f(40.0, 22.0, 40.0))
    pv.CreatePrimvar("coverage", Sdf.ValueTypeNames.Float).Set(float(coverage))
    pv.CreatePrimvar("densityScale", Sdf.ValueTypeNames.Float).Set(float(density))
    pv.CreatePrimvar("phaseG", Sdf.ValueTypeNames.Float).Set(0.7)
    pv.CreatePrimvar("layerTop", Sdf.ValueTypeNames.Float).Set(1.0)
    pv.CreatePrimvar("layerBottom", Sdf.ValueTypeNames.Float).Set(0.0)
    pv.CreatePrimvar("noiseFreq", Sdf.ValueTypeNames.Float).Set(3.0)
    pv.CreatePrimvar("worleyFreq", Sdf.ValueTypeNames.Float).Set(3.0)
    pv.CreatePrimvar("detailErosion", Sdf.ValueTypeNames.Float).Set(0.6)

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
    gamma = tree.add_node("gamma_correction")
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
                  gamma.get_input_socket("Texture"))
    tree.add_link(gamma.get_output_socket("Corrected"),
                  present.get_input_socket("Color"))

    vec_params = {(lpm, "Crosstalk"): [0.471, 0.49, 0.504]}
    for (node, sn), val in vec_params.items():
        node.get_input_socket(sn).set_default_value(val)

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    scalar_params = {
        (ray_gen, "Aperture"): 0.0, (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples, (gamma, "Gamma"): 2.2,
        (lpm, "LPM Exposure"): 0.0, (lpm, "HDR Max"): 4.0,
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

    WIDTH, HEIGHT, SPP = 640, 480, 64

    cases = [
        ("cloud_sunny", 0.15, 3.0, 55.0),
        ("cloud_overcast", 0.35, 4.5, 35.0),
    ]

    for name, cov, dens, elev in cases:
        scene = scene_dir / f"{name}.usda"
        _build_scene(scene, coverage=cov, density=dens, sun_elev_deg=elev)
        print(f"\n[{name}] coverage={cov} density={dens} elev={elev}deg")

        hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
        _build_render_graph(hydra, SPP)
        for _ in range(SPP):
            hydra.render()
        tex = hydra.get_output_texture()
        img = np.array(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
        img = np.flipud(img)
        rgb = np.clip(img[:, :, :3], 0, 1)

        # Region analysis: top third (sky), middle (cloud), bottom (ground).
        h = HEIGHT
        sky = rgb[: h // 3].mean(axis=(0, 1))
        mid = rgb[h // 3: 2 * h // 3].mean(axis=(0, 1))
        gnd = rgb[2 * h // 3:].mean(axis=(0, 1))
        finite = np.isfinite(img).all()

        try:
            from PIL import Image
            Image.fromarray((rgb * 255).astype(np.uint8)).save(out_dir / f"{name}.png")
            print(f"  saved {out_dir / (name + '.png')}")
        except ImportError:
            np.save(out_dir / f"{name}.npy", img)

        print(f"  finite={finite}")
        print(f"  sky mean RGB   = ({sky[0]:.3f},{sky[1]:.3f},{sky[2]:.3f})")
        print(f"  cloud mean RGB = ({mid[0]:.3f},{mid[1]:.3f},{mid[2]:.3f})")
        print(f"  ground mean RGB= ({gnd[0]:.3f},{gnd[1]:.3f},{gnd[2]:.3f})")
        if not finite:
            print(f"  !! WARNING: {name} contains NaN/Inf")

    print(f"\nDone. PNGs in {out_dir}")


if __name__ == "__main__":
    main()
