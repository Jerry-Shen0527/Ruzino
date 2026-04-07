from pxr import UsdGeom, Gf, Sdf
# Get the USD stage (global stage object is already available)
usd_stage = stage.get_pxr_stage()

# Clean up existing spheres from previous runs
spheres_path = Sdf.Path("/Spheres")
existing_prim = usd_stage.GetPrimAtPath(spheres_path)
if existing_prim:
    usd_stage.RemovePrim(spheres_path)
    print("Cleaned up existing spheres")

# Create 10x10 grid of spheres at z=1 with radius 0.5
radius = 0.5
z_height = 1.0
spacing = 1.5  # Distance between sphere centers

# Calculate grid offset to center it
grid_size = 10
grid_offset = (grid_size - 1) * spacing / 2.0

for i in range(grid_size):
    for j in range(grid_size):
        # Calculate position
        x = i * spacing - grid_offset
        y = j * spacing - grid_offset
        z = z_height

        # Create sphere
        sphere_path = f"/Spheres/Sphere_{i}_{j}"
        sphere = UsdGeom.Sphere.Define(usd_stage, sphere_path)
        sphere.CreateRadiusAttr(radius)

        # Set position using transform
        sphere_prim = usd_stage.GetPrimAtPath(sphere_path)
        xformable = UsdGeom.Xformable(sphere_prim)
        xformable.AddTranslateOp().Set(Gf.Vec3d(x, y, z))

print(f"Created 100 spheres (10x10 grid) at z={z_height} with radius={radius}")
