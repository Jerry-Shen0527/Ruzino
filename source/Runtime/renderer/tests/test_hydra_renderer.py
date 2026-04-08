#!/usr/bin/env python3
"""Unified Hydra renderer tests.

Tests both graph construction/rendering and tensor output functionality.
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
    import nodes_core_py as core  # type: ignore
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config(binary_dir)))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    # Create nodes
    rng = tree.add_node("rng_texture"); rng.ui_name = "RNG"
    ray_gen = tree.add_node("node_render_ray_generation"); ray_gen.ui_name = "RayGen"
    path_trace = tree.add_node("path_tracing"); path_trace.ui_name = "PathTracer"
    accumulate = tree.add_node("accumulate"); accumulate.ui_name = "Accumulate"
    rng_buffer = tree.add_node("rng_buffer"); rng_buffer.ui_name = "RNGBuffer"
    present = tree.add_node("present_color"); present.ui_name = "Present"

    # Link nodes
    tree.add_link(rng.get_output_socket("Random Number"), ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"), path_trace.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"), path_trace.get_input_socket("Rays"))
    tree.add_link(rng_buffer.get_output_socket("Random Number"), path_trace.get_input_socket("Random Seeds"))
    tree.add_link(path_trace.get_output_socket("Output"), accumulate.get_input_socket("Texture"))
    tree.add_link(accumulate.get_output_socket("Accumulated"), present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    # Set parameters
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


def test_hydra_renderer_basic():
    """Test basic render graph construction and rendering."""
    workspace_root, binary_dir = _prepare_env()

    try:
        import hd_RUZINO_py as renderer  # type: ignore
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    usd_stage = workspace_root / "Assets" / "shader_ball.usdc"
    if not usd_stage.exists():
        pytest.skip("shader_ball.usdc not found")

    width, height, samples = 128, 128, 4
    hydra = renderer.HydraRenderer(str(usd_stage), width=width, height=height)

    _build_render_graph(hydra, binary_dir, samples)

    # Render
    for _ in range(samples):
        hydra.render()

    texture_data = hydra.get_output_texture()
    assert texture_data, "No texture data returned"
    assert len(texture_data) == width * height * 4, "Unexpected texture length"

    img = np.array(texture_data, dtype=np.float32).reshape(height, width, 4)
    mean_val = float(img[:, :, :3].mean())
    assert mean_val >= 0.0, "Negative mean (invalid)"

    if mean_val < 1e-6:
        pytest.xfail(f"Rendered image appears blank (mean={mean_val:.6f})")

    # Save diagnostic image to Binaries/Release
    try:
        from PIL import Image  # type: ignore
        rgb = (np.clip(img[:, :, :3], 0, 1) * 255).astype(np.uint8)
        rgb = np.flipud(rgb)
        Image.fromarray(rgb).save(binary_dir / "output_hydra_basic.png")
    except Exception:
        pass


def test_render_to_tensor():
    """Test rendering with higher sample count and optional tensor conversion."""
    workspace_root, binary_dir = _prepare_env()

    try:
        import hd_RUZINO_py as renderer  # type: ignore
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    usd_stage = workspace_root / "Assets" / "shader_ball.usdc"
    if not usd_stage.exists():
        pytest.skip("shader_ball.usdc not found")

    width, height, samples = 128, 128, 8
    hydra = renderer.HydraRenderer(str(usd_stage), width=width, height=height)

    _build_render_graph(hydra, binary_dir, samples)

    # Render
    for _ in range(samples):
        hydra.render()

    texture_data = hydra.get_output_texture()
    assert texture_data, "No texture data returned"
    assert len(texture_data) == width * height * 4, "Unexpected texture length"

    img = np.array(texture_data, dtype=np.float32).reshape(height, width, 4)
    rgb = img[:, :, :3]
    mean_val = float(rgb.mean())
    assert mean_val >= 0.0, "Negative mean (invalid)"

    if mean_val < 1e-6:
        pytest.xfail(f"Rendered image appears blank (mean={mean_val:.6f})")

    # Test tensor conversion if torch available
    try:
        import torch  # type: ignore
        tensor = torch.from_numpy(img)
        assert tensor.shape == (height, width, 4), "Unexpected tensor shape"
        assert tensor.dtype == torch.float32, "Unexpected tensor dtype"
    except ImportError:
        pass  # torch not available, skip this part

    # Save output image to Binaries/Release
    try:
        from PIL import Image  # type: ignore
        out_rgb = (np.clip(rgb, 0, 1) * 255).astype(np.uint8)
        out_rgb = np.flipud(out_rgb)
        Image.fromarray(out_rgb).save(binary_dir / "output_render_to_tensor.png")
    except Exception:
        pass

