"""
Compare USD files to understand material structure differences
"""
import sys
import os

# Get binary directory
tests_dir = os.path.dirname(os.path.abspath(__file__))
binary_dir = os.path.abspath(os.path.join(tests_dir, "..", "..", "..", "..", "Binaries", "Release"))

# Set PXR_USD_WINDOWS_DLL_PATH so USD can find its DLLs
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir
print(f"Set PXR_USD_WINDOWS_DLL_PATH={binary_dir}")

# Add to Python path
sys.path.insert(0, binary_dir)

# Change to binary dir so DLLs can be loaded
os.chdir(binary_dir)

from pxr import Usd, UsdShade, Sdf, UsdGeom

def analyze_material_structure(stage, file_name):
    """Analyze materials in a USD stage"""
    print(f"\n{'='*80}")
    print(f"Analyzing: {file_name}")
    print(f"{'='*80}\n")

    materials = []
    material_paths = []

    # Find all materials
    for prim in stage.Traverse():
        if prim.IsA(UsdShade.Material):
            materials.append(prim)
            material_paths.append(prim.GetPath())
            print(f"Found Material: {prim.GetPath()}")
            print(f"  Type: {prim.GetTypeName()}")
            print(f"  Is Active: {prim.IsActive()}")
            print(f"  Is Defined: {prim.IsDefined()}")
            print(f"  Metadata: {prim.GetAllMetadata()}")

            # Check UsdShade.Material API
            material = UsdShade.Material(prim)

            # Surface output
            surface_output = material.GetSurfaceOutput()
            if surface_output:
                print(f"  Surface Output: {surface_output}")
                connections = surface_output.GetConnectedSources()
                print(f"  Surface Connections: {connections}")
                if connections and len(connections) > 0:
                    for source_info in connections[0]:
                        if hasattr(source_info, 'source'):
                            print(f"    - Source: {source_info.source.GetPath()}")
                            print(f"      Output Name: {source_info.sourceName}")
            else:
                print(f"  No surface output")

            # Volume output
            volume_output = material.GetVolumeOutput()
            if volume_output:
                print(f"  Volume Output: {volume_output}")

            # Displacement output
            displacement_output = material.GetDisplacementOutput()
            if displacement_output:
                print(f"  Displacement Output: {displacement_output}")

            # Check children (shaders)
            print(f"  Children:")
            for child in prim.GetChildren():
                print(f"    - {child.GetPath()} (Type: {child.GetTypeName()})")
                if child.IsA(UsdShade.Shader):
                    shader = UsdShade.Shader(child)
                    shader_id = shader.GetIdAttr().Get()
                    print(f"      Shader ID: {shader_id}")

                    # Get all inputs
                    for input in shader.GetInputs():
                        print(f"      Input: {input.GetBaseName()}")
                        value = input.Get()
                        connections = input.GetConnectedSources()
                        if connections:
                            print(f"        Connected to: {connections}")
                        else:
                            print(f"        Value: {value}")

            print()

    # Check material bindings
    print(f"\nMaterial Bindings:")
    for prim in stage.Traverse():
        if UsdShade.MaterialBindingAPI(prim).GetDirectBinding():
            binding = UsdShade.MaterialBindingAPI(prim).GetDirectBinding()
            print(f"  {prim.GetPath()} -> {binding.GetMaterialPath()}")

    print(f"\nTotal materials found: {len(materials)}")
    print(f"Material paths: {material_paths}")

    return materials, material_paths

def compare_files():
    """Compare the two USD files"""

    # File paths
    assets_path = os.path.join("..", "..", "..", "..", "Assets")
    chess_set_path = os.path.join(assets_path, "OpenChessSet", "chess_set.usda")
    # Also check one of the referenced piece files
    chess_piece_path = os.path.join(assets_path, "OpenChessSet", "assets", "King", "King.usd")
    shader_ball_path = os.path.join("material_tests", "shader_ball_TH_Rough_Wood.usdc")

    chess_set_abs = os.path.abspath(chess_set_path)
    chess_piece_abs = os.path.abspath(chess_piece_path)
    shader_ball_abs = os.path.abspath(shader_ball_path)

    print(f"Chess set path: {chess_set_abs}")
    print(f"Chess piece path: {chess_piece_abs}")
    print(f"Shader ball path: {shader_ball_abs}")

    # Open stages
    if os.path.exists(chess_set_abs):
        chess_stage = Usd.Stage.Open(chess_set_abs)
        chess_materials, chess_paths = analyze_material_structure(chess_stage, "chess_set.usda")
    else:
        print(f"ERROR: Chess set file not found at {chess_set_abs}")
        chess_materials, chess_paths = [], []

    if os.path.exists(chess_piece_abs):
        chess_piece_stage = Usd.Stage.Open(chess_piece_abs)
        chess_piece_materials, chess_piece_paths = analyze_material_structure(chess_piece_stage, "King.usd")
    else:
        print(f"ERROR: Chess piece file not found at {chess_piece_abs}")
        chess_piece_materials, chess_piece_paths = [], []

    if os.path.exists(shader_ball_abs):
        shader_ball_stage = Usd.Stage.Open(shader_ball_abs)
        shader_ball_materials, shader_ball_paths = analyze_material_structure(shader_ball_stage, "shader_ball_TH_Rough_Wood.usdc")
    else:
        print(f"ERROR: Shader ball file not found at {shader_ball_abs}")
        shader_ball_materials, shader_ball_paths = [], []

    # Summary
    print(f"\n{'='*80}")
    print(f"SUMMARY")
    print(f"{'='*80}")
    print(f"Chess set materials: {len(chess_materials)}")
    print(f"Chess piece materials: {len(chess_piece_materials)}")
    print(f"Shader ball materials: {len(shader_ball_materials)}")
    print(f"\nKey differences to investigate:")
    print(f"1. Material definition structure")
    print(f"2. Material path patterns")
    print(f"3. Shader network complexity")
    print(f"4. Referenced vs inline materials")

if __name__ == "__main__":
    compare_files()
