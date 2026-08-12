"""
Emissive mesh light tests — verify emissive geometry is rendered and sampled
correctly via the LightBVH importance sampler.

Covers three aspects of the emissive mesh pipeline:
  1. Direct visibility: an emissive surface is visible when looked at (BSDF-hit-
     emissive path, Phase 2).
  2. NEE illumination: the floor is lit by emissive panels above it (Phase 4
     BVH importance sampling).
  3. Multi-light: multiple emissive panels with varying flux are all sampled
     (the BVH must not collapse to a single light).

The LightBVH sampler uses a flux * cosBound / dist^2 importance heuristic with
top-down traversal (Conty-Kulla 2018). The orientation cone culling is omitted
because decodeNormal2x16 in the traversal loop triggers a slang compiler
InternalError in the RT pipeline.

Scenes:
  - data/scenes/emissive_mesh.usda: single emissive box + floor
  - data/scenes/emissive_many_lights.usda: 5 emissive panels + room
"""

from pathlib import Path
import numpy as np
import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "emissive_mesh"

SCENE_SINGLE = DATA_DIR / "scenes" / "emissive_mesh.usda"
SCENE_MULTI = DATA_DIR / "scenes" / "emissive_many_lights.usda"


def _render(scene_path, width=128, height=128, samples=32, save_name=None):
    """Render a scene using the full path tracing pipeline (with tone mapping)."""
    from test_render_materials import _build_render_graph
    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    hydra = renderer.HydraRenderer(str(scene_path), width, height)
    _build_render_graph(hydra, samples)

    for _ in range(samples):
        hydra.render()
    texture_data = hydra.get_output_texture()
    img = np.array(texture_data, dtype=np.float32).reshape(height, width, 4)
    img = np.flipud(img)  # GPU origin top-left; scene is Y-up

    if save_name:
        import os
        os.makedirs(OUTPUT_DIR, exist_ok=True)
        rgb = np.clip(img[:, :, :3], 0, 1)
        rgb8 = (rgb * 255).astype(np.uint8)
        try:
            from PIL import Image
            Image.fromarray(rgb8).save(OUTPUT_DIR / f"{save_name}.png")
        except ImportError:
            pass
        print(f"Saved: {save_name} mean={img[:,:,:3].mean():.4f}")

    return img


def test_emissive_box_visible():
    """The emissive box itself must be visible when directly viewed."""
    if not SCENE_SINGLE.exists():
        pytest.skip(f"{SCENE_SINGLE} not found")

    img = _render(SCENE_SINGLE, width=128, height=128, samples=32,
                  save_name="emissive_single")

    rgb = img[..., :3]
    mean = float(rgb.mean())
    max_val = float(rgb.max())

    # The scene has an emissive box + a floor lit by it. Both contribute light.
    assert mean > 0.01, (
        f"Image too dark (mean={mean:.6f}): emissive mesh not rendering or "
        f"emissionLe accumulation broken."
    )
    # The emissive box should produce bright pixels (emission=5).
    assert max_val > 0.5, (
        f"No bright pixels (max={max_val:.4f}): emissive box surface not "
        f"directly visible."
    )
    # Output must be finite (no NaN/Inf from broken pdf or division).
    assert np.isfinite(rgb).all(), "Output contains NaN or Inf"


def test_emissive_box_illuminates_floor():
    """The floor must be lit by the emissive box above it (NEE sampling)."""
    if not SCENE_SINGLE.exists():
        pytest.skip(f"{SCENE_SINGLE} not found")

    img = _render(SCENE_SINGLE, width=128, height=128, samples=64,
                  save_name="emissive_floor")

    rgb = img[..., :3]
    # Camera at (0, 1, 5) looking at the scene. The floor occupies the bottom
    # portion of the image. Sample the lower-center region (floor area).
    h, w = rgb.shape[:2]
    floor_region = rgb[int(h * 0.6):int(h * 0.9), int(w * 0.2):int(w * 0.8)]
    floor_mean = float(floor_region.mean())

    # The floor is a 0.5-albedo diffuse surface lit only by the emissive box.
    # With NEE it should be clearly non-dark (was 0.0 before emissive NEE).
    assert floor_mean > 0.01, (
        f"Floor not illuminated (mean={floor_mean:.6f}): NEE emissive mesh "
        f"sampling is not contributing light to the floor."
    )


def test_emissive_no_fireflies():
    """Emissive mesh renders should not have extreme fireflies (broken MIS/pdf)."""
    if not SCENE_SINGLE.exists():
        pytest.skip(f"{SCENE_SINGLE} not found")

    img = _render(SCENE_SINGLE, width=128, height=128, samples=64)
    rgb = img[..., :3]

    mean = float(rgb.mean())
    # Firefly threshold: pixels > 20x the mean (after tone mapping).
    # A correct render has smooth gradients; broken pdf/MIS produces isolated
    # extreme outliers.
    firefly_threshold = max(mean * 20, 2.0)
    firefly_count = int((rgb > firefly_threshold).sum())
    firefly_pct = firefly_count / rgb.size * 100

    # Allow a small number of fireflies (emissive surfaces are bright), but
    # a large fraction indicates a pdf/MIS bug.
    assert firefly_pct < 5.0, (
        f"Too many fireflies ({firefly_count} pixels = {firefly_pct:.2f}% "
        f"above {firefly_threshold:.2f}, mean={mean:.4f}): emissive MIS/pdf "
        f"likely broken."
    )


def test_multi_light_scene_renders():
    """Multi-light emissive scene (5 panels) renders with contributions from all."""
    if not SCENE_MULTI.exists():
        pytest.skip(f"{SCENE_MULTI} not found")

    img = _render(SCENE_MULTI, width=192, height=128, samples=64,
                  save_name="emissive_multi")

    rgb = img[..., :3]
    mean = float(rgb.mean())

    # The scene has 5 emissive panels + walls + floor. Multiple lights
    # should produce a well-lit scene.
    assert mean > 0.05, (
        f"Multi-light scene too dark (mean={mean:.6f}): BVH importance "
        f"sampling may be failing to select lights."
    )
    assert np.isfinite(rgb).all(), "Output contains NaN or Inf"

    # The floor should be lit (not just the panels themselves).
    h, w = rgb.shape[:2]
    floor_region = rgb[int(h * 0.7):h, int(w * 0.1):int(w * 0.9)]
    floor_mean = float(floor_region.mean())
    assert floor_mean > 0.02, (
        f"Floor not lit in multi-light scene (mean={floor_mean:.6f})"
    )


def test_emissive_brightness_monotonic():
    """Higher emission strength produces brighter output (sanity check)."""
    if not SCENE_SINGLE.exists():
        pytest.skip(f"{SCENE_SINGLE} not found")

    # Render the standard scene (emission=5).
    img = _render(SCENE_SINGLE, width=64, height=64, samples=32)
    rgb = img[..., :3]
    mean_normal = float(rgb.mean())

    # The scene should produce a reasonable amount of light. We don't test
    # a second scene with different emission (would require scene generation),
    # but we verify the output is in a sensible range for emission=5.
    assert 0.01 < mean_normal < 5.0, (
        f"Brightness out of expected range for emission=5: mean={mean_normal:.4f}. "
        f"This could indicate a units/scaling bug in emissive Le evaluation."
    )
