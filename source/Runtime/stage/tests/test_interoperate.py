"""
Test USD interoperability between pxr (Boost.Python) and stage_py (nanobind)

This demonstrates:
1. Creating Stage with stage_py and using it with node graph
2. Creating stage with pxr and converting to GeomPayload for node graph
"""

import sys
import os

# Setup paths
binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'Binaries', 'Release'))
rznode_python = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', 'Core', 'rznode', 'python'))

sys.path.insert(0, binary_dir)
sys.path.insert(0, rznode_python)
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir

from pxr import Usd, UsdGeom, Sdf
import stage_py
from ruzino_graph import RuzinoGraph


def test_stage_to_node_graph():
    """
    Test: Create Stage with stage_py, use it with node graph
    """
    print("\n" + "="*70)
    print("TEST: stage_py.Stage -> Node Graph")
    print("="*70)

    # Create a stage with stage_py
    print("\n1. Creating Stage with stage_py...")
    stage = stage_py.Stage("test_stage.usdc")
    print(f"   ✓ Created stage: {stage}")

    # Convert to GeomPayload
    print("\n2. Converting to GeomPayload...")
    payload = stage_py.create_payload_from_stage(stage, "/geometry")
    print(f"   ✓ Created payload with prim path: /geometry")

    # Use with node graph
    print("\n3. Using with node graph...")
    g = RuzinoGraph("StageTest")
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))

    grid = g.createNode("create_grid", name="grid")
    write = g.createNode("write_usd", name="write")
    g.addEdge(grid, "Geometry", write, "Geometry")

    inputs = {(grid, "resolution"): 5, (grid, "size"): 2.0}

    g.setGlobalParams(payload)
    g.prepare_and_execute(inputs, required_node=write)

    print("   ✓ Executed node graph")

    # Save using stage_py
    print("\n4. Saving stage...")
    stage.save()
    print(f"   ✓ Saved to test_stage.usdc")

    # Verify with pxr
    print("\n5. Verifying with pxr...")
    verify_stage = Usd.Stage.Open("test_stage.usdc")
    prim = verify_stage.GetPrimAtPath("/geometry")
    if prim.IsValid():
        mesh = UsdGeom.Mesh(prim)
        points = mesh.GetPointsAttr().Get(0)
        print(f"   ✓ Found mesh with {len(points) if points else 0} vertices")

    print("\n✅ TEST PASSED: stage_py.Stage works with node graph!")


def test_pxr_to_payload():
    """
    Test: Create stage with pxr, convert to GeomPayload using bridge API

    This tests the TRUE interop - Boost.Python to nanobind!
    """
    print("\n" + "="*70)
    print("TEST: pxr.Usd.Stage -> GeomPayload (TRUE INTEROP)")
    print("="*70)

    # Create stage with pxr
    print("\n1. Creating stage with pxr (Boost.Python)...")
    pxr_stage = Usd.Stage.CreateNew("test_pxr.usdc")
    print(f"   ✓ Created pxr stage: {pxr_stage}")

    # THE MAGIC: Convert pxr stage to GeomPayload using bridge
    print("\n2. Converting pxr stage to GeomPayload...")
    try:
        payload = stage_py.create_payload_from_pxr_stage(pxr_stage, "/geom")
        print(f"   ✓ Created GeomPayload from pxr stage!")

        # Use with node graph
        print("\n3. Using with node graph...")
        g = RuzinoGraph("PxrTest")
        g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))

        sphere = g.createNode("create_uv_sphere", name="sphere")
        write = g.createNode("write_usd", name="write")
        g.addEdge(sphere, "Geometry", write, "Geometry")

        inputs = {(sphere, "segments"): 16, (sphere, "rings"): 8, (sphere, "radius"): 1.0}

        g.setGlobalParams(payload)
        g.prepare_and_execute(inputs, required_node=write)

        print("   ✓ Executed node graph!")

        # Save using pxr
        print("\n4. Saving with pxr...")
        pxr_stage.Save()
        print("   ✓ Saved!")

        # Verify
        print("\n5. Verifying...")
        verify_stage = Usd.Stage.Open("test_pxr.usdc")
        prim = verify_stage.GetPrimAtPath("/geom")
        if prim.IsValid():
            mesh = UsdGeom.Mesh(prim)
            points = mesh.GetPointsAttr().Get(0)
            print(f"   ✓ Found mesh with {len(points) if points else 0} vertices")

        print("\n✅ TEST PASSED: TRUE INTEROP WORKS! 🎉")

    except Exception as e:
        print(f"   ✗ Failed: {e}")
        print("\n⚠️  Note: Bridge API might not be fully implemented yet")
        raise


def test_payload_to_pxr_stage():
    """
    Test: Extract pxr.Usd.Stage from GeomPayload
    """
    print("\n" + "="*70)
    print("TEST: GeomPayload -> pxr.Usd.Stage")
    print("="*70)

    # Create stage with stage_py
    print("\n1. Creating Stage with stage_py...")
    stage = stage_py.Stage("test_extract.usdc")
    payload = stage_py.create_payload_from_stage(stage, "/test")

    # Add some geometry through node graph
    print("\n2. Adding geometry via node graph...")
    g = RuzinoGraph("ExtractTest")
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))

    cube = g.createNode("create_cube", name="cube")
    write = g.createNode("write_usd", name="write")
    g.addEdge(cube, "Geometry", write, "Geometry")

    g.setGlobalParams(payload)
    g.prepare_and_execute({}, required_node=write)

    print("   ✓ Added geometry")

    # Extract pxr stage from payload
    print("\n3. Extracting pxr.Usd.Stage from GeomPayload...")
    try:
        pxr_stage = stage_py.get_pxr_stage_from_payload(payload)
        print(f"   ✓ Got pxr stage: {pxr_stage}")

        # Use pxr API to inspect
        print("\n4. Using pxr API to inspect...")
        prim = pxr_stage.GetPrimAtPath("/test")
        if prim.IsValid():
            print(f"   ✓ Found prim at /test")

        # Save with pxr
        pxr_stage.Save()
        print("   ✓ Saved with pxr API")

        print("\n✅ TEST PASSED: Can extract pxr stage from payload!")

    except Exception as e:
        print(f"   ✗ Failed: {e}")
        print("\n⚠️  Note: Extraction API might not be fully implemented yet")
        raise


if __name__ == "__main__":
    print("\n" + "="*70)
    print("  USD Interoperability Tests")
    print("  stage_py (nanobind) ↔ pxr (Boost.Python)")
    print("="*70)

    os.chdir(binary_dir)

    try:
        test_stage_to_node_graph()
        test_pxr_to_payload()
        test_payload_to_pxr_stage()

        print("\n" + "="*70)
        print("  ALL TESTS PASSED! 🎉")
        print("="*70)
        print("✅ stage_py.Stage -> Node Graph: WORKS")
        print("✅ pxr.Usd.Stage -> GeomPayload: WORKS")
        print("✅ GeomPayload -> pxr.Usd.Stage: WORKS")
        print("\n💡 Full interoperability achieved between pxr and stage_py!")

    except Exception as e:
        print(f"\n❌ TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
