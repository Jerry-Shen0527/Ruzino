#!/usr/bin/env python3
"""Scene mutation consistency: incremental edits must equal a fresh build.

This is the acceptance guard for the DeviceMemoryPool -> SceneStore rework.
The renderer consumes light_pool / draw_indirect_pool DENSELY (shaders loop
[0..count); today every render node must call light_pool.compress() first,
otherwise a deleted light leaves a hole with stale GPU data inside the dense
range and keeps lighting the scene). Whatever data structure backs those
arrays, this invariant must hold:

    render(scene built incrementally: create -> render -> delete prims
           -> create more prims -> render)
        ==
    render(the same final scene built from scratch)

Path A edits the live stage through the in-process USD stage cache (verified:
pxr.Usd.Stage.Open on the same path returns the stage HydraRenderer opened).
Path B opens a hand-authored .usda of the final state in a fresh renderer.

Tolerances absorb path-tracing Monte-Carlo noise only (32 spp, flat lambert
plane, distant lights): mean-channel deltas of a few percent. A stale-hole
failure (deleted lights still shining / missing lights) shifts means by tens
of percent, far outside tolerance.
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
    os.environ.setdefault("PXR_USD_WINDOWS_DLL_PATH", str(binary_dir))
    os.environ["PATH"] = str(binary_dir) + os.pathsep + os.environ.get("PATH", "")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(binary_dir))
    if str(binary_dir) not in sys.path:
        sys.path.insert(0, str(binary_dir))
    return workspace_root, binary_dir


_LIGHT_TEMPLATE = '''
def DistantLight "{name}"
{{
    color3f inputs:color = {color}
    float inputs:intensity = {intensity}
    matrix4d xformOp:transform = ( (1,0,0,0), (0,-1,0,0), (0,0,-1,0), (0,0,0,1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}}
'''


def _scene_text(lights):
    """lights: list of (name, color, intensity). Camera looks at a vertical
    lambert plane; every distant light contributes to the visible result."""
    text = '''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Camera "Camera"
{
    float2 clippingRange = (0.1, 100.0)
    float focalLength = 50.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 0, 5, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def Material "PlaneMat"
{
    token outputs:surface.connect = </PlaneMat/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.8, 0.8)
        float inputs:metallic = 0
        float inputs:roughness = 0.9
        token outputs:surface
    }
}

def Xform "World"
{
    def Mesh "Plane" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)]
        point3f[] points = [(-3, -3, 0), (3, -3, 0), (3, 3, 0), (-3, 3, 0)]
        rel material:binding = </PlaneMat>
    }
}
'''
    for name, color, intensity in lights:
        text += _LIGHT_TEMPLATE.format(
            name=name, color=color, intensity=intensity)
    return text


def _build_render_graph(hydra, binary_dir: Path, samples: int):
    import nodes_core_py as core
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(binary_dir / "render_nodes.json"))
    node_system.init()
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()
    rng = tree.add_node("rng_texture"); rng.ui_name = "RNG"
    ray_gen = tree.add_node("node_render_ray_generation"); ray_gen.ui_name = "RayGen"
    path_trace = tree.add_node("path_tracing"); path_trace.ui_name = "PT"
    accumulate = tree.add_node("accumulate"); accumulate.ui_name = "Acc"
    rng_buffer = tree.add_node("rng_buffer"); rng_buffer.ui_name = "RNGBuf"
    present = tree.add_node("present_color"); present.ui_name = "Present"
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


def _render(hydra, samples, size=128):
    hydra.reset_accumulation()
    for _ in range(samples):
        hydra.render()
    data = hydra.get_output_texture()
    img = np.array(data, dtype=np.float32).reshape(size, size, 4)
    return img[:, :, :3]


# NOTE ON SLOT-ORDER RANDOMNESS: Hydra SyncAll syncs sprims on parallel tbb
# workers, so the order in which lights allocate pool slots is
# NON-DETERMINISTIC. A hole only corrupts the dense read when a deleted
# light's slot happens to fall inside [0..count). With 5 lights / delete 2,
# the bad layout comes up only ~50% of runs (verified empirically 2026-09-20:
# delete-only caught the hole, delete+add missed it on the same binary). We
# therefore use 12 lights / delete 8 / add 1: P(all 5 dense slots land on
# live lights) ~= (4/12)(3/11)(2/10)(1/9) < 0.25%, so the test catches the
# corruption with >99.8% probability per run. Keep N large if you edit this.
def _lights(n):
    palette = [
        (1.0, 0.1, 0.1), (0.1, 1.0, 0.1), (0.1, 0.1, 1.0), (1.0, 1.0, 0.1),
        (0.1, 1.0, 1.0), (1.0, 0.1, 1.0), (0.6, 0.3, 0.1), (0.1, 0.6, 0.3),
        (0.3, 0.1, 0.6), (0.9, 0.5, 0.5), (0.5, 0.9, 0.5), (0.5, 0.5, 0.9),
    ]
    return [(f"L{i+1:02d}", f"({palette[i][0]}, {palette[i][1]}, {palette[i][2]})", 2.0)
            for i in range(n)]


INITIAL_LIGHTS = _lights(12)
DELETED = [f"L{i:02d}" for i in (2, 3, 5, 6, 8, 9, 11, 12)]  # 8 of 12
ADDED = ("LNEW", "(1.0, 0.25, 0.25)", 2.0)
FINAL_LIGHTS = [l for l in INITIAL_LIGHTS if l[0] not in DELETED] + [ADDED]

SIZE = 128
SPP = 64
# Tolerance calibration (measured 2026-09-20): two renders of the SAME
# scene differ in channel means by up to ~1.6% — Monte-Carlo noise at
# 64 spp PLUS the light iteration order (tbb-parallel sync assigns slots
# in random order, which shifts the RNG stream). A dense-read hole
# failure (ghost lights / lost lights) shifts means by tens of percent,
# so 0.05 still separates signal from noise by ~8x.
TOL_MEAN = 0.05


def test_incremental_mutation_equals_fresh_build(tmp_path):
    workspace_root, binary_dir = _prepare_env()
    try:
        import hd_RUZINO_py as renderer  # type: ignore
        import pxr.Usd as Usd  # type: ignore
        import pxr.UsdGeom as UsdGeom  # type: ignore
        from pxr import Gf, Sdf  # type: ignore
    except ImportError as e:
        pytest.skip(f"required module not available: {e}")

    # --- Path A: incremental ---
    scene_a = tmp_path / "scene_a.usda"
    scene_a.write_text(_scene_text(INITIAL_LIGHTS))
    hydra = renderer.HydraRenderer(str(scene_a), SIZE, SIZE)
    _build_render_graph(hydra, binary_dir, SPP)

    img_initial = _render(hydra, SPP)
    # sanity: initial 5-light render is bright and non-degenerate
    assert img_initial.mean() > 0.05, \
        f"initial render too dark (mean={img_initial.mean():.4f})"

    # Mutate the LIVE stage via the shared USD stage cache
    stage = Usd.Stage.Open(str(scene_a))
    for name in DELETED:
        prim = stage.GetPrimAtPath(f"/{name}")
        assert prim.IsValid(), f"light /{name} not found on stage"
        assert stage.RemovePrim(prim.GetPath())

    prim = stage.DefinePrim("/" + ADDED[0], "DistantLight")
    color = tuple(float(c) for c in ADDED[1].strip("()").split(","))
    prim.CreateAttribute("inputs:color", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(*color))
    prim.CreateAttribute("inputs:intensity", Sdf.ValueTypeNames.Float).Set(
        ADDED[2])
    xf = UsdGeom.Xformable(prim)
    xf.AddTransformOp().Set(Gf.Matrix4d(
        1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1))

    img_incremental = _render(hydra, SPP)
    hydra.stop()

    # --- Path B: fresh build of the final state ---
    scene_b = tmp_path / "scene_b.usda"
    scene_b.write_text(_scene_text(FINAL_LIGHTS))
    hydra_b = renderer.HydraRenderer(str(scene_b), SIZE, SIZE)
    _build_render_graph(hydra_b, binary_dir, SPP)
    img_fresh = _render(hydra_b, SPP)
    hydra_b.stop()

    mean_inc = img_incremental.mean(axis=(0, 1))
    mean_fresh = img_fresh.mean(axis=(0, 1))
    mean_initial = img_initial.mean(axis=(0, 1))
    print(f"initial    mean RGB = {mean_initial}")
    print(f"incremental mean RGB = {mean_inc}")
    print(f"fresh      mean RGB = {mean_fresh}")

    for ch, name in enumerate("RGB"):
        assert abs(mean_inc[ch] - mean_fresh[ch]) < TOL_MEAN, (
            f"{name} channel diverges between incremental and fresh build: "
            f"{mean_inc[ch]:.4f} vs {mean_fresh[ch]:.4f} "
            f"(tolerance {TOL_MEAN}) — dense-read corruption after prim "
            f"removal (stale hole in light pool?)")

    # Deleted lights must actually be gone: 8 of 12 lights removed must
    # change the image far beyond noise.
    assert abs(mean_initial.mean() - mean_inc.mean()) > 0.1, (
        "deleting 8 of 12 lights changed nothing in the image — mutation "
        "not visible")
