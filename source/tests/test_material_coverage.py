"""
Material coverage tests — render one scene per material class and export PNGs
for manual visual inspection.

These tests exist to guard the MaterialX slang generator's three material
paths before refactoring the shader post-processing in materialX.cpp:

  1. fallback            — Material with no surface terminal (shader_type_id=2)
  2. standard_surface    — MaterialX native (shader_type_id=0)
  3. UsdPreviewSurface   — USD native   (shader_type_id=1)

Each test renders a dedicated scene (source/tests/data/scenes/materials/) at
moderate SPP and writes a PNG to Binaries/Release/test_output/material_coverage/.
Tests assert only that the render is finite and non-trivial; the real verdict
is the PNG you inspect by eye.

Each test constructs its own HydraRenderer in-process. This is safe because
~Hd_RUZINO_RenderDelegate resets the process-global MaterialX shared_document
(see Hd_RUZINO_MaterialX::reset_shared_state), so the Nth renderer doesn't
inherit the previous scene's accumulated material/shader nodes. Previously the
static shared_document was never cleared, which polluted later renders and
forced a per-test subprocess workaround; that is no longer needed.
"""

import os
from pathlib import Path

import numpy as np
import pytest

from conftest import TEST_OUTPUT_DIR
from test_render_materials import _build_render_graph

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
SCENES_DIR = Path(__file__).resolve().parent / "data" / "scenes" / "materials"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "material_coverage"

RENDER_WIDTH = 800
RENDER_HEIGHT = 600
RENDER_SPP = 256


def _save_png(img, name):
    """Save a render to OUTPUT_DIR/<name>.png for manual inspection."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    rgb = np.clip(img[:, :, :3], 0, 1)
    rgb = (rgb * 255).astype(np.uint8)
    try:
        from PIL import Image
        Image.fromarray(rgb).save(OUTPUT_DIR / f"{name}.png")
        print(f"Saved: {OUTPUT_DIR / (name + '.png')}  mean={img[:,:,:3].mean():.4f}")
    except ImportError:
        # Pillow missing — still write .npy so the render isn't lost.
        np.save(OUTPUT_DIR / f"{name}.npy", img)
        print(f"Saved: {OUTPUT_DIR / (name + '.npy')} (PIL missing, no PNG)")


def _render_scene(scene_path, name, width=RENDER_WIDTH, height=RENDER_HEIGHT, spp=RENDER_SPP):
    """Render a scene at fixed SPP and write a PNG. Returns the image array."""
    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    hydra = renderer.HydraRenderer(str(scene_path), width, height)
    _build_render_graph(hydra, spp)
    for _ in range(spp):
        hydra.render()
    texture_data = hydra.get_output_texture()
    img = np.array(texture_data, dtype=np.float32).reshape(height, width, 4)
    # GPU textures have origin at top-left; flip to match camera/scene Y-up.
    img = np.flipud(img)
    _save_png(img, name)
    return img


def _assert_finite_nonblank(img, name):
    """Common sanity: no NaN/Inf, and not pure black."""
    assert np.isfinite(img).all(), f"{name}: render contains NaN/Inf"
    mean = float(img[:, :, :3].mean())
    assert mean > 1e-3, f"{name}: render too dim (mean={mean:.6f})"


# ---------------------------------------------------------------------------
# Each test renders one scene. Tests are independent so they can be run
# individually: ``pytest source/tests/test_material_coverage.py -k fallback``
# ---------------------------------------------------------------------------

def test_material_fallback():
    """Fallback path: Material with no surface terminal → 0.8 grey Lambert.

    Expected: a uniform mid-grey board. This guards shader_type_id=2.
    """
    scene = SCENES_DIR / "mat_fallback.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    img = _render_scene(scene, "mat_fallback")
    _assert_finite_nonblank(img, "fallback")


def test_material_standard_surface():
    """standard_surface path: red diffuse / metallic gold / semi-transparent green.

    Expected: three vertical boards — red (left), gold-metallic (center),
    greenish-translucent (right). This guards shader_type_id=0 AND the
    opacity/transmission computation path that P0 will refactor.
    """
    scene = SCENES_DIR / "mat_standard_surface.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    img = _render_scene(scene, "mat_standard_surface")
    _assert_finite_nonblank(img, "standard_surface")


def test_material_preview_surface():
    """UsdPreviewSurface path: red / blue-metallic / semi-transparent yellow.

    Expected: three boards — red (left), blue-metallic (center), translucent
    yellow (right). Guards shader_type_id=1 and the opacity<1 fetch path.
    """
    scene = SCENES_DIR / "mat_preview_surface.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    img = _render_scene(scene, "mat_preview_surface")
    _assert_finite_nonblank(img, "preview_surface")


def test_material_mixed():
    """Mixed materials in one scene: fallback + standard_surface + UsdPreviewSurface.

    Expected: three boards — grey (left, fallback), magenta (center,
    standard_surface), cyan (right, UsdPreviewSurface). Guards that all three
    dispatch paths coexist and the per-material shader_type_id routing is
    correct (the exact thing the callable dispatch relies on).
    """
    scene = SCENES_DIR / "mat_mixed.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    img = _render_scene(scene, "mat_mixed")
    _assert_finite_nonblank(img, "mixed")


def test_material_transmission():
    """Transmission path: three glass spheres over a checkerboard.

    Expected: clear, green-tinted, and rough glass spheres refracting the
    checkerboard behind them. The dielectric BSDF + eta_flipped handling is
    the path that was hard to get right, so this is the most important
    regression guard for the standard_surface transmission logic.

    Guards shader_type_id=0 with transmission>0 (mx_dielectric_bsdf +
    sample_standard_surface transmission sampling).
    """
    scene = SCENES_DIR / "mat_transmission_sphere.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    # Refraction needs more samples to converge — the default SPP 256 covers this.
    img = _render_scene(scene, "mat_transmission_sphere")
    _assert_finite_nonblank(img, "transmission")


def test_material_thin_walled():
    """thin_walled path: two yellow-tinted sheets over a diffuse wall.

    Expected (once thin_walled transmission is implemented): the LEFT sheet
    (thin_walled=true) shows the white wall behind it tinted YELLOW by
    transmission_color=(0.95,0.85,0.10), with NO refraction distortion
    (straight-through). The RIGHT sheet (thin_walled=false, solid) shows the
    same wall but with Snell-refraction artifacts on an open surface.

    Lighting note: RectLight is now an intersectable emissive quad in the
    TLAS (commit a03535a), so a BSDF ray can hit the light geometry directly.
    The thin_walled tint itself, however, is not yet implemented — until it
    is, this test only guards that the scene renders finite and non-blank.
    """
    scene = SCENES_DIR / "mat_thin_walled.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    # The tint arrives via a diffuse bounce (indirect), which is noisier than
    # direct illumination — the default SPP 256 keeps the oblique-bend
    # comparison legible.
    img = _render_scene(scene, "mat_thin_walled")
    _assert_finite_nonblank(img, "thin_walled")
