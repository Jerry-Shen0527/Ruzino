"""
Transmission white furnace test — energy conservation for a rough dielectric.

Companion to test_white_furnace.py (which exercises the diffuse/reflection path
via UsdPreviewSurface). This test exercises the **microfacet transmission BTDF**
under ND_standard_surface_surfaceshader — the path whose Jacobian, BTDF, and pdf
were reconciled to the PBRT-v4 / Mitsuba3 / Walter 2007 standard form.

Scene: a rough glass sphere (specular=1, transmission=1, IOR=1.5, roughness=0.5,
no absorption, white transmission_color) in a uniform environment (DomeLight
Le=1). With no absorption and an energy-conserving BSDF, every ray that enters
the sphere eventually exits carrying the radiance it gathered from the
environment, so the sphere's outgoing radiance integrates to exactly Le:

    sphere_mean / environment_mean == 1.0   (within tolerance)

A ratio above 1.0 means energy is being gained (BTDF or pdf over-estimated); a
ratio below 1.0 means energy is being lost (under-estimated, or a Fresnel
component with no lobe to carry it). The pre-fix code passed this only because
the BTDF (eval) and the sampling pdf deviated from the standard form by the
SAME factor, cancelling in f/pdf on the pure-BSDF-sampling path — but the MIS
pdf was wrong, biasing the NEE/BSDF-hit-light balance. After the fix, f, pdf,
and the sampler all match PBRT, so the furnace holds for real.

specular=1 (not 0) is deliberate: it gives the reflection lobe somewhere to
put the Fresnel-reflected energy (F ~= 4% at normal incidence). With specular=0
that energy is silently dropped and the sphere reads ~4% dark — a real but
unrelated loss that would mask the Jacobian error this test targets.

Scene: data/scenes/transmission_furnace.usda.
"""

from pathlib import Path
import numpy as np
import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "transmission_furnace"

SCENE = DATA_DIR / "scenes" / "transmission_furnace.usda"

# Rough glass spreads the transmission lobe; Monte-Carlo noise at 512 SPP is
# larger than for the pure-diffuse furnace (the lobe is sharper, so more
# variance per sample). 3% tolerance is generous but still catches the pre-fix
# MIS bias (which was angle-dependent and larger than 3% at grazing angles).
RATIO_TOL = 0.03


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


def test_transmission_furnace_energy_conservation():
    """Rough glass sphere must vanish into the uniform environment (ratio=1)."""
    if not SCENE.exists():
        pytest.skip(f"{SCENE} not found")

    img = _render(SCENE, width=128, height=128, samples=512)
    _save_image(img, "transmission_furnace_512")

    rgb = img[..., :3]
    flat = rgb.reshape(-1, 3).max(axis=1).reshape(128, 128)

    # Camera at z=8, sphere r=1: the sphere spans ~y/x [18,109] of the 128px
    # frame. Interior region excludes the rim (anti-aliasing / refraction
    # caustic at the silhouette); corners sample the environment.
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
        f"transmission energy not conserved: sphere/env = {ratio:.4f} "
        f"(expect 1.0 ± {RATIO_TOL}). BTDF/pdf Jacobian mismatch or a Fresnel "
        f"component with no carrying lobe."
    )

    # No spatial std assertion here: a rough glass sphere legitimately shows
    # internal structure (refraction caustic, Fresnel rim) even when energy is
    # conserved — unlike the pure-diffuse furnace which must be spatially flat.
    # The ratio check alone is the energy-conservation criterion.
