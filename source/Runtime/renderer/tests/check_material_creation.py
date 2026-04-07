"""
Check why materials are not being created by the delegate
"""
import sys
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
workspace_root = os.path.abspath(os.path.join(script_dir, "..", "..", "..", ".."))
binary_dir = os.path.join(workspace_root, "Binaries", "Release")

os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir
sys.path.insert(0, binary_dir)
os.chdir(binary_dir)

from pxr import Usd, UsdShade, Sdf, UsdGeom

def check_material_creation(file_path, file_name):
    """Check if materials should trigger CreateSprim"""
    print(f"\n{'='*100}")
    print(f"CHECKING: {file_name}")
    print(f"{'='*100}\n")

    if not os.path.exists(file_path):
        print(f"ERROR: File not found!")
        return

    stage = Usd.Stage.Open(file_path)

    print("MATERIALS AND THEIR PROPERTIES:")
    for prim in stage.Traverse():
        if prim.IsA(UsdShade.Material):
            path = prim.GetPath()
            print(f"\n  Path: {path}")
            print(f"    TypeName: {prim.GetTypeName()}")
            print(f"    Specifier: {prim.GetSpecifier()}")  # Def vs Over vs Class
            print(f"    IsAbstract: {prim.IsAbstract()}")
            print(f"    IsInPrototype: {prim.IsInPrototype()}")
            print(f"    IsInstance: {prim.IsInstance()}")
            print(f"    IsInstanceProxy: {prim.IsInstanceProxy()}")
            print(f"    HasAuthoredReferences: {prim.HasAuthoredReferences()}")
            print(f"    HasPayload: {prim.HasPayload()}")
            print(f"    HasVariantSets: {prim.HasVariantSets()}")

            # Check the composition structure
            prim_stack = prim.GetPrimStack()
            print(f"    PrimStack layers: {len(prim_stack)}")
            for i, spec in enumerate(prim_stack):
                print(f"      [{i}] {spec.layer.identifier} : {spec.path}")

            # Check if material is in a referenced prim
            parent = prim.GetParent()
            while parent:
                if parent.HasAuthoredReferences():
                    refs_list = parent.GetMetadata('references')
                    print(f"    >>> Parent {parent.GetPath()} has references: {refs_list}")
                parent = parent.GetParent()

def main():
    workspace_root = os.path.abspath(os.path.join(script_dir, "..", "..", "..", ".."))

    chess_set_file = os.path.join(workspace_root, "Assets", "OpenChessSet", "chess_set.usda")
    shader_ball = os.path.join(binary_dir, "material_tests", "shader_ball_TH_Rough_Wood.usdc")

    check_material_creation(chess_set_file, "chess_set.usda (NOT WORKING)")
    check_material_creation(shader_ball, "shader_ball (WORKING)")

    print(f"\n{'='*100}")
    print("HYPOTHESIS:")
    print(f"{'='*100}")
    print("Chess materials may be inside referenced USD files.")
    print("Hydra may not traverse into references to create materials.")
    print("Or the materials are using variants/prototypes that don't trigger CreateSprim.")

if __name__ == "__main__":
    main()
