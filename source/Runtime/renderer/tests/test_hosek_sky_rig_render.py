#!/usr/bin/env python3
"""Black-box render test for the HosekWilkieSky sky-sun rig.

The stage must be SELF-CONSISTENT — the renderer treats every light as
independent authored data and never rewrites directions:

  * rig scenario: a HosekWilkieSky prim (codeless schema; all sky attributes
    resolve from schema FALLBACKS, nothing authored except sunDirection)
    with a child DistantLight whose direction was synced by
    stage_py.sync_sun_light — exactly what the editor produces. The shadow
    must fall along the SKY's sun.
  * mismatch regression: a plain DomeLight + a DistantLight pointing the
    OTHER way renders as authored — shadow follows the LIGHT, proving no
    renderer-side sun binding survives.

Camera straight down (screen right = world +X). Fallback sunDirection
(0.5, 0.7, 0.5) has +X toward-sun, so the rig shadow falls screen LEFT;
the mismatch light points toward +X so its shadow falls screen RIGHT.

Environment is set up by the renderer tests conftest (cwd = Binaries).
"""
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

import hydra2_ab_common as ab


_RENDER_SCRIPT = r"""
import os, sys

binary_dir, stage_path, rznode_py, out_npy, frames_s, size_s = sys.argv[1:7]
frames, size = int(frames_s), int(size_s)

sys.path.insert(0, binary_dir)
sys.path.insert(0, rznode_py)
os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', binary_dir)
os.environ['PATH'] = binary_dir + os.pathsep + os.environ.get('PATH', '')

import hd_RUZINO_py as renderer

hydra = renderer.HydraRenderer(stage_path, size, size)

node_system = hydra.get_node_system()
config = os.path.join(binary_dir, 'render_nodes.json')
if not os.path.exists(config):
    print('NO_RENDER_NODES_CONFIG')
    sys.exit(0)
node_system.load_configuration(config)
node_system.init()

tree = node_system.get_node_tree()
executor = node_system.get_node_tree_executor()

rng = tree.add_node("rng_texture")
ray_gen = tree.add_node("node_render_ray_generation")
path_trace = tree.add_node("path_tracing")
accumulate = tree.add_node("accumulate")
rng_buffer = tree.add_node("rng_buffer")
present = tree.add_node("present_color")

tree.add_link(rng.get_output_socket("Random Number"), ray_gen.get_input_socket("random seeds"))
tree.add_link(ray_gen.get_output_socket("Pixel Target"), path_trace.get_input_socket("Pixel Target"))
tree.add_link(ray_gen.get_output_socket("Rays"), path_trace.get_input_socket("Rays"))
tree.add_link(rng_buffer.get_output_socket("Random Number"), path_trace.get_input_socket("Random Seeds"))
tree.add_link(path_trace.get_output_socket("Output"), accumulate.get_input_socket("Texture"))
tree.add_link(accumulate.get_output_socket("Accumulated"), present.get_input_socket("Color"))

executor.reset_allocator()
executor.prepare_tree(tree, present)

for _ in range(frames):
    hydra.render()
hydra.stop()

import numpy as np
img = np.array(hydra.get_output_texture(), dtype=np.float32).reshape(size, size, 4)
np.save(out_npy, img)
print('RENDER_DONE')
"""


def _norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def _cross(a, b):
    return [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]


def _camera_matrix(eye, target, up):
    """usda camera xform (row-major rows): right / up / -forward / eye."""
    f = _norm([t - e for t, e in zip(target, eye)])
    r = _norm(_cross(f, up))
    u = _cross(r, f)
    rows = [
        [r[0], r[1], r[2], 0.0],
        [u[0], u[1], u[2], 0.0],
        [-f[0], -f[1], -f[2], 0.0],
        [eye[0], eye[1], eye[2], 1.0],
    ]
    return ", ".join(
        "(" + ", ".join(f"{c:.6f}" for c in row) + ")" for row in rows
    )


def _wrong_sun_dirw(up_axis):
    """dirW (propagation) of the mismatch DistantLight: toward-sun at -X
    azimuth so its shadow falls toward +X (screen right)."""
    if up_axis == "Y":
        return (0.6428, -0.766, 0.0)
    return (0.6428, 0.0, -0.766)


def _ground_and_pole(up_axis):
    """Return (ground_points, ground_normal, pole_points) usda fragments."""
    H = 40.0
    if up_axis == "Y":
        ground = [(-H, 0, -H), (H, 0, -H), (H, 0, H), (-H, 0, H)]
        normal = "(0, 1, 0)"
        lo, hi, y0, y1 = -0.5, 0.5, 0.0, 3.0
        pole = [
            (lo, y0, lo), (hi, y0, lo), (hi, y0, hi), (lo, y0, hi),
            (lo, y1, lo), (hi, y1, lo), (hi, y1, hi), (lo, y1, hi),
        ]
    else:
        ground = [(-H, -H, 0), (H, -H, 0), (H, H, 0), (-H, H, 0)]
        normal = "(0, 0, 1)"
        lo, hi, z0, z1 = -0.5, 0.5, 0.0, 3.0
        pole = [
            (lo, lo, z0), (hi, lo, z0), (hi, hi, z0), (lo, hi, z0),
            (lo, lo, z1), (hi, lo, z1), (hi, hi, z1), (lo, hi, z1),
        ]
    pts = lambda ps: "[" + ", ".join(
        "(" + ", ".join(f"{c:.3f}" for c in p) + ")" for p in ps) + "]"
    return pts(ground), normal, pts(pole)


def _base_scene(up_axis, ground_points, ground_normal, pole_points, extras):
    # Camera straight down (slightly offset for a non-degenerate basis):
    # frame = all ground, screen right = world +X, pole at frame center.
    if up_axis == "Y":
        eye, target, up = (0, 14, 0.5), (0, 0, 0), (0, 0, -1)
    else:
        eye, target, up = (0, 0.5, 14), (0, 0, 0), (0, 1, 0)
    cam = _camera_matrix(eye, target, up)
    return """#usda 1.0
(
    metersPerUnit = 1
    upAxis = "%s"
)

def Camera "Camera"
{
    float2 clippingRange = (0.05, 1e5)
    float focalLength = 35.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( %s )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def Xform "World"
{
    def Mesh "Ground"
    {
        uniform token subdivisionScheme = "none"
        color3f[] primvars:displayColor = [(0.85, 0.85, 0.85)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = %s
        normal3f[] normals = [%s]
    }

    def Mesh "Pole"
    {
        uniform token subdivisionScheme = "none"
        color3f[] primvars:displayColor = [(0.9, 0.9, 0.9)]
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 2, 3, 5, 4, 7, 6, 1, 5, 6, 2, 4, 0, 3, 7, 3, 2, 6, 7, 4, 5, 1, 0]
        point3f[] points = %s
    }
%s
}
""" % (up_axis, cam, ground_points, ground_normal, pole_points, extras)


def _rig_scene(up_axis, ground_points, ground_normal, pole_points):
    # Sky + sun rig authored by the editor convention: only sunDirection is
    # explicit; shader_path/turbidity/albedo come from the schema fallbacks.
    # The child light has NO transform — sync_sun_light authors it next.
    return _base_scene(up_axis, ground_points, ground_normal, pole_points, """
    def HosekWilkieSky "Sky"
    {
        custom float3 inputs:sunDirection = (0.5, 0.7, 0.5)

        def DistantLight "Sun"
        {
            float inputs:intensity = 8
            float inputs:angle = 0.001
        }
    }""")


def _mismatch_scene(up_axis, ground_points, ground_normal, pole_points):
    # Plain (non-rig) dome + a DistantLight pointing the OTHER way: the
    # scene renders exactly as authored — shadow follows the LIGHT.
    dirw = "(%.4f, %.4f, %.4f, 0)" % _wrong_sun_dirw(up_axis)
    return _base_scene(up_axis, ground_points, ground_normal, pole_points, """

    def DistantLight "WrongSun"
    {
        float inputs:intensity = 8
        float inputs:angle = 0.001
        matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), %s, (0,0,0,1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }

    def DomeLight "Sky"
    {
        float inputs:intensity = 1.0
    }
""" % dirw)


def _sync_rig(stage_path):
    """Run the editor-side sync on the rig scene (root layer, like the demo
    generator) and save."""
    import stage_py
    from pxr import Usd

    st = Usd.Stage.Open(str(stage_path))
    stage_py.sync_sun_light(st, "/World/Sky", st.GetRootLayer())
    st.GetRootLayer().Save()


def _render(binary_dir, stage_path, tmp_path, name, size=128, frames=8):
    out_npy = tmp_path / f"{name}.npy"
    rznode_py = binary_dir.parent.parent / "source" / "Core" / "rznode" / "python"
    env = os.environ.copy()
    env.pop("HD_RUZINO_SIM_SCENE_INDEX_DEBUG", None)
    r = subprocess.run(
        [sys.executable, "-c", _RENDER_SCRIPT,
         str(binary_dir), str(stage_path), str(rznode_py),
         str(out_npy), str(frames), str(size)],
        capture_output=True, text=True, timeout=300,
        cwd=str(binary_dir), env=env,
    )
    combined = r.stdout + r.stderr
    assert "RENDER_DONE" in combined, f"{name}: render failed:\n{combined[-2000:]}"
    return np.load(out_npy)


def _shadow_sides(img):
    """Mean brightness left vs right thirds of the shadow band (center rows;
    the shadow is a horizontal streak from the pole at frame center)."""
    h, w = img.shape[:2]
    band = img[int(h * 0.40):int(h * 0.60), :, :3]
    third = w // 3
    left = float(band[:, :third].mean())
    right = float(band[:, -third:].mean())
    return left, right


@pytest.mark.skipif(
    not ab.hydra_py_available(ab.prepare_env()[1]),
    reason="hd_RUZINO_py not built",
)
def test_rig_shadow_follows_sky_sun(tmp_path):
    """Rig scenes: shadow falls along the sky's sun (screen left), for both
    the Y-up identity convention and the Z-up RotX(-90) dome transform."""
    workspace_root, binary_dir = ab.prepare_env()

    for name, up_axis in [("yup", "Y"), ("zup", "Z")]:
        ground, normal, pole = _ground_and_pole(up_axis)
        stage = tmp_path / f"hosek_rig_{name}.usda"
        stage.write_text(
            _rig_scene(up_axis, ground, normal, pole))
        _sync_rig(stage)

        img = _render(binary_dir, stage, tmp_path, f"rig_{name}")
        left, right = _shadow_sides(img)
        assert left > 0.001 and right > 0.001, (
            f"{name}: ground render blank")
        assert left < right, (
            f"{name}: expected rig shadow toward -X / screen left "
            f"(left={left:.4f} < right={right:.4f}) — schema fallback "
            "sunDirection did not reach the renderer or the synced child "
            "direction is wrong")


@pytest.mark.skipif(
    not ab.hydra_py_available(ab.prepare_env()[1]),
    reason="hd_RUZINO_py not built",
)
def test_mismatched_lights_render_as_authored(tmp_path):
    """Regression: a plain dome + reversed DistantLight renders as authored —
    the shadow follows the LIGHT (screen right), proving the renderer no
    longer overrides authored light directions."""
    workspace_root, binary_dir = ab.prepare_env()

    ground, normal, pole = _ground_and_pole("Y")
    stage = tmp_path / "mismatch.usda"
    stage.write_text(_mismatch_scene("Y", ground, normal, pole))

    img = _render(binary_dir, stage, tmp_path, "mismatch")
    left, right = _shadow_sides(img)
    assert left > 0.001 and right > 0.001, "ground render blank"
    assert right < left, (
        f"expected shadow toward +X / screen right per the authored light "
        f"(right={right:.4f} < left={left:.4f}) — renderer still overrides "
        "authored directions")
