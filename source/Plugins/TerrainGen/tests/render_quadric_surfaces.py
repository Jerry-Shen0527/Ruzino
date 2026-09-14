#!/usr/bin/env python3
"""
Quadric-surface mesh-conversion test: feeds an ANALYTIC surface through the
terrain pipeline as-is (adaptive mesh -> texture bake) and renders it, so
mesher artifacts (faceting, seam cracks, asymmetric refinement) are visible
against exact ground truth instead of noise terrain.

    python source/Plugins/TerrainGen/tests/render_quadric_surfaces.py [--surface paraboloid|saddle|both]

Injection needs no C++ change: the terrain family carries heightfields as
plain square grid meshes (terrain_carry.hpp), and the stock read_usd node
passes UsdGeomMesh points/topology through verbatim. Erosion is skipped on
purpose -- it would deform the analytic surface the mesher is being judged
on (wear/wetness channels therefore stay 0 and the bake's sediment/wet
biomes are absent by design).

Environment knobs:
    QUADRIC_RES    grid resolution, (RES-1) % 16 == 0 tiling contract (241)
    QUADRIC_TEXRES bake texture resolution (1024)
    QUADRIC_W/H    render size (1920 x 1080)
    QUADRIC_SPP    accumulation samples (96)

Outputs in Binaries/Release/test_output/:
    quadric_<surface>_source.usdc     analytic input grid (read_usd eats this)
    quadric_<surface>_test.usdc       adaptive mesh written by write_usd
    quadric_<surface>_albedo.png      baked by terrain_texture_bake
    quadric_<surface>_normal.png
    quadric_<surface>_full.png        full 3/4 render
    quadric_<surface>_closeup.png     close-up render
"""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))
os.chdir(str(BIN))

import numpy as np  # noqa: E402
from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt  # noqa: E402

RES = int(os.environ.get("QUADRIC_RES", "241"))
assert (RES - 1) % 16 == 0, "adaptive tiling contract: (RES-1) % 16 == 0"
SIZE = 100.0  # world extent, matches terrain_heightfield defaults
HEIGHT = 40.0
TEX_RES = int(os.environ.get("QUADRIC_TEXRES", "1024"))
WIDTH = int(os.environ.get("QUADRIC_W", "1920"))
HEIGHT_PX = int(os.environ.get("QUADRIC_H", "1080"))
SPP = int(os.environ.get("QUADRIC_SPP", "96"))
SCALE = 0.1  # render at 1/10 size: keeps the camera inside the ray-tMax
             # range that burned the treegen/terrain renders

OUT_DIR = BIN / "test_output"


def make_heights(kind):
    """Field-order (row-major y,x) analytic heights, peak-up for the
    paraboloid (a bowl grows snow on the rim, which reads wrong)."""
    axis = np.linspace(-1.0, 1.0, RES, dtype=np.float64)
    uu, vv = np.meshgrid(axis, axis)  # [y, x]
    if kind == "paraboloid":
        return (HEIGHT * (1.0 - 0.5 * (uu * uu + vv * vv))).astype(np.float32)
    if kind == "saddle":
        # Half amplitude: at full HEIGHT the high corners reach ~58 deg
        # slope and the bake's snow gate (fades 30..55 deg) kills every
        # snow pixel; half amplitude keeps them inside the gate.
        return (HEIGHT * 0.5 * (uu * uu - vv * vv)).astype(np.float32)
    raise ValueError(kind)


def analytic_height(kind, x, z):
    u = 2.0 * x / SIZE
    v = 2.0 * z / SIZE
    if kind == "paraboloid":
        return HEIGHT * (1.0 - 0.5 * (u * u + v * v))
    return HEIGHT * 0.5 * (u * u - v * v)


def write_source_usd(kind, path):
    """Square grid mesh with the EXACT vertex order and triangulation of
    terrain_carry::mesh_from_heightfield, so heightfield_from_mesh
    accepts it (res^2 verts, 2*(res-1)^2 tris)."""
    h = make_heights(kind)
    axis = np.linspace(-SIZE * 0.5, SIZE * 0.5, RES, dtype=np.float32)
    xx, zz = np.meshgrid(axis, axis)
    pts = np.empty((RES * RES, 3), np.float32)
    pts[:, 0] = xx.ravel()
    pts[:, 1] = h.ravel()
    pts[:, 2] = zz.ravel()

    gy, gx = np.meshgrid(np.arange(RES - 1), np.arange(RES - 1),
                         indexing="ij")
    i0 = (gy * RES + gx).ravel()
    i1 = i0 + 1
    i2 = i0 + RES
    i3 = i2 + 1
    indices = np.stack([i0, i2, i1, i1, i2, i3], axis=1).ravel().astype(np.int32)
    counts = np.full(2 * (RES - 1) * (RES - 1), 3, np.int32)

    uvs = np.empty((RES * RES, 2), np.float32)
    uvs[:, 0] = (np.arange(RES * RES, dtype=np.float32) % RES) / (RES - 1)
    uvs[:, 1] = (np.arange(RES * RES, dtype=np.float32) // RES) / (RES - 1)

    if path.exists():
        path.unlink()
    stage = Usd.Stage.CreateNew(str(path))
    mesh = UsdGeom.Mesh.Define(stage, "/terrain")
    mesh.CreatePointsAttr().Set(Vt.Vec3fArray.FromNumpy(pts))
    mesh.CreateFaceVertexCountsAttr().Set(counts.tolist())
    mesh.CreateFaceVertexIndicesAttr().Set(indices.tolist())
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "UVMap", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex)
    pv.Set([Gf.Vec2f(float(a), float(b)) for a, b in uvs])
    stage.Save()
    print(f"[quadric:{kind}] source grid: {RES}x{RES} verts, "
          f"{len(indices) // 3} tris -> {path.name}")


def run_graph(kind, tag, source_usd, out_usd, albedo, normal, detail,
              slope_weight, snow_line, sag_tolerance):
    """read_usd -> terrain_adaptive_mesh -> terrain_texture_bake ->
    write_usd, mirroring test_terrain_adaptive.py's harness."""
    import stage_py
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph(f"Quadric_{kind}{tag}")
    g.loadConfiguration(
        str(BIN / "Plugins" / "TerrainGen_geometry_nodes.json"))
    g.loadConfiguration(str(BIN / "geometry_nodes.json"))

    reader = g.createNode("read_usd", name=f"src_{kind}")
    adaptive = g.createNode("terrain_adaptive_mesh", name=f"ada_{kind}")
    bake = g.createNode("terrain_texture_bake", name=f"bake_{kind}")
    writer = g.createNode("write_usd", name=f"writer_{kind}")
    g.addEdge(reader, "Geometry", adaptive, "Height Field")
    g.addEdge(adaptive, "Height Field", bake, "Height Field")
    # bake passes its input through, so the writer sees the adaptive mesh
    g.addEdge(bake, "Height Field", writer, "Geometry")

    stage = stage_py.Stage(str(out_usd))
    payload = stage_py.create_payload_from_stage(stage, "/terrain")
    g.setGlobalParams(payload)

    inputs = {
        (reader, "File Name"): str(source_usd),
        (reader, "Prim Path"): "/terrain",
        (adaptive, "Detail"): detail,
        (adaptive, "Slope Weight"): slope_weight,
        (adaptive, "Max Subdiv"): 4,
        (bake, "Texture Resolution"): TEX_RES,
        (bake, "Output Path"): str(albedo),
        (bake, "Normal Output Path"): str(normal),
    }
    if snow_line is not None:
        inputs[(bake, "Snow Line")] = snow_line
    if sag_tolerance > 0.0:
        inputs[(adaptive, "Sag Tolerance")] = sag_tolerance
    g.prepare_and_execute(inputs, required_node=writer)
    stage.save()
    print(f"[quadric:{kind}] pipeline done -> {out_usd.name} "
          f"(Detail={detail} SlopeWeight={slope_weight} "
          f"SagTol={sag_tolerance if sag_tolerance > 0 else 'off'} "
          f"SnowLine={snow_line if snow_line is not None else 0.65})")


def check_against_analytic(kind, prefix):
    """Adaptive-mesh vertices must sit ON the analytic surface (exact
    field samples); any drift means the carry round-trip bent it."""
    from pxr import Usd as _Usd

    stage = _Usd.Stage.Open(str(OUT_DIR / f"{prefix}_test_modifiers.usdc"))
    mesh = UsdGeom.Mesh(stage.GetPrimAtPath("/terrain"))
    pts = np.array(mesh.GetPointsAttr().Get(), dtype=np.float64)
    fvi = np.array(mesh.GetFaceVertexIndicesAttr().Get(), dtype=np.int64)
    z_ref = analytic_height(kind, pts[:, 0], pts[:, 2])
    err = np.abs(pts[:, 1] - z_ref)
    dense_v = RES * RES
    dense_t = 2 * (RES - 1) * (RES - 1)
    print(f"[quadric:{kind}] adaptive mesh: {len(pts)} verts / "
          f"{len(fvi) // 3} tris (dense {dense_v} / {dense_t}, "
          f"{100.0 * len(fvi) // 3 / dense_t:.1f}% of dense tris)")
    print(f"[quadric:{kind}] analytic fidelity: max|dh|={err.max():.3e} "
          f"mean|dh|={err.mean():.3e}")
    return pts, fvi


def look_at(eye, target):
    eye, target = (np.asarray(v, np.float64) for v in (eye, target))
    up = np.array([0.0, 1.0, 0.0])
    fwd = target - eye
    fwd = fwd / np.linalg.norm(fwd)
    right = np.cross(fwd, up)
    right = right / np.linalg.norm(right)
    up2 = np.cross(right, fwd)
    m = Gf.Matrix4d()
    m.SetIdentity()
    m.SetRow(0, Gf.Vec4d(*right.tolist(), 0.0))
    m.SetRow(1, Gf.Vec4d(*up2.tolist(), 0.0))
    m.SetRow(2, Gf.Vec4d(*(-fwd).tolist(), 0.0))
    m.SetRow(3, Gf.Vec4d(*eye.tolist(), 1.0))
    return m


def render_views(kind, prefix, pts, fvi):
    """Same proven recipe as render_terrain_image.py: UsdPreviewSurface on
    the baked albedo + normal PNGs, sun + fill + procedural sky dome."""
    import hd_RUZINO_py as renderer
    import nodes_core_py as core
    from PIL import Image

    sp = pts * SCALE
    center = sp.mean(axis=0)
    extent = float(np.linalg.norm(sp.max(axis=0) - sp.min(axis=0)))

    scene_path = OUT_DIR / f"{prefix}_render_scene.usdc"
    if scene_path.exists():
        scene_path.unlink()
    rstage = Usd.Stage.CreateNew(str(scene_path))
    mesh = UsdGeom.Mesh.Define(rstage, "/Terrain")
    mesh.CreatePointsAttr().Set(Vt.Vec3fArray.FromNumpy(
        sp.astype(np.float32)))
    mesh.CreateFaceVertexCountsAttr().Set(
        np.full(len(fvi) // 3, 3, np.int32).tolist())
    mesh.CreateFaceVertexIndicesAttr().Set(fvi.astype(np.int32).tolist())
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)

    # The adaptive mesh carries uv in exact field parametrization.
    src = Usd.Stage.Open(str(OUT_DIR / f"{prefix}_test_modifiers.usdc"))
    src_uv = src.GetPrimAtPath("/terrain").GetAttribute("primvars:UVMap").Get()
    pv = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "UVMap", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex)
    pv.Set(src_uv)
    print(f"[quadric:{kind}] UVMap primvar: "
          f"{'ok' if src_uv else 'MISSING'} ({len(src_uv) if src_uv else 0})")

    mat = UsdShade.Material.Define(rstage, "/Terrain/Terrain_mat")
    pbr = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/PBRShader")
    pbr.CreateIdAttr("UsdPreviewSurface")
    st_reader = UsdShade.Shader.Define(
        rstage, "/Terrain/Terrain_mat/stReader")
    st_reader.CreateIdAttr("UsdPrimvarReader_float2")
    st_reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("UVMap")

    albedo = (OUT_DIR / f"{prefix}_albedo.png").resolve()
    tex = UsdShade.Shader.Define(rstage, "/Terrain/Terrain_mat/diffuseTexture")
    tex.CreateIdAttr("UsdUVTexture")
    tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(albedo.as_posix()))
    tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
        st_reader.ConnectableAPI(), "result")
    tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
    tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")
    pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        tex.ConnectableAPI(), "rgb")
    pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.95)

    nrm = (OUT_DIR / f"{prefix}_normal.png").resolve()
    nrm_tex = UsdShade.Shader.Define(
        rstage, "/Terrain/Terrain_mat/normalTexture")
    nrm_tex.CreateIdAttr("UsdUVTexture")
    nrm_tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(nrm.as_posix()))
    nrm_tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
        st_reader.ConnectableAPI(), "result")
    nrm_tex.CreateInput("scale", Sdf.ValueTypeNames.Color4f).Set(
        (2.0, 2.0, 2.0, 1.0))
    nrm_tex.CreateInput("bias", Sdf.ValueTypeNames.Color4f).Set(
        (-1.0, -1.0, -1.0, 0.0))
    nrm_tex.CreateInput("wrapS", Sdf.ValueTypeNames.Token).Set("clamp")
    nrm_tex.CreateInput("wrapT", Sdf.ValueTypeNames.Token).Set("clamp")
    pbr.CreateInput("normal", Sdf.ValueTypeNames.Vector3f).ConnectToSource(
        nrm_tex.ConnectableAPI(), "rgb")
    mat.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)

    cam = UsdGeom.Camera.Define(rstage, "/Camera")
    cam.GetFocalLengthAttr().Set(24.0)
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.GetClippingRangeAttr().Set((0.01, 2000.0))
    UsdGeom.Xformable(cam).AddTransformOp()

    # Lighting tuned for snow-dominant quadrics: the terrain-render recipe's
    # sun 3.0 / fill 1.5 / dome 2.0 clips a 0.9-albedo snowfield to a blank
    # white page now that the corrected normals actually receive the light.
    sun = UsdLux.DistantLight.Define(rstage, "/Sun")
    sun.CreateIntensityAttr().Set(0.8)
    sun.CreateAngleAttr().Set(0.53)
    sun_xf = Gf.Matrix4d()
    sun_xf.SetIdentity()
    sun_xf.SetRow(2, Gf.Vec4d(-0.45, -0.85, -0.3, 0.0))
    UsdGeom.Xformable(sun).AddTransformOp().Set(sun_xf)

    fill = UsdLux.DistantLight.Define(rstage, "/Fill")
    fill.CreateIntensityAttr().Set(0.4)
    fill_xf = Gf.Matrix4d()
    fill_xf.SetIdentity()
    fill_xf.SetRow(2, Gf.Vec4d(0.6, -0.3, 0.55, 0.0))
    UsdGeom.Xformable(fill).AddTransformOp().Set(fill_xf)

    sky = rstage.DefinePrim("/Sky", "DomeLight")
    sky.CreateAttribute(
        "shader_path", Sdf.ValueTypeNames.String).Set(
        "callables/eval_dome_light_procedural_sky.slang")
    UsdLux.DomeLight(sky).CreateIntensityAttr().Set(0.7)
    rstage.GetRootLayer().Save()

    # Views: full 3/4 + a close-up pinned to where the mesher must CHANGE
    # refinement (paraboloid: mid-slope level transition ring; saddle:
    # the flat diagonal crossing at the center).
    if kind == "paraboloid":
        aim_x, aim_z = 0.30 * SIZE, 0.0
        aim_y = analytic_height(kind, aim_x, aim_z)
        views = [
            ("full", (0.7, 0.5, 0.8), center + np.array([0.0, 0.02, 0.0]) * extent, 0.62),
            ("closeup", (0.9, 0.35, 0.5), center + np.array([aim_x, aim_y, aim_z]) * SCALE, 0.30),
        ]
    else:
        aim_x, aim_z = 0.35 * SIZE, -0.10 * SIZE
        aim_y = analytic_height(kind, aim_x, aim_z)
        views = [
            ("full", (0.55, 0.4, 0.75), center + np.array([0.0, 0.02, 0.0]) * extent, 0.62),
            ("closeup", (0.55, 0.16, 0.75), center + np.array([aim_x, aim_y, aim_z]) * SCALE, 0.38),
        ]

    for name, eye_dir, target, fit in views:
        eye_dir = np.asarray(eye_dir, np.float64)
        eye_dir = eye_dir / np.linalg.norm(eye_dir)
        eye = center + eye_dir * extent * fit
        xform = UsdGeom.Xformable(cam)
        xform.GetOrderedXformOps()[0].Set(
            look_at(eye, np.asarray(target, np.float64)))
        rstage.GetRootLayer().Save()

        hydra = renderer.HydraRenderer(str(scene_path), WIDTH, HEIGHT_PX)
        node_system = hydra.get_node_system()
        node_system.load_configuration(str(BIN / "render_nodes.json"))
        node_system.init()
        tree = node_system.get_node_tree()
        executor = node_system.get_node_tree_executor()
        rng = tree.add_node("rng_texture")
        ray_gen = tree.add_node("node_render_ray_generation")
        path_trace = tree.add_node("path_tracing")
        accumulate = tree.add_node("accumulate")
        rng_buffer = tree.add_node("rng_buffer")
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
            (ray_gen, "Aperture"): 0.0,
            (ray_gen, "Focus Distance"): 2.0,
            (ray_gen, "Scatter Rays"): False,
            (accumulate, "Max Samples"): SPP,
        }.items():
            socket = node.get_input_socket(socket_name)
            executor.sync_node_from_external_storage(
                socket, core.to_meta_any(value))

        for _ in range(SPP):
            hydra.render(0.0)
        raw = hydra.get_output_texture()
        if not raw or len(raw) != WIDTH * HEIGHT_PX * 4:
            print(f"[quadric:{kind}] bad texture for {name}")
            hydra.stop()
            continue
        img = np.asarray(raw, dtype=np.float32).reshape(HEIGHT_PX, WIDTH, 4)
        rgb = np.clip(img[:, :, :3], 0.0, 1.0)
        rgb = (rgb * 255).astype(np.uint8)
        rgb = np.flipud(rgb)
        out = OUT_DIR / f"{prefix}_{name}.png"
        Image.fromarray(rgb).save(out)
        print(f"[quadric:{kind}] saved {out.name} (mean {rgb.mean():.1f})")
        hydra.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", choices=["paraboloid", "saddle", "both"],
                    default="both")
    ap.add_argument("--detail", type=float, default=0.4)
    ap.add_argument("--slope-weight", type=float, default=1.0)
    ap.add_argument("--snow-line", type=float, default=None,
                    help="bake snow line (default: node default 0.65)")
    ap.add_argument("--sag", type=float, default=0.0,
                    help="adaptive mesh sag tolerance in world units "
                         "(curvature term; 0 = off)")
    ap.add_argument("--tag", default="",
                    help="output filename suffix, e.g. '_hi'")
    ap.add_argument("--no-render", action="store_true",
                    help="pipeline + stats only")
    args = ap.parse_args()
    kinds = (["paraboloid", "saddle"] if args.surface == "both"
             else [args.surface])

    OUT_DIR.mkdir(exist_ok=True)
    for kind in kinds:
        prefix = f"quadric_{kind}{args.tag}"
        source = OUT_DIR / f"{prefix}_source.usdc"
        write_source_usd(kind, source)
        run_graph(kind, args.tag, source,
                  OUT_DIR / f"{prefix}_test.usdc",
                  OUT_DIR / f"{prefix}_albedo.png",
                  OUT_DIR / f"{prefix}_normal.png",
                  args.detail, args.slope_weight, args.snow_line,
                  args.sag)
        pts, fvi = check_against_analytic(kind, prefix)
        if not args.no_render:
            render_views(kind, prefix, pts, fvi)
    print("[quadric] done")


if __name__ == "__main__":
    main()
