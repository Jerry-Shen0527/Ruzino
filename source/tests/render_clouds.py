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


def _build_scene(path, coverage=0.35, density=1.2, sun_elev_deg=45.0):
    if path.exists():
        path.unlink()
    stage = Usd.Stage.CreateNew(str(path))

    # A ground plane so we can see soft cloud shadows. Enlarged to ±5000 so
    # the far edge is well off-frame (a small plane left an empty black band
    # where rays miss everything). Subdividing into a 10×10 grid satisfies
    # "not just one quad"; the checkerboard itself is drawn by the shader
    # below from world XZ, so mesh density doesn't change the pattern.
    plane = UsdGeom.Mesh.Define(stage, "/Ground")
    DIV = 10
    HALF = 5000.0
    step = (2.0 * HALF) / DIV
    pts = []
    idx = []
    for iz in range(DIV + 1):
        for ix in range(DIV + 1):
            pts.append((ix * step - HALF, 0.0, iz * step - HALF))
    for iz in range(DIV):
        for ix in range(DIV):
            base = iz * (DIV + 1) + ix
            idx.append(base)
            idx.append(base + 1)
            idx.append(base + (DIV + 1) + 1)
            idx.append(base + (DIV + 1))
    plane.CreatePointsAttr().Set(pts)
    plane.CreateFaceVertexCountsAttr().Set([4] * (DIV * DIV))
    plane.CreateFaceVertexIndicesAttr().Set(idx)
    plane.CreateNormalsAttr().Set([(0, 1, 0)] * len(pts))
    plane.SetNormalsInterpolation("vertex")

    # Procedural checkerboard material (shader-path EVAL callable). The
    # attribute is config:shader_path on the MATERIAL prim — the renderer
    # reads it out of the Hydra material-network config dict (NOT inputs:,
    # which is the dome-light convention) and bypasses MaterialX generation
    # entirely, compiling eval_checkerboard_ground.slang directly.
    ground_mat = UsdShade.Material.Define(stage, "/GroundChecker")
    ground_mat.GetPrim().CreateAttribute(
        "config:shader_path", Sdf.ValueTypeNames.String).Set(
        "callables/checkerboard_ground.slang")
    # A surface output terminal is required for Hydra to treat this as a
    # bound-able material; the network is bypassed when shader_path is valid.
    surface_out = ground_mat.CreateSurfaceOutput()
    dummy = UsdShade.Shader.Define(stage, "/GroundChecker/PreviewSurface")
    dummy.CreateIdAttr("UsdPreviewSurface")
    dummy.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.5, 0.5, 0.5))
    surface_out.ConnectToSource(dummy.ConnectableAPI(),
                                "surface", UsdShade.AttributeType.Output)
    plane.GetPrim().ApplyAPI("MaterialBindingAPI")
    UsdShade.MaterialBindingAPI(plane).Bind(ground_mat)

    # Camera: looking up across the cloud layer.
    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.GetFocalLengthAttr().Set(35.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.1, 1e5))
    # Camera at human eye height (y=8) framing physically-scaled clouds
    # (base ~1 km up). Standing on the ground looking at 1 km-high clouds,
    # a normal-FOV camera can't hold BOTH the ground (horizon ~0 deg) and the
    # near cloud base (~40 deg) in one frame — near clouds are overhead. So
    # pitch to just above the horizon: frame shows the ground in the lower
    # portion and the cloud layer receding toward the horizon above it.
    # eye z=1200, target y=280 -> ~13 deg pitch, horizon sits in the lower frame.
    UsdGeom.Xformable(cam).AddTransformOp().Set(
        _look_at(eye=(0.0, 8.0, 1200.0), target=(0.0, 280.0, 0.0)))

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
    sun.CreateIntensityAttr().Set(1.5)  # was 5.0 — far too bright, blew out the ground
    sun.CreateAngleAttr().Set(0.53)
    UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)

    # Procedural cloud volume.
    vol = UsdVol.Volume.Define(stage, "/CloudLayer")
    pv = UsdGeom.PrimvarsAPI(vol)
    pv.CreatePrimvar("volumeType", Sdf.ValueTypeNames.Token).Set("cloud")
    # Physically-scaled cloud layer (1 unit = 1 meter): base at 1 km (typical
    # cumulus condensation level), top at 2.5 km (~1.5 km thick — real cumulus
    # congestus depth). Horizontal extent ±5000 matches the ground so clouds
    # reach the horizon.
    pv.CreatePrimvar("boundsMin", Sdf.ValueTypeNames.Float3).Set(Gf.Vec3f(-5000.0, 1000.0, -5000.0))
    pv.CreatePrimvar("boundsMax", Sdf.ValueTypeNames.Float3).Set(Gf.Vec3f(5000.0, 2500.0, 5000.0))
    pv.CreatePrimvar("coverage", Sdf.ValueTypeNames.Float).Set(float(coverage))
    pv.CreatePrimvar("densityScale", Sdf.ValueTypeNames.Float).Set(float(density))
    pv.CreatePrimvar("phaseG", Sdf.ValueTypeNames.Float).Set(0.7)
    pv.CreatePrimvar("layerTop", Sdf.ValueTypeNames.Float).Set(1.0)
    pv.CreatePrimvar("layerBottom", Sdf.ValueTypeNames.Float).Set(0.0)
    # Per-axis noise frequencies (.x=horizX, .y=vertical, .z=horizZ), tuned to
    # physical cloud scale. Individual cumulus clouds are ~1 km across, and the
    # layer is ~1.5 km thick, so: horizontal 10000m/1000m = 10 cells, vertical
    # 1500m/~500m = 3 cells. This 10:3 anisotropy matches real cloud aspect —
    # not the extreme 60:7 that made clouds look flat. cloud_intersection.slang
    # multiplies each normalized axis by these.
    pv.CreatePrimvar("noiseFreq", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(10.0, 3.0, 10.0))
    pv.CreatePrimvar("worleyFreq", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(10.0, 3.0, 10.0))
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
    present = tree.add_node("present_color")
    # NOTE: no gamma_correction node here. LPM already applies the display
    # gamma (color^(1/2.2)) in LDR mode (Display Mode=0), so chaining a
    # second gamma_correction would apply gamma twice and wash the image out.

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
        # LPM is the sole tone mapper: it does the HDR->LDR curve AND the
        # display gamma (no separate gamma_correction node — that would
        # double-gamma). With the double-gamma removed, midtones came out too
        # dark — recover them with a positive LPM Exposure (larger = brighter
        # midtones). HDR Max=2.0 fits the wider range (sunlit ground + sky).
        (lpm, "HDR Max"): 2.0, (lpm, "LPM Exposure"): 2.0,
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
        # (name, coverage, density, sun_elev_deg)
        # NOTE: coverage is SUBTRACTED as a density threshold in the shader
        # (cloud_intersection.slang: shape -= coverage), so LOWER coverage =
        # MORE cloud. Higher Y noise freq also lowers average density, so
        # overcast needs a lower threshold than sunny to still show cloud.
        ("cloud_sunny", 0.32, 3.0, 55.0),   # sparse, scattered clouds
        ("cloud_overcast", 0.05, 4.5, 35.0),  # near-full cover
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
