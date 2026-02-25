"""
Headless test for modifier mode - replicates manual UI operations.

Steps:
1. Create a sphere prim
2. Apply node graph: create_grid -> write_usd (should show grid)
3. Change node graph: input_geometry -> write_usd (should show original sphere)
"""

import os
import sys
import json
import tempfile

os.chdir('C:/Users/Pengfei/WorkSpace/Ruzino/Binaries/Release')
sys.path.insert(0, '.')
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = 'C:/Users/Pengfei/WorkSpace/Ruzino/Binaries/Release'

from pxr import Usd, Sdf, UsdGeom, Gf
import stage_py
import nodes_system_py

def create_grid_node_graph():
    """Node graph: create_grid -> write_usd"""
    return {
        "links_info": {
            "1": {"ID": 1, "StartPinID": 2, "EndPinID": 3}
        },
        "nodes_info": {
            "1": {"ID": 1, "id_name": "create_grid", "inputs": {"0": 4, "1": 5}, "outputs": {"0": 2}},
            "2": {"ID": 2, "id_name": "write_usd", "inputs": {"0": 3, "1": 6}, "outputs": None}
        },
        "sockets_info": {
            "2": {"ID": 2, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 0, "optional": False, "ui_name": "Geometry"},
            "3": {"ID": 3, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 1, "optional": False, "ui_name": "Geometry"},
            "4": {"ID": 4, "id_name": "int", "identifier": "resolution", "in_out": 1, "optional": False, "ui_name": "resolution", "value": 2},
            "5": {"ID": 5, "id_name": "float", "identifier": "size", "in_out": 1, "optional": False, "ui_name": "size", "value": 1.0},
            "6": {"ID": 6, "id_name": "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >", "identifier": "Sub Path", "in_out": 1, "optional": True, "ui_name": "Sub Path", "value": ""}
        },
        "nodes": {"node:1": {"location": {"x": 100, "y": 100}}, "node:2": {"location": {"x": 300, "y": 100}}},
        "selection": None,
        "view": {"scroll": {"x": 0, "y": 0}, "visible_rect": {"min": {"x": 0, "y": 0}, "max": {"x": 500, "y": 300}}, "zoom": 1}
    }

def create_input_geom_node_graph():
    """Node graph: input_geometry -> write_usd"""
    return {
        "links_info": {
            "1": {"ID": 1, "StartPinID": 2, "EndPinID": 3}
        },
        "nodes_info": {
            "1": {"ID": 1, "id_name": "input_geometry", "inputs": None, "outputs": {"0": 2}},
            "2": {"ID": 2, "id_name": "write_usd", "inputs": {"0": 3, "1": 4}, "outputs": None}
        },
        "sockets_info": {
            "2": {"ID": 2, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 0, "optional": False, "ui_name": "Geometry"},
            "3": {"ID": 3, "id_name": "class Ruzino::Geometry", "identifier": "Geometry", "in_out": 1, "optional": False, "ui_name": "Geometry"},
            "4": {"ID": 4, "id_name": "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >", "identifier": "Sub Path", "in_out": 1, "optional": True, "ui_name": "Sub Path", "value": ""}
        },
        "nodes": {"node:1": {"location": {"x": 100, "y": 100}}, "node:2": {"location": {"x": 300, "y": 100}}},
        "selection": None,
        "view": {"scroll": {"x": 0, "y": 0}, "visible_rect": {"min": {"x": 0, "y": 0}, "max": {"x": 500, "y": 300}}, "zoom": 1}
    }

def get_points_count(stage, prim_path):
    """Get the number of points in a prim's geometry."""
    prim = stage.GetPrimAtPath(prim_path)
    if not prim:
        return -1
    
    mesh = UsdGeom.Mesh(prim)
    if not mesh:
        return -1
    
    points = mesh.GetPointsAttr().Get()
    return len(points) if points else 0

def test_modifier_restore():
    print("=" * 60)
    print("Headless Modifier Restore Test")
    print("=" * 60)
    
    # Create a temporary USD file
    with tempfile.NamedTemporaryFile(suffix='.usda', delete=False, mode='w') as f:
        temp_usd_path = f.name
    
    try:
        # Step 1: Create a USD stage with a sphere
        print("\n[Step 1] Creating sphere prim...")
        stage = Usd.Stage.CreateNew(temp_usd_path)
        
        # Create a MESH sphere (not parametric) so we can compare points
        sphere = UsdGeom.Mesh.Define(stage, Sdf.Path('/test_sphere'))
        # Create a simple sphere-like mesh with specific points
        sphere.CreatePointsAttr([
            Gf.Vec3f(0, 1, 0),    # top
            Gf.Vec3f(1, 0, 0),    # equator
            Gf.Vec3f(0, 0, 1),
            Gf.Vec3f(-1, 0, 0),
            Gf.Vec3f(0, 0, -1),
            Gf.Vec3f(0, -1, 0),   # bottom
        ])
        sphere.CreateFaceVertexCountsAttr([3, 3, 3, 3, 3, 3, 3, 3])
        sphere.CreateFaceVertexIndicesAttr([0,1,2, 0,2,3, 0,3,4, 0,4,1, 5,2,1, 5,3,2, 5,4,3, 5,1,4])
        
        stage.GetRootLayer().Save()
        
        original_points = get_points_count(stage, '/test_sphere')
        print(f"  Original sphere mesh points count: {original_points}")
        
        # Step 2: Apply create_grid -> write_usd
        print("\n[Step 2] Applying create_grid -> write_usd...")
        
        binary_dir = 'C:/Users/Pengfei/WorkSpace/Ruzino/Binaries/Release'
        system = nodes_system_py.create_dynamic_loading_system()
        system.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system.init()
        
        system.get_node_tree().deserialize(json.dumps(create_grid_node_graph()))
        
        payload = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload, stage, '/test_sphere')
        stage_py.set_global_payload(system, payload)
        
        system.execute()
        
        grid_points = get_points_count(stage, '/test_sphere')
        print(f"  After create_grid, points count: {grid_points}")
        print(f"  Expected: 9 (grid with resolution=2)")
        
        if grid_points == 9:
            print("  ✓ Grid was applied successfully")
        else:
            print(f"  ✗ Expected 9 points, got {grid_points}")
        
        # Check session layer content
        session_layer = stage.GetSessionLayer()
        print(f"\n  Session layer ID: {session_layer.identifier}")
        prim_spec = session_layer.GetPrimAtPath('/test_sphere')
        if prim_spec:
            print(f"  Session layer has prim spec for /test_sphere")
            attrs = prim_spec.attributes
            print(f"  Attributes in session layer: {[a.name for a in attrs] if attrs else 'none'}")
        
        # Step 3: Change to input_geometry -> write_usd
        # CRITICAL: Use the SAME system, just change the node graph
        print("\n[Step 3] Changing to input_geometry -> write_usd...")
        print("  (This should restore the original sphere geometry)")
        
        # Clear session layer first to simulate proper modifier behavior
        print("\n  Clearing session layer over specs...")
        if prim_spec:
            attrs_to_clear = list(prim_spec.attributes)
            for attr in attrs_to_clear:
                attr.ClearDefaultValue()
            print(f"  Cleared {len(attrs_to_clear)} attributes from session layer")
        
        # Check what composed stage shows after clearing
        points_after_clear = get_points_count(stage, '/test_sphere')
        print(f"  After clearing session layer, composed stage points: {points_after_clear}")
        print(f"  (Should be 6 - the original sphere mesh)")
        
        if points_after_clear != 6:
            print("  ⚠ Session layer clear did NOT work! USD composition still shows modified data")
            # Try to actually remove the prim spec
            print("  Trying to remove prim spec from session layer...")
            session_layer.RemovePrim('/test_sphere')
            points_after_remove = get_points_count(stage, '/test_sphere')
            print(f"  After removing prim spec, composed stage points: {points_after_remove}")
        
        # Now execute with input_geometry
        system.get_node_tree().deserialize(json.dumps(create_input_geom_node_graph()))
        # Note: system.mark_tree_structure_changed() not available in Python API
        
        payload2 = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload2, stage, '/test_sphere')
        # CRITICAL: Set modifier mode so input_geometry reads from root layer
        payload2.is_modifier_mode = True
        payload2.current_modifier_index = 0  # First modifier reads from root layer
        stage_py.set_global_payload(system, payload2)
        
        system.execute()
        
        restored_points = get_points_count(stage, '/test_sphere')
        print(f"\n  After input_geometry, points count: {restored_points}")
        print(f"  Expected: {original_points} (original sphere mesh)")
        
        if restored_points == original_points:
            print("  ✓ Original sphere was restored!")
            return True
        else:
            print(f"  ✗ FAILED: Expected {original_points} points, got {restored_points}")
            print("  This means the session layer was NOT cleared properly")
            return False
            
    finally:
        if os.path.exists(temp_usd_path):
            os.unlink(temp_usd_path)

if __name__ == "__main__":
    success = test_modifier_restore()
    sys.exit(0 if success else 1)
