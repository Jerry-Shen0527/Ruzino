"""
Rendering tests — verify materials, geometry, and lighting render correctly.

Uses the path tracing pipeline with manually built render graph
(same pattern as source/Runtime/renderer/tests/test_rendering.py).
Environment setup is handled by conftest.py.
"""

import os
from pathlib import Path
import numpy as np
import pytest

from conftest import TEST_OUTPUT_DIR
import _render_compare

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
# Test output lives under Binaries/Release/test_output/ (never the source tree).
# data/reference/ holds the committed anchor image (the one allowed render
# product in source); data/scenes/ holds the small input scenes.
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "render_materials"
REFERENCE_DIR = DATA_DIR / "reference"
ANCHOR_PATH = REFERENCE_DIR / "cornell_box_anchor"


def _save_image(img, name):
    """Save rendered image as .npy and .png for inspection."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    np.save(OUTPUT_DIR / f"{name}.npy", img)
    # After LPM + gamma, values should be in [0,1] range
    rgb = np.clip(img[:, :, :3], 0, 1)
    rgb = (rgb * 255).astype(np.uint8)
    try:
        from PIL import Image
        Image.fromarray(rgb).save(OUTPUT_DIR / f"{name}.png")
    except ImportError:
        pass
    print(f"Saved: {name} shape={img.shape} mean={img[:,:,:3].mean():.4f}")


def _locate_config():
    primary = BINARY_DIR / "render_nodes.json"
    if primary.exists():
        return primary
    pytest.skip("render_nodes.json not found in Binaries/Release")


def _build_render_graph(hydra, samples=4):
    """Build the full path tracing render graph with tone mapping.

    Matches the pipeline from Assets/Hd_RUZINO_RendererPlugin/render_nodes_save.json:
    rng_texture → ray_gen → path_tracing → accumulate → lpm → gamma_correction → present_color
                                                      ↑
                                                 rng_buffer
    """
    import nodes_core_py as core

    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config()))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture"); rng.ui_name = "RNG"
    ray_gen = tree.add_node("node_render_ray_generation"); ray_gen.ui_name = "RayGen"
    path_trace = tree.add_node("path_tracing"); path_trace.ui_name = "PathTracer"
    accumulate = tree.add_node("accumulate"); accumulate.ui_name = "Accumulate"
    rng_buffer = tree.add_node("rng_buffer"); rng_buffer.ui_name = "RNGBuffer"
    lpm = tree.add_node("lpm"); lpm.ui_name = "LPM"
    gamma = tree.add_node("gamma_correction"); gamma.ui_name = "Gamma"
    present = tree.add_node("present_color"); present.ui_name = "Present"

    tree.add_link(rng.get_output_socket("Random Number"), ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"), path_trace.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"), path_trace.get_input_socket("Rays"))
    tree.add_link(rng_buffer.get_output_socket("Random Number"), path_trace.get_input_socket("Random Seeds"))
    tree.add_link(path_trace.get_output_socket("Output"), accumulate.get_input_socket("Texture"))
    tree.add_link(accumulate.get_output_socket("Accumulated"), lpm.get_input_socket("Input Color"))
    tree.add_link(lpm.get_output_socket("Output Color"), gamma.get_input_socket("Texture"))
    tree.add_link(gamma.get_output_socket("Corrected"), present.get_input_socket("Color"))

    # Vec params — set BEFORE prepare_tree so the executor picks them up
    # from socket dataField during initialization.
    # Uses default_value_typed_force to write bytes in-place, preserving
    # the socket's original C++ type (e.g. GfVec3f).
    vec_params = {
        (lpm, "Crosstalk"): [0.471, 0.49, 0.504],
    }
    for (node, socket_name), value in vec_params.items():
        socket = node.get_input_socket(socket_name)
        socket.set_default_value(value)

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    # Scalar params — via sync_node_from_external_storage (after prepare_tree)
    scalar_params = {
        (ray_gen, "Aperture"): 0.0,
        (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
        (gamma, "Gamma"): 2.2,
        # LPM tone mapping (from render_nodes_save.json)
        (lpm, "LPM Exposure"): 0.0,
        (lpm, "HDR Max"): 2.0,
        (lpm, "Contrast"): 1.0,
        (lpm, "Shoulder"): 1.0,
        (lpm, "Shoulder Contrast"): 1.0,
        (lpm, "Soft Gap"): 0.0,
        (lpm, "Color Space"): 0,
        (lpm, "Display Mode"): 0,
        (lpm, "Display Max Luminance"): 1000.0,
        (lpm, "Display Min Luminance"): 0.0,
    }
    for (node, socket_name), value in scalar_params.items():
        socket = node.get_input_socket(socket_name)
        meta = core.to_meta_any(value)
        executor.sync_node_from_external_storage(socket, meta)

    # Vec params that differ from node defaults — also sync to executor
    for (node, socket_name), value in vec_params.items():
        socket = node.get_input_socket(socket_name)
        socket.set_default_value(value)


def _render(scene_path, width=128, height=128, samples=4, save_name=None):
    """Render a scene using the path tracing pipeline."""
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

    # GPU textures have origin at top-left; flip to match camera/scene Y-up
    img = np.flipud(img)

    if save_name:
        _save_image(img, save_name)

    return img


def _make_incremental_prober(scene_path, width, height, max_samples):
    """Build one HydraRenderer and return a probe(spp) closure for convergence.

    The HD renderer plugin is a process-global singleton that does not tolerate
    repeated HydraRenderer construction/teardown within one process. A
    convergence sweep needs many renders at increasing SPP, so we build *one*
    renderer and drive it incrementally: each probe resets the accumulator and
    renders exactly ``spp`` frames, yielding that SPP's accumulated image.

    ``max_samples`` sets the accumulate node's Max Samples (a ceiling; rendering
    fewer frames than it is fine — the accumulator averages what it receives).

    Returns ``probe(spp) -> HxWx4 float32 ndarray``.
    """
    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    hydra = renderer.HydraRenderer(str(scene_path), width, height)
    _build_render_graph(hydra, max_samples)

    def probe(spp):
        hydra.reset_accumulation()
        for _ in range(spp):
            hydra.render()
        texture_data = hydra.get_output_texture()
        img = np.array(texture_data, dtype=np.float32).reshape(height, width, 4)
        # GPU textures have origin at top-left; flip to match camera/scene Y-up
        return np.flipud(img)

    return probe


# --- Cornell Box tests ---
#
# Outputs (PNG/diagnostics) go to Binaries/Release/test_output/render_materials/.
# The committed reference anchor lives in source/tests/data/reference/ (the one
# allowed render product in the source tree). Tests never write anywhere else
# under source/.
#
# The HD renderer plugin is a process-global singleton: building more than a
# couple of HydraRenderer instances in one process corrupts it ("Invalid plugin
# id Hd_RUZINO_RendererPlugin"). So all cornell tests share ONE renderer via the
# session-scoped `cornell_prober` fixture, driving it incrementally with
# reset_accumulation(). Different SPP are just different probe counts on the
# same renderer.


SCENE_CORNELL = DATA_DIR / "scenes" / "cornell_box.usda"
CORNELL_SIZE = 256


@pytest.fixture(scope="module")
def cornell_prober():
    """A shared incremental probe(spp) closure for the Cornell Box scene.

    Module-scoped so every cornell test reuses the single underlying
    HydraRenderer. Each call resets the accumulator and renders ``spp`` frames.
    """
    if not SCENE_CORNELL.exists():
        pytest.skip("cornell_box.usda not found")
    return _make_incremental_prober(
        SCENE_CORNELL, CORNELL_SIZE, CORNELL_SIZE, max_samples=256)


def test_cornell_box_renders(cornell_prober):
    """Cornell Box scene should produce non-trivial output (smoke test)."""
    img = cornell_prober(32)
    _save_image(img, "cornell_box_smoke")
    mean_val = float(img[:, :, :3].mean())
    assert mean_val > 0.01, f"Render too dim (mean={mean_val:.4f})"


def test_cornell_box_finite(cornell_prober):
    """Cornell Box output should be finite (no NaN/Inf)."""
    img = cornell_prober(32)
    assert np.isfinite(img).all(), "Render contains NaN or Inf"


def test_cornell_box_wall_colors(cornell_prober):
    """Left wall should be red-tinted, right wall green-tinted."""
    img = cornell_prober(32)
    h, w = img.shape[:2]

    # Sample vertical strips from left and right walls
    left_strip = img[:, :w // 4, :3]
    right_strip = img[:, 3 * w // 4:, :3]

    left_mean = left_strip.mean(axis=(0, 1))
    right_mean = right_strip.mean(axis=(0, 1))

    assert left_mean[0] > left_mean[1], \
        f"Left wall should be red-dominant, R={left_mean[0]:.4f} G={left_mean[1]:.4f}"
    assert right_mean[1] > right_mean[0], \
        f"Right wall should be green-dominant, G={right_mean[1]:.4f} R={right_mean[0]:.4f}"


def test_cornell_box_all_regions_visible(cornell_prober):
    """Floor, ceiling, back wall, and blocks should all produce visible output."""
    img = cornell_prober(32)
    h, w = img.shape[:2]

    # Check center region (back wall + blocks) is visible
    center = img[h // 4: 3 * h // 4, w // 4: 3 * w // 4, :3]
    center_mean = float(center.mean())
    assert center_mean > 0.01, f"Center region too dim (mean={center_mean:.4f})"

    # Check top region (ceiling) has some light
    top = img[:h // 4, :, :3]
    top_mean = float(top.mean())
    assert top_mean > 0.001, f"Top region (ceiling) too dim (mean={top_mean:.4f})"


def test_cornell_box_converges(cornell_prober):
    """As SPP grows, the render must converge toward the committed anchor.

    The anchor (data/reference/cornell_box_anchor.*, 512 SPP) is the convergence
    target. Probes at [16, 32, 64, 128, 256] SPP are each compared to the anchor
    by per-channel MSE. A correct path tracer yields a monotonically decreasing
    MSE curve that drops substantially — we judge the curve shape, not absolute
    values, so GPU/driver/RNG differences across machines don't cause false
    failures.

    Reuses the module-shared HydraRenderer (cornell_prober fixture). Anchor
    generation is handled once by source/tests/render_anchor.py and committed;
    the test skips if it is missing (run the script first).
    """
    if not ANCHOR_PATH.with_suffix(".npy").exists() and \
       not ANCHOR_PATH.with_suffix(".png").exists():
        pytest.skip(
            f"anchor not found at {ANCHOR_PATH}; run render_anchor.py to "
            f"generate and commit it first")

    probe_spps = [16, 32, 64, 128, 256]

    # Diagnostics (anchor.png, probe_<spp>.png, mse_curve.json) land under
    # OUTPUT_DIR (Binaries), never the source tree.
    curve = _render_compare.render_convergence(
        cornell_prober, ANCHOR_PATH, probe_spps, OUTPUT_DIR,
        width=CORNELL_SIZE, height=CORNELL_SIZE, anchor_spp=512)
    _render_compare.assert_converging(curve)
