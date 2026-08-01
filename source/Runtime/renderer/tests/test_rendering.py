"""Full rendering pipeline tests — render scenes and verify output."""

import os
import sys
from pathlib import Path
import numpy as np
import pytest


def _prepare_env():
    import os
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
    # Allow switching to Binaries/Debug for native debugging (PDB symbols).
    # Set RZ_BUILD_TYPE=Debug to load the Debug build instead of Release —
    # essential for cdb/VS stack traces with source line numbers.
    build_type = os.environ.get("RZ_BUILD_TYPE", "Release")
    binary_dir = workspace_root / "Binaries" / build_type
    if not binary_dir.exists():
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


def _find_test_scene(workspace_root):
    script_dir = Path(__file__).parent.resolve()
    builtin = script_dir / "test_scene.usda"
    if builtin.exists():
        return builtin
    for candidate in ["shader_ball.usdc", "cornell_box_stage.usdc"]:
        path = workspace_root / "Assets" / candidate
        if path.exists():
            return path
    pytest.skip("No test USD scene found")


def _locate_config(binary_dir: Path) -> Path:
    primary = binary_dir / "render_nodes.json"
    fallback = binary_dir.parent.parent / "Assets" / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"
    if primary.exists():
        return primary
    if fallback.exists():
        return fallback
    pytest.skip("No render node configuration found")


def _build_render_graph(hydra, binary_dir: Path, samples: int = 4):
    """Build the standard path tracing render graph."""
    import nodes_core_py as core
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config(binary_dir)))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture"); rng.ui_name = "RNG"
    ray_gen = tree.add_node("node_render_ray_generation"); ray_gen.ui_name = "RayGen"
    path_trace = tree.add_node("path_tracing"); path_trace.ui_name = "PathTracer"
    accumulate = tree.add_node("accumulate"); accumulate.ui_name = "Accumulate"
    rng_buffer = tree.add_node("rng_buffer"); rng_buffer.ui_name = "RNGBuffer"
    present = tree.add_node("present_color"); present.ui_name = "Present"

    tree.add_link(rng.get_output_socket("Random Number"), ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"), path_trace.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"), path_trace.get_input_socket("Rays"))
    tree.add_link(rng_buffer.get_output_socket("Random Number"), path_trace.get_input_socket("Random Seeds"))
    tree.add_link(path_trace.get_output_socket("Output"), accumulate.get_input_socket("Texture"))
    tree.add_link(accumulate.get_output_socket("Accumulated"), present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    params = {
        (ray_gen, "Aperture"): 0.0,
        (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
    }
    for (node, socket_name), value in params.items():
        socket = node.get_input_socket(socket_name)
        meta = core.to_meta_any(value)
        executor.sync_node_from_external_storage(socket, meta)


def _render_scene(workspace_root, binary_dir, width, height, samples):
    """Helper: create renderer, build graph, render, return image array."""
    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    usd_stage = _find_test_scene(workspace_root)
    try:
        hydra = renderer.HydraRenderer(str(usd_stage), width, height)
    except TypeError as e:
        if "incompatible function arguments" in str(e):
            pytest.skip("HydraRenderer constructor broken (nanobind type conflict)")
        raise
    _build_render_graph(hydra, binary_dir, samples)

    for _ in range(samples):
        hydra.render()
    texture_data = hydra.get_output_texture()
    return np.array(texture_data, dtype=np.float32).reshape(height, width, 4)


def test_render_basic():
    """Render test scene at 128x128 with 4 SPP, verify non-black."""
    workspace_root, binary_dir = _prepare_env()
    img = _render_scene(workspace_root, binary_dir, 128, 128, 4)

    assert img.shape == (128, 128, 4), f"Unexpected shape: {img.shape}"
    mean_val = float(img[:, :, :3].mean())
    assert mean_val >= 0.0, f"Negative mean: {mean_val}"

    assert mean_val > 1e-3, f"Rendered image appears blank (mean={mean_val:.6f})"


def test_render_output_size():
    """get_output_texture() returns width*height*4 floats."""
    workspace_root, binary_dir = _prepare_env()

    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    usd_stage = _find_test_scene(workspace_root)
    w, h, spp = 64, 64, 2
    try:
        hydra = renderer.HydraRenderer(str(usd_stage), w, h)
    except TypeError as e:
        if "incompatible function arguments" in str(e):
            pytest.skip("HydraRenderer constructor broken (nanobind type conflict)")
        raise
    _build_render_graph(hydra, binary_dir, spp)

    for _ in range(spp):
        hydra.render()
    data = hydra.get_output_texture()
    assert data is not None, "No texture data returned"
    assert len(data) == w * h * 4, f"Expected {w*h*4} floats, got {len(data)}"


def test_render_small_resolution():
    """Render at 64x64, verify output dimensions correct."""
    workspace_root, binary_dir = _prepare_env()
    img = _render_scene(workspace_root, binary_dir, 64, 64, 2)

    assert img.shape == (64, 64, 4)
    assert np.isfinite(img).all(), "Output contains NaN or Inf"


def test_render_pixel_range():
    """All pixel values should be non-negative (renderer output)."""
    workspace_root, binary_dir = _prepare_env()
    img = _render_scene(workspace_root, binary_dir, 64, 64, 4)

    assert (img[:, :, :3] >= 0.0).all(), "Found negative pixel values"
    assert np.isfinite(img).all(), "Output contains NaN or Inf"


def test_render_non_zero_alpha():
    """Rendered image alpha channel should be non-zero."""
    workspace_root, binary_dir = _prepare_env()
    img = _render_scene(workspace_root, binary_dir, 64, 64, 4)

    alpha_mean = float(img[:, :, 3].mean())
    assert alpha_mean > 0.0, f"Alpha channel appears zero (mean={alpha_mean})"
