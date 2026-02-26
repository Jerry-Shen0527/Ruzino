#!/usr/bin/env python3
"""
Comprehensive BRDF Testing Script

Tests all MaterialX materials with structured JSON logging:
1. Normalized eval vs normalized sample comparison
2. Importance test variance (non-zero pixels, mean-normalized)
3. Multiple UV positions for textured materials
4. Multiple incident light directions

Run directly: python brdf_comprehensive.py <material_folder>
"""

import sys
import os
from pathlib import Path
import numpy as np
import json
from datetime import datetime
import math

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
NUM_SAMPLES = 1000000  # 100K samples for reasonable speed
TEST_UV_POSITIONS = [(0.3, 0.3), (0.5, 0.5), (0.7, 0.7)]  # Three UV positions
TEST_ANGLES = [5, 35, 65]  # Three incident angles in degrees


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


def has_texture_maps(material_dir):
    """Check if material directory contains PNG texture files"""
    png_files = list(material_dir.glob("*.png"))
    return len(png_files) > 0


def find_material_in_folder(material_folder):
    """Find .mtlx file in the given material folder"""
    material_folder = Path(material_folder)
    mtlx_files = list(material_folder.glob("*.mtlx"))
    if not mtlx_files:
        return None
    return mtlx_files[0]  # Return first .mtlx file found


def compute_hemisphere_mask(resolution):
    """Create mask for valid pixels inside hemisphere projection"""
    height, width = resolution, resolution
    y_coords, x_coords = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    u = (x_coords + 0.5) / width
    v = (y_coords + 0.5) / height
    p = np.stack([u * 2 - 1, v * 2 - 1], axis=-1)
    r = np.sqrt(p[:, :, 0] ** 2 + p[:, :, 1] ** 2)
    return r <= 1.0


def save_texture_exr(array, filename):
    """Save texture as EXR format with HDR values, removing inf/nan"""
    rgb = array[:, :, :3].copy().astype(np.float32)
    
    # Replace inf and nan with 0 - check each condition separately
    rgb[np.isnan(rgb)] = 0.0
    rgb[np.isinf(rgb)] = 0.0
    
    # Additional safety: clamp extremely large values
    rgb = np.clip(rgb, -1e10, 1e10)
    
    rgb_exr = np.flipud(rgb)
    try:
        import imageio
        imageio.imwrite(filename, rgb_exr, format="EXR")
        return True
    except Exception as e:
        print(f"    WARNING: Could not save EXR {filename}: {e}")
        return False


def analyze_brdf_test(eval_array, sample_array, importance_array, valid_mask):
    """
    Analyze BRDF test results
    
    Returns:
        dict with keys:
        - eval_sample_similarity: normalized L2 distance between normalized eval and sample
        - importance_variance: variance of mean-normalized non-zero importance pixels
        - eval_stats: percentiles of eval values
        - sample_stats: percentiles of sample values
        - importance_stats: percentiles of importance values
    """
    # Extract single channel (stored as RGB with same values)
    eval_values = eval_array[:, :, 0][valid_mask]
    sample_values = sample_array[:, :, 0][valid_mask]
    importance_values = importance_array[:, :, 0][valid_mask]
    
    # 1. Normalize eval and sample distributions to sum to 1
    eval_sum = eval_values.sum()
    sample_sum = sample_values.sum()
    
    if eval_sum > 0:
        eval_normalized = eval_values / eval_sum
    else:
        eval_normalized = eval_values
    
    if sample_sum > 0:
        sample_normalized = sample_values / sample_sum
    else:
        sample_normalized = sample_values
    
    # Compute L2 distance between normalized distributions
    eval_sample_l2 = np.sqrt(np.mean((eval_normalized - sample_normalized) ** 2))
    
    # Compute correlation
    if eval_values.std() > 0 and sample_values.std() > 0:
        eval_sample_corr = np.corrcoef(eval_values, sample_values)[0, 1]
    else:
        eval_sample_corr = 0.0
    
    # 2. Importance test variance (non-zero pixels, mean-normalized)
    importance_nonzero = importance_values[importance_values > 1e-8]
    if len(importance_nonzero) > 0:
        importance_mean = importance_nonzero.mean()
        # Normalize to mean=0 by subtracting mean
        importance_centered = importance_nonzero - importance_mean
        importance_variance = importance_centered.var()
        importance_cv = importance_nonzero.std() / (importance_mean + 1e-8)
    else:
        importance_variance = 0.0
        importance_cv = 0.0
    
    return {
        "eval_sample_l2_distance": float(eval_sample_l2),
        "eval_sample_correlation": float(eval_sample_corr),
        "importance_variance": float(importance_variance),
        "importance_cv": float(importance_cv),
        "eval_percentiles": [float(x) for x in np.percentile(eval_values, [0, 25, 50, 75, 95, 100])],
        "sample_percentiles": [float(x) for x in np.percentile(sample_values, [0, 25, 50, 75, 95, 100])],
        "importance_percentiles": [float(x) for x in np.percentile(importance_nonzero, [0, 25, 50, 75, 95, 100])] if len(importance_nonzero) > 0 else [0, 0, 0, 0, 0, 0],
        "num_valid_pixels": int(len(eval_values)),
        "num_importance_nonzero": int(len(importance_nonzero))
    }


def test_material_at_config(hydra, tree, executor, analyzer, present_eval, present_pdf, present_sample, present_importance, 
                            incident_angle, uv_x, uv_y, valid_mask, material_name, output_dir):
    """
    Test material at specific configuration (angle, UV position)
    
    Returns:
        dict with analysis results or None on failure
    """
    # Convert angle to direction
    angle_rad = math.radians(incident_angle)
    incident_dir_x = math.sin(angle_rad)
    incident_dir_y = 0.0
    incident_dir_z = math.cos(angle_rad)
    
    # Normalize
    length = math.sqrt(incident_dir_x**2 + incident_dir_y**2 + incident_dir_z**2)
    if length > 0:
        incident_dir_x /= length
        incident_dir_y /= length
        incident_dir_z /= length
    
    # Set inputs
    inputs = {
        (analyzer, "Incident Direction X"): incident_dir_x,
        (analyzer, "Incident Direction Y"): incident_dir_y,
        (analyzer, "Incident Direction Z"): incident_dir_z,
        (analyzer, "UV X"): uv_x,
        (analyzer, "UV Y"): uv_y,
        (analyzer, "Material ID"): 0,
        (analyzer, "Resolution"): RESOLUTION,
        (analyzer, "Num Samples"): NUM_SAMPLES,
    }
    
    try:
        executor.reset_allocator()
        executor.prepare_tree(tree, present_eval)
        set_node_inputs(executor, inputs)
        hydra.render()
        
        # Retrieve outputs
        eval_data = hydra.get_output_texture("PresentEval")
        eval_array = np.array(eval_data, dtype=np.float32).reshape(RESOLUTION, RESOLUTION, 4)
        
        pdf_data = hydra.get_output_texture("PresentPDF")
        pdf_array = np.array(pdf_data, dtype=np.float32).reshape(RESOLUTION, RESOLUTION, 4)
        
        sample_data = hydra.get_output_texture("PresentSample")
        sample_array_raw = np.array(sample_data, dtype=np.float32).reshape(RESOLUTION, RESOLUTION, 4)
        sample_array = sample_array_raw.copy()
        sample_array[:, :, :3] = sample_array_raw[:, :, :3] / NUM_SAMPLES
        
        importance_data = hydra.get_output_texture("PresentImportance")
        importance_array = np.array(importance_data, dtype=np.float32).reshape(RESOLUTION, RESOLUTION, 4)
        
        # Save images with descriptive filenames
        # Format: materialname_angle30_uv0.5-0.5_eval.exr
        uv_str = f"{uv_x:.1f}-{uv_y:.1f}".replace(".", "p")
        base_filename = f"{material_name}_angle{incident_angle}_uv{uv_str}"
        
        eval_path = output_dir / f"{base_filename}_eval.exr"
        sample_path = output_dir / f"{base_filename}_sample.exr"
        importance_path = output_dir / f"{base_filename}_importance.exr"
        
        saved_images = {}
        if save_texture_exr(eval_array, str(eval_path)):
            saved_images["eval"] = str(eval_path.name)
        if save_texture_exr(sample_array, str(sample_path)):
            saved_images["sample"] = str(sample_path.name)
        if save_texture_exr(importance_array, str(importance_path)):
            saved_images["importance"] = str(importance_path.name)
        
        # Analyze
        results = analyze_brdf_test(eval_array, sample_array, importance_array, valid_mask)
        results["incident_angle"] = incident_angle
        results["uv_position"] = [uv_x, uv_y]
        results["saved_images"] = saved_images
        
        return results
        
    except Exception as e:
        print(f"    ERROR during test: {e}")
        return None


def test_material(material_path, output_dir):
    """
    Test a single material across all configurations
    
    Returns:
        dict with test results
    """
    material_name = material_path.stem
    print(f"\nTesting: {material_name}")
    
    # Create material-specific output directory
    material_output_dir = output_dir / material_name
    material_output_dir.mkdir(exist_ok=True)
    
    # Check if material has textures
    material_dir = material_path.parent
    has_textures = has_texture_maps(material_dir)
    
    # Determine UV positions to test
    if has_textures:
        uv_positions = TEST_UV_POSITIONS
        print(f"  Has textures - testing {len(uv_positions)} UV positions")
    else:
        uv_positions = [(0.5, 0.5)]  # Single position for uniform materials
        print(f"  No textures - testing single UV position")
    
    # Bind material
    shader_ball_path = assets_dir / "shader_ball.usdc"
    output_usd = material_output_dir / f"shader_ball_{material_name}.usdc"
    
    if not bind_material_to_shader_ball(shader_ball_path, material_path, output_usd):
        print(f"  FAILED to bind material")
        return {
            "material_name": material_name,
            "status": "failed",
            "error": "Failed to bind material to shader_ball"
        }
    
    # Create HydraRenderer
    try:
        hydra = renderer.HydraRenderer(str(output_usd), width=RESOLUTION, height=RESOLUTION)
    except Exception as e:
        print(f"  FAILED to create renderer: {e}")
        return {
            "material_name": material_name,
            "status": "failed",
            "error": f"Failed to create HydraRenderer: {str(e)}"
        }
    
    # Setup node system
    node_system = hydra.get_node_system()
    config_path = binary_dir / "render_nodes.json"
    if config_path.exists():
        try:
            node_system.load_configuration(str(config_path))
        except Exception as e:
            print(f"  FAILED to load config: {e}")
            return {
                "material_name": material_name,
                "status": "failed",
                "error": f"Failed to load configuration: {str(e)}"
            }
    
    tree = node_system.get_node_tree()
    executor = node_system.get_node_tree_executor()
    
    # Initialize with render
    try:
        hydra.render()
    except Exception as e:
        print(f"  WARNING: Initial render failed: {e}")
    
    # Create analyzer node
    try:
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
        
        tree.add_link(analyzer.get_output_socket("BRDF Eval"), present_eval.get_input_socket("Color"))
        tree.add_link(analyzer.get_output_socket("PDF"), present_pdf.get_input_socket("Color"))
        tree.add_link(analyzer.get_output_socket("Sample Distribution"), present_sample.get_input_socket("Color"))
        tree.add_link(analyzer.get_output_socket("Importance Test"), present_importance.get_input_socket("Color"))
    except Exception as e:
        print(f"  FAILED to create nodes: {e}")
        return {
            "material_name": material_name,
            "status": "failed",
            "error": f"Failed to create analyzer nodes: {str(e)}"
        }
    
    # Compute hemisphere mask once
    valid_mask = compute_hemisphere_mask(RESOLUTION)
    
    # Run tests for all configurations
    test_results = []
    total_tests = len(uv_positions) * len(TEST_ANGLES)
    current_test = 0
    
    for uv_x, uv_y in uv_positions:
        for angle in TEST_ANGLES:
            current_test += 1
            print(f"  [{current_test}/{total_tests}] UV=({uv_x:.1f},{uv_y:.1f}), Angle={angle}°...", end=" ")
            
            result = test_material_at_config(
                hydra, tree, executor, analyzer,
                present_eval, present_pdf, present_sample, present_importance,
                angle, uv_x, uv_y, valid_mask, material_name, material_output_dir
            )
            
            if result:
                test_results.append(result)
                print(f"OK (L2={result['eval_sample_l2_distance']:.6f}, Var={result['importance_variance']:.6f})")
            else:
                print("FAILED")
    
    # Compute aggregate statistics
    if test_results:
        avg_l2 = np.mean([r['eval_sample_l2_distance'] for r in test_results])
        avg_corr = np.mean([r['eval_sample_correlation'] for r in test_results])
        avg_variance = np.mean([r['importance_variance'] for r in test_results])
        avg_cv = np.mean([r['importance_cv'] for r in test_results])
        
        aggregate = {
            "avg_eval_sample_l2": float(avg_l2),
            "avg_eval_sample_correlation": float(avg_corr),
            "avg_importance_variance": float(avg_variance),
            "avg_importance_cv": float(avg_cv)
        }
    else:
        aggregate = {}
    
    return {
        "material_name": material_name,
        "material_path": str(material_path.relative_to(assets_dir)),
        "output_directory": str(material_output_dir.relative_to(output_dir)),
        "has_textures": has_textures,
        "status": "success" if test_results else "failed",
        "num_tests": len(test_results),
        "aggregate_stats": aggregate,
        "detailed_results": test_results
    }


def main():
    # Parse command line arguments
    if len(sys.argv) < 2:
        print("Usage: python test_brdf_comprehensive.py <material_folder_path>")
        print("Example: python test_brdf_comprehensive.py C:/path/to/matx_library/Aluminum_1k_8b_tAdaTTp")
        return 1
    
    material_folder = Path(sys.argv[1])
    if not material_folder.exists():
        print(f"Error: Material folder does not exist: {material_folder}")
        return 1
    
    print("="*80)
    print("BRDF Testing - Single Material")
    print("="*80)
    print(f"Material folder: {material_folder}")
    print(f"Resolution: {RESOLUTION}x{RESOLUTION}")
    print(f"Samples per test: {NUM_SAMPLES}")
    print(f"UV positions: {TEST_UV_POSITIONS}")
    print(f"Incident angles: {TEST_ANGLES}")
    print("="*80)
    
    # Find material file
    material_path = find_material_in_folder(material_folder)
    if not material_path:
        print(f"Error: No .mtlx file found in {material_folder}")
        return 1
    
    print(f"Found material: {material_path.name}\n")
    
    # Create output directory
    output_dir = binary_dir / "brdf_tests"
    output_dir.mkdir(exist_ok=True)
    
    # Test the material
    result = test_material(material_path, output_dir)
    
    # Save results to JSON
    material_name = material_path.stem
    output_json = output_dir / material_name / f"result_{material_name}.json"
    
    summary = {
        "test_config": {
            "resolution": RESOLUTION,
            "num_samples": NUM_SAMPLES,
            "uv_positions": TEST_UV_POSITIONS,
            "incident_angles": TEST_ANGLES,
            "timestamp": datetime.now().isoformat()
        },
        "result": result
    }
    
    with open(output_json, 'w') as f:
        json.dump(summary, f, indent=2)
    
    print("\n" + "="*80)
    print("RESULT")
    print("="*80)
    print(f"Status:           {result['status']}")
    if result["status"] == "success":
        print(f"Tests completed:  {result['num_tests']}")
        if result.get("aggregate_stats"):
            print(f"Avg L2:           {result['aggregate_stats']['avg_eval_sample_l2']:.6f}")
            print(f"Avg Variance:     {result['aggregate_stats']['avg_importance_variance']:.6f}")
    print(f"\nResults saved to: {output_json}")
    print("="*80)
    
    return 0 if result["status"] == "success" else 1


if __name__ == "__main__":
    sys.exit(main())
