#!/usr/bin/env python3
"""
Batch Material Testing - Test multiple MaterialX materials with headless_render.exe

This script:
1. Finds all .mtlx materials in Assets/matx_library
2. For each material:
   - Binds it to shader_ball.usdc meshes
   - Renders using headless_render.exe
   - Saves output image
3. Reports success/failure statistics

Usage:
    python test_materials_batch.py          # Test first 10 materials
    python test_materials_batch.py 5        # Test first 5 materials
    python test_materials_batch.py all      # Test all materials
    python test_materials_batch.py -v       # Verbose mode with detailed output
"""

import sys
import os
import glob
import subprocess
from pathlib import Path

# Parse arguments
VERBOSE = '-v' in sys.argv or '--verbose' in sys.argv
if VERBOSE:
    sys.argv = [arg for arg in sys.argv if arg not in ['-v', '--verbose']]

# Setup paths
script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Release"
assets_dir = workspace_root / "Assets"

# Set environment variables BEFORE importing USD
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = str(binary_dir)
mtlx_stdlib = binary_dir / "libraries"
if mtlx_stdlib.exists():
    os.environ['PXR_MTLX_STDLIB_SEARCH_PATHS'] = str(mtlx_stdlib)

sys.path.insert(0, str(binary_dir))

from pxr import Usd, UsdGeom, UsdShade, Sdf, UsdMtlx

def find_all_materials():
    """Find all .mtlx files in the matx_library"""
    matx_library = assets_dir / "matx_library"

    materials = []
    for mtlx_file in matx_library.rglob("*.mtlx"):
        materials.append(mtlx_file)

    materials.sort()
    return materials

def bind_material_to_shader_ball(shader_ball_path, material_path, output_path):
    """Bind a MaterialX material to shader_ball meshes"""
    if VERBOSE:
        print(f"  Opening stage: {shader_ball_path}")
    stage = Usd.Stage.Open(str(shader_ball_path))
    if not stage:
        if VERBOSE:
            print(f"  ERROR: Failed to open stage")
        return False

    # Find meshes
    preview_mesh = stage.GetPrimAtPath("/root/Preview_Mesh/Preview_Mesh")
    calibration_mesh = stage.GetPrimAtPath("/root/Calibration_Mesh/Calibration_Mesh")

    if not preview_mesh or not calibration_mesh:
        if VERBOSE:
            print(f"  ERROR: Failed to find meshes in stage")
        return False

    # Create materials scope
    materials_scope_path = "/root/_materials"
    materials_scope = stage.GetPrimAtPath(materials_scope_path)
    if not materials_scope:
        materials_scope = stage.DefinePrim(materials_scope_path, "Scope")

    # Get material name
    material_name = material_path.stem
    material_path_in_stage = f"{materials_scope_path}/{material_name}"

    # Remove old material if exists
    if stage.GetPrimAtPath(material_path_in_stage):
        stage.RemovePrim(material_path_in_stage)

    # Open MaterialX file to find material
    if VERBOSE:
        print(f"  Opening MaterialX file: {material_path.name}")
    mtlx_stage = Usd.Stage.Open(str(material_path))
    if not mtlx_stage:
        if VERBOSE:
            print(f"  ERROR: Failed to open MaterialX file")
        return False

    mtlx_material_path = None
    for prim in mtlx_stage.Traverse():
        if prim.IsA(UsdShade.Material):
            mtlx_material_path = prim.GetPath()
            if VERBOSE:
                print(f"  Found material at: {mtlx_material_path}")
            break

    if not mtlx_material_path:
        if VERBOSE:
            print(f"  ERROR: No material found in MaterialX file")
        return False

    # Create material prim and reference the .mtlx file
    material_prim = stage.DefinePrim(material_path_in_stage, "Material")
    material_prim.GetReferences().AddReference(str(material_path), mtlx_material_path)
    material = UsdShade.Material(material_prim)

    # Connect surface output
    shader_prim = None
    for child in material_prim.GetChildren():
        if child.IsA(UsdShade.Shader):
            shader_prim = child
            break

    if shader_prim:
        shader = UsdShade.Shader(shader_prim)
        shader_surface_output = shader.GetOutput("surface")
        if shader_surface_output:
            material_surface_output = material.CreateSurfaceOutput()
            material_surface_output.ConnectToSource(shader_surface_output)
            if VERBOSE:
                print(f"  Connected material surface output to shader")
        elif VERBOSE:
            print(f"  WARNING: Shader has no surface output")
    elif VERBOSE:
        print(f"  WARNING: No shader found in material")

    # Bind material to meshes
    UsdShade.MaterialBindingAPI(preview_mesh).Bind(material)
    UsdShade.MaterialBindingAPI(calibration_mesh).Bind(material)

    if VERBOSE:
        print(f"  Bound material to both meshes")
        # Verify bindings
        bound_mat_preview = UsdShade.MaterialBindingAPI(preview_mesh).ComputeBoundMaterial()[0]
        bound_mat_calibration = UsdShade.MaterialBindingAPI(calibration_mesh).ComputeBoundMaterial()[0]
        if bound_mat_preview:
            print(f"  Verified Preview_Mesh binding: {bound_mat_preview.GetPath()}")
        if bound_mat_calibration:
            print(f"  Verified Calibration_Mesh binding: {bound_mat_calibration.GetPath()}")

    # Save
    stage.GetRootLayer().Export(str(output_path))
    if VERBOSE:
        print(f"  Saved to: {output_path.name}")
    return True

def render_scene(usd_file, output_image, width=1920, height=1080, samples=4):
    """Render a USD scene using headless_render.exe"""
    render_exe = binary_dir / "headless_render.exe"
    render_nodes = assets_dir / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"

    # Use relative paths from binary directory
    usd_file_rel = os.path.relpath(usd_file, binary_dir)
    output_image_rel = os.path.relpath(output_image, binary_dir)
    render_nodes_rel = os.path.relpath(render_nodes, binary_dir)

    cmd = [
        str(render_exe),
        usd_file_rel,
        render_nodes_rel,
        output_image_rel,
        str(width),
        str(height),
        str(samples)
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,
            cwd=str(binary_dir)
        )
        if result.returncode != 0 and VERBOSE:
            print(f"  ERROR: Render failed with code {result.returncode}")
            if result.stderr:
                print(f"  STDERR: {result.stderr[:200]}")
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        if VERBOSE:
            print(f"  ERROR: Render timed out after 300s")
        return False
    except Exception as e:
        if VERBOSE:
            print(f"  ERROR: {e}")
        return False

def main():
    print("="*80)
    print("Batch Material Testing with headless_render.exe")
    print("="*80)
    if VERBOSE:
        print(f"Workspace: {workspace_root}")
        print(f"Binary dir: {binary_dir}")
        print(f"Assets dir: {assets_dir}")
        print(f"Verbose mode: ON\n")

    # Find all materials
    materials = find_all_materials()
    print(f"Found {len(materials)} material files\n")

    if not materials:
        print("No materials found!")
        return 1

    # Create output directory
    output_dir = binary_dir / "material_tests"
    output_dir.mkdir(exist_ok=True)
    print(f"Output directory: {output_dir}\n")

    # Determine test count from arguments
    test_count = 10  # Default
    if len(sys.argv) > 1:
        arg = sys.argv[1]
        if arg.lower() == 'all':
            test_count = len(materials)
        elif arg.isdigit():
            test_count = min(int(arg), len(materials))
    else:
        test_count = min(10, len(materials))

    # Test materials
    shader_ball = assets_dir / "shader_ball.usdc"

    print(f"Testing first {test_count} materials...\n")
    print("="*80)

    success_count = 0
    fail_count = 0

    for i, material_path in enumerate(materials[:test_count]):
        material_name = material_path.stem
        print(f"\n[{i+1}/{test_count}] {material_name}")
        print(f"  Material: {material_path.name}")

        # Bind material
        modified_usd = output_dir / f"shader_ball_{material_name}.usdc"
        print(f"  Binding material...", end=" ")
        if not bind_material_to_shader_ball(shader_ball, material_path, modified_usd):
            print("FAILED")
            fail_count += 1
            continue
        print("OK")

        # Render
        output_image = output_dir / f"{material_name}.png"
        print(f"  Rendering...", end=" ")
        if render_scene(modified_usd, output_image):
            print(f"OK")
            print(f"  ✓ Saved: {output_image.name}")
            success_count += 1
        else:
            print("FAILED")
            fail_count += 1

    # Summary
    print("\n" + "="*80)
    print("RESULTS")
    print("="*80)
    print(f"Total tested:  {test_count}")
    print(f"Succeeded:     {success_count}")
    print(f"Failed:        {fail_count}")
    print(f"Success rate:  {success_count/test_count*100:.1f}%")
    print("="*80)

    return 0 if fail_count == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
