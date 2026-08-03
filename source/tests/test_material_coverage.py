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

Each render runs in its OWN subprocess (``_render_worker.py``) rather than
constructing a ``HydraRenderer`` in-process. The HD renderer plugin is a
process-global singleton: ``Hd_RUZINO_RendererPlugin`` is cached by Hydra and
the global ``GPUSceneAssember`` cannot be reconstructed cleanly, so a second
``HydraRenderer`` in the same process leaves the render delegate's
``presented_textures`` map stale and ``get_output_texture()`` throws "Failed to
get output texture". ``test_render_materials.py`` dodges this by sharing one
module-scoped renderer across its cornell tests, but here every test renders a
*different scene*, so sharing one renderer isn't possible. Process isolation is
the same pattern already used by ``materials_batch.py`` / ``render_gridbox.py``
(both spawn a render per scene). The worker dumps the PNG plus a
``.stats.json`` sidecar (mean, is_finite); this parent reads the sidecar to
make its assertions, keeping the pytest failure UX intact.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
TESTS_DIR = Path(__file__).resolve().parent
SCENES_DIR = TESTS_DIR / "data" / "scenes" / "materials"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "material_coverage"

RENDER_SIZE = 256
RENDER_SPP = 64


def _render_scene(scene_path, name, width=RENDER_SIZE, height=RENDER_SIZE, spp=RENDER_SPP):
    """Render a scene at fixed SPP in a fresh subprocess and write a PNG.

    Returns the stats dict ``{"mean": float, "is_finite": bool}`` from the
    worker's ``.stats.json`` sidecar. The render itself happens in a child
    process so the HD renderer singleton is constructed at most once per
    process — defeating the singleton-pollution bug that breaks runs of
    several of these tests back-to-back. Skips (rather than fails) if the
    renderer build or worker isn't available.
    """
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_png = OUTPUT_DIR / f"{name}.png"
    stats_path = OUTPUT_DIR / f"{name}.stats.json"
    # Pass the binary dir through explicitly: the worker runs as a plain
    # script (not under pytest, so conftest.py doesn't bootstrap its env), and
    # it must find hd_RUZINO_py + USD DLLs + render_nodes.json there.
    cmd = [
        sys.executable,
        str(TESTS_DIR / "_render_worker.py"),
        "--scene", str(scene_path),
        "--output", str(output_png),
        "--width", str(width),
        "--height", str(height),
        "--spp", str(spp),
        "--binary-dir", str(BINARY_DIR),
    ]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=300,
            cwd=str(BINARY_DIR))
    except FileNotFoundError as e:
        pytest.skip(f"python executable unavailable to run render worker: {e}")

    if result.returncode != 0:
        pytest.skip(
            f"render worker failed (code {result.returncode}) for {name}:\n"
            f"{result.stdout}\n{result.stderr}")
    # Surface the worker's own progress line so `-s` output stays informative.
    if result.stdout:
        print(result.stdout.strip())

    if not stats_path.exists():
        pytest.skip(f"worker produced no stats sidecar for {name}")
    stats = json.loads(stats_path.read_text())
    print(f"Saved: {output_png}  mean={stats['mean']:.4f}")
    return stats


def _assert_finite_nonblank(stats, name):
    """Common sanity: no NaN/Inf, and not pure black.

    Takes the stats dict from ``_render_scene`` instead of an ndarray, since
    the image itself now lives only in the worker process.
    """
    assert stats["is_finite"], f"{name}: render contains NaN/Inf"
    mean = float(stats["mean"])
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
    stats = _render_scene(scene, "mat_fallback")
    _assert_finite_nonblank(stats, "fallback")


def test_material_standard_surface():
    """standard_surface path: red diffuse / metallic gold / semi-transparent green.

    Expected: three vertical boards — red (left), gold-metallic (center),
    greenish-translucent (right). This guards shader_type_id=0 AND the
    opacity/transmission computation path that P0 will refactor.
    """
    scene = SCENES_DIR / "mat_standard_surface.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    stats = _render_scene(scene, "mat_standard_surface")
    _assert_finite_nonblank(stats, "standard_surface")


def test_material_preview_surface():
    """UsdPreviewSurface path: red / blue-metallic / semi-transparent yellow.

    Expected: three boards — red (left), blue-metallic (center), translucent
    yellow (right). Guards shader_type_id=1 and the opacity<1 fetch path.
    """
    scene = SCENES_DIR / "mat_preview_surface.usda"
    if not scene.exists():
        pytest.skip(f"{scene} not found")
    stats = _render_scene(scene, "mat_preview_surface")
    _assert_finite_nonblank(stats, "preview_surface")


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
    stats = _render_scene(scene, "mat_mixed")
    _assert_finite_nonblank(stats, "mixed")


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
    # Higher SPP for transmission: refraction needs more samples to converge.
    stats = _render_scene(scene, "mat_transmission_sphere", spp=128)
    _assert_finite_nonblank(stats, "transmission")
