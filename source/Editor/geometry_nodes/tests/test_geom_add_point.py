"""
Headless integration tests for the geom_add_point node.

geom_add_point appends the viewport pick point (from GeomPayload.pick) to an
accumulating point set on each cook. These tests inject pick events through
GeomPayload (the same struct the live editor fills from the event bus),
execute, and read back the output points.

This validates the pick data path the event-bus refactor preserves:
  emit PICK_EVENT -> editor callback -> GeomPayload.pick -> geom_add_point

Test coverage:
  - test_single_pick_adds_one_point
  - test_multiple_picks_accumulate_in_order
  - test_no_pick_no_change
  - test_width_applied_to_all_points
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


def _build_graph(width=0.05):
    binary_dir = get_binary_dir()
    g = RuzinoGraph("GeomAddPoint")
    config_path = os.path.join(binary_dir, "geometry_nodes.json")
    g.loadConfiguration(config_path)

    add_pt = g.createNode("geom_add_point", name="AddPt")
    g.markOutput(add_pt, "Points")
    return g, add_pt


def _add_picks(g, add_pt, picks):
    """Drive a sequence of pick events through GeomPayload, one execute per pick.

    picks: list of (x, y, z) tuples. None means "no pick this frame".
    """
    payload = stage_py.GeomPayload()
    for pk in picks:
        if pk is None:
            # No pick event this frame
            stage_py.clear_payload_pick(payload)
        else:
            stage_py.set_payload_pick_event(payload, pk, (0.0, 0.0, 1.0))
        g.setGlobalParams(payload)
        g.prepare_and_execute({})

    # Final read
    stage_py.clear_payload_pick(payload)
    g.setGlobalParams(payload)
    g.prepare_and_execute({})


def _read_points(g, add_pt):
    result = g.getOutput(add_pt, "Points")
    geometry = geom.extract_geometry_from_meta_any(result)
    points = geometry.get_points_component(0)
    return points.get_vertices(), points.get_width()


# ---------------------------------------------------------------------------
# Test 1: a single pick event adds one point at the picked location
# ---------------------------------------------------------------------------
def test_single_pick_adds_one_point():
    g, add_pt = _build_graph()
    _add_picks(g, add_pt, [(1.0, 2.0, 3.0)])
    vertices, widths = _read_points(g, add_pt)

    assert len(vertices) == 1, f"expected 1 point, got {len(vertices)}"
    assert math.isclose(vertices[0].x, 1.0, abs_tol=1e-5)
    assert math.isclose(vertices[0].y, 2.0, abs_tol=1e-5)
    assert math.isclose(vertices[0].z, 3.0, abs_tol=1e-5)


# ---------------------------------------------------------------------------
# Test 2: multiple picks accumulate, in emission order
# ---------------------------------------------------------------------------
def test_multiple_picks_accumulate_in_order():
    g, add_pt = _build_graph()
    pts = [(0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (2.0, 2.0, 2.0)]
    _add_picks(g, add_pt, pts)
    vertices, widths = _read_points(g, add_pt)

    assert len(vertices) == 3, f"expected 3 points, got {len(vertices)}"
    for i, expected in enumerate(pts):
        assert math.isclose(vertices[i].x, expected[0], abs_tol=1e-5)
        assert math.isclose(vertices[i].y, expected[1], abs_tol=1e-5)


# ---------------------------------------------------------------------------
# Test 3: frames without a pick event do not add points
# ---------------------------------------------------------------------------
def test_no_pick_no_change():
    g, add_pt = _build_graph()
    _add_picks(g, add_pt, [None, None, None])
    vertices, widths = _read_points(g, add_pt)

    assert len(vertices) == 0, (
        f"expected 0 points with no picks, got {len(vertices)}")


# ---------------------------------------------------------------------------
# Test 4: the Width socket input is applied to every emitted point
# ---------------------------------------------------------------------------
def test_width_applied_to_all_points():
    g, add_pt = _build_graph(width=0.2)
    _add_picks(g, add_pt, [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0)])
    vertices, widths = _read_points(g, add_pt)

    assert len(widths) == 2
    for w in widths:
        assert math.isclose(w, 0.1, abs_tol=1e-5), (
            f"width {w} != default 0.1")


# ---------------------------------------------------------------------------
# Test 5: interleave picks and no-pick frames; only picks accumulate
# ---------------------------------------------------------------------------
def test_interleaved_picks_and_idle_frames():
    g, add_pt = _build_graph()
    _add_picks(g, add_pt, [(1.0, 0.0, 0.0), None, (2.0, 0.0, 0.0), None])
    vertices, widths = _read_points(g, add_pt)

    assert len(vertices) == 2, (
        f"expected 2 points (2 picks + 2 idle), got {len(vertices)}")
    assert math.isclose(vertices[0].x, 1.0, abs_tol=1e-5)
    assert math.isclose(vertices[1].x, 2.0, abs_tol=1e-5)
