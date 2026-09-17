#!/usr/bin/env python3
"""Volumetric fog composite tests (real-time raster pipeline).

Graph under test:

    rasterize ──► deferred_direct_lighting ──► volumetric_fog_composite ──► present_color
       └──────────────► (Position) ──────────────────┘

Scene: level camera (eye y=2) over a ground plane at y=0 with a
HosekWilkieSky rig (sun toward +X/+Y/+Z, i.e. screen-right of a camera
looking down -Z with +Y up), so the screen splits roughly into sky (top
half, above the horizon) and ground (bottom half).

Each render runs in a subprocess (GPU isolation, same pattern as
test_hosek_sky_rig_render.py); the scenario is parameterized by fog density
and the sky-background switch.
"""
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest


_RENDER_SCRIPT = r"""
import os, sys

binary_dir, stage_path, out_npy, density_s, sky_s, size_s = sys.argv[1:7]
density, sky_on, size = float(density_s), bool(int(sky_s)), int(size_s)

sys.path.insert(0, binary_dir)
os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', binary_dir)
os.environ['PATH'] = binary_dir + os.pathsep + os.environ.get('PATH', '')

import hd_RUZINO_py as renderer
import nodes_core_py as core

hydra = renderer.HydraRenderer(stage_path, size, size)

node_system = hydra.get_node_system()
config = os.path.join(binary_dir, 'render_nodes.json')
if not os.path.exists(config):
    print('NO_RENDER_NODES_CONFIG')
    sys.exit(0)
node_system.load_configuration(config)
node_system.init()

tree = node_system.get_node_tree()
executor = node_system.get_node_tree_executor()

raster = tree.add_node("rasterize")
deferred = tree.add_node("deferred_direct_lighting")
fog = tree.add_node("volumetric_fog_composite")
present = tree.add_node("present_color")

for n in ['Position', 'Texcoords', 'DiffuseColor', 'MetallicRoughness',
          'Normal', 'MaterialID']:
    tree.add_link(raster.get_output_socket(n),
                  deferred.get_input_socket(n))
tree.add_link(deferred.get_output_socket("Color"),
              fog.get_input_socket("Color"))
tree.add_link(raster.get_output_socket("Position"),
              fog.get_input_socket("Position"))
tree.add_link(fog.get_output_socket("Color"),
              present.get_input_socket("Color"))

executor.reset_allocator()
executor.prepare_tree(tree, present)

executor.sync_node_from_external_storage(
    fog.get_input_socket("Density"), core.to_meta_any(density))
executor.sync_node_from_external_storage(
    fog.get_input_socket("Sky Background"), core.to_meta_any(sky_on))

hydra.render()
hydra.stop()

import numpy as np
img = np.array(hydra.get_output_texture(), dtype=np.float32).reshape(size, size, 4)
np.save(out_npy, img)
print('RENDER_DONE')
"""


def _prepare_env():
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
    build_type = os.environ.get("RZ_BUILD_TYPE", "Release")
    binary_dir = workspace_root / "Binaries" / build_type
    if not binary_dir.exists():
        binary_dir = workspace_root / "Binaries" / "Release"
    return workspace_root, binary_dir


def _norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def _cross(a, b):
    return [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]


def _fog_scene(tmp_path):
    """Level camera over a ground plane + Hosek dome light.

    Camera at (0, 2, 12) looking toward -Z with +Y up: screen right = world
    +X, the horizon sits at mid-screen. Sun (0.8, 0.25, -0.1) is low toward
    +X slightly in front — its warm glow lands INSIDE the frame, screen right
    near the horizon, so the right sky must read warmer than the left.

    The dome is a plain DomeLight + authored inputs:shader_path (the
    render_hosek_sky.py convention — proven to reach the render-side Hosek
    cook). NOTE: a HosekWilkieSky schema prim does NOT get ingested as a dome
    light by the headless hydra2 flow (2026-09-16 finding); using the schema
    prim here would silently skip the cook.

    The ground binds a UsdPreviewSurface (displayColor-only meshes hit the
    "default material" path and decode black in the raster MaterialEvaluation
    pass). Authored via the pxr API — the proven pattern from
    test_raster_pipeline._build_instancer_scene.
    """
    from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf

    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)

    cam = UsdGeom.Camera.Define(stage, '/Camera')
    cam.GetFocalLengthAttr().Set(35.0)
    cam.GetClippingRangeAttr().Set(Gf.Vec2f(0.05, 1e5))
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.AddTransformOp().Set(
        _camera_gf_matrix((0, 2, 12), (0, 2, 0), (0, 1, 0)))

    mat = UsdShade.Material.Define(stage, '/GroundMat')
    shader = UsdShade.Shader.Define(stage, '/GroundMat/Shader')
    shader.CreateIdAttr('UsdPreviewSurface')
    shader.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.5, 0.5, 0.5))
    shader.CreateInput('roughness', Sdf.ValueTypeNames.Float).Set(0.8)
    mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),
                                              'surface')

    H = 40.0
    ground = UsdGeom.Mesh.Define(stage, '/World/Ground')
    ground.CreatePointsAttr().Set([
        Gf.Vec3f(-H, 0, -H), Gf.Vec3f(H, 0, -H),
        Gf.Vec3f(H, 0, H), Gf.Vec3f(-H, 0, H)])
    ground.CreateFaceVertexCountsAttr().Set([4])
    ground.CreateFaceVertexIndicesAttr().Set([0, 1, 2, 3])
    ground.CreateNormalsAttr().Set([Gf.Vec3f(0, 1, 0)])
    ground.GetSubdivisionSchemeAttr().Set('none')
    UsdShade.MaterialBindingAPI.Apply(ground.GetPrim()).Bind(mat)

    # Hosek dome (plain DomeLight + authored callable attrs; the child sun
    # lights the ground surface — deferred skips domes for surface lighting).
    dome = UsdLux.DomeLight.Define(stage, '/World/Sky')
    dome.GetPrim().CreateAttribute(
        'inputs:shader_path', Sdf.ValueTypeNames.String).Set(
        'callables/eval_dome_light_hosek_wilkie.slang')
    dome.GetPrim().CreateAttribute(
        'inputs:sunDirection', Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(0.8, 0.25, -0.1))
    sun = UsdLux.DistantLight.Define(stage, '/World/Sky/Sun')
    sun.GetIntensityAttr().Set(3.0)
    sun.GetAngleAttr().Set(0.001)

    stage_path = tmp_path / "fog_scene.usda"
    stage.GetRootLayer().Export(str(stage_path))
    return stage_path


def _camera_gf_matrix(eye, target, up):
    """Gf look-at matrix (rows: right / up / -forward / eye)."""
    from pxr import Gf
    f = _norm([t - e for t, e in zip(target, eye)])
    r = _norm(_cross(f, up))
    u = _cross(r, f)
    return Gf.Matrix4d(
        r[0], r[1], r[2], 0.0,
        u[0], u[1], u[2], 0.0,
        -f[0], -f[1], -f[2], 0.0,
        eye[0], eye[1], eye[2], 1.0)


def _render_fog(binary_dir, stage_path, tmp_path, name,
                density, sky=True, size=192):
    out_npy = tmp_path / f"{name}.npy"
    r = subprocess.run(
        [sys.executable, "-c", _RENDER_SCRIPT,
         str(binary_dir), str(stage_path), str(out_npy),
         str(density), "1" if sky else "0", str(size)],
        capture_output=True, text=True, timeout=300,
        cwd=str(binary_dir),
    )
    combined = r.stdout + r.stderr
    assert "RENDER_DONE" in combined, f"{name}: render failed:\n{combined[-3000:]}"
    return np.load(out_npy)


def _lum(img):
    return img[:, :, :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def test_fog_sky_background(tmp_path):
    """Zero fog + sky background on: the raster path finally has a Hosek sky.

    Above the horizon: non-black, blue-dominant (noon Hosek), warmer toward
    the sun side (screen right = world +X), brighter near the horizon than at
    the zenith (vertical orientation sanity)."""
    _, binary_dir = _prepare_env()
    stage = _fog_scene(tmp_path)
    img = _render_fog(binary_dir, stage, tmp_path, "sky_on", density=0.0)

    assert np.isfinite(img).all(), "Output contains NaN or Inf"
    size = img.shape[0]

    sky = img[: size // 4, :, :3]          # well above the horizon
    assert float(_lum(sky).mean()) > 0.005, \
        f"Sky is black (mean lum {_lum(sky).mean():.4f}) — background not filled"

    rgb = sky.reshape(-1, 3).mean(axis=0)
    assert rgb[2] > rgb[0], f"Sky not blue-dominant: rgb={rgb}"

    # Sun side (right half) warmer than the anti-sun side (left half).
    right = sky[:, sky.shape[1] * 3 // 4:, 0].mean()
    left = sky[:, : sky.shape[1] // 4, 0].mean()
    assert right > left, \
        f"Sun-side sky not warmer (right R={right:.4f} <= left R={left:.4f}) — image flipped?"

    # Vertical orientation: the sky (top quarter) is bluer than the ground
    # (bottom quarter). Sun-position independent, unlike a horizon-brightness
    # gradient (the sun glow sits above the frame here and can dominate it).
    ground = img[size * 3 // 4:, size // 4: 3 * size // 4, :3]
    sky_core = sky[:, size // 4: 3 * size // 4]
    assert (sky_core[:, :, 2] - sky_core[:, :, 0]).mean() > \
        (ground[:, :, 2] - ground[:, :, 0]).mean(), \
        "Top of image not bluer than bottom — vertical flip?"


def test_fog_sky_background_disabled(tmp_path):
    """Sky Background off: background pixels stay black (old behavior)."""
    _, binary_dir = _prepare_env()
    stage = _fog_scene(tmp_path)
    img = _render_fog(binary_dir, stage, tmp_path, "sky_off",
                      density=0.0, sky=False)

    size = img.shape[0]
    sky = img[: size // 4, :, :3]
    assert float(np.abs(sky).mean()) < 0.002, \
        f"Background not black with Sky Background off (mean {np.abs(sky).mean():.4f})"


def test_fog_height_fog_affects_ground(tmp_path):
    """Height fog A/B: denser fog changes the ground much more than the sky.

    The fog is concentrated near y=0; a level camera's ground rays cross that
    layer while sky rays never descend into it."""
    _, binary_dir = _prepare_env()
    stage = _fog_scene(tmp_path)
    clear = _render_fog(binary_dir, stage, tmp_path, "ab_clear", density=0.0)
    foggy = _render_fog(binary_dir, stage, tmp_path, "ab_foggy", density=0.4)

    size = clear.shape[0]
    diff = np.abs(foggy - clear)[:, :, :3]
    ground = diff[size * 5 // 8:, size // 4: 3 * size // 4]
    sky = diff[: size // 4, size // 4: 3 * size // 4]

    assert float(ground.mean()) > 0.01, \
        f"Fog did not change the ground (mean diff {ground.mean():.4f})"
    assert float(ground.mean()) > 2.0 * float(max(sky.mean(), 1e-6)), \
        (f"Ground not more fogged than sky "
         f"(ground {ground.mean():.4f} vs sky {sky.mean():.4f})")
