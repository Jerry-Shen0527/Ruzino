#!/usr/bin/env python3
"""RectLight single-sided visibility: front lit, back see-through, no shadow.

The emissive proxy quad of a RectLight is single-sided per the USD spec
(it emits along local -Z). Three guards, each a 128x128 path-traced render:

* front  — camera on the emitting side sees the quad's Le (bright).
* back   — camera behind the quad sees a red emissive plane THROUGH it;
           before LightBackfaceAnyHit the quad acted as a black opaque
           slab on the back side.
* shadow — a sunlit white plane above the light stays sunlit. Its shadow
           ray is retraced by ShadowHit (mask 0xFF) after passing an
           invisible opacity-0 plane, and must then pass through the light
           quad (LightShadowAnyHit) instead of being blocked by it
           (the quad's materialID == -1 read as opaque).
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


_RECT_LIGHT = '''
def RectLight "KeyLight"
{
    float inputs:height = 2
    float inputs:width = 2
    color3f inputs:color = (1, 1, 1)
    float inputs:intensity = 5
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 0, 0, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
'''

_RED_EMISSIVE_PLANE = '''
def Material "RedMat"
{
    token outputs:surface.connect = </RedMat/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0, 0, 0)
        color3f inputs:emissiveColor = (1, 0, 0)
        float inputs:roughness = 0.5
        token outputs:surface
    }
}

def Xform "World"
{
    def Mesh "RedPlane" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)]
        point3f[] points = [(-1, -1, -2), (1, -1, -2), (1, 1, -2), (-1, 1, -2)]
        rel material:binding = </RedMat>
    }
}
'''

_SUN_LIT_STACK = '''
def Material "WhiteMat"
{
    token outputs:surface.connect = </WhiteMat/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.8, 0.8)
        float inputs:metallic = 0
        float inputs:roughness = 0.9
        token outputs:surface
    }
}

def Material "InvisibleMat"
{
    token outputs:surface.connect = </InvisibleMat/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0, 0, 0)
        float inputs:opacity = 0.0
        token outputs:surface
    }
}

def DistantLight "Sun"
{
    color3f inputs:color = (1, 1, 1)
    float inputs:intensity = 4
    matrix4d xformOp:transform = ( (1,0,0,0), (0,-1,0,0), (0,0,-1,0), (0, 0, 0, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def Xform "World"
{
    def Mesh "WhitePlane" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        normal3f[] normals = [(0, 0, -1), (0, 0, -1), (0, 0, -1), (0, 0, -1)]
        point3f[] points = [(-0.7, -0.7, 3), (0.7, -0.7, 3), (0.7, 0.7, 3), (-0.7, 0.7, 3)]
        rel material:binding = </WhiteMat>
    }

    def Mesh "InvisiblePlane" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        normal3f[] normals = [(0, 0, -1), (0, 0, -1), (0, 0, -1), (0, 0, -1)]
        point3f[] points = [(-2, -2, 1.5), (2, -2, 1.5), (2, 2, 1.5), (-2, 2, 1.5)]
        rel material:binding = </InvisibleMat>
    }
}
'''

# 180-degree-about-Y camera: default USD cameras look -Z; this one looks +Z.
_CAM_LOOK_PLUS_Z = '''
def Camera "Camera"
{
    float2 clippingRange = (0.1, 100.0)
    float focalLength = 50.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( (-1,0,0,0), (0,1,0,0), (0,0,-1,0), (0, 0, -6, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
'''

_CAM_DEFAULT = '''
def Camera "Camera"
{
    float2 clippingRange = (0.1, 100.0)
    float focalLength = 50.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 0, CAM_Z, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
'''


def _cam_default(z):
    # .format() would eat the usda braces; plain substitution instead.
    return _CAM_DEFAULT.replace("CAM_Z", str(z))


def _scene(camera: str, *blocks: str) -> str:
    return f'''#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

{camera}
{"".join(blocks)}
'''


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


SIZE = 128
SPP = 64


def _render(hydra, samples=SPP):
    hydra.reset_accumulation()
    for _ in range(samples):
        hydra.render()
    data = hydra.get_output_texture()
    img = np.array(data, dtype=np.float32).reshape(SIZE, SIZE, 4)
    return img[:, :, :3]


def _center_mean(img, half=16):
    c = SIZE // 2
    return img[c - half:c + half, c - half:c + half].reshape(-1, 3).mean(axis=0)


@pytest.fixture(scope="module")
def renderer_mod():
    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"required module not available: {e}")
    yield renderer


def _open(scene_path, renderer_mod, binary_dir):
    hydra = renderer_mod.HydraRenderer(str(scene_path), SIZE, SIZE)
    _build_render_graph(hydra, binary_dir, SPP)
    return hydra


def test_front_face_is_emissive(tmp_path, renderer_mod):
    workspace_root, binary_dir = _prepare_env()
    scene = tmp_path / "front.usda"
    scene.write_text(_scene(_CAM_LOOK_PLUS_Z, _RECT_LIGHT))
    hydra = _open(scene, renderer_mod, binary_dir)
    try:
        center = _center_mean(_render(hydra))
    finally:
        hydra.stop()
    # Emitting face fills the view: bright white, far above MC noise.
    assert center.mean() > 0.5, \
        f"front face should be emissive, center mean RGB = {center}"


def test_back_face_is_see_through(tmp_path, renderer_mod):
    workspace_root, binary_dir = _prepare_env()
    scene = tmp_path / "back.usda"
    scene.write_text(_scene(_cam_default(6), _RECT_LIGHT,
                            _RED_EMISSIVE_PLANE))
    hydra = _open(scene, renderer_mod, binary_dir)
    try:
        center = _center_mean(_render(hydra))
    finally:
        hydra.stop()
    # Looking at the quad's back: the red plane behind it must be visible.
    # Before the fix the quad absorbed the ray (black slab).
    assert center[0] > 0.2, \
        f"red plane behind the light must be visible, center RGB = {center}"
    assert center[0] > 3.0 * center[1] and center[0] > 3.0 * center[2], \
        f"see-through must show RED, center RGB = {center}"


def test_light_does_not_block_sunlight(tmp_path, renderer_mod):
    workspace_root, binary_dir = _prepare_env()
    scene = tmp_path / "shadow.usda"
    scene.write_text(_scene(_cam_default(8), _RECT_LIGHT,
                            _SUN_LIT_STACK))
    hydra = _open(scene, renderer_mod, binary_dir)
    try:
        center = _center_mean(_render(hydra))
    finally:
        hydra.stop()
    # The shadow ray retraces past the opacity-0 plane with mask 0xFF and
    # would previously stop on the light quad (materialID -1 == opaque).
    assert center.mean() > 0.3, \
        f"sunlight must pass through the light quad, center RGB = {center}"
