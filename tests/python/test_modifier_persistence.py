"""
Test that modifier layer is persisted to sidecar file and restored on reopen.
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
        modifier_path = usd_path.replace(".usda", ".modifiers.usda")

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

        binary_dir = 'C:/Users/Pengfei/WorkSpace/Ruzino/Binaries/Release'
        system = nodes_system_py.create_dynamic_loading_system()
        system.load_configuration(os.path.join(binary_dir, "geometry_nodes.json"))
        system.load_configuration(os.path.join(binary_dir, "basic_nodes.json"))
        system.init()

        system.get_node_tree().deserialize(json.dumps(create_grid_node_graph()))

        payload = stage_py.GeomPayload()
        stage_py.set_payload_stage_and_prim(payload, usd_stage, '/test_sphere')
        payload.is_modifier_mode = True
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

        # Step 4: Save stage (should save modifier layer too)
        print("\n[Step 4] Saving stage...")
        cpp_stage.save()

        # Step 5: Check modifier file after save
        print(f"\n[Step 5] Checking modifier file after save...")
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

        # Check sublayers
        root_layer = usd_stage2.GetRootLayer()
        sublayers = root_layer.subLayerPaths
        print(f"  Sublayers: {sublayers}")

        has_modifier_sublayer = any("modifiers.usda" in s for s in sublayers)
        if not has_modifier_sublayer:
            print("  ✗ Modifier layer was NOT added as sublayer")
            return False

        print("  ✓ Modifier layer is in sublayers")

        # Verify geometry is still modified
        restored_points = get_points_count(usd_stage2, '/test_sphere')
        print(f"  Restored geometry: {restored_points} points")

        if restored_points != 9:
            print(f"  ✗ Expected 9 points, got {restored_points}")
            return False

        print("\n✓ Modifier persistence test passed!")
        return True


if __name__ == "__main__":
    success = test_modifier_persistence()
    sys.exit(0 if success else 1)
