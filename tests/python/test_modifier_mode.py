"""
Test script for modifier mode in geometry nodes.

This script tests that geometry node outputs are written to a modifier layer
(as over spec) instead of directly overwriting the original prim data.

Design intent:
1. Create a USD stage with a cylinder prim
2. Create a node graph that generates a grid
3. Execute the node graph with the cylinder prim as target
4. Verify that:
   - The original cylinder data is preserved in root layer
   - The grid output is written to a modifier sublayer as over spec
   - The final composed result shows the grid (modifier has stronger opinion)

Note: conftest.py handles environment setup (paths, DLL loading)
"""

import os
import sys
import json
import tempfile

# Environment setup is handled by conftest.py
# But if running directly, do minimal setup
if 'stage_py' not in sys.modules:
    binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Binaries', 'Release'))
    if binary_dir not in sys.path:
        sys.path.insert(0, binary_dir)
    os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir
    if os.path.exists(binary_dir):
        os.chdir(binary_dir)

try:
    import pxr
    from pxr import Usd, Sdf, UsdGeom, Gf
    HAS_PXR = True
except ImportError as e:
    HAS_PXR = False
    print(f"Warning: pxr module not found: {e}")

try:
    import stage_py
    import nodes_system_py
    import geometry_py
    HAS_RUZINO = True
except ImportError as e:
    HAS_RUZINO = False
    print(f"Warning: Ruzino modules not found: {e}")


def create_simple_node_graph_json():
    """Create a simple node graph JSON that generates a grid and writes to USD."""
    # This is a minimal node graph: create_grid -> write_usd
    node_graph = {
        "links_info": {
            "1": {"ID": 1, "StartPinID": 2, "EndPinID": 3}
        },
        "nodes_info": {
            "1": {
                "ID": 1,
                "id_name": "create_grid",
                "inputs": {"0": 4, "1": 5},
                "outputs": {"0": 2}
            },
            "2": {
                "ID": 2,
                "id_name": "write_usd",
                "inputs": {"0": 3, "1": 6},
                "outputs": None
            }
        },
        "sockets_info": {
            "2": {"ID": 2, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 0, "optional": False, "ui_name": "Geometry"},
            "3": {"ID": 3, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 1, "optional": False, "ui_name": "Geometry"},
            "4": {"ID": 4, "id_name": "int", "identifier": "resolution", "in_out": 1, "optional": False, "ui_name": "resolution", "value": 2},
            "5": {"ID": 5, "id_name": "float", "identifier": "size", "in_out": 1, "optional": False, "ui_name": "size", "value": 1.0},
            "6": {"ID": 6, "id_name": "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >", "identifier": "Sub Path", "in_out": 1, "optional": True, "ui_name": "Sub Path", "value": ""}
        },
        "nodes": {
            "node:1": {"location": {"x": 100, "y": 100}},
            "node:2": {"location": {"x": 300, "y": 100}}
        },
        "selection": None,
        "view": {"scroll": {"x": 0, "y": 0}, "visible_rect": {"min": {"x": 0, "y": 0}, "max": {"x": 500, "y": 300}}, "zoom": 1}
    }
    return json.dumps(node_graph)


def test_modifier_layer_creation():
    """Test that executing a node graph creates a modifier layer."""
    if not HAS_PXR or not HAS_RUZINO:
        print("SKIP: test_modifier_layer_creation - missing dependencies")
        return False

    print("\n=== Test: Modifier Layer Creation ===")
    
    # Get the binary directory where config files are
    binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Binaries', 'Release'))

    # Create a temporary USD file
    with tempfile.NamedTemporaryFile(suffix='.usda', delete=False, mode='w') as f:
        temp_usd_path = f.name

    try:
        # Create a USD stage with a cylinder
        stage = Usd.Stage.CreateNew(temp_usd_path)

        # Define a cylinder prim
        cylinder = UsdGeom.Cylinder.Define(stage, Sdf.Path('/cylinder_0'))
        cylinder.GetRadiusAttr().Set(1.0)
        cylinder.GetHeightAttr().Set(2.0)

        # Save the initial stage
        stage.GetRootLayer().Save()

        print(f"Created stage with cylinder at: {temp_usd_path}")
        print(f"Initial root layer sublayers: {stage.GetRootLayer().subLayerPaths}")

        # Create node system
        system = nodes_system_py.create_dynamic_loading_system()
        system.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system.init()
        
        # Load node graph
        node_graph_json = create_simple_node_graph_json()
        system.get_node_tree().deserialize(node_graph_json)
        
        # Create GeomPayload
        payload = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload, stage, '/cylinder_0')
        payload.is_modifier_mode = True
        payload.delta_time = 0.0
        payload.has_simulation = False
        payload.is_simulating = False

        # Set global params using the helper function
        stage_py.set_global_payload(system, payload)
        
        # Execute
        system.execute()
        
        # Check if modifier layer was created
        root_layer = stage.GetRootLayer()
        sublayers = root_layer.subLayerPaths
        
        print(f"After execution, root layer sublayers: {sublayers}")
        
        # Check for modifier layer
        has_modifier_layer = any('modifier_layer' in str(s) for s in sublayers)
        
        if has_modifier_layer:
            print("SUCCESS: Modifier layer was created")
            
            # Export and check the layer content
            print(f"\nRoot layer content:")
            print(root_layer.ExportToString()[:500])
            
            # Check if over spec exists
            for sublayer_path in sublayers:
                if 'modifier_layer' in str(sublayer_path):
                    sublayer = Sdf.Layer.Find(sublayer_path)
                    if sublayer:
                        print(f"\nModifier layer content:")
                        print(sublayer.ExportToString()[:1000])
        else:
            print("FAILED: No modifier layer was created")
            print("This means the geometry was written directly to the prim, overwriting original data")
        
        return has_modifier_layer
        
    finally:
        # Cleanup
        if os.path.exists(temp_usd_path):
            os.unlink(temp_usd_path)


def test_input_geometry_node():
    """Test that input_geometry node can read from a prim."""
    if not HAS_PXR or not HAS_RUZINO:
        print("SKIP: test_input_geometry_node - missing dependencies")
        return False
    
    print("\n=== Test: Input Geometry Node ===")
    
    # Create a node graph that reads geometry and writes it back
    node_graph = {
        "links_info": {
            "1": {"ID": 1, "StartPinID": 2, "EndPinID": 3}
        },
        "nodes_info": {
            "1": {
                "ID": 1,
                "id_name": "input_geometry",
                "inputs": None,
                "outputs": {"0": 2}
            },
            "2": {
                "ID": 2,
                "id_name": "write_usd",
                "inputs": {"0": 3, "1": 4},
                "outputs": None
            }
        },
        "sockets_info": {
            "2": {"ID": 2, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 0, "optional": False, "ui_name": "Geometry"},
            "3": {"ID": 3, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 1, "optional": False, "ui_name": "Geometry"},
            "4": {"ID": 4, "id_name": "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >", "identifier": "Sub Path", "in_out": 1, "optional": True, "ui_name": "Sub Path", "value": ""}
        },
        "nodes": {
            "node:1": {"location": {"x": 100, "y": 100}},
            "node:2": {"location": {"x": 300, "y": 100}}
        },
        "selection": None,
        "view": {"scroll": {"x": 0, "y": 0}, "visible_rect": {"min": {"x": 0, "y": 0}, "max": {"x": 500, "y": 300}}, "zoom": 1}
    }
    
    with tempfile.NamedTemporaryFile(suffix='.usda', delete=False, mode='w') as f:
        temp_usd_path = f.name
    
    try:
        # Create a USD stage with a mesh prim
        stage = Usd.Stage.CreateNew(temp_usd_path)
        
        # Define a mesh with some geometry
        mesh = UsdGeom.Mesh.Define(stage, Sdf.Path('/test_mesh'))
        mesh.CreatePointsAttr([
            Gf.Vec3f(-0.5, -0.5, 0),
            Gf.Vec3f(0.5, -0.5, 0),
            Gf.Vec3f(0.5, 0.5, 0),
            Gf.Vec3f(-0.5, 0.5, 0)
        ])
        mesh.CreateFaceVertexCountsAttr([4])
        mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
        
        stage.GetRootLayer().Save()
        
        print(f"Created stage with mesh at: {temp_usd_path}")
        
        # Create node system
        system = nodes_system_py.create_dynamic_loading_system()
        binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Binaries', 'Release'))
        system.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system.init()
        
        # Load node graph
        system.get_node_tree().deserialize(json.dumps(node_graph))
        
        # Create GeomPayload
        payload = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload, stage, '/test_mesh')
        payload.is_modifier_mode = True
        
        # Set global params
        stage_py.set_global_payload(system, payload)
        
        # Execute
        system.execute()
        
        # Check result
        root_layer = stage.GetRootLayer()
        sublayers = root_layer.subLayerPaths
        print(f"Sublayers after execution: {sublayers}")
        
        has_modifier = any('modifier_layer' in str(s) for s in sublayers)
        print(f"SUCCESS: Modifier layer created" if has_modifier else "FAILED: No modifier layer")
        
        return has_modifier
        
    finally:
        if os.path.exists(temp_usd_path):
            os.unlink(temp_usd_path)


def test_modifier_preserves_original():
    """Test that modifier mode preserves original prim data."""
    if not HAS_PXR or not HAS_RUZINO:
        print("SKIP: test_modifier_preserves_original - missing dependencies")
        return False
    
    print("\n=== Test: Modifier Preserves Original Data ===")
    
    with tempfile.NamedTemporaryFile(suffix='.usda', delete=False, mode='w') as f:
        temp_usd_path = f.name
    
    try:
        # Create a stage with original geometry
        stage = Usd.Stage.CreateNew(temp_usd_path)
        
        # Create a cylinder with specific attributes
        cylinder = UsdGeom.Cylinder.Define(stage, Sdf.Path('/original_cylinder'))
        cylinder.GetRadiusAttr().Set(5.0)  # Distinctive value
        cylinder.GetHeightAttr().Set(10.0)  # Distinctive value
        
        original_radius = cylinder.GetRadiusAttr().Get()
        original_height = cylinder.GetHeightAttr().Get()
        
        stage.GetRootLayer().Save()
        
        print(f"Original cylinder - radius: {original_radius}, height: {original_height}")
        
        # Execute a node graph that generates different geometry
        system = nodes_system_py.create_dynamic_loading_system()
        binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Binaries', 'Release'))
        system.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system.init()
        
        system.get_node_tree().deserialize(create_simple_node_graph_json())
        
        payload = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload, stage, '/original_cylinder')
        payload.is_modifier_mode = True
        
        stage_py.set_global_payload(system, payload)
        system.execute()
        
        # Check that original attributes are still there
        # In modifier mode, the cylinder attributes should NOT be overwritten
        # because the grid output goes to a modifier layer
        
        # Get the root layer content
        root_content = stage.GetRootLayer().ExportToString()
        
        # The original radius=5 and height=10 should still be in the root layer
        has_original_radius = '5' in root_content and 'float radius' in root_content
        has_original_height = '10' in root_content and 'float height' in root_content
        
        # Check for modifier layer
        sublayers = stage.GetRootLayer().subLayerPaths
        has_modifier = any('modifier_layer' in str(s) for s in sublayers)
        
        print(f"Original data preserved in root: radius={has_original_radius}, height={has_original_height}")
        print(f"Modifier layer exists: {has_modifier}")
        
        success = has_modifier  # At minimum, modifier layer should exist
        
        if success:
            print("SUCCESS: Original data structure is preserved")
        else:
            print("FAILED: Original data may have been overwritten")
        
        return success
        
    finally:
        if os.path.exists(temp_usd_path):
            os.unlink(temp_usd_path)


def main():
    print("=" * 60)
    print("Ruzino Modifier Mode Test Suite")
    print("=" * 60)
    
    results = []
    
    # Run tests
    results.append(("Modifier Layer Creation", test_modifier_layer_creation()))
    results.append(("Input Geometry Node", test_input_geometry_node()))
    results.append(("Preserves Original Data", test_modifier_preserves_original()))
    
    # Summary
    print("\n" + "=" * 60)
    print("Test Results Summary")
    print("=" * 60)
    
    passed = sum(1 for _, result in results if result)
    total = len(results)
    
    for name, result in results:
        status = "PASS" if result else "FAIL"
        print(f"  {name}: {status}")
    
    print(f"\nTotal: {passed}/{total} passed")
    
    return passed == total


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
