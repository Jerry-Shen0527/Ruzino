#!/usr/bin/env python3
"""
Python Render Graph API - High Level Interface

This module provides a Python interface to the Ruzino render graph system.
Since direct NodeTree access has binding issues, we use the C++ headless
executable as a backend.
"""

import subprocess
import os
from pathlib import Path
from typing import Optional, Tuple

class RuzinoRenderer:
    """High-level Python interface for Ruzino rendering"""

    def __init__(self, binary_dir: Optional[Path] = None):
        """
        Initialize renderer

        Args:
            binary_dir: Path to Binaries/Debug or Binaries/Release
        """
        if binary_dir is None:
            # Try to find binary directory
            binary_dir = Path(os.getcwd())
            if not (binary_dir / "headless_render.exe").exists():
                # Try common locations
                possible_dirs = [
                    Path.cwd() / "Binaries" / "Debug",
                    Path.cwd() / "Binaries" / "Release",
                    Path(__file__).parent.parent.parent.parent.parent / "Binaries" / "Debug",
                ]
                for pd in possible_dirs:
                    if pd.exists() and (pd / "headless_render.exe").exists():
                        binary_dir = pd
                        break

        self.binary_dir = Path(binary_dir)
        self.headless_exe = self.binary_dir / "headless_render.exe"

        if not self.headless_exe.exists():
            raise FileNotFoundError(
                f"headless_render.exe not found at {self.headless_exe}\n"
                f"Please build the project or specify correct binary_dir"
            )

        # Find assets directory
        self.assets_dir = self.binary_dir.parent.parent / "Assets"
        if not self.assets_dir.exists():
            raise FileNotFoundError(f"Assets directory not found at {self.assets_dir}")

    def render(
        self,
        usd_scene: str | Path,
        render_graph: str | Path,
        output_image: str | Path,
        width: int = 512,
        height: int = 512,
        spp: int = 4,
        timeout: int = 300,
        verbose: bool = False
    ) -> bool:
        """
        Execute a render using the node graph system

        Args:
            usd_scene: Path to USD scene file (.usdc, .usda)
            render_graph: Path to render graph JSON file
            output_image: Output image path (.png, .exr)
            width: Output image width
            height: Output image height
            spp: Samples per pixel
            timeout: Execution timeout in seconds
            verbose: Print detailed output

        Returns:
            True if render completed successfully
        """
        cmd = [
            str(self.headless_exe),
            str(usd_scene),
            str(render_graph),
            str(output_image),
            str(width),
            str(height),
            str(spp)
        ]

        if verbose:
            print(f"Executing: {' '.join(cmd)}")

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                cwd=str(self.binary_dir)
            )

            if verbose or result.returncode != 0:
                print(result.stdout)
                if result.stderr:
                    print(result.stderr)

            return result.returncode == 0

        except subprocess.TimeoutExpired:
            print(f"Render timed out after {timeout} seconds")
            return False
        except Exception as e:
            print(f"Error executing render: {e}")
            return False

    def render_with_verification(
        self,
        usd_scene: str | Path,
        render_graph: str | Path,
        output_image: str | Path,
        width: int = 512,
        height: int = 512,
        spp: int = 4,
        verify_not_black: bool = True
    ) -> Tuple[bool, dict]:
        """
        Render and verify output

        Returns:
            (success, stats) where stats contains image analysis
        """
        success = self.render(usd_scene, render_graph, output_image, width, height, spp)

        if not success:
            return False, {}

        # Verify output
        if not os.path.exists(output_image):
            return False, {"error": "Output file not created"}

        stats = {
            "file_size": os.path.getsize(output_image),
            "width": width,
            "height": height,
            "spp": spp
        }

        if verify_not_black:
            try:
                from PIL import Image
                import numpy as np

                img = Image.open(output_image)
                arr = np.array(img)

                stats["mode"] = img.mode
                stats["shape"] = arr.shape
                stats["dtype"] = str(arr.dtype)
                stats["min"] = int(arr.min())
                stats["max"] = int(arr.max())
                stats["mean"] = float(arr.mean())

                stats["is_black"] = (arr.max() == 0)

            except ImportError:
                stats["note"] = "PIL not available for verification"

        return True, stats


# Example usage
if __name__ == "__main__":
    print("Ruzino Renderer Python API")
    print("="*70)

    # Initialize renderer
    renderer = RuzinoRenderer()
    print(f"✓ Binary dir: {renderer.binary_dir}")
    print(f"✓ Assets dir: {renderer.assets_dir}")
    print()

    # Example render
    success, stats = renderer.render_with_verification(
        usd_scene=renderer.assets_dir / "shader_ball.usdc",
        render_graph=renderer.assets_dir / "Hd_RUZINO_RendererPlugin" / "render_nodes_save.json",
        output_image="python_api_test.png",
        width=512,
        height=512,
        spp=4
    )

    print("\nRender Results:")
    print("="*70)
    if success:
        print("✓✓✓ RENDER SUCCESSFUL!")
        print("\nImage Statistics:")
        for key, value in stats.items():
            print(f"  {key}: {value}")

        if stats.get("is_black", False):
            print("\n⚠ Warning: Image appears to be pure black")
        else:
            print("\n✓ Image contains visible content")
    else:
        print("✗ Render failed")
        if stats:
            print(f"Error: {stats.get('error', 'Unknown')}")
