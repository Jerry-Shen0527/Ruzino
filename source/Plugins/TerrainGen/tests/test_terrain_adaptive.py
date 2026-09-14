"""
Adaptive heightfield mesh (terrain_adaptive_mesh) pipeline tests.

Executes heightfield -> GPU erosion -> adaptive mesh -> write_usd end to
end, reads the mesh back with pxr, and asserts the properties that make
the scheme B mesher correct:

  watertight  -- every undirected edge is shared by exactly 2 triangles
                 (1 only on the field rim); every directed edge exactly
                 once (consistent winding). In a path tracer a crack is a
                 light leak, so this is THE contract.
  reduction   -- strictly fewer vertices than the dense grid on this
                 terrain, with the world bounds preserved.
  bake chain  -- texture_bake accepts the adaptive mesh (scatter +
                 dilation field reconstruction) and writes both PNGs.
"""
import os

import numpy as np
from ruzino_graph import RuzinoGraph

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
BINARY_DIR = os.path.abspath(
    os.path.join(TESTS_DIR, "..", "..", "..", "..", "Binaries", "Release"))
OUT_USD = os.path.join(BINARY_DIR, "test_output", "terrain_adaptive_test.usdc")
ALBEDO = os.path.join(BINARY_DIR, "test_output", "terrain_adaptive_albedo.png")
NORMAL = os.path.join(BINARY_DIR, "test_output", "terrain_adaptive_normal.png")

RES = 241  # (RES-1) % 16 == 0: the adaptive tiling contract


def _run_chain(tag, adaptive_inputs, with_bake=False):
    """heightfield -> erode(GPU) -> adaptive [-> bake]; saves the stage and
    returns the stage path."""
    import stage_py

    g = RuzinoGraph(f"Terrain_{tag}")
    g.loadConfiguration(
        os.path.join(BINARY_DIR, "Plugins", "TerrainGen_geometry_nodes.json"))
    g.loadConfiguration(os.path.join(BINARY_DIR, "geometry_nodes.json"))

    hf = g.createNode("terrain_heightfield", name=f"hf_{tag}")
    erode = g.createNode("terrain_erode_hydraulic", name=f"erode_{tag}")
    adaptive = g.createNode("terrain_adaptive_mesh", name=f"ada_{tag}")
    g.addEdge(hf, "Height Field", erode, "Height Field")
    g.addEdge(erode, "Height Field", adaptive, "Height Field")

    writer = g.createNode("write_usd", name=f"writer_{tag}")
    g.addEdge(adaptive, "Height Field", writer, "Geometry")

    stage = stage_py.Stage(OUT_USD)
    payload = stage_py.create_payload_from_stage(stage, "/terrain")
    g.setGlobalParams(payload)

    inputs = {
        (hf, "Resolution"): RES,
        (hf, "Size"): 100.0,
        (hf, "Height"): 40.0,
        (hf, "Base Level"): 0.0,
        (hf, "Seed"): 8,
        (erode, "Iterations"): 60,
    }
    for k, v in adaptive_inputs.items():
        inputs[(adaptive, k)] = v

    required = writer
    if with_bake:
        bake = g.createNode("terrain_texture_bake", name=f"bake_{tag}")
        g.addEdge(adaptive, "Height Field", bake, "Height Field")
        inputs[(bake, "Texture Resolution")] = 512
        inputs[(bake, "Output Path")] = ALBEDO
        inputs[(bake, "Normal Output Path")] = NORMAL
        required = bake

    g.prepare_and_execute(inputs, required_node=required)
    stage.save()
    return OUT_USD


def _load_mesh():
    """Read /terrain back from the write_usd modifiers sidecar."""
    from pxr import Usd, UsdGeom, Vt

    stage = Usd.Stage.Open(OUT_USD.replace(".usdc", "_modifiers.usdc"))
    mesh = UsdGeom.Mesh(stage.GetPrimAtPath("/terrain"))
    pts = np.array(mesh.GetPointsAttr().Get(), dtype=np.float64)
    fvi = np.array(mesh.GetFaceVertexIndicesAttr().Get(), dtype=np.int64)
    return pts, fvi


def test_adaptive_mesh_watertight_and_reduced():
    _run_chain("ada", {"Detail": 0.4, "Slope Weight": 1.0,
                       "Max Subdiv": 4})
    pts, fvi = _load_mesh()
    nv = len(pts)
    ntri = len(fvi) // 3
    dense_v = RES * RES
    dense_t = (RES - 1) * (RES - 1) * 2
    print(f"\nadaptive: {nv} verts / {ntri} tris "
          f"(dense {dense_v} / {dense_t})")

    assert ntri > 1000, "adaptive mesh should still be a real mesh"
    assert nv < dense_v * 0.8, (
        f"expected reduction, got {nv} of dense {dense_v}")

    # World bounds preserved (Size 100 centered on origin).
    assert abs(pts[:, 0].min() + 50.0) < 1e-3
    assert abs(pts[:, 0].max() - 50.0) < 1e-3
    assert abs(pts[:, 2].min() + 50.0) < 1e-3
    assert abs(pts[:, 2].max() - 50.0) < 1e-3

    # ---- watertightness (position-welded) ----
    # Tiles emit their own copies of shared seam vertices (identical
    # positions, distinct ids -- by design). Weld by position so seam
    # edges are recognized as shared; a REAL crack would survive welding
    # as two edges with count 1 whose endpoints are NOT both on the rim.
    key = np.round(pts * 1e6).astype(np.int64)
    _, first_idx, inverse = np.unique(
        key, axis=0, return_index=True, return_inverse=True)
    pts = pts[first_idx]
    nv = len(pts)
    print(f"welded: {nv} unique positions")
    tris = inverse[fvi.reshape(-1, 3)]
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    directed = np.concatenate([
        np.stack([a, b], axis=1),
        np.stack([b, c], axis=1),
        np.stack([c, a], axis=1),
    ])
    lo = np.minimum(directed[:, 0], directed[:, 1])
    hi = np.maximum(directed[:, 0], directed[:, 1])

    # Every directed edge exactly once (consistent winding).
    _, d_counts = np.unique(directed, axis=0, return_counts=True)
    assert d_counts.max() == 1, "directed edge reused: winding inconsistent"

    u_keys, u_counts = np.unique(lo * nv + hi, return_counts=True)
    assert u_counts.max() <= 2, "edge shared by >2 triangles (non-manifold)"

    # count==1 edges must lie on the field rim: BOTH endpoints on one of
    # the four boundary planes x/z == +-50.
    vert_rim = (
        (np.abs(pts[:, 0] - 50.0) < 1e-4)
        | (np.abs(pts[:, 0] + 50.0) < 1e-4)
        | (np.abs(pts[:, 2] - 50.0) < 1e-4)
        | (np.abs(pts[:, 2] + 50.0) < 1e-4))
    single = u_keys[u_counts == 1]
    edge_rim = vert_rim[single % nv] & vert_rim[single // nv]
    assert edge_rim.all(), (
        f"{int((~edge_rim).sum())} non-rim edges with a single triangle: "
        "cracks")


def test_adaptive_bake_chain_writes_textures():
    for p in (ALBEDO, NORMAL):
        if os.path.exists(p):
            os.unlink(p)
    _run_chain("adabake", {"Detail": 0.4, "Max Subdiv": 4}, with_bake=True)
    for p in (ALBEDO, NORMAL):
        assert os.path.exists(p) and os.path.getsize(p) > 1000, p


if __name__ == "__main__":
    test_adaptive_mesh_watertight_and_reduced()
    test_adaptive_bake_chain_writes_textures()
    print("adaptive mesh tests passed")
