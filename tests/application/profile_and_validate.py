import subprocess
import sys
import os
from pathlib import Path
import numpy as np
from PIL import Image


def run_command(cmd, cwd, description):
    """Run a command and handle errors."""
    print(f"\n{'='*60}")
    print(f"{description}")
    print(f"Command: {' '.join(cmd)}")
    print(f"Working directory: {cwd}")
    print(f"{'='*60}\n")

    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)

    if result.stdout:
        print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)

    if result.returncode != 0:
        print(f"\n❌ {description} failed with exit code {result.returncode}")
        sys.exit(1)

    print(f"✅ {description} completed successfully")
    return result


def compare_images(img1_path, img2_path, max_mean_diff=0.2, max_rmse=0.25):
    """
    Compare two images and return whether they're similar enough.

    Args:
        img1_path: Path to first image
        img2_path: Path to reference image
        max_mean_diff: Maximum allowed mean absolute difference (0-1 scale)
        max_rmse: Maximum allowed root mean square error (0-1 scale)

    Returns:
        bool: True if images are similar enough
    """
    print(f"\n{'='*60}")
    print("Comparing images")
    print(f"Output: {img1_path}")
    print(f"Reference: {img2_path}")
    print(f"{'='*60}\n")

    if not os.path.exists(img1_path):
        print(f"❌ Output image not found: {img1_path}")
        return False

    if not os.path.exists(img2_path):
        print(f"❌ Reference image not found: {img2_path}")
        return False

    # Load images
    img1 = Image.open(img1_path).convert('RGB')
    img2 = Image.open(img2_path).convert('RGB')

    # Check dimensions
    if img1.size != img2.size:
        print(f"⚠️  Image dimensions don't match:")
        print(f"   Output: {img1.size}")
        print(f"   Reference: {img2.size}")
        # Resize reference to match output for comparison
        img2 = img2.resize(img1.size, Image.Resampling.LANCZOS)
        print(f"   Resized reference to match output")

    # Convert to numpy arrays and normalize to [0, 1]
    arr1 = np.array(img1).astype(np.float32) / 255.0
    arr2 = np.array(img2).astype(np.float32) / 255.0

    # Calculate metrics
    diff = np.abs(arr1 - arr2)
    mean_diff = np.mean(diff)
    rmse = np.sqrt(np.mean((arr1 - arr2) ** 2))
    max_diff = np.max(diff)

    print(f"Image comparison metrics:")
    print(f"  Mean absolute difference: {mean_diff:.6f} (max allowed: {max_mean_diff})")
    print(f"  Root mean square error:   {rmse:.6f} (max allowed: {max_rmse})")
    print(f"  Maximum pixel difference: {max_diff:.6f}")

    # Check if images are similar enough
    if mean_diff > max_mean_diff or rmse > max_rmse:
        print(f"\n❌ Images differ too much!")
        print(f"   Mean diff: {mean_diff:.6f} > {max_mean_diff}: {'FAIL' if mean_diff > max_mean_diff else 'PASS'}")
        print(f"   RMSE: {rmse:.6f} > {max_rmse}: {'FAIL' if rmse > max_rmse else 'PASS'}")
        return False

    print(f"\n✅ Images are similar enough (within tolerance)")
    return True


def main():
    """Main function to build, render, and validate."""
    # Get paths
    script_dir = Path(__file__).parent
    ruzino_root = script_dir.parent.parent
    build_dir = ruzino_root / "build"
    binaries_dir = ruzino_root / "Binaries" / "Release"

    print(f"Ruzino root: {ruzino_root}")
    print(f"Build directory: {build_dir}")
    print(f"Binaries directory: {binaries_dir}")

    # Step 1: Build with ninja
    run_command(
        ["ninja"],
        cwd=str(build_dir),
        description="Building project with ninja"
    )

    # Step 2: Run headless render
    output_image = binaries_dir / "sponza.png"
    reference_image = binaries_dir / "sponza_ref.png"

    # Remove old output if exists
    if output_image.exists():
        print(f"\nRemoving old output: {output_image}")
        output_image.unlink()

    render_cmd = [
        str(binaries_dir / "headless_render.exe"),
        "-u", r"..\..\Assets\main_sponza\NewSponza_Main_USD_Zup_003.usdc",
        "-j", r"..\..\Assets\Hd_RUZINO_RendererPlugin\render_nodes_save.json",
        "-o", "sponza.png",
        "-w", "3840",
        "-h", "2160",
        "-s", "128",
        "-c", "/FreeCamera"
    ]

    run_command(
        render_cmd,
        cwd=str(binaries_dir),
        description="Rendering scene with headless_render"
    )

    # Step 3: Compare images
    if not compare_images(str(output_image), str(reference_image)):
        print("\n❌ Validation failed: Images differ too much")
        sys.exit(1)

    print(f"\n{'='*60}")
    print("✅ All tests passed!")
    print(f"{'='*60}\n")


if __name__ == "__main__":
    main()
