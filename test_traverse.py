#!/usr/bin/env python
"""
Test script to traverse stage and inspect prims
"""

import sys
import os

# Add binary directory to path
binary_dir = os.path.join(os.path.dirname(__file__), "Binaries", "Release")
sys.path.insert(0, binary_dir)

import stage_py


def test_traverse():
    print("=" * 80)
    print("Testing Stage Traverse Interface")
    print("=" * 80)

    # Load the tree grid file
    usd_file = os.path.join(binary_dir, "tree_grid_5x5x5.usdc")
    print(f"\nLoading: {usd_file}")

    if not os.path.exists(usd_file):
        print(f"ERROR: File not found: {usd_file}")
        return False

    # Create stage
    stage = stage_py.Stage(usd_file)

    # Traverse with different depths
    print("\n--- Depth 0 (root only) ---")
    print(stage.traverse_stage(0))

    print("\n--- Depth 1 ---")
    print(stage.traverse_stage(1))

    print("\n--- Depth 2 ---")
    print(stage.traverse_stage(2))

    print("\n--- Full traverse (all depths) ---")
    result = stage.traverse_stage()
    print(result)

    # Save to file for later inspection
    output_file = os.path.join(binary_dir, "stage_traverse_output.txt")
    with open(output_file, "w") as f:
        f.write(result)
    print(f"\nTraverse output saved to: {output_file}")

    return True


if __name__ == "__main__":
    try:
        success = test_traverse()
        if success:
            print("\n" + "=" * 80)
            print("✅ Test completed successfully!")
            print("=" * 80)
        else:
            print("\n" + "=" * 80)
            print("❌ Test failed!")
            print("=" * 80)
            sys.exit(1)
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)
