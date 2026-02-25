"""
Test that modifier layer is persisted to sidecar file and restored on reopen.
"""
import os
import sys
import json
import tempfile

binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Binaries', 'Release'))
os.chdir(binary_dir)
sys.path.insert(0, '.')
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir

from pxr import Usd, Sdf, UsdGeom, Gf
import stage_py
import nodes_system_py


def create_grid_node_graph():
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


def get_points_count(stage, prim_path):
    prim = stage.GetPrimAtPath(prim_path)
    if not prim:
        return -1
    mesh = UsdGeom.Mesh(prim)
    if not mesh:
        return -1
    points = mesh.GetPointsAttr().Get()
    return len(points) if points else 0


def test_modifier_persistence():
    print("=" * 60)
    print("Modifier Persistence Test (Sidecar File)")
    print("=" * 60)

    with tempfile.TemporaryDirectory() as tmpdir:
        usd_path = os.path.join(tmpdir, "test_scene.usda")
        modifier_path = usd_path.replace(".usda", "_modifiers.usda")

        print(f"\nUSD path: {usd_path}")
        print(f"Expected modifier path: {modifier_path}")

        # Step 1: Create a USD stage with a sphere
        print("\n[Step 1] Creating sphere mesh...")
        stage = Usd.Stage.CreateNew(usd_path)

        sphere = UsdGeom.Mesh.Define(stage, Sdf.Path('/test_sphere'))
        sphere.CreatePointsAttr([
            Gf.Vec3f(0, 1, 0),
            Gf.Vec3f(1, 0, 0),
            Gf.Vec3f(0, 0, 1),
            Gf.Vec3f(-1, 0, 0),
            Gf.Vec3f(0, 0, -1),
            Gf.Vec3f(0, -1, 0),
        ])
        sphere.CreateFaceVertexCountsAttr([3, 3, 3, 3, 3, 3, 3, 3])
        sphere.CreateFaceVertexIndicesAttr([0,1,2, 0,2,3, 0,3,4, 0,4,1, 5,2,1, 5,3,2, 5,4,3, 5,1,4])

        stage.GetRootLayer().Save()
        original_points = get_points_count(stage, '/test_sphere')
        print(f"  Original sphere: {original_points} points")

        # Step 2: Apply modifier using Stage class
        print("\n[Step 2] Applying create_grid -> write_usd...")

        # Use C++ Stage class for proper modifier layer management
        cpp_stage = stage_py.Stage(usd_path)
        usd_stage = cpp_stage.get_pxr_stage()  # Get pxr.Usd.Stage object
        
        # Get the modifier layer from C++ Stage (this is the key!)
        modifier_layer_id = cpp_stage.get_modifier_layer_identifier()
        print(f"  Modifier layer ID: {modifier_layer_id}")
        
        if modifier_layer_id:
            modifier_layer = Sdf.Layer.Find(modifier_layer_id)
            print(f"  Found modifier layer: {modifier_layer.identifier if modifier_layer else 'null'}")
        else:
            modifier_layer = None
            print("  WARNING: No modifier layer available!")

        binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Binaries', 'Release'))
        system = nodes_system_py.create_dynamic_loading_system()
        system.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system.init()

        system.get_node_tree().deserialize(json.dumps(create_grid_node_graph()))

        payload = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload, usd_stage, '/test_sphere')
        payload.is_modifier_mode = True
        if modifier_layer:
            stage_py.set_payload_modifier_layer(payload, modifier_layer)
        stage_py.set_global_payload(system, payload)

        system.execute()

        grid_points = get_points_count(usd_stage, '/test_sphere')
        print(f"  After modifier: {grid_points} points")

        if grid_points != 9:
            print(f"  ✗ Expected 9 points, got {grid_points}")
            return False

        # Step 3: Check modifier file before save
        print(f"\n[Step 3] Checking modifier file before save...")
        print(f"  Modifier file exists: {os.path.exists(modifier_path)}")

        # Step 4: Delete cpp_stage to trigger destructor save
        print("\n[Step 4] Deleting cpp_stage (triggers destructor save)...")
        del cpp_stage

        # Step 5: Check modifier file after destructor save
        print(f"\n[Step 5] Checking modifier file after destructor save...")
        print(f"  Modifier file exists: {os.path.exists(modifier_path)}")

        if not os.path.exists(modifier_path):
            print("  ✗ Modifier file was NOT created!")
            # Check what files exist in tmpdir
            print(f"  Files in tmpdir: {os.listdir(tmpdir)}")
            return False

        print("  ✓ Modifier file was created")

        # Step 6: Reopen stage and verify modifier layer loaded
        print("\n[Step 6] Reopening stage...")
        cpp_stage2 = stage_py.Stage()
        cpp_stage2.open_stage(usd_path)
        usd_stage2 = cpp_stage2.get_pxr_stage()  # Get pxr.Usd.Stage object

        # Check session layer (modifier is loaded into session layer)
        session_layer = usd_stage2.GetSessionLayer()
        session_content = session_layer.ExportToString()
        has_modifier_content = "points" in session_content.lower() and len(session_content) > 100
        print(f"  Session layer has modifier content: {has_modifier_content}")

        if not has_modifier_content:
            print("  ✗ Modifier content was NOT loaded into session layer")
            print(f"  Session layer content (first 200 chars): {session_content[:200]}")
            return False

        print("  ✓ Modifier content is in session layer")

        # Verify geometry is modified (9 points)
        restored_points = get_points_count(usd_stage2, '/test_sphere')
        print(f"  After reopen, geometry: {restored_points} points")
        if restored_points != 9:
            print(f"  ✗ Expected 9 points after reopen, got {restored_points}")
            return False

        # Step 7: Use input_geometry to restore original sphere
        print("\n[Step 7] Restoring original geometry with input_geometry...")
        
        def create_input_geom_node_graph():
            return {
                "links_info": {"1": {"ID": 1, "StartPinID": 2, "EndPinID": 3}},
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
        
        # Get session layer as modifier layer
        modifier_layer_id = cpp_stage2.get_modifier_layer_identifier()
        modifier_layer2 = Sdf.Layer.Find(modifier_layer_id) if modifier_layer_id else None
        
        system2 = nodes_system_py.create_dynamic_loading_system()
        system2.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system2.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system2.init()
        system2.get_node_tree().deserialize(json.dumps(create_input_geom_node_graph()))
        
        payload2 = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload2, usd_stage2, '/test_sphere')
        payload2.is_modifier_mode = True
        payload2.current_modifier_index = 0  # input_geometry should read from root layer
        if modifier_layer2:
            stage_py.set_payload_modifier_layer(payload2, modifier_layer2)
        stage_py.set_global_payload(system2, payload2)
        
        system2.execute()
        
        final_points = get_points_count(usd_stage2, '/test_sphere')
        print(f"  After input_geometry, geometry: {final_points} points")
        
        if final_points != 6:
            print(f"  ✗ Expected 6 points (original sphere), got {final_points}")
            return False

        print("\n✓ Modifier persistence test passed!")
        return True


if __name__ == "__main__":
    success = test_modifier_persistence()
    sys.exit(0 if success else 1)
