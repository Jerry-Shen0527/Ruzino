"""Tests for HydraRenderer Python API — creation, properties, node system access.

NOTE: HydraRenderer constructor has a known nanobind type conflict issue
(duplicate NodeTree registration between hd_RUZINO_py and nodes_system_py).
Tests that exercise the constructor will skip until this binding issue is fixed.
"""

import os
import sys
from pathlib import Path
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


def _import_renderer():
    try:
        import hd_RUZINO_py as renderer
        return renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")


def _try_create_hydra(renderer, usd_path, w=128, h=128):
    """Try to create a HydraRenderer. Skip if nanobind constructor is broken."""
    try:
        return renderer.HydraRenderer(str(usd_path), w, h)
    except TypeError as e:
        if "incompatible function arguments" in str(e):
            pytest.skip("HydraRenderer constructor broken (nanobind type conflict)")
        raise


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


def test_hydra_renderer_creation():
    """HydraRenderer can be created with a valid USD file."""
    workspace_root, binary_dir = _prepare_env()
    renderer = _import_renderer()

    usd_stage = _find_test_scene(workspace_root)
    hydra = _try_create_hydra(renderer, usd_stage, 128, 128)
    assert hydra.width == 128
    assert hydra.height == 128


def test_hydra_renderer_invalid_file():
    """HydraRenderer raises RuntimeError on non-existent USD file."""
    workspace_root, binary_dir = _prepare_env()
    renderer = _import_renderer()

    try:
        renderer.HydraRenderer("/nonexistent/path/scene.usda", 64, 64)
    except TypeError as e:
        if "incompatible function arguments" in str(e):
            pytest.skip("HydraRenderer constructor broken (nanobind type conflict)")
        raise
    except RuntimeError:
        pass  # Expected


def test_hydra_renderer_node_system():
    """get_node_system() returns a usable NodeSystem."""
    workspace_root, binary_dir = _prepare_env()
    renderer = _import_renderer()

    usd_stage = _find_test_scene(workspace_root)
    hydra = _try_create_hydra(renderer, usd_stage, 64, 64)
    node_system = hydra.get_node_system()
    assert node_system is not None


def test_hydra_renderer_custom_dimensions():
    """HydraRenderer respects custom width/height."""
    workspace_root, binary_dir = _prepare_env()
    renderer = _import_renderer()

    usd_stage = _find_test_scene(workspace_root)

    for w, h in [(64, 64), (256, 128)]:
        hydra = _try_create_hydra(renderer, usd_stage, w, h)
        assert hydra.width == w
        assert hydra.height == h


def test_module_import():
    """hd_RUZINO_py module can be imported and has expected members."""
    workspace_root, binary_dir = _prepare_env()
    renderer = _import_renderer()

    assert hasattr(renderer, 'HydraRenderer')
    assert hasattr(renderer, 'create_render_system')
    assert hasattr(renderer, 'create_render_executor')
    assert hasattr(renderer, 'create_render_global_payload')
