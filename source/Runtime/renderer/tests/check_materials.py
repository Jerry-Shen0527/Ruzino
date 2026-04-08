"""Quick script to check material surface connections"""
import sys
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
workspace_root = os.path.abspath(os.path.join(script_dir, "..", "..", "..", ".."))
binary_dir = os.path.join(workspace_root, "Binaries", "Release")

os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir
sys.path.insert(0, binary_dir)
os.chdir(binary_dir)

from pxr import Usd, UsdShade

stage = Usd.Stage.Open(r'..\..\Assets\OpenChessSet\chess_set_fixed.usda')

# Check a few materials
test_paths = [
    '/ChessSet/Black/King/Materials/M_King_B',
    '/ChessSet/White/Knight L/Materials/M_Knight_W',
    '/ChessSet/Chessboard/Materials/M_Chessboard'
]

for path in test_paths:
    print(f"\nChecking: {path}")
    prim = stage.GetPrimAtPath(path)
    if not prim:
        print(f"  ERROR: Prim not found!")
        continue

    print(f"  Type: {prim.GetTypeName()}")
    print(f"  Valid: {prim.IsValid()}")

    material = UsdShade.Material(prim)
    if material:
        surface_output = material.GetSurfaceOutput()
        if surface_output:
            connections = surface_output.GetConnectedSources()
            print(f"  Surface output exists: {len(connections)} connections")
            for conn in connections:
                print(f"    - {conn}")
        else:
            print(f"  NO surface output!")
    else:
        print(f"  Not a valid Material!")
