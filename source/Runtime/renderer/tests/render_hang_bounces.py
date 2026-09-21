"""Bounce-count bisect for the 5s PT dispatch: same graph as test_render_basic
but with Max Bounces settable via RZ_HANG_BOUNCES (default 2)."""

import os
import sys
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
    samples = int(os.environ.get("RZ_HANG_SPP", "4"))

    import hd_RUZINO_py as renderer
    import nodes_core_py as core

    scene = Path(__file__).parent / "test_scene.usda"
    print(f"[hang-b] bounces={bounces} spp={samples}", flush=True)

    width = height = 128
    hydra = renderer.HydraRenderer(str(scene), width, height)

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
    path_trace = tree.add_node("path_tracing")
    accumulate = tree.add_node("accumulate")
    present = tree.add_node("present_color")

    tree.add_link(rng.get_output_socket("Random Number"),
                  ray_gen.get_input_socket("random seeds"))
    tree.add_link(ray_gen.get_output_socket("Pixel Target"),
                  path_trace.get_input_socket("Pixel Target"))
    tree.add_link(ray_gen.get_output_socket("Rays"),
                  path_trace.get_input_socket("Rays"))
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
        socket = node.get_input_socket(socket_name)
        meta = core.to_meta_any(value)
        executor.sync_node_from_external_storage(socket, meta)

    import time
    for i in range(samples):
        t0 = time.perf_counter()
        hydra.render()
        print(f"[hang-b] render() {i} returned in {time.perf_counter()-t0:.2f}s",
              flush=True)

    print("[hang-b] SURVIVED", flush=True)


if __name__ == "__main__":
    main()
