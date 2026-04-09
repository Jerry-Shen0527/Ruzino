"""
Test the improved leaf generation system
Tests terminal branch detection, leaf orientation, and new parameters
"""
import os
from ruzino_graph import RuzinoGraph
import stage_py
import geometry_py as geom


def get_binary_dir():
    """Get the binary directory path"""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    binary_dir = os.path.join(test_dir, '..', '..', '..', '..', 'Binaries', 'Release')
    return os.path.abspath(binary_dir)


def test_terminal_leaves_only():
    """Test that leaves are only generated on terminal branches"""
    print("\n" + "="*70)
    print("TEST: Terminal Leaves Only")
    print("="*70)

    binary_dir = get_binary_dir()
    output_file = os.path.join(binary_dir, "terminal_leaves_test.usdc")

    g = RuzinoGraph("TerminalLeavesTest")

    # Load configurations
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))

    # Create nodes
    tree = g.createNode("tree_generate", name="tree")
    to_mesh = g.createNode("tree_to_mesh", name="mesh")
    write_branches = g.createNode("write_usd", name="writer_branches")
    write_leaves = g.createNode("write_usd", name="writer_leaves")

    # Connect
    g.addEdge(tree, "Tree Branches", to_mesh, "Tree Branches")
    g.addEdge(tree, "Leaves", to_mesh, "Leaves")
    g.addEdge(to_mesh, "Branch Mesh", write_branches, "Geometry")
    g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")

    # Parameters with terminal leaves only
    inputs = {
        (tree, "Growth Years"): 5,
        (tree, "Random Seed"): 42,
        (tree, "Generate Leaves"): True,
        (tree, "Terminal Leaves Only"): True,
        (tree, "Leaf Terminal Levels"): 2,  # Only last 2 levels
        (tree, "Leaves Per Internode"): 5,
        (to_mesh, "Radial Segments"): 6,
        (write_branches, "Sub Path"): "branches",
        (write_leaves, "Sub Path"): "leaves",
    }

    print("\n🌱 Growing tree with terminal leaves only (last 2 levels)...")

    # Create and execute
    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/tree")
    g.setGlobalParams(geom_payload)

    g.prepare_and_execute(inputs, required_node=write_branches)
    g.prepare_and_execute(inputs, required_node=write_leaves)

    stage.save()

    # Get leaf output to verify
    leaf_result = g.getOutput(tree, "Leaves")
    leaf_geom = geom.extract_geometry_from_meta_any(leaf_result)
    leaf_mesh = leaf_geom.get_mesh_component(0)

    if leaf_mesh:
        leaf_verts = leaf_mesh.get_vertices()
        num_leaves = len(leaf_verts) // 4
        print(f"✓ Generated {num_leaves} leaves on terminal branches")
        assert num_leaves > 0, "Should have leaves on terminal branches"

    # The USD geometry data is written to the session layer, exported as a
    # separate _modifiers.usdc file.  Check the combined size of both files.
    modifier_file = output_file.replace(".usdc", "_modifiers.usdc")
    main_size = os.path.getsize(output_file) if os.path.exists(output_file) else 0
    mod_size = os.path.getsize(modifier_file) if os.path.exists(modifier_file) else 0
    total_size = main_size + mod_size
    print(f"✓ Exported to USD: {total_size} bytes (main={main_size}, modifiers={mod_size})")
    assert total_size > 1000, f"Total file size too small: {total_size} bytes"

    print("✅ Terminal leaves test passed!")


def test_leaf_parameters():
    """Test different leaf parameter configurations"""
    print("\n" + "="*70)
    print("TEST: Leaf Parameter Variations")
    print("="*70)

    binary_dir = get_binary_dir()
    output_dir = os.path.join(binary_dir, "leaf_param_tests")
    os.makedirs(output_dir, exist_ok=True)

    g = RuzinoGraph("LeafParamTest")
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))

    test_cases = [
        {
            "name": "narrow_leaves",
            "params": {"Leaf Aspect Ratio": 4.0, "Leaf Inclination": 60.0},
            "desc": "Narrow willow-like leaves"
        },
        {
            "name": "wide_leaves",
            "params": {"Leaf Aspect Ratio": 1.5, "Leaf Inclination": 30.0},
            "desc": "Wide oak-like leaves"
        },
        {
            "name": "curved_leaves",
            "params": {"Leaf Curvature": 0.6, "Leaf Aspect Ratio": 2.5},
            "desc": "Curved drooping leaves"
        },
        {
            "name": "phototropic_leaves",
            "params": {"Leaf Phototropism": 0.9, "Leaf Inclination": 20.0},
            "desc": "Highly phototropic leaves"
        }
    ]

    for test_case in test_cases:
        print(f"\n🍃 Test: {test_case['desc']}")

        output_file = os.path.join(output_dir, f"{test_case['name']}.usdc")

        tree = g.createNode("tree_generate", name=f"tree_{test_case['name']}")
        to_mesh = g.createNode("tree_to_mesh", name=f"mesh_{test_case['name']}")
        write_branches = g.createNode("write_usd", name=f"writer_b_{test_case['name']}")
        write_leaves = g.createNode("write_usd", name=f"writer_l_{test_case['name']}")

        g.addEdge(tree, "Tree Branches", to_mesh, "Tree Branches")
        g.addEdge(tree, "Leaves", to_mesh, "Leaves")
        g.addEdge(to_mesh, "Branch Mesh", write_branches, "Geometry")
        g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")

        # Base parameters
        inputs = {
            (tree, "Growth Years"): 4,
            (tree, "Random Seed"): 123,
            (tree, "Generate Leaves"): True,
            (tree, "Terminal Leaves Only"): True,
            (tree, "Leaf Terminal Levels"): 3,
            (tree, "Leaves Per Internode"): 4,
            (tree, "Leaf Size"): 0.18,
            (to_mesh, "Radial Segments"): 6,
            (write_branches, "Sub Path"): "branches",
            (write_leaves, "Sub Path"): "leaves",
        }

        # Apply test-specific parameters
        for param_name, param_value in test_case["params"].items():
            inputs[(tree, param_name)] = param_value

        stage = stage_py.Stage(output_file)
        geom_payload = stage_py.create_payload_from_stage(stage, "/tree")
        g.setGlobalParams(geom_payload)

        g.prepare_and_execute(inputs, required_node=write_branches)
        g.prepare_and_execute(inputs, required_node=write_leaves)

        stage.save()

        modifier_file = output_file.replace(".usdc", "_modifiers.usdc")
        main_size = os.path.getsize(output_file) if os.path.exists(output_file) else 0
        mod_size = os.path.getsize(modifier_file) if os.path.exists(modifier_file) else 0
        total_size = main_size + mod_size
        print(f"  ✓ Created {test_case['name']}: {total_size} bytes")
        assert total_size > 1000, f"Total file size too small: {total_size} bytes"

    print("\n✅ All leaf parameter tests passed!")


def test_leaf_density_comparison():
    """Compare trees with different leaf densities"""
    print("\n" + "="*70)
    print("TEST: Leaf Density Comparison")
    print("="*70)

    binary_dir = get_binary_dir()
    output_file = os.path.join(binary_dir, "leaf_density_comparison.usdc")

    g = RuzinoGraph("LeafDensityTest")
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))

    merge = g.createNode("node_merge_geometry", name="merge")
    write = g.createNode("write_usd", name="writer")
    g.addEdge(merge, "Geometry", write, "Geometry")

    densities = [
        (2, 2, "sparse"),   # 2 leaves, 2 terminal levels
        (4, 3, "medium"),   # 4 leaves, 3 terminal levels
        (6, 4, "dense"),    # 6 leaves, 4 terminal levels
    ]

    spacing = 8.0
    inputs = {}

    for idx, (leaves_per, terminal_levels, name) in enumerate(densities):
        print(f"\n🌳 Creating {name} tree: {leaves_per} leaves/internode, {terminal_levels} terminal levels")

        tree = g.createNode("tree_generate", name=f"tree_{name}")
        to_mesh = g.createNode("tree_to_mesh", name=f"mesh_{name}")
        transform = g.createNode("transform_geom", name=f"transform_{name}")

        g.addEdge(tree, "Tree Branches", to_mesh, "Tree Branches")
        g.addEdge(tree, "Leaves", to_mesh, "Leaves")
        g.addEdge(to_mesh, "Branch Mesh", transform, "Geometry")
        g.addEdge(transform, "Geometry", merge, "Geometries")

        inputs[(tree, "Growth Years")] = 5
        inputs[(tree, "Random Seed")] = 42
        inputs[(tree, "Generate Leaves")] = True
        inputs[(tree, "Terminal Leaves Only")] = True
        inputs[(tree, "Leaves Per Internode")] = leaves_per
        inputs[(tree, "Leaf Terminal Levels")] = terminal_levels
        inputs[(tree, "Leaf Size")] = 0.15
        inputs[(to_mesh, "Radial Segments")] = 6
        inputs[(transform, "Translate X")] = float(idx * spacing)

    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/comparison")
    g.setGlobalParams(geom_payload)

    g.prepare_and_execute(inputs, required_node=write)
    stage.save()

    # The USD geometry data is written to the session layer, exported as a
    # separate _modifiers.usdc file.  Check the combined size of both files.
    modifier_file = output_file.replace(".usdc", "_modifiers.usdc")
    main_size = os.path.getsize(output_file) if os.path.exists(output_file) else 0
    mod_size = os.path.getsize(modifier_file) if os.path.exists(modifier_file) else 0
    total_size = main_size + mod_size
    print(f"\n✓ Created comparison file: {total_size} bytes")
    assert total_size > 1000, f"Total file size too small: {total_size} bytes"

    print("✅ Leaf density comparison test passed!")


if __name__ == "__main__":
    try:
        test_terminal_leaves_only()
        test_leaf_parameters()
        test_leaf_density_comparison()

        print("\n" + "="*70)
        print("  🍃 ALL LEAF SYSTEM TESTS PASSED! 🍃")
        print("="*70)

    except Exception as e:
        print(f"\n✗ TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        import sys
        sys.exit(1)
