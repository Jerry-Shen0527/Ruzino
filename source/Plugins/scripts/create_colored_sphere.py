from pxr import UsdGeom, Gf, Sdf, UsdShade
import math

# Get the USD stage (global stage object is already available)
usd_stage = stage.get_pxr_stage()

# Clean up existing spheres from previous runs
spheres_path = Sdf.Path("/Spheres")
existing_prim = usd_stage.GetPrimAtPath(spheres_path)
if existing_prim:
    usd_stage.RemovePrim(spheres_path)
    print("Cleaned up existing spheres")

# Create materials scope
materials_path = Sdf.Path("/Materials")
materials_prim = usd_stage.GetPrimAtPath(materials_path)
if not materials_prim:
    UsdGeom.Scope.Define(usd_stage, materials_path)

# Create 10x10 grid of spheres with animation
radius = 0.5
base_z_height = 2.0  # Average height at z=2
z_amplitude = 1.0    # Amplitude of oscillation (±1)
spacing = 1.5        # Distance between sphere centers

# Animation parameters
# 6 seconds at 60fps = 360 frames, 2 oscillation cycles
animation_duration = 360  # frames (6 seconds at 60fps)
num_cycles = 2        # number of complete oscillations

# Calculate grid offset to center it
grid_size = 10
grid_offset = (grid_size - 1) * spacing / 2.0

for i in range(grid_size):
    for j in range(grid_size):
        # Calculate base position
        x = i * spacing - grid_offset
        y = j * spacing - grid_offset

        # Create sphere
        sphere_path = f"/Spheres/Sphere_{i}_{j}"
        sphere = UsdGeom.Sphere.Define(usd_stage, sphere_path)
        sphere.CreateRadiusAttr(radius)

        # Get sphere prim for transforms
        sphere_prim = usd_stage.GetPrimAtPath(sphere_path)
        xformable = UsdGeom.Xformable(sphere_prim)

        # Create unique material for this sphere
        material_path = f"/Materials/Material_{i}_{j}"
        material = UsdShade.Material.Define(usd_stage, material_path)

        # Create UsdPreviewSurface shader
        shader = UsdShade.Shader.Define(usd_stage, f"{material_path}/PreviewSurface")
        shader.CreateIdAttr("UsdPreviewSurface")

        # Create diffuse color input
        diffuse_input = shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f)

        shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)

        # Create output
        shader.CreateOutput("surface", Sdf.ValueTypeNames.Token)

        # Connect material output
        material.CreateSurfaceOutput().ConnectToSource(shader.GetOutput("surface"))

        # Bind material to sphere
        UsdShade.MaterialBindingAPI(sphere_prim).Bind(material)

        # Add animation keyframes
        translate_op = xformable.AddTranslateOp()

        for frame in range(animation_duration + 1):
            # Normalize time to 0-1 range
            t = frame / animation_duration

            # Wave phase offset based on position (creates wave effect)
            phase_offset = 2.0 * math.pi * (i + j) / (2.0 * grid_size)

            # Calculate oscillating z height with wave effect (2 complete cycles)
            z = base_z_height + z_amplitude * math.sin(2.0 * math.pi * num_cycles * t + phase_offset)

            # Set position with time sampling
            translate_op.Set(Gf.Vec3d(x, y, z), Sdf.TimeCode(frame / 60))

            # Calculate color that changes over time (2 complete color cycles)
            hue_shift = t * 2.0 * math.pi * num_cycles
            r = 0.5 + 0.5 * math.sin(hue_shift + phase_offset)
            g = 0.5 + 0.5 * math.sin(hue_shift + phase_offset + 2.0 * math.pi / 3.0)
            b = 0.5 + 0.5 * math.sin(hue_shift + phase_offset + 4.0 * math.pi / 3.0)

            # Set color with time sampling
            diffuse_input.Set(Gf.Vec3f(r, g, b), Sdf.TimeCode(frame / 60))

print(f"Created 100 animated colored spheres (10x10 grid) with 6-second animation (60fps, 2 cycles)")
