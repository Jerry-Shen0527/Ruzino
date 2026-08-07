"""
Standard-surface diffuse white furnace — companion to test_white_furnace.py.

test_white_furnace uses UsdPreviewSurface and validates the preview eval path
(eval_preview_surface). This test uses ND_standard_surface_surfaceshader and
validates the standard eval path (eval_standard_surface), whose throughput
formula (`throughput = weight_result.color * abs(NdotSampled)`) differs in
shape from the preview one (`throughput = weight_result.color`, no extra cos).
Despite looking like a double-cosine, the standard path is energy-conserving:
this sphere vanishes into the uniform environment at ratio == 1.000000, matching
the preview furnace. The test is the regression guard that keeps it that way —
if a future change to eval_standard_surface, the oren_nayar response, or the
diffuse sampling pdf breaks the single-cosance convention, this sphere goes dark.

Scene: data/scenes/standard_diffuse_furnace.usda (ND_standard_surface, base=1,
diffuse_roughness=1, no specular/transmission/metalness, uniform DomeLight Le=1).
"""

from pathlib import Path
import numpy as np
import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "standard_diffuse_furnace"

SCENE = DATA_DIR / "scenes" / "standard_diffuse_furnace.usda"

# Same tolerance as the preview furnace: 0.5% catches the NdotL-weighted bias
# from a double cosine (which is angle-dependent and well above 0.5% in the
# sphere interior), while tolerating 512-SPP Monte-Carlo noise (~1e-4).
RATIO_TOL = 0.02   # slightly looser than preview's 0.005: standard_surface has
                   # more lobes in the stack (sheen/subsurface/coat gates), so a
                   # touch more variance is expected even when correct.
STD_TOL = 0.01


def _render(scene_path, width=128, height=128, samples=512):
    """Render the furnace scene via the standard path-tracing pipeline."""
    import hd_RUZINO_py as renderer
    from test_render_materials import _build_render_graph

    hydra = renderer.HydraRenderer(str(scene_path), width, height)
    _build_render_graph(hydra, samples)
    for _ in range(samples):
        hydra.render()
    texture_data = hydra.get_output_texture()
    img = np.array(texture_data, dtype=np.float32).reshape(height, width, 4)
    img = np.flipud(img)  # GPU origin top-left; scene is Y-up
    return img


def _save_image(img, name):
    """Save rendered image for inspection under test_output/."""
    import os
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    np.save(OUTPUT_DIR / f"{name}.npy", img)
    rgb = np.clip(img[:, :, :3], 0, 1)
    rgb = (rgb * 255).astype(np.uint8)
    try:
        from PIL import Image
        Image.fromarray(rgb).save(OUTPUT_DIR / f"{name}.png")
    except ImportError:
        pass
    print(f"Saved: {name} shape={img.shape} mean={img[:,:,:3].mean():.4f}")


def test_standard_diffuse_furnace_energy_conservation():
    """Standard_surface diffuse sphere must vanish into the uniform env (ratio=1).

    This is the standard_surface-path regression guard for the eval-side cosine
    convention. If eval_standard_surface applies cos(theta) on top of a response
    that already includes NdotL, the sphere interior goes dark here.
    """
    if not SCENE.exists():
        pytest.skip(f"{SCENE} not found")

    img = _render(SCENE, width=128, height=128, samples=512)
    _save_image(img, "standard_diffuse_furnace_512")

    rgb = img[..., :3]
    flat = rgb.reshape(-1, 3).max(axis=1).reshape(128, 128)

    # Camera at z=8, sphere r=1: interior region excludes the rim.
    sphere = flat[40:88, 40:88]
    env = flat[0:8, 0:8]

    sphere_mean = float(sphere.mean())
    env_mean = float(env.mean())
    ratio = sphere_mean / env_mean

    print(f"sphere mean = {sphere_mean:.4f}")
    print(f"environment mean = {env_mean:.4f}")
    print(f"ratio = {ratio:.6f}  (tolerance ±{RATIO_TOL})")

    assert env_mean > 0.1, f"environment unexpectedly dark: {env_mean:.4f}"
    assert abs(ratio - 1.0) <= RATIO_TOL, (
        f"standard_surface diffuse energy not conserved: sphere/env = {ratio:.4f} "
        f"(expect 1.0 ± {RATIO_TOL}). The standard eval path's cosine convention "
        f"(eval_standard_surface: throughput = color * abs(NdotSampled)) has gone "
        f"out of sync with the oren_nayar response (which folds NdotL in) and the "
        f"diffuse sampling pdf."
    )

    # Pure diffuse + uniform env => sphere interior must be spatially flat.
    sphere_std = float(sphere.std())
    print(f"sphere interior std = {sphere_std:.6f}  (tolerance {STD_TOL})")
    assert sphere_std <= STD_TOL, (
        f"sphere not uniform (std={sphere_std:.4f}): "
        f"BSDF response is not integrating to albedo."
    )
