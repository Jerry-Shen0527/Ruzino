#!/usr/bin/env python3
"""
Hosek-Wilkie analytic sky render test.

Renders the analytic sky dome (no geometry) at two solar elevations — noon
(sun high) and dusk (sun near the horizon) — and saves PNGs for visual
inspection. The Hosek model is CPU-cooked from (turbidity, albedo, elevation)
in light.cpp and evaluated per-pixel by the
eval_dome_light_hosek_wilkie.slang dome callable.

Run from Binaries/Release (so node-plugin DLLs resolve):

    python ../../source/tests/render_hosek_sky.py

Outputs land in Binaries/Release/test_output/hosek_sky/.
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
    if primary.exists():
        return primary
    fallback = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"
    if fallback.exists():
        return fallback
    sys.exit("render node config not found (render_nodes.json)")


from pxr import Usd, UsdGeom, UsdLux, Gf


def _build_sky_scene(path, sun_elev_deg, sun_az_deg, turbidity, albedo):
    """Author a minimal scene: just a camera looking up/across + a Hosek
    DomeLight. The dome's sunDirection is in dome-local space (+Y up):
    elevation = angle above horizon, azimuth measured in the XZ plane."""
    if path.exists():
        path.unlink()
    stage = Usd.Stage.CreateNew(str(path))

    # Camera: looking slightly up toward the horizon so the frame shows both
    # zenith (top) and horizon (bottom) of the sky. USD camera looks down -Z
    # with +Y up; tilt it so the horizon sits near the bottom of frame.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(35.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 1e5))

    eye = Gf.Vec3d(0.0, 0.0, 0.0)
    # Aim the camera at a point on the horizon in the +X direction, slightly up
    # so the zenith is visible. target = (horizon dir) tilted up a little.
    target = Gf.Vec3d(1.0, 0.15, 0.0)  # mostly forward, a bit up
    fwd = target - eye
    fwd.Normalize()
    up_world = Gf.Vec3d(0.0, 1.0, 0.0)
    right = Gf.Cross(fwd, up_world); right.Normalize()
    up = Gf.Cross(right, fwd)
    m = Gf.Matrix4d()
    m.SetIdentity()
    m.SetRow(0, Gf.Vec4d(right[0], right[1], right[2], 0.0))
    m.SetRow(1, Gf.Vec4d(up[0], up[1], up[2], 0.0))
    m.SetRow(2, Gf.Vec4d(-fwd[0], -fwd[1], -fwd[2], 0.0))
    m.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    UsdGeom.Xformable(cam).AddTransformOp().Set(m)

    # Hosek DomeLight. sunDirection is dome-local (+Y up):
    #   y = sin(elevation), horizontal = cos(elevation) at the azimuth.
    elev = math.radians(sun_elev_deg)
    az = math.radians(sun_az_deg)
    sd = Gf.Vec3f(math.cos(elev) * math.cos(az),
                  math.sin(elev),
                  math.cos(elev) * math.sin(az))
    dome = UsdLux.DomeLight.Define(stage, "/HosekSky")
    dome.CreateIntensityAttr().Set(1.0)
    # Custom attributes select the Hosek-Wilkie dome callable + its params.
    # light.cpp reads these with the two-form GetLightParamValue pattern
    # (bare + "inputs:" prefix), so inputs:-form is the natural convention.
    from pxr import Sdf
    dome.GetPrim().CreateAttribute(
        "inputs:shader_path", Sdf.ValueTypeNames.String).Set(
        "callables/eval_dome_light_hosek_wilkie.slang")
    dome.GetPrim().CreateAttribute(
        "inputs:turbidity", Sdf.ValueTypeNames.Float).Set(float(turbidity))
    dome.GetPrim().CreateAttribute(
        "inputs:groundAlbedo", Sdf.ValueTypeNames.Float).Set(float(albedo))
    dome.GetPrim().CreateAttribute(
        "inputs:sunDirection", Sdf.ValueTypeNames.Float3).Set(sd)

    stage.GetRootLayer().Save()
    return path


def _build_render_graph(hydra, samples):
    """Standard path-tracing graph: rng → raygen → path_tracing → accumulate
    → lpm → gamma → present_color. (Same as test_render_materials.)"""
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
        (ray_gen, "Aperture"): 0.0,
        (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
        (gamma, "Gamma"): 2.2,
        (lpm, "LPM Exposure"): 0.0,
        (lpm, "HDR Max"): 4.0,
        (lpm, "Contrast"): 1.0,
        (lpm, "Shoulder"): 1.0,
        (lpm, "Shoulder Contrast"): 1.0,
        (lpm, "Soft Gap"): 0.0,
        (lpm, "Color Space"): 0,
        (lpm, "Display Mode"): 0,
        (lpm, "Display Max Luminance"): 1000.0,
        (lpm, "Display Min Luminance"): 0.0,
    }
    for (node, sn), val in scalar_params.items():
        socket = node.get_input_socket(sn)
        meta = core.to_meta_any(val)
        executor.sync_node_from_external_storage(socket, meta)
    for (node, sn), val in vec_params.items():
        node.get_input_socket(sn).set_default_value(val)


def main():
    import numpy as np
    import hd_RUZINO_py as renderer

    out_dir = BIN / "test_output" / "hosek_sky"
    out_dir.mkdir(parents=True, exist_ok=True)
    scene_dir = BIN / "hosek_sky_scenes"
    scene_dir.mkdir(parents=True, exist_ok=True)

    cases = [
        # (name, elev_deg, az_deg, turbidity, albedo)
        ("noon", 60.0, 0.0, 3.0, 0.3),
        ("dusk", 6.0, 0.0, 6.0, 0.3),
        ("hazy_noon", 45.0, 90.0, 8.0, 0.3),
    ]

    WIDTH, HEIGHT, SPP = 640, 480, 64

    for name, elev, az, turb, alb in cases:
        scene = scene_dir / f"hosek_{name}.usda"
        _build_sky_scene(scene, elev, az, turb, alb)
        print(f"\n[{name}] elev={elev}deg az={az}deg turb={turb} albedo={alb}")

        hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
        _build_render_graph(hydra, SPP)
        for _ in range(SPP):
            hydra.render()
        tex = hydra.get_output_texture()
        img = np.array(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
        img = np.flipud(img)

        rgb = np.clip(img[:, :, :3], 0, 1)
        mean = rgb.mean()
        # Split the frame into top (zenith) and bottom (horizon) halves.
        top_mean = rgb[: HEIGHT // 2].mean(axis=(0, 1))
        bot_mean = rgb[HEIGHT // 2:].mean(axis=(0, 1))
        finite = np.isfinite(img).all()

        try:
            from PIL import Image
            Image.fromarray((rgb * 255).astype(np.uint8)).save(out_dir / f"{name}.png")
            print(f"  saved {out_dir / (name + '.png')}")
        except ImportError:
            np.save(out_dir / f"{name}.npy", img)

        print(f"  finite={finite} mean={mean:.4f}")
        print(f"  zenith mean RGB = ({top_mean[0]:.3f},{top_mean[1]:.3f},{top_mean[2]:.3f})")
        print(f"  horizon mean RGB= ({bot_mean[0]:.3f},{bot_mean[1]:.3f},{bot_mean[2]:.3f})")

        if not finite:
            print(f"  !! WARNING: {name} contains NaN/Inf")
        if mean < 1e-3:
            print(f"  !! WARNING: {name} too dim")

    print(f"\nDone. PNGs in {out_dir}")


if __name__ == "__main__":
    main()
