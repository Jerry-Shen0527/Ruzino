#!/usr/bin/env python3
"""
Batch Material Testing Script - Test multiple MaterialX materials with headless_render.exe

This script:
1. Finds all .mtlx materials in Assets/matx_library
2. For each material:
   - Binds it to shader_ball.usdc meshes
   - Renders using headless_render.exe
   - Saves output image
3. Reports success/failure statistics

Usage:
    python materials_batch.py          # Test first 10 materials
    python materials_batch.py 5        # Test first 5 materials
    python materials_batch.py all      # Test all materials
    python materials_batch.py -v       # Verbose mode with detailed output
    python materials_batch.py -j 4     # Use 4 worker processes
"""

import sys
import os
import glob
import subprocess
from pathlib import Path
from multiprocessing import Pool, cpu_count
import time
import json
from datetime import datetime

# Parse arguments
VERBOSE = "-v" in sys.argv or "--verbose" in sys.argv
if VERBOSE:
    sys.argv = [arg for arg in sys.argv if arg not in ["-v", "--verbose"]]

# Parse worker count
NUM_WORKERS = None
for i, arg in enumerate(sys.argv):
    if arg in ["-j", "--jobs"]:
        if i + 1 < len(sys.argv) and sys.argv[i + 1].isdigit():
            NUM_WORKERS = int(sys.argv[i + 1])
            sys.argv.pop(i + 1)
            sys.argv.pop(i)
        break
if NUM_WORKERS is None:
    NUM_WORKERS = cpu_count()

# Setup paths
script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Release"
assets_dir = workspace_root / "Assets"

# Set environment variables BEFORE importing USD
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(binary_dir)
mtlx_stdlib = binary_dir / "libraries"
if mtlx_stdlib.exists():
    os.environ["PXR_MTLX_STDLIB_SEARCH_PATHS"] = str(mtlx_stdlib)

sys.path.insert(0, str(binary_dir))

from pxr import Usd, UsdGeom, UsdShade, Sdf, UsdMtlx


def load_progress(progress_file):
    """Load completed materials from progress file"""
    if not progress_file.exists():
        return {"completed": [], "failed": [], "last_updated": None}

    try:
        with open(progress_file, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        print(f"Warning: Could not load progress file: {e}")
        return {"completed": [], "failed": [], "last_updated": None}


def save_progress(progress_file, completed, failed):
    """Save progress to JSON file"""
    progress_data = {
        "completed": completed,
        "failed": failed,
        "last_updated": datetime.now().isoformat(),
        "total_completed": len(completed),
        "total_failed": len(failed),
    }

    try:
        with open(progress_file, "w", encoding="utf-8") as f:
            json.dump(progress_data, f, indent=2, ensure_ascii=False)
    except Exception as e:
        print(f"Warning: Could not save progress: {e}")


def find_all_materials():
    """Find all .mtlx files in the matx_library"""
    matx_library = assets_dir / "matx_library"

    materials = []
    for mtlx_file in matx_library.rglob("*.mtlx"):
        materials.append(mtlx_file)

    materials.sort()
    return materials


def bind_material_to_shader_ball(
    shader_ball_path, material_path, output_path, verbose=False
):
    """Bind a MaterialX material to shader_ball meshes"""
    if verbose:
        print(f"  Opening stage: {shader_ball_path}")
    stage = Usd.Stage.Open(str(shader_ball_path))
    if not stage:
        if verbose:
            print(f"  ERROR: Failed to open stage")
        return False

    # Find meshes
    preview_mesh = stage.GetPrimAtPath("/root/Preview_Mesh/Preview_Mesh")
    calibration_mesh = stage.GetPrimAtPath("/root/Calibration_Mesh/Calibration_Mesh")

    if not preview_mesh or not calibration_mesh:
        if verbose:
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
    if verbose:
        print(f"  Opening MaterialX file: {material_path.name}")
    mtlx_stage = Usd.Stage.Open(str(material_path))
    if not mtlx_stage:
        if verbose:
            print(f"  ERROR: Failed to open MaterialX file")
        return False

    mtlx_material_path = None
    for prim in mtlx_stage.Traverse():
        if prim.IsA(UsdShade.Material):
            mtlx_material_path = prim.GetPath()
            if verbose:
                print(f"  Found material at: {mtlx_material_path}")
            break

    if not mtlx_material_path:
        if verbose:
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
            if verbose:
                print(f"  Connected material surface output to shader")
        elif verbose:
            print(f"  WARNING: Shader has no surface output")
    elif verbose:
        print(f"  WARNING: No shader found in material")

    # Bind material to meshes
    UsdShade.MaterialBindingAPI(preview_mesh).Bind(material)
    UsdShade.MaterialBindingAPI(calibration_mesh).Bind(material)

    if verbose:
        print(f"  Bound material to both meshes")
        # Verify bindings
        bound_mat_preview = UsdShade.MaterialBindingAPI(
            preview_mesh
        ).ComputeBoundMaterial()[0]
        bound_mat_calibration = UsdShade.MaterialBindingAPI(
            calibration_mesh
        ).ComputeBoundMaterial()[0]
        if bound_mat_preview:
            print(f"  Verified Preview_Mesh binding: {bound_mat_preview.GetPath()}")
        if bound_mat_calibration:
            print(
                f"  Verified Calibration_Mesh binding: {bound_mat_calibration.GetPath()}"
            )

    # Save
    stage.GetRootLayer().Export(str(output_path))
    if verbose:
        print(f"  Saved to: {output_path.name}")
    return True


def render_scene(
    usd_file, output_image, width=3000, height=3000, samples=64, verbose=False
):
    """Render a USD scene using headless_render.exe"""
    render_exe = binary_dir / "headless_render.exe"
    render_nodes = assets_dir / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json"

    # Use relative paths from binary directory
    usd_file_rel = os.path.relpath(usd_file, binary_dir)
    output_image_rel = os.path.relpath(output_image, binary_dir)
    render_nodes_rel = os.path.relpath(render_nodes, binary_dir)

    cmd = [
        str(render_exe),
        f"--usd={usd_file_rel}",
        f"--json={render_nodes_rel}",
        f"--output={output_image_rel}",
        f"--width={width}",
        f"--height={height}",
        f"--spp={samples}",
    ]

    if verbose:
        cmd.append("--verbose")
        print(f"  Command: {' '.join(cmd)}")
        print(f"  Working directory: {binary_dir}")

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=300, cwd=str(binary_dir)
        )
        if result.returncode != 0 and verbose:
            print(f"  ERROR: Render failed with code {result.returncode}")
            if result.stderr:
                print(f"  STDERR: {result.stderr[:200]}")
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        if verbose:
            print(f"  ERROR: Render timed out after 300s")
        return False
    except Exception as e:
        if verbose:
            print(f"  ERROR: {e}")
        return False


def test_single_material(args):
    """Test a single material (for multiprocessing)

    Args:
        args: tuple of (material_path, index, total_count, output_dir, shader_ball_path, progress_file, verbose)

    Returns:
        tuple: (material_name, success, elapsed_time)
    """
    (
        material_path,
        index,
        total_count,
        output_dir,
        shader_ball_path,
        progress_file,
        verbose,
    ) = args
    material_name = material_path.stem
    start_time = time.time()

    try:
        # Bind material
        modified_usd = output_dir / f"shader_ball_{material_name}.usdc"
        if not bind_material_to_shader_ball(
            shader_ball_path, material_path, modified_usd, verbose
        ):
            elapsed = time.time() - start_time
            print(
                f"[{index}/{total_count}] ✗ {material_name} - Binding FAILED ({elapsed:.1f}s)"
            )
            return (material_name, False, elapsed)

        # Render
        output_image = output_dir / f"{material_name}.png"
        if render_scene(modified_usd, output_image, verbose=verbose):
            elapsed = time.time() - start_time
            # Update progress file immediately
            if progress_file:
                progress = load_progress(progress_file)
                if material_name not in progress["completed"]:
                    progress["completed"].append(material_name)
                save_progress(progress_file, progress["completed"], progress["failed"])
            print(f"[{index}/{total_count}] ✓ {material_name} - OK ({elapsed:.1f}s)")
            return (material_name, True, elapsed)
        else:
            elapsed = time.time() - start_time
            # Update progress file for failed materials too
            if progress_file:
                progress = load_progress(progress_file)
                if material_name not in progress["failed"]:
                    progress["failed"].append(material_name)
                save_progress(progress_file, progress["completed"], progress["failed"])
            print(
                f"[{index}/{total_count}] ✗ {material_name} - Render FAILED ({elapsed:.1f}s)"
            )
            return (material_name, False, elapsed)

    except Exception as e:
        elapsed = time.time() - start_time
        # Update progress file for errors too
        if progress_file:
            progress = load_progress(progress_file)
            if material_name not in progress["failed"]:
                progress["failed"].append(material_name)
            save_progress(progress_file, progress["completed"], progress["failed"])
        print(
            f"[{index}/{total_count}] ✗ {material_name} - ERROR: {e} ({elapsed:.1f}s)"
        )
        return (material_name, False, elapsed)


def main():
    print("=" * 80)
    print("Batch Material Testing with headless_render.exe")
    print("=" * 80)
    print(f"Workers: {NUM_WORKERS}")
    if VERBOSE:
        print(f"Workspace: {workspace_root}")
        print(f"Binary dir: {binary_dir}")
        print(f"Assets dir: {assets_dir}")
        print(f"Verbose mode: ON")
    print("=" * 80)

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

    # Load progress
    progress_file = output_dir / "progress.json"
    progress = load_progress(progress_file)
    completed_materials = set(progress["completed"])
    failed_materials = set(progress["failed"])

    if completed_materials or failed_materials:
        print(
            f"Loaded progress: {len(completed_materials)} completed, {len(failed_materials)} failed"
        )
        if progress["last_updated"]:
            print(f"Last updated: {progress['last_updated']}")
        print()

    # Determine test count from arguments
    test_count = 10  # Default
    if len(sys.argv) > 1:
        arg = sys.argv[1]
        if arg.lower() == "all":
            test_count = len(materials)
        elif arg.isdigit():
            test_count = min(int(arg), len(materials))
    else:
        test_count = min(10, len(materials))

    # Test materials with multiprocessing
    shader_ball = assets_dir / "shader_ball.usdc"

    # Filter out already completed materials
    test_materials = materials[:test_count]
    pending_materials = [
        mat_path
        for mat_path in test_materials
        if mat_path.stem not in completed_materials
    ]
    skipped_count = len(test_materials) - len(pending_materials)

    if skipped_count > 0:
        print(f"Skipping {skipped_count} already completed materials\n")

    if not pending_materials:
        print("All materials already completed!")
        print("\n" + "=" * 80)
        print("RESULTS")
        print("=" * 80)
        print(f"Total tested:  {test_count}")
        print(f"Succeeded:     {len(completed_materials)}")
        print(f"Failed:        {len(failed_materials)}")
        print(f"Success rate:  {len(completed_materials)/test_count*100:.1f}%")
        print("=" * 80)
        return 0 if len(failed_materials) == 0 else 1

    print(
        f"\nTesting {len(pending_materials)} remaining materials with {NUM_WORKERS} workers...\n"
    )
    print("=" * 80)

    # Prepare arguments for multiprocessing
    task_args = [
        (mat_path, i + 1, test_count, output_dir, shader_ball, progress_file, VERBOSE)
        for i, mat_path in enumerate(pending_materials)
    ]

    # Run tests in parallel
    start_time = time.time()
    with Pool(processes=NUM_WORKERS) as pool:
        test_results = pool.map(test_single_material, task_args)
    total_elapsed = time.time() - start_time

    # Count results from this run
    new_success = sum(1 for _, success, _ in test_results if success)
    new_fail = len(test_results) - new_success
    avg_time = (
        sum(elapsed for _, _, elapsed in test_results) / len(test_results)
        if test_results
        else 0
    )

    # Reload final progress to get total counts
    final_progress = load_progress(progress_file)
    total_success = len(final_progress["completed"])
    total_fail = len(final_progress["failed"])

    # Summary
    print("\n" + "=" * 80)
    print("RESULTS")
    print("=" * 80)
    print(f"This run:")
    print(f"  Tested:      {len(pending_materials)}")
    print(f"  Succeeded:   {new_success}")
    print(f"  Failed:      {new_fail}")
    print(f"  Time:        {total_elapsed:.1f}s ({total_elapsed/60:.1f}m)")
    print(f"  Avg/mat:     {avg_time:.1f}s")
    print()
    print(f"Overall (out of first {test_count} materials):")
    print(f"  Completed:   {total_success}")
    print(f"  Failed:      {total_fail}")
    print(f"  Pending:     {test_count - total_success - total_fail}")
    print(f"  Success rate: {total_success/test_count*100:.1f}%")
    print()
    print(f"Progress saved to: {progress_file.name}")
    print("=" * 80)

    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
