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

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = DATA_DIR / "output"


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


# --- Cornell Box tests ---


def test_cornell_box_renders():
    """Cornell Box scene should produce non-trivial output."""
    scene = DATA_DIR / "scenes" / "cornell_box.usda"
    if not scene.exists():
        pytest.skip("cornell_box.usda not found")

    img = _render(scene, 1024, 1024, 64, save_name="cornell_box")
    mean_val = float(img[:, :, :3].mean())
    assert mean_val > 0.01, f"Render too dim (mean={mean_val:.4f})"


def test_cornell_box_finite():
    """Cornell Box output should be finite (no NaN/Inf)."""
    scene = DATA_DIR / "scenes" / "cornell_box.usda"
    if not scene.exists():
        pytest.skip("cornell_box.usda not found")

    img = _render(scene, 256, 256, 64, save_name="cornell_box_finite")
    assert np.isfinite(img).all(), "Render contains NaN or Inf"


def test_cornell_box_wall_colors():
    """Left wall should be red-tinted, right wall green-tinted."""
    scene = DATA_DIR / "scenes" / "cornell_box.usda"
    if not scene.exists():
        pytest.skip("cornell_box.usda not found")

    img = _render(scene, 256, 256, 64, save_name="cornell_box_colors")
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


def test_cornell_box_all_regions_visible():
    """Floor, ceiling, back wall, and blocks should all produce visible output."""
    scene = DATA_DIR / "scenes" / "cornell_box.usda"
    if not scene.exists():
        pytest.skip("cornell_box.usda not found")

    img = _render(scene, 256, 256, 64, save_name="cornell_box_regions")
    h, w = img.shape[:2]

    # Check center region (back wall + blocks) is visible
    center = img[h // 4: 3 * h // 4, w // 4: 3 * w // 4, :3]
    center_mean = float(center.mean())
    assert center_mean > 0.01, f"Center region too dim (mean={center_mean:.4f})"

    # Check top region (ceiling) has some light
    top = img[:h // 4, :, :3]
    top_mean = float(top.mean())
    assert top_mean > 0.001, f"Top region (ceiling) too dim (mean={top_mean:.4f})"
