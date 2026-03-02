"""
TreeGen Export Test
Export generated trees to USD for visualization
"""

import os
import sys

test_dir = os.path.dirname(os.path.abspath(__file__))
binary_dir = os.path.join(test_dir, "..", "..", "..", "..", "Binaries", "Release")
binary_dir = os.path.abspath(binary_dir)
sys.path.insert(0, binary_dir)

rznode_python = os.path.join(test_dir, "..", "..", "..", "Core", "rznode", "python")
sys.path.insert(0, os.path.abspath(rznode_python))
os.chdir(binary_dir)

from ruzino_graph import RuzinoGraph
import stage_py
import geometry_py as geom


def get_modifier_file_path(usd_file):
    """Get the modifier file path for a USD file."""
    dir_path = os.path.dirname(usd_file)
    filename = os.path.basename(usd_file)
    stem = os.path.splitext(filename)[0]
    ext = os.path.splitext(filename)[1]
    return os.path.join(dir_path, stem + "_modifiers" + ext)


def check_usd_file_size(output_file, min_size=1000):
    """Check if USD file (or its modifier file) has at least min_size bytes."""
    if not os.path.exists(output_file):
        return False, 0

    file_size = os.path.getsize(output_file)
    modifier_file = get_modifier_file_path(output_file)

    if os.path.exists(modifier_file):
        modifier_size = os.path.getsize(modifier_file)
        if modifier_size >= min_size:
            return True, modifier_size
        return False, modifier_size

    if file_size >= min_size:
        return True, file_size
    return False, file_size


def test_export_tree_to_usd():
    """Generate a tree and export to USD"""
    print("\n" + "=" * 70)
    print("TEST: Export Tree to USD")
    print("=" * 70)

    output_dir = os.path.join(binary_dir, "tree_output")
    os.makedirs(output_dir, exist_ok=True)

    output_file = os.path.join(output_dir, "procedural_tree.usdc")

    g = RuzinoGraph("TreeExportTest")

    # Load geometry nodes first
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    print(f"✓ Loaded geometry nodes configuration")

    # Load TreeGen nodes
    g.loadConfiguration(
        os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json")
    )
    print(f"✓ Loaded TreeGen configuration")

    # Create nodes
    tree = g.createNode("tree_generate", name="procedural_tree")
    to_mesh = g.createNode("tree_to_mesh", name="mesh_converter")
    write_branches = g.createNode("write_usd", name="writer_branches")
    write_leaves = g.createNode("write_usd", name="writer_leaves")

    # Connect nodes
    g.addEdge(tree, "Tree Branches", to_mesh, "Tree Branches")
    g.addEdge(tree, "Leaves", to_mesh, "Leaves")
    g.addEdge(to_mesh, "Branch Mesh", write_branches, "Geometry")
    g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")

    # Set parameters for a nice looking tree
    inputs = {
        (tree, "Growth Years"): 6,
        (tree, "Random Seed"): 42,
        (tree, "Apical Angle Variance"): 35.0,
        (tree, "Lateral Buds"): 4,
        (tree, "Branch Angle"): 50.0,
        (tree, "Growth Rate"): 2.8,
        (tree, "Internode Length"): 0.4,
        (tree, "Apical Control"): 2.2,
        (tree, "Apical Dominance"): 1.2,
        (tree, "Light Factor"): 0.65,
        (tree, "Phototropism"): 0.25,
        (tree, "Gravitropism"): 0.15,
        (tree, "Generate Leaves"): True,
        (tree, "Terminal Leaves Only"): True,
        (tree, "Leaf Terminal Levels"): 3,
        (tree, "Leaves Per Internode"): 4,
        (tree, "Leaf Size"): 0.2,
        (tree, "Leaf Aspect Ratio"): 2.5,
        (tree, "Leaf Inclination"): 40.0,
        (tree, "Leaf Phototropism"): 0.6,
        (tree, "Leaf Curvature"): 0.25,
        (to_mesh, "Radial Segments"): 8,
        (write_branches, "Sub Path"): "branches",
        (write_leaves, "Sub Path"): "leaves",
    }

    print("\n🌱 Growing procedural tree...")

    # Create Stage and convert to GeomPayload
    stage = stage_py.Stage(output_file)

    # IMPORTANT: Create def Mesh prims in root layer first
    # This ensures the modifier layer can use "over" to override them
    pxr_stage = stage.get_pxr_stage()
    from pxr import UsdGeom

    UsdGeom.Mesh.Define(pxr_stage, "/tree")
    UsdGeom.Mesh.Define(pxr_stage, "/tree/branches")
    UsdGeom.Mesh.Define(pxr_stage, "/tree/leaves")
    print(f"✓ Created def Mesh prims in root layer")

    geom_payload = stage_py.create_payload_from_stage(stage, "/tree")
    g.setGlobalParams(geom_payload)

    # Execute both outputs
    g.prepare_and_execute(inputs, required_node=write_branches)
    g.prepare_and_execute(inputs, required_node=write_leaves)
    print(
        f"✓ Executed graph with branches at /tree/branches and leaves at /tree/leaves"
    )

    # Save the stage
    stage.save()

    # Check file size
    if os.path.exists(output_file):
        success, size = check_usd_file_size(output_file)
        file_size_kb = size / 1024
        print(f"\n📊 Export Statistics:")
        print(f"  File size: {size:,} bytes ({file_size_kb:.1f} KB)")
        print(f"  Output: {output_file}")

        print(f"\n💡 You can open this file in:")
        print(f"   - Houdini (File > Import > USD)")
        print(f"   - usdview (usdview {output_file})")
        print(f"   - Blender with USD plugin")

        print("\n✅ Successfully exported tree to USD!")

        assert success, f"File too small: {size} bytes"
    else:
        print(f"✗ USD file not found: {output_file}")
        assert False, f"File not created: {output_file}"


if __name__ == "__main__":
    try:
        test_export_tree_to_usd()

        print("\n" + "=" * 70)
        print("  🌳 TREE EXPORT TEST PASSED! 🌳")
        print("=" * 70)

    except Exception as e:
        print(f"\n✗ TEST FAILED: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)
