"""Path-tracing TDR bisect harness — headless, self-exiting.

Renders the FULL saved render graph (rng -> raygen -> path_tracing ->
accumulate -> present) via HydraRenderer in-process, prints a heartbeat per
render() call, and exits by itself. Used to bisect which part of
path_tracing.slang's RayGen hangs/crashes the GPU, by moving an early
`return;` through the function (edited between runs, not in here).

Env:
  RZ_PT_SCENE   scene to render (default: repo Assets/stage.usdc, fallback
                test_scene.usda next to this script)
  RZ_PT_FRAMES  render() iterations (default 4)
  RZ_PT_SIZE    render resolution (default 128)
Exit: 0 = survived all frames; prints SURVIVED marker.
"""

import os
import sys
from pathlib import Path


def _prepare_env():
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
    build_type = os.environ.get("RZ_BUILD_TYPE", "Debug")
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
    frames = int(os.environ.get("RZ_PT_FRAMES", "4"))
    size = int(os.environ.get("RZ_PT_SIZE", "128"))

    scene = Path(os.environ.get(
        "RZ_PT_SCENE", str(workspace_root / "Assets" / "stage.usdc")))
    if not scene.exists():
        scene = Path(__file__).parent / "test_scene.usda"
    print(f"[pt-bisect] scene={scene.name} frames={frames} size={size}",
          flush=True)

    import hd_RUZINO_py as renderer
    import nodes_core_py as core

    hydra = renderer.HydraRenderer(str(scene), size, size)
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config(binary_dir)))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture")
    rng_buffer = tree.add_node("rng_buffer")
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
    tree.add_link(rng_buffer.get_output_socket("Random Number"),
                  path_trace.get_input_socket("Random Seeds"))
    tree.add_link(path_trace.get_output_socket("Output"),
                  accumulate.get_input_socket("Texture"))
    tree.add_link(accumulate.get_output_socket("Accumulated"),
                  present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    socket = accumulate.get_input_socket("Max Samples")
    executor.sync_node_from_external_storage(socket, core.to_meta_any(frames))

    for i in range(frames):
        print(f"[pt-bisect] render() call {i} begin", flush=True)
        hydra.render()
        print(f"[pt-bisect] render() call {i} returned", flush=True)
        if i == 0 and os.environ.get("RZ_PT_DUMP"):
            try:
                data = hydra.get_output_texture()
                import numpy as np
                arr = np.array(data, dtype=np.float32).reshape(size, size, 4)
                nz = int((arr[:, :, :3] != 0).sum())
                print(f"[pt-bisect] output after frame 0: nonzero_px={nz} "
                      f"chan0_max={arr[:,:,0].max():.3f} "
                      f"chan1_max={arr[:,:,1].max():.3f} "
                      f"chan2_max={arr[:,:,2].max():.3f}", flush=True)
                np.save(os.environ["RZ_PT_DUMP"], arr)
                print(f"[pt-bisect] dumped to {os.environ['RZ_PT_DUMP']}",
                      flush=True)
            except Exception as e:
                print(f"[pt-bisect] dump failed: {e}", flush=True)

    print("[pt-bisect] SURVIVED — all frames rendered", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
