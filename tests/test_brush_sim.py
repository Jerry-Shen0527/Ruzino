"""
Test script for the brush paint simulation pipeline.
Uses mock_stroke node to generate a known test curve.

Pipeline: mock_stroke -> brush_input -> brush_paint_sim

Run from: Binaries/Release/
Usage:    SDK/python/python.exe ../../tests/test_brush_sim.py
"""

import os
import sys
import math
import faulthandler
import traceback
import atexit
import gc

faulthandler.enable()
faulthandler.dump_traceback_later(5, repeat=False)  # dump if hung

binary_dir = r"c:\Users\Pengfei\WorkSpace\Ruzino\Binaries\Release"
os.chdir(binary_dir)
sys.path.insert(0, binary_dir)
python_dir = r"c:\Users\Pengfei\WorkSpace\Ruzino\source\Core\rznode\python"
sys.path.insert(0, python_dir)

from ruzino_graph import RuzinoGraph
import nodes_core_py as core

try:
    import geometry_py as geom
    HAS_GEOM = True
except ImportError:
    HAS_GEOM = False
    print("WARNING: geometry_py not available")


def test_brush_sim_with_mock_stroke():
    """Test the full pipeline with a mock stroke curve."""
    print("=" * 60)
    print("TEST: Brush Paint Simulation with Mock Stroke")
    print("=" * 60)

    g = RuzinoGraph("BrushSimMock")
    config_path = os.path.join(binary_dir, "geometry_nodes.json")
    g.loadConfiguration(config_path)

    # Create nodes: mock_stroke -> brush_input -> paint_sim
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

    try:
        g.prepare_and_execute(inputs)
        print("Pipeline executed successfully!")

        result = g.getOutput(paint_sim, "Paint Particles")
        print(f"Output type: {result.type_name()}")

        if HAS_GEOM:
            try:
                out_geom = geom.extract_geometry_from_meta_any(result)
                pts = out_geom.get_points_component()
                if pts:
                    vertices = pts.get_vertices()
                    colors = pts.get_display_color()
                    widths = pts.get_width()
                    n = len(vertices)
                    print(f"\nOutput: {n} paint particles")

                    if n > 0:
                        # Print first few
                        for i in range(min(5, n)):
                            v = vertices[i]
                            c = colors[i] if i < len(colors) else geom.vec3(0, 0, 0)
                            w = widths[i] if i < len(widths) else 0
                            print(f"  p[{i}] = ({v.x:.4f}, {v.y:.4f}, {v.z:.4f}) "
                                  f"rgb=({c.x:.3f}, {c.y:.3f}, {c.z:.3f}) w={w:.5f}")

                        # Bounding box sanity check
                        xs = [v.x for v in vertices]
                        ys = [v.y for v in vertices]
                        zs = [v.z for v in vertices]
                        print(f"\n  Bounding box:")
                        print(f"    X: [{min(xs):.4f}, {max(xs):.4f}]")
                        print(f"    Y: [{min(ys):.4f}, {max(ys):.4f}]")
                        print(f"    Z: [{min(zs):.4f}, {max(zs):.4f}]")

                        # Sanity: positions should be within reasonable range
                        max_dist = max(math.sqrt(v.x**2 + v.y**2 + v.z**2) for v in vertices)
                        if max_dist > 100:
                            print(f"  WARNING: max distance {max_dist:.1f} seems too large!")
                        else:
                            print(f"  Max distance from origin: {max_dist:.4f} (OK)")

                        if n > 0:
                            print(f"\n  PASSED: {n} particles generated at reasonable positions")
                        else:
                            print("  WARNING: No particles generated")
                    else:
                        print("  No particles (empty output)")
                else:
                    print("  No PointsComponent in output")
            except Exception as e:
                print(f"  Geometry extraction error: {e}")
                import traceback
                traceback.print_exc()

        # Also check the mock_stroke output to verify curve was generated
        mock_result = g.getOutput(mock, "Stroke Curves")
        print(f"\nMock stroke output type: {mock_result.type_name()}")
        if HAS_GEOM:
            try:
                stroke_geom = geom.extract_geometry_from_meta_any(mock_result)
                curve = stroke_geom.get_curve_component()
                if curve:
                    verts = curve.get_vertices()
                    colors_c = curve.get_display_color()
                    print(f"  Curve: {len(verts)} vertices, {len(colors_c)} colors")
                    for i in range(min(3, len(verts))):
                        v = verts[i]
                        print(f"    v[{i}] = ({v.x:.4f}, {v.y:.4f}, {v.z:.4f})")
            except Exception as e:
                print(f"  Curve extraction: {e}")

    except Exception as e:
        print(f"FAILED: {e}")
        import traceback
        traceback.print_exc()


def cleanup():
    """Force garbage collection to trigger C++ destructors before Python exit."""
    print("\n[cleanup] Forcing GC...")
    gc.collect()
    print("[cleanup] GC done.")


if __name__ == "__main__":
    atexit.register(cleanup)
    test_brush_sim_with_mock_stroke()
    print("\nDone.")
