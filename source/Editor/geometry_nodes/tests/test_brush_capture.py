"""
Headless integration tests for the brush_capture node.

brush_capture accumulates viewport brush points (delivered via GeomPayload)
into a stroke curve. These tests bypass the GUI entirely: they build a node
graph, inject brush state through GeomPayload (the same struct the live
editor's callback fills from the event bus), execute, and read back the
output curve.

This validates the data path that the event-bus refactor preserves:
  emit BRUSH_STATE -> editor callback -> GeomPayload.brush_* -> brush_capture

Test coverage:
  - test_single_point_capture:   one brush point -> curve has 1 vertex
  - test_multi_point_stroke:     several points in one stroke -> all captured,
                                 vert_count tracks the stroke
  - test_pen_up_finalizes_stroke: active=false after active points starts a
                                 new stroke segment
  - test_no_new_point_no_change: brush_new_point=False -> nothing appended
"""

import math
import os

import pytest

from ruzino_graph import RuzinoGraph
import geometry_py as geom
import stage_py


def get_binary_dir():
    return os.path.abspath(
        os.path.join(
            os.path.dirname(__file__),
            "..", "..", "..", "..", "Binaries", "Release"))


def _build_graph():
    """Create a graph with a single brush_capture node and mark its output."""
    binary_dir = get_binary_dir()
    g = RuzinoGraph("BrushCapture")
    config_path = os.path.join(binary_dir, "geometry_nodes.json")
    g.loadConfiguration(config_path)

    cap = g.createNode("brush_capture", name="Cap")
    g.markOutput(cap, "Stroke Curves")
    return g, cap


def _make_brush_payload(points, *, active=True, time_base=0.0):
    """Build a GeomPayload and drive a brush stroke through it.

    Executes the graph once per point (mimicking per-frame execution in the
    live editor). `points` is a list of (x, y, z) tuples. Each point is set
    as brush_new_point=True. Pass active=False to emit a pen-up event.
    """
    g, cap = _build_graph()
    payload = stage_py.GeomPayload()
    payload.brush_active = active

    for i, pt in enumerate(points):
        payload.brush_point = pt
        payload.brush_time = time_base + i * 0.01
        payload.brush_active = active
        payload.brush_new_point = True
        g.setGlobalParams(payload)
        g.prepare_and_execute({})

    # Final execute to read the accumulated curve
    payload.brush_new_point = False
    g.setGlobalParams(payload)
    g.prepare_and_execute({})
    return g, cap


def _read_curve(g, cap):
    """Extract the output curve geometry and return (vertices, vert_counts)."""
    result = g.getOutput(cap, "Stroke Curves")
    geometry = geom.extract_geometry_from_meta_any(result)
    curve = geometry.get_curve_component(0)
    vertices = curve.get_vertices()       # list of geom.vec3
    vert_counts = curve.get_vert_count()  # list of int (per-stroke lengths)
    return vertices, vert_counts


# ---------------------------------------------------------------------------
# Test 1: a single brush point produces a curve with one vertex
# ---------------------------------------------------------------------------
def test_single_point_capture():
    g, cap = _make_brush_payload([(1.0, 2.0, 3.0)])
    vertices, vert_counts = _read_curve(g, cap)

    assert len(vertices) == 1, f"expected 1 vertex, got {len(vertices)}"
    assert math.isclose(vertices[0].x, 1.0, abs_tol=1e-5)
    assert math.isclose(vertices[0].y, 2.0, abs_tol=1e-5)
    assert math.isclose(vertices[0].z, 3.0, abs_tol=1e-5)
    assert vert_counts == [1], f"expected vert_count [1], got {vert_counts}"


# ---------------------------------------------------------------------------
# Test 2: multiple points in one stroke are all captured, in order
# ---------------------------------------------------------------------------
def test_multi_point_stroke():
    pts = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)]
    g, cap = _make_brush_payload(pts)
    vertices, vert_counts = _read_curve(g, cap)

    assert len(vertices) == 4, f"expected 4 vertices, got {len(vertices)}"
    for i, expected in enumerate(pts):
        assert math.isclose(vertices[i].x, expected[0], abs_tol=1e-5)
        assert math.isclose(vertices[i].y, expected[1], abs_tol=1e-5)
    assert vert_counts == [4], f"expected vert_count [4], got {vert_counts}"


# ---------------------------------------------------------------------------
# Test 3: a pen-up event (active=False after active points) finalizes the
# current stroke; a subsequent active point starts a new stroke segment.
# ---------------------------------------------------------------------------
def test_pen_up_finalizes_stroke():
    g, cap = _build_graph()
    payload = stage_py.GeomPayload()

    # stroke 1: two active points
    for i, pt in enumerate([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0)]):
        payload.brush_point = pt
        payload.brush_time = i * 0.01
        payload.brush_active = True
        payload.brush_new_point = True
        g.setGlobalParams(payload)
        g.prepare_and_execute({})

    # pen up
    payload.brush_active = False
    payload.brush_new_point = True
    g.setGlobalParams(payload)
    g.prepare_and_execute({})

    # stroke 2: one active point
    payload.brush_point = (5.0, 5.0, 0.0)
    payload.brush_time = 0.5
    payload.brush_active = True
    payload.brush_new_point = True
    g.setGlobalParams(payload)
    g.prepare_and_execute({})

    payload.brush_new_point = False
    g.setGlobalParams(payload)
    g.prepare_and_execute({})

    vertices, vert_counts = _read_curve(g, cap)

    assert len(vertices) == 3, f"expected 3 total vertices, got {len(vertices)}"
    # Two stroke segments: first with 2 verts, second with 1
    assert sum(vert_counts) == 3
    assert len(vert_counts) == 2, (
        f"expected 2 stroke segments, got {len(vert_counts)}: {vert_counts}")
    assert vert_counts[0] == 2
    assert vert_counts[1] == 1


# ---------------------------------------------------------------------------
# Test 4: when brush_new_point is False, no new vertex is appended. This is
# the idle-between-events case — confirms the node doesn't hallucinate points.
# ---------------------------------------------------------------------------
def test_no_new_point_no_change():
    g, cap = _build_graph()
    payload = stage_py.GeomPayload()
    payload.brush_point = (1.0, 2.0, 3.0)
    payload.brush_active = True
    payload.brush_new_point = False  # no new point
    g.setGlobalParams(payload)
    g.prepare_and_execute({})
    g.prepare_and_execute({})  # execute twice to be sure

    vertices, vert_counts = _read_curve(g, cap)

    assert len(vertices) == 0, (
        f"expected 0 vertices with no new_point, got {len(vertices)}")
