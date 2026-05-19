"""
Simulation test — embedded node_json on USD prim.

Loads sim_transform_embedded.usda which has a node_json attribute
containing a transform_geom pipeline graph.
Environment setup is handled by conftest.py.
"""

import os
import json
from pathlib import Path
import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = DATA_DIR / "output"


def test_embedded_node_json_loads():
    """Verify the embedded node_json can be read from USD and parsed."""
    from pxr import Usd

    scene_path = DATA_DIR / "scenes" / "sim_transform_embedded.usda"
    if not scene_path.exists():
        pytest.skip("sim_transform_embedded.usda not found")

    stage = Usd.Stage.Open(str(scene_path))
    prim = stage.GetPrimAtPath("/Sphere")
    assert prim.IsValid(), "Sphere prim not found"

    attr = prim.GetAttribute("node_json")
    assert attr.IsValid(), "node_json attribute not found"

    node_json_str = attr.Get()
    assert node_json_str and len(node_json_str) > 0, "node_json is empty"

    graph = json.loads(node_json_str)
    assert "nodes_info" in graph
    assert "links_info" in graph

    node_types = {n["id_name"] for n in graph["nodes_info"].values()}
    assert "transform_geom" in node_types, f"Expected transform_geom, got: {node_types}"


def test_embedded_graph_deserializes():
    """Deserialize the embedded graph into a RuzinoGraph and verify structure."""
    from ruzino_graph import RuzinoGraph
    from pxr import Usd

    scene_path = DATA_DIR / "scenes" / "sim_transform_embedded.usda"
    stage = Usd.Stage.Open(str(scene_path))
    node_json = stage.GetPrimAtPath("/Sphere").GetAttribute("node_json").Get()

    g = RuzinoGraph("EmbeddedTest")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))
    g.loadConfiguration(str(BINARY_DIR / "basic_nodes.json"))
    g.deserialize(node_json)

    node_types = [n.name for n in g.nodes]
    assert "transform_geom" in node_types, f"transform_geom not found in: {node_types}"
    assert len(g.links) > 0, "No links in deserialized graph"


def test_embedded_graph_executes():
    """Execute the embedded graph and verify it produces output."""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    from ruzino_graph import RuzinoGraph
    from pxr import Usd, UsdGeom

    scene_path = DATA_DIR / "scenes" / "sim_transform_embedded.usda"
    stage = Usd.Stage.Open(str(scene_path))
    node_json = stage.GetPrimAtPath("/Sphere").GetAttribute("node_json").Get()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_path = str(OUTPUT_DIR / "sim_embedded_out.usdc")

    g = RuzinoGraph("EmbeddedExec")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))
    g.loadConfiguration(str(BINARY_DIR / "basic_nodes.json"))
    g.deserialize(node_json)

    output_stage = stage_py.Stage(output_path)
    UsdGeom.Mesh.Define(output_stage.get_pxr_stage(), "/Sphere")

    geom_payload = stage_py.create_payload_from_stage(output_stage, "/Sphere")
    g.setGlobalParams(geom_payload)

    write_node = next((n for n in g.nodes if n.name == "write_usd"), None)
    g.prepare_and_execute(required_node=write_node)
    output_stage.save()

    assert os.path.exists(output_path), "Output USD file not created"


def test_embedded_transform_offset():
    """Verify transform_geom shifted geometry by the expected amount."""
    try:
        import stage_py
        import geometry_py as geom
    except ImportError:
        pytest.skip("stage_py or geometry_py not available")

    import numpy as np
    from ruzino_graph import RuzinoGraph
    from pxr import Usd, UsdGeom

    scene_path = DATA_DIR / "scenes" / "sim_transform_embedded.usda"
    stage = Usd.Stage.Open(str(scene_path))
    node_json = stage.GetPrimAtPath("/Sphere").GetAttribute("node_json").Get()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_path = str(OUTPUT_DIR / "sim_embedded_verify.usdc")

    g = RuzinoGraph("EmbeddedVerify")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))
    g.loadConfiguration(str(BINARY_DIR / "basic_nodes.json"))
    g.deserialize(node_json)

    output_stage = stage_py.Stage(output_path)
    UsdGeom.Mesh.Define(output_stage.get_pxr_stage(), "/Sphere")
    geom_payload = stage_py.create_payload_from_stage(output_stage, "/Sphere")
    g.setGlobalParams(geom_payload)

    write_node = next(n for n in g.nodes if n.name == "write_usd")
    g.prepare_and_execute(required_node=write_node)

    result = g.getOutput(write_node, "Geometry")
    geometry = geom.extract_geometry_from_meta_any(result)
    mesh = geometry.get_mesh_component()
    assert mesh is not None, "No mesh in output geometry"

    vertices = mesh.get_vertices()
    x_mean = float(np.mean(vertices[:, 0]))

    # transform_geom has Translate X = 1.0
    assert abs(x_mean - 1.0) < 0.5, f"Expected x_mean ≈ 1.0, got {x_mean:.3f}"
