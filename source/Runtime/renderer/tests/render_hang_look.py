"""Render the hang-repro test scene at SAFE parameters (low bounces) and save
PNGs for visual inspection: is the PT output a sane image at all?"""

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
    bounces = int(os.environ.get("RZ_HANG_BOUNCES", "2"))
    samples = int(os.environ.get("RZ_HANG_SPP", "32"))
    size = int(os.environ.get("RZ_HANG_SIZE", "256"))

    import hd_RUZINO_py as renderer
    import nodes_core_py as core

    scene = Path(__file__).parent / os.environ.get(
        "RZ_HANG_SCENE", "test_scene.usda")
    out_dir = binary_dir / "test_output" / "hang_look"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[hang-look] bounces={bounces} spp={samples} size={size}", flush=True)

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

    rng = tree.add_node("rng_texture")
    ray_gen = tree.add_node("node_render_ray_generation")
    rng_buffer = tree.add_node("rng_buffer")
    path_trace = tree.add_node("path_tracing")
    accumulate = tree.add_node("accumulate")
    present = tree.add_node("present_color")

    tree.add_link(rng.get_output_socket("Random Number"),
                  ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"),
                  path_trace.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"),
                  path_trace.get_input_socket("Rays"))
    tree.add_link(rng_buffer.get_output_socket("Random Number"),
                  path_trace.get_input_socket("Random Seeds"))
    tree.add_link(path_trace.get_output_socket("Output"),
                  accumulate.get_input_socket("Texture"))
    tree.add_link(accumulate.get_output_socket("Accumulated"),
                  present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    for (node, socket_name), value in {
        (path_trace, "Max Bounces"): bounces,
        (accumulate, "Max Samples"): samples,
    }.items():
        try:
            socket = node.get_input_socket(socket_name)
        except RuntimeError:
            print(f"[hang-look] socket '{socket_name}' absent on this build — "
                  "skipping (build uses its default)", flush=True)
            continue
        meta = core.to_meta_any(value)
        executor.sync_node_from_external_storage(socket, meta)

    t0 = time.perf_counter()
    for i in range(samples):
        hydra.render()
    total = time.perf_counter() - t0
    print(f"[hang-look] {samples} renders in {total:.2f}s", flush=True)

    texture_data = hydra.get_output_texture()
    img = np.array(texture_data, dtype=np.float32).reshape(size, size, 4)
    rgb = np.clip(img[:, :, :3], 0.0, None)
    print(f"[hang-look] mean rgb = {rgb.mean():.4f} max = {rgb.max():.3f}",
          flush=True)

    from PIL import Image
    linear = np.clip(np.flipud(rgb), 0.0, 1.0)
    Image.fromarray((linear * 255).astype(np.uint8)).save(
        out_dir / "look_linear.png")
    gamma = np.power(linear, 1.0 / 2.2)
    Image.fromarray((gamma * 255).astype(np.uint8)).save(
        out_dir / "look_gamma.png")
    print(f"[hang-look] saved {out_dir / 'look_linear.png'} and look_gamma.png",
          flush=True)


if __name__ == "__main__":
    main()
