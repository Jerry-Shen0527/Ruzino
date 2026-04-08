#!/usr/bin/env python3
"""
Headless Render Executor - Python version

Usage:
    python headless_render_executor.py <usd_file> <json_script> <output_image> [width] [height] [spp]

Example:
    python headless_render_executor.py cornell_box.usdc render_graph.json output.exr 1920 1080 16
"""

import sys
import os
import argparse
from pathlib import Path

# Add the module path if needed
script_dir = Path(__file__).parent
sys.path.insert(0, str(script_dir))

from ruzino_render_graph import RuzinoRenderGraph

# Try importing additional dependencies
try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


def parse_arguments():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description="Headless render executor for Ruzino",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s scene.usdc render.json output.exr
  %(prog)s scene.usdc render.json output.png 1024 768 32
        """
    )

    parser.add_argument('usd_file', help='Path to USD scene file')
    parser.add_argument('json_script', help='Path to render graph JSON file')
    parser.add_argument('output_image', help='Output image file (PNG/HDR/EXR)')
    parser.add_argument('width', nargs='?', type=int, default=1920,
                       help='Image width (default: 1920)')
    parser.add_argument('height', nargs='?', type=int, default=1080,
                       help='Image height (default: 1080)')
    parser.add_argument('spp', nargs='?', type=int, default=16,
                       help='Samples per pixel (default: 16)')

    return parser.parse_args()


def validate_inputs(args):
    """Validate input files exist."""
    if not os.path.exists(args.usd_file):
        print(f"Error: USD file not found: {args.usd_file}", file=sys.stderr)
        return False

    if not os.path.exists(args.json_script):
        print(f"Error: JSON script not found: {args.json_script}", file=sys.stderr)
        return False

    # Create output directory if needed
    output_dir = os.path.dirname(args.output_image)
    if output_dir and not os.path.exists(output_dir):
        try:
            os.makedirs(output_dir, exist_ok=True)
        except Exception as e:
            print(f"Error: Cannot create output directory: {e}", file=sys.stderr)
            return False

    return True


def save_tensor_as_image(tensor, output_path, gamma=2.2):
    """
    Save a PyTorch tensor as an image file.

    Args:
        tensor: PyTorch tensor (H x W x C) on GPU or CPU
        output_path: Output file path
        gamma: Gamma correction value
    """
    if not HAS_TORCH:
        print("Warning: PyTorch not available, cannot save tensor directly")
        return False

    try:
        # Move to CPU if needed
        if tensor.is_cuda:
            tensor = tensor.cpu()

        # Convert to numpy
        img_data = tensor.numpy()

        # Apply gamma correction if needed
        if gamma != 1.0:
            img_data = np.power(np.clip(img_data, 0.0, 1.0), 1.0 / gamma)

        # Determine file format and save
        ext = os.path.splitext(output_path)[1].lower()

        if ext in ['.exr', '.hdr']:
            # HDR format - save as float
            try:
                import OpenEXR
                import Imath
                # Save EXR
                # TODO: Implement EXR saving
                print(f"EXR/HDR saving not yet implemented")
                return False
            except ImportError:
                print("Warning: OpenEXR not available for HDR output")
                return False
        else:
            # LDR format (PNG, JPG, etc)
            if HAS_NUMPY:
                # Convert to 8-bit
                img_data = (np.clip(img_data, 0, 1) * 255).astype(np.uint8)

                try:
                    from PIL import Image
                    img = Image.fromarray(img_data)
                    img.save(output_path)
                    print(f"✓ Saved image to: {output_path}")
                    return True
                except ImportError:
                    print("Warning: PIL not available for image saving")
                    return False

    except Exception as e:
        print(f"Error saving image: {e}", file=sys.stderr)
        return False


def main():
    """Main execution function."""
    args = parse_arguments()

    print("="*70)
    print("Ruzino Headless Render Executor (Python)")
    print("="*70)
    print(f"USD file:      {args.usd_file}")
    print(f"JSON script:   {args.json_script}")
    print(f"Output image:  {args.output_image}")
    print(f"Resolution:    {args.width}x{args.height}")
    print(f"SPP:           {args.spp}")
    print("="*70)

    # Validate inputs
    if not validate_inputs(args):
        return 1

    try:
        # Create render graph
        print("\n[1/6] Creating render graph...")
        g = RuzinoRenderGraph("HeadlessRender")

        # Initialize RHI
        print("[2/6] Initializing RHI...")
        try:
            g.initRHI()
        except Exception as e:
            print(f"Warning: RHI initialization issue: {e}")

        # Load configuration
        print("[3/6] Loading render configuration...")
        g.loadConfiguration(args.json_script)

        # Load USD stage
        print("[4/6] Loading USD stage...")
        g.loadUSDStage(args.usd_file)

        # Set render settings
        print("[5/6] Setting render parameters...")
        g.setRenderSettings(width=args.width, height=args.height, spp=args.spp)

        # Execute rendering
        print("[6/6] Executing render graph...")
        print("  This may take a while depending on scene complexity and SPP...")

        # The actual execution would happen here
        # For now, this is a framework - the actual render execution
        # needs the C++ node system to be properly integrated

        g.execute()

        print("\n✓ Rendering completed successfully")

        # Try to get and save output
        # This would need to identify the output node automatically
        # or be specified in the graph
        print(f"\nNote: Automatic output saving not yet fully implemented")
        print(f"      Output should be available in the graph nodes")

        return 0

    except Exception as e:
        print(f"\n✗ Error during rendering: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
