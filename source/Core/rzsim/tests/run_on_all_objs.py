"""
Test batch rendering of OBJ files.
"""

import sys
import os
from pathlib import Path
from batch_render import main as batch_render_main


def test_batch_render():
    """Run batch render for all OBJ files."""
    # Save original state
    original_argv = sys.argv.copy()
    original_cwd = os.getcwd()

    try:
        # Determine paths
        tests_dir = Path(__file__).parent
        release_dir = tests_dir / ".." / ".." / ".." / ".." / "Binaries" / "Release"
        release_dir = release_dir.resolve()

        # Change to Release directory (needed for DLLs)
        if release_dir.exists():
            os.chdir(release_dir)
            print(f"Changed working directory to: {release_dir}")
        else:
            print(f"Warning: Release directory not found: {release_dir}")
            return

        # Set up arguments for batch_render
        sys.argv = [
            'batch_render.py',
            '--backup',
            '-v'  # Verbose mode
        ]

        # Run batch render
        result = batch_render_main()

        # Check if successful (0 = success)
        assert result == 0, f"Batch render failed with code {result}"

    finally:
        # Restore original state
        sys.argv = original_argv
        os.chdir(original_cwd)
