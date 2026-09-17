"""
terrain_scatter_points + instance_on_points integration tests.

Node outputs are read DIRECTLY through the geometry_py bindings
(vertices / width / normals on PointsComponent, the "biome" vertex scalar
quantity on the baked mesh), so the data assertions check what the nodes
actually produced. The instancing test verifies the USD side: write_usd
must emit a PointInstancer over-spec + <path>/Prototype mesh layout, read
back as raw Sdf specs (in-process Usd.Stage composition on a layer the
writer stage still holds is unreliable).
"""
import os
from pathlib import Path

import numpy as np
import pytest

from ruzino_graph import RuzinoGraph

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY_DIR = os.path.abspath(
    os.path.join(TESTS_DIR, "..", "..", "..", "..", "Binaries", "Release"))
OUT_DIR = Path(BINARY_DIR) / "test_output"

RES = 128
WORLD = 100.0
TERRAIN_HEIGHT = 40.0
SEED = 8  # same seed as test_terrain_nodes.py fixtures


def _vec3s_to_np(vecs):
    """Mesh getters already return numpy arrays; Points getters return
    geometry_py.vec3 (x/y/z only) that needs the tuple unpack."""
    arr = np.asarray(vecs)
    if arr.dtype != object:
        return arr.astype(np.float64)
    return np.array([(v.x, v.y, v.z) for v in vecs], dtype=np.float64)


def _graph(name):
    g = RuzinoGraph(name)
    g.loadConfiguration(
        os.path.join(BINARY_DIR, "Plugins", "TerrainGen_geometry_nodes.json"))
    g.loadConfiguration(os.path.join(BINARY_DIR, "geometry_nodes.json"))
    return g


def _run_scatter(masked=False):
    """One graph: heightfield [-> bake] -> scatter. Returns
    (terrain mesh geometry, scatter points geometry, baked mesh or None)."""
    g = _graph("ScatterPlain" if not masked else "ScatterMasked")
    hf = g.createNode("terrain_heightfield", name="hf")
    prev = hf
    bake = None
    if masked:
        bake = g.createNode("terrain_texture_bake", name="bake")
        g.addEdge(hf, "Height Field", bake, "Height Field")
        prev = bake
    scatter = g.createNode("terrain_scatter_points", name="scatter")
    g.addEdge(prev, "Height Field", scatter, "Height Field")

    inputs = {
        (hf, "Resolution"): RES,
        (hf, "Size"): WORLD,
        (hf, "Height"): TERRAIN_HEIGHT,
        (hf, "Seed"): SEED,
        (scatter, "Count"): 200,
        (scatter, "Min Distance"): 4.0,
        (scatter, "Seed"): 3,
        (scatter, "Scale Min"): 0.7,
        (scatter, "Scale Max"): 1.3,
    }
    if masked:
        inputs.update({
            (bake, "Texture Resolution"): 256,
            (bake, "Output Path"): "test_output/scatter_mask_albedo.png",
            (bake, "Normal Output Path"):
                "test_output/scatter_mask_normal.png",
            (scatter, "Count"): 400,
            (scatter, "Min Distance"): 3.0,
            (scatter, "Mask Field"): "biome",
            (scatter, "Mask Min"): 2.0,
            (scatter, "Mask Max"): 2.0,
        })
    g.prepare_and_execute(inputs, required_node=scatter)
    # FRAMEWORK CAVEAT: a second getOutput(Geometry) extraction aliases the
    # executor's external storage and invalidates the previous wrapper
    # (reproduced with tree_generate->tree_to_mesh alone). Copy each output
    # to numpy IMMEDIATELY and never touch a prior wrapper again.
    import geometry_py as geom_py
    terrain = _vec3s_to_np(
        geom_py.extract_geometry_from_meta_any(
            g.getOutput(prev, "Height Field"))
        .get_mesh_component(0)
        .get_vertices())
    points_geom = geom_py.extract_geometry_from_meta_any(
        g.getOutput(scatter, "Points"))
    pc = points_geom.get_points_component(0)
    points = {
        "pos": _vec3s_to_np(pc.get_vertices()),
        "nrm": _vec3s_to_np(pc.get_normals()),
        "width": np.array(list(pc.get_width()), dtype=np.float64),
    }
    biome = None
    if masked:
        baked = geom_py.extract_geometry_from_meta_any(
            g.getOutput(bake, "Height Field"))
        mesh = baked.get_mesh_component(0)
        assert "biome" in mesh.get_vertex_scalar_quantity_names()
        biome = np.array(
            mesh.get_vertex_scalar_quantity("biome"), dtype=np.float64)
    return terrain, points, biome


def _grid_from_mesh(terrain_verts):
    res = int(round(np.sqrt(len(terrain_verts))))
    assert res * res == len(terrain_verts)
    return terrain_verts.reshape(res, res, 3), res


def _bilinear(grid, res, xz):
    """Bilinear sample of a (res,res) grid at world XZ positions."""
    half = WORLD / 2.0
    g = (xz + half) / WORLD * (res - 1)
    x0 = np.clip(np.floor(g[:, 0]).astype(int), 0, res - 2)
    y0 = np.clip(np.floor(g[:, 1]).astype(int), 0, res - 2)
    fx = g[:, 0] - x0
    fy = g[:, 1] - y0
    return (grid[y0, x0] * (1 - fx) * (1 - fy) +
            grid[y0, x0 + 1] * fx * (1 - fy) +
            grid[y0 + 1, x0] * (1 - fx) * fy +
            grid[y0 + 1, x0 + 1] * fx * fy)


def test_scatter_count_spacing_scales():
    _, points, _ = _run_scatter()
    pos = points["pos"]
    widths = points["width"]

    assert len(pos) == 200, f"expected 200 points, got {len(pos)}"
    half = WORLD / 2.0
    assert pos[:, 0].min() >= -half and pos[:, 0].max() <= half
    assert pos[:, 2].min() >= -half and pos[:, 2].max() <= half

    # Min-distance rejection: no two accepted points closer than min_dist.
    xy = pos[:, [0, 2]]
    d2 = ((xy[:, None, :] - xy[None, :, :]) ** 2).sum(-1)
    np.fill_diagonal(d2, np.inf)
    assert d2.min() >= 4.0 ** 2 * 0.99, \
        f"min pairwise distance {np.sqrt(d2.min()):.3f} < 4.0"

    # Per-point scale band carried in width.
    assert widths.min() >= 0.7 - 1e-4 and widths.max() <= 1.3 + 1e-4


def test_scatter_points_sit_on_surface():
    terrain, points, _ = _run_scatter()
    pos = points["pos"]
    grid, res = _grid_from_mesh(terrain)

    # Y matches the terrain surface: bilinear on the written grid, the same
    # field the node sampled.
    h = _bilinear(grid[:, :, 1], res, pos[:, [0, 2]])
    assert np.abs(pos[:, 1] - h).max() < 2e-3, \
        f"max height deviation {np.abs(pos[:, 1] - h).max():.4f}"

    # Upright by default (trees grow against gravity, not normal to slope).
    assert np.allclose(points["nrm"], [0.0, 1.0, 0.0], atol=1e-5)


def test_biome_mask_restricts_scatter():
    """bake attaches the biome vertex quantity; a grass-only mask must
    reject every non-grass point (checked against that same field)."""
    terrain, points, biome = _run_scatter(masked=True)
    grid, res = _grid_from_mesh(terrain)
    assert len(biome) == res * res

    pos = points["pos"]
    assert len(pos) > 0, "grass-only mask rejected everything"

    # Bilinear biome at each accepted point must be grass (2). Bilinear
    # across a biome boundary blends ids (2 vs 1 -> 1.6); accept a small
    # blend band but require the neighborhood to be grass-dominated.
    v = _bilinear(biome.reshape(res, res), res, pos[:, [0, 2]])
    assert v.min() >= 1.5, \
        f"non-grass point accepted (min biome value {v.min():.2f})"
    assert (v > 1.9).mean() > 0.95, \
        f"too many points on biome boundaries ({(v > 1.9).mean():.1%} clean)"


def test_instance_yup_width_scale():
    """instance_on_points: Y-up keeps prototypes upright, widths become
    per-instance scales x multiplier; write_usd emits the PointInstancer
    over-spec + Prototype mesh layout into the modifier sidecar."""
    from pxr import Sdf
    g = _graph("ScatterInst")
    hf = g.createNode("terrain_heightfield", name="hf")
    scatter = g.createNode("terrain_scatter_points", name="scatter")
    inst = g.createNode("instance_on_points", name="inst")
    w = g.createNode("write_usd", name="w_forest")
    g.addEdge(hf, "Height Field", scatter, "Height Field")
    g.addEdge(hf, "Height Field", inst, "Geometry")
    g.addEdge(scatter, "Points", inst, "Points")
    g.addEdge(inst, "Geometry", w, "Geometry")

    import stage_py
    stage_file = OUT_DIR / "scatterinst.usdc"
    if stage_file.exists():
        stage_file.unlink()
    stage = stage_py.Stage(str(stage_file))
    g.setGlobalParams(
        stage_py.create_payload_from_stage(stage, "/ScatterInst"))

    n = 60
    inputs = {
        (hf, "Resolution"): RES,
        (hf, "Size"): WORLD,
        (hf, "Height"): TERRAIN_HEIGHT,
        (hf, "Seed"): SEED,
        (scatter, "Count"): n,
        (scatter, "Min Distance"): 5.0,
        (scatter, "Seed"): 3,
        (scatter, "Scale Min"): 0.7,
        (scatter, "Scale Max"): 1.3,
        (inst, "Y-up"): True,
        (inst, "Use Width as Scale"): True,
        (inst, "Scale Multiplier"): 2.0,
        (w, "Sub Path"): "forest",
    }
    g.prepare_and_execute(inputs, required_node=w)
    assert stage.save()

    mod = Sdf.Layer.FindOrOpen(
        str(stage_file).replace(".usdc", "_modifiers.usdc"))
    assert mod, "modifier sidecar missing"

    inst_spec = mod.GetPrimAtPath("/ScatterInst/forest")
    assert inst_spec, "PointInstancer prim missing in modifier layer"
    assert inst_spec.typeName == "PointInstancer", inst_spec.typeName

    proto_spec = mod.GetPrimAtPath("/ScatterInst/forest/Prototype")
    assert proto_spec, "prototype mesh prim missing"
    assert proto_spec.typeName == "Mesh"
    proto_pts = proto_spec.attributes["points"].default
    assert len(proto_pts) == RES * RES, "prototype should be the terrain mesh"

    positions = inst_spec.attributes["positions"].default
    scales = inst_spec.attributes["scales"].default
    proto_indices = inst_spec.attributes["protoIndices"].default
    assert len(positions) == n and len(scales) == n
    assert len(proto_indices) == n and all(i == 0 for i in proto_indices)
    s = np.array([[v[0], v[1], v[2]] for v in scales])
    assert s.min() >= 1.4 - 1e-3 and s.max() <= 2.6 + 1e-3
    assert np.allclose(s[:, 0], s[:, 1]) and np.allclose(s[:, 0], s[:, 2]), \
        "uniform scale expected"

    orients = inst_spec.attributes["orientations"].default
    assert len(orients) == n
    # Upright points (0,1,0) with Y-up axis -> identity orientation.
    for q in orients:
        assert abs(q.real - 1.0) < 1e-4, f"orientation not identity: {q}"
        assert all(abs(c) < 1e-4 for c in q.imaginary), \
            f"orientation not identity: {q}"

    rel = inst_spec.relationships.get("prototypes")
    assert rel, "prototypes relationship missing"
    assert str(rel.targetPathList.GetAddedOrExplicitItems()[0]) == \
        "/ScatterInst/forest/Prototype"
