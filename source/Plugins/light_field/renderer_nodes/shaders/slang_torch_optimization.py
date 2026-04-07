import pathlib
import slangpy as spy
import torch
import numpy as np

# Create a SlangPy device; it will look in the local folder for any Slang includes
# Calculate shader path relative to this file
script_dir = pathlib.Path(__file__).parent.absolute()
shaders_path = script_dir.parent.parent.parent.parent / "Runtime" / "renderer" / "nodes" / "shaders" / "shaders"

device = spy.create_device(
    include_paths=[
        script_dir,
        str(shaders_path),
    ],
    enable_cuda_interop=True,
)

# Load torch wrapped module.
module = spy.TorchModule.load_from_file(device, "light_field_init_closest_lens.slang")

# Example usage of pixel_position_to_ray_with_fixed_lens_pos
if __name__ == "__main__":
    # Create sample input tensors
    pixel_position = torch.tensor([0.0, 0.0], dtype=torch.float32, device="cuda", requires_grad=True)
    lens_position = torch.tensor([0.0, 0.0, -40.0], dtype=torch.float32, device="cuda", requires_grad=True)

    # Set other parameters
    display_z = -50.0
    focal_length = 8.0
    pupil_diameter = 1.0
    view_distance = 100.0
    view_count = 64
    view_range = torch.tensor([-30.0, 30.0], dtype=torch.float32, device="cuda")
    rayOrigin = torch.zeros(3, dtype=torch.float32, device="cuda")
    rayDirection = torch.zeros(3, dtype=torch.float32, device="cuda")

    # Call the function
    result = module.pixel_position_to_ray_with_fixed_lens_pos(
        pixelPositionF=pixel_position,
        lensPosition=lens_position,
        displayZ=display_z,
        focalLength=focal_length,
        pupilDiameter=pupil_diameter,
        viewDistance=view_distance,
        viewCount=view_count,
        viewRange=view_range,
        rayOrigin=rayOrigin,
        rayDirection=rayDirection
    )

    print("Ray Origin:", rayOrigin)
    print("Ray Direction:", rayDirection)

    # Example backward pass
    if hasattr(result, 'Origin'):
        loss = torch.sum(result.Origin**2)
        loss.backward()
        print("Pixel position gradients:", pixel_position.grad)
        print("Lens position gradients:", lens_position.grad)