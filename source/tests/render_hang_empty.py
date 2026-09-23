#!/usr/bin/env python3
"""Empty-scene GPU-hang reproducer.

Mirrors the interactive app's saved graph EXACTLY (Assets/Hd_RUZINO_RendererPlugin/
render_nodes_save.json):

    rng_texture ──→ ray_generation ──→ path_tracing ──→ present_color
    rng_buffer  ──────────↗ (Random Seeds)

against a COMPLETELY EMPTY stage (no prims, like Assets/stage.usdc), and calls
hydra.render() in a loop. The interactive app TDRs (DXGI_ERROR_DEVICE_HUNG)
after ~1-2 minutes on this exact setup.

A watchdog thread hard-kills the process 25s into a single stuck render() --
before the 60s TdrDelay -- so a hang here prints HANG and exits instead of
blanking the user's screen.

Env knobs:
    RZ_HANG_FRAMES   total render() calls (default 2000)
    RZ_HANG_SCENE    scene path (default: writes/uses an empty .usda)
    RZ_HANG_SIZE     square resolution (default 540)
"""
import os
import sys
import threading
import time
from pathlib import Path


def _prepare_env():
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent
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


def main():
    workspace_root, binary_dir = _prepare_env()

    frames = int(os.environ.get("RZ_HANG_FRAMES", "2000"))
    size = int(os.environ.get("RZ_HANG_SIZE", "540"))
    graph_kind = os.environ.get("RZ_HANG_GRAPH", "full")

    scene = Path(os.environ.get(
        "RZ_HANG_SCENE", str(Path(__file__).parent / "empty_scene.usda")))
    if not scene.exists():
        scene.write_text("#usda 1.0\n")
    print(f"[hang-empty] scene={scene} prims=NONE frames={frames} "
          f"size={size}x{size}", flush=True)

    import hd_RUZINO_py as renderer

    hydra = renderer.HydraRenderer(str(scene), size, size)
    node_system = hydra.get_node_system()
    primary = binary_dir / "render_nodes.json"
    fallback = (workspace_root / "Assets" / "Hd_RUZINO_RendererPlugin" /
                "render_nodes_save.json")
    config = primary if primary.exists() else fallback
    node_system.load_configuration(str(config))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    # Exact app graph (no accumulate node).
    rng = tree.add_node("rng_texture")
    rng_buf = tree.add_node("rng_buffer")
    ray_gen = tree.add_node("node_render_ray_generation")
    path_trace = tree.add_node("path_tracing")
    present = tree.add_node("present_color")

    tree.add_link(rng.get_output_socket("Random Number"),
                  ray_gen.get_input_socket("random seeds"))
    if graph_kind != "rng_only":
        tree.add_link(ray_gen.get_output_socket("Pixel Target"),
                      path_trace.get_input_socket("Pixel Target"))
        tree.add_link(ray_gen.get_output_socket("Rays"),
                      path_trace.get_input_socket("Rays"))
    if graph_kind in ("full", "no_raygen"):
        tree.add_link(rng_buf.get_output_socket("Random Number"),
                      path_trace.get_input_socket("Random Seeds"))
    if graph_kind in ("full", "rng_only"):
        tree.add_link(path_trace.get_output_socket("Output"),
                      present.get_input_socket("Color"))

    executor.reset_allocator()
    required = {
        "full": present,
        "no_pt": ray_gen,       # rng -> raygen only
        "rng_only": rng,        # rng_texture alone
        "no_raygen": path_trace,  # rng_buffer -> PT (no camera rays)
    }[graph_kind]
    executor.prepare_tree(tree, required)
    print(f"[hang-empty] graph={graph_kind} required={graph_kind}", flush=True)

    # Watchdog: kill the process 25s into a single stuck render(), before
    # TdrDelay=60 elapses, so the driver never has to reset.
    watchdog_armed = threading.Event()
    hang_frame = [-1]
    armed_at = [0.0]

    def watchdog():
        while True:
            if watchdog_armed.is_set():
                # Frame 0 legitimately spends ~8s compiling the PT shader
                # program, so only fire once the render has actually
                # exceeded the budget (the old version killed as soon as
                # it observed the flag, hanging any render > 0.5s).
                if armed_at[0] and time.perf_counter() - armed_at[0] > 25.0:
                    frame = hang_frame[0]
                    print(f"[hang-empty] HANG: render() call {frame} did not "
                          f"return within 25s -- killing process before TDR",
                          flush=True)
                    os._exit(3)
            else:
                armed_at[0] = 0.0
            time.sleep(0.5)

    t = threading.Thread(target=watchdog, daemon=True)
    if not os.environ.get("RZ_HANG_NOWATCH"):
        t.start()

    t0 = time.perf_counter()
    worst = 0.0
    for i in range(frames):
        hang_frame[0] = i
        watchdog_armed.set()
        armed_at[0] = time.perf_counter()
        f0 = time.perf_counter()
        hydra.render(0.0)
        dt = time.perf_counter() - f0
        watchdog_armed.clear()
        worst = max(worst, dt)
        if dt > 1.0 or i % 100 == 0:
            print(f"[hang-empty] frame {i} took {dt:.3f}s "
                  f"(total {time.perf_counter()-t0:.1f}s)", flush=True)
        if dt > 25.0:  # watchdog would normally have fired; belt and braces
            print(f"[hang-empty] HANG-DETECTED frame {i}", flush=True)
            os._exit(3)

    print(f"[hang-empty] SURVIVED {frames} frames in "
          f"{time.perf_counter()-t0:.1f}s, worst frame {worst:.3f}s",
          flush=True)
    hydra.stop()


if __name__ == "__main__":
    main()
