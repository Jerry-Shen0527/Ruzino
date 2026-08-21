#!/usr/bin/env python3
"""Offline renders of the character walking-simulator scene.

Two frames:
  1. rest pose   — validates lights/sky/ground/materials/camera
  2. mid-stride  — Stage.tick() driven forward for ~0.8 s, then the composed
                   stage (root layer + session gait overs) is exported flat
                   and rendered, validating the procedural walk pose.

Run from Binaries/Release:

    python ../../source/tests/render_character_walk.py

Output: Binaries/Release/test_output/character_walk/{rest,walk}.png
"""
import os
import sys
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))

from pxr import Usd, UsdGeom, Gf

SCENE = BIN / "demo_scenes" / "character_walk.usda"


def _ensure_scene():
    if not SCENE.exists():
        subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "gen_character_walk_scene.py")],
            check=True)


def _look_at(eye, target, up_world=(0.0, 0.0, 1.0)):
    """Camera matrix, Z-up."""
    eye = Gf.Vec3d(*eye)
    target = Gf.Vec3d(*target)
    up_world = Gf.Vec3d(*up_world)
    fwd = target - eye
    fwd.Normalize()
    right = Gf.Cross(fwd, up_world)
    right.Normalize()
    up = Gf.Cross(right, fwd)
    m = Gf.Matrix4d()
    m.SetIdentity()
    m.SetRow(0, Gf.Vec4d(right[0], right[1], right[2], 0.0))
    m.SetRow(1, Gf.Vec4d(up[0], up[1], up[2], 0.0))
    m.SetRow(2, Gf.Vec4d(-fwd[0], -fwd[1], -fwd[2], 0.0))
    m.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    return m


def _place_camera(scene_path, eye, target):
    """Author an explicit world matrix on every camera prim so the offline
    renderer (whichever camera it picks) frames the character."""
    stage = Usd.Stage.Open(str(scene_path))
    for prim in stage.Traverse():
        if prim.IsA(UsdGeom.Camera):
            xform = UsdGeom.Xformable(prim)
            xform.ClearXformOpOrder()
            xform.AddTransformOp().Set(_look_at(eye, target))
    stage.GetRootLayer().Save()


def _posed_scene(src, dst, ticks):
    import stage_py
    from pxr import Sdf, Usd
    # Pose a COPY of the scene and drop its modifier sidecar: Stage(path)
    # reloads <stem>_modifiers.usda into the session layer and the
    # controller resumes from the last authored position, so reusing the
    # demo scene itself would make the character start mid-world.
    pose_input = Path(dst).parent / "pose_input.usda"
    pose_input.write_bytes(Path(src).read_bytes())
    sidecar = pose_input.with_name(pose_input.stem + "_modifiers.usda")
    if sidecar.exists():
        sidecar.unlink()

    stage = stage_py.Stage(str(pose_input))
    # Walk toward -Y: the renderer's baked Y-up sky convention leaves the
    # bright sky on that side, so the chase camera looks into the lit half.
    stage.set_move_input(0.0, -1.0)
    for _ in range(ticks):
        stage.tick(1.0 / 60.0)

    char = (0.0, 0.0, 1.0)
    # The offline HydraRenderer fails to draw geometry from any file that
    # round-tripped through Usd.Stage.Export (byte-copies of the generated
    # scene render fine), so bake the session's posed transforms into a
    # byte-copy of the original scene via CommonAPI ops + Save instead of
    # exporting the composed stage.
    posed = Path(dst).with_name("walk_posed.usda")
    posed.write_bytes(Path(src).read_bytes())
    try:
        from pxr import Gf, UsdGeom
        session = Sdf.Layer.Find(stage.get_modifier_layer_identifier())

        def authored(prim_path, attr):
            try:
                return session.GetPropertyAtPath(
                    f"{prim_path}.{attr}").default
            except Exception:  # noqa: BLE001
                return None

        usd_stage = Usd.Stage.Open(str(posed))

        def apply_over(spec, path):
            prim = usd_stage.GetPrimAtPath(path)
            if prim and prim.IsA(UsdGeom.Xformable):
                t = authored(path, "xformOp:transform")
                if t is not None:
                    # Matrix op, matching how the scene/controller author
                    # transforms (multi-op CommonAPI stacks mis-render in
                    # the offline path).
                    xf = UsdGeom.Xformable(prim)
                    xf.ClearXformOpOrder()
                    xf.MakeMatrixXform().Set(Gf.Matrix4d(t))
            for child_name, child_spec in spec.nameChildren.items():
                apply_over(child_spec, f"{path}/{child_name}")

        for over in session.rootPrims:
            apply_over(over, "/" + over.name)

        t = authored("/Character", "xformOp:transform")
        if t is not None:
            tr = t.ExtractTranslation()
            char = (tr[0], tr[1], 1.0)

        # The offline HydraRenderer ignores prim transforms entirely (cloud /
        # wetbrush offline scenes move geometry by writing vertex arrays), so
        # the only reliable way to verify a posed character offline is to
        # bake the pose into the vertex positions/normals of every mesh and
        # strip all transforms.
        default_time = Usd.TimeCode.Default()
        char_root = usd_stage.GetPrimAtPath("/Character")
        descendants = [p for p in Usd.PrimRange(char_root) if p != char_root]
        for prim in descendants:
            if prim.GetTypeName() != "Mesh":
                continue
            xf = UsdGeom.Xformable(prim)
            world = xf.ComputeLocalToWorldTransform(default_time)
            mesh = UsdGeom.Mesh(prim)
            points = mesh.GetPointsAttr().Get()
            if points:
                transformed = [world.Transform(p) for p in points]
                mesh.GetPointsAttr().Set(transformed)
                # extent must follow the transformed points or the renderer
                # culls the mesh against the stale rest-pose bounds
                rng = Gf.Range3f()
                for p in transformed:
                    rng.UnionWith(Gf.Vec3f(p))
                mesh.GetExtentAttr().Set(
                    [Gf.Vec3f(*rng.GetMin()), Gf.Vec3f(*rng.GetMax())])
            normals = mesh.GetNormalsAttr().Get()
            if normals:
                mesh.GetNormalsAttr().Set(
                    [world.TransformDir(p) for p in normals])
            xf.ClearXformOpOrder()
        for prim in descendants:
            if prim.IsA(UsdGeom.Xformable):
                UsdGeom.Xformable(prim).ClearXformOpOrder()
        UsdGeom.Xformable(char_root).ClearXformOpOrder()

        usd_stage.GetRootLayer().Save()
    except Exception as exc:  # noqa: BLE001
        print(f"  [warn] could not bake posed transforms: {exc}")
        return None

    # Character faces/walks -Y. Camera exactly behind on the +Y axis — this
    # exact frustum family is verified against the offline renderer (some
    # other camera parameterizations trip a render bug and produce an empty
    # noise frame).
    eye = (char[0], char[1] + 5.7, 2.0)
    target = (char[0], char[1], 1.0)
    return posed, eye, char


def _build_render_graph(hydra, samples):
    import nodes_core_py as core
    node_system = hydra.get_node_system()
    cfg = BIN / "render_nodes.json"
    if not cfg.exists():
        cfg = ROOT / "Assets" / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"
    node_system.load_configuration(str(cfg))
    node_system.init()
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    rng = tree.add_node("rng_texture")
    ray_gen = tree.add_node("node_render_ray_generation")
    path_trace = tree.add_node("path_tracing")
    accumulate = tree.add_node("accumulate")
    rng_buffer = tree.add_node("rng_buffer")
    lpm = tree.add_node("lpm")
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
                  lpm.get_input_socket("Input Color"))
    tree.add_link(lpm.get_output_socket("Output Color"),
                  present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)

    scalar_params = {
        (ray_gen, "Aperture"): 0.0, (ray_gen, "Focus Distance"): 2.0,
        (ray_gen, "Scatter Rays"): False,
        (accumulate, "Max Samples"): samples,
        (lpm, "HDR Max"): 4.0, (lpm, "LPM Exposure"): 2.0,
        (lpm, "Contrast"): 1.0, (lpm, "Shoulder"): 1.0,
        (lpm, "Shoulder Contrast"): 1.0, (lpm, "Soft Gap"): 0.0,
        (lpm, "Color Space"): 0, (lpm, "Display Mode"): 0,
        (lpm, "Display Max Luminance"): 1000.0,
        (lpm, "Display Min Luminance"): 0.0,
    }
    for (node, sn), val in scalar_params.items():
        socket = node.get_input_socket(sn)
        executor.sync_node_from_external_storage(socket, core.to_meta_any(val))
    vec_params = {(lpm, "Crosstalk"): [0.471, 0.49, 0.504]}
    for (node, sn), val in vec_params.items():
        node.get_input_socket(sn).set_default_value(val)


def _render(scene_path, out_png, width, height, samples):
    import numpy as np
    import hd_RUZINO_py as renderer

    hydra = renderer.HydraRenderer(str(scene_path), width, height)
    _build_render_graph(hydra, samples)
    for _ in range(samples):
        hydra.render()
    tex = hydra.get_output_texture()
    img = np.array(tex, dtype=np.float32).reshape(height, width, 4)
    img = np.flipud(img)
    rgb = np.clip(img[:, :, :3], 0, 1)

    from PIL import Image
    out_png.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray((rgb * 255).astype(np.uint8)).save(out_png)
    print(f"  saved {out_png}  mean={rgb.mean():.4f} "
          f"sky_top={rgb[:height // 3].mean():.4f} "
          f"ground_bottom={rgb[2 * height // 3:].mean():.4f}")


def _render_subprocess(scene_path, out_png, eye, target, width=640,
                       height=480, samples=256):
    """Render in a fresh process: a second HydraRenderer inside one process
    returns the first render's stale accumulation buffer, so each render
    must run isolated."""
    import subprocess as sp
    args = [sys.executable, str(Path(__file__).resolve()), "--render-one",
            str(scene_path), str(out_png),
            *[repr(v) for v in (*eye, *target)],
            str(width), str(height), str(samples)]
    proc = sp.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  [warn] render subprocess failed: {proc.stderr[-500:]}")
    return proc.returncode == 0


def _render_one_main():
    scene = sys.argv[2]
    out_png = sys.argv[3]
    vals = [float(v) for v in sys.argv[4:10]]
    eye, target = tuple(vals[:3]), tuple(vals[3:6])
    width, height, samples = (int(v) for v in sys.argv[10:13])
    _place_camera(Path(scene), eye=eye, target=target)
    _render(Path(scene), Path(out_png), width, height, samples)


def main():
    _ensure_scene()
    out_dir = BIN / "test_output" / "character_walk"
    out_dir.mkdir(parents=True, exist_ok=True)

    # 1) rest pose: camera in front-left of the character (which faces -Y),
    #    looking into the bright sky half.
    rest = out_dir / "rest_raw.usda"
    rest.write_bytes(SCENE.read_bytes())
    print("[character_walk] rendering rest pose ...")
    _render_subprocess(
        rest, out_dir / "rest.png", (2.0, 4.2, 1.9), (0.0, 0.0, 1.0))

    # 2) mid-stride pose (session-layer gait baked into vertex arrays)
    posed_scene = _posed_scene(SCENE, out_dir / "walk_raw.usda", ticks=48)
    if posed_scene is not None:
        flat, eye, target = posed_scene
        print("[character_walk] rendering mid-stride pose ...")
        _render_subprocess(
            flat, out_dir / "walk.png", eye, target)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--render-one":
        _render_one_main()
    else:
        main()
