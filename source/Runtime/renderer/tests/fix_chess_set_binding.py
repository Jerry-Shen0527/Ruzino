"""
Fix chess_set.usda by properly setting up MaterialX references with surface connections
Using the same approach as materials_batch_test.py that works successfully
"""
import sys
import os
from pathlib import Path

script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Release"

os.environ['PXR_USD_WINDOWS_DLL_PATH'] = str(binary_dir)
sys.path.insert(0, str(binary_dir))
os.chdir(str(binary_dir))

from pxr import Usd, UsdShade, Sdf

def fix_material_surface_connection(stage, materials_scope_path, mtlx_file_path):
    """
    Fix a Materials scope by ensuring surface output is properly connected
    This mimics the working approach from materials_batch_test.py
    """
    print(f"\nProcessing: {materials_scope_path}")

    # Get the Materials scope
    materials_scope = stage.GetPrimAtPath(materials_scope_path)
    if not materials_scope:
        print(f"  ERROR: Materials scope not found")
        return False

    # Check if .mtlx file exists
    usd_dir = Path(stage.GetRootLayer().realPath).parent
    mtlx_abs = usd_dir / mtlx_file_path
    if not mtlx_abs.exists():
        print(f"  ERROR: MaterialX file not found: {mtlx_abs}")
        return False

    print(f"  MaterialX: {mtlx_abs.name}")

    # Open the MaterialX file to find what materials it contains
    mtlx_stage = Usd.Stage.Open(str(mtlx_abs))
    if not mtlx_stage:
        print(f"  ERROR: Failed to open MaterialX file")
        return False

    # Find all materials in the .mtlx file
    mtlx_materials = []
    for prim in mtlx_stage.Traverse():
        if prim.IsA(UsdShade.Material):
            mtlx_materials.append(prim)

    print(f"  Found {len(mtlx_materials)} materials in MaterialX file")

    # Process each material in the current stage's Materials scope
    fixed_count = 0
    for child in materials_scope.GetChildren():
        if not child.IsA(UsdShade.Material):
            continue

        material = UsdShade.Material(child)
        material_name = child.GetName()

        # Check if this material already has a proper surface connection
        surface_output = material.GetSurfaceOutput()
        if surface_output:
            connections = surface_output.GetConnectedSources()
            if connections and any(connections):
                print(f"  {material_name}: Already has surface connections ✓")
                continue

        # This material needs fixing
        print(f"  {material_name}: Fixing surface connection...")

        # Find the corresponding shader prim
        shader_prim = None
        for mat_child in child.GetChildren():
            if mat_child.IsA(UsdShade.Shader):
                shader_prim = mat_child
                break

        if not shader_prim:
            print(f"    WARNING: No shader found, skipping")
            continue

        # Get shader's surface output
        shader = UsdShade.Shader(shader_prim)
        shader_surface = shader.GetOutput("surface")

        if not shader_surface:
            print(f"    WARNING: Shader has no surface output, skipping")
            continue

        # Create or get material surface output and connect it
        if not surface_output:
            surface_output = material.CreateSurfaceOutput()

        surface_output.ConnectToSource(shader_surface)
        print(f"    ✓ Connected surface output")
        fixed_count += 1

    print(f"  Fixed {fixed_count} materials")
    return True

def main():
    print("="*80)
    print("Fixing chess_set.usda MaterialX surface connections")
    print("="*80)

    chess_set_path = workspace_root / "Assets" / "OpenChessSet" / "chess_set.usda"
    output_path = workspace_root / "Assets" / "OpenChessSet" / "chess_set_fixed.usda"

    print(f"Input:  {chess_set_path}")
    print(f"Output: {output_path}")

    # Open stage
    stage = Usd.Stage.Open(str(chess_set_path))
    if not stage:
        print("ERROR: Failed to open chess_set.usda")
        return 1

    # Map of Materials scopes to their .mtlx files
    materials_map = {
        "/ChessSet/Chessboard/Materials": "assets/Chessboard/Chessboard_mat.mtlx",
        "/ChessSet/Black/King/Materials": "assets/King/King_mat.mtlx",
        "/ChessSet/Black/Queen/Materials": "assets/Queen/Queen_mat.mtlx",
        "/ChessSet/Black/BishopL/Materials": "assets/Bishop/Bishop_mat.mtlx",
        "/ChessSet/Black/BishopR/Materials": "assets/Bishop/Bishop_mat.mtlx",
        "/ChessSet/Black/KnightL/Materials": "assets/Knight/Knight_mat.mtlx",
        "/ChessSet/Black/KnightR/Materials": "assets/Knight/Knight_mat.mtlx",
        "/ChessSet/Black/RookL/Materials": "assets/Rook/Rook_mat.mtlx",
        "/ChessSet/Black/RookR/Materials": "assets/Rook/Rook_mat.mtlx",
        "/ChessSet/Black/Pawns/Pawn/Materials": "assets/Pawn/Pawn_mat.mtlx",
        "/ChessSet/White/King/Materials": "assets/King/King_mat.mtlx",
        "/ChessSet/White/Queen/Materials": "assets/Queen/Queen_mat.mtlx",
        "/ChessSet/White/BishopL/Materials": "assets/Bishop/Bishop_mat.mtlx",
        "/ChessSet/White/BishopR/Materials": "assets/Bishop/Bishop_mat.mtlx",
        "/ChessSet/White/KnightL/Materials": "assets/Knight/Knight_mat.mtlx",
        "/ChessSet/White/KnightR/Materials": "assets/Knight/Knight_mat.mtlx",
        "/ChessSet/White/RookL/Materials": "assets/Rook/Rook_mat.mtlx",
        "/ChessSet/White/RookR/Materials": "assets/Rook/Rook_mat.mtlx",
        "/ChessSet/White/Pawns/Pawn/Materials": "assets/Pawn/Pawn_mat.mtlx",
    }

    # Fix each materials scope
    for scope_path, mtlx_file in materials_map.items():
        fix_material_surface_connection(stage, scope_path, mtlx_file)

    # Save
    print(f"\n{'='*80}")
    print(f"Saving to: {output_path}")
    stage.Export(str(output_path), args={'format': 'usda'})

    # Verify
    print(f"\nVerifying fixed materials...")
    verify_stage = Usd.Stage.Open(str(output_path))

    all_ok = True
    for scope_path in materials_map.keys():
        scope = verify_stage.GetPrimAtPath(scope_path)
        if scope:
            for child in scope.GetChildren():
                if child.IsA(UsdShade.Material):
                    material = UsdShade.Material(child)
                    surface_output = material.GetSurfaceOutput()
                    if surface_output:
                        connections = surface_output.GetConnectedSources()
                        if connections and any(connections):
                            continue
                    print(f"  ✗ {child.GetPath()}: No surface connection!")
                    all_ok = False

    if all_ok:
        print("  ✓ All materials have proper surface connections!")

    print(f"\n{'='*80}")
    print("Test with: .\\Ruzino.exe ..\\..\\Assets\\OpenChessSet\\chess_set_fixed.usda")

    return 0 if all_ok else 1

if __name__ == "__main__":
    sys.exit(main())
