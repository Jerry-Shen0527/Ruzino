"""
Test the brush paint simulation pipeline (headless).
Pipeline: mock_stroke -> brush_input -> brush_paint_sim

Validates:
  - Pipeline executes without errors
  - Paint particles are generated with reasonable positions
  - RYB->RGB color mapping produces non-trivial colors
"""

import os
import math
import pytest

from ruzino_graph import RuzinoGraph
import nodes_core_py as core
import geometry_py as geom


def get_binary_dir():
    return os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "Binaries", "Release")
    )


def test_brush_sim_generates_particles():
    """Full pipeline: mock_stroke -> brush_input -> brush_paint_sim"""
    binary_dir = get_binary_dir()

    g = RuzinoGraph("BrushSimTest")
    config_path = os.path.join(binary_dir, "geometry_nodes.json")
    g.loadConfiguration(config_path)

    # Create node chain
    mock = g.createNode("mock_stroke", name="MockStroke")
    brush_in = g.createNode("brush_input", name="BrushInput")
    paint_sim = g.createNode("brush_paint_sim", name="PaintSim")

    g.addEdge(mock, "Stroke Curves", brush_in, "Stroke Curves")
    g.addEdge(brush_in, "Brush Stroke", paint_sim, "Brush Strokes")

    inputs = {
        (mock, "Num Points"): 30,
        (mock, "Amplitude"): 0.05,
        (mock, "Length"): 0.3,
        (mock, "Ink R (RYB)"): 1.0,
        (mock, "Ink Y (RYB)"): 0.0,
        (mock, "Ink B (RYB)"): 0.0,
        (brush_in, "Brush Width"): 0.02,
        (brush_in, "Brush Pressure"): 1.0,
        (brush_in, "Ink Amount"): 0.8,
        (paint_sim, "Resolution"): 256,
        (paint_sim, "Paper Size"): 1.0,
        (paint_sim, "Brush Radius"): 0.02,
        (paint_sim, "Ink Amount"): 0.8,
        (paint_sim, "Viscosity"): 0.5,
        (paint_sim, "Diffusion Rate"): 0.0001,
        (paint_sim, "Drying Rate"): 0.1,
    }

    g.markOutput(paint_sim, "Paint Particles")
    g.prepare_and_execute(inputs)

    result = g.getOutput(paint_sim, "Paint Particles")
    out_geom = geom.extract_geometry_from_meta_any(result)
    pts = out_geom.get_points_component()

    assert pts is not None, "Output has no PointsComponent"

    vertices = pts.get_vertices()
    colors = pts.get_display_color()

    n = len(vertices)
    assert n > 0, "No paint particles generated"

    print(f"  Generated {n} paint particles")

    # Bounding box sanity check — particles should be near the stroke
    xs = [v.x for v in vertices]
    ys = [v.y for v in vertices]
    print(f"  Bounding X: [{min(xs):.4f}, {max(xs):.4f}]")
    print(f"  Bounding Y: [{min(ys):.4f}, {max(ys):.4f}]")

    # Stroke is ~0.3 long centered at 0, so particles shouldn't be far away
    max_dist = max(math.sqrt(v.x**2 + v.y**2 + v.z**2) for v in vertices)
    assert max_dist < 5.0, f"Max distance {max_dist:.1f} too large — particles may be unbounded"

    # At least some particles should have non-trivial color (not pure white)
    non_white = [c for c in colors if c.x < 0.95 or c.y < 0.95 or c.z < 0.95]
    print(f"  Non-white particles: {len(non_white)}/{n}")
    assert len(non_white) > 0, "All particles are white — color deposit may be broken"


def test_brush_sim_empty_input():
    """When no stroke data is provided, output should be empty particles."""
    binary_dir = get_binary_dir()

    g = RuzinoGraph("BrushSimEmpty")
    config_path = os.path.join(binary_dir, "geometry_nodes.json")
    g.loadConfiguration(config_path)

    mock = g.createNode("mock_stroke", name="MockStroke")
    paint_sim = g.createNode("brush_paint_sim", name="PaintSim")

    g.addEdge(mock, "Stroke Curves", paint_sim, "Brush Strokes")

    inputs = {
        (mock, "Num Points"): 0,
    }

    g.markOutput(paint_sim, "Paint Particles")
    g.prepare_and_execute(inputs)

    result = g.getOutput(paint_sim, "Paint Particles")
    out_geom = geom.extract_geometry_from_meta_any(result)
    pts = out_geom.get_points_component()

    # Either no points component or empty vertices is acceptable
    if pts is not None:
        vertices = pts.get_vertices()
        assert len(vertices) == 0, f"Expected 0 particles for empty input, got {len(vertices)}"
