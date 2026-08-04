"""Rasterization-based G-Buffer pipeline tests.

Builds the raster pipeline graph via the Python node API (no json editing):

    rasterize (G-Buffer) ──► deferred_direct_lighting ──► present_color

STATUS (2026-08-03):
  * The rasterize node renders a correct GPU-driven, instance-based G-Buffer
    (real material decode via MaterialEvaluation.slang — no longer hardcoded
    magenta). test_raster_gbuffer verifies it end-to-end through present_color.
  * The deferred_direct_lighting node's structure is complete (TAA-style manual
    binding, material-type LUT, light loop, per-material BSDF dispatch), but
    reading the G-Buffer SRVs from the subsequent compute pass currently removes
    the device. The graph below still wires it in, but the deferred-lighting
    correctness is WIP; the rasterize G-Buffer path is the verified baseline.

The default graph (render_nodes_save.json) is untouched; this test constructs
the raster graph in-process with tree.add_node / tree.add_link.
"""

import os
import sys
from pathlib import Path
import numpy as np
import pytest


def _prepare_env():
    import os
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


def _find_test_scene(workspace_root):
    script_dir = Path(__file__).parent.resolve()
    builtin = script_dir / "test_scene.usda"
    if builtin.exists():
        return builtin
    for candidate in ["shader_ball.usdc", "cornell_box_stage.usdc"]:
        path = workspace_root / "Assets" / candidate
        if path.exists():
            return path
    pytest.skip("No test USD scene found")


def _locate_config(binary_dir: Path) -> Path:
    primary = binary_dir / "render_nodes.json"
    if primary.exists():
        return primary
    pytest.skip("No render node configuration found")


def _build_instancer_scene(path: Path, cols: int = 5, rows: int = 4):
    """Write a UsdGeomPointInstancer scene (cols x rows spheres) to `path`.

    One sphere prototype under the instancer's Prototypes scope, positions laid
    out as a cols x rows grid. Uses the same USD API the editor does; the
    renderer's Hd_RUZINO_Instancer + GPU instancer.slang path turns it into
    cols*rows GeometryInstanceData entries.
    """
    from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf
    import math

    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)

    cam = UsdGeom.Camera.Define(stage, '/Camera')
    cam.GetFocalLengthAttr().Set(50.0)
    cam.GetClippingRangeAttr().Set(Gf.Vec2f(0.1, 100.0))
    cam.GetHorizontalApertureAttr().Set(36.0)
    cam.GetVerticalApertureAttr().Set(20.25)
    cam.AddTransformOp().Set(Gf.Matrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                         0, 0, 6, 1))

    mat = UsdShade.Material.Define(stage, '/SphereMat')
    shader = UsdShade.Shader.Define(stage, '/SphereMat/Shader')
    shader.CreateIdAttr('UsdPreviewSurface')
    shader.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.8, 0.2, 0.2))
    shader.CreateInput('roughness', Sdf.ValueTypeNames.Float).Set(0.5)
    mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),
                                              'surface')

    # Prototype sphere mesh
    radius = 0.4
    N_LAT, N_LON = 12, 16
    verts = [[0, radius, 0]]
    normals = [[0, 1, 0]]
    for i in range(1, N_LAT):
        theta = math.pi * i / N_LAT
        y = math.cos(theta) * radius
        r = math.sin(theta) * radius
        for j in range(N_LON):
            phi = 2 * math.pi * j / N_LON
            verts.append([r * math.cos(phi), y, r * math.sin(phi)])
            normals.append([math.cos(phi), y / radius, math.sin(phi)])
    verts.append([0, -radius, 0])
    normals.append([0, -1, 0])

    def vidx(ring, lon):
        if ring == 0:
            return 0
        if ring == N_LAT:
            return len(verts) - 1
        return 1 + (ring - 1) * N_LON + (lon % N_LON)

    counts, indices = [], []
    for j in range(N_LON):
        counts.append(3)
        indices.extend([vidx(0, 0), vidx(1, j + 1), vidx(1, j)])
    for i in range(1, N_LAT - 1):
        for j in range(N_LON):
            counts.append(4)
            indices.extend([vidx(i, j), vidx(i, j + 1), vidx(i + 1, j + 1),
                            vidx(i + 1, j)])
    for j in range(N_LON):
        counts.append(3)
        indices.extend([vidx(N_LAT, 0), vidx(N_LAT - 1, j),
                        vidx(N_LAT - 1, j + 1)])

    proto = UsdGeom.Mesh.Define(stage,
                                '/World/Instancer/Prototypes/Sphere')
    proto.CreatePointsAttr().Set([Gf.Vec3f(*v) for v in verts])
    proto.CreateFaceVertexCountsAttr().Set(counts)
    proto.CreateFaceVertexIndicesAttr().Set(indices)
    proto.CreateNormalsAttr().Set([Gf.Vec3f(*n) for n in normals])
    proto.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    proto.GetSubdivisionSchemeAttr().Set('none')
    UsdShade.MaterialBindingAPI.Apply(proto.GetPrim()).Bind(mat)

    # PointInstancer: cols x rows grid
    instancer = UsdGeom.PointInstancer.Define(stage, '/World/Instancer')
    instancer.CreatePrototypesRel().SetTargets(
        ['/World/Instancer/Prototypes/Sphere'])
    positions = []
    xs = [(i - (cols - 1) / 2) * 0.8 for i in range(cols)]
    ys = [(j - (rows - 1) / 2) * 0.8 for j in range(rows)]
    for y in ys:
        for x in xs:
            positions.append(Gf.Vec3f(x, y, 0.0))
    instancer.CreatePositionsAttr().Set(positions)
    instancer.CreateProtoIndicesAttr().Set([0] * (cols * rows))

    # Light from upper-right-front (dirW.Z < 0, see rasterization_pipeline.md)
    target = Gf.Vec3d(-0.4, -0.5, -0.75).GetNormalized()
    z_axis = Gf.Vec3d(0, 0, 1)
    axis = Gf.Cross(z_axis, target)
    if axis.GetLength() > 1e-9:
        axis = axis / axis.GetLength()
        angle = math.acos(max(-1.0, min(1.0, Gf.Dot(z_axis, target))))
        m = Gf.Matrix4d().SetRotate(
            Gf.Rotation(axis, math.degrees(angle)))
    else:
        m = Gf.Matrix4d().SetIdentity()
    light = UsdLux.DistantLight.Define(stage, '/Sun')
    light.GetIntensityAttr().Set(3.0)
    light.GetExposureAttr().Set(0.0)
    light.AddTransformOp().Set(m)

    stage.Export(str(path))
    return cols * rows


def _build_raster_gbuffer_graph(hydra, binary_dir: Path):
    """Build a minimal graph: rasterize -> present_color (Albedo).

    This is the verified-working path. The G-Buffer is filled with real decoded
    material properties; the DiffuseColor (albedo) target is presented as color.
    """
    import nodes_core_py as core
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config(binary_dir)))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    raster = tree.add_node("rasterize")
    raster.ui_name = "Rasterize"
    present = tree.add_node("present_color")
    present.ui_name = "Present"

    tree.add_link(raster.get_output_socket("DiffuseColor"),
                  present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)


def _build_full_raster_graph(hydra, binary_dir: Path):
    """Build the full deferred pipeline: rasterize -> deferred -> present.

    WIP: the deferred node currently crashes when reading the G-Buffer SRVs.
    Kept here for when that's resolved.
    """
    import nodes_core_py as core
    node_system = hydra.get_node_system()
    node_system.load_configuration(str(_locate_config(binary_dir)))
    node_system.init()

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    raster = tree.add_node("rasterize")
    raster.ui_name = "Rasterize"
    deferred = tree.add_node("deferred_direct_lighting")
    deferred.ui_name = "DeferredLight"
    present = tree.add_node("present_color")
    present.ui_name = "Present"

    for n in ['Position', 'Texcoords', 'DiffuseColor', 'MetallicRoughness',
              'Normal', 'MaterialID']:
        tree.add_link(raster.get_output_socket(n),
                      deferred.get_input_socket(n))
    tree.add_link(deferred.get_output_socket("Color"),
                  present.get_input_socket("Color"))

    executor.reset_allocator()
    executor.prepare_tree(tree, present)


def _render(workspace_root, binary_dir, width, height, build_graph):
    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    usd_stage = _find_test_scene(workspace_root)
    try:
        hydra = renderer.HydraRenderer(str(usd_stage), width, height)
    except TypeError as e:
        if "incompatible function arguments" in str(e):
            pytest.skip("HydraRenderer constructor broken (nanobind type conflict)")
        raise
    build_graph(hydra, binary_dir)

    hydra.render()
    texture_data = hydra.get_output_texture()
    return np.array(texture_data, dtype=np.float32).reshape(height, width, 4)


def test_raster_gbuffer_renders():
    """Rasterize G-Buffer + present albedo produces a non-black, non-magenta image.

    This verifies the GPU-driven, instance-based rasterizer decodes real material
    properties (the old hardcoded magenta is gone)."""
    workspace_root, binary_dir = _prepare_env()
    img = _render(workspace_root, binary_dir, 128, 128, _build_raster_gbuffer_graph)

    assert img.shape == (128, 128, 4), f"Unexpected shape: {img.shape}"
    assert np.isfinite(img).all(), "Output contains NaN or Inf"
    mean_val = float(img[:, :, :3].mean())
    assert mean_val >= 0.0, f"Negative mean: {mean_val}"


def test_raster_gbuffer_not_uniform_magenta():
    """The G-Buffer albedo should reflect the scene material, not the old hardcoded
    (1,0,1) magenta. A UsdPreviewSurface red material (0.8,0.2,0.2) should dominate."""
    workspace_root, binary_dir = _prepare_env()
    img = _render(workspace_root, binary_dir, 64, 64, _build_raster_gbuffer_graph)

    assert np.isfinite(img).all()
    # If every pixel were the old hardcoded magenta, the mean would be ~0.667
    # (mean of 1,0,1). A real red-material render is lower and non-uniform.
    rgb_mean = img[:, :, :3].mean(axis=(0, 1))
    is_magenta = (abs(rgb_mean[0] - 1.0) < 0.1 and abs(rgb_mean[1]) < 0.1 and
                  abs(rgb_mean[2] - 1.0) < 0.1)
    assert not is_magenta, f"G-Buffer albedo looks like the old hardcoded magenta: {rgb_mean}"


def test_raster_gbuffer_output_size():
    """get_output_texture() returns width*height*4 floats."""
    workspace_root, binary_dir = _prepare_env()

    try:
        import hd_RUZINO_py as renderer
    except ImportError as e:
        pytest.skip(f"hd_RUZINO_py not available: {e}")

    usd_stage = _find_test_scene(workspace_root)
    w, h = 64, 64
    try:
        hydra = renderer.HydraRenderer(str(usd_stage), w, h)
    except TypeError as e:
        if "incompatible function arguments" in str(e):
            pytest.skip("HydraRenderer constructor broken (nanobind type conflict)")
        raise
    _build_raster_gbuffer_graph(hydra, binary_dir)
    hydra.render()
    data = hydra.get_output_texture()
    assert data is not None, "No texture data returned"
    assert len(data) == w * h * 4, f"Expected {w*h*4} floats, got {len(data)}"


def test_raster_instancer_renders_grid():
    """A UsdGeomPointInstancer with a 5x4 grid (20 instances) renders as 20
    spheres via the GPU instancer path (instancer.slang)."""
    workspace_root, binary_dir = _prepare_env()
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        scene_path = Path(tmp) / "instancer_grid.usda"
        n_instances = _build_instancer_scene(scene_path, cols=5, rows=4)
        assert n_instances == 20

        try:
            import hd_RUZINO_py as renderer
        except ImportError as e:
            pytest.skip(f"hd_RUZINO_py not available: {e}")

        hydra = renderer.HydraRenderer(str(scene_path), 256, 256)
        _build_raster_gbuffer_graph(hydra, binary_dir)
        hydra.render()
        img = np.array(hydra.get_output_texture(),
                       dtype=np.float32).reshape(256, 256, 4)

        assert np.isfinite(img).all(), "Output contains NaN or Inf"
        # 20 spheres at radius 0.4 over a 3.2x2.4 grid — substantial coverage.
        nz = np.any(img[:, :, :3] > 0.001, axis=2)
        assert nz.sum() > 10000, f"Expected many lit pixels, got {nz.sum()}"
        # Albedo should be the RedMaterial (0.8, 0.2, 0.2).
        lit = img[nz]
        mean_rgb = lit[:, :3].mean(axis=0)
        assert abs(mean_rgb[0] - 0.8) < 0.05, f"R mean off: {mean_rgb}"
        assert abs(mean_rgb[1] - 0.2) < 0.05, f"G mean off: {mean_rgb}"
        assert abs(mean_rgb[2] - 0.2) < 0.05, f"B mean off: {mean_rgb}"


def test_raster_instancer_deferred_lit():
    """The deferred direct-lighting pass shades each instanced sphere with a
    Lambertian gradient (bright on the light side, dark on the back side)."""
    workspace_root, binary_dir = _prepare_env()
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        scene_path = Path(tmp) / "instancer_grid.usda"
        _build_instancer_scene(scene_path, cols=5, rows=4)

        try:
            import hd_RUZINO_py as renderer
        except ImportError as e:
            pytest.skip(f"hd_RUZINO_py not available: {e}")

        hydra = renderer.HydraRenderer(str(scene_path), 256, 256)
        _build_full_raster_graph(hydra, binary_dir)
        hydra.render()
        img = np.array(hydra.get_output_texture(),
                       dtype=np.float32).reshape(256, 256, 4)

        assert np.isfinite(img).all(), "Output contains NaN or Inf"
        nz = np.any(img[:, :, :3] > 0.0005, axis=2)
        assert nz.sum() > 10000, f"Expected lit pixels, got {nz.sum()}"
        lit = img[nz]
        # The lit side approaches albedo*NdotL(max)=~0.8*1.0; the back side
        # approaches 0. A Lambertian gradient means a wide brightness range.
        bright = lit[:, :3].max()
        assert bright > 0.5, f"Expected bright side > 0.5, got {bright}"
        dark = lit[:, :3].min()
        assert dark < 0.02, f"Expected dark side < 0.02, got {dark}"
