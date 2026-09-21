"""Minimal hang reproducer — bisect which node's cached-context reuse hangs the GPU.

Splits the failing test_rendering.py graph into three sub-graphs, selected by
RZ_HANG_GRAPH env:
  acc    : rng_texture -> accumulate -> present        (no raygen, no PT, no TLAS read)
  raygen : rng_texture -> ray_generation -> present    (no accumulate, no PT)
  full   : rng_texture -> ray_generation -> accumulate -> present  (still no PT!)

PT is excluded in all three: if "full" still hangs, path_tracing/any-hit/TLAS-read
are exonerated and the culprit is in the raygen/acc/rng cached-context reuse
family. If only "acc" hangs, it is accumulate; if only "raygen", raygen.
"""

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


def _locate_config(binary_dir: Path) -> Path:
    primary = binary_dir / "render_nodes.json"
    fallback = (binary_dir.parent.parent / "Assets" /
                "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json")
    if primary.exists():
        return primary
    if fallback.exists():
        return fallback
    raise RuntimeError("No render node configuration found")


def main():
    workspace_root, binary_dir = _prepare_env()
    graph_kind = os.environ.get("RZ_HANG_GRAPH", "full")
    samples = int(os.environ.get("RZ_HANG_SPP", "4"))

    import hd_RUZINO_py as renderer
    import nodes_core_py as core

    scene = workspace_root / "Assets" / "shader_ball.usdc"
    if not scene.exists():
        scene = script_scene = Path(__file__).parent / "test_scene.usda"
    print(f"[hang-min] graph={graph_kind} scene={scene.name} spp={samples}",
          flush=True)

    width = height = 128
    hydra = renderer.HydraRenderer(str(scene), width, height)

    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config(binary_dir)))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture")
    present = tree.add_node("present_color")

    if graph_kind == "acc":
        acc = tree.add_node("accumulate")
        tree.add_link(rng.get_output_socket("Random Number"),
                      acc.get_input_socket("Texture"))
        tree.add_link(acc.get_output_socket("Accumulated"),
                      present.get_input_socket("Color"))
    elif graph_kind == "raygen":
        ray_gen = tree.add_node("node_render_ray_generation")
        tree.add_link(rng.get_output_socket("Random Number"),
                      ray_gen.get_input_socket("random seeds"))
        # present takes a Color texture; raygen outputs buffers, so stop the
        # graph at raygen itself (required node = raygen).
        required = ray_gen
    else:  # full: rng -> raygen -> accumulate -> present (NO path_tracing)
        ray_gen = tree.add_node("node_render_ray_generation")
        acc = tree.add_node("accumulate")
        tree.add_link(rng.get_output_socket("Random Number"),
                      ray_gen.get_input_socket("random seeds"))
        tree.add_link(rng.get_output_socket("Random Number"),
                      acc.get_input_socket("Texture"))
        tree.add_link(acc.get_output_socket("Accumulated"),
                      present.get_input_socket("Color"))

    executor.reset_allocator()
    required = None if graph_kind != "raygen" else required
    executor.prepare_tree(
        tree, required if required is not None else present)

    if graph_kind in ("acc", "full"):
        socket = acc.get_input_socket("Max Samples")
        meta = core.to_meta_any(samples)
        executor.sync_node_from_external_storage(socket, meta)

    for i in range(samples):
        print(f"[hang-min] render() call {i}", flush=True)
        hydra.render()
        print(f"[hang-min] render() call {i} returned", flush=True)

    print("[hang-min] SURVIVED — no hang in this graph", flush=True)


if __name__ == "__main__":
    main()
