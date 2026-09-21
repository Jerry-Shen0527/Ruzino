"""Rasterize the hang-repro scene (no PT / no TLAS) to split the black-image
bug: geometry+camera+material-blob (rasterize path) vs ray path (PT)."""

import os
import sys
import time
from pathlib import Path

import numpy as np


def _prepare_env():
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
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


def main():
    workspace_root, binary_dir = _prepare_env()
    size = int(os.environ.get("RZ_HANG_SIZE", "256"))

    import hd_RUZINO_py as renderer

    scene = Path(__file__).parent / "test_scene.usda"
    out_dir = binary_dir / "test_output" / "hang_look"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[hang-raster] size={size} scene={scene.name}", flush=True)

    hydra = renderer.HydraRenderer(str(scene), size, size)

    node_system = hydra.get_node_system()
    primary = binary_dir / "render_nodes.json"
    fallback = (binary_dir.parent.parent / "Assets" /
                "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json")
    config = primary if primary.exists() else fallback
    node_system.load_configuration(str(config))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    raster = tree.add_node("rasterize")
    present = tree.add_node("present_color")
    tree.add_link(raster.get_output_socket("DiffuseColor"),
                  present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    t0 = time.perf_counter()
    hydra.render()
    print(f"[hang-raster] render in {time.perf_counter()-t0:.2f}s", flush=True)

    texture_data = hydra.get_output_texture()
    img = np.array(texture_data, dtype=np.float32).reshape(size, size, 4)
    rgb = np.clip(img[:, :, :3], 0.0, None)
    print(f"[hang-raster] mean rgb = {rgb.mean():.4f} max = {rgb.max():.3f}",
          flush=True)

    from PIL import Image
    linear = np.clip(np.flipud(rgb), 0.0, 1.0)
    Image.fromarray((linear * 255).astype(np.uint8)).save(
        out_dir / "raster_linear.png")
    gamma = np.power(linear, 1.0 / 2.2)
    Image.fromarray((gamma * 255).astype(np.uint8)).save(
        out_dir / "raster_gamma.png")
    print(f"[hang-raster] saved raster_linear.png / raster_gamma.png",
          flush=True)


if __name__ == "__main__":
    main()
