"""
Test full tree generation pipeline: generate -> to_mesh -> write_usd
"""
import os
from ruzino_graph import RuzinoGraph
import stage_py
import geometry_py  # This triggers geometry nodes loading


def get_binary_dir():
    """Get the binary directory path"""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    binary_dir = os.path.join(test_dir, '..', '..', '..', '..', 'Binaries', 'Release')
    return os.path.abspath(binary_dir)


def test_full_tree_generation():
    """Test complete Plastic Trees generation pipeline"""
    print("\n" + "="*70)
    print("TEST: Full Plastic Trees Pipeline (generate -> to_mesh -> USD)")
    print("="*70)

    binary_dir = get_binary_dir()
    output_file = os.path.join(binary_dir, "plastic_tree.usdc")

    g = RuzinoGraph("PlasticTreeFullTest")

    # Load geometry nodes first (like test_write_usd does)
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    print(f"✓ Loaded geometry nodes configuration")

    # Then load TreeGen nodes
    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))
    print(f"✓ Loaded TreeGen configuration")

    # Create nodes
    tree_gen = g.createNode("tree_generate", name="tree")
    print(f"✓ Created tree_generate node")

    to_mesh = g.createNode("tree_to_mesh", name="mesh_converter")
    print(f"✓ Created tree_to_mesh node")

    write_branches = g.createNode("write_usd", name="writer_branches")
    print(f"✓ Created write_usd node for branches")

    write_leaves = g.createNode("write_usd", name="writer_leaves")
    print(f"✓ Created write_usd node for leaves")

    # Connect nodes:
    # tree_generate -> tree_to_mesh -> write_usd (branches and leaves)
    g.addEdge(tree_gen, "Tree Branches", to_mesh, "Tree Branches")
    g.addEdge(tree_gen, "Leaves", to_mesh, "Leaves")
    g.addEdge(to_mesh, "Branch Mesh", write_branches, "Geometry")
    g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")
    print(f"✓ Connected nodes")

    # Set parameters for Plastic Trees
    inputs = {
        (tree_gen, "Growth Years"): 3,
        (tree_gen, "Internode Length"): 1.0,
        (tree_gen, "Branch Angle"): 30.0,
        (tree_gen, "Generate Leaves"): True,
        (tree_gen, "Terminal Leaves Only"): True,
        (tree_gen, "Leaf Terminal Levels"): 3,
        (tree_gen, "Leaves Per Internode"): 4,
        (tree_gen, "Leaf Size"): 0.2,
        (tree_gen, "Leaf Aspect Ratio"): 2.0,
        (tree_gen, "Leaf Inclination"): 45.0,
        (tree_gen, "Leaf Phototropism"): 0.5,
        # Plastic Trees parameters
        (tree_gen, "Enable Plasticity"): True,
        (tree_gen, "Environmental Sensitivity"): 0.5,
        (tree_gen, "Phototropism"): 0.3,
        (tree_gen, "Gravitropism"): 0.2,
        (tree_gen, "Branch Flexibility"): 0.3,
        (tree_gen, "Cluster Translucency"): 0.5,
        (to_mesh, "Radial Segments"): 8,
        (write_branches, "Sub Path"): "branches",
        (write_leaves, "Sub Path"): "leaves",
    }
    print(f"✓ Set Plastic Trees parameters: Plasticity=ON, Sensitivity=0.5, Flexibility=0.3")

    # Create Stage and convert to GeomPayload
    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/plastic_tree")
    g.setGlobalParams(geom_payload)

    # Execute both outputs
    g.prepare_and_execute(inputs, required_node=write_branches)
    g.prepare_and_execute(inputs, required_node=write_leaves)
    print(f"✓ Executed graph with branches at /plastic_tree/branches and leaves at /plastic_tree/leaves")

    # Save the stage
    stage.save()

    # Check file size
    if os.path.exists(output_file):
        file_size = os.path.getsize(output_file)
        print(f"✓ USD file created: {file_size} bytes")

        if file_size > 1000:
            print("\n" + "="*70)
            print(f"✅ TEST PASSED: Plastic Tree USD file generated ({file_size} bytes)!")
            print(f"Output: {output_file}")
            print("   Trees adapt to environment with leaf cluster illumination!")
            print("="*70)
            assert file_size > 1000, f"File too small: {file_size} bytes"
        else:
            print(f"\n✗ TEST FAILED: USD file too small: {file_size} bytes")
            assert False, f"File too small: {file_size} bytes"
    else:
        print(f"✗ USD file not found: {output_file}")
        assert False, f"File not created: {output_file}"
