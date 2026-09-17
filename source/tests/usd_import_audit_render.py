"""Render one USD fixture headless and emit JSON probes for the import audit.

Runs in a SUBPROCESS (one per fixture) because the HD renderer plugin is a
process-global singleton that does not tolerate repeated HydraRenderer
construction. Bootstraps DLL/paths by importing conftest from the same
directory.

    python usd_import_audit_render.py <stage.usda> <out_dir> [--frames 0,24]
                                      [--size 96] [--spp 6] [--name label]

Writes <out_dir>/<name>.json (probes) and <name>_t<frame>.png/.npy per frame.
Always exits with a written JSON; exit code 1 on render error.
"""
import argparse
import json
import sys
import time
import traceback
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR))
import conftest  # noqa: F401  side-effect bootstrap: paths, DLL dirs, cwd

import numpy as np

PROJECT_ROOT = TESTS_DIR.parent.parent


def _locate_config():
    primary = conftest_binary_dir() / "render_nodes.json"
    if primary.exists():
        return primary
    raise FileNotFoundError("render_nodes.json not found in Binaries")


def conftest_binary_dir():
    return Path(conftest.TEST_OUTPUT_DIR).parent


def build_render_graph(hydra, samples):
    """Path-tracing graph, same chain as test_render_materials._build_render_graph."""
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

    vec_params = {
        (lpm, "Crosstalk"): [0.471, 0.49, 0.504],
    }
    for (node, socket_name), value in vec_params.items():
        node.get_input_socket(socket_name).set_default_value(value)

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    scalar_params = {
        (ray_gen, "Aperture"): 0.0,
        (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
        (gamma, "Gamma"): 2.2,
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


def render_frames(stage, width, height, spp, frames):
    import hd_RUZINO_py as renderer

    hydra = renderer.HydraRenderer(str(stage), width, height)
    build_render_graph(hydra, spp)

    imgs = {}
    try:
        for t in frames:
            hydra.reset_accumulation()
            for _ in range(spp):
                hydra.render(float(t))
            data = hydra.get_output_texture()
            img = np.array(data, dtype=np.float32).reshape(height, width, 4)
            imgs[t] = np.flipud(img)  # GPU origin top-left -> Y-up
    finally:
        try:
            hydra.stop()
        except Exception:
            pass
    return imgs


def save_frame(out_dir, name, t, img):
    np.save(out_dir / f"{name}_t{t:g}.npy", img)
    try:
        from PIL import Image
        rgb = np.clip(img[:, :, :3], 0, 1)
        Image.fromarray((rgb * 255).astype(np.uint8)).save(out_dir / f"{name}_t{t:g}.png")
    except ImportError:
        pass


def probes(img):
    h, w = img.shape[:2]
    if w != 96:
        # The horizontal probe ranges below (left_mid etc.) are tuned for
        # the default 96px renders; rescale them before changing --size.
        raise ValueError(f"probe regions tuned for 96px renders, got {w}px")
    rgb = img[:, :, :3]
    lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]

    def region(r0, r1, c0, c1):
        reg = rgb[r0:r1, c0:c1]
        reg_lum = lum[r0:r1, c0:c1]
        return {
            "mean": [float(x) for x in reg.mean(axis=(0, 1))],
            "lum": float(reg_lum.mean()),
        }

    bright = lum > 0.02
    if bright.any():
        cols = np.broadcast_to(np.arange(w), lum.shape)
        centroid_col = float(cols[bright].mean())
    else:
        centroid_col = None

    return {
        "mean": [float(x) for x in rgb.mean(axis=(0, 1))],
        "finite": bool(np.isfinite(img).all()),
        "coverage": float(bright.mean()),
        "bright_centroid_col": centroid_col,
        "regions": {
            "center": region(h // 4, 3 * h // 4, w // 4, 3 * w // 4),
            "left_mid": region(h // 3, 2 * h // 3, 10, 42),
            "right_mid": region(h // 3, 2 * h // 3, 54, 86),
            "center_col": region(h // 3, 2 * h // 3, 43, 53),
            "right_edge": region(h // 3, 2 * h // 3, 64, 92),
            # Top quarter, center half: bright when a centered object stays
            # SHORT (used by gprim_axes to catch a wrongly-tall default-Z
            # cylinder).
            "top_edge": region(0, h // 4, w // 4, 3 * w // 4),
            "bg_corner": region(0, 12, 0, 12),
        },
    }


def mse(a, b):
    return float(np.mean((a[:, :, :3] - b[:, :, :3]) ** 2))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("stage")
    p.add_argument("out_dir")
    p.add_argument("--frames", default="0")
    p.add_argument("--size", type=int, default=96)
    p.add_argument("--spp", type=int, default=6)
    p.add_argument("--name", default=None)
    args = p.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    name = args.name or Path(args.stage).stem
    result = {"fixture": name, "stage": str(args.stage), "frames": {},
              "mse_pairs": [], "timings_sec": {}}

    try:
        frames = [float(t) for t in args.frames.split(",") if t != ""]
        t0 = time.time()
        imgs = render_frames(args.stage, args.size, args.size, args.spp, frames)
        result["timings_sec"]["render"] = round(time.time() - t0, 2)
        for t, img in imgs.items():
            save_frame(out_dir, name, t, img)
            result["frames"][f"{t:g}"] = probes(img)
        ordered = [imgs[t] for t in frames]
        for (ta, ia), (tb, ib) in zip(zip(frames, ordered), zip(frames[1:], ordered[1:])):
            result["mse_pairs"].append({"frames": [ta, tb], "mse": mse(ia, ib)})
    except Exception:
        result["error"] = traceback.format_exc()
        (out_dir / f"{name}.json").write_text(json.dumps(result, indent=2))
        print(result["error"], file=sys.stderr)
        return 1

    (out_dir / f"{name}.json").write_text(json.dumps(result, indent=2))
    print(f"{name}: rendered frames {frames} in {result['timings_sec']['render']}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
