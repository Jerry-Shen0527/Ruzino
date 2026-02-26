#!/usr/bin/env python3
"""
Material BRDF Visualizer Script

Analyzes material BRDF properties:
1. BRDF Eval distribution
2. PDF distribution
3. Sample frequency distribution

Outputs three visualization images for analysis.
Run directly: python material_brdf_visualizer.py <material_folder>
"""

import sys
import os
from pathlib import Path
import numpy as np

# Setup paths
script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Debug"
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

    # Find meshes
    preview_mesh = stage.GetPrimAtPath("/root/Preview_Mesh/Preview_Mesh")
    calibration_mesh = stage.GetPrimAtPath("/root/Calibration_Mesh/Calibration_Mesh")

    if not preview_mesh or not calibration_mesh:
        print(f"✗ Could not find meshes in shader_ball")
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
    mtlx_stage = Usd.Stage.Open(str(material_path))
    if not mtlx_stage:
        print(f"✗ Could not open MaterialX file: {material_path}")
        return False

    mtlx_material_path = None
    for prim in mtlx_stage.Traverse():
        if prim.IsA(UsdShade.Material):
            mtlx_material_path = prim.GetPath()
            break

    if not mtlx_material_path:
        print(f"✗ No material found in {material_path}")
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

    # Bind material to meshes
    UsdShade.MaterialBindingAPI(preview_mesh).Bind(material)
    UsdShade.MaterialBindingAPI(calibration_mesh).Bind(material)

    # Save
    stage.GetRootLayer().Export(str(output_path))
    print(f"✓ Material bound and saved to: {output_path}")
    return True


# Test configuration
RESOLUTION = 512
NUM_SAMPLES = 1000000  # 1M samples for better statistics

# Bind material to shader_ball
shader_ball_path = assets_dir / "shader_ball.usdc"
material_path = assets_dir / "matx_library" / "Aluminum_1k_8b_tAdaTTp" / "Aluminum.mtlx"
output_usd = binary_dir / "shader_ball_with_material.usdc"

if not bind_material_to_shader_ball(shader_ball_path, material_path, output_usd):
    print("✗ Failed to bind material")
    exit(1)

# Create HydraRenderer with material-bound USD scene
try:
    hydra = renderer.HydraRenderer(str(output_usd), width=RESOLUTION, height=RESOLUTION)
except Exception as e:
    print(f"✗ Failed to create HydraRenderer: {e}")
    import traceback
    traceback.print_exc()
    exit(1)

# Get node system (already initialized by HydraRenderer)
node_system = hydra.get_node_system()

# Load render nodes configuration if not already loaded
config_path = binary_dir / "render_nodes.json"
if config_path.exists():
    try:
        node_system.load_configuration(str(config_path))
    except Exception as e:
        print(f"✗ Failed to load configuration: {e}")
        exit(1)
else:
    print(f"✗ Configuration file not found: {config_path}")
    exit(1)

tree = node_system.get_node_tree()
executor = node_system.get_node_tree_executor()

# Render a frame first to initialize materials
try:
    hydra.render()
except Exception as e:
    print(f"✗ Failed to render: {e}")

# Create BRDF analyzer node
try:
    analyzer = tree.add_node("material_brdf_analyzer")
except Exception as e:
    print(f"✗ Failed to create analyzer node: {e}")
    exit(1)
analyzer.ui_name = "BRDFAnalyzer"

# Create present nodes for all four outputs
present_eval = tree.add_node("present_color")
present_eval.ui_name = "PresentEval"
present_pdf = tree.add_node("present_color")
present_pdf.ui_name = "PresentPDF"
present_sample = tree.add_node("present_color")
present_sample.ui_name = "PresentSample"
present_importance = tree.add_node("present_color")
present_importance.ui_name = "PresentImportance"

# Connect analyzer outputs to present nodes
tree.add_link(
    analyzer.get_output_socket("BRDF Eval"), present_eval.get_input_socket("Color")
)
tree.add_link(analyzer.get_output_socket("PDF"), present_pdf.get_input_socket("Color"))
tree.add_link(
    analyzer.get_output_socket("Sample Distribution"),
    present_sample.get_input_socket("Color"),
)
tree.add_link(
    analyzer.get_output_socket("Importance Test"),
    present_importance.get_input_socket("Color"),
)

# Set analysis parameters (common for all four analyses)
# Use 45-degree angle to see broader distribution
import math

angle = math.radians(45)
incident_dir_x, incident_dir_y, incident_dir_z = (
    math.sin(angle),
    0.0,
    math.cos(angle),
)  # 45 degrees from normal

# Normalize incident direction
incident_dir_length = np.sqrt(incident_dir_x**2 + incident_dir_y**2 + incident_dir_z**2)
if incident_dir_length > 0:
    incident_dir_x /= incident_dir_length
    incident_dir_y /= incident_dir_length
    incident_dir_z /= incident_dir_length
uv_x, uv_y = 0.5, 0.5

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

# Execute analysis (all four at once)
print("Computing BRDF analysis...")
try:
    executor.reset_allocator()
    executor.prepare_tree(tree, present_eval)
    set_node_inputs(executor, inputs)
    hydra.render()
except Exception as e:
    print(f"✗ Failed during rendering: {e}")
    import traceback

    traceback.print_exc()
    exit(1)

# Retrieve all four outputs
try:
    brdf_eval_data = hydra.get_output_texture("PresentEval")
    eval_array = np.array(brdf_eval_data, dtype=np.float32).reshape(
        RESOLUTION, RESOLUTION, 4
    )

    pdf_data = hydra.get_output_texture("PresentPDF")
    pdf_array = np.array(pdf_data, dtype=np.float32).reshape(RESOLUTION, RESOLUTION, 4)

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
except Exception as e:
    print(f"✗ Failed to retrieve textures: {e}")
    import traceback

    traceback.print_exc()
    exit(1)

# Save images and perform statistical analysis
try:
    def save_texture_exr(array, filename):
        """Save only EXR format with HDR values"""
        rgb = array[:, :, :3]
        rgb_exr = np.flipud(rgb.astype(np.float32))
        try:
            import imageio
            imageio.imwrite(filename, rgb_exr, format="EXR")
        except Exception as e:
            print(f"✗ Could not save EXR: {e}")

    save_texture_exr(eval_array, "./brdf_eval.exr")
    save_texture_exr(pdf_array, "./brdf_pdf.exr")
    save_texture_exr(sample_array, "./brdf_sample_distribution.exr")
    save_texture_exr(importance_array, "./brdf_importance_test.exr")

    # ===== Statistical Analysis =====
    print("\n" + "=" * 70)
    print("Statistical Analysis")
    print("=" * 70)

    # Extract single channel values (they're stored as RGB with same values)
    eval_values = eval_array[:, :, 0]
    pdf_values = pdf_array[:, :, 0]
    sample_values = sample_array[:, :, 0]
    importance_values = importance_array[:, :, 0]

    # Only consider valid regions (inside hemisphere projection)
    # Create mask for valid pixels
    height, width = eval_values.shape
    y_coords, x_coords = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    u = (x_coords + 0.5) / width
    v = (y_coords + 0.5) / height
    p = np.stack([u * 2 - 1, v * 2 - 1], axis=-1)
    r = np.sqrt(p[:, :, 0] ** 2 + p[:, :, 1] ** 2)
    valid_mask = r <= 1.0

    eval_valid = eval_values[valid_mask]
    pdf_valid = pdf_values[valid_mask]
    sample_valid = sample_values[valid_mask]
    importance_valid = importance_values[valid_mask]

    # Print distribution statistics
    print(f"\nDistribution Analysis:")
    print(
        f"  BRDF Eval percentiles: {np.percentile(eval_valid, [0, 25, 50, 75, 95, 100])}"
    )
    print(
        f"  Sample percentiles: {np.percentile(sample_valid, [0, 25, 50, 75, 95, 100])}"
    )
    sample_sum = np.sum(sample_valid)
    print(f"  Sample sum: {sample_sum:.6f} (should be ~1.0)")

    # 1. Compare sample frequency with PDF
    print("\n1. Sample vs PDF:")
    if len(pdf_valid) > 0 and pdf_valid.std() > 0 and sample_valid.std() > 0:
        correlation = np.corrcoef(pdf_valid, sample_valid)[0, 1]
        print(f"   Correlation: {correlation:.6f}")

    # 2. Variance analysis of sample distribution
    print("\n2. Sample Distribution:")
    print(f"   Variance: {sample_valid.var():.6f}")
    print(
        f"   CV: {sample_valid.std() / (sample_valid.mean() + 1e-8):.6f}"
    )

    # 3. Compare BRDF eval with PDF
    print("\n3. BRDF vs PDF:")
    if len(eval_valid) > 0 and eval_valid.std() > 0 and pdf_valid.std() > 0:
        correlation_eval_pdf = np.corrcoef(eval_valid, pdf_valid)[0, 1]
        print(f"   Correlation: {correlation_eval_pdf:.6f}")

    # 4. Integration check
    print("\n4. Integration:")
    pixel_area = 1.0 / (height * width)
    pdf_integral = pdf_valid.sum() * pixel_area
    sample_integral = sample_valid.sum() * pixel_area
    print(f"   PDF integral: {pdf_integral:.6f}")
    print(f"   Sample integral: {sample_integral:.6f}")
    print(f"   Ratio (Sample/PDF): {sample_integral / (pdf_integral + 1e-8):.6f}")

    # 5. Importance sampling test
    print("\n5. Importance Test:")
    importance_nonzero = importance_valid[importance_valid > 0]
    if len(importance_nonzero) > 0:
        importance_mean = importance_nonzero.mean()
        importance_std = importance_nonzero.std()
        importance_cv = importance_std / (importance_mean + 1e-8)
        print(f"   Mean: {importance_mean:.6f}")
        print(f"   CV: {importance_cv:.6f} (lower is better)")
        print(f"   Range: [{importance_nonzero.min():.6f}, {importance_nonzero.max():.6f}]")

except ImportError as e:
    print(f"✗ Import error: {e}")

print("\n" + "=" * 70)
print("✓ BRDF Analysis Complete!")
print("=" * 70)
