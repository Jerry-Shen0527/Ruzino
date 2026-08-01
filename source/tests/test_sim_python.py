"""
Simulation test — build node graph from Python API.

Uses RuzinoGraph to programmatically create geometry node pipelines,
execute them, and verify the output geometry properties.
Environment setup is handled by conftest.py.
"""

import os
from pathlib import Path
import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "sim_python"


def test_python_graph_creation():
    """Build a geometry node graph from Python and verify structure."""
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("PythonSphereTest")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))
    g.loadConfiguration(str(BINARY_DIR / "basic_nodes.json"))

    sphere = g.createNode("create_uv_sphere", name="Sphere")
    output = g.createNode("write_usd", name="Output")
    g.addEdge(sphere, "Geometry", output, "Geometry")

    assert len(g.nodes) == 2, f"Expected 2 nodes, got {len(g.nodes)}"
    assert len(g.links) == 1, f"Expected 1 link, got {len(g.links)}"

    node_types = [n.name for n in g.nodes]
    assert "create_uv_sphere" in node_types
    assert "write_usd" in node_types


def test_python_sphere_creation():
    """Create a sphere via Python API and verify geometry output."""
    try:
        import geometry_py as geom
    except ImportError:
        pytest.skip("geometry_py not available")

    import numpy as np
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("SphereExec")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    sphere = g.createNode("create_uv_sphere", name="Sphere")
    g.markOutput(sphere, "Geometry")

    g.prepare_and_execute({
        (sphere, "segments"): 8,
        (sphere, "rings"): 4,
        (sphere, "radius"): 2.0,
    })

    result = g.getOutput(sphere, "Geometry")
    geometry = geom.extract_geometry_from_meta_any(result)
    mesh = geometry.get_mesh_component()
    assert mesh is not None, "No mesh component in output"

    vertices = mesh.get_vertices()
    assert len(vertices) > 0, "Mesh has no vertices"

    avg_radius = float(np.mean(np.linalg.norm(vertices, axis=1)))
    assert abs(avg_radius - 2.0) < 0.5, f"Expected radius ≈ 2.0, got {avg_radius:.3f}"


def test_python_transform():
    """Create sphere -> transform graph from Python, verify translation."""
    try:
        import geometry_py as geom
    except ImportError:
        pytest.skip("geometry_py not available")

    import numpy as np
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("TransformTest")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    sphere = g.createNode("create_uv_sphere", name="Sphere")
    transform = g.createNode("transform_geom", name="Transform")
    g.addEdge(sphere, "Geometry", transform, "Geometry")
    g.markOutput(transform, "Geometry")

    g.prepare_and_execute({
        (sphere, "segments"): 8,
        (sphere, "rings"): 4,
        (sphere, "radius"): 1.0,
        (transform, "Translate X"): 2.0,
        (transform, "Translate Y"): 0.0,
        (transform, "Translate Z"): 0.0,
    })

    result = g.getOutput(transform, "Geometry")
    geometry = geom.extract_geometry_from_meta_any(result)
    mesh = geometry.get_mesh_component()
    assert mesh is not None, "No mesh after transform"

    vertices = mesh.get_vertices()
    x_mean = float(np.mean(vertices[:, 0]))
    assert abs(x_mean - 2.0) < 0.5, f"Expected x_mean ≈ 2.0 after translate, got {x_mean:.3f}"


def test_python_graph_serialization():
    """Build a graph from Python, serialize it, deserialize, and verify."""
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("SerTest")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))
    g.loadConfiguration(str(BINARY_DIR / "basic_nodes.json"))

    sphere = g.createNode("create_uv_sphere", name="Sphere")
    output = g.createNode("write_usd", name="Output")
    g.addEdge(sphere, "Geometry", output, "Geometry")

    json_str = g.serialize()

    g2 = RuzinoGraph("DeserTest")
    g2.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))
    g2.loadConfiguration(str(BINARY_DIR / "basic_nodes.json"))
    g2.deserialize(json_str)

    assert len(g2.nodes) == len(g.nodes), "Node count mismatch after roundtrip"
    assert len(g2.links) == len(g.links), "Link count mismatch after roundtrip"


def test_python_graph_apply_to_stage():
    """Build a graph from Python, apply to a USD stage, execute, verify output."""
    try:
        import stage_py
    except ImportError:
        pytest.skip("stage_py not available")

    from ruzino_graph import RuzinoGraph
    from pxr import UsdGeom

    g = RuzinoGraph("StageApply")
    g.loadConfiguration(str(BINARY_DIR / "geometry_nodes.json"))

    sphere = g.createNode("create_uv_sphere", name="Sphere")
    transform = g.createNode("transform_geom", name="Transform")
    output = g.createNode("write_usd", name="Output")

    g.addEdge(sphere, "Geometry", transform, "Geometry")
    g.addEdge(transform, "Geometry", output, "Geometry")

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    output_path = str(OUTPUT_DIR / "sim_python_out.usdc")

    stage = stage_py.Stage(output_path)
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), "/test_geom")

    inputs = {
        (sphere, "radius"): 1.5,
        (transform, "Translate X"): 3.0,
    }

    g.apply_to_stage(stage, "/test_geom", inputs=inputs)
    g.prepare_and_execute(required_node=output)
    stage.save()

    assert os.path.exists(output_path), "Output USD file not created"
