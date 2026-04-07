"""
Manually convert a MaterialX file to proper USD Material with shader network
"""
import sys
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
workspace_root = os.path.abspath(os.path.join(script_dir, "..", "..", "..", ".."))
binary_dir = os.path.join(workspace_root, "Binaries", "Release")

os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir
sys.path.insert(0, binary_dir)
os.chdir(binary_dir)

from pxr import Usd, UsdShade, Sdf, UsdGeom, UsdMtlx

def create_test_material():
    """Create a simple test USD file with a working material"""
    output_path = os.path.join(binary_dir, "chess_test_material.usda")

    stage = Usd.Stage.CreateNew(output_path)

    # Create a simple mesh
    mesh_prim = stage.DefinePrim("/TestMesh", "Mesh")
    mesh = UsdGeom.Mesh(mesh_prim)

    # Set simple geometry
    mesh.CreatePointsAttr([(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateExtentAttr([(-1, 0, -1), (1, 0, 1)])

    # Create material with UsdPreviewSurface
    material_path = "/Materials/TestMaterial"
    material_prim = stage.DefinePrim(material_path, "Material")
    material = UsdShade.Material(material_prim)

    # Create shader
    shader_path = material_path + "/PreviewSurface"
    shader_prim = stage.DefinePrim(shader_path, "Shader")
    shader = UsdShade.Shader(shader_prim)
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((0.8, 0.2, 0.2))
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.5)
    shader.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(0.0)

    # Connect shader to material surface
    surface_output = shader.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput().ConnectToSource(surface_output)

    # Bind material to mesh
    UsdShade.MaterialBindingAPI(mesh_prim).Bind(material)

    stage.Save()
    print(f"Created test file: {output_path}")
    print("This file has a proper material with connected shader network")

    return output_path

def try_convert_mtlx():
    """Try to use UsdMtlx to convert MaterialX"""
    mtlx_path = os.path.join(workspace_root, "Assets", "OpenChessSet", "assets", "King", "King_mat.mtlx")
    output_path = os.path.join(binary_dir, "king_material_converted.usda")

    print(f"\nTrying to convert {mtlx_path}")

    try:
        # This may not work depending on USD build configuration
        stage = Usd.Stage.CreateNew(output_path)
        result = UsdMtlx.ReadMtlx(mtlx_path, stage)
        if result:
            stage.Save()
            print(f"Converted to: {output_path}")
        else:
            print("UsdMtlx.ReadMtlx failed")
    except Exception as e:
        print(f"Error during MaterialX conversion: {e}")

if __name__ == "__main__":
    test_file = create_test_material()
    print(f"\nTest this file in Ruzino: .\\Ruzino.exe {test_file}")

    try_convert_mtlx()
