"""
Test single terrain generation with USD output
Simplified test that doesn't require merge nodes
"""
import os
from ruzino_graph import RuzinoGraph
import stage_py


def get_binary_dir():
    """Get the binary directory path"""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    binary_dir = os.path.join(test_dir, '..', '..', '..', '..', 'Binaries', 'Release')
    return os.path.abspath(binary_dir)


def test_single_terrain():
    """Generate a single terrain and export to USD"""
    print("\n" + "="*70)
    print("TEST: Single Terrain USD Generation")
    print("="*70)

    binary_dir = get_binary_dir()
    output_file = os.path.join(binary_dir, "terrain_single.usdc")

    g = RuzinoGraph("SingleTerrain")

    # Load configurations
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    print(f"✓ Loaded geometry nodes configuration")

    g.loadConfiguration(os.path.join(binary_dir, "Plugins", "TerrainGen_geometry_nodes.json"))
    print(f"✓ Loaded TerrainGen configuration")

    # Create terrain generation node
    terrain_gen = g.createNode("terrain_generate", name="terrain")
    print(f"✓ Created terrain_generate node")

    # Create write node
    write_node = g.createNode("write_usd", name="writer")
    print(f"✓ Created write_usd node")

    # Connect terrain directly to write
    g.addEdge(terrain_gen, "Height Field", write_node, "Geometry")
    print(f"✓ Connected nodes")

    # Set terrain generation parameters
    inputs = {}
    inputs[(terrain_gen, "Grid Resolution")] = int(128)
    inputs[(terrain_gen, "Grid Size")] = float(100.0)
    inputs[(terrain_gen, "Max Height")] = float(50.0)
    inputs[(terrain_gen, "Octaves")] = int(6)
    inputs[(terrain_gen, "Frequency")] = float(3.0)
    inputs[(terrain_gen, "Persistence")] = float(0.5)
    inputs[(terrain_gen, "Lacunarity")] = float(2.0)
    inputs[(terrain_gen, "Enable Multi Scale")] = bool(True)
    inputs[(terrain_gen, "Mountain Amplitude")] = float(30.0)
    inputs[(terrain_gen, "Random Seed")] = int(42)

    print(f"\nParameters set: 128×128 terrain, Frequency=3.0, Multi-scale enabled")

    # Create Stage and convert to GeomPayload
    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/terrain")
    g.setGlobalParams(geom_payload)

    print(f"\nExecuting graph...")

    # Execute
    g.prepare_and_execute(inputs, required_node=write_node)
    print(f"✓ Executed graph")

    # Save the stage
    stage.save()

    # Check file size (geometry is in the _modifiers.usdc file)
    assert os.path.exists(output_file), f"File not created: {output_file}"
    modifier_file = output_file.replace(".usdc", "_modifiers.usdc")
    main_size = os.path.getsize(output_file)
    mod_size = os.path.getsize(modifier_file) if os.path.exists(modifier_file) else 0
    total_size = main_size + mod_size
    total_size_mb = total_size / (1024 * 1024)
    print(f"\n{'='*70}")
    print(f"✅ SUCCESS: Single Terrain Generated!")
    print(f"{'='*70}")
    print(f"Resolution: 128×128 vertices")
    print(f"File size: {total_size:,} bytes ({total_size_mb:.2f} MB)")
    print(f"Output: {output_file}")
    print(f"{'='*70}")

    # Verify reasonable file size
    assert total_size > 10000, f"File unexpectedly small: {total_size} bytes"
    print(f"\n✓ File size validation passed")


if __name__ == "__main__":
    test_single_terrain()
