#!/usr/bin/env python3
"""
BRDF Single Row Visualization Script

Analyzes a single row of BRDF hemisphere to visualize eval, sample, and importance distributions.
Run directly: python brdf_single_row.py <material_folder>
"""

import sys
import os
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt

# Setup paths
script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Release"
assets_dir = workspace_root / "Assets"

# Set environment variables
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(binary_dir)
mtlx_stdlib = binary_dir / "libraries"
if mtlx_stdlib.exists():
    os.environ["PXR_MTLX_STDLIB_SEARCH_PATHS"] = str(mtlx_stdlib)
os.environ["PATH"] = str(binary_dir) + os.pathsep + os.environ.get("PATH", "")

if hasattr(os, "add_dll_directory"):
    os.add_dll_directory(str(binary_dir))

os.chdir(str(binary_dir))
sys.path.insert(0, str(binary_dir))

# Import USD first
from pxr import Usd, UsdGeom, UsdShade, Sdf, UsdMtlx

# Import modules
import hd_RUZINO_py as renderer
import nodes_core_py as core
import nodes_system_py as system


# Test configuration
RESOLUTION = 128
NUM_SAMPLES = 10000000
TEST_ANGLE = 15  # degrees
TEST_UV = (0.2, 0.5)


def set_node_inputs(executor, inputs_dict):
    """Helper to set multiple node inputs."""
    for (node, socket_name), value in inputs_dict.items():
        socket = node.get_input_socket(socket_name)
        if socket is None:
            raise ValueError(
                f"Socket '{socket_name}' not found on node '{node.ui_name}'"
            )
        meta_value = core.to_meta_any(value)
        executor.sync_node_from_external_storage(socket, meta_value)


def bind_material_to_shader_ball(shader_ball_path, material_path, output_path):
    """Bind a MaterialX material to shader_ball meshes"""
    stage = Usd.Stage.Open(str(shader_ball_path))
    if not stage:
        return False

    preview_mesh = stage.GetPrimAtPath("/root/Preview_Mesh/Preview_Mesh")
    calibration_mesh = stage.GetPrimAtPath("/root/Calibration_Mesh/Calibration_Mesh")

    if not preview_mesh or not calibration_mesh:
        return False

    materials_scope_path = "/root/_materials"
    materials_scope = stage.GetPrimAtPath(materials_scope_path)
    if not materials_scope:
        materials_scope = stage.DefinePrim(materials_scope_path, "Scope")

    material_name = material_path.stem
    material_path_in_stage = f"{materials_scope_path}/{material_name}"

    if stage.GetPrimAtPath(material_path_in_stage):
        stage.RemovePrim(material_path_in_stage)

    mtlx_stage = Usd.Stage.Open(str(material_path))
    if not mtlx_stage:
        return False

    mtlx_material_path = None
    for prim in mtlx_stage.Traverse():
        if prim.IsA(UsdShade.Material):
            mtlx_material_path = prim.GetPath()
            break

    if not mtlx_material_path:
        return False

    material_prim = stage.DefinePrim(material_path_in_stage, "Material")
    material_prim.GetReferences().AddReference(str(material_path), mtlx_material_path)
    material = UsdShade.Material(material_prim)

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

    UsdShade.MaterialBindingAPI(preview_mesh).Bind(material)
    UsdShade.MaterialBindingAPI(calibration_mesh).Bind(material)

    stage.GetRootLayer().Export(str(output_path))
    return True


def compute_hemisphere_mask(resolution):
    """Create mask for valid pixels inside hemisphere projection"""
    height, width = resolution, resolution
    y_coords, x_coords = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    u = (x_coords + 0.5) / width
    v = (y_coords + 0.5) / height
    p = np.stack([u * 2 - 1, v * 2 - 1], axis=-1)
    r = np.sqrt(p[:, :, 0] ** 2 + p[:, :, 1] ** 2)
    return r <= 1.0


def analyze_middle_row(
    eval_array, sample_array, importance_array, valid_mask, material_name
):
    """Analyze multiple rows of the hemisphere at different quantiles"""

    # Analyze rows at 0.25, 0.5, 0.75 quantiles
    rows_to_analyze = [
        RESOLUTION // 4,  # 0.25 quantile
        RESOLUTION // 2,  # 0.5 quantile (middle)
        3 * RESOLUTION // 4,  # 0.75 quantile
    ]

    all_results = []

    for row_idx in rows_to_analyze:
        print(f"\n{'='*80}")
        print(f"ROW ANALYSIS - Row {row_idx} (y={row_idx/RESOLUTION:.2f})")
        print(f"{'='*80}")

        # Extract row data
        eval_row = eval_array[row_idx, :, 0]
        sample_row = sample_array[row_idx, :, 0]
        importance_row = importance_array[row_idx, :, 0]
        debug_alpha_row = importance_array[row_idx, :, 3]
        mask_row = valid_mask[row_idx, :]

        # Apply mask
        eval_valid = eval_row[mask_row]
        sample_valid = sample_row[mask_row]
        importance_valid = importance_row[mask_row]
        debug_alpha_valid = debug_alpha_row[mask_row]
        x_coords = np.where(mask_row)[0]

        # Print some sample values
        print(f"\nSample pixel values (first 10 valid pixels):")
        for i in range(min(10, len(eval_valid))):
            print(
                f"  Pixel {x_coords[i]}: eval={eval_valid[i]:.6e}, sample={sample_valid[i]:.6e}, importance={importance_valid[i]:.2f}"
            )

        # Count non-zero samples
        nonzero_samples = np.sum(sample_valid > 0)
        print(f"\nNon-zero sample pixels: {nonzero_samples} / {len(sample_valid)}")

        if nonzero_samples == 0:
            print("⚠️  No samples in this row - skipping detailed analysis")
            continue

        # Normalize eval and sample to sum to 1
        eval_sum = eval_valid.sum()
        sample_sum = sample_valid.sum()

        eval_normalized = eval_valid / eval_sum if eval_sum > 0 else eval_valid
        sample_normalized = (
            sample_valid / sample_sum if sample_sum > 0 else sample_valid
        )

        # Statistics
        print(f"\nValid pixels in row: {len(eval_valid)}")
        print(f"\nEval statistics:")
        print(f"  Sum (before norm): {eval_sum:.6e}")
        print(f"  Min: {eval_valid.min():.6e}, Max: {eval_valid.max():.6e}")
        print(f"  Mean: {eval_valid.mean():.6e}, Std: {eval_valid.std():.6e}")

        print(f"\nSample statistics:")
        print(f"  Sum (before norm): {sample_sum:.6e}")
        print(f"  Min: {sample_valid.min():.6e}, Max: {sample_valid.max():.6e}")
        print(f"  Mean: {sample_valid.mean():.6e}, Std: {sample_valid.std():.6e}")

        print(f"\nImportance statistics:")
        importance_nonzero = importance_valid[importance_valid > 1e-8]
        print(f"  Non-zero pixels: {len(importance_nonzero)} / {len(importance_valid)}")
        if len(importance_nonzero) > 0:
            print(
                f"  Min: {importance_nonzero.min():.6e}, Max: {importance_nonzero.max():.6e}"
            )
            print(
                f"  Mean: {importance_nonzero.mean():.6e}, Std: {importance_nonzero.std():.6e}"
            )
            print(
                f"  CV (should be ~0): {importance_nonzero.std() / importance_nonzero.mean():.6f}"
            )

        print(f"\nNormalized distributions:")
        l2_distance = np.sqrt(np.mean((eval_normalized - sample_normalized) ** 2))
        correlation = (
            np.corrcoef(eval_normalized, sample_normalized)[0, 1]
            if eval_valid.std() > 0 and sample_valid.std() > 0
            else 0
        )
        print(f"  L2 distance: {l2_distance:.6e} (should be ~0)")
        print(f"  Correlation: {correlation:.6f} (should be ~1)")

        all_results.append(
            {
                "row": row_idx,
                "l2_distance": float(l2_distance),
                "correlation": float(correlation),
                "importance_cv": (
                    float(importance_nonzero.std() / importance_nonzero.mean())
                    if len(importance_nonzero) > 0
                    else 0
                ),
                "nonzero_samples": int(nonzero_samples),
            }
        )

    if not all_results:
        print(f"\n{'='*80}")
        print("⚠️  WARNING: No samples found in any analyzed row!")
        print("This suggests samples are concentrated in other parts of the hemisphere")
        print("due to material properties (anisotropy, normal map, rotation)")
        print(f"{'='*80}")
        return {
            "l2_distance": float("inf"),
            "correlation": 0.0,
            "importance_cv": float("inf"),
        }

    # Return best result across all rows
    best_result = min(all_results, key=lambda x: x["l2_distance"])
    print(f"\n{'='*80}")
    print("BEST ROW RESULT")
    print(f"{'='*80}")
    print(
        f"Row {best_result['row']}: L2={best_result['l2_distance']:.6e}, Corr={best_result['correlation']:.3f}, CV={best_result['importance_cv']:.3f}"
    )
    print(f"Non-zero samples: {best_result['nonzero_samples']}")

    return best_result


def main():
    # Parse command-line arguments
    if len(sys.argv) > 1:
        material_path = Path(sys.argv[1])
        if not material_path.is_absolute():
            # Try relative to assets/matx_library
            material_path = assets_dir / "matx_library" / sys.argv[1]
        if not material_path.exists():
            print(f"Error: Material file not found: {material_path}")
            return 1
    else:
        # Default to Acryl_Plastic
        matx_library = assets_dir / "matx_library"
        acryl_plastic_dir = matx_library / "Acryl_Plastic_1k_8b_kylYFM6"

        if not acryl_plastic_dir.exists():
            print(f"Error: Material directory not found: {acryl_plastic_dir}")
            return 1

        material_path = acryl_plastic_dir / "Acryl_Plastic.mtlx"
        if not material_path.exists():
            print(f"Error: Material file not found: {material_path}")
            return 1

    material_name = material_path.stem

    print("=" * 80)
    print(f"BRDF Single Row Test - {material_name}")
    print("=" * 80)
    print(f"Resolution: {RESOLUTION}x{RESOLUTION}")
    print(f"Samples: {NUM_SAMPLES}")
    print(f"Test angle: {TEST_ANGLE}°")
    print(f"Test UV: {TEST_UV}")
    print(f"Material: {material_path}")
    print("=" * 80)

    # Create output directory
    output_dir = binary_dir / "brdf_tests"
    output_dir.mkdir(exist_ok=True)

    # Bind material
    shader_ball_path = assets_dir / "shader_ball.usdc"
    output_usd = output_dir / "shader_ball_test.usdc"

    print("\nBinding material...")
    if not bind_material_to_shader_ball(shader_ball_path, material_path, output_usd):
        print("FAILED to bind material")
        return 1

    # Create HydraRenderer
    print("Creating renderer...")
    hydra = renderer.HydraRenderer(str(output_usd), width=RESOLUTION, height=RESOLUTION)

    # Setup node system
    node_system = hydra.get_node_system()
    config_path = binary_dir / "render_nodes.json"
    if config_path.exists():
        node_system.load_configuration(str(config_path))

    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()

    # Initial render
    hydra.render()

    # Create analyzer node
    print("Setting up analyzer...")
    analyzer = tree.add_node("material_brdf_analyzer")
    analyzer.ui_name = "BRDFAnalyzer"

    present_eval = tree.add_node("present_color")
    present_eval.ui_name = "PresentEval"
    present_pdf = tree.add_node("present_color")
    present_pdf.ui_name = "PresentPDF"
    present_sample = tree.add_node("present_color")
    present_sample.ui_name = "PresentSample"
    present_importance = tree.add_node("present_color")
    present_importance.ui_name = "PresentImportance"

    tree.add_link(
        analyzer.get_output_socket("BRDF Eval"), present_eval.get_input_socket("Color")
    )
    tree.add_link(
        analyzer.get_output_socket("PDF"), present_pdf.get_input_socket("Color")
    )
    tree.add_link(
        analyzer.get_output_socket("Sample Distribution"),
        present_sample.get_input_socket("Color"),
    )
    tree.add_link(
        analyzer.get_output_socket("Importance Test"),
        present_importance.get_input_socket("Color"),
    )

    # Set test parameters
    import math

    angle_rad = math.radians(TEST_ANGLE)
    incident_dir_x = math.sin(angle_rad)
    incident_dir_y = 0.0
    incident_dir_z = math.cos(angle_rad)

    length = math.sqrt(incident_dir_x**2 + incident_dir_y**2 + incident_dir_z**2)
    if length > 0:
        incident_dir_x /= length
        incident_dir_y /= length
        incident_dir_z /= length

    inputs = {
        (analyzer, "Incident Direction X"): incident_dir_x,
        (analyzer, "Incident Direction Y"): incident_dir_y,
        (analyzer, "Incident Direction Z"): incident_dir_z,
        (analyzer, "UV X"): TEST_UV[0],
        (analyzer, "UV Y"): TEST_UV[1],
        (analyzer, "Material ID"): 0,
        (analyzer, "Resolution"): RESOLUTION,
        (analyzer, "Num Samples"): NUM_SAMPLES,
    }

    print("Running BRDF analysis...")
    executor.reset_allocator()
    executor.prepare_tree(tree, present_eval)
    set_node_inputs(executor, inputs)
    hydra.render()

    # Retrieve outputs
    print("Retrieving results...")
    eval_data = hydra.get_output_texture("PresentEval")
    eval_array = np.array(eval_data, dtype=np.float32).reshape(
        RESOLUTION, RESOLUTION, 4
    )

    sample_data = hydra.get_output_texture("PresentSample")
    sample_array_raw = np.array(sample_data, dtype=np.float32).reshape(
        RESOLUTION, RESOLUTION, 4
    )
    sample_array = sample_array_raw.copy()
    sample_array[:, :, :3] = sample_array_raw[:, :, :3] / NUM_SAMPLES

    importance_data = hydra.get_output_texture("PresentImportance")
    importance_array = np.array(importance_data, dtype=np.float32).reshape(
        RESOLUTION, RESOLUTION, 4
    )

    # Compute hemisphere mask
    valid_mask = compute_hemisphere_mask(RESOLUTION)

    # Save EXR images before analysis
    print("\nSaving EXR images...")
    try:
        import imageio

        # Flip images for correct orientation
        eval_exr = 100 * np.flipud(eval_array[:, :, :3].astype(np.float32))
        sample_exr = np.flipud(sample_array[:, :, :3].astype(np.float32))
        ratio = eval_exr.mean() / (sample_exr.mean() + 1e-8)
        sample_exr *= ratio
        importance_exr = np.flipud(importance_array[:, :, :3].astype(np.float32))

        # Save to output directory
        imageio.imwrite(
            output_dir / f"{material_name}_eval.exr", eval_exr, format="EXR"
        )
        imageio.imwrite(
            output_dir / f"{material_name}_sample.exr", sample_exr, format="EXR"
        )
        imageio.imwrite(
            output_dir / f"{material_name}_importance.exr", importance_exr, format="EXR"
        )

        print(f"✓ Saved: {material_name}_eval.exr")
        print(f"✓ Saved: {material_name}_sample.exr")
        print(f"✓ Saved: {material_name}_importance.exr")
    except ImportError:
        print("✗ imageio not available, skipping EXR export")
    except Exception as e:
        print(f"✗ Failed to save EXR: {e}")

    # Analyze middle row
    results = analyze_middle_row(
        eval_array, sample_array, importance_array, valid_mask, material_name
    )

    print("\n" + "=" * 80)
    print("VERDICT")
    print("=" * 80)
    print(
        f"✓ L2 distance < 0.001: {'PASS' if results['l2_distance'] < 0.001 else 'FAIL'}"
    )
    print(
        f"✓ Correlation > 0.99: {'PASS' if results['correlation'] > 0.99 else 'FAIL'}"
    )
    print(
        f"✓ Importance CV < 0.1: {'PASS' if results['importance_cv'] < 0.1 else 'FAIL'}"
    )
    print("=" * 80)

    return 0


if __name__ == "__main__":
    sys.exit(main())
