#!/usr/bin/env python3
"""
Example: Simple render using Ruzino Python API

This example demonstrates the basic workflow for setting up and executing
a render using the Python API.
"""

import sys
import os
from pathlib import Path

# Add the build directory to path if needed
# sys.path.insert(0, 'path/to/build/bin')

from ruzino_render_graph import RuzinoRenderGraph


def example_basic_render():
    """Basic rendering example."""
    print("="*70)
    print("Example: Basic Render")
    print("="*70)

    # Create graph
    g = RuzinoRenderGraph("ExampleRender")

    # Initialize RHI (Render Hardware Interface)
    print("\n[1] Initializing RHI...")
    try:
        g.initRHI()
    except Exception as e:
        print(f"Warning: {e}")

    # Load render configuration
    print("[2] Loading render configuration...")
    config_path = "path_tracing.json"  # Adjust path as needed
    if not os.path.exists(config_path):
        print(f"Config file not found: {config_path}")
        print("Please adjust the path in this script")
        return

    g.loadConfiguration(config_path)

    # Load USD scene
    print("[3] Loading USD scene...")
    usd_path = "cornell_box_stage.usdc"  # Adjust path as needed
    if not os.path.exists(usd_path):
        print(f"USD file not found: {usd_path}")
        print("Please adjust the path in this script")
        return

    g.loadUSDStage(usd_path)

    # Set render settings
    print("[4] Setting render parameters...")
    g.setRenderSettings(
        width=512,
        height=512,
        spp=4  # Low SPP for quick test
    )

    # Execute rendering
    print("[5] Executing render graph...")
    try:
        g.execute()
        print("\n✓ Rendering completed successfully!")
    except Exception as e:
        print(f"\n✗ Rendering failed: {e}")
        import traceback
        traceback.print_exc()


def example_custom_graph():
    """Example of building a custom render graph."""
    print("\n" + "="*70)
    print("Example: Custom Render Graph")
    print("="*70)

    g = RuzinoRenderGraph("CustomGraph")

    # Load configuration
    config_path = "path_tracing.json"
    if not os.path.exists(config_path):
        print(f"Config not found: {config_path}")
        return

    g.loadConfiguration(config_path)

    # Create nodes manually
    print("\nCreating nodes...")
    try:
        # Create rendering pipeline nodes
        rng = g.createNode("rng_texture", name="RandomSeeds")
        ray_gen = g.createNode("node_render_ray_generation", name="RayGen")
        path_trace = g.createNode("path_tracing", name="PathTrace")
        accumulate = g.createNode("accumulate", name="Accumulate")
        gamma = g.createNode("gamma_correction", name="Gamma")
        present = g.createNode("present_color", name="Present")

        print("✓ Created 6 nodes")

        # Connect pipeline
        print("\nConnecting nodes...")
        g.addEdge(rng, "Random Number", ray_gen, "random seeds")
        g.addEdge(ray_gen, "Rays", path_trace, "Rays")
        g.addEdge(path_trace, "Output", accumulate, "Texture")
        g.addEdge(accumulate, "Accumulated", gamma, "Texture")
        g.addEdge(gamma, "Corrected", present, "Color")

        print("✓ Connected pipeline")

        # Mark final output
        g.markOutput(present, "Color")

        # Show graph info
        print(f"\nGraph: {len(g.nodes)} nodes, {len(g.links)} links")

    except Exception as e:
        print(f"✗ Failed to build graph: {e}")
        import traceback
        traceback.print_exc()


def example_with_torch():
    """Example using PyTorch integration."""
    print("\n" + "="*70)
    print("Example: PyTorch Integration")
    print("="*70)

    try:
        import torch
    except ImportError:
        print("PyTorch not available - skipping this example")
        return

    g = RuzinoRenderGraph("TorchExample")

    print("\nThis example demonstrates:")
    print("  1. Rendering to NVRHI texture")
    print("  2. Converting to PyTorch tensor")
    print("  3. Processing with PyTorch")
    print("  4. Converting back to NVRHI")

    # Load configuration
    config_path = "path_tracing.json"
    if not os.path.exists(config_path):
        print(f"Config not found: {config_path}")
        return

    try:
        g.loadConfiguration(config_path)

        # Create render pipeline
        path_trace = g.createNode("path_tracing", name="PathTrace")

        # Create conversion nodes
        to_torch = g.createNode("nvrhi_to_torch", name="ToTorch")
        to_nvrhi = g.createNode("torch_to_nvrhi", name="ToNVRHI")

        # Connect: render -> to_torch
        g.addEdge(path_trace, "Output", to_torch, "Texture")

        # Later, after processing, connect back: to_nvrhi -> final output

        print("✓ Graph with PyTorch conversion nodes created")
        print("\nNote: Full execution requires proper scene setup")

    except Exception as e:
        print(f"✗ Failed: {e}")


def example_serialization():
    """Example of graph serialization."""
    print("\n" + "="*70)
    print("Example: Graph Serialization")
    print("="*70)

    g = RuzinoRenderGraph("SerializeExample")

    config_path = "path_tracing.json"
    if not os.path.exists(config_path):
        print(f"Config not found: {config_path}")
        return

    try:
        g.loadConfiguration(config_path)

        # Build a simple graph
        accum = g.createNode("accumulate", name="Accum")
        gamma = g.createNode("gamma_correction", name="Gamma")
        present = g.createNode("present_color", name="Present")

        g.addEdge(accum, "Accumulated", gamma, "Texture")
        g.addEdge(gamma, "Corrected", present, "Color")

        # Serialize to JSON
        json_str = g.serialize()

        print(f"✓ Serialized graph ({len(json_str)} chars)")

        # Save to file
        output_file = "example_render_graph.json"
        with open(output_file, 'w') as f:
            f.write(json_str)

        print(f"✓ Saved to: {output_file}")

        # Deserialize
        g2 = RuzinoRenderGraph("Deserialized")
        g2.loadConfiguration(config_path)
        g2.deserialize(json_str)

        print(f"✓ Deserialized: {len(g2.nodes)} nodes, {len(g2.links)} links")

    except Exception as e:
        print(f"✗ Failed: {e}")


def main():
    """Run all examples."""
    print("Ruzino Render Graph Python API Examples")
    print("="*70)

    # Uncomment the examples you want to run:

    # example_basic_render()
    example_custom_graph()
    # example_with_torch()
    example_serialization()

    print("\n" + "="*70)
    print("Examples completed!")
    print("="*70)
    print("\nNotes:")
    print("  - Adjust file paths in the script for your setup")
    print("  - Ensure render_nodes.json and USD files are accessible")
    print("  - Full rendering requires proper RHI initialization")


if __name__ == "__main__":
    main()
