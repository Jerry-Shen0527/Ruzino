#!/usr/bin/env python3
"""Time-coded animation rendering — regression test for the render-time path.

Ruzino's Hydra 2.0 direct-read ingest (hydra2Ingest.cpp) queries data sources
with a 0.0f *shutter offset*; the actual sample time comes from the stage
globals that UsdImagingGLEngine::Render advances via
``_usdImagingSceneIndex->SetTime(params.frame)`` (see SDK engine.cpp:485-497
and dataSourceAttribute.h — the argument is an offset against
``_stageGlobals.GetTime()``). HydraRenderer.render(time_code) forwards the
frame into that path.

This test protects that wiring end-to-end at the PIXEL level:

  * BoxA authors time-sampled ``points`` (geometry deformation path —
    DirtyPoints resample), BoxB authors a time-sampled ``xformOp:transform``
    (DirtyTransform path). Both are emissive with distinct colors (no lights
    -> deterministic BSDF-hit-emissive, no NEE noise).
  * At t=0 BoxA (red) is left / BoxB (blue) is right; at t=24 they swapped.
  * Rendering at t=0 vs t=24 must flip the left/right color dominance.

If this test fails, time flow from HydraRenderer.render(time_code) through
SetTime to the rprim Sync has regressed (e.g. hardcoded sampling, broken
dirty propagation on time change).
"""

import os
import sys
from pathlib import Path

import numpy as np
import pytest


def _prepare_env():
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
    binary_dir = workspace_root / "Binaries" / "Release"

    os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', str(binary_dir))
    mtlx_stdlib = binary_dir / "libraries"
    if mtlx_stdlib.exists():
        os.environ.setdefault('PXR_MTLX_STDLIB_SEARCH_PATHS', str(mtlx_stdlib))

    os.environ['PATH'] = str(binary_dir) + os.pathsep + os.environ.get('PATH', '')
    if hasattr(os, 'add_dll_directory'):
        try:
            os.add_dll_directory(str(binary_dir))
        except Exception:
            pass

    if str(binary_dir) not in sys.path:
        sys.path.insert(0, str(binary_dir))

    return workspace_root, binary_dir


# Scene layout: camera looks at origin from +Z. Two emissive boxes swap
# sides between t=0 and t=T_END — BoxA via animated `points` (deformation),
# BoxB via animated transform. Colors are distinct so a left/right color
# probe identifies who is where without needing a clean background.
X_LEFT, X_RIGHT = -1.2, 1.2
T_END = 24.0
HALF = 0.4


def _box_points(cx: float):
    """A unit-ish box (8 verts, 6 quads) centered at (cx, 0, 0)."""
    y0, y1 = -HALF, HALF
    z0, z1 = -HALF, HALF
    x0, x1 = cx - HALF, cx + HALF
    return [
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    ]


_FVC = [4, 4, 4, 4, 4, 4]
_FVI = [0, 1, 2, 3, 4, 5, 6, 7, 0, 3, 7, 4, 1, 5, 6, 2, 0, 4, 5, 1, 3, 2, 6, 7]
_NORMALS = ([(0, 0, -1)] * 4 + [(0, 0, 1)] * 4 + [(0, -1, 0)] * 4 +
            [(0, 1, 0)] * 4 + [(-1, 0, 0)] * 4 + [(1, 0, 0)] * 4)


def _emissive_material(stage, name, color, strength=4.0):
    from pxr import Sdf, Tf, UsdShade

    mat = UsdShade.Material.Define(stage, f"/{name}")
    shader = UsdShade.Shader.Define(stage, f"/{name}/Shader")
    shader.CreateIdAttr("ND_standard_surface_surfaceshader")
    shader.CreateInput("base", Sdf.ValueTypeNames.Float).Set(0.0)
    shader.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
    shader.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(1.0)
    shader.CreateInput("emission", Sdf.ValueTypeNames.Float).Set(strength)
    shader.CreateInput("emission_color", Sdf.ValueTypeNames.Color3f).Set(color)
    # Connect via SdfPath — the TfToken overload isn't exposed in this
    # build's Python.
    mat.CreateSurfaceOutput().ConnectToSource(
        Sdf.Path(f"/{name}/Shader.outputs:surface"))
    return mat


def _author_scene(path: Path):
    from pxr import Usd, UsdGeom, UsdShade, Sdf, Gf

    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, "Y")

    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(20.25)
    cam.CreateClippingRangeAttr((0.1, 100.0))
    xform = UsdGeom.XformCommonAPI(cam)
    xform.SetTranslate((0.0, 0.0, 4.0))
    xform.SetRotate((0.0, 0.0, 0.0))

    mat_a = _emissive_material(stage, "MatRed", (1.0, 0.05, 0.05))
    mat_b = _emissive_material(stage, "MatBlue", (0.05, 0.05, 1.0))

    # BoxA — animated `points` (deformation path): left at t=0, right at
    # t=T_END. No transform authored.
    box_a = UsdGeom.Mesh.Define(stage, "/World/BoxA")
    box_a.CreateSubdivisionSchemeAttr("none")
    box_a.CreateFaceVertexCountsAttr(_FVC)
    box_a.CreateFaceVertexIndicesAttr(_FVI)
    box_a.CreateNormalsAttr(_NORMALS)
    box_a.CreatePointsAttr().Set(_box_points(X_LEFT), 0.0)
    box_a.CreatePointsAttr().Set(_box_points(X_RIGHT), T_END)
    UsdShade.MaterialBindingAPI.Apply(box_a.GetPrim()).Bind(mat_a)

    # BoxB — animated transform: right at t=0, left at t=T_END.
    box_b = UsdGeom.Mesh.Define(stage, "/World/BoxB")
    box_b.CreateSubdivisionSchemeAttr("none")
    box_b.CreateFaceVertexCountsAttr(_FVC)
    box_b.CreateFaceVertexIndicesAttr(_FVI)
    box_b.CreateNormalsAttr(_NORMALS)
    box_b.CreatePointsAttr(_box_points(0.0))
    xform_b = UsdGeom.XformCommonAPI(box_b)
    xform_b.SetTranslate((X_RIGHT, 0.0, 0.0), time=0.0)
    xform_b.SetTranslate((X_LEFT, 0.0, 0.0), time=T_END)
    UsdShade.MaterialBindingAPI.Apply(box_b.GetPrim()).Bind(mat_b)

    stage.SetStartTimeCode(0.0)
    stage.SetEndTimeCode(T_END)
    stage.GetRootLayer().Save()


def _make_renderer(binary_dir: Path, scene: Path, width=160, height=120,
                   samples=8):
    """Create one HydraRenderer for the whole test. Both time codes must be
    rendered through a SINGLE instance: a second HydraRenderer in the same
    process deterministically renders black (known delegate-teardown issue,
    same class as the emissive-suite second-render-black bug)."""
    import hd_RUZINO_py as renderer

    hydra = renderer.HydraRenderer(str(scene), width, height)

    # Reuse the render-graph builder from the unified renderer tests.
    sys.path.insert(0, str(Path(__file__).parent))
    from test_hydra_renderer import _build_render_graph
    _build_render_graph(hydra, binary_dir, samples)
    return hydra


def _render_at(hydra, time_code: float, samples=8, width=160, height=120):
    if time_code != 0.0:
        # Warm up at t=0 first (initial sync + shader compile), then reset
        # the accumulator so only the requested time code's samples remain.
        hydra.render(0.0)
        hydra.reset_accumulation()
    for _ in range(samples):
        hydra.render(time_code)
    hydra.stop()
    data = hydra.get_output_texture()
    img = np.array(data, dtype=np.float32).reshape(height, width, 4)
    return img[:, :, :3]


def _side_dominance(img):
    """Return (left_is_red, right_is_red): True when that side's mean red
    channel clearly exceeds its mean blue channel."""
    h, w, _ = img.shape
    left = img[:, : w // 3]
    right = img[:, 2 * w // 3:]
    left_red = float(left[:, :, 0].mean()) > float(left[:, :, 2].mean()) * 1.5
    right_red = float(right[:, :, 0].mean()) > float(right[:, :, 2].mean()) * 1.5
    return left_red, right_red


def test_time_coded_render_swaps_animated_prims():
    workspace_root, binary_dir = _prepare_env()

    try:
        import hd_RUZINO_py  # noqa: F401
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    out_dir = binary_dir / "test_output" / "animation_render"
    out_dir.mkdir(parents=True, exist_ok=True)
    scene = out_dir / "swap_boxes.usda"
    if scene.exists():
        scene.unlink()
    _author_scene(scene)

    hydra = _make_renderer(binary_dir, scene)
    img_t0 = _render_at(hydra, 0.0)
    img_t1 = _render_at(hydra, T_END)
    # Save diagnostics.
    try:
        from PIL import Image
        for name, img in (("t0.png", img_t0), ("t24.png", img_t1)):
            rgb = img
            peak = max(float(rgb.max()), 1e-6)
            rgb8 = (np.clip(rgb / peak, 0, 1) * 255).astype(np.uint8)
            Image.fromarray(np.flipud(rgb8)).save(out_dir / name)
    except Exception:
        pass

    # Sanity: both renders actually show the boxes (not black / not blown out).
    for name, img in (("t=0", img_t0), ("t=T", img_t1)):
        assert float(img.mean()) > 1e-3, (
            f"render at {name} appears blank")

    left_red_t0, right_red_t0 = _side_dominance(img_t0)
    left_red_t1, right_red_t1 = _side_dominance(img_t1)

    # t=0: red box (animated points) left, blue box (animated transform) right.
    assert left_red_t0 and not right_red_t0, (
        "at t=0 the red box should dominate the left third and the blue box "
        "the right third — animated points/transform were not sampled at "
        f"t=0 (left_red={left_red_t0}, right_red={right_red_t0})")
    # t=T_END: swapped.
    assert (not left_red_t1) and right_red_t1, (
        "at t=T_END the boxes should have swapped sides — the renderer did "
        "not resample animated points/transform at the new time code "
        f"(left_red={left_red_t1}, right_red={right_red_t1})")

    # And the two time codes really produced different images.
    diff = float(np.abs(img_t0 - img_t1).mean())
    assert diff > 1e-3, f"renders at t=0 and t={T_END} are identical ({diff})"
