"""
Test 3x3 terrain grid generation with parameter variations using USD output
Each dimension varies a different parameter:
- X axis: Noise Scale (1.0, 3.0, 5.0)
- Z axis: Height Range (0.5, 1.0, 2.0)
"""
import os
from ruzino_graph import RuzinoGraph
import stage_py


def get_binary_dir():
    """Get the binary directory path"""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    binary_dir = os.path.join(test_dir, '..', '..', '..', '..', 'Binaries', 'Release')
    return os.path.abspath(binary_dir)


def test_terrain_grid_3x3():
    """Generate a 3x3 grid of terrains with parameter variations"""
    print("\n" + "="*70)
    print("TEST: 3×3 Terrain Grid Generation (USD)")
    print("="*70)

    binary_dir = get_binary_dir()
    output_file = os.path.join(binary_dir, "terrain_grid_3x3.usdc")

    g = RuzinoGraph("TerrainGrid3x3")

    # Load configurations
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    print(f"✓ Loaded geometry nodes configuration")

    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TerrainGen_geometry_nodes.json"))
    print(f"✓ Loaded TerrainGen configuration")

    # Create merge node to combine all terrains
    merge_node = g.createNode("node_merge_geometry", name="merge_all_terrains")
    print(f"✓ Created merge_geometry node")

    # Create write node
    write_node = g.createNode("write_usd", name="writer")
    print(f"✓ Created write_usd node")

    # Connect merge to write
    g.addEdge(merge_node, "Geometry", write_node, "Geometry")

    # Grid spacing
    spacing = 150.0

    # Parameter ranges - chosen for visual variation
    noise_scale_range = [1.0, 3.0, 5.0]  # X axis - terrain feature size
    height_range_vals = [0.5, 1.0, 2.0]  # Z axis - elevation variation

    inputs = {}
    terrain_count = 0

    print(f"\n{'='*70}")
    print(f"Generating 3×3 = 9 terrains...")
    print(f"X axis: Noise Scale {noise_scale_range}")
    print(f"Z axis: Height Range {height_range_vals}")
    print(f"{'='*70}\n")

    # Create 3x3 grid
    for x_idx, noise_scale in enumerate(noise_scale_range):
        for z_idx, height_range in enumerate(height_range_vals):
            terrain_count += 1

            # Create terrain generation node
            terrain_gen = g.createNode("terrain_generate", name=f"terrain_{x_idx}_{z_idx}")

            # Create transform node for positioning
            transform = g.createNode("transform_geom", name=f"transform_{x_idx}_{z_idx}")

            # Connect: terrain_generate -> transform -> merge
            g.addEdge(terrain_gen, "Height Field", transform, "Geometry")
            g.addEdge(transform, "Geometry", merge_node, "Geometries")

            # Calculate position
            pos_x = float(x_idx * spacing)
            pos_z = float(z_idx * spacing)

            # Set terrain generation parameters
            inputs[(terrain_gen, "Grid Resolution")] = int(64)
            inputs[(terrain_gen, "Grid Size")] = float(100.0)
            inputs[(terrain_gen, "Max Height")] = float(height_range * 50.0)
            inputs[(terrain_gen, "Random Seed")] = int(x_idx * 3 + z_idx)

            # Vary noise parameters
            inputs[(terrain_gen, "Frequency")] = float(noise_scale)
            inputs[(terrain_gen, "Octaves")] = int(6)
            inputs[(terrain_gen, "Persistence")] = float(0.5)
            inputs[(terrain_gen, "Lacunarity")] = float(2.0)
            inputs[(terrain_gen, "Enable Multi Scale")] = bool(True)
            inputs[(terrain_gen, "Mountain Amplitude")] = float(30.0)

            # Set transform parameters
            inputs[(transform, "Translate X")] = pos_x
            inputs[(transform, "Translate Y")] = float(0.0)
            inputs[(transform, "Translate Z")] = pos_z

            print(f"  Created terrain {terrain_count}/9 (scale={noise_scale}, height={height_range})")

    print(f"✓ Created all {terrain_count} terrain nodes in graph")

    # Create Stage and convert to GeomPayload
    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/terrain_grid")
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
        print(f"✅ SUCCESS: 3×3 Terrain Grid Generated!")
        print(f"{'='*70}")
        print(f"Total terrains: {terrain_count}")
        print(f"File size: {file_size:,} bytes ({file_size_mb:.2f} MB)")
        print(f"Output: {output_file}")
        print(f"{'='*70}")

        # Verify reasonable file size
        assert file_size > 50000, f"File unexpectedly small: {file_size} bytes"
        print(f"\n✓ File size validation passed")

    else:
        print(f"✗ USD file not found: {output_file}")
        assert False, f"File not created: {output_file}"


if __name__ == "__main__":
    test_terrain_grid_3x3()
