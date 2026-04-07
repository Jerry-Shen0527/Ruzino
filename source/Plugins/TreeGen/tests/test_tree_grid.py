"""
Test 5x5x5 tree grid generation with parameter variations
Each dimension varies a different parameter:
- X axis: Growth Years (1-5)
- Y axis: Branch Angle (15-75 degrees)
- Z axis: Apical Control (0.2-1.0)
"""
import os
from ruzino_graph import RuzinoGraph
import stage_py
import geometry_py


def get_binary_dir():
    """Get the binary directory path"""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    binary_dir = os.path.join(test_dir, '..', '..', '..', '..', 'Binaries', 'Release')
    return os.path.abspath(binary_dir)


def test_tree_grid_5x5x5():
    """Generate a 5x5x5 grid of trees with parameter variations"""
    print("\n" + "="*70)
    print("TEST: 5×5×5 Tree Grid Generation")
    print("="*70)

    binary_dir = get_binary_dir()
    output_file = os.path.join(binary_dir, "tree_grid_5x5x5.usdc")

    g = RuzinoGraph("TreeGrid5x5x5")

    # Load configurations
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    print(f"✓ Loaded geometry nodes configuration")

    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))
    print(f"✓ Loaded TreeGen configuration")

    # Create merge node to combine all trees
    merge_node = g.createNode("node_merge_geometry", name="merge_all_trees")
    print(f"✓ Created merge_geometry node")

    # Create write node
    write_node = g.createNode("write_usd", name="writer")
    print(f"✓ Created write_usd node")

    # Connect merge to write
    g.addEdge(merge_node, "Geometry", write_node, "Geometry")

    # Grid spacing
    spacing = 10.0

    # Parameter ranges - chosen for maximum visual variation
    growth_years_range = [2, 3, 4, 5, 6]  # X axis - tree age/size
    branch_angle_range = [20, 35, 50, 65, 80]  # Y axis (degrees) - branching width
    apical_control_range = [0.8, 1.5, 2.5, 3.5, 4.5]  # Z axis - trunk dominance (higher = stronger trunk)

    inputs = {}
    tree_count = 0

    print(f"\n{'='*70}")
    print(f"Generating 5×5×5 = 125 trees...")
    print(f"X axis: Growth Years {growth_years_range}")
    print(f"Y axis: Branch Angle {branch_angle_range}")
    print(f"Z axis: Apical Control {apical_control_range}")
    print(f"{'='*70}\n")

    # Create 5x5x5 grid
    for x_idx, growth_years in enumerate(growth_years_range):
        for y_idx, branch_angle in enumerate(branch_angle_range):
            for z_idx, apical_control in enumerate(apical_control_range):
                tree_count += 1

                # Create tree generation node
                tree_gen = g.createNode("tree_generate", name=f"tree_{x_idx}_{y_idx}_{z_idx}")

                # Create mesh conversion node
                to_mesh = g.createNode("tree_to_mesh", name=f"mesh_{x_idx}_{y_idx}_{z_idx}")

                # Create transform node for positioning
                transform = g.createNode("transform_geom", name=f"transform_{x_idx}_{y_idx}_{z_idx}")

                # Connect: tree_generate -> tree_to_mesh -> transform -> merge
                g.addEdge(tree_gen, "Tree Branches", to_mesh, "Tree Branches")
                g.addEdge(tree_gen, "Leaves", to_mesh, "Leaves")
                g.addEdge(to_mesh, "Branch Mesh", transform, "Geometry")
                g.addEdge(transform, "Geometry", merge_node, "Geometries")

                # Calculate position
                pos_x = float(x_idx * spacing)
                pos_y = float(y_idx * spacing)
                pos_z = float(z_idx * spacing)

                # Set parameters - explicitly use float/int types
                inputs[(tree_gen, "Growth Years")] = int(growth_years)
                inputs[(tree_gen, "Branch Angle")] = float(branch_angle)
                inputs[(tree_gen, "Apical Control")] = float(apical_control)
                inputs[(tree_gen, "Internode Length")] = float(0.8)  # Slightly smaller for density

                inputs[(to_mesh, "Radial Segments")] = int(6)  # Fewer segments for performance

                inputs[(transform, "Translate X")] = pos_x
                inputs[(transform, "Translate Y")] = pos_y
                inputs[(transform, "Translate Z")] = pos_z

                # Progress indicator
                if tree_count % 25 == 0:
                    print(f"  Created {tree_count}/125 trees...")

    print(f"✓ Created all {tree_count} tree nodes in graph")

    # Create Stage and convert to GeomPayload
    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/tree_grid")
    g.setGlobalParams(geom_payload)

    print(f"\nExecuting graph (this may take a moment)...")

    # Execute
    g.prepare_and_execute(inputs, required_node=write_node)
    print(f"✓ Executed graph")

    # Save the stage
    stage.save()

    # Check file size
    if os.path.exists(output_file):
        file_size = os.path.getsize(output_file)
        file_size_mb = file_size / (1024 * 1024)
        print(f"\n{'='*70}")
        print(f"✅ SUCCESS: 5×5×5 Tree Grid Generated!")
        print(f"{'='*70}")
        print(f"Total trees: {tree_count}")
        print(f"File size: {file_size:,} bytes ({file_size_mb:.2f} MB)")
        print(f"Output: {output_file}")
        print(f"{'='*70}")

        # Verify reasonable file size (should be substantial with 125 trees)
        assert file_size > 100000, f"File unexpectedly small: {file_size} bytes"
        print(f"\n✓ File size validation passed")

    else:
        print(f"✗ USD file not found: {output_file}")
        assert False, f"File not created: {output_file}"


if __name__ == "__main__":
    test_tree_grid_5x5x5()
