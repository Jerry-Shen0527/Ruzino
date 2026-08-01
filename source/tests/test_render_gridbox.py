"""
Build a path-tracing render graph from Python and verify it.

The Ruzino renderer (rz_render.exe / headless_render.exe) renders a USD scene
using a *render node graph* (rng_texture -> ray_generation -> path_tracing
-> accumulate -> gamma_correction -> present_color). That graph is normally
authored by hand; this test builds it purely from the Python API, serializes
it, and asserts the serialized graph round-trips with the expected pipeline.

The actual GPU render is environment-dependent (needs an OpenGL 4.5 context;
the headless sandbox here only exposes a software GL that Hydra rejects with
"HgiGL minimum OpenGL requirements not met"). So this test validates the
graph-construction half of the pipeline; see RENDERING below for invoking the
exe on a machine with a real GPU.
"""

import os
from pathlib import Path

import pytest

from conftest import TEST_OUTPUT_DIR

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data"
# Test output lives under Binaries/Release/test_output/ (never the source tree).
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "render_gridbox"

# Render-node id_names (must match render_nodes.json registration).
PIPELINE = [
    ("rng_texture", "Random Number", "node_render_ray_generation", "random seeds"),
    ("node_render_ray_generation", "Rays", "path_tracing", "Rays"),
    ("path_tracing", "Output", "accumulate", "Texture"),
    ("accumulate", "Accumulated", "gamma_correction", "Texture"),
    ("gamma_correction", "Corrected", "present_color", "Color"),
]


def _build_render_graph():
    from ruzino_render_graph import RuzinoRenderGraph

    g = RuzinoRenderGraph("GridBoxRender")
    g.loadConfiguration(str(BINARY_DIR / "render_nodes.json"))

    rng = g.createNode("rng_texture", name="RNG")
    ray_gen = g.createNode("node_render_ray_generation", name="RayGen")
    path_trace = g.createNode("path_tracing", name="PathTrace")
    accumulate = g.createNode("accumulate", name="Accumulate")
    gamma = g.createNode("gamma_correction", name="Gamma")
    present = g.createNode("present_color", name="Present")

    g.addEdge(rng, "Random Number", ray_gen, "random seeds")
    g.addEdge(ray_gen, "Rays", path_trace, "Rays")
    g.addEdge(path_trace, "Output", accumulate, "Texture")
    g.addEdge(accumulate, "Accumulated", gamma, "Texture")
    g.addEdge(gamma, "Corrected", present, "Color")
    g.markOutput(present, "Color")
    return g, present


def test_render_graph_builds():
    """The path-tracing render graph builds from Python with the full pipeline."""
    g, present = _build_render_graph()

    types = [n.name for n in g.nodes]
    for needed in ("RNG", "RayGen", "PathTrace", "Accumulate", "Gamma", "Present"):
        assert needed in types, f"missing render node {needed}: {types}"
    assert len(g.links) == len(PIPELINE), (
        f"expected {len(PIPELINE)} links, got {len(g.links)}")


def test_render_graph_serializes():
    """The render graph serializes and round-trips with its pipeline intact."""
    import json
    g, present = _build_render_graph()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_json = OUTPUT_DIR / "gridbox_render_graph.json"
    out_json.write_text(g.serialize(), encoding="utf-8")

    data = json.loads(out_json.read_text(encoding="utf-8"))
    assert "nodes_info" in data and "links_info" in data
    id_names = {n["id_name"] for n in data["nodes_info"].values()}
    for expected in ("rng_texture", "node_render_ray_generation",
                     "path_tracing", "accumulate", "gamma_correction",
                     "present_color"):
        assert expected in id_names, f"missing node type {expected} in serialized graph"
    assert len(data["links_info"]) == len(PIPELINE)
    print(f"  render graph: {len(id_names)} node types, "
          f"{len(data['links_info'])} links -> {out_json.name}")
